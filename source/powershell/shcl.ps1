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
##		$svrhost = shcl_get app.shcl server.host        ## ($host is read-only)
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

## Launchable? On Unix require an execute bit (any of user/group/other); on
## Windows (or pre-6 PowerShell, where $IsWindows is undefined) a leaf is enough,
## the OS decides by extension. Mirrors bash's `-x` test at every resolution site.
function _shcl_executable([string]$path) {
	if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { return $false }
	if ($IsWindows -or ($null -eq $IsWindows))              { return $true }
	$mode = (Get-Item -LiteralPath $path).UnixFileMode
	$exec = [System.IO.UnixFileMode]::UserExecute  -bor `
	        [System.IO.UnixFileMode]::GroupExecute -bor `
	        [System.IO.UnixFileMode]::OtherExecute
	return ($mode -band $exec) -ne 0
}

## A base path is a match if it is launchable, or (Windows) if base.exe is.
## Returns the concrete path or $null.
function _shcl_exe([string]$base) {
	if (_shcl_executable $base)        { return $base }
	if (_shcl_executable "$base.exe")  { return "$base.exe" }
	return $null
}

## Real directory of this file, following symlinks, so a linked-in copy still
## finds its sibling binary and the repo release/debug build tree.
function _shcl_scriptdir {
	$self = $PSCommandPath
	if (-not $self) { return $PSScriptRoot }
	$item = Get-Item -LiteralPath $self -ErrorAction SilentlyContinue
	if ($item) {
		$target = $item.ResolveLinkTarget($true)
		if ($target) { $self = $target.FullName }
	}
	return [System.IO.Path]::GetDirectoryName($self)
}
$script:_SHCL_ROOT = _shcl_scriptdir

## Locate the shcl binary and cache it in $script:_SHCL_BIN. SHCL_BIN, if set,
## always wins; the PATH probe asks for an Application so our own shcl() function
## can't shadow it.
$script:_SHCL_BIN = $null
function _shcl_resolve {
	if ($env:SHCL_BIN) {
		$pinned = _shcl_exe $env:SHCL_BIN         ## same .exe fallback the others get
		if ($pinned) { $script:_SHCL_BIN = $pinned; return $true }
		_shcl_err "shcl.ps1: SHCL_BIN is set but not executable: $($env:SHCL_BIN)"
		return $false
	}
	if ($script:_SHCL_BIN -and (_shcl_executable $script:_SHCL_BIN)) { return $true }
	$onPath = Get-Command shcl -CommandType Application -ErrorAction SilentlyContinue |
		Select-Object -First 1 -ExpandProperty Source
	$candidates = @(
		(_shcl_exe (Join-Path $script:_SHCL_ROOT 'shcl')),
		$onPath,
		(_shcl_exe (Join-Path $script:_SHCL_ROOT '../rust/target/release/shcl')),
		(_shcl_exe (Join-Path $script:_SHCL_ROOT '../rust/target/debug/shcl'))
	)
	foreach ($candidate in $candidates) {
		if ($candidate -and (_shcl_executable $candidate)) {
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
	exit ($LASTEXITCODE ?? 1)      ## null only if nothing ran; treat as usage/IO
}
