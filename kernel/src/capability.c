#include <base/cap.h>
#include <core/capability.h>
#include <kernel/capability.h>
#include <stddef.h>
#include <stdint.h>

cap_id_t cap_kernel_create(uint64_t object_id, cap_kernel_handler_t handler, process_id_t target, cap_rights_t rights) {
	struct cap_object* object;
	struct capability* cap;

	object = cap_object_create_kernel(object_id, handler);
	if (object == NULL) return CAP_ID_INVALID;

	cap = cap_create(object, target, rights, NULL);
	if (cap == NULL) {
		cap_object_destroy(object);
		return CAP_ID_INVALID;
	}

	return cap->cap_id;
}

void kernel_capability_init(void) {
	// Creates basic kernel objects and capabilities.
}
