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

##	Copyright © 2026 Bubbles (ID: XଌฅრX۳ᛟԃლፀƅꓩหδლც)
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
git -C "${work}/clone" config --unset core.hooksPath
( cd "${work}/clone" && bash "${script}" --hooks-only >/dev/null )
[[ "$(git -C "${work}/clone" config core.hooksPath)" == "cicd/hooks" ]] || fail "in-clone run did not set hooksPath"

## Not a clone: refused, nothing created.
mkdir "${work}/empty"
if ( cd "${work}" && bash "${script}" --hooks-only --dir empty >/dev/null 2>&1 ); then
	fail "--hooks-only accepted a directory that is not a clone"
fi

(( rc == 0 )) && echo "check-install-dev: OK: --hooks-only sets the hooks path and keepalive, idempotently, and refuses a non-clone"
exit "${rc}"


##	History:
##		- 2026-09-01 JC: Created.
