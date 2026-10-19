#requires -Version 7.0
<#
.SYNOPSIS
	Publishes a packaged BlinkDash release to Nexus Mods and/or GitHub, only when you ask for it.

.DESCRIPTION
	tools\Build-Release.ps1 builds the plugin and writes packages\BlinkDash-<Version>.zip and never
	touches the network. This is the other half, and it is deliberately a separate script invoked by
	hand: a build, a deploy or a re-package can never publish anything, because publishing happens
	only when -Nexus or -GitHub is named on the command line.

	Nexus goes through the public v3 Upload API - the same multipart-upload + finalise + create-version
	flow as Nexus-Mods/upload-action. The API cannot create a file entry, only add versions to one
	that already exists, so -FileId must be a real entry on the mod page (the default is BlinkDash's
	MAIN file). After the version is created the mod's displayed version is synced to it and the notes
	are posted as a changelog entry.

	GitHub goes through the gh CLI (authenticated on this machine) and attaches the same archive. The
	tag is v<Version> and the release body is the notes.

	In both cases the notes default to the commit subjects since the last release tag, so the changelog
	mirrors the git log without a second file to maintain; -Notes replaces them.

	Two guards exist because a wrong publish cannot be undone from here: the DLL inside the archive must
	report the -Version being published (a stale build tree or a -SkipBuild package is the usual way a
	release goes out with the wrong number), and a Nexus file entry that already sits at that version is
	refused unless -Force is given (re-uploading would create a duplicate version).

.PARAMETER Version
	The release being published, e.g. 1.2.2. Must match packages\BlinkDash-<Version>.zip as produced
	by Build-Release.ps1.

.PARAMETER Nexus
	Upload the archive to Nexus Mods as a new version of -FileId.

.PARAMETER GitHub
	Create the GitHub release v<Version> and attach the archive.

.PARAMETER Notes
	Release notes / changelog text. Defaults to the commit subjects since the last release tag.

.PARAMETER Archive
	Archive to publish. Defaults to packages\BlinkDash-<Version>.zip next to the repo root.

.PARAMETER FileId
	Nexus file entry to add a version to. It has to exist already; the API cannot create one.

.PARAMETER ModId
	Nexus mod id, used for the changelog entry and for the read-only sanity check.

.PARAMETER ApiKey
	Nexus API key. Defaults to $env:NEXUS_API_KEY, then to %USERPROFILE%\.pi\nexus-api-key.txt (kept
	outside the repo on purpose). The key is never printed.

.PARAMETER DryRun
	Validate the archive, the Nexus ids and the pushed commit, print what would be uploaded, and
	publish nothing. The only network calls are read-only.

.PARAMETER Force
	Publish a version the Nexus file entry already reports. Only for re-doing a publish that failed
	half-way; it creates a second version with the same number.

.EXAMPLE
	# publish 1.2.2 to both places
	.\tools\Publish-Release.ps1 -Version 1.2.2 -Nexus -GitHub

.EXAMPLE
	# see exactly what would happen, without uploading anything
	.\tools\Publish-Release.ps1 -Version 1.2.2 -Nexus -GitHub -DryRun

.EXAMPLE
	# only the archive and one-line notes, Nexus only
	.\tools\Publish-Release.ps1 -Version 1.2.2 -Nexus -Notes 'Fix the blink refusing on stairs.'
