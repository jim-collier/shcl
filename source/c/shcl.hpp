// SPDX-License-Identifier: MIT
// Copyright © 2026 Jim Collier [ID: 2უNაɘ«҂թȹɤξπ๙¿ձϖ]

// C++ typed veneer over the C core (shcl.h). This is NOT a second parser: it
// wraps the same shcl_* functions and adds a compile-time-typed surface
// (Read<T>, get<T>()), so it inherits the core's conformance. Drop shcl.h and
// shcl.hpp into your tree; in one TU, #define SHCL_IMPLEMENTATION before either.

#ifndef SHCL_HPP
#define SHCL_HPP

#include "shcl.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <utility>

namespace shcl {

enum class Strictness { Loose = SHCL_LOOSE, Standard = SHCL_STANDARD, Strict = SHCL_STRICT };
enum class Status { Good = SHCL_GOOD, Empty = SHCL_EMPTY, NotFound = SHCL_NOT_FOUND, BadType = SHCL_BAD_TYPE, Multiple = SHCL_MULTIPLE };
enum class WriteReason { Writable = SHCL_W_WRITABLE, BadPath = SHCL_W_BAD_PATH, ValueInPath = SHCL_W_VALUE_IN_PATH, Wildcard = SHCL_W_WILDCARD, NoSuchIndex = SHCL_W_NO_SUCH_INDEX, TooDeep = SHCL_W_TOO_DEEP };

template <class T> struct Read {
	T value{};
	Status status{};
	// Per-slot statuses, array reads only: one entry per slot the path
	// resolved, aligned with value, so a partially-resolved array says which
	// slots failed and why rather than only that the whole read did. Empty for
	// a scalar read.
	std::vector<Status> slots{};
	// Whether the author addressed this field at all: Good or Empty. Note this
	// deliberately answers differently from get_or, which falls back on Empty
	// like any other non-Good read - ok() asks "is this field spoken for",
	// get_or() asks "do I have a usable value", and an explicitly emptied field
	// is the case where those two diverge.
	bool ok() const { return shcl_status_ok(static_cast<shcl_status>(status)) != 0; }
};

struct Diagnostic { std::size_t line{}; bool is_error{}; std::string message{}; std::string code{}; };

// Owning structured datetime. The core's shcl_datetime borrows its frac digits
// from the document's arena; this copies them so the value keeps the veneer's
// RAII promise and may outlive the Document. Copies and moves re-point the C
// view at their own storage - a move re-points the source's too, or the
// moved-from object keeps a view into storage it just handed away.
class Datetime {
	shcl_datetime v_{};
	std::string frac_;
	// The view always describes this object's own storage, has_frac included -
	// so a moved-from Datetime, whose string is emptied, cannot go on formatting
	// a fraction it no longer holds.
	void rebind() { v_.frac.p = frac_.data(); v_.frac.n = frac_.size(); v_.has_frac = !frac_.empty(); }
public:
	Datetime() { v_.zone = SHCL_ZONE_NONE; }
	explicit Datetime(const shcl_datetime &v) : v_(v), frac_(v.frac.p ? std::string(v.frac.p, v.frac.n) : std::string()) { rebind(); }
	Datetime(const Datetime &o) : v_(o.v_), frac_(o.frac_) { rebind(); }
	Datetime &operator=(const Datetime &o) { v_ = o.v_; frac_ = o.frac_; rebind(); return *this; }
	Datetime(Datetime &&o) noexcept : v_(o.v_), frac_(std::move(o.frac_)) { rebind(); o.rebind(); }
	Datetime &operator=(Datetime &&o) noexcept { v_ = o.v_; frac_ = std::move(o.frac_); rebind(); o.rebind(); return *this; }
	// The C view, for shcl_datetime_str and friends: valid as long as *this.
	const shcl_datetime &c() const noexcept { return v_; }
	// The reference's textual form.
	std::string str() const { char b[SHCL_DT_BUF]; return std::string(b, shcl_datetime_str(&v_, b)); }
};

inline std::string to_str(shcl_str s) { return std::string(s.p, s.n); }

inline std::vector<Status> to_slots(const shcl_status *s, std::size_t n) {
	std::vector<Status> v; v.reserve(n);
	for (std::size_t i = 0; i < n; i++) v.push_back(static_cast<Status>(s[i]));
	return v;
}

// Status as text, for a log line or a message. Borrowed from static storage.
inline const char *to_string(Status s) { return shcl_status_name(static_cast<shcl_status>(s)); }
// The CLI exit code a read with this status ends on.
inline int status_code(Status s) { return shcl_status_code(static_cast<shcl_status>(s)); }

// The CLI's strictness spellings, loose|standard|strict or 1|2|3, in any case.
inline std::optional<Strictness> strictness_from_arg(std::string_view s) {
	shcl_strictness out = SHCL_STANDARD;
	if (!shcl_strictness_from_arg(s.data(), s.size(), &out)) return std::nullopt;
	return static_cast<Strictness>(out);
}

// A float in the reference's textual form, the spelling a save writes.
inline std::string format_float(double v) { char b[SHCL_FLOAT_BUF]; return std::string(b, shcl_format_float(v, b)); }

// Text to a datetime, per the whitelist. Empty when the text is not one.
inline std::optional<Datetime> parse_datetime(std::string_view s) {
	shcl_datetime out;
	if (!shcl_parse_datetime(s.data(), s.size(), &out)) return std::nullopt;
	return Datetime(out);
}

// The tokenizer's view of one line or one lookup path, as `shcl tokens` prints
// it. Every offset is into the text that was tokenized. The spans are copied
// out of the core, so a Tokens outlives the next read and the Document.
enum class Quote { None = SHCL_QUOTE_NONE, Single = SHCL_QUOTE_SINGLE, Double = SHCL_QUOTE_DOUBLE, Open = SHCL_QUOTE_OPEN };
enum class Rules { Current = SHCL_RULES_CURRENT, V2 = SHCL_RULES_V2 };
struct Piece { std::size_t start{}; std::size_t end{}; Quote quote{}; };
struct SegTok { Piece name{}; std::optional<Piece> selector{}; bool star{}; };
struct Tokens {
	std::vector<SegTok> segments{};
	std::optional<std::size_t> sep{};
	std::size_t value_start{};
	std::size_t value_end{};
	std::vector<Piece> elements{};
	std::optional<std::size_t> comment{};
	// Where the path stopped making sense, and why. The reason is static text.
	std::optional<std::size_t> fault_at{};
	const char *fault_why = nullptr;
	// The caller's element cap (0 = none), kept across calls. capped says it
	// stopped the scan, and elements is then incomplete.
	std::size_t cap{};
	bool capped{};

