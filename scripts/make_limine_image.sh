#!/usr/bin/env bash
set -euo pipefail

usage() {
	cat <<'EOF'
Usage: make_limine_image.sh --arch <arch> --kernel <path> --config <path> --output <path> --builddir <path>

Required arguments:
  --arch        Limine target architecture (x86_64, aarch64, riscv64, loongarch64).
  --kernel      Path to the kernel ELF binary to embed.
  --config      Path to the limine.conf configuration file.
  --output      Path of the image file to generate.
  --builddir    Meson build directory used for caching helper assets.

Optional arguments:
  --update-limine
                Refresh the cached Limine clone before building.
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
TARGET_ARCH=""
UPDATE_LIMINE=0

while [[ $# -gt 0 ]]; do
	case "$1" in
		--arch)
			TARGET_ARCH="$2"
			shift 2
			;;
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
		--update-limine)
			UPDATE_LIMINE=1
			shift 1
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
[[ -n "$KERNEL_PATH" ]] || error "missing required argument --kernel"
[[ -n "$CONFIG_PATH" ]] || error "missing required argument --config"
[[ -n "$OUTPUT_PATH" ]] || error "missing required argument --output"
[[ -n "$BUILD_DIR" ]] || error "missing required argument --builddir"

[[ -f "$KERNEL_PATH" ]] || error "kernel binary not found: $KERNEL_PATH"
[[ -f "$CONFIG_PATH" ]] || error "limine configuration not found: $CONFIG_PATH"

need_cmd git
need_cmd make
need_cmd xorriso

LIMINE_REPO="https://github.com/limine-bootloader/limine.git"

EFI_BOOT_FILE=""
configure_args=()
required_assets=()
target_clang_triple=""
target_cc_for_target="clang"
target_cflags_for_target=""
target_ld_for_target="ld.lld"
target_objcopy_for_target="llvm-objcopy"
target_objdump_for_target="llvm-objdump"
target_readelf_for_target="llvm-readelf"
target_toolchain_for_target="llvm-"

case "$TARGET_ARCH" in
	x86_64)
		target_clang_triple="x86_64-unknown-none-elf"
		EFI_BOOT_FILE="BOOTX64.EFI"
		required_assets=(
			"limine-bios.sys"
			"limine-bios-cd.bin"
			"limine-uefi-cd.bin"
			"$EFI_BOOT_FILE"
		)
		configure_args=(
			"--enable-bios"
			"--enable-bios-cd"
			"--enable-uefi-x86-64"
			"--enable-uefi-cd"
		)
		;;
	aarch64)
		target_clang_triple="aarch64-unknown-none-elf"
		EFI_BOOT_FILE="BOOTAA64.EFI"
		required_assets=(
			"limine-uefi-cd.bin"
			"$EFI_BOOT_FILE"
		)
		configure_args=(
			"--enable-uefi-aarch64"
			"--enable-uefi-cd"
		)
		;;
	riscv64)
		target_clang_triple="riscv64-unknown-none-elf"
		EFI_BOOT_FILE="BOOTRISCV64.EFI"
		required_assets=(
			"limine-uefi-cd.bin"
			"$EFI_BOOT_FILE"
		)
		configure_args=(
			"--enable-uefi-riscv64"
			"--enable-uefi-cd"
		)
		;;
	loongarch64)
		target_clang_triple="loongarch64-unknown-none-elf"
		EFI_BOOT_FILE="BOOTLOONGARCH64.EFI"
		required_assets=(
			"limine-uefi-cd.bin"
			"$EFI_BOOT_FILE"
		)
		configure_args=(
			"--enable-uefi-loongarch64"
			"--enable-uefi-cd"
		)
		;;
	*)
		error "unsupported architecture: $TARGET_ARCH"
		;;
esac

target_cflags_for_target="--target=${target_clang_triple}"

mkdir -p "$BUILD_DIR"

cache_dir="${BUILD_DIR}/limine-cache"
limine_root="${cache_dir}/limine"
mkdir -p "$cache_dir"

if (( UPDATE_LIMINE )); then
	log "refreshing cached Limine repository at ${limine_root}"
	rm -rf "$limine_root"
fi

if [[ ! -d "${limine_root}/.git" ]]; then
	if [[ -e "$limine_root" ]]; then
		log "removing stale Limine cache at ${limine_root}"
		rm -rf "$limine_root"
	fi
	log "cloning Limine from ${LIMINE_REPO}"
	git clone --depth=1 "$LIMINE_REPO" "$limine_root"
else
	log "using cached Limine repository at ${limine_root}"
fi

deploy_tool=""
limine_bin="${limine_root}/bin"
limine_build_stamp="${limine_root}/.limine-build-stamp"
limine_build_config="${limine_root}/.limine-build-config"
needs_limine_build=0
needs_limine_reconfigure=0

need_cmd "$target_cc_for_target"
need_cmd "$target_ld_for_target"
need_cmd "$target_objcopy_for_target"
need_cmd "$target_objdump_for_target"
need_cmd "$target_readelf_for_target"

limine_target_env=(
	"CC_FOR_TARGET=${target_cc_for_target}"
	"CFLAGS_FOR_TARGET=${target_cflags_for_target}"
	"LD_FOR_TARGET=${target_ld_for_target}"
	"OBJCOPY_FOR_TARGET=${target_objcopy_for_target}"
	"OBJDUMP_FOR_TARGET=${target_objdump_for_target}"
	"READELF_FOR_TARGET=${target_readelf_for_target}"
	"TOOLCHAIN_FOR_TARGET=${target_toolchain_for_target}"
)

