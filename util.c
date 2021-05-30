#include <u.h>
#include <libc.h>

void
usage(void)
{
	fprint(2, "%s [-abiC] [-m mtpt] [-s srv] [-c] path ...\n", argv0);
	exits("usage");
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
