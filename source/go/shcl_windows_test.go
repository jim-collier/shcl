// SPDX-License-Identifier: MIT
// Copyright © 2026 Jim Collier (CryptogID: ѳ6ᴚ℈𐀘𐇦ɛ𐊁¥Mﾏb϶Δ𐌞)

//go:build windows

package shcl

import (
	"os"
	"path/filepath"
	"syscall"
	"testing"
)

// Hidden and system survive a save. ReplaceFile's documented preserve list does
// not include the basic attributes and the fallback rename carries none, so a
// hidden config used to come back visible. Same fixture in every runner; its
// own file because the symbols are windows-only.
func TestSaveKeepsHiddenAndSystem(t *testing.T) {
	f := filepath.Join(t.TempDir(), "h.shcl")
	if err := os.WriteFile(f, []byte("a: 1\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	p, perr := syscall.UTF16PtrFromString(f)
	if perr != nil {
		t.Fatal(perr)
	}
	if serr := syscall.SetFileAttributes(p, syscall.FILE_ATTRIBUTE_HIDDEN|syscall.FILE_ATTRIBUTE_SYSTEM); serr != nil {
		t.Fatal(serr)
	}
	if err := Parse("a: 2\n").SaveFile(f); err != nil {
		t.Fatal(err)
	}
	after, aerr := syscall.GetFileAttributes(p)
	if aerr != nil {
		t.Fatal(aerr)
	}
	if after&syscall.FILE_ATTRIBUTE_HIDDEN == 0 {
		t.Error("file did not come back hidden")
	}
	if after&syscall.FILE_ATTRIBUTE_SYSTEM == 0 {
		t.Error("file did not come back system")
	}
	_ = syscall.SetFileAttributes(p, syscall.FILE_ATTRIBUTE_NORMAL)
}
