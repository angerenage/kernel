#pragma once

#include <base/syscall.h>
#include <base/trace.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct trace_label {
	const char*      name;
	size_t           length;
	trace_label_id_t id;
};

struct trace_scope {
	struct trace_label* label;
	bool                active;
};

#define TRACE_LABEL_INITIALIZER(label_name)                                                                            \
	{                                                                                                                  \
		.name   = (label_name),                                                                                        \
		.length = sizeof(label_name) - 1u,                                                                             \
		.id     = TRACE_LABEL_ID_INVALID,                                                                              \
	}

#define TRACE_LABEL(variable, label_name) static struct trace_label variable = TRACE_LABEL_INITIALIZER(label_name)

#define TRACE_SCOPE(variable, label)                                                                                   \
	struct trace_scope variable __attribute__((cleanup(trace_scope_close))) = trace_scope_open(label)

/* Emit a duration slice on the calling userspace thread. */
syscall_status_t trace_begin(struct trace_label* label);

/* Mark the end of a duration slice on the calling userspace thread. */
syscall_status_t trace_end(struct trace_label* label);

/* Emit a timestamped marker on the calling userspace thread. */
syscall_status_t trace_instant(struct trace_label* label);

/* Scope helper used by TRACE_SCOPE, including early-return paths. */
struct trace_scope trace_scope_open(struct trace_label* label);

/* Close a trace scope and emit the end of the duration slice. */
void trace_scope_close(struct trace_scope* scope);
