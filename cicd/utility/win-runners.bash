#!/usr/bin/env bash

##	Purpose:
##		Run every binding's conformance runner on whatever host this is invoked
##		on. The pipeline's own test stage is defined in cicd/config.bash and is
##		wired into the engine, which is bash plus Linux tooling all the way down,
##		so none of it runs on windows. That left the file tier - the one part of
##		the library with real per-platform code - covered only where the
##		platform-specific half never executes, and the python binding threw on
##		every windows overwrite for a release without anything noticing. This is
##		the smaller gate that does run there: the runners and the veneer smoke,
##		and nothing else.
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

## Fuzz iterations stay at the in-test default: the long soak is the Linux gate's
## job, and nothing about it is platform-dependent.
fRun "rust"        cargo test --manifest-path source/rust/Cargo.toml
fRun "go library"  go -C source/go test ./...
fRun "go cli"      go -C source/go/cmd test ./...
fRun "python"      "${py}" source/python/tests/conformance.py
fRun "c"           fRunC
fRun "c++ veneer"  fRunCxx

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
