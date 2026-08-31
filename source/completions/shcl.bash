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
	local cur prev cmd opts i word positional fileslot
	cur="${COMP_WORDS[COMP_CWORD]}"
	prev="${COMP_WORDS[COMP_CWORD-1]}"

	## Word 1 is the subcommand and nothing else.
	if (( COMP_CWORD == 1 )); then
		mapfile -t COMPREPLY < <(compgen -W "${_shcl_subcommands} -h --help -v -V --version --about --donate" -- "${cur}")
		return
	fi
	cmd="${COMP_WORDS[1]}"

	## Value options in the space form: the next word is the value, whatever it
	## looks like, so complete for the option rather than for the position.
	case "${prev}" in
		--strictness)          mapfile -t COMPREPLY < <(compgen -W 'loose standard strict 1 2 3' -- "${cur}"); return ;;
		--on-bad)              mapfile -t COMPREPLY < <(compgen -W 'error default flag' -- "${cur}"); return ;;
		--schema|--layer)      _shcl_files "${cur}"; return ;;
		--default|--set|--set-literal) return ;;
	esac
	## Same options in the =VALUE form.
	case "${cur}" in
		--strictness=*)   mapfile -t COMPREPLY < <(compgen -P '--strictness=' -W 'loose standard strict 1 2 3' -- "${cur#*=}"); return ;;
		--on-bad=*)       mapfile -t COMPREPLY < <(compgen -P '--on-bad=' -W 'error default flag' -- "${cur#*=}"); return ;;
		--schema=*)       _shcl_files "${cur#*=}" '--schema='; return ;;
		--layer=*)        _shcl_files "${cur#*=}" '--layer='; return ;;
	esac

	if [[ "${cur}" == -* ]]; then
		opts="$(_shcl_opts "${cmd}")"
		[[ "${opts}" == *'--<type>'* ]] && opts="${opts//--<type>/${_shcl_types}}"
		## -w is the only short option; the CLI takes no other.
		[[ "${opts}" == *'--write'* ]] && opts="${opts} -w"
		mapfile -t COMPREPLY < <(compgen -W "${opts} -h --help" -- "${cur}")
		return
	fi

	## A positional. Count the ones already given, skipping options and the
	## values that follow them in the space form.
	positional=0
	for (( i = 2; i < COMP_CWORD; i++ )); do
		word="${COMP_WORDS[i]}"
		case "${word}" in
			--default|--on-bad|--strictness|--schema|--layer|--set|--set-literal) (( i++ )) ;;
			-*) ;;
			*) (( positional++ )) ;;
		esac
	done
	fileslot="$(_shcl_fileslot "${cmd}")"
	(( fileslot && positional + 1 == fileslot )) && _shcl_files "${cur}"
}

## Filename completion. bash-completion's _filedir handles quoting, dirs and
## the compopt dance properly; the compgen fallback is for a hand-sourced copy
## on a box without it.
_shcl_files() {
	local pfx="${2-}"
	if [[ -z "${pfx}" ]] && declare -F _filedir >/dev/null; then
		_filedir
		return
	fi
	compopt -o filenames 2>/dev/null
	mapfile -t COMPREPLY < <(compgen -P "${pfx}" -f -- "$1")
}

complete -F _shcl shcl
