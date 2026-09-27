#!/usr/bin/env bash
# Build the PlayStation 2 port with PS2BUILD.
#
# Generates source/_build_info.h (git hash / branch / date, used by the
# crash reporter and startup banner) and then runs `ps2build build`.
# Extra arguments are passed through to ps2build.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

HASH="$(git rev-parse --short HEAD 2>/dev/null || echo unknown)"
if ! git diff --quiet 2>/dev/null; then HASH="${HASH}-dirty"; fi
BRANCH="$(git rev-parse --abbrev-ref HEAD 2>/dev/null || echo unknown)"
DATE="$(date -u +"%Y-%m-%dT%H:%M:%SZ")"

INFO_TMP="$(mktemp)"
{
	echo "#define GIT_HASH \"${HASH}\""
	echo "#define GIT_BRANCH \"${BRANCH}\""
	echo "#define BUILD_DATE \"${DATE}\""
} > "$INFO_TMP"
# only touch the header when it changes to avoid needless rebuilds
if ! cmp -s "$INFO_TMP" source/_build_info.h 2>/dev/null; then
	mv "$INFO_TMP" source/_build_info.h
else
	rm -f "$INFO_TMP"
fi

ps2build build "$@"

ELF=build/bin/nzportable.elf
if [ -f "$ELF" ]; then
	echo
	echo "== $ELF"
	ls -l "$ELF"
	SIZE_TOOL="$(dirname "$(command -v ps2build)")/toolchain/ee/bin/mips64r5900el-ps2-elf-size"
	[ -x "$SIZE_TOOL" ] || SIZE_TOOL="${SIZE_TOOL}.exe"
	if [ -x "$SIZE_TOOL" ]; then "$SIZE_TOOL" -A "$ELF" | grep -E "^\.(text|data|rodata|bss|sbss|sdata)|Total"; fi
fi
