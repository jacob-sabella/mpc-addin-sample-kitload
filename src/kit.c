/* What a kit project holds: pads with a sample, per bank, in its first program's instruments (the drum track Load Kit
 * takes, for every kit this addin's converter writes). The .xpj is gzip (libz is dlopened) of a short text header and
 * JSON; this walks the JSON just far enough. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>

#include "kitload.h"

typedef void *gzFile_t;
static gzFile_t (*z_open)(const char *, const char *);
static int (*z_read)(gzFile_t, void *, unsigned);
static int (*z_close)(gzFile_t);

static int zlib(void)
{
    if (z_open) return 0;
    void *h = dlopen("libz.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!h) return -1;
    z_read = (int (*)(gzFile_t, void *, unsigned))dlsym(h, "gzread");
    z_close = (int (*)(gzFile_t))dlsym(h, "gzclose");
    gzFile_t (*o)(const char *, const char *) = (gzFile_t (*)(const char *, const char *))dlsym(h, "gzopen");
    if (!o || !z_read || !z_close) return -1;
    z_open = o;
    return 0;
}

static char *slurp(const char *path, size_t *len)
{
    if (zlib()) return NULL;
    gzFile_t f = z_open(path, "rb");
    if (!f) return NULL;
    size_t cap = 1 << 18, n = 0;
    char *b = malloc(cap + 1);
    for (;;) {
        if (!b) break;
        if (n == cap) { char *nb = realloc(b, cap * 2 + 1); if (!nb) { free(b); b = NULL; break; } b = nb; cap *= 2; }
        int r = z_read(f, b + n, (unsigned)(cap - n));
        if (r <= 0) break;
        n += (size_t)r;
        if (n > (64u << 20)) break;
    }
    z_close(f);
    if (b) b[n] = 0;
    *len = n;
    return b;
}

int kl_kit_scan(const char *path, int used[NBANKS])
{
    for (int b = 0; b < NBANKS; b++) used[b] = 0;
    size_t n;
    char *s = slurp(path, &n);
    if (!s) return -1;
    /* "instruments" : [  (any spacing) */
    const char *p = s;
    for (;;) {
        p = strstr(p, "\"instruments\"");
        if (!p) { free(s); return -1; }
        p += strlen("\"instruments\"");
        while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++;
        if (*p != ':') continue;
        p++;
        while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++;
        if (*p == '[') break;
    }
    int depth = 0, pad = -1, has = 0;
    char key[32] = "";
    const char *end = s + n;
    for (; p < end; p++) {
        char c = *p;
        if (c == '"') {
            const char *q = ++p;
            while (p < end && *p != '"') { if (*p == '\\') p++; p++; }
            size_t l = (size_t)(p - q);
            const char *r = p + 1;
            while (r < end && (*r == ' ' || *r == '\n' || *r == '\r' || *r == '\t')) r++;
            if (r < end && *r == ':') { size_t k = l < sizeof key - 1 ? l : sizeof key - 1; memcpy(key, q, k); key[k] = 0; }
            else if (l && !has && !strcmp(key, "sampleName") && pad >= 0 && pad < NBANKS * 16) { has = 1; used[pad / 16]++; }
            continue;
        }
        if (c == '{' || c == '[') {
            if (c == '{' && depth == 1) { pad++; has = 0; }
            depth++;
        } else if ((c == '}' || c == ']') && --depth == 0) break;
    }
    free(s);
    return pad + 1;
}
