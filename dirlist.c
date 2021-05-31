#include "unionfs.h"

static uvlong
hash(char *s)
{
	uvlong h;
	
	h = 14695981039346656037ULL;
	while(*s++){
		h ^= *s;
		h *= 1099511628211ULL;
	}
	return h;
}

static int
seen(Dirlist *dl, char *name)
{
	usize probe, omseen, i;
	char *n, **oseen;

	probe = hash(name) % dl->mseen;
	while((n = dl->seen[probe]) != nil){
		if(strcmp(n, name) == 0)
			return 1;
		probe = (probe + 1) % dl->mseen;
	}
	dl->seen[probe] = name;
	dl->nseen++;
	if(dl->nseen > dl->mseen / 2){
		oseen = dl->seen;
		omseen = dl->mseen;
		dl->mseen *= 2;
		dl->nseen = 0;
		dl->seen = emalloc(dl->mseen * sizeof(char*));
		for(i = 0; i < omseen; i++)
			if(oseen[i] != nil)
				seen(dl, oseen[i]);
		free(oseen);
	}
	return 0;
}

void
dirlistfree(Dirlist *dl)
{
	if(dl == nil)
		return;
	free(dl->seen);
	free(dl->dirs);
	free(dl->all);
	free(dl);
}

Dirlist*
dirlist(int fd)
{
	long i, j;
	Dir *d;
	Dirlist *dl;
	
	dl = emalloc(sizeof(Dirlist));
	if((dl->nall = dirreadall(fd, &dl->all)) == -1){
		free(dl);
		return nil;
	}
	dl->dirs = emalloc((dl->nall + 1) * sizeof(Dir*));
	dl->mseen = dl->nall;
	dl->seen = emalloc(dl->mseen * sizeof(char*));
	for(j = 0, i = 0; d = &dl->all[i], i < dl->nall; i++){
		if(seen(dl, d->name))
			continue;
		dl->dirs[j++] = d;
	}
	dl->dirs[j] = nil;
	dl->ndirs = j;
	return dl;
}
