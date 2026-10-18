# Generates the effect shader name file (JSON) from the EFSH records in the given
# plugins. Skyrim strips editor IDs at runtime (TESForm::GetFormEditorID() returns
# "" unless the game was launched in editor mode), so the names have to come from
# the files themselves.
#
# The plugin still discovers every shader at runtime; this file only supplies the
# readable label for the ones it knows about. Anything not listed keeps its plugin
# and form id as the label.
#
# Keys are "<plugin file name>:<form id inside that plugin>", which is stable
# across load order changes.
#
# Usage:
#   .\tools\GenerateShaderNames.ps1 -Files <esm paths...> -Out dist\BlinkDash.ShaderNames.json
param(
	[Parameter(Mandatory = $true)][string[]] $Files,
	[string] $Out = "dist\BlinkDash.ShaderNames.json"
)

# "InvisFXShaderNightingale" -> "Invis Nightingale", "DLC1DeathHoundFXS" -> "Death Hound".
function Get-PrettyName {
	param([string] $edid)

	$n = $edid
	$n = $n -replace '^DLC0?1(?=[A-Z])', ''
	$n = $n -replace '^DLC0?2(?=[A-Z])', ''
	$n = $n -replace '^(MAG|FX)(?=[A-Z])', ''
	$n = $n -replace 'FXShader', ''
	$n = $n -replace 'FXS', ''
	$n = $n -replace 'Shader', ''
	$n = $n -replace 'FX(?=\d*$)', ''
	$n = $n -replace '_', ' '
	# -creplace: PowerShell's -replace is case insensitive, which would make the
	# camel case rules match between every pair of letters.
	$n = $n -creplace '(?<=[a-z0-9])(?=[A-Z])', ' '
	$n = $n -creplace '(?<=[A-Z])(?=[A-Z][a-z])', ' '
	$n = $n -creplace '(?<=[A-Za-z])(?=\d)', ' '
	$n = $n -replace '\s+', ' '
	$n = $n.Trim()

	if ([string]::IsNullOrWhiteSpace($n)) { return $edid }
	return $n
}

$latin1 = [System.Text.Encoding]::GetEncoding(28591)
$map = [ordered]@{}
$total = 0

foreach ($file in $Files) {
	if (-not (Test-Path $file)) {
		Write-Host "SKIP (missing): $file"
		continue
	}

	$bytes = [System.IO.File]::ReadAllBytes($file)
	$plugin = [System.IO.Path]::GetFileName($file)
	# 1 byte -> 1 char, so string indices are byte offsets.
	$text = $latin1.GetString($bytes)
	$found = 0
	$idx = 0

	while ($true) {
		$idx = $text.IndexOf("EFSH", $idx)
		if ($idx -lt 0) { break }

		$h = $idx
		$idx += 4

		if (($h + 30) -gt $bytes.Length) { continue }

		$dataSize = [BitConverter]::ToUInt32($bytes, $h + 4)
		$formID = [BitConverter]::ToUInt32($bytes, $h + 12)

		# A real EFSH record starts with the EDID subrecord, has a sane size and
		# fits inside the file. Anything else is a byte pattern inside other data.
		if ($text.Substring($h + 24, 4) -ne "EDID") { continue }
		if ($dataSize -lt 8 -or $dataSize -gt 0x4000) { continue }
		if (($h + 24 + $dataSize) -gt $bytes.Length) { continue }

		$edidLen = [BitConverter]::ToUInt16($bytes, $h + 28)
		if ($edidLen -le 0 -or $edidLen -gt 200) { continue }
		if (($h + 30 + $edidLen) -gt $bytes.Length) { continue }

		$edid = $latin1.GetString($bytes, $h + 30, $edidLen)
		# the subrecord is NUL terminated
		$nul = $edid.IndexOf([char]0)
		if ($nul -ge 0) { $edid = $edid.Substring(0, $nul) }
		$key = "{0}:{1:X6}" -f $plugin, ($formID -band 0xFFFFFF)
		$map[$key] = (Get-PrettyName $edid)
		$found++
		$total++
	}

	Write-Host ("{0}: {1} EFSH records" -f $plugin, $found)
	$bytes = $null
	$text = $null
}

# Sort the keys so the file is easy to hand edit.
$sorted = [ordered]@{}
foreach ($key in ($map.Keys | Sort-Object)) {
	$sorted[$key] = $map[$key]
}
$json = $sorted | ConvertTo-Json -Depth 3

$dir = Split-Path -Parent $Out
if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Path $dir | Out-Null }
# nlohmann::json rejects a UTF-8 BOM, so write without one.
[System.IO.File]::WriteAllText($Out, $json, (New-Object System.Text.UTF8Encoding($false)))
Write-Host ("wrote {0} names to {1}" -f $total, $Out)
