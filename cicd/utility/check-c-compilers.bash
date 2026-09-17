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

##	Copyright (c) 2026 Bubbles
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

## Under the gate a thin sweep is a failure, not a pass: the disagreement this
## exists for (gcc 12 and 13 against 14 and 15 over -Wclobbered) cannot show
## with one compiler, and a runner that lost one would otherwise report OK for
## good. Locally, what is installed is what is checked, and the summary says
## which. A thin sweep is noted in SHCL_GATE_SKIPS, so the run is not taken for
## a full gate.
declare -i nGcc=0
for c in "${compilers[@]}"; do
	if [[ "${c}" == gcc-* ]]; then nGcc+=1; fi
done
if ((nGcc < 2)) || [[ " ${compilers[*]} " != *" clang "* ]]; then
	if [[ -n "${SHCL_GATE_STRICT:-}" ]]; then
		echo "check-c-compilers: the gate needs two versioned gccs and clang; found: ${compilers[*]}" >&2
		exit 1
	fi
	echo check-c-compilers >> "${SHCL_GATE_SKIPS:-/dev/null}"
fi

fBuild(){  ## fBuild CC OPT SRC [FLAG...]
	local cc="$1" opt="$2" src="$3" out
	shift 3
	nRun+=1
	if ! out="$("${cc}" -std=c11 "${opt}" "$@" -Wall -Wextra -Werror -I"${repoDir}/source/c" \
		"${repoDir}/${src}" -o "${tmpDir}/out" -lm -lpthread 2>&1)"; then
		echo "check-c-compilers: ${cc} ${opt} ${*:+$* }refuses ${src}:" >&2
		## Not a pipe: head quits after 8 lines, and under pipefail the writer's
		## SIGPIPE on a long cascade ended the whole sweep.
		head -n 8 <<< "${out}" >&2
		nBad+=1
	fi
}

fRefuse(){  ## fRefuse CC SRC WANT - a build that must fail, and fail saying WANT
	local cc="$1" src="$2" want="$3" out
	nRun+=1
	if out="$("${cc}" -std=c11 -O2 -Wall -Wextra -Werror -I"${repoDir}/source/c" \
		"${src}" -o "${tmpDir}/out" -lm -lpthread 2>&1)"; then
		echo "check-c-compilers: ${cc} accepted ${src##*/}, which must not compile" >&2
		nBad+=1
	elif ! grep -qF -- "${want}" <<< "${out}"; then
		## Refused for some other reason, so it proves nothing about the guard.
		echo "check-c-compilers: ${cc} refused ${src##*/} without saying \"${want}\":" >&2
		head -n 8 <<< "${out}" >&2
		nBad+=1
	fi
}

## The header sets the POSIX feature level, and glibc only honors that before
## the first system header. Its sentinel says so; without it the symptom is
## pages of implicit-declaration errors that never mention include order. The
## three builds above are the other half: they include the header first and
## must stay clean, so a guard that fired unconditionally would show up there.
cat > "${tmpDir}/order-bad.c" <<'EOF'
#include <stdio.h>
#define SHCL_IMPLEMENTATION
#include "shcl.h"
int main(void){ return 0; }
EOF

## Same flags the build and test stages use, so a disagreement here is a
## disagreement there.
for cc in "${compilers[@]}"; do
	for src in source/c/cmd/shcl/main.c source/c/tests/conformance.c source/c/tests/mem_bounds.c; do
		fBuild "${cc}" -O2 "${src}"
	done
	## Most Linux consumers define _GNU_SOURCE, and glibc declares more under it,
	## so a static name in the header can collide with one of those functions.
	fBuild "${cc}" -O2 source/c/cmd/shcl/main.c -D_GNU_SOURCE
	fRefuse "${cc}" "${tmpDir}/order-bad.c" "included before any system header"
	## The two OOM tests get every level. Their shape is the one this gate was
	## written for - which locals a compiler thinks a setjmp's unwind can
	## clobber, and whether it gives the frame a pointer and saved xmm registers
	## at all - and gcc decides both per level, so -O2 alone proves one of five.
	## win-runners.bash sweeps the same five on windows for the same reason.
	for src in source/c/tests/oom_hook.c source/c/tests/oom_recover.c; do
		for opt in -O0 -O1 -O2 -Os -O3; do
			fBuild "${cc}" "${opt}" "${src}"
		done
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
##		2026-09-05  A floor on the compiler set under SHCL_GATE_STRICT.
##		2026-09-08  The two OOM tests build at every optimization level, since
##		            the shape they exist for is one gcc decides per level.
##		2026-09-14  A build with _GNU_SOURCE defined. A long error cascade no
##		            longer ends the sweep.
##		2026-09-17  A build that must be refused: the header included after a
##		            system header, which has to name include order.
