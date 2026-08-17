#include "registry.h"

#include <libc/stdlib.h>
#include <libc/string.h>
#include <limits.h>
#include <system/capability.h>
#include <system/signal.h>

struct registry_advertisement {
	struct init_service_selector   selector;
	uint32_t                       minor;
	process_id_t                   owner;
	cap_id_t                       capability;
	cap_rights_t                   client_rights;
	struct registry_advertisement* next;
};

struct registry_watch {
	uint64_t                   id;
	process_id_t               owner;
	struct init_protocol_query query;
	cap_id_t                   signal_capability;
	uint64_t                   counter;
	struct registry_watch*     next;
};

static struct registry_advertisement* advertisements;
static struct registry_watch*         watches;
static uint64_t                       next_watch_id = 1u;

static bool bounded_terminated(const char* value, size_t capacity, size_t* out_length) {
	if (value == NULL) return false;
	for (size_t i = 0u; i < capacity; i++) {
		if (value[i] != '\0') continue;
		if (out_length != NULL) *out_length = i;
		return true;
	}
	return false;
}

static bool name_character_valid(char value) {
	return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') || (value >= '0' && value <= '9') ||
	       value == '_' || value == '-' || value == '.';
}

static bool string_has_prefix(const char* value, const char* prefix, size_t prefix_length) {
	for (size_t i = 0u; i < prefix_length; i++) {
		if (value[i] != prefix[i]) return false;
	}
	return true;
}

bool registry_name_valid(const char* name) {
	size_t length;

	if (!bounded_terminated(name, INIT_NAME_MAX + 1u, &length) || length == 0u) return false;
	if ((length == 1u && name[0] == '.') || (length == 2u && name[0] == '.' && name[1] == '.')) return false;
	for (size_t i = 0u; i < length; i++) {
		if (!name_character_valid(name[i])) return false;
	}
	return true;
}

bool registry_namespace_valid(const char* namespace_path, bool allow_root) {
	size_t length;
	size_t component_start = 0u;

	if (!bounded_terminated(namespace_path, INIT_NAMESPACE_PATH_MAX + 1u, &length)) return false;
	if (length == 0u) return allow_root;
	for (size_t i = 0u; i <= length; i++) {
		if (i != length && namespace_path[i] != '/') {
			if (!name_character_valid(namespace_path[i])) return false;
			continue;
		}
		size_t component_length = i - component_start;
		if (component_length == 0u || component_length > INIT_NAME_MAX) return false;
		if ((component_length == 1u && namespace_path[component_start] == '.') ||
		    (component_length == 2u && namespace_path[component_start] == '.' &&
		     namespace_path[component_start + 1u] == '.'))
			return false;
		component_start = i + 1u;
	}
	return true;
}

bool registry_query_valid(const struct init_protocol_query* query) {
	return query != NULL && registry_namespace_valid(query->namespace_path, false) &&
	       registry_name_valid(query->protocol);
}

bool registry_selector_valid(const struct init_service_selector* selector) {
	return selector != NULL && registry_namespace_valid(selector->namespace_path, false) &&
	       registry_name_valid(selector->protocol) && registry_name_valid(selector->service);
}

static int selector_compare(const struct init_service_selector* left, const struct init_service_selector* right) {
	int comparison = strcmp(left->namespace_path, right->namespace_path);
	if (comparison != 0) return comparison;
	comparison = strcmp(left->protocol, right->protocol);
	if (comparison != 0) return comparison;
	if (left->major < right->major) return -1;
	if (left->major > right->major) return 1;
	return strcmp(left->service, right->service);
}

static bool query_matches(const struct init_protocol_query* query, const struct registry_advertisement* ad) {
	return strcmp(query->namespace_path, ad->selector.namespace_path) == 0 &&
	       strcmp(query->protocol, ad->selector.protocol) == 0 && query->major == ad->selector.major &&
	       ad->minor >= query->minor;
}

static bool advertisement_same(const struct registry_advertisement* ad, uint32_t minor, cap_id_t capability,
                               cap_rights_t rights) {
	return ad->minor == minor && ad->capability == capability && ad->client_rights == rights;
}

