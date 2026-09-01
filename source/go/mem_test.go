package shcl

// Allocation bounds the corpus cannot see: a capped parse must not hold what
// it is about to refuse.

import (
	"runtime"
	"strings"
	"testing"
)

// heldBy returns the heap bytes still live after f ran, with what f returned
// kept alive across the measurement: what the document holds, not what the
// parse allocated on the way (a per-line message the cap drops is garbage
// the collector takes back, not growth).
func heldBy(f func() any) uint64 {
	var before, after runtime.MemStats
	runtime.GC()
	runtime.ReadMemStats(&before)
	keep := f()
	runtime.GC()
	runtime.ReadMemStats(&after)
	runtime.KeepAlive(keep)
	if after.HeapAlloc < before.HeapAlloc {
		return 0 // the second collection also took back something older
	}
	return after.HeapAlloc - before.HeapAlloc
}

func TestElementCapBoundsTheParse(t *testing.T) {
	text := "arr: " + strings.Repeat("1, ", 200000) + "\nok: 5\n"
	capped := heldBy(func() any {
		doc, _ := ParseLimited(text, Standard, 0, 8, 0)
		if doc.LostCount() != 1 {
			t.Fatalf("lost %d", doc.LostCount())
		}
		return doc
	})
	// The refused line used to be built in full first (78x the text), so the
	// cap saved nothing. What a capped parse holds is the text and its lines.
	if capped > uint64(len(text))*8 {
		t.Fatalf("a capped parse held the array it refused: %d bytes for %d of text", capped, len(text))
	}
}

func TestDiagnosticCapBoundsTheParse(t *testing.T) {
	// The stacked spelling refuses each element line past the cap on its own,
	// and every refusal is a diagnostic: with the element cap alone, 200k
	// refused lines cost more than the elements they refused. The diagnostic
	// cap is what bounds that.
	text := "arr:\n" + strings.Repeat("\t* 1\n", 200000)
	capped := heldBy(func() any {
		doc, _ := ParseLimited(text, Standard, 0, 8, 100)
		if len(doc.Diagnostics()) != 101 || doc.LostCount() != 200000-8 {
			t.Fatalf("diags %d lost %d", len(doc.Diagnostics()), doc.LostCount())
		}
		return doc
	})
	if capped > uint64(len(text))*8 {
		t.Fatalf("a diagnostic-capped parse held its unlisted diagnostics: %d bytes for %d of text", capped, len(text))
	}
}
