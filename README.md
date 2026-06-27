# Kernel

## Introduction

This repository is an experimental microkernel project. Kernel code should be the smallest possible layer that provides the necessary primitives for building higher-level services in userspace.

The goal is to have a single codebase that can be built and run on multiple architectures, with a clean separation between reusable kernel logic and platform-specific behavior. The supported targets are `x86_64`, `aarch64`, `riscv64`, and `loongarch64`.

## Build and Run

This project uses Meson to:

- cross-compile the kernel for `x86_64`, `aarch64`, `riscv64`, and `loongarch64`
- build a bootable Limine ISO image
- build and run native Criterion tests

## Project Organization

The tree is split into layers so code can move toward the least specific place that can own it. The boundaries are meant to describe ownership, not a perfect dependency graph.

- `include/`: public headers for the layer contracts. `include/base/`, `include/core/`, `include/hal/`, and `include/kernel/` mirror the implementation layers.
- `base/`: standalone freestanding support code. It currently presents a libc-like API and deliberately leaves some low-level operations to the environment that links it, so the kernel, future userspace, and hosted mocks can provide different implementations. It should not know about boot protocols, CPUs, interrupts, address spaces, or platform devices.
- `core/`: reusable, platform-neutral kernel behavior. Code belongs here when it can be expressed in terms of core data structures and HAL contracts rather than a specific boot path, runtime policy, or architecture.
- `kernel/`: live-kernel composition. Code belongs here when it turns reusable services into one booted kernel image: boot protocol adaptation, initialization order, kernel-wide policy, and integration between services that should otherwise stay independent.
- `platforms/<platform>/`: concrete machine and architecture support. One platform backend is selected per build, and this is where entry code, context-switch details, and HAL implementations live.
- `boot/`: Limine configuration template used when producing the bootable ISO.
- `toolchain/`: Meson cross files for the supported kernel targets.
- `test/`: native Criterion tests plus hosted mocks for dependencies that are supplied by the platform or kernel when the real image is linked.

The top-level Meson file builds the image by collecting `base_sources`, `core_sources`, the selected `platform_kernel_objects`, and `kernel/` integration sources into `kernel.elf`, then packages that ELF into `kernel.iso`.

The HAL is the main contract between reusable kernel code and platform code. Public HAL interfaces live in `include/hal/`; `core` targets those interfaces, while the selected platform supplies the concrete behavior. Supported Meson platforms are `pc_x86_64`, `pc_aarch64`, `pc_riscv64`, and `pc_loongarch64`.

## Memory Model

Physical memory starts with the bootloader memory map. `pmm_init()` records the direct-map offset, reserves allocator metadata from usable memory, and manages contiguous 4 KiB page runs.

Virtual memory is split into two related layers:

- `address_space` in `include/core/vaddr_alloc.h` tracks ownership of virtual page ranges with a bitmap and stores the HAL paging handle for that space.
- `vmm` in `include/core/vmm.h` tracks allocation records by ID, owns backing pages for mapped allocations, applies protection policy, supports lazy mappings, resolves eligible page faults, and releases metadata/backing during teardown.

The kernel has a global managed virtual window at `MM_KERNEL_VMM_BASE` with size `MM_KERNEL_VMM_SIZE`. User processes receive separate address spaces over `MM_USER_VMM_BASE` and `MM_USER_VMM_SIZE`, with a null guard at the bottom. New hardware user address spaces inherit the kernel mappings required to enter and leave kernel mode.

The address-transfer helpers validate user ranges, fault in lazy pages when requested, and copy data between kernel and user address spaces without syscalls reaching directly into untrusted pointers.

## Execution Model

The scheduler is per-CPU. Each CPU owns a run queue and a permanent idle thread. Regular threads carry a kernel stack, an address-space pointer, priority state, wait queue links, join state, cancellation flags, and an owner kind.

Higher-level thread wrappers add ownership:

- `kthread` allocates a kernel stack from the kernel VMM window and runs a kernel entry point.
- `uthread` allocates both a user stack in its process address space and a kernel stack in the kernel address space, then uses the HAL userspace contract to build an initial user context.
- `process` owns a user address space and links its user threads for lifecycle operations.

The scheduler switches address spaces when the next thread requires a different one, falling back to the kernel address space for idle/kernel-only execution.

## Testing Model

Hosted Criterion tests are native executables. They link `base` or `core` sources with mocks instead of the real platform backend:

