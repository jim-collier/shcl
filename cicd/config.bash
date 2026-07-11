#!/usr/bin/env bash

#  shellcheck disable=2034  ## 'variable appears unused.' Everything here is consumed by cicd.bash.

##	Purpose:
##		- Project-specific CI/CD settings for shcl.
##		- The engine (cicd.bash) stays generic; everything project-specific lives here.
##		- Most stages are empty until the reference parser lands; an empty array means
##		  that stage reports "nothing configured" and the run continues. Fill each in
##		  as the reader(s) come online; later readers add commands here, not engine code.
##	History: At bottom of script.

##	Copyright © 2026 Jim Collier (ID: 1cv◂‡Vᛦ)
##	Licensed under The MIT License (MIT). Full text at:
##		https://mit-license.org/
##	SPDX-License-Identifier: MIT


## Only allow running 'sourced'.
declare -i isSourced_t6wqf=0; [[ "${BASH_SOURCE[0]}" == "${0}" ]] || isSourced_t6wqf=1
((isSourced_t6wqf)) || { echo -e "\nError in $(basename "${BASH_SOURCE[0]}"): This script is meant to be 'sourced' from within another script.\n"; exit 1; }


## Identity
APP_NAME="shcl"

## Stage 1: format. FMT_CMD rewrites in place (local runs); FMT_CHECK_CMD fails on
## any diff without touching files (what --ci runs). Fill both in together once the
## reference-parser language is picked (e.g. gofmt -l / cargo fmt --check).
FMT_CMD=()
FMT_CHECK_CMD=()

## Stage 2: build. The reference parser first; later a fan-out over all readers.
BUILD_CMD=()

## Stage 3: lint. Project lint (language toolchain), plus shellcheck over the
## pipeline's own scripts so cicd can't rot silently.
LINT_CMD=()
SHELLCHECK_TARGETS=(cicd/cicd.bash cicd/config.bash)

## Stage 4: tests. This will be the conformance-corpus runner (project/conformance/)
## driving every built reader - the contract between implementations.
TEST_CMD=()

## Stage 5: cross-compile (local runs only; --ci never runs this).
CROSS_CMD=()

## Stage 6: publish. Simple add/commit/push from the engine. PUBLISH_AUTO_MESSAGE,
## if set, is used when no -m is given and the run is unattended.
DO_PUBLISH=1
PUBLISH_AUTO_MESSAGE=""


##	History:
##		- 2026-07-11 JC: Created; all build/test stages stubbed pending the reference parser, shellcheck live from day one.
