#pragma once

#include <stdint.h>

int main(int argc, char** argv);

__attribute__((noreturn))
void exit(uintptr_t code);
