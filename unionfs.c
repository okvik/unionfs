#include "unionfs.h"

Srv thefs;
Branch *branch;
usize nbranch;
QLock mtptlock;
Mtpt *mtpt;

Mtpt*
mtptgrab(void)
{
	static int mtptnext = 0;
	Mtpt *m;

	qlock(&mtptlock);
	if(mtpt == nil){
		mtpt = emalloc(sizeof(Mtpt));
		mtpt->path = smprint("/mnt/mtpt%d", mtptnext++);
	}
	m = mtpt;
	mtpt = m->next;
	qunlock(&mtptlock);
	unmount(nil, m->path);
	return m;
}

void
mtptfree(Mtpt *m)
{
	qlock(&mtptlock);
	m->next = mtpt;
	mtpt = m;
	qunlock(&mtptlock);
}

FILE*
filenew(void)
{
	FILE *f;

	f = emalloc(sizeof(FILE));
	f->fd = -1;
	return f;
}

void
filefree(FILE *f)
{
	if(f == nil)
		return;
	if(f->name) free(f->name);
	if(f->uid) free(f->uid);
	if(f->gid) free(f->gid);
	if(f->muid) free(f->muid);
	if(f->path) s_free(f->path);
	if(f->realpath) s_free(f->realpath);
	if(f->fd != -1) close(f->fd);
	if(f->dl) dirlistfree(f->dl);
	if(f->mtpt) mtptfree(f->mtpt);
	free(f);
}

void
dircopy(Dir *a, Dir *b)
{
	a->type = b->type;
	a->dev = b->dev;
	a->qid = b->qid;
	a->mode = b->mode;
	a->mtime = b->mtime;
	a->atime = b->atime;
	if(a->name)
		free(a->name);
	a->name = estrdup(b->name);
	if(a->uid)
		free(a->uid);
	a->uid = estrdup(b->uid);
	if(a->gid)
		free(a->gid);
	a->gid = estrdup(b->gid);
	if(a->muid)
		free(a->muid);
	a->muid = estrdup(b->muid);
}

void
fsattach(Req *r)
{
	FILE *f;
	char *user;
	
	f = filenew();
	f->name = estrdup("/");
	f->mode = 0777|DMDIR;
	user = getuser();
	f->uid = estrdup(user);
	f->gid = estrdup(user);
	f->muid = estrdup(user);
	f->mtime = f->atime = time(0);
	f->type = 0xFFFFFFFF;
	f->dev = 0xFFFFFFFFFFFFFFFF;
	f->qid = (Qid){0, 0, QTDIR};
	f->qid = qencode(f);
	f->path = s_copy(f->name);
	f->realpath = s_new();
	
	r->fid->aux = f;
	r->fid->qid = f->qid;
	r->ofcall.qid = f->qid;
	respond(r, nil);
}

String*
walk(String *s, char *n0, char *n1)
{
	s_append(s, "/");
	s_append(s, n0);
	s_append(s, "/");
	s_append(s, n1);
	cleanname(s->base);
	s->ptr = s->base + strlen(s->base);
	return s;
}

char*
clone(Fid *fid, Fid *newfid, void*)
{
	FILE *f;
	FILE *parent = fid->aux;
	
	f = filenew();
	dircopy(f, parent);
	f->path = s_clone(parent->path);
	f->realpath = s_clone(parent->realpath);
	newfid->aux = f;
	return nil;
}

char*
walkto(Fid *fid, char *name, void *)
{
	Dir *d;
	FILE *f;
	int i;
	
	f = fid->aux;
	walk(f->path, name, nil);
	for(i = 0; i < nbranch; i++){
		s_reset(f->realpath);
		walk(f->realpath, branch[i].root, s_to_c(f->path));
		if((d = dirstat(s_to_c(f->realpath))) == nil)
			continue;
		dircopy(f, d);
		f->qid = qencode(d);
		free(d);
		fid->qid = f->qid;
		return nil;
	}
	return "not found";
}

void
fswalk(Req *r)
{
	walkandclone(r, walkto, clone, r);
}

void
destroyfid(Fid *fid)
{
	if(fid->aux)
		filefree(fid->aux);
	fid->aux = nil;
}

void
fsopen(Req *r)
{
	Fcall *T, *R;
	FILE *f;
	usize i;
	String *path;
	Dir *d;
	
	T = &r->ifcall;
	R = &r->ofcall;
	f = r->fid->aux;

	srvrelease(&thefs);
	if(f->mode & DMDIR){
		f->mtpt = mtptgrab();
		path = s_new();
		for(i = 0; i < nbranch; i++){
			s_reset(path);
			walk(path, branch[i].root, s_to_c(f->path));
			if((d = dirstat(s_to_c(path))) != nil){
				if(d->mode & DMDIR)
				if(bind(s_to_c(path), f->mtpt->path, MAFTER) == -1)
					sysfatal("bind: %r");
				free(d);
			}
		}
		s_free(path);
		if((f->fd = open(f->mtpt->path, T->mode)) < 0){
			responderror(r);
			goto done;
		}
	}else{
		if((f->fd = open(s_to_c(f->realpath), T->mode)) < 0){
			responderror(r);
			goto done;
		}
	}
	R->iounit = iounit(f->fd);
	respond(r, nil);
done:
	srvacquire(&thefs);
}

