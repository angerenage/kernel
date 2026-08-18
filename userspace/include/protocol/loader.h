#pragma once

/*
 * Program loader protocol.
 *
 * Loader services create processes from executable images exposed as Blob
 * capabilities. LOAD prepares a complete but non-running process and returns
 * a capability representing that loading state. The caller then completes the
 * loading lifecycle with either RUN, which starts the program, or CANCEL,
 * which abandons the prepared process.
 *
 * Current specification version: 1.0-beta
 */

#include <base/cap.h>
#include <base/process.h>
#include <stdint.h>

#define LOADER_PROTOCOL_NAME "loader"

/* Rights are part of the protocol contract and must be granted exactly. */
#define LOADER_V1_SERVICE_CAP_RIGHTS ((cap_rights_t)(CAP_CALL))
#define LOADER_V1_BLOB_CAP_RIGHTS ((cap_rights_t)(CAP_CALL | CAP_READ | CAP_REVOKE))
#define LOADER_V1_LOAD_CAP_RIGHTS ((cap_rights_t)(CAP_CALL | CAP_DELEGATE))
#define LOADER_V1_PROCESS_CAP_RIGHTS                                                                                   \
	((cap_rights_t)(CAP_CALL | CAP_READ | CAP_WAIT | CAP_MANAGE | CAP_DESTROY | CAP_EXEC | CAP_DELEGATE))
#define LOADER_V1_THREAD_CAP_RIGHTS                                                                                    \
	((cap_rights_t)(CAP_CALL | CAP_WAIT | CAP_MANAGE | CAP_READ | CAP_DESTROY | CAP_DELEGATE))

enum loader_v1_op {
	LOADER_V1_OP_LOAD   = 0u,
	LOADER_V1_OP_RUN    = 1u,
	LOADER_V1_OP_CANCEL = 2u,
};

struct loader_v1_request_header {
	uint32_t op;
};

/*
 * Prepare a program from a Blob capability owned by the loader process.
 *
 * The caller is responsible for delegating blob_cap to the loader before the
 * call. The delegated capability must grant exactly LOADER_V1_BLOB_CAP_RIGHTS.
 * The loader may use the delegated Blob only while servicing LOAD and must not
 * retain it after replying. The caller must revoke the temporary delegated
 * capability after LOAD returns, whether the operation succeeds or fails.
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
 * The prepared process is intentionally not exposed through a process
 * capability yet. Until the lifecycle terminates, RUN and CANCEL on load_cap
 * are the only operations allowed to control it. process_id is returned so the
 * caller may delegate capabilities to the prepared process before RUN.
 */
struct loader_v1_load_response {
	cap_id_t     load_cap; /* Rights: exactly LOADER_V1_LOAD_CAP_RIGHTS. */
	process_id_t process_id;
};

/*
 * Start a previously loaded program.
 *
 * RUN is issued on load_cap, not on the loader service capability. The argv
 * payload immediately follows this structure and consists of argc consecutive
 * NUL-terminated strings occupying exactly argv_size bytes.
 *
 * argc == 0 requires argv_size == 0. Validation failures leave the loading
 * object prepared and may be retried. Once the loader begins starting the
 * process, RUN consumes the loading object: on success it returns process and
 * main-thread capabilities, while a start failure must destroy the process and
 * invalidate load_cap before replying. Environment and auxiliary-vector data
 * are intentionally unspecified by the current beta revision.
 */
struct loader_v1_run_request {
	struct loader_v1_request_header header;
	uint32_t                        argc;
	uint32_t                        argv_size;
};

struct loader_v1_run_response {
	cap_id_t process_cap; /* Rights: exactly LOADER_V1_PROCESS_CAP_RIGHTS. */
	cap_id_t thread_cap;  /* Rights: exactly LOADER_V1_THREAD_CAP_RIGHTS. */
};

/*
 * Abandon a prepared program instead of starting it.
 *
 * CANCEL is issued on load_cap and has no response payload. On success the
 * prepared process and loader-owned loading state are destroyed and load_cap
 * is invalidated. CANCEL is only valid while RUN has not consumed the loading
 * object.
 */
struct loader_v1_cancel_request {
	struct loader_v1_request_header header;
};

/*
 * LOAD lifecycle:
 *
 *   loader service capability
 *           |
 *           | LOADER_V1_OP_LOAD
 *           v
 *      load_cap + process_id
 *           |
 *           +---- LOADER_V1_OP_RUN ----> process_cap + thread_cap
 *           |
 *           `---- LOADER_V1_OP_CANCEL --> destroyed
 *
 * A prepared process has no externally usable process-control capability. A
 * successful RUN transfers normal process control to its caller. CANCEL, or a
 * RUN failure after starting has begun, destroys the prepared process and its
 * loader-owned state.
 *
 * The loader is responsible for constructing the runtime startup state and
 * delegating the system capabilities required by every process before RUN.
 */
