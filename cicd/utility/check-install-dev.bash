#!/usr/bin/env bash

##	Purpose:
##		Run install-dev.bash --hooks-only against a throwaway clone and check
##		what it leaves behind. The hook setup was the one piece of that script
##		nothing exercised: the toolchain installs in front of it cannot run in
##		a gate, so a regression there would only be found by the next person
##		setting up a box. --hooks-only skips the installs, which makes the tail
##		runnable here - against the shipped script, not a copy of its logic.
##	Syntax:
##		check-install-dev.bash
##	Exit: 0 = all checks pass, 1 = a check failed (named), 2 = cannot set up.
##	History: At bottom of script.

##	Copyright (c) 2026 Bubbles
##	Licensed under The MIT License (MIT). Full text at:
##		https://mit-license.org/
##	SPDX-License-Identifier: MIT


set -Eeuo pipefail

root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
script="${root}/install-dev.bash"
[[ -r "${script}" ]] || { echo "check-install-dev: cannot read ${script}" >&2; exit 2; }

work="$(mktemp -d)"
trap 'rm -rf "${work}"' EXIT
## --no-hardlinks: /tmp is routinely a different filesystem from the repo.
git clone -q --no-hardlinks --local "${root}" "${work}/clone" || { echo "check-install-dev: local clone failed" >&2; exit 2; }

rc=0
fail() { echo "check-install-dev: FAIL: $*" >&2; rc=1; }

## From a neutral cwd, pointed at the clone by --dir: sets both configs.
( cd "${work}" && bash "${script}" --hooks-only --dir clone >/dev/null )
[[ "$(git -C "${work}/clone" config core.hooksPath)" == "cicd/hooks" ]] || fail "hooksPath not set"
[[ "$(git -C "${work}/clone" config core.sshCommand)" == *ServerAliveInterval* ]] || fail "ssh keepalive not set"

## Idempotent: a second run changes nothing and still exits 0.
before="$(git -C "${work}/clone" config --list --local)"
( cd "${work}" && bash "${script}" --hooks-only --dir clone >/dev/null ) || fail "second run failed"
[[ "$(git -C "${work}/clone" config --list --local)" == "${before}" ]] || fail "second run changed the config"

## A chosen sshCommand survives: the keepalive is only for the unconfigured.
git -C "${work}/clone" config core.sshCommand "ssh -i /keep/this"
( cd "${work}/clone" && bash "${script}" --hooks-only >/dev/null )
[[ "$(git -C "${work}/clone" config core.sshCommand)" == "ssh -i /keep/this" ]] || fail "a configured sshCommand was overwritten"

## Run inside the clone with no --dir: the in-clone detection finds it.
## `--unset` exits 5 when the key is not there, which under errexit would end
## the run here and take every check below it with it.
git -C "${work}/clone" config --unset core.hooksPath || true
( cd "${work}/clone" && bash "${script}" --hooks-only >/dev/null )
[[ "$(git -C "${work}/clone" config core.hooksPath)" == "cicd/hooks" ]] || fail "in-clone run did not set hooksPath"

## Not a clone: refused, and nothing written. The fixture is a repository that
## is not an shcl clone, since that is what the guard is for - a bare directory
## fails inside git config whether the guard is there or not, and the config
## is read back because the exit status alone proved nothing either way.
git init -q "${work}/other"
if ( cd "${work}" && bash "${script}" --hooks-only --dir other >/dev/null 2>&1 ); then
	fail "--hooks-only accepted a repository that is not an shcl clone"
fi
[[ -z "$(git -C "${work}/other" config --local core.hooksPath || true)" ]] || fail "a refused run set hooksPath on a foreign repository"
[[ -z "$(git -C "${work}/other" config --local core.sshCommand || true)" ]] || fail "a refused run set sshCommand on a foreign repository"

## An shcl-shaped tree that is not a repository: refused too.
mkdir -p "${work}/tree/cicd"
cp "${root}/cicd/cicd.bash" "${work}/tree/cicd/"
if ( cd "${work}" && bash "${script}" --hooks-only --dir tree >/dev/null 2>&1 ); then
	fail "--hooks-only accepted a tree that is not a repository"
fi

## The default path, not --hooks-only, pointed at a repository that is not
## shcl: refused before anything is fetched, with its branch and config as
## they were. The refusal comes ahead of the network, so this runs offline.
git -C "${work}/other" -c user.name=t -c user.email=t@t commit -q --allow-empty -m init
git -C "${work}/other" branch -q dev
other_branch="$(git -C "${work}/other" branch --show-current)"
## The message is read too: without the guard the run can still fail later,
## on the network, for a reason that has nothing to do with this.
if ( cd "${work}" && bash "${script}" --yes --dir other >"${work}/out" 2>&1 </dev/null ); then
	fail "the default path accepted a repository that is not an shcl clone"
fi
grep -qF "is not an shcl clone" "${work}/out" || fail "the default path did not refuse a foreign repository up front"
[[ "$(git -C "${work}/other" branch --show-current)" == "${other_branch}" ]] || fail "a refused run switched a foreign repository's branch"
[[ -z "$(git -C "${work}/other" config --local core.hooksPath || true)" ]] || fail "the default path set hooksPath on a foreign repository"

## A non-empty directory that is no repository at all is refused the same way.
mkdir -p "${work}/stuff" && : >"${work}/stuff/keep"
if ( cd "${work}" && bash "${script}" --yes --dir stuff >"${work}/out" 2>&1 </dev/null ); then
	fail "the default path accepted a non-empty directory that is not a clone"
fi
grep -qF "is not an shcl clone" "${work}/out" || fail "the default path did not refuse a non-empty directory up front"

(( rc == 0 )) && echo "check-install-dev: OK: --hooks-only sets the hooks path and keepalive, idempotently, and refuses a non-clone; the default path refuses one too"
exit "${rc}"


##	History:
##		- 2026-09-01 JC: Created.
