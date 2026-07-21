#!/usr/bin/env bash

#  shellcheck disable=2155  ## 'Declare and assign separately.' Cumbersome and unnecessary here.

##	Purpose:
##		Cross-binding differential check: every shipped binding must agree with
##		every other, byte for byte, on the same inputs - not just each pass the
##		corpus on its own. The first binding listed is the reference (Rust); each
##		other binding's CLI is run over the conformance corpus (canonical `fmt` of
##		every input.shcl, plus every reads.tsv row replayed as a `get`/`count`/
##		`instances` call) and, when given, over an extra directory of fuzz-dumped
##		inputs (`fmt` only). stdout and exit code must both match the reference.
##		With fewer than two bindings there is nothing to compare - prints a note
##		and exits 0, so it can stay wired into cicd from day one.
##	Syntax:
##		crosscheck.bash --corpus DIR [--extra DIR] [--min N] NAME|CLI [NAME|CLI ...]
##		  --corpus DIR  conformance corpus root (case dirs with input.shcl etc.)
##		  --extra DIR   also compare `fmt` over every *.shcl in this directory
##		  --min N       fail unless at least N comparisons ran (default 1, so a
##		                collapsed corpus/dump can't pass on zero)
##		  NAME|CLI      binding name + its CLI path; first entry is the reference
##	Exit: 0 all agree, 1 divergence, 2 usage/missing input or too few comparisons.
##	History: At bottom of script.

##	Copyright © 2026 Jim Collier (ID: 1cv◂‡Vᛦ)
##	Licensed under The MIT License (MIT). Full text at:
##		https://mit-license.org/
##	SPDX-License-Identifier: MIT


set -Eeuo pipefail

corpus=""; extra=""; bindings=(); declare -i minCompared=1
while (($#)); do case "$1" in
	--corpus)  corpus="${2:-}"; shift 2 ;;
	--extra)   extra="${2:-}"; shift 2 ;;
	--min)     minCompared="${2:-1}"; shift 2 ;;
	-h|--help) grep -E '^##' "$0" | sed 's/^##\t\?//'; exit 0 ;;
	*)         bindings+=("$1"); shift ;;
esac; done

[[ -d "$corpus" ]] || { echo "crosscheck: no corpus dir: $corpus" >&2; exit 2; }
if ((${#bindings[@]} < 2)); then
	echo "crosscheck: ${#bindings[@]} binding(s) configured - differential comparison activates when a second binding lands"
	exit 0
fi
for b in "${bindings[@]}"; do
	cli="${b#*|}"
	[[ -x "$cli" ]] || { echo "crosscheck: binding CLI not executable: $cli" >&2; exit 2; }
done

refName="${bindings[0]%%|*}"; refCli="${bindings[0]#*|}"
declare -i nCompared=0 nBad=0

##	Run one CLI invocation, capturing stdout and exit code as "<code>\n<stdout>X".
##	stderr is dropped: diagnostics wording is per-binding voice, not contract.
##	Two sentinel tricks: the inner 'X<rc>' rides rc inside the capture so the
##	substitution always exits 0 (no -e trip) and no separate rc grab is needed;
##	the trailing 'X' on our own output means the caller's `want="$(fRun ...)"`
##	has nothing to strip, so a dropped or doubled final newline stays visible.
fRun(){
	local cli="$1"; shift
	local out
	out="$("$cli" "$@" 2>/dev/null; printf 'X%d' "$?")"
	printf '%s\n%sX' "${out##*X}" "${out%X*}"
}

##	Like fRun, but feeds a file to stdin (the `set` write-ops script).
fRunStdin(){
	local cli="$1" stdinFile="$2"; shift 2
	local out
	out="$("$cli" "$@" <"$stdinFile" 2>/dev/null; printf 'X%d' "$?")"
	printf '%s\n%sX' "${out##*X}" "${out%X*}"
}

##	Compare every other binding against the reference for one stdin-fed call.
fCompareStdin(){
	local what="$1" stdinFile="$2"; shift 2
	local want got b name cli
	want="$(fRunStdin "$refCli" "$stdinFile" "$@")"
	for b in "${bindings[@]:1}"; do
		name="${b%%|*}"; cli="${b#*|}"
		got="$(fRunStdin "$cli" "$stdinFile" "$@")"
		nCompared+=1
		if [[ "$got" != "$want" ]]; then
			nBad+=1
			echo "DIVERGE ${what}: ${name} vs ${refName} (shcl $* <${stdinFile})"
			diff <(printf '%s\n' "$want") <(printf '%s\n' "$got") | head -12
		fi
	done
}

##	Compare every other binding against the reference for one invocation.
fCompare(){
	local what="$1"; shift
	local want got b name cli
	want="$(fRun "$refCli" "$@")"
	for b in "${bindings[@]:1}"; do
		name="${b%%|*}"; cli="${b#*|}"
		got="$(fRun "$cli" "$@")"
		nCompared+=1
		if [[ "$got" != "$want" ]]; then
			nBad+=1
			echo "DIVERGE ${what}: ${name} vs ${refName} (shcl $*)"
			diff <(printf '%s\n' "$want") <(printf '%s\n' "$got") | head -12
		fi
	done
}

##	Map one reads.tsv row to a CLI call. Columns: query, type, expected, status,
##	optional level. expected/status are the corpus contract (each binding's own
##	conformance runner asserts those); here only binding-vs-binding agreement
##	matters, so the row is just a recipe for an invocation.
fReadRow(){
	local input="$1" query="$2" type="$3" level="$4"
	local -a strictArg=()
	[[ -n "$level" ]] && strictArg=("--strictness=${level}")
	case "$type" in
		load)         fCompare "load" check "${strictArg[@]}" "$input"
		              fCompare "fmt ${level:-standard}" fmt "${strictArg[@]}" "$input" ;;
		count)        fCompare "count ${query}" count "${strictArg[@]}" "$input" "$query" ;;
		instances)    fCompare "instances ${query}" instances "${strictArg[@]}" "$input" "$query" ;;
		*'[]')        fCompare "get ${query} ${type}" get "--${type%[]}" --array "${strictArg[@]}" "$input" "$query"
		              fCompare "get ${query} ${type} slots" get "--${type%[]}" --array --slots "${strictArg[@]}" "$input" "$query" ;;
		*)            fCompare "get ${query} ${type}" get "--${type}" "${strictArg[@]}" "$input" "$query" ;;
	esac
}

