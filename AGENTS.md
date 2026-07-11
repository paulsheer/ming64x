# CLAUDE.md

ming64x is an ultra-low-latency X11 server for Windows, built on Linux via MinGW-w64 cross-compilation. It is forked from VcXsrv (the X.Org X server ported to Windows). The build output is a native Windows binary (`ming64x.exe`) plus an `xkbcomp.exe` helper.

## Build / run

- **Builds natively on Linux** — no Docker, no WSL, no Cygwin required. A standard ext4 checkout works fine.
- Toolchain: `x86_64-w64-mingw32-gcc` (MinGW-w64 cross-compiler), `flex`, `bison`, `python3` (with lxml, for GL/WGL wrapper generation).
- Pre-built dependencies live under `../dep-ming64x/prefix/` (set via `DEP_PREFIX`). Five header-only sub-repos are also required under `../dep-ming64x/`: `libxcb-util`, `libxcb-wm`, `libxtrans`, `xcb-proto`, `xorgproto`.
- Build command:
  ```
  cd xorg-server && make -f Makefile.mingw64 DEP_PREFIX=/path/to/dep-ming64x/prefix -j$(nproc)
  ```
  Add `NOOPT=1` for a debug build (`-O0 -ggdb` instead of `-O2 -ggdb`).
- Output: `xorg-server/build-mingw64/ming64x.exe` (X server) and `xorg-server/build-mingw64/xkbcomp.exe` (XKB compiler).
- Quick smoke test (see `start-ming64x.sh`):
  ```bash
  make -f Makefile.mingw64 -j 24
  ls -la build-mingw64/ming64x.exe
  ```
  Copy `ming64x.exe` and `xkbcomp.exe` to the Windows target machine alongside `install/ming64x/` (fonts, xkb data, config).
- Run on Windows: `ming64x :0 -listen tcp -ac`
- The repo also carries a legacy MSVC/mhmake build system (`buildall.sh`, `setenv.sh`, `tools/mhmake/`) and a Docker-based Windows build environment (`dockerBuild.cmd`). These are not used for the MinGW cross-build.

## Tests / lint

- No test suite, linter, or CI config exists. `buildall.sh` runs no tests.
- No enforced formatter. Match the surrounding X.Org style in the file you edit.

## Three build systems (read before touching build files)

- **`Makefile.mingw64`** — the active build system. A hand-written GNU makefile that compiles all X server sources in one shot. Adding or removing a source file means editing the source lists in this makefile (e.g. `WINCLIP_SRCS`, `XWIN_SRCS`, `GLX_SRCS`).
- **mhmake** (legacy MSVC) — per-directory `makefile` files in a custom syntax. Shared flags in `makefile.before` / `makefile.after`. Only the mhmake build produces the NSIS installer.
- **meson.build** — upstream build system files. Not invoked by any build script in this repo; they exist only for upstream parity. Updating them is optional and does not affect either Windows build.

When adding a new C source file, update `Makefile.mingw64` (and optionally the mhmake `makefile` and `meson.build`).

## Layout

- `xorg-server/` — the X.Org server. ming64x-specific code lives in `xorg-server/hw/xwin/` (the "xwin" DDX). Sibling `hw/` backends (xfree86, xwayland, xquartz, vfb, ...) are upstream and not built.
- `xorg-server/hw/xwin/winclipboard/` — clipboard bridge, runs in its own thread: `thread.c` setup, `xevents.c` X side, `wndproc.c` Win32 side, `textconv.c` / `imageconv.c` format conversion; `../winclipboardinit.c` and `../winclipboardwrappers.c` are lifecycle glue.
- `xorg-server/hw/xwin/glx/` — WGL-based OpenGL provider for the GLX extension. Python scripts `gen_gl_wrappers.py` generate GL/WGL dispatch wrappers at build time.
- `xorg-server/xkbcomp-support/` — stubs and support files so xkbcomp can be linked as a standalone exe.
- `pthreadlight/` — minimal pthreads-on-Windows replacement using native Win32 primitives (critical sections, condition variables, SRW locks). Replaces the pthreads4w library.
- `lib*/` (libX11, libxcb, libXext, libXfixes, ...) — vendored X client and protocol libraries. Headers are used at build time; the MinGW build links pre-built `.a` libraries from `DEP_PREFIX`.
- `mesalib/ freetype/ fontconfig/ openssl/ zlib/ pixman/ expat/ libxml2/` — vendored third-party deps. The MinGW build links pre-built versions from `DEP_PREFIX` rather than building these from source.
- `xkbcomp/` — XKB keymap compiler, built as a separate `.exe` alongside the server.
- `install/ming64x/` — runtime data (fonts, xkb data) to bundle alongside the exe on the Windows target.
- `apps/` — bundled X clients (xcalc, xclock, xhost, ...). `tools/` — build tooling (mhmake, plink). `docker/` — Dockerfile for the legacy MSVC build container.

## Architecture (xwin DDX)

- A standard X.Org server with a Windows DDX. Screen init `winscrinit.c`, main window proc `winwndproc.c`, input `winkeybd.c` / `winmouse.c`, drawing via a shadow framebuffer blitted into a Windows window.
- Multi-window mode: an integrated window manager (`winmultiwindowwm.c` + `winmultiwindowwndproc.c`) maps each X top-level window to a native Windows window.
- Clipboard runs in a dedicated thread bridging X selections and the Win32 clipboard.
- OpenGL: the GLX extension uses Windows WGL (`hw/xwin/glx/`) with Mesa's swrast driver compiled into the server. Generated GL/WGL wrappers are created at build time by Python scripts parsing `gl.xml` / `wgl.xml`.

## Conventions and gotchas

- Branches: `coolx11` is the active development branch. `master` / `released` track upstream VcXsrv releases.
- **pthreadlight**: this repo uses `pthreadlight/` (native Win32 sync primitives) instead of pthreads4w. Thread/sync code should use pthreadlight APIs or direct Win32 primitives, not pthreads.
- The `DEP_PREFIX` directory (`../dep-ming64x/prefix/`) must contain pre-built static libraries (`.a` files) and headers for all vendored deps. The makefile will error with a clear message if the dependency repos are missing.
- Patched headers: `build-mingw64/include/` contains MinGW-fixed versions of headers that are MSVC-specific in the source tree (removing `__declspec`, fixing typedefs, etc.). These are auto-generated by the makefile.
- Line endings are mixed per file; preserve each file's existing endings.
- Build artifacts land in `build-mingw64/` (MinGW) or `obj*/Debug/Release/` (MSVC).
- Upstream X.Org contribution guide: `xorg-server/CONTRIBUTING` (mailing-list / merge-request oriented; not necessarily this project's process).