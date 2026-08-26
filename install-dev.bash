#!/usr/bin/env bash
#••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••
## install-dev.bash
##
##	Dev-environment setup for shcl on Linux and macOS (on Windows, use WSL -
##	the dev pipeline is bash). Clones the repo if needed, installs what it can
##	without sudo (rustup, and the optional linters via pipx/npm/pwsh), and
##	prints the exact install hint for anything that needs the system package
##	manager. States the plan first, with an option to abort.
##
##	Usage (one-liner):
##		curl -fsSL https://raw.githubusercontent.com/jim-collier/shcl/main/install-dev.bash | bash
##	With options:
##		curl -fsSL .../install-dev.bash | bash -s -- --yes
##
##	Options:
##		--dir <path>   where to clone (default ./shcl; skipped when run inside
##		               an existing shcl clone).
##		--yes | -y     skip the confirmation prompt.
##
##	What a full dev box needs (see contributing.md "How to develop"):
##		gating:   rustup (rustfmt+clippy ride along), go, python3, gcc+g++,
##		          shellcheck, ruff, mypy, cppcheck, build, markdownlint-cli2,
##		          PSScriptAnalyzer (only if pwsh is present)
##		the gate: cicd/cicd.bash --ci
#••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••

##	Copyright © 2026 Jim Collier (CryptogID: ѳ6ᴚ℈𐀘𐇦ɛ𐊁¥Mﾏb϶Δ𐌞)
##	Licensed under The MIT License (MIT). Full text at:
##		https://mit-license.org/
##	SPDX-License-Identifier: MIT

set -euo pipefail

REPO_URL="https://github.com/jim-collier/shcl"
clone_dir="./shcl"
assume_yes=0

die() { printf 'install-dev.bash: %s\n' "$*" >&2; exit 1; }
have() { command -v "$1" >/dev/null 2>&1; }

## Usage text lives here, not in a sed slice of "$0": under the documented
## `curl | bash -s -- --help` pipe, $0 is just "bash" and sed reads the wrong
## file (or a stray one named "bash" in the cwd).
usage() {
	cat <<'EOF'
## install-dev.bash
##
##	Dev-environment setup for shcl on Linux and macOS (on Windows, use WSL -
##	the dev pipeline is bash). Clones the repo if needed, installs what it can
##	without sudo (rustup, and the optional linters via pipx/npm/pwsh), and
##	prints the exact install hint for anything that needs the system package
##	manager. States the plan first, with an option to abort.
##
##	Usage (one-liner):
##		curl -fsSL https://raw.githubusercontent.com/jim-collier/shcl/main/install-dev.bash | bash
##	With options:
##		curl -fsSL .../install-dev.bash | bash -s -- --yes
##
##	Options:
##		--dir <path>   where to clone (default ./shcl; skipped when run inside
##		               an existing shcl clone).
##		--yes | -y     skip the confirmation prompt.
##
##	What a full dev box needs (see contributing.md "How to develop"):
##		gating:   rustup (rustfmt+clippy ride along), go, python3, gcc+g++,
##		          shellcheck, ruff, mypy, cppcheck, build, markdownlint-cli2,
##		          PSScriptAnalyzer (only if pwsh is present)
##		the gate: cicd/cicd.bash --ci
#••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••
EOF
}

while (( $# )); do
	case "$1" in
		--dir=*) clone_dir="${1#*=}" ;;
		--dir)   (( $# >= 2 )) || die "missing value for --dir (try --dir=VALUE)"; shift; clone_dir="$1" ;;
		-y|--yes) assume_yes=1 ;;
		-h|--help) usage; exit 0 ;;
		*) die "unknown option: $1" ;;
	esac
	shift
done

case "$(uname -s)" in Linux|Darwin) ;; *) die "Linux/macOS only (on Windows, run this under WSL)" ;; esac
have git || die "git is required first"

## Already inside a clone? Then set up here instead of cloning again.
in_clone=0
if git rev-parse --show-toplevel >/dev/null 2>&1; then
	origin="$(git remote get-url origin 2>/dev/null || true)"
	[[ "${origin}" == *jim-collier/shcl* ]] && { in_clone=1; clone_dir="$(git rev-parse --show-toplevel)"; }
fi

