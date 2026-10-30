#include <string.h>

void *memset(void *dst, int c, usize n)
{
    u8 *d = dst;
    while (n--) *d++ = (u8)c;
    return dst;
}

void *memcpy(void *dst, const void *src, usize n)
{
    u8 *d = dst;
    const u8 *s = src;
    while (n--) *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, usize n)
{
    u8 *d = dst;
    const u8 *s = src;
    if (d == s || n == 0) return dst;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else {
        d += n;
        s += n;
        while (n--) *--d = *--s;
    }
    return dst;
}

int memcmp(const void *a, const void *b, usize n)
{
    const u8 *x = a, *y = b;
    while (n--) {
        if (*x != *y) return (int)*x - (int)*y;
        x++;
        y++;
    }
    return 0;
}

usize strlen(const char *s)
{
    const char *p = s;
    while (*p) p++;
    return (usize)(p - s);
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (int)(u8)*a - (int)(u8)*b;
}

int strncmp(const char *a, const char *b, usize n)
{
    while (n && *a && *a == *b) { a++; b++; n--; }
    if (!n) return 0;
    return (int)(u8)*a - (int)(u8)*b;
}

static int lower(int c)
{
    if (c >= 'A' && c <= 'Z') return c + 32;
    return c;
}

int strcasecmp(const char *a, const char *b)
{
    while (*a && lower(*a) == lower(*b)) { a++; b++; }
    return lower((u8)*a) - lower((u8)*b);
}

int strncasecmp(const char *a, const char *b, usize n)
{
    while (n && *a && lower(*a) == lower(*b)) { a++; b++; n--; }
    if (n == 0) return 0;
    return lower((u8)*a) - lower((u8)*b);
}

char *strcpy(char *dst, const char *src)
{
    char *d = dst;
    while ((*d++ = *src++)) { }
    return dst;
}

