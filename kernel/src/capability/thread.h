#pragma once

#include <base/cap.h>
#include <core/uthread.h>

/* Grant recipient a capability for target with the specified rights. */
cap_id_t kernel_thread_grant(struct uthread* target, process_id_t recipient, cap_rights_t rights);

/* Grant full thread-management rights to recipient. */
cap_id_t kernel_thread_grant_full(struct uthread* target, process_id_t recipient);
