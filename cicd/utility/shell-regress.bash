#!/usr/bin/env bash

##	Purpose:
##		Pin the tooling surface: the two wrappers, the one-liner's scope hygiene,
##		the packaging script's version handling, and the comparison worker's
##		argument handling. None of it is reachable from the corpus or the CLI
##		gate, and every row here is a defect a review round found.
##
##		Also scans the repo's own errexit scripts for the trap that has now bit
##		four times: a `grep` inside a command substitution whose result is
##		assigned. When it matches nothing the assignment fails, errexit kills
##		the script, and the check that would have printed the reason never runs.
##		A substitution ending in `|| true` is fine, which is the fix each time.
##	Syntax:
##		shell-regress.bash [--cli PATH]
##		  --cli PATH  the shcl binary the wrappers should resolve to
##		              (default source/rust/target/debug/shcl)
##	Exit: 0 = clean, 1 = a check failed, 2 = usage or missing input.
##	History: At bottom of script.

##	Copyright © 2026 Bubbles (ID: XଌฅრX۳ᛟԃლፀƅꓩหδლც)
##	Licensed under The MIT License (MIT). Full text at:
##		https://mit-license.org/
##	SPDX-License-Identifier: MIT


set -Eeuo pipefail

repoDir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
cli="${repoDir}/source/rust/target/debug/shcl"
while (($#)); do case "$1" in
	--cli)     cli="${2:-}"; shift 2 ;;
	-h|--help) grep -E '^##' "$0" | sed 's/^##\t\?//'; exit 0 ;;
	*)         echo "shell-regress: unknown argument: $1" >&2; exit 2 ;;
esac; done
[[ -x "${cli}" ]] || { echo "shell-regress: no shcl binary at ${cli}" >&2; exit 2; }

tmpDir="$(mktemp -d)"; trap 'rm -rf "${tmpDir}"' EXIT
printf 'a: 1\n' > "${tmpDir}/t.shcl"
declare -i nBad=0
fBad(){ echo "shell-regress: $1" >&2; nBad+=1 ;}

##	20260830 item 21: -x is true for a directory, so a directory passed as
##	SHCL_BIN got as far as being run.
out="$(SHCL_BIN="${tmpDir}" bash -c "source '${repoDir}/source/bash/shcl.bash'; shcl_get '${tmpDir}/t.shcl' a" 2>&1 || true)"
[[ "${out}" == *"not an executable file"* ]] || fBad "bash wrapper took a directory as SHCL_BIN: ${out@Q}"
out="$(SHCL_BIN="${tmpDir}/nope" bash -c "source '${repoDir}/source/bash/shcl.bash'; shcl_get '${tmpDir}/t.shcl' a" 2>&1 || true)"
[[ "${out}" == *"not an executable file"* ]] || fBad "bash wrapper took a missing SHCL_BIN: ${out@Q}"
out="$(SHCL_BIN="${cli}" bash -c "source '${repoDir}/source/bash/shcl.bash'; shcl_get '${tmpDir}/t.shcl' a" 2>&1 || true)"
[[ "${out}" == "1" ]] || fBad "bash wrapper did not read through a pinned SHCL_BIN: ${out@Q}"

##	The private cache is not an interface: an inherited value would beat every
##	documented lookup step.
out="$(_SHCL_BIN=/nonexistent SHCL_BIN="${cli}" bash -c "source '${repoDir}/source/bash/shcl.bash'; shcl_get '${tmpDir}/t.shcl' a" 2>&1 || true)"
[[ "${out}" == "1" ]] || fBad "bash wrapper honored an inherited _SHCL_BIN: ${out@Q}"

