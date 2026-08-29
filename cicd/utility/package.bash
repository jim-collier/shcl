#!/usr/bin/env bash

##	Purpose:
##		Build installable packages from the versioned release artifacts:
##		.deb + .rpm via nfpm (x86_64 and arm64, whichever binaries exist) and an
##		NSIS setup .exe per Windows binary. Payload mirrors install.bash: binary,
##		code/ drop-ins (lib.rs, shcl.go, shcl.py, shcl.h, shcl.hpp), scripts/
##		wrappers (shcl.bash, shcl.ps1), and for the Linux packages only the man/
##		page and completions/ for bash and zsh. Packages land beside the raw
##		binaries in the artifact dir, named into the same shcl-<version>-* family
##		so the engine's sha256sums rewrite picks them up. Two builds of one
##		commit give the same bytes (mtimes and build metadata are pinned to the
##		commit time through SOURCE_DATE_EPOCH).
##	Syntax:
##		package.bash ROOT ART_DIR VERSION
##	Exit: 0 = all buildable packages built (a missing tool warns and skips its
##	formats), nonzero = a package build failed.
##	History: At bottom of script.

##	Copyright © 2026 Bubbles (ID: XଌฅრX۳ᛟԃლፀƅꓩหδლც)
##	Licensed under The MIT License (MIT). Full text at:
##		https://mit-license.org/
##	SPDX-License-Identifier: MIT


set -Eeuo pipefail

root="${1:?usage: package.bash ROOT ART_DIR VERSION}"
artDir="${2:?usage: package.bash ROOT ART_DIR VERSION}"
ver="${3:?usage: package.bash ROOT ART_DIR VERSION}"
meDir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

fEcho(){ echo "[ $* ]"; }
fWarn(){ fEcho "WARNING: $*"; }

## Stage the shared payload (same file set install.bash pulls from a tag).
payload="$(mktemp -d)"
trap 'rm -rf "${payload}"' EXIT
mkdir -p "${payload}/code" "${payload}/scripts"
chmod 755 "${payload}" "${payload}/code" "${payload}/scripts"   ## tree type copies dir modes into the package
cp "${root}/source/rust/src/lib.rs" "${root}/source/go/shcl.go" "${root}/source/python/shcl.py" \
   "${root}/source/c/shcl.h" "${root}/source/c/shcl.hpp" "${payload}/code/"
cp "${root}/source/bash/shcl.bash" "${root}/source/powershell/shcl.ps1" "${payload}/scripts/"
chmod 644 "${payload}"/code/* "${payload}/scripts/shcl.ps1"
chmod 755 "${payload}/scripts/shcl.bash"

## Every package records the mtime of what it holds, so a payload staged a
## minute later gives different bytes for the same commit. Pin the staged tree
## and the binaries to the commit's time; nfpm reads the same value for the
## package's own build date. The engine exports it for the drop-ins tarball too.
export SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-$(git -C "${root}" log -1 --format=%ct)}"
fPinMtime(){ touch -d "@${SOURCE_DATE_EPOCH}" "$@"; }
find "${payload}" -exec touch -d "@${SOURCE_DATE_EPOCH}" {} +

built=0

## Linux: .deb + .rpm per arch with a binary present. nfpm arch names are
## GOARCH-style; the artifact names carry the uname-style spelling. The config
## template is sed-rendered per build (nfpm won't expand env vars in src paths).
if command -v nfpm >/dev/null 2>&1; then
	## The man page and the shell completions have a home only on Linux, so they
	## join the payload here and never reach the Windows setup. Both package
	## formats want the man page compressed; -n keeps the timestamp and the
	## original name out of the gzip header, so the same source gives the same
	## bytes on every build.
	mkdir -p "${payload}/man" "${payload}/completions"
	gzip -9 -n -c "${root}/source/man/shcl.1" > "${payload}/man/shcl.1.gz"
	cp "${root}/source/completions/shcl.bash" "${root}/source/completions/_shcl" "${payload}/completions/"
	chmod 644 "${payload}/man/shcl.1.gz" "${payload}"/completions/*
	fPinMtime "${payload}/man" "${payload}/man/shcl.1.gz" "${payload}/completions" "${payload}"/completions/*
	for pair in "x86_64|amd64" "arm64|arm64"; do
		osarch="${pair%%|*}"; goarch="${pair#*|}"
		bin="${artDir}/shcl-${ver}-linux-${osarch}"
		[[ -f "${bin}" ]] || continue
		fPinMtime "${bin}"
		sed -e "s|\${SHCL_VERSION}|${ver}|g" -e "s|\${SHCL_ARCH}|${goarch}|g" \
		    -e "s|\${SHCL_BIN}|${bin}|g" -e "s|\${SHCL_PAYLOAD}|${payload}|g" \
		    "${meDir}/../packaging/nfpm.yaml" > "${payload}/nfpm.yaml"
		for fmt in deb rpm; do
			out="${artDir}/shcl-${ver}-linux-${osarch}.${fmt}"
			nfpm package -f "${payload}/nfpm.yaml" -p "${fmt}" -t "${out}" >/dev/null
			fEcho "OK: package: $(basename "${out}") ($(du -h --apparent-size "${out}" | cut -f1))"
			built=$((built + 1))
		done
	done
else
	fWarn "nfpm not installed; .deb/.rpm skipped"
fi

## Windows: an NSIS setup per built .exe. The x86 installer stub runs fine on
## ARM64 Windows (emulated), so one .nsi covers both.
if command -v makensis >/dev/null 2>&1; then
	## Same icon the executables carry. Absent is not an error - the setup just
	## falls back to the NSIS default.
	icoArg=""; [[ -f "${root}/assets/shcl.ico" ]] && icoArg="${root}/assets/shcl.ico"
	for osarch in x86_64 arm64; do
		exe="${artDir}/shcl-${ver}-windows-${osarch}.exe"
		[[ -f "${exe}" ]] || continue
		fPinMtime "${exe}"
		out="${artDir}/shcl-${ver}-windows-${osarch}-setup.exe"
		makensis -V2 -DVERSION="${ver}" -DSRCEXE="${exe}" -DPAYLOAD="${payload}" -DOUTFILE="${out}" \
			${icoArg:+-DICON="${icoArg}"} "${meDir}/../packaging/shcl.nsi" >/dev/null
		fEcho "OK: package: $(basename "${out}") ($(du -h --apparent-size "${out}" | cut -f1))"
		built=$((built + 1))
	done
else
	fWarn "makensis not installed; NSIS setup skipped"
fi

((built)) || fWarn "no packages built (no matching binaries in ${artDir})"


##	History:
##		- 2026-07-22: Created: nfpm deb/rpm + NSIS setup over the release artifact dir.
##		- 2026-08-29: Reproducible output: mtimes pinned to the commit time; man page
##		  and completions staged for the Linux packages only.
