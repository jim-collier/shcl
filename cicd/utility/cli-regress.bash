#!/usr/bin/env bash

##	Purpose:
##		Pin CLI behavior the conformance corpus structurally cannot reach: a
##		closed stdin or stdout, '-' named twice on one command line, a carriage
##		return at the end of an ops line, the shape of an op-script error, and
##		whether a read failure still names its cause. Plus the one thing about
##		the help text a diff between bindings cannot see: how wide it is. Every
##		row runs against every binding and is checked against a fixed
##		expectation, not against the other bindings - four-way agreement proves
##		parity, not correctness, and each of these was a defect all four shared.
##
##		stdout and the exit code are contract and are matched exactly. stderr
##		wording is per-binding voice, so a row matches it with a regex that has
##		to hold for all four.
##	Syntax:
##		cli-regress.bash NAME|CLI [NAME|CLI ...]
##	Exit: 0 = every row passes everywhere, 1 = a row failed, 2 = usage.
##	History: At bottom of script.

##	Copyright (c) 2026 Bubbles
##	Licensed under The MIT License (MIT). Full text at:
##		https://mit-license.org/
##	SPDX-License-Identifier: MIT


set -Eeuo pipefail

repoDir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
bindings=()
while (($#)); do case "$1" in
	-h|--help) grep -E '^##' "$0" | sed 's/^##\t\?//'; exit 0 ;;
	*)         bindings+=("$1"); shift ;;
