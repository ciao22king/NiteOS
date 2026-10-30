#ifndef NEO_STRING_H
#define NEO_STRING_H

#include <nitekernel.h>

void *memset(void *dst, int c, usize n);
void *memcpy(void *dst, const void *src, usize n);
void *memmove(void *dst, const void *src, usize n);
int   memcmp(const void *a, const void *b, usize n);

usize strlen(const char *s);
int   strcmp(const char *a, const char *b);
int   strncmp(const char *a, const char *b, usize n);
int   strcasecmp(const char *a, const char *b);
int   strncasecmp(const char *a, const char *b, usize n);
char *strcpy(char *dst, const char *src);
char *strncpy(char *dst, const char *src, usize n);
char *strcat(char *dst, const char *src);
char *strchr(const char *s, int c);
int   atoi(const char *s);

/* Non-standard helpers used across the kernel. */
int   str_starts_with(const char *s, const char *prefix);
int   str_ends_with(const char *s, const char *suffix);
void  str_trim(char *s);
usize strlcpy(char *dst, const char *src, usize n);
char *utoa(u64 value, char *buf, int base);
char *itoa(i64 value, char *buf, int base);
int   snprintf(char *buf, usize n, const char *fmt, ...);
int   vsnprintf(char *buf, usize n, const char *fmt, void *args);

#endif
