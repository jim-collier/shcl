#!/usr/bin/env bash

##	Purpose:
##		Build the C surface with every C compiler on this box, not just the
##		default one. The hosted runner's gcc is not this box's gcc, and the two
##		disagree about what -Wall -Wextra -Werror rejects: a round once went out
##		green here and red there, because gcc 13 saw a maybe-uninitialized the
##		local gcc 14 and 15 both missed. Cheap enough to run every time.
##	Syntax:
##		check-c-compilers.bash [ROOT]
##	Exit: 0 = every compiler present is happy, 1 = one refused, 2 = usage.
##	History: At bottom of script.

##	Copyright © 2026 Bubbles (ID: XଌฅრX۳ᛟԃლፀƅꓩหδლც)
##	Licensed under The MIT License (MIT). Full text at:
##		https://mit-license.org/
##	SPDX-License-Identifier: MIT


set -Eeuo pipefail

repoDir="${1:-$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)}"
[[ -d "${repoDir}" ]] || { echo "check-c-compilers: no such directory: ${repoDir}" >&2; exit 2 ;}

tmpDir="$(mktemp -d)"; trap 'rm -rf "${tmpDir}"' EXIT
declare -i nBad=0 nRun=0

## Whatever is installed: the named versions plus the two default front ends.
compilers=()
for c in gcc-12 gcc-13 gcc-14 gcc-15 gcc-16 gcc clang; do
	command -v "${c}" >/dev/null 2>&1 && compilers+=("${c}")
done
((${#compilers[@]})) || { echo "check-c-compilers: no C compiler found" >&2; exit 2 ;}

## Same flags the build and test stages use, so a disagreement here is a
## disagreement there.
for cc in "${compilers[@]}"; do
	## The two OOM tests are here for -Wclobbered: the library's recovery point
	## is a setjmp, and which locals a compiler thinks the unwind can clobber
	## differs by version and optimization level.
	for src in source/c/cmd/shcl/main.c source/c/tests/conformance.c \
	           source/c/tests/oom_hook.c source/c/tests/oom_recover.c \
	           source/c/tests/mem_bounds.c; do
		nRun+=1
		if ! out="$("${cc}" -std=c11 -O2 -Wall -Wextra -Werror -I"${repoDir}/source/c" \
			"${repoDir}/${src}" -o "${tmpDir}/out" -lm -lpthread 2>&1)"; then
			echo "check-c-compilers: ${cc} refuses ${src}:" >&2
			echo "${out}" | head -n 8 >&2
			nBad+=1
		fi
	done
done

if ((nBad)); then
	echo "check-c-compilers: ${nBad} of ${nRun} build(s) failed" >&2
	exit 1
fi
echo "check-c-compilers: OK: ${nRun} build(s) across ${#compilers[@]} compiler(s) (${compilers[*]})"

##	History:
##		2026-08-31  Created, after gcc 13 on the hosted runner rejected what the
##		            local gcc 14 accepted.
##		2026-08-31  The two OOM tests, for -Wclobbered around the setjmp.
