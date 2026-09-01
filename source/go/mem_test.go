package shcl

// Allocation bounds the corpus cannot see: a capped parse must not hold what
// it is about to refuse.

import (
	"runtime"
	"strings"
	"testing"
)

// allocatedBy returns the bytes the runtime handed out while f ran.
func allocatedBy(f func()) uint64 {
	var before, after runtime.MemStats
	runtime.GC()
	runtime.ReadMemStats(&before)
	f()
	runtime.ReadMemStats(&after)
	return after.TotalAlloc - before.TotalAlloc
}

func TestElementCapBoundsTheParse(t *testing.T) {
	text := "arr: " + strings.Repeat("1, ", 200000) + "\nok: 5\n"
	capped := allocatedBy(func() {
		doc, _ := ParseLimited(text, Standard, 0, 8)
		if doc.LostCount() != 1 {
			t.Fatalf("lost %d", doc.LostCount())
		}
	})
	// The refused line used to be built in full first (78x the text), so the
	// cap saved nothing. What a capped parse holds is the text and its lines.
	if capped > uint64(len(text))*8 {
		t.Fatalf("a capped parse held the array it refused: %d bytes for %d of text", capped, len(text))
	}
}
