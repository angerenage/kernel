#pragma once

#include <stdarg.h>

#define EOF (-1)

int putchar(int);
int puts(const char*);
int printf(const char* restrict format, ...);
int sprintf(char* restrict buffer, const char* restrict format, ...);
