/*
 * embed.go - Embedded static assets
 *
 * Uses Go's embed directive to compile all web assets into the binary.
 *
 * Copyright (C) 2026 Andy Taylor (MW0MWZ)
 * SPDX-License-Identifier: GPL-2.0-only
 */

package main

import (
	"embed"
	"io/fs"
)

//go:embed web/templates web/static
var content embed.FS

// staticFileSystem returns the embedded static files as an fs.FS
// rooted at web/static/ so paths like /static/css/main.css work.
func staticFileSystem() fs.FS {
	sub, err := fs.Sub(content, "web/static")
	if err != nil {
		panic("embed: web/static not found — this is a build error")
	}
	return sub
}
