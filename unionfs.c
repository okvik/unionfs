#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>
#include "unionfs.h"

Branch *branch;
usize nbranch;
F *root;
Srv thefs;

F*
filenew(Dir *d)
{
	F *f;

	f = emalloc(sizeof(*f));
	f->ref = 1;
	f->Dir = *d;
	f->qid = qencode(d);
	f->name = estrdup(d->name);
	f->uid = estrdup(d->uid);
	f->gid = estrdup(d->gid);
	f->muid = estrdup(d->muid);
	return f;
}

void
filefree(F *f)
{
	if(f == root)
		return;
	if(decref(f))
		return;
	free(f->name);
	free(f->uid);
	free(f->gid);
	free(f->muid);
	free(f->path);
	free(f->fspath);
	free(f);
}

uint
fthash(char *s)
{
	uint h;
	for(h = 0; *s; s++)
		h = *s + 31*h;
	return h % Nftab;
}

Ftab*
ftnew(void)
{
	int i;
	Ftab *ft, *p;
	
	ft = emalloc(Nftab*sizeof(Ftab));
	for(i = 0; i < Nftab; i++){
		p = &ft[i];
		p->sz = Nftlist;
		p->l = emalloc(p->sz*sizeof(*p->l));
	}
	return ft;
}

void
ftfree(Ftab *ft)
{
	int i, j;
	Ftab *p;
	
	for(i = 0; i < Nftab; i++){
		p = &ft[i];
		for(j = 0; j < p->n; j++)
			filefree(p->l[j]);
		free(p->l);
	}
	free(ft);
}

void
ftadd(Ftab *ft, F *f)
{
	Ftab *p;
	
	p = &ft[fthash(f->name)];
	if(p->n == p->sz){
		p->sz *= 2;
		p->l = erealloc(p->l, p->sz*sizeof(*p->l));
	}
	p->l[p->n++] = f;
}

int
fthas(Ftab *ft, char *name)
{
	int i;
	Ftab *p;
	
	p = &ft[fthash(name)];
	for(i = 0; i < p->n; i++)
		if(strcmp(p->l[i]->name, name) == 0)
			return 1;
	return 0;
}

F*
ftidx(Ftab *ft, long i)
{
	long y;
	Ftab *p;
	
	for(y = 0; y < Nftab; y++){
		p = &ft[y];
		if(p->n == 0)
			continue;
		if(i >= p->n){
			i -= p->n;
			continue;
		}
		return p->l[i];
	}
	return nil;
}

Fstate*
fstatenew(F *f)
{
	Fstate *st;
	
	st = emalloc(sizeof(*st));
	st->fd = -1;
	st->file = (F*)copyref(f);
	return st;
}

void
fstatefree(Fstate *st)
{
	if(st->file)
		filefree(st->file);
	if(st->ftab)
		ftfree(st->ftab);
	close(st->fd);
	free(st);
}

void
initroot(void)
{
	char *user;
	Dir d;
	
	nulldir(&d);
	d.qid = (Qid){0, 0, QTDIR};
	d.name = ".";
	d.mode = 0777|DMDIR;
	user = getuser();
	d.uid = user;
	d.gid = user;
	d.muid = user;
	d.mtime = time(0);
	d.atime = time(0);
	d.length = 0;
	
	root = filenew(&d);
	root->fspath = estrdup(d.name);
	root->path = estrdup(d.name);
}

void
fsattach(Req *r)
{
	Fstate *st;
	
	st = fstatenew(root);
	r->fid->aux = st;
	r->fid->qid = root->qid;
	r->ofcall.qid = root->qid;
	respond(r, nil);
}

F*
filewalk(F *p, char *name)
{
	char *path, *np;
	Dir *d;
	F *f;
	
	np = mkpath(p->fspath, name, nil);
	for(int i = 0; i < nbranch; i++){
		path = mkpath(branch[i].root, np, nil);
		if((d = dirstat(path)) == nil){
			free(path);
			continue;
		}
		f = filenew(d);
		free(d);
		f->fspath = np;
		f->path = path;
		filefree(p);
		return f;
	}
	free(np);
	return nil;
}

char*
walk1(Fid *fid, char *name, void *)
{
	F *p, *f;
	Fstate *st;

	st = fid->aux;
	p = st->file;
	if((f = filewalk(p, name)) == nil)
		return "not found";
	st->file = f;

	fid->qid = f->qid;
	return nil;
}

char*
clone(Fid *old, Fid *new, void*)
{
	Fstate *fs;

	fs = old->aux;
	new->aux = fstatenew(fs->file);
	return nil;
}

