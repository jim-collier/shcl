// SPDX-License-Identifier: MIT
// Copyright © 2026 Jim Collier (CryptogID: ѳ6ᴚ℈𐀘𐇦ɛ𐊁¥Mﾏb϶Δ𐌞)

// SHCL reference implementation for C: parser, accessor, writer/formatter.
// Single-header, drop-in: copy this file into your tree and, in exactly ONE .c,
//   #define SHCL_IMPLEMENTATION
//   #include "shcl.h"
// Everything is byte-for-byte with the Rust reference (source/rust); the cicd
// cross-binding check compares CLI stdout + exit codes across every binding.
// The language spec lives in project/spec.md; project/conformance/ pins behavior.
// Structure deliberately mirrors the reference over C-local shortcuts, so a fix
// there ports here by mechanical diff (parity over idiom - see style-guide.md).
//
// A companion C++ typed veneer (get<int64_t>() etc.) sits in shcl.hpp; it wraps
// this core, it is not a second parser.
//
// Compile-time knobs, each defined before the implementation include:
//   SHCL_NO_FILE_IO  leave the file tier out (no file I/O in the library)
//   SHCL_OOM()       what an allocation failure outside a parse does; the
//                    default prints and exits 70, which suits the CLI and
//                    nothing else. A parse or a validate never reaches it:
//                    those unwind and return NULL.
// A hook that longjmps out arms its recovery point with SHCL_SETJMP, below.

// The file tier calls POSIX (fdopen, fileno, fchmod, open, fsync, getpid). Those
// prototypes are feature-gated, and a feature request only counts before the
// first system header - so it goes here rather than beside the code needing it,
// and a consumer who already asked for a level keeps theirs.
#if !defined(SHCL_NO_FILE_IO) && !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
	#define _POSIX_C_SOURCE 200809L
	#define _XOPEN_SOURCE 700
#endif

#ifndef SHCL_H
#define SHCL_H

#include <stddef.h>
#include <stdint.h>

// Arm a setjmp recovery point that a longjmp from inside this library has to
// reach. mingw's setjmp hands longjmp a target frame, and longjmp then unwinds
// through SEH to get there. gcc's unwind info for a function that has both a
// frame pointer and saved xmm registers puts those save slots at offsets the
// real unwinder resolves past the top of the stack, and the read faults. Wine
// resolves them from a different base and never sees it, which is why the same
// binary passes there. A NULL frame makes longjmp restore the context without
// unwinding at all, and a C recovery point needs nothing more, since nothing in
// between has a destructor or a __finally. Include <setjmp.h> before using it.
// The full shape is in style-guide.md under the C deviations.
#if defined(__MINGW32__) && defined(__x86_64__) && defined(__SEH__)
	#define SHCL_SETJMP(buf) _setjmp((buf), NULL)
#else
	#define SHCL_SETJMP(buf) setjmp(buf)
#endif

#ifdef __cplusplus
extern "C" {
#endif

// A borrowed byte string (length-delimited; text may hold NUL and is UTF-8).
typedef struct { const char *p; size_t n; } shcl_str;

typedef enum { SHCL_LOOSE, SHCL_STANDARD, SHCL_STRICT } shcl_strictness;
typedef enum { SHCL_SEV_ERROR, SHCL_SEV_HINT } shcl_severity;

// Read status sentinels. Empty is informational - the empty value still returns.
typedef enum {
	SHCL_GOOD, SHCL_EMPTY, SHCL_NOT_FOUND, SHCL_BAD_TYPE, SHCL_MULTIPLE
} shcl_status;

// Why a write would fail (shcl_write_reason_()): the distinctions behind a
// setter's bare 0. SHCL_W_WRITABLE = the path passes the writer's validation;
// the rest name the five ways it cannot.
typedef enum {
	SHCL_W_WRITABLE,
	SHCL_W_BAD_PATH,      // empty path, the scanner rejected it, or a segment carries a line break
	SHCL_W_VALUE_IN_PATH, // the path carries a `: value` part; writes take values separately
	SHCL_W_WILDCARD,      // wildcard selectors are query-only
	SHCL_W_NO_SUCH_INDEX, // a `[#k]` instance that does not (and can never) exist
	SHCL_W_TOO_DEEP       // deeper than the nesting cap; the writer never creates past it
} shcl_write_reason;

typedef struct shcl_doc shcl_doc;

// Local (floating) date/time unless a zone suffix was present. has_* fields say
// which parts were written; format via shcl_datetime_str.
typedef enum { SHCL_ZONE_NONE, SHCL_ZONE_UTC, SHCL_ZONE_OFFSET } shcl_zone_kind;
typedef struct {
	int has_date; int32_t year; uint32_t month; uint32_t day;
	int has_time; uint32_t hour; uint32_t minute; int has_sec; uint32_t sec;
	int has_frac; shcl_str frac;      // fractional-second digits, as typed
	shcl_zone_kind zone; int32_t off_min;
} shcl_datetime;

typedef struct { int64_t value;  shcl_status status; } shcl_read_i64;
typedef struct { double  value;  shcl_status status; } shcl_read_f64;
typedef struct { int     value;  shcl_status status; } shcl_read_bool;
typedef struct { shcl_str value; shcl_status status; } shcl_read_str;
typedef struct { shcl_datetime value; shcl_status status; } shcl_read_dt;

// Array results also carry one status per slot (element, or wildcard instance)
// in statuses[0..n); status is then the worst slot. NULL on whole-path errors.
typedef struct { int64_t *values; size_t n; shcl_status status; const shcl_status *statuses; } shcl_read_i64_arr;
typedef struct { double  *values; size_t n; shcl_status status; const shcl_status *statuses; } shcl_read_f64_arr;
typedef struct { int     *values; size_t n; shcl_status status; const shcl_status *statuses; } shcl_read_bool_arr;
typedef struct { shcl_str *values; size_t n; shcl_status status; const shcl_status *statuses; } shcl_read_str_arr;
typedef struct { shcl_datetime *values; size_t n; shcl_status status; const shcl_status *statuses; } shcl_read_dt_arr;

// Maximum nesting depth (levels below the document root), enforced at load and
// by the Writer. Deeper lines are skipped with an E016 error. The cap is what
// keeps the recursive tree walks (emit, merge, clone) safely inside every
// binding's stack, so a hostile or machine-generated document can make a load
// fail but never crash the consumer.
#define SHCL_MAX_DEPTH ((size_t)512)

// A parse never fails on the document's account: bad lines are skipped and
// diagnosed. It returns NULL only when an allocation failed, which is the one
// thing it cannot work around - the process is left standing either way. Text
// need not be NUL terminated. Free with shcl_free.
shcl_doc *shcl_parse(const char *text, size_t len);
shcl_doc *shcl_parse_with(const char *text, size_t len, shcl_strictness s);
// Parse with resource caps beside the strictness: max_nodes stops the parse
// once a line takes the node count past it (one E020 error; the unparsed
// remainder counts as lost), max_elements refuses any line whose array would
// hold more elements (E021, that line alone skipped), max_diags lists only
// that many diagnostics and ends the list with one E022 counting the rest
// (an error whenever an unlisted one was, so the strict gate still fails).
// 0 disables a cap.
shcl_doc *shcl_parse_limited(const char *text, size_t len, shcl_strictness s, size_t max_nodes, size_t max_elements, size_t max_diags);
void shcl_free(shcl_doc *d);

// True when a strict load would fail (strictness==strict and an error diagnostic
// exists). At loose/standard this is always false.
int shcl_strict_failed(const shcl_doc *d);
shcl_strictness shcl_strictness_of(const shcl_doc *d);

// Diagnostics, in emission order (parse-time diagnostics, then repeated-leaf hints).
size_t shcl_diag_count(const shcl_doc *d);
size_t shcl_diag_line(const shcl_doc *d, size_t i);
shcl_severity shcl_diag_severity(const shcl_doc *d, size_t i);
shcl_str shcl_diag_message(const shcl_doc *d, size_t i);
// Stable machine code (E001.., H001..) identifying the diagnostic kind - the
// contract; the message prose is a free, per-binding voice. NUL-terminated.
const char *shcl_diag_code(const shcl_doc *d, size_t i);
// How many lines or values parsing dropped that canonical output cannot
// re-emit - bad indentation, an unusable selector, a line past the depth cap.
// Content-malformed lines do NOT count: those are retained as trivia and
// survive a save. Nonzero means a save would delete hand-written content, so
// shcl_save_file refuses then (shcl_save_file_lossy overrides).
size_t shcl_lost_count(const shcl_doc *d);
// How many error-severity diagnostics the document carries - the "did this
// file have errors?" predicate, so recover-and-continue can't read as success
// by accident. Counts whatever the shcl_diag_* accessors hold (after
// shcl_load_and_validate, that includes validation errors).
size_t shcl_error_count(const shcl_doc *d);

// Schema validation (spec.md "Schema validation"): check d against a schema
// document (itself plain SHCL). Zero diagnostics = the document conforms.
// Diagnostic lines are document lines (0 = document scope); schema faults
// (V09x, schema-file lines) come first, and the surviving constraints still
// check the document. The unknown-field sweep runs too, unless a fault cost
// the schema a path spelling (an unreadable `field:` path, or a mount naming
// no declared fragment) - only those can turn declared fields into false
// unknowns; a key-level fault keeps its entry's chain. The result owns copies
// of all its strings - free with shcl_validation_free.
// The H001/H002 hints a schema disavows are NOT dropped by this call: they live
// on the parse's diagnostics, which validation does not touch. Parse then
// validate and they are still there - call shcl_suppress_declared_repeats /
// shcl_suppress_declared_reopens yourself, or use shcl_load_and_validate,
// which runs both for you.
// NULL only when an allocation failed.
typedef struct shcl_validation shcl_validation;
shcl_validation *shcl_validate(shcl_doc *d, shcl_doc *schema);
size_t shcl_validation_count(const shcl_validation *v);
size_t shcl_validation_line(const shcl_validation *v, size_t i);
shcl_severity shcl_validation_severity(const shcl_validation *v, size_t i);
shcl_str shcl_validation_message(const shcl_validation *v, size_t i);
const char *shcl_validation_code(const shcl_validation *v, size_t i);
void shcl_validation_free(shcl_validation *v);

// Drop the H001 hints a schema disavows: a field whose declared repeat upper
// bound is above 1 repeats BY DESIGN (repetition is its instance mechanism),
// so the repeated-bare-leaf hint is structurally a false positive there and
// trains users to ignore hints. Matching is by leaf name - the filter
// consumers were hand-rolling - which errs toward quiet, for a hint. Used by
// `check --schema` and shcl_load_and_validate; call it wherever doc
// diagnostics and a schema meet. Compacts doc's own diagnostic list in place
// (the C spelling of filtering a caller-held list).
void shcl_suppress_declared_repeats(shcl_doc *schema, shcl_doc *doc);

// Sibling for H002: drop the merge hints a schema disavows via `reopen: true`
// on a section's entry - a section meant to be written in parts. Same leaf-name
// matching and in-place compaction as shcl_suppress_declared_repeats.
void shcl_suppress_declared_reopens(shcl_doc *schema, shcl_doc *doc);

// One-shot load-and-validate: parse at a strictness, validate against a
// schema, and hand back a document whose shcl_diag_* accessors serve ONE
// combined list (parse first, then validation - the order `check --schema`
// prints), so half the errors can't vanish because a caller forgot one of the
// two lists. Fails only on an allocation, and then it is NULL: a strict-failing
// document comes back as the document plus its diagnostics (shcl_error_count
// answers "did it fail"). An
// empty schema text skips validation entirely. H001 hints the schema disavows
// (a declared repeat upper bound above 1) are dropped. Free with shcl_free.
shcl_doc *shcl_load_and_validate(const char *text, size_t len, const char *schema, size_t slen, shcl_strictness s);

// File tier (optional companion; compile out with -DSHCL_NO_FILE_IO to keep
// the core free of file I/O). Paths are UTF-8 on every platform: on windows
// they are widened for the file calls rather than read in the active code
// page, so a program's own main() has to hand over UTF-8 too (the wide
// command line, not the narrow argv). Load does not fail on the file's account: the
// document always comes back usable (empty when the file could not be read),
// and the status out-param (may be NULL) separates the four cases a consumer's
// own load path otherwise confuses. NULL means an allocation failed, as for a
// parse. Save writes the canonical text through a temp file in
// the same directory plus a rename - the same mechanics the CLI's --write
// uses - so an interrupted save can never truncate the config it rewrites.
#ifndef SHCL_NO_FILE_IO
typedef enum {
	SHCL_FILE_CLEAN,      /* read and parsed, no error diagnostics (hints allowed) */
	SHCL_FILE_HAD_ERRORS, /* read and parsed, but error diagnostics are present */
	SHCL_FILE_NOT_FOUND,  /* no file at the path */
	SHCL_FILE_UNREADABLE  /* exists but could not be read (permissions, a directory, bad encoding, past a shcl_read_file cap) */
} shcl_file_status;
/* Save refuses while the load dropped content the write would silently delete
   (shcl_lost_count); shcl_save_file_lossy is the override, and is the only way
   to write then. The two failures are separate values rather than one falsey
   answer because they need different handling: a refusal is the caller's to
   reverse, a write failure is the disk's answer with errno describing it. */
typedef enum {
	SHCL_SAVE_OK,      /* written */
	SHCL_SAVE_REFUSED, /* the lost-content gate fired; lossy overrides */
	SHCL_SAVE_FAILED   /* the write itself failed; errno describes it */
} shcl_save_result;
// Textual name of a file status, the shcl_status_name of this enum, so
// logging one reads as a case rather than a number. NUL-terminated, static.
const char *shcl_file_status_name(shcl_file_status s);
shcl_doc *shcl_load_file(const char *path, shcl_file_status *status);
shcl_doc *shcl_load_file_with(const char *path, shcl_strictness s, shcl_file_status *status);
// The read half on its own: the file's text, malloc'd and NUL-terminated (the
// caller frees it), with *len set; or NULL with the status saying why. A file
// past max_bytes is unreadable; 0 is no cap.
char *shcl_read_file(const char *path, size_t max_bytes, size_t *len, shcl_file_status *status);
shcl_save_result shcl_save_file(shcl_doc *d, const char *path);
shcl_save_result shcl_save_file_lossy(shcl_doc *d, const char *path);
int shcl_write_file_atomic(const char *path, const char *data, size_t n);
#endif

// Schema-driven generation (`shcl init --schema`): a commented, typed starter
// config from a schema document. Required paths are live (their `default`, or an
// empty value); optional paths are commented out; wildcard paths are listed in a
// trailing comment block. The output validates clean against the schema that
// produced it, checked against the finished text, so a schema whose own
// `default` breaks its field's constraints is a fault (V097) instead of a
// starter config that fails the first time it is checked; the faults land on
// the schema document's diagnostics. A footer naming the format and pointing at the spec is
// written last unless no_banner; the flag is negative so passing 0 writes the
// footer. *ok is set to 1 on success, 0 if the schema has faults (V09x) - then
// the returned string is empty and nothing was kept. Bytes live in the schema's
// read arena; valid until shcl_free, or until shcl_reads_release. Generation
// faults from an earlier call on the same schema are dropped first, so the list
// describes this call.
shcl_str shcl_generate(shcl_doc *schema, int no_banner, int *ok);

// Canonical form (block layout, tabs, insertion order, minimal quoting). The
// returned bytes live in the document's read arena; valid until shcl_free, or
// until shcl_reads_release. The
// bytes may contain NUL - never hand them to a strlen-based API.
shcl_str shcl_to_canonical(shcl_doc *d);

size_t shcl_count(shcl_doc *d, const char *path, size_t plen);
// Instance display values, in file order. Writes an arena-owned array to *out.
size_t shcl_instances(shcl_doc *d, const char *path, size_t plen, shcl_str **out);
// 1-based source line of the binding at a path, for consumer checks the
// schema cannot express. 0 when the path does not resolve to exactly one
// node, or the node was writer-built. Merged instances cite the first
// binding's line, matching diagnostics.
size_t shcl_line(shcl_doc *d, const char *path, size_t plen);
// 1 when the single scalar value at a path was quoted in the source, so a
// consumer can tell a quoted plain string from a bare word that happens to
// spell a reserved one - `mode: "on"` against `mode: on`. 0 for anything that
// is not one scalar element (empty, a raw block, an array, an unresolved or
// ambiguous path). Sits beside shcl_line rather than in the read structs for
// the same reason the raw text does: C keeps those two fields wide.
int shcl_quoted(shcl_doc *d, const char *path, size_t plen);

// The field name at a path exactly as the author spelled it (case unfolded,
// outer quotes stripped), so a message can echo SYMBOLS when the file said
// SYMBOLS. Escape sequences stay as written too: a name is stored, compared
// and emitted with its escapes RESOLVED, so this is the one call that hands the
// source spelling back - which is what an as-authored accessor is for.
// Resolution mirrors shcl_line: empty when the path does not resolve to
// exactly one node. Merged instances keep the first binding's spelling; a
// writer-built node keeps the spelling the setter's path used.
// Borrowed from the document's own arena, so it outlives shcl_reads_release and
// is valid until shcl_free or shcl_compact - the name is stored, not built.
shcl_str shcl_authored_name(shcl_doc *d, const char *path, size_t plen);
// The plural shcl_line: 1-based source lines at a path, in file order, so a
// repeated field - the case that most wants a citable line - yields every
// binding's. Wildcard slots that did not resolve stay in the list as 0, and a
// writer-built node is 0, so indices keep matching shcl_count. Writes an
// arena-owned array to *out.
size_t shcl_lines(shcl_doc *d, const char *path, size_t plen, size_t **out);
// Child field names under a path, in file order, duplicates included - the
// "what keys are in this section?" question shcl_paths (deduplicated,
// path-shaped) cannot answer. An empty or whitespace-only path enumerates the
// top level. Names come back as stored; shcl_quote_segment makes one
// splice-safe in a path. Writes an arena-owned array to *out.
size_t shcl_children(shcl_doc *d, const char *path, size_t plen, shcl_str **out);
// Every field path in the document, in file order, deduplicated - a query
// recipe for tooling. A segment that is not bare-name-safe is emitted quoted
// and escaped - the form the path scanner accepts - so each path is a
// well-formed lookup path and nothing in the document is hidden. Returns the
// count; *out stays valid until shcl_free, or until shcl_reads_release.
size_t shcl_paths(shcl_doc *d, shcl_str **out);
// Quote one path segment so it can be spliced into a lookup path: a bare name
// passes through, anything else comes back quoted and escaped in the form the
// path scanner accepts. Splicing user-typed text into a path without this is
// path injection - a dotted name silently reads as nesting. Same spelling
// shcl_paths and the canonical emitter produce. Result lives in the
// document's read arena; valid until shcl_free, or until shcl_reads_release.
shcl_str shcl_quote_segment(shcl_doc *d, const char *name, size_t len);

shcl_read_i64  shcl_read_int(shcl_doc *d, const char *path, size_t plen);
shcl_read_f64  shcl_read_float(shcl_doc *d, const char *path, size_t plen);
shcl_read_bool shcl_read_bool_(shcl_doc *d, const char *path, size_t plen);
shcl_read_dt   shcl_read_datetime(shcl_doc *d, const char *path, size_t plen);
shcl_read_str  shcl_read_string(shcl_doc *d, const char *path, size_t plen);
shcl_read_str  shcl_read_raw(shcl_doc *d, const char *path, size_t plen);
shcl_read_str  shcl_read_raw_info(shcl_doc *d, const char *path, size_t plen);

shcl_read_i64_arr  shcl_read_int_array(shcl_doc *d, const char *path, size_t plen);
shcl_read_f64_arr  shcl_read_float_array(shcl_doc *d, const char *path, size_t plen);
shcl_read_bool_arr shcl_read_bool_array(shcl_doc *d, const char *path, size_t plen);
shcl_read_dt_arr   shcl_read_datetime_array(shcl_doc *d, const char *path, size_t plen);
shcl_read_str_arr  shcl_read_string_array(shcl_doc *d, const char *path, size_t plen);

// Give back everything the read calls have handed out. Every result from a read
// - shcl_read_*, shcl_children, shcl_paths, shcl_instances, shcl_lines,
// shcl_quote_segment, shcl_to_canonical, shcl_generate - is invalid after this; the document itself is untouched
// and stays readable, so the next read works normally. Optional: leave it alone
// and results live until shcl_free, which is the documented contract and what a
// read-once consumer wants. A process polling the same document in a loop calls
// it between passes so the memory does not climb.
void shcl_reads_release(shcl_doc *d);
// The write-side counterpart. A write lands in a bump arena and the value it
// replaced stays there until shcl_free, so a process rewriting one field in a
// loop grows by a few dozen bytes per write. Compaction rebuilds the document
// into fresh arenas holding only what it now contains - same content, same
// diagnostics, same lost count, same strictness - and gives the old ones back.
// Every read result is invalid after it, as after shcl_reads_release. Optional,
// for a long-running writer; a write-once consumer never needs it. On an
// allocation failure the document is left as it was.
void shcl_compact(shcl_doc *d);

// Convenience tier: the value, or the call-site fallback unless the read is Good
// - so a missing/empty/bad/ambiguous read cannot masquerade as a real zero. The
// string/datetime/raw and array reads keep the shcl_read_* status tier above.
int64_t shcl_get_int(shcl_doc *d, const char *path, size_t plen, int64_t def);
double  shcl_get_float(shcl_doc *d, const char *path, size_t plen, double def);
int     shcl_get_bool(shcl_doc *d, const char *path, size_t plen, int def);
// The same three under the cross-binding spelling: `_or` means "with a
// fallback" in every binding, so a routine ported between two of them cannot
// keep the call name while changing which tier it lands on.
int64_t shcl_get_int_or(shcl_doc *d, const char *path, size_t plen, int64_t def);
double  shcl_get_float_or(shcl_doc *d, const char *path, size_t plen, double def);
int     shcl_get_bool_or(shcl_doc *d, const char *path, size_t plen, int def);

// --- Writer: typed emit, defaults, comments, structural edits ---------------
// The reverse of the reads. Each setter builds the canonical stored text for a
// typed value and places it at a path (creating intermediate nodes). New values
// are copied into the arena, so the caller's buffers need not outlive the call.
// Setters return 1 when the write applied, 0 when the path is unusable
// (wildcard, missing [#N] instance, a value part, or past the depth cap) or
// the value has no spelling the reader accepts (a non-finite float, a datetime
// the reader would refuse, a raw info-string holding a `#`) - nothing is
// created on failure. _default forms return 1 when already present.
// Worth checking rather than assuming: an ignored 0 means the save that follows
// writes a document missing the edit, and reports success doing it.
shcl_doc *shcl_new(void); // an empty document (start point for generation), or NULL on an allocation failure
int shcl_exists(shcl_doc *d, const char *path, size_t plen);       // 0/1
size_t shcl_remove(shcl_doc *d, const char *path, size_t plen);    // count deleted
int shcl_set_comment(shcl_doc *d, const char *path, size_t plen, const char *text, size_t tlen);
int shcl_set_empty(shcl_doc *d, const char *path, size_t plen);
// Why a write at this path would fail - the reason behind a setter's bare 0,
// so a consumer's error message need not guess. SHCL_W_WRITABLE means the same
// validation the setters run would pass. Probes only; never creates.
shcl_write_reason shcl_write_reason_(shcl_doc *d, const char *path, size_t plen);

int shcl_set_int(shcl_doc *d, const char *path, size_t plen, int64_t v);
int shcl_set_float(shcl_doc *d, const char *path, size_t plen, double v);
int shcl_set_bool(shcl_doc *d, const char *path, size_t plen, int v);
int shcl_set_string(shcl_doc *d, const char *path, size_t plen, const char *s, size_t slen);
int shcl_set_datetime(shcl_doc *d, const char *path, size_t plen, const shcl_datetime *dt);
int shcl_set_raw(shcl_doc *d, const char *path, size_t plen, const char *content, size_t clen, const char *info, size_t ilen);

int shcl_set_int_array(shcl_doc *d, const char *path, size_t plen, const int64_t *v, size_t n);
int shcl_set_float_array(shcl_doc *d, const char *path, size_t plen, const double *v, size_t n);
int shcl_set_bool_array(shcl_doc *d, const char *path, size_t plen, const int *v, size_t n);
int shcl_set_string_array(shcl_doc *d, const char *path, size_t plen, const char *const *v, const size_t *lens, size_t n);
int shcl_set_datetime_array(shcl_doc *d, const char *path, size_t plen, const shcl_datetime *v, size_t n);

// Binds text as value syntax rather than as data: "80, 443" becomes a
// two-element array where shcl_set_string would store one string that has to be
// quoted. For a caller holding value text - a config line, a user's --set
// argument - that has to be written without knowing its shape first. Returns 0
// for text that could not be one line's value (a line break, or a quote that
// never closes); an unquoted # ends the value as it would in a file.
int shcl_set_literal(shcl_doc *d, const char *path, size_t plen, const char *text, size_t tlen);

// Default (only-if-absent) forms - the "emit defaults" half of the Writer.
int shcl_set_int_default(shcl_doc *d, const char *path, size_t plen, int64_t v);
int shcl_set_float_default(shcl_doc *d, const char *path, size_t plen, double v);
int shcl_set_bool_default(shcl_doc *d, const char *path, size_t plen, int v);
int shcl_set_string_default(shcl_doc *d, const char *path, size_t plen, const char *s, size_t slen);
int shcl_set_datetime_default(shcl_doc *d, const char *path, size_t plen, const shcl_datetime *dt);
int shcl_set_literal_default(shcl_doc *d, const char *path, size_t plen, const char *text, size_t tlen);
int shcl_set_raw_default(shcl_doc *d, const char *path, size_t plen, const char *content, size_t clen, const char *info, size_t ilen);
int shcl_set_int_array_default(shcl_doc *d, const char *path, size_t plen, const int64_t *v, size_t n);
int shcl_set_float_array_default(shcl_doc *d, const char *path, size_t plen, const double *v, size_t n);
int shcl_set_bool_array_default(shcl_doc *d, const char *path, size_t plen, const int *v, size_t n);
int shcl_set_string_array_default(shcl_doc *d, const char *path, size_t plen, const char *const *v, const size_t *lens, size_t n);
int shcl_set_datetime_array_default(shcl_doc *d, const char *path, size_t plen, const shcl_datetime *v, size_t n);

// --- Layered loading --------------------------------------------------------
// Overlay `over` (a higher-priority layer) onto `d` (the lower one). Container
// instances merge by (name, value) like the in-file rule; a leaf name present
// in `over` replaces d's same-named children at that scope - provided those
// base children are leaves too (real override for scalars, arrays, raw blocks;
// a bare section header merges instead of wiping); over-only nodes are
// appended. `over`'s content is deep-copied into d's arena, so d stays valid
// after `over` is freed.
//
// The fold is not associative: (A+B)+C and A+(B+C) differ where a bare header
// meets an overridden leaf, so a cached upper pair is not the same document the
// CLI's left fold produces. d keeps its own strictness, so a value from a
// stricter layer reads with d's coercion. And a replaced node is kept until
// shcl_free or shcl_compact: this costs a pass over the touched scopes plus an
// index rebuild on the next read.
void shcl_merge(shcl_doc *d, const shcl_doc *over);

// CLI/aliases: 1|2|3 or loose|standard|strict. Returns 1 on success.
int shcl_strictness_from_arg(const char *s, size_t n, shcl_strictness *out);

// Format helpers matching the reference's textual output.
// out must be at least SHCL_F64_BUF bytes; returns the byte length written.
#define SHCL_F64_BUF 512
size_t shcl_format_f64(double v, char *out);
// Renders a datetime into out (>= SHCL_DT_BUF bytes); returns byte length. A
// frac longer than 30 bytes is truncated, and the whole rendering is clamped to
// SHCL_DT_BUF bytes, so a hand-built value cannot overrun the documented buffer
// (parsed input never gets near either limit).
#define SHCL_DT_BUF 64
size_t shcl_datetime_str(const shcl_datetime *dt, char *out);
// Status <-> the CLI exit code / textual name.
int shcl_status_code(shcl_status s);
const char *shcl_status_name(shcl_status s);
// Whether the author addressed the field at all: Good or Empty. A status
// predicate rather than a per-struct helper, since every read struct carries
// the same status. Note this deliberately answers differently from the
// convenience tier, which falls back on Empty like any other non-Good read -
// this asks "is this field spoken for", shcl_get_int_or asks "do I have a
// usable value", and an explicitly emptied field is where the two diverge.
int shcl_status_ok(shcl_status s);

#ifdef __cplusplus
}
#endif

// ===========================================================================
#ifdef SHCL_IMPLEMENTATION

#ifdef __cplusplus
// The implementation zero-initializes aggregates with the C idiom `{0}`; C++
// -Wextra flags every one as a missing-field-initializer, which would break a
// consumer compiling this header into a C++ TU with -Werror. Scoped to the
// implementation only.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <inttypes.h>
#include <setjmp.h>
#include <locale.h>

// SHCL spells a float with '.', but strtod and printf use whatever the host
// locale calls the decimal point - so in a consumer that has called setlocale
// both directions of float conversion need translating. The CLI never sets a
// locale, which is why only library callers ever saw this.
static const char *dec_point(void) {
	const char *p = localeconv()->decimal_point;
	return (p && *p) ? p : ".";
}

// --- arena (bump allocator; growable vectors grow by copy, bulk-freed) -------

// Where an allocation failure goes when there is no call to fail. A parse and a
// validate arm a recovery point first, so those unwind and hand the caller NULL
// instead; what is left is a read, a write or a merge on a document already
// built, and there the default is the CLI's contract (exit 70) - wrong for a
// process that is not the library's to end. An embedder defines SHCL_OOM before
// the implementation to longjmp out, log, or abort on its own terms. Nothing is
// unwound first, so a hook that returns leaks whatever was being built - and
// then aborts, because the allocation it was called for still failed. A hook
// that longjmps arms its recovery point with SHCL_SETJMP, for the reason given
// there: the unwind it starts crosses this library's frames, not just its own.
#ifndef SHCL_OOM
	#define SHCL_OOM() do { fprintf(stderr, "shcl: out of memory\n"); exit(70); } while (0)
#endif

/* Unwind to the recovery point `panic` names, or fall back to the macro when
   nothing armed one. Keeping on with a failed allocation is not an option: a
   bump arena holds its vectors' bookkeeping, so a request served out of
   nowhere hands back node indices that are no longer node indices. */
static void arena_panic(jmp_buf *panic) {
	if (panic) longjmp(*panic, 1);
	SHCL_OOM();
	/* A hook that returns leaves nothing to carry on with - the allocation
	   still failed - so it stops here rather than reading the null pointer one
	   line later, which is what used to happen. */
	abort();
}

typedef struct ShclBlock { struct ShclBlock *next; size_t used, cap; } ShclBlock;
/* last/last_n: the most recent allocation, so a vector or string builder that
   grows with nothing allocated after it extends in place. A bump arena cannot
   free, so without this every doubling abandons the copy before it - measured
   at two thirds of a large parse's memory. `last` always points into `head`.
   panic: where an allocation failure unwinds to; NULL means SHCL_OOM. */
typedef struct { ShclBlock *head; void *last; size_t last_n; int growing; jmp_buf *panic; } ShclArena;

static void *arena_alloc(ShclArena *a, size_t n) {
	n = (n + 15u) & ~(size_t)15u;
	if (n == 0) n = 16;
	if (!a->head || a->head->used + n > a->head->cap) {
		/* A block opened for a vector or builder that just doubled gets room to
		   double once more, so the next growth extends in place instead of
		   abandoning this copy. Without it a buffer that reaches N bytes has
		   spent about 2N getting there, and none of it is reclaimable. */
		size_t cap = n > (size_t)65536 ? (a->growing ? n * 2 : n) : (size_t)65536;
		ShclBlock *b = (ShclBlock *)malloc(sizeof(ShclBlock) + cap);
		if (!b) arena_panic(a->panic);
		b->next = a->head; b->used = 0; b->cap = cap; a->head = b;
	}
	void *p = (char *)(a->head + 1) + a->head->used;
	a->head->used += n;
	a->last = p; a->last_n = n;
	return p;
}
static void arena_free(ShclArena *a) {
	ShclBlock *b = a->head;
	while (b) { ShclBlock *n = b->next; free(b); b = n; }
	a->head = NULL; a->last = NULL; a->last_n = 0; a->growing = 0;
}
/* Hand an arena the recovery point its owner armed. */
static void arena_guard(ShclArena *a, jmp_buf *panic) { a->panic = panic; }
static void *arena_grow(ShclArena *a, void *old, size_t oldcap, size_t newcap, size_t sz) {
	/* Extend in place when nothing has been allocated since `old`. */
	if (old && a->head && old == a->last) {
		size_t want = (newcap * sz + 15u) & ~(size_t)15u;
		if (want <= a->last_n) return old;
		size_t extra = want - a->last_n;
		if (a->head->used + extra <= a->head->cap) {
			a->head->used += extra; a->last_n = want;
			return old;
		}
	}
	a->growing = 1;
	void *p = arena_alloc(a, newcap * sz);
	a->growing = 0;
	if (old && oldcap) memcpy(p, old, oldcap * sz);
	return p;
}
// Reset a scratch arena, keeping its newest (largest) block so steady-state
// reads never re-malloc. Bump arenas cannot free per-object, so without this
// every resolver temporary would live until shcl_free - a long-running process
// doing reads would grow without bound.
/* A point to roll back to. A bump arena cannot free one allocation, but it can
   give back everything since a mark, which is what a setter needs when the
   value it just encoded turns out to be refused. Only sound when nothing
   allocated after the mark is still referenced. */
typedef struct { ShclBlock *head; size_t used; void *last; size_t last_n; } ShclMark;

static ShclMark arena_mark(ShclArena *a) {
	ShclMark m; m.head = a->head; m.used = a->head ? a->head->used : 0;
	m.last = a->last; m.last_n = a->last_n;
	return m;
}

static void arena_release(ShclArena *a, ShclMark m) {
	while (a->head && a->head != m.head) { ShclBlock *n = a->head->next; free(a->head); a->head = n; }
	if (a->head) a->head->used = m.used;
	a->last = m.last; a->last_n = m.last_n;
}

static void arena_reset(ShclArena *a) {
	if (!a->head) return;
	ShclBlock *b = a->head->next;
	while (b) { ShclBlock *n = b->next; free(b); b = n; }
	a->head->next = NULL; a->head->used = 0;
	a->last = NULL; a->last_n = 0;
}
/* Like arena_reset, but the block kept is the largest rather than the newest.
   The name index's chain array is one block sized by the node arena, and a
   consumer merging once a second rebuilds it every time; freed, windows hands
   the block back to the system and faults it in again on the next build, a
   quarter of a millisecond per merge there. Kept, the rebuild reuses it. */
static void arena_reset_largest(ShclArena *a) {
	ShclBlock *best = NULL, *b;
	for (b = a->head; b; b = b->next) if (!best || b->cap > best->cap) best = b;
	for (b = a->head; b;) { ShclBlock *n = b->next; if (b != best) free(b); b = n; }
	a->head = best;
	if (best) { best->next = NULL; best->used = 0; }
	a->last = NULL; a->last_n = 0; a->growing = 0;
}

#define DEFINE_VEC(Name, T) \
	typedef struct { T *data; size_t len, cap; } Name; \
	static void Name##_push(ShclArena *a, Name *v, T x) { \
		if (v->len == v->cap) { size_t nc = v->cap ? v->cap * 2 : 8; \
			v->data = (T *)arena_grow(a, v->data, v->cap, nc, sizeof(T)); v->cap = nc; } \
		v->data[v->len++] = x; }

// --- byte-string helpers -----------------------------------------------------

typedef shcl_str ShclStr;
static ShclStr s_lit(const char *z) { ShclStr s; s.p = z; s.n = strlen(z); return s; }
static ShclStr s_empty(void) { ShclStr s; s.p = ""; s.n = 0; return s; }
static int s_eq(ShclStr a, ShclStr b) { return a.n == b.n && (a.n == 0 || memcmp(a.p, b.p, a.n) == 0); }
static int s_has_nl(ShclStr s) { for (size_t i = 0; i < s.n; i++) if (s.p[i] == '\n') return 1; return 0; }
static ShclStr s_dup(ShclArena *a, ShclStr x) {
	if (x.n == 0) return s_empty();
	char *m = (char *)arena_alloc(a, x.n); memcpy(m, x.p, x.n);
	ShclStr r; r.p = m; r.n = x.n; return r;
}
/* Keep s as-is when it already slices the retained input copy (src), else dup
   it into the arena. The parse dups the whole input once and stores slices of
   that copy; this is the store-site gate that makes mixed provenance safe. */
static ShclStr s_keep(ShclArena *a, ShclStr src, ShclStr s) {
	if (s.n && (uintptr_t)s.p >= (uintptr_t)src.p && (uintptr_t)s.p + s.n <= (uintptr_t)src.p + src.n) return s;
	return s_dup(a, s);
}
static ShclStr s_slice(ShclStr s, size_t from, size_t to) { ShclStr r; r.p = s.p + from; r.n = to - from; return r; }
static int s_starts(ShclStr s, const char *pre) {
	size_t n = strlen(pre); return s.n >= n && memcmp(s.p, pre, n) == 0;
}

typedef struct { char *data; size_t len, cap; } ShclSB;
static void sb_put(ShclArena *a, ShclSB *s, const char *p, size_t n) {
	if (!n) return;
	if (s->len + n > s->cap) { size_t nc = s->cap ? s->cap * 2 : 32;
		while (nc < s->len + n) nc *= 2;
		s->data = (char *)arena_grow(a, s->data, s->cap, nc, 1); s->cap = nc; }
	memcpy(s->data + s->len, p, n); s->len += n;
}
/* Open the builder at a size the caller already knows. A bump arena abandons
   every step of a doubling climb, so a 20 MB value built from 32 bytes cost
   about four times its own size. */
static void sb_reserve(ShclArena *a, ShclSB *s, size_t n) {
	if (n <= s->cap) return;
	s->data = (char *)arena_grow(a, s->data, s->cap, n, 1); s->cap = n;
}
static void sb_putc(ShclArena *a, ShclSB *s, char c) { sb_put(a, s, &c, 1); }
static void sb_puts(ShclArena *a, ShclSB *s, const char *z) { sb_put(a, s, z, strlen(z)); }
static void sb_putS(ShclArena *a, ShclSB *s, ShclStr x) { sb_put(a, s, x.p, x.n); }
static ShclStr sb_S(ShclSB *s) { ShclStr r; r.p = s->data ? s->data : ""; r.n = s->len; return r; }

// --- UTF-8 (input is validated at the CLI; here we assume it is well formed) --

static size_t utf8_decode(const char *p, size_t n, size_t i, uint32_t *cp) {
	unsigned char c = (unsigned char)p[i];
	if (c < 0x80) { *cp = c; return 1; }
	if ((c >> 5) == 0x6 && i + 1 < n) {
		*cp = ((uint32_t)(c & 0x1F) << 6) | (p[i + 1] & 0x3F); return 2;
	}
	if ((c >> 4) == 0xE && i + 2 < n) {
		*cp = ((uint32_t)(c & 0x0F) << 12) | ((uint32_t)(p[i + 1] & 0x3F) << 6) | (p[i + 2] & 0x3F); return 3;
	}
	if ((c >> 3) == 0x1E && i + 3 < n) {
		*cp = ((uint32_t)(c & 0x07) << 18) | ((uint32_t)(p[i + 1] & 0x3F) << 12)
			| ((uint32_t)(p[i + 2] & 0x3F) << 6) | (p[i + 3] & 0x3F); return 4;
	}
	*cp = c; return 1;
}
static size_t utf8_encode(uint32_t cp, char out[4]) {
	if (cp < 0x80) { out[0] = (char)cp; return 1; }
	if (cp < 0x800) { out[0] = (char)(0xC0 | (cp >> 6)); out[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
	if (cp < 0x10000) {
		out[0] = (char)(0xE0 | (cp >> 12)); out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
		out[2] = (char)(0x80 | (cp & 0x3F)); return 3;
	}
	out[0] = (char)(0xF0 | (cp >> 18)); out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
	out[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[3] = (char)(0x80 | (cp & 0x3F)); return 4;
}
// Byte length of the last codepoint in s (0 if empty); *cp gets its value.
static size_t utf8_last(ShclStr s, uint32_t *cp) {
	if (s.n == 0) { *cp = 0; return 0; }
	size_t i = s.n - 1;
	while (i > 0 && ((unsigned char)s.p[i] & 0xC0) == 0x80) i--;
	utf8_decode(s.p, s.n, i, cp);
	return s.n - i;
}
static void sb_put_cp(ShclArena *a, ShclSB *s, uint32_t cp) {
	char buf[4]; size_t l = utf8_encode(cp, buf); sb_put(a, s, buf, l);
}

// Decode s into a codepoint array with byte offsets (off has n+1 entries).
typedef struct { uint32_t *cp; size_t *off; size_t n; } ShclCPs;
static ShclCPs decode_cps(ShclArena *a, ShclStr s) {
	size_t m = 0;
	for (size_t i = 0; i < s.n;) { uint32_t c; i += utf8_decode(s.p, s.n, i, &c); m++; }
	ShclCPs r; r.n = m;
	r.cp = (uint32_t *)arena_alloc(a, (m ? m : 1) * sizeof(uint32_t));
	r.off = (size_t *)arena_alloc(a, (m + 1) * sizeof(size_t));
	size_t i = 0, k = 0;
	while (i < s.n) { uint32_t c; size_t l = utf8_decode(s.p, s.n, i, &c); r.cp[k] = c; r.off[k] = i; i += l; k++; }
	r.off[m] = s.n;
	return r;
}

// --- whitespace (Rust char::is_whitespace / Unicode White_Space) + ascii ------

static int is_ws(uint32_t c) {
	switch (c) {
	case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x20:
	case 0x85: case 0xA0: case 0x1680:
	case 0x2000: case 0x2001: case 0x2002: case 0x2003: case 0x2004:
	case 0x2005: case 0x2006: case 0x2007: case 0x2008: case 0x2009:
	case 0x200A: case 0x2028: case 0x2029: case 0x202F: case 0x205F: case 0x3000:
		return 1;
	default: return 0;
	}
}
static ShclStr trim_start(ShclStr s) {
	size_t i = 0;
	while (i < s.n) { uint32_t c; size_t l = utf8_decode(s.p, s.n, i, &c); if (!is_ws(c)) break; i += l; }
	return s_slice(s, i, s.n);
}
static ShclStr trim_end(ShclStr s) {
	while (s.n) { uint32_t c; size_t l = utf8_last(s, &c); if (!is_ws(c)) break; s.n -= l; }
	return s;
}
static ShclStr s_trim(ShclStr s) { return trim_end(trim_start(s)); }

/* The grammar's wsp: a space or a tab. The parser trims with this and nothing
   wider - a no-break space or a line separator after a value is content, and
   a Unicode trim used to delete it with no diagnostic. */
static int is_wsp(uint32_t c) { return c == ' ' || c == '\t'; }
static ShclStr trim_wsp_start(ShclStr s) {
	size_t i = 0;
	while (i < s.n && is_wsp((unsigned char)s.p[i])) i++;
	return s_slice(s, i, s.n);
}
/* The end of a line, or of a line's content before its comment: wsp, plus a
   carriage return, which the load takes off a line end anyway - so a retained
   line or a comment written back never ends in one the next load would strip.
   A CR followed by content stays content. */
static ShclStr trim_wsp_end(ShclStr s) {
	while (s.n && (is_wsp((unsigned char)s.p[s.n - 1]) || s.p[s.n - 1] == '\r')) s.n--;
	return s;
}
/* Both ends, wsp only: a value or element keeps a carriage return, which is
   content anywhere but at a line end. */
static ShclStr s_trim_wsp(ShclStr s) {
	s = trim_wsp_start(s);
	while (s.n && is_wsp((unsigned char)s.p[s.n - 1])) s.n--;
	return s;
}

static int is_adigit(uint32_t c) { return c >= '0' && c <= '9'; }
static int is_ahex(uint32_t c) { return is_adigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
static int is_aalnum(uint32_t c) {
	return is_adigit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
static int is_bare_name_char(uint32_t c) {
	return (c < 128 && is_aalnum(c)) || c == '-' || c == '_';
}
static int all_adigit0(ShclStr s) { for (size_t i = 0; i < s.n; i++) if (!is_adigit((unsigned char)s.p[i])) return 0; return 1; }
static int all_ahex(ShclStr s) { for (size_t i = 0; i < s.n; i++) if (!is_ahex((unsigned char)s.p[i])) return 0; return s.n > 0; }
static ShclStr ascii_lower(ShclArena *a, ShclStr s) {
	char *m = (char *)arena_alloc(a, s.n ? s.n : 1);
	for (size_t i = 0; i < s.n; i++) { unsigned char c = (unsigned char)s.p[i]; m[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : (char)c; }
	ShclStr r; r.p = m; r.n = s.n; return r;
}
static ShclStr fold_name(ShclArena *a, ShclStr s) { return ascii_lower(a, s); }
/* True when folding and escape resolution cannot change a name's spelling:
   all ASCII (the permissive decoder normalizes ill-formed bytes, so only
   ASCII is guaranteed identity), no A-Z (fold identity), no backslash
   (escape identity). Then the stored name can be the source slice itself. */
static int name_plain(ShclStr s) {
	for (size_t i = 0; i < s.n; i++) {
		unsigned char c = (unsigned char)s.p[i];
		if (c >= 0x80 || (c >= 'A' && c <= 'Z') || c == '\\') return 0;
	}
	return 1;
}

// --- in-memory model ---------------------------------------------------------

typedef struct { ShclStr text; int quoted; } ShclElement;
DEFINE_VEC(ShclVecEl, ShclElement)

typedef enum { V_EMPTY, V_CELL, V_RAW } ShclVKind;
typedef struct { ShclStr content; ShclStr info; unsigned char fence_char; size_t fence_len; } ShclRawVal;
typedef struct {
	ShclVKind kind;
	ShclElement *els; size_t nels;                 // V_CELL
	size_t cap_els;                            // stacked-list growth only (0 elsewhere)
	ShclRawVal *raw;                               // V_RAW only, else NULL: inline, the four fields sat in every node
} ShclValue;

DEFINE_VEC(ShclVecSize, size_t)
DEFINE_VEC(ShclVecS, ShclStr)

/* One whole-line comment held as trivia, plus whether a blank line preceded
   it - so a blank between comment-only regions survives the round-trip
   (blank runs collapse to one, same as nodes). */
typedef struct { ShclStr text; int blank_before; } ShclLead;
DEFINE_VEC(ShclVecLead, ShclLead)
static ShclLead lead_make(ShclStr text, int blank_before) { ShclLead l; l.text = text; l.blank_before = blank_before; return l; }
static ShclLead lead_plain(ShclStr text) { return lead_make(text, 0); }

/* Comment trivia, verbatim from `#` to end of line. Never part of identity
   or reads; merged instances concatenate leading, first trailing wins
   (later ones demote to leading - a canonical line has room for one). */
typedef struct {
	ShclVecLead leading;
	ShclStr trailing; /* n == 0 = none */
	/* Whole-line comments that followed this node's subtree at a deeper indent
	   than the next binding - they belong to this block, not the next node, so
	   a run trailing a block's last child stays put instead of re-attaching
	   dedented. Emitted after the subtree at this node's depth. */
	ShclVecLead after;
	/* Whole-line comments written inside this node's block when no bound child
	   could take them - a header whose children are all commented still owns
	   those lines. Emitted after the subtree one level deeper than this node. */
	ShclVecLead inside;
} ShclTrivia;

typedef struct {
	ShclStr name;
	/* The name as the author spelled it (case unfolded, quotes and escapes
	   resolved) - what shcl_authored_name hands back, via node_authored.
	   Merged instances keep the first binding's spelling, like shcl_line and
	   comments. Empty = spelled exactly like `name` (the overwhelmingly
	   common case). */
	ShclStr name_src;
	ShclValue value;
	ShclVecSize children;
	size_t parent;
	size_t line;
	/* Comment trivia, hung off to the side: most nodes carry none, and the
	   four empty containers were a third of every node. NULL = none. */
	ShclTrivia *trivia;
	int star_list;  /* value built from stacked "* " lines */
	int star_mixed; /* mix of "* " and field children already diagnosed */
	/* Blank-line grouping is the other half of hand-authored layout: set when
	   a blank line preceded this node's binding line (runs collapse to one). */
	int blank_before;
} ShclNode;
typedef struct { ShclNode *data; size_t len, cap; } ShclVecNode;

typedef struct { size_t line; shcl_severity sev; ShclStr message; const char *code; } ShclDiag;
DEFINE_VEC(ShclVecDiag, ShclDiag)

/* Hash-keyed child map (the parser's accelerator and the read index); the
   operations sit with the parser below. */
typedef struct ShclCMapEnt { struct ShclCMapEnt *next; uint64_t hash; size_t val; } ShclCMapEnt;
typedef struct { ShclCMapEnt **buckets; size_t cap, len; } ShclCMap;

struct shcl_doc {
	ShclArena arena;
	// Per-resolve temporaries (path scans, resolver vectors, display strings
	// built only to compare). Reset on entry to each resolve, so read-only use
	// of a long-lived document stays flat; anything HANDED BACK to the caller
	// lives in `reads` (valid until shcl_free or shcl_reads_release).
	ShclArena scratch;
	// Results handed back by the read calls. Its own arena so a long-running
	// consumer polling one document can give them back with shcl_reads_release
	// instead of growing until shcl_free. Freed with the document either way,
	// so a caller that never calls it sees the documented lifetime unchanged.
	ShclArena reads;
	ShclVecNode nodes;
	/* Armed for the length of a parse and cleared before it returns: the two
	   vectors above are malloc storage, so they cannot read it off an arena. */
	jmp_buf *panic;
	ShclVecDiag diags;
	shcl_strictness strictness;
	ShclVecLead orphans; /* top-level comments after the last binding line */
	/* Lines or values parsing dropped that canonical output cannot re-emit
	   (bad indentation, an unusable selector, past the depth cap, ...).
	   Content-malformed lines are NOT counted - they are retained as trivia
	   and survive a save. shcl_lost_count serves it; shcl_save_file gates on
	   it. */
	size_t lost;
	/* Read accelerator: the first child of each (parent, name), chained on to
	   the next same-named sibling, plus the chain tail so an append is O(1).
	   A hash collision chains a stranger in; the lookup checks the name, so
	   the chain is only ever a superset. Built on the first path lookup and
	   kept current by the writer (a new child appends, a removed one
	   unlinks); only a merge drops it. Without it every lookup scans the
	   parent's children, so a flat document read or written key by key was
	   quadratic. Its own arena, freed on drop: the document arena cannot
	   give it back.
	   index_built: 0 none, 1 built, 2 in flux. The build and every append
	   allocate, and an SHCL_OOM hook that longjmps leaves whatever they were
	   in the middle of; a lookup that finds 2 drops the lot and rebuilds,
	   because a half-built chain can loop and a walk over it never ends. */
	ShclArena index_arena;
	int index_built;
	ShclCMap index_first;    /* name_key -> first child */
	ShclCMap index_last;     /* name_key -> chain tail */
	size_t *index_next;  /* per node; NIL ends the chain */
	size_t index_next_cap;
};
#define ROOT ((size_t)0)
#define NODE(d, i) ((d)->nodes.data[i])
#define NIL ((size_t)-1)
/* Stack entry for a binding line that was skipped: it still owns its indent
   level, so the lines written under it are skipped with it instead of
   re-parenting one level up. */
#define DEAD ((size_t)-1)
/* Stack entry for a line whose indent matched no open level (E012): never a
   level a sibling can bind at, but deeper lines are still under it. */
#define UNOPENED ((size_t)-2)

/* The node vector lives in malloc storage, not the bump arena: the arena
   cannot reclaim the abandoned copy at each doubling, which held about one
   extra full array at peak. realloc extends in place or frees the old block.
   Every doc comes from do_parse (calloc zeroes the vector); shcl_free is the
   one teardown and frees it. */
static void nodes_push(shcl_doc *d, ShclNode x) {
	ShclVecNode *v = &d->nodes;
	if (v->len == v->cap) {
		size_t nc = v->cap ? v->cap * 2 : 8;
		ShclNode *nd = (ShclNode *)realloc(v->data, nc * sizeof(ShclNode));
		if (!nd) arena_panic(d->panic);
		v->data = nd; v->cap = nc;
	}
	v->data[v->len++] = x;
}

/* Nil-safe trivia reads (empty defaults) and the get-or-create for writes;
   the sidecar is allocated in the document arena on the first write. */
static ShclVecLead triv_leading(const ShclNode *n) { if (n->trivia) return n->trivia->leading; ShclVecLead v; memset(&v, 0, sizeof v); return v; }
static ShclStr triv_trailing(const ShclNode *n) { return n->trivia ? n->trivia->trailing : s_empty(); }
static ShclVecLead triv_after(const ShclNode *n) { if (n->trivia) return n->trivia->after; ShclVecLead v; memset(&v, 0, sizeof v); return v; }
static ShclVecLead triv_inside(const ShclNode *n) { if (n->trivia) return n->trivia->inside; ShclVecLead v; memset(&v, 0, sizeof v); return v; }
static ShclTrivia *triv_mut(ShclArena *a, ShclNode *n) {
	if (!n->trivia) { n->trivia = (ShclTrivia *)arena_alloc(a, sizeof(ShclTrivia)); memset(n->trivia, 0, sizeof(ShclTrivia)); }
	return n->trivia;
}

/* The as-authored name spelling; empty name_src means "same as name". */
static ShclStr node_authored(const ShclNode *n) { return n->name_src.n ? n->name_src : n->name; }
/* Store a name's authored spelling: the empty sentinel when it matches the
   folded name, so the duplicate string never gets allocated. */
static ShclStr spelled(ShclArena *a, ShclStr name, ShclStr name_src) {
	return s_eq(name_src, name) ? s_empty() : s_dup(a, name_src);
}

/* Merge a later instance into an earlier one under the in-file merge rule:
   children and trivia move over, first trailing wins (a second demotes to a
   leading line), first spelling stays. The caller drops the loser from the
   parent's child list; it keeps its arena slot, unreferenced. */
static void fold_node_into(shcl_doc *d, size_t survivor, size_t loser) {
	ShclArena *a = &d->arena;
	ShclVecSize kids = NODE(d, loser).children;
	for (size_t k = 0; k < kids.len; k++) {
		NODE(d, kids.data[k]).parent = survivor;
		ShclVecSize_push(a, &NODE(d, survivor).children, kids.data[k]);
	}
	NODE(d, loser).children.len = 0;
	const ShclTrivia *lt = NODE(d, loser).trivia;
	if (lt) {
		NODE(d, loser).trivia = NULL;
		ShclTrivia *st = triv_mut(a, &NODE(d, survivor));
		for (size_t k = 0; k < lt->leading.len; k++)
			ShclVecLead_push(a, &st->leading, lt->leading.data[k]);
		if (lt->trailing.n) {
			if (st->trailing.n == 0) st->trailing = lt->trailing;
			else ShclVecLead_push(a, &st->leading, lead_plain(lt->trailing));
		}
		for (size_t k = 0; k < lt->after.len; k++)
			ShclVecLead_push(a, &st->after, lt->after.data[k]);
		for (size_t k = 0; k < lt->inside.len; k++)
			ShclVecLead_push(a, &st->inside, lt->inside.data[k]);
	}
}

static ShclValue v_empty(void) { ShclValue v; memset(&v, 0, sizeof v); v.kind = V_EMPTY; return v; }
static int v_is_empty(const ShclValue *v) { return v->kind == V_EMPTY; }

static ShclStr value_display(ShclArena *a, const ShclValue *v) {
	if (v->kind == V_EMPTY) return s_empty();
	if (v->kind == V_RAW) return v->raw->content;
	ShclSB s = {0};
	for (size_t i = 0; i < v->nels; i++) { if (i) sb_puts(a, &s, ", "); sb_putS(a, &s, v->els[i].text); }
	return sb_S(&s);
}

// --- lexical helpers ---------------------------------------------------------

// How the name half of a field line ends.
enum { NAME_END, NAME_COLON, NAME_HASH };
/* Scan a field line's name half the way the path scanner reads it. A quote
   opens only where the scanner opens one - as a segment's first char (a quoted
   name) or a selector body's first char (a quoted discriminator) - so O'Brien
   in a bare selector is text, not an open quote hiding the `#` after it. A
   backslash shields the next char inside quotes only; a bare selector body
   runs to the first `]` unescaped, as it does in the scanner. With `sugar` a
   colon followed by `[` is selector sugar rather than the separator. An
   unquoted `#` ends the half wherever it sits: a comment, or a malformed name.
   *at gets the byte offset it ended at. */
static int name_half(ShclStr s, int sugar, size_t *at) {
	uint32_t inq = 0; int in_sel = 0, at_start = 1; size_t i = 0;
	*at = 0;
	while (i < s.n) {
		uint32_t c; size_t l = utf8_decode(s.p, s.n, i, &c);
		if (inq) {
			if (c == '\\') { i += l; if (i < s.n) { uint32_t d; i += utf8_decode(s.p, s.n, i, &d); } continue; }
			if (c == inq) inq = 0;
			i += l; continue;
		}
		if (is_wsp(c)) { i += l; continue; }
		if (c == '#') { *at = i; return NAME_HASH; }
		if (in_sel) {
			if (c == ']') in_sel = 0;
			else if (at_start && (c == '"' || c == '\'')) inq = c;
			at_start = 0; i += l; continue;
		}
		if ((c == '"' || c == '\'') && at_start) inq = c;
		else if (c == '[') { in_sel = 1; at_start = 1; i += l; continue; }
		else if (c == '.') { at_start = 1; i += l; continue; }
		else if (c == ':') {
			size_t r = i + 1;
			while (r < s.n && (s.p[r] == ' ' || s.p[r] == '\t')) r++;
			if (!(sugar && r < s.n && s.p[r] == '[')) { *at = i; return NAME_COLON; }
		}
		at_start = 0;
		i += l;
	}
	return NAME_END;
}
/* Byte offset of the `#` that starts a comment in value text, scanning from
   `from`; s.n when there is none. A quote opens a quoted piece only at the
   start of a piece - the start of the value, or after an unquoted comma -
   which is the spec's rule: a piece is quoted only when it begins with one.
   So an apostrophe in prose (don't panic  # keep) hides nothing. A backslash
   shields the next char. */
static size_t value_comment_at(ShclStr s, size_t from) {
	uint32_t inq = 0; int at_start = 1; size_t i = from;
	while (i < s.n) {
		uint32_t c; size_t l = utf8_decode(s.p, s.n, i, &c);
		if (c == '\\') { i += l; if (i < s.n) { uint32_t d; i += utf8_decode(s.p, s.n, i, &d); } at_start = 0; continue; }
		if (inq) { if (c == inq) inq = 0; }
		else if ((c == '"' || c == '\'') && at_start) inq = c;
		else if (c == '#') return i;
		else if (c == ',') { at_start = 1; i += l; continue; }
		if (!is_wsp(c)) at_start = 0;
		i += l;
	}
	return s.n;
}
// Split off an unquoted trailing comment from a field line: returns the
// content, *comment gets the tail from `#` on (n == 0 = none). The name half
// is read the scanner's way and the value half the value's way (see
// value_comment_at). Comments are kept as trivia.
static ShclStr split_comment(ShclStr s, ShclStr *comment) {
	size_t at, hash = s.n;
	*comment = s_empty();
	if (!s.n || !memchr(s.p, '#', s.n)) return s;
	switch (name_half(s, 1, &at)) {
	case NAME_HASH: hash = at; break;
	case NAME_COLON: hash = value_comment_at(s, at + 1); break;
	default: break;
	}
	if (hash == s.n) return s;
	*comment = s_slice(s, hash, s.n);
	return s_slice(s, 0, hash);
}
// The same for value text alone: a list element, or a setter's argument.
static ShclStr split_value_comment(ShclStr s, ShclStr *comment) {
	*comment = s_empty();
	if (!s.n || !memchr(s.p, '#', s.n)) return s;
	size_t hash = value_comment_at(s, 0);
	if (hash == s.n) return s;
	*comment = s_slice(s, hash, s.n);
	return s_slice(s, 0, hash);
}
// Split on unquoted commas; a quote opens only at the start of a piece (see
// value_comment_at), and a backslash shields the next char. Emits byte offsets.
static void split_unquoted_commas(ShclArena *a, ShclStr s, ShclVecSize *offs_start, ShclVecSize *offs_end) {
	uint32_t inq = 0; int at_start = 1; size_t i = 0, start = 0;
	if (!s.n || !memchr(s.p, ',', s.n)) { ShclVecSize_push(a, offs_start, 0); ShclVecSize_push(a, offs_end, s.n); return; }
	while (i < s.n) {
		uint32_t c; size_t l = utf8_decode(s.p, s.n, i, &c);
		if (c == '\\') { i += l; if (i < s.n) { uint32_t d; i += utf8_decode(s.p, s.n, i, &d); } at_start = 0; continue; }
		if (inq) { if (c == inq) inq = 0; }
		else if ((c == '"' || c == '\'') && at_start) inq = c;
		else if (c == ',') { ShclVecSize_push(a, offs_start, start); ShclVecSize_push(a, offs_end, i); start = i + l; at_start = 1; i += l; continue; }
		if (!is_wsp(c)) at_start = 0;
		i += l;
	}
	ShclVecSize_push(a, offs_start, start); ShclVecSize_push(a, offs_end, s.n);
}
// Count of comma-split pieces (used where the reference only needs .len()).
static size_t count_unquoted_pieces(ShclStr s) {
	uint32_t inq = 0; int at_start = 1; size_t i = 0, n = 1;
	if (!s.n || !memchr(s.p, ',', s.n)) return 1;
	while (i < s.n) {
		uint32_t c; size_t l = utf8_decode(s.p, s.n, i, &c);
		if (c == '\\') { i += l; if (i < s.n) { uint32_t d; i += utf8_decode(s.p, s.n, i, &d); } at_start = 0; continue; }
		if (inq) { if (c == inq) inq = 0; }
		else if ((c == '"' || c == '\'') && at_start) inq = c;
		else if (c == ',') { n++; at_start = 1; i += l; continue; }
		if (!is_wsp(c)) at_start = 0;
		i += l;
	}
	return n;
}

// A dangling trailing backslash would swallow its separator on re-emit; double
// it. Text that needs no doubling passes through as the slice it came in as -
// the store sites own the copy question (s_keep, or an explicit dup).
static ShclStr norm_dangling(ShclArena *a, ShclStr t) {
	size_t run = 0;
	while (run < t.n && t.p[t.n - 1 - run] == '\\') run++;
	if (run % 2 == 1) {
		char *m = (char *)arena_alloc(a, t.n + 1);
		memcpy(m, t.p, t.n); m[t.n] = '\\';
		ShclStr r; r.p = m; r.n = t.n + 1; return r;
	}
	return t;
}

/* True when some piece starts with a quote that never closes (missing or
   escaped). Such a piece stays literal - and the quote-aware comment strip has
   already swallowed any trailing # comment into it - so the parser calls it
   out instead of letting the typo look deliberate. Mid-text apostrophes
   (it's fine) are legal prose and stay silent. */
static int quoted_shape(ShclStr t);
static int unterminated_quote(ShclArena *a, ShclStr text) {
	if (!text.n || (!memchr(text.p, '"', text.n) && !memchr(text.p, '\'', text.n))) return 0;
	ShclVecSize starts = {0}, ends = {0};
	split_unquoted_commas(a, text, &starts, &ends);
	for (size_t i = 0; i < starts.len; i++) {
		ShclStr piece; piece.p = text.p + starts.data[i]; piece.n = ends.data[i] - starts.data[i];
		ShclStr t = s_trim_wsp(piece);
		if (!quoted_shape(t)) {
			if (t.n == 0) continue;
			unsigned char first = (unsigned char)t.p[0];
			if (first == '"' || first == '\'') return 1;
		}
	}
	return 0;
}

/* True when the text is one quote pair: a quote char at both ends, the last
   one not escaped. Quotes and the backslash are ASCII (and UTF-8 never puts an
   ASCII byte inside a multibyte sequence), so bytes suffice. */
static int quoted_shape(ShclStr t) {
	if (t.n == 0) return 0;
	unsigned char first = (unsigned char)t.p[0];
	if ((first != '"' && first != '\'') || t.n < 2 || (unsigned char)t.p[t.n - 1] != first) return 0;
	int esc = 0;
	for (size_t i = 1; i + 1 < t.n; i++) esc = (t.p[i] == '\\' && !esc);
	return !esc;
}

// Trim, then strip one matching outer quote pair if present. present=0 -> dropped.
// The text is stored raw (escapes NOT applied), so both shapes are exact source
// slices - only a dangling-backslash bare element builds a new string.
static int parse_element(ShclArena *a, ShclStr piece, ShclElement *out) {
	ShclStr t = s_trim_wsp(piece);
	if (t.n == 0) return 0;
	if (quoted_shape(t)) {
		out->text = s_slice(t, 1, t.n - 1);
		out->quoted = 1; return 1;
	}
	out->text = norm_dangling(a, t);
	out->quoted = 0; return 1;
}
// Reads text as the value half of a line - see shcl_set_literal.
static int literal_value(ShclArena *a, ShclArena *tmp, ShclStr text, ShclValue *out);

// Element texts land in `a` (only when built - see parse_element); the comma
// offsets and the growing element vector are per-call temporaries and go to
// `tmp`, so only the exact-size final array reaches the document arena.
/* Whether parse_cell would build more than max elements. Counts the pieces
   the way the splitter cuts them, without building any, so a capped parse
   refuses an over-long line before holding the array. */
static int cell_exceeds(ShclStr text, size_t max) {
	size_t count = 0, i = 0; int has_content = 0; uint32_t in_quote = 0;
	while (i < text.n) {
		uint32_t c; size_t l = utf8_decode(text.p, text.n, i, &c); i += l;
		if (c == '\\') { has_content = 1; if (i < text.n) i += utf8_decode(text.p, text.n, i, &c); continue; }
		if (in_quote) { if (c == in_quote) in_quote = 0; }
		else if ((c == '"' || c == '\'') && !has_content) in_quote = c;
		else if (c == ',') {
			if (has_content && ++count > max) return 1;
			has_content = 0; continue;
		}
		if (!is_wsp(c)) has_content = 1;
	}
	if (has_content) count++;
	return count > max;
}

static ShclValue parse_cell(ShclArena *a, ShclArena *tmp, ShclStr text) {
	ShclVecSize starts = {0}, ends = {0};
	split_unquoted_commas(tmp, text, &starts, &ends);
	ShclVecEl els = {0};
	for (size_t i = 0; i < starts.len; i++) {
		ShclElement e;
		if (parse_element(a, s_slice(text, starts.data[i], ends.data[i]), &e)) ShclVecEl_push(tmp, &els, e);
	}
	if (els.len == 0) return v_empty();
	ShclValue v; memset(&v, 0, sizeof v); v.kind = V_CELL;
	v.els = (ShclElement *)arena_alloc(a, els.len * sizeof(ShclElement));
	memcpy(v.els, els.data, els.len * sizeof(ShclElement));
	v.nels = els.len;
	return v;
}

// Escape processing (string reads): \t \n \\ \" \'; unknown escapes stay literal.
// Text with no backslash comes back as the slice it came in as - a read of a
// plain field must not build a copy in the arena (repeated reads of one field
// once grew a document without bound).
static ShclStr apply_escapes(ShclArena *a, ShclStr s) {
	if (!s.n || !memchr(s.p, '\\', s.n)) return s;
	ShclSB out = {0};
	size_t i = 0;
	while (i < s.n) {
		uint32_t c; size_t l = utf8_decode(s.p, s.n, i, &c); i += l;
		if (c != '\\') { sb_put_cp(a, &out, c); continue; }
		if (i >= s.n) { sb_putc(a, &out, '\\'); break; }
		uint32_t d; size_t l2 = utf8_decode(s.p, s.n, i, &d); i += l2;
		switch (d) {
		case 't': sb_putc(a, &out, '\t'); break;
		case 'n': sb_putc(a, &out, '\n'); break;
		case '\\': sb_putc(a, &out, '\\'); break;
		case '"': sb_putc(a, &out, '"'); break;
		case '\'': sb_putc(a, &out, '\''); break;
		default: sb_putc(a, &out, '\\'); sb_put_cp(a, &out, d); break;
		}
	}
	return sb_S(&out);
}

/* The predicate a `[value]` selector matches with: display form with escapes
   applied on both sides, so `["q\"uote"]` finds `'q"uote'` - a logical-string
   match, not spelling against spelling. */
/* The restriction a QUOTED [value] selector adds on top of the display
   match: quoting selects the scalar spelling only, so the scalar "a, b" and
   the list a, b stop meeting the same selector. */
static int single_scalar(const ShclValue *v) { return v->kind == V_CELL && v->nels == 1; }

static ShclStr disp_key(ShclArena *a, const ShclValue *v) {
	return apply_escapes(a, value_display(a, v));
}

// FNV-1a, fed the same byte sequence the old built key strings spelled - the
// accelerator maps key on a u64 and a hit verifies against the arena, so the
// strings themselves never get built. The hash only has to be stable within
// one parse, not injective; a collision just chains in the slot.
static uint64_t fnv_byte(uint64_t h, unsigned char b) { return (h ^ b) * 1099511628211ull; }
static uint64_t fnv_str(uint64_t h, ShclStr s) {
	for (size_t i = 0; i < s.n; i++) h = fnv_byte(h, (unsigned char)s.p[i]);
	return h;
}
/* A length prefix in decimal, spelled without allocating. */
static uint64_t fnv_dec(uint64_t h, size_t n) {
	char buf[20];
	size_t i = sizeof buf;
	do { buf[--i] = (char)('0' + n % 10); n /= 10; } while (n);
	while (i < sizeof buf) h = fnv_byte(h, (unsigned char)buf[i++]);
	return h;
}
static uint64_t cmap_hash(ShclStr name, ShclStr key) {
	uint64_t h = 1469598103934665603ull;
	h = fnv_str(h, name);
	h = fnv_byte(h, 0xFFu); /* separator; equality still verifies both parts */
	return fnv_str(h, key);
}
static uint64_t name_key(size_t parent, ShclStr name) {
	uint64_t h = 1469598103934665603ull;
	h = fnv_dec(h, parent);
	h = fnv_byte(h, 0xFFu);
	return fnv_str(h, name);
}

/* Hash of the (name, merge-key) pair, spelling the merge-key byte sequence -
   'e', or each cell element (and the raw info-string) length-prefixed so the
   sequence is injective - without building it as a string. */
/* apply_escapes as a streaming feed into the hash - the same state machine,
   one byte at a time, no intermediate string. Bytes suffice: every special
   character is ASCII and UTF-8 never puts an ASCII byte inside a multibyte
   sequence, so backslash-then-multibyte passes both through exactly as the
   codepoint walk would. */
typedef struct { uint64_t h; int pending; } ShclEscHash;
static void esc_push(ShclEscHash *e, unsigned char b) {
	if (e->pending) {
		e->pending = 0;
		switch (b) {
		case 't': e->h = fnv_byte(e->h, '\t'); break;
		case 'n': e->h = fnv_byte(e->h, '\n'); break;
		case '\\': e->h = fnv_byte(e->h, '\\'); break;
		case '"': e->h = fnv_byte(e->h, '"'); break;
		case '\'': e->h = fnv_byte(e->h, '\''); break;
		default: e->h = fnv_byte(e->h, '\\'); e->h = fnv_byte(e->h, b); break;
		}
	} else if (b == '\\') {
		e->pending = 1;
	} else {
		e->h = fnv_byte(e->h, b);
	}
}
static void esc_str(ShclEscHash *e, ShclStr s) {
	for (size_t i = 0; i < s.n; i++) esc_push(e, (unsigned char)s.p[i]);
}
static uint64_t esc_finish(ShclEscHash *e) {
	if (e->pending) { e->pending = 0; e->h = fnv_byte(e->h, '\\'); }
	return e->h;
}

/* The escape-resolved length of s, so a merge key can length-prefix an element
   without building the resolved text. Mirrors apply_escapes exactly: five
   escapes collapse two bytes to one, any other backslash pair keeps both, and
   a trailing lone backslash stands. */
static size_t esc_len(ShclStr s) {
	size_t n = 0;
	for (size_t i = 0; i < s.n; i++) {
		if (s.p[i] != '\\' || i + 1 >= s.n) { n++; continue; }
		unsigned char d = (unsigned char)s.p[i + 1];
		n++;
		if (d == 't' || d == 'n' || d == '\\' || d == '"' || d == '\'') i++;
	}
	return n;
}

/* One resolved byte at a time, so two texts compare with escapes applied and
   nothing built. */
static int esc_next(ShclStr s, size_t *i, unsigned char *out) {
	if (*i >= s.n) return 0;
	unsigned char c = (unsigned char)s.p[(*i)++];
	if (c != '\\' || *i >= s.n) { *out = c; return 1; }
	unsigned char d = (unsigned char)s.p[*i];
	switch (d) {
	case 't':  *out = '\t'; (*i)++; return 1;
	case 'n':  *out = '\n'; (*i)++; return 1;
	case '\\': *out = '\\'; (*i)++; return 1;
	case '"':  *out = '"';  (*i)++; return 1;
	case '\'': *out = '\''; (*i)++; return 1;
	default:   *out = '\\'; return 1;   /* the backslash stands; d comes next */
	}
}

/* Escape-resolved equality: two spellings of one string are one instance.
   Names have followed that rule since 2.0, and a `[value]` selector matches on
   the resolved text already - without this, one selector addressed two
   instances. */
static int esc_eq(ShclStr a, ShclStr b) {
	size_t i = 0, j = 0; unsigned char x = 0, y = 0;
	for (;;) {
		int ha = esc_next(a, &i, &x), hb = esc_next(b, &j, &y);
		if (!ha || !hb) return ha == hb;
		if (x != y) return 0;
	}
}

static uint64_t merge_hash(ShclStr name, const ShclValue *v) {
	uint64_t h = 1469598103934665603ull;
	h = fnv_str(h, name);
	h = fnv_byte(h, 0xFFu);
	if (v->kind == V_EMPTY) return fnv_byte(h, 'e');
	if (v->kind == V_CELL) {
		h = fnv_byte(h, 'c'); h = fnv_byte(h, ':');
		for (size_t i = 0; i < v->nels; i++) {
			h = fnv_dec(h, esc_len(v->els[i].text));
			h = fnv_byte(h, ':');
			ShclEscHash e; e.h = h; e.pending = 0;
			esc_str(&e, v->els[i].text);
			h = esc_finish(&e);
		}
		return h;
	}
	/* Info-string is part of identity (a `sql` and a `python` block are
	   different values even with equal bodies); fence style is not. */
	h = fnv_byte(h, 'r'); h = fnv_byte(h, ':');
	h = fnv_dec(h, v->raw->info.n);
	h = fnv_byte(h, ':');
	h = fnv_str(h, v->raw->info);
	return fnv_str(h, v->raw->content);
}

/* The exact (name, merge-key) equality a hashed hit is verified with -
   compares what the two key strings would have held, element by element. The
   quoted flag is not part of the key, same as the strings never carried it. */
static int value_eq(const ShclValue *a, const ShclValue *b) {
	if (a->kind != b->kind) return 0;
	if (a->kind == V_EMPTY) return 1;
	if (a->kind == V_CELL) {
		if (a->nels != b->nels) return 0;
		for (size_t i = 0; i < a->nels; i++)
			if (!esc_eq(a->els[i].text, b->els[i].text)) return 0;
		return 1;
	}
	return s_eq(a->raw->info, b->raw->info) && s_eq(a->raw->content, b->raw->content);
}
static int merge_eq(ShclStr name_a, const ShclValue *va, ShclStr name_b, const ShclValue *vb) {
	return s_eq(name_a, name_b) && value_eq(va, vb);
}


/* Hash of the (name, display-with-escapes-applied) pair a `[value]` selector
   matches with - what disp_key spells, streamed instead of built. */
static uint64_t disp_hash(ShclStr name, const ShclValue *v) {
	ShclEscHash e;
	e.h = 1469598103934665603ull;
	e.h = fnv_str(e.h, name);
	e.h = fnv_byte(e.h, 0xFFu);
	e.pending = 0;
	if (v->kind == V_CELL) {
		for (size_t i = 0; i < v->nels; i++) {
			if (i) { esc_push(&e, ','); esc_push(&e, ' '); }
			esc_str(&e, v->els[i].text);
		}
	} else if (v->kind == V_RAW) {
		esc_str(&e, v->raw->content);
	}
	return esc_finish(&e);
}
/* The query-side twin of disp_hash: the selector's text already has its
   escapes applied, so its bytes feed straight in. */
static uint64_t disp_hash_text(ShclStr name, ShclStr want) { return cmap_hash(name, want); }

// Opening fence: a run of >=3 backticks or tildes, then an optional info string.
// The info is a slice of rest; the parse stores it as a slice of the retained
// input copy.
typedef struct { int ok; unsigned char ch; size_t len; ShclStr info; } ShclFence;
static ShclFence fence_open(ShclStr rest) {
	ShclFence f; f.ok = 0; f.ch = 0; f.len = 0; f.info = s_empty();
	if (rest.n == 0) return f;
	unsigned char first = (unsigned char)rest.p[0];
	if (first != '`' && first != '~') return f;
	size_t run = 0;
	while (run < rest.n && (unsigned char)rest.p[run] == first) run++;
	if (run < 3) return f;
	f.ok = 1; f.ch = first; f.len = run;
	f.info = s_trim_wsp(s_slice(rest, run, rest.n));
	return f;
}
/* min_len is the opening fence's length, which the grammar puts at three or
   more, so the length test already rules out the empty line the loop below
   would otherwise accept. */
static int is_fence_close(ShclStr line, unsigned char ch, size_t min_len) {
	ShclStr t = s_trim_wsp(line);
	if (t.n < min_len) return 0;
	for (size_t i = 0; i < t.n; i++) if ((unsigned char)t.p[i] != ch) return 0;
	return 1;
}

/* The leading space/tab run of a line (the indent). */
static ShclStr leading_ws(ShclStr line) {
	size_t n = 0;
	while (n < line.n && (line.p[n] == ' ' || line.p[n] == '\t')) n++;
	return s_slice(line, 0, n);
}

/* Remove a raw block's nesting indent from one content line: only what the
   line actually shares with it, so a shallower line (whitespace-only, or
   written flush left) keeps its own spacing rather than being blanked. */
static ShclStr strip_common(ShclStr line, ShclStr common) {
	size_t k = 0;
	while (k < common.n && k < line.n && common.p[k] == line.p[k]) k++;
	return s_slice(line, k, line.n);
}

// --- path scanner ------------------------------------------------------------

typedef enum { SEL_NONE, SEL_VALUE, SEL_INDEX, SEL_WILDCARD } ShclSelTag;
/* quoted: the selector text was quoted in the path. A quoted selector is
   scalar-only - it matches a single-element value whose logical string equals
   the text - so quoting distinguishes the scalar "a, b" from the two-element
   list a, b, the same way quoting escapes elsewhere. */
typedef struct { ShclSelTag tag; ShclStr value; uint64_t index; int quoted; } ShclSelector; // u64: width must not vary with target pointer size
typedef struct { ShclStr name; ShclStr name_src; ShclSelector sel; int star; } ShclSegment; // name_src: as authored (unfolded); star: bare `*` name wildcard; quoted "*" stays a literal name
DEFINE_VEC(ShclVecSeg, ShclSegment)
typedef struct { int ok; ShclVecSeg segs; int has_value; ShclStr value_text; ShclStr err; } ShclPathScan;

// usize parse: optional single leading '+', >=1 digit, no overflow.
/* The spelling of an index selector - an optional `#`, an optional `+`, then
   digits - whatever its size. The grammar says 1*DIGIT, with no upper bound. */
static int index_shape(ShclStr body) {
	size_t i = 0;
	if (i < body.n && body.p[i] == '#') i++;
	if (i < body.n && body.p[i] == '+') i++;
	if (i >= body.n) return 0;
	for (; i < body.n; i++) if (!is_adigit((unsigned char)body.p[i])) return 0;
	return 1;
}
static int parse_u64(ShclStr s, uint64_t *out) {
	size_t i = 0;
	if (i < s.n && s.p[i] == '+') i++;
	if (i >= s.n) return 0;
	uint64_t v = 0;
	for (; i < s.n; i++) {
		unsigned char c = (unsigned char)s.p[i];
		if (!is_adigit(c)) return 0;
		if (v > (UINT64_MAX - (c - '0')) / 10) return 0;
		v = v * 10 + (c - '0');
	}
	*out = v; return 1;
}

// Byte-offset cursor over the path text (a decode_cps codepoint array per call
// was a parse hot spot). Positions advance by whole codepoints via the same
// utf8_decode, so the codepoint sequence - and every slice boundary - is
// identical to the old array walk, permissive decoding included.
static void skip_ws_path(ShclStr s, size_t *pos) {
	while (*pos < s.n) {
		uint32_t c; size_t l = utf8_decode(s.p, s.n, *pos, &c);
		if (c != ' ' && c != '\t') break;
		*pos += l;
	}
}
// Read a quoted name/value in a path (escape pairs preserved literally).
static int read_quoted_path(ShclArena *a, ShclStr src, size_t *pos, ShclStr *out, ShclStr *err) {
	uint32_t q; *pos += utf8_decode(src.p, src.n, *pos, &q);
	ShclSB sb = {0};
	for (;;) {
		if (*pos >= src.n) { *err = s_lit("unterminated quote"); return 0; }
		uint32_t ch; size_t l = utf8_decode(src.p, src.n, *pos, &ch);
		if (ch == '\\' && *pos + l < src.n) {
			uint32_t d; size_t l2 = utf8_decode(src.p, src.n, *pos + l, &d);
			sb_put_cp(a, &sb, ch); sb_put_cp(a, &sb, d);
			*pos += l + l2; continue;
		}
		*pos += l;
		if (ch == q) { *out = sb_S(&sb); return 1; }
		sb_put_cp(a, &sb, ch);
	}
}

static ShclPathScan scan_path_ex(ShclArena *a, ShclStr input, int stars) {
	ShclPathScan ps; ps.ok = 0; memset(&ps.segs, 0, sizeof ps.segs); ps.has_value = 0; ps.value_text = s_empty(); ps.err = s_empty();
	size_t pos = 0;
	for (;;) {
		skip_ws_path(input, &pos);
		if (pos >= input.n) { ps.err = s_lit("empty path"); return ps; }
		// Field name: quoted, bare, or (lookups only) the `*` name wildcard.
		int star = 0;
		ShclStr name;
		uint32_t cur; size_t curl = utf8_decode(input.p, input.n, pos, &cur);
		if (cur == '"' || cur == '\'') {
			if (!read_quoted_path(a, input, &pos, &name, &ps.err)) return ps;
		} else if (stars && cur == '*') {
			pos += curl;
			star = 1;
			name = s_lit("*");
		} else {
			size_t start = pos;
			while (pos < input.n) {
				uint32_t bc; size_t bl = utf8_decode(input.p, input.n, pos, &bc);
				if (!is_bare_name_char(bc)) break;
				pos += bl;
			}
			if (pos == start) {
				ShclSB e = {0}; sb_puts(a, &e, "expected field name, found '"); sb_put_cp(a, &e, cur); sb_putc(a, &e, '\'');
				ps.err = sb_S(&e); return ps;
			}
			name = s_slice(input, start, pos);
		}
		ShclSelector sel; sel.tag = SEL_NONE; sel.value = s_empty(); sel.index = 0; sel.quoted = 0;
		skip_ws_path(input, &pos);
		int have_bracket = 0; size_t bracket_end = 0; // byte offset just past the '['
		if (pos < input.n) {
			uint32_t sc; size_t sl = utf8_decode(input.p, input.n, pos, &sc);
			if (sc == '[') { have_bracket = 1; bracket_end = pos + sl; }
			else if (sc == ':') {
				size_t q = pos + sl; skip_ws_path(input, &q);
				if (q < input.n) {
					uint32_t qc; size_t ql = utf8_decode(input.p, input.n, q, &qc);
					if (qc == '[') { have_bracket = 1; bracket_end = q + ql; }
				}
			}
		}
		if (have_bracket) {
			pos = bracket_end;
			skip_ws_path(input, &pos);
			uint32_t oc = 0; size_t ocl = 0;
			if (pos < input.n) ocl = utf8_decode(input.p, input.n, pos, &oc);
			(void)ocl;
			if (pos < input.n && (oc == '"' || oc == '\'')) {
				ShclStr v; if (!read_quoted_path(a, input, &pos, &v, &ps.err)) return ps;
				sel.tag = SEL_VALUE; sel.value = v; sel.quoted = 1; // quotes force a value match, even numeric - and scalar-only
			} else {
				size_t start = pos;
				while (pos < input.n) {
					uint32_t bc; size_t bl = utf8_decode(input.p, input.n, pos, &bc);
					if (bc == ']') break;
					pos += bl;
				}
				ShclStr body = s_trim_wsp(s_slice(input, start, pos));
				uint64_t idx;
				if (body.n == 1 && body.p[0] == '*') {
					sel.tag = SEL_WILDCARD;
				} else if (body.n >= 1 && body.p[0] == '#' && parse_u64(s_slice(body, 1, body.n), &idx)) {
					sel.tag = SEL_INDEX; sel.index = idx;
				} else if (parse_u64(body, &idx)) {
					sel.tag = SEL_INDEX; sel.index = idx;
				} else if (index_shape(body)) {
					/* All digits but past u64: an index no instance can have,
					   not a value selector that would create one on a write. */
					sel.tag = SEL_INDEX; sel.index = UINT64_MAX;
				} else if (body.n == 0) {
					ps.err = s_lit("empty selector"); return ps;
				} else {
					sel.tag = SEL_VALUE; sel.value = norm_dangling(a, body);
				}
			}
			skip_ws_path(input, &pos);
			uint32_t cc = 0; size_t ccl = 0;
			if (pos < input.n) ccl = utf8_decode(input.p, input.n, pos, &cc);
			if (pos >= input.n || cc != ']') { ps.err = s_lit("unterminated selector"); return ps; }
			pos += ccl;
			skip_ws_path(input, &pos);
		}
		if (star && sel.tag != SEL_NONE) { ps.err = s_lit("selector on a name wildcard"); return ps; }
		/* Names resolve escapes, the same rule values follow when they are
		   compared: two spellings of one name are one name. name_src keeps the
		   source spelling, which is what shcl_authored_name hands back. */
		ShclSegment seg;
		seg.name = name_plain(name) ? name : fold_name(a, apply_escapes(a, name));
		seg.name_src = name; seg.sel = sel; seg.star = star;
		ShclVecSeg_push(a, &ps.segs, seg);
		if (pos >= input.n) { ps.ok = 1; ps.has_value = 0; return ps; }
		uint32_t dc; size_t dl = utf8_decode(input.p, input.n, pos, &dc);
		if (dc == '.') { pos += dl; continue; }
		if (dc == ':') {
			pos += dl;
			ps.ok = 1; ps.has_value = 1;
			ps.value_text = s_trim_wsp(s_slice(input, pos, input.n));
			return ps;
		}
		{ ShclSB e = {0}; sb_puts(a, &e, "unexpected '"); sb_put_cp(a, &e, dc); sb_puts(a, &e, "' after field"); ps.err = sb_S(&e); return ps; }
	}
}

static ShclPathScan scan_path(ShclArena *a, ShclStr input) { return scan_path_ex(a, input, 0); }

/* A value spelled the way JSON, TOML and YAML spell an array. The path scanner
   reads the brackets as a selector, so the line arrives with no value text and
   the old repair blamed a colon that is plainly there. The colon that counts
   is the field's own: one inside a quoted name or a selector is not it. */
static int looks_like_bracket_array(ShclStr content) {
	size_t colon;
	if (name_half(content, 0, &colon) != NAME_COLON) return 0;
	size_t b = colon + 1, e = content.n;
	while (b < e && (unsigned char)content.p[b] <= ' ') b++;
	while (e > b && (unsigned char)content.p[e - 1] <= ' ') e--;
	return e > b && content.p[b] == '[' && content.p[e - 1] == ']';
}
// Query spelling of scan_path: also accepts a bare `*` segment (the name
// wildcard - any child name). Document lines never take it; only lookups
// (reads, the writer probe, schema paths) do.
static ShclPathScan scan_lookup(ShclArena *a, ShclStr input) { return scan_path_ex(a, input, 1); }

// --- small integer/string helpers used below --------------------------------

static void sb_put_u64(ShclArena *a, ShclSB *s, uint64_t v) {
	char t[24]; int j = 0;
	if (v == 0) t[j++] = '0';
	while (v) { t[j++] = (char)('0' + (v % 10)); v /= 10; }
	char o[24]; for (int k = 0; k < j; k++) o[k] = t[j - 1 - k];
	// cppcheck-suppress uninitvar  ## j >= 1 always (v==0 writes '0'), so o[0..j-1] is filled
	sb_put(a, s, o, (size_t)j);
}
static int s_contains_char(ShclStr s, char c) { for (size_t i = 0; i < s.n; i++) if (s.p[i] == c) return 1; return 0; }
static int s_has_fence_run(ShclStr s) {
	for (size_t i = 0; i + 2 < s.n; i++)
		if ((s.p[i] == '`' || s.p[i] == '~') && s.p[i + 1] == s.p[i] && s.p[i + 2] == s.p[i]) return 1;
	return 0;
}

typedef struct { ShclStr indent; size_t node; } ShclStackEnt;
DEFINE_VEC(ShclVecStack, ShclStackEnt)
typedef struct { int present; size_t idx; shcl_status miss; } ShclSlot;
DEFINE_VEC(ShclVecSlot, ShclSlot)

// --- coercion ("intelligent but safe"; Loose re-admits a closed list) --------

static const uint32_t SHCL_CURRENCY[] = {
	'$', 0xA2, 0xA3, 0xA4, 0xA5, 0x20A9, 0x20AA, 0x20AB, 0x20AC, 0x20AD,
	0x20AE, 0x20B1, 0x20B2, 0x20B4, 0x20B9, 0x20BA, 0x20BC, 0x20BD, 0x20BE, 0x20BF,
};
/* The remainder after a leading currency symbol, with the space a person writes
   after one taken off - `$ 1200` reached the int path's thousands branch, which
   trims, and the float path's shape test, which does not. */
static ShclStr strip_currency(ShclStr t) {
	if (t.n == 0) return t;
	uint32_t c; size_t l = utf8_decode(t.p, t.n, 0, &c);
	for (size_t i = 0; i < sizeof(SHCL_CURRENCY) / sizeof(SHCL_CURRENCY[0]); i++)
		if (c == SHCL_CURRENCY[i]) return trim_start(s_slice(t, l, t.n));
	return t;
}

// [+/-]digits, fully consumed, no overflow.
static int parse_i64_s(ShclStr t, int64_t *out) {
	size_t i = 0; int neg = 0;
	if (i < t.n && (t.p[i] == '+' || t.p[i] == '-')) { neg = (t.p[i] == '-'); i++; }
	if (i >= t.n) return 0;
	uint64_t v = 0; uint64_t lim = neg ? (uint64_t)INT64_MAX + 1 : (uint64_t)INT64_MAX;
	for (; i < t.n; i++) {
		unsigned char c = (unsigned char)t.p[i];
		if (!is_adigit(c)) return 0;
		if (v > (lim - (c - '0')) / 10) return 0;
		v = v * 10 + (c - '0');
	}
	if (neg) *out = (v == (uint64_t)INT64_MAX + 1) ? INT64_MIN : -(int64_t)v;
	else *out = (int64_t)v;
	return 1;
}
// magnitude hex in [0, INT64_MAX]; overflow -> fail.
// The magnitude, as u64 (guarded against u64 overflow). The sign range-check is
// the caller's, so the negative i64_min magnitude (0x8000000000000000) reads.
static int parse_hex_u64(ShclStr h, uint64_t *out) {
	uint64_t v = 0;
	for (size_t i = 0; i < h.n; i++) {
		unsigned char c = (unsigned char)h.p[i]; int d;
		if (c >= '0' && c <= '9') d = c - '0';
		else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
		else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
		else return 0;
		if (v > (UINT64_MAX - (uint64_t)d) / 16) return 0;
		v = v * 16 + (uint64_t)d;
	}
	*out = v; return 1;
}
static void split_byte(ShclArena *a, ShclStr s, char sep, ShclVecS *out) {
	size_t start = 0;
	for (size_t i = 0; i <= s.n; i++)
		if (i == s.n || s.p[i] == sep) { ShclVecS_push(a, out, s_slice(s, start, i)); start = i + 1; }
}

static int parse_int_text(ShclArena *a, const ShclElement *e, shcl_strictness level, int64_t *out);
static int parse_float_text(ShclArena *a, const ShclElement *e, shcl_strictness level, double *out);
static int parse_int_text_wide(ShclArena *a, const ShclElement *e, double *out);

static int float_shape_ok(ShclStr t) {
	ShclStr body = t;
	if (body.n > 0 && (body.p[0] == '+' || body.p[0] == '-')) body = s_slice(body, 1, body.n);
	if (body.n == 0) return 0;
	ShclStr mant = body, exp = s_empty(); int has_exp = 0;
	for (size_t i = 0; i < body.n; i++)
		if (body.p[i] == 'e' || body.p[i] == 'E') { mant = s_slice(body, 0, i); exp = s_slice(body, i + 1, body.n); has_exp = 1; break; }
	if (has_exp) {
		ShclStr xb = exp;
		if (xb.n > 0 && (xb.p[0] == '+' || xb.p[0] == '-')) xb = s_slice(xb, 1, xb.n);
		if (xb.n == 0 || !all_adigit0(xb)) return 0;
	}
	ShclStr ip = mant, fp = s_empty(); int has_dot = 0;
	for (size_t i = 0; i < mant.n; i++)
		if (mant.p[i] == '.') { ip = s_slice(mant, 0, i); fp = s_slice(mant, i + 1, mant.n); has_dot = 1; break; }
	(void)has_dot;
	if (ip.n == 0 && fp.n == 0) return 0;
	return all_adigit0(ip) && all_adigit0(fp);
}
static int strtod_full(ShclArena *a, ShclStr t, double *out) {
	const char *dp = dec_point(); size_t dn = strlen(dp);
	char *buf = (char *)arena_alloc(a, t.n * dn + 1);
	size_t j = 0;
	for (size_t i = 0; i < t.n; i++) {
		if (t.p[i] == '.') { memcpy(buf + j, dp, dn); j += dn; }
		else buf[j++] = t.p[i];
	}
	buf[j] = '\0';
	char *end; double v = strtod(buf, &end);
	if (end != buf + j) return 0;
	/* A literal past the double range is an infinity, which no double holds
	   and no setter can write back: BadType, like a text that is not a number
	   at all. */
	if (!isfinite(v)) return 0;
	*out = v; return 1;
}
static int parse_float_text(ShclArena *a, const ShclElement *e, shcl_strictness level, double *out) {
	ShclStr t = s_trim(e->text); int percent = 0;
	if (level == SHCL_LOOSE) {
		t = strip_currency(t);
		if (t.n > 0 && t.p[t.n - 1] == '%') { t = trim_end(s_slice(t, 0, t.n - 1)); percent = 1; }
	}
	double v;
	if (float_shape_ok(t)) {
		if (!strtod_full(a, t, &v)) return 0;
	} else {
		ShclElement el; el.text = t; el.quoted = e->quoted;
		int64_t iv;
		if (parse_int_text(a, &el, SHCL_STANDARD, &iv)) v = (double)iv;
		else if (!parse_int_text_wide(a, &el, &v)) return 0;
	}
	*out = percent ? v / 100.0 : v; return 1;
}
/* The two integer spellings the plain float parse does not read - hex, and
   quoted thousands - past the i64 range, as a double: a float read is bounded
   by the double, not by the integer type. Hex goes in digit by digit in the
   double, so every binding rounds the same way; the spellings mirror
   parse_int_text. */
static int parse_int_text_wide(ShclArena *a, const ShclElement *e, double *out) {
	ShclStr t = s_trim(e->text);
	int neg = 0; ShclStr body = t;
	if (t.n > 0 && t.p[0] == '-') { neg = 1; body = s_slice(t, 1, t.n); }
	else if (t.n > 0 && t.p[0] == '+') body = s_slice(t, 1, t.n);
	double v = 0.0;
	if (s_starts(body, "0x") || s_starts(body, "0X")) {
		ShclStr h = s_slice(body, 2, body.n);
		if (h.n == 0 || !all_ahex(h)) return 0;
		for (size_t i = 0; i < h.n; i++) {
			unsigned char c = (unsigned char)h.p[i];
			int d = c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10;
			v = v * 16.0 + (double)d;
		}
	} else if (e->quoted && s_contains_char(body, ',')) {
		ShclVecS groups = {0}; split_byte(a, body, ',', &groups);
		int wf = groups.len > 1 && groups.data[0].n > 0 && groups.data[0].n <= 3 && all_adigit0(groups.data[0]);
		if (wf) for (size_t k = 1; k < groups.len; k++)
			if (groups.data[k].n != 3 || !all_adigit0(groups.data[k])) { wf = 0; break; }
		if (!wf) return 0;
		ShclSB b = {0};
		for (size_t i = 0; i < body.n; i++) if (body.p[i] != ',') sb_putc(a, &b, body.p[i]);
		if (!strtod_full(a, sb_S(&b), &v)) return 0;
	} else return 0;
	if (!isfinite(v)) return 0;
	*out = neg ? -v : v; return 1;
}
static int parse_int_text(ShclArena *a, const ShclElement *e, shcl_strictness level, int64_t *out) {
	ShclStr t = s_trim(e->text);
	if (level == SHCL_LOOSE) t = strip_currency(t);
	ShclStr body = t;
	if (body.n > 0 && (body.p[0] == '+' || body.p[0] == '-')) body = s_slice(body, 1, body.n);
	if (body.n > 0 && all_adigit0(body)) return parse_i64_s(t, out);
	int neg = 0; ShclStr hex = t;
	if (t.n > 0 && t.p[0] == '-') { neg = 1; hex = s_slice(t, 1, t.n); }
	else if (t.n > 0 && t.p[0] == '+') { hex = s_slice(t, 1, t.n); }
	if (s_starts(hex, "0x") || s_starts(hex, "0X")) {
		ShclStr h = s_slice(hex, 2, hex.n);
		if (all_ahex(h)) {
			uint64_t m; if (!parse_hex_u64(h, &m)) return 0;
			if (neg) {
				if (m == (uint64_t)INT64_MAX + 1) *out = INT64_MIN;
				else if (m <= (uint64_t)INT64_MAX) *out = -(int64_t)m;
				else return 0;
			} else {
				if (m <= (uint64_t)INT64_MAX) *out = (int64_t)m;
				else return 0;
			}
			return 1;
		}
	}
	if (e->quoted && s_contains_char(t, ',')) {
		ShclStr sign_body = t;
		if (sign_body.n > 0 && (sign_body.p[0] == '+' || sign_body.p[0] == '-')) sign_body = s_slice(sign_body, 1, sign_body.n);
		ShclVecS groups = {0}; split_byte(a, sign_body, ',', &groups);
		int wf = groups.len > 1 && groups.data[0].n > 0 && groups.data[0].n <= 3 && all_adigit0(groups.data[0]);
		if (wf) for (size_t k = 1; k < groups.len; k++)
			if (groups.data[k].n != 3 || !all_adigit0(groups.data[k])) { wf = 0; break; }
		if (wf) {
			ShclSB b = {0}; for (size_t i = 0; i < t.n; i++) if (t.p[i] != ',') sb_putc(a, &b, t.p[i]);
			return parse_i64_s(sb_S(&b), out);
		}
	}
	if (level == SHCL_LOOSE) {
		double f;
		if (parse_float_text(a, e, level, &f)) {
			double r = round(f);
			/* INT64_MAX has no exact double, so the top bound is 2^63 itself,
			   exclusively; INT64_MIN is exact. */
			if (r >= -9223372036854775808.0 && r < 9223372036854775808.0) {
				*out = (int64_t)r;
				return 1;
			}
		}
	}
	return 0;
}
static int parse_bool_text(ShclArena *a, ShclStr t, shcl_strictness level, int *out) {
	ShclStr s = ascii_lower(a, s_trim(t));
	#define SHCL_EQ(z) (s.n == strlen(z) && memcmp(s.p, z, s.n) == 0)
	if (SHCL_EQ("true")) { *out = 1; return 1; }
	if (SHCL_EQ("false")) { *out = 0; return 1; }
	if (level == SHCL_STRICT) return 0;
	if (SHCL_EQ("yes") || SHCL_EQ("on") || SHCL_EQ("1")) { *out = 1; return 1; }
	if (SHCL_EQ("no") || SHCL_EQ("off") || SHCL_EQ("0")) { *out = 0; return 1; }
	if (level == SHCL_LOOSE) {
		if (SHCL_EQ("t") || SHCL_EQ("y") || SHCL_EQ("enable") || SHCL_EQ("enabled")) { *out = 1; return 1; }
		if (SHCL_EQ("f") || SHCL_EQ("n") || SHCL_EQ("disable") || SHCL_EQ("disabled")) { *out = 0; return 1; }
	}
	#undef SHCL_EQ
	return 0;
}

// --- date/time (closed whitelist; shape match, then calendar validation) -----

static uint32_t month_from_name(ShclArena *a, ShclStr s) {
	static const char *names[] = {
		"jan","feb","mar","apr","may","jun","jul","aug","sep","oct","nov","dec",
		"january","february","march","april","june","july","august","september",
		"october","november","december"
	};
	static const uint32_t vals[] = { 1,2,3,4,5,6,7,8,9,10,11,12, 1,2,3,4,6,7,8,9,10,11,12 };
	ShclStr l = ascii_lower(a, s);
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
		if (l.n == strlen(names[i]) && memcmp(l.p, names[i], l.n) == 0) return vals[i];
	return 0;
}
static uint32_t days_in_month(int32_t y, uint32_t m) {
	switch (m) {
	case 1: case 3: case 5: case 7: case 8: case 10: case 12: return 31;
	case 4: case 6: case 9: case 11: return 30;
	case 2: return ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0) ? 29 : 28;
	default: return 0;
	}
}
static int valid_date(int32_t y, uint32_t m, uint32_t d) {
	return m >= 1 && m <= 12 && d >= 1 && d <= days_in_month(y, m);
}
static int parse_num2(ShclStr s, uint32_t *out) {
	if (!(s.n == 1 || s.n == 2) || !all_adigit0(s)) return 0;
	uint32_t v = 0; for (size_t i = 0; i < s.n; i++) v = v * 10 + (uint32_t)(s.p[i] - '0');
	*out = v; return 1;
}
static int parse_year4(ShclStr s, int32_t *out) {
	if (s.n != 4 || !all_adigit0(s)) return 0;
	int32_t v = 0; for (size_t i = 0; i < s.n; i++) v = v * 10 + (s.p[i] - '0');
	*out = v; return 1;
}
static void split_ws(ShclArena *a, ShclStr s, ShclVecS *out) {
	size_t i = 0;
	while (i < s.n) {
		uint32_t c; size_t l = utf8_decode(s.p, s.n, i, &c);
		if (is_ws(c)) { i += l; continue; }
		size_t start = i;
		while (i < s.n) { uint32_t d; size_t l2 = utf8_decode(s.p, s.n, i, &d); if (is_ws(d)) break; i += l2; }
		ShclVecS_push(a, out, s_slice(s, start, i));
	}
}
typedef struct { int ok; int32_t y; uint32_t m, d; } ShclDatePart;
static ShclDatePart parse_date_part(ShclArena *a, ShclStr s) {
	ShclDatePart r; r.ok = 0; r.y = 0; r.m = 0; r.d = 0;
	s = s_trim(s);
	if (s.n == 8 && all_adigit0(s)) {
		int32_t y; uint32_t m, d;
		if (parse_year4(s_slice(s, 0, 4), &y) && parse_num2(s_slice(s, 4, 6), &m) && parse_num2(s_slice(s, 6, 8), &d) && valid_date(y, m, d)) { r.ok = 1; r.y = y; r.m = m; r.d = d; }
		return r;
	}
	ShclVecS toks = {0}; split_ws(a, s, &toks);
	if (toks.len == 3) {
		uint32_t mm;
		if ((mm = month_from_name(a, toks.data[0]))) {
			ShclStr day_tok = toks.data[1];
			if (day_tok.n > 0 && day_tok.p[day_tok.n - 1] == ',') day_tok = s_slice(day_tok, 0, day_tok.n - 1);
			uint32_t d; int32_t y;
			/* The day is DD, like every other form's: a plain integer parse
			   takes a leading '+' and any number of leading zeros, which the
			   whitelist does not list and the delimited spellings refuse. */
			if (parse_num2(day_tok, &d) && parse_year4(toks.data[2], &y) && valid_date(y, mm, d)) { r.ok = 1; r.y = y; r.m = mm; r.d = d; }
			return r;
		}
		if ((mm = month_from_name(a, toks.data[1]))) {
			uint32_t d; int32_t y;
			if (parse_num2(toks.data[0], &d) && parse_year4(toks.data[2], &y) && valid_date(y, mm, d)) { r.ok = 1; r.y = y; r.m = mm; r.d = d; }
			return r;
		}
		return r;
	}
	if (toks.len != 1) return r;
	char delim = 0; int have = 0;
	for (size_t i = 0; i < s.n; i++) if (s.p[i] == '-' || s.p[i] == '/' || s.p[i] == '.') { delim = s.p[i]; have = 1; break; }
	if (!have) return r;
	ShclVecS parts = {0}; split_byte(a, s, delim, &parts);
	if (parts.len != 3) return r;
	for (size_t i = 0; i < parts.len; i++) if (parts.data[i].n == 0) return r;
	size_t dcount = 0; for (size_t i = 0; i < s.n; i++) if (s.p[i] == '-' || s.p[i] == '/' || s.p[i] == '.') dcount++;
	if (dcount != 2) return r;
	if (parts.data[0].n == 4 && all_adigit0(parts.data[0])) {
		int32_t y; uint32_t m, d;
		if (parse_year4(parts.data[0], &y) && parse_num2(parts.data[1], &m) && parse_num2(parts.data[2], &d) && valid_date(y, m, d)) { r.ok = 1; r.y = y; r.m = m; r.d = d; }
		return r;
	}
	uint32_t mm;
	if ((mm = month_from_name(a, parts.data[0]))) {
		uint32_t d; int32_t y;
		if (parse_num2(parts.data[1], &d) && parse_year4(parts.data[2], &y) && valid_date(y, mm, d)) { r.ok = 1; r.y = y; r.m = mm; r.d = d; }
		return r;
	}
	if ((mm = month_from_name(a, parts.data[1]))) {
		uint32_t d; int32_t y;
		if (parse_num2(parts.data[0], &d) && parse_year4(parts.data[2], &y) && valid_date(y, mm, d)) { r.ok = 1; r.y = y; r.m = mm; r.d = d; }
		return r;
	}
	return r;
}
typedef struct {
	int ok; uint32_t h, mi; int has_sec; uint32_t sec;
	int has_frac; ShclStr frac; shcl_zone_kind zone; int32_t off;
} ShclTimePart;
static uint32_t low_a(unsigned char c) { return (c >= 'A' && c <= 'Z') ? (uint32_t)(c + 32) : c; }
static ShclTimePart parse_time_part(ShclArena *a, ShclStr s) {
	ShclTimePart r; memset(&r, 0, sizeof r); r.zone = SHCL_ZONE_NONE;
	ShclStr t = s_trim(s);
	if (t.n > 0 && (t.p[t.n - 1] == 'Z' || t.p[t.n - 1] == 'z')) {
		r.zone = SHCL_ZONE_UTC; t = trim_end(s_slice(t, 0, t.n - 1));
	} else if (t.n >= 6 && ((unsigned char)t.p[t.n - 6] & 0xC0) != 0x80) {
		ShclStr tail = s_slice(t, t.n - 6, t.n);
		unsigned char sign = (unsigned char)tail.p[0];
		if ((sign == '+' || sign == '-') && is_adigit((unsigned char)tail.p[1]) && is_adigit((unsigned char)tail.p[2])
			&& tail.p[3] == ':' && is_adigit((unsigned char)tail.p[4]) && is_adigit((unsigned char)tail.p[5])) {
			int hh = (tail.p[1] - '0') * 10 + (tail.p[2] - '0');
			int mm = (tail.p[4] - '0') * 10 + (tail.p[5] - '0');
			if (hh <= 23 && mm <= 59) {
				int off = hh * 60 + mm; if (sign == '-') off = -off;
				r.zone = SHCL_ZONE_OFFSET; r.off = off;
				t = trim_end(s_slice(t, 0, t.n - 6));
			}
		}
	}
	int meridiem = -1; // 0 = AM, 1 = PM
	if (t.n >= 2 && low_a((unsigned char)t.p[t.n - 1]) == 'm' && low_a((unsigned char)t.p[t.n - 2]) == 'a') { meridiem = 0; t = s_slice(t, 0, t.n - 2); }
	else if (t.n >= 2 && low_a((unsigned char)t.p[t.n - 1]) == 'm' && low_a((unsigned char)t.p[t.n - 2]) == 'p') { meridiem = 1; t = s_slice(t, 0, t.n - 2); }
	t = trim_end(t);
	ShclStr hms = t; ShclStr frac = s_empty(); int has_frac = 0;
	for (size_t i = 0; i < t.n; i++) if (t.p[i] == '.') {
		hms = s_slice(t, 0, i); ShclStr f = s_slice(t, i + 1, t.n);
		if (f.n == 0 || f.n > 9 || !all_adigit0(f)) return r;
		frac = f; has_frac = 1; break;
	}
	ShclVecS parts = {0}; split_byte(a, hms, ':', &parts);
	if (parts.len < 2 || parts.len > 3) return r;
	if (has_frac && parts.len != 3) return r;
	uint32_t h_raw, mi;
	if (!parse_num2(parts.data[0], &h_raw)) return r;
	if (parts.data[1].n != 2 || !parse_num2(parts.data[1], &mi)) return r;
	int has_sec = 0; uint32_t sec = 0;
	if (parts.len == 3) { if (parts.data[2].n != 2 || !parse_num2(parts.data[2], &sec)) return r; has_sec = 1; }
	if (mi > 59 || (has_sec && sec > 59)) return r;
	uint32_t h;
	if (meridiem == -1) { if (h_raw > 23) return r; h = h_raw; }
	else {
		if (h_raw < 1 || h_raw > 12) return r;
		if (meridiem == 0) h = (h_raw == 12) ? 0 : h_raw;
		else h = (h_raw == 12) ? 12 : h_raw + 12;
	}
	r.ok = 1; r.h = h; r.mi = mi; r.has_sec = has_sec; r.sec = sec;
	r.has_frac = has_frac; r.frac = frac;
	return r;
}
static int parse_datetime(ShclArena *a, ShclStr text, shcl_datetime *out) {
	memset(out, 0, sizeof *out); out->zone = SHCL_ZONE_NONE;
	ShclStr t = s_trim(text);
	if (t.n == 0) return 0;
	size_t colon = (size_t)-1;
	for (size_t i = 0; i < t.n; i++) if (t.p[i] == ':') { colon = i; break; }
	if (colon != (size_t)-1) {
		size_t k = colon;
		while (k > 0 && is_adigit((unsigned char)t.p[k - 1]) && colon - k < 2) k--;
		if (k == colon) return 0;
		if (k == 0) {
			ShclTimePart tp = parse_time_part(a, t);
			if (!tp.ok) return 0;
			out->has_time = 1; out->hour = tp.h; out->minute = tp.mi;
			out->has_sec = tp.has_sec; out->sec = tp.sec;
			out->has_frac = tp.has_frac; out->frac = tp.frac; out->zone = tp.zone; out->off_min = tp.off;
			return 1;
		}
		uint32_t sepc; size_t sep_len = utf8_last(s_slice(t, 0, k), &sepc);
		if (!(sepc == 'T' || sepc == 't' || sepc == ' ' || sepc == '_' || sepc == '-' || sepc == '/' || sepc == '.')) return 0;
		ShclDatePart dp = parse_date_part(a, s_slice(t, 0, k - sep_len));
		if (!dp.ok) return 0;
		ShclTimePart tp = parse_time_part(a, s_slice(t, k, t.n));
		if (!tp.ok) return 0;
		out->has_date = 1; out->year = dp.y; out->month = dp.m; out->day = dp.d;
		out->has_time = 1; out->hour = tp.h; out->minute = tp.mi;
		out->has_sec = tp.has_sec; out->sec = tp.sec;
		out->has_frac = tp.has_frac; out->frac = tp.frac; out->zone = tp.zone; out->off_min = tp.off;
		return 1;
	}
	ShclDatePart dp = parse_date_part(a, t);
	if (!dp.ok) return 0;
	out->has_date = 1; out->year = dp.y; out->month = dp.m; out->day = dp.d;
	return 1;
}

// --- parser ------------------------------------------------------------------

/* Per-node hash-of-(name, merge-key) -> matching children. Pure lookup
   accelerator for select_or_create (the linear scan was O(children^2) per
   parent); the children vec keeps the order. Chained buckets, entries
   arena-allocated. An entry carries only the hash and a value - no key
   string is built or stored; equality past the hash is the caller's to
   verify against what the value names (merge_eq for the parser maps). Two
   different exact keys can collide in the hash, so same-hash entries keep
   insertion order (append, and the rehash preserves it) and first-inserted
   keeps winning like the scan did. A value that mutates in place (empty
   field filled, star element added) moves its entry via remap_child. */

/* The per-node accelerator slots, the reference's lazy child_map/disp_map
   shape: 8 bytes per node, NULL until the node's first entry, the map struct
   made in the parser arena on demand - an inline struct per node cost three
   times the slot and mostly held empty maps (leaves never fill one). The
   slot vector itself is parser-lifetime malloc storage, like the node vector
   and for the same reason (bump-arena doublings are never given back);
   do_parse frees both at its single exit. */
typedef struct { ShclCMap **data; size_t len, cap; } ShclVecMapPtr;
static void maps_push(jmp_buf *panic, ShclVecMapPtr *v, ShclCMap *x) {
	if (v->len == v->cap) {
		size_t nc = v->cap ? v->cap * 2 : 8;
		ShclCMap **nd = (ShclCMap **)realloc(v->data, nc * sizeof(ShclCMap *));
		if (!nd) arena_panic(panic);
		v->data = nd; v->cap = nc;
	}
	v->data[v->len++] = x;
}
static ShclCMap *map_mut(ShclArena *a, ShclVecMapPtr *v, size_t i) {
	if (!v->data[i]) { v->data[i] = (ShclCMap *)arena_alloc(a, sizeof(ShclCMap)); memset(v->data[i], 0, sizeof(ShclCMap)); }
	return v->data[i];
}

/* First entry with this hash, in insertion order; cmap_next walks the rest.
   m may be NULL: a node whose map was never created has no entries. */
static ShclCMapEnt *cmap_first(const ShclCMap *m, uint64_t h) {
	if (!m || !m->cap) return NULL;
	for (ShclCMapEnt *e = m->buckets[h & (m->cap - 1)]; e; e = e->next)
		if (e->hash == h) return e;
	return NULL;
}
static ShclCMapEnt *cmap_next(ShclCMapEnt *e, uint64_t h) {
	for (e = e->next; e; e = e->next)
		if (e->hash == h) return e;
	return NULL;
}
static void cmap_put(ShclArena *a, ShclCMap *m, uint64_t h, size_t val) {
	if (m->len + 1 > m->cap - m->cap / 4) { /* grow at 75%; also covers cap 0 */
		size_t nc = m->cap ? m->cap * 2 : 8;
		ShclCMapEnt **nb = (ShclCMapEnt **)arena_alloc(a, nc * sizeof(ShclCMapEnt *));
		memset(nb, 0, nc * sizeof(ShclCMapEnt *));
		for (size_t b = 0; b < m->cap; b++)
			for (ShclCMapEnt *e = m->buckets[b], *nx; e; e = nx) {
				nx = e->next;
				/* append, so same-hash entries keep their insertion order */
				size_t db = e->hash & (nc - 1);
				ShclCMapEnt **tail = &nb[db];
				while (*tail) tail = &(*tail)->next;
				e->next = NULL; *tail = e;
			}
		m->buckets = nb; m->cap = nc;
	}
	ShclCMapEnt *e = (ShclCMapEnt *)arena_alloc(a, sizeof *e);
	e->hash = h; e->val = val; e->next = NULL;
	ShclCMapEnt **tail = &m->buckets[h & (m->cap - 1)];
	while (*tail) tail = &(*tail)->next;
	*tail = e;
	m->len++;
}
/* Unlink the (hash, val) entry - a node holds at most one entry per map, so
   nothing else can match the pair. */
static void cmap_del(ShclCMap *m, uint64_t h, size_t val) {
	if (!m || !m->cap) return;
	for (ShclCMapEnt **pp = &m->buckets[h & (m->cap - 1)]; *pp; pp = &(*pp)->next) {
		ShclCMapEnt *e = *pp;
		if (e->hash == h && e->val == val) { *pp = e->next; m->len--; return; }
	}
}

/* A pending whole-line comment during parse: text, source indent (used only
   to decide whether it hangs on a deeper block), and the blank it consumed.
   Both strings slice the retained input copy. ceiling is the shortest
   incoming indent already checked against it: a later check can only hang it
   from a shorter one, so a longer one skips it. */
typedef struct { ShclStr text; ShclStr indent; int blank_before; size_t ceiling; } ShclPend;
DEFINE_VEC(ShclVecPend, ShclPend)
/* Every pending entry before end has a ceiling at or under indent_len. */
typedef struct { size_t end; size_t indent_len; } ShclPendMark;
DEFINE_VEC(ShclVecPendMark, ShclPendMark)

/* pending: whole-line comments waiting for the next line that binds a node.
   The source indent is kept only to decide after-attachment (a comment deeper
   than the next binding hangs on the block it sits in).
   pend_marks: lengths rise along the stack, so a hang check pops the marks
   above its own indent and walks only what they covered. Without it a run of
   retained bad lines is rewalked per line and a plain text file parses in
   quadratic time.
   star_*: a stacked list defers its merge-key remap while it is the open field
   (rebuilding the key per element is O(list^2) time); (key hash, display
   hash) at deferral start, and the deferred remap flushes before any other
   map lookup. */
/* dmaps: per-node hash-of-(name, display) -> first matching child - the
   `[value]` selector accelerator (display is a different, non-injective
   predicate from cmaps' merge key). Ownership is by hash alone and a query
   verifies its hit against the arena; same first-wins discipline, same
   mutation sites. */
// reent_node/reent_line pair up node -> line of the re-open that H002-hinted
// it (linear scan; re-opens are rare). A merge under a hinted container
// combines the same two textual regions, so it hints too even when it lands on
// the newest child at its own scope - that is how every merged level reports,
// not just the outermost. The stored line splits old children (hint) from ones
// the re-opened region itself created (silent).
/* tmp: the parser's own bookkeeping - the child accelerator, the indent stack,
   the pending-comment list, the line index - is dead the moment parsing ends,
   so it goes in the scratch arena rather than the document's. In a bump arena
   the document's is never reclaimed, and the accelerator alone is one hash map
   per node. Everything a node keeps (name, value, trivia text) is still dup'd
   into the document arena. Nothing resets scratch during a parse; the first
   read after it does. */
/* What a parse owns outright and has to give back, on the heap rather than in
   do_parse's frame: the recovery path is reached by longjmp, which leaves a
   local the parse has written to indeterminate. */
typedef struct { ShclArena line, hints; ShclVecMapPtr cmaps, dmaps; } ShclParseOwn;
typedef struct { shcl_doc *d; ShclArena *tmp; ShclArena *line; ShclArena *hints; ShclStr src; ShclVecStack stack; ShclVecMapPtr *cmaps; ShclVecMapPtr *dmaps; ShclVecPend pending; ShclVecPendMark pend_marks; int star_open; size_t star_node; uint64_t star_key; uint64_t star_disp; int saw_blank; ShclVecSize reent_node; ShclVecSize reent_line;
	/* shcl_parse_limited's caps, 0 = uncapped: nodes counted against the
	   arena (root excluded), elements against a single value's cell. */
	size_t max_nodes, max_elements;
	/* Diagnostic cap: past it nothing is listed, only counted (errors, hints),
	   for the one tail entry the parse ends with. */
	size_t max_diags, unlisted_errors, unlisted_hints; } ShclParser;

static void push_diag(shcl_doc *d, size_t line, shcl_severity sev, const char *code, ShclStr msg) {
	ShclDiag dg; dg.line = line; dg.sev = sev; dg.message = msg; dg.code = code;
	ShclVecDiag_push(&d->arena, &d->diags, dg);
}
/* Every parse diagnostic goes through here, so the cap sees them all. A
   message is built in the per-line arena and copied into the document only
   when listed, so an unlisted one costs nothing past its own line. */
static void p_diag(ShclParser *P, size_t line, shcl_severity sev, const char *code, ShclStr msg) {
	if (P->max_diags && P->d->diags.len >= P->max_diags) {
		if (sev == SHCL_SEV_ERROR) P->unlisted_errors++; else P->unlisted_hints++;
		return;
	}
	push_diag(P->d, line, sev, code, s_dup(&P->d->arena, msg));
}
static void p_err(ShclParser *P, size_t line, const char *code, ShclStr msg) { p_diag(P, line, SHCL_SEV_ERROR, code, msg); }

static void remap_child(ShclParser *P, size_t node, uint64_t old_key, uint64_t old_disp);
static size_t find_by_value(ShclParser *P, size_t cur, ShclStr name, ShclStr text, int quoted);

/* Apply a stacked list's deferred merge-key remap. Runs before any map lookup
   (and at end of parse), so the map is always fresh when queried. */
static void star_flush(ShclParser *P) {
	if (!P->star_open) return;
	P->star_open = 0;
	remap_child(P, P->star_node, P->star_key, P->star_disp);
}

static size_t select_or_create(ShclParser *P, size_t parent, ShclStr name, ShclStr name_src, ShclValue value, size_t line) {
	ShclArena *a = &P->d->arena;
	star_flush(P);
	uint64_t h = merge_hash(name, &value);
	for (ShclCMapEnt *e = cmap_first(P->cmaps->data[parent], h); e; e = cmap_next(e, h))
		if (merge_eq(NODE(P->d, e->val).name, &NODE(P->d, e->val).value, name, &value)) return e->val;
	size_t idx = P->d->nodes.len;
	ShclNode n; memset(&n, 0, sizeof n);
	n.name = s_keep(a, P->src, name);
	n.name_src = s_eq(name_src, name) ? s_empty() : s_keep(a, P->src, name_src);
	n.value = value; n.parent = parent; n.line = line; n.star_list = 0; n.star_mixed = 0;
	nodes_push(P->d, n);
	ShclVecSize_push(a, &NODE(P->d, parent).children, idx);
	maps_push(P->d->panic, P->cmaps, NULL);
	maps_push(P->d->panic, P->dmaps, NULL);
	cmap_put(P->tmp, map_mut(P->tmp, P->cmaps, parent), h, idx);
	uint64_t hd = disp_hash(name, &value);
	if (!cmap_first(P->dmaps->data[parent], hd))
		cmap_put(P->tmp, map_mut(P->tmp, P->dmaps, parent), hd, idx);
	return idx;
}

/* A node's value mutated in place: move its map entry from the old key to the
   new one. First-wins on both sides so lookups keep matching the earliest
   sibling, like the scan did. */
static void remap_child(ShclParser *P, size_t node, uint64_t old_key, uint64_t old_disp) {
	size_t parent = NODE(P->d, node).parent;
	ShclStr name = NODE(P->d, node).name;
	cmap_del(P->cmaps->data[parent], old_key, node);
	uint64_t h = merge_hash(name, &NODE(P->d, node).value);
	int already = 0;
	for (ShclCMapEnt *e = cmap_first(P->cmaps->data[parent], h); e; e = cmap_next(e, h))
		if (merge_eq(NODE(P->d, e->val).name, &NODE(P->d, e->val).value, name, &NODE(P->d, node).value)) { already = 1; break; }
	if (!already) cmap_put(P->tmp, map_mut(P->tmp, P->cmaps, parent), h, node);
	cmap_del(P->dmaps->data[parent], old_disp, node);
	uint64_t hd = disp_hash(name, &NODE(P->d, node).value);
	if (!cmap_first(P->dmaps->data[parent], hd)) cmap_put(P->tmp, map_mut(P->tmp, P->dmaps, parent), hd, node);
}

/* A value that mutates after its sibling group was keyed - an empty field
   filled by a fence, a stacked list closed - can land on a key an earlier
   sibling already holds, which the keyed lookup can no longer catch. Fold
   those pairs so the tree matches a reparse of its own canonical text.
   Depth-first, since folding can carry duplicates down a level. Grouping
   temporaries live in scratch (dead before the first resolve resets it). */
static void fold_late_dups(ShclParser *P) {
	shcl_doc *d = P->d;
	ShclArena *t = &d->scratch;
	ShclVecSize stack = {0};
	ShclVecSize_push(t, &stack, ROOT);
	while (stack.len) {
		size_t parent = stack.data[--stack.len];
		ShclCMap first; memset(&first, 0, sizeof first);
		ShclVecSize *ch = &NODE(d, parent).children;
		size_t w = 0;
		for (size_t k = 0; k < ch->len; k++) {
			size_t c = ch->data[k];
			uint64_t h = merge_hash(NODE(d, c).name, &NODE(d, c).value);
			size_t survivor = (size_t)-1;
			for (ShclCMapEnt *e = cmap_first(&first, h); e; e = cmap_next(e, h))
				if (merge_eq(NODE(d, e->val).name, &NODE(d, e->val).value, NODE(d, c).name, &NODE(d, c).value)) { survivor = e->val; break; }
			if (survivor != (size_t)-1) fold_node_into(d, survivor, c);
			else {
				cmap_put(t, &first, h, c);
				ch->data[w++] = c;
			}
		}
		ch->len = w;
		for (size_t k = 0; k < ch->len; k++) ShclVecSize_push(t, &stack, ch->data[k]);
	}
}

/* Hand pending leading comments (and this line's trailing one) to a node.
   First trailing wins; a later one demotes to leading so nothing is lost.
   Comment text is stored verbatim, so pending and trailing alike are slices
   of the retained input copy - nothing to duplicate. */
static void attach_trivia(ShclParser *P, size_t node, ShclStr trailing) {
	ShclArena *a = &P->d->arena;
	if (P->pending.len) {
		ShclTrivia *t = triv_mut(a, &NODE(P->d, node));
		for (size_t k = 0; k < P->pending.len; k++) {
			const ShclPend *p = &P->pending.data[k];
			ShclVecLead_push(a, &t->leading, lead_make(p->text, p->blank_before));
		}
		P->pending.len = 0;
		P->pend_marks.len = 0;
	}
	if (trailing.n) {
		ShclTrivia *t = triv_mut(a, &NODE(P->d, node));
		if (t->trailing.n == 0) t->trailing = trailing;
		else ShclVecLead_push(a, &t->leading, lead_plain(trailing));
	}
}

/* Comments written deeper than the incoming line belong to the block they sit
   in, not to the next binding: hang each on the deepest node whose bound
   indent prefixes the comment's, among the levels the incoming line is
   closing. Written at that node's own level the comment trails it (`after`);
   written deeper it sits inside the node's block (`inside`) - so a header
   whose children are all commented still owns them at their depth. Runs
   before the incoming line resolves (and at end of parse with the empty
   indent, so tail comments keep their block). */
static void hang_deeper_pending(ShclParser *P, ShclStr new_indent) {
	if (P->pending.len == 0) return;
	ShclArena *a = &P->d->arena;
	/* Only entries above the last mark at or under this indent can hang. */
	while (P->pend_marks.len && P->pend_marks.data[P->pend_marks.len - 1].indent_len > new_indent.n) P->pend_marks.len--;
	size_t w = P->pend_marks.len ? P->pend_marks.data[P->pend_marks.len - 1].end : 0;
	for (size_t k = w; k < P->pending.len; k++) {
		ShclPend p = P->pending.data[k];
		if (p.ceiling > new_indent.n) {
			/* A level shallower than the incoming line stays open and may
			   still gain children, so a comment must not hang there - it
			   would emit below the child; keep it pending instead. */
			size_t target = (size_t)-1; int at_own_level = 0;
			for (size_t ii = P->stack.len; ii-- > 0;) {
				ShclStr ind = P->stack.data[ii].indent; size_t n = P->stack.data[ii].node;
				if (n != ROOT && n != DEAD && n != UNOPENED && ind.n >= new_indent.n && p.indent.n >= ind.n && memcmp(p.indent.p, ind.p, ind.n) == 0) { target = n; at_own_level = ind.n == p.indent.n; break; }
			}
			/* A root node's trailing comment emits at column zero, which is
			   exactly how the document's own trailing comment is spelled, so
			   keeping the two apart here made a merge depend on whether the
			   layer had been formatted first. Let it orphan, the way a reload
			   of this document's own output reads it. A comment deeper than the
			   node keeps an indent of its own and comes back where it was, so
			   it still hangs. */
			if (target != (size_t)-1 && (!at_own_level || NODE(P->d, target).parent != ROOT)) {
				ShclLead lead = lead_make(p.text, p.blank_before);
				ShclTrivia *t = triv_mut(a, &NODE(P->d, target));
				if (at_own_level) ShclVecLead_push(a, &t->after, lead);
				else ShclVecLead_push(a, &t->inside, lead);
				continue;
			}
			p.ceiling = new_indent.n;
		}
		P->pending.data[w++] = p;
	}
	P->pending.len = w;
	if (P->pend_marks.len && P->pend_marks.data[P->pend_marks.len - 1].indent_len == new_indent.n) P->pend_marks.data[P->pend_marks.len - 1].end = w;
	else { ShclPendMark m; m.end = w; m.indent_len = new_indent.n; ShclVecPendMark_push(P->tmp, &P->pend_marks, m); }
}

static int resolve_parent(ShclParser *P, ShclStr indent, size_t *out) {
	size_t top = P->stack.len - 1;
	ShclStr ti = P->stack.data[top].indent; size_t tn = P->stack.data[top].node;
	if (indent.n > ti.n && (ti.n == 0 || memcmp(indent.p, ti.p, ti.n) == 0)) { *out = tn == UNOPENED ? DEAD : tn; return 1; }
	for (size_t ii = P->stack.len; ii-- > 0;) {
		if (s_eq(P->stack.data[ii].indent, indent) && P->stack.data[ii].node != UNOPENED) {
			size_t parent = (ii == 0) ? ROOT : P->stack.data[ii - 1].node;
			*out = parent == UNOPENED ? DEAD : parent;
			P->stack.len = ii ? ii : 1;
			return 1;
		}
	}
	/* Skipped, but it still owns its indent: whatever is written deeper is
	   skipped with it, and a sibling at the same bad indent is refused the
	   same way instead of binding one level up. */
	while (P->stack.len > 1) {
		ShclStr top_indent = P->stack.data[P->stack.len - 1].indent;
		if (indent.n > top_indent.n && (top_indent.n == 0 || memcmp(indent.p, top_indent.p, top_indent.n) == 0)) break;
		P->stack.len--;
	}
	{ ShclStackEnt se; se.indent = indent; se.node = UNOPENED; ShclVecStack_push(P->tmp, &P->stack, se); }
	return 0;
}

/* Diagnose a line written under a skipped line, and skip it too. Its own
   level stays dead so deeper lines go the same way. */
static void skip_under_dead(ShclParser *P, size_t line, ShclStr indent) {
	p_err(P, line, "E018", s_lit("parent line was skipped; line skipped"));
	P->d->lost++;
	ShclStackEnt se; se.indent = indent; se.node = DEAD; ShclVecStack_push(P->tmp, &P->stack, se);
}

/* The single H002 wording site: the merge hint and the schema suppressor
   both come here, same discipline as h001_head. */
static ShclStr h002_head(ShclArena *a, ShclStr name) {
	ShclSB s = {0};
	sb_puts(a, &s, "merged with '"); sb_putS(a, &s, name); sb_puts(a, &s, "' at ");
	return sb_S(&s);
}
static size_t reent_get(const ShclParser *P, size_t node) {
	for (size_t i = 0; i < P->reent_node.len; i++)
		if (P->reent_node.data[i] == node) return P->reent_line.data[i];
	return 0;
}
static void reent_set(ShclParser *P, size_t node, size_t line) {
	for (size_t i = 0; i < P->reent_node.len; i++)
		if (P->reent_node.data[i] == node) { P->reent_line.data[i] = line; return; }
	ShclVecSize_push(P->tmp, &P->reent_node, node); ShclVecSize_push(P->tmp, &P->reent_line, line);
}
static int attach_path(ShclParser *P, size_t parent, ShclSegment *segs, size_t nsegs, ShclValue value, size_t line, size_t *out) {
	ShclArena *a = &P->d->arena;
	/* Field child under a stacked list: diagnose the mix once, keep the field. */
	star_flush(P);
	if (NODE(P->d, parent).star_list && !NODE(P->d, parent).star_mixed) {
		NODE(P->d, parent).star_mixed = 1;
		p_err(P, line, "E001", s_lit("field mixed with list elements"));
	}
	/* Nesting cap: parent depth plus the segments this line adds. Checked
	   before any node is created so a rejected line leaves nothing behind. */
	size_t parent_depth = 0;
	for (size_t up = parent; up != ROOT; up = NODE(P->d, up).parent) parent_depth++;
	if (parent_depth + nsegs > SHCL_MAX_DEPTH) {
		ShclSB m = {0}; sb_puts(P->line, &m, "nesting deeper than "); sb_put_u64(P->line, &m, SHCL_MAX_DEPTH); sb_puts(P->line, &m, " levels; line skipped");
		p_err(P, line, "E016", sb_S(&m));
		P->d->lost++;
		return 0;
	}
	size_t cur = parent;
	for (size_t i = 0; i < nsegs; i++) {
		const ShclSegment *seg = &segs[i];
		int is_last = (i + 1 == nsegs);
		switch (seg->sel.tag) {
		case SEL_VALUE: {
			/* Same escape-applied display predicate resolution uses, so a
			   selector also selects an array-valued instance instead of
			   creating a spurious second one - via the dmaps accelerator (the
			   inline spelling was quadratic in siblings without it). Create
			   only when nothing matches. */
			size_t found = find_by_value(P, cur, seg->name, seg->sel.value, seg->sel.quoted);
			if (found != (size_t)-1) {
				cur = found;
			} else {
				ShclValue disc; memset(&disc, 0, sizeof disc); disc.kind = V_CELL;
				ShclElement *e = (ShclElement *)arena_alloc(a, sizeof(ShclElement));
				e->text = s_keep(a, P->src, seg->sel.value); e->quoted = 0;
				disc.els = e; disc.nels = 1;
				cur = select_or_create(P, cur, seg->name, seg->name_src, disc, line);
			}
			if (is_last && !v_is_empty(&value)) {
				ShclSB m = {0}; sb_puts(P->line, &m, "value after selector on '"); sb_putS(P->line, &m, seg->name); sb_puts(P->line, &m, "' ignored");
				p_err(P, line, "E002", sb_S(&m));
				P->d->lost++;
			}
			break;
		}
		case SEL_INDEX: {
			size_t found = (size_t)-1, nth = 0; ShclVecSize ch = NODE(P->d, cur).children;
			for (size_t k = 0; k < ch.len; k++) {
				size_t c = ch.data[k];
				if (!s_eq(NODE(P->d, c).name, seg->name)) continue;
				if (nth == seg->sel.index) { found = c; break; }
				nth++;
			}
			if (found != (size_t)-1) cur = found;
			else {
				ShclSB m = {0}; sb_puts(P->line, &m, "no instance "); sb_put_u64(P->line, &m, seg->sel.index); sb_puts(P->line, &m, " of '"); sb_putS(P->line, &m, seg->name); sb_putc(P->line, &m, '\'');
				p_err(P, line, "E003", sb_S(&m)); P->d->lost++; return 0;
			}
			/* Same as the value selector: the instance is already chosen, so a
			   trailing value has nowhere to bind. */
			if (is_last && !v_is_empty(&value)) {
				ShclSB m = {0}; sb_puts(P->line, &m, "value after selector on '"); sb_putS(P->line, &m, seg->name); sb_puts(P->line, &m, "' ignored");
				p_err(P, line, "E002", sb_S(&m));
				P->d->lost++;
			}
			break;
		}
		case SEL_WILDCARD:
			p_err(P, line, "E004", s_lit("wildcard selector is query-only")); P->d->lost++; return 0;
		case SEL_NONE: {
			size_t seg_parent = cur;
			size_t before = P->d->nodes.len;
			cur = select_or_create(P, cur, seg->name, seg->name_src, is_last ? value : v_empty(), line);
			/* Two separately-written bindings just combined: legal (the
			   merge rule), but only the parser can see it happened, so
			   say so. Adjacent re-mentions (still the newest binding at
			   this scope) and selector/path-intermediate merges stay
			   silent - those are the deliberate redundant-path idiom.
			   Under a hinted container the newest-child pass does not
			   apply to children the earlier region wrote: those merges
			   combine the same two regions, so every level reports. */
			if (is_last && cur < before && NODE(P->d, cur).line != line) {
				ShclVecSize sib = NODE(P->d, seg_parent).children;
				int non_last = (sib.len == 0 || sib.data[sib.len - 1] != cur);
				size_t rl = reent_get(P, seg_parent);
				int cross_region = (rl != 0 && NODE(P->d, cur).line < rl);
				if (non_last || cross_region) {
					ShclSB m = {0};
					sb_putS(P->line, &m, h002_head(P->line, seg->name));
					sb_puts(P->line, &m, "line "); sb_put_u64(P->line, &m, NODE(P->d, cur).line);
					sb_puts(P->line, &m, " (same name and value combine)");
					p_diag(P, line, SHCL_SEV_HINT, "H002", sb_S(&m));
					reent_set(P, cur, line);
				}
			}
			break;
		}
		}
	}
	*out = cur; return 1;
}

/* The child of `cur` named `name` whose display form is the selector text
   (escapes applied), or (size_t)-1. Quoted selectors only match a single
   scalar. */
static size_t find_by_value(ShclParser *P, size_t cur, ShclStr name, ShclStr text, int quoted) {
	ShclStr want = apply_escapes(P->line, text);
	uint64_t hd = disp_hash_text(name, want);
	size_t found = (size_t)-1;
	/* Ownership in dmaps is by hash alone, so the one candidate is verified
	   exactly against the arena; a failed verify is a miss. */
	{
		ShclCMapEnt *e = cmap_first(P->dmaps->data[cur], hd);
		if (e && s_eq(NODE(P->d, e->val).name, name)
			&& s_eq(disp_key(P->line, &NODE(P->d, e->val).value), want))
			found = e->val;
	}
	/* A quoted selector is scalar-only, so it is the one that needs the
	   fallback scan: the accelerator keeps just the first same-display
	   child, which may be the non-scalar one. An unquoted selector takes
	   whatever the accelerator holds and does not scan, so it can bind a raw
	   block where a quoted selector picks the scalar sibling. */
	if (found != (size_t)-1 && quoted && !single_scalar(&NODE(P->d, found).value)) {
		found = (size_t)-1;
	}
	if (found == (size_t)-1 && quoted) {
		ShclVecSize ch = NODE(P->d, cur).children;
		for (size_t k = 0; k < ch.len; k++) {
			size_t c = ch.data[k];
			if (s_eq(NODE(P->d, c).name, name) && single_scalar(&NODE(P->d, c).value) && s_eq(disp_key(P->line, &NODE(P->d, c).value), want)) { found = c; break; }
		}
	}
	return found;
}

/* Consume raw-block content after an opening fence. Returns the value; *next
   gets the next line index. The closing fence's indent is stripped from each
   content line (the opening line's when the block never closes); the rest is
   content. */
static ShclValue consume_raw(ShclParser *P, const ShclStr *lines, size_t nlines, size_t i, size_t open_line, ShclStr open_indent, ShclFence fence, size_t *next) {
	ShclArena *a = &P->d->arena;
	unsigned char ch = fence.ch; size_t len = fence.len; ShclStr info = fence.info;
	ShclVecS content = {0}; int closed = 0; /* line list: parse-lifetime temporary */
	ShclStr nest = open_indent;
	while (i < nlines) {
		if (is_fence_close(lines[i], ch, len)) {
			/* The closing fence's indent is the nesting; everything a content
			   line carries past it is content, so a body whose lines all
			   share an indent keeps it (a writer-built block depends on that). */
			nest = leading_ws(lines[i]);
			closed = 1; i++; break;
		}
		ShclVecS_push(P->tmp, &content, lines[i]); i++;
	}
	if (!closed) p_err(P, open_line, "E005", s_lit("unterminated raw block"));
	ShclSB out = {0};
	for (size_t k = 0; k < content.len; k++) {
		if (k) sb_putc(a, &out, '\n');
		sb_putS(a, &out, strip_common(content.data[k], nest));
	}
	ShclValue v; memset(&v, 0, sizeof v);
	v.kind = V_RAW;
	v.raw = (ShclRawVal *)arena_alloc(a, sizeof(ShclRawVal));
	v.raw->content = sb_S(&out); v.raw->info = info; v.raw->fence_char = ch; v.raw->fence_len = len;
	*next = i;
	return v;
}

/* Returns the node the block landed on ((size_t)-1 = no parent, diagnosed). */
static size_t bind_block(ShclParser *P, size_t parent, ShclValue value, size_t line) {
	if (parent == ROOT) { p_err(P, line, "E006", s_lit("raw block with no parent field")); P->d->lost++; return (size_t)-1; }
	if (v_is_empty(&NODE(P->d, parent).value)) {
		uint64_t old_key = merge_hash(NODE(P->d, parent).name, &NODE(P->d, parent).value);
		uint64_t old_disp = disp_hash(NODE(P->d, parent).name, &NODE(P->d, parent).value);
		NODE(P->d, parent).value = value;
		remap_child(P, parent, old_key, old_disp);
		return parent;
	}
	ShclStr name = NODE(P->d, parent).name; ShclStr name_src = node_authored(&NODE(P->d, parent)); size_t gp = NODE(P->d, parent).parent;
	return select_or_create(P, gp, name, name_src, value, line);
}

/* 1 when the element was added, 0 when the line was dropped. */
static int add_star_element(ShclParser *P, size_t parent, ShclStr body, size_t line) {
	ShclArena *a = &P->d->arena;
	if (parent == ROOT) { p_err(P, line, "E007", s_lit("list element with no parent field")); P->d->lost++; return 0; }
	/* Uniform-or-nothing (spec): a mix with field children is not a block array. */
	if (NODE(P->d, parent).children.len != 0) { p_err(P, line, "E008", s_lit("list element mixed with field children; ignored")); P->d->lost++; return 0; }
	ShclStr trimmed = s_trim_wsp(body);
	if (trimmed.n == 0) { p_err(P, line, "E009", s_lit("empty list element")); P->d->lost++; return 0; }
	if (count_unquoted_pieces(trimmed) > 1) { p_err(P, line, "E010", s_lit("bare comma in list element (one element per line)")); P->d->lost++; return 0; }
	if (unterminated_quote(P->line, trimmed)) p_err(P, line, "E017", s_lit("unterminated quote in value"));
	ShclElement el;
	if (!parse_element(a, trimmed, &el)) { p_err(P, line, "E009", s_lit("empty list element")); P->d->lost++; return 0; }
	/* Element cap: each element line past it is refused on its own, the way
	   any other bad element line is. */
	if (P->max_elements && NODE(P->d, parent).value.kind == V_CELL && NODE(P->d, parent).value.nels >= P->max_elements) {
		ShclSB m = {0}; sb_puts(P->line, &m, "array longer than "); sb_put_u64(P->line, &m, P->max_elements); sb_puts(P->line, &m, " elements; line skipped");
		p_err(P, line, "E021", sb_S(&m));
		P->d->lost++;
		return 0;
	}
	ShclNode *node = &NODE(P->d, parent);
	if (node->value.kind == V_EMPTY) {
		uint64_t old_key = merge_hash(node->name, &node->value);
		uint64_t old_disp = disp_hash(node->name, &node->value);
		/* Seed capacity for geometric growth: a fresh full-size copy per `* `
		   line kept every discarded copy in the arena - quadratic memory. */
		ShclElement *arr = (ShclElement *)arena_alloc(a, 4 * sizeof(ShclElement)); arr[0] = el;
		node->value.kind = V_CELL; node->value.els = arr; node->value.nels = 1; node->value.cap_els = 4;
		node->star_list = 1;
		remap_child(P, parent, old_key, old_disp);
		/* Defer further remaps until the list closes; the map entry made above
		   stays valid because nothing can look this node up until a non-star
		   line binds (which flushes first). */
		P->star_open = 1; P->star_node = parent; P->star_key = merge_hash(node->name, &node->value); P->star_disp = disp_hash(node->name, &node->value);
	} else if (node->value.kind == V_CELL && node->star_list) {
		if (!P->star_open || P->star_node != parent) {
			star_flush(P);
			P->star_open = 1; P->star_node = parent; P->star_key = merge_hash(node->name, &node->value); P->star_disp = disp_hash(node->name, &node->value);
		}
		if (node->value.nels == node->value.cap_els) {
			size_t nc = node->value.cap_els ? node->value.cap_els * 2 : 4;
			ShclElement *arr = (ShclElement *)arena_alloc(a, nc * sizeof(ShclElement));
			memcpy(arr, node->value.els, node->value.nels * sizeof(ShclElement));
			node->value.els = arr; node->value.cap_els = nc;
		}
		node->value.els[node->value.nels++] = el;
	} else {
		p_err(P, line, "E011", s_lit("field already has a value; list element ignored"));
		P->d->lost++;
		return 0;
	}
	return 1;
}

/* The single H001 wording site: the hint builder and the schema suppressor
   both come here, so the suppressor matches the exact head the builder
   emitted - never a re-parse of free prose. (The leaf name cannot ride on
   the diagnostic itself: consumers build diagnostics literally, so the field
   set is frozen.) */
static ShclStr h001_head(ShclArena *a, ShclStr name) {
	ShclSB s = {0};
	sb_putc(a, &s, '\'');
	sb_putS(a, &s, name);
	sb_puts(a, &s, "' repeats as a bare leaf - did you mean '");
	sb_putS(a, &s, name);
	sb_puts(a, &s, ": ");
	return sb_S(&s);
}

static void emit_repeated_leaf_hints(ShclParser *P) {
	ShclArena *a = &P->d->arena;
	/* Grouping bookkeeping (name buckets, member lists, joined displays) is
	   dead on return, so it lives in its own arena, freed here - built in the
	   document arena it cost several times the hints it found and could never
	   be given back. Only the hint messages land in the document arena. The
	   parse owns it rather than this frame, so an allocation failure - which
	   unwinds straight out of here - still has something to free it with. */
	ShclArena *tmp = P->hints;
	arena_guard(tmp, a->panic);
	for (size_t parent = 0; parent < P->d->nodes.len; parent++) {
		ShclVecS names = {0}; ShclVecSize *groups = NULL; size_t ngroups = 0, cgroups = 0;
		ShclCMap group_of; memset(&group_of, 0, sizeof group_of);
		ShclVecSize ch = NODE(P->d, parent).children;
		for (size_t k = 0; k < ch.len; k++) {
			size_t c = ch.data[k]; ShclStr nm = NODE(P->d, c).name;
			uint64_t h = cmap_hash(nm, s_empty());
			size_t g = (size_t)-1;
			for (ShclCMapEnt *e = cmap_first(&group_of, h); e; e = cmap_next(e, h))
				if (s_eq(names.data[e->val], nm)) { g = e->val; break; }
			if (g == (size_t)-1) {
				ShclVecS_push(tmp, &names, nm);
				if (ngroups == cgroups) { size_t nc = cgroups ? cgroups * 2 : 8; groups = (ShclVecSize *)arena_grow(tmp, groups, cgroups, nc, sizeof(ShclVecSize)); cgroups = nc; }
				memset(&groups[ngroups], 0, sizeof(ShclVecSize)); g = ngroups++;
				cmap_put(tmp, &group_of, h, g);
			}
			ShclVecSize_push(tmp, &groups[g], c);
		}
		for (size_t gi = 0; gi < ngroups; gi++) {
			ShclVecSize grp = groups[gi];
			if (grp.len < 2) continue;
			int all_scalar = 1; size_t maxline = 0;
			for (size_t k = 0; k < grp.len; k++) {
				size_t c = grp.data[k];
				if (!(NODE(P->d, c).children.len == 0 && NODE(P->d, c).value.kind == V_CELL && !NODE(P->d, c).star_list)) { all_scalar = 0; break; }
				if (NODE(P->d, c).line > maxline) maxline = NODE(P->d, c).line;
			}
			if (!all_scalar) continue;
			ShclSB joined = {0};
			for (size_t k = 0; k < grp.len; k++) { if (k) sb_puts(tmp, &joined, ", "); sb_putS(tmp, &joined, value_display(tmp, &NODE(P->d, grp.data[k]).value)); }
			ShclSB m = {0}; sb_putS(tmp, &m, h001_head(tmp, names.data[gi])); sb_putS(tmp, &m, sb_S(&joined)); sb_puts(tmp, &m, "'?");
			p_diag(P, maxline, SHCL_SEV_HINT, "H001", sb_S(&m));
		}
	}
	arena_free(tmp);
}

/* The parse proper. Split from do_parse so that no local of a function holding
   a setjmp is written after it: which of those a compiler thinks an unwind
   could clobber varies by version and optimization level, and -Wclobbered is
   an error here. */
static void parse_body(shcl_doc *d, ShclParseOwn *own, const char *text, size_t len, size_t max_nodes, size_t max_elements, size_t max_diags) {
	ShclArena *a = &d->arena;
	ShclNode root; memset(&root, 0, sizeof root); root.value = v_empty(); root.parent = 0; root.line = 0;
	nodes_push(d, root);
	/* Per-line temporaries - the path scan above all, which allocates a segment
	   vector for every line parsed - reset at the top of each iteration. They
	   cannot share the scratch arena: that one carries the parser's bookkeeping
	   for the whole parse. Everything a node keeps is dup'd into the document
	   arena before the next reset. */
	ShclParser P; P.d = d; P.tmp = &d->scratch; P.line = &own->line; P.hints = &own->hints; P.cmaps = &own->cmaps; P.dmaps = &own->dmaps; memset(&P.stack, 0, sizeof P.stack); memset(&P.pending, 0, sizeof P.pending); memset(&P.pend_marks, 0, sizeof P.pend_marks);
	P.star_open = 0; P.star_node = 0; P.star_key = 0; P.star_disp = 0; P.saw_blank = 0;
	P.max_nodes = max_nodes; P.max_elements = max_elements; P.max_diags = max_diags; P.unlisted_errors = 0; P.unlisted_hints = 0;
	memset(&P.reent_node, 0, sizeof P.reent_node); memset(&P.reent_line, 0, sizeof P.reent_line);
	ShclStackEnt e0; e0.indent = s_empty(); e0.node = ROOT; ShclVecStack_push(P.tmp, &P.stack, e0);
	maps_push(d->panic, P.cmaps, NULL);
	maps_push(d->panic, P.dmaps, NULL);

	ShclStr full; full.p = text ? text : ""; full.n = len;
	if (full.n >= 3 && (unsigned char)full.p[0] == 0xEF && (unsigned char)full.p[1] == 0xBB && (unsigned char)full.p[2] == 0xBF) full = s_slice(full, 3, full.n);
	/* The whole input, retained once in the document arena. Every stored
	   string below is either a slice of this copy (names, element texts,
	   comments, raw info) or built beside it, so nothing references the
	   caller's buffer and per-piece duplication disappears. */
	full = s_dup(a, full);
	P.src = full;
	ShclVecS lines = {0};
	{
		size_t start = 0;
		for (size_t i = 0; i <= full.n; i++) {
			if (i == full.n || full.p[i] == '\n') {
				/* A newline-terminated text splits into one more piece than it
				   has lines. An unterminated raw block took that empty tail as
				   a body line, so the same last line read differently with and
				   without its newline, which the grammar says are one document. */
				if (i == full.n && start == full.n && full.n) break;
				ShclStr l = s_slice(full, start, i);
				/* The whole trailing CR run goes, not just one: a raw block keeps its
				   content untrimmed, so a line left ending in CR would be written back
				   as CRLF and read as neither - the one shape where the count shows. */
				while (l.n > 0 && l.p[l.n - 1] == '\r') l.n--;
				ShclVecS_push(P.tmp, &lines, l);
				start = i + 1;
			}
		}
	}
	size_t i = 0;
	int node_capped = 0;
	while (i < lines.len) {
		/* Node cap: reported at the first line not parsed, so the count can
		   overshoot by at most one line's path. The unparsed remainder counts
		   as lost, which is what keeps shcl_save_file from writing a silently
		   truncated document. */
		if (P.max_nodes && d->nodes.len - 1 > P.max_nodes) {
			ShclSB m = {0}; sb_puts(P.line, &m, "node cap of "); sb_put_u64(P.line, &m, P.max_nodes); sb_puts(P.line, &m, " exceeded; parse stopped");
			p_err(&P, i + 1, "E020", sb_S(&m));
			for (size_t r = i; r < lines.len; r++) if (s_trim_wsp(lines.data[r]).n) d->lost++;
			node_capped = 1;
			break;
		}
		arena_reset(&own->line);
		size_t lineno = i + 1;
		ShclStr line = trim_wsp_end(lines.data[i]);
		size_t ind = 0; while (ind < line.n && (line.p[ind] == ' ' || line.p[ind] == '\t')) ind++;
		ShclStr indent = s_slice(line, 0, ind);
		ShclStr rest = s_slice(line, ind, line.n);
		if (rest.n == 0) { P.saw_blank = 1; i++; continue; }
		/* Whole-line comment: hold it for the next line that binds a node. It
		   consumes a pending blank into its own flag, so a blank between
		   comment-only regions survives the round-trip. Text and indent are
		   slices of the retained input copy, so they store as-is. */
		if (rest.p[0] == '#') {
			ShclPend pd; pd.text = rest; pd.indent = indent; pd.blank_before = P.saw_blank; pd.ceiling = indent.n; P.saw_blank = 0;
			ShclVecPend_push(P.tmp, &P.pending, pd);
			i++; continue;
		}
		/* Any other line consumes the pending blank; only a field line that
		   binds turns it into grouping. */
		int had_blank = P.saw_blank; P.saw_blank = 0;
		/* A binding line claims the pending comments - but deeper-written ones
		   hang on their own block first. */
		hang_deeper_pending(&P, indent);
		ShclFence f = fence_open(rest);
		if (f.ok) {
			size_t parent;
			int resolved = resolve_parent(&P, indent, &parent);
			size_t next; ShclValue val = consume_raw(&P, lines.data, lines.len, i + 1, lineno, indent, f, &next);
			/* The body goes with its fence: parsed live, it would read as root
			   bindings and the closing fence would open a second block. */
			if (!resolved) { p_err(&P, lineno, "E012", s_lit("indentation matches no open level")); d->lost++; i = next; continue; }
			if (parent == DEAD) skip_under_dead(&P, lineno, indent);
			else {
				size_t bnode = bind_block(&P, parent, val, lineno);
				if (bnode != (size_t)-1) attach_trivia(&P, bnode, s_empty());
			}
			i = next; continue;
		}
		if (rest.n >= 1 && rest.p[0] == '*') {
			ShclStr after = s_slice(rest, 1, rest.n);
			/* A `*` alone after the trim: whether a space followed it decides
			   between an empty element and a malformed line, and only the
			   untrimmed line still knows. */
			int spaced = after.n >= 1 && (after.p[0] == ' ' || after.p[0] == '\t');
			if (after.n == 0 && lines.data[i].n > indent.n + 1) spaced = lines.data[i].p[indent.n + 1] == ' ' || lines.data[i].p[indent.n + 1] == '\t';
			if (spaced) {
				size_t parent;
				if (!resolve_parent(&P, indent, &parent)) { p_err(&P, lineno, "E012", s_lit("indentation matches no open level")); d->lost++; i++; continue; }
				if (parent == DEAD) { skip_under_dead(&P, lineno, indent); i++; continue; }
				ShclStr ecomment; ShclStr body = split_value_comment(after, &ecomment);
				/* Elements have no node of their own; trivia rides the field. At the
				   root there is no field (E007), so the comment rides the document
				   like any other pending one. */
				if (parent != ROOT) attach_trivia(&P, parent, ecomment);
				else if (ecomment.n) { ShclPend pd; pd.text = ecomment; pd.indent = indent; pd.blank_before = had_blank; pd.ceiling = indent.n; ShclVecPend_push(P.tmp, &P.pending, pd); }
				/* A dropped element holds its indent level like any skipped line,
				   so what is written under it is skipped with it (E018) rather
				   than re-parenting to the field. */
				if (!add_star_element(&P, parent, body, lineno)) { ShclStackEnt se; se.indent = indent; se.node = DEAD; ShclVecStack_push(P.tmp, &P.stack, se); }
				i++; continue;
			}
			{
				size_t parent;
				if (!resolve_parent(&P, indent, &parent)) { p_err(&P, lineno, "E012", s_lit("indentation matches no open level")); d->lost++; i++; continue; }
				if (parent == DEAD) { skip_under_dead(&P, lineno, indent); i++; continue; }
			}
			p_err(&P, lineno, "E013", s_lit("malformed line: '*' must be followed by a space"));
			/* Content-malformed at any position, so it is safe to retain
			   verbatim as trivia: re-emitted, it re-diagnoses identically and
			   can never read as a live binding. A hand-typo no longer
			   vanishes on the consumer's next save. The BOM exception the
			   sibling site below carries cannot apply here: this line starts
			   with the '*' that brought us in. */
			{
				ShclPend pd; pd.text = trim_wsp_end(rest); pd.indent = indent; pd.blank_before = had_blank; pd.ceiling = indent.n;
				ShclVecPend_push(P.tmp, &P.pending, pd);
				ShclStackEnt se; se.indent = indent; se.node = DEAD; ShclVecStack_push(P.tmp, &P.stack, se);
			}
			i++; continue;
		}
		ShclStr comment; ShclStr before = split_comment(rest, &comment);
		/* A same-line fence runs to the end of the line: the child-indent
		   spelling keeps a `#` in its info-string, the grammar gives the
		   same-line alternative no comment at all, and the emitter already
		   assumes it. Without this, `a: ```c#` loses the `#`. The cheap test
		   comes first so an ordinary commented line is not scanned twice. */
		if (comment.n && s_has_fence_run(before)) {
			ShclPathScan pre = scan_path(&own->line, trim_wsp_end(before));
			if (pre.ok && pre.has_value && fence_open(pre.value_text).ok) { before = rest; comment.p = NULL; comment.n = 0; }
		}
		ShclStr content = trim_wsp_end(before);
		if (content.n == 0) {
			/* Only a comment survived (e.g. an escaped lead-in); keep it. */
			if (comment.n) {
				ShclPend pd; pd.text = comment; pd.indent = indent; pd.blank_before = had_blank; pd.ceiling = indent.n;
				ShclVecPend_push(P.tmp, &P.pending, pd);
			}
			i++; continue;
		}
		size_t parent;
		if (!resolve_parent(&P, indent, &parent)) { p_err(&P, lineno, "E012", s_lit("indentation matches no open level")); d->lost++; i++; continue; }
		if (parent == DEAD) { skip_under_dead(&P, lineno, indent); i++; continue; }
		ShclPathScan scan = scan_path(&own->line, content);
		if (!scan.ok) {
			ShclSB m = {0}; sb_puts(P.line, &m, "malformed line skipped: "); sb_putS(P.line, &m, scan.err); p_err(&P, lineno, "E014", sb_S(&m));
			/* Content-malformed at any position - retained as trivia, same
			   rationale (and same BOM exception) as the bad '*' line above. */
			if (rest.n >= 3 && (unsigned char)rest.p[0] == 0xEF && (unsigned char)rest.p[1] == 0xBB && (unsigned char)rest.p[2] == 0xBF) d->lost++;
			else {
				ShclPend pd; pd.text = trim_wsp_end(rest); pd.indent = indent; pd.blank_before = had_blank; pd.ceiling = indent.n;
				ShclVecPend_push(P.tmp, &P.pending, pd);
			}
			ShclStackEnt se; se.indent = indent; se.node = DEAD; ShclVecStack_push(P.tmp, &P.stack, se);
			i++; continue;
		}
		size_t next = i + 1;
		ShclValue value;
		if (!scan.has_value) {
			if (looks_like_bracket_array(content)) {
				/* The brackets never survive the load, so a rewrite would bake
				   the changed value in and the file would check clean forever
				   after. Count it lost so the save gate stops that. */
				p_err(&P, lineno, "E019", s_lit("bracket array syntax; an array is comma-separated, without brackets"));
				P.d->lost++;
			} else p_err(&P, lineno, "E015", s_lit("missing colon; repaired as an empty value"));
			value = v_empty();
		}
		else if (scan.value_text.n == 0) value = v_empty();
		else {
			ShclFence vf = fence_open(scan.value_text);
			if (vf.ok) value = consume_raw(&P, lines.data, lines.len, i + 1, lineno, indent, vf, &next);
			else {
				/* Element cap: the whole line is refused, so a capped load
				   never holds a truncated array that would read as the
				   document's value. Counted before anything splits the value
				   (the quote check does too), or the cap would bound nothing. */
				if (P.max_elements && cell_exceeds(scan.value_text, P.max_elements)) {
					ShclSB m = {0}; sb_puts(P.line, &m, "array longer than "); sb_put_u64(P.line, &m, P.max_elements); sb_puts(P.line, &m, " elements; line skipped");
					p_err(&P, lineno, "E021", sb_S(&m));
					P.d->lost++;
					ShclStackEnt se; se.indent = indent; se.node = DEAD; ShclVecStack_push(P.tmp, &P.stack, se);
					i = next; continue;
				}
				if (unterminated_quote(&own->line, scan.value_text)) p_err(&P, lineno, "E017", s_lit("unterminated quote in value"));
				value = parse_cell(a, &own->line, scan.value_text);
			}
		}
		size_t node = 0; /* attach_path fills it; the init quiets gcc's inlining-dependent maybe-uninitialized */
		if (attach_path(&P, parent, scan.segs.data, scan.segs.len, value, lineno, &node)) {
			if (had_blank) NODE(d, node).blank_before = 1;
			attach_trivia(&P, node, comment);
			ShclStackEnt se; se.indent = indent; se.node = node; ShclVecStack_push(P.tmp, &P.stack, se);
		} else {
			ShclStackEnt se; se.indent = indent; se.node = DEAD; ShclVecStack_push(P.tmp, &P.stack, se);
		}
		i = next;
	}
	/* A cap crossed on the document's last line still reports, with nothing
	   left to skip. */
	if (!node_capped && P.max_nodes && d->nodes.len - 1 > P.max_nodes) {
		ShclSB m = {0}; sb_puts(P.line, &m, "node cap of "); sb_put_u64(P.line, &m, P.max_nodes); sb_puts(P.line, &m, " exceeded; parse stopped");
		p_err(&P, lines.len, "E020", sb_S(&m));
	}
	star_flush(&P);
	fold_late_dups(&P);
	emit_repeated_leaf_hints(&P);
	/* Indented tail comments keep their block; only top-level ones orphan. */
	hang_deeper_pending(&P, s_empty());
	for (size_t k = 0; k < P.pending.len; k++)
		ShclVecLead_push(a, &d->orphans, lead_make(P.pending.data[k].text, P.pending.data[k].blank_before));
	/* The emitter drops a blank before the first thing it prints, so a document
	   that kept one there would not survive its own canonical form:
	   load(emit(load(x))) and load(x) would differ on that bit, and a merge -
	   where the line is no longer first - would place a blank the author never
	   wrote. Clear it here, once, wherever output starts. */
	{
		ShclVecSize kids = NODE(d, ROOT).children;
		if (kids.len) {
			ShclNode *n = &NODE(d, kids.data[0]);
			if (n->trivia && n->trivia->leading.len) n->trivia->leading.data[0].blank_before = 0;
			else n->blank_before = 0;
		} else if (d->orphans.len) {
			d->orphans.data[0].blank_before = 0;
		}
	}
	/* The one entry past the cap: what was not listed, and whether any of it
	   was an error, so a consumer scanning the list for errors still finds
	   one and a strict load still fails. */
	if (P.unlisted_errors + P.unlisted_hints) {
		ShclSB m = {0};
		sb_puts(a, &m, "diagnostic cap of "); sb_put_u64(a, &m, P.max_diags);
		sb_puts(a, &m, " reached; "); sb_put_u64(a, &m, P.unlisted_errors + P.unlisted_hints);
		sb_puts(a, &m, " more not listed, "); sb_put_u64(a, &m, P.unlisted_errors); sb_puts(a, &m, " of them errors");
		push_diag(d, 0, P.unlisted_errors ? SHCL_SEV_ERROR : SHCL_SEV_HINT, "E022", sb_S(&m)); /* line 0: about the list */
	}
}

/* -Wclobbered guesses at which locals an unwind could leave indeterminate, and
   it guesses badly once parse_body is inlined into the frame holding the
   setjmp: the recovery path below reads only the two volatile carriers and
   returns. clang has no such warning, so naming it there is itself an error. */
#if defined(__GNUC__) && !defined(__clang__)
	#pragma GCC diagnostic push
	#pragma GCC diagnostic ignored "-Wclobbered"
#endif
static shcl_doc *do_parse(const char *text, size_t len, shcl_strictness strict, size_t max_nodes, size_t max_elements, size_t max_diags) {
	/* The two the unwind path has to reach. volatile because that path arrives
	   by longjmp, which leaves an ordinary local indeterminate. */
	shcl_doc *volatile doc = (shcl_doc *)calloc(1, sizeof *doc);
	if (!doc) return NULL;
	ShclParseOwn *volatile owned = (ShclParseOwn *)calloc(1, sizeof *owned);
	if (!owned) { free(doc); return NULL; }
	jmp_buf panic;
	if (SHCL_SETJMP(panic)) {
		/* An allocation failed somewhere below. Nothing built so far can be
		   trusted and there is no way to finish, so the whole document goes and
		   the caller gets NULL - with the process still standing, which is the
		   point. */
		shcl_doc *bad = doc; ShclParseOwn *badOwn = owned;
		bad->panic = NULL;
		arena_free(&badOwn->line); arena_free(&badOwn->hints);
		free(badOwn->cmaps.data); free(badOwn->dmaps.data); free(badOwn);
		shcl_free(bad);
		return NULL;
	}
	doc->panic = &panic;
	arena_guard(&doc->arena, &panic); arena_guard(&doc->scratch, &panic);
	arena_guard(&doc->reads, &panic); arena_guard(&doc->index_arena, &panic);
	arena_guard(&owned->line, &panic); arena_guard(&owned->hints, &panic);
	doc->strictness = strict;
	parse_body(doc, owned, text, len, max_nodes, max_elements, max_diags);
	/* The recovery point is this frame's; leaving it armed would send a later
	   read or write jumping into a frame that is gone. */
	shcl_doc *d = doc; ShclParseOwn *own = owned;
	d->panic = NULL;
	arena_guard(&d->arena, NULL); arena_guard(&d->scratch, NULL);
	arena_guard(&d->reads, NULL); arena_guard(&d->index_arena, NULL);
	arena_free(&own->line); arena_free(&own->hints);
	free(own->cmaps.data); free(own->dmaps.data); free(own);
	/* The parser borrows scratch for its lines vector, per-parent maps, stack
	   and pending lists - about ten times the input, dead the moment the parse
	   ends. Every resolve resets it anyway, so a document nobody reads would
	   otherwise carry all of it until it was freed. */
	arena_free(&d->scratch);
	return d;
}
#if defined(__GNUC__) && !defined(__clang__)
	#pragma GCC diagnostic pop
#endif

// --- accessor: path resolution ----------------------------------------------

typedef enum { R_NONE, R_ONE, R_MANY, R_SLOTS } ShclRKind;
typedef struct { ShclRKind kind; size_t one; ShclVecSize many; ShclVecSlot slots; } ShclResolved;

/* Size the chain array to the arena, NIL-filled past the old end, so a node
   the writer pushes always has a slot. The array lives in the index arena and
   can move on growth; the stored pointer is the only reference. */
static void index_reserve(shcl_doc *d, size_t need) {
	if (need <= d->index_next_cap) return;
	size_t nc = d->index_next_cap ? d->index_next_cap * 2 : 8;
	while (nc < need) nc *= 2;
	d->index_next = (size_t *)arena_grow(&d->index_arena, d->index_next, d->index_next_cap, nc, sizeof(size_t));
	for (size_t i = d->index_next_cap; i < nc; i++) d->index_next[i] = NIL;
	d->index_next_cap = nc;
}
static void index_append(shcl_doc *d, uint64_t key, size_t node) {
	ShclArena *a = &d->index_arena;
	int was = d->index_built;
	d->index_built = 2;
	index_reserve(d, node + 1);
	d->index_next[node] = NIL;
	ShclCMapEnt *prev = cmap_first(&d->index_last, key);
	if (prev) { d->index_next[prev->val] = node; prev->val = node; }
	else { cmap_put(a, &d->index_first, key, node); cmap_put(a, &d->index_last, key, node); }
	d->index_built = was;
}
/* Walks the chain to find the predecessor; a chain is one name's siblings. */
static void index_unlink(shcl_doc *d, uint64_t key, size_t node) {
	ShclCMapEnt *fe = cmap_first(&d->index_first, key);
	if (!fe) return;
	size_t next = d->index_next[node];
	if (fe->val == node) {
		if (next == NIL) {
			cmap_del(&d->index_first, key, node);
			ShclCMapEnt *le = cmap_first(&d->index_last, key);
			if (le) cmap_del(&d->index_last, key, le->val);
		} else {
			fe->val = next;
		}
	} else {
		size_t c = fe->val;
		while (c != NIL && d->index_next[c] != node) c = d->index_next[c];
		if (c == NIL) return;
		d->index_next[c] = next;
		if (next == NIL) {
			ShclCMapEnt *le = cmap_first(&d->index_last, key);
			if (le) le->val = c;
		}
	}
	d->index_next[node] = NIL;
}
/* A merge drops the index, and so does a lookup that finds a cut-short one; the next lookup rebuilds it. */
static void index_drop(shcl_doc *d) {
	arena_reset_largest(&d->index_arena);
	memset(&d->index_first, 0, sizeof d->index_first);
	memset(&d->index_last, 0, sizeof d->index_last);
	d->index_next = NULL;
	d->index_next_cap = 0;
	d->index_built = 0;
}
static void name_index(shcl_doc *d) {
	if (d->index_built == 1) return;
	if (d->index_built) index_drop(d);
	d->index_built = 2;
	index_reserve(d, d->nodes.len ? d->nodes.len : 1);
	/* From the root, not across the arena: a removed subtree's nodes are still
	   there with their child lists intact, so an arena walk indexes every node
	   the document ever held. Chains stay in file order - a chain is one
	   parent's same-named children, and each parent's are appended in order.
	   The stack rides in the index arena, which this call owns. */
	ShclVecSize stack = {0};
	ShclVecSize_push(&d->index_arena, &stack, ROOT);
	while (stack.len) {
		size_t p = stack.data[--stack.len];
		ShclVecSize ch = NODE(d, p).children;
		for (size_t k = 0; k < ch.len; k++) {
			index_append(d, name_key(p, NODE(d, ch.data[k]).name), ch.data[k]);
			ShclVecSize_push(&d->index_arena, &stack, ch.data[k]);
		}
	}
	d->index_built = 1;
}

static void children_named(shcl_doc *d, ShclArena *a, size_t parent, ShclStr name, ShclVecSize *out) {
	name_index(d);
	ShclCMapEnt *e = cmap_first(&d->index_first, name_key(parent, name));
	size_t c = e ? e->val : NIL;
	while (c != NIL) {
		if (s_eq(NODE(d, c).name, name) && NODE(d, c).parent == parent) ShclVecSize_push(a, out, c);
		c = d->index_next[c];
	}
}

static ShclResolved resolve_from(shcl_doc *d, const size_t *start, size_t nstart, ShclSegment *segs, size_t nsegs) {
	ShclArena *a = &d->scratch; // candidates, slots, compare strings: dead after the call
	ShclVecSize cur = {0};
	// cppcheck-suppress objectIndex  ## single-element callers pass nstart == 1, so start[i] stays at 0
	for (size_t i = 0; i < nstart; i++) ShclVecSize_push(a, &cur, start[i]);
	for (size_t si = 0; si < nsegs; si++) {
		const ShclSegment *seg = &segs[si];
		ShclVecSize next = {0};
		for (size_t k = 0; k < cur.len; k++) {
			ShclVecSize ch = NODE(d, cur.data[k]).children;
			if (seg->star) {
				for (size_t j = 0; j < ch.len; j++) ShclVecSize_push(a, &next, ch.data[j]);
			} else {
				children_named(d, a, cur.data[k], seg->name, &next);
			}
		}
		if (seg->star) {
			// Name wildcard: same per-slot split as `[*]`, over every child.
			ShclSegment *rest = segs + si + 1; size_t nrest = nsegs - si - 1;
			ShclVecSlot slots = {0};
			for (size_t k = 0; k < next.len; k++) {
				ShclSlot sl; sl.present = 0; sl.idx = 0; sl.miss = SHCL_NOT_FOUND;
				if (nrest == 0) { sl.present = 1; sl.idx = next.data[k]; }
				else {
					size_t inst = next.data[k]; ShclResolved r = resolve_from(d, &inst, 1, rest, nrest);
					if (r.kind == R_ONE) { sl.present = 1; sl.idx = r.one; }
					else if (r.kind == R_SLOTS) {
						// A wildcard after a wildcard: the inner slots join the
						// outer list, so the two compose into one flat run of
						// leaves rather than one unreadable slot.
						for (size_t j = 0; j < r.slots.len; j++) ShclVecSlot_push(a, &slots, r.slots.data[j]);
						continue;
					}
					else if (r.kind != R_NONE) sl.miss = SHCL_MULTIPLE;
				}
				ShclVecSlot_push(a, &slots, sl);
			}
			ShclResolved R; R.kind = R_SLOTS; R.slots = slots; memset(&R.many, 0, sizeof R.many); R.one = 0;
			return R;
		}
		switch (seg->sel.tag) {
		case SEL_NONE: cur = next; break;
		case SEL_VALUE: {
			ShclVecSize f = {0};
			ShclStr want = apply_escapes(a, seg->sel.value);
			for (size_t k = 0; k < next.len; k++) if (s_eq(disp_key(a, &NODE(d, next.data[k]).value), want) && (!seg->sel.quoted || single_scalar(&NODE(d, next.data[k]).value))) ShclVecSize_push(a, &f, next.data[k]);
			cur = f; break;
		}
		case SEL_INDEX: {
			ShclVecSize f = {0};
			if (seg->sel.index < next.len) ShclVecSize_push(a, &f, next.data[seg->sel.index]);
			cur = f; break;
		}
		case SEL_WILDCARD: {
			ShclSegment *rest = segs + si + 1; size_t nrest = nsegs - si - 1;
			ShclVecSlot slots = {0};
			for (size_t k = 0; k < next.len; k++) {
				ShclSlot sl; sl.present = 0; sl.idx = 0; sl.miss = SHCL_NOT_FOUND;
				if (nrest == 0) { sl.present = 1; sl.idx = next.data[k]; }
				else {
					size_t inst = next.data[k]; ShclResolved r = resolve_from(d, &inst, 1, rest, nrest);
					if (r.kind == R_ONE) { sl.present = 1; sl.idx = r.one; }
					else if (r.kind == R_SLOTS) {
						// A wildcard after a wildcard: the inner slots join the
						// outer list, so the two compose into one flat run of
						// leaves rather than one unreadable slot.
						for (size_t j = 0; j < r.slots.len; j++) ShclVecSlot_push(a, &slots, r.slots.data[j]);
						continue;
					}
					else if (r.kind != R_NONE) sl.miss = SHCL_MULTIPLE;
				}
				ShclVecSlot_push(a, &slots, sl);
			}
			ShclResolved R; R.kind = R_SLOTS; R.slots = slots; memset(&R.many, 0, sizeof R.many); R.one = 0;
			return R;
		}
		}
	}
	ShclResolved R; memset(&R, 0, sizeof R);
	if (cur.len == 0) R.kind = R_NONE;
	else if (cur.len == 1) { R.kind = R_ONE; R.one = cur.data[0]; }
	else { R.kind = R_MANY; R.many = cur; }
	return R;
}
static int resolve(shcl_doc *d, ShclStr path, ShclResolved *out) {
	// Every public read/query funnels through here, so this reset is the
	// scratch lifetime: the previous resolve's temporaries die now, and the
	// ShclResolved this call fills stays usable until the next resolve.
	arena_reset(&d->scratch);
	ShclPathScan ps = scan_lookup(&d->scratch, path);
	if (!ps.ok || ps.has_value) return 0;
	size_t root = ROOT;
	*out = resolve_from(d, &root, 1, ps.segs.data, ps.segs.len);
	return 1;
}
static shcl_status value_at(shcl_doc *d, ShclStr path, ShclValue **out) {
	ShclResolved r;
	if (!resolve(d, path, &r)) return SHCL_NOT_FOUND;
	if (r.kind == R_NONE) return SHCL_NOT_FOUND;
	if (r.kind == R_MANY || r.kind == R_SLOTS) return SHCL_MULTIPLE;
	*out = &NODE(d, r.one).value; return SHCL_GOOD;
}
static shcl_status scalar_at(shcl_doc *d, ShclStr path, ShclElement **el) {
	ShclValue *v; shcl_status st = value_at(d, path, &v);
	if (st != SHCL_GOOD) { *el = NULL; return st; }
	if (v->kind == V_EMPTY) { *el = NULL; return SHCL_EMPTY; }
	if (v->kind == V_RAW) { *el = NULL; return SHCL_BAD_TYPE; }
	if (v->nels == 1) { *el = &v->els[0]; return SHCL_GOOD; }
	*el = NULL; return SHCL_BAD_TYPE;
}

// ShclElement list for array reads plus a per-slot pre-status: NULL entry => the
// slot has no coercible scalar and sts[i] already says why (a present element
// can still turn BadType if coercion fails). Wildcard slots stay aligned - the
// spec never drops one silently. The lists land in `a`: public reads pass the
// doc read arena (results live until shcl_free or shcl_reads_release); internal queries pass a private
// arena so probing a caller-owned doc leaves nothing behind.
static shcl_status array_elements(shcl_doc *d, ShclArena *a, ShclStr path, ShclElement ***els, shcl_status **sts, size_t *n) {
	ShclResolved r;
	*els = NULL; *sts = NULL; *n = 0;
	if (!resolve(d, path, &r)) return SHCL_NOT_FOUND;
	if (r.kind == R_SLOTS) {
		size_t m = r.slots.len;
		ShclElement **arr = (ShclElement **)arena_alloc(a, (m ? m : 1) * sizeof(ShclElement *));
		shcl_status *st = (shcl_status *)arena_alloc(a, (m ? m : 1) * sizeof(shcl_status));
		for (size_t i = 0; i < m; i++) {
			arr[i] = NULL;
			if (!r.slots.data[i].present) { st[i] = r.slots.data[i].miss; continue; }
			ShclValue *v = &NODE(d, r.slots.data[i].idx).value;
			if (v->kind == V_EMPTY) st[i] = SHCL_EMPTY;
			else if (v->kind == V_CELL && v->nels == 1) { arr[i] = &v->els[0]; st[i] = SHCL_GOOD; }
			else st[i] = SHCL_BAD_TYPE; // raw block, or an array is not one scalar
		}
		// No slots at all means the wildcard's parent is not there, so the
		// path did not resolve - Empty is for a node that is.
		*els = arr; *sts = st; *n = m; return m == 0 ? SHCL_NOT_FOUND : SHCL_GOOD;
	}
	if (r.kind == R_NONE) return SHCL_NOT_FOUND;
	if (r.kind == R_MANY) return SHCL_MULTIPLE;
	ShclValue *v = &NODE(d, r.one).value;
	if (v->kind == V_EMPTY) return SHCL_EMPTY;
	if (v->kind == V_RAW) return SHCL_BAD_TYPE;
	size_t m = v->nels;
	ShclElement **arr = (ShclElement **)arena_alloc(a, (m ? m : 1) * sizeof(ShclElement *));
	shcl_status *st = (shcl_status *)arena_alloc(a, (m ? m : 1) * sizeof(shcl_status));
	for (size_t i = 0; i < m; i++) { arr[i] = &v->els[i]; st[i] = SHCL_GOOD; }
	*els = arr; *sts = st; *n = m; return SHCL_GOOD;
}

static shcl_status worst_slot(const shcl_status *sts, size_t n, shcl_status floor_) {
	shcl_status w = floor_;
	for (size_t i = 0; i < n; i++) if (sts[i] > w) w = sts[i];
	return w;
}

size_t shcl_count(shcl_doc *d, const char *path, size_t plen) {
	ShclStr p; p.p = path; p.n = plen;
	ShclResolved r; if (!resolve(d, p, &r)) return 0;
	switch (r.kind) { case R_NONE: return 0; case R_ONE: return 1; case R_MANY: return r.many.len; case R_SLOTS: return r.slots.len; }
	return 0;
}
static ShclStr emit_name(ShclArena *a, ShclStr name);

size_t shcl_paths(shcl_doc *d, shcl_str **out) {
	ShclArena *a = &d->reads;
	// Walk stack + dedup set: dead after the call. Every other read resets
	// scratch inside its path lookup; this one takes no path, so it resets
	// here, or the document grows 11 KB per call for its whole lifetime.
	arena_reset(&d->scratch);
	ShclArena *t = &d->scratch;
	typedef struct { size_t node; ShclStr prefix; } PEnt;
	PEnt *stack = NULL; size_t sn = 0, sc = 0;
	shcl_str *arr = NULL; size_t n = 0, cap = 0;
	ShclCMap seen; memset(&seen, 0, sizeof seen);
	#define PPUSH(N, P) do { if (sn == sc) { size_t nc = sc ? sc * 2 : 16; stack = (PEnt *)arena_grow(t, stack, sc, nc, sizeof(PEnt)); sc = nc; } stack[sn].node = (N); stack[sn].prefix = (P); sn++; } while (0)
	ShclVecSize top = NODE(d, ROOT).children;
	for (size_t i = top.len; i > 0; i--) PPUSH(top.data[i - 1], s_empty());
	while (sn) {
		PEnt e = stack[--sn];
		ShclStr seg = emit_name(a, NODE(d, e.node).name);
		ShclStr path;
		if (e.prefix.n == 0) path = seg;
		else { ShclSB b = {0}; sb_putS(a, &b, e.prefix); sb_putc(a, &b, '.'); sb_putS(a, &b, seg); path = sb_S(&b); }
		uint64_t h = cmap_hash(path, s_empty());
		int dup = 0;
		for (ShclCMapEnt *en = cmap_first(&seen, h); en; en = cmap_next(en, h)) {
			ShclStr sp; sp.p = arr[en->val].p; sp.n = arr[en->val].n;
			if (s_eq(sp, path)) { dup = 1; break; }
		}
		if (!dup) {
			cmap_put(t, &seen, h, n);
			if (n == cap) { size_t nc = cap ? cap * 2 : 16; arr = (shcl_str *)arena_grow(a, arr, cap, nc, sizeof(shcl_str)); cap = nc; }
			arr[n].p = path.p; arr[n].n = path.n; n++;
		}
		ShclVecSize kids = NODE(d, e.node).children;
		for (size_t i = kids.len; i > 0; i--) PPUSH(kids.data[i - 1], path);
	}
	#undef PPUSH
	if (!arr) arr = (shcl_str *)arena_alloc(a, sizeof(shcl_str));
	*out = arr; return n;
}

shcl_str shcl_quote_segment(shcl_doc *d, const char *name, size_t len) {
	ShclStr in; in.p = name; in.n = len;
	ShclStr q = emit_name(&d->reads, in);
	if (q.p == in.p) q = s_dup(&d->reads, in); // bare passthrough: copy so the result outlives the caller's buffer
	shcl_str out; out.p = q.p; out.n = q.n;
	return out;
}

static size_t instances_in(shcl_doc *d, ShclArena *a, ShclStr p, shcl_str **out) {
	// Wildcard slots that did not resolve stay in the list as "" so indices
	// keep matching shcl_count.
	ShclResolved r;
	if (!resolve(d, p, &r)) { *out = (shcl_str *)arena_alloc(a, sizeof(shcl_str)); return 0; }
	if (r.kind == R_SLOTS) {
		size_t m = r.slots.len;
		shcl_str *arr = (shcl_str *)arena_alloc(a, (m ? m : 1) * sizeof(shcl_str));
		for (size_t k = 0; k < m; k++)
			arr[k] = r.slots.data[k].present ? value_display(a, &NODE(d, r.slots.data[k].idx).value) : s_empty();
		*out = arr; return m;
	}
	ShclVecSize nodes = {0};
	if (r.kind == R_ONE) ShclVecSize_push(a, &nodes, r.one);
	else if (r.kind == R_MANY) for (size_t k = 0; k < r.many.len; k++) ShclVecSize_push(a, &nodes, r.many.data[k]);
	shcl_str *arr = (shcl_str *)arena_alloc(a, (nodes.len ? nodes.len : 1) * sizeof(shcl_str));
	for (size_t k = 0; k < nodes.len; k++) arr[k] = value_display(a, &NODE(d, nodes.data[k]).value);
	*out = arr; return nodes.len;
}
size_t shcl_instances(shcl_doc *d, const char *path, size_t plen, shcl_str **out) {
	ShclStr p; p.p = path; p.n = plen;
	return instances_in(d, &d->reads, p, out);
}

size_t shcl_line(shcl_doc *d, const char *path, size_t plen) {
	ShclStr p; p.p = path; p.n = plen;
	ShclResolved r; if (!resolve(d, p, &r)) return 0;
	if (r.kind != R_ONE) return 0;
	return NODE(d, r.one).line; // writer-built nodes carry 0
}

int shcl_quoted(shcl_doc *d, const char *path, size_t plen) {
	ShclStr p; p.p = path; p.n = plen; ShclElement *el;
	if (scalar_at(d, p, &el) != SHCL_GOOD) return 0;
	return el->quoted;
}

shcl_str shcl_authored_name(shcl_doc *d, const char *path, size_t plen) {
	ShclStr p; p.p = path; p.n = plen;
	ShclResolved r; if (!resolve(d, p, &r)) return s_empty();
	if (r.kind != R_ONE) return s_empty();
	return node_authored(&NODE(d, r.one));
}

size_t shcl_lines(shcl_doc *d, const char *path, size_t plen, size_t **out) {
	// Wildcard slots that did not resolve stay in the list as 0 so indices
	// keep matching shcl_count.
	ShclArena *a = &d->reads; ShclStr p; p.p = path; p.n = plen;
	ShclResolved r;
	if (!resolve(d, p, &r)) { *out = (size_t *)arena_alloc(a, sizeof(size_t)); return 0; }
	if (r.kind == R_SLOTS) {
		size_t m = r.slots.len;
		size_t *arr = (size_t *)arena_alloc(a, (m ? m : 1) * sizeof(size_t));
		for (size_t k = 0; k < m; k++)
			arr[k] = r.slots.data[k].present ? NODE(d, r.slots.data[k].idx).line : 0;
		*out = arr; return m;
	}
	ShclVecSize nodes = {0};
	if (r.kind == R_ONE) ShclVecSize_push(a, &nodes, r.one);
	else if (r.kind == R_MANY) for (size_t k = 0; k < r.many.len; k++) ShclVecSize_push(a, &nodes, r.many.data[k]);
	size_t *arr = (size_t *)arena_alloc(a, (nodes.len ? nodes.len : 1) * sizeof(size_t));
	for (size_t k = 0; k < nodes.len; k++) arr[k] = NODE(d, nodes.data[k]).line; // writer-built nodes carry 0
	*out = arr; return nodes.len;
}

size_t shcl_children(shcl_doc *d, const char *path, size_t plen, shcl_str **out) {
	// Names come back as stored (already arena-owned); only the array is new.
	ShclArena *a = &d->reads; ShclStr p; p.p = path; p.n = plen;
	size_t node = ROOT;
	if (s_trim(p).n != 0) {
		ShclResolved r;
		if (!resolve(d, p, &r) || r.kind != R_ONE) { *out = (shcl_str *)arena_alloc(a, sizeof(shcl_str)); return 0; }
		node = r.one;
	}
	ShclVecSize kids = NODE(d, node).children;
	shcl_str *arr = (shcl_str *)arena_alloc(a, (kids.len ? kids.len : 1) * sizeof(shcl_str));
	for (size_t k = 0; k < kids.len; k++) { arr[k].p = NODE(d, kids.data[k]).name.p; arr[k].n = NODE(d, kids.data[k]).name.n; }
	*out = arr; return kids.len;
}

// --- Writer ------------------------------------------------------------------
// The reverse of the reads. Reads and shcl_to_canonical walk the children vecs,
// so mutating the arena directly is enough - the parser's child map is gone by
// now. New value text is dup'd into the arena; the caller's buffers may go away.

static ShclStr w_dupz(ShclArena *a, const char *p, size_t n) { ShclStr s; s.p = p; s.n = n; return s_dup(a, s); }
static ShclStr w_int_text(ShclArena *a, int64_t v) { char b[32]; int n = snprintf(b, sizeof b, "%lld", (long long)v); return w_dupz(a, b, (size_t)n); }
static ShclStr w_float_text(ShclArena *a, double v) { char b[SHCL_F64_BUF]; size_t n = shcl_format_f64(v, b); return w_dupz(a, b, n); }
static ShclStr w_bool_text(int v) { return v ? s_lit("true") : s_lit("false"); }
static ShclStr w_dt_text(ShclArena *a, const shcl_datetime *dt) { char b[SHCL_DT_BUF]; size_t n = shcl_datetime_str(dt, b); return w_dupz(a, b, n); }
/* Whether a datetime's canonical spelling reads back as the same value: the
   setter's inverse-of-the-read promise, checked by making the round trip. */
static int dt_reads_back(ShclArena *scratch, const shcl_datetime *dt) {
	char b[SHCL_DT_BUF]; ShclStr t; t.p = b; t.n = shcl_datetime_str(dt, b);
	shcl_datetime back;
	if (!parse_datetime(scratch, t, &back)) return 0;
	if (back.has_date != dt->has_date || back.has_time != dt->has_time || back.has_sec != dt->has_sec || back.has_frac != dt->has_frac || back.zone != dt->zone) return 0;
	if (dt->has_date && (back.year != dt->year || back.month != dt->month || back.day != dt->day)) return 0;
	if (dt->has_time && (back.hour != dt->hour || back.minute != dt->minute || (dt->has_sec && back.sec != dt->sec))) return 0;
	if (dt->has_frac && !s_eq(back.frac, dt->frac)) return 0;
	if (dt->zone == SHCL_ZONE_OFFSET && back.off_min != dt->off_min) return 0;
	return 1;
}

// Inverse of a scalar string read (apply_escapes): only backslash, newline, and
// tab need encoding; emit_element wraps quote/reserved chars, reparse strips it.
static void bare_quote_counts(ShclStr t, size_t *dq, size_t *sq);
static ShclStr w_encode_string(ShclArena *a, ShclStr s) {
	ShclSB b = {0};
	sb_reserve(a, &b, s.n);   /* the common value has nothing to escape */
	for (size_t i = 0; i < s.n; i++) {
		char c = s.p[i];
		if (c == '\\') sb_puts(a, &b, "\\\\");
		else if (c == '\n') sb_puts(a, &b, "\\n");
		else if (c == '\t') sb_puts(a, &b, "\\t");
		else sb_putc(a, &b, c);
	}
	ShclStr out = sb_S(&b);
	/* The emitter escapes a bare double quote when both quote kinds appear, so
	   a reparse of the written line stores the escaped spelling. Store it here
	   too, or shcl_instances and a read's raw text differ between a written
	   document and its own reload. */
	size_t dq, sq; bare_quote_counts(out, &dq, &sq);
	if (dq && sq) {
		ShclSB e = {0};
		sb_reserve(a, &e, out.n + dq);
		for (size_t i = 0; i < out.n; i++) {
			if (out.p[i] == '\\') { sb_putc(a, &e, out.p[i]); if (i + 1 < out.n) sb_putc(a, &e, out.p[++i]); }
			else if (out.p[i] == '"') sb_puts(a, &e, "\\\"");
			else sb_putc(a, &e, out.p[i]);
		}
		return sb_S(&e);
	}
	return out;
}

// Pick a backtick fence long enough that no content line closes it early.
static void w_choose_fence(ShclStr content, unsigned char *fc, size_t *fl) {
	size_t maxrun = 0, start = 0;
	for (size_t i = 0;; i++) {
		if (i == content.n || content.p[i] == '\n') {
			ShclStr t = s_trim_wsp(s_slice(content, start, i));
			if (t.n > 0) {
				int all = 1;
				for (size_t k = 0; k < t.n; k++) if (t.p[k] != '`') { all = 0; break; }
				if (all && t.n > maxrun) maxrun = t.n;
			}
			if (i == content.n) break;
			start = i + 1;
		}
	}
	*fc = '`'; *fl = maxrun + 1 < 3 ? 3 : maxrun + 1;
}

static ShclValue w_cell1(ShclArena *a, ShclStr text) {
	ShclValue v; memset(&v, 0, sizeof v); v.kind = V_CELL;
	ShclElement *e = (ShclElement *)arena_alloc(a, sizeof(ShclElement)); e->text = text; e->quoted = 0;
	v.els = e; v.nels = 1; return v;
}
// Inline-array value; the empty array is an empty value (reads back Empty).
static ShclValue w_array(ShclArena *a, const ShclStr *texts, size_t n) {
	if (n == 0) return v_empty();
	ShclValue v; memset(&v, 0, sizeof v); v.kind = V_CELL;
	ShclElement *els = (ShclElement *)arena_alloc(a, n * sizeof(ShclElement));
	for (size_t i = 0; i < n; i++) { els[i].text = texts[i]; els[i].quoted = 0; }
	v.els = els; v.nels = n; return v;
}

static size_t w_new_child(shcl_doc *d, size_t parent, ShclStr name, ShclStr name_src, ShclValue value) {
	ShclArena *a = &d->arena;
	size_t idx = d->nodes.len;
	ShclNode n; memset(&n, 0, sizeof n);
	n.name = s_dup(a, name); n.name_src = spelled(a, name, name_src); n.value = value; n.parent = parent;
	/* Hand-written files separate top-level sections with a blank line;
	   writer-built ones do the same (the emitter never blanks line 1). */
	n.blank_before = (parent == ROOT);
	nodes_push(d, n);
	ShclVecSize_push(a, &NODE(d, parent).children, idx);
	if (d->index_built == 1) index_append(d, name_key(parent, name), idx);
	return idx;
}

// Why a write at this path would fail - the validation walk w_place runs
// before creating anything. SHCL_W_WRITABLE means w_place's gate would pass;
// nothing is created. Temporaries (scan, compare strings) go into `a`.
/* The validation walk w_write_reason and w_place share. `trail`, when non-NULL,
   receives where each segment landed - (size_t)-1 from the point the path falls
   off the existing tree - so w_place can create from exactly there instead of
   scanning the path and walking the tree a second time. `ps` is the caller's
   already-scanned path, so the scan happens once too. */
static shcl_write_reason w_probe_write(shcl_doc *d, ShclArena *a, const ShclPathScan *psp, size_t *trail) {
	const ShclPathScan ps = *psp;
	if (!ps.ok) return SHCL_W_BAD_PATH;
	if (ps.has_value) return SHCL_W_VALUE_IN_PATH;
	if (ps.segs.len == 0) return SHCL_W_BAD_PATH;
	/* Writer side of the load-time nesting cap: never create deeper. */
	if (ps.segs.len > SHCL_MAX_DEPTH) return SHCL_W_TOO_DEEP;
	/* Once this probe falls off the existing tree, a later `[#k]` can never
	   match (fresh intermediates are created childless), so an index segment
	   past that point is unresolvable. */
	int off = 0; size_t pr = ROOT;
	for (size_t i = 0; i < ps.segs.len; i++) {
		ShclSegment *seg = &ps.segs.data[i];
		if (seg->star) return SHCL_W_WILDCARD;
		/* A newline in a SELECTOR has no one-line spelling, so the emitted
		   binding would split across two lines and reparse as neither. The
		   selector stores its path text raw and the value emitter never escapes
		   a line break, so nothing downstream can rescue it - and the reload
		   loses nothing it can count, so the save gate would not catch it. A
		   newline in a NAME is fine: names are stored escape-resolved and
		   emitted through the name escaper, which spells a line break \n and
		   reads it back as one. */
		if (seg->sel.tag == SEL_VALUE && s_has_nl(seg->sel.value)) return SHCL_W_BAD_PATH;
		if (seg->sel.tag == SEL_WILDCARD) return SHCL_W_WILDCARD;
		if (seg->sel.tag == SEL_INDEX) {
			if (off) return SHCL_W_NO_SUCH_INDEX;
			ShclVecSize matches = {0};
			children_named(d, a, pr, seg->name, &matches);
			if (seg->sel.index >= (uint64_t)matches.len) return SHCL_W_NO_SUCH_INDEX;
			pr = matches.data[seg->sel.index];
		} else if (!off) {
			size_t found = (size_t)-1;
			ShclStr want = (seg->sel.tag == SEL_VALUE) ? apply_escapes(a, seg->sel.value) : s_empty();
			ShclVecSize cands = {0};
			children_named(d, a, pr, seg->name, &cands);
			for (size_t k = 0; k < cands.len; k++) {
				size_t c = cands.data[k];
				if (seg->sel.tag == SEL_VALUE && !(s_eq(disp_key(a, &NODE(d, c).value), want) && (!seg->sel.quoted || single_scalar(&NODE(d, c).value)))) continue;
				found = c; break;
			}
			if (found == (size_t)-1) off = 1; else pr = found;
		}
		if (trail) trail[i] = off ? (size_t)-1 : pr;
	}
	return SHCL_W_WRITABLE;
}

static shcl_write_reason w_write_reason(shcl_doc *d, ShclArena *a, ShclStr path) {
	ShclPathScan ps = scan_lookup(a, path);
	return w_probe_write(d, a, &ps, NULL);
}

// Walk (creating as needed) to the node a write targets. Returns 1 + *out, or 0
// if the path is unusable for a write (w_write_reason says why). Validation
// runs first, so a doomed path leaves no half-created intermediates behind.
static int w_place(shcl_doc *d, ShclStr path, size_t *out) {
	ShclArena *a = &d->arena;
	// The probe, the scan, and the compare strings are dead once this returns,
	// so they go through scratch (reset like resolve's; no resolve runs in
	// here) - repeated setters must not grow the doc. Only what w_new_child
	// dups persists.
	ShclArena *t = &d->scratch;
	arena_reset(t);
	ShclPathScan ps = scan_lookup(t, path);
	size_t *trail = (size_t *)arena_alloc(t, (ps.segs.len ? ps.segs.len : 1) * sizeof(size_t));
	if (w_probe_write(d, t, &ps, trail) != SHCL_W_WRITABLE) return 0;
	size_t cur = ROOT;
	for (size_t i = 0; i < ps.segs.len; i++) {
		const ShclSegment *seg = &ps.segs.data[i];
		/* The probe already resolved every segment that exists; only the tail
		   it fell off has anything to create. */
		if (trail[i] != (size_t)-1) { cur = trail[i]; continue; }
		if (seg->sel.tag == SEL_NONE) cur = w_new_child(d, cur, seg->name, seg->name_src, v_empty());
		else if (seg->sel.tag == SEL_VALUE) cur = w_new_child(d, cur, seg->name, seg->name_src, w_cell1(a, s_dup(a, seg->sel.value)));
		/* Unreachable: w_probe_write refuses a wildcard outright and an
		   unresolvable index, so neither reaches an empty trail slot. */
		else return 0;
	}
	*out = cur; return 1;
}

/* A written value may now collide with a same-named sibling under the in-file
   merge rule; fold the pair the way a reparse would (earlier sibling survives,
   later one folds children and trivia in) so Writer output stays a formatter
   fixpoint. */
/* Folding moves the loser's children up a level, where they can collide with
   the survivor's own. The parser's fold is depth-first for the same reason;
   only a node that just received children can hold a new pair. */
static void w_fold_dups_below(shcl_doc *d, size_t start) {
	ShclArena *t = &d->scratch;
	ShclVecSize stack = {0};
	ShclVecSize_push(t, &stack, start);
	while (stack.len) {
		size_t parent = stack.data[--stack.len];
		ShclCMap first; memset(&first, 0, sizeof first);
		ShclVecSize *ch = &NODE(d, parent).children;
		size_t w = 0;
		for (size_t k = 0; k < ch->len; k++) {
			size_t c = ch->data[k];
			uint64_t h = merge_hash(NODE(d, c).name, &NODE(d, c).value);
			size_t survivor = (size_t)-1;
			for (ShclCMapEnt *e = cmap_first(&first, h); e; e = cmap_next(e, h))
				if (merge_eq(NODE(d, e->val).name, &NODE(d, e->val).value, NODE(d, c).name, &NODE(d, c).value)) { survivor = e->val; break; }
			if (survivor != (size_t)-1) {
				ShclVecSize moved = NODE(d, c).children;
				fold_node_into(d, survivor, c);
				if (d->index_built == 1) {
					index_unlink(d, name_key(parent, NODE(d, c).name), c);
					for (size_t m = 0; m < moved.len; m++) {
						ShclStr nm = NODE(d, moved.data[m]).name;
						index_unlink(d, name_key(c, nm), moved.data[m]);
						index_append(d, name_key(survivor, nm), moved.data[m]);
					}
				}
				ShclVecSize_push(t, &stack, survivor);
			} else {
				cmap_put(t, &first, h, c);
				ch->data[w++] = c;
			}
		}
		ch->len = w;
	}
}

static void w_collapse_dup(shcl_doc *d, size_t node) {
	size_t parent = NODE(d, node).parent;
	const ShclNode *me = &NODE(d, node);
	ShclVecSize cands = {0};
	children_named(d, &d->scratch, parent, me->name, &cands);
	size_t other = (size_t)-1;
	for (size_t k = 0; k < cands.len; k++) {
		size_t c = cands.data[k];
		if (c != node && merge_eq(NODE(d, c).name, &NODE(d, c).value, me->name, &me->value)) { other = c; break; }
	}
	if (other == (size_t)-1) return;
	ShclVecSize ch = NODE(d, parent).children;
	size_t pos_node = (size_t)-1, pos_other = (size_t)-1;
	for (size_t k = 0; k < ch.len; k++) {
		if (ch.data[k] == node) pos_node = k;
		else if (ch.data[k] == other) pos_other = k;
	}
	size_t survivor = (pos_other < pos_node) ? other : node;
	size_t loser = (survivor == node) ? other : node;
	ShclVecSize moved = NODE(d, loser).children;
	fold_node_into(d, survivor, loser);
	ShclVecSize *pk = &NODE(d, parent).children;
	size_t w = 0;
	for (size_t k = 0; k < pk->len; k++) if (pk->data[k] != loser) pk->data[w++] = pk->data[k];
	pk->len = w;
	if (d->index_built == 1) {
		index_unlink(d, name_key(parent, NODE(d, loser).name), loser);
		for (size_t k = 0; k < moved.len; k++) {
			ShclStr nm = NODE(d, moved.data[k]).name;
			index_unlink(d, name_key(loser, nm), moved.data[k]);
			index_append(d, name_key(survivor, nm), moved.data[k]);
		}
	}
	w_fold_dups_below(d, survivor);
}

/* The setters encode into the document arena before the path is validated, and
   a bump arena never gives that back - a refused 20 MB write used to cost the
   document 85 MB permanently. w_place allocates only in scratch until its
   probe passes, so on a refusal nothing but the encoded value sits past the
   mark. The mark is taken by the caller, before it encodes. */
static int w_set_marked(shcl_doc *d, ShclStr path, ShclValue v, ShclMark m) {
	size_t idx;
	if (!w_place(d, path, &idx)) { arena_release(&d->arena, m); return 0; }
	NODE(d, idx).value = v;
	w_collapse_dup(d, idx);
	return 1;
}

shcl_doc *shcl_new(void) { return shcl_parse("", 0); }

int shcl_exists(shcl_doc *d, const char *path, size_t plen) {
	ShclStr p; p.p = path; p.n = plen; ShclResolved r;
	if (!resolve(d, p, &r)) return 0;
	if (r.kind == R_ONE || r.kind == R_MANY) return 1;
	if (r.kind == R_SLOTS) for (size_t i = 0; i < r.slots.len; i++) if (r.slots.data[i].present) return 1;
	return 0;
}

size_t shcl_remove(shcl_doc *d, const char *path, size_t plen) {
	ShclArena *a = &d->arena; ShclStr p; p.p = path; p.n = plen; ShclResolved r;
	if (!resolve(d, p, &r)) return 0;
	ShclVecSize targets = {0};
	if (r.kind == R_ONE) ShclVecSize_push(a, &targets, r.one);
	else if (r.kind == R_MANY) targets = r.many;
	else if (r.kind == R_SLOTS) for (size_t i = 0; i < r.slots.len; i++) if (r.slots.data[i].present) ShclVecSize_push(a, &targets, r.slots.data[i].idx);
	for (size_t i = 0; i < targets.len; i++) {
		size_t t = targets.data[i]; size_t pn = NODE(d, t).parent;
		ShclVecSize *kids = &NODE(d, pn).children;
		size_t w = 0;
		for (size_t k = 0; k < kids->len; k++) if (kids->data[k] != t) kids->data[w++] = kids->data[k];
		kids->len = w;
		if (d->index_built == 1) index_unlink(d, name_key(pn, NODE(d, t).name), t);
	}
	return targets.len;
}

shcl_write_reason shcl_write_reason_(shcl_doc *d, const char *path, size_t plen) {
	ShclStr p; p.p = path; p.n = plen;
	// A probe, not a write: temporaries go into scratch (reset like resolve's -
	// the previous query's die now), never permanently into the doc arena.
	arena_reset(&d->scratch);
	return w_write_reason(d, &d->scratch, p);
}

int shcl_set_comment(shcl_doc *d, const char *path, size_t plen, const char *text, size_t tlen) {
	ShclArena *a = &d->arena; ShclStr p; p.p = path; p.n = plen; size_t idx;
	if (!w_place(d, p, &idx)) return 0;
	ShclStr line; line.p = text; line.n = tlen;
	for (size_t i = 0; i < line.n; i++) if (line.p[i] == '\n') { line.n = i; break; }
	ShclStr out;
	if (line.n == 0 || line.p[0] != '#') { ShclSB b = {0}; sb_puts(a, &b, "# "); sb_putS(a, &b, line); out = sb_S(&b); }
	else out = s_dup(a, line);
	/* Without this the load trims what was written and the writer's output
	   stops being a fmt fixpoint. Blank text leaves a bare `#`. */
	out = trim_wsp_end(out);
	/* The node's own blank moves above its first comment; otherwise the blank
	   would separate the comment from what it annotates. Above the first one
	   already there, when there is one. */
	ShclNode *nd = &NODE(d, idx);
	ShclLead lead = lead_plain(out);
	ShclTrivia *t = triv_mut(a, nd);
	if (nd->blank_before) {
		nd->blank_before = 0;
		if (t->leading.len) t->leading.data[0].blank_before = 1;
		else lead.blank_before = 1;
	}
	ShclVecLead_push(a, &t->leading, lead);
	return 1;
}

int shcl_set_empty(shcl_doc *d, const char *path, size_t plen) { ShclStr p; p.p = path; p.n = plen; ShclMark m = arena_mark(&d->arena); return w_set_marked(d, p, v_empty(), m); }
int shcl_set_int(shcl_doc *d, const char *path, size_t plen, int64_t v) { ShclArena *a = &d->arena; ShclStr p; p.p = path; p.n = plen; ShclMark m = arena_mark(a); return w_set_marked(d, p, w_cell1(a, w_int_text(a, v)), m); }
/* An infinity or a NaN has no spelling the reader accepts, and neither does a
   datetime the reader would refuse (month 13, a fraction with no seconds, an
   empty struct): each fails the write rather than binding text that cannot
   read back. */
int shcl_set_float(shcl_doc *d, const char *path, size_t plen, double v) { if (!isfinite(v)) return 0; ShclArena *a = &d->arena; ShclStr p; p.p = path; p.n = plen; ShclMark m = arena_mark(a); return w_set_marked(d, p, w_cell1(a, w_float_text(a, v)), m); }
int shcl_set_bool(shcl_doc *d, const char *path, size_t plen, int v) { ShclArena *a = &d->arena; ShclStr p; p.p = path; p.n = plen; ShclMark m = arena_mark(a); return w_set_marked(d, p, w_cell1(a, w_bool_text(v)), m); }
int shcl_set_string(shcl_doc *d, const char *path, size_t plen, const char *s, size_t slen) { ShclArena *a = &d->arena; ShclStr p; p.p = path; p.n = plen; ShclStr in; in.p = s; in.n = slen; ShclMark m = arena_mark(a); return w_set_marked(d, p, w_cell1(a, w_encode_string(a, in)), m); }

static int literal_value(ShclArena *a, ShclArena *tmp, ShclStr text, ShclValue *out) {
	for (size_t i = 0; i < text.n; i++) { if (text.p[i] == '\n' || text.p[i] == '\r') return 0; }
	ShclStr comment; ShclStr v = s_trim_wsp(split_value_comment(text, &comment));
	if (unterminated_quote(tmp, v)) return 0;
	/* Bracket-array text is refused too: in a file it is E019 and the line is
	   lost, so writing it as a two-element array holding `[1` and `2]` would
	   be a different wrong answer. */
	if (v.n && v.p[0] == '[' && v.p[v.n - 1] == ']') return 0;
	/* One copy of the value text up front: parse_cell stores slices, and the
	   caller's buffer need not outlive the call (the setter contract). */
	*out = parse_cell(a, tmp, s_dup(a, v));
	return 1;
}

int shcl_set_literal(shcl_doc *d, const char *path, size_t plen, const char *text, size_t tlen) {
	ShclArena *a = &d->arena; ShclStr p; p.p = path; p.n = plen; ShclStr in; in.p = text; in.n = tlen;
	ShclValue v;
	ShclMark m = arena_mark(a);
	if (!literal_value(a, &d->scratch, in, &v)) { arena_release(a, m); return 0; }
	return w_set_marked(d, p, v, m);
}
int shcl_set_datetime(shcl_doc *d, const char *path, size_t plen, const shcl_datetime *dt) { if (!dt_reads_back(&d->scratch, dt)) return 0; ShclArena *a = &d->arena; ShclStr p; p.p = path; p.n = plen; ShclMark m = arena_mark(a); return w_set_marked(d, p, w_cell1(a, w_dt_text(a, dt)), m); }
// Bind a raw block at a path, picking a fence longer than any content line.
// The info-string is stored as a fence line would read it back (trimmed); one
// holding a line break or an unquoted `#` has no fence-line spelling (the `#`
// would read back as a comment) and fails the write. A body line ending in CR
// fails for the same reason: the load takes the whole trailing CR run off every
// line, so it would not read back.
int shcl_set_raw(shcl_doc *d, const char *path, size_t plen, const char *content, size_t clen, const char *info, size_t ilen) {
	ShclArena *a = &d->arena; ShclStr p; p.p = path; p.n = plen;
	ShclStr it; it.p = info; it.n = ilen;
	if (it.n && (memchr(it.p, '\n', it.n) || memchr(it.p, '\r', it.n))) return 0;
	ShclStr icomment; split_comment(it, &icomment);
	if (icomment.n) return 0;
	for (size_t i = 0; i < clen; i++) if (content[i] == '\r' && (i + 1 == clen || content[i + 1] == '\n')) return 0;
	it = s_trim(it);
	ShclMark m = arena_mark(a);
	ShclStr c = w_dupz(a, content, clen), inf = s_dup(a, it);
	unsigned char fc; size_t fl; w_choose_fence(c, &fc, &fl);
	ShclValue v; memset(&v, 0, sizeof v); v.kind = V_RAW;
	v.raw = (ShclRawVal *)arena_alloc(a, sizeof(ShclRawVal));
	v.raw->content = c; v.raw->info = inf; v.raw->fence_char = fc; v.raw->fence_len = fl;
	return w_set_marked(d, p, v, m);
}

int shcl_set_int_array(shcl_doc *d, const char *path, size_t plen, const int64_t *v, size_t n) {
	ShclArena *a = &d->arena; ShclStr p; p.p = path; p.n = plen; ShclMark m = arena_mark(a); ShclStr *t = (ShclStr *)arena_alloc(a, (n ? n : 1) * sizeof(ShclStr));
	for (size_t i = 0; i < n; i++) t[i] = w_int_text(a, v[i]);
	return w_set_marked(d, p, w_array(a, t, n), m);
}
int shcl_set_float_array(shcl_doc *d, const char *path, size_t plen, const double *v, size_t n) {
	for (size_t i = 0; i < n; i++) if (!isfinite(v[i])) return 0;
	ShclArena *a = &d->arena; ShclStr p; p.p = path; p.n = plen; ShclMark m = arena_mark(a); ShclStr *t = (ShclStr *)arena_alloc(a, (n ? n : 1) * sizeof(ShclStr));
	for (size_t i = 0; i < n; i++) t[i] = w_float_text(a, v[i]);
	return w_set_marked(d, p, w_array(a, t, n), m);
}
int shcl_set_bool_array(shcl_doc *d, const char *path, size_t plen, const int *v, size_t n) {
	ShclArena *a = &d->arena; ShclStr p; p.p = path; p.n = plen; ShclMark m = arena_mark(a); ShclStr *t = (ShclStr *)arena_alloc(a, (n ? n : 1) * sizeof(ShclStr));
	for (size_t i = 0; i < n; i++) t[i] = w_bool_text(v[i]);
	return w_set_marked(d, p, w_array(a, t, n), m);
}
int shcl_set_string_array(shcl_doc *d, const char *path, size_t plen, const char *const *v, const size_t *lens, size_t n) {
	ShclArena *a = &d->arena; ShclStr p; p.p = path; p.n = plen; ShclMark m = arena_mark(a); ShclStr *t = (ShclStr *)arena_alloc(a, (n ? n : 1) * sizeof(ShclStr));
	for (size_t i = 0; i < n; i++) { ShclStr in; in.p = v[i]; in.n = lens[i]; t[i] = w_encode_string(a, in); }
	return w_set_marked(d, p, w_array(a, t, n), m);
}
int shcl_set_datetime_array(shcl_doc *d, const char *path, size_t plen, const shcl_datetime *v, size_t n) {
	for (size_t i = 0; i < n; i++) if (!dt_reads_back(&d->scratch, &v[i])) return 0;
	ShclArena *a = &d->arena; ShclStr p; p.p = path; p.n = plen; ShclMark m = arena_mark(a); ShclStr *t = (ShclStr *)arena_alloc(a, (n ? n : 1) * sizeof(ShclStr));
	for (size_t i = 0; i < n; i++) t[i] = w_dt_text(a, &v[i]);
	return w_set_marked(d, p, w_array(a, t, n), m);
}

int shcl_set_int_default(shcl_doc *d, const char *path, size_t plen, int64_t v) { if (!shcl_exists(d, path, plen)) return shcl_set_int(d, path, plen, v); return shcl_write_reason_(d, path, plen) == SHCL_W_WRITABLE; }
int shcl_set_float_default(shcl_doc *d, const char *path, size_t plen, double v) { if (!shcl_exists(d, path, plen)) return shcl_set_float(d, path, plen, v); return shcl_write_reason_(d, path, plen) == SHCL_W_WRITABLE; }
int shcl_set_bool_default(shcl_doc *d, const char *path, size_t plen, int v) { if (!shcl_exists(d, path, plen)) return shcl_set_bool(d, path, plen, v); return shcl_write_reason_(d, path, plen) == SHCL_W_WRITABLE; }
int shcl_set_string_default(shcl_doc *d, const char *path, size_t plen, const char *s, size_t slen) { if (!shcl_exists(d, path, plen)) return shcl_set_string(d, path, plen, s, slen); return shcl_write_reason_(d, path, plen) == SHCL_W_WRITABLE; }
int shcl_set_literal_default(shcl_doc *d, const char *path, size_t plen, const char *text, size_t tlen) { if (!shcl_exists(d, path, plen)) return shcl_set_literal(d, path, plen, text, tlen); return shcl_write_reason_(d, path, plen) == SHCL_W_WRITABLE; }
int shcl_set_datetime_default(shcl_doc *d, const char *path, size_t plen, const shcl_datetime *dt) { if (!shcl_exists(d, path, plen)) return shcl_set_datetime(d, path, plen, dt); return shcl_write_reason_(d, path, plen) == SHCL_W_WRITABLE; }
int shcl_set_raw_default(shcl_doc *d, const char *path, size_t plen, const char *content, size_t clen, const char *info, size_t ilen) { if (!shcl_exists(d, path, plen)) return shcl_set_raw(d, path, plen, content, clen, info, ilen); return shcl_write_reason_(d, path, plen) == SHCL_W_WRITABLE; }
int shcl_set_int_array_default(shcl_doc *d, const char *path, size_t plen, const int64_t *v, size_t n) { if (!shcl_exists(d, path, plen)) return shcl_set_int_array(d, path, plen, v, n); return shcl_write_reason_(d, path, plen) == SHCL_W_WRITABLE; }
int shcl_set_float_array_default(shcl_doc *d, const char *path, size_t plen, const double *v, size_t n) { if (!shcl_exists(d, path, plen)) return shcl_set_float_array(d, path, plen, v, n); return shcl_write_reason_(d, path, plen) == SHCL_W_WRITABLE; }
int shcl_set_bool_array_default(shcl_doc *d, const char *path, size_t plen, const int *v, size_t n) { if (!shcl_exists(d, path, plen)) return shcl_set_bool_array(d, path, plen, v, n); return shcl_write_reason_(d, path, plen) == SHCL_W_WRITABLE; }
int shcl_set_string_array_default(shcl_doc *d, const char *path, size_t plen, const char *const *v, const size_t *lens, size_t n) { if (!shcl_exists(d, path, plen)) return shcl_set_string_array(d, path, plen, v, lens, n); return shcl_write_reason_(d, path, plen) == SHCL_W_WRITABLE; }
int shcl_set_datetime_array_default(shcl_doc *d, const char *path, size_t plen, const shcl_datetime *v, size_t n) { if (!shcl_exists(d, path, plen)) return shcl_set_datetime_array(d, path, plen, v, n); return shcl_write_reason_(d, path, plen) == SHCL_W_WRITABLE; }

// --- Layered loading: overlay a higher-priority document onto a lower one ----

// Deep-copy a value from `over`'s arena into `d`'s, so the merged doc is
// self-contained (over may be freed after the merge).
static ShclValue w_dup_value(ShclArena *a, const ShclValue *v) {
	ShclValue r; memset(&r, 0, sizeof r); r.kind = v->kind;
	if (v->kind == V_CELL) {
		r.nels = v->nels;
		r.els = (ShclElement *)arena_alloc(a, (v->nels ? v->nels : 1) * sizeof(ShclElement));
		for (size_t i = 0; i < v->nels; i++) { r.els[i].text = s_dup(a, v->els[i].text); r.els[i].quoted = v->els[i].quoted; }
	} else if (v->kind == V_RAW) {
		r.raw = (ShclRawVal *)arena_alloc(a, sizeof(ShclRawVal));
		r.raw->content = s_dup(a, v->raw->content); r.raw->info = s_dup(a, v->raw->info);
		r.raw->fence_char = v->raw->fence_char; r.raw->fence_len = v->raw->fence_len;
	}
	return r;
}

// Deep-copy over's subtree at `oi` into d's arena under `parent`. d->nodes may
// reallocate on push, so parent's children vec is fetched only via NODE().
static ShclTrivia *w_clone_trivia(ShclArena *a, const ShclTrivia *st) {
	ShclTrivia *nt = (ShclTrivia *)arena_alloc(a, sizeof(ShclTrivia));
	memset(nt, 0, sizeof(ShclTrivia));
	nt->trailing = s_dup(a, st->trailing);
	for (size_t i = 0; i < st->leading.len; i++) ShclVecLead_push(a, &nt->leading, lead_make(s_dup(a, st->leading.data[i].text), st->leading.data[i].blank_before));
	for (size_t i = 0; i < st->after.len; i++) ShclVecLead_push(a, &nt->after, lead_make(s_dup(a, st->after.data[i].text), st->after.data[i].blank_before));
	for (size_t i = 0; i < st->inside.len; i++) ShclVecLead_push(a, &nt->inside, lead_make(s_dup(a, st->inside.data[i].text), st->inside.data[i].blank_before));
	return nt;
}
static size_t w_clone_subtree(shcl_doc *d, const shcl_doc *over, size_t oi, size_t parent) {
	ShclArena *a = &d->arena;
	const ShclNode *src = &over->nodes.data[oi];
	ShclNode n; memset(&n, 0, sizeof n);
	n.name = s_dup(a, src->name);
	n.name_src = s_dup(a, src->name_src);
	n.value = w_dup_value(a, &src->value);
	n.parent = parent;
	n.line = src->line;
	n.star_list = src->star_list;
	n.star_mixed = src->star_mixed;
	n.blank_before = src->blank_before;
	if (src->trivia) n.trivia = w_clone_trivia(a, src->trivia);
	size_t idx = d->nodes.len;
	nodes_push(d, n);
	// Snapshot the source children (const, stable) before recursing.
	size_t nk = over->nodes.data[oi].children.len;
	for (size_t i = 0; i < nk; i++) {
		size_t ok = over->nodes.data[oi].children.data[i];
		size_t c = w_clone_subtree(d, over, ok, idx);
		ShclVecSize_push(a, &NODE(d, idx).children, c);
	}
	return idx;
}

/* A matched instance keeps the base node, so the over side's comments have to
   move onto it or they are lost. Same rule as an in-file merge: leading
   concatenates in layer order, first trailing wins. Text is dup'd into d's
   arena - over may be freed after the merge. */
static void adopt_trivia(shcl_doc *d, size_t base, const shcl_doc *over, size_t ok) {
	ShclArena *a = &d->arena;
	const ShclTrivia *st = over->nodes.data[ok].trivia;
	if (!st) return;
	ShclTrivia *bt = triv_mut(a, &NODE(d, base));
	for (size_t i = 0; i < st->leading.len; i++)
		ShclVecLead_push(a, &bt->leading, lead_make(s_dup(a, st->leading.data[i].text), st->leading.data[i].blank_before));
	if (st->trailing.n) {
		if (bt->trailing.n == 0) bt->trailing = s_dup(a, st->trailing);
		else ShclVecLead_push(a, &bt->leading, lead_plain(s_dup(a, st->trailing)));
	}
	for (size_t i = 0; i < st->after.len; i++)
		ShclVecLead_push(a, &bt->after, lead_make(s_dup(a, st->after.data[i].text), st->after.data[i].blank_before));
	for (size_t i = 0; i < st->inside.len; i++)
		ShclVecLead_push(a, &bt->inside, lead_make(s_dup(a, st->inside.data[i].text), st->inside.data[i].blank_before));
}

// One grouping pass over each side, then a single children rebuild: the old
// shape re-filtered the over side per distinct name and re-scanned (and
// re-keyed) the base side per over node - three O(K^2) terms at one parent,
// plus a fresh children vector per replaced name.
static void w_overlay(shcl_doc *d, size_t bp, const shcl_doc *over, size_t op) {
	ShclArena *a = &d->arena;
	ShclArena *t = &d->scratch;
	ShclVecSize okids = over->nodes.data[op].children; // const doc: stable
	// Over side: name -> bucket of child positions, in first-appearance
	// order. Map hits verify against what the entry's value names (hash-only
	// entries store no key).
	ShclVecS order = {0}; ShclVecSize *buckets = NULL; size_t nb = 0, cb = 0;
	ShclCMap group_of; memset(&group_of, 0, sizeof group_of);
	for (size_t i = 0; i < okids.len; i++) {
		size_t k = okids.data[i]; ShclStr nm = over->nodes.data[k].name;
		uint64_t h = cmap_hash(nm, s_empty());
		size_t g = (size_t)-1;
		for (ShclCMapEnt *e = cmap_first(&group_of, h); e; e = cmap_next(e, h))
			if (s_eq(order.data[e->val], nm)) { g = e->val; break; }
		if (g == (size_t)-1) {
			if (nb == cb) { size_t nc = cb ? cb * 2 : 8; buckets = (ShclVecSize *)arena_grow(t, buckets, cb, nc, sizeof(ShclVecSize)); cb = nc; }
			memset(&buckets[nb], 0, sizeof buckets[nb]);
			g = nb++;
			cmap_put(t, &group_of, h, g);
			ShclVecS_push(t, &order, nm);
		}
		ShclVecSize_push(t, &buckets[g], i);
	}
	// Base side, one pass: does the name exist / have a container instance
	// (entries name a representative base child), and which child carries
	// each (name, merge key).
	ShclVecSize base = {0};
	{ ShclVecSize bk = NODE(d, bp).children; for (size_t i = 0; i < bk.len; i++) ShclVecSize_push(t, &base, bk.data[i]); }
	ShclCMap in_base, has_cont, by_key;
	memset(&in_base, 0, sizeof in_base); memset(&has_cont, 0, sizeof has_cont); memset(&by_key, 0, sizeof by_key);
	for (size_t i = 0; i < base.len; i++) {
		size_t b = base.data[i]; ShclStr nm = NODE(d, b).name;
		uint64_t hn = cmap_hash(nm, s_empty());
		int seen = 0;
		for (ShclCMapEnt *e = cmap_first(&in_base, hn); e; e = cmap_next(e, hn))
			if (s_eq(NODE(d, e->val).name, nm)) { seen = 1; break; }
		if (!seen) cmap_put(t, &in_base, hn, b);
		if (NODE(d, b).children.len > 0) {
			int seenc = 0;
			for (ShclCMapEnt *e = cmap_first(&has_cont, hn); e; e = cmap_next(e, hn))
				if (s_eq(NODE(d, e->val).name, nm)) { seenc = 1; break; }
			if (!seenc) cmap_put(t, &has_cont, hn, b);
		}
		uint64_t hk = merge_hash(nm, &NODE(d, b).value);
		int seenk = 0;
		for (ShclCMapEnt *e = cmap_first(&by_key, hk); e; e = cmap_next(e, hk))
			if (merge_eq(NODE(d, e->val).name, &NODE(d, e->val).value, nm, &NODE(d, b).value)) { seenk = 1; break; }
		if (!seenk) cmap_put(t, &by_key, hk, b);
	}
	// Decide per name. A name whose over-side nodes are all leaves is an
	// override - but only when the base side of the group is leaf-shaped too.
	// Against a base container, a childless over-node is a wrapper mention,
	// not a leaf, so it falls through to the instance merge: a bare section
	// header in a higher layer never wipes the subtree below it. Replaced
	// groups splice in the rebuild; everything appended (unmatched instances,
	// and replaced names base never had) keeps the over file's order, which
	// the per-name pass here would otherwise regroup: app_at is indexed by
	// the over child's position.
	ShclVecSize *rep = (ShclVecSize *)arena_alloc(t, (nb ? nb : 1) * sizeof(ShclVecSize));
	int *is_rep = (int *)arena_alloc(t, (nb ? nb : 1) * sizeof(int));
	size_t *app_at = (size_t *)arena_alloc(t, (okids.len ? okids.len : 1) * sizeof(size_t));
	for (size_t i = 0; i < okids.len; i++) app_at[i] = (size_t)-1;
	size_t nappended = 0;
	int any_rep = 0;
	ShclValue ev; memset(&ev, 0, sizeof ev); ev.kind = V_EMPTY;
	for (size_t gi = 0; gi < nb; gi++) {
		ShclStr name = order.data[gi];
		ShclVecSize grp = buckets[gi];
		memset(&rep[gi], 0, sizeof rep[gi]); is_rep[gi] = 0;
		int over_leafy = 1;
		for (size_t i = 0; i < grp.len; i++) if (over->nodes.data[okids.data[grp.data[i]]].children.len > 0) { over_leafy = 0; break; }
		uint64_t hn = cmap_hash(name, s_empty());
		int inb = 0, bc = 0;
		for (ShclCMapEnt *e = cmap_first(&in_base, hn); e; e = cmap_next(e, hn))
			if (s_eq(NODE(d, e->val).name, name)) { inb = 1; break; }
		for (ShclCMapEnt *e = cmap_first(&has_cont, hn); e; e = cmap_next(e, hn))
			if (s_eq(NODE(d, e->val).name, name)) { bc = 1; break; }
		if (over_leafy && !bc) {
			for (size_t i = 0; i < grp.len; i++) {
				size_t pos = grp.data[i];
				size_t c = w_clone_subtree(d, over, okids.data[pos], bp);
				if (inb) ShclVecSize_push(t, &rep[gi], c);
				else { app_at[pos] = c; nappended++; }
			}
			if (inb) {
				is_rep[gi] = 1; any_rep = 1;
				/* The replaced leaf's comments go with it, which the spec
				   allows; a content-malformed line retained on it is content
				   the parser promised to keep, so those move onto the
				   replacement. A comment starts with `#`, a retained line
				   never does. The texts already live in the document arena. */
				ShclVecLead kept = {0};
				for (size_t i = 0; i < base.len; i++) {
					size_t b = base.data[i];
					if (!s_eq(NODE(d, b).name, name)) continue;
					const ShclTrivia *bt = NODE(d, b).trivia;
					if (!bt) continue;
					const ShclVecLead *lists[3] = { &bt->leading, &bt->inside, &bt->after };
					for (size_t li = 0; li < 3; li++)
						for (size_t k = 0; k < lists[li]->len; k++)
							if (!(lists[li]->data[k].text.n && lists[li]->data[k].text.p[0] == '#')) ShclVecLead_push(t, &kept, lists[li]->data[k]);
				}
				if (kept.len) {
					ShclTrivia *ct = triv_mut(a, &NODE(d, rep[gi].data[0]));
					ShclVecLead lead = {0};
					for (size_t k = 0; k < kept.len; k++) ShclVecLead_push(a, &lead, kept.data[k]);
					for (size_t k = 0; k < ct->leading.len; k++) ShclVecLead_push(a, &lead, ct->leading.data[k]);
					ct->leading = lead;
				}
			}
		} else {
			for (size_t i = 0; i < grp.len; i++) {
				size_t pos = grp.data[i];
				size_t ok = okids.data[pos];
				uint64_t hk = merge_hash(name, &over->nodes.data[ok].value);
				size_t b = (size_t)-1;
				for (ShclCMapEnt *e = cmap_first(&by_key, hk); e; e = cmap_next(e, hk))
					if (merge_eq(NODE(d, e->val).name, &NODE(d, e->val).value, name, &over->nodes.data[ok].value)) { b = e->val; break; }
				/* A raw block in the higher layer fills a same-named empty binding
				   below, exactly as a fence line fills one inside a single file.
				   Without it, merging two documents and parsing them run together
				   disagree: both bindings survive here and fold there, so the
				   merged output is not a formatter fixpoint. */
				if (b == (size_t)-1 && over->nodes.data[ok].value.kind == V_RAW) {
					uint64_t he = merge_hash(name, &ev);
					size_t emt = (size_t)-1;
					for (ShclCMapEnt *e = cmap_first(&by_key, he); e; e = cmap_next(e, he))
						if (merge_eq(NODE(d, e->val).name, &NODE(d, e->val).value, name, &ev)) { emt = e->val; break; }
					if (emt != (size_t)-1) {
						NODE(d, emt).value = w_dup_value(a, &over->nodes.data[ok].value);
						cmap_del(&by_key, he, emt);
						int seenk = 0;
						for (ShclCMapEnt *e = cmap_first(&by_key, hk); e; e = cmap_next(e, hk))
							if (merge_eq(NODE(d, e->val).name, &NODE(d, e->val).value, name, &over->nodes.data[ok].value)) { seenk = 1; break; }
						if (!seenk) cmap_put(t, &by_key, hk, emt);
						b = emt;
					}
				}
				if (b != (size_t)-1) { adopt_trivia(d, b, over, ok); w_overlay(d, b, over, ok); }
				else { app_at[pos] = w_clone_subtree(d, over, ok, bp); nappended++; }
			}
		}
	}
	if (!any_rep && nappended == 0) return;
	// Rebuild once: each replaced group lands at its name's first original
	// position (dropped nodes stay in the arena, unreferenced - reads and
	// emit walk children from the root), appends go at the end. One splice
	// per group, flagged on the group itself.
	int *spliced = (int *)arena_alloc(t, (nb ? nb : 1) * sizeof(int));
	for (size_t gi = 0; gi < nb; gi++) spliced[gi] = 0;
	/* Built in scratch and copied out exactly sized below: a builder growing in
	   the document arena abandons its doubling chain there, which cost about a
	   megabyte per merge on a 40000-child parent. */
	ShclVecSize nw = {0};
	for (size_t i = 0; i < base.len; i++) {
		size_t b = base.data[i]; ShclStr nm = NODE(d, b).name;
		uint64_t hn = cmap_hash(nm, s_empty());
		size_t g = (size_t)-1;
		for (ShclCMapEnt *e = cmap_first(&group_of, hn); e; e = cmap_next(e, hn))
			if (s_eq(order.data[e->val], nm)) { g = e->val; break; }
		if (g != (size_t)-1 && is_rep[g]) {
			if (!spliced[g]) {
				spliced[g] = 1;
				for (size_t k = 0; k < rep[g].len; k++) ShclVecSize_push(t, &nw, rep[g].data[k]);
			}
		} else {
			ShclVecSize_push(t, &nw, b);
		}
	}
	for (size_t k = 0; k < okids.len; k++) if (app_at[k] != (size_t)-1) ShclVecSize_push(t, &nw, app_at[k]);
	/* Into the parent's own array when it fits, which a leaf override always
	   does: an exact-sized copy per merge abandoned the whole list in the
	   document arena each time - 420 KB per merge on a 40000-key parent, where
	   the spec promises about a megabyte for five hundred. Appends that outgrow
	   it get room to double, so a stack of them amortizes. */
	ShclVecSize kept = NODE(d, bp).children;
	if (!kept.data || nw.len > kept.cap) {
		kept.cap = nw.len ? nw.len * 2 : 1;
		kept.data = (size_t *)arena_alloc(a, kept.cap * sizeof(size_t));
	}
	for (size_t k = 0; k < nw.len; k++) kept.data[k] = nw.data[k];
	kept.len = nw.len;
	NODE(d, bp).children = kept;
}

void shcl_merge(shcl_doc *d, const shcl_doc *over) {
	index_drop(d);
	d->lost += over->lost;
	ShclArena *a = &d->arena;
	arena_reset(&d->scratch); // merge temporaries (compare keys, clone lists) die here
	w_overlay(d, ROOT, over, ROOT);
	// Layers commonly share a footer; keeping one copy of each keeps a stack
	// of files from repeating it once per layer. Only the lines already here
	// count: a layer's own repeats are its content.
	size_t had = d->orphans.len;
	for (size_t i = 0; i < over->orphans.len; i++) {
		ShclStr ot = over->orphans.data[i].text;
		int dup = 0;
		for (size_t k = 0; k < had; k++) if (s_eq(d->orphans.data[k].text, ot)) { dup = 1; break; }
		if (!dup) ShclVecLead_push(a, &d->orphans, lead_make(s_dup(a, ot), over->orphans.data[i].blank_before));
	}
}

int64_t shcl_get_int(shcl_doc *d, const char *path, size_t plen, int64_t def) {
	shcl_read_i64 r = shcl_read_int(d, path, plen); return r.status == SHCL_GOOD ? r.value : def;
}
double shcl_get_float(shcl_doc *d, const char *path, size_t plen, double def) {
	shcl_read_f64 r = shcl_read_float(d, path, plen); return r.status == SHCL_GOOD ? r.value : def;
}
int shcl_get_bool(shcl_doc *d, const char *path, size_t plen, int def) {
	shcl_read_bool r = shcl_read_bool_(d, path, plen); return r.status == SHCL_GOOD ? r.value : def;
}
int64_t shcl_get_int_or(shcl_doc *d, const char *path, size_t plen, int64_t def) {
	return shcl_get_int(d, path, plen, def);
}
double shcl_get_float_or(shcl_doc *d, const char *path, size_t plen, double def) {
	return shcl_get_float(d, path, plen, def);
}
int shcl_get_bool_or(shcl_doc *d, const char *path, size_t plen, int def) {
	return shcl_get_bool(d, path, plen, def);
}

shcl_read_i64 shcl_read_int(shcl_doc *d, const char *path, size_t plen) {
	shcl_read_i64 R; ShclStr p; p.p = path; p.n = plen; ShclElement *el; shcl_status st = scalar_at(d, p, &el);
	if (st != SHCL_GOOD) { R.value = 0; R.status = st; return R; }
	int64_t v; if (parse_int_text(&d->scratch, el, d->strictness, &v)) { R.value = v; R.status = SHCL_GOOD; }
	else { R.value = 0; R.status = SHCL_BAD_TYPE; }
	return R;
}
shcl_read_f64 shcl_read_float(shcl_doc *d, const char *path, size_t plen) {
	shcl_read_f64 R; ShclStr p; p.p = path; p.n = plen; ShclElement *el; shcl_status st = scalar_at(d, p, &el);
	if (st != SHCL_GOOD) { R.value = 0; R.status = st; return R; }
	double v; if (parse_float_text(&d->scratch, el, d->strictness, &v)) { R.value = v; R.status = SHCL_GOOD; }
	else { R.value = 0; R.status = SHCL_BAD_TYPE; }
	return R;
}
shcl_read_bool shcl_read_bool_(shcl_doc *d, const char *path, size_t plen) {
	shcl_read_bool R; ShclStr p; p.p = path; p.n = plen; ShclElement *el; shcl_status st = scalar_at(d, p, &el);
	if (st != SHCL_GOOD) { R.value = 0; R.status = st; return R; }
	int v; if (parse_bool_text(&d->scratch, el->text, d->strictness, &v)) { R.value = v; R.status = SHCL_GOOD; }
	else { R.value = 0; R.status = SHCL_BAD_TYPE; }
	return R;
}
shcl_read_dt shcl_read_datetime(shcl_doc *d, const char *path, size_t plen) {
	shcl_read_dt R; ShclStr p; p.p = path; p.n = plen; ShclElement *el; shcl_status st = scalar_at(d, p, &el);
	memset(&R.value, 0, sizeof R.value); R.value.zone = SHCL_ZONE_NONE;
	if (st != SHCL_GOOD) { R.status = st; return R; }
	// scratch, not the doc arena: parse_datetime only allocates split temporaries
	// there, and the frac it hands back slices el->text, which outlives the call.
	if (parse_datetime(&d->scratch, el->text, &R.value)) R.status = SHCL_GOOD;
	else { memset(&R.value, 0, sizeof R.value); R.value.zone = SHCL_ZONE_NONE; R.status = SHCL_BAD_TYPE; }
	return R;
}
static ShclStr emit_element(ShclArena *a, const ShclElement *e);

shcl_read_str shcl_read_string(shcl_doc *d, const char *path, size_t plen) {
	shcl_read_str R; ShclStr p; p.p = path; p.n = plen; ShclValue *v; shcl_status st = value_at(d, p, &v);
	if (st != SHCL_GOOD) { R.value = s_empty(); R.status = st; return R; }
	if (v->kind == V_EMPTY) { R.value = s_empty(); R.status = SHCL_EMPTY; }
	else if (v->kind == V_RAW) { R.value = v->raw->content; R.status = SHCL_GOOD; }
	else if (v->nels == 1) { R.value = apply_escapes(&d->reads, v->els[0].text); R.status = SHCL_GOOD; }
	else {
		/* Canonical inline form (quoting + escapes intact), so the string
		   re-parses to the same array - not the bare display join. */
		ShclArena *a = &d->reads; ShclSB s = {0};
		for (size_t i = 0; i < v->nels; i++) { if (i) sb_puts(a, &s, ", "); sb_putS(a, &s, emit_element(a, &v->els[i])); }
		R.value = sb_S(&s); R.status = SHCL_GOOD;
	}
	return R;
}
shcl_read_str shcl_read_raw(shcl_doc *d, const char *path, size_t plen) {
	shcl_read_str R; ShclStr p; p.p = path; p.n = plen; ShclValue *v; shcl_status st = value_at(d, p, &v);
	R.value = s_empty(); R.status = st;
	if (st != SHCL_GOOD) return R;
	switch (v->kind) {
	case V_RAW: R.value = v->raw->content; break;
	case V_EMPTY: R.status = SHCL_EMPTY; break;
	case V_CELL: R.status = SHCL_BAD_TYPE; break;
	}
	return R;
}
shcl_read_str shcl_read_raw_info(shcl_doc *d, const char *path, size_t plen) {
	shcl_read_str R; ShclStr p; p.p = path; p.n = plen; ShclValue *v; shcl_status st = value_at(d, p, &v);
	if (st != SHCL_GOOD) { R.value = s_empty(); R.status = st; return R; }
	if (v->kind == V_RAW) { R.value = v->raw->info; R.status = SHCL_GOOD; }
	else { R.value = s_empty(); R.status = SHCL_BAD_TYPE; }
	return R;
}

static shcl_read_i64_arr read_int_array_in(shcl_doc *d, ShclArena *a, ShclStr p) {
	shcl_read_i64_arr R; ShclElement **els; shcl_status *sts; size_t n;
	shcl_status st = array_elements(d, a, p, &els, &sts, &n);
	if (st != SHCL_GOOD && st != SHCL_EMPTY) { R.values = NULL; R.n = 0; R.status = st; R.statuses = NULL; return R; }
	int64_t *out = (int64_t *)arena_alloc(a, (n ? n : 1) * sizeof(int64_t));
	for (size_t i = 0; i < n; i++) { int64_t v; if (els[i] && parse_int_text(&d->scratch, els[i], d->strictness, &v)) out[i] = v; else { out[i] = 0; if (els[i]) sts[i] = SHCL_BAD_TYPE; } }
	R.values = out; R.n = n; R.status = worst_slot(sts, n, st); R.statuses = sts; return R;
}
shcl_read_i64_arr shcl_read_int_array(shcl_doc *d, const char *path, size_t plen) {
	ShclStr p; p.p = path; p.n = plen;
	return read_int_array_in(d, &d->reads, p);
}
shcl_read_f64_arr shcl_read_float_array(shcl_doc *d, const char *path, size_t plen) {
	shcl_read_f64_arr R; ShclStr p; p.p = path; p.n = plen; ShclElement **els; shcl_status *sts; size_t n;
	shcl_status st = array_elements(d, &d->reads, p, &els, &sts, &n);
	if (st != SHCL_GOOD && st != SHCL_EMPTY) { R.values = NULL; R.n = 0; R.status = st; R.statuses = NULL; return R; }
	double *out = (double *)arena_alloc(&d->reads, (n ? n : 1) * sizeof(double));
	for (size_t i = 0; i < n; i++) { double v; if (els[i] && parse_float_text(&d->scratch, els[i], d->strictness, &v)) out[i] = v; else { out[i] = 0; if (els[i]) sts[i] = SHCL_BAD_TYPE; } }
	R.values = out; R.n = n; R.status = worst_slot(sts, n, st); R.statuses = sts; return R;
}
shcl_read_bool_arr shcl_read_bool_array(shcl_doc *d, const char *path, size_t plen) {
	shcl_read_bool_arr R; ShclStr p; p.p = path; p.n = plen; ShclElement **els; shcl_status *sts; size_t n;
	shcl_status st = array_elements(d, &d->reads, p, &els, &sts, &n);
	if (st != SHCL_GOOD && st != SHCL_EMPTY) { R.values = NULL; R.n = 0; R.status = st; R.statuses = NULL; return R; }
	int *out = (int *)arena_alloc(&d->reads, (n ? n : 1) * sizeof(int));
	for (size_t i = 0; i < n; i++) { int v; if (els[i] && parse_bool_text(&d->scratch, els[i]->text, d->strictness, &v)) out[i] = v; else { out[i] = 0; if (els[i]) sts[i] = SHCL_BAD_TYPE; } }
	R.values = out; R.n = n; R.status = worst_slot(sts, n, st); R.statuses = sts; return R;
}
shcl_read_dt_arr shcl_read_datetime_array(shcl_doc *d, const char *path, size_t plen) {
	shcl_read_dt_arr R; ShclStr p; p.p = path; p.n = plen; ShclElement **els; shcl_status *sts; size_t n;
	shcl_status st = array_elements(d, &d->reads, p, &els, &sts, &n);
	if (st != SHCL_GOOD && st != SHCL_EMPTY) { R.values = NULL; R.n = 0; R.status = st; R.statuses = NULL; return R; }
	shcl_datetime *out = (shcl_datetime *)arena_alloc(&d->reads, (n ? n : 1) * sizeof(shcl_datetime));
	for (size_t i = 0; i < n; i++) {
		memset(&out[i], 0, sizeof out[i]); out[i].zone = SHCL_ZONE_NONE;
		if (els[i]) { if (!parse_datetime(&d->scratch, els[i]->text, &out[i])) { memset(&out[i], 0, sizeof out[i]); out[i].zone = SHCL_ZONE_NONE; sts[i] = SHCL_BAD_TYPE; } }
	}
	R.values = out; R.n = n; R.status = worst_slot(sts, n, st); R.statuses = sts; return R;
}
shcl_read_str_arr shcl_read_string_array(shcl_doc *d, const char *path, size_t plen) {
	shcl_read_str_arr R; ShclStr p; p.p = path; p.n = plen; ShclElement **els; shcl_status *sts; size_t n;
	shcl_status st = array_elements(d, &d->reads, p, &els, &sts, &n);
	if (st != SHCL_GOOD && st != SHCL_EMPTY) { R.values = NULL; R.n = 0; R.status = st; R.statuses = NULL; return R; }
	shcl_str *out = (shcl_str *)arena_alloc(&d->reads, (n ? n : 1) * sizeof(shcl_str));
	for (size_t i = 0; i < n; i++) out[i] = els[i] ? apply_escapes(&d->reads, els[i]->text) : s_empty();
	R.values = out; R.n = n; R.status = worst_slot(sts, n, st); R.statuses = sts; return R;
}

// --- formatter (canonical output) -------------------------------------------

static void bare_quote_counts(ShclStr t, size_t *dq, size_t *sq) {
	*dq = 0; *sq = 0; size_t i = 0;
	while (i < t.n) {
		uint32_t c; size_t l = utf8_decode(t.p, t.n, i, &c); i += l;
		if (c == '\\') { if (i < t.n) { uint32_t e; i += utf8_decode(t.p, t.n, i, &e); } continue; }
		if (c == '"') (*dq)++; else if (c == '\'') (*sq)++;
	}
}
static ShclStr quote_text(ShclArena *a, ShclStr t) {
	/* A dangling trailing backslash would turn the closing quote into an
	   escape pair - the scanner reads the path back wrong, or not at all.
	   Store the doubled spelling (identical on string read), the same rule
	   the element parser applies to bare text. */
	if (t.n && t.p[t.n - 1] == '\\') t = norm_dangling(a, t);
	size_t dq, sq; bare_quote_counts(t, &dq, &sq);
	ShclSB s = {0};
	if (dq == 0) { sb_putc(a, &s, '"'); sb_putS(a, &s, t); sb_putc(a, &s, '"'); }
	else if (sq == 0) { sb_putc(a, &s, '\''); sb_putS(a, &s, t); sb_putc(a, &s, '\''); }
	else {
		sb_putc(a, &s, '"'); size_t i = 0;
		while (i < t.n) {
			uint32_t c; size_t l = utf8_decode(t.p, t.n, i, &c); i += l;
			if (c == '\\') { sb_putc(a, &s, '\\'); if (i < t.n) { uint32_t e; size_t l2 = utf8_decode(t.p, t.n, i, &e); i += l2; sb_put_cp(a, &s, e); } }
			else if (c == '"') sb_puts(a, &s, "\\\"");
			else sb_put_cp(a, &s, c);
		}
		sb_putc(a, &s, '"');
	}
	return sb_S(&s);
}
// is_data_format: true when the text reads as an int, float, bool, or datetime
// at standard strictness - fixed there deliberately, so canonical form cannot
// vary with the load strictness.
// One pass over the bytes before any coercion. At Standard the int, float and
// datetime forms all require at least one ASCII digit; the only formats that do
// not are the boolean words, and the longest of those is "false". An ordinary
// quoted string fails both tests, so emit stops running four full coercions on
// every quoted element it writes.
static int is_data_format(ShclArena *a, const ShclElement *e) {
	int64_t iv; double fv; int bv; shcl_datetime dv;
	int has_digit = 0;
	for (size_t i = 0; i < e->text.n; i++) if (e->text.p[i] >= '0' && e->text.p[i] <= '9') { has_digit = 1; break; }
	if (!has_digit) {
		ShclStr t = s_trim(e->text);
		return t.n <= 5 && parse_bool_text(a, t, SHCL_STANDARD, &bv);
	}
	if (parse_int_text(a, e, SHCL_STANDARD, &iv)) return 1;
	if (parse_float_text(a, e, SHCL_STANDARD, &fv)) return 1;
	if (parse_bool_text(a, e->text, SHCL_STANDARD, &bv)) return 1;
	if (parse_datetime(a, e->text, &dv)) return 1;
	return 0;
}
// Minimal quoting: bare unless a reserved character (or lookalike hazard) forces it.
// One addition: an author-quoted element keeps its quotes unless the text reads as
// one of SHCL's own data formats - quoting those is just spelling (readers type the
// value either way), but quoting a plain string is the escape and must survive
// canonicalization. This clause only ever adds quoting, so a bare emit stays safe.
static ShclStr emit_element(ShclArena *a, const ShclElement *e) {
	ShclStr t = e->text;
	int needs = (t.n == 0);
	if (!needs) {
		size_t i = 0;
		while (i < t.n) { uint32_t c; size_t l = utf8_decode(t.p, t.n, i, &c); i += l;
			if (c == ' ' || c == '\t' || c == ',' || c == ':' || c == '#' || c == '"' || c == '\'' || c == '[' || c == ']') { needs = 1; break; } }
	}
	/* Edge whitespace beyond the space/tab above still has to force quotes: the
	   parser trims the full White_Space set, so a bare NBSP (or VT, FF, NEL,
	   ideographic space) at either end would not survive the reload. Edges only
	   - interior whitespace is never trimmed and quoting it would move bytes. */
	if (!needs && t.n) {
		uint32_t f, l; utf8_decode(t.p, t.n, 0, &f); utf8_last(t, &l);
		if (is_ws(f) || is_ws(l)) needs = 1;
	}
	if (!needs) { ShclFence f = fence_open(t); if (f.ok) needs = 1; }
	if (!needs && e->quoted && !is_data_format(a, e)) needs = 1;
	return needs ? quote_text(a, t) : t;
}
/* Emit a stored (escape-resolved) name in a spelling that reads back as the
   same name: bare when it can be, else quoted with the escapes apply_escapes
   undoes. This is a true inverse of the name parse, which quote_text is not -
   that one picks a quote style to AVOID escaping and never escapes a backslash,
   which is right for a value (stored in its escaped spelling) and wrong for a
   name (stored resolved). */
static ShclStr escape_name(ShclArena *a, ShclStr name) {
	if (name.n > 0) {
		int allbare = 1; size_t i = 0;
		while (i < name.n) { uint32_t c; size_t l = utf8_decode(name.p, name.n, i, &c); i += l; if (!is_bare_name_char(c)) { allbare = 0; break; } }
		if (allbare) return name;
	}
	ShclSB b = {0};
	sb_putc(a, &b, '"');
	for (size_t i = 0; i < name.n; i++) {
		char c = name.p[i];
		if (c == '\\') sb_puts(a, &b, "\\\\");
		else if (c == '"') sb_puts(a, &b, "\\\"");
		else if (c == '\t') sb_puts(a, &b, "\\t");
		else if (c == '\n') sb_puts(a, &b, "\\n");
		else sb_putc(a, &b, c);
	}
	sb_putc(a, &b, '"');
	return sb_S(&b);
}
static ShclStr emit_name(ShclArena *a, ShclStr name) { return escape_name(a, name); }
/* Inline comment, canonically two spaces before the `#`. */
static void emit_trailing(ShclArena *a, ShclSB *out, ShclStr trailing) {
	if (trailing.n) { sb_puts(a, out, "  "); sb_putS(a, out, trailing); }
}
// Emit a sibling run. The parent walk already knows whether an earlier
// same-name sibling is empty (the raw same-line-fence hazard), so one
// seen-empties set here replaces a per-child rescan of the whole run.
static void emit_node(shcl_doc *d, size_t idx, size_t depth, int would_merge, ShclSB *out);
static void emit_children(shcl_doc *d, const ShclVecSize *kids, size_t depth, ShclSB *out) {
	ShclCMap empties; memset(&empties, 0, sizeof empties);
	for (size_t i = 0; i < kids->len; i++) {
		size_t c = kids->data[i];
		ShclNode *n = &NODE(d, c);
		uint64_t h = cmap_hash(n->name, s_empty());
		int seen = 0; /* entries name the empty sibling, so a hit verifies */
		for (ShclCMapEnt *e = cmap_first(&empties, h); e; e = cmap_next(e, h))
			if (s_eq(NODE(d, e->val).name, n->name)) { seen = 1; break; }
		int wm = n->value.kind == V_RAW && seen;
		if (v_is_empty(&n->value) && !seen)
			cmap_put(&d->scratch, &empties, h, c);
		emit_node(d, c, depth, wm, out);
	}
}

static void emit_node(shcl_doc *d, size_t idx, size_t depth, int would_merge, ShclSB *out) {
	/* The whole emit - the output buffer and the quoted/escaped spellings both -
	   is built in scratch; shcl_to_canonical copies the finished bytes into the
	   document arena once. Building it there instead retained several times the
	   output on every save, in an arena that cannot give it back. */
	ShclArena *a = &d->scratch;
	ShclNode *node = &NODE(d, idx);
	ShclValue *v = &node->value;
	ShclVecLead lead = triv_leading(node);
	ShclStr trailing = triv_trailing(node);
	/* Same-line fence spelling can't carry an inline comment (an unbalanced
	   quote in the info-string could hide the `#` on reparse), so its trailing
	   comment joins the leading lines instead; the flag comes from the
	   parent's walk. Each blank rides its own comment (or the binding line),
	   never as the first output line. */
	for (size_t k = 0; k < lead.len; k++) {
		if (lead.data[k].blank_before && out->len) sb_putc(a, out, '\n');
		for (size_t z = 0; z < depth; z++) sb_putc(a, out, '\t');
		sb_putS(a, out, lead.data[k].text); sb_putc(a, out, '\n');
	}
	if (node->blank_before && out->len) sb_putc(a, out, '\n');
	if (would_merge && trailing.n) {
		for (size_t z = 0; z < depth; z++) sb_putc(a, out, '\t');
		sb_putS(a, out, trailing); sb_putc(a, out, '\n');
	}
	for (size_t k = 0; k < depth; k++) sb_putc(a, out, '\t');
	sb_putS(a, out, emit_name(a, node->name));
	sb_putc(a, out, ':');
	if (v->kind == V_EMPTY) { emit_trailing(a, out, trailing); sb_putc(a, out, '\n'); }
	else if (v->kind == V_CELL) {
		sb_putc(a, out, ' ');
		for (size_t i = 0; i < v->nels; i++) { if (i) sb_puts(a, out, ", "); sb_putS(a, out, emit_element(a, &v->els[i])); }
		emit_trailing(a, out, trailing);
		sb_putc(a, out, '\n');
	} else {
		const ShclRawVal *r = v->raw;
		if (would_merge) sb_putc(a, out, ' ');
		else { emit_trailing(a, out, trailing); sb_putc(a, out, '\n'); }
		if (!would_merge) for (size_t k = 0; k < depth + 1; k++) sb_putc(a, out, '\t');
		for (size_t k = 0; k < r->fence_len; k++) sb_putc(a, out, (char)r->fence_char);
		if (r->info.n > 0) { if ((unsigned char)r->info.p[0] == r->fence_char) sb_putc(a, out, ' '); sb_putS(a, out, r->info); }
		sb_putc(a, out, '\n');
		if (r->content.n > 0) {
			size_t start = 0;
			for (size_t i = 0; i <= r->content.n; i++) if (i == r->content.n || r->content.p[i] == '\n') {
				ShclStr l = s_slice(r->content, start, i);
				if (l.n > 0) for (size_t z = 0; z < depth + 1; z++) sb_putc(a, out, '\t');
				sb_putS(a, out, l); sb_putc(a, out, '\n');
				start = i + 1;
			}
		}
		for (size_t k = 0; k < depth + 1; k++) sb_putc(a, out, '\t');
		for (size_t k = 0; k < r->fence_len; k++) sb_putc(a, out, (char)r->fence_char);
		sb_putc(a, out, '\n');
	}
	ShclVecSize ch = NODE(d, idx).children;
	emit_children(d, &ch, depth + 1, out);
	/* Comments this block owns with no child to carry them, one deeper. */
	ShclVecLead ins = triv_inside(&NODE(d, idx));
	for (size_t k = 0; k < ins.len; k++) {
		const ShclLead *c = &ins.data[k];
		if (c->blank_before && out->len) sb_putc(a, out, '\n');
		for (size_t z = 0; z < depth + 1; z++) sb_putc(a, out, '\t');
		sb_putS(a, out, c->text); sb_putc(a, out, '\n');
	}
	/* Comments that hung on this block after its last child. */
	ShclVecLead aft = triv_after(&NODE(d, idx));
	for (size_t k = 0; k < aft.len; k++) {
		const ShclLead *c = &aft.data[k];
		if (c->blank_before && out->len) sb_putc(a, out, '\n');
		for (size_t z = 0; z < depth; z++) sb_putc(a, out, '\t');
		sb_putS(a, out, c->text); sb_putc(a, out, '\n');
	}
}
/* The canonical text, built in scratch (valid until the next resolve). Emit's
   own temporaries go there too, so a program that saves periodically does not
   grow by several times the output on every save. */
static ShclStr emit_canonical(shcl_doc *d) {
	arena_reset(&d->scratch);
	ShclSB out = {0};
	/* Pre-sized by node count, so the builder does not double its way up. */
	out.cap = d->nodes.len * 24;
	out.data = (char *)arena_alloc(&d->scratch, out.cap);
	ShclVecSize rc = NODE(d, ROOT).children;
	emit_children(d, &rc, 0, &out);
	/* Comments that never found a following line re-emit at the end. */
	for (size_t k = 0; k < d->orphans.len; k++) {
		if (d->orphans.data[k].blank_before && out.len) sb_putc(&d->scratch, &out, '\n');
		sb_putS(&d->scratch, &out, d->orphans.data[k].text); sb_putc(&d->scratch, &out, '\n');
	}
	return sb_S(&out);
}
shcl_str shcl_to_canonical(shcl_doc *d) {
	/* The returned bytes live in the document arena - that is the documented
	   contract. A save goes through emit_canonical directly, so it retains
	   nothing (200 saves of 79 KB grew a document by 17 MB). */
	return s_dup(&d->reads, emit_canonical(d));
}

// --- format helpers + remaining public API ----------------------------------

// Whether a decimal spelling reads back as exactly v, decided with integer
// arithmetic rather than strtod: more than one C runtime (msvcrt, and wine's)
// parses some 15-digit spellings one ulp off, and a spelling that only reads
// back on a correct libc is not a spelling every binding agrees on. A double
// is m * 2^e; a decimal d * 10^k reads back when it sits inside v's rounding
// interval, half a spacing each way, except below a power of two where the
// spacing halves. A midpoint reads back only when m is even (ties to even).
// Both ends are scaled to integers by 2^S * 10^T once per value; a candidate
// then costs one small multiply and a shift before the compare.
typedef struct { uint32_t w[96]; int n; } ShclBig;
static void big_set(ShclBig *b, uint64_t v) { memset(b, 0, sizeof *b); b->w[0] = (uint32_t)v; b->w[1] = (uint32_t)(v >> 32); b->n = b->w[1] ? 2 : (b->w[0] ? 1 : 0); }
static void big_mul_small(ShclBig *b, uint32_t m) {
	uint64_t carry = 0;
	for (int i = 0; i < b->n; i++) { uint64_t t = (uint64_t)b->w[i] * m + carry; b->w[i] = (uint32_t)t; carry = t >> 32; }
	if (carry && b->n < (int)(sizeof b->w / sizeof b->w[0])) b->w[b->n++] = (uint32_t)carry;
}
static void big_mul_pow10(ShclBig *b, int t) {
	static const uint32_t p10[9] = { 1u, 10u, 100u, 1000u, 10000u, 100000u, 1000000u, 10000000u, 100000000u };
	for (; t >= 9; t -= 9) big_mul_small(b, 1000000000u);
	if (t > 0 && t < 9) big_mul_small(b, p10[t]);
}
static void big_shl(ShclBig *b, int bits) {
	int limbs = bits / 32, cap = (int)(sizeof b->w / sizeof b->w[0]);
	if (limbs && b->n) {
		if (b->n + limbs > cap) limbs = cap - b->n;
		memmove(b->w + limbs, b->w, (size_t)b->n * sizeof b->w[0]);
		memset(b->w, 0, (size_t)limbs * sizeof b->w[0]);
		b->n += limbs;
	}
	if (bits % 32) big_mul_small(b, 1u << (bits % 32));
}
static int big_cmp(const ShclBig *a, const ShclBig *b) {
	if (a->n != b->n) return a->n < b->n ? -1 : 1;
	for (int i = a->n; i-- > 0;) if (a->w[i] != b->w[i]) return a->w[i] < b->w[i] ? -1 : 1;
	return 0;
}
typedef struct { ShclBig lo, hi; int S, T, even; } ShclF64Interval;
// exp10 is the decimal exponent of v's 17-digit spelling: the smallest k any
// shorter spelling can carry is exp10 - 16, which fixes T for all of them.
static void f64_interval(double v, int exp10, ShclF64Interval *iv) {
	uint64_t bits; memcpy(&bits, &v, sizeof bits);
	int E = (int)((bits >> 52) & 0x7FF); uint64_t F = bits & 0xFFFFFFFFFFFFFull;
	uint64_t m = E ? (F | (1ull << 52)) : F; int e = E ? E - 1075 : -1074;
	int above = e - 1, below = (E > 1 && F == 0) ? e - 2 : e - 1;   // log2 of each half-width
	iv->S = below < 0 ? -below : 0;
	iv->T = exp10 - 16 < 0 ? 16 - exp10 : 0;
	iv->even = (m & 1) == 0;
	// v -+ 2^x = (m * 2^(e-x) -+ 1) * 2^x, and e - x is 1 or 2.
	big_set(&iv->lo, (m << (e - below)) - 1); big_shl(&iv->lo, below + iv->S); big_mul_pow10(&iv->lo, iv->T);
	big_set(&iv->hi, (m << (e - above)) + 1); big_shl(&iv->hi, above + iv->S); big_mul_pow10(&iv->hi, iv->T);
}
static int f64_reads_back(const char *tmp, const ShclF64Interval *iv) {
	// The spelling: digits then an exponent, sign already known to match v.
	const char *s = tmp; if (*s == '-' || *s == '+') s++;
	uint64_t d = 0; int nd = 0;
	for (; *s && *s != 'e' && *s != 'E'; s++) if (*s >= '0' && *s <= '9') { d = d * 10 + (uint64_t)(*s - '0'); nd++; }
	if (!nd || *s == '\0') return 0;
	int k = atoi(s + 1) - (nd - 1);
	if (k + iv->T < 0) return 0;
	ShclBig D; big_set(&D, d); big_mul_pow10(&D, k + iv->T); big_shl(&D, iv->S);
	int cl = big_cmp(&D, &iv->lo), ch = big_cmp(&D, &iv->hi);
	if (cl > 0 && ch < 0) return 1;
	return (cl == 0 || ch == 0) && iv->even;
}

// The correctly rounded string at a precision is the closest one, but at a
// power of two the rounding interval is lopsided, and the neighbor one digit
// up or down can read back while the closest does not. The shortest-digits
// algorithms the other bindings use find it; stepping the last digit of the
// "%.*e" text in tmp by delta (with carry) and reading it back does the same.
// A carry past the leading digit is a shorter spelling, already tried.
static int f64_neighbor(const char *tmp, const ShclF64Interval *iv, int delta, char *out) {
	strcpy(out, tmp);
	char *e = strchr(out, 'e');
	if (!e || e == out) return 0;
	char *p = e - 1;
	for (;;) {
		if (*p >= '0' && *p <= '9') {
			int d = *p - '0' + delta;
			if (d >= 0 && d <= 9) { *p = (char)('0' + d); break; }
			*p = (char)(d < 0 ? '9' : '0');
		}
		if (p == out) return 0;
		p--;
	}
	if (*(out[0] == '-' ? out + 1 : out) == '0') return 0;
	return f64_reads_back(out, iv);
}

size_t shcl_format_f64(double v, char *out) {
	if (isnan(v)) { memcpy(out, "NaN", 3); return 3; }
	if (isinf(v)) { if (v < 0) { memcpy(out, "-inf", 4); return 4; } memcpy(out, "inf", 3); return 3; }
	if (v == 0.0) { if (signbit(v)) { memcpy(out, "-0", 2); return 2; } out[0] = '0'; return 1; }
	char tmp[64], alt[64]; int prec;
	// A locale's decimal point is whatever it is; the digit walks skip it.
	ShclF64Interval iv;
	snprintf(tmp, sizeof tmp, "%.16e", v);
	f64_interval(v, atoi(strchr(tmp, 'e') + 1), &iv);
	for (prec = 1; prec <= 17; prec++) {
		snprintf(tmp, sizeof tmp, "%.*e", prec - 1, v);
		if (f64_reads_back(tmp, &iv)) break;
		if (f64_neighbor(tmp, &iv, 1, alt) || f64_neighbor(tmp, &iv, -1, alt)) { memcpy(tmp, alt, sizeof tmp); break; }
	}
	{
		const char *dp = dec_point(); size_t dn = strlen(dp);
		if (dn != 1 || *dp != '.') {
			char *q = strstr(tmp, dp);
			if (q) { *q = '.'; memmove(q + 1, q + dn, strlen(q + dn) + 1); }
		}
	}
	const char *s = tmp; int neg = 0;
	if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++;
	char digits[24]; int nd = 0;
	digits[nd++] = *s++;
	if (*s == '.') { s++; while (*s >= '0' && *s <= '9') digits[nd++] = *s++; }
	int E = 0;
	if (*s == 'e' || *s == 'E') { s++; int es = 1; if (*s == '-') { es = -1; s++; } else if (*s == '+') s++; while (*s >= '0' && *s <= '9') { E = E * 10 + (*s - '0'); s++; } E *= es; }
	int pointPos = E + 1;
	char *o = out; if (neg) *o++ = '-';
	if (pointPos <= 0) { *o++ = '0'; *o++ = '.'; for (int z = 0; z < -pointPos; z++) *o++ = '0'; for (int k = 0; k < nd; k++) *o++ = digits[k]; }
	else if (pointPos >= nd) { for (int k = 0; k < nd; k++) *o++ = digits[k]; for (int z = 0; z < pointPos - nd; z++) *o++ = '0'; }
	else { for (int k = 0; k < pointPos; k++) *o++ = digits[k]; *o++ = '.'; for (int k = pointPos; k < nd; k++) *o++ = digits[k]; }
	return (size_t)(o - out);
}
size_t shcl_datetime_str(const shcl_datetime *dt, char *out) {
	// Every field is public, so a hand-built struct can carry values parsing
	// never yields (a -1 sentinel, epoch seconds in sec): render whole into a
	// worst-case local buffer (~109 bytes), then clamp the copy to the
	// documented SHCL_DT_BUF. Parsed values stay under the clamp (<= 56 with
	// the frac cap), so their output is unchanged.
	char b[128];
	char *o = b;
	if (dt->has_date) { o += sprintf(o, "%04d-%02u-%02u", dt->year, dt->month, dt->day); if (dt->has_time) *o++ = 'T'; }
	if (dt->has_time) {
		o += sprintf(o, "%02u:%02u", dt->hour, dt->minute);
		if (dt->has_sec) o += sprintf(o, ":%02u", dt->sec);
		if (dt->has_frac) {
			// frac keeps its own cap: the fixed parts plus 30 digits stay
			// inside the clamp for every parsed value.
			size_t fn = dt->frac.n > 30 ? 30 : dt->frac.n;
			*o++ = '.'; memcpy(o, dt->frac.p, fn); o += fn;
		}
	}
	if (dt->zone == SHCL_ZONE_UTC) *o++ = 'Z';
	else if (dt->zone == SHCL_ZONE_OFFSET) {
		// widen before negating: INT32_MIN has no 32-bit negation
		long long off = dt->off_min; char sign = off < 0 ? '-' : '+'; long long ao = off < 0 ? -off : off;
		o += sprintf(o, "%c%02lld:%02lld", sign, ao / 60, ao % 60);
	}
	size_t n = (size_t)(o - b);
	if (n > SHCL_DT_BUF) n = SHCL_DT_BUF;
	memcpy(out, b, n);
	return n;
}
int shcl_status_code(shcl_status s) {
	switch (s) { case SHCL_GOOD: return 0; case SHCL_EMPTY: return 2; case SHCL_NOT_FOUND: return 3; case SHCL_BAD_TYPE: return 4; case SHCL_MULTIPLE: return 5; }
	return 1;
}
const char *shcl_status_name(shcl_status s) {
	switch (s) { case SHCL_GOOD: return "Good"; case SHCL_EMPTY: return "Empty"; case SHCL_NOT_FOUND: return "NotFound"; case SHCL_BAD_TYPE: return "BadType"; case SHCL_MULTIPLE: return "Multiple"; }
	return "Good";
}
int shcl_status_ok(shcl_status s) { return s == SHCL_GOOD || s == SHCL_EMPTY; }
int shcl_strictness_from_arg(const char *s, size_t n, shcl_strictness *out) {
	char buf[16]; if (n >= sizeof buf) return 0;
	for (size_t i = 0; i < n; i++) { unsigned char c = (unsigned char)s[i]; buf[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : (char)c; }
	buf[n] = '\0';
	if (!strcmp(buf, "loose") || !strcmp(buf, "1")) { *out = SHCL_LOOSE; return 1; }
	if (!strcmp(buf, "standard") || !strcmp(buf, "2")) { *out = SHCL_STANDARD; return 1; }
	if (!strcmp(buf, "strict") || !strcmp(buf, "3")) { *out = SHCL_STRICT; return 1; }
	return 0;
}

shcl_doc *shcl_parse(const char *text, size_t len) { return do_parse(text, len, SHCL_STANDARD, 0, 0, 0); }
shcl_doc *shcl_parse_with(const char *text, size_t len, shcl_strictness s) { return do_parse(text, len, s, 0, 0, 0); }
/* Parse with resource caps beside the strictness, for input the consumer does
   not control: a document amplifies to many times its byte size in memory, so
   a size cap alone cannot bound what a load allocates. max_nodes stops the
   parse once a line takes the node count past it - one E020 error, and the
   unparsed remainder counts as lost so a save cannot silently truncate.
   max_elements refuses any line whose array would hold more elements (E021,
   that line alone is skipped). 0 disables a cap, making this shcl_parse_with.
   Both caps are parse-time only; the write API is the consumer's own
   arithmetic. A cap diagnostic is an error, so shcl_error_count answers
   whether a Strict load would have failed; the parsed part stays readable. */
shcl_doc *shcl_parse_limited(const char *text, size_t len, shcl_strictness s, size_t max_nodes, size_t max_elements, size_t max_diags) { return do_parse(text, len, s, max_nodes, max_elements, max_diags); }
void shcl_free(shcl_doc *d) { if (!d) return; free(d->nodes.data); arena_free(&d->arena); arena_free(&d->scratch); arena_free(&d->reads); arena_free(&d->index_arena); free(d); }
void shcl_reads_release(shcl_doc *d) { if (d) arena_reset(&d->reads); }
void shcl_compact(shcl_doc *d) {
	if (!d) return;
	shcl_doc *n = shcl_new();
	if (!n) return;
	ShclArena *a = &n->arena;
	// The tree, root's own trivia included, then everything the load recorded
	// that a save or a strict gate reads off the document.
	const ShclNode *root = &d->nodes.data[ROOT];
	NODE(n, ROOT).blank_before = root->blank_before;
	if (root->trivia) NODE(n, ROOT).trivia = w_clone_trivia(a, root->trivia);
	for (size_t i = 0; i < root->children.len; i++) {
		size_t c = w_clone_subtree(n, d, root->children.data[i], ROOT);
		ShclVecSize_push(a, &NODE(n, ROOT).children, c);
	}
	for (size_t i = 0; i < d->diags.len; i++) push_diag(n, d->diags.data[i].line, d->diags.data[i].sev, d->diags.data[i].code, s_dup(a, d->diags.data[i].message));
	for (size_t i = 0; i < d->orphans.len; i++) ShclVecLead_push(a, &n->orphans, lead_make(s_dup(a, d->orphans.data[i].text), d->orphans.data[i].blank_before));
	n->strictness = d->strictness;
	n->lost = d->lost;
	// Swap the rebuilt document in and give the old storage back.
	shcl_doc old = *d;
	*d = *n;
	free(n);
	free(old.nodes.data); arena_free(&old.arena); arena_free(&old.scratch); arena_free(&old.reads); arena_free(&old.index_arena);
}
int shcl_strict_failed(const shcl_doc *d) {
	if (d->strictness != SHCL_STRICT) return 0;
	for (size_t i = 0; i < d->diags.len; i++) if (d->diags.data[i].sev == SHCL_SEV_ERROR) return 1;
	return 0;
}
shcl_strictness shcl_strictness_of(const shcl_doc *d) { return d->strictness; }
size_t shcl_diag_count(const shcl_doc *d) { return d->diags.len; }
size_t shcl_diag_line(const shcl_doc *d, size_t i) { return d->diags.data[i].line; }
shcl_severity shcl_diag_severity(const shcl_doc *d, size_t i) { return d->diags.data[i].sev; }
shcl_str shcl_diag_message(const shcl_doc *d, size_t i) { return d->diags.data[i].message; }
const char *shcl_diag_code(const shcl_doc *d, size_t i) { return d->diags.data[i].code; }

// ===========================================================================
// Validator: schema-as-SHCL
// The schema is an ordinary parsed document: a flat list of `field: <path>`
// instances whose children are the constraints (closed vocabulary - see
// spec.md "Schema validation"). Validation reuses the accessor's path scan and
// the typed coercions, so document strictness composes for free. Schema faults
// (V09x) come first and the surviving constraints still check the document;
// the unknown-field sweep skips only when a fault cost a path spelling. One
// line-number space per result.
// Everything (scratch and results) lives in the validation's own arena.

/* scratch: v_unknown's suggestion workspace. On the validation rather than in
   that frame so an allocation failure, which unwinds past it, can free it. */
struct shcl_validation { ShclArena arena, scratch; ShclVecDiag diags; };

static const char *v_schema_types[] = {
	"int", "float", "bool", "string", "datetime", "raw",
	"int-array", "float-array", "bool-array", "string-array", "datetime-array",
};

typedef enum { ALLOW_INTS, ALLOW_FLOATS, ALLOW_BOOLS, ALLOW_DATES, ALLOW_STRINGS } ShclAllowKind;

typedef struct {
	ShclStr path; // as written in the schema; message text only
	ShclVecSeg segs;
	const char *ty; // member of v_schema_types; NULL = untyped
	int required;
	int has_allowed; ShclAllowKind akind; size_t a_n;
	int64_t *a_ints; double *a_floats; int *a_bools; shcl_datetime *a_dates; ShclStr *a_strs;
	int has_min_i, has_max_i, has_min_f, has_max_f;
	int64_t min_i, max_i; double min_f, max_f;
	int has_repeat; uint64_t rep_lo, rep_hi;
	int reopen;                 // H002 suppressor only; validation ignores it
	ShclStr inherits;           // fragment mounted at this path (subtree shape); .n == 0 = none
	size_t inherits_line; // schema line of the `inherits` key, for V095
	// Generator-only (`shcl init`): validation ignores both. has_* gates them.
	int has_desc; ShclStr desc;
	int has_default; ShclStr default_text;
} ShclVCons;
DEFINE_VEC(ShclVecVCons, ShclVCons)

// An interpreted schema: the top-level constraints plus the named fragments
// their `inherits` keys can mount.
typedef struct { ShclStr name; ShclVecVCons fields; } ShclVFrag;
DEFINE_VEC(ShclVecVFrag, ShclVFrag)
// paths_complete: 0 when a fault cost the schema a path spelling (unreadable
// `field:` path, or a mount naming no declared fragment). Key-level faults
// keep their entry's chain, so only these two classes can turn declared
// fields into false unknowns - the sweep runs unless one of them happened.
typedef struct { ShclVecVCons cons; ShclVecVFrag frags; int paths_complete; } ShclVSchemaDef;

static const ShclVecVCons *v_frag_get(const ShclVSchemaDef *def, ShclStr name) {
	for (size_t i = 0; i < def->frags.len; i++)
		if (s_eq(def->frags.data[i].name, name)) return &def->frags.data[i].fields;
	return NULL;
}

static void v_diag(ShclArena *a, ShclVecDiag *out, size_t line, const char *code, ShclStr msg) {
	ShclDiag dg; dg.line = line; dg.sev = SHCL_SEV_ERROR; dg.message = msg; dg.code = code;
	ShclVecDiag_push(a, out, dg);
}
static ShclStr v_msgz(ShclArena *a, const char *z) { ShclStr s; s.p = z; s.n = strlen(z); return s_dup(a, s); }
static ShclStr v_msg3(ShclArena *a, const char *pre, ShclStr mid, const char *post) {
	ShclSB s = {0, 0, 0};
	sb_puts(a, &s, pre); sb_putS(a, &s, mid); sb_puts(a, &s, post);
	return sb_S(&s);
}
static ShclStr v_msg_key(ShclArena *a, const char *key) {
	ShclSB s = {0, 0, 0};
	sb_puts(a, &s, "bad schema constraint '"); sb_puts(a, &s, key); sb_puts(a, &s, "'");
	return sb_S(&s);
}

// One scalar constraint value (escapes applied), or 0.
static int v_single_text(ShclArena *a, const ShclValue *v, ShclStr *out) {
	if (v->kind != V_CELL || v->nels != 1) return 0;
	*out = apply_escapes(a, v->els[0].text);
	return 1;
}

// Field-wise datetime equality (struct compare would read unset fields).
/* Two datetimes naming the same moment, whatever the spelling. The struct
   mirrors what was written, so 12:00:00Z and 12:00:00+00:00 are different values
   field by field while naming one time, and 12:00:00 and 12:00:00.0 differ only
   in written precision. A [value] selector matches on text, but an allowed set
   is about the value, so it compares here. An absent zone is local and matches
   no zone at all - that is the one spelling difference that is a real
   difference. */
static ShclStr v_frac_key(ShclStr f) {
	while (f.n && f.p[f.n - 1] == '0') f.n--;
	return f;
}
/* Days since 1970-01-01 for a civil date, negative before it. */
static int64_t v_days_from_civil(int32_t y, uint32_t m, uint32_t d) {
	int64_t yy = m <= 2 ? (int64_t)y - 1 : (int64_t)y;
	int64_t era = (yy >= 0 ? yy : yy - 399) / 400;
	int64_t yoe = yy - era * 400;
	int64_t doy = (153 * (int64_t)((m + 9) % 12) + 2) / 5 + (int64_t)d - 1;
	int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + doe - 719468;
}
/* A zoned value as an instant: the written clock less its offset, the date
   carrying the day wrap. A time alone lives on a 24-hour cycle. */
static int64_t v_moment_minutes(const shcl_datetime *x, int32_t off) {
	int64_t hm = (x->has_time ? (int64_t)x->hour * 60 + x->minute : 0) - off;
	if (x->has_date) return v_days_from_civil(x->year, x->month, x->day) * 1440 + hm;
	return ((hm % 1440) + 1440) % 1440;
}
static int v_same_moment(const shcl_datetime *x, const shcl_datetime *y) {
	if (x->has_date != y->has_date || x->has_time != y->has_time) return 0;
	if (x->has_time && (x->has_sec != y->has_sec || (x->has_sec && x->sec != y->sec))) return 0;
	{
		ShclStr a; a.p = x->has_frac ? x->frac.p : ""; a.n = x->has_frac ? x->frac.n : 0;
		ShclStr b; b.p = y->has_frac ? y->frac.p : ""; b.n = y->has_frac ? y->frac.n : 0;
		if (!s_eq(v_frac_key(a), v_frac_key(b))) return 0;
	}
	{
		int xh = x->zone != SHCL_ZONE_NONE, yh = y->zone != SHCL_ZONE_NONE;
		if (xh != yh) return 0;
		if (!xh) {
			if (x->has_date && (x->year != y->year || x->month != y->month || x->day != y->day)) return 0;
			if (x->has_time && (x->hour != y->hour || x->minute != y->minute)) return 0;
			return 1;
		}
		int xo = x->zone == SHCL_ZONE_OFFSET ? x->off_min : 0;
		int yo = y->zone == SHCL_ZONE_OFFSET ? y->off_min : 0;
		return v_moment_minutes(x, xo) == v_moment_minutes(y, yo);
	}
}

// One `field:` instance (top-level or inside a fragment) -> a constraint into
// *out. Zero return = faults were reported and the constraint is dropped.
static int v_parse_field(ShclArena *a, shcl_doc *schema, size_t f, ShclVecDiag *faults, ShclVCons *out) {
	ShclNode *node = &NODE(schema, f);
	ShclStr path;
	if (!v_single_text(a, &node->value, &path)) {
		v_diag(a, faults, node->line, "V093", v_msgz(a, "bad schema path"));
		return 0;
	}
	ShclPathScan ps = scan_lookup(a, path);
	if (!ps.ok || ps.has_value) {
		v_diag(a, faults, node->line, "V093", v_msg3(a, "bad schema path: ", path, ""));
		return 0;
	}
	ShclVCons c; memset(&c, 0, sizeof c);
	c.path = path; c.segs = ps.segs;
	// Deferred so `min: 1` may precede `type: int` in the file.
	int required = -1;
	int reopen_seen = 0;
	size_t allowed_at = (size_t)-1, min_at = (size_t)-1, max_at = (size_t)-1, default_at = (size_t)-1;
	ShclVecSize kids = NODE(schema, f).children;
	for (size_t ki = 0; ki < kids.len; ki++) {
		ShclNode *kid = &NODE(schema, kids.data[ki]);
		if (v_is_empty(&kid->value)) continue; // dangling key: treated as absent
		if (s_eq(kid->name, s_lit("type"))) {
			ShclStr t;
			int ok = v_single_text(a, &kid->value, &t);
			const char *canon = NULL;
			if (ok) {
				ShclStr low = ascii_lower(a, t);
				for (size_t x = 0; x < sizeof v_schema_types / sizeof v_schema_types[0]; x++)
					if (s_eq(low, s_lit(v_schema_types[x]))) { canon = v_schema_types[x]; break; }
				if (canon) {
					if (c.ty) v_diag(a, faults, kid->line, "V092", v_msg_key(a, "type"));
					else c.ty = canon;
				} else {
					v_diag(a, faults, kid->line, "V091", v_msg3(a, "unknown schema type '", low, "'"));
				}
			} else {
				v_diag(a, faults, kid->line, "V092", v_msg_key(a, "type"));
			}
		} else if (s_eq(kid->name, s_lit("required"))) {
			ShclStr t; int b = 0;
			int ok = v_single_text(a, &kid->value, &t) && parse_bool_text(a, t, SHCL_STANDARD, &b);
			if (ok && required < 0) required = b;
			else v_diag(a, faults, kid->line, "V092", v_msg_key(a, "required"));
		} else if (s_eq(kid->name, s_lit("reopen"))) {
			/* Consumed by the H002 suppressor; validation itself ignores it,
			   but a bad value still faults so a typo cannot silently disavow
			   nothing. */
			ShclStr t; int b = 0;
			int ok = v_single_text(a, &kid->value, &t) && parse_bool_text(a, t, SHCL_STANDARD, &b);
			if (ok && !reopen_seen) { reopen_seen = 1; c.reopen = b; }
			else v_diag(a, faults, kid->line, "V092", v_msg_key(a, "reopen"));
		} else if (s_eq(kid->name, s_lit("allowed"))) {
			if (kid->value.kind == V_CELL && allowed_at == (size_t)-1) allowed_at = kids.data[ki];
			else v_diag(a, faults, kid->line, "V092", v_msg_key(a, "allowed"));
		} else if (s_eq(kid->name, s_lit("min"))) {
			if (kid->value.kind == V_CELL && kid->value.nels == 1 && min_at == (size_t)-1) min_at = kids.data[ki];
			else v_diag(a, faults, kid->line, "V092", v_msg_key(a, "min"));
		} else if (s_eq(kid->name, s_lit("max"))) {
			if (kid->value.kind == V_CELL && kid->value.nels == 1 && max_at == (size_t)-1) max_at = kids.data[ki];
			else v_diag(a, faults, kid->line, "V092", v_msg_key(a, "max"));
		} else if (s_eq(kid->name, s_lit("repeat"))) {
			if (kid->value.kind == V_CELL && !c.has_repeat && (kid->value.nels == 1 || kid->value.nels == 2)) {
				uint64_t lo, hi;
				if (parse_u64(kid->value.els[0].text, &lo) && parse_u64(kid->value.els[kid->value.nels - 1].text, &hi) && lo <= hi) {
					c.has_repeat = 1; c.rep_lo = lo; c.rep_hi = hi;
				} else {
					v_diag(a, faults, kid->line, "V092", v_msg_key(a, "repeat"));
				}
			} else {
				v_diag(a, faults, kid->line, "V092", v_msg_key(a, "repeat"));
			}
		} else if (s_eq(kid->name, s_lit("inherits"))) {
			ShclStr t;
			if (v_single_text(a, &kid->value, &t) && t.n && c.inherits.n == 0) {
				c.inherits = t; c.inherits_line = kid->line;
			} else {
				v_diag(a, faults, kid->line, "V092", v_msg_key(a, "inherits"));
			}
		} else if (s_eq(kid->name, s_lit("desc"))) {
			// Generator-only (`shcl init`); validation ignores it. First wins.
			// A comma in a sentence makes the value several elements, and the
			// comment is prose: take them all, spelled as written.
			if (!c.has_desc && kid->value.kind == V_CELL) {
				ShclSB s = {0, 0, 0};
				for (size_t x = 0; x < kid->value.nels; x++) { if (x) sb_puts(a, &s, ", "); sb_putS(a, &s, apply_escapes(a, kid->value.els[x].text)); }
				c.has_desc = 1; c.desc = sb_S(&s);
			}
		} else if (s_eq(kid->name, s_lit("default"))) {
			if (!c.has_default) {
				if (kid->value.kind == V_CELL) {
					ShclSB s = {0, 0, 0};
					for (size_t x = 0; x < kid->value.nels; x++) { if (x) sb_puts(a, &s, ", "); sb_putS(a, &s, emit_element(a, &kid->value.els[x])); }
					c.has_default = 1; c.default_text = sb_S(&s);
				}
				default_at = kids.data[ki];
			}
		} else {
			v_diag(a, faults, kid->line, "V090", v_msg3(a, "unknown schema key '", kid->name, "'"));
		}
	}
	c.required = required > 0;
	/* A raw block has no inline spelling, so a `default` that is one cannot
	   reach a generated line - it used to be dropped and the field emitted with
	   no value at all - and a `default` under `type: raw` goes out inline and
	   then fails its own type check. */
	if (default_at != (size_t)-1) {
		const ShclNode *dkid = &schema->nodes.data[default_at];
		int raw_typed = c.ty && !strcmp(c.ty, "raw");
		if (dkid->value.kind == V_RAW || (raw_typed && c.has_default))
			v_diag(a, faults, dkid->line, "V092", v_msg_key(a, "default"));
	}
	const char *base = c.ty ? c.ty : "string";
	size_t blen = strlen(base);
	if (blen > 6 && memcmp(base + blen - 6, "-array", 6) == 0) {
		// Base kind name, without the -array suffix (still a literal member).
		for (size_t x = 0; x < sizeof v_schema_types / sizeof v_schema_types[0]; x++)
			if (strlen(v_schema_types[x]) == blen - 6 && memcmp(v_schema_types[x], base, blen - 6) == 0) { base = v_schema_types[x]; break; }
	}
	if (allowed_at != (size_t)-1) {
		ShclNode *kid = &NODE(schema, allowed_at);
		ShclElement *els = kid->value.els; size_t n = kid->value.nels;
		// Schema values are read at Standard; only the document's values
		// coerce at the document's strictness.
		int ok = 1;
		c.a_n = n;
		if (strcmp(base, "int") == 0) {
			c.akind = ALLOW_INTS;
			c.a_ints = (int64_t *)arena_alloc(a, (n ? n : 1) * sizeof(int64_t));
			for (size_t x = 0; x < n && ok; x++) ok = parse_int_text(a, &els[x], SHCL_STANDARD, &c.a_ints[x]);
		} else if (strcmp(base, "float") == 0) {
			c.akind = ALLOW_FLOATS;
			c.a_floats = (double *)arena_alloc(a, (n ? n : 1) * sizeof(double));
			for (size_t x = 0; x < n && ok; x++) ok = parse_float_text(a, &els[x], SHCL_STANDARD, &c.a_floats[x]);
		} else if (strcmp(base, "bool") == 0) {
			c.akind = ALLOW_BOOLS;
			c.a_bools = (int *)arena_alloc(a, (n ? n : 1) * sizeof(int));
			for (size_t x = 0; x < n && ok; x++) ok = parse_bool_text(a, els[x].text, SHCL_STANDARD, &c.a_bools[x]);
		} else if (strcmp(base, "datetime") == 0) {
			c.akind = ALLOW_DATES;
			c.a_dates = (shcl_datetime *)arena_alloc(a, (n ? n : 1) * sizeof(shcl_datetime));
			for (size_t x = 0; x < n && ok; x++) ok = parse_datetime(a, els[x].text, &c.a_dates[x]);
		} else if (strcmp(base, "raw") == 0) {
			ok = 0; // a raw body has no element space to enumerate
		} else {
			c.akind = ALLOW_STRINGS;
			c.a_strs = (ShclStr *)arena_alloc(a, (n ? n : 1) * sizeof(ShclStr));
			for (size_t x = 0; x < n; x++) c.a_strs[x] = apply_escapes(a, els[x].text);
		}
		if (ok) c.has_allowed = 1;
		else v_diag(a, faults, kid->line, "V092", v_msg_key(a, "allowed"));
	}
	for (int mm = 0; mm < 2; mm++) {
		int is_min = mm == 0;
		size_t at = is_min ? min_at : max_at;
		if (at == (size_t)-1) continue;
		ShclNode *kid = &NODE(schema, at);
		const ShclElement *el = &kid->value.els[0];
		const char *key = is_min ? "min" : "max";
		if (strcmp(base, "int") == 0) {
			int64_t v;
			if (parse_int_text(a, el, SHCL_STANDARD, &v)) {
				if (is_min) { c.has_min_i = 1; c.min_i = v; }
				else { c.has_max_i = 1; c.max_i = v; }
			} else v_diag(a, faults, kid->line, "V092", v_msg_key(a, key));
		} else if (strcmp(base, "float") == 0) {
			double v;
			if (parse_float_text(a, el, SHCL_STANDARD, &v)) {
				if (is_min) { c.has_min_f = 1; c.min_f = v; }
				else { c.has_max_f = 1; c.max_f = v; }
			} else v_diag(a, faults, kid->line, "V092", v_msg_key(a, key));
		} else {
			v_diag(a, faults, kid->line, "V092", v_msg_key(a, key));
		}
	}
	/* A lower bound above the upper one admits nothing, so every value fails
	   twice and the schema, not the config, is what has to change. Reported at
	   the max line. The range goes, not the field: a key-level fault keeps its
	   entry, so the path still legalizes its name chain for the unknown-field
	   sweep, and the document is not told off twice per value for a range it
	   could never have satisfied. */
	if ((c.has_min_i && c.has_max_i && c.min_i > c.max_i)
	    || (c.has_min_f && c.has_max_f && c.min_f > c.max_f)) {
		size_t line = max_at != (size_t)-1 ? NODE(schema, max_at).line : node->line;
		v_diag(a, faults, line, "V092", v_msg_key(a, "max"));
		c.has_min_i = c.has_max_i = c.has_min_f = c.has_max_f = 0;
	}
	*out = c;
	return 1;
}

// Interpret a parsed schema document into constraints and fragments, plus any
// schema faults (V09x, schema-file lines). Whatever parsed cleanly is kept
// even when faults are present - a broken key drops that key, a broken field
// drops that field - so a caller can still check the document against the
// surviving constraints.
static void v_build_schema(ShclArena *a, shcl_doc *schema, ShclVSchemaDef *def, ShclVecDiag *faults) {
	def->paths_complete = 1;
	ShclVecSize top = NODE(schema, ROOT).children;
	for (size_t fi = 0; fi < top.len; fi++) {
		ShclNode *node = &NODE(schema, top.data[fi]);
		if (s_eq(node->name, s_lit("field"))) {
			ShclVCons c;
			if (v_parse_field(a, schema, top.data[fi], faults, &c)) ShclVecVCons_push(a, &def->cons, c);
			else def->paths_complete = 0;
		} else if (s_eq(node->name, s_lit("fragment"))) {
			ShclStr name;
			if (!v_single_text(a, &node->value, &name) || name.n == 0) {
				v_diag(a, faults, node->line, "V094", v_msgz(a, "bad schema fragment"));
				continue;
			}
			if (v_frag_get(def, name)) {
				v_diag(a, faults, node->line, "V094", v_msg3(a, "bad schema fragment '", name, "': duplicate"));
				continue;
			}
			ShclVFrag fr; fr.name = name; memset(&fr.fields, 0, sizeof fr.fields);
			ShclVecSize kids = NODE(schema, top.data[fi]).children;
			for (size_t ki = 0; ki < kids.len; ki++) {
				const ShclNode *kid = &NODE(schema, kids.data[ki]);
				if (s_eq(kid->name, s_lit("field"))) {
					ShclVCons c;
					if (v_parse_field(a, schema, kids.data[ki], faults, &c)) ShclVecVCons_push(a, &fr.fields, c);
					else def->paths_complete = 0;
				} else {
					ShclSB s = {0, 0, 0};
					sb_puts(a, &s, "bad schema fragment '"); sb_putS(a, &s, name);
					sb_puts(a, &s, "': unknown key '"); sb_putS(a, &s, kid->name); sb_puts(a, &s, "'");
					v_diag(a, faults, kid->line, "V094", sb_S(&s));
				}
			}
			ShclVecVFrag_push(a, &def->frags, fr);
		} else {
			v_diag(a, faults, node->line, "V090", v_msg3(a, "unknown schema key '", node->name, "'"));
		}
	}
	// Every mount must name a declared fragment; cycles (self or mutual) are
	// legal - expansion is demand-driven against a finite document.
	for (size_t g = 0; g <= def->frags.len; g++) {
		const ShclVecVCons *list = g == 0 ? &def->cons : &def->frags.data[g - 1].fields;
		for (size_t i = 0; i < list->len; i++) {
			const ShclVCons *c = &list->data[i];
			if (c->inherits.n && !v_frag_get(def, c->inherits)) {
				v_diag(a, faults, c->inherits_line, "V095", v_msg3(a, "unknown schema fragment '", c->inherits, "'"));
				def->paths_complete = 0;
			}
		}
	}
	// One constraint per line in practice, so line order = file order. Insertion
	// sort keeps equal lines stable (qsort is not stable).
	for (size_t i = 1; i < faults->len; i++) {
		ShclDiag key = faults->data[i];
		size_t j = i;
		while (j > 0 && faults->data[j - 1].line > key.line) { faults->data[j] = faults->data[j - 1]; j--; }
		faults->data[j] = key;
	}
}

// Levenshtein distance over codepoints capped at cap, for the "did you mean"
// prose (never the code): anything past the cap comes back as cap + 1. Only
// the band |i - j| <= cap of the table is computed, so a pair costs linear
// time in the names' length, and a length gap past the cap needs no table at
// all.
static size_t v_edit_distance(ShclArena *a, ShclStr sa, ShclStr sb, size_t cap) {
	ShclCPs ca = decode_cps(a, sa);
	ShclCPs cb = decode_cps(a, sb);
	size_t inf = cap + 1;
	if ((ca.n > cb.n ? ca.n - cb.n : cb.n - ca.n) > cap) return inf;
	size_t *prev = (size_t *)arena_alloc(a, (cb.n + 1) * sizeof(size_t));
	size_t *cur = (size_t *)arena_alloc(a, (cb.n + 1) * sizeof(size_t));
	for (size_t j = 0; j <= cb.n; j++) { prev[j] = j < inf ? j : inf; cur[j] = inf; }
	for (size_t i = 1; i <= ca.n; i++) {
		cur[0] = i < inf ? i : inf;
		size_t lo = i > cap ? i - cap : 1;
		size_t hi = i + cap < cb.n ? i + cap : cb.n;
		if (lo > 1) cur[lo - 1] = inf;
		size_t row_min = cur[0];
		for (size_t j = lo; j <= hi; j++) {
			size_t cost = ca.cp[i - 1] == cb.cp[j - 1] ? 0 : 1;
			size_t m = prev[j] + 1;
			if (cur[j - 1] + 1 < m) m = cur[j - 1] + 1;
			if (prev[j - 1] + cost < m) m = prev[j - 1] + cost;
			if (m > inf) m = inf;
			cur[j] = m;
			if (m < row_min) row_min = m;
		}
		if (hi < cb.n) cur[hi + 1] = inf;
		// No cell in a later row can come back under this row's minimum.
		if (row_min > cap) return inf;
		size_t *t = prev; prev = cur; cur = t;
	}
	return prev[cb.n];
}

// Closest legal sibling name (same parent chain, schema order, edit distance
// <= 2) appended as "; did you mean 'x'?" - or nothing. Prose only.
static void v_suggest(ShclArena *a, ShclArena *tmp, const ShclVecS *names, ShclStr name, ShclSB *msg) {
	/* tmp holds the DP rows and codepoint decodes - dead after this call.
	   Resetting per unknown field keeps a wholesale unmatched document (the
	   case this feature exists for) at one sweep's peak. The sibling lists
	   are prebuilt once per validate by v_unknown. */
	arena_reset(tmp);
	if (!names) return;
	int have = 0; size_t best_dist = 0; ShclStr best_name = s_empty();
	for (size_t i = 0; i < names->len; i++) {
		size_t dist = v_edit_distance(tmp, name, names->data[i], 2);
		if (dist <= 2 && (!have || dist < best_dist)) { have = 1; best_dist = dist; best_name = names->data[i]; }
	}
	if (have) {
		sb_puts(a, msg, "; did you mean '");
		sb_putS(a, msg, best_name);
		sb_puts(a, msg, "'?");
	}
}

// Resolution contexts: the whole document for a plain path; each enclosing
// instance for the part of a path after a wildcard. required/repeat evaluate
// per context (anchor line 0 = document scope), so `server[*].port` + required
// means a port under EACH server - vacuously true with no servers.
typedef struct { size_t anchor; ShclVecSize found; } ShclVCtx;
DEFINE_VEC(ShclVecVCtx, ShclVCtx)

static void v_contexts(ShclArena *a, shcl_doc *d, const size_t *start, size_t nstart, ShclSegment *segs, size_t nsegs, size_t anchor, ShclVecVCtx *out) {
	ShclVecSize cur = {0};
	// cppcheck-suppress objectIndex  ## single-element callers pass nstart == 1, so start[i] stays at 0
	for (size_t i = 0; i < nstart; i++) ShclVecSize_push(a, &cur, start[i]);
	for (size_t si = 0; si < nsegs; si++) {
		const ShclSegment *seg = &segs[si];
		ShclVecSize next = {0};
		for (size_t k = 0; k < cur.len; k++) {
			ShclVecSize ch = NODE(d, cur.data[k]).children;
			if (seg->star) {
				for (size_t j = 0; j < ch.len; j++) ShclVecSize_push(a, &next, ch.data[j]);
			} else {
				children_named(d, a, cur.data[k], seg->name, &next);
			}
		}
		if (seg->star) {
			// Name wildcard: same per-instance split as `[*]`, any child name.
			ShclSegment *rest = segs + si + 1; size_t nrest = nsegs - si - 1;
			if (nrest == 0) {
				ShclVCtx ctx; ctx.anchor = anchor; ctx.found = next; ShclVecVCtx_push(a, out, ctx);
			} else {
				for (size_t k = 0; k < next.len; k++) {
					size_t inst = next.data[k];
					v_contexts(a, d, &inst, 1, rest, nrest, NODE(d, inst).line, out);
				}
			}
			return;
		}
		switch (seg->sel.tag) {
		case SEL_NONE: cur = next; break;
		case SEL_VALUE: {
			ShclVecSize f = {0};
			ShclStr want = apply_escapes(a, seg->sel.value);
			for (size_t k = 0; k < next.len; k++) if (s_eq(disp_key(a, &NODE(d, next.data[k]).value), want) && (!seg->sel.quoted || single_scalar(&NODE(d, next.data[k]).value))) ShclVecSize_push(a, &f, next.data[k]);
			cur = f; break;
		}
		case SEL_INDEX: {
			ShclVecSize f = {0};
			if (seg->sel.index < next.len) ShclVecSize_push(a, &f, next.data[seg->sel.index]);
			cur = f; break;
		}
		case SEL_WILDCARD: {
			ShclSegment *rest = segs + si + 1; size_t nrest = nsegs - si - 1;
			if (nrest == 0) {
				ShclVCtx ctx; ctx.anchor = anchor; ctx.found = next; ShclVecVCtx_push(a, out, ctx);
			} else {
				for (size_t k = 0; k < next.len; k++) {
					size_t inst = next.data[k];
					v_contexts(a, d, &inst, 1, rest, nrest, NODE(d, inst).line, out);
				}
			}
			return;
		}
		}
	}
	ShclVCtx ctx; ctx.anchor = anchor; ctx.found = cur; ShclVecVCtx_push(a, out, ctx);
}

static void v_wrong_type(ShclArena *a, ShclVecDiag *out, size_t line, const ShclVCons *c) {
	ShclSB s = {0, 0, 0};
	sb_puts(a, &s, "wrong type at '"); sb_putS(a, &s, c->path);
	sb_puts(a, &s, "': value is not a valid "); sb_puts(a, &s, c->ty ? c->ty : "string");
	v_diag(a, out, line, "V003", sb_S(&s));
}
/* Value text for a diagnostic message: line breaks and tabs escaped, so one
   diagnostic is one line. A raw block's body is the value that made this
   necessary - it carries its own newlines. */
static ShclStr v_one_line(ShclArena *a, ShclStr t) {
	ShclSB o = {0, 0, 0};
	sb_reserve(a, &o, t.n);
	for (size_t i = 0; i < t.n; i++) {
		switch (t.p[i]) {
		case '\\': sb_puts(a, &o, "\\\\"); break;
		case '\n': sb_puts(a, &o, "\\n"); break;
		case '\r': sb_puts(a, &o, "\\r"); break;
		case '\t': sb_puts(a, &o, "\\t"); break;
		default: sb_putc(a, &o, t.p[i]); break;
		}
	}
	return sb_S(&o);
}
static void v_not_allowed(ShclArena *a, ShclVecDiag *out, size_t line, const ShclVCons *c, ShclStr text) {
	ShclSB s = {0, 0, 0};
	sb_puts(a, &s, "value not allowed at '"); sb_putS(a, &s, c->path);
	sb_puts(a, &s, "': "); sb_putS(a, &s, v_one_line(a, text));
	v_diag(a, out, line, "V004", sb_S(&s));
}

// Diagnostic messages go to a (they outlive the walk); coercion temporaries
// and compare strings go to lv, the walk level's scratch.
static void v_node(ShclArena *a, ShclArena *lv, shcl_doc *d, const ShclVCons *c, size_t n, ShclVecDiag *out) {
	ShclNode *node = &NODE(d, n);
	size_t line = node->line;
	const char *ty = c->ty;
	size_t tlen = ty ? strlen(ty) : 0;
	int is_array = ty && tlen > 6 && memcmp(ty + tlen - 6, "-array", 6) == 0;
	size_t blen = is_array ? tlen - 6 : tlen;
	// base kind compare helper against a literal
	#define V_BASE_IS(z) (ty && blen == strlen(z) && memcmp(ty, z, blen) == 0)
	if (node->value.kind == V_EMPTY) {
		// Empty passes everything; required already counted it as present.
		return;
	}
	if (node->value.kind == V_RAW) {
		// A raw block satisfies `raw` and scalar `string` (any value reads as a
		// string); every other kind is a type miss.
		if (ty && ((!V_BASE_IS("raw") && !V_BASE_IS("string")) || is_array)) { v_wrong_type(a, out, line, c); return; }
		if (c->has_allowed && c->akind == ALLOW_STRINGS) {
			int found = 0;
			for (size_t x = 0; x < c->a_n; x++) if (s_eq(c->a_strs[x], node->value.raw->content)) { found = 1; break; }
			if (!found) v_not_allowed(a, out, line, c, node->value.raw->content);
		}
		return;
	}
	ShclElement *els = node->value.els; size_t nels = node->value.nels;
	if (V_BASE_IS("raw")) { v_wrong_type(a, out, line, c); return; }
	// A scalar kind on a multi-element value is the array-where-one-scalar-
	// expected miss - except string, which reads arrays.
	if (ty && !is_array && !V_BASE_IS("string") && nels > 1) { v_wrong_type(a, out, line, c); return; }
	if (V_BASE_IS("int")) {
		int64_t *vals = (int64_t *)arena_alloc(lv, (nels ? nels : 1) * sizeof(int64_t));
		for (size_t x = 0; x < nels; x++)
			if (!parse_int_text(lv, &els[x], d->strictness, &vals[x])) { v_wrong_type(a, out, line, c); return; }
		if (c->has_allowed && c->akind == ALLOW_INTS) {
			for (size_t x = 0; x < nels; x++) {
				int found = 0;
				for (size_t y = 0; y < c->a_n; y++) if (c->a_ints[y] == vals[x]) { found = 1; break; }
				if (!found) { v_not_allowed(a, out, line, c, els[x].text); break; }
			}
		}
		if (c->has_min_i) { for (size_t x = 0; x < nels; x++) if (vals[x] < c->min_i) { v_diag(a, out, line, "V005", v_msg3(a, "value below min at '", c->path, "'")); break; } }
		if (c->has_max_i) { for (size_t x = 0; x < nels; x++) if (vals[x] > c->max_i) { v_diag(a, out, line, "V006", v_msg3(a, "value above max at '", c->path, "'")); break; } }
	} else if (V_BASE_IS("float")) {
		double *vals = (double *)arena_alloc(lv, (nels ? nels : 1) * sizeof(double));
		for (size_t x = 0; x < nels; x++)
			if (!parse_float_text(lv, &els[x], d->strictness, &vals[x])) { v_wrong_type(a, out, line, c); return; }
		if (c->has_allowed && c->akind == ALLOW_FLOATS) {
			for (size_t x = 0; x < nels; x++) {
				int found = 0;
				for (size_t y = 0; y < c->a_n; y++) if (c->a_floats[y] == vals[x]) { found = 1; break; }
				if (!found) { v_not_allowed(a, out, line, c, els[x].text); break; }
			}
		}
		if (c->has_min_f) { for (size_t x = 0; x < nels; x++) if (vals[x] < c->min_f) { v_diag(a, out, line, "V005", v_msg3(a, "value below min at '", c->path, "'")); break; } }
		if (c->has_max_f) { for (size_t x = 0; x < nels; x++) if (vals[x] > c->max_f) { v_diag(a, out, line, "V006", v_msg3(a, "value above max at '", c->path, "'")); break; } }
	} else if (V_BASE_IS("bool")) {
		int *vals = (int *)arena_alloc(lv, (nels ? nels : 1) * sizeof(int));
		for (size_t x = 0; x < nels; x++)
			if (!parse_bool_text(lv, els[x].text, d->strictness, &vals[x])) { v_wrong_type(a, out, line, c); return; }
		if (c->has_allowed && c->akind == ALLOW_BOOLS) {
			for (size_t x = 0; x < nels; x++) {
				int found = 0;
				for (size_t y = 0; y < c->a_n; y++) if ((c->a_bools[y] != 0) == (vals[x] != 0)) { found = 1; break; }
				if (!found) { v_not_allowed(a, out, line, c, els[x].text); break; }
			}
		}
	} else if (V_BASE_IS("datetime")) {
		shcl_datetime *vals = (shcl_datetime *)arena_alloc(lv, (nels ? nels : 1) * sizeof(shcl_datetime));
		for (size_t x = 0; x < nels; x++)
			if (!parse_datetime(lv, els[x].text, &vals[x])) { v_wrong_type(a, out, line, c); return; }
		if (c->has_allowed && c->akind == ALLOW_DATES) {
			for (size_t x = 0; x < nels; x++) {
				int found = 0;
				for (size_t y = 0; y < c->a_n; y++) if (v_same_moment(&c->a_dates[y], &vals[x])) { found = 1; break; }
				if (!found) { v_not_allowed(a, out, line, c, els[x].text); break; }
			}
		}
	} else {
		// string kind or untyped: every element coerces; only the allowed set
		// can fail, in logical-string space.
		if (c->has_allowed && c->akind == ALLOW_STRINGS) {
			for (size_t x = 0; x < nels; x++) {
				ShclStr s = apply_escapes(lv, els[x].text);
				int found = 0;
				for (size_t y = 0; y < c->a_n; y++) if (s_eq(c->a_strs[y], s)) { found = 1; break; }
				if (!found) { v_not_allowed(a, out, line, c, s); break; }
			}
		}
	}
	#undef V_BASE_IS
}

/* (fragment, node) pairs already mounted: the map holds hashes only, so the
   parallel frag/node lists hold what each entry actually names - that is what
   a hit verifies against. */
typedef struct { ShclCMap map; ShclVecS frag; ShclVecSize node; } ShclVMounts;

// A mounted fragment's fields run per resolved node, right after that node's
// own checks, in fragment order - depth-first, so diagnostic order stays
// derivable. Termination is structural: every mount descends at least one
// document level, and the document is finite (depth capped at 512, so this C
// stack recursion is safe - same rationale as v_contexts' own).
// lv is this level's scratch arena (next slot in the caller's per-level pool);
// resetting it at entry reuses the previous sibling call's block instead of
// retaining every level's temporaries in the validation arena until it is
// freed. Level L's contexts stay live in lv while deeper levels run in lv+1.
static void v_check_from(ShclArena *a, ShclArena *lv, shcl_doc *d, const ShclVCons *c, const ShclVSchemaDef *def, size_t start, size_t anchor0, ShclVecDiag *out, ShclVMounts *mounted) {
	arena_reset(lv);
	ShclVecVCtx ctxs = {0};
	v_contexts(lv, d, &start, 1, c->segs.data, c->segs.len, anchor0, &ctxs);
	for (size_t i = 0; i < ctxs.len; i++) {
		ShclVCtx *ctx = &ctxs.data[i];
		if (c->required && ctx->found.len == 0)
			v_diag(a, out, ctx->anchor, "V002", v_msg3(a, "required path missing: ", c->path, ""));
		if (c->has_repeat) {
			uint64_t n = (uint64_t)ctx->found.len;
			if (n < c->rep_lo || n > c->rep_hi) {
				ShclSB s = {0, 0, 0};
				sb_puts(a, &s, "instance count out of bounds at '"); sb_putS(a, &s, c->path);
				sb_puts(a, &s, "': "); sb_put_u64(a, &s, n);
				sb_puts(a, &s, " not in "); sb_put_u64(a, &s, c->rep_lo);
				sb_puts(a, &s, ".."); sb_put_u64(a, &s, c->rep_hi);
				v_diag(a, out, ctx->anchor, "V007", sb_S(&s));
			}
		}
		for (size_t k = 0; k < ctx->found.len; k++) {
			size_t n = ctx->found.data[k];
			v_node(a, lv, d, c, n, out);
			if (c->inherits.n) {
				const ShclVecVCons *fcs = v_frag_get(def, c->inherits);
				if (fcs) {
					// Two constraints can resolve to the same node and mount the
					// same fragment there. The second mount would repeat the
					// first's work and its diagnostics, and repeating it per
					// level is what makes a recursive schema cost double per
					// document level, so each pair is done once.
					char kb[sizeof n]; memcpy(kb, &n, sizeof n);
					ShclStr nkey; nkey.p = kb; nkey.n = sizeof n;
					uint64_t h = cmap_hash(c->inherits, nkey);
					int seen = 0;
					for (ShclCMapEnt *e = cmap_first(&mounted->map, h); e; e = cmap_next(e, h))
						if (mounted->node.data[e->val] == n && s_eq(mounted->frag.data[e->val], c->inherits)) { seen = 1; break; }
					if (!seen) {
						size_t mi = mounted->frag.len;
						ShclVecS_push(a, &mounted->frag, c->inherits);
						ShclVecSize_push(a, &mounted->node, n);
						cmap_put(a, &mounted->map, h, mi);
						for (size_t fi = 0; fi < fcs->len; fi++)
							v_check_from(a, lv + 1, d, &fcs->data[fi], def, n, NODE(d, n).line, out, mounted);
					}
				}
			}
		}
	}
}

// Append a segment to a chain key. Chain keys join segments length-prefixed
// (`<len>:<name>`), not with a bare NUL: NUL is legal in a quoted name, so a
// single field named "x\0y" would impersonate the two-segment path x.y. Same
// injectivity reasoning as the merge key's cell encoding - and like it, the
// length unit is each binding's native one (bytes here), because only
// injectivity matters.
static void chain_push(ShclArena *a, ShclSB *chain, ShclStr name) {
	char buf[32];
	snprintf(buf, sizeof buf, "%zu:", name.n);
	sb_puts(a, chain, buf);
	sb_putS(a, chain, name);
}

// Decode the next length-prefixed segment of a chain key at *i. Total: bails
// at the first shape the encoder can't have produced.
static int chain_next(ShclStr chain, size_t *i, ShclStr *nm) {
	size_t k = *i, n = 0;
	if (k >= chain.n) return 0;
	while (k < chain.n && chain.p[k] >= '0' && chain.p[k] <= '9') { n = n * 10 + (size_t)(chain.p[k] - '0'); k++; }
	if (k >= chain.n || chain.p[k] != ':' || k + 1 + n > chain.n) return 0;
	k++;
	*nm = s_slice(chain, k, k + n);
	*i = k + n;
	return 1;
}

// Element-wise chain match against the star-bearing schema paths: a `*`
// segment matches any one name, and every prefix of a path is legal.
static int star_legal(const ShclVecSeg *pats, size_t npats, ShclStr chain) {
	if (npats == 0) return 0;
	for (size_t pi = 0; pi < npats; pi++) {
		const ShclVecSeg *p = &pats[pi];
		size_t part = 0, i = 0; int match = 1;
		ShclStr nm;
		while (match && chain_next(chain, &i, &nm)) {
			if (part >= p->len || (!p->data[part].star && !s_eq(p->data[part].name, nm))) match = 0;
			part++;
		}
		if (match) return 1;
	}
	return 0;
}

// Chain legality through fragment mounts: the general matcher - element-wise
// like star_legal (stars wild, prefixes legal), and when a mount's whole path
// matched with chain left over, the remainder is retried against the mounted
// fragment's fields. Terminates: every descent consumes >= 1 part.
static int chain_parts_legal(const ShclVecVCons *cons, const ShclVSchemaDef *def, ShclStr chain, size_t from) {
	for (size_t ci = 0; ci < cons->len; ci++) {
		const ShclVCons *c = &cons->data[ci];
		size_t n = c->segs.len;
		size_t part = 0, i = from, rem = from;
		int match = 1;
		ShclStr nm;
		while (match && chain_next(chain, &i, &nm)) {
			if (part < n) {
				if (!c->segs.data[part].star && !s_eq(c->segs.data[part].name, nm)) match = 0;
				if (part + 1 == n) rem = i; // remainder starts past the matched prefix
			}
			part++;
		}
		if (!match) continue;
		if (part <= n) return 1; // a prefix of a legal path
		if (c->inherits.n) {
			const ShclVecVCons *fcs = v_frag_get(def, c->inherits);
			if (fcs && chain_parts_legal(fcs, def, chain, rem)) return 1;
		}
	}
	return 0;
}
static int chain_legal(const ShclVSchemaDef *def, ShclStr chain) {
	return chain_parts_legal(&def->cons, def, chain, 0);
}

// Unknown-field sweep: a schema path legalizes its name chain and every prefix
// (selectors ignored). Only the topmost unknown node is reported; its subtree
// is implied unknown and skipped.
static void v_unknown(ShclArena *a, ShclArena *tmp, shcl_doc *d, const ShclVSchemaDef *def, ShclVecDiag *out) {
	const ShclVecVCons *cons = &def->cons;
	// Chains below a fragment mount only match by descending the mounts.
	int has_mounts = 0;
	for (size_t i = 0; i < cons->len; i++) if (cons->data[i].inherits.n) { has_mounts = 1; break; }
	arena_guard(tmp, a->panic); // v_suggest scratch, reset per unknown field
	// Legal chains in a hash set (the linear scan compounded the quadratic),
	// and sibling names bucketed per parent chain, built once: v_suggest used
	// to rebuild every chain per unknown field. The map entries hold hashes
	// only; legal_chains / sib_chain hold what each names, for the verify.
	ShclCMap legal; memset(&legal, 0, sizeof legal);
	ShclVecS legal_chains = {0};
	ShclCMap sib_of; memset(&sib_of, 0, sizeof sib_of);
	ShclVecS sib_chain = {0}; /* parent chain per sibs bucket */
	ShclVecS *sibs = NULL; size_t nsib = 0, csib = 0;
	// Paths with a `*` segment can't live in the exact-chain hash; they
	// match element-wise (a star matches any one name, prefixes included).
	ShclVecSeg *star_pats = NULL; size_t nstar = 0, cstar = 0;
	for (size_t i = 0; i < cons->len; i++) {
		int has_star = 0;
		for (size_t si = 0; si < cons->data[i].segs.len; si++) if (cons->data[i].segs.data[si].star) { has_star = 1; break; }
		if (has_star) {
			if (nstar == cstar) { size_t nc = cstar ? cstar * 2 : 8; star_pats = (ShclVecSeg *)arena_grow(a, star_pats, cstar, nc, sizeof(ShclVecSeg)); cstar = nc; }
			star_pats[nstar++] = cons->data[i].segs;
		}
		ShclSB chain = {0, 0, 0};
		for (size_t si = 0; si < cons->data[i].segs.len; si++) {
			if (cons->data[i].segs.data[si].star) break; // no sibling entry for '*'; deeper chains are pattern-only
			ShclStr nm = cons->data[i].segs.data[si].name;
			ShclStr pc = s_dup(a, sb_S(&chain));
			uint64_t hp = cmap_hash(pc, s_empty());
			size_t g = (size_t)-1;
			for (ShclCMapEnt *e = cmap_first(&sib_of, hp); e; e = cmap_next(e, hp))
				if (s_eq(sib_chain.data[e->val], pc)) { g = e->val; break; }
			if (g == (size_t)-1) {
				if (nsib == csib) { size_t nc = csib ? csib * 2 : 8; sibs = (ShclVecS *)arena_grow(a, sibs, csib, nc, sizeof(ShclVecS)); csib = nc; }
				memset(&sibs[nsib], 0, sizeof sibs[nsib]);
				g = nsib++;
				cmap_put(a, &sib_of, hp, g);
				ShclVecS_push(a, &sib_chain, pc);
			}
			ShclVecS_push(a, &sibs[g], nm);
			chain_push(a, &chain, nm);
			ShclStr full = s_dup(a, sb_S(&chain));
			uint64_t hf = cmap_hash(full, s_empty());
			int have = 0;
			for (ShclCMapEnt *e = cmap_first(&legal, hf); e; e = cmap_next(e, hf))
				if (s_eq(legal_chains.data[e->val], full)) { have = 1; break; }
			if (!have) { cmap_put(a, &legal, hf, legal_chains.len); ShclVecS_push(a, &legal_chains, full); }
		}
	}
	ShclVecSize snode = {0}; ShclVecS schain = {0}; ShclVecS sshown = {0};
	ShclVecSize top = NODE(d, ROOT).children;
	for (size_t i = top.len; i > 0; i--) {
		ShclVecSize_push(a, &snode, top.data[i - 1]);
		ShclVecS_push(a, &schain, s_empty());
		ShclVecS_push(a, &sshown, s_empty());
	}
	while (snode.len) {
		size_t n = snode.data[snode.len - 1];
		ShclStr pchain = schain.data[snode.len - 1];
		ShclStr pshown = sshown.data[snode.len - 1];
		snode.len--; schain.len--; sshown.len--;
		const ShclNode *node = &NODE(d, n);
		ShclSB cb = {0, 0, 0};
		sb_putS(a, &cb, pchain);
		chain_push(a, &cb, node->name);
		ShclStr chain = sb_S(&cb);
		ShclSB sb2 = {0, 0, 0};
		if (pshown.n) { sb_putS(a, &sb2, pshown); sb_putc(a, &sb2, '.'); }
		sb_putS(a, &sb2, node->name);
		ShclStr shown = sb_S(&sb2);
		int found = 0;
		{
			uint64_t hc = cmap_hash(chain, s_empty());
			for (ShclCMapEnt *e = cmap_first(&legal, hc); e; e = cmap_next(e, hc))
				if (s_eq(legal_chains.data[e->val], chain)) { found = 1; break; }
		}
		if (!found && !star_legal(star_pats, nstar, chain) && !(has_mounts && chain_legal(def, chain))) {
			ShclSB msg = {0, 0, 0};
			sb_puts(a, &msg, "unknown field '"); sb_putS(a, &msg, shown); sb_puts(a, &msg, "'");
			size_t sg = (size_t)-1;
			uint64_t hpc = cmap_hash(pchain, s_empty());
			for (ShclCMapEnt *e = cmap_first(&sib_of, hpc); e; e = cmap_next(e, hpc))
				if (s_eq(sib_chain.data[e->val], pchain)) { sg = e->val; break; }
			v_suggest(a, tmp, sg == (size_t)-1 ? NULL : &sibs[sg], node->name, &msg);
			v_diag(a, out, node->line, "V001", sb_S(&msg));
			continue;
		}
		ShclVecSize ch = node->children;
		for (size_t i = ch.len; i > 0; i--) {
			ShclVecSize_push(a, &snode, ch.data[i - 1]);
			ShclVecS_push(a, &schain, chain);
			ShclVecS_push(a, &sshown, shown);
		}
	}
	arena_free(tmp);
}

shcl_validation *shcl_validate(shcl_doc *d, shcl_doc *schema) {
	/* Same shape as do_parse: the two the unwind path has to reach are
	   volatile, the working copies below are not. */
	shcl_validation *volatile val = (shcl_validation *)malloc(sizeof *val);
	if (!val) return NULL;
	memset(val, 0, sizeof *val);
	ShclArena *volatile levels = NULL;
	jmp_buf panic;
	if (SHCL_SETJMP(panic)) {
		shcl_validation *bad = val; ShclArena *badLevels = levels;
		if (badLevels) { for (size_t i = 0; i <= SHCL_MAX_DEPTH; i++) arena_free(&badLevels[i]); free(badLevels); }
		/* The name index is the only thing on the document this call builds,
		   and half of one is worse than none. Everything else it touched is
		   scratch. */
		index_drop(d);
		arena_guard(&d->index_arena, NULL); arena_guard(&d->scratch, NULL); arena_guard(&d->reads, NULL);
		arena_free(&bad->arena); arena_free(&bad->scratch); free(bad);
		return NULL;
	}
	shcl_validation *v = val;
	arena_guard(&v->arena, &panic);
	/* Reading the document allocates too - the name index above all - so the
	   read-side arenas unwind here rather than to SHCL_OOM. */
	arena_guard(&d->index_arena, &panic); arena_guard(&d->scratch, &panic); arena_guard(&d->reads, &panic);
	ShclArena *a = &v->arena;
	ShclVSchemaDef def; memset(&def, 0, sizeof def);
	ShclVecDiag faults = {0};
	v_build_schema(a, schema, &def, &faults);
	v->diags = faults;
	// One scratch arena per mount-recursion level, reset and reused across
	// sibling calls: peak retention is one block per active document level,
	// not every level of every walk. The parser caps depth at SHCL_MAX_DEPTH
	// and every mount starts at least one level deeper, so the pool cannot be
	// outrun; untouched slots never allocate.
	// On the heap, not the stack: one slot per level of the depth cap is 16 KB,
	// which is fine on a main thread and not on a small-stack one.
	ShclArena *lvls = (ShclArena *)calloc(SHCL_MAX_DEPTH + 1, sizeof *lvls);
	if (!lvls) arena_panic(&panic);
	levels = lvls;
	for (size_t i = 0; i <= SHCL_MAX_DEPTH; i++) arena_guard(&lvls[i], &panic);
	// One mount set for the whole schema: two top-level paths can resolve to
	// the same node and mount the same fragment there, and the spec says each
	// fragment runs once per node. Entries live in the validation arena, so
	// the set needs no own teardown.
	ShclVMounts mounted; memset(&mounted, 0, sizeof mounted);
	for (size_t i = 0; i < def.cons.len; i++) v_check_from(a, lvls, d, &def.cons.data[i], &def, ROOT, 0, &v->diags, &mounted);
	// Nothing returns between the alloc and here, so every slot is reached.
	for (size_t i = 0; i <= SHCL_MAX_DEPTH; i++) arena_free(&lvls[i]);
	free(lvls);
	levels = NULL;
	if (def.paths_complete) v_unknown(a, &v->scratch, d, &def, &v->diags);
	/* This frame is about to go; the arenas outlive it. */
	arena_guard(&v->arena, NULL);
	arena_guard(&d->index_arena, NULL); arena_guard(&d->scratch, NULL); arena_guard(&d->reads, NULL);
	return v;
}
size_t shcl_validation_count(const shcl_validation *v) { return v->diags.len; }
size_t shcl_validation_line(const shcl_validation *v, size_t i) { return v->diags.data[i].line; }
shcl_severity shcl_validation_severity(const shcl_validation *v, size_t i) { return v->diags.data[i].sev; }
shcl_str shcl_validation_message(const shcl_validation *v, size_t i) {
	shcl_str s; s.p = v->diags.data[i].message.p; s.n = v->diags.data[i].message.n; return s;
}
const char *shcl_validation_code(const shcl_validation *v, size_t i) { return v->diags.data[i].code; }
void shcl_validation_free(shcl_validation *v) { if (!v) return; arena_free(&v->arena); arena_free(&v->scratch); free(v); }

/* Leaf names of the schema entries pick accepts, top-level fields and every
   fragment's fields alike. Read through the built schema, so the names are
   the ones validation will use (escapes resolved) and an entry whose key
   faulted disavows nothing. Everything is built in tmp. */
static int v_pick_repeat(const ShclVCons *c) { return c->has_repeat && c->rep_hi > 1; }
static int v_pick_reopen(const ShclVCons *c) { return c->reopen; }
static ShclVecS v_disavowed_names(shcl_doc *schema, ShclArena *tmp, int (*pick)(const ShclVCons *)) {
	ShclVSchemaDef def; memset(&def, 0, sizeof def);
	ShclVecDiag faults = {0};
	v_build_schema(tmp, schema, &def, &faults);
	ShclVecS names = {0};
	for (size_t g = 0; g <= def.frags.len; g++) {
		const ShclVecVCons *list = g == 0 ? &def.cons : &def.frags.data[g - 1].fields;
		for (size_t i = 0; i < list->len; i++) {
			const ShclVCons *c = &list->data[i];
			if (!pick(c) || c->segs.len == 0) continue;
			/* Name wildcard: no single leaf name to disavow. */
			const ShclSegment *last = &c->segs.data[c->segs.len - 1];
			if (!last->star) ShclVecS_push(tmp, &names, last->name);
		}
	}
	return names;
}

void shcl_suppress_declared_repeats(shcl_doc *schema, shcl_doc *doc) {
	/* Everything this probe builds - instance/repeat query results as well as
	   the collected names (which must survive the per-read scratch resets) -
	   goes into its own arena, freed on exit: the function owns neither doc,
	   so it must not leave allocations behind in either. */
	ShclArena tmp; memset(&tmp, 0, sizeof tmp);
	ShclVecS names = v_disavowed_names(schema, &tmp, v_pick_repeat);
	if (!names.len) { arena_free(&tmp); return; }
	ShclVecS heads = {0};
	for (size_t k = 0; k < names.len; k++) ShclVecS_push(&tmp, &heads, h001_head(&tmp, names.data[k]));
	size_t w = 0;
	for (size_t i = 0; i < doc->diags.len; i++) {
		ShclDiag dg = doc->diags.data[i];
		int drop = 0;
		if (strcmp(dg.code, "H001") == 0) {
			ShclStr m = dg.message;
			for (size_t k = 0; k < heads.len; k++) {
				ShclStr h = heads.data[k];
				if (m.n >= h.n && memcmp(m.p, h.p, h.n) == 0) { drop = 1; break; }
			}
		}
		if (!drop) doc->diags.data[w++] = dg;
	}
	doc->diags.len = w;
	arena_free(&tmp);
}

void shcl_suppress_declared_reopens(shcl_doc *schema, shcl_doc *doc) {
	/* Same arena discipline as the H001 suppressor above. */
	ShclArena tmp; memset(&tmp, 0, sizeof tmp);
	ShclVecS names = v_disavowed_names(schema, &tmp, v_pick_reopen);
	if (!names.len) { arena_free(&tmp); return; }
	ShclVecS heads = {0};
	for (size_t k = 0; k < names.len; k++) ShclVecS_push(&tmp, &heads, h002_head(&tmp, names.data[k]));
	size_t w = 0;
	for (size_t i = 0; i < doc->diags.len; i++) {
		ShclDiag dg = doc->diags.data[i];
		int drop = 0;
		if (strcmp(dg.code, "H002") == 0) {
			ShclStr m = dg.message;
			for (size_t k = 0; k < heads.len; k++) {
				ShclStr h = heads.data[k];
				if (m.n >= h.n && memcmp(m.p, h.p, h.n) == 0) { drop = 1; break; }
			}
		}
		if (!drop) doc->diags.data[w++] = dg;
	}
	doc->diags.len = w;
	arena_free(&tmp);
}

// How many lines or values parsing dropped that canonical output cannot
// re-emit - bad indentation, an unusable selector, a line past the depth cap.
// Content-malformed lines do NOT count: those are retained as trivia and
// survive a save. Nonzero means a save would delete hand-written content, so
// shcl_save_file refuses then (shcl_save_file_lossy overrides).
size_t shcl_lost_count(const shcl_doc *d) { return d->lost; }

size_t shcl_error_count(const shcl_doc *d) {
	size_t n = 0;
	for (size_t i = 0; i < d->diags.len; i++) if (d->diags.data[i].sev == SHCL_SEV_ERROR) n++;
	return n;
}

shcl_doc *shcl_load_and_validate(const char *text, size_t len, const char *schema, size_t slen, shcl_strictness s) {
	shcl_doc *d = shcl_parse_with(text, len, s);
	if (!d) return NULL;
	ShclStr st; st.p = schema ? schema : ""; st.n = schema ? slen : 0;
	if (s_trim(st).n != 0) {
		shcl_doc *sd = shcl_parse(schema, slen);
		if (!sd) { shcl_free(d); return NULL; }
		// A schema that did not load would silently drop the constraints on
		// its broken lines, or report every field as unknown - either way
		// blaming the document for the schema. Say so instead, as `check`
		// does, and validate nothing.
		int sbad = 0;
		for (size_t i = 0; i < sd->diags.len; i++) if (sd->diags.data[i].sev == SHCL_SEV_ERROR) { sbad = 1; break; }
		if (sbad) {
			push_diag(d, 0, SHCL_SEV_ERROR, "V099", s_lit("schema failed to load"));
			shcl_free(sd);
			return d;
		}
		shcl_validation *v = shcl_validate(d, sd);
		if (!v) { shcl_free(sd); shcl_free(d); return NULL; }
		for (size_t i = 0; i < v->diags.len; i++) {
			ShclDiag dg = v->diags.data[i];
			/* the validation arena dies below; codes are static strings */
			dg.message = s_dup(&d->arena, dg.message);
			ShclVecDiag_push(&d->arena, &d->diags, dg);
		}
		shcl_suppress_declared_repeats(sd, d);
		shcl_suppress_declared_reopens(sd, d);
		shcl_validation_free(v);
		shcl_free(sd);
	}
	return d;
}

// --- File tier (optional; compile out with -DSHCL_NO_FILE_IO) ----------------

#ifndef SHCL_NO_FILE_IO
#include <errno.h>
#include <fcntl.h>
#ifdef _WIN32
	#include <windows.h>
	#include <io.h>
	#include <process.h>
	#include <sys/stat.h>
	#include <wchar.h>
#else
	#include <unistd.h>
	#include <sys/stat.h>
	// realpath is XSI; declared here so the single-header build works under a
	// plain -std=c11 -D_POSIX_C_SOURCE consumer too (the symbol is always in
	// libc even when the prototype is feature-gated away).
	extern char *realpath(const char *, char *);
#endif

#ifdef _WIN32
// The narrow file calls read a path in the active code page, so a UTF-8 path
// with anything outside it cannot be opened - or worse, opens a mojibake name
// that round-trips through the same mistake. Convert once and use the wide
// forms. Bad UTF-8 fails (EINVAL) rather than folding to U+FFFD, which would
// quietly name a different file.
static wchar_t *shcl_widen(const char *s) {
	int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, NULL, 0);
	wchar_t *w = n > 0 ? (wchar_t *)malloc((size_t)n * sizeof(wchar_t)) : NULL;
	if (w) MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, w, n);
	else errno = n > 0 ? ENOMEM : EINVAL;
	return w;
}

// The Win32 calls report through GetLastError and leave errno alone, and
// errno is what the header promises a failed write describes. The common
// causes map; the rest is EIO, which at least is not "Success".
static int shcl_errno_from_win32(DWORD e) {
	switch (e) {
	case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: case ERROR_INVALID_DRIVE: return ENOENT;
	case ERROR_ACCESS_DENIED: case ERROR_SHARING_VIOLATION: case ERROR_LOCK_VIOLATION: case ERROR_USER_MAPPED_FILE: return EACCES;
	case ERROR_ALREADY_EXISTS: case ERROR_FILE_EXISTS: return EEXIST;
	case ERROR_NOT_ENOUGH_MEMORY: case ERROR_OUTOFMEMORY: return ENOMEM;
	case ERROR_INVALID_NAME: case ERROR_BAD_PATHNAME: case ERROR_INVALID_PARAMETER: case ERROR_FILENAME_EXCED_RANGE: return EINVAL;
	case ERROR_DISK_FULL: case ERROR_HANDLE_DISK_FULL: return ENOSPC;
	case ERROR_BUSY: return EBUSY;
	case ERROR_DIRECTORY: return ENOTDIR;
	default: return EIO;
	}
}

// ReplaceFile carries the destination's ACLs, security attributes and named
// streams onto the replacement; a move publishes a brand-new file and leaves
// all of it behind. What it does NOT carry is the basic attributes - hidden and
// system - which the save re-applies by hand. It needs the destination to
// exist, and it fails rather than skip a merge it cannot do (no WRITE_DAC,
// say), so a create and any failure fall back to MoveFileEx - which is there
// regardless because C rename() will not replace an existing file on Windows at
// all. WRITE_THROUGH is asked for and documented as unsupported by ReplaceFile;
// the move's own WRITE_THROUGH is the one that means something.
static int shcl_publish_file(const wchar_t *tmp, const wchar_t *target) {
	int ok = (GetFileAttributesW(target) != INVALID_FILE_ATTRIBUTES
			&& ReplaceFileW(target, tmp, NULL, REPLACEFILE_WRITE_THROUGH, NULL, NULL))
		|| MoveFileExW(tmp, target, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
	if (!ok) errno = shcl_errno_from_win32(GetLastError());
	return ok;
}
#endif

#ifdef _WIN32
static char *shcl_resolve_target(const char *file);
#endif
static FILE *shcl_fopen_rb(const char *path) {
#ifdef _WIN32
	// Through the same resolver the write side uses, so a read past MAX_PATH
	// works too: the narrow and wide file calls both refuse such a path unless
	// it carries the long-path prefix. A path the resolver cannot spell is
	// opened as given, which is what it did before.
	char *real = shcl_resolve_target(path);
	wchar_t *w = shcl_widen(real ? real : path);
	free(real);
	FILE *f = w ? _wfopen(w, L"rb") : NULL;
	int e = errno; free(w); errno = e;
	return f;
#else
	return fopen(path, "rb");
#endif
}

// Where the directory part of TARGET ends: the last separator, or NULL for a
// bare name. Windows takes either slash, and a path built with the platform
// separator is all backslashes; a drive-relative `C:x` has no separator at all
// and splits after the colon, where the reference's Path::parent splits it.
static const char *shcl_last_sep(const char *target) {
	const char *sep = strrchr(target, '/');
#ifdef _WIN32
	const char *bs = strrchr(target, '\\');
	if (bs && (!sep || bs > sep)) sep = bs;
	if (!sep && target[0] && target[1] == ':') sep = target + 1;
#endif
	return sep;
}

#ifdef _WIN32
// The path a save actually rewrites. A symlink or junction is followed, so a
// save through a linked-in config replaces the file it points at rather than
// the link - the same thing the POSIX side has always done. The answer comes
// back \\?\-prefixed, which is also what carries a path past MAX_PATH, so the
// prefix is kept only where the name would otherwise be too long for the temp
// file beside it; a short path stays the plain name it was. A file that is not
// there yet has no final path, so its full path is prefixed by hand. malloc'd
// UTF-8; NULL with errno saying why.
static char *shcl_narrow(const wchar_t *w) {
	int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
	char *s = n > 0 ? (char *)malloc((size_t)n) : NULL;
	if (s) WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
	else errno = n > 0 ? ENOMEM : EINVAL;
	return s;
}
static char *shcl_resolve_target(const char *file) {
	wchar_t *w = shcl_widen(file);
	if (!w) return NULL;
	wchar_t *full = NULL;
	HANDLE h = CreateFileW(w, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
	                       NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
	if (h != INVALID_HANDLE_VALUE) {
		DWORD need = GetFinalPathNameByHandleW(h, NULL, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
		if (need && (full = (wchar_t *)malloc((size_t)need * sizeof *full))
		    && !GetFinalPathNameByHandleW(h, full, need, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS)) {
			free(full); full = NULL;
		}
		CloseHandle(h);
	}
	if (!full) {
		// Not there yet (or not openable): build the long-path spelling from
		// the full path instead. \\server\share becomes \\?\UNC\server\share.
		DWORD need = GetFullPathNameW(w, 0, NULL, NULL);
		if (!need) { free(w); errno = shcl_errno_from_win32(GetLastError()); return NULL; }
		wchar_t *fp = (wchar_t *)malloc((size_t)need * sizeof *fp);
		if (!fp) { free(w); errno = ENOMEM; return NULL; }
		if (!GetFullPathNameW(w, need, fp, NULL)) {
			DWORD e = GetLastError();
			free(fp); free(w); errno = shcl_errno_from_win32(e); return NULL;
		}
		int unc = fp[0] == L'\\' && fp[1] == L'\\';
		full = (wchar_t *)malloc((wcslen(fp) + 10) * sizeof *full);
		if (!full) { free(fp); free(w); errno = ENOMEM; return NULL; }
		wcscpy(full, unc ? L"\\\\?\\UNC" : L"\\\\?\\");
		wcscat(full, unc ? fp + 1 : fp);
		free(fp);
	}
	free(w);
	char *out = shcl_narrow(full);
	free(full);
	if (!out) return NULL;
	// The temp file sits beside the target with a suffix of its own, so the
	// prefix stays on anything near the limit, not only over it. Below that it
	// comes off, so an ordinary save writes the plain name it always did.
	size_t on = strlen(out);
	if (on + 48 < MAX_PATH) {
		if (!strncmp(out, "\\\\?\\UNC\\", 8)) { memmove(out + 2, out + 8, on - 8 + 1); }
		else if (!strncmp(out, "\\\\?\\", 4)) memmove(out, out + 4, on - 4 + 1);
	}
	return out;
}
#endif

#ifndef _WIN32
// The path a save actually rewrites. A symlink is followed so the write goes
// through it; realpath does that but needs the target to exist, so a dangling
// link is walked by hand and the file is created where it points. A path that
// is no link at all is a plain create at the path as given. A link cycle is an
// error (errno ELOOP): silently creating a regular file in its place would be
// the exact replacement the symlink walk exists to avoid. malloc'd; NULL with
// errno saying why.
static char *shcl_resolve_target(const char *file) {
	char *p = realpath(file, NULL);
	if (p) return p;
	size_t pn = strlen(file);
	if (!(p = (char *)malloc(pn + 1))) return NULL;
	memcpy(p, file, pn + 1);
	for (int hop = 0; hop < 40; hop++) {
		size_t cap = 256; char *link = NULL; ssize_t got;
		for (;;) {
			char *grown = (char *)realloc(link, cap);
			if (!grown) { free(link); free(p); return NULL; }
			link = grown;
			got = readlink(p, link, cap);
			if (got < 0 || (size_t)got < cap) break;
			cap *= 2;
		}
		if (got < 0) { free(link); break; }
		link[got] = '\0';
		if (link[0] == '/') { free(p); p = link; continue; }
		// Relative to the link's own directory; a bare name sits in ".".
		const char *slash = strrchr(p, '/');
		size_t dn = slash ? (size_t)(slash - p) : 0;
		char *joined = (char *)malloc(dn + (size_t)got + 3);
		if (!joined) { free(link); free(p); return NULL; }
		if (!slash) { memcpy(joined, "./", 2); memcpy(joined + 2, link, (size_t)got + 1); }
		else if (dn == 0) { joined[0] = '/'; memcpy(joined + 1, link, (size_t)got + 1); }
		else { memcpy(joined, p, dn); joined[dn] = '/'; memcpy(joined + dn + 1, link, (size_t)got + 1); }
		free(link); free(p); p = joined;
	}
	{
		char probe[1];
		if (readlink(p, probe, sizeof probe) >= 0) { free(p); errno = ELOOP; return NULL; }
	}
	const char *slash = strrchr(p, '/');
	if (!slash) return p;
	size_t dn = (size_t)(slash - p);
	char *dir = (char *)malloc(dn ? dn + 1 : 2);
	if (!dir) return p;
	if (dn) { memcpy(dir, p, dn); dir[dn] = '\0'; } else { dir[0] = '/'; dir[1] = '\0'; }
	char *rd = realpath(dir, NULL);
	free(dir);
	if (!rd) return p;
	size_t rn = strlen(rd), nn = strlen(slash + 1);
	if (rn == 1 && rd[0] == '/') rn = 0; // the root already ends in the separator
	char *out = (char *)malloc(rn + nn + 2);
	if (out) { memcpy(out, rd, rn); out[rn] = '/'; memcpy(out + rn + 1, slash + 1, nn + 1); free(p); p = out; }
	free(rd);
	return p;
}

// fsync the directory a save published into. The fsync on the file only covered
// the file; the rename is a directory change, so without this a power cut right
// after a save can lose the publish and leave the old content. Best effort - a
// filesystem that refuses an fsync on a directory is not a reason to fail a
// write that already succeeded.
static void shcl_sync_dir(const char *target) {
	const char *slash = strrchr(target, '/');
	size_t n = slash ? (size_t)(slash - target) : 0;
	char *dir = (char *)malloc(n + 2);
	if (!dir) return;
	if (!slash) { dir[0] = '.'; n = 1; }
	else if (n == 0) { dir[0] = '/'; n = 1; }
	else memcpy(dir, target, n);
	dir[n] = '\0';
	int dfd = open(dir, O_RDONLY);
	if (dfd >= 0) { (void)fsync(dfd); close(dfd); }
	free(dir);
}
#endif

// The file tier's write mechanism (also what the CLI's --write uses): a temp
// file in the same dir, then a rename over the target, so an interrupted write
// can never truncate the config it rewrites. The data is synced before the
// rename so a crash cannot publish an empty file. The target is resolved
// through symlinks first (a dangling link gets its file created where it
// points) and the original's whole mode - setuid, setgid and sticky included,
// as an editor's rewrite would carry it - is copied onto the temp file; other
// hard links to the old inode keep the old content (inherent to rename).
// Returns 1 on success, 0 on failure with errno left describing it.
/* A path that names a directory rather than a file: it ends in a separator, or
   its last component is `.` or `..`. The OS refuses to open such a path as a
   regular file, but a path cleanup drops the trailing separator first, so a
   save through `f/.` used to rewrite `f` in some bindings. */
static int shcl_names_a_directory(const char *path) {
	size_t n = strlen(path);
	if (!n) return 0;
	char lastc = path[n - 1];
	if (lastc == '/') return 1;
#ifdef _WIN32
	if (lastc == '\\') return 1;
#endif
	const char *last = shcl_last_sep(path);
	last = last ? last + 1 : path;
	return !strcmp(last, ".") || !strcmp(last, "..");
}

int shcl_write_file_atomic(const char *path, const char *data, size_t n) {
	if (shcl_names_a_directory(path)) { errno = EISDIR; return 0; }
#ifndef _WIN32
	char *real = shcl_resolve_target(path);
	if (!real) return 0;
	const char *target = real;
	#define SHCL_FILE_CLEANUP() do { free(real); } while (0)
	#define SHCL_FILE_UNLINK() remove(tmp)
#else
	char *real = shcl_resolve_target(path);
	if (!real) return 0;
	const char *target = real;
	wchar_t *wtarget = shcl_widen(target), *wtmp = NULL;
	if (!wtarget) { free(real); return 0; }
	// A read-only file cannot be replaced, and a read-only temp cannot be
	// removed after a failure, so the attribute comes off the target for the
	// publish and goes back on the new file after it - the same outcome as
	// POSIX, where the rename never needed the file writable. Hidden and system
	// ride back the same way: ReplaceFile's documented preserve list does not
	// include the basic attributes, and the fallback move carries nothing, so a
	// hidden config came back visible.
	#define SHCL_CARRIED_ATTRS ((DWORD)(FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))
	DWORD attrs = GetFileAttributesW(wtarget);
	int read_only = attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_READONLY) != 0;
	DWORD carried = attrs == INVALID_FILE_ATTRIBUTES ? 0 : (attrs & SHCL_CARRIED_ATTRS);
	#define SHCL_FILE_CLEANUP() do { free(real); free(wtarget); free(wtmp); } while (0)
	#define SHCL_FILE_UNLINK() _wremove(wtmp)
#endif
	const char *slash = shcl_last_sep(target);
	char *tmp = (char *)malloc(strlen(target) + 48);
	if (!tmp) { SHCL_FILE_CLEANUP(); return 0; }
	// Exclusive create: anything already sitting at the predictable name -
	// including a planted symlink - must fail rather than be written through.
	// A file that already exists keeps its own mode, so its temp is born private
	// and the real mode goes on below - the copy is never briefly readable to
	// anyone the original was not. A file that does not exist yet has no mode to
	// preserve, so it takes the one an ordinary create would: 0666 narrowed by
	// the umask, like every other file the user's tools produce.
#ifndef _WIN32
	struct stat st;
	int have_st = (stat(target, &st) == 0);
#endif
	int fd = -1;
	for (int attempt = 0; attempt < 8; attempt++) {
		if (slash) sprintf(tmp, "%.*s.%s.tmp%ld.%d", (int)(slash - target + 1), target, slash + 1, (long)getpid(), attempt);
		else sprintf(tmp, ".%s.tmp%ld.%d", target, (long)getpid(), attempt);
#ifdef _WIN32
		free(wtmp);
		if (!(wtmp = shcl_widen(tmp))) break;
		fd = _wopen(wtmp, _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY, _S_IREAD | _S_IWRITE);
#else
		fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL, have_st ? 0600 : 0666);
#endif
		if (fd >= 0) break;
	}
	if (fd < 0) { free(tmp); SHCL_FILE_CLEANUP(); return 0; }
	FILE *f = fdopen(fd, "wb");
	if (!f) { close(fd); SHCL_FILE_UNLINK(); free(tmp); SHCL_FILE_CLEANUP(); return 0; }
	int ok = (n == 0 || fwrite(data, 1, n, f) == n) && fflush(f) == 0;
#ifdef _WIN32
	ok = ok && _commit(_fileno(f)) == 0;
#else
	ok = ok && fsync(fileno(f)) == 0;
	// On the descriptor, so umask cannot narrow it the way it narrows a create
	// mode, and after the data, because a write by anyone but root clears
	// setuid/setgid. Best effort: a filesystem that cannot carry the mode is
	// not a failure.
	if (ok && have_st) (void)fchmod(fileno(f), st.st_mode & 07777);
#endif
	ok = (fclose(f) == 0) && ok;
#ifdef _WIN32
	if (read_only) SetFileAttributesW(wtarget, attrs & ~(DWORD)FILE_ATTRIBUTE_READONLY);
	ok = ok && shcl_publish_file(wtmp, wtarget);
	if (read_only || carried) {
		DWORD now = GetFileAttributesW(wtarget);
		if (now != INVALID_FILE_ATTRIBUTES)
			SetFileAttributesW(wtarget, now | carried | (read_only ? (DWORD)FILE_ATTRIBUTE_READONLY : 0));
	}
	#undef SHCL_CARRIED_ATTRS
#else
	ok = ok && rename(tmp, target) == 0;
	if (ok) shcl_sync_dir(target);
#endif
	// The unlink must not overwrite the errno the failure left behind.
	if (!ok) { int e = errno; SHCL_FILE_UNLINK(); errno = e; }
	free(tmp);
	SHCL_FILE_CLEANUP();
#undef SHCL_FILE_CLEANUP
#undef SHCL_FILE_UNLINK
	return ok ? 1 : 0;
}

// Whole-buffer UTF-8 validation. The parser assumes well-formed input, so the
// file tier has to reject bad bytes the way the reference's read-to-string and
// python's decoding open do.
static int shcl_utf8_valid(const char *p, size_t n) {
	size_t i = 0;
	while (i < n) {
		unsigned char c = (unsigned char)p[i];
		if (c < 0x80) { i++; continue; }
		size_t need; uint32_t cp; uint32_t lo;
		if ((c >> 5) == 0x6) { need = 1; cp = c & 0x1F; lo = 0x80; }
		else if ((c >> 4) == 0xE) { need = 2; cp = c & 0x0F; lo = 0x800; }
		else if ((c >> 3) == 0x1E) { need = 3; cp = c & 0x07; lo = 0x10000; }
		else return 0;
		if (i + need >= n) return 0;
		for (size_t k = 1; k <= need; k++) { unsigned char cc = (unsigned char)p[i + k]; if ((cc & 0xC0) != 0x80) return 0; cp = (cp << 6) | (cc & 0x3F); }
		if (cp < lo || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
		i += need + 1;
	}
	return 1;
}

// File tier, read half on its own: the text of PATH, malloc'd and
// NUL-terminated (the caller frees it), with *LEN set and *STATUS CLEAN; or
// NULL with the status saying why not - NOT_FOUND, or UNREADABLE for
// everything else (permissions, a directory, bad encoding, or a file past
// MAX_BYTES; 0 is no cap). shcl_load_file is this plus a parse. A consumer
// that needs the exact bytes it last saw - to tell its own save coming back as
// a change notification from somebody else's edit - or a bound on how much it
// will read before a parse, calls this and parses the text itself.
char *shcl_read_file(const char *path, size_t max_bytes, size_t *len, shcl_file_status *status) {
	FILE *f = shcl_fopen_rb(path);
	if (!f) {
		if (status) *status = (errno == ENOENT) ? SHCL_FILE_NOT_FOUND : SHCL_FILE_UNREADABLE;
		return NULL;
	}
	// One byte past the cap is read, so a file exactly at it passes and one
	// over is caught without trusting a length from stat.
	size_t limit = (max_bytes && max_bytes < (size_t)-1) ? max_bytes + 1 : (size_t)-1;
	size_t cap = (size_t)1 << 16, n = 0;
	if (cap > limit) cap = limit;
	char *buf = (char *)malloc(cap + 1);
	int rerr = buf == NULL;
	while (!rerr) {
		if (n == cap) {
			if (cap >= limit) break;
			size_t ncap = cap > limit / 2 ? limit : cap * 2;
			char *nb = (char *)realloc(buf, ncap + 1);
			if (!nb) { rerr = 1; break; }
			buf = nb; cap = ncap;
		}
		size_t got = fread(buf + n, 1, cap - n, f);
		n += got;
		if (got < cap - n + got) {
			if (ferror(f)) rerr = 1; // a directory reads this way on POSIX
			break;
		}
	}
	fclose(f);
	if (!rerr && max_bytes && n > max_bytes) rerr = 1;
	// The read succeeds on any bytes, unlike the reference's read-to-string and
	// python's decoding open - so bad encoding needs its own test, or a binary
	// file loads clean, reads back mangled, and a later save writes the mangled
	// version over the original. Its own copy rather than the CLI's: that one
	// also gates argv and stdin, which exist with the file tier compiled out.
	if (!rerr && !shcl_utf8_valid(buf, n)) rerr = 1;
	if (rerr) {
		free(buf);
		if (status) *status = SHCL_FILE_UNREADABLE;
		return NULL;
	}
	buf[n] = '\0';
	if (len) *len = n;
	if (status) *status = SHCL_FILE_CLEAN;
	return buf;
}

// File tier, load half: read and parse PATH. Never fails - the document
// always comes back usable (empty when the file could not be read), and the
// status out-param separates the four cases consumers otherwise confuse:
// absent, present-but-unreadable, parsed with errors, clean.
shcl_doc *shcl_load_file_with(const char *path, shcl_strictness s, shcl_file_status *status) {
	size_t len = 0;
	shcl_file_status st = SHCL_FILE_UNREADABLE;
	char *buf = shcl_read_file(path, 0, &len, &st);
	if (!buf) {
		if (status) *status = st;
		return shcl_parse_with("", 0, s);
	}
	shcl_doc *d = shcl_parse_with(buf, len, s);
	free(buf);
	if (!d) { if (status) *status = st; return NULL; }
	if (status) {
		*status = SHCL_FILE_CLEAN;
		for (size_t i = 0; i < d->diags.len; i++)
			if (d->diags.data[i].sev == SHCL_SEV_ERROR) { *status = SHCL_FILE_HAD_ERRORS; break; }
	}
	return d;
}

const char *shcl_file_status_name(shcl_file_status s) {
	switch (s) { case SHCL_FILE_CLEAN: return "Clean"; case SHCL_FILE_HAD_ERRORS: return "HadErrors"; case SHCL_FILE_NOT_FOUND: return "NotFound"; case SHCL_FILE_UNREADABLE: return "Unreadable"; }
	return "Clean";
}

shcl_doc *shcl_load_file(const char *path, shcl_file_status *status) {
	return shcl_load_file_with(path, SHCL_STANDARD, status);
}

// File tier, save half: write the document's canonical text to PATH through
// shcl_write_file_atomic. SHCL_SAVE_OK on success. Refuses when parsing lost
// content that a save would silently delete (shcl_lost_count) - that is
// SHCL_SAVE_REFUSED, distinct from SHCL_SAVE_FAILED so the caller need not
// guess which happened; shcl_save_file_lossy writes anyway.
shcl_save_result shcl_save_file(shcl_doc *d, const char *path) {
	if (d->lost > 0) return SHCL_SAVE_REFUSED;
	ShclStr c = emit_canonical(d);
	return shcl_write_file_atomic(path, c.p, c.n) ? SHCL_SAVE_OK : SHCL_SAVE_FAILED;
}

// shcl_save_file without the lost-content gate: writes even when parsing
// dropped lines this save deletes. The caller owns that choice. Never returns
// SHCL_SAVE_REFUSED - the gate is the one thing it skips.
shcl_save_result shcl_save_file_lossy(shcl_doc *d, const char *path) {
	ShclStr c = emit_canonical(d);
	return shcl_write_file_atomic(path, c.p, c.n) ? SHCL_SAVE_OK : SHCL_SAVE_FAILED;
}
#endif /* SHCL_NO_FILE_IO */

// --- Schema-driven generation (`shcl init --schema`) ------------------------

static ShclStr v_allowed_join(ShclArena *a, const ShclVCons *c) {
	ShclSB s = {0, 0, 0};
	char nb[64];
	for (size_t i = 0; i < c->a_n; i++) {
		if (i) sb_puts(a, &s, ", ");
		switch (c->akind) {
			case ALLOW_INTS: { snprintf(nb, sizeof nb, "%" PRId64, c->a_ints[i]); sb_puts(a, &s, nb); break; }
			case ALLOW_FLOATS: { char fb[SHCL_F64_BUF]; ShclStr f; f.p = fb; f.n = shcl_format_f64(c->a_floats[i], fb); sb_putS(a, &s, f); break; }
			case ALLOW_BOOLS: sb_puts(a, &s, c->a_bools[i] ? "true" : "false"); break;
			case ALLOW_DATES: { char db[SHCL_DT_BUF]; ShclStr d; d.p = db; d.n = shcl_datetime_str(&c->a_dates[i], db); sb_putS(a, &s, d); break; }
			case ALLOW_STRINGS: sb_putS(a, &s, c->a_strs[i]); break;
		}
	}
	return sb_S(&s);
}

// The `# type, ...` annotation line summarizing a constraint, ASCII only.
static ShclStr v_gen_annotation(ShclArena *a, const ShclVCons *c, ShclStr tyname) {
	ShclSB s = {0, 0, 0};
	char nb[80];
	sb_putS(a, &s, tyname);
	if (c->has_allowed) {
		sb_puts(a, &s, ", one of: "); sb_putS(a, &s, v_allowed_join(a, c));
	} else if (c->has_min_i || c->has_max_i) {
		if (c->has_min_i && c->has_max_i) snprintf(nb, sizeof nb, ", %" PRId64 "-%" PRId64, c->min_i, c->max_i);
		else if (c->has_min_i) snprintf(nb, sizeof nb, ", >= %" PRId64, c->min_i);
		else snprintf(nb, sizeof nb, ", <= %" PRId64, c->max_i);
		sb_puts(a, &s, nb);
	} else if (c->has_min_f || c->has_max_f) {
		char fb[SHCL_F64_BUF];
		sb_puts(a, &s, ", ");
		if (c->has_min_f && c->has_max_f) {
			ShclStr f; f.p = fb; f.n = shcl_format_f64(c->min_f, fb); sb_putS(a, &s, f);
			sb_putc(a, &s, '-');
			ShclStr g; g.p = fb; g.n = shcl_format_f64(c->max_f, fb); sb_putS(a, &s, g);
		} else if (c->has_min_f) {
			sb_puts(a, &s, ">= "); ShclStr f; f.p = fb; f.n = shcl_format_f64(c->min_f, fb); sb_putS(a, &s, f);
		} else {
			sb_puts(a, &s, "<= "); ShclStr f; f.p = fb; f.n = shcl_format_f64(c->max_f, fb); sb_putS(a, &s, f);
		}
	}
	if (c->has_repeat) {
		if (c->rep_lo == c->rep_hi) snprintf(nb, sizeof nb, ", repeat %" PRIu64, c->rep_lo);
		else snprintf(nb, sizeof nb, ", repeat %" PRIu64 "-%" PRIu64, c->rep_lo, c->rep_hi);
		sb_puts(a, &s, nb);
	}
	if (c->required) sb_puts(a, &s, ", required");
	return sb_S(&s);
}

// A field must exist when required or its repeat lower bound is 1+; a
// commented-out line for either would fail the very schema that produced it.
static int g_must_exist(const ShclVCons *c) { return c->required || (c->has_repeat && c->rep_lo >= 1); }
static int g_has_wild(const ShclVCons *c) {
	for (size_t si = 0; si < c->segs.len; si++) if (c->segs.data[si].sel.tag == SEL_WILDCARD) return 1;
	return 0;
}
// `[#N]` needs a pre-existing instance and its `#` would start a comment on a
// binding line; a newline inside a selector has no one-line spelling, since the
// value emitter never escapes one. A path deeper than a document may nest
// cannot be generated either: the line would draw E016 on the way back in. A
// newline in a NAME is writable: names are stored escape-resolved and the name
// escaper spells one `\n`.
static int g_unwritable(const ShclVCons *c) {
	if (c->segs.len > SHCL_MAX_DEPTH) return 1;
	for (size_t si = 0; si < c->segs.len; si++) {
		const ShclSegment *sg = &c->segs.data[si];
		if (sg->sel.tag == SEL_INDEX || sg->star) return 1;
		if (sg->sel.tag == SEL_VALUE && memchr(sg->sel.value.p, '\n', sg->sel.value.n)) return 1;
	}
	return 0;
}
// A repeat lower bound of 2 or more is the one documented shortfall - the line
// is emitted once and the count reported - so it is not the fault below.
static int g_cannot_satisfy(const ShclVCons *c) { return c->required || (c->has_repeat && c->rep_lo == 1); }
static int g_path_has_nl(const ShclVCons *c) {
	for (size_t k = 0; k < c->path.n; k++) if (c->path.p[k] == '\n') return 1;
	return 0;
}
// s with every '\n' escaped to backslash-n (comments and annotations must stay
// one line no matter what an allowed value smuggles in).
static ShclStr g_escape_nl(ShclArena *a, ShclStr s) {
	int has = 0;
	for (size_t k = 0; k < s.n; k++) if (s.p[k] == '\n') { has = 1; break; }
	if (!has) return s;
	ShclSB b = {0, 0, 0};
	for (size_t k = 0; k < s.n; k++) {
		if (s.p[k] == '\n') sb_puts(a, &b, "\\n");
		else sb_putc(a, &b, s.p[k]);
	}
	return sb_S(&b);
}
// A default carrying a literal newline cannot sit on a value line; the quoted
// escaped spelling reads back to the same string.
static ShclStr g_default_text(ShclArena *a, ShclStr v) {
	int has = 0;
	for (size_t k = 0; k < v.n; k++) if (v.p[k] == '\n') { has = 1; break; }
	if (!has) return v;
	ShclSB b = {0, 0, 0};
	sb_putc(a, &b, '"');
	for (size_t k = 0; k < v.n; k++) {
		char ch = v.p[k];
		if (ch == '\\') sb_puts(a, &b, "\\\\");
		else if (ch == '"') sb_puts(a, &b, "\\\"");
		else if (ch == '\n') sb_puts(a, &b, "\\n");
		else if (ch == '\t') sb_puts(a, &b, "\\t");
		else sb_putc(a, &b, ch);
	}
	sb_putc(a, &b, '"');
	return sb_S(&b);
}

// Ceiling on how many fields one schema may expand to. Fragments that mount
// each other at more than one path multiply, so a short schema can otherwise
// ask for more output than the machine can hold; past this the generator
// reports a schema fault rather than running until something breaks.
#define GEN_MAX_FIELDS ((size_t)10000)

// Footer telling whoever opens the generated file what the format is and where
// its spec lives. It is output, so every binding emits these bytes exactly; the
// Legal line names SHCL as its subject so it cannot be read as a claim over the
// config it sits in.
#define GEN_BANNER \
	"#\n" \
	"# This config file format is SHCL.\n" \
	"# \"Simple Hierarchical Config Language\"\n" \
	"#    Home     https://github.com/jim-collier/shcl\n" \
	"#    Syntax   https://github.com/jim-collier/shcl/blob/main/project/spec.md\n" \
	"#    Legal    SHCL is Copyright © 2026 Jim Collier. License: MIT. No warranty.\n" \
	"#\n"

/* Whether a V007 from the self-check is the sanctioned kind: its message ends
   `: N not in LO..HI`, and LO is 2 or more. */
static int v007_sanctioned(ShclStr message) {
	const char *tail = NULL;
	for (size_t i = 0; i + 8 <= message.n; i++) if (memcmp(message.p + i, " not in ", 8) == 0) tail = message.p + i + 8;
	if (!tail) return 0;
	uint64_t lo = 0; size_t k = 0, n = message.n - (size_t)(tail - message.p);
	while (k < n && tail[k] >= '0' && tail[k] <= '9') { lo = lo * 10 + (uint64_t)(tail[k] - '0'); k++; }
	return k > 0 && lo >= 2;
}

/* Parent lines that carry a value, keyed by their segment names: the live
   must-exist concrete constraints with a default. A dotted child of one has
   to select that instance by the value, or it names the empty-valued one. */
typedef struct { const ShclVecSeg *segs; ShclStr value; } ShclParentValue;
typedef struct { ShclParentValue *data; size_t len; } ShclParentValues;
static const ShclStr *parent_value_for(const ShclParentValues *pv, const ShclVecSeg *segs, size_t n) {
	for (size_t i = 0; i < pv->len; i++) {
		if (pv->data[i].segs->len != n) continue;
		int eq = 1;
		for (size_t k = 0; k < n && eq; k++) eq = s_eq(pv->data[i].segs->data[k].name, segs->data[k].name);
		if (eq) return &pv->data[i].value;
	}
	return NULL;
}

/* A default's spelling inside a `[value]` selector: the value-side text as
   is, except that a bracket or a backslash would end or escape the selector,
   so those go quoted (the selector matches on the escaped display, so the
   quoted spelling finds the bare value). */
static ShclStr gen_selector_text(ShclArena *a, ShclStr v) {
	if (memchr(v.p, '\n', v.n)) return g_default_text(a, v);
	// The scanner reads a bare selector body as an index when it is all digits
	// (with an optional sign or `#`), and as a wildcard when it is `*`, so a
	// default of that shape has to be quoted or the line names an instance
	// that is not there.
	ShclStr body = s_trim(v);
	uint64_t ix;
	int reads_as_selector = (body.n == 1 && body.p[0] == '*')
		|| parse_u64(body, &ix)
		|| (body.n >= 1 && body.p[0] == '#' && parse_u64(s_slice(body, 1, body.n), &ix));
	if ((memchr(v.p, '[', v.n) || memchr(v.p, ']', v.n) || memchr(v.p, '\\', v.n) || reads_as_selector) && !quoted_shape(v)) return quote_text(a, v);
	return v;
}

// Render parsed segments back as a dotted path, dropping wildcard selectors
// (a generated line targets the one instance it materializes) and quoting a
// name that needs it, so the result is a path the scanner reads back the same.
// A segment whose prefix names a live line carrying a value selects that
// instance by the value, in place of a wildcard or a bare name.
static ShclStr gen_path_text(ShclArena *a, const ShclVecSeg *segs, const ShclParentValues *pv) {
	ShclSB out = {0, 0, 0};
	char nb[32];
	for (size_t i = 0; i < segs->len; i++) {
		const ShclSegment *s = &segs->data[i];
		if (i > 0) sb_putc(a, &out, '.');
		if (s->star) sb_putc(a, &out, '*');
		else sb_putS(a, &out, emit_name(a, s->name));
		const ShclStr *v = (i + 1 < segs->len && s->sel.tag != SEL_VALUE) ? parent_value_for(pv, segs, i + 1) : NULL;
		if (v) { sb_putc(a, &out, '['); sb_putS(a, &out, gen_selector_text(a, *v)); sb_putc(a, &out, ']'); continue; }
		switch (s->sel.tag) {
		case SEL_VALUE:
			sb_putc(a, &out, '[');
			if (s->sel.quoted) sb_putS(a, &out, quote_text(a, s->sel.value));
			else sb_putS(a, &out, s->sel.value);
			sb_putc(a, &out, ']'); break;
		case SEL_INDEX: { int nn = snprintf(nb, sizeof nb, "[#%" PRIu64 "]", s->sel.index); sb_put(a, &out, nb, (size_t)nn); break; }
		case SEL_WILDCARD: case SEL_NONE: break;
		}
	}
	return sb_S(&out);
}

// Inline every fragment mount into a flat constraint list, depth-first in
// schema order, each field's path and segments prefixed by its mount's. A
// mount whose fragment is already expanding (a cycle) stops there and is
// recorded as (path, fragment name) for the trailing not-generated block.
static void g_expand_go(ShclArena *a, const ShclVecVCons *list, const ShclVSchemaDef *def, const ShclStr *at_path, const ShclVecSeg *at_segs, ShclVecS *stack, ShclVecVCons *out, ShclVecS *cut_path, ShclVecS *cut_frag) {
	for (size_t i = 0; i < list->len; i++) {
		const ShclVCons *c = &list->data[i];
		ShclVCons cc = *c;
		if (at_path) {
			ShclSB p = {0, 0, 0};
			sb_putS(a, &p, *at_path); sb_putc(a, &p, '.'); sb_putS(a, &p, c->path);
			cc.path = sb_S(&p);
			ShclVecSeg segs = {0, 0, 0};
			for (size_t k = 0; k < at_segs->len; k++) ShclVecSeg_push(a, &segs, at_segs->data[k]);
			for (size_t k = 0; k < c->segs.len; k++) ShclVecSeg_push(a, &segs, c->segs.data[k]);
			cc.segs = segs;
		}
		ShclStr path = cc.path; ShclVecSeg segs = cc.segs;
		if (out->len > GEN_MAX_FIELDS) return;
		ShclVecVCons_push(a, out, cc);
		if (c->inherits.n) {
			int cycling = 0;
			for (size_t k = 0; k < stack->len; k++) if (s_eq(stack->data[k], c->inherits)) { cycling = 1; break; }
			// A chain long enough to outrun the stack, or a mount that
			// re-enters, stops here and is noted instead of expanded.
			if (cycling || stack->len >= SHCL_MAX_DEPTH) {
				ShclVecS_push(a, cut_path, g_escape_nl(a, path));
				ShclVecS_push(a, cut_frag, c->inherits);
			} else {
				const ShclVecVCons *fcs = v_frag_get(def, c->inherits);
				if (fcs) {
					ShclVecS_push(a, stack, c->inherits);
					g_expand_go(a, fcs, def, &path, &segs, stack, out, cut_path, cut_frag);
					stack->len--;
				}
			}
		}
	}
}
static void g_expand_mounts(ShclArena *a, const ShclVSchemaDef *def, ShclVecVCons *out, ShclVecS *cut_path, ShclVecS *cut_frag) {
	ShclVecS stack = {0, 0, 0};
	g_expand_go(a, &def->cons, def, NULL, NULL, &stack, out, cut_path, cut_frag);
}

shcl_str shcl_generate(shcl_doc *schema, int no_banner, int *ok) {
	// Only the returned bytes are contracted to live in the schema's arena;
	// the constraint/fault lists, expansion copies, and the output builder are
	// ~60x that, so they build in a private arena (shcl_validate's discipline)
	// and die here - the caller may generate from one schema repeatedly.
	ShclArena tmp; memset(&tmp, 0, sizeof tmp);
	ShclArena *a = &tmp;
	ShclVSchemaDef def; memset(&def, 0, sizeof def);
	ShclVecDiag faults = {0, 0, 0};
	/* V096 and V097 can only come from generation, so any on the schema are
	   this call's predecessors. Drop them, or a caller generating in a loop
	   collects one copy per attempt and the diagnostic count stops meaning
	   anything. */
	{
		size_t w = 0;
		for (size_t i = 0; i < schema->diags.len; i++) {
			const char *c = schema->diags.data[i].code;
			if (c && (!strcmp(c, "V096") || !strcmp(c, "V097"))) continue;
			schema->diags.data[w++] = schema->diags.data[i];
		}
		schema->diags.len = w;
	}
	// Generation lays the whole schema out, so unlike validation it has no
	// safe partial mode: any fault fails it.
	v_build_schema(a, schema, &def, &faults);
	shcl_str r;
	if (faults.len) {
		// Recorded on the schema document like every other generation fault,
		// so a caller sees the V09x list itself and does not have to rebuild
		// it by validating an empty document (which adds that document's own
		// V002/V007 to the list).
		for (size_t i = 0; i < faults.len; i++) push_diag(schema, faults.data[i].line, faults.data[i].sev, faults.data[i].code, s_dup(&schema->arena, faults.data[i].message));
		if (ok) *ok = 0;
		ShclStr e = s_empty(); r.p = e.p; r.n = e.n; arena_free(&tmp); return r;
	}
	if (ok) *ok = 1;
	ShclVecVCons cons = {0, 0, 0};
	ShclVecS cut_path = {0, 0, 0}, cut_frag = {0, 0, 0};
	g_expand_mounts(a, &def, &cons, &cut_path, &cut_frag);
	if (cons.len > GEN_MAX_FIELDS) {
		// Generation-only fault: recorded on the schema document (this
		// signature has no fault list of its own to return).
		ShclSB m = {0, 0, 0};
		sb_puts(a, &m, "schema expands past "); sb_put_u64(a, &m, GEN_MAX_FIELDS);
		sb_puts(a, &m, " fields; fragments mounted at more than one path multiply");
		// the diag outlives this call: its text must leave the private arena
		push_diag(schema, 0, SHCL_SEV_ERROR, "V096", s_dup(&schema->arena, sb_S(&m)));
		if (ok) *ok = 0;
		ShclStr e = s_empty(); r.p = e.p; r.n = e.n; arena_free(&tmp); return r;
	}
	// Live concrete paths materialize instances; decide which must-exist
	// wildcards get filled (their first-wildcard parent chain is a prefix of
	// some live path's name list). Fixpoint: a fill can materialize another's
	// parent. Live paths are stored as their segment-name lists.
	size_t nlive = 0, clive = 0;
	ShclVecSeg *live = NULL;
	int *fill = (int *)arena_alloc(a, (cons.len ? cons.len : 1) * sizeof *fill);
	for (size_t i = 0; i < cons.len; i++) fill[i] = 0;
	#define LIVE_PUSH(SEGS) do { if (nlive == clive) { clive = clive ? clive * 2 : 8; ShclVecSeg *nl = (ShclVecSeg *)arena_alloc(a, clive * sizeof *nl); for (size_t t = 0; t < nlive; t++) nl[t] = live[t]; live = nl; } live[nlive++] = (SEGS); } while (0)
	for (size_t i = 0; i < cons.len; i++) {
		const ShclVCons *c = &cons.data[i];
		if (!g_has_wild(c) && !g_unwritable(c) && g_must_exist(c)) LIVE_PUSH(c->segs);
	}
	for (;;) {
		int changed = 0;
		for (size_t i = 0; i < cons.len; i++) {
			const ShclVCons *c = &cons.data[i];
			if (fill[i] || !g_has_wild(c) || g_unwritable(c) || !g_must_exist(c)) continue;
			size_t k = 0;
			while (c->segs.data[k].sel.tag != SEL_WILDCARD) k++;
			size_t plen = k + 1; // parent chain: names up to and including the wildcard segment
			int hit = 0;
			for (size_t li = 0; li < nlive && !hit; li++) {
				if (live[li].len < plen) continue;
				int eq = 1;
				for (size_t s2 = 0; s2 < plen; s2++) if (!s_eq(live[li].data[s2].name, c->segs.data[s2].name)) { eq = 0; break; }
				hit = eq;
			}
			// A wildcard in the last segment needs no other line to
			// materialize its parent: the line generated from it is that
			// instance.
			if (plen == c->segs.len) hit = 1;
			if (hit) { fill[i] = 1; LIVE_PUSH(c->segs); changed = 1; }
		}
		if (!changed) break;
	}
	#undef LIVE_PUSH
	/* A live line with a value materializes an instance carrying that value,
	   and a dotted child names the empty-valued instance instead - so `srv:
	   web` followed by `srv.port:` is two `srv` nodes, and the child never
	   lands where the schema looks. Any line under such a parent selects it
	   by its value: `srv[web].port:`. */
	ShclParentValues pv; pv.len = 0;
	pv.data = (ShclParentValue *)arena_alloc(a, (cons.len ? cons.len : 1) * sizeof *pv.data);
	for (size_t i = 0; i < cons.len; i++) {
		const ShclVCons *c = &cons.data[i];
		// A filled wildcard emits a valued line of its own, so it belongs here too.
		if ((!g_has_wild(c) || fill[i]) && !g_unwritable(c) && g_must_exist(c) && c->has_default) { pv.data[pv.len].segs = &c->segs; pv.data[pv.len].value = c->default_text; pv.len++; }
	}
	/* A path that cannot be written at all belongs in the trailing note, but one
	   that must exist can never be satisfied from there: the self-check would
	   then report the document as missing a path, which points at the config
	   rather than at the schema line that cannot be generated. */
	for (size_t i = 0; i < cons.len; i++) {
		const ShclVCons *c = &cons.data[i];
		if (!g_cannot_satisfy(c) || !g_unwritable(c) || g_has_wild(c)) continue;
		ShclSB m = {0, 0, 0};
		sb_puts(a, &m, "required path cannot be generated: ");
		sb_putS(a, &m, g_escape_nl(a, c->path));
		push_diag(schema, 0, SHCL_SEV_ERROR, "V097", s_dup(&schema->arena, sb_S(&m)));
		if (ok) *ok = 0;
		ShclStr e = s_empty(); r.p = e.p; r.n = e.n; arena_free(&tmp); return r;
	}
	ShclSB out = {0, 0, 0};
	ShclVecS wild_path = {0, 0, 0}, wild_type = {0, 0, 0};
	/* Dropping a trailing `[*]` can render the same line a concrete sibling
	   already wrote; the first spelling wins. Hash first, bytes only on a
	   hash hit, so the scan stays cheap at the field cap. */
	ShclVecS emitted = {0, 0, 0};
	uint64_t *emitted_hash = (uint64_t *)arena_alloc(a, (cons.len ? cons.len : 1) * sizeof *emitted_hash);
	int first = 1;
	for (size_t i = 0; i < cons.len; i++) {
		ShclVCons *c = &cons.data[i];
		ShclStr tyname;
		if (c->ty) { tyname.p = c->ty; tyname.n = strlen(c->ty); } else tyname = s_lit("any");
		if (g_unwritable(c) || (g_has_wild(c) && !fill[i])) {
			ShclVecS_push(a, &wild_path, g_escape_nl(a, c->path)); ShclVecS_push(a, &wild_type, tyname);
			continue;
		}
		// A filled wildcard emits in dotted form, targeting the materialized
		// instance - by its value when the materializing line carries one.
		// Rebuilt from the parsed segments, not by cutting text out of the
		// path: the same path can be written several ways, and only the
		// segments say what it means. Otherwise the schema's own spelling.
		int under_valued_parent = 0;
		for (size_t k = 1; k < c->segs.len && !under_valued_parent; k++)
			under_valued_parent = c->segs.data[k - 1].sel.tag == SEL_NONE && parent_value_for(&pv, &c->segs, k) != NULL;
		// A name carrying a newline has no verbatim spelling on a binding line;
		// the segment renderer escapes it, so such a path goes through there
		// whether or not it was filled.
		ShclStr path = (fill[i] || under_valued_parent || g_path_has_nl(c)) ? gen_path_text(a, &c->segs, &pv) : c->path;
		uint64_t ph = fnv_str(1469598103934665603ull, path);
		int dup = 0;
		for (size_t k = 0; k < emitted.len && !dup; k++) dup = emitted_hash[k] == ph && s_eq(emitted.data[k], path);
		if (dup) continue;
		emitted_hash[emitted.len] = ph;
		ShclVecS_push(a, &emitted, path);
		if (!first) sb_putc(a, &out, '\n');
		first = 0;
		if (c->has_desc) {
			size_t start = 0;
			for (size_t k = 0; k <= c->desc.n; k++) {
				if (k == c->desc.n || c->desc.p[k] == '\n') {
					sb_puts(a, &out, "# ");
					ShclStr ln; ln.p = c->desc.p + start; ln.n = k - start; sb_putS(a, &out, ln);
					sb_putc(a, &out, '\n');
					start = k + 1;
				}
			}
		}
		sb_puts(a, &out, "# "); sb_putS(a, &out, g_escape_nl(a, v_gen_annotation(a, c, tyname))); sb_putc(a, &out, '\n');
		if (!g_must_exist(c)) sb_putc(a, &out, '#');
		sb_putS(a, &out, path);
		if (c->has_default) { sb_puts(a, &out, ": "); sb_putS(a, &out, g_default_text(a, c->default_text)); }
		else sb_putc(a, &out, ':');
		sb_putc(a, &out, '\n');
	}
	// Cycle-cut mounts last: their "type" column names the fragment that
	// belongs at the path.
	for (size_t i = 0; i < cut_path.len; i++) {
		ShclVecS_push(a, &wild_path, cut_path.data[i]);
		ShclVecS_push(a, &wild_type, cut_frag.data[i]);
	}
	if (wild_path.len) {
		if (!first) sb_putc(a, &out, '\n');
		sb_puts(a, &out, "# Paths needing an instance name (not generated):\n");
		for (size_t i = 0; i < wild_path.len; i++) {
			sb_puts(a, &out, "#   "); sb_putS(a, &out, wild_path.data[i]);
			sb_puts(a, &out, "   "); sb_putS(a, &out, wild_type.data[i]); sb_putc(a, &out, '\n');
		}
	}
	if (!no_banner) {
		if (out.len) sb_putc(a, &out, '\n');
		sb_puts(a, &out, GEN_BANNER);
	}
	ShclStr s = sb_S(&out);
	/* The output promises to validate clean against the schema that produced
	   it, so check that here rather than trusting each branch above. A
	   `default` outside its own field's constraints is the schema's fault, and
	   the author should hear about it instead of getting a starter config that
	   fails the first time it is checked. The one sanctioned shortfall is a
	   V007 for a repeat lower bound of 2+: generating that many identical
	   lines merges them into one. A lower bound of 1 is a must-exist path
	   like any other, so its V007 is a fault. */
	{
		shcl_doc *self_ = shcl_parse(s.p, s.n);
		shcl_validation *v = self_ ? shcl_validate(self_, schema) : NULL;
		size_t nv = v ? shcl_validation_count(v) : 0, nbad = 0;
		for (size_t i = 0; i < nv; i++) {
			const char *code = shcl_validation_code(v, i);
			if (shcl_validation_severity(v, i) != SHCL_SEV_ERROR) continue;
			if (code && strcmp(code, "V007") == 0 && v007_sanctioned(shcl_validation_message(v, i))) continue;
			ShclSB m = {0, 0, 0};
			sb_puts(a, &m, "generated value fails the schema that produced it: ");
			sb_putS(a, &m, shcl_validation_message(v, i));
			/* the diag outlives this call: its text must leave the private arena */
			push_diag(schema, 0, SHCL_SEV_ERROR, "V097", s_dup(&schema->arena, sb_S(&m)));
			nbad++;
		}
		if (v) shcl_validation_free(v);
		if (self_) shcl_free(self_);
		if (nbad) {
			if (ok) *ok = 0;
			ShclStr e = s_empty(); r.p = e.p; r.n = e.n; arena_free(&tmp);
			return r;
		}
	}
	/* Only now does the text leave the private arena, and it goes to the read
	   arena rather than the document's own: a refused call then costs the
	   schema nothing, and a caller generating in a loop can give the copies
	   back with shcl_reads_release. */
	ShclStr kept = s_dup(&schema->reads, s); r.p = kept.p; r.n = kept.n;
	arena_free(&tmp);
	return r;
}

#ifdef __cplusplus
#pragma GCC diagnostic pop
#endif

// The implementation's short internal macros would otherwise outlive the header
// in the consumer's own translation unit, where these names are common.
#undef DEFINE_VEC
#undef ROOT
#undef NODE
#undef NIL
#undef DEAD
#undef UNOPENED
#undef GEN_MAX_FIELDS
#undef GEN_BANNER

#endif // SHCL_IMPLEMENTATION
#endif // SHCL_H
