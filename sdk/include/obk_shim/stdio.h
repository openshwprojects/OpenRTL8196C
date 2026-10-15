#ifndef _SHIM_STDIO_H
#define _SHIM_STDIO_H

#include <stddef.h>
#include <stdarg.h>

typedef struct _FILE FILE;
#define EOF (-1)
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

int printf(const char *fmt, ...);
int vprintf(const char *fmt, va_list ap);
int snprintf(char *buf, unsigned size, const char *fmt, ...);
int vsnprintf(char *buf, unsigned size, const char *fmt, va_list ap);
int sprintf(char *buf, const char *fmt, ...);
int vsprintf(char *buf, const char *fmt, va_list ap);
int sscanf(const char *str, const char *fmt, ...);
int vsscanf(const char *str, const char *fmt, va_list ap);
int putchar(int c);
int puts(const char *s);

#endif
