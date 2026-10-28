#!/usr/bin/env python3

import argparse
import bisect
import json
import os
from pathlib import Path
import re
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
import zlib


MAGIC = 0x313045434152544B
VERSION = 1
MAX_CPUS = 64
FLAG_FINALIZED = 1 << 0
FLAG_VIRTUAL_TSC = 1 << 1

RECORD_INSTANT = 0
RECORD_BEGIN = 1
RECORD_END = 2

EVENT_SCHED_SWITCH = 1
EVENT_SCHED_BLOCK = 2
EVENT_SCHED_WAKE = 3
EVENT_SYSCALL = 4
EVENT_CAP_CALL_PHASE = 5
EVENT_CAP_CALL_HANDLER = 6
EVENT_CAP_CALL_OPERATION = 7
EVENT_SEMAPHORE_PHASE = 8
EVENT_VM_PHASE = 9
EVENT_SCHED_PHASE = 10
EVENT_VM_TLB_RANGE = 11
EVENT_VM_BACKING_WRITE = 12

HEADER = struct.Struct("<QIIQQIIQIIQ")
CPU_DESCRIPTOR = struct.Struct("<QQQQ")
RECORD = struct.Struct("<QQQIHBB")
CHECKSUM_OFFSET = 56

SYSCALL_NAMES = (
    "nop",
    "yield",
    "sleep_ms",
    "tick_count",
    "monotonic_time_ns",
    "self",
    "exit_process",
    "exit_thread",
    "create_process",
    "channel_create",
    "channel_destroy",
    "channel_event_recv",
    "signal_create",
    "signal_send",
    "signal_send_coalesced",
    "signal_read",
    "signal_wait",
    "signal_try_wait",
    "cap_create",
    "cap_delegate",
    "cap_derive",
    "cap_call",
    "cap_reply",
    "cap_revoke",
    "cap_recv",
    "cap_valid",
    "cap_drop",
    "cap_unpublish",
    "upcall_dropped_count",
    "upcall_return",
)

BLOCK_REASONS = (
    "none",
    "wait_queue",
    "semaphore",
    "signal",
    "mutex",
    "rwlock",
    "join",
    "sleep",
    "parked",
)

CAP_CALL_PHASE_NAMES = (
    "cap_acquire",
    "request_alloc",
    "request_copy",
    "response_validate",
    "begin_call",
    "kernel_response_alloc",
    "kernel_handler",
    "kernel_response_copy",
    "endpoint_retain",
    "pending_create",
    "request_attach",
    "channel_enqueue",
    "reply_wait",
    "pending_destroy",
    "wait_try_acquire",
    "wait_block",
    "wait_result",
)

CAP_CALL_OPERATION_NAMES = (
    "process_info",
    "process_run",
    "process_wait",
    "process_detach",
    "process_kill",
    "process_spawn_thread",
    "mapping_info",
    "mapping_protect",
    "mapping_unmap",
    "mapping_sync",
    "memory_info",
    "memory_slice",
)

SEMAPHORE_PHASE_NAMES = (
    "check",
    "queue_lock",
    "sched_block",
    "cancel_check",
)

VM_PHASE_NAMES = (
    "mapping_cap_acquire",
    "mapping_address_space_unmap",
    "mapping_object_destroy",
    "address_space_hal_unmap",
    "address_space_remove",
    "address_space_release",
    "paging_prepare",
    "paging_change",
    "paging_shootdown",
    "paging_commit",
    "paging_change_record",
    "paging_change_descend",
    "paging_change_empty_scan",
    "paging_change_retire",
    "tlb_shootdown_lock",
    "tlb_shootdown_local",
    "tlb_shootdown_targets",
    "tlb_shootdown_send",
    "tlb_shootdown_wait",
    "backing_materialize",
)

SCHED_PHASE_NAMES = (
    "block_prepare",
    "block_enqueue",
    "block_cancel_check",
    "block_queue_unlock",
    "dispatch_finish_previous",
    "dispatch_select",
    "dispatch_address_space",
    "dispatch_switch",
    "switch_fp_save",
    "switch_fp_restore",
    "switch_io_bitmap",
    "address_space_lock",
    "address_space_track",
    "address_space_cr3",
)