void
destroyfid(Fid *fid)
{
	if(fid->aux)
		fstatefree(fid->aux);
	fid->aux = nil;
}

void
fswalk(Req *r)
{
	srvrelease(&thefs);
	walkandclone(r, walk1, clone, nil);
	srvacquire(&thefs);
}

Ftab*
filereaddir(F *p)
{
	int fd;
	long n;
	Dir *dir, *d;
	char *path;
	F *f;
	Ftab *ft;

	ft = ftnew();
	for(usize i = 0; i < nbranch; i++){
		path = mkpath(branch[i].root, p->fspath, nil);
		if((d = dirstat(path)) == nil){
		err:
			free(path);
			continue;
		}
		free(d);
		if((fd = open(path, OREAD)) < 0)
			goto err;
		free(path);
		while((n = dirread(fd, &dir)) > 0){
			for(usize j = 0; j < n; j++){
				if(i > 0 && fthas(ft, dir[j].name))
					continue;
				f = filenew(&dir[j]);
				ftadd(ft, f);
			}
			free(dir);
		}
		if(n < 0)
			fprint(2, "dirread: %r\n");
		close(fd);
	}
	return ft;
}

void
fsopen(Req *r)
{
	Fcall *T, *R;
	Fstate *st;
	F *f;
	
	T = &r->ifcall;
	R = &r->ofcall;
	st = r->fid->aux;
	f = st->file;

	srvrelease(&thefs);
	if(f->mode&DMDIR)
		st->ftab = filereaddir(f);
	else{
		if((st->fd = open(f->path, T->mode)) < 0){
			responderror(r);
			srvacquire(&thefs);
			return;
		}
		R->iounit = iounit(st->fd);
	}
	respond(r, nil);
	srvacquire(&thefs);
}

int
mkdirp(char *path)
{
	int fd;
	char *p;
	Dir *d;
	
	assert(path != nil);
	if((d = dirstat(path)) != nil){
		free(d);
		return 1;
	}
	path = p = estrdup(path);
	for(; p != nil;){
		if(p[0] == '/')
			p++;
		if(p = strchr(p, '/'))
			*p = 0;
		if((d = dirstat(path)) == nil){
			if((fd = create(path, 0, 0777|DMDIR)) < 0){
				free(path);
				return -1;
			}
			close(fd);
		}
		free(d);
		if(p != nil)
			*p++ = '/';
	}
	free(path);
	return 1;
}

void
fscreate(Req *r)
{
	char *path, *npath;
	usize i;
	Dir *d;
	Fcall *T, *R;
	Fstate *st;
	F *f, *nf;
	
	T = &r->ifcall;
	R = &r->ofcall;
	st = r->fid->aux;
	f = st->file;
	
	srvrelease(&thefs);
	for(i = 0; i < nbranch; i++)
		if(branch[i].create == 1)
			break;
	path = mkpath(branch[i].root, f->fspath, nil);
	if(mkdirp(path) < 0){
		responderror(r);
		srvacquire(&thefs);
		return;
	}
	npath = mkpath(path, T->name, nil);
	free(path);
	st = emalloc(sizeof(*st));
	if((st->fd = create(npath, T->mode, T->perm)) < 0){
		responderror(r);
		srvacquire(&thefs);
		return;
	}
	if((d = dirfstat(st->fd)) == nil){
		fstatefree(st);
		responderror(r);
		srvacquire(&thefs);
		return;
	}
	nf = filenew(d);
	free(d);
	nf->path = npath;
	nf->fspath = estrdup(f->fspath);
	st->file = nf;
	
	r->fid->aux = st;
	r->fid->qid = nf->qid;
	R->qid = nf->qid;
	respond(r, nil);
	srvacquire(&thefs);
}

void
fsremove(Req *r)
{
	Fstate *st;
	F *f;
	
	st = r->fid->aux;
	f = st->file;
	srvrelease(&thefs);
	if(remove(f->path) < 0){
		responderror(r);
		srvacquire(&thefs);
		return;
	}
	respond(r, nil);
	srvacquire(&thefs);
}

void
dirfill(Dir *dir, F *f)
{
	*dir = f->Dir;
	dir->qid = f->qid;
	dir->name = estrdup(f->name);
	dir->uid = estrdup(f->uid);
	dir->gid = estrdup(f->gid);
	dir->muid = estrdup(f->muid);
}

