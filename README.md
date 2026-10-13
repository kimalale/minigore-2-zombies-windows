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

## Video Options

The game renders at the window's real pixel resolution, so a larger window or fullscreen gives a sharper image rather than a stretched one.

| Option | Default | Description |
| :--- | :--- | :--- |
| `--width <px>` / `--height <px>` | `1280` / `720` | Initial window size |
| `--fullscreen` | off | Borderless fullscreen at desktop resolution |
| `--msaa <n>` | `4` | Multisample anti-aliasing samples (`0` disables) |
| `--aniso <n>` | `16` | Anisotropic filtering on mipmapped textures (clamped to the GPU maximum) |

Press `F11` or `Alt+Enter` in game to toggle fullscreen. Options can also be passed to `run.bat`:
```cmd
run.bat --fullscreen --msaa 8
```

---

## Smart Zombie AI

By default zombies use a new pursuit AI instead of running straight at you in a line:

- **Encircling:** most of the horde spreads out into evenly spaced slots around you and closes in from every side, so there's no single gap to escape through.
- **Interception:** zombies aim where you are heading, not where you were, which makes kiting in circles much less effective.
- **Spreading out:** zombies keep a little distance from each other instead of stacking up into a conga line.
- A share of zombies still charge straight in, and within a few body-lengths every zombie reverts to the original chase, so lunges and attacks work exactly as before.
- **Throwers** (bomb and poison) act like artillery: they hold position near the edge of their throw range instead of walking into your guns, spread out around you in a slowly circling formation, and back off when you rush them.
- **Bosses** charge straight at you, leading your movement, and the horde gets out of their way. While a boss is closing in, the other zombies gather on the far side of you, so backing away from the boss means backing into the horde.
- Giants keep their original circling attack.

| Option | Default | Description |
| :--- | :--- | :--- |
| `--ai <smart\|classic>` | `smart` | `classic` restores the original behaviour |
| `--ai-encircle <0-1>` | `0.75` | Share of zombies that surround you (the rest charge straight in) |
| `--ai-lead <0-1>` | `0.8` | How far ahead of a moving player zombies aim |
| `--ai-spread <0-2>` | `0.4` | How strongly zombies spread out from each other |
| `--ai-no-throwers` | — | Throwers walk straight in as before |
| `--ai-no-bosses` | — | Bosses and the horde don't coordinate |

The AI hooks into `ZombieStateFollow::update` in the original binary (`native_port/ai_hooks.c`); the steering itself lives in `native_port/ai_core.c`.

---

## Troubleshooting Freezes

A watchdog thread monitors presented frames. If the game stops drawing for 6 seconds while the window is focused, it writes `hang_report.txt` (also printed to the console) with:
- live host mutex / condition variable counts and the input queue depth
- every game thread's registers and a stack scan symbolized against `libminigore2.so` (mangled C++ names; pipe through `c++filt` to demangle)

Please attach `hang_report.txt` when reporting a freeze. A report can also appear during an unusually long level load; that is harmless.

---

## Game Assets & Data
- `assets/`: Game shaders, font atlases, models, textures, patterns, sounds, and speech files.
- `lib/x86/`: Original compiled Android x86 ELF library (`libminigore2.so`).
- `saves/`: In-game save files and local progress are saved here.