class TraceError(Exception):
    pass


class SymbolTable:
    def __init__(self, addresses, names):
        self.addresses = addresses
        self.names = names

    def resolve(self, address):
        index = bisect.bisect_right(self.addresses, address) - 1
        if index < 0:
            return f"0x{address:x}"
        offset = address - self.addresses[index]
        if offset == 0:
            return self.names[index]
        return f"{self.names[index]}+0x{offset:x}"


def load_symbols(kernel_elf):
    if kernel_elf is None:
        return None
    tool = shutil.which("llvm-nm") or shutil.which("nm")
    if tool is None:
        raise TraceError("llvm-nm or nm is required to symbolize kernel handlers")
    result = subprocess.run(
        [tool, "-n", "--defined-only", str(kernel_elf)],
        check=True,
        capture_output=True,
        text=True,
    )
    symbols = []
    for line in result.stdout.splitlines():
        fields = line.split(maxsplit=2)
        if len(fields) != 3:
            continue
        try:
            address = int(fields[0], 16)
        except ValueError:
            continue
        symbols.append((address, fields[2]))
    if not symbols:
        raise TraceError(f"no symbols found in {kernel_elf}")
    return SymbolTable(
        [symbol[0] for symbol in symbols], [symbol[1] for symbol in symbols]
    )


def indexed_name(names, index, prefix):
    return names[index] if index < len(names) else f"{prefix}_{index}"


def parse_trace(path):
    data = bytearray(Path(path).read_bytes())
    fixed_header_size = HEADER.size + MAX_CPUS * CPU_DESCRIPTOR.size
    if len(data) < fixed_header_size:
        raise TraceError("trace is shorter than its fixed header")

    (
        magic,
        version,
        header_size,
        total_size,
        frequency,
        record_size,
        cpu_count,
        session_id,
        flags,
        _reserved,
        checksum,
    ) = HEADER.unpack_from(data)
    if (
        magic != MAGIC
        or version != VERSION
        or header_size < fixed_header_size
        or total_size != len(data)
        or frequency == 0
        or record_size != RECORD.size
        or not 0 < cpu_count <= MAX_CPUS
        or not flags & FLAG_FINALIZED
    ):
        raise TraceError("invalid or unsupported trace header")

    struct.pack_into("<Q", data, CHECKSUM_OFFSET, 0)
    actual_checksum = zlib.crc32(data) & 0xFFFFFFFF
    struct.pack_into("<Q", data, CHECKSUM_OFFSET, checksum)
    if checksum != actual_checksum:
        raise TraceError(
            f"checksum mismatch: header={checksum:08x}, actual={actual_checksum:08x}"
        )

    records = []
    dropped = 0
    for cpu in range(cpu_count):
        descriptor_offset = HEADER.size + cpu * CPU_DESCRIPTOR.size
        offset, capacity, committed, cpu_dropped = CPU_DESCRIPTOR.unpack_from(
            data, descriptor_offset
        )
        if (
            committed > capacity
            or offset > len(data)
            or capacity > (len(data) - offset) // RECORD.size
        ):
            raise TraceError(f"invalid CPU {cpu} descriptor")
        dropped += cpu_dropped
        for index in range(committed):
            record = RECORD.unpack_from(data, offset + index * RECORD.size)
            if record[4] >= cpu_count:
                raise TraceError(f"record references invalid CPU {record[4]}")
            records.append(record)

    records.sort(key=lambda record: (record[0], record[4]))
    return {
        "frequency": frequency,
        "session_id": session_id,
        "flags": flags,
        "cpu_count": cpu_count,
        "dropped": dropped,
        "records": records,
    }


