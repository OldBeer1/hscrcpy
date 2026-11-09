# SPDX-License-Identifier: Apache-2.0
[CmdletBinding()]
param(
    [string]$RepositoryRoot,
    [string]$BuildDirectory,
    [string]$CltRoot,
    [string]$CliRoot,
    [string]$JavaHome = 'C:\Software\DevEco Studio\jbr',
    [string]$Destination,
    [switch]$NoArchive
)
$ErrorActionPreference = 'Stop'
if (-not $RepositoryRoot) { $RepositoryRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path) }
$RepositoryRoot = [IO.Path]::GetFullPath($RepositoryRoot)
if (-not $BuildDirectory) { $BuildDirectory = Join-Path $RepositoryRoot 'build' }
if (-not $CltRoot) { $CltRoot = Join-Path $RepositoryRoot '.tools\command-line-tools' }
if (-not $CliRoot) { $CliRoot = Join-Path $RepositoryRoot '.tools\deveco-cli' }
if (-not $Destination) {
    $Destination = Join-Path $RepositoryRoot ('artifacts\hscrcpy-migration-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
}
$Destination = [IO.Path]::GetFullPath($Destination)
if (Test-Path -LiteralPath $Destination) { throw 'Destination already exists. Choose a new directory; existing files are preserved.' }
$unsigned = Join-Path $RepositoryRoot 'audio-companion\entry\build\default\outputs\default\entry-default-unsigned.hap'
foreach ($required in @($unsigned, (Join-Path $BuildDirectory 'hscrcpy.exe'),
    (Join-Path $CliRoot 'node_modules\@deveco\deveco-cli\dist\cli.js'),
    (Join-Path $JavaHome 'bin\java.exe'), (Join-Path $CltRoot 'sdk\default\sdk-pkg.json'))) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) { throw ('Required input absent: ' + $required) }
}
[IO.Directory]::CreateDirectory($Destination) | Out-Null
function Copy-NewFile([string]$Source, [string]$Relative) {
    $target = [IO.Path]::GetFullPath((Join-Path $Destination $Relative))
    if (-not $target.StartsWith($Destination.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Output path escaped destination.'
    }
    [IO.Directory]::CreateDirectory((Split-Path -Parent $target)) | Out-Null
    [IO.File]::Copy($Source, $target, $false)
}
function Copy-NewTree([string]$Source, [string]$Relative) {
    $sourceRoot = [IO.Path]::GetFullPath($Source).TrimEnd('\')
    if ((Get-Item -LiteralPath $sourceRoot).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Source root is a reparse point.' }
    foreach ($item in Get-ChildItem -LiteralPath $sourceRoot -Recurse -Force) {
        if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw ('Reparse point is not packaged: ' + $item.FullName) }
        if (-not $item.PSIsContainer) {
            $relativeItem = $item.FullName.Substring($sourceRoot.Length + 1)
            Copy-NewFile $item.FullName (Join-Path $Relative $relativeItem)
        }
    }
}
foreach ($name in @('hscrcpy.exe','hdc.exe','libusb_shared.dll','avcodec-62.dll','avutil-60.dll','swscale-9.dll','swresample-6.dll')) {
    Copy-NewFile (Join-Path $BuildDirectory $name) $name
}
foreach ($server in Get-ChildItem -LiteralPath (Join-Path $RepositoryRoot 'server') -File -Filter '*.so') {
    Copy-NewFile $server.FullName ('server\' + $server.Name)
}
Copy-NewTree (Join-Path $RepositoryRoot 'res') 'res'
Copy-NewFile $unsigned 'phone\audio-capture-unsigned.hap'
# Only explicit public project roots and source/resource extensions are allowed.
foreach ($file in @('oh-package.json5','hvigorfile.ts','build-profile.example.json5','README.md',
    'entry\oh-package.json5','entry\hvigorfile.ts','entry\build-profile.json5')) {
    Copy-NewFile (Join-Path $RepositoryRoot ('audio-companion\' + $file)) ('phone\project\audio-companion\' + $file)
}
foreach ($sourceRoot in @('AppScope','hvigor','entry\src\main')) {
    $source = Join-Path $RepositoryRoot ('audio-companion\' + $sourceRoot)
    foreach ($file in Get-ChildItem -LiteralPath $source -Recurse -File) {
        if ($file.FullName -match '\\(?:oh_modules|build|\.hvigor|\.cxx|\.idea)\\') { continue }
        if ($file.Extension -notin @('.json','.json5','.ts','.ets','.cpp','.h','.svg','.png') -and $file.Name -ne 'CMakeLists.txt') { continue }
        $relative = $file.FullName.Substring((Join-Path $RepositoryRoot 'audio-companion').Length + 1)
        Copy-NewFile $file.FullName ('phone\project\audio-companion\' + $relative)
    }
}
Copy-NewFile (Join-Path $RepositoryRoot 'common\audio_protocol.h') 'phone\project\common\audio_protocol.h'
# A signing-only subset of the official CLT. No SDK compiler/IDE is required at runtime.
foreach ($relative in @('version.txt','sdk\default\sdk-pkg.json',
    'sdk\default\openharmony\toolchains\hdc.exe','sdk\default\openharmony\toolchains\libusb_shared.dll',
    'sdk\default\openharmony\toolchains\oh-uni-package.json','sdk\default\openharmony\toolchains\NOTICE.txt',
    'sdk\default\openharmony\toolchains\lib\hap-sign-tool.jar',
    'sdk\default\openharmony\toolchains\lib\PermissionDefinitions.json',
    'sdk\default\openharmony\ets\oh-uni-package.json','sdk\default\openharmony\native\oh-uni-package.json')) {
    Copy-NewFile (Join-Path $CltRoot $relative) ('tools\clt\' + $relative)
}
Copy-NewFile (Join-Path $CltRoot 'tool\node\node.exe') 'tools\clt\tool\node\node.exe'
Copy-NewFile (Join-Path $CltRoot 'tool\node\LICENSE') 'licenses\Node-LICENSE.txt'
Copy-NewTree (Join-Path $CliRoot 'node_modules') 'tools\deveco-cli\node_modules'
Copy-NewTree $JavaHome 'tools\java'
Copy-NewFile (Join-Path $RepositoryRoot 'deployment\phone-tools.mjs') 'deployment\phone-tools.mjs'
Copy-NewFile (Join-Path $RepositoryRoot 'deployment\Assistant.ps1') 'deployment\Assistant.ps1'
Copy-NewFile (Join-Path $RepositoryRoot 'deployment\SensitiveSigner.java') 'deployment\SensitiveSigner.java'
foreach ($name in @('LICENSE','AUDIO.md','MIGRATION.md')) { Copy-NewFile (Join-Path $RepositoryRoot $name) $name }
Copy-NewFile (Join-Path $RepositoryRoot 'deployment\THIRD_PARTY.md') 'licenses\THIRD_PARTY.md'
Copy-NewFile (Join-Path $RepositoryRoot 'deployment\licenses\GPL-3.0.txt') 'licenses\GPL-3.0.txt'
Copy-NewFile (Join-Path $CliRoot 'node_modules\@deveco\deveco-cli\LICENSE') 'licenses\DevEco-CLI-LICENSE'
Copy-NewFile (Join-Path $CliRoot 'node_modules\@deveco\deveco-cli\THIRD-PARTY-LICENSES') 'licenses\DevEco-CLI-DEPENDENCIES'
Copy-NewFile (Join-Path $CltRoot 'sdk\default\openharmony\toolchains\NOTICE.txt') 'licenses\OpenHarmony-SDK-NOTICE.txt'
Copy-NewFile (Join-Path $RepositoryRoot '.tools\nghttp2-1.71.0\COPYING') 'licenses\nghttp2-COPYING'
Copy-NewFile (Join-Path $RepositoryRoot 'common\nlohmann\LICENSE.MIT') 'licenses\nlohmann-LICENSE.MIT'
$layout = [ordered]@{
    formatVersion = 1; appVersion = '2.0.2-audio-migration1'
    node = 'tools/clt/tool/node/node.exe'
    clt = 'tools/clt'
    cli = 'tools/deveco-cli/node_modules/@deveco/deveco-cli/dist/cli.js'
    javaHome = 'tools/java'
    signerBridge = 'deployment/SensitiveSigner.java'
    unsignedHap = 'phone/audio-capture-unsigned.hap'
    project = 'phone/project'
    phoneSha256 = (Get-FileHash -LiteralPath $unsigned -Algorithm SHA256).Hash.ToLowerInvariant()
    cliVersion = '1.3.0-stable'
    ffmpegVersion = 'N-124795-gc27a3b12e3-20260604'
    containsPersonalSigningMaterial = $false
}
[IO.File]::WriteAllText((Join-Path $Destination 'package-layout.json'),($layout | ConvertTo-Json -Depth 6),[Text.UTF8Encoding]::new($false))
foreach ($launcher in @(@('迁移助手.cmd','Menu'),@('开始使用.cmd','Start'),@('准备新手机.cmd','Setup'),
    @('检测手机.cmd','Check'),@('恢复连接.cmd','Recover'),@('校验运行包.cmd','Verify'),@('更新手机签名.cmd','Renew'))) {
    $lines = @('@echo off','setlocal','chcp 65001 >nul',
        ('"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0deployment\Assistant.ps1" -Action ' + $launcher[1]),
        'set "HSCRCPY_RESULT=%ERRORLEVEL%"')
    if ($launcher[1] -in @('Start','Menu')) { $lines += 'if not "%HSCRCPY_RESULT%"=="0" pause' } else { $lines += 'pause' }
    $lines += 'exit /b %HSCRCPY_RESULT%'
    # Windows PowerShell 5.1 reads the BOM in the referenced script; CMD files stay ASCII.
    [IO.File]::WriteAllLines((Join-Path $Destination $launcher[0]),$lines,[Text.ASCIIEncoding]::new())
}
# A source snapshot identifies the code behind the desktop binary.
foreach ($relative in @('src','common','deployment','tests')) { Copy-NewTree (Join-Path $RepositoryRoot $relative) ('source\' + $relative) }
foreach ($relative in @('CMakeLists.txt','README.md','LICENSE','MIGRATION.md')) {
    Copy-NewFile (Join-Path $RepositoryRoot $relative) ('source\' + $relative)
}
$manifest = foreach ($file in Get-ChildItem -LiteralPath $Destination -Recurse -File) {
    [ordered]@{path=$file.FullName.Substring($Destination.Length+1).Replace('\','/'); bytes=$file.Length;
        sha256=(Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()}
}
[IO.File]::WriteAllText((Join-Path $Destination 'files.sha256.json'),($manifest | ConvertTo-Json -Depth 4),[Text.UTF8Encoding]::new($false))
Write-Host ('Prepared migration directory: ' + $Destination)
if (-not $NoArchive) {
    $archive = $Destination + '.zip'
    if (Test-Path -LiteralPath $archive) { throw 'Archive already exists; it was preserved.' }
    & (Join-Path $PSScriptRoot 'Export-Kit.ps1') -PackageRoot $Destination -ArchivePath $archive
}
