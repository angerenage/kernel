#!/usr/bin/env bash
set -euo pipefail

usage() {
	cat <<'EOF'
Usage: run_qemu.sh --arch <arch> --builddir <path> [-- <extra qemu args>]

Required arguments:
  --arch        Target architecture (x86_64, aarch64, riscv64, loongarch64).
  --builddir    Meson build directory containing kernel.iso.

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

find_firmware() {
	local qemu_bin="$1"
	local firmware_name="$2"
	local qemu_dir
	local candidate
	local -a search_dirs=()

	qemu_dir="$(cd "$(dirname "$qemu_bin")" && pwd)"

	if [[ -n "${QEMU_FIRMWARE_DIR:-}" ]]; then
		search_dirs+=("$QEMU_FIRMWARE_DIR")
	fi

	search_dirs+=(
		"${qemu_dir}/share"
		"${qemu_dir}/../share"
		/usr/share/qemu
		/usr/share
		/usr/local/share/qemu
		/usr/local/share
		/mingw64/share/qemu
		/mingw64/share
	)

	for candidate_dir in "${search_dirs[@]}"; do
		candidate="${candidate_dir}/${firmware_name}"
		if [[ -f "$candidate" ]]; then
			printf '%s\n' "$candidate"
			return 0
		fi
	done

	return 1
}

TARGET_ARCH=""
BUILD_DIR=""
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
[[ -n "$BUILD_DIR" ]] || error "missing required argument --builddir"

BUILD_DIR="$(abspath "$BUILD_DIR")"
ISO_PATH="${BUILD_DIR}/kernel.iso"
[[ -f "$ISO_PATH" ]] || error "kernel ISO not found: $ISO_PATH"

firmware_name=""
firmware_vars_name=""
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
		firmware_name="edk2-aarch64-code.fd"
		qemu_args=(
			-M virt
			-cpu cortex-a72
			-m 2G
			-device ramfb
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
		firmware_name="edk2-riscv-code.fd"
		firmware_vars_name="edk2-riscv-vars.fd"
		qemu_args=(
			-M virt,acpi=off,pflash0=pflash0,pflash1=pflash1
			-cpu rv64
			-m 2G
			-device ramfb
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
		firmware_name="edk2-loongarch64-code.fd"
		qemu_args=(
			-M virt
			-cpu la464
			-m 2G
			-device ramfb
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

if [[ -n "$firmware_name" ]]; then
	firmware_path="$(find_firmware "$qemu_bin" "$firmware_name")" || error \
		"firmware '${firmware_name}' not found; set QEMU_FIRMWARE_DIR or install the edk2 firmware package"

	if [[ "$TARGET_ARCH" == "riscv64" ]]; then
		firmware_vars_template="$(find_firmware "$qemu_bin" "$firmware_vars_name")" || error \
			"firmware '${firmware_vars_name}' not found; set QEMU_FIRMWARE_DIR or install the edk2 firmware package"
		firmware_vars_path="${BUILD_DIR}/${firmware_vars_name}"
		qemu_args=(
			-blockdev "node-name=pflash0,driver=file,read-only=on,filename=${firmware_path}"
			-blockdev "node-name=pflash1,driver=file,filename=${firmware_vars_path}"
			"${qemu_args[@]}"
		)
	else
		qemu_args=(-bios "$firmware_path" "${qemu_args[@]}")
	fi
fi

if [[ "$qemu_bin" == *.exe ]]; then
	ISO_PATH="$(to_native_path "$ISO_PATH")"
	for ((i = 0; i < ${#qemu_args[@]}; i++)); do
		if [[ "${qemu_args[i]}" == "-cdrom" ]]; then
			qemu_args[i + 1]="$ISO_PATH"
			break
		fi
	done

	if [[ -n "$firmware_name" ]]; then
		firmware_path="$(to_native_path "$firmware_path")"
		if [[ "$TARGET_ARCH" == "riscv64" ]]; then
			firmware_vars_path="$(to_native_path "$firmware_vars_path")"
			qemu_args[1]="node-name=pflash0,driver=file,read-only=on,filename=${firmware_path}"
			qemu_args[3]="node-name=pflash1,driver=file,filename=${firmware_vars_path}"
		else
			qemu_args[1]="$firmware_path"
		fi
	fi
fi

exec "$qemu_bin" "${qemu_args[@]}" "${EXTRA_ARGS[@]}"
