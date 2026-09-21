<!-- markdownlint-disable MD010 MD033 MD041 -->
# Conformance corpus

Golden cases that pin every released SHCL binding to identical behavior. Each independent parser runs this corpus in CI; drift on any case means the binding is non-conformant and is not released. (CLI-wrapper bindings and companion typed surfaces inherit conformance from their core.)

## Case layout

Each case is a directory `NNN-short-name/` containing:

- `input.shcl` - the source, usually deliberately messy.

- `expected.shcl` - the canonical formatter output for that input (block form, tabs, insertion order, minimal quoting, redundancy collapsed), at Standard strictness.

- `reads.tsv` - expected typed reads (required). Columns, tab-separated: `query` `type` `expected` `status` `[level]` `[slots]`. `type` uses `int|float|bool|datetime|string|raw|rawinfo` and `[]` for array forms (except `raw`/`rawinfo`, which have no array form), or the pseudo-calls `count`/`instances`/`load`/`lost`/`children`/`paths`. `rawinfo` reads a raw block's info-string (the fence tag) rather than its content. `expected` is the value (`-` when not applicable); `status` is one of `Good|Empty|NotFound|BadType|Multiple`. The optional fifth column is the strictness level (`loose|standard|strict`), default `standard`. The optional sixth column (requires the fifth) pins the per-slot statuses of an array read, `|`-joined in slot order; the row's `status` is then the worst slot. The `load` pseudo-call asserts whether the document loads at that level: query `-`, expected `ok` or `fail`, status `-`. The `lost` pseudo-call asserts the document's lost count: query `-`, expected the number, status `-`. The `children` pseudo-call lists a path's children in file order, repeats kept, `|`-joined: an empty query is the root, a missing path lists nothing, status `-`. The `paths` pseudo-call lists every path in the document once, `|`-joined and quoted where a name needs it: query `-`, status `-`. In `expected`, a newline inside a raw-block value is written `\n` (a literal newline or tab would break the TSV).

- `expected-diags.txt` - the diagnostic golden (required): the exact `check` stdout at Standard strictness - one `line N: Severity: CODE` line per diagnostic in emission order, then the summary line (`ok (N diagnostic(s))`, or `failed: N diagnostic(s), M error(s)` when errors are present). Pins count, line, severity, and stable code per case, including the mandatory repeated-leaf hint (`H001`) and the zero-diagnostic cases.

- `write.ops` + `expected-write.shcl` (optional, as a pair) - the **Writer** dimension. `write.ops` is a script of write operations (one per line, tab-separated); each binding applies it to `input.shcl` via its library Writer and must produce `expected-write.shcl` byte-for-byte, and that output must be a formatter fixpoint. A blank line or one starting with `#` is skipped. Op grammar (`<T>` = `int|float|bool|string|datetime`):
	- `<T><TAB>PATH<TAB>VALUE` - set a scalar. `datetime` VALUE is any accepted spelling and is stored canonically.
	- `<T>-array<TAB>PATH<TAB>V1<TAB>V2...` - set an inline array (no elements = an empty value).
	- `<T>[-array]-default<TAB>...` - set only if the path does not already resolve.
	- `literal<TAB>PATH<TAB>TEXT` (and `literal-default`) - set from value syntax rather than data, so `80, 443` stores a two-element array where the `string` op would store one quoted string. `TEXT` is read as the value half of a line: it is trimmed, a `#` outside quotes ends it wherever it sits, and text carrying a line break, an unclosed quote or a leading `[` is rejected.
	- `raw<TAB>PATH<TAB>INFO<TAB>CONTENT` (and `raw-default`) - set a raw block; `INFO` may be empty. `INFO` is taken as written - the escape decode below is for `CONTENT` only.
	- `empty<TAB>PATH`, `comment<TAB>PATH<TAB>TEXT`, `remove<TAB>PATH`.
	- `string` and `raw` `CONTENT` values decode `\n` `\t` `\\` (so a multi-line value fits on one op line); no other escapes are interpreted. A `comment` value decodes them too, but a comment is one line, so a decoded `\n` only gets the op refused. The setters re-encode for storage, so a value read back equals the logical value it was set from.
	- Op values are gated with the reference's grammar before any write: an int is an optional sign plus ASCII digits within i64 range; a float follows the Rust `f64` grammar (sign, `inf`/`infinity`/`nan` case-insensitive, or decimal digits with optional `.`/exponent - no underscores, hex, padding, or non-ASCII digits; overflow stores `inf`). A malformed value, a bad datetime, or an unusable path (wildcard, missing `[#N]`) rejects the op: the CLI exits 1 with empty stdout.

