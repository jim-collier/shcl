// SPDX-License-Identifier: MIT
// Copyright © 2026 Jim Collier (CryptogID: ѳ6ᴚ℈𐀘𐇦ɛ𐊁¥Mﾏb϶Δ𐌞)

// Package shcl is the Go binding of SHCL: parser, accessor, writer/formatter.
// Single file on purpose - the drop-in story is "copy this file into your tree".
// Behavior is pinned to the Rust reference: the conformance corpus in
// project/conformance/ plus the cicd cross-binding differential check keep the
// two byte-for-byte identical, so any divergence here is a bug by definition.
// Structure deliberately mirrors the reference over Go idiom, so a fix there
// ports here by mechanical diff (parity over idiom - see style-guide.md).
//
// Writing a mapper - the shape of a real consumer that walks a document into
// its own model (the surface is 60+ methods, but a mapper needs about six):
//
//	doc := shcl.LoadAndValidate(text, schemaText, shcl.Standard)
//	if doc.ErrorCount() > 0 {
//		for _, d := range doc.Diagnostics() { // one combined list: parse + validation
//			log.Printf("line %d: %s: %s", d.Line, d.Code, d.Message)
//		}
//	}
//	// Iterate instances positionally: Count + [#i]. By-value selectors are
//	// for point lookups; they collapse same-named entities and misread
//	// numeric names, so a mapper walks by index.
//	for i := 0; i < doc.Count("table"); i++ {
//		base := fmt.Sprintf("table[#%d]", i)
//		name := doc.ReadString(base).Value // the discriminator
//		// Open (map-shaped) sections: ask what keys exist, in file order.
//		for _, col := range doc.Children(base + ".columns") {
//			path := base + ".columns." + shcl.QuoteSegment(col) // user-typed names are quoted, never spliced bare
//			r := doc.ReadString(path)
//			if r.Status != shcl.Good {
//				log.Printf("line %d: bad column %q", r.Line, col)
//			}
//			_ = r.Value
//		}
//		_ = name
//	}
//	// Verbatim multi-line content (DDL, templates) lives in fenced raw
//	// blocks: ReadRaw returns it byte-for-byte, ReadRawInfo the fence tag.
package shcl

import (
	"errors"
	"fmt"
	"io"
	"math"
	"os"
	"path/filepath"
	"runtime"
	"sort"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"unicode"
	"unicode/utf8"
)

// ---------------------------------------------------------------------------
// Public surface
// ---------------------------------------------------------------------------

// Strictness is the per-document forgiveness knob. Set once at load.
type Strictness int

const (
	Loose    Strictness = iota // widest read coercions (re-admits currency and friends)
	Standard                   // the default
	Strict                     // any error diagnostic fails the load
)

// StrictnessFromArg accepts the CLI spellings: loose|standard|strict or 1|2|3.
func StrictnessFromArg(s string) (Strictness, bool) {
	switch asciiLower(s) {
	case "loose", "1":
		return Loose, true
	case "standard", "2":
		return Standard, true
	case "strict", "3":
		return Strict, true
	}
	return Standard, false
}

// Severity: only Error fails a strict load; Hint flags legal-but-lookalike input.
type Severity int

const (
	SeverityError Severity = iota // fails a strict load
	SeverityHint                  // legal-but-lookalike input
)

// String is the severity as check prints it.
func (s Severity) String() string {
	if s == SeverityHint {
		return "Hint"
	}
	return "Error"
}

// Diagnostic is one parser or validator finding, tied to a source line.
type Diagnostic struct {
	Line     int // 1-based
	Severity Severity
	Message  string
	// Code is the stable machine code (E001.., H001..) identifying the diagnostic
	// kind - the contract; Message is a free, per-binding voice.
	Code string
}

// Status is the read sentinel. Empty is informational - the empty value is
// still returned.
type Status int

const (
	Good     Status = iota // value present and coercible
	Empty                  // path resolved but carries no value
	NotFound               // path resolved to no node
	BadType                // value would not coerce to the asked type
	Multiple               // path resolved to more than one node
)

// String names the status.
func (s Status) String() string {
	switch s {
	case Good:
		return "Good"
	case Empty:
		return "Empty"
	case NotFound:
		return "NotFound"
	case BadType:
		return "BadType"
	case Multiple:
		return "Multiple"
	}
	return "Good"
}

// WriteReason is why a write would fail (WriteReason()): the distinctions
// behind a setter's bare false. Writable = the path passes the writer's
// validation; the rest name the five ways it cannot.
type WriteReason int

const (
	Writable    WriteReason = iota // the path passes the writer's validation
	BadPath                        // empty path, the scanner rejected it, or a segment carries a line break
	ValueInPath                    // the path carries a `: value` part; writes take values separately
	Wildcard                       // wildcard selectors are query-only
	NoSuchIndex                    // a `[#k]` instance that does not (and can never) exist
	TooDeep                        // deeper than the nesting cap; the writer never creates past it
)

// String names the reason.
func (r WriteReason) String() string {
	switch r {
	case Writable:
		return "Writable"
	case BadPath:
		return "BadPath"
	case ValueInPath:
		return "ValueInPath"
	case Wildcard:
		return "Wildcard"
	case NoSuchIndex:
		return "NoSuchIndex"
	case TooDeep:
		return "TooDeep"
	}
	return "Writable"
}

// Read is the full-tier read result: value plus status plus the original raw
// text (when the path resolved), so a caller can always recover what was
// actually in the file. Array reads also carry one status per slot (element,
// or wildcard instance) in Slots; Status is then the worst slot. Scalar reads
// leave Slots nil.
// Line is the 1-based source line of the resolved binding (0 when the path
// did not resolve to one node, or the node was writer-built), so a consumer
// check the schema cannot express can still cite the line. Quoted is true
// when the read's single scalar element was quoted in the source - the escape
// hatch that lets a downstream language reserve `@null` while `"@null"` stays
// a plain string. Arrays, raw blocks, and empties leave it false.
type Read[T any] struct {
	Value  T
	Status Status
	Raw    *string
	Slots  []Status
	Line   int
	Quoted bool
}

// Ok reports whether the author addressed this field at all: Good or Empty.
// Note this deliberately answers differently from the *Or tier, which falls
// back on Empty like any other non-Good read - Ok asks "is this field spoken
// for", GetIntOr asks "do I have a usable value", and an explicitly emptied
// field is the case where those two diverge.
func (r Read[T]) Ok() bool { return r.Status == Good || r.Status == Empty }

func (r Read[T]) at(line int, quoted bool) Read[T] {
	r.Line = line
	r.Quoted = quoted
	return r
}

// OK is the same test as Ok.
//
// Deprecated: use Ok; OK goes away at the next major.
func (r Read[T]) OK() bool { return r.Ok() }

// LoadError is a failed Strict load: the diagnostics that failed it, plus the
// recovered tree.
type LoadError struct {
	Diagnostics []Diagnostic
	// Document is the tree the parse produced anyway. Recover-and-continue
	// means the diagnostics are the point of a failed strict load, and the
	// tree is what a Standard load would have kept.
	Document *Document
}

// Error summarizes the failure, naming the first few error diagnostics.
func (e *LoadError) Error() string {
	// Name the first few failures right in the message; the bare count made
	// callers dig for information the error was already holding.
	var errs []Diagnostic
	for _, d := range e.Diagnostics {
		if d.Severity == SeverityError {
			errs = append(errs, d)
		}
	}
	msg := fmt.Sprintf("strict load failed: %d error diagnostic(s)", len(errs))
	for i, d := range errs {
		if i == 3 {
			msg += fmt.Sprintf("; +%d more", len(errs)-3)
			break
		}
		msg += fmt.Sprintf("; line %d: %s %s", d.Line, d.Code, d.Message)
	}
	return msg
}

// ZoneKind says how a datetime's zone suffix was written.
type ZoneKind int

const (
	ZoneUTC    ZoneKind = iota // trailing Z
	ZoneOffset                 // +hh:mm / -hh:mm
)

// Zone is a datetime's zone suffix as written.
type Zone struct {
	Kind          ZoneKind
	OffsetMinutes int
}

// DateTime is a local (floating) date/time unless a zone suffix was present.
// Fields mirror what was written: a date-only value has no time, and vice versa.
type DateTime struct {
	HasDate          bool
	Year, Month, Day int
	HasTime          bool
	Hour, Minute     int
	HasSeconds       bool
	Second           int
	Frac             string // fractional-second digits as typed ("" = none)
	Zone             *Zone
}

// String is the canonical spelling, mirroring what was written.
func (dt DateTime) String() string {
	var b strings.Builder
	if dt.HasDate {
		fmt.Fprintf(&b, "%04d-%02d-%02d", dt.Year, dt.Month, dt.Day)
		if dt.HasTime {
			b.WriteByte('T')
		}
	}
	if dt.HasTime {
		fmt.Fprintf(&b, "%02d:%02d", dt.Hour, dt.Minute)
		if dt.HasSeconds {
			fmt.Fprintf(&b, ":%02d", dt.Second)
		}
		if dt.Frac != "" {
			b.WriteByte('.')
			b.WriteString(dt.Frac)
		}
	}
	if dt.Zone != nil {
		if dt.Zone.Kind == ZoneUTC {
			b.WriteByte('Z')
		} else {
			off := dt.Zone.OffsetMinutes
			sign := byte('+')
			if off < 0 {
				sign = '-'
				off = -off
			}
			fmt.Fprintf(&b, "%c%02d:%02d", sign, off/60, off%60)
		}
	}
	return b.String()
}

// FormatFloat renders a float the way the reference does (shortest round-trip
// decimal, never scientific notation) - the cross-binding contract for CLI and
// corpus output.
func FormatFloat(v float64) string {
	if math.IsNaN(v) {
		return "NaN"
	}
	if math.IsInf(v, 1) {
		return "inf"
	}
	if math.IsInf(v, -1) {
		return "-inf"
	}
	return strconv.FormatFloat(v, 'f', -1, 64)
}

// ---------------------------------------------------------------------------
// In-memory model
// ---------------------------------------------------------------------------
// One rule covers everything: a node is (field-name, value, children); nodes
// merge when (name, value) matches; empty values merge into the wrapper node.

type element struct {
	text   string // quote-stripped, escapes NOT applied (applied on string read; names differ - see scanPathEx)
	quoted bool
}

// lead is one whole-line comment held as trivia, plus whether a blank line
// preceded it - so a blank between comment-only regions survives the
// round-trip (blank runs collapse to one, same as nodes).
type lead struct {
	text        string
	blankBefore bool
}

func plainLead(text string) lead {
	return lead{text: text}
}

// pend is a pending whole-line comment during parse: text, source indent (used
// only to decide whether it hangs on a deeper block), and the blank it consumed.
// ceiling is the shortest incoming indent already checked against it: a later
// check can only hang it from a shorter one, so a longer one skips it.
type pend struct {
	text        string
	indent      string
	blankBefore bool
	ceiling     int
}

// pendMark: every pending entry before end has a ceiling at or under indentLen.
type pendMark struct {
	end       int
	indentLen int
}

type valueKind int

const (
	vEmpty valueKind = iota
	vCell            // one element = scalar, more = inline array
	vRaw
)

type rawValue struct {
	content   string
	info      string
	fenceChar byte
	fenceLen  int
}

type value struct {
	kind valueKind
	els  []element
	raw  *rawValue // behind a pointer: inline, its four fields would ride on every node
}

// keyText is the identity spelling of an element's text: escapes resolved, so
// two spellings of one string are one instance. Names have followed that rule
// since 2.0, and a `[value]` selector matches on the resolved text already -
// without this, one selector addressed two instances. The common case has
// nothing to resolve and returns the input.
func keyText(s string) string {
	if !strings.Contains(s, "\\") {
		return s
	}
	return applyEscapes(s)
}

// key is the merge key: nodes with equal (name, key) collapse into one.
func (v *value) key() string {
	switch v.kind {
	case vEmpty:
		return "e"
	case vCell:
		// Length-prefix each element so the joined key is injective: a bare NUL
		// separator lets `[a, b]` collide with the single element "a\0b" (NUL is
		// legal in a quoted string), silently merging them.
		var b strings.Builder
		b.WriteString("c:")
		for _, e := range v.els {
			t := keyText(e.text)
			b.WriteString(strconv.Itoa(len(t)))
			b.WriteByte(':')
			b.WriteString(t)
		}
		return b.String()
	}
	// Info-string is part of identity (a `sql` and a `python` block are
	// different values even with equal bodies); fence style is not. Info is
	// length-prefixed for the same injectivity reason as cell elements.
	return "r:" + strconv.Itoa(len(v.raw.info)) + ":" + v.raw.info + v.raw.content
}

// display is the spelled-out form; also what selectors match against (case-sensitive).
func (v *value) display() string {
	switch v.kind {
	case vEmpty:
		return ""
	case vCell:
		parts := make([]string, len(v.els))
		for i, e := range v.els {
			parts[i] = e.text
		}
		return strings.Join(parts, ", ")
	}
	return v.raw.content
}

func (v *value) isEmpty() bool {
	return v.kind == vEmpty
}

type nodeData struct {
	name      string // ASCII-folded to lower; non-ASCII never folds
	value     value
	children  []int
	parent    int
	line      int
	starList  bool // value built from stacked "* " lines
	starMixed bool // mix of "* " and field children already diagnosed
	// Blank-line grouping is the other half of hand-authored layout: set when
	// a blank line preceded this node's binding line (runs collapse to one).
	blankBefore bool
	// This node has decided its src - set by the first source line whose
	// value the node holds, whether or not a string was worth keeping.
	srcSet bool
	// Comment trivia, kept off to the side: most nodes carry none, and the
	// four empty containers were a third of every node.
	trivia *trivia
	// Verbatim value text from the source line (after the colon, comment
	// stripped, trimmed) - what a read's Raw hands back. nil when the value
	// was synthesized (writer, stacked list, fence) OR when the spelling is
	// exactly the display form; raw falls back to the display form either way.
	src *string
	// The name as the author spelled it (case unfolded, quotes and escapes
	// resolved) - what AuthoredName() hands back, via authored(). Merged
	// instances keep the first binding's spelling, like Line() and comments.
	// Empty = spelled exactly like name (the overwhelmingly common case).
	nameSrc string
}

// trivia is comment trivia, verbatim from `#` to end of line. Never part of
// identity or reads; merged instances concatenate leading, first trailing
// wins (later ones demote to leading - a canonical line has room for one).
type trivia struct {
	leading  []lead
	trailing string // empty = none
	// Whole-line comments that followed this node's subtree at a deeper indent
	// than the next binding - they belong to this block, not the next node, so
	// a run trailing a block's last child stays put instead of re-attaching
	// dedented. Emitted after the subtree at this node's depth.
	after []lead
	// Whole-line comments written inside this node's block when no bound child
	// could take them - a header whose children are all commented still owns
	// those lines. Emitted after the subtree one level deeper than this node.
	inside []lead
}

func (n *nodeData) leading() []lead {
	if n.trivia == nil {
		return nil
	}
	return n.trivia.leading
}

func (n *nodeData) trailing() string {
	if n.trivia == nil {
		return ""
	}
	return n.trivia.trailing
}

func (n *nodeData) after() []lead {
	if n.trivia == nil {
		return nil
	}
	return n.trivia.after
}

func (n *nodeData) inside() []lead {
	if n.trivia == nil {
		return nil
	}
	return n.trivia.inside
}

func (n *nodeData) trivMut() *trivia {
	if n.trivia == nil {
		n.trivia = &trivia{}
	}
	return n.trivia
}

// authored is the as-authored name spelling; empty nameSrc means "same as name".
func (n *nodeData) authored() string {
	if n.nameSrc == "" {
		return n.name
	}
	return n.nameSrc
}

// spelled stores a name's authored spelling: the empty sentinel when it
// matches the folded name, so the duplicate string never gets allocated.
func spelled(name, nameSrc string) string {
	if nameSrc == name {
		return ""
	}
	return nameSrc
}

// srcMatchesDisplay: true when a source value spelling is exactly the display
// form - the case where src need not be stored, since raw's fallback
// reproduces it.
func srcMatchesDisplay(v *value, s string) bool {
	switch v.kind {
	case vEmpty:
		return s == ""
	case vRaw:
		return s == v.raw.content
	}
	rest := s
	for i := range v.els {
		var ok bool
		if i > 0 {
			rest, ok = strings.CutPrefix(rest, ", ")
			if !ok {
				return false
			}
		}
		rest, ok = strings.CutPrefix(rest, v.els[i].text)
		if !ok {
			return false
		}
	}
	return rest == ""
}

// Document is a parsed SHCL document: the tree, its diagnostics, and its
// strictness level.
type Document struct {
	arena      []nodeData
	diags      []Diagnostic
	strictness Strictness
	orphans    []lead // top-level comments after the last binding line
	// Lines or values parsing dropped that canonical output cannot re-emit
	// (bad indentation, an unusable selector, past the depth cap, ...).
	// Content-malformed lines are NOT counted - they are retained as trivia
	// and survive a save. LostCount() serves it; SaveFile() gates on it.
	lost int
	// Built on the first path lookup and kept current by the writer (a new
	// child appends, a removed one unlinks); only a merge drops it. Without it
	// every lookup scans the parent's children, so a flat document read or
	// written key by key was quadratic. Reads may run concurrently, so the
	// build is guarded and the pointer is atomic; a write is exclusive anyway.
	index   atomic.Pointer[nameIndex]
	indexMu sync.Mutex
}

// nameIndex is the first child of each (parent, name), chained on to the next
// same-named sibling, plus the chain tail so an append is O(1). A hash
// collision chains a stranger in; the lookup checks the name, so the chain is
// only ever a superset.
type nameIndex struct {
	first    map[uint64]int
	last     map[uint64]int
	nextSame []int // per node; nilNode ends the chain
}

const nilNode = -1

func (ix *nameIndex) append(key uint64, node int) {
	for len(ix.nextSame) <= node {
		ix.nextSame = append(ix.nextSame, nilNode)
	}
	ix.nextSame[node] = nilNode
	if prev, seen := ix.last[key]; seen {
		ix.nextSame[prev] = node
	} else {
		ix.first[key] = node
	}
	ix.last[key] = node
}

// unlink walks the chain to find the predecessor; a chain is one name's
// siblings.
func (ix *nameIndex) unlink(key uint64, node int) {
	head, hit := ix.first[key]
	if !hit {
		return
	}
	next := ix.nextSame[node]
	if head == node {
		if next == nilNode {
			delete(ix.first, key)
			delete(ix.last, key)
		} else {
			ix.first[key] = next
		}
	} else {
		c := head
		for c != nilNode && ix.nextSame[c] != node {
			c = ix.nextSame[c]
		}
		if c == nilNode {
			return
		}
		ix.nextSame[c] = next
		if next == nilNode {
			ix.last[key] = c
		}
	}
	ix.nextSame[node] = nilNode
}

func nameKey(parent int, name string) uint64 {
	h := newFnv()
	h.dec(parent)
	h.byte(0xFF)
	h.bytes(name)
	return h.h
}

const root = 0

// dead is the stack entry for a binding line that was skipped: it still owns
// its indent level, so the lines written under it are skipped with it instead
// of re-parenting one level up.
const dead = -1

// unopened is the stack entry for a line whose indent matched no open level
// (E012): never a level a sibling can bind at, but deeper lines are still
// under it.
const unopened = -2

// foldNodeInto merges a later instance into an earlier one under the in-file
// merge rule: children and trivia move over, first trailing wins (a second
// demotes to a leading line), first spelling stays. The caller drops the loser
// from the parent's child list; it keeps its arena slot, unreferenced.
func foldNodeInto(arena []nodeData, survivor, loser int) {
	kids := arena[loser].children
	arena[loser].children = nil
	for _, k := range kids {
		arena[k].parent = survivor
	}
	arena[survivor].children = append(arena[survivor].children, kids...)
	if lt := arena[loser].trivia; lt != nil {
		arena[loser].trivia = nil
		st := arena[survivor].trivMut()
		st.leading = append(st.leading, lt.leading...)
		if lt.trailing != "" {
			if st.trailing == "" {
				st.trailing = lt.trailing
			} else {
				st.leading = append(st.leading, plainLead(lt.trailing))
			}
		}
		st.after = append(st.after, lt.after...)
		st.inside = append(st.inside, lt.inside...)
	}
}

// MaxDepth is the maximum nesting depth (levels below the document root),
// enforced at load and by the Writer. Deeper lines are skipped with an E016
// error. The cap is what keeps the recursive tree walks (emit, merge, clone)
// safely inside every binding's stack, so a hostile or machine-generated
// document can make a load fail but never crash the consumer.
const MaxDepth = 512

// ---------------------------------------------------------------------------
// Lexical helpers
// ---------------------------------------------------------------------------

// asciiLower folds A-Z only; non-ASCII passes through untouched. Nearly every
// name is already folded, so the scan comes first: copying and then finding
// nothing to change was the whole cost on the common path.
func asciiLower(s string) string {
	i := 0
	for ; i < len(s); i++ {
		if s[i] >= 'A' && s[i] <= 'Z' {
			break
		}
	}
	if i == len(s) {
		return s
	}
	b := []byte(s)
	for ; i < len(b); i++ {
		if b[i] >= 'A' && b[i] <= 'Z' {
			b[i] += 'a' - 'A'
		}
	}
	return string(b)
}

func isBareNameChar(c rune) bool {
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_'
}

func isASCIIDigit(b byte) bool {
	return b >= '0' && b <= '9'
}

// allDigits reports whether every byte is an ASCII digit (true for "").
func allDigits(s string) bool {
	for i := 0; i < len(s); i++ {
		if !isASCIIDigit(s[i]) {
			return false
		}
	}
	return true
}

func allHexDigits(s string) bool {
	for i := 0; i < len(s); i++ {
		b := s[i]
		if !isASCIIDigit(b) && !(b >= 'a' && b <= 'f') && !(b >= 'A' && b <= 'F') {
			return false
		}
	}
	return true
}

// stripSign removes one leading '+' or '-'.
func stripSign(s string) string {
	if s != "" && (s[0] == '+' || s[0] == '-') {
		return s[1:]
	}
	return s
}

func trimEndWS(s string) string {
	return strings.TrimRightFunc(s, unicode.IsSpace)
}

// leadingWS is the leading space/tab run of a line (the indent).
func leadingWS(s string) string {
	i := 0
	for i < len(s) && (s[i] == ' ' || s[i] == '\t') {
		i++
	}
	return s[:i]
}

// splitComment splits off an unquoted trailing comment: (content, comment from
// `#` on, "" = none). A `\` shields the next char throughout. Comments are
// kept as trivia.
func splitComment(s string) (string, string) {
	if strings.IndexByte(s, '#') < 0 {
		return s, ""
	}
	var inQuote rune
	skip := false
	for i, c := range s {
		if skip {
			skip = false
			continue
		}
		if c == '\\' {
			skip = true
			continue
		}
		switch {
		case inQuote != 0 && c == inQuote:
			inQuote = 0
		case inQuote == 0 && (c == '"' || c == '\''):
			inQuote = c
		case inQuote == 0 && c == '#':
			return s[:i], s[i:]
		}
	}
	return s, ""
}

// splitUnquotedCommas splits on unquoted commas; `\` shields the next char.
func splitUnquotedCommas(s string) []string {
	if strings.IndexByte(s, ',') < 0 {
		return []string{s}
	}
	var parts []string
	var inQuote rune
	skip := false
	start := 0
	for i, c := range s {
		if skip {
			skip = false
			continue
		}
		if c == '\\' {
			skip = true
			continue
		}
		switch {
		case inQuote != 0 && c == inQuote:
			inQuote = 0
		case inQuote == 0 && (c == '"' || c == '\''):
			inQuote = c
		case inQuote == 0 && c == ',':
			parts = append(parts, s[start:i])
			start = i + 1
		}
	}
	return append(parts, s[start:])
}

// normalizeDanglingBackslash: a dangling trailing backslash would swallow the
// separator after it on re-emit; store the doubled spelling instead (identical
// on string read).
func normalizeDanglingBackslash(t string) string {
	run := 0
	for j := len(t) - 1; j >= 0 && t[j] == '\\'; j-- {
		run++
	}
	if run%2 == 1 {
		return t + "\\"
	}
	return t
}

// unterminatedQuote reports whether some piece starts with a quote that never
// closes (missing or escaped). Such a piece stays literal - and the quote-aware
// comment strip has already swallowed any trailing # comment into it - so the
// parser calls it out instead of letting the typo look deliberate. Mid-text
// apostrophes (it's fine) are legal prose and stay silent.
func unterminatedQuote(text string) bool {
	if strings.IndexByte(text, '"') < 0 && strings.IndexByte(text, '\'') < 0 {
		return false
	}
	for _, piece := range splitUnquotedCommas(text) {
		t := strings.TrimSpace(piece)
		if !quotedShape(t) {
			if t == "" {
				continue
			}
			if first := t[0]; first == '"' || first == '\'' {
				return true
			}
		}
	}
	return false
}

// quotedShape is true when the text is one quote pair: a quote char at both
// ends, the last one not escaped. Bytes, not runes: both quotes and the
// backslash are ASCII, and UTF-8 never puts an ASCII byte inside a multibyte
// sequence, so the first byte, the last byte and the escape parity are the
// same answers the decoded form gives.
func quotedShape(t string) bool {
	if t == "" {
		return false
	}
	first := t[0]
	if (first != '"' && first != '\'') || len(t) < 2 || t[len(t)-1] != first {
		return false
	}
	esc := false
	for i := 1; i+1 < len(t); i++ {
		esc = t[i] == '\\' && !esc
	}
	return !esc
}

// parseElement trims, then strips one matching outer quote pair if present.
// Unquoted empty slots return ok=false (dropped, never an error).
func parseElement(piece string) (element, bool) {
	t := strings.TrimSpace(piece)
	if t == "" {
		return element{}, false
	}
	if quotedShape(t) {
		return element{text: t[1 : len(t)-1], quoted: true}, true
	}
	return element{text: normalizeDanglingBackslash(t)}, true
}

// cellExceeds reports whether parseCell would build more than max elements.
// Counts the pieces the way the splitter cuts them, without building any, so
// a capped parse refuses an over-long line before holding the array.
func cellExceeds(text string, max int) bool {
	count := 0
	hasContent := false
	var inQuote rune
	skip := false
	for _, c := range text {
		if skip {
			skip = false
			continue
		}
		if c == '\\' {
			hasContent = true
			skip = true
			continue
		}
		switch {
		case inQuote != 0 && c == inQuote:
			inQuote = 0
		case inQuote == 0 && (c == '"' || c == '\''):
			inQuote = c
		case inQuote == 0 && c == ',':
			if hasContent {
				count++
				if count > max {
					return true
				}
			}
			hasContent = false
			continue
		}
		if !unicode.IsSpace(c) {
			hasContent = true
		}
	}
	if hasContent {
		count++
	}
	return count > max
}

func parseCell(text string) value {
	var els []element
	for _, piece := range splitUnquotedCommas(text) {
		if e, ok := parseElement(piece); ok {
			els = append(els, e)
		}
	}
	if len(els) == 0 {
		return value{kind: vEmpty}
	}
	return value{kind: vCell, els: els}
}

// applyEscapes handles string reads: \t \n \\ \" \'; unknown escapes stay literal.
func applyEscapes(s string) string {
	// Bytes: every escape this recognizes is ASCII, and any other byte - a
	// continuation byte included - is copied through untouched, so the result
	// is the same string the rune walk built without decoding and re-encoding
	// it. This runs on every string read and on every selector compare.
	if !strings.ContainsRune(s, '\\') {
		return s
	}
	out := make([]byte, 0, len(s))
	for i := 0; i < len(s); i++ {
		c := s[i]
		if c != '\\' {
			out = append(out, c)
			continue
		}
		if i+1 >= len(s) {
			out = append(out, '\\')
			break
		}
		i++
		switch s[i] {
		case 't':
			out = append(out, '\t')
		case 'n':
			out = append(out, '\n')
		case '\\':
			out = append(out, '\\')
		case '"':
			out = append(out, '"')
		case '\'':
			out = append(out, '\'')
		default:
			out = append(out, '\\', s[i])
		}
	}
	return string(out)
}

