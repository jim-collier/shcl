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

if ((nBad)); then
	echo "check-docs: ${nBad} check(s) failed" >&2
	exit 1
fi
echo "check-docs: OK"

##	History:
##		2026-08-30  Created, after a double negative reversed the legal-advice
##		            disclaimer in a document that invites verbatim reuse.
