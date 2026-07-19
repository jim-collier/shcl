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
##		crosscheck.bash --corpus DIR [--extra DIR] NAME|CLI [NAME|CLI ...]
##		  --corpus DIR  conformance corpus root (case dirs with input.shcl etc.)
##		  --extra DIR   also compare `fmt` over every *.shcl in this directory
##		  NAME|CLI      binding name + its CLI path; first entry is the reference
##	Exit: 0 all agree (or nothing to compare), 1 divergence, 2 usage/missing input.
##	History: At bottom of script.

##	Copyright © 2026 Jim Collier (ID: 1cv◂‡Vᛦ)
##	Licensed under The MIT License (MIT). Full text at:
##		https://mit-license.org/
##	SPDX-License-Identifier: MIT


set -Eeuo pipefail

corpus=""; extra=""; bindings=()
while (($#)); do case "$1" in
	--corpus)  corpus="${2:-}"; shift 2 ;;
	--extra)   extra="${2:-}"; shift 2 ;;
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

##	Run one CLI invocation, capturing stdout and exit code ("<code>\n<stdout>").
##	stderr is dropped: diagnostics wording is per-binding voice, not contract.
fRun(){
	local cli="$1"; shift
	local out rc=0
	out="$("$cli" "$@" 2>/dev/null)" || rc=$?
	printf '%s\n%s' "$rc" "$out"
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
	fCompare "fmt $(basename "$caseDir")" fmt "$input"
	tsv="${caseDir}reads.tsv"
	if [[ -f "$tsv" ]]; then
		while IFS=$'\t' read -r query type _expected _status level _rest; do
			[[ -z "$query" || "$query" == "query" ]] && continue
			fReadRow "$input" "$query" "$type" "${level:-}"
		done < "$tsv"
	fi
done

if [[ -n "$extra" && -d "$extra" ]]; then
	for f in "$extra"/*.shcl; do
		[[ -e "$f" ]] || continue
		fCompare "fmt $(basename "$f")" fmt "$f"
	done
fi

if ((nBad)); then
	echo "crosscheck: ${nBad}/${nCompared} comparison(s) diverged"
	exit 1
fi
echo "crosscheck: ${#bindings[@]} bindings agree on ${nCompared} comparison(s)"


##	Script history:
##		- 20260712 JC: Created.