// dispKey is the predicate a [value] selector matches with: display form with
// escapes applied on both sides, so ["q\"uote"] finds 'q"uote' - a
// logical-string match, not spelling against spelling.
// singleScalar is the restriction a QUOTED [value] selector adds on top of
// the display match: quoting selects the scalar spelling only, so the scalar
// "a, b" and the list a, b stop meeting the same selector.
func singleScalar(v *value) bool {
	return v.kind == vCell && len(v.els) == 1
}

func dispKey(v *value) string {
	return applyEscapes(v.display())
}

// fnv is FNV-1a, fed the same byte sequence the key strings would spell - the
// accelerator maps key on a uint64 and verify hits against the arena, so the
// strings themselves never get built. The hash only has to be stable within
// one parse, not injective; a collision just chains in the slot.
type fnv struct {
	h uint64
}

func newFnv() fnv {
	return fnv{h: 0xcbf29ce484222325}
}

func (f *fnv) byte(b byte) {
	f.h = (f.h ^ uint64(b)) * 0x100000001b3
}

func (f *fnv) bytes(s string) {
	for i := 0; i < len(s); i++ {
		f.byte(s[i])
	}
}

// dec spells a length prefix in decimal, without allocating.
func (f *fnv) dec(n int) {
	var buf [20]byte
	i := len(buf)
	for {
		i--
		buf[i] = '0' + byte(n%10)
		n /= 10
		if n == 0 {
			break
		}
	}
	for ; i < len(buf); i++ {
		f.byte(buf[i])
	}
}

// mergeHash hashes the (name, merge-key) pair, spelling what value.key()
// spells without building it.
func mergeHash(name string, v *value) uint64 {
	f := newFnv()
	f.bytes(name)
	f.byte(0xFF) // separator; equality still verifies both parts
	switch v.kind {
	case vEmpty:
		f.byte('e')
	case vCell:
		f.bytes("c:")
		for i := range v.els {
			t := keyText(v.els[i].text)
			f.dec(len(t))
			f.byte(':')
			f.bytes(t)
		}
	default:
		f.bytes("r:")
		f.dec(len(v.raw.info))
		f.byte(':')
		f.bytes(v.raw.info)
		f.bytes(v.raw.content)
	}
	return f.h
}

// valueHash hashes the value's merge key alone (no name part).
func valueHash(v *value) uint64 {
	return mergeHash("", v)
}

// mergeEq is the exact (name, merge-key) equality a hashed hit is verified
// with - compares what the two key strings would hold, element by element.
func mergeEq(nameA string, va *value, nameB string, vb *value) bool {
	if nameA != nameB || va.kind != vb.kind {
		return false
	}
	switch va.kind {
	case vEmpty:
		return true
	case vCell:
		if len(va.els) != len(vb.els) {
			return false
		}
		for i := range va.els {
			if keyText(va.els[i].text) != keyText(vb.els[i].text) {
				return false
			}
		}
		return true
	}
	return va.raw.info == vb.raw.info && va.raw.content == vb.raw.content
}

// escHash is applyEscapes as a streaming feed into the hash - the same state
// machine, one byte at a time, no intermediate string.
type escHash struct {
	f       fnv
	pending bool
}

func (e *escHash) push(b byte) {
	switch {
	case e.pending:
		e.pending = false
		switch b {
		case 't':
			e.f.byte('\t')
		case 'n':
			e.f.byte('\n')
		case '\\':
			e.f.byte('\\')
		case '"':
			e.f.byte('"')
		case '\'':
			e.f.byte('\'')
		default:
			e.f.byte('\\')
			e.f.byte(b)
		}
	case b == '\\':
		e.pending = true
	default:
		e.f.byte(b)
	}
}

func (e *escHash) pushString(s string) {
	for i := 0; i < len(s); i++ {
		e.push(s[i])
	}
}

func (e *escHash) finish() uint64 {
	if e.pending {
		e.f.byte('\\')
	}
	return e.f.h
}

// dispHash hashes the (name, display-with-escapes-applied) pair a `[value]`
// selector matches with - what dispKey would spell, streamed instead of built.
func dispHash(name string, v *value) uint64 {
	f := newFnv()
	f.bytes(name)
	f.byte(0xFF)
	e := escHash{f: f}
	switch v.kind {
	case vEmpty:
	case vCell:
		for i := range v.els {
			if i > 0 {
				e.push(',')
				e.push(' ')
			}
			e.pushString(v.els[i].text)
		}
	default:
		e.pushString(v.raw.content)
	}
	return e.finish()
}

// dispHashText is the query-side twin of dispHash: the selector's text already
// has its escapes applied, so its bytes feed straight in.
func dispHashText(name, want string) uint64 {
	f := newFnv()
	f.bytes(name)
	f.byte(0xFF)
	f.bytes(want)
	return f.h
}

// slot is one accelerator bucket: node indices in insertion order. Nearly
// always one entry; a second means two different keys collided in the 64-bit
// hash.
type slot struct {
	one  int
	many []int // nil = the single entry is `one`
}

func (s *slot) push(idx int) {
	if s.many == nil {
		s.many = []int{s.one, idx}
	} else {
		s.many = append(s.many, idx)
	}
}

func (s *slot) firstMatch(f func(int) bool) (int, bool) {
	if s.many == nil {
		if f(s.one) {
			return s.one, true
		}
		return 0, false
	}
	for _, c := range s.many {
		if f(c) {
			return c, true
		}
	}
	return 0, false
}

// remove drops idx if present; true = the slot is now empty and the caller
// should remove the map entry.
func (s *slot) remove(idx int) bool {
	if s.many == nil {
		return s.one == idx
	}
	kept := s.many[:0]
	for _, c := range s.many {
		if c != idx {
			kept = append(kept, c)
		}
	}
	if len(kept) == 1 {
		s.one = kept[0]
		s.many = nil
		return false
	}
	s.many = kept
	return len(kept) == 0
}

// The bucket helpers below carry the read-modify-write a map entry needs
// (map values are not addressable), so the call sites stay one line each.

// slotInsert chains idx into the bucket at h, insertion order preserved.
func slotInsert(m map[uint64]slot, h uint64, idx int) {
	if s, ok := m[h]; ok {
		s.push(idx)
		m[h] = s
	} else {
		m[h] = slot{one: idx}
	}
}

// slotFirstMatch: the first index in the bucket at h passing the verify
// callback (nil map = no bucket).
func slotFirstMatch(m map[uint64]slot, h uint64, f func(int) bool) (int, bool) {
	if m == nil {
		return 0, false
	}
	s, ok := m[h]
	if !ok {
		return 0, false
	}
	return s.firstMatch(f)
}

// slotRemove drops idx from the bucket at h, deleting the entry when it empties.
func slotRemove(m map[uint64]slot, h uint64, idx int) {
	s, ok := m[h]
	if !ok {
		return
	}
	if s.remove(idx) {
		delete(m, h)
	} else {
		m[h] = s
	}
}

// fenceOpen matches an opening fence: a run of >=3 backticks or tildes, then
// an optional info-string.
func fenceOpen(rest string) (ch byte, length int, info string, ok bool) {
	if rest == "" {
		return 0, 0, "", false
	}
	first := rest[0]
	if first != '`' && first != '~' {
		return 0, 0, "", false
	}
	run := 0
	for run < len(rest) && rest[run] == first {
		run++
	}
	if run < 3 {
		return 0, 0, "", false
	}
	return first, run, strings.TrimSpace(rest[run:]), true
}

// minLen is the opening fence's length, which the grammar puts at three or
// more, so the length test already rules out the empty line the loop below
// would otherwise accept.
func isFenceClose(line string, ch byte, minLen int) bool {
	t := strings.TrimSpace(line)
	if len(t) < minLen {
		return false
	}
	for i := 0; i < len(t); i++ {
		if t[i] != ch {
			return false
		}
	}
	return true
}

// stripCommon removes a raw block's nesting indent from one content line: only
// what the line actually shares with it, so a shallower line (whitespace-only,
// or written flush left) keeps its own spacing rather than being blanked.
func stripCommon(line, common string) string {
	k := 0
	for k < len(common) && k < len(line) && common[k] == line[k] {
		k++
	}
	return line[k:]
}

// ---------------------------------------------------------------------------
// Path scanner (shared by file lines and accessor queries)
// ---------------------------------------------------------------------------

type selKind int

const (
	selByValue selKind = iota
	selByIndex
	selWildcard
)

type selector struct {
	kind  selKind
	value string
	index uint64
	// quoted: the selector text was quoted in the path. A quoted selector is
	// scalar-only - it matches a single-element value whose logical string
	// equals the text - so quoting distinguishes the scalar "a, b" from the
	// two-element list a, b, the same way quoting escapes elsewhere.
	quoted bool
}

type segment struct {
	name    string // folded
	nameSrc string // as authored: unfolded, quotes stripped, escapes applied
	sel     *selector
	star    bool // bare `*` name wildcard; quoted "*" stays a literal name
}

type pathScan struct {
	segments  []segment
	valueText *string // text after the separator colon, trimmed
}

// parseIndex mirrors the reference's unsigned-integer parse: one optional
// leading '+', then ASCII digits, 64-bit range.
func parseIndex(s string) (uint64, bool) {
	t := s
	if t != "" && t[0] == '+' {
		t = t[1:]
	}
	if t == "" || !allDigits(t) {
		return 0, false
	}
	n, err := strconv.ParseUint(t, 10, 64)
	if err != nil {
		return 0, false
	}
	return n, true
}

// scanPath scans `a . b : [sel] . c : value`. Whitespace around dots/colons/
// brackets is insignificant. A colon is a selector colon only when the next
// non-ws char is `[`; otherwise it separates the value. An error means
// genuinely ambiguous input, which the caller skips with a diagnostic.
// looksLikeBracketArray: a value spelled the way JSON, TOML and YAML spell an
// array. The path scanner reads the brackets as a selector, so the line arrives
// with no value text and the old repair blamed a colon that is plainly there.
func looksLikeBracketArray(content string) bool {
	colon := strings.IndexByte(content, ':')
	if colon < 0 {
		return false
	}
	rest := strings.TrimSpace(content[colon+1:])
	return strings.HasPrefix(rest, "[") && strings.HasSuffix(rest, "]")
}

func scanPath(input string) (pathScan, error) {
	return scanPathEx(input, false)
}

// scanLookup is the query spelling of scanPath: also accepts a bare `*`
// segment (the name wildcard - any child name). Document lines never take it;
// only lookups (reads, the writer probe, schema paths) do.
func scanLookup(input string) (pathScan, error) {
	return scanPathEx(input, true)
}

func scanPathEx(input string, stars bool) (pathScan, error) {
	// Byte cursor with inline rune decoding (a []rune per call was a parse hot
	// spot). Every position the scanner stops on is a rune boundary: it only
	// byte-matches ASCII structure chars, which UTF-8 guarantees cannot appear
	// inside a multibyte sequence, and otherwise advances by whole runes.
	// Backslash still shields the next RUNE, multibyte included.
	pos := 0
	skipWS := func() {
		for pos < len(input) && (input[pos] == ' ' || input[pos] == '\t') {
			pos++
		}
	}
	// A span rebuilt rune-by-rune matches the old []rune round-trip exactly:
	// invalid UTF-8 bytes each become U+FFFD, valid text passes through. The
	// valid (overwhelmingly common) case slices instead of copying.
	spanString := func(s string) string {
		if utf8.ValidString(s) {
			return s
		}
		var out []rune
		for _, c := range s {
			out = append(out, c)
		}
		return string(out)
	}
	readQuoted := func() (string, error) {
		q := rune(input[pos]) // caller checked: ASCII quote
		pos++
		var out strings.Builder
		for {
			if pos >= len(input) {
				return "", errors.New("unterminated quote")
			}
			c, cw := utf8.DecodeRuneInString(input[pos:])
			if c == '\\' && pos+1 < len(input) {
				next, nw := utf8.DecodeRuneInString(input[pos+1:])
				out.WriteRune(c)
				out.WriteRune(next)
				pos += 1 + nw
				continue
			}
			pos += cw
			if c == q {
				return out.String(), nil
			}
			out.WriteRune(c)
		}
	}
	var segments []segment
	for {
		skipWS()
		if pos >= len(input) {
			return pathScan{}, errors.New("empty path")
		}
		// Field name: quoted, bare, or (lookups only) the `*` name wildcard.
		var name string
		star := false
		if input[pos] == '"' || input[pos] == '\'' {
			n, err := readQuoted()
			if err != nil {
				return pathScan{}, err
			}
			name = n
		} else if stars && input[pos] == '*' {
			pos++
			star = true
			name = "*"
		} else {
			start := pos
			// Bare-name chars are ASCII, so the byte-as-rune view is exact
			// (bytes >= 0x80 map to runes the predicate rejects either way).
			for pos < len(input) && isBareNameChar(rune(input[pos])) {
				pos++
			}
			if pos == start {
				c, _ := utf8.DecodeRuneInString(input[pos:])
				return pathScan{}, fmt.Errorf("expected field name, found '%c'", c)
			}
			name = input[start:pos]
		}
		var sel *selector
		skipWS()
		// Optional selector, with its optional sugar colon (colon counts as
		// selector sugar only when the next non-ws char is an open bracket).
		bracketAt := -1
		if pos < len(input) && input[pos] == '[' {
			bracketAt = pos
		} else if pos < len(input) && input[pos] == ':' {
			q := pos + 1
			for q < len(input) && (input[q] == ' ' || input[q] == '\t') {
				q++
			}
			if q < len(input) && input[q] == '[' {
				bracketAt = q
			}
		}
		if bracketAt >= 0 {
			pos = bracketAt + 1
			skipWS()
			if pos < len(input) && (input[pos] == '"' || input[pos] == '\'') {
				v, err := readQuoted()
				if err != nil {
					return pathScan{}, err
				}
				// quotes force a value match, even numeric - and scalar-only
				sel = &selector{kind: selByValue, value: v, quoted: true}
			} else {
				start := pos
				for pos < len(input) && input[pos] != ']' {
					pos++
				}
				body := strings.TrimSpace(spanString(input[start:pos]))
				if body == "*" {
					sel = &selector{kind: selWildcard}
				} else if n, ok := hashIndex(body); ok {
					sel = &selector{kind: selByIndex, index: n}
				} else if n, ok := parseIndex(body); ok {
					sel = &selector{kind: selByIndex, index: n}
				} else if body == "" {
					return pathScan{}, errors.New("empty selector")
				} else {
					sel = &selector{kind: selByValue, value: normalizeDanglingBackslash(body)}
				}
			}
			skipWS()
			if pos >= len(input) || input[pos] != ']' {
				return pathScan{}, errors.New("unterminated selector")
			}
			pos++
			skipWS()
		}
		if star && sel != nil {
			return pathScan{}, errors.New("selector on a name wildcard")
		}
		// Names resolve escapes, the same rule values follow when they are
		// compared: two spellings of one name are one name. nameSrc keeps the
		// source spelling, which is what AuthoredName hands back.
		segments = append(segments, segment{name: asciiLower(applyEscapes(name)), nameSrc: name, sel: sel, star: star})
		if pos >= len(input) {
			return pathScan{segments: segments}, nil
		}
		switch input[pos] {
		case '.':
			pos++
		case ':':
			pos++
			rest := strings.TrimSpace(spanString(input[pos:]))
			return pathScan{segments: segments, valueText: &rest}, nil
		default:
			c, _ := utf8.DecodeRuneInString(input[pos:])
			return pathScan{}, fmt.Errorf("unexpected '%c' after field", c)
		}
	}
}

func hashIndex(body string) (uint64, bool) {
	if !strings.HasPrefix(body, "#") {
		return 0, false
	}
	return parseIndex(body[1:])
}

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------

type stackEnt struct {
	indent string
	node   int
}

type parser struct {
	arena []nodeData
	diags []Diagnostic
	// (indent, node) for each open level; [0] is the virtual root.
	stack []stackEnt
	// Per-node hash-of-(name, value-key) -> matching children, parallel to
	// arena and nil until a parent's first insert, so leaves never allocate
	// one. Pure lookup accelerator for selectOrCreate; children keeps the
	// order. No key strings are stored - a hit is verified against the arena
	// with mergeEq.
	childMap []map[uint64]slot
	// Per-node hash-of-(name, display) -> first matching child: the `[value]`
	// selector accelerator (its predicate is display(), a different and
	// non-injective key from childMap's). Same first-wins discipline, same
	// mutation sites; ownership is by hash, and a query verifies its hit.
	dispMap []map[uint64]int
	// Whole-line comments waiting for the next line that binds a node. The
	// source indent is kept only to decide after-attachment (a comment deeper
	// than the next binding hangs on the block it sits in).
	pending []pend
	// Lengths rise along the stack, so a hang check pops the marks above its
	// own indent and walks only what they covered. Without it a run of
	// retained bad lines is rewalked per line and a plain text file parses in
	// quadratic time.
	pendMarks []pendMark
	sawBlank  bool // a blank line waits to become the next bound node's blankBefore
	// An open stacked list defers its merge-key remap (rebuilding the key per
	// element is O(list^2) time); (node, key hash, display hash) at deferral
	// start, flushed before any map lookup and at end of parse.
	starOpen bool
	starNode int
	starKey  uint64
	starDisp uint64
	// Node -> line of the re-open that H002-hinted it. A merge under a hinted
	// container combines the same two textual regions, so it hints too even
	// when it lands on the newest child at its own scope - that is how every
	// merged level reports, not just the outermost. The stored line splits old
	// children (hint) from ones the re-opened region itself created (silent).
	reentered map[int]int
	lost      int // dropped lines/values canonical output cannot re-emit
	// ParseLimited's caps, 0 = uncapped: nodes counted against the arena
	// (root excluded), elements against a single value's cell, diagnostics
	// against the list. Past the diagnostic cap nothing is listed, only
	// counted (errors, hints), for the one tail entry the parse ends with.
	maxNodes       int
	maxElements    int
	maxDiags       int
	unlistedErrors int
	unlistedHints  int
}

func newParser() *parser {
	return &parser{
		arena:     []nodeData{{}},
		stack:     []stackEnt{{}},
		childMap:  []map[uint64]slot{nil},
		dispMap:   []map[uint64]int{nil},
		reentered: map[int]int{},
	}
}

func (p *parser) err(line int, code, msg string) {
	p.diag(Diagnostic{Line: line, Severity: SeverityError, Message: msg, Code: code})
}

// diag: every parse diagnostic goes through here, so the cap sees them all.
func (p *parser) diag(d Diagnostic) {
	if p.maxDiags != 0 && len(p.diags) >= p.maxDiags {
		if d.Severity == SeverityError {
			p.unlistedErrors++
		} else {
			p.unlistedHints++
		}
		return
	}
	p.diags = append(p.diags, d)
}

// selectOrCreate finds (or creates by merge rule) the child of parent with
// this (name, value).
func (p *parser) selectOrCreate(parent int, name, nameSrc string, v value, line int) int {
	p.starFlush()
	h := mergeHash(name, &v)
	if c, ok := slotFirstMatch(p.childMap[parent], h, func(c int) bool {
		return mergeEq(p.arena[c].name, &p.arena[c].value, name, &v)
	}); ok {
		return c
	}
	idx := len(p.arena)
	hd := dispHash(name, &v)
	p.arena = append(p.arena, nodeData{name: name, nameSrc: spelled(name, nameSrc), value: v, parent: parent, line: line})
	p.arena[parent].children = append(p.arena[parent].children, idx)
	p.childMap = append(p.childMap, nil)
	p.dispMap = append(p.dispMap, nil)
	if p.childMap[parent] == nil {
		p.childMap[parent] = map[uint64]slot{}
	}
	slotInsert(p.childMap[parent], h, idx)
	if p.dispMap[parent] == nil {
		p.dispMap[parent] = map[uint64]int{}
	}
	if _, ok := p.dispMap[parent][hd]; !ok {
		p.dispMap[parent][hd] = idx
	}
	return idx
}

// starFlush applies an open stacked list's deferred remap. Runs before any map
// lookup (and at end of parse), so both maps are always fresh when queried.
func (p *parser) starFlush() {
	if p.starOpen {
		p.starOpen = false
		p.remapChild(p.starNode, p.starKey, p.starDisp)
	}
}

// remapChild: a node's value mutated in place (empty field filled, star element
// added): move its map entry from the old key to the new one. First-wins on
// both sides so lookups keep matching the earliest sibling, like the scan did.
func (p *parser) remapChild(node int, oldKey, oldDisp uint64) {
	parent := p.arena[node].parent
	if p.childMap[parent] != nil {
		slotRemove(p.childMap[parent], oldKey, node)
	}
	newKey := mergeHash(p.arena[node].name, &p.arena[node].value)
	_, already := slotFirstMatch(p.childMap[parent], newKey, func(c int) bool {
		return mergeEq(p.arena[c].name, &p.arena[c].value, p.arena[node].name, &p.arena[node].value)
	})
	if !already {
		if p.childMap[parent] == nil {
			p.childMap[parent] = map[uint64]slot{}
		}
		slotInsert(p.childMap[parent], newKey, node)
	}
	if m := p.dispMap[parent]; m != nil {
		if c, ok := m[oldDisp]; ok && c == node {
			delete(m, oldDisp)
		}
	}
	newDisp := dispHash(p.arena[node].name, &p.arena[node].value)
	if p.dispMap[parent] == nil {
		p.dispMap[parent] = map[uint64]int{}
	}
	if _, ok := p.dispMap[parent][newDisp]; !ok {
		p.dispMap[parent][newDisp] = node
	}
}

// foldLateDups: a value that mutates after its sibling group was keyed - an
// empty field filled by a fence, a stacked list closed - can land on a key an
// earlier sibling already holds, which the keyed lookup can no longer catch.
// Fold those pairs so the tree matches a reparse of its own canonical text.
// Depth-first, since folding can carry duplicates down a level.
func (p *parser) foldLateDups() {
	stack := []int{root}
	for len(stack) > 0 {
		parent := stack[len(stack)-1]
		stack = stack[:len(stack)-1]
		kids := p.arena[parent].children
		p.arena[parent].children = nil
		first := map[uint64]slot{}
		keep := make([]int, 0, len(kids))
		for _, c := range kids {
			h := mergeHash(p.arena[c].name, &p.arena[c].value)
			if survivor, ok := slotFirstMatch(first, h, func(x int) bool {
				return mergeEq(p.arena[x].name, &p.arena[x].value, p.arena[c].name, &p.arena[c].value)
			}); ok {
				foldNodeInto(p.arena, survivor, c)
			} else {
				slotInsert(first, h, c)
				keep = append(keep, c)
			}
		}
		stack = append(stack, keep...)
		p.arena[parent].children = keep
	}
}

// attachTrivia hands pending leading comments (and this line's trailing one)
// to a node. First trailing wins; a later one demotes to leading so nothing
// is lost.
func (p *parser) attachTrivia(node int, trailing string) {
	if len(p.pending) > 0 {
		t := p.arena[node].trivMut()
		for _, pn := range p.pending {
			t.leading = append(t.leading, lead{text: pn.text, blankBefore: pn.blankBefore})
		}
		p.pending = p.pending[:0]
		p.pendMarks = p.pendMarks[:0]
	}
	if trailing != "" {
		t := p.arena[node].trivMut()
		if t.trailing == "" {
			t.trailing = trailing
		} else {
			t.leading = append(t.leading, plainLead(trailing))
		}
	}
}

// hangDeeperPending: comments written deeper than the incoming line belong to
// the block they sit in, not to the next binding: hang each on the deepest node
// whose bound indent prefixes the comment's, among the levels the incoming
// line is closing. Written at that node's own level the comment trails it
// (after); written deeper it sits inside the node's block (inside) - so a
// header whose children are all commented still owns them at their depth. Runs
// before the incoming line resolves (and at end of parse with the empty
// indent, so tail comments keep their block).
func (p *parser) hangDeeperPending(newIndent string) {
	if len(p.pending) == 0 {
		return
	}
	newLen := len(newIndent)
	// Only entries above the last mark at or under this indent can hang.
	for len(p.pendMarks) > 0 && p.pendMarks[len(p.pendMarks)-1].indentLen > newLen {
		p.pendMarks = p.pendMarks[:len(p.pendMarks)-1]
	}
	start := 0
	if len(p.pendMarks) > 0 {
		start = p.pendMarks[len(p.pendMarks)-1].end
	}
	taken := append([]pend(nil), p.pending[start:]...)
	p.pending = p.pending[:start]
	for _, pn := range taken {
		if pn.ceiling > newLen {
			// A level shallower than the incoming line stays open and may
			// still gain children, so a comment must not hang there - it
			// would emit below the child; keep it pending instead.
			target := -1
			atOwnLevel := false
			for j := len(p.stack) - 1; j >= 0; j-- {
				ent := p.stack[j]
				if ent.node != root && ent.node != dead && ent.node != unopened && len(ent.indent) >= len(newIndent) &&
					strings.HasPrefix(pn.indent, ent.indent) {
					target = ent.node
					atOwnLevel = len(ent.indent) == len(pn.indent)
					break
				}
			}
			if target >= 0 {
				l := lead{text: pn.text, blankBefore: pn.blankBefore}
				t := p.arena[target].trivMut()
				if atOwnLevel {
					t.after = append(t.after, l)
				} else {
					t.inside = append(t.inside, l)
				}
				continue
			}
			pn.ceiling = newLen
		}
		p.pending = append(p.pending, pn)
	}
	if n := len(p.pendMarks); n > 0 && p.pendMarks[n-1].indentLen == newLen {
		p.pendMarks[n-1].end = len(p.pending)
	} else {
		p.pendMarks = append(p.pendMarks, pendMark{end: len(p.pending), indentLen: newLen})
	}
}

// resolveParent resolves which open level this indent belongs to. Child only
// when the current top's indent is a proper prefix; otherwise the indent must
// equal an open level exactly (dedent), else it is a recoverable error.
func (p *parser) resolveParent(indent string) (int, bool) {
	top := p.stack[len(p.stack)-1]
	if len(indent) > len(top.indent) && strings.HasPrefix(indent, top.indent) {
		if top.node == unopened {
			return dead, true
		}
		return top.node, true
	}
	for i := len(p.stack) - 1; i >= 0; i-- {
		if p.stack[i].indent == indent && p.stack[i].node != unopened {
			// Sibling of stack[i]: its parent is the entry below it. Keep the
			// sentinel; a top-level line resolves to root.
			parent := root
			if i > 0 {
				parent = p.stack[i-1].node
			}
			keep := i
			if keep < 1 {
				keep = 1
			}
			p.stack = p.stack[:keep]
			if parent == unopened {
				return dead, true
			}
			return parent, true
		}
	}
	// Skipped, but it still owns its indent: whatever is written deeper is
	// skipped with it, and a sibling at the same bad indent is refused the
	// same way instead of binding one level up.
	for len(p.stack) > 1 {
		topIndent := p.stack[len(p.stack)-1].indent
		if len(indent) > len(topIndent) && strings.HasPrefix(indent, topIndent) {
			break
		}
		p.stack = p.stack[:len(p.stack)-1]
	}
	p.stack = append(p.stack, stackEnt{indent: indent, node: unopened})
	return 0, false
}

// skipUnderDead diagnoses a line written under a skipped line, and skips it
// too. Its own level stays dead so deeper lines go the same way.
func (p *parser) skipUnderDead(line int, indent string) {
	p.err(line, "E018", "parent line was skipped; line skipped")
	p.lost++
	p.stack = append(p.stack, stackEnt{indent: indent, node: dead})
}

