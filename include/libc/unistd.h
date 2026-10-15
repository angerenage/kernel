#pragma once

#include <stddef.h>

typedef __PTRDIFF_TYPE__ ssize_t;

ssize_t read(int descriptor, void* buffer, size_t count);
ssize_t write(int descriptor, const void* buffer, size_t count);
