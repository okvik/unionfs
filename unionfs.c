#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>

typedef struct Union Union;
typedef struct Fil Fil;
typedef struct Flist Flist;
typedef struct Fstate Fstate;
typedef struct Qidmap Qidmap;

struct Union {
	char *root;
	int create;
	Union *prev, *next;
};

enum {
	Nqidbits = 5,
	Nqidmap = 1 << Nqidbits,
};

struct Qidmap {
	Ref;
	ushort type;
	uint dev;
	uvlong path;
	uvlong qpath;
	Qidmap *next;
};

struct Fil {
	Ref;
	Dir;
	Qidmap *qmap;
	char *path;	/* real path */
	char *fspath;	/* internal path */
};

struct Flist {
	Fil *file;
	Flist *next;
};

struct Fstate {
	int fd;
	Fil *file;
	Flist *dir, *idx;
};

Union u0 = {.next = &u0, .prev = &u0};
Union *unionlist = &u0;
uvlong qidnext;
Qidmap *qidmap[Nqidmap];

void
LOG(char *fmt, ...)
{
	static int fd = -1;
	va_list args;

	if(fd < 0 && (fd = open("/mnt/log/log", OWRITE)) < 0){
		fprint(2, "failed to open log pipe\n");
		return;
	}
	va_start(args, fmt);
	vfprint(fd, fmt, args);
	va_end(args);
}

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
qidhash(uvlong path)
{
	int h, n;
	
	h = 0;
	for(n = 0; n < 64; n += Nqidbits){
		h ^= path;
		path >>= Nqidbits;
	}
	return h & (Nqidmap-1);
}

Qidmap*
qidlookup(Dir *d)
{
	int h;
	Qidmap *q;
	
	h = qidhash(d->qid.path);
	for(q = qidmap[h]; q != nil; q = q->next)
		if(q->type == d->type && q->dev == d->dev && q->path == d->qid.path)
			return q;
	return nil;
}

int
qidexists(uvlong path)
{
	int h;
	Qidmap *q;
	
	for(h = 0; h < Nqidmap; h++)
		for(q = qidmap[h]; q != nil; q = q->next)
			if(q->qpath == path)
				return 1;
	return 0;
}

Qidmap*
qidnew(Dir *d)
{
	int h;
	uvlong path;
	Qidmap *q;
	
	if(q = qidlookup(d))
		return (Qidmap*)copyref(q);
	path = d->qid.path;
	while(qidexists(path)){
		path &= (1LL<<48)-1;
		if(++qidnext >= 1<<16)
			qidnext = 1;
		path |= qidnext<<48;
	}
	q = emalloc(sizeof(*q));
	q->type = d->type;
	q->dev = d->dev;
	q->path = d->qid.path;
	q->qpath = path;
	h = qidhash(q->path);
	q->next = qidmap[h];
	qidmap[h] = q;
	return (Qidmap*)copyref(q);
}

