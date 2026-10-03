#!/usr/bin/env bash
set -euo pipefail

usage() {
	cat <<'EOF'
Usage: format.sh [options]

Run clang-format on selected source files, then enforce a newline after
__attribute__(...) in normal C code.

Options:
  --root <dir>             Root directory to scan (default: .)
  --staged                 Format only the content currently staged in Git
  --ext <glob>             File glob to include (repeatable). Defaults:
                             *.c *.h
  --exclude <glob>         Path pattern to exclude (repeatable). Matches
                           root-relative paths, absolute paths, or basenames.
  --clang-format <path>    Path to clang-format binary (default: clang-format)
  --dry-run                Print what would be done without modifying anything
  -h, --help               Show this help and exit

Notes:
- Perl is required for the attribute rewrite.
- In --staged mode, only the Git index is modified.
- build-* directories and include/limine.h are skipped by default.
EOF
}

error() {
	echo "format.sh: $*" >&2
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
STAGED_ONLY=0

EXTS=("*.c" "*.h")
EXCLUDES=("include/limine.h")
PRUNE_DIRS=("build-*")
FILES=()

# Skip complete preprocessor directives, including multiline ones, then insert
# a newline after __attribute__((...)) only when another token follows on the
# same line.
#
# (?&P) recursively matches balanced parentheses.
ATTRIBUTE_PERL_EXPR='
	s{
		^[\x20\t]*\#(?:[^\n]*\\\r?\n)*[^\n]*(?:\r?\n|\z)(*SKIP)(*F)
		|
		__attribute__(?&P)\K[\x20\t]+
		(?(DEFINE)(?<P>\((?:[^()]+|(?&P))*\)))
	}{\n}gmx
'

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
		--staged)
			STAGED_ONLY=1
			shift
			;;
		--ext)
			[[ $# -ge 2 ]] || error "--ext requires an argument"
			EXTS+=("$2")
			shift 2
			;;
		--ext=*)
			EXTS+=("${1#*=}")
			shift
			;;
		--exclude)
			[[ $# -ge 2 ]] || error "--exclude requires an argument"
			EXCLUDES+=("$2")
			shift 2
			;;
		--exclude=*)
			EXCLUDES+=("${1#*=}")
			shift
			;;
		--clang-format)
			[[ $# -ge 2 ]] || error "--clang-format requires an argument"
			CLANG_FORMAT_BIN="$2"
			shift 2
			;;
		--clang-format=*)
			CLANG_FORMAT_BIN="${1#*=}"
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

ROOT="$(abspath "$ROOT")"
[[ -d "$ROOT" ]] || error "root directory not found: $ROOT"

need_cmd perl
need_cmd "$CLANG_FORMAT_BIN"

relative_path() {
	local path="$1"

	case "$path" in
		"$ROOT")
			printf '.\n'
			;;
		"$ROOT"/*)
			printf '%s\n' "${path#"$ROOT"/}"
			;;
		*)
			printf '%s\n' "$path"
			;;
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

matches_extension() {
	local path="$1"
	local base="${path##*/}"
	local ext

	for ext in "${EXTS[@]}"; do
		if [[ "$base" == $ext ]]; then
			return 0
		fi
	done

	return 1
}

