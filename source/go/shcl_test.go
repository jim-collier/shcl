// SPDX-License-Identifier: MIT
// Copyright © 2026 Jim Collier (CryptogID: ѳ6ᴚ℈𐀘𐇦ɛ𐊁¥Mﾏb϶Δ𐌞)

// Conformance-corpus runner. Every shipped binding must pass this corpus; the
// Go binding runs it natively here. Case layout and reads.tsv column meanings
// are documented in project/conformance/README.md.

package shcl

import (
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"testing"
)

func corpusDir() string {
	return filepath.Join("..", "..", "project", "conformance")
}

// tsvEscape gives the TSV-safe form: real newlines/tabs in a value are
// written \n / \t.
func tsvEscape(s string) string {
	return strings.ReplaceAll(strings.ReplaceAll(s, "\n", "\\n"), "\t", "\\t")
}

func parseLevel(t *testing.T, s string) Strictness {
	switch s {
	case "", "standard":
		return Standard
	case "loose":
		return Loose
	case "strict":
		return Strict
	}
	t.Fatalf("unknown level '%s' in reads.tsv", s)
	return Standard
}

type corpusCase struct {
	name        string
	input       string
	expectedFmt string
	reads       string
	// Golden `check` stdout at Standard: diag lines (line/severity/code) + summary.
	expectedDiags string
	// Write dimension (optional): an ops script and its golden canonical output.
	writeOps      string
	expectedWrite string
	hasWrite      bool
	// Bad-op dimension (optional): ops that must each be rejected, applied alone.
	writeBadOps string
	hasWriteBad bool
	// Schema dimension (optional): a schema and the golden `check --schema` stdout.
	schema           string
	expectedValidate string
	hasSchema        bool
	// Layered-load dimension (optional): lower-priority layer files (in filename
	// order), optional `path=value` overrides, and the golden merged canonical.
	layers         []string
	mergeSets      string
	expectedMerged string
	hasMerge       bool
	// Generation dimension (optional): a schema and the golden `init` output.
	initSchema   string
	expectedInit string
	hasInit      bool
}

func loadCases(t *testing.T) []corpusCase {
	dir := corpusDir()
	entries, err := os.ReadDir(dir)
	if err != nil {
		t.Fatalf("corpus dir %s: %v", dir, err)
	}
	var cases []corpusCase
	for _, entry := range entries {
		if !entry.IsDir() {
			continue
		}
		caseDir := filepath.Join(dir, entry.Name())
		input, err := os.ReadFile(filepath.Join(caseDir, "input.shcl"))
		if err != nil {
			continue
		}
		expected, err := os.ReadFile(filepath.Join(caseDir, "expected.shcl"))
		if err != nil {
			t.Fatalf("%s: %v", entry.Name(), err)
		}
		reads, err := os.ReadFile(filepath.Join(caseDir, "reads.tsv"))
		if err != nil {
			t.Fatalf("%s: %v", entry.Name(), err)
		}
		diags, err := os.ReadFile(filepath.Join(caseDir, "expected-diags.txt"))
		if err != nil {
			t.Fatalf("%s: %v", entry.Name(), err)
		}
		cc := corpusCase{
			name:          entry.Name(),
			input:         string(input),
			expectedFmt:   string(expected),
			reads:         string(reads),
			expectedDiags: string(diags),
		}
		if ops, err := os.ReadFile(filepath.Join(caseDir, "write.ops")); err == nil {
			ew, err2 := os.ReadFile(filepath.Join(caseDir, "expected-write.shcl"))
			if err2 != nil {
				t.Fatalf("%s: write.ops without expected-write.shcl", entry.Name())
			}
			cc.writeOps, cc.expectedWrite, cc.hasWrite = string(ops), string(ew), true
		}
		if bad, err := os.ReadFile(filepath.Join(caseDir, "write-bad.ops")); err == nil {
			cc.writeBadOps, cc.hasWriteBad = string(bad), true
		}
		if sch, err := os.ReadFile(filepath.Join(caseDir, "schema.shcl")); err == nil {
			ev, err2 := os.ReadFile(filepath.Join(caseDir, "expected-validate.txt"))
			if err2 != nil {
				t.Fatalf("%s: schema.shcl without expected-validate.txt", entry.Name())
			}
			cc.schema, cc.expectedValidate, cc.hasSchema = string(sch), string(ev), true
		}
		if em, err := os.ReadFile(filepath.Join(caseDir, "expected-merged.shcl")); err == nil {
			// Layer files: every layer*.shcl, in filename (= priority) order.
			dirEntries, _ := os.ReadDir(caseDir)
			var layerNames []string
			for _, de := range dirEntries {
				n := de.Name()
				if strings.HasPrefix(n, "layer") && strings.HasSuffix(n, ".shcl") {
					layerNames = append(layerNames, n)
				}
			}
			sort.Strings(layerNames)
			for _, n := range layerNames {
				lb, lerr := os.ReadFile(filepath.Join(caseDir, n))
				if lerr != nil {
					t.Fatalf("%s: %v", entry.Name(), lerr)
				}
				cc.layers = append(cc.layers, string(lb))
			}
			if ms, err2 := os.ReadFile(filepath.Join(caseDir, "merge.sets")); err2 == nil {
				cc.mergeSets = string(ms)
			}
			cc.expectedMerged, cc.hasMerge = string(em), true
		}
		if is, err := os.ReadFile(filepath.Join(caseDir, "init-schema.shcl")); err == nil {
			ei, err2 := os.ReadFile(filepath.Join(caseDir, "expected-init.shcl"))
			if err2 != nil {
				t.Fatalf("%s: init-schema.shcl without expected-init.shcl", entry.Name())
			}
			cc.initSchema, cc.expectedInit, cc.hasInit = string(is), string(ei), true
		}
		cases = append(cases, cc)
	}
	sort.Slice(cases, func(i, j int) bool { return cases[i].name < cases[j].name })
	if len(cases) == 0 {
		t.Fatalf("no corpus cases found under %s", dir)
	}
	return cases
}

