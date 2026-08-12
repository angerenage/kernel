#ifndef TEST_IPC_TEST_SUPPORT_H
#define TEST_IPC_TEST_SUPPORT_H

#include <base/cap.h>
#include <base/channel.h>
#include <base/heap.h>
#include <base/message.h>
#include <base/process.h>
#include <core/capability.h>
#include <core/capability_call.h>
#include <core/channel.h>
#include <core/message.h>
#include <core/pmm.h>
#include <core/ring_buffer.h>
#include <criterion/criterion.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define KiB(x) ((size_t)(x) * 1024u)

void ipc_test_init_heap(void);

#endif
