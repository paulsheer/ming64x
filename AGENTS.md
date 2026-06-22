# CLAUDE.md

VcXsrv is an X11 server for Windows (the X.Org X server ported to MSVC), comparable to Xming or Cygwin/X. The build output is a native Windows binary (`vcxsrv.exe`) plus helper tools and an NSIS installer.

## Build / run

- Build only on a case-insensitive filesystem (a Windows drive) from a WSL or Cygwin bash shell. A pure Linux (ext4) checkout will not build, due to header filename case collisions. Authoritative steps: `HOW_TO_BUILD.txt`.
- Toolchain: Visual Studio 2022 Community, Strawberry Perl at `c:\perl`, Cygwin (bison, flex, gawk, gperf, nasm, sed), Python 3.9 (lxml, mako), NSIS for the installer. `MSBuild.exe`, `nasm.exe`, `perl.exe`, `python.exe` must be on PATH. (`README.md` says VS2012; it is stale, trust `HOW_TO_BUILD.txt`.)
- Set up the Windows/VS environment first: `source ./setenv.sh 1` (64-bit) or `source ./setenv.sh 0` (32-bit). Exports `MHMAKECONF` (= repo root), `PYTHON3`, `IS64`, `CFLAGS=-FS`; prepends VS, nasm, perl, and mhmake to PATH.
- Full build (dependencies + server + installer):
  `./buildall.sh <1|0> <jobs> <D|R|A> [N]`
  - arg1: `1` = 64-bit, `0` = 32-bit
  - arg2: parallel jobs (cpu count + 1)
  - arg3: `D` = Debug, `R` = Release, `A` = both
  - arg4 (optional): `N` = build only the server, skip dependencies
  - example: `./buildall.sh 1 9 D`
- Rebuild just the server (after deps are built and `setenv.sh` is sourced):
  `tools/mhmake/Release64/mhmake.exe -P<jobs> -C xorg-server MAKESERVER=1 [DEBUG=1]`
  (use `tools/mhmake/Release` for 32-bit; `MAKESERVER=1` is mandatory or `xorg-server/makefile` errors out.)
- Docker build env (no local toolchain): `dockerBuild.cmd` builds the `vcxb` image (about 2 hours), `runDocker.cmd` mounts the repo at `c:\src`; inside, follow `HOW_TO_BUILD.txt` (clone, `cygwin`, `export SHELLOPTS; set -o igncr`, then `buildall`).
- Run: launch the built `vcxsrv.exe`, or use the XLaunch wizard (built from `hw/xwin/xlaunch`) to generate a launch config.

## Tests / lint

- No VcXsrv test suite, linter, or CI config exists in this tree; `buildall.sh` runs no tests. Upstream `xorg-server` tests are not wired into the mhmake build. (Verified: no CI files, no test or lint target found.)
- No enforced formatter for the server code (no `.clang-format` at the root or in `xorg-server/`; only some vendored deps ship one). Match the surrounding X.Org style in the file you edit.

## Two build systems (read before touching build files)

- The repo carries BOTH the mhmake/MSVC build and upstream `meson.build` files. Only the mhmake build produces the Windows server and releases; meson is never invoked by any VcXsrv build script (verified).
- mhmake makefiles are per-directory `makefile` files in a custom (not GNU make) syntax (`load_makefile`, `$(OBJDIR)`, helper functions). Shared flags and defines live in `makefile.before` / `makefile.after`, included via `$(MHMAKECONF)`. `MAKESERVER=1` adds the `XKB_IN_SERVER XFree86Server HAVE_DIX_CONFIG_H` defines.
- Adding or removing a server source file means editing the relevant mhmake `makefile` (for example its `CSRCS` list), plus the static lib entry and `LINKLIBS` in `xorg-server/makefile`. Updating `meson.build` is optional and only for upstream parity; it does not affect the Windows build.

## Layout

- `xorg-server/` the X.Org server. VcXsrv-specific code lives in `xorg-server/hw/xwin/` (the "xwin" DDX). Sibling `hw/` backends (xfree86, xwayland, xquartz, vfb, ...) are upstream and not built for Windows.
- `xorg-server/hw/xwin/winclipboard/` clipboard bridge, runs in its own thread: `thread.c` setup, `xevents.c` X side, `wndproc.c` Win32 side, `textconv.c` / `imageconv.c` format conversion; `../winclipboardinit.c` and `../winclipboardwrappers.c` are lifecycle glue.
- `xorg-server/hw/xwin/xlaunch/` C++ XLaunch config GUI (separate exe). `libwinmain/winmain.c` the WinMain wrapper.
- `lib*/` (libX11, libxcb, libXext, libXfixes, ...) vendored X client and protocol libraries.
- `mesalib/ freetype/ fontconfig/ openssl/ zlib/ pixman/ pthreads/ expat/ libxml2/ dxtn/` vendored third-party deps.
- `X11/` duplicated copies of public headers that also live under the `lib*/include/X11/` dirs; the MSVC build includes from here (see gotcha below).
- `apps/` bundled X clients (xcalc, xclock, xhost, ...). `tools/` build tooling (`mhmake`, `plink`). `docker/` build image. `releasenotes/` per-version notes (current line: 21.1.16.x).

## Architecture (xwin DDX)

- A standard X.Org server with a Windows DDX. Screen init `winscrinit.c`, main window proc `winwndproc.c`, input `winkeybd.c` / `winmouse.c`, drawing via a shadow framebuffer blitted into a Windows window.
- Multi-window mode: an integrated window manager (`winmultiwindowwm.c` + `winmultiwindowwndproc.c`) maps each X top-level window to a native Windows window.
- Clipboard runs in a dedicated thread bridging X selections and the Win32 clipboard (see the winclipboard files above).

## Conventions and gotchas

- Branches: `released` holds pristine upstream sources; `master` holds the changes needed to compile with VS, and releases are built from `master`. Base new work on `master`. (`README.md`)
- Duplicated headers: when you change a public header under a `lib*/include/X11/` dir, update its twin under top-level `X11/`. `filesthatshouldbethesame.py` lists the pairs and checks they stay byte-identical (appears to be run manually, not wired into the build).
- Line endings are mixed per file (some CRLF, some LF); preserve each file's existing endings, do not mass-convert. In Cygwin/Docker, `export SHELLOPTS; set -o igncr` is required so CRLF scripts run.
- `python.exe` specifically must resolve (buildall warns you may need to copy e.g. `python2.7.exe` to `python.exe`). Perl must be at `c:\perl`.
- Build artifacts land in `obj*`, `Debug`, `Release` dirs (gitignored); `OBJDIR` is derived from `IS64` / `DEBUG` / `MAKESERVER` in `makefile.before`.
- Upstream X.Org contribution guide: `xorg-server/CONTRIBUTING` (mailing-list / merge-request oriented; not necessarily VcXsrv's own process).