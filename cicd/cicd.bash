#!/usr/bin/env bash

#  shellcheck disable=1091  ## 'source is valid here, but shellcheck doesn't know the path to it.'
#  shellcheck disable=2016  ## 'Expressions don't expand in single quotes, use double quotes for that.' I know, and I often want an explicit '$'.
#  shellcheck disable=2034  ## 'variable appears unused.' Complains about valid use of variable indirection.
#  shellcheck disable=2086  ## 'Double quote to prevent globbing and word splitting.' (OK for integers.)
#  shellcheck disable=2154  ## 'referenced but not assigned.' False hit on trap strings that assign the var they use (rc=$?).
#  shellcheck disable=2155  ## 'Declare and assign separately to avoid masking return values.' Cumbersome and unnecessary.
#  shellcheck disable=2317  ## 'Can't reach.' (I.e. an 'exit' is used for debugging - and makes an unusable visual mess.)

##	- Purpose: Local CI/CD pipeline. Generic engine, per-project settings live in config.bash.
##	- Stages (fail-fast, any error aborts before the next stage):
##	   1. format (in place locally; check-only under --ci)
##	   2. build
##	   3. lint (project lint + shellcheck of the cicd scripts themselves)
##	   4. tests (conformance corpus + whatever else config wires in)
##	   5. cross-compile (local only; skipped under --ci)
##	   6. backup + publish to git (local only; skipped under --ci)
##	- Syntax:
##	  cicd/cicd.bash [options]
##	  Options:
##	   --ci                correctness gate only: format check (no rewrite), build,
##	                       lint, tests; non-interactive, no cross/publish. This is
##	                       what the GitHub workflow runs - one definition of "passing".
##	   -q, --quiet         quiet + unattended (no prompt)
##	   -y, --yes           unattended (no prompt) but not quiet
##	   -m, --message MSG   publish hands-off with this commit message (no editor)
##	   --no-fmt            skip the formatter stage
##	   --no-lint           skip the lint stage
##	   --no-cross          skip the cross-compile stage
##	   --no-publish        skip the git backup + publish stage
##	   -h, --help          show this help
##	- If neither -q/-y nor -m is given, the run prompts once for a commit message
##	  (blank = git editor; Ctrl+C aborts the whole run), then finishes unattended.
##	- Reuse: copy the cicd/ directory into another project and edit config.bash.

##	History: At bottom of script.

##	Copyright © 2026 Jim Collier (ID: 1cv◂‡Vᛦ)
##	Licensed under The MIT License (MIT). Full text at:
##		https://mit-license.org/
##	SPDX-License-Identifier: MIT


set -Eeuo pipefail

## Find the repo root and load project config.
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "${here}/.." && pwd)"   ## the git repo root (cicd/..)

## Cap every stage at 50% of cores; config command arrays can consume CPU_CAP.
_cores="$(nproc 2>/dev/null || echo 2)"
CPU_CAP=$(( _cores / 2 )); (( CPU_CAP < 1 )) && CPU_CAP=1
export CPU_CAP

## shellcheck source=config.bash
source "${here}/config.bash"
cd "${root}"
stamp="$(date +%Y%m%d-%H%M%S)"

## Parse options.
assume_yes=0; quiet=0; ci_mode=0; cli_message=""
while (($#)); do case "$1" in
	--ci)                     ci_mode=1; assume_yes=1; shift ;;
	-q|--quiet)               quiet=1; assume_yes=1; shift ;;
	-y|--yes)                 assume_yes=1; shift ;;
	--no-fmt)                 FMT_CMD=(); FMT_CHECK_CMD=(); shift ;;
	--no-lint)                LINT_CMD=(); SHELLCHECK_TARGETS=(); shift ;;
	--no-cross)               CROSS_CMD=(); shift ;;
	--no-publish)             DO_PUBLISH=0; shift ;;
	--message=*|--msg=*|-m=*) cli_message="${1#*=}"; shift ;;
	-m|--message|--msg)       cli_message="${2-}"; shift; (($#)) && shift ;;
	-h|--help)                sed -n '/^##	- Purpose:/,/^##	History:/p' "${BASH_SOURCE[0]}" | sed '$d; s/^##	\{0,1\}//'; exit 0 ;;
	*) echo "unknown option: $1 (try --help)" >&2; exit 2 ;;
esac; done

## --ci: correctness only, deterministic, side-effect free.
if ((ci_mode)); then
	FMT_CMD=()          ## check-only via FMT_CHECK_CMD; never rewrite in CI
	CROSS_CMD=()
	DO_PUBLISH=0
else
	FMT_CHECK_CMD=()    ## locally the formatter rewrites in place instead
fi

## Publish commit message: -m wins, then config, then a default when unattended.
publish_msg=""
if   [[ -n "$cli_message" ]];              then publish_msg="$cli_message"
elif [[ -n "${PUBLISH_AUTO_MESSAGE:-}" ]]; then publish_msg="$PUBLISH_AUTO_MESSAGE"
elif ((assume_yes));                       then publish_msg="${APP_NAME} CI/CD ${stamp}"
fi