if command -v pwsh > /dev/null 2>&1; then
	out="$(pwsh -NoProfile -Command ". '${repoDir}/source/powershell/shcl.ps1'; \$env:SHCL_BIN = '${tmpDir}'; shcl_get '${tmpDir}/t.shcl' a" 2>&1 || true)"
	[[ "${out}" == *"not executable"* ]] || fBad "PowerShell wrapper took a directory as SHCL_BIN: ${out@Q}"

	##	20260830b item 10: the symlink resolver called a .NET 6 method that
	##	Windows PowerShell 5.1 does not have, unguarded and at load, so every
	##	dot-source on 5.1 hit it. An Env: item stands in for 5.1's method-less
	##	FileInfo. The link row is the other half: resolution still works where
	##	the method does exist.
	#  shellcheck disable=2016  ## PowerShell's own $variables, quoted so bash leaves them alone.
	{
		echo 'Set-StrictMode -Version Latest'
		echo '$ErrorActionPreference = "Stop"'
		sed -n '/^function _shcl_scriptdir/,/^}/p' "${repoDir}/source/powershell/shcl.ps1"
		echo 'try { $null = _shcl_scriptdir "Env:HOME"; Write-Output "resolved" } catch { Write-Output "threw" }'
	} > "${tmpDir}/scriptdir.ps1"
	out="$(pwsh -NoProfile -File "${tmpDir}/scriptdir.ps1" 2>&1 || true)"
	[[ "${out}" == "resolved" ]] || fBad "PowerShell wrapper called a missing link resolver: ${out@Q}"
	mkdir -p "${tmpDir}/real" "${tmpDir}/lnk"
	cp "${repoDir}/source/powershell/shcl.ps1" "${tmpDir}/real/"
	ln -sf "${tmpDir}/real/shcl.ps1" "${tmpDir}/lnk/shcl.ps1"
	out="$(pwsh -NoProfile -Command ". '${tmpDir}/lnk/shcl.ps1'; Write-Output \"root=\$script:_SHCL_ROOT\"" 2>&1 || true)"
	[[ "${out}" == "root=${tmpDir}/real" ]] || fBad "PowerShell wrapper did not resolve its own symlink: ${out@Q}"

	##	20260829 item 16 and 20260830 item 8: the script used to end the caller's
	##	shell, and then to leave strict mode and its functions behind in it.
	##	Through a file rather than -Command, so the PowerShell keeps its own
	##	quoting instead of fighting bash's.
	#  shellcheck disable=2016  ## PowerShell's own $variables, quoted so bash leaves them alone.
	{
		echo '$ErrorActionPreference = "Continue"'
		echo "& ([scriptblock]::Create((Get-Content '${repoDir}/install.ps1' -Raw))) -Help | Out-Null"
		echo 'Write-Output "eap=$ErrorActionPreference"'
		echo 'Write-Output ("fn=" + [bool](Get-Command Exit-Install -ErrorAction SilentlyContinue))'
		echo 'try { $null = @(1)[5]; Write-Output "strict=off" } catch { Write-Output "strict=on" }'
		echo 'Write-Output "alive"'
	} > "${tmpDir}/scope.ps1"
	out="$(pwsh -NoProfile -File "${tmpDir}/scope.ps1" 2>&1 || true)"
	[[ "${out}" == *"alive"* ]]        || fBad "install.ps1 -Help did not return to the caller: ${out@Q}"
	[[ "${out}" == *"eap=Continue"* ]] || fBad "install.ps1 changed the caller's error preference: ${out@Q}"
	[[ "${out}" == *"fn=False"* ]]     || fBad "install.ps1 left its functions in the caller: ${out@Q}"
	[[ "${out}" == *"strict=off"* ]]   || fBad "install.ps1 left strict mode on in the caller: ${out@Q}"
else
	echo "shell-regress: pwsh not installed - PowerShell rows skipped"
fi

##	20260830b item 12: nothing set the modes on a system install, and sudo keeps
##	the caller's umask, so under 077 the tree and the launcher came out 0700 and
##	only root could run what had just been installed for everyone. 20260901b
##	item 16: the widening covered the install root alone, and a man1 directory
##	the installer had to create stayed root-only. The installer's own lay-down
##	step runs here on a staged payload, under that umask, into a sandbox whose
##	bin and man1 directories do not exist yet.
eval "$(sed -n '/^fWidenModes()/,/^}/p;/^fTopMissing()/,/^}/p;/^fLayDown()/,/^}/p' "${repoDir}/install.bash")"
(
	umask 077
	mkdir -p "${tmpDir}/stage/code" "${tmpDir}/stage/scripts" "${tmpDir}/stage/man" "${tmpDir}/stage/completions" "${tmpDir}/sys/usr/local/share"
	printf 'bin\n'  > "${tmpDir}/stage/shcl";      chmod 700 "${tmpDir}/stage/shcl"
	printf 'data\n' > "${tmpDir}/stage/code/lib.rs"
	printf 'data\n' > "${tmpDir}/stage/scripts/shcl.bash"
	printf 'man\n'  > "${tmpDir}/stage/man/shcl.1"
	printf 'comp\n' > "${tmpDir}/stage/completions/shcl.bash"
	## The installer's own globals, as the lifted function reads them.
	# shellcheck disable=SC2034
	asroot="" tmp="${tmpDir}/stage" dest="${tmpDir}/sys/opt/shcl" link="${tmpDir}/sys/usr/local/bin/shcl"
	# shellcheck disable=SC2034
	manlink="${tmpDir}/sys/usr/local/share/man/man1/shcl.1" target=system have_dropins=1 have_docs=1
	fLayDown
)
while IFS= read -r row; do
	case "${row}" in
		"755 d "*|"755 f ${tmpDir}/sys/opt/shcl/shcl"|"644 f "*|"777 l "*) ;;
		*) fBad "install.bash left a system install unreadable: ${row}" ;;
	esac