	// Asked of the core rather than counted here, so what counts as an element
	// is decided in one place.
	std::size_t element_count() const {
		std::vector<shcl_piece> e; e.reserve(elements.size());
		for (const Piece &x : elements) e.push_back({x.start, x.end, static_cast<shcl_quote>(x.quote)});
		shcl_tokens t{}; t.elements = e.data(); t.nelem = e.size();
		return shcl_tokens_element_count(&t);
	}
};

class Document {
	// unique_ptr owns the C handle: moves transfer it, copies stay deleted,
	// and destruction frees it - no hand-written rule of five to get wrong.
	struct Free { void operator()(shcl_doc *d) const noexcept { shcl_free(d); } };
	std::unique_ptr<shcl_doc, Free> d_;
	static Status st(shcl_status s) { return static_cast<Status>(s); }

	// What the array setters hand the core. Each lives only for the call.
	struct StrArgs { std::vector<const char *> p; std::vector<std::size_t> n; };
	static StrArgs str_args(const std::vector<std::string> &v) {
		StrArgs a; a.p.reserve(v.size()); a.n.reserve(v.size());
		for (const std::string &s : v) { a.p.push_back(s.data()); a.n.push_back(s.size()); }
		return a;
	}
	// std::vector<bool> packs its bits and has no data(), and the core takes ints.
	static std::vector<int> bool_args(const std::vector<bool> &v) { return std::vector<int>(v.begin(), v.end()); }
	// Each view borrows its fraction digits from the Datetime it came from.
	static std::vector<shcl_datetime> dt_args(const std::vector<Datetime> &v) {
		std::vector<shcl_datetime> a; a.reserve(v.size());
		for (const Datetime &x : v) a.push_back(x.c());
		return a;
	}

	// The core's spans live in the read arena, which the next read gives back,
	// so they are copied before the call returns.
	static void copy_tokens(const shcl_tokens &t, Tokens &out) {
		auto piece = [](const shcl_piece &p) { return Piece{p.start, p.end, static_cast<Quote>(p.quote)}; };
		out.segments.clear();
		out.segments.reserve(t.nseg);
		for (std::size_t i = 0; i < t.nseg; i++) {
			const shcl_seg_tok &s = t.segments[i];
			out.segments.push_back({piece(s.name), s.has_selector ? std::optional<Piece>(piece(s.selector)) : std::nullopt, s.star != 0});
		}
		out.sep = t.has_sep ? std::optional<std::size_t>(t.sep) : std::nullopt;
		out.value_start = t.value_start;
		out.value_end = t.value_end;
		out.elements.clear();
		out.elements.reserve(t.nelem);
		for (std::size_t i = 0; i < t.nelem; i++) out.elements.push_back(piece(t.elements[i]));
		out.comment = t.has_comment ? std::optional<std::size_t>(t.comment) : std::nullopt;
		out.fault_at = t.has_fault ? std::optional<std::size_t>(t.fault_at) : std::nullopt;
		out.fault_why = t.has_fault ? t.fault_why : nullptr;
		out.capped = t.capped != 0;
	}
public:
	// An empty document, so a default-constructed Document is usable (every
	// accessor hands the handle to the C core, which takes no null).
	Document() : d_(shcl_new()) {}
	// Takes ownership: the Document frees d.
	explicit Document(shcl_doc *d) : d_(d) {}
	Document(const Document &) = delete;
	Document &operator=(const Document &) = delete;
	Document(Document &&) noexcept = default;
	Document &operator=(Document &&) noexcept = default;

	// False when the parse or load could not allocate and there is no document
	// to work with. True on any system that has not run out of memory, which is
	// most of them; every accessor below assumes a true one.
	explicit operator bool() const { return d_ != nullptr; }

