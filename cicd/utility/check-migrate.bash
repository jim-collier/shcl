#!/usr/bin/env bash

#  shellcheck disable=2155  ## 'Declare and assign separately.' Cumbersome and unnecessary here.

##	Purpose:
##		The migration gate: for every corpus input and every fuzz-dumped
##		document, the current parser's reading of `migrate`'s output equals the
##		2.x parser's reading of the original. Read, not canonical text: the two
##		emitters spell a value differently on purpose (single quotes for a
##		backslash, no `\'`), so the trees are compared through the reads - the
##		path list, the instance count at every path, and each instance's
##		string array, raw body and info string. The 2.x side is a build of the
##		last pre-cut dev commit, pinned below by hash and built into its own
##		gitignored target the way the pre-push gate builds, so the comparison
##		never rests on whatever `shcl` is on PATH.
##
##		Compared are the documents 2.x read cleanly: no line it kept as
##		malformed (a line 2.x could not read may read as a binding now, which
##		is a gain, not a migration), no bracket array it counted lost (2.x
##		itself refused to save that), no dropped line of any kind. Every other
##		document must come through equal, with two named exceptions: a fence
##		label holding a `#`, which 2.x ran to the end of the line and which
##		ends at the `#` now, with no quoting to spell it; and a carriage return
##		in the middle of a line, which 2.x kept as content and which is a blank
##		at a piece's edge now. Each is asserted on a corpus case, so the list
##		cannot rot.
##	Syntax:
##		check-migrate.bash [--corpus DIR] [--iters N] [--min N]
##		  --corpus DIR  conformance corpus root (default project/conformance)
##		  --iters N     fuzz iterations to dump (default 2000)
##		  --min N       fail unless at least N documents were compared (default 100)
##	Exit: 0 = every compared document equal, 1 = a divergence, 2 = usage or a build failure.
##	History: At bottom of script.

##	Copyright (c) 2026 Bubbles
##	Licensed under The MIT License (MIT). Full text at:
##		https://mit-license.org/
##	SPDX-License-Identifier: MIT


set -Eeuo pipefail

meDir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd -- "${meDir}/../.." && pwd)"
corpus="${root}/project/conformance"; declare -i iters=2000 minCompared=100
while (($#)); do case "$1" in
	--corpus)  corpus="${2:-}"; shift 2 ;;
	--iters)   iters="${2:-2000}"; shift 2 ;;
	--min)     minCompared="${2:-100}"; shift 2 ;;
	-h|--help) grep -E '^##' "$0" | sed 's/^##\t\?//'; exit 0 ;;
	*)         echo "check-migrate: unknown argument: $1" >&2; exit 2 ;;
esac; done
[[ -d "${corpus}" ]] || { echo "check-migrate: no corpus dir: ${corpus}" >&2; exit 2; }

##	The last dev commit before the lexical cut: the funnel merge.
pin="1a337e76dfeff27c5eb15e6c620a9a49ba9c3760"
if ! git -C "${root}" cat-file -e "${pin}^{commit}" 2>/dev/null; then
	## A shallow clone (the hosted runner) has no history; a full sha can still
	## be fetched on its own.
	git -C "${root}" fetch --depth=1 origin "${pin}" >/dev/null 2>&1 || true
	git -C "${root}" cat-file -e "${pin}^{commit}" 2>/dev/null \
		|| { echo "check-migrate: the pinned 2.x commit ${pin:0:12} is not in this clone" >&2; exit 2; }
fi

tmpDir="$(mktemp -d)"; tree=""
fCleanup(){
	if [[ -n "${tree}" ]]; then git -C "${root}" worktree remove --force "${tree}" >/dev/null 2>&1 || true; fi
	git -C "${root}" worktree prune >/dev/null 2>&1 || true
	rm -rf "${tmpDir}"
}
trap fCleanup EXIT

##	Build the 2.x CLI once per pin; the target dir keeps it across runs.
oldTarget="${root}/source/rust/target-migrate"
oldCli="${oldTarget}/debug/shcl"
if [[ ! -x "${oldCli}" || "$(cat "${oldTarget}/.pin" 2>/dev/null || true)" != "${pin}" ]]; then
	echo "check-migrate: building the 2.x reference at ${pin:0:12}"
	tree="$(mktemp -d "${TMPDIR:-/tmp}/shcl-migrate.XXXXXX")"
	git -C "${root}" worktree add --detach "${tree}" "${pin}" >/dev/null 2>&1 \
		|| { echo "check-migrate: cannot check ${pin:0:12} out" >&2; exit 2; }
	mkdir -p "${oldTarget}"
	ln -s "${oldTarget}" "${tree}/source/rust/target"
	( cd "${tree}/source/rust" && cargo build --quiet ) || { echo "check-migrate: the 2.x build failed" >&2; exit 2; }
	printf '%s\n' "${pin}" > "${oldTarget}/.pin"
	fCleanup; tree=""; tmpDir="$(mktemp -d)"
fi
newCli="${root}/source/rust/target/debug/shcl"
[[ -x "${newCli}" ]] || ( cd "${root}/source/rust" && cargo build --quiet )

##	The soup, from the current fuzz generator.
dump="${tmpDir}/dump"; mkdir -p "${dump}"
( cd "${root}/source/rust" && SHCL_FUZZ_DUMP="${dump}" SHCL_FUZZ_ITERS="${iters}" SHCL_FUZZ_DUMP_MAX=500 \
	cargo test --quiet --test fuzz_smoke mutated_inputs >/dev/null 2>&1 ) || { echo "check-migrate: the fuzz dump failed" >&2; exit 2; }

