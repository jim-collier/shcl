#!/usr/bin/env bash

##	Purpose:
##		Check the pre-push hook's skip for a tree a gate already passed, on a
##		throwaway repository with the gate stubbed out. The tree green-tree.bash
##		computes has to be the one a commit of the working copy gets, a recorded
##		tree has to let the hook through without the gate, and a push to main
##		with nothing recorded still has to run it. A skip that fires when it
##		should not is a push that reaches main untested, and nothing downstream
##		would say so. Only main is gated, so every other ref is checked for the
##		gate not running at all.
##	Syntax:
##		check-push-gate.bash
##	Exit: 0 = all checks pass, 1 = a check failed (named), 2 = cannot set up.
##	History: At bottom of script.

##	Copyright (c) 2026 Bubbles
##	Licensed under The MIT License (MIT). Full text at:
##		https://mit-license.org/
##	SPDX-License-Identifier: MIT


set -Eeuo pipefail

root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
helper="${root}/cicd/utility/green-tree.bash"
for f in "${helper}" "${root}/cicd/hooks/pre-push" "${root}/.gitignore"; do
	[[ -r "${f}" ]] || { echo "check-push-gate: cannot read ${f}" >&2; exit 2; }
done

## No global or system config reaches the throwaway repository: identity rules,
## a hooks path or commit signing there would change what is being checked.
unset GIT_CONFIG_COUNT GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE SHCL_SKIP_HOOK SHCL_GATE_RERUN STUB_RC
export GIT_CONFIG_GLOBAL=/dev/null GIT_CONFIG_NOSYSTEM=1
export GIT_AUTHOR_NAME=check GIT_AUTHOR_EMAIL=check@example.invalid
export GIT_COMMITTER_NAME=check GIT_COMMITTER_EMAIL=check@example.invalid

work="$(mktemp -d)"
trap 'rm -rf "${work}"' EXIT
## The hook's worktrees and the helper's scratch index go in here too.
export TMPDIR="${work}"

rc=0
fail() { echo "check-push-gate: FAIL: $*" >&2; rc=1; }
repo="${work}/repo"
zeros=0000000000000000000000000000000000000000

git init -q -b dev "${repo}" || { echo "check-push-gate: git init failed" >&2; exit 2; }
mkdir -p "${repo}/cicd/hooks" "${repo}/cicd/utility" "${repo}/source/rust"
cp "${root}/cicd/hooks/pre-push" "${repo}/cicd/hooks/pre-push"
cp "${helper}" "${repo}/cicd/utility/green-tree.bash"
cp "${root}/.gitignore" "${repo}/.gitignore"
cat > "${repo}/cicd/cicd.bash" <<'EOF'
#!/usr/bin/env bash
echo "ran ${SHCL_GATE_REF:-}" >> "${STUB_LOG}"
exit "${STUB_RC:-0}"
EOF
printf 'one\n' > "${repo}/a.txt"
printf 'gone\n' > "${repo}/c.txt"
printf '#!/bin/sh\n' > "${repo}/x.sh"
: > "${repo}/source/rust/.keep"
chmod +x "${repo}/cicd/cicd.bash" "${repo}/cicd/hooks/pre-push" "${repo}/cicd/utility/green-tree.bash" "${repo}/x.sh"
{ git -C "${repo}" add --all && git -C "${repo}" commit -q -m base; } || { echo "check-push-gate: first commit failed" >&2; exit 2; }
helper="${repo}/cicd/utility/green-tree.bash"

## Every kind of change a commit of the working copy picks up: an edit, a new
## file, a deletion, a mode, and an ignored file that must stay out. a.txt is
## staged and then edited again, so the index and the disk disagree.
printf 'two\n' > "${repo}/a.txt"
git -C "${repo}" add a.txt
printf 'three\n' > "${repo}/a.txt"
printf 'new\n' > "${repo}/b.txt"
rm "${repo}/c.txt"
chmod -x "${repo}/x.sh"
mkdir -p "${repo}/cicd/artifacts" && printf 'junk\n' > "${repo}/cicd/artifacts/junk"
staged="$(git -C "${repo}" diff --cached --name-only)"
tree="$("${helper}" tree "${repo}")" || fail "tree exited non-zero on a dirty working copy"
[[ "$(git -C "${repo}" diff --cached --name-only)" == "${staged}" ]] || fail "tree changed the real index"
git -C "${repo}" add --all && git -C "${repo}" commit -q -m work
head="$(git -C "${repo}" rev-parse 'HEAD^{tree}')"
[[ "${tree}" == "${head}" ]] || fail "tree gave ${tree}, and a commit of the same working copy got ${head}"

