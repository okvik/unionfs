#include <u.h>
#include <libc.h>
#include <thread.h>
#include "unionfs.h"

void
usage(void)
{
	fprint(2, "%s [-abiC] [-m mtpt] [-s srv] [-c] path ...\n", argv0);
	exits("usage");
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

/*
 * Error-checked library functions
 */

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