	// The C handle, for anything the veneer leaves out. The Document still owns
	// it, so never shcl_free it, and it is null on a moved-from Document. Every
	// veneer call that copies a result gives the core's read memory back first,
	// so a shcl_str taken through this handle does not survive the next veneer
	// read on the same document.
	shcl_doc *c() const noexcept { return d_.get(); }

	// A parse never fails on the document's account: bad lines are skipped and
	// diagnosed.
	static Document parse(std::string_view t) { return Document(shcl_parse(t.data(), t.size())); }
	static Document parse_with(std::string_view t, Strictness s) { return Document(shcl_parse_with(t.data(), t.size(), static_cast<shcl_strictness>(s))); }
	// Parse with resource caps (E020 stops the parse past max_nodes, E021
	// refuses a line whose array would exceed max_elements, E022 ends a
	// diagnostics list cut at max_diags with a count of the rest; 0 = no cap).
	static Document parse_limited(std::string_view t, Strictness s, std::size_t max_nodes, std::size_t max_elements, std::size_t max_diags) { return Document(shcl_parse_limited(t.data(), t.size(), static_cast<shcl_strictness>(s), max_nodes, max_elements, max_diags)); }
	// What migrate produced, and what it could not carry across: current when
	// the file already names its format, ambiguous for pieces the two rule
	// sets read differently and nothing can decide between, lost for lines
	// 2.x bound a value on that nothing binds now.
	struct Migration {
		std::string text;
		bool current = false;
		std::size_t ambiguous = 0;
		std::size_t lost = 0;
	};
	// A document written under the 2.x lexical rules, rewritten so this parser
	// reads the same tree; text to text, no document involved. from_v2 says the
	// file really was written for 2.x, which is the only thing that can settle
	// the spellings the two rule sets read differently.
	static Migration migrate(std::string_view t, bool from_v2) {
		shcl_migration m = shcl_migrate(t.data(), t.size(), from_v2 ? 1 : 0);
		// Owned from the call on, so a throw below cannot leak the C buffer.
		std::unique_ptr<char, void (*)(void *)> p(m.text, &std::free);
		return Migration{std::string(p.get(), m.len), m.current != 0, m.ambiguous, m.lost};
	}
	// migrate() without the version line or the migrated note, for a program
	// that writes SHCL_GEN_BANNER itself, which carries the version line.
	static Migration migrate_unstamped(std::string_view t, bool from_v2) {
		shcl_migration m = shcl_migrate_unstamped(t.data(), t.size(), from_v2 ? 1 : 0);
		std::unique_ptr<char, void (*)(void *)> p(m.text, &std::free);
		return Migration{std::string(p.get(), m.len), m.current != 0, m.ambiguous, m.lost};
	}
	// The format major a document's Format line names, read the way migrate
	// reads it; none when no line names one. migrate hands a file back
	// untouched exactly when this is SHCL_FORMAT_MAJOR or more.
	static std::optional<std::uint32_t> format_version(std::string_view t) {
		std::int64_t v = shcl_format_version(t.data(), t.size());
		if (v < 0) return std::nullopt;
		return static_cast<std::uint32_t>(v);
	}

#ifndef SHCL_NO_FILE_IO
	// File tier: load does not fail on the file's account (the document always
	// comes back usable, empty when the file could not be read; the status
	// separates absent / unreadable / parsed-with-errors / clean - an
	// allocation failure is the exception, and shows as a false document), and
	// save writes canonical text atomically - the CLI --write mechanics.
	enum class FileStatus { Clean = SHCL_FILE_CLEAN, HadErrors = SHCL_FILE_HAD_ERRORS, NotFound = SHCL_FILE_NOT_FOUND, Unreadable = SHCL_FILE_UNREADABLE };
	static Document load_file(const std::string &path, FileStatus *status = nullptr) {
		// Initialized: an out-parameter the caller reads unconditionally should
		// never depend on the callee having written it.
		shcl_file_status cs = SHCL_FILE_UNREADABLE;
		Document d(shcl_load_file(path.c_str(), &cs));
		if (status) *status = static_cast<FileStatus>(cs);
		return d;
	}
	// Textual name of a file status, for a log line. Borrowed, static.
	static const char *to_string(FileStatus s) { return shcl_file_status_name(static_cast<shcl_file_status>(s)); }
	static Document load_file_with(const std::string &path, Strictness s, FileStatus *status = nullptr) {
		shcl_file_status cs = SHCL_FILE_UNREADABLE;
		Document d(shcl_load_file_with(path.c_str(), static_cast<shcl_strictness>(s), &cs));
		if (status) *status = static_cast<FileStatus>(cs);
		return d;
	}
	// The read half on its own: the file's text, or nullopt with the status
	// saying why (a file past max_bytes is Unreadable; 0 is no cap). load_file
	// is this plus a parse.
	static std::optional<std::string> read_file(const std::string &path, std::size_t max_bytes = 0, FileStatus *status = nullptr) {
		shcl_file_status cs = SHCL_FILE_UNREADABLE;
		std::size_t n = 0;
		// Owned from the call on, so a throw below cannot leak the C buffer.
		std::unique_ptr<char, void (*)(void *)> p(shcl_read_file(path.c_str(), max_bytes, &n, &cs), &std::free);
		if (status) *status = static_cast<FileStatus>(cs);
		if (!p) return std::nullopt;
		return std::string(p.get(), n);
	}
	enum class SaveResult { Ok = SHCL_SAVE_OK, Refused = SHCL_SAVE_REFUSED, Failed = SHCL_SAVE_FAILED };
	// Not a bool: Refused is the lost-content gate, which save_file_lossy
	// overrides, and folding it into a failed write leaves the caller with an
	// override they cannot tell they need.
	SaveResult save_file(const std::string &path) const { return static_cast<SaveResult>(shcl_save_file(d_.get(), path.c_str())); }
	SaveResult save_file_lossy(const std::string &path) const { return static_cast<SaveResult>(shcl_save_file_lossy(d_.get(), path.c_str())); }
	// The temp-file-and-rename write the saves go through, for bytes that are
	// not a document. False when it failed, with errno saying why.
	static bool write_file_atomic(const std::string &path, std::string_view data) { return shcl_write_file_atomic(path.c_str(), data.data(), data.size()) != 0; }
#endif