- `write-bad.ops` (optional) - the **bad-op** dimension. Each line (same grammar as `write.ops`; blank/`#` skipped), applied ALONE to a fresh parse of `input.shcl`, must be rejected and leave the document unchanged. The differential harness replays each line through every CLI's `set` and compares stdout and exit code.

- `schema.shcl` + `expected-validate.txt` (optional, as a pair) - the **schema validation** dimension (spec.md "Schema validation"). `schema.shcl` is a schema (itself plain SHCL); `expected-validate.txt` is the exact `check --schema schema.shcl input.shcl` stdout at Standard strictness - the document's parse diagnostics, then the validation diagnostics (`V###` codes), then the summary line, under the same format as `expected-diags.txt`. Each binding replays it via its library `Validate`; a schema that does not itself load cleanly must yield the single `line 0: Error: V099` diagnostic, mirroring the CLI.

- `init-schema.shcl` + `expected-init.shcl` (optional, as a pair) - the **generation** dimension (spec.md "Schema-driven generation"). `init-schema.shcl` is a schema (with the generator-only `desc`/`default` vocabulary); `expected-init.shcl` is the exact `init --schema=init-schema.shcl` stdout. Each binding runs its library `generate` and matches the golden byte-for-byte; the golden must itself load with no error diagnostics AND validate clean against the schema that produced it; the differential harness replays `init --schema=...` across every CLI, with and without `--no-banner`. The golden carries the format footer, since that is what a default run writes; each runner also generates with `no_banner` set and checks that what comes back is exactly the golden minus the footer, so the flag needs no goldens of its own.

- `expected-migrate.shcl` + `expected-migrate-diags.txt` (optional, as a pair) - the **migration** dimension. `expected-migrate.shcl` is the exact `migrate` output for `input.shcl`, byte for byte. `expected-migrate-diags.txt` is the load diagnostics of that output at Standard strictness, in `expected-diags.txt`'s format, so a rewrite that no longer loads cannot pass. Each binding runs its library `migrate` and matches both, telling it the input is a 2.x file, since every case here is one by construction and that is the one thing migrate cannot read off the text. The output therefore carries the `Format` line migrate stamps a file it rewrote. A `fmt` fixpoint is not required, because migrate keeps the author's layout, but `migrate` on its own output must change nothing, and every runner checks that over every case. The differential harness replays `migrate` across every CLI both ways, with and without `--from-2x`, since the two take different arms.

- `expected-merged.shcl` (optional; triggers the **layered-load** dimension) - the canonical form of merging any `layer*.shcl` files (read in filename order, which is priority order, lowest first) under `input.shcl` (the highest file layer) via the library `merge`, then applying the `merge.sets` overrides (optional; one `path=value` per line, `#` comments skipped) as the top layer. Each binding folds the layers with `merge` and matches the golden byte-for-byte, which must also be a formatter fixpoint; the differential harness replays `fmt --layer=... --set=... input.shcl` across every CLI. A later layer's leaf name replaces earlier same-named leaves (real override for scalars, arrays, raw blocks); container instances merge by `(name, value)` like the in-file rule.

## Notes

Case `001` corrects the autoformat shown in `../../../notes.txt`: instances are kept in **insertion order** (Chicago, Cleveland, Boston, Philly), not sorted alphabetically, and values are **minimally quoted** (city names stay bare unless a reserved char forces quotes). Those two points are the intentional divergence from the original by-example draft.

Case `002` pins the stacked (`*`) array form: an inline comma array and the `*`-per-line form read byte-identical and both canonicalize to the inline form, while repeated leaf lines stay separate **instances** (not an array).

Case `003` pins the strictness bundles on coercion: currency, `%`, float->int rounding, and the widened boolean set exist only at `loose`; `strict` narrows booleans to `true`/`false`.

Case `004` pins load behavior per level: a malformed line is skipped with a diagnostic at `loose`/`standard` (the rest of the file still reads), and fails the whole load at `strict`.

Case `005` pins raw-block binding: a fence is a value line for its parent field - both spellings (same-line and the canonical child-indent) bind as the field's value; a fence under an already-valued field creates a new instance, addressed with the normal `[0]`/`[#N]` selectors.

Case `006` pins forgiving inline arrays: stray commas never error. Leading, doubled, and trailing commas drop their empty slots (`red,,blue` -> `red, blue`), an all-comma value (`,,,`) is the empty array, and a `""`-quoted element is the one way to keep a deliberately empty slot.

Case `007` also pins that a multibyte char inside a time-shaped value's zone tail is a plain `BadType`, never a crash.

Case `008` pins 10-element typed arrays of every kind (int, float, bool, datetime) - large enough to force output-buffer growth in the CLIs, which is where per-element formatted output can go stale.

