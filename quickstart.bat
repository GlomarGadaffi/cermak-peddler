@echo off
setlocal enabledelayedexpansion

echo ===================================================
echo             pocket-dial - Quickstart Build
echo ===================================================
echo.

rem Check if CMake is in PATH
where cmake >nul 2>nul
if %errorlevel% equ 0 (
    set "CMAKE_BIN=cmake"
    goto cmake_found
)

echo [DEBUG] CMake not found in system PATH. Searching standard directories...

rem Discovery logic for bundled CMake inside VS and standard installations
if exist "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" (
    set "CMAKE_BIN=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    set "PATH=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%PATH%"
    echo [DEBUG] Found CMake in Visual Studio Build Tools 2026/18.
    goto cmake_found
)
if exist "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" (
    set "CMAKE_BIN=C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    set "PATH=C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%PATH%"
    echo [DEBUG] Found CMake in Visual Studio 2022 Community.
    goto cmake_found
)
if exist "C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" (
    set "CMAKE_BIN=C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    set "PATH=C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%PATH%"
    echo [DEBUG] Found CMake in Visual Studio 2022 Professional.
    goto cmake_found
)
if exist "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" (
    set "CMAKE_BIN=C:\Program Files\Microsoft Visual Studio\2022\Enterprise\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    set "PATH=C:\Program Files\Microsoft Visual Studio\2022\Enterprise\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%PATH%"
    echo [DEBUG] Found CMake in Visual Studio 2022 Enterprise.
    goto cmake_found
)
if exist "C:\Program Files\CMake\bin\cmake.exe" (
    set "CMAKE_BIN=C:\Program Files\CMake\bin\cmake.exe"
    set "PATH=C:\Program Files\CMake\bin;%PATH%"
    echo [DEBUG] Found CMake in Program Files.
    goto cmake_found
)
if exist "C:\Program Files (x86)\CMake\bin\cmake.exe" (
    set "CMAKE_BIN=C:\Program Files (x86)\CMake\bin\cmake.exe"
    set "PATH=C:\Program Files (x86)\CMake\bin;%PATH%"
    echo [DEBUG] Found CMake in Program Files (x86).
    goto cmake_found
)

echo [ERROR] CMake was not found on your system.
echo Please install CMake from https://cmake.org/download/
echo or ensure Visual Studio is installed with C++ development support.
echo.
pause
exit /b 1

:cmake_found
echo Using CMake at: "%CMAKE_BIN%"
echo.

echo [1/3] Configuring project with CMake...
"%CMAKE_BIN%" -B build -DCMAKE_BUILD_TYPE=Release
if %errorlevel% neq 0 (
    echo [ERROR] CMake configuration failed.
    echo Please make sure you have Visual Studio or Build Tools installed.
    echo.
    pause
    exit /b 1
)

echo.
echo [2/3] Building executable...
"%CMAKE_BIN%" --build build --config Release
if %errorlevel% neq 0 (
    echo [ERROR] Build failed.
    echo.
    pause
    exit /b 1
)

echo.
echo [3/3] Launching pocket-dial server...
echo.

rem Detect binary location
set "RUN_CMD="
if exist "build\Release\SipServer.exe" (
    set "RUN_CMD=build\Release\SipServer.exe"
) else if exist "build\SipServer.exe" (
    set "RUN_CMD=build\SipServer.exe"
)

if "%RUN_CMD%"=="" (
    echo [ERROR] Could not locate the built SipServer.exe binary.
    pause
    exit /b 1
)

rem Start the web browser automatically after a short delay
echo Starting dashboard in your default browser...
start "" "http://localhost:8080/"

rem Run the server
"%RUN_CMD%"

pause