char *strncpy(char *dst, const char *src, usize n)
{
    usize i = 0;
    for (; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = '\0';
    return dst;
}

char *strcat(char *dst, const char *src)
{
    char *d = dst + strlen(dst);
    while ((*d++ = *src++)) { }
    return dst;
}

usize strlcpy(char *dst, const char *src, usize n)
{
    usize len = strlen(src);
    if (n) {
        usize c = len < n - 1 ? len : n - 1;
        memcpy(dst, src, c);
        dst[c] = '\0';
    }
    return len;
}

char *strchr(const char *s, int c)
{
    for (; *s; s++)
        if (*s == (char)c) return (char *)s;
    return (c == 0) ? (char *)s : NULL;
}

int str_starts_with(const char *s, const char *prefix)
{
    while (*prefix)
        if (*s++ != *prefix++) return 0;
    return 1;
}

int str_ends_with(const char *s, const char *suffix)
{
    usize ls = strlen(s), lf = strlen(suffix);
    if (lf > ls) return 0;
    return memcmp(s + ls - lf, suffix, lf) == 0;
}

void str_trim(char *s)
{
    char *p = s;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    usize len = strlen(s);
    while (len && (s[len - 1] == ' ' || s[len - 1] == '\t' ||
                   s[len - 1] == '\n' || s[len - 1] == '\r'))
        s[--len] = '\0';
}

int atoi(const char *s)
{
    int sign = 1, val = 0;
    if (*s == '-') { sign = -1; s++; }
    while (*s >= '0' && *s <= '9') val = val * 10 + (*s++ - '0');
    return sign * val;
}

static const char digits_lower[] = "0123456789abcdef";
static const char digits_upper[] = "0123456789ABCDEF";

static char *num_to_str(u64 value, char *buf, int base, int upper, int neg)
{
    char tmp[68];
    int i = 0;
    const char *digits = upper ? digits_upper : digits_lower;
    if (value == 0) tmp[i++] = '0';
    while (value) {
        tmp[i++] = digits[value % (u64)base];
        value /= (u64)base;
    }
    if (neg) tmp[i++] = '-';
    int j = 0;
    while (i) buf[j++] = tmp[--i];
    buf[j] = '\0';
    return buf;
}

char *utoa(u64 value, char *buf, int base)
{
    return num_to_str(value, buf, base, 0, 0);
}

char *itoa(i64 value, char *buf, int base)
{
    if (value < 0) return num_to_str((u64)(-value), buf, base, 0, 1);
    return num_to_str((u64)value, buf, base, 0, 0);
}

/* Minimal printf supporting %s %c %d %u %x %X %p %ll* and width/zero pad. */
int vsnprintf(char *buf, usize n, const char *fmt, void *args)
{
    __builtin_va_list ap;
    __builtin_va_copy(ap, *(__builtin_va_list *)args);

    usize written = 0;
#define PUT(ch) do { if (written + 1 < n) buf[written] = (ch); written++; } while (0)

    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { PUT(*p); continue; }

        p++;
        int zero = 0, left = 0, width = 0, is_long = 0;
        while (*p == '0' || *p == '-' || *p == '+' || *p == ' ') {
            if (*p == '0') zero = 1;
            if (*p == '-') left = 1;
            p++;
        }
        while (*p >= '0' && *p <= '9') width = width * 10 + (*p++ - '0');
        int precision = -1;
        if (*p == '.') {
            p++;
            precision = 0;
            while (*p >= '0' && *p <= '9') precision = precision * 10 + (*p++ - '0');
        }
        while (*p == 'l') { is_long++; p++; }

        char tmp[80];
        const char *s = tmp;
        int neg = 0;

        switch (*p) {
        case 's': {
            s = __builtin_va_arg(ap, const char *);
            if (!s) s = "(null)";
            break;
        }
        case 'c': {
            tmp[0] = (char)__builtin_va_arg(ap, int);
            tmp[1] = '\0';
            break;
        }
        case 'd': case 'i': {
            i64 v = is_long ? __builtin_va_arg(ap, i64) : __builtin_va_arg(ap, int);
            if (v < 0) { neg = 1; v = -v; }
            num_to_str((u64)v, tmp, 10, 0, 0);
            if (neg) {
                memmove(tmp + 1, tmp, strlen(tmp) + 1);
                tmp[0] = '-';
            }
            break;
        }
        case 'u': {
            u64 v = is_long ? __builtin_va_arg(ap, u64) : __builtin_va_arg(ap, u32);
            num_to_str(v, tmp, 10, 0, 0);
            break;
        }
        case 'x': {
            u64 v = is_long ? __builtin_va_arg(ap, u64) : __builtin_va_arg(ap, u32);
            num_to_str(v, tmp, 16, 0, 0);
            break;
        }
        case 'X': {
            u64 v = is_long ? __builtin_va_arg(ap, u64) : __builtin_va_arg(ap, u32);
            num_to_str(v, tmp, 16, 1, 0);
            break;
        }
        case 'p': {
            u64 v = (u64)(usize)__builtin_va_arg(ap, void *);
            tmp[0] = '0'; tmp[1] = 'x';
            num_to_str(v, tmp + 2, 16, 0, 0);
            break;
        }
        case '%': {
            tmp[0] = '%'; tmp[1] = '\0';
            break;
        }
        default: {
            tmp[0] = '%'; tmp[1] = *p; tmp[2] = '\0';
            break;
        }
        }

        usize len = strlen(s);
        if (precision >= 0 && (usize)precision < len) len = (usize)precision;
        int pad = width > (int)len ? width - (int)len : 0;
        if (!left)
            while (pad--) PUT(zero ? '0' : ' ');
        for (usize i = 0; i < len; i++) PUT(s[i]);
        if (left)
            while (pad--) PUT(' ');
    }
#undef PUT
    if (n) buf[written < n ? written : n - 1] = '\0';
    __builtin_va_end(ap);
    return (int)written;
}

int snprintf(char *buf, usize n, const char *fmt, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    int r = vsnprintf(buf, n, fmt, &ap);
    __builtin_va_end(ap);
    return r;
}
