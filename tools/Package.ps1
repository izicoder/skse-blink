#requires -Version 7.0
<#
.SYNOPSIS
	Stages the mod into the archive that gets uploaded to Nexus Mods.

.DESCRIPTION
	The archive is laid out the way a mod manager expects to install it, so the FOMOD-less
	zip is dropped straight into MO2/Vortex:

		SKSE/Plugins/BlinkDash.dll
		SKSE/Plugins/BlinkDash.json              (the shipped defaults, written on first run)
		SKSE/Plugins/BlinkDash.ShaderNames.json  (readable labels for the effect dropdown)
		docs/README.md, docs/LICENSE             (documentation; "docs" is ignored by the managers)

	The DLL is taken from the build output by default; pass -FromFolder to package an already
	deployed mod folder (e.g. the MO2 one MOD_OUTPUT_FOLDER points at) instead.

.PARAMETER Version
	Version string for the archive name and, from Publish-Nexus.ps1, the Nexus file version.

.PARAMETER Dll
	BlinkDash.dll to package. Defaults to the vs2022 build output.

.PARAMETER FromFolder
	Package a mod folder instead: its SKSE/Plugins contents are used as-is.

.PARAMETER Output
	Archive to write. Defaults to packages/BlinkDash-<Version>.zip next to the repo root.
#>
[CmdletBinding()]
param(
	[Parameter(Mandatory)][string]$Version,
	[string]$Dll = (Join-Path $PSScriptRoot '..\build\vs2022\BlinkDash.dll'),
	[string]$FromFolder,
	[string]$Output = (Join-Path $PSScriptRoot "..\packages\BlinkDash-$Version.zip")
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

# Stage in a scratch folder and zip that: mixing "files at the root" and "files in a subfolder"
# is impossible in one ZipFile call, and the archive must have SKSE/Plugins at its root.
$stage = Join-Path $root 'packages\.staging'
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
$plugins = Join-Path $stage 'SKSE\Plugins'
New-Item -ItemType Directory -Path $plugins -Force | Out-Null

if ($FromFolder) {
	$source = (Resolve-Path (Join-Path $FromFolder 'SKSE\Plugins')).Path
	Copy-Item (Join-Path $source '*.dll') $plugins
	Copy-Item (Join-Path $source '*.json') $plugins
	Write-Host "Staged from $source"
}
else {
	if (-not (Test-Path $Dll)) { throw "No DLL at $Dll - build first, or pass -FromFolder." }
	Copy-Item $Dll $plugins
	# The defaults are shipped, not a saved config: the plugin only writes BlinkDash.json when
	# it does not exist, so the user's own file is never clobbered by an update.
	Copy-Item (Join-Path $root 'dist\BlinkDash.json') $plugins
	Copy-Item (Join-Path $root 'dist\BlinkDash.ShaderNames.json') $plugins
	Write-Host "Staged from $Dll"
}

# README and LICENSE go in a docs/ folder: MO2 ignores a root docs folder, so the archive's
# data root stays exactly SKSE/Plugins.
$docs = Join-Path $stage 'docs'
New-Item -ItemType Directory -Path $docs -Force | Out-Null
Copy-Item (Join-Path $root 'README.md') $docs
Copy-Item (Join-Path $root 'LICENSE') $docs

New-Item -ItemType Directory -Path (Split-Path $Output) -Force | Out-Null
if (Test-Path $Output) { Remove-Item $Output -Force }

# ZipFile (not Compress-Archive) so the entries use forward slashes, which is what the mod
# managers and the Nexus scanners expect to see.
Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::CreateFromDirectory(
	$stage,
	(Join-Path (Resolve-Path (Split-Path $Output)).Path (Split-Path $Output -Leaf)),
	[System.IO.Compression.CompressionLevel]::Optimal,
	$false
)

Remove-Item $stage -Recurse -Force

$zip = Get-Item $Output
Write-Host ("Packaged {0} ({1:N0} bytes)" -f $zip.FullName, $zip.Length)
Write-Output $zip.FullName
