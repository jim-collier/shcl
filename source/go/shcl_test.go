// SPDX-License-Identifier: MIT
// Copyright © 2026 Jim Collier

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
	// Write dimension (optional): an ops script and its golden canonical output.
	writeOps      string
	expectedWrite string
	hasWrite      bool
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
		cc := corpusCase{
			name:        entry.Name(),
			input:       string(input),
			expectedFmt: string(expected),
			reads:       string(reads),
		}
		if ops, err := os.ReadFile(filepath.Join(caseDir, "write.ops")); err == nil {
			ew, err2 := os.ReadFile(filepath.Join(caseDir, "expected-write.shcl"))
			if err2 != nil {
				t.Fatalf("%s: write.ops without expected-write.shcl", entry.Name())
			}
			cc.writeOps, cc.expectedWrite, cc.hasWrite = string(ops), string(ew), true
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

func applyOpTest(t *testing.T, doc *Document, line, at string) {
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
	i64 := func(s string) int64 { n, _ := strconv.ParseInt(s, 10, 64); return n }
	f64 := func(s string) float64 { n, _ := strconv.ParseFloat(s, 64); return n }
	ints := func(xs []string) []int64 {
		o := make([]int64, len(xs))
		for i, s := range xs {
			o[i] = i64(s)
		}
		return o
	}
	flts := func(xs []string) []float64 {
		o := make([]float64, len(xs))
		for i, s := range xs {
			o[i] = f64(s)
		}
		return o
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
	dt := func(s string) DateTime {
		x, ok := ParseDateTime(s)
		if !ok {
			t.Fatalf("%s: bad datetime %s", at, s)
		}
		return x
	}
	dts := func(xs []string) []DateTime {
		o := make([]DateTime, len(xs))
		for i, s := range xs {
			o[i] = dt(s)
		}
		return o
	}
	switch f[0] {
	case "int":
		doc.SetInt(path, i64(v))
	case "float":
		doc.SetFloat(path, f64(v))
	case "bool":
		doc.SetBool(path, v == "true")
	case "string":
		doc.SetString(path, unescapeOpsTest(v))
	case "datetime":
		doc.SetDateTime(path, dt(v))
	case "int-default":
		doc.SetIntDefault(path, i64(v))
	case "float-default":
		doc.SetFloatDefault(path, f64(v))
	case "bool-default":
		doc.SetBoolDefault(path, v == "true")
	case "string-default":
		doc.SetStringDefault(path, unescapeOpsTest(v))
	case "datetime-default":
		doc.SetDateTimeDefault(path, dt(v))
	case "int-array":
		doc.SetIntArray(path, ints(arr))
	case "float-array":
		doc.SetFloatArray(path, flts(arr))
	case "bool-array":
		doc.SetBoolArray(path, bools(arr))
	case "string-array":
		doc.SetStringArray(path, strs(arr))
	case "datetime-array":
		doc.SetDateTimeArray(path, dts(arr))
	case "int-array-default":
		doc.SetIntArrayDefault(path, ints(arr))
	case "float-array-default":
		doc.SetFloatArrayDefault(path, flts(arr))
	case "bool-array-default":
		doc.SetBoolArrayDefault(path, bools(arr))
	case "string-array-default":
		doc.SetStringArrayDefault(path, strs(arr))
	case "datetime-array-default":
		doc.SetDateTimeArrayDefault(path, dts(arr))
	case "raw":
		doc.SetRaw(path, unescapeOpsTest(get(3)), v)
	case "raw-default":
		doc.SetRawDefault(path, unescapeOpsTest(get(3)), v)
	case "empty":
		doc.SetEmpty(path)
	case "comment":
		doc.SetComment(path, v)
	case "remove":
		doc.Remove(path)
	default:
		t.Fatalf("%s: unknown op '%s'", at, f[0])
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