	// One-shot load-and-validate: parse at a strictness, validate against a
	// schema, and hand back a document whose diagnostics() serve ONE combined
	// list (parse first, then validation). Never fails: error_count() answers
	// "did it fail". An empty schema text skips validation entirely; H001
	// hints the schema disavows (declared repeat upper bound above 1) are
	// dropped.
	static Document load_and_validate(std::string_view text, std::string_view schema, Strictness s) {
		return Document(shcl_load_and_validate(text.data(), text.size(), schema.data(), schema.size(), static_cast<shcl_strictness>(s)));
	}

	// Drop from doc's diagnostics, in place, the hints a schema disavows: H001
	// for a field whose declared repeat upper bound is above 1, H002 for a
	// section marked `reopen: true`. load_and_validate runs both; a parse
	// followed by validate() runs neither.
	static void suppress_declared_repeats(const Document &schema, Document &doc) { shcl_suppress_declared_repeats(schema.d_.get(), doc.d_.get()); }
	static void suppress_declared_reopens(const Document &schema, Document &doc) { shcl_suppress_declared_reopens(schema.d_.get(), doc.d_.get()); }

	// True when a strict load would fail: strict, and an error diagnostic exists.
	bool strict_failed() const { return shcl_strict_failed(d_.get()) != 0; }
	Strictness strictness() const { return static_cast<Strictness>(shcl_strictness_of(d_.get())); }
	// The canonical text lives in the read arena like every other result, so
	// it is released first the way the reads below are: a save loop otherwise
	// holds every copy until the Document goes.
	std::string to_canonical() const { shcl_reads_release(d_.get()); return to_str(shcl_to_canonical(d_.get())); }

	// Diagnostics in emission order: parse-time ones, then repeated-leaf hints.
	std::vector<Diagnostic> diagnostics() const {
		std::vector<Diagnostic> v; std::size_t n = shcl_diag_count(d_.get());
		v.reserve(n);
		for (std::size_t i = 0; i < n; i++)
			v.push_back({shcl_diag_line(d_.get(), i), shcl_diag_severity(d_.get(), i) == SHCL_SEV_ERROR, to_str(shcl_diag_message(d_.get(), i)), shcl_diag_code(d_.get(), i)});
		return v;
	}

	// How many error-severity diagnostics the document carries - the "did
	// this file have errors?" predicate. After load_and_validate, that
	// includes validation errors.
	std::size_t error_count() const { return shcl_error_count(d_.get()); }

	// How many lines or values parsing dropped that canonical output cannot
	// re-emit. Content-malformed lines do NOT count - those survive a save.
	// Nonzero is why save_file refuses; save_file_lossy is the override.
	std::size_t lost_count() const { return shcl_lost_count(d_.get()); }

	// Schema validation (spec.md "Schema validation"): empty result = conforms.
	// Schema faults (V09x, schema-file lines) come first; the surviving
	// constraints still check the document, and the unknown-field sweep skips
	// only when a fault cost a path spelling. The H001/H002 hints a schema
	// disavows are NOT dropped here - they live on the parse's diagnostics,
	// which validation does not touch; load_and_validate is the call that
	// drops them.
	std::vector<Diagnostic> validate(const Document &schema) const {
		std::vector<Diagnostic> v;
		// Owned from the call on, so a throw while copying cannot leak it.
		std::unique_ptr<shcl_validation, void (*)(shcl_validation *)> r(shcl_validate(d_.get(), schema.d_.get()), &shcl_validation_free);
		if (!r) return v;  // an allocation failed; the document is finished
		std::size_t n = shcl_validation_count(r.get());
		v.reserve(n);
		for (std::size_t i = 0; i < n; i++)
			v.push_back({shcl_validation_line(r.get(), i), shcl_validation_severity(r.get(), i) == SHCL_SEV_ERROR, to_str(shcl_validation_message(r.get(), i)), shcl_validation_code(r.get(), i)});
		return v;
	}

	// Layered loading: overlay `over` (a higher-priority layer) onto this doc.
	// Leaf names in `over` override; container instances merge by (name, value).
	// A document merged onto itself is left as it is.
	void merge(const Document &over) { shcl_merge(d_.get(), over.d_.get()); }

	// Give back what repeated writes left behind: the document is rebuilt into
	// fresh storage holding only what it now contains. For a long-running
	// writer; a write-once consumer never needs it.
	void compact() { shcl_compact(d_.get()); }

