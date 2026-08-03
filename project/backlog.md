<!-- markdownlint-disable MD007 -- Unordered list indentation -->
<!-- markdownlint-disable MD010 -- No hard tabs -->
<!-- markdownlint-disable MD033 -- No inline html -->
<!-- markdownlint-disable MD055 -- Table pipe style [Expected: leading_and_trailing; Actual: leading_only; Missing trailing pipe] -->
<!-- markdownlint-disable MD041 -- First line in a file should be a top-level heading -->
# Shcl backlog

The product backlog: bugs, features, enhancements, and code-review findings. Day-to-day tracking is moving to Github Issues, and/or nano-git-db (which is built on shcl), so this file will thin out over time.

<!-- TOC ignore:true -->
## Table of contents
<!-- TOC -->

- [Conventions](#conventions)
- [Backlog](#backlog)
	- [Misc to-do](#misc-to-do)
	- [Bugs](#bugs)
	- [Features and enhancements](#features-and-enhancements)
	- [Code Reviews](#code-reviews)
	- [Done](#done)
		- [Done - Bugs](#done---bugs)
		- [Done - Features and enhancements](#done---features-and-enhancements)
		- [Done - Code reviews](#done---code-reviews)
		- [Done - Initial requirements](#done---initial-requirements)
		- [Done - Misc to-do](#done---misc-to-do)
	- [Future and/or deferred](#future-andor-deferred)
	- [Canceled](#canceled)

<!-- /TOC -->

## Conventions

In each section, items are listed approximately from newest to oldest. (Tip: use a clipboard or macro manager to make using these emojis easier.)

| Icon | Status
| :--: | :--
| 🔘   | Not started
| 🛠️   | Started, and/or partially complete
| ✋   | Defer
| ✅   | Complete
| 🚫   | Canceled

## Backlog

### Misc to-do

### Bugs

### Features and enhancements

- ✅ The schema can't declare an open section. From TradeClanker: wildcards select instances (`server[*].port`), but there's no way to say "any child name under `indicators`, each shaped like this" - so a config with one map-shaped section can't use schema validation at all, and they keep a hand-maintained known-paths map instead. Everything else in their config would express cleanly as a schema.
	- A name-position wildcard is the missing construct. Distinct from the schema-fragments item below (shape reuse), but they'd be designed together - both grow the schema language, so both go through the same design-first gate.
	- Done: bare `*` name segment in lookup paths, all four bindings. Works in schemas (per-child contexts, unknown-sweep pattern match) and in reads (slots like `[*]`); writer refuses it; quoted `"*"` stays a literal name; document lines unchanged. Cases 037/038; spec + design.md updated.

- ✅ Schema fragments. From nano-git-db, their top feature request: the schema language can't express recursion (their arbitrarily-nesting layout blocks are generated to a fixed depth of 8, past which validation silently stops and correct keys start reporting as unknown) or path aliases (wrapped vs flat spellings force ~60 lines of string surgery over their own schema file at init). Both are the same missing idea - "this subtree has the shape of that one" - and a single fragment/reference construct closes both, taking their 99 hand-written field entries plus two generators down to something reviewable.
	- Big. Design first, post-1.0. Weigh hard against keeping the schema language small, but two independent workarounds in the most rigorous consumer argue it earns its keep.
	- Done: `fragment: <name>` declares (children are ordinary `field:` instances with relative paths), `inherits: <name>` mounts, all four bindings. Recursion/mutual refs legal with no depth limit (demand-driven expansion); `init` expands mounts and cuts where a fragment re-enters; H001 disavowal covers fragment-declared repeats; new V094/V095 faults. Cases 039-041; spec + design.md updated.

- 🔘 Ports: Tier 3 after v1.0.
	- Each drop-in where possible, corpus-green before shipping.
	- Type via a typed entry point or compile-time generic, never a runtime type field:
	- Languages:
		- 🔘 C#
		- 🔘 Java [and Kotlin]
		- 🔘 JavaScript

### Code Reviews

- **20260802**:

	- **Bugs**:

		- ✅ Code Review 20260802 item 1: formatting a file can change what it means.
			- Reproduced: a field that repeats, where the second one is an empty field later filled by a stacked list, formats to two identical lines. Reformatting that output collapses them to one, so a read that returned Multiple now returns a value.
			- Cause: when a node's value is filled in after the fact, it moves to a new merge key. If a later sibling already holds that key the filled node is left beside it instead of merging.
			- Note: all four bindings behave the same, so the cross-binding check can't see it, and no test case has this shape.
			- Fixed: duplicate siblings are folded once parsing finishes, so the tree matches a reparse of its own canonical text. Folding is depth-first, since merging two parents can leave duplicate children a level down. Case 042.

		- ✅ Code Review 20260802 item 2: in-place writes create their temporary file unsafely.
			- Reproduced: the temporary name is predictable, and nothing stops it being a symlink someone else planted. The config's contents get written through that link, and the rename then turns the config itself into a symlink.
			- Second problem: the file's permissions are copied on only after the data is written, so a 600 config is briefly world-readable. Interrupting the write leaves that copy behind for good.
			- All four bindings. Only matters where someone else can write to the config's directory, but that includes shared and temp locations.
			- Fixed: the temporary is created exclusively, so an existing file or link under that name makes the attempt fail rather than be written through, and the next name is tried. It is born private and given the target's permissions through the open handle before any data, which also keeps a group-writable config from being narrowed by the umask.
			- Note: an interrupted write can still leave a temporary behind. It now carries the config's own permissions, so it is not an exposure, and clearing it would need signal handling in all four builds.
			- The differential harness gained a check for it, since none of this shows up in normal output.

		- ✅ Code Review 20260802 item 3: one schema line can switch off the unknown-field check.
			- Reproduced: a field path written as a quoted name that starts with a star, such as a wildcard hostname key, silently becomes a real wildcard. Every unknown top-level name then passes, and the constraints on that line never apply.
			- Cause: the schema's own value parsing strips the quotes before the path is scanned, so the scanner sees a bare star.
			- The spec says the opposite in two places: a quoted star stays a literal name.
			- Fails open, which is the worst direction for the one check meant to catch typos.
			- Not a defect after all: quoting works at two levels, and the schema is behaving correctly at both. The outer quotes are ordinary string quotes around the value, needed by any path holding a selector, and they come off before the path is read - so a wildcard inside a quoted path stays a wildcard, which composed paths rely on. Quoting the segment itself, inside the value, does give a literal name, and the unknown-field check still catches typos alongside it.
			- Fixed the real problem, which is that nothing said so: the spec now spells out both levels with worked examples, including that the tempting spelling declares an open section rather than a literal name.

		- ✅ Code Review 20260802 item 4: validating against a recursive schema can hang.
			- Reproduced: when two constraint paths match the same node and both mount the same fragment, the work doubles per level of the document. A file around thirty lines deep takes over a minute; a little deeper and it never finishes.
			- The C build also runs out of memory, because each level's working data is kept until the whole validation ends rather than being released on the way back out.
			- Fixed: each shape is checked once per node, so two paths reaching the same node cost one pass. A document 500 levels deep now validates in well under a tenth of a second.
			- Fixed in C as well: the working data for each level is released on the way back out. The same document went from 11.5 GB to 2.2 MB. Case 043.

		- ✅ Code Review 20260802 item 5: generating a file from a recursive schema can crash.
			- Reproduced: a long chain of fragments overflows the stack and aborts the process in the reference build; Python raises instead; a schema of about 130 lines that branches can eat all available memory before it finishes.
			- Validation of the same schemas is fine. Only generation expands every path up front.
			- Note: generation reads a file the user supplies, so this is reachable from ordinary use.
			- Fixed: a mount chain that reaches the nesting cap is noted like one that re-enters, rather than followed. A schema whose mounts multiply past a field ceiling now reports a schema fault, V096.
			- Validation is unaffected: it still follows the document, so it needs no limit.

		- ✅ Code Review 20260802 item 6: Python raises on a very long number where the others return cleanly.
			- Reproduced: a value of five thousand digits makes the Python build exit with a stack trace, while the reference reports a bad type. Same for oversized selector indexes, schema repeat counts, and a day number inside a quoted date.
			- Cause: Python refuses to convert decimal strings past a few thousand digits, and that happens before the code's own range check.
			- Fixed: leading zeros are dropped and the rest is length-checked against what the type can hold before any conversion, so a small value written behind thousands of zeros still reads. Hexadecimal was never affected.

		- ✅ Code Review 20260802 item 7: the C date formatter can write past the buffer it documents.
			- Reproduced: the header promises 64 bytes is enough, and caps only the fractional seconds. The year and the other fields come straight from a struct the caller fills in, so a hand-built value can need about 109 bytes. The library's own writer hits this too.
			- Values that came from parsing are always short enough, so the test corpus can't see it.
			- Fixed: the text is built in full and then clamped to the documented size, which is now stated at the declaration. Output for values that came from parsing is unchanged.
			- Found alongside it: negating the most negative offset was itself undefined, and is now done at a width that holds it.

		- ✅ Code Review 20260802 item 8: comments get dropped when documents merge.
			- Reproduced: merging layers loses every comment attached to a section that exists in both. The higher layer's comments are the ones that disappear.
			- Separately, a write that merges two duplicate fields drops any comment that hung below the losing one.
			- Both contradicted the documented promise that comments travel with the node they belong to.
			- Fixed: a matched instance now takes on the higher layer's comments, and the shared fold carries the comments that hang below a block. Spec and case 042 pin both.

		- ✅ Code Review 20260802 item 9: the one-shot load-and-validate ignores a broken schema.
			- Reproduced: a schema with a bad indent loads partially and validation runs anyway, so constraints on the dropped lines quietly vanish. A badly broken schema makes every field in the config report as unknown.
			- The command-line tool gets this right and reports a schema failure. Only the library shortcut skipped the check.
			- Fixed: the shortcut now reports the same schema failure and validates nothing. An empty schema still means skip validation, as before.

		- ✅ Code Review 20260802 item 10: one write operation is spelled differently by the reference.
			- Reproduced: `datetime-array-default` is rejected by the reference and accepted by the other three.
			- Write output and exit codes are supposed to match everywhere. No test case uses this operation, which is why it went unnoticed.
			- Fixed: the reference accepts it like the others. The vocabulary was then checked verb by verb across all four, and a test line for it was added so the gap cannot reopen.

		- ✅ Code Review 20260802 item 11: writing in place with layers overwrites the file with the merged result.
			- Reproduced: formatting with a lower layer and `--write` folds that layer's contents permanently into the top file, which defeats the point of layering.
			- Help text says layering prints the merged document; it doesn't mention what `--write` then does.
			- Fixed: the combination is refused, like other option pairs that cannot both hold.

		- ✅ Code Review 20260802 item 12: a value that looks like a help flag takes over the command.
			- Reproduced: passing `-h` or `--version` as a default value, a path, or a filename prints help or the version to normal output and exits successfully. A caller reading a value gets the help text back.
			- Cause: the whole argument list is scanned for those flags before options are parsed.
			- Fixed: only a flag in option position counts. A value that reads like one, and anything after the file, is data.

		- ✅ Code Review 20260802 item 13: generated files don't always load.
			- Reproduced: a wildcard written with spaces inside the brackets, or with the alternate colon spelling, produces a line the parser rejects or a path that fails its own schema. A deep chain of fragments produces paths past the nesting limit.
			- Cause: the wildcard is stripped out of the path as text rather than rebuilt from the parsed segments.
			- The documented promise is that generated output always loads clean and validates against the schema that produced it.
			- Fixed: the path is rebuilt from its parsed segments rather than cut out of the text, so every spelling of a wildcard behaves the same. A path deeper than a document may nest now goes to the trailing note instead of being written out.

		- ✅ Code Review 20260802 item 14: the C++ wrapper can hand back a dangling date.
			- Reproduced: every other read in the wrapper copies its text out, but the structured date read copies the struct while its fractional-seconds pointer still points into the document. Letting the document go out of scope and then using the date reads freed memory.
			- Fixed: the wrapper's date read now owns its fractional digits, so it stays valid after the document goes away, like every other read there.
			- Note: this changes what that one call returns, so C++ callers using it need a small edit. Documenting the borrow instead was the alternative, but it would have left a wrapper whose whole purpose is lifetime safety handing out something unsafe.

		- ✅ Code Review 20260802 item 15: the Go repeat-hint filter damages the caller's list.
			- Reproduced: it filters in place while returning a new list, so calling it the obvious way leaves the document's own diagnostics shuffled and duplicated.
			- The reference takes the list by reference, so the mutation is expected there. The Go spelling returns a value, which reads as a copy.
			- Command-line use is unaffected; this only bit programs using the library.
			- Fixed: it builds its own list, so the caller's is never disturbed.

		- ✅ Code Review 20260802 item 16: three C entry points pile up garbage in documents they don't own.
			- Reproduced: each setter keeps about a kilobyte of path-scanning leftovers, the repeat-hint filter leaves a few hundred bytes in the schema, and generation leaves several kilobytes there. None of it is ever reused or released.
			- Measured: half a million setter calls grew a document by 600 MB; routing the same work through the existing scratch space brought that to 40 MB.
			- The command-line tool is unaffected, since it exits after one pass. A long-running program that holds a parsed schema is not.
			- Fixed: all three now do their working allocation somewhere temporary and keep only what they promise to return. Half a million writes went from 604 MB to 39 MB, and the two schema entry points from 100 MB and 2.4 GB to near nothing.

		- ✅ Code Review 20260802 item 17: the segment-quoting helper mangles a name ending in a backslash.
			- Reproduced: the closing quote gets treated as escaped, so the result is a path the scanner rejects, and the set silently fails.
			- The helper exists to make user-typed text safe to splice into a path, so this is the case it was written for.
			- Fixed in the shared quoting helper, so segment quoting, path enumeration and the formatter all get it. Any name now round-trips.

		- ✅ Code Review 20260802 item 18: repeat-hint suppression can silence the wrong field.
			- Reproduced: a schema path whose last segment is quoted and contains a dot is split on that dot, so the leftover text matches an unrelated field name.
			- Matching on the name alone is deliberate. Splitting the raw text rather than using the parsed segments was not.
			- Fixed: the name comes from the parsed path. A quoted literal star is no longer mistaken for the wildcard either.

		- ✅ Code Review 20260802 item 19: a null byte inside a field name can pose as a dotted path.
			- Reproduced: a single field whose name contains a null passes the unknown-field check as if it were two nested names.
			- Cause: the check joins path parts with a null before comparing.
			- Pre-existing, but the new wildcard matching is built on the same joined text.
			- Note: left as it is. Closing it means changing how the check compares paths, which touches the wildcard matching in all four, and a name holding a null byte cannot be written by hand. Kept on the list rather than closed quietly.

		- ✅ Code Review 20260802 item 20: end-of-file comments multiply when layering.
			- Reproduced: a footer comment shared by three layers appears three times in the merged output. The result still formats stably.
			- Fixed: each distinct end-of-file comment is carried over once.

		- ✅ Code Review 20260802 item 21: the default install location isn't on a normal user's path.
			- Reproduced: a system install links the program into a directory reserved for administrator sessions, so the user who ran the installer can't invoke it by name. The closing check passes because it uses the full path.
			- The project's own packages install to the ordinary location instead.
			- Fixed: a system install links into the ordinary location, matching the project's own packages, and the path note prints for both targets.

		- ✅ Code Review 20260802 item 22: the shell wrapper trusts an inherited private variable.
			- Reproduced: the wrapper caches the resolved program path in a variable it also reads from the environment, so setting that variable picks the program with no message and beats every documented lookup step.
			- The PowerShell wrapper gets this right by clearing it when loaded.
			- Fixed: the shell wrapper clears it at load too.

		- ✅ Code Review 20260802 item 23: hosted CI installs a checking tool without verifying the download.
			- The workflow pins its actions by commit and its packages by version, then fetches one tool as an archive with no checksum and installs it ahead of the system copy.
			- Fixed: the download is checked against a pinned hash before it is installed.

		- ✅ Code Review 20260802 item 24: the installers handle their own options poorly.
			- Reproduced: asking for help through the documented pipe prints nothing and exits successfully, because the script tries to read its own file, which isn't there when piped. With a stray file of the right name in the current directory it prints that file instead.
			- Also, giving an option without its value exits silently, where the program itself explains what's missing.
			- Fixed in all three scripts: the help text is carried in the script instead of read back out of its own file, and a missing option value is reported the way the program reports it.

		- ✅ Code Review 20260802 item 25: small gaps in argument handling across all four builds.
			- There is no way to end the options and pass a path that starts with a dash.
			- `init` ignores extra arguments; every other subcommand rejects them.
			- An option expecting a value will take the next flag as that value without complaint.
			- Reading the write operations from standard input while also asking for standard input as a layer silently produces nothing.
			- Fixed: `--` now ends the options, `init` rejects extra arguments, and asking for standard input twice is refused. The help text says what a value option does with the next argument, which is the one case left as it was, since that is how options normally behave.

	- **Improvements**:

		- 🔘 Code Review 20260802 item 26: the parser copies each line more than it needs to.
			- Every line is copied into a fresh string, the indent is copied again, and the path scanner copies the whole line into a character list per call.
			- The profile agrees: those three account for roughly a quarter to a third of parsing time, and they are the current top of the profile.
			- Speed is already fine in absolute terms, so this is optional. It also has to be done in all four builds to keep them in step.

		- 🔘 Code Review 20260802 item 27: Python scans character by character where a built-in would do.
			- The comment splitter and the comma splitter both loop per character on every line and value.
			- Measured: skipping the loop when the line holds no marker at all is around forty times faster on the common case. Python is the slowest build, so this is where it pays.

		- ✅ Code Review 20260802 item 28: most exported Go functions have no doc comment.
			- The style guide requires one starting with the name; about sixty were missing, including the whole writer surface. Nothing checks this automatically.
			- Fixed: 74 of them, one line each.

		- ✅ Code Review 20260802 item 29: a doc comment sits on the wrong function.
			- The description of the element parser ended up attached to the helper inserted above it, in both the reference and Go.
			- Fixed: moved onto the function it describes, matching the other two.

		- ✅ Code Review 20260802 item 30: repeat-hint suppression reads the field name out of the hint text.
			- It splits the hint's wording on quotes to recover the name, so a behavior of the public interface depends on how a message is phrased.
			- Fixed: the wording is built in one place and both the hint and the filter use it, so a reword moves them together. A carried field was the stricter fix but would have broken every caller that builds a diagnostic.

		- ✅ Code Review 20260802 item 31: index conversions would truncate on a 32-bit build.
			- A selector index above four billion would wrap and select the wrong element instead of finding nothing.
			- Not reachable on any current target. Noted so the remaining ports don't inherit it.
			- Fixed in the reference with a checked conversion. The other three were audited and already compare before indexing, so they were already safe.

		- ✅ Code Review 20260802 item 32: the grammar file disagrees with the parser in a few places.
			- The parser accepts a leading plus on an index, and allows dots and other characters inside a selector; the grammar says neither.
			- The bare-name character ranges include two characters the neighbouring comment says are excluded.
			- Unknown escape pairs are kept as written rather than rejected, and a backslash shields the next character in more places than the grammar shows.
			- Needs a call on each: tighten the parser, or widen the grammar to match it.
			- Decided: the parser is the contract, since it is released, so the grammar and spec were widened to describe what it accepts. Quoting is now stated as a rule about what the formatter writes, not about what input is legal.

		- ✅ Code Review 20260802 item 33: several public documents claimed things the code doesn't do.
			- Fixed: the contributor notes named the wrong toolchain requirements, the design notes listed platforms that aren't built and a wrapper that doesn't exist, the schema vocabulary list was missing its two newest keys, and both files still described the project as heading toward its first release.
			- Fixed: the readme's install note, one interface name in an example, and the missing PowerShell option syntax.
			- Fixed: the changelog was missing one new public function, and overstated that the writer refuses name wildcards, since removal across them works.
			- Fixed: the style guide described a comment divider spelling Python doesn't use.

- **20260727**:

	- **Improvements**:

		- 🔘 Code Review 20260727 item 2: the reference clones a node name several times per index remap.
			- `remap_child` runs on every in-place value mutation - an empty field being filled, a stacked-list element being appended - and clones the name up to five times, because it looks a key up and then removes it separately.
			- Building each key once and reusing it would about halve that, with no change in behavior. The other three bindings have their own shape and would need their own look.
			- Not evidence-backed: the profiler puts the leaders elsewhere (path scanning, code-point iteration, comma splitting), so this is a code-reading finding, not a measured one. Worth doing only if a profile ever points here.

- **20260725**:

	- **Improvements**:

	- 🛠️ Code Review 20260725 item 28: give the loader opt-out limits.
		- Nothing bounds input size, nesting depth, node count or array length in any binding, and parse costs 35-100x the input in memory.
		- A consuming program handed a config path from a user, a shared directory or a container volume has no way to refuse something unreasonable.
		- ✅ Done: the depth half, done with item 2 - a fixed 512-level cap and `E016`.
		- ✋ Deferred post-1.0: the depth cap closed the crash class. Size, node and array knobs are additive API that can land later without breaking anything, and a consuming program can bound input size itself before calling parse.

	- 🛠️ Code Review 20260725 item 29: the stable diagnostic code is derived by prefix-matching the prose it is supposed to free.
		- All four bindings recover the code from `msg.starts_with(...)` over about 30 hand-ordered prefixes, so rewording a message can change a code, and the ordering is load-bearing.
		- Separately, `V001`-`V099` were fully tabled but `E001`-`E015` and `H001` were listed nowhere, while users are told to gate CI on `check`.
		- ✅ Done: the doc half. `E001`-`E016` plus `H001` are now tabled in the spec's Diagnostics section.
		- ✋ Deferred: threading the code through every call site. Large, mechanical, and invisible to users, and every corpus case pins the code per line - so the exposure is limited to messages no case exercises.

### Done

#### Done - Bugs

- ✅ Paths() silently drops any node whose name isn't a bare identifier - and its whole subtree.
	- Fixed: non-bare segments now emit quoted and escaped via the same spelling the canonical formatter uses, so every returned path resolves. Shared fixture updated in all four runners; spec traversal section documents the enumeration. From TradeClanker, where it's a live bug: a quoted field name parses with zero diagnostics and reads back fine, but Paths() skips it, so the enumeration disagrees with what the document contains. Their unknown-field check is a Paths() walk, so a typo whose name needs quoting sails through the one check built to catch typos, and an author who writes two indicators gets one with no error. It also breaks round-tripping: a set with a quoted segment succeeds and canonical output writes it correctly, but re-parsing that output can't enumerate it - the writer produces documents the reader can't see.
	- Fix: emit the segment quoted and escaped, the form the path scanner already accepts. Silently dropping is the worst of the options; even a diagnostic would beat it.
	- The skip is currently deliberate and pinned - doc comment, spec wording, and the shared paths fixture in all four runners assert it - so the fixture and docs move together with the code.

- ✅ A strict parse hands back nothing usable.
	- Fixed both ways: the failure now carries the parsed document (Go returns it non-nil beside the error too, so the natural `doc, err :=` path can't panic), and the message names the first three diagnostics with line and code. C already returned the doc; rust/python failure objects gained the document. From TradeClanker: the Go ParseWith at Strict returns a nil Document alongside the error, so the natural `doc, err :=` then `doc.Diagnostics()` panics - and that's the shape callers get pushed into, because the error's message is only "strict load failed: N error diagnostic(s)". The error value does carry the full diagnostics list, but the obvious path hides it: every line, message, and code it's already holding is absent from the message.
	- Fix candidates, not exclusive: return the parsed document alongside the error (the diagnostics are the point of a failed strict load), and/or put the first few diagnostic lines in the message. Check what the other three bindings do on strict fail first - whatever changes has to keep the four surfaces parallel.

- ✅ `Raw` on a read result is not raw.
	- Fixed: the parser keeps the source line's value span and reads hand it back verbatim; writer-built/stacked-list/raw-block values fall back to display. Rust/go/python (the bindings whose read result exposes raw); shared fixture in those runners; spec's full-tier bullet now says exactly what raw is. Reported by the nano-git-db devs, who found it corrupting regexes: the doc comment promises the original text from the file, but every read fills it from the canonical display form, which joins elements with `, `. So `regex: ^\d{2,3}$` comes back as `^\d{2, 3}$` with zero diagnostics - and anything already written `a, b` round-trips looking correct, so it passes casual testing and fails on `{2,3}`.
	- Fix: keep the source line's value span in the parser and make Raw a slice of it; or, if that's costly, rename the field to what it actually is (canonical) and add a real verbatim text read. Their vote is true source text, and that's the better product.
	- A true-raw read also answers their separate request to read a comma-bearing scalar (a one-line regex) verbatim without resorting to a fenced block. They also floated having the reader consult the schema's declared type before comma-splitting; declined - coupling the reader to the schema buys the same result at much higher complexity.
	- All four bindings; reads are contract, so check corpus/crosscheck exposure before changing anything.

- ✅ By-value selectors match the as-written spelling, not the logical string.
	- Fixed the preferred way: escapes applied on both sides at every compare/index site (resolver, parser attach + disp_map, writer place, validator contexts) in all four bindings; spec pins the logical-string match; corpus case 033 pins reads and the write path. From convert-base-v2: `["q\"uote"]` and a document's `'q"uote'` are the same string but don't cross-match, because both sides keep escape pairs verbatim and the comparison is spelling against spelling. The failure is a silent NotFound, and anyone with a quote-bearing discriminator hits it eventually. The spec says matching is against the display form but is silent on escapes.
	- Preferred: apply escapes on both sides before comparing (least surprise). Fallback if that's judged too risky this close after rc1: one spec line pinning raw match.
	- Touches resolve, writer place(), and the parser's ByValue arm in all four bindings; corpus case either way.

#### Done - Features and enhancements

- ✅ Lower the go directive to the tested floor.
	- Declared 1.20 (strings.CutSuffix is the newest stdlib dependency; everything else predates generics). Hosted CI now installs a stable toolchain instead of reading go.mod - the pipeline's own `go -C` needs a current release - while the directive gates the consumer floor. From convert-base-v2: go.mod declares 1.24, and that directive is their recorded reason for vendoring the file instead of using it as a module - it would drag their deliberately-1.21 project up. The identical file compiles and passes their full suite under 1.21, so 1.24 is declaration, not need; find the real floor (generics suggest possibly 1.18) and declare that.

- ✅ Docs batch from the feedback round:
	- Landed in the spec (Empty-vs-NotFound as an advertised full-tier feature; a "choosing [#i] vs [value] when mapping entities" bullet in the traversal section) and in the Go package docs (a "writing a mapper" worked example: one-shot load, Count+[#i] iteration, Children for open sections, QuoteSegment, raw blocks). README deliberately untouched - it carries an in-flight edit; its pass can fold these in later. IndexOf not added - Count+[#i] covers the need without growing the surface.
	- Advertise the Empty vs NotFound distinction. convert-base-v2 mapped it straight onto their tri-state marker convention with no adapter ("empty means explicitly disabled, absent means default") and called it rare among config parsers - it belongs in the README/spec as a feature, not something discovered by reading source.
	- A spec paragraph on choosing `[#i]` vs `[value]` selectors when mapping entities: by-value misreads an entity whose name is numeric and collapses two same-named entities, and since matching is against the display form, a scalar spelled `"a, b"` and the two-element list meet the same selector. nano-git-db and convert-base-v2 each worked these out the hard way; nano-git-db suggests an IndexOf(path, value) alongside.
	- A "writing a mapper" worked example in the Go package docs: the exported surface is 60+ methods, and the shape of a real consumer - descend by path prefix, Count then `[#i]`, schema validation for line-numbered errors, fences for verbatim - cost them most of a day to discover.

- ✅ Hosted CI runs its four pinned actions on a deprecated Node.
	- Re-pinned to current-Node releases, SHA-with-tag-comment shape kept: checkout v6.1.0, setup-go v6.5.0, setup-python v6.3.0, cache v5.1.0. Green on dev.
	- All four are pinned by commit SHA and target Node 20, which the runners now force onto Node 24 with a warning on every run.
	- Working today, and the warning is the only symptom. It stops working whenever the runners drop the shim.
	- Fix is to re-pin each action to a release built for the current Node, keeping the pin-by-SHA-with-tag-in-a-comment shape.

- ✅ Add existence of PowerShell wrapper acknowledgement to design.md
	- Added to the Shell wrappers section: dual-mode design, no param block (verbatim $args), exit codes into $LASTEXITCODE, not in the differential (a forwarder).

- ✅ Glossary of terms
	- Covered by the spec's Terminology section, now extended with the wider-surface nouns (trivia, raw block/fence, canonical form, layer, schema, corpus).

- ✅ Writer output has no blank lines between top-level sections.
	- Done, default on with no knob: writer-created top-level nodes set blank_before (the emitter never blanks line 1). Write goldens 014/016/029 regenerated; spec Writer section documents the shape. From TradeClanker: hand-written examples and writer output disagree on shape, so they post-process the library's output with string surgery - which is what using the writer was meant to end. Blank-before already exists as parsed trivia, so the writer setting it when it creates a new top-level node (or a knob to) keeps the fixpoint property intact.
	- Write-corpus goldens would churn; decide the default deliberately since written output is contract.

- ✅ to_canonical() drops blank lines between comment-only regions. It shouldn't do that.
	- Fixed: each held comment records its own preceding blank (runs still collapse to one; never as first output line). Blanks between comment regions - leading, merged, and end-of-file - now round-trip.

- ✅ to_canonical() also loses comment indentation two ways, from the SilkTerm devs: a comment run trailing a block's last child re-attaches to the following node and de-indents to column 0, and orphan comments after the last binding always emit unindented. Together with the blank-line item above, fixing these lets them delete most of their save-repair pass.
	- Fixed structurally rather than by storing verbatim indent: a comment written deeper than the next binding hangs on the block it sits in (new after-trivia, emitted after the block's last child at the block's indent); the same rule keeps indented tail-of-file comments with their block. Corpus case 034 pins it; fmt stays a fixpoint (20k-iteration fuzz soak).
	- Means recording indent (and probably original attachment) as trivia; fmt must stay a fixpoint. All four parsers + emit, golden churn expected.

- ✅ Expose whether a value was quoted.
	- Done: `quoted` on the read result (rust/go/python; C read structs stay value+status by design) - true for a quoted single scalar element, false for arrays/raw/empty. From nano-git-db: `a: @null` and `a: "@null"` read identically, so a language built on shcl can't reserve a sentinel value and still let a user write it literally - they had to make `@null` unconditionally reserved and walk back docs that promised quoting would escape it. The parser already tracks quoted per element and drops it at the read boundary; one field on the read result makes "quoting is the escape" true for every downstream language.

- ✅ Line numbers on the read path.
	- Done both ways as filed: `Line(path)` accessor in all four bindings + veneer, and `line` on the read result itself (rust/go/python). 0 = unresolved or writer-built; merged instances cite the first binding, matching diagnostics. From nano-git-db: node line is populated and never exported, so any consumer check the schema can't express can only name the entity, never the line - their warnings degraded from `line 81: ...` to `table issue: ...`. A Line(path) accessor is the cheapest high-value ask in their list.
	- Second consumer, same gap (TradeClanker): parser diagnostics carry a line and reads don't, so half the errors a user sees cite a line and half don't - a bad value reports nothing while a malformed line one field above reports `line 12`. They want the line on the read result itself, not just a separate accessor; do both, it's the same plumbing.

- ✅ Enumerate a node's children.
	- Done: `Children(path)` in all four bindings + veneer - file order, duplicates included, empty path = top level; names as stored, QuoteSegment for splicing. From nano-git-db: Paths() dedupes, so there is no way to ask "what keys are under this section?" - they hardcode a ten-name hook list purely because they can't ask what's under `code:`, and any open-ended or map-shaped section is unmodellable. Children(path) returning child names in file order, duplicates included, deletes more of their workaround code than anything else they asked for.
	- TradeClanker asks for the same thing for the same reason (reading an open section means scanning all of Paths() and prefix-matching), and notes it also sidesteps the Paths() quoted-name bug for that use.

- ✅ Hint when a merge combines non-adjacent bindings.
	- Done: H002, hint severity, at the later line with the earlier line named in the prose. Fires only on a last-segment no-selector merge into a non-last child - adjacent re-mentions and the dotted redundant-path idiom stay silent. Corpus case 035; 001/013 diags goldens gained one each. From nano-git-db: merge-by-(name, value) means two separately-written `table: t` sections silently become one combined table, and only the parser can know it happened - their consumer-side "already defined; first wins" check was unreachable and got deleted as dead code. A hint-severity diagnostic carrying both line numbers would make that class of check possible without touching the documented merge semantics.
	- New H-code; diagnostics are contract, so all four bindings plus the expected-diags goldens move together.

- ✅ Suppress H001 where the schema declares the repetition.
	- Done at check --schema assembly (and inside the one-shot), via a shared library helper so CLIs and runners can't drift; matched by leaf name, the filter consumers hand-rolled. Corpus case 036. From nano-git-db: the repeated-bare-leaf hint is structurally a false positive for any field whose repetition is the instance mechanism (`unique:`, `index:`, `row:`), so every correct file warns on load and users learn to ignore warnings; they filter it by hand for exactly three names. When a schema is present and a path declares repeat with an upper bound above 1, drop H001 there - the information already exists and goes unused.
	- H001 is parse-time and the schema arrives at validate, so the suppression belongs where check --schema assembles output, not in the parser.

- ✅ One-shot load-and-validate, plus an error predicate.
	- Done: LoadAndValidate(text, schema, strictness) -> document carrying one combined diagnostics list (never throws; empty schema skips validation; declared-repeat H001s dropped), plus ErrorCount() on the document. All four bindings + veneer. From nano-git-db: doc diagnostics and schema validation come back as two separate lists that must be merged by hand (forget one and half the errors vanish), and there's no HasErrors/ErrorCount, so "did this file have errors" is consumer bookkeeping - which matters, since recover-and-continue means a mixed-indentation file otherwise returns no error at all. A combined entry point (text + schema + strictness in, doc + one diagnostic list out) plus an error count removes a whole class of consumer mistake.
	- TradeClanker independently hand-rolled the same thing: no strictness level means "forgiving about spelling, loud about a dropped line", so at Standard an error diagnostic comes back beside a nil error and the line is silently skipped, and every consumer writes the same fifteen lines. An Errors() helper on the document is the smallest shape of this ask, from a second consumer.

- ✅ Setters should say why they failed.
	- Done as an additive probe (setter returns are frozen post-1.0): `WriteReason(path)` in all four bindings + veneer runs the writer's exact validation without creating anything and names the failure - Writable/BadPath/ValueInPath/Wildcard/NoSuchIndex/TooDeep. place() now pre-gates on it, so the two can't drift. CLI stays exit 1. From TradeClanker: Set* returns a bare pass/fail, and false covers an empty path, a malformed path, a wildcard, a depth overrun, and an unresolvable index indistinguishably - their workbench's error message is literally a guess because the library won't say. A small reason enum (or per-binding equivalent) on the write result fixes it; CLI behavior stays exit 1.
	- All four bindings move together; the write corpus pins outputs, not the reason surface, so exposure should be small - verify.

- ✅ Building a path from a user-typed name is injection.
	- Done: QuoteSegment/quote_segment/shcl_quote_segment + veneer, sharing the emitter's name-quoting - same spelling both directions as the Paths() fix. From TradeClanker: Set* accepts segments that aren't valid bare names, and a dotted name is silently reinterpreted as nesting - a consumer concatenating a user-typed indicator name into a path is doing path injection without knowing it. Export a quote-segment helper (the escaping the path scanner already understands), or segment-wise setters taking a list that can't be injected into. Pairs naturally with the Paths() quoted-name bug above - same escaping code both directions.

- ✅ Dev-environment install script (Linux, macOS, Windows), runnable via a single `curl` or `wget`. Clones, installs dependencies, states what it will do with an option to abort.
	- Done: `install-dev.bash` at repo root. Linux + macOS directly; Windows via WSL (the dev pipeline is bash). Clones (or detects an existing clone), installs the no-sudo pieces itself (rustup, ruff/mypy/cppcheck via pipx, markdownlint via npm, PSScriptAnalyzer via pwsh), and prints the exact package-manager hint for what needs root (go, python3, gcc, shellcheck). Shellcheck-gated.

- ✅ Release-install script, `install.bash` and cross-platform `install.ps1`. In [repo] root, usage instructions in README.md.
	- Done: both at repo root, README Installing has the one-liners. Latest release via the GitHub API (`--release dev` = newest incl. pre-releases, default; `stable` = newest full release), binary picked by OS/arch, sha256-verified, `code/` (drop-in files) and `scripts/` (wrappers) pulled from the tag's source tarball. Idempotent (atomic binary swap); states the plan and confirms first.
	- Decisions along the way: `objects/` skipped - nothing statically-linkable is published yet (revisit with packaging). Linux user install lands in `~/.local/share/shcl` with the `~/.local/bin/shcl` symlink (one path can't be both the dir and the symlink). Windows user install adds its own dir to the user PATH instead of symlinking (symlinks need elevation); it landed in `%USERPROFILE%\bin\Shcl` at first and moved to `%LOCALAPPDATA%\Programs\Shcl` before 1.0.0. macOS/BSD get a clear "no prebuilt binaries yet" pointer at build-from-source.
	- Behavior:
		- Runnable via a single `curl` or `wget`. Downloads, installs, and runs the latest release, with an option to abort.
		- Idempotent. Updates existing.
	- Args:
		- `--release[=]<dev[elopment]|stable>` Install latest dev rather than
		- `--target[=]<user|system>`
			- System (default):
				- Linux:
					- /opt/shcl/
						- shcl  ## Rust binary
						- code/  ## Drop-in code files.
						- objects/  ## Or a better name for per-language statically-linkable objects)
						- scripts/  ## .bash and cross-platform .ps1 wrappers
					- /usr/local/sbin/shcl  ## Symlink
				- Windows:
					- C:\Program Files\Shcl\
						- shcl.exe  ## Rust binary
						- code/  ## Drop-in code files.
						- objects/  ## Or a better name for per-language statically-linkable objects)
						- scripts/  ## .ps1 wrapper
					- Add C:\Program Files\Shcl to %PATH%.
				- macOS: ?
				- BSD: ?
			- User:
				- Linux:
					- ~/.local/bin/shcl/
						- (Same subdirs under here as system install)
					~/.local/bin/shcl  ## Symlink
				- Windows:
					- %USERPROFILE%\bin\Shcl
						- (Same subdirs under here as system install)
					- %USERPROFILE%\bin\shcl.exe  ## Symlink
					- Add %USERPROFILE%\bin\ to %PATH%.
				- macOS: ?
				- BSD: ?

- ✅ Schema-as-SHCL validation. The schema is a plain SHCL file (type, required, allowed values). `Validate(doc, schemaDoc)` returns structured diagnostics and catches unknown or misspelled fields. See `design.md` "Schema validation".
	- Done: flat `field: <path>` schema shape, no grammar change; `Validate` in all four drop-ins plus the C++ veneer; `shcl check --schema` in all four CLIs; `V###` diagnostic codes; corpus cases 021-024 with a validate golden per case; crosscheck replays `check --schema`. Regex constraints and datetime ranges rejected for cross-binding parity; normative section in `spec.md`.
	- Note: needs the reference parser first, then spec the schema vocabulary alongside it.

- ✅ Expand the conformance corpus (`conformance/`) to cover the hard edges: dates and ambiguity, coercion, quoting and escapes, indentation errors, raw blocks, selectors and wildcards, strictness levels.
	- Done: cases for stacked arrays, coercion bundles, strict-load behavior, forgiving commas, raw blocks, and the full date whitelist.
	- ✅ Model diagnostic expectations (count, severity, the mandatory repeated-leaf hint). Every case now carries an `expected-diags.txt` golden (exact `check` stdout at standard: line/severity/code per diagnostic + the summary line), verified natively by all four runners. Pins the `H001` repeated-leaf hint and the zero-diagnostic cases too.

- ✅ Layered loading. `Load(defaults, site, user, ...)` merges later over earlier via the existing merge rule, with CLI and env overrides on top.
	- Done: `merge(base, over)` in all four drop-ins plus the C++ veneer; leaf names override, containers merge by `(name, value)`. CLI `--layer=FILE` (repeatable) on get/fmt/count/instances/set, `--set=PATH=VALUE` as the top layer; `fmt` doubles as merge. Env mapping dropped (belongs to the consuming program). Corpus case `025-layered` + `expected-merged.shcl` golden, all four native runners, crosscheck replays `fmt --layer/--set`; normative spec section.

- ✅ Schema-driven generation. Writer plus schema emits a commented, typed starter config (`shcl init --schema ...`). Depends on schema validation.
	- Done: `generate(schema)` in all four drop-ins plus the C++ veneer; `shcl init --schema=FILE` in all four CLIs. Required fields live, optional commented, wildcards in a trailing block; `desc` -> comment, a fixed-format annotation line summarizes type/constraints (byte-for-byte parity contract). Uses the schema's `default`/`desc` vocabulary. Corpus case `026-init-schema` + `expected-init.shcl` golden, all four native runners assert output + clean reload, crosscheck replays `init --schema`; normative spec section.

- ✅ Language spec rationalized from `notes.txt`: terminology, model, types, accessor and writer API, formatter, raw blocks. See `spec.md` and `grammar.abnf`.
	- ✅ stacked (`*`) block-array form alongside inline commas. Both spellings read the same and canonicalize to inline.
	- ✅ date and time formats pinned to a closed whitelist (year-first or named-month only, then calendar-validated).
	- ✅ adoption sweep. Cut the currency, percent, float-to-int rounding, and extra boolean tokens from default behavior. Case folding restricted to ASCII. Repeated-leaf hint made mandatory.
	- ✅ three strictness levels (loose, standard, strict) as a per-document knob, default standard. Loose re-admits the cut coercions; strict fails the load on any error.
	- ✅ bindings re-tiered. Tier 1 reference plus CLI; Tier 2 Go, C, Python; Tier 3 the rest, post-v1.0.
	- ✅ raw-block binding reworked. A fence is a value line for its parent field. Child-indent spelling is canonical.
	- ✅ inline-array commas made fully forgiving. Stray commas drop their empty slots and never error.
	- ✅ Model diagnostic expectations (count, severity, the mandatory repeated-leaf hint) in the corpus. See conformance item below.

- ✅ Tier 2 ports:
	- Languages:
		- ✅ Go
		- ✅ C (with a C++ veneer)
		- ✅ Python
	- Each an independent parser with the same flags, output, and exit codes as the reference. All corpus-green and held byte-for-byte to the reference on every build.
	- ✅ Shell wrappers around the CLI, not parsers.
		- ✅ Bash wrapper (`source/bash/shcl.bash`).
			- Runs as a script or sourced for typed helpers. Forwards the binary's exit code unchanged.
		- ✅ PowerShell wrapper (`source/powershell/shcl.ps1`).
			- Runs as a script or dot-sourced for the same typed helpers; forwards the binary's exit code in `$LASTEXITCODE`. Cross-platform binary resolution (also matches `.exe`).

- ✅ A CI/CD pipeline driven by `cicd/cicd.bash`: builds, tests, and can commit and push. Packaging and publishing are opt-in. See `design.md` for the split-by-responsibility rationale.
	- ✅ all stages live. Format check, build, lint, tests plus fuzz smoke, profiler, native and cross builds, versioned artifacts, README demo gif, publish.
	- ✅ `--ci` mode is the correctness gate the GitHub workflow runs, so local and CI share one definition of passing.
	- ✅ cross-binding differential check. Every binding CLI must agree with the reference byte-for-byte on the corpus and a fuzz-dumped input set.
	- ✅ dogfood stage installs the fresh release binary to a fixed local dir. Off under `--ci`, no sudo path.
	- ✅ lint stage widened to every binding. ruff and mypy for Python, cppcheck for C, markdownlint for docs, PSScriptAnalyzer for the ps1 wrapper. All gating, locally and in CI; setup steps in `contributing.md`.
	- ✅ demo gif: 50 fps motion, the screen clears between commands, gifsicle pass. 2.8 MB -> 0.3 MB.
		- The `check` step needed `expect_exit = 6`; since `check` started exiting on errors it had been aborting the stage, so the committed gif was stale.
		- ✅ demo gif: opens by naming both usage modes (CLI and drop-in library), the formatter step says values are never rewritten, and the loop seam cuts to black instead of crossfading. 0.3 MB -> 0.2 MB.
		- ✅ demo gif: output that fits on one screen now arrives at once like a real terminal, only an overflowing view scrolls, the cursor no longer blinks, and every motion frame is exactly 50 fps.
			- Nothing had actually been scrolling: the window is 22 rows and both long outputs fit, so the lines were popping in one at a time on a timer. That was the stepping.
			- Scroll smoothness is pixels per frame, so the step is rounded to an exact divisor of the line height - a line boundary never lands mid-step.
			- ✅ demo gif: the cursor blinks again while the prompt is idle, glides at sub-pixel resolution, and this demo drops the blank line between output and the next prompt.
				- The block is drawn with coverage-blended edges instead of snapping to whole pixels, so a 3 px per frame glide reads as continuous.
				- No padding line after output: the demo is about exact output shape, and a blank line invites misreading it. Scenario knob `blankafter`, on by default.
	- ✅ Packaging (.deb, .rpm, NSIS). Wire it when release cuts start.
		- Done: stage 6 builds .deb + .rpm (nfpm) per Linux binary and an NSIS setup per Windows binary into the release artifact dir, covered by the same sha256sums. `--no-package` to skip; off under `--ci` and `--quick`. Packages use distro layout (/usr/bin + /usr/share/shcl); payload matches install.bash.

- ✅ Accessor: two-tier junior-friendly surface (convenience default plus full status), consistent across all bindings. A supplied default implies default mode.
	- Done alongside review item 6: the full status tier (`Read`/`read_*`) already shipped; the convenience default tier now ships in every library binding (`GetIntOr`/`get_*(default=)`/`shcl_get_*`/`get_or<T>`/native `unwrap_or`), plus the CLI `--default` for the wrapper bindings. Supplying the fallback is Default mode - value on `Good`, fallback otherwise.

- ✅ README rewrite: problem-first pitch, file and read-call examples, a format comparison table, a "wrong choice" section, and honest alpha status.

#### Done - Code reviews

- **20260727**:

	- **Ehnhancements**:

		- ✅ Code Review 20260725 item 37: harden the installers' transport and integrity story.
			- `curl` and `wget` follow redirects with no protocol pin or TLS floor.
			- The sums file arrives over the same channel as the binary, so it catches corruption but not substitution. The source tarball, which supplies the drop-in files consumers compile in, is not verified at all.
			- ✅ Done: the transport half. curl and wget pin https through redirects with a TLS 1.2 floor (`install.bash`, and `install-dev.bash`'s rustup fetch); `install.ps1` floors TLS 1.2/1.3 for every download.
			- ✅ Done: the signature half, ahead of 1.0.0. The sums file is signed offline with an RSA-4096 key; both installers carry the public half inlined and verify it before reading any checksum out of the file. `openssl` joined curl/wget as a hard prerequisite - there is no install-anyway fallback. `cicd/utility/sign-release.bash` does the signing and refuses if the key does not match the one the shipped installers trust.
			- Key custody is offline and signing is manual, deliberately: a key in CI would be reachable by the same compromise the signature defends against. RSA rather than Ed25519 purely so the verifier is one both `openssl` and Windows PowerShell 5.1 already have. Rationale and threat model in `design.md`.
			- Still not covered: the source tarball that supplies the drop-in files. It is fetched by tag, so it is as trustworthy as the tag, but it carries no signature of its own. Worth a look post-1.0.

		- ✅ Code Review 20260727 item 1: the Windows user install goes somewhere Windows does not expect.
			- `install.ps1` puts a `user` target in `%USERPROFILE%\bin\Shcl`. The convention for a per-user program is `%LOCALAPPDATA%\Programs\`, which is where winget and most installers put one.
			- Low stakes while the only release is a pre-release, and cheap to change now. Later it means a migration step for anyone who already installed.
			- The system target (`C:\Program Files\Shcl`) is already conventional.
			- Done before the 1.0.0 cut, which was the last moment it stayed free: `user` now lands in `%LOCALAPPDATA%\Programs\Shcl` and that dir goes on the user PATH directly. The old shape needed a second copy of the exe in a parent `bin` dir for PATH to find it; that whole branch is gone.

- **20260726**:

	- **Bugs**:

		- ✅ Code Review 20260726 item 1: an in-place write does not preserve the file it replaces.
			- Cause: `fmt --write` and `set --write` build a temp file and rename it over the target, so the target's own identity is discarded.
			- Reproduced: a config at mode 600 comes back at the umask default, which on a normal box means world-readable. Config files are exactly where secrets sit, so this is the one that matters.
			- Reproduced: a symlinked config is replaced by a regular file. The link breaks, the real file behind it keeps the old content, and the edit appears to vanish. Dotfile managers make this a common layout.
			- Reproduced: hard links break the same way - the other name keeps the old content.
			- Fixed: all four bindings resolve the path through symlinks before choosing the temp directory and the rename target, then copy the original's mode onto the temp file. Mode copying is best effort, so a filesystem that cannot carry it still completes the write.
			- Note: hard links cannot survive a rename at all. Atomicity is worth more, so that one is a documented limitation rather than a fix.
			- Verified: the differential harness gained an in-place-write dimension comparing the tree a write leaves behind - mode, symlink, link count, content - which pins all three cases across the bindings.

		- ✅ Code Review 20260726 item 2: C datetime reads accumulate in the never-freed document arena.
			- Cause: the read path hands the document arena to the datetime parser for its split temporaries, so a long-running reader grows without bound. Every other C read path had moved to the per-call scratch arena; these two were missed.
			- Reproduced: 1M datetime reads of a 45-byte document reach 377 MB. The same loop reading an int stays flat at 2 MB.
			- Fixed: both datetime read paths allocate their temporaries from scratch. The fractional-seconds value they hand back points into the document text, which outlives the call, so the return contract is unchanged.
			- Verified: 1M reads now stay flat.

		- ✅ Code Review 20260726 item 3: the fuzz run is documented as deterministic but is not reproducible.
			- Cause: the mutation PRNG is fixed-seed, but its seed inputs come back from the corpus directory in whatever order the filesystem gives. That order decides every mutation.
			- Reproduced: the cross-binding comparison count moves by a few hundred between runs on an unchanged tree, which makes the number useless as a regression signal. A failing case also cannot be re-run, which is the main thing a fuzz gate is for.
			- Fixed: the seed list is sorted before use.
			- Verified: two runs on the same tree now produce an identical input set.

		- ✅ Code Review 20260726 item 4: the cross-binding harness reports only the first divergence.
			- Cause: it prints a divergence, then pipes `diff` into `head` to show it. Under the script's own strict settings that pipeline's nonzero status aborts the run, so every later divergence is lost and the "N of M diverged" summary never prints.
			- Note: never a correctness hole. The run still exited nonzero and the gate still failed; the cost was diagnosis, one divergence at a time with no count.
			- Reproduced: only because a new check was deliberately run against a known-bad build. An error path that is itself fatal stays untested until the day it is needed.
			- Fixed: the diff is allowed to fail. All divergences print, followed by the summary.

		- ✅ Code Review 20260726 item 5: hosted CI cannot install its own pinned lint toolchain.
			- Cause: the tool pins carry the Cppcheck binary's version, and the workflow installed that same string as a package version. No such package exists, so every hosted run failed at setup within seconds.
			- Note: the wheel bundles a Cppcheck two major versions ahead of its own package number, which is what made the two look interchangeable.
			- Note: only the hosted gate was affected. The local pipeline probes the installed binary, so it stayed green, which is why this went unnoticed from the day the pins were added.
			- Fixed: the workflow installs the package version, with a comment saying why the two differ.
			- Verified: the other three pins do resolve.

		- ✅ Code Review 20260726 item 6: shellcheck is a gating linter but the only one left unpinned.
			- Cause: hosted CI used whatever the runner image shipped, which is older and noisier than the local copy, so a script could pass the local gate and fail CI on a warning newer releases no longer emit. That is what happened to `install.bash`.
			- Note: the pinning pass that covered the other linters missed it because it is preinstalled rather than installed by a step, so there was no version to write down.
			- Fixed: CI installs the pinned version ahead of the image's copy, and the pin joins the others in the drift list. The one flagged line in `install.bash` was rewritten to the shape the rest of that file already uses.
			- Fixed alongside it: the drift probe read only the first line of a version command's output, so a tool that leads with a banner always looked like it had drifted.

- **20260725**:

	- **Bugs**:

		- ✅ Code Review 20260725 item 1: a higher layer that names a container with no children deletes the whole subtree below it.
			- `server:` (or `server: web1` with an empty body) in an over layer wipes every child the lower layers put there, silently, exit 0.
			- Worse than it reads: the wipe covers every same-named instance, so mentioning `server: web1` also deletes an untouched `server: web2`.
			- A body that is only a comment counts as empty, because comments are trivia rather than children.
			- Fixed: the leaf-override path now applies only when the base side of the name group is also all-childless, so a bare section header merges (matching instance untouched, unmatched appended as an empty instance) and leaf clearing still works. No way to blank a section from a higher layer; a deletion spelling is deferred post-1.0. All four bindings, spec reworded, corpus case 027.

		- ✅ Code Review 20260725 item 2: deep documents crash three of the four bindings, each at a different depth.
			- Fixed with a load-time nesting cap: 512 levels, enforced in all four parsers before any node is created (`E016`, line skipped) and mirrored by the Writer's `place`. Every measured cliff sat far above the cap, so the existing recursion is now safe everywhere; the reference keeps its recursive walks on purpose (structural parity beats a one-binding rewrite). Spec gained the cap and the load-code table; corpus case 028 pins the error, a reference test pins the 512 boundary and the writer refusal.
			- Emit and merge recurse per nesting level while the parser is iterative, so a document parses fine and then dies when anything formats or merges it.
			- Python merge is the acute one - a 4.9 KB file is enough. The reference aborts on `fmt` around 33k levels; C segfaults; Go survives by growing to tens of GB.
			- A dotted path buys one level per two bytes, so depth is cheap for an attacker and needs no odd syntax.
			- Same input, four different exit codes: the inverse of the product guarantee.

		- ✅ Code Review 20260725 item 3: `shcl set` validates its op values four different ways.
			- Rust and Go reject a malformed int; Python accepts unbounded ints, underscores, padding and non-ASCII digits; C silently writes 0 or a truncated/saturated value and exits 0.
			- Python can emit an integer no binding can read back, so the Writer produces out-of-contract output.
			- The ops script on stdin also skips the UTF-8 gate the file reader applies, so bad bytes become U+FFFD instead of exit 1.
			- stdout and exit codes are contract here and the crosscheck replays `set`, so this only escapes CI because no corpus op line carries a malformed value.
			- Fixed: go/python/C now gate op values with the reference's exact grammar (sign + ASCII digits + i64 range for ints, the Rust f64 grammar for floats - overflow stores `inf`); C's truncating staging buffers are gone; the ops stdin gets the same UTF-8 gate as file input in all four. Corpus case 029 pins accept and reject sets cross-binding (`write-bad.ops` dimension in all four runners + the differential harness).

		- ✅ Code Review 20260725 item 4: Go `Validate` panics on a schema path with a `[#N]` selector at N >= 2^63.
			- `int(seg.sel.index)` wraps negative, the bounds check passes, and the index panics. The three sibling sites in the same file compare against `uint64(len(...))` correctly.
			- Hard process abort inside a library call, which is exactly what the status-as-data design exists to avoid.
			- One-line fix, Go only.
			- Fixed: the bounds check compares in uint64 like the sibling sites.

		- ✅ Code Review 20260725 item 5: C keeps every transient allocation in the never-freed document arena.
			- Reads, merges and validation all allocate scratch there, so a read-only document grows without bound in a long-running process.
			- Measured: 1M reads of a 30-byte document reach 1.13 GB; a 626 KB layer merge peaks at 9.8 GB where the reference uses 27 MB.
			- Rust, Go and Python free the same temporaries per call, so this is C-only.
			- Fixed: the doc gained a scratch arena reset on entry to every resolve (path scans, resolver vectors, compare strings, int/float/bool coercion temps) and on merge entry; `w_replace_leaf_group` rebuilds the children vector in place; `v_suggest` gets a per-field-reset scratch for its DP rows and chains. Returned bytes still live in the persistent arena per the documented contract (string reads accumulate only their returned copies). Measured: 1M int reads 1.13 GB -> flat 2 MB; 16k-sibling merge 2.53 GB -> 38 MB; 2k-field validation 1.38 GB -> 17 MB.

		- ✅ Code Review 20260725 item 6: C grows a stacked `*` list one element at a time, so parsing it is quadratic in memory.
			- A fresh array is allocated and copied per `* ` line, and the arena keeps every discarded copy.
			- An 11 KB file already costs 38 MB; 95 KB costs 2.4 GB; 249 KB costs 14.8 GB.
			- Reachable from a plain `fmt` or `check` on an ordinary-looking config.
			- Fixed: the element array grows geometrically, and the per-element merge-key rebuild is deferred while the list is the open field (flushed before any other map lookup, so behavior is unchanged). 20k elements: 14.8 GB and 40 s -> 7.6 MB, instant; output byte-identical to the reference. The other bindings' key-rebuild time cost is item 24's territory.

		- ✅ Code Review 20260725 item 7: `fmt --write` truncates the config in place, and C reports success when the write fails.
			- C never checks `fwrite`/`fclose`, so a failed write prints nothing and exits 0 while the other three exit 1 - a live exit-code divergence.
			- No binding uses temp-file-and-rename, so an interrupted write destroys the file the tool exists to protect. The dogfood installer was made atomic for this exact reason.
			- Fixed: all four CLIs write through a temp-file-and-rename in the target's directory (data synced before the rename), and C checks every stdio call, so a failed or interrupted write exits 1 and never leaves a truncated file.

		- ✅ Code Review 20260725 item 8: both Windows installers damage `PATH`.
			- NSIS reads `PATH` through a 1024-char string and writes the truncated value back. Observed in a wine run: a 1427-char machine PATH came back as 22 chars. The uninstaller does the same.
			- `install.ps1` reads `PATH` expanded and writes it back as `REG_SZ`, baking `%USERPROFILE%`-style references and downgrading the value type.
			- The two Windows install paths also disagree with each other about how to test for an existing entry.
			- Fixed: the NSIS installer/uninstaller no longer pass PATH through NSIS strings at all - a generated PowerShell script edits the registry value directly (unexpanded read, segment-wise case-insensitive test, REG_EXPAND_SZ preserved), and a failed edit warns instead of writing back a truncated value. `install.ps1` likewise reads unexpanded and writes REG_EXPAND_SZ, with a settings-change broadcast.

		- ✅ Code Review 20260725 item 9: `shcl init --schema=X` generates a config that `shcl check --schema=X` rejects.
			- The generator only consults `required`; `repeat` with a lower bound of 1 or more is ignored entirely.
			- A wildcard `required` bites whenever some other live path materializes the wildcard's parent.
			- The project's own corpus golden fails against its own schema, so the two newest features contradict each other on the fixture that is supposed to pin them.
			- Fixed in all four generators: fields with a `repeat` lower bound of 1+ generate live like `required`, and a must-exist wildcard whose parent gets materialized by another live line is generated in dotted form under that instance. Generated output now validates clean against its own schema (asserted by every runner; the one documented exception is a repeat lower bound of 2+, which cannot be auto-satisfied). Case 026's golden regenerated.

		- ✅ Code Review 20260725 item 10: `shcl init --schema` can emit lines that do not parse.
			- A `[#N]` selector puts a `#` on the field line, which starts a comment, so the line truncates and reports E014.
			- A newline inside an `allowed` value breaks out of the annotation comment and injects a live binding.
			- The spec promises the generated file loads with no error diagnostics.
			- Fixed: `[#N]` paths (and paths carrying a literal newline) go to the trailing not-generated block instead of emitting a broken line; newlines in the annotation are escaped to `\n`; a default with a newline is written in its quoted escaped spelling. Corpus case 030 pins all three.

		- ✅ Code Review 20260725 item 11: an unterminated quote in a value is accepted silently and swallows the trailing comment.
			- The stray opening quote stays in the value, no diagnostic at any strictness, and `fmt` then re-quotes it so the typo looks deliberate.
			- Comment stripping is quote-aware, so `listen: "0.0.0.0:443  # note` eats the author's comment into the value.
			- One of the commonest hand-authoring mistakes, and the exact class of error the product markets itself as catching. Path position already diagnoses it; only value position is silent.
			- Fixed: a value or array element that OPENS with a quote it never closes now draws `E017` (value kept as written, fails strict) in all four parsers - field values and stacked `*` elements both. Mid-text apostrophes (`it's fine`) stay legal prose. Corpus case 031.

		- ✅ Code Review 20260725 item 12: a write to an unusable path silently succeeds and can leave a half-created path behind.
			- `place()` creates intermediates as it walks, then returns nothing on a wildcard or a missing `[#N]` - the nodes it already made stay.
			- `--set=server[*].port=1`, `--set==v` and a typo'd path all print the untouched document and exit 0.
			- For the mechanism the spec designates as the override surface, a typo that silently does not apply is the worst available failure mode.
			- Fixed: `place` pre-validates the whole path before creating anything (wildcard, missing `[#N]`, value part, past the depth cap), every setter reports whether the write applied, and the CLIs exit 1 on a rejected op or `--set` instead of printing the untouched document.

		- ✅ Code Review 20260725 item 13: the Writer can create two siblings with the same name and value, so its output is not a formatter fixpoint.
			- The Writer deliberately skips the parser's merge index and nothing else applies the merge rule, so re-reading its output collapses the pair and loses an instance.
			- The existing fuzz misses it because it always starts from an empty document and never has a sibling to collide with.
			- Fixed: after a value write, a merge-rule collision with a sibling folds the pair the way a reparse would (earlier survives; children and trivia fold in), in all four bindings.

		- ✅ Code Review 20260725 item 14: the C CLI caps `--layer` and `--set` at 64 each; the other three are unbounded.
			- Past 64 the C CLI exits 1 with empty stdout while the others exit 0 and print the merged document.
			- The cap appears in neither the spec nor the usage text, on an option the spec expects programs to generate in bulk.
			- Fixed: layers, sets, and positional args all grow dynamically now (the fixed 64/65/8 arrays are gone); verified with a 70-layer merge.

		- ✅ Code Review 20260725 item 15: `shcl_datetime_str` writes past its documented 64-byte buffer.
			- `frac` is a public field with no cap and is `memcpy`'d unbounded; the public `shcl_set_datetime` passes a 64-byte stack array.
			- Not reachable from parsed input - the parser bounds every component - so this is a defensiveness gap in a public API, not an input-driven hole.
			- Fixed: the frac copy is capped so the render always fits the documented 64 bytes; header doc states the truncation.

		- ✅ Code Review 20260725 item 16: option validation is per-subcommand for two options and global for everything else.
			- `shcl set --write FILE` is accepted and discarded, so a user copying the habit from `fmt --write` gets exit 0 and an unmodified file.
			- Also silently ignored: `--schema` on `get`, `--slots` on `count`, `--int` on `fmt`.
			- Either implement `set --write` or reject it; silently dropping it is the one case that must not stay.
			- Fixed: `set --write` is implemented (atomic in-place rewrite, `-` rejected), and every subcommand now validates its options against an allow-list - an option it does not use is a usage error (exit 1), in all four CLIs.

		- ✅ Code Review 20260725 item 17: `check --schema` cannot tell a schema line number from a document line number.
			- Both files' diagnostics interleave in one list with nothing marking which file a line belongs to.
			- `init` already prefixes `schema line N`, so the same fault renders two ways in two commands.
			- Fixed with the cheap half: the stderr prose spells `schema line N` for `V090`-`V093` (whose numbers are schema-file lines per the code table) in all four CLIs; the compared stdout keeps the uniform shape since the code already names the space. The structural `source`/`column` fields stay future work.

		- ✅ Code Review 20260725 item 18: `Load(defaults, site, user)` is documented but exists in no binding.
			- README presents it as the layered-loading API; only `merge(base, over)` exists, so a reader's first call does not compile.
			- README also advertises environment overrides, which were deliberately dropped, and its Status block still lists three shipped features under "not done yet".
			- Same class as the prior review's item 6, regenerated by the newest features.
			- Fixed: the README's layered-loading bullet now shows the real `merge(base, over)` fold and the CLI `--layer`/`--set` stack, notes the deliberate no-env-mapping stance, and the Status block lists the three shipped features instead of calling them not-done.

		- ✅ Code Review 20260725 item 19: the spec's ergonomic-tier table does not compile for C++ and misstates the C signatures.
			- `doc.get_or<int>(...)` and `doc.get<int>(...)` fail as a link error - the least actionable diagnostic a junior can get - because `get<T>` is specialized for only four types.
			- The C rows drop the `plen` argument and name `shcl_get_int_ex`, which does not exist.
			- This is the table the spec points a junior at.
			- Fixed: the C rows show the real length-delimited signatures (`shcl_get_int(doc, path, plen, 0)` / `shcl_read_int`), the C++ rows use `int64_t`, and the veneer's `get<T>` primary template now carries a static_assert so an unsupported `T` (a bare `int` included) fails with a readable message instead of a link error.

		- ✅ Code Review 20260725 item 20: Go and Python `clone_subtree` share element storage with the `over` document instead of copying it.
			- Latent only - no public API mutates a value in place after parse today - but the docstrings and spec both say the content is copied.
			- One line per port, and exactly the structural drift the parity rule exists to prevent.
			- Fixed: the Go clone copies the element backing array, the Python clone builds a fresh value (elements included), matching the reference and C.

		- ✅ Code Review 20260725 item 21: C and C++ `generate()` give no way to see which schema line is at fault.
			- `shcl_generate` signals failure with a bare `ok` flag and discards the fault list the other three return, so C `init` prints only the summary.
			- Reachable today through `shcl_validate` against an empty document, so the CLI can be fixed without reshaping the API.
			- Fixed via the review's own suggestion: on a generation failure the C CLI validates an empty document against the schema and prints the reproduced V09x fault list; the veneer documents the same trick. No API reshape.

		- ✅ Code Review 20260725 item 22: the same schema fault exits 1 through `init` and 6 through `check --schema`.
			- Exit 1 is documented as "usage or I/O error", which a semantically broken schema is neither.
			- A pipeline that reads 1 as "invoked wrong" and 6 as "config is bad" gets the wrong answer from `init`.
			- Fixed: `init` exits 6 for a schema that fails to load or has faults, in all four CLIs; exit 1 stays usage/IO.

		- ✅ Code Review 20260725 item 23: `shcl.h` does not compile as a drop-in under `g++ -Wall -Wextra -Werror`.
			- The bare `{0}` initializers trip `-Wmissing-field-initializers` in C++ mode; the file is already inconsistent, spelling one of them out in full.
			- The veneer gate compiles with `-Wall -Werror` only, so the repo's own build hides it.
			- Fixed: the implementation section suppresses `-Wmissing-field-initializers` for C++ TUs only (the `{0}` zero-init idiom is correct C; C++ -Wextra flags every one), and the veneer gate now compiles with `-Wextra` so the repo's own build proves it.

	- **Improvements**:

		- ✅ Code Review 20260725 item 24: `merge()` is O(children^2) per parent in all four bindings.
			- The over-side name dedup, the per-name group filter and the base-side instance match are all linear scans, and each rebuilds merge keys as it goes.
			- Parsing 32k keys costs 56 ms; merging them costs 16 s in the reference. The marquee feature is the slowest thing in the product.
			- The same accelerator the prior review's item 12 added to the parser applies here.
			- Fixed in all four bindings.
				- One grouping pass per side, with every merge key computed once, and a single children rebuild per parent.
				- 32k keys: the reference went from 16 s to 0.07 s, C from 14.4 s to 0.05 s. Output is byte-identical, and instance order is unchanged.
				- The stacked-list key rebuild in the reference, Go and Python went the same way, reusing the deferred flush C already had.

		- ✅ Code Review 20260725 item 25: a `[value]` selector is looked up by linear scan, so the inline spelling is quadratic.
			- 20k lines of `srv[hostN].port: N` take 9 s against 0.06 s for the equivalent block form.
			- Both spellings are spec-equal, so the user hits a 150x cliff for a cosmetic choice.
			- The read and write paths scan siblings the same way, since the parser's child index is deliberately dropped at load. That part is fine at hand-authored sizes; if it is ever worth touching, name interning is the low-risk option - a cached side index has to be invalidated at five mutation sites in four bindings, and a missed invalidation is a silent wrong value rather than a slow one.
			- Fixed at parse in all four: a display-keyed sibling map beside the merge-key map (same first-wins discipline, same mutation sites, flushed with the stacked-list deferral before any lookup). 20k inline-selector lines: 9 s -> 0.09 s. The read/write-path scans stay linear on purpose, as the item itself recommended.

		- ✅ Code Review 20260725 item 26: the validator's "did you mean" rebuilds the whole schema index once per unknown field.
			- Bites when a document is wholesale unmatched - the wrong file, or a schema for another app - which is the case the feature exists for.
			- C compounds it with a linear scan of the legal-chain set where the other three use a hash set.
			- Output is stderr prose, so the fix needs no corpus change.
			- Fixed in all four: legal chains and per-parent-chain sibling-name lists are built once per validate and handed to the suggester; C also swapped its linear legal-chain scan for the hash set the other three use.

		- ✅ Code Review 20260725 item 27: `to_canonical` is O(raw-siblings^2).
			- Each raw node rescans the parent's children to decide whether it merges with the line above; the parent's own walk already has that information.
			- 32k raw blocks under one field format in 2.8 s against a 0.05 s parse. Narrow, but free to fix and behavior-preserving.
			- Fixed in all four: the parent's walk carries a seen-empties set and passes each child its would-merge flag, so no node rescans its siblings. 32k raw siblings: 2.8 s -> 0.05 s.

		- ✅ Code Review 20260725 item 30: the canonical formatter discards blank-line grouping.
			- Comments were rescued as trivia by the prior review's item 4; blank lines are the other half of the same thing and were left out, so `fmt` flattens a grouped config into a wall.
			- Field names are also folded to lowercase and the spec never says so, which makes `fmt --write` a surprise.
			- The trivia model already exists, so this is a per-node flag and one emit line per binding.
			- Fixed: a `blank_before` trivia flag in all four parsers preserves one blank line before a binding (runs collapse; a blank before a comment group rides with it; fixpoint holds). Four goldens regenerated, case 032 pins it. The name folding was decided as correct-and-documented: the spec's formatter section now states lowercase is the canonical spelling.

		- ✅ Code Review 20260725 item 31: `paths()` exists only in the reference.
			- Go, Python and C consumers handed an unknown document cannot enumerate it; `Count` and `Instances` both require knowing the path already.
			- Straight violation of the guide's "same function inventory" rule on a public method, and about 20 lines per port.
			- Fixed: `Paths()`/`paths()`/`shcl_paths` (and the veneer's `paths()`) now exist in every binding, mirroring the reference's walk (file order, deduplicated, bare-name-safe segments only); a shared fixture is pinned in all four native runners.

		- ✅ Code Review 20260725 item 32: `--set` is described as the top layer but behaves as a first-instance edit.
			- A real top layer replaces every same-named leaf; `--set` targets the first instance and leaves the rest, so the two disagree on repeated leaves.
			- The spec already names the Writer as the mechanism, so this is a missing clause rather than wrong behavior.
			- Fixed with the missing spec clause: `--set` is a Writer edit targeting the first matching instance; whole-group override belongs in a `--layer` file.

		- ✅ Code Review 20260725 item 33: the convenience read tier is incomplete in C (3 of 11 types) and C++ (4 of 11).
			- No convenience read for string - the most common config read - or for raw, datetime, or any array.
			- The omission has a real C rationale (those types return borrowed memory), but the spec claims full coverage and the style guide's deviation list does not mention it.
			- Resolved as documentation (the C rationale is real - borrowed memory and lengths do not fit a value-or-default signature): the spec's ergonomic-tier section and the style guide's C deviation list now both state the exact coverage.

		- ✅ Code Review 20260725 item 34: Python's public `get_*` raises a private-named `_StatusError`.
			- A caller cannot catch it without reaching into a private name, so in practice they will write a bare `except Exception`.
			- Fixed: the class is public `StatusError` now (docstring included), so callers can catch it by name.

		- ✅ Code Review 20260725 item 35: the profiler stage samples only `fmt`, on a workload where everything is still linear.
			- The read path, `merge`, `validate`, `generate` and the Writer are never sampled, so all three 2026-07-23/24 features could go quadratic without moving a sample.
			- The cheapest half of the fix is a wall-clock number per workload in the run log: a flamegraph shows where time goes, not that total time grew 4x.
			- Fixed with the cheap half the review named first: the profiler stage now logs a wall-clock line per surface (fmt, merge, reads, validate, generate, set) from a `PROFILE_TIMED` list in config, so any of them going quadratic shows as a number moving even though the flamegraph still samples fmt.

		- ✅ Code Review 20260725 item 36: CI installs its lint toolchain unpinned every run.
			- `TOOL_PINS` already tracks the versions the local gate uses, so CI and local disagree about what "passing" was tested against.
			- Actions are also referenced by floating tag rather than commit SHA - generic hardening, small blast radius here.
			- Fixed: ci.yml installs ruff/mypy/cppcheck/markdownlint at the exact `TOOL_PINS` versions, and every action is referenced by commit SHA with the tag in a comment.

		- ✅ Code Review 20260725 item 38: the style guide bans the section rules the code actually uses.
			- "No banner dividers" against 28 of them in the reference, 28 in Go, 18 in Python, 12 in C - and the guide is what a Tier 3 author is told to read first.
			- The code is right: in a 3400-line drop-in file the section rules are the only navigation aid. Amend the rule and pin one spelling per language.
			- One real inconsistency alongside it: `shcl.h` uses the shell house `//•••` rule, which is both off-style for C and the only non-ASCII comment character in the C bindings.
			- Fixed: the guide now sanctions section rules as the one allowed banner and pins each binding's exact spelling; the lone `//•••` shell-style rule in `shcl.h` became the C `// ===` divider.

		- ✅ Code Review 20260725 item 39: panic macros are used outside tests in the reference.
			- Eight sites - six `unreachable!` (four of them in the newest validator and generator code) and two `unwrap()`.
			- Each is provably unreachable today, but they are invariants asserted by a panic in a library whose contract is that it never bails on a whole file, and three ports copy the structure.
			- Fixed: every non-test `unreachable!`/`unwrap()` now degrades instead of aborting - a slipped invariant skips the constraint, returns no-parent, or keeps the match total with an empty string. Zero panic macros left outside tests (the feature-gated profiling `expect`s never ship).

		- ✅ Code Review 20260725 item 40: the CLI usage block is hand-duplicated across four CLIs with no drift check, and its exit-code line is wrong.
			- It still says exit 6 means a strict load failure; the prior review's item 36 made `check` exit 6 on any error diagnostic, at any strictness.
			- `help`, `version`, a bare invocation and an unknown subcommand are the largest user-visible output in the project and the crosscheck never runs them.
			- Fixed: the exit-code sentence now says `6 check failed or strict load failure` in all four, and the crosscheck pins the whole usage surface - `help`, `version`, a bare invocation, and an unknown subcommand are compared byte-for-byte across bindings on every run.

		- ✅ Code Review 20260725 item 41: changelog has no Unreleased section, and contributing.md never explains the corpus workflow.
			- Five finished feature sets since the beta2 tag are recorded only in git; populating it now is also the raw material for the 1.0 notes.
			- contributing.md does state the parity rule, but nothing points a first-time contributor at how to add a conformance case, so their PR will be structurally wrong.
			- Fixed: changelog.md gained an Unreleased section covering everything since beta2 (the 1.0 notes' raw material), and contributing.md gained an "Adding a conformance case" walkthrough.

- **20260716**

	- **Bugs**

		- ✅ Code Review 20260716 item 1: C CLI reads freed memory on typed array output.
			- `get --int|--float|--datetime --array` with more than 8 elements prints from a stale pointer after the line buffer grows; large arrays segfault.
			- Fixed: owned line entries no longer store a pointer into the growable array; corpus case 008 pins 10-element typed arrays of every kind.

		- ✅ Code Review 20260716 item 2: Rust parser panics on a multibyte char in the timezone tail of a datetime value.
			- A garbled or hostile config aborts the consumer (exit 134) instead of returning BadType.
			- Fixed: zone tail is now checked byte-wise, so no str slice can land mid-char; corpus 007 `bad5` pins BadType across all bindings.

		- ✅ Code Review 20260716 item 3: wildcard array reads swallow per-slot NotFound/BadType.
			- A missing sub-path yields a silent zero with status Good - the exact trap the fallback design exists to prevent.
			- `count` and `instances` also disagree on the same wildcard path, breaking index alignment.
			- Fixed in all four bindings: array reads carry per-slot statuses, aggregate = worst slot, `instances` keeps unresolved slots as "", CLI grows `--slots` and per-slot `--default` substitution. Spec pinned, corpus case 009 + a slots column in reads.tsv, crosscheck replays `--slots`.

		- ✅ Code Review 20260716 item 7: `fmt` ignores `--strictness`.
			- Fixed: `fmt` loads at the requested strictness in all four CLIs; strict failure exits 6 with diagnostics, no output. Crosscheck now replays `fmt` at each `load` row's level.

		- ✅ Code Review 20260716 item 8: mixed `*`/field child lines silently build a block array.
			- Fixed: uniform-or-nothing enforced in all four parsers - first mixed field diagnoses an Error (field kept), later `*` lines under that parent are Errors and dropped. Corpus case 010.

		- ✅ Code Review 20260716 item 9: `field[disc]` matches differently at parse-time vs query-time.
			- Fixed: parse-side selectors match the display form like queries do; create only when nothing matches. Corpus case 011; spec's selector bullet updated.

		- ✅ Code Review 20260716 item 10: raw-block merge identity ignores the info-string.
			- Decided: info-string is part of a block's identity (fence style is not); equal bodies with different infos stay two instances. All four parsers; corpus case 012; spec updated.

		- ✅ Code Review 20260716 item 11: reading an array as a string drops quoting and escapes.
			- Fixed: array-as-string is the canonical inline form (minimal quoting, escapes intact, re-parses to the same array) in all four bindings. Corpus rows in case 011; spec's Strings section updated.

		- ✅ Code Review 20260716 item 12: parse time is quadratic in siblings.
			- Fixed: per-parent (name, value-key) lookup map in all four parsers; sibling scan and hint grouping are linear now. 100k flat lines: reference went from minutes to 0.2s, Python to 1.5s.

		- ✅ Code Review 20260716 item 13: Python formatter recurses and crashes on deep nesting.
			- Fixed: emit walks an explicit stack; the CLI recursion-limit bump is gone. Depth 25000 formats fine from both the CLI and library callers.

		- ✅ Code Review 20260716 item 14: PowerShell wrapper exits 0 when the resolved binary will not launch.
			- Resolution accepts any plain file (no executable check); a stale non-executable `shcl` yields empty output and exit 0.
			- Fixed: every ps1 resolution site now goes through `_shcl_executable` (Unix requires an execute bit, Windows keeps a bare leaf); the run-path passthrough is `exit ($LASTEXITCODE ?? 1)`.

		- ✅ Code Review 20260716 item 15: crosscheck cannot see trailing-newline differences.
			- Command substitution strips them before compare, so a binding that drops or doubles the final newline ships green.
			- Fixed: capture helpers append a trailing sentinel so `$()` has nothing to strip; a dropped or doubled final newline is now a divergence.

		- ✅ Code Review 20260716 item 16: crosscheck passes with zero comparisons.
			- An empty fuzz dump or a corpus layout change silently drops most of the 1764 comparisons and the gate still passes.
			- Fixed: exits 2 when no comparison ran, when `--extra` matches no `*.shcl`, or below an optional `--min N` floor.

		- ✅ Code Review 20260716 item 17: demo gif generator ignores step exit codes.
			- A renamed flag renders the error text into the gif and the pipeline publishes it onto the committed asset.
			- Fixed: a step whose exit differs from `expect_exit` (default 0) aborts the render, so cicd skips the publish `cp`.

		- ✅ Code Review 20260716 item 21: `fmt --write -` silently drops `--write` and exits 0.
			- Should be a usage error pointing at piping stdout instead. Same in all four CLIs.
			- Fixed: `--write` with stdin is a usage error (exit 1) in all four CLIs; `--write FILE` still rewrites.

		- ✅ Code Review 20260716 item 23: `field[sel]: value` is grammar-legal but has no spec'd meaning.
			- The value is dropped with an Error diagnostic, so strict loads fail on a line the grammar allows. Align spec, grammar, and code.
			- Fixed by spec'ing the code's existing behavior: a value after a last-segment selector is defined as an `error` (instance kept from the discriminator, value dropped; fails Strict). spec.md Selectors + grammar.abnf note updated; corpus case 018.

		- ✅ Code Review 20260716 item 24: invalid-UTF-8 command-line args abort the reference.
			- Rust exits 134 (panic); Go/Python/C all exit 3. The reference is the outlier on its own exit-code contract.
			- Fixed: all four validate argv as UTF-8 up front and exit 1 (`invalid argument encoding`); the reference uses `args_os` instead of the panicking `args`.

		- ✅ Code Review 20260716 item 25: broken stdout pipe gives three different exit codes.
			- `shcl fmt big | head`: Rust 134, Go 141, Python 0. Pick one behavior and pin it.
			- Fixed: uniform die-by-SIGPIPE (141). Rust and Python restore the default SIGPIPE disposition; Go and C already died by signal.

		- ✅ Code Review 20260716 item 26: C CLI has unchecked `realloc` on input/output paths.
			- OOM segfaults instead of taking the clean exit-70 path the arena already has.
			- Fixed: every CLI allocation goes through `xrealloc`, which now exits 70 with the library's message on OOM.

		- ✅ Code Review 20260716 item 27: crosscheck skips the last `reads.tsv` row if the file lacks a trailing newline.
			- One-line `|| [[ -n "$query" ]]` guard fixes it.
			- Fixed with exactly that guard on the read loop.

		- ✅ Code Review 20260716 item 30: NUL-joined merge key conflates distinct values.
			- `x: a, b` and `x: "a<NUL>b"` merge to one instance; the second value is silently lost. Make the key injective.
			- Fixed in all four parsers: each cell element (and the raw info-string) is length-prefixed, so the key is injective. Corpus case 017 (count = 2) pins it; the crosscheck skips it (bash can't carry a NUL) and the native runners do the pinning.

		- ✅ Code Review 20260716 item 32: wrappers invoked via symlink lose the sibling-binary and repo-build fallbacks.
			- Both wrappers compute the script dir without resolving links; resolve the real path first.
			- Fixed: bash follows `${BASH_SOURCE[0]}` through symlinks by hand (bare `readlink` loop, POSIX); ps1 resolves `$PSCommandPath` via `ResolveLinkTarget` into `$script:_SHCL_ROOT`.

		- ✅ Code Review 20260716 item 33: ps1 header's own usage example assigns to read-only `$host`.
			- Copying the documented example fails; rename the example variable.
			- Fixed: header example now uses `$svrhost`.

		- ✅ Code Review 20260716 item 34: ps1 `SHCL_BIN` probe skips the `.exe` fallback its header promises.
			- Route the pin through the same `_shcl_exe` helper the other probes use.
			- Fixed: the `SHCL_BIN` pin resolves through `_shcl_exe`, so a base name matches its `.exe`.

	- **Improvements**

		- ✅ Code Review 20260716 item 4: `fmt` deletes every comment with no warning, and the spec never discloses it.
			- Direct hit on the hand-author audience; retrofitting comment storage later touches all five codebases.
			- Decide before 1.0: preserve comments as trivia, or spec the loss and warn on `fmt --write`.
			- Done: comments survive `fmt` in all four parsers - whole-line comments re-emit above the node the next line binds, trailing ones stay on their line, end-of-file comments land at the end. Spec'd under Comments + Canonical formatter; corpus case 013 pins it and the older cases' expected files now keep their comments.

		- ✅ Code Review 20260716 item 5: the Writer half of the spec'd API exists in no binding and has no backlog item.
			- Spec presents Accessor+Writer as the two halves; schema-driven generation depends on it.
			- Done: Writer implemented in all four bindings (full CRUD - typed `set_<T>`/arrays/`raw`/`empty`, `_default` only-if-absent forms, `exists`, `set_comment`, `remove`), each setter the exact inverse of its read. New `shcl set` CLI applies a tab-delimited write-ops script from stdin. Corpus cases 014-016 pair `write.ops` with golden `expected-write.shcl` (matched by every binding's runner + a fixpoint check), the cross-binding differential replays `set`, and a 50k reference fuzz pins the string round-trip. Spec Writer bullet updated.

		- ✅ Code Review 20260716 item 6: README lead code examples call APIs that do not exist.
			- `GetIntOr(...)` (Go), `get_int(..., default=)` (Python), `get_or<T>` (C++) are all missing; a new user's first copy-paste fails.
			- Done: implemented the spec's convenience tier (not gutted the examples), so the README calls are now real as written. Go `GetIntOr`/`Get*Or` + array forms; Python `get_*`/`get_*_array` gained a `default=` param (must-exist and raises without one); C `shcl_get_int/float/bool`; C++ `get_or<T>`. Rust's is the native `get_int(path).unwrap_or(def)` and gained matching `get_*_array` so arrays have the same tier. Semantic pinned everywhere: value only on `Good`, else the call-site fallback (Empty falls back too). Reference unit test + Go test + C++ veneer CHECKs added.

		- ✅ Code Review 20260716 item 18: query-side behavior is barely pinned.
			- No corpus rows for wildcards, on-bad modes, or defaults; the fuzz differential compares `fmt` only.
			- The accessor side is where five hand-written ports diverge most easily.
			- Done: added a `--rawinfo` CLI type (+ the `rawinfo` reads.tsv type in all four runners) so the info-string read is pinnable; the reference `Document::paths()` drives a fuzz-dump-derived `<name>.reads.tsv` that the crosscheck `--extra` replays (reads over fuzz soup, not just fmt); every scalar read row is also replayed under `--on-bad=error` and `--default=<x>`; corpus case 020-accessor-surface pins wildcards (with a missing slot), a `[value]` selector, and both raw reads. Crosscheck ~1983 -> ~4203 comparisons.

		- ✅ Code Review 20260716 item 19: diagnostic wording became a byte-for-byte 5-way contract by accident.
			- `check` prints prose to stdout and crosscheck compares it, so every English message is frozen across bindings - contradicting design.md's per-binding-voice rule.
			- Give diagnostics stable codes; compare codes, free the prose.
			- Done: `Diagnostic` carries a stable `code` (`E001..`/`H001..`) in all four bindings; `check` prints `line N: severity: CODE` to stdout and the prose to stderr, so the differential check compares codes (not wording). C exposes `shcl_diag_code`. Includes item 36 (below).

		- ✅ Code Review 20260716 item 20: README still says no tagged release exists.
			- Contradicts the v1.0.0-beta1 tag, the changelog, and the published prerelease binaries; badge still says Alpha.
			- Done: lifecycle badge Alpha -> Beta, Status/Installing sections now reflect the `v1.0.0-beta1` pre-release and its prebuilt binaries; release-cut checklist in `design.md` gained a "README status pass" step so it can't drift again.

		- ✅ Code Review 20260716 item 22: `--on-bad=error` messages are bare enum names.
			- `app.name: BadType` - no value, no requested type, no file, no suggested fix. Stderr is not contract, so this is free to improve.
			- Done in all four CLIs: `shcl: cannot read <path> as <type>: <reason> (in <file>)`, where a BadType names the offending raw value (`value "$1200" is not a valid int`), and NotFound/Empty/Multiple get a plain-English reason. Array reads say `<type> array`. Stderr, so not crosscheck-pinned.

		- ✅ Code Review 20260716 item 28: dogfood install is a non-atomic in-place `cp`.
			- A launch during the copy sees a torn binary, and the synced dest dir can propagate it. Copy to a temp name, then `mv` over.
			- Done: the dogfood stage now copies the binary and each wrapper to a hidden temp name in the same dest dir, then `mv -f` (atomic rename) over the target, so a hand-launched copy only ever sees the complete old or new file; a failed copy is cleaned up and warned, not left torn.

		- ✅ Code Review 20260716 item 29: selector index parses as `usize` in the reference but `u64` in Go.
			- Latent divergence on any future 32-bit build. Pin the reference to `u64`.
			- Done: reference `Selector::ByIndex` is now `u64` (cast to `usize` only at the Vec index sites); C's `Selector.index` likewise moved from `size_t` to `uint64_t` (`parse_usize` -> `parse_u64`), closing the same latent 32-bit truncation. Go was already `u64`, Python is unbounded. No 64-bit behavior change.

		- ✅ Code Review 20260716 item 31: spec prose contradicts grammar and code on bare non-ASCII field names.
			- Prose says only reserved chars need quotes (and uses `Strasse` with a sharp s as an example); grammar and parser reject them.
			- Also: hex `-0x8000000000000000` (i64 min) reads BadType while the decimal spelling works.
			- Done: aligned prose to the code (grammar, `is_bare_name_char`, and the emit predicate already agreed) - a bare field name is ASCII letters/digits/`-`/`_` only; anything else, including non-ASCII, must be quoted, and the `Straße` examples are now shown quoted. Hex fixed in all four parsers: parse the magnitude as u64, then range-check against the sign, so `-0x8000000000000000` reads i64-min like its decimal spelling and `+0x8000000000000000` stays BadType. Corpus case 019-hex-int-bounds pins it (crosscheck now 585).

		- ✅ Code Review 20260716 item 35: value-taking options reject the space-separated form with a misleading error.
			- `--default 99` reports "unknown option: --default". Accept the space form or explain the `=` requirement.
			- Done: all four CLIs now accept both `--default=VALUE` and `--default VALUE` (same for `--on-bad`/`--strictness`), via an index loop that consumes the next arg; a value option with nothing after it says `missing value for --default (try --default=VALUE)`. Help text notes both spellings.

		- ✅ Code Review 20260716 item 36: `check` reports "ok" and exits 0 even when diagnostics include Errors.
			- A CI gate on `check` passes configs whose lines were dropped. Nonzero exit or clearer wording; note it is corpus-pinned, so change everywhere at once.
			- Done (folded into item 19): `check` exits 6 whenever any error diagnostic is present, at any strictness - `failed: N diagnostic(s), M error(s)` for a standard/loose load that dropped lines, `strict load failed: N diagnostic(s)` for a strict failure, `ok (N diagnostic(s))` + exit 0 only when clean. Same strings and exit in all four bindings.

		- ✅ Code Review 20260716 item 37: `--version`/`-h` are undiscoverable.
			- Help text never mentions them; `shcl help` and `shcl version` are rejected; `-w` is accepted but undocumented.
			- Done in all four CLIs: the usage block gained a `shcl help | version` line noting `-h/--help` and `-V/--version`; `shcl help` and `shcl version` are now accepted subcommands; the `fmt` line shows `[--write|-w]`.

		- ✅ Code Review 20260716 item 38: wrapper documentation drift.
			- README omits the PowerShell wrapper; spec says "POSIX sh" but the shell wrapper is deliberately Bash. Align the words with the artifacts.
			- Done: README blurb + Status now name both the Bash and PowerShell wrappers; spec's two concrete "shipped wrapper" claims say Bash instead of POSIX sh (the illustrative two-tier table row stays, like the other not-yet-shipped language rows).

#### Done - Initial requirements

- ✅ Initialize the git repo at `github/` and wire the remote. `main` plus a feature-branch flow.

- ✅ Resolve the open minor items at the end of `spec.md` (currency set, wildcard-missing behavior, `onBad` surface, percent-to-int, fence info-string). All settled inline under "Resolved minor items".

- ✅ Rust reference parser (Tier 1) implementing the full spec, driven by the corpus. The `shcl` CLI builds from it.
	- Done: single-file zero-dependency library plus the CLI (`get`, `fmt`, `check`, `count`, `instances`). Corpus-green, with fuzz smoke in the test run.
	- Note: fuzzing surfaced two formatter rules, now in `spec.md`.

#### Done - Misc to-do

- ✅ Set up pub/priv key for download signing.

- ✅ Set up account on crate.io and get publish API key.

- ✅ Set up account on pypi.org and get publish API key.

### Future and/or deferred

### Canceled

- 🚫 Default configuration hard-coded.
	- 🚫 Overridden by a per-user config file, created the first time a default is changed.
		- 🚫 Settings live under `~/.config`, resistant to errors (do not bail on the whole file over one bad line).
	- 🚫 Overridden by program options at run time.
	- Dropped: strictness and on-bad are the consuming program's contract, not the user's - a user-level override would silently weaken guarantees an app makes about its own config handling, and would make the same `shcl` command mean different things on different machines. Nothing else the CLI exposes is presentation-only, so there is nothing left for a config file to hold. Rationale in `design.md`; runtime options and the library's per-document strictness argument stay as they are.
