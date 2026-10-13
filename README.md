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

6. **Gameplay Hooks (`native_port/hook_util.c`, `ai_hooks.c`, `gun_hooks.c`, `ui_nav.c`)**:
   - Patches the original binary's GOT and vtables, never its code, apart from one patched call in `ZombieStateFollow::update`. Through those hooks: smarter zombies, mouse aim, recoil, rebalanced guns, threat-aware aim assist and controller menu navigation.

7. **Multiplayer (`native_port/coop.c`, `net_host.c`, `guest.c`, `lockstep.c`, `determinism.c`)**:
   - Player 2 joins as a companion character driven by a remote player. It runs either streamed from the host or in lockstep, where both PCs run the game and exchange only inputs.
   - Lockstep relies on the game being deterministic. A virtual clock, pinned random seeds and a fixed date make it repeatable, and a record/replay test checks it.

8. **Diagnostics (`native_port/watchdog.c`, `log.c`)**:
   - A hang watchdog, a crash reporter and a non-blocking log writing to `game_log.txt`.

---

## Controls

| Action | Keyboard | Mouse | Xbox / XInput Controller |
| :--- | :--- | :--- | :--- |
| **Move** | `W` `A` `S` `D` | — | Left Stick / D-Pad |
| **Aim & Shoot** | Arrow Keys / `I` `J` `K` `L` | Hold Left Button (aims at cursor) | Right Stick |
| **Shoot ahead** | — | — | Right Trigger (fires the way you're moving; hold Left Trigger to strafe) |
| **Menu Cursor** | — | Move | D-Pad jumps between options; Left Stick moves a free cursor |
| **Menu Select / Tap** | — | Left Click (Right Click on in-game HUD) | A |
| **Action / Confirm** | `Space` / `Enter` | — | A (in a level) |
| **Back / Pause** | `Escape` | — | B / Start / View |
| **Fullscreen** | `F11` / `Alt+Enter` | — | — |

Controllers rumble on shots (scaled by the weapon), when you take a hit, and when you die. Sticks use a radial deadzone, so small tilts give fine movement without snapping to the axes.

In a level, keyboard, mouse aim and controller input are written straight into the game's stick state as exact vectors (the same path the engine's own gamepad support uses), rather than by emulating touches on the on-screen virtual sticks. `--touch-sticks` restores the old touch emulation.

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
- **Recoil kick:** every shot nudges the camera back along your aim, scaled by the weapon's punch (the shotgun kicks hard, the machine gun lightly). It stacks with the game's own screen shakes. (Off in own-world co-op.)
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

Play the campaign with a friend over the internet or your local network. No server or account needed: one player hosts, the other joins directly. There are two ways to play:

| | Own-world co-op (`--lockstep-host` / `--lockstep-join`) | Streamed co-op (`--host` / `--join`) |
| :--- | :--- | :--- |
| Player 2's PC | Runs the full game | Shows a video stream of the host's game |
| Picture and sound | Full resolution and frame rate, local sound | Compressed video (default 960 px, 30 fps) |
| Bandwidth | A few kB/s | 3-12 Mbit/s upload from the host |
| Player 2 needs | The same build and game files as the host | Only this program |
| Input delay | 67 ms for both players (adjustable) | Player 2's round trip to the host |

Own-world co-op is the better experience when both players have the game; streamed co-op works when player 2's PC can't run it. Both use TCP port 27015 and the same `--code`.

### Streamed Co-op

**Host (player 1):**
```cmd
run.bat --host
```
**Friend (player 2)** joins with the host's IP address:
```cmd
run.bat --join 203.0.113.7
```

- Player 2 drops in as an extra character beside you, with their own gun and health, once you start a level. If they go down they respawn after a few seconds.
- Each player has their own screen: player 2's view is rendered separately on the host and streamed, so you can split up and roam the map independently. Zombies chase whichever of you is closer. Player 2's screen shows their own health bar. (`--coop-view shared` instead uses one camera that keeps both of you in view.)
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
| `--coop-view <split\|shared>` | `split` | Own screen per player, or one shared camera |

At the defaults the stream needs roughly 8-12 Mbit/s of upload from the host; `--coop-width 640 --coop-quality 45` brings that to about 3-4 Mbit/s.

