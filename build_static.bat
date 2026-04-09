@echo off
REM 静态编译脚本 for thread_scheduler (Windows 版本)
REM 使用方法：build_static.bat

setlocal enabledelayedexpansion

echo ==========================================
echo Thread Scheduler - Static Build (Windows)
echo ==========================================

set SCRIPT_DIR=%~dp0
set BUILD_DIR=%SCRIPT_DIR%build_static

REM Clean build directory
if exist "%BUILD_DIR%" (
    echo Cleaning old build directory...
    rmdir /s /q "%BUILD_DIR%"
)

REM Clean stale cmake artifacts from source directory
echo Cleaning stale CMake artifacts from source directory...
if exist "%SCRIPT_DIR%CMakeCache.txt" del "%SCRIPT_DIR%CMakeCache.txt"
if exist "%SCRIPT_DIR%Makefile" del "%SCRIPT_DIR%Makefile"
if exist "%SCRIPT_DIR%cmake_install.cmake" del "%SCRIPT_DIR%cmake_install.cmake"
if exist "%SCRIPT_DIR%CMakeFiles" rmdir /s /q "%SCRIPT_DIR%CMakeFiles"

mkdir "%BUILD_DIR%"

REM 检测 NDK 环境变量
if "%NDK%"=="" (
    if exist "%USERPROFILE%\android-ndk-r25c" (
        set NDK=%USERPROFILE%\android-ndk-r25c
    ) else if exist "%USERPROFILE%\android-ndk-r26b" (
        set NDK=%USERPROFILE%\android-ndk-r26b
    ) else if exist "C:\android-ndk" (
        set NDK=C:\android-ndk
    ) else (
        echo ERROR: NDK not found. Please set NDK environment variable.
        echo Example: set NDK=C:\android-ndk-r25c
        exit /b 1
    )
)

echo NDK path: %NDK%

if not exist "%NDK%" (
    echo ERROR: NDK directory not found: %NDK%
    exit /b 1
)

REM 设置工具链
set TOOLCHAIN=%NDK%\toolchains\llvm\prebuilt\windows-x86_64
if not exist "%TOOLCHAIN%" (
    echo ERROR: Toolchain not found: %TOOLCHAIN%
    exit /b 1
)

echo Toolchain path: %TOOLCHAIN%

REM 目标架构（可修改）
if "%TARGET_ARCH%"=="" set TARGET_ARCH=arm64-v8a
if "%API_LEVEL%"=="" set API_LEVEL=29

if "%TARGET_ARCH%"=="arm64-v8a" (
    set TARGET=aarch64-linux-android
) else if "%TARGET_ARCH%"=="armeabi-v7a" (
    set TARGET=armv7a-linux-androideabi
) else if "%TARGET_ARCH%"=="x86_64" (
    set TARGET=x86_64-linux-android
) else if "%TARGET_ARCH%"=="x86" (
    set TARGET=i686-linux-android
) else (
    echo ERROR: Unknown architecture: %TARGET_ARCH%
    exit /b 1
)

echo Target: %TARGET% (API %API_LEVEL%)

REM 设置编译器
set AR=%TOOLCHAIN%\bin\llvm-ar.exe
set CC=%TOOLCHAIN%\bin\%TARGET%%API_LEVEL%-clang.exe
set CXX=%TOOLCHAIN%\bin\%TARGET%%API_LEVEL%-clang++.exe
set LD=%TOOLCHAIN%\bin\ld.exe
set RANLIB=%TOOLCHAIN%\bin\llvm-ranlib.exe
set STRIP=%TOOLCHAIN%\bin\llvm-strip.exe

echo CXX: %CXX%

if not exist "%CXX%" (
    echo ERROR: C++ compiler not found: %CXX%
    exit /b 1
)

REM Copy CMakeLists for static build (not needed anymore, using option)
rem copy "%SCRIPT_DIR%CMakeLists_static.txt" "%BUILD_DIR%\CMakeLists.txt"

REM CMake 配置
echo.
echo Configuring CMake with static linking...
cmake -S "%SCRIPT_DIR%" -B "%BUILD_DIR%" -G "Visual Studio 17 2022" -A x64 ^
      -DCMAKE_BUILD_TYPE=Release ^
      -DBUILD_STATIC=ON

if %ERRORLEVEL% neq 0 exit /b %ERRORLEVEL%

REM 编译
echo.
echo Building...
cmake --build "%BUILD_DIR%" --config Release -j %NUMBER_OF_PROCESSORS%

if %ERRORLEVEL% neq 0 exit /b %ERRORLEVEL%

REM Check for binary
echo.
echo Build complete!
echo Search for binary in build directory...
dir "%BUILD_DIR%\" /s /b *.exe | findstr thread_scheduler_static.exe || dir "%BUILD_DIR%\" /b

endlocal
