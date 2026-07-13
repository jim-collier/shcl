#!/usr/bin/env bash

#  shellcheck disable=1091  ## 'source is valid here, but shellcheck doesn't know the path to it.'
#  shellcheck disable=2016  ## 'Expressions don't expand in single quotes, use double quotes for that.' I know, and I often want an explicit '$'.
#  shellcheck disable=2034  ## 'variable appears unused.' Complains about valid use of variable indirection.
#  shellcheck disable=2086  ## 'Double quote to prevent globbing and word splitting.' (OK for integers.)
#  shellcheck disable=2153  ## 'Possible misspelling.' False hit on vars assigned in the sourced config.bash.
#  shellcheck disable=2154  ## 'referenced but not assigned.' False hit on trap strings that assign the var they use (rc=$?).
#  shellcheck disable=2155  ## 'Declare and assign separately to avoid masking return values.' Cumbersome and unnecessary.
#  shellcheck disable=2317  ## 'Can't reach.' (I.e. an 'exit' is used for debugging - and makes an unusable visual mess.)

##	- Purpose: Local CI/CD pipeline. Generic engine, per-project settings live in config.bash.
##	- Stages (fail-fast, any error aborts before the next stage):
##	   1. format (in place locally; check-only under --ci)
##	   2. build (debug - what the tests run against)
##	   3. lint (project lint + shellcheck of the cicd scripts themselves)
##	   4. tests (conformance corpus + fuzz smoke + cross-binding differential check)
##	   5. profiler (flamegraph SVG + hot-spot report; non-gating artifact, local only)
##	   6. release + cross-compile + versioned artifacts + sha256sums (local only)
##	   7. demo gif for the README (local only; non-gating)
##	   8. backup + publish to git (local only)
##	- Syntax:
##	  cicd/cicd.bash [options]
##	  Options:
##	   --ci                correctness gate only: format check (no rewrite), build,
##	                       lint, tests; non-interactive, no cross/publish. This is
##	                       what the GitHub workflow runs - one definition of "passing".
##	   --quick             skip the slow stages: cross-compile, profiler, demo gif
##	   -q, --quiet         quiet + unattended (no prompt)
##	   -y, --yes           unattended (no prompt) but not quiet
##	   -m, --message MSG   publish hands-off with this commit message (no editor)
##	   --no-fmt            skip the formatter stage
##	   --no-lint           skip the lint stage
##	   --no-cross          skip the cross-compile targets (native release still builds)
##	   --no-profile        skip the profiler stage
##	   --no-gif            skip the demo gif refresh
##	   --no-publish        skip the git backup + publish stage
##	   -h, --help          show this help
##	- If neither -q/-y nor -m is given, the run prompts once for a commit message
##	  (blank = git editor; Ctrl+C aborts the whole run), then finishes unattended.
##	- Reuse: copy the cicd/ directory into another project and edit config.bash.

##	History: At bottom of script.

##	Copyright © 2026 Jim Collier (ID: 1cv◂‡Vᛦ)
##	Licensed under The MIT License (MIT). Full text at:
##		https://mit-license.org/
##	SPDX-License-Identifier: MIT


set -Eeuo pipefail

## Find the repo root and load project config.
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "${here}/.." && pwd)"   ## the git repo root (cicd/..)

## The rustup-routed toolchain must win over any system rust, or the pin in
## rust-toolchain.toml is meaningless and target/ fills with mixed artifacts.
export PATH="${HOME}/.cargo/bin:${PATH}"

## Cap every stage at 50% of cores; config command arrays can consume CPU_CAP.
_cores="$(nproc 2>/dev/null || echo 2)"
CPU_CAP=$(( _cores / 2 )); (( CPU_CAP < 1 )) && CPU_CAP=1
export CPU_CAP

## shellcheck source=config.bash
source "${here}/config.bash"
source "${here}/utility/include/gfs-rotate.bash"   ## gfs_rotate() for log/artifact pruning
cd "${root}"
stamp="$(date +%Y%m%d-%H%M%S)"

