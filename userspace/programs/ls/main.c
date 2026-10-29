#include <base/cap.h>
#include <libc/stdlib.h>
#include <libc/string.h>
#include <protocol/vfs.h>
#include <runtime/registry.h>
#include <runtime/tty_session.h>
#include <runtime/vfs.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <system/capability.h>
#include <system/display.h>
#include <system/trace.h>

#define VFS_PROTOCOL_VERSION_MAJOR 1u
#define VFS_PROTOCOL_VERSION_MINOR 0u

enum {
	LIST_PATH_CAPACITY          = 4096u,
	LIST_INITIAL_ENTRY_CAPACITY = (CAP_MAX_RESPONSE_SIZE - sizeof(struct filesystem_directory_enumerate_response)) /
		sizeof(struct filesystem_directory_entry),
};

enum list_option {
	LIST_OPTION_ALL          = 1u << 0,
	LIST_OPTION_ALMOST_ALL   = 1u << 1,
	LIST_OPTION_LONG         = 1u << 2,
	LIST_OPTION_ONE_PER_LINE = 1u << 3,
};

struct list_context {
	cap_id_t vfs_cap;
	cap_id_t working_directory_cap;
	/* Zero when standard input is not a terminal: output is then one plain name per line. */
	size_t   terminal_columns;
	uint32_t options;
};

/* A lexically normalized path, relative to the working directory unless absolute. */
struct list_path {
	char   bytes[LIST_PATH_CAPACITY];
	size_t size;
	bool   absolute;
};

struct list_item {
	const uint8_t*              name;
	size_t                      name_size;
	struct filesystem_node_info info;
};

struct list_directory {
	const char*                 operand;
	struct list_path            path;
	cap_id_t                    capability;
	struct filesystem_node_info info;
};

/* A listing is built in memory and written at once, since every write to stdout is a terminal call. */
struct list_output {
	char*  bytes;
	size_t size;
	size_t capacity;
	bool   failed;
};

TRACE_LABEL(trace_ls_main, "ls/main");
TRACE_LABEL(trace_ls_enumerate, "ls/directory/enumerate");
TRACE_LABEL(trace_ls_output, "ls/output");

static bool write_error(const char* operand, const char* message) {
	if (operand == NULL) return fprintf(stderr, "ls: %s\n", message) >= 0;
	return fprintf(stderr, "ls: %s: %s\n", operand, message) >= 0;
}

static bool call_succeeded(struct filesystem_call_result result) {
	return result.transport_status == SYSCALL_STATUS_OK && result.status == FILESYSTEM_STATUS_OK;
}

static const char* filesystem_error_message(struct filesystem_call_result result) {
	if (result.transport_status != SYSCALL_STATUS_OK) return "filesystem operation failed";
	switch (result.status) {
	case FILESYSTEM_STATUS_NOT_FOUND:
		return "no such file or directory";
	case FILESYSTEM_STATUS_NOT_DIRECTORY:
		return "not a directory";
	case FILESYSTEM_STATUS_ACCESS_DENIED:
		return "permission denied";
	case FILESYSTEM_STATUS_INVALID_ARGUMENT:
		return "invalid path";
	default:
		return "filesystem operation failed";
	}
}

static bool remove_component(struct list_path* path) {
	size_t root = path->absolute ? 1u : 0u;

	if (path->size == root) return path->absolute;
	while (path->size > root && path->bytes[path->size - 1u] != '/') path->size--;
	if (path->size > root) path->size--;
	return true;
}

/* Returns NULL on success, or the message describing why the path cannot be used. */
static const char* normalize_path(const char* operand, struct list_path* out) {
	size_t length = strlen(operand);
	size_t offset = 0u;

	out->absolute = length != 0u && operand[0] == '/';
	out->size     = 0u;
	if (out->absolute) out->bytes[out->size++] = '/';
	while (offset < length) {
		size_t start;
		size_t size;
		size_t separator_size;

		while (offset < length && operand[offset] == '/') offset++;
		start = offset;
		while (offset < length && operand[offset] != '/') offset++;
		size = offset - start;
		if (size == 0u || (size == 1u && operand[start] == '.')) continue;
		if (size == 2u && operand[start] == '.' && operand[start + 1u] == '.') {
			/* Programs only hold the working directory, not a path to its parent. */
			if (!remove_component(out)) return "parent of the working directory is not reachable";
			continue;
		}
		separator_size = out->size > (out->absolute ? 1u : 0u) ? 1u : 0u;
		if (size > FILESYSTEM_NAME_MAX || separator_size + size > sizeof(out->bytes) - out->size) return "invalid path";
		if (separator_size != 0u) out->bytes[out->size++] = '/';
		memcpy(out->bytes + out->size, operand + start, size);
		out->size += size;
	}
	return NULL;
}

