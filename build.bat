@echo off
setlocal

set CYGWIN=D:\Program Files\cygwin64
set CMAKE=%CYGWIN%\bin\cmake.exe
set GCC=%CYGWIN%\bin\x86_64-w64-mingw32-gcc.exe
set GXX=%CYGWIN%\bin\x86_64-w64-mingw32-g++.exe

echo === HScrcpy Build Script ===
echo.

if "%1"=="clean" (
    echo Cleaning build directory...
    if exist build rmdir /s /q build
    echo Done.
    goto :end
)

if not exist build mkdir build

echo Configuring...
"%CMAKE%" -B build -S . -DCMAKE_C_COMPILER="/cygdrive/d/Program Files/cygwin64/bin/x86_64-w64-mingw32-gcc.exe" -DCMAKE_CXX_COMPILER="/cygdrive/d/Program Files/cygwin64/bin/x86_64-w64-mingw32-g++.exe" -DCMAKE_SYSTEM_NAME=Windows
if %errorlevel% neq 0 (
    echo Configuration failed!
    goto :error
)

echo.
echo Building...
"%CMAKE%" --build build -j
if %errorlevel% neq 0 (
    echo Build failed!
    goto :error
)

echo.
echo Copying runtime files...

if not exist build\server (
    if exist server (
        xcopy /E /I /Y server build\server
        echo   server\ - copied
    ) else (
        echo   WARNING: server\ directory not found!
    )
) else (
    echo   server\ - already exists
)

set FFMPEG_DIR=ffmpeg\ffmpeg-master-latest-win64-gpl-shared\bin
if exist "%FFMPEG_DIR%" (
    for %%f in (avcodec-62.dll avutil-60.dll swscale-9.dll swresample-6.dll avcodec-61.dll avutil-59.dll swscale-8.dll) do (
        if exist "%FFMPEG_DIR%\%%f" (
            copy /Y "%FFMPEG_DIR%\%%f" build\ >nul 2>&1
            echo   %%f - copied
        )
    )
) else (
    echo   FFmpeg DLLs not found in ffmpeg\ - gRPC streaming will use snapshot fallback
)

echo.
echo === Build succeeded ===
echo Output: build\hscrcpy.exe
echo.

goto :end

:error
echo.
echo === Build FAILED ===
exit /b 1

:end
endlocal
