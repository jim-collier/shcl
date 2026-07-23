#••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••
## install.ps1
##
##	Release installer for shcl (Simple Hierarchical Config Language) on Windows.
##	Downloads the latest release from GitHub, verifies the checksum, and lays
##	out the binary plus the drop-in source files and wrappers. Idempotent:
##	re-running updates an existing install in place.
##
##	Usage (one-liner, defaults):
##		irm https://raw.githubusercontent.com/jim-collier/shcl/main/install.ps1 | iex
##	With options (download first, or wrap in a script block):
##		& ([scriptblock]::Create((irm https://raw.githubusercontent.com/jim-collier/shcl/main/install.ps1))) -Target user -Yes
##
##	Options:
##		-Release <dev|stable>   dev = newest release including pre-releases
##		                        (default); stable = newest full release.
##		-Target <user|system>   system (default): C:\Program Files\Shcl, added
##		                        to the machine PATH (needs an elevated shell).
##		                        user: %USERPROFILE%\bin\Shcl, with shcl.exe
##		                        copied to %USERPROFILE%\bin and that dir added
##		                        to the user PATH.
##		-Yes                    skip the confirmation prompt.
##
##	Layout under the install dir:
##		shcl.exe    the CLI binary
##		code\       drop-in single-file bindings (lib.rs, shcl.go, shcl.py,
##		            shcl.h, shcl.hpp)
##		scripts\    the PowerShell wrapper (shcl.ps1) and bash wrapper
#••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••

##	Copyright © 2026 Jim Collier (ID: 1cv◂‡Vᛦ)
##	Licensed under The MIT License (MIT). Full text at:
##		https://mit-license.org/
##	SPDX-License-Identifier: MIT

param(
	[ValidateSet('dev', 'development', 'stable')] [string]$Release = 'dev',
	[ValidateSet('user', 'system')] [string]$Target = 'system',
	[switch]$Yes
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if ($Release -eq 'development') { $Release = 'dev' }

$repo = 'jim-collier/shcl'

function Fail([string]$msg) {
	[Console]::Error.WriteLine("install.ps1: $msg")
	exit 1
}

## Windows only; elsewhere install.bash (Linux) or build from source.
if (($PSVersionTable.PSVersion.Major -ge 6) -and -not $IsWindows) {
	Fail 'this installer is for Windows - on Linux use install.bash, elsewhere build from source (see README.md)'
}

$arch = switch ($env:PROCESSOR_ARCHITECTURE) {
	'AMD64' { 'x86_64' }
	'ARM64' { 'arm64' }
	default { Fail "no prebuilt binary for $($env:PROCESSOR_ARCHITECTURE)" }
}

## Resolve the tag. GitHub's /releases/latest is exactly "newest non-prerelease";
## dev takes the newest of everything.
$api = if ($Release -eq 'stable') { "https://api.github.com/repos/$repo/releases/latest" }
	else { "https://api.github.com/repos/$repo/releases?per_page=1" }
try { $rel = Invoke-RestMethod -Uri $api } catch { Fail "cannot fetch the $Release release (none published yet, or network down)" }
if ($rel -is [array]) { $rel = $rel[0] }
if (-not $rel -or -not $rel.tag_name) { Fail "no $Release release found" }
$tag = $rel.tag_name
$version = $tag.TrimStart('v')

## Destinations. A system install writes under Program Files and the machine
## PATH, so it needs an elevated shell.
if ($Target -eq 'system') {
	$principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
	if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
		Fail 'a system install needs an elevated shell (or pass -Target user)'
	}
	$dest = Join-Path $env:ProgramFiles 'Shcl'
	$pathScope = 'Machine'
	$pathDir = $dest
} else {
	$dest = Join-Path $env:USERPROFILE 'bin\Shcl'
	$pathScope = 'User'
	$pathDir = Join-Path $env:USERPROFILE 'bin'
}

