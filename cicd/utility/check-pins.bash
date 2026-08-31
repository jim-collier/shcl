#!/usr/bin/env bash

#  shellcheck disable=2034  ## 'variable appears unused.' config.bash's arrays, of which only TOOL_PINS and CPPCHECK_WHEEL are read here.

##	Purpose:
##		Fail when a pin in config.bash TOOL_PINS is not the one ci.yml installs.
##		The two lists are kept by hand; the last time they drifted, hosted CI
##		stayed red for days while the local gate was green. For every pinned
##		tool the hosted gate installs, some line of ci.yml has to name the tool
##		and its pinned version together as one token (ruff==X, staticcheck@X,
##		a download URL carrying X, go-version: "X") - whole words, so `build`
##		is not satisfied by a line that merely contains those letters, nor
##		`1.5.0` by govulncheck's v1.5.0. cppcheck is the odd one: pip installs
##		its PyPI wheel, whose version (CPPCHECK_WHEEL) is not the binary version
##		TOOL_PINS checks, so ci.yml has to carry the wheel token and, on a line
##		naming cppcheck, the binary version that wheel bundles - bump both.
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
[[ -n "${CPPCHECK_WHEEL:-}" ]] || { echo "check-pins: config.bash has no CPPCHECK_WHEEL" >&2; exit 2; }

## Not installed by the hosted gate, which builds no cross targets.
notInCi=(cargo-zigbuild)

## Does ci.yml name tool $1 at version $2 as one token? The name is bounded on
## the left, then the joiner ci.yml uses (==, @, -, /, or setup-go's
## `-version: "`), an optional v, the version, and nothing digit-like after.
fNamesAt(){ grep -qE -- "(^|[^A-Za-z0-9_-])${1}(==|@|-|/|-version: \")v?${2//./\\.}([^0-9]|$)" "${ciFile}"; }

rc=0
for pin in "${TOOL_PINS[@]}"; do
	name="${pin%%|*}"; rest="${pin#*|}"; ver="${rest%%|*}"
	skip=0
	for x in "${notInCi[@]}"; do
		if [[ "${x}" == "${name}" ]]; then skip=1; fi
	done
	((skip)) && continue
	if [[ "${name}" == cppcheck ]]; then
		fNamesAt cppcheck "${CPPCHECK_WHEEL}" || { echo "check-pins: cppcheck's wheel is ${CPPCHECK_WHEEL} in config.bash, and no line of ci.yml installs cppcheck==${CPPCHECK_WHEEL}" >&2; rc=1; }
		## The binary version rides in the comment beside that install line. Two
		## steps rather than one pipeline so nothing sits downstream of an
		## early-exiting reader.
		namedLines="$(grep -iE -- '(^|[^A-Za-z0-9_-])cppcheck' "${ciFile}" || true)"
		grep -qE -- "(^|[^0-9.])${ver//./\\.}([^0-9]|$)" <<<"${namedLines}" && continue
	elif fNamesAt "${name}" "${ver}"; then
		continue
	fi
	echo "check-pins: ${name} is pinned at ${ver} in config.bash, and no line of ci.yml names ${name} with that version" >&2
	rc=1
done

((rc)) || echo "check-pins: OK: every TOOL_PINS entry the hosted gate installs matches ci.yml"
exit "${rc}"


##	History:
##		- 2026-08-29 JC: Created.
##		- 2026-08-30 JC: Whole-token matching; the cppcheck wheel is checked
##		  against CPPCHECK_WHEEL as well as the binary version.
