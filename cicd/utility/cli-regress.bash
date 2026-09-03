#!/usr/bin/env bash

##	Purpose:
##		Pin CLI behavior the conformance corpus structurally cannot reach: a
##		closed stdin or stdout, '-' named twice on one command line, a carriage
##		return at the end of an ops line, the shape of an op-script error, and
##		whether a read failure still names its cause. Plus the one thing about
##		the help text a diff between bindings cannot see: how wide it is. Every
##		row runs against every binding and is checked against a fixed
##		expectation, not against the other bindings - four-way agreement proves
##		parity, not correctness, and each of these was a defect all four shared.
##
##		stdout and the exit code are contract and are matched exactly. stderr
##		wording is per-binding voice, so a row matches it with a regex that has
##		to hold for all four.
##	Syntax:
##		cli-regress.bash NAME|CLI [NAME|CLI ...]
##	Exit: 0 = every row passes everywhere, 1 = a row failed, 2 = usage.
##	History: At bottom of script.

##	Copyright © 2026 Bubbles (ID: XଌฅრX۳ᛟԃლፀƅꓩหδლც)
##	Licensed under The MIT License (MIT). Full text at:
##		https://mit-license.org/
##	SPDX-License-Identifier: MIT


set -Eeuo pipefail

bindings=()
while (($#)); do case "$1" in
	-h|--help) grep -E '^##' "$0" | sed 's/^##\t\?//'; exit 0 ;;
	*)         bindings+=("$1"); shift ;;
