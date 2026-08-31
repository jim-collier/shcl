#!/usr/bin/env bash

##	Purpose:
##		Compile the README's C example the way a reader would: the block
##		verbatim, wrapped in a main(), against the vendored header and with the
##		compile line the README itself gives.
##
##		The example is the first thing a C consumer copies, and it is the one
##		example nothing else in the pipeline builds. It also carries a build
##		order that is easy to "tidy" wrong: shcl.h asks for a POSIX level, a
##		feature request only counts before the first system header, and putting
##		<stdio.h> above the header turns the file tier into a wall of implicit
##		declarations.
##	Syntax:
##		check-readme-c.bash [README] [HEADER]
##	Exit: 0 = the example builds, 1 = it does not, 2 = usage or missing input.
##	History: At bottom of script.

##	Copyright © 2026 Bubbles (ID: XଌฅრX۳ᛟԃლፀƅꓩหδლც)
##	Licensed under The MIT License (MIT). Full text at:
##		https://mit-license.org/
##	SPDX-License-Identifier: MIT


set -Eeuo pipefail

repoDir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
readme="${1:-${repoDir}/README.md}"
header="${2:-${repoDir}/source/c/shcl.h}"
[[ -f "${readme}" ]] || { echo "check-readme-c: no README at ${readme}" >&2; exit 2 ;}
[[ -f "${header}" ]] || { echo "check-readme-c: no header at ${header}" >&2; exit 2 ;}

tmpDir="$(mktemp -d)"; trap 'rm -rf "${tmpDir}"' EXIT
cp "${header}" "${tmpDir}/"

##	The C example is the fenced ```c block; the file has exactly one.
awk '/^```c$/ { inBlock = 1; next } /^```$/ { inBlock = 0 } inBlock' "${readme}" > "${tmpDir}/block.c"
[[ -s "${tmpDir}/block.c" ]] || { echo "check-readme-c: no c example found in ${readme}" >&2; exit 2 ;}

##	Without a system include of its own the ordering is not being exercised at
##	all - the header pulls in what the example uses, and any order compiles.
grep -q '^#include <' "${tmpDir}/block.c" \
	|| { echo "check-readme-c: the example shows no system include, so its build order proves nothing" >&2; exit 1 ;}

##	Everything above the P() macro is the preamble; the rest is statements, so
##	it needs a function around it.
awk 'BEGIN { pre = 1 }
     /^#define P\(s\)/ { pre = 0; print; print ""; print "int main(void) {"; next }
     pre { print; next }
     { print }
     END { print "\treturn 0;"; print "}" }' "${tmpDir}/block.c" > "${tmpDir}/example.c"

##	The compile line the README gives its readers, plus -Werror so a warning
##	the reader would see is a failure here.
if ! cc -std=c11 -O2 -Wall -Wextra -Werror -I"${tmpDir}" "${tmpDir}/example.c" -o "${tmpDir}/example" -lm 2> "${tmpDir}/cc.err"; then
	echo "check-readme-c: the README's C example does not build:" >&2
	head -n 20 "${tmpDir}/cc.err" >&2
	exit 1
fi
echo "check-readme-c: OK: the README's C example builds as written"

##	History:
##		2026-08-30  Created, after the example was found not to build with the
##		            system includes a reader adds above the header.
