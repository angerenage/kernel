#!/usr/bin/env bash
set -euo pipefail

usage() {
	cat <<'EOF'
Usage: make_limine_image.sh --kernel <path> --config <path> --output <path> --builddir <path>

Required arguments:
  --kernel      Path to the kernel ELF binary to embed.
  --config      Path to the limine.conf configuration file.
  --output      Path of the image file to generate.
  --builddir    Meson build directory used for caching helper assets.

Optional arguments:
  -h, --help    Show this help and exit.
EOF
}

error() {
	echo "make_limine_image.sh: $*" >&2
	exit 1
}

log() {
	echo "[limine] $*"
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

KERNEL_PATH=""
CONFIG_PATH=""
OUTPUT_PATH=""
BUILD_DIR=""

while [[ $# -gt 0 ]]; do
	case "$1" in
		--kernel)
			KERNEL_PATH="$2"
			shift 2
			;;
		--config)
			CONFIG_PATH="$2"
			shift 2
			;;
		--output)
			OUTPUT_PATH="$2"
			shift 2
			;;
		--builddir)
			BUILD_DIR="$2"
			shift 2
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

[[ -n "$KERNEL_PATH" ]] || error "missing required argument --kernel"
[[ -n "$CONFIG_PATH" ]] || error "missing required argument --config"
[[ -n "$OUTPUT_PATH" ]] || error "missing required argument --output"
[[ -n "$BUILD_DIR" ]] || error "missing required argument --builddir"

[[ -f "$KERNEL_PATH" ]] || error "kernel binary not found: $KERNEL_PATH"
[[ -f "$CONFIG_PATH" ]] || error "limine configuration not found: $CONFIG_PATH"

need_cmd git
need_cmd make
need_cmd xorriso
need_cmd nasm

LIMINE_REPO="https://github.com/limine-bootloader/limine.git"

mkdir -p "$BUILD_DIR"

cache_dir="${BUILD_DIR}/limine-cache"
limine_root="${cache_dir}/limine"
mkdir -p "$cache_dir"

rm -rf "$limine_root"
log "cloning latest Limine from ${LIMINE_REPO}"
git clone --depth=1 "$LIMINE_REPO" "$limine_root"

log "preparing Limine build system"
(
	cd "$limine_root"
	if [[ -x ./bootstrap ]]; then
		./bootstrap
	fi
	./configure \
		--enable-bios \
		--enable-bios-cd \
		--enable-uefi-x86-64 \
		--enable-uefi-cd
)

log "building Limine utilities"
make -C "$limine_root"

deploy_tool=""
limine_bin="${limine_root}/bin"
if [[ -x "${limine_bin}/limine-deploy" ]]; then
	deploy_tool="${limine_bin}/limine-deploy"
elif [[ -x "${limine_root}/limine-deploy" ]]; then
	deploy_tool="${limine_root}/limine-deploy"
elif [[ -x "${limine_bin}/limine" ]]; then
	deploy_tool="${limine_bin}/limine"
elif [[ -x "${limine_root}/limine" ]]; then
	deploy_tool="${limine_root}/limine"
else
	error "no Limine deployment utility found after build (expected limine or limine-deploy)"
fi

required_assets=(
	"limine-bios.sys"
	"limine-bios-cd.bin"
	"limine-uefi-cd.bin"
)

asset_dir="$limine_root"
if [[ -d "$limine_bin" ]]; then
	asset_dir="$limine_bin"
fi

for asset in "${required_assets[@]}"; do
	[[ -f "${asset_dir}/${asset}" ]] || error "expected Limine asset missing: ${asset_dir}/${asset}"
done

output_dirname="$(dirname "$OUTPUT_PATH")"
mkdir -p "$output_dirname"

kernel_abs="$(abspath "$KERNEL_PATH")"
config_abs="$(abspath "$CONFIG_PATH")"
output_abs="$(abspath "$OUTPUT_PATH")"

work_dir="${BUILD_DIR}/limine-image"
iso_root="${work_dir}/iso_root"
rm -rf "$iso_root"
mkdir -p "$iso_root"
mkdir -p "$iso_root/EFI/BOOT"

log "copying kernel and configuration into staging area"
cp -f "$kernel_abs" "${iso_root}/kernel.elf"
cp -f "$config_abs" "${iso_root}/limine.conf"

log "copying Limine support files"
cp -f "${asset_dir}/limine-bios.sys" "$iso_root/"
cp -f "${asset_dir}/limine-bios-cd.bin" "$iso_root/"
cp -f "${asset_dir}/limine-uefi-cd.bin" "$iso_root/"

efi_dir="$asset_dir"
if [[ -f "${limine_bin}/BOOTX64.EFI" ]]; then
	efi_dir="$limine_bin"
elif [[ -f "${limine_root}/BOOTX64.EFI" ]]; then
	efi_dir="$limine_root"
fi

if [[ -f "${efi_dir}/BOOTX64.EFI" ]]; then
	cp -f "${efi_dir}/BOOTX64.EFI" "${iso_root}/EFI/BOOT/BOOTX64.EFI"
else
	log "warning: BOOTX64.EFI missing from Limine archive; UEFI boot may not work"
fi
if [[ -f "${efi_dir}/BOOTIA32.EFI" ]]; then
	cp -f "${efi_dir}/BOOTIA32.EFI" "${iso_root}/EFI/BOOT/BOOTIA32.EFI"
fi

log "creating bootable image at $output_abs"
xorriso -as mkisofs \
	-R -J -joliet-long -iso-level 3 \
	-b limine-bios-cd.bin \
	-no-emul-boot -boot-load-size 4 -boot-info-table \
	--efi-boot limine-uefi-cd.bin \
	-efi-boot-part --efi-boot-image --protective-msdos-label \
	-o "$output_abs" "$iso_root"

log "deploying Limine stage files into image"
(
	cd "$limine_root"
	if [[ "$(basename "$deploy_tool")" = "limine-deploy" ]]; then
		"$deploy_tool" "$output_abs"
	else
		"$deploy_tool" bios-install "$output_abs"
	fi
)

log "image ready: $output_abs"
