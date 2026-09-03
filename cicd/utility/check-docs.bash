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

##	Each language example says a setter reports whether the write applied, then
##	three of the four called the first two bare. The Rust one checks all three,
##	because the type system makes it, so it is the comparison a reader has.
readme="${repoDir}/README.md"
if [[ -f "${readme}" ]]; then
	for fence in rust go python c; do
		block="$(awk -v f="^\`\`\`${fence}\$" '$0 ~ f, /^```$/' "${readme}")"
		[[ -n "${block}" ]] || continue
		calls="$(grep -cE '(doc\.[Ss]et[A-Za-z_]+\(|shcl_set_[a-z]+\(doc)' <<<"${block}" || true)"
		checked="$(grep -cE '(if !doc\.[Ss]et|if not doc\.set_|if \(!shcl_set_)' <<<"${block}" || true)"
		((calls == 0)) && continue
		((checked >= calls)) || fBad "README ${fence} example calls ${calls} setter(s) and checks ${checked}"
	done
fi

##	The stamps terminate an item: an outcome bullet below them makes them stop
##	being a reliable end marker, and puts the result furthest from the finding.
while IFS= read -r hit; do
	fBad "backlog.md: sub-bullet below the stamps: ${hit}"
done < <(awk '
	match($0, /^\t+- (Opened|Closed): /) { stamp = NR; indent = length($0) - length(substr($0, RSTART + RLENGTH)); next }
	stamp == NR - 1 && /^\t+- / { print NR ": " substr($0, 1, 60) }
	{ stamp = 0 }' "${backlog}" || true)

##	How a defect was found is not what changed. The gate that caught it belongs
##	in the cause line where it is the point, not in a bullet of its own.
while IFS= read -r hit; do
	fBad "backlog.md: says how it was found rather than what changed: ${hit}"
done < <(grep -nE '^[[:space:]]*- Found (by|while) ' "${backlog}" || true)

##	V096 and V097 come only from generation, so validating anything against the
##	schema cannot reproduce them - the veneer header used to send a reader that
##	way for the fault list, which returns the validated document's own V002 and
##	V007 instead. The C CLI made the same mistake and was fixed in 20260830b.
while IFS= read -r hit; do
	fBad "shcl.hpp: sends a reader to validate() for generation faults: ${hit}"
done < <(grep -nE 'for the fault list, validate\(\)' "${repoDir}/source/c/shcl.hpp" || true)

##	A bare `Mon DD, YYYY` is two array elements, not a date: the comma splits
##	first. The bullet listing that spelling has to say so, or a reader copies it
##	unquoted out of the spec and gets a BadType.
grep -q 'in the space form a comma may follow the day .*only inside quotes' "${repoDir}/project/spec.md" \
	|| fBad "spec.md: the Mon DD, YYYY bullet does not say the comma spelling needs quotes"

##	Prose that names the CLI's edit options, or the subcommands that take a
##	layer, goes stale the moment one is added. Each claim is checked against the
##	shipped help text rather than against a copy of the list.
help="$("${repoDir}/source/rust/target/debug/shcl" help 2>/dev/null || true)"
if [[ -n "${help}" ]]; then
	for opt in --remove --set-default --set-literal-default; do
		grep -q -- "${opt}" <<<"${help}" || continue
		grep -q -- "\`${opt}" "${repoDir}/README.md" \
			|| fBad "README.md: ${opt} is in the help and not in the edit-options paragraph"
		grep -qF -- "${opt//-/\\-}" "${repoDir}/source/man/shcl.1" \
			|| fBad "shcl.1: ${opt} is in the help and not in the man page"
	done
	##	The man page's WRITE OPS sentence lists the options that stop stdin
	##	being read; the CLI reads it only when none of the five is given.
	writeops="$(sed -n '/^\.SH WRITE OPS/,/One op per line/p' "${repoDir}/source/man/shcl.1")"
	for opt in '\-\-set' '\-\-set\-literal' '\-\-set\-default' '\-\-set\-literal\-default' '\-\-remove'; do
		grep -qF -- "${opt}" <<<"${writeops}" \
			|| fBad "shcl.1: WRITE OPS does not name ${opt//\\/} among the options that carry the edits"
	done
fi

##	Every subcommand that takes a layer has to be in the spec's list of them.
##	Driven off the CLI rather than off a copy: the list went stale twice.
if [[ -n "${help}" ]]; then
	#  shellcheck disable=2016  ## the backticks are the document's own markdown.
	layerLine="$(grep -n 'takes repeated `--layer=FILE`' "${repoDir}/project/spec.md" | head -1 || true)"
	[[ -n "${layerLine}" ]] || fBad "spec.md: no sentence listing the subcommands that take --layer"
	tmpErr="$(mktemp)"
	for cmd in get fmt count instances children paths set check; do
		"${repoDir}/source/rust/target/debug/shcl" "${cmd}" --layer=/dev/null /dev/null a < /dev/null > /dev/null 2>"${tmpErr}" || true
		grep -qE 'unknown option|not valid for' "${tmpErr}" && continue
		grep -qF -- "\`${cmd}\`" <<<"${layerLine}" \
			|| fBad "spec.md: ${cmd} takes --layer and is not in the list of subcommands that do"
	done
	rm -f "${tmpErr}"
fi

##	Every subcommand that loads a document prints the load's diagnostics, so a
##	README transcript reading a damaged file has to show them - the get example
##	sat under a check example that showed the same file's diagnostic and said
##	nothing itself.
readmeGet="$(sed -n '/shcl get server.shcl log-level/,/^```$/p' "${repoDir}/README.md")"
grep -q 'E014' <<<"${readmeGet}" \
	|| fBad "README.md: the get transcript on the damaged file shows no load diagnostic"

##	Three merge facts a consumer folding layers itself has to know, and that
##	nothing in the code or the corpus can tell them: the fold is not
##	associative, the merged document keeps the base's strictness, and a
##	replaced node is kept. The spec says them, and so does every binding's
##	merge doc comment - a port that loses one leaves its own users guessing.
grep -q 'fold is not associative' "${repoDir}/project/spec.md" \
	|| fBad "spec.md: Layered loading does not say the fold is not associative"
for src in source/rust/src/lib.rs source/go/shcl.go source/python/shcl.py source/c/shcl.h; do
	grep -q 'fold is not associative' "${repoDir}/${src}" \
		|| fBad "${src}: the merge doc comment does not say the fold is not associative"
done

##	Two rules every port has to implement identically and that lived only in the
##	code: which way a float-to-int tie rounds at Loose, and the order the
##	aggregate status of an array read takes its worst slot in.
#  shellcheck disable=2016  ## the backticks are the document's own markdown.
grep -q 'rounds half away from zero' "${repoDir}/project/spec.md" \
	|| fBad "spec.md: the coercion table does not say which way a float-to-int tie rounds"
#  shellcheck disable=2016  ## the backticks are the document's own markdown.
grep -qF -- '`Good` < `Empty` < `NotFound` < `BadType` < `Multiple`' "${repoDir}/project/spec.md" \
	|| fBad "spec.md: the aggregate status rule does not give the ordering"

if ((nBad)); then
	echo "check-docs: ${nBad} check(s) failed" >&2
	exit 1
fi
echo "check-docs: OK"

##	History:
##		2026-08-30  Created, after a double negative reversed the legal-advice
##		            disclaimer in a document that invites verbatim reuse.
