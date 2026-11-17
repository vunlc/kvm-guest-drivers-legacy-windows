# KVM/QEMU Windows guest drivers (virtio-win) #

## About this fork: Windows 7 support ##

This is a fork of [virtio-win/kvm-guest-drivers-windows][upstream] that
brings file sharing back to **Windows 7 SP1 / Server 2008 R2** guests.
Upstream dropped the Windows 7 targets in 2022 (commit `05ca163d`). Every
other driver here is unchanged from upstream and still targets Windows 10
and 11 only.

| Component | What it adds | Status | Docs |
| --- | --- | --- | --- |
| Build system and `VirtIO` libraries | `Win7` build configurations (KMDF 1.9, `_NT_TARGET_VERSION` 0x0601). `Dma.c` falls back when KMDF 1.11/1.13 APIs are missing. | Used by both drivers below | [viofs/WIN7.md](viofs/WIN7.md) |
| `viofs` (virtio-fs) | Port of the upstream driver and `virtiofs.exe` service. Uses a bounce-buffer read path for KMDF 1.9 and `RegisterDeviceNotification` instead of `CM_Register_Notification`. | x64 builds cleanly; **not yet runtime-tested** | [viofs/WIN7.md](viofs/WIN7.md) |
| `vio9p` (virtio-9p / VirtFS) | **New** driver, WinFsp-based 9P2000.L service and installer for QEMU `-virtfs` / UTM "VirtFS" shares. Mounts the share as a drive letter (`Z:` by default). | Tested on Windows 7 SP1 x64 under UTM | [vio9p/README.md](vio9p/README.md) |

### Quick start ###

Windows 7 builds need the **EWDK for Windows 10, version 2004
(10.0.19041)**, because WDKs from Windows 11 22H2 on can no longer target
Windows 7. Mount it at `c:\ewdk10_2004` or set `EWDK_WIN7_DIR`. The
WinFsp development files must also be installed. Build from a fresh command
prompt:

```bat
cd vio9p
buildWin7.bat amd64       &rem output: Install\win7\amd64\ (unsigned)
..\build\signAll.bat      &rem test-sign with the repo's test certificate
```

Signing is required: 64-bit Windows 7 does not load an unsigned driver, even
in test-signing mode.

`viofs` builds the same way with `viofs\buildWin7.bat`. For a ready-to-run
VirtFS setup, build `vio9p-win7-setup.exe` as described in the
[vio9p installer section](vio9p/README.md#installer).

On 64-bit Windows 7, install KB4474419 and KB4490628 (SHA-2 code signing)
first. Test-signed drivers also need `bcdedit /set testsigning on`.

To pick up upstream fixes, rebase onto upstream `master`. Upstream does not
review or support the Windows 7 changes.

The rest of this file is the upstream README.

- - - -

This repository contains KVM/QEMU Windows guest drivers, for both
paravirtual and emulated hardware. The code builds and ships as part
of the virtio-win RPM on Fedora and Red Hat Enterprise Linux, and the
binaries are also available in the form of distribution-neutral ISO
and VFD images. If all you want is use virtio-win in your Windows
virtual machines, go to the
[Fedora virtIO-win documentation][fedora-virtio]
for information on obtaining the binaries.

If you'd like to build virtio-win from sources, clone this repo and
follow the instructions in [Building the Drivers][wiki-building].
Note that the drivers you build will be either unsigned or test-signed
with Tools/VirtIOTestCert.cer, which means that Windows will not load
them by default. See [Microsoft's driver signing page][ms-signing]
for more information on test-signing.

If you want to build cross-signed binaries (like the ones that ship in
the Fedora RPM), you'll need your own code-signing certificate.
Cross-signed drivers can be used on all versions of Windows except for
the latest Windows 10 with secure boot enabled. However, systems with
cross-signed drivers will not receive Microsoft support.

If you want to produce Microsoft-signed binaries (fully supported,
like the ones that ship in the Red Hat Enterprise Linux RPM), you'll
need to submit the drivers to Microsoft along with a set of test
results (so called WHQL process). If you decide to WHQL the drivers,
make sure to base them on commit eb2996de or newer, since the GPL
license used prior to this commit is not compatible with WHQL.
Additionally, we ask that you make a change to the Hardware IDs so
that your drivers will *not* match devices exposed by the upstream
versions of KVM/QEMU. This is especially important if you plan to
distribute the drivers with Windows Update, see the 
[Microsoft publishing restrictions][ms-publishing] for more details.

[upstream]:https://github.com/virtio-win/kvm-guest-drivers-windows
[fedora-virtio]:https://docs.fedoraproject.org/en-US/quick-docs/creating-windows-virtual-machines-using-virtio-drivers/index.html
[wiki-building]:https://virtio-win.github.io/Development/Building-the-drivers-using-Windows-11-24H2-EWDK
[ms-signing]:https://docs.microsoft.com/en-us/windows-hardware/drivers/install/installing-test-signed-driver-packages
[ms-publishing]:https://docs.microsoft.com/en-us/windows-hardware/drivers/dashboard/publishing-restrictions
- - - -
