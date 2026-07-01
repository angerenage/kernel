#include <runtime/diagnostic.h>

#if defined(RUNTIME_DIAGNOSTICS)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum runtime_diagnostic_value_type {
	RUNTIME_DIAGNOSTIC_VALUE_NONE,
	RUNTIME_DIAGNOSTIC_VALUE_SIGNED,
	RUNTIME_DIAGNOSTIC_VALUE_UNSIGNED,
	RUNTIME_DIAGNOSTIC_VALUE_BOOLEAN,
	RUNTIME_DIAGNOSTIC_VALUE_POINTER,
};

struct runtime_diagnostic {
	const char*                        function_name;
	const char*                        message;
	const char*                        value_name;
	enum runtime_diagnostic_value_type value_type;
	union {
		int64_t     signed_value;
		uint64_t    unsigned_value;
		bool        boolean_value;
		const void* pointer_value;
	} value;
};

static bool                      runtime_diagnostic_locked;
static struct runtime_diagnostic runtime_diagnostic_current;

static void runtime_diagnostic_lock(void) {
	while (__atomic_test_and_set(&runtime_diagnostic_locked, __ATOMIC_ACQUIRE)) {
	}
}

static void runtime_diagnostic_unlock(void) {
	__atomic_clear(&runtime_diagnostic_locked, __ATOMIC_RELEASE);
}

static void runtime_diagnostic_store(struct runtime_diagnostic diagnostic) {
	runtime_diagnostic_lock();
	runtime_diagnostic_current = diagnostic;
	runtime_diagnostic_unlock();
}

void runtime_diagnostic_set_at(const char* function_name, const char* message) {
	runtime_diagnostic_store((struct runtime_diagnostic){.function_name = function_name, .message = message});
}

void runtime_diagnostic_set_signed_at(const char* function_name, const char* message, const char* value_name,
                                      int64_t value) {
	runtime_diagnostic_store((struct runtime_diagnostic){
		.function_name      = function_name,
		.message            = message,
		.value_name         = value_name,
		.value_type         = RUNTIME_DIAGNOSTIC_VALUE_SIGNED,
		.value.signed_value = value,
	});
}

void runtime_diagnostic_set_unsigned_at(const char* function_name, const char* message, const char* value_name,
                                        uint64_t value) {
	runtime_diagnostic_store((struct runtime_diagnostic){
		.function_name        = function_name,
		.message              = message,
		.value_name           = value_name,
		.value_type           = RUNTIME_DIAGNOSTIC_VALUE_UNSIGNED,
		.value.unsigned_value = value,
	});
}

void runtime_diagnostic_set_boolean_at(const char* function_name, const char* message, const char* value_name,
                                       bool value) {
	runtime_diagnostic_store((struct runtime_diagnostic){
		.function_name       = function_name,
		.message             = message,
		.value_name          = value_name,
		.value_type          = RUNTIME_DIAGNOSTIC_VALUE_BOOLEAN,
		.value.boolean_value = value,
	});
}

void runtime_diagnostic_set_pointer_at(const char* function_name, const char* message, const char* value_name,
                                       const void* value) {
	runtime_diagnostic_store((struct runtime_diagnostic){
		.function_name       = function_name,
		.message             = message,
		.value_name          = value_name,
		.value_type          = RUNTIME_DIAGNOSTIC_VALUE_POINTER,
		.value.pointer_value = value,
	});
}

char* runtime_diagnostic_get(void) {
	struct runtime_diagnostic diagnostic;
	size_t                    function_name_length;
	size_t                    message_length;
	size_t                    value_name_length;
	size_t                    buffer_size;
	char*                     buffer;

	runtime_diagnostic_lock();
	diagnostic = runtime_diagnostic_current;
	runtime_diagnostic_unlock();

	if (diagnostic.message == NULL) return NULL;
	if (diagnostic.function_name == NULL) diagnostic.function_name = "<unknown>";
	function_name_length = strlen(diagnostic.function_name);
	message_length       = strlen(diagnostic.message);
	if (diagnostic.value_type == RUNTIME_DIAGNOSTIC_VALUE_NONE) {
		if (function_name_length > SIZE_MAX - message_length - 3u) return NULL;
		buffer = malloc(function_name_length + message_length + 3u);
		if (buffer == NULL) return NULL;
		(void)sprintf(buffer, "%s: %s", diagnostic.function_name, diagnostic.message);
		return buffer;
	}

	if (diagnostic.value_name == NULL) diagnostic.value_name = "value";
	value_name_length = strlen(diagnostic.value_name);
	if (value_name_length > SIZE_MAX - 40u || message_length > SIZE_MAX - value_name_length - 40u ||
	    function_name_length > SIZE_MAX - message_length - value_name_length - 40u) {
		return NULL;
	}
	buffer_size = function_name_length + message_length + value_name_length + 40u;
	buffer      = malloc(buffer_size);
	if (buffer == NULL) return NULL;

	switch (diagnostic.value_type) {
	case RUNTIME_DIAGNOSTIC_VALUE_SIGNED:
		(void)sprintf(buffer,
		              "%s: %s: %s = %lld",
		              diagnostic.function_name,
		              diagnostic.message,
		              diagnostic.value_name,
		              (long long)diagnostic.value.signed_value);
		break;
	case RUNTIME_DIAGNOSTIC_VALUE_UNSIGNED:
		(void)sprintf(buffer,
		              "%s: %s: %s = %llu",
		              diagnostic.function_name,
		              diagnostic.message,
		              diagnostic.value_name,
		              (unsigned long long)diagnostic.value.unsigned_value);
		break;
	case RUNTIME_DIAGNOSTIC_VALUE_BOOLEAN:
		(void)sprintf(buffer,
		              "%s: %s: %s = %s",
		              diagnostic.function_name,
		              diagnostic.message,
		              diagnostic.value_name,
		              diagnostic.value.boolean_value ? "true" : "false");
		break;
	case RUNTIME_DIAGNOSTIC_VALUE_POINTER:
		if (diagnostic.value.pointer_value == NULL) {
			(void)sprintf(
				buffer, "%s: %s: %s = NULL", diagnostic.function_name, diagnostic.message, diagnostic.value_name);
		}
		else {
			(void)sprintf(buffer,
			              "%s: %s: %s = %p",
			              diagnostic.function_name,
			              diagnostic.message,
			              diagnostic.value_name,
			              (void*)diagnostic.value.pointer_value);
		}
		break;
	case RUNTIME_DIAGNOSTIC_VALUE_NONE:
		break;
	}
	return buffer;
}

void runtime_diagnostic_clear(void) {
	runtime_diagnostic_store((struct runtime_diagnostic){0});
}

#endif