done < <(find "${tmpDir}/sys/opt" "${tmpDir}/sys/usr/local/bin" "${tmpDir}/sys/usr/local/share/man" -printf '%m %y %p\n' | sort)
[[ -L "${tmpDir}/sys/usr/local/share/man/man1/shcl.1" ]] || fBad "install.bash did not link the man page"

##	20260901b item 18: the "not on your PATH" note compared strings against
##	`:dir:`, so a PATH element written with a trailing slash was not seen.
eval "$(sed -n '/^fOnPath()/,/^}/p' "${repoDir}/install.bash")"
(
	PATH="/usr/bin:${tmpDir}/pbin/:/bin"
	fOnPath "${tmpDir}/pbin"  || fBad "install.bash: a PATH element with a trailing slash was not seen"
	fOnPath "${tmpDir}/pbin/" || fBad "install.bash: a directory asked for with a trailing slash was not seen"
	fOnPath "${tmpDir}/pbi"   && fBad "install.bash: a PATH prefix was taken for the directory"
	fOnPath "${tmpDir}/pbin/x" && fBad "install.bash: a deeper directory was taken for a PATH element"
	exit 0
)

##	20260830b item 9: the stable channel took GitHub's date-ordered "latest
##	release" verbatim, so a patch back-ported to an older line after a newer one
##	shipped was handed out as stable. The fixture is in publish order, newest
##	first, and the answer must not be the first row.
cat > "${tmpDir}/rel.json" <<'JSON'
[
  {
    "tag_name": "v1.2.1",
    "draft": false,
    "prerelease": false
  },
  {
    "tag_name": "v2.1.0-alpha.1",
    "draft": false,
    "prerelease": true
  },
  {
    "tag_name": "v2.2.0",
    "draft": true,
    "prerelease": false
  },
  {
    "tag_name": "v2.0.0",
    "draft": false,
    "prerelease": false
  },
  {
    "tag_name": "v2.1.0-alpha.10",
    "draft": false,
    "prerelease": true
  },
  {
    "tag_name": "v2.1.0-alpha.2",
    "draft": false,
    "prerelease": true
  }
]
JSON
##	The function comes out of the shipped installer by name, so the gate runs
##	the real text rather than a copy that can drift.
eval "$(sed -n '/^fPickTag()/,/^}/p' "${repoDir}/install.bash")"
out="$(fPickTag stable "${tmpDir}/rel.json")"
[[ "${out}" == "v2.0.0" ]] || fBad "install.bash stable channel picked ${out@Q}, want v2.0.0"
##	20260829 item 21: a pre-release suffix compared as text, so alpha.10 sorted
##	below alpha.2. Both installers order the digit runs numerically.
out="$(fPickTag dev "${tmpDir}/rel.json")"
[[ "${out}" == "v2.1.0-alpha.10" ]] || fBad "install.bash dev channel picked ${out@Q}, want v2.1.0-alpha.10"

