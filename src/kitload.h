/* State shared by the hooks (hooks.c), the KITLOAD page (page.c) and the kit reader (kit.c). */
#ifndef KITLOAD_H
#define KITLOAD_H
#include <stdio.h>

#define KLOG(fmt, ...) fprintf(stderr, "sample-kitload: " fmt "\n", ##__VA_ARGS__)

#define NBANKS 8

struct kl_state {
    int ask;             /* B3 Load Kit stops at the page (ALL / LOAD / CANCEL); 0: Load Kit as MPC does it */
    int src[NBANKS];     /* per project bank: the kit bank loaded into it, or -1 (keep the project's pads) */
    int active;          /* the hooks are installed (firmware matched) */
    int page_ok;         /* the page is open on the draw handler */
    int last_pads;       /* pads changed by the last mapped load, -1 none yet */
    /* the Load Kit waiting at the page */
    volatile int prompt; /* 1: a kit is waiting; the page shows it */
    char kit_name[64];
    int kit_used[NBANKS];   /* pads with a sample per kit bank; all -1 when the kit couldn't be read */
};

extern struct kl_state KL;

enum { KL_GO_NONE, KL_GO_MAPPED, KL_GO_ALL, KL_GO_CANCEL };

int  kl_mapped(void);                 /* how many project banks have a kit bank */
void kl_conf_load(void);
void kl_conf_save(void);
void kl_page_start(void);
void kl_page_show(void);              /* the page takes the screen */
void kl_page_hide(void);
int  kl_hooks_install(void);          /* 0 when the firmware matched and every hook is in */
void kl_answer(int go);               /* page.c: what to do with the waiting Load Kit */
int  kl_page_shown(void);             /* the page has the screen */
int  kl_kit_scan(const char *path, int used[NBANKS]);   /* kit.c: pads read, or -1 */

#endif
