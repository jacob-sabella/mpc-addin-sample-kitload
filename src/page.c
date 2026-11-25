/* The KITLOAD page (through mpc-addin-sample-draw-handler) and the settings file.
 *
 * After B3 Load Kit (ask on), the page comes up with the kit:
 *
 *   [ALL]      [LOAD]      [CANCEL]      (along the top: B1-B3 are above the screen)
 *   LOAD KIT: 909 KIT
 *   KIT BANKS  A16 B16 C16 D4
 *   > BANK A  <- KIT A
 *     BANK B  -
 *     ...
 *
 * One row per project bank: the kit bank that goes there, or "-" (the bank keeps its pads). Dial: pick a row. Dial
 * pressed (or K1): that row's kit bank. ALL: the whole kit, as MPC does it. LOAD: only the mapped banks. CANCEL: nothing.
 *
 * Opened from RACK's page list with no kit waiting, it shows the settings: ASK (B3 stops here, or loads the whole kit
 * as MPC does), and the map the next kit starts with (SET steps the row, CLEAR empties it). The dial pressed goes back
 * to MPC. */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "kitload.h"
#include "sample_draw.h"

struct kl_state KL = { .ask = 1, .src = { -1, -1, -1, -1, -1, -1, -1, -1 }, .last_pads = -1,
                       .kit_used = { -1, -1, -1, -1, -1, -1, -1, -1 } };

static int sel;
static sdc_page page;
static volatile int want_show;
static volatile long long conf_dirty;   /* when the settings last changed and weren't saved yet (ms), 0 none */

static long long now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000LL + t.tv_nsec / 1000000; }

int kl_mapped(void)
{
    int n = 0;
    for (int b = 0; b < NBANKS; b++) n += KL.src[b] >= 0;
    return n;
}

/* ---- settings: <addin dir>/mpc_sample_kitload.conf ---- */
static void conf_path(char *out, size_t n)
{
    out[0] = 0;
    FILE *f = fopen("/proc/self/maps", "re");
    if (!f) return;
    uintptr_t me = (uintptr_t)&conf_path;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        unsigned long a, b;
        char *path = strchr(line, '/');
        if (!path || sscanf(line, "%lx-%lx", &a, &b) != 2 || me < a || me >= b) continue;
        path[strcspn(path, "\n")] = 0;
        char *slash = strrchr(path, '/');
        if (slash) *slash = 0;
        snprintf(out, n, "%s/mpc_sample_kitload.conf", path);
        break;
    }
    fclose(f);
}

/* map=A:C,B:D  (kit bank : project bank) */
static void parse_map(const char *v)
{
    for (int b = 0; b < NBANKS; b++) KL.src[b] = -1;
    for (const char *p = v; *p; p++)
        if (p[0] >= 'A' && p[0] <= 'H' && p[1] == ':' && p[2] >= 'A' && p[2] <= 'H') {
            KL.src[p[2] - 'A'] = p[0] - 'A';
            p += 2;
        }
}

void kl_conf_load(void)
{
    char path[512];
    conf_path(path, sizeof path);
    FILE *f = path[0] ? fopen(path, "re") : NULL;
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq++ = 0;
        eq[strcspn(eq, "\r\n")] = 0;
        if (!strcmp(line, "ask")) KL.ask = atoi(eq) != 0;
        else if (!strcmp(line, "map")) parse_map(eq);
    }
    fclose(f);
}

void kl_conf_save(void)
{
    char path[512], tmp[520];
    conf_path(path, sizeof path);
    if (!path[0]) return;
    snprintf(tmp, sizeof tmp, "%s.new", path);
    FILE *f = fopen(tmp, "we");
    if (!f) return;
    fprintf(f, "# KITLOAD page settings (the page writes this file)\n");
    fprintf(f, "# ask=1: B3 Load Kit stops at the page (ALL / LOAD / CANCEL). ask=0: Load Kit as MPC does it.\n");
    fprintf(f, "# map=KITBANK:PROJECTBANK,...  the map the page starts with, e.g. A:C,B:D (the kit's A on bank C, its B on D).\n");
    fprintf(f, "ask=%d\nmap=", KL.ask);
    int first = 1;
    for (int b = 0; b < NBANKS; b++)
        if (KL.src[b] >= 0) { fprintf(f, "%s%c:%c", first ? "" : ",", 'A' + KL.src[b], 'A' + b); first = 0; }
    fprintf(f, "\n");
    fclose(f);
    rename(tmp, path);
}

