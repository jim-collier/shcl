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

## Pinned versions of the cargo-installed helpers the pipeline uses; the engine
## warns (non-gating) when an installed tool has drifted from its pin, so a box
## update can't silently change results. Format: "name|version|command...".
## The rustc/clippy toolchain itself is pinned by rust-toolchain.toml at repo root.
TOOL_PINS=(
	"cargo-zigbuild|0.23.0|cargo-zigbuild --version"
)

## Stage 2: debug build (what the tests exercise). Capped at half the cores.
## The Go binding builds here too - crosscheck needs its CLI in stage 4.
BUILD_CMD=(cargo build -j "${CPU_CAP}" --manifest-path "${MANIFEST}")
BUILD_EXTRA=('go -C source/go build -o shcl ./cmd/shcl')

## Stage 3: lint. clippy gates (-D warnings); shellcheck covers the pipeline's own
## scripts so cicd can't rot silently.
LINT_CMD=(cargo clippy -j "${CPU_CAP}" --manifest-path "${MANIFEST}" --all-targets -- -D warnings)
LINT_EXTRA=('go -C source/go vet ./...')
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
)

## Stage 4: tests. cargo test runs the conformance corpus (project/conformance/)
## plus the deterministic fuzz smoke; the env var raises the fuzz iteration count
## well past the quick in-editor default.
TEST_CMD=(env SHCL_FUZZ_ITERS=20000 cargo test -j "${CPU_CAP}" --manifest-path "${MANIFEST}")
TEST_EXTRA=('go -C source/go test ./...')

## Stage 4b: cross-binding differential check (crosscheck.bash). Every entry is
## "name|cli-path" (stage 2 has built them by then); the first is the reference.
## Every cicd run requires all bindings to agree byte for byte on the corpus
## plus a fuzz-dumped input set. XCHECK_GEN (eval'd, ${XCHECK_DUMP_DIR} exported
## by the engine) produces that input set; it only runs with two or more bindings.
BINDING_CLIS=(
	"rust|source/rust/target/debug/shcl"
	"go|source/go/shcl"
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

## Stage 7: demo gif for the README. Rendered from a scripted scenario against the
## fresh native release binary, so the captured output can never go stale.
GIF_ENABLE=1
GIF_SCENARIO="cicd/demo-scenario.toml"
GIF_OUT="assets/demo.gif"

## Full-run output is tee'd here (gitignored) and gfs-rotated; lint-report.bash
## surfaces warnings from the newest log at session start.
LINT_LOG_DIR="cicd/artifacts/lint"

## Stage 8: backup + publish via the standalone publisher (versioned RAR backup of
## the whole project dir, then stash/pull/pop, add, commit, push).
GIT_PUBLISH=(cicd/utility/n8git_backup-and-publish)
PUBLISH_AUTO_MESSAGE=""


##	History:
##		- 2026-07-11 JC: Created; all build/test stages stubbed pending the reference parser, shellcheck live from day one.
##		- 2026-07-12 JC: Wired the real stages: cargo fmt/clippy/test with the fuzz env, release + three cross targets, artifacts dir, demo gif scenario, n8git publish.
##		- 2026-07-12 JC: Added the profiler settings + binding list for the crosscheck; version guard dropped.
##		- 2026-07-12 JC: Go binding wired in: gofmt/build/vet/test extras + second crosscheck entry.