def decode_trace(input_path, output_path, kernel_elf=None):
    trace = parse_trace(input_path)
    symbols = load_symbols(kernel_elf)
    records = trace["records"]
    frequency = trace["frequency"]
    origin = records[0][0] if records else 0
    maximum = records[-1][0] if records else origin
    thread_ids = {}
    cpu_seen = [False] * trace["cpu_count"]

    for _timestamp, arg0, arg1, event_id, _cpu, _record_type, _flags in records:
        referenced_threads = (arg0, arg1) if event_id == EVENT_SCHED_SWITCH else (arg0,)
        for raw in referenced_threads:
            if raw != 0 and raw not in thread_ids:
                thread_ids[raw] = len(thread_ids) + 1

    def timestamp_us(ticks, start=origin):
        return (ticks - start) * 1_000_000.0 / frequency

    def thread_number(raw):
        if raw == 0:
            return 0
        return thread_ids[raw]

    def metadata(name, pid, display_name, tid=0):
        return {
            "ph": "M",
            "name": name,
            "pid": pid,
            "tid": tid,
            "args": {"name": display_name},
        }

    output = Path(output_path)
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("w", encoding="utf-8") as stream:
        stream.write('{"displayTimeUnit":"ns","traceEvents":[')
        first = True

        def emit(event):
            nonlocal first
            if not first:
                stream.write(",")
            first = False
            json.dump(event, stream, separators=(",", ":"))

        emit(metadata("process_name", 0, "kernel CPUs"))
        emit(metadata("process_name", 1, "kernel threads"))
        for cpu in range(trace["cpu_count"]):
            emit(metadata("thread_name", 0, f"CPU {cpu}", 1000 + cpu))
        for raw, tid in thread_ids.items():
            emit(metadata("thread_name", 1, f"kernel thread {tid} (0x{raw:x})", tid))
        emit(
            {
                "ph": "i",
                "s": "g",
                "name": "trace metadata",
                "pid": 0,
                "tid": 0,
                "ts": 0,
                "args": {
                    "session": f'{trace["session_id"]:x}',
                    "dropped_records": trace["dropped"],
                    "clock_frequency": frequency,
                    "virtual_tsc": bool(trace["flags"] & FLAG_VIRTUAL_TSC),
                },
            }
        )

        for timestamp, arg0, arg1, event_id, cpu, record_type, _flags in records:
            if event_id == EVENT_SCHED_SWITCH:
                if cpu_seen[cpu]:
                    emit(
                        {
                            "ph": "E",
                            "cat": "scheduler",
                            "pid": 0,
                            "tid": 1000 + cpu,
                            "ts": timestamp_us(timestamp),
                        }
                    )
                cpu_seen[cpu] = True
                emit(
                    {
                        "ph": "B",
                        "cat": "scheduler",
                        "name": f"thread {thread_number(arg1)}",
                        "pid": 0,
                        "tid": 1000 + cpu,
                        "ts": timestamp_us(timestamp),
                        "args": {"thread": f"0x{arg1:x}"},
                    }
                )
                continue

            tid = thread_number(arg0)
            if event_id in (EVENT_SCHED_BLOCK, EVENT_SCHED_WAKE):
                emit(
                    {
                        "ph": "i",
                        "s": "t",
                        "cat": "scheduler",
                        "name": "block" if event_id == EVENT_SCHED_BLOCK else "wake",
                        "pid": 1,
                        "tid": tid,
                        "ts": timestamp_us(timestamp),
                        "args": {
                            "reason": indexed_name(BLOCK_REASONS, arg1, "reason")
                        },
                    }
                )
            elif event_id == EVENT_SYSCALL and record_type == RECORD_BEGIN:
                emit(
                    {
                        "ph": "B",
                        "cat": "syscall",
                        "name": indexed_name(SYSCALL_NAMES, arg1, "syscall"),
                        "pid": 1,
                        "tid": tid,
                        "ts": timestamp_us(timestamp),
                    }
                )
            elif event_id == EVENT_SYSCALL and record_type == RECORD_END:
                emit(
                    {
                        "ph": "E",
                        "cat": "syscall",
                        "pid": 1,
                        "tid": tid,
                        "ts": timestamp_us(timestamp),
                        "args": {"status": arg1},
                    }
                )
            elif event_id == EVENT_CAP_CALL_PHASE and record_type == RECORD_BEGIN:
                emit(
                    {
                        "ph": "B",
                        "cat": "cap_call",
                        "name": indexed_name(CAP_CALL_PHASE_NAMES, arg1, "phase"),
                        "pid": 1,
                        "tid": tid,
                        "ts": timestamp_us(timestamp),
                    }
                )
            elif event_id == EVENT_CAP_CALL_PHASE and record_type == RECORD_END:
                emit(
                    {
                        "ph": "E",
                        "cat": "cap_call",
                        "pid": 1,
                        "tid": tid,
                        "ts": timestamp_us(timestamp),
                    }
                )
            elif event_id == EVENT_CAP_CALL_HANDLER and record_type == RECORD_BEGIN:
                handler_name = symbols.resolve(arg1) if symbols is not None else f"handler 0x{arg1:x}"
                emit(
                    {
                        "ph": "B",
                        "cat": "cap_handler",
                        "name": handler_name,
                        "pid": 1,
                        "tid": tid,
                        "ts": timestamp_us(timestamp),
                        "args": {"address": f"0x{arg1:x}"},
                    }
                )
            elif event_id == EVENT_CAP_CALL_HANDLER and record_type == RECORD_END:
                emit(
                    {
                        "ph": "E",
                        "cat": "cap_handler",
                        "pid": 1,
                        "tid": tid,
                        "ts": timestamp_us(timestamp),
                    }
                )
            elif event_id == EVENT_CAP_CALL_OPERATION and record_type == RECORD_BEGIN:
                emit(
                    {
                        "ph": "B",
                        "cat": "cap_operation",
                        "name": indexed_name(CAP_CALL_OPERATION_NAMES, arg1, "operation"),
                        "pid": 1,
                        "tid": tid,
                        "ts": timestamp_us(timestamp),
                    }
                )
            elif event_id == EVENT_CAP_CALL_OPERATION and record_type == RECORD_END:
                emit(
                    {
                        "ph": "E",
                        "cat": "cap_operation",
                        "pid": 1,
                        "tid": tid,
                        "ts": timestamp_us(timestamp),
                    }
                )
            elif event_id == EVENT_SEMAPHORE_PHASE and record_type == RECORD_BEGIN:
                emit(
                    {
                        "ph": "B",
                        "cat": "semaphore",
                        "name": indexed_name(SEMAPHORE_PHASE_NAMES, arg1, "phase"),
                        "pid": 1,
                        "tid": tid,
                        "ts": timestamp_us(timestamp),
                    }
                )
            elif event_id == EVENT_SEMAPHORE_PHASE and record_type == RECORD_END:
                emit(
                    {
                        "ph": "E",
                        "cat": "semaphore",
                        "pid": 1,
                        "tid": tid,
                        "ts": timestamp_us(timestamp),
                    }
                )
            elif event_id == EVENT_VM_PHASE and record_type == RECORD_BEGIN:
                emit(
                    {
                        "ph": "B",
                        "cat": "vm",
                        "name": indexed_name(VM_PHASE_NAMES, arg1, "phase"),
                        "pid": 1,
                        "tid": tid,
                        "ts": timestamp_us(timestamp),
                    }
                )
            elif event_id == EVENT_VM_PHASE and record_type == RECORD_END:
                emit(
                    {
                        "ph": "E",
                        "cat": "vm",
                        "pid": 1,
                        "tid": tid,
                        "ts": timestamp_us(timestamp),
                    }
                )
            elif event_id == EVENT_SCHED_PHASE and record_type == RECORD_BEGIN:
                emit(
                    {
                        "ph": "B",
                        "cat": "sched",
                        "name": indexed_name(SCHED_PHASE_NAMES, arg1, "phase"),
                        "pid": 1,
                        "tid": tid,
                        "ts": timestamp_us(timestamp),
                    }
                )
            elif event_id == EVENT_SCHED_PHASE and record_type == RECORD_END:
                emit(
                    {
                        "ph": "E",
                        "cat": "sched",
                        "pid": 1,
                        "tid": tid,
                        "ts": timestamp_us(timestamp),
                    }
                )
            elif event_id == EVENT_VM_TLB_RANGE:
                emit(
                    {
                        "ph": "i",
                        "s": "t",
                        "cat": "vm",
                        "name": "tlb_invalidation_range",
                        "pid": 1,
                        "tid": tid,
                        "ts": timestamp_us(timestamp),
                        "args": {"pages": arg1},
                    }
                )
            elif event_id == EVENT_VM_BACKING_WRITE:
                emit(
                    {
                        "ph": "i",
                        "s": "t",
                        "cat": "vm",
                        "name": "backing_write",
                        "pid": 1,
                        "tid": tid,
                        "ts": timestamp_us(timestamp),
                        "args": {"bytes": arg1},
                    }
                )
            else:
                emit(
                    {
                        "ph": "i",
                        "s": "t",
                        "cat": "kernel",
                        "name": f"event_{event_id}",
                        "pid": 1,
                        "tid": tid,
                        "ts": timestamp_us(timestamp),
                        "args": {"arg1": arg1},
                    }
                )

        for cpu in range(trace["cpu_count"]):
            if cpu_seen[cpu]:
                emit(
                    {
                        "ph": "E",
                        "cat": "scheduler",
                        "pid": 0,
                        "tid": 1000 + cpu,
                        "ts": timestamp_us(maximum),
                    }
                )
        stream.write("]}\n")

    print(
        f"ktrace: wrote {output} ({len(records)} records, {trace['dropped']} dropped)"
    )


