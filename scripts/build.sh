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
Usage: build.sh (--arch <arch> | --all) [--builddir <path>] [--build-root <path>] [--build-prefix <name>] [--setup|-s] [--compile|-c] [-sc] [--reconfigure] [--optimization <level>] [--no-tests] [--test-sanitizers] [--kernel-selftests] [--kernel-selftests-autorun] [--kernel-selftests-suite <name>] [--kernel-selftests-iommu <kind>] [--kernel-boot-debug] [--kernel-trace] [--kernel-trace-duration-ms <ms>] [--kernel-trace-buffer-kb <KiB>] [--kernel-trace-transport <qemu|serial>] [--crash-server-url <url>]

Target selection:
  --arch <arch>  Build a single architecture (x86_64, aarch64, riscv64, loongarch64).
  --all          Build every supported architecture in parallel.

Actions:
  --setup|-s        Configure Meson build directories.
  --compile|-c      Compile the configured build directories.
                    When neither --setup nor --compile is passed, both are run.
  -sc            Short form for running both setup and compile.
  --reconfigure  Force Meson reconfiguration for existing build directories.
  --optimization <level>
                  Meson optimization level: plain, 0, g, 1, 2, 3, or s.
                  Defaults to 0.
  --no-tests     Configure Meson with -Dtests=false.
  --test-sanitizers
                  Instrument native tests with AddressSanitizer and
                  UndefinedBehaviorSanitizer.
  --kernel-selftests
                  Configure Meson with -Dkernel_selftests=true.
  --kernel-selftests-autorun
                  Configure Meson with -Dkernel_selftests_autorun=true.
                  Implies --kernel-selftests.
  --kernel-selftests-suite <name>
                  Configure Meson with -Dkernel_selftests_suite=<name>.
                  Implies --kernel-selftests and boots only that suite.
  --kernel-selftests-iommu <kind>
                  Require none, vtd, amd, smmuv3, riscv, or loongarch-v1.
                  Implies --kernel-selftests.
  --kernel-boot-debug
                  Configure Meson with -Dkernel_boot_debug=true.
                  Enables verbose boot diagnostics in the generated image.
  --kernel-trace  Compile tracing and enable one boot-time profiling session.
  --kernel-trace-duration-ms <ms>
                  Stop and export the trace after this many milliseconds.
  --kernel-trace-buffer-kb <KiB>
                  Reserve this many KiB of trace records per CPU.
  --kernel-trace-transport <qemu|serial>
                  Export through a QEMU RAM marker or a framed serial stream.
  --crash-server-url <url>
                  Embed the HTTP(S) base URL used for crash decoder links.

Build directories:
  --builddir <path>
                  Build directory for a single --arch run.
  --build-root <path>
                  Directory that contains per-architecture build directories.
                  Defaults to the repository root.
  --build-prefix <name>
                  Prefix for per-architecture build directories under
                  --build-root. Defaults to build, producing build-<arch>.

Examples:
  bash scripts/build.sh --arch x86_64
  bash scripts/build.sh --arch x86_64 --builddir build-debug-x86_64
  bash scripts/build.sh --all --build-prefix build-debug
  bash scripts/build.sh --all --build-root /tmp/kernel-builds
  bash scripts/build.sh --arch aarch64 --setup
  bash scripts/build.sh --arch riscv64 -sc
  bash scripts/build.sh --arch x86_64 --kernel-selftests --kernel-selftests-autorun
  bash scripts/build.sh --arch x86_64 --kernel-selftests-suite vmm
  bash scripts/build.sh --arch x86_64 --kernel-boot-debug
  bash scripts/build.sh --all --compile
  bash scripts/build.sh --all -sc
EOF
}

error() {
	echo "build.sh: $*" >&2
	exit 1
}

log() {
	echo "[build] $*"
}

log_arch() {
	local arch="$1"
	shift
	echo "[build:${arch}] $*"
}

need_cmd() {
	if ! command -v "$1" >/dev/null 2>&1; then
		error "required tool '$1' not found in PATH"
	fi
}