static bool acquire_vfs(struct list_context* context) {
	static const struct registry_protocol_query query = {
		.namespace_path = VFS_NAMESPACE,
		.protocol       = VFS_PROTOCOL_NAME,
		.major          = VFS_PROTOCOL_VERSION_MAJOR,
		.minor          = VFS_PROTOCOL_VERSION_MINOR,
	};
	struct registry_service_handle service = {.capability = CAP_ID_INVALID};
	struct registry_call_result    acquired;

	if (context->vfs_cap != CAP_ID_INVALID) return true;
	acquired = registry_acquire(&query, VFS_SERVICE_NAME, &service);
	if (acquired.transport_status != SYSCALL_STATUS_OK || acquired.status != REGISTRY_STATUS_OK) {
		if (service.capability != CAP_ID_INVALID) (void)cap_drop(service.capability);
		return false;
	}
	context->vfs_cap = service.capability;
	return context->vfs_cap != CAP_ID_INVALID;
}

/* The working directory itself is borrowed rather than opened, so release it with release_node. */
static struct filesystem_call_result open_path(struct list_context* context, const struct list_path* path,
                                               struct filesystem_node_handle* out_node) {
	const struct filesystem_call_result failed = {.status           = FILESYSTEM_STATUS_NONE,
	                                              .transport_status = SYSCALL_STATUS_FAILED};

	*out_node = (struct filesystem_node_handle){.capability = CAP_ID_INVALID};
	if (path->absolute) {
		if (!acquire_vfs(context)) return failed;
		return vfs_open(context->vfs_cap, path->bytes, path->size, CAP_READ, out_node);
	}
	if (context->working_directory_cap == CAP_ID_INVALID) return failed;
	if (path->size != 0u)
		return vfs_directory_open(context->working_directory_cap, path->bytes, path->size, CAP_READ, out_node);
	out_node->capability = context->working_directory_cap;
	return vfs_directory_info(context->working_directory_cap, &out_node->info);
}

static void release_node(const struct list_context* context, cap_id_t capability) {
	if (capability != CAP_ID_INVALID && capability != context->working_directory_cap) (void)cap_drop(capability);
}

static void output_append(struct list_output* output, const void* bytes, size_t size) {
	if (output->failed || size == 0u) return;
	if (size > output->capacity - output->size) {
		size_t capacity = output->capacity == 0u ? 256u : output->capacity;
		char*  grown;

		while (size > capacity - output->size) {
			if (capacity > SIZE_MAX / 2u) {
				output->failed = true;
				return;
			}
			capacity *= 2u;
		}
		grown = realloc(output->bytes, capacity);
		if (grown == NULL) {
			output->failed = true;
			return;
		}
		output->bytes    = grown;
		output->capacity = capacity;
	}
	memcpy(output->bytes + output->size, bytes, size);
	output->size += size;
}

static void output_append_padding(struct list_output* output, size_t count) {
	static const char spaces[] = "                ";

	while (count != 0u) {
		size_t chunk = count < sizeof(spaces) - 1u ? count : sizeof(spaces) - 1u;

		output_append(output, spaces, chunk);
		count -= chunk;
	}
}

static bool flush_output(struct list_output* output) {
	bool success;
	TRACE_SCOPE(trace, &trace_ls_output);

	if (output->failed) success = write_error(NULL, "out of memory");
	else success = output->size == 0u || fwrite(output->bytes, 1u, output->size, stdout) == output->size;
	free(output->bytes);
	*output = (struct list_output){0};
	return success;
}

static size_t item_width(const struct list_item* item) {
	size_t width = 0u;

	for (size_t i = 0u; i < item->name_size; i++) {
		if ((item->name[i] & 0xc0u) != 0x80u) width++;
	}
	return width;
}

