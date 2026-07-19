#!/usr/bin/env bash

#  shellcheck disable=2016  ## 'Expressions don't expand in single quotes.' Deliberate: the eval'd command strings expand in the engine, not here.
#  shellcheck disable=2034  ## 'variable appears unused.' Everything here is consumed by cicd.bash.

##	Purpose:
##		- Project-specific CI/CD settings for shcl.
##		- The engine (cicd.bash) stays generic; everything project-specific lives here.
##		- All command arrays run from the repo root; the Rust crate lives in
##		  source/rust (Tier 1 reference + the shcl CLI), so cargo goes through
##		  --manifest-path. Later bindings (Go/C/Python) add their commands here,
##		  not engine code.
##	History: At bottom of script.

##	Copyright © 2026 Jim Collier (ID: 1cv◂‡Vᛦ)
##	Licensed under The MIT License (MIT). Full text at:
##		https://mit-license.org/
##	SPDX-License-Identifier: MIT


## Only allow running 'sourced'.
declare -i isSourced_t6wqf=0; [[ "${BASH_SOURCE[0]}" == "${0}" ]] || isSourced_t6wqf=1
((isSourced_t6wqf)) || { echo -e "\nError in $(basename "${BASH_SOURCE[0]}"): This script is meant to be 'sourced' from within another script.\n"; exit 1; }


## Identity
APP_NAME="shcl"
EXE_NAME="shcl"
MANIFEST="source/rust/Cargo.toml"
VERSION_MANIFEST="${MANIFEST}"   ## the single canonical version source (tags + artifact names read it)

## Stage 1: format. FMT_CMD rewrites in place (local runs); FMT_CHECK_CMD fails on
## any diff without touching files (what --ci runs). rustfmt.toml at repo root
## (tabs) is the style source.
FMT_CMD=(cargo fmt --manifest-path "${MANIFEST}")
FMT_CHECK_CMD=(cargo fmt --check --manifest-path "${MANIFEST}")
FMT_EXTRA=('gofmt -w source/go')
FMT_CHECK_EXTRA=('gofmt_diff="$(gofmt -l source/go)"; [[ -z "${gofmt_diff}" ]] || { echo "gofmt: needs formatting: ${gofmt_diff}" >&2; false; }')

## Pinned versions of the external helper tools the pipeline uses; the engine
## warns (non-gating) when an installed tool has drifted from its pin, so a box
## update can't silently change results. Format: "name|version|command...".
## The rustc/clippy toolchain itself is pinned by rust-toolchain.toml at repo root.
TOOL_PINS=(
	"cargo-zigbuild|0.23.0|cargo-zigbuild --version"
	"ruff|0.15.22|ruff --version"
	"mypy|2.3.0|mypy --version"
	"cppcheck|2.17.1|cppcheck --version"
	"markdownlint-cli2|0.23.1|markdownlint-cli2 --help"
)

## Stage 2: debug build (what the tests exercise). Capped at half the cores.
## The Go and C binding CLIs build here too - crosscheck needs them in stage 4.
## C has no separate fmt/lint stage (no committed zero-dep formatter); the
## -Wall -Wextra -Werror compile is its quality gate, same spirit as clippy.
BUILD_CMD=(cargo build -j "${CPU_CAP}" --manifest-path "${MANIFEST}")
BUILD_EXTRA=(
	'go -C source/go build -o shcl ./cmd/shcl'
	'python3 -m py_compile source/python/shcl.py source/python/cmd/shcl/main.py source/python/tests/conformance.py'
	'cc -std=c11 -O2 -Wall -Wextra -Werror -Isource/c source/c/cmd/shcl/main.c -o source/c/shcl -lm -s'
)

## Stage 3: lint. clippy gates (-D warnings); shellcheck covers the pipeline's own
## scripts so cicd can't rot silently. The extras gate every other binding too:
## go vet, ruff + mypy (Python), cppcheck (C, exhaustive - ~20s), markdownlint
## over all tracked .md (config in .markdownlint-cli2.jsonc at repo root), and
## PSScriptAnalyzer on the ps1 wrapper. shfmt is deliberately NOT here - its
## output fights the hand-formatted shell style, so it stays interactive-only.
LINT_CMD=(cargo clippy -j "${CPU_CAP}" --manifest-path "${MANIFEST}" --all-targets -- -D warnings)
LINT_EXTRA=(
	'go -C source/go vet ./...'
	'ruff check source/python'
	'env MYPYPATH=source/python mypy source/python/shcl.py source/python/cmd/shcl/main.py source/python/tests/conformance.py'
	'cppcheck --error-exitcode=1 --enable=warning,portability --inline-suppr --check-level=exhaustive --quiet -Isource/c source/c/cmd/shcl/main.c source/c/tests/conformance.c'
	'markdownlint-cli2'
	'pwsh -NoProfile -Command "Invoke-ScriptAnalyzer -Path source/powershell/shcl.ps1 -Severity Warning,Error -EnableExit"'
)
## n8git_backup-and-publish is excluded: SC1083 false-hits its legitimate git
## @{u} upstream refs, and the script is a proven drop-in kept byte-close to its
## sibling copies.
SHELLCHECK_TARGETS=(
	cicd/cicd.bash
	cicd/config.bash
	cicd/utility/crosscheck.bash
	cicd/utility/lint-report.bash
	cicd/utility/git-auto-msg.bash
	cicd/utility/include/gfs-rotate.bash
	source/bash/shcl.bash
)

