@echo off
setlocal EnableExtensions EnableDelayedExpansion

title VRam V1.0 - Build Script
cd /d "%~dp0"

echo ==================================================
echo   VRam V1.0 - Native C++ Release x64 Builder
echo ==================================================
echo.

set "MISSING_CMAKE=0"
set "MISSING_VS=0"
set "CMAKE_EXE="

where cmake >nul 2>&1
if %errorlevel%==0 (
    for /f "delims=" %%I in ('where cmake') do (
        set "CMAKE_EXE=%%I"
        goto :cmake_found
    )
)
if exist "%ProgramFiles%\CMake\bin\cmake.exe" (
    set "CMAKE_EXE=%ProgramFiles%\CMake\bin\cmake.exe"
    set "PATH=%ProgramFiles%\CMake\bin;%PATH%"
    goto :cmake_found
)
set "MISSING_CMAKE=1"
:cmake_found

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VS_PATH="
set "VS_VER="
if exist "%VSWHERE%" (
    for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
        set "VS_PATH=%%I"
    )
    for /f "usebackq delims=" %%V in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationVersion`) do (
        set "VS_VER=%%V"
    )
)
if not defined VS_PATH (
    set "MISSING_VS=1"
)

if "%MISSING_CMAKE%"=="0" if "%MISSING_VS%"=="0" goto :tools_ready

echo [!] Build tools yang dibutuhkan belum lengkap:
if "%MISSING_CMAKE%"=="1" echo     - CMake ^(https://cmake.org/download/^)
if "%MISSING_VS%"=="1" echo     - Visual Studio 2022 / Build Tools C++ ^(Desktop development with C++^)
echo.
echo Pilih aksi:
echo   [1] Download / Install sekarang
echo   [2] Batal
echo.
choice /C 12 /N /M "Masukkan pilihan (1/2): "
if errorlevel 2 (
    echo.
    echo Build dibatalkan oleh pengguna.
    pause
    exit /b 1
)

echo.
where winget >nul 2>&1
if %errorlevel%==0 (
    if "%MISSING_CMAKE%"=="1" (
        echo [*] Mengunduh dan menginstall CMake melalui winget...
        winget install --id Kitware.CMake -e --accept-package-agreements --accept-source-agreements
        if exist "%ProgramFiles%\CMake\bin\cmake.exe" (
            set "CMAKE_EXE=%ProgramFiles%\CMake\bin\cmake.exe"
            set "PATH=%ProgramFiles%\CMake\bin;%PATH%"
            set "MISSING_CMAKE=0"
        )
    )
    if "%MISSING_VS%"=="1" (
        echo [*] Mengunduh dan menginstall Visual Studio 2022 Build Tools ^(C++^)...
        winget install --id Microsoft.VisualStudio.2022.BuildTools -e --accept-package-agreements --accept-source-agreements --override "--wait --passive --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
    )
) else (
    echo [!] winget tidak tersedia. Membuka halaman download resmi di browser...
    if "%MISSING_CMAKE%"=="1" start "" "https://cmake.org/download/"
    if "%MISSING_VS%"=="1" start "" "https://visualstudio.microsoft.com/visual-cpp-build-tools/"
)

echo.
echo Jika instalasi baru selesai, tutup terminal ini lalu jalankan ulang build.bat.
pause
exit /b 1

:tools_ready
echo [OK] CMake : %CMAKE_EXE%
echo [OK] MSVC  : %VS_PATH% ^(%VS_VER%^)
echo.

tasklist /FI "IMAGENAME eq VRam.exe" 2>nul | find /I "VRam.exe" >nul
if %errorlevel%==0 (
    echo [!] VRam.exe sedang berjalan dan dapat mengunci output linker.
    choice /C YN /N /M "Tutup VRam.exe yang sedang berjalan sekarang? (Y/N): "
    if errorlevel 2 (
        echo Build dibatalkan. Tutup VRam.exe terlebih dahulu lalu jalankan build.bat lagi.
        pause
        exit /b 1
    )
    taskkill /F /IM VRam.exe >nul 2>&1
    timeout /t 1 /nobreak >nul
)

set "CMAKE_GEN=Visual Studio 17 2022"
echo %VS_VER% | findstr /B "16." >nul && set "CMAKE_GEN=Visual Studio 16 2019"
echo %VS_VER% | findstr /B "18." >nul && set "CMAKE_GEN=Visual Studio 18 2026"

echo [*] Configuring CMake ^(%CMAKE_GEN% - x64^)...
"%CMAKE_EXE%" -B build -G "%CMAKE_GEN%" -A x64
if errorlevel 1 (
    echo [ERROR] CMake configure gagal.
    pause
    exit /b 1
)

echo.
echo [*] Building Release x64...
"%CMAKE_EXE%" --build build --config Release
if errorlevel 1 (
    echo [ERROR] Build gagal.
    pause
    exit /b 1
)

echo.
echo ==================================================
echo   Build selesai: build\Release\VRam.exe
echo ==================================================
pause
exit /b 0
