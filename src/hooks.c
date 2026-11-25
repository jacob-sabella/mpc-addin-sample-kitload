/* Hooks into MPC Sample 1.4.0 (the "MPC Sample" binary, sha256 473fb726...): Load Kit, bank by bank.
 *
 * What Load Kit does (LoadProjectAsKitCommand::execute, an UndoableCommand):
 *   1. runs a ClearSamplePoolCommand (empties the sample pool),
 *   2. reads the kit project into a SerialisableProjectData, loads its samples,
 *   3. takes its first drum track and runs a LoadTrackCommand with it. That command puts a new program on the track and
 *      keeps the one it replaces alive (for undo); the kit's drum program goes into the new one through
 *      MpcProgramDataImporter -> drum import (DRUM_IMPORT), which sets the program's own values and then, for each kit
 *      pad i, PAD_IMPORT(new program's pad i, kit pad i), and for all 128 pads PAD_NOTE(program, pad, kit byte).
 *
 * B3 Load Kit, with ask on (KL.ask) and the page working: our execute reads the kit's path from the command, the KITLOAD
 * page takes the screen with the kit's banks, and execute waits (MPC's main thread; buttons come in on JUCE's MIDI input
 * thread, so the page still works) for ALL / LOAD / CANCEL, then loads in MPC's own flow. If the page doesn't get the
 * screen, or loses it before an answer (the page button cycles it away), the Load Kit is cancelled rather than leave
 * MPC's main thread waiting. Undo of a cancelled one does nothing.
 *
 * A mapped load (LOAD with banks mapped), while it runs:
 *   - the ClearSamplePoolCommand does nothing (the pads that stay need their samples),
 *   - a project pad in a mapped bank whose kit pad exists gets it: PAD_IMPORT / PAD_NOTE are pointed at the kit pad at
 *     the same position in the mapped kit bank,
 *   - every other pad's PAD_IMPORT / PAD_NOTE is skipped (it comes up fresh in the new program).
 * Then, still in execute, once MPC's own load is done:
 *   - mapped pads the import never reached (past the kit's last pad) get their kit pad (PAD_IMPORT),
 *   - every other pad gets the user's own back from the replaced program: PAD_EXPORT into a kit pad as scratch, then
 *     PAD_IMPORT of that, and its PAD_NOTE byte. A scratch kit pad that a mapped pad came from is written back from that
 *     pad afterwards, so a redo imports the same thing.
 * Undo is MPC's own: LoadTrackCommand restores the whole program it kept. Redo re-runs execute; the map used the first
 * time is kept per command (until MPC deletes the command) so a redo does the same thing.
 *
 * Every address is checked against the instructions/pointers expected there before anything is patched, and either
 * every patch goes in or none does: on any other firmware the addin stays off and Load Kit is untouched.
 *
 * Built with -mgeneral-regs-only: the hook callbacks run between MPC's caller and the original function and must not
 * touch the FP/SIMD argument registers. */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

#include "kitload.h"

/* ---- firmware 1.4.0 ---- */
#define LPAK_VT       0x76e5480UL   /* vtable of LoadProjectAsKitCommand */
#define LPAK_EXEC     0x1aeeab0UL
#define LPAK_UNDO     0x1aee870UL
#define LPAK_DTOR     0x1aec030UL   /* its complete destructor (the deleting one calls it) */
#define CSP_VT        0x76e4ef0UL   /* vtable of ClearSamplePoolCommand */
#define CSP_EXEC      0x1ae9000UL
#define CSP_UNDO      0x1ae9930UL
#define CSP_DTOR      0x1ae9ff0UL   /* its complete destructor */
#define CSP_DTOR_DEL  0x1ae9f30UL   /* its deleting destructor (the complete one inlined) */
#define LTC_VT        0x7726e50UL   /* vtable of LoadTrackCommand */
#define LTC_EXEC      0x224e5f0UL
#define LTC_OLDPROG   0x3e50        /* LoadTrackCommand: the program it replaces, kept alive for undo (the user's pads) */
#define VT_EXEC       0x40          /* UndoableCommand slot 8: execute, slot 9: undo */
#define VT_UNDO       0x48
#define DRUM_IMPORT   0x2d7fca0UL   /* (live drum program, SerialisableDrumProgramData *) */
#define PAD_IMPORT    0x2d1ee10UL   /* (live instrument, SerialisableInstrument *) */
#define PAD_EXPORT    0x2d1f920UL   /* (live instrument, SerialisableInstrument *): the reverse of PAD_IMPORT */
#define PAD_NOTE      0x2dd90c0UL   /* (live drum program, int pad, uint8 value from data + 0x4a + pad) */
#define DATA_INSTR    0xd0          /* std::vector<SerialisableInstrument> in the drum data */
#define DATA_PADBYTES 0x4a          /* 128 bytes, PAD_NOTE's values */
#define INSTR_SIZE    0x1340
#define PROG_INSTR    0x28b0        /* live drum program: std::vector<Instrument *> of its pads */
#define PROG_NOTES    0x1b40        /*   uint8_t * of the PAD_NOTE values, per pad */
#define NPADS         (NBANKS * 16)

