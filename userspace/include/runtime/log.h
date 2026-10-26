#pragma once

#include <stdbool.h>
#include <stdio.h>

/* Emit verbose boot diagnostics only when kernel_boot_debug is enabled. */
#if defined(BOOT_DEBUG_LOGGING)
#define RUNTIME_DEBUG_LOG(...) ((void)printf(__VA_ARGS__))
#define RUNTIME_DEBUG_ERROR(...) ((void)fprintf(stderr, __VA_ARGS__))
#else
#define RUNTIME_DEBUG_LOG(...)                                                                                         \
	do {                                                                                                               \
		if (false) (void)printf(__VA_ARGS__);                                                                          \
	} while (0)
#define RUNTIME_DEBUG_ERROR(...)                                                                                       \
	do {                                                                                                               \
		if (false) (void)fprintf(stderr, __VA_ARGS__);                                                                 \
	} while (0)
#endif