void
qidfree(Qidmap *q)
{
	int h;
	Qidmap *l;
	
	if(decref(q))
		return;
	h = qidhash(q->path);
	if(qidmap[h] == q)
		qidmap[h] = q->next;
	else{
		for(l = qidmap[h]; l->next != q; l = l->next)
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

Fil*
filenew(Dir *d)
{
	Fil *f;
	
	f = emalloc(sizeof(*f));
	f->ref = 1;
	f->qmap = qidnew(d);
	f->Dir = *d;
	f->qid.path = f->qmap->qpath;
	f->name = estrdup(d->name);
	f->uid = estrdup(d->uid);
	f->gid = estrdup(d->gid);
	f->muid = estrdup(d->muid);
	return f;
}

void
filefree(Fil *f)
{
	if(decref(f))
		return;
//	qidfree(f->qmap);
	free(f->name);
	free(f->uid);
	free(f->gid);
	free(f->muid);
	free(f->path);
	free(f->fspath);
	free(f);
}

void
flistadd(Flist **list, Fil *f)
{
	Flist *p;
	
	p = emalloc(sizeof(*p));
	p->file = f;
	p->next = *list;
	*list = p;
}

void
flistfree(Flist *l)
{
	Flist *lp;
	
	for(lp = l; lp != nil; l = lp){
		lp = lp->next;
		filefree(l->file);
		free(l);
	}
}

int
flisthas(Flist *list, char *name)
{
	for(; list != nil; list = list->next)
		if(strcmp(list->file->name, name) == 0)
			return 1;
	return 0;
}

Fstate*
fstatenew(Fil *f)
{
	Fstate *st;
	
	st = emalloc(sizeof(*st));
	st->fd = -1;
	st->file = (Fil*)copyref(f);
	return st;
}

void
fstatefree(Fstate *st)
{
	if(st->file)
		filefree(st->file);
	if(st->dir)
		flistfree(st->dir);
	close(st->fd);
	free(st);
}

void
fsattach(Req *r)
{
	Dir d;
	Fil *root;
	Fstate *st;
	
	nulldir(&d);
	d.qid = (Qid){0, 0, QTDIR};
	d.name = ".";
	d.mode = 0777|DMDIR;
	d.uid = r->fid->uid;
	d.gid = r->fid->uid;
	d.muid = r->fid->uid;
	d.mtime = time(0);
	d.atime = time(0);
	d.length = 0;
	
	root = filenew(&d);
	root->fspath = estrdup(d.name);
	root->path = estrdup(d.name);
	
	st = fstatenew(root);
	decref(root);

	r->fid->aux = st;
	r->fid->qid = root->qid;
	r->ofcall.qid = root->qid;
	respond(r, nil);
}

Fil*
filewalk(Fil *p, char *name)
{
	char *path;
	char *s;
	Dir *d;
	Fil *f;
	Union *u;
	
	if(strcmp(name, "..") == 0){
		if((s = strrchr(p->fspath, '/')) == nil)
			return p;
		*s = 0;
		name = "";
	}
	for(u = unionlist->next; u != unionlist; u = u->next){
		path = mkpath(u->root, p->fspath, name, nil);
		if(d = dirstat(path)){
			f = filenew(d);
			free(d);
			f->fspath = mkpath(p->fspath, name, nil);
			f->path = path;
			return f;
		}
		free(path);
	}
	return nil;
}

char*
walk1(Fid *fid, char *name, void *)
{
	Fil *p, *f;
	Fstate *st;

	/* not sure if needed */
	if(!(fid->qid.type&QTDIR))
		return "walk in non-directory";
	
	st = fid->aux;
	p = st->file;
	if((f = filewalk(p, name)) == nil)
		return "no file";
	st->file = f;
	filefree(p);

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

Flist*
filereaddir(Fil *p)
{
	int fd;
	long i, n;
	Dir *dir, *d;
	char *path;
	Union *u;
	Fil *f;
	Flist *list;

	list = nil;
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
		n = dirreadall(fd, &dir);
		close(fd);
		if(n < 0)
			continue;
		for(i = 0; i < n; i++){
			if(flisthas(list, dir[i].name))
				continue;
			f = filenew(&dir[i]);
			flistadd(&list, f);
		}
		free(dir);
	}
	return list;
}

void
fsopen(Req *r)
{
	Fcall *i, *o;
	Fstate *st;
	Fil *f;
	
	i = &r->ifcall;
	o = &r->ofcall;
	st = r->fid->aux;
	f = st->file;

	if(f->mode&DMDIR){
		st->dir = filereaddir(f);
		st->idx = st->dir;
	}else{
		st->fd = open(f->path, i->mode);
		if(st->fd < 0){
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
	path = p = strdup(path);
	for(; p != nil ;){
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
	Fil *f, *nf;
	
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
	st->fd = create(npath, i->mode, i->perm);
	if(st->fd < 0){
		responderror(r);
		return;
	}
	d = dirfstat(st->fd);
	if(d == nil){
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
	r->fid->qid = f->qid;
	o->qid = f->qid;
	respond(r, nil);
}

void
fsremove(Req *r)
{
	Fstate *st;
	Fil *f;
	
	st = r->fid->aux;
	f = st->file;
	if(remove(f->path) < 0){
		responderror(r);
		return;
	}
	respond(r, nil);
}

void
dirfill(Dir *dir, Fil *f)
{
	*dir = f->Dir;
	dir->qid = f->qid;
	dir->name = estrdup(f->name);
	dir->uid = estrdup(f->uid);
	dir->gid = estrdup(f->gid);
	dir->muid = estrdup(f->muid);
}

int
dirgen(int, Dir *dir, void *aux)
{
	Fstate *fs;
	Flist *l;
	
	fs = aux;
	l = fs->idx;
	if(l == nil)
		return -1;
	dirfill(dir, l->file);
	fs->idx = l->next;
	return 0;
}

void
fsread(Req *r)
{
	long n;
	Fcall *i, *o;
	Fil *f;
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
	Fil *f;
	
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
	fprint(2, "%s [-D] [-abC] [-M | -m mtpt] [-s srv] [[-c] path ...]\n", argv0);
	exits("usage");
}

void
main(int argc, char *argv[])
{
	int c, i;
	int mflag;
	char *mtpt, *srv;
	Union *u;

	c = 0;
	mflag = MREPL|MCREATE;
	mtpt = "/mnt/union";
	srv = nil;
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
		srv = EARGF(usage());
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
		u = emalloc(sizeof(*u));
		u->create = c == 1 ? c : 0;
		u->root = mkpath(argv[i], nil);
		unionlink(unionlist, u);
	}
	if(c == 0)
		unionlist->next->create = 1;
	
	postmountsrv(&fs, srv, mtpt, mflag);
	
	exits(nil);
}
