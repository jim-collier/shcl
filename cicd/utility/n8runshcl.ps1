#!/usr/bin/env pwsh
#••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••
## n8runshcl.ps1
##
##	Runs the newest release build of shcl without disturbing the dogfooded
##	install. Independent of the cicd pipeline - it never builds anything, it
##	just takes whatever the last release build left behind.
##
##	Each run stamps a dated copy into its own directory, well away from PATH
##	and under a name nothing else can claim, so a copy already running is never
##	overwritten underneath itself. Older copies are removed once nothing is
##	using them.
##
##	Usage:
##		pwsh n8runshcl.ps1 get --int app.shcl server.port
##		pwsh n8runshcl.ps1 -ListCopies
##		pwsh n8runshcl.ps1 -NoLaunch          ## stage a copy, print its path
##
##	Everything after the script's own options is handed to the binary as-is.
##	Put `--` first if an argument would otherwise bind as a script parameter.
##
##	Where things go:
##		copies   <repo>/cicd/artifacts/runbuilds/shcl-<yyyyMMdd-HHmmss>[.exe]
##		source   <repo>/source/rust/target/release/shcl[.exe]
##
##	The exit code is the binary's own.
#••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••

##	Copyright © 2026 Jim Collier (CryptogID: ѳ6ᴚ℈𐀘𐇦ɛ𐊁¥Mﾏb϶Δ𐌞)
##	Licensed under The MIT License (MIT). Full text at:
##		https://mit-license.org/
##	SPDX-License-Identifier: MIT

<#
.SYNOPSIS
Runs the newest release build of shcl from a private stamped copy.
.DESCRIPTION
Stages the last release build under cicd/artifacts/runbuilds with a date stamp, launches it with the remaining arguments, and prunes older copies that nothing is using. Never builds; never touches the dogfooded install.
.PARAMETER Keep
How many stamped copies to keep, newest first. Copies in use are neither counted nor removed.
.PARAMETER ListCopies
List the staged copies and exit.
.PARAMETER NoLaunch
Stage a copy and print its path instead of running it.
.PARAMETER Rest
Everything else, handed to the binary as-is. Put -- first if an argument would otherwise bind as a script parameter.
.EXAMPLE
pwsh n8runshcl.ps1 get --int app.shcl server.port
#>
[CmdletBinding()]
param(
	## How many stamped copies to keep, newest first. In-use copies are never
	## counted against it, and never removed. Named only - $Rest owns position 0,
	## or the binary's own first argument would bind here instead of passing on.
	[ValidateRange(1, 100)] [int]$Keep = 5,
	[switch]$ListCopies,
	[switch]$NoLaunch,
	[Parameter(Position = 0, ValueFromRemainingArguments = $true)] [string[]]$Rest
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$exeSuffix = if ($IsWindows) { '.exe' } else { '' }
$repoRoot  = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$sourceBin = Join-Path $repoRoot "source/rust/target/release/shcl$exeSuffix"
$copyDir   = Join-Path $repoRoot 'cicd/artifacts/runbuilds'

#••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••
# Helpers
#••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••

function Get-StagedCopy {
	## Newest first. The stamp sorts lexically, so the name is the ordering.
	## The comma keeps a one-element result an array - returning it bare would
	## unroll to a scalar, and every caller here counts what it gets back.
	if (-not (Test-Path -LiteralPath $copyDir)) { return , @() }
	$found = @(Get-ChildItem -LiteralPath $copyDir -File -Filter "shcl-*$exeSuffix" |
		Sort-Object -Property Name -Descending)
	return , $found
}

function Test-CopyInUse([string]$path) {
	## Windows holds an exclusive lock on a running image, so a write open that
	## fails is the answer, and no process walk is needed.
	if ($IsWindows) {
		try {
			$fs = [System.IO.File]::Open($path, 'Open', 'Write', 'None')
			$fs.Dispose()
			return $false
		} catch { return $true }
	}
	## Everywhere else, ask which running image each process was started from.
	## Processes owned by somebody else report no path rather than throwing, so
	## the worst case is keeping a copy a moment longer than needed.
	$resolved = (Resolve-Path -LiteralPath $path).Path
	$running  = @(Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $resolved })
	return ($running.Count -gt 0)
}

function Remove-AgedCopy {
	[CmdletBinding(SupportsShouldProcess)]
	param([int]$Keep)

	## Keep the newest few, then drop what is left - but never a copy something
	## is still running, however old it is.
	$all = Get-StagedCopy
	if ($all.Count -le $Keep) { return }
	foreach ($stale in $all[$Keep..($all.Count - 1)]) {
		if (Test-CopyInUse $stale.FullName) {
			Write-Verbose "in use, keeping: $($stale.Name)"
			continue
		}
		if ($PSCmdlet.ShouldProcess($stale.FullName, 'Remove aged copy')) {
			Remove-Item -LiteralPath $stale.FullName -Force -ErrorAction SilentlyContinue
			Write-Verbose "removed: $($stale.Name)"
		}
	}
}

#••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••
# Main
#••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••

if ($ListCopies) {
	## Objects rather than formatted lines, so this composes with Where-Object
	## and friends. Formatting is the caller's business.
	foreach ($c in (Get-StagedCopy)) {
		[PSCustomObject]@{
			Name    = $c.Name
			Bytes   = $c.Length
			Built   = $c.LastWriteTime
			InUse   = (Test-CopyInUse $c.FullName)
			Path    = $c.FullName
		}
	}
	exit 0
}

if (-not (Test-Path -LiteralPath $sourceBin)) {
	Write-Error "no release build at $sourceBin - run cicd/cicd.bash first"
	exit 1
}

New-Item -ItemType Directory -Force -Path $copyDir | Out-Null

## Stamp from the build's own mtime, not the clock: running twice against one
## build should reuse that build's copy rather than pile up identical ones.
$stamp  = (Get-Item -LiteralPath $sourceBin).LastWriteTime.ToString('yyyyMMdd-HHmmss')
$staged = Join-Path $copyDir "shcl-$stamp$exeSuffix"

if (-not (Test-Path -LiteralPath $staged)) {
	Copy-Item -LiteralPath $sourceBin -Destination $staged -Force
	if (-not $IsWindows) { & chmod '+x' $staged }
}

Remove-AgedCopy -Keep $Keep

if ($NoLaunch) { Write-Output $staged; exit 0 }

& $staged @Rest
exit $LASTEXITCODE

##	History:
##		- 2026-08-19 JC: Created. Stamped copies off the release build, aged out when nothing is running them, arguments passed straight through.