	// Schema-driven generation (`shcl init`): a commented, typed starter config
	// from this document read as a schema, and whether it succeeded - false on
	// schema faults, with the text then empty; for the fault list, read
	// diagnostics() on this document after the call. Validating an empty
	// document against the schema does not reproduce them: V096 and V097 are
	// generation-only, and what comes back is the empty document's own V002 and
	// V007. A footer naming the format and pointing at the spec is written last
	// unless no_banner. Not const: the faults are recorded as diagnostics on
	// this document, and an earlier call's are dropped first.
	std::pair<std::string, bool> generate(bool no_banner = false) {
		shcl_reads_release(d_.get());
		int ok = 0;
		std::string s = to_str(shcl_generate(d_.get(), no_banner ? 1 : 0, &ok));
		return {std::move(s), ok != 0};
	}

	// Instance count at a path (0 when nothing matches).
	std::size_t count(std::string_view p) const { return shcl_count(d_.get(), p.data(), p.size()); }

	// Each read below hands back the previous one's core memory first: the
	// veneer copies every result into owned std types, so the arena behind it is
	// dead as soon as the copy is made, and a long-lived Document stays flat
	// instead of holding every result until it is destroyed. The one thing to
	// know when mixing APIs: a shcl_str taken from the C core on the same handle
	// does not survive the next veneer read.

	// Quote one path segment for splicing into a lookup path (injection-safe).
	std::string quote_segment(std::string_view name) const { shcl_reads_release(d_.get()); return to_str(shcl_quote_segment(d_.get(), name.data(), name.size())); }

	// Every field path, file order, deduplicated. A segment that is not
	// bare-name-safe comes back quoted, so each path reads back as a lookup.
	std::vector<std::string> paths() const {
		shcl_reads_release(d_.get());
		shcl_str *v; std::size_t n = shcl_paths(d_.get(), &v);
		std::vector<std::string> r; r.reserve(n);
		for (std::size_t i = 0; i < n; i++) r.push_back(to_str(v[i]));
		return r;
	}

	// paths() one instance at a time: every binding's path, with [#i] on each
	// segment whose name its parent repeats, so each path reads one node.
	std::vector<std::string> instance_paths() const {
		shcl_reads_release(d_.get());
		shcl_str *v; std::size_t n = shcl_instance_paths(d_.get(), &v);
		std::vector<std::string> r; r.reserve(n);
		for (std::size_t i = 0; i < n; i++) r.push_back(to_str(v[i]));
		return r;
	}

	// Instance display values at a path, in file order.
	std::vector<std::string> instances(std::string_view p) const {
		shcl_reads_release(d_.get());
		shcl_str *a; std::size_t n = shcl_instances(d_.get(), p.data(), p.size(), &a);
		std::vector<std::string> v; v.reserve(n);
		for (std::size_t i = 0; i < n; i++) v.push_back(to_str(a[i]));
		return v;
	}

	// 1-based source line of the binding at a path; 0 when it does not resolve
	// to exactly one node or the node was writer-built.
	std::size_t line(std::string_view p) const { return shcl_line(d_.get(), p.data(), p.size()); }

	// Whether the single scalar value at a path was quoted in the source, so a
	// quoted plain string is distinguishable from a bare word that happens to
	// spell a reserved one. False for anything that is not one scalar element.
	// A written value counts as quoted when a save would quote it.
	bool quoted(std::string_view p) const { return shcl_quoted(d_.get(), p.data(), p.size()) != 0; }

	// Whether a path resolves to at least one node.
	bool exists(std::string_view p) const { return shcl_exists(d_.get(), p.data(), p.size()) != 0; }

	// The field name at a path exactly as the author spelled it (case
	// unfolded, outer quotes stripped - escape sequences stay as written too,
	// where every other name operation sees them resolved); empty when the path
	// does not resolve to exactly one node.
	std::string authored_name(std::string_view p) const { return to_str(shcl_authored_name(d_.get(), p.data(), p.size())); }

	// The plural line(): 1-based source lines at a path, in file order, so a
	// repeated field - the case that most wants a citable line - yields every
	// binding's. Unresolved wildcard slots stay in the list as 0; a miss is
	// the empty vector.
	std::vector<std::size_t> lines(std::string_view p) const {
		shcl_reads_release(d_.get());
		std::size_t *a; std::size_t n = shcl_lines(d_.get(), p.data(), p.size(), &a);
		std::vector<std::size_t> v; v.reserve(n);
		for (std::size_t i = 0; i < n; i++) v.push_back(a[i]);
		return v;
	}

	// Why a write at a path would fail - the reason behind a setter's bare
	// failure. Probes only; never creates.
	WriteReason write_reason(std::string_view p) const { return static_cast<WriteReason>(shcl_write_reason_(d_.get(), p.data(), p.size())); }

