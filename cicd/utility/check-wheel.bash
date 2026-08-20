#!/usr/bin/env bash

##	Purpose:
##		Keep the Python distribution to the library alone. The CLI and the test
##		runner live beside the module and must never ship: the Rust binary is the
##		only CLI this project distributes, and a PyPI install is a dependency,
##		not an installation. Only one line of pyproject.toml enforces that today
##		(py-modules), so a switch to package discovery would start shipping cmd/
##		and tests/ with nothing to notice. This builds both artifacts and reads
##		what is actually in them.
##	Syntax:
##		check-wheel.bash [ROOT]
##		  ROOT   repo root (default: two levels up from this script)
##	Exit: 0 = the wheel and sdist carry the library and nothing else, 1 = they do not.
##	History: At bottom of script.

##	Copyright © 2026 Bubbles (ID: XଌฅრX۳ᛟԃლፀƅꓩหδლც)
##	Licensed under The MIT License (MIT). Full text at:
##		https://mit-license.org/
##	SPDX-License-Identifier: MIT


set -Eeuo pipefail

meDir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
root="${1:-$(cd -- "${meDir}/../.." && pwd)}"
pyDir="${root}/source/python"

command -v pyproject-build >/dev/null 2>&1 \
	|| { echo "check-wheel: pyproject-build not installed (pipx install build)" >&2; exit 1; }

## Build into a temp dir, and take the build's own leavings with it: an sdist
## build writes shcl.egg-info and build/ back into the source tree, which is
## exactly the stale-artifact litter this repo keeps having to sweep up.
outDir="$(mktemp -d)"
trap 'rm -rf "${outDir}" "${pyDir}/build" "${pyDir}/shcl.egg-info"' EXIT

( cd "${pyDir}" && pyproject-build --outdir "${outDir}" ) >/dev/null 2>&1 \
	|| { echo "check-wheel: build failed" >&2; exit 1; }

## Indent a multi-line block for the error output. Parameter expansion rather
## than a sed pipe: one less fork, and shellcheck prefers it.
fIndent(){ printf '  %s\n' "${1//$'\n'/$'\n'  }"; }

rc=0

## The wheel's payload is everything outside the .dist-info metadata directory.
## Exactly one file belongs there.
wheelPayload="$(python3 - "${outDir}" <<'PY'
import glob, sys, zipfile
names = zipfile.ZipFile(glob.glob(sys.argv[1] + "/*.whl")[0]).namelist()
print("\n".join(sorted(n for n in names if ".dist-info/" not in n and not n.endswith("/"))))
PY
)"
if [[ "${wheelPayload}" != "shcl.py" ]]; then
	echo "check-wheel: the wheel should carry shcl.py and nothing else; it carries:" >&2
	fIndent "${wheelPayload:-(nothing)}" >&2
	rc=1
fi

## An entry point would put a `shcl` command on PATH from a pip install, which
## is the thing this whole arrangement exists to prevent.
if python3 - "${outDir}" <<'PY'
import glob, sys, zipfile
z = zipfile.ZipFile(glob.glob(sys.argv[1] + "/*.whl")[0])
sys.exit(0 if any(n.endswith("entry_points.txt") for n in z.namelist()) else 1)
PY
then
	echo "check-wheel: the wheel declares entry points; a pip install would install a command" >&2
	rc=1
fi

## The sdist is looser by nature - it carries the project files - but the CLI
## and the tests still have no business in it.
strays="$(tar tzf "${outDir}"/*.tar.gz | grep -E '/(cmd|tests)/' || true)"
if [[ -n "${strays}" ]]; then
	echo "check-wheel: the sdist carries the CLI or the tests:" >&2
	fIndent "${strays}" >&2
	rc=1
fi

((rc == 0)) && echo "check-wheel: the python distribution is the library alone"
exit "${rc}"


##	History:
##		- 2026-08-20 JC: Created. Builds the wheel and sdist and reads what is in them, rather than trusting the one pyproject line that keeps the CLI out.
