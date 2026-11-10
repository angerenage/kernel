#!/usr/bin/env bash
set -euo pipefail

usage() {
	cat <<'EOF'
Usage: remove-unused-includes.sh [options]

Remove includes that clang-tidy's misc-include-cleaner check reports as unused
in every discovered compilation of a project C source.

Options:
  --root <dir>               Repository root (default: .)
  --build-dir <dir>          Compilation database directory (repeatable).
                             By default, discover all databases below <root>.
  --clang-tidy <path>        clang-tidy binary (default: clang-tidy)
  --run-clang-tidy <path>    run-clang-tidy helper (default: run-clang-tidy)
  --jobs <count>             Number of parallel checks (default: 1)
  --dry-run                  Report unused includes without modifying files
  -h, --help                 Show this help and exit

Notes:
- Third-party and test sources are excluded.
- If no compilation databases are found, cleanup is skipped successfully.
- An include is removed only when every discovered compilation of its source
  file reports it unused. This preserves configuration-dependent includes.
- Missing-include diagnostics are disabled; this script only removes includes.
- Project <libc/...> headers are preserved because clang's standard-library
  mappings do not recognize the project's prefixed header spellings. Headers
  used only by conditional lock-debug code are also preserved.
EOF
}

error() {
	echo "remove-unused-includes.sh: $*" >&2
	exit 1
}

log() {
	echo "[include-cleanup] $*"
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

escape_regex() {
	printf '%s' "$1" | sed 's/[][(){}.^$*+?|\\]/\\&/g'
}

ROOT="."
BUILD_DIRS=()
CLANG_TIDY_BIN="clang-tidy"
RUN_CLANG_TIDY_BIN="run-clang-tidy"
JOBS=1
DRY_RUN=0

while [[ $# -gt 0 ]]; do
	case "$1" in
		--root)
			[[ $# -ge 2 ]] || error "--root requires an argument"
			ROOT="$2"
			shift 2
			;;
		--root=*)
			ROOT="${1#*=}"
			shift
			;;
		--build-dir)
			[[ $# -ge 2 ]] || error "--build-dir requires an argument"
			BUILD_DIRS+=("$2")
			shift 2
			;;
		--build-dir=*)
			BUILD_DIRS+=("${1#*=}")
			shift
			;;
		--clang-tidy)
			[[ $# -ge 2 ]] || error "--clang-tidy requires an argument"
			CLANG_TIDY_BIN="$2"
			shift 2
			;;
		--clang-tidy=*)
			CLANG_TIDY_BIN="${1#*=}"
			shift
			;;
		--run-clang-tidy)
			[[ $# -ge 2 ]] || error "--run-clang-tidy requires an argument"
			RUN_CLANG_TIDY_BIN="$2"
			shift 2
			;;
		--run-clang-tidy=*)
			RUN_CLANG_TIDY_BIN="${1#*=}"
			shift
			;;
		--jobs)
			[[ $# -ge 2 ]] || error "--jobs requires an argument"
			JOBS="$2"
			shift 2
			;;
		--jobs=*)
			JOBS="${1#*=}"
			shift
			;;
		--dry-run)
			DRY_RUN=1
			shift
			;;
		-h | --help)
			usage
			exit 0
			;;
		*)
			error "unknown argument: $1"
			;;
	esac
done

