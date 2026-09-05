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

##	Is tool $1 here? Under the gate a missing one is a failure rather than a
##	skipped block: a runner that loses a tool would otherwise report OK forever.
##	Locally it stays a skip, since a working copy need not carry every tool.
fHave(){
	command -v "$1" > /dev/null 2>&1 && return 0
	[[ -n "${SHCL_GATE_STRICT:-}" ]] && fBad "$1 is missing and the gate requires it"
	return 1
}

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

##	20260904 items 10 and 11: bash cuts `--opt=value` at the `=` before a
##	completion function runs, and the three value options added in 20260830b
##	were missing from the lists that skip a value, so the FILE slot went to
##	the value. Driven the way readline hands the words over, both with
##	bash-completion's word joining and with the completion's own fallback.
mkdir -p "${tmpDir}/comp" && touch "${tmpDir}/comp/alpha.shcl" "${tmpDir}/comp/beta.txt"
fComplete(){
	## $1 = lib|bare, $2 = the command line as typed (a trailing space means a
	## fresh word). Prints COMPREPLY space-joined.
	local setup=":"
	[[ "$1" == lib ]] && setup="source /usr/share/bash-completion/bash_completion"
	bash -c '
		'"${setup}"'; source '"'${repoDir}/source/completions/shcl.bash'"'
		line="$1"; COMP_WORDS=()
		for t in ${line}; do
			if [[ "${t}" == --*=* ]]; then COMP_WORDS+=("${t%%=*}" "="); v="${t#*=}"; [[ -n "${v}" ]] && COMP_WORDS+=("${v}")
			else COMP_WORDS+=("${t}"); fi
		done
		[[ "${line}" == *" " ]] && COMP_WORDS+=("")
		COMP_CWORD=$(( ${#COMP_WORDS[@]} - 1 )); COMP_LINE="${line}"; COMP_POINT=${#line}
		cd '"'${tmpDir}/comp'"'; COMPREPLY=(); _shcl; printf "%s" "${COMPREPLY[*]}"
	' _ "$2" 2>/dev/null || true
}
compModes=(bare)
[[ -r /usr/share/bash-completion/bash_completion ]] && compModes+=(lib)
for mode in "${compModes[@]}"; do
	out="$(fComplete "${mode}" "shcl check --strictness=st")"
	[[ "${out}" == "standard strict" ]] || fBad "bash completion (${mode}) on --strictness=st: ${out@Q}"
	out="$(fComplete "${mode}" "shcl check --strictness=")"
	[[ "${out}" == *standard* ]] || fBad "bash completion (${mode}) on --strictness=: ${out@Q}"
	out="$(fComplete "${mode}" "shcl check --strictness=standard al")"
	[[ "${out}" == "alpha.shcl" ]] || fBad "bash completion (${mode}) lost the FILE slot after --strictness=standard: ${out@Q}"
	out="$(fComplete "${mode}" "shcl get --on-bad=fl")"
	[[ "${out}" == "flag" ]] || fBad "bash completion (${mode}) on --on-bad=fl: ${out@Q}"
	out="$(fComplete "${mode}" "shcl fmt --schema=al")"
	[[ "${out}" == "alpha.shcl" ]] || fBad "bash completion (${mode}) on --schema=al: ${out@Q}"
	out="$(fComplete "${mode}" "shcl fmt --remove ")"
	[[ -z "${out}" ]] || fBad "bash completion (${mode}) offered files where --remove takes a PATH: ${out@Q}"
	out="$(fComplete "${mode}" "shcl fmt --remove x ")"
	[[ "${out}" == "alpha.shcl beta.txt" ]] || fBad "bash completion (${mode}) lost the FILE slot after --remove x: ${out@Q}"
	out="$(fComplete "${mode}" "shcl fmt --set-default=x=1 ")"
	[[ "${out}" == "alpha.shcl beta.txt" ]] || fBad "bash completion (${mode}) lost the FILE slot after --set-default=x=1: ${out@Q}"
	out="$(fComplete "${mode}" "shcl check --strictness st")"
	[[ "${out}" == "standard strict" ]] || fBad "bash completion (${mode}) on the space form: ${out@Q}"
done

if fHave pwsh; then
	##	20260904 item 16: PowerShell reads a bare `--` as its own token and drops
	##	it before a dot-sourced function sees its arguments; the quoted spelling
	##	is the documented way through. Both halves are pinned, so a PowerShell
	##	release that changes either shows up here.
	printf -- '-dash: 5\n' > "${tmpDir}/dash.shcl"
	out="$(pwsh -NoProfile -Command ". '${repoDir}/source/powershell/shcl.ps1'; \$env:SHCL_BIN = '${cli}'; shcl get -- '${tmpDir}/dash.shcl' '-dash'" 2>&1 || true)"
	[[ "${out}" == *"unknown option"* ]] || fBad "pwsh now hands a bare -- to the sourced function; the wrapper note is stale: ${out@Q}"
	out="$(pwsh -NoProfile -Command ". '${repoDir}/source/powershell/shcl.ps1'; \$env:SHCL_BIN = '${cli}'; shcl get '--' '${tmpDir}/dash.shcl' '-dash'" 2>&1 || true)"
	[[ "${out}" == "5" ]] || fBad "pwsh dot-sourced shcl did not take a quoted --: ${out@Q}"

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

##	20260901b item 34: the uninstall's payload globs used to expand in the
##	unprivileged shell that called sudo, so a system tree only root could list
##	left them unexpanded - nothing was removed and the run then reported the
##	install directory as holding files it had not put there. The stand-in below
##	is what privilege buys: a directory the calling shell cannot read and the
##	command it runs can.
eval "$(sed -n '/^fRemoveLaidDown()/,/^}/p' "${repoDir}/install.bash")"
(
	udir="${tmpDir}/uninst"
	mkdir -p "${udir}/code" "${udir}/scripts" "${udir}/man" "${udir}/completions"
	printf 'x\n' > "${udir}/code/lib.rs"
	printf 'x\n' > "${udir}/scripts/shcl.bash"
	printf 'x\n' > "${udir}/man/shcl.1"
	printf 'x\n' > "${udir}/completions/shcl.bash"
	export SHCL_TEST_LOCKED="${udir}/code"
	cat > "${tmpDir}/asroot" <<-'EOS'
		#!/bin/bash
		chmod 700 "${SHCL_TEST_LOCKED}"
		"$@"; rc=$?
		[[ -d "${SHCL_TEST_LOCKED}" ]] && chmod 000 "${SHCL_TEST_LOCKED}"
		exit "${rc}"
	EOS
	chmod 755 "${tmpDir}/asroot"
	chmod 000 "${udir}/code"
	# shellcheck disable=SC2034  ## the lifted function's own global
	asroot="${tmpDir}/asroot"
	fRemoveLaidDown "${udir}"
	chmod 700 "${udir}/code" 2>/dev/null || true
	for d in code scripts man completions; do
		[[ -e "${udir}/${d}" ]] && fBad "install.bash uninstall left ${d} behind"
	done
	rmdir "${udir}" 2>/dev/null || fBad "install.bash uninstall did not empty the install directory"
	exit "${nBad}"
) || nBad=$((nBad + 1))

##	20260901b item 36: only a real file at the bin path was refused, so a
##	symlink to a cargo-built or hand-built copy was replaced with nothing said.
eval "$(sed -n '/^fLinkOwner()/,/^}/p' "${repoDir}/install.bash")"
(
	odir="${tmpDir}/owner"; mkdir -p "${odir}/dest" "${odir}/bin" "${odir}/other"
	printf 'x\n' > "${odir}/dest/shcl"; printf 'x\n' > "${odir}/other/shcl"
	[[ "$(fLinkOwner "${odir}/bin/shcl" "${odir}/dest")" == "free" ]] \
		|| fBad "install.bash refused a bin path with nothing at it"
	ln -s "${odir}/dest/shcl" "${odir}/bin/shcl"
	[[ "$(fLinkOwner "${odir}/bin/shcl" "${odir}/dest")" == "ours" ]] \
		|| fBad "install.bash refused its own link"
	ln -sfn "${odir}/other/shcl" "${odir}/bin/shcl"
	[[ "$(fLinkOwner "${odir}/bin/shcl" "${odir}/dest")" == "elsewhere ${odir}/other/shcl" ]] \
		|| fBad "install.bash would replace a symlink pointing somewhere else"
	rm -f "${odir}/bin/shcl"; printf 'x\n' > "${odir}/bin/shcl"
	[[ "$(fLinkOwner "${odir}/bin/shcl" "${odir}/dest")" == "file" ]] \
		|| fBad "install.bash would replace a real file at the bin path"
	exit "${nBad}"
) || nBad=$((nBad + 1))

##	20260901b item 37: the receipt ran the link the installer had just written,
##	so another shcl earlier on PATH was invisible and the next one the user
##	typed was someone else's.
eval "$(sed -n '/^fShadowedBy()/,/^}/p' "${repoDir}/install.bash")"
(
	sdir="${tmpDir}/shadow"; mkdir -p "${sdir}/ours" "${sdir}/theirs"
	printf '#!/bin/sh\necho ours\n' > "${sdir}/ours/shcl"; chmod 755 "${sdir}/ours/shcl"
	printf '#!/bin/sh\necho theirs\n' > "${sdir}/theirs/shcl"; chmod 755 "${sdir}/theirs/shcl"
	PATH="${sdir}/ours:${PATH}" fShadowedBy "${sdir}/ours/shcl" >/dev/null \
		&& fBad "install.bash called its own copy a shadow"
	out="$(PATH="${sdir}/theirs:${sdir}/ours:${PATH}" fShadowedBy "${sdir}/ours/shcl" || true)"
	[[ "${out}" == "${sdir}/theirs/shcl" ]] \
		|| fBad "install.bash did not see the copy shadowing it: ${out@Q}"
	PATH="${sdir}/nowhere:/nonexistent" fShadowedBy "${sdir}/ours/shcl" >/dev/null \
		&& fBad "install.bash reported a shadow where there is no shcl at all"
	exit "${nBad}"
) || nBad=$((nBad + 1))
grep -q 'Get-Command shcl -ErrorAction SilentlyContinue' "${repoDir}/install.ps1" \
	|| fBad "install.ps1 never asks what shcl resolves to on PATH"

##	20260901b item 39: a read-only HOME got through both downloads and then
##	failed on a raw mkdir error. The destinations are probed first, and the
##	probe has to walk up to whatever exists.
eval "$(sed -n '/^fNearestExisting()/,/^}/p' "${repoDir}/install.bash")"
(
	wdir="${tmpDir}/writable"; mkdir -p "${wdir}/home"
	[[ "$(fNearestExisting "${wdir}/home/.local/share/shcl")" == "${wdir}/home" ]] \
		|| fBad "install.bash did not walk up to the nearest existing directory"
	[[ "$(fNearestExisting "${wdir}/home")" == "${wdir}/home" ]] \
		|| fBad "install.bash did not accept a directory that is already there"
	chmod 500 "${wdir}/home"
	near="$(fNearestExisting "${wdir}/home/.local/share/shcl")"
	[[ -w "${near}" ]] && fBad "install.bash would have downloaded into a read-only home"
	chmod 700 "${wdir}/home"
	exit "${nBad}"
) || nBad=$((nBad + 1))
# shellcheck disable=SC2016  ## install.bash's own $variable, matched literally
downloadLine="$( { grep -n 'downloading \${asset}' "${repoDir}/install.bash" || true; } | head -n1 | cut -d: -f1)"
probeLine="$( { grep -n 'is not writable' "${repoDir}/install.bash" || true; } | head -n1 | cut -d: -f1)"
if [[ -z "${downloadLine}" || -z "${probeLine}" ]] || ((probeLine >= downloadLine)); then
	fBad "install.bash downloads before it knows the destination can be written"
fi

##	20260901b item 40: a fresh clone was left on main while contributing.md says
##	to branch from dev. The lifted function decides; an existing checkout or an
##	edited tree is left where it is.
eval "$(sed -n '/^fStartOnDev()/,/^}/p' "${repoDir}/install-dev.bash")"
(
	gdir="${tmpDir}/devclone"; mkdir -p "${gdir}/origin"
	git -C "${gdir}/origin" init -q --initial-branch=main
	git -C "${gdir}/origin" -c user.email=t@t -c user.name=t commit -q --allow-empty -m first
	git -C "${gdir}/origin" branch dev
	git clone -q --no-hardlinks "${gdir}/origin" "${gdir}/a" 2>/dev/null
	fStartOnDev "${gdir}/a"
	[[ "$(git -C "${gdir}/a" branch --show-current)" == "dev" ]] \
		|| fBad "install-dev.bash left a fresh clone on main"
	git clone -q --no-hardlinks "${gdir}/origin" "${gdir}/b" 2>/dev/null
	printf 'x\n' > "${gdir}/b/edited"
	git -C "${gdir}/b" add edited
	fStartOnDev "${gdir}/b"
	[[ "$(git -C "${gdir}/b" branch --show-current)" == "main" ]] \
		|| fBad "install-dev.bash moved a clone with work already in it"
	mkdir -p "${gdir}/mainonly"
	git -C "${gdir}/mainonly" init -q --initial-branch=main
	git -C "${gdir}/mainonly" -c user.email=t@t -c user.name=t commit -q --allow-empty -m first
	git clone -q --no-hardlinks "${gdir}/mainonly" "${gdir}/c" 2>/dev/null
	fStartOnDev "${gdir}/c"
	[[ "$(git -C "${gdir}/c" branch --show-current)" == "main" ]] \
		|| fBad "install-dev.bash moved a clone of a repo with no dev branch"
	exit "${nBad}"
) || nBad=$((nBad + 1))

##	20260901b item 41: an unauthenticated 403 is the API's rate limit and both
##	installers called it "none published yet, or network down". The status
##	itself comes from curl; what is checked here is what each status is called.
eval "$(sed -n '/^fApiFailure()/,/^}/p' "${repoDir}/install.bash")"
[[ "$(fApiFailure 403 stable)" == *"rate limit"* ]] || fBad "install.bash does not call a 403 a rate limit"
[[ "$(fApiFailure 429 stable)" == *"rate limit"* ]] || fBad "install.bash does not call a 429 a rate limit"
[[ "$(fApiFailure 401 stable)" == *"GITHUB_TOKEN"* ]] || fBad "install.bash does not blame the token for a 401"
[[ "$(fApiFailure 404 stable)" == *"none published yet"* ]] || fBad "install.bash calls a 404 a rate limit"
[[ "$(fApiFailure 000 dev)" == *"dev release"* ]] || fBad "install.bash does not name the channel when it cannot reach the API"
grep -q 'rate limit' "${repoDir}/install.bash" || fBad "install.bash does not name a rate limit"
grep -q 'rate limit' "${repoDir}/install.ps1"  || fBad "install.ps1 does not name a rate limit"
grep -q 'GITHUB_TOKEN' "${repoDir}/install.bash" || fBad "install.bash ignores GITHUB_TOKEN"
grep -q 'GITHUB_TOKEN' "${repoDir}/install.ps1"  || fBad "install.ps1 ignores GITHUB_TOKEN"

##	20260901b item 46: the PowerShell wrapper's header ran one line out to 126
##	columns where its bash twin wraps. Comment lines only - the code in both
##	carries a couple of long ones on purpose.
for wrapper in source/bash/shcl.bash source/powershell/shcl.ps1; do
	long="$(awk '/^#{2}/ && length > 100 { print NR": "length }' "${repoDir}/${wrapper}")"
	[[ -z "${long}" ]] || fBad "${wrapper}: header comment runs past 100 columns at ${long//$'\n'/, }"
done

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

##	20260901b item 17: sign-release.bash wrote the signature first and checked
##	the key after, so a run with the wrong key failed and left a .sig behind
##	that looked finished; and nothing checked the sums file's name against the
##	version, or its entries against the files. A throwaway key stands in for
##	the wrong one, and every refusal must leave no .sig.
if fHave openssl; then
	sver="$(sed -n 's/^version *= *"\(.*\)".*/\1/p' "${repoDir}/source/rust/Cargo.toml" | head -1)"
	openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out "${tmpDir}/wrong.pem" 2>/dev/null
	fSignRun(){   ## fSignRun DIR: run the signer on DIR with the throwaway key; stderr in signOut
		signOut="$(bash "${repoDir}/cicd/utility/sign-release.bash" --key "${tmpDir}/wrong.pem" --dir "$1" --no-tag-check 2>&1 || true)"
	}
	## Right name, right sums, wrong key: refused on the key, nothing written.
	mkdir -p "${tmpDir}/sign1"; printf 'bin\n' > "${tmpDir}/sign1/shcl-${sver}-linux-x86_64"
	(cd "${tmpDir}/sign1" && sha256sum "shcl-${sver}-linux-x86_64" > "shcl-${sver}-sha256sums.txt")
	fSignRun "${tmpDir}/sign1"
	[[ "${signOut}" == *"not this key"* ]] || fBad "sign-release.bash did not refuse the wrong key: ${signOut@Q}"
	[[ ! -e "${tmpDir}/sign1/shcl-${sver}-sha256sums.txt.sig" ]] || fBad "sign-release.bash left a .sig behind after refusing the key"
	## Stale sums: an entry that no longer matches its file.
	mkdir -p "${tmpDir}/sign2"; cp "${tmpDir}/sign1/"* "${tmpDir}/sign2/"; printf 'rebuilt\n' > "${tmpDir}/sign2/shcl-${sver}-linux-x86_64"
	fSignRun "${tmpDir}/sign2"
	[[ "${signOut}" == *"does not match the files"* ]] || fBad "sign-release.bash signed a stale sums file: ${signOut@Q}"
	[[ ! -e "${tmpDir}/sign2/shcl-${sver}-sha256sums.txt.sig" ]] || fBad "sign-release.bash left a .sig behind after a stale sums file"
	## A sums file from another version.
	mkdir -p "${tmpDir}/sign3"; printf 'bin\n' > "${tmpDir}/sign3/shcl-0.0.1-linux-x86_64"
	(cd "${tmpDir}/sign3" && sha256sum "shcl-0.0.1-linux-x86_64" > "shcl-0.0.1-sha256sums.txt")
	fSignRun "${tmpDir}/sign3"
	[[ "${signOut}" == *"is not the sums file for ${sver}"* ]] || fBad "sign-release.bash signed a sums file for another version: ${signOut@Q}"
	[[ ! -e "${tmpDir}/sign3/shcl-0.0.1-sha256sums.txt.sig" ]] || fBad "sign-release.bash left a .sig behind after a misnamed sums file"
else
	echo "shell-regress: openssl not installed - signing rows skipped"
fi

##	20260901b item 20: flame-report.py took any file with a sample count and one
##	frame for a whole flamegraph, so a profile cut off mid-write reported a
##	fraction of itself at exit 0 and recorded the marker; a row height other
##	than the constant it assumed double-counted; a non-UTF-8 file was a
##	traceback. Three frames are a whole graph; a graph missing the frame
##	between a leaf and the root, one cut off before its closing tag, and
##	random bytes must each skip at 2.
fFlame(){   ## fFlame FILE STEP FRAMES...: a flamegraph of 100 samples with rows STEP apart
	local file="$1" step="$2"; shift 2
	{
		printf '<svg total_samples="100">\n'
		printf '<title>all (100 samples, 100%%)</title><rect x="0" y="%s" fg:x="0" fg:w="100"/>\n' "$((step * 3))"
		for fr in "$@"; do
			IFS='|' read -r fname fx fy fw <<<"${fr}"
			printf '<title>%s (%s samples)</title><rect x="0" y="%s" fg:x="%s" fg:w="%s"/>\n' "${fname}" "${fw}" "$((step * fy))" "${fx}" "${fw}"
		done
		printf '</svg>\n'
	} > "${file}"
}
mkdir -p "${tmpDir}/flame"
fFlame "${tmpDir}/flame/flame_20260101-000000_whole.svg" 16 'shcl::Parser::parse|0|2|60' 'shcl::Document::to_canonical|60|2|40' 'shcl::scan_path|0|1|10'
fFlame "${tmpDir}/flame/flame_20260101-000000_tall.svg"  24 'shcl::Parser::parse|0|2|60' 'shcl::Document::to_canonical|60|2|40' 'shcl::scan_path|0|1|10'
fFlame "${tmpDir}/flame/flame_20260101-000000_gap.svg"   16 'shcl::Document::to_canonical|60|2|40' 'shcl::scan_path|0|1|10'
head -c 200 "${tmpDir}/flame/flame_20260101-000000_whole.svg" > "${tmpDir}/flame/flame_20260101-000000_cut.svg"
head -c 1000 /dev/urandom > "${tmpDir}/flame/flame_20260101-000000_junk.svg"
fFlameRun(){ flameRc=0; flameOut="$(python3 "${repoDir}/cicd/utility/flame-report.py" --file "$1" 2>&1)" || flameRc=$?; }
fFlameRun "${tmpDir}/flame/flame_20260101-000000_whole.svg"
[[ "${flameRc}" == 0 && "${flameOut}" == *"parse (tokenize/merge/diags) .:  60.0%"* ]] || fBad "flame-report.py misread a whole graph (rc ${flameRc}): ${flameOut@Q}"
fFlameRun "${tmpDir}/flame/flame_20260101-000000_tall.svg"
[[ "${flameRc}" == 0 && "${flameOut}" == *"parse (tokenize/merge/diags) .:  60.0%"* && "${flameOut}" == *"other ........................:   0.0%"* ]] || fBad "flame-report.py misread a graph with a different row height (rc ${flameRc}): ${flameOut@Q}"
for bad in gap cut junk; do
	fFlameRun "${tmpDir}/flame/flame_20260101-000000_${bad}.svg"
	[[ "${flameRc}" == 2 ]] || fBad "flame-report.py accepted a ${bad} graph (rc ${flameRc}): ${flameOut@Q}"
	[[ "${flameOut}" != *Traceback* ]] || fBad "flame-report.py tracebacked on a ${bad} graph"
done
##	20260901b item 43: the sampler drops a sample whose leaf is inside libc
##	rather than truncating it, so a third to half the profiled time never
##	reaches the graph and every percentage in the report is a share of what
##	survived. The profiler writes the two counts beside the SVG; the report has
##	to say them, and has to stay quiet where an older graph has no such file.
printf '640 1600\n' > "${tmpDir}/flame/flame_20260101-000000_whole.svg.samples"
fFlameRun "${tmpDir}/flame/flame_20260101-000000_whole.svg"
[[ "${flameOut}" == *"640 of about 1600 samples reached the graph (40%)"* ]] \
	|| fBad "flame-report.py does not say how much of the profile the graph is missing: ${flameOut@Q}"
rm -f "${tmpDir}/flame/flame_20260101-000000_whole.svg.samples"
fFlameRun "${tmpDir}/flame/flame_20260101-000000_whole.svg"
[[ "${flameOut}" != *"reached the graph"* ]] \
	|| fBad "flame-report.py invented a sample count for a graph that carries none"
grep -q '{}.samples' "${repoDir}/source/rust/src/main.rs" \
	|| fBad "the profiler does not record how many samples reached the graph"

##	20260901b item 21: lint-report.bash counted the `-D warnings` in the clippy
##	command line the pre-push gate's nested run echoes as a warning, so every
##	run that pushed to dev read as one finding. A real clippy warning and a
##	cppcheck one still count; the two echoed command lines do not.
printf 'Lint ...........: cargo clippy --all-targets -- -D warnings\nLint ...........: cppcheck --enable=warning,portability src.c\nOK: lint\n' > "${tmpDir}/run_20260101-000000.log"
lintOut="$(bash "${repoDir}/cicd/utility/lint-report.bash" --file "${tmpDir}/run_20260101-000000.log" 2>&1 || true)"
[[ "${lintOut}" == "CLEAN "* ]] || fBad "lint-report.bash counted an echoed command line as a warning: ${lintOut@Q}"
printf 'warning: unused variable: x\n --> src/main.rs:1:1\nsrc.c:12:3: warning: uninitialized variable [uninitvar]\n' >> "${tmpDir}/run_20260101-000000.log"
lintOut="$(bash "${repoDir}/cicd/utility/lint-report.bash" --file "${tmpDir}/run_20260101-000000.log" 2>&1 || true)"
[[ "${lintOut}" == "FLAG "*"(2 warning line(s))"* ]] || fBad "lint-report.bash missed a real warning: ${lintOut@Q}"

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

##	20260901b item 34: the same list with no whitespace. The API is documented
##	as pretty-printing, and a proxy that does not left the picker reading one
##	field per release, so it answered nothing and the run said no release was
##	published at all.
tr -d ' \t\n' < "${tmpDir}/rel.json" > "${tmpDir}/rel-compact.json"
out="$(fPickTag stable "${tmpDir}/rel-compact.json")"
[[ "${out}" == "v2.0.0" ]] || fBad "install.bash stable channel on compact json picked ${out@Q}, want v2.0.0"
out="$(fPickTag dev "${tmpDir}/rel-compact.json")"
[[ "${out}" == "v2.1.0-alpha.10" ]] || fBad "install.bash dev channel on compact json picked ${out@Q}, want v2.1.0-alpha.10"

if fHave pwsh; then
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

##	20260901b item 34, both windows-only and neither reachable from a linux
##	pwsh: a 32-bit host reads Program Files (x86) out of ProgramFiles, and
##	Windows PowerShell 5.1 turns a native command's stderr into error records
##	under `2>&1`, which the script's own Stop preference then throws on. The
##	first is an expression a linux pwsh can evaluate; the second is source
##	order, the way the smoke-run placement below is.
# shellcheck disable=SC2016  ## PowerShell's own $variable, matched literally
pfLine="$( { grep -F 'programFiles = if ($env:ProgramW6432)' "${repoDir}/install.ps1" || true; } | head -n1)"
if [[ -z "${pfLine}" ]]; then
	fBad "install.ps1 reads ProgramFiles without asking for the 64-bit one"
elif fHave pwsh; then
	#  shellcheck disable=2016  ## PowerShell's own $variables.
	{
		echo '$env:ProgramFiles = "C:\Program Files (x86)"'
		echo '$env:ProgramW6432 = "C:\Program Files"'
		printf '%s\n' "${pfLine}"
		echo 'Write-Output ("wow=" + $programFiles)'
		echo 'Remove-Item Env:ProgramW6432'
		printf '%s\n' "${pfLine}"
		echo 'Write-Output ("plain=" + $programFiles)'
	} > "${tmpDir}/pf.ps1"
	out="$(pwsh -NoProfile -File "${tmpDir}/pf.ps1" 2>&1 || true)"
	[[ "${out}" == *'wow=C:\Program Files'* && "${out}" != *'wow=C:\Program Files (x86)'* ]] \
		|| fBad "install.ps1 puts a 64-bit install under the x86 program files: ${out@Q}"
	[[ "${out}" == *'plain=C:\Program Files (x86)'* ]] \
		|| fBad "install.ps1 ignores ProgramFiles where there is no 64-bit one: ${out@Q}"
fi
##	20260901b item 38: the setup .exe writes the same directory and registers
##	itself with Add/Remove Programs, so deleting its files from here left that
##	entry pointing at nothing. Windows-only, so the check is source order.
setupTest="$( { grep -n "uninstall.exe'" "${repoDir}/install.ps1" || true; } | head -n1 | cut -d: -f1)"
setupWipe="$( { grep -n "Remove-Item -Force -LiteralPath (Join-Path \$dest 'shcl.exe')" "${repoDir}/install.ps1" || true; } | head -n1 | cut -d: -f1)"
if [[ -z "${setupTest}" || -z "${setupWipe}" ]] || ((setupTest >= setupWipe)); then
	fBad "install.ps1 removes a setup install's files without deferring to its uninstaller"
fi
grep -q 'Add/Remove Programs entry still shows the version it installed' "${repoDir}/install.ps1" \
	|| fBad "install.ps1 does not say a setup install's Add/Remove entry goes stale when it writes over one"

eapLine="$( { grep -n "ErrorActionPreference = 'Continue'" "${repoDir}/install.ps1" || true; } | head -n1 | cut -d: -f1)"
smokeRun="$( { grep -n "shcl.exe') version 2>&1" "${repoDir}/install.ps1" || true; } | head -n1 | cut -d: -f1)"
if [[ -z "${eapLine}" || -z "${smokeRun}" ]] || ((eapLine >= smokeRun)); then
	fBad "install.ps1 runs the smoke test under its own Stop preference, which 5.1 throws on"
fi
grep -q "finally { \$ErrorActionPreference = \$smokeEap }" "${repoDir}/install.ps1" \
	|| fBad "install.ps1 does not put the caller's error preference back after the smoke run"

##	20260901b item 35: the rpm listed the payload's subdirectories and not their
##	parent, so removing the package left /usr/share/shcl behind. The package
##	read-back lives in package.bash, which only runs at release time; building a
##	stub package here gives it something to fail on every run.
## Not through fHave: the package read-back's own home is package.bash, which
## runs at release time on a box that has these. This is the same check with
## something to fail on every run, and the hosted gate installs no packager.
if command -v nfpm >/dev/null 2>&1 && command -v dpkg-deb >/dev/null 2>&1 && command -v rpm >/dev/null 2>&1; then
	pDir="${tmpDir}/nfpm"
	mkdir -p "${pDir}/payload/code" "${pDir}/payload/scripts" "${pDir}/payload/man" "${pDir}/payload/doc" "${pDir}/payload/completions"
	printf 'x\n' > "${pDir}/payload/code/lib.rs"
	printf 'x\n' > "${pDir}/payload/scripts/shcl.bash"
	printf 'x\n' > "${pDir}/payload/doc/copyright"
	printf 'x\n' > "${pDir}/payload/completions/shcl.bash"
	printf 'x\n' > "${pDir}/payload/completions/_shcl"
	printf 'x\n' | gzip -9nc > "${pDir}/payload/man/shcl.1.gz"
	printf 'x\n' | gzip -9nc > "${pDir}/payload/doc/changelog.gz"
	printf 'x\n' > "${pDir}/shcl"
	sed -e "s|\${SHCL_VERSION}|9.9.9|g" -e "s|\${SHCL_ARCH}|amd64|g" \
	    -e "s|\${SHCL_BIN}|${pDir}/shcl|g" -e "s|\${SHCL_PAYLOAD}|${pDir}/payload|g" \
	    -e "s|\${SHCL_GLIBC}|2.34|g" -e "s|\${SHCL_DEB_LIBGCC}||g" -e "s|\${SHCL_RPM_LIBGCC}||g" \
	    "${repoDir}/cicd/packaging/nfpm.yaml" > "${pDir}/nfpm.yaml"
	if nfpm package -f "${pDir}/nfpm.yaml" -p deb -t "${pDir}/p.deb" >/dev/null 2>&1 \
	   && nfpm package -f "${pDir}/nfpm.yaml" -p rpm -t "${pDir}/p.rpm" >/dev/null 2>&1; then
		##	The shipped read-back, run on the stub packages.
		eval "$(sed -n '/^fCheckDeps()/,/^}/p' "${repoDir}/cicd/utility/package.bash")"
		# shellcheck disable=SC2329  ## called from the lifted fCheckDeps
		( fDie(){ echo "shell-regress: $*" >&2; exit 1; }; fCheckDeps "${pDir}/p" 2.34 "" ) \
			|| fBad "the packages do not read back the way package.bash requires"
	else
		fBad "nfpm could not build a package from cicd/packaging/nfpm.yaml"
	fi
fi

##	20260830b item 8: a prerelease version reached NSIS's four-integer version
##	field verbatim, and makensis rejected it under errexit, so the release stage
##	died on the first prerelease cut. A fake .exe is enough - the setup never
##	runs, it only has to build.
if fHave makensis; then
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
##	20260904 item 23: check-c-compilers.bash reported OK with one compiler and
##	never read the gate flag, so a runner that lost its compilers would have
##	kept passing. Under the flag a thin sweep has to fail before it builds.
##	The PATH keeps every tool but the compilers, with one gcc put back.
mkdir -p "${tmpDir}/onecc"
for f in /usr/bin/* /bin/*; do
	if [[ -x "${f}" ]]; then ln -sf "${f}" "${tmpDir}/onecc/" 2>/dev/null || true; fi
done
rm -f "${tmpDir}"/onecc/gcc-* "${tmpDir}/onecc/clang" "${tmpDir}/onecc/cc" "${tmpDir}/onecc/gcc"
ln -sf "$(command -v gcc || command -v cc)" "${tmpDir}/onecc/gcc"
out="$(PATH="${tmpDir}/onecc" SHCL_GATE_STRICT=1 "${BASH}" "${repoDir}/cicd/utility/check-c-compilers.bash" "${repoDir}" 2>&1 || true)"
[[ "${out}" == *"needs two versioned gccs and clang"* ]] || fBad "check-c-compilers passed the gate with one compiler: ${out@Q}"

##	20260904 item 24: the publish script ran `git config user.name` as a bare
##	statement under set -e, so a repository with no identity died in the trap
##	before doing anything; and its ssh-host probe assigned from a pipeline
##	whose failure killed the script ahead of the fallback on the next line.
##	Both lines have to carry their fallback.
pub="${repoDir}/cicd/utility/n8git_backup-and-publish"
[[ "$(grep -cE 'git config user\.(name|email)[^|]*\|\|' "${pub}" || true)" == 2 ]] || fBad "n8git_backup-and-publish reads the git identity without a fallback"
grep -qE 'sshHost="\$\(git remote get-url origin[^)]*\|\| true\)"' "${pub}" || fBad "n8git_backup-and-publish assigns sshHost without a fallback"

##	20260904 item 25: largedoc's memory ceilings were strictly per input MiB, so
##	at one MiB the runtime's own footprint failed a healthy tree. Run the gate
##	small, which is exactly the size a developer shrinks it to.
if [[ -x "${cli}" && -x "${repoDir}/source/go/shcl" && -x "${repoDir}/source/c/shcl" ]]; then
	out="$(bash "${repoDir}/cicd/utility/largedoc.bash" --mib 1 "rust|${cli}" "go|${repoDir}/source/go/shcl" "python|${repoDir}/source/python/cmd/shcl/main.py" "c|${repoDir}/source/c/shcl" 2>&1 || true)"
	[[ "${out}" == *"OK"* && "${out}" != *"TOO BIG"* ]] || fBad "largedoc --mib 1 fails on a healthy tree: $(tail -n 3 <<<"${out}")"
fi

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

##	Every tracked shell script, whether or not it is named `*.bash`: the
##	pre-push hook and the publish script carry no extension, and both are
##	`set -e` scripts the scans below are about. Untracked-but-not-ignored files
##	are in, so a script written and not yet added is still scanned; build output
##	is out, because it is ignored. Written once and reused.
fShellFiles(){
	local f
	while IFS= read -r f; do
		case "${f}" in
			*.bash) printf '%s\n' "${repoDir}/${f}" ;;
			*)      [[ -f "${repoDir}/${f}" ]] && head -1 "${repoDir}/${f}" | grep -qE '^#!.*\b(bash|sh)\b' \
			            && printf '%s\n' "${repoDir}/${f}" ;;
		esac
	done < <(git -C "${repoDir}" ls-files --cached --others --exclude-standard | sort -u)
}

##	The two static scans, as functions so the self-test below can run them over a
##	file holding every spelling they are meant to catch. Both were written for
##	one spelling each and missed the ordinary ones.
##
##	`\t` in a grep -E pattern: POSIX ERE has no such escape, so the pattern
##	matches nothing under the grep a script gets, while matching fine under the
##	interactive one on this box. Either quote style, either option spelling.
fScanTabEre(){
	grep -nE "grep [^|;]*(-[A-Za-z]*E[A-Za-z]*|--extended-regexp)[^|;]*['\"][^'\"]*\\\\t" "$1" || true
}

##	A `grep` inside an assigned command substitution with no `|| true`: when it
##	matches nothing the assignment fails, errexit kills the script, and the
##	check that would have printed the reason never runs. Every way to spell an
##	assignment counts - a keyword prefix, an array element, `+=`, backticks.
##	Line continuations are joined first, so a substitution that ends in
##	`|| true` several lines down is read as guarded.
fScanUnguardedGrep(){
	sed -e :a -e '/\\$/N; s/\\\n//; ta' "$1" \
		| grep -nE '^[[:space:]]*(local|declare|typeset|export|readonly)?[[:space:]]*[A-Za-z_][A-Za-z0-9_]*(\[[^]]*\])?\+?=.*(\$\(|`).*\bgrep\b' \
		| grep -vE '\|\|[[:space:]]*(true|:)' || true
}

##	The self-test: one file carrying every spelling, so a scan that stops seeing
##	one of them fails here rather than going quiet over the repo.
##	The strict switch itself: under the gate a missing tool has to be a failure,
##	or a runner that loses one reports OK forever. Locally it stays a skip.
{
	strictBad=0
	( SHCL_GATE_STRICT=1; fBad(){ exit 7 ;}; fHave definitely-not-a-tool ) 2>/dev/null || strictBad=$?
	((strictBad == 7)) || fBad "fHave did not fail on a missing tool under the gate"
	laxBad=0
	( unset SHCL_GATE_STRICT; fBad(){ exit 7 ;}; fHave definitely-not-a-tool ) 2>/dev/null || laxBad=$?
	((laxBad == 1)) || fBad "fHave did not skip a missing tool outside the gate (exit ${laxBad})"
}

##	The escape is assembled rather than written, so the bait for the second scan
##	does not trip that scan when it sweeps this file.
bs=$'\\'
#  shellcheck disable=2016  ## the bait is the literal text of the shapes being scanned for.
{
	printf '%s\n' '#!/usr/bin/env bash' 'set -Eeuo pipefail' \
		'local x="$(grep -c a f)"' 'declare y="$(grep -c a f)"' 'export Z="$(grep -c a f)"' \
		'readonly W="$(grep -c a f)"' 'arr[0]="$(grep -c a f)"' 'acc+="$(grep -c a f)"' \
		'bt="`grep -c a f`"' 'ok="$(grep -c a f || true)"' \
		"grep --extended-regexp \"x${bs}ty\" f" "grep -E 'a${bs}tb' f" 'grep -E "[[:space:]]" f'
} > "${tmpDir}/scanbait.bash"
n="$(fScanUnguardedGrep "${tmpDir}/scanbait.bash" | wc -l)"
((n == 7)) || fBad "the unguarded-grep scan found ${n} of 7 spellings"
n="$(fScanTabEre "${tmpDir}/scanbait.bash" | wc -l)"
((n == 2)) || fBad "the backslash-t scan found ${n} of 2 spellings"

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
done < <(fShellFiles)

##	`\t` in a grep -E pattern. POSIX ERE has no such escape, so the pattern
##	matches nothing under the grep a script gets, while matching fine under the
##	interactive one on this box - a check that looks like it works and asserts
##	nothing. Twice in one round. Use a literal tab or `[[:space:]]`.
while IFS= read -r f; do
	hits="$(fScanTabEre "${f}")"
	if [[ -n "${hits}" ]]; then
		while IFS= read -r h; do fBad "${f#"${repoDir}/"}: backslash-t in a grep -E pattern, which POSIX ERE does not read as a tab: ${h}"; done <<<"${hits}"
	fi
done < <(fShellFiles)

##	The static half. Line continuations are joined first, so a substitution that
##	ends in `|| true` several lines down is read as guarded.
while IFS= read -r f; do
	grep -qE '^set -[A-Za-z]*e' "${f}" || continue
	hits="$(fScanUnguardedGrep "${f}")"
	if [[ -n "${hits}" ]]; then
		while IFS= read -r h; do fBad "${f#"${repoDir}/"}: unguarded grep in an assigned substitution: ${h}"; done <<<"${hits}"
	fi
done < <(fShellFiles)

##	20260902 item 18: the two corpus replays split a reads.tsv row with
##	`IFS=$'\t' read`, which drops a leading or doubled tab because tab is IFS
##	whitespace whatever IFS is set to - so the top-level `children` row arrived
##	as a type nothing had an arm for and ran nothing. Lifted out of
##	crosscheck.bash by name so the check cannot drift from the shipped text.
eval "$(sed -n '/^fSplitTabs()/,/^}/p' "${repoDir}/cicd/utility/crosscheck.bash")"
cols=()
fSplitTabs "$(printf '\tchildren\tdb|web\t-')"
[[ "${#cols[@]}" == 4 && -z "${cols[0]}" && "${cols[1]}" == "children" ]] \
	|| fBad "fSplitTabs dropped a leading empty field: ${cols[*]@Q}"
fSplitTabs "$(printf 'nope\tchildren\t\t-')"
[[ "${#cols[@]}" == 4 && "${cols[2]}" == "" && "${cols[3]}" == "-" ]] \
	|| fBad "fSplitTabs dropped a middle empty field: ${cols[*]@Q}"
fSplitTabs "one"
[[ "${#cols[@]}" == 1 && "${cols[0]}" == "one" ]] || fBad "fSplitTabs mangled a single field: ${cols[*]@Q}"

##	20260902 item 17: the crosscheck's float-spelling dimension built its powers
##	of two as `2 ^ e`, which gawk computes as 1/(2^1074) for a subnormal - so 52
##	of its rows were the value zero and tested nothing - and drew its "fixed"
##	random set from srand()/rand(), whose sequence differs between awks. The
##	shipped generator is lifted out of crosscheck.bash by name so the check
##	cannot drift from it.
gen="$(sed -n "/^awk 'BEGIN{/,/^}' > /p" "${repoDir}/cicd/utility/crosscheck.bash" | sed "1s/^awk '//; \$s/}' > .*/}/")"
[[ -n "${gen}" ]] || fBad "could not lift the float generator out of crosscheck.bash"
printf '%s
' "${gen}" > "${tmpDir}/floats.awk"
first=""
for a in gawk mawk "busybox awk" awk; do
	read -r -a acmd <<<"${a}"
	command -v "${acmd[0]}" > /dev/null 2>&1 || continue
	"${acmd[@]}" -f "${tmpDir}/floats.awk" > "${tmpDir}/floats.out" 2>/dev/null || { fBad "float generator failed under ${a}"; continue; }
	zeros="$(grep -cE $'^float	p[0-9]+	-?0$' "${tmpDir}/floats.out" || true)"
	((zeros == 0)) || fBad "float generator wrote ${zeros} zero-valued power row(s) under ${a}"
	sum="$(sha256sum < "${tmpDir}/floats.out")"
	if [[ -z "${first}" ]]; then first="${sum}"
	elif [[ "${sum}" != "${first}" ]]; then fBad "float generator differs under ${a}"; fi
done
[[ -n "${first}" ]] || fBad "no awk found to run the float generator"

if ((nBad)); then
	echo "shell-regress: ${nBad} check(s) failed" >&2
	exit 1
fi
echo "shell-regress: OK: wrappers, one-liner scope, packaging, installers, comparison worker, and no unguarded greps, failed-test loop bodies or tab escapes in an ERE"

##	History:
##		2026-08-30  Created, pinning the wrapper and installer defects from the
##		            20260829 and 20260830 rounds.
