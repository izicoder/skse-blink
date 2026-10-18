#requires -Version 7.0
<#
.SYNOPSIS
	Uploads the packaged archive to Nexus Mods using the official Upload API (v3).

.DESCRIPTION
	One command for a whole release: create an upload session, PUT the archive to the presigned
	URL, finalise it, wait for it to become usable, then create the mod file (or a new version of
	an existing one) and optionally append a changelog. Nothing touches the website UI.

	Only the API key is required to authenticate - the header is `apikey`, the same key the
	official Nexus-Mods/upload-action GitHub Action uses.

	The mod page itself must already exist: the Upload API can only add files to a mod, it cannot
	create one. So the very first file is uploaded by hand on the site; every update after that
	is `.\tools\Publish-Nexus.ps1 -Version 1.0.1`.

.PARAMETER Version
	Version string for this upload, shown on the Files tab (e.g. 1.0.1).

.PARAMETER Archive
	Zip to upload. Defaults to packages/BlinkDash-<Version>.zip, i.e. what Package.ps1 writes.

.PARAMETER ModId
	The mod's game-scoped id - the number in the mod page URL,
	https://www.nexusmods.com/skyrimspecialedition/mods/<ModId>. Needed for the first file of a
	mod and for changelog entries.

.PARAMETER FileId
	The v3 mod file id of the file this release is a new version of (the update chain). Shown
	under the Advanced option on the mod's Files tab. Pass it for every release after the first;
	without it the upload is added as a brand new file on the mod page.

.PARAMETER Changelog
	Changelog text appended to this version (needs -ModId).

.PARAMETER UpdateModVersion
	Also bump the mod's main version to -Version.

.PARAMETER ArchiveExisting
	Archive the previous version when adding this one.

.PARAMETER NoModManagerDownload
	Hide the "Download with manager" buttons for this file.

.EXAMPLE
	# first release, new file on an existing mod page
	.\tools\Package.ps1 -Version 1.0.0
	.\tools\Publish-Nexus.ps1 -Version 1.0.0 -ModId 123456 -UpdateModVersion

.EXAMPLE
	# every later release, new version of the existing file
	.\tools\Package.ps1 -Version 1.0.1
	.\tools\Publish-Nexus.ps1 -Version 1.0.1 -ModId 123456 -FileId 987654 -UpdateModVersion `
		-Changelog "Fixed the landing offset on slopes."
#>
[CmdletBinding()]
param(
	[Parameter(Mandatory)][string]$Version,
	[string]$Archive,
	[string]$ModId,
	[string]$FileId,
	[string]$ApiKey = $env:NEXUSMODS_API_KEY,
	[string]$GameDomain = 'skyrimspecialedition',
	[string]$Name = 'BlinkDash',
	[ValidateSet('main', 'optional', 'miscellaneous')][string]$Category = 'main',
	[string]$Description = 'Unpack into the Skyrim data folder, or install with your mod manager.',
	[string]$Changelog,
	[switch]$UpdateModVersion,
	[switch]$ArchiveExisting,
	[switch]$NoModManagerDownload
)

$ErrorActionPreference = 'Stop'
$ApiBase = 'https://api.nexusmods.com/v3'

if (-not $Archive) { $Archive = Join-Path $PSScriptRoot "..\packages\BlinkDash-$Version.zip" }
if (-not $ApiKey) { throw 'No API key: set NEXUSMODS_API_KEY or pass -ApiKey (https://www.nexusmods.com/settings/api-keys).' }
if (-not (Test-Path $Archive)) { throw "No archive at $Archive - run tools\Package.ps1 first." }
if (-not $ModId -and -not $FileId) { throw 'Pass -FileId (update an existing file) or -ModId (add a new file to the mod).' }

$headers = @{ apikey = $ApiKey; 'User-Agent' = 'BlinkDash-release-script' }

# NOTE: $Archive and $archive would be the same variable (PowerShell is case-insensitive), and
# the parameter's [string] constraint would coerce the FileInfo straight back to a path - so the
# LocalName below must not be just a case variant of the parameter.
$zip = Get-Item -LiteralPath $Archive
$leaf = $zip.Name
$size = $zip.Length

# The API wants the md5 both ways: hex in the session request (where it is signed into the
# presigned URL) and base64 in the Content-MD5 header of the PUT. Required from 2026-12-01, so
# sending it now costs nothing and avoids the future breaking change.
$md5 = [System.Security.Cryptography.MD5]::Create().ComputeHash([System.IO.File]::ReadAllBytes($zip.FullName))
$md5Hex = [System.BitConverter]::ToString($md5).Replace('-', '').ToLowerInvariant()
$md5Base64 = [System.Convert]::ToBase64String($md5)

function Invoke-NexusApi {
	param([string]$Method, [string]$Path, $Body)
	$call = @{ Method = $Method; Uri = "$ApiBase$Path"; Headers = $headers; ErrorAction = 'Stop' }
	if ($null -ne $Body) {
		$call.Body = ($Body | ConvertTo-Json -Depth 8)
		$call.ContentType = 'application/json'
	}
	try {
		Invoke-RestMethod @call
	}
	catch {
		# Surface the RFC 9457 problem body: "detail" is where the API explains the refusal.
		$detail = $_.ErrorDetails.Message
		if (-not $detail -and $_.Exception.Response) {
			$detail = [System.IO.StreamReader]::new($_.Exception.Response.GetResponseStream()).ReadToEnd()
		}
		throw "$Method $Path failed: $($_.Exception.Message)$(if ($detail) { "`n$detail" })"
	}
}

