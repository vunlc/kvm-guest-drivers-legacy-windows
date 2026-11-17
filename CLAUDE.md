# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this repository is

A fork of upstream virtio-win (`upstream` remote, `master` branch) that brings Windows 7 SP1 / Server 2008 R2 support back for file sharing. The fork's commits sit on top of upstream on `main`; to pick up upstream fixes, rebase onto `upstream/master`. Only these parts differ from upstream:

- `build/` and `VirtIO/`: `Win7 Release` configurations (KMDF 1.9, `_NT_TARGET_VERSION` 0x0601) and KMDF-version fallbacks in `VirtIO/WDF`.
- `viofs/` (virtio-fs): Windows 7 port of the upstream driver and `virtiofs.exe`. It builds but has **not been runtime-tested**. See [viofs/WIN7.md](viofs/WIN7.md).
- `vio9p/` (virtio-9p / QEMU VirtFS): a **new** driver, service and installer that doesn't exist upstream. It is tested on Win7 SP1 x64 under UTM. See [vio9p/README.md](vio9p/README.md).

Every other driver (NetKVM, viostor, vioscsi, Balloon, …) is unchanged upstream code that targets Windows 10/11 only. Leave those drivers alone unless asked, so that rebases stay clean. Only x64 matters for the Win7 work; 32-bit builds aren't wanted.

## Building

Builds need Windows and run only there, not on the macOS checkout. Windows 7 targets need the **EWDK for Windows 10 2004 (10.0.19041)**, because newer WDKs can't target Win7. They also need the WinFsp dev files under `C:\Program Files (x86)\WinFsp`. Run the build from a fresh `cmd.exe`, not one that already ran a newer EWDK's `SetupBuildEnv.cmd`: `SetVsEnv.bat` skips setup if `EnterpriseWDK` is already set.

```bat
cd vio9p
set EWDK_WIN7_DIR=D:        &rem EWDK location (default c:\ewdk10_2004); set it before building
buildWin7.bat amd64         &rem -> vio9p\Install\win7\amd64\ (unsigned)
..\build\signAll.bat        &rem test-sign with build\VirtIOTestCert.pfx; required, x64 Win7 won't load unsigned drivers
installer\build-installer.bat <path-to-winfsp-msi>   &rem -> vio9p\Install\vio9p-win7-setup.exe (IExpress)
```

`viofs\buildWin7.bat amd64` works the same way. Each driver directory has a `clean.bat`.

How the build works: `buildWin7.bat` calls `build\build.bat <sln> Win7 [amd64|x86] [/Rebuild]`. That script maps the target to the VS configuration `"Win7 Release|x64"`. Only `Win7 Release` exists in the solutions and projects, so `build.bat`'s `Debug` option fails for Win7 even though `Driver.Common.props` mentions `Win7 Debug`. `build\SetVsEnv.bat Win7` picks the EWDK from `EWDK_WIN7_DIR`; Win10 and Win11 use `EWDK11_DIR` and `EWDK11_26H1_DIR`. The Win7 configs are defined in `build\Driver.Common.props`, `Driver.Initial.props` and `Driver.PackOne.targets`. Win7 builds are Desktop drivers, not Universal. `signAll.bat` signs every `.sys`/`.cat` whose path contains `win7`, `win10` or `win11`, using the matching EWDK's `signtool`.

There is no test suite. Testing means installing on a Windows 7 guest; the checklists are in the two docs linked above. Code style is checked with clang-format. The repo root has `.clang-format` for driver code, and `VirtIO/.clang-format` covers the VirtIO library.

## Windows 7 compatibility patterns

Code shared with the Win10/11 builds must keep compiling for all targets. Branch on these compile-time switches, never at run time:

- `VIRTIO_WDF_KMDF_PRE_1_11` / `..._PRE_1_13` in `VirtIO/WDF/VirtIOWdf.h`: KMDF 1.9 has no `WdfDmaTransactionInitializeUsingOffset` (chained MDLs) and no `WdfDeviceQueryPropertyEx`. viofs aliases the first as `VIRT_FS_BOUNCE_READ` and copies reads through a nonpaged bounce buffer. vio9p always uses bounce buffers.
- `NTDDI_VERSION > NTDDI_WIN7` guards `ExInitializeDriverRuntime` (NX pool opt-in). Use `ExAllocatePoolUninitialized` + `RtlZeroMemory`, not `ExAllocatePoolZero`.
- `#if (_WIN32_WINNT >= _WIN32_WINNT_WIN8)` in `viofs/svc/utils.h` (its `#else` is the Win7 path) switches device arrival and removal from `CM_Register_Notification` (Win8+) to `RegisterDeviceNotification` with a hidden message-only window. A Win8+ import would stop the service from loading on Win7.

## vio9p architecture

- **`pci/vio9p.sys`** is a thin KMDF transport for `PCI\VEN_1AF4&DEV_1009` / `DEV_1049`. It reads the mount tag and moves opaque 9P messages over one virtqueue. Its I/O queue dispatches in parallel. When the ring is full, a request fails with `STATUS_RETRY` and the service resends it. A 200 ms watchdog timer recovers lost interrupts and notifications. Device ACLs (SYSTEM + Administrators only) are set in the INF `Security` line. Setting them from the driver made AddDevice fail.
- **`shared/vio9p_ioctl.h`** is the whole driver/service contract: `GUID_DEVINTERFACE_VIO9P`, `IOCTL_VIO9P_GET_MOUNT_TAG` and `IOCTL_VIO9P_REQUEST` (one T-message in, one R-message out).
- **`svc/vio9psvc.exe`** holds all file system logic. It contains a 9P2000.L client (`p9proto.h`) and a WinFsp file system (`vio9psvc.cpp`). It runs as the `VirtIO-9P` service, with self-install switches such as `--install-driver` and `--notify-drive`. Directory listing caches, the folder-fid cache, host-change polling, mount retry and per-session Explorer notifications all live here; their design notes are in the vio9p README. Registry settings are under `HKLM\Software\VirtIO-9P`. `TraceRpc=1` together with `DebugLogFile` logs every 9P request.
- The service includes **`viofs/svc/utils.h`**, so a change there affects both viofs and vio9p.
- **`installer/`** has `install-win7.bat` and `uninstall-win7.bat`, which IExpress bundles with the WinFsp MSI and the test certificate.

## Repo conventions and gotchas

- **Batch files run by `cmd.exe` on Win7 need CRLF line endings**, or cmd fails with "cannot find the batch label". `.gitattributes` forces CRLF only for `vio9p/**/*.bat` and `viofs/buildWin7.bat`. A file copied into a VM by other means than git must be converted first.
- `_local/` holds the EWDK ISO, the WinFsp MSI and built outputs. It is ignored through `.git/info/exclude`; never commit it.
- Upstream's `.github/` CI and `.hck-ci/` were removed on purpose; don't restore them.
- Upstream's [contributing.md](contributing.md) asks for clang-format-clean code and commit subjects prefixed with the component, e.g. `vio9p: …` or `build, VirtIO: …`. This fork's history follows that prefix style.
