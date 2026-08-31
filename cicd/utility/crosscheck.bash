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

##	Copyright © 2026 Bubbles (ID: XଌฅრX۳ᛟԃლፀƅꓩหδლც)
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
tmpDir="$(mktemp -d)"; trap 'rm -rf "$tmpDir"' EXIT
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

##	Run one CLI invocation, leaving "<code>\n<stdout>" in runOut. stderr is
##	dropped: diagnostics wording is per-binding voice, not contract. stdout goes
##	through a file and a builtin read rather than $(...): a substitution strips
##	trailing newlines, so a dropped or doubled final one would be invisible, and
##	it would cost two more forks per launch on top of the CLI's own - there are
##	about eight thousand launches in a run.
runOut=""
fRun(){
	local cli="$1"; shift
	local rc=0
	"$cli" "$@" >"${tmpDir}/run.out" 2>/dev/null || rc=$?
	fReadOut "$rc"
}

##	Like fRun, but feeds a file to stdin (the `set` write-ops script).
fRunStdin(){
	local cli="$1" stdinFile="$2"; shift 2
	local rc=0
	"$cli" "$@" <"$stdinFile" >"${tmpDir}/run.out" 2>/dev/null || rc=$?
	fReadOut "$rc"
}

##	read -d '' takes the file whole, trailing newlines included, and returns 1
##	at EOF, which is the normal end here. A NUL in stdout ends the read early -
##	the same limit $(...) had, since a bash string cannot hold one.
fReadOut(){
	local out=""
	IFS= read -r -d '' out <"${tmpDir}/run.out" || true
	runOut="$1"$'\n'"${out}"
}

##	Compare every other binding against the reference for one stdin-fed call.
fCompareStdin(){
	local what="$1" stdinFile="$2"; shift 2
	local want got b name cli
	fRunStdin "$refCli" "$stdinFile" "$@"; want="${runOut}"
	for b in "${bindings[@]:1}"; do
		name="${b%%|*}"; cli="${b#*|}"
		fRunStdin "$cli" "$stdinFile" "$@"; got="${runOut}"
		nCompared+=1
		if [[ "$got" != "$want" ]]; then
			nBad+=1
			echo "DIVERGE ${what}: ${name} vs ${refName} (shcl $* <${stdinFile})"
			diff <(printf '%s\n' "$want") <(printf '%s\n' "$got") | head -12 || true
		fi
	done
}

##	Compare every other binding against the reference for one invocation.
fCompare(){
	local what="$1"; shift
	local want got b name cli
	fRun "$refCli" "$@"; want="${runOut}"
	for b in "${bindings[@]:1}"; do
		name="${b%%|*}"; cli="${b#*|}"
		fRun "$cli" "$@"; got="${runOut}"
		nCompared+=1
		if [[ "$got" != "$want" ]]; then
			nBad+=1
			echo "DIVERGE ${what}: ${name} vs ${refName} (shcl $*)"
			diff <(printf '%s\n' "$want") <(printf '%s\n' "$got") | head -12 || true
		fi
	done
}

##	Describe a tree so an in-place write can be compared by its side effects.
##	One sorted line per path: relative name, type, mode, link count, symlink
##	target, content. Anything the writer leaves behind (a stray temp file, a
##	replaced symlink, a widened mode) shows up as a diff.
fWriteState(){
	local root="$1" p rel
	while IFS= read -r p; do
		rel="${p#"${root}/"}"
		if [[ -L "$p" ]]; then
			printf '%s\tsymlink -> %s\n' "$rel" "$(readlink "$p")"
		elif [[ -d "$p" ]]; then
			printf '%s\t%s\n' "$rel" "$(stat -c '%F mode=%a' "$p")"
		else
			printf '%s\t%s\t%s\n' "$rel" "$(stat -c '%F mode=%a links=%h' "$p")" "$(tr '\n' '|' <"$p")"
		fi
	done < <(find "$root" -mindepth 1 | sort)
}

##	Compare the filesystem side effects of an in-place write, not just stdout.
##	The writer publishes a new inode, so it can silently drop the target's mode
##	or replace a symlink with a regular file - neither of which a stdout compare
##	can see. fixture builds a fresh tree under $1 and echoes the path to pass the
##	CLI; every binding gets its own identical copy.
fCompareWrite(){
	local what="$1" fixture="$2"; shift 2
	local want got b name cli root target
	root="${tmpDir}/write-${refName}"; rm -rf "$root"; mkdir -p "$root"
	target="$("$fixture" "$root")"
	# The exit code rides along: a write that refuses leaves the tree alone, so
	# the state compare alone cannot tell a refusal from a no-op success.
	fRun "$refCli" "$@" "$target"; want="${runOut}$(fWriteState "$root")"
	for b in "${bindings[@]:1}"; do
		name="${b%%|*}"; cli="${b#*|}"
		root="${tmpDir}/write-${name}"; rm -rf "$root"; mkdir -p "$root"
		target="$("$fixture" "$root")"
		fRun "$cli" "$@" "$target"; got="${runOut}$(fWriteState "$root")"
		nCompared+=1
		if [[ "$got" != "$want" ]]; then
			nBad+=1
			echo "DIVERGE ${what}: ${name} vs ${refName} (shcl $* <fixture>)"
			diff <(printf '%s\n' "$want") <(printf '%s\n' "$got") | head -12 || true
		fi
	done
}

