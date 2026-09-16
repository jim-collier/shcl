#!/usr/bin/env bash

##	Purpose:
##		Check the pre-push hook's skip for a tree a gate already passed, on a
##		throwaway repository with the gate stubbed out. The tree green-tree.bash
##		computes has to be the one a commit of the working copy gets, a recorded
##		tree has to let the hook through without the gate, and anything else
##		still has to run it. A skip that fires when it should not is a push that
##		reaches dev untested, and nothing downstream would say so.
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
echo ran >> "${STUB_LOG}"
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

fPush dev "${sha}"
((hookRc == 0 && ran == 1)) || fail "a commit nothing recorded, pushed to dev: exit ${hookRc}, gate ran ${ran} time(s)"
export STUB_RC=1; fPush dev "${sha}"; unset STUB_RC
((hookRc == 1)) || fail "a red gate did not refuse the push (exit ${hookRc})"
fPush feature "${sha}"
((hookRc == 0 && ran == 0)) || fail "a feature branch push: exit ${hookRc}, gate ran ${ran} time(s)"

"${helper}" record "${repo}" "$(git -C "${repo}" rev-parse 'HEAD^{tree}')" || fail "record refused a clean checkout"
fPush dev "${sha}"
((hookRc == 0 && ran == 0)) || fail "a recorded tree, pushed to dev: exit ${hookRc}, gate ran ${ran} time(s)"
[[ "${hookOut}" == *"not running it again"* ]] || fail "the hook let a recorded tree through without saying so: ${hookOut}"
fPush main "${sha}"
((hookRc == 0 && ran == 0)) || fail "a recorded tree, pushed to main: exit ${hookRc}, gate ran ${ran} time(s)"
export SHCL_GATE_RERUN=1; fPush dev "${sha}"; unset SHCL_GATE_RERUN
((hookRc == 0 && ran == 1)) || fail "SHCL_GATE_RERUN=1 on a recorded tree: exit ${hookRc}, gate ran ${ran} time(s)"

## A checkout from before the helper existed has none, which reads as not passed.
mv "${helper}" "${helper}.off"
fPush dev "${sha}"
mv "${helper}.off" "${helper}"
((hookRc == 0 && ran == 1)) || fail "with the helper missing: exit ${hookRc}, gate ran ${ran} time(s)"

## A --no-ff merge of a branch that passed, onto a dev that has not moved since,
## has the branch's tree.
git -C "${repo}" checkout -q -b topic
printf 'seven\n' > "${repo}/d.txt"
git -C "${repo}" commit -q -am seven
"${helper}" record "${repo}" "$(git -C "${repo}" rev-parse 'HEAD^{tree}')" || fail "record refused the topic branch"
git -C "${repo}" checkout -q dev
git -C "${repo}" merge -q --no-ff -m "Merge topic" topic
fPush dev "$(git -C "${repo}" rev-parse HEAD)"
((hookRc == 0 && ran == 0)) || fail "a merge of a branch that passed: exit ${hookRc}, gate ran ${ran} time(s)"

## One more commit on dev: a tree nobody ran, so the gate runs.
printf 'eight\n' > "${repo}/d.txt"
git -C "${repo}" commit -q -am eight
fPush dev "$(git -C "${repo}" rev-parse HEAD)"
((hookRc == 0 && ran == 1)) || fail "a new tree after a recorded merge: exit ${hookRc}, gate ran ${ran} time(s)"

## A merge nobody ran: dev takes it on sight, main still gates it.
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

(( rc == 0 )) && echo "check-push-gate: OK: the tree matches a commit of the working copy, a recorded tree and a merge to dev skip the gate, and every other push still runs it"
exit "${rc}"


##	History:
##		- 2026-09-14 JC: Created.
##		- 2026-09-16 JC: A merge pushed to dev skips the gate.
