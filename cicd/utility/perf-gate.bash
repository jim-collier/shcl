#!/usr/bin/env bash

##	Purpose:
##		Fail when the read, write or malformed-input path stops being linear.
##		Two rounds running, a fix that was correct made bulk writes 4.5x slower
##		and absent-path defaults 140x slower, and both reached dev because
##		nothing had a number to fail on. A third round found a plain text file
##		parsing in quadratic time. Each workload is timed against the same
##		binding's parse-only baseline on the same machine, so the gate carries
##		no wall-clock constant and does not care how fast the runner is:
##		applying the ops, or refusing every line, must stay small beside
##		reading a well-formed document of the same size. A per-op index
##		rebuild, a scan of every sibling, or a rewalk of the retained lines
##		breaks that ratio by more than an order of magnitude.
##	Syntax:
##		perf-gate.bash [--keys N] [--factor F] NAME|CLI [NAME|CLI ...]
##		  --keys N    flat keys in the generated document (default 40000)
##		  --factor F  allowed multiple of the baseline (default 3)
##	Exit: 0 = every workload within budget, 1 = over budget, 2 = usage.
##	History: At bottom of script.

##	Copyright © 2026 Bubbles (ID: XଌฅრX۳ᛟԃლፀƅꓩหδლც)
##	Licensed under The MIT License (MIT). Full text at:
##		https://mit-license.org/
##	SPDX-License-Identifier: MIT


set -Eeuo pipefail

declare -i keys=40000 factor=3
bindings=()
while (($#)); do case "$1" in
	--keys)    keys="${2:-40000}"; shift 2 ;;
	--factor)  factor="${2:-3}"; shift 2 ;;
	-h|--help) grep -E '^##' "$0" | sed 's/^##\t\?//'; exit 0 ;;
	*)         bindings+=("$1"); shift ;;
esac; done
((${#bindings[@]})) || { echo "perf-gate: no bindings given" >&2; exit 2; }
for b in "${bindings[@]}"; do
	cli="${b#*|}"
	[[ -x "${cli}" ]] || { echo "perf-gate: binding CLI not executable: ${cli}" >&2; exit 2; }
done

tmpDir="$(mktemp -d)"; trap 'rm -rf "${tmpDir}"' EXIT
doc="${tmpDir}/doc.shcl"
awk -v n="${keys}" 'BEGIN{ for (i = 0; i < n; i++) printf "k%d: %d\n", i, i }' > "${doc}"
: > "${tmpDir}/base.ops"
## Every existing key rewritten: the writer's own path, nothing else.
awk -v n="${keys}" 'BEGIN{ for (i = 0; i < n; i++) printf "int\tk%d\t%d\n", i, i + 1 }' > "${tmpDir}/writes.ops"
## Paths that do not resolve: the set-if-absent half, which is where the
## whole-index rebuild hid.
awk 'BEGIN{ for (i = 0; i < 1000; i++) printf "int-default\tnew%d\t%d\n", i, i }' > "${tmpDir}/defaults.ops"
## Every existing key looked up and left alone. set-if-absent on a path that
## already resolves is a path read and nothing else, which is the only way to
## drive one lookup per key through a single process. Without an index that
## outlives the parse this is a sibling scan per key, so it goes quadratic.
awk -v n="${keys}" 'BEGIN{ for (i = 0; i < n; i++) printf "int-default\tk%d\t0\n", i }' > "${tmpDir}/reads.ops"
## The same line count with no colon on any line. Every line is refused and
## retained as trivia, and the retained list must not be rewalked per line.
badDoc="${tmpDir}/bad.shcl"
awk -v n="${keys}" 'BEGIN{ for (i = 0; i < n; i++) print "no colon here" }' > "${badDoc}"

##	Milliseconds for one run of $2 (an ops file, or a document when $3 is
##	"check") through CLI $1, best of two so a scheduling hiccup does not fail
##	the gate.
fTimeMs(){
	local cli="$1" input="$2" mode="${3:-set}" best=0 ms start end
	for _ in 1 2; do
		start="$(date +%s%N)"
		if [[ "${mode}" == check ]]; then
			"${cli}" check "${input}" > /dev/null 2>&1 || true
		else
			"${cli}" set "${doc}" < "${input}" > /dev/null 2>&1 || true
		fi
		end="$(date +%s%N)"
		ms=$(( (end - start) / 1000000 ))
		if ((best == 0 || ms < best)); then best="${ms}"; fi
	done
	printf '%s' "${best}"
}

declare -i nBad=0
for b in "${bindings[@]}"; do
	name="${b%%|*}"; cli="${b#*|}"
	baseMs="$(fTimeMs "${cli}" "${tmpDir}/base.ops")"
	## A floor, so a binding fast enough to land near the clock's resolution is
	## not judged on noise.
	budget=$(( baseMs * factor ))
	floor=$(( baseMs + 250 ))
	if ((budget < floor)); then budget="${floor}"; fi
	for w in writes defaults reads badlines; do
		if [[ "${w}" == badlines ]]; then
			ms="$(fTimeMs "${cli}" "${badDoc}" check)"
		else
			ms="$(fTimeMs "${cli}" "${tmpDir}/${w}.ops")"
		fi
		if ((ms > budget)); then
			echo "perf-gate: ${name}: ${w} took ${ms} ms against a ${budget} ms budget (parse-only baseline ${baseMs} ms)" >&2
			nBad+=1
		else
			echo "perf-gate: ${name}: ${w} ${ms} ms, budget ${budget} ms (baseline ${baseMs} ms)"
		fi
	done
done

if ((nBad)); then
	echo "perf-gate: ${nBad} workload(s) over budget" >&2
	exit 1
fi
echo "perf-gate: OK: ${keys} keys, ${#bindings[@]} binding(s) within ${factor}x their own parse baseline"

##	History:
##		2026-08-30  Created after two superlinear write regressions reached dev
##		            in consecutive rounds with no numeric gate to catch them.
##		2026-09-01  badlines workload: a document of refused lines, which went
##		            quadratic through the retained-trivia list.