##	Same idea, for the one write property a fixture cannot set up in advance:
##	the temp file's name carries the writer's pid, so the decoy can only be
##	planted by the process that is about to become the CLI. `exec` from a
##	subshell hands the CLI that subshell's pid, so `$$` names the file it is
##	about to create. Writing through the decoy would leave `stolen` behind and
##	turn the target into a symlink; both show up in the state compare. The decoy
##	itself is removed before comparing, since its name holds a per-run pid.
fPlantRun(){
	local root="$1" cli="$2"; shift 2
	bash -c 'r="$1"; c="$2"; shift 2; ln -sfn "${r}/stolen" "${r}/.c.shcl.tmp$$.0"; exec "$c" "$@"' _ "$root" "$cli" "$@" >/dev/null 2>&1 || true
	find "$root" -maxdepth 1 -type l -name '.c.shcl.tmp*' -delete
}

fComparePlant(){
	local what="$1"; shift
	local want got b name cli root target
	root="${tmpDir}/plant-${refName}"; rm -rf "$root"; mkdir -p "$root"
	target="$(fFixMode "$root")"
	fPlantRun "$root" "$refCli" "$@" "$target"
	want="$(fWriteState "$root")"
	for b in "${bindings[@]:1}"; do
		name="${b%%|*}"; cli="${b#*|}"
		root="${tmpDir}/plant-${name}"; rm -rf "$root"; mkdir -p "$root"
		target="$(fFixMode "$root")"
		fPlantRun "$root" "$cli" "$@" "$target"
		got="$(fWriteState "$root")"
		nCompared+=1
		if [[ "$got" != "$want" ]]; then
			nBad+=1
			echo "DIVERGE ${what}: ${name} vs ${refName} (shcl $* <fixture>)"
			diff <(printf '%s\n' "$want") <(printf '%s\n' "$got") | head -12 || true
		fi
	done
}

##	Fixtures for fCompareWrite. Each builds its tree and echoes the path to hand
##	the CLI. The unformatted spacing is deliberate: the write must actually
##	rewrite the file, or the checks below prove nothing.
fFixMode(){    printf 'a:  1\n' >"$1/c.shcl"; chmod 600 "$1/c.shcl"; echo "$1/c.shcl"; }
fFixSymlink(){ mkdir -p "$1/real"; printf 'a:  1\n' >"$1/real/c.shcl"; ln -s real/c.shcl "$1/c.shcl"; echo "$1/c.shcl"; }
fFixHardlink(){ printf 'a:  1\n' >"$1/c.shcl"; ln "$1/c.shcl" "$1/other.shcl"; echo "$1/c.shcl"; }
##	A link to a file that is not there yet: the write must create the target
##	behind the link, not turn the link into a file.
fFixDangling(){ mkdir -p "$1/real"; ln -s real/c.shcl "$1/c.shcl"; echo "$1/c.shcl"; }
##	Nothing at the path at all - the create case. The tree it leaves behind is
##	the whole point, so the fixture deliberately builds nothing.
fFixAbsent(){  echo "$1/c.shcl"; }
##	A load that dropped a line canonical output cannot re-emit (a BOM-led one),
##	so the in-place write is the destructive case the save gate exists for.
fFixLost(){    printf 'a:  1\n\xef\xbb\xbfb: 2\n' >"$1/c.shcl"; chmod 600 "$1/c.shcl"; echo "$1/c.shcl"; }

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
		children)     fCompare "children ${query}" children "${strictArg[@]}" "$input" "$query" ;;
		paths)        fCompare "paths" paths "${strictArg[@]}" "$input" ;;
		*'[]')        fCompare "get ${query} ${type}" get "--${type%[]}" --array "${strictArg[@]}" "$input" "$query"
		              fCompare "get ${query} ${type} slots" get "--${type%[]}" --array --slots "${strictArg[@]}" "$input" "$query" ;;
		*)            fCompare "get ${query} ${type}" get "--${type}" "${strictArg[@]}" "$input" "$query"
		              # on-bad=error (exit-code differential; message goes to dropped stderr)
		              # and a default substitution (stdout differential) - the accessor
		              # policy surface, where hand-written ports diverge most easily.
		              fCompare "get ${query} ${type} on-bad=error" get "--${type}" --on-bad=error "${strictArg[@]}" "$input" "$query"
		              fCompare "get ${query} ${type} default" get "--${type}" "--default=<x>" "${strictArg[@]}" "$input" "$query" ;;
	esac
}