## Take stock: what gets installed (no sudo), what only gets a hint.
pkg_hint="your package manager"
if have apt-get; then pkg_hint="sudo apt-get install"
elif have dnf; then pkg_hint="sudo dnf install"
elif have pacman; then pkg_hint="sudo pacman -S"
elif have brew; then pkg_hint="brew install"
fi
todo=() hints=()
if ! have cargo && [[ ! -x "${HOME}/.cargo/bin/cargo" ]]; then todo+=("rustup (official installer, user-space)"); fi
have go         || hints+=("go          - ${pkg_hint} golang (or https://go.dev/dl)")
have python3    || hints+=("python3     - ${pkg_hint} python3")
have cc         || hints+=("gcc/g++     - ${pkg_hint} build-essential (or gcc gcc-c++)")
have shellcheck || hints+=("shellcheck  - ${pkg_hint} shellcheck")
if have pipx; then
	for t in ruff mypy cppcheck; do have "$t" || todo+=("${t} (pipx, user-space)"); done
	have pyproject-build || todo+=("build (pipx, user-space)")
else
	hints+=("pipx        - ${pkg_hint} pipx  (then re-run for ruff/mypy/cppcheck/build)")
fi
if have npm; then
	have markdownlint-cli2 || todo+=("markdownlint-cli2 (npm -g --prefix ~/.local)")
else
	hints+=("npm         - ${pkg_hint} npm  (then re-run for markdownlint-cli2)")
fi
if have pwsh; then
	pwsh -NoProfile -Command "if (-not (Get-Module -ListAvailable PSScriptAnalyzer)) { exit 1 }" >/dev/null 2>&1 \
		|| todo+=("PSScriptAnalyzer (pwsh Install-Module, user-scope)")
fi

## The plan.
echo
if (( in_clone )); then
	printf 'using the existing clone at %s\n' "${clone_dir}"
else
	printf 'will clone %s -> %s\n' "${REPO_URL}" "${clone_dir}"
fi
if (( ${#todo[@]} )); then
	echo "will install (no sudo):"
	printf '  %s\n' "${todo[@]}"
fi
if (( ${#hints[@]} )); then
	echo "missing - install these yourself (needs the system package manager):"
	printf '  %s\n' "${hints[@]}"
fi
if (( ! ${#todo[@]} && ! ${#hints[@]} )) && (( in_clone )); then
	echo "everything is already in place"
fi
if (( ! assume_yes )); then
	## Ask on the terminal, and treat "cannot ask" as the abort it is. Testing
	## /dev/tty for readability was not the same question: it passes in plenty of
	## unattended contexts where the read then dies on a raw shell error.
	reply=""
	if ! read -r -p "Proceed? [y/N] " reply </dev/tty 2>/dev/null; then
		die "no terminal to confirm on - pass --yes"
	fi
	case "${reply}" in y|Y|yes|Yes|YES) ;; *) echo "aborted"; exit 1 ;; esac
fi

## Clone. Resolve the dir to an absolute path afterwards - later steps refer to
## it from inside the clone, where a relative --dir no longer points here.
if (( ! in_clone )); then
	echo
	[[ -e "${clone_dir}/.git" ]] || git clone "${REPO_URL}" "${clone_dir}"
	cd "${clone_dir}"
	clone_dir="$(pwd)"
fi

## Install the user-space pieces.
echo
if ! have cargo && [[ ! -x "${HOME}/.cargo/bin/cargo" ]]; then
	echo "installing rustup..."
	curl -fsSL --proto '=https' --proto-redir '=https' --tlsv1.2 https://sh.rustup.rs | sh -s -- -y --no-modify-path
fi
if have pipx; then
	for t in ruff mypy cppcheck; do have "$t" || pipx install "$t"; done
	have pyproject-build || pipx install build   ## the package is "build", the command is not
fi
if have npm && ! have markdownlint-cli2; then
	npm install -g --prefix "${HOME}/.local" markdownlint-cli2
fi
if have pwsh; then
	pwsh -NoProfile -Command "if (-not (Get-Module -ListAvailable PSScriptAnalyzer)) { Install-Module PSScriptAnalyzer -Scope CurrentUser -Force }"
fi

## Point git at the tracked hooks rather than copying them in, so an update to
## the hook arrives with a pull instead of needing a reinstall.
if [[ -d "${clone_dir}/.git" ]]; then
	git -C "${clone_dir}" config core.hooksPath cicd/hooks
	echo "git hooks: core.hooksPath -> cicd/hooks (pre-push gates main and dev)"
fi

echo
echo "done. The gate is:  cicd/cicd.bash --ci"
echo "(rust-toolchain.toml pins the toolchain; the first cargo run fetches it.)"
(( ${#hints[@]} )) && echo "note: the hinted packages above are still missing."
echo
exit 0
