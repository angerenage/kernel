#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * Debug-only, process-local diagnostic strings for userspace APIs.
 *
 * Message and value-name strings are not copied and must remain valid until
 * the diagnostic is replaced or cleared. The provided macros use string
 * literals for both. Successful calls do not clear an earlier diagnostic.
 */
#ifdef RUNTIME_DIAGNOSTICS

/* Register a diagnostic with its userspace API call site, or clear it when message is null. */
void runtime_diagnostic_set_at(const char* function_name, const char* message);

void runtime_diagnostic_set_signed_at(const char* function_name, const char* message, const char* value_name,
                                      int64_t value);
void runtime_diagnostic_set_unsigned_at(const char* function_name, const char* message, const char* value_name,
                                        uint64_t value);
void runtime_diagnostic_set_boolean_at(const char* function_name, const char* message, const char* value_name,
                                       bool value);
void runtime_diagnostic_set_pointer_at(const char* function_name, const char* message, const char* value_name,
                                       const void* value);

/*
 * Allocate and format the current diagnostic. The caller owns the returned string and must free it.
 * Return null when there is no diagnostic or when allocation fails.
 */
char* runtime_diagnostic_get(void);

/* Clear the currently registered diagnostic. */
void runtime_diagnostic_clear(void);

#define runtime_diagnostic_set(message) runtime_diagnostic_set_at(__func__, message)
#define RUNTIME_DIAGNOSTIC_SET(message) runtime_diagnostic_set(message)
#define RUNTIME_DIAGNOSTIC_NAMED_VALUE(message, value_name, value)                                                     \
	_Generic((value),                                                                                                  \
	    bool: runtime_diagnostic_set_boolean_at,                                                                       \
	    char: runtime_diagnostic_set_signed_at,                                                                        \
	    signed char: runtime_diagnostic_set_signed_at,                                                                 \
	    unsigned char: runtime_diagnostic_set_unsigned_at,                                                             \
	    short: runtime_diagnostic_set_signed_at,                                                                       \
	    unsigned short: runtime_diagnostic_set_unsigned_at,                                                            \
	    int: runtime_diagnostic_set_signed_at,                                                                         \
	    unsigned int: runtime_diagnostic_set_unsigned_at,                                                              \
	    long: runtime_diagnostic_set_signed_at,                                                                        \
	    unsigned long: runtime_diagnostic_set_unsigned_at,                                                             \
	    long long: runtime_diagnostic_set_signed_at,                                                                   \
	    unsigned long long: runtime_diagnostic_set_unsigned_at,                                                        \
	    default: runtime_diagnostic_set_pointer_at)(__func__, (message), (value_name), (value))

#define RUNTIME_DIAGNOSTIC_VALUE(message, value) RUNTIME_DIAGNOSTIC_NAMED_VALUE(message, #value, value)

