#!/usr/bin/env pwsh
## winpath-sandbox.ps1 - run winpath-regress.ps1 inside Windows Sandbox, so a
## developer box gets the registry gate the hosted runner gets without its own
## PATH being rewritten. The sandbox is a throwaway machine with a throwaway
## registry and an administrator user, which is what that test needs and what a
## workstation cannot offer. The repo is mapped read-only and a scratch dir
## read-write, which is the only way anything comes back out.
##
## Exit: 0 = the inner run passed, 1 = it failed, 2 = cannot set up.

param([int]$TimeoutSeconds = 420)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Exit-Setup([string]$Why) { Write-Output "winpath-sandbox: $Why"; exit 2 }

if (-not ($env:OS -eq 'Windows_NT')) { Exit-Setup 'not windows; nothing to run here' }

$sandboxExe = Join-Path $env:SystemRoot 'System32\WindowsSandbox.exe'
if (-not (Test-Path $sandboxExe)) { Exit-Setup 'Windows Sandbox is not installed on this host' }

## Only one sandbox runs at a time, so one already up is somebody else's and
## closing it at the end would not be ours to do.
if (Get-Process -Name 'WindowsSandboxRemoteSession' -ErrorAction SilentlyContinue) {
	Exit-Setup 'a sandbox is already running; not touching it'
}

$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$work = Join-Path ([IO.Path]::GetTempPath()) ('shcl-wsb-' + [Guid]::NewGuid().ToString('N').Substring(0, 8))
$out = Join-Path $work 'out'
New-Item -ItemType Directory -Path $out -Force | Out-Null

## The batch file is what the sandbox runs at logon. It writes the marker last,
## so seeing the marker means the result file is already complete. The echo
## lines put the redirect FIRST: `echo EXIT=%ERRORLEVEL%>>f` ends in `0>>f`,
## which cmd reads as a redirect of handle 0 and the exit code never lands.
$runCmd = @'
@echo off
powershell -NoProfile -ExecutionPolicy Bypass -File C:\repo\cicd\utility\winpath-regress.ps1 > C:\out\result.txt 2>&1
>>C:\out\result.txt echo EXIT=%ERRORLEVEL%
>C:\out\done.txt echo done
'@
Set-Content -Path (Join-Path $out 'run.cmd') -Value $runCmd -Encoding ASCII

$wsb = @"
<Configuration>
	<MappedFolders>
		<MappedFolder>
			<HostFolder>$root</HostFolder>
			<SandboxFolder>C:\repo</SandboxFolder>
			<ReadOnly>true</ReadOnly>
		</MappedFolder>
		<MappedFolder>
			<HostFolder>$out</HostFolder>
			<SandboxFolder>C:\out</SandboxFolder>
			<ReadOnly>false</ReadOnly>
		</MappedFolder>
	</MappedFolders>
	<LogonCommand>
		<Command>cmd.exe /c C:\out\run.cmd</Command>
	</LogonCommand>
	<Networking>Disable</Networking>
</Configuration>
"@
$wsbPath = Join-Path $work 'winpath.wsb'
Set-Content -Path $wsbPath -Value $wsb -Encoding UTF8

$marker = Join-Path $out 'done.txt'
$result = Join-Path $out 'result.txt'
$failed = 0
try {
	Write-Output "winpath-sandbox: starting a sandbox on $root"
	Start-Process -FilePath $sandboxExe -ArgumentList $wsbPath | Out-Null
	$deadline = (Get-Date).AddSeconds($TimeoutSeconds)
	while (-not (Test-Path $marker) -and (Get-Date) -lt $deadline) { Start-Sleep -Seconds 2 }
	if (-not (Test-Path $marker)) {
		Write-Output "winpath-sandbox: the run did not finish within $TimeoutSeconds seconds"
		if (Test-Path $result) { Get-Content $result | ForEach-Object { Write-Output $_ } }
		$failed = 1
	}
	else {
		$lines = @(Get-Content $result)
		$lines | ForEach-Object { Write-Output $_ }
		$tail = $lines | Where-Object { $_ -match '^EXIT=' } | Select-Object -Last 1
		if ($null -eq $tail) { Write-Output 'winpath-sandbox: no exit code came back'; $failed = 1 }
		elseif ($tail.Trim() -ne 'EXIT=0') { $failed = 1 }
	}
}
finally {
	## Nothing was running when this started, so any session now is the one
	## started above. Closing the client tears down the whole machine.
	Get-Process -Name 'WindowsSandboxRemoteSession' -ErrorAction SilentlyContinue |
		Stop-Process -Force -ErrorAction SilentlyContinue
	Start-Sleep -Seconds 3
	Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}

if ($failed -eq 0) { Write-Output 'winpath-sandbox: OK'; exit 0 }
Write-Output 'winpath-sandbox: FAILED'
exit 1