if command -v pwsh > /dev/null 2>&1; then
	#  shellcheck disable=2016  ## PowerShell's own $variables, quoted so bash leaves them alone.
	{
		sed -n '/^\tfunction Select-ReleaseTag/,/^\t}/p' "${repoDir}/install.ps1"
		echo "\$rel = Get-Content '${tmpDir}/rel.json' -Raw | ConvertFrom-Json"
		echo 'Write-Output ("stable=" + (Select-ReleaseTag stable $rel).tag_name)'
		echo 'Write-Output ("dev=" + (Select-ReleaseTag dev $rel).tag_name)'
	} > "${tmpDir}/pick.ps1"
	out="$(pwsh -NoProfile -File "${tmpDir}/pick.ps1" 2>&1 || true)"
	[[ "${out}" == *"stable=v2.0.0"* ]]        || fBad "install.ps1 stable channel: ${out@Q}"
	[[ "${out}" == *"dev=v2.1.0-alpha.10"* ]]  || fBad "install.ps1 dev channel: ${out@Q}"
fi

##	20260830b item 8: a prerelease version reached NSIS's four-integer version
##	field verbatim, and makensis rejected it under errexit, so the release stage
##	died on the first prerelease cut. A fake .exe is enough - the setup never
##	runs, it only has to build.
if command -v makensis > /dev/null 2>&1; then
	pkgDir="${tmpDir}/pkg"; mkdir -p "${pkgDir}"
	: > "${pkgDir}/shcl-2.1.0-alpha.1-windows-x86_64.exe"
	if "${repoDir}/cicd/utility/package.bash" "${repoDir}" "${pkgDir}" "2.1.0-alpha.1" > "${tmpDir}/pkg.log" 2>&1; then
		[[ -f "${pkgDir}/shcl-2.1.0-alpha.1-windows-x86_64-setup.exe" ]] \
			|| fBad "packaging a prerelease built no setup: $(cat "${tmpDir}/pkg.log")"
	else
		fBad "packaging a prerelease failed: $(cat "${tmpDir}/pkg.log")"
	fi
else
	echo "shell-regress: makensis not installed - packaging row skipped"
fi

##	20260830b item 11: $IsWindows does not exist on Windows PowerShell 5.1, and
##	reading it there throws under a caller's strict mode. Every read has to sit
##	behind a version test that short-circuits first, which cannot be exercised
##	from a 7.x session because $PSVersionTable is read-only.
while IFS= read -r h; do
	fBad "source/powershell/shcl.ps1: unguarded \$IsWindows read: ${h}"
done < <(grep -n 'IsWindows' "${repoDir}/source/powershell/shcl.ps1" \
	| grep -vE '^[0-9]+:##' | grep -vE 'PSVersion\.Major -lt 6 -or' || true)

##	The comparison worker: a bad ITERS used to be a traceback, and a zero one
##	raised while formatting a time that was never taken. The listing must also
##	survive a loader failing some way other than a missing import, and must not
##	grow a duplicate search-path entry per call.
worker="${repoDir}/cicd/utility/comparison/pyworker.py"
if [[ -f "${worker}" ]]; then
	: > "${tmpDir}/w.shcl"
	printf 'a: 1\n' > "${tmpDir}/w.shcl"
	for bad in abc 0 -1; do
		out="$(python3 "${worker}" shcl "${tmpDir}/w.shcl" "${bad}" 2>&1)" && rc=0 || rc=$?
		((rc == 2)) || fBad "pyworker.py ITERS=${bad}: exit ${rc}, expected 2"
		[[ "${out}" == usage:* ]] || fBad "pyworker.py ITERS=${bad} did not print the usage line: ${out}"
	done
	## Its own loader is called once per listed entry, so a path pushed per call
	## shows up as a duplicate after two.
	dups="$(python3 - "${worker}" <<'PYEOF'
import runpy, sys
path = sys.argv[1]
mod = runpy.run_path(path)
before = list(sys.path)
mod["load_shcl"](); mod["load_shcl"]()
print(len(sys.path) - len(before))
PYEOF
)"
	[[ "${dups}" == "1" ]] || fBad "pyworker.py load_shcl added ${dups} path entries over two calls, expected 1"
fi

##	The completions check against a subcommand that takes no options. One side
##	emitted a row for it and the other dropped any row with an empty option
##	list, so the two could never agree however the completions spelled it - a
##	lint failure blaming the completions on the day such a subcommand is added.
##	The fixture is the real files with one added, so the check runs against the
##	extractors as shipped rather than a hand-written stand-in.
gate="${repoDir}/cicd/utility/check-completions.bash"
if [[ -x "${gate}" ]]; then
	fix="${tmpDir}/optless"
	mkdir -p "${fix}/source/rust/src" "${fix}/source/completions"
	python3 - "${repoDir}" "${fix}" <<'PYEOF'
