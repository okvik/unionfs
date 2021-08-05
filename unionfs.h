#include <u.h>
#include <libc.h>
#include <String.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>

typedef struct Branch Branch;
typedef struct FILE FILE;
typedef struct Mtpt Mtpt;
typedef struct Dirlist Dirlist;

struct Branch {
	char *root;
	int create;
};

struct FILE {
	Dir;
	String *realpath;
	String *path;

	int fd;
	Mtpt *mtpt;
	Dirlist *dl;
	
	int pid;
	int flushed;
};

struct Mtpt {
	char *path;
	Mtpt *next;
};

struct Dirlist {
	Dir *all;
	long nall;
	Dir **dirs;
	long ndirs;
	
	/* implementation-specific */
	char **seen;
	usize nseen, mseen;
};

void usage(void);
Qid qencode(Dir*);
Dirlist *dirlist(int);
void dirlistfree(Dirlist*);
void *emalloc(ulong);
char *estrdup(char*);
