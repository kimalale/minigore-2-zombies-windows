# Minigore 2: Zombies - Native Windows Port

Native Windows WOW64 port of **Minigore 2: Zombies** (Mountain Sheep) executing the authentic Android x86 ELF binary (`lib/x86/libminigore2.so`) directly on desktop Windows without emulators, virtual machines, or engine recreation.

---

## Technical Architecture

Instead of rewriting or recreating the game in Unity or another engine, this project implements a native in-process Windows host environment that executes the original Mountain Sheep engine directly:

1. **In-Process 32-bit ELF Loader & Relocator (`native_port/elf_loader.c`)**:
   - Maps ELF segments (`LOAD`) into committed WOW64 virtual memory with precise permissions (`PAGE_EXECUTE_READWRITE`, `PAGE_READONLY`, etc.).
   - Resolves all 10,288+ symbol relocations (`R_386_32`, `R_386_PC32`, `R_386_GLOB_DAT`, `R_386_JUMP_SLOT`, `R_386_RELATIVE`).
   - Invokes 159 `init_array` static constructors.

2. **Bionic Libc & Pthread Compatibility Layer (`native_port/shims.c`)**:
   - Complete Bionic C runtime emulation: `_ctype_`, `_tolower_tab_`, `_toupper_tab_`, `__sF`, math, formatting, dynamic linking.
   - Robust `pthread_mutex` pool preventing Bionic 4-byte `pthread_mutex_t` from clobbering neighboring C++ class fields (such as `xt::EventQueue::s_deque`).
   - Thread stack commit and SEH on-demand commit handler to resolve Linux GCC `sub esp, 0x4980` stack probe guard page exceptions.

3. **Android NDK Host Layer (`native_port/android_ndk.c`)**:
   - High-performance asset streaming (`AAssetManager`, `AAsset`, `AAssetDir`) loading uncompressed textures, fonts, sounds, models, and JSON patterns.
   - Non-blocking event looper (`ALooper_pollAll`, `AInputQueue`) dispatching lifecycle commands (`APP_CMD_START`, `APP_CMD_INIT_WINDOW`, `APP_CMD_RESUME`, `APP_CMD_GAINED_FOCUS`).

4. **Desktop OpenGL / EGL Translation Layer (`native_port/egl_wrap.c`, `native_port/gl_wrap.c`)**:
   - Desktop OpenGL 2.1+ / 4.6 context emulation for EGL 1.4 (`eglCreateContext`, `eglCreateWindowSurface`, `eglMakeCurrent`, `eglSwapBuffers`).
   - Translates all GLES2 shader compilation and rendering pipelines seamlessly to desktop GPUs.

5. **OpenSL ES Audio Host (`native_port/opensles.c`)**:
   - Full OpenSL ES engine (`SLEngineItf`, `SLPlayItf`, `SLBufferQueueItf`, `SLVolumeItf`, `SLSeekItf`, `SLPlaybackRateItf`, `SLEffectSendItf`).
   - Streams in-game music, sound effects, and voice lines through a low-latency SDL2 circular audio buffer.

---

## Controls

| Action | Keyboard | Mouse | Gamepad |
| :--- | :--- | :--- | :--- |
| **Move** | `W` `A` `S` `D` | — | Left Analog Stick |
| **Aim & Shoot** | Arrow Keys / `I` `J` `K` `L` | Left Click & Drag | Right Analog Stick |
| **Menu Selection** | — | Left Click | A Button / D-Pad |
| **Action / Confirm** | `Space` / `Enter` | Left Click | A Button |
| **Pause / Back** | `Escape` | — | B Button / Start |

---

## Prerequisites & Building

- **Toolchain**: MinGW-w64 32-bit GCC (`C:\msys64\mingw32\bin\gcc.exe`).
- **Dependencies**: Included `SDL2.dll` (x86) and standard Windows libraries (`opengl32`, `gdi32`, `user32`, `kernel32`).

### Build:
Double-click or run:
```cmd
build.bat
```

### Run:
```cmd
run.bat
```
or run `Minigore2_Windows.exe` directly.

---

## Game Assets & Data
- `assets/`: Game shaders, font atlases, models, textures, patterns, sounds, and speech files.
- `lib/x86/`: Original compiled Android x86 ELF library (`libminigore2.so`).
- `saves/`: In-game save files and local progress are saved here.