// attachPath walks path segments under parent, select-or-creating; returns the
// node for the last segment carrying v. ok=false aborts the line (diagnosed).
func (p *parser) attachPath(parent int, segs []segment, v value, line int) (int, bool) {
	p.starFlush()
	// Field child under a stacked list: diagnose the mix once, keep the field.
	if p.arena[parent].starList && !p.arena[parent].starMixed {
		p.arena[parent].starMixed = true
		p.err(line, "E001", "field mixed with list elements")
	}
	// Nesting cap: parent depth plus the segments this line adds. Checked
	// before any node is created so a rejected line leaves nothing behind.
	parentDepth := 0
	for up := parent; up != root; up = p.arena[up].parent {
		parentDepth++
	}
	if parentDepth+len(segs) > MaxDepth {
		p.err(line, "E016", fmt.Sprintf("nesting deeper than %d levels; line skipped", MaxDepth))
		p.lost++
		return 0, false
	}
	cur := parent
	for i := range segs {
		seg := &segs[i]
		isLast := i+1 == len(segs)
		switch {
		case seg.sel != nil && seg.sel.kind == selByValue:
			// Same escape-applied display predicate resolution uses, so a
			// selector also selects an array-valued instance instead of creating
			// a spurious second one - via the dispMap accelerator (the inline
			// spelling was quadratic in siblings without it). Create only when
			// nothing matches.
			// A quoted selector is scalar-only, so it is the one that needs the
			// fallback scan: the accelerator keeps just the first same-display
			// child, which may be the non-scalar one. An unquoted selector takes
			// whatever the accelerator holds and does not scan, so it can bind a
			// raw block where a quoted selector picks the scalar sibling.
			if found, ok := p.findByValue(cur, seg.name, seg.sel.value, seg.sel.quoted); ok {
				cur = found
			} else {
				disc := value{kind: vCell, els: []element{{text: seg.sel.value}}}
				cur = p.selectOrCreate(cur, seg.name, seg.nameSrc, disc, line)
			}
			if isLast && !v.isEmpty() {
				// `a.b[X]: v` - the discriminator is the value; a second
				// value has nowhere unambiguous to go.
				p.err(line, "E002", fmt.Sprintf("value after selector on '%s' ignored", seg.name))
				p.lost++
			}
		case seg.sel != nil && seg.sel.kind == selByIndex:
			found, ok := 0, false
			var nth uint64
			for _, c := range p.arena[cur].children {
				if p.arena[c].name != seg.name {
					continue
				}
				if nth == seg.sel.index {
					found, ok = c, true
					break
				}
				nth++
			}
			if ok {
				cur = found
			} else {
				p.err(line, "E003", fmt.Sprintf("no instance %d of '%s'", seg.sel.index, seg.name))
				p.lost++
				return 0, false
			}
			if isLast && !v.isEmpty() {
				// Same as the value selector: the instance is already chosen,
				// so a trailing value has nowhere to bind.
				p.err(line, "E002", fmt.Sprintf("value after selector on '%s' ignored", seg.name))
				p.lost++
			}
		case seg.sel != nil:
			p.err(line, "E004", "wildcard selector is query-only")
			p.lost++
			return 0, false
		case !isLast:
			cur = p.selectOrCreate(cur, seg.name, seg.nameSrc, value{kind: vEmpty}, line)
		default:
			parent := cur
			before := len(p.arena)
			cur = p.selectOrCreate(cur, seg.name, seg.nameSrc, v, line)
			// Two separately-written bindings just combined: legal (the
			// merge rule), but only the parser can see it happened, so
			// say so. Adjacent re-mentions (still the newest binding at
			// this scope) and selector/path-intermediate merges stay
			// silent - those are the deliberate redundant-path idiom.
			// Under a hinted container the newest-child pass does not
			// apply to children the earlier region wrote: those merges
			// combine the same two regions, so every level reports.
			if cur < before && p.arena[cur].line != line {
				kids := p.arena[parent].children
				nonLast := len(kids) == 0 || kids[len(kids)-1] != cur
				rl, ok := p.reentered[parent]
				crossRegion := ok && p.arena[cur].line < rl
				if nonLast || crossRegion {
					p.diag(Diagnostic{
						Line:     line,
						Severity: SeverityHint,
						Message:  fmt.Sprintf("%sline %d (same name and value combine)", h002Head(seg.name), p.arena[cur].line),
						Code:     "H002",
					})
					p.reentered[cur] = line
				}
			}
		}
	}
	return cur, true
}

// findByValue: the child of cur named name whose display form is the selector
// text (escapes applied), or ok=false. Quoted selectors only match a single
// scalar.
func (p *parser) findByValue(cur int, name, text string, quoted bool) (int, bool) {
	want := applyEscapes(text)
	found, ok := 0, false
	if m := p.dispMap[cur]; m != nil {
		if c, hit := m[dispHashText(name, want)]; hit && p.arena[c].name == name && dispKey(&p.arena[c].value) == want {
			found, ok = c, true
		}
	}
	if ok && quoted && !singleScalar(&p.arena[found].value) {
		ok = false
	}
	if !ok && quoted {
		for _, c := range p.arena[cur].children {
			if p.arena[c].name == name && singleScalar(&p.arena[c].value) && dispKey(&p.arena[c].value) == want {
				found, ok = c, true
				break
			}
		}
	}
	return found, ok
}

// consumeRaw consumes raw-block content after an opening fence. Returns the
// value and the next line index. The closing fence's indent is stripped from
// each content line (the opening line's when the block never closes); the
// rest is content.
func (p *parser) consumeRaw(
	lines []string, i, openLine int, openIndent string, ch byte, length int, info string,
) (value, int) {
	var content []string
	nest := openIndent
	closed := false
	for i < len(lines) {
		if isFenceClose(lines[i], ch, length) {
			// The closing fence's indent is the nesting; everything a content
			// line carries past it is content, so a body whose lines all
			// share an indent keeps it (a writer-built block depends on that).
			nest = leadingWS(lines[i])
			closed = true
			i++
			break
		}
		content = append(content, lines[i])
		i++
	}
	if !closed {
		p.err(openLine, "E005", "unterminated raw block")
	}
	stripped := make([]string, len(content))
	for j, l := range content {
		stripped[j] = stripCommon(l, nest)
	}
	return value{
		kind: vRaw,
		raw:  &rawValue{content: strings.Join(stripped, "\n"), info: info, fenceChar: ch, fenceLen: length},
	}, i
}

// bindBlock: a bare fence line is a value line for its parent field: fills an
// empty value, else creates a new instance of that field (the repeated-leaf
// rule). Returns the node the block landed on (-1 = no parent, diagnosed).
func (p *parser) bindBlock(parent int, v value, line int) int {
	if parent == root {
		p.err(line, "E006", "raw block with no parent field")
		p.lost++
		return -1
	}
	if p.arena[parent].value.isEmpty() {
		oldKey := mergeHash(p.arena[parent].name, &p.arena[parent].value)
		oldDisp := dispHash(p.arena[parent].name, &p.arena[parent].value)
		p.arena[parent].value = v
		p.remapChild(parent, oldKey, oldDisp)
		return parent
	}
	name, nameSrc, grand := p.arena[parent].name, p.arena[parent].authored(), p.arena[parent].parent
	return p.selectOrCreate(grand, name, nameSrc, v, line)
}

// addStarElement: one stacked-list element (`* scalar`) appends to the
// parent's array.
func (p *parser) addStarElement(parent int, body string, line int) {
	if parent == root {
		p.err(line, "E007", "list element with no parent field")
		p.lost++
		return
	}
	// Uniform-or-nothing (spec): a mix with field children is not a block array.
	if len(p.arena[parent].children) != 0 {
		p.err(line, "E008", "list element mixed with field children; ignored")
		p.lost++
		return
	}
	trimmed := strings.TrimSpace(body)
	if trimmed == "" {
		p.err(line, "E009", "empty list element")
		p.lost++
		return
	}
	// One scalar per line; a bare comma is an error, not a second element.
	if len(splitUnquotedCommas(trimmed)) > 1 {
		p.err(line, "E010", "bare comma in list element (one element per line)")
		p.lost++
		return
	}
	if unterminatedQuote(trimmed) {
		p.err(line, "E017", "unterminated quote in value")
	}
	el, ok := parseElement(trimmed)
	if !ok {
		p.err(line, "E009", "empty list element")
		p.lost++
		return
	}
	// Element cap: each element line past it is refused on its own, the way
	// any other bad element line is.
	if p.maxElements != 0 && p.arena[parent].value.kind == vCell && len(p.arena[parent].value.els) >= p.maxElements {
		p.err(line, "E021", fmt.Sprintf("array longer than %d elements; line skipped", p.maxElements))
		p.lost++
		return
	}
	switch {
	case p.arena[parent].value.isEmpty():
		oldKey := mergeHash(p.arena[parent].name, &p.arena[parent].value)
		oldDisp := dispHash(p.arena[parent].name, &p.arena[parent].value)
		p.arena[parent].value = value{kind: vCell, els: []element{el}}
		p.arena[parent].starList = true
		// First element: remap now (Empty -> cell changes both keys), then
		// open the deferral window with the current keys. Rebuilding the
		// keys per appended element was O(list^2) time; the maps only need
		// to be fresh when queried, and every query flushes first.
		p.remapChild(parent, oldKey, oldDisp)
		p.starOpen = true
		p.starNode = parent
		p.starKey = mergeHash(p.arena[parent].name, &p.arena[parent].value)
		p.starDisp = dispHash(p.arena[parent].name, &p.arena[parent].value)
	case p.arena[parent].value.kind == vCell && p.arena[parent].starList:
		if !(p.starOpen && p.starNode == parent) {
			p.starFlush()
			p.starOpen = true
			p.starNode = parent
			p.starKey = mergeHash(p.arena[parent].name, &p.arena[parent].value)
			p.starDisp = dispHash(p.arena[parent].name, &p.arena[parent].value)
		}
		p.arena[parent].value.els = append(p.arena[parent].value.els, el)
	default:
		p.err(line, "E011", "field already has a value; list element ignored")
		p.lost++
	}
}

// emitRepeatedLeafHints flags legal input that looks like a common mistake: a
// field repeating as a bare scalar leaf. Mandatory hint per spec (never fails
// a load). Groups are in first-appearance order so hint order is deterministic
// across bindings.
func (p *parser) emitRepeatedLeafHints() {
	type group struct {
		name  string
		nodes []int
	}
	for parent := range p.arena {
		var byName []group
		groupOf := make(map[string]int)
		for _, c := range p.arena[parent].children {
			name := p.arena[c].name
			if g, ok := groupOf[name]; ok {
				byName[g].nodes = append(byName[g].nodes, c)
			} else {
				groupOf[name] = len(byName)
				byName = append(byName, group{name: name, nodes: []int{c}})
			}
		}
		for _, g := range byName {
			if len(g.nodes) < 2 {
				continue
			}
			allScalarLeaves := true
			for _, c := range g.nodes {
				n := &p.arena[c]
				if len(n.children) != 0 || n.value.kind != vCell || n.starList {
					allScalarLeaves = false
					break
				}
			}
			if !allScalarLeaves {
				continue
			}
			line := 0
			vals := make([]string, 0, len(g.nodes))
			for _, c := range g.nodes {
				if p.arena[c].line > line {
					line = p.arena[c].line
				}
				vals = append(vals, p.arena[c].value.display())
			}
			p.diag(Diagnostic{
				Line:     line,
				Severity: SeverityHint,
				Message:  fmt.Sprintf("%s%s'?", h001Head(g.name), strings.Join(vals, ", ")),
				Code:     "H001",
			})
		}
	}
}

func (p *parser) parse(text string, strictness Strictness) *Document {
	// UTF-8 BOM strip, then split keeping raw lines (CR stripped per line).
	// The whole trailing CR run goes, not just one: a raw block keeps its content
	// untrimmed, so a line left ending in CR would be written back as CRLF and
	// read as neither - the one shape where the count is visible.
	text = strings.TrimPrefix(text, "\uFEFF")
	lines := strings.Split(text, "\n")
	for j, l := range lines {
		lines[j] = strings.TrimRight(l, "\r")
	}
	i := 0
	nodeCapped := false
	for i < len(lines) {
		// Node cap: reported at the first line not parsed, so the count can
		// overshoot by at most one line's path. The unparsed remainder counts
		// as lost, which is what keeps SaveFile from writing a silently
		// truncated document.
		if p.maxNodes != 0 && len(p.arena)-1 > p.maxNodes {
			p.err(i+1, "E020", fmt.Sprintf("node cap of %d exceeded; parse stopped", p.maxNodes))
			for _, l := range lines[i:] {
				if strings.TrimSpace(l) != "" {
					p.lost++
				}
			}
			nodeCapped = true
			break
		}
		lineno := i + 1
		line := trimEndWS(lines[i])
		indent := leadingWS(line)
		rest := line[len(indent):]
		if rest == "" {
			p.sawBlank = true
			i++
			continue
		}
		// Whole-line comment: hold it for the next line that binds a node.
		// It consumes a pending blank into its own flag, so a blank between
		// comment-only regions survives the round-trip.
		if strings.HasPrefix(rest, "#") {
			p.pending = append(p.pending, pend{text: rest, indent: indent, blankBefore: p.sawBlank, ceiling: len(indent)})
			p.sawBlank = false
			i++
			continue
		}
		// Any other line consumes the pending blank; only a field line that
		// binds turns it into grouping.
		hadBlank := p.sawBlank
		p.sawBlank = false
		// A binding line claims the pending comments - but deeper-written
		// ones hang on their own block first.
		p.hangDeeperPending(indent)
		// Child-indent fence: a value line for its parent field.
		if ch, length, info, ok := fenceOpen(rest); ok {
			parent, okp := p.resolveParent(indent)
			v, next := p.consumeRaw(lines, i+1, lineno, indent, ch, length, info)
			if !okp {
				// The body goes with its fence: parsed live, it would read as
				// root bindings and the closing fence would open a second block.
				p.err(lineno, "E012", "indentation matches no open level")
				p.lost++
				i = next
				continue
			}
			if parent == dead {
				p.skipUnderDead(lineno, indent)
			} else if node := p.bindBlock(parent, v, lineno); node >= 0 {
				p.attachTrivia(node, "")
			}
			i = next
			continue
		}
		// Stacked-list element: colon-less by construction ('*' can't begin a name).
		if strings.HasPrefix(rest, "*") {
			after := rest[1:]
			if strings.HasPrefix(after, " ") || strings.HasPrefix(after, "\t") {
				parent, okp := p.resolveParent(indent)
				if !okp {
					p.err(lineno, "E012", "indentation matches no open level")
					p.lost++
					i++
					continue
				}
				if parent == dead {
					p.skipUnderDead(lineno, indent)
					i++
					continue
				}
				body, comment := splitComment(after)
				// Elements have no node of their own; trivia rides the field.
				if parent != root {
					p.attachTrivia(parent, comment)
				}
				p.addStarElement(parent, body, lineno)
				i++
				continue
			}
			parent, okp := p.resolveParent(indent)
			if !okp {
				p.err(lineno, "E012", "indentation matches no open level")
				p.lost++
				i++
				continue
			}
			if parent == dead {
				p.skipUnderDead(lineno, indent)
				i++
				continue
			}
			p.err(lineno, "E013", "malformed line: '*' must be followed by a space")
			// Content-malformed at any position, so it is safe to retain
			// verbatim as trivia: re-emitted, it re-diagnoses identically
			// and can never read as a live binding. A hand-typo no longer
			// vanishes on the consumer's next save. The BOM exception the
			// sibling site below carries cannot apply here: this line starts
			// with the '*' that brought us in.
			p.pending = append(p.pending, pend{text: trimEndWS(rest), indent: indent, blankBefore: hadBlank, ceiling: len(indent)})
			p.stack = append(p.stack, stackEnt{indent: indent, node: dead})
			i++
			continue
		}
		// Field line.
		before, comment := splitComment(rest)
		// A same-line fence runs to the end of the line: the child-indent
		// spelling keeps a `#` in its info-string, the grammar gives the
		// same-line alternative no comment at all, and the emitter already
		// assumes it. Without this, `a: ```c#` loses the `#`. The cheap test
		// comes first so an ordinary commented line is not scanned twice.
		if comment != "" && (strings.Contains(before, "```") || strings.Contains(before, "~~~")) {
			if sc, err := scanPath(trimEndWS(before)); err == nil && sc.valueText != nil {
				if _, _, _, okf := fenceOpen(*sc.valueText); okf {
					before, comment = rest, ""
				}
			}
		}
		content := trimEndWS(before)
		if content == "" {
			// Only a comment survived (e.g. an escaped lead-in); keep it.
			if comment != "" {
				p.pending = append(p.pending, pend{text: comment, indent: indent, blankBefore: hadBlank, ceiling: len(indent)})
			}
			i++
			continue
		}
		parent, okp := p.resolveParent(indent)
		if !okp {
			p.err(lineno, "E012", "indentation matches no open level")
			p.lost++
			i++
			continue
		}
		if parent == dead {
			p.skipUnderDead(lineno, indent)
			i++
			continue
		}
		scan, serr := scanPath(content)
		if serr != nil {
			p.err(lineno, "E014", "malformed line skipped: "+serr.Error())
			// Content-malformed at any position - retained as trivia, same
			// rationale (and same BOM exception) as the bad '*' line above.
			if strings.HasPrefix(rest, "\ufeff") {
				p.lost++
			} else {
				p.pending = append(p.pending, pend{text: trimEndWS(rest), indent: indent, blankBefore: hadBlank, ceiling: len(indent)})
			}
			p.stack = append(p.stack, stackEnt{indent: indent, node: dead})
			i++
			continue
		}
		next := i + 1
		// The verbatim value span, kept for reads' Raw (only the plain
		// scalar/inline-array case has a one-line source spelling).
		srcText, haveSrc := "", false
		var v value
		switch {
		case scan.valueText == nil:
			if looksLikeBracketArray(content) {
				// The brackets never survive the load, so a rewrite would bake
				// the changed value in and the file would check clean forever
				// after. Count it lost so the save gate stops that.
				p.err(lineno, "E019", "bracket array syntax; an array is comma-separated, without brackets")
				p.lost++
			} else {
				// A clean path with no colon is the one defined repair:
				// the obvious intent is that path with an empty value.
				p.err(lineno, "E015", "missing colon; repaired as an empty value")
			}
			v = value{kind: vEmpty}
		case *scan.valueText == "":
			v = value{kind: vEmpty}
		default:
			if ch, length, info, ok := fenceOpen(*scan.valueText); ok {
				// Same-line fence spelling.
				v, next = p.consumeRaw(lines, i+1, lineno, indent, ch, length, info)
			} else {
				// Element cap: the whole line is refused, so a capped load
				// never holds a truncated array that would read as the
				// document's value. Counted before anything splits the value
				// (the quote check does too), or the cap would bound nothing.
				if p.maxElements != 0 && cellExceeds(*scan.valueText, p.maxElements) {
					p.err(lineno, "E021", fmt.Sprintf("array longer than %d elements; line skipped", p.maxElements))
					p.lost++
					p.stack = append(p.stack, stackEnt{indent: indent, node: dead})
					i = next
					continue
				}
				if unterminatedQuote(*scan.valueText) {
					p.err(lineno, "E017", "unterminated quote in value")
				}
				srcText, haveSrc = *scan.valueText, true
				v = parseCell(*scan.valueText)
			}
		}
		// Record only when the bound node holds exactly this line's value
		// (a merge into an equal-valued node keeps the first line's span;
		// a value dropped after a last-segment selector records nothing).
		var vkey uint64
		if haveSrc {
			vkey = valueHash(&v)
		}
		if node, ok := p.attachPath(parent, scan.segments, v, lineno); ok {
			if haveSrc && !p.arena[node].srcSet && valueHash(&p.arena[node].value) == vkey {
				p.arena[node].srcSet = true
				if !srcMatchesDisplay(&p.arena[node].value, srcText) {
					s := srcText
					p.arena[node].src = &s
				}
			}
			if hadBlank {
				p.arena[node].blankBefore = true
			}
			p.attachTrivia(node, comment)
			p.stack = append(p.stack, stackEnt{indent: indent, node: node})
		} else {
			p.stack = append(p.stack, stackEnt{indent: indent, node: dead})
		}
		i = next
	}
	// A cap crossed on the document's last line still reports, with nothing
	// left to skip.
	if !nodeCapped && p.maxNodes != 0 && len(p.arena)-1 > p.maxNodes {
		p.err(len(lines), "E020", fmt.Sprintf("node cap of %d exceeded; parse stopped", p.maxNodes))
	}
	p.starFlush()
	p.foldLateDups()
	p.emitRepeatedLeafHints()
	// Indented tail comments keep their block; only top-level ones orphan.
	p.hangDeeperPending("")
	orphans := make([]lead, 0, len(p.pending))
	for _, pn := range p.pending {
		orphans = append(orphans, lead{text: pn.text, blankBefore: pn.blankBefore})
	}
	p.pending = p.pending[:0]
	// The emitter drops a blank before the first thing it prints, so a document
	// that kept one there would not survive its own canonical form:
	// load(emit(load(x))) and load(x) would differ on that bit, and a merge -
	// where the line is no longer first - would place a blank the author never
	// wrote. Clear it here, once, wherever output starts.
	if kids := p.arena[root].children; len(kids) > 0 {
		n := &p.arena[kids[0]]
		if n.trivia != nil && len(n.trivia.leading) > 0 {
			n.trivia.leading[0].blankBefore = false
		} else {
			n.blankBefore = false
		}
	} else if len(orphans) > 0 {
		orphans[0].blankBefore = false
	}
	// The one entry past the cap: what was not listed, and whether any of it
	// was an error, so a consumer scanning the list for errors still finds
	// one and a Strict load still fails.
	if more := p.unlistedErrors + p.unlistedHints; more > 0 {
		sev := SeverityHint
		if p.unlistedErrors > 0 {
			sev = SeverityError
		}
		p.diags = append(p.diags, Diagnostic{
			Line:     0, // about the list, not a line
			Severity: sev,
			Code:     "E022",
			Message:  fmt.Sprintf("diagnostic cap of %d reached; %d more not listed, %d of them errors", p.maxDiags, more, p.unlistedErrors),
		})
	}
	return &Document{arena: p.arena, diags: p.diags, strictness: strictness, orphans: orphans, lost: p.lost}
}

// ---------------------------------------------------------------------------
// Document: load, diagnostics, formatter
// ---------------------------------------------------------------------------

// Parse parses at Standard strictness. Never fails: bad lines are skipped and
// diagnosed, good values stay readable.
func Parse(text string) *Document {
	return newParser().parse(text, Standard)
}

// ParseWith parses at a chosen strictness. Only Strict can fail (any error
// diagnostic). The Document comes back even then - non-nil alongside the
// error, with the error carrying it too - so `doc, err :=` callers can
// inspect doc.Diagnostics() without a nil check blowing up.
func ParseWith(text string, strictness Strictness) (*Document, error) {
	doc := newParser().parse(text, strictness)
	if strictness == Strict {
		for _, d := range doc.diags {
			if d.Severity == SeverityError {
				return doc, &LoadError{Diagnostics: append([]Diagnostic(nil), doc.diags...), Document: doc}
			}
		}
	}
	return doc, nil
}

// ParseLimited parses with resource caps beside the strictness, for input the
// consumer does not control: a document amplifies to many times its byte size
// in memory, so a size cap alone cannot bound what a load allocates. maxNodes
// stops the parse once a line takes the node count past it - one E020 error,
// and the unparsed remainder counts as lost so a save cannot silently
// truncate. maxElements refuses any line whose array would hold more elements
// (E021, that line alone is skipped). maxDiags bounds the diagnostics list
// itself - a document of nothing but bad lines costs a diagnostic per line -
// by listing the first that many and ending with one E022 that counts the
// rest; ErrorCount and the Strict gate see that tail as an error whenever an
// unlisted one was. 0 disables a cap, making this ParseWith. The caps are
// parse-time only; the write API is the consumer's own arithmetic. As
// everywhere, the error fires only at Strict, and a cap diagnostic is an
// error, so a capped Strict load fails; at other levels the parsed part stays
// readable.
func ParseLimited(text string, strictness Strictness, maxNodes, maxElements, maxDiags int) (*Document, error) {
	p := newParser()
	p.maxNodes = maxNodes
	p.maxElements = maxElements
	p.maxDiags = maxDiags
	doc := p.parse(text, strictness)
	if strictness == Strict {
		for _, d := range doc.diags {
			if d.Severity == SeverityError {
				return doc, &LoadError{Diagnostics: append([]Diagnostic(nil), doc.diags...), Document: doc}
			}
		}
	}
	return doc, nil
}

// Diagnostics is everything the load recorded (after LoadAndValidate,
// validation findings too).
func (d *Document) Diagnostics() []Diagnostic {
	// A copy: the reference hands out a borrowed view nobody can append to,
	// and a Go slice sharing the document's backing array would let a
	// caller's append and the document's next one overwrite each other.
	return append([]Diagnostic(nil), d.diags...)
}

// LostCount is how many lines or values parsing dropped that canonical
// output cannot re-emit - bad indentation, an unusable selector, a line past
// the depth cap. Content-malformed lines do NOT count: those are retained as
// trivia and survive a save. Nonzero means a SaveFile would delete
// hand-written content, so SaveFile refuses then (SaveFileLossy overrides).
func (d *Document) LostCount() int {
	return d.lost
}

// ErrorCount is how many error-severity diagnostics the document carries - the
// "did this file have errors?" predicate, so recover-and-continue can't read
// as success by accident. Counts whatever Diagnostics() holds (after
// LoadAndValidate, that includes validation errors).
func (d *Document) ErrorCount() int {
	n := 0
	for _, dg := range d.diags {
		if dg.Severity == SeverityError {
			n++
		}
	}
	return n
}

// LoadAndValidate is the one-shot load-and-validate: parse at a strictness,
// validate against a schema, and hand back the document carrying ONE combined
// diagnostics list (parse first, then validation - the order `check --schema`
// prints), so half the errors can't vanish because a caller forgot one of the
// two lists. Never fails: a strict-failing document comes back as the document
// plus its diagnostics (ErrorCount answers "did it fail"). An empty schema
// text skips validation entirely. H001 hints the schema disavows (a declared
// repeat upper bound above 1) are dropped.
func LoadAndValidate(text, schemaText string, strictness Strictness) *Document {
	doc := newParser().parse(text, strictness)
	if strings.TrimSpace(schemaText) != "" {
		schema := Parse(schemaText)
		// A schema that did not load would silently drop the constraints on
		// its broken lines, or report every field as unknown - either way
		// blaming the document for the schema. Say so instead, as `check`
		// does, and validate nothing.
		for _, sd := range schema.diags {
			if sd.Severity == SeverityError {
				doc.diags = append(doc.diags, Diagnostic{
					Line: 0, Severity: SeverityError, Message: "schema failed to load", Code: "V099",
				})
				return doc
			}
		}
		doc.diags = append(doc.diags, doc.Validate(schema)...)
		doc.diags = SuppressDeclaredRepeats(schema, doc.diags)
		doc.diags = SuppressDeclaredReopens(schema, doc.diags)
	}
	return doc
}

// Strictness is the level the document was loaded at.
func (d *Document) Strictness() Strictness {
	return d.strictness
}

// ToCanonical emits the canonical form: block layout, tabs, insertion order,
// minimal quoting, redundancy collapsed, comments re-emitted as attached
// trivia. Scalar text is never rewritten.
func (d *Document) ToCanonical() string {
	var out strings.Builder
	out.Grow(len(d.arena) * 24)
	d.emitChildren(d.arena[root].children, 0, &out)
	// Comments that never found a following line re-emit at the end.
	for _, c := range d.orphans {
		if c.blankBefore && out.Len() > 0 {
			out.WriteByte('\n')
		}
		out.WriteString(c.text)
		out.WriteByte('\n')
	}
	return out.String()
}

// writeTrailing writes an inline comment, canonically two spaces before the `#`.
func writeTrailing(out *strings.Builder, trailing string) {
	if trailing != "" {
		out.WriteString("  ")
		out.WriteString(trailing)
	}
}

// emitChildren emits a sibling run. The parent walk already knows whether an
// earlier same-name sibling is empty (the raw same-line-fence hazard), so one
// seen-empties set here replaces a per-child rescan of the whole run.
func (d *Document) emitChildren(kids []int, depth int, out *strings.Builder) {
	empties := map[string]bool{}
	for _, c := range kids {
		n := &d.arena[c]
		wm := n.value.kind == vRaw && empties[n.name]
		if n.value.isEmpty() {
			empties[n.name] = true
		}
		d.emitNode(c, depth, wm, out)
	}
}

