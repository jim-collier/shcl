#!/usr/bin/env bash

#  shellcheck disable=2034  ## 'variable appears unused.' config.bash's arrays, of which only TOOL_PINS is read here.

##	Purpose:
##		Fail when a pin in config.bash TOOL_PINS is not the one ci.yml installs.
##		The two lists are kept by hand; the last time they drifted, hosted CI
##		stayed red for days while the local gate was green. For every pinned
##		tool the hosted gate installs, some line of ci.yml has to name the tool
##		and its pinned version together (ruff==X, staticcheck@X, a download URL
##		carrying X). cppcheck is the odd one: its PyPI package version is not the
##		binary version TOOL_PINS checks, so the match lands on the comment beside
##		the package pin that records which binary the package carries - bump both.
##	Syntax:
##		check-pins.bash
##	Exit: 0 = every pin matches, 1 = a mismatch (named), 2 = cannot read inputs.
##	History: At bottom of script.

##	Copyright © 2026 Bubbles (ID: XଌฅრX۳ᛟԃლፀƅꓩหδლც)
##	Licensed under The MIT License (MIT). Full text at:
##		https://mit-license.org/
##	SPDX-License-Identifier: MIT


set -Eeuo pipefail

cicdDir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
ciFile="${cicdDir}/../.github/workflows/ci.yml"
[[ -r "${ciFile}" ]] || { echo "check-pins: cannot read ${ciFile}" >&2; exit 2; }

## config.bash expands the cap while it loads; any value will do here.
CPU_CAP="${CPU_CAP:-1}"
# shellcheck source-path=SCRIPTDIR
# shellcheck source=../config.bash
source "${cicdDir}/config.bash"
declare -p TOOL_PINS &>/dev/null || { echo "check-pins: config.bash has no TOOL_PINS" >&2; exit 2; }

## Not installed by the hosted gate, which builds no cross targets.
notInCi=(cargo-zigbuild)

rc=0
for pin in "${TOOL_PINS[@]}"; do
	name="${pin%%|*}"; rest="${pin#*|}"; ver="${rest%%|*}"
	skip=0; for x in "${notInCi[@]}"; do [[ "${x}" == "${name}" ]] && skip=1; done
	((skip)) && continue
	## Lines naming the tool, then the version among them. Two steps rather than
	## one pipeline so nothing sits downstream of an early-exiting reader.
	namedLines="$(grep -iF -- "${name}" "${ciFile}" || true)"
	if grep -qF -- "${ver}" <<<"${namedLines}"; then
		continue
	fi
	echo "check-pins: ${name} is pinned at ${ver} in config.bash, and no line of ci.yml names ${name} with that version" >&2
	rc=1
done

((rc)) || echo "check-pins: OK: every TOOL_PINS entry the hosted gate installs matches ci.yml"
exit "${rc}"


##	History:
##		- 2026-08-29 JC: Created.
