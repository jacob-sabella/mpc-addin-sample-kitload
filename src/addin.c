/* mpc-addin-sample-kitload: Load Kit on the MPC Sample, bank by bank (see hooks.c). */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "kitload.h"

static int is_mpc_sample(void)
{
    char exe[256];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0) return 0;
    exe[n] = 0;
    const char *bn = strrchr(exe, '/');
    return !strcmp(bn ? bn + 1 : exe, "MPC Sample");
}

__attribute__((constructor)) static void kl_init(void)
{
    if (getenv("MPC_SAMPLE_KITLOAD_DISABLE") || !is_mpc_sample()) return;
    kl_conf_load();
    KLOG("starting (%s)", ADDIN_VERSION);
    KL.active = kl_hooks_install() == 0;
    kl_page_start();
}
