#requires -Version 7.0
<#
.SYNOPSIS
	Builds the plugin and packages the archive that gets uploaded to Nexus Mods by hand.

.DESCRIPTION
	One script for a whole release: it configures and builds the vs2022 preset with the
	requested version stamped into the binary, stages the mod the way a mod manager expects to
	install it, and writes packages/BlinkDash-<Version>.zip. Uploading that zip is a manual step
	on the mod page - nothing here talks to Nexus Mods.

	The archive is laid out as:

		SKSE/Plugins/BlinkDash.dll
		SKSE/Plugins/BlinkDash.json              (the shipped defaults, written on first run)
		SKSE/Plugins/BlinkDash.ShaderNames.json  (readable labels for the effect dropdown)
		docs/README.md, docs/LICENSE             (documentation; "docs" is ignored by the managers)

	The build needs the MSVC environment a "developer shell" provides, but starting this from a
	normal shell is fine: vcvars64.bat is located with vswhere and imported into this process,
	so CMake, Ninja and the compiler are found either way. The toolset still has to share its
	major.minor with the prebuilt CommonLibSSE bundle, or the build silently falls back to the
	~16 minute source build (see the note in CMakeLists.txt) - which is why the script does not
	pick a toolset of its own.

.PARAMETER Version
	Version for this release, e.g. 1.0.1. It is stamped into the DLL (BLINKDASH_VERSION, so the
	log line and the SKSE plugin record carry it) and it names the archive.

.PARAMETER SkipBuild
	Package what is already in the build tree. For re-packaging after a failed upload rather
	than for skipping the stamp: the DLL keeps whatever version it was last built with.

.PARAMETER FromFolder
	Package an already deployed mod folder instead: its SKSE/Plugins contents are used as-is.
	That is what MOD_OUTPUT_FOLDER deploys, so it is the way to archive exactly what the game
	is running.

.PARAMETER Output
	Archive to write. Defaults to packages/BlinkDash-<Version>.zip next to the repo root.

.EXAMPLE
	# build 1.0.1, stamp it into the DLL and package it
	.\tools\Build-Release.ps1 -Version 1.0.1

.EXAMPLE
	# re-zip the build that is already there, without building
	.\tools\Build-Release.ps1 -Version 1.0.1 -SkipBuild
#>
[CmdletBinding()]
param(
	[Parameter(Mandatory)][string]$Version,
	[switch]$SkipBuild,
	[string]$FromFolder,
	[string]$Output
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

# Three components is what the plugin reports; CMake accepts up to four, so allow both.
if ($Version -notmatch '^\d+(\.\d+){1,3}$') {
	throw "-Version must look like 1.0.1, got '$Version'."
}
if (-not $Output) { $Output = Join-Path $root "packages\BlinkDash-$Version.zip" }

# ---------------------------------------------------------------------------
# Toolchain (MSVC + CMake + Ninja), so this runs from any shell
# ---------------------------------------------------------------------------
function Get-VsInstallPath
{
	$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
	if (-not (Test-Path $vswhere)) { return $null }
	$found = & $vswhere -latest -products * `
		-requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
		-property installationPath
	if ($LASTEXITCODE -ne 0) { return $null }
	return ($found | Select-Object -First 1)
}

function Initialize-MsvcEnvironment
{
	$vsPath = Get-VsInstallPath
	if (-not $vsPath) {
		# No VS found: assume the caller is already in a developer shell and let it fail loudly
		# at configure time if that is wrong.
		if (Get-Command cl.exe -ErrorAction SilentlyContinue) { return }
		throw 'Visual Studio with the C++ workload not found (vswhere reported nothing).'
	}

	# The IDE's own cmake and ninja are the copies the preset was written against; they are not
	# on PATH outside a developer prompt.
	$env:PATH = @(
		(Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin'),
		(Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja'),
		$env:PATH
	) -join ';'

	# What "developer shell" actually means: import vcvars64's environment into this process.
	# Re-importing it in a shell that already has it is harmless.
	$vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'
	if (-not (Test-Path $vcvars)) { throw "No vcvars64.bat at $vcvars." }
	cmd /c "`"$vcvars`" >nul 2>&1 && set" | ForEach-Object {
		# "=C:=C:\..." lines start with '=' and must not be turned into an environment variable.
		if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($matches[1])" -Value $matches[2] }
	}
}