SERIAL_MARKER = re.compile(
    rb"KTRACE_STREAM_BEGIN version=(\d+) session=([0-9a-fA-F]+) "
    rb"size=(\d+) crc32=([0-9a-fA-F]+)[^\n]*\n"
)


def extract_serial(input_path, output_path):
    data = Path(input_path).read_bytes()
    match = SERIAL_MARKER.search(data)
    if match is None or int(match.group(1)) != VERSION:
        raise TraceError("no supported serial trace marker found")
    size = int(match.group(3))
    payload = data[match.end() : match.end() + size]
    if len(payload) != size:
        raise TraceError("serial capture ended inside the trace payload")
    output = Path(output_path)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(payload)
    print(
        f"ktrace: extracted session {int(match.group(2), 16):x} to {output} "
        f"(marker CRC {int(match.group(4), 16):08x})"
    )


READY_MARKER = re.compile(
    rb"^KTRACE_READY .*phys=0x([0-9a-fA-F]+) size=(\d+).*$", re.MULTILINE
)


def terminate_process(process):
    if process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()


def qmp_command(stream, execute, arguments=None):
    request_id = execute
    request = {"execute": execute, "id": request_id}
    if arguments is not None:
        request["arguments"] = arguments
    stream.write(json.dumps(request, separators=(",", ":")).encode() + b"\r\n")
    stream.flush()
    while True:
        line = stream.readline()
        if not line:
            raise TraceError("QEMU monitor disconnected")
        response = json.loads(line)
        if response.get("id") != request_id:
            continue
        if "error" in response:
            raise TraceError(f"QEMU monitor rejected {execute}: {response['error']}")
        return response.get("return")