	// Writes. A setter creates the path as needed and returns false when the
	// path is unusable (write_reason() says why) or the value has no spelling
	// the reader accepts: a non-finite float, a datetime the reader would
	// refuse, a raw info string holding a `#`. Nothing is created on false. An
	// ignored false means the save that follows writes a document missing the
	// edit, hence nodiscard. A _default form writes only where nothing is yet,
	// and returns true when something already is. The value it replaced stays
	// in the document's storage until compact().
	[[nodiscard]] bool set_int(std::string_view p, int64_t v) { return shcl_set_int(d_.get(), p.data(), p.size(), v) != 0; }
	[[nodiscard]] bool set_float(std::string_view p, double v) { return shcl_set_float(d_.get(), p.data(), p.size(), v) != 0; }
	[[nodiscard]] bool set_bool(std::string_view p, bool v) { return shcl_set_bool(d_.get(), p.data(), p.size(), v ? 1 : 0) != 0; }
	[[nodiscard]] bool set_string(std::string_view p, std::string_view v) { return shcl_set_string(d_.get(), p.data(), p.size(), v.data(), v.size()) != 0; }
	[[nodiscard]] bool set_datetime(std::string_view p, const Datetime &v) { return shcl_set_datetime(d_.get(), p.data(), p.size(), &v.c()) != 0; }
	// A fence longer than any content line is picked for it. A body line ending
	// in CR fails the write, since a load takes that CR off.
	[[nodiscard]] bool set_raw(std::string_view p, std::string_view content, std::string_view info) { return shcl_set_raw(d_.get(), p.data(), p.size(), content.data(), content.size(), info.data(), info.size()) != 0; }

	// Inline arrays, one per call.
	[[nodiscard]] bool set_int_array(std::string_view p, const std::vector<int64_t> &v) { return shcl_set_int_array(d_.get(), p.data(), p.size(), v.data(), v.size()) != 0; }
	[[nodiscard]] bool set_float_array(std::string_view p, const std::vector<double> &v) { return shcl_set_float_array(d_.get(), p.data(), p.size(), v.data(), v.size()) != 0; }
	[[nodiscard]] bool set_bool_array(std::string_view p, const std::vector<bool> &v) { auto b = bool_args(v); return shcl_set_bool_array(d_.get(), p.data(), p.size(), b.data(), b.size()) != 0; }
	[[nodiscard]] bool set_string_array(std::string_view p, const std::vector<std::string> &v) { auto a = str_args(v); return shcl_set_string_array(d_.get(), p.data(), p.size(), a.p.data(), a.n.data(), v.size()) != 0; }
	[[nodiscard]] bool set_datetime_array(std::string_view p, const std::vector<Datetime> &v) { auto a = dt_args(v); return shcl_set_datetime_array(d_.get(), p.data(), p.size(), a.data(), a.size()) != 0; }

	// Text bound as value syntax rather than as data, so "80, 443" is a
	// two-element array where set_string would store one string. False for text
	// no single line could hold: a line break, or a quote that never closes. A
	// `#` outside quotes ends the value as it would in a file.
	[[nodiscard]] bool set_literal(std::string_view p, std::string_view text) { return shcl_set_literal(d_.get(), p.data(), p.size(), text.data(), text.size()) != 0; }

	// Default (only-if-absent) forms of each setter above.
	[[nodiscard]] bool set_int_default(std::string_view p, int64_t v) { return shcl_set_int_default(d_.get(), p.data(), p.size(), v) != 0; }
	[[nodiscard]] bool set_float_default(std::string_view p, double v) { return shcl_set_float_default(d_.get(), p.data(), p.size(), v) != 0; }
	[[nodiscard]] bool set_bool_default(std::string_view p, bool v) { return shcl_set_bool_default(d_.get(), p.data(), p.size(), v ? 1 : 0) != 0; }
	[[nodiscard]] bool set_string_default(std::string_view p, std::string_view v) { return shcl_set_string_default(d_.get(), p.data(), p.size(), v.data(), v.size()) != 0; }
	[[nodiscard]] bool set_datetime_default(std::string_view p, const Datetime &v) { return shcl_set_datetime_default(d_.get(), p.data(), p.size(), &v.c()) != 0; }
	[[nodiscard]] bool set_literal_default(std::string_view p, std::string_view text) { return shcl_set_literal_default(d_.get(), p.data(), p.size(), text.data(), text.size()) != 0; }
	[[nodiscard]] bool set_raw_default(std::string_view p, std::string_view content, std::string_view info) { return shcl_set_raw_default(d_.get(), p.data(), p.size(), content.data(), content.size(), info.data(), info.size()) != 0; }
	[[nodiscard]] bool set_int_array_default(std::string_view p, const std::vector<int64_t> &v) { return shcl_set_int_array_default(d_.get(), p.data(), p.size(), v.data(), v.size()) != 0; }
	[[nodiscard]] bool set_float_array_default(std::string_view p, const std::vector<double> &v) { return shcl_set_float_array_default(d_.get(), p.data(), p.size(), v.data(), v.size()) != 0; }
	[[nodiscard]] bool set_bool_array_default(std::string_view p, const std::vector<bool> &v) { auto b = bool_args(v); return shcl_set_bool_array_default(d_.get(), p.data(), p.size(), b.data(), b.size()) != 0; }
	[[nodiscard]] bool set_string_array_default(std::string_view p, const std::vector<std::string> &v) { auto a = str_args(v); return shcl_set_string_array_default(d_.get(), p.data(), p.size(), a.p.data(), a.n.data(), v.size()) != 0; }
	[[nodiscard]] bool set_datetime_array_default(std::string_view p, const std::vector<Datetime> &v) { auto a = dt_args(v); return shcl_set_datetime_array_default(d_.get(), p.data(), p.size(), a.data(), a.size()) != 0; }