function Get-ConfigurePreset
{
	# vs2022 holds the shared settings. A git-ignored CMakeUserPresets.json may add a preset that
	# inherits it to set MOD_OUTPUT_FOLDER, and that is the one to configure with: same build
	# tree, but the release also deploys to the mod folder the game runs from.
	$preset = 'vs2022'
	$userPresets = Join-Path $root 'CMakeUserPresets.json'
	if (Test-Path $userPresets) {
		$inheriting = (Get-Content $userPresets -Raw | ConvertFrom-Json).configurePresets |
			Where-Object { $_.inherits -eq 'vs2022' } |
			Select-Object -First 1
		if ($inheriting) { $preset = $inheriting.name }
	}
	return $preset
}

function Get-BinaryDir
{
	# Read the build directory out of the preset instead of hardcoding it, so changing the preset
	# cannot leave this script building (or packaging from) a stale folder.
	$configure = (Get-Content (Join-Path $root 'CMakePresets.json') -Raw | ConvertFrom-Json).
		configurePresets | Where-Object { $_.name -eq 'vs2022' } | Select-Object -First 1
	if (-not $configure.binaryDir) { throw 'The vs2022 preset has no binaryDir.' }
	return $configure.binaryDir.Replace('${sourceDir}', $root).Replace('/', '\')
}

function Invoke-Native
{
	param([string]$Command, [string[]]$Arguments)
	& $Command @Arguments
	if ($LASTEXITCODE -ne 0) { throw "$Command $($Arguments -join ' ') failed with exit code $LASTEXITCODE." }
}

# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------
$binaryDir = Get-BinaryDir
$dll = Join-Path $binaryDir 'BlinkDash.dll'

if (-not $SkipBuild) {
	Initialize-MsvcEnvironment

	# The preset interpolates $env:VCPKG_ROOT, so it must be set before configure runs. A checkout
	# next to the repo is what CMakeLists documents; the environment wins if it is already set.
	if (-not $env:VCPKG_ROOT) {
		$local = Join-Path $root '.vcpkg'
		if (Test-Path (Join-Path $local 'scripts\buildsystems\vcpkg.cmake')) { $env:VCPKG_ROOT = $local }
		else { throw 'vcpkg not found: set VCPKG_ROOT or clone one into .vcpkg.' }
	}

	$preset = Get-ConfigurePreset
	Write-Host "Configuring $preset with version $Version"
	Invoke-Native cmake @('--preset', $preset, "-DBLINKDASH_VERSION=$Version")

	Write-Host 'Building'
	Invoke-Native cmake @('--build', $binaryDir)
}

# ---------------------------------------------------------------------------
# Stage and zip
# ---------------------------------------------------------------------------
# Stage in a scratch folder and zip that: mixing "files at the root" and "files in a subfolder"
# is impossible in one ZipFile call, and the archive must have SKSE/Plugins at its root.
$stage = Join-Path $root 'packages\.staging'
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
$plugins = Join-Path $stage 'SKSE\Plugins'
New-Item -ItemType Directory -Path $plugins -Force | Out-Null

if ($FromFolder) {
	$source = (Resolve-Path (Join-Path $FromFolder 'SKSE\Plugins')).Path
	Copy-Item (Join-Path $source '*.dll') $plugins
	# Only the shader name table ships. BlinkDash.json is the user's own config: the plugin writes
	# a default one on first run, so shipping a copy would replace a tuned file on every update.
	Copy-Item (Join-Path $source 'BlinkDash.ShaderNames.json') $plugins
	Write-Host "Staged from $source"
}
else {
	if (-not (Test-Path $dll)) { throw "No DLL at $dll - build it (drop -SkipBuild), or pass -FromFolder." }
	Copy-Item $dll $plugins
	# Deliberately NOT dist\BlinkDash.json: that file is generated by the plugin when none exists,
	# and putting it in the archive would reset every existing install's settings on update.
	Copy-Item (Join-Path $root 'dist\BlinkDash.ShaderNames.json') $plugins
	Write-Host "Staged from $dll"
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
Write-Host 'Upload it by hand on the mod page (Files tab -> Upload a new file).'
Write-Output $zip.FullName
