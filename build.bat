@echo off
setlocal enabledelayedexpansion

echo =========================================================
echo   Compiling Minigore 2: Zombies Native Windows Port
echo =========================================================

set PATH=C:\msys64\mingw32\bin;%PATH%

rem Stamp the build with the git commit so logs show which code is running
set BUILD_ID=unknown
for /f %%i in ('git rev-parse --short HEAD 2^>nul') do set BUILD_ID=%%i
git diff --quiet 2>nul || set BUILD_ID=%BUILD_ID%+local

set CFLAGS=-O2 -Wall -Wno-unused-function -Wno-unused-variable -I native_port -I C:\msys64\mingw32\include -DBUILD_ID=\"%BUILD_ID%\"
set LDFLAGS=-L C:\msys64\mingw32\lib -Wl,--stack=16777216 -lmingw32 -lSDL2main -lSDL2 -lopengl32 -lws2_32 -lwinmm -lm

echo Compiling native host sources...
gcc %CFLAGS% ^
    native_port\main.c ^
    native_port\elf_loader.c ^
    native_port\gl_wrap.c ^
    native_port\egl_wrap.c ^
    native_port\android_ndk.c ^
    native_port\opensles.c ^
    native_port\shims.c ^
    native_port\watchdog.c ^
    native_port\log.c ^
    native_port\java_stubs.c ^
    native_port\ai_core.c ^
    native_port\ai_hooks.c ^
    native_port\hook_util.c ^
    native_port\gun_hooks.c ^
    native_port\net_proto.c ^
    native_port\net_host.c ^
    native_port\coop.c ^
    native_port\guest.c ^
    native_port\stb_impl.c ^
    %LDFLAGS% ^
    -o Minigore2_Windows.exe

if %ERRORLEVEL% equ 0 (
    echo.
    echo Build SUCCESSFUL: Minigore2_Windows.exe
    echo.
) else (
    echo.
    echo Build FAILED with code %ERRORLEVEL%!
    echo.
    exit /b %ERRORLEVEL%
)
