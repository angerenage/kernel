#!/usr/bin/env bash
set -euo pipefail

error() {
	echo "install-hooks.sh: $*" >&2
	exit 1
}

need_cmd() {
	if ! command -v "$1" >/dev/null 2>&1; then
		error "required tool '$1' not found in PATH"
	fi
}

need_cmd git

repo_root="$(git rev-parse --show-toplevel 2>/dev/null)" || error "run this script from inside the repository"
source_hook="${repo_root}/.githooks/pre-commit"
target_hook="${repo_root}/.git/hooks/pre-commit"

[[ -f "$source_hook" ]] || error "hook template not found: ${source_hook}"

mkdir -p "$(dirname "$target_hook")"
cp "$source_hook" "$target_hook"

if command -v chmod >/dev/null 2>&1; then
	chmod +x "$target_hook" || true
fi

echo "[hooks] installed pre-commit hook at ${target_hook}"