#define RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(parameter) RUNTIME_DIAGNOSTIC_VALUE("invalid parameter", parameter)
#define RUNTIME_DIAGNOSTIC_INVALID_PARAMETER_INDEX(operation, index)                                                   \
	RUNTIME_DIAGNOSTIC_VALUE("invalid parameter index reported by " #operation, index)
#define RUNTIME_DIAGNOSTIC_INVALID_STATE(message) runtime_diagnostic_set("invalid state: " message)
#define RUNTIME_DIAGNOSTIC_DENIED(operation) runtime_diagnostic_set(#operation " denied")
#define RUNTIME_DIAGNOSTIC_UNAVAILABLE(operation) runtime_diagnostic_set(#operation " unavailable")

#define RUNTIME_DIAGNOSTIC_PRIVATE_FAILED(operation) runtime_diagnostic_set(#operation " failed")
#define RUNTIME_DIAGNOSTIC_PRIVATE_FAILED_VALUE(operation, value) RUNTIME_DIAGNOSTIC_VALUE(#operation " failed", value)
#define RUNTIME_DIAGNOSTIC_PRIVATE_SELECT_FAILED(_1, _2, selected, ...) selected
#define RUNTIME_DIAGNOSTIC_FAILED(...)                                                                                 \
	RUNTIME_DIAGNOSTIC_PRIVATE_SELECT_FAILED(                                                                          \
		__VA_ARGS__, RUNTIME_DIAGNOSTIC_PRIVATE_FAILED_VALUE, RUNTIME_DIAGNOSTIC_PRIVATE_FAILED)(__VA_ARGS__)

#define RUNTIME_DIAGNOSTIC_SYSCALL_RESULT(operation, result)                                                           \
	do {                                                                                                               \
		switch ((result).status) {                                                                                     \
		case SYSCALL_STATUS_OK:                                                                                        \
			break;                                                                                                     \
		case SYSCALL_STATUS_UNKNOWN_SYSCALL:                                                                           \
			RUNTIME_DIAGNOSTIC_NAMED_VALUE(                                                                            \
				#operation " reported an unknown syscall", "syscall_number", (result).value);                          \
			break;                                                                                                     \
		case SYSCALL_STATUS_BAD_ARGUMENT:                                                                              \
			RUNTIME_DIAGNOSTIC_NAMED_VALUE(                                                                            \
				"invalid parameter index reported by " #operation, "argument_index", (result).value);                  \
			break;                                                                                                     \
		case SYSCALL_STATUS_DENIED:                                                                                    \
			RUNTIME_DIAGNOSTIC_DENIED(operation);                                                                      \
			break;                                                                                                     \
		case SYSCALL_STATUS_FAILED:                                                                                    \
			if ((result).value == 0u) {                                                                                \
				RUNTIME_DIAGNOSTIC_FAILED(operation);                                                                  \
			}                                                                                                          \
			else {                                                                                                     \
				RUNTIME_DIAGNOSTIC_NAMED_VALUE(#operation " failed", "detail", (result).value);                        \
			}                                                                                                          \
			break;                                                                                                     \
		case SYSCALL_STATUS_UNAVAILABLE:                                                                               \
			RUNTIME_DIAGNOSTIC_UNAVAILABLE(operation);                                                                 \
			break;                                                                                                     \
		default:                                                                                                       \
			RUNTIME_DIAGNOSTIC_NAMED_VALUE(#operation " failed", "status", (result).status);                           \
			break;                                                                                                     \
		}                                                                                                              \
	} while (0)

#define RUNTIME_DIAGNOSTIC_OPERATION_RESULT(operation, result)                                                         \
	do {                                                                                                               \
		if ((result).status == SYSCALL_STATUS_BAD_ARGUMENT) {                                                          \
			RUNTIME_DIAGNOSTIC_NAMED_VALUE(#operation " rejected request", "detail", (result).value);                  \
		}                                                                                                              \
		else {                                                                                                         \
			RUNTIME_DIAGNOSTIC_SYSCALL_RESULT(operation, result);                                                      \
		}                                                                                                              \
	} while (0)

#else

#define RUNTIME_DIAGNOSTIC_SET(message) ((void)0)
#define RUNTIME_DIAGNOSTIC_NAMED_VALUE(message, value_name, value) ((void)0)
#define RUNTIME_DIAGNOSTIC_VALUE(message, value) ((void)0)
#define RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(parameter) ((void)0)
#define RUNTIME_DIAGNOSTIC_INVALID_PARAMETER_INDEX(operation, index) ((void)0)
#define RUNTIME_DIAGNOSTIC_INVALID_STATE(message) ((void)0)
#define RUNTIME_DIAGNOSTIC_DENIED(operation) ((void)0)
#define RUNTIME_DIAGNOSTIC_UNAVAILABLE(operation) ((void)0)
#define RUNTIME_DIAGNOSTIC_FAILED(...) ((void)0)
#define RUNTIME_DIAGNOSTIC_SYSCALL_RESULT(operation, result) ((void)0)
#define RUNTIME_DIAGNOSTIC_OPERATION_RESULT(operation, result) ((void)0)
#define runtime_diagnostic_set(message) ((void)0)
#define runtime_diagnostic_set_at(function_name, message) ((void)0)
#define runtime_diagnostic_set_signed_at(function_name, message, value_name, value) ((void)0)
#define runtime_diagnostic_set_unsigned_at(function_name, message, value_name, value) ((void)0)
#define runtime_diagnostic_set_boolean_at(function_name, message, value_name, value) ((void)0)
#define runtime_diagnostic_set_pointer_at(function_name, message, value_name, value) ((void)0)
#define runtime_diagnostic_get() ((char*)0)
#define runtime_diagnostic_clear() ((void)0)

#endif
