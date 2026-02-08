#pragma once

#include <stdarg.h>

#define EOF (-1)

void putchar(char);
void puts(const char*);
void printf(const char* restrict format, ...);
void sprintf(char* buffer, const char* restrict format, ...);
