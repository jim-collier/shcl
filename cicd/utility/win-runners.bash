#!/usr/bin/env bash

##	Purpose:
##		Run every binding's conformance runner on whatever host this is invoked
##		on. The pipeline's own test stage is defined in cicd/config.bash and is
##		wired into the engine, which is bash plus Linux tooling all the way down,
##		so none of it runs on windows. That left the file tier - the one part of
##		the library with real per-platform code - covered only where the
##		platform-specific half never executes, and the python binding threw on
##		every windows overwrite for a release without anything noticing. This is
##		the smaller gate that does run there: the runners, the veneer smoke and,
##		on a real windows host, the installers' registry PATH handling.
##	Syntax:
##		win-runners.bash [ROOT]
##		  ROOT   repo root (default: two levels up from this script)
##		CC, CXX and PYTHON override the tools it reaches for.
##	Exit: 0 = every runner passed, 1 = at least one did not, 2 = a tool is missing.
##	History: At bottom of script.

##	Copyright © 2026 Bubbles (ID: XଌฅრX۳ᛟԃლፀƅꓩหδლც)
##	Licensed under The MIT License (MIT). Full text at:
##		https://mit-license.org/
##	SPDX-License-Identifier: MIT


set -Eeuo pipefail

meDir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
root="${1:-$(cd -- "${meDir}/../.." && pwd)}"
cd -- "${root}"

cc="${CC:-gcc}"
cxx="${CXX:-g++}"
py="${PYTHON:-}"
if [[ -z "${py}" ]]; then
	if command -v python3 >/dev/null 2>&1; then py=python3; else py=python; fi
fi

## Without the suffix the link succeeds and the run cannot find what it wrote.
exe=""
case "$(uname -s 2>/dev/null || true)" in MINGW*|MSYS*|CYGWIN*) exe=".exe" ;; esac

## A missing compiler otherwise surfaces as an unreadable build error three
## stages later; say which tool and stop.
for tool in cargo go "${py}" "${cc}" "${cxx}"; do
	command -v "${tool}" >/dev/null 2>&1 || { echo "win-runners: not found: ${tool}" >&2; exit 2; }
done

work="$(mktemp -d)"
trap 'rm -rf "${work}"' EXIT

echo "win-runners: root ${root}"
echo "win-runners: $(uname -s 2>/dev/null || echo unknown), cc=${cc} cxx=${cxx} py=${py}"

failed=()
fRun() {   ## fRun NAME COMMAND [ARG ...]
	local name="$1"; shift
	echo
	echo "[ ${name} ]"
	if "$@"; then echo "win-runners: ${name}: OK"
	else echo "win-runners: ${name}: FAILED" >&2; failed+=("${name}"); fi
}

## The C pair build into the temp dir rather than the tree, so a run leaves
## nothing behind for the next one to pick up stale.
fRunC() {
	"${cc}" -std=c11 -O2 -Wall -Wextra -Werror -Isource/c \
		source/c/tests/conformance.c -o "${work}/conformance${exe}" -lm \
		&& "${work}/conformance${exe}" project/conformance
}
fRunCxx() {
	"${cxx}" -std=c++17 -O2 -Wall -Wextra -Werror -Isource/c \
		source/c/tests/veneer_smoke.cpp -o "${work}/veneer${exe}" -lm \
		&& "${work}/veneer${exe}"
}
fRunOom() {
	"${cc}" -std=c11 -O2 -Wall -Wextra -Werror -Isource/c \
		source/c/tests/oom_hook.c -o "${work}/oom_hook${exe}" -lm \
		&& "${work}/oom_hook${exe}"
}
## The allocation bounds move with the allocator, so they belong here rather
## than on linux alone. The unwind test is deliberately not here: it crashes on
## a real windows host and passes everywhere else (20260901b item 48).
fRunMemBounds() {
	"${cc}" -std=c11 -O2 -Wall -Wextra -Werror -Isource/c \
		source/c/tests/mem_bounds.c -o "${work}/mem_bounds${exe}" -lm \
		&& "${work}/mem_bounds${exe}"
}
## The C CLI's argv: the narrow one arrives in the active code page, best-fit
## mapped, so a name the page cannot spell reached a different file. The two
## names here are the shapes that went wrong: one outside the page, one the
## page maps onto a plain letter.
fRunCcli() {
	"${cc}" -std=c11 -O2 -Wall -Wextra -Werror -Isource/c \
		source/c/cmd/shcl/main.c -o "${work}/shcl-c${exe}" -lm || return 1
	local dir="${work}/argv"
	mkdir -p "${dir}"
	printf 'a: 1\n' > "${dir}/a.shcl"
	printf 'b: 1\n' > "${dir}/ā.shcl"
	printf 'c: 1\n' > "${dir}/café.shcl"
	local got
	got="$("${work}/shcl-c${exe}" get "${dir}/café.shcl" c 2>&1)" || { echo "win-runners: c cli: café.shcl: ${got}" >&2; return 1; }
	[[ "${got}" == "1" ]] || { echo "win-runners: c cli: café.shcl read ${got@Q}" >&2; return 1; }
	"${work}/shcl-c${exe}" set --write --set x=1 "${dir}/ā.shcl" > /dev/null || return 1
	[[ "$(cat "${dir}/a.shcl")" == "a: 1" ]] || { echo "win-runners: c cli: a.shcl was rewritten in place of ā.shcl" >&2; return 1; }
	grep -q '^x: 1$' "${dir}/ā.shcl" || { echo "win-runners: c cli: ā.shcl was not written" >&2; return 1; }
}