struct sig { uintptr_t off; uint32_t w[4]; };
static const struct sig SIGS[] = {
    { LPAK_EXEC,    { 0xd503233f, 0xa9ba7bfd, 0x52800001, 0x910003fd } },
    { LPAK_UNDO,    { 0xd503233f, 0xd10503ff, 0x52800001, 0xa90e7bfd } },
    { LPAK_DTOR,    { 0xd503233f, 0xa9bd7bfd, 0xb002dfc1, 0x91120021 } },
    { LTC_EXEC,     { 0xd503233f, 0xd104c3ff, 0xa90d7bfd, 0x910343fd } },
    { CSP_EXEC,     { 0xd503233f, 0xd10543ff, 0xf002f942, 0xf9412842 } },
    { CSP_UNDO,     { 0xd503233f, 0xd10683ff, 0xf002f942, 0xf9412842 } },
    { CSP_DTOR,     { 0xd503233f, 0xa9bd7bfd, 0xf002dfc1, 0x913bc021 } },
    { CSP_DTOR_DEL, { 0xd503233f, 0xa9bd7bfd, 0xf002dfc1, 0x913bc021 } },
    { DRUM_IMPORT,  { 0xd503233f, 0xd10203ff, 0xd2837e02, 0xd2838003 } },
    { PAD_IMPORT,   { 0xd503233f, 0xd10303ff, 0xd00267a2, 0xf9412842 } },
    { PAD_EXPORT,   { 0xd503233f, 0xd10343ff, 0xb00267a2, 0xf9412842 } },
    { PAD_NOTE,     { 0xd503233f, 0xd100c3ff, 0xf00261c3, 0xf9412863 } },
};

static uintptr_t g_base;

static uintptr_t find_base(void)
{
    FILE *f = fopen("/proc/self/maps", "re");
    if (!f) return 0;
    char line[512];
    uintptr_t base = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned long a, b, off;
        char perms[8];
        if (sscanf(line, "%lx-%lx %7s %lx", &a, &b, perms, &off) < 4) continue;
        char *path = strchr(line, '/');
        if (!path) continue;
        path[strcspn(path, "\n")] = 0;
        const char *bn = strrchr(path, '/');
        if (bn && !strcmp(bn + 1, "MPC Sample") && off == 0) { base = a; break; }
    }
    fclose(f);
    return base;
}

static int safe_read(uintptr_t a, void *out, size_t n)
{
    struct iovec l = { out, n }, r = { (void *)a, n };
    return process_vm_readv(getpid(), &l, 1, &r, 1, 0) == (ssize_t)n ? 0 : -1;
}

static uintptr_t rd(uintptr_t a) { uintptr_t v = 0; return a && !safe_read(a, &v, 8) ? v : 0; }

/* ---- patching: prepared first, then all put in or none ---- */
static int set_prot(uintptr_t addr, size_t len, int prot)
{
    long pg = sysconf(_SC_PAGESIZE);
    uintptr_t p = addr & ~(uintptr_t)(pg - 1);
    size_t span = ((addr + len - p) + (size_t)pg - 1) & ~(size_t)(pg - 1);
    return mprotect((void *)p, span, prot);
}