for caseDir in "$corpus"/*/; do
	input="${caseDir}input.shcl"
	[[ -f "$input" ]] || continue
	# bash variables cannot hold a NUL, so $() capture would silently drop it and
	# warn. Such cases (e.g. the merge-key NUL case) are pinned by the native
	# conformance runners instead; skip them here, out loud.
	if [[ "$(LC_ALL=C tr -dc '\000' < "$input" | wc -c)" -ne 0 ]]; then
		echo "crosscheck: skipping $(basename "$caseDir") (NUL in input; native runners pin it)"
		continue
	fi
	fCompare "fmt $(basename "$caseDir")" fmt "$input"
	# Write dimension: apply the case's ops script and compare canonical output.
	ops="${caseDir}write.ops"
	[[ -f "$ops" ]] && fCompareStdin "set $(basename "$caseDir")" "$ops" set "$input"
	tsv="${caseDir}reads.tsv"
	if [[ -f "$tsv" ]]; then
		while IFS=$'\t' read -r query type _expected _status level _rest || [[ -n "$query" ]]; do
			[[ -z "$query" || "$query" == "query" ]] && continue
			fReadRow "$input" "$query" "$type" "${level:-}"
		done < "$tsv"
	fi
done

if [[ -n "$extra" && -d "$extra" ]]; then
	declare -i nExtra=0
	for f in "$extra"/*.shcl; do
		[[ -e "$f" ]] || continue
		nExtra+=1
		# Same NUL limitation as the corpus loop; silently skip (a dump can be large).
		[[ "$(LC_ALL=C tr -dc '\000' < "$f" | wc -c)" -ne 0 ]] && continue
		fCompare "fmt $(basename "$f")" fmt "$f"
	done
	if ((nExtra == 0)); then
		echo "crosscheck: --extra ${extra} matched no *.shcl (empty fuzz dump?)" >&2
		exit 2
	fi
fi

if ((nBad)); then
	echo "crosscheck: ${nBad}/${nCompared} comparison(s) diverged"
	exit 1
fi
if ((nCompared < minCompared)); then
	echo "crosscheck: only ${nCompared} comparison(s), need at least ${minCompared} (corpus/dump collapsed?)" >&2
	exit 2
fi
echo "crosscheck: ${#bindings[@]} bindings agree on ${nCompared} comparison(s)"


##	Script history:
##		- 20260712 JC: Created.
##		- 20260721 JC: Preserve trailing newlines in compares; zero-comparison and
##		               empty-extra floors; keep the last reads.tsv row when the
##		               file has no trailing newline; skip NUL-bearing inputs (bash
##		               can't hold a NUL; native runners pin those).
