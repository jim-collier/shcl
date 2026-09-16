#!/usr/bin/env bash

##	Purpose:
##		Fail when shcl.h declares a public call that the C++ veneer neither
##		makes nor lists below with the reason it does not need to. The veneer
##		began as a read layer and fell behind C one addition at a time, with
##		nothing to say so, until it could load and save a document but not
##		change a value in it. A call counts as made when shcl.hpp names it
##		outside a comment, which also counts a function passed by pointer.
##		The list has to stay true as well: a listed call the veneer now makes,
##		or one shcl.h no longer declares, fails too.
##	Syntax:
##		check-veneer.bash
##	Exit: 0 = every public call is made or listed, 1 = one is not (named),
##	      2 = cannot read inputs.
##	History: At bottom of script.

##	Copyright (c) 2026 Bubbles
##	Licensed under The MIT License (MIT). Full text at:
##		https://mit-license.org/
##	SPDX-License-Identifier: MIT


set -Eeuo pipefail

srcDir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../source/c" && pwd)"
header="${srcDir}/shcl.h"
veneer="${srcDir}/shcl.hpp"
[[ -r "${header}" && -r "${veneer}" ]] || { echo "check-veneer: cannot read ${header} or ${veneer}" >&2; exit 2; }

## Public calls the veneer reaches another way, and how.
declare -A covered=(
	[shcl_get_int]="get_or<int64_t> reads through get<T> and checks the status itself"
	[shcl_get_float]="get_or<double> reads through get<T> and checks the status itself"
	[shcl_get_bool]="get_or<bool> reads through get<T> and checks the status itself"
	[shcl_get_int_or]="get_or<int64_t> reads through get<T> and checks the status itself"
	[shcl_get_float_or]="get_or<double> reads through get<T> and checks the status itself"
	[shcl_get_bool_or]="get_or<bool> reads through get<T> and checks the status itself"
)

## Declarations start at column 0 with their return type; everything above the
## implementation guard is the public half. A floor on the count, since a
## pattern that stopped matching would otherwise pass with nothing to check.
declared=()
while IFS= read -r name; do
	declared+=("${name}")
done < <(sed -n '1,/^#ifdef SHCL_IMPLEMENTATION/p' "${header}" | sed -nE 's/^[A-Za-z_][A-Za-z0-9_ *]*[ *](shcl_[a-z0-9_]+)\(.*/\1/p' | sort -u)
((${#declared[@]} >= 50)) || { echo "check-veneer: found ${#declared[@]} public calls in shcl.h, too few to trust; has the declaration style changed?" >&2; exit 2; }

declare -A made=()
while IFS= read -r name; do
	made["${name}"]=1
done < <(sed -E 's#//.*$##' "${veneer}" | { grep -oE 'shcl_[a-z0-9_]+' || true; } | sort -u)

declare -A isDeclared=()
rc=0
for name in "${declared[@]}"; do
	isDeclared["${name}"]=1
	if [[ -n "${made[${name}]:-}" ]]; then
		if [[ -n "${covered[${name}]:-}" ]]; then
			echo "check-veneer: shcl.hpp calls ${name}, which is also listed as reached another way; take it off the list" >&2; rc=1
		fi
	elif [[ -z "${covered[${name}]:-}" ]]; then
		echo "check-veneer: shcl.h declares ${name} and shcl.hpp never calls it; wrap it, or list it in this script with the reason" >&2; rc=1
	fi
done
for name in "${!covered[@]}"; do
	if [[ -z "${isDeclared[${name}]:-}" ]]; then
		echo "check-veneer: ${name} is listed as reached another way, and shcl.h no longer declares it" >&2; rc=1
	fi
done

((rc)) || echo "check-veneer: OK: each of the ${#declared[@]} public calls in shcl.h is made by shcl.hpp or listed with a reason (${#covered[@]})"
exit "${rc}"


##	History:
##		- 2026-09-16 JC: Created.