Case `009` pins wildcard slot alignment: a missing sub-path keeps its slot (per-slot `NotFound`, value zero/default), an uncoercible one reads `BadType`, the aggregate status is the worst slot, and `count`/`instances` stay index-aligned with the read (unresolved slots enumerate as "").

Case `010` pins uniform-or-nothing: mixing `*` elements and field children under one parent is not a block array - the first mixed field diagnoses an Error (and keeps the field), every `*` line after a field child is an Error and is dropped, and the document loads at `standard` but fails at `strict`.

Case `011` pins selector-vs-instance matching on the display form: `base[Boston, MA]` selects the existing array-valued instance `base: Boston, MA` instead of creating a second one. It also pins array-as-string: a multi-element value read as one string is the canonical inline form (minimal quoting, escapes intact), while the array-of-strings read unquotes and applies escapes per element.

Case `012` pins raw-block identity: the info-string is part of a block's value, so equal bodies with `sql` and `python` infos are two instances (never a silent merge that drops an info).

Cases `014`-`016` pin the **Writer**. `014` builds a document from an empty base (scalars, arrays, a comment above a later-set field, an empty section, and a `-default` that no-ops when the field already exists). `015` edits an existing document (overwrite the first instance of a leaf, `-default` that keeps the present value, `remove`, and a `[value]` selector that adds children under the matching instance). `016` pins the emit hazards: raw blocks (fence chosen so the content cannot close it early, info-string as identity), tricky strings (tab/quote/backslash and a fence-lookalike, minimally quoted so they read back verbatim), an explicit empty string (`""`, distinct from an empty value), and a bare 8-digit date stored canonically.

Case `013` pins comment preservation through `fmt`: a whole-line comment re-emits above the node bound by the next line (merged instances concatenate theirs), a trailing comment stays on its line (a second one from a merged instance moves above), comments among `*` elements ride the field line, a comment between a bare header and its fence attaches to that field, `#` inside a raw block stays content, and comments after the last binding line re-emit at the end. The older cases' expected files carry their inputs' comments too.

Case `017` pins merge-key injectivity: a single element holding a literal NUL (`x: "a<NUL>b"`) stays distinct from the two-element array `x: a, b` (`count = 2`), where a bare-NUL-joined key would merge them and drop the second. The input carries an actual NUL byte, so the cross-binding differential skips it (bash cannot hold a NUL) and the four native runners do the pinning.

Case `018` pins `field[disc]: value`: a value after a last-segment selector is an `error` (the instance is created from the discriminator, the value dropped), so the document loads at `standard` but fails at `strict`, and `city` ends up with the two discriminator instances.

Case `019` pins i64 bounds across hex and decimal spellings: the int read parses the magnitude as u64 and range-checks it against the sign, so `-0x8000000000000000` reads i64-min like its decimal spelling, `0x7fffffffffffffff` reads i64-max, and the positive `0x8000000000000000` overflows to `BadType`.

Case `020` pins the accessor surface that ports diverge on: wildcard reads across instances (`server[*].port`, and `server[*].region` where one instance lacks the sub-path, so a slot is `NotFound` and the aggregate is the worst slot), a `[value]` selector read, and a raw block read both ways (`raw` for content, `rawinfo` for the `sql` info-string).

Cases `021`-`024` pin the schema validation dimension. `021` is the all-pass sweep (every constraint kind satisfied, including a quoted wildcard path, constraints for one path split across two merged `field` instances, and an empty value passing `type: bool`). `022` produces every data-validation code `V001`-`V007` at least once - unknown fields with and without a "did you mean" suggestion, `required` missing at document scope (line 0) and per wildcard instance (that instance's line), `repeat` violated at both scopes, plus the `H001` hint riding along in the combined output. `023` produces the schema-fault codes (`V090`-`V093`) and pins that a broken schema suppresses data validation (the document's own violation must NOT be reported). `024` pins `V099`: a schema that does not parse cleanly yields exactly one line-0 diagnostic.

Case `041` pins generation over fragments: `desc`/`default` flow through the mount expansion, both mounts of the shared `node` fragment generate one full level, and the recursive `children` mounts go in the trailing block with the fragment's name in the type column; the golden validates clean against its own schema.

Case `040` pins fragment faults: `inherits` naming no declared fragment is `V095`, a nameless declaration and a non-`field` key inside one are `V094`, all at schema lines. The surviving constraint checks nothing here (its mount names the missing fragment), and the unknown-field sweep is off under faults, so the document's unknown fields draw no `V001`.