import sys
repo, fix = sys.argv[1], sys.argv[2]
TAB = chr(9)
NL = chr(10)
s = open(repo + "/source/rust/src/main.rs").read()
arm = TAB * 2 + '"count" | "instances" | "children" | "paths" => &['
s = s.replace(arm, TAB * 2 + '"ping" => &[],' + NL + arm, 1)
s = s.replace(TAB + '"paths",' + NL + "];", TAB + '"paths",' + NL + TAB + '"ping",' + NL + "];", 1)
disp = TAB * 2 + '"paths" => do_paths(o),'
s = s.replace(disp, disp + NL + TAB * 2 + '"ping" => 0,', 1)
open(fix + "/source/rust/src/main.rs", "w").write(s)
for name in ("shcl.bash", "_shcl"):
    c = open(repo + "/source/completions/" + name).read()
    row = TAB * 2 + "count|instances|children|paths) echo '--strictness"
    c = c.replace(row, TAB * 2 + "ping)            echo '' ;;" + NL + row, 1)
    if name == "shcl.bash":
        c = c.replace("instances children paths help", "instances children paths ping help", 1)
    else:
        pl = TAB * 2 + "'paths:every field path in the document'"
        c = c.replace(pl, pl + NL + TAB * 2 + "'ping:say nothing'", 1)
    open(fix + "/source/completions/" + name, "w").write(c)
PYEOF
	if ! out="$("${gate}" "${fix}" 2>&1)"; then
		fBad "check-completions rejects an option-less subcommand the completions spell correctly: ${out}"
	fi
fi

##	Both bash installers used to heredoc their own source header, so the help
##	opened with the file name as a comment and every wrapped line carried a `##`
##	and a hard tab. The PowerShell installer printed clean prose, so the two
##	documented installers spoke in different registers.
for inst in install.bash install-dev.bash; do
	[[ -f "${repoDir}/${inst}" ]] || continue
	help="$(bash "${repoDir}/${inst}" --help 2>&1 || true)"
	if grep -q '^##' <<<"${help}"; then
		fBad "${inst} --help prints comment markup"
	fi
	if grep -qP '\t' <<<"${help}"; then
		fBad "${inst} --help prints hard tabs, which render raggedly off tab width 8"
	fi
	[[ -n "${help}" ]] || fBad "${inst} --help printed nothing"
done

##	The bash uninstall reported "removed" while leaving a directory full of files
##	it had not installed. The PowerShell installer already said so; this is the
##	same wording on the bash side.
if [[ -f "${repoDir}/install.bash" ]]; then
	fake="${tmpDir}/fakehome"
	mkdir -p "${fake}/.local/share/shcl"
	out="$(HOME="${fake}" bash "${repoDir}/install.bash" --target user --uninstall --yes 2>&1 || true)"
	grep -q '^removed$' <<<"${out}" || fBad "install.bash --uninstall of an empty dir did not report a clean removal: ${out}"
	mkdir -p "${fake}/.local/share/shcl"
	printf 'x\n' > "${fake}/.local/share/shcl/not-ours.txt"
	out="$(HOME="${fake}" bash "${repoDir}/install.bash" --target user --uninstall --yes 2>&1 || true)"
	grep -q 'did not put there' <<<"${out}" || fBad "install.bash --uninstall said nothing about the files it left behind: ${out}"
	[[ -f "${fake}/.local/share/shcl/not-ours.txt" ]] || fBad "install.bash --uninstall removed a file it did not install"
fi

##	install.ps1 used to run the binary only after writing it into place, where
##	the Linux installer runs it from the temp dir first so one that will not
##	start never becomes an install. Order in the source is the whole assertion;
##	running the real thing needs a network and a release.
ps1="${repoDir}/install.ps1"
if [[ -f "${ps1}" ]]; then
	smokeLine="$({ grep -n "(Join-Path \$tmp 'shcl.exe') version" "${ps1}" || true ;} | head -n1 | cut -d: -f1)"
	publishLine="$({ grep -n "Move-Item -Force -LiteralPath (Join-Path \$dest '.shcl.exe.new')" "${ps1}" || true ;} | head -n1 | cut -d: -f1)"
	if [[ -z "${smokeLine}" ]]; then
		fBad "install.ps1 never runs the downloaded binary from the temp dir"
	elif [[ -z "${publishLine}" ]]; then
		fBad "install.ps1: cannot find where the binary is published"
	elif ((smokeLine >= publishLine)); then
		fBad "install.ps1 runs the binary at line ${smokeLine}, after publishing it at ${publishLine}"
	fi
	## A bare native call throws under this script's error preference on 7.4+,
	## which turned a failing binary into an exception after the success message.
	grep -q 'LASTEXITCODE -ne 0' <<<"$(sed -n "${smokeLine:-1},+4p" "${ps1}")" \
		|| fBad "install.ps1 does not test the smoke run's exit status"