abspath() {
	case "$1" in
		/*) printf '%s\n' "$1" ;;
		*) printf '%s/%s\n' "$(pwd)" "$1" ;;
	esac
}

cross_file_for_arch() {
	case "$1" in
		x86_64) printf '%s\n' "toolchain/x86_64-elf.ini" ;;
		aarch64) printf '%s\n' "toolchain/aarch64-elf.ini" ;;
		riscv64) printf '%s\n' "toolchain/riscv64-elf.ini" ;;
		loongarch64) printf '%s\n' "toolchain/loongarch64-elf.ini" ;;
		*) error "unsupported architecture: $1" ;;
	esac
}

platform_for_arch() {
	case "$1" in
		x86_64) printf '%s\n' "pc_x86_64" ;;
		aarch64) printf '%s\n' "pc_aarch64" ;;
		riscv64) printf '%s\n' "pc_riscv64" ;;
		loongarch64) printf '%s\n' "pc_loongarch64" ;;
		*) error "unsupported architecture: $1" ;;
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

build_dir_for_arch() {
	local arch="$1"

	if [[ -n "$BUILD_DIR" ]]; then
		printf '%s\n' "$BUILD_DIR"
	else
		printf '%s/%s-%s\n' "$BUILD_ROOT" "$BUILD_PREFIX" "$arch"
	fi
}

setup_arch() {
	local arch="$1"
	local build_dir
	local cross_file
	local platform
	local -a meson_args

	build_dir="$(build_dir_for_arch "$arch")"
	cross_file="$(cross_file_for_arch "$arch")"
	platform="$(platform_for_arch "$arch")"
	meson_args=(
		setup
		"$build_dir"
		"--cross-file" "$cross_file"
		"-Dplatform=${platform}"
		"-Doptimization=${OPTIMIZATION}"
		"-Dtests=$( (( BUILD_TESTS )) && printf true || printf false )"
		"-Dtest_sanitizers=$( (( TEST_SANITIZERS )) && printf true || printf false )"
		"-Dkernel_selftests=$( (( BUILD_KERNEL_SELFTESTS )) && printf true || printf false )"
		"-Dkernel_selftests_autorun=$( (( KERNEL_SELFTESTS_AUTORUN )) && printf true || printf false )"
		"-Dkernel_selftests_suite=${KERNEL_SELFTESTS_SUITE}"
		"-Dkernel_selftests_iommu=${KERNEL_SELFTESTS_IOMMU}"
		"-Dkernel_boot_debug=$( (( KERNEL_BOOT_DEBUG )) && printf true || printf false )"
		"-Dkernel_trace=$( (( KERNEL_TRACE )) && printf true || printf false )"
		"-Dkernel_trace_duration_ms=${KERNEL_TRACE_DURATION_MS}"
		"-Dkernel_trace_buffer_kb=${KERNEL_TRACE_BUFFER_KB}"
		"-Dkernel_trace_transport=${KERNEL_TRACE_TRANSPORT}"
	)

	if (( RECONFIGURE )) || [[ -d "$build_dir" ]]; then
		meson_args=(
			setup
			"$build_dir"
			"--reconfigure"
			"--cross-file" "$cross_file"
			"-Dplatform=${platform}"
			"-Doptimization=${OPTIMIZATION}"
			"-Dtests=$( (( BUILD_TESTS )) && printf true || printf false )"
			"-Dtest_sanitizers=$( (( TEST_SANITIZERS )) && printf true || printf false )"
			"-Dkernel_selftests=$( (( BUILD_KERNEL_SELFTESTS )) && printf true || printf false )"
			"-Dkernel_selftests_autorun=$( (( KERNEL_SELFTESTS_AUTORUN )) && printf true || printf false )"
			"-Dkernel_selftests_suite=${KERNEL_SELFTESTS_SUITE}"
			"-Dkernel_selftests_iommu=${KERNEL_SELFTESTS_IOMMU}"
			"-Dkernel_boot_debug=$( (( KERNEL_BOOT_DEBUG )) && printf true || printf false )"
			"-Dkernel_trace=$( (( KERNEL_TRACE )) && printf true || printf false )"
			"-Dkernel_trace_duration_ms=${KERNEL_TRACE_DURATION_MS}"
			"-Dkernel_trace_buffer_kb=${KERNEL_TRACE_BUFFER_KB}"
			"-Dkernel_trace_transport=${KERNEL_TRACE_TRANSPORT}"
		)
	fi
	if [[ -n "$CRASH_SERVER_URL" ]]; then
		meson_args+=("-Dcrash_server_url=${CRASH_SERVER_URL}")
	fi

	log_arch "$arch" "configuring in ${build_dir}"
	(
		cd "$REPO_ROOT"
		meson "${meson_args[@]}"
	)
}

compile_arch() {
	local arch="$1"
	local build_dir

	build_dir="$(build_dir_for_arch "$arch")"

	[[ -d "$build_dir" ]] || error "build directory not found for ${arch}: ${build_dir}; run with --setup or use --all to configure every target first"

	log_arch "$arch" "compiling from ${build_dir}"
	(
		cd "$REPO_ROOT"
		meson compile -C "$build_dir"
	)
}

run_arch() {
	local arch="$1"

	if (( DO_SETUP )); then
		setup_arch "$arch"
	fi

	if (( DO_COMPILE )); then
		compile_arch "$arch"
	fi
}

run_arches_in_parallel() {
	local -a pids=()
	local -a running_arches=()
	local arch
	local pid
	local idx
	local status
	local failed=0

	for arch in "$@"; do
		(
			run_arch "$arch"
		) &
		pid="$!"
		pids+=( "$pid" )
		running_arches+=( "$arch" )
		log_arch "$arch" "started background job ${pid}"
	done

	for idx in "${!pids[@]}"; do
		pid="${pids[$idx]}"
		arch="${running_arches[$idx]}"

		if wait "$pid"; then
			log_arch "$arch" "finished successfully"
		else
			status=$?
			echo "build.sh: ${arch} failed with exit code ${status}" >&2
			failed=1
		fi
	done

	(( failed == 0 )) || exit 1
}

TARGET_ARCH=""
ALL_ARCHES=0
BUILD_DIR=""
BUILD_ROOT="$REPO_ROOT"
BUILD_PREFIX="build"
DO_SETUP=0
DO_COMPILE=0
RECONFIGURE=0
OPTIMIZATION="0"
BUILD_TESTS=1
TEST_SANITIZERS=0
BUILD_KERNEL_SELFTESTS=0
KERNEL_SELFTESTS_AUTORUN=0
KERNEL_SELFTESTS_SUITE=""
KERNEL_SELFTESTS_IOMMU=""
KERNEL_BOOT_DEBUG=0
KERNEL_TRACE=0
KERNEL_TRACE_DURATION_MS=5000
KERNEL_TRACE_BUFFER_KB=256
KERNEL_TRACE_TRANSPORT="qemu"
CRASH_SERVER_URL=""

while [[ $# -gt 0 ]]; do
	case "$1" in
		--arch)
			[[ $# -ge 2 ]] || error "missing value for --arch, if you want to build every architecture, use --all instead"
			TARGET_ARCH="$2"
			shift 2
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
		--build-root)
			[[ $# -ge 2 ]] || error "missing value for --build-root"
			BUILD_ROOT="$2"
			shift 2
			;;
		--build-root=*)
			BUILD_ROOT="${1#*=}"
			shift
			;;
		--build-prefix)
			[[ $# -ge 2 ]] || error "missing value for --build-prefix"
			BUILD_PREFIX="$2"
			shift 2
			;;
		--build-prefix=*)
			BUILD_PREFIX="${1#*=}"
			shift
			;;
		-s)
			DO_SETUP=1
			shift
			;;
		--setup)
			DO_SETUP=1
			shift
			;;
		-c)
			DO_COMPILE=1
			shift
			;;
		--compile)
			DO_COMPILE=1
			shift
			;;
		-sc)
			DO_SETUP=1
			DO_COMPILE=1
			shift
			;;
		--reconfigure)
			RECONFIGURE=1
			shift
			;;
		--optimization)
			[[ $# -ge 2 ]] || error "missing value for --optimization"
			case "$2" in plain|0|g|1|2|3|s) ;; *) error "unsupported optimization level: $2" ;; esac
			OPTIMIZATION="$2"
			shift 2
			;;
		--optimization=*)
			OPTIMIZATION="${1#*=}"
			case "$OPTIMIZATION" in plain|0|g|1|2|3|s) ;; *) error "unsupported optimization level: $OPTIMIZATION" ;; esac
			shift
			;;
		--no-tests)
			BUILD_TESTS=0
			shift
			;;
		--test-sanitizers)
			TEST_SANITIZERS=1
			shift
			;;
		--kernel-selftests)
			BUILD_KERNEL_SELFTESTS=1
			shift
			;;
		--kernel-selftests-autorun)
			BUILD_KERNEL_SELFTESTS=1
			KERNEL_SELFTESTS_AUTORUN=1
			shift
			;;
		--kernel-selftests-suite)
			[[ $# -ge 2 ]] || error "missing value for --kernel-selftests-suite"
			[[ -n "$2" ]] || error "--kernel-selftests-suite requires a non-empty suite name"
			BUILD_KERNEL_SELFTESTS=1
			KERNEL_SELFTESTS_SUITE="$2"
			shift 2
			;;
		--kernel-selftests-suite=*)
			[[ -n "${1#*=}" ]] || error "--kernel-selftests-suite requires a non-empty suite name"
			BUILD_KERNEL_SELFTESTS=1
			KERNEL_SELFTESTS_SUITE="${1#*=}"
			shift
			;;
		--kernel-selftests-iommu)
			[[ $# -ge 2 ]] || error "missing value for --kernel-selftests-iommu"
			case "$2" in
				none|vtd|amd|smmuv3|riscv|loongarch-v1) ;;
				*) error "unsupported selftest IOMMU kind: $2" ;;
			esac
			BUILD_KERNEL_SELFTESTS=1
			KERNEL_SELFTESTS_IOMMU="$2"
			shift 2
			;;
		--kernel-selftests-iommu=*)
			KERNEL_SELFTESTS_IOMMU="${1#*=}"
			case "$KERNEL_SELFTESTS_IOMMU" in
				none|vtd|amd|smmuv3|riscv|loongarch-v1) ;;
				*) error "unsupported selftest IOMMU kind: $KERNEL_SELFTESTS_IOMMU" ;;
			esac
			BUILD_KERNEL_SELFTESTS=1
			shift
			;;
		--kernel-boot-debug)
			KERNEL_BOOT_DEBUG=1
			shift
			;;
		--kernel-trace)
			KERNEL_TRACE=1
			shift
			;;
		--kernel-trace-duration-ms)
			[[ $# -ge 2 && "$2" =~ ^[1-9][0-9]*$ ]] || error "--kernel-trace-duration-ms requires a positive integer"
			KERNEL_TRACE=1
			KERNEL_TRACE_DURATION_MS="$2"
			shift 2
			;;
		--kernel-trace-buffer-kb)
			[[ $# -ge 2 && "$2" =~ ^[1-9][0-9]*$ ]] || error "--kernel-trace-buffer-kb requires a positive integer"
			KERNEL_TRACE=1
			KERNEL_TRACE_BUFFER_KB="$2"
			shift 2
			;;
		--kernel-trace-transport)
			[[ $# -ge 2 ]] || error "missing value for --kernel-trace-transport"
			case "$2" in qemu|serial) ;; *) error "unsupported kernel trace transport: $2" ;; esac
			KERNEL_TRACE=1
			KERNEL_TRACE_TRANSPORT="$2"
			shift 2
			;;
		--crash-server-url)
			[[ $# -ge 2 ]] || error "missing value for --crash-server-url"
			[[ -n "$2" ]] || error "--crash-server-url requires a non-empty URL"
			CRASH_SERVER_URL="$2"
			shift 2
			;;
		--crash-server-url=*)
			CRASH_SERVER_URL="${1#*=}"
			[[ -n "$CRASH_SERVER_URL" ]] || error "--crash-server-url requires a non-empty URL"
			shift
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

if [[ "$TARGET_ARCH" == "all" ]]; then
	ALL_ARCHES=1
	TARGET_ARCH=""
fi

if (( ALL_ARCHES )) && [[ -n "$TARGET_ARCH" ]]; then
	error "use either --arch <arch> or --all, not both"
fi

if (( ALL_ARCHES )) && [[ -n "$BUILD_DIR" ]]; then
	error "--builddir cannot be combined with --all; use --build-root and/or --build-prefix instead"
fi

if [[ -n "$BUILD_DIR" ]] && [[ "$BUILD_ROOT" != "$REPO_ROOT" ]]; then
	error "use either --builddir or --build-root, not both"
fi

if [[ -n "$BUILD_DIR" ]] && [[ "$BUILD_PREFIX" != "build" ]]; then
	error "use either --builddir or --build-prefix, not both"
fi

[[ -n "$BUILD_PREFIX" ]] || error "--build-prefix requires a non-empty value"

if (( TEST_SANITIZERS )) && (( ! BUILD_TESTS )); then
	error "--test-sanitizers cannot be combined with --no-tests"
fi

BUILD_ROOT="$(abspath "$BUILD_ROOT")"
if [[ -n "$BUILD_DIR" ]]; then
	BUILD_DIR="$(abspath "$BUILD_DIR")"
fi

if (( ! ALL_ARCHES )) && [[ -z "$TARGET_ARCH" ]]; then
	error "missing target selection; use --arch <arch> for one target or --all to setup and compile every architecture"
fi

if (( ! DO_SETUP )) && (( ! DO_COMPILE )); then
	DO_SETUP=1
	DO_COMPILE=1
fi

need_cmd meson

selected_arches=()
if (( ALL_ARCHES )); then
	selected_arches=("${ARCHES[@]}")
else
	validate_arch "$TARGET_ARCH"
	selected_arches=("$TARGET_ARCH")
fi

if (( ALL_ARCHES )); then
	run_arches_in_parallel "${selected_arches[@]}"
else
	for arch in "${selected_arches[@]}"; do
		run_arch "$arch"
	done
fi
