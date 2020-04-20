enum {
	Nqbit = 5,
	Nqtab = 1<<Nqbit,
	Nftab = 32,
	Nftlist = 32,
};

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

struct Qtab {
	ushort type;
	uint dev;
	uvlong path, uniqpath;
	Qtab *next;
};

struct F {
	Ref;
	Dir;
	Qtab *qtab;
	char *path;   /* real path */
	char *fspath; /* internal path */
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

void usage(void);
char *mkpath(char*, ...);
Ref *copyref(Ref*);
void *emalloc(ulong);
void *erealloc(void*, ulong);
char *estrdup(char*);
