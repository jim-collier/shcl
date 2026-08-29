<#
	PSScriptAnalyzer settings for the PowerShell wrapper, the Windows installer,
	and the build runner.

	Pinned here rather than passed on the command line so running the analyzer by
	hand gives the same answer the pipeline gets, and so a new rule in a later
	release cannot quietly change what passes.
#>
@{
	Severity = @('Error', 'Warning')

	Rules = @{
		PSUseCompatibleSyntax = @{
			Enable         = $true
			## The installer has to run on the Windows PowerShell that ships with
			## the OS, not just on 7.
			TargetVersions = @('5.1', '7.0')
		}
	}
}
