typedef struct Branch Branch;
typedef struct FILE FILE;
typedef struct Mtpt Mtpt;

struct Branch {
	char *root;
	int create;
};

struct FILE {
	Dir;
	char *realpath;
	char *path;

	int fd;
	Mtpt *mtpt;
	Dir *dirs;
	long ndirs;
};

struct Mtpt {
	char *path;
	Mtpt *next;
};

void usage(void);
Qid qencode(Dir*);
char *mkpath(char*, ...);
void *emalloc(ulong);
char *estrdup(char*);
