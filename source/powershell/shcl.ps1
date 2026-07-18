#!/usr/bin/env pwsh
#••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••
## shcl.ps1
##
##	Shell binding for shcl (Simple Hierarchical Config Language): a convenient
##	front end to the compiled `shcl` binary. Run it as a script, or dot-source
##	it and call the functions - both take the same arguments and hand back the
##	binary's exit code (in $LASTEXITCODE).
##
##	Usage (as a script):
##		pwsh shcl.ps1 get --int  app.shcl  server.port
##		pwsh shcl.ps1 fmt --write app.shcl
##		pwsh shcl.ps1 check app.shcl
##
##	Usage (dot-sourced):
##		. ./shcl.ps1
##		$port = shcl get --int app.shcl server.port     ## the whole CLI
##		$port = shcl_int app.shcl server.port           ## same, typed helper
##		$host = shcl_get app.shcl server.host
##		if ((shcl_bool app.shcl features.debug) -eq 'true') { Enable-Debug }
##		$hosts = shcl_array --string app.shcl cluster.hosts
##
##	Functions defined when dot-sourced (each mirrors the CLI, sets $LASTEXITCODE):
##		shcl                 the whole CLI: get|fmt|check|count|instances ...
##		shcl_get             read a string (the default type)
##		shcl_int shcl_float shcl_bool shcl_datetime shcl_raw
##		                     read one typed value
##		shcl_array           read an array (pass a --type, else --string)
##		shcl_fmt shcl_check shcl_count shcl_instances
##		                     the matching subcommands
##
##	Finding the binary (first hit wins):
##		$env:SHCL_BIN, else a `shcl` beside this file, else `shcl` on PATH, else
##		the repo release/debug build. Set SHCL_BIN to pin an exact one. On Windows
##		a bare name also matches its `.exe`.
##
##	Exit codes (straight from the binary): 0 good, 1 usage/IO, 2 empty,
##	3 not found, 4 bad type, 5 multiple instances, 6 strict load failure.
#••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••

##	Copyright © 2026 Jim Collier (ID: 1cv◂‡Vᛦ)
##	Licensed under The MIT License (MIT). Full text at:
##		https://mit-license.org/
##	SPDX-License-Identifier: MIT

## No script-level param block on purpose: it would try to bind `get`/`--int` as
## parameters. Without one, every argument lands in $args verbatim, exactly what
## a passthrough front end wants.

#••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••
# Core
#••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••

## Plain one-line stderr message (Write-Error decorates with a multi-line block).
function _shcl_err([string]$msg) { [Console]::Error.WriteLine($msg) }

## A base path is a match if it exists, or (Windows) if base.exe does. Returns
## the concrete path or $null.
function _shcl_exe([string]$base) {
	if (Test-Path -LiteralPath $base -PathType Leaf)        { return $base }
	if (Test-Path -LiteralPath "$base.exe" -PathType Leaf)  { return "$base.exe" }
	return $null
}

## Locate the shcl binary and cache it in $script:_SHCL_BIN. SHCL_BIN, if set,
## always wins; the PATH probe asks for an Application so our own shcl() function
## can't shadow it.
$script:_SHCL_BIN = $null
function _shcl_resolve {
	if ($env:SHCL_BIN) {
		if (Test-Path -LiteralPath $env:SHCL_BIN -PathType Leaf) { $script:_SHCL_BIN = $env:SHCL_BIN; return $true }
		_shcl_err "shcl.ps1: SHCL_BIN is set but not found: $($env:SHCL_BIN)"
		return $false
	}
	if ($script:_SHCL_BIN -and (Test-Path -LiteralPath $script:_SHCL_BIN -PathType Leaf)) { return $true }
	$onPath = Get-Command shcl -CommandType Application -ErrorAction SilentlyContinue |
		Select-Object -First 1 -ExpandProperty Source
	$candidates = @(
		(_shcl_exe (Join-Path $PSScriptRoot 'shcl')),
		$onPath,
		(_shcl_exe (Join-Path $PSScriptRoot '../rust/target/release/shcl')),
		(_shcl_exe (Join-Path $PSScriptRoot '../rust/target/debug/shcl'))
	)
	foreach ($candidate in $candidates) {
		if ($candidate -and (Test-Path -LiteralPath $candidate -PathType Leaf)) {
			$script:_SHCL_BIN = $candidate
			return $true
		}
	}
	_shcl_err "shcl.ps1: cannot find a shcl binary (set SHCL_BIN, or put it on PATH)"
	return $false
}

## The whole CLI. Everything else is sugar over this. On a resolve failure we
## mimic the binary's own usage/IO code so callers still see a nonzero.
function shcl {
	if (-not (_shcl_resolve)) { $global:LASTEXITCODE = 1; return }
	& $script:_SHCL_BIN @args
}

#••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••
# Typed sugar (dot-sourced use; the reason to source rather than call the binary)
#••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••

function shcl_get       { shcl get @args }
function shcl_int       { shcl get --int @args }
function shcl_float     { shcl get --float @args }
function shcl_bool      { shcl get --bool @args }
function shcl_datetime  { shcl get --datetime @args }
function shcl_raw       { shcl get --raw @args }
function shcl_array     { shcl get --array @args }   ## prefix a --type, else string
function shcl_fmt       { shcl fmt @args }
function shcl_check     { shcl check @args }
function shcl_count     { shcl count @args }
function shcl_instances { shcl instances @args }

#••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••
# Run path
#••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••

## When executed (not dot-sourced), be the CLI: forward args, forward the code.
## InvocationName is '.' only when dot-sourced.
if ($MyInvocation.InvocationName -ne '.') {
	shcl @args
	exit $LASTEXITCODE
}
