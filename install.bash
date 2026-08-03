#!/usr/bin/env bash
#••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••
## install.bash
##
##	Release installer for shcl (Simple Hierarchical Config Language) on Linux.
##	Downloads the latest release from GitHub, checks the sha256sums file against
##	the release signing key before trusting a checksum out of it, and lays out
##	the binary plus the drop-in source files and shell wrappers. Idempotent:
##	re-running updates an existing install in place.
##
##	Needs curl or wget, plus openssl for the signature check.
##
##	Usage (one-liner):
##		curl -fsSL https://raw.githubusercontent.com/jim-collier/shcl/main/install.bash | bash
##		wget -qO- https://raw.githubusercontent.com/jim-collier/shcl/main/install.bash | bash
##	With options:
##		curl -fsSL .../install.bash | bash -s -- --target=user --yes
##
##	Options (both --opt=VALUE and --opt VALUE work):
##		--release <dev|stable>   dev = newest release including pre-releases
##		                         (default); stable = newest full release.
##		--target <user|system>   system (default): /opt/shcl + a symlink at
##		                         /usr/local/bin/shcl (sudo if not root).
##		                         user: ~/.local/share/shcl + a symlink at
##		                         ~/.local/bin/shcl. No sudo.
##		--yes | -y               skip the confirmation prompt.
##
##	Layout under the install dir:
##		shcl        the CLI binary
##		code/       drop-in single-file bindings (lib.rs, shcl.go, shcl.py,
##		            shcl.h, shcl.hpp)
##		scripts/    shell wrappers (shcl.bash, shcl.ps1)
##
##	macOS and the BSDs have no prebuilt binaries yet - build from source or use
##	a drop-in file (see README.md).
#••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••

##	Copyright © 2026 Jim Collier (ID: 1cv◂‡Vᛦ)
##	Licensed under The MIT License (MIT). Full text at:
##		https://mit-license.org/
##	SPDX-License-Identifier: MIT

set -euo pipefail

REPO="jim-collier/shcl"
release="dev"
target="system"
assume_yes=0

## Release signing key. The sha256sums file is signed offline with the matching
## private key, so replacing a release asset is not enough - an attacker would
## have to forge this signature too. Baked in rather than fetched, because a key
## downloaded over the same channel as the artifact proves nothing. Rotating it
## means publishing a new installer; treat that as a breaking change.
readonly SIGNING_KEY='-----BEGIN PUBLIC KEY-----
MIICIjANBgkqhkiG9w0BAQEFAAOCAg8AMIICCgKCAgEAk5W58wTiFTlHUCsIuHES
qexain6AC8WwFmCDsjfliOIDa2vPhkSVOqMsSbYH/OL94pHZ+Bs0agNXrl99ANol
zwQ4rvu6gAsc4GCb0Krbbq2B+jKqTM8xeN7tFLWKd5E08IOF2HA4ugQSlK+rC6ez
bBqP1MuJFFxqxDhEtGef9v/nuhX2kWq3v0uN6Y0umbghuNAR7gmoSOwbb8uYfVOA
H1OAWV2To2wyIe6WWt4BPmFJBpEI53k4rmoDVdjmJFoj2vETHmEh2QfTPA5541jP
LeuO8p8V6+Aa8i32EtVeT1+ozwHidku/CZZOAdxYZ7yXAZdG3eOOxcHVfmXVwqRx
PR+lA3E/KcRcN9oeeveXS35jwH0h3hSh6sJOr1q0qMtM7bB4Lxt47wXHTJ0VPneG
5xbmO5pUS3LMcZwnXXavYjh2kYS52ZLhi1JbPFgPyYUiIv76IUbwtpEXbONi12g7
fioZ6cStZAekJs33Wkee6NmSY54AozxTkcNUJTgs81eMa/gRL8l3jud8AWqL5vyk
qpG1PTN70vSgrHD4wNMp2QX29Iv+A6+FO4B1oxjrnokg212rwqX004Ep0csu/JjO
l9XHvwp0Iucfi8zCg7ozDcU3dsDnUJ8A3PtJ47jEt1n37/oiM6pWDXVVBjz4DI9i
ACmdUphTcGhYvn91ORZVxt0CAwEAAQ==
-----END PUBLIC KEY-----'

die() { printf 'install.bash: %s\n' "$*" >&2; exit 1; }