## Stage 4: tests. cargo test runs the conformance corpus (project/conformance/)
## plus the deterministic fuzz smoke; the env var raises the fuzz iteration count
## well past the quick in-editor default.
TEST_CMD=(env SHCL_FUZZ_ITERS=20000 cargo test -j "${CPU_CAP}" --manifest-path "${MANIFEST}")
TEST_EXTRA=(
	'go -C source/go test ./...'
	'python3 source/python/tests/conformance.py'
	'cbin="$(mktemp)"; cc -std=c11 -O2 -Wall -Wextra -Werror -Isource/c source/c/tests/conformance.c -o "${cbin}" -lm && "${cbin}" project/conformance; crc=$?; rm -f "${cbin}"; ((crc==0))'
	'vbin="$(mktemp)"; g++ -std=c++17 -O2 -Wall -Werror -Isource/c source/c/tests/veneer_smoke.cpp -o "${vbin}" -lm && "${vbin}"; vrc=$?; rm -f "${vbin}"; ((vrc==0))'
)

## Stage 4b: cross-binding differential check (crosscheck.bash). Every entry is
## "name|cli-path" (stage 2 has built them by then); the first is the reference.
## Every cicd run requires all bindings to agree byte for byte on the corpus
## plus a fuzz-dumped input set. XCHECK_GEN (eval'd, ${XCHECK_DUMP_DIR} exported
## by the engine) produces that input set; it only runs with two or more bindings.
BINDING_CLIS=(
	"rust|source/rust/target/debug/shcl"
	"go|source/go/shcl"
	"python|source/python/cmd/shcl/main.py"
	"c|source/c/shcl"
)
XCHECK_GEN='env SHCL_FUZZ_DUMP="${XCHECK_DUMP_DIR}" SHCL_FUZZ_ITERS=2000 SHCL_FUZZ_DUMP_MAX=500 cargo test -j "${CPU_CAP}" --manifest-path '"${MANIFEST}"' --test fuzz_smoke --quiet'

## Stage 5: profiler. Optimized-with-symbols build (cargo profile "profiling",
## feature-gated pprof sampler - never in a normal build), run over a workload big
## enough to sample meaningfully: the whole corpus + the demo config, repeated.
## Kernel perf is locked down on this box (perf_event_paranoid=3), hence the
## in-process sampler. PROFILE_WORKLOAD_GEN / PROFILE_RUN are eval'd by the engine
## with PROFILE_WORKLOAD / PROFILE_OUT / PROFILE_SECS exported.
PROFILE_ENABLE=1
PROFILE_SECS=8
PROFILE_BUILD_CMD=(cargo build --profile profiling --features profiling -j "${CPU_CAP}" --manifest-path "${MANIFEST}")
PROFILE_BIN="source/rust/target/profiling/${EXE_NAME}"
PROFILE_OUT_DIR="cicd/artifacts/profiling"   ## relative to repo root; gitignored
PROFILE_WORKLOAD_GEN='{ for i in $(seq 40); do cat project/conformance/*/input.shcl cicd/demo/app.shcl; echo; done; } > "${PROFILE_WORKLOAD}"'
PROFILE_RUN='SHCL_PROFILE_OUT="${PROFILE_OUT}" SHCL_PROFILE_SECS="${PROFILE_SECS}" "${PROFILE_BIN}" fmt "${PROFILE_WORKLOAD}" >/dev/null'

