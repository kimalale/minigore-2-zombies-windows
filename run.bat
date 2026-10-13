@echo off
setlocal

set PATH=C:\msys64\mingw32\bin;%PATH%

rem Rebuild whenever the executable is missing or any source file is newer
rem than it (e.g. after "git pull"), so the game you run is the code you have.
powershell -NoProfile -Command "$exe = Get-Item 'Minigore2_Windows.exe' -ErrorAction SilentlyContinue; if (-not $exe) { exit 1 }; $newer = Get-ChildItem native_port -Recurse -File -Include *.c,*.h | Where-Object { $_.LastWriteTime -gt $exe.LastWriteTime }; if ($newer) { exit 1 } else { exit 0 }"
if errorlevel 1 (
    echo Source code changed since the last build - rebuilding...
    call build.bat
    if errorlevel 1 (
        echo Build failed - not launching an out-of-date game.
        pause
        exit /b 1
    )
)

echo Launching Minigore 2: Zombies (Native Windows Port)...
Minigore2_Windows.exe %*