int
dirgen(int i, Dir *dir, void *aux)
{
	Fstate *fs;
	F *f;
	
	fs = aux;
	f = ftidx(fs->ftab, i);
	if(f == nil)
		return -1;
	dirfill(dir, f);
	return 0;
}

void
fsread(Req *r)
{
	long n;
	Fcall *T, *R;
	F *f;
	Fstate *st;
	
	T = &r->ifcall;
	R = &r->ofcall;
	st = r->fid->aux;
	f = st->file;

	srvrelease(&thefs);
	if(f->mode&DMDIR){
		dirread9p(r, dirgen, st);
		respond(r, nil);
		srvacquire(&thefs);
		return;
	}
	if((n = pread(st->fd, R->data, T->count, T->offset)) < 0){
		responderror(r);
		srvacquire(&thefs);
		return;
	}
	r->ofcall.count = n;
	respond(r, nil);
	srvacquire(&thefs);
}

void
fswrite(Req *r)
{
	Fcall *T, *R;
	Fstate *fs;
	
	T = &r->ifcall;
	R = &r->ofcall;
	fs = r->fid->aux;
	
	srvrelease(&thefs);
	if((R->count = pwrite(fs->fd, T->data, T->count, T->offset)) != T->count){
		responderror(r);
		srvacquire(&thefs);
		return;
	}
	respond(r, nil);
	srvacquire(&thefs);
}

void
fsstat(Req *r)
{
	Fstate *st;
	
	st = r->fid->aux;
	dirfill(&r->d, st->file);
	respond(r, nil);
}

void
fswstat(Req *r)
{
	Fstate *st;
	F *f;
	
	st = r->fid->aux;
	f = st->file;
	
	srvrelease(&thefs);
	if(dirwstat(f->path, &r->d) < 0){
		responderror(r);
		srvacquire(&thefs);
		return;
	}
	respond(r, nil);
	srvacquire(&thefs);
}

char*
pivot(char *p)
{
	static n = 0;
	char *q;

	if((q = smprint("/mnt/union.%d.%d", getpid(), n++)) == nil)
		sysfatal("smprint: %r");
	if(bind(p, q, MREPL) < 0)
		sysfatal("bind: %r");
	return q;
}

void
main(int argc, char *argv[])
{
	int c, i, mflag, stdio;
	char *mtpt, *srvname, *path, *p;
	Dir *d;
	Branch *b;

	c = 0;
	mflag = MREPL|MCREATE;
	mtpt = nil;
	srvname = nil;
	stdio = 0;
	ARGBEGIN{
	case 'a':
		mflag |= MAFTER;
		break;
	case 'b':
		mflag |= MBEFORE;
		break;
	case 'c':
		c = 1;
		break;
	case 'C':
		mflag |= MCACHE;
		break;
	case 'D':
		chatty9p++;
		break;
	case 'm':
		mtpt = EARGF(usage());
		break;
	case 's':
		srvname = EARGF(usage());
		break;
	case 'i':
		stdio = 1;
		break;
	default:
		usage();
	}ARGEND;
	if(argc < 1)
		usage();
	if((mtpt || srvname) == 0)
		mtpt = "/mnt/union";
	nbranch = argc;
	branch = b = emalloc(nbranch * sizeof(Branch));
	for(i = 0; i < argc; i++){
		if(strcmp(argv[i], "-c") == 0){
			nbranch--;
			c++;
			continue;
		}

		path = mkpath(argv[i], nil);
		if((d = dirstat(path)) == nil){
			fprint(2, "%s: %s does not exist, skipping\n", argv0, path);
			free(path);
			continue;
		}
		free(d);
		if(mtpt && strcmp(path, mtpt) == 0){
			p = pivot(path);
			free(path);
			path = p;
		}
		b->root = path;
		b->create = c == 1 ? c : 0;
		b++;
	}
	if(branch[0].root == nil)
		sysfatal("empty branch list");
	if(c == 0)
		branch[0].create = 1;
	
	thefs.attach = fsattach;
	thefs.walk = fswalk;
	thefs.open = fsopen;
	thefs.create = fscreate;
	thefs.remove = fsremove;
	thefs.read = fsread;
	thefs.write = fswrite;
	thefs.stat = fsstat;
	thefs.wstat = fswstat;
	thefs.destroyfid = destroyfid;
	initroot();
	if(stdio == 0){
		postmountsrv(&thefs, srvname, mtpt, mflag);
		exits(nil);
	}
	thefs.infd = 0;
	thefs.outfd = 1;
	srv(&thefs);
	exits(nil);
}
