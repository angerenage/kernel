#pragma once

/*
 * Program loader protocol.
 *
 * Loader services create processes from executable images exposed as Blob
 * capabilities. LOAD prepares a complete but non-running process and returns
 * a capability representing that loading state. RUN is then issued on that
 * capability to start the program with its arguments.
 *
 * Current specification version: 1.0-beta
 */

#include <base/cap.h>
#include <base/process.h>
#include <stdint.h>

#define LOADER_PROTOCOL_NAME "loader"

#define LOADER_PROTOCOL_VERSION_MAJOR 1u
#define LOADER_PROTOCOL_VERSION_MINOR 0u

enum loader_v1_op {
	LOADER_V1_OP_LOAD = 0u,
	LOADER_V1_OP_RUN  = 1u,
};

struct loader_v1_request_header {
	uint32_t op;
};

/*
 * Prepare a program from a Blob capability owned by the loader process.
 *
 * The caller is responsible for delegating blob_cap to the loader before the
 * call. The delegated capability must grant CAP_CALL | CAP_READ. The loader
 * must release that delegated capability before completing LOAD, whether the
 * operation succeeds or fails.
 *
 * The process name immediately follows this structure and contains exactly
 * name_size bytes. It is not NUL-terminated by the protocol.
 */
struct loader_v1_load_request {
	struct loader_v1_request_header header;
	cap_id_t                        blob_cap;
	uint32_t                        name_size;
	uint32_t                        reserved;
};

/*
 * A successful LOAD returns a non-running process and a loading capability.
 *
 * load_cap represents loader-private state associated with process_cap and is
 * used for the subsequent RUN operation. The process capability is returned
 * separately so the caller can use the normal process-control protocol. The
 * process ID is returned for capability delegation and for convenience.
 */
struct loader_v1_load_response {
	cap_id_t     load_cap;
	cap_id_t     process_cap;
	process_id_t process_id;
};

/*
 * Start a previously loaded program.
 *
 * RUN is issued on load_cap, not on the loader service capability. The argv
 * payload immediately follows this structure and consists of argc consecutive
 * NUL-terminated strings occupying exactly argv_size bytes.
 *
 * argc == 0 requires argv_size == 0. A loading object may be run at most once.
 * Environment and auxiliary-vector data are intentionally unspecified by the
 * current beta revision.
 */
struct loader_v1_run_request {
	struct loader_v1_request_header header;
	uint32_t                        argc;
	uint32_t                        argv_size;
};

struct loader_v1_run_response {
	cap_id_t thread_cap;
};

/*
 * LOAD lifecycle:
 *
 *   loader service capability
 *           |
 *           | LOADER_V1_OP_LOAD
 *           v
 *   load_cap + process_cap + process_id
 *           |
 *           | LOADER_V1_OP_RUN
 *           v
 *       thread_cap
 *
 * The loader is responsible for constructing the runtime startup state and
 * delegating the system capabilities required by every process before RUN.
 */
