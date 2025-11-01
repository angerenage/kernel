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
  --exclude <glob>         Path pattern to exclude (repeatable). Examples:
                             --exclude '*/limine.h'
  --clang-format <path>    Path to clang-format binary (default: clang-format)
  --dry-run                Print commands instead of modifying files
  -h, --help               Show this help and exit

Notes:
- Perl is preferred for the attribute rewrite (handles nested parentheses). Falls back to sed if Perl is absent.
- Exclusions are applied as -not -path globs to find(1).
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
EXCLUDES=()

while [[ $# -gt 0 ]]; do
	case "$1" in
		--root)
			ROOT="$2"; shift 2 ;;
		--ext)
			EXTS+=( "$2" ); shift 2 ;;
		--exclude)
			EXCLUDES+=( "$2" ); shift 2 ;;
		--clang-format)
			CLANG_FORMAT_BIN="$2"; shift 2 ;;
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
	_args=( "$ROOT" -type f "(" )
	for i in "${!EXTS[@]}"; do
		(( i > 0 )) && _args+=( -o )
		_args+=( -name "${EXTS[$i]}" )
	done
	_args+=( ")" )
	for ex in "${EXCLUDES[@]}"; do
		_args+=( -not -path "$ex" )
	done
}

gather_files() {
	local args=()
	build_find_args args
	# shellcheck disable=SC2207
	FILES=( $(find "${args[@]}" -print0 | xargs -0 -I{} printf '%s\n' "{}" | sort) )
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
