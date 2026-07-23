#!/usr/bin/env bash
#••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••
## install.bash
##
##	Release installer for shcl (Simple Hierarchical Config Language) on Linux.
##	Downloads the latest release from GitHub, verifies the checksum, and lays
##	out the binary plus the drop-in source files and shell wrappers. Idempotent:
##	re-running updates an existing install in place.
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
##		                         /usr/local/sbin/shcl (sudo if not root).
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

die() { printf 'install.bash: %s\n' "$*" >&2; exit 1; }

## Value options accept --opt=VALUE and --opt VALUE, like the shcl CLI.
while (( $# )); do
	case "$1" in
		--release=*) release="${1#*=}" ;;
		--release)   shift; release="${1:-}" ;;
		--target=*)  target="${1#*=}" ;;
		--target)    shift; target="${1:-}" ;;
		-y|--yes)    assume_yes=1 ;;
		-h|--help)   sed -n '3,33p' "$0" 2>/dev/null || true; exit 0 ;;
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

## curl or wget, whichever is present.
if command -v curl >/dev/null; then
	fetch() { curl -fsSL -o "$2" "$1"; }
elif command -v wget >/dev/null; then
	fetch() { wget -qO "$2" "$1"; }
else
	die "need curl or wget"
fi

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
[[ -n "${tag}" ]] && [[ "${tag}" != null ]] || die "no ${release} release found"
version="${tag#v}"

## Destinations.
if [[ "${target}" == "system" ]]; then
	dest="/opt/shcl"
	link="/usr/local/sbin/shcl"
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
if [[ "${target}" == "user" && ":${PATH}:" != *":${HOME}/.local/bin:"* ]]; then
	printf 'note: %s is not on your PATH\n' "${HOME}/.local/bin"
fi
"${link}" version 2>/dev/null || "${dest}/shcl" version
