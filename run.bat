@echo off
setlocal

set PATH=C:\msys64\mingw32\bin;%PATH%

if not exist Minigore2_Windows.exe (
    echo Executable Minigore2_Windows.exe not found! Building first...
    call build.bat
    if errorlevel 1 exit /b 1
)

echo Launching Minigore 2: Zombies (Native Windows Port)...
Minigore2_Windows.exe %*