build_find_args() {
	local -n args_ref=$1
	local i

	args_ref=("$ROOT")

	if (( ${#PRUNE_DIRS[@]} > 0 )); then
		args_ref+=("(" -type d "(")

		for i in "${!PRUNE_DIRS[@]}"; do
			(( i > 0 )) && args_ref+=(-o)
			args_ref+=(-name "${PRUNE_DIRS[$i]}")
		done

		args_ref+=(")" -prune ")" -o)
	fi

	args_ref+=(-type f "(")

	for i in "${!EXTS[@]}"; do
		(( i > 0 )) && args_ref+=(-o)
		args_ref+=(-name "${EXTS[$i]}")
	done

	args_ref+=(")" -print0)
}

sort_files() {
	local -n input_ref=$1

	if (( ${#input_ref[@]} == 0 )); then
		FILES=()
		return
	fi

	mapfile -t FILES < <(
		printf '%s\n' "${input_ref[@]}" |
			sort -u
	)
}

gather_files() {
	local args=()
	local found=()
	local file

	need_cmd find

	build_find_args args

	while IFS= read -r -d '' file; do
		matches_exclude "$file" && continue
		found+=("$file")
	done < <(find "${args[@]}")

	sort_files found
}

gather_staged_files() {
	local git_root
	local found=()
	local path
	local file

	need_cmd git

	git_root="$(
		git -C "$ROOT" rev-parse --show-toplevel 2>/dev/null
	)" || error "--staged requires a Git working tree"

	ROOT="$git_root"

	while IFS= read -r -d '' path; do
		matches_extension "$path" || continue

		file="${ROOT}/${path}"
		matches_exclude "$file" && continue

		git -C "$ROOT" cat-file -e ":$path" 2>/dev/null || continue

		found+=("$file")
	done < <(
		git -C "$ROOT" diff \
			--cached \
			--name-only \
			--diff-filter=ACMR \
			-z
	)

	sort_files found
}

apply_attribute_breaks_to() {
	perl -0777 -i -pe "$ATTRIBUTE_PERL_EXPR" "$@"
}

run_clang_format() {
	[[ ${#FILES[@]} -eq 0 ]] && return

	log "running $CLANG_FORMAT_BIN on ${#FILES[@]} file(s)"

	if (( DRY_RUN )); then
		printf 'format working tree: %s\n' "${FILES[@]}"
		return
	fi

	"$CLANG_FORMAT_BIN" -i -- "${FILES[@]}"
}

apply_attribute_breaks() {
	[[ ${#FILES[@]} -eq 0 ]] && return

	log "enforcing newline after __attribute__(...)"

	if (( DRY_RUN )); then
		printf 'rewrite attributes: %s\n' "${FILES[@]}"
		return
	fi

	apply_attribute_breaks_to "${FILES[@]}"
}

format_staged_file() {
	local file="$1"
	local rel
	local original_tmp
	local formatted_tmp
	local merged_tmp
	local mode
	local blob

	rel="$(relative_path "$file")"

	if (( DRY_RUN )); then
		printf 'format staged: %s\n' "$rel"
		return
	fi

	original_tmp="$(mktemp)" ||
		error "failed to create temporary file"

	formatted_tmp="$(mktemp)" || {
		rm -f "$original_tmp"
		error "failed to create formatted temporary file"
	}

	merged_tmp="$(mktemp)" || {
		rm -f "$original_tmp" "$formatted_tmp"
		error "failed to create merge temporary file"
	}

	if ! git -C "$ROOT" show ":$rel" >"$original_tmp"; then
		rm -f "$original_tmp" "$formatted_tmp" "$merged_tmp"
		error "failed to read staged content: $rel"
	fi

	if ! "$CLANG_FORMAT_BIN" \
		--assume-filename="$ROOT/$rel" \
		<"$original_tmp" \
		>"$formatted_tmp"; then

		rm -f "$original_tmp" "$formatted_tmp" "$merged_tmp"
		error "clang-format failed for staged file: $rel"
	fi

	if ! apply_attribute_breaks_to "$formatted_tmp"; then
		rm -f "$original_tmp" "$formatted_tmp" "$merged_tmp"
		error "attribute rewrite failed for staged file: $rel"
	fi

	if [[ -f "$file" ]]; then
		if git merge-file \
			-p \
			"$file" \
			"$original_tmp" \
			"$formatted_tmp" \
			>"$merged_tmp"; then

			cat "$merged_tmp" >"$file"
		else
			rm -f "$original_tmp" "$formatted_tmp" "$merged_tmp"

			error \
				"unable to propagate formatting into partially staged file: $rel
The staged and unstaged changes overlap with formatting changes."
		fi
	fi

	mode="$(
		git -C "$ROOT" ls-files -s -- "$rel" |
			awk 'NR == 1 { print $1 }'
	)"

	if [[ -z "$mode" ]]; then
		rm -f "$original_tmp" "$formatted_tmp" "$merged_tmp"
		error "unable to determine index mode for: $rel"
	fi

	if ! blob="$(
		git -C "$ROOT" hash-object -w "$formatted_tmp"
	)"; then
		rm -f "$original_tmp" "$formatted_tmp" "$merged_tmp"
		error "failed to create Git blob for: $rel"
	fi

	if ! git -C "$ROOT" update-index \
		--cacheinfo "$mode,$blob,$rel"; then

		rm -f "$original_tmp" "$formatted_tmp" "$merged_tmp"
		error "failed to update Git index for: $rel"
	fi

	rm -f "$original_tmp" "$formatted_tmp" "$merged_tmp"
}

format_staged_files() {
	local file

	[[ ${#FILES[@]} -eq 0 ]] && return

	need_cmd awk
	need_cmd mktemp

	log "formatting ${#FILES[@]} staged file(s)"

	for file in "${FILES[@]}"; do
		format_staged_file "$file"
	done
}

# --- main ---
if (( STAGED_ONLY )); then
	gather_staged_files
else
	gather_files
fi

echo "Found ${#FILES[@]} file(s)."

if (( ${#FILES[@]} == 0 )); then
	echo "Nothing to do."
	exit 0
fi

if (( STAGED_ONLY )); then
	format_staged_files
else
	run_clang_format
	apply_attribute_breaks
fi

echo "Done."
