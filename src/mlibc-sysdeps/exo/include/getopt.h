#ifndef _GETOPT_H
#define _GETOPT_H

#include <bits/getopt.h>

#ifdef __cplusplus
extern "C" {
#endif

int getopt(int __argc, char *const __argv[], const char *__optstring);

extern char *optarg;
extern int optind;
extern int opterr;
extern int optopt;

#ifdef __cplusplus
}
#endif

#endif