## Output helpers: fEcho / fEcho_Clean, blank-collapsing.
declare -i _wasLastEchoBlank=0
fEcho_ResetBlankCounter(){ _wasLastEchoBlank=0; }
fEcho_Clean(){ if [[ -n "${1:-}" ]]; then echo -e "$*"; _wasLastEchoBlank=0; elif [[ $_wasLastEchoBlank -eq 0 ]] && echo; then _wasLastEchoBlank=1; fi; }
fEcho(){       if [[ -n "$*"     ]]; then fEcho_Clean "[ $* ]"; else fEcho_Clean ""; fi; }
fEcho_Force(){ fEcho_ResetBlankCounter; fEcho "$*"; }
_letterbox="••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••"
fSection(){ fEcho_Clean; fEcho_Clean "${_letterbox}"; fEcho "$*"; }
fDie(){ { fEcho_Force "FAILED: $*"; } >&2; exit 1; }
trap 'rc=$?; printf "\n[ CICD ABORTED (exit %s) at line %s: %s ]\n" "$rc" "$LINENO" "$BASH_COMMAND" >&2; exit $rc' ERR

## Preflight: show the plan, then capture the commit message so the rest runs unattended.
if ((! quiet)); then
	fEcho_Clean
	fEcho_Clean "${APP_NAME} $( ((ci_mode)) && echo 'CI gate' || echo 'local CI/CD')"
	fEcho_Clean
	fEcho_Clean "Repo root ......: ${root}"
	fEcho_Clean "Format .........: $( ((ci_mode)) && echo "${FMT_CHECK_CMD[*]:-(none configured)}" || echo "${FMT_CMD[*]:-(skipped)}")"
	fEcho_Clean "Build ..........: ${BUILD_CMD[*]:-(none configured yet)}"
	fEcho_Clean "Lint ...........: ${LINT_CMD[*]:-(none configured yet)}  + shellcheck: ${SHELLCHECK_TARGETS[*]:-(none)}"
	fEcho_Clean "Tests ..........: ${TEST_CMD[*]:-(none configured yet)}"
	fEcho_Clean "Cross-compile ..: ${CROSS_CMD[*]:-(skipped)}"
	if ((DO_PUBLISH)); then
		fEcho_Clean "Publish (last) .: git add/commit/push$( [[ -n "$publish_msg" ]] && echo " (hands-off: \"${publish_msg}\")" || echo ' (will prompt for message; blank = editor)')"
	else
		fEcho_Clean "Publish (last) .: (skipped)"
	fi
	fEcho_Clean
	fEcho_Clean "Fail-fast: any error aborts before the next stage."
	fEcho_Clean
fi
if ((! assume_yes)) && ((DO_PUBLISH)) && [[ -z "$publish_msg" ]]; then
	read -r -p "Publish commit message (blank = editor; Ctrl+C aborts): " m
	fEcho_ResetBlankCounter
	[[ -n "$m" ]] && publish_msg="$m"
fi

## Stage 1: format. In place locally; check-only (fail on diff) under --ci.
fSection "1/6  Format"
if ((ci_mode)); then
	if ((${#FMT_CHECK_CMD[@]})); then "${FMT_CHECK_CMD[@]}"; fEcho "OK: format clean"
	else fEcho_Clean "no format check configured"; fi
elif ((${#FMT_CMD[@]})); then
	"${FMT_CMD[@]}"; fEcho "OK: formatted"
else
	fEcho_Clean "format skipped"
fi

## Stage 2: build.
fSection "2/6  Build"
if ((${#BUILD_CMD[@]})); then
	"${BUILD_CMD[@]}"; fEcho "OK: build"
else
	fEcho_Clean "nothing to build yet"
fi

## Stage 3: lint. Project lint first, then shellcheck over the cicd scripts (and
## any other bash config lists), so the pipeline can't rot silently. shellcheck
## is optional locally (warn if missing) but present on GitHub runners.
fSection "3/6  Lint"
if ((${#LINT_CMD[@]})); then
	"${LINT_CMD[@]}"; fEcho "OK: lint clean"
else
	fEcho_Clean "no project lint configured yet"
fi
if ((${#SHELLCHECK_TARGETS[@]})); then
	if command -v shellcheck >/dev/null 2>&1; then
		shellcheck "${SHELLCHECK_TARGETS[@]}"
		fEcho "OK: shellcheck clean (${#SHELLCHECK_TARGETS[@]} file(s))"
	elif ((ci_mode)); then
		fDie "shellcheck required under --ci but not found"
	else
		fEcho "WARNING: shellcheck not installed; skipped"
	fi
fi

## Stage 4: tests (conformance corpus + anything else config wires in).
fSection "4/6  Tests"
if ((${#TEST_CMD[@]})); then
	"${TEST_CMD[@]}"; fEcho "OK: tests passed"
else
	fEcho_Clean "no tests configured yet"
fi

## Stage 5: cross-compile (local only).
fSection "5/6  Cross-compile"
if ((${#CROSS_CMD[@]})); then
	"${CROSS_CMD[@]}"; fEcho "OK: cross-compile"
else
	fEcho_Clean "cross-compile skipped"
fi

## Stage 6: backup + publish.
fSection "6/6  Backup + publish"
if ((DO_PUBLISH)); then
	git add -A
	if git diff --cached --quiet; then
		fEcho_Clean "nothing to commit"
	elif [[ -n "$publish_msg" ]]; then
		git commit -q -m "$publish_msg"
	else
		git commit   ## opens the editor
	fi
	git push origin HEAD
	fEcho "OK: published"
else
	fEcho_Clean "publish skipped"
fi

fSection "${APP_NAME} CI/CD: done."
fEcho_Clean


##	History:
##		- 2026-07-11 JC: Created from the sister-project engine, trimmed to what shcl needs pre-code; added --ci gate mode shared with the GitHub workflow.
