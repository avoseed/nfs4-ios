# Downloads an owner-encrypted CI artifact and creates a local installable IPA.
# Keeps immutable package history and synchronizes only the latest NFS4 IPA to iCloud.
param(
    [string]$Repo = 'avoseed/nfs4-ios',
    [string]$App = 'NFS4',
    [string]$Run = '',
    [string]$Notes = '',
    [switch]$SkipICloud,
    [string]$ArchiveRoot = 'E:\ios-ports\ipa',
    [string]$CloudTarget = (Join-Path $env:USERPROFILE 'iCloudDrive\iOS 포트 IPA')
)
$ErrorActionPreference = 'Stop'
if ($Repo -ne 'avoseed/nfs4-ios' -or $App -ne 'NFS4') { throw 'Unexpected repository or app.' }
$root = [IO.Path]::GetFullPath((Split-Path $PSScriptRoot -Parent))
if (-not $Run) {
    $Run = gh run list -R $Repo --status success --limit 1 --json databaseId --jq '.[0].databaseId'
    if ($LASTEXITCODE -ne 0 -or -not $Run) { throw 'No successful CI run.' }
}
if ($Run -notmatch '^\d+$') { throw 'Run must be a numeric Actions run ID.' }
$names = gh api "repos/$Repo/actions/runs/$Run/artifacts" --jq '.artifacts[].name'
if ($LASTEXITCODE -ne 0) { throw 'Artifact listing failed.' }
$artifact = $names | Where-Object { $_ -match '^NFS4-(\d+\.\d+\.\d+)-b(\d+)-encrypted$' } | Select-Object -First 1
if (-not $artifact) { throw 'Encrypted IPA artifact not found.' }
$null = $artifact -match '^NFS4-(\d+\.\d+\.\d+)-b(\d+)-encrypted$'
$version, $build = $Matches[1], $Matches[2]
$baseName = "NFS4-$version-b$build"
$download = Join-Path $root "tmp\artifact-$Run"
New-Item -ItemType Directory -Force $download | Out-Null
gh run download $Run -R $Repo -n $artifact -D $download
if ($LASTEXITCODE -ne 0) { throw 'Artifact download failed.' }
$outDir = Join-Path $root 'dist\ipa'
New-Item -ItemType Directory -Force $outDir | Out-Null
$file = Join-Path $outDir "$baseName.ipa"
if (-not (Test-Path -LiteralPath $file)) {
    python -B (Join-Path $PSScriptRoot 'ci_crypto.py') decrypt (Join-Path $download "$baseName.ipa.aes") $file
    if ($LASTEXITCODE -ne 0) { throw 'Artifact decryption failed.' }
}
$info = python -I (Join-Path $PSScriptRoot 'ipa_info.py') $file | ConvertFrom-Json
if ($LASTEXITCODE -ne 0 -or $info.CFBundleShortVersionString -ne $version -or $info.CFBundleVersion -ne $build) {
    throw 'IPA version does not match the CI artifact.'
}
$log = Join-Path $root 'dist\RELEASES.md'
if (-not (Test-Path -LiteralPath $log)) {
    "# Local app packages`n`n| Version | Build | Actions run | Notes |`n|---|---|---|---|" | Set-Content -LiteralPath $log -Encoding utf8
}
if (-not (Select-String -LiteralPath $log -SimpleMatch "| $version | $build |" -Quiet)) {
    "| $version | $build | $Run | $Notes |" | Add-Content -LiteralPath $log -Encoding utf8
}
if (-not $SkipICloud) {
    $revision = gh run view $Run -R $Repo --json headSha --jq .headSha
    if ($LASTEXITCODE -ne 0) { throw 'Cannot resolve build source revision.' }
    & (Join-Path $PSScriptRoot 'sync-icloud.ps1') -Target $CloudTarget -ArchiveRoot $ArchiveRoot -Run $Run -Commit $revision -Notes $Notes
}
[pscustomobject]@{ Version = $version; Build = $build; File = $file; PublicRelease = $false; iCloudSynced = (-not $SkipICloud) }
