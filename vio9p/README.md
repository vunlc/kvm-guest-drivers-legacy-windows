# vio9p: QEMU VirtFS (virtio-9p) for Windows 7

Neither upstream virtio-win nor anyone else provides a Windows driver for
virtio-9p, the device behind QEMU's `-virtfs` / `-fsdev` sharing and UTM's
"VirtFS" directory share mode. This directory adds one, modeled on the viofs
(virtio-fs) driver:

| Component | Role |
| --- | --- |
| `pci/vio9p.sys` | KMDF transport driver for `PCI\VEN_1AF4&DEV_1009` (transitional) and `DEV_1049` (modern). Reads the mount tag and moves 9P messages over the single virtqueue. Built against KMDF 1.9, so it runs on Windows 7. |
| `svc/vio9psvc.exe` | Service with a 9P2000.L client (`p9proto.h`) that mounts the share as a drive through WinFsp. It reuses the Windows 7 device-notification code in `..\viofs\svc\utils.h`. |
| `Vio9p Package` | Driver package (INF, CAT, binaries). |

## Status

The driver was built with the EWDK 2004 (VS 2019 16.7, WDK 10.0.19041). It was
tested in a Windows 7 SP1 x64 guest under UTM 5.0.8 on macOS. In UTM the share
uses `-fsdev local,security_model=mapped-xattr` and
`-device virtio-9p-pci,mount_tag=share`.

| Check | Result |
| --- | --- |
| Driver installs and loads; device "VirtIO 9P (VirtFS) Device" reports OK | Pass |
| Service mounts the share (`Z:`, file system `VirtFS`, label = mount tag) | Pass |
| Directory listing (same as the host) | Pass |
| File reads (SHA-1 identical to the host copies) | Pass |
| Create directories and files, write, append, overwrite | Pass |
| 4 MB binary copy, byte-for-byte compare | Pass |
| Rename, move, delete files and directories | Pass |
| Removing a non-empty directory is refused | Pass |
| Auto-start as Windows service `VirtIO-9P` | Pass |
| Z: mounts by itself after reboot (about 11 s after boot) | Pass |
| Z: visible to the logged-on user and shown in Explorer, also right after boot with automatic login | Pass |
| Device restart while mounted: unmounts, then remounts by itself | Pass |
| Driver update / device restart | Pass |

UTM's **Read Only** option for the shared directory makes the host refuse all
writes with `EPERM`; turn it off to allow writes.

> **Do not share a folder that contains the VM itself**, for example a
> Downloads folder that holds `<VM>.utm`. The guest could then see, and if
> anything ever wrote to them, corrupt its own disk image and EFI variables.

## Performance

Measured in the test VM (emulated x86 on Apple Silicon, so absolute numbers
are low), with a tree of 1,090 entries in 114 directories and a 64 MB file:

| Test | Before tuning | Now |
| --- | --- | --- |
| `dir /s` of the tree | 32.0 s | 4.0 s |
| Same, again | 29.6 s | 3.9 s |
| 9P requests for one `dir /s` | 49,663 | 9,050 (measured before the lazy directory open, which removes more) |
| Read 64 MB (Z: to C:) | 0.64 s | 0.67 s |
| Write 64 MB (C: to Z:) | 0.89 s | 0.89 s |
| Copy a folder of 273 small files (4 MB) from Z: | 11.7 s | 4.05 s |
| 9P requests for that copy | 9,482 | about 3,700 |

Large files copy at about 70–100 MB/s with 128 KiB per request. Small files
are limited by round trips: each one costs a lookup, an open, a read and a
close. Setting
`HKLM\Software\VirtIO-9P\TraceRpc` = 1 together with `DebugLogFile` logs the
type and duration of every 9P request.

## Building

```bat
cd vio9p
set EWDK_WIN7_DIR=D:         &rem location of the EWDK 2004 (default c:\ewdk10_2004)
buildWin7.bat amd64          &rem or x86; see ..\viofs\WIN7.md for the toolchain
..\build\signAll.bat         &rem optional: test-sign with build\VirtIOTestCert.pfx
```