func docFor(t *testing.T, c *corpusCase, level Strictness) *Document {
	doc, err := ParseWith(c.input, level)
	if err != nil {
		t.Fatalf("%s: load failed at level %d but reads.tsv has reads there: %v", c.name, level, err)
	}
	return doc
}

func TestCanonicalFormatMatchesExpected(t *testing.T) {
	for _, c := range loadCases(t) {
		got := Parse(c.input).ToCanonical()
		if got != c.expectedFmt {
			t.Errorf("%s: canonical output differs from expected.shcl\ngot:\n%s\nwant:\n%s", c.name, got, c.expectedFmt)
			continue
		}
		// The formatter must be a fixpoint: canonicalizing its own output changes nothing.
		if again := Parse(got).ToCanonical(); again != got {
			t.Errorf("%s: formatter is not idempotent", c.name)
		}
	}
}

// unescapeOpsTest decodes an ops value: \n \t \\ only (mirrors the CLI).
func unescapeOpsTest(s string) string {
	var b strings.Builder
	for i := 0; i < len(s); i++ {
		if s[i] != '\\' || i+1 >= len(s) {
			b.WriteByte(s[i])
			continue
		}
		i++
		switch s[i] {
		case 'n':
			b.WriteByte('\n')
		case 't':
			b.WriteByte('\t')
		case '\\':
			b.WriteByte('\\')
		default:
			b.WriteByte('\\')
			b.WriteByte(s[i])
		}
	}
	return b.String()
}

// Value gates mirror the CLI's exactly: grammar first (reference FromStr
// shape), then range; float overflow yields +/-Inf, not an error.
func intGrammarTest(s string) bool {
	if s != "" && (s[0] == '+' || s[0] == '-') {
		s = s[1:]
	}
	if s == "" {
		return false
	}
	for i := 0; i < len(s); i++ {
		if s[i] < '0' || s[i] > '9' {
			return false
		}
	}
	return true
}

func floatGrammarTest(s string) bool {
	if s != "" && (s[0] == '+' || s[0] == '-') {
		s = s[1:]
	}
	if s == "" {
		return false
	}
	low := strings.ToLower(s)
	if low == "inf" || low == "infinity" || low == "nan" {
		return true
	}
	i := 0
	digits := func() int {
		n := 0
		for i < len(s) && s[i] >= '0' && s[i] <= '9' {
			i++
			n++
		}
		return n
	}
	if digits() > 0 {
		if i < len(s) && s[i] == '.' {
			i++
			digits()
		}
	} else {
		if s[i] != '.' {
			return false
		}
		i++
		if digits() == 0 {
			return false
		}
	}
	if i < len(s) && (s[i] == 'e' || s[i] == 'E') {
		i++
		if i < len(s) && (s[i] == '+' || s[i] == '-') {
			i++
		}
		if digits() == 0 {
			return false
		}
	}
	return i == len(s)
}