/* Size of the C0, DEL or UTF-8 encoded C1 control character at index, or zero for any other byte. */
static size_t control_size(const uint8_t* name, size_t size, size_t index) {
	if (name[index] < 0x20u || name[index] == 0x7fu) return 1u;
	if (name[index] == 0xc2u && index + 1u < size && name[index + 1u] >= 0x80u && name[index + 1u] <= 0x9fu) return 2u;
	return 0u;
}

static void output_append_name(struct list_output* output, const struct list_context* context,
                               const struct list_item* item) {
	bool   colored = context->terminal_columns != 0u && item->info.type == FILESYSTEM_NODE_DIRECTORY;
	size_t start   = 0u;

	if (colored) output_append(output, "\x1b[1;34m", 7u);
	/* Control characters in a name would otherwise be interpreted by the terminal. */
	for (size_t i = 0u; i < item->name_size;) {
		size_t control = control_size(item->name, item->name_size, i);

		if (control == 0u) {
			i++;
			continue;
		}
		output_append(output, item->name + start, i - start);
		output_append(output, "?", 1u);
		i += control;
		start = i;
	}
	output_append(output, item->name + start, item->name_size - start);
	if (colored) output_append(output, "\x1b[0m", 4u);
}

static void output_append_long(struct list_output* output, const struct list_context* context,
                               const struct list_item* item, size_t size_width) {
	char size[24];
	int  size_length = snprintf(size, sizeof(size), "%llu", (unsigned long long)item->info.size);
	char mode[]      = {item->info.type == FILESYSTEM_NODE_DIRECTORY ? 'd' : '-',
	                    'r',
	                    (item->info.flags & FILESYSTEM_NODE_READ_ONLY) != 0u ? '-' : 'w',
	                    ' '};

	output_append(output, mode, sizeof(mode));
	output_append_padding(output, size_width - (size_t)size_length);
	output_append(output, size, (size_t)size_length);
	output_append(output, " ", 1u);
	output_append_name(output, context, item);
	output_append(output, "\n", 1u);
}

static void output_append_items(struct list_output* output, const struct list_context* context,
                                const struct list_item* items, size_t count) {
	size_t width = 0u;
	size_t columns;
	size_t rows;

	if (count == 0u) return;
	if ((context->options & LIST_OPTION_LONG) != 0u) {
		size_t size_width = 1u;

		for (size_t i = 0u; i < count; i++) {
			size_t digits = 1u;
			for (uint64_t size = items[i].info.size; size >= 10u; size /= 10u) digits++;
			if (digits > size_width) size_width = digits;
		}
		for (size_t i = 0u; i < count; i++) output_append_long(output, context, &items[i], size_width);
		return;
	}
	for (size_t i = 0u; i < count; i++) {
		size_t current = item_width(&items[i]);
		if (current > width) width = current;
	}
	width += 2u;
	columns = (context->options & LIST_OPTION_ONE_PER_LINE) != 0u || width > context->terminal_columns
	              ? 1u
	              : context->terminal_columns / width;
	rows    = (count + columns - 1u) / columns;
	/* Entries fill each column top to bottom before moving to the next one. */
	for (size_t row = 0u; row < rows; row++) {
		for (size_t index = row; index < count; index += rows) {
			output_append_name(output, context, &items[index]);
			if (index + rows < count) output_append_padding(output, width - item_width(&items[index]));
		}
		output_append(output, "\n", 1u);
	}
}

static int compare_items(const void* left, const void* right) {
	const struct list_item* left_item  = left;
	const struct list_item* right_item = right;
	size_t size  = left_item->name_size < right_item->name_size ? left_item->name_size : right_item->name_size;
	int    order = memcmp(left_item->name, right_item->name, size);

	if (order != 0) return order;
	return (left_item->name_size > right_item->name_size) - (left_item->name_size < right_item->name_size);
}

static int compare_directories(const void* left, const void* right) {
	return strcmp(((const struct list_directory*)left)->operand, ((const struct list_directory*)right)->operand);
}

