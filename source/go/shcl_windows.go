// SPDX-License-Identifier: MIT
// Copyright © 2026 Jim Collier [ID: 2უNაɘ«҂թȹɤξπ๙¿ձϖ]

//go:build windows

// Windows has no rename that keeps the destination's ACLs, attributes and named
// streams; ReplaceFile exists for exactly this publish step and carries them
// onto the replacement. shcl.go is deliberately droppable on its own and so
// cannot reach a windows-only symbol, which is why this arrives as a hook: with
// this file the module upgrades the publish, without it a rename still works.
package shcl

import (
	"os"
	"strings"
	"syscall"
	"unsafe"
)

func init() {
	publishFile = windowsPublishFile
	publishNewFile = windowsPublishNewFile
	carriedAttrs = windowsCarriedAttrs
	restoreAttrs = windowsRestoreAttrs
	notADiskFile = windowsNotADiskFile
}

// A device carries the same ARCHIVE bit an ordinary file does, so an attribute
// test cannot tell them apart. The handle is the OS's own answer, and it keeps
// an exotic but real path (a volume-prefixed `\\.\C:\dir\file`) out of the
// device case. CON refuses that open outright (ERROR_INVALID_PARAMETER) and a
// serial port nobody has answers not-found, so a failed open cannot mean
// "nothing is there": a reserved name resolves into the device namespace from
// whatever directory it is typed in, and the full path is where that shows.
func windowsNotADiskFile(path string) bool {
	const fileReadAttributes = 0x80
	p, perr := syscall.UTF16PtrFromString(path)
	if perr != nil {
		return false
	}
	h, herr := syscall.CreateFile(p, fileReadAttributes,
		syscall.FILE_SHARE_READ|syscall.FILE_SHARE_WRITE|syscall.FILE_SHARE_DELETE,
		nil, syscall.OPEN_EXISTING, syscall.FILE_FLAG_BACKUP_SEMANTICS, 0)
	if herr == nil {
		kind, kerr := syscall.GetFileType(h)
		syscall.CloseHandle(h)
		return kerr == nil && kind != syscall.FILE_TYPE_DISK
	}
	full, ferr := syscall.FullPath(path)
	return ferr == nil && strings.HasPrefix(full, `\\.\`)
}

// A move without MOVEFILE_REPLACE_EXISTING refuses a target that exists, which
// is all a create needs, and it works on volumes with no hard links.
func windowsPublishNewFile(tmp, target string) error {
	from, ferr := syscall.UTF16PtrFromString(tmp)
	if ferr != nil {
		return ferr
	}
	to, terr := syscall.UTF16PtrFromString(target)
	if terr != nil {
		return terr
	}
	return syscall.MoveFile(from, to)
}

// ReplaceFile's documented preserve list is creation time, short name, object
// id, DACLs, security attributes, encryption, compression and named streams -
// not the basic attributes - and the fallback rename carries nothing at all, so
// hidden and system are re-applied by hand after the publish. Read-only is
// handled separately: it has to come OFF before the publish.
const carriedFileAttrs = syscall.FILE_ATTRIBUTE_HIDDEN | syscall.FILE_ATTRIBUTE_SYSTEM

func windowsCarriedAttrs(fi os.FileInfo) uint32 {
	d, ok := fi.Sys().(*syscall.Win32FileAttributeData)
	if !ok {
		return 0
	}
	return d.FileAttributes & carriedFileAttrs
}

func windowsRestoreAttrs(path string, bits uint32) {
	p, err := syscall.UTF16PtrFromString(path)
	if err != nil {
		return
	}
	now, aerr := syscall.GetFileAttributes(p)
	if aerr != nil {
		return
	}
	_ = syscall.SetFileAttributes(p, now|bits)
}

// REPLACEFILE_WRITE_THROUGH: asked for, and documented by Microsoft as not
// supported by ReplaceFile. Durability here rests on the file's own fsync
// before the publish; the fallback move's own WRITE_THROUGH is the one that
// means something.
const replaceFileWriteThrough = 0x1

// kernel32 by name is loaded from the system directory, not the working one -
// syscall keeps a list of the DLLs that deserve that and kernel32 is on it.
var procReplaceFileW = syscall.NewLazyDLL("kernel32.dll").NewProc("ReplaceFileW")

// ReplaceFile needs the destination to exist, and it fails rather than skip a
// merge it cannot do (no WRITE_DAC, say), so a create and any failure fall back
// to the plain rename this replaces.
func windowsPublishFile(tmp, target string) error {
	if _, serr := os.Stat(target); serr == nil && replaceFileW(tmp, target) == nil {
		return nil
	}
	return os.Rename(tmp, target)
}

func replaceFileW(tmp, target string) error {
	if ferr := procReplaceFileW.Find(); ferr != nil {
		return ferr
	}
	replaced, rerr := syscall.UTF16PtrFromString(target)
	if rerr != nil {
		return rerr
	}
	replacement, perr := syscall.UTF16PtrFromString(tmp)
	if perr != nil {
		return perr
	}
	r, _, callErr := procReplaceFileW.Call(
		uintptr(unsafe.Pointer(replaced)), uintptr(unsafe.Pointer(replacement)),
		0, uintptr(replaceFileWriteThrough), 0, 0)
	if r == 0 {
		return callErr
	}
	return nil
}
