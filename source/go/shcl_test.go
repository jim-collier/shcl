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
		cases = append(cases, corpusCase{
			name:        entry.Name(),
			input:       string(input),
			expectedFmt: string(expected),
			reads:       string(reads),
		})
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
