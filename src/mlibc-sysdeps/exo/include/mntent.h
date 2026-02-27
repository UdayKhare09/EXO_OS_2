#ifndef _MNTENT_H
#define _MNTENT_H

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

struct mntent {
    char *mnt_fsname;
    char *mnt_dir;
    char *mnt_type;
    char *mnt_opts;
    int mnt_freq;
    int mnt_passno;
};

#define MNTTAB "/etc/fstab"
#define MOUNTED "/etc/mtab"

FILE *setmntent(const char *filename, const char *type);
struct mntent *getmntent(FILE *stream);
struct mntent *getmntent_r(FILE *stream, struct mntent *mntbuf, char *buf, int buflen);
int addmntent(FILE *stream, const struct mntent *mnt);
int endmntent(FILE *stream);

#ifdef __cplusplus
}
#endif

#endif
