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
- `git`
- `bash`
- `make`
- `xorriso`

If you want to build and run the native test targets, you also need Criterion (`criterion`).

On Windows, the ISO generation step depends on Unix-style tools (`bash`, `make`, `xorriso`). In practice, this project is easiest to build from an MSYS2 or similar shell environment with those tools installed.

For the non-`x86_64` targets, QEMU also needs UEFI firmware. The bundled filenames vary by package, so the recommended launch path is `scripts/run.sh` or `scripts/run_qemu.sh`, which auto-detect the firmware near the installed QEMU binary or use `QEMU_FIRMWARE_DIR` if you set it explicitly.

## Configure

The recommended entry point is the build helper:

```sh
bash scripts/build.sh --arch x86_64 --setup
bash scripts/build.sh --all --setup
bash scripts/build.sh --arch x86_64 --setup --no-tests
bash scripts/build.sh --arch x86_64 --setup --kernel-selftests
bash scripts/build.sh --arch x86_64 --setup --kernel-selftests --kernel-selftests-autorun
```

When no architecture is provided, the helper exits with guidance to use either `--arch <arch>` or `--all`.
Use `--no-tests` if you want a kernel-only configure without native test dependencies installed.
Use `--kernel-selftests` to compile in-kernel selftest suites into the kernel.
Use `--kernel-selftests-autorun` to also inject `kernel.selftest=1` into the generated image so that booting the ISO runs the in-kernel tests automatically.

If you prefer calling Meson directly, configure the architecture you want to build from the repository root:

```sh
meson setup build-x86_64 --cross-file toolchain/x86_64-elf.ini -Dplatform=pc_x86_64
meson setup build-aarch64 --cross-file toolchain/aarch64-elf.ini -Dplatform=pc_aarch64
meson setup build-riscv64 --cross-file toolchain/riscv64-elf.ini -Dplatform=pc_riscv64
meson setup build-loongarch64 --cross-file toolchain/loongarch64-elf.ini -Dplatform=pc_loongarch64
```

If the build directory already exists and you want to reconfigure it:

```sh
meson setup build-x86_64 --reconfigure --cross-file toolchain/x86_64-elf.ini -Dplatform=pc_x86_64
meson setup build-aarch64 --reconfigure --cross-file toolchain/aarch64-elf.ini -Dplatform=pc_aarch64
meson setup build-riscv64 --reconfigure --cross-file toolchain/riscv64-elf.ini -Dplatform=pc_riscv64
meson setup build-loongarch64 --reconfigure --cross-file toolchain/loongarch64-elf.ini -Dplatform=pc_loongarch64
```

## Compile

To configure and compile in one command:

```sh
bash scripts/build.sh --arch x86_64
bash scripts/build.sh --arch riscv64 -sc
bash scripts/build.sh --arch x86_64 --kernel-selftests --kernel-selftests-autorun
bash scripts/build.sh --all
bash scripts/build.sh --all -sc
```

When you use `--all`, the helper runs each architecture in parallel. Setup still happens before compile within each individual target.

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

## Run Tests and Launch QEMU

The broader helper is `scripts/run.sh`. In normal mode it launches QEMU and requires either `--arch <arch>` or `--all`. In test mode (`--test` or `-t`) it detects the current machine architecture with `uname -m` and runs the matching native tests.

```sh
bash scripts/run.sh --arch x86_64
bash scripts/run.sh --arch aarch64
bash scripts/run.sh --arch riscv64
bash scripts/run.sh --arch loongarch64
bash scripts/run.sh --test
bash scripts/run.sh -t
bash scripts/run.sh --kernel-selftest --arch x86_64
```

If you want to launch QEMU for every supported target, the helper does that sequentially in one command:

```sh
bash scripts/run.sh --all
```

If you use a non-standard build directory, override it explicitly for a single-architecture run:

```sh
bash scripts/run.sh --arch x86_64 --builddir out/kernel-x86_64
```

If you want to launch QEMU in debug mode, use `--debug` and optionally `--debug-port`:

```sh
bash scripts/run.sh --arch x86_64 --debug --debug-port 4321
```

If you want a headless non-interactive selftest run that exits with success or failure, use:

```sh
bash scripts/build.sh --arch x86_64 --kernel-selftests --kernel-selftests-autorun --no-tests
bash scripts/run.sh --kernel-selftest --arch x86_64 --timeout 45
```

If you only want a specific test suite:

```sh
bash scripts/run.sh -t --test-name pmm
```

You can still use the narrower QEMU-only helper directly if you want:

```sh
bash scripts/run_qemu.sh --arch x86_64
bash scripts/run_qemu.sh --arch aarch64
bash scripts/run_qemu.sh --arch riscv64
bash scripts/run_qemu.sh --arch loongarch64
```

## In-Kernel Selftests

The hosted Criterion tests under `test/` remain the right place for fast native unit coverage. For checks that must run inside the live kernel after boot-time memory initialization, there is now a small in-kernel selftest runner.

Build-time control is split into two flags:

- `--kernel-selftests` compiles the in-kernel selftest suites into the kernel binary
- `--kernel-selftests-autorun` writes `kernel.selftest=1` into the generated image command line so they run automatically on boot

The kernel runs selftests immediately after `pmm`, `vmm`, and `kheap` initialization, prints per-test results to serial, and emits a final `kernel: selftests result: PASS` or `FAIL` marker for automation.

The first in-kernel example is a `kheap` smoke test that:

- allocates two blocks from the real kernel heap
- checks alignment and free-space accounting
- frees and reallocates to verify block reuse
- restores the heap state before continuing

To add more in-kernel tests:

- define one or more `struct kernel_selftest_case` entries in a new source file under `kernel/test/selftests/`
- export a `const struct kernel_selftest_suite`
- register that suite in [`kernel/test/selftest.c`](/c:/Users/anger/Code/kernel/new/kernel/test/selftest.c)

Use the assertion macros in [`include/kernel/selftest.h`](/c:/Users/anger/Code/kernel/new/include/kernel/selftest.h). For tests that allocate or lock resources, prefer the `_GOTO` variants so cleanup still runs on failure.

## Notes

- The default platform is `pc_x86_64`.
- The available Meson platforms are `pc_x86_64`, `pc_aarch64`, `pc_riscv64`, and `pc_loongarch64`.
- These platform names describe the kernel target rather than the emulator; the same binaries are intended to be usable on matching hardware, while `scripts/run.sh` and `scripts/run_qemu.sh` remain QEMU launch helpers.
- The test binaries are built natively, while the kernel is cross-compiled with the selected file in `toolchain/`.
- `-Dtests=false` skips configuring the native Criterion test targets. `scripts/build.sh --no-tests` is the helper equivalent.
- `-Dkernel_selftests=true` compiles in-kernel selftest code into the kernel. `scripts/build.sh --kernel-selftests` is the helper equivalent.
- `-Dkernel_selftests_autorun=true` injects `kernel.selftest=1` into the generated image. `scripts/build.sh --kernel-selftests-autorun` is the helper equivalent.
- `scripts/build.sh` supports `--arch <arch>` for one target and `--all` to configure and/or compile every supported target in one parallel run.
- `scripts/run.sh` has three modes: test mode with `--test`/`-t`, kernel selftest mode with `--kernel-selftest`, and QEMU mode otherwise.
- The Limine helper script clones Limine into the build directory the first time the ISO target is built.
- The `virt` machines for non-`x86_64` targets need explicit edk2 firmware. `scripts/run.sh` and `scripts/run_qemu.sh` locate the matching firmware image automatically, or you can point one of them at it with `QEMU_FIRMWARE_DIR`.
- The non-`x86_64` targets are UEFI-only in this repository; BIOS ISO deployment remains `x86_64`-only.
- A repo-managed pre-commit hook template lives in `.githooks/pre-commit`. Install it into your local `.git/hooks` with `bash scripts/install-hooks.sh`. It formats staged `*.c` and `*.h` files before commit and aborts if one of them is only partially staged.