static void notify_watches(const struct registry_advertisement* old_ad, const struct registry_advertisement* new_ad) {
	struct registry_watch** cursor = &watches;

	while (*cursor != NULL) {
		struct registry_watch* watch   = *cursor;
		bool                   valid   = false;
		bool                   matches = (old_ad != NULL && query_matches(&watch->query, old_ad)) ||
		               (new_ad != NULL && query_matches(&watch->query, new_ad));

		if (cap_valid(watch->signal_capability, &valid) != SYSCALL_STATUS_OK || !valid) {
			*cursor = watch->next;
			free(watch);
			continue;
		}
		if (matches) {
			struct signal_send_response response;
			watch->counter++;
			if (watch->counter == 0u) watch->counter = 1u;
			(void)signal_send(
				watch->signal_capability, watch->counter, 0u, 0u, 0u, SIGNAL_SEND_FLAG_COALESCE, &response);
		}
		cursor = &watch->next;
	}
}

static void prune_dead_advertisements(void) {
	struct registry_advertisement** cursor = &advertisements;

	while (*cursor != NULL) {
		struct registry_advertisement* ad    = *cursor;
		bool                           valid = false;
		if (cap_valid(ad->capability, &valid) == SYSCALL_STATUS_OK && valid) {
			cursor = &ad->next;
			continue;
		}
		*cursor = ad->next;
		notify_watches(ad, NULL);
		free(ad);
	}
}

enum init_registry_status registry_advertise(process_id_t owner, const struct init_service_selector* selector,
                                             uint32_t minor, cap_id_t capability, cap_rights_t client_rights) {
	struct registry_advertisement** cursor;
	struct registry_advertisement*  replacement;
	bool                            valid = false;

	if (owner == PROCESS_PID_INVALID || !registry_selector_valid(selector) || capability == CAP_ID_INVALID ||
	    client_rights == 0u)
		return INIT_REGISTRY_INVALID_ARGUMENT;
	if (cap_valid(capability, &valid) != SYSCALL_STATUS_OK || !valid) return INIT_REGISTRY_INVALID_ARGUMENT;
	prune_dead_advertisements();
	cursor = &advertisements;
	while (*cursor != NULL && selector_compare(&(*cursor)->selector, selector) < 0) cursor = &(*cursor)->next;
	if (*cursor != NULL && selector_compare(&(*cursor)->selector, selector) == 0) {
		struct registry_advertisement* current = *cursor;
		if (current->owner != owner) return INIT_REGISTRY_CONFLICT;
		if (advertisement_same(current, minor, capability, client_rights)) return INIT_REGISTRY_OK;
		replacement = malloc(sizeof(*replacement));
		if (replacement == NULL) return INIT_REGISTRY_NO_MEMORY;
		*replacement = (struct registry_advertisement){
			.selector      = *selector,
			.minor         = minor,
			.owner         = owner,
			.capability    = capability,
			.client_rights = client_rights,
			.next          = current->next,
		};
		*cursor = replacement;
		notify_watches(current, replacement);
		free(current);
		return INIT_REGISTRY_OK;
	}
	replacement = malloc(sizeof(*replacement));
	if (replacement == NULL) return INIT_REGISTRY_NO_MEMORY;
	*replacement = (struct registry_advertisement){
		.selector      = *selector,
		.minor         = minor,
		.owner         = owner,
		.capability    = capability,
		.client_rights = client_rights,
		.next          = *cursor,
	};
	*cursor = replacement;
	notify_watches(NULL, replacement);
	return INIT_REGISTRY_OK;
}

enum init_registry_status registry_withdraw(process_id_t owner, const struct init_service_selector* selector) {
	struct registry_advertisement** cursor;
	if (owner == PROCESS_PID_INVALID || !registry_selector_valid(selector)) return INIT_REGISTRY_INVALID_ARGUMENT;
	prune_dead_advertisements();
	cursor = &advertisements;
	while (*cursor != NULL && selector_compare(&(*cursor)->selector, selector) < 0) cursor = &(*cursor)->next;
	if (*cursor == NULL || selector_compare(&(*cursor)->selector, selector) != 0) return INIT_REGISTRY_NOT_FOUND;
	if ((*cursor)->owner != owner) return INIT_REGISTRY_DENIED;
	struct registry_advertisement* removed = *cursor;
	*cursor                                = removed->next;
	notify_watches(removed, NULL);
	free(removed);
	return INIT_REGISTRY_OK;
}

static void fill_service_entry(const struct registry_advertisement* ad, cap_id_t capability,
                               struct init_service_entry* entry) {
	*entry = (struct init_service_entry){
		.selector   = ad->selector,
		.minor      = ad->minor,
		.capability = capability,
		.rights     = ad->client_rights,
	};
}

