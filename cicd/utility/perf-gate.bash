#!/usr/bin/env bash

##	Purpose:
##		Fail when the read, write or malformed-input path stops being linear.
##		Two rounds running, a fix that was correct made bulk writes 4.5x slower
##		and absent-path defaults 140x slower, and both reached dev because
##		nothing had a number to fail on. A third round found a plain text file
##		parsing in quadratic time, and a fourth the did-you-mean suggestion
##		quadratic in name length. Each workload is timed against the same
##		binding's parse-only baseline on the same machine, so the gate carries
##		no wall-clock constant and does not care how fast the runner is:
##		applying the ops, refusing every line, or suggesting a name for every
##		unknown field, must stay small beside reading a well-formed document
##		of the same size. A per-op index rebuild, a scan of every sibling, a
##		rewalk of the retained lines, or a full edit-distance table per name
##		pair breaks that ratio by more than an order of magnitude.
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
## Unknown fields with long names against a schema of long names, all one
## length so the length prefilter rejects nothing: every pair must cost time
## linear in the length, not the full table. 30 x 30 pairs at 800 characters
## is 2 s to 90 s across the bindings with a full table and well under their
## baselines with a banded one.
sugSchema="${tmpDir}/sug-schema.shcl"; sugDoc="${tmpDir}/sug.shcl"
awk 'BEGIN{ pad = sprintf("%794s", ""); gsub(/ /, "x", pad); for (i = 0; i < 30; i++) printf "field: s%05d%s\n", i, pad }' > "${sugSchema}"
awk 'BEGIN{ pad = sprintf("%794s", ""); gsub(/ /, "x", pad); for (i = 0; i < 30; i++) printf "u%05d%s: 1\n", i, pad }' > "${sugDoc}"
## A fragment that mounts itself by two paths, against a document nesting
## sixty levels deep with one unknown field at the bottom. The unknown-field
## sweep has to decide that chain by descending the mounts, and a matcher that
## walks both mounts at every level is 2^60 there; one that remembers a
## (fragment, depth) it has already failed is linear. The same document with
## no unknown field never enters the matcher, so that shape proves nothing.
recSchema="${tmpDir}/rec-schema.shcl"; recDoc="${tmpDir}/rec.shcl"
printf 'fragment: f\n\tfield: child\n\t\tinherits: f\n\tfield: child.child\n\t\tinherits: f\nfield: root\n\tinherits: f\n' > "${recSchema}"
awk 'BEGIN{ print "root:"; for (i = 1; i <= 60; i++) { for (j = 0; j < i; j++) printf "\t"; print "child:" }
	for (j = 0; j <= 60; j++) printf "\t"; print "unknown: 1" }' > "${recDoc}"

##	Milliseconds for one run of $2 (an ops file, a document when $3 is
##	"check", or a document validated against ${sugSchema} when $3 is
##	"suggest") through CLI $1, best of two so a scheduling hiccup does not
##	fail the gate.
##	The exit code and the size of what came out are checked too: the timer used
##	to discard both, so a CLI that printed a usage error and exited 1 was inside
##	every budget and the gate reported OK. A run that did not do the work is a
##	failure, not a fast one.
fTimeMs(){
	local cli="$1" input="$2" mode="${3:-set}" want="${4:-1}" best=0 ms start end rc lines
	for _ in 1 2; do
		start="$(date +%s%N)"
		rc=0
		if [[ "${mode}" == check ]]; then
			"${cli}" check "${input}" > "${tmpDir}/out" 2>/dev/null || rc=$?
		elif [[ "${mode}" == suggest ]]; then
			"${cli}" check --schema "${sugSchema}" "${input}" > "${tmpDir}/out" 2>/dev/null || rc=$?
		elif [[ "${mode}" == recurse ]]; then
			"${cli}" check --schema "${recSchema}" "${input}" > "${tmpDir}/out" 2>/dev/null || rc=$?
		else
			"${cli}" set "${doc}" < "${input}" > "${tmpDir}/out" 2>/dev/null || rc=$?
		fi
		end="$(date +%s%N)"
		##	`check` on a document with diagnostics exits 6; everything else here
		##	succeeds. Anything else means the run did no work.
		local wantRc=0
		[[ "${mode}" == check || "${mode}" == suggest || "${mode}" == recurse ]] && wantRc=6
		if ((rc != wantRc)); then
			echo "perf-gate: ${cli##*/}: ${mode} run exited ${rc}, expected ${wantRc} - it did not do the work" >&2
			printf '%s' -1; return
		fi
		lines="$(wc -l < "${tmpDir}/out")"
		if ((lines < want)); then
			echo "perf-gate: ${cli##*/}: ${mode} run printed ${lines} line(s), expected at least ${want}" >&2
			printf '%s' -1; return
		fi
		ms=$(( (end - start) / 1000000 ))
		if ((best == 0 || ms < best)); then best="${ms}"; fi
	done
	printf '%s' "${best}"
}

declare -i nBad=0
for b in "${bindings[@]}"; do
	name="${b%%|*}"; cli="${b#*|}"
	##	Every `set` run prints the whole document, so the line count is the key
	##	count; `check` prints one line per diagnostic plus a summary.
	baseMs="$(fTimeMs "${cli}" "${tmpDir}/base.ops" set "${keys}")"
	if ((baseMs < 0)); then nBad+=1; continue; fi
	## A floor, so a binding fast enough to land near the clock's resolution is
	## not judged on noise.
	budget=$(( baseMs * factor ))
	floor=$(( baseMs + 250 ))
	if ((budget < floor)); then budget="${floor}"; fi
	for w in writes defaults reads badlines suggest recurse; do
		if [[ "${w}" == badlines ]]; then
			ms="$(fTimeMs "${cli}" "${badDoc}" check 2)"
		elif [[ "${w}" == suggest ]]; then
			ms="$(fTimeMs "${cli}" "${sugDoc}" suggest 2)"
		elif [[ "${w}" == recurse ]]; then
			ms="$(fTimeMs "${cli}" "${recDoc}" recurse 2)"
		else
			ms="$(fTimeMs "${cli}" "${tmpDir}/${w}.ops" set "${keys}")"
		fi
		if ((ms < 0)); then
			nBad+=1
		elif ((ms > budget)); then
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
##		2026-09-02  suggest workload: long unknown names against long schema names,
##		            which cost a full edit-distance table per pair.
##		2026-09-05  recurse workload: an unknown field deep under a fragment that
##		            mounts itself twice, which doubled the chain walk per level.
