#!/usr/bin/env bash

##	Purpose:
##		Sign a release's sha256sums file with the offline release key, so the
##		installers have something to verify against. The sums file is the trust
##		root for every install path: replacing a release asset is only useful to
##		an attacker who can also replace the sums, and this signature is what
##		makes that second step infeasible.
##
##		The private key never lives in this repo, on the build box, or in CI - it
##		is offline, and this script is run by hand at release time. A key sitting
##		in CI would be reachable by exactly the compromise this defends against.
##
##		Signs, then verifies what it just wrote, then checks that the key used
##		is the one the shipped installers actually trust - a signature made with
##		the wrong key verifies perfectly on its own and fails for every user, so
##		that last check is the one that matters.
##	Syntax:
##		sign-release.bash --key FILE [--dir DIR]
##		  --key FILE  private signing key (PEM). Prompts if passphrase-protected.
##		  --dir DIR   release artifact dir (default cicd/artifacts/release)
##	Exit: 0 signed and verified, 1 failure, 2 usage/missing input.
##	History: At bottom of script.

##	Copyright © 2026 Bubbles (ID: XଌฅრX۳ᛟԃლፀƅꓩหδლც)
##	Licensed under The MIT License (MIT). Full text at:
##		https://mit-license.org/
##	SPDX-License-Identifier: MIT

set -Eeuo pipefail

fDie(){ printf 'sign-release.bash: %s\n' "$1" >&2; exit "${2:-1}"; }

root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
key=""
dir="${root}/cicd/artifacts/release"

while (( $# )); do
	case "$1" in
		--key=*) key="${1#*=}" ;;
		--key)   shift; key="${1:-}" ;;
		--dir=*) dir="${1#*=}" ;;
		--dir)   shift; dir="${1:-}" ;;
		-h|--help) sed -n '3,24p' "$0"; exit 0 ;;
		*) fDie "unknown option: $1" 2 ;;
	esac
	shift
done

[[ -n "${key}" ]]  || fDie "need --key FILE" 2
[[ -r "${key}" ]]  || fDie "cannot read key: ${key}" 2
[[ -d "${dir}" ]]  || fDie "no such dir: ${dir}" 2
command -v openssl >/dev/null || fDie "need openssl"

## Exactly one sums file, or we would be signing an ambiguous trust root.
mapfile -t sums < <(find "${dir}" -maxdepth 1 -type f -name '*-sha256sums.txt' | sort)
(( ${#sums[@]} == 1 )) || fDie "expected exactly 1 *-sha256sums.txt in ${dir}, found ${#sums[@]}" 2
sumsfile="${sums[0]}"
sigfile="${sumsfile}.sig"

openssl dgst -sha256 -sign "${key}" -out "${sigfile}" "${sumsfile}" \
	|| fDie "signing failed"

## Verify what we just wrote, against the public half of the same key.
pub="$(mktemp)"; trap 'rm -f "${pub}"' EXIT
openssl pkey -in "${key}" -pubout -out "${pub}" 2>/dev/null || fDie "cannot derive the public key"
openssl dgst -sha256 -verify "${pub}" -signature "${sigfile}" "${sumsfile}" >/dev/null 2>&1 \
	|| fDie "wrote a signature that does not verify"

## The check that actually catches mistakes: is this the key the installers
## trust? Signing with the wrong key produces a perfectly valid signature that
## fails for every user, and nothing else here would notice.
inst="${root}/install.bash"
if [[ -r "${inst}" ]]; then
	instpub="$(mktemp)"; trap 'rm -f "${pub}" "${instpub}"' EXIT
	sed -n '/-----BEGIN PUBLIC KEY-----/,/-----END PUBLIC KEY-----/p' "${inst}" > "${instpub}"
	fFp(){ openssl pkey -pubin -in "$1" -outform DER -pubout 2>/dev/null | sha256sum | cut -d' ' -f1; }
	if [[ "$(fFp "${pub}")" != "$(fFp "${instpub}")" ]]; then
		fDie "signed with a key the installers do not trust - install.bash carries a different one"
	fi
fi

printf 'signed  %s\n' "${sumsfile##*/}"
printf 'wrote   %s\n' "${sigfile##*/}"
printf 'key fp  %s\n' "$(openssl pkey -in "${key}" -pubout 2>/dev/null | openssl pkey -pubin -outform DER -pubout 2>/dev/null | sha256sum | cut -d' ' -f1)"
printf 'attach both the sums file and its .sig to the release.\n'

##	History:
##		- 2026-07-28 JC: Written for the 1.0.0 cut, when the sums file became a
##		  signed trust root rather than something trusted for being on https.
