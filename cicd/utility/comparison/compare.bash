#!/usr/bin/env bash

##	Purpose:
##		Measure SHCL against JSON, YAML, TOML and XML on documents that hold the
##		same data in each format, at a size where parser behavior actually shows.
##		The README makes claims about how SHCL compares; this is where the
##		numbers behind them come from, and rerunning it is how they stay honest.
##
##		The comparison holds the language constant at Rust, so every format is
##		read by a mature native crate built with the same compiler and flags.
##		Detailed results accumulate in results.shcl - written, pruned and read
##		back through this repo's own library, which is both the storage format
##		and the point.
##
##		Run on demand, never as a pipeline stage: benchmarks are noisy and slow,
##		and a red build caused by a busy machine teaches nobody anything. The
##		Rust crate beside this script is the only thing in the repo with
##		third-party dependencies, and it stays out of the gate for that reason
##		too - the gate must not need crates.io to pass.
##	Syntax:
##		compare.bash [--no-build] [tool options...]
##		  --no-build   use the binary already built
##		  --help       full option list from the tool itself
##	Exit: 0 = every measurement completed, 1 = one or more failed.
##	History: At bottom of script.

##	Copyright © 2026 Bubbles (ID: XଌฅრX۳ᛟԃლፀƅꓩหδლც)
##	Licensed under The MIT License (MIT). Full text at:
##		https://mit-license.org/
##	SPDX-License-Identifier: MIT


set -Eeuo pipefail

here="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
bin="${here}/target/release/shcl-comparison"

build=1
args=()
for a in "$@"; do
	case "${a}" in
		--no-build) build=0 ;;
		*) args+=("${a}") ;;
	esac
done

## rustup's cargo, so the pinned toolchain wins over whatever the system has.
export PATH="${HOME}/.cargo/bin:${PATH}"

if ((build)); then
	echo "[ Building the comparison tool ]"
	cargo build --release --manifest-path "${here}/Cargo.toml"
	echo
fi

[[ -x "${bin}" ]] || { echo "compare: no binary at ${bin} - drop --no-build" >&2; exit 1; }

exec "${bin}" "${args[@]}"


##	History:
##		- 2026-08-20 JC: Created, with the Rust tool beside it.
