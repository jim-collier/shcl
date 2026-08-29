#!/usr/bin/env bash

##	Purpose:
##		Prove every binding still parses a document far larger than anything the
##		corpus holds. The corpus is 56 small cases and the fuzz set is smaller
##		still, so nothing else in the pipeline would notice a parser going
##		quadratic, a buffer that only grows wrong past a few megabytes, or a
##		memory profile that puts a real config out of reach on a modest machine.
##		Generates one document, formats it through every binding, and requires
##		them to agree byte for byte on the result - then checks the reference's
##		own invariants at that scale and gates wall clock and peak memory.
##	Syntax:
##		largedoc.bash [--mib N] [--keep] NAME|CLI [NAME|CLI ...]
##		  --mib N   document size to generate, in MiB (default 100)
##		  --keep    leave the working directory behind for inspection
##		  NAME|CLI  binding label and its CLI path; the first is the reference
##	Exit: 0 = all bindings agree and stay inside their limits, 1 = they do not.
##	History: At bottom of script.

##	Copyright © 2026 Bubbles (ID: XଌฅრX۳ᛟԃლፀƅꓩหδლც)
##	Licensed under The MIT License (MIT). Full text at:
##		https://mit-license.org/
##	SPDX-License-Identifier: MIT


set -Eeuo pipefail

# shellcheck source-path=SCRIPTDIR
source "$(dirname -- "${BASH_SOURCE[0]}")/include/largedoc-gen.bash"   ## largedoc_gen()

mib=100
keep=0
bindings=()
while (($#)); do case "$1" in
	--mib)    mib="$2"; shift 2 ;;
	--keep)   keep=1; shift ;;
	-h|--help) sed -n '/^##	Purpose:/,/^##	History:/p' "${BASH_SOURCE[0]}" | sed '$d; s/^##	\{0,1\}//'; exit 0 ;;
	-*)       echo "largedoc: unknown option: $1" >&2; exit 2 ;;
	*)        bindings+=("$1"); shift ;;
esac; done