enum init_registry_status registry_lookup(process_id_t caller, const struct init_protocol_query* query,
                                          const char* service, struct init_service_entry* out_entry) {
	cap_id_t delegated;
	if (caller == PROCESS_PID_INVALID || !registry_query_valid(query) || !registry_name_valid(service) ||
	    out_entry == NULL)
		return INIT_REGISTRY_INVALID_ARGUMENT;
	prune_dead_advertisements();
	for (struct registry_advertisement* ad = advertisements; ad != NULL; ad = ad->next) {
		if (!query_matches(query, ad) || strcmp(service, ad->selector.service) != 0) continue;
		if (cap_delegate(ad->capability, caller, ad->client_rights, &delegated) != SYSCALL_STATUS_OK)
			return INIT_REGISTRY_RESOURCE_FAILURE;
		fill_service_entry(ad, delegated, out_entry);
		return INIT_REGISTRY_OK;
	}
	return INIT_REGISTRY_NOT_FOUND;
}

enum init_registry_status registry_enumerate(process_id_t caller, const struct init_protocol_query* query,
                                             uint64_t offset, uint64_t size, struct init_service_entry* entries,
                                             uint64_t* out_returned, uint64_t* out_total) {
	uint64_t total    = 0u;
	uint64_t returned = 0u;
	if (caller == PROCESS_PID_INVALID || !registry_query_valid(query) || (size != 0u && entries == NULL) ||
	    out_returned == NULL || out_total == NULL)
		return INIT_REGISTRY_INVALID_ARGUMENT;
	prune_dead_advertisements();
	for (struct registry_advertisement* ad = advertisements; ad != NULL; ad = ad->next) {
		cap_id_t delegated;
		if (!query_matches(query, ad)) continue;
		if (total++ < offset || returned >= size) continue;
		if (cap_delegate(ad->capability, caller, ad->client_rights, &delegated) != SYSCALL_STATUS_OK)
			return INIT_REGISTRY_RESOURCE_FAILURE;
		fill_service_entry(ad, delegated, &entries[returned++]);
	}
	*out_returned = returned;
	*out_total    = total;
	return INIT_REGISTRY_OK;
}

static int browse_compare(const struct init_browse_entry* left, const struct init_browse_entry* right) {
	if (left->kind < right->kind) return -1;
	if (left->kind > right->kind) return 1;
	int comparison = strcmp(left->name, right->name);
	if (comparison != 0) return comparison;
	if (left->major < right->major) return -1;
	if (left->major > right->major) return 1;
	if (left->minor < right->minor) return -1;
	if (left->minor > right->minor) return 1;
	return 0;
}

static enum init_registry_status browse_add(struct init_browse_entry** values, size_t* count, size_t* capacity,
                                            const struct init_browse_entry* candidate) {
	for (size_t i = 0u; i < *count; i++) {
		if (browse_compare(&(*values)[i], candidate) == 0) return INIT_REGISTRY_OK;
	}
	if (*count == *capacity) {
		size_t new_capacity = *capacity == 0u ? 8u : *capacity * 2u;
		if (new_capacity < *capacity || new_capacity > SIZE_MAX / sizeof(**values)) return INIT_REGISTRY_NO_MEMORY;
		void* grown = realloc(*values, new_capacity * sizeof(**values));
		if (grown == NULL) return INIT_REGISTRY_NO_MEMORY;
		*values   = grown;
		*capacity = new_capacity;
	}
	size_t position = *count;
	while (position != 0u && browse_compare(candidate, &(*values)[position - 1u]) < 0) {
		(*values)[position] = (*values)[position - 1u];
		position--;
	}
	(*values)[position] = *candidate;
	(*count)++;
	return INIT_REGISTRY_OK;
}