## Usage text lives here, not in a sed slice of "$0": under the documented
## `curl | bash -s -- --help` pipe, $0 is just "bash" and sed reads the wrong
## file (or a stray one named "bash" in the cwd).
usage() {
	cat <<'EOF'
## install.bash
##
##	Release installer for shcl (Simple Hierarchical Config Language) on Linux.
##	Downloads the latest release from GitHub, checks the sha256sums file against
##	the release signing key before trusting a checksum out of it, and lays out
##	the binary plus the drop-in source files and shell wrappers. Idempotent:
##	re-running updates an existing install in place.
##
##	Needs curl or wget, plus openssl for the signature check.
##
##	Usage (one-liner):
##		curl -fsSL https://raw.githubusercontent.com/jim-collier/shcl/main/install.bash | bash
##		wget -qO- https://raw.githubusercontent.com/jim-collier/shcl/main/install.bash | bash
##	With options:
##		curl -fsSL .../install.bash | bash -s -- --target=user --yes
##
##	Options (both --opt=VALUE and --opt VALUE work):
##		--release <dev|stable>   dev = newest release including pre-releases
##		                         (default); stable = newest full release.
##		--target <user|system>   system (default): /opt/shcl + a symlink at
##		                         /usr/local/bin/shcl (sudo if not root).
##		                         user: ~/.local/share/shcl + a symlink at
##		                         ~/.local/bin/shcl. No sudo.
##		--yes | -y               skip the confirmation prompt.
##
##	Layout under the install dir:
##		shcl        the CLI binary
##		code/       drop-in single-file bindings (lib.rs, shcl.go, shcl.py,
##		            shcl.h, shcl.hpp)
##		scripts/    shell wrappers (shcl.bash, shcl.ps1)
##
EOF
}

## Value options accept --opt=VALUE and --opt VALUE, like the shcl CLI.
while (( $# )); do
	case "$1" in
		--release=*) release="${1#*=}" ;;
		--release)   (( $# >= 2 )) || die "missing value for --release (try --release=VALUE)"; shift; release="$1" ;;
		--target=*)  target="${1#*=}" ;;
		--target)    (( $# >= 2 )) || die "missing value for --target (try --target=VALUE)"; shift; target="$1" ;;
		-y|--yes)    assume_yes=1 ;;
		-h|--help)   usage; exit 0 ;;
		*) die "unknown option: $1" ;;
	esac
	shift
done
case "${release}" in dev|development) release="dev" ;; stable) ;; *) die "--release must be dev or stable" ;; esac
case "${target}" in user|system) ;; *) die "--target must be user or system" ;; esac

## Platform gate: prebuilt binaries exist for Linux x86_64/arm64 only.
os="$(uname -s)"
[[ "${os}" == "Linux" ]] || die "no prebuilt ${os} binaries yet - build from source or use a drop-in file (see README.md)"
case "$(uname -m)" in
	x86_64|amd64)  arch="x86_64" ;;
	aarch64|arm64) arch="arm64" ;;
	*) die "no prebuilt binary for $(uname -m)" ;;
esac

## curl or wget, whichever is present. https is pinned through redirects and
## TLS floored at 1.2, so a bounced download can't silently downgrade.
if command -v curl >/dev/null; then
	fetch() { curl -fsSL --proto '=https' --proto-redir '=https' --tlsv1.2 -o "$2" "$1"; }
elif command -v wget >/dev/null; then
	fetch() { wget -q --https-only --secure-protocol=TLSv1_2 -O "$2" "$1"; }
else
	die "need curl or wget"
fi
## openssl is a hard requirement, same class as curl/wget: without it the
## release signature cannot be checked, and installing unverified is not on
## offer. Verify by hand and use the DIY path if the box genuinely lacks it.
command -v openssl >/dev/null || die "need openssl to verify the release signature (see README.md for a manual install)"

## Resolve the tag. GitHub's /releases/latest is exactly "newest non-prerelease";
## dev takes the newest of everything.
if [[ "${release}" == "stable" ]]; then
	api="https://api.github.com/repos/${REPO}/releases/latest"
else
	api="https://api.github.com/repos/${REPO}/releases?per_page=1"
fi
tmp="$(mktemp -d)"
trap 'rm -rf "${tmp}"' EXIT
fetch "${api}" "${tmp}/rel.json" || die "cannot fetch the ${release} release (none published yet, or network down)"
tag="$(grep -o '"tag_name": *"[^"]*"' "${tmp}/rel.json" | head -n1 | sed 's/.*"\(v[^"]*\)"/\1/')"
[[ -n "${tag}" && "${tag}" != null ]] || die "no ${release} release found"
version="${tag#v}"

## Destinations.
if [[ "${target}" == "system" ]]; then
	dest="/opt/shcl"
	link="/usr/local/bin/shcl"
	asroot=""
	[[ "$(id -u)" == 0 ]] || asroot="sudo"