fi

##	The profiler stage's hot-spot report. Its only diagnostics go to stderr, and
##	the stage used to discard them, so the log recorded the failure with no cause.
report="${repoDir}/cicd/utility/flame-report.py"
if [[ -f "${report}" ]]; then
	out="$(python3 "${report}" --dir "${tmpDir}/no-such-profile-dir" 2>&1 || true)"
	[[ "${out}" == *"no profiling dir"* ]] || fBad "flame-report.py says nothing usable about a missing directory: ${out}"
	if grep -qE 'flame-report\.py[^|]*2>/dev/null' "${repoDir}/cicd/cicd.bash"; then
		fBad "cicd.bash discards the hot-spot report's stderr, which is its only diagnostic"
	fi
fi

##	A one-line loop body that is a `[[ ... ]] && ...` list. When the test fails
##	on the last iteration the loop returns 1, which is harmless at statement
##	level on this bash but kills the caller the moment the loop becomes the last
##	command in a function. Unmatched globs are the usual way in. `|| continue`
##	or an `if` is the fix.
while IFS= read -r f; do
	grep -qE '^set -[A-Za-z]*e' "${f}" || continue
	hits="$(grep -nE 'do[[:space:]]+\[\[[^]]*\]\][[:space:]]*&&[^;]*;[[:space:]]*done' "${f}" || true)"
	if [[ -n "${hits}" ]]; then
		while IFS= read -r h; do fBad "${f#"${repoDir}/"}: loop body ends on a failed-test && list: ${h}"; done <<<"${hits}"
	fi
done < <(find "${repoDir}" -name '*.bash' -not -path '*/target/*' -not -path '*/.git/*' | sort)

##	`\t` in a grep -E pattern. POSIX ERE has no such escape, so the pattern
##	matches nothing under the grep a script gets, while matching fine under the
##	interactive one on this box - a check that looks like it works and asserts
##	nothing. Twice in one round. Use a literal tab or `[[:space:]]`.
while IFS= read -r f; do
	hits="$(grep -nE "grep [^|;]*-[A-Za-z]*E[A-Za-z]* '[^']*\\\\t" "${f}" || true)"
	if [[ -n "${hits}" ]]; then
		while IFS= read -r h; do fBad "${f#"${repoDir}/"}: backslash-t in a grep -E pattern, which POSIX ERE does not read as a tab: ${h}"; done <<<"${hits}"
	fi
done < <(find "${repoDir}/cicd" -name '*.bash' | sort)

##	The static half. Line continuations are joined first, so a substitution that
##	ends in `|| true` several lines down is read as guarded.
while IFS= read -r f; do
	grep -qE '^set -[A-Za-z]*e' "${f}" || continue
	hits="$(sed -e :a -e '/\\$/N; s/\\\n//; ta' "${f}" \
		| grep -nE '^[[:space:]]*[A-Za-z_][A-Za-z0-9_]*=.*\$\(.*\bgrep\b' \
		| grep -vE '\|\|[[:space:]]*(true|:)' || true)"
	if [[ -n "${hits}" ]]; then
		while IFS= read -r h; do fBad "${f#"${repoDir}/"}: unguarded grep in an assigned substitution: ${h}"; done <<<"${hits}"
	fi
done < <(find "${repoDir}" -name '*.bash' -not -path '*/target/*' -not -path '*/.git/*' | sort)

if ((nBad)); then
	echo "shell-regress: ${nBad} check(s) failed" >&2
	exit 1
fi
echo "shell-regress: OK: wrappers, one-liner scope, packaging, installers, comparison worker, and no unguarded greps, failed-test loop bodies or tab escapes in an ERE"

##	History:
##		2026-08-30  Created, pinning the wrapper and installer defects from the
##		            20260829 and 20260830 rounds.
