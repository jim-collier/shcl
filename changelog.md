# Changelog

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## Unreleased

### Added

- `ReadFile(path, maxBytes)` in every binding and the C++ veneer: the file tier's read half on its own - the file's text, or the load status saying why not, with a cap on how much is read (past it is `Unreadable`; 0 is no cap). `LoadFile` is now this plus a parse. It is for a consumer that needs the exact bytes it last saw, to tell its own save coming back as a change notification from somebody else's edit, or a bound on what it will read before parsing - both of which meant keeping a hand-rolled read beside the library.

- `ParseLimited` (each binding's spelling, and the C++ veneer): a parse with caller-supplied caps, for input the consumer does not control. A document holds many times its byte size in memory, so `ReadFile`'s byte cap alone cannot bound a load. A node cap stops the parse with one `E020` and counts the unparsed remainder as lost, so a save cannot silently truncate; an element cap refuses any line whose array would exceed it (`E021`), skipping the line whole rather than truncating the value; a diagnostic cap lists only that many and ends the list with one `E022` that counts the rest, since a document of nothing but bad lines costs a diagnostic per line. 0 disables a cap.

- `shcl_compact()` in the C binding, and `compact()` on the C++ veneer: the write-side counterpart to `shcl_reads_release`. A write lands in the document's bump arena and the value it replaced stays there until `shcl_free`, so a process rewriting one field once a second grew by a few megabytes a day with no way to give it back. Compaction rebuilds the document into fresh arenas holding only what it now contains, diagnostics, lost count and strictness included, so a save or a strict gate afterwards reads the same. Optional; a write-once consumer never needs it.

- `shcl_reads_release()` in the C binding: gives back the memory the read calls have handed out, without touching the document. Read results live in the document's arena until it is freed, which is right for a read-once consumer and wrong for a process polling one document in a loop - 200k array reads held 15.7 MB it could not give back. Optional, so nothing changes for a caller that ignores it; the C++ veneer calls it on every read, since it copies each result out immediately.

- An allocation failure no longer ends the process in the C binding. A parse and a validate give back everything they held and return NULL, `shcl_load_file` follows its parse, and the C++ veneer's `Document` tests false. A document retains several times its input size, so a config file that read fine could still exhaust the arena - which turned a config problem into the application quitting. Everywhere else, on a document already built, the new `SHCL_OOM()` hook is the answer: the default is still the CLI's print-and-exit-70, and a consumer whose process is not the library's to end defines its own before the implementation.

- `V097`: `init` checks its own output against the schema that produced it before returning it. A field typed `int` with `min: 1`, `max: 10` and `default: 99` used to generate the comment `# int, 1-10, required` and then `server.port: 99` on the next line, so the starter config failed the schema it came from. The schema is faulted now, naming the field.

- `E019`: a value spelled with brackets, the way JSON, TOML and YAML spell an array. It used to be reported as a missing colon on a line that plainly has one, and the brackets were dropped so silently that an in-place `fmt --write` rewrote `ports: [80, 443]` to `ports: "80, 443"` and the file checked clean from then on. The load counts it as lost content now, so an in-place rewrite refuses unless `--lossy` is passed.

- `E018`: a line indented under a line that was skipped is now skipped with it, with its own diagnostic, instead of re-parenting one level up. A skipped header used to hand its children to its parent, so the document gained structure the author never wrote.

- `shcl children FILE [PATH]` and `shcl paths FILE`: the traversal half of the accessor, which the CLI did not carry. A script could read an open section's values but never learn its keys, so the only route was parsing `fmt` output in a shell. Names print in the form a path accepts, quoted where a bare name will not do, so one holding a dot or a quote goes straight back into the next read. The wrappers gain `shcl_children` and `shcl_paths`.

- `--remove=PATH`, `--set-default=PATH=VALUE` and `--set-literal-default=PATH=TEXT` on the CLI. Removal and the set-if-absent family were reachable only through a tab-separated ops script on stdin, which is awkward to write in a shell and easy to get wrong; scalar sets had been given an option form for exactly that reason. All five spellings share one ordered list, so two touching the same path resolve in the order given. Raw blocks still go in through the ops script.

- `get_raw_info` and `get_raw_info_or` in Rust, Go and Python (`GetRawInfo`/`GetRawInfoOr` in Go). A raw block's info-string was the one typed read with no convenience tier, so reading it meant dropping to the status tier while every other type had the short form - and the CLI had carried `--rawinfo` all along. C and the C++ veneer keep the status tier alone, as they do for every read handing back borrowed memory.

- A `DateTime` alias beside `Datetime` in Rust and Python, so the capitalization a consumer tries first still compiles.

- The Linux installer runs the just-verified binary before anything is written, so a system whose glibc is older than the prebuilt binary's floor hears so at install time, with the build-from-source route named, instead of hitting a raw loader error at first use. README states the floor.

- The Windows setup handles a running `shcl.exe` and an existing older install instead of failing partway through.

### Changed

- A `min` above its `max` is a schema fault. The range admits nothing, so every value drew both a below-min and an above-max error and the config looked wrong when the schema was. The field is dropped and reported once, like any other broken one.

- A `datetime` `allowed` set compares the moment, not the spelling. `allowed: 12:00:00Z` refused a config saying `12:00:00+00:00`, and `12:00:00` refused `12:00:00.0`, because the value mirrors what was written and the comparison was field by field. A value with no zone is still local and still matches no zoned one - that is the one spelling difference that is a real difference.

- The installers say more about what went wrong and refuse more of what would go wrong. A GitHub rate limit is named as one instead of "none published yet, or network down", and `GITHUB_TOKEN` is used when set. A destination that cannot be written is found before the downloads, not after them. A symlink at the bin path pointing at someone else's build is refused like a real file there. Both say when another `shcl` earlier on PATH will win. A Windows uninstall leaves a setup.exe install to its own uninstaller. `install-dev.bash` puts a fresh clone on `dev`.

- A comment trailing a top-level field is the document's, not the field's. Both spellings emit at column zero, so the distinction had nothing to come back to on a reload - and it made merging a layer differ from merging that same layer after formatting it. A comment written deeper than its field still belongs to it.

- A stdin nothing is attached to reads as an empty document in every CLI, on every platform. It always did on Linux; on Windows a closed handle came back as an error and the run exited 8.

- C: a save on Windows follows a symlink or junction to the file it points at, and works on a path past the old 260-character limit. The link used to be replaced by a regular file, and a deep path was refused; the other three bindings already did both.

- Naming a directory as the file says `PATH: Is a directory` in every CLI, on every platform. The message used to be whatever the language handed back from a failed read, which was four different sentences on Linux and a fifth on Windows.

- Diagnostics under `--layer` say which file they came from, a strict failure in a layer included. Two layers with a bad line 2 printed the same thing twice with nothing to tell them apart. A single-file load is unchanged.

- Three generation gaps. A `desc` holding a comma is several elements, and the comment came out missing entirely; it carries the whole sentence now. A `default` with no value-line spelling - a raw block, or any default under `type: raw` - is a `V092` fault at its schema line instead of being dropped silently or reported as a wrong type in the output. And `V096`/`V097` are printed as `line 0` rather than `schema line 0`: they are about the generated document, not a line of the schema.

- Three CLI shapes that surprised. `--default` together with an explicit `--on-bad=error` is a usage error instead of a silent win for whichever came last. An ops line with more tab-separated fields than its op takes is an error instead of having the extras dropped - a `raw` whose content held a literal tab lost everything after it and reported success. And `check --schema` prints the schema's own load diagnostics, so the `H001` that explains a `V092` on a repeated `allowed` is visible rather than invisible.

- A save through a path that names a directory is refused everywhere. `save_file("f/")` rewrote `f` in the reference, and `f/.` did in Go, because the path cleanup drops the trailing separator before the OS ever sees it. Go's string setters also refuse text that is not valid UTF-8 instead of storing a replacement character per bad byte and reporting success.

- Python's typed array setters take a list of their type rather than any iterable. `set_string_array("k", "abc")` wrote three elements, because a `str` is a sequence of one-character strings, and a generator was consumed by the type check before the setter read it - an empty value written and `True` returned. `set_comment`, `set_raw` and `set_literal` gate their arguments now too, instead of raising from somewhere inside.

- A written value carrying both quote kinds is stored the way its own reload stores it. `SetString("k", "q\"q'")` kept the quote bare while the emitter escaped it, so `Instances` and a read's raw text differed between a written document and a reload of the same text - the one place `set(x)` and `load(emit(set(x)))` disagreed. The value read back the same either way; only the source spelling differed.

- The name index is rebuilt by walking the document rather than the arena. A set-and-remove cycle leaves its nodes behind, and the rebuild indexed every one of them, so the first read after a merge grew with the number of edits ever made instead of with the document - 1000 live nodes behind 400000 dead ones cost 48 ms a read. C's merge and its string setter also stop abandoning a doubling chain in the document arena: a merge onto a 40000-key base cost 786 KB and costs 320 KB, and a 20 MB string value cost 55 MB and costs 20 MB.

- Two spellings of one value are one instance. `a: "q\"uote"` and `a: 'q"uote'` were two, while `a["q\"uote"]` matched both - so one selector addressed two nodes, `count` said 2 and a read could only answer `Multiple`. Identity resolves escapes now, the way a selector already did and the way names have since 2.0. Quoting was never part of identity and still is not.

- On Windows a save keeps the file's hidden and system attributes. `ReplaceFile`'s documented preserve list stops at security attributes and named streams, and the fallback rename carries nothing, so a hidden config came back visible. They are re-applied after the publish now, the way read-only already was. The `REPLACEFILE_WRITE_THROUGH` flag Microsoft documents as unsupported is no longer described as what makes the write durable; the file's own flush before the publish is.

- An array read of a one-element cell reports the element's quoting, like the scalar read of the same node. `read_int("h")` on `h: "5"` said quoted and `read_int_array("h")` said not, because the array path always answered false. More than one element still reports false: there is no single element to report.

- At Loose, a space after a currency symbol no longer decides whether the value reads. `$ 1200` read as 1200 while `$ 3.14` was `BadType`, because the int path reached a branch that trims and the float path tested the shape on the untrimmed remainder. The space comes off once, for both.

- `shcl_generate` keeps nothing when it refuses, and what it returns can be given back. The output was copied into the schema's own arena before the self-check, so a call that failed kept text it never returned, and a call that succeeded left a copy no `shcl_reads_release` could reclaim - 21.9 KB per call in a loop. The bytes live in the read arena now, and generation faults from an earlier call are dropped rather than stacked up. The C++ veneer's `generate()` releases first, like every other copying wrapper.

- `init` names the path it cannot generate. A required path with a `[#N]` selector, or one past the nesting cap, went to the trailing comment block and then failed the self-check with "required path missing", which points at the generated config rather than at the schema line nothing can satisfy. It is a `V097` fault naming the path now. A name carrying a newline is generated rather than refused: names have been stored escape-resolved since 2.0 and the name escaper spells one.

- A blank line before the first thing canonical output prints is dropped at load. Canonical output never starts with a blank, so a document that kept the flag did not survive its own canonical form: merging a layer gave a different result from merging its `fmt`, and the fold placed a blank line the author never wrote. Three shapes did it - a file starting with a blank line, a blank after a leading line the load dropped, and a blank on a later instance that merged into the first.

- A refused `--set` or a failing ops line no longer swallows the load's diagnostics. The edit was applied before anything was printed, so a `get --set` on a file with a dropped line reported the refusal and said nothing about the damage. The diagnostics belong to the load and now go out before any edit runs.

- Go's `LoadError` and Python's `diagnostics()` and `LoadError` hand back a copy. Each returned the document's own list, so a caller sorting or clearing what it was given silently changed what the document reported, and the document's next append landed in the caller's slot. Go's `Diagnostics()` was fixed for this in 2.0; these were the ones it missed.

- The named-month date forms hold the day to `DD`. `Jul +12 2026`, `Jul 0012 2026` and `+12 Jul 2026` read as 12 July, because the space-separated spellings parsed the day as a plain integer where every delimited spelling holds it to one or two digits. The spec calls the format list a closed whitelist and spells the day `DD`.

- A stdout that cannot be written exits 8 instead of reporting success. `shcl fmt f.shcl > /dev/full` exited 0 with an empty stderr in three of the four CLIs and killed the Python one with an interpreter message; the help and the man page have said 8 for a stream that could not be written all along. A reader that closed early is still the quiet exit, since nobody is there to read a complaint.

- A stderr that cannot be written no longer costs the document. The reference aborted with nothing on stdout at all when a diagnostic could not be printed, which turned an unwritable log into a lost `fmt`. Diagnostics are best-effort now; the exit code still carries the outcome.

- A wildcard read whose parent does not exist reports `NotFound` instead of `Empty`. `x[*]` on a document with no `x` said the path was there and empty, which is the answer for a field written with nothing after the colon; `x` on its own said `NotFound`. The two agree now.

- A wildcard after a wildcard flattens instead of answering `Multiple` for every slot. `server[*].*` reported one unreadable slot per instance, `count` counted instances rather than leaves, and `Remove` on such a path removed nothing. The inner slots now join the outer run, so the result is one slot per resolved leaf and the two wildcards compose the way the spec says they do.

- A float literal past the double range (`1e400`) reads as `BadType` instead of an infinity at `Good`. No double holds the value, and the infinity could not be written back, so a read-modify-write left a field the reader then refused. A literal below the range still reads as zero.

- `get`, `count` and `instances` print the load's diagnostics to stderr, the way `fmt` and `set` already did. Below strict a damaged file used to read back a correct value at exit 0 with nothing said, so the only way to learn a line had been dropped was a separate `check` run. One report per run; stdout is unchanged.

- A file or stream that could not be read or written now exits 8, and exit 1 means a usage error alone. A missing file, an unreadable one, a directory named where a file was wanted, and a target whose directory refuses a write all used to share 1 with a mistyped flag, so a script could not tell "fix the command line" from "fix the path". A path a write option refuses stays at 1, since what has to change there is the option's value.

- An in-place write the save gate refuses now exits 7, its own code, instead of sharing 1 with usage and I/O errors, so a script can tell "pass `--lossy` or fix the file" apart from "the command line is wrong".

- A raw block's nesting is the closing fence's own indent (the opening line's when the block never closes), and each body line loses only what it shares with that indent. A body whose lines all sit past the fence keeps that shared indent, which previously could not be stored at all, and emit pads every non-empty body line by the same rule. The rule is symmetric now, so the all-blank-body special case is gone.

- `fmt` and `set` print the load's diagnostics to stderr in both modes, not only with `--write`, so a recovered-from typo is just as visible when the result goes to stdout. The stdout bytes are unchanged.

- The informational flags (`-h`/`--help`, `-v`/`-V`/`--version`, `--about`, `--donate`) are recognized anywhere in option position, after FILE included. `--` still ends the options, so a file or path spelled like a flag stays reachable.

- `-` (stdin) may be named only once across FILE, `--layer` and `--schema`. Two names for one stream each read part of a document; the second is now a usage error.

- `--set` and `--set-literal` split PATH from VALUE at the first `=` outside quotes and brackets, so a selector may hold one (`x[a=b].c=1`).

- The `bool` write op accepts exactly `true` and `false`. Anything else is a bad value at exit 1, where it used to write `false` at exit 0.

- An in-place write carries over the file's whole mode, setuid, setgid and sticky bits included, and a read-only file is rewritten on every platform: Windows clears the attribute for the publish and restores it after, so the platforms agree.

- Python's typed setters raise `TypeError` on a value of the wrong type instead of writing its text form. `set_float` still takes an int and writes the float it names; a magnitude past the float range becomes `inf`, the value the other bindings store.

- The C++ veneer's `generate` is no longer `const`: a schema fault it reports goes onto the document's diagnostics, which mutates it.

- The stderr voice is tidier: messages dropped their `shcl:` prefix, a usage error answers with a `usage: shcl ...` line, a strict-load failure lists the diagnostics above its `strict load failed: N error diagnostic(s)` summary, and a schema-fault line carries its `V` code the way load diagnostics carry theirs. stdout and the exit codes are untouched, so nothing scripted against the contract moves.

- A raw block's info-string runs to the end of the line in both spellings. On the same-line form (`db: ```c#`) a `#` used to open a trailing comment, so the label came back as `c` and the rest moved onto the field line, while the same text under a child indent stayed whole. An info-string is never interpreted, which is what the grammar and the spec both already said. A comment about a same-line block goes on the line above.

- Reading a float as an int at loose strictness refuses anything at or past 2^63, rather than saturating to the integer maximum. `9223372036854775808.0` used to read as `9223372036854775807` while the same number spelled without the `.0` correctly refused. No double holds the integer maximum, so `9223372036854775807.0` is refused too; the plain decimal spelling still reads exactly.

- The Linux installer's stable channel picks the highest version rather than the most recently published release, so a patch back-ported to an older line after a newer one shipped is no longer handed out as stable. Both installers now list releases for both channels and drop drafts, which have no assets to install.

### Fixed

- A quote in the middle of a bare value no longer swallows the rest of the line. `note: don't panic  # keep this` used to load as the string `don't panic  # keep this` with no diagnostic, and the next `fmt --write` baked that in at exit 0, comment gone for good; `b: it's fine, ok` read as one element where the same words without the apostrophe read as two. A piece is quoted only when it begins with a quote, which is what the spec and the grammar always said.

- A schema with one crossed range (`min` above `max`) no longer switches off the unknown-field check for the whole document. The fault is still reported at the `max` line; the range is dropped, the field keeps its other constraints, and unknown fields are reported as they are under a sound schema.

- `Set<T>Default` and `--set-default` refuse a wildcard path whether or not its slots resolve. They used to report success and write nothing when the wildcard matched something, and refuse when it did not, so the same call passed or failed on the document's contents.

- Merging a layer over a leaf no longer deletes a malformed line the parser had retained above or under that leaf. The line stays in the merged document, where a save writes it back out, instead of vanishing with the base leaf's comments and a lost count of zero.

- A colon before a field's own colon no longer hides a bracket array. `"a:b": [80, 443]` and `srv[db:5432].ports: [80, 443]` were reported as a missing colon, counted nothing lost, and were rewritten to a quoted string by `fmt --write` at exit 0. They are `E019` now, and the save gate refuses like it does for the plain spelling.

- An allocation failure inside a C parse or validate crashed the process on Windows instead of returning NULL, on any binary built with mingw at `-O1`, `-O2` or `-Os`. The recovery unwinds through SEH there, and it was reading off the top of the stack on the way. The whole point of the recovery is that a config problem does not take the application down with it, so on Windows it had been doing the opposite of what it promised. An embedder whose `SHCL_OOM()` hook longjmps out is exposed to the same thing, since the unwind crosses these frames too, so the header now carries `SHCL_SETJMP(buf)` for arming that recovery point.

- A line whose indent matches no open level (`E012`) and a `*` line with no space after it (`E013`) now hold their indent level, so what is written under them is skipped with them (`E018`) instead of attaching one level up, a fence line at a bad indent takes its whole body with it instead of parsing it as top-level bindings, and a second line at the same bad indent is refused the same way rather than binding.

- A value after an index selector on the last segment (`a[0]: 2`) was dropped with no diagnostic and no lost count, so an in-place write deleted it at exit 0. It is reported (`E002`) and counted as lost now, as a value after a value selector always was.

- A fragment mounted at one node by two schema paths reported every fault under it twice.

- The `H001`/`H002` hints a schema disavows were matched on the schema's raw text, so a field path with an escaped quote in it kept its hint, and a `repeat` or `reopen` that faulted (`repeat: 0x2`, three elements) still silenced it. Both now go by the built schema.

- A merge appended a layer's unmatched nodes grouped by name instead of in that file's order, and dropped a footer comment the layer repeated itself. Merging onto an empty document is the identity again.

- The did-you-mean suggestion cost a full edit-distance table per name pair, so a schema and document with long field names took seconds to minutes to check. The distance is capped at the threshold and computed within that band, so it is linear in the name length.

- Every binding spells a float the same way. C printed 17 digits for 46 exact powers of two where 16 read back, and the reference rounded an exact tie between two shortest spellings away from zero where Go, Python and C round to even (`2.9802322387695312e-08` came back as `...313` from one and `...312` from the other three). Ties round to even everywhere now.

- On Windows the CLI aborted when the program reading its output closed early (`fmt` piped into `more`); it exits quietly now, as it dies quietly of SIGPIPE elsewhere.

- The C CLI on Windows took its arguments in the active code page, so a path outside it was refused and a name the page best-fits (`ā.shcl` to `a.shcl`) reached the wrong file, `--write` included. It reads the wide command line now, and the header says paths are UTF-8 on every platform. A failed publish on Windows also left errno at 0, so the CLI printed `Success` beside its failure exit; the Win32 error is mapped onto errno now.

- The `.deb` and `.rpm` declared no dependencies, so they installed on a system whose glibc is older than the binary needs and the binary then failed to load. They declare the glibc floor and libgcc read off the binary, and the deb carries its copyright and changelog files.

- A system install under a tight umask left a bin or man1 directory the installer had to create root-only; the "not on your PATH" note fired when the directory was on PATH with a trailing slash.

- The C++ veneer's `to_canonical()` never gave the read memory back, so a save loop grew without bound.

- Go's `Diagnostics()` and both suppress filters could hand back a slice sharing the document's own backing array.

- A file of lines with no colon at a constant indent parsed in quadratic time - a 1 MB plain text file took half a minute, and neither `ParseLimited` cap could stop it because no nodes or elements were built. Each refused line is kept as trivia, and every following line rewalked the whole retained list. The list is walked only as far as an incoming line could change it now, so the parse is linear again.

- `ParseLimited`'s element cap bounded nothing for an inline array: the line was built in full and refused afterwards, so 9 MB of input peaked at the same 256 MB with the cap as without, and in C the refused array stayed held for the document's lifetime. The count is taken before anything splits the value now, so a refused line costs its text and no more.

- `SetFloat` wrote `inf`, `-inf` and `NaN`, and `SetDateTime` wrote whatever the struct held (month 99, February 30, a fraction with no seconds, an empty struct as an empty value), each reporting success and each leaving a field the reader refused. Both refuse the value now and return false, the way `SetRaw` refuses an info-string it cannot spell. The CLI's float ops refuse `inf`, `nan` and a literal past the double range for the same reason; a datetime op already did.

- `init` wrote a child under a valued parent as a dotted line - `srv: web` and then `srv.port:` - which is two `srv` instances to the parser, so the child never landed where the schema looks. With a repeat lower bound of 1 on the child the self-check waved it through, and the starter config failed the schema that produced it at the very next `check --schema`. A line under a valued live parent now selects that instance by its value (`srv[web].port:`), and the self-check lets through only the one documented shortfall, a repeat lower bound of 2 or more. The C CLI reported a schema that does not build with the faults an empty document would owe it added on; it reports the build faults alone now, like the other three.

- `SetLiteral` (and `--set-literal`) took bracket-array text and wrote a two-element array holding `[80` and `443]`, with nothing said, where the same text in a file is `E019` and the line is refused. It refuses the text now, the way it already refused a quote that never closes.

- `shcl_paths` in the C binding grew the document by about 11 KB on every call, and `shcl_reads_release` could not give it back, so a process polling a document's key list climbed for the document's lifetime. It was the one read that took no path and so missed the scratch reset the path lookup does; it resets on entry now.

- The C validator put one scratch arena per level of the nesting cap on the stack - 16 KB, fine on a main thread and past the whole stack of a small worker, where it crashed. They are heap-allocated now.

- Go's atomic write ignored the result of closing the temp file, so a write error that surfaced only at close would publish a truncated file over the target. The other three bindings already reported it.

- The Linux installer runs the downloaded binary before writing anything, and so does the Windows one now - a binary that will not start never becomes an install. The Windows installer used to run it only after publishing, where a failure arrived as an exception after the success message.

- `install.bash --uninstall` said "removed" while leaving a directory full of files it had not installed. It now removes the directory only when empty and names what it left, matching the Windows installer.

- Both bash installers print their help as prose. It used to be the source header verbatim, comment markers and hard tabs included.

- `fmt` and `set` with `--layer` reported only the lowest layer's diagnostics, so damage in FILE itself - the file named on the command line - went unmentioned. Every layer's diagnostics are printed now, lowest first.

- C: a save to a path spelled with backslashes failed on Windows, so a consumer that built its config path with the platform separator could never save. The temp name now splits on either separator, and a drive-relative `C:x` target splits after the colon.

- C: the file tier reached Windows through the code-page file calls, so a path with a character outside the active code page could not be opened or, worse, was written under a mojibake name. Every file call is the wide one now, and a path that is not valid UTF-8 fails with `EINVAL` rather than opening something else.

- `set_raw` (and the `raw` op) trims the info-string the way a fence line reads it back, and refuses one holding a line break or an unquoted `#`. Either would read back as something other than what was written.

- `read_file`'s byte cap saturates instead of overflowing when set near the integer ceiling.

- `set_raw` refuses a body whose lines end in a carriage return. The load takes the whole trailing CR run off every line, so such a body did not read back: `a\r\nb` came back as `a\nb` and a body of one CR came back empty. A CR mid-line is content and still round-trips.

- `set_comment` trims its text the way the load does, so what is written is what reads back and the writer's output stays a formatter fixpoint. Text that is blank leaves a bare `#`.

- A system install by `install.bash` is readable and runnable by every user whatever umask the caller had. Under `umask 077` the install directory, the binary, the man page and the completions all came out mode 0700, so only root could use what had just been installed for everyone. The run also repairs a tree an earlier install wrote too tightly.

- The Windows setup builds for a prerelease version. Its four-integer version field took the package version verbatim, which the tool rejects for anything carrying a prerelease tail, and the release stage died there.

- The PowerShell wrapper works on Windows PowerShell 5.1 again, which its header claims support for. It called a .NET 6 method that 5.1 does not have, unguarded and at load, so every dot-source hit it; and it read a PowerShell 6+ variable before the test meant to guard the read, which throws under a caller's strict mode.

- The README's C example builds as a reader would write it. The library header asks for a POSIX level, a feature request only counts before the first system header, and the example did not say so - adding `<stdio.h>` above it, the natural place, gave five implicit declarations and a pointer-from-integer error.

- A save through a dangling symlink creates the file where the link points, in all four bindings, instead of replacing the link with a regular file. A symlink cycle at the target is reported as the error it is, instead of the link being replaced.

- Reading a path in a flat document is no longer quadratic in the sibling count: a name index replaces the per-read sibling scan, and path writes and absent-path defaults go through the same index. Forty thousand flat keys read in under half a second where it took tens of seconds.

- `set_comment` puts a node's blank separator line above the comment it attaches, so the comment sits against the node it documents instead of below the gap.

- A write-ops script with CRLF line endings works in all four CLIs: the trailing carriage return on an op line is stripped instead of read into the last field.

- C: the string reads and the save no longer grow the document's arena, so a long-running program polling one field no longer grows without bound. A zero-length write also no longer passes `fwrite` a null buffer.

- The C and Python CLIs no longer write CRLF line endings on Windows stdout.

- Python: the CLI writes UTF-8 whatever the locale says, a closed stdin reads as an empty document, and a closed stdout is silent rather than a traceback.

- Python: `save_file` no longer leaks its temp file when the document holds a lone surrogate; validation, generation and wildcard reads are iterative, so a document at the depth cap cannot exhaust the interpreter stack; and `read_file(0)` no longer reads stdin.

- The C++ veneer's `validate` and `read_file` no longer leak, and a default-constructed `Document` is usable instead of carrying a null handle.

- The Windows installer no longer ends the shell that piped it into `iex`, and its strict mode and error preference stay out of that shell too; `-Uninstall` no longer risks prompting a recursive delete of a directory it did not lay down; prereleases sort correctly when picking the newest release; and the download and extraction steps work on Windows PowerShell 5.1.

- `install.bash` no longer exits silently when a release lacks an asset it looks for: the check that names the gap runs, and the binary-only fallback works. It also refuses to overwrite a `~/.local/bin/shcl` it did not create.

- Both installers print the matching uninstall hint, and abort with a message when no terminal is there to answer the prompt.

- The packages and the drop-ins tarball are reproducible: rebuilding a tag produces byte-identical artifacts, as the binaries already did.

## v2.0.0 - 2026-08-26

The first major since 1.0.0. Two things change incompatibly, both listed under Changed: escapes now resolve in field names, so `"a\"b"` and `'a"b'` are one field where they used to be two; and the C++ veneer's `read_datetime` returns the structured value every other binding's does, with the textual form moving to `read_datetime_str`. Canonical output also moves for several shapes - in each case one where the old output lost something or would not settle - so a file reformatted by 2.0 can differ from the same file reformatted by 1.2. Go consumers update the import path to `github.com/jim-collier/shcl/source/go/v2`, and anyone who installed the Go CLI switches to the Rust binary, which is the only one distributed now - see Removed. Rust and Python consumers change a version constraint and nothing else.

### Added

- A file tier in every binding and the C++ veneer: `LoadFile`/`SaveFile` beside the existing text entry points, with a load status (`Clean`, `HadErrors`, `NotFound`, `Unreadable`) telling a caller why a load came back thin. The atomic temp-file-and-rename write moved out of the CLIs and into the libraries, so a consumer no longer has to reimplement it to avoid truncating a config on a full disk. The C binding compiles the whole tier out under `SHCL_NO_FILE_IO` for freestanding builds. A save that **creates** a file gives it the mode an ordinary create would - `0666` narrowed by the umask, the same bits an editor or a shell redirect would have produced - while a save that rewrites an existing file keeps the permission bits it already had.

- `LostCount()`, and a save gate built on it: `SaveFile` refuses to write a document that lost content on load, rather than quietly persisting the loss. `SaveFileLossy` is the explicit override, and the refusal is a value the caller can act on - `SaveError::Refused` in Rust, a `*SaveRefused` error type in Go, `SaveRefused`/`SaveFailed` raised from a `SaveError` base in Python, `SHCL_SAVE_REFUSED`/`SHCL_SAVE_FAILED` in C. The gate answers before any I/O, so a lost document saved to an unwritable path still reports the refusal rather than the write failure.

- `AuthoredName(path)`: a name as it was spelled in the source, escapes and all. It is the one accessor that hands back source text - everywhere else a name is stored, compared, emitted and enumerated with its escapes resolved - which is exactly why round-tripping a document that cares about the original spelling needs it.

- A convenience tier spelled `_or` in every binding (`GetIntOr`, `get_int_or`, `shcl_get_int_or`, `get_or<T>`): the value on `Good`, the caller's fallback otherwise. Rust and Python gained eleven each, C three, so a routine ported between two bindings can no longer keep the call name while silently changing tier.

- `reopen: true` as a schema key, plus `SuppressDeclaredReopens`: a section a config is expected to re-open in several places declares it once, instead of every consumer learning to ignore `H002`.

- Rust gained `Clone`, `FromStr` (with `Infallible` as the error, since a Standard parse cannot fail), and `Display` on `Document` and `Status`, so `println!("{doc}")` prints the canonical form. `format_f64` is public, and is what the float setters use.

- The C++ veneer gained the rest of the surface it was missing: the file tier, per-slot array statuses, a datetime array read, `exists`, `quoted`, and status-to-text. Go gained a `String` on its load status and the five array status reductions; Go and C gained an `ok()` predicate, which the other two already had.

- `shcl --lossy` on `set --write`, the CLI half of the save gate, and `-v` beside `-V`. Read failures now explain themselves on stderr in every mode rather than only under `--on-bad=error`, with stdout byte-identical; `--on-bad=default` and an empty value outside error mode stay quiet on purpose.

- `shcl set --write` creates FILE when nothing is at the path yet, so a first write no longer has to be preceded by a `touch`. `fmt --write` has nothing to format and still reports the file missing, and a file that exists but cannot be read stays an error in both - the alternative is writing over something unread.

- A man page and shell completions. `man shcl` covers every subcommand, option, write op and exit code, with the per-subcommand option ownership the help only hints at, and the bash and zsh completions offer each subcommand exactly the options it accepts - an option a subcommand does not take is a usage error, so offering it would be a lie. The `.deb` and `.rpm` put all three where each shell already looks; the installer symlinks the man page into the target's own `man1` directory and leaves the completions with the line to enable them.

- The installer gained `--uninstall`/`-Uninstall`, a sudo pre-check, a working no-terminal guard, and a pastable `export PATH=` line when the install directory is not on the path.

- Windows executables carry the program icon and the metadata the properties panel expects - product name, description, version, company and copyright - on both the x86_64 and ARM64 builds and on the setup. The version is read from the build, so it cannot drift from what `shcl version` reports.

- Release builds are reproducible. On the pinned toolchain, building a given commit produces a byte-identical binary on any machine and from any directory, for all four shipped targets, so the published checksum can be reproduced rather than trusted. README says so beside the checksum instructions.

- `-Help` on the Windows installer, listing the same options the Linux installer's `--help` does. The documented one-liner pipes the script straight into the shell, so there was no file left for `Get-Help` to describe.

### Changed

- **Field names resolve their escapes.** `"a\"b"` and `'a"b'` name one field, where each used to be a separate field keyed by its own spelling - which meant two spellings of one name were two names, while the same two spellings as *values* were one string. Names are compared, emitted and enumerated resolved; `AuthoredName` is how the source spelling is still reachable. A line break in a name is writable as a result, since names emit through an escaper that spells it `\n`; in a `[value]` selector it is still refused, because that text is stored raw.

- **The C++ veneer's datetime reads swap names.** `read_datetime` returns `Read<Datetime>`, the structured value every other binding's `read_datetime` returns; the textual form it used to return is now `read_datetime_str`. The old pair had the names backwards relative to the rest of the project, and a veneer consumer calling `read_datetime` will get a compile error rather than a silent change of meaning.

- Author quoting on a plain string survives the formatter. `ver: "8"` still normalizes to `ver: 8` - a reader types that value the same either way, so the quotes say nothing - but a quoted plain string keeps its quotes through `fmt` and `init`, because stripping them un-escaped values a downstream language treats as special (`"@null"`, a quoted function reference), which is the case the `quoted` read flag exists for.

- A quoted by-value selector matches a scalar only. `x["a, b"]` selects a single-element value whose logical string is `a, b`, rather than matching against the whole display form; a bare selector is unchanged.

- Parsing costs far less memory in every binding, and less time. A 100 MiB document used to hold 39x to 72x its size in memory (3.8 GB in Rust up to 7.0 GB in C); it now holds 21x to 47x (2.1 GB in Rust, 3.0 in C, 3.3 in Go, 4.7 in Python), with C moving from the heaviest binding to among the lightest, and load time down as much as 40%. Output is byte-identical - this is the same parse, keeping less.

- A line that is malformed in content but positionally sound is retained as inert trivia and written back, instead of vanishing. Lines whose position cannot be recovered still drop and still count toward `LostCount`, which is what the save gate reads. A BOM-led line is deliberately excluded - re-emitting it produces content the parser reads back as live.

- `shcl set --write` consults the save gate. It previously deleted unreadable lines at exit 0 with nothing on stderr; the justification on record - that a person sees the diagnostics anyway - was false at the default strictness, where they see nothing at all.

- `shcl set -` follows stdin: the piped document when the edits come from options, an empty base when the ops script has stdin. The two meanings never compete, so nothing that worked before changed.

- The unknown-field sweep runs through a key-level schema fault instead of skipping wholesale, and skips only where the fault costs the entry its path (`V093`, `V095`).

- `H002` reports at every merged level, not just the first: a re-entered map carries the re-open line down to its children.

- A bare `shcl` prints the help and exits 0, the same as `shcl help` - one convention, "asking for help succeeds" - where it used to print the same text and exit 1.

- Every setter in the reference is `#[must_use]`, so a discarded write is a compile warning rather than a silent no-op. The other bindings have no equivalent annotation; the behavior is unchanged in all four.

- Performance, all measured: the quoted-emit gate costs 0.09s where it cost 0.24s on 400k bindings; the setters share one path scan and one tree walk (80k writes, 1.31s to 1.14s); the C binding parses the same corpus in 751 MB and 0.80s where it took 1332 MB and 1.19s, and retains one copy of its output during emit rather than four; Go went from 660 MB to 593 MB and 1.42s to 1.11s.

### Fixed

- Go, Python and C disagreed with the reference on a quoted selector whose lookup needed the rare fallback scan: they created a node where the reference selects one. The accelerator is meant to be a pure speed-up, so a miss must never change the answer - the three ports now rescan on an outright miss as well as on a wrong-shape hit.

- The C binding's number parsing and formatting followed the host program's locale. Under a comma-decimal locale the canonical output diverged from the other three bindings, every float read came back `BadType`, and the float formatter truncated `1.5` to `1`. Both sites now translate the decimal point themselves - pinning the locale would have been a process-wide side effect a library has no business causing, and is not thread-safe.

- A setter accepted a path segment or a by-value selector containing a line break, and wrote a document that reparsed to nothing. Both are refused now (a name spells the break instead; see Changed).

- A value with leading or trailing whitespace outside space and tab was truncated on write. The edge-whitespace quoting rule now covers the whole set all four bindings already agreed on.

- `fmt` emptied a whitespace-only line inside a raw block, and then, once it stopped, grew a raw block whose body has *no* non-blank line by one indent level on every pass, without bound. The common indent is taken from non-blank lines, so such a body has none to strip - and the formatter now adds none back, leaving it byte-for-byte. Normalizing it away would also have ended the growth, but a raw block promising verbatim content is the wrong place to discard a line of non-breaking or ideographic space.

- Merged output was not always a formatter fixpoint: an empty binding in the base and a same-named block in the overlay both survived a merge, where parsing the two run together folds them. Merge adopts the parser's own empty-fill rule now, so the two agree. That also removes the emitter's workaround for the resulting pair, which spelled the fence on the name's line and lost an info string containing `#` outright.

- A raw block body line ending in more than one carriage return was not a fixpoint: the load stripped one, the write turned the survivor into a line ending, and the reload dropped it. The whole trailing run comes off at load now. A carriage return inside a line is content and still round-trips.

- Python's integer setters range-check against `i64`, so a value no other binding could store is refused rather than written and read back wrong.

- Python's merge and clone walks are explicit stacks. A document at the documented depth cap, merged from a caller 900 frames deep, used to exhaust the interpreter's stack past about 485 levels and leave the base document half-mutated.

- The C++ veneer's `Datetime` move operations left the moved-from value holding a view of digits it had handed away, so it formatted a fraction it no longer held. The invariant now lives in the rebind, which cannot be bypassed.

- On Windows an in-place write left the file's ACLs, attributes and alternate data streams behind. A rename publishes a new file, and everything the old one carried outside its contents went with it; the write goes through `ReplaceFile` now, which carries them onto the replacement, and falls back to the old replacing move when the file is being created or the merge cannot be done. On POSIX the containing directory is synced after the rename as well as the file before it, so a power cut can no longer lose the publish itself and leave the old content. What a write still cannot carry - other hard links, POSIX ACLs, extended attributes and the SELinux label among them - is now stated in the spec rather than left to be discovered.

- The PowerShell wrapper needed PowerShell 7. It used one operator that older versions do not have, on the line that forwards the binary's exit code, so it failed outright on the Windows PowerShell 5.1 that ships with the OS. Spelled the long way now, and it runs on both.

### Removed

- The C++ veneer's `read_datetime_raw`. Its job is `read_datetime`'s now; see Changed.

- `go install github.com/jim-collier/shcl/source/go/cmd/shcl@latest`. The Go module is the library alone now - its CLI sits in a module of its own and no longer ships inside the published one. That CLI was only ever a test fixture for the cross-binding check, byte-identical to the reference by construction and with nothing of its own to offer; the Rust binary is the CLI this project distributes, and it is what the packages, the installers and `cargo install shcl` all deliver.

## v1.2.0 - 2026-08-04

### Added

- `shcl about` and `shcl donate` on the CLI, each also spelled `--about` and `--donate` the way `help` and `version` already are. `about` gives the version, copyright, project home, license and a short description of what SHCL is; `donate` points at the GitHub Sponsors page.

- `Lines(path)` (each binding's spelling, plus the C++ veneer): the plural of `Line(path)`. A repeated field - the case that most wants a citable line, and the one the singular returns 0 for - yields every binding's line in file order; unresolved wildcard slots stay as 0 so indices keep matching `Count`.

### Changed

- `help`, `about` and `donate` print with a blank line above and below, so the block stands clear of the shell prompts either side of it. Bare `shcl` still prints the same help text unpadded - it is a usage error rather than something asked for - and `version` stays a single bare line so it is still easy to capture.

- A schema fault no longer suppresses data validation wholesale. Faults (`V090`+) are still reported first and still fail `check --schema`, but the constraints that parsed cleanly now check the document too - a typo in one constraint cannot hide a real violation of another. The unknown-field sweep still requires a fault-free schema, since a dropped constraint would turn the fields it declared into false unknowns. `shcl init` is unchanged: generation still fails on any fault.

### Fixed

- The C CLI treated an informational flag in a `--set-literal` value as a request for that output, where the other three read it as the value. Its option-skip list was missing `--set-literal`, so `shcl get --set-literal -h FILE PATH` printed the help text instead of reading.

- A block header whose children were all commented out got those comments back one indent level shallow from the formatter (and a childless header's tail-of-file comments lost their indent entirely). A comment written deeper than a block's last binding now stays inside that binding's block at its own depth, in every binding; comment runs at the binding's own level trail it as before.

## v1.1.0 - 2026-08-04

### Added

- Name-position wildcard `*` in lookup paths: `Get("indicators.*.period")` slots across children of any name the way `[*]` slots across instances (per-slot statuses, `Count`/`Instances` aligned), and a schema path can declare an open section - "any child name here, each shaped like this" - without enumerating names. The setters refuse a wildcard path, while `Remove` on one removes every matching slot; a field literally named `*` stays addressable quoted (`"*"`).

- Schema fragments: `fragment: <name>` declares a reusable shape (ordinary `field:` entries with relative paths), `inherits: <name>` mounts it at a field's path. Recursive and mutually-referencing shapes are legal with no depth limit - validation follows a mount only where the document has nodes - so arbitrarily-nesting structures (layout trees and the like) validate at any depth, and one shape can be mounted at several paths instead of duplicating its fields. `shcl init` expands mounts into the starter config, cutting where a fragment would re-enter itself. New schema-fault codes `V094` (bad fragment declaration) and `V095` (`inherits` names no declared fragment).

- `QuoteSegment` in every binding and the C++ veneer: quotes one path segment so a user-typed name can be spliced into a lookup path safely - without it, a dotted name silently reads as nesting.

- `Children(path)`: child field names in file order, duplicates included, so an open (map-shaped) section can finally be read without prefix-scanning `Paths()`. The empty path enumerates the top level.

- `Line(path)`, plus `line` and `quoted` on the read result (Rust/Go/Python; C keeps its value+status structs and gains the `shcl_line`/`shcl_children` accessors): consumer-side checks can cite the source line, and a quoted value is distinguishable from a bare one - `a: @null` vs `a: "@null"`.

- `LoadAndValidate(text, schema, strictness)`: parse and validate in one call, returning the document with one combined diagnostics list - no more hand-merging two lists and losing half the errors - plus `ErrorCount()` as the "did this file have errors?" predicate.

- A new `H002` hint when a binding merges with a non-adjacent earlier one (two separately-written `table: t` sections silently becoming one combined table is now visible; the prose names the earlier line). Adjacent re-mentions and selector/path-intermediate merges - the deliberate redundant-path idiom - stay silent.

- `check --schema` now drops `H001` (repeated bare leaf) for fields whose schema declares a repeat upper bound above 1: there, repetition is the instance mechanism by declaration, and the hint was training users to ignore hints.

- `SuppressDeclaredRepeats(schema, diags)` in every binding: drops the repeated-field hint for any field whose schema allows more than one instance - the same filter `check --schema` applies, callable wherever document diagnostics and a schema meet.

- `WriteReason(path)`: the reason a write at a path would fail - `BadPath`, `ValueInPath`, `Wildcard`, `NoSuchIndex`, or `TooDeep` - behind the setters' bare pass/fail. A probe only; it never creates.

- `V096`: generating a starter config from a schema whose fragments mount each other along several paths now reports a schema fault instead of expanding until it runs out of memory.

- `shcl set --write FILE --set PATH=VALUE` writes edits given as options straight back to the file. Previously the only way to persist an edit was a tab-separated op script piped in on stdin, which is awkward to write by hand in any shell and impossible to read once tabs are invisible; edits now need neither a pipe nor a tab. The options are repeatable and apply in the order given.

- `--set-literal=PATH=TEXT` beside it, for the values `--set` cannot spell. A `--set` value is data, so `ports=80, 443` stores one quoted string; the same text through `--set-literal` stores a two-element array, because it goes in as value syntax the way a file spells it. Both share one ordered list, so the last option to touch a path wins. Raw blocks, set-only-if-absent and removal still go in as an op script.

- `SetLiteral` and `SetLiteralDefault` in every binding (`set_literal` / `shcl_set_literal`), the library half of the above: they read their argument the way the parser reads the value half of a line, so a consumer holding value text can write it without first working out its shape. Text carrying a line break, or a quote that never closes, is rejected rather than written; an unquoted `#` ends the value as it would in a file. The op script gained a matching `literal` op.

- Generated starter configs (`shcl init --schema`) end with a short footer naming the format and linking its home page and spec, so whoever opens the file next can find out what it is and how to edit it. `--no-banner` leaves it out, and `generate` takes the same flag in every binding.

### Changed

- `generate` gained a `no_banner` argument in every binding (Python and the C++ veneer default it; Rust, Go and C callers must pass it). The flag is negative on purpose: passing false, zero, or nothing writes the footer, so the opt-out is the thing a caller has to ask for.

- The Go module's `go` directive dropped from 1.24 to 1.20, the tested floor (`strings.CutSuffix` is the newest stdlib dependency) - so pinned-toolchain projects can use the module instead of vendoring the file.

- `shcl set` no longer reads a write-ops script from stdin when any `--set` is given. This only affects a command line that passed both, which previously applied the `--set` values and then the piped ops; pass the whole edit one way or the other. The wording of the two `--write` refusals also split, naming just the option that clashed rather than "--layer or --set".

- Writer-created top-level nodes now carry the blank-line grouping a hand-written file would have: one blank line between top-level sections. Written and generated output changes shape accordingly.

- Parsing is faster: the reference no longer copies every line and its indent, and three bindings scan paths without building a character list per line. Formatting a large file is roughly 10 percent quicker in the reference, Go and C, and about 12 percent in Python.

### Fixed

- The canonical formatter no longer loses hand-authored comment layout two ways: a blank line between comment-only regions survives the round-trip, and a comment written deeper than the next binding stays with the block it sits in (re-emitted after that block's last child, at the block's indent) instead of re-attaching dedented to the next field.

- A failed strict load is no longer a dead end: the failure carries the parsed document (Go's `ParseWith` also returns it non-nil alongside the error), and the message names the first few diagnostics instead of a bare count.

- `Paths()` no longer hides a node whose name needs quoting, nor its subtree. Non-bare segments come back quoted and escaped, so every returned path is a valid lookup path and the enumeration matches the document.

- A read's raw text is now the value span exactly as authored on its source line, as documented - not the canonical display form, which silently rewrote `^\d{2,3}$` to `^\d{2, 3}$`. Values with no one-line source spelling (writer-built, stacked lists, raw blocks) keep the display-form fallback.

- By-value selectors now apply escape sequences to both sides before comparing, so `["q\"uote"]` finds an instance written `'q"uote'`. Previously the comparison was spelling against spelling and the mismatch was a silent `NotFound`; the writer's placement walk had the same blind spot and could create a spurious second instance.

- Formatting a file could change what it meant. A value that only became final after its siblings were keyed - a stacked list closing onto an earlier instance's value, or a fenced block filling an empty field that matched an earlier one - was left beside its match instead of merging, so the file formatted to two entries, reformatted to one, and a read that correctly reported several instances started returning a value.

- Rewriting a file in place created its temporary under a predictable name without demanding exclusive creation, so anything already sitting there - including a symlink someone else had planted - was written through, and the rename then turned the config itself into a link. The temporary is now created exclusively and given the target's permissions before any data goes into it, so a private config is never briefly readable to anyone else either.

- Validating against a recursive schema could stop finishing, because a shape reached from two paths was re-checked per path per level. Each shape is now checked once per node. The C binding also held every level's working data until the end; that is released as it goes now.

- Python raised on numerals beyond a few thousand digits where the other bindings report a bad type. Length is checked before conversion now.

- The C date formatter could write past the buffer size it documents when handed a value the caller built rather than one that came from parsing.

- The C++ wrapper's structured date read handed back a pointer into the document's storage, so the value dangled once the document was gone. It owns its fractional digits now, which changes what `read_datetime_raw` returns.

- Comments were dropped when documents merged: a section present in both layers lost the higher layer's comments, and a write that merged duplicate fields lost any comment hanging below the losing one. A footer several layers share is now carried over once rather than repeated per layer.

- The one-shot load-and-validate ignored a schema that failed to load, so constraints on its broken lines silently vanished. It reports the schema failure now, like `check --schema` does.

- A value or a path that read like `-h` or `--version` printed help or the version and exited successfully. Only a flag in option position counts now, and `--` ends the options so a path may begin with a dash.

- Rewriting a file in place while merging layers folded those layers permanently into it. The combination is refused.

- Generated starter configs did not always load: a wildcard written with spaces or with the alternate colon spelling produced a line the parser rejects, and a deep chain of fragments produced paths past the nesting limit.

- Three C entry points left working data in documents they do not own, growing a long-running program that holds one parsed schema.

- The Go repeat-hint filter rewrote the caller's list while returning a new one.

- Quoting a name that ends in a backslash produced a path that could not be read back.

- Repeat-hint suppression could silence an unrelated field when a schema path ended in a quoted name containing a dot.

- A field name containing a null byte could satisfy a two-segment schema path, slipping past the unknown-field check.

- A system install linked the program into a directory that is not on a normal user's path.

## v1.0.0 - 2026-07-28

First stable release. The language, the read and write APIs, and the diagnostic codes are now covered by semantic versioning: a breaking change to any of them means 2.0.0.

### Added

- Published packages. Rust on crates.io (`cargo add shcl`), Python on PyPI (`pip install shcl`), and the Go module by tag (`github.com/jim-collier/shcl/source/go`). Binding versions move in lockstep with the product version, so each ecosystem's ordinary compatible-version operator tracks minor and patch releases without crossing a major.

- Signed releases. The `sha256sums` file now comes with a detached signature made offline with the project signing key, and both install scripts verify it before trusting any checksum out of that file. The public key is at `shcl-signing.pub` for manual verification.

### Changed

- The Windows `user` install moved from `%USERPROFILE%\bin\Shcl` to `%LOCALAPPDATA%\Programs\Shcl`, and adds that directory to `PATH` directly instead of keeping a second copy of the executable one level up.

- `install.bash` now requires `openssl` alongside `curl` or `wget`. There is no unverified-install fallback.

- The trademark policy is now built around conformance: an implementation that passes the published corpus may carry the name, without asking and without affiliation.

## v1.0.0-rc1 - 2026-07-26

### Added

- Schema validation: a schema is itself a plain SHCL file. `validate(doc, schema)` in every binding, and `shcl check --schema=FILE` with stable `V###` codes.

- Layered loading: `merge(base, over)` in every binding, plus repeatable `--layer=FILE` and `--set=PATH=VALUE` overrides on the loading subcommands.

- Schema-driven generation: `generate(schema)` and `shcl init --schema=FILE` emit a commented, typed starter config that validates clean against its own schema.

- Installer packages: a `.deb` and `.rpm` per Linux binary and a Windows setup, all covered by the release checksums.

- `paths()` in every binding and the C++ veneer, to enumerate a document's field paths in file order.

- `set --write` rewrites the base file in place, matching `fmt --write`.

- Two new diagnostics: `E016` for a document nested past the 512-level cap, and `E017` for a value that opens a quote it never closes.

### Changed

- Blank lines between bindings now survive the canonical formatter instead of being discarded. Runs of them collapse to one.

- Every subcommand rejects options it does not use instead of silently ignoring them.

- `init` exits 6 on a broken schema, matching `check --schema`, and lists the schema faults.

- Large-document performance: merging, inline `[value]` selectors, stacked lists, raw-block formatting, and schema suggestions all dropped from quadratic to linear. A 32k-key merge went from 16 seconds to 0.07 in the reference, with byte-identical output.

- C memory use is flat for read-heavy and merge-heavy workloads, where it previously grew without bound.

- The Windows installers edit `PATH` in the registry segment-wise and preserve its type. Installer downloads require https with a TLS 1.2 floor.

- The library degrades gracefully on a slipped internal invariant rather than aborting.

- The spec states that lowercase is a field name's canonical spelling.

### Fixed

- A bare section header in a higher layer merged over a base document deleted the whole subtree below it. It now merges. Clearing a leaf from a layer still works.

- A write to an unusable path (a wildcard, or a missing `[#N]`) reported success and could leave a half-created path behind. Setters now report whether the write applied, and nothing is created unless the whole path is usable.

- `fmt --write` could leave a truncated file if interrupted, and C reported success on a failed write. Both writers are now atomic.

- An in-place write no longer discards what it replaces. The target's permission bits are preserved, and a symlinked config is written through instead of being replaced by a regular file. Any other hard link to the file still keeps the old content, which a rename cannot avoid.

- The Writer could create two siblings that a reparse would merge, so its output was not a formatter fixpoint.

- `shcl set` op values were validated four different ways across the bindings. All four now use the reference's grammar.

- `shcl init --schema` could generate a file that its own schema rejected, and could emit lines that do not parse.

- Go `Validate` aborted the process on a schema path with a `[#N]` selector at or above 2^63.

- Deeply nested documents crashed three of the four bindings, each at a different depth. The nesting cap catches them at load.

- The C CLI silently capped `--layer` and `--set` at 64 each.

- `shcl.h` did not compile as a drop-in C++ translation unit under `-Wall -Wextra -Werror`.

- README and spec examples that named APIs which do not exist.

## v1.0.0-beta2 - 2026-07-21

### Added

- Writer API in every binding (typed `set_*`, array and `_default` forms, `set_raw`, `remove`, comments, empty-doc constructor) plus a `shcl set` CLI subcommand driven by a tab-delimited ops script.

- Comments are preserved through the canonical formatter as node trivia rather than being discarded.

- Stable diagnostic codes (`E###`/`H###`) on `check`, printed as `line N: severity: CODE` on stdout with the prose on stderr.

- Convenience read tier with whole-value fallbacks (`get_*_or` / `default=`) across the bindings.

- `--rawinfo` accessor for a raw block's info string.

- Per-slot statuses on wildcard array reads, plus `--slots` and per-slot `--default` on the CLI.

### Changed

- `check` exits nonzero whenever any error diagnostic is present, not only on strict-load failure.

- Value options accept both `--opt=VALUE` and the space-separated `--opt VALUE` spelling; `help` and `version` are also accepted as subcommands.

- Uniform behavior across bindings for broken-pipe exit, invalid argument encoding, and out-of-memory.

- Dogfood install is now atomic (stage to a temp file in the destination, then rename over the target).

### Fixed

- Hex integer bounds now read `i64::MIN` correctly and reject out-of-range magnitudes.

- Merge keys made injective so distinct arrays and raw blocks can no longer collide.

- Large-document formatting no longer risks a stack overflow (iterative emit).

## v1.0.0-beta1 - 2026-07-15

First tagged pre-release.

### Added

- Language spec and formal grammar (`project/spec.md`, `project/grammar.abnf`).

- Conformance corpus of golden cases that every binding must pass.

- Rust reference parser and the `shcl` CLI (`get`, `fmt`, `check`, `count`, `instances`).

- Independent parsers for Go, C (with a C++ veneer), and Python, each corpus-green.

- Bash wrapper around the CLI, usable as a script or sourced for typed helpers.

- Three strictness levels (loose, standard, strict) as a per-document knob.

- Raw fenced blocks for verbatim multi-line content.