def capture_qemu(output_path, timeout, cwd, command, kernel_elf=None):
    if command and command[0] == "--":
        command = command[1:]
    if not command:
        raise TraceError("capture-qemu requires a command after --")
    if command.count("@QMP_SOCKET@") != 1:
        raise TraceError("capture command must contain one @QMP_SOCKET@ argument")
    if timeout <= 0:
        raise TraceError("capture timeout must be positive")

    output = Path(output_path).resolve()
    base = output.with_suffix("")
    raw_path = Path(f"{base}.ktrace")
    log_path = Path(f"{base}.serial.log")
    if any(character in str(raw_path) for character in ('"', "\\", "\n", "\r")):
        raise TraceError("output path contains characters unsupported by QEMU pmemsave")
    output.parent.mkdir(parents=True, exist_ok=True)
    raw_path.unlink(missing_ok=True)

    process = None
    with tempfile.TemporaryDirectory(prefix="kernel-ktrace-") as temporary:
        qmp_path = str(Path(temporary) / "qmp.sock")
        command = [qmp_path if part == "@QMP_SOCKET@" else part for part in command]
        try:
            with log_path.open("wb") as log:
                process = subprocess.Popen(
                    command,
                    cwd=cwd,
                    stdout=log,
                    stderr=subprocess.STDOUT,
                )
                deadline = time.monotonic() + timeout
                marker = None
                while time.monotonic() < deadline:
                    log.flush()
                    marker = READY_MARKER.search(log_path.read_bytes())
                    if marker is not None:
                        break
                    if process.poll() is not None:
                        raise TraceError("QEMU exited before producing KTRACE_READY")
                    time.sleep(0.1)
                if marker is None:
                    raise TraceError(
                        f"timed out after {timeout:g}s waiting for KTRACE_READY"
                    )

                physical = int(marker.group(1), 16)
                size = int(marker.group(2))
                with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
                    connection.settimeout(5)
                    connection.connect(qmp_path)
                    stream = connection.makefile("rwb", buffering=0)
                    greeting = json.loads(stream.readline())
                    if "QMP" not in greeting:
                        raise TraceError("invalid QEMU monitor greeting")
                    qmp_command(stream, "qmp_capabilities")
                    qmp_command(stream, "stop")
                    qmp_command(
                        stream,
                        "human-monitor-command",
                        {
                            "command-line": (
                                f'pmemsave 0x{physical:x} {size} "{raw_path}"'
                            )
                        },
                    )
                    qmp_command(stream, "quit")
                process.wait(timeout=10)
        finally:
            if process is not None:
                terminate_process(process)
            if log_path.exists():
                sys.stdout.write(log_path.read_text(encoding="utf-8", errors="replace"))

    if not raw_path.is_file():
        raise TraceError(f"QEMU did not create the raw trace: {raw_path}")
    decode_trace(raw_path, output, kernel_elf)
    print(f"ktrace: raw trace: {raw_path}")
    print(f"ktrace: Perfetto JSON: {output}")
    print(f"ktrace: serial log: {log_path}")


