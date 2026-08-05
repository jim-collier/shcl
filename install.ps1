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
##		-Yes                    skip the confirmation prompt.
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
function Test-ReleaseSignature([string]$file, [string]$sigFile) {
	try {
		$rsa = [System.Security.Cryptography.RSA]::Create()
		$p = New-Object System.Security.Cryptography.RSAParameters
		$p.Modulus  = [Convert]::FromBase64String($signingModulus)
		$p.Exponent = [Convert]::FromBase64String($signingExponent)
		$rsa.ImportParameters($p)
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
	Fail 'this installer is for Windows - on Linux use install.bash, elsewhere build from source (see README.md)'
}

$arch = switch ($env:PROCESSOR_ARCHITECTURE) {
	'AMD64' { 'x86_64' }
	'ARM64' { 'arm64' }
	default { Fail "no prebuilt binary for $($env:PROCESSOR_ARCHITECTURE)" }
}

## Transport floor for every download: TLS 1.2+ (1.3 where the runtime knows it).
try {
	[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12 -bor [Net.SecurityProtocolType]::Tls13
} catch {
	[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
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
	## Per-user apps belong under LOCALAPPDATA\Programs - it is where Windows
	## itself puts them, and it is already excluded from roaming profiles.
	$dest = Join-Path $env:LOCALAPPDATA 'Programs\Shcl'
	$pathScope = 'User'
	$pathDir = $dest
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
	Invoke-WebRequest -Uri "$base/shcl-$version-sha256sums.txt.sig" -OutFile (Join-Path $tmp 'sums.txt.sig')

	## Check the signature before trusting anything the sums file says. Order is
	## the whole point: a checksum read out of an unverified file proves nothing.
	if (-not (Test-ReleaseSignature (Join-Path $tmp 'sums.txt') (Join-Path $tmp 'sums.txt.sig'))) {
		Fail 'signature check failed on sha256sums - refusing to install'
	}

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

	## PATH, idempotently - straight at the registry. [Environment]::Get expands
	## %VAR% references before returning and Set writes the result back REG_SZ,
	## which freezes every reference and downgrades the value type (user PATHs
	## commonly carry %USERPROFILE%). Reading unexpanded and writing
	## REG_EXPAND_SZ keeps the stored value intact.
	$hive = if ($pathScope -eq 'Machine') { [Microsoft.Win32.Registry]::LocalMachine } else { [Microsoft.Win32.Registry]::CurrentUser }
	$subkey = if ($pathScope -eq 'Machine') { 'SYSTEM\CurrentControlSet\Control\Session Manager\Environment' } else { 'Environment' }
	$envKey = $hive.OpenSubKey($subkey, $true)
	$current = [string]$envKey.GetValue('Path', '', [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
	if (($current -split ';') -notcontains $pathDir) {
		$joined = if ($current -and -not $current.EndsWith(';')) { "$current;$pathDir" } else { "$current$pathDir" }
		$envKey.SetValue('Path', $joined, [Microsoft.Win32.RegistryValueKind]::ExpandString)
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
	$envKey.Close()

	Write-Output "installed shcl $version -> $dest\shcl.exe"
	& (Join-Path $dest 'shcl.exe') version
} finally {
	Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}