else
	dest="${HOME}/.local/share/shcl"
	link="${HOME}/.local/bin/shcl"
	asroot=""
fi

## State the plan; abort is the default when there is no tty to confirm on.
existing="new install"
[[ -e "${dest}/shcl" ]] && existing="updates the existing install"
printf 'shcl %s (%s, linux-%s) -> %s (%s)\n' "${version}" "${release}" "${arch}" "${dest}" "${existing}"
printf '  binary   %s/shcl (symlink %s)\n' "${dest}" "${link}"
printf '  drop-ins %s/code/, wrappers %s/scripts/\n' "${dest}" "${dest}"
[[ -n "${asroot}" ]] && printf '  uses sudo for %s and %s\n' "${dest}" "${link}"
if (( ! assume_yes )); then
	[[ -r /dev/tty ]] || die "no tty to confirm on - pass --yes"
	read -r -p "Proceed? [y/N] " reply </dev/tty
	[[ "${reply}" == y || "${reply}" == Y ]] || { echo "aborted"; exit 1; }
fi

## Download and verify the binary.
asset="shcl-${version}-linux-${arch}"
base="https://github.com/${REPO}/releases/download/${tag}"
echo "downloading ${asset}..."
fetch "${base}/${asset}" "${tmp}/shcl" || die "download failed: ${asset}"
fetch "${base}/shcl-${version}-sha256sums.txt" "${tmp}/sums" || die "download failed: sha256sums"
fetch "${base}/shcl-${version}-sha256sums.txt.sig" "${tmp}/sums.sig" || die "download failed: sha256sums signature"

## Check the signature before trusting anything the sums file says. Order is the
## whole point: a checksum read out of an unverified file proves nothing.
printf '%s\n' "${SIGNING_KEY}" > "${tmp}/signing.pub"
openssl dgst -sha256 -verify "${tmp}/signing.pub" -signature "${tmp}/sums.sig" "${tmp}/sums" >/dev/null 2>&1 \
	|| die "signature check failed on sha256sums - refusing to install"

want="$(grep " ${asset}\$" "${tmp}/sums" | cut -d' ' -f1)"
got="$(sha256sum "${tmp}/shcl" | cut -d' ' -f1)"
[[ -n "${want}" && "${got}" == "${want}" ]] || die "sha256 mismatch on ${asset}"

## Drop-in code files and wrappers come from the tag's source tarball.
echo "downloading source payload (${tag})..."
fetch "https://github.com/${REPO}/archive/refs/tags/${tag}.tar.gz" "${tmp}/src.tgz" || die "download failed: source tarball"
tar -xzf "${tmp}/src.tgz" -C "${tmp}"
srcroot="$(find "${tmp}" -maxdepth 1 -type d -name "shcl-*" | head -n1)"
[[ -n "${srcroot}" ]] || die "unexpected source tarball layout"
mkdir -p "${tmp}/code" "${tmp}/scripts"
cp "${srcroot}/source/rust/src/lib.rs" "${srcroot}/source/go/shcl.go" "${srcroot}/source/python/shcl.py" \
   "${srcroot}/source/c/shcl.h" "${srcroot}/source/c/shcl.hpp" "${tmp}/code/"
cp "${srcroot}/source/bash/shcl.bash" "${srcroot}/source/powershell/shcl.ps1" "${tmp}/scripts/"
chmod 755 "${tmp}/shcl" "${tmp}/scripts/shcl.bash"

## Install. The binary goes in via a hidden temp + mv in the same dir, so a
## running copy only ever sees the complete old or new file.
${asroot} mkdir -p "${dest}/code" "${dest}/scripts" "$(dirname "${link}")"
${asroot} cp "${tmp}/shcl" "${dest}/.shcl.new"
${asroot} mv -f "${dest}/.shcl.new" "${dest}/shcl"
${asroot} cp "${tmp}"/code/* "${dest}/code/"
${asroot} cp "${tmp}"/scripts/* "${dest}/scripts/"
${asroot} ln -sfn "${dest}/shcl" "${link}"

printf 'installed shcl %s -> %s\n' "${version}" "${link}"
## Both targets: an install nobody can invoke by name looks fine to the version
## check below, so say something when the symlink dir is off the PATH.
linkdir="$(dirname "${link}")"
if [[ ":${PATH}:" != *":${linkdir}:"* ]]; then
	printf 'note: %s is not on your PATH\n' "${linkdir}"
fi
"${link}" version 2>/dev/null || "${dest}/shcl" version
