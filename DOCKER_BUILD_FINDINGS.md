# Docker Build Findings And Full Build Process

This document captures the Docker-based VcXsrv build process that worked in this repo, plus the failures we hit and how we fixed or worked around them.

It is written for the Windows container flow used in this checkout, not for a native local Visual Studio/Cygwin build.

## What finally worked

We successfully produced:

- `vcxsrv.exe`
- a full 64-bit installer
- a corrected installer that includes complete `xkbdata` keyboard data

Final working installer artifact:

- `build-output\vcxsrv-64.21.1.16.1.installer.exe`

## Root cause summary

The first "successful" build was only partially successful:

- `vcxsrv.exe` linked correctly
- the installer was created
- but the installed app crashed on startup with:
  - `Failed to activate virtual core keyboard: 2`

The actual runtime problem was missing XKB keyboard data:

- `C:\Program Files\VcXsrv\xkbdata\rules\xorg` was missing
- VcXsrv failed keyboard initialization and exited

That missing runtime data came from an incomplete `xkeyboard-config` build inside the Docker container.

## Full Docker build flow

### 1. Build the base Docker image

From the repo root:

```powershell
docker build -t vcxb -f docker/Dockerfile docker
```

Notes:

- Docker must be in Windows Containers mode.
- The Docker daemon storage size needs to be large enough. The checked-in Dockerfile notes `120GB`.
- The image build is slow and can look stalled for long stretches.

### 2. Start a container with the repo mounted

The checked-in runner is:

```cmd
runDocker.cmd
```

It currently runs:

```cmd
docker run -m 4G -v<repo-path>:c:\src -it vcxb
```

Inside the container, the working source tree used during our build was:

```cmd
git clone C:\src C:\vcx
```

### 3. Use Cygwin bash and enable CRLF-safe shell behavior

Inside the container:

```bash
export SHELLOPTS
set -o igncr
cd /cygdrive/c/vcx
source ./setenv.sh 1
```

Important:

- `setenv.sh 1` is required for the 64-bit build environment.
- Running from a Windows-backed case-insensitive filesystem is required.

### 4. Build dependencies and server

The repo documents `./buildall.sh 1 <jobs> R`, but the Docker image needed several fixes before that path was reliable.

The most reliable server build command after dependencies were prepared was:

```bash
tools/mhmake/Release64/mhmake.exe -P7 -C xorg-server MAKESERVER=1
```

### 5. Build the installer

The installer is created from:

```bash
cd xorg-server/installer
./packageall.sh nox86
```

This produces:

- `vcxsrv-64.<version>.installer.exe`

## Problems we hit and what they meant

### 1. Windows container layer import failure

Error:

```text
hcsshim::ImportLayer failed in Win32: The system cannot find the path specified. (0x3)
```

Meaning:

- Docker/Windows container setup was unhealthy or incomplete on the host.
- This was a host/container runtime issue, not a VcXsrv source issue.

### 2. `jom.exe` was expected but not present

Found in:

- [buildall.sh](buildall.sh)

Problem:

- `buildall.sh` invokes `jom.exe /J...`
- the Docker image did not include `jom.exe`

Workaround used:

- replace `jom.exe` usage with `nmake.exe` inside the container session

### 3. `rc.exe` / Windows SDK 26100 issue

Observed behavior:

- the newer Windows SDK resource compiler path caused failures

Working workaround:

- use VS environment targeting SDK `10.0.19041.0`
- copy `rc.exe` and `rcdll.dll` from `10.0.19041.0` into the `10.0.26100.0` SDK bin location inside the container

Why:

- this avoided the resource compiler failure during the build

### 4. `winflexbison` was missing or incompatible

Problem areas:

- `tools/mhmake`
- `xkbcomp`
- `hw/xwin/doflexbison.bat`
- Mesa GLSL parser generation

What was needed:

- working `win_flex.exe`
- working `win_bison.exe`
- a usable `BISON_PKGDATADIR`
- compatible parser skeleton data

Meaning:

- the stock container toolchain was not enough for the parser-generation parts of this tree

### 5. `wsl ./...` commands do not work inside this container

Found in:

- [xorg-server/xkeyboard-config/makefile](xorg-server/xkeyboard-config/makefile)
- [xorg-server/dix/makefile](xorg-server/dix/makefile)

Problem:

- the build invokes `wsl ./build.sh` and `wsl ./generate-atoms ...`
- that is not valid inside the Windows Docker container we used

Workaround used:

- replace those calls with explicit Cygwin bash invocations

Why this matters:

- if these calls do not run, the build may appear to continue but generated data can be incomplete

### 6. `fontconfig` + `gperf` CRLF issue

Problem:

- CRLF input caused `gperf` trouble in the container flow

Workaround used:

- strip `\r` before feeding the file to `gperf`

### 7. `xkeyboard-config` required extra Python/build tooling

The container was missing:

- `meson`
- `ninja`
- Python module `strenum`

This matters because:

- [xorg-server/xkeyboard-config/build.sh](xorg-server/xkeyboard-config/build.sh) uses `meson`
- [xorg-server/xkeyboard-config/rules/meson.build](xorg-server/xkeyboard-config/rules/meson.build) requires `strenum`

Without these:

- `xorg-server\xkbdata` is not fully generated
- the installer can still be built
- but the installed app may crash at startup because `xkbdata\rules\xorg` is missing

### 8. `patch` was not available in the container

Problem:

- `xorg-server/installer/packageall.sh` tries to build the "noadmin" variant by applying `noadmin.patch`
- the container did not have `patch`

Effect:

- the normal installer built successfully
- the no-admin installer path was noisy and not used

## The installer/runtime issue we debugged

When the first installer was run, VcXsrv failed with:

```text
Failed to activate virtual core keyboard: 2
```

The real clue was in:

- `%LOCALAPPDATA%\Temp\VCXSrv.0.log`

The key lines were:

```text
XKB: Couldn't open rules file C:\Program Files\VcXsrv\xkbdata/rules/xorg
XKB: Failed to load keymap.
Keyboard initialization failed.
```

Installed folder inspection showed:

- only a few `*.dir` files existed under `xkbdata`
- the full `rules`, `symbols`, `keycodes`, `types`, `compat`, and `geometry` trees were missing

## How we fixed the runtime issue

We regenerated `xorg-server\xkbdata` correctly by ensuring the container had:

- `meson`
- `ninja`
- `strenum`

Then we rebuilt the installer from that corrected build state.

That fixed installer included:

- `xkbdata\rules\xorg`
- the full XKB runtime data tree

Result:

- the new install started correctly

## Recommended verification steps

After building the installer:

1. Verify the installer exists in `build-output`.
2. Install it normally.
3. Confirm this file exists after installation:

```text
C:\Program Files\VcXsrv\xkbdata\rules\xorg
```

4. If startup fails, inspect:

```text
%LOCALAPPDATA%\Temp\VCXSrv.0.log
```

5. If the log mentions missing XKB rules or keymap compilation failure, the packaged `xkbdata` is incomplete.

## Recommended future improvements

- Update the Docker image so it includes:
  - `meson`
  - `ninja`
  - `strenum`
  - `patch`
  - working `winflexbison`
- Replace `wsl ...` build steps with container-safe Cygwin bash invocations when building in Docker.
- Document or script the SDK `19041` resource compiler workaround if SDK `26100` remains unreliable.
- Consider updating `buildall.sh` or the Docker flow so it does not depend on `jom.exe`.
- Add a packaging verification check that fails if `xkbdata\rules\xorg` is missing from the installer staging output.

## Direct Docker Changes

These are the changes I would make directly in the repo to reduce repeat failures.

- Add `patch`, `meson`, `ninja`, and `strenum` to the Docker build environment up front, not as manual recovery steps.
- Install `winflexbison` as a first-class container dependency and verify the real binaries and `data` directory are available in the image.
- Pin the Windows SDK used by the Docker build to the known-good `10.0.19041.0` toolset, or fail early if the newer SDK resource compiler is detected.
- Replace `jom.exe` usage in [buildall.sh](buildall.sh) with a tool that is actually installed in the image, or install `jom` explicitly and verify it with a startup check.
- Replace `wsl ./build.sh` in [xorg-server/xkeyboard-config/makefile](xorg-server/xkeyboard-config/makefile) with a container-safe Cygwin bash invocation when building under Docker.
- Replace `wsl ./generate-atoms ...` in [xorg-server/dix/makefile](xorg-server/dix/makefile) with the same container-safe approach.
- Add a pre-package validation step in [xorg-server/installer/packageall.sh](xorg-server/installer/packageall.sh) that checks for `xkbdata\rules\xorg` and the rest of the generated XKB tree before NSIS runs.
- Add a Docker image smoke test that confirms the build environment is complete before the source build starts. At minimum this should check `MSBuild.exe`, `nasm.exe`, `perl.exe`, `python.exe`, `patch`, `meson`, `ninja`, `win_bison.exe`, and `win_flex.exe`.
- Increase the default container memory in [runDocker.cmd](runDocker.cmd) from `4G` to a safer value, or make it configurable, because the image and build are both memory-hungry.

## One-Command Runner

The practical one-command path today is [build-vcxsrv-installer.ps1](build-vcxsrv-installer.ps1).

It repackages the verified `vcxsrv-built-release-x64-xkbfixed` image into the final installer and copies the built `vcxsrv.exe` into `build-output`.

Once the Dockerfile and build-script hardening above lands, this runner can be upgraded from "repackage a verified image" into a true pristine rebuild from `vcxb`.

## Practical takeaway

The main lesson is that a linked `vcxsrv.exe` and a successfully generated installer are not enough to call the build done.

For this project, a valid Docker build must also prove that:

- `xkeyboard-config` fully generated `xorg-server\xkbdata`
- the installer packaged that runtime data
- the installed app contains `xkbdata\rules\xorg`
- VcXsrv starts without the virtual core keyboard failure