// tryApplyOpTest applies one write-ops line via the library Writer, with the
// same value gates the CLI applies. A non-nil error = the op must be rejected
// (bad value or unusable path).
func tryApplyOpTest(doc *Document, line string) error {
	f := strings.Split(line, "\t")
	get := func(i int) string {
		if i < len(f) {
			return f[i]
		}
		return ""
	}
	path, v := get(1), get(2)
	arr := []string{}
	if len(f) > 2 {
		arr = f[2:]
	}
	pint := func(s string) (int64, error) {
		if !intGrammarTest(s) {
			return 0, fmt.Errorf("bad int: %s", s)
		}
		n, err := strconv.ParseInt(s, 10, 64)
		if err != nil {
			return 0, fmt.Errorf("bad int: %s", s)
		}
		return n, nil
	}
	pflt := func(s string) (float64, error) {
		if !floatGrammarTest(s) {
			return 0, fmt.Errorf("bad float: %s", s)
		}
		n, err := strconv.ParseFloat(s, 64)
		if err != nil {
			if ne, ok := err.(*strconv.NumError); !ok || ne.Err != strconv.ErrRange {
				return 0, fmt.Errorf("bad float: %s", s)
			}
		}
		return n, nil
	}
	ints := func(xs []string) ([]int64, error) {
		o := make([]int64, len(xs))
		for i, s := range xs {
			n, err := pint(s)
			if err != nil {
				return nil, err
			}
			o[i] = n
		}
		return o, nil
	}
	flts := func(xs []string) ([]float64, error) {
		o := make([]float64, len(xs))
		for i, s := range xs {
			n, err := pflt(s)
			if err != nil {
				return nil, err
			}
			o[i] = n
		}
		return o, nil
	}
	bools := func(xs []string) []bool {
		o := make([]bool, len(xs))
		for i, s := range xs {
			o[i] = s == "true"
		}
		return o
	}
	strs := func(xs []string) []string {
		o := make([]string, len(xs))
		for i, s := range xs {
			o[i] = unescapeOpsTest(s)
		}
		return o
	}
	dt := func(s string) (DateTime, error) {
		x, ok := ParseDateTime(s)
		if !ok {
			return x, fmt.Errorf("bad datetime: %s", s)
		}
		return x, nil
	}
	dts := func(xs []string) ([]DateTime, error) {
		o := make([]DateTime, len(xs))
		for i, s := range xs {
			x, err := dt(s)
			if err != nil {
				return nil, err
			}
			o[i] = x
		}
		return o, nil
	}
	wrote := false
	switch f[0] {
	case "int":
		n, err := pint(v)
		if err != nil {
			return err
		}
		wrote = doc.SetInt(path, n)
	case "float":
		n, err := pflt(v)
		if err != nil {
			return err
		}
		wrote = doc.SetFloat(path, n)
	case "bool":
		wrote = doc.SetBool(path, v == "true")
	case "string":
		wrote = doc.SetString(path, unescapeOpsTest(v))
	case "datetime":
		x, err := dt(v)
		if err != nil {
			return err
		}
		wrote = doc.SetDateTime(path, x)
	case "literal":
		wrote = doc.SetLiteral(path, v)
	case "literal-default":
		wrote = doc.SetLiteralDefault(path, v)
	case "int-default":
		n, err := pint(v)
		if err != nil {
			return err
		}
		wrote = doc.SetIntDefault(path, n)
	case "float-default":
		n, err := pflt(v)
		if err != nil {
			return err
		}
		wrote = doc.SetFloatDefault(path, n)
	case "bool-default":
		wrote = doc.SetBoolDefault(path, v == "true")
	case "string-default":
		wrote = doc.SetStringDefault(path, unescapeOpsTest(v))
	case "datetime-default":
		x, err := dt(v)
		if err != nil {
			return err
		}
		wrote = doc.SetDateTimeDefault(path, x)
	case "int-array":
		xs, err := ints(arr)
		if err != nil {
			return err
		}
		wrote = doc.SetIntArray(path, xs)
	case "float-array":
		xs, err := flts(arr)
		if err != nil {
			return err
		}
		wrote = doc.SetFloatArray(path, xs)
	case "bool-array":
		wrote = doc.SetBoolArray(path, bools(arr))
	case "string-array":
		wrote = doc.SetStringArray(path, strs(arr))
	case "datetime-array":
		xs, err := dts(arr)
		if err != nil {
			return err
		}
		wrote = doc.SetDateTimeArray(path, xs)
	case "int-array-default":
		xs, err := ints(arr)
		if err != nil {
			return err
		}
		wrote = doc.SetIntArrayDefault(path, xs)
	case "float-array-default":
		xs, err := flts(arr)
		if err != nil {
			return err
		}
		wrote = doc.SetFloatArrayDefault(path, xs)
	case "bool-array-default":
		wrote = doc.SetBoolArrayDefault(path, bools(arr))
	case "string-array-default":
		wrote = doc.SetStringArrayDefault(path, strs(arr))
	case "datetime-array-default":
		xs, err := dts(arr)
		if err != nil {
			return err
		}
		wrote = doc.SetDateTimeArrayDefault(path, xs)
	case "raw":
		wrote = doc.SetRaw(path, unescapeOpsTest(get(3)), v)
	case "raw-default":
		wrote = doc.SetRawDefault(path, unescapeOpsTest(get(3)), v)
	case "empty":
		wrote = doc.SetEmpty(path)
	case "comment":
		wrote = doc.SetComment(path, v)
	case "remove":
		doc.Remove(path)
		wrote = true
	default:
		return fmt.Errorf("unknown op: %s", f[0])
	}
	if !wrote {
		return fmt.Errorf("cannot write %s", path)
	}
	return nil
}

// applyOpTest is the good-path wrapper: the op must apply.
func applyOpTest(t *testing.T, doc *Document, line, at string) {
	if err := tryApplyOpTest(doc, line); err != nil {
		t.Fatalf("%s: %s", at, err)
	}
}

func TestDiagnosticsMatchExpected(t *testing.T) {
	// Pins count, line, severity, and stable code per case - the same shape
	// `check` prints to stdout at Standard (its cross-binding contract).
	for _, c := range loadCases(t) {
		diags := Parse(c.input).Diagnostics()
		var got strings.Builder
		errors := 0
		for _, d := range diags {
			fmt.Fprintf(&got, "line %d: %s: %s\n", d.Line, d.Severity, d.Code)
			if d.Severity == SeverityError {
				errors++
			}
		}
		if errors > 0 {
			fmt.Fprintf(&got, "failed: %d diagnostic(s), %d error(s)\n", len(diags), errors)
		} else {
			fmt.Fprintf(&got, "ok (%d diagnostic(s))\n", len(diags))
		}
		if got.String() != c.expectedDiags {
			t.Errorf("%s: diagnostics differ from expected-diags.txt\ngot:\n%s\nwant:\n%s", c.name, got.String(), c.expectedDiags)
		}
	}
}