esac; done
((${#bindings[@]})) || { echo "cli-regress: no bindings given" >&2; exit 2; }
for b in "${bindings[@]}"; do
	cli="${b#*|}"
	[[ -x "${cli}" ]] || { echo "cli-regress: binding CLI not executable: ${cli}" >&2; exit 2; }
done

tmpDir="$(mktemp -d)"; trap 'rm -rf "${tmpDir}"' EXIT
printf 'a: 1\n'          > "${tmpDir}/ok.shcl"
printf 'a: 1\n  bad\nb 2\n' > "${tmpDir}/bad.shcl"
mkdir -p "${tmpDir}/adir"
## The deepest a document can legally go: one level under the 512 cap. Python
## was the binding still recursing a frame per level, so this is the shape that
## would exhaust its stack.
awk 'BEGIN{ for (i = 0; i < 511; i++) { for (j = 0; j < i; j++) printf "\t"; printf "n%d:\n", i }
	for (j = 0; j < 511; j++) printf "\t"; printf "leaf: 1\n" }' > "${tmpDir}/deep.shcl"
## A schema whose own default breaks the field's constraints. Generation used to
## emit it anyway, so the starter config failed the schema that produced it.
printf 'field: server.port\n\ttype: int\n\trequired: yes\n\tmin: 1\n\tmax: 10\n\tdefault: 99\n' > "${tmpDir}/baddef.shcl"
## A must-exist path with no name to generate: repeat 1 is a must-exist bound
## like `required`, and generation cannot satisfy it, so the schema is faulted
## rather than a starter that fails its first check. Repeat 2 is the one
## documented shortfall and generates.
printf 'field: "*"\n\ttype: int\n\trepeat: 1\n' > "${tmpDir}/star1.shcl"
printf 'field: "*"\n\ttype: int\n\trepeat: 2\n' > "${tmpDir}/star2.shcl"
## The generation field ceiling, either side of it: the cap used to fire AT the
## limit while its message said past it.
awk 'BEGIN{ for (i = 0; i < 10000; i++) printf "field: f%d\n", i }' > "${tmpDir}/cap10000.shcl"
awk 'BEGIN{ for (i = 0; i < 10001; i++) printf "field: f%d\n", i }' > "${tmpDir}/cap10001.shcl"
## A schema whose own load has something to say: two `field: a` instances merge,
## so `allowed` repeats as a bare leaf - which is exactly what the V092 under it
## is about, and it was invisible.
printf 'field: a\n\tallowed: x\nfield: a\n\tallowed: y\n' > "${tmpDir}/hintschema.shcl"
## A must-exist path with nothing to generate from: an index selector needs an
## instance that is not there, and a path past the nesting cap would draw E016
## on the way back in. Either way the fault names the path rather than reporting
## the generated config as missing it.
printf 'field: "srv[#1].port"\n\trequired: yes\n' > "${tmpDir}/idxreq.shcl"
## A schema that does not build: the report is the build faults alone, not the
## faults plus what an empty document would owe the schema.
printf 'field: a\n\ttype: int\n\trequired: yes\nfield: b\n\ttype: nope\n' > "${tmpDir}/nobuild.shcl"
## An instance whose discriminator holds an '=', which is what made --set's own
## split ambiguous.
printf 'x[a=b]:\n\tc: 0\n' > "${tmpDir}/sel.shcl"
## A name a path cannot hold bare, for the traversal commands: enumerating keys
## is only useful if what comes back can be read straight back.
printf 'db:\n\thost: h\n\t"odd.key": 2\nweb:\n\tport: 1\n' > "${tmpDir}/tree.shcl"
## Two plain keys, for the edit options: what each one leaves behind is the
## whole assertion, so the document has to be small enough to spell out.
printf 'a: 1\nb: 2\n' > "${tmpDir}/two.shcl"

##	Rows: id | argv | stdin | rc | stdout | stderr-regex
##	argv placeholders: %F% the good file, %B% the two-error file, %D% a directory,
##	%P% the deepest legal document, %S% the self-contradicting schema, %S1%/%S2%
##	a nameless must-exist path at repeat 1 and 2, %S3% a schema that does not
##	build, %S4% a required path with an index selector, %S5%/%S6% a schema at and
##	one past the generation field ceiling, %S7% a schema whose own load hints,
##	%X% an
##	instance whose discriminator holds an '=', %T% a document with a name that
##	needs quoting in a path, %F2% a two-key file for the edit options, %M% a
##	path with no file at it.
##	stdin: printf %b text, '-' none, '@closedin' / '@closedout' close that
##	stream, '@fullout' / '@fullerr' point it at a device that is always full.
##	stdout and stderr: '-' means unchecked; an empty stdout field means exactly empty.
##	A stderr regex starting with '!' must match NO line.
##	Each row names the round and item it pins.
rows=(
	## 20260830 item 15: a second '-' read an empty document that looked like an answer.
	'dup-stdin-schema|check --schema=- -|a: 1\n|1||named only once'
	'dup-stdin-layer|get --layer=- - a|a: 1\n|1||named only once'
	## 20260830 item 10: the reference kept a trailing CR on an ops line, the ports stripped it.
	'ops-line-cr|set %F%|int\tx\t1\r|0|a: 1\n\nx: 1\n|-'
	'ops-lone-cr|set %F%|int\tx\t1\n\r|0|a: 1\n\nx: 1\n|-'
	## 20260830 item 14: Python raised a traceback, C exited nonzero.
	'closed-stdin|fmt -|@closedin|0||^$'
	'closed-stdout|fmt %F%|@closedout|0|-|^$'
	## 20260830 item 16: C dropped the line number and the offending op.
	'bad-op-unknown|set %F%|bogus\ta\t1\n|1|-|op line 1: unknown op: bogus'
	## 20260830 item 18: C answered a directory with a bare "read error". Exit 8
	## since 20260830b item 22 split I/O out of the usage code.
	'read-dir-names-error|fmt %D%|-|8|-|[Ii]s a directory'
	## 20260830 item 17: C printed a bare count instead of naming the diagnostics.
	'strict-load-list|fmt --strictness=strict %B%|-|6|-|strict load failed: 2 error diagnostic'
	## 20260830 round: an unknown command is judged before its options.
	'unknown-cmd-before-opts|bogus --nope %F%|-|1|-|unknown command: bogus'
	## 20260829 item 10: Python recursed a frame per level in three places.
	'deep-nesting|fmt %P%|-|0|-|^$'
	## 20260830b item 4: init emitted a config that fails the schema that made it.
	'init-bad-default|init --schema=%S%|-|6||V097 generated value fails the schema'
	## 20260901 item 5: the self-check waved every V007 through, so a repeat
	## lower bound of 1 - a must-exist path - went out as a config that fails
	## its own schema at exit 0.
	'init-star-repeat1|init --schema=%S1%|-|6||V097 required path cannot be generated'
	'init-star-repeat2|init --schema=%S2%|-|0|-|^$'
	## 20260902 item 19: an index selector or a path past the cap got the
	## self-check's "required path missing", which points at the config rather
	## than at the schema line nothing can generate.
	'init-index-required|init --schema=%S4%|-|6||V097 required path cannot be generated: srv\[#1\].port'
	## 20260902 item 20: V096 fired at exactly the ceiling, on a schema with no
	## fragments, saying the schema expands past it.
	'init-cap-at-limit|init --no-banner --schema=%S5%|-|0|-|^$'
	'init-cap-over|init --no-banner --schema=%S6%|-|6||V096 schema expands past 10000 fields'
	'init-build-fault|init --schema=%S3%|-|6||V091 unknown schema type'
	'init-build-fault-only|init --schema=%S3%|-|6||!V002'
	## 20260830 item 35: -h and --help after FILE were an unknown option, though
	## every other option is read there.
	'help-after-file|get %F% -h|-|0|-|-'
	'help-after-file-long|get %F% --help|-|0|-|-'
	## 20260830 item 47: at the default strictness a recovered-from typo was
	## silent unless --write was passed, so stdout carried the repair with
	## nothing said about it.
	'fmt-diags-without-write|fmt %B%|-|0|-|E015 missing colon'
	'set-diags-without-write|set --set=a=2 %B%|-|0|-|E015 missing colon'
	## 20260829 item 6: --set split PATH from VALUE at the first '=' anywhere, so
	## a selector holding one could not be addressed at all.
	'set-eq-in-selector|set --set=x[a=b].c=1 %X%|-|0|x: a=b\n\tc: 1\n|-'
	## 20260830b item 18: a read below strict returned the value and said nothing
	## about a line the load had dropped, so a damaged file read clean at exit 0.
	'get-diags|get %B% a|-|0|1|E015 missing colon'
	'count-diags|count %B% a|-|0|1|E015 missing colon'
	'instances-diags|instances %B% a|-|0|1|E015 missing colon'
	## 20260830b item 22: usage and I/O shared exit 1, so a script could not
	## tell "the command line is wrong" from "that file is not there".
	'io-missing-file|get %M% a|-|8|-|-'
	'io-missing-layer|fmt --layer=%M% %F%|-|8|-|-'
	'io-missing-check|check %M%|-|8|-|-'
	'io-missing-schema|init --schema=%M%|-|8|-|-'
	'usage-unknown-option|get --nope %F% a|-|1|-|unknown option'
	'usage-bad-write-path|set --set=a[*]=1 %F%|-|1|-|wildcard path cannot be written'
	## 20260830b item 21: removal and the set-if-absent family had no option
	## form, so a one-key edit meant a printf with a literal tab piped into set.
	## The five spellings share one ordered list, so the last one on a path wins.
	'remove-option|set --remove=b %F2%|-|0|a: 1\n|-'
	'set-default-absent|set --set-default=c=3 %F2%|-|0|a: 1\nb: 2\n\nc: 3\n|-'
	'set-default-present|set --set-default=a=9 %F2%|-|0|a: 1\nb: 2\n|-'
	'set-literal-default|set --set-literal-default=p=1,2 %F2%|-|0|a: 1\nb: 2\n\np: 1, 2\n|-'
	'set-family-order|set --set=a=5 --remove=a %F2%|-|0|b: 2\n|-'
	'remove-ephemeral|get --remove=a %F2% a|-|3|-|-'
	'remove-write-refused|fmt --write --remove=a %F2%|-|1|-|cannot be combined with --remove'
	'remove-empty-path|set --remove= %F2%|-|1|-|bad --remove value'
	## 20260830b item 19: a script could read an open section's values but never
	## learn its keys, so the only route was parsing fmt output in shell. A name
	## needing quotes comes back path-ready, or enumerating it buys nothing.
	'children-top|children %T%|-|0|db\nweb|-'
	'children-quoted|children %T% db|-|0|host\n"odd.key"|-'
	'children-missing|children %T% nope|-|0||-'
	'paths-all|paths %T%|-|0|db\ndb.host\ndb."odd.key"\nweb\nweb.port|-'
	## 20260902 item 41: the schema's own diagnostics were never printed, so the
	## hint that explains a schema fault could not be seen.
	'schema-own-hints|check --schema=%S7% %F%|-|6|-|schema line 4: Hint: H001'
	## 20260902 item 41: extra tab-separated fields were dropped, so a raw whose
	## content held a literal tab lost everything after it at exit 0.
	'ops-extra-fields-raw|set %F%|raw\tk\t\tbody\twith\ttabs\n|1|-|raw takes 4 tab-separated'
	'ops-extra-fields-int|set %F%|int\tk\t1\textra\n|1|-|int takes 3 tab-separated'
	'ops-array-takes-any|set %F%|int-array\tk\t1\t2\t3\n|0|a: 1\n\nk: 1, 2, 3\n|-'
	## 20260902 item 41: --default and --on-bad=error each overwrote the other's
	## mode, so which one applied depended on which was typed last.
	'default-vs-onbad|get --int --default=7 --on-bad=error %F% nope|-|1|-|--default cannot be combined with --on-bad=error'
	'onbad-vs-default|get --int --on-bad=error --default=7 %F% nope|-|1|-|--default cannot be combined with --on-bad=error'
	'default-with-onbad-default|get --int --default=7 --on-bad=default %F% nope|-|0|7|-'
	'default-alone|get --int --default=7 %F% nope|-|0|7|-'
	## 20260902 item 15: a refused edit returned before the load's diagnostics
	## were printed, so a damaged file said nothing about the damage.
	'refused-set-still-reports|get --set=a[*]=1 %B% a|-|1|-|E015 missing colon'
	'refused-op-still-reports|set %B%|int\ta[*]\t1\n|1|-|E015 missing colon'
	## 20260902 item 14: Go read a non-UTF-8 ops script as a usage error.
	'ops-not-utf8|set %F%|\xff\n|8|-|-'
	## 20260902 items 8 and 9: a stdout that could not be written was reported
	## as success by three CLIs, and a stderr that could not be written aborted
	## the reference with nothing on stdout at all.
	'full-stdout-fmt|fmt %F%|@fullout|8|-|[Nn]o space left'
	'full-stdout-check|check %F%|@fullout|8|-|-'
	'full-stdout-get|get %F% a|@fullout|8|-|-'
	'full-stdout-set|set --set=a=2 %F%|@fullout|8|-|-'
	'full-stderr-keeps-stdout|fmt %B%|@fullerr|0|a: 1\n\tbad:\nb 2\n|-'
	## Found working 20260830b item 18: a merge does not carry diagnostics, so
	## reading them off the merged doc reported the lowest layer and stayed
	## silent about FILE - the one file the caller actually named.
	'layer-base-diags|fmt --layer=%F% %B%|-|0|-|E015 missing colon'
	'layer-base-diags-set|set --set=q=1 --layer=%F% %B%|-|0|-|E015 missing colon'
)

declare -i nRun=0 nBad=0

for row in "${rows[@]}"; do
	IFS='|' read -r id argv stdinSpec wantRc wantOut wantErr <<<"${row}"
	argv="${argv//%F%/${tmpDir}/ok.shcl}"
	argv="${argv//%B%/${tmpDir}/bad.shcl}"
	argv="${argv//%D%/${tmpDir}/adir}"
	argv="${argv//%P%/${tmpDir}/deep.shcl}"
	argv="${argv//%S%/${tmpDir}/baddef.shcl}"
	argv="${argv//%S1%/${tmpDir}/star1.shcl}"
	argv="${argv//%S2%/${tmpDir}/star2.shcl}"
	argv="${argv//%S3%/${tmpDir}/nobuild.shcl}"
	argv="${argv//%S4%/${tmpDir}/idxreq.shcl}"
	argv="${argv//%S5%/${tmpDir}/cap10000.shcl}"
	argv="${argv//%S6%/${tmpDir}/cap10001.shcl}"
	argv="${argv//%S7%/${tmpDir}/hintschema.shcl}"
	argv="${argv//%X%/${tmpDir}/sel.shcl}"
	argv="${argv//%T%/${tmpDir}/tree.shcl}"
	argv="${argv//%F2%/${tmpDir}/two.shcl}"
	argv="${argv//%M%/${tmpDir}/not-there.shcl}"
	## A device that is always full exists on linux and not on windows; the
	## rows that need one are skipped out loud rather than passing vacuously.
	if [[ "${stdinSpec}" == @full* && ! -w /dev/full ]]; then
		echo "cli-regress: skipping ${id} (no /dev/full here)"
		continue
	fi
	read -r -a args <<<"${argv}"
	for b in "${bindings[@]}"; do
		name="${b%%|*}"; cli="${b#*|}"
		rc=0
		case "${stdinSpec}" in
			@closedin)  "${cli}" "${args[@]}" >"${tmpDir}/out" 2>"${tmpDir}/err" 0<&- || rc=$? ;;
			@closedout) "${cli}" "${args[@]}" 2>"${tmpDir}/err" >&- || rc=$?; : >"${tmpDir}/out" ;;
			@fullout)   "${cli}" "${args[@]}" 2>"${tmpDir}/err" >/dev/full || rc=$?; : >"${tmpDir}/out" ;;
			@fullerr)   "${cli}" "${args[@]}" >"${tmpDir}/out" 2>/dev/full || rc=$?; : >"${tmpDir}/err" ;;
			-)          "${cli}" "${args[@]}" >"${tmpDir}/out" 2>"${tmpDir}/err" </dev/null || rc=$? ;;
			*)          printf '%b' "${stdinSpec}" | "${cli}" "${args[@]}" >"${tmpDir}/out" 2>"${tmpDir}/err" || rc=$? ;;
		esac
		nRun+=1
		if ((rc != wantRc)); then
			echo "cli-regress: ${id} [${name}]: exit ${rc}, expected ${wantRc}" >&2; nBad+=1; continue
		fi
		if [[ "${wantOut}" != "-" ]]; then
			gotOut="$(cat "${tmpDir}/out")"
			expOut="$(printf '%b' "${wantOut}")"
			if [[ "${gotOut}" != "${expOut}" ]]; then
				echo "cli-regress: ${id} [${name}]: stdout ${gotOut@Q}, expected ${expOut@Q}" >&2; nBad+=1; continue
			fi
		fi
		if [[ "${wantErr}" != "-" ]]; then
			## The stdin notice is a prompt, not a diagnostic; it is not what a row is about.
			gotErr="$(grep -v 'reading write-ops from stdin' "${tmpDir}/err" || true)"
			if [[ "${wantErr}" == !* ]]; then
				if grep -qE -- "${wantErr#!}" <<<"${gotErr}"; then
					echo "cli-regress: ${id} [${name}]: stderr ${gotErr@Q} matches /${wantErr#!}/" >&2; nBad+=1
				fi
			elif ! grep -qE -- "${wantErr}" <<<"${gotErr}"; then
				echo "cli-regress: ${id} [${name}]: stderr ${gotErr@Q} does not match /${wantErr}/" >&2; nBad+=1
			fi
		fi
	done
