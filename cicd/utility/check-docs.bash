#!/usr/bin/env bash

##	Purpose:
##		Document invariants that no linter checks and that a review round has
##		already found broken. Prose is mostly a matter of taste and stays out of
##		here; what goes in is a claim the documents make about themselves.
##	Syntax:
##		check-docs.bash [ROOT]
##	Exit: 0 = clean, 1 = a check failed, 2 = usage or missing input.
##	History: At bottom of script.

##	Copyright © 2026 Bubbles (ID: XଌฅრX۳ᛟԃლፀƅꓩหδლც)
##	Licensed under The MIT License (MIT). Full text at:
##		https://mit-license.org/
##	SPDX-License-Identifier: MIT


set -Eeuo pipefail

repoDir="${1:-$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)}"
[[ -d "${repoDir}" ]] || { echo "check-docs: no such directory: ${repoDir}" >&2; exit 2 ;}
declare -i nBad=0
fBad(){ echo "check-docs: $1" >&2; nBad+=1 ;}

##	A disclaimer that says the opposite of what it means is worse than none, and
##	these documents invite verbatim reuse, so the error travels. One negation in
##	the sentence is the disclaimer; two is "None of this is not legal advice".
##	The backlog quotes the broken wording as a finding, so it is not a document
##	making the claim and does not belong here.
while IFS= read -r hit; do
	fBad "reversed legal-advice disclaimer: ${hit}"
done < <(grep -rn "legal advice" --include='*.md' "${repoDir}" \
	| grep -v '/backlog\.md:' \
	| awk -F: 'BEGIN { neg = "(^|[^a-z])(no|none|not|never|nothing|nobody)([^a-z]|$)" }
	{
		loc = $1 ":" $2
		text = $0
		sub(/^[^:]*:[0-9]+:/, "", text)
		n = split(text, part, /\. /)
		for (i = 1; i <= n; i++) {
			if (part[i] !~ /legal advice/) continue
			low = tolower(part[i]); cnt = 0
			while (match(low, neg)) { cnt++; low = substr(low, RSTART + RLENGTH - 1) }
			if (cnt > 1) print loc ": " part[i]
		}
	}' || true)

##	The backlog states an order for its finished sections and then drifted out of
##	it twice: rounds, loose items, rounds again. Loose items first, rounds after,
##	each run newest first.
backlog="${repoDir}/project/backlog.md"
if [[ -f "${backlog}" ]]; then
	while IFS= read -r problem; do fBad "${problem}"; done < <(awk '
		/^#+ / {
			if (heading != "") check()
			heading = ($0 ~ /Done|Canceled/) ? $0 : ""
			seenRound = 0; prevRound = ""; problem = ""
			next
		}
		heading != "" && /^- / {
			isRound = ($0 ~ /^- Code review [0-9]{8}[a-z]?:/)
			if (isRound) {
				tag = $0; sub(/^- Code review /, "", tag); sub(/:.*$/, "", tag)
				if (prevRound != "" && tag > prevRound && problem == "") problem = heading ": rounds out of order at " tag
				prevRound = tag; seenRound = 1
			} else if (seenRound && problem == "") {
				problem = heading ": a loose item sits below a code-review round"
			}
		}
		END { if (heading != "") check() }
		function check() { if (problem != "") print problem }
	' "${backlog}")
fi

##	The documents list five integration modes, two of which are a shared library.
##	Nothing in the tree builds one - no crate-type, no export macro in the C
##	header, and the release stage produces binaries, packages and the drop-in
##	tarball. So a document may describe compiling one, but may not offer it as
##	something already built. If a build for it is ever added, this check is what
##	says the wording may go back.
buildsSharedLib=0
if grep -rqE 'crate-type[^=]*=[^]]*(cdylib|dylib|staticlib)' --include='Cargo.toml' "${repoDir}"; then
	buildsSharedLib=1
fi
if ((! buildsSharedLib)); then
	while IFS= read -r hit; do
		fBad "offers a shared library as already built, and nothing builds one: ${hit}"
	done < <(grep -rniE '(prebuilt|pre-built|published|shipped)[^.]*\.(so|dll|dylib)|\b(prebuilt|pre-built)\b[^.]*shared librar' \
		--include='*.md' "${repoDir}" | grep -v '/backlog\.md:' || true)
fi

##	The Windows installer refuses outright without `tar`, and the README's
##	prerequisites used to cover only the Linux side, so the requirement was
##	reachable only by running it and failing.
ps1="${repoDir}/install.ps1"
readme="${repoDir}/README.md"
if [[ -f "${ps1}" && -f "${readme}" ]] && grep -q "needs tar to unpack" "${ps1}"; then
	grep -qE '(^|[^a-z])tar([^a-z]|$).*[Ww]indows|[Ww]indows.*(^|[^a-z])tar([^a-z]|$)' "${readme}" \
		|| fBad "install.ps1 requires tar and README.md never says so on the Windows side"
fi

##	Two top-level bullets with no blank line between them. Auto-generated TOC
##	blocks are the exception - the tool strips blank lines out of them, so a
##	`<!-- TOC -->` region is skipped, as is any list of bare anchor links, which
##	is what a hand-maintained contents block looks like.
while IFS= read -r f; do
	while IFS= read -r hit; do
		fBad "adjacent top-level bullets: ${f#"${repoDir}/"}:${hit}"
	done < <(awk '
		/<!-- TOC -->/      { toc = 1 }
		/<!-- \/TOC -->/    { toc = 0 }
		{
			isBullet = ($0 ~ /^- /)
			anchor   = ($0 ~ /^- \[[^]]*\]\(#[^)]*\)[[:space:]]*$/)
			if (!toc && !anchor && prevBullet && isBullet) print NR ": " substr($0, 1, 60)
			prevBullet = isBullet && !anchor
			prev = $0
		}' "${f}" || true)
done < <(find "${repoDir}" -name '*.md' -not -path '*/target/*' -not -path '*/.git/*' -not -path '*/node_modules/*' | sort)

##	The claim that the code goes to stdout and the prose to stderr. The stderr
##	line carries the code too, and has since the round that changed the CLI's
##	voice - so a document saying otherwise describes a split that is not there.
while IFS= read -r hit; do
	fBad "says the prose alone goes to stderr, but the stderr line carries the code: ${hit}"
done < <(grep -rn "prose to stderr" --include='*.md' "${repoDir}" | grep -v '/backlog\.md:' || true)

if ((nBad)); then
	echo "check-docs: ${nBad} check(s) failed" >&2
	exit 1
fi
echo "check-docs: OK"

##	History:
##		2026-08-30  Created, after a double negative reversed the legal-advice
##		            disclaimer in a document that invites verbatim reuse.