func (d *Document) emitNode(idx, depth int, wouldMerge bool, out *strings.Builder) {
	node := &d.arena[idx]
	pad := strings.Repeat("\t", depth)
	// Same-line fence spelling can't carry an inline comment (an unbalanced
	// quote in the info-string could hide the `#` on reparse), so its trailing
	// comment joins the leading lines instead; the flag comes from the parent's
	// walk. Each blank rides its own comment (or the binding line), never as
	// the first output line.
	for _, c := range node.leading() {
		if c.blankBefore && out.Len() > 0 {
			out.WriteByte('\n')
		}
		out.WriteString(pad)
		out.WriteString(c.text)
		out.WriteByte('\n')
	}
	if node.blankBefore && out.Len() > 0 {
		out.WriteByte('\n')
	}
	if wouldMerge && node.trailing() != "" {
		out.WriteString(pad)
		out.WriteString(node.trailing())
		out.WriteByte('\n')
	}
	out.WriteString(pad)
	out.WriteString(emitName(node.name))
	out.WriteByte(':')
	switch node.value.kind {
	case vEmpty:
		writeTrailing(out, node.trailing())
		out.WriteByte('\n')
	case vCell:
		out.WriteByte(' ')
		for k := range node.value.els {
			if k > 0 {
				out.WriteString(", ")
			}
			out.WriteString(emitElement(&node.value.els[k]))
		}
		writeTrailing(out, node.trailing())
		out.WriteByte('\n')
	default:
		// Child-indent spelling is canonical: bare name line, fenced block one
		// level deeper, verbatim content. Exception: if an earlier same-name
		// sibling is empty, the bare `name:` header would merge into it on
		// reparse and the fence would fill that instance instead - so use the
		// same-line spelling there.
		r := node.value.raw
		if wouldMerge {
			out.WriteByte(' ')
		} else {
			writeTrailing(out, node.trailing())
			out.WriteByte('\n')
		}
		bodyPad := pad + "\t"
		fence := strings.Repeat(string(rune(r.fenceChar)), r.fenceLen)
		if !wouldMerge {
			out.WriteString(bodyPad)
		}
		out.WriteString(fence)
		if r.info != "" {
			// An info-string starting with the fence char would extend the run
			// on reparse; a space keeps the fence length intact.
			if r.info[0] == r.fenceChar {
				out.WriteByte(' ')
			}
			out.WriteString(r.info)
		}
		out.WriteByte('\n')
		if r.content != "" {
			for _, l := range strings.Split(r.content, "\n") {
				if l != "" {
					out.WriteString(bodyPad)
				}
				out.WriteString(l)
				out.WriteByte('\n')
			}
		}
		out.WriteString(bodyPad)
		out.WriteString(fence)
		out.WriteByte('\n')
	}
	d.emitChildren(d.arena[idx].children, depth+1, out)
	// Comments this block owns with no child to carry them, one deeper.
	ipad := pad + "\t"
	for _, c := range d.arena[idx].inside() {
		if c.blankBefore && out.Len() > 0 {
			out.WriteByte('\n')
		}
		out.WriteString(ipad)
		out.WriteString(c.text)
		out.WriteByte('\n')
	}
	// Comments that hung on this block after its last child.
	for _, c := range d.arena[idx].after() {
		if c.blankBefore && out.Len() > 0 {
			out.WriteByte('\n')
		}
		out.WriteString(pad)
		out.WriteString(c.text)
		out.WriteByte('\n')
	}
}

// escapeName emits a stored (escape-resolved) name in a spelling that reads
// back as the same name: bare when it can be, else quoted with the escapes
// applyEscapes undoes. This is a true inverse of the name parse, which quoteText
// is not - that one picks a quote style to AVOID escaping and never escapes a
// backslash, which is right for a value (stored in its escaped spelling) and
// wrong for a name (stored resolved).
func escapeName(name string) string {
	if name != "" {
		bare := true
		for _, c := range name {
			if !isBareNameChar(c) {
				bare = false
				break
			}
		}
		if bare {
			return name
		}
	}
	var out strings.Builder
	out.WriteByte('"')
	for i := 0; i < len(name); i++ {
		switch name[i] {
		case '\\':
			out.WriteString(`\\`)
		case '"':
			out.WriteString(`\"`)
		case '\t':
			out.WriteString(`\t`)
		case '\n':
			out.WriteString(`\n`)
		default:
			out.WriteByte(name[i])
		}
	}
	out.WriteByte('"')
	return out.String()
}

func emitName(name string) string {
	return escapeName(name)
}

// QuoteSegment quotes one path segment so it can be spliced into a lookup
// path: a bare name passes through, anything else comes back quoted and
// escaped in the form the path scanner accepts. Splicing user-typed text into
// a path without this is path injection - a dotted name silently reads as
// nesting. Same spelling Paths and the canonical emitter produce.
func QuoteSegment(name string) string {
	return emitName(name)
}

// h001Head is the single H001 wording site: the hint builder and the schema
// suppressor both come here, so the suppressor matches the exact head the
// builder emitted - never a re-parse of free prose. (The leaf name cannot
// ride on Diagnostic itself: consumers build Diagnostic literals, so its
// field set is frozen.)
func h001Head(name string) string {
	return fmt.Sprintf("'%s' repeats as a bare leaf - did you mean '%s: ", name, name)
}

// SuppressDeclaredRepeats drops the H001 hints a schema disavows: a field
// whose declared repeat upper bound is above 1 repeats BY DESIGN (repetition
// is its instance mechanism), so the repeated-bare-leaf hint is structurally a
// false positive there and trains users to ignore hints. Matching is by leaf
// name - the filter consumers were hand-rolling - which errs toward quiet, for
// a hint. Used by `check --schema` and LoadAndValidate; call it wherever doc
// diagnostics and a schema meet. Returns the filtered slice as a fresh
// allocation and never disturbs the input (the reference filters its list in
// place behind &mut; a Go return reads as a copy, so it must behave as one).
func SuppressDeclaredRepeats(schema *Document, diags []Diagnostic) []Diagnostic {
	names := disavowedNames(schema, func(c *constraint) bool { return c.repeat != nil && c.repeat[1] > 1 })
	if len(names) == 0 {
		return append([]Diagnostic(nil), diags...)
	}
	heads := make([]string, len(names))
	for i, n := range names {
		heads[i] = h001Head(n)
	}
	// Filter into a fresh slice: the input commonly IS the document's own
	// diagnostics list, so filtering in place would corrupt the caller's data.
	kept := make([]Diagnostic, 0, len(diags))
	for _, d := range diags {
		if d.Code == "H001" {
			drop := false
			for _, h := range heads {
				if strings.HasPrefix(d.Message, h) {
					drop = true
					break
				}
			}
			if drop {
				continue
			}
		}
		kept = append(kept, d)
	}
	return kept
}

// ReadFile is the file tier's read half on its own: the text of path with
// FileClean, or "" with the status that says why not - FileNotFound, or
// FileUnreadable for everything else (permissions, a directory, bad encoding,
// or a file past maxBytes; 0 is no cap). LoadFile is this plus a parse. A
// consumer that needs the exact bytes it last saw - to tell its own save coming
// back as a change notification from somebody else's edit - or a bound on how
// much it will read before a parse, calls this and parses the text itself.
func ReadFile(path string, maxBytes int) (string, FileStatus) {
	f, err := os.Open(path)
	if err != nil {
		if os.IsNotExist(err) {
			return "", FileNotFound
		}
		return "", FileUnreadable
	}
	defer f.Close()
	// One byte past the cap is read, so a file exactly at it passes and one
	// over is caught without trusting a length from Stat.
	var r io.Reader = f
	if maxBytes > 0 {
		// Saturating: a cap spelled as the type maximum must not wrap to a
		// negative limit and read nothing.
		limit := int64(maxBytes)
		if limit < math.MaxInt64 {
			limit++
		}
		r = io.LimitReader(f, limit)
	}
	data, err := io.ReadAll(r)
	if err != nil || (maxBytes > 0 && len(data) > maxBytes) {
		return "", FileUnreadable
	}
	// Reading succeeds on any bytes here, unlike the reference's read-to-string
	// and python's decoding open - so bad encoding needs its own test, or a
	// binary file loads Clean, reads back mangled, and a later save writes the
	// mangled version over the original.
	if !utf8.Valid(data) {
		return "", FileUnreadable
	}
	return string(data), FileClean
}

// WriteFileAtomic is the file tier's write mechanism (also what the CLI's
// `--write` uses): a temp file in the same dir, then a rename over the
// target, so an interrupted write can never truncate the config it rewrites.
// The data is synced before the rename so a crash cannot publish an empty file.
//
// A rename publishes a new inode, so the target is resolved through symlinks
// first (otherwise a linked-in config gets replaced by a regular file and the
// real one is left stale) and the original's mode is copied onto the temp file
// (otherwise a 600 config comes back at whatever the umask allows). Other hard
// links to the old inode cannot survive a rename and keep the old content.
// The i/o failures wrap rather than flatten, so a caller can tell a permission
// failure from a full disk with errors.Is instead of matching on prose.
func WriteFileAtomic(file, data string) error {
	target, terr := resolveTarget(file)
	if terr != nil {
		return fmt.Errorf("%s: %w", file, terr)
	}
	dir := filepath.Dir(target)
	base := filepath.Base(target)
	// Exclusive create: the name is predictable, so anything already sitting
	// there - including a symlink someone else planted - must make this fail
	// rather than be written through. Retry past a stale collision, then give
	// up; refusing to write beats writing somewhere unintended.
	// A file that already exists keeps its own mode, so its temp is born private
	// and the real mode goes on below - the copy is never briefly readable to
	// anyone the original was not. A file that does not exist yet has no mode to
	// preserve, so it takes the one an ordinary create would: 0666 narrowed by
	// the umask, like every other file the user's tools produce.
	existing, existErr := os.Stat(target)
	born := os.FileMode(0o600)
	if existErr != nil {
		born = 0o666
	}
	// Windows: a read-only file cannot be replaced, and a read-only temp cannot
	// be removed after a failure, so the attribute comes off the target for the
	// publish and goes back on the new file after it - the same outcome as
	// POSIX, where the rename never needed the file writable. Hidden and system
	// ride back the same way: ReplaceFile's documented preserve list does not
	// include the basic attributes, and the rename fallback carries nothing, so
	// a hidden config came back visible.
	readOnly := runtime.GOOS == "windows" && existErr == nil && existing.Mode().Perm()&0o200 == 0
	var carried uint32
	if existErr == nil {
		carried = carriedAttrs(existing)
	}
	var f *os.File
	var tmp string
	var last error
	for attempt := 0; attempt < 8; attempt++ {
		tmp = filepath.Join(dir, "."+base+".tmp"+strconv.Itoa(os.Getpid())+"."+strconv.Itoa(attempt))
		h, oerr := os.OpenFile(tmp, os.O_WRONLY|os.O_CREATE|os.O_EXCL, born)
		if oerr == nil {
			f = h
			break
		}
		last = oerr
	}
	if f == nil {
		return fmt.Errorf("%s: cannot create temporary file: %w", file, last)
	}
	var err error
	if _, werr := f.WriteString(data); werr != nil {
		err = werr
	} else {
		err = f.Sync()
	}
	// On the handle, so umask cannot narrow it the way it narrows a create
	// mode, and after the data, because a write by anyone but root clears
	// setuid/setgid. Best effort: a filesystem that cannot carry the mode is
	// not a reason to fail a write that otherwise succeeded. The whole mode
	// goes, setuid/setgid/sticky included, as an editor's rewrite would carry
	// it.
	if err == nil && existErr == nil && runtime.GOOS != "windows" {
		_ = f.Chmod(existing.Mode())
	}
	// The sync above is what forces the data out, so a close error here is the
	// unlikely tail of it - but a write error that only surfaces on close would
	// otherwise publish a truncated temp file over the target.
	if cerr := f.Close(); err == nil {
		err = cerr
	}
	if err != nil {
		os.Remove(tmp)
		return fmt.Errorf("%s: %w", file, err)
	}
	if readOnly {
		setReadOnly(target, false)
	}
	rerr := publishFile(tmp, target)
	if carried != 0 {
		restoreAttrs(target, carried) // whether or not the publish went through
	}
	if readOnly {
		setReadOnly(target, true) // whether or not the publish went through
	}
	if rerr != nil {
		os.Remove(tmp)
		return fmt.Errorf("%s: %w", file, rerr)
	}
	syncDir(dir)
	return nil
}

// resolveTarget is the path a save actually rewrites. A symlink is followed so
// the write goes through it; EvalSymlinks does that but needs the target to
// exist, so a dangling link is walked by hand and the file is created where it
// points. A path that is no link at all is a plain create at the path as given.
// A link cycle is an error: silently creating a regular file in its place
// would be the exact replacement the symlink walk exists to avoid.
func resolveTarget(file string) (string, error) {
	if p, err := filepath.EvalSymlinks(file); err == nil {
		return p, nil
	}
	p := file
	for hop := 0; hop < 40; hop++ {
		next, err := os.Readlink(p)
		if err != nil {
			break
		}
		if filepath.IsAbs(next) {
			p = next
		} else {
			p = filepath.Join(filepath.Dir(p), next)
		}
	}
	if _, err := os.Readlink(p); err == nil {
		return "", errors.New("too many levels of symbolic links")
	}
	if dir, err := filepath.EvalSymlinks(filepath.Dir(p)); err == nil {
		return filepath.Join(dir, filepath.Base(p)), nil
	}
	return p, nil
}

// setReadOnly toggles the windows read-only attribute, which is all Chmod
// controls there.
func setReadOnly(path string, on bool) {
	if m, err := os.Stat(path); err == nil {
		mode := m.Mode().Perm() | 0o200
		if on {
			mode &^= 0o222
		}
		_ = os.Chmod(path, mode)
	}
}

// publishFile moves the finished temp file over the target. Rename is atomic
// within a filesystem, which is all POSIX needs. The windows build swaps this
// out for one that goes through ReplaceFile (shcl_windows.go): a rename there
// publishes a brand-new file and leaves the destination's ACLs, attributes and
// named streams behind.
//
// A hook rather than a build-tagged pair so that this file still compiles and
// works on its own - the drop-in story is the whole point of the single file,
// and a tree that took the module gets the better windows publish anyway.
var publishFile = func(tmp, target string) error { return os.Rename(tmp, target) }

// carriedAttrs is the attribute bits a publish will not carry across by itself
// - hidden and system on windows, nothing anywhere else - and restoreAttrs
// turns them back on afterwards. Hooks for the same reason publishFile is.
var carriedAttrs = func(os.FileInfo) uint32 { return 0 }

var restoreAttrs = func(string, uint32) {}

// syncDir fsyncs the directory a save published into. The Sync on the file only
// covered the file; the rename is a directory change, so without this a power
// cut right after a save can lose the publish and leave the old content. Best
// effort - windows has no directory fsync, and a filesystem that refuses one is
// not a reason to fail a write that already succeeded.
func syncDir(dir string) {
	if d, derr := os.Open(dir); derr == nil {
		_ = d.Sync()
		_ = d.Close()
	}
}

// FileStatus is what LoadFile found: the four cases a consumer's own load
// path otherwise confuses. FileClean and FileHadErrors both carry a usable
// document; FileNotFound and FileUnreadable come back with an empty one.
type FileStatus int

const (
	FileClean     FileStatus = iota // read and parsed, no error diagnostics (hints allowed)
	FileHadErrors                   // read and parsed, but error diagnostics are present
	FileNotFound                    // no file at the path
	// exists but could not be read (permissions, a directory, bad encoding,
	// past a ReadFile cap)
	FileUnreadable
)

// String names the file status, so logging one reads as a case rather than a
// number.
func (s FileStatus) String() string {
	switch s {
	case FileClean:
		return "Clean"
	case FileHadErrors:
		return "HadErrors"
	case FileNotFound:
		return "NotFound"
	case FileUnreadable:
		return "Unreadable"
	}
	return "Clean"
}

// LoadFile is the file tier's load half: read and parse path at Standard.
// Never fails - the document always comes back usable (empty when the file
// could not be read), and the status separates the four cases consumers
// otherwise confuse: absent, present-but-unreadable, parsed with errors,
// clean.
func LoadFile(path string) (*Document, FileStatus) {
	return LoadFileWith(path, Standard)
}

// LoadFileWith is LoadFile at a chosen strictness. A strict-failing file
// reports FileHadErrors; the recover-and-continue document still comes back.
func LoadFileWith(path string, level Strictness) (*Document, FileStatus) {
	text, st := ReadFile(path, 0)
	if st != FileClean {
		return newParser().parse("", level), st
	}
	doc := newParser().parse(text, level)
	for _, d := range doc.diags {
		if d.Severity == SeverityError {
			return doc, FileHadErrors
		}
	}
	return doc, FileClean
}

// SaveRefused is the error SaveFile returns when the lost-content gate fires.
// It is its own type so a caller can tell a refusal from a write failure with
// errors.As rather than by matching on the message: a refusal is the caller's
// to reverse (SaveFileLossy is the override), a write failure is the disk's
// answer.
type SaveRefused struct {
	Path string
	Lost int // lines/values the load dropped (see LostCount)
}

// Error spells the refusal with the path and the count, so a log line says
// which file and how much a lossy save would drop.
func (e *SaveRefused) Error() string {
	return fmt.Sprintf("%s: refusing to save: load dropped %d line(s)/value(s) "+
		"this write would delete (see diagnostics; SaveFileLossy overrides)", e.Path, e.Lost)
}

// SaveFile is the file tier's save half: write this document's canonical text
// to path through WriteFileAtomic, so an interrupted save can never truncate
// the config it rewrites - the same mechanics the CLI's `--write` uses.
// Refuses when parsing lost content that a save would silently delete (see
// LostCount); SaveFileLossy writes anyway. A refusal comes back as
// *SaveRefused, a write failure as the wrapped i/o error.
func (d *Document) SaveFile(path string) error {
	if d.lost > 0 {
		return &SaveRefused{Path: path, Lost: d.lost}
	}
	return WriteFileAtomic(path, d.ToCanonical())
}

// SaveFileLossy is SaveFile without the lost-content gate: writes even when
// parsing dropped lines this save deletes. The caller owns that choice. It
// never returns *SaveRefused - the gate is the one thing it skips.
func (d *Document) SaveFileLossy(path string) error {
	return WriteFileAtomic(path, d.ToCanonical())
}

// disavowedNames returns the leaf names of the schema entries pick accepts,
// top-level fields and every fragment's fields alike. Read through the built
// schema, so the names are the ones validation will use (escapes resolved)
// and an entry whose key faulted disavows nothing.
func disavowedNames(schema *Document, pick func(*constraint) bool) []string {
	def, _ := buildSchema(schema)
	var names []string
	all := [][]constraint{def.cons}
	for _, fcs := range def.frags {
		all = append(all, fcs)
	}
	for _, list := range all {
		for i := range list {
			c := &list[i]
			if !pick(c) || len(c.segs) == 0 {
				continue
			}
			// Name wildcard: no single leaf name to disavow.
			if seg := c.segs[len(c.segs)-1]; !seg.star {
				names = append(names, seg.name)
			}
		}
	}
	return names
}

// h002Head is the single H002 wording site: the merge hint and the schema
// suppressor both come here, same discipline as h001Head.
func h002Head(name string) string {
	return fmt.Sprintf("merged with '%s' at ", name)
}

// SuppressDeclaredReopens drops the H002 hints a schema disavows: a section
// whose entry declares `reopen: true` is MEANT to be written in parts, so the
// merge hint is structurally a false positive there. Matching is by leaf
// name, same as the H001 suppressor, and it errs toward quiet, for a hint.
// Used by `check --schema` and LoadAndValidate; call it wherever doc
// diagnostics and a schema meet. Returns a fresh slice like the H001 one.
func SuppressDeclaredReopens(schema *Document, diags []Diagnostic) []Diagnostic {
	names := disavowedNames(schema, func(c *constraint) bool { return c.reopen })
	if len(names) == 0 {
		return append([]Diagnostic(nil), diags...)
	}
	heads := make([]string, len(names))
	for i, n := range names {
		heads[i] = h002Head(n)
	}
	kept := make([]Diagnostic, 0, len(diags))
	for _, d := range diags {
		if d.Code == "H002" {
			drop := false
			for _, h := range heads {
				if strings.HasPrefix(d.Message, h) {
					drop = true
					break
				}
			}
			if drop {
				continue
			}
		}
		kept = append(kept, d)
	}
	return kept
}

// emitElement uses minimal quoting: bare unless a reserved character (or
// lookalike hazard) forces it.
// One addition: an author-quoted element keeps its quotes unless the text reads as
// one of SHCL's own data formats - quoting those is just spelling (readers type the
// value either way), but quoting a plain string is the escape and must survive
// canonicalization. This clause only ever adds quoting, so a bare emit stays safe.
func emitElement(e *element) string {
	t := e.text
	needs := t == ""
	if !needs {
		for _, c := range t {
			switch c {
			case ' ', '\t', ',', ':', '#', '"', '\'', '[', ']':
				needs = true
			}
			if needs {
				break
			}
		}
	}
	// Edge whitespace beyond the space/tab above still has to force quotes: the
	// parser trims the full White_Space set, so a bare NBSP (or VT, FF, NEL,
	// ideographic space) at either end would not survive the reload. Edges only
	// - interior whitespace is never trimmed and quoting it would move bytes.
	if !needs && t != "" {
		r, _ := utf8.DecodeRuneInString(t)
		l, _ := utf8.DecodeLastRuneInString(t)
		if unicode.IsSpace(r) || unicode.IsSpace(l) {
			needs = true
		}
	}
	if !needs {
		if _, _, _, ok := fenceOpen(t); ok {
			needs = true
		}
	}
	if !needs && e.quoted && !isDataFormat(e) {
		needs = true
	}
	if needs {
		return quoteText(t)
	}
	return t
}

// isDataFormat reports whether the text reads as an int, float, bool, or
// datetime at standard strictness - fixed there deliberately, so canonical
// form cannot vary with the load strictness.
func isDataFormat(e *element) bool {
	// One pass over the bytes before any coercion. At Standard the int, float
	// and datetime forms all require at least one ASCII digit; the only formats
	// that do not are the boolean words, and the longest of those is "false".
	// An ordinary quoted string fails both tests, so emit stops running four
	// full coercions on every quoted element it writes.
	hasDigit := false
	for i := 0; i < len(e.text); i++ {
		if e.text[i] >= '0' && e.text[i] <= '9' {
			hasDigit = true
			break
		}
	}
	if !hasDigit {
		t := strings.TrimSpace(e.text)
		if len(t) > 5 {
			return false
		}
		_, ok := parseBoolText(t, Standard)
		return ok
	}
	if _, ok := parseIntText(e, Standard); ok {
		return true
	}
	if _, ok := parseFloatText(e, Standard); ok {
		return true
	}
	if _, ok := parseBoolText(e.text, Standard); ok {
		return true
	}
	if _, ok := ParseDateTime(e.text); ok {
		return true
	}
	return false
}

// bareQuoteCounts counts quote chars that are NOT already escaped in the raw
// text; escaped ones must stay untouched or every round-trip would re-escape them.
func bareQuoteCounts(t string) (dq, sq int) {
	// Bytes: the three characters matched are ASCII, and a continuation byte
	// matches none of them, so skipping one byte after a backslash counts the
	// same as skipping one rune.
	for i := 0; i < len(t); i++ {
		switch t[i] {
		case '\\':
			i++
		case '"':
			dq++
		case '\'':
			sq++
		}
	}
	return dq, sq
}

func quoteText(t string) string {
	// A dangling trailing backslash would turn the closing quote into an
	// escape pair - the scanner reads the path back wrong, or not at all.
	// Store the doubled spelling (identical on string read), the same rule
	// the element parser applies to bare text.
	if strings.HasSuffix(t, "\\") {
		t = normalizeDanglingBackslash(t)
	}
	dq, sq := bareQuoteCounts(t)
	if dq == 0 {
		return "\"" + t + "\""
	}
	if sq == 0 {
		return "'" + t + "'"
	}
	// Both quote kinds appear bare: escape the doubles, wrap in doubles.
	var out strings.Builder
	out.WriteByte('"')
	for i := 0; i < len(t); i++ {
		switch t[i] {
		case '\\':
			out.WriteByte(t[i])
			if i+1 < len(t) {
				i++
				out.WriteByte(t[i])
			}
		case '"':
			out.WriteString("\\\"")
		default:
			out.WriteByte(t[i])
		}
	}
	out.WriteByte('"')
	return out.String()
}

// ---------------------------------------------------------------------------
// Accessor: path resolution
// ---------------------------------------------------------------------------

type resolvedKind int

const (
	resNone resolvedKind = iota
	resOne
	resMany
	// resSlots (wildcard): one slot per instance, in file order; negative =
	// the sub-path did not land on one node (-1 missing, -2 ambiguous).
	resSlots
)

type resolved struct {
	kind  resolvedKind
	one   int
	many  []int
	slots []int
}

func (d *Document) nameIndex() *nameIndex {
	if idx := d.index.Load(); idx != nil {
		return idx
	}
	d.indexMu.Lock()
	defer d.indexMu.Unlock()
	if idx := d.index.Load(); idx != nil {
		return idx
	}
	idx := &nameIndex{
		first:    map[uint64]int{},
		last:     map[uint64]int{},
		nextSame: make([]int, len(d.arena)),
	}
	for i := range idx.nextSame {
		idx.nextSame[i] = nilNode
	}
	for p := range d.arena {
		for _, c := range d.arena[p].children {
			idx.append(nameKey(p, d.arena[c].name), c)
		}
	}
	d.index.Store(idx)
	return idx
}

func (d *Document) childrenNamed(parent int, name string) []int {
	idx := d.nameIndex()
	var out []int
	c, hit := idx.first[nameKey(parent, name)]
	if !hit {
		c = nilNode
	}
	for c != nilNode {
		if d.arena[c].name == name && d.arena[c].parent == parent {
			out = append(out, c)
		}
		c = idx.nextSame[c]
	}
	return out
}

func (d *Document) resolveFrom(start []int, segs []segment) resolved {
	cur := append([]int(nil), start...)
	for i := range segs {
		seg := &segs[i]
		var next []int
		for _, n := range cur {
			if seg.star {
				next = append(next, d.arena[n].children...)
			} else {
				next = append(next, d.childrenNamed(n, seg.name)...)
			}
		}
		if seg.star {
			// Name wildcard: same per-slot split as `[*]`, over every child.
			rest := segs[i+1:]
			slots := make([]int, 0, len(next))
			for _, inst := range next {
				if len(rest) == 0 {
					slots = append(slots, inst)
					continue
				}
				r := d.resolveFrom([]int{inst}, rest)
				switch r.kind {
				case resOne:
					slots = append(slots, r.one)
				case resNone:
					slots = append(slots, -1)
				case resSlots:
					// A wildcard after a wildcard: the inner slots join the
					// outer list, so the two compose into one flat run of
					// leaves rather than one unreadable slot.
					slots = append(slots, r.slots...)
				default:
					slots = append(slots, -2)
				}
			}
			return resolved{kind: resSlots, slots: slots}
		}
		switch {
		case seg.sel == nil:
			cur = next
		case seg.sel.kind == selByValue:
			want := applyEscapes(seg.sel.value)
			var filtered []int
			for _, c := range next {
				if dispKey(&d.arena[c].value) == want && (!seg.sel.quoted || singleScalar(&d.arena[c].value)) {
					filtered = append(filtered, c)
				}
			}
			cur = filtered
		case seg.sel.kind == selByIndex:
			if seg.sel.index < uint64(len(next)) {
				cur = []int{next[seg.sel.index]}
			} else {
				cur = nil
			}
		default:
			// Wildcard: remaining path resolves per-instance; slots stay aligned.
			rest := segs[i+1:]
			slots := make([]int, 0, len(next))
			for _, inst := range next {
				if len(rest) == 0 {
					slots = append(slots, inst)
					continue
				}
				r := d.resolveFrom([]int{inst}, rest)
				switch r.kind {
				case resOne:
					slots = append(slots, r.one)
				case resNone:
					slots = append(slots, -1)
				case resSlots:
					// A wildcard after a wildcard: the inner slots join the
					// outer list, so the two compose into one flat run of
					// leaves rather than one unreadable slot.
					slots = append(slots, r.slots...)
				default:
					slots = append(slots, -2)
				}
			}
			return resolved{kind: resSlots, slots: slots}
		}
	}
	switch len(cur) {
	case 0:
		return resolved{kind: resNone}
	case 1:
		return resolved{kind: resOne, one: cur[0]}
	}
	return resolved{kind: resMany, many: cur}
}

func (d *Document) resolve(path string) (resolved, bool) {
	scan, err := scanLookup(path)
	if err != nil || scan.valueText != nil {
		return resolved{}, false // a query has no value part
	}
	return d.resolveFrom([]int{root}, scan.segments), true
}

// Count returns the instance count at a path (0 when nothing matches).
func (d *Document) Count(path string) int {
	r, ok := d.resolve(path)
	if !ok {
		return 0
	}
	switch r.kind {
	case resOne:
		return 1
	case resMany:
		return len(r.many)
	case resSlots:
		return len(r.slots)
	}
	return 0
}