done

## The help text is a column-aligned table sitting at exactly 80 wide, and it is
## hand-duplicated in four CLIs, so one added word wraps it in every terminal at
## once and nothing else here would notice. Only help is checked: about and
## donate carry the copyright symbol and the CryptogID by design, so a byte
## count is not a column count there, and neither is aligned anyway.
maxCols=80
for b in "${bindings[@]}"; do
	name="${b%%|*}"; cli="${b#*|}"
	for cmd in help --help; do
		text="$("${cli}" "${cmd}" 2>/dev/null </dev/null || true)"
		nRun+=1
		if [[ -z "${text}" ]]; then
			echo "cli-regress: help-width [${name}]: ${cmd} printed nothing" >&2; nBad+=1; continue
		fi
		## ASCII first: it is what makes the byte count below a column count.
		if LC_ALL=C grep -q '[^ -~]' <<<"${text}"; then
			echo "cli-regress: help-width [${name}]: ${cmd} is not plain ASCII" >&2; nBad+=1
		fi
		if grep -q "$(printf '\t')" <<<"${text}"; then
			echo "cli-regress: help-width [${name}]: ${cmd} prints hard tabs" >&2; nBad+=1
		fi
		while IFS= read -r wide; do
			echo "cli-regress: help-width [${name}]: ${cmd} line ${wide}" >&2; nBad+=1
		done < <(LC_ALL=C awk -v m="${maxCols}" 'length($0) > m { print NR " is " length($0) " columns: " substr($0, 1, 40) }' <<<"${text}")
	done
done

if ((nBad)); then
	echo "cli-regress: ${nBad} of ${nRun} check(s) failed" >&2
	exit 1
fi
echo "cli-regress: OK: ${#rows[@]} row(s) across ${#bindings[@]} binding(s), ${nRun} check(s)"

##	History:
##		2026-08-30  Created, pinning the CLI defects from the 20260829 and
##		            20260830 rounds that no corpus case can express.
##		2026-08-31  Help width, after the help text was found sitting at exactly
##		            80 columns with nothing to fail on.
