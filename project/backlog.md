<!-- markdownlint-disable MD007 -- Unordered list indentation -->
<!-- markdownlint-disable MD010 -- No hard tabs -->
<!-- markdownlint-disable MD033 -- No inline html -->
<!-- markdownlint-disable MD055 -- Table pipe style [Expected: leading_and_trailing; Actual: leading_only; Missing trailing pipe] -->
<!-- markdownlint-disable MD041 -- First line in a file should be a top-level heading -->
# Requirements

This is a product backlog just for pre-v1.0.0 release. After that, bugs, features, and enhancements will be managed in Github Issues.

<!-- TOC ignore:true -->
## Table of contents
<!-- TOC -->

- [Conventions](#conventions)
- [Initial requirements](#initial-requirements)
	- [Platform and foundations](#platform-and-foundations)
	- [Build, CI/CD, and install](#build-cicd-and-install)
	- [Configuration and persistence](#configuration-and-persistence)
- [Backlog](#backlog)
	- [Misc to-do](#misc-to-do)
	- [Bugs](#bugs)
	- [Features and enhancements](#features-and-enhancements)
	- [Done](#done)
		- [Done - Initial requirements](#done---initial-requirements)
		- [Done - Bugs](#done---bugs)
		- [Done - Features and enhancements](#done---features-and-enhancements)
	- [Future and/or deferred](#future-andor-deferred)
	- [Canceled](#canceled)

<!-- /TOC -->

## Conventions

In each section, items are listed approximately from newest to oldest.

| Icon | Status
| :--: | :--
| 🔘   | Not started
| 🛠️   | Started, and/or partially complete
| ✋   | Defer
| ✅   | Complete
| 🚫   | Canceled

## Initial requirements

### Platform and foundations

- ✅ Language spec rationalized from `notes.txt`: terminology, model, types, accessor and writer API, formatter, raw blocks. See `spec.md` and `grammar.abnf`.
	- ✅ stacked (`*`) block-array form alongside inline commas. Both spellings read the same and canonicalize to inline.
	- ✅ date and time formats pinned to a closed whitelist (year-first or named-month only, then calendar-validated).
	- ✅ adoption sweep. Cut the currency, percent, float-to-int rounding, and extra boolean tokens from default behavior. Case folding restricted to ASCII. Repeated-leaf hint made mandatory.
	- ✅ three strictness levels (loose, standard, strict) as a per-document knob, default standard. Loose re-admits the cut coercions; strict fails the load on any error.
	- ✅ bindings re-tiered. Tier 1 reference plus CLI; Tier 2 Go, C, Python; Tier 3 the rest, post-v1.0.
	- ✅ raw-block binding reworked. A fence is a value line for its parent field. Child-indent spelling is canonical.
	- ✅ inline-array commas made fully forgiving. Stray commas drop their empty slots and never error.
	- ✅ Model diagnostic expectations (count, severity, the mandatory repeated-leaf hint) in the corpus. See conformance item below.

- 🛠️ Ports to the remaining binding languages, in tiers. Tier 2 done; Tier 3 (C#, Java, JavaScript) after v1.0. Each drop-in where possible, corpus-green before shipping. Type via a typed entry point or compile-time generic, never a runtime type field.
	- ✅ Go, C (with a C++ veneer), and Python, each an independent parser with the same flags, output, and exit codes as the reference. All corpus-green and held byte-for-byte to the reference on every build.
	- Note: remaining companion surfaces are veneers, not new parsers. Kotlin over the Java core, TypeScript over the JavaScript core.
	- ✅ Shell wrappers around the CLI, not parsers.
		- ✅ Bash wrapper (`source/bash/shcl.bash`). Runs as a script or sourced for typed helpers. Forwards the binary's exit code unchanged.
		- ✅ PowerShell wrapper (`source/powershell/shcl.ps1`). Runs as a script or dot-sourced for the same typed helpers; forwards the binary's exit code in `$LASTEXITCODE`. Cross-platform binary resolution (also matches `.exe`).

- ✅ Expand the conformance corpus (`conformance/`) to cover the hard edges: dates and ambiguity, coercion, quoting and escapes, indentation errors, raw blocks, selectors and wildcards, strictness levels.
	- Done: cases for stacked arrays, coercion bundles, strict-load behavior, forgiving commas, raw blocks, and the full date whitelist.
	- ✅ Model diagnostic expectations (count, severity, the mandatory repeated-leaf hint). Every case now carries an `expected-diags.txt` golden (exact `check` stdout at standard: line/severity/code per diagnostic + the summary line), verified natively by all four runners. Pins the `H001` repeated-leaf hint and the zero-diagnostic cases too.

### Build, CI/CD, and install

- 🛠️ A CI/CD pipeline driven by `cicd/cicd.bash`: builds, tests, and can commit and push. Packaging and publishing are opt-in. See `design.md` for the split-by-responsibility rationale.
	- ✅ all stages live. Format check, build, lint, tests plus fuzz smoke, profiler, native and cross builds, versioned artifacts, README demo gif, publish.
	- ✅ `--ci` mode is the correctness gate the GitHub workflow runs, so local and CI share one definition of passing.
	- ✅ cross-binding differential check. Every binding CLI must agree with the reference byte-for-byte on the corpus and a fuzz-dumped input set.
	- ✅ dogfood stage installs the fresh release binary to a fixed local dir. Off under `--ci`, no sudo path.
	- ✅ lint stage widened to every binding. ruff and mypy for Python, cppcheck for C, markdownlint for docs, PSScriptAnalyzer for the ps1 wrapper. All gating, locally and in CI; setup steps in `contributing.md`.
	- ✅ Packaging (.deb, .rpm, NSIS). Wire it when release cuts start.
		- Done: stage 6 builds .deb + .rpm (nfpm) per Linux binary and an NSIS setup per Windows binary into the release artifact dir, covered by the same sha256sums. `--no-package` to skip; off under `--ci` and `--quick`. Packages use distro layout (/usr/bin + /usr/share/shcl); payload matches install.bash.

### Configuration and persistence

- 🚫 Default configuration hard-coded.
	- 🚫 Overridden by a per-user config file, created the first time a default is changed.
		- 🚫 Settings live under `~/.config`, resistant to errors (do not bail on the whole file over one bad line).
	- 🚫 Overridden by program options at run time.
	- Dropped: strictness and on-bad are the consuming program's contract, not the user's - a user-level override would silently weaken guarantees an app makes about its own config handling, and would make the same `shcl` command mean different things on different machines. Nothing else the CLI exposes is presentation-only, so there is nothing left for a config file to hold. Rationale in `design.md`; runtime options and the library's per-document strictness argument stay as they are.

## Backlog

### Misc to-do

### Bugs

- ✅ Code Review 20260725 item 1: a higher layer that names a container with no children deletes the whole subtree below it.
	- `server:` (or `server: web1` with an empty body) in an over layer wipes every child the lower layers put there, silently, exit 0.
	- Worse than it reads: the wipe covers every same-named instance, so mentioning `server: web1` also deletes an untouched `server: web2`.
	- A body that is only a comment counts as empty, because comments are trivia rather than children.
	- Needs a decision, not just a fix: the code matches the spec's own wording, and the obvious narrowing removes the ability to blank a section from a higher layer.
	- Decided and fixed: the leaf-override path now applies only when the base side of the name group is also all-childless, so a bare section header merges (matching instance untouched, unmatched appended as an empty instance) and leaf clearing still works. No way to blank a section from a higher layer; a deletion spelling is deferred post-1.0. All four bindings, spec reworded, corpus case 027.
	- Detail: `design.md` - Code Review 20260725, item 1.

- ✅ Code Review 20260725 item 2: deep documents crash three of the four bindings, each at a different depth.
	- Fixed with a load-time nesting cap: 512 levels, enforced in all four parsers before any node is created (`E016`, line skipped) and mirrored by the Writer's `place`. Every measured cliff sat far above the cap, so the existing recursion is now safe everywhere; the reference keeps its recursive walks on purpose (structural parity beats a one-binding rewrite). Spec gained the cap and the load-code table; corpus case 028 pins the error, a reference test pins the 512 boundary and the writer refusal.
	- Emit and merge recurse per nesting level while the parser is iterative, so a document parses fine and then dies when anything formats or merges it.
	- Python merge is the acute one - a 4.9 KB file is enough. The reference aborts on `fmt` around 33k levels; C segfaults; Go survives by growing to tens of GB.
	- A dotted path buys one level per two bytes, so depth is cheap for an attacker and needs no odd syntax.
	- Same input, four different exit codes: the inverse of the product guarantee.
	- Detail: `design.md` - Code Review 20260725, item 2.

- ✅ Code Review 20260725 item 3: `shcl set` validates its op values four different ways.
	- Rust and Go reject a malformed int; Python accepts unbounded ints, underscores, padding and non-ASCII digits; C silently writes 0 or a truncated/saturated value and exits 0.
	- Python can emit an integer no binding can read back, so the Writer produces out-of-contract output.
	- The ops script on stdin also skips the UTF-8 gate the file reader applies, so bad bytes become U+FFFD instead of exit 1.
	- stdout and exit codes are contract here and the crosscheck replays `set`, so this only escapes CI because no corpus op line carries a malformed value.
	- Detail: `design.md` - Code Review 20260725, item 3.
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
	- Detail: `design.md` - Code Review 20260725, item 5.
	- Fixed: the doc gained a scratch arena reset on entry to every resolve (path scans, resolver vectors, compare strings, int/float/bool coercion temps) and on merge entry; `w_replace_leaf_group` rebuilds the children vector in place; `v_suggest` gets a per-field-reset scratch for its DP rows and chains. Returned bytes still live in the persistent arena per the documented contract (string reads accumulate only their returned copies). Measured: 1M int reads 1.13 GB -> flat 2 MB; 16k-sibling merge 2.53 GB -> 38 MB; 2k-field validation 1.38 GB -> 17 MB.

- ✅ Code Review 20260725 item 6: C grows a stacked `*` list one element at a time, so parsing it is quadratic in memory.
	- A fresh array is allocated and copied per `* ` line, and the arena keeps every discarded copy.
	- An 11 KB file already costs 38 MB; 95 KB costs 2.4 GB; 249 KB costs 14.8 GB.
	- Reachable from a plain `fmt` or `check` on an ordinary-looking config.
	- Detail: `design.md` - Code Review 20260725, item 6.
	- Fixed: the element array grows geometrically, and the per-element merge-key rebuild is deferred while the list is the open field (flushed before any other map lookup, so behavior is unchanged). 20k elements: 14.8 GB and 40 s -> 7.6 MB, instant; output byte-identical to the reference. The other bindings' key-rebuild time cost is item 24's territory.

- ✅ Code Review 20260725 item 7: `fmt --write` truncates the config in place, and C reports success when the write fails.
	- C never checks `fwrite`/`fclose`, so a failed write prints nothing and exits 0 while the other three exit 1 - a live exit-code divergence.
	- No binding uses temp-file-and-rename, so an interrupted write destroys the file the tool exists to protect. The dogfood installer was made atomic for this exact reason.
	- Fixed: all four CLIs write through a temp-file-and-rename in the target's directory (data synced before the rename), and C checks every stdio call, so a failed or interrupted write exits 1 and never leaves a truncated file.

- ✅ Code Review 20260725 item 8: both Windows installers damage `PATH`.
	- NSIS reads `PATH` through a 1024-char string and writes the truncated value back. Observed in a wine run: a 1427-char machine PATH came back as 22 chars. The uninstaller does the same.
	- `install.ps1` reads `PATH` expanded and writes it back as `REG_SZ`, baking `%USERPROFILE%`-style references and downgrading the value type.
	- The two Windows install paths also disagree with each other about how to test for an existing entry.
	- Detail: `design.md` - Code Review 20260725, item 8.
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
	- Detail: `design.md` - Code Review 20260725, item 17.
	- Fixed with the cheap half: the stderr prose spells `schema line N` for `V090`-`V093` (whose numbers are schema-file lines per the code table) in all four CLIs; the compared stdout keeps the uniform shape since the code already names the space. The structural `source`/`column` fields stay future work, noted in design.md.

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

### Features and enhancements

- 🔘 Code Review 20260725 item 24: `merge()` is O(children^2) per parent in all four bindings.
	- The over-side name dedup, the per-name group filter and the base-side instance match are all linear scans, and each rebuilds merge keys as it goes.
	- Parsing 32k keys costs 56 ms; merging them costs 16 s in the reference. The marquee feature is the slowest thing in the product.
	- The same accelerator the prior review's item 12 added to the parser applies here.
	- Detail: `design.md` - Code Review 20260725, item 24.

- 🔘 Code Review 20260725 item 25: a `[value]` selector is looked up by linear scan, so the inline spelling is quadratic.
	- 20k lines of `srv[hostN].port: N` take 9 s against 0.06 s for the equivalent block form.
	- Both spellings are spec-equal, so the user hits a 150x cliff for a cosmetic choice.
	- The read and write paths scan siblings the same way, since the parser's child index is deliberately dropped at load. That part is fine at hand-authored sizes; if it is ever worth touching, name interning is the low-risk option - a cached side index has to be invalidated at five mutation sites in four bindings, and a missed invalidation is a silent wrong value rather than a slow one.

- 🔘 Code Review 20260725 item 26: the validator's "did you mean" rebuilds the whole schema index once per unknown field.
	- Bites when a document is wholesale unmatched - the wrong file, or a schema for another app - which is the case the feature exists for.
	- C compounds it with a linear scan of the legal-chain set where the other three use a hash set.
	- Output is stderr prose, so the fix needs no corpus change.

- 🔘 Code Review 20260725 item 27: `to_canonical` is O(raw-siblings^2).
	- Each raw node rescans the parent's children to decide whether it merges with the line above; the parent's own walk already has that information.
	- 32k raw blocks under one field format in 2.8 s against a 0.05 s parse. Narrow, but free to fix and behavior-preserving.

- ✋ Code Review 20260725 item 28: give the loader opt-out limits.
	- Nothing bounds input size, nesting depth, node count or array length in any binding, and parse costs 35-100x the input in memory.
	- A consuming program handed a config path from a user, a shared directory or a container volume has no way to refuse something unreasonable.
	- The depth cap is the shared fix for item 2 and should land with it; the rest is the wider self-defense story.
	- Depth half landed with item 2 (fixed 512-level cap, `E016`). Size/node/array limits still open.
	- Deferred post-1.0: the depth cap closed the crash class; size/node/array knobs are additive API that can land without breaking anything. A consuming program can bound input size itself before calling parse.

- ✋ Code Review 20260725 item 29: the stable diagnostic code is derived by prefix-matching the prose it is supposed to free.
	- All four bindings recover the code from `msg.starts_with(...)` over ~30 hand-ordered prefixes, so rewording a message can change a code and the ordering is load-bearing.
	- Separately, `V001`-`V099` are fully tabled but `E001`-`E015` and `H001` are enumerated nowhere, while users are told to gate CI on `check`.
	- The doc half is cheap and should land before 1.0; threading the code through every call site is the larger, riskier half.
	- Doc half landed with the item-2 depth work: `E001`-`E016` + `H001` now tabled in the spec's Diagnostics section. Code-threading half still open.
	- Doc half done (the E-table); the code-threading half is deferred - large, mechanical, no user-visible payoff, and every corpus case pins code-per-line so the practical exposure is messages no case exercises.

- 🔘 Code Review 20260725 item 30: the canonical formatter discards blank-line grouping.
	- Comments were rescued as trivia by the prior review's item 4; blank lines are the other half of the same thing and were left out, so `fmt` flattens a grouped config into a wall.
	- Field names are also folded to lowercase and the spec never says so, which makes `fmt --write` a surprise.
	- The trivia model already exists, so this is a per-node flag and one emit line per binding.
	- Detail: `design.md` - Code Review 20260725, item 30.

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

- 🔘 Code Review 20260725 item 35: the profiler stage samples only `fmt`, on a workload where everything is still linear.
	- The read path, `merge`, `validate`, `generate` and the Writer are never sampled, so all three 2026-07-23/24 features could go quadratic without moving a sample.
	- The cheapest half of the fix is a wall-clock number per workload in the run log: a flamegraph shows where time goes, not that total time grew 4x.

- 🔘 Code Review 20260725 item 36: CI installs its lint toolchain unpinned every run.
	- `TOOL_PINS` already tracks the versions the local gate uses, so CI and local disagree about what "passing" was tested against.
	- Actions are also referenced by floating tag rather than commit SHA - generic hardening, small blast radius here.

- 🛠️ Code Review 20260725 item 37: harden the installers' transport and integrity story.
	- `curl`/`wget` follow redirects with no protocol pin or TLS floor.
	- The sums file arrives over the same channel as the binary, so it catches corruption but not substitution; the source tarball, which supplies the drop-in files consumers compile in, is not verified at all.
	- A detached signature over the sums file with the key inlined in the installer is what would make it a real trust root.
	- Transport half done: curl/wget pin https through redirects with a TLS 1.2 floor (install.bash, install-dev.bash's rustup fetch), and install.ps1 floors TLS 1.2/1.3 for every download. The detached-signature trust root still needs a signing-key decision before it can land.

- ✅ Code Review 20260725 item 38: the style guide bans the section rules the code actually uses.
	- "No banner dividers" against 28 of them in the reference, 28 in Go, 18 in Python, 12 in C - and the guide is what a Tier 3 author is told to read first.
	- The code is right: in a 3400-line drop-in file the section rules are the only navigation aid. Amend the rule and pin one spelling per language.
	- One real inconsistency alongside it: `shcl.h` uses the shell house `//•••` rule, which is both off-style for C and the only non-ASCII comment character in the C bindings.
	- Fixed: the guide now sanctions section rules as the one allowed banner and pins each binding's exact spelling; the lone `//•••` shell-style rule in `shcl.h` became the C `// ===` divider.

- 🔘 Code Review 20260725 item 39: panic macros are used outside tests in the reference.
	- Eight sites - six `unreachable!` (four of them in the newest validator and generator code) and two `unwrap()`.
	- Each is provably unreachable today, but they are invariants asserted by a panic in a library whose contract is that it never bails on a whole file, and three ports copy the structure.

- ✅ Code Review 20260725 item 40: the CLI usage block is hand-duplicated across four CLIs with no drift check, and its exit-code line is wrong.
	- It still says exit 6 means a strict load failure; the prior review's item 36 made `check` exit 6 on any error diagnostic, at any strictness.
	- `help`, `version`, a bare invocation and an unknown subcommand are the largest user-visible output in the project and the crosscheck never runs them.
	- Fixed: the exit-code sentence now says `6 check failed or strict load failure` in all four, and the crosscheck pins the whole usage surface - `help`, `version`, a bare invocation, and an unknown subcommand are compared byte-for-byte across bindings on every run.

- ✅ Code Review 20260725 item 41: changelog has no Unreleased section, and contributing.md never explains the corpus workflow.
	- Five landed feature sets since the beta2 tag are recorded only in git; populating it now is also the raw material for the 1.0 notes.
	- contributing.md does state the parity rule, but nothing points a first-time contributor at how to add a conformance case, so their PR will be structurally wrong.
	- Fixed: changelog.md gained an Unreleased section covering everything since beta2 (the 1.0 notes' raw material), and contributing.md gained an "Adding a conformance case" walkthrough.

- 🔘 Glossary of terms

- ✅ Dev-environment install script (Linux, macOS, Windows), runnable via a single `curl` or `wget`. Clones, installs dependencies, states what it will do with an option to abort.
	- Done: `install-dev.bash` at repo root. Linux + macOS directly; Windows via WSL (the dev pipeline is bash). Clones (or detects an existing clone), installs the no-sudo pieces itself (rustup, ruff/mypy/cppcheck via pipx, markdownlint via npm, PSScriptAnalyzer via pwsh), and prints the exact package-manager hint for what needs root (go, python3, gcc, shellcheck). Shellcheck-gated.

- ✅ Release-install script, `install.bash` and cross-platform `install.ps1`. In [repo] root, usage instructions in README.md.
	- Done: both at repo root, README Installing has the one-liners. Latest release via the GitHub API (`--release dev` = newest incl. pre-releases, default; `stable` = newest full release), binary picked by OS/arch, sha256-verified, `code/` (drop-in files) and `scripts/` (wrappers) pulled from the tag's source tarball. Idempotent (atomic binary swap); states the plan and confirms first.
	- Decisions along the way: `objects/` skipped - nothing statically-linkable is published yet (revisit with packaging). Linux user install lands in `~/.local/share/shcl` with the `~/.local/bin/shcl` symlink (one path can't be both the dir and the symlink). Windows user install copies the exe beside the dir instead of symlinking (symlinks need elevation) and adds to the user PATH. macOS/BSD get a clear "no prebuilt binaries yet" pointer at build-from-source.
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

- ✅ Layered loading. `Load(defaults, site, user, ...)` merges later over earlier via the existing merge rule, with CLI and env overrides on top.
	- Done: `merge(base, over)` in all four drop-ins plus the C++ veneer; leaf names override, containers merge by `(name, value)`. CLI `--layer=FILE` (repeatable) on get/fmt/count/instances/set, `--set=PATH=VALUE` as the top layer; `fmt` doubles as merge. Env mapping dropped (belongs to the consuming program). Corpus case `025-layered` + `expected-merged.shcl` golden, all four native runners, crosscheck replays `fmt --layer/--set`; normative spec section.

- ✅ Schema-driven generation. Writer plus schema emits a commented, typed starter config (`shcl init --schema ...`). Depends on schema validation.
	- Done: `generate(schema)` in all four drop-ins plus the C++ veneer; `shcl init --schema=FILE` in all four CLIs. Required fields live, optional commented, wildcards in a trailing block; `desc` -> comment, a fixed-format annotation line summarizes type/constraints (byte-for-byte parity contract). Uses the schema's `default`/`desc` vocabulary. Corpus case `026-init-schema` + `expected-init.shcl` golden, all four native runners assert output + clean reload, crosscheck replays `init --schema`; normative spec section.

### Done

#### Done - Initial requirements

- ✅ Initialize the git repo at `github/` and wire the remote. `main` plus a feature-branch flow.

- ✅ Resolve the open minor items at the end of `spec.md` (currency set, wildcard-missing behavior, `onBad` surface, percent-to-int, fence info-string). All settled inline under "Resolved minor items".

- ✅ Rust reference parser (Tier 1) implementing the full spec, driven by the corpus. The `shcl` CLI builds from it.
	- Done: single-file zero-dependency library plus the CLI (`get`, `fmt`, `check`, `count`, `instances`). Corpus-green, with fuzz smoke in the test run.
	- Note: fuzzing surfaced two formatter rules, now in `spec.md`.

#### Done - Bugs

- ✅ Code Review 20260716 item 1: C CLI reads freed memory on typed array output.
	- `get --int|--float|--datetime --array` with more than 8 elements prints from a stale pointer after the line buffer grows; large arrays segfault.
	- Fixed: owned line entries no longer store a pointer into the growable array; corpus case 008 pins 10-element typed arrays of every kind.
	- Detail: `design.md` - Code Review 20260716, item 1.

- ✅ Code Review 20260716 item 2: Rust parser panics on a multibyte char in the timezone tail of a datetime value.
	- A garbled or hostile config aborts the consumer (exit 134) instead of returning BadType.
	- Fixed: zone tail is now checked byte-wise, so no str slice can land mid-char; corpus 007 `bad5` pins BadType across all bindings.
	- Detail: `design.md` - Code Review 20260716, item 2.

- ✅ Code Review 20260716 item 3: wildcard array reads swallow per-slot NotFound/BadType.
	- A missing sub-path yields a silent zero with status Good - the exact trap the fallback design exists to prevent.
	- `count` and `instances` also disagree on the same wildcard path, breaking index alignment.
	- Fixed in all four bindings: array reads carry per-slot statuses, aggregate = worst slot, `instances` keeps unresolved slots as "", CLI grows `--slots` and per-slot `--default` substitution. Spec pinned, corpus case 009 + a slots column in reads.tsv, crosscheck replays `--slots`.
	- Detail: `design.md` - Code Review 20260716, item 3.

- ✅ Code Review 20260716 item 7: `fmt` ignores `--strictness`.
	- Fixed: `fmt` loads at the requested strictness in all four CLIs; strict failure exits 6 with diagnostics, no output. Crosscheck now replays `fmt` at each `load` row's level.
	- Same in all four CLIs. Detail: `design.md` - Code Review 20260716, item 7.

- ✅ Code Review 20260716 item 8: mixed `*`/field child lines silently build a block array.
	- Fixed: uniform-or-nothing enforced in all four parsers - first mixed field diagnoses an Error (field kept), later `*` lines under that parent are Errors and dropped. Corpus case 010.
	- Detail: `design.md` - Code Review 20260716, item 8.

- ✅ Code Review 20260716 item 9: `field[disc]` matches differently at parse-time vs query-time.
	- Fixed: parse-side selectors match the display form like queries do; create only when nothing matches. Corpus case 011; spec's selector bullet updated.
	- Detail: `design.md` - Code Review 20260716, item 9.

- ✅ Code Review 20260716 item 10: raw-block merge identity ignores the info-string.
	- Decided: info-string is part of a block's identity (fence style is not); equal bodies with different infos stay two instances. All four parsers; corpus case 012; spec updated.
	- Detail: `design.md` - Code Review 20260716, item 10.

- ✅ Code Review 20260716 item 11: reading an array as a string drops quoting and escapes.
	- Fixed: array-as-string is the canonical inline form (minimal quoting, escapes intact, re-parses to the same array) in all four bindings. Corpus rows in case 011; spec's Strings section updated.
	- Detail: `design.md` - Code Review 20260716, item 11.

- ✅ Code Review 20260716 item 12: parse time is quadratic in siblings.
	- Fixed: per-parent (name, value-key) lookup map in all four parsers; sibling scan and hint grouping are linear now. 100k flat lines: reference went from minutes to 0.2s, Python to 1.5s.
	- Detail: `design.md` - Code Review 20260716, item 12.

- ✅ Code Review 20260716 item 13: Python formatter recurses and crashes on deep nesting.
	- Fixed: emit walks an explicit stack; the CLI recursion-limit bump is gone. Depth 25000 formats fine from both the CLI and library callers.
	- Detail: `design.md` - Code Review 20260716, item 13.

- ✅ Code Review 20260716 item 14: PowerShell wrapper exits 0 when the resolved binary will not launch.
	- Resolution accepts any plain file (no executable check); a stale non-executable `shcl` yields empty output and exit 0.
	- Fixed: every ps1 resolution site now goes through `_shcl_executable` (Unix requires an execute bit, Windows keeps a bare leaf); the run-path passthrough is `exit ($LASTEXITCODE ?? 1)`.
	- Detail: `design.md` - Code Review 20260716, item 14.

- ✅ Code Review 20260716 item 15: crosscheck cannot see trailing-newline differences.
	- Command substitution strips them before compare, so a binding that drops or doubles the final newline ships green.
	- Fixed: capture helpers append a trailing sentinel so `$()` has nothing to strip; a dropped or doubled final newline is now a divergence.
	- Detail: `design.md` - Code Review 20260716, item 15.

- ✅ Code Review 20260716 item 16: crosscheck passes with zero comparisons.
	- An empty fuzz dump or a corpus layout change silently drops most of the 1764 comparisons and the gate still passes.
	- Fixed: exits 2 when no comparison ran, when `--extra` matches no `*.shcl`, or below an optional `--min N` floor.
	- Detail: `design.md` - Code Review 20260716, item 16.

- ✅ Code Review 20260716 item 17: demo gif generator ignores step exit codes.
	- A renamed flag renders the error text into the gif and the pipeline publishes it onto the committed asset.
	- Fixed: a step whose exit differs from `expect_exit` (default 0) aborts the render, so cicd skips the publish `cp`.
	- Detail: `design.md` - Code Review 20260716, item 17.

- ✅ Code Review 20260716 item 21: `fmt --write -` silently drops `--write` and exits 0.
	- Should be a usage error pointing at piping stdout instead. Same in all four CLIs.
	- Fixed: `--write` with stdin is a usage error (exit 1) in all four CLIs; `--write FILE` still rewrites.

- ✅ Code Review 20260716 item 23: `field[sel]: value` is grammar-legal but has no spec'd meaning.
	- The value is dropped with an Error diagnostic, so strict loads fail on a line the grammar allows. Align spec, grammar, and code.
	- Fixed by spec'ing the code's existing behavior: a value after a last-segment selector is defined as an `error` (instance kept from the discriminator, value dropped; fails Strict). spec.md Selectors + grammar.abnf note updated; corpus case 018.
	- Detail: `design.md` - Code Review 20260716, item 23.

- ✅ Code Review 20260716 item 24: invalid-UTF-8 command-line args abort the reference.
	- Rust exits 134 (panic); Go/Python/C all exit 3. The reference is the outlier on its own exit-code contract.
	- Fixed: all four validate argv as UTF-8 up front and exit 1 (`invalid argument encoding`); the reference uses `args_os` instead of the panicking `args`.
	- Detail: `design.md` - Code Review 20260716, item 24.

- ✅ Code Review 20260716 item 25: broken stdout pipe gives three different exit codes.
	- `shcl fmt big | head`: Rust 134, Go 141, Python 0. Pick one behavior and pin it.
	- Fixed: uniform die-by-SIGPIPE (141). Rust and Python restore the default SIGPIPE disposition; Go and C already died by signal.
	- Detail: `design.md` - Code Review 20260716, item 25.

- ✅ Code Review 20260716 item 26: C CLI has unchecked `realloc` on input/output paths.
	- OOM segfaults instead of taking the clean exit-70 path the arena already has.
	- Fixed: every CLI allocation goes through `xrealloc`, which now exits 70 with the library's message on OOM.

- ✅ Code Review 20260716 item 27: crosscheck skips the last `reads.tsv` row if the file lacks a trailing newline.
	- One-line `|| [[ -n "$query" ]]` guard fixes it.
	- Fixed with exactly that guard on the read loop.

- ✅ Code Review 20260716 item 30: NUL-joined merge key conflates distinct values.
	- `x: a, b` and `x: "a<NUL>b"` merge to one instance; the second value is silently lost. Make the key injective.
	- Fixed in all four parsers: each cell element (and the raw info-string) is length-prefixed, so the key is injective. Corpus case 017 (count = 2) pins it; the crosscheck skips it (bash can't carry a NUL) and the native runners do the pinning.
	- Detail: `design.md` - Code Review 20260716, item 30.

- ✅ Code Review 20260716 item 32: wrappers invoked via symlink lose the sibling-binary and repo-build fallbacks.
	- Both wrappers compute the script dir without resolving links; resolve the real path first.
	- Fixed: bash follows `${BASH_SOURCE[0]}` through symlinks by hand (bare `readlink` loop, POSIX); ps1 resolves `$PSCommandPath` via `ResolveLinkTarget` into `$script:_SHCL_ROOT`.

- ✅ Code Review 20260716 item 33: ps1 header's own usage example assigns to read-only `$host`.
	- Copying the documented example fails; rename the example variable.
	- Fixed: header example now uses `$svrhost`.

- ✅ Code Review 20260716 item 34: ps1 `SHCL_BIN` probe skips the `.exe` fallback its header promises.
	- Route the pin through the same `_shcl_exe` helper the other probes use.
	- Fixed: the `SHCL_BIN` pin resolves through `_shcl_exe`, so a base name matches its `.exe`.

#### Done - Features and enhancements

- ✅ Style guide: write `style-guide.md` (canonical coding + prose style) and point everything at it.
	- Key decision made prominent everywhere: parity over idiom - bindings mirror the reference's structure over per-language idiom, so byte-for-byte sync stays maintainable and fixes port by diff.
	- Done: guide at repo root with per-language sections and each accepted deviation explained; pointers added in README (Docs), contributing.md (Style), design.md (Direction decisions), and all four binding headers.

- ✅ Code Review 20260716 item 4: `fmt` deletes every comment with no warning, and the spec never discloses it.
	- Direct hit on the hand-author audience; retrofitting comment storage later touches all five codebases.
	- Decide before 1.0: preserve comments as trivia, or spec the loss and warn on `fmt --write`. Detail: `design.md` - Code Review 20260716, item 4.
	- Done: comments survive `fmt` in all four parsers - whole-line comments re-emit above the node the next line binds, trailing ones stay on their line, end-of-file comments land at the end. Spec'd under Comments + Canonical formatter; corpus case 013 pins it and the older cases' expected files now keep their comments.

- ✅ Code Review 20260716 item 5: the Writer half of the spec'd API exists in no binding and has no backlog item.
	- Spec presents Accessor+Writer as the two halves; schema-driven generation depends on it.
	- Done: Writer implemented in all four bindings (full CRUD - typed `set_<T>`/arrays/`raw`/`empty`, `_default` only-if-absent forms, `exists`, `set_comment`, `remove`), each setter the exact inverse of its read. New `shcl set` CLI applies a tab-delimited write-ops script from stdin. Corpus cases 014-016 pair `write.ops` with golden `expected-write.shcl` (matched by every binding's runner + a fixpoint check), the cross-binding differential replays `set`, and a 50k reference fuzz pins the string round-trip. Spec Writer bullet + `design.md` item 5 updated.

- ✅ Code Review 20260716 item 6: README lead code examples call APIs that do not exist.
	- `GetIntOr(...)` (Go), `get_int(..., default=)` (Python), `get_or<T>` (C++) are all missing; a new user's first copy-paste fails.
	- Done: implemented the spec's convenience tier (not gutted the examples), so the README calls are now real as written. Go `GetIntOr`/`Get*Or` + array forms; Python `get_*`/`get_*_array` gained a `default=` param (must-exist and raises without one); C `shcl_get_int/float/bool`; C++ `get_or<T>`. Rust's is the native `get_int(path).unwrap_or(def)` and gained matching `get_*_array` so arrays have the same tier. Semantic pinned everywhere: value only on `Good`, else the call-site fallback (Empty falls back too). Reference unit test + Go test + C++ veneer CHECKs added.

- ✅ Code Review 20260716 item 18: query-side behavior is barely pinned.
	- No corpus rows for wildcards, on-bad modes, or defaults; the fuzz differential compares `fmt` only.
	- The accessor side is where five hand-written ports diverge most easily. Detail: `design.md` - Code Review 20260716, item 18.
	- Done: added a `--rawinfo` CLI type (+ the `rawinfo` reads.tsv type in all four runners) so the info-string read is pinnable; the reference `Document::paths()` drives a fuzz-dump-derived `<name>.reads.tsv` that the crosscheck `--extra` replays (reads over fuzz soup, not just fmt); every scalar read row is also replayed under `--on-bad=error` and `--default=<x>`; corpus case 020-accessor-surface pins wildcards (with a missing slot), a `[value]` selector, and both raw reads. Crosscheck ~1983 -> ~4203 comparisons.

- ✅ Code Review 20260716 item 19: diagnostic wording became a byte-for-byte 5-way contract by accident.
	- `check` prints prose to stdout and crosscheck compares it, so every English message is frozen across bindings - contradicting design.md's per-binding-voice rule.
	- Give diagnostics stable codes; compare codes, free the prose. Detail: `design.md` - Code Review 20260716, item 19.
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
	- Also: hex `-0x8000000000000000` (i64 min) reads BadType while the decimal spelling works. Detail: `design.md` - Code Review 20260716, item 31.
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

- ✅ Accessor: two-tier junior-friendly surface (convenience default plus full status), consistent across all bindings. A supplied default implies default mode.
	- Done alongside review item 6: the full status tier (`Read`/`read_*`) already shipped; the convenience default tier now ships in every library binding (`GetIntOr`/`get_*(default=)`/`shcl_get_*`/`get_or<T>`/native `unwrap_or`), plus the CLI `--default` for the wrapper bindings. Supplying the fallback is Default mode - value on `Good`, fallback otherwise.

- ✅ README rewrite: problem-first pitch, file and read-call examples, a format comparison table, a "wrong choice" section, and honest alpha status.

### Future and/or deferred

### Canceled
