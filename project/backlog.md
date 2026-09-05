<!-- markdownlint-disable MD007 -- Indent count -->
<!-- markdownlint-disable MD010 -- No hard tabs -->
<!-- markdownlint-disable MD033 -- No inline html -->
<!-- markdownlint-disable MD055 -- Table pipe style [Expected: leading_and_trailing; Actual: leading_only; Missing trailing pipe] -->
<!-- markdownlint-disable MD041 -- First line in a file should be a top-level heading -->

<!-- TOC ignore:true -->
# SHCL backlog

The product backlog: bugs, features, enhancements, and code-review findings. Outside reports come in through GitHub Issues (see `contributing.md`); the work itself is tracked here.

<!-- TOC ignore:true -->
## Table of contents
<!-- TOC -->

- [Conventions](#conventions)
- [Backlog](#backlog)
	- [Bugs](#bugs)
	- [Features and enhancements](#features-and-enhancements)
	- [Done](#done)
		- [Done - Bugs](#done---bugs)
		- [Done - Features and enhancements](#done---features-and-enhancements)
	- [Future and/or deferred](#future-andor-deferred)
	- [Canceled](#canceled)

<!-- /TOC -->

## Conventions

In each section, items are listed approximately from newest to oldest. Inside Done and Canceled, loose items come first and code-review rounds after, each run newest first. "Approximately" is meant: items closed in the same week are often grouped by topic instead, which reads better than exact date order and is not worth unpicking. (Tip: use a clipboard or macro manager to make using these emojis easier.)

| Icon | Status
| :--: | :--
| 🔘   | Not started
| 🛠️   | Started, and/or partially complete
| 🔬   | Testing not started or finished
| ✋   | Defer
| ✅   | Complete
| 🚫   | Canceled

Sub-bullets under an item lead with what they are, so an item can be read by skimming the prefixes. The vocabulary: `Reproduced:` what was actually seen, `Cause:` why, `Decided:` a call that had to be made before code, `Fixed:` what changed, `Pinned by:` what now fails if it comes back, `Left alone:` what was looked at and deliberately not touched, `Measured:` one headline number, `Note:` anything else. Finding text written when the item was filed keeps whatever shape it was filed in.

Every item carries the date it was opened and, once settled, the date it closed. An item found and closed in one pass has no separate opened date. A deferred item keeps only its opened date: deferring is a decision to come back, not a settlement, so there is nothing to stamp closed - which is why a canceled item carries both and a deferred one does not.

## Backlog

### Bugs

- Code review 20260904:

	- A from-scratch adversarial pass over ground the earlier rounds recorded as unread: the parser judged against `grammar.abnf` and the spec rather than against the other bindings; the C validator body and `shcl_load_and_validate`; `SHCL_OOM` under a failing allocator for the writer, merge and generator; the reference's write side judged against the spec; the Go and Python read, coercion, datetime and validation bodies judged against the spec; the merge and generator items the last two rounds listed as not reached; the shell completions, the two wrappers and the documents against each other; the gates nobody had injected into; and every fix of the last two rounds, including what those fixes cost. Thirty-two defects here, twenty-one enhancements under Features and enhancements. Every item below was reproduced on this box, with two stated exceptions: item 32 reproduces as a shape rather than as a failure, and items 24 and 32 were reproduced as their own source lines under the script's shell options rather than by running the publish stage.
	- Nineteen of the defects are shapes all four bindings share, so the four-way check cannot see any of them. Three are C only. Six are gates, fixtures or documents that assert less than they claim. Item 1 is the round's worst: the recovery path the C header tells an embedder to use turns an allocation failure into a hang that cannot be interrupted.
	- The last two rounds cost nothing measurable except one deliberate trade. Rust, Go and Python are flat on parse, fmt, bulk writes, absent defaults, bulk reads, `check --schema`, `init` and merge. The C parse of a 20 MiB document is 2-7% slower because a parse now gives back its scratch arena instead of leaving it, and the memory that buys is real: `check --schema` peak RSS down 19%, merge down 13%.

	- ✅ Item 1: an allocation failure inside the lazy name-index build leaves the document in a state where the next lookup never returns.
		- Reproduced in C with the documented longjmp-ing `SHCL_OOM` hook: a 50000-child document, a failing allocator, then the same public `shcl_children` call again. It never returns, and no signal short of a watchdog gets the process back. Two allocations in is enough.
		- Cause: `name_index` sets `index_built` only after the whole walk, so a hook that unwinds mid-walk leaves the flag at 0 and the chains half built. The next lookup re-walks over them, `index_append` re-appends a node already on its chain, and `index_next[node]` ends up pointing at `node`. `children_named` then walks that chain forever.
		- Note: `shcl_validate` already guards exactly this, and says why - "the name index is the only thing on the document this call builds, and half of one is worse than none". Every other path through `name_index` has no such guard, because there the unwind belongs to the embedder. The header's own `SHCL_OOM` text is what points an embedder at this.
		- Note: read but not reproduced, in `index_append`'s second arm the first `cmap_put` can succeed and the second fail, leaving a key in `index_first` with none in `index_last`. A later append then adds a second `index_first` entry and a lookup can return the wrong chain head.
		- Note: `oom_hook.c` does the right thing but its fixture is three lines, so the index build never spans two allocations.
		- Fixed: the index flag has a third state, in flux, that the build and every append hold while they allocate. A lookup that finds it drops the index and rebuilds; the writer only touches a fully built one. That covers the second `cmap_put` arm too.
		- Pinned by: `oom_hook.c`, a 20000-child lookup cut short by the hook and then repeated, at every budget up to the one it completes under. Fails on the old header, passes on the new.
		- Opened: 20260904-170000
		- Closed: 20260905-090331

	- 🔘 Item 2: a quote anywhere in a bare value swallows the rest of the line, so a trailing comment is destroyed on the next write at exit 0.
		- Reproduced in all four. `note: don't panic  # keep this` loads with zero diagnostics and `fmt --write` leaves `note: "don't panic  # keep this"`. The comment is gone and the result is a fixpoint, so nothing will ever notice. The same apostrophe stops comma splitting: `b: it's fine, ok` is one element where `b: it is fine, ok` is two.
		- Cause: `split_comment`, `split_unquoted_commas` and `cell_exceeds` enter quote state on any `"` or `'`, wherever it sits. The spec says a piece is quoted only when it begins with one, and the grammar agrees. `unterminated_quote` already implements the correct rule, so the helpers disagree with each other.
		- Note: this violates two spec sentences at once - "a value *beginning* with a quote opens a quoted element" with "mid-text whitespace, `:`, `'`, `]`, even a `"`, all pass through", and "Comments are never discarded". An English apostrophe in a config value is about as common as input gets.
		- Opened: 20260904-170100

	- 🔘 Item 3: bracket-array detection looks at the first colon on the line, so any earlier colon hides the array and `fmt --write` bakes it in at exit 0.
		- Reproduced in all four. `"a:b": [80, 443]` reports `E015 missing colon` on a line that has one, and `fmt --write` leaves `"a:b": "80, 443"` at exit 0. A selector holding a colon does it too: `srv[db:5432].ports: [80, 443]`.
		- Cause: `looks_like_bracket_array` uses `content.find(':')`, which lands inside the quoted name or the selector rather than at the field separator, so the line falls through to `E015`. `E015` counts no lost content, so the save gate never fires.
		- Note: the spec says of `E019` that it "counts as lost content and an in-place rewrite refuses (`--lossy` overrides) rather than baking the changed value in". That promise is exactly what fails here. The `E015` diagnostic is also wrong on its own terms.
		- Opened: 20260904-170200

	- 🔘 Item 4: one reversed `min`/`max` range turns off the unknown-field sweep for the whole document.
		- Reproduced in all four. A sound schema reports both unknown fields; changing `min: 1` to `min: 70000` on an unrelated field reports one `V092` and neither unknown field. The check still exits 6, so it looks like it is working.
		- Cause: `parse_field` treats a crossed range as an entry-level failure and `build_schema` clears the paths-complete flag, which the sweep reads. The spec says the sweep "turns off only when a fault cost a path spelling outright (an unreadable `field:` path, or a mount naming no declared fragment)". A crossed range is neither - the spec's own constraint table says the field is dropped, not that its name is lost.
		- Note: the dropped field also loses its `repeat`-based `H001` disavowal, for the same reason.
		- Note: the 20260902 clean list records "`min > max` no fault" for Python. That is wrong twice - there is a fault, and this is its side effect.
		- Opened: 20260904-170300

	- 🔘 Item 5: a merge deletes a retained content-malformed line without counting it lost, so a save writes the truncated file instead of refusing.
		- Reproduced in all four and in the veneer. Base `square-miles 300` plus `ok: 1`, overlay `ok: 2`: the merged canonical is `ok: 2` alone, `lost_count()` is 0, and `save_file` returns Ok. The malformed line is gone from the file the consumer writes back.
		- Cause: a content-malformed line is stored in the same trivia slot as a comment, and a leaf override drops the base leaf's trivia. The spec sanctions dropping the base leaf's comments; it does not sanction dropping content it promised to retain "so a hand-typo in a config survives the consumer loading, editing, and writing the file back".
		- Note: library-only, because `--write` refuses `--layer`. A consumer folding layers and saving is the exact case the lost gate exists for.
		- Opened: 20260904-170400

	- 🔘 Item 6: `Set<T>Default` reports success on a wildcard path, which is the one path shape every setter is supposed to refuse.
		- Reproduced in all four, library and CLI. `--set-default='srv[*].port=99'` exits 0 with nothing written when the path resolves, and exits 1 when it does not. Plain `--set` on the same path exits 1 either way. All twelve default forms are affected.
		- Cause: each `_default` form is `if !exists(path) { return set_X(...) } true`, and `exists` is true for a wildcard whose slots resolve, so the refusal is never reached. Whether an unusable path passes now depends on the document's contents.
		- Note: it also disagrees with `write_reason`, whose doc comment says "`Writable` means the same validation `place()` runs would pass". `write_reason` correctly says `Wildcard` on the same path.
		- Opened: 20260904-170500

	- 🔘 Item 7: the index-rebuild timing test cannot reliably fail on the defect it names, and Python's copy can never fail.
		- Reproduced by backing the fix out: the Rust test passed on two of five runs, with the churned side landing either side of the bound. Go fails by under 2x. Python's fixture is 20000 churn iterations and 50 merges where Rust and Go use 100000 and 200, so the whole cost of the defect is about 52 ms against a 250 ms constant term - it cannot fail on any machine.
		- Cause: widening the constant from 25 to 250 ms for a shared runner made the constant larger than the defect. The factor was meant to be what catches it; the constant now absorbs the whole thing.
		- Note: the C copy is still at +25 and does bite, by 12%. On windows it does not run at all - the clock-coarseness guard needs a fresh side over 20 ms and windows measures it at 0.1.
		- Note: a count is the right measure here, not a clock. The defect is how many nodes were walked, which is exact.
		- Opened: 20260904-170600

	- 🔘 Item 8: `allowed` on `type: datetime` compares the written clock rather than the moment.
		- Reproduced in all four. A document value of `2026-01-01T13:00:00+01:00` against `allowed: 2026-01-01T12:00:00Z` is `V004`. The same instant written at the same offset passes.
		- Cause: the comparison is field-wise and then requires equal offsets, which makes the three offset-zero spellings agree and nothing else. The spec says "For `datetime` the coerced space is the moment".
		- Note: normalize a zoned value to its instant before comparing, and keep the field-wise path for zone-less values so "a value with no zone is local and matches no zoned one" still holds.
		- Opened: 20260904-170700

	- 🔘 Item 9: `check-install-dev.bash` passes with the guard it exists to test removed.
		- Reproduced: replacing the clone check in `install-dev.bash` with a no-op leaves the gate green. Its negative fixture is a bare directory that is not a git repository, so the nonzero exit it reads comes from `git config` refusing a non-repository, not from the script.
		- Note: the guardless script then sets `core.hooksPath` and rewrites `core.sshCommand` on any unrelated git repository named with `--dir`. The gate's own comment also promises "nothing created" and nothing checks that.
		- Note: the fixture has to be a git repository that is not an shcl clone, plus one that is an shcl tree and not a repository, and the assertion should read the config back rather than only the exit status.
		- Opened: 20260904-170800

	- 🔘 Item 10: the bash completion cannot see the `--opt=VALUE` form at all, so the spelling every document uses loses the FILE slot.
		- Reproduced in an interactive bash over a pty. `shcl check --strictness=<TAB>` offers nothing, and `shcl check --strictness=standard al<TAB>` offers nothing where `alpha.shcl` belongs. The space form works.
		- Cause: `COMP_WORDBREAKS` contains `=`, so bash has already split the word into three before `_shcl` runs, and the file calls neither `_init_completion` nor `_get_comp_words_by_ref` to re-join them. The four `=VALUE` arms are dead code, and the value is then counted as a positional, which is what eats the FILE slot.
		- Note: the zsh completion is right here - it uses `compset -P` and zsh does not split on `=`.
		- Opened: 20260904-170900

	- 🔘 Item 11: both completions omit `--remove`, `--set-default` and `--set-literal-default` from the value-option lists.
		- Reproduced in both. `shcl fmt --remove <TAB>` offers filenames where a PATH belongs, and `shcl fmt --remove x <TAB>` offers nothing where the FILE belongs. `--set` is listed and behaves correctly, which is the control.
		- Cause: the CLI has ten value-taking options; the space-form dispatch and the positional-skip list each carry seven. The round that added the three options updated the option table, which `check-completions.bash` diffs, and not the two lists it does not read.
		- Opened: 20260904-171000

	- 🔘 Item 12: a trailing non-breaking space or line separator in a bare value is deleted with no diagnostic and no lost count.
		- Reproduced in all four. A line spelled `a: val` with a U+00A0 after the value loads clean, and `fmt --write` leaves `a: val`. Same for U+2028, VT and FF.
		- Cause: the line trim uses the language's Unicode whitespace set where the grammar says `wsp = SP / HTAB` and puts those characters in the bare value alphabet.
		- Opened: 20260904-171100

	- 🔘 Item 13: an unterminated raw block in a newline-terminated file gains a trailing empty line the file never had.
		- Reproduced in all four. `x: ```sql` then `body` with a final newline reads back `body\n\n`; the same two lines without the final newline read back `body\n`. `fmt` writes the fabricated line out.
		- Cause: splitting the text on `\n` yields a phantom final element, which `consume_raw` takes as a body line. The grammar says the last line may lack a newline, so the two spellings are the same document.
		- Opened: 20260904-171200

	- 🔘 Item 14: `*` followed by a space and nothing else reports `E013` with a message its own input disproves, and kills the block beneath it.
		- Reproduced in all four. `* ` gives `E013 malformed line: '*' must be followed by a space` - the line does have a space - and the child line under it is dropped as `E018`. The same empty element spelled `* #c` takes the `E009` path and keeps its block.
		- Cause: the line trim removes the trailing space before the marker check, so the marker is a bare `*`. The spec's code table gives this shape `E009`.
		- Note: `E013` pushes a dead level and `E009` does not, so the wrong code costs the block as well as the message.
		- Opened: 20260904-171300

	- 🔘 Item 15: a dropped stacked-`*` element does not take its indented block with it, while the malformed spelling of the same mistake does.
		- Reproduced in all four. Under `sizes:` with a `k: 1` sibling, `* small` with `n: 5` beneath it gives `E008` and `paths` reports `sizes.n` - the child re-parented one level up. Spelled `*small` the same document gives `E013` plus `E018` and the child is gone. `E007` and `E011` behave like `E008`.
		- Note: the spec says of `E018` that a skipped line's block "never re-parents one level up", and says the same of a dedent to a bad column. What is not defensible either way is that two spellings of one mistake give the reader two different answers - the same reasoning that settled the `E012` sentinel in the 20260901b round.
		- Decided: needs a call on whether a dropped element opens a block at all. `E001` already keeps a field among list elements at the parent, so keeping the child is arguable; giving the same mistake two answers is not.
		- Opened: 20260904-171400

	- 🔘 Item 16: the PowerShell wrapper drops a bare `--` when it is dot-sourced, which is the mode its own header documents.
		- Reproduced on pwsh 7.6. `. ./shcl.ps1; shcl get -- t.shcl '-dash'` gives `unknown option: -dash` at exit 1; quoting the token as `'--'` works; the bash wrapper is correct in both modes.
		- Cause: PowerShell's own parser consumes a bare `--` as its end-of-parameters token before `@args` is built, so the wrapper never sees it. The script forms are unaffected.
		- Note: help and the man page both say "Use `--` to end the options when a FILE or PATH begins with a dash", and the wrapper's header says both modes "take the same arguments". The workaround is written down nowhere.
		- Opened: 20260904-171500

	- 🔘 Item 17: `raw-default` is an accepted write op in all four CLIs and appears in no document.
		- Reproduced in all four: a `raw-default` ops line writes the block and exits 0. It is in none of the help's op list, the man page, the README, the spec, the changelog or either completion.
		- Note: the help's op table positively implies it does not exist - it spells `<type>[-array]-default` over a `<type>` set of `int|float|bool|string|datetime` and puts `raw` on its own line with no `[-default]`. The README sends a reader wanting raw edits to exactly that list.
		- Decided: the library contract already covers it, since the spec documents `Set<T>Default` generically over a `<T>` set that includes `SetRaw`. So this is a documentation gap, not a surface to remove.
		- Opened: 20260904-171600

	- 🔘 Item 18: `SetComment` leaves the blank separator between the new comment and its node when the node already carries a comment.
		- Reproduced in all four. On `a: 1` / `# note` / blank / `b: 2`, adding a comment to `b` gives `# note` / `# new` / blank / `b: 2`. With no pre-existing comment the rule works.
		- Cause: the blank is only moved above the comment when the node's leading list is empty. The spec says the call places "a node's blank separator line above the comment so the comment sits against its node", with no such condition, and the code's own comment says the same.
		- Note: cosmetic - the result is still a fixpoint.
		- Opened: 20260904-171700

	- 🔘 Item 19: `SetLiteral` accepts text a file line would not read as one value.
		- Reproduced in all four. `--set-literal='x=```sql'` stores `"```sql"` at exit 0 where the file line `x: ```sql` is `E005`; `--set-literal='y=[80'` stores `"[80"` where the file line is `E014`.
		- Cause: `literal_value` refuses a line break, an unterminated quote and complete bracket text, and nothing else. The spec says the call "reads its argument the way the parser reads the half of a line after the colon", rejecting "only" those three.
		- Note: the refusal list is also wider than the spec in one direction - a bare CR is refused, where a file keeps it as content mid-line and `set_string` accepts it.
		- Decided: the spec sentence is the half to fix. Refusing a fence opener now would change a released API for no gain, so the "only" list should say what the code does.
		- Opened: 20260904-171800

	- 🔘 Item 20: a hexadecimal integer above the i64 range is `BadType` as a float, while the same number in decimal reads fine.
		- Reproduced in all four. `0x8000000000000000` and `0xFFFFFFFFFFFFFFFF` through `get --float` exit 4; `9223372036854775808` and `18446744073709551615` exit 0. Quoted thousands separators have the same hole.
		- Cause: the float read recognizes a decimal integer by its own shape test but reaches a hex one only through the i64 integer parse, so the i64 bound leaks into a read the spec bounds by the double range - "An integer is a valid float on read" and "The value must fit a double".
		- Opened: 20260904-171900

	- 🔘 Item 21: every C merge retains a whole children array per parent it rebuilds, so the merge cost the spec states is wrong by two orders of magnitude in C.
		- Measured: 200 merges of an eight-leaf overlay on a 40000-key base grow the C process by 419,687 bytes per merge - 84 MB - where Python grows 4,958 and Rust and Go stay near a kilobyte. At the spec's own 500 merges that is about 210 MB against the stated "about a megabyte".
		- Cause: `w_overlay` allocates the replacement children array in the document arena, sized by the parent's whole child count, and abandons the old one. The comment three lines above records the same fight already won for the builder's doubling chain, which was moved to scratch; the exact-sized copy was not.
		- Note: `shcl_compact` does reclaim it - 84 MB back to 2.9 MB in use - but nothing tells a repeatedly-merging C consumer that. The `shcl_compact` header comment is written for repeated writes, "a few dozen bytes per write".
		- Opened: 20260904-172000

	- 🔘 Item 22: C alone keeps an `H001`/`H002` hint for a field whose declared leaf name is empty.
		- Reproduced: a document of two `"": 1` lines under a schema declaring `field: '""'` with `repeat: 1, 5` gives one hint in C and none in the other three.
		- Cause: the C suppressor carries an extra `&& last->name.n` the other three do not. The spec's only stated exception is the name wildcard.
		- Note: a stdout divergence the crosscheck could see - the corpus just has no empty-named field under a schema.
		- Opened: 20260904-172100

	- 🔘 Item 23: `check-c-compilers.bash` reports OK with a single compiler and never reads `SHCL_GATE_STRICT`.
		- Reproduced: with gcc-12 to gcc-15 and clang hidden from `PATH`, the gate prints "OK: 5 build(s) across 1 compiler(s)" and exits 0 under `SHCL_GATE_STRICT=1`.
		- Cause: the compiler list is whatever `command -v` finds, with no floor, and the script does not read the flag three other gates do. The engine's own comment says "A gate that quietly skips what it cannot run is a gate that stops checking when a runner loses a tool. The gates read this and fail on a skip instead".
		- Note: the disagreement it exists for is real. Removing the `-Wclobbered` suppression around `do_parse` makes gcc-12 and gcc-13 refuse the file while 14, 15, clang and this box's default `cc` all build it clean.
		- Opened: 20260904-172200

	- 🔘 Item 24: the publish stage aborts before doing anything in a repository with no git identity.
		- Reproduced as its own source lines under the script's shell options: `git config user.name` as a bare statement exits 1 when the key is unset, and `set -e` plus the script's own ERR trap end the run there. It happens before the backup and before any git action, and arrives as a trap dump naming a line rather than a message about identity.
		- Note: git identity on this box comes from three `includeIf gitdir:` rules, so a repository outside those paths has none, and a bare runner has none either. The block runs on every non-quiet publish.
		- Note: the same file's ssh probe assigns from a pipeline in its own statement, so the `${sshHost:-github.com}` fallback on the next line can never fire - the only way the variable ends up empty is git failing, which kills the script at the assignment first.
		- Opened: 20260904-172300

	- 🔘 Item 25: `largedoc.bash --mib 1` fails on a healthy tree.
		- Reproduced: at 1 MiB python peaks at 71 MiB against a 65 MiB budget and the gate reports FAILED. At 2 MiB rust reads 58 against 64, one line from the same false failure.
		- Cause: the per-binding ceilings are strictly per input MiB with no constant term, so at small sizes the fixed interpreter and runtime baseline dominates. `--mib N` is a documented option.
		- Note: the pipeline only ever runs it at 100 MiB, so it does not bite there. A developer shrinking it to iterate gets a defect report that is arithmetic.
		- Opened: 20260904-172400

	- 🔘 Item 26: four items from the last two rounds closed with nothing pinning them.
		- 20260902 item 29: `perf-gate.bash`'s exit-code and output-size checks exist, but nothing ever feeds the gate a broken CLI. `grep -rn perf-gate cicd/` finds only the shellcheck target and the real invocation. Delete both checks and nothing fails.
		- 20260901b item 42: nothing compares main's `install.bash`, `install.ps1` and `install-dev.bash` against dev's. The three are byte-identical today, so it is holding - but this is the one class of drift that reaches every user the moment it happens, since the README one-liners fetch from main as they run.
		- 20260901b item 45: no gate reads `style-guide.md` at all. The corrections can be reverted and nothing fails.
		- 20260902 item 7, second half: no test anywhere covers a rendered path being emitted once when a schema spells the same name twice. The behavior is right and agrees four-way; the corpus has no schema carrying both spellings. The first half is pinned by case 082.
		- Opened: 20260904-172500

	- 🔘 Item 27: 20260901b item 14 records a manual check as though it were committed.
		- Reproduced: the item says "Pinned by planting `.ro.shcl.tmp999.0` in the fixture's directory". Nothing in the tree plants that file, and a correct run leaves no temp for either filter to see, so restoring the old dirent filter passes the whole suite.
		- Note: the fixture is now capable of catching a regression it could not catch before, which is worth having. The "Pinned by" line is what makes an unpinned item look pinned to the next review.
		- Opened: 20260904-172600

	- 🔘 Item 28: `SHCL_GATE_STRICT` is asserted nowhere, so deleting the one line that arms it disarms five gate skips silently.
		- Reproduced by reading: `cicd.bash` exports it, `check-locale.bash` and `package.bash` read it, and `shell-regress.bash` exercises only its own `fHave` helper. Removing the export changes no gate's result.
		- Note: that is the failure 20260902 item 32 was filed against, one level up.
		- Opened: 20260904-172700

	- 🔘 Item 29: three fixes from the last two rounds are guarded by nothing, in a way that lets each come back quietly.
		- 20260902 item 18: `shell-regress.bash` pins the `fSplitTabs` half by lifting it out of `crosscheck.bash` by name, but nothing asserts `sanitize-c.bash` carries its new `children` and `paths` arms. Reverting the item leaves the gate silently replaying `get --children`. The unknown-type arm added by the same fix narrows the recurrence path but does not close it.
		- 20260902 items 23 and 35 and 20260901b item 26: each shipped as prose in the spec, and `check-docs.bash` asserts one sentence of the three groups. The datetime tolerance paragraph, the five merge behaviors and two of the three merge facts can all be deleted with every gate green.
		- 20260902 item 44: the fix is windows-only and the `cli-regress.bash` row that pins it never runs on windows, because `win-runners.bash` carries no cli-regress row. On linux the pre-fix code was already correct, so the row passes either way.
		- Opened: 20260904-172800

	- 🔘 Item 30: two doc comments merged onto one function in Rust and Go, leaving the next one undocumented.
		- Reproduced by reading: `quoted_shape`'s description sits above `one_line` in the reference, and Go's is worse - the text begins "quotedShape is true when ..." directly above `func oneLine`. C and Python have it right.
		- Note: exactly the class the release recipe lists as a mechanical pre-release check, and it says to compare comment inventory across bindings rather than code. Present since the functions were written.
		- Opened: 20260904-172900

	- 🔘 Item 31: four documents disagree with the code or with each other.
		- The `fmt` synopsis omits `[options]` in the help and in the man page's SYNOPSIS, while the same binaries' usage-error line spells it correctly and `fmt` accepts seven of them. `-w` is missing from the man SYNOPSIS too.
		- The spec says `--set` and `--set-literal` "share one ordered list"; the help, man page and README all say five options share it, which is what the CLI does. The spec never mentions `--remove`, `--set-default`, `--set-literal-default` or `--slots` at all.
		- The spec's worked example of a non-associative fold does not exhibit one: with `p: 1`, `p: 2` and a bare `p:` over an `x: 1` base, every grouping gives the same result, because a bare header is a wrapper mention only against a container instance and neither `p` has children. The general claim is true and the bindings' own doc comments state the right condition.
		- `grammar.abnf` puts `index-sel` inside `field-line` with no query-only carve-out, so it documents `a[#0].b: 1` as legal where all four say `E014`; and its `newline = [ CR ] LF` contradicts the spec's "a line's entire trailing carriage-return run is removed", which is what the code does.
		- Note: also here because it is one sweep - a bare numeric selector past u64 silently becomes a value selector rather than an index, against `1*DIGIT`, and a trailing comment on a root-level `*` element line is discarded, against "Comments are never discarded". The second is contained by the save gate at exit 7.
		- Opened: 20260904-173000

	- 🔘 Item 32: `package.bash` uses the pipeline shape the same file documents as wrong forty-nine lines earlier.
		- The file states the rule at the deb listing: "Into a variable first: `grep -q` quitting early would kill the tar behind `dpkg-deb` with SIGPIPE and fail the pipeline for the wrong reason." The libgcc dependency probe then spells it `readelf -d "${bin}" | grep -q 'NEEDED.*libgcc_s'` under `pipefail`.
		- Reproduced as a shape, not as a failure: the same construct with a writer that outgrows the pipe buffer returns 141, which the `if` reads as "no match". Today `readelf -d` output fits the buffer, so the dependency is added correctly.
		- Note: this is the defect 20260901b item 15 fixed, re-entering the same file. Correct today, wrong the day the probe widens - and the consequence is a shipped package silently declaring no libgcc dependency.
		- Opened: 20260904-175100

### Features and enhancements

- Code review 20260904:

	- The enhancement half of the round filed under Bugs above. Twenty-one items. Nothing here violates a stated rule; each is a measured cost, a gate that could assert more, or a document that could say more.

	- 🔘 Item 33: C's schema build is quadratic in the fragment count.
		- Measured `check --schema` on a tiny document against N fragments, rust debug / go / c: 2,000 fragments 0.06 / 0.03 / 0.02 s; 8,000 0.28 / 0.05 / 0.34; 16,000 0.61 / 0.14 / 0.90; 32,000 1.15 / 0.24 / 3.93.
		- Cause: the C fragment lookup is a linear scan and the duplicate check calls it once per fragment. The reference uses a map. Disabling the duplicate check in a scratch copy drops 32,000 fragments from 4.02 s to 0.20 s. It is paid three times per `check --schema`, once for validation and once for each suppressor.
		- Opened: 20260904-173100

	- 🔘 Item 34: C looks the mounted fragment up inside the per-node loop, though the mount is invariant for the constraint.
		- Measured 2,000 fragments over 50,000 mount nodes at 0.53 s; hoisting the lookup in a scratch copy gives byte-identical output in 0.36 s.
		- Opened: 20260904-173200

	- 🔘 Item 35: `check-completions.bash` proves the option table and nothing a user types.
		- It never sources or runs either completion file. Everything it does not cover is a live defect: the value-option skip list, the `=VALUE` handling, the `--strictness` and `--on-bad` value lists, the file-slot map, the "`-w` is the only short option" claim, and where the informational flags are offered.
		- Note: driving `_shcl` with a fixed `COMP_WORDS`/`COMP_CWORD`, and the zsh function with stubbed `_describe`/`_values`/`_files`/`compadd`/`compset`, is about fifteen lines each. Items 10 and 11 both came out of exactly that.
		- Opened: 20260904-173300

	- 🔘 Item 36: give `perf-gate.bash` a self-test.
		- Two bait CLIs, one exiting 1 instantly and one printing nothing, and two assertions, in the shape `shell-regress.bash`'s own scan self-test already uses. That closes item 26's first bullet properly rather than leaving the checks to be deleted by the next person tidying the script.
		- Opened: 20260904-173400

	- 🔘 Item 37: gate main's installers against dev's.
		- One `git diff --quiet origin/main origin/dev -- install.bash install.ps1 install-dev.bash` in `check-docs.bash` turns the docs-only-exception decision into something enforced. It currently rests on remembering, and it is the only drift in the tree that reaches users the moment it happens.
		- Opened: 20260904-173500

	- 🔘 Item 38: assert the spec sentences recent rounds added.
		- `check-docs.bash` already greps the spec for a dozen phrases. The datetime tolerance paragraph, the five merge behaviors and the two remaining merge facts are three more greps. Where a round's whole deliverable is prose, a grep is what makes it a fix rather than a note.
		- Opened: 20260904-173600

	- 🔘 Item 39: nothing gates the two wrappers past `SHCL_BIN`.
		- `shell-regress.bash` covers `SHCL_BIN` resolution, the caller-scope hygiene, the symlink resolution and the pre-.NET-6 guard, and caps the header comment width. Nothing covers argument pass-through, exit-code pass-through, stdin and stdout fidelity, or whether the two wrappers agree - which is why item 16 was never seen. The whole matrix run this round is about forty rows.
		- Opened: 20260904-173700

	- 🔘 Item 40: pin "pprof must never ship" to the artifact, and compile the profiling build somewhere.
		- The rule holds today: the release binary and every packaged artifact carry zero matches for pprof, inferno or quick-xml, and no default-feature build resolves the dependency. It rests entirely on the release command not carrying the feature flag. One `strings` line in `package.bash` or `sign-release.bash` would pin it to the file rather than to the command. The stake is a GPL-incompatible license that `deny.toml` allows on exactly that basis.
		- Note: the `--features profiling` build is neither linted nor compiled by any gate, so the feature-gated block only breaks visibly in a full local run. `cargo check --features profiling` in the lint extras costs seconds after the first build.
		- Opened: 20260904-173800

	- 🔘 Item 41: the man page's rendered width is ungated, and one line already runs past 80.
		- Rendered at `MANWIDTH=80`, exactly one line is 81 columns, from an unfilled example block. `cli-regress.bash` pins the help at 80 and says why; nothing does the same for the page next to it.
		- Opened: 20260904-173900

	- 🔘 Item 42: a set-then-remove pair grows the document by about 312 bytes in Rust, Go and Python, with no way to reclaim it.
		- Measured over 20,000 iterations with a counting allocator. Every other repeated setter is flat at 0.00 bytes per call; `set_comment` is 58 bytes, which is its semantics. `remove` unlinks the node and leaves its arena slot and its index entry.
		- Note: C has `shcl_compact`. The other three have nothing, and nothing says so.
		- Opened: 20260904-174000

	- 🔘 Item 43: `shcl_reads_release` plateaus at the largest single read result and the header does not say so.
		- Measured: after `to_canonical` on a 100 MiB document it leaves one 95.9 MiB block held until `shcl_free`.
		- Opened: 20260904-174100

	- 🔘 Item 44: point a repeatedly-merging C consumer at `shcl_compact`.
		- Follows from item 21. The `shcl_compact` comment sells compaction to a repeated writer, "a few dozen bytes per write". Merging costs four orders of magnitude more per call, and the merge comment mentions compaction only in passing.
		- Opened: 20260904-174200

	- 🔘 Item 45: say what `SHCL_SETJMP` costs an embedder, and decide its scope.
		- The macro's own comment justifies the non-unwinding path with "nothing in between has a destructor or a `__finally`". That is true of the library's two arming sites and not of the case the same header points an embedder at, where the frames between their recovery point and the failed allocation are theirs. A C++ embedder with RAII on mingw gets destructors skipped.
		- Note: the guard is `__MINGW32__ && __x86_64__ && __SEH__`. mingw's own header takes the same unwinding branch on aarch64, so an ARM64 windows build of the C binding keeps the original behavior. `style-guide.md` scopes the deviation to mingw x86_64, so this is a limit to confirm rather than a contradiction - nothing builds C for that target today.
		- Opened: 20260904-174300

	- 🔘 Item 46: `shcl_validate` leaves one arena guarded on a frame it has returned from.
		- The teardown clears the validation arena and the document's three, and not `v->scratch`, which `v_unknown` armed on the same recovery point. Inert today, because that arena is only written inside `v_unknown` and freed afterwards. It becomes a jump into a dead frame the moment anything allocates into it after the call. `do_parse` clears all four of its arenas explicitly, which is what makes the omission read as an oversight.
		- Opened: 20260904-174400

	- 🔘 Item 47: a file whose basename runs past about 241 characters cannot be rewritten, and the cut-off moves with the pid.
		- Measured with a 7-digit pid: basenames of 240 and 241 characters save, 242 and 243 fail at exit 8 with no temp left behind. All eight attempts use the same length, so they fail together.
		- Note: the failure is safe but not deterministic - a name in that band saves on a machine with a short pid and fails on one with a long pid. A fixed-width temp stem removes the band.
		- Opened: 20260904-174500

	- 🔘 Item 48: the Loose currency strip also eats the whitespace after the symbol.
		- `$ 1200` reads as 1200 in all four. The spec says only "a single leading symbol ... is stripped" and calls the list closed, so a port written from the spec would refuse it.
		- Opened: 20260904-174600

	- 🔘 Item 49: two neighbouring reads disagree about an empty binding.
		- `read_raw_info` on an empty binding is `BadType` where `read_raw` on the same node is `Empty`. The spec is silent, so this is a call to make rather than a rule broken.
		- Opened: 20260904-174700

	- 🔘 Item 50: a setter refused for its value reports the message written for `set_literal`.
		- A `raw` op with an unquoted `#` in its info string reports "the value text is not one value". The real reason is the info string. Prose is not part of the contract, but the wording sends a reader to the wrong half of the line.
		- Opened: 20260904-174800

	- 🔘 Item 51: three places the spec could say more.
		- A column-zero tail comment: the paragraph says a comment written at the level of the block's last binding trails that binding, and then that only top-level tail comments remain end-of-file orphans. For a comment at column zero after the last top-level binding those read against each other. The behavior is settled and the changelog states it; the sentence describing the old behavior is still there.
		- The generator has no stated output ceiling. A mount chain produces dotted paths, so output is quadratic in depth: 44 KB of schema at the 512 cap generates 1,067,121 bytes. Time is linear in output and the depth cap bounds it, so nothing is wrong - but "a starter config" does not prepare a reader for a megabyte.
		- Non-UTF-8 input is unspecified at library level. Every CLI refuses the file at exit 8, while Go and C accept the bytes and Python accepts surrogate-escaped ones, and the three then give three different "did you mean" answers. One sentence saying it is unspecified is cheaper than making them agree.
		- Opened: 20260904-174900

	- 🔘 Item 52: shell-trap and gate cleanups across the pipeline.
		- `cicd.bash`'s `fWriteSums` ends its final subshell on an `&&` list, so an empty artifact directory would abort the run through the ERR trap. Not reachable today.
		- Three `sed ... | head -1` pipelines sit under `pipefail` (`cicd.bash`, `shell-regress.bash`, `sign-release.bash`), safe only while the pattern matches exactly one line. `sed -n '...{p;q}'` removes the reader.
		- `check-install-dev.bash` aborts on its own `git config --unset` when the key is absent, so one failing check hides the three after it.
		- `cli-regress.bash` skips its `/dev/full` rows with no `SHCL_GATE_STRICT` check, unlike the two gates that read it.
		- `check-c-compilers.bash` builds only at `-O2`, while the class it exists for is optimization-dependent - `win-runners.bash` sweeps five levels for that exact reason.
		- Two output helpers spell the blank-line test `[[ VAR -eq 0 ]]`, and `n8git_backup-and-publish` carries a dead helper reading `${!i}` with no default. Shapes to retire, not live faults.
		- Note: the sweep for the recorded bash traps found no `((n++))` anywhere and every glob loop guarded, so this is what is left.
		- Opened: 20260904-175000

	- 🔘 Item 53: the README's Go example does not compile, and the Zig one is the only language example nothing builds.
		- The Go fragment declares a variable it never uses, which is a hard compile error, so it does not build even with the package clause, `func main` and the imports a reader adds. The Rust fragment has the same unused binding and only warns.
		- Note: the C example is gated by `check-readme-c.bash`. The Zig example builds and runs correctly today with the build line the README prints, and nothing checks it.
		- Opened: 20260904-175200

### Done

#### Done - Bugs

- ✅ The round went out green locally and red on the hosted runner: gcc 13 rejected what gcc 14 and 15 accept.
	- Reproduced: `apply_op`'s array branches allocate a slot array and fill only the first `an`, and at `an` of zero the setter is handed slots nothing wrote. The callee reads none of them, but a compiler that inlines the allocator cannot see that and calls them uninitialized. gcc 13 says so under `-Werror`; 12, 14, 15 and clang do not.
	- Cause: nothing in the array branches changed this round. Inlining did, because the round added a function beside them, and the warning is inlining-dependent.
	- Fixed: all four array branches zero their slots. Every compiler on this box now builds the C surface clean.
	- Pinned by a new gate, `check-c-compilers.bash`, which builds `main.c` and the runner with every gcc and clang present rather than only the default. The old gate used one compiler, which is why the local run could be green while the hosted one was not.
	- Opened: 20260831-160500
	- Closed: 20260831-161500

- ✅ With `--layer`, the diagnostics for FILE itself were dropped and only the lowest layer's were printed.
	- In the same fold as item 18 of the 20260830b round.
	- Reproduced in all four bindings: `fmt --layer=good.shcl broken.shcl` reported nothing, while swapping the two files reported the same damage correctly. `set` had its own copy of the fold and the same hole.
	- Cause: a merge does not carry diagnostics over, so the merged document only holds the lowest layer's. Both folds read them off it.
	- Fixed: the fold now collects each layer's diagnostics as it loads and hands them back with the document, lowest first. C keeps the over-layer documents alive for the same reason it keeps their text.
	- Pinned by `cli-regress.bash` rows `layer-base-diags` and `layer-base-diags-set`, which fail in all four bindings without the fix.
	- Left alone: the lines are still unlabeled, so a multi-layer report does not say which file a line number belongs to. Labeling would change single-file output too, which every corpus case pins.
	- Opened: 20260831-070237
	- Closed: 20260831-070237

- ✅ Four serious limitations and/or bugs in the C version, found while integrating v2.0.0 into nemo-anywhere.
	- The issues:
		- ✅ Every save fails on a backslash path
			- The temp name was split on `/` only. Now either separator on Windows, and a drive-relative `C:x` splits after the colon.
		- ✅ The library exits the caller's process
			- The five `exit(70)` sites go through one `SHCL_OOM()` macro an embedder can define. The default is unchanged.
			- Then the rest of it: a parse and a validate arm a recovery point, so an allocation failure unwinds out of them and the call returns NULL instead of reaching the macro at all. `shcl_load_file` follows its parse and the C++ veneer's `Document` tests false. The report's own sketch - carry on out of a small scratch block - was tried and abandoned: a bump arena holds its vectors' bookkeeping, so an allocation served out of nowhere hands back node indices that are no longer node indices.
		- ✅ File tier is code-page bound, not UTF-8
			- Every file call on Windows is the wide one now. A path that is not valid UTF-8 is refused rather than opened under another name.
		- ✅ Reader has no size limit and returns no bytes
			- `ReadFile(path, maxBytes)` in all four bindings and the veneer, and `LoadFile` is built on it. Past the cap is `Unreadable`, so the status enum did not change.
	- Much more detail, including recommended fixes, is in the defect report on file.
	- Check to see if other versions have the same problems.
		- Checked. The first three are C-only: Rust, Go and Python split paths and open files through their runtimes, and allocation failure there is the language's own contract. The fourth is a gap in all four.
	- Opened: 20260828-131305
	- Closed: 20260828-133327

- ✅ Hosted CI has been failing on every push since the supply-chain gates went in.
	- Cause: `staticcheck`, `govulncheck` and `cargo-deny` went into `LINT_EXTRA` and `TOOL_PINS` in the 20260819 round, but nothing was ever added to `ci.yml` to install them. The lint stage aborted at exit 127 about a minute in, so every run since has been red while the local gate stayed green. That is exactly how it went unnoticed.
	- Fixed by installing all three at their pinned versions. `cargo-deny` comes as a prebuilt binary with a sha256 pin, the same treatment `shellcheck` already gets: building it from source costs minutes and pulls a dependency tree the gate has no reason to compile.
	- `cargo-zigbuild` is still missing there and stays that way - it only feeds the cross stage, which `--ci` skips, so it is a warning and not a failure.
	- Second cause behind the same red, found once the first was cleared. The Go toolchain was `stable`, which had rolled to 1.27. staticcheck carries its own type checker that cannot read export data from a Go newer than the release it was cut against. Pinned to the 1.26 series in both jobs; move it when the staticcheck pin moves.
	- Opened: n/a
	- Closed: 20260821-121316

- ✅ A raw block body line ending in more than one carriage return is not a `fmt` fixpoint.
	- Same family as the two below, and the one none of the shallower runs could reach.
	- Minimal reproducer: a fenced block whose body line ends `\r\r\n`. Load strips one CR, emit writes the survivor back, and the reload reads `\r\n` as an ordinary line ending and drops it. All four bindings.
	- A raw body is the only content kept untrimmed, so it is the only place a trailing CR is visible at all; everywhere else the line trim removes it.
	- Fixed by taking the whole trailing CR run off at load, not just one. A line ending in CR has no spelling that survives a write, so normalizing once is the only stable answer, and it matches the line-ending policy already in place rather than inventing a second one. A CR inside a line is content and still round-trips untouched.
	- Verified: pinned by a fixture in all four runners rather than a corpus case: a golden holding the bytes would be rewritten by any platform's line-ending translation.
	- Opened: n/a
	- Closed: 20260818-183513

- ✅ A raw block whose body is entirely whitespace grows by one indent level on every `fmt`.
	- Out of reach of the character set the fuzzer used before Code review 20260817 item 29 widened it.
	- Minimal reproducer: `r:` then a fenced block at one tab whose only body line is a single tab. Each `fmt` adds a tab to that line, without bound. All four bindings, so the corpus is what can pin it.
	- Cause: the common indent a raw block strips on reload is computed from its non-blank lines, and this block has none - so nothing is stripped, while emit adds depth+1 tabs every pass. Pre-existing: reproduces before the performance pass, and arrived with Code review 20260817 item 7, which stopped blanking such lines.
	- Proposed fix: when a block has no non-blank content line, take the common indent from the whitespace-only lines themselves. That normalizes an all-whitespace body to empty once, which is the lesser evil against growth without bound - but it moves canonical output, so it wants a decision, a corpus case and a spec sentence.
	- Same family as the merge item below: raw blocks and whitespace. Settled together, as one branch.
	- Fixed, but by leaving the body alone rather than normalizing it to empty as proposed above. Emit adds no indent exactly where load stripped none, so the two stay inverses and the body survives byte-for-byte. Normalizing would also have ended the growth, but it discards whatever the body held, and a line of non-breaking or ideographic space is content, not layout, inside a construct whose whole promise is verbatim. Case 055 pins both, including the non-space-whitespace body.
		- Superseded by Code review 20260829 item 2: the stripped indent is now the closing fence's own, with no special case for a blank body.
	- Opened: 20260818-170931
	- Closed: 20260818-183513

- ✅ Merged output is not always a formatter fixpoint: an empty binding in the base and a same-named block in the overlay both survive the merge, where a parse of the two would fold them.
	- Needs a long soak to reach; the pipeline's shorter run never does. Pre-existing: reproduces identically on the commit before the performance pass.
	- Minimal reproducer. Base: `blk:` alone. Overlay: `blk:` carrying a raw block and a child. Merged, both survive; re-parsed, they fold, so the canonical form changes on the second pass.
	- Cause: the overlay's node has a child, so it takes the instance-merge path and looks for a base sibling with the same (name, value) key. An empty node's key never matches a block's, so it appends instead of filling - while the parser's own rule is that a later binding fills an earlier empty one of the same name. Parser and merge disagree about the same two lines.
	- A second face of the same bug, and the one that showed first. The emitter works around the pair by writing the block's fence on the name's line (`blk: ```info`), and the value half of that line is comment-split on reparse. So an info string containing `#` comes back as a trailing comment and the fence loses it outright. That half is content loss, not just instability.
	- Fixed: merge adopts the parser's empty-fill rule, so a merge and a parse of the two layers run together produce the same document. The fill is limited to a raw block, which is the limit of the parser's own rule; an unmatched valued instance still appends, as a parse of the same two lines does. Case 056 pins both halves.
	- The info-string half needed no separate fix: with the pair folded, the emitter never reaches the same-line-fence spelling for it, so the `#` survives. Pinned in the same case.
	- Raise the soak in the pipeline, or at least run a long one before a cut: the short gate cannot see this class. Related to Code review 20260817 item 29.
	- Opened: 20260818-163310
	- Closed: 20260818-183513

- ✅ `SaveFile` creates a brand-new file at mode 0600, whatever the umask says.
	- A file written through the file tier came out `rw-------` under a 0002 umask, where every other tool would have written `rw-rw-r--`.
	- Cause is one missing branch, not a mistake: `write_file_atomic` opens its temp file at 0600 on purpose, so the copy is never briefly readable to anyone the original was not, and then copies the real mode off the target. When the target does not exist yet there is no mode to copy, so the private one stays. All four bindings mirror it.
	- Two defensible answers, and it wants a decision rather than a patch. Either 0600 is the right default for a file that may hold secrets and the spec should say so out loud, or a new file should be created at `0666 & ~umask` like everything else a person runs. In the second case only an existing file's mode is preserved.
	- Cheap to settle now: the whole file tier is unreleased, so either answer is free today and a behavior change later.
	- Settled the second way: a new file is created at `0666 & ~umask`, like anything else a person runs. 0600 would be a surprise the caller never asked for and could not see. A config that needs to be private needs that from the umask or an explicit chmod, not from a library quietly deciding.
	- Fixed in all four bindings by choosing the temp file's create mode from whether the target already exists, rather than by chmod'ing afterwards. An existing target still gets a private temp and its own mode copied on, so nothing about the case the privacy was for changed.
	- Verified: pinned in all four runners rather than `crosscheck.bash`: the CLI cannot create a file at all (`set --write` on a missing FILE is an error), so the path is library-only and no CLI comparison can reach it. The fixture compares against a file made by the language's own ordinary create, so it states the rule without hard-coding a umask.
	- Spec says it now, in the file-tier paragraph beside the rest of the save's mechanics.
	- Opened: 20260820-075114
	- Closed: 20260820-120244

- ✅ The Python binding threw outright when saving over an existing file on Windows.
	- Cause: `os.fchmod` is POSIX-only, and the guard around the mode copy caught `OSError`; an `AttributeError` walks straight past it. So the whole call escaped `save_file`, which documents that it reports rather than throws. Only on the overwrite path, which is the common one.
	- Fixed by making the mode copy conditional on the function existing, which is the right condition: the mode concept is POSIX's, and Windows now carries the destination's attributes across in the publish step instead.
	- The runner fixture that missed it was POSIX-gated in all four bindings, because it asserts modes. Split so the create and the overwrite are exercised on every platform and only the mode assertions stay POSIX-only. The same fixture in all four, and it would have caught this.
	- Opened: n/a
	- Closed: 20260820-154417

- ✅ Canonical output dropped the author's quoting on plain strings, in `fmt` and in `generate` defaults.
	- From nano-git-db: a quoted `"fFoo()"` default came out bare in the starter file, so their function-ref convention read it back as a call. One `fmt` pass did the same to a document (`"@null"` -> `@null`), un-escaping the sentinel the `quoted` read flag exists to protect. `emit_element` re-derived quoting from content alone.
	- Done, all four bindings: a quoted element keeps its quotes unless the text reads as an int, float, bool, or datetime at standard strictness - those still normalize to bare (`ver: "8"` -> `ver: 8`). The clause only ever adds quoting over the reserved-character minimum, so no bare emit becomes unsafe (quoted thousands stay covered by the comma rule).
	- Goldens 017 (NUL string keeps quotes) and 030 (newline default now in its quoted spelling, which the spec prose had promised all along) moved; spec and `design.md` updated.
	- Opened: n/a
	- Closed: 20260817-111802

- ✅ A block header whose children are all commented handed those comments back one indent level shallow through `fmt`.
	- Reported from SilkTerm, whose template config is mostly commented-out defaults: `rotate:`, `contrast_mask:`, `text.scrim:`, `cursor.size:`, `selection:` all lost a level. That was the entire remaining diff against their template, all of it leading tabs.
	- Reproduced: `rotate:` followed by two depth-1 comments re-emitted them at column 0. With even one live child under the same header the comment kept its depth, so only childless headers lost fidelity, and the loss was always exactly one level.
	- Cause: after-trivia hung on the deepest open level whose indent prefixes the comment's indent. A childless header never opens its level, since no binding line ever resolves under it, so the comments hung one level up and re-emitted at the header's depth.
	- Fixed: a hung comment now splits by written depth. At the last binding's own level it trails that binding as before; deeper, it sits inside that binding's block at its own depth, so a childless header keeps its commented children indented.
	- Verified: all four bindings, new case 045 pins it, and goldens 027/034 moved - a tail note and a deep tail note keep their written depth now.
	- Opened: 20260804-143741
	- Closed: 20260804-151804

- ✅ The PowerShell wrapper's sourced `shcl` cannot be fed an op script on stdin. It is a function, so it forwards arguments but not pipeline input: `$ops | shcl set --write f.shcl` drops the ops and the CLI then blocks on console stdin until it is killed. The Bash wrapper pipes fine, so the two wrappers are not at parity, and `set` is the only subcommand that reads stdin.
	- Workaround, now in the README, is to pipe to the binary instead, resolved as `Get-Command shcl -CommandType Application` - plain `Get-Command shcl` returns the sourced function, whose `.Source` is empty. Callers should not need to know that.
	- Not crosscheck-visible: the wrappers are forwarders and deliberately sit outside `BINDING_CLIS`, so nothing in the pipeline exercises this.
	- Fixed: `shcl` forwards `$input` to the binary, but only when `$MyInvocation.ExpectingInput` says something was piped. The guard matters - forwarding an empty `$input` would hand the binary a closed stdin, so a bare `shcl set f.shcl` would read zero ops instead of the console.
	- Verified both modes against the real binary: dot-sourced with ops piped, and run as a script with a shell-level pipe (that one reaches the process stdin directly, not the PowerShell pipeline, so it takes the other branch). Same ops through the Bash and PowerShell wrappers now leave byte-identical files.
	- Opened: 20260803-162351
	- Closed: 20260804-080738

- ✅ Paths() silently drops any node whose name isn't a bare identifier, along with its whole subtree.
	- Reported from TradeClanker, where it was a live bug. A quoted field name parses with no diagnostics and reads back fine, but the enumeration skips it.
	- Impact: their unknown-field check walks Paths(), so a typo whose name needs quoting slipped past the one check built to catch typos. It also broke round-tripping, since the writer could create a node the enumeration would not report.
	- Cause: the skip was deliberate, and pinned by a doc comment, the spec wording, and the shared paths fixture in all four runners.
	- Fixed: non-bare segments now emit quoted and escaped, in the same spelling the canonical formatter uses, so every returned path resolves. The fixture, the doc comment, and the spec's traversal section moved with the code.
	- Opened: 20260802-110332
	- Closed: 20260802-113718

- ✅ A strict parse hands back nothing usable.
	- Reported from TradeClanker. The Go parse returned a nil document beside the error, so the natural `doc, err :=` followed by `doc.Diagnostics()` panicked.
	- Cause: the error value did carry the full diagnostics list, but the message was only a count, so the obvious path hid every line and code it was already holding.
	- Fixed both ways, in all four bindings. The failure now carries the parsed document, and the message names the first three diagnostics with line and code. Go returns the document non-nil beside the error, so the natural path cannot panic.
	- Opened: n/a
	- Closed: 20260802-120213

- ✅ `Raw` on a read result was not raw.
	- Reported from nano-git-db, which found it corrupting regexes. The doc comment promised the original text from the file, but every read filled the field from the canonical display form, which joins elements with a comma and a space.
	- Reproduced: `regex: ^\d{2,3}$` came back as `^\d{2, 3}$` with no diagnostics. Anything already written `a, b` round-tripped looking correct, so casual testing passed and only a value like `{2,3}` failed.
	- Fixed: the parser keeps the source line's value span, and reads hand it back verbatim. Writer-built values, stacked-list elements, and raw blocks fall back to the display form. Rust, Go and Python carry it - those are the bindings whose read result exposes the field.
	- Note: this also covers their separate request to read a comma-bearing scalar verbatim without a fenced block. Having the reader consult the schema's declared type before comma-splitting was declined, since it buys the same result at much higher complexity.
	- Opened: n/a
	- Closed: 20260804-095938

- ✅ By-value selectors matched the as-written spelling, not the logical string.
	- Reported from convert-base-v2. A `["q\"uote"]` selector and a document's `'q"uote'` are the same string but did not cross-match, because both sides kept their escape pairs verbatim and the comparison was spelling against spelling.
	- Impact: a silent NotFound. Anyone using a quote-bearing discriminator would hit it eventually. The spec pinned matching against the display form but said nothing about escapes.
	- Fixed: escapes are applied on both sides at every compare and index site, in all four bindings - the resolver, the parser's attach path, the writer's place walk, and the validator's contexts. The spec now pins the logical-string match, and corpus case 033 pins both the reads and the write path.
	- Opened: n/a
	- Closed: 20260804-095938

- Code review 20260902:

	- A fresh adversarial pass started from scratch, aimed at ground no earlier round had read: the C writer, resolver, generator and file tier; the generator bodies in all four bindings; the reference's read, coercion, datetime and validation code judged against the spec rather than against the other bindings; the CLI against its own help and man page; the Go and Python writers and file tiers; the merge code at library level; every gate under fault injection; and the windows builds run under wine. Twenty-eight defects here, seventeen enhancements under Features and enhancements. Everything marked confirmed was reproduced on this box; the two marked plausible rest on vendor documentation or a library-level probe in one binding.
	- Eighteen of the defects are shapes all four bindings share, so the four-way check cannot see them. Four are C or C++ only. Three are gates or fixtures that assert less than they claim. The fix round before this one (20260901b) cost nothing measurable: parse, fmt, bulk read, bulk write, absent defaults and float writes are unchanged against `b49501d`, and the did-you-mean workload went from 24 s to 0.09 s. A thousand structurally generated documents (bad dedents, fences and `*` lines at bad columns, content beneath skipped lines, bracket arrays, mixed indent) agree four-way and hold every fixpoint and write-gate property, so the E012/E013 change from that round held up.

	- ✅ Item 1: the reference CLI's `get --float` and the Rust generator's annotation line never got the round-half-even rule, so the distributed CLI prints a tie differently from the other three.
		- Reproduced: `f: 1125899906842624.2` through `get --float` prints `...624.3` from the reference and `...624.2` from Go, Python and C; same on `--array`, and same digit in `init`'s `# float, <lo>-<hi>` and `one of:` text for a tie-valued bound. The `set` path is right in all four, which is what 20260901b item 10 verified.
		- Cause: `main.rs` formats reads with `to_string()` at both `Kind::Float` arms, and `allowed_join` and `gen_annotation` in `lib.rs` do the same; `format_f64` is public and unused by either. The crosscheck's float dimension replays `set` only and corpus `080` reads one int, so nothing compared a float read.
		- Fixed: every float the reference prints goes through `format_f64` - both CLI read arms, the generator's `one of:` list and its numeric bound line, and the corpus runner's own float rows, which formatted reads the same wrong way.
		- Pinned by corpus `080` (four scalar float reads and a three-element array over the tie values, replayed through `get --float` by the crosscheck) and `026` (a float bound pair and a float `allowed` set, both tie-valued, in the generated annotation). Both diverged on the old reference and agree now.
		- Opened: 20260902-170000
		- Closed: 20260902-181500

	- ✅ Item 2: nothing pins the "counted as lost" half of 20260901b item 3, and the corpus has no way to pin any lost count.
		- Reproduced: with the one `lost += 1` after the index arm's `E002` deleted in a scratch build, the Rust corpus stays green (33 of 33), the crosscheck on `076` agrees, and `fmt --write` on `a: 1` / `a[0]: 2` exits 0 and deletes the second line. `project/conformance/README.md` and the backlog both say `076` pins it.
		- Cause: `expected-diags.txt` pins codes only, the write dimension exercises the save gate through its own BOM fixture and never through a corpus input, and the four runners assert `lost_count()` only in hand-written fixtures, none for the selector shape. The same gap covers every other "counted as lost" rule (items 1 and 2 of that round included).
		- Fixed: `reads.tsv` takes a `lost` pseudo-call, asserted by all four runners, and every case that drops content carries one; two cases that report errors while losing nothing carry a zero. The crosscheck now copies each corpus input into a fresh tree and runs `fmt --write` over it, comparing the exit code and the resulting bytes across the four, so the save gate is exercised by real inputs rather than by one hand-built fixture.
		- Pinned by the ten nonzero `lost` rows plus the per-case write comparison. With the increment deleted, the corpus fails on `076` and the crosscheck reports the file rewritten with two lines gone at exit 0.
		- Opened: 20260902-170100
		- Closed: 20260902-183000

	- ✅ Item 3: a wildcard followed by another wildcard reports every slot `Multiple`, and `Remove` on it removes nothing.
		- Reproduced in all four. `server[*].*` on three servers with two, one and no children gives three `Multiple` slots at exit 5, `count` says 3, `instances` prints three empty lines, and `set --remove='server[*].*'` leaves the document unchanged at exit 0. The spec says the two compose and that `Remove` on a wildcard removes every resolved slot. Same for `*.*` and `a[*].b[*]`.
		- Cause: `resolve_from` resolves the rest of the path per instance and maps anything that is not exactly one node to `Multiple`, so a nested slot list is `Multiple` whatever its length. Rust `lib.rs` both wildcard arms, Go `resolveFrom`, Python `_resolve_from`, C `resolve_from`.
		- Decided: flatten. The inner slots join the outer run, so the result is one slot per resolved leaf and `count`, `instances`, the read and `Remove` all see the same list. A repeated leaf under one instance still reports `Multiple`, since that is the same ambiguity a path with no wildcard has. Reasoning in `design.md` -> open sections; the spec's compose sentence now says so, and there is a changelog line under Changed.
		- Fixed: the two wildcard arms in all four splice a nested slot list into the outer one. Python's flat sub-walk grew an explicit stack so it can widen the run instead of ending it there; the other three had a recursive call already.
		- Pinned by corpus `081` (three instances with two, one and no children, plus a repeated leaf and a `*.port` read whose slots straddle two parents; the write dimension removes `server[*].*`). All four failed it before on the reads, the instance list and the removal, and pass now.
		- Opened: 20260902-170200
		- Closed: 20260902-190000

	- ✅ Item 4: a wildcard read over a parent that does not exist reports `Empty` where the path does not resolve.
		- Reproduced in all four. On a document with no `x`, `get --int --array x[*]` exits 2 and `get --int x` exits 3. The spec defines `Empty` as present but no value, and this is the tri-state the spec advertises.
		- Cause: the array read maps an empty slot list to `Empty` in all four (Rust `read_array`, Go `readArray`, Python `_read_array`, C `array_elements`).
		- Fixed: an empty slot list is `NotFound` in all four, so a wildcard whose parent is absent answers the same as the bare path.
		- Pinned by corpus `081` (`nosuch[*]` and `nosuch[*].deeper`, plus a count of zero). All four reported `Empty` before and `NotFound` now.
		- Opened: 20260902-170300
		- Closed: 20260902-191500

	- ✅ Item 5: a valued line generated from a filled wildcard is not in the generator's parent-value map, so its dotted child names the empty instance and a satisfiable schema refuses to generate.
		- Reproduced in all four. `field: a` required, `field: "a[*].b"` required with `default: bee`, `field: "a[*].b[*].c"` required: `init` exits 6 with `V097 required path missing: a[*].b[*].c`. The text behind the refusal is `a:` / `a.b: bee` / `a.b.c:`; the hand-corrected `a.b[bee].c:` validates clean. Same shape through a fragment mount under a valued parent.
		- Cause: the parent-value map is built only from constraints with no wildcard, so a filled wildcard line with a default never enters it, and `under_valued_parent` consults only that map. 20260901 item 5 covered concrete parents only.
		- Fixed: the map is built after the fill decision and takes filled wildcards too, so a line generated from `a[*].b` with a default is a valued parent like any other and `a[*].b[*].c` renders `a.b[bee].c:`.
		- Pinned by corpus `082`, whose schema generates and then validates clean against itself. All four refused it at exit 6 before.
		- Opened: 20260902-170400
		- Closed: 20260902-201500

	- ✅ Item 6: a valued live parent whose default is a bare integer is selected as `[8]`, which the scanner reads as an index, so `init` exits 6.
		- Reproduced in all four. `field: num` required `default: 8` plus `field: "num[*].v"` required generates `num: 8` / `num[8].v:`, which loads with `E003 no instance 8 of 'num'`; `init` reports V097. Same with `default: "8"` (emitted bare by the data-format rule) and with `type: int`. `[007]` and `[+3]` fail the same way.
		- Cause: `gen_selector_text` quotes only a default holding `[`, `]`, `\` or a newline; the spec's index rule (`[0]` bare numeric is an index, `["2020"]` quoted is a value) is not applied.
		- Fixed: the selector text quotes a default whose trimmed spelling the scanner would read as a selector of its own - all digits with an optional sign, the same with a leading `#`, or a bare `*` - beside the bracket and backslash cases it already quoted. Each binding asks its own scanner's number parser, so the four cannot drift.
		- Pinned by corpus `082` (`default: 8` under a wildcard child, and a `*` default beside it). All four refused the schema before.
		- Opened: 20260902-170500
		- Closed: 20260902-201500

	- ✅ Item 7: a must-exist path whose only wildcard is its last segment (`w[*]`) is never filled, so `init` exits 6; beside a `field: w` it is emitted twice.
		- Reproduced in all four. `field: "w[*]"` required `default: v` alone generates only the trailing block and fails V097; `w: v` on its own passes `check --schema`. With `field: w` added the output carries `# any, required` / `w: v` twice.
		- Cause: the fill loop wants a live line whose name list has the wildcard segment's own chain as a prefix; for a trailing wildcard that is the field's own name, so only a separate `field: w` satisfies it, and then both lines emit.
		- Fixed: a wildcard in the last segment is always fillable, since the line generated from it is the instance it needs, and no rendered path is emitted twice - the first spelling in schema order wins, so `field: w` beside `field: "w[*]"` produces one line. A duplicate whose default differs loses that default; the two spellings describe one field, and the surviving line still satisfies both.
		- Pinned by corpus `082` (a `w[*]` alone). All four refused the schema before.
		- Opened: 20260902-170600
		- Closed: 20260902-201500

	- ✅ Item 8: a stdout that cannot be written is reported as success by three CLIs, and as 120 by the fourth.
		- Reproduced: `fmt f.shcl > /dev/full` exits 0 with an empty stderr in Rust, Go and C; Python exits 120 with an interpreter message. Same for `check`, `get` and `set`. The help and man page say 8 for a stream that could not be written.
		- Cause: the reference's `out!`/`outln!` treat every write error as a broken pipe and exit 0 (on unix SIGPIPE is restored, so EPIPE never reaches the branch and every error that does is not a broken pipe); Go drops `fmt.Print` results; C never checks `ferror(stdout)`; Python never flushes inside `main`.
		- Fixed: every stdout write in all four goes through one pair of helpers that exit 8 with the OS message on a write error, and the buffered tail is flushed before the exit code is returned so a failure there is caught too. A broken pipe stays the quiet exit. The C CLI also points a standard stream that was closed before the start at the null device, which the other three runtimes already do - without it every write failed with EBADF and the next file opened landed on the closed descriptor.
		- Pinned by four `cli-regress` rows (`fmt`, `check`, `get` and `set` with stdout on a device that is always full). All four reported success before; the rows skip out loud where there is no such device.
		- Opened: 20260902-170700
		- Closed: 20260902-213000

	- ✅ Item 9: the reference aborts with nothing on stdout when stderr cannot be written.
		- Reproduced: `fmt bad.shcl 2>/dev/full` on a document with one diagnostic exits 134 (SIGABRT) with an empty stdout; Go and C exit 0 with the document; Python exits 120 with nothing. A single hint is enough, since every loading subcommand prints its diagnostics.
		- Cause: `eprintln!` panics on a write error and the release profile has `panic = "abort"`, so the abort lands before `out!` has printed anything. A closed stderr is fine (EBADF is discarded); a full or failing one is not.
		- Decided: a diagnostic that cannot be printed is dropped. There is nowhere to report the failure, the document on stdout is still good, and the exit code already carries the outcome - Go and C did this already.
		- Fixed: the reference's diagnostics go through an `errln!` that ignores the write result, and Python's stderr is wrapped so a failed write is dropped instead of raising.
		- Pinned by a `cli-regress` row that runs `fmt` on a document with a diagnostic, stderr on a full device, and requires the canonical text on stdout at exit 0. The reference exited 101 and Python 120 before, both with an empty stdout.
		- Opened: 20260902-170800
		- Closed: 20260902-213000

	- ✅ Item 10: a C parse leaves its temporaries in the scratch arena until the first resolve, so a parsed document holds about ten times its input in dead memory.
		- Reproduced: a 49 MiB input parses to 970 MB RSS with 487 MB of it in `d->scratch`; 10 MiB gives 204 MB with 97 MB dead. One `arena_free(&d->scratch)` at the end of `do_parse` gives 766 MB and 157 MB with parse time unchanged, and 46,600 writer, merge and compact operations under ASan and UBSan with that free applied show nothing live pointed into it.
		- Cause: `parse_body` takes `P.tmp = &d->scratch` for its lines vector, per-parent maps, stack and pending lists, and `do_parse` frees only the two `own` arenas on exit. The header describes `scratch` as per-resolve temporaries reset on entry to each resolve, and says nothing about the parser using it. The 20260901 round's C amplification numbers were measured after a read and so did not see it.
		- Fixed: `do_parse` gives the scratch arena back at its single exit. Every resolve resets it anyway, so nothing else changes.
		- Measured: a 15 MB document holds 398 MB after the parse instead of 569 MB; peak is unchanged, since the scratch is live while the parse runs.
		- Pinned by `mem_bounds.c`, which parses 20000 sections and requires the scratch arena to be empty afterwards. It held 12.3 MB against a 398 KB input before. The sanitizer run is clean, so nothing handed out points into it.
		- Opened: 20260902-170900
		- Closed: 20260902-215000

	- ✅ Item 11: a refused C setter still consumes document arena, against the header's "nothing is created on failure".
		- Reproduced: a refused 20 MB `set_string` on a wildcard path costs 85 MB of arena, permanently; ten refused `set_raw` calls with a 20 MB body cost 200 MB; small values cost 48 to 80 bytes per refused call. Rust builds the value first too but drops it when `place` refuses.
		- Cause: every setter encodes the value into `d->arena` before `w_set` validates the path. Only `shcl_compact` gives it back.
		- Fixed: the arena takes a mark and a release, and every setter marks before it encodes; a refusal rolls the arena back to the mark. `w_place` allocates only in scratch until its own probe passes, so nothing but the encoded value sits past the mark. No extra path walk, which a probe-first fix would have cost every successful write.
		- Measured: a refused 20 MB `set_string` costs nothing where it cost 55 MB; ten refused 20 MB `set_raw` calls cost nothing where they cost 265 MB; 10000 refused `set_int` calls cost nothing where they cost 480 KB.
		- Pinned by `mem_bounds.c`, which refuses 24 MB of writes on a wildcard path and requires the arena not to grow. It grew 27 MB before. Sanitizers, the compiler sweep and cppcheck are clean.
		- Opened: 20260902-171000
		- Closed: 20260902-222000

	- ✅ Item 12: the named-month space forms accept a day token that is not `DD`.
		- Reproduced in all four. `Jul +12 2026`, `+12 Jul 2026`, `Jul 0012 2026` and `Jul 00000000000000012 2026` all read Good as `2026-07-12`, while `Jul-012-2026` is refused and `Jul 12 +2026` is refused because the year is held to four digits. The spec calls the format list a closed whitelist and spells the day `DD`.
		- Cause: the space forms parse the day token with a full unsigned parse (Rust `u32` from_str, which takes a leading `+` and any leading zeros), mirrored deliberately by Python's `_parse_u32`, Go's `parseU32` and C's `parse_u32_lenient`; the delimited form uses the 1-2 digit `parse_num2`.
		- Fixed: both space forms read the day with the same one-or-two-digit parse the delimited forms use. The lenient u32 parse had no other caller and is gone from all three ports.
		- Pinned by corpus `007` (`Jul +12 2026`, `Jul 0012 2026`, `+12 Jul 2026`, all BadType). All four read them as 12 July before.
		- Opened: 20260902-171100
		- Closed: 20260902-224500

	- ✅ Item 13: Go's `LoadError.Diagnostics` and Python's `diagnostics()` and `LoadError` hand out the document's own list.
		- Reproduced: in Go, setting `le.Diagnostics[0].Message` on a strict-load error changes `doc.Diagnostics()[0].Message`; in Python `doc.diagnostics().clear()` takes `error_count()` to 0, and `LoadError.diagnostics` is the same list object. 20260901b item 23 fixed Go's `Diagnostics()` and the two suppressors and did not reach the third hand-out; Python was not looked at.
		- Fixed: Go's `LoadError` and Python's `diagnostics()` and `LoadError` each hand back a copy. The suppressors keep their in-place contract, which mirrors the reference's `&mut Vec`.
		- Pinned by the Go aliasing test, extended to write through a `LoadError`, and by a new fixture in the Python runner that clears both what `diagnostics()` returned and what a failed strict load carried. Both fail on the old code.
		- Opened: 20260902-171200
		- Closed: 20260902-230000

	- ✅ Item 14: the Go CLI exits 1 where the other three exit 8 on an ops script that is not UTF-8.
		- Reproduced: `printf '\xff\n' | shcl set f.shcl` exits 8 in Rust, C and Python and 1 in Go. The same bytes as a document exit 8 in all four. The standing decision makes 1 the usage code alone.
		- Cause: `main.go`'s ops read returns 1 after `utf8.Valid` fails; its `readInput` already takes the exit-8 path for a document.
		- Fixed: the ops read takes the same exit-8 path its document read does.
		- Pinned by a `cli-regress` row feeding a lone `0xff` to `set`. Go exited 1 before.
		- Opened: 20260902-171300
		- Closed: 20260902-231000

	- ✅ Item 15: a refused `--set` or a failing op suppresses the load's diagnostics.
		- Reproduced in all four. On a document with an `E014` line, `get --set='a[*]=1' f a` prints only the refusal at exit 1 and never the `E014`; same for an ops line the writer refuses. The help says every subcommand that loads a document prints the load's diagnostics once per run.
		- Cause: `load_layered` returns the refusal before the caller's `say_diagnostics`, and `do_set` returns from the `--set` refusal and the op failure before its own call to it.
		- Fixed: the layered load prints its own diagnostics, before the edit list runs, in all four; the five callers that printed them afterwards no longer do, so a run still reports once. `set` does the same before its option edits and its ops script.
		- Pinned by two `cli-regress` rows: a refused `--set` and a refused ops line on a document with an `E015`, each of which must still report it. All four printed only the refusal before.
		- Opened: 20260902-171400
		- Closed: 20260902-234500

	- ✅ Item 16: a layer's leading blank line survives the load but not the canonical form, so a merge of a file and a merge of its `fmt` differ.
		- Reproduced in all four. With `A` holding `a: 1` and `B` holding a blank line then `b: 2`, `fmt --layer=A B` prints `a: 1`, a blank, `b: 2`, while `fmt --layer=A <(fmt B)` prints no blank. Same with a leading comment and with a comment-only layer. In a 700-seed soak, 129 seeds (every layer beginning with a blank line) gave a different result with an in-memory merged document as `over` than with its reparse; as `base` it never differed.
		- Cause: the parser sets the pending blank on the first bound node like any other and the emitter suppresses it only at output start, so `load(emit(load(B)))` differs from `load(B)` on that one bit and `merge` copies it unchanged.
		- Fixed: the parse clears the blank on whatever canonical output would print first - the first root child, its first leading comment, or the first footer line - in all four. One place, so no emitter or merge special case is needed.
		- Note: the property found two more shapes of the same defect that the finding did not name: a blank after a leading line the load dropped (a BOM-led one), and a blank on a later instance that merged into the first. Clearing at the emitted-first position covers all three; dropping it only at the start of the file covered one.
		- Pinned by corpus `083` (a layer leading with two blank lines, plus a comment-only layer leading with one) and by a new fuzz property: merging a layer must equal merging its canonical form. Both failed in all four before; the property fails within 30000 iterations on each of the three shapes.
		- Note: the three ports first got a parse-time version of this as well, and keeping both made `a.b: 1` under a leading blank drop a blank the reference kept. The crosscheck's fuzz dimension caught it. One place decides it now, in all four.
		- Opened: 20260902-171500
		- Closed: 20260903-001500

	- ✅ Item 17: the crosscheck's float-spelling dimension writes `0` for every subnormal power of two under gawk, so its "every power of two" claim is false on this box.
		- Reproduced: `gawk 'BEGIN{printf "%.17g", 2^-1074}'` prints `0` (it computes a negative power as `1/(2^1074)`, which is `1/inf`); mawk prints the subnormal. The generated ops file starts with 52 `float p<n> 0` rows. The hosted runner's default awk decides which rows it exercises there, and `srand(20260902); rand()` differs per awk as well, so the "fixed" random set is not fixed either.
		- Fixed: the powers are built by exact halving and doubling from 1, and the random set comes from a fixed integer generator whose every product stays under 2^53. gawk, mawk and busybox awk now write byte-identical ops files, subnormals included.
		- Pinned by `shell-regress.bash`, which lifts the generator out of `crosscheck.bash` by name, runs it under every awk on the box, and requires no zero-valued power row and identical output. The old generator fails all three ways.
		- Opened: 20260902-171600
		- Closed: 20260903-003000

	- ✅ Item 18: `sanitize-c.bash` never runs the `children` and `paths` commands, against its claim to replay every `reads.tsv` row.
		- Reproduced: its row replay has no arm for the two, so a `children` row becomes `get --children`, which the C CLI refuses at exit 1 (not 77), and the run counts as clean. Six corpus cases carry such rows. The two enumeration paths in `main.c`, including the quoted-name spelling, run under no sanitizer.
		- Fixed: the two arms are there, and a row type with no arm is now an error in both replays instead of a `get --<type>` every binding refuses the same way.
		- Note: found while working it, and fixed with it - both replays split a row with `IFS=$'\t' read`, and tab is IFS whitespace whatever IFS is set to, so a leading or doubled tab disappeared and every later column shifted. The top-level `children` row (empty query) arrived as a type nothing had an arm for, and had never been replayed by either gate. Both split by hand now.
		- Pinned by `shell-regress.bash`: the splitter is lifted out of `crosscheck.bash` by name and must keep a leading and a middle empty field, and the unknown-type guard fires with the arms removed.
		- Opened: 20260902-171700
		- Closed: 20260903-005000

	- ✅ Item 19: a must-exist path with a `[#N]` selector, one past the depth cap, or one carrying a literal newline gets V097 where the spec describes the trailing block.
		- Reproduced in all four. `field: "srv[#1].port"` required (alone or beside a live `field: srv`) exits 6 with `V097 required path missing`; same for a 513-segment required path and for `field: "\"a\nb\""` required. The spec's trailing-block sentence lists all three as "collected into a trailing comment block", and its self-check sentence requires the output to validate, and a must-exist path in the trailing block can never satisfy both.
		- Cause: `unwritable` sends them to the trailing block and the self-check then reports the missing path. The newline clause is also stale on its own: names resolve escapes since 2026-08-18, `emit_name` spells a newline as `\n`, and `gen_path_text` already goes through it, so the path is writable.
		- Decided: refuse, naming the path. The trailing block can never satisfy a must-exist path, so the self-check's "required path missing" points a reader at the generated config when the problem is the schema line. A `repeat` lower bound of 2 or more keeps its documented shortfall and still generates.
		- Fixed: a must-exist path that cannot be written is a `V097` fault carrying the path, in all four, before anything is emitted. The newline clause is gone from the unwritable test - only a newline inside a selector is unwritable now - and a path whose text holds one renders through the segment renderer, which escapes it.
		- Pinned by a `cli-regress` row (`field: "srv[#1].port"` required, message and exit) and corpus `082`, whose schema now carries a quoted escaped name. All four gave the old message before.
		- Note: a literal newline in a schema path is unreachable from a file - a schema value holding one is an unterminated quote - so the newline half of the fix is defensive and has no case of its own. Its old clause never fired either, which is why the escaped spelling already worked.
		- Opened: 20260902-171800
		- Closed: 20260903-011500

	- ✅ Item 20: V096 fires at exactly 10000 fields with a message that says the schema expands past 10000.
		- Reproduced in all four: 9999 plain `field:` lines generate, 10000 give `V096 schema expands past 10000 fields; fragments mounted at more than one path multiply`, on a schema with no fragments.
		- Cause: `cons.len() >= GEN_MAX_FIELDS` in `generate` and inside each `expand_mounts`.
		- Fixed: the ceiling means at most that many, in all four. The expander stops one past it so `generate` can tell the two apart.
		- Pinned by two `cli-regress` rows, a schema of exactly 10000 fields and one of 10001. The first exited 6 before.
		- Opened: 20260902-171900
		- Closed: 20260903-013000

	- ✅ Item 21: `shcl_generate` keeps the output of a V097-failing call in the schema's arena, and a succeeding call's output can never be released.
		- Reproduced: 1000 calls on a schema whose default fails its own constraint grow the heap by 21.9 KB per call while returning nothing, and leave 1001 copies of the same diagnostic on the schema; 1000 succeeding calls grow it by one output each, and `shcl_reads_release` cannot reclaim them because the output goes to `schema->arena`, not `schema->reads`. The header's own comment says everything but the returned bytes dies inside the call.
		- Cause: the output is copied into the schema arena before the self-check, and the failure path returns empty without reclaiming it; faults are appended to the schema on every call.
		- Fixed: the self-check runs on the private-arena text and the bytes are copied into the schema's read arena only on success, so a refusal keeps nothing and a caller can reclaim what it got. V096 and V097 come from nowhere else, so any left on the schema are dropped before generating - the list describes this call. The header says both, `shcl_reads_release` lists generation, and the veneer's `generate()` releases first like every other copying wrapper.
		- Measured: 200 refused calls grow the schema by 19.5 KB of diagnostic text (it was 49 KB plus 200 stacked diagnostics), and 200 succeeding calls with a release between leave the document exactly as it started (it grew 6.4 KB).
		- Pinned by `mem_bounds.c`, which fails both ways on the old header.
		- Opened: 20260902-172000
		- Closed: 20260903-015000

	- ✅ Item 22: `shcl.hpp` tells the veneer user to recover generation faults by validating an empty document, which cannot reproduce them.
		- Reproduced: `validate(empty, schema)` after a failed `generate()` gives `V002`, never the `V097` the generator recorded. V096 and V097 are generation-only codes. The C CLI made the same mistake and was fixed in the 20260830b round; the veneer comment kept the old recipe.
		- Fixed: the comment says to read `diagnostics()` on the schema after the call, and says why validating cannot answer it.
		- Pinned by `check-docs.bash`, which refuses the old sentence, and by `veneer_smoke.cpp`, which requires the V097 to be on the schema and absent from validating an empty document against it.
		- Opened: 20260902-172100
		- Closed: 20260903-020000

	- ✅ Item 23: the datetime whitelist admits spellings the spec does not list.
		- Reproduced in all four, all Good: `9:30` (one-digit hour with no meridiem), `2026-7-1` and `2026/7/4` (one-digit month and day), `14:30z` and `2026-07-12t14:30` (lower case), `14:30 Z` and `14:30 +05:00` (space before the zone), `2026-07-12  14:30` (two spaces as the separator). The spec says the formats are a closed whitelist and anything else is BadType. Correctly refused in the same run: `24:00`, `14:30:60`, `2026-02-29`, `12-Jul/2026`, `2026-07-12 T 14:30`, `Jul 12,2026`.
		- Cause: `parse_num2` takes one or two digits, the zone and meridiem arms trim before matching, and the combined-separator scan trims around it; same functions in all four.
		- Decided: list them. Every one is a spelling a person writes by hand and none can be misread, so tightening would turn working configs into `BadType` for nothing - against the forgiving-parser stance the rest of the language takes. The whitelist claim is what was wrong, not the code.
		- Fixed: spec text only. The datetime section names the tolerances (a one-digit hour, month or day; a lower-case `z` or `t`; whitespace before the meridiem or zone; a run of spaces as the combined separator) and says what is still refused inside a value.
		- Pinned by corpus `007`: six tolerance rows read Good and the two separator shapes read BadType, so the set cannot drift either way.
		- Opened: 20260902-172200
		- Closed: 20260903-021500

	- ✅ Item 24: at Loose, `$ 1200` reads as 1200 but `$ 3.14` is BadType.
		- Reproduced in all four: whitespace after the currency symbol is tolerated on the integer path only, because the float path tests the shape on the untrimmed remainder and then falls into the integer path, which trims.
		- Fixed: the currency strip takes the space with the symbol, in all four, so both paths see the same remainder.
		- Pinned by corpus `003` (`"$ 3.14"` reading 3.14 as a float and 3 as an int at Loose, and BadType at Standard). All four read BadType before.
		- Opened: 20260902-172300
		- Closed: 20260903-023000

	- ✅ Item 25: `Jul 12, 2026` is listed as an admitted date spelling, and bare in a file the comma splits it into a two-element array.
		- Reproduced in all four: `b: Jul 12, 2026` reads BadType (two elements, `Jul 12` and `2026`) while the quoted spelling reads `2026-07-12`. The code is right by the array rule; the Integers section says "only inside quotes, since `,` is reserved bare" and the datetime bullet says nothing.
		- Fixed: spec text only. The bullet says the comma spelling works only inside quotes, and what a bare one reads as instead.
		- Pinned by `check-docs.bash` (the bullet must carry the clause) and corpus `007`, where the bare spelling reads BadType as a date and two elements as a string array.
		- Opened: 20260902-172400
		- Closed: 20260903-024000

	- ✅ Item 26: a one-element array read reports `quoted` false for a quoted element.
		- Reproduced in Rust at library level (plausible for the other three by port): `read_int("h")` on `h: "5"` gives `quoted` true, `read_int_array("h")` on the same node gives false, because the array read always ends with `quoted` false. The spec's flag is "true when the read's single scalar element was quoted in the source".
		- Fixed: the array read takes the flag from the element when the cell holds exactly one, so it agrees with the scalar read of the same node. C needed nothing: `shcl_quoted` is path-based and already answered for a one-element cell.
		- Pinned by the read-surface fixture in all three runners: a one-element quoted cell reads quoted as an array, a bare one does not, and a two-element cell reports nothing. All three failed the first before.
		- Opened: 20260902-172500
		- Closed: 20260903-025500

	- ✅ Item 27: stale prose in the README, the man page and the spec.
		- Reproduced by re-running the transcript and reading. `README.md` line 504's `get` transcript omits the `E014` line every loading subcommand has printed since 20260831, while the `check` transcript above it shows its stderr; `README.md` line 830 says raw blocks, set-only-if-absent and removal have no option form, though `--set-default`, `--set-literal-default` and `--remove` exist since 20260830b item 21; the man page's WRITE OPS sentence says the ops script is read when no `--set` or `--set-literal` carries the edits, where the code reads it only when none of the five options is given; `spec.md` line 599 lists the loading subcommands without `children` and `paths`.
		- Fixed: all four, with the transcript re-run rather than edited by hand. Raw blocks are named as the one edit with no option form.
		- Pinned by `check-docs.bash`, driven off the CLI rather than off a copy of any list: an edit option in the help must appear in the README paragraph and the man page, the WRITE OPS sentence must name all five, every subcommand that accepts `--layer` must be in the spec's list of them, and the README's `get` transcript on the damaged file must show a load diagnostic. Six checks fail on the old text.
		- Opened: 20260902-172600
		- Closed: 20260903-032000

	- ✅ Item 28: the windows publish's durability claim rests on a flag Microsoft documents as unsupported, and "attributes" in the carry-over claim is broader than what `ReplaceFile` preserves.
		- Plausible: `design.md`'s file-tier decision says Windows has no directory sync and `ReplaceFile` is asked to write through instead; the Go comment says the flag means "do not return until the change is on the disk". Microsoft's `ReplaceFileW` reference lists `REPLACEFILE_WRITE_THROUGH` as "This value is not supported", and its preserve list (creation time, short name, object id, DACLs, security attributes, encryption, compression, named streams) does not include the basic attributes. Under wine, hidden and system are lost across a `set --write` in all three bindings; only read-only, which the code handles by hand, survives. Real NTFS could not be checked here.
		- Decided: carry them. Losing hidden on a saved config is a change the author never asked for and cannot prevent except by re-setting it after every write, so the attributes are re-applied rather than the promise narrowed.
		- Fixed: all four re-apply hidden and system to the target after the publish, alongside the read-only restore that was already there. Go does it through two more hooks in `shcl_windows.go`, so `shcl.go` stays droppable on its own. The wording in `design.md`, `spec.md` and the four publish comments now says security attributes and named streams, says the basic attributes are re-applied by hand, and says the WRITE_THROUGH flag is documented as unsupported so durability rests on the file's own flush.
		- Pinned by the windows save fixture in all four runners: a file marked hidden and system is saved and must come back with both. Rust, Go and C fail it on the old code under wine; Python's runs on the hosted job, where windows Python is.
		- Opened: 20260902-172700
		- Closed: 20260903-041500

- Code review 20260901b:

	- The areas the 20260901 round recorded as not reached: the C parser and emitter read line by line, Go's validation walk, Python's validator, `v_suggest`, a full run under mingw and wine, the installers and packaging, `--layer` and merge semantics, and the three tooling scripts nobody had opened. Twenty-three defects here, the rest under Features and enhancements. Everything below was reproduced, not read off the code.
	- Nine of the defects are shapes all four bindings share, so the four-way check cannot see them. Seven are C or C++ only, which it also cannot see. Four were found only by running the windows builds; two are the release tooling.

	- ✅ Item 1: a line refused with `E012` does not hold its indent level, so what was written under it re-parents, and a refused fence line's body is parsed as live bindings.
		- Reproduced in all four bindings. `d: 3` written under a refused `c: 2` becomes a child of the level above it, where the spec's `E018` row says it is skipped with the line it sits under. The 20260829 fix that added `E018` covered the `E014` and `E021` arms and missed all three `E012` arms.
		- The fence arm is the damaging one. A fence at a bad indent is skipped but its body is not consumed, so raw content becomes root bindings and the closing fence opens a second, unterminated block.
		- Fixed: a line whose indent matches no open level now holds that indent as a level of its own, in all three `E012` arms. Deeper lines are skipped under it (`E018`), a refused fence line consumes its body, and a second line at the same bad indent is refused the same way rather than binding one level up. Spec rule reworded.
		- Pinned by corpus `075`, which every binding failed before the change (a fence body read as root bindings, and a bad-indent line's children re-parenting up) and passes now. Existing `061` (a single `E012` line) is unchanged.
		- Opened: 20260901-190000
		- Closed: 20260902-090000

	- ✅ Item 2: a line refused with `E013` (a `*` with no space) does not hold its level either, so its next sibling is lost.
		- Reproduced in all four. Content under the bad line re-parents up, and the line's own next sibling then reports `E012` and is dropped. The `E014` arm beside it, same class of defect, does this right.
		- Fixed: the `*`-with-no-space arm resolves its level first, like the `E014` arm beside it, and pushes the level as skipped. Its sibling binds where it should and what is written under it is skipped with it.
		- Pinned by corpus `075` (the `p` block), failing before and passing now in all four.
		- Opened: 20260901-190100
		- Closed: 20260902-090000

	- ✅ Item 3: a value after a last-segment index selector is dropped with no diagnostic and no lost count, so a save deletes it.
		- Reproduced in all four. `a[0]: 2` loads clean and formats to nothing; the same with a raw block loads clean and the block vanishes. `fmt --write` exits 0 and bakes it in. The spec says the trailing value is reported as an error and counted as lost; the value-selector arm does exactly that, the index arm has no check at all.
		- Fixed: the index arm carries the same last-segment check as the value arm: the trailing value is reported (`E002`) and counted as lost, so an in-place write refuses instead of deleting it.
		- Pinned by corpus `076` (plain value and same-line fence after `a[0]`, plus a value after a non-last index that still binds). Failed in all four before, passes now.
		- Opened: 20260901-190200
		- Closed: 20260902-091500

	- ✅ Item 4: a fragment mounted at one node by two top-level schema paths is checked twice, and its diagnostics repeat.
		- Reproduced in all four. `field: a` and `field: "a[*]"` both inheriting one fragment report each of its faults twice. The spec says once per node. The dedupe set is created per top-level constraint, so it only covers mounts reached inside one constraint's own recursion.
		- Fixed: the mounted set is created once per validation and shared by every top-level constraint, so a (fragment, node) pair runs once whichever paths reach it. The per-constraint wrapper is gone in all four.
		- Pinned by corpus `077` (`srv` and `srv[*]` both inheriting one fragment; each fault once). Failed in all four before, passes now.
		- Opened: 20260901-190300
		- Closed: 20260902-093000

	- ✅ Item 5: `H001`/`H002` disavowal re-reads the schema through the raw path text and the raw `repeat` value, so an escaped quote in a segment defeats it and a faulted `repeat` still disavows.
		- Reproduced in all four. `field: "a.\"b.c\""` with `repeat: 2` still prints the hint, while the unescaped spellings work. `repeat: 0x2` and a three-element `repeat` are schema faults and still silence the hint.
		- Cause: both suppressors take paths from `instances()` display text and read `repeat` with the loose array read, where the schema build reads both correctly. Deriving the disavowed names from the built constraints closes both.
		- Fixed: both suppressors take their names from the built schema (a shared `disavowed_names` helper over the top-level constraints and every fragment's), so the leaf name is the resolved one validation uses and a `repeat` or `reopen` that faulted disavows nothing. The constraint carries `reopen` now; the display-text and loose-array reads are gone from all four.
		- Pinned by corpus `078` (escaped-quote path with `repeat: 2`, `repeat: 0x2`, a three-element `repeat`, `reopen: yes`, `reopen: 0x1`). Failed in all four before, passes now.
		- Opened: 20260901-190400
		- Closed: 20260902-100000

	- ✅ Item 6: a merge appends unmatched `over` nodes grouped by name, where the spec says file order.
		- Reproduced in all four. `c, s, c` in the layer comes out `c, c, s`. So merging onto an empty base is not the identity, and a layered `fmt` reorders siblings the layer's author put in a deliberate order. Reads are unaffected, which is why nothing caught it.
		- Fixed: each appended clone remembers its position among the over node's children and the rebuild emits them in that order. Replaced leaf groups still splice at the name's first base position, as the spec says.
		- Pinned by corpus `079` (layer appends `c, a, b, a` in file order) and the merge fuzz property, which now also asserts that a merge onto an empty document is the identity over every seed and mutation. Both failed in all four before and pass now.
		- Opened: 20260901-190500
		- Closed: 20260902-104500

	- ✅ Item 7: a layer's own repeated footer comments collapse to one.
		- Reproduced in all four. The once-per-layer footer dedupe compares each line against a list that already holds the lines just added from the same layer, so a within-file repeat is dropped. Another way an empty-base merge fails to be the identity.
		- Fixed: the footer dedupe compares against the lines the base held before the merge, so a layer's own repeated line is kept and a line the base already has is still carried once.
		- Pinned by corpus `079` (`# more` twice in the top layer) and the same identity property.
		- Opened: 20260901-190600
		- Closed: 20260902-104500

	- ✅ Item 8: the did-you-mean suggestion is quadratic in name length, so a check against a schema with long field names takes seconds to minutes.
		- Measured: 300 unknown fields of 300 characters against 300 schema names of 300 characters, 188 KB in all, takes 23 s in the release reference, 27 s in Go, 10 s in C. Double both lengths and the reference did not finish in two minutes.
		- Cause: a full Levenshtein table per pair with no prefilter, while the result is discarded past distance 2. A length difference over 2 can be rejected outright, and a banded table bounded by the threshold makes the rest linear.
		- Fixed: the edit distance takes the cap (2): a length gap past it returns at once, only the band of cells within the cap is computed, and a row whose minimum passes the cap ends the pair. The 188 KB repro went from 23 s to 0.06 s in the reference; the 2000-character shape that did not finish in two minutes takes 0.04 s. The banded result equals the full table for every distance at or under the cap (checked over 200k random pairs at four caps), so no suggestion changes.
		- Pinned by `perf-gate.bash`'s new `suggest` workload: 30 unknown 800-character names against 30 schema names of the same length, timed against the binding's parse baseline. Over budget on the old code in every binding (17 s, 1.9 s, 86 s, 0.66 s for rust, go, python, c) and 10 to 370 ms now.
		- Opened: 20260901-190700
		- Closed: 20260902-113000

	- ✅ Item 9: C formats an exact power of two with 17 digits where the other three print 16.
		- Reproduced, C only: 46 of the 2098 powers of two, none of 11,206 random doubles. `get --float` and a float write op both show it, so it reaches a saved file.
		- Cause: the shortest-round-trip loop tries only the correctly rounded string at each precision. At a power of two the rounding interval is lopsided and the neighbor one digit up is the one that round-trips.
		- Fixed: at each precision the C formatter also tries the spelling one last digit up and one down before adding a digit, which is the neighbor a shortest-digits algorithm picks when the closest spelling falls outside a lopsided interval. All 2098 powers of two and 208k doubles now spell the same in all four.
		- Pinned by corpus `080` (the 46 powers of two plus the ties) and a `float spelling` dimension in `crosscheck.bash` over every power of two and 3000 fixed random doubles. Both diverged on the old C and agree now.
		- Opened: 20260901-190800
		- Closed: 20260902-121500

	- ✅ Item 10: on a value exactly halfway at 17 digits, the reference rounds up and the other three round half-even.
		- Reproduced: `2.9802322387695312e-08` written back is `...313` from Rust and `...312` from Go, Python and C. Two hits in 11,206 random doubles; both spellings parse to the same double.
		- Needs a decision on which side to match. Matching the reference means each port detects the tie and bumps the digit; matching the three means the reference post-processes its own formatter.
		- Fixed: decided for round-half-even: it is IEEE's own tie rule, what three of the four did already, and what Python's `repr` and Go's `strconv` print, so a value read from another tool's output spells the same here. The reference now takes the correctly rounded spelling of the shortest length whenever it reads back (core rounds that to even), and keeps its shortest spelling only when it does not (the lopsided power-of-two case, where every binding has one choice). Ties turned out to occur at any length, not only 17 digits: `811212085039910.25` is one at 16.
		- Pinned by corpus `080` and the crosscheck float dimension; the old reference diverged on nine of the corpus values.
		- Opened: 20260901-190900
		- Closed: 20260902-121500

	- ✅ Item 11: the distributed CLI aborts on Windows when the program reading its output closes early.
		- Reproduced under wine: `fmt` of a 40k-key file piped through `head` panics with "failed printing to stdout: Pipe not connected", and the release build turns the panic into an abort. Go and C exit 0 silently there, and all three are silent on Linux.
		- Cause: the broken-pipe handling restores the default signal action and is compiled only on unix. Windows has no SIGPIPE; the write returns an error and the next print panics. Piping into `more` or `Select-Object -First` is ordinary use.
		- Fixed: every stdout write in the CLI goes through one pair of macros that exit 0 quietly when the write fails, which is what Go and C do on windows. unix is unchanged: the SIGPIPE default is still restored and the signal ends the process before the error branch is reached.
		- Pinned by `tests/cli_pipe.rs`, which runs the built CLI with a reader that closes after one byte and requires an empty stderr and a clean exit (SIGPIPE or 0 on unix, 0 elsewhere). It runs on the hosted windows job; on the windows build it fails on the old code with the panic text on stderr and passes now.
		- Opened: 20260901-191000
		- Closed: 20260902-124500

	- ✅ Item 12: the C CLI on Windows takes its arguments in the active code page and checks them as UTF-8, so a non-ASCII path is refused, and a name the code page best-fits maps to a different file, including for `--write`.
		- Reproduced under wine with code page 1252. `café.shcl` is refused as bad encoding. With `ā.shcl` and `a.shcl` both present, `set --write` on the first exits 0 and rewrites the second.
		- The library's own file tier was made wide in the 20260828 fixes; the CLI's narrow `main` and the header's silence on argument encoding are what is left. The C CLI is not distributed, but a C consumer's own `main` inherits the same trap.
		- Fixed: on windows the C CLI takes its arguments from the wide command line and converts them to UTF-8, and its own file opens and the directory check go through the library's wide calls instead of the narrow `fopen`/`_access`. The header's file-tier comment now says paths are UTF-8 on every platform and that a consumer's `main` has to hand over UTF-8 too.
		- Pinned by a `c cli argv` step in `win-runners.bash`, which builds the C CLI on the windows job, reads `café.shcl` and writes `ā.shcl` beside an `a.shcl` that must stay untouched. On the old build the first is refused and the second rewrites `a.shcl`; both pass now.
		- Opened: 20260901-191100
		- Closed: 20260902-140000

	- ✅ Item 13: on Windows, `shcl_write_file_atomic` reports failure with `errno` untouched when the publish step fails, so the C CLI prints `Success` beside exit 8.
		- Reproduced under wine: a target held open by another process, or a device name as the target, both print `<file>: Success`. Rust and Go name the real cause.
		- Cause: the wide publish calls set the Win32 last error and nothing maps it to `errno`, against what the header promises.
		- Fixed: a failed publish maps `GetLastError` onto errno (`EACCES` for a sharing or lock violation, `ENOENT`, `EEXIST`, `ENOSPC`, `EINVAL`, `EIO` for the rest), and the temp-file unlink on the failure path no longer overwrites the errno the failure left, on either platform.
		- Pinned by a windows-only fixture in the C runner: a write over a target held open without delete sharing, and one to a device name, both fail with a non-zero errno. Fails on the old header, passes now.
		- Note: wine's `strtod`, mingw's `__mingw_strtod` and the hosted windows runner's C runtime all read `7.67844768714563e-239` one ulp high where glibc and Python agree, which failed corpus `080` on the C runner there. The formatter's read-back test is now integer arithmetic on the double's rounding interval (`f64_interval` / `f64_reads_back`), agreeing with glibc on 20 million spellings, so the C spelling no longer depends on the libc. The parser's float reads still go through `strtod`; recorded as a deviation in `style-guide.md`.
		- Opened: 20260901-191200
		- Closed: 20260902-140000

	- ✅ Item 14: the C runner's Windows read-only fixture cannot see a leftover temp file, because it skips every dotfile and the temp name starts with a dot.
		- Reproduced: a planted `.ro.shcl.tmp999.0` passes the fixture. The other three runners count it.
		- Fixed: the fixture skips only `.` and `..` when counting what the write left behind, so a leftover dot-named temp counts like it does in the other three runners.
		- Pinned by planting `.ro.shcl.tmp999.0` in the fixture's directory: the old filter counted nothing, the new one fails the fixture.
		- Opened: 20260901-191300
		- Closed: 20260902-140000

	- ✅ Item 15: the `.deb` and `.rpm` declare no dependencies, so they install cleanly on a system where the binary cannot run.
		- Reproduced with the pipeline's own packages: lintian reports undeclared ELF prerequisites and the rpm lists no requires. The binary needs glibc 2.34 and libgcc; on Debian 11 or RHEL 8 the package installs and `shcl` dies with a loader error, which is exactly what the installer's glibc check exists to prevent.
		- Also from the same lintian run: no copyright file, no changelog, an unknown `License` field. Cheap to close together.
		- Fixed: the packages declare what the binary links against, read off the binary at build time: its newest `GLIBC_` symbol version is the glibc floor (`libc6 (>= 2.34)` and `glibc >= 2.34` on x86_64, 2.30 on arm64), and `libgcc-s1` / `libgcc` join only when `libgcc_s` is in the dynamic section (x86_64 only; arm64 links it statically). The deb also carries `/usr/share/doc/shcl/copyright` (the license) and `changelog.gz`. lintian's two errors and the prerequisites warning are gone; the `License` field warning is nfpm's own and stays.
		- Pinned by `package.bash` itself: after each build it reads the deb's Depends and the rpm's Requires back and fails unless they carry the floor it derived, libgcc when needed, and the two doc files. Against the 2.0.0 package it fails on the empty Depends.
		- Opened: 20260901-191400
		- Closed: 20260902-143000

	- ✅ Item 16: a system install under umask 077 still leaves the man directory root-only when the installer has to create it.
		- Reproduced with the system paths redirected into a sandbox. The 20260830b fix widens the install root only; `man1` under `/usr/local/share/man` is created by a plain `mkdir` under the caller's umask and is not widened, and a fresh Debian does not have it.
		- Fixed: the lay-down step notes, before each `mkdir -p`, the shallowest directory it will have to create (the install root's, the bin directory's, the man1 directory's) and widens those along with the install root; a directory that already existed is left alone. The step is a function now (`fLayDown`), so it can run on a staged payload.
		- Pinned by `shell-regress.bash`, which runs `fLayDown` under umask 077 into a sandbox where `bin` and `man1` do not exist yet and requires every directory 755. With the widening narrowed back to the install root it reports four root-only directories.
		- Opened: 20260901-191500
		- Closed: 20260902-150000

	- ✅ Item 17: `sign-release.bash` signs before it checks the key, and checks nothing about the sums file.
		- Reproduced with a throwaway key: the run fails on the key check and leaves a valid-looking `.sig` beside the sums file. The tag check compares against `Cargo.toml` but never against the sums file's own name, and the sums are never verified against the files present, so a stale sums file from a rebuilt tree signs clean.
		- Fixed: the signer checks before it writes: the sums file must be named `shcl-<version>-sha256sums.txt` for the `Cargo.toml` version, every entry must match the file beside it (`sha256sum -c --strict`), and the key must be the one all three shipped copies trust. Only then is the signature written, and one that fails its own verify is removed.
		- Pinned by `shell-regress.bash`: a throwaway key against a correct sums file is refused with no `.sig` left, a stale entry and a sums file for another version are each refused before the key is looked at. The old signer failed all five checks.
		- Opened: 20260901-191600
		- Closed: 20260902-153000

	- ✅ Item 18: the installer's "not on your PATH" note fires when the directory is on PATH with a trailing slash.
		- Reproduced. A string compare against `:dir:` misses `dir/`, which the shell resolves fine. Same shape in `install-dev.bash`.
		- Fixed: a `fOnPath` helper in both installers walks PATH by element and ignores a trailing slash on either side.
		- Pinned by `shell-regress.bash`: a PATH element `dir/` is seen, `dir/` asked for is seen, and a prefix or a deeper path is not.
		- Opened: 20260901-191700
		- Closed: 20260902-150000

	- ✅ Item 19: the profiler workload merges half its nodes and prints a hint per unit, so the profile measures hint formatting and stderr writes, and the run log carries a million hint lines.
		- Reproduced: the generator's trailing `service: svcN` line reopens the block above it, so every unit's two `port` leaves collide. 37% of the profiled CPU is the unbuffered hint stream. Today's run log is 93 MB, and a million of its lines are `H001`, from the profiler stage and the large-document fixpoint check.
		- The generator's own header says nothing in it merges, and the config comment says it was introduced to stop measuring exactly this.
		- Fixed: each unit's second `service` line carries its own value (`svcN-b`), so nothing merges and the document loads with no diagnostics; the profiler run and every timed workload close stderr as well as stdout, so the log no longer carries the hint stream.
		- Pinned by the large-document gate, which now requires the generated document to load with exactly zero diagnostics (`ok (0 diagnostic(s))`). The old generator gives 6107 at 2 MiB.
		- Opened: 20260901-191800
		- Closed: 20260902-154500

	- ✅ Item 20: `flame-report.py` accepts a truncated or reshaped profile as a good one, and tracebacks on a non-UTF-8 file.
		- Reproduced: a profile cut off at 35 KB reports attribution summing to 9% at exit 0, and one with the row height changed reports 173% parse, both with the seen marker written. Random bytes give a decode traceback instead of the skip path.
		- Cause: any file with a sample count and one frame passes, and the row step is a hard-coded constant that nothing verifies.
		- Fixed: the report skips at exit 2, with no marker written, unless the file ends in `</svg>`, its rows are evenly spaced, exactly one root frame spans every sample, and the self times sum to the sample count. The row height is read off the rows instead of assumed, and the file is decoded with replacement so bytes that are not UTF-8 skip like any other bad input.
		- Pinned by `shell-regress.bash` with synthetic graphs: a whole one and one at another row height report 60/40 with nothing in `other`; a graph missing the frame between a leaf and the root, one cut off before its closing tag, and random bytes each skip at 2 with no traceback. The old script accepted the first two and tracebacked on the third.
		- Opened: 20260901-191900
		- Closed: 20260902-160000

	- ✅ Item 21: `lint-report.bash` flags the nested pre-push gate's own plan line.
		- Reproduced on today's log. The line it reports is the `-D warnings` in the clippy command the pre-push hook echoes during the publish stage, which is why only runs that push to dev or main show a warning. The cppcheck `--enable=warning` echo is already excluded; this one is not.
		- Fixed: the echoed `-D warnings` is excluded the way the cppcheck `--enable=warning` echo already was. Today's log reads CLEAN.
		- Pinned by `shell-regress.bash`: a log holding both echoed command lines is CLEAN, and one with a clippy and a cppcheck warning appended is FLAG with exactly two lines. The old script counted three.
		- Opened: 20260901-192000
		- Closed: 20260902-161500

	- ✅ Item 22: the C++ veneer's `to_canonical()` never gives the read arena back, so a save loop grows without bound.
		- Measured: 1000 calls on a 2.9 MB document, 94 MB to 2.9 GB. Every other copying wrapper releases before its call; this one was missed.
		- Fixed: `to_canonical()` releases the read arena before its call, like every other copying wrapper.
		- Pinned by `veneer_smoke.cpp`: 200 `to_canonical()` calls on a 30 KB document leave the read arena holding at most two copies. The old veneer holds all 200.
		- Opened: 20260901-192100
		- Closed: 20260902-162000

	- ✅ Item 23: Go's two suppress filters return the caller's slice when nothing is disavowed, against their own comment, and `Diagnostics()` hands out the live internal slice.
		- Reproduced: the returned slice shares its backing array, so an append by the caller and the document's own next append overwrite each other. The in-place-filter bug fixed on 20260831 was the drop path; this is its sibling on the keep-everything path, which the test for it does not cover.
		- Fixed: both suppressors copy on the keep-everything path, and `Diagnostics()` returns a copy of the document's list, so nothing handed out shares a backing array with the document.
		- Pinned by `TestSuppressLeavesTheCallersDiagnosticsAlone`, extended: with a schema that disavows nothing, each suppressor's result and `Diagnostics()` itself must not point at the caller's or the document's array. Fails on the old code.
		- Opened: 20260901-192200
		- Closed: 20260902-163000

- Code review 20260901:

	- A fresh adversarial pass, run from scratch rather than from the previous rounds' notes, and aimed at what the three merges of 20260901 changed plus the ground the 20260830b round recorded as unreached. Six defects here, three enhancements under Features and enhancements. Everything below was reproduced, not read off the code.
	- The four-way check proves the bindings agree, so five of the six are shapes all four share and it cannot see. The sixth is C-only, which it also cannot see.
	- Closing the round: the fuzz that feeds the four-way check builds half its inputs from line-level shapes now (duplicate keys with children, a refused line with content beneath it, bracket arrays, mixed and staircase indent, comments at every depth, stacked elements against fields, a BOM, an open quote), where before it only mutated corpus text character by character. A second property runs a write over that soup and checks the result is still a formatter fixpoint. 20,000 iterations of each pass, and a 500-document dump agrees across the four bindings on 6,402 comparisons.

	- ✅ Item 1: colon-less lines at a constant indent make the parse quadratic, and no cap stops it.
		- Reproduced in all four bindings. 1.1 MB of a plain text file takes 30.9 s in the release reference; time goes up fourfold for every doubling of the input. Go and Python are three to six times slower again, C about a third of the reference.
		- Cause: a line that fails to scan is retained as trivia on the pending list, and `hang_deeper_pending` walks that whole list on every line that reaches it. Nothing ever claims the entries, because only a binding line drains them, so the list grows by one per line and every line rewalks it.
		- The same line count with a binding line between each bad one runs in 0.05 s against 7.92 s, which is what pins the cause to the pending list rather than the stack or the diagnostics.
		- Only equal indent is affected: bad lines at increasing depth hang on their blocks and drain, and finish in 0.04 s.
		- `ParseLimited` gives a consumer no defense here. No nodes and no elements are built, so neither cap ever fires. The one shape a caller cannot bound is the one that costs the most.
		- Reachable by accident, not just by malice: pointing the CLI at a text file that is not SHCL is the whole reproducer.
		- Arrived with the 20260817 round's change to retain malformed lines as trivia. The retention is right; walking the list per line is what costs.
		- Fixed: each pending entry remembers the shortest incoming indent it was already checked against, and a stack of marks records how far the list is settled at each indent. A hang check pops the marks above its own indent and walks only the entries they covered, so an entry is rewalked only when a shallower line arrives, and a shallower line can only arrive as many times as the indent is long. 40k bad lines: 7.6 s to 0.08 s in the reference, and linear in all four bindings, including the alternating-indent and sawtooth shapes that a max-indent shortcut alone would have left quadratic.
		- Pinned by `perf-gate.bash`'s new `badlines` workload, which checks a document of refused lines against the same binding's parse baseline. It fails on the old code in every binding (the reference took 7.8 s against a 0.3 s budget) and passes now. Trivia placement is unchanged: 9,000 fuzzed documents mixing comments, blanks, bad lines, stacked elements and mixed indent format byte-identically to the previous build, and the corpus is unchanged.
		- Opened: 20260901-140000
		- Closed: 20260901-160000

	- ✅ Item 2: the element cap is applied after the array is built, so it bounds nothing.
		- Spec says the caps exist because bounding the bytes read cannot bound what a load allocates, and that only counting what the parse builds can. The inline spelling builds the whole array first and refuses the line afterwards, so the peak is whatever the text asked for.
		- Measured: 9 MB of input as one over-cap array peaks at 256 MB with a cap of 8, against 257 MB with no cap at all. The cap saves 0.3%.
		- In C the memory is not even a peak. 12 MB of input as one refused array leaves 108.5 MB held for the document's lifetime. The same 12 MB as a plain string value that is kept costs 11.7 MB, which is what rules out ordinary parse overhead.
		- The stacked spelling is worse than useless. Its cap is enforced correctly, per element, before the push - but every refused line emits a diagnostic, and the diagnostics cost more than the elements they refuse: 15 MB of input peaks at 510 MB with the cap set and 254 MB with it off. Setting the cap doubles the memory.
		- Trivia is outside both caps too, so a comment-only document amplifies fortyfold with the caps set as tight as they go and neither one firing.
		- The check has to happen while the array is being built, and the per-line diagnostics need collapsing.
		- Fixed, inline spelling: the element count is taken by a scan that builds nothing, and it runs before the quote check as well, since that check splits the value too. A refused 200k-element line now costs about the text in every binding (Rust 1.0x, Go 0.01x, Python 4x, C 1.4x) against 38x to 78x before.
		- Pinned three ways in all four bindings: an allocation bound on that line (under 8x the text; the old code fails at 38x and up), a table of thirteen value spellings whose cap count must equal the count the array reads back as (quoted and escaped commas, empty and blank slots, Unicode blanks, an open quote), and a refused line reporting `E021` alone. Rust's bound lives in its own test binary because the counting allocator is process-wide; C's is a new `mem_bounds.c`, in the compiler sweep and the sanitizer run too.
		- The stacked spelling's cap was already right; its cost is the per-line diagnostics, which item 8's ceiling bounds. Trivia amplification is the same per-line overhead any document has and is not a cap matter; a caller bounds it by bytes with `ReadFile`.
		- Opened: 20260901-140100
		- Closed: 20260901-163000

	- ✅ Item 3: in C, `shcl_paths` grows the document on every call and `shcl_reads_release` cannot give it back.
		- The header states the invariant this breaks. `scratch` is documented as "reset on entry to each resolve, so read-only use of a long-lived document stays flat", and `shcl_reads_release` names `shcl_paths` in the list of calls whose results it gives back, for "a process polling the same document in a loop... so the memory does not climb".
		- Measured on a 180-path document, with `shcl_reads_release` called between every pass: 11.4 KB per call, 557 MB over 50,000 calls. All of it comes back at `shcl_free`, so it is unbounded growth for the document's lifetime rather than a leak past it.
		- Cause: `shcl_paths` puts its walk stack and dedup set in `d->scratch` and never resets it. Every other read resets scratch on entry, at the path lookup - and `shcl_paths` takes no path, so it misses the one reset that would cover it.
		- Every other read entry point is flat: twelve were measured and only this one climbs. One path-based read anywhere in the same loop drops it to 0.4 bytes per call, which is why nothing has caught it.
		- C only, so the four-way check is structurally blind. It matters most to exactly the kind of consumer the C binding exists for.
		- Fixed: `shcl_paths` resets scratch on entry, the way the path lookup does for every other read.
		- Pinned in `mem_bounds.c`: 2,000 released passes of one read call each, judged after a warm-up pass, must not grow the process by more than 4 KB. One call per loop, because a path read in the same loop resets scratch on the next call's behalf and hid this for a round. `shcl_paths` grew 22.8 MB on the old code; `shcl_children`, an array read and `shcl_to_canonical` were flat before and after.
		- Opened: 20260901-140200
		- Closed: 20260901-164500

	- ✅ Item 4: the typed setters write values their own reader refuses, and report success.
		- Spec is explicit that each setter is the exact inverse of the matching read - `SetFloat` emits the number text the reader accepts, `SetDateTime` stores the canonical spelling. Neither holds.
		- `SetFloat` with an infinity or a NaN writes `inf`, `-inf` or `NaN`; reading the field back is `BadType`. The float formatter has no finiteness gate.
		- `SetDateTime` accepts any struct the caller builds. Fourteen out-of-range shapes were tried and all fourteen wrote a value that will not read back - month 99, February 30, a twenty-digit fraction, a fraction with no seconds, an offset of 166 hours. A default-constructed struct, which is what a caller reaches for first, writes an empty string.
		- Every one of them returns true, so `WriteReason` says the write applied.
		- The CLI is inconsistent with itself about this. Its datetime op validates the text through the reader's own parser and refuses a bad one; the float op ten lines away parses with the language's own float reader, which takes `inf` and `nan`, and writes them.
		- All four bindings agree, and the C rendering matches the reference byte for byte on all fourteen, so this is shared logic and not a parity break.
		- Fixed: `SetFloat` and its array form refuse a non-finite value; `SetDateTime` and its array form render the struct and parse it back, and refuse unless the fields come back equal. Both return false and bind nothing, the way `SetRaw` already did for an info-string it cannot spell. The CLI's float ops (`float`, `float-default`, `float-array`, and the default array) refuse `inf`, `nan` and a literal past the double range in all four bindings. Spec: the setter contract names the refusal.
		- Pinned by a runner fixture in all four bindings (three non-finite floats through the scalar, default and array setters; ten datetime shapes through the same three; a valid value still writes and reads back equal; the document is byte-identical after every refusal) and by corpus 029's `write-bad.ops`, which gained `1e400`, `INF`, `nan`, `-inf`, `infinity`, an array holding `inf` and a default of `nan`. Those first three were in `write.ops` with `inf`/`NaN` as the expected output, from the 20260725 round's parity fix; that round's item 3 carries a note. Fails on the old code in all four (Rust 2 tests, Go 2, Python 14 lines, C 29).
		- The C `dt_clamp` fixture still hands the setter its worst-case struct: the render into the fixed buffer happens before the value is judged, so the clamp is still exercised, and the setter is now expected to refuse.
		- Opened: 20260901-140300
		- Closed: 20260901-173000

	- ✅ Item 5: `init` emits a starter config that `check --schema` rejects, with nothing said, when a repeat lower bound is 1.
		- Generation promises its output validates clean against the schema that produced it, with one documented exception: a repeat lower bound of 2 or more, which cannot be auto-satisfied because identical generated lines would merge.
		- The self-check filters out every `V007` instead of only that case, so a lower bound of 1 slips through as well. A lower bound of 1 is a must-exist path, which generation is supposed to satisfy.
		- Reproduced in all four bindings on two schemas that differ in one word. With `required: yes` on `"srv[*].port"`, generation fails loudly with `V097`. With `repeat: 1` on the same path, `init` exits 0, emits `srv: web` and `srv.port:`, and the very next `check --schema` on that file exits 6.
		- The generated pair does not mean what it looks like: `srv: web` and `srv.port:` are two instances of `srv`, one carrying the discriminator and the other carrying the port, so the wildcard finds an instance with no port. That is the outcome the dotted-form rule exists to prevent - a live line materializes the parent, but the dotted line then targets a different instance than the one it created.
		- Six of 700 generated schemas hit it; the other 43 failures in that set are the documented 2-or-more case.
		- Fixed, in two parts. The generator now selects a valued live parent by its value: `srv: web` is followed by `srv[web].port:`, which lands on the instance the first line made, and a concrete child (`srv.host`) gets the same treatment, since `srv.host:` under `srv: web` was two instances as well. A default that would end or escape the selector (`]`, `[`, a backslash) is spelled quoted; the selector matches on the escaped display, so it still finds the bare value. And the self-check lets through only a `V007` whose lower bound is 2 or more, read off the message's `not in LO..HI`, so `repeat: 1` on a path generation cannot satisfy (a nameless `*` path) faults the schema the way `required` already did. Spec: the dotted-form rule names the selector.
		- Of the six schemas, the `srv[*].port` shape now generates a config that passes its own schema; the five with `repeat: 1` on a `*` field fault with `V097`. The 700 schemas produce byte-identical stdout, stderr and exit codes across the four bindings; the 46 remaining self-schema failures are all the sanctioned lower bound of 2 or 3.
		- Found on the way: the C CLI reported a schema that does not build by validating an empty document, which added that document's own `V002`/`V007` to the fault list. `shcl_generate` records its build faults on the schema document now, like the other generation faults, and the CLI prints those.
		- Pinned by corpus `073-init-parent-value` (valued parent with a wildcard child and a concrete child, a quoted default, a `]` in a default, an optional parent, a bare parent; fails on the old code in all four runners, which also check the output against its own schema), and by three `cli-regress` rows: `repeat: 1` on `*` exits 6 with `V097 ... not in 1..1`, `repeat: 2` generates, and a build fault reports `V091` with no `V002` beside it. The last one needed a negative stderr form in the harness.
		- Left alone: an optional parent with a default (`#opt: x`) and a must-exist child still emits `opt.child:`, which materializes an empty `opt`. Uncommenting the parent then makes two instances. The schema says the parent is optional, so the generator does not make it live; a selector form there would.
		- Opened: 20260901-140400
		- Closed: 20260901-181500

	- ✅ Item 6: `set_literal` reads bracket-array text differently from the parser, silently.
		- Spec says `SetLiteral` reads its argument the way the parser reads the half of a line after the colon. For bracket text it does not.
		- `p: [1, 2]` in a file is `E019`, counted as lost content, and `fmt --write` refuses it at exit 7. `--set-literal 'p=[1, 2]'` writes a two-element array holding `[1` and `2]`, with no diagnostic, at exit 0.
		- So the door the `E019` work closed is still open through the writer, and what comes through it is a different wrong answer rather than the same one.
		- All four bindings agree. Refusing the shape, the way `SetLiteral` already refuses a quote that never closes, is the consistent answer.
		- Fixed: `SetLiteral` and `SetLiteralDefault` refuse a value that, after the comment is split off and the ends trimmed, starts with `[` and ends with `]` - the parser's own test for `E019`. The refusal returns false and binds nothing. Spec: the `SetLiteral` sentence names it beside the other two refusals.
		- Pinned by corpus `065-bracket-array`, which gained a `write-bad.ops` (four bracket spellings, one through the default form, one with a trailing comment) and a `write.ops` for the two neighbours that stay accepted: a quoted `"[80, 443]"`, which is a string, and an unclosed `[80, 443`, which the parser also reads as two elements. Fails on the old code in all four runners.
		- Opened: 20260901-140500
		- Closed: 20260901-183000

- Code review 20260830b:

	- A third directive pass, run the same day as the 20260830 round and after it merged. Nine parallel audits over the four bindings, the pipeline, the installers, the docs and the backlog. Items 1 to 17 are here and all closed; 18 to 57 are still open under Features and enhancements. The two prior rounds this week were exhaustive on the code, so most of what is left sits in the writer's fixpoint guarantee, the C read tier, and the documents.
	- Items 5 to 17 were worked as a bugs-only pass: every fix left a test that fails without it and passes with it, run both ways per binding, and three of them needed the pipeline extended before the defect was reachable at all.

	- ✅ Item 1: a written duplicate folds one level but not the next, so `set` output is not a `fmt` fixpoint.
		- Reproduced: file `b: 1, 2` over a block `b:` with `a: 2` under it, ops `int b.a 2` then `empty b`. The write emits `a: 2` twice; `fmt` on that output collapses it back to one.
		- Cause: the fold moves the loser's children onto the survivor and stops. The parser's own late-duplicate fold is depth-first for exactly this reason; the writer's path is not.
		- No value is lost, since a reload merges the pair. What breaks is the promise that a write leaves a canonical file, so a "fmt changes nothing" gate fails right after a legitimate edit.
		- All four bindings, and the cross-binding check cannot see it. Fold the moved children after the merge, the shape the parser already uses.
		- Fixed: the writer folds depth-first now, the shape the parser already used. Only a node that just received children is rechecked, so the cost stays with the fold.
		- Pinned by corpus case `062-write-fold-deep`, which fails on all four bindings without the change.
		- Opened: 20260830-140346
		- Closed: 20260830-145320

	- ✅ Item 2: a bracket array is mis-diagnosed, read wrong, then baked into a string by `fmt --write`.
		- Reproduced: `ports: [80, 443]` reports `E015 missing colon; repaired as an empty value`. The line has a colon.
		- `get --array` returns one slot holding `80, 443` at exit 0, so a script gets a wrong answer and no signal.
		- `fmt --write` rewrites the line to `ports: "80, 443"`. The quoted-plain-string rule then makes that the authored spelling, so the file reports clean forever and the one warning is gone. E015 is a repair, not a loss, so the save gate does not fire.
		- Bracket syntax is what most authors arrive with, from JSON, TOML and YAML. A pasted YAML list gets the same treatment: `- red` reports `unexpected 'r' after field`.
		- Cheapest fix is to name the shape in the diagnostic prose, which is per-binding voice and outside the differential contract, so no golden moves. A dedicated code is stronger and costs four bindings plus a corpus case.
		- Decided: both, and now rather than later. A new code is additive today and expensive to add once there are consumers keying on the old one.
		- Fixed: new code `E019` names the shape, and the load counts it as lost content, so `fmt --write` refuses at exit 7 instead of baking `"80, 443"` into the file. Spec table and changelog carry it.
		- The wrong-answer-at-exit-0 half is closed by item 18: once the read subcommands print load diagnostics, a `get` on this file says so too.
		- A pasted YAML list is untouched and stays `E014`. Its text is kept verbatim as trivia, so nothing is lost and the save gate has nothing to refuse.
		- Pinned by corpus case `065-bracket-array`, which reports `E015` on all four bindings without the change.
		- Opened: 20260830-140346
		- Closed: 20260830-171500

	- ✅ Item 3: array reads grow the document arena on every call, without bound.
		- Measured: 200k reads of one three-element array add 15.7 MB. The same loop over a scalar field adds nothing. Confirmed against `read_int_array`; the other four array reads allocate the same way.
		- Cause: the result array and its status array come out of the document arena, which is only freed at `shcl_free`.
		- This is the defect already fixed twice in this file for the scalar path, and both fixes carry a comment saying so. The array family never got the same treatment.
		- C only. The other three return owned collections the runtime reclaims, so the cross-binding check is blind to it. A long-running consumer polling an array field grows steadily. The veneer copies into a vector and never looks at the arena memory again, so there it is pure waste.
		- Fix is a decision, not mechanical: a third arena reset per read call changes the documented lifetime of what a read hands back.
		- Decided: keep the lifetime, and give consumers a way out rather than leaving them to work around it. Read results moved to their own arena inside the document, and a new `shcl_reads_release` gives that arena back without touching the document. A caller that never calls it sees exactly the old contract.
		- The same change covers item 35's four accessors, and `shcl_to_canonical` with them, so the whole read surface follows one rule: anything handed to the caller lives in the read arena.
		- The C++ veneer calls it on every read. It copies each result into owned std types the moment it gets it, so the arena behind it was pure waste, and C++ now reclaims like the other three.
		- Pinned by two fixtures: the C runner asserts array reads do not touch the document arena and that a release-per-read loop stays flat, and the veneer smoke asserts a 20k-read loop stays flat. Both fail with the change backed out.
		- Opened: 20260830-140346
		- Closed: 20260830-181500

	- ✅ Item 4: `init` emits a starter config that fails the schema that produced it.
		- Reproduced: a field typed `int` with `min: 1`, `max: 10` and `default: 99` generates `# int, 1-10, required` and then `server.port: 99` on the next line. `check --schema` against the same schema fails with `V006`, exit 6.
		- The generated comment names the range the generated value breaks.
		- Nothing checks the default against the same field's own constraints; it is written straight out.
		- The doc comment promises the output "always loads clean and validates clean against its schema", and that promise is copied into all four bindings and the man page. A starter config that fails its own schema is the worst first impression the tool can make.
		- Either fault the schema at build time with a new V09x, or comment the offending line out and say why. The doc comment has to match whichever is chosen.
		- Decided: fault the schema. A default outside its own field's constraints is an error in the schema, and saying so is more use to the author than quietly commenting the field out.
		- Fixed with a new `V097`. Generation now checks its finished text against the schema that produced it rather than trusting each branch, so the doc comment's promise is enforced instead of assumed, and any future defect of the same class is caught with it. `V007` stays exempt, being the documented repeat-lower-bound shortfall.
		- The C CLI could not report it at first: on a generation fault it rebuilt the fault list by validating an empty document, which cannot reproduce a fault the generator found. It now prints the diagnostics the generator recorded, and keeps the empty-document trick for build faults that leave none.
		- Pinned by the `init-bad-default` row in `cli-regress.bash`, across all four bindings.
		- Opened: 20260830-140346
		- Closed: 20260830-191500

	- ✅ Item 5: `set_raw` accepts a CR that the load then strips, so content does not survive a round trip.
		- Reproduced: `set_raw` with body `a\r\nb` reads back as `a\nb`; a body of one CR reads back empty.
		- Cause: the call refuses CR and LF in the info string but does not check the body. The load strips the whole trailing CR run per line, and a raw block is the one place content is not trimmed.
		- Two things break at once: silent loss between what a consumer wrote and what it reads back, and the writer's output stops being a fixpoint. A CR mid-line is fine and still round-trips, so only end-of-line CR is affected.
		- All four bindings. Refuse a body whose lines end in CR, the way the info string is already refused, and add a runner fixture beside the existing one.
		- Fixed: a body line ending in CR is refused, the way the info string already was. A CR mid-line is content and still round-trips.
		- Pinned by the set_raw fixture in all four runners, which fails on every binding without the change.
		- Opened: 20260830-140346
		- Closed: 20260830-215127

	- ✅ Item 6: `set_comment` does not trim its text, and the load does.
		- Reproduced: writing comment text `x ` stores `# x `, which reparses as `# x`. Writer output is not a fixpoint.
		- Empty text stores `# `, a single space stores `#  `. Non-ASCII trailing whitespace does the same, because the parser trims the full whitespace set and the setter trims nothing.
		- All four bindings. Trim with the parser's own trim before the prefix, and drop the write when nothing is left.
		- Fixed: the built comment line is trimmed with the parser's own trim, so what is written is what reads back. Text that is blank leaves a bare `#`, which is a valid comment line and a fixpoint, rather than dropping the write.
		- Pinned by corpus case `066-comment-trim`, covering a trailing space, a trailing non-ASCII space and empty text. The corpus already asserts that writer output is a fmt fixpoint, so the case fails on all four bindings without the change.
		- Opened: 20260830-140346
		- Closed: 20260830-215127

	- ✅ Item 7: the loose int range check is off by one at the top end.
		- Reproduced: `9223372036854775808.0` read as int at loose strictness returns `9223372036854775807`. The plain decimal spelling of the same number correctly refuses. Two spellings of one value disagree.
		- Cause: the bound is compared against `i64::MAX as f64`, which rounds up to 2^63, so the value passes and the cast then saturates.
		- Rust saturates, so here it is only a wrong answer. The same shape in C is an out-of-range float-to-int conversion, which is undefined behavior, and this is the file the other three mirror.
		- The low end is already exact. Compare against the exact bound.
		- Fixed: the top bound is 2^63 itself and the compare is strict, in all four. The low end was already exact and is unchanged.
		- Behavior change: `9223372036854775807.0` now reads BadType at loose. There is no double that holds i64 max, so the float path cannot honestly produce it; the plain decimal spelling still reads exactly.
		- Pinned by corpus case `067-i64-float-bound`.
		- Opened: 20260830-140346
		- Closed: 20260830-215127

	- ✅ Item 8: the Windows setup build fails outright on any prerelease version.
		- The installer script writes a four-integer version field straight from the package version. A version like `2.1.0-alpha.1` produces something the tool rejects, and it exits nonzero.
		- The packaging script runs under `errexit`, so that failure kills the whole release stage. A prerelease cut cannot get past it.
		- Never exercised: the version field went in after the last prerelease, so no prerelease has been cut since. The installers still offer a dev channel, so this is live workflow.
		- Fix: reuse the digit-splitting the Rust build script already does for the same field, pass the quad separately, and leave the display strings on the full version.
		- Fixed: the packaging script derives the four-integer field the way the Rust build script does and passes it separately, so the display strings keep the full version. A release version gives the same quad as before.
		- Pinned by a row in `shell-regress.bash` that packages `2.1.0-alpha.1` for real and requires a setup out the other end.
		- Opened: 20260830-140346
		- Closed: 20260830-215127

	- ✅ Item 9: the installers' stable channel is not version-sorted, so a back-ported patch outranks a newer release.
		- Both scripts point stable at the "latest release" endpoint and take its tag verbatim. That endpoint is newest by publication date, not by version.
		- Cut a `1.2.1` fix after `2.0.0` and every stable install is handed `1.2.1`.
		- The comment directly above the code states this exact hazard as the reason the dev channel sorts, then leaves the stable channel on the date-ordered endpoint. Both scripts and the README promise "newest full release", which date order does not give.
		- Fix: list releases for both channels, drop prereleases and drafts for stable, and run the comparator that already exists.
		- Fixed: both channels list releases and take the highest version through the comparator that was already there. Stable drops pre-releases; a draft is dropped on either channel, since it has no published assets to install.
		- Pinned by four rows in `shell-regress.bash` against a fixture in publish order, where the date-ordered answer and the version-ordered answer differ. The rows run the shipped selection, lifted out of each installer by name.
		- Opened: 20260830-140346
		- Closed: 20260830-215127

	- ✅ Item 10: the PowerShell wrapper calls a .NET 6 method on the PowerShell 5.1 it claims to support.
		- The symlink resolver calls a method that arrived in .NET 6. Windows PowerShell 5.1 runs on .NET Framework and does not have it. The header says the file runs on 5.1.
		- The helper runs unconditionally at load, so every dot-source and every run hits it. The error suppression on the line above covers the lookup, not the method call.
		- Confirmed failure shape on 7.6.5 against a same-named missing method: at default preferences it writes a red error and continues, under strict mode it halts, under stop-on-error it exits 1. All three are inherited from whatever sources the file.
		- Fix: test for the member before calling it and fall through to the unresolved-path branch already there.
		- Fixed: the member is tested for before it is called, and its absence falls through to the unresolved-path branch that was already there.
		- Pinned by a row in `shell-regress.bash` that hands the resolver an item with no such method, which is the 5.1 shape, plus a row that resolves a real symlink so the working path stays covered.
		- Opened: 20260830-140346
		- Closed: 20260830-215127

	- ✅ Item 11: the same wrapper reads a PowerShell 6+ variable before the test meant to guard it.
		- The platform test puts the variable first and the null check second, so on 5.1 the first operand is evaluated and throws under a caller's strict mode.
		- The installer guards the identical read with a version test that short-circuits first. The wrapper never got that treatment, though the comment above it shows the 5.1 case was considered.
		- Fix: put the version test first.
		- Fixed: the version test comes first and short-circuits, matching the installer.
		- Pinned by a scan in `shell-regress.bash`: every `$IsWindows` read in the wrapper has to sit behind that test. It cannot be exercised from a 7.x session, because `$PSVersionTable` is read-only and cannot be shadowed even locally.
		- Opened: 20260830-140346
		- Closed: 20260830-215127

	- ✅ Item 12: a system install under a non-default umask lands mode 0700 and only root can run it.
		- Reproduced under `umask 077`: the install directory, the payload directory and the binary all came out mode 0700. The same lines write the system paths, leaving the launcher resolvable only by root, and the man page and completions unreadable.
		- Cause: the script sets no umask, and `sudo` keeps the caller's unless sudoers overrides it.
		- The packages set explicit modes, so the two install routes disagree on the same box.
		- Fix: set a umask before the install block, or set the modes explicitly.
		- Fixed: a system install widens the modes of what it just wrote, which also repairs a tree an earlier run left too tight. A user install still follows the caller's umask; it is one user's copy.
		- Pinned by a row in `shell-regress.bash` that stages a tree under `umask 077` the way the installer stages one, then requires 755 directories and 644 data.
		- Opened: 20260830-140346
		- Closed: 20260830-215127

	- ✅ Item 13: the README's C example does not build as written.
		- The snippet calls into the standard library but shows no system includes, so a reader adds them, and the natural place is above the library header.
		- That order fails: the library asks for a POSIX level, and a feature request only counts before the first system header. Reproduced with the README's own compile line: five implicit declarations and a pointer-from-integer error.
		- With the library header first, the same example compiles clean and produces a file matching the other three examples.
		- The constraint is written in the header, but nowhere a README reader looks.
		- Fix: show the system includes below the library header, or say the header goes first.
		- Fixed: the example shows its system includes below the library header, with the reason, and the prose says the same.
		- Harness extended: `check-readme-c.bash` lifts the example out of the README, wraps it in a main, and builds it with the compile line the README gives. It also refuses an example carrying no system include, since without one the order proves nothing.
		- Opened: 20260830-140346
		- Closed: 20260830-215127

	- ✅ Item 14: the style guide describes a name index the code has not had for two rounds.
		- It says the index is built during the parse, keyed on name and value, and discarded afterward, with the writer mutating the tree directly instead of maintaining it.
		- The code says the opposite three ways: built on the first path lookup, keyed on parent and name, and kept current by the writer, with only a merge dropping it.
		- Nothing else repeats the stale claim, and the changelog has it right.
		- Fixed: the entry now describes both indexes. The parser's `(name, value)` child index goes when the parse ends; a second index keyed on `(parent, name)` is built at the first path lookup and kept current by the writer, and only a merge drops it.
		- Harness extended: `perf-gate.bash` gained a bulk-read workload, so the surviving index has a number to fail on. Without it the read path is a sibling scan per key and goes quadratic.
		- Opened: 20260830-140346
		- Closed: 20260830-215127

	- ✅ Item 15: a double negative in the guidelines license footer reverses its meaning.
		- It reads "None of this is not legal advice". The same disclaimer in the trademark document is written correctly.
		- That document is CC BY 4.0 and invites verbatim reuse, so the error travels into anyone's copy.
		- Fixed: the sentence says what it means.
		- Harness extended: `check-docs.bash` flags a second negation in the same sentence as "legal advice", which is the shape of the error. The backlog is excluded, since it quotes the broken wording as a finding.
		- Opened: 20260830-140346
		- Closed: 20260830-215127

	- ✅ Item 16: the spec says an info string is never interpreted, and on the same-line fence it is.
		- Spelled on the same line as the field, an info string of `html # note` reads back as `html`, with the rest moved onto the field line. Spelled under a child indent, the same text stays whole and is a fixpoint.
		- The grammar has the same gap: the same-line alternative allows no comment, and the info-string rule admits a `#`.
		- The emit side of this is documented. The parse side, which is where the two spellings diverge, is not.
		- Decided: the code was wrong, not the documents. The grammar gives the same-line alternative no comment at all, the spec says an info string is never interpreted, and the emitter already assumes text after a fence is label material. Nothing else in the language has two spellings that mean different things.
		- Fixed: a same-line fence takes the rest of the line as its info string, in all four. `db: ```c#` now labels the block `c#` instead of silently dropping the `#`.
		- Behavior change: a trailing comment on a same-line fence is no longer a comment. It goes on the line above.
		- Note: `set_raw` still refuses an unquoted `#` in an info string. That refusal is now conservative rather than necessary, since both spellings round-trip one. Relaxing it is a separate change and is not a bug.
		- Pinned by corpus case `068-info-hash-spellings`, which reads the same label through both spellings.
		- Opened: 20260830-140346
		- Closed: 20260830-215127

	- ✅ Item 17: both Done sections break the two-run layout the conventions now promise.
		- The conventions say loose items and code-review rounds form two runs per Done section, each newest first. Both sections run rounds, then loose items, then rounds again.
		- The two newest rounds are the ones stranded above the loose run.
		- The 20260830 round filed exactly this and closed it, but only the conventions sentence was added and the file was never reordered, so the stated convention is now false.
		- Canceled puts its loose item first and rounds after, so one order has to be picked and written down.
		- Decided: loose items first, rounds after, each run newest first. That is the order Canceled already had, and it moved two bullets instead of fourteen.
		- Fixed: both Done sections reordered, and the conventions sentence now names the order rather than describing two runs without saying which comes first.
		- Harness extended: `check-docs.bash` reads the order back out of the file, and also catches rounds that fall out of date order.
		- Opened: 20260830-140346
		- Closed: 20260830-215127

- Code review 20260830:

	- A second pass over the same directives one day after the last round, aimed first at the code that round changed and then at the whole repo. Items 1 to 23 are here; 24 to 52 are under Done - Features and enhancements. Most of the defects are shared by all four bindings, which is the class the cross-binding check cannot see.

	- ✅ Item 1: `remove` leaves the name index stale, so a removed node keeps answering reads.
		- Reproduced: `a: 1` / `b: 2`, then `remove a`: `exists("a")` is still true and `read_int("a")` still gives 1. A `set_int_default("a", 3)` after the remove writes nothing.
		- Cause: the index is dropped before the resolve that finds the targets, that resolve rebuilds it, and the removal then only unlinks the node from its parent.
		- All four bindings. Drop the index after the removal, and add a remove-then-read fixture to every runner.
		- Opened: 20260830-093632
		- Closed: 20260830-124432

	- ✅ Item 2: an in-place write drops the setuid and setgid bits, which the spec says it keeps.
		- Reproduced: `chmod 6750` a file, `fmt --write` it, the bits are gone. All four bindings.
		- Cause: the mode is applied to the temp file before the data is written, and the kernel clears those bits on the write.
		- Apply the mode after the data and the fsync, or take the promise out of the spec.
		- Opened: 20260830-093632
		- Closed: 20260830-124432

	- ✅ Item 3: the duplicate-fold on write hashes every sibling before comparing names, and bulk writes got 4.5x slower.
		- Measured: 40k writes into a flat 40k-key document went from 13.5 s to 61 s with the 20260829 round. Comparing the name first brings it back.
		- All four bindings.
		- Closed by item 4's fix; flat 40k writes run in tenths of a second now.
		- Opened: 20260830-093632
		- Closed: 20260830-124432

	- ✅ Item 4: every `set_*_default` on a path that does not exist rebuilds the whole name index and throws it away.
		- Measured: 1000 absent defaults on a 40k-node document went 140x slower.
		- Cause: the setter checks existence through the index, and the write that follows drops it.
		- The writer itself is still O(siblings) per op. Either check existence with the writer's own probe walk, or keep the index alive across writes and use it in the writer too.
		- All four bindings.
		- Fix (b): the index lives across writes and the writer uses it. Covers item 3 too.
		- Opened: 20260830-093632
		- Closed: 20260830-124432

	- ✅ Item 5: the pre-push hook links the checkout's cargo target dir into its worktree, and the next `cargo test` in the checkout fails.
		- Reproduced: after a push, 8 of 30 conformance tests fail with the corpus dir under the hook's temp path, because the test build baked that path in and cargo thinks it is fresh. The fuzz test does not fail; it silently drops its corpus seeds and fuzzes three strings.
		- Give the gate its own target dir, and make the seed loader fail loudly when the corpus dir is missing.
		- The gate builds in its own target-gate dir; the checkout's target is never touched.
		- Opened: 20260830-093632
		- Closed: 20260830-124432

	- ✅ Item 6: the pre-push hook gates only the last protected ref in a multi-ref push.
		- `git push origin dev main` tests main's commit and never dev's.
		- Collect every ref and gate each distinct commit.
		- Opened: 20260830-093632
		- Closed: 20260830-124432

	- ✅ Item 7: `install.bash` exits with no message on three lookups that can come up empty.
		- A `grep` inside a command substitution fails under `pipefail`, and the script dies before the check that would have printed the reason: no dev release, a sums file without the drop-ins entry, or the asset name missing from the sums.
		- The documented binary-only fallback for a release with no drop-ins can never run. Every stable release before 2.0.0 would have hit it.
		- Opened: 20260830-093632
		- Closed: 20260830-124432

	- ✅ Item 8: `install.ps1` under the `irm | iex` one-liner leaves strict mode, `$ErrorActionPreference = 'Stop'` and its functions in the caller's shell.
		- Before 20260829 item 16 the script ended the shell, which hid this. Now the shell survives with the changed state.
		- The scriptblock form runs in a child scope and is clean. Make it the documented one-liner, or wrap the script body in a scriptblock.
		- The body runs in a scriptblock scope now, so iex leaves nothing behind.
		- Opened: 20260830-093632
		- Closed: 20260830-124432

	- ✅ Item 9: `set_raw` accepts an info string with an unquoted `#`, and the same-line spelling loses it on reload with no diagnostic.
		- Reproduced: an empty `k:` followed by a raw `k[#1]` with info `a # b` saves as `k: ```a # b`; reading it back gives info `a` and `check` says ok.
		- Only reachable when an empty same-named sibling precedes the raw node. Refuse it in `set_raw`, as the line-break check does. All four bindings.
		- Opened: 20260830-093632
		- Closed: 20260830-124432

	- ✅ Item 10: an ops script whose last line ends in a bare carriage return is read differently by the reference and the three ports.
		- The reference keeps the CR (so `int a 1<CR>` is a bad int), the ports strip it. The crosscheck never feeds one.
		- Spec decision: strip one trailing CR per line everywhere, or keep the reference's rule and make the ports match. Then add a fixture.
		- Decided: an ops line loses one trailing carriage return in all four CLIs; a lone one becomes a blank line and is skipped.
		- Opened: 20260830-093632
		- Closed: 20260830-124432

	- ✅ Item 11: Python's `set_float` accepts an int and writes it digit for digit, so a value above 2^53 becomes text the reference cannot produce.
		- `set_float("x", 9007199254740993)` writes exactly that; the reference writes `9007199254740992`. A huge int writes hundreds of digits where the reference writes `inf`.
		- Convert to float first, and decide what an int too large for a float should do.
		- Opened: 20260830-093632
		- Closed: 20260830-124432

	- ✅ Item 12: the Python CLI writes stdout in the locale encoding, so a Windows pipe or file raises `UnicodeEncodeError` on non-ASCII content.
		- The newline fix from 20260829 item 11 set the newline only. The reference writes bytes. Set the encoding to UTF-8 in the same call.
		- Opened: 20260830-093632
		- Closed: 20260830-124432

	- ✅ Item 13: C passes a null pointer with length zero to `memchr` on the new fast paths.
		- Reached from `shcl_set_raw` and `shcl_set_literal` with a `(NULL, 0)` span, which is the usual C spelling of "no text". UBSan flags it; glibc declares the argument nonnull.
		- Six sites; guard on the length first.
		- Opened: 20260830-093632
		- Closed: 20260830-124432

	- ✅ Item 14: the Python CLI crashes with a traceback on a closed stdin or stdout, and the C CLI's exit code differs from the reference on a closed stdin.
		- Rust and Go print nothing and exit 0. Python raises on `sys.stdin.buffer`. C prints `read error` and exits 1.
		- Opened: 20260830-093632
		- Closed: 20260830-124432

	- ✅ Item 15: two `-` inputs on one command line read stdin once, and the second one gets an empty document that looks like a real answer.
		- `check --schema=- -` validates the schema against nothing; `get --layer=- -` reads the layer as the base. `set` already refuses this; the other commands do not. All four.
		- Opened: 20260830-093632
		- Closed: 20260830-124432

	- ✅ Item 16: the C CLI's op-script errors drop the `op line N:` prefix and the offending op.
		- The other three print `op line 2: unknown op: bogus`; C prints `unknown op`. Same for every op error.
		- Opened: 20260830-093632
		- Closed: 20260830-124432

	- ✅ Item 17: the C CLI's strict-load failure prints a bare count where the spec and the other three name the diagnostics.
		- Opened: 20260830-093632
		- Closed: 20260830-124432

	- ✅ Item 18: the C CLI loses the reason on a read failure and on a temp-file failure.
		- A directory gives `dir: read error` (the reference names the error); a missing parent dir loses the "cannot create temporary file" context.
		- Opened: 20260830-093632
		- Closed: 20260830-124432

	- ✅ Item 19: a writer-created top-level node with a comment emits the section blank line between the comment and the node.
		- `int x 5` then `comment x note` emits the comment, a blank line, then `x: 5`, so the comment reads as belonging to the field above. It is a fmt fixpoint, so it never self-corrects. All four.
		- Opened: 20260830-093632
		- Closed: 20260830-124432

	- ✅ Item 20: a blank answer at the pipeline's commit-message prompt never opens the editor; the publisher makes up `shcl <stamp>`.
		- The help, the prompt and a comment all say "blank = editor". The publisher is always passed `--quiet`, which is what makes it auto-generate. This is the half of 20260829 item 33 that was not done.
		- Separate "no continue prompt" from "auto-message" in the publisher, or fix the three strings.
		- New publisher --no-prompt flag; cicd passes it unless run with -q, so a blank message reaches the editor.
		- Opened: 20260830-093632
		- Closed: 20260830-124432

	- ✅ Item 21: the bash wrapper accepts a directory as `SHCL_BIN` and fails with a shell error.
		- `-x` is true for a directory. Test `-f` too. The PowerShell wrapper already refuses it.
		- Opened: 20260830-093632
		- Closed: 20260830-124432

	- ✅ Item 22: `design.md` still records the raw-block indent rule that the 20260829 round replaced.
		- The Formatter section says the stripped indent comes from the non-blank body lines and that a whitespace-only body is left alone. The spec and the corpus now say the closing fence's own indent, with no special case.
		- Also stale: the name-escape decision is dated "the coming major" (2.0.0 has been out since 2026-08-27), and the positioning line still quotes the "friendliest API" boast the README dropped.
		- Opened: 20260830-093632
		- Closed: 20260830-124432

	- ✅ Item 23: `n8runshcl.ps1`'s advice to put `--` first fails under `pwsh -File`.
		- It works from inside a pwsh session, which is the only place it is needed. Reword or drop the sentence.
		- Opened: 20260830-093632
		- Closed: 20260830-124432

- Code review 20260829:

	- Standards pass over the whole repo (code style, performance, the pipeline, docs, the front page, the installers) plus an adversarial read of all four bindings and the installers. Unlike the 20260819 round this one did turn up correctness defects, most of them shared by all four bindings, which is exactly the class the cross-binding check cannot see. Items 1 to 25 are here; 26 to 68 are under Done - Features and enhancements.

	- ✅ Item 1: a skipped binding line drops out of the indent stack, so its children re-parent and its next sibling is lost.
		- Reproduced: `a:` / `\tb[#5]: x` / `\t\tc: 1` / `\td[9]: q` / `\t\te: 2`. Line 2 is skipped (E014), then `c: 1` and `e: 2` attach to `a`, and line 4 gets E012 instead of the E003 it should get.
		- Cause: only a successful bind pushes its indent level. A skipped line leaves nothing for its children to hang from.
		- Shared by all four bindings, so the crosscheck agrees on the wrong answer. Needs a corpus case with the fix.
		- Fixed: a skipped line now holds its indent level, so what was written under it is skipped with it (`E018`, new) and the next sibling binds where it should. Corpus case 057.
		- Case 057 was widened later to a whole subtree: a grandchild under a skipped line, under both the dropped and the retained-as-trivia spelling, plus `children` and `paths` rows so enumeration cannot hand back a name a read then refuses. Backing the skip out re-parents the grandchild with its parent, which the earlier one-level rows did not see.
		- Opened: 20260829-071126
		- Closed: 20260829-093755

	- ✅ Item 2: `set_raw` loses a body's shared indent on the next reload.
		- Reproduced: set a raw body of `  a` / `  b`, save, load, read it back: `a` / `b`. Interior relative indent survives; a leading indent every line shares does not.
		- Cause: emit adds the block indent, load strips the common indent of all body lines rather than just the fence's own indent.
		- An indented SQL or YAML snippet is the headline raw-block use, so this is not a corner. All four bindings agree.
		- Fix is a spec decision: strip only up to the fence line's indent (the CommonMark rule), or have `set_raw` refuse a body it cannot spell.
		- Fixed: by a spec change: the nesting stripped from a raw body is the closing fence's own indent (the opening line's when the block never closes), so a shared body indent is content and survives a reload. The emitter no longer special-cases an all-blank body. Cases 053 and 055 updated, 058 added.
		- Opened: 20260829-071126
		- Closed: 20260829-093755

	- ✅ Item 3: `set_raw` writes the info string unchecked.
		- A line break in the info splits the block on reload with no diagnostic; leading or trailing whitespace and a trailing CR vanish.
		- `set_literal` already refuses a line break; `set_raw` should do the same for the info, and trim it so write and read agree. All four bindings.
		- Fixed: the info-string is trimmed the way a fence line reads it, and one holding a line break fails the write. All four bindings, fixture in every runner.
		- Opened: 20260829-071126
		- Closed: 20260829-093755

	- ✅ Item 4: `read_file` with the largest possible cap reads nothing.
		- The over-cap check adds one to the cap. At the type's maximum that overflows: the reference panics in a debug build and reads zero bytes in release, Go returns an empty document reported Clean.
		- A consumer who spells "no cap" as the type maximum gets an empty document and no status. Saturate, or treat the maximum as no cap. Check Python and C too.
		- Fixed: the probe saturates at the type maximum (Python reads in pieces instead of preallocating the cap; C already saturated). Fixture in every runner.
		- Opened: 20260829-071126
		- Closed: 20260829-093755

	- ✅ Item 5: the `bool` write op accepts any text and writes `false`.
		- `bool\tp\tyes` writes `p: false` at exit 0 in every CLI. The int, float and datetime ops reject bad text; bool compares against the single word `true`.
		- Accept the standard boolean token set and reject the rest; add a `write-bad.ops` row.
		- Fixed: `bool` ops take exactly `true` or `false`; anything else is `bad bool` at exit 1. All four CLIs and runners.
		- Opened: 20260829-071126
		- Closed: 20260829-093755

	- ✅ Item 6: `--set` splits at the first `=`, so a selector holding one cannot be addressed.
		- `--set 'x[a=b].c=1'` fails with `cannot write x[a`. Values holding `=` are fine.
		- Split at the first `=` outside quotes and brackets, and say so in the help text.
		- Fixed: PATH ends at the first `=` outside quotes and brackets. The help text and the man page say so.
		- Opened: 20260829-071126
		- Closed: 20260829-093755

	- ✅ Item 7: a dangling symlink is replaced by a regular file, except in Python, which writes through it.
		- Rust, Go and C fall back to the given path when the link cannot be resolved, so the link becomes a file. Python resolves the link anyway and creates the target.
		- Two defects: the spec promises a link is written through, and the four bindings do not leave the same file tree behind.
		- Fixed: Rust, Go and C follow a dangling link by hand and create the file where it points. The spec says so and the crosscheck has a fixture.
		- Opened: 20260829-071126
		- Closed: 20260829-093755

	- ✅ Item 8: the save contract differs by platform and by binding around read-only files, mode bits and ownership.
		- On Windows a read-only target cannot be rewritten (the reference copies the target's mode onto the temp file before publishing, and ReplaceFile refuses a read-only destination). On POSIX the same file is rewritten at exit 0.
		- The reference likely leaves its read-only temp file behind on Windows, because the delete does not clear the attribute first. Go's does.
		- The reference copies the full mode (setuid, setgid, sticky included); Go copies the permission bits only.
		- The spec's list of what a save cannot preserve names hard links, ACLs, xattrs and labels, but not ownership, sticky directories or read-only files.
		- Pick one rule for each, document it, and pin the Windows case in the hosted windows job.
		- Fixed: decided and written down. The whole mode is copied, setuid, setgid and sticky included. A read-only file is rewritten on every platform; Windows clears the attribute for the publish and sets it back after, publish failure included. The spec lists ownership, sticky directories and read-only files with the rest. Windows fixture in every runner, run by the hosted windows job.
		- Opened: 20260829-071126
		- Closed: 20260829-093755

	- ✅ Item 9: Python `save_file` on a document holding a lone surrogate raises and leaves the temp file behind.
		- The write raises a `UnicodeEncodeError`, which is a `ValueError`, and the cleanup only catches `OSError`. Same as the fix applied elsewhere in the file tier last round; this site was missed.
		- Fixed: the write catches ValueError alongside OSError and removes the temp.
		- Opened: 20260829-071126
		- Closed: 20260829-093755

	- ✅ Item 10: Python still recurses one frame per level in three places.
		- Validate through mounts, generate through mount expansion, and resolve through wildcard segments each need about one frame per level for a 512-deep document. From a caller already deep in its own stack they raise `RecursionError`.
		- The 20260817 close-out said validate and resolve survive from a deep caller; that holds for the plain forms only.
		- Fixed: the three walks, plus two more of the same kind found on the way (`_v_contexts`, `_chain_parts_legal`), use explicit stacks. The fixture runs each from a deep caller.
		- Opened: 20260829-071126
		- Closed: 20260829-093755

	- ✅ Item 11: on Windows the C and Python CLIs write CRLF, so their stdout is not byte-identical to Rust and Go.
		- Neither switches stdout or stdin to binary mode. Invisible today because the windows job only runs the test runners, never the CLIs.
		- Fixed: both CLIs put their streams in binary or newline-free mode on Windows.
		- Opened: 20260829-071126
		- Closed: 20260829-093755

	- ✅ Item 12: C `shcl_write_file_atomic` passes a null buffer to `fwrite` when given no data.
		- Undefined behavior on the public call; unreachable through `shcl_save_file`. Skip the write when the length is zero.
		- Fixed: the write is skipped at length zero.
		- Opened: 20260829-071126
		- Closed: 20260829-093755

	- ✅ Item 13: the C++ veneer leaks two C resources on throw, and a default-constructed `Document` is a null handle.
		- `validate()` and `read_file()` free the C buffer after code that can throw. Wrap both in `unique_ptr` with the C free as deleter.
		- Every accessor on a default-constructed `Document` hands a null pointer to the C core. Delete the constructor or back it with an empty document.
		- Fixed: the two C buffers are held in `unique_ptr`; a default-constructed Document is an empty document.
		- Opened: 20260829-071126
		- Closed: 20260829-093755

	- ✅ Item 14: Python typed setters accept the wrong type.
		- `set_int("a", 3.5)` returns True and writes `a: 3.5`, which every binding then reads back as BadType. `set_float("b", True)` raises an unrelated decimal error.
		- Gate on `isinstance` and return False, or raise `TypeError`.
		- Fixed: every typed setter checks its argument and raises TypeError naming the setter and the type it got; int is not bool, float takes int.
		- Opened: 20260829-071126
		- Closed: 20260829-093755

	- ✅ Item 15: Python `read_file` and `load_file` take an int as a file descriptor and close it.
		- `read_file(0)` reads and closes stdin. Run the argument through `os.fspath` first, which rejects an int.
		- Fixed: paths go through `os.fspath`, which rejects an int.
		- Opened: 20260829-071126
		- Closed: 20260829-093755

	- ✅ Item 16: `install.ps1` closes the caller's shell on every early exit under both documented one-liners.
		- `-Help`, a refused prompt, and every failure path call `exit`. Under `irm | iex` or the scriptblock form there is no script file, so `exit` ends the session. The default `-Target system` in an unelevated shell closes the window on the error message.
		- Use `throw` or `return`, and only `exit` when invoked as a file.
		- Fixed: the failure helper throws, help and a refused prompt return, and `exit` is used only when running from a file.
		- Opened: 20260829-071126
		- Closed: 20260829-093755

	- ✅ Item 17: the Linux binary needs glibc 2.34 or newer, and the installer only finds out after installing.
		- The published binary is dynamically linked. On Alpine, Ubuntu 20.04, RHEL 8 the install completes and then the final version check fails with a raw shell error, leaving the install in place. Nothing documents the floor.
		- Smoke-run the binary before touching the destination and fail with a message naming the floor and the `cargo install` route; state the floor in the README. A static musl build would remove the class.
		- Fixed: the binary is smoke-run before anything is installed; on failure the message names the glibc 2.34 floor and `cargo install`. The README states the floor.
		- Opened: 20260829-071126
		- Closed: 20260829-093755

	- ✅ Item 18: `install.ps1 -Uninstall` can offer a recursive delete of the install directory, default Yes.
		- The directory removal has no `-Recurse`, so when anything else is in there PowerShell prompts to delete all children. The NSIS setup installs to the same directory with its own uninstaller, so that case is real.
		- Remove the directory only when empty, as the bash installer does.
		- Fixed: the directory is removed only when empty, as the bash installer does.
		- Opened: 20260829-071126
		- Closed: 20260829-093755

	- ✅ Item 19: both installers print an uninstall hint that does not work under the one-liner.
		- Bash prints `$0`, which is `bash` or a file-descriptor number there; PowerShell prints a filename the one-liner user never has. Print the documented one-liner with the options appended.
		- Fixed: both print the documented one-liner with the uninstall option, or the file form when run from a file.
		- Opened: 20260829-071126
		- Closed: 20260829-093755

	- ✅ Item 20: the no-terminal abort prints a raw shell error before the intended message.
		- The `read` opens `/dev/tty` before stderr is redirected, so the open failure prints first. Three sites across the two bash installers. Swap the redirection order.
		- Fixed: redirection order swapped at all three sites.
		- Opened: 20260829-071126
		- Closed: 20260829-093755

	- ✅ Item 21: the PowerShell installer orders pre-releases as text.
		- `rc10` sorts below `rc2`, so with ten or more pre-releases on one version it picks the wrong one. The bash side sorts the number as a number. Only matters past nine, but the two installers should agree.
		- Fixed: digit runs in the pre-release suffix sort numerically, matching `sort -V` on the bash side.
		- Opened: 20260829-071126
		- Closed: 20260829-093755

	- ✅ Item 22: `install.ps1` fails on two older Windows setups.
		- `tar` is called unguarded; Windows before 10 1803 (Server 2016 is still in support) has none, and the failure is a raw CommandNotFound after the download.
		- `Invoke-WebRequest` runs without `-UseBasicParsing`, which on PowerShell 5.1 needs the IE engine and fails on Server Core.
		- Fixed: `-UseBasicParsing` everywhere, and `tar` is probed before the download.
		- Opened: 20260829-071126
		- Closed: 20260829-093755

	- ✅ Item 23: the bin symlink overwrites a real file; the man symlink refuses to.
		- A hand-placed `~/.local/bin/shcl` (the DIY route the README describes) is silently replaced, and uninstall then removes it. Give the bin link the same guard the man link has.
		- Fixed: the bin link gets the man link's guard, on install and on uninstall.
		- Opened: 20260829-071126
		- Closed: 20260829-093755

	- ✅ Item 24: `sign-release.bash` exits silently when `xxd` is missing.
		- Only `openssl` is probed; the fingerprint step pipes through `xxd` and `base64 -w0` under strict mode with no error trap. Probe both, or drop the pair.
		- Fixed: the modulus round trip goes through printf and openssl, so xxd is no longer used.
		- Opened: 20260829-071126
		- Closed: 20260829-093755

	- ✅ Item 25: `install-dev.bash` assumes curl and only recognizes the upstream remote.
		- A wget-only machine fails after the plan; a contributor inside a fork's clone gets a second nested clone of upstream. Probe curl or wget like the release installer; match the repo by directory layout, not remote URL.
		- Fixed: curl or wget, and the clone is recognized by its layout.
		- Opened: 20260829-071126
		- Closed: 20260829-093755

- Code review 20260822:

	- Item 1 is here; 2 to 9 are under Done - Features and enhancements.

	- ✅ Item 1: `install-dev.bash` never set up the git hooks on a fresh clone.
		- After the clone it changed into the new directory, so the relative clone path no longer resolved and the hooks step silently skipped at exit 0. The path is made absolute after the `cd` now.
		- Opened: n/a
		- Closed: 20260822-115416

- Code review 20260817:

	- Items 1 to 16 are here; 17 to 24 and 26 to 30 are under Done - Features and enhancements; 25 is under Canceled.

	- ✅ Item 1: the three ports disagree with the reference on a quoted selector.
		- Reproduced: a document whose quoted selector needs the rare fallback scan formats to two lines under the reference and three under go, python and c.
		- Cause: the reference rescans whenever the lookup accelerator fails to hand back a single scalar. The ports only rescan when the accelerator found something and it was the wrong kind, so an outright miss falls through and creates a node instead of selecting one.
		- The accelerator is meant to be a pure speed-up, so a miss must never change the answer. The reference is right and the three ports need to follow it.
		- Not visible to the corpus or the cross-binding check, because no case has this form. Needs a case as part of the fix.
		- Moves output bytes in three bindings. Do it before the next cut; the ports currently define a different language than the reference does.
		- Fixed: the three ports now take the fallback scan on an outright accelerator miss as well as on a wrong-kind hit, matching the reference. Case 051 pins the miss, the selection reaching the right instance, and the two paths that still create.
		- Opened: 20260817-204524
		- Closed: 20260818-091437

	- ✅ Item 2: the c library no longer builds for windows.
		- Reproduced: the file tier added a `windows.h` include, and one of the library's own tables shares a name with a type that header defines. The mingw build of the library alone now fails outright.
		- New on dev, so it is an unreleased regression, not a released one. Defining the no-file-io switch still builds.
		- Renaming the table with the library's own prefix clears it - a static in a public single header should carry the prefix anyway.
		- Missed because the cross-compile stage only builds the rust binary, so the windows branches of the c code have never been compiled by cicd. See item 28.
		- Fixed: the table carries the prefix, and the whole c cli now cross-compiles clean at the project's warning level. The four other short internal macros the implementation left behind are undefined at the end of the block for the same reason - they would otherwise outlive the header in the consumer's own file.
		- Opened: 20260817-204524
		- Closed: 20260818-113345

	- ✅ Item 3: the drop-in build the readme documents no longer compiles.
		- Reproduced: the readme's own compile line fails on dev with implicit declarations from the new file tier. Same for c99 and c17; only the gnu dialect or a posix define still works.
		- The header already anticipated this for one symbol and guarded it, but the other seven from the same block went unguarded.
		- The project never sees it because both of its own builds define the posix macro themselves. A consumer following the header's stated recipe does not.
		- Fix: define the posix and xopen macros at the top of the file-io block, guarded so a consumer that already set them wins.
		- Fixed, but at the top of the FILE rather than the block: a feature request only counts before the first system header, and the block sits well below them. Guarded, so a consumer who already asked for a level keeps theirs. The documented line now works on c99, c11, c17 and the gnu dialect.
		- Opened: 20260817-204524
		- Closed: 20260818-113345

	- ✅ Item 4: c float handling breaks under the host program's locale.
		- Reproduced under a comma-decimal locale: canonical output diverges from the other three bindings, every float read comes back bad-type, and the float formatter truncates 1.5 to "1" - so every float the writer emits loses its fraction.
		- Cause: the number parser and the number formatter both go through library calls that follow the locale's decimal point. The other three bindings' equivalents are locale-independent by definition.
		- The cli is safe: it pins the locale at startup, with a comment saying exactly why. The library, which is the actual product for c, does not, and a host application setting its own locale is ordinary.
		- Fix: pin the numeric locale around those two sites.
		- Fixed by translating the decimal point at both sites instead of pinning: pinning is a process-wide side effect a library has no business causing, and it is not thread-safe. Whole corpus now formats byte-identically under a comma-decimal locale.
		- Pinned since 20260831 by `check-locale.bash`, which builds a comma-decimal locale of its own and runs the whole C corpus and the CLI under it. The earlier reading that this could not be tested was about the corpus, where it holds, and not about a gate. The runner's own float parsing had the same defect and was fixed with it.
		- Opened: 20260817-204524
		- Closed: 20260818-113345

	- ✅ Item 5: a setter accepts a path it cannot write back.
		- Reproduced: setting a value under a quoted segment containing a newline succeeds, and produces a document that no longer parses. Reading the value back gives not-found and the file reports two errors.
		- All four bindings. The generator already rejects this exact case; the writer's own path check does not.
		- Worse, the reload counts nothing as lost, so the new save gate does not refuse - which is precisely the class of loss the gate was added for.
		- Fix: reject a newline or carriage return in a segment at the write check, so nothing is created. A carriage return alone is the same class.
		- Fixed in all four, for a segment name and for a by-value selector - the selector stores the path text raw, so it bypassed the escaping the ordinary setters already do. The escaped spelling is a different path and still writes fine.
		- A carriage return turned out not to be the same class: it round-trips intact, so rejecting it would be a behavior change with no defect behind it. Only the line break is refused.
		- Verified: pinned by the write-reason fixture in all four runners rather than the corpus: an ops line is line-based and cannot carry a raw newline. Same reason the reason-code list is spelled out in the spec instead.
		- Opened: 20260817-204524
		- Closed: 20260818-114245

	- ✅ Item 6: values with unusual whitespace at the edges are silently truncated on reload.
		- Cause: the emitter decides whether to quote from a fixed short list of characters, but the parser trims values against the full unicode whitespace set. Anything in the gap has no spelling that survives a round trip.
		- Reproduced across the gap: a value ending in a carriage return, a non-breaking space, a vertical tab, or any of the unicode spaces comes back shortened. Same loss through arrays and through comments. Plain spaces and tabs are fine - they are on the list.
		- All four bindings. The writer fuzzer cannot see it: its character set contains none of these.
		- Fix: also quote when the first or last character is whitespace by the same definition the parser trims by. Edge-only on purpose - quoting on interior whitespace would move bytes for documents that round-trip fine today.
		- Fixed in all four exactly that way; every existing case still matches byte for byte, so nothing moved. Checked first that all four already agree on which characters count as whitespace - they do, or the fix itself would have split them.
		- Verified: case 052 pins it through the writer, where the loss was reachable: an author-quoted value already survived, so only a written one showed it.
		- Opened: 20260817-204524
		- Closed: 20260818-115430

	- ✅ Item 7: formatting deletes the contents of a blank-looking line inside a raw block.
		- Reproduced: a raw block whose body has a line of only spaces formats with that line emptied. Reading the block back confirms the spaces are gone.
		- A raw block is contracted as verbatim, with only the common nesting indent stripped, so this is content loss rather than a documented normalization.
		- All four bindings. No corpus case has a whitespace-only line, so nothing pins the current behavior.
		- Fix: strip the common indent from such a line like any other, and fall back to empty only when the prefix is absent.
		- Fixed in all four with one shared helper: strip however much of the common indent the line actually shares. A blank-looking line takes no part in computing that indent, so it can be shorter than it. That is why the old code special-cased it at all, and blanking was the wrong way out.
		- Superseded by Code review 20260829 item 2: the common-indent rule is gone; each body line loses only what it shares with the closing fence's own indent.
		- Verified: case 053 pins the whole gradient: a line longer than the indent, one shorter, and a truly empty one.
		- The case carries meaningful trailing whitespace. An editor that trims on save would quietly break it; that is the first thing to check if it ever starts failing on its own.
		- Opened: 20260817-204524
		- Closed: 20260818-115430

	- ✅ Item 8: an in-place write silently deletes lines it could not read.
		- Reproduced: a config with one unreadable line, plus one setting change, comes back a line shorter. Exit code zero, nothing on stdout, nothing on stderr, no record the line existed.
		- All four clis. Same for formatting in place.
		- The library grew a save gate for exactly this in the round just finished. The clis neither call it nor consult the count behind it.
		- design.md justifies the current behavior on the grounds that someone sees the diagnostics on stderr. At the default strictness they see nothing at all, so either the code or that sentence has to change.
		- Fix: print the load's errors to stderr on an in-place write, and refuse when anything was dropped unless an override flag is passed - mirroring the library so the two cannot disagree about what is safe.
		- Fixed exactly that way in all four clis, with `--lossy` as the override. The mirroring is literal: the write now calls the library's own save rather than the raw atomic write, so there is one copy of the rule, not five. Only the refusal's wording is the cli's, since the override a user has is a flag rather than a function name.
		- The whole load's diagnostics go out, not just the errors, so what an in-place write reports and what `check` reports on the same file cannot drift apart.
		- `--lossy` on its own is a usage error: it would read as protection the command never had.
		- Retained malformed lines do not trip it, since they survive the rewrite, so the refusal fires only where content would actually be deleted.
		- design.md's justifying sentence was the false half and is rewritten; the spec and the readme now state the refusal.
		- Verified: pinned in `crosscheck.bash`, not the corpus: refusing leaves the file byte-identical, which only the write dimension's tree compare can see. That compare was itself blind to the exit code, so a refusal and a no-op success looked alike; it now carries the code too.
		- Opened: 20260817-204524
		- Closed: 20260818-121906

	- ✅ Item 9: piping a document into `set` throws it away.
		- Reproduced: `cat app.shcl | shcl set - --set workers=99` prints only the new setting. The piped document is gone, exit code zero, nothing on stderr. Chaining two `set` calls loses the first.
		- Cause: `-` means "read the document from stdin" on every other subcommand, and "empty base" on `set`, where stdin is reserved for the ops script. With `--set` given, stdin is never read.
		- All four clis. Without `--set` the same command is loud and correct, so it is the combination that goes quiet.
		- Fix: either make `set -` read the document like everything else and move the ops script to its own option, or make the quiet combination an error.
		- Fixed as the first option, but without a new option for the ops script: `-` follows stdin. With the edits given as options no ops are read, so stdin carries the document the way it does on every other subcommand; only when stdin is the ops script does `-` still mean an empty base. The two meanings never compete, so nothing that works today changes.
		- Erroring instead was rejected: `set - --set a=1` to build a document from nothing is a legitimate use, and refusing it would leave no spelling at all for the piped case the item is about.
		- Sniffing whether stdin is a terminal was also rejected - it would make the same command mean different things in a pipeline and interactively, which the corpus and the crosscheck could never pin.
		- The one behavior change: `set - --set ...` with no redirection now waits on stdin at a terminal, exactly as `fmt -` and every other `-` already does. Redirect from /dev/null for an empty base.
		- Verified: both meanings pinned in `crosscheck.bash` (the piped document, and the ops script over an empty base). Help, spec and the `--layer=-` refusal wording updated to match.
		- Opened: 20260817-204524
		- Closed: 20260818-122701

	- ✅ Item 10: go accepts a non-text file as cleanly loaded.
		- The new file tier reports clean for a file that is not valid text, where the reference and python both report unreadable and hand back an empty document.
		- Go's own status comment dropped the bad-encoding clause the other two keep, which reads as an omission rather than a decision.
		- Values then come back mangled while the canonical form re-emits the original bytes, and a later save writes the mangled version.
		- The cli is unaffected - it checks separately. Library only.
		- Fixed in go, and in C, which had it too and the item did not name: neither read path validates, so both loaded a binary file clean. Rust's read-to-string and python's decoding open reject it for free.
		- C's validator is its own copy rather than the cli's: that one also gates argv and the ops script, which exist with the file tier compiled out.
		- Both status comments now carry the bad-encoding clause the other two kept. Pinned by the shared file-tier fixture in all four runners.
		- Opened: 20260817-204524
		- Closed: 20260818-124653

	- ✅ Item 11: the python file tier raises where it promises a status.
		- A path containing a null byte raises out of both load and save, which their own docstrings say cannot happen. The catch lists two exception types and the one that actually fires is a third.
		- The reference returns unreadable and an error respectively, so a consumer porting the four-case handling gets an exception on python and a status everywhere else.
		- Fix: add the missing type to both catch clauses.
		- Fixed. The save half raised from the path resolution before any i/o, so that call is what got the guard rather than a catch clause.
		- Verified: pinned in the python runner only, not the shared fixture: a C path string cannot carry a NUL at all, so there is nothing to compare against.
		- Opened: 20260817-204524
		- Closed: 20260818-124653

	- ✅ Item 12: python accepts an integer the other bindings cannot express.
		- Cause: Python has no fixed integer width, and the setter adds no range check, so a value beyond the 64-bit range writes happily and then reads back as bad-type - by every binding, including python itself.
		- The cli already range-checks in the same situation. The library does not.
		- Fix: range-check in the integer setters and return false, which is the failure channel they already have.
		- Fixed in the scalar and array setters; the only-if-absent forms delegate to those, so all four are covered. The style guide already listed unbounded int as a banned shortcut - the rule existed, the setter just never applied it.
		- Verified: pinned in the python runner, including both in-range edges so the check cannot drift inward.
		- Opened: 20260817-204524
		- Closed: 20260818-124653

	- ✅ Item 13: the c++ date wrapper leaves the moved-from object pointing at freed memory.
		- Cause: both move operations rebind only the destination, so the source still points into the buffer the destination took, and reads from it after the destination dies.
		- Short values hide it; a date with a long fractional part does not.
		- Fix: rebind the source too. One line in each of the two operations.
		- Fixed, and the invariant moved into the rebind helper itself rather than the two call sites: the view always describes this object's own storage, has_frac included, so a moved-from value can no longer format a fraction it no longer holds.
		- Correction to the filing: the fraction is capped at nine digits, which fits the small-string buffer on the common implementations, so in practice this read a stale-but-live buffer rather than a freed one. Still wrong, still unspecified, and the fix is the same.
		- The smoke test moved a value and only ever checked the destination, which is why it survived. It now checks the moved-from half of both operations.
		- Opened: 20260817-204524
		- Closed: 20260818-124653

	- ✅ Item 14: python can raise on a document at the documented depth limit.
		- Cause: the parse-side walks were deliberately made iterative because python's frame budget is small. The merge, clone, validate and resolve walks kept the reference's recursion.
		- A document at the 512-level cap costs about one frame per level, so it succeeds from a shallow caller and raises from a deep one. The depth cap is documented as what makes a hostile document fail without crashing the consumer; on this binding it can crash the consumer.
		- The partially merged base document is left mutated when it raises.
		- Fix: convert the merge and clone walks to explicit stacks like the parse side, or document the requirement and raise something typed.
		- Fixed the first way, which the parse and emit walks already set the precedent for. Clone became a node copy driven by a stack; overlay split into a driver and a one-level worker that returns the pairs still to merge.
		- Deferring those pairs changes no result: each level's rebuild depends on nothing the deeper levels do, and a name that produces a pending pair is never one the rebuild replaces, so the base node it names survives. The walk stays depth-first and in order.
		- Verified: validate and resolve turned out not to need it. All four walks now survive a document at the cap from a deep caller, where merge used to raise well short of the cap.
		- Opened: 20260817-204524
		- Closed: 20260818-124653

	- ✅ Item 15: the as-authored name does not resolve escapes, against the spec and its own three comments.
		- The spec and the doc comments in all four bindings say the name comes back with quotes and escapes resolved. Escapes are not resolved - a name written with an escaped tab comes back as a backslash and a "t".
		- The value side does resolve escapes, so the two halves of the api disagree about what "as authored" means.
		- New and unreleased, so settling it is free now and expensive after the cut. Pick one: resolve the escapes, or correct the spec and the three comments to say quotes only.
		- Settled the second way: the docs were the wrong half. Names are never escape-processed anywhere - the spec's escape rule is a value rule, and a name is stored, compared, emitted and enumerated in its escaped spelling. Resolving in this one call would hand back a string that no longer names the node.
		- Five comments, not three: rust, go, python, c and the c++ veneer. Spec corrected to say so explicitly, so the next reader does not re-open it.
		- Verified: pinned in all four runners, so a later pass cannot quietly flip it back.
		- What this exposed has its own item, filed below: two names that differ only in escaping are different names, where the equivalent two values are the same string.
		- Opened: 20260817-204524
		- Closed: 20260818-124653

	- ✅ Item 16: small defects, batched.
		- Python's merge docstring sits below the first statement, so it is an inert expression and the method has no documentation at all - the one method whose override rule most needs explaining.
		- Go's error-count doc comment was split by an insertion, so the count function is undocumented and the lost-line count renders under the wrong name on the package page.
		- Go flattens the underlying i/o error to text in three places, so a caller cannot tell a permission failure from a full disk without matching on the message. The fix is to wrap.
		- Go's cli asserts the parse error's type without the checked form. Unreachable today, but it is the top-level error path.
		- The reference carries a dead byte-order-mark branch, with a comment describing behavior that only exists at the sibling site. Harmless, but it reads as a live invariant and will be copied.
		- The c++ header uses two standard types without including their headers, and declares one status out-parameter uninitialized.
		- One go doc line left far past where the rest of the block wraps.
		- All seven fixed. The python docstring move needed care - the statement it sat below is the one that carries a merged layer's lost count forward, and dropping it would have silently disarmed the save gate on any merged document.
		- The dead branch was the one under the bad-`*` line: that line begins with the `*` that got it there, so it can never also begin with a byte-order mark. Removed in all four, with the comment now pointing at the sibling site where the exception is real.
		- Go's i/o failures wrap instead of flattening, so a caller can separate a permission failure from a full disk without matching on prose. Printed text is unchanged.
		- Opened: 20260817-204524
		- Closed: 20260818-124653

- Code review 20260802:

	- Items 1 to 25 are here; 26 to 33 are under Done - Features and enhancements.

	- ✅ Item 1: formatting a file can change what it means.
		- Reproduced: a field that repeats, where the second one is an empty field later filled by a stacked list, formats to two identical lines. Reformatting that output collapses them to one, so a read that returned Multiple now returns a value.
		- Cause: when a node's value is filled in after the fact, it moves to a new merge key. If a later sibling already holds that key the filled node is left beside it instead of merging.
		- Note: all four bindings behave the same, so the cross-binding check can't see it, and no test case has this form.
		- Fixed: duplicate siblings are folded once parsing finishes, so the tree matches a reparse of its own canonical text. Folding is depth-first, since merging two parents can leave duplicate children a level down. Case 042.
		- Opened: 20260803-111610
		- Closed: 20260803-120312

	- ✅ Item 2: in-place writes create their temporary file unsafely.
		- Reproduced: the temporary name is predictable, and nothing stops it being a symlink someone else planted. The config's contents get written through that link, and the rename then turns the config itself into a symlink.
		- Second problem: the file's permissions are copied on only after the data is written, so a 600 config is briefly world-readable. Interrupting the write leaves that copy behind for good.
		- All four bindings. Only matters where someone else can write to the config's directory, but that includes shared and temp locations.
		- Fixed: the temporary is created exclusively, so an existing file or link under that name makes the attempt fail rather than be written through, and the next name is tried. It is born private and given the target's permissions through the open handle before any data, which also keeps a group-writable config from being narrowed by the umask.
		- Note: an interrupted write can still leave a temporary behind. It now carries the config's own permissions, so it is not an exposure, and clearing it would need signal handling in all four builds.
		- Verified: the differential harness gained a check for it, since none of this shows up in normal output.
		- Opened: 20260803-111610
		- Closed: 20260803-121341

	- ✅ Item 3: one schema line can switch off the unknown-field check.
		- Reproduced: a field path written as a quoted name that starts with a star, such as a wildcard hostname key, silently becomes a real wildcard. Every unknown top-level name then passes, and the constraints on that line never apply.
		- Cause: the schema's own value parsing strips the quotes before the path is scanned, so the scanner sees a bare star.
		- The spec says the opposite in two places: a quoted star stays a literal name.
		- Fails open, which is the worst direction for the one check meant to catch typos.
		- Not a defect after all: quoting works at two levels, and the schema is behaving correctly at both. The outer quotes are ordinary string quotes around the value, needed by any path holding a selector, and they come off before the path is read. So a wildcard inside a quoted path stays a wildcard, which composed paths rely on. Quoting the segment itself, inside the value, does give a literal name, and the unknown-field check still catches typos alongside it.
		- Fixed the real problem, which is that nothing said so: the spec now spells out both levels with worked examples, including that the tempting spelling declares an open section rather than a literal name.
		- Opened: 20260803-111610
		- Closed: 20260803-124328

	- ✅ Item 4: validating against a recursive schema can hang.
		- Reproduced: when two constraint paths match the same node and both mount the same fragment, the work doubles per level of the document. A file around thirty lines deep takes over a minute; a little deeper and it never finishes.
		- The C build also runs out of memory, because each level's working data is kept until the whole validation ends rather than being released on the way back out.
		- Fixed: each constraint set is checked once per node, so two paths reaching the same node cost one pass. A document near the depth cap now validates near-instantly.
		- Fixed in C as well: the working data for each level is released on the way back out. The same document went from gigabytes to megabytes. Case 043.
		- Opened: 20260803-111610
		- Closed: 20260803-124328

	- ✅ Item 5: generating a file from a recursive schema can crash.
		- Reproduced: a long chain of fragments overflows the stack and aborts the process in the reference build; Python raises instead; a short schema that branches can eat all available memory before it finishes.
		- Validation of the same schemas is fine. Only generation expands every path up front.
		- Note: generation reads a file the user supplies, so this is reachable from ordinary use.
		- Fixed: a mount chain that reaches the nesting cap is noted like one that re-enters, rather than followed. A schema whose mounts multiply past a field ceiling now reports a schema fault, V096.
		- Validation is unaffected: it still follows the document, so it needs no limit.
		- Opened: 20260803-111610
		- Closed: 20260803-124328

	- ✅ Item 6: Python raises on a very long number where the others return cleanly.
		- Reproduced: a value of five thousand digits makes the Python build exit with a stack trace, while the reference reports a bad type. Same for oversized selector indexes, schema repeat counts, and a day number inside a quoted date.
		- Cause: Python refuses to convert decimal strings past a few thousand digits, and that happens before the code's own range check.
		- Fixed: leading zeros are dropped and the rest is length-checked against what the type can hold before any conversion, so a small value written behind thousands of zeros still reads. Hexadecimal was never affected.
		- Opened: 20260803-111610
		- Closed: 20260803-130248

	- ✅ Item 7: the C date formatter can write past the buffer it documents.
		- Reproduced: the header promises 64 bytes is enough, and caps only the fractional seconds. The year and the other fields come straight from a struct the caller fills in, so a hand-built value can need well over that. The library's own writer hits this too.
		- Values that came from parsing are always short enough, so the test corpus can't see it.
		- Fixed: the text is built in full and then clamped to the documented size, which is now stated at the declaration. Output for values that came from parsing is unchanged.
		- Found alongside it: negating the most negative offset was itself undefined, and is now done at a width that holds it.
		- Pinned since 20260831 by the C runner's `dt_clamp` fixture, which renders a struct filled with the extreme of every field. The clamp is checked directly; the working buffer behind it is checked by the sanitizer run.
		- Opened: 20260803-111610
		- Closed: 20260803-130248

	- ✅ Item 8: comments get dropped when documents merge.
		- Reproduced: merging layers loses every comment attached to a section that exists in both. The higher layer's comments are the ones that disappear.
		- Separately, a write that merges two duplicate fields drops any comment that hung below the losing one.
		- Both contradicted the documented promise that comments travel with the node they belong to.
		- Fixed: a matched instance now takes on the higher layer's comments, and the shared fold carries the comments that hang below a block. Spec and case 042 pin both.
		- Opened: 20260803-111610
		- Closed: 20260803-120312

	- ✅ Item 9: the one-shot load-and-validate ignores a broken schema.
		- Reproduced: a schema with a bad indent loads partially and validation runs anyway, so constraints on the dropped lines quietly vanish. A badly broken schema makes every field in the config report as unknown.
		- The command-line tool gets this right and reports a schema failure. Only the library shortcut skipped the check.
		- Fixed: the shortcut now reports the same schema failure and validates nothing. An empty schema still means skip validation, as before.
		- Opened: 20260803-111610
		- Closed: 20260803-131801

	- ✅ Item 10: one write operation is spelled differently by the reference.
		- Reproduced: `datetime-array-default` is rejected by the reference and accepted by the other three.
		- Write output and exit codes are supposed to match everywhere. No test case uses this operation, which is why it went unnoticed.
		- Fixed: the reference accepts it like the others. The vocabulary was then checked verb by verb across all four, and a test line for it was added so the gap cannot reopen.
		- Opened: 20260803-111610
		- Closed: 20260803-131801

	- ✅ Item 11: writing in place with layers overwrites the file with the merged result.
		- Reproduced: formatting with a lower layer and `--write` folds that layer's contents permanently into the top file, which defeats the point of layering.
		- Help text says layering prints the merged document; it doesn't mention what `--write` then does.
		- Fixed: the combination is refused, like other option pairs that cannot both hold.
		- Opened: 20260803-111610
		- Closed: 20260803-131801

	- ✅ Item 12: a value that looks like a help flag takes over the command.
		- Reproduced: passing `-h` or `--version` as a default value, a path, or a filename prints help or the version to normal output and exits successfully. A caller reading a value gets the help text back.
		- Cause: the whole argument list is scanned for those flags before options are parsed.
		- Fixed: only a flag in option position counts. A value that reads like one, and anything after the file, is data.
		- Opened: 20260803-111610
		- Closed: 20260803-131801

	- ✅ Item 13: generated files don't always load.
		- Reproduced: a wildcard written with spaces inside the brackets, or with the alternate colon spelling, produces a line the parser rejects or a path that fails its own schema. A deep chain of fragments produces paths past the nesting limit.
		- Cause: the wildcard is stripped out of the path as text rather than rebuilt from the parsed segments.
		- The documented promise is that generated output always loads clean and validates against the schema that produced it.
		- Fixed: the path is rebuilt from its parsed segments rather than cut out of the text, so every spelling of a wildcard behaves the same. A path deeper than a document may nest now goes to the trailing note instead of being written out.
		- Opened: 20260803-111610
		- Closed: 20260803-124328

	- ✅ Item 14: the C++ wrapper can hand back a dangling date.
		- Reproduced: every other read in the wrapper copies its text out, but the structured date read copies the struct while its fractional-seconds pointer still points into the document. Letting the document go out of scope and then using the date reads freed memory.
		- Fixed: the wrapper's date read now owns its fractional digits, so it stays valid after the document goes away, like every other read there.
		- Note: this changes what that one call returns, so C++ callers using it need a small edit. Documenting the borrow instead was the alternative, but it would have left a wrapper whose whole purpose is lifetime safety handing out something unsafe.
		- Opened: 20260803-111610
		- Closed: 20260803-130248

	- ✅ Item 15: the Go repeat-hint filter damages the caller's list.
		- Reproduced: it filters in place while returning a new list, so calling it the obvious way leaves the document's own diagnostics shuffled and duplicated.
		- The reference takes the list by reference, so the mutation is expected there. The Go spelling returns a value, which reads as a copy.
		- Command-line use is unaffected; this only bit programs using the library.
		- Fixed: it builds its own list, so the caller's is never disturbed.
		- Pinned since 20260831 by `TestSuppressLeavesTheCallersDiagnosticsAlone`, over both filters. Backing the fix out reproduces the shuffle and the duplicate exactly.
		- Opened: 20260803-111610
		- Closed: 20260803-134341

	- ✅ Item 16: three C entry points pile up garbage in documents they don't own.
		- Reproduced: each setter keeps about a kilobyte of path-scanning leftovers, the repeat-hint filter leaves a few hundred bytes in the schema, and generation leaves several kilobytes there. None of it is ever reused or released.
		- A long run of setter calls grew a document by hundreds of megabytes; routing the same work through the existing scratch space removes nearly all of it.
		- The command-line tool is unaffected, since it exits after one pass. A long-running program that holds a parsed schema is not.
		- Fixed: all three now do their working allocation somewhere temporary and keep only what they promise to return. A long run of writes now grows the document by a small fraction of what it did, and the two schema entry points by near nothing.
		- Opened: 20260803-111610
		- Closed: 20260803-130248

	- ✅ Item 17: the segment-quoting helper mangles a name ending in a backslash.
		- Reproduced: the closing quote gets treated as escaped, so the result is a path the scanner rejects, and the set silently fails.
		- The helper exists to make user-typed text safe to splice into a path, so this is the case it was written for.
		- Fixed in the shared quoting helper, so segment quoting, path enumeration and the formatter all get it. Any name now round-trips.
		- Opened: 20260803-111610
		- Closed: 20260803-134341

	- ✅ Item 18: repeat-hint suppression can silence the wrong field.
		- Reproduced: a schema path whose last segment is quoted and contains a dot is split on that dot, so the leftover text matches an unrelated field name.
		- Matching on the name alone is deliberate. Splitting the raw text rather than using the parsed segments was not.
		- Fixed: the name comes from the parsed path. A quoted literal star is no longer mistaken for the wildcard either.
		- Opened: 20260803-111610
		- Closed: 20260803-134341

	- ✅ Item 19: a null byte inside a field name can pose as a dotted path.
		- Reproduced: a single field whose name contains a null passes the unknown-field check as if it were two nested names.
		- Cause: the check joins path parts with a null before comparing.
		- Pre-existing, but the new wildcard matching is built on the same joined text.
		- Fixed: each part is now written with its length, the same way merge keys already solved this, so no name can pose as two.
		- Opened: 20260803-111610
		- Closed: 20260803-134341

	- ✅ Item 20: end-of-file comments multiply when layering.
		- Reproduced: a footer comment shared by three layers appears three times in the merged output. The result still formats stably.
		- Fixed: each distinct end-of-file comment is carried over once.
		- Opened: 20260803-111610
		- Closed: 20260803-120312

	- ✅ Item 21: the default install location isn't on a normal user's path.
		- Reproduced: a system install links the program into a directory reserved for administrator sessions, so the user who ran the installer can't invoke it by name. The closing check passes because it uses the full path.
		- The project's own packages install to the ordinary location instead.
		- Fixed: a system install links into the ordinary location, matching the project's own packages, and the path note prints for both targets.
		- Opened: 20260803-111610
		- Closed: 20260803-134341

	- ✅ Item 22: the shell wrapper trusts an inherited private variable.
		- Reproduced: the wrapper caches the resolved program path in a variable it also reads from the environment, so setting that variable picks the program with no message and beats every documented lookup step.
		- The PowerShell wrapper gets this right by clearing it when loaded.
		- Fixed: the shell wrapper clears it at load too.
		- Opened: 20260803-111610
		- Closed: 20260803-134341

	- ✅ Item 23: hosted CI installs a checking tool without verifying the download.
		- The workflow pins its actions by commit and its packages by version, then fetches one tool as an archive with no checksum and installs it ahead of the system copy.
		- Fixed: the download is checked against a pinned hash before it is installed.
		- Pinned since 20260831 in `check-pins.bash`: every file the workflow fetches has to be named on a line that checks it against a sha256.
		- Opened: 20260803-111610
		- Closed: 20260803-134341

	- ✅ Item 24: the installers handle their own options poorly.
		- Reproduced: asking for help through the documented pipe prints nothing and exits successfully, because the script tries to read its own file, which isn't there when piped. With a stray file of the right name in the current directory it prints that file instead.
		- Also, giving an option without its value exits silently, where the program itself explains what's missing.
		- Fixed in all three scripts: the help text is carried in the script instead of read back out of its own file, and a missing option value is reported the way the program reports it.
		- Opened: 20260803-111610
		- Closed: 20260803-134341

	- ✅ Item 25: small gaps in argument handling across all four builds.
		- There is no way to end the options and pass a path that starts with a dash.
		- `init` ignores extra arguments; every other subcommand rejects them.
		- An option expecting a value will take the next flag as that value without complaint.
		- Reading the write operations from standard input while also asking for standard input as a layer silently produces nothing.
		- Fixed: `--` now ends the options, `init` rejects extra arguments, and asking for standard input twice is refused. The help text says what a value option does with the next argument, which is the one case left as it was, since that is how options normally behave.
		- Opened: 20260803-111610
		- Closed: 20260803-131801

- Code review 20260726:

	- ✅ Item 1: an in-place write does not preserve the file it replaces.
		- Cause: `fmt --write` and `set --write` build a temp file and rename it over the target, so the target's own identity is discarded.
		- Reproduced: a config at mode 600 comes back at the umask default, which on a normal box means world-readable. Config files are exactly where secrets sit, so this is the one that matters.
		- Reproduced: a symlinked config is replaced by a regular file. The link breaks, the real file behind it keeps the old content, and the edit appears to vanish. Dotfile managers make this a common layout.
		- Reproduced: hard links break the same way - the other name keeps the old content.
		- Fixed: all four bindings resolve the path through symlinks before choosing the temp directory and the rename target, then copy the original's mode onto the temp file. Mode copying is best effort, so a filesystem that cannot carry it still completes the write.
		- Note: hard links cannot survive a rename at all. Atomicity matters more, so that one is a documented limitation rather than a fix.
		- Verified: the differential harness gained an in-place-write dimension comparing the tree a write leaves behind (mode, symlink, link count, content), which pins all three cases across the bindings.
		- Opened: 20260726-112941
		- Closed: 20260726-115207

	- ✅ Item 2: C datetime reads accumulate in the never-freed document arena.
		- Cause: the read path hands the document arena to the datetime parser for its split temporaries, so a long-running reader grows without bound. Every other C read path had moved to the per-call scratch arena; these two were missed.
		- Reproduced: a long loop of datetime reads on a tiny document grows to hundreds of megabytes. The same loop reading an int stays flat.
		- Fixed: both datetime read paths allocate their temporaries from scratch. The fractional-seconds value they hand back points into the document text, which outlives the call, so the return contract is unchanged.
		- Verified: the same loop now stays flat.
		- Opened: n/a
		- Closed: 20260726-112941

	- ✅ Item 3: the fuzz run is documented as deterministic but is not reproducible.
		- Cause: the mutation PRNG is fixed-seed, but its seed inputs come back from the corpus directory in whatever order the filesystem gives. That order decides every mutation.
		- Reproduced: the cross-binding comparison count moves by a few hundred between runs on an unchanged tree, which makes the number useless as a regression signal. A failing case also cannot be re-run, which is the main thing a fuzz gate is for.
		- Fixed: the seed list is sorted before use.
		- Verified: two runs on the same tree now produce an identical input set.
		- Opened: n/a
		- Closed: 20260726-112941

	- ✅ Item 4: the cross-binding harness reports only the first divergence.
		- Cause: it prints a divergence, then pipes `diff` into `head` to show it. Under the script's own strict settings that pipeline's nonzero status aborts the run, so every later divergence is lost and the "N of M diverged" summary never prints.
		- Note: never a correctness hole. The run still exited nonzero and the gate still failed; the cost was diagnosis, one divergence at a time with no count.
		- Reproduced: only because a new check was deliberately run against a known-bad build. An error path that is itself fatal stays untested until the day it is needed.
		- Fixed: the diff is allowed to fail. All divergences print, followed by the summary.
		- Opened: n/a
		- Closed: 20260726-115207

	- ✅ Item 5: hosted CI cannot install its own pinned lint toolchain.
		- Cause: the tool pins carry the Cppcheck binary's version, and the workflow installed that same string as a package version. No such package exists, so every hosted run failed at setup within seconds.
		- Note: the wheel bundles a Cppcheck two major versions ahead of its own package number, which is what made the two look interchangeable.
		- Note: only the hosted gate was affected. The local pipeline probes the installed binary, so it stayed green, which is why this went unnoticed from the day the pins were added.
		- Fixed: the workflow installs the package version, with a comment saying why the two differ.
		- Verified: the other three pins do resolve.
		- Opened: n/a
		- Closed: 20260726-145039

	- ✅ Item 6: shellcheck is a gating linter but the only one left unpinned.
		- Cause: hosted CI used whatever the runner image carried, which is older and noisier than the local copy, so a script could pass the local gate and fail CI on a warning newer releases no longer emit. That is what happened to `install.bash`.
		- Note: the pinning pass that covered the other linters missed it because it is preinstalled rather than installed by a step, so there was no version to write down.
		- Fixed: CI installs the pinned version ahead of the image's copy, and the pin joins the others in the drift list. The one flagged line in `install.bash` was rewritten to the form the rest of that file already uses.
		- Fixed alongside it: the drift probe read only the first line of a version command's output, so a tool that leads with a banner always looked like it had drifted.
		- Opened: n/a
		- Closed: 20260726-150154

- Code review 20260725:

	- Items 1 to 23 are here; 24 to 41 are under Done - Features and enhancements, with the deferred halves of 28 and 29 under Future and/or deferred.

	- ✅ Item 1: a higher layer that names a container with no children deletes the whole subtree below it.
		- Reproduced: `server:` (or `server: web1` with an empty body) in an over layer wipes every child the lower layers put there, silently, exit 0.
		- Wider than it reads: the wipe covers every same-named instance, so mentioning `server: web1` also deletes an untouched `server: web2`.
		- A body that is only a comment counts as empty, because comments are trivia rather than children.
		- Fixed: the leaf-override path now applies only when the base side of the name group is also all-childless, so a bare section header merges (matching instance untouched, unmatched appended as an empty instance) and leaf clearing still works. No way to blank a section from a higher layer; a deletion spelling is deferred. All four bindings, spec reworded, corpus case 027.
		- Opened: 20260725-152141
		- Closed: 20260725-162837

	- ✅ Item 2: deep documents crash three of the four bindings, each at a different depth.
		- Fixed with a load-time nesting cap: 512 levels, enforced in all four parsers before any node is created (`E016`, line skipped) and mirrored by the Writer's `place`. Every observed cliff sat far above the cap, so the existing recursion is now safe everywhere; the reference keeps its recursive walks on purpose (structural parity beats a one-binding rewrite). Spec gained the cap and the load-code table; corpus case 028 pins the error, a reference test pins the 512 boundary and the writer refusal.
		- Cause: emit and merge recurse per nesting level while the parser is iterative, so a document parses fine and then dies when anything formats or merges it.
		- Reproduced: Python merge is the acute one; a few kilobytes is enough. The reference aborts on `fmt` at tens of thousands of levels; C segfaults; Go survives by growing without bound.
		- A dotted path buys one level per two bytes, so depth is cheap for an attacker and needs no odd syntax.
		- Same input, four different exit codes: the inverse of the product guarantee.
		- Opened: 20260725-152141
		- Closed: 20260725-163911

	- ✅ Item 3: `shcl set` validates its op values four different ways.
		- Reproduced: Rust and Go reject a malformed int; Python accepts unbounded ints, underscores, padding and non-ASCII digits; C silently writes 0 or a truncated/saturated value and exits 0.
		- Python can emit an integer no binding can read back, so the Writer produces out-of-contract output.
		- The ops script on stdin also skips the UTF-8 gate the file reader applies, so bad bytes become U+FFFD instead of exit 1.
		- stdout and exit codes are contract here and the crosscheck replays `set`, so this only escapes CI because no corpus op line carries a malformed value.
		- Fixed: go/python/C now gate op values with the reference's exact grammar: sign + ASCII digits + i64 range for ints, the Rust f64 grammar for floats, with overflow stored as `inf`. C's truncating staging buffers are gone. The ops stdin gets the same UTF-8 gate as file input in all four. Corpus case 029 pins accept and reject sets cross-binding (`write-bad.ops` dimension in all four runners + the differential harness).
		- Superseded in part by the 20260901 round's item 4: overflow is refused now rather than stored as `inf`, since the reader refuses `inf`. The grammar gate and the UTF-8 gate stand.
		- Opened: 20260725-152141
		- Closed: 20260725-170306

	- ✅ Item 4: Go `Validate` panics on a schema path with a `[#N]` selector at N >= 2^63.
		- Cause: `int(seg.sel.index)` wraps negative, the bounds check passes, and the index panics. The three sibling sites in the same file compare against `uint64(len(...))` correctly.
		- Hard process abort inside a library call, which is exactly what the status-as-data design exists to avoid.
		- One-line fix, Go only.
		- Fixed: the bounds check compares in uint64 like the sibling sites.
		- Opened: 20260725-152141
		- Closed: 20260725-171744

	- ✅ Item 5: C keeps every transient allocation in the never-freed document arena.
		- Cause: reads, merges and validation all allocate scratch there, so a read-only document grows without bound in a long-running process.
		- A long loop of reads on a tiny document reaches gigabytes; a sub-megabyte layer merge peaks at gigabytes where the reference uses megabytes.
		- Rust, Go and Python free the same temporaries per call, so this is C-only.
		- Fixed: the doc gained a scratch arena reset on entry to every resolve (path scans, resolver vectors, compare strings, int/float/bool coercion temps) and on merge entry. `w_replace_leaf_group` rebuilds the children vector in place. `v_suggest` gets a per-field-reset scratch for its DP rows and chains. Returned bytes still live in the persistent arena per the documented contract (string reads accumulate only their returned copies). Reads stay flat now, and a large merge or validation peaks at megabytes rather than gigabytes.
		- Opened: 20260725-152141
		- Closed: 20260725-173108

	- ✅ Item 6: C grows a stacked `*` list one element at a time, so parsing it is quadratic in memory.
		- Cause: a fresh array is allocated and copied per `* ` line, and the arena keeps every discarded copy.
		- Reproduced: a file of a few kilobytes already costs tens of megabytes, and a few hundred kilobytes costs more memory than most machines have.
		- Reachable from a plain `fmt` or `check` on an ordinary-looking config.
		- Fixed: the element array grows geometrically, and the per-element merge-key rebuild is deferred while the list is the open field (flushed before any other map lookup, so behavior is unchanged). A list of tens of thousands of elements now parses instantly in a few megabytes, with output byte-identical to the reference. The other bindings' key-rebuild time cost is item 24's territory.
		- Opened: 20260725-152141
		- Closed: 20260725-173108

	- ✅ Item 7: `fmt --write` truncates the config in place, and C reports success when the write fails.
		- Cause: C never checks `fwrite`/`fclose`, so a failed write prints nothing and exits 0 while the other three exit 1 - a live exit-code divergence.
		- Cause: no binding uses temp-file-and-rename, so an interrupted write destroys the file the tool exists to protect. The dogfood installer was made atomic for this exact reason.
		- Fixed: all four CLIs write through a temp-file-and-rename in the target's directory (data synced before the rename), and C checks every stdio call, so a failed or interrupted write exits 1 and never leaves a truncated file.
		- Opened: 20260725-152141
		- Closed: 20260725-170306

	- ✅ Item 8: both Windows installers damage `PATH`.
		- Cause: NSIS reads `PATH` through a 1024-char string and writes the truncated value back. Observed: a long machine PATH came back truncated to a fraction of its length. The uninstaller does the same.
		- Cause: `install.ps1` reads `PATH` expanded and writes it back as `REG_SZ`, baking `%USERPROFILE%`-style references and downgrading the value type.
		- The two Windows install paths also disagree with each other about how to test for an existing entry.
		- Fixed: the NSIS installer/uninstaller no longer pass PATH through NSIS strings at all. A generated PowerShell script edits the registry value directly (unexpanded read, segment-wise case-insensitive test, REG_EXPAND_SZ preserved), and a failed edit warns instead of writing back a truncated value. `install.ps1` likewise reads unexpanded and writes REG_EXPAND_SZ, with a settings-change broadcast.
		- Opened: 20260725-152141
		- Closed: 20260725-174721

	- ✅ Item 9: `shcl init --schema=X` generates a config that `shcl check --schema=X` rejects.
		- Cause: the generator only consults `required`; `repeat` with a lower bound of 1 or more is ignored entirely.
		- Cause: a wildcard `required` bites whenever some other live path materializes the wildcard's parent.
		- The project's own corpus golden fails against its own schema, so the two newest features contradict each other on the fixture that is supposed to pin them.
		- Fixed in all four generators: fields with a `repeat` lower bound of 1+ generate live like `required`, and a must-exist wildcard whose parent gets materialized by another live line is generated in dotted form under that instance. Generated output now validates clean against its own schema (asserted by every runner; the one documented exception is a repeat lower bound of 2+, which cannot be auto-satisfied). Case 026's golden regenerated.
		- Opened: 20260725-152141
		- Closed: 20260725-171744

	- ✅ Item 10: `shcl init --schema` can emit lines that do not parse.
		- Cause: a `[#N]` selector puts a `#` on the field line, which starts a comment, so the line truncates and reports E014.
		- Cause: a newline inside an `allowed` value breaks out of the annotation comment and injects a live binding.
		- The spec promises the generated file loads with no error diagnostics.
		- Fixed: `[#N]` paths (and paths carrying a literal newline) go to the trailing not-generated block instead of emitting a broken line. Newlines in the annotation are escaped to `\n`. A default with a newline is written in its quoted escaped spelling. Corpus case 030 pins all three.
		- Opened: 20260725-152141
		- Closed: 20260725-171744

	- ✅ Item 11: an unterminated quote in a value is accepted silently and swallows the trailing comment.
		- Reproduced: the stray opening quote stays in the value, no diagnostic at any strictness, and `fmt` then re-quotes it so the typo looks deliberate.
		- Cause: comment stripping is quote-aware, so `listen: "0.0.0.0:443  # note` eats the author's comment into the value.
		- One of the commonest hand-authoring mistakes, and the exact class of error the product markets itself as catching. Path position already diagnoses it; only value position is silent.
		- Fixed: a value or array element that opens with a quote it never closes now draws `E017` (value kept as written, fails strict) in all four parsers - field values and stacked `*` elements both. Mid-text apostrophes (`it's fine`) stay legal prose. Corpus case 031.
		- Opened: 20260725-152141
		- Closed: 20260725-173852

	- ✅ Item 12: a write to an unusable path silently succeeds and can leave a half-created path behind.
		- Cause: `place()` creates intermediates as it walks, then returns nothing on a wildcard or a missing `[#N]` - the nodes it already made stay.
		- Reproduced: `--set=server[*].port=1`, `--set==v` and a typo'd path all print the untouched document and exit 0.
		- For the mechanism the spec designates as the override route, a typo that silently does not apply is the worst available failure mode.
		- Fixed: `place` pre-validates the whole path before creating anything (wildcard, missing `[#N]`, value part, past the depth cap). Every setter reports whether the write applied, and the CLIs exit 1 on a rejected op or `--set` instead of printing the untouched document.
		- Opened: 20260725-152141
		- Closed: 20260725-170306

	- ✅ Item 13: the Writer can create two siblings with the same name and value, so its output is not a formatter fixpoint.
		- Cause: the Writer deliberately skips the parser's merge index and nothing else applies the merge rule, so re-reading its output collapses the pair and loses an instance.
		- The existing fuzz misses it because it always starts from an empty document and never has a sibling to collide with.
		- Fixed: after a value write, a merge-rule collision with a sibling folds the pair the way a reparse would (earlier survives; children and trivia fold in), in all four bindings.
		- Opened: 20260725-152141
		- Closed: 20260725-170306

	- ✅ Item 14: the C CLI caps `--layer` and `--set` at 64 each; the other three are unbounded.
		- Reproduced: past 64 the C CLI exits 1 with empty stdout while the others exit 0 and print the merged document.
		- The cap appears in neither the spec nor the usage text, on an option the spec expects programs to generate in bulk.
		- Fixed: layers, sets, and positional args all grow dynamically now (the fixed 64/65/8 arrays are gone); verified with a 70-layer merge.
		- Opened: 20260725-152141
		- Closed: 20260725-170306

	- ✅ Item 15: `shcl_datetime_str` writes past its documented 64-byte buffer.
		- Cause: `frac` is a public field with no cap and is `memcpy`'d unbounded; the public `shcl_set_datetime` passes a 64-byte stack array.
		- Not reachable from parsed input, since the parser bounds every component, so this is a defensiveness gap in a public API, not an input-driven hole.
		- Fixed: the frac copy is capped so the render always fits the documented 64 bytes; header doc states the truncation.
		- Pinned since 20260831 by the C runner's `dt_clamp` fixture, with the same round's item 7.
		- Opened: 20260725-152141
		- Closed: 20260725-170306

	- ✅ Item 16: option validation is per-subcommand for two options and global for everything else.
		- Reproduced: `shcl set --write FILE` is accepted and discarded, so a user copying the habit from `fmt --write` gets exit 0 and an unmodified file.
		- Also silently ignored: `--schema` on `get`, `--slots` on `count`, `--int` on `fmt`.
		- Either implement `set --write` or reject it; silently dropping it is the one case that must not stay.
		- Fixed: `set --write` is implemented (atomic in-place rewrite, `-` rejected), and every subcommand now validates its options against an allow-list - an option it does not use is a usage error (exit 1), in all four CLIs.
		- Opened: 20260725-152141
		- Closed: 20260725-170306

	- ✅ Item 17: `check --schema` cannot tell a schema line number from a document line number.
		- Cause: both files' diagnostics interleave in one list with nothing marking which file a line belongs to.
		- `init` already prefixes `schema line N`, so the same fault renders two ways in two commands.
		- Fixed with the cheap half: the stderr prose spells `schema line N` for `V090`-`V093` (whose numbers are schema-file lines per the code table) in all four CLIs; the compared stdout keeps the uniform form since the code already names the space. The structural `source`/`column` fields stay future work.
		- Opened: 20260725-152141
		- Closed: 20260725-171744

	- ✅ Item 18: `Load(defaults, site, user)` is documented but exists in no binding.
		- Cause: README presents it as the layered-loading API; only `merge(base, over)` exists, so a reader's first call does not compile.
		- README also advertises environment overrides, which were deliberately dropped, and its Status block still lists three finished features under "not done yet".
		- Same class as the prior review's item 6, regenerated by the newest features.
		- Fixed: the README's layered-loading bullet now shows the real `merge(base, over)` fold and the CLI `--layer`/`--set` stack, notes the deliberate no-env-mapping stance, and the Status block lists the three finished features instead of calling them not-done.
		- Opened: 20260725-152141
		- Closed: 20260725-175440

	- ✅ Item 19: the spec's ergonomic-tier table does not compile for C++ and misstates the C signatures.
		- Cause: `doc.get_or<int>(...)` and `doc.get<int>(...)` fail as a link error, the least actionable diagnostic a junior can get, because `get<T>` is specialized for only four types.
		- Cause: the C rows drop the `plen` argument and name `shcl_get_int_ex`, which does not exist.
		- This is the table the spec points a junior at.
		- Fixed: the C rows show the real length-delimited signatures (`shcl_get_int(doc, path, plen, 0)` / `shcl_read_int`), and the C++ rows use `int64_t`. The veneer's `get<T>` primary template now carries a static_assert so an unsupported `T` (a bare `int` included) fails with a readable message instead of a link error.
		- Opened: 20260725-152141
		- Closed: 20260725-175440

	- ✅ Item 20: Go and Python `clone_subtree` share element storage with the `over` document instead of copying it.
		- Latent only, since no public API mutates a value in place after parse today, but the docstrings and spec both say the content is copied.
		- One line per port, and exactly the structural drift the parity rule exists to prevent.
		- Fixed: the Go clone copies the element backing array, the Python clone builds a fresh value (elements included), matching the reference and C.
		- Opened: 20260725-152141
		- Closed: 20260725-162837

	- ✅ Item 21: C and C++ `generate()` give no way to see which schema line is at fault.
		- Cause: `shcl_generate` signals failure with a bare `ok` flag and discards the fault list the other three return, so C `init` prints only the summary.
		- Reachable today through `shcl_validate` against an empty document, so the CLI can be fixed without reshaping the API.
		- Fixed via the review's own suggestion: on a generation failure the C CLI validates an empty document against the schema and prints the reproduced V09x fault list; the veneer documents the same trick. No API reshape.
		- Opened: 20260725-152141
		- Closed: 20260725-171744

	- ✅ Item 22: the same schema fault exits 1 through `init` and 6 through `check --schema`.
		- Exit 1 is documented as "usage or I/O error", which a semantically broken schema is neither.
		- A pipeline that reads 1 as "invoked wrong" and 6 as "config is bad" gets the wrong answer from `init`.
		- Fixed: `init` exits 6 for a schema that fails to load or has faults, in all four CLIs; exit 1 stays usage/IO.
		- Opened: 20260725-152141
		- Closed: 20260725-171744

	- ✅ Item 23: `shcl.h` does not compile as a drop-in under `g++ -Wall -Wextra -Werror`.
		- Cause: the bare `{0}` initializers trip `-Wmissing-field-initializers` in C++ mode; the file is already inconsistent, spelling one of them out in full.
		- The veneer gate compiles with `-Wall -Werror` only, so the repo's own build hides it.
		- Fixed: the implementation section suppresses `-Wmissing-field-initializers` for C++ TUs only (the `{0}` zero-init idiom is correct C; C++ -Wextra flags every one), and the veneer gate now compiles with `-Wextra` so the repo's own build proves it.
		- Opened: 20260725-152141
		- Closed: 20260725-175440

- Code review 20260716:

	- Items 1 to 3, 7 to 17, 21, 23 to 27, 30 and 32 to 34 are here; 4 to 6, 18 to 20, 22, 28, 29, 31 and 35 to 38 are under Done - Features and enhancements.

	- ✅ Item 1: C CLI reads freed memory on typed array output.
		- Reproduced: `get --int|--float|--datetime --array` with more than 8 elements prints from a stale pointer after the line buffer grows; large arrays segfault.
		- Fixed: owned line entries no longer store a pointer into the growable array; corpus case 008 pins 10-element typed arrays of every kind.
		- Opened: 20260718-165550
		- Closed: 20260718-174231

	- ✅ Item 2: Rust parser panics on a multibyte char in the timezone tail of a datetime value.
		- A garbled or hostile config aborts the consumer (exit 134) instead of returning BadType.
		- Fixed: zone tail is now checked byte-wise, so no str slice can fall mid-char; corpus 007 `bad5` pins BadType across all bindings.
		- Opened: 20260718-165550
		- Closed: 20260718-174231

	- ✅ Item 3: wildcard array reads swallow per-slot NotFound/BadType.
		- Reproduced: a missing sub-path yields a silent zero with status Good - the exact trap the fallback design exists to prevent.
		- `count` and `instances` also disagree on the same wildcard path, breaking index alignment.
		- Fixed in all four bindings: array reads carry per-slot statuses, aggregate = worst slot, `instances` keeps unresolved slots as "", CLI grows `--slots` and per-slot `--default` substitution. Spec pinned, corpus case 009 + a slots column in reads.tsv, crosscheck replays `--slots`.
		- Opened: 20260718-165550
		- Closed: 20260718-175628

	- ✅ Item 7: `fmt` ignores `--strictness`.
		- Fixed: `fmt` loads at the requested strictness in all four CLIs; strict failure exits 6 with diagnostics, no output. Crosscheck now replays `fmt` at each `load` row's level.
		- Opened: 20260718-165550
		- Closed: 20260719-150545

	- ✅ Item 8: mixed `*`/field child lines silently build a block array.
		- Fixed: uniform-or-nothing enforced in all four parsers - first mixed field diagnoses an Error (field kept), later `*` lines under that parent are Errors and dropped. Corpus case 010.
		- Opened: 20260718-165550
		- Closed: 20260719-150545

	- ✅ Item 9: `field[disc]` matches differently at parse-time vs query-time.
		- Fixed: parse-side selectors match the display form like queries do; create only when nothing matches. Corpus case 011; spec's selector bullet updated.
		- Opened: 20260718-165550
		- Closed: 20260719-150545

	- ✅ Item 10: raw-block merge identity ignores the info-string.
		- Decided: info-string is part of a block's identity (fence style is not); equal bodies with different infos stay two instances. All four parsers; corpus case 012; spec updated.
		- Opened: 20260718-165550
		- Closed: 20260719-150545

	- ✅ Item 11: reading an array as a string drops quoting and escapes.
		- Fixed: array-as-string is the canonical inline form (minimal quoting, escapes intact, re-parses to the same array) in all four bindings. Corpus rows in case 011; spec's Strings section updated.
		- Opened: 20260718-165550
		- Closed: 20260719-150545

	- ✅ Item 12: parse time is quadratic in siblings.
		- Fixed: per-parent (name, value-key) lookup map in all four parsers; sibling scan and hint grouping are linear now. A large flat file went from minutes to under a second in the reference, and to a few seconds in Python.
		- Opened: 20260718-165550
		- Closed: 20260719-163626

	- ✅ Item 13: Python formatter recurses and crashes on deep nesting.
		- Fixed: emit walks an explicit stack; the CLI recursion-limit bump is gone. Depth 25000 formats fine from both the CLI and library callers.
		- Opened: 20260718-165550
		- Closed: 20260719-163626

	- ✅ Item 14: PowerShell wrapper exits 0 when the resolved binary will not launch.
		- Cause: resolution accepts any plain file (no executable check); a stale non-executable `shcl` yields empty output and exit 0.
		- Fixed: every ps1 resolution site now goes through `_shcl_executable` (Unix requires an execute bit, Windows keeps a bare leaf); the run-path passthrough is `exit ($LASTEXITCODE ?? 1)`.
		- Opened: 20260718-165550
		- Closed: 20260721-104508

	- ✅ Item 15: crosscheck cannot see trailing-newline differences.
		- Cause: command substitution strips them before compare, so a binding that drops or doubles the final newline passes green.
		- Fixed: capture helpers append a trailing sentinel so `$()` has nothing to strip; a dropped or doubled final newline is now a divergence.
		- Opened: 20260718-165550
		- Closed: 20260721-105049

	- ✅ Item 16: crosscheck passes with zero comparisons.
		- An empty fuzz dump or a corpus layout change silently drops most of the comparisons and the gate still passes.
		- Fixed: exits 2 when no comparison ran, when `--extra` matches no `*.shcl`, or below an optional `--min N` floor.
		- Opened: 20260718-165550
		- Closed: 20260721-105049

	- ✅ Item 17: demo gif generator ignores step exit codes.
		- A renamed flag renders the error text into the gif and the pipeline publishes it onto the committed asset.
		- Fixed: a step whose exit differs from `expect_exit` (default 0) aborts the render, so cicd skips the publish `cp`.
		- Opened: 20260718-165550
		- Closed: 20260721-105049

	- ✅ Item 21: `fmt --write -` silently drops `--write` and exits 0.
		- Should be a usage error pointing at piping stdout instead. Same in all four CLIs.
		- Fixed: `--write` with stdin is a usage error (exit 1) in all four CLIs; `--write FILE` still rewrites.
		- Opened: 20260718-165550
		- Closed: 20260721-110039

	- ✅ Item 23: `field[sel]: value` is grammar-legal but has no spec'd meaning.
		- The value is dropped with an Error diagnostic, so strict loads fail on a line the grammar allows. Align spec, grammar, and code.
		- Fixed by spec'ing the code's existing behavior: a value after a last-segment selector is defined as an `error` (instance kept from the discriminator, value dropped; fails Strict). spec.md Selectors + grammar.abnf note updated; corpus case 018.
		- Opened: 20260718-165550
		- Closed: 20260721-111149

	- ✅ Item 24: invalid-UTF-8 command-line args abort the reference.
		- Reproduced: Rust exits 134 (panic); Go/Python/C all exit 3. The reference is the outlier on its own exit-code contract.
		- Fixed: all four validate argv as UTF-8 up front and exit 1 (`invalid argument encoding`); the reference uses `args_os` instead of the panicking `args`.
		- Opened: 20260718-165550
		- Closed: 20260721-110039

	- ✅ Item 25: broken stdout pipe gives three different exit codes.
		- Reproduced: `shcl fmt big | head`: Rust 134, Go 141, Python 0. Pick one behavior and pin it.
		- Fixed: uniform die-by-SIGPIPE (141). Rust and Python restore the default SIGPIPE disposition; Go and C already died by signal.
		- Opened: 20260718-165550
		- Closed: 20260721-110039

	- ✅ Item 26: C CLI has unchecked `realloc` on input/output paths.
		- OOM segfaults instead of taking the clean exit-70 path the arena already has.
		- Fixed: every CLI allocation goes through `xrealloc`, which now exits 70 with the library's message on OOM.
		- Opened: 20260718-165550
		- Closed: 20260721-110039

	- ✅ Item 27: crosscheck skips the last `reads.tsv` row if the file lacks a trailing newline.
		- One-line `|| [[ -n "$query" ]]` guard fixes it.
		- Fixed with exactly that guard on the read loop.
		- Opened: 20260718-165550
		- Closed: 20260721-105049

	- ✅ Item 30: NUL-joined merge key conflates distinct values.
		- Reproduced: `x: a, b` and `x: "a<NUL>b"` merge to one instance; the second value is silently lost. Make the key injective.
		- Fixed in all four parsers: each cell element (and the raw info-string) is length-prefixed, so the key is injective. Corpus case 017 (count = 2) pins it; the crosscheck skips it (bash can't carry a NUL) and the native runners do the pinning.
		- Opened: 20260718-165550
		- Closed: 20260721-111149

	- ✅ Item 32: wrappers invoked via symlink lose the sibling-binary and repo-build fallbacks.
		- Cause: both wrappers compute the script dir without resolving links; resolve the real path first.
		- Fixed: bash follows `${BASH_SOURCE[0]}` through symlinks by hand (bare `readlink` loop, POSIX); ps1 resolves `$PSCommandPath` via `ResolveLinkTarget` into `$script:_SHCL_ROOT`.
		- Opened: 20260718-165550
		- Closed: 20260721-104508

	- ✅ Item 33: ps1 header's own usage example assigns to read-only `$host`.
		- Copying the documented example fails; rename the example variable.
		- Fixed: header example now uses `$svrhost`.
		- Opened: 20260718-165550
		- Closed: 20260721-104508

	- ✅ Item 34: ps1 `SHCL_BIN` probe skips the `.exe` fallback its header promises.
		- Route the pin through the same `_shcl_exe` helper the other probes use.
		- Fixed: the `SHCL_BIN` pin resolves through `_shcl_exe`, so a base name matches its `.exe`.
		- Opened: 20260718-165550
		- Closed: 20260721-104508

#### Done - Features and enhancements

- ✅ `install-dev.bash --hooks-only`, and a gate over its hook setup.
	- The hook setup was the one piece of that script nothing exercised: the toolchain installs in front of it cannot run in a gate. Noted as still ungated when the test-gap round closed.
	- Fixed: `--hooks-only` skips the toolchain and just points git at the tracked hooks. Useful on its own for a box that already has the tools, and it makes the tail runnable. An explicit `--dir` wins over in-clone detection under it, so it can be aimed at any clone.
	- Pinned by: `check-install-dev.bash`, on a throwaway local clone - both configs set, a second run changes nothing, a chosen `core.sshCommand` survives, the in-clone path works, and a directory that is not a clone is refused.
	- Opened: 20260901-114703
	- Closed: 20260901-114703

- ✅ The Windows installers' PATH handling is tested against a real registry.
	- Noted as still ungated when the test-gap round closed: it needs a registry, which only the hosted windows job has.
	- Fixed on the way in: `install.ps1`'s two PATH edits are one function now, and the setup's PATH script is a real file (`cicd/packaging/shclpath.ps1`) packed into the installer instead of line-by-line writes from the .nsi - so the text that ships is the text the gate runs.
	- Pinned by: `winpath-regress.ps1`, run by `win-runners.bash` on the hosted windows job against the runner's own Environment keys, saved first and restored after. What it holds: `%VAR%` references survive unexpanded, the value stays REG_EXPAND_SZ, segments compare whole (a superstring dir still appends), add is idempotent, and remove takes exactly its own segment. Later widened to a PATH ending in `;` (the add does not double it, the remove drops it) and an empty one (the add leaves no leading `;`).
	- Opened: 20260901-114703
	- Closed: 20260901-114703

- ✅ Close the remaining test gaps across the recent review rounds.
	- The earlier pass built three gates and left the rounds' own item list unaudited. This one read every closed item from 20260829, 20260830 and 20260830b and asked what would fail if the fix were backed out.
	- Most were already covered, several by a fixture whose item text never named it. Four behavior gaps were left, and each now has a row: `-h` after FILE, load diagnostics on stderr without `--write`, `--set` splitting at an `=` inside a selector, and a pre-release suffix ordered numerically rather than as text in both installers.
	- A symlink cycle joins the file tier's fixtures in all four runners. The save has to fail and say why, and must not "fix" the cycle by dropping a regular file over one of the links.
	- Not reachable and deliberately left: prose and wording items, and the Python CLI's Windows stdout encoding, which only the hosted Windows job can exercise.
	- Closed: 20260830-215127

- ✅ Regression tests for the fixes of the last three review rounds.
	- Those rounds closed 120 items between them and left three corpus cases behind, so most fixes had nothing pinning them and a later round kept re-finding the same classes.
	- Two corpus cases for defects a corpus can carry: a `set_int_default` after a `remove` (the stale name index), and a `set_raw` info string holding an unquoted `#`. Both fail on all four bindings with their fix backed out.
	- The set-id bits joined the file-tier fixture in all four runners: a mode applied before the data lets the kernel clear setuid and setgid.
	- `cli-regress.bash` pins eleven CLI behaviors the corpus structurally cannot reach - closed stdin and stdout, `-` named twice on one command line, a carriage return ending an ops line, the shape of an op-script error, whether a read failure still names its cause, and a document nested to the depth cap. Every row runs against all four bindings and is matched against a fixed expectation, not against the other bindings.
	- `perf-gate.bash` times bulk writes and absent-path defaults against the same binding's parse-only baseline, so it carries no wall-clock constant. The two superlinear write regressions of the last fortnight came in at 15x and 160x over budget.
	- `shell-regress.bash` covers the wrappers, the one-liner's scope hygiene, and scans every errexit script for the trap that has now bit four times: a `grep` in an assigned command substitution with no `|| true`.
	- All three gates are in the test stage, so `--ci` and the hosted gate run them.
	- Opened: 20260830-145320
	- Closed: 20260830-163000

- ✅ Run the four runners on Windows in CI.
	- The file tier now has real platform-specific code: the publish step differs by OS, and the create-versus-overwrite fixture in every runner was widened to exercise both paths on every platform. Nothing in the pipeline ran any of it on Windows, so the fixture only fired when someone loaded the repo there by hand.
	- The gap it closes is proven, not hypothetical: the Python binding threw on every Windows overwrite and no gate here could see it.
	- Done: a second `windows` job in `ci.yml` runs `cicd/utility/win-runners.bash` under Git Bash - the runners plus the veneer smoke, and nothing else. The corpus goldens, crosscheck, large document and every lint gate stay on the Linux job, which is where they are defined. gcc is the compiler because MSVC has no `dirent.h` for the C runner to walk the corpus with.
	- Done: `.gitattributes` turns off end-of-line conversion. Git for Windows defaults `core.autocrlf` on, which would rewrite every golden on checkout - and the corpus deliberately holds cases whose line endings are the thing under test.
	- Two real defects found on the way, both in test code. The C runner would not compile for Windows at all (POSIX two-argument `mkdir`, and a temp root that only knew `TMPDIR` and `/tmp`). The Python runner seeded its file-tier fixtures in text mode, which would have fed different bytes there than everywhere else.
	- Verified: Rust, Go and C were confirmed green on the Windows path before this went in. Python could not be checked the same way, and is also the one that had already broken there.
	- The first run justified the item three times over.
		- It caught a real library defect: `save_file` on a path holding a NUL raised `ValueError` straight past a contract that promises a returned message. POSIX raises at the resolve; Windows raises later, at the first call that touches the filesystem.
		- It also caught two test files that would not compile there at all. The C++ veneer smoke had the same POSIX two-argument `mkdir` as the C runner.
		- The C runner tripped a truncation diagnostic that gcc 15 raises and gcc 14 does not, so that one was never Windows-specific, just unseen.
	- Opened: 20260820-154417
	- Closed: 20260821-114838

- ✅ Test every binding against a document far larger than the corpus.
	- Nothing in the pipeline parsed more than a few kilobytes: 56 corpus cases of a few hundred bytes each, and fuzz inputs smaller still. A parser going quadratic, or a buffer that only misbehaves past a few megabytes, had no gate at all - and this project has already released one buffer-growth defect that no small case could see.
	- Done: `cicd/utility/largedoc.bash` generates a document (100 MiB by default, `LARGEDOC_MIB` in `config.bash`), formats it through all four bindings, and requires them to agree on the result byte for byte. It also holds each binding to a wall-clock and peak-RSS ceiling, checks that formatting is a fixpoint at that size, and reads back a 20000-element array whole.
	- Done: it runs in the tests stage after the crosscheck, in full local runs and in the GitHub gate. `--quick` skips it, since the fast loop is for the small cases, and `--no-largedoc` skips it outright.
	- The generated document is structured, not padded: repeated instances of one name, nesting, inline and bullet arrays, quoted values holding the separator, raw blocks, comments, blank lines and non-ASCII.
	- The measurements it produced are the item below.
	- Opened: n/a
	- Closed: 20260819-100700

- ✅ Cut what a document costs in memory, and what Python costs in time.
	- The gate above is the first thing to measure either. A large document cost tens of times its own size in memory in every binding, C the worst.
	- Done: the multiplier roughly halved in every binding. Three shared cuts. The parser's accelerator maps key on hashes and verify against the tree instead of storing built key strings. Comment trivia moved behind a per-node pointer most nodes never allocate. The authored name and source value spellings are stored only when they differ from what the node already holds.
	- Done: C got three more of its own, taking it from the heaviest binding to the second lightest. The node vector and map slots moved out of the bump arena, which cannot reclaim a doubling. Strings slice one retained copy of the input instead of duplicating each piece. The repeated-leaf hint pass stopped leaving its dead bookkeeping in the document arena.
	- Every binding also got faster.
	- Python's time half moved only a little. The cost is spread across the interpreter's per-line work with no single hot spot left, so a real cut would mean restructuring the parser away from the reference's structure. Left there deliberately: the Python CLI exists for the differential check and stays well inside its gate ceiling.
	- Done: the largedoc gate's memory ceilings were lowered to match, so the gains cannot silently regress.
	- Verified: confirmed independently by the format comparison below, which measured the reference on four document kinds and put a number on the time half too. Its published numbers predated this work and were refreshed by the rerun item below.
	- Opened: 20260819-100700
	- Closed: 20260821-150025

- ✅ Rerun the format comparison and refresh the README performance numbers before the next cut.
	- Done: a full two-tier run at the stress size, recorded in results.shcl; README tables and prose refreshed, design.md's summary paragraph too. Numbers only; the wording moved just where a claim would otherwise have gone false.
	- The stress read time and peak memory both halved, so SHCL now sits below TOML and YAML on memory and at a quarter of `toml_edit`.
	- The like-for-like pair moved apart: the Rust binding gained more against `toml` than the Python one did against `tomllib`, so the closing sentence now says "the same few-fold gap" rather than "similar gap".
	- Opened: 20260821-150025
	- Closed: 20260821-183607

- ✅ A man page and shell completions for the CLI.
	- Split out of Code review 20260817 item 28, which batched them with polish they do not belong with: this is a new deliverable, not a fix.
	- `shcl.1` in roff, plus bash and zsh completions that know which options each subcommand takes - the same table the CLIs already carry for their usage check, so the two can be kept in step.
	- Packaging follows: the .deb and .rpm need a man dir and a completions dir, and the installer needs somewhere to put them for a user-target install.
	- Done: `source/man/shcl.1` covers every subcommand, option, write op and exit code, with the per-subcommand ownership the help only hints at. No version string in it, so the release bump is still eight files.
	- Done: `source/completions/shcl.bash` and `source/completions/_shcl` carry the CLI's own option table, one arm per subcommand, spelled identically in both files. They complete values for `--strictness` and `--on-bad`, files for `--schema`/`--layer` and the FILE slot, and nothing for a PATH - no filename there could ever be right.
	- Done: `cicd/utility/check-completions.bash` diffs the CLI's table against both completion files and fails the lint stage on any disagreement, so an option added to one cannot drift from the others.
	- Done: the .deb and .rpm install the man page and both completions into the distribution's own directories, so they work with nothing to configure. The installer symlinks the man page into the target's man1 dir and leaves the completions under the install dir with the line to paste for each shell - the reasoning is in `design.md`. Uninstall removes both, and the man symlink only when it points back into the install dir.
	- Done: the man page and completions ride in the signed drop-in payload, so nothing unverified is installed. A payload from before they existed installs what it has and says so.
	- Opened: 20260818-165540
	- Closed: 20260819-084518

- ✅ Measure SHCL against JSON, YAML, TOML and XML.
	- The front page compares the five on features and says nothing about what any of it costs, which leaves the obvious question unanswered and the obvious objection unmet.
	- Done: `cicd/utility/comparison/` - a Rust tool and a `compare.bash` launcher. It builds one document per kind holding the same data in every format, loads each with its own ecosystem's crate, and reports file size, gzipped size, parse and emit time, peak memory and whether a load-and-save gives the file back.
	- Six document kinds, because one kind hides most of what separates these formats. Four scale to whatever size the run asks for: long and flat, wide and deep, an array of records, and multi-line text blocks.
	- The language is held constant at Rust so the comparison is between formats rather than between implementations. TOML and XML each get two libraries, one that keeps only the data and one that keeps the file, since only the second is doing the job SHCL does.
	- A pre-flight check makes every library parse its own file and find the same number of scalar values, so a size or speed number can never come from documents that are not the same data.
	- Results accumulate in `results.shcl`, written and pruned through this repo's own library and read back with its own CLI. Detailed enough to re-derive any published figure, and it keeps the newest runs only.
	- Deliberately not a pipeline gate: benchmarks are noisy and slow, and a red build caused by a busy machine teaches nothing. Reasoning and the full method are in `design.md` -> Format comparison.
	- Done: the README carries a simplified table under "How it compares to...". It is favorable on size and unfavorable on speed, and says so. The front page already tells people not to use SHCL for high-volume machine data, so the plain table costs nothing and answers the question a reader would otherwise have to guess at.
	- Found, and left for the item above: SHCL is many times slower to load than `serde_json` and holds tens of times the input size in memory. Both are the memory-and-speed item, now with a second measurement backing it.
	- Added after the first round: a Python tier, over the same documents, so the obvious objection (that this measures one implementation and calls it a format) has an answer.
		- Most of what Python reaches for is a C extension wearing a Python name, so the row that carries the weight is `tomllib`, the one other pure-Python parser. SHCL trails it in Python by about the same factor it trails `toml` in Rust.
		- That is the aggregate over four kinds, and it is the number to quote. The per-kind spread is wide in both tiers and does not track kind for kind.
		- Libraries that are not installed are skipped and named rather than failing the run.
	- Added at the same time: rows are ordered by the geometric mean of each library's parse time over every kind, fastest first, in the printed tables and in the results file alike. SHCL sorts last in both tiers. The number the order comes from is recorded beside each library, so it can be re-derived rather than trusted.
	- Turned up by the Python tier, about a library rather than a format: `lxml` goes quadratic on the long-and-flat kind, with or without `huge_tree`, so it is not the ceiling-lifting flag. Millions of distinct element names against libxml2's name dictionary is the likely cause. Nothing else in either tier does it, and the kinds whose names repeat are unaffected. Noted in `design.md`, not filed as our bug.
	- `lxml` also needs `huge_tree` on to accept a document this size at all; without it the parse fails outright. That is the fair setting, not a thumb on the scale - the ceilings guard against hostile input, and this input is ours.
	- Added later: the other two kinds carry their own realistic size instead of scaling: a hand-edited application config of a couple of kilobytes, and a schema definition of a few hundred. The stress sizes answer how a parser behaves at volume, which is not the question most readers have. A config file measured at the stress size is not a config file anybody has.
	- The result at those sizes: SHCL writes the smallest file of the five in both, well under JSON and XML on the schema definition, and reads either in a time nobody would notice. The README now shows all three sizes, smallest first, so the speed column can be read against a size somebody recognizes.
	- Small documents get proportionally more timed runs, since best of three says nothing when the parse takes microseconds, and the count actually used is recorded beside each kind.
	- Also fixed here: `check-wheel.bash` discarded the build's output, so a transient failure reported nothing but "build failed". The build stands up an isolated environment and can fail for reasons that have nothing to do with the package. It shows the tail of the log now.
	- Opened: n/a
	- Closed: 20260820-075114

- ✅ Windows saves go through `ReplaceFile`, and POSIX saves sync the directory.
	- A move publishes a new file, so on Windows the destination's ACLs, attributes and named streams were left behind on every save. `ReplaceFile` exists for this exact step and carries them onto the replacement. It needs a destination and fails rather than skip a merge it cannot do, so a create or a failure falls back to the replacing move, never worse than before.
	- Done: all four bindings, none of them taking a dependency for it: the reference declares the one function it needs, C already had `windows.h`, Python goes through `ctypes`. Go could not, since `shcl.go` promises to work when copied out on its own and cannot name a windows-only symbol - so the publish step became a hook and a small windows-only file swaps it in. Compiled, vetted and staticchecked in the cross stage, which is the only place that sees `GOOS=windows` at all.
	- Separately, the `fsync` on the file only ever promised the contents; the move is a directory change. All four now sync the directory after it, so a power cut cannot lose the publish and leave the old content. Best effort, POSIX-only.
	- What still does not survive a save is written down rather than papered over - other hard links, POSIX ACLs, xattrs and SELinux labels among them. Spec and `design.md`.
	- Opened: n/a
	- Closed: 20260820-154417

- ✅ `set --write` creates a FILE that does not exist yet.
	- `--write` names the file the command produces, so refusing a missing one was an obstacle rather than a safeguard: the workaround was to `touch` it first, the same act with an extra step.
	- Only `set --write`. `fmt --write` has nothing to format and still reports it missing, and a file that exists but cannot be read stays an error in both - the alternative is writing over something unread.
	- Done: all four CLIs, help text and man page moved together, and the four agree byte-for-byte on the create, both refusals and the unreadable case. Pinned in `crosscheck.bash` rather than a runner: this one is reachable from the CLI, and the tree compare covers the created file's mode as well.
	- Opened: n/a
	- Closed: 20260820-154417

- ✅ A quoted by-value selector is scalar-only.
	- From convert-base-v2, still open at v1.2.0: the scalar `"a, b"` and the list `a, b` met the same selector (display-form matching), so the read could only come back Multiple.
	- Done, all four bindings: `x["a, b"]` matches only a single-element value whose logical string equals the text; bare selectors keep the whole-display match, so existing paths behave identically. The quoted flag rides the scanner's selector, the parser accelerator gets a rare fallback scan, and the generator re-emits quoted selectors quoted.
	- Root cause of one C-only corpus failure en route: the flag was uninitialized on the bare path - fixed at the single declaration site. New case 050; spec selector prose updated in both places; decision in `design.md`.
	- Opened: n/a
	- Closed: 20260817-171503

- ✅ Hand-edited configs are structurally safe across a round trip.
	- From nemo-anywhere, their strongest ask: a malformed line was diagnosed and dropped, so a stray typo plus one settings change equaled a silently vanished hand-written line on write-back.
	- Done, all four bindings, split by what is provably safe. Content-malformed lines (unreadable at any position) are retained as inert trivia and re-emitted in place, still diagnosed. Lines the parser could read but not apply (bad indent, unusable selector, depth cap, dropped list elements) cannot be made inert, since re-emitted they could parse as live content. Those count into a new `LostCount()`, and `SaveFile` refuses while it is nonzero, with `SaveFileLossy` as the explicit override.
	- Verified: the fuzzer caught the one retention hole (a BOM-led line, rewritten by the file-start strip) - those count as lost. Long soak green.
	- Goldens 004 and 013 now keep their malformed lines; new case 049 pins retention in and out of blocks; fixtures in every runner. Spec (Diagnostics + File tier) and `design.md` updated. CLI `--write` behavior was left unchanged at the time, on the grounds that the stderr diagnostics are visible.
		- Superseded by Code review 20260817 item 8: the CLI `--write` now consults the save gate and refuses a rewrite that would drop a line unless `--lossy` is passed.
	- Opened: n/a
	- Closed: 20260817-122516

- ✅ File tier: `LoadFile`/`SaveFile` with a four-way status, atomic save.
	- From nemo-anywhere: every consumer that persists a config re-implemented the same load/save dance and repeated the same mistakes - absent confused with unreadable, torn writes.
	- Done, all four bindings + veneer. Load never fails (usable document plus Clean / HadErrors / NotFound / Unreadable status). Save writes canonical text through the atomic temp-and-rename that moved from the CLIs into the libraries, so the CLIs now call the same code and cannot drift. C guards the tier behind `SHCL_NO_FILE_IO` so the core stays free of file I/O.
	- Verified: fixture in every runner (missing file, directory, broken file, save round-trip); spec gained a File tier section; decision recorded in `design.md`.
	- Opened: n/a
	- Closed: 20260817-120726

- ✅ `AuthoredName(path)`: the field name as the author spelled it.
	- From convert-base-v2: names store folded, so a message could only echo `symbols` when the file said `SYMBOLS`.
	- Done, all four bindings + veneer: the parser and writer keep the as-authored spelling (unfolded, outer quotes stripped, escapes left as written; see Code review 20260817 item 15) beside the folded name. Resolution mirrors `Line(path)`, merged instances keep the first binding's spelling, a writer-built node keeps the setter path's. Fixture extended in every runner; spec Accessor section updated.
	- Opened: n/a
	- Closed: 20260818-154310

- ✅ H002 reports every merged level, and `reopen:` in a schema disavows it.
	- From nano-git-db: no way to declare "this section is meant to be re-opened", so every legitimate re-open hinted forever - and only the outermost merge reported, so filtering by hand waved nested merges through.
	- Done, all four bindings: the parser carries the re-open line down, so merges under a hinted container hint too, each naming its own earlier line; content the re-opened region itself wrote stays silent, and adjacent re-mentions stay silent as before.
	- Done: new schema key `reopen: true` (a dedicated key, not a `repeat` overload) disavows the hint by leaf name, dropped at `check --schema` and the one-shot exactly like the H001 suppression; a bad value is a V092 fault.
	- Verified: golden 001 gained the previously-invisible nested hint; new case 048 pins three-level reporting plus the disavowal. Spec vocabulary table and `design.md` updated.
	- Opened: n/a
	- Closed: 20260817-114152

- ✅ The unknown-field sweep survives schema faults.
	- From nano-git-db, round two of the same trap: v1.2.0 let surviving constraints check through faults, but the sweep still skipped on any fault, so their schema self-check probe was still required.
	- Done, all four bindings: only two fault classes actually lose a name chain, an unreadable `field:` path (V093) and a mount naming no declared fragment (V095), so the sweep now skips only on those. Every key-level fault keeps its entry, whose path still legalizes its chain.
	- Verified: case 046 gained the previously-invisible V001; new case 047 pins that a path-losing fault still holds the sweep back. Spec, `design.md`, and the veneer smoke test updated.
	- Opened: n/a
	- Closed: 20260817-112718

- ✅ NUL-transparency documented at the point of use, not just at the type.
	- From nemo-anywhere: the `shcl_str` typedef says the bytes may hold NUL; `shcl_to_canonical` did not, and the canonical getter is where the strlen mistake actually gets made. One doc line on the getter.
	- Opened: n/a
	- Closed: 20260817-105427

- ✅ `about` and `donate` on the CLI, and blank-line padding around the outputs a person asks for.
	- Asked for: an `--about` like the other projects' one, and a `--donate` naming the sponsors page.
	- Done: both take either spelling, `shcl about` or `shcl --about`, matching how `help` and `version` already work. `about` prints the version, copyright, project home, license with its SPDX link and a no-warranty line, then a short description of what SHCL is. `donate` points at the sponsors page and says a star, a good bug report or a mention count too.
	- Both are stdout, so both are byte-for-byte contracts across the four CLIs, and both spellings are pinned in the differential check.
	- Done: `help`, `about` and `donate` now print a blank line above and below, so the block stands clear of the prompts either side. Bare `shcl` keeps printing the same help text unpadded, since it is a usage error rather than something asked for, and `version` stays a bare line for capture.
	- Found en route: C's option-skip list was missing `--set-literal`, so `shcl get --set-literal -h FILE PATH` answered with the help text where the other three read the value. Predated this change; fixed with it.
	- Opened: n/a
	- Closed: 20260804-170930

- ✅ A plural line accessor: a repeated field is where a consumer most wants to cite a line, and the one case `line()` returned nothing for.
	- Reported from convert-base-v2, which skipped line-pinning its config errors over this. `line()` returns 0 unless the path resolves to exactly one node, so a repeated field, resolved as many, got 0, even though the parser's own repeat hint for that field carries a line.
	- Done: `lines(path)` in all four bindings plus the veneer, mirroring `instances()`. File order, unresolved wildcard slots as 0 so indices keep matching `count()`, a miss is the empty list. The same fixture was extended in every runner.
	- A line-bearing `children()` variant was skipped: `children()` gives the names and `lines(parent.name)` gives that name's lines, so the composition already answers the walk-a-section case without a second parallel-array accessor.
	- Opened: 20260804-143741
	- Closed: 20260804-153849

- ✅ A schema fault suppressed all data validation; it now validates with the surviving constraints instead.
	- Reported from nano-git-db as "a schema fault silently disables all data validation, so a broken schema looks like a clean file". They added a schema self-check test as a workaround.
	- Not literally silent - verified: every schema fault is an Error diagnostic, `validate()` returns them, and `check --schema` exits 6. The file only looks clean if the caller drops the schema-line diagnostics.
	- The substantive gap was real though: one broken constraint turned off validation for the whole file, even though the schema builder already drops broken constraints one by one before deciding to bail.
	- Done, as a three-way split. Faults report first, the surviving constraints still check the document, and only the unknown-field sweep needs a fault-free schema - a dropped constraint would turn its own fields into false unknowns. Generation still fails on any fault.
	- Verified: all four bindings, case 046 pins it, and goldens 023/040 turned out to be unchanged (their survivors trigger nothing).
	- Opened: n/a
	- Closed: 20260804-175129

- ✅ The README installed the CLI six different ways and never once showed it running.
	- Every code example was library code. A reader who took the packages, the installers or the prebuilt binaries had nothing telling them what the command actually does, and the demo gif was the only place the CLI appeared at all.
	- Done: a `Using the CLI` section between the file example and the per-language examples. It shows typed reads with a fallback, `count`/`instances`/`[*]` over repeated fields, a broken line reported while the rest of the file still loads, schema validation naming the field you meant, and `init` writing a commented starter file. The transcripts are verbatim output.
	- Done: the write-half notes moved out of the C subsection, where they had ended up by accident, into their own section at the end of the examples. Its after-save file is now the real output rather than an abridged one, which also fixed inline comments that had gone stale against the input above it.
	- Done: Features gained the three capabilities the list had been silent on: querying repeated fields, stable diagnostic codes, and the writer keeping comments attached across a save.
	- Done: the GitHub About blurb was a paragraph; it now matches the tagline and fits the width the card actually shows. Sponsorship is mentioned once in the support section and once in `contributing.md`, both times as an aside.
	- Opened: n/a
	- Closed: 20260804-102729

- ✅ A generated config file said nothing about what format it was in, so whoever opened it next had no way to find the syntax.
	- Done: `shcl init` ends the file with a short comment footer naming the format and linking its home page and spec, after a blank line. `--no-banner` leaves it out, and `generate` takes the same flag in every binding plus the C++ veneer.
	- The flag is negative so the footer is what a caller gets by saying nothing - the opt-out is the thing that has to be asked for.
	- The `Legal` line names SHCL as its subject ("SHCL is Copyright ...") rather than opening with the copyright, so it cannot be misread as a claim over the config it sits in.
	- Verified: the footer is output, so it is a byte-for-byte cross-binding contract like the annotation line. The three init goldens carry it; each runner also generates with the flag set and checks the result is the golden minus the footer, so the flag costs no extra goldens.
	- Opened: n/a
	- Closed: 20260804-092009

- ✅ A value that is not a plain scalar could not be written as an option: `--set` reads its value as data, so `ports=80, 443` stored one quoted string rather than an array.
	- Done: `--set-literal PATH=TEXT` reads the text as value syntax instead, the way the parser reads the half of a line after the colon, so the same text writes a two-element array. Backed by `SetLiteral`/`SetLiteralDefault` in all four bindings, a matching `literal` op in the write-ops script, and corpus case 044.
	- It parses the text rather than splicing it, so there is no way to inject syntax: the result is a value or a rejection, output stays canonical, and a written document is still a formatter fixpoint. Rejects only what could not be one line's value (a line break, or a quote that never closes - the same text the parser reports E017 for); an unquoted `#` ends the value as it would in a file.
	- Both spellings share one ordered list, so the last option to touch a path wins.
	- Investigated and deliberately not built: a "force this value to be a string" option. Quoting is a rule about canonical output, not a type marker: `fmt` normalizes `ver: "8"` to `ver: 8`, and values are typed by the reader, so there is nothing to force.
	- No C++ veneer change: the veneer exposes reads only, so adding one setter there would have been the odd one out.
	- Opened: n/a
	- Closed: 20260804-085353

- ✅ Persisting an edit from a shell needed a tab-separated op script on stdin, which is the root cause behind both shell wrappers' rough edges. Tabs are invisible in source and survive neither retyping nor an editor that expands them, and the PowerShell wrapper could not carry stdin at all.
	- Done: `set --write --set PATH=VALUE`, repeatable, applied in order, no pipe and no tabs. `--set` was already applied through the Writer on `set` rather than layered, so the edits were persistent in all but name - only the `--write` refusal stood in the way, and it existed for `--layer`'s sake.
	- Deliberately no new typed options: `--set` writes the value as literal config text, so `workers=8` already reads as an integer. A `--int`/`--string` family was compared against the op script and produced byte-identical output, so it would have been a redundant second way to do what `--set` already does.
	- Known gap at the time, now closed by the item below: an array value could not be written as an option, because a comma made it a quoted string.
	- Given any `--set`, `set` no longer reads stdin - otherwise passing edits as options blocks on the console, which is the hang this was meant to remove.
	- Opened: n/a
	- Closed: 20260804-082647

- ✅ The schema can't declare an open section. From TradeClanker: wildcards select instances (`server[*].port`), but there's no way to say "any child name under `indicators`, each built like this". So a config with one map-like section can't use schema validation at all, and they keep a hand-maintained known-paths map instead. Everything else in their config would express cleanly as a schema.
	- A name-position wildcard is the missing construct. Distinct from the schema-fragments item below (structure reuse), but they'd be designed together - both grow the schema language, so both go through the same design-first gate.
	- Done: bare `*` name segment in lookup paths, all four bindings. Works in schemas (per-child contexts, unknown-sweep pattern match) and in reads (slots like `[*]`); writer refuses it; quoted `"*"` stays a literal name; document lines unchanged. Cases 037/038; spec + design.md updated.
	- Opened: 20260802-110332
	- Closed: 20260802-150827

- ✅ Schema fragments. From nano-git-db, the top feature request: the schema language can't express recursion or path aliases. Arbitrarily-nesting layout blocks are generated to a fixed depth, past which validation silently stops and correct keys start reporting as unknown. Wrapped vs flat spellings force dozens of lines of string surgery over their own schema file at init. Both are the same missing idea, "this subtree is built like that one", and a single fragment/reference construct closes both, taking dozens of hand-written field entries plus two generators down to something reviewable.
	- Big. Design first, post-1.0. Weigh hard against keeping the schema language small, but two independent workarounds in the most rigorous consumer argue it earns its keep.
	- Done: `fragment: <name>` declares (children are ordinary `field:` instances with relative paths), `inherits: <name>` mounts, all four bindings. Recursion/mutual refs legal with no depth limit (demand-driven expansion); `init` expands mounts and cuts where a fragment re-enters; H001 disavowal covers fragment-declared repeats; new V094/V095 faults. Cases 039-041; spec + design.md updated.
	- Opened: n/a
	- Closed: 20260804-080545

- ✅ Lower the go directive to the tested floor.
	- Done: declared 1.20 (strings.CutSuffix is the newest stdlib dependency; everything else predates generics).
		- Hosted CI now installs a stable toolchain instead of reading go.mod, since the pipeline's own `go -C` needs a current release, while the go.mod directive gates the consumer floor.
		- From convert-base-v2: go.mod declares 1.24, and that declaration is their recorded reason for vendoring the file instead of using it as a module - it would drag their deliberately-1.21 project up.
		- The identical file compiles and passes their full suite under 1.21, so 1.24 is declaration, not need; find the real floor (generics suggest possibly 1.18) and declare that.
	- Opened: n/a
	- Closed: 20260802-131850

- ✅ Docs batch from the feedback round:
	- Done in the spec (Empty-vs-NotFound as an advertised full-tier feature; a "choosing [#i] vs [value] when mapping entities" bullet in the traversal section). Done in the Go package docs (a "writing a mapper" worked example: one-shot load, Count+[#i] iteration, Children for open sections, QuoteSegment, raw blocks). IndexOf not added; Count+[#i] covers the need without growing the API.
	- Advertise the Empty vs NotFound distinction. convert-base-v2 mapped it straight onto their tri-state marker convention with no adapter ("empty means explicitly disabled, absent means default") and called it rare among config parsers. It belongs in the README/spec as a feature, not something discovered by reading source.
	- A spec paragraph on choosing `[#i]` vs `[value]` selectors when mapping entities. By-value misreads an entity whose name is numeric and collapses two same-named entities. Since matching is against the display form, a scalar spelled `"a, b"` and the two-element list meet the same selector. nano-git-db and convert-base-v2 each worked these out the hard way; nano-git-db suggests an IndexOf(path, value) alongside.
	- A "writing a mapper" worked example in the Go package docs. The exported API is 60+ methods, and the pattern of a real consumer (descend by path prefix, Count then `[#i]`, schema validation for line-numbered errors, fences for verbatim) cost them most of a day to discover.
	- Opened: 20260802-105223
	- Closed: 20260802-130604

- ✅ Hosted CI runs its four pinned actions on a deprecated Node.
	- Fixed: all four re-pinned to current-Node releases, keeping the SHA-with-tag-comment form. Green on dev.
	- Cause: all four are pinned by commit SHA and target Node 20, which the runners now force onto Node 24 with a warning on every run.
	- Working today, and the warning is the only symptom. It stops working whenever the runners drop the shim.
	- Fix is to re-pin each action to a release built for the current Node, keeping the pin-by-SHA-with-tag-in-a-comment form.
	- Opened: 20260727-181445
	- Closed: 20260802-130604

- ✅ Add existence of PowerShell wrapper acknowledgement to design.md
	- Done: added to the Shell wrappers section: dual-mode design, no param block (verbatim $args), exit codes into $LASTEXITCODE, not in the differential (a forwarder).
	- Opened: 20260802-105223
	- Closed: 20260802-130604

- ✅ Glossary of terms
	- Done: covered by the spec's Terminology section, now extended with the wider nouns (trivia, raw block/fence, canonical form, layer, schema, corpus).
	- Opened: 20260723-134323
	- Closed: 20260802-130604

- ✅ Writer output has no blank lines between top-level sections.
	- Done, default on with no knob: writer-created top-level nodes set blank_before (the emitter never blanks line 1). Write goldens 014/016/029 regenerated; spec Writer section documents the layout. From TradeClanker: hand-written examples and writer output disagree on layout, so they post-process the library's output with string surgery - which is what using the writer was meant to end. Blank-before already exists as parsed trivia, so the writer setting it when it creates a new top-level node (or a knob to) keeps the fixpoint property intact.
	- Write-corpus goldens would churn; decide the default deliberately since written output is contract.
	- Opened: 20260802-110332
	- Closed: 20260802-123956

- ✅ to_canonical() drops blank lines between comment-only regions. It shouldn't do that.
	- Fixed: each held comment records its own preceding blank (runs still collapse to one; never as first output line). Blanks between comment regions (leading, merged, and end-of-file) now round-trip.
	- Opened: 20260802-105223
	- Closed: 20260802-123956

- ✅ to_canonical() also loses comment indentation two ways, from the SilkTerm devs: a comment run trailing a block's last child re-attaches to the following node and de-indents to column 0, and orphan comments after the last binding always emit unindented. Together with the blank-line item above, fixing these lets them delete most of their save-repair pass.
	- Fixed structurally rather than by storing verbatim indent: a comment written deeper than the next binding hangs on the block it sits in (new after-trivia, emitted after the block's last child at the block's indent). The same rule keeps indented tail-of-file comments with their block. Corpus case 034 pins it; fmt stays a fixpoint under the fuzz soak.
	- Means recording indent (and probably original attachment) as trivia; fmt must stay a fixpoint. All four parsers + emit, golden churn expected.
	- Opened: 20260802-105223
	- Closed: 20260802-123956

- ✅ Expose whether a value was quoted.
	- Done: `quoted` on the read result (rust/go/python; C read structs stay value+status by design) - true for a quoted single scalar element, false for arrays/raw/empty.
		- From nano-git-db: `a: @null` and `a: "@null"` read identically, so a language built on shcl can't reserve a sentinel value and still let a user write it literally. They had to make `@null` unconditionally reserved and walk back docs that promised quoting would escape it.
		- The parser already tracks quoted per element and drops it at the read boundary; one field on the read result makes "quoting is the escape" true for every downstream language.
	- Opened: n/a
	- Closed: 20260802-121147

- ✅ Line numbers on the read path.
	- Done both ways as filed: `Line(path)` accessor in all four bindings + veneer, and `line` on the read result itself (rust/go/python). 0 = unresolved or writer-built; merged instances cite the first binding, matching diagnostics. From nano-git-db: node line is populated and never exported, so any consumer check the schema can't express can only name the entity, never the line - their warnings degraded from `line 81: ...` to `table issue: ...`. A Line(path) accessor is the cheapest high-value ask in their list.
	- Second consumer, same gap (TradeClanker): parser diagnostics carry a line and reads don't, so half the errors a user sees cite a line and half don't. A bad value reports nothing while a malformed line one field above reports `line 12`. They want the line on the read result itself, not just a separate accessor; do both, it's the same code path.
	- Opened: n/a
	- Closed: 20260802-121147

- ✅ Enumerate a node's children.
	- Done: `Children(path)` in all four bindings + veneer: file order, duplicates included, empty path = top level; names as stored, QuoteSegment for splicing. From nano-git-db: Paths() dedupes, so there is no way to ask "what keys are under this section?". They hardcode a ten-name hook list purely because they can't ask what's under `code:`, and any open-ended or map-like section is unmodellable. Children(path) returning child names in file order, duplicates included, deletes more of their workaround code than anything else they asked for.
	- TradeClanker asks for the same thing for the same reason (reading an open section means scanning all of Paths() and prefix-matching), and notes it also sidesteps the Paths() quoted-name bug for that use.
	- Opened: n/a
	- Closed: 20260802-121147

- ✅ Hint when a merge combines non-adjacent bindings.
	- Done: H002, hint severity, at the later line with the earlier line named in the prose.
		- Fires only on a last-segment no-selector merge into a non-last child - adjacent re-mentions and the dotted redundant-path idiom stay silent. Corpus case 035; 001/013 diags goldens gained one each.
		- From nano-git-db: merge-by-(name, value) means two separately-written `table: t` sections silently become one combined table, and only the parser can know it happened - their consumer-side "already defined; first wins" check was unreachable and got deleted as dead code.
		- A hint-severity diagnostic carrying both line numbers makes that class of check possible without touching the documented merge semantics.
	- New H-code; diagnostics are contract, so all four bindings plus the expected-diags goldens move together.
	- Opened: 20260802-105223
	- Closed: 20260802-125957

- ✅ Suppress H001 where the schema declares the repetition.
	- Done at check --schema assembly (and inside the one-shot), via a shared library helper so CLIs and runners can't drift; matched by leaf name, the filter consumers hand-rolled. Corpus case 036.
		- From nano-git-db: the repeated-bare-leaf hint is structurally a false positive for any field whose repetition is the instance mechanism (`unique:`, `index:`, `row:`). So every correct file warns on load and users learn to ignore warnings; they filter it by hand for exactly three names.
		- When a schema is present and a path declares repeat with an upper bound above 1, H001 is dropped there - the information already exists and would otherwise go unused.
	- H001 is parse-time and the schema arrives at validate, so the suppression belongs where check --schema assembles output, not in the parser.
	- Opened: 20260802-105223
	- Closed: 20260802-125957

- ✅ One-shot load-and-validate, plus an error predicate.
	- Done: LoadAndValidate(text, schema, strictness) -> document carrying one combined diagnostics list (never throws; empty schema skips validation; declared-repeat H001s dropped), plus ErrorCount() on the document. All four bindings + veneer.
		- From nano-git-db: doc diagnostics and schema validation come back as two separate lists that must be merged by hand (forget one and half the errors vanish), and there's no HasErrors/ErrorCount, so "did this file have errors" is consumer bookkeeping.
		- That matters, since recover-and-continue means a mixed-indentation file otherwise returns no error at all.
		- A combined entry point (text + schema + strictness in, doc + one diagnostic list out) plus an error count removes a whole class of consumer mistake.
	- TradeClanker independently hand-rolled the same thing. No strictness level means "forgiving about spelling, loud about a dropped line", so at Standard an error diagnostic comes back beside a nil error and the line is silently skipped, and every consumer writes the same fifteen lines. An Errors() helper on the document is the smallest form of this ask, from a second consumer.
	- Opened: 20260802-105223
	- Closed: 20260802-125957

- ✅ Setters should say why they failed.
	- Done as an additive probe (setter returns are frozen post-1.0): `WriteReason(path)` in all four bindings + veneer runs the writer's exact validation without creating anything and names the failure: Writable/BadPath/ValueInPath/Wildcard/NoSuchIndex/TooDeep. place() now pre-gates on it, so the two can't drift. CLI stays exit 1.
		- From TradeClanker: Set* returns a bare pass/fail, and false covers an empty path, a malformed path, a wildcard, a depth overrun, and an unresolvable index indistinguishably - their workbench's error message is literally a guess because the library won't say.
		- A small reason enum (or per-binding equivalent) on the write result fixes it; CLI behavior stays exit 1.
	- All four bindings move together; the write corpus pins outputs, not the reason values, so exposure should be small; verify.
	- Opened: n/a
	- Closed: 20260802-122258

- ✅ Building a path from a user-typed name is injection.
	- Done: QuoteSegment/quote_segment/shcl_quote_segment + veneer, sharing the emitter's name-quoting - same spelling both directions as the Paths() fix.
		- From TradeClanker: Set* accepts segments that aren't valid bare names, and a dotted name is silently reinterpreted as nesting - a consumer concatenating a user-typed indicator name into a path is doing path injection without knowing it.
		- Export a quote-segment helper (the escaping the path scanner already understands), or segment-wise setters taking a list that can't be injected into.
		- Pairs naturally with the Paths() quoted-name bug above - same escaping code both directions.
	- Opened: 20260802-110332
	- Closed: 20260802-113718

- ✅ Dev-environment install script (Linux, macOS, Windows), runnable via a single `curl` or `wget`. Clones, installs dependencies, states what it will do with an option to abort.
	- Done: `install-dev.bash` at repo root. Linux + macOS directly; Windows via WSL (the dev pipeline is bash). Clones (or detects an existing clone), installs the no-sudo pieces itself (rustup, ruff/mypy/cppcheck via pipx, markdownlint via npm, PSScriptAnalyzer via pwsh), and prints the exact package-manager hint for what needs root (go, python3, gcc, shellcheck). Shellcheck-gated.
	- Opened: 20260713-065600
	- Closed: 20260722-195027

- ✅ Release-install script, `install.bash` and cross-platform `install.ps1`. In [repo] root, usage instructions in README.md.
	- Done: both at repo root, README Installing has the one-liners. Latest release via the GitHub API (`--release dev` = newest incl. pre-releases, default; `stable` = newest full release), binary picked by OS/arch, sha256-verified, `code/` (drop-in files) and `scripts/` (wrappers) pulled from the tag's source tarball. Idempotent (atomic binary swap); states the plan and confirms first.
	- Decisions along the way: `objects/` skipped - nothing statically-linkable is published yet (revisit with packaging). Linux user install goes to `~/.local/share/shcl` with the `~/.local/bin/shcl` symlink (one path can't be both the dir and the symlink). Windows user install adds its own dir to the user PATH instead of symlinking (symlinks need elevation); it went to `%USERPROFILE%\bin\Shcl` at first and moved to `%LOCALAPPDATA%\Programs\Shcl` before 1.0.0. macOS/BSD get a clear "no prebuilt binaries yet" pointer at build-from-source.
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
	- Opened: 20260722-194151
	- Closed: 20260722-195027

- ✅ Schema-as-SHCL validation. The schema is a plain SHCL file (type, required, allowed values). `Validate(doc, schemaDoc)` returns structured diagnostics and catches unknown or misspelled fields. See `design.md` "Schema validation".
	- Done: flat `field: <path>` schema layout, no grammar change; `Validate` in all four drop-ins plus the C++ veneer; `shcl check --schema` in all four CLIs; `V###` diagnostic codes; corpus cases 021-024 with a validate golden per case; crosscheck replays `check --schema`. Regex constraints and datetime ranges rejected for cross-binding parity; normative section in `spec.md`.
	- Note: needs the reference parser first, then spec the schema vocabulary alongside it.
	- Opened: 20260713-065600
	- Closed: 20260723-143512

- ✅ Expand the conformance corpus (`conformance/`) to cover the hard edges: dates and ambiguity, coercion, quoting and escapes, indentation errors, raw blocks, selectors and wildcards, strictness levels.
	- Done: cases for stacked arrays, coercion bundles, strict-load behavior, forgiving commas, raw blocks, and the full date whitelist.
	- ✅ Model diagnostic expectations (count, severity, the mandatory repeated-leaf hint). Every case now carries an `expected-diags.txt` golden (exact `check` stdout at standard: line/severity/code per diagnostic + the summary line), verified natively by all four runners. Pins the `H001` repeated-leaf hint and the zero-diagnostic cases too.
	- Opened: 20260711-150807
	- Closed: 20260722-194151

- ✅ Layered loading. `Load(defaults, site, user, ...)` merges later over earlier via the existing merge rule, with CLI and env overrides on top.
	- Done: `merge(base, over)` in all four drop-ins plus the C++ veneer; leaf names override, containers merge by `(name, value)`. CLI `--layer=FILE` (repeatable) on get/fmt/count/instances/set, `--set=PATH=VALUE` as the top layer; `fmt` doubles as merge. Env mapping dropped (belongs to the consuming program). Corpus case `025-layered` + `expected-merged.shcl` golden, all four native runners, crosscheck replays `fmt --layer/--set`; normative spec section.
	- Opened: 20260713-065600
	- Closed: 20260724-131644

- ✅ Schema-driven generation. Writer plus schema emits a commented, typed starter config (`shcl init --schema ...`). Depends on schema validation.
	- Done: `generate(schema)` in all four drop-ins plus the C++ veneer; `shcl init --schema=FILE` in all four CLIs. Required fields live, optional commented, wildcards in a trailing block; `desc` -> comment, a fixed-format annotation line summarizes type/constraints (byte-for-byte parity contract). Uses the schema's `default`/`desc` vocabulary. Corpus case `026-init-schema` + `expected-init.shcl` golden, all four native runners assert output + clean reload, crosscheck replays `init --schema`; normative spec section.
	- Opened: 20260713-065600
	- Closed: 20260724-133500

- ✅ Language spec rationalized from `notes.txt`: terminology, model, types, accessor and writer API, formatter, raw blocks. See `spec.md` and `grammar.abnf`.
	- ✅ stacked (`*`) block-array form alongside inline commas. Both spellings read the same and canonicalize to inline.
	- ✅ date and time formats pinned to a closed whitelist (year-first or named-month only, then calendar-validated).
	- ✅ adoption sweep. Cut the currency, percent, float-to-int rounding, and extra boolean tokens from default behavior. Case folding restricted to ASCII. Repeated-leaf hint made mandatory.
	- ✅ three strictness levels (loose, standard, strict) as a per-document knob, default standard. Loose re-admits the cut coercions; strict fails the load on any error.
	- ✅ bindings re-tiered. Tier 1 reference plus CLI; Tier 2 Go, C, Python; Tier 3 the rest, post-v1.0.
	- ✅ raw-block binding reworked. A fence is a value line for its parent field. Child-indent spelling is canonical.
	- ✅ inline-array commas made fully forgiving. Stray commas drop their empty slots and never error.
	- ✅ Model diagnostic expectations (count, severity, the mandatory repeated-leaf hint) in the corpus. See conformance item below.
	- Opened: 20260711-150807
	- Closed: 20260722-194151

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
	- Opened: n/a
	- Closed: 20260728-114451

- ✅ A CI/CD pipeline driven by `cicd/cicd.bash`: builds, tests, and can commit and push. Packaging and publishing are opt-in. See `design.md` for the split-by-responsibility rationale.
	- ✅ all stages live. Format check, build, lint, tests plus fuzz smoke, profiler, native and cross builds, versioned artifacts, README demo gif, publish.
	- ✅ `--ci` mode is the correctness gate the GitHub workflow runs, so local and CI share one definition of passing.
	- ✅ cross-binding differential check. Every binding CLI must agree with the reference byte-for-byte on the corpus and a fuzz-dumped input set.
	- ✅ dogfood stage installs the fresh release binary to a fixed local dir. Off under `--ci`, no sudo path.
	- ✅ lint stage widened to every binding. ruff and mypy for Python, cppcheck for C, markdownlint for docs, PSScriptAnalyzer for the ps1 wrapper. All gating, locally and in CI; setup steps in `contributing.md`.
	- ✅ demo gif: 50 fps motion, the screen clears between commands, gifsicle pass, a tenth of the size.
		- The `check` step needed `expect_exit = 6`; since `check` started exiting on errors it had been aborting the stage, so the committed gif was stale.
		- ✅ demo gif: opens by naming both usage modes (CLI and drop-in library), the formatter step says values are never rewritten, and the loop seam cuts to black instead of crossfading. Smaller again.
		- ✅ demo gif: output that fits on one screen now arrives at once like a real terminal, only an overflowing view scrolls, the cursor no longer blinks, and every motion frame is exactly 50 fps.
			- Nothing had actually been scrolling: the window is 22 rows and both long outputs fit, so the lines were popping in one at a time on a timer. That was the stepping.
			- Scroll smoothness is pixels per frame, so the step is rounded to an exact divisor of the line height - a line boundary never falls mid-step.
			- ✅ demo gif: the cursor blinks again while the prompt is idle, glides at sub-pixel resolution, and this demo drops the blank line between output and the next prompt.
				- The block is drawn with coverage-blended edges instead of snapping to whole pixels, so a 3 px per frame glide reads as continuous.
				- No padding line after output: the demo is about exact output layout, and a blank line invites misreading it. Scenario knob `blankafter`, on by default.
	- ✅ Packaging (.deb, .rpm, NSIS). Wire it when release cuts start.
		- Done: stage 6 builds .deb + .rpm (nfpm) per Linux binary and an NSIS setup per Windows binary into the release artifact dir, covered by the same sha256sums. `--no-package` to skip; off under `--ci` and `--quick`. Packages use distro layout (/usr/bin + /usr/share/shcl); payload matches install.bash.
	- Opened: 20260713-065600
	- Closed: 20260728-114451

- ✅ Accessor: two-tier junior-friendly API (convenience default plus full status), consistent across all bindings. A supplied default implies default mode.
	- Done alongside review item 6: the full status tier (`Read`/`read_*`) was already there; the convenience default tier is now in every library binding (`GetIntOr`/`get_*(default=)`/`shcl_get_*`/`get_or<T>`/native `unwrap_or`), plus the CLI `--default` for the wrapper bindings. Supplying the fallback is Default mode - value on `Good`, fallback otherwise.
	- Opened: 20260711-195150
	- Closed: 20260721-123412

- ✅ README rewrite: problem-first pitch, file and read-call examples, a format comparison table, a "wrong choice" section, and a plain statement of alpha status.
	- Opened: n/a
	- Closed: 20260713-065600

- ✅ Set up pub/priv key for download signing.
	- Opened: n/a
	- Closed: 20260728-133352

- ✅ Set up account on crate.io and get publish API key.
	- Opened: n/a
	- Closed: 20260728-114451

- ✅ Set up account on pypi.org and get publish API key.
	- Opened: n/a
	- Closed: 20260728-114451

- ✅ Initialize the git repo at `github/` and wire the remote. `main` plus a feature-branch flow.
	- Opened: n/a
	- Closed: 20260713-065600

- ✅ Resolve the open minor items at the end of `spec.md` (currency set, wildcard-missing behavior, `onBad` API, percent-to-int, fence info-string). All settled inline under "Resolved minor items".
	- Opened: n/a
	- Closed: 20260713-065600

- ✅ Rust reference parser (Tier 1) implementing the full spec, driven by the corpus. The `shcl` CLI builds from it.
	- Done: single-file zero-dependency library plus the CLI (`get`, `fmt`, `check`, `count`, `instances`). Corpus-green, with fuzz smoke in the test run.
	- Note: fuzzing turned up two formatter rules, now in `spec.md`.
	- Opened: n/a
	- Closed: 20260713-065600

- Code review 20260902:

	- The enhancement half of the round whose defects are under Bugs. Gate soundness, spec sentences the code needs, and library-level shapes no document rules on.

	- ✅ Item 29: `perf-gate.bash` passes a CLI that fails every workload instantly.
		- A stub that prints a usage error and exits 1 gets `OK: within 3x their own parse baseline`; the timer discards the exit status and nothing checks that the workload did anything. Today's workloads do run (an uncapped `edit_distance` blows the suggest budget by 20x), so the gate measures what it says and cannot tell when it stops. Require exit 0 (6 for `suggest`) and a non-empty stdout of the expected size per run.
		- Fixed: every timed run checks its exit code (0 for a write workload, 6 for the two that check a document with diagnostics) and that stdout carries at least the lines the work would produce - the key count for a write, two for a check. A run that did neither is a failure rather than a fast one.
		- Pinned by the gate itself against a stub that prints a usage error and exits 1: it reported OK before and names the failing run now.
		- Opened: 20260902-172800
		- Closed: 20260903-053000

	- ✅ Item 30: `shell-regress.bash`'s two static scans miss the ordinary spellings of what they scan for.
		- The unguarded-grep scan finds `a=$(grep`, `b="$(grep` and `c=$( grep` and misses `local x="$(grep ...)"`, `declare`, `export`, `readonly`, backticks, `+=`, an array element, and a `$(` whose `grep` is on the next line; it also skips every `set -e` script without a `.bash` extension, which is the pre-push hook and the publish script. The `\t`-in-ERE scan reads single-quoted patterns under `cicd/` only, so a double-quoted pattern, `--`, `--extended-regexp` and the installers are outside it. No live instance today. Allow the keyword prefixes and both quotes, and drive the file list off `git ls-files` plus a shebang test.
		- Fixed: the assignment scan takes a keyword prefix, an array element, `+=` and backticks; the escape scan takes either quote style and the long option spelling; and both sweep every tracked or untracked-but-not-ignored file that is named `*.bash` or carries a shell shebang, which brings in the pre-push hook, the publish script and the installers.
		- Pinned by a self-test inside the gate: one synthetic file carries all nine spellings plus two that must not match, and each scan has to find its own. The old scans find none of the seven assignment shapes and one of the two escape shapes.
		- Opened: 20260902-172900
		- Closed: 20260903-055000

	- ✅ Item 31: PSScriptAnalyzer is the one lint tool with no version pin, and `check-pins.bash` holds one direction only.
		- `ci.yml` installs it with no `-RequiredVersion` and `TOOL_PINS` has no entry, against the comment that the two agree; a new PSSA rule reddens hosted CI while the local gate stays green. Deleting the `ruff` entry from a scratch `config.bash` while `ci.yml` still installs it passes `check-pins`. Pin 1.25.0 in both places and walk `ci.yml`'s install lines the other way.
		- Fixed: `TOOL_PINS` carries PSScriptAnalyzer at 1.25.0 with a version command the drift warning can read, `ci.yml` installs that exact version, and `check-pins.bash` knows the `-RequiredVersion` joiner. It also walks the other way now: every pip, npm, `go install` and `Install-Module` line that names a version has to have a `TOOL_PINS` entry.
		- Pinned by the gate itself, both ways: dropping the `ruff` entry from `config.bash` is reported, and taking `-RequiredVersion` off the PSSA line is reported.
		- Opened: 20260902-173000
		- Closed: 20260903-060000

	- ✅ Item 32: five gates exit 0 when a tool they need is missing.
		- `check-locale.bash` skips without `localedef`, `shell-regress.bash` skips 12 checks without `pwsh`, 6 without `openssl` and the prerelease packaging row without `makensis` (the hosted runner has none, so 20260830b item 8 is pinned on this box only), and `package.bash` skips the rpm read-back without `rpm`. A strict switch under `--ci` that turns a skip into a failure keeps a runner that loses a tool from going quiet.
		- Fixed: `--ci` exports `SHCL_GATE_STRICT`, and all five skips read it - a missing tool is a failure under the gate and still a skip in a working copy, which need not carry every tool.
		- Pinned by `shell-regress.bash` itself, which exercises its own `fHave` both ways; flattening the helper back to a bare `command -v` fails it.
		- Note: the hosted runner had no NSIS compiler, so the strict switch would have failed it on the prerelease packaging row; the workflow installs `nsis` now, which also means that row - 20260830b item 8's pin - runs somewhere other than this box for the first time.
		- Opened: 20260902-173100
		- Closed: 20260903-062000

	- ✅ Item 33: `tests/cli_pipe.rs` cannot see the `out!`-to-`println!` regression on Linux, and its header does not say so.
		- With the macros' quiet exit replaced by a panic and `reset_sigpipe` kept, the test passes on Linux because SIGPIPE ends the process first; only the windows job pins the macros. A one-line note in the test header, so nobody reads the linux pass as covering 20260901b item 11.
		- Fixed: the header says it - a green run on linux says nothing about the macros.
		- Note: a comment carries no test of its own, and nothing else here is worth pinning; the four `cli-regress` rows added for 20260902 item 8 are what actually exercises the failure branch on linux.
		- Opened: 20260902-173200
		- Closed: 20260903-063000

	- ✅ Item 34: value identity ignores escapes while a selector resolves them, so one selector addresses two instances.
		- `a: "q\"uote"` and `a: 'q"uote'` are two instances, in one file and across layers, yet `a["q\"uote"]` matches both (`count` 2, `get` exit 5), and writing `a["q\"uote"].j: 2` in an over layer merges into the base's instance while the block spelling appends a second. Names took the resolve-escapes rule on 2026-08-18; the spec's merge sentence says nothing about escapes for values. A spec decision: either the identity key resolves escapes (all four take the resolved string in the key and hash) or the spec says a selector may address several spellings.
		- Decided: the identity key resolves escapes. The selector already matched on the resolved text, names have followed that rule since 2.0, and quoting was never part of identity either (`a: x` and `a: "x"` have always been one instance), so this is the spelling-is-not-identity rule finished rather than started. Recorded in `design.md`; the spec's merge bullet says it.
		- Fixed: the merge key, its hash and the equality behind a hash hit all take the escape-resolved element text, in all four. An element with no backslash is keyed on its own bytes, so the parse pays nothing for it - C resolves through the streaming hash it already had and compares a byte at a time, with neither side built.
		- Pinned by corpus `084` (two escape spellings of one value, a tab escape, and a bare-versus-quoted pair, with selector reads in both spellings and a layer that merges into the same instance). All four reported two instances before.
		- Opened: 20260902-173300
		- Closed: 20260903-071500

	- ✅ Item 35: three merge facts the spec should state.
		- The fold is not associative: with `p: 1`, `p: 2` and `p:` over `x: 1`, `(A+B)+C` overrides the leaf and `A+(B+C)` keeps `p: 1` as a wrapper-mention peer; 56 of 300 generated triples differ the same way, so a consumer caching a pre-merged upper pair gets a different document from the CLI fold. A merged document keeps the base's strictness, so a value from a stricter layer reads with the base's coercion (library only). And a merge costs a pass over the base's touched scopes plus an index rebuild on the next read, with replaced nodes kept until the document is dropped (measured in all four: 40k-key base plus a 3-line over is 5 to 66 ms, the next read 2 to 17 ms, 500 merges of an 8-node over grow the process by 0.5 to 1.5 MB). One sentence each in Layered loading and the merge doc comments.
		- Fixed: the spec's Layered loading section states all three with the measured shape of the cost, and every binding's merge doc comment says the same three.
		- Pinned by `check-docs.bash`, which requires the sentence in the spec and in all four merge comments.
		- Opened: 20260902-173400
		- Closed: 20260903-073000

	- ✅ Item 36: the C merge grows the document by the parent's whole child list per merge, and `w_encode_string` amplifies a string value about four times.
		- A one-leaf over onto a 40k-key document costs 1 MB of arena per merge (100 merges, 100 MB), because `w_overlay` rebuilds the parent's children into a fresh doubling vector in the document arena and the abandoned copies are never reused. `set_string` of a 20 MB value grows the arena by 85 MB, from the 32-byte builder doubling through blocks of twice each request. Build the rebuilt list in scratch and copy it exactly sized; reserve the builder up front.
		- Fixed: the rebuilt child list is built in scratch and copied into the document arena at exactly its size, and the string builder opens at the value's own length. A bump arena abandons every step of a doubling climb, which is what both were paying for.
		- Measured: 100 merges onto a 40000-key base cost 786 KB each and cost 320 KB now, which is the list itself; a 20 MB `set_string` cost 55 MB and costs 20 MB.
		- Pinned by `mem_bounds.c`: a merge may cost no more than half again the list it rebuilds, and a string value no more than its own size plus a block. Both fail on the old header.
		- Opened: 20260902-173500
		- Closed: 20260903-081500

	- ✅ Item 37: the name index rebuild walks every node ever created, removed subtrees included.
		- After 100k set-and-remove cycles a document with 1000 live nodes holds 400k, and every rebuild (the first read after any merge) indexes the dead ones too: 48 ms per read against 0 on a fresh document; `shcl_compact` cures it in C and the other three have no compact. Rust's `name_index` iterates the arena the same way. Build the index by walking from the root in all four.
		- Fixed: the rebuild walks from the root in all four. Chains keep file order, since a chain is one parent's same-named children and each parent's are appended in its own list order.
		- Measured: 200 rebuild-and-read cycles behind 100000 set-and-remove pairs took 68 ms in C and take 2.6 ms; the same shape in the reference went from 269 ms to 3 ms.
		- Pinned by a fixture in all four runners: the same document timed with and without the churn, bounded by a ratio rather than a wall-clock constant. All four fail it on the old code.
		- Note: the chain array is still sized by the arena, which is a fill the walk cannot avoid; the bound is loose enough to leave that alone and tight enough to catch the walk.
		- Opened: 20260902-173600
		- Closed: 20260903-090000

	- ✅ Item 38: `instances()` shows a writer-built string differently from its reparse when the text holds both quote kinds.
		- `set_string("k", "q\"q'")` stores `q"q'`, canonical output is `k: "q\"q'"`, and a reparse stores `q\"q'` with the escape intact; reads and selectors agree on both sides, `instances()` returns display text and so differs. All four. The one observable where `set(x)` and `load(emit(set(x)))` disagree; either spell `\"` in the encoders when both quote kinds are present, or leave it documented.
		- Decided: spell it. The alternative documents a difference between a document and its own reload, which is the one thing the fixpoint rules exist to rule out.
		- Fixed: the string encoder escapes the bare double quotes when both kinds are present, which is what the emitter writes and therefore what a reparse stores. Reads are unchanged - escapes are resolved on the way out - and identity was already escape-resolved.
		- Pinned by a fixture in all four runners and by a new fuzz property: a written document and its own reload must agree on `instances`, not only on the value. Both fail in all four on the old encoders; the property fails within 50 iterations.
		- Opened: 20260902-173700
		- Closed: 20260903-095000

	- ✅ Item 39: Python's array setters take any iterable, so a string or a generator writes the wrong value at `True`.
		- `set_string_array("k", "abc")` writes `k: a, b, c`; `set_int_array("k", (x for x in [1, 2, 3]))` writes an empty value and returns `True`, because the type gate consumes the generator before the setter reads it. `set_comment`, `set_raw` and `set_literal` have no type gate at all and raise from inside on a non-string. The module's own comment says a typed setter takes exactly the type its name says. Refuse `str` and `bytes`, materialize the iterable once, and gate the three.
		- Fixed: the shared element check refuses `str`, `bytes` and `bytearray` outright and materializes the iterable once, handing the list back to the setter; the three untyped setters call the same type gate their typed siblings use.
		- Pinned by a fixture in the Python runner (five wrong-type calls that must raise, and a generator that must write three elements). Python-only: the other three bindings are statically typed here.
		- Opened: 20260902-173800
		- Closed: 20260903-103000

	- ✅ Item 40: two library-level parity points in the file tier and the Go writer.
		- Rust alone writes through a trailing `/` on a regular-file path (`save_file("f/")` succeeds where POSIX refuses `open("f/")`); Go refuses `f/` but writes `f/.`; Python refuses both. Go's `SetString` of a string that is not UTF-8 stores U+FFFD per bad byte and reports success, so the value does not read back verbatim; return false from the string setters on invalid input, or say so in the Go doc comment.
		- Decided: refuse in both cases. A document is not a directory, and a value that cannot read back verbatim is what every other setter already refuses.
		- Fixed: all four refuse a path that ends in a separator or whose last component is `.` or `..`, before the path is resolved; the spec's save section says so. Go's `SetString` and `SetStringArray` refuse text that is not valid UTF-8.
		- Pinned by a save fixture in all four runners (three directory-shaped spellings refused, the file unchanged, the plain path still saving) and a Go test for the UTF-8 refusal. The reference took `f/` and Go took `f/.`; Python and C already refused both, which the fixture now holds them to.
		- Opened: 20260902-173900
		- Closed: 20260903-113000

	- ✅ Item 41: CLI shapes that are consistent across the four and still surprise.
		- `get --array --default=0` on a missing or empty array prints one line, indistinguishable from a one-element array equal to the default. Extra tab-separated fields on an ops line are dropped silently (`raw` loses everything after a literal tab in its content). `--default` followed by `--on-bad=error` drops the default; the other order keeps it. Schema-side hints are never printed by `check --schema` or `init`, so the `H001` that explains a `V092` from a merged `allowed` is invisible. `children` prints nothing at exit 0 for a missing path, a repeated field and a childless node alike, at the library level too. Each wants a decision, not a fix.
		- Decided: three are fixed and two are documented. `--default` with a contradicting `--on-bad` is a usage error, since honoring one of the two silently is what surprises and the order dependence made it unpredictable. Extra fields on an ops line are an error: dropping them lost content at exit 0, and a tab inside a value has an escape. The schema's own diagnostics are printed with its own line numbers, on stderr where a V099's already go, so stdout stays the code contract and no golden moves. The array-default ambiguity and `children`'s three silences are inherent to a line-per-element surface, so the man page says what to ask instead.
		- Fixed: all four CLIs, one behavior at a time; the man page carries the two documented shapes.
		- Pinned by seven `cli-regress` rows: the option conflict in both orders, the consistent pair, `--default` on its own, extra fields on a `raw` and on an `int` with an array form that still takes any number, and a schema whose own load hints. All fail on the old CLIs.
		- Note: the C conflict check first read an uninitialized field, so `--default` on its own was refused. Only the crosscheck's own `--default` dimension saw it; the rows here all named `--on-bad` or neither. There is a row for `--default` alone now.
		- Opened: 20260902-174000
		- Closed: 20260903-131500

	- ✅ Item 42: three spec sentences the ports need.
		- The Loose float-to-int tie rule (the code rounds half away from zero, `-2.5` to `-3`, while the formatter decision is half-even and Python's `round` is half-even). The aggregate-status ordering for array reads (`Good < Empty < NotFound < BadType < Multiple`, derived from the enum order in all four and written nowhere). And `format_f64`'s doc comment says the CLI uses it, which becomes true with item 1.
		- Decided: the tie rounds half away from zero, which is what all four already do and what every language's own `round` but Python's does. Half-even is the formatter's rule for choosing a spelling; picking an integer is a different operation, and changing it would move values under every consumer.
		- Fixed: the coercion table names the direction with both signs, and the aggregate-status rule gives the ordering. `format_f64`'s comment became true with item 1 and needed nothing.
		- Pinned by `check-docs.bash` for both sentences, and by corpus rows: `003` reads `-2.5` and `3.5` at Loose, and `081` carries a wildcard read whose four slots are Good, Empty, NotFound and Multiple with the worst as the aggregate.
		- Opened: 20260902-174100
		- Closed: 20260903-140000

	- ✅ Item 43: generator gaps the spec does not rule on.
		- A `desc` that is not one scalar (`desc: a, b`) produces no comment and no fault. `type: raw` with a `default` can never generate (the default is emitted inline, so V097 reports wrong type), and a default carrying a raw block is dropped silently. `init` prints `schema line 0` for V096 and V097, whose line space is the document.
		- Decided: `desc` takes the whole value, since a comma in a sentence is what an author types and a comment has room for it. A `default` with no value-line spelling is a schema fault, not a dropped value: the alternative is a starter config missing a field's default with nothing said. And V096/V097 print as `line 0`, since neither is at a schema line.
		- Fixed: all four read `desc` as every element joined as written, fault a raw-valued `default` and a `default` under `type: raw` with `V092` at its own line, and print the two generation-only codes in the document's line space. `init`'s fault printer goes through the shared diagnostic line now instead of hard-coding the schema prefix.
		- Pinned by three `cli-regress` rows (a raw default, a comma `desc` with its whole generated output, and the V097 line space), each failing in all four before. The spec's generation section says both rules and the code table names the line space.
		- Opened: 20260902-174200
		- Closed: 20260903-152000

	- ✅ Item 44: the C CLI's "cannot create temporary file" phase wording is decided by `_waccess`, which on Windows reports every existing directory writable.
		- The C library reports errno alone and the CLI guesses the phase from `_waccess(dir, 2)`; on real Windows an ACL-protected directory gets the bare errno where Rust and Go name the temp-create phase. Cosmetic under wine, which follows the unix mode bits. Record the failing phase in the library's write, or probe with an exclusive create.
		- Decided: probe. Recording the phase means an out-parameter or a per-thread last-error on a call that returns errno today, which is API growth for a message; creating a file is what the write itself does, so it answers for the platform it is on.
		- Fixed: the CLI creates a uniquely named file in the target's directory and removes it, instead of asking `access`. Only on the failure path.
		- Pinned by a `cli-regress` row: a file in a directory with no write permission must name the temp-create phase in all four.
		- Note: the row cannot fail on the old C here - `access(dir, W_OK)` is right on linux, and it is `_waccess` on windows that answers yes for every existing directory. Said plainly rather than papered over; the hosted windows job runs the row, but an ACL-protected directory is not something the runner can be given portably.
		- Opened: 20260902-174300
		- Closed: 20260903-160000

	- ✅ Item 45: `shcl_authored_name`'s comment says its result lives in the read arena; it lives in the document arena.
		- The real lifetime is longer than stated (until `shcl_free` or `shcl_compact`), so no caller is hurt; `shcl_children` hands back the same kind of pointer and says so correctly. Fix the sentence.
		- Fixed: the comment says the document's own arena, and why - the name is stored rather than built.
		- Pinned by `check-docs.bash` for the sentence and by `mem_bounds.c`, which reads an authored name, releases the read arena and requires the bytes to still be there.
		- Note: `mem_bounds`'s index-rebuild timing check is skipped under a sanitizer, which costs per allocation rather than per node walked; it was marginal there and would have been a flaky gate. The plain build in the test stage is where the ratio is judged.
		- Opened: 20260902-174400
		- Closed: 20260903-163000

	- ✅ Item 47: merging a layer differs from merging the same layer's canonical form.
		- Present before this round's changes. The merge fuzz property covers it, and the gate's own 200,000 iterations reach it - but the seed set carries the corpus, so this round's new cases shifted the sequence and moved the shape into range. The quick run stops at 20,000 and never sees it.
		- Reproduced with a base of one refused line (a name carrying a vertical tab, kept as trivia) and a layer whose trailing comment sits inside a stacked element's raw body. The layer and its own canonical form emit the same text, so the fixpoint property holds for the layer alone; they disagree about where the comment is attached, and only a merge shows it. The base's trivia comes out before the layer's comment one way and after it the other.
		- Base `t<VT>o: 5`, layer `*<tab>```` / `  line1` / `  #` / `` `` ``. Direct gives `line1: / # / t<VT>o: 5`; through the canonical form, `line1: / t<VT>o: 5 / #`.
		- Cause: a comment trailing a field at the field's own indent is kept as that field's, and a document's own trailing comment is kept separately and emitted after everything. For a top-level field the two are spelled the same - column zero - so a reload cannot tell them apart, and a merge, where the field's comment travels with the field and the document's is appended, puts them in different orders. An error-repaired document is what makes the two spellings meet: the field was written indented and ended up at the top level.
		- Fixed: a comment trailing a top-level field is the document's. One written deeper than its field still belongs to the field, since that has an indent to come back to. All four bindings.
		- Pinned by corpus `087`, which every binding failed before the change, and by the merge fuzz property that found it - clean over 200,000 iterations now, where it failed at 44,208.
		- Opened: 20260904-025000
		- Closed: 20260904-034000

	- ✅ Item 48: the C allocation-failure unwind crashes on a real windows host.
		- `oom_recover.c` segfaults on the hosted windows runner (mingw gcc, `-O2`), and passes on linux and under wine with the same source and the same compiler family. The recovery point is a `setjmp`, and on x86_64 mingw a `longjmp` unwinds through SEH, which wine's runtime and the real one do not implement alike - so the two disagree about a stack the library builds identically.
		- Until it is understood, the test runs on linux (in the test stage, the compiler sweep and under the sanitizers) and not on the windows job; `mem_bounds.c` does run there. A consumer building the header with mingw is the one this would reach.
		- Reproduced on a real windows host, first run and every run. The fault is a read past the top of the stack from inside `RtlVirtualUnwind`, on the first budget tight enough to reach the `longjmp` at all - so every recovery was crashing, not an edge case among them.
		- Cause: mingw's `setjmp` hands `longjmp` a target frame, so `longjmp` runs a full `RtlUnwindEx` to get there. Taking that frame address forces a frame pointer, and gcc then puts a vectorized function's xmm save slots at offsets from the post-prologue rsp while `UWOP_SET_FPREG` in the same unwind info tells the unwinder to take them from rbp, which sits above the whole frame. The difference is the frame's own size and it lands off the end of the stack. Wine derives rsp differently and never reads there.
		- Fixed: both recovery points arm with `_setjmp(buf, NULL)` on mingw x86_64, which makes `longjmp` restore the context without unwinding at all. That is all a C recovery point needs - nothing in between has a destructor or a `__finally`. Recorded as a C deviation in `style-guide.md`, and the `SHCL_OOM` docs now point an embedder whose hook longjmps at the same thing, since that unwind crosses these frames too.
		- Pinned by a `win-runners.bash` step that builds and runs the test at `-O0`, `-O1`, `-O2`, `-O3` and `-Os`. The shape needs both a frame pointer and saved xmm registers, which gcc decides per level: backed out, three of the five crash and two do not, so a single-level gate could have gone either way.
		- Fixed on the way: the load half wrote its fixture to a hardcoded `/tmp`, which a mingw binary does not translate. The new gate would have rested on the runner happening to have a `C:\tmp`.
		- Follow-on: the macro is `SHCL_SETJMP` and now sits in the public half of the header, since the embedder the `SHCL_OOM` docs point at arms the recovery point in their own translation unit and could not reach it. `oom_hook.c` arms with it too, and the gate sweeps both oom tests across the five levels rather than only the recovery one - the hook's recovery point is the same shape, one inlining decision from the same fault.
		- Opened: 20260904-052000
		- Closed: 20260904-070000

- Code review 20260901b:

	- The enhancement half of the round whose bugs are under Bugs. Test gaps, decisions the spec leaves open, and the smaller installer and tooling items.

	- ✅ Item 24: diagnostics printed under `--layer` do not say which file they came from.
		- Two layers with a bad line 2 print `line 2: ...` twice, indistinguishable. Schema diagnostics already carry a `schema line N` prefix for the same reason.
		- Fixed: each layer's diagnostics carry its own file name, in all four, and only when more than one file is loaded - a single-file run prints exactly what it did.
		- Pinned by two `cli-regress` rows: a layered load must name the second file, and a single-file load must not name anything.
		- Opened: 20260901-192300
		- Closed: 20260903-171500

	- ✅ Item 25: nothing in the suite reads from a merged in-memory document; only the merged text is compared.
		- Every read path through a merged arena (dropped nodes, rebuilt index, cloned lists, wildcard slots) is uncovered. A property run found them all correct today, so this is a gap, not a defect. A `reads-merged.tsv` per case, replayed with the layer arguments, would close it.
		- Decided: a property, not a golden. Every read of the merged document has to agree with the same read on a reparse of its text, which is an oracle the suite already trusts - and it needs no new corpus files, so it covers every merged case there is and every shape the fuzz generates rather than the rows somebody thought to write.
		- Fixed: all four runners compare `paths`, `count`, `children`, a string read and a string-array read with its slots between the merged document and a reparse of its canonical form, over every merged case; the reference's merge fuzz does the same every eighth iteration.
		- Note: `instances` is deliberately not compared. It hands back the source spelling, and canonical output legitimately respells a value - escaping a quote to keep it on one line - so the two differ for a reason that has nothing to do with merging. The property found that difference on an input carrying both quote kinds behind an `E017`, and it is the documented behavior of that call, not a defect.
		- Opened: 20260901-192400
		- Closed: 20260903-180000

	- ✅ Item 26: merge behaviors the spec does not state, needing a decision each.
		- `Line(path)` after a merge cites a line of whichever file the node came from, unlabeled. A leaf override drops the base leaf's comments and its blank-line grouping, where the in-file fold keeps both. Merging a document over itself doubles its leading comments. `lost` sums across layers, so a library consumer that merges then saves to a fresh path is refused for a line a layer dropped. A strict failure in a lower layer prints only that layer's diagnostics. All four bindings agree on every one.
		- Decided: state all five, change none. Each follows from a rule that is already there - a leaf override replaces the binding the comment described, `lost` is about content the merged document no longer has whatever it is written to, a strict failure ends the fold - and all four already agree, so the gap was the writing down.
		- Fixed: the spec's Layered loading section states all five. The one thing that did change is a message: a strict failure in a layer now says which layer, the same labelling item 24 gave the rest.
		- Pinned by a `cli-regress` row for the labelled strict failure; the five statements are behavior the corpus's merged cases already hold.
		- Opened: 20260901-192500
		- Closed: 20260903-190000

	- ✅ Item 27: datetime `allowed` equality is on the written shape, so `-00:00` equals `+00:00` and neither equals `Z`, and `12:00:00` is not `12:00:00.0`.
		- All four agree. The struct mirrors what was written, so this is arguably by design, but it is not a rule anyone would write down. Either compare on a normalized key or state the as-written rule in the spec.
		- Decided: compare the moment. The `allowed` row already says the comparison is in the coerced space of `type`, and for a datetime that space is a time, not a spelling - the struct mirroring the source is right for reads and wrong for a set membership test. It only ever admits values that used to be refused, so nothing that passed starts failing.
		- Fixed: the `allowed` comparison in all four ignores the zone spelling (`Z` and a zero offset are one zone) and trailing zeros in the fraction. An absent zone still matches no zoned value: a floating time is a different thing.
		- Pinned by corpus `085`, whose schema admits one moment in five spellings and refuses the floating one. All four reported three extra V004s before.
		- Opened: 20260901-192600
		- Closed: 20260903-200000

	- ✅ Item 28: a raw body in a `V004` message embeds its newlines, so one diagnostic spans several stderr lines.
		- Fixed: the value a `V004` message quotes has its line breaks and tabs escaped, in all four, so one diagnostic is one line however the value was written.
		- Pinned by a `cli-regress` row: a raw block with a two-line body against a string `allowed` must report on one line.
		- Opened: 20260901-192700
		- Closed: 20260903-210000

	- ✅ Item 29: validation questions the spec leaves open.
		- `type: string` on a multi-element value checks type against the joined string and `allowed` per element, so `allowed: "x, y, z"` fails on `c: x, y, z` while `get --string` returns the joined form. `min` above `max` is not a schema fault. Both consistent across the four.
		- Decided: the `allowed` interaction is the documented rule met head-on - the row already says element values, and `allowed: "x, y, z"` is one element - so it is written down rather than changed. A crossed `min`/`max` is a schema fault: the range admits nothing, so the config was told off twice per value for something only the schema can fix.
		- Fixed: a `min` above its `max` reports `V092` at the `max` line and drops the field, in all four. The spec's `allowed` and `min`/`max` rows say both rules.
		- Pinned by corpus `023`, which gained a crossed range; all four reported two per-value errors and no fault before.
		- Opened: 20260901-192800
		- Closed: 20260903-220000

	- ✅ Item 30: `E003` is reachable from a file after all, and the spec, the backlog and the corpus all say it is not.
		- `a[5].b: 2` with one `a` reports it in all four. The spec's "unreachable" reasoning covers only the `[#N]` spelling; the bare `[N]` index is documented and reaches it. Reword the row and add a corpus row.
		- Fixed: the code row says which spelling cannot reach it and which does, instead of telling a reader the code never fires.
		- Pinned by corpus `086` (a bare index past the end, and a valid one beside it) and by `check-docs.bash`, which refuses the old wording.
		- Opened: 20260901-192900
		- Closed: 20260903-224500

	- ✅ Item 31: the hosted windows job runs one of the three C memory tests.
		- `oom_recover.c`, the setjmp unwind and the most platform-sensitive test in the suite, and `mem_bounds.c` are built on Linux only. Both pass under wine, so it is two lines in `win-runners.bash`.
		- Fixed: `win-runners.bash` builds and runs `mem_bounds.c` there. `oom_recover.c` was held off that job at the time because it crashed on a real windows host; item 48 fixed the crash and put it on, swept across five optimization levels.
		- The index-rebuild check is a ratio against a fresh document, and it went wrong two ways once it ran off this box. Windows counts whole milliseconds, so the fresh side reads one tick or none and the ratio rests on nothing; it is skipped now where the clock cannot resolve that side, the way it is already skipped under a sanitizer. And the constant term was tight enough that a shared runner's descheduling crossed it, so all four bindings' copies carry a wider one - the factor is what catches the defect, which is orders rather than a fraction. Every allocation bound in the file still runs on windows.
		- Pinned by the two new rows. The ratio still catches an index rebuild that walks every node the document ever held, unchanged.
		- Opened: 20260901-193000
		- Closed: 20260903-231500

	- ✅ Item 32: two `cli-regress` rows are Linux-only and nothing says so.
		- A directory as the input expects "is a directory"; Windows says access denied, invalid function or permission denied depending on the binding, because none of the three stats the path first. The closed-stdin row cannot be judged under wine at all. Either give the rows a per-platform expectation or report the directory case from a stat.
		- Decided: stat the path. A per-platform expectation would only have written down four spellings of the same thing, and the four CLIs disagreed with each other on Linux too - `Is a directory (os error 21)`, `read PATH: is a directory`, `[Errno 21] Is a directory: 'PATH'` and `PATH: Is a directory`.
		- Fixed: all four CLIs answer `PATH: Is a directory` before the open, on every platform.
		- Pinned by the `read-dir-names-error` row, which now expects that one line anchored rather than a loose match on three words. The closed-stdin row says in a comment that it is POSIX-only, since closing fd 0 has no windows equivalent.
		- Opened: 20260901-193100
		- Closed: 20260903-234500

	- ✅ Item 33: three windows behaviors that wine cannot show and the hosted job should verify.
		- Go's CLI likely exits 8 on a closed stdin on real Windows where Rust and C exit 0, because only the Go runtime on Linux reopens closed standard fds. The C file tier passes paths over 260 characters through unprefixed where Rust and Go add the long-path prefix. The C file tier on Windows does not resolve a symlinked target, so a save through a link may replace the link. None of the three could be exercised under wine.
		- Decided: all three are defects, not just gaps in what is checked, so each was fixed rather than only pinned.
		- Fixed, stdin: a stdin nothing is attached to reads as an empty document in all four CLIs. Windows reports a handle the shell closed as an invalid handle or an invalid function rather than as end of input, and all three windows CLIs exited 8 on it - not only Go.
		- Fixed, C on windows: the save target is resolved through the final path, so a symlink or junction is followed and the answer carries the long-path prefix. The prefix is kept only where the name plus the temp file's suffix would exceed the old limit.
		- Pinned by a `win-runners` closed-stdin row across all four, and by two windows-only C runner fixtures: a save and rewrite through a path past 260 characters, and a save through a symlink that has to leave the link in place. The link fixture skips where no link could be made, which is every run under wine and a windows host without the privilege.
		- The long-path fixture found a second half on the hosted job: the read side went through the plain wide open, so a path past the limit could be written and not read. Reads take the same resolver now.
		- Note: the symlink half is judged only on the hosted windows job. Nothing else here can make a windows symlink: wine reports one as created and creates nothing. Both windows fixtures pass there.
		- Opened: 20260901-193200
		- Closed: 20260904-004500

	- ✅ Item 34: installer behaviors read but not run.
		- Uninstalling a system tree laid down by the pre-fix installer under a tight umask leaves the four subdirectories and prints a false "files this installer did not put there" note, because the glob does not expand in a root-only directory. `install.ps1` from a 32-bit host lands the 64-bit binary under the x86 program files. The tag picker in `install.bash` returns nothing on compact JSON. On PowerShell 5.1 a native command writing to stderr under `2>&1` throws before the exit code is read.
		- Fixed, uninstall: the payload globs moved inside the privileged shell, as `fRemoveLaidDown`, so they expand where the directory can be read. Still no recursive delete.
		- Fixed, program files: a system install reads `ProgramW6432` when it is set, which is only where it differs from `ProgramFiles`.
		- Fixed, tag picker: the three fields are split out of the response before they are read, so a release on one line works the same as a pretty-printed one.
		- Fixed, 5.1 stderr: the smoke run drops the Stop preference for the call and puts it straight back.
		- Pinned by four `shell-regress` blocks. The uninstall one runs the shipped function against a directory the calling shell cannot read and the command it runs can, which is what privilege buys and needs no root. The tag picker gets the same fixture with its whitespace removed. The program files line is evaluated in a pwsh with both variables set, then with only one. The 5.1 rule is source order, since the throw needs a 5.1 that cannot be run here.
		- Opened: 20260901-193300
		- Closed: 20260904-011500

	- ✅ Item 35: the `.rpm` does not own `/usr/share/shcl`, so removal leaves the directory behind. The `.deb` is fine.
		- Fixed: the package definition declares the directory, so both formats own it and both remove it.
		- Pinned by the package read-back in `package.bash`, which now checks the directory in both formats, and by a `shell-regress` block that builds a stub package and puts it through that same read-back - the release-time check had nothing to fail on until a release. That block needs a packager, so it runs where one is and stays out of the hosted gate, which installs none.
		- Opened: 20260901-193400
		- Closed: 20260904-013000

	- ✅ Item 36: `install.bash` silently replaces a symlink at the bin path that points somewhere else.
		- Only a regular file is refused. A link to a cargo-built or hand-built copy is overwritten with no word about it, where the uninstall path already knows how to test "only ours".
		- Fixed: the check is `fLinkOwner`, and it answers free, ours, file or elsewhere. The last two are refused before any download, naming what is there and where it points.
		- Pinned by a `shell-regress` block over all four answers.
		- Opened: 20260901-193500
		- Closed: 20260904-014000

	- ✅ Item 37: neither installer notices a different `shcl` shadowing the one it just installed.
		- The receipt line runs the installed link directly, never what `shcl` resolves to on PATH. On Windows the user PATH entry is appended after the machine PATH, so a setup.exe install shadows every later user install permanently.
		- Fixed: both installers ask what `shcl` resolves to and name the other copy when it is not the one just written.
		- Note: on windows a user install cannot be put ahead of a machine one - windows reads the machine entries first - so saying which copy wins is the whole of what an installer can do there without rewriting someone else's PATH.
		- Pinned by a `shell-regress` block over the three answers (ours, someone else's, none at all) and a source check that the PowerShell side asks the same question.
		- Opened: 20260901-193600
		- Closed: 20260904-014500

	- ✅ Item 38: `install.ps1 -Uninstall -Target system` over a setup.exe install guts it and leaves the Add/Remove Programs entry pointing at nothing, and the reverse leaves a stale version there.
		- Both write the same directory. When `uninstall.exe` is present, run it or say to.
		- Fixed: an uninstall that finds `uninstall.exe` in the directory refuses and points at it, rather than deleting the files under a registration it cannot remove. An install over one says the Add/Remove entry still names the version the setup put there.
		- Pinned by source order in `shell-regress`, the way the smoke-run placement is: the whole script refuses to run off windows, so nothing here can reach that branch.
		- Opened: 20260901-193700
		- Closed: 20260904-015000

	- ✅ Item 39: a read-only HOME fails after both downloads, with two raw `mkdir` errors.
		- The plan step could probe the nearest existing parent for writability before spending the downloads.
		- Fixed: the three destinations are probed before anything is fetched, each by walking up to whatever exists, and the refusal names the directory that cannot be written. A sudo install skips the probe, since root is what writes there.
		- Pinned by a `shell-regress` block over the walk-up and a read-only home, plus a source-order check that the probe precedes the first download.
		- Opened: 20260901-193800
		- Closed: 20260904-015500

	- ✅ Item 40: `install-dev.bash` leaves a fresh clone on `main` while contributing.md says to branch from `dev`.
		- Fixed: a fresh clone is put on `dev` when the remote has one. A checkout that is already on another branch, or has edits in it, is left where it is.
		- Pinned by a `shell-regress` block over three clones: fresh, one with staged work, and one of a repo with no dev branch.
		- Opened: 20260901-193900
		- Closed: 20260904-020000

	- ✅ Item 41: both installers report a GitHub API rate limit as "none published yet, or network down".
		- An unauthenticated 403 is common behind a shared address. Honor `GITHUB_TOKEN` when set and name a 403.
		- Fixed: both send `GITHUB_TOKEN` when it is set, and both name a 403 or 429 as the rate limit it is. A 401 blames the token rather than the network, which is what a wrong token gets.
		- Pinned by a `shell-regress` block over the four statuses. The status itself is curl's own reporting.
		- Opened: 20260901-194000
		- Closed: 20260904-020500

	- ✅ Item 42: every installer fix since 2.0.0 is on dev only, while the README one-liners fetch `install.bash` and `install.ps1` from main.
		- 424 changed lines across four rounds, including the shell-scope leak and the umask tree, and every user today runs the pre-fix scripts whichever release they get. The next cut resolves it once; the standing question is whether installer fixes count under the docs-only main merge exception, or the one-liners should fetch at a tag.
		- Decided: the installers count under the docs-only exception. Nothing ships them and the one-liners read them from main as they run, so a fix on dev reaches nobody. Fetching at a tag was rejected - it would freeze an installer's bugs into every release cut with them. Recorded in `design.md` under Branch flow.
		- Done: the three scripts are on main, matching dev byte for byte, with no tag, no bump and no changelog entry. The one-liners now fetch the fixed scripts.
		- Opened: 20260901-194100
		- Closed: 20260904-021000

	- ✅ Item 43: the flamegraph drops 35 to 60% of the profiled CPU time and the report does not say so.
		- The sampler's library blocklist drops any sample whose leaf is inside libc rather than truncating it, so allocation, copying and write time are invisible and every percentage is over the survivors. Today's graph kept 930 of about 1600 samples. Print the expected count beside the kept one at least.
		- Decided: the blocklist stays. It keeps the signal handler out of libc's own unwinder, which is what it is for; what it cannot do is go unsaid.
		- Fixed: the profiler writes the kept and expected counts beside the SVG, and the report leads with them and with what the missing samples were. Measured on a fresh run: 762 of about 1194.
		- Pinned by `shell-regress` rows over a graph with the counts beside it and one without, plus a check that the profiler still records them.
		- Opened: 20260901-194200
		- Closed: 20260904-022000

	- ✅ Item 44: a third of the parser's visible self-time is freeing the per-line path buffer.
		- Today's profile puts 33% under dropping the path scan's segment vector, which builds two strings per segment per line and frees them per line. Keeping the vector across lines and clearing it would remove most of that. With item 43's blind spot the true share is likely larger.
		- Fixed: a name with no backslash and no upper case is already its own resolved, folded spelling, so the scanner's own buffer becomes the segment name and nothing else is built. That is nearly every name in a document. `fold_name` returns a `Cow` for the same reason `key_text` does.
		- Measured: three allocations per segment down to one, which is the segment vector growing. Parse of a flat 6.7 MB document with two segments a line, best of fifteen: 218 ms to 209 ms.
		- Note: the 33% reading overstated it, and item 43's own caveat is why. What the graph shows there is Rust's drop glue; the free itself lands in libc, whose samples the sampler drops. Timing says four percent, which is real and repeatable and which no wall-clock threshold could gate.
		- Pinned by an allocation count instead: `mem_caps.rs` parses the same document with one segment a line and with four, and requires the extra segments to cost under two allocations each. Before the change they cost three; the reading is exact and does not care how fast the machine is.
		- Note: the two measuring helpers in that file had a lock each, so a reading taken while the other test ran counted its allocations too. One lock for both now.
		- Opened: 20260901-194300
		- Closed: 20260904-024000

	- ✅ Item 45: two style-guide gaps.
		- A Python deviation bullet sits under the C heading. The Python section does not list the iterative walks that replace the reference's recursion, whose reason lives only in a closed backlog item and the function comments.
		- Fixed: the misplaced bullet is under Python, and the iterative emit, overlay and clone walks are listed there with their reason - CPython's recursion limit sits below the depth cap the spec allows, and raising it would trade a traceback for a stack overflow.
		- Opened: 20260901-194400
		- Closed: 20260904-030000

	- ✅ Item 46: the PowerShell wrapper's header carries one ragged 126-column line where its bash twin wraps.
		- Fixed: it wraps, and so does the bash wrapper's array example, which ran to 106 columns.
		- Pinned by a `shell-regress` row over both wrappers' comment lines. Code lines are left out: both carry a long one on purpose.
		- Opened: 20260901-194500
		- Closed: 20260904-030200

- Code review 20260901:

	- The enhancement half of the round whose bugs are under Bugs. Three items, kept to what was actually reproduced.

	- ✅ Item 7: a float literal past the double range reads as infinity, at `Good`.
		- `1e400` reads as `inf` and exits 0, in all four bindings. So does `1e309` and `1.8e308`. The negative spellings give `-inf`.
		- Not a spec violation - the float section states no range - which is why this is here rather than under Bugs.
		- It does contradict the reasoning behind the 20260830b decision on the loose float-to-int read, which stopped saturating at 2^63 because no double holds that value and the path could not honestly produce it. The same sentence applies to a float literal the double cannot hold.
		- It also feeds item 4: read the value, write it back unchanged, and the field no longer reads.
		- Done: the float reader refuses a non-finite parse result in all four bindings, at every strictness, so `1e400` is `BadType` like any text that is not a number. Underflow (`1e-400`) still reads as zero, and the largest double still reads. Spec: the float section states the range. Changelog entry under Changed, since a read that answered now refuses.
		- Pinned by corpus `074-float-range`: scalar, array and loose-currency spellings at three strictness levels, plus the largest double and an underflow. Fails on the old code in all four runners.
		- Opened: 20260901-140600
		- Closed: 20260901-190000

	- ✅ Item 8: nothing bounds the diagnostic list.
		- A parse emits one diagnostic per bad line with no ceiling, so a document of nothing but bad lines costs several hundred bytes each. Three million of them run to roughly 500 MB.
		- That is the mechanism behind item 2's stacked-array measurement, but it is not specific to `E021` - any per-line code does it, and a caller reaching for `ParseLimited` because the input is untrusted has no way to cap it.
		- A ceiling with a "and N more" tail is the usual shape. It would want a spec line, since the diagnostics list is a contract.
		- Done: `ParseLimited` takes a third cap, `max_diags` (0 = uncapped, and the plain parse stays uncapped). Every parse diagnostic goes through one gate; past the cap it is counted by severity and dropped, and the parse ends its list with one `E022` at line 0 naming the cap, the count not listed and how many of those were errors. The tail is an error whenever an unlisted one was, so a consumer scanning the list still finds an error, `error_count` stays nonzero and a Strict load still fails; a hint otherwise. In C a message is built in the per-line arena and copied into the document only when listed, so an unlisted one costs nothing past its line. The C++ veneer mirrors the new parameter. Spec: `E022` row and the Limits bullet; the changelog's `ParseLimited` entry names the third cap.
		- Pinned by the `parse_limited` fixture in all four runners (fifty bad lines at cap 10: ten `E014` then the tail with its exact message; error count 11; Strict fails; hints past the cap leave a hint tail that loads at Strict; at the cap exactly there is no tail) and by an allocation bound in all four: 200k stacked element lines under an element cap of 8 and a diagnostic cap of 100 must stay under 8x the text (16x in Python, where the split lines alone are 10x). With the cap made a no-op every fixture and every bound fails.
		- Opened: 20260901-140700
		- Closed: 20260901-200000

	- ✅ Item 9: C has no write-side counterpart to `shcl_reads_release`.
		- Repeated writes to the same path grow the document arena: about 48 bytes per `shcl_set_int`, 63 per `shcl_set_string`, none of it reclaimed until `shcl_free`. Overwriting one field once a second is a few megabytes a day.
		- Inherent to a bump arena and not documented as otherwise, so it is not a defect. The reads got their own arena and a release call for the same reason, and a long-running writer has the same problem with no answer.
		- Smaller than item 3 and reachable only by a consumer that writes in a loop. Worth a documented note even if no call is added.
		- Done: `shcl_compact(d)` rebuilds the document into fresh arenas through the clone the merge already uses, carries the diagnostics, orphans, lost count and strictness across, swaps the rebuilt document in and frees the old storage. Read results are invalid after it, as after `shcl_reads_release`; on an allocation failure the document is left as it was. The C++ veneer exposes `compact()`. Recorded in `style-guide.md` as a sanctioned deviation, named in the README's C note, and in the changelog under Added.
		- Pinned by `mem_bounds.c` (100k rewrites of one field grow the arena from 800 bytes to 4.8 MB; compaction brings it to 864 and the document reads the same) and by a property over every corpus case in the C runner: canonical output, every diagnostic, the lost count, the error count and the strictness are unchanged across a compaction. Clean under ASan, UBSan and the leak check.
		- Opened: 20260901-140800
		- Closed: 20260901-203000

- Code review 20260830b:

	- The enhancement half of the round. Items 18 to 57; bugs 1 to 17 are under Bugs. Several of these are design questions rather than defects, and a few are recorded so they are not re-derived next round.

	- ✅ Item 18: the read subcommands print no load diagnostics at all.
		- `fmt` and `set` print them on stderr in both modes, settled last round. `get`, `count` and `instances` print nothing at any strictness below strict.
		- Reproduced: a file whose line 3 is dropped reads clean through all three, empty stderr, exit 0.
		- This is what makes item 2 bite. It is also the asymmetry that matters most, since the read subcommands are the ones a script actually runs.
		- The only escapes are a separate `check` pass, or strict, which fails the read instead of mentioning anything. There is no "read the value and tell me the file is damaged".
		- Against it: a read in a loop would start emitting per-call noise, which the write subcommands do not have. A quiet flag, or emitting once per process, covers that.
		- Decided: every subcommand that loads a document reports, once per run, in the shape fmt and set already use. One CLI run is one load, so the loop case is one line per call and no quiet flag was added. This supersedes the "get stays quiet" half of item 47 of the 20260830 round, noted there.
		- Fixed: in all four bindings. Help text, man page and design updated; the four help texts still match byte for byte.
		- Pinned by `cli-regress.bash` rows `get-diags`, `count-diags` and `instances-diags`, which fail in all four bindings without the fix.
		- Left alone: `check` builds its own diagnostic list and is unchanged, and the crosscheck discards stderr so it neither sees nor is moved by this.
		- Opened: 20260830-140346
		- Closed: 20260831-073000

	- ✅ Item 19: there is no CLI traversal command, so a script can read an open section's values but never learn its keys.
		- The README leads with "everything the library does, the binary does from a shell". Children and Paths have no CLI or wrapper spelling.
		- Both the design and the spec name traversal as one of the accessor's two modes. The CLI carries the lookup half plus count and instances and stops there.
		- Reproduced on a three-key open section: a wildcard read returns the three values correctly, and nothing returns the three names. `instances` on the wrapper prints three blank lines, because those instances have no value.
		- The only workaround is parsing `fmt` output in shell, which is the thing the project exists to prevent.
		- Additive: two subcommands, four CLIs, help, man page, both completion files, a corpus case. No parity risk, minor version.
		- Fixed: `children FILE [PATH]` and `paths FILE` in all four CLIs, with PATH left out enumerating the top level. Both take the read options the other enumeration subcommands take.
		- Decided: names print in the form a path accepts, quoted where a bare name will not do, matching what `paths` already emitted. Enumerating keys is only worth doing if what comes back can be read straight back, and for an ordinary name the output is identical either way, so no option was added to choose.
		- Also: help, man page, both completion files, both wrappers (`shcl_children`, `shcl_paths`), and a README example.
		- Pinned by three layers, each verified by backing the code out: corpus case `069-traversal` through the four runners (new `children` and `paths` reads.tsv kinds), `cli-regress.bash` rows `children-top`, `children-quoted`, `children-missing` and `paths-all` against fixed output, and the crosscheck replaying both kinds through all four CLIs.
		- Also fixed: `check-completions.bash` read the command list with a single-line grep, which read nothing once rustfmt wrapped the array at nine entries.
		- Opened: 20260830-140346
		- Closed: 20260831-081500

	- ✅ Item 20: two of the five advertised integration modes have no artifact and no build.
		- The design lists five and calls the fourth "link the prebuilt shared library". The spec repeats shared and bundled as part of the goal.
		- Nothing builds one. The manifest declares a library and a binary only, no C ABI surface is exported, and the release stage produces binaries, packages, setups and the drop-in tarball, nothing else.
		- The C header declares its API with plain externs and no export macro, so a shared object is buildable on ELF by default visibility and a Windows DLL is not.
		- Worth deciding rather than drifting. A real shared library means an ABI commitment, a soname, symbol versioning and a headers package, all of which pull against the one-file zero-dependency premise.
		- Narrowing the documents is the cheap answer and probably the right one: drop "prebuilt", and say those two modes mean compiling the drop-in.
		- Decided: narrow the documents. A published shared object means an ABI to keep - soname, symbol versioning, a headers package - which pulls against the one-file zero-dependency premise. The two modes now read as compiling a drop-in source file two ways, and the paragraph says plainly that neither is published prebuilt and why.
		- Pinned by `check-docs.bash`: a document may not offer a `.so`, `.dll` or `.dylib` as already built while no manifest declares a library crate-type. Verified by restoring the old wording and watching it fail.
		- Opened: 20260830-140346
		- Closed: 20260831-083000

	- ✅ Item 21: `remove` and the set-if-absent family have no option form, only the stdin ops script.
		- The design records why scalar sets got a command-line option: a tab-separated script on stdin is something no shell writes comfortably and no editor preserves.
		- That reasoning was applied to sets and then left standing for `remove`, which is at least as common an edit, and for the defaults family, which is the whole write-out-defaults half of the writer.
		- Reproduced: removing one key needs a printf with a literal tab piped into `set --write`. Getting the tab wrong fails loudly, so this is friction, not a correctness hazard.
		- Raw blocks belong on stdin, so this is not an argument to retire the ops script.
		- Additive: repeatable options joining the ordered edit list the set option already uses.
		- Fixed: `--remove=PATH`, `--set-default=PATH=VALUE` and `--set-literal-default=PATH=TEXT` in all four CLIs. All five spellings share one ordered list, so two touching the same path resolve in the order given, and they are valid on the same subcommands `--set` already was.
		- Note: removing nothing is not an error, matching the ops script's `remove`.
		- Also: the `--write cannot be combined with` refusal names the option actually given rather than always saying `--set`, and the help's subcommand lists for `--layer` and `--set` read "all but check/init" - they had gone stale when item 19 added two subcommands.
		- Pinned by eight `cli-regress.bash` rows covering each spelling, a default that must not clobber, ordering within the list, the ephemeral form on a read, the `--write` refusal and an empty path. 32 of 136 checks fail without the code.
		- Opened: 20260830-140346
		- Closed: 20260831-093000

	- ✅ Item 22: exit 1 is still the catch-all for usage, I/O and an unwritable path, a week after 7 was carved out.
		- Exit 7 was created on the argument that a script could not tell "pass the lossy flag or fix the file" from "the command line is wrong". The same argument applies to a setter that cannot write, and was not applied.
		- Reproduced, all exit 1: an unknown flag, a missing file, a wildcard path, and an index selector naming no instance. Three different remedies, one code.
		- The prose already distinguishes them, so the information exists and only the code throws it away. The write-reason vocabulary names six causes in every binding for exactly this purpose.
		- Convention is against the current split: usage errors sharing a code with I/O is unusual.
		- Honest counter: the message already tells you, so this is coherence rather than capability.
		- Decided: new exit 8 for a file or stream that could not be read or written, and 1 becomes the usage code alone. Same reasoning as exit 7, applied to what was left in the catch-all: fixing a command line and fixing a path, a permission or a disk are unrelated remedies.
		- A path a write option refuses (a wildcard, an index naming no instance) stays at 1 rather than taking a third code, because what has to change there is the option's value. Two codes, not three.
		- Fixed: in all four bindings - every file, schema, layer and stdin read, and the save's generic failure. Help, man page, spec, both wrappers and design updated.
		- Pinned by six `cli-regress.bash` rows, four I/O and two usage, plus the existing directory-read row moved from 1 to 8. 20 of 160 checks fail without the code.
		- Opened: 20260830-140346
		- Closed: 20260831-101500

	- ✅ Item 23: the raw info read is the one typed entry point with no convenience tier, and the deviation is recorded nowhere.
		- The spec promises each typed entry point comes in two tiers. Every read type has the short form except this one, in all four bindings and the veneer.
		- The CLI does have the convenience form, so the two surfaces disagree about whether this read has a fallback spelling.
		- The C tier restriction is a sanctioned deviation and is written down. This one is in no document.
		- Either add the companion in the four, or record it as deliberate. Adding it changes no output.
		- Decided: both. Rust, Go and Python gained `get_raw_info` and `get_raw_info_or`, so the CLI's convenience form now has a library counterpart.
		- C and the veneer keep the status tier alone. That is the existing recorded deviation for every read handing back borrowed memory; the spec and style guide now name raw-info in it rather than leaving it to be read into "raw".
		- Pinned in each binding's convenience-tier fixture, which gained a raw block: the three assert the new calls read through and fall back, C asserts the status-tier route. Verified by removing the methods and watching each runner fail.
		- No output changed, so the corpus and crosscheck are untouched.
		- Opened: 20260830-140346
		- Closed: 20260831-110000

	- ✅ Item 24: the Python value display and merge key pay a join and a generator on every single-element cell.
		- Measured on an 8.5 MiB document: display is called 799k times and the merge key 1.4M times, for 363k source lines. The join is the largest non-parse entry in a profile.
		- A length-one fast path in both took parse-plus-emit from 8.52 s to 7.94 s, about 7%, with byte-identical output.
		- Python-only spelling. Rust and Go stream these through a hash and build nothing, so this narrows the gap rather than widening a structural difference.
		- Fixed: a length-one branch in `_Value.display` and in `_merge_key`, which is the shape of every scalar field.
		- Pinned in the Python runner by a counting sequence: a one-element cell must not be walked at all, a two-element one must be walked twice. That is exact, where a wall-clock threshold on a constant-factor win either flakes or never fires.
		- Output is byte-identical, verified over a 1.7 MB document and the whole corpus.
		- Opened: 20260830-140346
		- Closed: 20260831-113000

	- ✅ Item 25: the Python source-attach guard builds two merge keys per line, usually to compare a value with itself.
		- The key is computed eagerly, then compared against the key of the value just passed in, which in the common case is the same object.
		- An identity check first took another 2% off, and skips the work entirely when the flag is already set. With item 24 the pair is about 10%, output unchanged.
		- Fixed: an identity test before the key compare. The bound node holds the object just parsed in the common case, so neither key is built.
		- Pinned in the Python runner: parsing 200 plain lines must build zero value keys. It built 400 before.
		- Opened: 20260830-140346
		- Closed: 20260831-113000

	- ✅ Item 26: the Python path scanner rebuilds two closures on every call.
		- They are defined in the function body, so they are recreated once per document line, each carrying a cell.
		- Module-level helpers taking the same arguments keep the call flow and the names, so the mirror of the reference's inner functions survives.
		- Not measured in isolation, so the size of the win is unknown.
		- Fixed: the two helpers are module level now, taking the buffer and its length. The reference spells them as inner functions; the deviation is that Python rebuilds a closure per call and the scanner runs once per document line.
		- Pinned in the Python runner: the scanner's code object must carry no inner code objects.
		- Measured with items 24 and 25: parse-plus-emit of a large document got about 17% faster, output byte-identical.
		- Opened: 20260830-140346
		- Closed: 20260831-113000

	- ✅ Item 27: a comment on the value-lookup fallback does not match the code, in all four bindings.
		- It says a non-scalar hit and an outright miss both fall to the fallback scan. The fallback is gated on the selector being quoted, so an unquoted miss returns nothing with no scan.
		- All four agree with each other, so this is a comment defect, not a behavior defect.
		- An attempt to build the input the comment worries about did not succeed, and did not get far enough to call the case unreachable.
		- Fixed in all four: the comment now says the quoted selector is the one that needs the fallback scan, and that an unquoted one takes whatever the accelerator holds and does not scan.
		- The behavior the corrected comment describes is corpus-visible, so it is pinned rather than just described: case `070-selector-fallback` has a raw block and a scalar sibling with the same display, and `x[hi]` binds the raw while `x["hi"]` binds the scalar. The read path fans out to both, which the same case pins.
		- Left alone: the unquoted path would create a spurious instance if the accelerator entry were ever dropped while a sibling still satisfied it. A second attempt to build that input failed too, so it stays recorded rather than fixed.
		- Note: making the unquoted path scan on a miss is not the answer. A miss is the ordinary create path, so scanning there is quadratic in siblings - the regression class the perf gate exists for.
		- Opened: 20260830-140346
		- Closed: 20260831-120000

	- ✅ Item 28: four small robustness gaps in the comparison worker.
		- A non-numeric iteration count is a traceback rather than the usage line printed two lines above it.
		- A zero iteration count formats a value that is still unset, and raises.
		- The listing catches only import errors, so a loader failing any other way takes the whole listing down instead of reporting one entry unavailable.
		- The loader inserts a path on every call and the listing calls every loader, so the search path grows a duplicate each time.
		- Internal tooling, not shipped code.
		- Fixed: all four. A non-numeric or non-positive ITERS is the usage line at exit 2, the listing catches any loader failure rather than only a missing import, and the shcl loader only pushes its path when it is not already there.
		- Pinned in `shell-regress.bash`, whose purpose already covers the tooling the corpus cannot reach: three bad ITERS spellings and a duplicate-path check. Seven checks fail without the fix.
		- Opened: 20260830-140346
		- Closed: 20260831-123000

	- ✅ Item 29: the Go ASCII lower-case helper copies before deciding nothing changed.
		- Scanning first and copying only on a hit took the read benchmark from 9.27 ms to 7.59 ms, about 18%, with identical behavior.
		- Go-only, no cross-binding effect. Recorded as a measured win, not a defect.
		- Fixed: scan for an upper-case byte first, and copy only from where one was found.
		- Measured here on a 5000-key document: the read path got about 8% faster. The mixed-case call got slightly slower, which is the right trade - nearly every name is already folded.
		- Pinned by a Go test asserting an already-folded name allocates nothing. It allocated once before. An allocation count is exact where a wall-clock threshold on a constant-factor win would flake.
		- Opened: 20260830-140346
		- Closed: 20260831-130000

	- ✅ Item 30: the Go on-bad option folds with Unicode case where the other three fold ASCII.
		- Two different folds sit in one file for two adjacent options, and the binding already carries the ASCII helper the strictness option uses.
		- No code point folds into the accepted words, so this is drift rather than a live divergence.
		- Fixed: the Go CLI carries its own ASCII fold now, mirroring the library helper the strictness option already goes through and C's own equality check.
		- Found a second site in the same file with the same divergence: the float-grammar check that accepts inf/infinity/nan folded by Unicode too, where Python and C fold ASCII there. Both use the new helper.
		- Pinned by the existing crosscheck, which replays every option spelling through all four CLIs; the fold is only observable as agreement, since no code point folds into the accepted words.
		- Opened: 20260830-140346
		- Closed: 20260831-130500

	- ✅ Item 31: an uncommented error discard in the Go corpus runner.
		- The directive bans discarding an error without a reason. Low risk, since the same directory was just read, so a failure would drop a case's layer files rather than fail the case.
		- The prior sweep of discards covered the library and the CLI, not the test file.
		- Fixed: the discard stands; what it lacked was the reason. The directory was read moments earlier for the case files, so a failure here means it vanished mid-run - the case then has no layers and the merge assertion below reports it.
		- Nothing new proves this one: a comment has nothing to fail. The behavior it describes is already covered by the merge assertion it points at.
		- Opened: 20260830-140346
		- Closed: 20260831-131000

	- ✅ Item 32: the Go atomic write does not check the close.
		- The sync runs first and its error is checked, so a deferred write error reaching only the close is unlikely.
		- The reference drops the handle the same way, so checking it is a per-binding deviation rather than a parity fix. Read only, not reproduced.
		- Cause: the item's premise was wrong. C tests `fclose`, Python's close sits inside the try whose handler turns a failure into a failed save, and Rust checks the `sync_all` that is the only thing it can check on a `File`.
		- Note: Go alone dropped it, so this is a parity fix rather than the per-binding deviation the item assumed.
		- Fixed: the close's error becomes the save's error when nothing earlier failed. Without it a write error surfacing only at close would publish a truncated temp file over the target.
		- Nothing new proves this one, and that is worth saying plainly: a close that fails after a successful fsync needs a filesystem this box cannot produce, and faking one would test the fake. What it rests on is the other three bindings already behaving this way.
		- Opened: 20260830-140346
		- Closed: 20260831-131500

	- ✅ Item 33: the Rust command dispatch ends in a catch-all that silently aliases any new subcommand.
		- Adding a seventh entry to the command table without adding a dispatch arm runs `instances` instead, with no compile error and no message. It is only safe today because the caller gates on the table first.
		- The style directive asks for exhaustive matches and no catch-all unless needed. Here two lists have to be kept in step by hand and nothing enforces it.
		- Spell the last arm out and keep an explicit unreachable, or derive both from one table.
		- Fixed in all four, not just the reference: every one had the same fall-through, so leaving three would be the same finding again next round. Each command has its own arm and the last one is a refusal.
		- Pinned by `check-completions.bash`, which already parses the command table out of the reference: it now also parses the dispatch arms and diffs the two. Removing one arm makes it fail, naming the command that lost it.
		- The extractor matches the arm arrow rather than the indentation. `\t` is not an escape in POSIX ERE, so a pattern carrying one matches under the interactive grep on this box and nothing at all under the grep a script gets - it looked like a working check while asserting nothing.
		- Opened: 20260830-140346
		- Closed: 20260831-134000

	- ✅ Item 34: a redundant condition in the closing-fence test.
		- The length test is already implied by the minimum the opening fence enforces, so the emptiness check can never decide anything.
		- Harmless, but it is the dead-condition class the review directive asks for, and the same shape is flagged on the C side.
		- Fixed: removed in all four, with a comment saying why the length test is enough - the opening fence is three characters or more, so nothing empty can reach the all-same-character loop.
		- The invariant the length test now carries alone is already pinned by corpus case `053-raw-blank-line`. Dropping the length test instead of the emptiness test fails it in every binding, which is what says the remaining condition is the load-bearing one.
		- Opened: 20260830-140346
		- Closed: 20260831-140000

	- ✅ Item 35: four more C accessors grow the arena the way item 3 does, but these are documented.
		- Same measurement run: 200k calls add up to 31 MB, depending on the accessor.
		- Unlike the array reads, the header states the contract: the result lives in the document's arena until it is freed. So the growth is what was promised.
		- Recorded beside item 3 so a fix for that one does not quietly change these without a decision. No change needed unless the contract is revisited.
		- Closed by item 3's fix. The contract was revisited deliberately and kept; these accessors moved to the read arena with the rest, so `shcl_reads_release` covers them and the promise in the header still holds for anyone who never calls it.
		- Opened: 20260830-140346
		- Closed: 20260830-181500

	- ✅ Item 36: a latent unmatched-glob shape in the C sanitizer script.
		- A layer-collection loop ends on a conditional, which is the shape that aborts under errexit when the glob matches nothing.
		- Guarded in practice, and all four corpus cases that reach it do have layer files. An attempt to reproduce the abort on this box's bash did not abort, so the trap may not apply to this version.
		- Latent shape only, not a live defect. If touched anyway, use a null glob or continue on the miss.
		- Reproduced, once the right shape was tried: on this bash the loop is harmless at statement level, which is why the first attempt saw nothing. The same loop as the last command in a function returns 1 and kills the caller.
		- Note: latent in the sense that where it sits today is safe, not in the sense that the trap has gone.
		- Fixed with `|| continue` on the miss. The same shape was in `crosscheck.bash` and, in a non-glob form, in `check-pins.bash`; both are fixed too.
		- Pinned by a repo-wide scan in `shell-regress.bash`, beside the grep-substitution one. Restoring any of the three makes it fail, naming the file and line.
		- That scan also caught a line added earlier in this round, in `check-completions.bash`, which is the argument for having it.
		- Opened: 20260830-140346
		- Closed: 20260831-143000

	- ✅ Item 37: the C validator puts a 16 KB array on the stack per call.
		- One slot per depth level, sized to the depth cap. Fine on a main thread, possibly not on a small-stack thread.
		- Noticed but not chased: whether all slots are freed on every exit path was not verified.
		- Fixed: the level arenas are heap-allocated and freed with the rest.
		- Note: the unchased half is answered - nothing returns between the allocation and the free loop, so every slot is reached on every path. Said in a comment beside it.
		- Pinned by a POSIX-only fixture in the C runner that validates on a thread with the smallest stack the platform allows. It segfaults with the array back on the stack and passes without it. The runner and the sanitizer build now link pthread.
		- Opened: 20260830-140346
		- Closed: 20260831-150000

	- ✅ Item 38: the completions check rejects an option-less subcommand however the completions spell it.
		- One side emits a row for every subcommand, the other drops any with an empty option list, so the two can never agree on such a subcommand.
		- Reproduced on a copied tree: adding one makes the check fail against both completion files, and adding the matching completion arm does not clear it.
		- Costs a confusing lint failure the day an option-less subcommand is added, blaming the completions when they are correct. None exists today.
		- Two greps in the same function also lack the guard their siblings have; they survive only because command substitutions do not inherit errexit.
		- Both greps are guarded now, and `shell-regress.bash` scans for the shape repo-wide. The option-less subcommand half is still open.
		- Fixed: the completion side keeps a row with an empty option list, the way the CLI side always did. Its `*)` default arm is still excluded, since the pattern only matches subcommand words.
		- Pinned in `shell-regress.bash` by a fixture built from the real files with an option-less subcommand added to all three, so the check runs against the extractors as shipped. It fails without the fix even though the completions are correct, and still catches a genuinely missing completion arm.
		- Opened: 20260830-140346
		- Closed: 20260831-153000

	- ✅ Item 39: the demo's typing speeds sit under the bands the directive names.
		- Letters are drawn from a range whose mean lands in band once jitter is applied; digits do not.
		- The demo has been tuned with feedback twice, so this may be deliberate. It is not recorded anywhere.
		- Either raise the constants or note the tuning in the demo script.
		- Decided: the second option - the tuning is recorded beside the constants rather than raised. They were set by watching the result twice, and what reads as natural on screen is slower than what a person actually types, because the viewer is reading the command rather than recalling it. Digits are slower again.
		- Nothing new proves this one: a note has nothing to fail, and the numbers it explains are a matter of taste rather than a contract.
		- Opened: 20260830-140346
		- Closed: 20260831-160000

	- ✅ Item 40: the profiler stage swallows the reason a hotspot report is missing.
		- Stderr is discarded, and the report's only diagnostics go there, so the log records the failure with no cause. A missing directory, no flamegraphs and an unparseable file all read alike.
		- The fallback already treats the exit code as non-fatal, so keeping stderr costs nothing.
		- Fixed: stderr is kept, and the fallback line now points at the reason above it rather than standing alone.
		- Pinned in `shell-regress.bash` two ways: the report must still say something usable about a missing directory, and the stage must not redirect its stderr away. Restoring the redirect makes it fail.
		- Opened: 20260830-140346
		- Closed: 20260831-161000

	- ✅ Item 41: both bash installers print comment markup in their help.
		- The help text is the source header heredoc'd verbatim, comment prefixes and hard tabs included, so it opens with the file name as a comment and every wrapped line carries the prefix.
		- The PowerShell installer prints clean prose, so the two documented installers print help in visibly different registers.
		- Tab-indented help renders raggedly wherever tab width is not 8.
		- Fixed: both rewritten as plain prose with space indentation, so all three installers now read the same way. The source headers are unchanged; only what `--help` prints moved.
		- Pinned in `shell-regress.bash`: neither installer's help may carry a comment prefix or a hard tab, and neither may print nothing. Restoring either old heredoc makes it fail.
		- Opened: 20260830-140346
		- Closed: 20260831-163000

	- ✅ Item 42: the bash uninstall says "removed" while leaving a directory full of files it did not install.
		- Reproduced: a stray file in the install directory survives, the directory removal fails silently, and the script reports removal anyway with no mention of what is left.
		- The PowerShell installer handles the same case properly and says so.
		- Fixed: the install dir is removed only when it is empty, and when it is not the script says what it did remove and names the directory it left, in the same words the PowerShell installer already used.
		- Pinned in `shell-regress.bash` under a fake HOME, both ways: an empty dir still reports a clean removal, a stray file gets named, and the stray file is still there afterwards.
		- Opened: 20260830-140346
		- Closed: 20260831-164500

	- ✅ Item 43: the README does not name the Windows installer's archive-tool requirement.
		- The script refuses outright without it and names the Windows versions that carry it. The README's prerequisites cover only the Linux side.
		- The script's own message is clear, so this costs a failed run rather than a bad install. One clause fixes it.
		- Fixed: one clause under the Windows one-liner, naming the Windows versions that carry `tar` and pointing at the setup .exe for anything older - the same escape the script's own message gives.
		- Pinned in `check-docs.bash`: while `install.ps1` carries that refusal, the README has to name `tar` on the Windows side. A loose match was not enough, since the README says "starter" and "start" in several places.
		- Opened: 20260830-140346
		- Closed: 20260831-170000

	- ✅ Item 44: the PowerShell installer runs the binary only after writing it into place.
		- The bash installer runs the version check from the temp directory first, precisely so a binary that will not start never becomes an install.
		- On PowerShell 7.4 and later a nonzero exit from that line throws under the script's own error preference, giving a success message followed by an exception, with the install left in place.
		- Read, not reproduced, since no failing binary was available.
		- Fixed: the smoke run happens from the temp dir before anything is written, matching the Linux installer, and its output is captured with the exit status tested rather than called bare. The line at the end now prints what that run already produced, so a failing binary cannot reach a success message.
		- Pinned in `shell-regress.bash` as a source-order check: the smoke run must appear before the publish, and must test `$LASTEXITCODE`. Running the real installer needs a network and a release, so order is what can honestly be asserted here.
		- Opened: 20260830-140346
		- Closed: 20260831-172000

	- ✅ Item 45: the analyzer settings gate syntax only, and the comment reads as more.
		- Syntax compatibility says nothing about which framework members exist, which is what items 10 and 11 are. The type-compatibility rule was tried and reports nothing, because it matches type names and not instance members.
		- So no lint rule covers that class. The practical guard is a member test in the code, not a settings change.
		- Worth saying so in the settings comment rather than leaving it reading as full coverage.
		- Fixed: said in the settings comment - the rule covers syntax and nothing else, the two defects it did not catch are named, and `PSUseCompatibleTypes` is recorded as tried and useless here because it matches type names rather than instance members. The guard for that class is a member test in the code.
		- Nothing new proves this one: a comment has nothing to fail. What it points at - the member test in the wrapper - is already pinned by its own row in `shell-regress.bash`.
		- Opened: 20260830-140346
		- Closed: 20260831-173000

	- ✅ Item 46: the design and changelog break the blank-line-between-top-level-bullets rule.
		- 25 tight pairs in the design document outside its table of contents, and one in the changelog where the other 174 top-level bullets are spaced.
		- The spec, style guide, trademark and both package READMEs have none. The README's only hits are inside generated table-of-contents blocks, where the tool strips blank lines.
		- Fixed: all 26 spaced, 25 in the design document and the one in the changelog.
		- Pinned in `check-docs.bash` over every markdown file in the repo, skipping generated TOC regions and bare anchor-link lists, which is what the tool strips blank lines out of. Restoring the design document makes it report 25.
		- Opened: 20260830-140346
		- Closed: 20260831-175000

	- ✅ Item 47: the spec tables a load-time code no file can produce.
		- The block is introduced as the load-time codes so a gate can key on them. The selector code cannot fire from a file, because the marker opens a comment before the selector is read.
		- Both spellings that should produce it report the empty-selector code instead. No corpus case pins it, and the backlog reached the same conclusion earlier.
		- A gate keyed on it can never fire. Note it as unreachable from a file, or drop the row.
		- Decided: noted rather than dropped. The row stays, since the code is real in the source's message-to-code mapping, but it now says it is unreachable and why, so nobody keys a gate on it.
		- Reproduced both halves, and one was wrong in the earlier note: a file reports `E014` (empty selector), not `E012`, and quoting the `#` makes it an ordinary discriminator. On a write path the same situation comes back as the `NoSuchIndex` write reason rather than as a diagnostic, which is what makes the code unreachable rather than merely hard to reach.
		- Nothing new proves this one: the whole finding is that no input reaches the code, so there is nothing to assert. What the note prevents is a gate being written against it.
		- Opened: 20260830-140346
		- Closed: 20260831-181000

	- ✅ Item 48: two documents describe a stdout and stderr split that this week changed.
		- Both say the stable code goes to stdout and the prose to stderr. The stderr line now carries the code as well.
		- The transcript directly above one of those sentences shows the code on both lines.
		- Say the code is on both and that only stdout is the contract.
		- Fixed: both say it the way it works now - both streams carry the code, only stdout is the contract, and the stderr line adds the prose. Checked against real output rather than rewritten from the item text.
		- Pinned in `check-docs.bash`: no document may say the prose alone goes to stderr. Restoring the README sentence makes it fail.
		- Opened: 20260830-140346
		- Closed: 20260831-182000

	- ✅ Item 49: three of the four language examples ignore the setter returns the surrounding prose says to check.
		- Each calls the first two setters bare and checks only the third, while the comment above each block and the prose below both say an ignored failure means the save writes a config missing the edit and reports success.
		- The Rust example checks all three, because the type system forces it, so the one example a reader can compare against is the odd one out.
		- Fixed: Go, Python and C check all three setters and name the write reason, the way the Rust example does.
		- The C example still builds, which `check-readme-c.bash` proves; the Python block parses and the Go block is well-formed Go using the accessor it already called once.
		- Pinned in `check-docs.bash`: in each language block the number of setter calls and the number of checked calls have to match. Restoring the README makes it name all three blocks.
		- Opened: 20260830-140346
		- Closed: 20260831-184000

	- ✅ Item 50: one bold-lead bullet in the design document uses a trailing dash where every other closes the bold with punctuation.
		- Fixed: the bold now closes with a period, matching every other bold-lead bullet in the file.
		- Pinned by `check-docs.bash`? No - a one-line wording fix has nothing to fail. A repo-wide scan for the shape returns nothing now, which is the check that would have found it.
		- Opened: 20260830-140346
		- Closed: 20260831-185000

	- ✅ Item 51: the 20260830 round's sentence-length item is marked complete, and that half was not done.
		- The item names roughly sixty sub-bullets over forty words. The file currently has 263 over forty words and 62 over sixty; at the round's starting commit it was 272 and 67.
		- The other halves were done properly, so the item was worked, just not on this part.
		- Either split the long ones, or reopen the item with the remaining half stated.
		- Decided: reopen it as stated rather than split three hundred bullets of closed rounds. The counts are now 334 over forty words and 75 over sixty, higher than when the item was filed because this round added its own.
		- Fixed: this round's own sub-bullets, which is where the habit is set. Nothing here is over sixty words.
		- Left alone: the finding text of closed rounds. It was written by whoever filed it, splitting it means rewriting three thousand lines of prose nobody will read again, and the risk of changing what an item meant is real. The convention now says a filed finding keeps the shape it was filed in.
		- Opened: 20260830-140346
		- Closed: 20260831-185500

	- ✅ Item 52: twelve outcome bullets sit below the opened and closed stamps.
		- Every other item closes with the stamps. Twelve in the 20260830 round append a result after them, so the stamps stop being a reliable item terminator, and the trailing bullets are the ones a reader most wants next to the finding.
		- Eight of the twelve also carry no classification prefix.
		- Fixed: thirteen bullets moved above the stamps, so the stamps terminate every item.
		- Pinned by `check-docs.bash`: no sub-bullet may follow an `Opened:`/`Closed:` run at the same indent.
		- Opened: 20260830-140346
		- Closed: 20260831-190000

	- ✅ Item 53: deferred items carry no closed stamp and canceled items do.
		- Three deferred items have an opened date only. Every canceled item has both, so the two settled-but-not-done states are stamped differently with nothing saying why.
		- Either stamp the deferral date, or say in the conventions that a deferred item keeps only an opened date.
		- Decided: the second option. A deferred item keeps only its opened date, because deferring is a decision to come back rather than a settlement, so there is nothing to stamp closed. Said in the conventions.
		- Nothing new proves this one: it is a convention, and the three deferred items already follow it.
		- Opened: 20260830-140346
		- Closed: 20260831-190500

	- ✅ Item 54: classification prefixes are applied unevenly across rounds.
		- Coverage on item sub-bullets ranges from 97% down to 39% by round, and the two newest rounds are among the weaker ones, so the pattern is not just age.
		- Loose items in the Done sections are near zero.
		- Classify what is there; do not add content.
		- Cause: the vocabulary was never written down, so each round invented its own.
		- Fixed: the conventions now list it - `Reproduced:`, `Cause:`, `Decided:`, `Fixed:`, `Pinned by:`, `Left alone:`, `Measured:`, `Note:` - and this round's outcome bullets all carry one.
		- Left alone: the finding text of closed rounds, for the same reason as item 51. The convention says a filed finding keeps the shape it was filed in, which makes the unevenness intended rather than a gap.
		- Opened: 20260830-140346
		- Closed: 20260831-191000

	- ✅ Item 55: two measurement-dense lines survived the number sweep.
		- Each carries three or more timings, in the same round that filed the one-headline-number rule.
		- Fixed: four, not two. This round had added two more of exactly the same shape before the item was reached.
		- Note: each now carries one headline figure, which is the rule the round that filed it wrote.
		- Opened: 20260830-140346
		- Closed: 20260831-191500

	- ✅ Item 56: seven bullets say how a defect was found rather than what changed.
		- The clearest reads "found by reading the four write paths against each other, not by a test".
		- The 20260830 round removed method clauses but left the discovery-mechanism ones.
		- Drop them, or fold into the cause line where the gate that caught it is the point.
		- Fixed: nine, counting one this round had just added. The pure-method ones are gone; the rest were folded into what they were actually saying - a coverage gap, a symptom, or the gate that first measured something.
		- Pinned by `check-docs.bash`: no backlog bullet may open with "Found by" or "Found while".
		- Note: the check for it read `\t` in an ERE at first, which POSIX has no escape for - so it matched nothing under the grep a script gets while matching under the interactive one. `shell-regress.bash` now scans for that shape, since the same mistake had already cost a check earlier in this round.
		- Opened: 20260830-140346
		- Closed: 20260831-192000

	- ✅ Item 57: the loose runs in both Done sections are ordered only loosely.
		- 17 backwards steps by closed date. Most are same-week and read as topic grouping.
		- The conventions say "approximately", so this only matters if that word is meant to go.
		- Decided: "approximately" stays, and the conventions now say what it covers - items closed in the same week are often grouped by topic, which reads better than exact date order.
		- Left alone: the 17 backwards steps, all of them inside such a group. Sorting them strictly would break the grouping to satisfy a word the conventions never meant strictly.
		- Opened: 20260830-140346
		- Closed: 20260831-192500

- Code review 20260830:

	- The enhancement half of the round. Items 1 to 23 are under Done - Bugs.

	- ✅ Item 24: the changelog's Unreleased section has none of the 20260829 round.
		- It carries only the C file-tier fixes from 20260828. Needed before the 2.1.0 cut: `E018`, the `DateTime` alias, the raw-block nesting change, the `--set` split rule, the `bool` op gate, the whole-mode copy, the installer smoke run, and the round's user-visible fixes. Internal tooling stays out.
		- Unreleased carries both rounds now; nothing blocks the 2.1.0 cut.
		- Opened: 20260830-093632
		- Closed: 20260830-124432

	- ✅ Item 25: the `bool` op gate from 20260829 item 5 is not in the four conformance runners, so no corpus row can pin it.
		- The runners still write `false` for `yes`. Port the gate and add a bad-bool row to a `write-bad.ops`.
		- Opened: 20260830-093632
		- Closed: 20260830-124432

	- ✅ Item 26: stderr diagnostics still come in three forms, and 20260829 item 59 was only half applied.
		- `check` and `init` print schema faults without the code, so a script cannot key on `V091`. A strict-load failure prints each diagnostic twice (once as a line, again in the summary). Four messages carry a `shcl:` prefix and the rest do not.
		- One line form everywhere, and one prefix rule.
		- Opened: 20260830-093632
		- Closed: 20260830-124432

	- ✅ Item 27: 20260829 item 51 was only half applied.
		- The three identical `resolve_parent` match blocks in the parser and four argument matches in the CLI still use `match` where `let-else` reads better. The three copies plus the dead-parent check that follows each could be one helper.
		- Opened: 20260830-093632
		- Closed: 20260830-124432

	- ✅ Item 28: a `set_raw` test asserts on a document parsed before the refused writes, so the assertion cannot fail.
		- Opened: 20260830-093632
		- Closed: 20260830-124432

	- ✅ Item 29: a symlink cycle is silently replaced by a regular file on write.
		- The 40-hop walk gives up and the rename falls on whichever link it stopped at. Refuse it as every other tool does.
		- Opened: 20260830-093632
		- Closed: 20260830-124432

	- ✅ Item 30: small reference tidy.
		- The type-flag parser matches the same string twice with a catch-all standing in for `--string`.
		- The name index builds two hash maps keyed identically where one would do.
		- The comparison tool prints a save error with the debug formatter.
		- A dead `if i == 0` branch after `truncate(max(i, 1))` in `resolve_parent`, mirrored into Python.
		- Opened: 20260830-093632
		- Closed: 20260830-124432

	- ✅ Item 31: Go tidy.
		- `Read[T]` exports `Ok()` and `OK()`, same body; nothing calls the second. Deprecate it now, remove at the next major.
		- `doCheck` shadows the `errors` package with a counter.
		- Nine lines over 120 columns, two of them from the last round.
		- OK() stays as a deprecated alias of Ok(); both were public API.
		- Opened: 20260830-093632
		- Closed: 20260830-124432

	- ✅ Item 32: Python tidy.
		- The runner's `_op_int` lost the digit-length gate the CLI has, so its comment is wrong about how a long value is rejected.
		- `Document.__init__` and its attributes are unhinted; `_Value.fence_char` is typed `str` but starts as `None`.
		- The pipeline's three Python utilities are still unhinted and on `os.path`, as 20260829 item 54 listed.
		- `_resolve_target` differs from the reference on an empty name (creates the temp file in the parent of the cwd) and on a lexical `..` through a missing directory.
		- Opened: 20260830-093632
		- Closed: 20260830-124432

	- ✅ Item 33: C tidy.
		- The veneer smoke test never calls ten of the veneer's public methods.
		- The sanitizer run covers `fmt`, `check` and `set` only; `get`, `init`, `--layer`, `--write` and the veneer never run under ASan.
		- A dead `prec` clamp in the float formatter.
		- Fifty-odd internal typedefs are unprefixed and end up in the consumer's implementation TU; either document "one TU of its own" or prefix them.
		- The veneer's `generate()` is `const` but can push a diagnostic onto the schema; the hand-written rule of five could be a `unique_ptr` deleter.
		- Internal typedefs carry the Shcl prefix; the veneer owns its handle via unique_ptr and generate() is no longer const.
		- Opened: 20260830-093632
		- Closed: 20260830-124432

	- ✅ Item 34: the "value X is not a valid int" line renders the value four ways across the four CLIs, and C's can span lines.
		- Rust prints the source spelling escaped, Go escapes differently, Python and C print the resolved text raw, so a raw block or a tab breaks C's message across lines. C also gates the rawinfo case differently.
		- Decision: parity on this line, or at least keep it on one line.
		- Decided: one shared quoting for the reason line in every CLI; C reports the logical value (its read structs carry no source text, by standing decision) and uses the same resolve gate.
		- Opened: 20260830-093632
		- Closed: 20260830-124432

	- ✅ Item 35: `-h` and `--help` after the FILE argument are an unknown option, although every other option is accepted there.
		- The guard's stated reason never applies. Drop it in all four.
		- Opened: 20260830-093632
		- Closed: 20260830-124432

	- ✅ Item 36: CLI message wording.
		- A rejected `--set-literal` text is reported as an unwritable path.
		- An extra argument gets "get needs FILE and PATH".
		- Option validation runs before the command name is checked, so `shcl foo --int` complains about `--int`.
		- `--strictness` values are case-insensitive, `--on-bad` values are not.
		- `--set==5` and `--set "it's=1"` get messages that do not name the problem.
		- Opened: 20260830-093632
		- Closed: 20260830-124432

	- ✅ Item 37: exit code 1 covers both "add `--lossy`" and "file missing", and `init`'s exit 6 is in no table.
		- A script gating a rewrite cannot tell the refusal apart. Decision: a code of its own, and a table entry for `init`.
		- Decided: the refusal has its own exit code, 7. Help, man page, README, wrappers and design.md carry it, along with exit 6's init clause.
		- Opened: 20260830-093632
		- Closed: 20260830-124432

	- ✅ Item 38: five help lines run past 80 columns; one wraps mid-word on a default terminal.
		- Opened: 20260830-093632
		- Closed: 20260830-124432

	- ✅ Item 39: man page and help drift.
		- The `.TH` date is 2026-08-18 and the page changed twice since.
		- `get` in the synopsis is set with `.RI`, so it renders roman with italic brackets while every other subcommand is bold.
		- The help header promises a subcommand list per option and `--set` and `--set-literal` have none.
		- Neither names the four refusals the CLI enforces, or that `--raw` has no array form.
		- Opened: 20260830-093632
		- Closed: 20260830-124432

	- ✅ Item 40: completion drift the checker cannot see.
		- Neither file offers `--about` or `--donate`; the zsh file offers no top-level options at all and claims to be line for line the bash one.
		- Opened: 20260830-093632
		- Closed: 20260830-124432

	- ✅ Item 41: wrapper docs.
		- The bash wrapper's header example uses `mapfile` on a wrapper whose target is bash 3.2.
		- Both headers describe exit 6 as strict load failure only.
		- The PowerShell wrapper needs pwsh 7.3 on Linux and macOS and nothing says so.
		- Opened: 20260830-093632
		- Closed: 20260830-124432

	- ✅ Item 42: the installer's glibc message names 2.34 for every arch; the arm64 binary needs 2.30.
		- Opened: 20260830-093632
		- Closed: 20260830-124432

	- ✅ Item 43: dev setup and pin checks.
		- `install-dev.bash` skips the hooks setup in a git worktree, where `.git` is a file.
		- The cppcheck wheel version is now spelled in three places and only two are compared.
		- `check-pins.bash` matches by substring; a pin named `build` matches five workflow lines.
		- Opened: 20260830-093632
		- Closed: 20260830-124432

	- ✅ Item 44: two steps still run past the half-the-cores cap: the backup archive in the publish stage and `cargo deny`.
		- Opened: 20260830-093632
		- Closed: 20260830-124432

	- ✅ Item 45: pipeline comment accuracy.
		- The pre-push header says sharing the target dir avoids a rebuild; cargo keys on the package path, so it rebuilds anyway. The real reason for the link is the relative binary paths.
		- A ragged four-line comment in `cicd.bash`.
		- The utility scripts print `tool: message` lines rather than the `fEcho` family. Record the convention either way.
		- Opened: 20260830-093632
		- Closed: 20260830-124432

	- ✅ Item 46: one silent-exit assignment left in `sign-release.bash`, and the four `--help` outputs start and end without a blank line.
		- Opened: 20260830-093632
		- Closed: 20260830-124432

	- ✅ Item 47: `fmt` and `get` without `--write` say nothing about an error-severity line they skipped.
		- `fmt --write` prints it and `check` exits 6, but `fmt file > new` never learns a line was dropped. Decision.
		- Decided: fmt and set print the load's diagnostics in both modes; get stays quiet.
		- Superseded 20260831 by item 18 of the 20260830b round: the read subcommands print them too. A read below strict returns the value and reported nothing at all, which is the case where the silence costs most.
		- Opened: 20260830-093632
		- Closed: 20260830-124432

	- ✅ Item 48: doc wording.
		- Avoid-list words that survived 20260829 item 63: "land" in design.md and the spec, "ships" in README, the changelog and the man page, "worth checking" in README, "honest" and "human form" in two binding comments.
		- README: the tagline repeats "file"; the crate paragraph says the same thing in two adjacent lines and the "installs no command" note three times; "one of them advertised as the differentiating feature" does not say which.
		- Two spec bullets are single paragraphs of 971 and 526 words.
		- The README's Docs list omits the changelog.
		- The `dsl` topic on the repo cuts against the pitch.
		- Comments added last round carry timings that will go stale.
		- Dash-as-parentheses is still dense in design.md and the spec.
		- Opened: 20260830-093632
		- Closed: 20260830-124432

	- ✅ Item 49: backlog accuracy.
		- 20260727 item 2 was done by the 2026-08-21 memory pass; the remap clones nothing now. Move it to Done.
		- One closed item still says `--write` is deliberately unchanged, the opposite of the standing decision.
		- Several closed sub-bullets are overtaken: "as of the coming major", "the rest of the veneer list is still open", "worth a look post-1.0", the raw-block fixes that 20260829 item 2 superseded, and the deferred size limit that `read_file` now covers.
		- Opened: 20260830-093632
		- Closed: 20260830-124432

	- ✅ Item 50: backlog structure.
		- One item in a Done round has no number; one Opened stamp has no time.
		- Rounds split across sections do not say where their other items are, so numbering gaps look like lost items.
		- Loose items and rounds are two separate newest-first runs inside each Done section, which the conventions line does not say.
		- The unused testing icon row; five file-level lint disables that the repo config already covers; six "Code Review" against 21 "Code review".
		- Opened: 20260830-093632
		- Closed: 20260830-124432

	- ✅ Item 51: backlog prose.
		- About sixty sub-bullets over forty words, a dozen of them lists that only need bullets; 34 bullets with paired dashes as parentheses.
		- "shape" as a category word about forty times, a handful of "surfaces", "worth", all-caps and bold emphasis, and a few dramatic adjectives.
		- Opened: 20260830-093632
		- Closed: 20260830-124432

	- ✅ Item 52: backlog detail that belongs in private notes.
		- Environment and method clauses ("on this box", "under wine", "fault-injected", "converted mechanically", "staged on its own branch").
		- About fifty lines of timings, byte counts and per-binding multipliers; keep one headline number per item.
		- The header's claim that tracking is moving to GitHub Issues.
		- Opened: 20260830-093632
		- Closed: 20260830-124432

- Code review 20260829:

	- The enhancement half of the round. Items 1 to 25 are under Done - Bugs.

	- ✅ Item 26: packages and the drop-ins tarball are not reproducible.
		- Two builds seconds apart give different `.deb`, `.rpm` and setup checksums: the staged payload carries the build-time mtime, and the rpm stamps build time and the build host's name.
		- The drop-ins tarball records the local user and group and the checkout mtimes, so a fresh clone or another box gives a different sum, and the sums file signs it.
		- The README claim is scoped to binaries and is still true. Set `SOURCE_DATE_EPOCH` from the commit, touch the payload to it, and build the tarball sorted with numeric owner zero.
		- Fixed: `SOURCE_DATE_EPOCH` from the commit, the payload touched to it, the rpm build host pinned, the tarball built sorted with numeric owner zero. Two builds seconds apart give identical sums for every package and the tarball.
		- Opened: 20260829-071126
		- Closed: 20260829-093755

	- ✅ Item 27: tool pins are copied by hand into the hosted CI file, with nothing checking the two lists agree.
		- They match today. The last time they did not, hosted CI stayed red for days. A lint-stage check that greps each pin out of the workflow file would catch the next one.
		- Fixed: `check-pins.bash` in the lint stage greps every pin out of the workflow file.
		- Opened: 20260829-071126
		- Closed: 20260829-093755

	- ✅ Item 28: the dev setup script and contributing.md omit three tools the gate requires, and install the rest unpinned.
		- staticcheck, govulncheck and cargo-deny gate under `--ci`, and neither the script nor the doc names them; a fresh box following the doc fails the documented gate. The four tools that are installed come at latest, so the first run warns about pin drift.
		- contributing.md also says hosted CI uses the current Go; it pins the 1.26 series, for a reason the workflow file explains.
		- Fixed: contributing.md names the three tools and points at the pin list; the dev setup script installs every tool at its pinned version, the three included.
		- Opened: 20260829-071126
		- Closed: 20260829-093755

	- ✅ Item 29: two steps still run past the half-the-cores cap.
		- The Rust test harness threads are uncapped (the fuzz tests are the heavy ones), and ruff uses every core. Two environment variables.
		- Fixed: `RUST_TEST_THREADS` and `RAYON_NUM_THREADS` set to the cap.
		- Opened: 20260829-071126
		- Closed: 20260829-093755

	- ✅ Item 30: every run flags advisory noise, so the session-start lint check never reads clean.
		- cargo-deny warns about three duplicate crates inside the profiling chain that is never released, and the lint report's grep also catches govulncheck's "your code does not appear to call" lines. Allow the duplicates with a reason, and exclude the informational block.
		- Fixed: the three duplicates are allowed with a reason, and the report skips the informational block.
		- Opened: 20260829-071126
		- Closed: 20260829-093755

	- ✅ Item 31: the pre-push hook gates the working tree, not the commit being pushed.
		- An uncommitted fix can pass a broken commit; an uncommitted breakage can block a good one. At least warn on a dirty tree; better, run the gate in a detached worktree at the pushed commit.
		- Fixed: the hook runs the gate in a detached worktree at the pushed commit, sharing the target dir.
		- Opened: 20260829-071126
		- Closed: 20260829-093755

	- ✅ Item 32: the Windows setup does not handle a running or older install, and stages files it never installs.
		- No check for a running `shcl.exe` (NSIS pops its own retry dialog), no "upgrading from X" line. The payload stages the man page and completions that the setup never copies.
		- Fixed: the setup checks for a running exe, prints the version it upgrades from, and the payload no longer stages the man page or completions.
		- Opened: 20260829-071126
		- Closed: 20260829-093755

	- ✅ Item 33: `-q` does not flow through to the stage commands.
		- It only suppresses the preflight block; cargo, go, ruff and the packagers run at normal verbosity either way. Either pass each tool's quiet flag or document `-q` as "no prompt".
		- Fixed: `-q` passes each tool's quiet flag where one exists; the usage text says what it means.
		- Opened: 20260829-071126
		- Closed: 20260829-093755

	- ✅ Item 34: nothing asserts the release tag matches the crate version at signing.
		- Cargo.toml is the version source and the three CLI mirrors are gated, but a mistyped tag would publish assets whose names disagree with it. The signing script can refuse when `git describe --exact-match` and the crate version differ.
		- Fixed: signing refuses unless a `v<version>` tag points at HEAD (`--no-tag-check` for rehearsals).
		- Opened: 20260829-071126
		- Closed: 20260829-093755

	- ✅ Item 35: the demo gif runs 33 seconds while the scenario file says 20 to 30.
		- Known and accepted at 33 last round; the comment overstates. Trim a pause or fix the comment.
		- Fixed: the comments, not the gif: the two screenful reads need their pauses, so the scenario says about 33 s.
		- Opened: 20260829-071126
		- Closed: 20260829-093755

	- ✅ Item 36: the profiler workload is forty copies of the corpus, so the flamegraph measures merging, not parsing.
		- Four lines in five merge into an existing node, and a fifth of the samples go to formatting the merge hint. The large-document generator already produces a structured, merge-free document; feed the profiler that.
		- Fixed: the profiler runs on a mid-sized structured document from the shared generator.
		- Opened: 20260829-071126
		- Closed: 20260829-093755

	- ✅ Item 37: the comparison tool's lock file is stale and rewrites itself.
		- It pins the path dependency at 1.2.0 while the crate is 2.0.0, so any cargo run there dirties the tree. Update it at each cut; add it to the cut recipe.
		- Fixed: lock updated, and the cut recipe carries the step.
		- Opened: 20260829-071126
		- Closed: 20260829-093755

	- ✅ Item 38: three lint gaps in the pipeline's own tooling.
		- The flamegraph report and the gif generator are never linted, and fail ruff. The benchmark worker is linted but at ruff's defaults, because the project rule set is not discoverable from that directory.
		- The publish helper is kept out of shellcheck for a reason that no longer applies (it disables the rule itself and passes clean).
		- The PowerShell analyzer settings exclude a rule nothing trips, with a comment describing a choice that was never made.
		- Fixed: the two utilities are linted under the project rule set (E101 off for the cicd files) and pass; the worker too; the publish helper is back in shellcheck; the analyzer exclusion is gone.
		- Opened: 20260829-071126
		- Closed: 20260829-093755

	- ✅ Item 39: nine load-time diagnostic codes are pinned by no corpus case or unit test.
		- E003 to E007 and E009 to E012 appear nowhere in the corpus or the test files. Item 1 lived in that gap. One case per code.
		- Fixed: cases 057 to 061 pin E004 to E007, E009 to E012 and the new E018. E003 cannot come from a file (the `#` of `[#N]` always opens a comment there), so there is nothing to pin.
		- Opened: 20260829-071126
		- Closed: 20260829-093755

	- ✅ Item 40: no sanitizer build anywhere in the pipeline.
		- An address-and-undefined-behavior build of the C test programs found item 12 in one run. It is one extra compile line in the C test stage.
		- Fixed: `sanitize-c.bash` in the test stage builds the runners and the CLI under ASan and UBSan and runs the corpus.
		- Opened: 20260829-071126
		- Closed: 20260829-093755

	- ✅ Item 41: the reference clones every line's value on attach.
		- The value is owned by the attach call and cloned once more in the last-segment arm, where nothing reads it afterwards. It is the top leaf in the newest flamegraph. Reference only; no output change.
		- Fixed: the value is moved into the last segment.
		- Opened: 20260829-071126
		- Closed: 20260829-093755

	- ✅ Item 42: the reference still decodes each element to a code-point array, twice per value line.
		- Go and C dropped this in the 20260817 round; the reference was not touched. Quotes are ASCII, so a byte check of the two ends is exact.
		- Fixed: byte checks through a shared `quoted_shape` helper.
		- Opened: 20260829-071126
		- Closed: 20260829-093755

	- ✅ Item 43: three repeated-work habits on the value-line path.
		- The comment and comma splitters walk every character with no "contains" fast path (Rust and Go; Python has one).
		- Every value line is comma-split twice, once for the unterminated-quote check and once to parse (all four).
		- Every value line's source text is cloned and then dropped in the common case (Rust and Go).
		- Fixed: contains fast paths on both splitters and the quote check, and the source text is copied only when stored.
		- Opened: 20260829-071126
		- Closed: 20260829-093755

	- ✅ Item 44: C string reads and saves grow the document arena on every call.
		- A string read copies the value into the never-freed arena even with no escape to resolve: a long loop of reads of one field grew a document by tens of megabytes. A save retains a full copy of the output each time.
		- Return the element slice when there is no backslash, as Python does; emit from scratch for a save.
		- Fixed: a read with no backslash returns a view of the element, and a save emits into scratch. Fixture pins the arena size over a long loop of reads and saves.
		- Opened: 20260829-071126
		- Closed: 20260829-093755

	- ✅ Item 45: every path read scans the parent's children, so reading every key is quadratic.
		- Reading a flat document key by key takes seconds at a few thousand keys and tens of seconds at tens of thousands. design.md recommends exactly that loader pattern. The parse-time index is thrown away after the parse.
		- Keep a per-parent name index (built lazily, invalidated by the writer), or document the cost per read.
		- Fixed: a lazily built name index (first child per parent and name, chained to the next same-named sibling), dropped by every write. Tens of thousands of keys now read in well under a second. All four bindings.
		- Opened: 20260829-071126
		- Closed: 20260829-093755

	- ✅ Item 46: the writer's duplicate fold builds a key string per same-named sibling.
		- Quadratic in sets under one parent, with a string allocation per compare. The parser already has an allocation-free hash-and-equal pair; the writer should use it.
		- Fixed: the fold compares by hash and equality.
		- Opened: 20260829-071126
		- Closed: 20260829-093755

	- ✅ Item 47: smaller allocation habits, one pass.
		- Emit builds up to three pad strings per node and a joined vector per element list (Rust and Go). Validation builds two full-path strings per node. The repeated-leaf hint pass allocates a vector per child. `[#i]` collects every sibling to pick one. Go's quoted-name reader grows a rune slice. Python has a dozen per-character loops that a `str` builtin replaces exactly.
		- Fixed: the listed habits in the reference and the ports (pads, joined lists, `[#i]`, the quoted-name reader, the Python loops). Left as is: the validation sweep's per-node path strings and the hint pass's per-child list, both bounded by node count.
		- Opened: 20260829-071126
		- Closed: 20260829-093755

	- ✅ Item 48: pipeline scripts fork inside loops.
		- The crosscheck wraps each of its 8000 launches in an extra subshell for an exit-code sentinel, probes for NUL with `tr | wc` per case, and calls `basename` per label. The large-document gate samples memory with an awk fork twenty times a second for the whole run. The rotation helper runs `date -d` five times per file. The flamegraph report builds regexes per frame.
		- Fixed: one fork per CLI launch, a fork-free NUL probe, no `basename`; the memory sampler reads `/proc` without forking; one date call per file; regexes compiled once and rows bisected.
		- Opened: 20260829-071126
		- Closed: 20260829-093755

	- ✅ Item 49: clones in the reference that need a borrow or a reason.
		- One per parsed line in the parent resolver, where a borrow compiles; two before iterating another document's children, where no borrow conflict exists (Go mirrors both as copies); five more that are needed but say nothing about why.
		- Fixed: the parent resolver borrows, the overlay walks the other document's children directly, and the clones that stay say why.
		- Opened: 20260829-071126
		- Closed: 20260829-093755

	- ✅ Item 50: the build script and the comparison tool are off house style.
		- No license header on `build.rs` (it is in the published crate) or the three comparison sources; the comparison tool uses the shell bullet rule as a Rust banner, and has two unexplained unwraps.
		- Fixed: headers on all four files, banners gone, the two unwraps explained.
		- Opened: 20260829-071126
		- Closed: 20260829-093755

	- ✅ Item 51: small Rust and Go structure fixes.
		- Twelve extract-or-bail matches where the file already uses `let .. else`. One catch-all arm standing in for a single named variant. CLI kind and on-bad state carried as strings and matched by string. A handful of trailing comments past the wrap width. One exported Go method without a doc comment.
		- Fixed: `let .. else` where the bail arm binds nothing, the raw read names its variant, the CLI's kind and on-bad are enums (Go too), lines wrapped, the doc comment added.
		- Opened: 20260829-071126
		- Closed: 20260829-093755

	- ✅ Item 52: three functions nest nine tabs deep with repeated bail blocks.
		- The attach path, the array reader and the schema node check. A per-slot helper flattens each. Cross-binding, so it costs more than the rest of this list.
		- Fixed: `find_by_value`, `coerced`, and the parse-all form in the schema node check, mirrored where the language has it.
		- Opened: 20260829-071126
		- Closed: 20260829-093755

	- ✅ Item 53: the datetime type is `ShclDateTime` in Rust and Python and `DateTime` in Go, and the style guide records neither as the deviation.
		- Renaming the Rust type is a breaking change; recording the Go name and adding an alias is not.
		- Fixed: `DateTime` is an alias in Rust and Python, and the style guide records the Go name as the deviation.
		- Opened: 20260829-071126
		- Closed: 20260829-093755

	- ✅ Item 54: Python tidy.
		- The conformance runner formats with `.format()` ten times. The library's public functions are mostly unhinted (a known gap with no item tracking it). Three files opened without `with`; one dead helper; one broad `except` with no reason. The cicd utilities use `os.path` where `pathlib` fits.
		- Fixed: f-strings, the public functions hinted, `Path.touch()`, the dead helper removed.
		- Opened: 20260829-071126
		- Closed: 20260829-093755

	- ✅ Item 55: C tidy.
		- gcc 15 at a stricter level reports one sign conversion, one cast that drops const, and three shadowed locals in the test runner; cppcheck lists 22 pointers that could be const. The 64-byte datetime buffer is a bare literal in five places. The one-letter string typedef cannot be searched for.
		- Fixed: clean under gcc 15's stricter set, the const pointers made const, `SHCL_DT_BUF` for the datetime buffers, and the alias is `Str`.
		- Opened: 20260829-071126
		- Closed: 20260829-093755

	- ✅ Item 56: C++ veneer structure.
		- `generate` takes an out-parameter where every other binding returns a pair; the diagnostic struct has no member initializers; two lines spell bare `size_t`.
		- Fixed: `generate` returns a pair, the diagnostic has initializers, `std::size_t`.
		- Opened: 20260829-071126
		- Closed: 20260829-093755

	- ✅ Item 57: PowerShell tidy.
		- The installer is not an advanced script, passes paths positionally, has a helper with no verb, and three one-letter names. The runner script has no comment-based help, so `Get-Help` shows nothing for its parameters.
		- Fixed: advanced script, named parameters, `Exit-Install`, real names; the runner script has comment-based help.
		- Opened: 20260829-071126
		- Closed: 20260829-093755

	- ✅ Item 58: the rotation helper's bucket maps have one-letter names.
		- Fixed: renamed.
		- Opened: 20260829-071126
		- Closed: 20260829-093755

	- ✅ Item 59: strict-failure lines on stderr omit the diagnostic code that every other stderr line carries.
		- Fixed: the code is printed, in the same form as the write-back lines.
		- Opened: 20260829-071126
		- Closed: 20260829-093755

	- ✅ Item 60: two stale claims on the front page.
		- "Two of them carry the CLI as well as the library": only the crate does since 2.0, and the same section says so two paragraphs down.
		- "Bindings are byte-for-byte identical": their output is; the sources are not.
		- Fixed: both sentences.
		- Opened: 20260829-071126
		- Closed: 20260829-093755

	- ✅ Item 61: design.md and the two tables of contents.
		- design.md sends readers to a notes file that is not in the repo; its lockstep example uses the 1.x version constraints; both its TOC and the README's have drifted from the headings (anchors still resolve).
		- Fixed: the pointer is gone, the example is on 2.x, both tables of contents match their headings.
		- Opened: 20260829-071126
		- Closed: 20260829-093755

	- ✅ Item 62: CODEOWNERS guards a file that does not exist.
		- The donation page entry came from a sibling project. Drop it or point it at the support section.
		- Fixed: entry dropped.
		- Opened: 20260829-071126
		- Closed: 20260829-093755

	- ✅ Item 63: spelling and word-choice pass.
		- Six British spellings in prose and comments. "honest" eleven times, "crucial" once, "key" as an adjective twice, "human" nine times outside the policy doc. The trademark contact is obfuscated with a non-ASCII glyph unlike the other two files. Three typos in the guidelines doc; "Github" once in the backlog.
		- Fixed: across the repo; the spec's diagnostics paragraph says prose message now.
		- Opened: 20260829-071126
		- Closed: 20260829-093755

	- ✅ Item 64: README grammar and the ten worst run-on sentences.
		- Five README sentences read wrong (a dangling relative clause, two parenthetical-dash constructions, a non sequitur, a badge alt text with the shebang backwards). Four README and six design.md sentences run past 45 words on dashes and semicolons.
		- Fixed: the five sentences rewritten, the ten run-ons split.
		- Opened: 20260829-071126
		- Closed: 20260829-093755

	- ✅ Item 65: performance prose versus the numbers under it.
		- "Slightly (trivially) slower to read" sits above a stress-read row that is eight times JSON; "columns are ordered fastest to slowest" is false for the schema table; the unit labels mix KiB, KB and MiB over decimal values. Numbers-only edits by rule; the prose needs a look from its author.
		- Fixed: labels are KB and MB, the ordering sentence is true as written, and the slower-to-read sentence agrees with the numbers, with no number changed.
		- Opened: 20260829-071126
		- Closed: 20260829-093755

	- ✅ Item 66: the file-tier story is told five times in the README.
		- The same three-line comment heads four code examples, and the temp-file-and-rename explanation appears in full three times. One home for each.
		- Fixed: the load comment lives on the Rust example and the save story in What saving does; the rest point there.
		- Opened: 20260829-071126
		- Closed: 20260829-093755

	- ✅ Item 67: the front-page pitch.
		- The tagline is a boast; the "You get" block lists deliverables, not benefits; the provable claims (never deletes a line you typed, signed and reproducible releases, comments survive) first appear far down. Sponsorship is one line near the end and the badge row has no sponsors badge while five badges are decoration.
		- The About panel's homepage points at the spec; the topic list names the competing formats.
		- Fixed: plain tagline, a benefits list leading with the provable claims, a sponsors badge in a shorter row, the homepage points at the README, the competing-format topics are gone.
		- Opened: 20260829-071126
		- Closed: 20260829-093755

	- ✅ Item 68: backlog upkeep.
		- Most Done sub-bullets lack the Cause / Fixed / Verified prefix; one closed item quotes a crosscheck total that is not comparable across corpus changes.
		- Done: the defect-report line under Done - Bugs carried a private absolute path; replaced with a plain pointer.
		- Fixed: a label on the Done sub-bullets whose role is clear (128 of them); narrative and reasoning bullets stay as written. The quoted crosscheck total is gone.
		- Opened: 20260829-071126
		- Closed: 20260829-094951

- Code review 20260822:

	- Second standards pass, same scope as 20260819. One real bug, the rest polish; all of it settled in one round. Items 2 to 9 are here; 1 is under Done - Bugs.

	- ✅ Item 2: installer output and dev-channel resolution.
		- Done: all three install scripts now open and close with a blank line and put one between output sections.
		- The dev channel listed one release and took it, and the API orders that list by publish date - so a maintenance release cut on an older line would win. Both installers now list up to 100 and take the highest version, with a final outranking its own pre-releases.
		- Done: `install-dev.bash` confirms the same way `install.bash` does (try the terminal read, treat "cannot ask" as abort) and accepts "yes". `install.ps1` consults `PROCESSOR_ARCHITEW6432` and turns the progress bar off for the downloads.
		- Opened: n/a
		- Closed: 20260822-115416

	- ✅ Item 3: the Go build, vet, test, staticcheck and govulncheck steps ran uncapped. They honor the same half-the-cores cap as cargo now, via `GOMAXPROCS`.
		- Opened: n/a
		- Closed: 20260822-115416

	- ✅ Item 4: `--quick` still ran the long fuzz soak. It now swaps in the short depth; the long soak stays on full runs and the gate.
		- Opened: n/a
		- Closed: 20260822-115416

	- ✅ Item 5: the dogfood fallback dir needed root, so it could never take. Replaced with `~/.local/bin`.
		- Opened: n/a
		- Closed: 20260822-115416

	- ✅ Item 6: the demo gif ran 110 seconds. Cut to four steps at 33, and the high-level script now lives at `cicd/demo/script.txt` beside the scenario.
		- Opened: n/a
		- Closed: 20260822-115416

	- ✅ Item 7: 60 public items in the Rust reference had no doc comment where the Go binding documents the same inventory. Filled from the Go text; one stranded doc block (`quote_segment`'s, sitting on `format_f64`) moved home.
		- Opened: n/a
		- Closed: 20260822-115416

	- ✅ Item 8: comment tidy. The Python binding's five ad-hoc group dividers folded to plain comments per the style guide. The Go date cluster's `Atoi` discards got their safety note. The style guide's lowercase-filename rule now names all the tool-fixed exceptions.
		- Opened: n/a
		- Closed: 20260822-115416

	- ✅ Item 9: doc pass - README grammar and casing (`SHCL` throughout, one spelling of bulletproof), backlog and design.md long paragraphs broken into sub-bullets, the release section of design.md trimmed to decisions, and a stale pre-repo file reference reworded.
		- Opened: n/a
		- Closed: 20260822-115416

- Code review 20260819:

	- Standards pass rather than a defect hunt: code style, performance, the pipeline, docs, the README pitch and the installers, each checked against how it is supposed to work. No correctness defect turned up, so everything here is a polish gap. Items 1 to 6, 8 to 16 and 18 to 21 are here; 7 is under Future and/or deferred; 17 is under Canceled.

	- ✅ Item 1: the pipeline never refreshes from the remote before it runs.
		- Cause: the only pull happens inside the publish stage, after build and tests. Anything merged upstream in the meantime gets pushed without the pipeline having seen it.
		- Wanted: a sync step ahead of stage 1 that fetches, fast-forwards when only behind, and stops early when the branches have diverged. Offline or no upstream should warn and carry on. Needs a flag to skip it.
		- Publish keeps its own pull either way, as a second guard.
		- Done: a sync stage ahead of the format stage. Fast-forwards when only behind, wrapping a dirty tree in a stash; stops the run when the branch and its upstream have both moved; warns and carries on when offline, untracked, or on a detached head. `--no-sync` skips it, and `--ci` turns it off since the runner already has the exact commit.
		- Done: a stash pop that conflicts now stops the run rather than letting the build proceed over conflict markers. Git keeps the entry, so the work is recoverable.
		- Opened: 20260819-111243
		- Closed: 20260819-121316

	- ✅ Item 2: Windows executables carry no icon and no file metadata.
		- Cause: nothing in the repo embeds a resource, and there is no icon file to embed. The cross-built exe gets the generic shell icon, and its properties panel is blank where a version, description and copyright belong.
		- Affects both Windows targets and the setup that carries them.
		- Done: a build script writes the resource and hands it to whichever resource compiler is present, so both Windows binaries now carry the icon and a filled-in properties panel - product name, description, version, company and copyright.
		- The version comes from `Cargo.toml` through the build environment, so it cannot drift and the release bump still touches the same eight files.
		- No new dependency, and nothing is required: a build with no resource compiler around warns and carries on rather than failing.
		- The setup gets the same icon and its own metadata.
		- `assets/shcl.ico` is built from `assets/icon.png`, the project mark. Seven sizes from 16 up to 256; the four letters stay readable at the smallest.
		- Opened: 20260819-111243
		- Closed: 20260819-124948

	- ✅ Item 3: nothing addresses reproducible builds.
		- Two machines building the same tag should produce the same checksum. Never attempted, so it is unknown whether the build is already close.
		- Assess before promising anything: if it holds, say so in README.md, since the release already publishes signed checksums and that is what a reader would want to check against.
		- Verified: it now holds: the same commit builds byte-identical on all four release targets, from different directories.
		- One thing was actually broken. The Windows linker stamps the build time into the header, so two builds of one commit differed in exactly two bytes. Both Windows targets now ask the linker not to, each in the spelling its own linker accepts.
		- The other targets already reproduced and needed nothing.
		- Done: README.md says so now, next to the checksum instructions, since verifying a download against a checksum you produced yourself is the point.
		- Opened: 20260819-111243
		- Closed: 20260819-124948

	- ✅ Item 4: no runner for the latest build.
		- A sister project keeps a script that copies the newest release build somewhere off `PATH` under a timestamped name, ages out the copies nothing is using, and launches the newest with arguments passed through. The dogfood stage covers the installed copy only.
		- One cross-platform PowerShell script is enough.
		- Done: `cicd/utility/n8runshcl.ps1`. Stamps a dated copy of the release build into `cicd/artifacts/runbuilds/`, well off PATH, and launches it with every argument passed through. `-ListCopies` reports what is staged, `-NoLaunch` just stages and prints the path, `-Keep` sets how many to retain.
		- The stamp comes from the build's own timestamp, so running twice against one build reuses its copy instead of piling up duplicates.
		- Aged-out copies are removed only when nothing is running them - checked by an exclusive open on Windows and by which image each process was started from elsewhere.
		- Opened: 20260819-111243
		- Closed: 20260819-121316

	- ✅ Item 5: no advisory gate, and Go is linted by `go vet` alone.
		- Nothing runs cargo-deny, cargo-audit, govulncheck or pip-audit. Nothing would report a toolchain advisory.
		- A small dependency set, not an empty one: the libraries are dependency-free by design, but the profiler chain, the Go standard library version and the pinned toolchain are all real.
		- `staticcheck` is expected of Go and is not wired in, though it has been run by hand and was clean.
		- Done: staticcheck and govulncheck join go vet, and cargo-deny runs over the rust lockfile with all features on. All three are pinned like the rest of the tooling, so a result that changes because the tool moved says so.
		- The deny config is `source/rust/deny.toml`: permissive licenses only, unknown registries refused, a known vulnerability fails the run.
		- Two advisories are recorded as accepted rather than hidden. Both are quick-xml 0.26 under inferno under pprof - nothing here parses XML, the chain is behind the profiling feature and is never released, and inferno pins that version so there is nowhere to move. They come out when pprof carries a newer inferno.
		- Done: a dependency refresh went with it and cleared the yanked `spin` the release notes used to have to explain.
		- Opened: 20260819-111243
		- Closed: 20260819-123753

	- ✅ Item 6: the gate is not wired to a pre-push hook.
		- `--ci` already is the gate. Nothing runs it automatically, so the only automatic check happens after a push, in the hosted workflow.
		- A hook would catch it before the push instead.
		- Done: `cicd/hooks/pre-push`, enabled by pointing `core.hooksPath` at `cicd/hooks`. `install-dev.bash` sets it up.
		- It only runs the gate when the push would move `main` or `dev`; feature branches push without waiting. It leaves out the large-document stage, which the hosted workflow still covers.
		- `git push --no-verify` overrides it.
		- Opened: 20260819-111243
		- Closed: 20260819-121316

	- ✅ Item 8: the Python linter runs with almost no rules, and neither linter nor type checker is configured where it can be seen.
		- Cause: `ruff` at defaults is pycodestyle errors plus pyflakes. None of the Python style rules apply at that selection.
		- Cause: `pyproject.toml` has no `[tool.ruff]` and no `[tool.mypy]`. The type checker's strictness is a comment at the top of `shcl.py`, so it covers that one file - the CLI and the test runner are checked at bare defaults, where an untyped function passes.
		- A full rule selection was tried before and mostly conflicts with the parity rule. The answer is a curated selection plus config that lives in `pyproject.toml`, not the full set.
		- Done: `[tool.ruff]` and `[tool.mypy]` in `pyproject.toml`, so running either by hand gives what the pipeline gets.
		- The rule selection is curated, and every exclusion says why. Most of what a full selection reports is forms this binding has to keep to stay in step with the reference; the ones that were real are fixed.
		- Done: the type check now covers all three files instead of one, and found two genuine errors in the test runner - one name bound to both a text and a binary file handle in the same scope.
		- Opened: 20260819-111243
		- Closed: 20260819-123753

	- ✅ Item 9: the Python CLI builds every message with `.format()`.
		- About a dozen sites. The package claims 3.9 and up, where f-strings have always been available.
		- Cosmetic, but it is the one place the Python binding reads dated.
		- Done: 149 sites across the library and both CLIs.
		- Verified the messages did not move: every diagnostic the corpus produces is byte-identical before and after, and the four bindings still agree.
		- 42 remain in the test runner, where the conversion is not one-for-one. Left alone.
		- Opened: 20260819-111243
		- Closed: 20260819-123753

	- ✅ Item 10: five `raise` statements inside an `except` drop the original cause, and two suppression comments are dead.
		- One is in the released CLI, on the path that reports a stream that was not valid UTF-8. The other four are in the Python test runner.
		- The two dead comments suppress a rule that is not enabled, so they suppress nothing.
		- Done: the CLI's decode failure now carries its cause, and the four in the test runner drop theirs deliberately - an assertion failure is not caused by the exception it caught.
		- Done: the dead suppression comment is gone; the other turned out to be live once the rule set grew.
		- Opened: 20260819-111243
		- Closed: 20260819-123753

	- ✅ Item 11: the whitespace table's characters read as mistakes.
		- 16 of them are spelled as literal invisible characters in the Python module, which is exactly what a linter flags as an ambiguous character. They are deliberate, and nothing in the file says so.
		- The C copy spells the same set numerically. Either spelling works; what is missing is the note that the Python one is on purpose, so nobody "fixes" it and splits the bindings.
		- Done: the table carries a suppression on each of its lines, with a note saying the characters are deliberate and that changing the set means changing it in all four bindings at once.
		- Opened: 20260819-111243
		- Closed: 20260819-123753

	- ✅ Item 12: the two style documents disagree about indentation.
		- One asks for four spaces in Rust, Python and PowerShell. The project uses hard tabs in all three, and `rustfmt.toml` sets `hard_tabs` to get it - which is also a formatter default being overridden, in a rule set that says not to override them.
		- Not something to change quietly in either direction. Every binding and every conformance golden would move.
		- Needs a ruling on which document wins.
		- Settled: tabs, unless a language prevents it or pushes hard the other way. None of the six here do, so all six stay on tabs and no code moves.
		- Done: `style-guide.md` carries the reasoning, including that PEP 8 itself asks for consistency with existing tab-indented code, and that the Python tab rule is switched off on purpose rather than by oversight.
		- Opened: 20260819-111243
		- Closed: 20260819-123753

	- ✅ Item 13: three languages have no style enforcement file.
		- Missing: a golangci config, a clang-format and clang-tidy pair, and a PSScriptAnalyzer settings file.
		- C is the only binding with no format gate at all, which is deliberate, since there is no zero-dependency formatter to commit, but it means C style is held by review alone.
		- Some of this is already settled: pedantic clippy is advisory, and shfmt and clang-tidy were both rejected as gates. The rest was never decided.
		- Done for PowerShell: `PSScriptAnalyzerSettings.psd1`, applied to all three scripts. It pins the severity set and checks syntax against both 5.1 and 7.
		- That check found the wrapper using an operator only 7 understands. Rewritten the long way, so the wrapper now runs on the PowerShell that comes with Windows.
		- Declined, with reasons recorded in `style-guide.md`: clang-format and clang-tidy would rewrite about nine lines in ten of the C binding; golangci-lint wraps checks that already gate on their own.
		- Opened: 20260819-111243
		- Closed: 20260819-123753

	- ✅ Item 14: the release profile was never compared for size.
		- Size and speed are the two priorities for a release binary, and only speed has been chosen for. The size-first optimization level has never been built or measured against the current one.
		- Cheap to settle: build both, compare bytes and the large-document timings, keep whichever wins.
		- Measured on a large document. Size-first costs far more speed than it saves space: the smallest setting is a fifth smaller and half again slower, and the middle one saves less and still costs speed.
		- Decision: keep the speed-first setting. A modest saving on a small binary does not justify halving the throughput of a tool whose whole job is reading large files.
		- Recorded so this does not get re-asked.
		- Opened: 20260819-111243
		- Closed: 20260819-132623

	- ✅ Item 15: a release behind in build leftovers.
		- `source/python/dist/` still holds the 1.2.0 wheel and archive, and the metadata directory beside it matches. Ignored by git, so they sit there indefinitely.
		- The release recipe already says to clear them before a cut. Clearing them now costs nothing.
		- Done: removed. They regenerate at the next cut.
		- Opened: 20260819-111243
		- Closed: 20260819-132623

	- ✅ Item 16: two run-on bullets in `spec.md`.
		- The pair covering `--lossy` and what a bare `-` means. Both pack several clauses into one sentence with dashes doing the joining, and they are the only two top-level bullets in the file not separated by a blank line.
		- Done: split into separate sentences and bullets, and the neighboring bullet above them had the same problem and got the same treatment. No top-level bullet in the file is missing its blank line now.
		- Opened: 20260819-111243
		- Closed: 20260819-132623

	- ✅ Item 18: the "5/10" disclosure under the projects table looks low.
		- Counting the table, more than five entries appear to trace back to the same author once the company-hosted ones are included.
		- The line exists to build trust. An undercount does the opposite, so get it exactly right or drop the count and name the relationship instead.
		- Done: reads "most of these are by the same author" now. A count that can be argued with is worse than none, and the disclosure is the point.
		- Opened: 20260819-111243
		- Closed: 20260819-132623

	- ✅ Item 19: the repository About panel is thinner than it needs to be.
		- No website link at all, though the spec and the generated API docs are both obvious candidates.
		- The topic list names Rust, Go and Python but not C, C++ or the CLI, so two of the four bindings and half the product are invisible to topic search.
		- Done: the About panel points at the spec, and the topic list gained c, cpp, cli and config-management.
		- Opened: 20260819-111243
		- Closed: 20260819-132623

	- ✅ Item 20: one project in the table has a blank release status.
		- Done: filled in as in development, which is what the repository shows - active, no release yet. Needs a second look if that is not the intended wording.
		- Opened: 20260819-111243
		- Closed: 20260819-132623

	- ✅ Item 21: the Windows installer has no help switch.
		- The Linux one answers `--help`. The Windows one relies on the comment block at the top of the file, which the documented one-liner cannot reach - it pipes the script straight into the shell, so there is nothing left to ask for help about.
		- Everything else in both installers matches: signature checked before any checksum, idempotent, states its plan and asks, detects the architecture, and uninstalls only what it laid down.
		- Done: `-Help` prints the options and exits. Written into the script rather than left to the comment block, because the documented one-liner pipes the script into the shell and leaves nothing to ask about.
		- Done: README lists it alongside the others.
		- Opened: 20260819-111243
		- Closed: 20260819-132623

- Code review 20260817:

	- Items 17 to 24 and 26 to 30 are here; 1 to 16 are under Done - Bugs; 25 is under Canceled.

	- ✅ Item 17: the save gate's failure channel is wrong in three bindings.
		- Cause: Python returns an error string, so `doc.save_file(path)` on its own line, the obvious spelling, silently does nothing when the gate fires and the program reports success. That is worse than the loss it prevents, because at least a lossy save leaves a file. It should raise.
		- Cause: the reference returns a plain-string error, which does not compose into a caller's error type and makes the refusal distinguishable from a disk failure only by matching on prose.
		- Cause: C returns the same value for "refused for safety" and "the write failed", and the header never mentions the gate or that the lossy call is its override.
		- All three are free to fix now and expensive after the cut, because the whole file tier is unreleased.
		- Fixed in all four plus the veneer, one channel per language: `SaveError::Refused`/`Io` in the reference, a `*SaveRefused` error in go, `SaveRefused`/`SaveFailed` raised from a `SaveError` base in python, and `SHCL_SAVE_OK`/`REFUSED`/`FAILED` in c. The header now states the gate and names the lossy call as its override.
		- The four CLIs stopped sniffing `lost_count() > 0` to guess which failure they were looking at and branch on the value. Output is byte-identical.
		- The veneer's `save_file` returned a bool, which folded the same two cases, so it returns the result type now - and gained `save_file_lossy`, since a refusal it cannot override is a dead end. The rest of item 21's veneer list closed under item 21.
		- Verified: new shared fixture in all four runners: the gate answers before any i/o, so a lost document saved to an unwritable path still reports the refusal, and a clean one reports the write failure. Nothing here is visible on stdout, so the corpus cannot see it.
		- Opened: 20260817-204524
		- Closed: 20260818-142148

	- ✅ Item 18: the documentation teaches the bug the file tier was built to remove.
		- Every headline example in the front-page readme and the per-binding readmes hand-rolls file i/o: a plain read, and a plain non-atomic write back. The file tier, the load status, the lost-line count and the as-authored name appear in no readme at all.
		- A newcomer copying the example ends up with a config writer that can truncate a file on interrupt and silently drops lines the parser could not read.
		- The package pages bake their readme in permanently at publish, so a bad example on the next cut is unfixable for that version.
		- Also: the front-page readme states a schema-fault rule the code does not implement - a key-level fault no longer switches off the unknown-field sweep. The spec has it right; the readme is stale from that chunk.
		- Also: installation sits about four fifths of the way down the front page, after seven language examples. The first screenful is good; the ordering is not.
		- Fixed: all seven front-page examples and both per-binding readmes load and save through the file tier now. The load status is in each one, and the lost count, the save gate and the as-authored name are in "What saving does". Features gained a file-tier bullet.
		- Verified: every example builds and runs, zig included, the c one under the readme's own `-std=c11` line. The zig note about its standard library moving between releases got shorter rather than longer: routing the file work through the c tier removes the part that was actually unstable.
		- Fixed: the schema-fault sentence now matches the code: a broken constraint keeps its entry so the sweep still runs, and only a lost path spelling turns it off.
		- Fixed: installation moved above "Using the CLI" - install, then use, then embed. The one "below" in a cross-reference went with it.
		- Opened: 20260817-204524
		- Closed: 20260818-144317

	- ✅ Item 19: a setter returning false is a real failure mode that nothing makes discoverable.
		- Cause: setters report failure only through their return value, and every documented example discards it. The write-reason call exists, is well designed, and is referenced from nowhere a reader will look.
		- A wildcard path or an out-of-range instance applies nothing, returns false, and the program then saves a config missing the change and reports success. In the reference it compiles without a warning.
		- Fix: mark the setters must-use in the reference, and have at least one example per binding check a setter and print the reason. Both additive.
		- Fixed: all 26 reference setters are `#[must_use]` now. Only five sites in the whole repo were discarding one, all in tests, since the CLIs already checked. Each became an assertion, including one that pins a too-deep path as not writable.
		- Fixed: the examples check a setter and print the write reason: all three writes in the front-page rust one (must_use leaves no choice), one write in go, python and c, and one in each per-binding readme. Every example builds and runs, the rust ones under `-D warnings`.
		- One sentence about the ignored-false consequence went into all four setter docs plus the spec; the rust-only annotation is recorded as a sanctioned API deviation in the style guide.
		- Opened: 20260817-204524
		- Closed: 20260818-145500

	- ✅ Item 20: the same call name sits on a different tier in each binding.
		- Cause: the reference and go put `get` on the status tier. Python puts it on the convenience tier with a raising mode. C puts it on the convenience tier with a mandatory default argument.
		- Each is defensible alone; the set is not, while the product's pitch is that a version means the same behavior in every language.
		- Porting a routine between two of them keeps the call name, changes the arity, and converts "I inspect the status" into "I never find out".
		- Fix additively: add the `_or` spelling the spec's own table already uses where it is missing, and keep the existing names. Removing anything is breaking.
		- Fixed that way. Rust and python gained all eleven `get_*_or`, c the three its convenience tier covers; go already had them. Nothing was removed - the native idiom still reads the same (`get_int(p).unwrap_or(0)`, `get_int(p, default=0)`), and the plain `get_*` keeps whatever it meant in each binding.
		- `_or` now means "with a fallback" in all four with no exception to remember, which is the property that makes the spec's table portable rather than a per-binding lookup. The table and its surrounding paragraph say so.
		- Verified: pinned by extending the existing convenience-tier fixture rather than adding a second one, and that fixture now exists in all four runners - it was rust and go only.
		- Opened: 20260817-204524
		- Closed: 20260818-152706

	- ✅ Item 21: inventory gaps between the bindings.
		- Go is missing all five array forms of the status tier, so it offers three tiers for scalars and two for arrays.
		- C and c++ cannot read the quoted flag at all. This round changed the formatter specifically so a consumer can tell a reserved word from a quoted plain string, and half the bindings cannot make that distinction.
		- The c++ veneer has only half the new file tier: no lost count, no lossy save, no strictness form, and its array reads drop the per-slot statuses. A user whose save returns false has no route to the reason without dropping to the raw pointer the veneer exists to hide.
		- Go's load status is the only enum in the package with no text form, so logging one prints a number.
		- All additive. Should go in the same cut as the tier.
		- All four closed. Go gained the five `Get*Array` reductions and a `String` on the load status; c and the veneer gained a `quoted` accessor, which sits beside `line` rather than in the read structs for the same reason the raw text does.
		- The veneer gained the rest of the file tier (lost count, the strictness form, a name for the load status) plus per-slot statuses on every array read, a datetime array read it never had, `exists`, and a status-to-text helper. The lossy save went in with item 17.
		- C got the same load-status name function, since `shcl_status_name` exists and a file status had nothing - the gap the item names in go was in c too.
		- Also fixed while in there: c's doc comment for the lost count was the error count's, so the lost count was undocumented and the error count read as describing the wrong call. The same defect item 16 fixed in go.
		- Verified: the veneer smoke now covers the file tier at all, which it did not before.
		- Opened: 20260817-204524
		- Closed: 20260818-152706

	- ✅ Item 22: the spec normatively describes a parameter no binding implements.
		- Cause: the spec calls the three on-bad modes the canonical API everywhere and even names python's spelling for it. It exists only as a cli flag; no library has it.
		- The two existing tiers already cover two of the three modes as described. Only the spec is wrong.
		- Fix: describe the tiers as built, and mark the per-slot substitution behavior cli-only. Do it before the cut so the published spec matches the published api.
		- Fixed that way, in the spec's core-call section and the six other places that referred to on-bad as a per-call parameter. The mode is now stated as which tier you call: the full tier is Flag, the convenience tier is Default, and Error has no library form at all. A read that cannot reach a value is a normal outcome here, so a caller who wants a throw raises on the status themselves.
		- Per-slot substitution is marked cli-only for the same reason it exists there: a shell caller has no slot list to inspect.
		- design.md's three accessor bullets said the same thing and were rewritten with it, so the decision and the spec cannot drift apart again.
		- Opened: 20260817-204524
		- Closed: 20260818-153553

	- ✅ Item 23: two documented behaviors that quietly disagree.
		- The `ok` helper counts empty as fine; the convenience tier falls back on empty. So there are two blessed ways to ask "did I get a value" that answer differently for an explicitly emptied field - which is exactly the case the empty-versus-missing distinction is sold on.
		- The declared-repeat and declared-reopen suppressors are applied automatically only by the combined load-and-validate call. A caller who parses and then validates, the composition the api most obviously invites, gets the hints a schema explicitly disavowed, with nothing at the call site saying so.
		- Both are doc-only fixes: one sentence each, in all four.
		- Both done. The `ok` sentence names the divergence rather than hiding it: one asks whether the author spoke for the field, the other whether there is a usable value, and an explicitly emptied field is where they part.
		- Not doc-only in the end for `ok`: it existed in rust, python and the veneer only, so go gained `Ok` and c a `shcl_status_ok` predicate. A status predicate rather than a per-struct helper, since all five read structs carry the same status. Documenting a distinction two bindings could not express would have been the wrong half of the fix.
		- The suppressor sentence says the hints live on the parse's diagnostics, which validation does not touch, and names both the manual calls and the one-shot that runs them.
		- Verified: the divergence is pinned in all four runners, not just described.
		- Opened: 20260817-204524
		- Closed: 20260818-153553

	- ✅ Item 24: two names to settle while they are still free.
		- The as-authored name call reads as though it might return the file it came from, now that loading from a file exists. "Source" already means the source text and the source line elsewhere in the api.
		- The c++ date reads are inverted against the rest of the api: the plain name returns text and the "raw" name returns the parsed value, while "raw" everywhere else means the text exactly as written.
		- The first is unreleased, so renaming costs nothing today. The second can be fixed additively by adding clear names and retiring the old pair at a major.
		- ✅ First half done: `SourceName` is `AuthoredName` in all four bindings plus the veneer, with no alias, since no release carries it. "Authored" says what it returns without borrowing a word the api already spends on the source text and the source line.
		- ✅ Second half done, with the names major as planned: `read_datetime` is the structured read it is in every other binding, `read_datetime_str` is the textual one, and `read_datetime_raw` is gone. The parity defect underneath the naming one goes with it.
		- Opened: 20260817-204524
		- Closed: 20260818-172643

	- ✅ Item 26: the reference is missing cheap standard trait support.
		- No clone on the document, no parse-from-string trait, no display wrapping the canonical form, no display on the status type, and no public float formatter - which the other three all export.
		- Parsing via the standard trait and printing a document are the first two things a rust user tries. Printing a status today ends up in user-facing messages as debug output.
		- All additive, none of it structural, so the parity rule is untouched.
		- All five done. `Clone` is a derive and correct for free: the arena is index-based, so cloning the vector copies the whole tree with no reference to fix up - pinned by editing a clone and checking the original.
		- `FromStr` carries `Infallible` as its error, not a load error, because parsing at Standard genuinely cannot fail - a malformed line is a diagnostic. The fallible load is still `parse_with` at Strict.
		- `format_f64` is a wrapper over rust's own Display, which already spells floats the way the contract requires. The two setter sites call it now, so the rule has one home rather than a `format!` at each.
		- Verified: Rust-only fixture, deliberately: nothing here is new behavior, and the other three already export the same capabilities under their own names.
		- Opened: 20260817-204524
		- Closed: 20260818-155051

	- ✅ Item 27: performance, six items.
		- The new author-quoting clause runs four full coercions per quoted element on every emit. Emitting a quoted document costs about three times the same document bare, and it falls on formatting, the command most likely to run on a large file. A cheap first-character test or a cached classification removes it.
		- Every setter scans the path and walks the tree twice: the write check does both, throws them away, and the caller redoes them. One of each would do.
		- C routes every per-line temporary into the permanent document arena, so parse memory is many times the input and cannot be reclaimed, several times the reference's on the same file. There is already a scratch arena for exactly this; parsing does not use it.
		- C's emit-path format probe allocates into the document arena too, so each save retains about four times the output size. A long-running program that saves periodically grows without bound.
		- Go decodes to a rune slice and back in three string helpers on hot paths, where every character it matches is ascii. Several times the cost of the byte-loop equivalent, and the binding uses noticeably more memory than the reference.
		- Python calls a per-character predicate from the path scanner and the emitter. It is the top entry by self time in a parse-and-emit profile at about a fifth of the total; a character-set membership test cuts about a tenth off the whole run.
		- All six done, each measured before and after on one large document.
		- Quoting clause: one pass over the bytes gates the four coercions. At standard strictness int, float and datetime all need an ASCII digit, and the only formats that do not are the boolean words, so a plain quoted string is rejected without coercing anything. The extra cost of a fully-quoted document over a bare one fell by more than half.
		- Setters: `write_reason` and `place` share one path scan and one tree walk. The validation walk now records where each segment stopped, so the create pass starts exactly where the path fell off the tree instead of re-walking it. A long run of writes got measurably faster. Validation still runs before anything is created, so a doomed path still leaves nothing behind.
		- C memory and time both fell by about a third. Three parts. The parser's own bookkeeping (the child accelerator above all) moved to the scratch arena. The per-line temporaries got their own arena reset each line. The element parser stopped decoding every element to a code-point array to look at its two ends, which is the same defect as the go item below.
		- C emit: the output is built in scratch and copied into the document arena once, so a save retains exactly its output rather than about four times it. The arena also grows the last allocation in place now, which a bump arena could never do before.
		- Go: the same code-point round-trips, byte-level now: the quote scan, the element parser, the quote counter, the quoting rewrite, and `applyEscapes`, which runs on every string read and every selector compare. Memory down a little, time down by a fifth.
		- Python: the bare-name predicate is a frozenset lookup and the emitter's per-character generator is one C-level set operation. About a tenth off a parse-and-emit run, as filed.
		- Verified beyond the usual gate, since none of this may move a byte: a C build that poisons every arena reset agrees with the reference across the corpus and a fuzz set, and the four-way crosscheck is unchanged.
		- Opened: 20260817-204524
		- Closed: 20260818-162811

	- ✅ Item 28: cli and installer polish, batched.
		- Every read failure is silent at the default mode: a wrong type, a typo'd path and a missing value all give an empty result, a meaningful exit code, and nothing on stderr. The error-mode message is good and is not what anyone gets by default. Printing it to stderr regardless would leave stdout byte-identical.
		- `set` with a file and no ops flag blocks on stdin at a terminal with no prompt and no output, which reads as a hang.
		- `check` is the only subcommand that rejects the layer and set options, so the merged document a program will actually load cannot be validated directly. The pipe workaround is clean but not obvious.
		- No man page and no shell completions, while deb, rpm and an installer are published. The help documents which options exist but not which subcommand each belongs to.
		- The installer fetches and installs a source archive with no checksum and no signature, right after correctly verifying the binary, and marks a script from it executable. The signed sums file has no entry for it.
		- There is no uninstall path at all, and the success message does not say how to undo the install.
		- The installer's no-terminal guard does not fire, so an unattended install dies on a raw shell error instead of the intended message.
		- Smaller ones:
			- Bare `shcl` prints the full help to stdout and exits 1; pick one convention.
			- `-v` is rejected while `-V` works.
			- The prompt accepts only a bare "y" and rejects "yes".
			- The elevation path is chosen without checking the tool exists, so it fails after both downloads.
			- The path note says what is wrong but not how to fix it.
			- The two shell wrappers list different subcommands and neither lists `init`.
			- `--strictness` is rejected on `init` though it parses a shcl file.
		- ✅ Silent reads: the reason now goes to stderr in every mode, so the full message is what a user gets by default rather than what they get only after finding a flag. Stdout is byte-identical. Two silences are deliberate and commented: `--on-bad=default`, where the caller has already said the miss is expected, and Empty outside error mode, since an empty value is a legitimate answer here - the same reason `ok` counts it as fine.
		- ✅ `set` blocking on stdin: it says what it is waiting for before it waits. Unconditional, so a pipeline and a terminal behave identically - the alternative was sniffing the terminal, which this round already rejected once for good reasons.
		- ✅ `check` and `--layer`: the refusal stands, because diagnostics cite line numbers and a merged document has none, but the message now spells out the pipeline that does the job. Same for `--strictness` on `init`, which was never an oversight - a schema is a program artifact and always loads at Standard.
		- ✅ Smaller ones, all done:
			- A bare run now prints the help and exits 0, the same as `shcl help`: one convention, "asking for help succeeds".
			- `-v` works.
			- The prompt takes "yes".
			- sudo is checked for before the plan promises it.
			- The path note carries the export line to paste.
			- Both wrappers list the same subcommands, `init` included.
			- Every help option now names the subcommands it belongs to.
		- ✅ Installer payload: the drop-ins and wrappers are a release asset covered by the same signed sums as the binary, built by the pipeline into the artifact dir before the sums are written. The generated source tarball, which carries no signature at all, is out of the path, and the one file the installer marks executable is now one it verified. A release without the asset installs the binary and says what it skipped rather than falling back to something unverified.
		- ✅ Uninstall: `--uninstall`/`-Uninstall` removes exactly what the matching install laid down (binary, symlink or PATH entry, and the two payload dirs), and the success message names it. It runs before any network call, so removing does not need a release to exist.
		- ✅ The no-terminal guard now asks the question it means: it tries the read and treats failure as the abort. Testing `/dev/tty` for readability passed in plenty of unattended contexts where the read then died on a raw shell error.
		- ✋ Deferred: the man page and shell completions. Those are a new deliverable with packaging consequences (.deb/.rpm placement, a completions dir per shell), not polish on an existing one - filed on their own under Features, where they are now done.
		- Opened: 20260817-204524
		- Closed: 20260819-084518

	- ✅ Item 29: the gaps that let this round through.
		- The cross-binding check cannot see any defect the four bindings share, and most of the bugs above are exactly that kind. The corpus is the only thing that can catch them, and only if a case has the form.
		- The cross-compile stage builds the rust binary only, so the c binding's windows branches have never been compiled here. That is how item 2 got in.
		- The python lint and type gates run at defaults with no configuration, and the module has no type hints - so the type gate analyzes almost nothing and cannot fail. Turning on checking of unannotated bodies shows a real misuse trap immediately.
		- Nothing is built for a 32-bit target, so the size arithmetic there is unverified. (Moot - see the cancellation below.)
		- The writer fuzzer's character set contains no carriage return and no unusual whitespace, which is why item 6 survived.
		- Python exports its own imports and one internal constant, because the module declares no public list.
		- ✅ The type gate is live. Checking of unannotated bodies is on, and the two core data classes carry field types; without those every field was `Any` and the checker still had nothing to check. `assignment` stays off, because the structures mirrored from the reference are tagged tuples and index-or-None locals; everything that catches a real misuse is on. Seventeen locals gained a one-word annotation and two accumulators that changed type mid-function were split in the process.
		- ✅ Python declares `__all__`, so `import *` no longer hands out math, os, stat, Decimal, Enum and an internal constant alongside the API.
		- ✅ The writer fuzzer's character set gained the carriage return and six unusual whitespace characters - the gap that let the edge-whitespace truncation through. It found something immediately: a long soak on the new set found a raw block whose all-whitespace body grows by one indent level on every `fmt`, filed above.
		- ✅ The cross stage runs cross-compile checks as well as artifact builds: the C library and CLI for Windows through mingw, and the library with file I/O compiled out. Neither had ever been built here, which is how the Windows regression reached dev.
		- ✅ The deep soak is written down where it will be seen (`cicd/config.bash`, beside the gate) with the command and the reason. The gate itself was raised once the two fixpoint bugs it found were fixed. Both needed that depth to show up at all, so a gate below it could not see the class that produced them.
		- 🚫 Canceled: a 32-bit build. 32-bit is not a target, so there is nothing to verify. The whole line traces to one observation: C's `decode_cps` sizes an allocation as `(m+1) * sizeof(size_t)` unchecked, which only overflows where `size_t` is 32 bits, and then only past a gigabyte of input. On every supported target that arithmetic is 64-bit and cannot overflow at any input size the parser will accept.
		- ✋ Standing, not fixable by a gate: the cross-binding check cannot see a defect all four bindings share. Only the corpus can, and only if a case has the form. Both bugs filed this round are exactly that kind, and both were found by the fuzzer rather than the differential. That is the practical answer: widen the fuzzer, and add a corpus case whenever it finds something.
		- Opened: 20260817-204524
		- Closed: 20260818-170931

	- ✅ Item 30: names compare by their escaped spelling, so two spellings of the same name are two names. Done: escapes resolve on names, since 2.0.0.
		- Settled alongside Code review 20260817 item 15. A name authored `"q\"r"` is stored, matched and emitted with the backslash intact, so it is a different name from one authored `'q"r'`. The same two spellings as values are the same string, which the spec states outright for selectors.
		- Not a bug against any current contract: every part of the name pipeline agrees, and item 15's fix documents it. But the two halves of the language disagree about what a quoted string means, and a consumer building a path from user text has to know which half it is in.
		- It does not take an exotic character: there are two quote styles, so `'a"b'` and `"a\"b"` are two fields, and the same pair as values are one string. Verified.
		- Decided to resolve, because names already normalize once (ASCII case folds), so this finishes a rule rather than adding one. Rationale in `design.md` -> Guiding principles.
		- Deferred at the time to the next major, since it moves canonical output for a published spelling and merges two fields that were distinct until then. `QuoteSegment` was the mitigation meanwhile.
		- Version cost is a number, not a migration. crates.io and PyPI carry 2.x beside 1.x with nothing to do; only Go pays, since v2 goes in the module path (`source/go/v2`) and every Go consumer edits an import.
		- The plan: two sites per binding, unescape on name parse, and point name emit at the value escaper instead of the minimal name one. `AuthoredName` then becomes the real as-authored escape hatch rather than differing only by case, and item 15's doc decision still holds, since it returns source text. The fiddly part is the schema's two-level path quoting, which gains an unescape level and needs its own corpus case.
		- Do not cut a major for this alone; batch it with item 24's c++ date read pair.
		- Done, batched with that pair. Two sites per binding as planned: escapes resolve where the path scanner stores a segment name, and names emit through a new name escaper. The plan was half wrong: the value escaper could not be reused. It picks a quote style to avoid escaping and never escapes a backslash. That is right for a value (stored in its escaped spelling) and wrong for a name (now stored resolved), so the four bindings gained a real inverse of the name parse instead.
		- The as-authored accessor still hands back the source spelling, deliberately: that is what it is for, and item 15's decision stands unchanged; only its justification moved.
		- One restriction fell away: a line break in a name is writable now, since the name escaper spells it `\n` and reads it back. One in a `[value]` selector is still refused, since the value emitter has no such spelling, so nothing downstream could rescue it. The write-reason fixture in all four runners pins both halves.
		- The schema's two-level path quoting needed no code change: the outer level is an ordinary string read and the inner is the path scanner, which now resolves. Corpus case 054 pins it along with the merge of two spellings, a tab, a backslash and an apostrophe in a name, and the H001 that now fires because the two spellings are one repeated leaf.
		- Done: the version identity went in at the 2.0.0 cut. 2.0.0 across the eight bump files, the Go module path moved to `source/go/v2` (go.mod, the CLI import, and every README reference), the per-binding dependency constraints, and the changelog entry for everything since 1.2.0.
		- Opened: 20260818-124653
		- Closed: 20260818-172643

- Code review 20260804:

	- ✅ Item 1: the C CLI keeps its `--set` overrides in two parallel arrays, where the other three bindings keep one structured list.
		- The other bindings split `PATH=VALUE` when the option is parsed and store the path, the value and which spelling produced it together. C stores the raw string and re-splits it at apply time, with a second array carrying the spellings.
		- Keeping two arrays aligned needs a trick at the push site: one of the two counts is incremented into a local and thrown away, so the arrays grow in step. That is easy to break and easy to misread.
		- It also leaves the applier doing pointer arithmetic on a separator it assumes is there. Correct today, because the parser rejects a value without one, but the guarantee sits far from the code that relies on it.
		- Fix: give C a small struct (path, path length, value, which spelling) and one vector of it. The re-split, the parallel array, and the lockstep trick all go away together, and the four bindings end up with the same structure.
		- No behavior change, so nothing in the corpus or the crosscheck moves; it is a readability and parity fix, not a bug.
		- Fixed: one `SetOpt` vector holding path, path length, value and spelling. The re-split at apply time, the second array and the lockstep trick are all gone, and the four bindings now keep the same structure.
		- Taken out of order, ahead of item 21 of the 20260830b round: that item adds three more spellings to the same list, and doing it over two parallel arrays would have made the lockstep trick worse rather than removing it.
		- Nothing new proves this one, because a refactor with no behavior change has nothing to fail: what it rests on is byte-identical output from all four bindings on the same edits, plus the corpus, `cli-regress` and the crosscheck staying green.
		- Opened: 20260804-101457
		- Closed: 20260831-090000

- Code review 20260802:

	- Items 26 to 33 are here; 1 to 25 are under Done - Bugs.

	- ✅ Item 26: the parser copies each line more than it needs to.
		- Cause: every line is copied into a fresh string, the indent is copied again, and the path scanner copies the whole line into a character list per call.
		- The profile agrees: those three account for roughly a quarter to a third of parsing time, and they are the current top of the profile.
		- Fixed where the waste existed: the reference no longer copies each line or its indent, and three of the four now scan paths by byte rather than building a character list per call. Formatting a large file got about a tenth faster in the reference and a little faster in Go and C. Python had none of the three.
		- Verified: output is unchanged, checked against a long property run, the whole corpus, and thousands of generated documents per build.
		- Opened: 20260803-111610
		- Closed: 20260803-142116

	- ✅ Item 27: Python scans character by character where a built-in would do.
		- Cause: the comment splitter and the comma splitter both loop per character on every line and value.
		- Fixed: three of these now check for the character before scanning for it, which is a whole-string check the runtime does in C. Formatting a large file got about a tenth faster.
		- The remaining leaders there need restructuring rather than a guard, so they were left alone.
		- Opened: 20260803-111610
		- Closed: 20260803-142116

	- ✅ Item 28: most exported Go functions have no doc comment.
		- The style guide requires one starting with the name; about sixty were missing, including the whole writer API. Nothing checks this automatically.
		- Fixed: 74 of them, one line each.
		- Opened: 20260803-111610
		- Closed: 20260803-134341

	- ✅ Item 29: a doc comment sits on the wrong function.
		- Cause: the description of the element parser ended up attached to the helper inserted above it, in both the reference and Go.
		- Fixed: moved onto the function it describes, matching the other two.
		- Opened: 20260803-111610
		- Closed: 20260803-134341

	- ✅ Item 30: repeat-hint suppression reads the field name out of the hint text.
		- Cause: it splits the hint's wording on quotes to recover the name, so a behavior of the public interface depends on how a message is phrased.
		- Fixed: the wording is built in one place and both the hint and the filter use it, so a reword moves them together. A carried field was the stricter fix but would have broken every caller that builds a diagnostic.
		- Opened: 20260803-111610
		- Closed: 20260803-134341

	- ✅ Item 31: index conversions would truncate on a 32-bit build.
		- A selector index above four billion would wrap and select the wrong element instead of finding nothing.
		- Not reachable on any current target. Noted so the remaining ports don't inherit it.
		- Fixed in the reference with a checked conversion. The other three were audited and already compare before indexing, so they were already safe.
		- Opened: 20260803-111610
		- Closed: 20260803-134341

	- ✅ Item 32: the grammar file disagrees with the parser in a few places.
		- The parser accepts a leading plus on an index, and allows dots and other characters inside a selector; the grammar says neither.
		- The bare-name character ranges include two characters the neighboring comment says are excluded.
		- Unknown escape pairs are kept as written rather than rejected, and a backslash shields the next character in more places than the grammar shows.
		- Needs a call on each: tighten the parser, or widen the grammar to match it.
		- Decided: the parser is the contract, since it is released, so the grammar and spec were widened to describe what it accepts. Quoting is now stated as a rule about what the formatter writes, not about what input is legal.
		- Opened: 20260803-111610
		- Closed: 20260803-134341

	- ✅ Item 33: several public documents claimed things the code doesn't do.
		- Fixed: the contributor notes named the wrong toolchain requirements. The design notes listed platforms that aren't built and a wrapper that doesn't exist. The schema vocabulary list was missing its two newest keys, and both files still described the project as heading toward its first release.
		- Fixed: the readme's install note, one interface name in an example, and the missing PowerShell option syntax.
		- Fixed: the changelog was missing one new public function, and overstated that the writer refuses name wildcards, since removal across them works.
		- Fixed: the style guide described a comment divider spelling Python doesn't use.
		- Opened: n/a
		- Closed: 20260803-111610

- Code review 20260727:

	- ✅ Item 1: the Windows user install goes somewhere Windows does not expect.
		- Cause: `install.ps1` puts a `user` target in `%USERPROFILE%\bin\Shcl`. The convention for a per-user program is `%LOCALAPPDATA%\Programs\`, which is where winget and most installers put one.
		- Low stakes while the only release is a pre-release, and cheap to change now. Later it means a migration step for anyone who already installed.
		- The system target (`C:\Program Files\Shcl`) is already conventional.
		- Done before the 1.0.0 cut, which was the last moment it stayed free: `user` now goes to `%LOCALAPPDATA%\Programs\Shcl` and that dir goes on the user PATH directly. The old layout needed a second copy of the exe in a parent `bin` dir for PATH to find it; that whole branch is gone.
		- Opened: 20260727-134444
		- Closed: 20260728-131605

	- ✅ Item 2: the reference clones a node name several times per index remap.
		- `remap_child` runs on every in-place value mutation (an empty field being filled, a stacked-list element being appended) and cloned the name up to five times, because it looked a key up and then removed it separately.
		- Building each key once and reusing it would about halve that, with no change in behavior. The other three bindings have their own structure and would need their own look.
		- Not evidence-backed: the profiler put the leaders elsewhere (path scanning, code-point iteration, comma splitting), so this was a code-reading finding. Do it only if a profile ever points here.
		- Done by the memory pass under Done - Features and enhancements: the accelerator maps key on hashes and verify against the tree, so `remap_child` builds no key strings and clones nothing.
		- Opened: 20260727-134444
		- Closed: 20260821-150025

- Code review 20260725:

	- Items 24 to 41 are here; 1 to 23 are under Done - Bugs.

	- ✅ Item 28: size, node count and array length limits.
		- The depth cap closed the crash class. The rest is additive API that can be added later without breaking anything, and a consuming program can bound input size itself before calling parse.
		- The size half came first: `read_file(path, max_bytes)` caps the read in all four bindings.
		- Measured: a 4.3 MB document of one-line fields holds 252 MB resident, about 59x, so the byte cap alone cannot bound a load. That is what reopened the deferred half.
		- Decided: the caps are the caller's, not constants - `parse_limited(text, strictness, max_nodes, max_elements)` in every binding and the C++ veneer, 0 = uncapped. At the node cap the parse stops with one `E020` and the unparsed remainder counts as lost; an over-long array is refused whole (`E021`), never truncated. The depth cap keeps its skip-and-continue recovery; reasons in design.md.
		- Fixed: all four bindings plus the veneer, byte-identical diagnostics.
		- Pinned by: a parse_limited fixture in all four runners plus a veneer smoke line - cap crossing and its line number, lost accounting, the last-line cross, the one-line overshoot, both array spellings, strict failure carrying the document.
		- Opened: 20260725-152141
		- Closed: 20260901-112357

	- ✅ Item 29: thread the diagnostic code through every call site.
		- All four bindings recover the code from about 30 hand-ordered message prefixes, so rewording a message can change a code, and the ordering matters.
		- Large, mechanical and invisible to users. Every corpus case pins the code per line, so the exposure is limited to messages no case exercises.
		- Fixed: every diagnostic site names its code directly and the prefix maps are gone, in all four bindings. Message wording is now free for real, which is what the code field's doc comment already claimed.
		- Pinned by: corpus 071 and 072, new, covering the schema-fault and typed-allowed arms nothing exercised before. With them the suite reaches every diagnostic site except the two below.
		- Note: two sites are unreachable from any input and stay as defensive code - a duplicate fragment name (same-name nodes merge at parse, so the check can never see one) and the second empty-list-element check (its input was already trimmed and tested non-empty). Same class as E003.
		- Opened: 20260725-152141
		- Closed: 20260901-105524

	- ✅ Item 24: `merge()` is O(children^2) per parent in all four bindings.
		- Cause: the over-side name dedup, the per-name group filter and the base-side instance match are all linear scans, and each rebuilds merge keys as it goes.
		- Reproduced: parsing tens of thousands of keys takes milliseconds; merging them takes many seconds in the reference. The headline feature is the slowest thing in the product.
		- The same accelerator the prior review's item 12 added to the parser applies here.
		- Fixed in all four bindings.
			- One grouping pass per side, with every merge key computed once, and a single children rebuild per parent.
			- The same merge now takes milliseconds in the reference and in C. Output is byte-identical, and instance order is unchanged.
			- The stacked-list key rebuild in the reference, Go and Python went the same way, reusing the deferred flush C already had.
		- Opened: 20260725-152141
		- Closed: 20260725-184454

	- ✅ Item 25: a `[value]` selector is looked up by linear scan, so the inline spelling is quadratic.
		- Reproduced: tens of thousands of `srv[hostN].port: N` lines take seconds against milliseconds for the equivalent block form.
		- Both spellings are spec-equal, so the user hits a cliff of two orders of magnitude for a cosmetic choice.
		- The read and write paths scan siblings the same way, since the parser's child index is deliberately dropped at load. That part is fine at hand-authored sizes. If it ever needs touching, name interning is the low-risk option: a cached side index has to be invalidated at five mutation sites in four bindings, and a missed invalidation is a silent wrong value rather than a slow one.
		- Fixed at parse in all four: a display-keyed sibling map beside the merge-key map (same first-wins discipline, same mutation sites, flushed with the stacked-list deferral before any lookup). The inline-selector case now parses as fast as the block form. The read/write-path scans stay linear on purpose, as the item itself recommended.
		- Opened: 20260725-152141
		- Closed: 20260725-184454

	- ✅ Item 26: the validator's "did you mean" rebuilds the whole schema index once per unknown field.
		- Bites when a document is wholesale unmatched (the wrong file, or a schema for another app), which is the case the feature exists for.
		- C compounds it with a linear scan of the legal-chain set where the other three use a hash set.
		- Output is stderr prose, so the fix needs no corpus change.
		- Fixed in all four: legal chains and per-parent-chain sibling-name lists are built once per validate and handed to the suggester; C also swapped its linear legal-chain scan for the hash set the other three use.
		- Opened: 20260725-152141
		- Closed: 20260725-184454

	- ✅ Item 27: `to_canonical` is O(raw-siblings^2).
		- Cause: each raw node rescans the parent's children to decide whether it merges with the line above; the parent's own walk already has that information.
		- Reproduced: tens of thousands of raw blocks under one field take seconds to format against a near-instant parse. Narrow, but free to fix and behavior-preserving.
		- Fixed in all four: the parent's walk carries a seen-empties set and passes each child its would-merge flag, so no node rescans its siblings. Formatting now matches the parse time.
		- Opened: 20260725-152141
		- Closed: 20260725-184454

	- ✅ Item 28: give the loader opt-out limits - the depth half.
		- Nothing bounded input size, nesting depth, node count or array length in any binding, and a parse costs many times the input in memory.
		- A consuming program handed a config path from a user, a shared directory or a container volume had no way to refuse something unreasonable.
		- Done: a fixed 512-level depth cap and `E016`, alongside item 2. That closes the crash class.
		- The remaining knobs are deferred - see Future and/or deferred.
		- Opened: 20260725-152141
		- Closed: 20260725-163911

	- ✅ Item 29: publish the diagnostic codes - the doc half.
		- `V001`-`V099` were fully tabled, but `E001`-`E015` and `H001` were listed nowhere, while users are told to gate CI on `check`.
		- Done: `E001`-`E016` plus `H001` are now tabled in the spec's Diagnostics section.
		- Threading the code through every call site is deferred - see Future and/or deferred.
		- Opened: 20260725-152141
		- Closed: 20260725-175440

	- ✅ Item 30: the canonical formatter discards blank-line grouping.
		- Cause: comments were rescued as trivia by the prior review's item 4; blank lines are the other half of the same thing and were left out, so `fmt` flattens a grouped config into a wall.
		- Field names are also folded to lowercase and the spec never says so, which makes `fmt --write` a surprise.
		- The trivia model already exists, so this is a per-node flag and one emit line per binding.
		- Fixed: a `blank_before` trivia flag in all four parsers preserves one blank line before a binding (runs collapse; a blank before a comment group rides with it; fixpoint holds). Four goldens regenerated, case 032 pins it. The name folding was decided as correct-and-documented: the spec's formatter section now states lowercase is the canonical spelling.
		- Opened: 20260725-152141
		- Closed: 20260725-182548

	- ✅ Item 31: `paths()` exists only in the reference.
		- Go, Python and C consumers handed an unknown document cannot enumerate it; `Count` and `Instances` both require knowing the path already.
		- Straight violation of the guide's "same function inventory" rule on a public method, and about 20 lines per port.
		- Fixed: `Paths()`/`paths()`/`shcl_paths` (and the veneer's `paths()`) now exist in every binding, mirroring the reference's walk (file order, deduplicated, bare-name-safe segments only); a shared fixture is pinned in all four native runners.
		- Opened: 20260725-152141
		- Closed: 20260725-180643

	- ✅ Item 32: `--set` is described as the top layer but behaves as a first-instance edit.
		- Cause: a real top layer replaces every same-named leaf; `--set` targets the first instance and leaves the rest, so the two disagree on repeated leaves.
		- The spec already names the Writer as the mechanism, so this is a missing clause rather than wrong behavior.
		- Fixed with the missing spec clause: `--set` is a Writer edit targeting the first matching instance; whole-group override belongs in a `--layer` file.
		- Opened: 20260725-152141
		- Closed: 20260725-180134

	- ✅ Item 33: the convenience read tier is incomplete in C (3 of 11 types) and C++ (4 of 11).
		- No convenience read for string, the most common config read, or for raw, datetime, or any array.
		- The omission has a real C rationale (those types return borrowed memory), but the spec claims full coverage and the style guide's deviation list does not mention it.
		- Resolved as documentation (the C rationale is real - borrowed memory and lengths do not fit a value-or-default signature): the spec's ergonomic-tier section and the style guide's C deviation list now both state the exact coverage.
		- Opened: 20260725-152141
		- Closed: 20260725-180134

	- ✅ Item 34: Python's public `get_*` raises a private-named `_StatusError`.
		- A caller cannot catch it without reaching into a private name, so in practice they will write a bare `except Exception`.
		- Fixed: the class is public `StatusError` now (docstring included), so callers can catch it by name.
		- Opened: 20260725-152141
		- Closed: 20260725-180134

	- ✅ Item 35: the profiler stage samples only `fmt`, on a workload where everything is still linear.
		- The read path, `merge`, `validate`, `generate` and the Writer are never sampled, so all three 2026-07-23/24 features could go quadratic without moving a sample.
		- The cheapest half of the fix is a wall-clock number per workload in the run log: a flamegraph shows where time goes, not that total time grew several-fold.
		- Fixed with the cheap half the review named first: the profiler stage now logs a wall-clock line per workload (fmt, merge, reads, validate, generate, set) from a `PROFILE_TIMED` list in config. Any of them going quadratic shows as a number moving even though the flamegraph still samples fmt.
		- Opened: 20260725-152141
		- Closed: 20260725-181714

	- ✅ Item 36: CI installs its lint toolchain unpinned every run.
		- `TOOL_PINS` already tracks the versions the local gate uses, so CI and local disagree about what "passing" was tested against.
		- Actions are also referenced by floating tag rather than commit SHA - generic hardening, low risk here.
		- Fixed: ci.yml installs ruff/mypy/cppcheck/markdownlint at the exact `TOOL_PINS` versions, and every action is referenced by commit SHA with the tag in a comment.
		- Opened: 20260725-152141
		- Closed: 20260725-181714

	- ✅ Item 37: harden the installers' transport and integrity story.
		- `curl` and `wget` follow redirects with no protocol pin or TLS floor.
		- The sums file arrives over the same channel as the binary, so it catches corruption but not substitution. The source tarball, which supplies the drop-in files consumers compile in, is not verified at all.
		- ✅ Done: the transport half. curl and wget pin https through redirects with a TLS 1.2 floor (`install.bash`, and `install-dev.bash`'s rustup fetch); `install.ps1` floors TLS 1.2/1.3 for every download.
		- ✅ Done: the signature half, ahead of 1.0.0. The sums file is signed offline with an RSA-4096 key; both installers carry the public half inlined and verify it before reading any checksum out of the file. `openssl` joined curl/wget as a hard prerequisite - there is no install-anyway fallback. `cicd/utility/sign-release.bash` does the signing and refuses if the key does not match the one the released installers trust.
		- Key custody is offline and signing is manual, deliberately: a key in CI would be reachable by the same compromise the signature defends against. RSA rather than Ed25519 purely so the verifier is one both `openssl` and Windows PowerShell 5.1 already have. Rationale and threat model in `design.md`.
		- Still not covered: the source tarball that supplies the drop-in files. It is fetched by tag, so it is as trustworthy as the tag, but it carries no signature of its own.
			- Closed by Code review 20260817 item 28: the drop-ins now travel in a signed release asset, and the source tarball is out of the install path.
		- Opened: 20260725-152141
		- Closed: 20260728-131605

	- ✅ Item 38: the style guide bans the section rules the code actually uses.
		- "No banner dividers" against dozens of them in every binding, and the guide is what a Tier 3 author is told to read first.
		- The code is right: in a 3400-line drop-in file the section rules are the only navigation aid. Amend the rule and pin one spelling per language.
		- One real inconsistency alongside it: `shcl.h` uses the shell house `//•••` rule, which is both off-style for C and the only non-ASCII comment character in the C bindings.
		- Fixed: the guide now sanctions section rules as the one allowed banner and pins each binding's exact spelling; the lone `//•••` shell-style rule in `shcl.h` became the C `// ===` divider.
		- Opened: 20260725-152141
		- Closed: 20260725-180134

	- ✅ Item 39: panic macros are used outside tests in the reference.
		- Eight sites - six `unreachable!` (four of them in the newest validator and generator code) and two `unwrap()`.
		- Each is provably unreachable today, but they are invariants asserted by a panic in a library whose contract is that it never bails on a whole file, and three ports copy the structure.
		- Fixed: every non-test `unreachable!`/`unwrap()` now degrades instead of aborting - a slipped invariant skips the constraint, returns no-parent, or keeps the match total with an empty string. Zero panic macros left outside tests (the feature-gated profiling `expect`s are never released).
		- Opened: 20260725-152141
		- Closed: 20260725-181125

	- ✅ Item 40: the CLI usage block is hand-duplicated across four CLIs with no drift check, and its exit-code line is wrong.
		- Cause: it still says exit 6 means a strict load failure; the prior review's item 36 made `check` exit 6 on any error diagnostic, at any strictness.
		- `help`, `version`, a bare invocation and an unknown subcommand are the largest user-visible output in the project and the crosscheck never runs them.
		- Fixed: the exit-code sentence now says `6 check failed or strict load failure` in all four, and the crosscheck pins the whole usage output: `help`, `version`, a bare invocation, and an unknown subcommand are compared byte-for-byte across bindings on every run.
		- Opened: 20260725-152141
		- Closed: 20260725-180134

	- ✅ Item 41: changelog has no Unreleased section, and contributing.md never explains the corpus workflow.
		- Five finished feature sets since the beta2 tag are recorded only in git; populating it now is also the raw material for the 1.0 notes.
		- contributing.md does state the parity rule, but nothing points a first-time contributor at how to add a conformance case, so their PR will be structurally wrong.
		- Fixed: changelog.md gained an Unreleased section covering everything since beta2 (the 1.0 notes' raw material), and contributing.md gained an "Adding a conformance case" walkthrough.
		- Opened: 20260725-152141
		- Closed: 20260725-180134

- Code review 20260716:

	- Items 4 to 6, 18 to 20, 22, 28, 29, 31 and 35 to 38 are here; the rest of the round is under Done - Bugs.

	- ✅ Item 4: `fmt` deletes every comment with no warning, and the spec never discloses it.
		- Direct hit on the hand-author audience; retrofitting comment storage later touches all five codebases.
		- Decide before 1.0: preserve comments as trivia, or spec the loss and warn on `fmt --write`.
		- Done: comments survive `fmt` in all four parsers - whole-line comments re-emit above the node the next line binds, trailing ones stay on their line, end-of-file comments stay at the end. Spec'd under Comments + Canonical formatter; corpus case 013 pins it and the older cases' expected files now keep their comments.
		- Opened: 20260718-165550
		- Closed: 20260719-192715

	- ✅ Item 5: the Writer half of the spec'd API exists in no binding and has no backlog item.
		- Spec presents Accessor+Writer as the two halves; schema-driven generation depends on it.
		- Done: Writer implemented in all four bindings (full CRUD - typed `set_<T>`/arrays/`raw`/`empty`, `_default` only-if-absent forms, `exists`, `set_comment`, `remove`), each setter the exact inverse of its read. New `shcl set` CLI applies a tab-delimited write-ops script from stdin. Corpus cases 014-016 pair `write.ops` with golden `expected-write.shcl` (matched by every binding's runner + a fixpoint check), the cross-binding differential replays `set`, and a reference fuzz run pins the string round-trip. Spec Writer bullet updated.
		- Opened: 20260718-165550
		- Closed: 20260721-091902

	- ✅ Item 6: README lead code examples call APIs that do not exist.
		- Cause: `GetIntOr(...)` (Go), `get_int(..., default=)` (Python), `get_or<T>` (C++) are all missing; a new user's first copy-paste fails.
		- Done: implemented the spec's convenience tier (not gutted the examples), so the README calls are now real as written. Go `GetIntOr`/`Get*Or` + array forms; Python `get_*`/`get_*_array` gained a `default=` param (must-exist and raises without one); C `shcl_get_int/float/bool`; C++ `get_or<T>`. Rust's is the native `get_int(path).unwrap_or(def)` and gained matching `get_*_array` so arrays have the same tier. Semantic pinned everywhere: value only on `Good`, else the call-site fallback (Empty falls back too). Reference unit test + Go test + C++ veneer CHECKs added.
		- Opened: 20260718-165550
		- Closed: 20260721-123412

	- ✅ Item 18: query-side behavior is barely pinned.
		- No corpus rows for wildcards, on-bad modes, or defaults; the fuzz differential compares `fmt` only.
		- The accessor side is where five hand-written ports diverge most easily.
		- Done: added a `--rawinfo` CLI type (+ the `rawinfo` reads.tsv type in all four runners) so the info-string read is pinnable. The reference `Document::paths()` drives a fuzz-dump-derived `<name>.reads.tsv` that the crosscheck `--extra` replays (reads over fuzz soup, not just fmt). Every scalar read row is also replayed under `--on-bad=error` and `--default=<x>`. Corpus case 020-accessor-surface pins wildcards (with a missing slot), a `[value]` selector, and both raw reads. Crosscheck comparisons roughly doubled.
		- Opened: 20260718-165550
		- Closed: 20260721-131824

	- ✅ Item 19: diagnostic wording became a byte-for-byte 5-way contract by accident.
		- Cause: `check` prints prose to stdout and crosscheck compares it, so every English message is frozen across bindings - contradicting design.md's per-binding-voice rule.
		- Give diagnostics stable codes; compare codes, free the prose.
		- Done: `Diagnostic` carries a stable `code` (`E001..`/`H001..`) in all four bindings; `check` prints `line N: severity: CODE` to stdout and the prose to stderr, so the differential check compares codes (not wording). C exposes `shcl_diag_code`. Includes item 36 (below).
		- Opened: 20260718-165550
		- Closed: 20260721-130525

	- ✅ Item 20: README still says no tagged release exists.
		- Contradicts the v1.0.0-beta1 tag, the changelog, and the published prerelease binaries; badge still says Alpha.
		- Done: lifecycle badge Alpha -> Beta, Status/Installing sections now reflect the `v1.0.0-beta1` pre-release and its prebuilt binaries; release-cut checklist in `design.md` gained a "README status pass" step so it can't drift again.
		- Opened: 20260718-165550
		- Closed: 20260721-122219

	- ✅ Item 22: `--on-bad=error` messages are bare enum names.
		- Reproduced: `app.name: BadType` - no value, no requested type, no file, no suggested fix. Stderr is not contract, so this is free to improve.
		- Done in all four CLIs: `shcl: cannot read <path> as <type>: <reason> (in <file>)`, where a BadType names the offending raw value (`value "$1200" is not a valid int`), and NotFound/Empty/Multiple get a plain-English reason. Array reads say `<type> array`. Stderr, so not crosscheck-pinned.
		- Opened: 20260718-165550
		- Closed: 20260721-125256

	- ✅ Item 28: dogfood install is a non-atomic in-place `cp`.
		- A launch during the copy sees a torn binary. Copy to a temp name, then `mv` over.
		- Done: the dogfood stage now copies the binary and each wrapper to a hidden temp name in the same dest dir, then `mv -f` (atomic rename) over the target. A hand-launched copy only ever sees the complete old or new file, and a failed copy is cleaned up and warned, not left torn.
		- Opened: 20260718-165550
		- Closed: 20260721-125414

	- ✅ Item 29: selector index parses as `usize` in the reference but `u64` in Go.
		- Latent divergence on any future 32-bit build. Pin the reference to `u64`.
		- Done: reference `Selector::ByIndex` is now `u64` (cast to `usize` only at the Vec index sites); C's `Selector.index` likewise moved from `size_t` to `uint64_t` (`parse_usize` -> `parse_u64`), closing the same latent 32-bit truncation. Go was already `u64`, Python is unbounded. No 64-bit behavior change.
		- Opened: 20260718-165550
		- Closed: 20260721-124351

	- ✅ Item 31: spec prose contradicts grammar and code on bare non-ASCII field names.
		- Prose says only reserved chars need quotes (and uses `Strasse` with a sharp s as an example); grammar and parser reject them.
		- Also: hex `-0x8000000000000000` (i64 min) reads BadType while the decimal spelling works.
		- Done: aligned prose to the code (grammar, `is_bare_name_char`, and the emit predicate already agreed) - a bare field name is ASCII letters/digits/`-`/`_` only; anything else, including non-ASCII, must be quoted, and the `Straße` examples are now shown quoted. Hex fixed in all four parsers: parse the magnitude as u64, then range-check against the sign, so `-0x8000000000000000` reads i64-min like its decimal spelling and `+0x8000000000000000` stays BadType. Corpus case 019-hex-int-bounds pins it.
		- Opened: 20260718-165550
		- Closed: 20260721-124351

	- ✅ Item 35: value-taking options reject the space-separated form with a misleading error.
		- Reproduced: `--default 99` reports "unknown option: --default". Accept the space form or explain the `=` requirement.
		- Done: all four CLIs now accept both `--default=VALUE` and `--default VALUE` (same for `--on-bad`/`--strictness`), via an index loop that consumes the next arg; a value option with nothing after it says `missing value for --default (try --default=VALUE)`. Help text notes both spellings.
		- Opened: 20260718-165550
		- Closed: 20260721-125256

	- ✅ Item 36: `check` reports "ok" and exits 0 even when diagnostics include Errors.
		- A CI gate on `check` passes configs whose lines were dropped. Nonzero exit or clearer wording; note it is corpus-pinned, so change everywhere at once.
		- Done (folded into item 19): `check` exits 6 whenever any error diagnostic is present, at any strictness - `failed: N diagnostic(s), M error(s)` for a standard/loose load that dropped lines, `strict load failed: N diagnostic(s)` for a strict failure, `ok (N diagnostic(s))` + exit 0 only when clean. Same strings and exit in all four bindings.
		- Opened: 20260718-165550
		- Closed: 20260721-130525

	- ✅ Item 37: `--version`/`-h` are undiscoverable.
		- Help text never mentions them; `shcl help` and `shcl version` are rejected; `-w` is accepted but undocumented.
		- Done in all four CLIs: the usage block gained a `shcl help | version` line noting `-h/--help` and `-V/--version`; `shcl help` and `shcl version` are now accepted subcommands; the `fmt` line shows `[--write|-w]`.
		- Opened: 20260718-165550
		- Closed: 20260721-125256

	- ✅ Item 38: wrapper documentation drift.
		- README omits the PowerShell wrapper; spec says "POSIX sh" but the shell wrapper is deliberately Bash. Align the words with the artifacts.
		- Done: README blurb + Status now name both the Bash and PowerShell wrappers; spec's two concrete "released wrapper" claims say Bash instead of POSIX sh (the illustrative two-tier table row stays, like the other not-yet-released language rows).
		- Opened: 20260718-165550
		- Closed: 20260721-122219

### Future and/or deferred

- ✋ Ports: Tier 3.
	- Each a drop-in where possible, and corpus-green before release.
	- Type via a typed entry point or compile-time generic, never a runtime type field.
	- Languages, in the order they are wanted:
		- ✋ JavaScript (node)
		- ✋ C#
		- ✋ Java, and Kotlin with it
	- Opened: 20260728-114451

- ✋ BSD package.
	- Listed among the packaging targets and never built. README.md is accurate.
	- Opened: 20260819-111243

### Canceled

- 🚫 Default configuration hard-coded.
	- 🚫 Overridden by a per-user config file, created the first time a default is changed.
		- 🚫 Settings live under `~/.config`, resistant to errors (do not bail on the whole file over one bad line).
	- 🚫 Overridden by program options at run time.
	- Dropped: strictness and on-bad are the consuming program's contract, not the user's. A user-level override would silently weaken guarantees an app makes about its own config handling, and would make the same `shcl` command mean different things on different machines. Nothing else the CLI exposes is presentation-only, so there is nothing left for a config file to hold. Rationale in `design.md`; runtime options and the library's per-document strictness argument stay as they are.
	- Opened: 20260711-150807
	- Closed: 20260723-134323

- Code review 20260819:

	- Item 17 is here; the rest of the round is under Done - Features and enhancements and Future and/or deferred.

	- 🚫 Item 17: the AI acceptability guidelines are unreachable from the README.
		- A substantial public document that the Docs list does not mention, so the only way to find it is to browse the file listing.
		- Not a defect. The file is meant to be there for anyone who goes looking, without the README pointing at it - the front page is about what the project does, and that document is not part of the pitch.
		- It was briefly added to the Docs list and has been taken back out. Leave it unlinked.
		- Opened: 20260819-111243
		- Closed: 20260819-132623

- Code review 20260817:

	- Item 25 is here; the rest of the round is under Done.

	- 🚫 Item 25: no way to read a whole config into a structure. Declined, recorded as a decision.
		- Every binding is path-at-a-time. A forty-key config is forty call sites and forty literal defaults.
		- Users arrive from libraries that decode a whole document into a typed structure in one call, and that is the comparison a reviewer makes.
		- It does not conflict with "values are typed by the reader": a field's declared type is the reader's requested type, applied in bulk.
		- The real cost is parity: it is per-binding machinery with no reference structure to mirror. Decide it either way, but record the decision in design.md so it reads as a choice rather than an omission.
		- Declined because the reference cannot implement it: a derive-based decoder needs a proc-macro, which is a second crate, and one file per binding with no dependencies is what the product is. Hand-written reflection instead gives each binding its own machinery with nothing to mirror, the parity rule inverted.
		- Doing it in only the two languages where it is cheap would be worse than not doing it: the same config would load two different ways depending on the language, which is the one thing the crosscheck exists to prevent.
		- Recorded in `design.md` -> Consumer API, so it reads as a choice rather than an omission. Reversible; the reasoning is what would have to change, not the code.
		- Opened: 20260817-204524
		- Closed: 20260818-155051