[[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || error "--jobs must be a positive integer"

ROOT="$(abspath "$ROOT")"
[[ -d "$ROOT" ]] || error "root directory not found: $ROOT"
ROOT="$(cd "$ROOT" && pwd)"

need_cmd find
need_cmd sort

root_regex="$(escape_regex "$ROOT")"
source_filter="^${root_regex}/(?!.*\\/test\\/)(base|boot|core|firmware|kernel|platforms|userspace)/.*\\.c$"
config='{CheckOptions: {misc-include-cleaner.MissingIncludes: false, misc-include-cleaner.UnusedIncludes: true, misc-include-cleaner.IgnoreHeaders: "libc/.*;base/io_port\\.h;core/cpu\\.h;hal/hcf\\.h"}}'

discover_build_dirs() {
	local discovered=()
	local build_dir
	local database

	if (( ${#BUILD_DIRS[@]} == 0 )); then
		while IFS= read -r -d '' database; do
			discovered+=("${database%/compile_commands.json}")
		done < <(
			find "$ROOT" \
				-path "$ROOT/.git" -prune -o \
				-path "$ROOT/third_party" -prune -o \
				-type f -name compile_commands.json -print0
		)
	else
		for build_dir in "${BUILD_DIRS[@]}"; do
			if [[ "$build_dir" != /* ]]; then
				build_dir="${ROOT}/${build_dir}"
			fi

			[[ -f "${build_dir}/compile_commands.json" ]] ||
				error "compilation database not found: ${build_dir}/compile_commands.json"
			discovered+=("$(cd "$build_dir" && pwd)")
		done
	fi

	if (( ${#discovered[@]} == 0 )); then
		BUILD_DIRS=()
		return
	fi

	mapfile -t BUILD_DIRS < <(printf '%s\n' "${discovered[@]}" | sort -u)
}

declare -A compilation_counts=()
declare -A unused_counts=()
declare -A include_lines=()

record_compilations() {
	local log_file="$1"
	local count
	local line
	local source

	while IFS= read -r line; do
		if [[ "$line" =~ clang-tidy.*[[:space:]]($root_regex/[^[:space:]]+\.c)$ ]]; then
			source="${BASH_REMATCH[1]}"
			(( compilation_counts["$source"] += 1 ))
		elif [[ "$line" =~ \(1/([0-9]+)\)[[:space:]]Processing[[:space:]]file[[:space:]](.+)\.$ ]]; then
			count="${BASH_REMATCH[1]}"
			source="${BASH_REMATCH[2]}"
			if (( count > 1 )); then
				(( compilation_counts["$source"] += count - 1 ))
			fi
		fi
	done < "$log_file"
}

record_unused_includes() {
	local build_dir="$1"
	local log_file="$2"
	local diagnostic
	local diagnostic_path
	local file
	local key
	local line
	local args=(
		-clang-tidy-binary "$CLANG_TIDY_BIN"
		-p "$build_dir"
		-checks=-*,misc-include-cleaner
		-config "$config"
		-source-filter "$source_filter"
		-j "$JOBS"
		-quiet
	)

	log "analyzing ${build_dir}/compile_commands.json"
	if ! "$RUN_CLANG_TIDY_BIN" "${args[@]}" >"$log_file" 2>&1; then
		cat "$log_file" >&2
		error "clang-tidy analysis failed for $build_dir"
	fi
	record_compilations "$log_file"

	while IFS= read -r diagnostic; do
		if [[ "$diagnostic" =~ ^(.+):([0-9]+):[0-9]+:[[:space:]]warning:[[:space:]]included[[:space:]]header.*\[misc-include-cleaner\]$ ]]; then
			diagnostic_path="${BASH_REMATCH[1]}"
			line="${BASH_REMATCH[2]}"
			if [[ "$diagnostic_path" == /* ]]; then
				file="$(realpath -m "$diagnostic_path")"
			else
				file="$(realpath -m "$build_dir/$diagnostic_path")"
			fi
			key="${file}|${line}"
			(( unused_counts["$key"] += 1 ))
			include_lines["$key"]="$(sed -n "${line}p" "$file")"
		fi
	done < "$log_file"
}

apply_common_removals() {
	local accepted=()
	local count
	local file
	local include_line
	local key
	local line
	local source_count

	for key in "${!unused_counts[@]}"; do
		file="${key%|*}"
		line="${key##*|}"
		count="${unused_counts[$key]}"
		source_count="${compilation_counts[$file]:-0}"

		if (( source_count > 0 && count == source_count )); then
			accepted+=("$key")
		fi
	done

	if (( ${#accepted[@]} == 0 )); then
		log "no includes are unused in every discovered configuration"
		return
	fi

	while IFS= read -r key; do
		file="${key%|*}"
		line="${key##*|}"
		include_line="$(sed -n "${line}p" "$file")"

		[[ "$include_line" == "${include_lines[$key]}" ]] ||
			error "source changed during analysis: ${file}:${line}"
		[[ "$include_line" =~ ^[[:space:]]*#[[:space:]]*include[[:space:]] ]] ||
			error "expected an include at ${file}:${line}"

		if (( DRY_RUN )); then
			printf 'would remove %s:%s: %s\n' "${file#"$ROOT"/}" "$line" "$include_line"
		else
			log "removing ${file#"$ROOT"/}:${line}: ${include_line}"
			sed -i "${line}d" "$file"
		fi
	done < <(printf '%s\n' "${accepted[@]}" | sort -t '|' -k1,1 -k2,2nr)
}

discover_build_dirs

if (( ${#BUILD_DIRS[@]} == 0 )); then
	log "no compilation databases found; skipping"
	exit 0
fi

need_cmd "$CLANG_TIDY_BIN"
need_cmd "$RUN_CLANG_TIDY_BIN"
need_cmd grep
need_cmd mktemp
need_cmd realpath
need_cmd sed

if ! "$CLANG_TIDY_BIN" \
	--checks='-*,misc-include-cleaner' \
	--list-checks 2>/dev/null |
	grep -q 'misc-include-cleaner'; then
	error "'$CLANG_TIDY_BIN' does not provide the misc-include-cleaner check"
fi

temp_dir="$(mktemp -d)" || error "failed to create temporary directory"
trap 'rm -rf "$temp_dir"' EXIT

log "using ${#BUILD_DIRS[@]} compilation database(s)"

for index in "${!BUILD_DIRS[@]}"; do
	record_unused_includes "${BUILD_DIRS[$index]}" "$temp_dir/$index.log"
done

apply_common_removals
log "done"