esac; done
((${#bindings[@]})) || { echo "cli-regress: no bindings given" >&2; exit 2; }
for b in "${bindings[@]}"; do
	cli="${b#*|}"
	[[ -x "${cli}" ]] || { echo "cli-regress: binding CLI not executable: ${cli}" >&2; exit 2; }
done

##	The unwritable directory below has to be made writable again or the cleanup
##	cannot empty it.
tmpDir="$(mktemp -d)"; trap 'chmod -R u+w "${tmpDir}" 2>/dev/null; rm -rf "${tmpDir}"' EXIT
printf 'a: 1\n'          > "${tmpDir}/ok.shcl"
printf 'a: 1\n  bad\nb 2\n' > "${tmpDir}/bad.shcl"
## A second damaged file, so a layered load has two to tell apart.
printf 'c: 1\nalso bad\n' > "${tmpDir}/bad2.shcl"
mkdir -p "${tmpDir}/adir"
## The deepest a document can legally go: one level under the 512 cap. Python
## was the binding still recursing a frame per level, so this is the shape that
## would exhaust its stack.
awk 'BEGIN{ for (i = 0; i < 511; i++) { for (j = 0; j < i; j++) printf "\t"; printf "n%d:\n", i }
	for (j = 0; j < 511; j++) printf "\t"; printf "leaf: 1\n" }' > "${tmpDir}/deep.shcl"
## A schema whose own default breaks the field's constraints. Generation used to
## emit it anyway, so the starter config failed the schema that produced it.
printf 'field: server.port\n\ttype: int\n\trequired: yes\n\tmin: 1\n\tmax: 10\n\tdefault: 99\n' > "${tmpDir}/baddef.shcl"
## A must-exist path with no name to generate: repeat 1 is a must-exist bound
## like `required`, and generation cannot satisfy it, so the schema is faulted
## rather than a starter that fails its first check. Repeat 2 is the one
## documented shortfall and generates.
printf 'field: "*"\n\ttype: int\n\trepeat: 1\n' > "${tmpDir}/star1.shcl"
printf 'field: "*"\n\ttype: int\n\trepeat: 2\n' > "${tmpDir}/star2.shcl"
## The generation field ceiling, either side of it: the cap used to fire AT the
## limit while its message said past it.
awk 'BEGIN{ for (i = 0; i < 10000; i++) printf "field: f%d\n", i }' > "${tmpDir}/cap10000.shcl"
awk 'BEGIN{ for (i = 0; i < 10001; i++) printf "field: f%d\n", i }' > "${tmpDir}/cap10001.shcl"
## A generator-only `default` the schema cannot spell on a value line: a raw
## block has no inline form, and a `type: raw` field's default goes out inline
## and then fails its own type check. Both used to be dropped or reported as a
## wrong type in the generated output.
printf 'field: b\n\ttype: raw\n\tdefault: hello\n\trequired: yes\n' > "${tmpDir}/rawdef.shcl"
## A `desc` with a comma in it: the value is several elements, and the comment
## used to come out missing rather than carrying the sentence.
printf 'field: a\n\tdesc: one, two\n\trequired: yes\n' > "${tmpDir}/commadesc.shcl"
## A field name carrying a line break, and a flat name carrying a dot. Both used
## to be pasted into the diagnostic exactly as stored, so one hint arrived as
## three stderr lines and a flat name read just like nesting.
printf '"a\\nb": 1\n"a\\nb": 2\n' > "${tmpDir}/nbname.shcl"
## An indented malformed line behind a two-byte character, so the E014 column
## counts the indent and bytes rather than characters.
printf 'a:\n\t"\303\251" x\n' > "${tmpDir}/colbytes.shcl"
## A malformed line behind a blank run holding a carriage return, which is a
## blank and never indent. The column left the run out.
printf '\r  b[c: 2\n' > "${tmpDir}/colcr.shcl"
## An int-array whose second element breaks the max, and a float whose bound is
## integral. A range diagnostic used to name the field and nothing else.
printf 'field: ns\n\ttype: int-array\n\tmax: 10\nfield: fs\n\ttype: float-array\n\tmin: 1.0\n' > "${tmpDir}/range.shcl"
printf 'ns: 5, 20, 3\nfs: 2.0, 0.5, 4.0\n' > "${tmpDir}/outofrange.shcl"
printf '"x.y": 1\n' > "${tmpDir}/dotname.shcl"
## Schema paths and a type carrying a line break. Every code that names schema
## text printed it raw, so one diagnostic arrived as two stderr lines.
printf 'field: "a.\\"x\\ny\\""\n\trequired: yes\nfield: "b.\\"x\\ny\\""\n\ttype: int\n\tmin: 5\n\tmax: 6\n\tallowed: 5, 6, 1\n\trepeat: 3\nfield: "c.\\"x\\ny\\""\n\ttype: bool\n' > "${tmpDir}/nlschema.shcl"
printf 'b:\n\t"x\\ny": 9\n\t"x\\ny": 1\nc:\n\t"x\\ny": maybe\n' > "${tmpDir}/nldoc.shcl"
printf 'field: k\n\ttype: "in\\nt"\nfield: "d.\\"x\\ny\\"."\n' > "${tmpDir}/nlfault.shcl"
printf 'field: x.y\n\ttype: int\n' > "${tmpDir}/dotschema.shcl"
## A directory a write cannot create a temp file in. The phase is worth naming -
## it is the difference between "fix the file" and "fix its directory" - and the
## C CLI used to guess it from an access() that answers yes for every existing
## directory on windows.
mkdir -p "${tmpDir}/nowrite"
printf 'a: 1\n' > "${tmpDir}/nowrite/f.shcl"
chmod 500 "${tmpDir}/nowrite"
## A raw block against a string `allowed`: the body carries its own newlines, so
## one diagnostic used to span several stderr lines.
#  shellcheck disable=2016  ## the backticks are the fence the fixture needs.
printf 'b:\n\t```\n\tline one\n\tline two\n\t```\n' > "${tmpDir}/rawval.shcl"
printf 'field: b\n\ttype: string\n\tallowed: nope\n' > "${tmpDir}/rawvalschema.shcl"
## A schema whose own load has something to say: two `field: a` instances merge,
## so `allowed` repeats as a bare leaf - which is exactly what the V092 under it
## is about, and it was invisible.
printf 'field: a\n\tallowed: x\nfield: a\n\tallowed: y\n' > "${tmpDir}/hintschema.shcl"
## A must-exist path with nothing to generate from: an index selector needs an
## instance that is not there, and a path past the nesting cap would draw E016
## on the way back in. Either way the fault names the path rather than reporting
## the generated config as missing it.
printf 'field: "srv[#1].port"\n\trequired: yes\n' > "${tmpDir}/idxreq.shcl"
## Two must-exist paths nothing can generate, either side of a field that
## generates fine: the refusal used to stop at the first, so fixing one only
## bought the next one's message.
printf 'field: "srv[#1].port"\n\trequired: yes\nfield: "*"\n\ttype: int\n\trepeat: 1\nfield: ok\n\ttype: int\n' > "${tmpDir}/twoblocked.shcl"
## A schema that does not build: the report is the build faults alone, not the
## faults plus what an empty document would owe the schema.
printf 'field: a\n\ttype: int\n\trequired: yes\nfield: b\n\ttype: nope\n' > "${tmpDir}/nobuild.shcl"
## An instance whose discriminator holds an '=', which is what made --set's own
## split ambiguous.
printf 'x[a=b]:\n\tc: 0\n' > "${tmpDir}/sel.shcl"
## An instance whose discriminator holds an apostrophe: ordinary text in a bare
## selector, which the split used to read as an open quote.
printf "srv[O'Brien]:\n\tport: 0\n" > "${tmpDir}/quote.shcl"
## A name a path cannot hold bare, for the traversal commands: enumerating keys
## is only useful if what comes back can be read straight back.
printf 'db:\n\thost: h\n\t"odd.key": 2\nweb:\n\tport: 1\n' > "${tmpDir}/tree.shcl"
## Two plain keys, for the edit options: what each one leaves behind is the
## whole assertion, so the document has to be small enough to spell out.
printf 'a: 1\nb: 2\n' > "${tmpDir}/two.shcl"
## Bracket text on a value line is kept verbatim and binds nothing, so the
## rewrite goes through unchanged. The 2.x selector sugar reads the same way
## now, and the line under it goes with it, so that file refuses to save until
## migrate rewrites it. The sugar file is copied fresh for every run of a row
## that names %W%, since a rewrite is the thing being tested.
printf 'ports: [80, 443]\n' > "${tmpDir}/brarray.shcl"
printf 'srv["1,000"].port: 1\n' > "${tmpDir}/selcomma.shcl"
printf 'base:[Boston]\n\tlat: 42\n' > "${tmpDir}/sugar.shcl"
## A value the two rule sets read differently: 2.x resolved the backslash and
## these rules do not, and the bytes are the same either way, so a rewrite that
## guesses damages whichever file it guessed wrong about. Copied fresh for the
## rows that rewrite, the way the sugar file is.
printf 'p: %s\n' "'C:\temp'" > "${tmpDir}/bsrc.shcl"
## The bracket array again, for the rows that rewrite it. %BA% is shared, and a
## migrate that stamps the file would leave the next binding nothing to do.
printf 'ports: [80, 443]\n' > "${tmpDir}/brsrc.shcl"
## A file that says which rules wrote it, which is the whole answer: there is
## nothing to migrate and a second run must not touch it.
printf 'p: 1\n##    Format   3\n' > "${tmpDir}/stamped.shcl"
## A Format line inside a raw body is that block's content. Taken as the
## file's, the first made a current file look like 2.x and rewrote it at exit
## 0, and the second made any file look current.
#  shellcheck disable=2016  ## the backticks are the fence the fixture needs.
printf 'p: %s\nnote:\n\t```\n##    Format   2\n\t```\n' "'C:\temp'" > "${tmpDir}/rawfmt2.shcl"
#  shellcheck disable=2016
printf 'p: %s\nnote:\n\t```\n##    Format   3\n\t```\n' "'C:\temp'" > "${tmpDir}/rawfmt3.shcl"
## A stamped file behind a BOM, whose value 2.x would have read another way.
printf '\357\273\277##    Format   3\np: %s\n' 'C:\temp' > "${tmpDir}/bomstamped.shcl"
## An older Format line with migrate's own stamp after it, so the first line
## found is the older one.
printf 'a: 1\n##    Format   0\n##    Format   3\n' > "${tmpDir}/twostamps.shcl"
## A default on a path whose last segment selects by value. A value after that
## selector is ignored, so generation used to write a line that failed its own
## check. One default contradicts the selector and one names it.
printf 'field: "a[b]"\n\trequired: yes\n\tdefault: hello\n' > "${tmpDir}/seldefbad.shcl"
printf 'field: "a[b]"\n\trequired: yes\n\tdefault: b\n' > "${tmpDir}/seldefok.shcl"
## An optional field's line is commented, so the self-check never read its
## default. The second schema must still pass, since each line works alone;
## checked all at once, srv and srv.port make two srv against a repeat of 1.
printf 'field: port\n\ttype: int\n\tmax: 10\n\tdefault: 99\n' > "${tmpDir}/optdefbad.shcl"
printf 'field: srv\n\trepeat: 0, 1\n\tdefault: web\nfield: srv.port\n\ttype: int\n\tdefault: 80\nfield: "a[b]"\n\tdefault: c\n' > "${tmpDir}/optdefok.shcl"
## An optional field whose default names another instance than its path
## selects. Its line is commented, so the check only looked at the value.
## The second schema is the optdefok one with `a[b]` defaulting to `b`, which
## is what that one now has to say to pass.
printf 'field: env[prod]\n\tdefault: staging\n' > "${tmpDir}/optselbad.shcl"
printf 'field: srv\n\trepeat: 0, 1\n\tdefault: web\nfield: srv.port\n\ttype: int\n\tdefault: 80\nfield: "a[b]"\n\tdefault: b\n' > "${tmpDir}/optdefok2.shcl"

## A 250-character basename. The temp file used to carry the whole name plus
## the process id, which put it over the filesystem's limit somewhere in the
## low 240s - at a point that moved with the width of the pid.
longName="$(printf 'l%.0s' $(seq 245)).shcl"
printf 'k: 1\n' > "${tmpDir}/${longName}"
## Sixty four-byte characters: 245 bytes, inside the filesystem's limit. The
## temp name's cap counted characters, so this one could not be rewritten.
wideName="$(printf '\xf0\x9f\x98\x80%.0s' $(seq 60)).shcl"
printf 'k: 1\n' > "${tmpDir}/${wideName}"

##	Rows: id | argv | stdin | rc | stdout | stderr-regex [| created-file]
##	The last field is optional: when given, %C% must hold exactly that text
##	after the run, which is how a write that prints nothing gets asserted.
##	argv placeholders: %F% the good file, %B% the two-error file, %B2% a second
##	damaged file for a layered load, %D% a directory,
##	%P% the deepest legal document, %S% the self-contradicting schema, %S1%/%S2%
##	a nameless must-exist path at repeat 1 and 2, %S3% a schema that does not
##	build, %S4% a required path with an index selector, %SF% two of them either
##	side of a field that generates, %S5%/%S6% a schema at and
##	one past the generation field ceiling, %S7% a schema whose own load hints,
##	%S8% a raw default, %S9% a desc with a comma, %N% a file in a directory that
##	takes no temp file, %R%/%SA% a raw block and a schema that refuses it, %X% an
##	instance whose discriminator holds an '=', %Q% one whose discriminator holds
##	an apostrophe, %T% a document with a name that needs quoting in a path,
##	%F2% a two-key file for the edit options, %M% a path with no file at it,
##	%BA% a bracket array, %SQ% a selector whose discriminator needs quotes,
##	%W% a fresh copy of the selector-sugar file, %BS% a fresh copy of a file
##	whose value reads differently under the two rule sets, %BW% a fresh copy of
##	the bracket array, %V3% a file that already names its format,
##	%V3B% the same behind a BOM, %V03% an older Format line and then the
##	current one, %RF2%/%RF3% a raw body holding a Format line,
##	%SB%/%SC% a last-segment selector whose default contradicts it and one
##	whose default names it, %SD%/%SE% an optional field's bad default and
##	optional lines that each pass alone, %SH% an optional field whose default
##	names another instance than its path selects, %SI% the %SE% schema with a
##	default that names its instance,
##	%C% a path with nothing at it, cleared before every binding's run,
##	%L% a fresh copy of a file whose basename is 250 characters, %LW% one whose
##	basename is 245 bytes of four-byte characters,
##	%NB% a repeated field name carrying a line break, %DN%/%SN% a flat name
##	carrying a dot and a schema that declares it as nesting, %SL%/%SM% schema
##	paths and a type carrying a line break, valid and faulted, and %DL% a
##	document for them, %CB% a malformed
##	line indented and behind a non-ASCII name, %CR% one behind a carriage
##	return that is not indent, %SG%/%DG% a schema with an int
##	and a float range and a document that breaks both.
##	stdin: printf %b text, '-' none, '@closedin' / '@closedout' close that
##	stream, '@fullout' / '@fullerr' point it at a device that is always full.
##	stdout and stderr: '-' means unchecked; an empty stdout field means exactly empty.
##	A stderr regex starting with '!' must match NO line.
##	Each row names the round and item it pins.
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) onWindows=1 ;; *) onWindows=0 ;; esac
rows=(
	## 20260830 item 15: a second '-' read an empty document that looked like an answer.
	'dup-stdin-schema|check --schema=- -|a: 1\n|1||named only once'
	'dup-stdin-layer|get --layer=- - a|a: 1\n|1||named only once'
	## 20260830 item 10: the reference kept a trailing CR on an ops line, the ports stripped it.
	'ops-line-cr|set %F%|int\tx\t1\r|0|a: 1\n\nx: 1\n|-'
	'ops-lone-cr|set %F%|int\tx\t1\n\r|0|a: 1\n\nx: 1\n|-'
	## 20260908: one CR comes off an ops line, not two - the second is the value's.
	'ops-double-cr|set %F%|string\tx\tv\r\r\n|0|a: 1\n\nx: "v\r"\n|-'
	## 20260830 item 14: Python raised a traceback, C exited nonzero. POSIX-only:
	## the row closes fd 0, and windows has no equivalent a shell can set up.
	'closed-stdin|fmt -|@closedin|0||^$'
	'closed-stdout|fmt %F%|@closedout|0|-|^$'
	## 20260830 item 16: C dropped the line number and the offending op.
	'bad-op-unknown|set %F%|bogus\ta\t1\n|1|-|op line 1: unknown op: bogus'
	## 20260830 item 18: C answered a directory with a bare "read error". Exit 8
	## since 20260830b item 22 split I/O out of the usage code. The wording is
	## the CLIs' own, from a stat ahead of the read, so the row can expect one
	## spelling rather than four platforms' worth (20260901b item 32).
	'read-dir-names-error|fmt %D%|-|8|-|^[^ ]*adir: Is a directory$'
	## 20260830 item 17: C printed a bare count instead of naming the diagnostics.
	'strict-load-list|fmt --strictness=strict %B%|-|6|-|strict load failed: 2 error diagnostic'
	## 20260830 round: an unknown command is judged before its options.
	'unknown-cmd-before-opts|bogus --nope %F%|-|1|-|unknown command: bogus'
	## 20260909 item 59: a real option in front of the subcommand was called
	## unknown and then handed its own spelling back as a suggestion, and a value
	## option in space form that ate the filename got only the usage line.
	'opt-before-cmd|--schema=x check %F%|-|1||^option --schema goes after the subcommand'
	'unknown-opt-before-cmd|--nope check %F%|-|1||^unknown option: --nope'
	'opt-space-ate-file|check --schema %F%|-|1||took .* as its value, so no FILE is left'
	## init takes no FILE, so the space form has to keep working there.
	'opt-space-init-ok|init --no-banner --schema %S2%|-|0|-|^$'
	## 20260829 item 10: Python recursed a frame per level in three places.
	'deep-nesting|fmt %P%|-|0|-|^$'
	## 20260830b item 4: init emitted a config that fails the schema that made it.
	'init-bad-default|init --schema=%S%|-|6||V097 generated value fails the schema'
	## 20260901 item 5: the self-check waved every V007 through, so a repeat
	## lower bound of 1 - a must-exist path - went out as a config that fails
	## its own schema at exit 0.
	'init-star-repeat1|init --schema=%S1%|-|6||V097 required path cannot be generated: \* \(a \* name segment has no name to write\)'
	'init-star-repeat2|init --schema=%S2%|-|0|-|^$'
	## 20260902 item 19: an index selector or a path past the cap got the
	## self-check's "required path missing", which points at the config rather
	## than at the schema line nothing can generate.
	'init-index-required|init --schema=%S4%|-|6||V097 required path cannot be generated: srv\[#1\].port \(a \[#N\] selector needs an instance'
	## 20260909 item 49: one unwritable path took the whole schema with it and
	## the message said only which path, so the reason and the other blockers
	## were both left to guesswork.
	'init-blocked-first|init --schema=%SF%|-|6||V097 required path cannot be generated: srv\[#1\].port \(a \[#N\] selector needs an instance'
	'init-blocked-second|init --schema=%SF%|-|6||V097 required path cannot be generated: \* \(a \* name segment has no name to write\)'
	## 20260902 item 20: V096 fired at exactly the ceiling, on a schema with no
	## fragments, saying the schema expands past it.
	'init-cap-at-limit|init --no-banner --schema=%S5%|-|0|-|^$'
	'init-cap-over|init --no-banner --schema=%S6%|-|6||V096 schema expands past 10000 fields'
	## 20260902 item 43: a raw default was dropped or misreported, a desc with a
	## comma produced no comment, and V096/V097 named a schema line space they
	## are not in.
	'init-raw-default|init --schema=%S8%|-|6||schema line 3: Error: V092'
	'init-comma-desc|init --no-banner --schema=%S9%|-|0|## one, two\n## any, required\na:\n|-'
	'init-genfault-line-space|init --schema=%S4%|-|6||^line 0: Error: V097'
	'init-build-fault|init --schema=%S3%|-|6||V091 unknown schema type'
	'init-build-fault-only|init --schema=%S3%|-|6||!V002'
	## 20260909 item 5: a value after a last-segment selector is ignored, so a
	## default there generated a line that failed check at exit 6.
	'init-selector-default-contradicts|init --schema=%SB%|-|6||V097 generated value fails the schema that produced it: required path missing: a\[b\]'
	'init-selector-default-consistent|init --no-banner --schema=%SC%|-|0|## any, required\na: b\n|-'
	## Loose bug from 20260909 item 5: an optional field's bad default went out
	## commented at exit 0.
	'init-optional-bad-default|init --schema=%SD%|-|6||V097 generated value fails the schema that produced it: value above max 10 at .port.: 99'
	## 20260918 item 4: the same for a default that names another instance.
	'init-optional-selector-default|init --schema=%SH%|-|6||V097 generated value fails the schema that produced it: default does not name the instance its path selects: env\[prod\]$'
	## Retired 2026-09-17: a commented child of a commented valued parent now
	## selects the parent's default, so the dotted `srv.port` this row expected
	## became two instances once both lines were uncommented. The row below is
	## the same schema and exit code with the new spelling.
	# 'init-optional-defaults-ok|init --no-banner --schema=%SE%|-|0|## any, repeat 0-1\n# srv: web\n\n## int\n# srv.port: 80\n\n## any\n# a: c\n|-'
	## Retired 2026-09-18 by 20260918 item 4: its `a[b]` carried `default: c`,
	## which names another instance than the path selects, and the spec says
	## that fails generation for an optional field too. The row below is the
	## same schema with the default naming `b`.
	# 'init-optional-defaults-ok|init --no-banner --schema=%SE%|-|0|## any, repeat 0-1\n# srv: web\n\n## int\n# srv[web].port: 80\n\n## any\n# a: c\n|-'
	'init-optional-defaults-ok-named|init --no-banner --schema=%SI%|-|0|## any, repeat 0-1\n# srv: web\n\n## int\n# srv[web].port: 80\n\n## any\n# a: b\n|-'
	## 20260830 item 35: -h and --help after FILE were an unknown option, though
	## every other option is read there.
	'help-after-file|get %F% -h|-|0|-|-'
	'help-after-file-long|get %F% --help|-|0|-|-'
	## 20260830 item 47: at the default strictness a recovered-from typo was
	## silent unless --write was passed, so stdout carried the repair with
	## nothing said about it.
	'fmt-diags-without-write|fmt %B%|-|0|-|E015 missing colon'
	'set-diags-without-write|set --set=a=2 %B%|-|0|-|E015 missing colon'
	## 20260829 item 6: --set split PATH from VALUE at the first '=' anywhere, so
	## a selector holding one could not be addressed at all.
	'set-eq-in-selector|set --set=x[a=b].c=1 %X%|-|0|x: a=b\n\tc: 1\n|-'
	## 20260905 item 3: a quote anywhere in the path was read as opening a quoted
	## piece, so an apostrophe in a bare selector left every later '=' looking
	## quoted and the option was refused while get on the same path worked.
	"set-quote-in-selector|set --set=srv[O'Brien].port=9 %Q%|-|0|srv: \"O'Brien\"\n\tport: 9\n|-"
	"set-default-quote-in-selector|set --set-default=srv[O'Brien].port=9 %Q%|-|0|srv: \"O'Brien\"\n\tport: 0\n|-"
	"set-quoted-selector-eq|set --set=x[\"k]=v\"].d=2 %X%|-|0|x: a=b\n\tc: 0\n\nx: \"k]=v\"\n\td: 2\n|-"
	"set-open-quote-refused|set --set=a[\"open=1 %X%|-|1|-|bad --set value"
	## 20260909 item 13: a value built by a setter or a selector read as
	## unquoted, so quoted thousands were BadType until a save and reload.
	'set-quoted-thousands|get --int --set=a=1,000 %F% a|-|0|1000|-'
	"set-selector-thousands|get --int --set=x[\"1,000\"].y=1 %F% x|-|0|1000|-"
	'selector-thousands|get --int %SQ% srv|-|0|1000|-'
	## 3.0: bracket text after the colon is one outcome, kept verbatim. The
	## 2.x selector sugar is that shape now too, and migrate is what carries
	## a file written with it across.
	## One diagnostic is one line, and a name is spelled the way the emitter
	## would write it. A line break in a name used to split one hint across
	## three stderr lines, and a flat `x.y` printed the same as `x` nesting `y`.
	'diag-name-line-break|check %NB%|-|0|line 2: Hint: H001\nok (1 diagnostic(s))\n|^line 2: Hint: H001 ."a\\nb". repeats'
	'diag-name-dotted|check --schema=%SN% %DN%|-|6|line 1: Error: V001\nfailed: 1 diagnostic(s), 1 error(s)\n|unknown field ."x\.y".'
	## 20260918 item 14: the same for schema text, which every code below printed raw.
	'schema-text-v002|check --schema=%SL% %DL%|-|6|-|V002 required path missing: a\."x\\ny"$'
	'schema-text-v003|check --schema=%SL% %DL%|-|6|-|V003 wrong type at .c\."x\\ny".: value is not a valid bool$'
	'schema-text-v004|check --schema=%SL% %DL%|-|6|-|V004 value not allowed at .b\."x\\ny".: 9$'
	'schema-text-v005|check --schema=%SL% %DL%|-|6|-|V005 value below min 5 at .b\."x\\ny".: 1$'
	'schema-text-v006|check --schema=%SL% %DL%|-|6|-|V006 value above max 6 at .b\."x\\ny".: 9$'
	'schema-text-v007|check --schema=%SL% %DL%|-|6|-|V007 instance count out of bounds at .b\."x\\ny".: 2 not in 3\.\.3$'
	'schema-text-v091|check --schema=%SM% %DL%|-|6|-|V091 unknown schema type .in\\nt.$'
	'schema-text-v093|check --schema=%SM% %DL%|-|6|-|V093 bad schema path: d\."x\\ny"\.$'
	'bracket-array-check|check %BA%|-|6|line 1: Error: E019\nfailed: 1 diagnostic(s), 1 error(s)\n|-'
	'bracket-array-write-kept|fmt --write %BA%|-|0||-'
	'sugar-check|check %W%|-|6|line 1: Error: E019\nline 2: Error: E018\nfailed: 2 diagnostic(s), 2 error(s)\n|-'
	'sugar-check-strict|check --strictness=strict %W%|-|6|-|-'
	'sugar-write-refused|fmt --write %W%|-|7|-|dropped 1 line'
	'sugar-migrate|migrate %W%|-|0|base: Boston\n\tlat: 42\n##    Format   3\n##    Migrated from SHCL 2.x.\n|-'
	'sugar-migrate-write|migrate --write %W%|-|0||-'
	## 20260909 item 4: a 3.0 file spells a backslash value the same way a 2.x
	## one does, so migrating on a guess changed a correct file at exit 0. The
	## file has to say which rules wrote it, or the caller has to.
	'migrate-ambiguous-refused|migrate %BS%|-|7|-|does not say which it was written for'
	"migrate-ambiguous-kept|migrate %BS%|-|7|p: 'C:\\\\temp'\n|-"
	'migrate-ambiguous-write-refused|migrate --write %BS%|-|7|-|refusing to rewrite'
	'migrate-from-2x|migrate --from-2x %BS%|-|0|p: "C:\\temp"\n##    Format   3\n##    Migrated from SHCL 2.x.\n|-'
	## A file that names its format has nothing to migrate, which is what stops
	## the second run from rewriting the first run's output.
	'migrate-stamped-noop|migrate %V3%|-|0|p: 1\n##    Format   3\n|nothing to migrate'
	## 20260918 item 1: the version scan read raw bodies the rewrite skips.
	'migrate-format-in-raw-old|migrate %RF2%|-|7|-|does not say which it was written for'
	'migrate-format-in-raw-new|migrate %RF3%|-|7|-|does not say which it was written for'
	## 20260918 item 2: C looked for the version line before taking off a BOM.
	'migrate-bom-stamped|migrate %V3B%|-|0|-|nothing to migrate'
	'migrate-bom-stamped-from-2x|migrate --from-2x %V3B%|-|0|-|nothing to migrate'
	## Found by the fuzz in the 20260918 fix round: the first Format line decided,
	## so a file naming an older format was stamped again on every run.
	'migrate-two-stamps|migrate %V03%|-|0|-|nothing to migrate'
	## 20260909 item 10: 2.x bound the bracket array and nothing binds it now.
	## Leaving the line is the decision; exiting 0 was the defect, since a
	## scripted migration could not tell "migrated" from "gave up".
	'migrate-lost-binding|migrate %BA%|-|7|-|bound a value under 2.x that nothing binds now'
	'migrate-lost-write-refused|migrate --write %BW%|-|7|-|refusing to rewrite'
	'migrate-lost-write-lossy|migrate --write --lossy %BW%|-|0|-|bound a value under 2.x'
	## 20260909 item 41: telling a file that needs migrating from one that does
	## not took a diff of the output, and --write said nothing either way.
	'migrate-check-names|migrate --check %W%|-|6||w\.shcl:1: migrate would rewrite this line'
	'migrate-check-clean|migrate --check %F%|-|0||!.'
	'migrate-check-stamped|migrate --check %V3%|-|0||nothing to migrate'
	'migrate-check-ambiguous|migrate --check %BS%|-|7||does not say which it was written for'
	'migrate-check-from-2x|migrate --check --from-2x %BS%|-|6||bs\.shcl:1: migrate would rewrite'
	'migrate-check-write|migrate --check --write %W%|-|1|-|--check cannot be combined with --write'
	'migrate-write-says|migrate --write %W%|-|0||migrated, 1 line\(s\) rewritten'
	## 20260904 item 47: the temp beside a long-named file ran past the name limit.
	'long-name-write|fmt --write %L%|-|0||-'
	## 20260909 item 16.
	'wide-name-write|fmt --write %LW%|-|0||-'
	'sugar-migrate-write-stdin|migrate --write -|-|1|-|cannot rewrite stdin'
	'tokens-line|tokens %F%|-|0|1:0 name=0-1 sep=1 value=3-4 elem=3-4\n|-'
	'tokens-fault|tokens %B%|-|0|1:0 name=0-1 sep=1 value=3-4 elem=3-4\n2:2 name=0-3\n3:0 name=0-1 fault=2:unexpected character after the path\n|-'
	## 20260830b item 18: a read below strict returned the value and said nothing
	## about a line the load had dropped, so a damaged file read clean at exit 0.
	'get-diags|get %B% a|-|0|1|E015 missing colon'
	'count-diags|count %B% a|-|0|1|E015 missing colon'
	'instances-diags|instances %B% a|-|0|1|E015 missing colon'
	## 20260830b item 22: usage and I/O shared exit 1, so a script could not
	## tell "the command line is wrong" from "that file is not there".
	'io-missing-file|get %M% a|-|8|-|-'
	'io-missing-layer|fmt --layer=%M% %F%|-|8|-|-'
	'io-missing-check|check %M%|-|8|-|-'
	'io-missing-schema|init --schema=%M%|-|8|-|-'
	'usage-unknown-option|get --nope %F% a|-|1|-|unknown option'
	'usage-bad-write-path|set --set=a[*]=1 %F%|-|1|-|wildcard path cannot be written'
	## 20260830b item 21: removal and the set-if-absent family had no option
	## form, so a one-key edit meant a printf with a literal tab piped into set.
	## The five spellings share one ordered list, so the last one on a path wins.
	'remove-option|set --remove=b %F2%|-|0|a: 1\n|-'
	'set-default-absent|set --set-default=c=3 %F2%|-|0|a: 1\nb: 2\n\nc: 3\n|-'
	'set-default-present|set --set-default=a=9 %F2%|-|0|a: 1\nb: 2\n|-'
	'set-literal-default|set --set-literal-default=p=1,2 %F2%|-|0|a: 1\nb: 2\n\np: 1, 2\n|-'
	'set-family-order|set --set=a=5 --remove=a %F2%|-|0|b: 2\n|-'
	'remove-ephemeral|get --remove=a %F2% a|-|3|-|-'
	'remove-write-refused|fmt --write --remove=a %F2%|-|1|-|cannot be combined with --remove'
	'remove-empty-path|set --remove= %F2%|-|1|-|bad --remove value'
	## 20260830b item 19: a script could read an open section's values but never
	## learn its keys, so the only route was parsing fmt output in shell. A name
	## needing quotes comes back path-ready, or enumerating it buys nothing.
	'children-top|children %T%|-|0|db\nweb|-'
	'children-quoted|children %T% db|-|0|host\n"odd.key"|-'
	'children-missing|children %T% nope|-|0||-'
	'paths-all|paths %T%|-|0|db\ndb.host\ndb."odd.key"\nweb\nweb.port|-'
	## 20260901b item 28: a value with newlines in it stays on one line.
	'diag-value-one-line|check --schema=%SA% %R%|-|6|-|not allowed at .b.: line one.nline two'
	## 20260901b item 24: two layers with a bad line 2 printed the same thing
	## twice, with nothing to say which file each came from.
	'layer-diags-named|fmt --layer=%B% %B2%|-|0|-|bad2.shcl line 2: Error: E014'
	'single-file-diags-unnamed|fmt %B%|-|0|-|^line 3: Error: E014'
	## 20260909 item 34: E014 says where on the line the path went wrong, as
	## a byte column, which the tokenizer computed and the message dropped.
	'e014-column|check %B%|-|6|-|^line 3: Error: E014 malformed line skipped: unexpected character after the path, at column 3$'
	'e014-column-bytes|check %CB%|-|6|-|^line 2: Error: E014 malformed line skipped: unexpected character after the path, at column 7$'
	## 20260918 item 15: the blank run between the indent and the text counts.
	'e014-column-cr-lead|check %CR%|-|6|-|E014 .*, at column 5$'
	## 20260901b item 26: a strict failure in a lower layer ends the fold there,
	## and says which layer it was.
	'layer-strict-names-the-layer|fmt --strictness=strict --layer=%B% %B2%|-|6|-|bad.shcl line 2: Error: E015'
	## 20260902 item 44: the failing phase is named, not guessed.
	'write-names-the-phase|set --write --set=a=2 %N%|-|8|-|cannot create temporary file'
	## 20260902 item 41: the schema's own diagnostics were never printed, so the
	## hint that explains a schema fault could not be seen.
	'schema-own-hints|check --schema=%S7% %F%|-|6|-|schema line 4: Hint: H001'
	## 20260902 item 41: extra tab-separated fields were dropped, so a raw whose
	## content held a literal tab lost everything after it at exit 0.
	'ops-extra-fields-raw|set %F%|raw\tk\t\tbody\twith\ttabs\n|1|-|raw takes 4 tab-separated'
	## 20260909 item 60: one message covered four causes across both halves of
	## the op, so a refusal never said which half to look at.
	'ops-raw-bad-info|set %F%|raw\tk\tc#x\tbody\n|1|-|the info string has no spelling that reads back'
	'ops-raw-bad-body|set %F%|raw\tk\tc\tbody\r\\nmore\n|1|-|the block body has no spelling that reads back'
	'ops-extra-fields-int|set %F%|int\tk\t1\textra\n|1|-|int takes 3 tab-separated'
	'ops-array-takes-any|set %F%|int-array\tk\t1\t2\t3\n|0|a: 1\n\nk: 1, 2, 3\n|-'
	## 20260902 item 41: --default and --on-bad=error each overwrote the other's
	## mode, so which one applied depended on which was typed last.
	'default-vs-onbad|get --int --default=7 --on-bad=error %F% nope|-|1|-|--default cannot be combined with --on-bad=error'
	'onbad-vs-default|get --int --on-bad=error --default=7 %F% nope|-|1|-|--default cannot be combined with --on-bad=error'
	'default-with-onbad-default|get --int --default=7 --on-bad=default %F% nope|-|0|7|-'
	'default-alone|get --int --default=7 %F% nope|-|0|7|-'
	## 20260902 item 15: a refused edit returned before the load's diagnostics
	## were printed, so a damaged file said nothing about the damage.
	'refused-set-still-reports|get --set=a[*]=1 %B% a|-|1|-|E015 missing colon'
	'refused-op-still-reports|set %B%|int\ta[*]\t1\n|1|-|E015 missing colon'
	## 20260902 item 14: Go read a non-UTF-8 ops script as a usage error.
	'ops-not-utf8|set %F%|\xff\n|8|-|-'
	## 20260902 items 8 and 9: a stdout that could not be written was reported
	## as success by three CLIs, and a stderr that could not be written aborted
	## the reference with nothing on stdout at all.
	'full-stdout-fmt|fmt %F%|@fullout|8|-|[Nn]o space left'
	'full-stdout-check|check %F%|@fullout|8|-|-'
	'full-stdout-get|get %F% a|@fullout|8|-|-'
	'full-stdout-set|set --set=a=2 %F%|@fullout|8|-|-'
	'full-stderr-keeps-stdout|fmt %B%|@fullerr|0|a: 1\n\tbad:\nb 2\n|-'
	## Found working 20260830b item 18: a merge does not carry diagnostics, so
	## reading them off the merged doc reported the lowest layer and stayed
	## silent about FILE - the one file the caller actually named.
	'layer-base-diags|fmt --layer=%F% %B%|-|0|-|E015 missing colon'
	'layer-base-diags-set|set --set=q=1 --layer=%F% %B%|-|0|-|E015 missing colon'
	## A created file says what format it is. The block goes at the bottom, the
	## edits above it, and --no-banner leaves it out. A file that is already
	## there is never given one. 20260909 item 56: the blank line above the
	## block, which init's output has and this one used to be missing.
	'create-info-block|set --write %C% --set=srv.port=8080|-|0|-|-|srv:\n\tport: 8080\n\n##\n## This config file format is SHCL.\n## "Simple Hierarchical Config Language"\n##    Format   3\n##    Home     https://github.com/jim-collier/shcl\n##    Syntax   https://github.com/jim-collier/shcl/blob/v3.0.0/project/spec.md\n##    Legal    SHCL is Copyright \xc2\xa9 2026 Jim Collier [ID: 2უNაɘ«҂թȹɤξπ๙¿ձϖ]. License: MIT. No warranty.\n##\n'
	'create-no-banner|set --write --no-banner %C% --set=srv.port=8080|-|0|-|-|srv:\n\tport: 8080\n'
	## 20260909 item 35: set without --write took --no-banner and did nothing with
	## it, where --lossy in the same spot was refused.
	'no-banner-without-write|set --no-banner --set=a=2 %F%|-|1|-|only meaningful with --write'
	## 20260909 item 33: the help put migrate and tokens among the subcommands
	## that take --strictness, --layer and --set. They refuse all three.
	'migrate-no-strictness|migrate --strictness=strict %F%|-|1|-|not valid for migrate'
	'tokens-no-layer|tokens --layer=%F% %F%|-|1|-|not valid for tokens'
	'migrate-no-set|migrate --set=a=2 %F%|-|1|-|not valid for migrate'
	## 20260909 item 6: the create was decided before the wait on stdin, so a
	## file made during the wait was replaced by the edits at exit 0.
	'create-appeared|set --write %C%|@appear|8|-|exists|b: 2\n'
	## Literal text is read the way a file line is, so a # opens a comment
	## there too and only what comes before it is written.
	'literal-hash|set --write --no-banner %C% --set-literal=color=red#ff0000|-|0|-|-|color: red\n'
	## 20260909 item 42: explain. A code is looked up whatever its case, an
	## unknown one is a usage error that says where the list is, and the
	## subcommand takes no options and no second code.
	'explain-code|explain E019|-|0|-|-'
	'explain-lower|explain e019|-|0|-|-'
	'explain-unknown|explain E999|-|1|-|unknown diagnostic code: E999'
	'explain-two|explain E019 E020|-|1|-|usage: shcl explain'
	'explain-no-options|explain --strictness=strict E019|-|1|-|not valid for explain'
	## 20260909 item 43: a near miss on a command, an option or a code says what
	## was probably meant. A word nothing is near says nothing.
	'suggest-command|frmt %F%|-|1|-|did you mean .fmt.'
	'suggest-option|get --stricness=1 %F% a|-|1|-|did you mean .--strictness.'
	'suggest-option-space|get --slot %F% a|-|1|-|did you mean .--slots.'
	'suggest-code|explain E19|-|1|-|did you mean .E019.'
	'suggest-none|zzzzzzzz %F%|-|1|-|!did you mean'
	## An unknown option and a bad option value end with the help pointer, like
	## the other usage errors that do not name their own fix.
	'help-ptr-unknown-option|get --nope %F% a|-|1|-|^unknown option: --nope \(see --help\)$'
	'help-ptr-suggest-option|get --stricness=1 %F% a|-|1|-|did you mean .--strictness..? \(see --help\)$'
	'help-ptr-on-bad|get --on-bad=zz %F% a|-|1|-|^bad --on-bad value: zz \(see --help\)$'
	'help-ptr-strictness|get --strictness=9 %F% a|-|1|-|^bad --strictness value: 9 \(see --help\)$'
	'help-ptr-remove|set --remove= %F2%|-|1|-|^bad --remove value \(want PATH\) \(see --help\)$'
	'help-ptr-set|set --set==1 %F2%|-|1|-|^bad --set value .*: =1 \(see --help\)$'
	## 20260909 item 44: help narrows to one subcommand, by name or by the flag
	## after it, and refuses a name that is not one.
	'help-subcommand|help get|-|0|-|-'
	'help-subcommand-flag|fmt --help|-|0|-|-'
	'help-subcommand-unknown|help frmt|-|1|-|unknown command: frmt'
	'help-too-many|help get set|-|1|-|usage: shcl help'
	## 20260909 item 48: V005 and V006 named the field alone, so a long report
	## made you open the schema for every range failure. The int row also pins
	## that the element named is the one that broke the bound, not the first;
	## the float row pins that the bound is spelled the way the annotation line
	## spells it, so `min: 1.0` reads as 1.
	'range-max-names-value|check --schema=%SG% %DG%|-|6|line 1: Error: V006\nline 2: Error: V005\nfailed: 2 diagnostic(s), 2 error(s)\n|V006 value above max 10 at .ns.: 20$'
	'range-min-names-bound|check --schema=%SG% %DG%|-|6|-|V005 value below min 1 at .fs.: 0\.5$'
)

