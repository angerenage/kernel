#pragma once

#include <stdarg.h>
#include <stddef.h>

#define EOF (-1)

typedef struct libc_file FILE;

extern FILE* stdin;
extern FILE* stdout;
extern FILE* stderr;

FILE* fopen(const char* restrict path, const char* restrict mode);
int   fclose(FILE* stream);

size_t fread(void* restrict buffer, size_t size, size_t count, FILE* restrict stream);
size_t fwrite(const void* restrict buffer, size_t size, size_t count, FILE* restrict stream);

int  feof(FILE* stream);
int  ferror(FILE* stream);
void clearerr(FILE* stream);
int  fflush(FILE* stream);

int putchar(int);
int puts(const char*);
int printf(const char* restrict format, ...);
int sprintf(char* restrict buffer, const char* restrict format, ...);
