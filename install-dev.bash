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
##		          staticcheck, govulncheck, cargo-deny (all at the pinned versions),
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
##		          staticcheck, govulncheck, cargo-deny (all at the pinned versions),
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

## curl or wget, whichever is present, the same way install.bash does it; only
## the rustup step needs one. https is pinned through redirects and TLS floored
## at 1.2, so a bounced download can't silently downgrade.
if have curl; then
	fetch() { curl -fsSL --proto '=https' --proto-redir '=https' --tlsv1.2 -o "$2" "$1"; }
elif have wget; then
	fetch() { wget -q --https-only --secure-protocol=TLSv1_2 -O "$2" "$1"; }
fi

## Already inside a clone? Then set up here instead of cloning again. Known by
## shape, not by remote URL, so a fork's clone counts too.
in_clone=0
if top="$(git rev-parse --show-toplevel 2>/dev/null)" \
	&& [[ -f "${top}/cicd/cicd.bash" ]] && grep -q '^name = "shcl"' "${top}/source/rust/Cargo.toml" 2>/dev/null; then
	in_clone=1; clone_dir="${top}"
fi

## Tool versions come from TOOL_PINS in cicd/config.bash, so a dev box gets what
## the gate expects rather than whatever is newest today (the pipeline warns on
## every run otherwise). Outside a clone the file is fetched from main: there is
## nothing else to read it from before the clone exists.
if (( in_clone )); then
	pins_file="${clone_dir}/cicd/config.bash"
else
	have curl || have wget || die "need curl or wget"
	pins_file="$(mktemp)"
	trap 'rm -f "${pins_file}"' EXIT
	fetch "https://raw.githubusercontent.com/jim-collier/shcl/main/cicd/config.bash" "${pins_file}" || die "cannot fetch cicd/config.bash for the tool pins"
fi
## "name|version|command" out of TOOL_PINS -> pin_ver, pin_cmd.
pin() {
	local line
	line="$(awk -v name="$1" '/^TOOL_PINS=\(/ { inpins=1; next } inpins && /^\)/ { exit } inpins && index($0, "\"" name "|") { sub(/^[ \t]*"/, ""); sub(/"[ \t]*$/, ""); print; exit }' "${pins_file}")"
	[[ -n "${line}" ]] || die "no TOOL_PINS entry for $1 in cicd/config.bash"
	pin_ver="${line#*|}"; pin_cmd="${pin_ver#*|}"; pin_ver="${pin_ver%%|*}"
}
## Installed at the pinned version? The same test the pipeline's drift warning
## makes; missing and drifted both count as "install".
at_pin() {
	pin "$1"
	# shellcheck disable=SC2086
	[[ "$(${pin_cmd} 2>/dev/null | head -5 | tr '\n' ' ')" == *"${pin_ver}"* ]]
}

## Take stock: what gets installed (no sudo), what only gets a hint. The cargo
## and go bin dirs join PATH for the checks below, since that is where the
## installs land whether or not the user's profile knows it yet.
orig_path="${PATH}"
[[ -d "${HOME}/.cargo/bin" ]] && PATH="${HOME}/.cargo/bin:${PATH}"
gobin=""
have go && gobin="$(go env GOPATH)/bin" && PATH="${gobin}:${PATH}"
pkg_hint="your package manager"
if have apt-get; then pkg_hint="sudo apt-get install"
elif have dnf; then pkg_hint="sudo dnf install"
elif have pacman; then pkg_hint="sudo pacman -S"
elif have brew; then pkg_hint="brew install"
fi
todo=() hints=()
need_rustup=0
if ! have cargo && [[ ! -x "${HOME}/.cargo/bin/cargo" ]]; then
	if have curl || have wget; then need_rustup=1; todo+=("rustup (official installer, user-space)")
	else hints+=("curl/wget   - ${pkg_hint} curl  (then re-run for rustup)")
	fi
