#!/usr/bin/env bash

##	Purpose:
##		Build installable packages from the versioned release artifacts:
##		.deb + .rpm via nfpm (x86_64 and arm64, whichever binaries exist) and an
##		NSIS setup .exe per Windows binary. Payload mirrors install.bash: binary,
##		code/ drop-ins (lib.rs, shcl.go, shcl.py, shcl.h, shcl.hpp), scripts/
##		wrappers (shcl.bash, shcl.ps1). Packages land beside the raw binaries in
##		the artifact dir, named into the same shcl-<version>-* family so the
##		engine's sha256sums rewrite picks them up.
##	Syntax:
##		package.bash ROOT ART_DIR VERSION
##	Exit: 0 = all buildable packages built (a missing tool warns and skips its
##	formats), nonzero = a package build failed.
##	History: At bottom of script.

##	Copyright © 2026 Jim Collier (ID: 1cv◂‡Vᛦ)
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

built=0

## Linux: .deb + .rpm per arch with a binary present. nfpm arch names are
## GOARCH-style; the artifact names carry the uname-style spelling. The config
## template is sed-rendered per build (nfpm won't expand env vars in src paths).
if command -v nfpm >/dev/null 2>&1; then
	for pair in "x86_64|amd64" "arm64|arm64"; do
		osarch="${pair%%|*}"; goarch="${pair#*|}"
		bin="${artDir}/shcl-${ver}-linux-${osarch}"
		[[ -f "${bin}" ]] || continue
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
	for osarch in x86_64 arm64; do
		exe="${artDir}/shcl-${ver}-windows-${osarch}.exe"
		[[ -f "${exe}" ]] || continue
		out="${artDir}/shcl-${ver}-windows-${osarch}-setup.exe"
		makensis -V2 -DVERSION="${ver}" -DSRCEXE="${exe}" -DPAYLOAD="${payload}" -DOUTFILE="${out}" \
			"${meDir}/../packaging/shcl.nsi" >/dev/null
		fEcho "OK: package: $(basename "${out}") ($(du -h --apparent-size "${out}" | cut -f1))"
		built=$((built + 1))
	done
else
	fWarn "makensis not installed; NSIS setup skipped"
fi

((built)) || fWarn "no packages built (no matching binaries in ${artDir})"


##	History:
##		- 2026-07-22 JC: Created: nfpm deb/rpm + NSIS setup over the release artifact dir.