declare -i nRun=0 nBad=0

for row in "${rows[@]}"; do
	IFS='|' read -r id argv stdinSpec wantRc wantOut wantErr wantFile <<<"${row}"
	argv="${argv//%F%/${tmpDir}/ok.shcl}"
	argv="${argv//%B2%/${tmpDir}/bad2.shcl}"
	argv="${argv//%B%/${tmpDir}/bad.shcl}"
	argv="${argv//%D%/${tmpDir}/adir}"
	argv="${argv//%P%/${tmpDir}/deep.shcl}"
	argv="${argv//%S%/${tmpDir}/baddef.shcl}"
	argv="${argv//%S1%/${tmpDir}/star1.shcl}"
	argv="${argv//%S2%/${tmpDir}/star2.shcl}"
	argv="${argv//%S3%/${tmpDir}/nobuild.shcl}"
	argv="${argv//%S4%/${tmpDir}/idxreq.shcl}"
	argv="${argv//%SF%/${tmpDir}/twoblocked.shcl}"
	argv="${argv//%S5%/${tmpDir}/cap10000.shcl}"
	argv="${argv//%S6%/${tmpDir}/cap10001.shcl}"
	argv="${argv//%S7%/${tmpDir}/hintschema.shcl}"
	argv="${argv//%S8%/${tmpDir}/rawdef.shcl}"
	argv="${argv//%S9%/${tmpDir}/commadesc.shcl}"
	argv="${argv//%SA%/${tmpDir}/rawvalschema.shcl}"
	argv="${argv//%SB%/${tmpDir}/seldefbad.shcl}"
	argv="${argv//%SC%/${tmpDir}/seldefok.shcl}"
	argv="${argv//%SD%/${tmpDir}/optdefbad.shcl}"
	argv="${argv//%SE%/${tmpDir}/optdefok.shcl}"
	argv="${argv//%SH%/${tmpDir}/optselbad.shcl}"
	argv="${argv//%SI%/${tmpDir}/optdefok2.shcl}"
	argv="${argv//%R%/${tmpDir}/rawval.shcl}"
	argv="${argv//%N%/${tmpDir}/nowrite/f.shcl}"
	argv="${argv//%X%/${tmpDir}/sel.shcl}"
	argv="${argv//%Q%/${tmpDir}/quote.shcl}"
	argv="${argv//%BA%/${tmpDir}/brarray.shcl}"
	argv="${argv//%SQ%/${tmpDir}/selcomma.shcl}"
	argv="${argv//%NB%/${tmpDir}/nbname.shcl}"
	argv="${argv//%DN%/${tmpDir}/dotname.shcl}"
	argv="${argv//%SN%/${tmpDir}/dotschema.shcl}"
	argv="${argv//%SL%/${tmpDir}/nlschema.shcl}"
	argv="${argv//%SM%/${tmpDir}/nlfault.shcl}"
	argv="${argv//%DL%/${tmpDir}/nldoc.shcl}"
	argv="${argv//%CB%/${tmpDir}/colbytes.shcl}"
	argv="${argv//%CR%/${tmpDir}/colcr.shcl}"
	argv="${argv//%SG%/${tmpDir}/range.shcl}"
	argv="${argv//%DG%/${tmpDir}/outofrange.shcl}"
	## %W% and %L% are rewritten in place, so each binding gets its own fresh
	## copy below.
	argv="${argv//%V3%/${tmpDir}/stamped.shcl}"
	argv="${argv//%V3B%/${tmpDir}/bomstamped.shcl}"
	argv="${argv//%V03%/${tmpDir}/twostamps.shcl}"
	argv="${argv//%RF2%/${tmpDir}/rawfmt2.shcl}"
	argv="${argv//%RF3%/${tmpDir}/rawfmt3.shcl}"
	freshCopy=0
	if [[ "${argv}" == *%W%* ]]; then
		freshCopy=1
		argv="${argv//%W%/${tmpDir}/w.shcl}"
	fi
	freshBs=0
	if [[ "${argv}" == *%BS%* ]]; then
		freshBs=1
		argv="${argv//%BS%/${tmpDir}/bs.shcl}"
	fi
	freshBw=0
	if [[ "${argv}" == *%BW%* ]]; then
		freshBw=1
		argv="${argv//%BW%/${tmpDir}/bw.shcl}"
	fi
	freshCreate=0
	if [[ "${argv}" == *%C%* ]]; then
		freshCreate=1
		argv="${argv//%C%/${tmpDir}/created.shcl}"
	fi
	freshLong=0
	if [[ "${argv}" == *%L%* ]]; then
		freshLong=1
		argv="${argv//%L%/${tmpDir}/${longName}}"
	fi
	freshWide=0
	if [[ "${argv}" == *%LW%* ]]; then
		freshWide=1
		argv="${argv//%LW%/${tmpDir}/${wideName}}"
	fi
	argv="${argv//%T%/${tmpDir}/tree.shcl}"
	argv="${argv//%F2%/${tmpDir}/two.shcl}"
	argv="${argv//%M%/${tmpDir}/not-there.shcl}"
	## A device that is always full exists on linux and not on windows; the
	## rows that need one are skipped out loud rather than passing vacuously.
	## On windows the msys layer answers for /dev/full and takes the write, a
	## closed stdout is an invalid handle each runtime spells its own way, and
	## a chmod does not make a directory unwritable - so those rows are POSIX
	## rows and say so there.
	if [[ "${stdinSpec}" == @full* && ! -w /dev/full ]]; then
		##	Under the gate a skip is a failure, the way the other gates read it:
		##	these rows are the only cover a full-disk write has, and a runner
		##	that quietly loses /dev/full would report OK forever. On windows the
		##	row is skipped a line further down, which is a platform fact rather
		##	than a missing device.
		if [[ -n "${SHCL_GATE_STRICT:-}" && "${onWindows}" == 0 ]]; then
			echo "cli-regress: ${id}: no /dev/full here and the gate requires it" >&2; nBad+=1; continue
		fi
		echo "cli-regress: skipping ${id} (no /dev/full here)"
		echo "cli-regress ${id}" >> "${SHCL_GATE_SKIPS:-/dev/null}"
		continue
	fi
	if [[ "${onWindows}" == 1 && ( "${stdinSpec}" == @full* || "${stdinSpec}" == @closedout || "${stdinSpec}" == @appear || "${id}" == write-names-the-phase ) ]]; then
		echo "cli-regress: skipping ${id} (POSIX fixture; not judged on windows)"
		continue
	fi
	read -r -a args <<<"${argv}"
	for b in "${bindings[@]}"; do
		name="${b%%|*}"; cli="${b#*|}"
		((freshCopy)) && cp "${tmpDir}/sugar.shcl" "${tmpDir}/w.shcl"
		((freshBs)) && cp "${tmpDir}/bsrc.shcl" "${tmpDir}/bs.shcl"
		((freshBw)) && cp "${tmpDir}/brsrc.shcl" "${tmpDir}/bw.shcl"
		((freshLong)) && printf 'k: 1\n' > "${tmpDir}/${longName}"
		((freshWide)) && printf 'k: 1\n' > "${tmpDir}/${wideName}"
		((freshCreate)) && rm -f "${tmpDir}/created.shcl"
		rc=0
		case "${stdinSpec}" in
			@closedin)  "${cli}" "${args[@]}" >"${tmpDir}/out" 2>"${tmpDir}/err" 0<&- || rc=$? ;;
			@closedout) "${cli}" "${args[@]}" 2>"${tmpDir}/err" >&- || rc=$?; : >"${tmpDir}/out" ;;
			@fullout)   "${cli}" "${args[@]}" 2>"${tmpDir}/err" >/dev/full || rc=$?; : >"${tmpDir}/out" ;;
			@fullerr)   "${cli}" "${args[@]}" >"${tmpDir}/out" 2>/dev/full || rc=$?; : >"${tmpDir}/err" ;;
			-)          "${cli}" "${args[@]}" >"${tmpDir}/out" 2>"${tmpDir}/err" </dev/null || rc=$? ;;
			## The file turns up while the command waits on stdin: after its
			## notice and before the ops, so the create has already been decided.
			@appear)
				rm -f "${tmpDir}/in.fifo"; mkfifo "${tmpDir}/in.fifo"
				"${cli}" "${args[@]}" >"${tmpDir}/out" 2>"${tmpDir}/err" <"${tmpDir}/in.fifo" &
				appearPid=$!
				exec {fifoFd}>"${tmpDir}/in.fifo"
				for ((w = 0; w < 200; w++)); do
					grep -q 'reading write-ops' "${tmpDir}/err" && break
					sleep 0.05
				done
				printf 'b: 2\n' >"${tmpDir}/created.shcl"
				printf 'int\tk\t1\n' >&"${fifoFd}"
				exec {fifoFd}>&-
				wait "${appearPid}" || rc=$?
				;;
			*)          printf '%b' "${stdinSpec}" | "${cli}" "${args[@]}" >"${tmpDir}/out" 2>"${tmpDir}/err" || rc=$? ;;
		esac
		nRun+=1
		if ((rc != wantRc)); then
			echo "cli-regress: ${id} [${name}]: exit ${rc}, expected ${wantRc}" >&2; nBad+=1; continue
		fi
		if [[ "${wantOut}" != "-" ]]; then
			gotOut="$(cat "${tmpDir}/out")"
			expOut="$(printf '%b' "${wantOut}")"
			if [[ "${gotOut}" != "${expOut}" ]]; then
				echo "cli-regress: ${id} [${name}]: stdout ${gotOut@Q}, expected ${expOut@Q}" >&2; nBad+=1; continue
			fi
		fi
		if [[ -n "${wantFile}" && "${wantFile}" != "-" ]]; then
			gotFile="$(cat "${tmpDir}/created.shcl" 2>/dev/null || true)"
			expFile="$(printf '%b' "${wantFile}")"
			if [[ "${gotFile}" != "${expFile}" ]]; then
				echo "cli-regress: ${id} [${name}]: created file ${gotFile@Q}, expected ${expFile@Q}" >&2; nBad+=1; continue
			fi
		fi
		if [[ "${wantErr}" != "-" ]]; then
			## The stdin notice is a prompt, not a diagnostic; it is not what a row is about.
			gotErr="$(grep -v 'reading write-ops from stdin' "${tmpDir}/err" || true)"
			if [[ "${wantErr}" == !* ]]; then
				if grep -qE -- "${wantErr#!}" <<<"${gotErr}"; then
					echo "cli-regress: ${id} [${name}]: stderr ${gotErr@Q} matches /${wantErr#!}/" >&2; nBad+=1
				fi
			elif ! grep -qE -- "${wantErr}" <<<"${gotErr}"; then
				echo "cli-regress: ${id} [${name}]: stderr ${gotErr@Q} does not match /${wantErr}/" >&2; nBad+=1
			fi
		fi
	done
