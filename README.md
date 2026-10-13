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

| Action | Keyboard | Mouse | Xbox / XInput Controller |
| :--- | :--- | :--- | :--- |
| **Move** | `W` `A` `S` `D` | — | Left Stick / D-Pad |
| **Aim & Shoot** | Arrow Keys / `I` `J` `K` `L` | Hold Left Button (aims at cursor) | Right Stick |
| **Shoot ahead** | — | — | Right Trigger (fires the way you're moving; hold Left Trigger to strafe) |
| **Menu Cursor** | — | Move | Left Stick / D-Pad moves an on-screen cursor |
| **Menu Select / Tap** | — | Left Click (Right Click on in-game HUD) | A |
| **Action / Confirm** | `Space` / `Enter` | — | A (in a level) |
| **Back / Pause** | `Escape` | — | B / Start / View |
| **Fullscreen** | `F11` / `Alt+Enter` | — | — |

Controllers rumble on shots (scaled by the weapon), when you take a hit, and when you die. Sticks use a radial deadzone, so small tilts give fine movement without snapping to the axes.

--- | :--- | :--- | :--- |
| **Move** | `W` `A` `S` `D` | — | Left Analog Stick |
| **Aim & Shoot** | Arrow Keys / `I` `J` `K` `L` | Hold Left Button (aims at cursor) | Right Analog Stick |
| **Menu Selection** | — | Left Click (Right Click on in-game HUD) | A Button / D-Pad |
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

## Guns

- **Mouse aiming:** during a level, hold the **left mouse button** to aim at the cursor and fire, like a PC twin-stick shooter. Use the **right mouse button** to tap on-screen HUD buttons. In menus the left button clicks as before.
- **Recoil kick:** every shot nudges the camera back along your aim, scaled by the weapon's punch (the shotgun kicks hard, the machine gun lightly). It stacks with the game's own screen shakes.
- **Rebalanced:** your guns deal 20% more damage and ammo pickups hold 30% more rounds, to keep pace with the smarter horde.
- **Smarter aim assist:** with the game's *Auto Aim* option switched on, the assist picks the most dangerous target near where you're aiming instead of just the nearest enemy: zombies about to reach you, throwers bombing you, and bosses. It reaches far enough to answer throwers holding back at range, and it never swings your aim more than 40° away from where you're pointing.

| Option | Default | Description |
| :--- | :--- | :--- |
| `--no-mouse-aim` | — | Left mouse button acts as a touch, as before |
| `--gun-kick <0-2>` | `1` | Camera recoil strength (`0` disables) |
| `--gun-damage <0.5-3>` | `1.2` | Player gun damage multiplier |
| `--gun-ammo <0.5-3>` | `1.3` | Rounds per ammo pickup multiplier |
| `--no-smart-aim` | — | Original aim assist (nearest enemy) |

These hook the original binary through its GOT and vtables (`native_port/gun_hooks.c`, `native_port/hook_util.c`); no game code is patched.

---

## Online Co-op (Peer to Peer)

Play the campaign with a friend over the internet or your local network. No server or account needed: one player hosts, the other joins directly.

**Host (player 1):**
```cmd
run.bat --host
```
**Friend (player 2)** joins with the host's IP address:
```cmd
run.bat --join 203.0.113.7
```

- Player 2 drops in as an extra character beside you, with their own gun and health, once you start a level. If they go down they respawn after a few seconds.
- The camera keeps both of you in view, pulling back as you spread out.
- Player 2 sees your game streamed to their window (video and sound) and plays with keyboard, mouse (hold the left button to aim) or an Xbox controller. The host drives the menus.
- If the connection drops, the guest reconnects on its own, and player 2's character falls back to the game's companion AI in the meantime.

**Connecting over the internet:** the host must forward **TCP port 27015** on their router to their PC, or both players can join the same virtual LAN with Tailscale, ZeroTier or Radmin VPN and use that address. On the same home network, use the LAN address the host's console prints.

| Option | Default | Description |
| :--- | :--- | :--- |
| `--host [port]` | `27015` | Host a game |
| `--join <ip[:port]>` | — | Join a host as player 2 |
| `--code <text>` | — | Join code; set the same on both sides to keep strangers out |
| `--coop-width <px>` | `960` | Streamed video width |
| `--coop-fps <n>` | `30` | Streamed frame rate |
| `--coop-quality <10-95>` | `60` | JPEG quality; lower it (and the width) on slow uploads |

At the defaults the stream needs roughly 8-12 Mbit/s of upload from the host; `--coop-width 640 --coop-quality 45` brings that to about 3-4 Mbit/s.

---

## Troubleshooting Freezes and Crashes

A watchdog thread monitors presented frames. If the game stops drawing while the window is focused (for 10 seconds during play, or 60 seconds while starting up, when the graphics driver may be compiling shaders), it writes `hang_report.txt` (also printed to the console) with:
- live host mutex / condition variable counts and the input queue depth
- every game thread's registers and a stack scan symbolized against `libminigore2.so` (mangled C++ names; pipe through `c++filt` to demangle)

If the game crashes, `crash_report.txt` records the exception, where it happened and the registers. Please attach whichever file appears when reporting a problem.

---

## Game Assets & Data
- `assets/`: Game shaders, font atlases, models, textures, patterns, sounds, and speech files.
- `lib/x86/`: Original compiled Android x86 ELF library (`libminigore2.so`).
- `saves/`: In-game save files and local progress are saved here.
