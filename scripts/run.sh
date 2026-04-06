#!/usr/bin/env bash
set -euo pipefail

readonly ARCHES=(
	"x86_64"
	"aarch64"
	"riscv64"
	"loongarch64"
)

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

usage() {
	cat <<'EOF'
Usage: run.sh [--test|-t | --kernel-selftest] [--test-name <name>] [--arch <arch> | --all] [--builddir <path>] [--headless] [--debug] [--debug-port <port>] [--timeout <seconds>] [-- <extra qemu args>]

Modes:
  Default mode         Launch QEMU. Requires --arch <arch> or --all.
  --test, -t           Run Meson tests for the current machine architecture.
  --kernel-selftest    Boot QEMU, watch serial output, and exit non-interactively
                       based on in-kernel selftest PASS/FAIL markers.

QEMU options:
  --arch <arch>        Target architecture (x86_64, aarch64, riscv64, loongarch64).
  --all                Launch QEMU for every architecture sequentially.
  --builddir <path>    Meson build directory to use for single-architecture runs.
                       Defaults to <repo>/build-<arch>.
  --headless           Pass --headless through to the QEMU launcher.
  --debug              Pass --debug through to the QEMU launcher.
  --debug-port <port>  Pass --debug-port through to the QEMU launcher.
  --timeout <seconds>  Selftest mode timeout. Defaults to 30 seconds.

Test options:
  --test-name <name>   Run only the named Meson test (repeatable).

General:
  -h, --help           Show this help and exit.

Test arch resolution:
  Test mode detects the current machine architecture from uname(1) and uses
  the matching <repo>/build-<arch> directory. If the host architecture is not
  supported, the script exits with an error.

Examples:
  bash scripts/run.sh --arch x86_64
  bash scripts/run.sh --arch riscv64 --debug
  bash scripts/run.sh --all
  bash scripts/run.sh --test
  bash scripts/run.sh -t --test-name pmm
  bash scripts/run.sh --kernel-selftest --arch x86_64 --headless
EOF
}

error() {
	echo "run.sh: $*" >&2
	exit 1
}

log() {
	echo "[run] $*"
}