	// Delete the nodes at a path, subtrees included, and say how many.
	std::size_t remove(std::string_view p) { return shcl_remove(d_.get(), p.data(), p.size()); }
	// A leading comment line on the node at a path, creating an empty node when
	// there is none so a section can be annotated. A missing `#` is added. Text
	// holding a line break is refused, since a comment is one line.
	[[nodiscard]] bool set_comment(std::string_view p, std::string_view text) { return shcl_set_comment(d_.get(), p.data(), p.size(), text.data(), text.size()) != 0; }
	// Take off the comment lines above the nodes at a path, so a comment can be
	// replaced, and say how many came off.
	std::size_t clear_comments(std::string_view p) { return shcl_clear_comments(d_.get(), p.data(), p.size()); }
	// The info block at the end, an old one taken off first; false only takes
	// it off. Says how many old blocks came off.
	std::size_t set_banner(bool on) { return shcl_set_banner(d_.get(), on ? 1 : 0); }
	// An empty value, which is not the empty string.
	[[nodiscard]] bool set_empty(std::string_view p) { return shcl_set_empty(d_.get(), p.data(), p.size()) != 0; }

	// Tokenize one line (sep ':') or one lookup path (path: the bare `*` name
	// wildcard is admitted, and a `#` in a selector body is the [#N] index, not
	// a comment). text is the line after its indent, or the path. out.cap is
	// read and every other field is replaced. A member rather than a free call
	// because the core tokenizes into this document's read memory.
	void tokenize(std::string_view text, char sep, bool path, Rules rules, Tokens &out) const {
		shcl_reads_release(d_.get());
		shcl_tokens t{}; t.cap = out.cap;
		shcl_tokenize(d_.get(), text.data(), text.size(), sep, path ? 1 : 0, static_cast<shcl_rules>(rules), &t);
		copy_tokens(t, out);
	}
	// The value half alone: everything from `from` on, split into pieces, with
	// the comment found on the way.
	void tokenize_value(std::string_view text, std::size_t from, Rules rules, Tokens &out) const {
		shcl_reads_release(d_.get());
		shcl_tokens t{}; t.cap = out.cap;
		shcl_tokenize_value(d_.get(), text.data(), text.size(), from, static_cast<shcl_rules>(rules), &t);
		copy_tokens(t, out);
	}

	// Child field names under a path, file order, duplicates included; "" is
	// the top level, and a path with several instances lists each one's in
	// turn. Names as stored - quote_segment() splices one into a path.
	std::vector<std::string> children(std::string_view p) const {
		shcl_reads_release(d_.get());
		shcl_str *a; std::size_t n = shcl_children(d_.get(), p.data(), p.size(), &a);
		std::vector<std::string> v; v.reserve(n);
		for (std::size_t i = 0; i < n; i++) v.push_back(to_str(a[i]));
		return v;
	}

	// Typed reads: the value and why it is missing or unreadable, if it is.
	Read<int64_t> read_int(std::string_view p) const { auto r = shcl_read_int(d_.get(), p.data(), p.size()); return {r.value, st(r.status)}; }
	Read<double> read_float(std::string_view p) const { auto r = shcl_read_float(d_.get(), p.data(), p.size()); return {r.value, st(r.status)}; }
	Read<bool> read_bool(std::string_view p) const { auto r = shcl_read_bool_(d_.get(), p.data(), p.size()); return {r.value != 0, st(r.status)}; }
	Read<std::string> read_string(std::string_view p) const { shcl_reads_release(d_.get()); auto r = shcl_read_string(d_.get(), p.data(), p.size()); return {to_str(r.value), st(r.status)}; }
	Read<std::string> read_raw(std::string_view p) const { shcl_reads_release(d_.get()); auto r = shcl_read_raw(d_.get(), p.data(), p.size()); return {to_str(r.value), st(r.status)}; }
	Read<std::string> read_raw_info(std::string_view p) const { shcl_reads_release(d_.get()); auto r = shcl_read_raw_info(d_.get(), p.data(), p.size()); return {to_str(r.value), st(r.status)}; }

	// Datetime as the reference's textual form (the common need).
	Read<std::string> read_datetime_str(std::string_view p) const {
		auto r = shcl_read_datetime(d_.get(), p.data(), p.size());
		char buf[SHCL_DT_BUF]; std::size_t k = shcl_datetime_str(&r.value, buf);
		return {std::string(buf, k), st(r.status)};
	}
	// Structured datetime, matching read_datetime in every other binding. Owning
	// (unlike the core's shcl_read_datetime), so it may outlive the Document.
	// This used to be spelled read_datetime_raw, with read_datetime returning
	// the text - backwards twice over, since "raw" means the text exactly as
	// written everywhere else. Both old spellings are gone as of this major.
	Read<Datetime> read_datetime(std::string_view p) const { auto r = shcl_read_datetime(d_.get(), p.data(), p.size()); return {Datetime(r.value), st(r.status)}; }

