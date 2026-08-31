#!/usr/bin/env bash

##	Purpose:
##		Pin the shell surface: the two wrappers, the one-liner's scope hygiene,
##		and the packaging script's version handling. None of it is reachable
##		from the corpus or the CLI gate, and every row here is a defect a review
##		round found in a shipped script.
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
echo "shell-regress: OK: wrappers, one-liner scope, packaging, and no unguarded grep substitutions"

##	History:
##		2026-08-30  Created, pinning the wrapper and installer defects from the
##		            20260829 and 20260830 rounds.
