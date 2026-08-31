#!/usr/bin/env bash

##	Purpose:
##		Compile the C test programs, the C++ veneer smoke and the C CLI under the
##		address and undefined-behavior sanitizers, and run them over the corpus.
##		The optimized test line proves the output; this proves the memory behind
##		it. A read past a buffer, a NULL handed to a library call, or signed
##		overflow that happens not to change stdout passes every other gate and
##		fails here. The test programs run as they are; the CLI is driven the way
##		crosscheck.bash drives it, over every dimension the corpus has: fmt,
##		check, check --schema, the write-ops and bad-ops scripts, the layered
##		load, init --schema, and every reads.tsv row as get/count/instances.
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
flags=(-O1 -g "-fsanitize=address,undefined" -fno-sanitize-recover=all -fno-omit-frame-pointer -Wall -Wextra -Werror -Isource/c)
export ASAN_OPTIONS="exitcode=77:${ASAN_OPTIONS:-}" UBSAN_OPTIONS="exitcode=77:print_stacktrace=1:${UBSAN_OPTIONS:-}"

work="$(mktemp -d)"; trap 'rm -rf "${work}"' EXIT
## compiler, language standard, source, output.
fBuild(){ "$1" "-std=$2" "${flags[@]}" "$3" -o "$4" -lm -lpthread || { echo "sanitize-c: build failed: $3" >&2; exit 2; }; }

rc=0
fBuild cc c11 source/c/tests/conformance.c "${work}/conformance"
"${work}/conformance" "${corpus}" || { echo "sanitize-c: conformance runner: exit $?" >&2; rc=1; }
## The OOM hook there longjmps out of the parse, so every budget that trips it
## abandons a partly built document on purpose; that is the test, not a leak.
fBuild cc c11 source/c/tests/oom_hook.c "${work}/oom_hook"
ASAN_OPTIONS="${ASAN_OPTIONS}:detect_leaks=0" "${work}/oom_hook" || { echo "sanitize-c: oom_hook: exit $?" >&2; rc=1; }
## The C++ veneer owns the C handle by hand (rule of five over a raw pointer),
## which is exactly the kind of code a leak or double free hides in.
fBuild g++ c++17 source/c/tests/veneer_smoke.cpp "${work}/veneer_smoke"
"${work}/veneer_smoke" || { echo "sanitize-c: veneer_smoke: exit $?" >&2; rc=1; }

## The CLI's own exit codes are the corpus contract, checked elsewhere; here only
## a sanitizer stop counts, with its report.
fBuild cc c11 source/c/cmd/shcl/main.c "${work}/shcl"
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
## One reads.tsv row as the CLI calls crosscheck.bash makes of it. Columns:
## query, type, expected, status, optional level; only the invocation matters
## here, so expected and status are not read.
fReadRow(){
	local input="$1" query="$2" type="$3" level="$4"
	local -a strictArg=()
	[[ -n "$level" ]] && strictArg=("--strictness=${level}")
	case "$type" in
		load)         fCli check "${strictArg[@]}" "$input"
		              fCli fmt "${strictArg[@]}" "$input" ;;
		count)        fCli count "${strictArg[@]}" "$input" "$query" ;;
		instances)    fCli instances "${strictArg[@]}" "$input" "$query" ;;
		*'[]')        fCli get "--${type%[]}" --array "${strictArg[@]}" "$input" "$query"
		              fCli get "--${type%[]}" --array --slots "${strictArg[@]}" "$input" "$query" ;;
		*)            fCli get "--${type}" "${strictArg[@]}" "$input" "$query"
		              fCli get "--${type}" --on-bad=error "${strictArg[@]}" "$input" "$query"
		              fCli get "--${type}" "--default=<x>" "${strictArg[@]}" "$input" "$query" ;;
	esac
}
for caseDir in "${corpus}"/*/; do
	input="${caseDir}input.shcl"
	[[ -f "${input}" ]] || continue
	fCli fmt "${input}"
	fCli check "${input}"
	if [[ -f "${caseDir}schema.shcl" ]]; then fCli check "--schema=${caseDir}schema.shcl" "${input}"; fi
	if [[ -f "${caseDir}write.ops" ]]; then fCli set "${input}" < "${caseDir}write.ops"; fi
	## Each bad op alone, as its own script.
	if [[ -f "${caseDir}write-bad.ops" ]]; then
		while IFS= read -r bline || [[ -n "$bline" ]]; do
			[[ -z "$bline" || "$bline" == \#* ]] && continue
			printf '%s\n' "$bline" > "${work}/badop.line"
			fCli set "${input}" < "${work}/badop.line"
		done < "${caseDir}write-bad.ops"
	fi
	## Layered load: layer*.shcl under the input (filename order = priority),
	## plus any merge.sets overrides.
	if [[ -f "${caseDir}expected-merged.shcl" ]]; then
		layerArgs=()
		## `continue` on the miss, not a trailing `&&`: an unmatched glob leaves
		## the pattern in $lf, the test fails, and a loop whose body ends on a
		## failed && list returns 1 - which kills the caller the day this loop
		## becomes the last thing in a function.
		for lf in "${caseDir}"layer*.shcl; do
			[[ -f "$lf" ]] || continue
			layerArgs+=("--layer=$lf")
		done
		setArgs=()
		if [[ -f "${caseDir}merge.sets" ]]; then
			while IFS= read -r sline || [[ -n "$sline" ]]; do
				[[ -z "$sline" || "$sline" == \#* ]] && continue
				setArgs+=("--set=$sline")
			done < "${caseDir}merge.sets"
		fi
		fCli fmt "${layerArgs[@]}" "${setArgs[@]}" "${input}"
	fi
	if [[ -f "${caseDir}init-schema.shcl" ]]; then
		fCli init "--schema=${caseDir}init-schema.shcl"
		fCli init --no-banner "--schema=${caseDir}init-schema.shcl"
	fi
	if [[ -f "${caseDir}reads.tsv" ]]; then
		while IFS=$'\t' read -r query type _expected _status level _rest || [[ -n "$query" ]]; do
			[[ -z "$query" || "$query" == "query" ]] && continue
			fReadRow "${input}" "$query" "$type" "${level:-}"
		done < "${caseDir}reads.tsv"
	fi
done
if ((nBad)); then
	echo "sanitize-c: ${nBad}/${nRuns} CLI run(s) stopped by a sanitizer" >&2; rc=1
else
	echo "sanitize-c: OK: runner, oom_hook, veneer_smoke and ${nRuns} CLI run(s) clean under ASan+UBSan"
fi
exit "${rc}"


##	History:
##		- 2026-08-29 JC: Created. The first sanitized run found a NULL fwrite the
##		  optimized gate had been passing.
##		- 2026-08-30 JC: The CLI now covers reads.tsv, init --schema, the layered
##		  load and the bad-ops scripts, and the C++ veneer smoke runs too.