## The hook gates inside a worktree with a target link of its own added. The
## tree there has to be the commit's, or the run it starts can never record.
git -C "${repo}" worktree add -q --detach "${work}/wt" HEAD
ln -s "${repo}/source/rust/target-gate" "${work}/wt/source/rust/target"
[[ "$("${helper}" tree "${work}/wt" || true)" == "${head}" ]] || fail "the target link the hook adds to its worktree changes the tree"

if "${helper}" passed "${repo}" "${head}"; then fail "a tree nothing recorded passed"; fi
"${helper}" record "${repo}" "${head}" || fail "record refused the tree the working copy holds"
"${helper}" passed "${repo}" "${head}" || fail "a recorded tree did not pass"
"${helper}" passed "${work}/wt" "${head}" || fail "a worktree does not see its clone's recorded trees"
git -C "${repo}" worktree remove --force "${work}/wt"

## Files that change between the start of a run and its end: nothing recorded.
printf 'four\n' > "${repo}/a.txt"
moved="$("${helper}" tree "${repo}")"
printf 'five\n' > "${repo}/a.txt"
if "${helper}" record "${repo}" "${moved}" 2>/dev/null; then fail "recorded a tree the working copy no longer holds"; fi
if "${helper}" passed "${repo}" "${moved}"; then fail "a tree refused at record time passed anyway"; fi
git -C "${repo}" checkout -q -- a.txt

## Not a tree hash, so it never reaches the pattern: '.*' would match any line.
prc=0; "${helper}" passed "${repo}" '.*' 2>/dev/null || prc=$?
((prc == 2)) || fail "passed took '.*' as a tree (exit ${prc})"

export STUB_LOG="${work}/stub.log"
fPush(){  ## fPush BRANCH SHA -> hookRc, hookOut, ran (times the stubbed gate ran)
	: > "${STUB_LOG}"
	hookRc=0
	hookOut="$(printf 'refs/heads/x %s refs/heads/%s %s\n' "$2" "$1" "${zeros}" | bash "${repo}/cicd/hooks/pre-push" 2>&1)" || hookRc=$?
	ran="$(wc -l < "${STUB_LOG}")"
}

printf 'six\n' > "${repo}/d.txt"
git -C "${repo}" add d.txt && git -C "${repo}" commit -q -m six
sha="$(git -C "${repo}" rev-parse HEAD)"

fPush main "${sha}"
((hookRc == 0 && ran == 1)) || fail "a commit nothing recorded, pushed to main: exit ${hookRc}, gate ran ${ran} time(s)"
## 20260918 item 6: the installer drift check judges the tree as main only
## when the gate is told that is what it stands in for.
grep -qx 'ran main' "${STUB_LOG}" || fail "the hook did not tell the gate it stands in for main: $(cat "${STUB_LOG}")"
export STUB_RC=1; fPush main "${sha}"; unset STUB_RC
((hookRc == 1)) || fail "a red gate did not refuse the push (exit ${hookRc})"
fPush feature "${sha}"
((hookRc == 0 && ran == 0)) || fail "a feature branch push: exit ${hookRc}, gate ran ${ran} time(s)"
fPush dev "${sha}"
((hookRc == 0 && ran == 0)) || fail "a commit nothing recorded, pushed to dev: exit ${hookRc}, gate ran ${ran} time(s)"

"${helper}" record "${repo}" "$(git -C "${repo}" rev-parse 'HEAD^{tree}')" || fail "record refused a clean checkout"
fPush main "${sha}"
((hookRc == 0 && ran == 0)) || fail "a recorded tree, pushed to main: exit ${hookRc}, gate ran ${ran} time(s)"
[[ "${hookOut}" == *"not running it again"* ]] || fail "the hook let a recorded tree through without saying so: ${hookOut}"
export SHCL_GATE_RERUN=1; fPush main "${sha}"; unset SHCL_GATE_RERUN
((hookRc == 0 && ran == 1)) || fail "SHCL_GATE_RERUN=1 on a recorded tree: exit ${hookRc}, gate ran ${ran} time(s)"

## A checkout from before the helper existed has none, which reads as not passed.
mv "${helper}" "${helper}.off"
fPush main "${sha}"
mv "${helper}.off" "${helper}"
((hookRc == 0 && ran == 1)) || fail "with the helper missing: exit ${hookRc}, gate ran ${ran} time(s)"