/* Returns NULL on success, or the message describing the failure. */
static const char* read_directory(cap_id_t directory_cap, struct filesystem_directory_entry** out_entries,
                                  size_t* out_count) {
	struct filesystem_directory_entry* entries  = NULL;
	size_t                             count    = 0u;
	size_t                             capacity = 0u;
	uint64_t                           total    = 0u;
	TRACE_SCOPE(trace, &trace_ls_enumerate);

	for (;;) {
		struct filesystem_call_result result;
		size_t                        returned;

		if (count == capacity) {
			struct filesystem_directory_entry* grown;
			size_t                             next = capacity == 0u ? LIST_INITIAL_ENTRY_CAPACITY : capacity * 2u;

			if (total > next && total <= SIZE_MAX / sizeof(*entries)) next = (size_t)total;
			grown = next > SIZE_MAX / sizeof(*entries) ? NULL : realloc(entries, next * sizeof(*entries));
			if (grown == NULL) {
				free(entries);
				return "out of memory";
			}
			entries  = grown;
			capacity = next;
		}
		result = vfs_directory_enumerate(directory_cap, count, entries + count, capacity - count, &returned, &total);
		if (!call_succeeded(result)) {
			free(entries);
			return filesystem_error_message(result);
		}
		count += returned;
		if (returned == 0u || count >= total) break;
	}
	*out_entries = entries;
	*out_count   = count;
	return NULL;
}

static struct filesystem_node_info read_parent_info(struct list_context*         context,
                                                    const struct list_directory* directory) {
	struct filesystem_node_handle parent = {.capability = CAP_ID_INVALID};
	struct list_path*             path   = malloc(sizeof(*path));
	struct filesystem_node_info   info   = directory->info;

	if (path == NULL) return info;
	*path = directory->path;
	if (remove_component(path) && call_succeeded(open_path(context, path, &parent))) info = parent.info;
	release_node(context, parent.capability);
	free(path);
	return info;
}

/* Fill items with the entries ls shows for an opened directory. */
static size_t collect_items(struct list_context* context, const struct list_directory* directory,
                            const struct filesystem_directory_entry* entries, size_t count, struct list_item* items) {
	size_t item_count = 0u;

	if ((context->options & LIST_OPTION_ALL) != 0u) {
		struct filesystem_node_info parent_info = {.type = FILESYSTEM_NODE_DIRECTORY};

		/* Only the long format shows more of ".." than its type. */
		if ((context->options & LIST_OPTION_LONG) != 0u) parent_info = read_parent_info(context, directory);
		items[item_count++] = (struct list_item){.name = (const uint8_t*)".", .name_size = 1u, .info = directory->info};
		items[item_count++] = (struct list_item){.name = (const uint8_t*)"..", .name_size = 2u, .info = parent_info};
	}
	for (size_t i = 0u; i < count; i++) {
		if (entries[i].name_size == 0u ||
		    (entries[i].name[0] == '.' && (context->options & (LIST_OPTION_ALL | LIST_OPTION_ALMOST_ALL)) == 0u))
			continue;
		items[item_count++] =
			(struct list_item){.name = entries[i].name, .name_size = entries[i].name_size, .info = entries[i].info};
	}
	qsort(items, item_count, sizeof(*items), compare_items);
	return item_count;
}

static bool list_directory(struct list_context* context, const struct list_directory* directory, bool header,
                           bool* printed) {
	struct filesystem_directory_entry* entries;
	struct list_item*                  items;
	struct list_output                 output = {0};
	size_t                             count;
	const char*                        error;

	error = read_directory(directory->capability, &entries, &count);
	if (error != NULL) {
		(void)write_error(directory->operand, error);
		return false;
	}
	items = malloc((count + 2u) * sizeof(*items));
	if (items == NULL) {
		free(entries);
		(void)write_error(directory->operand, "out of memory");
		return false;
	}
	count = collect_items(context, directory, entries, count, items);
	if (header) {
		if (*printed) output_append(&output, "\n", 1u);
		output_append(&output, directory->operand, strlen(directory->operand));
		output_append(&output, ":\n", 2u);
	}
	output_append_items(&output, context, items, count);
	*printed = true;
	free(items);
	free(entries);
	return flush_output(&output);
}

/* Like GNU ls, options may follow operands; every argument after "--" is an operand. */
static bool is_operand(const char* argument, bool after_separator) {
	return after_separator || argument[0] != '-' || argument[1] == '\0';
}

static size_t terminal_columns(void) {
	struct tty_session_get_info_response info;
	cap_id_t                             stdin_cap;

	/* Standard output is write-only, so the terminal size is read through standard input. */
	display_get_standard_streams(&stdin_cap, NULL, NULL);
	if (stdin_cap == CAP_ID_INVALID || tty_session_get_info(stdin_cap, &info) != SYSCALL_STATUS_OK ||
	    info.columns > SIZE_MAX)
		return 0u;
	return (size_t)info.columns;
}

