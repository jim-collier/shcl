#!/usr/bin/env bash

##	Purpose:
##		Keep the shell completions in step with the CLI. Both completion files
##		carry the same per-subcommand option table the CLI validates against in
##		check_opts(), so an option added to one and not the others would offer a
##		completion the CLI rejects as a usage error - or hide a real one. This
##		diffs all three tables and fails on any disagreement.
##	Syntax:
##		check-completions.bash [ROOT]
##		  ROOT   repo root (default: two levels up from this script)
##	Exit: 0 = the three tables agree, 1 = they do not.
##	History: At bottom of script.

##	Copyright © 2026 Bubbles (ID: XଌฅრX۳ᛟԃლፀƅꓩหδლც)
##	Licensed under The MIT License (MIT). Full text at:
##		https://mit-license.org/
##	SPDX-License-Identifier: MIT


set -Eeuo pipefail

meDir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
root="${1:-$(cd -- "${meDir}/../.." && pwd)}"

mainRs="${root}/source/rust/src/main.rs"
compFiles=("${root}/source/completions/shcl.bash" "${root}/source/completions/_shcl")

for f in "${mainRs}" "${compFiles[@]}"; do
	[[ -f "${f}" ]] || { echo "check-completions: missing ${f}" >&2; exit 1; }
done

## The CLI's table, one arm per line: `get<TAB>--array --default ...`. The arm
## list is flattened to a single line first, then split on the `],` that ends
## each one - option names never contain a bracket, so nothing else can match.
fRustTable() {
	sed -n '/let allowed: &\[&str\] = match cmd {/,/^\t};$/p' "${mainRs}" \
	| tr '\n' ' ' | tr -s ' ' | sed 's/\], /]\n/g' \
	| while read -r arm; do
		[[ "${arm}" == *'=>'* && "${arm}" == *'"'* ]] || continue   ## skips the _ => &[] catch-all
		names="$(grep -o '"[a-z]*"' <<<"${arm%%=>*}" | tr -d '"' | paste -sd'|')"
		opts="$(grep -o -- '--[a-z<>-]*' <<<"${arm#*=>}" | sort -u | paste -sd' ')"
		printf '%s\t%s\n' "${names}" "${opts}"
	done
}

## The same table out of a completion file. Both files spell it identically, so
## one extractor covers them.
fCompTable() {
	sed -n "s/^[[:space:]]*\([a-z|]\+\))[[:space:]]*echo '\([^']*\)'.*/\1\t\2/p" "$1" \
	| while IFS=$'\t' read -r names opts; do
		[[ -n "${opts}" ]] || continue
		printf '%s\t%s\n' "${names}" "$(tr ' ' '\n' <<<"${opts}" | sort -u | paste -sd' ')"
	done
}

rc=0
rustTable="$(fRustTable)"
[[ -n "${rustTable}" ]] || { echo "check-completions: no option table found in ${mainRs}" >&2; exit 1; }

for cf in "${compFiles[@]}"; do
	compTable="$(fCompTable "${cf}")"
	if ! diff <(printf '%s\n' "${rustTable}") <(printf '%s\n' "${compTable}") >/dev/null; then
		echo "check-completions: $(basename "${cf}") disagrees with the CLI (< CLI, > completion):" >&2
		diff <(printf '%s\n' "${rustTable}") <(printf '%s\n' "${compTable}") >&2 || true
		rc=1
	fi
done

((rc)) || echo "check-completions: OK ($(wc -l <<<"${rustTable}") subcommands, ${#compFiles[@]} completion files)"
exit "${rc}"


##	Script history:
##		- 20260818: Created.
