enum {
	Nftab = 32,
	Nftlist = 32,
};

typedef struct Branch Branch;
typedef struct F F;
typedef struct Ftab Ftab;
typedef struct Fstate Fstate;

struct Branch {
	char *root;
	int create;
};

struct F {
	Ref;
	Dir;
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
Qid qencode(Dir*);
char *mkpath(char*, ...);
Ref *copyref(Ref*);
void *emalloc(ulong);
void *erealloc(void*, ulong);
char *estrdup(char*);