// Paths returns every field path in the document, in file order, deduplicated -
// a query recipe for tooling. A segment that is not bare-name-safe is emitted
// quoted and escaped - the form the path scanner accepts - so each path is a
// well-formed lookup path and nothing in the document is hidden.
func (d *Document) Paths() []string {
	var out []string
	seen := map[string]bool{}
	type ent struct {
		node   int
		prefix string
	}
	var stack []ent
	top := d.arena[root].children
	for i := len(top) - 1; i >= 0; i-- {
		stack = append(stack, ent{top[i], ""})
	}
	for len(stack) > 0 {
		e := stack[len(stack)-1]
		stack = stack[:len(stack)-1]
		seg := emitName(d.arena[e.node].name)
		path := seg
		if e.prefix != "" {
			path = e.prefix + "." + seg
		}
		if !seen[path] {
			seen[path] = true
			out = append(out, path)
		}
		kids := d.arena[e.node].children
		for i := len(kids) - 1; i >= 0; i-- {
			stack = append(stack, ent{kids[i], path})
		}
	}
	return out
}

// Line returns the 1-based source line of the binding at a path, for consumer
// checks the schema cannot express. 0 when the path does not resolve to
// exactly one node, or the node was writer-built. Merged instances cite the
// first binding's line, matching diagnostics.
func (d *Document) Line(path string) int {
	r, ok := d.resolve(path)
	if !ok || r.kind != resOne {
		return 0
	}
	return d.arena[r.one].line
}

// AuthoredName is the field name at a path exactly as the author spelled it
// (case unfolded, outer quotes stripped), so a message can echo `SYMBOLS` when
// the file said SYMBOLS. Escape sequences stay as written too: a name is
// stored, compared and emitted with its escapes RESOLVED, so this is the one
// call that hands the source spelling back - which is what an as-authored
// accessor is for. Resolution mirrors Line(): empty when the path does not resolve to
// exactly one node. Merged instances keep the first binding's spelling; a
// writer-built node keeps the spelling the setter's path used.
func (d *Document) AuthoredName(path string) string {
	r, ok := d.resolve(path)
	if !ok || r.kind != resOne {
		return ""
	}
	return d.arena[r.one].authored()
}

// Lines is the plural Line(): 1-based source lines at a path, in file order,
// so a repeated field - the case that most wants a citable line - yields
// every binding's. Wildcard slots that did not resolve stay in the list as 0,
// and a writer-built node is 0, so indices keep matching Count().
func (d *Document) Lines(path string) []int {
	r, ok := d.resolve(path)
	if !ok {
		return nil
	}
	switch r.kind {
	case resOne:
		return []int{d.arena[r.one].line}
	case resMany:
		out := make([]int, 0, len(r.many))
		for _, n := range r.many {
			out = append(out, d.arena[n].line)
		}
		return out
	case resSlots:
		out := make([]int, 0, len(r.slots))
		for _, s := range r.slots {
			if s >= 0 {
				out = append(out, d.arena[s].line)
			} else {
				out = append(out, 0)
			}
		}
		return out
	}
	return nil
}

// Children returns the child field names under a path, in file order,
// duplicates included - the "what keys are in this section?" question Paths()
// (deduplicated, path-shaped) cannot answer. "" enumerates the top level.
// Names come back as stored; QuoteSegment() makes one splice-safe in a path.
func (d *Document) Children(path string) []string {
	node := root
	if strings.TrimSpace(path) != "" {
		r, ok := d.resolve(path)
		if !ok || r.kind != resOne {
			return nil
		}
		node = r.one
	}
	kids := d.arena[node].children
	out := make([]string, 0, len(kids))
	for _, c := range kids {
		out = append(out, d.arena[c].name)
	}
	return out
}

// Instances returns the instance values at a path, in file order. Wildcard
// slots that did not resolve stay in the list as "" so indices keep matching
// Count().
func (d *Document) Instances(path string) []string {
	r, ok := d.resolve(path)
	if !ok {
		return nil
	}
	switch r.kind {
	case resOne:
		return []string{d.arena[r.one].value.display()}
	case resMany:
		out := make([]string, 0, len(r.many))
		for _, n := range r.many {
			out = append(out, d.arena[n].value.display())
		}
		return out
	case resSlots:
		out := make([]string, 0, len(r.slots))
		for _, s := range r.slots {
			if s >= 0 {
				out = append(out, d.arena[s].value.display())
			} else {
				out = append(out, "")
			}
		}
		return out
	}
	return nil
}

// ---------------------------------------------------------------------------
// Writer: typed emit, defaults, comments, structural edits
// ---------------------------------------------------------------------------
// The reverse of the Accessor. A setter builds the canonical stored text for a
// typed value (the inverse of the matching read) and places it at a path,
// creating intermediate nodes on the way. Reads and ToCanonical walk children
// slices, so mutating the arena directly is enough - the parser's child map is
// already gone and is not maintained here.

func boolText(v bool) string {
	if v {
		return "true"
	}
	return "false"
}

// literalValue reads text as the value half of a line, for the setters that
// take value syntax rather than data. Rejects what could not have come off one
// line: a line break, or a quote that never closes. An unquoted # ends the
// value here exactly as it would in a file.
func literalValue(text string) (value, bool) {
	if strings.ContainsAny(text, "\n\r") {
		return value{}, false
	}
	v, _ := splitComment(text)
	v = strings.TrimFunc(v, unicode.IsSpace)
	// Bracket-array text is refused too: in a file it is E019 and the line is
	// lost, so writing it as a two-element array holding `[1` and `2]` would
	// be a different wrong answer.
	if unterminatedQuote(v) || (strings.HasPrefix(v, "[") && strings.HasSuffix(v, "]")) {
		return value{}, false
	}
	return parseCell(v), true
}

func cellOf(text string) value {
	return value{kind: vCell, els: []element{{text: text}}}
}

// encodeString is the inverse of a scalar string read (applyEscapes): only
// backslash, newline, and tab need encoding; emitElement wraps quote/reserved
// chars itself, and reparse strips that wrapping.
func encodeString(s string) string {
	var b strings.Builder
	for _, c := range s {
		switch c {
		case '\\':
			b.WriteString("\\\\")
		case '\n':
			b.WriteString("\\n")
		case '\t':
			b.WriteString("\\t")
		default:
			b.WriteRune(c)
		}
	}
	return b.String()
}

// chooseFence picks a backtick fence long enough that no content line closes it.
func chooseFence(content string) (byte, int) {
	maxrun := 0
	for _, line := range strings.Split(content, "\n") {
		t := strings.TrimSpace(line)
		if t != "" && strings.Trim(t, "`") == "" && len(t) > maxrun {
			maxrun = len(t)
		}
	}
	if maxrun+1 < 3 {
		return '`', 3
	}
	return '`', maxrun + 1
}

// arrayCell builds an inline-array value; the empty array is an empty value.
func arrayCell(texts []string) value {
	if len(texts) == 0 {
		return value{kind: vEmpty}
	}
	els := make([]element, len(texts))
	for i, t := range texts {
		els[i] = element{text: t}
	}
	return value{kind: vCell, els: els}
}

// New returns a fresh document with no bindings - the start point for
// schema-driven generation. Set values, then ToCanonical().
func New() *Document {
	return Parse("")
}

func (d *Document) newChild(parent int, name, nameSrc string, v value) int {
	idx := len(d.arena)
	// Hand-written files separate top-level sections with a blank line;
	// writer-built ones do the same (the emitter never blanks line 1).
	d.arena = append(d.arena, nodeData{
		name: name, nameSrc: spelled(name, nameSrc), value: v, parent: parent,
		blankBefore: parent == root,
	})
	d.arena[parent].children = append(d.arena[parent].children, idx)
	if ix := d.index.Load(); ix != nil {
		ix.append(nameKey(parent, name), idx)
	}
	return idx
}

// WriteReason reports why a write at this path would fail - the reason behind
// a setter's bare false, so a consumer's error message need not guess.
// Writable means the same validation place() runs would pass; nothing is
// created.
func (d *Document) WriteReason(path string) WriteReason {
	scan, err := scanLookup(path)
	if err != nil {
		return BadPath
	}
	r, _ := d.probeWrite(scan)
	return r
}

// probeWrite is the validation walk WriteReason and place share. The returned
// trail records where each segment landed - -1 from the point the path falls
// off the existing tree - so place can create from exactly there instead of
// scanning the path and walking the tree a second time.
func (d *Document) probeWrite(scan pathScan) (WriteReason, []int) {
	if scan.valueText != nil {
		return ValueInPath, nil
	}
	if len(scan.segments) == 0 {
		return BadPath, nil
	}
	// Writer side of the load-time nesting cap: never create deeper.
	if len(scan.segments) > MaxDepth {
		return TooDeep, nil
	}
	// The probe walk place() validates with: once it falls off the existing
	// tree, a later `[#k]` can never match (fresh intermediates are created
	// childless), so an index segment past that point is unresolvable.
	trail := make([]int, 0, len(scan.segments))
	probe, alive := root, true
	for i := range scan.segments {
		seg := &scan.segments[i]
		if seg.star {
			return Wildcard, nil
		}
		// A newline in a SELECTOR has no one-line spelling, so the emitted
		// binding would split across two lines and reparse as neither. The
		// selector stores its path text raw and the value emitter never escapes
		// a line break, so nothing downstream can rescue it - and the reload
		// loses nothing it can count, so the save gate would not catch it. A
		// newline in a NAME is fine: names are stored escape-resolved and
		// emitted through the name escaper, which spells a line break \n and
		// reads it back as one.
		if seg.sel != nil && seg.sel.kind == selByValue && strings.Contains(seg.sel.value, "\n") {
			return BadPath, nil
		}
		switch {
		case seg.sel == nil:
			if alive {
				alive = false
				if m := d.childrenNamed(probe, seg.name); len(m) > 0 {
					probe, alive = m[0], true
				}
			}
		case seg.sel.kind == selByValue:
			if alive {
				want := applyEscapes(seg.sel.value)
				alive = false
				for _, c := range d.childrenNamed(probe, seg.name) {
					if dispKey(&d.arena[c].value) == want && (!seg.sel.quoted || singleScalar(&d.arena[c].value)) {
						probe, alive = c, true
						break
					}
				}
			}
		case seg.sel.kind == selByIndex:
			if !alive {
				return NoSuchIndex, nil
			}
			matches := d.childrenNamed(probe, seg.name)
			if seg.sel.index >= uint64(len(matches)) {
				return NoSuchIndex, nil
			}
			probe = matches[seg.sel.index]
		default:
			return Wildcard, nil
		}
		if alive {
			trail = append(trail, probe)
		} else {
			trail = append(trail, -1)
		}
	}
	return Writable, trail
}

// place walks (creating as needed) to the node a write targets. A trailing name
// with no selector hits the first same-named instance (or a new one); a [value]
// selector selects the matching instance or creates it; [#k] must already
// exist. ok=false means the path is unusable for a write (WriteReason says
// why). Validation runs first, so a doomed path leaves no half-created
// intermediates behind.
func (d *Document) place(path string) (int, bool) {
	scan, err := scanLookup(path)
	if err != nil {
		return 0, false
	}
	reason, trail := d.probeWrite(scan)
	if reason != Writable {
		return 0, false
	}
	cur := root
	for i := range scan.segments {
		seg := &scan.segments[i]
		// The probe already resolved every segment that exists; only the tail
		// it fell off has anything to create.
		if trail[i] >= 0 {
			cur = trail[i]
			continue
		}
		switch {
		case seg.sel == nil:
			cur = d.newChild(cur, seg.name, seg.nameSrc, value{kind: vEmpty})
		case seg.sel.kind == selByValue:
			cur = d.newChild(cur, seg.name, seg.nameSrc, cellOf(seg.sel.value))
		default:
			// Unreachable: probeWrite refuses a wildcard outright and an
			// unresolvable index, so neither reaches an empty trail slot.
			return 0, false
		}
	}
	return cur, true
}

func (d *Document) setValue(path string, v value) bool {
	idx, ok := d.place(path)
	if !ok {
		return false
	}
	d.arena[idx].value = v
	d.arena[idx].src = nil // written value has no source spelling
	d.collapseDup(idx)
	return true
}

// collapseDup: a written value may now collide with a same-named sibling under
// the in-file merge rule; fold the pair the way a reparse would (earlier
// sibling survives, later one folds children and trivia in) so Writer output
// stays a formatter fixpoint.
func (d *Document) collapseDup(node int) {
	parent := d.arena[node].parent
	me := &d.arena[node]
	other := -1
	for _, c := range d.childrenNamed(parent, me.name) {
		o := &d.arena[c]
		if c != node && mergeEq(o.name, &o.value, me.name, &me.value) {
			other = c
			break
		}
	}
	if other < 0 {
		return
	}
	pos := func(n int) int {
		for i, c := range d.arena[parent].children {
			if c == n {
				return i
			}
		}
		return int(^uint(0) >> 1)
	}
	survivor, loser := other, node
	if pos(node) < pos(other) {
		survivor, loser = node, other
	}
	moved := append([]int(nil), d.arena[loser].children...)
	foldNodeInto(d.arena, survivor, loser)
	keep := d.arena[parent].children[:0]
	for _, c := range d.arena[parent].children {
		if c != loser {
			keep = append(keep, c)
		}
	}
	d.arena[parent].children = keep
	if ix := d.index.Load(); ix != nil {
		ix.unlink(nameKey(parent, d.arena[loser].name), loser)
		for _, k := range moved {
			name := d.arena[k].name
			ix.unlink(nameKey(loser, name), k)
			ix.append(nameKey(survivor, name), k)
		}
	}
	d.foldDupsBelow(survivor)
}

// foldDupsBelow: folding moves the loser's children up a level, where they can
// collide with the survivor's own. The parser's fold is depth-first for the
// same reason; only a node that just received children can hold a new pair.
func (d *Document) foldDupsBelow(start int) {
	stack := []int{start}
	for len(stack) > 0 {
		parent := stack[len(stack)-1]
		stack = stack[:len(stack)-1]
		kids := d.arena[parent].children
		d.arena[parent].children = nil
		first := map[uint64]slot{}
		keep := make([]int, 0, len(kids))
		for _, c := range kids {
			h := mergeHash(d.arena[c].name, &d.arena[c].value)
			if survivor, ok := slotFirstMatch(first, h, func(x int) bool {
				return mergeEq(d.arena[x].name, &d.arena[x].value, d.arena[c].name, &d.arena[c].value)
			}); ok {
				moved := append([]int(nil), d.arena[c].children...)
				foldNodeInto(d.arena, survivor, c)
				if ix := d.index.Load(); ix != nil {
					ix.unlink(nameKey(parent, d.arena[c].name), c)
					for _, k := range moved {
						name := d.arena[k].name
						ix.unlink(nameKey(c, name), k)
						ix.append(nameKey(survivor, name), k)
					}
				}
				stack = append(stack, survivor)
			} else {
				slotInsert(first, h, c)
				keep = append(keep, c)
			}
		}
		d.arena[parent].children = keep
	}
}

// Exists is true when the path resolves to at least one real node.
func (d *Document) Exists(path string) bool {
	r, ok := d.resolve(path)
	if !ok {
		return false
	}
	switch r.kind {
	case resOne, resMany:
		return true
	case resSlots:
		for _, s := range r.slots {
			if s >= 0 {
				return true
			}
		}
	}
	return false
}

// Remove deletes the node(s) at a path (with their subtrees); returns how many.
func (d *Document) Remove(path string) int {
	r, ok := d.resolve(path)
	if !ok {
		return 0
	}
	var targets []int
	switch r.kind {
	case resOne:
		targets = []int{r.one}
	case resMany:
		targets = r.many
	case resSlots:
		for _, s := range r.slots {
			if s >= 0 {
				targets = append(targets, s)
			}
		}
	}
	for _, t := range targets {
		p := d.arena[t].parent
		kids := d.arena[p].children[:0]
		for _, c := range d.arena[p].children {
			if c != t {
				kids = append(kids, c)
			}
		}
		d.arena[p].children = kids
		if ix := d.index.Load(); ix != nil {
			ix.unlink(nameKey(p, d.arena[t].name), t)
		}
	}
	return len(targets)
}

// SetComment attaches a leading comment line to the node at a path (creating an
// empty node if absent, so a section can be annotated). A missing '#' is added;
// only the first line is kept (a comment is one line), and trailing whitespace
// comes off the way the load takes it, so text that is blank leaves a bare '#'.
func (d *Document) SetComment(path, text string) bool {
	idx, ok := d.place(path)
	if !ok {
		return false
	}
	line := text
	if i := strings.IndexByte(line, '\n'); i >= 0 {
		line = line[:i]
	}
	if !strings.HasPrefix(line, "#") {
		line = "# " + line
	}
	// Without this the load trims what was written and the writer's output
	// stops being a fmt fixpoint.
	line = trimEndWS(line)
	// The node's own blank moves above its first comment; otherwise the blank
	// would separate the comment from what it annotates.
	nd := &d.arena[idx]
	l := plainLead(line)
	if nd.blankBefore && len(nd.leading()) == 0 {
		l.blankBefore = true
		nd.blankBefore = false
	}
	t := nd.trivMut()
	t.leading = append(t.leading, l)
	return true
}

// SetInt binds an integer at path, creating the path as needed; false = path
// not writable (WriteReason says why - same for every setter). Worth checking
// rather than assuming: an ignored false means the save that follows writes a
// document missing the edit, and reports success doing it.
func (d *Document) SetInt(path string, v int64) bool {
	return d.setValue(path, cellOf(strconv.FormatInt(v, 10)))
}

// SetFloat binds a float at path, in the canonical shortest spelling. An
// infinity or a NaN has no spelling the reader accepts, so it fails the write
// rather than binding a value that cannot read back.
func (d *Document) SetFloat(path string, v float64) bool {
	if math.IsInf(v, 0) || math.IsNaN(v) {
		return false
	}
	return d.setValue(path, cellOf(FormatFloat(v)))
}

// SetBool binds true/false at path.
func (d *Document) SetBool(path string, v bool) bool {
	return d.setValue(path, cellOf(boolText(v)))
}

// SetString binds a string at path, escaped so it reads back exactly.
func (d *Document) SetString(path, v string) bool {
	return d.setValue(path, cellOf(encodeString(v)))
}

// SetDateTime binds a datetime at path, in its canonical spelling. The
// struct's fields are public and carry no invariant, so a value the reader
// would refuse (month 13, a fraction with no seconds, an empty struct) fails
// the write rather than binding text that cannot read back.
func (d *Document) SetDateTime(path string, v DateTime) bool {
	if !datetimeReadsBack(v) {
		return false
	}
	return d.setValue(path, cellOf(v.String()))
}

// SetEmpty binds an empty value at path (distinct from the empty string).
func (d *Document) SetEmpty(path string) bool {
	return d.setValue(path, value{kind: vEmpty})
}

// SetRaw binds a raw block at path, picking a fence longer than any content line.
// The info-string is stored as a fence line would read it back (trimmed); one
// holding a line break or an unquoted `#` has no fence-line spelling (the `#`
// would read back as a comment) and fails the write. A body line ending in CR
// fails for the same reason: the load takes the whole trailing CR run off every
// line, so it would not read back.
func (d *Document) SetRaw(path, content, info string) bool {
	if _, c := splitComment(info); strings.ContainsAny(info, "\n\r") || c != "" {
		return false
	}
	for _, line := range strings.Split(content, "\n") {
		if strings.HasSuffix(line, "\r") {
			return false
		}
	}
	info = strings.TrimSpace(info)
	fc, fl := chooseFence(content)
	return d.setValue(path, value{kind: vRaw, raw: &rawValue{content: content, info: info, fenceChar: fc, fenceLen: fl}})
}

// SetIntArray binds an inline integer array at path.
func (d *Document) SetIntArray(path string, v []int64) bool {
	texts := make([]string, len(v))
	for i, x := range v {
		texts[i] = strconv.FormatInt(x, 10)
	}
	return d.setValue(path, arrayCell(texts))
}

// SetFloatArray binds an inline float array at path.
func (d *Document) SetFloatArray(path string, v []float64) bool {
	for _, x := range v {
		if math.IsInf(x, 0) || math.IsNaN(x) {
			return false
		}
	}
	texts := make([]string, len(v))
	for i, x := range v {
		texts[i] = FormatFloat(x)
	}
	return d.setValue(path, arrayCell(texts))
}

// SetBoolArray binds an inline bool array at path.
func (d *Document) SetBoolArray(path string, v []bool) bool {
	texts := make([]string, len(v))
	for i, x := range v {
		texts[i] = boolText(x)
	}
	return d.setValue(path, arrayCell(texts))
}

// SetStringArray binds an inline string array at path, per-element escaped.
func (d *Document) SetStringArray(path string, v []string) bool {
	texts := make([]string, len(v))
	for i, x := range v {
		texts[i] = encodeString(x)
	}
	return d.setValue(path, arrayCell(texts))
}

// SetDateTimeArray binds an inline datetime array at path.
func (d *Document) SetDateTimeArray(path string, v []DateTime) bool {
	for _, x := range v {
		if !datetimeReadsBack(x) {
			return false
		}
	}
	texts := make([]string, len(v))
	for i, x := range v {
		texts[i] = x.String()
	}
	return d.setValue(path, arrayCell(texts))
}

// Default (only-if-absent) forms - the "emit defaults" half of the Writer.

// SetIntDefault is SetInt only when path has no node yet.
func (d *Document) SetIntDefault(path string, v int64) bool {
	if !d.Exists(path) {
		return d.SetInt(path, v)
	}
	return true
}

// SetFloatDefault is SetFloat only when path has no node yet.
func (d *Document) SetFloatDefault(path string, v float64) bool {
	if !d.Exists(path) {
		return d.SetFloat(path, v)
	}
	return true
}

// SetBoolDefault is SetBool only when path has no node yet.
func (d *Document) SetBoolDefault(path string, v bool) bool {
	if !d.Exists(path) {
		return d.SetBool(path, v)
	}
	return true
}

// SetLiteral binds text at path as value syntax rather than as data: "80, 443"
// becomes a two-element array where SetString would store one string that has
// to be quoted. This is how a caller holding value text - a config line, a
// user's --set argument - writes it without knowing its shape first. Fails on
// text that could not be one line's value (see literalValue).
func (d *Document) SetLiteral(path, text string) bool {
	v, ok := literalValue(text)
	if !ok {
		return false
	}
	return d.setValue(path, v)
}

// SetLiteralDefault is SetLiteral only when path has no node yet.
func (d *Document) SetLiteralDefault(path, text string) bool {
	if !d.Exists(path) {
		return d.SetLiteral(path, text)
	}
	return true
}

// SetStringDefault is SetString only when path has no node yet.
func (d *Document) SetStringDefault(path, v string) bool {
	if !d.Exists(path) {
		return d.SetString(path, v)
	}
	return true
}

// SetDateTimeDefault is SetDateTime only when path has no node yet.
func (d *Document) SetDateTimeDefault(path string, v DateTime) bool {
	if !d.Exists(path) {
		return d.SetDateTime(path, v)
	}
	return true
}

// SetRawDefault is SetRaw only when path has no node yet.
func (d *Document) SetRawDefault(path, content, info string) bool {
	if !d.Exists(path) {
		return d.SetRaw(path, content, info)
	}
	return true
}

// SetIntArrayDefault is SetIntArray only when path has no node yet.
func (d *Document) SetIntArrayDefault(path string, v []int64) bool {
	if !d.Exists(path) {
		return d.SetIntArray(path, v)
	}
	return true
}

// SetFloatArrayDefault is SetFloatArray only when path has no node yet.
func (d *Document) SetFloatArrayDefault(path string, v []float64) bool {
	if !d.Exists(path) {
		return d.SetFloatArray(path, v)
	}
	return true
}

// SetBoolArrayDefault is SetBoolArray only when path has no node yet.
func (d *Document) SetBoolArrayDefault(path string, v []bool) bool {
	if !d.Exists(path) {
		return d.SetBoolArray(path, v)
	}
	return true
}

// SetStringArrayDefault is SetStringArray only when path has no node yet.
func (d *Document) SetStringArrayDefault(path string, v []string) bool {
	if !d.Exists(path) {
		return d.SetStringArray(path, v)
	}
	return true
}

// SetDateTimeArrayDefault is SetDateTimeArray only when path has no node yet.
func (d *Document) SetDateTimeArrayDefault(path string, v []DateTime) bool {
	if !d.Exists(path) {
		return d.SetDateTimeArray(path, v)
	}
	return true
}

// ---------------------------------------------------------------------------
// Layered loading: overlay a higher-priority document onto a lower one.
// ---------------------------------------------------------------------------

// Merge overlays over (a higher-priority layer) onto d (the lower one).
// Container instances merge by (name, value) exactly like the in-file rule; a
// leaf name present in over replaces d's same-named children at that scope -
// provided those base children are leaves too - so scalars, arrays, and raw
// blocks get real override while a bare section header merges instead of
// wiping. over-only nodes are appended. Comment trivia rides with each node.
// Load(defaults, site, user) is a left fold of this: each later file overlaid
// on the earlier ones.
//
// The fold is not associative: (A+B)+C and A+(B+C) differ where a bare header
// meets an overridden leaf, so a cached upper pair is not the same document the
// CLI's left fold produces. d keeps its own strictness, so a value from a
// stricter layer reads with d's coercion. And a replaced node is kept until the
// document is dropped: this costs a pass over the touched scopes plus an index
// rebuild on the next read.
func (d *Document) Merge(over *Document) {
	d.index.Store(nil)
	d.lost += over.lost
	d.overlay(root, over, root)
	// Layers commonly share a footer; keeping one copy of each keeps a
	// stack of files from repeating it once per layer. Only the lines
	// already here count: a layer's own repeats are its content.
	had := len(d.orphans)
	for _, o := range over.orphans {
		seen := false
		for _, e := range d.orphans[:had] {
			if e.text == o.text {
				seen = true
				break
			}
		}
		if !seen {
			d.orphans = append(d.orphans, o)
		}
	}
}

// One grouping pass over each side, then a single children rebuild: the old
// shape re-filtered the over side per distinct name and re-scanned (and
// re-keyed) the base side per over node - three O(K^2) terms at one parent,
// plus a full vector rebuild per replaced name.

// adoptTrivia: a matched instance keeps the base node, so the over side's
// comments have to move onto it or they are lost. Same rule as an in-file
// merge: leading concatenates in layer order, first trailing wins.
func (d *Document) adoptTrivia(base int, over *Document, ok int) {
	st := over.arena[ok].trivia
	if st == nil {
		return
	}
	bt := d.arena[base].trivMut()
	// append copies the lead values, so the merged doc never shares a
	// backing array with over, which the caller may still mutate.
	bt.leading = append(bt.leading, st.leading...)
	if st.trailing != "" {
		if bt.trailing == "" {
			bt.trailing = st.trailing
		} else {
			bt.leading = append(bt.leading, plainLead(st.trailing))
		}
	}
	bt.after = append(bt.after, st.after...)
	bt.inside = append(bt.inside, st.inside...)
}