Case `039` pins fragments end to end: a self-recursive `node` fragment validates a 10-level layout tree clean, the same fragment mounted at `doc.layout` (an alias) still flags an unknown field beneath it, and a `repeat` declared on a fragment field disavows the document's `H001` under `--schema` (the plain-`check` golden keeps the hint).

Case `038` pins open-section schema validation: a `*` name segment in a schema path resolves every child of `indicators` regardless of name, so `required: yes` on `indicators.*.period` fires per child (the `V002` anchors at the offending child's own line), a field outside the declared shape is still `V001`, and children of any name are legal without enumeration.

Case `037` pins the name wildcard in lookups: `*` slots across children of any name with per-slot statuses (a childless slot keeps `NotFound`), composes with `[value]` selectors, keeps `count`/`instances` slot-aligned, scalar reads on it stay `Multiple`, and a field literally named `*` is addressed quoted (`"*"`), never by the wildcard. The write ops pin `remove` across wildcard slots, and `write-bad.ops` pins that setters refuse `*` paths (path validated whole, document unchanged).

Case `036` pins schema-declared repeat suppression: with `--schema`, an `H001` whose field declares a repeat upper bound above 1 is dropped (repetition is that field's instance mechanism by declaration) while an undeclared repeat keeps its hint; the plain `check` goldens keep both.

Case `035` pins the `H002` merge hint: a binding that merges with a non-adjacent earlier one is hinted at the later line. Adjacent re-mentions and dotted redundant-path re-opens stay silent. Its strict `load ok` row is one of the two that pin the strictness table's hint row - a hint never fails a load at any level.

Case `034` pins comment placement fidelity: a comment run written deeper than the next binding hangs on the block it sits in (re-emitted after that block's last child, at the block's indent), an over-deep comment normalizes to its block's level, and end-of-file comment regions keep the blank lines between them.

Case `033` pins escape-applied selector matching: a `["q\"uote"]` selector finds an instance written `'q"uote'` (and a bare `[it's]` finds `"it\'s"`) - the match is logical string against logical string, whichever spelling either side used. The write op does the same through the writer's place walk: the set applies to the existing instance instead of creating a spurious second one.

Case `032` pins blank-line grouping: a run of blanks collapses to one, a blank before a comment group stays with the group, and the blank survives the format round-trip (the file never starts with one).

Case `031` pins the unterminated-quote diagnostic (`E017`): a value that opens a quote it never closes, where the trailing comment still ends it, and an array-looking one, where the comma still splits the elements. Each draws one error, the pieces are kept as written (fmt re-quotes them), and the load still succeeds at Standard and fails at Strict.

Case `030` pins the generator's edge handling: an `[#N]` path and an unmaterialized optional wildcard go in the trailing not-generated block (an emitted `#` would start a comment), and a newline smuggled through an `allowed` value or a `default` stays escaped (`\n` in the annotation; the quoted spelling on the value line) instead of injecting a line. The golden validates clean against its own schema like every generation golden.

Case `029` pins the write-op value gates and unusable-path rejection: the good script covers the boundary values every binding must ACCEPT (`1e400` -> `inf`, `.5`, `5.`, `INF`, `nan`, i64 min, a `+` sign) and `write-bad.ops` covers what every binding must REJECT identically (hex, junk, trailing garbage, out-of-range, underscores, padding, non-ASCII digits, empty, malformed floats, a bad datetime, a wildcard path, a missing `[#N]`).

Case `043` pins the cost of a recursive schema: a shape mounted from two paths that both reach the same node is checked once, not once per path. Without that, a document a couple of dozen levels deep doubles the work per level and validation stops finishing, so a regression here shows up as a case that hangs rather than one that fails. The generator's own limits are pinned by a reference unit test instead - a schema long enough to reach them would be a corpus file nobody could read.

Case `042` pins the late-merge rule: a value that only becomes final after its siblings were keyed - a stacked list closing onto an earlier instance's value, a fence filling an empty field that matches an earlier block - still merges, and merging two parents merges the identical children they now share. Its layer file also pins the trivia side of a merge: the higher layer's section comment and trailing comment survive onto the matched base node, and a footer comment both files carry appears once.

Case `028` pins the 512-level nesting cap: a 513-segment dotted path draws exactly one `E016` and is skipped (the sibling line survives, strict load fails). The at-cap boundary and the Writer's refusal to create deeper are pinned by a reference unit test - an at-cap golden would be a 130 KB file for no extra coverage.

Case `027` pins the layered-merge wrapper rule: a childless over-node whose base-side name group has a container instance merges instead of replacing - bare `server:` appends an empty instance, `server: web1` with no body leaves both base servers untouched - while a childless leaf group still overrides (`mode:` clears `mode: fast`). The over layer's trailing comment-only body rides through as an orphan.

Case `026` pins schema-driven generation: `init-schema.shcl` carries `desc`/`default` on required and optional fields, an `allowed` set (rendered `one of: ...`), int and float ranges, a `repeat` bound, and a required `server[*].host` wildcard. The golden `expected-init.shcl` shows must-exist fields live (required, and `replicas` via its repeat lower bound), optional fields commented out, and the wildcard filled in dotted form (`server.host:`) because `server.port` materializes its parent - so the golden validates clean against its own schema.

Case `025` pins layered loading: a defaults layer, a site layer, and `input.shcl` as the user layer are merged bottom-up, then a `merge.sets` override. It exercises scalar override (`port`), repeated-leaf override (the whole `tags` list is replaced, not appended), container merge by `(name, value)` (both layers' children of `server: web1` combine), a new container instance from a higher layer (`server: web3`), and a `--set` override applied last.

Note when reading comparison counts: the fuzz seed set includes the corpus inputs, so adding a case shifts every mutated input after it and moves the derived total either way. The count is a per-tree constant, not a coverage score; the corpus-only count is the one that tracks coverage.

Case `046` pins partial validation under a schema fault: a bad `min` is a `V092` at its schema line, the surviving constraints still flag a wrong type and a missing required field, and the unknown field draws no `V001` (the sweep needs a fault-free schema).

Case `047` pins that a schema fault which loses a path (`field: workers, extra`, `V093`) still holds the unknown-field sweep back, so `mystery` draws no `V001`.

Case `048` pins `H002` three levels deep: re-opening `table: users` hints at the reopened line and at each nested re-mention, beside the `H001` for the repeated `col`. Under the schema, `reopen: true` drops the top hint and `repeat` drops the `H001`, while the nested hints stay.

Case `049` pins retained lines: a malformed line inside a block and one at the top level (`E014`, `E013`) are kept verbatim, written back where they sat, and count nothing lost.

Case `050` pins that quoting decides a value's elements: `x: "a, b"` is one element and `x: a, b` two, so they are two instances (`H001`) and `x["a, b"]` selects the first.

Case `051` pins a selector that reaches an instance spelled another way: a bare `x[a"b, c]` finds the two-element instance and a quoted `x["a\"b, c"]` the one-element one, while `y["p, r"]` and `z["m, n"]` match nothing and create an instance.

Case `052` pins edge whitespace through the writer: a value set with a no-break space, a vertical tab, a form feed or another Unicode space at an edge is quoted on output, so it reads back whole.

Case `053` pins blank lines in a raw body: a line longer than the closing fence's indent keeps the rest, a shorter one keeps what it has, and an empty line stays empty.

Case `054` pins escapes in names: `"a\"b"` and `'a"b'` are one name, so the two lines are a repeated leaf (`H001`), and a tab, a backslash and an apostrophe in a quoted name read back, from a lookup and from a schema path quoted twice.

Case `055` pins a raw block whose body is whitespace only: each line loses just what it shares with the closing fence's indent and keeps the rest, so the spacing survives a round trip and the block cannot gain a level per pass.

Case `056` pins the merge rule for an empty binding: a raw block in the higher layer fills a same-named empty binding below (`blk`), exactly as a fence line fills one inside a single file, while a valued binding still appends beside it (`two`). The merged golden is what parsing the two layers run together produces, which is what makes it a formatter fixpoint.

Case `057` pins that a skipped binding line keeps its indent level (`E018`): the lines written under it are skipped with it instead of attaching one level up, the next line at its own indent still binds where it should, and a malformed line that is retained as trivia loses its block the same way. The strict load fails.

Case `058` pins raw-block nesting: the closing fence's indent is what comes off each body line, so a body whose lines all sit past the fence keeps that shared indent, a line flush left keeps nothing, and the write op stores an info-string trimmed the way a fence line reads it.

Case `059` pins the two raw-block errors: a fence with no parent field (`E006`, the block is dropped) and a block that never closes (`E005`, the content runs to the end of the file and the block still binds).

Case `060` pins the stacked-list errors: an element with no parent field (`E007`), an empty element (`E009`, a `*` followed only by a comment), a bare comma in an element (`E010`), and an element under a field that already holds a value (`E011`). The survivors still read as the list.

Case `061` pins `E012`: a dedent to a column that matches no open level is skipped, and the next line at a real level binds where it belongs.

Case `062` pins a writer fold: `empty b` clears the value of `b: 1, 2`, which then merges with the `b` below it, leaving one `b` holding `a: 2`.

Case `063` pins `remove` followed by a `-default` on the same path: the default finds the path gone and writes it again, at the end.

Case `064` pins that `raw` refuses an info string holding a `#`, which the fence line would read as a comment.

Case `065` pins bracket text after the colon (`ports: [80, 443]`): `E019`, kept as written, nothing read and nothing lost. Its `write-bad.ops` refuses the same text from `literal`, in the default form, behind a comment and with no closing bracket, and `write.ops` accepts the quoted string.

Case `066` pins comment text through the writer: a trailing space comes off, a trailing no-break space stays, and an empty comment is a bare `#`.

Case `067` pins the i64 edge at the loose float fallback: `9223372036854775807.0` lands on 2^63 as a double and is `BadType` as an int, while `-9223372036854775808.0` reads.

Case `068` pins a `#` on a fence line in both spellings: it ends the label and opens the line's comment, so ```` ```c# ```` labels the block `c`.

Case `069` pins traversal through the `children` and `paths` rows: children in file order with repeats kept, nothing for a missing path, and every path once, quoted where a name needs it.

Case `070` pins a selector over a raw block and a scalar with the same display: `x[hi]` binds the raw block and `x["hi"]` the scalar, and a read of `x[hi]` counts both.

Case `071` pins the schema-fault arms: a constraint given two values where it takes one (`type`, `repeat`, `inherits`, `min`, `max`), given twice (`allowed`), or given a value it cannot take (`maybe`, `abc`, a `min` on a string) draws `V092` at its schema line.

Case `072` pins `allowed` on typed fields: a raw block against a string list, a float array beside `min` and `max`, a bool, a datetime and an int are each compared by type.

Case `073` pins `init` for a valued parent: the child lines select the instance by its value (`srv[web].port`), a quoted default stays quoted in the selector, and the output validates against its own schema.

Case `074` pins the float range: a value past the double range is `BadType` at every level, as a scalar, an array element or loose currency, the largest double reads, and an underflow reads as 0.

Case `075` pins that a skipped line holds its indent level in the other two skip shapes too: a line refused with `E012`, and a `*` line with no space (`E013`). What is written under either is skipped with it (`E018`), a fence line at a bad indent takes its whole body with it, and a second line at the same bad indent is refused the same way rather than binding one level up.

Case `076` pins a value written after an index selector on the last segment (`a[0]: 2`): the instance is selected and the value is reported (`E002`) and counted as lost, exactly as after a value selector, so a save cannot quietly delete it. A same-line fence there is the same case. A value after an index that is not last still binds the deeper leaf.

Case `077` pins that a fragment mounted at one node by two schema paths (`srv` and `srv[*]` both inheriting `unit`) runs once per node: each fault under it is reported once, not once per path.

Case `078` pins what a schema disavows: a `repeat` above 1 drops the `H001` hint for a field whose path carries an escaped quote, a `reopen: true` drops the `H002` hint, and a `repeat` or `reopen` that faults (`V092`) disavows nothing, so the hint stays beside the fault.

Case `079` pins the order a merge appends in: unmatched higher-layer nodes keep that file's order (`c, a, b, a`) rather than regrouping by name, and a footer line the layer repeats itself is kept twice, while one the base already carries is carried over once. Merging onto an empty base is the identity.

Case `080` pins float spelling on the values where shortest-round-trip formatters are allowed to differ: powers of two, whose rounding interval is lopsided so the closest short spelling does not read back and the neighbor does, and exact ties between two spellings of the shortest length, which round to even. Every binding writes the same digits.

Case `081` pins wildcards that compose: `server[*].*` reads every child of every instance, `*.port` keeps a slot per top-level field with the worst status as the aggregate, a wildcard on a missing parent is `NotFound` with a count of zero, and the write removes `server[*].*`.

Case `082` pins `init` over wildcards: a filled wildcard is itself a valued parent (`a.b[bee].c`), an all-digit default is quoted inside a selector (`num["8"]`), and a trailing wildcard fills from its own line. The golden validates against its schema.

Case `083` pins a merge with layers that start with blank lines, one of them holding only a comment: the result equals merging the layers' canonical forms.

Case `084` pins value identity across spellings: `"q\"uote"` and `'q"uote'` are one instance, and so are `nobs` and `'nobs'`. A selector in either spelling finds it, and a layer merges into it.

Case `085` pins `allowed` on a time: `Z`, `+00:00` and `-00:00` are all the moment `12:00:00Z`, a zero fraction matches the same clock without one, and a time with no offset is not the `Z` moment (`V004`).

Case `086` pins a bare index on a binding line that names no instance: `a[5].b` is `E003`, dropped and counted lost, while `c[1].d` reaches the second `c`.

Case `087` pins where a merge puts a layer holding only a comment: above the base's trailing comment, after the field. The field is spelled without a colon (`E015`) and still binds.

Case `088` pins a quote in the middle of a bare value as content: `don't panic` leaves its trailing comment a comment, and apostrophes, mid-text double quotes and a quoted `#` read as written.

Case `089` pins bracket text behind a colon that is not the field's own: after a quoted name holding one, a selector holding one, and selector sugar, `[80, 443]` is still `E019`, kept, with nothing lost.

Case `090` pins a crossed range (`min` above `max`): `V092` at its schema line, while the other constraints still apply and the unknown-field sweep still names `bogus` and `other`.

Case `091` pins a leaf override in a merge: the base leaf's value goes, but a retained bad line above it and a bad `*` line under an overridden field stay in the merged file.

Case `092` pins that a default form refuses a wildcard path whether or not its slots resolve, while the same default on a named instance writes only what is missing.

Case `093` pins `allowed` on a datetime with an offset: the same instant written at another offset matches, across a day boundary too, and the same clock at another offset does not.

Case `094` pins what trimming takes off a bare piece: a space, a tab and a carriage return. A no-break space, a line separator, a vertical tab or a form feed at an edge is content, kept and quoted on output.

Case `095` pins that a dropped element holds its level: what is written under an `E008`, `E009` or `E011` element is `E018` and lost with it.

Cases `096` and `097` pin a raw block that never closes, with and without a final newline: `E005`, and the block reads `body` either way.

Case `098` pins a comment added to a node that already has one, with a blank line above the node: the new comment goes under the old one, and the blank moves above both.

Case `099` pins `literal` on text that only looks like syntax: a fence opener and a spaced string are stored as quoted strings, while an unclosed quote in the first or a later piece and a leading `[` are refused.

Case `100` pins integers past the i64 range: hex, decimal and quoted-thousands spellings read as floats and are `BadType` as ints.

Case `101` pins an empty name: two `""` leaves are a repeated leaf (`H001`), and a schema declaring `repeat: 1, 5` on the field drops the hint. It carries the `H001` half of the strict `load ok` pair described under case `035`.

Case `102` pins one field spelled twice in a schema (`w` and `w[*]`): `init` writes one line.

Case `103` pins an all-digit selector past the u64 range: an index naming no instance, so the binding line is `E003` and lost, a read is `NotFound`, and a write through it is refused.

Case `104` pins an element with no parent field at the top of a file (`E007`, dropped): its trailing comment is kept.

Case `105` pins bare selector bodies holding an apostrophe, a mid-text quote or a trailing backslash, and quoted ones spanning a `]` or holding a `#`, each followed by a comment that stays a comment. No diagnostics and nothing lost.

Case `106` pins the 2.x selector sugar under the 3.0 rules: `base:[Boston]` is bracket text, `E019` and kept, the line under it is `E018`, and the load fails at Strict.

Case `107` pins recursive fragment mounts under the validation memo: a type fault seven mounts down (`V003`), a star path through a mount, and an unknown leaf at the bottom (`V001`) are all still reported.

Case `108` pins bracket text holding an index or a wildcard (`[80]`, `[*]`): `E019`, kept, nothing lost.

Case `109` pins a `#` in a bare selector body on a file line: it opens a comment, the selector is left unterminated (`E014`), and the line is kept with nothing lost. `[#N]` is a lookup spelling only.

Case `110` pins `init` for a schema listed out of tree order, a parent after its child with another field between: the output keeps tree order and loads with no diagnostics, hints included.

Case `111` pins a backslash in a bare selector body as a character: `p[C:\temp]` and `r[a\nb]` select the instances already there rather than creating second ones.

Case `112` pins three setters through the tokenizer: a comment's trailing blanks come off, a raw info string keeps its leading blank, and a quoted `#` given to `literal` stays in the value. A comment holding a line break is refused.

Case `113` pins `init` spelling a line break in a by-value selector and in a name escaped, so neither starts a new line.

Case `114` pins raw reads: an empty binding is `Empty` for `raw` and `rawinfo`, a value that is not a block is `BadType`, and a block reads its body and label.

Case `115` pins a carriage return as a blank outside a raw body: trimmed at the edge of a name, a selector, a value, an element and a comment, and content in the middle of one.

Case `116` pins a selector body that opens a quote it never closes: `E017`, kept as bare text quotes and all, so `srv["prod]` is its own instance and not `srv[prod]`, and a line whose selector and value both open one reports each.

Case `045` pins comment depth under childless headers: a header whose children are all commented keeps them indented under it (top-level, nested, and at end of file), while a commented line trailing a live child keeps the existing trails-the-binding placement.

Case `044` pins the value-syntax setter: an array, a single element, a quoted element keeping its internal comma, trimming, a `#` outside quotes ending the value wherever it sits (`#ff0000` leaves it empty, `a#b` keeps `a`, `x#` keeps `x`), an empty value, and the only-if-absent form both skipping an existing path and creating a new one. Its `write-bad.ops` pins the two rejections - a value opening a quote it never closes (the same text the parser reports `E017` for) and a wildcard path.

Case `117` pins how `migrate` spells a 2.x `name:[disc]` line on a last segment. A discriminator carrying a fence run or a leading `[` is quoted when the sugar becomes a value, so the rewrite opens no raw block and no bracket text, at top level, nested, spaced, with a trailing comment and with children below. An ordinary discriminator stays bare (`plain: Boston`), one the formatter would quote is quoted (`city: "New York"`), and the author's quotes are kept. The input itself is refused line by line under the current rules, which is the damage migrate exists to prevent.

Cases `118` and `119` pin how `migrate` spells a piece holding a backslash: always in double quotes, in a value, a star element, a selector and the sugar arm. 2.x read a backslash in bare and single-quoted text as an escape, so the migrated file reads the same under 2.x, and a second run changes nothing. `118` also pins a bare backslash before a comma, the line end and a comment, where a spelling that reads right alone would shield what follows it. `119` also pins a quoted discriminator with text between its closing quote and the `]`, which 2.x refused as a malformed line, so `migrate` leaves it as written.

Cases `122` and `123` pin a backslash in a selector body, which 2.x shielded inside quotes only: a bare body runs to its first `]`, so the line still reads and the rest of it migrates. `122` is clean under 2.x, so the migrate gate compares it document by document; `123` carries the sugar spellings, where a comma behind a backslash never made the brackets an array, and a real two-element bracket array stays as written.

Case `124` pins a CRLF file whose raw block closes before a line `migrate` rewrites. The closing fence ends in a carriage return, and the block still has to close there, or the sugar and the backslash value after it are taken as block content and left as written.

Case `120` pins `init` on a last-segment by-value selector with a default: the line is spelled as the bare path carrying the default (`env: prod`), and without a default the selector line stays.

Case `121` pins that a default form judges the value as well as the path. Its `write-bad.ops` gives bracket text and an open quote on paths that already resolve, where the form writes nothing, and bracket text again on a path that does not. Every line is refused either way.

Case `125` pins an optional child of an optional valued field in `init`: the commented child selects the parent by its default (`# srv[web].port: 80`), so uncommenting both lines names one instance. The input is that output with both lines uncommented.

Case `126` pins a skipped field line whose value opens a raw block, under a skipped parent (`E018`) and at an indent that matches no open level (`E012`). The body goes with the line. Read as lines, it bound its own `port` and `name`, and its closing fence opened a block that ran to the end of the file.

Case `130` pins a wildcard remove over a leaf that repeats under one instance. A read calls such a slot ambiguous, and the remove used to skip it, leave the data and exit 0. Its reads still expect the ambiguous slot, so the two answers are pinned apart.

Case `129` pins a remove that matches many nodes at once. One path takes every child of a parent, another takes several top-level instances, a third matches nothing, and a write after them shows the parent and the name index still answer. Each match used to be dropped on its own, rebuilding the parent's whole child list every time.

Case `128` pins the column a kept `*` element holds, beside case `095`'s dropped one (20260918b item 28). A field written deeper binds under the field (`E001`), the next field at the element's column binds as its sibling, and an element after a field child is `E008`. Every later sibling used to be `E012` and lost.

Case `127` pins the `init` shapes where the generator guessed what the scanner would read (20260918b items 6, 7, 8, 25, 26 and 27). The first of two lines on one path is the instance its child selects. Two by-value fields with different values are two instances. An all-digit value past 64 bits is a quoted body, since bare it reads as an index. A quoted array element selects by the elements joined. A `#` in a selector body is quoted. A fragment name holding a line break stays inside the trailing block. The input is the output with every line uncommented, and its reads count the instances.

Beyond the fixed corpus, the differential harness (`cicd/utility/crosscheck.bash`) also derives accessor coverage over the fuzz set: the reference's fuzz dump writes a `<name>.reads.tsv` beside each dumped input (paths it knows exist, cycling type and strictness), which the `--extra` replay runs through the same row machinery. Every scalar read row - corpus and fuzz-derived - is additionally replayed under `--on-bad=error` (an exit-code differential) and `--default=<x>` (a stdout differential), so the on-bad/default policy surface is pinned cross-binding too.

Not yet modeled natively (as golden files): the on-bad/default outputs (covered cross-binding via the harness above, not by per-row `expected`). Diagnostic expectations are modeled natively via `expected-diags.txt` (above) and cross-binding via the `load` rows.