## Parse options.
assume_yes=0; quiet=0; ci_mode=0; quick=0; cli_message=""
while (($#)); do case "$1" in
	--ci)                     ci_mode=1; assume_yes=1; shift ;;
	--quick)                  quick=1; shift ;;
	-q|--quiet)               quiet=1; assume_yes=1; shift ;;
	-y|--yes)                 assume_yes=1; shift ;;
	--no-fmt)                 FMT_CMD=(); FMT_CHECK_CMD=(); shift ;;
	--no-lint)                LINT_CMD=(); SHELLCHECK_TARGETS=(); shift ;;
	--no-cross)               CROSS_TARGETS=(); shift ;;
	--no-profile)             PROFILE_ENABLE=0; shift ;;
	--no-gif)                 GIF_ENABLE=0; shift ;;
	--no-publish)             GIT_PUBLISH=(); shift ;;
	--message=*|--msg=*|-m=*) cli_message="${1#*=}"; shift ;;
	-m|--message|--msg)       cli_message="${2-}"; shift; (($#)) && shift ;;
	-h|--help)                sed -n '/^##	- Purpose:/,/^##	History:/p' "${BASH_SOURCE[0]}" | sed '$d; s/^##	\{0,1\}//'; exit 0 ;;
	*) echo "unknown option: $1 (try --help)" >&2; exit 2 ;;
esac; done

## --ci: correctness only, deterministic, side-effect free.
if ((ci_mode)); then
	FMT_CMD=()          ## check-only via FMT_CHECK_CMD; never rewrite in CI
	CROSS_TARGETS=()
	RELEASE_NATIVE_CMD=()
	PROFILE_ENABLE=0
	GIF_ENABLE=0
	GIT_PUBLISH=()
else
	FMT_CHECK_CMD=()    ## locally the formatter rewrites in place instead
fi
if ((quick)); then
	CROSS_TARGETS=()
	PROFILE_ENABLE=0
	GIF_ENABLE=0
fi

## Publish commit message: -m wins, then config, then a default when unattended.
publish_msg=""
if   [[ -n "$cli_message" ]];              then publish_msg="$cli_message"
elif [[ -n "${PUBLISH_AUTO_MESSAGE:-}" ]]; then publish_msg="$PUBLISH_AUTO_MESSAGE"
elif ((assume_yes));                       then publish_msg="${APP_NAME} CI/CD ${stamp}"
fi

## Output helpers: fEcho / fEcho_Clean, blank-collapsing.
declare -i _wasLastEchoBlank=0
fEcho_ResetBlankCounter(){ _wasLastEchoBlank=0; }
fEcho_Clean(){ if [[ -n "${1:-}" ]]; then echo -e "$*"; _wasLastEchoBlank=0; elif [[ $_wasLastEchoBlank -eq 0 ]] && echo; then _wasLastEchoBlank=1; fi; }
fEcho(){       if [[ -n "$*"     ]]; then fEcho_Clean "[ $* ]"; else fEcho_Clean ""; fi; }
fEcho_Force(){ fEcho_ResetBlankCounter; fEcho "$*"; }
_letterbox="••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••"
fSection(){ fEcho_Clean; fEcho_Clean "${_letterbox}"; fEcho "$*"; }
fDie(){ { fEcho_Force "FAILED: $*"; } >&2; exit 1; }
trap 'rc=$?; printf "\n[ CICD ABORTED (exit %s) at line %s: %s ]\n" "$rc" "$LINENO" "$BASH_COMMAND" >&2; exit $rc' ERR

## Current version from the single canonical source.
fVersion(){ sed -n 's/^version *= *"\(.*\)".*/\1/p' "${root}/${VERSION_MANIFEST}" | head -1; }