func (d *Document) overlay(baseParent int, over *Document, overParent int) {
	overKids := over.arena[overParent].children
	// Over side: name -> node bucket, in first-appearance order.
	var order []string
	type overKid struct{ pos, node int }
	groups := map[string][]overKid{}
	for pos, k := range overKids {
		n := over.arena[k].name
		if _, seen := groups[n]; !seen {
			order = append(order, n)
		}
		groups[n] = append(groups[n], overKid{pos, k})
	}
	// Base side, one pass: does the name have a container instance, and
	// which child carries each (name, key) - every key computed once. The
	// list is cloned because the splices below rewrite it as they go.
	baseKids := append([]int(nil), d.arena[baseParent].children...)
	hasContainer := map[string]bool{}
	byKey := map[[2]string]int{}
	for _, b := range baseKids {
		name := d.arena[b].name
		hasContainer[name] = hasContainer[name] || len(d.arena[b].children) > 0
		key := [2]string{name, d.arena[b].value.key()}
		if _, dup := byKey[key]; !dup {
			byKey[key] = b
		}
	}
	// Decide per name. A name whose over-side nodes are all leaves is an
	// override - but only when the base side of the group is leaf-shaped
	// too. Against a base container, a childless over-node is a wrapper
	// mention, not a leaf, so it falls through to the instance merge: a
	// bare section header in a higher layer never wipes the subtree below.
	// Replaced groups splice in the rebuild; everything appended (unmatched
	// instances, and replaced names base never had) keeps the over file's
	// order, which the per-name pass here would otherwise regroup.
	replace := map[string][]int{}
	var appended []overKid
	emptyKey := (&value{kind: vEmpty}).key()
	for _, name := range order {
		group := groups[name]
		overLeafy := true
		for _, k := range group {
			if len(over.arena[k.node].children) > 0 {
				overLeafy = false
				break
			}
		}
		baseContainer, inBase := hasContainer[name]
		if overLeafy && !baseContainer {
			clones := make([]int, len(group))
			for i, ok := range group {
				clones[i] = d.cloneSubtree(over, ok.node, baseParent)
			}
			if inBase {
				replace[name] = clones
			} else {
				for i, ok := range group {
					appended = append(appended, overKid{ok.pos, clones[i]})
				}
			}
		} else {
			for _, k := range group {
				ok := k.node
				okey := over.arena[ok].value.key()
				target, found := byKey[[2]string{name, okey}]
				// A raw block in the higher layer fills a same-named empty
				// binding below, exactly as a fence line fills one inside a
				// single file. Without it, merging two documents and parsing
				// them run together disagree: both bindings survive here and
				// fold there, so the merged output is not a formatter fixpoint.
				if !found && over.arena[ok].value.kind == vRaw {
					empty := [2]string{name, emptyKey}
					if b, hit := byKey[empty]; hit {
						cv := cloneValue(&over.arena[ok].value)
						d.arena[b].value = cv
						delete(byKey, empty)
						if _, dup := byKey[[2]string{name, okey}]; !dup {
							byKey[[2]string{name, okey}] = b
						}
						target, found = b, true
					}
				}
				if found {
					d.adoptTrivia(target, over, ok)
					d.overlay(target, over, ok)
				} else {
					c := d.cloneSubtree(over, ok, baseParent)
					appended = append(appended, overKid{k.pos, c})
				}
			}
		}
	}
	if len(replace) == 0 && len(appended) == 0 {
		return
	}
	// Rebuild once: each replaced group lands at its name's first original
	// position (dropped nodes stay in the arena, unreferenced - reads and
	// emit walk children from the root), appends go at the end.
	newKids := make([]int, 0, len(baseKids)+len(appended))
	spliced := map[string]bool{}
	for _, b := range baseKids {
		name := d.arena[b].name
		if clones, rep := replace[name]; rep {
			if !spliced[name] {
				spliced[name] = true
				newKids = append(newKids, clones...)
			}
		} else {
			newKids = append(newKids, b)
		}
	}
	sort.Slice(appended, func(i, j int) bool { return appended[i].pos < appended[j].pos })
	for _, k := range appended {
		newKids = append(newKids, k.node)
	}
	d.arena[baseParent].children = newKids
}

// cloneValue copies the value's storage too - a struct copy would share the
// els backing array (and the raw block) with `over`, and the clone must
// survive `over` being released.
func cloneValue(v *value) value {
	cv := *v
	cv.els = append([]element(nil), cv.els...)
	if cv.raw != nil {
		r := *cv.raw
		cv.raw = &r
	}
	return cv
}

// cloneTrivia deep-copies a trivia sidecar (nil stays nil).
func cloneTrivia(t *trivia) *trivia {
	if t == nil {
		return nil
	}
	c := trivia{
		leading:  append([]lead(nil), t.leading...),
		trailing: t.trailing,
		after:    append([]lead(nil), t.after...),
		inside:   append([]lead(nil), t.inside...),
	}
	return &c
}

// cloneSubtree deep-copies over's subtree at oi into d's arena under parent.
func (d *Document) cloneSubtree(over *Document, oi, parent int) int {
	src := over.arena[oi]
	cv := cloneValue(&src.value)
	var srcCopy *string
	if src.src != nil {
		s := *src.src
		srcCopy = &s
	}
	nd := nodeData{
		name:        src.name,
		nameSrc:     src.nameSrc,
		value:       cv,
		parent:      parent,
		line:        src.line,
		starList:    src.starList,
		starMixed:   src.starMixed,
		trivia:      cloneTrivia(src.trivia),
		blankBefore: src.blankBefore,
		srcSet:      src.srcSet,
		src:         srcCopy,
	}
	idx := len(d.arena)
	d.arena = append(d.arena, nd)
	for _, ok := range over.arena[oi].children {
		c := d.cloneSubtree(over, ok, idx)
		d.arena[idx].children = append(d.arena[idx].children, c)
	}
	return idx
}

// ---------------------------------------------------------------------------
// Coercion ("intelligent but safe"; Loose re-admits a closed list of tricks)
// ---------------------------------------------------------------------------

var currencyRunes = []rune{
	'$', '¢', '£', '¤', '¥', '₩', '₪', '₫', '€', '₭', '₮', '₱', '₲', '₴', '₹', '₺', '₼', '₽', '₾', '₿',
}

// stripCurrency is the remainder after a leading currency symbol, with the
// space a person writes after one taken off - `$ 1200` reached the int path's
// thousands branch, which trims, and the float path's shape test, which does
// not.
func stripCurrency(t string) string {
	r, size := utf8.DecodeRuneInString(t)
	for _, c := range currencyRunes {
		if r == c {
			return strings.TrimLeftFunc(t[size:], unicode.IsSpace)
		}
	}
	return t
}

func parseIntText(e *element, level Strictness) (int64, bool) {
	t := strings.TrimSpace(e.text)
	if level == Loose {
		t = stripCurrency(t)
	}
	// Plain decimal.
	body := stripSign(t)
	if body != "" && allDigits(body) {
		v, err := strconv.ParseInt(t, 10, 64)
		return v, err == nil
	}
	// Hex.
	neg := false
	hex := t
	if strings.HasPrefix(t, "-") {
		neg = true
		hex = t[1:]
	} else {
		hex = strings.TrimPrefix(t, "+")
	}
	if strings.HasPrefix(hex, "0x") || strings.HasPrefix(hex, "0X") {
		h := hex[2:]
		if h != "" && allHexDigits(h) {
			// Parse the magnitude as u64, then range-check against the sign, so the
			// negative math.MinInt64 magnitude (0x8000000000000000) reads like its
			// decimal spelling instead of overflowing a signed parse.
			m, err := strconv.ParseUint(h, 16, 64)
			if err != nil {
				return 0, false
			}
			if neg {
				if m == uint64(math.MaxInt64)+1 {
					return math.MinInt64, true
				}
				if m <= uint64(math.MaxInt64) {
					return -int64(m), true
				}
				return 0, false
			}
			if m <= uint64(math.MaxInt64) {
				return int64(m), true
			}
			return 0, false
		}
	}
	// Thousands separators, only inside quotes (bare commas are reserved).
	if e.quoted && strings.Contains(t, ",") {
		signBody := stripSign(t)
		groups := strings.Split(signBody, ",")
		wellFormed := len(groups) > 1 && groups[0] != "" && len(groups[0]) <= 3 && allDigits(groups[0])
		if wellFormed {
			for _, g := range groups[1:] {
				if len(g) != 3 || !allDigits(g) {
					wellFormed = false
					break
				}
			}
		}
		if wellFormed {
			v, err := strconv.ParseInt(strings.ReplaceAll(t, ",", ""), 10, 64)
			return v, err == nil
		}
	}
	// Loose: a float (including %) rounds, half away from zero.
	if level == Loose {
		if f, ok := parseFloatText(e, level); ok {
			r := math.Round(f)
			// math.MaxInt64 has no exact float64 - it rounds up to 2^63 - so
			// the top bound is 2^63 itself, exclusively. MinInt64 is exact.
			if r >= float64(math.MinInt64) && r < 9223372036854775808.0 {
				return int64(r), true
			}
		}
	}
	return 0, false
}

func floatShapeOK(t string) bool {
	body := stripSign(t)
	if body == "" {
		return false
	}
	mantissa := body
	if idx := strings.IndexAny(body, "eE"); idx >= 0 {
		mantissa = body[:idx]
		xb := stripSign(body[idx+1:])
		if xb == "" || !allDigits(xb) {
			return false
		}
	}
	intPart, fracPart := mantissa, ""
	if d := strings.IndexByte(mantissa, '.'); d >= 0 {
		intPart, fracPart = mantissa[:d], mantissa[d+1:]
	}
	if intPart == "" && fracPart == "" {
		return false
	}
	return allDigits(intPart) && allDigits(fracPart)
}

func parseFloatText(e *element, level Strictness) (float64, bool) {
	t := strings.TrimSpace(e.text)
	percent := false
	if level == Loose {
		t = stripCurrency(t)
		if inner, ok := strings.CutSuffix(t, "%"); ok {
			t = trimEndWS(inner)
			percent = true
		}
	}
	var v float64
	if floatShapeOK(t) {
		f, err := strconv.ParseFloat(t, 64)
		if err != nil {
			// Underflow keeps the reference's parse result (0). Overflow is an
			// infinity, which no double holds and no setter can write back:
			// BadType, like a text that is not a number at all.
			var ne *strconv.NumError
			if !errors.As(err, &ne) || ne.Err != strconv.ErrRange || math.IsInf(f, 0) {
				return 0, false
			}
		}
		v = f
	} else {
		// An integer is a valid float on read (incl. hex and quoted thousands).
		el := element{text: t, quoted: e.quoted}
		n, ok := parseIntTextNoLoose(&el)
		if !ok {
			return 0, false
		}
		v = float64(n)
	}
	if percent {
		v /= 100.0
	}
	return v, true
}

// parseIntTextNoLoose: integer forms only (no Loose float fallback) - used by
// the float path so the two can't recurse into each other.
func parseIntTextNoLoose(e *element) (int64, bool) {
	return parseIntText(e, Standard)
}

func parseBoolText(t string, level Strictness) (bool, bool) {
	s := asciiLower(strings.TrimSpace(t))
	switch s {
	case "true":
		return true, true
	case "false":
		return false, true
	}
	if level == Strict {
		return false, false
	}
	switch s {
	case "yes", "on", "1":
		return true, true
	case "no", "off", "0":
		return false, true
	}
	if level == Loose {
		switch s {
		case "t", "y", "enable", "enabled":
			return true, true
		case "f", "n", "disable", "disabled":
			return false, true
		}
	}
	return false, false
}

// ---------------------------------------------------------------------------
// Date/time (closed whitelist; shape match, then calendar validation)
// ---------------------------------------------------------------------------

var months = map[string]int{
	"jan": 1, "feb": 2, "mar": 3, "apr": 4, "may": 5, "jun": 6,
	"jul": 7, "aug": 8, "sep": 9, "oct": 10, "nov": 11, "dec": 12,
	"january": 1, "february": 2, "march": 3, "april": 4, "june": 6,
	"july": 7, "august": 8, "september": 9, "october": 10, "november": 11, "december": 12,
}

func monthFromName(s string) (int, bool) {
	m, ok := months[asciiLower(s)]
	return m, ok
}

func daysInMonth(y, m int) int {
	switch m {
	case 1, 3, 5, 7, 8, 10, 12:
		return 31
	case 4, 6, 9, 11:
		return 30
	case 2:
		if (y%4 == 0 && y%100 != 0) || y%400 == 0 {
			return 29
		}
		return 28
	}
	return 0
}

func validDate(y, m, d int) bool {
	return m >= 1 && m <= 12 && d >= 1 && d <= daysInMonth(y, m)
}

// The Atoi error discards through this date cluster are safe: every input is
// length-bounded and allDigits-checked first, so Atoi cannot fail on it.
func parseYear4(s string) (int, bool) {
	if len(s) != 4 || !allDigits(s) {
		return 0, false
	}
	y, _ := strconv.Atoi(s)
	return y, true
}

func parseNum2(s string) (int, bool) {
	if (len(s) != 1 && len(s) != 2) || !allDigits(s) {
		return 0, false
	}
	n, _ := strconv.Atoi(s)
	return n, true
}

func parseDatePart(s string) (y, m, d int, ok bool) {
	s = strings.TrimSpace(s)
	// Compact 8-digit YYYYMMDD.
	if len(s) == 8 && allDigits(s) {
		y, _ = strconv.Atoi(s[:4])
		m, _ = strconv.Atoi(s[4:6])
		d, _ = strconv.Atoi(s[6:8])
		if validDate(y, m, d) {
			return y, m, d, true
		}
		return 0, 0, 0, false
	}
	// Space-separated named-month forms; a comma may follow the day in "Mon DD, YYYY".
	toks := strings.Fields(s)
	if len(toks) == 3 {
		if mo, found := monthFromName(toks[0]); found {
			// The day is DD, like every other form's: a plain integer parse
			// takes a leading '+' and any number of leading zeros, which the
			// whitelist does not list and the delimited spellings refuse.
			dv, ok1 := parseNum2(strings.TrimSuffix(toks[1], ","))
			yv, ok2 := parseYear4(toks[2])
			if ok1 && ok2 && validDate(yv, mo, dv) {
				return yv, mo, dv, true
			}
			return 0, 0, 0, false
		}
		if mo, found := monthFromName(toks[1]); found {
			dv, ok1 := parseNum2(toks[0])
			yv, ok2 := parseYear4(toks[2])
			if ok1 && ok2 && validDate(yv, mo, dv) {
				return yv, mo, dv, true
			}
			return 0, 0, 0, false
		}
		return 0, 0, 0, false
	}
	if len(toks) != 1 {
		return 0, 0, 0, false
	}
	// Delimited forms: one of - / . used uniformly.
	var delim byte
	nDelims := 0
	for i := 0; i < len(s); i++ {
		if s[i] == '-' || s[i] == '/' || s[i] == '.' {
			if delim == 0 {
				delim = s[i]
			}
			nDelims++
		}
	}
	if delim == 0 {
		return 0, 0, 0, false
	}
	parts := strings.Split(s, string(rune(delim)))
	if len(parts) != 3 || parts[0] == "" || parts[1] == "" || parts[2] == "" {
		return 0, 0, 0, false
	}
	// The delimiter must be uniform: no other delimiter chars anywhere.
	if nDelims != 2 {
		return 0, 0, 0, false
	}
	if len(parts[0]) == 4 && allDigits(parts[0]) {
		// Year-first all-numeric.
		y, _ = strconv.Atoi(parts[0])
		mv, ok1 := parseNum2(parts[1])
		dv, ok2 := parseNum2(parts[2])
		if ok1 && ok2 && validDate(y, mv, dv) {
			return y, mv, dv, true
		}
		return 0, 0, 0, false
	}
	if mo, found := monthFromName(parts[0]); found {
		dv, ok1 := parseNum2(parts[1])
		yv, ok2 := parseYear4(parts[2])
		if ok1 && ok2 && validDate(yv, mo, dv) {
			return yv, mo, dv, true
		}
		return 0, 0, 0, false
	}
	if mo, found := monthFromName(parts[1]); found {
		dv, ok1 := parseNum2(parts[0])
		yv, ok2 := parseYear4(parts[2])
		if ok1 && ok2 && validDate(yv, mo, dv) {
			return yv, mo, dv, true
		}
		return 0, 0, 0, false
	}
	return 0, 0, 0, false // everything else (MM/DD/YYYY, 2-digit years, epoch) is rejected by decision
}

// parseTimePart: time with optional meridiem, fraction, zone:
// `H:MM[:SS[.f+]][ AM|PM][Z|+HH:MM]`.
func parseTimePart(s string) (h, mi, sec int, hasSec bool, frac string, zone *Zone, ok bool) {
	t := strings.TrimSpace(s)
	// Zone suffix first (only valid after a time).
	if rest, cut := strings.CutSuffix(t, "Z"); cut {
		zone = &Zone{Kind: ZoneUTC}
		t = trimEndWS(rest)
	} else if rest, cut := strings.CutSuffix(t, "z"); cut {
		zone = &Zone{Kind: ZoneUTC}
		t = trimEndWS(rest)
	} else if len(t) >= 6 {
		tail := t[len(t)-6:]
		sign := tail[0]
		if (sign == '+' || sign == '-') &&
			isASCIIDigit(tail[1]) && isASCIIDigit(tail[2]) && tail[3] == ':' &&
			isASCIIDigit(tail[4]) && isASCIIDigit(tail[5]) {
			hh := int(tail[1]-'0')*10 + int(tail[2]-'0')
			mm := int(tail[4]-'0')*10 + int(tail[5]-'0')
			if hh <= 23 && mm <= 59 {
				off := hh*60 + mm
				if sign == '-' {
					off = -off
				}
				zone = &Zone{Kind: ZoneOffset, OffsetMinutes: off}
				t = trimEndWS(t[:len(t)-6])
			}
		}
	}
	// Meridiem: mandatory minutes already implied by the H:MM shape; dotted
	// a.m. is rejected (the '.' fails the digit checks below).
	var meridiem *bool // nil = none, otherwise true = PM
	lower := asciiLower(t)
	if rest, cut := strings.CutSuffix(lower, "am"); cut {
		am := false
		meridiem = &am
		t = t[:len(trimEndWS(rest))]
	} else if rest, cut := strings.CutSuffix(lower, "pm"); cut {
		pm := true
		meridiem = &pm
		t = t[:len(trimEndWS(rest))]
	}
	t = trimEndWS(t)
	// Fraction: only after seconds, '.' delimiter, 1-9 digits.
	hms := t
	if idx := strings.IndexByte(t, '.'); idx >= 0 {
		f := t[idx+1:]
		if f == "" || len(f) > 9 || !allDigits(f) {
			return 0, 0, 0, false, "", nil, false
		}
		frac = f
		hms = t[:idx]
	}
	parts := strings.Split(hms, ":")
	if len(parts) < 2 || len(parts) > 3 {
		return 0, 0, 0, false, "", nil, false
	}
	if frac != "" && len(parts) != 3 {
		return 0, 0, 0, false, "", nil, false // fraction can only follow HH:MM:SS
	}
	hRaw, ok1 := parseNum2(parts[0])
	if !ok1 {
		return 0, 0, 0, false, "", nil, false
	}
	if len(parts[1]) != 2 {
		return 0, 0, 0, false, "", nil, false
	}
	mi, ok1 = parseNum2(parts[1])
	if !ok1 {
		return 0, 0, 0, false, "", nil, false
	}
	if len(parts) == 3 {
		if len(parts[2]) != 2 {
			return 0, 0, 0, false, "", nil, false
		}
		sec, ok1 = parseNum2(parts[2])
		if !ok1 {
			return 0, 0, 0, false, "", nil, false
		}
		hasSec = true
	}
	if mi > 59 || (hasSec && sec > 59) {
		return 0, 0, 0, false, "", nil, false
	}
	if meridiem == nil {
		if hRaw > 23 {
			return 0, 0, 0, false, "", nil, false
		}
		h = hRaw
	} else {
		if hRaw < 1 || hRaw > 12 {
			return 0, 0, 0, false, "", nil, false
		}
		switch {
		case !*meridiem && hRaw == 12:
			h = 0
		case !*meridiem:
			h = hRaw
		case hRaw == 12:
			h = 12
		default:
			h = hRaw + 12
		}
	}
	return h, mi, sec, hasSec, frac, zone, true
}

// datetimeReadsBack reports whether a datetime's canonical spelling reads
// back as the same value: the setter's inverse-of-the-read promise, checked
// by making the round trip.
func datetimeReadsBack(v DateTime) bool {
	back, ok := ParseDateTime(v.String())
	if !ok || (v.Zone == nil) != (back.Zone == nil) || (v.Zone != nil && *v.Zone != *back.Zone) {
		return false
	}
	v.Zone, back.Zone = nil, nil
	return v == back
}

// ParseDateTime is the whole-value date/time parse per the whitelist.
// ok=false means BadType.
func ParseDateTime(text string) (DateTime, bool) {
	t := strings.TrimSpace(text)
	if t == "" {
		return DateTime{}, false
	}
	if colon := strings.IndexByte(t, ':'); colon >= 0 {
		// Scan back over the 1-2 hour digits to find where the time starts.
		k := colon
		for k > 0 && isASCIIDigit(t[k-1]) && colon-k < 2 {
			k--
		}
		if k == colon {
			return DateTime{}, false // ':' with no hour digits before it
		}
		if k == 0 {
			// Time-only value.
			h, mi, sec, hasSec, frac, zone, ok := parseTimePart(t)
			if !ok {
				return DateTime{}, false
			}
			return DateTime{HasTime: true, Hour: h, Minute: mi, HasSeconds: hasSec, Second: sec, Frac: frac, Zone: zone}, true
		}
		// Combined: one separator char between date and time.
		sep, sepLen := utf8.DecodeLastRuneInString(t[:k])
		switch sep {
		case 'T', 't', ' ', '_', '-', '/', '.':
		default:
			return DateTime{}, false
		}
		y, mo, d, okd := parseDatePart(t[:k-sepLen])
		if !okd {
			return DateTime{}, false
		}
		h, mi, sec, hasSec, frac, zone, ok := parseTimePart(t[k:])
		if !ok {
			return DateTime{}, false
		}
		return DateTime{
			HasDate: true, Year: y, Month: mo, Day: d,
			HasTime: true, Hour: h, Minute: mi, HasSeconds: hasSec, Second: sec,
			Frac: frac, Zone: zone,
		}, true
	}
	// Date-only.
	y, mo, d, ok := parseDatePart(t)
	if !ok {
		return DateTime{}, false
	}
	return DateTime{HasDate: true, Year: y, Month: mo, Day: d}, true
}

// ---------------------------------------------------------------------------
// Accessor: typed reads
// ---------------------------------------------------------------------------

// nodeAt returns the single node at a path, or the failing status.
func (d *Document) nodeAt(path string) (int, Status) {
	r, ok := d.resolve(path)
	if !ok {
		return -1, NotFound
	}
	switch r.kind {
	case resNone:
		return -1, NotFound
	case resMany, resSlots:
		return -1, Multiple
	}
	return r.one, Good
}

// rawOf is a read's Raw: the verbatim source value text when the value came
// from one source line, else the display form (writer-built, stacked list, raw
// block - shapes with no one-line source spelling).
func (d *Document) rawOf(n int) string {
	if s := d.arena[n].src; s != nil {
		return *s
	}
	return d.arena[n].value.display()
}

func scalarElement(v *value) (*element, Status) {
	switch {
	case v.kind == vEmpty:
		return nil, Empty
	case v.kind == vRaw:
		return nil, BadType
	case len(v.els) == 1:
		return &v.els[0], Good
	}
	return nil, BadType // an array is not one scalar
}

func readScalar[T any](d *Document, path string, coerce func(*element) (T, bool)) Read[T] {
	var zero T
	n, st := d.nodeAt(path)
	if n < 0 {
		return Read[T]{Value: zero, Status: st}
	}
	v := &d.arena[n].value
	raw := d.rawOf(n)
	line := d.arena[n].line
	el, est := scalarElement(v)
	if el == nil {
		return Read[T]{Value: zero, Status: est, Raw: &raw}.at(line, false)
	}
	if val, ok := coerce(el); ok {
		return Read[T]{Value: val, Status: Good, Raw: &raw}.at(line, el.quoted)
	}
	return Read[T]{Value: zero, Status: BadType, Raw: &raw}.at(line, el.quoted)
}

// ReadInt is the full-tier integer read at path, coerced per the document's strictness.
func (d *Document) ReadInt(path string) Read[int64] {
	lvl := d.strictness
	return readScalar(d, path, func(e *element) (int64, bool) { return parseIntText(e, lvl) })
}

// ReadFloat is the full-tier float read at path, coerced per the document's strictness.
func (d *Document) ReadFloat(path string) Read[float64] {
	lvl := d.strictness
	return readScalar(d, path, func(e *element) (float64, bool) { return parseFloatText(e, lvl) })
}

// ReadBool is the full-tier bool read at path, coerced per the document's strictness.
func (d *Document) ReadBool(path string) Read[bool] {
	lvl := d.strictness
	return readScalar(d, path, func(e *element) (bool, bool) { return parseBoolText(e.text, lvl) })
}

// ReadDateTime is the full-tier datetime read at path.
func (d *Document) ReadDateTime(path string) Read[DateTime] {
	return readScalar(d, path, func(e *element) (DateTime, bool) { return ParseDateTime(e.text) })
}

// ReadString: any value reads as a string: a raw block yields its content, an
// array its canonical inline text. Escapes are applied.
func (d *Document) ReadString(path string) Read[string] {
	n, st := d.nodeAt(path)
	if n < 0 {
		return Read[string]{Status: st}
	}
	v := &d.arena[n].value
	raw := d.rawOf(n)
	line := d.arena[n].line
	switch {
	case v.kind == vEmpty:
		return Read[string]{Status: Empty, Raw: &raw}.at(line, false)
	case v.kind == vRaw:
		return Read[string]{Value: v.raw.content, Status: Good, Raw: &raw}.at(line, false)
	case len(v.els) == 1:
		return Read[string]{Value: applyEscapes(v.els[0].text), Status: Good, Raw: &raw}.at(line, v.els[0].quoted)
	}
	// Canonical inline form (quoting + escapes intact), so the string
	// re-parses to the same array - not the bare display join.
	parts := make([]string, len(v.els))
	for k := range v.els {
		parts[k] = emitElement(&v.els[k])
	}
	return Read[string]{Value: strings.Join(parts, ", "), Status: Good, Raw: &raw}.at(line, false)
}

// ReadRaw: raw-block content (verbatim). Non-block values are BadType.
func (d *Document) ReadRaw(path string) Read[string] {
	n, st := d.nodeAt(path)
	if n < 0 {
		return Read[string]{Status: st}
	}
	v := &d.arena[n].value
	raw := d.rawOf(n)
	line := d.arena[n].line
	switch v.kind {
	case vRaw:
		return Read[string]{Value: v.raw.content, Status: Good, Raw: &raw}.at(line, false)
	case vEmpty:
		return Read[string]{Status: Empty, Raw: &raw}.at(line, false)
	}
	return Read[string]{Status: BadType, Raw: &raw}.at(line, false)
}

// ReadRawInfo: the advisory info-string of a raw block ("" when absent).
func (d *Document) ReadRawInfo(path string) Read[string] {
	n, st := d.nodeAt(path)
	if n < 0 {
		return Read[string]{Status: st}
	}
	v := &d.arena[n].value
	raw := d.rawOf(n)
	line := d.arena[n].line
	if v.kind == vRaw {
		return Read[string]{Value: v.raw.info, Status: Good, Raw: &raw}.at(line, false)
	}
	return Read[string]{Status: BadType, Raw: &raw}.at(line, false)
}

func readArray[T any](d *Document, path string, coerce func(*element) (T, bool)) Read[[]T] {
	var zero T
	// Wildcard paths: one slot per instance, missing sub-paths keep their slot
	// (spec: never silently dropped). Each slot reads like a scalar of the
	// target type and records its own status; the aggregate is the worst one.
	r, ok := d.resolve(path)
	if !ok {
		return Read[[]T]{Value: []T{}, Status: NotFound}
	}
	switch r.kind {
	case resSlots:
		out := make([]T, 0, len(r.slots))
		sts := make([]Status, 0, len(r.slots))
		for _, slot := range r.slots {
			if slot == -1 {
				out = append(out, zero)
				sts = append(sts, NotFound)
				continue
			}
			if slot < 0 {
				out = append(out, zero)
				sts = append(sts, Multiple)
				continue
			}
			el, est := scalarElement(&d.arena[slot].value)
			if el == nil {
				out = append(out, zero)
				sts = append(sts, est)
				continue
			}
			val, cst := coerced(coerce, el)
			out = append(out, val)
			sts = append(sts, cst)
		}
		// No slots at all means the wildcard's parent is not there, so the
		// path did not resolve - Empty is for a node that is.
		status := NotFound
		if len(sts) > 0 {
			status = Good
			for _, s := range sts {
				if s > status {
					status = s
				}
			}
		}
		return Read[[]T]{Value: out, Status: status, Slots: sts}
	case resNone:
		return Read[[]T]{Value: []T{}, Status: NotFound}
	case resMany:
		return Read[[]T]{Value: []T{}, Status: Multiple}
	}
	v := &d.arena[r.one].value
	raw := d.rawOf(r.one)
	line := d.arena[r.one].line
	switch v.kind {
	case vEmpty:
		return Read[[]T]{Value: []T{}, Status: Empty, Raw: &raw}.at(line, false)
	case vRaw:
		return Read[[]T]{Value: []T{}, Status: BadType, Raw: &raw}.at(line, false)
	}
	out := make([]T, 0, len(v.els))
	sts := make([]Status, 0, len(v.els))
	status := Good
	for i := range v.els {
		val, cst := coerced(coerce, &v.els[i])
		out = append(out, val)
		sts = append(sts, cst)
		if cst > status {
			status = cst
		}
	}
	// A one-element cell has a single scalar element, so the flag means the
	// same thing here as on the scalar read of the same node.
	return Read[[]T]{Value: out, Status: status, Raw: &raw, Slots: sts}.at(line, len(v.els) == 1 && v.els[0].quoted)
}

