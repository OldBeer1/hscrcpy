# SPDX-License-Identifier: Apache-2.0
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$PackageRoot,
    [string]$ArchivePath
)
$ErrorActionPreference = 'Stop'
$PackageRoot = [IO.Path]::GetFullPath($PackageRoot).TrimEnd('\')
if (-not $ArchivePath) { $ArchivePath = $PackageRoot + '.zip' }
$ArchivePath = [IO.Path]::GetFullPath($ArchivePath)
$hashPath = $ArchivePath + '.sha256'
if ((Test-Path -LiteralPath $ArchivePath) -or (Test-Path -LiteralPath $hashPath)) {
    throw 'Archive or checksum already exists; existing files are preserved.'
}
$manifestPath = Join-Path $PackageRoot 'files.sha256.json'
$manifest = Get-Content -LiteralPath $manifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
if ($manifest.Count -eq 0) { throw 'Package manifest is empty.' }
$seen = @{}
foreach ($file in $manifest) {
    $source = [IO.Path]::GetFullPath((Join-Path $PackageRoot $file.path))
    if (-not $source.StartsWith($PackageRoot + '\',[StringComparison]::OrdinalIgnoreCase)) {
        throw 'Manifest contains a path outside the package.'
    }
    $key = $file.path.Replace('\','/')
    if ($seen.ContainsKey($key) -or $key -eq 'files.sha256.json') { throw 'Manifest contains a duplicate or reserved path.' }
    if ((Get-Item -LiteralPath $source).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Manifest file is a reparse point.' }
    if ((Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash -ine $file.sha256) {
        throw ('Package file changed: ' + $file.path)
    }
    $seen[$key] = $source
}
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$stream = [IO.File]::Open($ArchivePath,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
$archive = [IO.Compression.ZipArchive]::new($stream,[IO.Compression.ZipArchiveMode]::Create,$false)
$prefix = [IO.Path]::GetFileName($PackageRoot) + '/'
try {
    foreach ($key in ($seen.Keys | Sort-Object)) {
        [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive,$seen[$key],$prefix+$key,
            [IO.Compression.CompressionLevel]::Optimal) | Out-Null
    }
    [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive,$manifestPath,$prefix+'files.sha256.json',
        [IO.Compression.CompressionLevel]::Optimal) | Out-Null
} finally { $archive.Dispose(); $stream.Dispose() }
# Validate the archive entry whitelist. Runtime logs/settings or signing state are never added.
$readback = [IO.Compression.ZipFile]::OpenRead($ArchivePath)
try {
    if ($readback.Entries.Count -ne $seen.Count + 1) { throw 'Archive entry count differs from the public manifest.' }
    foreach ($entry in $readback.Entries) {
        if (-not $entry.FullName.StartsWith($prefix,[StringComparison]::Ordinal)) { throw 'Unexpected archive path.' }
        $key = $entry.FullName.Substring($prefix.Length)
        if ($key -ne 'files.sha256.json' -and -not $seen.ContainsKey($key)) { throw 'Unexpected file was included in archive.' }
    }
} finally { $readback.Dispose() }
$hash = (Get-FileHash -LiteralPath $ArchivePath -Algorithm SHA256).Hash.ToLowerInvariant()
$line = $hash + '  ' + [IO.Path]::GetFileName($ArchivePath) + [Environment]::NewLine
$hashStream = [IO.File]::Open($hashPath,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
try { $bytes = [Text.Encoding]::ASCII.GetBytes($line); $hashStream.Write($bytes,0,$bytes.Length) }
finally { $hashStream.Dispose() }
Get-Item -LiteralPath $ArchivePath | Select-Object FullName,Length
Write-Host ('SHA-256: ' + $hash)