((${#bindings[@]})) || { echo "largedoc: no bindings given" >&2; exit 2; }
if ! [[ "${mib}" =~ ^[1-9][0-9]*$ ]]; then echo "largedoc: --mib wants a positive integer" >&2; exit 2; fi

## Per-binding ceilings, "name|seconds per input MiB|peak RSS MiB per input MiB".
## Expressed per-MiB so they follow --mib instead of being pinned to one size.
## Time carries wide headroom - the pipeline runs the reference unoptimized (about
## six times slower than a release build), and a shared CI runner is slower again;
## the target is a growth-rate regression, not a stopwatch. Memory is held much
## closer, since peak RSS barely moves between machines or build profiles.
## Measured at 100 MiB on a workstation: rust 0.53 s/MiB and 21 MiB/MiB, go 0.08
## and 33-41 (concurrent GC moves it run to run, so its ceiling carries more
## slack), c 0.04 and 30, python 1.11 and 47.
limits=(
	"rust|3.00|32"
	"go|1.00|55"
	"c|1.00|45"
	"python|4.00|65"
)

fLimit() {   ## $1 = binding name, $2 = field (2=secs, 3=rss); empty when unlisted
	local e
	for e in "${limits[@]}"; do [[ "${e%%|*}" == "$1" ]] && { cut -d'|' -f"$2" <<<"${e}"; return 0; }; done
	return 0   ## an unlisted binding is measured and reported, just not gated
}

work="$(mktemp -d "${TMPDIR:-/tmp}/shcl-largedoc.XXXXXX")"
((keep)) || trap 'rm -rf "${work}"' EXIT

## Room for the document plus the reference's formatted copy, with slack.
avail_kib="$(df -Pk "${work}" | awk 'NR==2{print $4}')"
need_kib=$(( mib * 1024 * 3 ))
((avail_kib >= need_kib)) || {
	echo "largedoc: need ~$((need_kib/1024)) MiB free for ${work}, have $((avail_kib/1024)) MiB" >&2
	exit 1
}

doc="${work}/large.shcl"

## The generator lives in include/largedoc-gen.bash (the profiler workload is
## the same document at a smaller size).
echo "largedoc: generating ${mib} MiB -> ${doc}"
largedoc_gen "${mib}" > "${doc}"

actualMib=$(( $(stat -c%s "${doc}") / 1048576 ))
echo "largedoc: ${actualMib} MiB, $(grep -c '' "${doc}") lines"

## Wall clock and peak RSS for one run. VmHWM is the kernel's own high-water
## mark and only ever climbs, so sampling it is exact up to the last sample.
## The poll is builtins only - a read of /proc and a timed read on a fd that
## never delivers, as the sleep - so twenty samples a second cost no forks.
## Sets runSecs/runRssMib/runRc.
exec {tickFd}<> <(:)
fRunMeasured() {
	local out="$1"; shift
	local pid hwm=0 key val t0 t1
	t0="$(date +%s.%N)"
	"$@" > "${out}" 2>"${work}/stderr" & pid=$!
	while kill -0 "${pid}" 2>/dev/null; do
		while read -r key val _; do
			[[ "${key}" == "VmHWM:" ]] && ((val > hwm)) && hwm="${val}"
		done < "/proc/${pid}/status" 2>/dev/null || true
		read -rt 0.05 -u "${tickFd}" _ || true
	done
	runRc=0; wait "${pid}" || runRc=$?
	t1="$(date +%s.%N)"
	runSecs="$(awk -v a="${t0}" -v b="${t1}" 'BEGIN{printf "%.1f", b-a}')"
	runRssMib=$(( hwm / 1024 ))
}

rc=0
refName="${bindings[0]%%|*}"
refSum=""
printf '\n%-8s %8s %10s   %s\n' "binding" "secs" "peak MiB" "result"

for entry in "${bindings[@]}"; do
	name="${entry%%|*}"; cli="${entry#*|}"
	[[ -r "${cli}" ]] || { printf '%-8s %8s %10s   MISSING: %s\n' "${name}" - - "${cli}"; rc=1; continue; }

	out="${work}/out-${name}.shcl"
	fRunMeasured "${out}" "${cli}" fmt "${doc}"
	note=""
	if ((runRc != 0)); then
		note="FAILED (exit ${runRc}): $(head -c 200 "${work}/stderr" | tr '\n' ' ')"
		rc=1
	else
		sum="$(sha256sum "${out}" | cut -d' ' -f1)"
		if [[ -z "${refSum}" ]]; then refSum="${sum}"; note="reference"
		elif [[ "${sum}" == "${refSum}" ]]; then note="agrees"
		else note="DIFFERS from ${refName}"; rc=1
		fi

		maxSecs="$(fLimit "${name}" 2)"
		if [[ -n "${maxSecs}" ]]; then
			maxRss="$(fLimit "${name}" 3)"
			if awk -v s="${runSecs}" -v m="${maxSecs}" -v n="${actualMib}" 'BEGIN{exit !(s > m*n)}'; then
				note="${note}; TOO SLOW (over $(awk -v m="${maxSecs}" -v n="${actualMib}" 'BEGIN{printf "%.0f", m*n}')s)"; rc=1
			fi
			if ((runRssMib > maxRss * actualMib)); then
				note="${note}; TOO BIG (over $((maxRss * actualMib)) MiB)"; rc=1
			fi
		fi
	fi
	printf '%-8s %8s %10s   %s\n' "${name}" "${runSecs}" "${runRssMib}" "${note}"

	## Only the reference's output is kept, for the invariant checks below.
	[[ "${name}" == "${refName}" ]] || rm -f "${out}"
done

## Reference-only invariants at this scale. Each is another full parse, so the
## list stays short: formatting has to be a fixpoint, and a long array has to
## read back whole - a stale buffer pointer after growth is a defect this
## project has actually shipped, and no small case can see it.
refOut="${work}/out-${refName}.shcl"
if [[ -s "${refOut}" ]]; then
	refCli="${bindings[0]#*|}"
	echo
	if "${refCli}" fmt "${refOut}" | cmp -s - "${refOut}"; then
		echo "largedoc: fmt is a fixpoint at ${actualMib} MiB"
	else
		echo "largedoc: FAILED: fmt is not a fixpoint at ${actualMib} MiB" >&2; rc=1
	fi
	wideCount="$("${refCli}" get --int --array "${refOut}" wide | grep -c '' || true)"
	if ((wideCount == 20000)); then
		echo "largedoc: long array read back whole (${wideCount} elements)"
	else
		echo "largedoc: FAILED: long array read back ${wideCount} of 20000 elements" >&2; rc=1
	fi
fi

echo
if ((rc == 0)); then echo "largedoc: OK (${#bindings[@]} binding(s) agree at ${actualMib} MiB)"
else echo "largedoc: FAILED" >&2; fi
((keep)) && echo "largedoc: kept ${work}"
exit "${rc}"


##	History:
##		- 2026-08-19 JC: Created. Nothing else in the pipeline runs a document
##		  bigger than a few kilobytes, so growth-rate and buffer-growth defects
##		  had no gate at all.
##		- 2026-08-29 JC: Generator moved to include/largedoc-gen.bash; the memory
##		  poll no longer forks per sample.
