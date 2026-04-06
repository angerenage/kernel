#!/usr/bin/env bash
set -euo pipefail

usage() {
	cat <<'EOF'
Usage: run_qemu.sh --arch <arch> [--builddir <path>] [--headless] [--debug] [--debug-port <port>] [-- <extra qemu args>]

Required arguments:
  --arch        Target architecture (x86_64, aarch64, riscv64, loongarch64).

Optional arguments:
  --builddir    Meson build directory containing kernel.iso.
                Defaults to build-<arch>.
  --headless    Disable graphical display and keep serial on stdio.
  --debug       Start QEMU paused with a GDB stub on localhost and enable
                QEMU debug logging in the build directory.
  --debug-port  TCP port to use for the GDB stub in --debug mode.
                Defaults to 1234.

Optional environment:
  QEMU_FIRMWARE_DIR
                Directory containing edk2 firmware images. When unset, the
                script searches common locations relative to the QEMU binary.
EOF
}

error() {
	echo "run_qemu.sh: $*" >&2
	exit 1
}

abspath() {
	case "$1" in
		/*) printf '%s\n' "$1" ;;
		*) printf '%s/%s\n' "$(pwd)" "$1" ;;
	esac
}

to_native_path() {
	local path="$1"
	local drive=""
	local rest=""

	if command -v cygpath >/dev/null 2>&1; then
		cygpath -w "$path"
		return 0
	fi

	case "$path" in
		/mnt/[a-zA-Z]/*)
			drive="${path#/mnt/}"
			drive="${drive%%/*}"
			rest="${path#/mnt/${drive}/}"
			rest="${rest//\//\\}"
			printf '%s:\\%s\n' "${drive^^}" "$rest"
			return 0
			;;
		/[a-zA-Z]/*)
			drive="${path#/}"
			drive="${drive%%/*}"
			rest="${path#/${drive}/}"
			rest="${rest//\//\\}"
			printf '%s:\\%s\n' "${drive^^}" "$rest"
			return 0
			;;
		*)
			printf '%s\n' "$path"
			return 0
			;;
	esac
}

resolve_cmd() {
	local base_name="$1"

	if command -v "$base_name" >/dev/null 2>&1; then
		command -v "$base_name"
		return 0
	fi

	if command -v "${base_name}.exe" >/dev/null 2>&1; then
		command -v "${base_name}.exe"
		return 0
	fi

	return 1
}

default_builddir_for_arch() {
	case "$1" in
		x86_64|aarch64|riscv64|loongarch64)
			printf 'build-%s\n' "$1"
			;;
		*)
			error "unsupported architecture: $1"
			;;
	esac
}

find_firmware() {
	local qemu_bin="$1"
	local qemu_dir
	local candidate
	local -a search_dirs=()
	local firmware_name

	shift
	qemu_dir="$(cd "$(dirname "$qemu_bin")" && pwd)"

	if [[ -n "${QEMU_FIRMWARE_DIR:-}" ]]; then
		search_dirs+=("$QEMU_FIRMWARE_DIR")
	fi

	search_dirs+=(
		"${qemu_dir}/share"
		"${qemu_dir}/../share"
		/usr/share/AAVMF
		/usr/share/qemu-efi-aarch64
		/usr/share/qemu-efi-riscv64
		/usr/share/qemu-efi-loongarch64
		/usr/share/qemu
		/usr/share
		/usr/local/share/qemu
		/usr/local/share
		/mingw64/share/qemu
		/mingw64/share
	)

	for firmware_name in "$@"; do
		for candidate_dir in "${search_dirs[@]}"; do
			candidate="${candidate_dir}/${firmware_name}"
			if [[ -f "$candidate" ]]; then
				printf '%s\n' "$candidate"
				return 0
			fi
		done
	done

	return 1
}

TARGET_ARCH=""
BUILD_DIR=""
DEBUG_MODE=0
DEBUG_PORT=1234
HEADLESS=0
EXTRA_ARGS=()

while [[ $# -gt 0 ]]; do
	case "$1" in
		--arch)
			TARGET_ARCH="$2"
			shift 2
			;;
		--builddir)
			BUILD_DIR="$2"
			shift 2
			;;
		--debug)
			DEBUG_MODE=1
			shift 1
			;;
		--headless)
			HEADLESS=1
			shift 1
			;;
		--debug-port)
			DEBUG_PORT="$2"
			shift 2
			;;
		--)
			shift
			EXTRA_ARGS=("$@")
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

[[ -n "$TARGET_ARCH" ]] || error "missing required argument --arch"

if [[ -z "$BUILD_DIR" ]]; then
	BUILD_DIR="$(default_builddir_for_arch "$TARGET_ARCH")"
fi

BUILD_DIR="$(abspath "$BUILD_DIR")"
ISO_PATH="${BUILD_DIR}/kernel.iso"
[[ -f "$ISO_PATH" ]] || error "kernel ISO not found: $ISO_PATH (use --builddir <path> if you are not using the default build-${TARGET_ARCH} directory)"
DEBUG_LOG_PATH="${BUILD_DIR}/qemu-${TARGET_ARCH}.debug.log"

firmware_names=()
firmware_vars_names=()
qemu_name=""
qemu_bin=""
qemu_args=()

case "$TARGET_ARCH" in
	x86_64)
		qemu_name="qemu-system-x86_64"
		qemu_args=(
			-M q35
			-cdrom "$ISO_PATH"
			-serial stdio
			-no-reboot
			-no-shutdown
		)
		;;
	aarch64)
		qemu_name="qemu-system-aarch64"
		firmware_names=("edk2-aarch64-code.fd" "AAVMF_CODE.fd" "QEMU_EFI.fd")
		qemu_args=(
			-M virt
			-cpu cortex-a72
			-m 2G
			-device virtio-gpu-pci
			-device qemu-xhci
			-device usb-kbd
			-device usb-mouse
			-cdrom "$ISO_PATH"
			-serial stdio
			-no-reboot
			-no-shutdown
		)
		;;
	riscv64)
		qemu_name="qemu-system-riscv64"
		firmware_names=("edk2-riscv-code.fd" "RISCV_VIRT_CODE.fd")
		firmware_vars_names=("edk2-riscv-vars.fd" "RISCV_VIRT_VARS.fd")
		qemu_args=(
			-M virt,acpi=off,pflash0=pflash0,pflash1=pflash1
			-cpu rv64
			-m 2G
			-device virtio-gpu-pci
			-device qemu-xhci
			-device usb-kbd
			-device usb-mouse
			-cdrom "$ISO_PATH"
			-serial stdio
			-no-reboot
			-no-shutdown
		)
		;;
	loongarch64)
		qemu_name="qemu-system-loongarch64"
		firmware_names=("edk2-loongarch64-code.fd" "LOONGARCH_VIRT_CODE.fd" "QEMU_EFI.fd")
		qemu_args=(
			-M virt
			-cpu la464
			-m 2G
			-device virtio-gpu-pci
			-device qemu-xhci
			-device usb-kbd
			-device usb-mouse
			-cdrom "$ISO_PATH"
			-serial stdio
			-no-reboot
			-no-shutdown
		)
		;;
	*)
		error "unsupported architecture: $TARGET_ARCH"
		;;
esac

qemu_bin="$(resolve_cmd "$qemu_name")" || error "required tool '$qemu_name' not found in PATH"

if (( ${#firmware_names[@]} > 0 )); then
	firmware_path="$(find_firmware "$qemu_bin" "${firmware_names[@]}")" || error \
		"firmware '${firmware_names[*]}' not found; set QEMU_FIRMWARE_DIR or install the edk2 firmware package"

	if [[ "$TARGET_ARCH" == "riscv64" ]]; then
		firmware_vars_template="$(find_firmware "$qemu_bin" "${firmware_vars_names[@]}")" || error \
			"firmware '${firmware_vars_names[*]}' not found; set QEMU_FIRMWARE_DIR or install the edk2 firmware package"
		firmware_vars_path="${BUILD_DIR}/${firmware_vars_names[0]}"
		if [[ ! -f "$firmware_vars_path" ]]; then
			cat "$firmware_vars_template" > "$firmware_vars_path"
		fi
		chmod u+w "$firmware_vars_path" 2>/dev/null || true
		qemu_args=(
			-blockdev "node-name=pflash0,driver=file,read-only=on,filename=${firmware_path}"
			-blockdev "node-name=pflash1,driver=file,filename=${firmware_vars_path}"
			"${qemu_args[@]}"
		)
	else
		qemu_args=(-bios "$firmware_path" "${qemu_args[@]}")
	fi
fi

if (( DEBUG_MODE )); then
	qemu_args+=(
		-S
		-gdb "tcp:127.0.0.1:${DEBUG_PORT}"
		-d guest_errors,int,cpu_reset
		-D "$DEBUG_LOG_PATH"
	)
fi

if (( HEADLESS )); then
	qemu_args+=(-display none)
fi

if [[ "$qemu_bin" == *.exe ]]; then
	ISO_PATH="$(to_native_path "$ISO_PATH")"
	for ((i = 0; i < ${#qemu_args[@]}; i++)); do
		if [[ "${qemu_args[i]}" == "-cdrom" ]]; then
			qemu_args[i + 1]="$ISO_PATH"
			break
		fi
	done

	if (( ${#firmware_names[@]} > 0 )); then
		firmware_path="$(to_native_path "$firmware_path")"
		if [[ "$TARGET_ARCH" == "riscv64" ]]; then
			firmware_vars_path="$(to_native_path "$firmware_vars_path")"
			qemu_args[1]="node-name=pflash0,driver=file,read-only=on,filename=${firmware_path}"
			qemu_args[3]="node-name=pflash1,driver=file,filename=${firmware_vars_path}"
		else
			qemu_args[1]="$firmware_path"
		fi
	fi

	if (( DEBUG_MODE )); then
		DEBUG_LOG_PATH="$(to_native_path "$DEBUG_LOG_PATH")"
		for ((i = 0; i < ${#qemu_args[@]}; i++)); do
			if [[ "${qemu_args[i]}" == "-D" ]]; then
				qemu_args[i + 1]="$DEBUG_LOG_PATH"
				break
			fi
		done
	fi
fi

if (( DEBUG_MODE )); then
	echo "run_qemu.sh: debug mode enabled (gdb stub on tcp:127.0.0.1:${DEBUG_PORT}, log: ${DEBUG_LOG_PATH})" >&2
fi

exec "$qemu_bin" "${qemu_args[@]}" "${EXTRA_ARGS[@]}"
