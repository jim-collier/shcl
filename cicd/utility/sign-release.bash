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
##		sign-release.bash --key FILE [--dir DIR] [--no-tag-check]
##		  --key FILE      private signing key (PEM). Prompts if passphrase-protected.
##		  --dir DIR       release artifact dir (default cicd/artifacts/release)
##		  --no-tag-check  sign even when HEAD does not carry the v<version> tag
##		                  (rehearsals only; a real cut is signed at the tag)
##	Exit: 0 signed and verified, 1 failure, 2 usage/missing input.
##	History: At bottom of script.

##	Copyright © 2026 Bubbles (ID: XଌฅრX۳ᛟԃლፀƅꓩหδლც)
##	Licensed under The MIT License (MIT). Full text at:
##		https://mit-license.org/
##	SPDX-License-Identifier: MIT

set -Eeuo pipefail

fDie(){ printf 'sign-release.bash: %s\n' "$1" >&2; exit "${2:-1}"; }

## Usage text lives here, not in a sed slice of "$0": run through a pipe or an
## odd interpreter, $0 is not this file and sed reads the wrong one.
fUsage(){
	cat <<'EOF'
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
##		sign-release.bash --key FILE [--dir DIR] [--no-tag-check]
##		  --key FILE      private signing key (PEM). Prompts if passphrase-protected.
##		  --dir DIR       release artifact dir (default cicd/artifacts/release)
##		  --no-tag-check  sign even when HEAD does not carry the v<version> tag
##		                  (rehearsals only; a real cut is signed at the tag)
##	Exit: 0 signed and verified, 1 failure, 2 usage/missing input.
##	History: At bottom of script.

EOF
}

## $0 fallback so `--help` works when piped (bash -s has no BASH_SOURCE, and
## set -u would trip on it before the options ever parse).
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]:-$0}")/../.." && pwd)"
key=""
dir="${root}/cicd/artifacts/release"
tagCheck=1

while (( $# )); do
	case "$1" in
		--key=*) key="${1#*=}" ;;
		--key)   (( $# >= 2 )) || fDie "missing value for --key (try --key=VALUE)" 2; shift; key="$1" ;;
		--dir=*) dir="${1#*=}" ;;
		--dir)   (( $# >= 2 )) || fDie "missing value for --dir (try --dir=VALUE)" 2; shift; dir="$1" ;;
		--no-tag-check) tagCheck=0 ;;
		-h|--help) fUsage; exit 0 ;;
		*) fDie "unknown option: $1" 2 ;;
	esac
	shift
done

[[ -n "${key}" ]]  || fDie "need --key FILE" 2
[[ -r "${key}" ]]  || fDie "cannot read key: ${key}" 2
[[ -d "${dir}" ]]  || fDie "no such dir: ${dir}" 2
command -v openssl >/dev/null || fDie "need openssl"

## The tag is what the installers and the release page name the assets by, and
## Cargo.toml is the version they were built from. A mistyped tag would sign a
## sums file whose entries no download URL can reach, so refuse unless HEAD
## carries exactly v<version>. `git tag --points-at` rather than `describe`:
## the cut puts two tags on the commit (v2.0.0 and source/go/v2.0.0).
if (( tagCheck )); then
	ver="$(sed -n 's/^version *= *"\(.*\)".*/\1/p' "${root}/source/rust/Cargo.toml" | head -1)"
	[[ -n "${ver}" ]] || fDie "cannot read the version from source/rust/Cargo.toml"
	headTags="$(git -C "${root}" tag --points-at HEAD 2>/dev/null || true)"
	if ! grep -qxF "v${ver}" <<<"${headTags}"; then
		fDie "HEAD is not tagged v${ver} (Cargo.toml says ${ver}); tag the cut first, or --no-tag-check for a rehearsal"
	fi
fi

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

## The checks that actually catch mistakes: is this the key every shipped copy
## trusts? Signing with the wrong key produces a perfectly valid signature that
## then fails for every user, and nothing else here would notice. Three copies
## have to agree - the published .pub, the PEM inlined in install.bash, and the
## raw modulus inlined in install.ps1 - so any one of them drifting is caught
## here rather than by somebody's failed install.
tmppub="$(mktemp)"; trap 'rm -f "${pub}" "${tmppub}"' EXIT
fFp(){ openssl pkey -pubin -in "$1" -outform DER -pubout 2>/dev/null | sha256sum | cut -d' ' -f1; }
want="$(fFp "${pub}")"
[[ -n "${want}" ]] || fDie "cannot fingerprint the signing key"

## Published key file, the one README tells people to verify against.
if [[ -r "${root}/shcl-signing.pub" ]]; then
	[[ "$(fFp "${root}/shcl-signing.pub")" == "${want}" ]] || fDie "shcl-signing.pub is not this key"
fi

## install.bash carries the PEM inside a single-quoted bash string, so the
## BEGIN line has a 'readonly SIGNING_KEY=' prefix and the END line a trailing
## quote. Strip both, or what comes out is not a PEM at all.
if [[ -r "${root}/install.bash" ]]; then
	sed -n '/BEGIN PUBLIC KEY/,/END PUBLIC KEY/p' "${root}/install.bash" \
		| sed "s/^[^-]*'//; s/'[[:space:]]*$//" > "${tmppub}"
	[[ "$(fFp "${tmppub}")" == "${want}" ]] || fDie "install.bash carries a different key"
fi

## install.ps1 carries the bare modulus, not a PEM - compare that directly.
## openssl gives the modulus as hex; printf turns each pair into its byte and
## openssl base64s the result, so no other tool is needed for the round trip.
if [[ -r "${root}/install.ps1" ]]; then
	psmod="$(sed -n "s/^\$signingModulus = '\(.*\)'.*/\1/p" "${root}/install.ps1")"
	modhex="$(openssl rsa -pubin -in "${pub}" -modulus -noout 2>/dev/null | sed 's/^Modulus=//; s/../\\x&/g')"
	[[ -n "${modhex}" ]] || fDie "cannot read the key's modulus"
	keymod="$(printf '%b' "${modhex}" | openssl enc -base64 -A)"
	[[ -n "${psmod}" && "${psmod}" == "${keymod}" ]] || fDie "install.ps1 carries a different key"
fi

printf 'signed  %s\n' "${sumsfile##*/}"
printf 'wrote   %s\n' "${sigfile##*/}"
printf 'key fp  %s\n' "$(openssl pkey -in "${key}" -pubout 2>/dev/null | openssl pkey -pubin -outform DER -pubout 2>/dev/null | sha256sum | cut -d' ' -f1)"
printf 'attach both the sums file and its .sig to the release.\n'

##	History:
##		- 2026-07-28 JC: Written for the 1.0.0 cut, when the sums file became a
##		  signed trust root rather than something trusted for being on https.
##		- 2026-08-29 JC: Refuse to sign off the v<version> tag (--no-tag-check
##		  for rehearsals); the modulus round trip needs only openssl now.
