#••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••
## install.ps1
##
##	Release installer for shcl (Simple Hierarchical Config Language) on Windows.
##	Downloads the latest release from GitHub, checks the sha256sums file against
##	the release signing key before trusting a checksum out of it, and lays out
##	the binary plus the drop-in source files and wrappers. Idempotent:
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
##		                        user: %LOCALAPPDATA%\Programs\Shcl, added to
##		                        the user PATH. No elevation.
##		-Uninstall              remove what an install of the same -Target laid
##		                        down (binary, code\, scripts\, PATH entry).
##		-Yes                    skip the confirmation prompt.
##		-Help                   print the options and exit.
##
##	Layout under the install dir:
##		shcl.exe    the CLI binary
##		code\       drop-in single-file bindings (lib.rs, shcl.go, shcl.py,
##		            shcl.h, shcl.hpp)
##		scripts\    the PowerShell wrapper (shcl.ps1) and bash wrapper
#••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••

##	Copyright © 2026 Jim Collier (CryptogID: ѳ6ᴚ℈𐀘𐇦ɛ𐊁¥Mﾏb϶Δ𐌞)
##	Licensed under The MIT License (MIT). Full text at:
##		https://mit-license.org/
##	SPDX-License-Identifier: MIT

[CmdletBinding()]
param(
	[ValidateSet('dev', 'development', 'stable')] [string]$Release = 'dev',
	[ValidateSet('user', 'system')] [string]$Target = 'system',
	[switch]$Yes,
	[switch]$Uninstall,
	[switch]$Help
)