func TestValidationMatchesExpected(t *testing.T) {
	// Schema dimension: golden = the exact `check --schema` stdout at Standard
	// (doc parse diags, then validation diags, then the summary). A schema that
	// does not load cleanly is a single V099, mirroring the CLI.
	for _, c := range loadCases(t) {
		if !c.hasSchema {
			continue
		}
		doc := Parse(c.input)
		diags := append([]Diagnostic{}, doc.Diagnostics()...)
		sdoc := Parse(c.schema)
		bad := false
		for _, sd := range sdoc.Diagnostics() {
			if sd.Severity == SeverityError {
				bad = true
			}
		}
		if bad {
			diags = append(diags, Diagnostic{Line: 0, Severity: SeverityError, Message: "schema failed to load", Code: "V099"})
		} else {
			diags = append(diags, doc.Validate(sdoc)...)
			diags = SuppressDeclaredRepeats(sdoc, diags)
			diags = SuppressDeclaredReopens(sdoc, diags)
		}
		var got strings.Builder
		errors := 0
		for _, d := range diags {
			fmt.Fprintf(&got, "line %d: %s: %s\n", d.Line, d.Severity, d.Code)
			if d.Severity == SeverityError {
				errors++
			}
		}
		if errors > 0 {
			fmt.Fprintf(&got, "failed: %d diagnostic(s), %d error(s)\n", len(diags), errors)
		} else {
			fmt.Fprintf(&got, "ok (%d diagnostic(s))\n", len(diags))
		}
		if got.String() != c.expectedValidate {
			t.Errorf("%s: validation output differs from expected-validate.txt\ngot:\n%s\nwant:\n%s", c.name, got.String(), c.expectedValidate)
		}
	}
}

func TestWriteOpsMatchExpected(t *testing.T) {
	for _, c := range loadCases(t) {
		if !c.hasWrite {
			continue
		}
		doc := Parse(c.input)
		for n, line := range strings.Split(c.writeOps, "\n") {
			line = strings.TrimSuffix(line, "\r")
			if line == "" || strings.HasPrefix(line, "#") {
				continue
			}
			applyOpTest(t, doc, line, fmt.Sprintf("%s: write.ops line %d", c.name, n+1))
		}
		got := doc.ToCanonical()
		if got != c.expectedWrite {
			t.Errorf("%s: writer output differs from expected-write.shcl\ngot:\n%s\nwant:\n%s", c.name, got, c.expectedWrite)
			continue
		}
		if again := Parse(got).ToCanonical(); again != got {
			t.Errorf("%s: written output is not a fmt fixpoint", c.name)
		}
	}
}

func TestWriteBadOpsAreRejected(t *testing.T) {
	// Bad-op dimension: each write-bad.ops line, applied alone to the case
	// input, must be rejected (bad value, bad datetime, or unusable path) and
	// leave the document unchanged.
	for _, c := range loadCases(t) {
		if !c.hasWriteBad {
			continue
		}
		for n, line := range strings.Split(c.writeBadOps, "\n") {
			line = strings.TrimSuffix(line, "\r")
			if line == "" || strings.HasPrefix(line, "#") {
				continue
			}
			doc := Parse(c.input)
			before := doc.ToCanonical()
			if err := tryApplyOpTest(doc, line); err == nil {
				t.Errorf("%s: write-bad.ops line %d was accepted: %s", c.name, n+1, line)
				continue
			}
			if got := doc.ToCanonical(); got != before {
				t.Errorf("%s: write-bad.ops line %d changed the document: %s", c.name, n+1, line)
			}
		}
	}
}

func TestOneShotLoadAndValidate(t *testing.T) {
	// One combined diagnostics list (parse first, then validation) and an
	// error predicate, so recover-and-continue can't read as success by
	// accident. Same fixture in every runner.
	text := ": nope\nport: x\n"
	schema := "field: port\n\ttype: int\n"
	doc := LoadAndValidate(text, schema, Standard)
	var codes []string
	for _, d := range doc.Diagnostics() {
		codes = append(codes, d.Code)
	}
	if strings.Join(codes, ",") != "E014,V003" {
		t.Errorf("codes: got %v, want [E014 V003]", codes)
	}
	if got := doc.ErrorCount(); got != 2 {
		t.Errorf("error count: got %d, want 2", got)
	}
	if got := doc.ReadString("port").Value; got != "x" { // doc still usable
		t.Errorf("port: got %q, want \"x\"", got)
	}
	// Strict never errors out here; the diagnostics are the answer.
	strict := LoadAndValidate(text, schema, Strict)
	if strict.ErrorCount() < 2 {
		t.Errorf("strict error count: got %d, want >= 2", strict.ErrorCount())
	}
	// An empty schema declares nothing and validates nothing.
	plain := LoadAndValidate("a: 1\n", "", Standard)
	if plain.ErrorCount() != 0 || len(plain.Diagnostics()) != 0 {
		t.Errorf("plain: got %d errors, %d diags, want 0, 0", plain.ErrorCount(), len(plain.Diagnostics()))
	}
}