## Stage 6: native release + cross targets. One per line:
## "label|os-arch|artifact|command...". os-arch feeds the versioned artifact name
## (<exe>-<version>-<os-arch>[.exe]). macOS is deferred (no Apple SDK on this box).
RELEASE_NATIVE_CMD=(cargo build --release -j "${CPU_CAP}" --manifest-path "${MANIFEST}")
RELEASE_NATIVE_BIN="source/rust/target/release/${EXE_NAME}"
RELEASE_NATIVE_OSARCH="linux-x86_64"
CROSS_TARGETS=(
	"Windows x86_64 (mingw)|windows-x86_64|source/rust/target/x86_64-pc-windows-gnu/release/${EXE_NAME}.exe|cargo build --release -j \${CPU_CAP} --manifest-path ${MANIFEST} --target x86_64-pc-windows-gnu"
	"Linux ARM64 (zig)|linux-arm64|source/rust/target/aarch64-unknown-linux-gnu/release/${EXE_NAME}|cargo zigbuild --release -j \${CPU_CAP} --manifest-path ${MANIFEST} --target aarch64-unknown-linux-gnu"
	"Windows ARM64 (zig)|windows-arm64|source/rust/target/aarch64-pc-windows-gnullvm/release/${EXE_NAME}.exe|cargo zigbuild --release -j \${CPU_CAP} --manifest-path ${MANIFEST} --target aarch64-pc-windows-gnullvm"
)
RELEASE_ARTIFACT_DIR="cicd/artifacts/release"   ## relative to repo root; gitignored

## Stage 7: dogfood. Drop the freshly built native release binary into the first
## existing+writable dir below, under EXE_NAME, so the copy you launch by hand is
## always current (same fixed path the sister project uses). Only runs when a
## release binary got built - --ci builds none, so it no-ops there. Empty the list
## to skip. No sudo: a non-writable dest is passed over, not force-installed.
DOGFOOD_FIXED_DESTS=(
	"${HOME}/synced/0-0/common/exec/util/linux/bin"
	"/usr/local/sbin"
)
## Shell wrappers ride along, each renamed to <EXE_NAME>.<ext>. A missing source is
## skipped silently. Each has its own preferred dest list (first existing+writable
## wins), matched by file extension below - a wrapper is a sourceable include, so it
## belongs with its kind, not on PATH beside the binary; falls back to the binary's
## dest if none of its own exist.
DOGFOOD_WRAPPERS=(
	"source/bash/shcl.bash"
	"source/powershell/shcl.ps1"
)
DOGFOOD_WRAPPER_DESTS_bash=(
	"${HOME}/synced/0-0/common/exec/util/linux/bash/include"
)
DOGFOOD_WRAPPER_DESTS_ps1=(
	"${HOME}/synced/0-0/common/exec/util/0_crossplatform"
)

## Stage 8: demo gif for the README. Rendered from a scripted scenario against the
## fresh native release binary, so the captured output can never go stale.
GIF_ENABLE=1
GIF_SCENARIO="cicd/demo-scenario.toml"
GIF_OUT="assets/demo.gif"
## Every render is kept, gfs-rotated, in the synced (non-repo) private tree; the
## newest is copied onto GIF_OUT above. The ../ leaves the repo root on purpose -
## private/ is a sibling symlink, so rotated gifs never touch the tracked tree.
GIF_ROTATE_DIR="../private/demos/gif"

## Full-run output is tee'd here (gitignored) and gfs-rotated; lint-report.bash
## surfaces warnings from the newest log at session start.
LINT_LOG_DIR="cicd/artifacts/lint"

## Stage 9: backup + publish via the standalone publisher (versioned RAR backup of
## the whole project dir, then stash/pull/pop, add, commit, push).
GIT_PUBLISH=(cicd/utility/n8git_backup-and-publish)
PUBLISH_AUTO_MESSAGE=""


##	History:
##		- 2026-07-11 JC: Created; all build/test stages stubbed pending the reference parser, shellcheck live from day one.
##		- 2026-07-12 JC: Wired the real stages: cargo fmt/clippy/test with the fuzz env, release + three cross targets, artifacts dir, demo gif scenario, n8git publish.
##		- 2026-07-12 JC: Added the profiler settings + binding list for the crosscheck; version guard dropped.
##		- 2026-07-12 JC: Go binding wired in: gofmt/build/vet/test extras + second crosscheck entry.
##		- 2026-07-12 JC: Python binding wired in: py_compile build gate + conformance test extra + third crosscheck entry.
##		- 2026-07-13 JC: Dogfood stage: install the native release binary (+ bash wrapper when it exists) to a fixed local dir.
##		- 2026-07-13 JC: C binding wired in: cc build extra (-Werror) + conformance/veneer test extras + fourth crosscheck entry.
##		- 2026-07-18 JC: Lint stage widened: ruff + mypy, cppcheck, markdownlint, PSScriptAnalyzer, with tool pins.