#>
[CmdletBinding()]
param(
	[Parameter(Mandatory)][string]$Version,
	[switch]$Nexus,
	[switch]$GitHub,
	[string]$Notes,
	[string]$Archive,
	[int]$FileId = 822769,
	[int]$ModId = 195907,
	[string]$ApiKey,
	[switch]$DryRun,
	[switch]$Force
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

if ($Version -notmatch '^\d+(\.\d+){1,3}$') { throw "-Version must look like 1.2.2, got '$Version'." }
if (-not ($Nexus -or $GitHub)) { throw 'Nothing to publish: pass -Nexus and/or -GitHub.' }
if (-not $Archive) { $Archive = Join-Path $root "packages\BlinkDash-$Version.zip" }
if (-not (Test-Path $Archive)) {
	throw "No archive at $Archive - build it first: .\tools\Build-Release.ps1 -Version $Version"
}
$archiveItem = Get-Item $Archive

$tag = "v$Version"
$nexusBase = 'https://api.nexusmods.com/v3'
$nexusV1 = 'https://api.nexusmods.com/v1'
$game = 'skyrimspecialedition'

# ---------------------------------------------------------------------------
# Shared helpers
# ---------------------------------------------------------------------------
function Test-ReleaseArchive
{
	param([string]$Path, [string]$Version)

	# A release with no DLL in it is a broken package. Checking here means failing before Nexus has
	# a file version to undo, and before GitHub has a release pointing at nothing.
	Add-Type -AssemblyName System.IO.Compression.FileSystem
	$zip = [System.IO.Compression.ZipFile]::OpenRead($Path)
	try {
		$names = @($zip.Entries | ForEach-Object { $_.FullName })
		foreach ($required in 'SKSE/Plugins/BlinkDash.dll', 'SKSE/Plugins/BlinkDash.ShaderNames.json') {
			if ($names -notcontains $required) { throw "$Path does not contain $required - refusing to publish it." }
		}

		# -Version is a claim about the archive, and the plugin logs BLINKDASH_VERSION at startup, so
		# that string literal sits in the DLL. Reading it back catches the one way a release goes out
		# wrong without anyone noticing: a stale build tree (or -SkipBuild) packaged under a new number.
		$stream = $zip.GetEntry('SKSE/Plugins/BlinkDash.dll').Open()
		try {
			$memory = [System.IO.MemoryStream]::new()
			$stream.CopyTo($memory)
			$bytes = $memory.ToArray()
		}
		finally { $stream.Dispose() }
	}
	finally { $zip.Dispose() }

	# Latin1 decodes byte-for-byte, and the null terminator on the needle stops "1.2.1" from matching
	# inside a longer literal such as "1.2.10".
	$text = [System.Text.Encoding]::Latin1.GetString($bytes)
	if ($text.IndexOf("$Version`0", [System.StringComparison]::Ordinal) -lt 0) {
		throw "The DLL in $Path does not report version $Version - it was built with a different -Version (usually a stale build tree, or -SkipBuild after a bump). Rebuild it before publishing."
	}

	return $names
}

function Invoke-Git
{
	param([string[]]$Arguments)
	$output = & git -C $root @Arguments
	if ($LASTEXITCODE -ne 0) { throw "git $($Arguments -join ' ') failed with exit code $LASTEXITCODE." }
	return $output
}

function Get-ReleaseNotes
{
	param([string]$Override, [string]$Version)

	if ($Override) { return $Override.Trim() }

	# The previous publish is what created the last tag, so the range between it and HEAD is exactly
	# "what changed since the release users have". With no tag yet (or none reachable) the whole
	# history is the honest answer, capped so a first release does not post 300 lines.
	$lastTag = (& git -C $root describe --tags --abbrev=0 2>$null)
	$range = if ($LASTEXITCODE -eq 0 -and $lastTag) { "$lastTag..HEAD" } else { 'HEAD' }
	$subjects = @(& git -C $root log $range --pretty=format:'- %s' --max-count=100)
	if ($LASTEXITCODE -ne 0) { throw "git log $range failed with exit code $LASTEXITCODE." }
	if ($subjects.Count -eq 0) { return "BlinkDash $Version." }
	return ($subjects -join "`n")
}

# ---------------------------------------------------------------------------
# Nexus Mods (v3 Upload API)
# ---------------------------------------------------------------------------
function Get-NexusApiKey
{
	param([string]$Explicit)

	if ($Explicit) { return $Explicit.Trim() }
	if ($env:NEXUS_API_KEY) { return $env:NEXUS_API_KEY.Trim() }

	# Stored outside the repo deliberately: this file must never be committed or end up in an archive.
	$file = Join-Path $env:USERPROFILE '.pi\nexus-api-key.txt'
	if (Test-Path $file) { return (Get-Content $file -Raw).Trim() }

	throw 'No Nexus API key: set NEXUS_API_KEY or write one to %USERPROFILE%\.pi\nexus-api-key.txt.'
}

function Invoke-ApiRequest
{
	<#
		One wrapper for every API call so failures read like something actionable. -SkipHttpErrorCheck
		is the point: it keeps the response body (Nexus puts the reason there) instead of throwing a
		WebException that hides it.
	#>
	param(
		[string]$Method,
		[string]$Uri,
		[hashtable]$Headers,
		$Body,
		[string]$ContentType = 'application/json'
	)

	$parameters = @{
		Method             = $Method
		Uri                = $Uri
		SkipHttpErrorCheck = $true
		StatusCodeVariable = 'statusCode'
		TimeoutSec         = 300
	}
	if ($Headers) { $parameters.Headers = $Headers }
	if ($null -ne $Body) {
		$parameters.Body = $Body
		$parameters.ContentType = $ContentType
	}

	$response = Invoke-RestMethod @parameters
	if ($statusCode -ge 300) {
		$detail = if ($response -is [string]) { $response } else { $response | ConvertTo-Json -Depth 8 -Compress }
		throw "$Method $Uri failed with HTTP $statusCode`: $detail"
	}
	return $response
}

function Send-UploadPart
{
	# The presigned URL signs Content-Type and Content-Length, so both go out exactly as the reference
	# client (Nexus-Mods/upload-action) sends them, and nothing else does.
	param([string]$Url, [byte[]]$Bytes, [int]$Number, [int]$Total)

	Write-Host ("  part {0}/{1} ({2:N0} bytes)" -f $Number, $Total, $Bytes.Length)
	$response = Invoke-WebRequest -Method Put -Uri $Url -Body $Bytes `
		-ContentType 'application/octet-stream' -SkipHttpErrorCheck `
		-StatusCodeVariable 'statusCode' -TimeoutSec 600
	if ($statusCode -ge 300) { throw "Part $Number upload failed with HTTP $statusCode`: $($response.Content)" }

	# The ETag comes back quoted; S3 wants it unquoted in the completion XML.
	$etag = $response.Headers['ETag']
	if (-not $etag) { $etag = $response.Headers['etag'] }
	if ($etag -is [array]) { $etag = $etag[0] }
	if (-not $etag) { throw "Part $Number upload returned no ETag." }
	return ([string]$etag).Trim('"')
}

function Publish-NexusFile
{
	param(
		[string]$Key,
		[string]$Path,
		[int]$FileId,
		[int]$ModId,
		[string]$Version,
		[string]$Notes,
		[switch]$DryRun,
		[switch]$Force
	)

	$headers = @{ apikey = $Key; 'User-Agent' = 'BlinkDash/release' }

	# The ids are defaults in this script, so confirm with read-only calls that they still point at
	# this mod's file before anything is created. Uploading into the wrong entry is not undoable.
	$mod = Invoke-ApiRequest -Method Get -Uri "$nexusV1/games/$game/mods/$ModId.json" -Headers $headers
	$files = @((Invoke-ApiRequest -Method Get -Uri "$nexusV1/games/$game/mods/$ModId/files.json" -Headers $headers).files)
	$target = $files | Where-Object { $_.file_id -eq $FileId } | Select-Object -First 1
	if (-not $target) { throw "File $FileId is not a file of mod $ModId ('$($mod.name)')." }
	Write-Host "Nexus: mod $ModId '$($mod.name)' v$($mod.version); target file $FileId '$($target.name)' v$($target.version) [$($target.category_name)]"

	# Nexus happily accepts a second version carrying the same number, and the API has no delete for
	# one. A half-failed publish is the only reason to want that, so it has to be asked for.
	if ($target.version -eq $Version -and -not $Force) {
		throw "File $FileId already reports version $($target.version) on Nexus - publishing $Version again would create a duplicate. Pass -Force only if that is what you want."
	}
	if ($target.version -eq $Version) {
		Write-Warning "File $FileId already reports version $($target.version); -Force given, so this adds a second version with the same number."
	}

	if ($DryRun) {
		Write-Host ("Dry run: would upload {0} ({1:N0} bytes) as version {2} of file {3}, sync the mod version and post the changelog." -f `
			(Split-Path $Path -Leaf), (Get-Item $Path).Length, $Version, $FileId)
		return
	}

	# 1. Ask for a multipart upload session; Nexus answers with one presigned URL per part.
	$size = (Get-Item $Path).Length
	$body = @{ filename = (Split-Path $Path -Leaf); size_bytes = "$size" } | ConvertTo-Json -Compress
	$session = (Invoke-ApiRequest -Method Post -Uri "$nexusBase/uploads/multipart" -Headers $headers -Body $body).data
	$uploadId = $session.id
	$partUrls = @($session.part_presigned_urls)
	if ($partUrls.Count -eq 0) { throw "Upload $uploadId came back with no part URLs." }
	$partSize = [int]$session.part_size_bytes
	Write-Host "  upload $uploadId`: $($partUrls.Count) part(s) of $partSize bytes"

	# 2. PUT every part and keep the ETags: completion is impossible without them.
	$parts = @()
	$stream = [System.IO.File]::OpenRead($Path)
	try {
		for ($i = 0; $i -lt $partUrls.Count; $i++) {
			$buffer = [byte[]]::new($partSize)
			$read = 0
			while ($read -lt $partSize) {
				$n = $stream.Read($buffer, $read, $partSize - $read)
				if ($n -le 0) { break }
				$read += $n
			}
			if ($read -eq 0) { break }
			$chunk = if ($read -eq $partSize) { $buffer } else { $buffer[0..($read - 1)] }
			$etag = Send-UploadPart -Url $partUrls[$i] -Bytes $chunk -Number ($i + 1) -Total $partUrls.Count
			$parts += [pscustomobject]@{ Number = $i + 1; ETag = $etag }
		}
	}
	finally { $stream.Dispose() }

	# 3. Close the S3 multipart upload with the part list.
	$xml = '<CompleteMultipartUpload>' + (($parts | ForEach-Object {
		"<Part><PartNumber>$($_.Number)</PartNumber><ETag>$($_.ETag)</ETag></Part>"
	}) -join '') + '</CompleteMultipartUpload>'
	Invoke-ApiRequest -Method Post -Uri $session.complete_presigned_url -Body $xml -ContentType 'application/xml' | Out-Null

	# 4. finalise, then wait until Nexus has scanned the archive: versions can only be attached to an
	#    upload in the "available" state, and that lag is Nexus', not a bug here.
	Invoke-ApiRequest -Method Post -Uri "$nexusBase/uploads/$uploadId/finalise" -Headers $headers | Out-Null
	$state = ''
	for ($attempt = 0; $attempt -lt 60; $attempt++) {
		$state = (Invoke-ApiRequest -Method Get -Uri "$nexusBase/uploads/$uploadId" -Headers $headers).data.state
		if ($state -eq 'available') { break }
		Start-Sleep -Milliseconds ([Math]::Min(2000 * [Math]::Pow(1.5, $attempt), 30000))
	}
	if ($state -ne 'available') { throw "Upload $uploadId never became available (last state '$state')." }
	Write-Host '  upload is available'

	# 5. Attach the upload as a new version of the existing file entry, and pull the mod's displayed
	#    version along with it so the page and the DLL cannot disagree.
	$versionBody = @{
		upload_id          = $uploadId
		name               = "BlinkDash $Version"
		version            = $Version
		file_category      = 'main'
		update_mod_version = $true
	} | ConvertTo-Json -Compress
	$created = (Invoke-ApiRequest -Method Post -Uri "$nexusBase/mod-files/$FileId/versions" -Headers $headers -Body $versionBody).data.version
	Write-Host "  created file version $($created.id) '$($created.name)'"

	# 6. The changelog is what users actually read on the mod page, so it gets the same text as the
	#    GitHub release body. It is the only cosmetic step, and it runs after the version is live, so a
	#    failure here must not fail the publish - that would push towards re-uploading and a duplicate.
	if ($Notes) {
		$changelogBody = @{ version = $Version; changelog = $Notes } | ConvertTo-Json -Compress
		try {
			Invoke-ApiRequest -Method Post -Uri "$nexusBase/mods/$ModId/changelogs" -Headers $headers -Body $changelogBody | Out-Null
			Write-Host '  changelog entry added'
		}
		catch {
			Write-Warning "The file version is published, but adding the changelog entry failed: $($_.Exception.Message) - add it on the mod page."
		}
	}
}

# ---------------------------------------------------------------------------
# GitHub (gh CLI)
# ---------------------------------------------------------------------------
function Publish-GitHubRelease
{
	param([string]$Path, [string]$Version, [string]$Notes, [switch]$DryRun)

	# gh creates the tag itself, but it would default to the remote's default branch: if HEAD is not
	# pushed, the release would claim to be a commit nobody else can see. Refuse instead.
	# @() because a single-line git output comes back as a bare string, and indexing a string yields
	# its first character rather than its first line.
	$head = @(Invoke-Git @('rev-parse', 'HEAD'))[0].Trim()
	$upstream = (& git -C $root rev-parse '@{u}' 2>$null | Select-Object -First 1)
	if ($upstream) { $upstream = $upstream.Trim() }
	if (-not $upstream -or $upstream -ne $head) {
		throw "HEAD ($($head.Substring(0, 7))) is not pushed to its upstream branch - push the release commit first."
	}

	if (Invoke-Git @('status', '--porcelain')) {
		Write-Warning 'Working tree has uncommitted changes; the release is made from the pushed commit anyway.'
	}

	if ($DryRun) {
		Write-Host "Dry run: would create GitHub release $tag at $($head.Substring(0, 7)) with $([System.IO.Path]::GetFileName($Path))"
		Write-Host 'Notes:'
		Write-Host $Notes
		return
	}

	$tmp = Join-Path ([System.IO.Path]::GetTempPath()) "blinkdash-$Version-notes.md"
	Set-Content -Path $tmp -Value $Notes -Encoding utf8
	try {
		# --target pins the tag to the commit just verified instead of letting gh guess.
		& gh release create $tag $Path --title "BlinkDash $Version" --notes-file $tmp --target $head
		if ($LASTEXITCODE -ne 0) { throw "gh release create $tag failed with exit code $LASTEXITCODE." }
	}
	finally { Remove-Item $tmp -Force -ErrorAction SilentlyContinue }
	Write-Host "  GitHub release $tag created"
}

# ---------------------------------------------------------------------------
# Go
# ---------------------------------------------------------------------------
Write-Host ("Publishing BlinkDash {0}: {1} ({2:N0} bytes)" -f `
	$Version, $archiveItem.Name, $archiveItem.Length)
Write-Host "  archive has $((Test-ReleaseArchive -Path $archiveItem.FullName -Version $Version).Count) entries"

$notes = Get-ReleaseNotes -Override $Notes -Version $Version

if ($Nexus) {
	$key = Get-NexusApiKey -Explicit $ApiKey
	Publish-NexusFile -Key $key -Path $archiveItem.FullName -FileId $FileId -ModId $ModId `
		-Version $Version -Notes $notes -DryRun:$DryRun -Force:$Force
}

if ($GitHub) {
	Publish-GitHubRelease -Path $archiveItem.FullName -Version $Version -Notes $notes -DryRun:$DryRun
}

if ($DryRun) { Write-Host 'Dry run: nothing was published.' }
else { Write-Host 'Done.' }
