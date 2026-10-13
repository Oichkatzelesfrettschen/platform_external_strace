#ifndef MSM_GLUE_STUBS_H
#define MSM_GLUE_STUBS_H
#define _GNU_SOURCE
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <errno.h>
#include <unistd.h>
FILE *msm_test_fopen(const char *, const char *);
#define fopen msm_test_fopen
typedef uint64_t kernel_ulong_t;
struct tcb { int pid; kernel_ulong_t u_arg[3]; bool entry, failed; void *private_data; };
#define entering(tcp) ((tcp)->entry)
#define syserror(tcp) ((tcp)->failed)
extern unsigned current_wordsize;
#define RVAL_IOCTL_DECODED 8
int getfdpath(struct tcb *, int, char *, unsigned int);
int umoven(struct tcb *, kernel_ulong_t, unsigned int, void *);
void tprintf(const char *, ...);
void tprints(const char *);
void *get_tcb_priv_data(struct tcb *);
int set_tcb_priv_data(struct tcb *, void *, void (*)(void *));
#endif
