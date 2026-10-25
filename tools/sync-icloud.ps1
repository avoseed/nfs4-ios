# Publish only NFS4's current IPA; retain immutable local and shared history.
param(
    [string]$Target = (Join-Path $env:USERPROFILE 'iCloudDrive\iOS 포트 IPA'),
    [string]$ArchiveRoot = 'E:\ios-ports\ipa',
    [string]$Run = '',
    [string]$Commit = '',
    [string]$Notes = ''
)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Split-Path $PSScriptRoot -Parent))
$local = Join-Path $root 'dist\ipa'
$Target = [IO.Path]::GetFullPath($Target)
$ArchiveRoot = [IO.Path]::GetFullPath($ArchiveRoot)
if (-not (Test-Path -LiteralPath (Split-Path $Target -Parent))) { throw 'iCloud Drive parent does not exist.' }
if (-not (Test-Path -LiteralPath $ArchiveRoot)) { throw 'Shared IPA archive does not exist; pass -ArchiveRoot explicitly.' }
$archive = Join-Path $ArchiveRoot 'NFS4'
$latestDir = Join-Path $ArchiveRoot 'latest'
$pattern = '^NFS4-(\d+\.\d+\.\d+)-b(\d+)\.ipa$'
$stamp = [DateTimeOffset]::UtcNow.ToOffset([TimeSpan]::FromHours(9)).ToString('yyyy-MM-dd HH:mm')