##	2.x read this document cleanly when every code it reports is one that
##	binds the line: E001 (field kept), E005 (fence unterminated, body kept),
##	E015 (repaired), E017 (kept literally), and the two hints.
fClean2x(){
	local codes
	codes="$("${oldCli}" check "$1" 2>/dev/null | { grep -oE '(E|H)[0-9]{3}' || true; } | sort -u)"
	[[ -z "${codes}" ]] && return 0
	grep -vqE '^(E001|E005|E015|E017|H001|H002)$' <<<"${codes}" && return 1
	return 0
}

##	Everything a tree is, read through one CLI: the paths, the count at each,
##	and per instance the string array, the raw body and the info string, with
##	the exit codes, one line per read. A path holding a tab cannot ride the
##	loop; those are left to the native runners.
fReadTree(){
	local cli="$1" doc="$2" p n i q
	"${cli}" paths "${doc}" 2>/dev/null || echo "paths exit $?"
	while IFS= read -r p; do
		[[ -n "${p}" && "${p}" != *$'\t'* ]] || continue
		n="$("${cli}" count "${doc}" "${p}" 2>/dev/null || true)"
		echo "count ${p} = ${n}"
		[[ "${n}" =~ ^[0-9]+$ ]] || continue
		for ((i = 0; i < n; i++)); do
			q="${p}[#${i}]"
			"${cli}" get --string --array "${doc}" "${q}" 2>/dev/null || echo "string exit $?"
			"${cli}" get --raw "${doc}" "${q}" 2>/dev/null || echo "raw exit $?"
			"${cli}" get --rawinfo "${doc}" "${q}" 2>/dev/null || echo "rawinfo exit $?"
		done
	done < <("${cli}" paths "${doc}" 2>/dev/null || true)
}

##	One read answers differently by decision rather than by migration: the info
##	string of an empty binding was BadType in 2.x and is Empty now, matching the
##	raw-content read on the same node. The two reads sit next to each other in
##	the tree above, so the 2.x side's answer is brought forward here rather than
##	dropping the info string from the comparison entirely.
##	Each read prints its (empty) value before its status line, so the raw status
##	sits two lines back.
fAge2xReads(){ awk '$0 == "rawinfo exit 4" && prev == "" && prev2 == "raw exit 2" { $0 = "rawinfo exit 2" } { print; prev2 = prev; prev = $0 }'; }

##	A NUL-bearing document cannot ride a command substitution; the native
##	runners pin those.
fHasNul(){ IFS= read -r -d '' _ <"$1"; }

##	A 2.x fence label carrying a `#` has no spelling here at all: the `#` opens
##	the line's comment now, and nothing quotes a label. So `migrate` leaves the
##	line as written and the reading differs by design. Matched on the text
##	rather than by file name, because a fuzz document's number moves every time
##	the corpus grows.
fInfoHashLabel(){ grep -qE '(```|~~~)[^#]*#' "$1"; }

##	A carriage return with more text after it on its line. At a piece's edge it
##	was content to 2.x and is trimmed now; in the middle of a piece it reads the
##	same either way, and the corpus pins that, so matching loosely costs little.
fCrMidLine(){ grep -q $'\r[^\r]' "$1"; }

declare -i nCompared=0 nSkipped=0 nBad=0
for f in "${corpus}"/*/input.shcl "${dump}"/*.shcl; do
	[[ -f "${f}" ]] || continue
	name="${f%/input.shcl}"; name="${name##*/}"
	if fHasNul "${f}"; then nSkipped+=1; continue; fi
	if ! fClean2x "${f}"; then nSkipped+=1; continue; fi
	if fInfoHashLabel "${f}" || fCrMidLine "${f}"; then nSkipped+=1; continue; fi
	"${newCli}" migrate "${f}" > "${tmpDir}/migrated.shcl" 2>/dev/null || true
	want="$(fReadTree "${oldCli}" "${f}" | fAge2xReads)"
	got="$(fReadTree "${newCli}" "${tmpDir}/migrated.shcl")"
	nCompared+=1
	if [[ "${want}" != "${got}" ]]; then
		nBad+=1
		echo "check-migrate: DIVERGE ${name}: the 2.x reads of the original and the current reads of the migrated text differ"
		diff <(printf '%s\n' "${want}") <(printf '%s\n' "${got}") | head -12 || true
	fi
done

##	Each named case has to keep carrying its shape, or the exception is stale.
fInfoHashLabel "${corpus}/068-info-hash-spellings/input.shcl" 2>/dev/null \
	|| { echo "check-migrate: 068-info-hash-spellings no longer carries a fence label holding a #" >&2; nBad+=1; }
fCrMidLine "${corpus}/094-unicode-space/input.shcl" 2>/dev/null \
	|| { echo "check-migrate: 094-unicode-space no longer carries a mid-line carriage return" >&2; nBad+=1; }
if ((nCompared < minCompared)); then
	echo "check-migrate: only ${nCompared} document(s) compared, need at least ${minCompared} (${nSkipped} skipped)" >&2
	exit 2
fi
if ((nBad)); then
	echo "check-migrate: ${nBad} divergence(s) over ${nCompared} document(s) (${nSkipped} skipped as not clean under 2.x)" >&2
	exit 1
fi
echo "check-migrate: OK: ${nCompared} document(s) migrate to the tree 2.x read (${nSkipped} skipped as not clean under 2.x or carrying a shape 2.x could spell and this cannot)"

##	History:
##		2026-09-08  Created with the 3.0 lexical cut, pinned on the funnel merge.