### Own-World Co-op (each player runs the game)

Instead of streaming video, both PCs run the full game and only controller input is sent (a few hundred bytes per frame). Each player gets full resolution, full frame rate and their own sound, with no compression artefacts, and it needs almost no bandwidth.

**Host (player 1):**
```cmd
run.bat --lockstep-host
```
**Friend (player 2)**, with the host's IP address:
```cmd
run.bat --lockstep-join 203.0.113.7
```

- The host waits for player 2 before the game starts, then sends its saves and settings so both games start identically. Player 2's own saves are not touched; their copy lives in `saves_lockstep\`.
- Once a level starts, player 2 appears as an extra character in **both** worlds. The host sees it beside them; player 2's screen follows player 2. Both screens show both players' health bars (`P1` / `P2`, with `YOU` on yours) and a marker over your partner, or an arrow at the screen edge pointing to them when they're off-screen.
- The host drives the menus (player 2 sees them too). Each player's input takes effect after a short delay of 4 frames (67 ms) on both PCs, which hides the network delay. On a slow or distant connection, raise it with `--lockstep-delay 6` (host). If a game hitches, the other waits for it.
- Both games compare a fingerprint of their worlds every second. If they ever differ, `OUT OF SYNC` appears on screen and `game_log.txt` says what differed. Please report it.
- Both PCs need the same build and game files (checked when connecting). Player 2's window takes the host's window size, and the window can't be resized during the session. If either player quits, the other carries on alone.
- Uses the same port (TCP 27015) and `--code` as streamed co-op. See *Connecting over the internet* above.

| Option | Default | Description |
| :--- | :--- | :--- |
| `--lockstep-host [port]` | `27015` | Host; waits for player 2 before the game starts |
| `--lockstep-join <ip[:port]>` | — | Join a host as player 2 |
| `--lockstep-delay <1-30>` | `4` | Host: input delay in frames (60 per second); raise it if the game stutters on a slow connection |
| `--code <text>` | — | Join code; set the same on both sides |

Camera recoil is switched off in this mode, because it would move the host's camera differently on the two PCs.

---

## Determinism Test

Own-world co-op only works if the game produces exactly the same state from the same inputs. The port makes it do so:
- a virtual clock advances exactly 1/60 s per frame;
- the date is fixed;
- the game's own random seed (normally taken from the CPU clock at startup) is pinned;
- the smart AI's per-zombie choices don't depend on memory addresses.

This test checks it:

1. `run.bat --record test.rec` - play a level normally for a minute or two (don't resize the window), then quit. Inputs and a fingerprint of the game state for every frame are saved, with a snapshot of your saves in `test.rec.saves`.
2. `run.bat --replay test.rec` - the recording plays back with no input from you. When it finishes, `game_log.txt` reports either `RESULT: IN SYNC` or the first frame where the state differed and what differed (characters, other entities, random numbers).

Both runs play from a scratch copy of the save snapshot (`saves_determinism/`), so your real saves are never touched. Co-op is disabled while testing.

---

## Troubleshooting Freezes and Crashes

A watchdog thread monitors presented frames. If the game stops drawing while the window is focused (for 10 seconds during play, or 60 seconds while starting up, when the graphics driver may be compiling shaders), it writes `hang_report.txt` (also printed to the console) with:
- live host mutex / condition variable counts and the input queue depth
- every game thread's registers and a stack scan symbolized against `libminigore2.so` (mangled C++ names; pipe through `c++filt` to demangle)

If the game crashes, `crash_report.txt` records the exception, where it happened and the registers. Everything the game prints is also saved to `game_log.txt`, even if the window closes. Please attach these files when reporting a problem.

---

## Game Assets & Data
- `assets/`: Game shaders, font atlases, models, textures, patterns, sounds, and speech files.
- `lib/x86/`: Original compiled Android x86 ELF library (`libminigore2.so`).
- `saves/`: In-game save files and local progress are saved here.
- `saves_lockstep/`: Own-world co-op player 2's copy of the host's saves (replaced each session).
- `saves_determinism/`, `<file>.rec.saves/`: Scratch saves and the snapshot used by the determinism test.
- `game_log.txt`, `hang_report.txt`, `crash_report.txt`: Diagnostics (see *Troubleshooting*).