## A --no-ff merge of a branch that passed, onto a branch that has not moved
## since, has the branch's tree.
git -C "${repo}" checkout -q -b topic
printf 'seven\n' > "${repo}/d.txt"
git -C "${repo}" commit -q -am seven
"${helper}" record "${repo}" "$(git -C "${repo}" rev-parse 'HEAD^{tree}')" || fail "record refused the topic branch"
git -C "${repo}" checkout -q dev
git -C "${repo}" merge -q --no-ff -m "Merge topic" topic
fPush main "$(git -C "${repo}" rev-parse HEAD)"
((hookRc == 0 && ran == 0)) || fail "a merge of a branch that passed: exit ${hookRc}, gate ran ${ran} time(s)"

## One more commit: a tree nobody ran, so main gates it and dev does not.
printf 'eight\n' > "${repo}/d.txt"
git -C "${repo}" commit -q -am eight
fPush main "$(git -C "${repo}" rev-parse HEAD)"
((hookRc == 0 && ran == 1)) || fail "a new tree after a recorded merge: exit ${hookRc}, gate ran ${ran} time(s)"
fPush dev "$(git -C "${repo}" rev-parse HEAD)"
((hookRc == 0 && ran == 0)) || fail "a new tree pushed to dev: exit ${hookRc}, gate ran ${ran} time(s)"

## A merge nobody ran, the everyday case: dev takes it, main gates it.
git -C "${repo}" checkout -q -b topic2
printf 'nine\n' > "${repo}/d.txt"
git -C "${repo}" commit -q -am nine
git -C "${repo}" checkout -q dev
git -C "${repo}" merge -q --no-ff -m "Merge topic2" topic2
merged="$(git -C "${repo}" rev-parse HEAD)"
fPush dev "${merged}"
((hookRc == 0 && ran == 0)) || fail "a merge pushed to dev: exit ${hookRc}, gate ran ${ran} time(s)"
fPush main "${merged}"
((hookRc == 0 && ran == 1)) || fail "a merge pushed to main: exit ${hookRc}, gate ran ${ran} time(s)"

## 20260918 item 6: the installer drift check, on a clone of this repository
## with the refs as they stand between a dev push that changes an installer and
## the main push that follows it. The hook runs before git moves origin/main,
## so judged by the refs, the main push that ends the drift was refused.
clone="${work}/clone"
if git clone -q --shared --no-checkout "${root}" "${clone}" 2>/dev/null && git -C "${clone}" checkout -q --detach "$(git -C "${root}" rev-parse HEAD)" 2>/dev/null; then
	base="$(git -C "${clone}" rev-parse HEAD)"
	printf '# drift\n' >> "${clone}/install.bash"
	git -C "${clone}" commit -q -am drift
	moved="$(git -C "${clone}" rev-parse HEAD)"
	git -C "${clone}" update-ref refs/remotes/origin/main "${base}"
	git -C "${clone}" update-ref refs/remotes/origin/dev "${moved}"
	## The check under test is this checkout's, not the commit's. The clone has
	## no build, so its help checks skip; that skip is the clone's, and neither
	## fails nor gets noted in the run's own skip list.
	cp "${root}/cicd/utility/check-docs.bash" "${clone}/cicd/utility/check-docs.bash"
	fDocs(){ docsRc=0; docsOut="$(env -u SHCL_GATE_STRICT "$@" SHCL_GATE_SKIPS=/dev/null bash "${clone}/cicd/utility/check-docs.bash" 2>&1)" || docsRc=$? ;}
	fDocs -u SHCL_GATE_REF
	((docsRc == 1)) && [[ "${docsOut}" == *"installer on dev and not on main"*install.bash* ]] \
		|| fail "a run outside the hook did not see dev ahead of main: exit ${docsRc}"
	fDocs SHCL_GATE_REF=main
	((docsRc == 0)) || fail "the main push that brings the installers level was refused: exit ${docsRc}: $(tail -c 300 <<<"${docsOut}")"
	git -C "${clone}" checkout -q --detach "${base}"
	cp "${root}/cicd/utility/check-docs.bash" "${clone}/cicd/utility/check-docs.bash"
	fDocs SHCL_GATE_REF=main
	((docsRc == 1)) && [[ "${docsOut}" == *"between this push to main and dev"*install.bash* ]] \
		|| fail "a main push that leaves the installers behind dev went through: exit ${docsRc}: $(tail -c 300 <<<"${docsOut}")"
else
	fail "cannot clone ${root} for the installer drift check"
fi

(( rc == 0 )) && echo "check-push-gate: OK: the tree matches a commit of the working copy, a recorded tree skips the gate, only a push to main runs it, a red gate refuses, and the drift check judges the pushed tree as main"
exit "${rc}"


##	History:
##		- 2026-09-14 JC: Created.
##		- 2026-09-16 JC: Only a push to main is gated.
##		- 2026-09-18 JC: The gate is told it stands in for main, and the
##		  installer drift check is run on a clone with the refs set as they
##		  are between the dev push and the main push.
