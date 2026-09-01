// SPDX-License-Identifier: MIT
// Copyright © 2026 Jim Collier (CryptogID: ѳ6ᴚ℈𐀘𐇦ɛ𐊁¥Mﾏb϶Δ𐌞)

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
	bool ok() const { return status == Status::Good || status == Status::Empty; }
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

class Document {
	// unique_ptr owns the C handle: moves transfer it, copies stay deleted,
	// and destruction frees it - no hand-written rule of five to get wrong.
	struct Free { void operator()(shcl_doc *d) const noexcept { shcl_free(d); } };
	std::unique_ptr<shcl_doc, Free> d_;
	static Status st(shcl_status s) { return static_cast<Status>(s); }
public:
	// An empty document, so a default-constructed Document is usable (every
	// accessor hands the handle to the C core, which takes no null).
	Document() : d_(shcl_parse("", 0)) {}
	explicit Document(shcl_doc *d) : d_(d) {}
	Document(const Document &) = delete;
	Document &operator=(const Document &) = delete;
	Document(Document &&) noexcept = default;
	Document &operator=(Document &&) noexcept = default;

	// False when the parse or load could not allocate and there is no document
	// to work with. True on any system that has not run out of memory, which is
	// most of them; every accessor below assumes a true one.
	explicit operator bool() const { return d_ != nullptr; }

	static Document parse(std::string_view t) { return Document(shcl_parse(t.data(), t.size())); }
	static Document parse_with(std::string_view t, Strictness s) { return Document(shcl_parse_with(t.data(), t.size(), static_cast<shcl_strictness>(s))); }
	// Parse with resource caps (E020 stops the parse past max_nodes, E021
	// refuses a line whose array would exceed max_elements, E022 ends a
	// diagnostics list cut at max_diags with a count of the rest; 0 = no cap).
	static Document parse_limited(std::string_view t, Strictness s, std::size_t max_nodes, std::size_t max_elements, std::size_t max_diags) { return Document(shcl_parse_limited(t.data(), t.size(), static_cast<shcl_strictness>(s), max_nodes, max_elements, max_diags)); }

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

	bool strict_failed() const { return shcl_strict_failed(d_.get()) != 0; }
	Strictness strictness() const { return static_cast<Strictness>(shcl_strictness_of(d_.get())); }
	std::string to_canonical() const { return to_str(shcl_to_canonical(d_.get())); }

	std::vector<Diagnostic> diagnostics() const {
		std::vector<Diagnostic> v; std::size_t n = shcl_diag_count(d_.get());
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
		for (std::size_t i = 0; i < n; i++)
			v.push_back({shcl_validation_line(r.get(), i), shcl_validation_severity(r.get(), i) == SHCL_SEV_ERROR, to_str(shcl_validation_message(r.get(), i)), shcl_validation_code(r.get(), i)});
		return v;
	}

	// Layered loading: overlay `over` (a higher-priority layer) onto this doc.
	// Leaf names in `over` override; container instances merge by (name, value).
	void merge(const Document &over) { shcl_merge(d_.get(), over.d_.get()); }

	// Schema-driven generation (`shcl init`): a commented, typed starter config
	// from this document read as a schema, and whether it succeeded - false on
	// schema faults, with the text then empty; for the fault list, validate()
	// an empty document against this schema - it reproduces the same V09x
	// diagnostics. A footer naming the format and pointing at the spec is
	// written last unless no_banner. Not const: a schema that expands past the
	// generator's field cap records the fault as a diagnostic on this document.
	std::pair<std::string, bool> generate(bool no_banner = false) {
		int ok = 0;
		std::string s = to_str(shcl_generate(d_.get(), no_banner ? 1 : 0, &ok));
		return {std::move(s), ok != 0};
	}

	std::size_t count(std::string_view p) const { return shcl_count(d_.get(), p.data(), p.size()); }
	// Every field path, file order, deduplicated (bare-name-safe segments only).
	// Quote one path segment for splicing into a lookup path (injection-safe).
	// Each read below hands back the previous one's core memory first: the
	// veneer copies every result into owned std types, so the arena behind it is
	// dead as soon as the copy is made, and a long-lived Document stays flat
	// instead of holding every result until it is destroyed. The one thing to
	// know when mixing APIs: a shcl_str taken from the C core on the same handle
	// does not survive the next veneer read.
	std::string quote_segment(std::string_view name) const { shcl_reads_release(d_.get()); return to_str(shcl_quote_segment(d_.get(), name.data(), name.size())); }

	std::vector<std::string> paths() const {
		shcl_reads_release(d_.get());
		shcl_str *v; std::size_t n = shcl_paths(d_.get(), &v);
		std::vector<std::string> r; r.reserve(n);
		for (std::size_t i = 0; i < n; i++) r.push_back(to_str(v[i]));
		return r;
	}

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

	// Child field names under a path, file order, duplicates included; "" is
	// the top level. Names as stored - quote_segment() splices one into a path.
	std::vector<std::string> children(std::string_view p) const {
		shcl_reads_release(d_.get());
		shcl_str *a; std::size_t n = shcl_children(d_.get(), p.data(), p.size(), &a);
		std::vector<std::string> v; v.reserve(n);
		for (std::size_t i = 0; i < n; i++) v.push_back(to_str(a[i]));
		return v;
	}

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
	// Datetimes as their textual form, matching read_datetime_str; the owning
	// Datetime form is per-element and stays on the scalar read.
	Read<std::vector<std::string>> read_datetime_array(std::string_view p) const {
		shcl_reads_release(d_.get());
		auto r = shcl_read_datetime_array(d_.get(), p.data(), p.size());
		std::vector<std::string> v; v.reserve(r.n);
		for (std::size_t i = 0; i < r.n; i++) { char buf[SHCL_DT_BUF]; std::size_t k = shcl_datetime_str(&r.values[i], buf); v.push_back(std::string(buf, k)); }
		return {std::move(v), st(r.status), to_slots(r.statuses, r.n)};
	}

	// Compile-time-typed read: get<int64_t>/get<double>/get<bool>/get<std::string>.
	// Any other T - a bare `int` included - fails right here with this message,
	// not as a bare undefined-symbol link error.
	template <class T> Read<T> get(std::string_view) const {
		static_assert(sizeof(T) == 0,
			"shcl::Document::get<T>: T must be exactly int64_t, double, bool, or std::string");
		return {};
	}

	// Convenience tier: the value, or the call-site fallback unless Good - so a
	// missing/empty/bad/ambiguous read cannot masquerade as a real zero.
	template <class T> T get_or(std::string_view p, T def) const {
		auto r = get<T>(p);
		return r.status == Status::Good ? r.value : def;
	}
};

template <> inline Read<int64_t> Document::get<int64_t>(std::string_view p) const { return read_int(p); }
template <> inline Read<double> Document::get<double>(std::string_view p) const { return read_float(p); }
template <> inline Read<bool> Document::get<bool>(std::string_view p) const { return read_bool(p); }
template <> inline Read<std::string> Document::get<std::string>(std::string_view p) const { return read_string(p); }

} // namespace shcl

#endif // SHCL_HPP