// ReadIntArray is the full-tier integer-array read at path (per-slot statuses in Slots).
func (d *Document) ReadIntArray(path string) Read[[]int64] {
	lvl := d.strictness
	return readArray(d, path, func(e *element) (int64, bool) { return parseIntText(e, lvl) })
}

// ReadFloatArray is the full-tier float-array read at path.
func (d *Document) ReadFloatArray(path string) Read[[]float64] {
	lvl := d.strictness
	return readArray(d, path, func(e *element) (float64, bool) { return parseFloatText(e, lvl) })
}

// ReadBoolArray is the full-tier bool-array read at path.
func (d *Document) ReadBoolArray(path string) Read[[]bool] {
	lvl := d.strictness
	return readArray(d, path, func(e *element) (bool, bool) { return parseBoolText(e.text, lvl) })
}

// ReadDateTimeArray is the full-tier datetime-array read at path.
func (d *Document) ReadDateTimeArray(path string) Read[[]DateTime] {
	return readArray(d, path, func(e *element) (DateTime, bool) { return ParseDateTime(e.text) })
}

// ReadStringArray is the full-tier string-array read at path, escapes applied per element.
func (d *Document) ReadStringArray(path string) Read[[]string] {
	return readArray(d, path, func(e *element) (string, bool) { return applyEscapes(e.text), true })
}

// Full tier, status form: the value is only meaningful when the status is
// Good. Empty still comes back as non-Good here; use Read* to also get the
// empty value.

// GetInt is ReadInt reduced to (value, status).
func (d *Document) GetInt(path string) (int64, Status) {
	r := d.ReadInt(path)
	return r.Value, r.Status
}

// GetFloat is ReadFloat reduced to (value, status).
func (d *Document) GetFloat(path string) (float64, Status) {
	r := d.ReadFloat(path)
	return r.Value, r.Status
}

// GetBool is ReadBool reduced to (value, status).
func (d *Document) GetBool(path string) (bool, Status) {
	r := d.ReadBool(path)
	return r.Value, r.Status
}

// GetString is ReadString reduced to (value, status).
func (d *Document) GetString(path string) (string, Status) {
	r := d.ReadString(path)
	return r.Value, r.Status
}

// GetRaw is ReadRaw reduced to (value, status).
func (d *Document) GetRaw(path string) (string, Status) {
	r := d.ReadRaw(path)
	return r.Value, r.Status
}

// GetRawInfo is ReadRawInfo reduced to (value, status).
func (d *Document) GetRawInfo(path string) (string, Status) {
	r := d.ReadRawInfo(path)
	return r.Value, r.Status
}

// GetDateTime is ReadDateTime reduced to (value, status).
func (d *Document) GetDateTime(path string) (DateTime, Status) {
	r := d.ReadDateTime(path)
	return r.Value, r.Status
}

// The array forms of the same reduction. The status is the whole read's, so a
// partially-resolved array reports non-Good with the resolved slots still in
// the value; the per-slot statuses are the Read*Array tier's Slots.

// GetIntArray is ReadIntArray reduced to (value, status).
func (d *Document) GetIntArray(path string) ([]int64, Status) {
	r := d.ReadIntArray(path)
	return r.Value, r.Status
}

// GetFloatArray is ReadFloatArray reduced to (value, status).
func (d *Document) GetFloatArray(path string) ([]float64, Status) {
	r := d.ReadFloatArray(path)
	return r.Value, r.Status
}

// GetBoolArray is ReadBoolArray reduced to (value, status).
func (d *Document) GetBoolArray(path string) ([]bool, Status) {
	r := d.ReadBoolArray(path)
	return r.Value, r.Status
}

// GetStringArray is ReadStringArray reduced to (value, status).
func (d *Document) GetStringArray(path string) ([]string, Status) {
	r := d.ReadStringArray(path)
	return r.Value, r.Status
}

// GetDateTimeArray is ReadDateTimeArray reduced to (value, status).
func (d *Document) GetDateTimeArray(path string) ([]DateTime, Status) {
	r := d.ReadDateTimeArray(path)
	return r.Value, r.Status
}

// Convenience tier: one value, one call-site fallback, no status to inspect.
// The fallback is returned unless the read is Good (matching the reference's
// get_int(path).unwrap_or(def)), so an empty/missing/bad/ambiguous read can
// never masquerade as a real zero. Array forms fall back to the whole default
// array; per-slot substitution is the ReadIntArray tier or the CLI --default.

// GetIntOr is the integer at path, or def when the read is not Good.
func (d *Document) GetIntOr(path string, def int64) int64 {
	if r := d.ReadInt(path); r.Status == Good {
		return r.Value
	}
	return def
}

// GetFloatOr is the float at path, or def when the read is not Good.
func (d *Document) GetFloatOr(path string, def float64) float64 {
	if r := d.ReadFloat(path); r.Status == Good {
		return r.Value
	}
	return def
}

// GetBoolOr is the bool at path, or def when the read is not Good.
func (d *Document) GetBoolOr(path string, def bool) bool {
	if r := d.ReadBool(path); r.Status == Good {
		return r.Value
	}
	return def
}

// GetStringOr is the string at path, or def when the read is not Good.
func (d *Document) GetStringOr(path string, def string) string {
	if r := d.ReadString(path); r.Status == Good {
		return r.Value
	}
	return def
}

// GetRawOr is the raw-block content at path, or def when the read is not Good.
func (d *Document) GetRawOr(path string, def string) string {
	if r := d.ReadRaw(path); r.Status == Good {
		return r.Value
	}
	return def
}

// GetRawInfoOr is the raw block's info-string at path, or def when the read is
// not Good.
func (d *Document) GetRawInfoOr(path string, def string) string {
	if r := d.ReadRawInfo(path); r.Status == Good {
		return r.Value
	}
	return def
}

// GetDateTimeOr is the datetime at path, or def when the read is not Good.
func (d *Document) GetDateTimeOr(path string, def DateTime) DateTime {
	if r := d.ReadDateTime(path); r.Status == Good {
		return r.Value
	}
	return def
}

// GetIntArrayOr is the integer array at path, or def when the read is not Good.
func (d *Document) GetIntArrayOr(path string, def []int64) []int64 {
	if r := d.ReadIntArray(path); r.Status == Good {
		return r.Value
	}
	return def
}

// GetFloatArrayOr is the float array at path, or def when the read is not Good.
func (d *Document) GetFloatArrayOr(path string, def []float64) []float64 {
	if r := d.ReadFloatArray(path); r.Status == Good {
		return r.Value
	}
	return def
}

// GetBoolArrayOr is the bool array at path, or def when the read is not Good.
func (d *Document) GetBoolArrayOr(path string, def []bool) []bool {
	if r := d.ReadBoolArray(path); r.Status == Good {
		return r.Value
	}
	return def
}

// GetStringArrayOr is the string array at path, or def when the read is not Good.
func (d *Document) GetStringArrayOr(path string, def []string) []string {
	if r := d.ReadStringArray(path); r.Status == Good {
		return r.Value
	}
	return def
}

// GetDateTimeArrayOr is the datetime array at path, or def when the read is not Good.
func (d *Document) GetDateTimeArrayOr(path string, def []DateTime) []DateTime {
	if r := d.ReadDateTimeArray(path); r.Status == Good {
		return r.Value
	}
	return def
}

// ---------------------------------------------------------------------------
// Validator: schema-as-SHCL
// ---------------------------------------------------------------------------
// The schema is an ordinary parsed document: a flat list of `field: <path>`
// instances whose children are the constraints (closed vocabulary - see
// spec.md "Schema validation"). Validation reuses the accessor's path scan and
// the typed coercions, so document strictness composes for free. Schema faults
// (V09x) come first and the surviving constraints still check the document;
// the unknown-field sweep skips only when a fault cost a path spelling. One
// line-number space per result.

var schemaTypes = []string{
	"int",
	"float",
	"bool",
	"string",
	"datetime",
	"raw",
	"int-array",
	"float-array",
	"bool-array",
	"string-array",
	"datetime-array",
}

type allowedKind int

const (
	allowInts allowedKind = iota
	allowFloats
	allowBools
	allowDates
	allowStrings
)

// The allowed set, pre-coerced at schema-build time into the constraint's type
// space so per-node checks are a plain contains.
type allowedSet struct {
	kind   allowedKind
	ints   []int64
	floats []float64
	bools  []bool
	dates  []DateTime
	strs   []string
}

type constraint struct {
	path         string // as written in the schema; message text only
	segs         []segment
	ty           string // member of schemaTypes; "" = untyped
	required     bool
	allowed      *allowedSet
	minI         *int64
	maxI         *int64
	minF         *float64
	maxF         *float64
	repeat       *[2]uint64
	reopen       bool   // H002 suppressor only; validation ignores it
	inherits     string // fragment mounted at this path (subtree shape); "" = none
	inheritsLine int    // schema line of the `inherits` key, for V095
	// Generator-only (`shcl init`): validation ignores both.
	desc        *string // `desc`, a one-line description
	defaultText *string // `default`, emitted as an inline value
}

// An interpreted schema: the top-level constraints plus the named fragments
// their `inherits` keys can mount.
type schemaDef struct {
	cons  []constraint
	frags map[string][]constraint
	// False when a fault cost the schema a path spelling (unreadable `field:`
	// path, or a mount naming no declared fragment). Key-level faults keep
	// their entry's chain, so only these two classes can turn declared fields
	// into false unknowns - the sweep runs unless one of them happened.
	pathsComplete bool
}

// coerced is one slot of an array read: the coerced value, or the type's zero
// with BadType.
func coerced[T any](coerce func(*element) (T, bool), el *element) (T, Status) {
	if v, ok := coerce(el); ok {
		return v, Good
	}
	var zero T
	return zero, BadType
}

func vdiag(out *[]Diagnostic, line int, code, msg string) {
	*out = append(*out, Diagnostic{Line: line, Severity: SeverityError, Message: msg, Code: code})
}

// singleText is one scalar constraint value (escapes applied), or not.
func singleText(v *value) (string, bool) {
	if v.kind == vCell && len(v.els) == 1 {
		return applyEscapes(v.els[0].text), true
	}
	return "", false
}

// dtEqual compares datetimes field-wise (Zone is a pointer, so == would
// compare identity, not value).
func dtEqual(a, b DateTime) bool {
	if (a.Zone == nil) != (b.Zone == nil) {
		return false
	}
	if a.Zone != nil && *a.Zone != *b.Zone {
		return false
	}
	a.Zone, b.Zone = nil, nil
	return a == b
}

// buildSchema interprets a parsed schema document into constraints and
// fragments, plus any schema faults (V09x, schema-file lines). Whatever parsed
// cleanly is kept even when faults are present - a broken key drops that key,
// a broken field drops that field - so a caller can still check the document
// against the surviving constraints.
func buildSchema(schema *Document) (schemaDef, []Diagnostic) {
	var faults []Diagnostic
	var cons []constraint
	frags := map[string][]constraint{}
	pathsComplete := true
	for _, f := range schema.arena[root].children {
		node := &schema.arena[f]
		switch node.name {
		case "field":
			if c, ok := parseField(schema, f, &faults); ok {
				cons = append(cons, c)
			} else {
				pathsComplete = false
			}
		case "fragment":
			name, ok := singleText(&node.value)
			if !ok || name == "" {
				vdiag(&faults, node.line, "V094", "bad schema fragment")
				continue
			}
			if _, dup := frags[name]; dup {
				vdiag(&faults, node.line, "V094", fmt.Sprintf("bad schema fragment '%s': duplicate", name))
				continue
			}
			var fcs []constraint
			for _, k := range schema.arena[f].children {
				kid := &schema.arena[k]
				if kid.name == "field" {
					if c, ok := parseField(schema, k, &faults); ok {
						fcs = append(fcs, c)
					} else {
						pathsComplete = false
					}
				} else {
					vdiag(&faults, kid.line, "V094", fmt.Sprintf("bad schema fragment '%s': unknown key '%s'", name, kid.name))
				}
			}
			frags[name] = fcs
		default:
			vdiag(&faults, node.line, "V090", fmt.Sprintf("unknown schema key '%s'", node.name))
		}
	}
	// Every mount must name a declared fragment; cycles (self or mutual) are
	// legal - expansion is demand-driven against a finite document.
	checkMount := func(c *constraint) {
		if c.inherits == "" {
			return
		}
		if _, ok := frags[c.inherits]; !ok {
			vdiag(&faults, c.inheritsLine, "V095", fmt.Sprintf("unknown schema fragment '%s'", c.inherits))
			pathsComplete = false
		}
	}
	for i := range cons {
		checkMount(&cons[i])
	}
	for _, fcs := range frags {
		for i := range fcs {
			checkMount(&fcs[i])
		}
	}
	// One constraint per line in practice, so line order = file order.
	sort.SliceStable(faults, func(i, j int) bool { return faults[i].Line < faults[j].Line })
	return schemaDef{cons: cons, frags: frags, pathsComplete: pathsComplete}, faults
}

// parseField turns one `field:` instance (top-level or inside a fragment) into
// a constraint. ok=false = faults were reported and the constraint is dropped.
func parseField(schema *Document, f int, faults *[]Diagnostic) (constraint, bool) {
	node := &schema.arena[f]
	path, ok := singleText(&node.value)
	if !ok {
		vdiag(faults, node.line, "V093", "bad schema path")
		return constraint{}, false
	}
	scan, err := scanLookup(path)
	if err != nil || scan.valueText != nil {
		vdiag(faults, node.line, "V093", fmt.Sprintf("bad schema path: %s", path))
		return constraint{}, false
	}
	c := constraint{path: path, segs: scan.segments}
	// Deferred so `min: 1` may precede `type: int` in the file.
	var required *bool
	reopenSeen := false
	allowedAt := -1
	minAt := -1
	maxAt := -1
	for _, k := range schema.arena[f].children {
		kid := &schema.arena[k]
		if kid.value.isEmpty() {
			continue // dangling key: treated as absent
		}
		switch kid.name {
		case "type":
			t, ok := singleText(&kid.value)
			if ok {
				t = asciiLower(t)
			}
			switch {
			case ok && containsString(schemaTypes, t):
				if c.ty != "" {
					vdiag(faults, kid.line, "V092", "bad schema constraint 'type'")
				} else {
					c.ty = t
				}
			case ok:
				vdiag(faults, kid.line, "V091", fmt.Sprintf("unknown schema type '%s'", t))
			default:
				vdiag(faults, kid.line, "V092", "bad schema constraint 'type'")
			}
		case "required":
			t, ok := singleText(&kid.value)
			var b bool
			if ok {
				b, ok = parseBoolText(t, Standard)
			}
			if ok && required == nil {
				required = &b
			} else {
				vdiag(faults, kid.line, "V092", "bad schema constraint 'required'")
			}
		// Consumed by the H002 suppressor; validation itself ignores it, but
		// a bad value still faults so a typo cannot silently disavow nothing.
		case "reopen":
			t, ok := singleText(&kid.value)
			var b bool
			if ok {
				b, ok = parseBoolText(t, Standard)
			}
			if ok && !reopenSeen {
				reopenSeen = true
				c.reopen = b
			} else {
				vdiag(faults, kid.line, "V092", "bad schema constraint 'reopen'")
			}
		case "allowed":
			if kid.value.kind == vCell && allowedAt < 0 {
				allowedAt = k
			} else {
				vdiag(faults, kid.line, "V092", "bad schema constraint 'allowed'")
			}
		case "min":
			if kid.value.kind == vCell && len(kid.value.els) == 1 && minAt < 0 {
				minAt = k
			} else {
				vdiag(faults, kid.line, "V092", "bad schema constraint 'min'")
			}
		case "max":
			if kid.value.kind == vCell && len(kid.value.els) == 1 && maxAt < 0 {
				maxAt = k
			} else {
				vdiag(faults, kid.line, "V092", "bad schema constraint 'max'")
			}
		case "repeat":
			if kid.value.kind == vCell && c.repeat == nil && (len(kid.value.els) == 1 || len(kid.value.els) == 2) {
				lo, okLo := parseIndex(kid.value.els[0].text)
				hi, okHi := parseIndex(kid.value.els[len(kid.value.els)-1].text)
				if okLo && okHi && lo <= hi {
					c.repeat = &[2]uint64{lo, hi}
				} else {
					vdiag(faults, kid.line, "V092", "bad schema constraint 'repeat'")
				}
			} else {
				vdiag(faults, kid.line, "V092", "bad schema constraint 'repeat'")
			}
		case "inherits":
			t, ok := singleText(&kid.value)
			if ok && t != "" && c.inherits == "" {
				c.inherits = t
				c.inheritsLine = kid.line
			} else {
				vdiag(faults, kid.line, "V092", "bad schema constraint 'inherits'")
			}
		// Generator-only (`shcl init`); validation ignores both. First
		// occurrence wins (a merged schema could carry two).
		case "desc":
			if c.desc == nil {
				if t, ok := singleText(&kid.value); ok {
					c.desc = &t
				}
			}
		case "default":
			if c.defaultText == nil {
				if t, ok := emitValueInline(&kid.value); ok {
					c.defaultText = &t
				}
			}
		default:
			vdiag(faults, kid.line, "V090", fmt.Sprintf("unknown schema key '%s'", kid.name))
		}
	}
	if required != nil {
		c.required = *required
	}
	base := strings.TrimSuffix(c.ty, "-array")
	if base == "" {
		base = "string"
	}
	if allowedAt >= 0 {
		kid := &schema.arena[allowedAt]
		els := kid.value.els
		// Schema values are read at Standard; only the document's values
		// coerce at the document's strictness.
		set := &allowedSet{}
		ok := true
		switch base {
		case "int":
			set.kind = allowInts
			for i := range els {
				v, o := parseIntText(&els[i], Standard)
				if !o {
					ok = false
					break
				}
				set.ints = append(set.ints, v)
			}
		case "float":
			set.kind = allowFloats
			for i := range els {
				v, o := parseFloatText(&els[i], Standard)
				if !o {
					ok = false
					break
				}
				set.floats = append(set.floats, v)
			}
		case "bool":
			set.kind = allowBools
			for i := range els {
				v, o := parseBoolText(els[i].text, Standard)
				if !o {
					ok = false
					break
				}
				set.bools = append(set.bools, v)
			}
		case "datetime":
			set.kind = allowDates
			for i := range els {
				v, o := ParseDateTime(els[i].text)
				if !o {
					ok = false
					break
				}
				set.dates = append(set.dates, v)
			}
		case "raw":
			ok = false // a raw body has no element space to enumerate
		default:
			set.kind = allowStrings
			for i := range els {
				set.strs = append(set.strs, applyEscapes(els[i].text))
			}
		}
		if ok {
			c.allowed = set
		} else {
			vdiag(faults, kid.line, "V092", "bad schema constraint 'allowed'")
		}
	}
	for _, mm := range []struct {
		at    int
		isMin bool
	}{{minAt, true}, {maxAt, false}} {
		if mm.at < 0 {
			continue
		}
		kid := &schema.arena[mm.at]
		el := &kid.value.els[0]
		key := "max"
		if mm.isMin {
			key = "min"
		}
		switch base {
		case "int":
			if v, ok := parseIntText(el, Standard); ok {
				if mm.isMin {
					c.minI = &v
				} else {
					c.maxI = &v
				}
			} else {
				vdiag(faults, kid.line, "V092", fmt.Sprintf("bad schema constraint '%s'", key))
			}
		case "float":
			if v, ok := parseFloatText(el, Standard); ok {
				if mm.isMin {
					c.minF = &v
				} else {
					c.maxF = &v
				}
			} else {
				vdiag(faults, kid.line, "V092", fmt.Sprintf("bad schema constraint '%s'", key))
			}
		default:
			vdiag(faults, kid.line, "V092", fmt.Sprintf("bad schema constraint '%s'", key))
		}
	}
	return c, true
}

func containsString(xs []string, s string) bool {
	for _, x := range xs {
		if x == s {
			return true
		}
	}
	return false
}

// emitValueInline re-emits a schema `default`/`allowed` value as an inline value
// (minimal quoting, array elements joined by ", "). ok=false for empty or raw -
// neither has a usable one-line form. Used by the generator, not the validator.
func emitValueInline(v *value) (string, bool) {
	if v.kind != vCell {
		return "", false
	}
	parts := make([]string, len(v.els))
	for i := range v.els {
		parts[i] = emitElement(&v.els[i])
	}
	return strings.Join(parts, ", "), true
}

// ---------------------------------------------------------------------------
// Schema-driven generation: a schema + the Writer -> a commented starter config.
// ---------------------------------------------------------------------------

func allowedJoin(a *allowedSet) string {
	var parts []string
	switch a.kind {
	case allowInts:
		for _, x := range a.ints {
			parts = append(parts, strconv.FormatInt(x, 10))
		}
	case allowFloats:
		for _, x := range a.floats {
			parts = append(parts, FormatFloat(x))
		}
	case allowBools:
		for _, x := range a.bools {
			if x {
				parts = append(parts, "true")
			} else {
				parts = append(parts, "false")
			}
		}
	case allowDates:
		for _, x := range a.dates {
			parts = append(parts, x.String())
		}
	case allowStrings:
		parts = append(parts, a.strs...)
	}
	return strings.Join(parts, ", ")
}

// genAnnotation is the `# type, ...` line summarizing a constraint, ASCII only.
func genAnnotation(c *constraint, tyname string) string {
	parts := []string{tyname}
	switch {
	case c.allowed != nil:
		parts = append(parts, "one of: "+allowedJoin(c.allowed))
	case c.minI != nil || c.maxI != nil:
		switch {
		case c.minI != nil && c.maxI != nil:
			parts = append(parts, fmt.Sprintf("%d-%d", *c.minI, *c.maxI))
		case c.minI != nil:
			parts = append(parts, fmt.Sprintf(">= %d", *c.minI))
		default:
			parts = append(parts, fmt.Sprintf("<= %d", *c.maxI))
		}
	case c.minF != nil || c.maxF != nil:
		switch {
		case c.minF != nil && c.maxF != nil:
			parts = append(parts, FormatFloat(*c.minF)+"-"+FormatFloat(*c.maxF))
		case c.minF != nil:
			parts = append(parts, ">= "+FormatFloat(*c.minF))
		default:
			parts = append(parts, "<= "+FormatFloat(*c.maxF))
		}
	}
	if c.repeat != nil {
		if c.repeat[0] == c.repeat[1] {
			parts = append(parts, fmt.Sprintf("repeat %d", c.repeat[0]))
		} else {
			parts = append(parts, fmt.Sprintf("repeat %d-%d", c.repeat[0], c.repeat[1]))
		}
	}
	if c.required {
		parts = append(parts, "required")
	}
	return strings.Join(parts, ", ")
}

// genDefaultText: a default carrying a literal newline cannot sit on a value
// line; the quoted escaped spelling reads back to the same string.
func genDefaultText(v string) string {
	if !strings.Contains(v, "\n") {
		return v
	}
	var s strings.Builder
	s.WriteByte('"')
	for _, ch := range v {
		switch ch {
		case '\\':
			s.WriteString("\\\\")
		case '"':
			s.WriteString("\\\"")
		case '\n':
			s.WriteString("\\n")
		case '\t':
			s.WriteString("\\t")
		default:
			s.WriteRune(ch)
		}
	}
	s.WriteByte('"')
	return s.String()
}

