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
##		shcl                 the whole CLI: get|set|fmt|check|init|count|instances|
##		                     children|paths ...
##		                     (pipeline input is forwarded, so `set` can be piped)
##		shcl_get             read a string (the default type)
##		shcl_int shcl_float shcl_bool shcl_datetime shcl_raw
##		                     read one typed value
##		shcl_array           read an array (pass a --type, else --string)
##		shcl_fmt shcl_check shcl_count shcl_instances shcl_children shcl_paths
##		                     the matching subcommands
##
##	Finding the binary (first hit wins):
##		$env:SHCL_BIN, else a `shcl` beside this file, else `shcl` on PATH, else
##		the repo release/debug build. Set SHCL_BIN to pin an exact one. On Windows
##		a bare name also matches its `.exe`.
##
##	Exit codes (straight from the binary): 0 good, 1 usage/IO, 2 empty,
##	3 not found, 4 bad type, 5 multiple instances, 6 check failed, strict
##	load failure, or a faulty init schema, 7 in-place write refused
##	(--lossy overrides). A nonzero code is not an error to PowerShell - unless
##	$PSNativeCommandUseErrorActionPreference is on under an ErrorActionPreference
##	of Stop, where a not-found read throws instead of returning 3.
##
##	Runs on Windows PowerShell 5.1 and PowerShell 7 on Windows; elsewhere it
##	needs PowerShell 7.3 or newer (the execute-bit check reads UnixFileMode).
#••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••

##	Copyright © 2026 Jim Collier (CryptogID: ѳ6ᴚ℈𐀘𐇦ɛ𐊁¥Mﾏb϶Δ𐌞)
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
## The version test comes first so 5.1 never reads $IsWindows, which does not
## exist there and throws under a caller's strict mode.
function _shcl_executable([string]$path) {
	if (-not (Test-Path -LiteralPath $path -PathType Leaf))       { return $false }
	if ($PSVersionTable.PSVersion.Major -lt 6 -or $IsWindows)     { return $true }
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
function _shcl_scriptdir([string]$self = $PSCommandPath) {
	if (-not $self) { return $PSScriptRoot }
	$item = Get-Item -LiteralPath $self -ErrorAction SilentlyContinue
	## ResolveLinkTarget arrived in .NET 6, so Windows PowerShell 5.1 has no
	## such method and calling it is an error the SilentlyContinue above does
	## not cover. Without it the path is used as given, links and all.
	if ($item -and ($item.PSObject.Methods.Name -contains 'ResolveLinkTarget')) {
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
## Pipeline input only reaches a function through $input, and only forward it
## when there is some: an empty $input would hand the binary a closed stdin, so
## `set` would read zero ops instead of falling through to the console.
function shcl {
	if (-not (_shcl_resolve)) { $global:LASTEXITCODE = 1; return }
	if ($MyInvocation.ExpectingInput) { $input | & $script:_SHCL_BIN @args }
	else { & $script:_SHCL_BIN @args }
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
function shcl_children { shcl children @args }
function shcl_paths { shcl paths @args }

#••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••
# Run path
#••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••

## When executed (not dot-sourced), be the CLI: forward args, forward the code.
## InvocationName is '.' only when dot-sourced.
if ($MyInvocation.InvocationName -ne '.') {
	shcl @args
	## Spelled the long way rather than with ??, so this runs on the Windows
	## PowerShell 5.1 that ships with the OS as well as on 7.
	$rc = $LASTEXITCODE
	if ($null -eq $rc) { $rc = 1 }   ## null only if nothing ran; treat as usage/IO
	exit $rc
}
