#!/usr/bin/env bash

##	Purpose:
##		Compile the C test programs and the C CLI under the address and
##		undefined-behavior sanitizers, and run them over the corpus. The
##		optimized test line proves the output; this proves the memory behind it.
##		A read past a buffer, a NULL handed to a library call, or signed overflow
##		that happens not to change stdout passes every other gate and fails here.
##		The corpus runner and the OOM-hook test run as they are; the CLI is run
##		per case over fmt, check, check --schema and the write-ops script.
##	Syntax:
##		sanitize-c.bash [CORPUS_DIR]
##		  CORPUS_DIR  conformance corpus root (default project/conformance)
##	Exit: 0 = clean, 1 = a program failed or a sanitizer reported, 2 = build failed.
##	History: At bottom of script.

##	Copyright © 2026 Bubbles (ID: XଌฅრX۳ᛟԃლፀƅꓩหδლც)
##	Licensed under The MIT License (MIT). Full text at:
##		https://mit-license.org/
##	SPDX-License-Identifier: MIT


set -Eeuo pipefail

root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${root}"
corpus="${1:-project/conformance}"
[[ -d "${corpus}" ]] || { echo "sanitize-c: no corpus dir: ${corpus}" >&2; exit 2; }

## -O1 keeps line numbers honest in a report; -fno-sanitize-recover makes every
## UBSan finding fatal instead of a note that scrolls past. 77 is no exit code
## any of these programs uses, so a sanitizer stop cannot be mistaken for the
## program's own verdict.
flags=(-std=c11 -O1 -g "-fsanitize=address,undefined" -fno-sanitize-recover=all -fno-omit-frame-pointer -Wall -Wextra -Werror -Isource/c)
export ASAN_OPTIONS="exitcode=77:${ASAN_OPTIONS:-}" UBSAN_OPTIONS="exitcode=77:print_stacktrace=1:${UBSAN_OPTIONS:-}"

work="$(mktemp -d)"; trap 'rm -rf "${work}"' EXIT
fBuild(){ cc "${flags[@]}" "$1" -o "$2" -lm || { echo "sanitize-c: build failed: $1" >&2; exit 2; }; }

rc=0
fBuild source/c/tests/conformance.c "${work}/conformance"
"${work}/conformance" "${corpus}" || { echo "sanitize-c: conformance runner: exit $?" >&2; rc=1; }
## The OOM hook there longjmps out of the parse, so every budget that trips it
## abandons a partly built document on purpose; that is the test, not a leak.
fBuild source/c/tests/oom_hook.c "${work}/oom_hook"
ASAN_OPTIONS="${ASAN_OPTIONS}:detect_leaks=0" "${work}/oom_hook" || { echo "sanitize-c: oom_hook: exit $?" >&2; rc=1; }

## The CLI's own exit codes are the corpus contract, checked elsewhere; here only
## a sanitizer stop counts, with its report.
fBuild source/c/cmd/shcl/main.c "${work}/shcl"
declare -i nRuns=0 nBad=0
fCli(){
	nRuns+=1
	"${work}/shcl" "$@" >/dev/null 2>"${work}/stderr" || {
		local code=$?
		if ((code == 77)); then
			nBad+=1
			echo "sanitize-c: shcl $*"
			sed 's/^/    /' "${work}/stderr"
		fi
	}
}
for caseDir in "${corpus}"/*/; do
	input="${caseDir}input.shcl"
	[[ -f "${input}" ]] || continue
	fCli fmt "${input}"
	fCli check "${input}"
	if [[ -f "${caseDir}schema.shcl" ]]; then fCli check "--schema=${caseDir}schema.shcl" "${input}"; fi
	if [[ -f "${caseDir}write.ops" ]]; then fCli set "${input}" < "${caseDir}write.ops"; fi
done
if ((nBad)); then
	echo "sanitize-c: ${nBad}/${nRuns} CLI run(s) stopped by a sanitizer" >&2; rc=1
else
	echo "sanitize-c: OK: runner, oom_hook and ${nRuns} CLI run(s) clean under ASan+UBSan"
fi
exit "${rc}"


##	History:
##		- 2026-08-29 JC: Created. The first sanitized run found a NULL fwrite the
##		  optimized gate had been passing.