## (Re)write the sha256sums file over every artifact in the release dir except
## the sums file itself.
fWriteSums(){
	[[ -n "${art_dir:-}" && -d "${art_dir:-/nonexist}" ]] || return 0
	( cd "${art_dir}"
	  files=(); for x in "${EXE_NAME}-${ver}-"*; do [[ "$x" == "$sums" || ! -f "$x" ]] && continue; files+=("$x"); done
	  ((${#files[@]})) && sha256sum "${files[@]}" > "${sums}" )
}

## Preflight: show the plan, then capture the commit message so the rest runs unattended.
if ((! quiet)); then
	fEcho_Clean
	fEcho_Clean "${APP_NAME} $( ((ci_mode)) && echo 'CI gate' || echo 'local CI/CD')"
	fEcho_Clean
	fEcho_Clean "Repo root ......: ${root}"
	fEcho_Clean "Format .........: $( ((ci_mode)) && echo "${FMT_CHECK_CMD[*]:-(none configured)}" || echo "${FMT_CMD[*]:-(skipped)}")"
	fEcho_Clean "Build (debug) ..: ${BUILD_CMD[*]:-(none configured)}"
	fEcho_Clean "Lint ...........: ${LINT_CMD[*]:-(none configured)}  + shellcheck: ${#SHELLCHECK_TARGETS[@]} file(s)"
	fEcho_Clean "Tests ..........: ${TEST_CMD[*]:-(none configured)}  + crosscheck: ${#BINDING_CLIS[@]} binding(s)"
	fEcho_Clean "Profiler .......: $( ((PROFILE_ENABLE)) && echo "${PROFILE_SECS}s run -> flamegraph SVG -> ${PROFILE_OUT_DIR}/" || echo '(skipped)')"
	if ((${#RELEASE_NATIVE_CMD[@]})); then
		fEcho_Clean "Release ........: native + ${#CROSS_TARGETS[@]} cross target(s) -> ${RELEASE_ARTIFACT_DIR}/"
	else
		fEcho_Clean "Release ........: (skipped)"
	fi
	fEcho_Clean "Demo gif .......: $( ((GIF_ENABLE)) && echo "${GIF_OUT}" || echo '(skipped)')"
	if ((${#GIT_PUBLISH[@]})); then
		fEcho_Clean "Publish (last) .: ${GIT_PUBLISH[*]}$( [[ -n "$publish_msg" ]] && echo " (hands-off: \"${publish_msg}\")" || echo ' (will prompt for message; blank = editor)')"
	else
		fEcho_Clean "Publish (last) .: (skipped)"
	fi
	fEcho_Clean
	fEcho_Clean "Fail-fast: any error aborts before the next stage."
	fEcho_Clean
fi
if ((! assume_yes)) && ((${#GIT_PUBLISH[@]})) && [[ -z "$publish_msg" ]]; then
	read -r -p "Publish commit message (blank = editor; Ctrl+C aborts): " m
	fEcho_ResetBlankCounter
	[[ -n "$m" ]] && publish_msg="$m"
fi

## Tee the whole run to a rotated log so warnings can be reviewed later
## (lint-report.bash reads the newest one). Skipped under --ci (read-only gate).
if ((! ci_mode)) && [[ -n "${LINT_LOG_DIR:-}" ]] && mkdir -p "${root}/${LINT_LOG_DIR}" 2>/dev/null; then
	gfs_rotate "${root}/${LINT_LOG_DIR}" run log >/dev/null 2>&1 || true
	exec > >(tee "${root}/${LINT_LOG_DIR}/run_${stamp}.log") 2>&1
fi

## Warn (non-gating) when a pinned helper tool has drifted from TOOL_PINS, so a
## box update can't silently change results. Format: "name|version|command...".
if declare -p TOOL_PINS &>/dev/null; then
	for pin in "${TOOL_PINS[@]}"; do
		pin_name="${pin%%|*}"; rest="${pin#*|}"; pin_ver="${rest%%|*}"; pin_cmd="${rest#*|}"
		have="$(${pin_cmd} 2>/dev/null | head -1 || true)"
		if [[ -z "$have" ]]; then
			fEcho "WARNING: pinned tool missing: ${pin_name} (want ${pin_ver}); its stage will skip or fail"
		elif [[ "$have" != *"${pin_ver}"* ]]; then
			fEcho "WARNING: ${pin_name} drifted from pin ${pin_ver}: ${have}"
		fi
	done
fi

## Stage 1: format. In place locally; check-only (fail on diff) under --ci.
fSection "1/8  Format"
if ((ci_mode)); then
	if ((${#FMT_CHECK_CMD[@]})); then "${FMT_CHECK_CMD[@]}"; fEcho "OK: format clean"
	else fEcho_Clean "no format check configured"; fi
elif ((${#FMT_CMD[@]})); then
	"${FMT_CMD[@]}"; fEcho "OK: formatted"
else
	fEcho_Clean "format skipped"
fi

## Stage 2: debug build (fast compile sanity; the tests run against this).
fSection "2/8  Build (debug)"
if ((${#BUILD_CMD[@]})); then
	"${BUILD_CMD[@]}"; fEcho "OK: build"
else
	fEcho_Clean "nothing to build yet"
fi

## Stage 3: lint. Project lint first, then shellcheck over the cicd scripts (and
## any other bash config lists), so the pipeline can't rot silently. shellcheck
## is optional locally (warn if missing) but present on GitHub runners.
fSection "3/8  Lint"
if ((${#LINT_CMD[@]})); then
	"${LINT_CMD[@]}"; fEcho "OK: lint clean"
else
	fEcho_Clean "no project lint configured yet"
fi
if ((${#SHELLCHECK_TARGETS[@]})); then
	if command -v shellcheck >/dev/null 2>&1; then
		shellcheck "${SHELLCHECK_TARGETS[@]}"
		fEcho "OK: shellcheck clean (${#SHELLCHECK_TARGETS[@]} file(s))"
	elif ((ci_mode)); then
		fDie "shellcheck required under --ci but not found"
	else
		fEcho "WARNING: shellcheck not installed; skipped"
	fi
fi

## Stage 4: tests (conformance corpus + fuzz smoke + anything else config wires in),
## then the cross-binding differential check: every binding CLI must agree with
## every other, byte for byte, on the corpus AND on a freshly fuzz-dumped input set.
## With one binding it is a no-op note; it gets teeth the day a second binding lands.
fSection "4/8  Tests"
if ((${#TEST_CMD[@]})); then
	"${TEST_CMD[@]}"; fEcho "OK: tests passed"
else
	fEcho_Clean "no tests configured yet"
fi
if ((${#BINDING_CLIS[@]})); then
	xcheck_extra=()
	if ((${#BINDING_CLIS[@]} >= 2)) && [[ -n "${XCHECK_GEN:-}" ]]; then
		export XCHECK_DUMP_DIR="${root}/cicd/artifacts/crosscheck"
		rm -rf "${XCHECK_DUMP_DIR}"; mkdir -p "${XCHECK_DUMP_DIR}"
		eval "${XCHECK_GEN}"
		xcheck_extra=(--extra "${XCHECK_DUMP_DIR}")
	fi
	"${here}/utility/crosscheck.bash" --corpus "${root}/project/conformance" "${xcheck_extra[@]}" "${BINDING_CLIS[@]}"
fi

## Stage 5: profiler. Non-gating artifact, not a pass/fail test: an optimized
## build with symbols runs a heavy workload under an in-process sampler (kernel
## perf is locked down on this box) and writes a flamegraph SVG, gfs-rotated like
## the run logs; flame-report.py prints the hot spots into the log. Environmental
## problems skip with a warning; a build or run failure is the app's fault -> die.
fSection "5/8  Profiler"
if ((PROFILE_ENABLE)); then
	fEcho_Clean "building: ${PROFILE_BUILD_CMD[*]}"
	"${PROFILE_BUILD_CMD[@]}" || fDie "profiler build failed (app problem)"
	[[ -f "${PROFILE_BIN}" ]] || fDie "profiler binary missing: ${PROFILE_BIN}"
	profile_dir="${root}/${PROFILE_OUT_DIR}"
	mkdir -p "${profile_dir}"
	export PROFILE_WORKLOAD="${profile_dir}/workload.shcl"
	eval "${PROFILE_WORKLOAD_GEN}"
	[[ -s "${PROFILE_WORKLOAD}" ]] || fDie "profiler workload came out empty: ${PROFILE_WORKLOAD}"
	## Born canonical (role "frequent"); the rotation retags the newest as "latest".
	export PROFILE_OUT="${profile_dir}/flame_${stamp}_frequent.svg"
	fEcho_Clean "sampling ${PROFILE_SECS}s: ${PROFILE_RUN}"
	eval "${PROFILE_RUN}" || fDie "profiler run failed (non-zero exit - app problem)"
	[[ -s "${PROFILE_OUT}" ]] || fDie "profiler produced no SVG (app problem): ${PROFILE_OUT}"
	gfs_rotate "${profile_dir}" flame svg
	latest="${profile_dir}/flame_${stamp}_latest.svg"
	[[ -e "$latest" ]] || latest="${PROFILE_OUT}"
	fEcho "OK: flamegraph: ${latest}"
	fEcho_Clean "open: ${latest}  (in a browser)"
	## Hot-spot summary into the log (non-fatal, no marker - the marker belongs to
	## the per-session --check gate, not the pipeline).
	if command -v python3 >/dev/null 2>&1; then
		fEcho_Clean ""
		python3 "${here}/utility/flame-report.py" --dir "${profile_dir}" 2>/dev/null || fEcho_Clean "hot spots: (report unavailable)"
	else
		fEcho "WARNING: python3 not found; hot-spot report skipped"
	fi
else
	fEcho_Clean "profiler skipped"
fi

## Stage 6: native release + cross targets, collected under versioned names plus
## a sha256 checksums file, ready to attach to a release as plain uploads.
## Naming (stable; download links depend on it): <exe>-<version>-<os-arch>[.exe]
fSection "6/8  Release builds"
if ((${#RELEASE_NATIVE_CMD[@]})); then
	"${RELEASE_NATIVE_CMD[@]}"
	[[ -f "${RELEASE_NATIVE_BIN}" ]] || fDie "native release binary missing: ${RELEASE_NATIVE_BIN}"
	fEcho "OK: native release: ${RELEASE_NATIVE_BIN} ($(du -h "${RELEASE_NATIVE_BIN}" | cut -f1))"
	built_arts=("${RELEASE_NATIVE_OSARCH:-native}|${RELEASE_NATIVE_BIN}")
	for t in "${CROSS_TARGETS[@]}"; do
		t_label="${t%%|*}"; rest="${t#*|}"; t_osarch="${rest%%|*}"; rest="${rest#*|}"; t_art="${rest%%|*}"; t_cmd="${rest#*|}"
		fEcho "cross: ${t_label}"
		eval "${t_cmd}"
		[[ -f "${t_art}" ]] || fDie "missing artifact for ${t_label}: ${t_art}"
		fEcho "OK: ${t_label}: ${t_art} ($(du -h "${t_art}" | cut -f1))"
		built_arts+=("${t_osarch}|${t_art}")
	done
	if [[ -n "${RELEASE_ARTIFACT_DIR:-}" ]]; then
		ver="$(fVersion)"
		[[ -n "$ver" ]] || fDie "no version found in ${VERSION_MANIFEST}"
		art_dir="${root}/${RELEASE_ARTIFACT_DIR}"
		rm -rf "${art_dir}"; mkdir -p "${art_dir}"
		sums="${EXE_NAME}-${ver}-sha256sums.txt"
		for pair in "${built_arts[@]}"; do
			p_osarch="${pair%%|*}"; p_src="${pair#*|}"
			p_ext=""; [[ "$p_src" == *.exe ]] && p_ext=".exe"
			cp -f "${p_src}" "${art_dir}/${EXE_NAME}-${ver}-${p_osarch}${p_ext}"
		done
		fWriteSums
		fEcho "OK: ${#built_arts[@]} release artifact(s) + ${sums} -> ${RELEASE_ARTIFACT_DIR}/"
		((${#CROSS_TARGETS[@]})) || fEcho_Clean "note: cross targets skipped - artifact set is partial (native only)"
	fi
else
	fEcho_Clean "release builds skipped"
fi

## Stage 7: demo gif for the README (non-gating: a missing Pillow/font/etc. skips).
fSection "7/8  Demo gif"
if ((GIF_ENABLE)); then
	if "${here}/utility/gen-demo-gif.py" --scenario "${root}/${GIF_SCENARIO}" --out "${root}/${GIF_OUT}" --bin "${root}/${RELEASE_NATIVE_BIN}" --quiet; then
		fEcho "OK: demo gif -> ${GIF_OUT} ($(du -h "${root}/${GIF_OUT}" | cut -f1))"
	else
		fEcho "WARNING: demo gif skipped (non-fatal)"
	fi
else
	fEcho_Clean "demo gif skipped"
fi

## Stage 8: backup + publish via the standalone publisher.
fSection "8/8  Backup + publish"
if ((${#GIT_PUBLISH[@]})); then
	command -v rar >/dev/null 2>&1 || export GIT_BACKUP_AND_PUBLISH_NOBACKUP=1
	if [[ -n "$publish_msg" ]]; then
		## Hands-off: quiet env skips the publisher's continue-prompt; the
		## GIT_EDITOR helper fills the message so `git commit` won't open an editor.
		GIT_BACKUP_AND_PUBLISH_QUIET=1 GIT_AUTO_MESSAGE="${publish_msg}" \
			GIT_EDITOR="${here}/utility/git-auto-msg.bash" "${GIT_PUBLISH[@]}" --quiet
	else
		"${GIT_PUBLISH[@]}" --quiet
	fi
	fEcho "OK: published"
else
	fEcho_Clean "publish skipped"
fi

fSection "${APP_NAME} CI/CD: done."
fEcho_Clean


##	History:
##		- 2026-07-11 JC: Created from the sister-project engine, trimmed to what shcl needs pre-code; added --ci gate mode shared with the GitHub workflow.
##		- 2026-07-12 JC: Filled out for the reference parser: debug+release split, cross targets + versioned artifacts + sha256sums, run-log tee with gfs rotation, tool-pin drift warnings, demo gif stage, dev version guard, publish via n8git_backup-and-publish.
##		- 2026-07-12 JC: Dropped the dev version guard; added the profiler stage (flamegraph + flame-report) and the cross-binding crosscheck after tests.
