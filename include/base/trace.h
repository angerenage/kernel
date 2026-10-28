#pragma once

#include <stddef.h>
#include <stdint.h>

typedef uint64_t trace_label_id_t;

#define TRACE_LABEL_ID_INVALID ((trace_label_id_t)0u)
#define TRACE_LABEL_NAME_MAX 63u

enum trace_event_type {
	TRACE_EVENT_INSTANT = 0u,
	TRACE_EVENT_BEGIN,
	TRACE_EVENT_END,
};
