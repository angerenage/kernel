# Kernel Build and Run

This project uses Meson to:

- cross-compile the kernel for `x86_64`, `aarch64`, `riscv64`, and `loongarch64`
- build a bootable Limine ISO image
- build and run native Criterion tests

## Prerequisites

Make sure these tools are available on your `PATH`:

- `meson`
- `ninja`
- `clang`, `lld`, `llvm-ar`, `llvm-strip`
- `qemu-system-x86_64`
- `qemu-system-aarch64`
- `qemu-system-riscv64`
- `qemu-system-loongarch64`
- `pkg-config`
- Criterion (`criterion`)
- `git`
- `bash`
- `make`
- `xorriso`

On Windows, the ISO generation step depends on Unix-style tools (`bash`, `make`, `xorriso`). In practice, this project is easiest to build from an MSYS2 or similar shell environment with those tools installed.

For the non-`x86_64` targets, QEMU also needs UEFI firmware. The bundled filenames vary by package, so the recommended launch path is `scripts/run_qemu.sh`, which auto-detects the firmware near the installed QEMU binary or uses `QEMU_FIRMWARE_DIR` if you set it explicitly.

## Configure

The recommended entry point is the build helper:

```sh
bash scripts/build.sh --arch x86_64 --setup
bash scripts/build.sh --all --setup
```

When no architecture is provided, the helper exits with guidance to use either `--arch <arch>` or `--all`.

If you prefer calling Meson directly, configure the architecture you want to build from the repository root:

```sh
meson setup build-x86_64 --cross-file toolchain/x86_64-elf.ini -Dplatform=pc_qemu_x86_64
meson setup build-aarch64 --cross-file toolchain/aarch64-elf.ini -Dplatform=pc_qemu_aarch64
meson setup build-riscv64 --cross-file toolchain/riscv64-elf.ini -Dplatform=pc_qemu_riscv64
meson setup build-loongarch64 --cross-file toolchain/loongarch64-elf.ini -Dplatform=pc_qemu_loongarch64
```

If the build directory already exists and you want to reconfigure it:

```sh
meson setup build-x86_64 --reconfigure --cross-file toolchain/x86_64-elf.ini -Dplatform=pc_qemu_x86_64
meson setup build-aarch64 --reconfigure --cross-file toolchain/aarch64-elf.ini -Dplatform=pc_qemu_aarch64
meson setup build-riscv64 --reconfigure --cross-file toolchain/riscv64-elf.ini -Dplatform=pc_qemu_riscv64
meson setup build-loongarch64 --reconfigure --cross-file toolchain/loongarch64-elf.ini -Dplatform=pc_qemu_loongarch64
```

## Compile

To configure and compile in one command:

```sh
bash scripts/build.sh --arch x86_64
bash scripts/build.sh --arch riscv64 -sc
bash scripts/build.sh --all
bash scripts/build.sh --all -sc
```

To compile only already-configured build directories:

```sh
bash scripts/build.sh --arch x86_64 --compile
bash scripts/build.sh --all --compile
```

If you prefer calling Meson directly, build everything for the configured target:

```sh
meson compile -C build-x86_64
meson compile -C build-aarch64
meson compile -C build-riscv64
meson compile -C build-loongarch64
```

This produces:

- `build-x86_64/kernel/kernel.elf`
- `build-x86_64/kernel.iso`
- `build-aarch64/kernel/kernel.elf`
- `build-aarch64/kernel.iso`
- `build-riscv64/kernel/kernel.elf`
- `build-riscv64/kernel.iso`
- `build-loongarch64/kernel/kernel.elf`
- `build-loongarch64/kernel.iso`

## Launch the Kernel

You can launch the kernel in QEMU with the generated ISO using the helper script:

```sh
bash scripts/run_qemu.sh --arch x86_64
bash scripts/run_qemu.sh --arch aarch64
bash scripts/run_qemu.sh --arch riscv64
bash scripts/run_qemu.sh --arch loongarch64
```

If you use a non-standard build directory, override it explicitly:

```sh
bash scripts/run_qemu.sh --arch x86_64 --builddir out/kernel-x86_64
```

If you want to launch QEMU in debug mode, you can simply use the `--debug` flag and use `--debug-port` to specify a custom port if you don't want to use the default `1234`:

```sh
meson test -C build-x86_64 --debug --debug-port 4321
```

## Run the Tests

Run all tests:

```sh
meson test -C build-x86_64 --print-errorlogs
```

Run only the `early_alloc` suite:

```sh
meson test -C build-x86_64 early_alloc --print-errorlogs
```

## Notes

- The default platform is `pc_qemu_x86_64`.
- The available Meson platforms are `pc_qemu_x86_64`, `pc_qemu_aarch64`, `pc_qemu_riscv64`, and `pc_qemu_loongarch64`.
- The test binaries are built natively, while the kernel is cross-compiled with the selected file in `toolchain/`.
- `scripts/build.sh` supports `--arch <arch>` for one target and `--all` to configure and/or compile every supported target in one run.
- The Limine helper script clones Limine into the build directory the first time the ISO target is built.
- The `virt` machines for non-`x86_64` targets need explicit edk2 firmware. `scripts/run_qemu.sh` locates the matching firmware image automatically, or you can point it at one with `QEMU_FIRMWARE_DIR`.
- The non-`x86_64` targets are UEFI-only in this repository; BIOS ISO deployment remains `x86_64`-only.
- The non-`x86_64` serial HAL is currently a stub, so those builds are expected to render through the framebuffer first rather than print early serial logs.
