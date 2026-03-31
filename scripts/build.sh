#!/usr/bin/env bash
set -euo pipefail

readonly ARCHES=(
	"x86_64"
	"aarch64"
	"riscv64"
	"loongarch64"
)

usage() {
	cat <<'EOF'
Usage: build.sh (--arch <arch> | --all) [--setup|-s] [--compile|-c] [-sc] [--reconfigure] [--no-tests]

Target selection:
  --arch <arch>  Build a single architecture (x86_64, aarch64, riscv64, loongarch64).
  --all          Build every supported architecture in parallel.

Actions:
  --setup|-s        Configure Meson build directories.
  --compile|-c      Compile the configured build directories.
                    When neither --setup nor --compile is passed, both are run.
  -sc            Short form for running both setup and compile.
  --reconfigure  Force Meson reconfiguration for existing build directories.
  --no-tests     Configure Meson with -Dtests=false.

Examples:
  bash scripts/build.sh --arch x86_64
  bash scripts/build.sh --arch aarch64 --setup
  bash scripts/build.sh --arch riscv64 -sc
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
		x86_64) printf '%s\n' "pc_qemu_x86_64" ;;
		aarch64) printf '%s\n' "pc_qemu_aarch64" ;;
		riscv64) printf '%s\n' "pc_qemu_riscv64" ;;
		loongarch64) printf '%s\n' "pc_qemu_loongarch64" ;;
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

setup_arch() {
	local arch="$1"
	local build_dir="build-${arch}"
	local cross_file
	local platform
	local -a meson_args

	cross_file="$(cross_file_for_arch "$arch")"
	platform="$(platform_for_arch "$arch")"
	meson_args=(
		setup
		"$build_dir"
		"--cross-file" "$cross_file"
		"-Dplatform=${platform}"
		"-Dtests=$( (( BUILD_TESTS )) && printf true || printf false )"
	)

	if (( RECONFIGURE )) || [[ -d "$build_dir" ]]; then
		meson_args=(
			setup
			"$build_dir"
			"--reconfigure"
			"--cross-file" "$cross_file"
			"-Dplatform=${platform}"
			"-Dtests=$( (( BUILD_TESTS )) && printf true || printf false )"
		)
	fi

	log_arch "$arch" "configuring in ${build_dir}"
	meson "${meson_args[@]}"
}

compile_arch() {
	local arch="$1"
	local build_dir="build-${arch}"

	[[ -d "$build_dir" ]] || error "build directory not found for ${arch}: ${build_dir}; run with --setup or use --all to configure every target first"

	log_arch "$arch" "compiling from ${build_dir}"
	meson compile -C "$build_dir"
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
DO_SETUP=0
DO_COMPILE=0
RECONFIGURE=0
BUILD_TESTS=1

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
		--no-tests)
			BUILD_TESTS=0
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