done

## The help text is a column-aligned table sitting at exactly 80 wide, and it is
## hand-duplicated in four CLIs, so one added word wraps it in every terminal at
## once and nothing else here would notice. Only help is checked: about and
## donate carry the copyright symbol and the author ID by design, so a byte
## count is not a column count there, and neither is aligned anyway.
maxCols=80
for b in "${bindings[@]}"; do
	name="${b%%|*}"; cli="${b#*|}"
	## The narrowed helps and the code table are cut from the same text and make
	## the same 80-column promise, and there are far too many of them to eyeball.
	## Both lists come from the CLI under test, so a new code or subcommand is
	## covered without a second list to keep in step.
	checks=(help --help explain)
	while read -r sub; do
		[[ -n "${sub}" ]] && checks+=("help ${sub}")
	done < <("${cli}" help 2>/dev/null </dev/null | { grep -oE '^  shcl [a-z]+' || true ;} | awk '{print $2}' | sort -u)
	while read -r code; do
		[[ -n "${code}" ]] && checks+=("explain ${code}")
	done < <("${cli}" explain 2>/dev/null </dev/null | { grep -oE '^[EHV][0-9]+' || true ;})
	for cmd in "${checks[@]}"; do
		read -r -a cmdArgv <<<"${cmd}"
		text="$("${cli}" "${cmdArgv[@]}" 2>/dev/null </dev/null || true)"
		nRun+=1
		if [[ -z "${text}" ]]; then
			echo "cli-regress: help-width [${name}]: ${cmd} printed nothing" >&2; nBad+=1; continue
		fi
		## ASCII first: it is what makes the byte count below a column count.
		if LC_ALL=C grep -q '[^ -~]' <<<"${text}"; then
			echo "cli-regress: help-width [${name}]: ${cmd} is not plain ASCII" >&2; nBad+=1
		fi
		if grep -q "$(printf '\t')" <<<"${text}"; then
			echo "cli-regress: help-width [${name}]: ${cmd} prints hard tabs" >&2; nBad+=1
		fi
		while IFS= read -r wide; do
			echo "cli-regress: help-width [${name}]: ${cmd} line ${wide}" >&2; nBad+=1
		done < <(LC_ALL=C awk -v m="${maxCols}" 'length($0) > m { print NR " is " length($0) " columns: " substr($0, 1, 40) }' <<<"${text}")
	done
