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

##	Copyright (c) 2026 Bubbles
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
elif [[ -n "${SHCL_GATE_STRICT:-}" ]]; then
	echo "check-readme: no zig here, and the gate requires it" >&2
	exit 1
else
	echo "check-readme: skipping the Zig example (no zig here)"
	echo check-readme >> "${SHCL_GATE_SKIPS:-/dev/null}"
fi
##	The transcripts. A ```console block reads as real output, so every `$ shcl`
##	line in one is run against the README's own server.shcl and schema, and has
##	to print what the block shows, stderr included, in the order a terminal
##	shows it. They drifted twice, each time a new line of output reached no
##	transcript. Two files the README describes rather than shows are made here:
##	server.shcl with the colon knocked off line 3, for the block that starts
##	with `shcl check server.shcl`, and an app.shcl whose line 2 carries the
##	misspelled key its schema block reports.
cli="${SHCL_CLI:-${repoDir}/source/rust/target/debug/shcl}"
[[ -x "${cli}" ]] || { echo "check-readme: no built CLI at ${cli} to run the transcripts with" >&2; exit 2 ;}
tx="${tmpDir}/tx"
mkdir -p "${tx}/bin" "${tx}/cases"
ln -s "${cli}" "${tx}/bin/shcl"
awk '/^## What a .shcl file looks like/ { f = 1 } f && /^```text$/ { b = 1; next } b && /^```$/ { exit } b' "${readme}" > "${tx}/server-shown.shcl"
awk '/^Hand it a schema/ { f = 1 } f && /^```text$/ { b = 1; next } b && /^```$/ { exit } b' "${readme}" > "${tx}/app-schema.shcl"
[[ -s "${tx}/server-shown.shcl" && -s "${tx}/app-schema.shcl" ]] \
	|| { echo "check-readme: the transcripts' server.shcl or schema block is gone from ${readme}" >&2; exit 2 ;}
sed '3s/://' "${tx}/server-shown.shcl" > "${tx}/server-knocked.shcl"
printf 'workers: 4\nlog-levle: warn\n' > "${tx}/app.shcl"
##	One case per command: N.cmd, N.want (the lines under it, less the blank that
##	ends them) and N.blk (which block it is in).
awk -v dir="${tx}/cases" '
	function done_case() { sub(/\n\n$/, "\n", buf); printf "%s", buf > (dir "/" n ".want"); close(dir "/" n ".want"); n = 0 }
	/^```console$/ { inb = 1; blk++; next }
	inb && /^```$/ { if (n) done_case(); inb = 0; next }
	inb && /^\$ / { if (n) done_case(); n = ++cases; print substr($0, 3) > (dir "/" n ".cmd"); close(dir "/" n ".cmd"); print blk > (dir "/" n ".blk"); close(dir "/" n ".blk"); buf = ""; next }
	inb && n { buf = buf $0 "\n" }' "${readme}"
nCases=0; nTxBad=0; lastBlk=""
for cmdFile in $(find "${tx}/cases" -name '*.cmd' | sort -t/ -k1 -V); do
	base="${cmdFile%.cmd}"
	cmd="$(cat "${cmdFile}")"
	blk="$(cat "${base}.blk")"
	if [[ "${blk}" != "${lastBlk}" ]]; then
		lastBlk="${blk}"
		if [[ "${cmd}" == "shcl check server.shcl"* ]]; then
			cp "${tx}/server-knocked.shcl" "${tx}/server.shcl"
		else
			cp "${tx}/server-shown.shcl" "${tx}/server.shcl"
		fi
	fi
	[[ "${cmd}" == shcl\ * ]] || continue
	want="$(cat "${base}.want")"
	got="$(cd "${tx}" && PATH="${tx}/bin:${PATH}" bash -c "${cmd}" 2>&1 </dev/null || true)"
	nCases=$((nCases + 1))
	if [[ "${got}" != "${want}" ]]; then
		echo "check-readme: transcript: '${cmd}' prints:" >&2
		diff <(printf '%s\n' "${want}") <(printf '%s\n' "${got}") | sed 's/^/	/' >&2 || true
		nTxBad=$((nTxBad + 1))
	fi
done
if ((nCases < 10)); then
	echo "check-readme: transcript: only ${nCases} command(s) found in the README's console blocks" >&2
	exit 1
fi
((nTxBad == 0)) || exit 1
echo "check-readme: the ${nCases} transcript command(s) print what the README shows"
echo "check-readme: OK"

##	History:
##		2026-08-30  Created, after the example was found not to build with the
##		            system includes a reader adds above the header.
##		2026-09-08  Go and Zig join it, after the Go fragment was found not to
##		            compile at all and the Zig one to be checked by nothing.
##		2026-09-17  The zig skip is a failure under SHCL_GATE_STRICT and is noted
##		            in SHCL_GATE_SKIPS, so a local run that took it is not
##		            recorded as having run everything.
##		2026-09-18  The console transcripts are run and compared, after a new
##		            line of output reached none of them for the second time.