static void *alloc_near(uintptr_t target)
{
    long pg = sysconf(_SC_PAGESIZE);
    uintptr_t base = target & ~(uintptr_t)(pg - 1);
    for (int dir = 0; dir < 2; dir++)
        for (uintptr_t off = 0x200000; off < 0x7000000; off += (uintptr_t)pg) {
            uintptr_t want = dir ? base + off : base - off;
            void *p = mmap((void *)want, (size_t)pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
            if (p == MAP_FAILED) continue;
            if ((uintptr_t)p == want) return p;
            munmap(p, (size_t)pg);
        }
    return NULL;
}

/* A hook on a function starting with PACIASP. cb gets the argument registers (regs[0..7], regs[8] = x8) and may
 * change them; returning nonzero skips the function (the caller sees regs[0] in x0), zero runs it with the regs. */
typedef int (*hook_cb)(uint64_t *regs);

struct hook { uintptr_t target; uint32_t *t; uint32_t branch; int in; const char *name; };

static int hook_prepare(struct hook *h, uintptr_t target, hook_cb cb, const char *name)
{
    memset(h, 0, sizeof *h);
    h->target = target; h->name = name;
    if (*(volatile uint32_t *)target != 0xd503233f) { KLOG("%s: no PACIASP at %p", name, (void *)target); return -1; }
    uint32_t *t = alloc_near(target);
    if (!t) { KLOG("%s: no trampoline space", name); return -1; }
    long pg = sysconf(_SC_PAGESIZE);
    int64_t fwd = ((int64_t)(uintptr_t)t - (int64_t)target) / 4;
    if (fwd < -0x2000000 || fwd > 0x1FFFFFF) { KLOG("%s: trampoline out of range", name); munmap(t, (size_t)pg); return -1; }
    static const uint32_t pre[] = {
        0xd10183ff,                         /* sub  sp, sp, #0x60          */
        0xa90007e0, 0xa9010fe2, 0xa90217e4, /* stp  x0..x5                 */
        0xa9031fe6, 0xa9047be8,             /* stp  x6,x7 ; stp x8,x30     */
        0x910003e0,                         /* mov  x0, sp                 */
        0x580001b0,                         /* ldr  x16, =cb (word 20)     */
        0xd63f0200,                         /* blr  x16                    */
        0x2a0003f1,                         /* mov  w17, w0                */
        0xa94007e0, 0xa9410fe2, 0xa94217e4, /* ldp  x0..x5                 */
        0xa9431fe6, 0xa9447be8,             /* ldp  x6,x7 ; ldp x8,x30     */
        0x910183ff,                         /* add  sp, sp, #0x60          */
        0x35000071,                         /* cbnz w17, ret (word 19)     */
        0xd503233f,                         /* paciasp (the one replaced)  */
    };
    memcpy(t, pre, sizeof pre);                                      /* words 0-17 */
    int64_t back = ((int64_t)(target + 4) - (int64_t)(uintptr_t)&t[18]) / 4;
    t[18] = 0x14000000 | ((uint32_t)back & 0x3FFFFFF);               /* b target+4 */
    t[19] = 0xd65f03c0;                                              /* ret */
    *(uint64_t *)&t[20] = (uint64_t)(uintptr_t)cb;
    if (mprotect(t, (size_t)pg, PROT_READ | PROT_EXEC) != 0) {
        KLOG("%s: mprotect trampoline: %s", name, strerror(errno)); munmap(t, (size_t)pg); return -1;
    }
    __builtin___clear_cache((char *)t, (char *)&t[22]);
    h->t = t;
    h->branch = 0x14000000 | ((uint32_t)fwd & 0x3FFFFFF);
    return 0;
}

static int code_write(uintptr_t target, uint32_t word)
{
    if (set_prot(target, 4, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) return -1;
    *(volatile uint32_t *)target = word;
    __builtin___clear_cache((char *)target, (char *)target + 4);
    set_prot(target, 4, PROT_READ | PROT_EXEC);
    return 0;
}

static int hook_commit(struct hook *h)
{
    if (code_write(h->target, h->branch)) { KLOG("%s: mprotect: %s", h->name, strerror(errno)); return -1; }
    h->in = 1;
    return 0;
}

static void hook_remove(struct hook *h)
{
    if (h->in && !code_write(h->target, 0xd503233f)) h->in = 0;
}

static void hook_free(struct hook *h)
{
    if (h->t && !h->in) munmap(h->t, (size_t)sysconf(_SC_PAGESIZE));
    h->t = NULL;
}

/* Replace a vtable slot (in RELRO). 0 when done. */
static int slot_write(uintptr_t slot, uintptr_t v)
{
    if (set_prot(slot, 8, PROT_READ | PROT_WRITE) != 0) return -1;
    *(volatile uintptr_t *)slot = v;
    set_prot(slot, 8, PROT_READ);
    return 0;
}

/* ---- the commands: what each Load Kit did, until MPC deletes it ---- */
typedef int (*cmd_fn)(void *self);
static cmd_fn lpak_exec_orig, lpak_undo_orig, csp_exec_orig, csp_undo_orig, ltc_exec_orig;
typedef void (*pad_xfer_fn)(void *inst, void *ser);
typedef void (*pad_note_fn)(void *prog, int pad, unsigned value);
static pad_xfer_fn pad_export, pad_import;
static pad_note_fn pad_note;

enum { C_CANCELLED = 1, C_RAN = 2 };
#define NCMD 256
static struct { void *cmd; int state, mapped, src[NBANKS]; } cmds[NCMD];
static void *csp_skipped[NCMD];
static unsigned cmd_evict, csp_evict;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

/* (lock held) */
static int cmd_find(void *self)
{
    for (int i = 0; i < NCMD; i++) if (cmds[i].cmd == self) return i;
    return -1;
}

static void cmd_set(void *self, int state, int mapped, const int src[NBANKS])
{
    pthread_mutex_lock(&lock);
    int i = cmd_find(self);
    if (i < 0) i = cmd_find(NULL);
    if (i < 0) i = (int)(cmd_evict++ % NCMD);   /* 256 Load Kits alive at once: the oldest is forgotten */
    cmds[i].cmd = self; cmds[i].state = state; cmds[i].mapped = mapped;
    memcpy(cmds[i].src, src, sizeof cmds[i].src);
    pthread_mutex_unlock(&lock);
}

static void csp_mark(void *self, int skipped)
{
    pthread_mutex_lock(&lock);
    int i;
    for (i = 0; i < NCMD && csp_skipped[i] != self; i++) ;
    if (skipped && i == NCMD) {
        for (i = 0; i < NCMD && csp_skipped[i]; i++) ;
        if (i == NCMD) i = (int)(csp_evict++ % NCMD);
        csp_skipped[i] = self;
    } else if (!skipped && i < NCMD) csp_skipped[i] = NULL;
    pthread_mutex_unlock(&lock);
}

static int csp_was_skipped(void *self)
{
    pthread_mutex_lock(&lock);
    int r = 0;
    for (int i = 0; i < NCMD; i++) if (csp_skipped[i] == self) r = 1;
    pthread_mutex_unlock(&lock);
    return r;
}

/* MPC deletes a command: forget it (its address can come back for a new one) */
static int on_lpak_dtor(uint64_t *regs)
{
    pthread_mutex_lock(&lock);
    int i = cmd_find((void *)(uintptr_t)regs[0]);
    if (i >= 0) cmds[i].cmd = NULL;
    pthread_mutex_unlock(&lock);
    return 0;
}

static int on_csp_dtor(uint64_t *regs)
{
    csp_mark((void *)(uintptr_t)regs[0], 0);
    return 0;
}

/* ---- the mapped load ---- */
static struct {
    volatile int on;                  /* a mapped Load Kit is running */
    int src[NBANKS];
    uintptr_t data, begin;            /* the kit's drum data and its pads */
    int kcount;                       /*   how many */
    uintptr_t ltc;                    /* the LoadTrackCommand running inside the mapped load */
    uintptr_t newp, newp_vt;          /* the program the kit goes into */
    uintptr_t oldp, oldp_vt;          /* the program it replaces (the user's pads) */
    int pads, kept, lost;
} M;

/* the kit pad for project pad `pad`, or -1 (the project's own pad stays) */
static int kit_pad(int pad)
{
    if (pad < 0 || pad >= NPADS) return -1;
    int s = M.src[pad / 16];
    int k = s < 0 ? -1 : s * 16 + pad % 16;
    return k >= 0 && k < M.kcount ? k : -1;
}

/* ---- the kit's path: a juce::String-like char * in the command (+0x60 on 1.4.0), else any char * there to a .xpj ---- */
static int read_cstr(uintptr_t p, char *out, size_t cap)
{
    if (p < 0x10000) return -1;
    size_t n = 0;
    while (n + 1 < cap) {
        unsigned char c;
        if (safe_read(p + n, &c, 1)) return -1;
        if (!c) break;
        if (c < 32 || c == 127) return -1;   /* UTF-8 (>= 0x80) is fine: names with accents */
        out[n++] = (char)c;
    }
    out[n] = 0;
    return n > 4 && !strcmp(out + n - 4, ".xpj") ? 0 : -1;
}

static int kit_path(void *self, char *out, size_t cap)
{
    if (!read_cstr(rd((uintptr_t)self + 0x60), out, cap)) return 0;
    for (int off = 0; off < 0x100; off += 8)
        if (!read_cstr(rd((uintptr_t)self + (uintptr_t)off), out, cap)) return 0;
    return -1;
}

/* After MPC's own load (main thread, inside execute; the hooks are off again): the pads the import skipped or never
 * reached. Returns -1 when the programs can't be trusted (nothing done). */
static int finish_load(void)
{
    if (!M.data || !M.ltc || rd(M.ltc) != g_base + LTC_VT || !M.newp || rd(M.newp) != M.newp_vt ||
        !M.oldp || rd(M.oldp) != M.oldp_vt) {
        KLOG("after the load: track command or programs gone (cmd %#lx, new %#lx, old %#lx): unmapped banks left empty",
             (unsigned long)M.ltc, (unsigned long)M.newp, (unsigned long)M.oldp);
        return -1;
    }
    uintptr_t nb = rd(M.newp + PROG_INSTR), ne = rd(M.newp + PROG_INSTR + 8);
    uintptr_t ob = rd(M.oldp + PROG_INSTR), oe = rd(M.oldp + PROG_INSTR + 8);
    uintptr_t onotes = rd(M.oldp + PROG_NOTES);
    if (!nb || ne < nb || (ne - nb) % 8 || !ob || oe < ob || (oe - ob) % 8) {
        KLOG("after the load: pad lists unreadable: unmapped banks left empty");
        return -1;
    }
    int ncount = (int)((ne - nb) / 8), ocount = (int)((oe - ob) / 8);
    uintptr_t pad_vt = rd(rd(nb));   /* every pad of a drum program is the same kind of object */

    /* mapped pads past the kit's last pad: PAD_IMPORT never came for them */
    for (int pad = M.kcount; pad < NPADS && pad < ncount; pad++) {
        int k = kit_pad(pad);
        uintptr_t n = rd(nb + (uintptr_t)pad * 8);
        if (k < 0 || !n || rd(n) != pad_vt) continue;
        pad_import((void *)n, (void *)(M.begin + (uintptr_t)k * INSTR_SIZE));
        M.pads++;
    }

    /* the scratch: a kit pad no mapped pad came from, else kit pad 0 (written back afterwards from a pad it went to) */
    if (M.kcount < 1) { KLOG("after the load: an empty kit, nothing to copy through: unmapped banks left empty"); return -1; }
    int from[NPADS], j = -1, back = -1;
    memset(from, 0, sizeof from);
    for (int pad = 0; pad < NPADS; pad++) { int k = kit_pad(pad); if (k >= 0) from[k] = 1; }
    for (int k = 0; k < M.kcount && k < NPADS; k++) if (!from[k]) { j = k; break; }
    if (j < 0) {
        j = 0;
        for (int pad = 0; pad < NPADS && back < 0; pad++) if (kit_pad(pad) == 0) back = pad;
    }
    void *scratch = (void *)(M.begin + (uintptr_t)j * INSTR_SIZE);

    /* the user's own pads, back from the replaced program */
    for (int pad = 0; pad < NPADS; pad++) {
        if (kit_pad(pad) >= 0) continue;
        uintptr_t o = pad < ocount ? rd(ob + (uintptr_t)pad * 8) : 0, n = pad < ncount ? rd(nb + (uintptr_t)pad * 8) : 0;
        if (!o || !n || rd(o) != pad_vt || rd(n) != pad_vt) { M.lost++; continue; }
        pad_export((void *)o, scratch);
        pad_import((void *)n, scratch);
        uint8_t v;
        if (onotes && !safe_read(onotes + (uintptr_t)pad, &v, 1)) pad_note((void *)M.newp, pad, v);
        M.kept++;
    }
    if (back >= 0) {
        uintptr_t n = rd(nb + (uintptr_t)back * 8);
        if (n && rd(n) == pad_vt) pad_export((void *)n, scratch);
    }
    return 0;
}

/* ---- running Load Kit ---- */
static int run(void *self, int mapped, const int src[NBANKS])
{
    if (!mapped) {
        KLOG("Load Kit: all banks");
        return lpak_exec_orig(self);
    }
    char desc[64]; int n = 0;
    desc[0] = 0;
    for (int b = 0; b < NBANKS; b++) if (src[b] >= 0) n += snprintf(desc + n, sizeof desc - (size_t)n, " %c<-%c", 'A' + b, 'A' + src[b]);
    KLOG("Load Kit: mapped (project<-kit)%s", desc);
    memset((void *)&M, 0, sizeof M);
    memcpy(M.src, src, sizeof M.src);
    __atomic_store_n(&M.on, 1, __ATOMIC_RELEASE);
    int ret = lpak_exec_orig(self);
    __atomic_store_n(&M.on, 0, __ATOMIC_RELEASE);
    if (M.data) finish_load();
    KLOG("Load Kit: done (%d), %d pads from the kit, %d of the project's kept%s%s", ret, M.pads, M.kept,
         M.lost ? ", some left empty" : "", M.data ? "" : "; the drum import never ran");
    KL.last_pads = M.data ? M.pads : -1;
    return ret;
}

/* B3 Load Kit with ask on: execute waits here, on MPC's main thread, while the page asks. MPC's own screen stays as it
 * was meanwhile (the page covers it); the buttons are read on JUCE's MIDI input thread and the page draws from its own,
 * so the page works. If the page never gets the screen, or loses it (the page button), the Load Kit is cancelled. */
static volatile int answer;

void kl_answer(int go) { __atomic_store_n(&answer, go, __ATOMIC_RELEASE); }

static int ask(const char *path)
{
    const char *bn = strrchr(path, '/');
    bn = bn ? bn + 1 : path;
    int len = (int)strlen(bn) - 4;
    if (len > (int)sizeof KL.kit_name - 1) len = (int)sizeof KL.kit_name - 1;
    for (int i = 0; i < len; i++) KL.kit_name[i] = (unsigned char)bn[i] < 0x80 ? bn[i] : '?';   /* the font is ASCII */
    KL.kit_name[len] = 0;
    if (kl_kit_scan(path, KL.kit_used) < 0) for (int b = 0; b < NBANKS; b++) KL.kit_used[b] = -1;
    __atomic_store_n(&answer, KL_GO_NONE, __ATOMIC_RELEASE);
    KL.prompt = 1;
    KLOG("Load Kit: %s, asking", path);
    kl_page_show();
    int go, seen = 0;
    for (int ms = 0; (go = __atomic_load_n(&answer, __ATOMIC_ACQUIRE)) == KL_GO_NONE; ms += 20) {
        if (kl_page_shown()) seen = 1;
        else if (seen || ms >= 3000) {
            if ((go = __atomic_load_n(&answer, __ATOMIC_ACQUIRE)) != KL_GO_NONE) break;   /* answered as it closed */
            KLOG("%s: Load Kit cancelled", seen ? "the page was closed" : "the page didn't get the screen");
            go = KL_GO_CANCEL;
            break;
        }
        if (ms > 10 * 60 * 1000) { go = KL_GO_CANCEL; KLOG("no answer in 10 minutes: Load Kit cancelled"); break; }
        usleep(20000);
    }
    KL.prompt = 0;
    kl_page_hide();
    return go;
}

static int lpak_exec(void *self)
{
    pthread_mutex_lock(&lock);
    int i = cmd_find(self), state = 0, mapped = 0, src[NBANKS];
    if (i >= 0) { state = cmds[i].state; mapped = cmds[i].mapped; memcpy(src, cmds[i].src, sizeof src); }
    pthread_mutex_unlock(&lock);
    if (state == C_RAN) return run(self, mapped, src);     /* redo: as the first time */

    char path[512];
    int go = KL_GO_ALL;
    if (KL.ask && KL.page_ok) {
        if (kit_path(self, path, sizeof path) == 0) go = ask(path);
        else KLOG("Load Kit: the kit's path not found, loading the whole kit");
    }
    memcpy(src, KL.src, sizeof src);
    if (go == KL_GO_CANCEL) {
        cmd_set(self, C_CANCELLED, 0, src);
        KLOG("Load Kit: cancelled");
        return 0;
    }
    mapped = go == KL_GO_MAPPED && kl_mapped() > 0;
    cmd_set(self, C_RAN, mapped, src);
    return run(self, mapped, src);
}

static int lpak_undo(void *self)
{
    pthread_mutex_lock(&lock);
    int i = cmd_find(self), cancelled = i >= 0 && cmds[i].state == C_CANCELLED;
    pthread_mutex_unlock(&lock);
    if (cancelled) { KLOG("undo of a Load Kit that loaded nothing"); return 0; }
    return lpak_undo_orig(self);
}

static int ltc_exec(void *self)
{
    if (__atomic_load_n(&M.on, __ATOMIC_ACQUIRE)) M.ltc = (uintptr_t)self;
    return ltc_exec_orig(self);
}

static int csp_exec(void *self)
{
    if (__atomic_load_n(&M.on, __ATOMIC_ACQUIRE)) {
        csp_mark(self, 1);
        KLOG("sample pool kept");
        return 0;
    }
    csp_mark(self, 0);   /* runs for real (a redo outside a mapped load): its undo is real too */
    return csp_exec_orig(self);
}

static int csp_undo(void *self)
{
    return csp_was_skipped(self) ? 0 : csp_undo_orig(self);
}

static int on_drum_import(uint64_t *regs)
{
    if (!__atomic_load_n(&M.on, __ATOMIC_ACQUIRE)) return 0;
    uintptr_t data = (uintptr_t)regs[1], b = rd(data + DATA_INSTR), e = rd(data + DATA_INSTR + 8);
    M.data = data;
    M.begin = b;
    M.kcount = b && e > b && (e - b) % INSTR_SIZE == 0 ? (int)((e - b) / INSTR_SIZE) : 0;
    M.newp = (uintptr_t)regs[0];
    M.newp_vt = rd(M.newp);
    M.oldp = M.ltc ? rd(M.ltc + LTC_OLDPROG) : 0;
    if (M.oldp == M.newp) M.oldp = 0;
    M.oldp_vt = rd(M.oldp);
    if (M.oldp_vt != M.newp_vt) M.oldp = 0;   /* not a drum program like the new one: nothing to keep from */
    KLOG("drum import: %d kit pads; the project's pads %s", M.kcount, M.oldp ? "found" : "not found (unmapped banks come up empty)");
    return 0;
}

static int on_pad_import(uint64_t *regs)
{
    if (!__atomic_load_n(&M.on, __ATOMIC_ACQUIRE) || !M.data) return 0;
    uintptr_t p = (uintptr_t)regs[1];
    if (p < M.begin || p >= M.begin + (uintptr_t)M.kcount * INSTR_SIZE) return 0;
    int k = kit_pad((int)((p - M.begin) / INSTR_SIZE));
    if (k < 0) return 1;   /* the project's own pad: put back after the load */
    regs[1] = M.begin + (uintptr_t)k * INSTR_SIZE;
    M.pads++;
    return 0;
}

static int on_pad_note(uint64_t *regs)
{
    if (!__atomic_load_n(&M.on, __ATOMIC_ACQUIRE) || !M.data) return 0;
    int k = kit_pad((int)regs[1]);
    if (k < 0) return 1;
    uint8_t v;
    if (safe_read(M.data + DATA_PADBYTES + (uintptr_t)k, &v, 1)) return 1;
    regs[2] = v;
    return 0;
}

int kl_hooks_install(void)
{
    g_base = find_base();
    if (!g_base) { KLOG("MPC Sample binary not found in memory"); return -1; }
    for (size_t i = 0; i < sizeof SIGS / sizeof *SIGS; i++)
        if (memcmp((void *)(g_base + SIGS[i].off), SIGS[i].w, sizeof SIGS[i].w)) {
            KLOG("not firmware 1.4.0 (code at +%#lx differs): off, Load Kit unchanged", (unsigned long)SIGS[i].off);
            return -1;
        }
    struct { uintptr_t slot, expect; void *fn; cmd_fn *orig; } slots[] = {
        { LTC_VT + VT_EXEC,  LTC_EXEC,  (void *)ltc_exec,  &ltc_exec_orig },
        { CSP_VT + VT_EXEC,  CSP_EXEC,  (void *)csp_exec,  &csp_exec_orig },
        { CSP_VT + VT_UNDO,  CSP_UNDO,  (void *)csp_undo,  &csp_undo_orig },
        { LPAK_VT + VT_UNDO, LPAK_UNDO, (void *)lpak_undo, &lpak_undo_orig },
        { LPAK_VT + VT_EXEC, LPAK_EXEC, (void *)lpak_exec, &lpak_exec_orig },   /* last: nothing runs ours before */
    };
    enum { NSLOTS = sizeof slots / sizeof *slots };
    for (int i = 0; i < NSLOTS; i++)
        if (*(uintptr_t *)(g_base + slots[i].slot) != g_base + slots[i].expect) {
            KLOG("vtables differ from 1.4.0: off, Load Kit unchanged");
            return -1;
        }
    pad_export = (pad_xfer_fn)(g_base + PAD_EXPORT);
    pad_import = (pad_xfer_fn)(g_base + PAD_IMPORT);
    pad_note = (pad_note_fn)(g_base + PAD_NOTE);
    for (int i = 0; i < NSLOTS; i++) *slots[i].orig = (cmd_fn)(g_base + slots[i].expect);

    struct hook hk[6];
    const struct { uintptr_t off; hook_cb cb; const char *name; } want[6] = {
        { LPAK_DTOR, on_lpak_dtor, "load kit delete" }, { CSP_DTOR, on_csp_dtor, "clear pool delete" },
        { CSP_DTOR_DEL, on_csp_dtor, "clear pool delete (2)" }, { DRUM_IMPORT, on_drum_import, "drum import" },
        { PAD_IMPORT, on_pad_import, "pad import" }, { PAD_NOTE, on_pad_note, "pad note" },
    };
    int nh = 0, ok = 1, ns = 0;
    for (; nh < 6; nh++) if (hook_prepare(&hk[nh], g_base + want[nh].off, want[nh].cb, want[nh].name)) { ok = 0; break; }
    for (int i = 0; ok && i < nh; i++) if (hook_commit(&hk[i])) ok = 0;
    for (; ok && ns < NSLOTS; ns++)
        if (slot_write(g_base + slots[ns].slot, (uintptr_t)slots[ns].fn)) { KLOG("vtable: mprotect: %s", strerror(errno)); ok = 0; break; }
    if (!ok) {   /* back to MPC's own code, every bit of it */
        while (ns-- > 0) slot_write(g_base + slots[ns].slot, g_base + slots[ns].expect);
        for (int i = 0; i < nh; i++) { hook_remove(&hk[i]); hook_free(&hk[i]); }
        KLOG("hooks not put in: off, Load Kit unchanged");
        return -1;
    }
    KLOG("hooks in (base %p)", (void *)g_base);
    return 0;
}
