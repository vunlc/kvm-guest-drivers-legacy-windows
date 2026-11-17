# virtio-fs (viofs) on Windows 7 / Server 2008 R2

Upstream removed the Windows 7 targets in 2022 (commit `05ca163d`). This branch
adds a `Win7 Release` configuration (x86 and x64) for the viofs driver, the
`virtiofs.exe` service, the VirtIO libraries they link against and the package
project. The Windows 10/11 configurations still build the same code.

## What changed and why

| Area | Windows 7 issue | Change |
| --- | --- | --- |
| KMDF version | Windows 7 ships KMDF 1.9 in-box. Win10 builds use 1.15. | Win7 configs set `KMDF_VERSION_MINOR=9`. No co-installer is needed. |
| `VirtIO/WDF/Dma.c` | `WdfDmaTransactionInitializeUsingOffset` is KMDF 1.11+. | KMDF < 1.11 uses `WdfDmaTransactionInitialize` for a single, unchained MDL. A chained MDL gets `STATUS_NOT_SUPPORTED`. |
| `VirtIO/WDF/Dma.c` | `WdfDeviceQueryPropertyEx` is KMDF 1.13+. Windows 7 has no DMA remapping. | KMDF < 1.13 skips the IOMMU check and reports the IOMMU as inactive. |
| `viofs/pci/ioctl.c`, `isrdpc.c` | The zero-copy read IOCTL maps a two-MDL chain (reply header plus the caller's buffer). | With KMDF < 1.11 (`VIRT_FS_BOUNCE_READ`), the device writes into one nonpaged bounce buffer. The header and data are copied out when the request completes. |
| `viofs/pci/power.c` | `ExAllocatePoolZero` is not available when targeting Windows 7. | Uses `ExAllocatePoolUninitialized` followed by `RtlZeroMemory`. Behavior is the same on all targets. |
| `viofs/pci/viofs.c` | The NX pool opt-in exists only on Win8+. | `ExInitializeDriverRuntime` is compiled only when `NTDDI_VERSION > NTDDI_WIN7`, as it was before 2022. |
| `viofs/svc/utils.h` | `CM_Register_Notification` is Windows 8+. Its import would stop `virtiofs.exe` from loading on Windows 7. | When built with `_WIN32_WINNT < 0x0602`, device arrival and removal come from `RegisterDeviceNotification` through a hidden message-only window. Win8+ builds still use `CM_Register_Notification`. Both paths share one callback interface. |
| Build infra (`build/`) | Win7 support was removed from the props, targets and scripts. | Adds `Win7` to `build.bat`, `Driver.Common.props`, `Driver.Initial.props`, `Driver.PackOne.targets` and `SetVsEnv.bat`. |

## Toolchain

WDKs from Windows 11 22H2 (10.0.22621) on can no longer target Windows 7.
Use an older kit:

* **EWDK for Windows 10, version 2004 (10.0.19041)**. This is the kit the
  repository used for its Win7 builds. Unpack or mount it at `c:\ewdk10_2004`,
  or set `EWDK_WIN7_DIR` to its location.
* Alternatively, Visual Studio 2019 with WDK 10.0.19041 installed. Run the build
  from a "Developer Command Prompt" where `EnterpriseWDK` is not set.
* **WinFsp** development files under `C:\Program Files (x86)\WinFsp`
  (`inc\`, `lib\`). The service links `winfsp-x64.lib` or `winfsp-x86.lib`.

The `.bat` files must have Windows (CRLF) line endings. A normal Git checkout
on Windows converts them. If you copy the tree by other means, convert them
first, or `cmd.exe` fails with "cannot find the batch label".

Build from a fresh command prompt, not from one that already ran a newer EWDK's
`SetupBuildEnv.cmd`:

```bat
cd viofs
buildWin7.bat            &rem x86 + amd64
buildWin7.bat amd64      &rem 64-bit only
```

The output is in `viofs\Install\win7\{x86,amd64}\`: `viofs.sys`, `viofs.inf`,
`viofs.cat` and `virtiofs.exe`.

To test-sign the output with the repository's test certificate, run
`build\signAll.bat` with `EWDK_WIN7_DIR` set to the EWDK location.

## Installing in a Windows 7 SP1 guest

1. **Host side.** Start `virtiofsd`. Give QEMU a shared memory backend and a
   `vhost-user-fs-pci` device, for example
   `-object memory-backend-memfd,id=mem,size=4G,share=on -numa node,memdev=mem`
   `-chardev socket,id=char0,path=/tmp/vfsd.sock`
   `-device vhost-user-fs-pci,chardev=char0,tag=myfs`.
2. **SHA-2 support.** The build signs with SHA-256. A Windows 7 guest needs
   KB4474419 and KB4490628, or any later rollup, to verify those signatures.
3. **Signing.** Test-signed drivers need test mode on x64:
   `bcdedit /set testsigning on`, then reboot. You can also sign with your own
   certificate.
4. **Driver.** In Device Manager, update the driver of the
   "PCI Device" (`PCI\VEN_1AF4&DEV_105A`) and point it at the `Install\win7\<arch>` folder.
5. **WinFsp.** Install a WinFsp release that still supports Windows 7. Recent
   releases list Windows 7 as supported, but check the release notes of the
   version you install.
6. **Service.** Copy `virtiofs.exe` to a permanent location, then run:
   ```bat
   sc create VirtioFsSvc binpath= "C:\Program Files\VirtIO-FS\virtiofs.exe" start= auto depend= "WinFsp.Launcher/VirtioFsDrv" DisplayName= "Virtio FS Service"
   sc start VirtioFsSvc
   ```
   The share appears as a new drive letter. The registry options are the same
   as on Windows 10.

## Known limitations

* **Built, but not yet runtime-tested.** The `Win7 Release|x64` build was done
  with the EWDK 2004 (VS 2019 16.7, WDK 10.0.19041) inside a Windows 7 SP1 x64
  VM. It completed with 0 warnings and 0 errors. The x86 build has not been run
  yet. Run a smoke test before you rely on it.
  Suggested checks:
  * mount the share
  * read and write large files (this covers the bounce-buffer read path)
  * create, rename and delete files and directories
  * disable and re-enable the device in Device Manager while the service runs
    (this covers the `RegisterDeviceNotification` path)
  * restart the service
* **Extra copy on reads.** Reads through `IOCTL_VIRTFS_FUSE_REQUEST_READ` copy
  data once more than on Windows 10. Each read in flight also uses a nonpaged
  bounce buffer of up to 4 MiB.
* **No DMA remapping.** DMA remapping (IOMMU) support is Windows 10 only.
* **Upstream status.** Upstream virtio-win does not support Windows 7. Expect
  to rebase these changes yourself.
