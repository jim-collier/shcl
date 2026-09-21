// SPDX-License-Identifier: MIT
// Copyright © 2026 Jim Collier [ID: 2უNაɘ«҂թȹɤξπ๙¿ձϖ]

//go:build windows

// A --write FILE the CLI is going to refuse must not be read first, and a read
// of CON waits on the console. os.Stat sees a device it can open; a reserved
// name with no device behind it needs the full path, which resolves into the
// device namespace from whatever directory the name is typed in. The library
// has the same call for its own save, private there, so this is its own copy -
// the CLI has to answer before the read, not after.

package main

import (
	"strings"
	"syscall"
)

func init() {
	notADiskFile = windowsNotADiskFile
}

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
