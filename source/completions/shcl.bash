#••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••
## shcl.bash (bash completion)
##
##	Tab completion for the shcl CLI. Installed as
##	<datadir>/bash-completion/completions/shcl, or sourced by hand:
##		source shcl.bash
##
##	The option table below mirrors check_opts() in the CLI, one arm per
##	subcommand. cicd/utility/check-completions.bash diffs the two, so an option
##	added to the CLI without a line here fails the build.
#••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••

##	Copyright © 2026 Jim Collier (CryptogID: ѳ6ᴚ℈𐀘𐇦ɛ𐊁¥Mﾏb϶Δ𐌞)
##	Licensed under The MIT License (MIT). Full text at:
##		https://mit-license.org/
##	SPDX-License-Identifier: MIT

## Subcommands. The CLI takes exactly one, and always as the first word.
_shcl_subcommands='get set fmt check init count instances children paths help version about donate'

## Type options, valid on `get` only. The table below carries them as the single
## token --<type>, exactly as the CLI's own table does.
_shcl_types='--int --float --bool --datetime --string --raw --rawinfo'

## Options that take a value, in either spelling. Mirrors the list asked_for()
## steps over in the CLI; check-completions.bash diffs the two.
_shcl_valopts='--default --on-bad --strictness --schema --layer --set --set-literal --set-default --set-literal-default --remove'

## Options each subcommand accepts. Anything else is a usage error to the CLI,
## so offering it here would be a lie.
_shcl_opts() {
	case "$1" in
		get)             echo '--<type> --array --slots --default --on-bad --strictness --layer --set --set-literal --set-default --set-literal-default --remove' ;;
		set)             echo '--strictness --layer --set --set-literal --set-default --set-literal-default --remove --write --lossy' ;;
		fmt)             echo '--write --lossy --strictness --layer --set --set-literal --set-default --set-literal-default --remove' ;;
		check)           echo '--strictness --schema' ;;
		init)            echo '--schema --no-banner' ;;
		count|instances|children|paths) echo '--strictness --layer --set --set-literal --set-default --set-literal-default --remove' ;;
		*)               echo '' ;;
	esac
}

## Positional file slot per subcommand: the argument number FILE occupies, or 0
## for the subcommands that take none. Everything after FILE is a PATH, which
## nothing here can enumerate - so it completes to nothing rather than to
## filenames that would always be wrong.
_shcl_fileslot() {
	case "$1" in
		get|set|fmt|check|count|instances|children|paths) echo 1 ;;
		*)                                 echo 0 ;;
	esac
}

_shcl() {
	## split: older bash-completion releases hand it back from -s; declared so
	## it never leaks into the shell.
	# shellcheck disable=SC2034
	local cur prev cmd opts i word positional fileslot cword split
	local -a words

	## bash has already cut `--opt=value` into three words here, because `=`
	## is in COMP_WORDBREAKS. bash-completion's _init_completion -s joins them
	## back, with prev the option and cur the value; the fallback does the
	## same by hand, for a hand-sourced copy on a box without it.
	if declare -F _init_completion >/dev/null; then
		_init_completion -s || return
	else
		_shcl_words
	fi

	## Word 1 is the subcommand and nothing else.
	if (( cword == 1 )); then
		mapfile -t COMPREPLY < <(compgen -W "${_shcl_subcommands} -h --help -v -V --version --about --donate" -- "${cur}")
		return
	fi
	cmd="${words[1]}"

	## Value options, either spelling: the next word is the value, whatever it
	## looks like, so complete for the option rather than for the position.
	case "${prev}" in
		--strictness)     mapfile -t COMPREPLY < <(compgen -W 'loose standard strict 1 2 3' -- "${cur}"); return ;;
		--on-bad)         mapfile -t COMPREPLY < <(compgen -W 'error default flag' -- "${cur}"); return ;;
		--schema|--layer) _shcl_files "${cur}"; return ;;
	esac
	## The rest take a PATH, or a value nothing here can enumerate.
	[[ " ${_shcl_valopts} " == *" ${prev} "* ]] && return

	if [[ "${cur}" == -* ]]; then
		opts="$(_shcl_opts "${cmd}")"
		[[ "${opts}" == *'--<type>'* ]] && opts="${opts//--<type>/${_shcl_types}}"
		## -w is the only short option; the CLI takes no other.
		[[ "${opts}" == *'--write'* ]] && opts="${opts} -w"
		mapfile -t COMPREPLY < <(compgen -W "${opts} -h --help" -- "${cur}")
		return
	fi

	## A positional. Count the ones already given, skipping options and the
	## values that follow them in the space form (the =VALUE form is one word).
	positional=0
	for (( i = 2; i < cword; i++ )); do
		word="${words[i]}"
		if [[ " ${_shcl_valopts} " == *" ${word} "* ]]; then
			(( i++ ))
		elif [[ "${word}" != -* ]]; then
			(( positional++ ))
		fi
	done
	fileslot="$(_shcl_fileslot "${cmd}")"
	(( fileslot && positional + 1 == fileslot )) && _shcl_files "${cur}"
}

## Without bash-completion: rejoin what bash cut at `=`, then split the current
## word the way _init_completion -s does. Sets words, cword, cur and prev.
_shcl_words() {
	local w
	words=()
	for (( i = 0; i <= COMP_CWORD; i++ )); do
		w="${COMP_WORDS[i]}"
		if (( ${#words[@]} )) && [[ "${w}" == "=" && "${words[-1]}" == --* && "${words[-1]}" != *=* ]]; then
			words[-1]+="="
		elif (( ${#words[@]} )) && [[ "${words[-1]}" == --*= && "${COMP_WORDS[i-1]}" == "=" ]]; then
			words[-1]+="${w}"
		else
			words+=("${w}")
		fi
	done
	cword=$(( ${#words[@]} - 1 ))
	cur="${words[cword]}"
	prev="${words[cword-1]}"
	if [[ "${cur}" == --?*=* ]]; then
		prev="${cur%%=*}"
		cur="${cur#*=}"
	fi
}

## Filename completion. bash-completion's _filedir handles quoting, dirs and
## the compopt dance properly; the compgen fallback is for a hand-sourced copy
## on a box without it.
_shcl_files() {
	if declare -F _filedir >/dev/null; then
		_filedir
		return
	fi
	compopt -o filenames 2>/dev/null
	mapfile -t COMPREPLY < <(compgen -f -- "$1")
}

complete -F _shcl shcl
