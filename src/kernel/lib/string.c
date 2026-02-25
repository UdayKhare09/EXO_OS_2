#include "string.h"

/* ── Additional string functions ─────────────────────────────────────────── */
char *strcat(char *dest, const char *src) {
    char *d = dest;
    while (*d) d++;
    while ((*d++ = *src++));
    return dest;
}

char *strncat(char *dest, const char *src, size_t n) {
    char *d = dest;
    while (*d) d++;
    while (n-- && (*d++ = *src++));
    *d = '\0';
    return dest;
}

char *strrchr(const char *s, int c) {
    const char *last = NULL;
    do {
        if (*s == (char)c) last = s;
    } while (*s++);
    return (char *)last;
}

/* ── kvsnprintf — minimal format engine ─────────────────────────────────── */
static void emit(char *buf, size_t size, size_t *pos, char ch) {
    if (*pos + 1 < size) buf[(*pos)++] = ch;
}

static void emit_str(char *buf, size_t size, size_t *pos, const char *s,
                     int width, int left_align) {
    if (!s) s = "(null)";
    int len = (int)strlen(s);
    if (!left_align)
        for (int i = len; i < width; i++) emit(buf, size, pos, ' ');
    while (*s) emit(buf, size, pos, *s++);
    if (left_align)
        for (int i = len; i < width; i++) emit(buf, size, pos, ' ');
}

static void emit_uint(char *buf, size_t size, size_t *pos, uint64_t v,
                      int base, int upper, int width, int zero_pad) {
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[20];
    int  n = 0;
    if (v == 0) { tmp[n++] = '0'; }
    else while (v) { tmp[n++] = digits[v % (uint64_t)base]; v /= (uint64_t)base; }
    char pad = zero_pad ? '0' : ' ';
    for (int i = n; i < width; i++) emit(buf, size, pos, pad);
    for (int i = n - 1; i >= 0; i--) emit(buf, size, pos, tmp[i]);
}

int kvsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
    if (!buf || size == 0) return -1;
    size_t pos = 0;
    for (const char *f = fmt; *f; f++) {
        if (*f != '%') { emit(buf, size, &pos, *f); continue; }
        f++;
        if (!*f) break;

        /* Flags */
        int left_align = 0, zero_pad = 0;
        if (*f == '-') { left_align = 1; f++; }
        if (*f == '0') { zero_pad   = 1; f++; }

        /* Width */
        int width = 0;
        while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');

        /* Long/size modifier */
        int lng = 0;
        if (*f == 'z') { lng = (sizeof(size_t) == 8) ? 2 : 1; f++; }
        else if (*f == 'l') { lng = 1; f++;
            if (*f == 'l') { lng = 2; f++; } }

        switch (*f) {
        case 'c':
            emit(buf, size, &pos, (char)va_arg(ap, int));
            break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            emit_str(buf, size, &pos, s, width, left_align);
            break;
        }
        case 'd': case 'i': {
            int64_t v = (lng == 2) ? va_arg(ap, long long)
                      : (lng == 1) ? (int64_t)va_arg(ap, long)
                      :              (int64_t)va_arg(ap, int);
            if (v < 0) { emit(buf, size, &pos, '-'); v = -v; }
            emit_uint(buf, size, &pos, (uint64_t)v, 10, 0, width, zero_pad);
            break;
        }
        case 'u': {
            uint64_t v = (lng == 2) ? va_arg(ap, unsigned long long)
                       : (lng == 1) ? (uint64_t)va_arg(ap, unsigned long)
                       :              (uint64_t)va_arg(ap, unsigned int);
            emit_uint(buf, size, &pos, v, 10, 0, width, zero_pad);
            break;
        }
        case 'x': {
            uint64_t v = (lng == 2) ? va_arg(ap, unsigned long long)
                       : (lng == 1) ? (uint64_t)va_arg(ap, unsigned long)
                       :              (uint64_t)va_arg(ap, unsigned int);
            emit_uint(buf, size, &pos, v, 16, 0, width, zero_pad);
            break;
        }
        case 'X': {
            uint64_t v = (lng == 2) ? va_arg(ap, unsigned long long)
                       : (lng == 1) ? (uint64_t)va_arg(ap, unsigned long)
                       :              (uint64_t)va_arg(ap, unsigned int);
            emit_uint(buf, size, &pos, v, 16, 1, width, zero_pad);
            break;
        }
        case 'p': {
            uintptr_t v = (uintptr_t)va_arg(ap, void *);
            emit(buf, size, &pos, '0'); emit(buf, size, &pos, 'x');
            emit_uint(buf, size, &pos, (uint64_t)v, 16, 0, 16, 1);
            break;
        }
        case '%':
            emit(buf, size, &pos, '%');
            break;
        default:
            emit(buf, size, &pos, '%');
            emit(buf, size, &pos, *f);
            break;
        }
    }
    buf[pos < size ? pos : size - 1] = '\0';
    return (int)pos;
}

int ksnprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = kvsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return r;
}

void *memset(void *dest, int c, size_t n) {
    uint8_t *p = (uint8_t *)dest;
    while (n--) *p++ = (uint8_t)c;
    return dest;
}

void *memcpy(void *dest, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
    return dest;
}

void *memmove(void *dest, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else {
        d += n; s += n;
        while (n--) *--d = *--s;
    }
    return dest;
}

int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;
    while (n--) {
        if (*pa != *pb) return (int)*pa - (int)*pb;
        pa++; pb++;
    }
    return 0;
}

size_t strlen(const char *s) {
    size_t n = 0;
    while (*s++) n++;
    return n;
}

char *strcpy(char *dest, const char *src) {
    char *d = dest;
    while ((*d++ = *src++));
    return dest;
}

char *strncpy(char *dest, const char *src, size_t n) {
    char *d = dest;
    while (n && (*d++ = *src++)) n--;
    while (n--) *d++ = '\0';
    return dest;
}

int strcmp(const char *a, const char *b) {
    while (*a && (*a == *b)) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    while (n && *a && (*a == *b)) { a++; b++; n--; }
    if (!n) return 0;
    return (unsigned char)*a - (unsigned char)*b;
}

char *strchr(const char *s, int c) {
    while (*s) {
        if (*s == (char)c) return (char *)s;
        s++;
    }
    return (c == '\0') ? (char *)s : NULL;
}
