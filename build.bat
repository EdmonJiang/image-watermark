@echo off
setlocal
set GCC=D:\SOFT\Embarcadero_Dev-Cpp_6.3_TDM-GCC_9.2_Portable\TDM-GCC-64\bin\gcc.exe
set WINDRES=D:\SOFT\Embarcadero_Dev-Cpp_6.3_TDM-GCC_9.2_Portable\TDM-GCC-64\bin\windres.exe

"%WINDRES%" -i resource.rc -o resource.o
if errorlevel 1 (echo Resource compile failed & exit /b 1)

"%GCC%" -O2 -mwindows -municode -Wall -o watermark.exe watermark.c resource.o -lcomctl32 -lgdi32 -lgdiplus -lole32 -loleaut32 -luuid -lcomdlg32 -lshlwapi -lm
if errorlevel 1 (echo Compile failed & exit /b 1)

echo Build OK: watermark.exe
endlocal
