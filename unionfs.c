#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>

typedef struct Union Union;
typedef struct F F;
typedef struct Ftab Ftab;
typedef struct Fstate Fstate;
typedef struct Qtab Qtab;

struct Union {
	char *root;
	int create;
	Union *prev, *next;
};

enum {
	Nqbit = 5,
	Nqtab = 1<<Nqbit,
	Nftab = 32,
	Nftlist = 32,
};

struct Qtab {
	Ref;
	ushort type;
	uint dev;
	uvlong path, uniqpath;
	Qtab *next;
};

struct F {
	Ref;
	Dir;
	Qtab *qtab;
	char *path;	/* real path */
	char *fspath;	/* internal path */
};

struct Ftab {
	long n, sz;
	F **l;
};

struct Fstate {
	int fd;
	F *file;
	Ftab *ftab;
};

Union u0 = {.next = &u0, .prev = &u0};
Union *unionlist = &u0;
Qtab *qidtab[Nqtab];
F *root;

void*
emalloc(ulong sz)
{
	void *v;

	if((v = malloc(sz)) == nil)
		sysfatal("emalloc: %r");
	memset(v, 0, sz);
	setmalloctag(v, getcallerpc(&sz));
	
	return v;
}

void*
erealloc(void *v, ulong sz)
{
	if((v = realloc(v, sz)) == nil && sz != 0)
		sysfatal("realloc: %r");
	setrealloctag(v, getcallerpc(&v));
	return v;
}

char*
estrdup(char *s)
{
	char *p;

	if((p = strdup(s)) == nil)
		sysfatal("estrdup: %r");
	setmalloctag(p, getcallerpc(&s));
	return p;
}

char*
mkpath(char *a0, ...)
{
	va_list args;
	int i;
	char *a;
	char *ap[] = {a0, "", ""};

	va_start(args, a0);
	for(i = 1; (a = va_arg(args, char*)) != nil && i < 3; i++)
		ap[i] = a;
	va_end(args);
	if((a = smprint("%s/%s/%s", ap[0], ap[1], ap[2])) == nil)
		sysfatal("smprint: %r");

	return cleanname(a);
}

Ref*
copyref(Ref *r)
{
	incref(r);
	return r;
}

int
qthash(uvlong path)
{
	int h, n;
	
	h = 0;
	for(n = 0; n < 64; n += Nqbit){
		h ^= path;
		path >>= Nqbit;
	}
	return h & (Nqtab-1);
}

uvlong
uniqpath(uvlong path)
{
	static u16int salt;
	int h, have;
	Qtab *q;

	for(;;){
		have = 0;
		h = qthash(path);
		for(q = qidtab[h]; q != nil; q = q->next)
			if(q->uniqpath == path){
				have = 1;
				break;
			}
		if(have == 0)
			return path;
		path = ((uvlong)salt<<48) | (path&((uvlong)1<<48)-1);
		++salt;
	}
}

Qtab*
qtadd(Dir *d)
{
	int h;
	Qtab *q;

	h = qthash(d->qid.path);
	for(q = qidtab[h]; q != nil; q = q->next)
		if(q->type == d->type
		&& q->dev == d->dev
		&& q->path == d->qid.path)
			return q;

	q = emalloc(sizeof(*q));
	q->type = d->type;
	q->dev = d->dev;
	q->path = d->qid.path;
	q->uniqpath = uniqpath(q->path);

	h = qthash(q->path);
	q->next = qidtab[h];
	qidtab[h] = q;
	return (Qtab*)copyref(q);
}

void
qtfree(Qtab *q)
{
	int h;
	Qtab *l;
	
	if(decref(q))
		return;
	h = qthash(q->path);
	if(qidtab[h] == q)
		qidtab[h] = q->next;
	else{
		for(l = qidtab[h]; l->next != q; l = l->next)
			;
		l->next = q->next;
	}
	free(q);
}

void
unionlink(Union *p, Union *n)
{
	p = p->prev;
	n->next = p->next;
	n->prev = p;
	p->next->prev = n;
	p->next = n;
}