def parse_args():
    parser = argparse.ArgumentParser(description="Capture and decode kernel traces.")
    subparsers = parser.add_subparsers(dest="operation", required=True)

    decode = subparsers.add_parser("decode", help="convert a binary trace to Perfetto JSON")
    decode.add_argument("input")
    decode.add_argument("output")
    decode.add_argument("--kernel-elf", help="unstripped kernel ELF used for symbolization")

    serial = subparsers.add_parser(
        "extract-serial", help="extract a binary trace from a serial capture"
    )
    serial.add_argument("input")
    serial.add_argument("output")

    capture = subparsers.add_parser(
        "capture-qemu", help="run QEMU, retrieve a completed trace, and decode it"
    )
    capture.add_argument("--output", required=True)
    capture.add_argument("--timeout", type=float, default=30)
    capture.add_argument("--cwd", default=os.getcwd())
    capture.add_argument("--kernel-elf", help="unstripped kernel ELF used for symbolization")
    capture.add_argument("command", nargs=argparse.REMAINDER)
    return parser.parse_args()


def main():
    args = parse_args()
    try:
        if args.operation == "decode":
            decode_trace(args.input, args.output, args.kernel_elf)
        elif args.operation == "extract-serial":
            extract_serial(args.input, args.output)
        elif args.operation == "capture-qemu":
            capture_qemu(
                args.output, args.timeout, args.cwd, args.command, args.kernel_elf
            )
    except (OSError, ValueError, TraceError, subprocess.SubprocessError) as error:
        print(f"ktrace: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