/* ---- page ---- */
#define BG     0x0a0c14
#define FG     0xd8dce8
#define DIM    0x5a6070
#define ACCENT 0x39c0ff
#define WARN   0xffb040
#define SELBG  0x1c2438

static int kit_known(void) { return KL.kit_used[0] >= 0; }

static void keys(sdc_canvas *cv, const char *k0, const char *k1, const char *k2)
{
    const char *k[3] = { k0, k1, k2 };
    for (int i = 0; i < 3; i++) {
        int x = 4 + i * 106;
        sdc_frame(cv, x, 2, 100, 18, 1, DIM);   /* along the top: B1-B3 sit above the screen */
        sdc_text(cv, x + (100 - sdc_text_width(k[i], 1)) / 2, 8, 1, FG, k[i]);
    }
}

static void rows(sdc_canvas *cv, int y0, int active)
{
    for (int b = 0; b < NBANKS; b++) {
        int y = y0 + b * 19;
        if (b == sel) sdc_fill(cv, 4, y - 3, cv->w - 8, 17, SELBG);
        char row[40];
        int empty = KL.src[b] >= 0 && KL.prompt && kit_known() && KL.kit_used[KL.src[b]] == 0;
        if (KL.src[b] >= 0) snprintf(row, sizeof row, "BANK %c  <-  KIT %c%s", 'A' + b, 'A' + KL.src[b], empty ? " (EMPTY)" : "");
        else snprintf(row, sizeof row, "BANK %c  -", 'A' + b);
        sdc_text(cv, 12, y, 2, KL.src[b] >= 0 ? (empty ? WARN : active ? ACCENT : FG) : DIM, row);
    }
}

static void render(void *user, sdc_canvas *cv)
{
    (void)user;
    sdc_fill(cv, 0, 0, cv->w, cv->h, BG);
    if (!KL.active) {
        sdc_text(cv, 8, 6, 2, FG, "KITLOAD");
        sdc_text(cv, 8, 40, 2, WARN, "NOT ON THIS FIRMWARE");
        sdc_text(cv, 8, 62, 1, DIM, "THIS ADDIN IS FOR MPC SAMPLE 1.4.0. LOAD KIT IS UNCHANGED.");
        return;
    }
    if (KL.prompt) {
        char t[80];
        snprintf(t, sizeof t, "LOAD KIT: %s", KL.kit_name);
        sdc_text(cv, 8, 26, 2, FG, t);
        char u[80] = "KIT BANKS ";
        if (kit_known()) {
            int n = (int)strlen(u);
            for (int b = 0; b < NBANKS; b++)
                if (KL.kit_used[b] > 0) n += snprintf(u + n, sizeof u - (size_t)n, " %c%d", 'A' + b, KL.kit_used[b]);
        } else strcat(u, " (NOT READ)");
        sdc_text(cv, 8, 44, 1, ACCENT, u);
        rows(cv, 58, 1);
        sdc_text(cv, 8, 214, 1, DIM, "DIAL: BANK   DIAL PRESS / K1: KIT BANK FOR IT");
        sdc_text(cv, 8, 226, 1, DIM, kl_mapped() ? "LOAD: ONLY THE MAPPED BANKS CHANGE" : "MAP A BANK FOR LOAD, OR ALL FOR THE WHOLE KIT");
        keys(cv, "ALL", "LOAD", "CANCEL");
        return;
    }
    sdc_text(cv, 8, 26, 2, FG, "KITLOAD");
    const char *a = KL.ask ? "ASK ON LOAD KIT" : "LOAD KIT AS NORMAL";
    sdc_text(cv, cv->w - 8 - sdc_text_width(a, 1), 30, 1, KL.ask ? ACCENT : DIM, a);
    sdc_text(cv, 8, 44, 1, DIM, "THE MAP THE NEXT LOAD KIT STARTS WITH:");
    rows(cv, 58, 0);
    if (KL.last_pads >= 0) {
        char l[48];
        snprintf(l, sizeof l, "LAST MAPPED LOAD: %d PADS", KL.last_pads);
        sdc_text(cv, 8, 226, 1, DIM, l);
    }
    keys(cv, KL.ask ? "ASK: ON" : "ASK: OFF", "SET", "CLEAR");
}

