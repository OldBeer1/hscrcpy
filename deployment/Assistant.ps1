# SPDX-License-Identifier: Apache-2.0
[CmdletBinding()]
param(
    [ValidateSet('Menu','Check','Start','Setup','Renew','Recover','Install','Verify')]
    [string]$Action = 'Menu',
    [string]$PackageRoot,
    [string]$HapPath
)
$ErrorActionPreference = 'Stop'
try {
    [Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
    $OutputEncoding = [Console]::OutputEncoding
} catch { }
$scriptDirectory = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $PackageRoot) { $PackageRoot = Split-Path -Parent $scriptDirectory }
$PackageRoot = [IO.Path]::GetFullPath($PackageRoot)
$layout = Get-Content -LiteralPath (Join-Path $PackageRoot 'package-layout.json') -Raw -Encoding UTF8 | ConvertFrom-Json
$node = Join-Path $PackageRoot $layout.node
$helper = Join-Path $scriptDirectory 'phone-tools.mjs'
function Invoke-PhoneAction([string]$Name, [string[]]$ExtraArguments = @()) {
    & $node $helper $Name --root $PackageRoot @ExtraArguments
    if ($LASTEXITCODE -ne 0) { throw '操作未完成，请按上面的提示处理。不会自动卸载已有应用。' }
}
function Test-PackageFiles {
    $manifest = Get-Content -LiteralPath (Join-Path $PackageRoot 'files.sha256.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    foreach ($file in $manifest) {
        $target = [IO.Path]::GetFullPath((Join-Path $PackageRoot $file.path))
        if (-not $target.StartsWith($PackageRoot.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
            throw '校验清单包含目录外路径。'
        }
        if (-not (Test-Path -LiteralPath $target -PathType Leaf)) { throw ('缺少文件：' + $file.path) }
        if ((Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash -ine $file.sha256) {
            throw ('文件校验失败：' + $file.path)
        }
    }
    Write-Host '运行包文件校验通过。' -ForegroundColor Green
}
function Invoke-SelectedAction([string]$Name) {
    switch ($Name) {
        'Check' { Invoke-PhoneAction 'check' }
        'Setup' { Invoke-PhoneAction 'setup' }
        'Renew' { Invoke-PhoneAction 'setup' @('--renew') }
        'Recover' {
            Write-Host '这会重启电脑端共享 HDC 服务，已有调试连接需要重连。'
            Invoke-PhoneAction 'recover'
            Write-Host '看到 Connected 后，在投屏窗口点击刷新和开始按钮。'
        }
        'Verify' { Test-PackageFiles }
        'Install' {
            if (-not $HapPath) {
                Add-Type -AssemblyName System.Windows.Forms
                $dialog = New-Object System.Windows.Forms.OpenFileDialog
                try {
                    $dialog.Filter = 'HarmonyOS signed package (*.hap)|*.hap'
                    $dialog.Title = '选择你为这部手机签名的 hscrcpy 音频安装包'
                    if ($dialog.ShowDialog() -ne [System.Windows.Forms.DialogResult]::OK) { return }
                    $script:HapPath = $dialog.FileName
                } finally { $dialog.Dispose() }
            }
            Invoke-PhoneAction 'install' @('--hap', $HapPath)
        }
        'Start' {
            Invoke-PhoneAction 'setup'
            $deviceText = & $node $helper device-info --root $PackageRoot
            if ($LASTEXITCODE -ne 0) { throw '无法确定本次要连接的手机。' }
            $device = ($deviceText -join [Environment]::NewLine) | ConvertFrom-Json
            if ($device.serial -notmatch '^[A-Za-z0-9_.:-]+$') { throw '设备标识无效。' }
            $exe = Join-Path $PackageRoot 'hscrcpy.exe'
            $running = Get-Process -Name hscrcpy -ErrorAction SilentlyContinue | Where-Object {
                try { [IO.Path]::GetFullPath($_.Path) -ieq $exe } catch { $false }
            }
            if ($running) {
                Write-Host '投屏程序已打开，请在现有窗口刷新设备并点开始。'
            } else {
                $previousPath = $env:PATH
                try {
                    $env:PATH = $PackageRoot + ';' + $env:PATH
                    Start-Process -FilePath $exe -ArgumentList @('--auto-start','--audio','-s',$device.serial) -WorkingDirectory $PackageRoot
                } finally { $env:PATH = $previousPath }
            }
        }
    }
}
try {
    if ($Action -ne 'Menu') { Invoke-SelectedAction $Action; return }
    do {
        Write-Host ''
        Write-Host 'hscrcpy 手机与电脑迁移助手' -ForegroundColor Cyan
        Write-Host '1 开始使用（未准备的手机会进入签名安装流程）'
        Write-Host '2 只检测手机'
        Write-Host '3 准备新手机'
        Write-Host '4 安装已有签名包'
        Write-Host '5 恢复连接（重启共享 HDC 服务）'
        Write-Host '6 校验运行包'
        Write-Host '7 更新手机签名（到期时使用；不会自动卸载旧应用）'
        Write-Host '0 退出'
        $choice = Read-Host '请选择'
        $selected = switch ($choice) {
            '1' {'Start'} '2' {'Check'} '3' {'Setup'} '4' {'Install'} '5' {'Recover'} '6' {'Verify'} '7' {'Renew'} default {$null}
        }
        if ($selected) {
            try { Invoke-SelectedAction $selected } catch { Write-Host $_.Exception.Message -ForegroundColor Red }
        }
    } while ($choice -ne '0')
} catch { Write-Error $_.Exception.Message; exit 1 }