## A stdin nothing is attached to reads as an empty document, exit 0. POSIX
## reports that as EOF and every binding already agreed there; windows answers
## with an invalid handle or an invalid function instead, which each runtime
## spells its own way, so this is the only place the rule can be judged.
fRunClosedStdin() {
	local bad=0
	"${cc}" -std=c11 -O2 -Wall -Wextra -Werror -Isource/c \
		source/c/cmd/shcl/main.c -o "${work}/shcl-c${exe}" -lm || return 1
	cargo build --quiet --manifest-path source/rust/Cargo.toml || return 1
	go -C source/go/cmd build -o "${work}/shcl-go${exe}" ./shcl || return 1
	local clis=(
		"rust|source/rust/target/debug/shcl${exe}"
		"go|${work}/shcl-go${exe}"
		"python|${py} source/python/cmd/shcl/main.py"
		"c|${work}/shcl-c${exe}"
	)
	local entry name cmd out rc
	for entry in "${clis[@]}"; do
		name="${entry%%|*}"; cmd="${entry#*|}"
		## Split on purpose: the python entry is an interpreter plus a script.
		rc=0; out="$(${cmd} fmt - 0<&- 2>&1)" || rc=$?
		if ((rc != 0)) || [[ -n "${out}" ]]; then
			echo "win-runners: closed stdin: ${name} exited ${rc} saying ${out@Q}" >&2
			bad=1
		fi
	done
	((bad == 0))
}

## Fuzz iterations stay at the in-test default: the long soak is the Linux gate's
## job, and nothing about it is platform-dependent.
fRun "rust"        cargo test --manifest-path source/rust/Cargo.toml
fRun "go library"  go -C source/go test ./...
fRun "go cli"      go -C source/go/cmd test ./...
fRun "python"      "${py}" source/python/tests/conformance.py
fRun "c"           fRunC
fRun "c++ veneer"  fRunCxx
fRun "c oom hook"  fRunOom
fRun "c mem bounds" fRunMemBounds
fRun "c cli argv"  fRunCcli
fRun "closed stdin" fRunClosedStdin
## The installers' PATH handling needs a real registry, which only exists here:
## it edits and restores the runner's own Environment keys, so it stays off
## every other host.
case "$(uname -s 2>/dev/null || true)" in
	MINGW*|MSYS*|CYGWIN*)
		fRun "windows path" powershell -NoProfile -ExecutionPolicy Bypass -File cicd/utility/winpath-regress.ps1
		;;
esac

echo
if ((${#failed[@]} == 0)); then
	echo "win-runners: OK"
else
	echo "win-runners: FAILED: ${failed[*]}" >&2
	exit 1
fi


##	Script history:
##		- 20260821: Created. Nothing in the pipeline ran any binding on windows,
##		  where the file tier's publish step is a different code path in all four.
##		- 20260901: The installers' PATH handling joins, windows hosts only - it
##		  needs a real registry.