- generic HAL mocks live in `test/mocks/hal/`
- generic base mocks live in `test/mocks/base/`
- subsystem-specific mocks stay next to the tests that need them, such as `test/vmm/mock_paging.c`

This is why `core` can depend on HAL contracts and still remain testable outside the live kernel. In-kernel selftests under `kernel/test/` cover behavior that must run after boot-time memory and scheduler initialization.

## Syscall Results

On success, the value is the syscall result. On error, the value is a syscall/status-specific diagnostic payload, such as the unsupported syscall number or the index of a problematic argument.

Status values are grouped by range:

- `0xx`: success. `SYSCALL_STATUS_OK` is `0`.
- `1xx`: caller-side errors. The syscall request was malformed or unsupported, so retrying it unchanged should not work.
- `2xx`: kernel-side errors. The request was understood, but the kernel could not complete it because required state or resources were unavailable.

Current codes:

- `SYSCALL_STATUS_OK` (`0`): the syscall completed successfully.
- `SYSCALL_STATUS_UNKNOWN_SYSCALL` (`100`): the syscall number is not implemented.
- `SYSCALL_STATUS_BAD_ARGUMENT` (`101`): an argument is invalid or cannot be represented by the kernel.
- `SYSCALL_STATUS_FAILED` (`200`): the request is valid, but the kernel operation failed while handling it.
- `SYSCALL_STATUS_UNAVAILABLE` (`201`): the syscall depends on kernel state that is not available yet.

Use `syscall_status_is_success()`, `syscall_status_is_caller_error()`, and `syscall_status_is_kernel_error()` when callers only care about the result class.

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
bash scripts/build.sh --all --setup --build-prefix build-debug
bash scripts/build.sh --arch x86_64 --setup --no-tests
bash scripts/build.sh --arch x86_64 --setup --test-sanitizers
bash scripts/build.sh --arch x86_64 --setup --kernel-selftests
bash scripts/build.sh --arch x86_64 --setup --kernel-selftests --kernel-selftests-autorun
bash scripts/build.sh --arch x86_64 --setup --kernel-selftests-suite vmm
```

When no architecture is provided, the helper exits with guidance to use either `--arch <arch>` or `--all`.
Use `--no-tests` if you want a kernel-only configure without native test dependencies installed.
Use `--test-sanitizers` to run native tests with AddressSanitizer and UndefinedBehaviorSanitizer instrumentation.
Use `--kernel-selftests` to compile in-kernel selftest suites into the kernel.
Use `--kernel-selftests-autorun` to also inject `kernel.selftest=1` into the generated image so that booting the ISO runs the in-kernel tests automatically.
Use `--kernel-selftests-suite <name>` to inject `kernel.selftest.suite=<name>` into the generated image and boot only that suite. This also enables selftest autorun for the generated image.
Use `--builddir <path>` for a custom single-architecture build directory.
Use `--build-root <path>` and/or `--build-prefix <name>` for per-architecture directories; for example, `--build-prefix build-debug` produces `build-debug-x86_64`, `build-debug-aarch64`, `build-debug-riscv64`, and `build-debug-loongarch64`.

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
bash scripts/build.sh --arch x86_64 --builddir build-debug-x86_64
bash scripts/build.sh --arch x86_64 --kernel-selftests --kernel-selftests-autorun
bash scripts/build.sh --arch x86_64 --kernel-selftests-suite vmm
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
bash scripts/run.sh --test --builddir build-debug-x86_64
bash scripts/run.sh --kernel-selftest --arch x86_64
```

If you want to launch QEMU for every supported target, the helper does that sequentially in one command:

```sh
bash scripts/run.sh --all
```

If you use a non-standard build directory, override it explicitly for a single-architecture run:

```sh
bash scripts/run.sh --arch x86_64 --builddir out/kernel-x86_64
bash scripts/run.sh --kernel-selftest --arch x86_64 --builddir build-debug-x86_64
```

If you use matching per-architecture build directories, select them with the same root/prefix options used by `scripts/build.sh`:

```sh
bash scripts/run.sh --all --build-prefix build-debug
bash scripts/run.sh --arch riscv64 --build-root /tmp/kernel-builds --build-prefix build-debug
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

If you only want a specific in-kernel selftest suite:

```sh
bash scripts/build.sh --arch x86_64 --kernel-selftests-suite vmm --no-tests
bash scripts/run.sh --kernel-selftest --arch x86_64 --timeout 45
```

If you only want a specific hosted Criterion test suite:

```sh
bash scripts/run.sh -t --test-name pmm
```

You can still use the narrower QEMU-only helper directly if you want:

```sh
bash scripts/run_qemu.sh --arch x86_64
bash scripts/run_qemu.sh --arch aarch64
bash scripts/run_qemu.sh --arch riscv64
bash scripts/run_qemu.sh --arch loongarch64
bash scripts/run_qemu.sh --arch x86_64 --builddir build-debug-x86_64
```

## In-Kernel Selftests

The hosted Criterion tests under `test/` remain the right place for fast native unit coverage. For checks that must run inside the live kernel after boot-time memory initialization, there is now a small in-kernel selftest runner.

Build-time control uses three options:

- `--kernel-selftests` compiles the in-kernel selftest suites into the kernel binary
- `--kernel-selftests-autorun` writes `kernel.selftest=1` into the generated image command line so they run automatically on boot
- `--kernel-selftests-suite <name>` writes `kernel.selftest.suite=<name>` into the generated image command line and limits autorun to that suite

The kernel runs selftests from a selftest-only runner after `pmm`, `vmm`, `heap`, and scheduler initialization, prints per-test results to serial, and emits a final `selftest: result: PASS` or `FAIL` marker for automation.
When a suite filter is present, only the matching registered suite is executed.

To add more in-kernel tests:

- define one or more `struct kernel_selftest_case` entries in a new source file under `kernel/test/selftests/`
- export a `const struct kernel_selftest_suite`
- register that suite in [`kernel/test/selftest.c`](/c:/Users/anger/Code/kernel/new/kernel/test/selftest.c)

Use the assertion macros in [`kernel/test/selftest.h`](/c:/Users/anger/Code/kernel/new/kernel/test/selftest.h). For tests that allocate or lock resources, prefer the `_GOTO` variants so cleanup still runs on failure.

## Notes

- The default platform is `pc_x86_64`.
- The available Meson platforms are `pc_x86_64`, `pc_aarch64`, `pc_riscv64`, and `pc_loongarch64`.
- These platform names describe the kernel target rather than the emulator; the same binaries are intended to be usable on matching hardware, while `scripts/run.sh` and `scripts/run_qemu.sh` remain QEMU launch helpers.
- The test binaries are built natively, while the kernel is cross-compiled with the selected file in `toolchain/`.
- `-Dtests=false` skips configuring the native Criterion test targets. `scripts/build.sh --no-tests` is the helper equivalent.
- `-Dtest_sanitizers=true` instruments native tests with AddressSanitizer and UndefinedBehaviorSanitizer. `scripts/build.sh --test-sanitizers` is the helper equivalent.
- `-Dkernel_selftests=true` compiles in-kernel selftest code into the kernel. `scripts/build.sh --kernel-selftests` is the helper equivalent.
- `-Dkernel_selftests_autorun=true` injects `kernel.selftest=1` into the generated image. `scripts/build.sh --kernel-selftests-autorun` is the helper equivalent.
- `-Dkernel_selftests_suite=<name>` injects `kernel.selftest.suite=<name>` into the generated image and also causes selftests to autorun for that image. `scripts/build.sh --kernel-selftests-suite <name>` is the helper equivalent.
- `-Dkernel_boot_debug=true` injects `loglevel=debug` into the generated image and enables verbose boot diagnostics. `scripts/build.sh --kernel-boot-debug` is the helper equivalent.
- `scripts/build.sh` supports `--arch <arch>` for one target and `--all` to configure and/or compile every supported target in one parallel run.
- `scripts/build.sh`, `scripts/run.sh`, and `scripts/run_qemu.sh` support `--builddir <path>` for single-target custom build directories, plus `--build-root <path>` and `--build-prefix <name>` for matching per-architecture directory sets.
- `scripts/run.sh` has three modes: test mode with `--test`/`-t`, kernel selftest mode with `--kernel-selftest`, and QEMU mode otherwise.
- The Limine helper script clones Limine into the build directory the first time the ISO target is built.
- The `virt` machines for non-`x86_64` targets need explicit edk2 firmware. `scripts/run.sh` and `scripts/run_qemu.sh` locate the matching firmware image automatically, or you can point one of them at it with `QEMU_FIRMWARE_DIR`.
- The non-`x86_64` targets are UEFI-only in this repository; BIOS ISO deployment remains `x86_64`-only.
- A repo-managed pre-commit hook template lives in `.githooks/pre-commit`. Install it into your local `.git/hooks` with `bash scripts/install-hooks.sh`. It formats staged `*.c` and `*.h` files before commit and aborts if one of them is only partially staged.