## State the plan, then confirm.
$existing = if (Test-Path (Join-Path $dest 'shcl.exe')) { 'updates the existing install' } else { 'new install' }
Write-Output "shcl $version ($Release, windows-$arch) -> $dest ($existing)"
Write-Output "  binary   $dest\shcl.exe"
Write-Output "  drop-ins $dest\code\, wrappers $dest\scripts\"
Write-Output "  adds $pathDir to the $pathScope PATH if missing"
if (-not $Yes) {
	$reply = Read-Host 'Proceed? [y/N]'
	if ($reply -notin @('y', 'Y')) { Write-Output 'aborted'; exit 1 }
}

$tmp = Join-Path ([IO.Path]::GetTempPath()) ("shcl-install-" + [IO.Path]::GetRandomFileName())
New-Item -ItemType Directory -Path $tmp | Out-Null
try {
	## Download and verify the binary.
	$asset = "shcl-$version-windows-$arch.exe"
	$base = "https://github.com/$repo/releases/download/$tag"
	Write-Output "downloading $asset..."
	Invoke-WebRequest -Uri "$base/$asset" -OutFile (Join-Path $tmp 'shcl.exe')
	Invoke-WebRequest -Uri "$base/shcl-$version-sha256sums.txt" -OutFile (Join-Path $tmp 'sums.txt')
	$want = (Get-Content (Join-Path $tmp 'sums.txt') | Where-Object { $_ -match [regex]::Escape($asset) } | ForEach-Object { ($_ -split '\s+')[0] } | Select-Object -First 1)
	$got = (Get-FileHash -Algorithm SHA256 (Join-Path $tmp 'shcl.exe')).Hash.ToLower()
	if (-not $want -or $got -ne $want.ToLower()) { Fail "sha256 mismatch on $asset" }

	## Drop-in code files and wrappers come from the tag's source zipball.
	Write-Output "downloading source payload ($tag)..."
	Invoke-WebRequest -Uri "https://github.com/$repo/archive/refs/tags/$tag.zip" -OutFile (Join-Path $tmp 'src.zip')
	Expand-Archive -Path (Join-Path $tmp 'src.zip') -DestinationPath $tmp
	$srcroot = Get-ChildItem -Directory -Path $tmp -Filter 'shcl-*' | Select-Object -First 1
	if (-not $srcroot) { Fail 'unexpected source zipball layout' }

	## Install. The binary goes in via a temp name + Move-Item in the same dir,
	## so a running copy only ever sees the complete old or new file.
	New-Item -ItemType Directory -Force -Path $dest, (Join-Path $dest 'code'), (Join-Path $dest 'scripts') | Out-Null
	Copy-Item (Join-Path $tmp 'shcl.exe') (Join-Path $dest '.shcl.exe.new')
	Move-Item -Force (Join-Path $dest '.shcl.exe.new') (Join-Path $dest 'shcl.exe')
	$s = $srcroot.FullName
	Copy-Item "$s\source\rust\src\lib.rs", "$s\source\go\shcl.go", "$s\source\python\shcl.py", "$s\source\c\shcl.h", "$s\source\c\shcl.hpp" (Join-Path $dest 'code')
	Copy-Item "$s\source\powershell\shcl.ps1", "$s\source\bash\shcl.bash" (Join-Path $dest 'scripts')
	## A user install also drops the exe in %USERPROFILE%\bin so PATH finds it
	## directly (a real symlink needs elevation or developer mode).
	if ($Target -eq 'user') {
		Copy-Item (Join-Path $dest 'shcl.exe') (Join-Path $pathDir '.shcl.exe.new')
		Move-Item -Force (Join-Path $pathDir '.shcl.exe.new') (Join-Path $pathDir 'shcl.exe')
	}

	## PATH, idempotently.
	$current = [Environment]::GetEnvironmentVariable('Path', $pathScope)
	if (($current -split ';') -notcontains $pathDir) {
		[Environment]::SetEnvironmentVariable('Path', "$current;$pathDir", $pathScope)
		Write-Output "added $pathDir to the $pathScope PATH (new shells pick it up)"
	}

	Write-Output "installed shcl $version -> $dest\shcl.exe"
	& (Join-Path $dest 'shcl.exe') version
} finally {
	Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}
