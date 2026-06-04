# FFmpeg 共享库下载脚本
# 用法: .\download_ffmpeg.ps1
# 下载 BtbN 构建的 FFmpeg GPL shared 版本到 ffmpeg/ 目录

$FFMPEG_DIR = "ffmpeg"
$ARCHIVE_NAME = "ffmpeg-master-latest-win64-gpl-shared.zip"
$DOWNLOAD_URL = "https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/$ARCHIVE_NAME"
$EXTRACT_DIR = "$FFMPEG_DIR\ffmpeg-master-latest-win64-gpl-shared"

# 检查是否已存在
if (Test-Path "$EXTRACT_DIR\include\libavcodec\avcodec.h") {
    Write-Host "FFmpeg 已存在于 $EXTRACT_DIR，跳过下载。" -ForegroundColor Green
    Write-Host "如需重新下载，请删除该目录后重新运行此脚本。" -ForegroundColor Yellow
    exit 0
}

# 确保目录存在
if (-not (Test-Path $FFMPEG_DIR)) {
    New-Item -ItemType Directory -Path $FFMPEG_DIR | Out-Null
}

Write-Host "正在下载 FFmpeg 共享库..." -ForegroundColor Cyan
Write-Host "URL: $DOWNLOAD_URL" -ForegroundColor Gray

try {
    # 使用 .NET WebClient 下载（兼容性好）
    $client = New-Object System.Net.WebClient
    $client.DownloadFile($DOWNLOAD_URL, "$FFMPEG_DIR\$ARCHIVE_NAME")
    $client.Dispose()
} catch {
    # 备用：调用 curl
    Write-Host "WebClient 下载失败，尝试 curl..." -ForegroundColor Yellow
    & curl -L -o "$FFMPEG_DIR\$ARCHIVE_NAME" "$DOWNLOAD_URL"
}

# 解压
Write-Host "正在解压..." -ForegroundColor Cyan
Expand-Archive -Path "$FFMPEG_DIR\$ARCHIVE_NAME" -DestinationPath $FFMPEG_DIR -Force

# 清理 zip
Remove-Item "$FFMPEG_DIR\$ARCHIVE_NAME" -Force

Write-Host "完成!" -ForegroundColor Green
Write-Host "FFmpeg 已下载到: $EXTRACT_DIR" -ForegroundColor Green
Write-Host ""
Write-Host "运行时还需要将以下 DLL 复制到 exe 目录 (build.bat 会自动处理):" -ForegroundColor Yellow
Write-Host "  $EXTRACT_DIR\bin\avcodec-62.dll"
Write-Host "  $EXTRACT_DIR\bin\avutil-60.dll"
Write-Host "  $EXTRACT_DIR\bin\swscale-9.dll"
Write-Host "  $EXTRACT_DIR\bin\swresample-6.dll"