## The body runs in a scope of its own, so strict mode, the preference
## variables and every function and variable below stay out of the shell that
## piped this script into iex. The options go in as arguments, and so does
## whether a file was invoked, since inside the block $MyInvocation names the
## block: under either documented one-liner there is no script file, and `exit`
## there ends the shell that ran it. Only a file invocation may exit;
## everything else returns or throws.
& {
	param([string]$Release, [string]$Target, [bool]$Yes, [bool]$Uninstall, [bool]$Help, [bool]$invokedAsFile, [string]$scriptPath)

	Set-StrictMode -Version Latest
	$ErrorActionPreference = 'Stop'

	## Spelled out here rather than left to Get-Help: the documented one-liner pipes
	## this script straight into the shell, so there is no file left to ask about.
	if ($Help) {
		Write-Output ''
		@'
shcl installer

Usage:
    & ([scriptblock]::Create((irm https://raw.githubusercontent.com/jim-collier/shcl/main/install.ps1))) -Target user

Options:
    -Release <dev|stable>   dev = newest release including pre-releases
                            (default); stable = newest full release.
    -Target <user|system>   system (default): C:\Program Files\Shcl, added to
                            the machine PATH (needs an elevated shell).
                            user: %LOCALAPPDATA%\Programs\Shcl, added to the
                            user PATH. No elevation needed.
    -Uninstall              remove what an install of the same -Target laid
                            down, and nothing else.
    -Yes                    skip the confirmation prompt.
    -Help                   this text.

The release signature is checked before any checksum is read out of the sums
file. Nothing unverified is installed.
'@ | Write-Output
		Write-Output ''
		return
	}

	if ($Release -eq 'development') { $Release = 'dev' }

	$repo = 'jim-collier/shcl'

	function Exit-Install([string]$Message) {
		if ($invokedAsFile) {
			[Console]::Error.WriteLine("install.ps1: $Message")
			exit 1
		}
		throw "install.ps1: $Message"
	}

	## Release signing key, carried as raw RSA parameters rather than PEM on purpose:
	## ImportFromPem needs PowerShell 7, and 5.1 is still what most Windows boxes
	## run. Modulus + exponent import the same on both. The sha256sums file is signed
	## offline with the matching private key, so replacing a release asset is not
	## enough - an attacker would have to forge this signature too. Baked in rather
	## than fetched, because a key downloaded over the same channel as the artifact
	## proves nothing. Rotating it means publishing a new installer.
	$signingModulus = 'k5W58wTiFTlHUCsIuHESqexain6AC8WwFmCDsjfliOIDa2vPhkSVOqMsSbYH/OL94pHZ+Bs0agNXrl99ANolzwQ4rvu6gAsc4GCb0Krbbq2B+jKqTM8xeN7tFLWKd5E08IOF2HA4ugQSlK+rC6ezbBqP1MuJFFxqxDhEtGef9v/nuhX2kWq3v0uN6Y0umbghuNAR7gmoSOwbb8uYfVOAH1OAWV2To2wyIe6WWt4BPmFJBpEI53k4rmoDVdjmJFoj2vETHmEh2QfTPA5541jPLeuO8p8V6+Aa8i32EtVeT1+ozwHidku/CZZOAdxYZ7yXAZdG3eOOxcHVfmXVwqRxPR+lA3E/KcRcN9oeeveXS35jwH0h3hSh6sJOr1q0qMtM7bB4Lxt47wXHTJ0VPneG5xbmO5pUS3LMcZwnXXavYjh2kYS52ZLhi1JbPFgPyYUiIv76IUbwtpEXbONi12g7fioZ6cStZAekJs33Wkee6NmSY54AozxTkcNUJTgs81eMa/gRL8l3jud8AWqL5vykqpG1PTN70vSgrHD4wNMp2QX29Iv+A6+FO4B1oxjrnokg212rwqX004Ep0csu/JjOl9XHvwp0Iucfi8zCg7ozDcU3dsDnUJ8A3PtJ47jEt1n37/oiM6pWDXVVBjz4DI9iACmdUphTcGhYvn91ORZVxt0='
	$signingExponent = 'AQAB'

	## Detached PKCS#1 v1.5 / SHA-256 signature over a file. Any failure - malformed
	## key, unreadable signature, bad maths - comes back false, never an exception
	## that a caller might mistake for a pass.

	## PATH, idempotently - straight at the registry. [Environment]::Get expands
	## %VAR% references before returning and Set writes the result back REG_SZ,
	## which freezes every reference and downgrades the value type (user PATHs
	## commonly carry %USERPROFILE%). Reading unexpanded and writing
	## REG_EXPAND_SZ keeps the stored value intact. True when it wrote.
	function Update-ShclPath {
		## The installer's own confirm prompt is the gate; a nested -WhatIf
		## plumbing would dead-end at the irm|iex one-liner anyway.
		[Diagnostics.CodeAnalysis.SuppressMessageAttribute('PSUseShouldProcessForStateChangingFunctions', '')]
		param([string]$Scope, [string]$Dir, [switch]$Remove)
		$hive = if ($Scope -eq 'Machine') { [Microsoft.Win32.Registry]::LocalMachine } else { [Microsoft.Win32.Registry]::CurrentUser }
		$subkey = if ($Scope -eq 'Machine') { 'SYSTEM\CurrentControlSet\Control\Session Manager\Environment' } else { 'Environment' }
		$envKey = $hive.OpenSubKey($subkey, $true)
		try {
			$current = [string]$envKey.GetValue('Path', '', [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
			if ($Remove) {
				$new = @($current -split ';' | Where-Object { $_ -and $_ -ne $Dir }) -join ';'
				if ($new -eq $current) { return $false }
				$envKey.SetValue('Path', $new, [Microsoft.Win32.RegistryValueKind]::ExpandString)
				return $true
			}
			if (($current -split ';') -contains $Dir) { return $false }
			$joined = if ($current -and -not $current.EndsWith(';')) { "$current;$Dir" } else { "$current$Dir" }
			$envKey.SetValue('Path', $joined, [Microsoft.Win32.RegistryValueKind]::ExpandString)
			return $true
		} finally {
			$envKey.Close()
		}
	}

	function Select-ReleaseTag([string]$Channel, $Releases) {
		$all = @($Releases) | Where-Object { $_.tag_name -match '^v\d+\.\d+\.\d+' }
		$all = @($all) | Where-Object { -not $_.draft }
		if ($Channel -eq 'stable') { $all = @($all) | Where-Object { -not $_.prerelease } }
		@($all) | Sort-Object `
			@{ Expression = { [version](($_.tag_name.TrimStart('v') -split '-', 2)[0]) } }, `
			@{ Expression = { $_.tag_name -notmatch '-' } }, `
			@{ Expression = { [regex]::Replace((($_.tag_name -split '-', 2) + '')[1], '\d+', [Text.RegularExpressions.MatchEvaluator]{ param($m) $m.Value.PadLeft(10, '0') }) } } |
			Select-Object -Last 1
	}

	function Test-ReleaseSignature([string]$file, [string]$sigFile) {
		try {
			$rsa = [System.Security.Cryptography.RSA]::Create()
			$rsaParams = New-Object System.Security.Cryptography.RSAParameters
			$rsaParams.Modulus  = [Convert]::FromBase64String($signingModulus)
			$rsaParams.Exponent = [Convert]::FromBase64String($signingExponent)
			$rsa.ImportParameters($rsaParams)
			return $rsa.VerifyData(
				[System.IO.File]::ReadAllBytes($file),
				[System.IO.File]::ReadAllBytes($sigFile),
				[System.Security.Cryptography.HashAlgorithmName]::SHA256,
				[System.Security.Cryptography.RSASignaturePadding]::Pkcs1)
		} catch {
			return $false
		}
	}

	## Windows only; elsewhere install.bash (Linux) or build from source.
	if (($PSVersionTable.PSVersion.Major -ge 6) -and -not $IsWindows) {
		Exit-Install 'this installer is for Windows - on Linux use install.bash, elsewhere build from source (see README.md)'
	}

	## A 32-bit shell on a 64-bit OS reports x86; ARCHITEW6432 carries the real arch.
	$archRaw = if ($env:PROCESSOR_ARCHITEW6432) { $env:PROCESSOR_ARCHITEW6432 } else { $env:PROCESSOR_ARCHITECTURE }
	$arch = switch ($archRaw) {
		'AMD64' { 'x86_64' }
		'ARM64' { 'arm64' }
		default { Exit-Install "no prebuilt binary for $archRaw" }
	}

	## Transport floor for every download: TLS 1.2+ (1.3 where the runtime knows it).
	try {
		[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12 -bor [Net.SecurityProtocolType]::Tls13
	} catch {
		[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
	}
	## Windows PowerShell repaints the progress bar per chunk, which slows a download
	## by an order of magnitude. Scoped to this script; nothing here needs the bar.
	$ProgressPreference = 'SilentlyContinue'

	## Resolve the tag. Highest version wins, never newest by date: GitHub's
	## /releases/latest is date-ordered, so a patch back-ported to an older line
	## after a newer one shipped would be handed out as "stable". A draft has no
	## published assets, so neither channel can install one; stable drops
	## pre-releases on top of that. Within one version a
	## final outranks its own pre-releases (v2.0.0-rc1 < v2.0.0); a pre-release
	## suffix compares with its digit runs zero-padded, so rc2 < rc10 - the same
	## order install.bash gets from sort -V.
	$api = "https://api.github.com/repos/$repo/releases?per_page=100"
	try { $rel = Invoke-RestMethod -Uri $api -UseBasicParsing } catch { Exit-Install "cannot fetch the $Release release (none published yet, or network down)" }
	$rel = Select-ReleaseTag $Release $rel
	if (-not $rel -or -not $rel.tag_name) { Exit-Install "no $Release release found" }
	$tag = $rel.tag_name
	$version = $tag.TrimStart('v')

	## Destinations. A system install writes under Program Files and the machine
	## PATH, so it needs an elevated shell.
	if ($Target -eq 'system') {
		$principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
		if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
			Exit-Install 'a system install needs an elevated shell (or pass -Target user)'
		}
		$dest = Join-Path $env:ProgramFiles 'Shcl'
		$pathScope = 'Machine'
		$pathDir = $dest
	} else {
		## Per-user apps belong under LOCALAPPDATA\Programs - it is where Windows
		## itself puts them, and it is already excluded from roaming profiles.
		$dest = Join-Path $env:LOCALAPPDATA 'Programs\Shcl'
		$pathScope = 'User'
		$pathDir = $dest
	}

	## Uninstall: the reverse of what the install lays down, and nothing else - the
	## binary, the two payload dirs, the install dir if it empties, and the PATH
	## entry. Never a recursive delete of a path the user may have pointed elsewhere.
	if ($Uninstall) {
		Write-Output ''
		Write-Output "removing shcl: $dest (and the $pathScope PATH entry)"
		if (-not $Yes) {
			$reply = Read-Host 'Proceed? [y/N]'
			if ($reply -notin @('y', 'Y', 'yes', 'Yes', 'YES')) {
				Write-Output 'aborted'
				if ($invokedAsFile) { exit 1 }
				return
			}
		}
		Remove-Item -Force -LiteralPath (Join-Path $dest 'shcl.exe') -ErrorAction SilentlyContinue
		Remove-Item -Recurse -Force -LiteralPath (Join-Path $dest 'code'), (Join-Path $dest 'scripts') -ErrorAction SilentlyContinue
		## Only an empty dir goes: the setup .exe installs here too, and a Remove-Item
		## on a populated dir would offer to take everything in it.
		if (Test-Path -LiteralPath $dest) {
			if (@(Get-ChildItem -Force -LiteralPath $dest).Count -eq 0) {
				Remove-Item -Force -LiteralPath $dest -ErrorAction SilentlyContinue
			} else {
				Write-Output "left $dest in place: it holds files this installer did not put there"
			}
		}
		$null = Update-ShclPath -Scope $pathScope -Dir $pathDir -Remove
		Write-Output 'removed'
		Write-Output ''
		return
	}

	## The drop-in payload is a tar.gz. Windows 10 1803 and Server 2019 ship tar;
	## anything older finds out here, before a download, not after.
	if (-not (Get-Command tar -ErrorAction SilentlyContinue)) {
		Exit-Install 'needs tar to unpack the drop-in payload (Windows 10 1803, Server 2019 and later ship it) - on an older Windows use the setup .exe from the releases page'
	}

	## State the plan, then confirm.
	Write-Output ''
	$existing = if (Test-Path -LiteralPath (Join-Path $dest 'shcl.exe')) { 'updates the existing install' } else { 'new install' }
	Write-Output "shcl $version ($Release, windows-$arch) -> $dest ($existing)"
	Write-Output "  binary   $dest\shcl.exe"
	Write-Output "  drop-ins $dest\code\, wrappers $dest\scripts\"
	Write-Output "  adds $pathDir to the $pathScope PATH if missing"
	if (-not $Yes) {
		$reply = Read-Host 'Proceed? [y/N]'
		if ($reply -notin @('y', 'Y', 'yes', 'Yes', 'YES')) {
			Write-Output 'aborted'
			if ($invokedAsFile) { exit 1 }
			return
		}
	}

	$tmp = Join-Path ([IO.Path]::GetTempPath()) ("shcl-install-" + [IO.Path]::GetRandomFileName())
	New-Item -ItemType Directory -Path $tmp | Out-Null
	try {
		## Download and verify the binary.
		Write-Output ''
		$asset = "shcl-$version-windows-$arch.exe"
		$base = "https://github.com/$repo/releases/download/$tag"
		Write-Output "downloading $asset..."
		Invoke-WebRequest -Uri "$base/$asset" -OutFile (Join-Path $tmp 'shcl.exe') -UseBasicParsing
		Invoke-WebRequest -Uri "$base/shcl-$version-sha256sums.txt" -OutFile (Join-Path $tmp 'sums.txt') -UseBasicParsing
		Invoke-WebRequest -Uri "$base/shcl-$version-sha256sums.txt.sig" -OutFile (Join-Path $tmp 'sums.txt.sig') -UseBasicParsing

		## Check the signature before trusting anything the sums file says. Order is
		## the whole point: a checksum read out of an unverified file proves nothing.
		if (-not (Test-ReleaseSignature (Join-Path $tmp 'sums.txt') (Join-Path $tmp 'sums.txt.sig'))) {
			Exit-Install 'signature check failed on sha256sums - refusing to install'
		}

		$want = (Get-Content -LiteralPath (Join-Path $tmp 'sums.txt') | Where-Object { $_ -match [regex]::Escape($asset) } | ForEach-Object { ($_ -split '\s+')[0] } | Select-Object -First 1)
		$got = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $tmp 'shcl.exe')).Hash.ToLower()
		if (-not $want -or $got -ne $want.ToLower()) { Exit-Install "sha256 mismatch on $asset" }

		## Drop-in code files and wrappers come from a release asset covered by the
		## same signed sums file as the binary. They used to come from GitHub's
		## generated source zipball, which carries neither a signature nor a
		## checksum. Releases predating the asset install the binary alone.
		$dropins = "shcl-$version-dropins.tar.gz"
		$wantSrc = (Get-Content -LiteralPath (Join-Path $tmp 'sums.txt') | Where-Object { $_ -match [regex]::Escape($dropins) } | ForEach-Object { ($_ -split '\s+')[0] } | Select-Object -First 1)
		$haveDropins = $false
		$srcroot = $null
		if ($wantSrc) {
			Write-Output "downloading $dropins..."
			Invoke-WebRequest -Uri "$base/$dropins" -OutFile (Join-Path $tmp 'dropins.tgz') -UseBasicParsing
			$gotSrc = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $tmp 'dropins.tgz')).Hash.ToLower()
			if ($gotSrc -ne $wantSrc.ToLower()) { Exit-Install "sha256 mismatch on $dropins" }
			$unpackDir = Join-Path $tmp 'x'
			New-Item -ItemType Directory -Path $unpackDir | Out-Null
			tar -xzf (Join-Path $tmp 'dropins.tgz') -C $unpackDir
			if ($LASTEXITCODE -ne 0) { Exit-Install "cannot unpack $dropins" }
			$srcroot = Get-Item -LiteralPath $unpackDir
			$haveDropins = $true
		}

		## Run it from the temp dir before anything is written, the way the Linux
		## installer does: a binary that will not start here should never become
		## an install. Redirected and status-tested rather than called bare,
		## since a nonzero exit from a native command throws under this script's
		## own error preference on 7.4 and later - which used to mean a success
		## message followed by an exception, with the install left in place.
		$smokeOut = & (Join-Path $tmp 'shcl.exe') version 2>&1
		if ($LASTEXITCODE -ne 0) {
			Exit-Install "the downloaded shcl.exe does not run here: $($smokeOut -join ' ')"
		}

		## Install. The binary goes in via a temp name + Move-Item in the same dir,
		## so a running copy only ever sees the complete old or new file.
		New-Item -ItemType Directory -Force -Path $dest | Out-Null
		Copy-Item -LiteralPath (Join-Path $tmp 'shcl.exe') -Destination (Join-Path $dest '.shcl.exe.new')
		Move-Item -Force -LiteralPath (Join-Path $dest '.shcl.exe.new') -Destination (Join-Path $dest 'shcl.exe')
		if ($haveDropins) {
			New-Item -ItemType Directory -Force -Path (Join-Path $dest 'code'), (Join-Path $dest 'scripts') | Out-Null
			$payloadRoot = $srcroot.FullName
			Copy-Item -LiteralPath "$payloadRoot\source\rust\src\lib.rs", "$payloadRoot\source\go\shcl.go", "$payloadRoot\source\python\shcl.py", "$payloadRoot\source\c\shcl.h", "$payloadRoot\source\c\shcl.hpp" -Destination (Join-Path $dest 'code')
			Copy-Item -LiteralPath "$payloadRoot\source\powershell\shcl.ps1", "$payloadRoot\source\bash\shcl.bash" -Destination (Join-Path $dest 'scripts')
		}

		if (Update-ShclPath -Scope $pathScope -Dir $pathDir) {
			Write-Output "added $pathDir to the $pathScope PATH (new shells pick it up)"
			## Nudge running shells; best-effort.
			try {
				$sig = '[DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Auto)] public static extern IntPtr SendMessageTimeout(IntPtr hWnd, uint Msg, UIntPtr wParam, string lParam, uint fuFlags, uint uTimeout, out UIntPtr lpdwResult);'
				$w32 = Add-Type -MemberDefinition $sig -Name 'ShclEnvBroadcast' -Namespace 'Win32' -PassThru
				$out = [UIntPtr]::Zero
				$null = $w32::SendMessageTimeout([IntPtr]0xffff, 0x1A, [UIntPtr]::Zero, 'Environment', 2, 5000, [ref]$out)
			} catch {
				Write-Output 'PATH change broadcast skipped (new shells still pick it up)'
			}
		}

		Write-Output ''
		Write-Output "installed shcl $version -> $dest\shcl.exe"
		if (-not $haveDropins) { Write-Output "note: this release ships no signed drop-in payload, so $dest\code and $dest\scripts were skipped - take them from the repo if you want them" }
		$rerun = if ($invokedAsFile) { "& '$scriptPath'" } else { '& ([scriptblock]::Create((irm https://raw.githubusercontent.com/jim-collier/shcl/main/install.ps1)))' }
		Write-Output "to remove it again: $rerun -Uninstall -Target $Target"
		## Already proved above, from the temp dir; this line is the receipt.
		Write-Output $smokeOut
		Write-Output ''
	} finally {
		Remove-Item -Recurse -Force -LiteralPath $tmp -ErrorAction SilentlyContinue
	}
} $Release $Target $Yes $Uninstall $Help ($MyInvocation.MyCommand -is [Management.Automation.ExternalScriptInfo]) $PSCommandPath