F*
filenew(Dir *d)
{
	F *f;
	
	f = emalloc(sizeof(*f));
	f->ref = 1;
	f->qtab = qtadd(d);
	f->Dir = *d;
	f->qid.path = f->qtab->uniqpath;
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
//	qtfree(f->qtab);
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
	Union *u;
	
	np = mkpath(p->fspath, name, nil);
	if(strcmp(np, ".") == 0){
		free(np);
		filefree(p);
		return root;
	}
	for(u = unionlist->next; u != unionlist; u = u->next){
		path = mkpath(u->root, np, nil);
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

	/* not sure if needed */
	if(!(fid->qid.type&QTDIR))
		return "walk in non-directory";
	
	st = fid->aux;
	p = st->file;
	if((f = filewalk(p, name)) == nil)
		return "no file";
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
	walkandclone(r, walk1, clone, nil);
}

Ftab*
filereaddir(F *p)
{
	int fd;
	long i, n;
	Dir *dir, *d;
	char *path;
	Union *u;
	F *f;
	Ftab *ft;

	ft = ftnew();
	for(u = unionlist->next; u != unionlist; u = u->next){
		path = mkpath(u->root, p->fspath, nil);
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
			for(i = 0; i < n; i++){
				if(u->prev != unionlist && fthas(ft, dir[i].name))
					continue;
				f = filenew(&dir[i]);
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
	Fcall *i, *o;
	Fstate *st;
	F *f;
	
	i = &r->ifcall;
	o = &r->ofcall;
	st = r->fid->aux;
	f = st->file;

	if(f->mode&DMDIR)
		st->ftab = filereaddir(f);
	else{
		if((st->fd = open(f->path, i->mode)) < 0){
			responderror(r);
			return;
		}
		o->iounit = iounit(st->fd);
	}
	respond(r, nil);
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
	Dir *d;
	Fcall *i, *o;
	Union *u;
	Fstate *st;
	F *f, *nf;
	
	i = &r->ifcall;
	o = &r->ofcall;
	st = r->fid->aux;
	f = st->file;
	
	for(u = unionlist->next; u != unionlist; u = u->next)
		if(u->create == 1)
			break;
	path = mkpath(u->root, f->fspath, nil);
	if(mkdirp(path) < 0){
		responderror(r);
		return;
	}
	npath = mkpath(path, i->name, nil);
	free(path);
	st = emalloc(sizeof(*st));
	if((st->fd = create(npath, i->mode, i->perm)) < 0){
		responderror(r);
		return;
	}
	if((d = dirfstat(st->fd)) == nil){
		fstatefree(st);
		responderror(r);
		return;
	}
	nf = filenew(d);
	free(d);
	nf->path = npath;
	nf->fspath = estrdup(f->fspath);
	st->file = nf;
	
	r->fid->aux = st;
	r->fid->qid = nf->qid;
	o->qid = nf->qid;
	respond(r, nil);
}

void
fsremove(Req *r)
{
	Fstate *st;
	F *f;
	
	st = r->fid->aux;
	f = st->file;
	if(remove(f->path) < 0){
		responderror(r);
		return;
	}
	respond(r, nil);
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
	Fcall *i, *o;
	F *f;
	Fstate *st;
	
	i = &r->ifcall;
	o = &r->ofcall;
	st = r->fid->aux;
	f = st->file;

	if(f->mode&DMDIR){
		dirread9p(r, dirgen, st);
		respond(r, nil);
		return;
	}
	if((n = pread(st->fd, o->data, i->count, i->offset)) < 0){
		responderror(r);
		return;
	}
	r->ofcall.count = n;
	respond(r, nil);
}

void
fswrite(Req *r)
{
	Fcall *i, *o;
	Fstate *fs;
	
	i = &r->ifcall;
	o = &r->ofcall;
	fs = r->fid->aux;

	if((o->count = pwrite(fs->fd, i->data, i->count, i->offset)) != i->count){
		responderror(r);
		return;
	}
	respond(r, nil);
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
	if(dirwstat(f->path, &r->d) < 0){
		responderror(r);
		return;
	}
	respond(r, nil);
}

Srv fs = {
	.attach = fsattach,
	.walk = fswalk,
	.open = fsopen,
	.create = fscreate,
	.remove = fsremove,
	.read = fsread,
	.write = fswrite,
	.stat = fsstat,
	.wstat = fswstat,
	.destroyfid = destroyfid,
};

void
usage(void)
{
	fprint(2, "%s [-abiC] [-M | -m mtpt] [-s srv] [-c] path ...\n", argv0);
	exits("usage");
}

void
main(int argc, char *argv[])
{
	int c, i, mflag, stdio;
	char *mtpt, *srvname;
	Dir *d;
	Union *u;

	c = 0;
	mflag = MREPL|MCREATE;
	mtpt = "/mnt/union";
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
	case 'M':
		mtpt = nil;
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
	for(i = 0; i < argc; i++){
		if(strncmp(argv[i], "-c", 2) == 0){
			c++;
			continue;
		}
		if(strcmp(argv[i], mtpt) == 0){
			fprint(2, "%s: mountpoint cycle, skipping branch %s\n", argv0, argv[i]);
			continue;
		}
		if((d = dirstat(argv[i])) == nil){
			fprint(2, "%s: %s does not exist, skipping\n", argv0, argv[i]);
			continue;
		}
		free(d);
		u = emalloc(sizeof(*u));
		u->create = c == 1 ? c : 0;
		u->root = mkpath(argv[i], nil);
		unionlink(unionlist, u);
	}
	if(unionlist->next == &u0)
		sysfatal("empty branch list");
	if(c == 0)
		unionlist->next->create = 1;
	
	initroot();

	if(stdio == 0){
		postmountsrv(&fs, srvname, mtpt, mflag);
		exits(nil);
	}
	fs.nopipe = 1;
	fs.infd = 0;
	fs.outfd = 1;
	srv(&fs);
	exits(nil);
}
