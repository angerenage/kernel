#!/usr/bin/env bash
set -euo pipefail

usage() {
	cat <<'EOF'
Usage: format_sources.sh [options]

Run clang-format on selected source files, then enforce a newline after __attribute__(...).

Options:
  --root <dir>             Root directory to scan (default: .)
  --ext <glob>             File glob to include (repeatable). Defaults:
                             *.c *.h
  --exclude <glob>         Path pattern to exclude (repeatable). Matches
                           root-relative paths, absolute paths, or basenames.
                           Examples:
                             --exclude '*/limine.h'
                             --exclude test
                             --exclude=platforms/pc_qemu_x86_64
  --clang-format <path>    Path to clang-format binary (default: clang-format)
  --dry-run                Print commands instead of modifying files
  -h, --help               Show this help and exit

Notes:
- Perl is preferred for the attribute rewrite (handles nested parentheses). Falls back to sed if Perl is absent.
- `build-*` directories and `include/limine.h` are skipped by default.
EOF
}

error() {
	echo "format_sources.sh: $*" >&2
	exit 1
}

log() {
	echo "[format] $*"
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

ROOT="."
CLANG_FORMAT_BIN="clang-format"
DRY_RUN=0

# Defaults (can be added-to via --ext)
EXTS=( "*.c" "*.h" )
EXCLUDES=( "include/limine.h" )
PRUNE_DIRS=( "build-*" )

while [[ $# -gt 0 ]]; do
	case "$1" in
		--root)
			ROOT="$2"; shift 2 ;;
		--root=*)
			ROOT="${1#*=}"; shift 1 ;;
		--ext)
			EXTS+=( "$2" ); shift 2 ;;
		--ext=*)
			EXTS+=( "${1#*=}" ); shift 1 ;;
		--exclude)
			EXCLUDES+=( "$2" ); shift 2 ;;
		--exclude=*)
			EXCLUDES+=( "${1#*=}" ); shift 1 ;;
		--clang-format)
			CLANG_FORMAT_BIN="$2"; shift 2 ;;
		--clang-format=*)
			CLANG_FORMAT_BIN="${1#*=}"; shift 1 ;;
		--dry-run)
			DRY_RUN=1; shift 1 ;;
		-h|--help)
			usage; exit 0 ;;
		*)
			error "unknown argument: $1" ;;
	esac
done

ROOT="$(abspath "$ROOT")"
[[ -d "$ROOT" ]] || error "root directory not found: $ROOT"

need_cmd find
need_cmd xargs
need_cmd "$CLANG_FORMAT_BIN" || true	# nicer error later if missing

# Build find expression
build_find_args() {
	local -n _args=$1
	_args=( "$ROOT" )
	if (( ${#PRUNE_DIRS[@]} > 0 )); then
		_args+=( "(" -type d "(" )
		for i in "${!PRUNE_DIRS[@]}"; do
			(( i > 0 )) && _args+=( -o )
			_args+=( -name "${PRUNE_DIRS[$i]}" )
		done
		_args+=( ")" -prune ")" -o )
	fi
	_args+=( -type f "(" )
	for i in "${!EXTS[@]}"; do
		(( i > 0 )) && _args+=( -o )
		_args+=( -name "${EXTS[$i]}" )
	done
	_args+=( ")" -print0 )
}

relative_path() {
	local path="$1"

	case "$path" in
		"$ROOT") printf '.\n' ;;
		"$ROOT"/*) printf '%s\n' "${path#"$ROOT"/}" ;;
		*) printf '%s\n' "$path" ;;
	esac
}

matches_exclude() {
	local path="$1"
	local rel
	local base
	local ex

	rel="$(relative_path "$path")"
	base="${path##*/}"

	for ex in "${EXCLUDES[@]}"; do
		if [[ "$path" == $ex || "$rel" == $ex || "$base" == $ex ]]; then
			return 0
		fi

		if [[ "$path" == $ex/* || "$rel" == $ex/* ]]; then
			return 0
		fi
	done

	return 1
}

gather_files() {
	local args=()
	local unsorted_files=()
	local file

	build_find_args args
	while IFS= read -r -d '' file; do
		if matches_exclude "$file"; then
			continue
		fi
		unsorted_files+=( "$file" )
	done < <(find "${args[@]}")

	if (( ${#unsorted_files[@]} == 0 )); then
		FILES=()
		return 0
	fi

	mapfile -t FILES < <(printf '%s\n' "${unsorted_files[@]}" | sort)
}

run_clang_format() {
	[[ ${#FILES[@]} -eq 0 ]] && return 0
	need_cmd "$CLANG_FORMAT_BIN"

	log "running $CLANG_FORMAT_BIN on ${#FILES[@]} file(s)"
	if (( DRY_RUN )); then
		printf '%s\0' "${FILES[@]}" | xargs -0 -n 50 echo "$CLANG_FORMAT_BIN -i"
	else
		printf '%s\0' "${FILES[@]}" | xargs -0 -n 50 "$CLANG_FORMAT_BIN" -i
	fi
}

apply_attribute_breaks() {
	[[ ${#FILES[@]} -eq 0 ]] && return 0
	log "enforcing newline after __attribute__(...)"

	if command -v perl >/dev/null 2>&1; then
		# Perl (handles nested parentheses via named recursion):
		#   (?<attr>__attribute__(?&P)) capture balanced __attribute__((...))
		#   (?(DEFINE)(?<P>\((?:[^()]+|(?&P))*\))) defines the recursive rule
		#   \s+  whitespace before the next token, replaced by newline
		if (( DRY_RUN )); then
			printf '%s\0' "${FILES[@]}" | xargs -0 -n 50 echo perl -0777 -pe \
				"'s{(?<attr>__attribute__(?&P))\\s+(?(DEFINE)(?<P>\\((?:[^()]+|(?&P))*\\)))}{$+{attr}\\n}g' -i"
		else
			printf '%s\0' "${FILES[@]}" | xargs -0 -n 50 perl -0777 -pe \
				's{(?<attr>__attribute__(?&P))\s+(?(DEFINE)(?<P>\((?:[^()]+|(?&P))*\)))}{$+{attr}\n}g' -i
		fi
	elif command -v sed >/dev/null 2>&1; then
		# sed fallback (does not handle nested parens perfectly, but fine for common cases)
		if sed --version >/dev/null 2>&1; then
			# GNU sed
			if (( DRY_RUN )); then
				printf '%s\0' "${FILES[@]}" | xargs -0 -n 1 echo sed -E -i \
					"'s/^(__attribute__\\(\\([^)]*\\)\\))\\s+/\\1\\n/'"
			else
				printf '%s\0' "${FILES[@]}" | xargs -0 -n 1 sed -E -i \
					's/^(__attribute__\(\([^)]*\)\))\s+/\1\n/'
			fi
		else
			# BSD sed
			if (( DRY_RUN )); then
				printf '%s\0' "${FILES[@]}" | xargs -0 -n 1 echo sed -E -i "''" \
					"'s/^(__attribute__\\(\\([^)]*\\)\\))\\s+/\\1\\n/'"
			else
				printf '%s\0' "${FILES[@]}" | xargs -0 -n 1 sed -E -i '' \
					's/^(__attribute__\(\([^)]*\)\))\s+/\1\n/'
			fi
		fi
	else
		error "neither perl nor sed available for attribute post-processing"
	fi
}

# --- main ---
gather_files
echo "Found ${#FILES[@]} file(s)."

if (( ${#FILES[@]} > 0 )); then
	run_clang_format
	apply_attribute_breaks
	echo "Done."
else
	echo "Nothing to do."
fi