abspath() {
	case "$1" in
		/*) printf '%s\n' "$1" ;;
		*) printf '%s/%s\n' "$(pwd)" "$1" ;;
	esac
}

validate_arch() {
	local arch="$1"
	local candidate

	for candidate in "${ARCHES[@]}"; do
		if [[ "$candidate" == "$arch" ]]; then
			return 0
		fi
	done

	error "unsupported architecture: ${arch}"
}

default_builddir_for_arch() {
	validate_arch "$1"
	printf '%s/build-%s\n' "$REPO_ROOT" "$1"
}

host_arch() {
	local raw_arch

	raw_arch="$(uname -m 2>/dev/null || printf 'x86_64\n')"
	case "$raw_arch" in
		x86_64|amd64) printf '%s\n' "x86_64" ;;
		aarch64|arm64) printf '%s\n' "aarch64" ;;
		riscv64) printf '%s\n' "riscv64" ;;
		loongarch64|loong64) printf '%s\n' "loongarch64" ;;
		*) return 1 ;;
	esac
}

run_tests() {
	local arch="$1"
	local build_dir="$2"
	local -a meson_args
	local test_name

	[[ -d "$build_dir" ]] || error "build directory not found for ${arch}: ${build_dir}"

	meson_args=(
		test
		-C "$build_dir"
		--print-errorlogs
	)

	for test_name in "${TEST_NAMES[@]}"; do
		meson_args+=("$test_name")
	done

	log "running tests for ${arch} from ${build_dir}"
	(
		cd "$REPO_ROOT"
		meson "${meson_args[@]}"
	)
}

run_qemu_for_arch() {
	local arch="$1"
	local build_dir="$2"
	local -a qemu_args

	qemu_args=(
		"${SCRIPT_DIR}/run_qemu.sh"
		--arch "$arch"
		--builddir "$build_dir"
	)

	if (( HEADLESS )); then
		qemu_args+=( --headless )
	fi

	if (( DEBUG_MODE )); then
		qemu_args+=( --debug --debug-port "$DEBUG_PORT" )
	fi

	if (( ${#EXTRA_QEMU_ARGS[@]} > 0 )); then
		qemu_args+=( -- "${EXTRA_QEMU_ARGS[@]}" )
	fi

	log "launching QEMU for ${arch} from ${build_dir}"
	(
		cd "$REPO_ROOT"
		bash "${qemu_args[@]}"
	)
}

stop_process() {
	local pid="$1"

	if ! kill -0 "$pid" >/dev/null 2>&1; then
		wait "$pid" 2>/dev/null || true
		return 0
	fi

	kill "$pid" >/dev/null 2>&1 || true
	for _ in 1 2 3 4 5; do
		if ! kill -0 "$pid" >/dev/null 2>&1; then
			wait "$pid" 2>/dev/null || true
			return 0
		fi
		sleep 1
	done

	kill -KILL "$pid" >/dev/null 2>&1 || true
	wait "$pid" 2>/dev/null || true
}

run_kernel_selftests() {
	local arch="$1"
	local build_dir="$2"
	local timeout_seconds="$3"
	local log_file="${build_dir}/kernel-selftest-${arch}.log"
	local qemu_pid=""
	local outcome="timeout"
	local qemu_status=0
	local deadline=0
	local -a qemu_args

	[[ -d "$build_dir" ]] || error "build directory not found for ${arch}: ${build_dir}"
	rm -f "$log_file"

	qemu_args=(
		"${SCRIPT_DIR}/run_qemu.sh"
		--arch "$arch"
		--builddir "$build_dir"
		--headless
	)

	if (( DEBUG_MODE )); then
		qemu_args+=( --debug --debug-port "$DEBUG_PORT" )
	fi

	if (( ${#EXTRA_QEMU_ARGS[@]} > 0 )); then
		qemu_args+=( -- "${EXTRA_QEMU_ARGS[@]}" )
	fi

	log "running in-kernel selftests for ${arch} from ${build_dir}"
	(
		cd "$REPO_ROOT"
		bash "${qemu_args[@]}"
	) >"$log_file" 2>&1 &
	qemu_pid="$!"

	deadline=$((SECONDS + timeout_seconds))
	while :; do
		if [[ -f "$log_file" ]] && grep -Fq "kernel: selftests result: PASS" "$log_file"; then
			outcome="pass"
			break
		fi
		if [[ -f "$log_file" ]] && grep -Fq "kernel: selftests result: FAIL" "$log_file"; then
			outcome="fail"
			break
		fi
		if ! kill -0 "$qemu_pid" >/dev/null 2>&1; then
			wait "$qemu_pid" || qemu_status=$?
			outcome="exited"
			break
		fi
		if (( SECONDS >= deadline )); then
			outcome="timeout"
			break
		fi
		sleep 1
	done

	stop_process "$qemu_pid"
	[[ -f "$log_file" ]] && cat "$log_file"

	case "$outcome" in
		pass)
			log "in-kernel selftests passed for ${arch}"
			return 0
			;;
		fail)
			error "in-kernel selftests failed for ${arch}"
			;;
		exited)
			error "QEMU exited before selftests completed for ${arch} (status ${qemu_status})"
			;;
		timeout)
			error "timed out after ${timeout_seconds}s waiting for in-kernel selftests on ${arch}"
			;;
		*)
			error "unexpected selftest outcome: ${outcome}"
			;;
	esac
}

DO_TEST=0
DO_KERNEL_SELFTEST=0
TARGET_ARCH=""
ALL_ARCHES=0
BUILD_DIR=""
DEBUG_MODE=0
DEBUG_PORT=1234
DEBUG_PORT_SET=0
HEADLESS=0
SELFTEST_TIMEOUT=30
TEST_NAMES=()
EXTRA_QEMU_ARGS=()

while [[ $# -gt 0 ]]; do
	case "$1" in
		--test|-t)
			DO_TEST=1
			shift
			;;
		--kernel-selftest)
			DO_KERNEL_SELFTEST=1
			shift
			;;
		--arch)
			[[ $# -ge 2 ]] || error "missing value for --arch"
			TARGET_ARCH="$2"
			shift 2
			;;
		--arch=*)
			TARGET_ARCH="${1#*=}"
			shift
			;;
		--all)
			ALL_ARCHES=1
			shift
			;;
		--builddir)
			[[ $# -ge 2 ]] || error "missing value for --builddir"
			BUILD_DIR="$2"
			shift 2
			;;
		--builddir=*)
			BUILD_DIR="${1#*=}"
			shift
			;;
		--headless)
			HEADLESS=1
			shift
			;;
		--test-name)
			[[ $# -ge 2 ]] || error "missing value for --test-name"
			TEST_NAMES+=( "$2" )
			shift 2
			;;
		--test-name=*)
			TEST_NAMES+=( "${1#*=}" )
			shift
			;;
		--debug)
			DEBUG_MODE=1
			shift
			;;
		--debug-port)
			[[ $# -ge 2 ]] || error "missing value for --debug-port"
			DEBUG_PORT="$2"
			DEBUG_PORT_SET=1
			shift 2
			;;
		--debug-port=*)
			DEBUG_PORT="${1#*=}"
			DEBUG_PORT_SET=1
			shift
			;;
		--timeout)
			[[ $# -ge 2 ]] || error "missing value for --timeout"
			SELFTEST_TIMEOUT="$2"
			shift 2
			;;
		--timeout=*)
			SELFTEST_TIMEOUT="${1#*=}"
			shift
			;;
		--)
			shift
			EXTRA_QEMU_ARGS=( "$@" )
			break
			;;
		-h|--help)
			usage
			exit 0
			;;
		*)
			error "unknown argument: $1"
			;;
	esac
done

if (( ALL_ARCHES )) && [[ -n "$TARGET_ARCH" ]]; then
	error "use either --arch <arch> or --all, not both"
fi

if (( DO_TEST )) && (( DO_KERNEL_SELFTEST )); then
	error "use either --test or --kernel-selftest, not both"
fi

if (( DO_TEST )); then
	[[ -z "$TARGET_ARCH" ]] || error "--arch is not used in test mode"
	(( ! ALL_ARCHES )) || error "--all is not used in test mode"
	[[ -z "$BUILD_DIR" ]] || error "--builddir is not used in test mode"
	(( ! HEADLESS )) || error "--headless is not used in test mode"
	(( ! DEBUG_MODE )) || error "--debug is not used in test mode"
	(( ! DEBUG_PORT_SET )) || error "--debug-port is not used in test mode"
	[[ "$SELFTEST_TIMEOUT" == "30" ]] || error "--timeout is not used in test mode"
	(( ${#EXTRA_QEMU_ARGS[@]} == 0 )) || error "extra QEMU arguments are not used in test mode"

	TARGET_ARCH="$(host_arch)" || error "unsupported host architecture for tests: $(uname -m 2>/dev/null || printf 'unknown')"
	BUILD_DIR="$(default_builddir_for_arch "$TARGET_ARCH")"
	run_tests "$TARGET_ARCH" "$BUILD_DIR"
elif (( DO_KERNEL_SELFTEST )); then
	(( ! ALL_ARCHES )) || error "--all is not used in kernel selftest mode"
	(( ${#TEST_NAMES[@]} == 0 )) || error "--test-name is only valid with --test"
	[[ -n "$TARGET_ARCH" ]] || error "--kernel-selftest requires --arch <arch>"

	validate_arch "$TARGET_ARCH"
	if [[ -n "$BUILD_DIR" ]]; then
		BUILD_DIR="$(abspath "$BUILD_DIR")"
	else
		BUILD_DIR="$(default_builddir_for_arch "$TARGET_ARCH")"
	fi
	run_kernel_selftests "$TARGET_ARCH" "$BUILD_DIR" "$SELFTEST_TIMEOUT"
else
	(( ${#TEST_NAMES[@]} == 0 )) || error "--test-name requires --test or -t"
	if (( DEBUG_MODE )) && [[ -z "$TARGET_ARCH" ]] && (( ! ALL_ARCHES )); then
		error "--debug requires --arch <arch> or --all"
	fi
	if (( DEBUG_PORT_SET )) && [[ -z "$TARGET_ARCH" ]] && (( ! ALL_ARCHES )); then
		error "--debug-port requires --arch <arch> or --all"
	fi
	if [[ -z "$TARGET_ARCH" ]] && (( ! ALL_ARCHES )); then
		error "missing target selection; use --arch <arch> to launch one target or --all to launch every target"
	fi
	if (( ALL_ARCHES )) && [[ -n "$BUILD_DIR" ]]; then
		error "--builddir cannot be combined with --all"
	fi

	if (( ALL_ARCHES )); then
		for arch in "${ARCHES[@]}"; do
			run_qemu_for_arch "$arch" "$(default_builddir_for_arch "$arch")"
		done
	else
		validate_arch "$TARGET_ARCH"
		if [[ -n "$BUILD_DIR" ]]; then
			BUILD_DIR="$(abspath "$BUILD_DIR")"
		else
			BUILD_DIR="$(default_builddir_for_arch "$TARGET_ARCH")"
		fi
		run_qemu_for_arch "$TARGET_ARCH" "$BUILD_DIR"
	fi
fi