enum init_registry_status registry_browse(const char* namespace_path, uint64_t offset, uint64_t size,
                                          struct init_browse_entry* entries, uint64_t* out_returned,
                                          uint64_t* out_total) {
	struct init_browse_entry* values   = NULL;
	size_t                    count    = 0u;
	size_t                    capacity = 0u;
	size_t                    namespace_length;
	enum init_registry_status status = INIT_REGISTRY_OK;

	if (!registry_namespace_valid(namespace_path, true) || (size != 0u && entries == NULL) || out_returned == NULL ||
	    out_total == NULL)
		return INIT_REGISTRY_INVALID_ARGUMENT;
	namespace_length = strlen(namespace_path);
	prune_dead_advertisements();
	for (struct registry_advertisement* ad = advertisements; ad != NULL; ad = ad->next) {
		const char*              remainder;
		const char*              slash;
		struct init_browse_entry candidate = {0};
		if (namespace_length == 0u) {
			remainder = ad->selector.namespace_path;
		}
		else {
			if (!string_has_prefix(ad->selector.namespace_path, namespace_path, namespace_length)) continue;
			if (ad->selector.namespace_path[namespace_length] == '\0') {
				candidate.kind  = INIT_BROWSE_PROTOCOL;
				candidate.major = ad->selector.major;
				candidate.minor = ad->minor;
				(void)strlcpy(candidate.name, ad->selector.protocol, sizeof(candidate.name));
				status = browse_add(&values, &count, &capacity, &candidate);
				if (status != INIT_REGISTRY_OK) break;
				continue;
			}
			if (ad->selector.namespace_path[namespace_length] != '/') continue;
			remainder = ad->selector.namespace_path + namespace_length + 1u;
		}
		slash = remainder;
		while (*slash != '\0' && *slash != '/') slash++;
		candidate.kind          = INIT_BROWSE_NAMESPACE;
		size_t component_length = (size_t)(slash - remainder);
		memcpy(candidate.name, remainder, component_length);
		candidate.name[component_length] = '\0';
		status                           = browse_add(&values, &count, &capacity, &candidate);
		if (status != INIT_REGISTRY_OK) break;
	}
	if (status == INIT_REGISTRY_OK) {
		uint64_t available = offset >= count ? 0u : (uint64_t)count - offset;
		uint64_t returned  = available < size ? available : size;
		if (returned != 0u) memcpy(entries, values + (size_t)offset, (size_t)returned * sizeof(*entries));
		*out_returned = returned;
		*out_total    = count;
	}
	free(values);
	return status;
}

enum init_registry_status registry_watch(process_id_t owner, const struct init_protocol_query* query,
                                         cap_id_t signal_capability, uint64_t* out_subscription_id,
                                         uint64_t* out_counter) {
	struct registry_watch*      watch;
	struct signal_send_response response;
	bool                        valid = false;
	if (owner == PROCESS_PID_INVALID || !registry_query_valid(query) || signal_capability == CAP_ID_INVALID ||
	    out_subscription_id == NULL || out_counter == NULL)
		return INIT_REGISTRY_INVALID_ARGUMENT;
	if (cap_valid(signal_capability, &valid) != SYSCALL_STATUS_OK || !valid) return INIT_REGISTRY_INVALID_ARGUMENT;
	watch = malloc(sizeof(*watch));
	if (watch == NULL) return INIT_REGISTRY_NO_MEMORY;
	if (next_watch_id == 0u) {
		free(watch);
		return INIT_REGISTRY_RESOURCE_FAILURE;
	}
	*watch = (struct registry_watch){
		.id                = next_watch_id++,
		.owner             = owner,
		.query             = *query,
		.signal_capability = signal_capability,
		.counter           = 1u,
		.next              = watches,
	};
	watches = watch;
	if (signal_send(signal_capability, 1u, 0u, 0u, 0u, SIGNAL_SEND_FLAG_COALESCE, &response) != SYSCALL_STATUS_OK) {
		watches = watch->next;
		free(watch);
		return INIT_REGISTRY_DELIVERY_FAILED;
	}
	*out_subscription_id = watch->id;
	*out_counter         = watch->counter;
	return INIT_REGISTRY_OK;
}

enum init_registry_status registry_unwatch(process_id_t owner, uint64_t subscription_id) {
	struct registry_watch** cursor;
	if (owner == PROCESS_PID_INVALID || subscription_id == 0u) return INIT_REGISTRY_INVALID_ARGUMENT;
	cursor = &watches;
	while (*cursor != NULL && (*cursor)->id != subscription_id) cursor = &(*cursor)->next;
	if (*cursor == NULL) return INIT_REGISTRY_NOT_FOUND;
	if ((*cursor)->owner != owner) return INIT_REGISTRY_DENIED;
	struct registry_watch* removed = *cursor;
	*cursor                        = removed->next;
	free(removed);
	return INIT_REGISTRY_OK;
}