void
fsremove(Req *r)
{
	FILE *f;
	
	f = r->fid->aux;
	srvrelease(&thefs);
	if(remove(s_to_c(f->realpath)) < 0){
		responderror(r);
		goto done;
	}
	respond(r, nil);
done:
	srvacquire(&thefs);
}

int
dirgen(int i, Dir *d, void *aux)
{
	Dirlist *dl = aux;
	Dir *dd;
	
	if(dl->ndirs == i)
		return -1;
	dd = dl->dirs[i];
	dircopy(d, dd);
	d->qid = qencode(dd);
	return 0;
}

void
fsread(Req *r)
{
	long n;
	Fcall *T, *R;
	FILE *f;
	
	T = &r->ifcall;
	R = &r->ofcall;
	f = r->fid->aux;

	srvrelease(&thefs);
	if(f->mode&DMDIR){
		if(T->offset == 0){
			if(seek(f->fd, 0, 0) == -1)
				goto error;
			if(f->dl != nil)
				dirlistfree(f->dl);
			if((f->dl = dirlist(f->fd)) == nil)
				goto error;
		}
		dirread9p(r, dirgen, f->dl);
	}else{
		if((n = pread(f->fd, R->data, T->count, T->offset)) < 0)
			goto error;
		r->ofcall.count = n;
	}
	respond(r, nil);
	srvacquire(&thefs);
	return;
error:
	responderror(r);
	srvacquire(&thefs);
}

void
fswrite(Req *r)
{
	Fcall *T, *R;
	FILE *f;
	
	T = &r->ifcall;
	R = &r->ofcall;
	f = r->fid->aux;
	
	srvrelease(&thefs);
	if((R->count = pwrite(f->fd, T->data, T->count, T->offset)) != T->count){
		responderror(r);
		goto done;
	}
	respond(r, nil);
done:
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
	String *realpath;
	usize i;
	Dir *d;
	Fcall *T, *R;
	FILE *parent, *f;
	int fd;
	
	T = &r->ifcall;
	R = &r->ofcall;
	parent = r->fid->aux;
	
	srvrelease(&thefs);
	for(i = 0; i < nbranch; i++)
		if(branch[i].create == 1)
			break;
	realpath = s_new();
	walk(realpath, branch[i].root, s_to_c(parent->path));
	if(mkdirp(s_to_c(realpath)) < 0){
error:
		s_free(realpath);
		responderror(r);
		srvacquire(&thefs);
		return;
	}
	walk(realpath, T->name, nil);
	if((fd = create(s_to_c(realpath), T->mode, T->perm)) < 0)
		goto error;
	if((d = dirfstat(fd)) == nil)
		goto error;
	f = filenew();
	dircopy(f, d);
	f->fd = fd;
	f->qid = qencode(d);
	free(d);
	f->path = walk(s_clone(parent->path), T->name, nil);
	f->realpath = realpath;
	filefree(parent);
	
	r->fid->aux = f;
	R->qid = f->qid;
	respond(r, nil);
	srvacquire(&thefs);
}

void
fsstat(Req *r)
{
	FILE *f = r->fid->aux;
	
	dircopy(&r->d, f);
	respond(r, nil);
}

void
fswstat(Req *r)
{
	FILE *f = r->fid->aux;
	
	srvrelease(&thefs);
	if(dirwstat(s_to_c(f->realpath), &r->d) < 0){
		responderror(r);
		goto done;
	}
	respond(r, nil);
done:
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
	char *mountat, *srvname, *path;
	Dir *d;
	Branch *b;

	c = 0;
	mflag = MREPL|MCREATE;
	mountat = nil;
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
		mountat = EARGF(usage());
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
	if((mountat || srvname) == 0)
		mountat = "/mnt/union";
	nbranch = argc;
	branch = b = emalloc(nbranch * sizeof(Branch));
	for(i = 0; i < argc; i++){
		if(strcmp(argv[i], "-c") == 0){
			nbranch--;
			c++;
			continue;
		}

		path = cleanname(argv[i]);
		if((d = dirstat(path)) == nil){
			fprint(2, "%s: %s does not exist, skipping\n", argv0, path);
			continue;
		}
		free(d);
		if(mountat && strcmp(path, mountat) == 0)
			path = pivot(path);
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
	if(stdio == 0){
		postmountsrv(&thefs, srvname, mountat, mflag);
		exits(nil);
	}
	thefs.infd = 0;
	thefs.outfd = 1;
	srv(&thefs);
	exits(nil);
}