# Every v3 response wraps its payload in { data: ... } (except the ones with no body);
# unwrap it so callers read the payload directly.
function Get-Data {
	param($Response)
	if ($null -ne $Response.data) { $Response.data } else { $Response }
}

Write-Host "Uploading $leaf ($('{0:N0}' -f $size) bytes) as version $Version"

# 1. Open an upload session. The returned presigned URL is bound to this filename (and md5), so
#    it cannot be reused for different content.
$session = Get-Data (Invoke-NexusApi POST '/uploads' @{ size_bytes = $size; filename = $leaf; md5 = $md5Hex })
if ($size -gt 100MB) { throw 'Over 100 MiB: that needs the multipart upload endpoints.' }
Write-Host "  upload session $($session.id)"

# 2. PUT the data. Content-Disposition and Content-MD5 are part of the URL signature, so the
#    upload is rejected unless they match the session request exactly.
$client = [System.Net.Http.HttpClient]::new()
try {
	$content = [System.Net.Http.StreamContent]::new([System.IO.File]::OpenRead($zip.FullName))
	$content.Headers.ContentType = [System.Net.Http.Headers.MediaTypeHeaderValue]::Parse('application/octet-stream')
	$content.Headers.ContentDisposition = [System.Net.Http.Headers.ContentDispositionHeaderValue]::Parse("attachment; filename=`"$leaf`"")
	$content.Headers.Add('Content-MD5', $md5Base64)
	$response = $client.PutAsync($session.presigned_url, $content).GetAwaiter().GetResult()
	if (-not $response.IsSuccessStatusCode) {
		throw "PUT failed: $([int]$response.StatusCode) $($response.Content.ReadAsStringAsync().GetAwaiter().GetResult())"
	}
	Write-Host '  data uploaded'
}
finally {
	$client.Dispose()
}

# 3. Close the session, then wait for the backend to finish (it hash-checks and scans the file).
Invoke-NexusApi POST "/uploads/$($session.id)/finalise" $null | Out-Null
for ($attempt = 0; $attempt -lt 60; $attempt++) {
	Start-Sleep -Seconds ([Math]::Min(2 * [Math]::Pow(1.5, $attempt), 30))
	$state = (Get-Data (Invoke-NexusApi GET "/uploads/$($session.id)" $null)).state
	Write-Host "  state: $state"
	if ($state -eq 'available') { break }
	if ($state -ne 'created') { throw "Unexpected upload state '$state'." }
	if ($attempt -eq 59) { throw "Upload $($session.id) never became available." }
}

# 4. Turn the upload into a mod file. The name/version patterns are strict: letters, digits,
#    spaces and _'().- for the name, letters/digits/.- for the version.
$file = @{
	upload_id                        = $session.id
	name                             = ($Name -replace '[^a-zA-Z0-9 _''().-]', '')
	version                          = $Version
	file_category                    = $Category
	description                      = $Description
	update_mod_version               = [bool]$UpdateModVersion
	allow_mod_manager_download       = -not $NoModManagerDownload
	primary_mod_manager_download     = $false
	show_requirements_pop_up         = $false
}

if ($FileId) {
	$file.archive_existing_file = [bool]$ArchiveExisting
	$result = Get-Data (Invoke-NexusApi POST "/mod-files/$FileId/versions" $file)
	Write-Host "  new version $($result.version.id) of mod file $FileId"
}
else {
	$file.mod_id = $ModId
	$result = Get-Data (Invoke-NexusApi POST '/mod-files' $file)
	Write-Host "  new mod file $($result.file.id) on mod $ModId (keep this id for later releases)"
}

# 5. Changelog, if asked for. /mods/{id} wants the mod's internal id, which is not the URL number
#    for every game, so resolve it and fall back to what was passed.
if ($Changelog) {
	$modId = $ModId
	if (-not $modId) { throw '-Changelog also needs -ModId.' }
	try {
		$modId = (Get-Data (Invoke-NexusApi GET "/games/$GameDomain/mods/$ModId" $null)).id
	}
	catch {
		Write-Warning "Could not resolve the internal mod id, using ${ModId}: $_"
	}
	Invoke-NexusApi POST "/mods/$modId/changelogs" @{ version = $Version; changelog = $Changelog } | Out-Null
	Write-Host '  changelog added'
}

Write-Host "Done: $Name $Version is on Nexus Mods."