The output goes to `Install\win7\<arch>\`.

## Installer

`installer\build-installer.bat <winfsp-msi>` packages a signed build into
`Install\vio9p-win7-setup.exe`. It's a self-extracting IExpress package
(IExpress ships with Windows) that bundles the unmodified WinFsp 2.1 installer.

Running `vio9p-win7-setup.exe` (Windows asks for administrator rights) does
the following:

1. **License:** shows the license, including WinFsp's.
2. **WinFsp:** installs WinFsp silently if it's missing.
3. **Test signing:** trusts the test certificate and enables test-signing mode
   when needed. A restart is then required, and the installer says so.
4. **Driver:** installs it with `vio9psvc.exe --install-driver`. That uses
   `UpdateDriverForPlugAndPlayDevices` with `INSTALLFLAG_FORCE`, because
   Windows 7's `pnputil` can't replace an installed driver of the same version.
5. **Service:** copies `vio9psvc.exe` to `%ProgramFiles%\vio9p` and creates the
   `VirtIO-9P` service:
   * **Start type:** automatic.
   * **Depends on:** `PlugPlay` only. WinFsp loads its kernel driver on demand.
     That driver's service name includes a build timestamp (for example
     `WinFsp+20261116T025732Z`) and changes with every WinFsp release, so a
     dependency on it would break after an upgrade.
   * **Recovery:** restart if the service exits unexpectedly.

   The service deliberately does not depend on `Vio9pDrv`. That driver is
   started by Plug and Play when the device exists. As a service dependency,
   a missing device would stop the service from starting at all.
6. **Uninstaller:** registers "VirtIO 9P (VirtFS) for Windows 7" in Programs and
   Features.

`vio9p-win7-setup.exe /Q` installs silently. The uninstaller
(`uninstall-win7.bat`, also available from Programs and Features):

1. Removes the service.
2. Detaches the device (`vio9psvc.exe --remove-devices`).
3. Deletes every vio9p driver package, earlier versions included.
4. Rescans, so the device shows as having no driver.
5. Removes the files.

WinFsp, the certificate and test-signing mode stay in place.

For 64-bit Windows 7, install KB4490628 and KB4474419 (SHA-2 code signing
support) first. The installer warns if it cannot confirm them.

Options can be given as `vio9psvc.exe [-d DebugFlags] [-D DebugLogFile] [-m MountPoint] [-t Tag]`.
They can also be set as values under `HKLM\Software\VirtIO-9P`: `DebugFlags`
(DWORD), and `DebugLogFile`, `MountPoint` and `Tag` (strings). The default
mount point `*` picks the next free drive letter. `-t` selects one device when
there are several.

## Design notes and limitations

* **Drive letters are visible to every user.** Drive letters are created
  through the Windows Mount Manager (WinFsp's `\\.\X:` form). A plain `X:`
  created by this LocalSystem service at boot was visible only to SYSTEM, not
  to the logged-on user.
* **Explorer is notified.** Explorer only learns about new drives from
  notifications in its own session. On mount and unmount, the service starts
  itself briefly in each logged-on session as that user
  (`vio9psvc.exe --notify-drive X: add|remove`). That copy sends the shell
  and `WM_DEVICECHANGE` notifications so Explorer shows or drops the drive
  right away. For an added drive it first waits until the session's taskbar
  (Explorer) exists, then notifies twice, because at boot the automatic login
  races with the mount. The service also handles `WTS_SESSION_LOGON`, so users
  who log in later are notified as well.
* **Parallel requests.** The driver's I/O queue dispatches in parallel, so
  many 9P requests can be on the ring at once. If the ring is full, the
  request fails with `STATUS_RETRY` and the service resends it.
* **Directory listing.** 9P2000.L has no "readdir with attributes", so every
  entry costs a walk, a getattr and a clunk. Four things keep listings fast:
  * Lookups for a directory's entries run with 8 requests in flight.
  * Clunks are sent from a background thread, so nothing waits for them.
  * Whole listings and per-entry attributes are cached for 1 second, the same
    as WinFsp's file-information timeout. Programs open a directory several
    times in a row (`dir /s` about seven times). Any change made through the
    file system clears the caches.
  * Directories are opened for reading only when a listing has to be fetched
    from the host.
* **Folder lookup cache.** Windows opens every parent folder of a file to
  check the case of its name, so before this cache each file copied from
  eight levels down cost about 13 path walks from the root. The service now
  keeps the 9P fid of each folder it has looked up, for 10 seconds:
  * Opening a cached folder needs no walk.
  * A file is looked up with one walk step from its cached parent folder.
  * The cache holds only where a folder is, never what it contains, so
    listings and new files are still read from the host.
  * QEMU's fids stand for host paths, so a cached fid finds the same thing a
    walk from the root would. QEMU does move fids when a folder is renamed
    through 9P, so renames and folder deletes made through Z: clear the cache.
* **Changes made on the host.** 9P cannot tell the guest that something
  changed, so the service polls instead:
  * Every 2 seconds it re-reads each folder that has stayed open for at least
    1.5 seconds. Explorer keeps a handle open on the folder it shows.
  * Names added or removed since Windows last listed the folder are reported
    with `FspFileSystemNotify`, so Explorer and other `ReadDirectoryChangesW`
    users update by themselves. In testing, the VM was notified about a
    second after a file was created or deleted on the Mac.
  * At most 32 folders are polled at a time.
  * Only new, deleted and renamed entries are detected. A file changed in
    place on the host (new size or date) shows after a refresh (F5).
  * Changes made through Z: are reported by WinFsp itself and are not
    reported a second time.
* **Mounting is retried.** At boot the service can start before the device
  is ready, and the device-arrival notification is not reliable for that case.
  A mount thread therefore tries every 3 seconds until the share is mounted;
  notifications only wake it early. After the device is removed it unmounts
  and starts retrying.
* **Timeouts.** The service opens the device for overlapped I/O. A 9P request
  that gets no reply is cancelled after 10 s during the handshake and after
  60 s otherwise, so it cannot block the service forever.
* **Watchdog.** After one boot, the first request never completed until the
  device was restarted. That fits either a lost interrupt or a lost
  notification. A 200 ms periodic timer runs while the device is in D0. When
  requests are outstanding, it collects finished replies and re-notifies the
  device, so such a loss costs at most 200 ms instead of hanging the service.
* **Device access control.** The device accepts raw 9P messages, so the INF
  sets `Security` = `D:P(A;;GA;;;SY)(A;;GA;;;BA)` and `FILE_DEVICE_SECURE_OPEN`.
  Only SYSTEM (the service) and Administrators can open it. Assigning an SDDL
  in the driver itself made AddDevice fail for this PnP device, so the INF is
  the only place it is set.
* **Bounce buffers.** Each request goes through a bounce buffer, which is the
  only DMA path KMDF 1.9 supports. The client negotiates `msize` = 128 KiB.
* **Identity.** The service attaches as the owner (uid/gid) of the share's root
  directory. Depending on the security model, the server chowns new files to
  the attaching user, which fails when QEMU runs unprivileged.
* **Security.** Windows ACLs are not stored. Every file gets one descriptor
  that grants SYSTEM, Administrators and Everyone full access. Files without
  write permission bits show as read-only.
* **Not supported:**
  * symbolic links (shown as plain entries, opening them fails)
  * hard links, extended attributes, alternate data streams
* **Sleep and hibernation.** A request on the ring while the device powers down
  is not resubmitted.