// Generate emits a commented, typed starter config from a schema (`shcl init
// --schema`). Paths that must exist (required, or a repeat lower bound of 1+)
// are live (their `default`, or an empty value); optional paths are commented
// out so the file is valid and minimal as-is. A must-exist wildcard path whose
// parent gets materialized by another live line is generated too, in dotted
// form - otherwise the file would fail the very schema that produced it - and
// remaining wildcard or `[#N]` paths (which cannot be materialized) are listed
// in a trailing comment block. The output always loads clean and validates
// clean against its schema, except a repeat lower bound of 2+ (identical
// generated lines would merge, so the shortfall is reported). The promise is
// checked against the finished text, so a schema whose own `default` breaks its
// field's constraints is a fault (V097) instead of a starter config that fails
// the first time it is checked. A footer naming
// the format and pointing at the spec is written last unless noBanner; the
// flag is negative so leaving it alone writes the footer. faults != nil =
// schema faults (V09x), same as Validate / check --schema.
func Generate(schema *Document, noBanner bool) (string, []Diagnostic) {
	// Generation lays the whole schema out, so unlike validation it has no
	// safe partial mode: any fault fails it.
	def, faults := buildSchema(schema)
	if len(faults) > 0 {
		return "", faults
	}
	cons, cuts := expandMounts(&def)
	if len(cons) > genMaxFields {
		msg := fmt.Sprintf("schema expands past %d fields; fragments mounted at more than one path multiply", genMaxFields)
		return "", []Diagnostic{{Line: 0, Severity: SeverityError, Message: msg, Code: "V096"}}
	}
	mustExist := func(c *constraint) bool {
		return c.required || (c.repeat != nil && c.repeat[0] >= 1)
	}
	hasWild := func(c *constraint) bool {
		for _, s := range c.segs {
			if s.sel != nil && s.sel.kind == selWildcard {
				return true
			}
		}
		return false
	}
	// `[#N]` needs a pre-existing instance and its `#` would start a comment on
	// a binding line; a newline inside a selector has no one-line spelling,
	// since the value emitter never escapes one. Both go to the trailing note
	// instead of emitting a broken line. A path deeper than a document may nest
	// cannot be generated either: the line would draw E016 on the way back in.
	// A newline in a NAME is writable: names are stored escape-resolved and the
	// name escaper spells one `\n`.
	unwritable := func(c *constraint) bool {
		if len(c.segs) > MaxDepth {
			return true
		}
		for _, s := range c.segs {
			if (s.sel != nil && s.sel.kind == selByIndex) || s.star {
				return true
			}
			if s.sel != nil && s.sel.kind == selByValue && strings.Contains(s.sel.value, "\n") {
				return true
			}
		}
		return false
	}
	// Live concrete paths materialize instances; decide which must-exist
	// wildcards get filled (their first-wildcard parent chain is a prefix of
	// some live path). Fixpoint: a fill can materialize another's parent.
	namesOf := func(segs []segment) []string {
		names := make([]string, len(segs))
		for j, s := range segs {
			names[j] = s.name
		}
		return names
	}
	isPrefix := func(p, parent []string) bool {
		if len(p) < len(parent) {
			return false
		}
		for j := range parent {
			if p[j] != parent[j] {
				return false
			}
		}
		return true
	}
	var live [][]string
	for i := range cons {
		c := &cons[i]
		if !hasWild(c) && !unwritable(c) && mustExist(c) {
			live = append(live, namesOf(c.segs))
		}
	}
	fill := make([]bool, len(cons))
	for {
		changed := false
		for i := range cons {
			c := &cons[i]
			if fill[i] || !hasWild(c) || unwritable(c) || !mustExist(c) {
				continue
			}
			k := 0
			for j, s := range c.segs {
				if s.sel != nil && s.sel.kind == selWildcard {
					k = j
					break
				}
			}
			parent := namesOf(c.segs[:k+1])
			// A wildcard in the last segment needs no other line to
			// materialize its parent: the line generated from it is that
			// instance.
			if k+1 == len(c.segs) {
				fill[i] = true
				live = append(live, namesOf(c.segs))
				changed = true
				continue
			}
			for _, p := range live {
				if isPrefix(p, parent) {
					fill[i] = true
					live = append(live, namesOf(c.segs))
					changed = true
					break
				}
			}
		}
		if !changed {
			break
		}
	}
	// A live line with a value materializes an instance carrying that value,
	// and a dotted child names the empty-valued instance instead - so `srv:
	// web` followed by `srv.port:` is two `srv` nodes, and the child never
	// lands where the schema looks. Any line under such a parent selects it
	// by its value: `srv[web].port:`.
	// A filled wildcard emits a valued line of its own, so it belongs here too.
	parentValues := map[string]string{}
	for i := range cons {
		c := &cons[i]
		if (!hasWild(c) || fill[i]) && !unwritable(c) && mustExist(c) && c.defaultText != nil {
			parentValues[namesKey(namesOf(c.segs))] = *c.defaultText
		}
	}
	// A path that cannot be written at all belongs in the trailing note, but one
	// that must exist can never be satisfied from there: the self-check would
	// then report the document as missing a path, which points at the config
	// rather than at the schema line that cannot be generated. A repeat lower
	// bound of 2 or more is the one documented shortfall - the line is emitted
	// once and the count reported - so it is not this fault.
	for i := range cons {
		c := &cons[i]
		cannotSatisfy := c.required || (c.repeat != nil && c.repeat[0] == 1)
		if cannotSatisfy && unwritable(c) && !hasWild(c) {
			msg := "required path cannot be generated: " + strings.ReplaceAll(c.path, "\n", "\\n")
			return "", []Diagnostic{{Line: 0, Severity: SeverityError, Message: msg, Code: "V097"}}
		}
	}
	var b strings.Builder
	var wild [][2]string
	// Dropping a trailing `[*]` can render the same line a concrete sibling
	// already wrote; the first spelling wins.
	emitted := map[string]bool{}
	first := true
	for i := range cons {
		c := &cons[i]
		tyname := c.ty
		if tyname == "" {
			tyname = "any"
		}
		if unwritable(c) || (hasWild(c) && !fill[i]) {
			wild = append(wild, [2]string{strings.ReplaceAll(c.path, "\n", "\\n"), tyname})
			continue
		}
		// A filled wildcard emits in dotted form, targeting the materialized
		// instance - by its value when the materializing line carries one.
		// Rebuilt from the parsed segments, not by cutting text out of the
		// path: the same path can be written several ways, and only the
		// segments say what it means. Otherwise the schema's own spelling.
		underValuedParent := false
		for k := 1; k < len(c.segs); k++ {
			if _, ok := parentValues[namesKey(namesOf(c.segs[:k]))]; ok && c.segs[k-1].sel == nil {
				underValuedParent = true
				break
			}
		}
		// A name carrying a newline has no verbatim spelling on a binding line;
		// the segment renderer escapes it, so such a path goes through there
		// whether or not it was filled.
		path := c.path
		if fill[i] || underValuedParent || strings.Contains(c.path, "\n") {
			path = genPathText(c.segs, parentValues)
		}
		if emitted[path] {
			continue
		}
		emitted[path] = true
		if !first {
			b.WriteByte('\n')
		}
		first = false
		if c.desc != nil {
			for _, line := range strings.Split(*c.desc, "\n") {
				b.WriteString("# ")
				b.WriteString(line)
				b.WriteByte('\n')
			}
		}
		b.WriteString("# ")
		// The annotation is a comment: a newline smuggled in via an allowed
		// string value must not break out of it.
		b.WriteString(strings.ReplaceAll(genAnnotation(c, tyname), "\n", "\\n"))
		b.WriteByte('\n')
		prefix := "#"
		if mustExist(c) {
			prefix = ""
		}
		if c.defaultText != nil {
			fmt.Fprintf(&b, "%s%s: %s\n", prefix, path, genDefaultText(*c.defaultText))
		} else {
			fmt.Fprintf(&b, "%s%s:\n", prefix, path)
		}
	}
	// Cycle-cut mounts last: their "type" column names the fragment that
	// belongs at the path.
	wild = append(wild, cuts...)
	if len(wild) > 0 {
		if !first {
			b.WriteByte('\n')
		}
		b.WriteString("# Paths needing an instance name (not generated):\n")
		for _, w := range wild {
			fmt.Fprintf(&b, "#   %s   %s\n", w[0], w[1])
		}
	}
	if !noBanner {
		if b.Len() > 0 {
			b.WriteByte('\n')
		}
		b.WriteString(genBanner)
	}
	// The output promises to validate clean against the schema that produced
	// it, so check that here rather than trusting each branch above. A
	// `default` outside its own field's constraints is the schema's fault, and
	// the author should hear about it instead of getting a starter config that
	// fails the first time it is checked. The one sanctioned shortfall is a
	// V007 for a repeat lower bound of 2+: generating that many identical
	// lines merges them into one. A lower bound of 1 is a must-exist path
	// like any other, so its V007 is a fault.
	text := b.String()
	var bad []Diagnostic
	for _, d := range Parse(text).Validate(schema) {
		if d.Severity == SeverityError && !(d.Code == "V007" && v007Sanctioned(d.Message)) {
			bad = append(bad, Diagnostic{
				Line:     0,
				Severity: SeverityError,
				Code:     "V097",
				Message:  "generated value fails the schema that produced it: " + d.Message,
			})
		}
	}
	if len(bad) > 0 {
		return "", bad
	}
	return text, nil
}

// genMaxFields is the ceiling on how many fields one schema may expand to.
// Fragments that mount each other at more than one path multiply, so a short
// schema can otherwise ask for more output than the machine can hold; past
// this the generator reports a schema fault rather than running until
// something breaks.
const genMaxFields = 10000

// genBanner is the footer telling whoever opens the generated file what the
// format is and where its spec lives. It is output, so every binding emits
// these bytes exactly; the Legal line names SHCL as its subject so it cannot
// be read as a claim over the config it sits in.
const genBanner = "#\n" +
	"# This config file format is SHCL.\n" +
	"# \"Simple Hierarchical Config Language\"\n" +
	"#    Home     https://github.com/jim-collier/shcl\n" +
	"#    Syntax   https://github.com/jim-collier/shcl/blob/main/project/spec.md\n" +
	"#    Legal    SHCL is Copyright © 2026 Jim Collier. License: MIT. No warranty.\n" +
	"#\n"

// v007Sanctioned reports whether a V007 from the self-check is the sanctioned
// kind: its message ends `: N not in LO..HI`, and LO is 2 or more.
func v007Sanctioned(message string) bool {
	i := strings.LastIndex(message, " not in ")
	if i < 0 {
		return false
	}
	lo, _, _ := strings.Cut(message[i+len(" not in "):], "..")
	n, err := strconv.ParseUint(lo, 10, 64)
	return err == nil && n >= 2
}

// namesKey joins segment names for the parent-value map, each length-prefixed
// so no name content can make two different prefixes read the same.
func namesKey(names []string) string {
	var b strings.Builder
	for _, n := range names {
		fmt.Fprintf(&b, "%d:%s", len(n), n)
	}
	return b.String()
}

// genSelectorText is a default's spelling inside a `[value]` selector: the
// value-side text as is, except that a bracket or a backslash would end or
// escape the selector, so those go quoted (the selector matches on the
// escaped display, so the quoted spelling finds the bare value).
func genSelectorText(v string) string {
	if strings.Contains(v, "\n") {
		return genDefaultText(v)
	}
	// The scanner reads a bare selector body as an index when it is all digits
	// (with an optional sign or `#`), and as a wildcard when it is `*`, so a
	// default of that shape has to be quoted or the line names an instance
	// that is not there.
	body := strings.TrimSpace(v)
	_, isIndex := parseIndex(body)
	if !isIndex {
		_, isIndex = hashIndex(body)
	}
	readsAsSelector := body == "*" || isIndex
	if (strings.ContainsAny(v, "[]\\") || readsAsSelector) && !quotedShape(v) {
		return quoteText(v)
	}
	return v
}

// genPathText renders parsed segments back as a dotted path, dropping
// wildcard selectors (a generated line targets the one instance it
// materializes) and quoting a name that needs it, so the result is a path the
// scanner reads back the same. A segment whose prefix names a live line
// carrying a value selects that instance by the value, in place of a wildcard
// or a bare name.
func genPathText(segs []segment, parentValues map[string]string) string {
	var out strings.Builder
	names := make([]string, 0, len(segs))
	for i, s := range segs {
		if i > 0 {
			out.WriteByte('.')
		}
		if s.star {
			out.WriteByte('*')
		} else {
			out.WriteString(emitName(s.name))
		}
		names = append(names, s.name)
		if v, ok := parentValues[namesKey(names)]; ok && i+1 < len(segs) && (s.sel == nil || s.sel.kind != selByValue) {
			out.WriteByte('[')
			out.WriteString(genSelectorText(v))
			out.WriteByte(']')
			continue
		}
		if s.sel != nil {
			switch s.sel.kind {
			case selByValue:
				out.WriteByte('[')
				if s.sel.quoted {
					out.WriteString(quoteText(s.sel.value))
				} else {
					out.WriteString(s.sel.value)
				}
				out.WriteByte(']')
			case selByIndex:
				fmt.Fprintf(&out, "[#%d]", s.sel.index)
			case selWildcard:
			}
		}
	}
	return out.String()
}

// expandMounts inlines every fragment mount into a flat constraint list,
// depth-first in schema order, each field's path and segments prefixed by its
// mount's. A mount whose fragment is already expanding (a cycle) stops there
// and is returned as (path, fragment name) for the trailing not-generated
// block.
func expandMounts(def *schemaDef) ([]constraint, [][2]string) {
	var out []constraint
	var cuts [][2]string
	var stack []string
	var walk func(list []constraint, atPath string, atSegs []segment, mounted bool)
	walk = func(list []constraint, atPath string, atSegs []segment, mounted bool) {
		for i := range list {
			cc := list[i]
			if mounted {
				cc.path = atPath + "." + list[i].path
				// Fresh backing array per clone: appending to a shared one
				// would let sibling clones stomp each other's segments.
				segs := make([]segment, 0, len(atSegs)+len(list[i].segs))
				segs = append(segs, atSegs...)
				segs = append(segs, list[i].segs...)
				cc.segs = segs
			}
			path := cc.path
			segs := cc.segs
			if len(out) > genMaxFields {
				return
			}
			out = append(out, cc)
			if fr := list[i].inherits; fr != "" {
				onStack := false
				for _, x := range stack {
					if x == fr {
						onStack = true
						break
					}
				}
				// A chain long enough to outrun the stack, or a mount that
				// re-enters, stops here and is noted instead of expanded.
				if onStack || len(stack) >= MaxDepth {
					cuts = append(cuts, [2]string{strings.ReplaceAll(path, "\n", "\\n"), fr})
				} else if fcs, ok := def.frags[fr]; ok {
					stack = append(stack, fr)
					walk(fcs, path, segs, true)
					stack = stack[:len(stack)-1]
				}
			}
		}
	}
	walk(def.cons, "", nil, false)
	return out, cuts
}

// editDistance is the Levenshtein distance capped at cap, for the "did you
// mean" prose (never the code): anything past the cap comes back as cap + 1.
// Only the band |i - j| <= cap of the table is computed, so a pair costs
// linear time in the names' length, and a length gap past the cap needs no
// table at all.
func editDistance(a, b string, cap int) int {
	ar := []rune(a)
	br := []rune(b)
	inf := cap + 1
	if absDiff(len(ar), len(br)) > cap {
		return inf
	}
	prev := make([]int, len(br)+1)
	cur := make([]int, len(br)+1)
	for j := range prev {
		prev[j] = minInt(j, inf)
		cur[j] = inf
	}
	for i := 1; i <= len(ar); i++ {
		cur[0] = minInt(i, inf)
		lo := maxInt(i-cap, 1)
		hi := minInt(i+cap, len(br))
		if lo > 1 {
			cur[lo-1] = inf
		}
		rowMin := cur[0]
		for j := lo; j <= hi; j++ {
			cost := 1
			if ar[i-1] == br[j-1] {
				cost = 0
			}
			cur[j] = minInt(min3(prev[j]+1, cur[j-1]+1, prev[j-1]+cost), inf)
			rowMin = minInt(rowMin, cur[j])
		}
		if hi < len(br) {
			cur[hi+1] = inf
		}
		// No cell in a later row can come back under this row's minimum.
		if rowMin > cap {
			return inf
		}
		prev, cur = cur, prev
	}
	return prev[len(br)]
}

func absDiff(a, b int) int {
	if a > b {
		return a - b
	}
	return b - a
}

func minInt(a, b int) int {
	if a < b {
		return a
	}
	return b
}

func maxInt(a, b int) int {
	if a > b {
		return a
	}
	return b
}

func min3(a, b, c int) int {
	if b < a {
		a = b
	}
	if c < a {
		a = c
	}
	return a
}

// Validate checks this document against a schema document (itself plain SHCL -
// spec.md "Schema validation"). An empty result means the document conforms.
// Diagnostic lines are document lines (0 = document scope); schema faults
// (V09x, schema-file lines) come first, and the surviving constraints still
// check the document. The unknown-field sweep runs too, unless a fault cost
// the schema a path spelling (an unreadable `field:` path, or a mount naming
// no declared fragment) - only those can turn declared fields into false
// unknowns; a key-level fault keeps its entry's chain.
// The H001/H002 hints a schema disavows are NOT dropped by this call: they
// live on the parse's diagnostics, which validation does not touch. Parse then
// validate and they are still there - apply SuppressDeclaredRepeats /
// SuppressDeclaredReopens yourself, or use LoadAndValidate, which runs both
// for you.
func (d *Document) Validate(schema *Document) []Diagnostic {
	def, faults := buildSchema(schema)
	out := faults
	// One mount set for the whole schema: two top-level paths can resolve to
	// the same node and mount the same fragment there, and the spec says each
	// fragment runs once per node.
	mounted := make(map[fragMount]bool)
	for i := range def.cons {
		d.vCheckFrom(&def.cons[i], &def, root, 0, &out, mounted)
	}
	if def.pathsComplete {
		d.vUnknown(&def, &out)
	}
	return out
}

// vContexts collects resolution contexts: the whole document for a plain path;
// each enclosing instance for the part of a path after a wildcard. required/
// repeat evaluate per context (anchor line 0 = document scope), so
// `server[*].port` + required means a port under EACH server - vacuously true
// with no servers.
type vContext struct {
	anchor int
	found  []int
}

func (d *Document) vContexts(start []int, segs []segment, anchor int, out *[]vContext) {
	cur := start
	for i, seg := range segs {
		var next []int
		for _, n := range cur {
			if seg.star {
				next = append(next, d.arena[n].children...)
			} else {
				next = append(next, d.childrenNamed(n, seg.name)...)
			}
		}
		if seg.star {
			// Name wildcard: same per-instance split as `[*]`, any child name.
			rest := segs[i+1:]
			if len(rest) == 0 {
				*out = append(*out, vContext{anchor: anchor, found: next})
			} else {
				for _, inst := range next {
					d.vContexts([]int{inst}, rest, d.arena[inst].line, out)
				}
			}
			return
		}
		if seg.sel == nil {
			cur = next
			continue
		}
		switch seg.sel.kind {
		case selByValue:
			want := applyEscapes(seg.sel.value)
			cur = nil
			for _, c := range next {
				if dispKey(&d.arena[c].value) == want && (!seg.sel.quoted || singleScalar(&d.arena[c].value)) {
					cur = append(cur, c)
				}
			}
		case selByIndex:
			// Compare in uint64 like the sibling sites: int() of a huge index
			// wraps negative and the bounds check would pass straight into a panic.
			if seg.sel.index < uint64(len(next)) {
				cur = []int{next[seg.sel.index]}
			} else {
				cur = nil
			}
		case selWildcard:
			rest := segs[i+1:]
			if len(rest) == 0 {
				*out = append(*out, vContext{anchor: anchor, found: next})
			} else {
				for _, inst := range next {
					d.vContexts([]int{inst}, rest, d.arena[inst].line, out)
				}
			}
			return
		}
	}
	*out = append(*out, vContext{anchor: anchor, found: cur})
}

// fragMount keys one (fragment, node) pair; a struct key, not a joined
// string, so a fragment name containing the separator cannot collide.
type fragMount struct {
	fr string
	n  int
}

// A mounted fragment's fields run per resolved node, right after that node's
// own checks, in fragment order - depth-first, so diagnostic order stays
// derivable. Termination is structural: every mount descends at least one
// document level, and the document is finite.
func (d *Document) vCheckFrom(
	c *constraint, def *schemaDef, start, anchor0 int, out *[]Diagnostic, mounted map[fragMount]bool,
) {
	var ctxs []vContext
	d.vContexts([]int{start}, c.segs, anchor0, &ctxs)
	for _, ctx := range ctxs {
		if c.required && len(ctx.found) == 0 {
			vdiag(out, ctx.anchor, "V002", fmt.Sprintf("required path missing: %s", c.path))
		}
		if c.repeat != nil {
			n := uint64(len(ctx.found))
			if n < c.repeat[0] || n > c.repeat[1] {
				vdiag(out, ctx.anchor, "V007", fmt.Sprintf("instance count out of bounds at '%s': %d not in %d..%d",
					c.path, n, c.repeat[0], c.repeat[1]))
			}
		}
		for _, n := range ctx.found {
			d.vNode(c, n, out)
			if c.inherits != "" {
				if fcs, ok := def.frags[c.inherits]; ok {
					// Two constraints can resolve to the same node and mount the
					// same fragment there. The second mount would repeat the
					// first's work and its diagnostics, and repeating it per
					// level is what makes a recursive schema cost double per
					// document level, so each pair is done once.
					key := fragMount{fr: c.inherits, n: n}
					if !mounted[key] {
						mounted[key] = true
						for i := range fcs {
							d.vCheckFrom(&fcs[i], def, n, d.arena[n].line, out, mounted)
						}
					}
				}
			}
		}
	}
}

func (d *Document) vNode(c *constraint, n int, out *[]Diagnostic) {
	node := &d.arena[n]
	line := node.line
	base := strings.TrimSuffix(c.ty, "-array")
	isArray := strings.HasSuffix(c.ty, "-array")
	wrong := func() {
		vdiag(out, line, "V003", fmt.Sprintf("wrong type at '%s': value is not a valid %s", c.path, c.ty))
	}
	switch node.value.kind {
	// Empty passes everything; required already counted it as present.
	case vEmpty:
	case vRaw:
		// A raw block satisfies `raw` and scalar `string` (any value reads as a
		// string); every other kind is a type miss.
		if c.ty != "" && (base != "raw" && base != "string" || isArray) {
			wrong()
			return
		}
		if c.allowed != nil && c.allowed.kind == allowStrings {
			if !containsString(c.allowed.strs, node.value.raw.content) {
				vdiag(out, line, "V004", fmt.Sprintf("value not allowed at '%s': %s", c.path, node.value.raw.content))
			}
		}
	case vCell:
		els := node.value.els
		if base == "raw" {
			wrong()
			return
		}
		// A scalar kind on a multi-element value is the array-where-one-scalar-
		// expected miss - except string, which reads arrays.
		if c.ty != "" && !isArray && base != "string" && len(els) > 1 {
			wrong()
			return
		}
		switch base {
		case "int":
			vals := make([]int64, 0, len(els))
			for i := range els {
				v, ok := parseIntText(&els[i], d.strictness)
				if !ok {
					wrong()
					return
				}
				vals = append(vals, v)
			}
			if c.allowed != nil && c.allowed.kind == allowInts {
				for i, v := range vals {
					if !containsInt(c.allowed.ints, v) {
						vdiag(out, line, "V004", fmt.Sprintf("value not allowed at '%s': %s", c.path, els[i].text))
						break
					}
				}
			}
			if c.minI != nil && anyIntBelow(vals, *c.minI) {
				vdiag(out, line, "V005", fmt.Sprintf("value below min at '%s'", c.path))
			}
			if c.maxI != nil && anyIntAbove(vals, *c.maxI) {
				vdiag(out, line, "V006", fmt.Sprintf("value above max at '%s'", c.path))
			}
		case "float":
			vals := make([]float64, 0, len(els))
			for i := range els {
				v, ok := parseFloatText(&els[i], d.strictness)
				if !ok {
					wrong()
					return
				}
				vals = append(vals, v)
			}
			if c.allowed != nil && c.allowed.kind == allowFloats {
				for i, v := range vals {
					if !containsFloat(c.allowed.floats, v) {
						vdiag(out, line, "V004", fmt.Sprintf("value not allowed at '%s': %s", c.path, els[i].text))
						break
					}
				}
			}
			if c.minF != nil && anyFloatBelow(vals, *c.minF) {
				vdiag(out, line, "V005", fmt.Sprintf("value below min at '%s'", c.path))
			}
			if c.maxF != nil && anyFloatAbove(vals, *c.maxF) {
				vdiag(out, line, "V006", fmt.Sprintf("value above max at '%s'", c.path))
			}
		case "bool":
			vals := make([]bool, 0, len(els))
			for i := range els {
				v, ok := parseBoolText(els[i].text, d.strictness)
				if !ok {
					wrong()
					return
				}
				vals = append(vals, v)
			}
			if c.allowed != nil && c.allowed.kind == allowBools {
				for i, v := range vals {
					if !containsBool(c.allowed.bools, v) {
						vdiag(out, line, "V004", fmt.Sprintf("value not allowed at '%s': %s", c.path, els[i].text))
						break
					}
				}
			}
		case "datetime":
			vals := make([]DateTime, 0, len(els))
			for i := range els {
				v, ok := ParseDateTime(els[i].text)
				if !ok {
					wrong()
					return
				}
				vals = append(vals, v)
			}
			if c.allowed != nil && c.allowed.kind == allowDates {
				for i, v := range vals {
					if !containsDate(c.allowed.dates, v) {
						vdiag(out, line, "V004", fmt.Sprintf("value not allowed at '%s': %s", c.path, els[i].text))
						break
					}
				}
			}
		default:
			// string kind or untyped: every element coerces; only the allowed
			// set can fail, in logical-string space.
			if c.allowed != nil && c.allowed.kind == allowStrings {
				for i := range els {
					s := applyEscapes(els[i].text)
					if !containsString(c.allowed.strs, s) {
						vdiag(out, line, "V004", fmt.Sprintf("value not allowed at '%s': %s", c.path, s))
						break
					}
				}
			}
		}
	}
}

func containsInt(xs []int64, v int64) bool {
	for _, x := range xs {
		if x == v {
			return true
		}
	}
	return false
}

func containsFloat(xs []float64, v float64) bool {
	for _, x := range xs {
		if x == v {
			return true
		}
	}
	return false
}

func containsBool(xs []bool, v bool) bool {
	for _, x := range xs {
		if x == v {
			return true
		}
	}
	return false
}

func containsDate(xs []DateTime, v DateTime) bool {
	for _, x := range xs {
		if dtEqual(x, v) {
			return true
		}
	}
	return false
}

func anyIntBelow(xs []int64, lo int64) bool {
	for _, x := range xs {
		if x < lo {
			return true
		}
	}
	return false
}

func anyIntAbove(xs []int64, hi int64) bool {
	for _, x := range xs {
		if x > hi {
			return true
		}
	}
	return false
}

func anyFloatBelow(xs []float64, lo float64) bool {
	for _, x := range xs {
		if x < lo {
			return true
		}
	}
	return false
}

func anyFloatAbove(xs []float64, hi float64) bool {
	for _, x := range xs {
		if x > hi {
			return true
		}
	}
	return false
}

// vUnknown is the unknown-field sweep: a schema path legalizes its name chain
// and every prefix (selectors ignored). Only the topmost unknown node is
// reported; its subtree is implied unknown and skipped.
func (d *Document) vUnknown(def *schemaDef, out *[]Diagnostic) {
	cons := def.cons
	// Chains below a fragment mount only match by descending the mounts.
	hasMounts := false
	for i := range cons {
		if cons[i].inherits != "" {
			hasMounts = true
			break
		}
	}
	legal := map[string]bool{}
	// Sibling names per parent chain, built once (schema order): vSuggest
	// used to rebuild every chain per unknown field, which bit hardest on
	// the wholesale-unmatched documents the feature exists for.
	siblings := map[string][]string{}
	// Paths with a `*` segment can't live in the exact-chain hash; they
	// match element-wise (a star matches any one name, prefixes included).
	var starPats [][]segment
	for i := range cons {
		for _, s := range cons[i].segs {
			if s.star {
				starPats = append(starPats, cons[i].segs)
				break
			}
		}
		chain := ""
		for _, s := range cons[i].segs {
			if s.star {
				break // no sibling entry for '*'; deeper chains are pattern-only
			}
			siblings[chain] = append(siblings[chain], s.name)
			chain = chainPush(chain, s.name)
			legal[chain] = true
		}
	}
	type frame struct {
		node  int
		chain string
		shown string
	}
	var stack []frame
	kids := d.arena[root].children
	for i := len(kids) - 1; i >= 0; i-- {
		stack = append(stack, frame{node: kids[i]})
	}
	for len(stack) > 0 {
		fr := stack[len(stack)-1]
		stack = stack[:len(stack)-1]
		node := &d.arena[fr.node]
		chain := chainPush(fr.chain, node.name)
		shown := node.name
		if fr.shown != "" {
			shown = fr.shown + "." + node.name
		}
		if !legal[chain] && !starLegal(starPats, chain) && !(hasMounts && chainLegal(cons, def.frags, chain)) {
			hint := vSuggest(siblings, fr.chain, node.name)
			vdiag(out, node.line, "V001", fmt.Sprintf("unknown field '%s'%s", shown, hint))
			continue
		}
		for i := len(node.children) - 1; i >= 0; i-- {
			stack = append(stack, frame{node: node.children[i], chain: chain, shown: shown})
		}
	}
}

// chainPush appends a segment to a chain key. Chain keys join segments
// length-prefixed (`<len>:<name>`), not with a bare NUL: NUL is legal in a
// quoted name, so a single field named "x\x00y" would impersonate the
// two-segment path x.y. Same injectivity reasoning as the merge key's cell
// encoding - and like it, the length unit is each binding's native one
// (bytes here), because only injectivity matters.
func chainPush(chain, name string) string {
	return chain + strconv.Itoa(len(name)) + ":" + name
}

// chainParts decodes a chain key back into its segments. Total: bails at the
// first shape the encoder can't have produced.
func chainParts(chain string) []string {
	var parts []string
	i := 0
	for i < len(chain) {
		n := 0
		for i < len(chain) && chain[i] >= '0' && chain[i] <= '9' {
			n = n*10 + int(chain[i]-'0')
			i++
		}
		if i >= len(chain) || chain[i] != ':' || i+1+n > len(chain) {
			break
		}
		i++
		parts = append(parts, chain[i:i+n])
		i += n
	}
	return parts
}

// starLegal is the element-wise chain match against the star-bearing schema
// paths: a `*` segment matches any one name, and every prefix of a path is
// legal.
func starLegal(pats [][]segment, chain string) bool {
	if len(pats) == 0 {
		return false
	}
	parts := chainParts(chain)
	for _, p := range pats {
		if len(p) < len(parts) {
			continue
		}
		ok := true
		for i, seg := range parts {
			if !p[i].star && p[i].name != seg {
				ok = false
				break
			}
		}
		if ok {
			return true
		}
	}
	return false
}

// chainLegal is chain legality through fragment mounts: the general matcher -
// element-wise like starLegal (stars wild, prefixes legal), and when a mount's
// whole path matched with chain left over, the remainder is retried against
// the mounted fragment's fields. Terminates: every descent consumes >= 1 part.
func chainLegal(cons []constraint, frags map[string][]constraint, chain string) bool {
	parts := chainParts(chain)
	return chainPartsLegal(cons, frags, parts)
}

func chainPartsLegal(cons []constraint, frags map[string][]constraint, parts []string) bool {
	for i := range cons {
		c := &cons[i]
		n := len(c.segs)
		k := len(parts)
		if n < k {
			k = n
		}
		matched := true
		for j := 0; j < k; j++ {
			if !c.segs[j].star && c.segs[j].name != parts[j] {
				matched = false
				break
			}
		}
		if !matched {
			continue
		}
		if len(parts) <= n {
			return true
		}
		if c.inherits != "" {
			if fcs, ok := frags[c.inherits]; ok && chainPartsLegal(fcs, frags, parts[n:]) {
				return true
			}
		}
	}
	return false
}

// vSuggest finds the closest legal sibling name (same parent chain, schema
// order, edit distance <= 2) as "; did you mean 'x'?" - or nothing. Prose
// only, never contract.
func vSuggest(siblings map[string][]string, parentChain, name string) string {
	bestDist := -1
	bestName := ""
	for _, s := range siblings[parentChain] {
		dist := editDistance(name, s, 2)
		if dist <= 2 && (bestDist < 0 || dist < bestDist) {
			bestDist = dist
			bestName = s
		}
	}
	if bestDist < 0 {
		return ""
	}
	return fmt.Sprintf("; did you mean '%s'?", bestName)
}
