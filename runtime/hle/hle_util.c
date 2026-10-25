#include "hle.h"
#include "ac3_capture.h"
#include <stdarg.h>
#include <stdio.h>

int ac3_hle_trace_enabled = 0;

static void tee(const char *fn, const char *fmt, va_list ap, int paren) {
    char line[160];
    int n = snprintf(line, sizeof line, "%s%s", fn, paren ? "(" : ": ");
    if (n < 0) return;
    if ((size_t)n < sizeof line) {
        int m = vsnprintf(line + n, sizeof line - (size_t)n, fmt, ap);
        if (m > 0 && paren) {
            size_t end = (size_t)n + (size_t)m;
            if (end < sizeof line - 1) { line[end] = ')'; line[end + 1] = 0; }
        }
    }
    ac3_capture_log(line);
}

void hle_warn(const char *fn, const char *fmt, ...) {
    enum { CAP = 512 };
    static const char *seen_fn[CAP];
    static const char *seen_fmt[CAP];
    static int n = 0;
    for (int i = 0; i < n; i++)
        if (seen_fn[i] == fn && seen_fmt[i] == fmt) return;
    if (n < CAP) { seen_fn[n] = fn; seen_fmt[n] = fmt; n++; }

    va_list ap;
    va_start(ap, fmt);
    tee(fn, fmt, ap, 0);
    va_end(ap);
    va_start(ap, fmt);
    fprintf(stderr, "ac3: %s: ", fn);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

void hle_trace(const char *fn, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    tee(fn, fmt, ap, 1);
    va_end(ap);
    if (!ac3_hle_trace_enabled) return;
    va_start(ap, fmt);
    fprintf(stderr, "  [hle] %s(", fn);
    vfprintf(stderr, fmt, ap);
    fputs(")\n", stderr);
    va_end(ap);
}

void hle_warn_always(const char *fn, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    tee(fn, fmt, ap, 0);
    va_end(ap);
    va_start(ap, fmt);
    fprintf(stderr, "ac3: %s: ", fn);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}
