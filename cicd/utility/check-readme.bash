#!/usr/bin/env bash

##	Purpose:
##		Compile the README's C, Go and Zig examples the way a reader would: each
##		block verbatim, wrapped in whatever a reader has to add around it, and
##		built with the line the README itself gives.
##
##		These are the first thing a consumer copies, and nothing else in the
##		pipeline builds them. The C one also carries a build order that is easy
##		to "tidy" wrong: shcl.h asks for a POSIX level, a feature request only
##		counts before the first system header, and putting <stdio.h> above the
##		header turns the file tier into a wall of implicit declarations. The Go
##		one is unforgiving in a different way - an unused variable there is a
##		compile error, not a warning, so a fragment that reads fine does not
##		build.
##	Syntax:
##		check-readme.bash [README] [HEADER]
##	Exit: 0 = every example builds, 1 = one does not, 2 = usage or missing input.
##	History: At bottom of script.

##	Copyright © 2026 Bubbles (ID: XଌฅრX۳ᛟԃლፀƅꓩหδლც)
##	Licensed under The MIT License (MIT). Full text at:
##		https://mit-license.org/
##	SPDX-License-Identifier: MIT


set -Eeuo pipefail

repoDir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
readme="${1:-${repoDir}/README.md}"
header="${2:-${repoDir}/source/c/shcl.h}"
[[ -f "${readme}" ]] || { echo "check-readme: no README at ${readme}" >&2; exit 2 ;}
[[ -f "${header}" ]] || { echo "check-readme: no header at ${header}" >&2; exit 2 ;}

tmpDir="$(mktemp -d)"; trap 'rm -rf "${tmpDir}"' EXIT
cp "${header}" "${tmpDir}/"

##	The C example is the fenced ```c block; the file has exactly one.
awk '/^```c$/ { inBlock = 1; next } /^```$/ { inBlock = 0 } inBlock' "${readme}" > "${tmpDir}/block.c"
[[ -s "${tmpDir}/block.c" ]] || { echo "check-readme: no c example found in ${readme}" >&2; exit 2 ;}

##	Without a system include of its own the ordering is not being exercised at
##	all - the header pulls in what the example uses, and any order compiles.
grep -q '^#include <' "${tmpDir}/block.c" \
	|| { echo "check-readme: the example shows no system include, so its build order proves nothing" >&2; exit 1 ;}

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
	echo "check-readme: the README's C example does not build:" >&2
	head -n 20 "${tmpDir}/cc.err" >&2
	exit 1
fi
echo "check-readme: the C example builds as written"

##	Go. The fragment is statements plus the import line it already shows, so a
##	reader adds a package clause, a main() and the two standard imports the
##	body calls. The module resolves the library from the tree rather than the
##	proxy, so this needs no network.
awk '/^```go$/ { inBlock = 1; next } /^```$/ { inBlock = 0 } inBlock' "${readme}" > "${tmpDir}/block.go"
[[ -s "${tmpDir}/block.go" ]] || { echo "check-readme: no go example found in ${readme}" >&2; exit 2 ;}
mkdir -p "${tmpDir}/goex"
{
	echo 'package main'
	echo
	echo 'import ('
	echo '	"fmt"'
	echo '	"log"'
	sed -n 's/^import shcl \(.*\)$/	shcl \1/p' "${tmpDir}/block.go"
	echo ')'
	echo
	echo 'func main() {'
	grep -v '^import shcl ' "${tmpDir}/block.go"
	echo '}'
} > "${tmpDir}/goex/main.go"
{
	echo 'module readme-example'
	echo
	echo 'go 1.24'
	echo
	echo 'require github.com/jim-collier/shcl/source/go/v2 v2.0.0'
	echo
	echo "replace github.com/jim-collier/shcl/source/go/v2 => ${repoDir}/source/go"
} > "${tmpDir}/goex/go.mod"
if ! ( cd "${tmpDir}/goex" && GOFLAGS=-mod=mod go build -o /dev/null . ) 2> "${tmpDir}/go.err"; then
	echo "check-readme: the README's Go example does not build:" >&2
	head -n 20 "${tmpDir}/go.err" >&2
	exit 1
fi
echo "check-readme: the Go example builds as written"

##	Zig. The block is helper functions and then statements, so the statements
##	go in a main(); everything else is verbatim, including the two-line impl.c
##	and the build line the README prints beside it. Skipped out loud where
##	there is no zig - it is not a build dependency of anything shipped.
if command -v zig >/dev/null 2>&1; then
	awk '/^```zig$/ { inBlock = 1; next } /^```$/ { inBlock = 0 } inBlock' "${readme}" > "${tmpDir}/block.zig"
	[[ -s "${tmpDir}/block.zig" ]] || { echo "check-readme: no zig example found in ${readme}" >&2; exit 2 ;}
	mkdir -p "${tmpDir}/zigex"
	cp "${header}" "${tmpDir}/zigex/"
	printf '#define SHCL_IMPLEMENTATION\n#include "shcl.h"\n' > "${tmpDir}/zigex/impl.c"
	awk 'BEGIN { pre = 1 }
	     pre && /^\/\/ The C file tier/ { pre = 0; print "pub fn main() void {"; print; next }
	     { print }
	     END { print "}" }' "${tmpDir}/block.zig" > "${tmpDir}/zigex/main.zig"
	grep -q '^pub fn main' "${tmpDir}/zigex/main.zig" \
		|| { echo "check-readme: the zig example no longer carries the line the main() split is taken at" >&2; exit 1 ;}
	if ! ( cd "${tmpDir}/zigex" && zig build-exe main.zig impl.c -lc -lm -I. ) > "${tmpDir}/zig.err" 2>&1; then
		echo "check-readme: the README's Zig example does not build:" >&2
		head -n 20 "${tmpDir}/zig.err" >&2
		exit 1
	fi
	echo "check-readme: the Zig example builds as written"
else
	echo "check-readme: skipping the Zig example (no zig here)"
fi
echo "check-readme: OK"

##	History:
##		2026-08-30  Created, after the example was found not to build with the
##		            system includes a reader adds above the header.
##		2026-09-08  Go and Zig join it, after the Go fragment was found not to
##		            compile at all and the Zig one to be checked by nothing.