limine_build_signature="$(printf '%s\n' \
	"arch=${TARGET_ARCH}" \
	"configure_args=${configure_args[*]}" \
	"cc_for_target=${target_cc_for_target}" \
	"cflags_for_target=${target_cflags_for_target}" \
	"ld_for_target=${target_ld_for_target}" \
	"objcopy_for_target=${target_objcopy_for_target}" \
	"objdump_for_target=${target_objdump_for_target}" \
	"readelf_for_target=${target_readelf_for_target}" \
	"toolchain_for_target=${target_toolchain_for_target}")"

if [[ ! -f "$limine_build_stamp" ]]; then
	needs_limine_build=1
fi

if [[ ! -f "$limine_build_config" ]] || [[ "$(cat "$limine_build_config")" != "$limine_build_signature" ]]; then
	needs_limine_build=1
	needs_limine_reconfigure=1
fi

asset_probe_dir="$limine_root"
if [[ -d "$limine_bin" ]]; then
	asset_probe_dir="$limine_bin"
fi

if (( ! needs_limine_build )); then
	for asset in "${required_assets[@]}"; do
		if [[ ! -f "${asset_probe_dir}/${asset}" ]]; then
			needs_limine_build=1
			break
		fi
	done
fi

if (( needs_limine_reconfigure )); then
	log "invalidating cached Limine artifacts for ${TARGET_ARCH}"
	rm -rf \
		"${limine_root}/bin" \
		"${limine_root}/common-bios" \
		"${limine_root}/common-uefi-x86-64" \
		"${limine_root}/common-uefi-ia32" \
		"${limine_root}/common-uefi-aarch64" \
		"${limine_root}/common-uefi-riscv64" \
		"${limine_root}/common-uefi-loongarch64" \
		"${limine_root}/decompressor-build"
	rm -f \
		"$limine_build_stamp" \
		"$limine_build_config" \
		"${limine_root}/stage1.stamp"
fi

if (( needs_limine_build )); then
	log "preparing Limine build system"
	(
		cd "$limine_root"
		if [[ -x ./bootstrap ]]; then
			./bootstrap
		fi
		if [[ -x ./configure ]]; then
			env "${limine_target_env[@]}" ./configure "${configure_args[@]}"
		fi
	)

	log "building Limine utilities"
	env "${limine_target_env[@]}" make -C "$limine_root"
	printf '%s\n' "$limine_build_signature" > "$limine_build_config"
	touch "$limine_build_stamp"
else
	log "using cached Limine build artifacts"
fi

if [[ "$TARGET_ARCH" == "x86_64" ]]; then
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
fi

asset_dir="$limine_root"
if [[ -d "$limine_bin" ]]; then
	asset_dir="$limine_bin"
fi

find_asset() {
	local asset_name="$1"

	if [[ -f "${asset_dir}/${asset_name}" ]]; then
		printf '%s\n' "${asset_dir}/${asset_name}"
		return 0
	fi

	if [[ -f "${limine_root}/${asset_name}" ]]; then
		printf '%s\n' "${limine_root}/${asset_name}"
		return 0
	fi

	return 1
}

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
for asset in "${required_assets[@]}"; do
	asset_path="$(find_asset "$asset")" || error "expected Limine asset missing: ${asset}"
	case "$asset" in
		BOOT*.EFI)
			cp -f "$asset_path" "${iso_root}/EFI/BOOT/${asset}"
			;;
		*)
			cp -f "$asset_path" "$iso_root/"
			;;
	esac
done

if [[ "$TARGET_ARCH" == "x86_64" ]]; then
	bootia32_path="$(find_asset "BOOTIA32.EFI" || true)"
	if [[ -n "$bootia32_path" ]]; then
		cp -f "$bootia32_path" "${iso_root}/EFI/BOOT/BOOTIA32.EFI"
	fi
fi

log "creating bootable image at $output_abs"
xorriso_cmd=(
	xorriso -as mkisofs
	-R -J -joliet-long -iso-level 3
	-o "$output_abs" "$iso_root"
)

if [[ "$TARGET_ARCH" == "x86_64" ]]; then
	xorriso_cmd=(
		xorriso -as mkisofs
		-R -J -joliet-long -iso-level 3
		-b limine-bios-cd.bin
		-no-emul-boot -boot-load-size 4 -boot-info-table
		-hfsplus -apm-block-size 2048
		--efi-boot limine-uefi-cd.bin
		-efi-boot-part --efi-boot-image --protective-msdos-label
		-o "$output_abs" "$iso_root"
	)
else
	xorriso_cmd=(
		xorriso -as mkisofs
		-R -J -joliet-long -iso-level 3
		-hfsplus -apm-block-size 2048
		--efi-boot limine-uefi-cd.bin
		-efi-boot-part --efi-boot-image --protective-msdos-label
		-o "$output_abs" "$iso_root"
	)
fi

"${xorriso_cmd[@]}"

if [[ "$TARGET_ARCH" == "x86_64" ]]; then
	log "deploying Limine stage files into image"
	(
		cd "$limine_root"
		if [[ "$(basename "$deploy_tool")" = "limine-deploy" ]]; then
			"$deploy_tool" "$output_abs"
		else
			"$deploy_tool" bios-install "$output_abs"
		fi
	)
fi

log "image ready: $output_abs"