function Assert-InRoot([string]$Path, [string]$Parent) {
    $absolute = [IO.Path]::GetFullPath($Path)
    $boundary = [IO.Path]::GetFullPath($Parent).TrimEnd('\') + '\'
    if (-not $absolute.StartsWith($boundary, [StringComparison]::OrdinalIgnoreCase)) { throw "Path escaped allowed directory: $absolute" }
}
function Hash([string]$Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() }
function Ensure-Copy([string]$Source, [string]$Destination, [string]$AllowedRoot, [string]$ExpectedHash) {
    Assert-InRoot $Destination $AllowedRoot
    New-Item -ItemType Directory -Force (Split-Path $Destination -Parent) | Out-Null
    if (Test-Path -LiteralPath $Destination) {
        if ((Hash $Destination) -ne $ExpectedHash) { throw "Version filename reused with different contents: $Destination" }
        return
    }
    $temp = Join-Path (Split-Path $Destination -Parent) ('.' + [IO.Path]::GetFileName($Destination) + '.' + [Guid]::NewGuid().ToString('N') + '.uploading')
    Assert-InRoot $temp $AllowedRoot
    try {
        Copy-Item -LiteralPath $Source -Destination $temp
        if ((Hash $temp) -ne $ExpectedHash) { throw 'Copy checksum mismatch.' }
        Move-Item -LiteralPath $temp -Destination $Destination
    } finally {
        if (Test-Path -LiteralPath $temp) { Assert-InRoot $temp $AllowedRoot; Remove-Item -LiteralPath $temp }
    }
}
function Other-App-Hashes {
    $result = @{}
    foreach ($dir in @($Target, $latestDir)) {
        if (Test-Path -LiteralPath $dir) {
            foreach ($file in Get-ChildItem -LiteralPath $dir -File -Filter '*.ipa') {
                if ($file.Name -notmatch '^NFS4-') { $result[$file.FullName] = Hash $file.FullName }
            }
        }
    }
    return $result
}

$otherBefore = Other-App-Hashes
$candidates = @()
foreach ($dir in @($local, $archive, $latestDir, $Target)) {
    if (Test-Path -LiteralPath $dir) { $candidates += @(Get-ChildItem -LiteralPath $dir -File -Filter 'NFS4-*.ipa') }
}
if (-not $candidates.Count) { throw 'No versioned NFS4 IPA found.' }
$records = @()
foreach ($group in ($candidates | Group-Object Name)) {
    if ($group.Name -notmatch $pattern) { throw "Unrecognized NFS4 version filename: $($group.Name)" }
    $version, $build = $Matches[1], $Matches[2]
    $source = $group.Group[0]
    $hashes = @($group.Group | ForEach-Object { Hash $_.FullName } | Select-Object -Unique)
    if ($hashes.Count -ne 1) { throw "Conflicting copies of $($group.Name); no history overwritten." }
    $info = python -I (Join-Path $PSScriptRoot 'ipa_info.py') $source.FullName | ConvertFrom-Json
    if ($LASTEXITCODE -ne 0 -or $info.CFBundleIdentifier -ne 'io.github.avoseed.nfs4' -or
        $info.CFBundleShortVersionString -ne $version -or $info.CFBundleVersion -ne $build) { throw "IPA identity mismatch: $($source.FullName)" }
    $records += [pscustomobject]@{ Name=$source.Name; Version=$version; VersionOrder=[version]$version; Build=[long]$build; Source=$source.FullName; SHA256=$hashes[0]; Bytes=$source.Length }
}
$current = $records | Sort-Object VersionOrder,Build -Descending | Select-Object -First 1

# Preserve every known version in both histories before touching any latest copy.
foreach ($record in $records) {
    Ensure-Copy $record.Source (Join-Path $local $record.Name) $local $record.SHA256
    Ensure-Copy $record.Source (Join-Path $archive $record.Name) $archive $record.SHA256
}
Ensure-Copy $current.Source (Join-Path $Target $current.Name) $Target $current.SHA256
Ensure-Copy $current.Source (Join-Path $latestDir $current.Name) $latestDir $current.SHA256

# Single-file removals are restricted to NFS4 and only follow verified archiving.
foreach ($dir in @($Target, $latestDir)) {
    foreach ($file in Get-ChildItem -LiteralPath $dir -File -Filter 'NFS4-*.ipa') {
        if ($file.Name -ieq $current.Name) { continue }
        Assert-InRoot $file.FullName $dir
        $record = $records | Where-Object { $_.Name -ieq $file.Name } | Select-Object -First 1
        if (-not $record -or (Hash (Join-Path $local $record.Name)) -ne $record.SHA256 -or
            (Hash (Join-Path $archive $record.Name)) -ne $record.SHA256) { throw 'Old copy was not safely archived.' }
        Remove-Item -LiteralPath $file.FullName
    }
}

# Rebuild the human-readable list from actual files; other app IPA files are untouched.
$lines = @("iOS 포트 앱 최신 IPA ($stamp 기준)", '')
foreach ($file in Get-ChildItem -LiteralPath $Target -File -Filter '*.ipa' | Sort-Object Name) {
    if ($file.BaseName -match '^(.+)-(\d+\.\d+\.\d+)-b(\d+)$') { $lines += '{0,-12} {1,-8} (빌드 {2})' -f $Matches[1],$Matches[2],$Matches[3] }
}
$lines += '', 'LiveContainer에서 + 를 눌러 이 폴더의 IPA를 고르면 설치(덮어쓰기)됩니다.'
Set-Content -LiteralPath (Join-Path $Target '_버전목록.txt') -Value $lines -Encoding utf8

$historyFile = Join-Path $root 'dist\release-history.json'
$prior = @{}
if (Test-Path -LiteralPath $historyFile) {
    foreach ($entry in @(Get-Content -Raw -LiteralPath $historyFile | ConvertFrom-Json)) { $prior[$entry.filename] = $entry }
}
$history = @()
foreach ($record in $records | Sort-Object VersionOrder,Build) {
    $previous = $prior[$record.Name]
    $isCurrent = $record.Name -ieq $current.Name
    $history += [pscustomobject]@{
        version=$record.Version; build=$record.Build; filename=$record.Name; sha256=$record.SHA256; bytes=$record.Bytes
        first_archived_kst=$(if ($previous) { $previous.first_archived_kst } else { $stamp })
        actions_run=$(if ($isCurrent -and $Run) { $Run } elseif ($previous) { $previous.actions_run } else { '' })
        source_commit=$(if ($isCurrent -and $Commit) { $Commit } elseif ($previous) { $previous.source_commit } else { '' })
        local_archive=(Join-Path $local $record.Name); shared_archive=(Join-Path $archive $record.Name); latest=$isCurrent
    }
}
ConvertTo-Json -InputObject $history -Depth 5 | Set-Content -LiteralPath $historyFile -Encoding utf8
$releaseLog = Join-Path $ArchiveRoot 'RELEASES.md'
if (-not (Test-Path -LiteralPath $releaseLog)) { "# IPA releases`n`n| 날짜 | 앱 | 버전 | 빌드 | 커밋 | 내용 |`n|---|---|---|---|---|---|" | Set-Content -LiteralPath $releaseLog -Encoding utf8 }
$needle = "| NFS4 | $($current.Version) | $($current.Build) |"
if (-not (Select-String -LiteralPath $releaseLog -SimpleMatch $needle -Quiet)) {
    $revision = if ($Commit) { 'avoseed/nfs4-ios@' + $Commit.Substring(0,[Math]::Min(7,$Commit.Length)) } else { 'avoseed/nfs4-ios' }
    $note = $Notes.Replace('|','/').Replace("`r",' ').Replace("`n",' ')
    "| $stamp | NFS4 | $($current.Version) | $($current.Build) | $revision | $note |" | Add-Content -LiteralPath $releaseLog -Encoding utf8
}
$otherAfter = Other-App-Hashes
if ($otherBefore.Count -ne $otherAfter.Count) { throw 'Other app file inventory changed during synchronization.' }
foreach ($name in $otherBefore.Keys) { if ($otherAfter[$name] -ne $otherBefore[$name]) { throw "Other app file changed: $name" } }
if (@(Get-ChildItem -LiteralPath $Target -File -Filter 'NFS4-*.ipa').Count -ne 1) { throw 'iCloud has more than one NFS4 IPA.' }
[pscustomobject]@{ Version=$current.Version; Build=$current.Build; iCloud=(Join-Path $Target $current.Name); SharedArchive=$archive; LocalHistory=$historyFile; SHA256=$current.SHA256; OtherAppFilesPreserved=$otherBefore.Count }