/*
 * Like GNU ls, unusable operands are reported first, then file operands are
 * listed together, then each directory, all in name order.
 */
int main(int argc, char** argv, size_t capc, const cap_id_t* capv) {
	static char            current_name[] = ".";
	static char*           current_argv[] = {current_name};
	struct list_context    context        = {.vfs_cap               = CAP_ID_INVALID,
	                                         .working_directory_cap = filesystem_get_working_directory()};
	struct list_item*      files;
	struct list_directory* directories;
	struct list_output     output          = {0};
	size_t                 operand_count   = 0u;
	size_t                 file_count      = 0u;
	size_t                 directory_count = 0u;
	bool                   after_separator = false;
	bool                   printed         = false;
	bool                   success         = true;
	TRACE_SCOPE(trace, &trace_ls_main);

	if (argc < 0 || (argc != 0 && argv == NULL) || capc != 0u || capv != NULL) return 1;
	for (int i = 0; i < argc; i++) {
		if (is_operand(argv[i], after_separator)) {
			operand_count++;
			continue;
		}
		if (strcmp(argv[i], "--") == 0) {
			after_separator = true;
			continue;
		}
		if (argv[i][1] == '-') {
			(void)fprintf(stderr, "ls: unknown option %s\n", argv[i]);
			return 1;
		}
		for (const char* letter = argv[i] + 1; *letter != '\0'; letter++) {
			if (*letter == 'a')
				context.options = (context.options & ~(uint32_t)LIST_OPTION_ALMOST_ALL) | LIST_OPTION_ALL;
			else if (*letter == 'A')
				context.options = (context.options & ~(uint32_t)LIST_OPTION_ALL) | LIST_OPTION_ALMOST_ALL;
			else if (*letter == 'l') context.options |= LIST_OPTION_LONG;
			else if (*letter == '1') context.options |= LIST_OPTION_ONE_PER_LINE;
			else {
				(void)fprintf(stderr, "ls: unknown option -%c\n", *letter);
				return 1;
			}
		}
	}
	if (operand_count == 0u) {
		argc          = 1;
		argv          = current_argv;
		operand_count = 1u;
	}
	context.terminal_columns = terminal_columns();
	files                    = malloc(operand_count * sizeof(*files));
	directories              = malloc(operand_count * sizeof(*directories));
	if (files == NULL || directories == NULL) {
		free(directories);
		free(files);
		(void)write_error(NULL, "out of memory");
		return 1;
	}
	after_separator = false;
	for (int i = 0; i < argc; i++) {
		struct list_directory*        directory = &directories[directory_count];
		struct filesystem_node_handle node;
		struct filesystem_call_result result;
		const char*                   error;

		if (!is_operand(argv[i], after_separator)) {
			after_separator = after_separator || strcmp(argv[i], "--") == 0;
			continue;
		}
		error = normalize_path(argv[i], &directory->path);
		if (error != NULL) {
			(void)write_error(argv[i], error);
			success = false;
			continue;
		}
		result = open_path(&context, &directory->path, &node);
		if (!call_succeeded(result)) {
			release_node(&context, node.capability);
			(void)write_error(argv[i], filesystem_error_message(result));
			success = false;
			continue;
		}
		if (node.info.type == FILESYSTEM_NODE_DIRECTORY) {
			directory->operand    = argv[i];
			directory->capability = node.capability;
			directory->info       = node.info;
			directory_count++;
			continue;
		}
		release_node(&context, node.capability);
		files[file_count++] =
			(struct list_item){.name = (const uint8_t*)argv[i], .name_size = strlen(argv[i]), .info = node.info};
	}
	qsort(files, file_count, sizeof(*files), compare_items);
	qsort(directories, directory_count, sizeof(*directories), compare_directories);
	if (file_count != 0u) {
		output_append_items(&output, &context, files, file_count);
		success = flush_output(&output) && success;
		printed = true;
	}
	for (size_t i = 0u; i < directory_count; i++) {
		success = list_directory(&context, &directories[i], operand_count > 1u, &printed) && success;
		release_node(&context, directories[i].capability);
	}
	free(directories);
	free(files);
	if (context.vfs_cap != CAP_ID_INVALID) (void)cap_drop(context.vfs_cap);
	return success ? 0 : 1;
}