done

## The man page sits next to that help and had nothing holding it to the same
## width; rendered at 80 it already carried one 81-column line, from an example
## block nroff does not fill. Rendered rather than read, because the source's
## line lengths are not the page's. The overstrike sequences nroff writes for
## bold come off first, or every emphasized line reads as double its width.
manPage="${repoDir}/source/man/shcl.1"
if [[ -f "${manPage}" ]] && command -v man >/dev/null 2>&1; then
	rendered="$(MANWIDTH=80 MAN_KEEP_FORMATTING='' man --nh --nj -l "${manPage}" 2>/dev/null | sed 's/.\x08//g' || true)"
	nRun+=1
	if [[ -z "${rendered}" ]]; then
		echo "cli-regress: man-width: the page rendered to nothing" >&2; nBad+=1
	else
		while IFS= read -r wide; do
			echo "cli-regress: man-width: line ${wide}" >&2; nBad+=1
		done < <(LC_ALL=C awk -v m="${maxCols}" 'length($0) > m { print NR " is " length($0) " columns: " substr($0, 1, 40) }' <<<"${rendered}")
	fi
else
	echo "cli-regress: skipping the man page width check (no man here)"
fi

if ((nBad)); then
	echo "cli-regress: ${nBad} of ${nRun} check(s) failed" >&2
	exit 1
fi
echo "cli-regress: OK: ${#rows[@]} row(s) across ${#bindings[@]} binding(s), ${nRun} check(s)"

##	History:
##		2026-08-30  Created, pinning the CLI defects from the 20260829 and
##		            20260830 rounds that no corpus case can express.
##		2026-08-31  Help width, after the help text was found sitting at exactly
##		            80 columns with nothing to fail on.
##		2026-09-08  Man page width, rendered at 80, after the page next to that
##		            help was found carrying an 81-column example line.
