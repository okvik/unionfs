/*
 * Encode (Dir.type, Dir.dev, Dir.Qid.path) into Qid.path.
 * We do this by making an educated guess on how the input
 * bits are going to be laid out in the common case which
 * allows a direct encoding scheme to be used for most
 * files; otherwise we must resort to storing a full map
 * entry into a table.
 * 
 * The bits of output Qid.path are assigned as follows:
 * 
 * For directly encoded paths the bit 63 is set to zero,
 * otherwise it is set to one.  This splits the path space
 * in half.
 * 
 * For table-mapped paths the rest of the bits are simply
 * counting from 0 to 2^63-1. There is no relation between
 * input and output bits; the entire input tuple and the
 * output Qid.path are stored into a map.
 *
 * The direct encoding scheme works by assuming several
 * properties on the structure of input tuple fields.
 * First, it is evident that the Plan 9 kernel uses a very
 * small number of device types (Dir.type), the most common
 * of which is devmnt (M). Furthermore, the file servers
 * mounted through this type are the ones serving the most
 * files and are most likely to be served by unionfs.
 * Therefore we dedicate the entire space to this type
 * of device.
 * Secondly, the device instance (Dir.dev) is always a
 * simple counter value, as are the Qid.paths of most
 * file servers.  This lets us simply drop some number
 * of upper bits from each to make them fit into our space.
 * 
 * We do this as follows:
 * - 32-bit Dir.dev is reduced to its lower 28 bits.
 * - 64-bit Dir.qid.path is reduced to its lower 35 bits.
 * 
 * If type isn't M, or if the reduction of dev or path
 * results in a loss of information, that is, if some of
 * the cut bits are set; then the tuple must be table-mapped.
 *
 * The amount of bits allocated to each encoded field
 * was determined by assuming that it is not very common for
 * most file servers to host many billions of files, while a
 * busy server will easily make many millions of mounts during
 * its uptime.
 * Making the kernel reuse device instance numbers could
 * be done to help the matter significantly, letting us use
 * almost the entire space just for paths.
 */
 
#include "unionfs.h"

#define mask(v) ((1ull << v) - 1)

enum {
	Qmapsize = 1024,

	MapBit = 63,
	DevBits = 28,
	PathBits = 35,
};

typedef struct Qmap Qmap;
struct Qmap {
	ushort type;
	uint dev;
	uvlong path;
	uvlong qpath;
	Qmap *next;
};

RWLock qidmaplock;
Qmap *qidmap[Qmapsize];

uvlong
qhash(Dir *d)
{
	uvlong h;
	usize i;
	uchar *key;
	
	h = 14695981039346656037ULL;
	key = (uchar*)&d->type;
	for(i = 0; i < sizeof(d->type); i++){
		h ^= key[i];
		h *= 1099511628211ULL;
	}
	key = (uchar*)&d->dev;
	for(i = 0; i < sizeof(d->dev); i++){
		h ^= key[i];
		h *= 1099511628211ULL;
	}
	key = (uchar*)&d->qid.path;
	for(i = 0; i < sizeof(d->qid.path); i++){
		h ^= key[i];
		h *= 1099511628211ULL;
	}
	return h;
}

Qid
qencode(Dir *d)
{
	ushort type;
	uint dev;
	uvlong path;
	Qid qid;

	qid.vers = d->qid.vers;
	qid.type = d->qid.type;
	type = d->type;
	dev = d->dev;
	path = d->qid.path;
	if(type == 'M'
	&& (dev & ~mask(DevBits)) == 0
	&& (path & ~mask(PathBits)) == 0){
		qid.path = 0
			| (0ull << MapBit)
			| ((uvlong)dev) << (63 - DevBits)
			| path;
		return qid;
	}

	static uvlong qpath = 0;
	Qmap *q;
	uvlong h;

	h = qhash(d) % Qmapsize;
	rlock(&qidmaplock);
	for(q = qidmap[h]; q != nil; q = q->next){
		if(q->type == type && q->dev == dev && q->path == path){
			qid.path = q->qpath;
			runlock(&qidmaplock);
			return qid;
		}
	}
	runlock(&qidmaplock);
	q = emalloc(sizeof(Qmap));
	q->type = type;
	q->dev = dev;
	q->path = path;
	wlock(&qidmaplock);
	q->qpath = 0
		| (1ull << MapBit)
		| qpath++;
	q->next = qidmap[h];
	qidmap[h] = q;
	qid.path = q->qpath;
	wunlock(&qidmaplock);
	return qid;
}