	// Array reads carry the per-slot statuses in .slots, so a partly-resolved
	// array says which slots failed rather than only that the read did.
	Read<std::vector<int64_t>> read_int_array(std::string_view p) const { shcl_reads_release(d_.get()); auto r = shcl_read_int_array(d_.get(), p.data(), p.size()); return {std::vector<int64_t>(r.values, r.values + r.n), st(r.status), to_slots(r.statuses, r.n)}; }
	Read<std::vector<double>> read_float_array(std::string_view p) const { shcl_reads_release(d_.get()); auto r = shcl_read_float_array(d_.get(), p.data(), p.size()); return {std::vector<double>(r.values, r.values + r.n), st(r.status), to_slots(r.statuses, r.n)}; }
	Read<std::vector<bool>> read_bool_array(std::string_view p) const {
		shcl_reads_release(d_.get());
		auto r = shcl_read_bool_array(d_.get(), p.data(), p.size());
		std::vector<bool> v; v.reserve(r.n); for (std::size_t i = 0; i < r.n; i++) v.push_back(r.values[i] != 0);
		return {std::move(v), st(r.status), to_slots(r.statuses, r.n)};
	}
	Read<std::vector<std::string>> read_string_array(std::string_view p) const {
		shcl_reads_release(d_.get());
		auto r = shcl_read_string_array(d_.get(), p.data(), p.size());
		std::vector<std::string> v; v.reserve(r.n); for (std::size_t i = 0; i < r.n; i++) v.push_back(to_str(r.values[i]));
		return {std::move(v), st(r.status), to_slots(r.statuses, r.n)};
	}
	// Structured, matching read_datetime and every other binding's array read.
	// Owning, so the values may outlive the Document.
	Read<std::vector<Datetime>> read_datetime_array(std::string_view p) const {
		shcl_reads_release(d_.get());
		auto r = shcl_read_datetime_array(d_.get(), p.data(), p.size());
		std::vector<Datetime> v; v.reserve(r.n);
		for (std::size_t i = 0; i < r.n; i++) v.push_back(Datetime(r.values[i]));
		return {std::move(v), st(r.status), to_slots(r.statuses, r.n)};
	}
	// Datetimes as their textual form, matching read_datetime_str.
	Read<std::vector<std::string>> read_datetime_array_str(std::string_view p) const {
		shcl_reads_release(d_.get());
		auto r = shcl_read_datetime_array(d_.get(), p.data(), p.size());
		std::vector<std::string> v; v.reserve(r.n);
		for (std::size_t i = 0; i < r.n; i++) { char buf[SHCL_DT_BUF]; std::size_t k = shcl_datetime_str(&r.values[i], buf); v.push_back(std::string(buf, k)); }
		return {std::move(v), st(r.status), to_slots(r.statuses, r.n)};
	}

	// Compile-time-typed read over int64_t, double, bool, std::string and
	// Datetime, and a std::vector of any of those. Any other T - a bare `int`
	// included - fails right here with this message, not as a bare
	// undefined-symbol link error.
	template <class T> Read<T> get(std::string_view) const {
		static_assert(sizeof(T) == 0,
			"shcl::Document::get<T>: T must be exactly int64_t, double, bool, std::string or shcl::Datetime, or a std::vector of one");
		return {};
	}

	// Convenience tier: the value, or the call-site fallback unless Good - so a
	// missing/empty/bad/ambiguous read cannot masquerade as a real zero. C
	// keeps this tier to the three value types, since its other reads hand back
	// borrowed memory; the veneer copies every result, so it has the full tier
	// the other bindings do. get_or<T> covers every get<T> type.
	template <class T> T get_or(std::string_view p, T def) const {
		auto r = get<T>(p);
		if (r.status == Status::Good) return std::move(r.value);
		return def;
	}
	// A raw block and its info string are strings too, so no T can pick them.
	std::string get_raw_or(std::string_view p, std::string def) const {
		auto r = read_raw(p);
		if (r.status == Status::Good) return std::move(r.value);
		return def;
	}
	std::string get_raw_info_or(std::string_view p, std::string def) const {
		auto r = read_raw_info(p);
		if (r.status == Status::Good) return std::move(r.value);
		return def;
	}
};

template <> inline Read<int64_t> Document::get<int64_t>(std::string_view p) const { return read_int(p); }
template <> inline Read<double> Document::get<double>(std::string_view p) const { return read_float(p); }
template <> inline Read<bool> Document::get<bool>(std::string_view p) const { return read_bool(p); }
template <> inline Read<std::string> Document::get<std::string>(std::string_view p) const { return read_string(p); }
template <> inline Read<Datetime> Document::get<Datetime>(std::string_view p) const { return read_datetime(p); }
template <> inline Read<std::vector<int64_t>> Document::get<std::vector<int64_t>>(std::string_view p) const { return read_int_array(p); }
template <> inline Read<std::vector<double>> Document::get<std::vector<double>>(std::string_view p) const { return read_float_array(p); }
template <> inline Read<std::vector<bool>> Document::get<std::vector<bool>>(std::string_view p) const { return read_bool_array(p); }
template <> inline Read<std::vector<std::string>> Document::get<std::vector<std::string>>(std::string_view p) const { return read_string_array(p); }
template <> inline Read<std::vector<Datetime>> Document::get<std::vector<Datetime>>(std::string_view p) const { return read_datetime_array(p); }

} // namespace shcl

#endif // SHCL_HPP