func TestWriteReasonNamesTheFailure(t *testing.T) {
	// The reason behind a setter's bare false. Same fixture in every runner.
	doc := Parse("a:\n\tb: 1\n")
	if got := doc.WriteReason("a.b"); got != Writable {
		t.Errorf("a.b: got %v, want Writable", got)
	}
	if got := doc.WriteReason("a.new[Boston].x"); got != Writable { // creatable
		t.Errorf("a.new[Boston].x: got %v, want Writable", got)
	}
	if got := doc.WriteReason(""); got != BadPath {
		t.Errorf("empty path: got %v, want BadPath", got)
	}
	if got := doc.WriteReason("a..b"); got != BadPath {
		t.Errorf("a..b: got %v, want BadPath", got)
	}
	if got := doc.WriteReason("a.b: 2"); got != ValueInPath {
		t.Errorf("a.b: 2: got %v, want ValueInPath", got)
	}
	if got := doc.WriteReason("a[*].b"); got != Wildcard {
		t.Errorf("a[*].b: got %v, want Wildcard", got)
	}
	if got := doc.WriteReason("a[#5].b"); got != NoSuchIndex {
		t.Errorf("a[#5].b: got %v, want NoSuchIndex", got)
	}
	if got := doc.WriteReason("nope[#0].b"); got != NoSuchIndex {
		t.Errorf("nope[#0].b: got %v, want NoSuchIndex", got)
	}
	deep := strings.TrimSuffix(strings.Repeat("d.", 513), ".")
	if got := doc.WriteReason(deep); got != TooDeep {
		t.Errorf("deep path: got %v, want TooDeep", got)
	}
	// A literal line break in a segment: the binding would emit across two lines
	// and reparse as neither. The escaped spelling is a different path and writes
	// fine. Not corpus-pinnable - an ops line cannot carry a raw newline.
	if got := doc.WriteReason("\"x\ny\".b"); got != BadPath {
		t.Errorf("newline in name: got %v, want BadPath", got)
	}
	if got := doc.WriteReason("a[\"p\nq\"].b"); got != BadPath {
		t.Errorf("newline in selector: got %v, want BadPath", got)
	}
	if got := doc.WriteReason("\"x\\ny\".b"); got != Writable {
		t.Errorf("escaped newline in name: got %v, want Writable", got)
	}
	// The probe never creates: the doc is unchanged after all of the above.
	if n := doc.Count("a"); n != 1 {
		t.Errorf("count a: got %d, want 1", n)
	}
	if got := doc.Paths(); strings.Join(got, ",") != "a,a.b" {
		t.Errorf("paths: got %v", got)
	}
}

func TestReadSurfaceLineQuotedChildren(t *testing.T) {
	// Line/Quoted on the read result, Line(path), Children(path). Same
	// fixture in every runner (C pins the accessors; its read structs stay
	// value+status).
	text := "a: @null\nb: \"@null\"\ncode:\n\thook: 1\n\thook: 2\n\tdone: 3\n"
	doc := Parse(text)
	if doc.ReadString("a").Quoted {
		t.Error("a reads quoted")
	}
	if !doc.ReadString("b").Quoted {
		t.Error("b reads unquoted")
	}
	if got := doc.ReadString("b").Line; got != 2 {
		t.Errorf("b line: got %d, want 2", got)
	}
	if got := doc.Line("code.done"); got != 6 {
		t.Errorf("code.done line: got %d, want 6", got)
	}
	if got := doc.Line("code"); got != 3 {
		t.Errorf("code line: got %d, want 3", got)
	}
	if got := doc.Line("missing"); got != 0 {
		t.Errorf("missing line: got %d, want 0", got)
	}
	// Lines(): the plural - a repeated field cites every binding, wildcard
	// slots keep their index (0 = unresolved), a miss is the empty list.
	if got := doc.Line("code.hook"); got != 0 { // Multiple - the singular's gap
		t.Errorf("code.hook line: got %d, want 0", got)
	}
	if got := doc.Lines("code.hook"); fmt.Sprint(got) != "[4 5]" {
		t.Errorf("code.hook lines: got %v", got)
	}
	if got := doc.Lines("code.done"); fmt.Sprint(got) != "[6]" {
		t.Errorf("code.done lines: got %v", got)
	}
	if got := doc.Lines("a"); fmt.Sprint(got) != "[1]" {
		t.Errorf("a lines: got %v", got)
	}
	if got := doc.Lines("code[*].done"); fmt.Sprint(got) != "[6]" {
		t.Errorf("code[*].done lines: got %v", got)
	}
	if got := doc.Lines("code[*].nope"); fmt.Sprint(got) != "[0]" {
		t.Errorf("code[*].nope lines: got %v", got)
	}
	if got := doc.Lines("missing"); len(got) != 0 {
		t.Errorf("missing lines: got %v", got)
	}
	if got := doc.Children("code"); strings.Join(got, ",") != "hook,hook,done" {
		t.Errorf("code children: got %v", got)
	}
	if got := doc.Children(""); strings.Join(got, ",") != "a,b,code" {
		t.Errorf("top children: got %v", got)
	}
	if got := doc.Children("missing"); len(got) != 0 {
		t.Errorf("missing children: got %v", got)
	}
	// SourceName(): the author's spelling, unfolded; merged instances keep
	// the first binding's; unresolved or Multiple is empty; writer-built
	// keeps the setter path's spelling.
	d2 := Parse("SYMBOLS: 3\nCode:\n\tx: 1\ncode:\n\ty: 2\n")
	if got := d2.SourceName("symbols"); got != "SYMBOLS" {
		t.Errorf("SourceName(symbols): got %q", got)
	}
	if got := d2.SourceName("code"); got != "Code" {
		t.Errorf("SourceName(code): got %q", got)
	}
	if got := d2.SourceName("missing"); got != "" {
		t.Errorf("SourceName(missing): got %q", got)
	}
	if !d2.SetInt("NewTop.n", 1) {
		t.Errorf("SetInt NewTop.n failed")
	}
	if got := d2.SourceName("newtop"); got != "NewTop" {
		t.Errorf("SourceName(newtop): got %q", got)
	}
	// Escapes are NOT resolved: a name is stored, compared and emitted in its
	// escaped spelling, so resolving here would name a node that does not exist.
	d3 := Parse("\"Ab\\tCd\": 2\n")
	if got := d3.SourceName("\"ab\\tcd\""); got != "Ab\\tCd" {
		t.Errorf("SourceName escaped: got %q", got)
	}
}

