@echo off
REM ============================================================
REM  Image Watermark Tool - build script
REM
REM  Requirements: a GCC toolchain only (TDM-GCC or MinGW-w64).
REM                No third-party libraries needed.
REM
REM  Compiler lookup order:
REM    1. Environment variables GCC / WINDRES (full path allowed)
REM    2. gcc / windres found on PATH
REM
REM  Usage:
REM    build.bat
REM    set GCC=D:\path\to\gcc.exe
REM    build.bat
REM
REM  NOTE: kept ASCII-only on purpose - cmd.exe reads .bat files
REM        using the OEM codepage, so non-ASCII text can corrupt
REM        the script on some systems.
REM ============================================================
setlocal

if not defined GCC     set "GCC=gcc"
if not defined WINDRES set "WINDRES=windres"

echo [1/2] Compiling icon resource...
"%WINDRES%" -i resource.rc -o resource.o
if errorlevel 1 goto resfail

echo [2/2] Compiling watermark.exe...
"%GCC%" -O2 -mwindows -municode -Wall -o watermark.exe watermark.c resource.o ^
    -lcomctl32 -lgdi32 -lgdiplus -lole32 -loleaut32 -luuid -lcomdlg32 -lshlwapi -lm
if errorlevel 1 goto ccfail

echo.
echo Build OK: watermark.exe
endlocal
exit /b 0

:resfail
echo.
echo [ERROR] Failed to compile the icon resource.
echo         Make sure windres is available: set WINDRES or add it to PATH.
endlocal
exit /b 1

:ccfail
echo.
echo [ERROR] Compilation failed.
echo         Make sure gcc is available: set GCC or add it to PATH.
endlocal
exit /b 1