fi
have go         || hints+=("go          - ${pkg_hint} golang (or https://go.dev/dl)")
have python3    || hints+=("python3     - ${pkg_hint} python3")
have cc         || hints+=("gcc/g++     - ${pkg_hint} build-essential (or gcc gcc-c++)")
have shellcheck || hints+=("shellcheck  - ${pkg_hint} shellcheck")
if have pipx; then
	for t in ruff mypy cppcheck build; do at_pin "$t" || todo+=("${t} ${pin_ver} (pipx, user-space)"); done
else
	hints+=("pipx        - ${pkg_hint} pipx  (then re-run for ruff/mypy/cppcheck/build)")
fi
if have npm; then
	at_pin markdownlint-cli2 || todo+=("markdownlint-cli2 ${pin_ver} (npm -g --prefix ~/.local)")
else
	hints+=("npm         - ${pkg_hint} npm  (then re-run for markdownlint-cli2)")
fi
## The supply-chain trio the gate runs. The Go pair ride the go hint when go is
## missing; cargo-deny builds through cargo, which may itself arrive this run.
if have go; then
	for t in staticcheck govulncheck; do at_pin "$t" || todo+=("${t} ${pin_ver} (go install, user-space)"); done
fi
if (( need_rustup )) || have cargo || [[ -x "${HOME}/.cargo/bin/cargo" ]]; then
	at_pin cargo-deny || todo+=("cargo-deny ${pin_ver} (cargo install, user-space - builds from source, takes a while)")
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
	if ! read -r -p "Proceed? [y/N] " reply 2>/dev/null </dev/tty; then
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
if (( need_rustup )); then
	echo "installing rustup..."
	fetch https://sh.rustup.rs - | sh -s -- -y --no-modify-path
	PATH="${HOME}/.cargo/bin:${PATH}"
fi
if have pipx; then
	for t in ruff mypy build; do at_pin "$t" || pipx install --force "${t}==${pin_ver}"; done
	## TOOL_PINS pins the cppcheck binary; the PyPI package that carries it has
	## its own version (see the note beside the pin). The one place this is
	## spelled besides ci.yml.
	at_pin cppcheck || pipx install --force "cppcheck==1.5.1"
fi
if have npm; then
	at_pin markdownlint-cli2 || npm install -g --prefix "${HOME}/.local" "markdownlint-cli2@${pin_ver}"
fi
if have go; then
	at_pin staticcheck || go install "honnef.co/go/tools/cmd/staticcheck@${pin_ver}"
	at_pin govulncheck || go install "golang.org/x/vuln/cmd/govulncheck@${pin_ver}"
fi
if have cargo; then
	at_pin cargo-deny || cargo install cargo-deny --version "${pin_ver}" --locked
fi
if have pwsh; then
	pwsh -NoProfile -Command "if (-not (Get-Module -ListAvailable PSScriptAnalyzer)) { Install-Module PSScriptAnalyzer -Scope CurrentUser -Force }"
fi

## Point git at the tracked hooks rather than copying them in, so an update to
## the hook arrives with a pull instead of needing a reinstall.
if [[ -d "${clone_dir}/.git" ]]; then
	git -C "${clone_dir}" config core.hooksPath cicd/hooks
	echo "git hooks: core.hooksPath -> cicd/hooks (pre-push gates main and dev)"
	## The gate runs for minutes while git already holds the ssh session open;
	## without keepalives GitHub drops it and the push dies of SIGPIPE after a
	## green gate. Only set when nothing is configured, so a chosen key stays.
	if ! git -C "${clone_dir}" config core.sshCommand >/dev/null; then
		git -C "${clone_dir}" config core.sshCommand "ssh -o ServerAliveInterval=30 -o ServerAliveCountMax=30"
		echo "git ssh: keepalives on, so the pre-push gate cannot outlive the connection"
	fi
fi

echo
echo "done. The gate is:  cicd/cicd.bash --ci"
echo "(rust-toolchain.toml pins the toolchain; the first cargo run fetches it.)"
(( ${#hints[@]} )) && echo "note: the hinted packages above are still missing."
for bindir in "${HOME}/.cargo/bin" "${gobin}"; do
	[[ -n "${bindir}" && -d "${bindir}" && ":${orig_path}:" != *":${bindir}:"* ]] && echo "note: ${bindir} is not on your PATH - the tools installed there need it to be."
done
echo
exit 0