##	bash variables cannot hold a NUL, so a captured stdout would drop it. read
##	-d '' stops at a NUL and returns 0 only when it found one, so this probes a
##	file for one with no fork.
fHasNul(){ IFS= read -r -d '' _ <"$1"; }

for caseDir in "$corpus"/*/; do
	input="${caseDir}input.shcl"
	[[ -f "$input" ]] || continue
	caseName="${caseDir%/}"; caseName="${caseName##*/}"
	# NUL-bearing cases (e.g. the merge-key NUL case) are pinned by the native
	# conformance runners instead; skip them here, out loud.
	if fHasNul "$input"; then
		echo "crosscheck: skipping ${caseName} (NUL in input; native runners pin it)"
		continue
	fi
	fCompare "fmt ${caseName}" fmt "$input"
	# Write dimension: apply the case's ops script and compare canonical output.
	ops="${caseDir}write.ops"
	[[ -f "$ops" ]] && fCompareStdin "set ${caseName}" "$ops" set "$input"
	# Bad-op dimension: each write-bad.ops line, applied alone, must produce the
	# same (empty) stdout and same nonzero exit in every binding.
	badops="${caseDir}write-bad.ops"
	if [[ -f "$badops" ]]; then
		badN=0
		while IFS= read -r bline || [[ -n "$bline" ]]; do
			[[ -z "$bline" || "$bline" == \#* ]] && continue
			badN=$((badN+1))
			printf '%s\n' "$bline" > "${tmpDir}/badop.line"
			fCompareStdin "set-bad ${caseName} line ${badN}" "${tmpDir}/badop.line" set "$input"
		done < "$badops"
	fi
	# Layered-load dimension: replay `fmt` with the case's layer*.shcl merged under
	# input.shcl (filename order = priority) plus any merge.sets --set overrides.
	if [[ -f "${caseDir}expected-merged.shcl" ]]; then
		layerArgs=()
		for lf in "${caseDir}"layer*.shcl; do [[ -f "$lf" ]] && layerArgs+=("--layer=$lf"); done
		setArgs=()
		if [[ -f "${caseDir}merge.sets" ]]; then
			while IFS= read -r sline || [[ -n "$sline" ]]; do
				[[ -z "$sline" || "$sline" == \#* ]] && continue
				setArgs+=("--set=$sline")
			done < "${caseDir}merge.sets"
		fi
		fCompare "merge ${caseName}" fmt "${layerArgs[@]}" "${setArgs[@]}" "$input"
	fi
	# Schema dimension: replay check --schema (codes + summary + exit are the contract).
	schema="${caseDir}schema.shcl"
	[[ -f "$schema" ]] && fCompare "check --schema ${caseName}" check "--schema=${schema}" "$input"
	# Generation dimension: replay init --schema (the generated starter is the
	# contract), both with the format footer and with --no-banner.
	initschema="${caseDir}init-schema.shcl"
	if [[ -f "$initschema" ]]; then
		fCompare "init ${caseName}" init "--schema=${initschema}"
		fCompare "init --no-banner ${caseName}" init --no-banner "--schema=${initschema}"
	fi
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
		if fHasNul "$f"; then continue; fi
		fCompare "fmt ${f##*/}" fmt "$f"
		# Derived reads.tsv (the reference dumps one per input, paths it knows exist):
		# replay the accessor rows too, so the fuzz set covers reads, not just fmt.
		reads="${f%.shcl}.reads.tsv"
		if [[ -f "$reads" ]]; then
			while IFS=$'\t' read -r query type _expected _status level _rest || [[ -n "$query" ]]; do
				[[ -z "$query" || "$query" == "query" ]] && continue
				fReadRow "$f" "$query" "$type" "${level:-}"
			done < "$reads"
		fi
	done
	if ((nExtra == 0)); then
		echo "crosscheck: --extra ${extra} matched no *.shcl (empty fuzz dump?)" >&2
		exit 2
	fi
fi

# Usage surface: help/version/bare/unknown are the largest user-visible output
# in the project and are hand-duplicated per CLI, so pin them here too.
fCompare "usage help" help
fCompare "usage help flag" --help
fCompare "usage version" version
fCompare "usage bare"
fCompare "usage unknown" definitely-not-a-subcommand
# about/donate carry both spellings and the blank-line padding; bare help above
# is the control, since it prints the same text with no padding.
fCompare "usage about" about
fCompare "usage about flag" --about
fCompare "usage donate" donate
fCompare "usage donate flag" --donate

# In-place writes: the rename that makes them atomic also replaces the inode, so
# these pin what the target keeps. Mode must survive (config files hold secrets)
# and a symlinked config must be written through, not replaced. The hard-link
# case pins the documented limitation: rename cannot preserve the other name.
fCompareWrite "write keeps mode" fFixMode fmt --write
fCompareWrite "write follows symlink" fFixSymlink fmt --write
fCompareWrite "write creates behind a dangling symlink" fFixDangling set --write --set=a=1
fCompareWrite "write breaks hard link" fFixHardlink fmt --write
fComparePlant "write refuses a planted temp" fmt --write

# The save gate, from the CLI side. Refusing leaves the file byte-identical, so
# the tree compare is what sees it; --lossy is the only way past.
fCompareWrite "write refuses to drop a line" fFixLost fmt --write
fCompareWrite "write --lossy drops it anyway" fFixLost fmt --write --lossy
fCompareWrite "set --write refuses to drop a line" fFixLost set --write --set a=2
fCompareWrite "set --write --lossy drops it anyway" fFixLost set --write --lossy --set a=2
fCompare "--lossy needs --write" fmt --lossy missing.shcl
fCompare "--lossy is not valid for get" get --lossy missing.shcl a

# `set --write --set` persists edits given as options and reads no ops from
# stdin, so nothing here feeds one. The gates around it are pinned too: --layer
# still cannot be written back anywhere, and --set stays ephemeral off 'set'.
fCompareWrite "set --write applies --set" fFixMode set --write --set a=2
fCompareWrite "set --write --set adds a path" fFixMode set --write --set b.c=hello
fCompare "set --write rejects --layer" set --write --layer=missing.shcl missing.shcl
fCompare "fmt --write rejects --set" fmt --write --set a=1 missing.shcl
# `set --write` on a FILE that is not there yet creates it; `fmt --write` has
# nothing to format and still refuses. The state compare covers the created
# file's mode too, which is umask-derived and so must match across bindings.
fCompareWrite "set --write creates a missing file" fFixAbsent set --write --set a=1
fCompareWrite "fmt --write still refuses a missing file" fFixAbsent fmt --write

# `set -` follows stdin, so the same spelling means two things and both are
# pinned: the piped document when an option holds the edits, an empty base when
# stdin is the ops script instead.
printf 'int\tx\t7\n' >"${tmpDir}/emptybase.ops"
fCompareStdin "set - reads the piped document" project/conformance/044-write-literal/input.shcl set - --set b=2
fCompareStdin "set - is an empty base for ops" "${tmpDir}/emptybase.ops" set -

# --set-literal takes value syntax, so the same text lands as an array where
# --set stores one quoted string; the pair is compared to pin that difference.
# The rejections are the parser's own, so they have to agree with it.
fCompareWrite "set --write applies --set-literal" fFixMode set --write --set-literal 'a=80, 443'
fCompare "set --set-literal array" set --set-literal 'a=80, 443' project/conformance/044-write-literal/input.shcl
fCompare "set --set quotes the same text" set --set 'a=80, 443' project/conformance/044-write-literal/input.shcl
fCompare "set --set-literal keeps a quoted element" set --set-literal 'a="x, y", z' project/conformance/044-write-literal/input.shcl
fCompare "set --set-literal rejects an open quote" set --set-literal 'a="oops' project/conformance/044-write-literal/input.shcl
fCompare "set --set-literal wants PATH=VALUE" set --set-literal noequals project/conformance/044-write-literal/input.shcl

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
##		- 20260712: Created.
##		- 20260721: Preserve trailing newlines in compares; zero-comparison and
##		               empty-extra floors; keep the last reads.tsv row when the
##		               file has no trailing newline; skip NUL-bearing inputs (bash
##		               can't hold a NUL; native runners pin those).
##		- 20260724: Layered-load dimension (fmt with --layer/--set) for cases
##		               carrying expected-merged.shcl; generation dimension (init
##		               --schema) for cases carrying init-schema.shcl.
##		- 20260726: In-place write dimension: compare the tree an in-place write
##		               leaves behind (mode, symlink, link count, content), not
##		               just stdout. Also made the DIVERGE diff non-fatal - under
##		               pipefail its nonzero status was aborting the run, so only
##		               the first divergence ever printed and the summary never did.
##		- 20260804: Pin `set --write --set` (edits as options, no ops on stdin)
##		               and the two gates that bound it; then `--set-literal`
##		               beside `--set` on the same text, so the data-vs-syntax
##		               split cannot drift.
##		- 20260829: One fork per CLI launch instead of three (stdout through a
##		               file and a builtin read), and no forks at all for the NUL
##		               probe and the case names.