func TestFileTierLoadSave(t *testing.T) {
	// LoadFile/SaveFile: the status separates absent / unreadable / parsed
	// with errors / clean, and a save round-trips through the atomic write.
	// Same fixture in every runner.
	dir := t.TempDir()
	f := dir + "/t.shcl"

	if _, st := LoadFile(f); st != FileNotFound {
		t.Errorf("missing file: got status %v", st)
	}
	if _, st := LoadFile(dir); st != FileUnreadable { // a directory is not readable
		t.Errorf("directory: got status %v", st)
	}
	// Bad encoding is unreadable too: the parser assumes well-formed text, so a
	// binary file loading clean would read back mangled and a later save would
	// write the mangled version over the original.
	if err := os.WriteFile(f, []byte("a: 1\nb: \xff\xfe bad\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	if d, st := LoadFile(f); st != FileUnreadable || d.ToCanonical() != "" {
		t.Errorf("bad encoding: got status %v canonical %q", st, d.ToCanonical())
	}

	if err := os.WriteFile(f, []byte("a: 1\n: broken\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	doc, st := LoadFile(f)
	if st != FileHadErrors {
		t.Errorf("broken file: got status %v", st)
	}
	if v, vst := doc.GetInt("a"); vst != Good || v != 1 {
		t.Errorf("broken file read: got %v %v", v, vst)
	}

	if err := os.WriteFile(f, []byte("a: 1\nb: x\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	doc, st = LoadFile(f)
	if st != FileClean {
		t.Errorf("clean file: got status %v", st)
	}
	if !doc.SetInt("c", 3) {
		t.Fatal("SetInt failed")
	}
	if err := doc.SaveFile(f); err != nil {
		t.Fatal(err)
	}
	back, st := LoadFile(f)
	if st != FileClean {
		t.Errorf("saved file: got status %v", st)
	}
	if back.ToCanonical() != doc.ToCanonical() {
		t.Errorf("save round-trip mismatch")
	}
}

func TestLostAndSaveGate(t *testing.T) {
	// Content-malformed lines are retained as trivia (LostCount 0, the line
	// survives a save); position-dependent drops count as lost and make
	// SaveFile refuse until the caller opts into SaveFileLossy. Same fixture
	// in every runner.
	kept := Parse("a: 1\nsquare-miles 300\nb: 2\n")
	if kept.LostCount() != 0 {
		t.Errorf("kept LostCount: got %d", kept.LostCount())
	}
	if !strings.Contains(kept.ToCanonical(), "square-miles 300\n") {
		t.Errorf("retained line missing from canonical output")
	}
	lost := Parse("a:\n\tb: 1\n  c: 2\n") // indent matches no level
	if lost.LostCount() != 1 {
		t.Errorf("lost LostCount: got %d", lost.LostCount())
	}
	f := t.TempDir() + "/t.shcl"
	if err := kept.SaveFile(f); err != nil {
		t.Fatal(err)
	}
	back, _ := LoadFile(f)
	if !strings.Contains(back.ToCanonical(), "square-miles 300\n") {
		t.Errorf("retained line lost through save round-trip")
	}
	if err := lost.SaveFile(f); err == nil {
		t.Errorf("SaveFile did not refuse a lossy save")
	}
	if err := lost.SaveFileLossy(f); err != nil {
		t.Fatal(err)
	}
}

func TestStrictFailureCarriesDocument(t *testing.T) {
	// A failed strict load hands back the document (non-nil, and on the
	// error too) and names the first failures in the message.
	doc, err := ParseWith("ok: 1\n: nope\n", Strict)
	if err == nil || doc == nil {
		t.Fatalf("want non-nil doc and error, got %v %v", doc, err)
	}
	le := err.(*LoadError)
	if le.Document != doc || len(le.Diagnostics) == 0 {
		t.Fatalf("error does not carry the document/diagnostics")
	}
	if r := doc.ReadInt("ok"); r.Value != 1 {
		t.Fatalf("doc unusable: %v", r)
	}
	if !strings.Contains(err.Error(), "; line ") {
		t.Fatalf("message lacks diagnostics: %s", err.Error())
	}
}

func TestRawIsSourceText(t *testing.T) {
	// Raw: the verbatim value span from the source line - not the display
	// join, which rewrites `{2,3}` to `{2, 3}`. Same fixture in every runner
	// whose read result exposes raw (the C read structs deliberately do not).
	doc := Parse("regex: ^\\d{2,3}$\nlist: a,  \"b c\"\n")
	if r := doc.ReadString("regex"); r.Raw == nil || *r.Raw != "^\\d{2,3}$" {
		t.Errorf("regex raw: got %v", r.Raw)
	}
	if r := doc.ReadStringArray("list"); r.Raw == nil || *r.Raw != "a,  \"b c\"" {
		t.Errorf("list raw: got %v", r.Raw)
	}
	// A written value has no source spelling; raw falls back to display. The
	// selector's escaped spelling must land on the existing instance.
	doc2 := Parse("who: 'q\"uote'\n")
	if !doc2.SetInt("who[\"q\\\"uote\"].n", 5) {
		t.Fatal("SetInt with escaped selector failed")
	}
	if n := doc2.Count("who"); n != 1 {
		t.Errorf("who count: got %d, want 1", n)
	}
	r := doc2.ReadInt("who['q\"uote'].n")
	if r.Value != 5 || r.Status != Good {
		t.Errorf("read back: got (%d, %v), want (5, Good)", r.Value, r.Status)
	}
	if r.Raw == nil || *r.Raw != "5" {
		t.Errorf("written raw: got %v, want 5", r.Raw)
	}
}

func TestLayeredMergeMatchesExpected(t *testing.T) {
	// Layered-load dimension: fold the layer files (lowest first) and input.shcl
	// (highest file layer) via the library Merge, apply the path=value overrides
	// as the top layer, and match the golden merged canonical.
	for _, c := range loadCases(t) {
		if !c.hasMerge {
			continue
		}
		texts := append(append([]string(nil), c.layers...), c.input)
		doc := Parse(texts[0])
		for _, t2 := range texts[1:] {
			doc.Merge(Parse(t2))
		}
		for _, line := range strings.Split(c.mergeSets, "\n") {
			if line == "" || strings.HasPrefix(line, "#") {
				continue
			}
			eq := strings.IndexByte(line, '=')
			if eq < 0 {
				t.Fatalf("%s: bad merge.sets line: %s", c.name, line)
			}
			doc.SetString(line[:eq], line[eq+1:])
		}
		got := doc.ToCanonical()
		if got != c.expectedMerged {
			t.Errorf("%s: merged output differs from expected-merged.shcl\ngot:\n%s\nwant:\n%s", c.name, got, c.expectedMerged)
			continue
		}
		if again := Parse(got).ToCanonical(); again != got {
			t.Errorf("%s: merged output is not a fmt fixpoint", c.name)
		}
	}
}

func TestInitGenerationMatchesExpected(t *testing.T) {
	// Generation dimension: Generate on the schema must reproduce the golden
	// starter config, and that output must itself load cleanly.
	for _, c := range loadCases(t) {
		if !c.hasInit {
			continue
		}
		got, faults := Generate(Parse(c.initSchema), false)
		if faults != nil {
			t.Fatalf("%s: init schema has faults", c.name)
		}
		if got != c.expectedInit {
			t.Errorf("%s: init output differs from expected-init.shcl\ngot:\n%s\nwant:\n%s", c.name, got, c.expectedInit)
			continue
		}
		// The footer is the only difference the flag makes: everything before
		// it is byte-for-byte what the default run produced.
		bare, _ := Generate(Parse(c.initSchema), true)
		if bare == "" || !strings.HasPrefix(got, bare) {
			t.Errorf("%s: --no-banner output is not a prefix of the default", c.name)
			continue
		}
		if !strings.Contains(got[len(bare):], "This config file format is SHCL.") {
			t.Errorf("%s: default init output is missing the format footer", c.name)
		}
		doc := Parse(got)
		for _, d := range doc.Diagnostics() {
			if d.Severity == SeverityError {
				t.Errorf("%s: generated starter does not load cleanly", c.name)
				break
			}
		}
		// And it must satisfy the very schema that produced it - case 026's
		// golden once failed its own schema (repeat lower bound and a
		// materialized wildcard were ignored).
		vs := doc.Validate(Parse(c.initSchema))
		for _, d := range vs {
			if d.Severity == SeverityError {
				t.Errorf("%s: generated starter fails its own schema: %+v", c.name, vs)
				break
			}
		}
	}
}

func TestConvenienceTierFallsBackOnlyOnGood(t *testing.T) {
	// Mirror of the reference: the *Or value survives only on Good; Empty,
	// BadType, and NotFound all yield the call-site fallback.
	d := Parse("a: 42\nb: not-a-number\ne:\narr: 1, 2, 3\n")
	if got := d.GetIntOr("a", 9); got != 42 {
		t.Fatalf("GetIntOr Good = %d, want 42", got)
	}
	for _, p := range []string{"b", "e", "missing"} {
		if got := d.GetIntOr(p, 9); got != 9 {
			t.Fatalf("GetIntOr(%q) = %d, want fallback 9", p, got)
		}
	}
	if got := d.GetIntArrayOr("arr", []int64{7}); len(got) != 3 || got[0] != 1 || got[2] != 3 {
		t.Fatalf("GetIntArrayOr Good = %v, want [1 2 3]", got)
	}
	if got := d.GetIntArrayOr("missing", []int64{7}); len(got) != 1 || got[0] != 7 {
		t.Fatalf("GetIntArrayOr missing = %v, want fallback [7]", got)
	}
}

func TestReadsMatchExpected(t *testing.T) {
	for _, c := range loadCases(t) {
		for n, line := range strings.Split(c.reads, "\n") {
			if n == 0 || strings.TrimSpace(line) == "" {
				continue // header
			}
			cols := strings.Split(line, "\t")
			if len(cols) < 4 {
				t.Fatalf("%s: reads.tsv line %d too short", c.name, n+1)
			}
			query, kind, expected, status := cols[0], cols[1], cols[2], cols[3]
			level := Standard
			if len(cols) > 4 {
				level = parseLevel(t, cols[4])
			}
			at := fmt.Sprintf("%s: reads.tsv line %d (%s %s)", c.name, n+1, query, kind)

			if kind == "load" {
				_, err := ParseWith(c.input, level)
				ok := err == nil
				var want bool
				switch expected {
				case "ok":
					want = true
				case "fail":
					want = false
				default:
					t.Fatalf("%s: bad load expectation '%s'", at, expected)
				}
				if ok != want {
					t.Errorf("%s: load outcome: got ok=%t want ok=%t", at, ok, want)
				}
				continue
			}

			doc := docFor(t, &c, level)
			if kind == "count" {
				want, err := strconv.Atoi(expected)
				if err != nil {
					t.Fatalf("%s: bad count", at)
				}
				if got := doc.Count(query); got != want {
					t.Errorf("%s: count: got %d want %d", at, got, want)
				}
				continue
			}
			if kind == "instances" {
				if got := strings.Join(doc.Instances(query), "|"); got != expected {
					t.Errorf("%s: instances: got %q want %q", at, got, expected)
				}
				continue
			}

			var gotValue string
			var gotStatus Status
			var gotSlots []Status
			switch kind {
			case "int":
				r := doc.ReadInt(query)
				gotValue, gotStatus = strconv.FormatInt(r.Value, 10), r.Status
			case "float":
				r := doc.ReadFloat(query)
				gotValue, gotStatus = FormatFloat(r.Value), r.Status
			case "bool":
				r := doc.ReadBool(query)
				gotValue, gotStatus = strconv.FormatBool(r.Value), r.Status
			case "datetime":
				r := doc.ReadDateTime(query)
				gotValue, gotStatus = r.Value.String(), r.Status
			case "string":
				r := doc.ReadString(query)
				gotValue, gotStatus = tsvEscape(r.Value), r.Status
			case "raw":
				r := doc.ReadRaw(query)
				gotValue, gotStatus = tsvEscape(r.Value), r.Status
			case "rawinfo":
				r := doc.ReadRawInfo(query)
				gotValue, gotStatus = tsvEscape(r.Value), r.Status
			case "int[]":
				r := doc.ReadIntArray(query)
				gotSlots = r.Slots
				parts := make([]string, len(r.Value))
				for i, v := range r.Value {
					parts[i] = strconv.FormatInt(v, 10)
				}
				gotValue, gotStatus = strings.Join(parts, "|"), r.Status
			case "float[]":
				r := doc.ReadFloatArray(query)
				gotSlots = r.Slots
				parts := make([]string, len(r.Value))
				for i, v := range r.Value {
					parts[i] = FormatFloat(v)
				}
				gotValue, gotStatus = strings.Join(parts, "|"), r.Status
			case "bool[]":
				r := doc.ReadBoolArray(query)
				gotSlots = r.Slots
				parts := make([]string, len(r.Value))
				for i, v := range r.Value {
					parts[i] = strconv.FormatBool(v)
				}
				gotValue, gotStatus = strings.Join(parts, "|"), r.Status
			case "datetime[]":
				r := doc.ReadDateTimeArray(query)
				gotSlots = r.Slots
				parts := make([]string, len(r.Value))
				for i, v := range r.Value {
					parts[i] = v.String()
				}
				gotValue, gotStatus = strings.Join(parts, "|"), r.Status
			case "string[]":
				r := doc.ReadStringArray(query)
				gotSlots = r.Slots
				parts := make([]string, len(r.Value))
				for i, v := range r.Value {
					parts[i] = tsvEscape(v)
				}
				gotValue, gotStatus = strings.Join(parts, "|"), r.Status
			default:
				t.Fatalf("%s: unknown type '%s'", at, kind)
			}
			if gotStatus.String() != status {
				t.Errorf("%s: status: got %s want %s", at, gotStatus, status)
			}
			if expected != "-" && gotValue != expected {
				t.Errorf("%s: value: got %q want %q", at, gotValue, expected)
			}
			// Optional 6th column: per-slot statuses, |-joined (needs col 5 set).
			if len(cols) > 5 {
				parts := make([]string, len(gotSlots))
				for i, st := range gotSlots {
					parts[i] = st.String()
				}
				if got := strings.Join(parts, "|"); got != cols[5] {
					t.Errorf("%s: slots: got %q want %q", at, got, cols[5])
				}
			}
		}
	}
}

func TestPathsEnumerationShape(t *testing.T) {
	// Paths(): file order, deduplicated, non-bare segments quoted so every
	// path resolves. Same fixture is pinned in every runner.
	doc := Parse("a: 1\na.b: 2\n\"q n\": 3\nx:\n\tb: 4\nx.b: 5\n")
	got := doc.Paths()
	want := []string{"a", "a.b", "\"q n\"", "x", "x.b"}
	if len(got) != len(want) {
		t.Fatalf("paths: got %v want %v", got, want)
	}
	for i := range want {
		if got[i] != want[i] {
			t.Fatalf("paths: got %v want %v", got, want)
		}
	}
	for _, p := range got {
		if doc.Count(p) < 1 {
			t.Fatalf("emitted path does not resolve: %s", p)
		}
	}
	// QuoteSegment: same spelling both directions, injection-safe.
	if QuoteSegment("port") != "port" || QuoteSegment("q n") != "\"q n\"" || QuoteSegment("a.b") != "\"a.b\"" {
		t.Fatalf("QuoteSegment spelling drift")
	}
	if r := doc.ReadInt(QuoteSegment("q n")); r.Value != 3 || r.Status != Good {
		t.Fatalf("quoted segment read: %v %v", r.Value, r.Status)
	}
}
