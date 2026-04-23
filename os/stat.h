#ifndef __STAT_H__
#define __STAT_H__

#include "types.h"

/* File-type bits for Stat.mode */
#define STAT_DIR  0x040000   /* directory  */
#define STAT_FILE 0x100000   /* regular file */

struct Stat {
    uint64 dev;      /* drive number — always 0 in this implementation */
    uint64 ino;      /* inode number */
    uint32 mode;     /* file type (STAT_DIR or STAT_FILE) */
    uint32 nlink;    /* number of hard links */
    uint64 pad[7];   /* reserved for compatibility */
};

#endif /* __STAT_H__ */