/* the next kit bank for the row: -, then the kit's banks with pads (all of them when the kit isn't known) */
static void step_row(void)
{
    int v = KL.src[sel];
    for (int i = 0; i < NBANKS + 1; i++) {
        v = v + 1 >= NBANKS ? -1 : v + 1;
        if (v < 0 || !KL.prompt || !kit_known() || KL.kit_used[v] > 0) break;
    }
    KL.src[sel] = v;
}

static void answer(int go)
{
    if (go == KL_GO_MAPPED && !kl_mapped()) return;   /* nothing mapped: LOAD does nothing, ALL is the whole kit */
    if (go != KL_GO_CANCEL) { conf_dirty = 0; kl_conf_save(); }   /* the map is the next one's start */
    kl_answer(go);
    kl_page_hide();
}

static void on_input(void *user, const mpc_sdraw_event *e)
{
    (void)user;
    if (e->type == MPC_SDRAW_EV_DIAL) {
        sel = ((sel + e->value) % NBANKS + NBANKS) % NBANKS;
        return;
    }
    if (KL.prompt) {
        if (e->type == MPC_SDRAW_EV_KEY && e->value) {
            if (e->id == 0) answer(KL_GO_ALL);
            else if (e->id == 1) answer(KL_GO_MAPPED);
            else if (e->id == 2) answer(KL_GO_CANCEL);
            else if (e->id == 3) step_row();
        } else if (e->type == MPC_SDRAW_EV_KNOB && e->id == 0) {
            KL.src[sel] = e->value * (NBANKS + 1) / 128 - 1;
        }
        return;
    }
    int changed = 0;
    if (e->type == MPC_SDRAW_EV_KEY && e->value) {
        if (e->id == 0) { KL.ask = !KL.ask; changed = 1; }
        else if (e->id == 1) { step_row(); changed = 1; }
        else if (e->id == 2) { for (int b = 0; b < NBANKS; b++) KL.src[b] = -1; changed = 1; }
        else if (e->id == 3) kl_page_hide();
    } else if (e->type == MPC_SDRAW_EV_KNOB && e->id == 0) {
        int v = e->value * (NBANKS + 1) / 128 - 1;
        if (v != KL.src[sel]) { KL.src[sel] = v; changed = 1; }
    }
    if (changed) conf_dirty = now_ms();   /* saved by the page thread once the knob rests */
}

void kl_page_show(void) { want_show = 1; }       /* from MPC's main thread: the page thread asks for the screen */

void kl_page_hide(void)
{
    if (KL.page_ok) page.api->release(page.c);
}

int kl_page_shown(void)
{
    return KL.page_ok && page.api->has_screen(page.c);
}

static void *page_thread(void *arg)
{
    (void)arg;
    for (int i = 0; i < 120 && !KL.page_ok; i++) {           /* the draw handler may load after us */
        if (sdc_page_open(&page, "KITLOAD", 12, render, NULL) == 0) {
            if (sdc_page_set_input(&page, on_input, NULL)) {   /* a page nobody can answer: Load Kit as normal */
                KLOG("draw handler too old for input: no page, Load Kit as normal");
                return NULL;
            }
            KL.page_ok = 1;
            KLOG("KITLOAD page open");
            break;
        }
        usleep(500000);
    }
    if (!KL.page_ok) { KLOG("no draw handler: no page, Load Kit as normal"); return NULL; }
    for (;;) {
        if (want_show) {
            want_show = 0;
            for (int i = 0; i < 10 && page.api->request(page.c) != 0; i++) usleep(50000);
        }
        long long d = conf_dirty;
        if (d && now_ms() - d >= 500) { conf_dirty = 0; kl_conf_save(); }
        usleep(20000);
    }
    return NULL;
}

void kl_page_start(void)
{
    pthread_t t;
    if (pthread_create(&t, NULL, page_thread, NULL) == 0) pthread_detach(t);
}
