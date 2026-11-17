/*
 * vio9psvc.exe - mounts a QEMU VirtFS (virtio-9p, 9P2000.L) share as a
 * Windows drive using WinFsp. The transport is vio9p.sys.
 *
 * The service structure (device arrival/removal handling, WinFsp service
 * glue) follows the virtiofs service in ..\..\viofs\svc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met :
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and / or other materials provided with the distribution.
 * 3. Neither the names of the copyright holders nor the names of their contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#pragma warning(push, 3)
#include "winfsp/winfsp.h"
#pragma warning(pop)

#include <sddl.h>
#include <shlobj.h>
#include <wtsapi32.h>
#include <dbt.h>
#include <newdev.h>

#pragma comment(lib, "newdev.lib")
#include <setupapi.h>
#include <initguid.h>
#include <stdio.h>
#include <wctype.h>

#include <algorithm>
#include <iterator>
#include <memory>
#include <unordered_map>
#include <string>
#include <vector>

#include "vio9p_ioctl.h"
#include "p9proto.h"
#pragma warning(push)
#pragma warning(disable : 4505) // helpers in utils.h this service does not use
#include "utils.h"        // ..\..\viofs\svc: device lookup and arrival/removal notifications
#pragma warning(pop)

#define FS_SERVICE_NAME    TEXT("VirtIO-9P")
#define FS_SERVICE_REGKEY  TEXT("Software\\") FS_SERVICE_NAME
#define FS_NAME            TEXT("VirtFS")
#define ALLOCATION_UNIT    4096
#define DEFAULT_MSIZE      (128 * 1024)
#define DEFAULT_FILE_MODE  0644
#define DEFAULT_DIR_MODE   0755
#define FILETIME_UNIX_BASE 116444736000000000ULL

// Everyone, Administrators and SYSTEM get full access; ownership and ACLs are
// not stored on the host.
#define DEFAULT_SDDL L"O:BAG:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FA;;;WD)"

#define DBG(format, ...) FspDebugLog("*** %s: " format "\n", __FUNCTION__, __VA_ARGS__)

// A walked (never opened) fid shared by the directory fid cache and open
// directory handles. The last reference clunks it.
struct FID_REF
{
    struct VIO9P *Fs;
    uint32_t Fid;
    FID_REF(struct VIO9P *F, uint32_t Id) : Fs(F), Fid(Id)
    {
    }
    ~FID_REF();
    FID_REF(const FID_REF &) = delete;
    FID_REF &operator=(const FID_REF &) = delete;
};

struct VIO9P_FILE_CONTEXT
{
    // Files: opened with Tlopen/Tlcreate. Directories: opened lazily, only when
    // the listing has to be read from the host (see EnsureDirOpen).
    uint32_t Fid{p9::NOFID};
    uint32_t DirFid{p9::NOFID}; // directories: unopened fid, for metadata and walks
    std::shared_ptr<FID_REF> DirRef; // owns DirFid (possibly shared with FidCache)
    SRWLOCK OpenLock = SRWLOCK_INIT;
    uint32_t Iounit{0};
    bool IsDir{false};
    bool IsRoot{false};
    PVOID DirBuffer{nullptr};
    std::wstring Path; // as given by WinFsp, e.g. "\\dir\\file"
    std::wstring WatchKey; // directories: key in VIO9P::Watches
};

// Fid for getattr/setattr: the open fid of a file, or a directory's walk fid.
static inline uint32_t MetaFid(const struct VIO9P_FILE_CONTEXT *Ctx)
{
    return Ctx->DirFid != p9::NOFID ? Ctx->DirFid : Ctx->Fid;
}

struct VIO9P
{
    FSP_FILE_SYSTEM *FileSystem{nullptr};
    HANDLE Device{INVALID_HANDLE_VALUE};
    p9::Client Client;
    uint32_t RootFid{p9::NOFID};

    ULONG DebugFlags{0};
    std::wstring MountPoint{L"*"};
    std::wstring Tag{};
    std::wstring VolumeLabel{};

    PSECURITY_DESCRIPTOR DefaultSd{nullptr};
    ULONG DefaultSdSize{0};

    // Used to handle device arrival.
    DeviceInterfaceNotification DevInterfaceNotification{};
    // Used to handle device removal.
    DeviceHandleNotification DevHandleNotification{};
    bool HandleNotificationRegistered{false};

    // The mount thread keeps trying to mount while the share is not mounted.
    // Device notifications only wake it early; Lock serializes mounting with
    // device removal and service stop.
    CRITICAL_SECTION Lock;
    HANDLE MountThread{NULL};
    HANDLE StopEvent{NULL};
    HANDLE WakeEvent{NULL};
    NTSTATUS LastLoggedStatus{STATUS_SUCCESS};
    bool LoggedWaiting{false};
    // Drive letter of the current mount (0 if none or not a drive letter).
    volatile WCHAR MountedDrive{0};

    VIO9P()
    {
        InitializeCriticalSection(&Lock);
        InitializeCriticalSection(&ClunkLock);
    }

    ~VIO9P()
    {
        if (DefaultSd != nullptr)
        {
            LocalFree(DefaultSd);
        }
        if (StopEvent != NULL)
        {
            CloseHandle(StopEvent);
        }
        if (WakeEvent != NULL)
        {
            CloseHandle(WakeEvent);
        }
        if (MountThread != NULL)
        {
            CloseHandle(MountThread);
        }
        if (PollStopEvent != NULL)
        {
            CloseHandle(PollStopEvent);
        }
        DeleteCriticalSection(&ClunkLock);
        DeleteCriticalSection(&Lock);
    }

    // Fids are released ("clunked") on a background thread: nothing waits for
    // the reply, so this takes a round trip off every lookup and close.
    CRITICAL_SECTION ClunkLock;
    std::vector<uint32_t> ClunkPending;
    HANDLE ClunkEvent{NULL};
    HANDLE ClunkThread{NULL};
    volatile bool ClunkStop{false};

    void QueueClunk(uint32_t Fid);
    void StartClunker();
    void StopClunker();
    static DWORD WINAPI ClunkThreadProc(PVOID Param);

    // Attributes seen while listing a directory, keyed by upper-cased path and
    // kept as long as WinFsp caches file information. Lets the lookups that
    // usually follow a listing (GetSecurityByName, Open) skip the host. Any
    // change made through this file system clears it.
    struct CACHED_ATTR
    {
        p9::Attr Attr;
        ULONGLONG Tick;
    };
    SRWLOCK CacheLock = SRWLOCK_INIT;
    std::unordered_map<std::wstring, CACHED_ATTR> AttrCache;

    // Whole directory listings, same lifetime and invalidation as AttrCache.
    // Programs open a directory several times in a row (cmd's "dir /s" about
    // seven times), and every new handle would otherwise list it again.
    struct DIR_ITEM
    {
        std::string Name;
        p9::Attr Attr;
    };
    struct DIR_LISTING
    {
        std::vector<DIR_ITEM> Items;
        ULONGLONG Tick;
    };
    std::unordered_map<std::wstring, std::shared_ptr<const DIR_LISTING>> DirCache;

    std::shared_ptr<const DIR_LISTING> DirCacheGet(const std::wstring &Path);
    void DirCachePut(const std::wstring &Path, std::shared_ptr<const DIR_LISTING> Listing);

    // Walked fids of directories, keyed like AttrCache. Lookups start from the
    // deepest cached directory instead of the root (a file deep in the tree is
    // one walk step instead of one per path component), and opening a cached
    // directory needs no walk at all. Windows opens every parent directory of
    // a file to check the case of its name, so this matters for each file.
    // QEMU's fids name host paths, so walking from a cached fid finds the same
    // object as walking from the root. QEMU does move the paths of fids under
    // a directory renamed through 9P, so renames and deletes made here clear
    // the cache; entries also expire.
    struct CACHED_FID
    {
        std::shared_ptr<FID_REF> Ref;
        ULONGLONG Tick;
    };
    SRWLOCK FidCacheLock = SRWLOCK_INIT;
    std::unordered_map<std::wstring, CACHED_FID> FidCache;

    std::shared_ptr<FID_REF> FidCacheGet(const std::wstring &Key);
    void FidCachePut(const std::wstring &Key, const std::shared_ptr<FID_REF> &Ref);
    void FidCacheDrop(const std::wstring &Key);
    void FidCacheClear();
    NTSTATUS WalkComponents(PCWSTR FileName,
                            const std::vector<std::string> &Parts,
                            size_t Count,
                            uint32_t NewFid,
                            size_t *FailedAt);

    // 9P has no change notification, so changes made on the host are found by
    // polling. Explorer keeps a handle open on the folder it shows (to receive
    // change notifications); every directory that stays open for a while is
    // read again every POLL_INTERVAL_MS, and names added or removed since
    // Windows last listed it are reported through FspFileSystemNotify.
    struct WATCH
    {
        std::wstring Path;              // as first opened, for walking
        ULONG Handles{0};               // open handles of this directory
        ULONGLONG Since{0};             // when Handles went from 0 to 1
        ULONGLONG Touched{0};           // last open, close or listing
        bool HaveNames{false};          // Names is a valid baseline
        std::vector<std::string> Names; // sorted, as Windows last saw them
    };
    SRWLOCK WatchLock = SRWLOCK_INIT;
    std::unordered_map<std::wstring, WATCH> Watches; // keyed like AttrCache
    HANDLE PollThread{NULL};
    HANDLE PollStopEvent{NULL};

    void WatchOpen(const std::wstring &Key, const std::wstring &Path);
    void WatchClose(const std::wstring &Key);
    void WatchListed(const std::wstring &Key, const std::wstring &Path, const std::vector<std::string> &Names);
    void WatchForget(const std::wstring &Key);
    void StartPoller();
    void StopPoller();
    void PollOnce();
    static DWORD WINAPI PollThreadProc(PVOID Param);

    static std::wstring CacheKey(const std::wstring &Path);
    void CachePut(const std::wstring &Path, const p9::Attr &Attr);
    bool CacheGet(const std::wstring &Path, p9::Attr *Attr);
    void CacheClear();

    NTSTATUS Start();
    VOID Stop();
    DWORD FindDeviceInterface();
    VOID TryMount();
    VOID DevArrival();
    VOID DevQueryRemove();
    VOID CloseDeviceInterface();

    NTSTATUS WalkPath(PCWSTR FileName, uint32_t *NewFid);
    NTSTATUS WalkParent(PCWSTR FileName, uint32_t *ParentFid, std::string *Leaf);
};

static void DeviceNotificationCallback(PVOID Context, DeviceNotifyAction Action);

#define ATTR_CACHE_TTL_MS 1000

void VIO9P::QueueClunk(uint32_t Fid)
{
    if (Fid == p9::NOFID)
    {
        return;
    }
    EnterCriticalSection(&ClunkLock);
    bool queued = (ClunkThread != NULL);
    if (queued)
    {
        try
        {
            ClunkPending.push_back(Fid);
        }
        catch (std::bad_alloc &)
        {
            queued = false;
        }
    }
    LeaveCriticalSection(&ClunkLock);

    if (queued)
    {
        SetEvent(ClunkEvent);
    }
    else
    {
        Client.Clunk(Fid);
    }
}

DWORD WINAPI VIO9P::ClunkThreadProc(PVOID Param)
{
    VIO9P *fs = (VIO9P *)Param;
    std::vector<uint32_t> batch;

    for (;;)
    {
        WaitForSingleObject(fs->ClunkEvent, INFINITE);

        EnterCriticalSection(&fs->ClunkLock);
        batch.swap(fs->ClunkPending);
        bool stop = fs->ClunkStop;
        LeaveCriticalSection(&fs->ClunkLock);

        for (uint32_t fid : batch)
        {
            fs->Client.Clunk(fid);
        }
        batch.clear();

        if (stop)
        {
            return 0;
        }
    }
}

void VIO9P::StartClunker()
{
    if (ClunkEvent == NULL)
    {
        ClunkEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    }
    ClunkStop = false;
    if (ClunkEvent != NULL)
    {
        HANDLE thread = CreateThread(NULL, 0, ClunkThreadProc, this, 0, NULL);
        EnterCriticalSection(&ClunkLock);
        ClunkThread = thread;
        LeaveCriticalSection(&ClunkLock);
    }
}

// Releases the remaining fids and ends the thread. Call after the dispatcher
// has stopped, before the device handle is closed.
void VIO9P::StopClunker()
{
    EnterCriticalSection(&ClunkLock);
    HANDLE thread = ClunkThread;
    ClunkThread = NULL;
    ClunkStop = true;
    LeaveCriticalSection(&ClunkLock);

    if (thread != NULL)
    {
        SetEvent(ClunkEvent);
        WaitForSingleObject(thread, INFINITE);
        CloseHandle(thread);
    }
    ClunkPending.clear();
}

std::wstring VIO9P::CacheKey(const std::wstring &Path)
{
    std::wstring key = Path;
    if (key.size() > 1 && key.back() == L'\\')
    {
        key.pop_back();
    }
    if (!key.empty())
    {
        CharUpperBuffW(&key[0], (DWORD)key.size());
    }
    return key;
}

void VIO9P::CachePut(const std::wstring &Path, const p9::Attr &Attr)
{
    try
    {
        std::wstring key = CacheKey(Path);
        AcquireSRWLockExclusive(&CacheLock);
        if (AttrCache.size() > 50000)
        {
            AttrCache.clear();
        }
        AttrCache[key] = CACHED_ATTR{Attr, GetTickCount64()};
        ReleaseSRWLockExclusive(&CacheLock);
    }
    catch (std::bad_alloc &)
    {
    }
}

bool VIO9P::CacheGet(const std::wstring &Path, p9::Attr *Attr)
{
    bool hit = false;
    try
    {
        std::wstring key = CacheKey(Path);
        AcquireSRWLockShared(&CacheLock);
        auto it = AttrCache.find(key);
        if (it != AttrCache.end() && GetTickCount64() - it->second.Tick < ATTR_CACHE_TTL_MS)
        {
            *Attr = it->second.Attr;
            hit = true;
        }
        ReleaseSRWLockShared(&CacheLock);
    }
    catch (std::bad_alloc &)
    {
    }
    return hit;
}

void VIO9P::CacheClear()
{
    AcquireSRWLockExclusive(&CacheLock);
    AttrCache.clear();
    DirCache.clear();
    ReleaseSRWLockExclusive(&CacheLock);
}

std::shared_ptr<const VIO9P::DIR_LISTING> VIO9P::DirCacheGet(const std::wstring &Path)
{
    std::shared_ptr<const DIR_LISTING> listing;
    if (Path.empty())
    {
        return listing;
    }
    try
    {
        std::wstring key = CacheKey(Path);
        AcquireSRWLockShared(&CacheLock);
        auto it = DirCache.find(key);
        if (it != DirCache.end() && GetTickCount64() - it->second->Tick < ATTR_CACHE_TTL_MS)
        {
            listing = it->second;
        }
        ReleaseSRWLockShared(&CacheLock);
    }
    catch (std::bad_alloc &)
    {
    }
    return listing;
}

void VIO9P::DirCachePut(const std::wstring &Path, std::shared_ptr<const DIR_LISTING> Listing)
{
    if (Path.empty())
    {
        return;
    }
    try
    {
        std::wstring key = CacheKey(Path);
        AcquireSRWLockExclusive(&CacheLock);
        if (DirCache.size() > 1000)
        {
            DirCache.clear();
        }
        DirCache[key] = std::move(Listing);
        ReleaseSRWLockExclusive(&CacheLock);
    }
    catch (std::bad_alloc &)
    {
    }
}

#define FID_CACHE_TTL_MS 10000
#define FID_CACHE_MAX 1024

FID_REF::~FID_REF()
{
    Fs->QueueClunk(Fid);
}

std::shared_ptr<FID_REF> VIO9P::FidCacheGet(const std::wstring &Key)
{
    std::shared_ptr<FID_REF> ref;
    AcquireSRWLockShared(&FidCacheLock);
    auto it = FidCache.find(Key);
    if (it != FidCache.end() && GetTickCount64() - it->second.Tick < FID_CACHE_TTL_MS)
    {
        ref = it->second.Ref;
    }
    ReleaseSRWLockShared(&FidCacheLock);
    return ref;
}

void VIO9P::FidCachePut(const std::wstring &Key, const std::shared_ptr<FID_REF> &Ref)
{
    // Replaced or evicted references are released outside the lock: the last
    // one queues a clunk.
    std::unordered_map<std::wstring, CACHED_FID> evicted;
    std::shared_ptr<FID_REF> replaced;
    try
    {
        AcquireSRWLockExclusive(&FidCacheLock);
        if (FidCache.size() >= FID_CACHE_MAX)
        {
            evicted.swap(FidCache);
        }
        CACHED_FID &slot = FidCache[Key];
        replaced = std::move(slot.Ref);
        slot.Ref = Ref;
        slot.Tick = GetTickCount64();
        ReleaseSRWLockExclusive(&FidCacheLock);
    }
    catch (std::bad_alloc &)
    {
        ReleaseSRWLockExclusive(&FidCacheLock);
    }
}

void VIO9P::FidCacheDrop(const std::wstring &Key)
{
    std::shared_ptr<FID_REF> dropped;
    AcquireSRWLockExclusive(&FidCacheLock);
    auto it = FidCache.find(Key);
    if (it != FidCache.end())
    {
        dropped = std::move(it->second.Ref);
        FidCache.erase(it);
    }
    ReleaseSRWLockExclusive(&FidCacheLock);
}

void VIO9P::FidCacheClear()
{
    std::unordered_map<std::wstring, CACHED_FID> old;
    AcquireSRWLockExclusive(&FidCacheLock);
    old.swap(FidCache);
    ReleaseSRWLockExclusive(&FidCacheLock);
}

#define POLL_INTERVAL_MS 2000
#define POLL_MIN_OPEN_MS 1500 // ignore directories opened only briefly
#define POLL_MAX_DIRS 32
#define WATCH_KEEP_MS 60000 // keep the names of closed directories this long
#define WATCH_MAX 4096

// "\\A\\B" -> "\\A", "\\A" -> "\\"
static std::wstring ParentKey(const std::wstring &Key)
{
    size_t pos = Key.find_last_of(L'\\');
    if (pos == std::wstring::npos || pos == 0)
    {
        return L"\\";
    }
    return Key.substr(0, pos);
}

void VIO9P::WatchOpen(const std::wstring &Key, const std::wstring &Path)
{
    try
    {
        AcquireSRWLockExclusive(&WatchLock);
        WATCH &w = Watches[Key];
        if (w.Handles++ == 0)
        {
            w.Since = GetTickCount64();
            w.Path = Path;
        }
        w.Touched = GetTickCount64();
        ReleaseSRWLockExclusive(&WatchLock);
    }
    catch (std::bad_alloc &)
    {
        ReleaseSRWLockExclusive(&WatchLock);
    }
}

void VIO9P::WatchClose(const std::wstring &Key)
{
    AcquireSRWLockExclusive(&WatchLock);
    auto it = Watches.find(Key);
    if (it != Watches.end() && it->second.Handles > 0)
    {
        it->second.Handles--;
        it->second.Touched = GetTickCount64();
    }
    ReleaseSRWLockExclusive(&WatchLock);
}

// Records what Windows was just shown, as the baseline for polling.
void VIO9P::WatchListed(const std::wstring &Key, const std::wstring &Path, const std::vector<std::string> &Names)
{
    try
    {
        AcquireSRWLockExclusive(&WatchLock);
        if (Watches.size() >= WATCH_MAX)
        {
            for (auto it = Watches.begin(); it != Watches.end();)
            {
                it = (it->second.Handles == 0) ? Watches.erase(it) : std::next(it);
            }
        }
        WATCH &w = Watches[Key];
        if (w.Path.empty())
        {
            w.Path = Path;
        }
        w.Names = Names;
        w.HaveNames = true;
        w.Touched = GetTickCount64();
        ReleaseSRWLockExclusive(&WatchLock);
    }
    catch (std::bad_alloc &)
    {
        ReleaseSRWLockExclusive(&WatchLock);
    }
}

// The directory was changed through this file system; WinFsp reports that
// change itself, so the next poll only takes a new baseline.
void VIO9P::WatchForget(const std::wstring &Key)
{
    AcquireSRWLockExclusive(&WatchLock);
    auto it = Watches.find(Key);
    if (it != Watches.end())
    {
        it->second.HaveNames = false;
    }
    ReleaseSRWLockExclusive(&WatchLock);
}

//
// Helpers
//

static bool WideToUtf8(const WCHAR *Src, int Len, std::string &Out)
{
    Out.clear();
    if (Len == 0)
    {
        return true;
    }
    int n = WideCharToMultiByte(CP_UTF8, 0, Src, Len, NULL, 0, NULL, NULL);
    if (n <= 0)
    {
        return false;
    }
    Out.resize(n);
    return WideCharToMultiByte(CP_UTF8, 0, Src, Len, &Out[0], n, NULL, NULL) == n;
}

static int Utf8ToWide(const std::string &Src, WCHAR *Out, int OutChars)
{
    if (Src.empty())
    {
        return 0;
    }
    return MultiByteToWideChar(CP_UTF8, 0, Src.data(), (int)Src.size(), Out, OutChars);
}

// "\a\b\c" -> {"a","b","c"}
static NTSTATUS SplitPath(PCWSTR FileName, std::vector<std::string> &Parts)
{
    Parts.clear();
    const WCHAR *p = FileName;
    while (*p)
    {
        while (*p == L'\\')
        {
            p++;
        }
        const WCHAR *start = p;
        while (*p && *p != L'\\')
        {
            p++;
        }
        if (p > start)
        {
            std::string part;
            if (!WideToUtf8(start, (int)(p - start), part))
            {
                return STATUS_OBJECT_NAME_INVALID;
            }
            Parts.push_back(std::move(part));
        }
    }
    return STATUS_SUCCESS;
}

// Cache key of the first Components path components of FileName (as split by
// SplitPath), e.g. "\\a\\b\\c", 2 -> "\\A\\B".
static std::wstring PrefixKey(PCWSTR FileName, size_t Components)
{
    std::wstring key;
    const WCHAR *p = FileName;
    size_t done = 0;
    while (*p && done < Components)
    {
        while (*p == L'\\')
        {
            p++;
        }
        const WCHAR *start = p;
        while (*p && *p != L'\\')
        {
            p++;
        }
        if (p > start)
        {
            key += L'\\';
            key.append(start, p - start);
            done++;
        }
    }
    if (!key.empty())
    {
        CharUpperBuffW(&key[0], (DWORD)key.size());
    }
    return key;
}

static UINT64 UnixToFileTime(uint64_t Sec, uint64_t Nsec)
{
    return Sec * 10000000ULL + Nsec / 100 + FILETIME_UNIX_BASE;
}

static void FileTimeToUnix(UINT64 FileTime, uint64_t *Sec, uint64_t *Nsec)
{
    if (FileTime < FILETIME_UNIX_BASE)
    {
        *Sec = 0;
        *Nsec = 0;
        return;
    }
    FileTime -= FILETIME_UNIX_BASE;
    *Sec = FileTime / 10000000ULL;
    *Nsec = (FileTime % 10000000ULL) * 100;
}

static UINT32 AttrToFileAttributes(const p9::Attr &A)
{
    if (A.IsDir())
    {
        return FILE_ATTRIBUTE_DIRECTORY;
    }
    UINT32 attributes = FILE_ATTRIBUTE_ARCHIVE;
    if ((A.mode & 0222) == 0)
    {
        attributes |= FILE_ATTRIBUTE_READONLY;
    }
    return attributes;
}

static void FillFileInfo(const p9::Attr &A, FSP_FSCTL_FILE_INFO *FileInfo)
{
    ZeroMemory(FileInfo, sizeof(*FileInfo));
    FileInfo->FileAttributes = AttrToFileAttributes(A);
    FileInfo->FileSize = A.IsDir() ? 0 : A.size;
    FileInfo->AllocationSize = (FileInfo->FileSize + ALLOCATION_UNIT - 1) / ALLOCATION_UNIT * ALLOCATION_UNIT;
    FileInfo->LastAccessTime = UnixToFileTime(A.atime_sec, A.atime_nsec);
    FileInfo->LastWriteTime = UnixToFileTime(A.mtime_sec, A.mtime_nsec);
    FileInfo->ChangeTime = UnixToFileTime(A.ctime_sec, A.ctime_nsec);
    FileInfo->CreationTime = (A.valid & p9::GETATTR_BTIME) ? UnixToFileTime(A.btime_sec, A.btime_nsec)
                                                            : FileInfo->LastWriteTime;
    FileInfo->IndexNumber = A.qid.path;
}

static NTSTATUS GetFileInfoByFid(VIO9P *Fs, uint32_t Fid, FSP_FSCTL_FILE_INFO *FileInfo)
{
    p9::Attr attr;
    NTSTATUS status = Fs->Client.Getattr(Fid, &attr);
    if (NT_SUCCESS(status))
    {
        FillFileInfo(attr, FileInfo);
    }
    return status;
}

static NTSTATUS CopyDefaultSecurity(VIO9P *Fs, PSECURITY_DESCRIPTOR SecurityDescriptor, SIZE_T *PSecurityDescriptorSize)
{
    if (PSecurityDescriptorSize == nullptr)
    {
        return STATUS_SUCCESS;
    }
    if (*PSecurityDescriptorSize < Fs->DefaultSdSize)
    {
        *PSecurityDescriptorSize = Fs->DefaultSdSize;
        return STATUS_BUFFER_OVERFLOW;
    }
    *PSecurityDescriptorSize = Fs->DefaultSdSize;
    if (SecurityDescriptor != nullptr)
    {
        memcpy(SecurityDescriptor, Fs->DefaultSd, Fs->DefaultSdSize);
    }
    return STATUS_SUCCESS;
}

// Walks the first Count components of Parts into NewFid, starting from the
// deepest cached directory on the way. *FailedAt indexes Parts.
NTSTATUS VIO9P::WalkComponents(PCWSTR FileName,
                               const std::vector<std::string> &Parts,
                               size_t Count,
                               uint32_t NewFid,
                               size_t *FailedAt)
{
    std::shared_ptr<FID_REF> base;
    size_t skip = 0;

    try
    {
        for (size_t k = Count; k > 0 && !base; k--)
        {
            base = FidCacheGet(PrefixKey(FileName, k));
            if (base)
            {
                skip = k;
            }
        }
        std::vector<std::string> rest(Parts.begin() + skip, Parts.begin() + Count);
        size_t failedAt = 0;
        NTSTATUS status = Client.Walk(base ? base->Fid : RootFid, NewFid, rest, nullptr, &failedAt);
        *FailedAt = skip + failedAt;
        return status;
    }
    catch (std::bad_alloc &)
    {
        *FailedAt = 0;
        return STATUS_INSUFFICIENT_RESOURCES;
    }
}

NTSTATUS VIO9P::WalkPath(PCWSTR FileName, uint32_t *NewFid)
{
    std::vector<std::string> parts;
    NTSTATUS status = SplitPath(FileName, parts);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    uint32_t fid = Client.NewFid();
    size_t failedAt = 0;
    status = WalkComponents(FileName, parts, parts.size(), fid, &failedAt);
    if (!NT_SUCCESS(status))
    {
        // A missing intermediate directory is "path not found".
        if ((status == STATUS_OBJECT_NAME_NOT_FOUND || status == STATUS_NOT_A_DIRECTORY) &&
            failedAt + 1 < parts.size())
        {
            status = STATUS_OBJECT_PATH_NOT_FOUND;
        }
        return status;
    }

    *NewFid = fid;
    return STATUS_SUCCESS;
}

NTSTATUS VIO9P::WalkParent(PCWSTR FileName, uint32_t *ParentFid, std::string *Leaf)
{
    std::vector<std::string> parts;
    NTSTATUS status = SplitPath(FileName, parts);
    if (!NT_SUCCESS(status))
    {
        return status;
    }
    if (parts.empty())
    {
        return STATUS_ACCESS_DENIED; // the root has no parent
    }

    *Leaf = parts.back();

    uint32_t fid = Client.NewFid();
    size_t failedAt = 0;
    status = WalkComponents(FileName, parts, parts.size() - 1, fid, &failedAt);
    if (!NT_SUCCESS(status))
    {
        return (status == STATUS_OBJECT_NAME_NOT_FOUND || status == STATUS_NOT_A_DIRECTORY)
                   ? STATUS_OBJECT_PATH_NOT_FOUND
                   : status;
    }

    *ParentFid = fid;
    return STATUS_SUCCESS;
}

// Opens Fid for I/O. Directories keep an unopened clone in DirFid for walking.
// For a directory, DirRef (if given) already owns PathFid.
static NTSTATUS OpenFileContext(VIO9P *Fs,
                                uint32_t PathFid,
                                bool IsDir,
                                bool Write,
                                VIO9P_FILE_CONTEXT **PContext,
                                std::shared_ptr<FID_REF> DirRef = nullptr)
{
    std::unique_ptr<VIO9P_FILE_CONTEXT> ctx(new (std::nothrow) VIO9P_FILE_CONTEXT);
    NTSTATUS status;

    if (!ctx)
    {
        if (!DirRef)
        {
            Fs->QueueClunk(PathFid);
        }
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    ctx->IsDir = IsDir;
    if (IsDir)
    {
        // Opened for reading only if the listing is not cached (EnsureDirOpen).
        if (!DirRef)
        {
            try
            {
                DirRef = std::make_shared<FID_REF>(Fs, PathFid);
            }
            catch (std::bad_alloc &)
            {
                Fs->QueueClunk(PathFid);
                return STATUS_INSUFFICIENT_RESOURCES;
            }
        }
        ctx->DirRef = std::move(DirRef);
        ctx->DirFid = PathFid;
    }
    else
    {
        status = Fs->Client.Lopen(PathFid, Write ? p9::L_O_RDWR : p9::L_O_RDONLY, &ctx->Iounit);
        if (!NT_SUCCESS(status))
        {
            Fs->QueueClunk(PathFid);
            return status;
        }
        ctx->Fid = PathFid;
    }

    *PContext = ctx.release();
    return STATUS_SUCCESS;
}

// Opens a directory for Treaddir on first use.
static NTSTATUS EnsureDirOpen(VIO9P *Fs, VIO9P_FILE_CONTEXT *Ctx)
{
    NTSTATUS status = STATUS_SUCCESS;

    AcquireSRWLockExclusive(&Ctx->OpenLock);
    if (Ctx->Fid == p9::NOFID)
    {
        uint32_t ioFid = Fs->Client.NewFid();
        size_t failedAt;

        status = Fs->Client.Walk(Ctx->DirFid, ioFid, {}, nullptr, &failedAt);
        if (NT_SUCCESS(status))
        {
            status = Fs->Client.Lopen(ioFid, p9::L_O_RDONLY, &Ctx->Iounit);
            if (NT_SUCCESS(status))
            {
                Ctx->Fid = ioFid;
            }
            else
            {
                Fs->QueueClunk(ioFid);
            }
        }
    }
    ReleaseSRWLockExclusive(&Ctx->OpenLock);
    return status;
}

static void FreeFileContext(VIO9P *Fs, VIO9P_FILE_CONTEXT *Ctx)
{
    if (Ctx->Fid != p9::NOFID)
    {
        Fs->QueueClunk(Ctx->Fid);
    }
    Ctx->DirRef.reset(); // clunks DirFid unless FidCache still holds it
    FspFileSystemDeleteDirectoryBuffer(&Ctx->DirBuffer);
    delete Ctx;
}

//
// WinFsp file system operations
//

static NTSTATUS GetVolumeInfo(FSP_FILE_SYSTEM *FileSystem, FSP_FSCTL_VOLUME_INFO *VolumeInfo)
{
    VIO9P *Fs = (VIO9P *)FileSystem->UserContext;
    p9::StatFs st;
    NTSTATUS status = Fs->Client.Statfs(Fs->RootFid, &st);

    if (!NT_SUCCESS(status))
    {
        return status;
    }

    VolumeInfo->TotalSize = st.blocks * st.bsize;
    VolumeInfo->FreeSize = st.bavail * st.bsize;

    size_t len = min(Fs->VolumeLabel.size(), sizeof(VolumeInfo->VolumeLabel) / sizeof(WCHAR));
    memcpy(VolumeInfo->VolumeLabel, Fs->VolumeLabel.c_str(), len * sizeof(WCHAR));
    VolumeInfo->VolumeLabelLength = (UINT16)(len * sizeof(WCHAR));

    return STATUS_SUCCESS;
}

// Looks up FileName: its attributes and, for a directory, a walked fid shared
// with FidCache. For a file, *PFid receives a walked fid the caller owns
// (only if PFid is not null; otherwise it is clunked).
static NTSTATUS LookupPath(VIO9P *Fs,
                           PCWSTR FileName,
                           p9::Attr *Attr,
                           std::shared_ptr<FID_REF> *PDirRef,
                           uint32_t *PFid)
{
    bool isRoot = (FileName[0] == L'\\' && FileName[1] == L'\0');
    std::wstring key;
    NTSTATUS status;

    PDirRef->reset();
    if (PFid != nullptr)
    {
        *PFid = p9::NOFID;
    }

    try
    {
        if (!isRoot)
        {
            key = PrefixKey(FileName, (size_t)-1);
        }
    }
    catch (std::bad_alloc &)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    // A cached directory: no walk.
    if (!key.empty())
    {
        std::shared_ptr<FID_REF> ref = Fs->FidCacheGet(key);
        if (ref)
        {
            if (Fs->CacheGet(FileName, Attr))
            {
                status = STATUS_SUCCESS;
            }
            else
            {
                status = Fs->Client.Getattr(ref->Fid, Attr);
                if (NT_SUCCESS(status))
                {
                    Fs->CachePut(FileName, *Attr);
                }
            }
            if (NT_SUCCESS(status) && Attr->IsDir())
            {
                *PDirRef = std::move(ref);
                return STATUS_SUCCESS;
            }
            // Gone or replaced on the host: forget it and walk again.
            Fs->FidCacheDrop(key);
        }
    }

    uint32_t fid;
    status = Fs->WalkPath(FileName, &fid);
    if (!NT_SUCCESS(status))
    {
        return status;
    }
    if (!Fs->CacheGet(FileName, Attr))
    {
        status = Fs->Client.Getattr(fid, Attr);
        if (!NT_SUCCESS(status))
        {
            Fs->QueueClunk(fid);
            return status;
        }
        Fs->CachePut(FileName, *Attr);
    }

    if (Attr->IsDir())
    {
        try
        {
            *PDirRef = std::make_shared<FID_REF>(Fs, fid);
        }
        catch (std::bad_alloc &)
        {
            Fs->QueueClunk(fid);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        if (!key.empty())
        {
            Fs->FidCachePut(key, *PDirRef);
        }
    }
    else if (PFid != nullptr)
    {
        *PFid = fid;
    }
    else
    {
        Fs->QueueClunk(fid);
    }
    return STATUS_SUCCESS;
}

static NTSTATUS GetSecurityByName(FSP_FILE_SYSTEM *FileSystem,
                                  PWSTR FileName,
                                  PUINT32 PFileAttributes,
                                  PSECURITY_DESCRIPTOR SecurityDescriptor,
                                  SIZE_T *PSecurityDescriptorSize)
{
    VIO9P *Fs = (VIO9P *)FileSystem->UserContext;
    p9::Attr attr;

    if (!Fs->CacheGet(FileName, &attr))
    {
        std::shared_ptr<FID_REF> dirRef;
        NTSTATUS status = LookupPath(Fs, FileName, &attr, &dirRef, nullptr);
        if (!NT_SUCCESS(status))
        {
            return status;
        }
    }

    if (PFileAttributes != nullptr)
    {
        *PFileAttributes = AttrToFileAttributes(attr);
    }

    return CopyDefaultSecurity(Fs, SecurityDescriptor, PSecurityDescriptorSize);
}

static NTSTATUS Create(FSP_FILE_SYSTEM *FileSystem,
                       PWSTR FileName,
                       UINT32 CreateOptions,
                       UINT32 GrantedAccess,
                       UINT32 FileAttributes,
                       PSECURITY_DESCRIPTOR SecurityDescriptor,
                       UINT64 AllocationSize,
                       PVOID *PFileContext,
                       FSP_FSCTL_FILE_INFO *FileInfo)
{
    VIO9P *Fs = (VIO9P *)FileSystem->UserContext;
    VIO9P_FILE_CONTEXT *ctx = nullptr;
    uint32_t parentFid;
    std::string leaf;

    UNREFERENCED_PARAMETER(GrantedAccess);
    UNREFERENCED_PARAMETER(FileAttributes);
    UNREFERENCED_PARAMETER(SecurityDescriptor);
    UNREFERENCED_PARAMETER(AllocationSize);

    Fs->CacheClear();
    NTSTATUS status = Fs->WalkParent(FileName, &parentFid, &leaf);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    if (CreateOptions & FILE_DIRECTORY_FILE)
    {
        uint32_t dirFid = Fs->Client.NewFid();
        size_t failedAt;

        status = Fs->Client.Mkdir(parentFid, leaf, DEFAULT_DIR_MODE);
        if (NT_SUCCESS(status))
        {
            status = Fs->Client.Walk(parentFid, dirFid, {leaf}, nullptr, &failedAt);
        }
        Fs->QueueClunk(parentFid);
        if (!NT_SUCCESS(status))
        {
            return status;
        }
        status = OpenFileContext(Fs, dirFid, true, false, &ctx);
    }
    else
    {
        ctx = new (std::nothrow) VIO9P_FILE_CONTEXT;
        if (ctx == nullptr)
        {
            Fs->QueueClunk(parentFid);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        // Tlcreate turns parentFid into the newly created, opened file.
        status = Fs->Client.Lcreate(parentFid, leaf, p9::L_O_RDWR | p9::L_O_EXCL, DEFAULT_FILE_MODE, &ctx->Iounit);
        if (!NT_SUCCESS(status))
        {
            Fs->QueueClunk(parentFid);
            delete ctx;
            return status;
        }
        ctx->Fid = parentFid;
    }

    if (!NT_SUCCESS(status))
    {
        return status;
    }

    status = GetFileInfoByFid(Fs, MetaFid(ctx), FileInfo);
    if (!NT_SUCCESS(status))
    {
        FreeFileContext(Fs, ctx);
        return status;
    }

    try
    {
        ctx->Path = FileName;
        Fs->WatchForget(ParentKey(VIO9P::CacheKey(ctx->Path)));
        if (ctx->IsDir)
        {
            ctx->WatchKey = VIO9P::CacheKey(ctx->Path);
            Fs->WatchOpen(ctx->WatchKey, ctx->Path);
        }
    }
    catch (std::bad_alloc &)
    {
    }
    *PFileContext = ctx;
    return STATUS_SUCCESS;
}

static NTSTATUS Open(FSP_FILE_SYSTEM *FileSystem,
                     PWSTR FileName,
                     UINT32 CreateOptions,
                     UINT32 GrantedAccess,
                     PVOID *PFileContext,
                     FSP_FSCTL_FILE_INFO *FileInfo)
{
    VIO9P *Fs = (VIO9P *)FileSystem->UserContext;
    VIO9P_FILE_CONTEXT *ctx = nullptr;
    uint32_t fid;
    p9::Attr attr;
    std::shared_ptr<FID_REF> dirRef;

    UNREFERENCED_PARAMETER(CreateOptions);

    NTSTATUS status = LookupPath(Fs, FileName, &attr, &dirRef, &fid);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    bool write = (GrantedAccess & (FILE_WRITE_DATA | FILE_APPEND_DATA)) != 0;
    if (dirRef)
    {
        fid = dirRef->Fid;
    }
    status = OpenFileContext(Fs, fid, attr.IsDir(), write, &ctx, std::move(dirRef));
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    ctx->IsRoot = (FileName[0] == L'\\' && FileName[1] == L'\0');
    try
    {
        ctx->Path = FileName;
        if (ctx->IsDir)
        {
            ctx->WatchKey = VIO9P::CacheKey(ctx->Path);
            Fs->WatchOpen(ctx->WatchKey, ctx->Path);
        }
    }
    catch (std::bad_alloc &)
    {
    }
    FillFileInfo(attr, FileInfo);
    *PFileContext = ctx;
    return STATUS_SUCCESS;
}

static NTSTATUS Overwrite(FSP_FILE_SYSTEM *FileSystem,
                          PVOID FileContext,
                          UINT32 FileAttributes,
                          BOOLEAN ReplaceFileAttributes,
                          UINT64 AllocationSize,
                          FSP_FSCTL_FILE_INFO *FileInfo)
{
    VIO9P *Fs = (VIO9P *)FileSystem->UserContext;
    VIO9P_FILE_CONTEXT *ctx = (VIO9P_FILE_CONTEXT *)FileContext;

    UNREFERENCED_PARAMETER(FileAttributes);
    UNREFERENCED_PARAMETER(ReplaceFileAttributes);
    UNREFERENCED_PARAMETER(AllocationSize);

    Fs->CacheClear();
    NTSTATUS status = Fs->Client.Setattr(MetaFid(ctx), p9::SETATTR_SIZE, 0, 0, 0, 0, 0, 0);
    if (!NT_SUCCESS(status))
    {
        return status;
    }
    return GetFileInfoByFid(Fs, MetaFid(ctx), FileInfo);
}

static VOID Cleanup(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext, PWSTR FileName, ULONG Flags)
{
    VIO9P *Fs = (VIO9P *)FileSystem->UserContext;
    VIO9P_FILE_CONTEXT *ctx = (VIO9P_FILE_CONTEXT *)FileContext;

    // With PostCleanupWhenModifiedOnly, Cleanup only arrives for files that
    // were changed or deleted.
    Fs->CacheClear();

    if ((Flags & FspCleanupDelete) && FileName != nullptr)
    {
        uint32_t parentFid;

        if (ctx->IsDir)
        {
            Fs->FidCacheClear();
        }
        try
        {
            Fs->WatchForget(ParentKey(VIO9P::CacheKey(FileName)));
        }
        catch (std::bad_alloc &)
        {
        }
        std::string leaf;

        if (NT_SUCCESS(Fs->WalkParent(FileName, &parentFid, &leaf)))
        {
            NTSTATUS status = Fs->Client.Unlinkat(parentFid, leaf, ctx->IsDir ? p9::L_AT_REMOVEDIR : 0);
            if (!NT_SUCCESS(status))
            {
                DBG("unlink '%S' failed: 0x%08x", FileName, status);
            }
            Fs->QueueClunk(parentFid);
        }
    }
}

static VOID Close(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext)
{
    VIO9P *Fs = (VIO9P *)FileSystem->UserContext;
    VIO9P_FILE_CONTEXT *ctx = (VIO9P_FILE_CONTEXT *)FileContext;
    if (!ctx->WatchKey.empty())
    {
        Fs->WatchClose(ctx->WatchKey);
    }
    FreeFileContext(Fs, ctx);
}

static uint32_t IoChunk(VIO9P *Fs, VIO9P_FILE_CONTEXT *Ctx)
{
    uint32_t chunk = Fs->Client.MaxIo();
    if (Ctx->Iounit != 0 && Ctx->Iounit < chunk)
    {
        chunk = Ctx->Iounit;
    }
    return chunk;
}

static NTSTATUS Read(FSP_FILE_SYSTEM *FileSystem,
                     PVOID FileContext,
                     PVOID Buffer,
                     UINT64 Offset,
                     ULONG Length,
                     PULONG PBytesTransferred)
{
    VIO9P *Fs = (VIO9P *)FileSystem->UserContext;
    VIO9P_FILE_CONTEXT *ctx = (VIO9P_FILE_CONTEXT *)FileContext;
    uint32_t chunk = IoChunk(Fs, ctx);
    ULONG total = 0;

    while (total < Length)
    {
        uint32_t want = min(chunk, Length - total);
        uint32_t got = 0;
        NTSTATUS status = Fs->Client.Read(ctx->Fid, Offset + total, want, (PUINT8)Buffer + total, &got);
        if (!NT_SUCCESS(status))
        {
            if (total == 0)
            {
                return status;
            }
            break;
        }
        total += got;
        if (got < want)
        {
            break;
        }
    }

    *PBytesTransferred = total;
    return (total == 0 && Length != 0) ? STATUS_END_OF_FILE : STATUS_SUCCESS;
}

static NTSTATUS Write(FSP_FILE_SYSTEM *FileSystem,
                      PVOID FileContext,
                      PVOID Buffer,
                      UINT64 Offset,
                      ULONG Length,
                      BOOLEAN WriteToEndOfFile,
                      BOOLEAN ConstrainedIo,
                      PULONG PBytesTransferred,
                      FSP_FSCTL_FILE_INFO *FileInfo)
{
    VIO9P *Fs = (VIO9P *)FileSystem->UserContext;
    VIO9P_FILE_CONTEXT *ctx = (VIO9P_FILE_CONTEXT *)FileContext;
    uint32_t chunk = IoChunk(Fs, ctx);
    ULONG total = 0;
    NTSTATUS status;

    Fs->CacheClear();
    if (WriteToEndOfFile || ConstrainedIo)
    {
        p9::Attr attr;
        status = Fs->Client.Getattr(MetaFid(ctx), &attr);
        if (!NT_SUCCESS(status))
        {
            return status;
        }
        if (ConstrainedIo)
        {
            if (Offset >= attr.size)
            {
                *PBytesTransferred = 0;
                FillFileInfo(attr, FileInfo);
                return STATUS_SUCCESS;
            }
            if (Offset + Length > attr.size)
            {
                Length = (ULONG)(attr.size - Offset);
            }
        }
        else
        {
            Offset = attr.size;
        }
    }

    while (total < Length)
    {
        uint32_t want = min(chunk, Length - total);
        uint32_t wrote = 0;
        status = Fs->Client.Write(ctx->Fid, Offset + total, (PUINT8)Buffer + total, want, &wrote);
        if (!NT_SUCCESS(status))
        {
            if (total == 0)
            {
                return status;
            }
            break;
        }
        total += wrote;
        if (wrote < want)
        {
            break;
        }
    }

    *PBytesTransferred = total;
    return GetFileInfoByFid(Fs, MetaFid(ctx), FileInfo);
}

static NTSTATUS Flush(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext, FSP_FSCTL_FILE_INFO *FileInfo)
{
    VIO9P *Fs = (VIO9P *)FileSystem->UserContext;
    VIO9P_FILE_CONTEXT *ctx = (VIO9P_FILE_CONTEXT *)FileContext;

    if (ctx == nullptr)
    {
        return STATUS_SUCCESS; // volume flush
    }
    if (!ctx->IsDir)
    {
        NTSTATUS status = Fs->Client.Fsync(ctx->Fid);
        if (!NT_SUCCESS(status))
        {
            return status;
        }
    }
    return GetFileInfoByFid(Fs, MetaFid(ctx), FileInfo);
}

static NTSTATUS GetFileInfo(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext, FSP_FSCTL_FILE_INFO *FileInfo)
{
    VIO9P_FILE_CONTEXT *ctx = (VIO9P_FILE_CONTEXT *)FileContext;
    return GetFileInfoByFid((VIO9P *)FileSystem->UserContext, MetaFid(ctx), FileInfo);
}

static NTSTATUS SetBasicInfo(FSP_FILE_SYSTEM *FileSystem,
                             PVOID FileContext,
                             UINT32 FileAttributes,
                             UINT64 CreationTime,
                             UINT64 LastAccessTime,
                             UINT64 LastWriteTime,
                             UINT64 ChangeTime,
                             FSP_FSCTL_FILE_INFO *FileInfo)
{
    VIO9P *Fs = (VIO9P *)FileSystem->UserContext;
    VIO9P_FILE_CONTEXT *ctx = (VIO9P_FILE_CONTEXT *)FileContext;
    uint64_t aSec = 0, aNsec = 0, mSec = 0, mNsec = 0;
    uint32_t valid = 0;

    UNREFERENCED_PARAMETER(FileAttributes);
    UNREFERENCED_PARAMETER(CreationTime);
    UNREFERENCED_PARAMETER(ChangeTime);

    if (LastAccessTime != 0)
    {
        FileTimeToUnix(LastAccessTime, &aSec, &aNsec);
        valid |= p9::SETATTR_ATIME | p9::SETATTR_ATIME_SET;
    }
    if (LastWriteTime != 0)
    {
        FileTimeToUnix(LastWriteTime, &mSec, &mNsec);
        valid |= p9::SETATTR_MTIME | p9::SETATTR_MTIME_SET;
    }
    if (valid != 0)
    {
        Fs->CacheClear();
        NTSTATUS status = Fs->Client.Setattr(MetaFid(ctx), valid, 0, 0, aSec, aNsec, mSec, mNsec);
        if (!NT_SUCCESS(status))
        {
            return status;
        }
    }
    return GetFileInfoByFid(Fs, MetaFid(ctx), FileInfo);
}

static NTSTATUS SetFileSize(FSP_FILE_SYSTEM *FileSystem,
                            PVOID FileContext,
                            UINT64 NewSize,
                            BOOLEAN SetAllocationSize,
                            FSP_FSCTL_FILE_INFO *FileInfo)
{
    VIO9P *Fs = (VIO9P *)FileSystem->UserContext;
    VIO9P_FILE_CONTEXT *ctx = (VIO9P_FILE_CONTEXT *)FileContext;
    NTSTATUS status;

    if (SetAllocationSize)
    {
        // Allocation is managed by the host; only shrinking it truncates data.
        p9::Attr attr;
        status = Fs->Client.Getattr(MetaFid(ctx), &attr);
        if (!NT_SUCCESS(status))
        {
            return status;
        }
        if (NewSize >= attr.size)
        {
            FillFileInfo(attr, FileInfo);
            return STATUS_SUCCESS;
        }
    }

    Fs->CacheClear();
    status = Fs->Client.Setattr(MetaFid(ctx), p9::SETATTR_SIZE, 0, NewSize, 0, 0, 0, 0);
    if (!NT_SUCCESS(status))
    {
        return status;
    }
    return GetFileInfoByFid(Fs, MetaFid(ctx), FileInfo);
}

static bool IsDotName(const std::string &Name)
{
    return Name == "." || Name == "..";
}

static NTSTATUS CanDelete(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext, PWSTR FileName)
{
    VIO9P *Fs = (VIO9P *)FileSystem->UserContext;
    VIO9P_FILE_CONTEXT *ctx = (VIO9P_FILE_CONTEXT *)FileContext;

    UNREFERENCED_PARAMETER(FileName);

    if (ctx->IsRoot)
    {
        return STATUS_ACCESS_DENIED;
    }
    if (!ctx->IsDir)
    {
        return STATUS_SUCCESS;
    }

    std::vector<p9::DirEntry> entries;
    uint64_t offset = 0;
    NTSTATUS openStatus = EnsureDirOpen(Fs, ctx);
    if (!NT_SUCCESS(openStatus))
    {
        return openStatus;
    }
    for (;;)
    {
        NTSTATUS status = Fs->Client.Readdir(ctx->Fid, offset, entries);
        if (!NT_SUCCESS(status))
        {
            return status;
        }
        if (entries.empty())
        {
            return STATUS_SUCCESS;
        }
        for (const auto &e : entries)
        {
            if (!IsDotName(e.name))
            {
                return STATUS_DIRECTORY_NOT_EMPTY;
            }
            offset = e.offset;
        }
    }
}

static NTSTATUS Rename(FSP_FILE_SYSTEM *FileSystem,
                       PVOID FileContext,
                       PWSTR FileName,
                       PWSTR NewFileName,
                       BOOLEAN ReplaceIfExists)
{
    VIO9P *Fs = (VIO9P *)FileSystem->UserContext;
    VIO9P_FILE_CONTEXT *ctx = (VIO9P_FILE_CONTEXT *)FileContext;
    uint32_t oldParent, newParent;
    std::string oldLeaf, newLeaf;

    Fs->CacheClear();
    // QEMU moves the paths of fids under a renamed directory.
    Fs->FidCacheClear();
    try
    {
        Fs->WatchForget(ParentKey(VIO9P::CacheKey(FileName)));
        Fs->WatchForget(ParentKey(VIO9P::CacheKey(NewFileName)));
    }
    catch (std::bad_alloc &)
    {
    }
    NTSTATUS status = Fs->WalkParent(FileName, &oldParent, &oldLeaf);
    if (!NT_SUCCESS(status))
    {
        return status;
    }
    status = Fs->WalkParent(NewFileName, &newParent, &newLeaf);
    if (!NT_SUCCESS(status))
    {
        Fs->QueueClunk(oldParent);
        return status;
    }

    if (!ReplaceIfExists)
    {
        uint32_t probe = Fs->Client.NewFid();
        p9::Qid targetQid;
        size_t failedAt;

        if (NT_SUCCESS(Fs->Client.Walk(newParent, probe, {newLeaf}, &targetQid, &failedAt)))
        {
            Fs->QueueClunk(probe);

            // On a case-insensitive host a case-only rename finds the file itself.
            p9::Attr self;
            if (!NT_SUCCESS(Fs->Client.Getattr(MetaFid(ctx), &self)) || self.qid.path != targetQid.path)
            {
                Fs->QueueClunk(oldParent);
                Fs->QueueClunk(newParent);
                return STATUS_OBJECT_NAME_COLLISION;
            }
        }
    }

    status = Fs->Client.Renameat(oldParent, oldLeaf, newParent, newLeaf);
    Fs->QueueClunk(oldParent);
    Fs->QueueClunk(newParent);
    return status;
}

static NTSTATUS GetSecurity(FSP_FILE_SYSTEM *FileSystem,
                            PVOID FileContext,
                            PSECURITY_DESCRIPTOR SecurityDescriptor,
                            SIZE_T *PSecurityDescriptorSize)
{
    UNREFERENCED_PARAMETER(FileContext);
    return CopyDefaultSecurity((VIO9P *)FileSystem->UserContext, SecurityDescriptor, PSecurityDescriptorSize);
}

static NTSTATUS SetSecurity(FSP_FILE_SYSTEM *FileSystem,
                            PVOID FileContext,
                            SECURITY_INFORMATION SecurityInformation,
                            PSECURITY_DESCRIPTOR ModificationDescriptor)
{
    UNREFERENCED_PARAMETER(FileSystem);
    UNREFERENCED_PARAMETER(FileContext);
    UNREFERENCED_PARAMETER(SecurityInformation);
    UNREFERENCED_PARAMETER(ModificationDescriptor);
    // Windows ACLs are not stored on the host.
    return STATUS_SUCCESS;
}

static BOOLEAN AddDirEntry(VIO9P_FILE_CONTEXT *Ctx, const std::string &Name, const p9::Attr &Attr, NTSTATUS *Status)
{
    alignas(8) UINT8 buf[FIELD_OFFSET(FSP_FSCTL_DIR_INFO, FileNameBuf) + 256 * sizeof(WCHAR)];
    FSP_FSCTL_DIR_INFO *dirInfo = (FSP_FSCTL_DIR_INFO *)buf;

    int len = Utf8ToWide(Name, dirInfo->FileNameBuf, 255);
    if (len <= 0)
    {
        return TRUE; // skip names that cannot be represented
    }

    ZeroMemory(dirInfo, FIELD_OFFSET(FSP_FSCTL_DIR_INFO, FileNameBuf));
    dirInfo->Size = (UINT16)(FIELD_OFFSET(FSP_FSCTL_DIR_INFO, FileNameBuf) + len * sizeof(WCHAR));
    FillFileInfo(Attr, &dirInfo->FileInfo);

    return FspFileSystemFillDirectoryBuffer(&Ctx->DirBuffer, dirInfo, Status);
}

// Looks up attributes of directory entries with several requests in flight.
// 9P2000.L has no "readdir with attributes", so every entry needs a walk, a
// getattr and a clunk; running them concurrently hides the round-trip time.
#define DIR_LOOKUP_WORKERS 8

struct DIR_LOOKUP
{
    VIO9P *Fs;
    uint32_t DirFid;
    const std::vector<p9::DirEntry> *Entries;
    std::vector<p9::Attr> *Attrs;
    std::vector<char> *Found;
    volatile LONG Next;
};

static DWORD WINAPI DirLookupWorker(PVOID Param)
{
    DIR_LOOKUP *work = (DIR_LOOKUP *)Param;
    LONG count = (LONG)work->Entries->size();

    for (;;)
    {
        LONG i = InterlockedIncrement(&work->Next) - 1;
        if (i >= count)
        {
            break;
        }
        const p9::DirEntry &e = (*work->Entries)[i];
        if (IsDotName(e.name))
        {
            continue;
        }

        uint32_t fid = work->Fs->Client.NewFid();
        size_t failedAt;
        if (NT_SUCCESS(work->Fs->Client.Walk(work->DirFid, fid, {e.name}, nullptr, &failedAt)))
        {
            (*work->Found)[i] = NT_SUCCESS(work->Fs->Client.Getattr(fid, &(*work->Attrs)[i]));
            work->Fs->QueueClunk(fid);
        }
    }
    return 0;
}

static void LookupDirEntries(VIO9P *Fs,
                             uint32_t DirFid,
                             const std::vector<p9::DirEntry> &Entries,
                             std::vector<p9::Attr> &Attrs,
                             std::vector<char> &Found)
{
    DIR_LOOKUP work = {Fs, DirFid, &Entries, &Attrs, &Found, 0};
    HANDLE threads[DIR_LOOKUP_WORKERS - 1];
    DWORD started = 0;

    Attrs.assign(Entries.size(), p9::Attr());
    Found.assign(Entries.size(), 0);

    // The calling thread is one worker; add more only when it pays off.
    DWORD extra = (DWORD)min(Entries.size() / 4, (size_t)(DIR_LOOKUP_WORKERS - 1));
    for (DWORD i = 0; i < extra; i++)
    {
        threads[started] = CreateThread(NULL, 0, DirLookupWorker, &work, 0, NULL);
        if (threads[started] != NULL)
        {
            started++;
        }
    }

    DirLookupWorker(&work);

    if (started != 0)
    {
        WaitForMultipleObjects(started, threads, TRUE, INFINITE);
        for (DWORD i = 0; i < started; i++)
        {
            CloseHandle(threads[i]);
        }
    }
}

// Reads a whole directory from the host: entry names via Treaddir, then their
// attributes with parallel lookups. Also seeds the per-path attribute cache.
static NTSTATUS ListDirectory(VIO9P *Fs, VIO9P_FILE_CONTEXT *Ctx, std::vector<VIO9P::DIR_ITEM> &Items)
{
    p9::Attr self;
    std::vector<p9::DirEntry> entries;
    uint64_t offset = 0;

    NTSTATUS status = EnsureDirOpen(Fs, Ctx);
    if (NT_SUCCESS(status))
    {
        status = Fs->Client.Getattr(MetaFid(Ctx), &self);
    }

    while (NT_SUCCESS(status))
    {
        status = Fs->Client.Readdir(Ctx->Fid, offset, entries);
        if (!NT_SUCCESS(status) || entries.empty())
        {
            break;
        }

        std::vector<p9::Attr> attrs;
        std::vector<char> found;
        LookupDirEntries(Fs, Ctx->DirFid, entries, attrs, found);

        for (size_t i = 0; i < entries.size(); i++)
        {
            const auto &e = entries[i];
            offset = e.offset;

            if (IsDotName(e.name))
            {
                if (!Ctx->IsRoot)
                {
                    Items.push_back(VIO9P::DIR_ITEM{e.name, self});
                }
                continue;
            }
            if (!found[i])
            {
                continue; // vanished meanwhile
            }

            Items.push_back(VIO9P::DIR_ITEM{e.name, attrs[i]});

            if (!Ctx->Path.empty())
            {
                WCHAR wname[256];
                int len = Utf8ToWide(e.name, wname, 255);
                if (len > 0)
                {
                    std::wstring path = Ctx->Path;
                    if (path.back() != L'\\')
                    {
                        path += L'\\';
                    }
                    path.append(wname, len);
                    Fs->CachePut(path, attrs[i]);
                }
            }
        }
    }
    return status;
}

static NTSTATUS ReadDirectory(FSP_FILE_SYSTEM *FileSystem,
                              PVOID FileContext,
                              PWSTR Pattern,
                              PWSTR Marker,
                              PVOID Buffer,
                              ULONG BufferLength,
                              PULONG PBytesTransferred)
{
    VIO9P *Fs = (VIO9P *)FileSystem->UserContext;
    VIO9P_FILE_CONTEXT *ctx = (VIO9P_FILE_CONTEXT *)FileContext;
    NTSTATUS status = STATUS_SUCCESS;

    UNREFERENCED_PARAMETER(Pattern);

    if (FspFileSystemAcquireDirectoryBuffer(&ctx->DirBuffer, Marker == NULL, &status))
    {
        std::shared_ptr<const VIO9P::DIR_LISTING> listing = Fs->DirCacheGet(ctx->Path);
        if (!listing)
        {
            std::shared_ptr<VIO9P::DIR_LISTING> fresh;
            try
            {
                fresh = std::make_shared<VIO9P::DIR_LISTING>();
                status = ListDirectory(Fs, ctx, fresh->Items);
            }
            catch (std::bad_alloc &)
            {
                status = STATUS_INSUFFICIENT_RESOURCES;
            }
            if (NT_SUCCESS(status))
            {
                fresh->Tick = GetTickCount64();
                Fs->DirCachePut(ctx->Path, fresh);
                listing = fresh;
            }
        }

        if (listing)
        {
            for (const auto &item : listing->Items)
            {
                if (!AddDirEntry(ctx, item.Name, item.Attr, &status))
                {
                    break;
                }
            }
            if (!ctx->WatchKey.empty())
            {
                try
                {
                    std::vector<std::string> names;
                    names.reserve(listing->Items.size());
                    for (const auto &item : listing->Items)
                    {
                        if (!IsDotName(item.Name))
                        {
                            names.push_back(item.Name);
                        }
                    }
                    std::sort(names.begin(), names.end());
                    Fs->WatchListed(ctx->WatchKey, ctx->Path, names);
                }
                catch (std::bad_alloc &)
                {
                }
            }
        }

        FspFileSystemReleaseDirectoryBuffer(&ctx->DirBuffer);
    }

    if (!NT_SUCCESS(status))
    {
        return status;
    }

    FspFileSystemReadDirectoryBuffer(&ctx->DirBuffer, Marker, Buffer, BufferLength, PBytesTransferred);
    return STATUS_SUCCESS;
}

// clang-format off
static FSP_FILE_SYSTEM_INTERFACE Vio9pInterface =
{
    .GetVolumeInfo = GetVolumeInfo,
    .GetSecurityByName = GetSecurityByName,
    .Create = Create,
    .Open = Open,
    .Overwrite = Overwrite,
    .Cleanup = Cleanup,
    .Close = Close,
    .Read = Read,
    .Write = Write,
    .Flush = Flush,
    .GetFileInfo = GetFileInfo,
    .SetBasicInfo = SetBasicInfo,
    .SetFileSize = SetFileSize,
    .CanDelete = CanDelete,
    .Rename = Rename,
    .GetSecurity = GetSecurity,
    .SetSecurity = SetSecurity,
    .ReadDirectory = ReadDirectory,
};
// clang-format on

//
// Device and mount management
//

#define MOUNT_RETRY_MS       3000
#define HANDSHAKE_TIMEOUT_MS 10000
#define REQUEST_TIMEOUT_MS   60000

static NTSTATUS GetMountTag(HANDLE Device, PWSTR Tag, DWORD TagSize)
{
    DWORD returned = 0;
    ZeroMemory(Tag, TagSize);
    return p9::IoctlWithTimeout(Device, IOCTL_VIO9P_GET_MOUNT_TAG, NULL, 0, Tag, TagSize - sizeof(WCHAR), &returned, 5000);
}

// Opens the first vio9p device (or the one whose mount tag equals Tag) for
// overlapped I/O, so that every request can time out.
DWORD VIO9P::FindDeviceInterface()
{
    HDEVINFO devInfo = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_VIO9P, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devInfo == INVALID_HANDLE_VALUE)
    {
        return GetLastError();
    }

    DWORD error = ERROR_NO_MORE_ITEMS;
    for (DWORD index = 0;; index++)
    {
        SP_DEVICE_INTERFACE_DATA ifData;
        ZeroMemory(&ifData, sizeof(ifData));
        ifData.cbSize = sizeof(ifData);
        if (!SetupDiEnumDeviceInterfaces(devInfo, NULL, &GUID_DEVINTERFACE_VIO9P, index, &ifData))
        {
            break;
        }

        DWORD required = 0;
        SetupDiGetDeviceInterfaceDetailW(devInfo, &ifData, NULL, 0, &required, NULL);
        if (required < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W))
        {
            continue;
        }
        std::vector<BYTE> buf(required);
        auto detail = (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)buf.data();
        detail->cbSize = sizeof(*detail);
        if (!SetupDiGetDeviceInterfaceDetailW(devInfo, &ifData, detail, required, NULL, NULL))
        {
            continue;
        }

        HANDLE h = CreateFileW(detail->DevicePath,
                               GENERIC_READ | GENERIC_WRITE,
                               0,
                               NULL,
                               OPEN_EXISTING,
                               FILE_FLAG_OVERLAPPED,
                               NULL);
        if (h == INVALID_HANDLE_VALUE)
        {
            continue;
        }

        if (!Tag.empty())
        {
            WCHAR mountTag[VIO9P_MAX_TAG_LEN + 1];
            if (!NT_SUCCESS(GetMountTag(h, mountTag, sizeof(mountTag))) || Tag != mountTag)
            {
                CloseHandle(h);
                continue;
            }
        }

        Device = h;
        error = ERROR_SUCCESS;
        break;
    }

    SetupDiDestroyDeviceInfoList(devInfo);
    return error;
}

// Explorer keeps its own list of drives and only learns about new ones from
// notifications in its own logon session, which a service in session 0 cannot
// send. So for each logged-on session we briefly run this executable as that
// session's user with "--notify-drive X: add|remove"; it sends the shell and
// WM_DEVICECHANGE notifications from inside the session and exits.
static void SendDriveNotifications(WCHAR Drive, bool Arrival);

// Helper mode: wait until this session's shell (taskbar) exists so the
// notification is not lost while Explorer is still starting, then notify, and
// repeat once in case Explorer was not fully initialized yet.
static int NotifyDriveInThisSession(WCHAR Drive, bool Arrival)
{
    if (Arrival)
    {
        for (int i = 0; i < 240 && FindWindowW(L"Shell_TrayWnd", NULL) == NULL; i++)
        {
            Sleep(500); // up to 2 minutes
        }
        Sleep(2000);
    }

    SendDriveNotifications(Drive, Arrival);
    if (Arrival)
    {
        Sleep(8000);
        SendDriveNotifications(Drive, Arrival);
    }
    return 0;
}

static void SendDriveNotifications(WCHAR Drive, bool Arrival)
{
    WCHAR root[] = L"X:\\";
    root[0] = Drive;

    DEV_BROADCAST_VOLUME volume;
    ZeroMemory(&volume, sizeof(volume));
    volume.dbcv_size = sizeof(volume);
    volume.dbcv_devicetype = DBT_DEVTYP_VOLUME;
    volume.dbcv_unitmask = 1u << (Drive - L'A');

    DWORD recipients = BSM_APPLICATIONS;
    BroadcastSystemMessageW(BSF_FORCEIFHUNG | BSF_NOHANG | BSF_NOTIMEOUTIFNOTHUNG,
                            &recipients,
                            WM_DEVICECHANGE,
                            Arrival ? DBT_DEVICEARRIVAL : DBT_DEVICEREMOVECOMPLETE,
                            (LPARAM)&volume);
    SHChangeNotify(Arrival ? SHCNE_DRIVEADD : SHCNE_DRIVEREMOVED, SHCNF_PATHW, root, NULL);
}

// Returns the drive letter of a mount point like "\\.\Z:" or "Z:", or 0.
static WCHAR MountPointDrive(PCWSTR MountPoint)
{
    if (MountPoint == NULL)
    {
        return 0;
    }
    if (wcsncmp(MountPoint, L"\\\\.\\", 4) == 0)
    {
        MountPoint += 4;
    }
    if (iswalpha(MountPoint[0]) && MountPoint[1] == L':' && MountPoint[2] == L'\0')
    {
        return (WCHAR)towupper(MountPoint[0]);
    }
    return 0;
}

// Notifies every logged-on session, or only OnlySession if it is not 0xFFFFFFFF.
static void NotifyDriveInUserSessions(WCHAR Drive, bool Arrival, DWORD OnlySession = 0xFFFFFFFF)
{
    PWTS_SESSION_INFOW sessions = NULL;
    DWORD count = 0;
    WCHAR exePath[MAX_PATH];

    if (Drive == 0 || GetModuleFileNameW(NULL, exePath, MAX_PATH) == 0)
    {
        return;
    }
    if (!WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &sessions, &count))
    {
        return;
    }

    for (DWORD i = 0; i < count; i++)
    {
        HANDLE token = NULL;

        // Session 0 has no user desktop; sessions without a logged-on user have no token.
        if (sessions[i].SessionId == 0 || (OnlySession != 0xFFFFFFFF && sessions[i].SessionId != OnlySession) ||
            !WTSQueryUserToken(sessions[i].SessionId, &token))
        {
            continue;
        }

        std::wstring cmd = L"\"" + std::wstring(exePath) + L"\" --notify-drive ";
        cmd += Drive;
        cmd += Arrival ? L": add" : L": remove";

        STARTUPINFOW si;
        PROCESS_INFORMATION pi;
        ZeroMemory(&si, sizeof(si));
        si.cb = sizeof(si);
        si.lpDesktop = (LPWSTR)L"winsta0\\default";

        if (CreateProcessAsUserW(token, NULL, &cmd[0], NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
        {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
        else
        {
            DBG("notify session %lu failed: %lu", sessions[i].SessionId, GetLastError());
        }
        CloseHandle(token);
    }

    WTSFreeMemory(sessions);
}

// Drive letters go through the Mount Manager ("\\.\X:" in WinFsp) so that they
// are global and visible in every logon session. A plain "X:" created by this
// LocalSystem service at boot was only visible to SYSTEM, not to the user.
static NTSTATUS SetMountPoint(FSP_FILE_SYSTEM *FileSystem, const std::wstring &MountPoint)
{
    WCHAR mountmgrDrive[] = L"\\\\.\\X:";
    bool anyDrive = (MountPoint == L"*");
    bool oneDrive = (MountPoint.size() == 2 && MountPoint[1] == L':' && iswalpha(MountPoint[0]));

    if (!anyDrive && !oneDrive)
    {
        return FspFileSystemSetMountPoint(FileSystem, (PWSTR)MountPoint.c_str()); // directory etc.
    }
    if (oneDrive)
    {
        mountmgrDrive[4] = (WCHAR)towupper(MountPoint[0]);
        return FspFileSystemSetMountPoint(FileSystem, mountmgrDrive);
    }

    // Next free letter, counting down from Z: like WinFsp does.
    DWORD drives = GetLogicalDrives();
    NTSTATUS status = STATUS_NO_SUCH_DEVICE;
    for (WCHAR drive = L'Z'; drive >= L'D'; drive--)
    {
        if (drives & (1u << (drive - L'A')))
        {
            continue;
        }
        mountmgrDrive[4] = drive;
        status = FspFileSystemSetMountPoint(FileSystem, mountmgrDrive);
        if (NT_SUCCESS(status))
        {
            break;
        }
    }
    return status;
}

NTSTATUS VIO9P::Start()
{
    FSP_FSCTL_VOLUME_PARAMS volumeParams;
    FILETIME fileTime;
    WCHAR mountTag[VIO9P_MAX_TAG_LEN + 1];
    p9::Qid rootQid;

    Client.Reset(Device);
    Client.TimeoutMs = HANDSHAKE_TIMEOUT_MS;

    NTSTATUS status = Client.Version(DEFAULT_MSIZE);
    if (!NT_SUCCESS(status))
    {
        DBG("9P version negotiation failed: 0x%08lx", status);
        return status;
    }

    // Act as the owner of the shared directory. Depending on the server's
    // security model, files we create are chown'ed to the attaching user and
    // the group we pass; using the share owner keeps that from failing when
    // QEMU runs unprivileged (as UTM does on macOS).
    uint32_t ownerUid = 0, ownerGid = 0;
    {
        uint32_t probeFid = Client.NewFid();
        p9::Attr rootAttr;
        status = Client.Attach(probeFid, 0, &rootQid);
        if (NT_SUCCESS(status))
        {
            if (NT_SUCCESS(Client.Getattr(probeFid, &rootAttr)))
            {
                ownerUid = rootAttr.uid;
                ownerGid = rootAttr.gid;
            }
            Client.Clunk(probeFid);
        }
    }

    RootFid = Client.NewFid();
    status = Client.Attach(RootFid, ownerUid, &rootQid);
    if (!NT_SUCCESS(status))
    {
        DBG("9P attach failed: 0x%08lx", status);
        RootFid = p9::NOFID;
        return status;
    }
    Client.Gid = ownerGid;
    Client.TimeoutMs = REQUEST_TIMEOUT_MS;
    CacheClear();
    StartClunker();
    DBG("msize %u, acting as uid %u gid %u", Client.Msize, ownerUid, ownerGid);

    if (!NT_SUCCESS(GetMountTag(Device, mountTag, sizeof(mountTag))))
    {
        mountTag[0] = L'\0';
    }
    VolumeLabel = mountTag[0] ? mountTag : FS_NAME;

    GetSystemTimeAsFileTime(&fileTime);

    ZeroMemory(&volumeParams, sizeof(volumeParams));
    volumeParams.Version = sizeof(FSP_FSCTL_VOLUME_PARAMS);
    volumeParams.SectorSize = ALLOCATION_UNIT;
    volumeParams.SectorsPerAllocationUnit = 1;
    volumeParams.VolumeCreationTime = ((PLARGE_INTEGER)&fileTime)->QuadPart;
    volumeParams.FileInfoTimeout = 1000;
    // Let the WinFsp driver cache directory listings for as long as file
    // information, so repeated listings do not go back to the host.
    volumeParams.DirInfoTimeoutValid = 1;
    volumeParams.DirInfoTimeout = 1000;
    volumeParams.CaseSensitiveSearch = 0;
    volumeParams.CasePreservedNames = 1;
    volumeParams.UnicodeOnDisk = 1;
    volumeParams.PersistentAcls = 0;
    volumeParams.PostCleanupWhenModifiedOnly = 1;
    volumeParams.FlushAndPurgeOnCleanup = 1;
    volumeParams.UmFileContextIsUserContext2 = 1;
    volumeParams.MaxComponentLength = 255;
    wcscpy_s(volumeParams.FileSystemName, sizeof(volumeParams.FileSystemName) / sizeof(WCHAR), FS_NAME);

    status = FspFileSystemCreate((PWSTR)TEXT(FSP_FSCTL_DISK_DEVICE_NAME), &volumeParams, &Vio9pInterface, &FileSystem);
    if (!NT_SUCCESS(status))
    {
        goto out_clunk;
    }
    FileSystem->UserContext = this;

    FspFileSystemSetDebugLog(FileSystem, DebugFlags);

    status = SetMountPoint(FileSystem, MountPoint);
    if (!NT_SUCCESS(status))
    {
        goto out_del_fs;
    }

    status = FspFileSystemStartDispatcher(FileSystem, 0);
    if (!NT_SUCCESS(status))
    {
        goto out_del_fs;
    }

    FspServiceLog(EVENTLOG_INFORMATION_TYPE,
                  (PWSTR)L"Mounted VirtFS share '%s' at %s (msize %lu, uid %lu, gid %lu).",
                  VolumeLabel.c_str(),
                  FspFileSystemMountPoint(FileSystem),
                  (ULONG)Client.Msize,
                  (ULONG)ownerUid,
                  (ULONG)ownerGid);
    MountedDrive = MountPointDrive(FspFileSystemMountPoint(FileSystem));
    StartPoller();
    NotifyDriveInUserSessions(MountedDrive, true);
    return STATUS_SUCCESS;

out_del_fs:
    FspFileSystemDelete(FileSystem);
    FileSystem = NULL;
out_clunk:
    FidCacheClear();
    StopClunker();
    Client.Clunk(RootFid);
    RootFid = p9::NOFID;
    return status;
}

VOID VIO9P::Stop()
{
    if (FileSystem == NULL)
    {
        return;
    }

    WCHAR drive = MountPointDrive(FspFileSystemMountPoint(FileSystem));
    MountedDrive = 0;
    StopPoller(); // it uses FileSystem
    FspFileSystemStopDispatcher(FileSystem);
    FspFileSystemDelete(FileSystem);
    FileSystem = NULL;
    FidCacheClear(); // before StopClunker, which sends the last clunks
    StopClunker();
    CacheClear();
    NotifyDriveInUserSessions(drive, false);

    if (RootFid != p9::NOFID)
    {
        Client.Clunk(RootFid);
        RootFid = p9::NOFID;
    }
}

// Reads the names in a directory (without "." and ".."), sorted.
static NTSTATUS ReadNames(VIO9P *Fs, const std::wstring &Path, std::vector<std::string> &Names)
{
    uint32_t fid;
    uint32_t iounit;
    std::vector<p9::DirEntry> entries;
    uint64_t offset = 0;

    Names.clear();
    NTSTATUS status = Fs->WalkPath(Path.c_str(), &fid);
    if (!NT_SUCCESS(status))
    {
        return status;
    }
    status = Fs->Client.Lopen(fid, p9::L_O_RDONLY, &iounit);
    while (NT_SUCCESS(status))
    {
        status = Fs->Client.Readdir(fid, offset, entries);
        if (!NT_SUCCESS(status) || entries.empty())
        {
            break;
        }
        for (auto &e : entries)
        {
            offset = e.offset;
            if (!IsDotName(e.name))
            {
                Names.push_back(std::move(e.name));
            }
        }
    }
    Fs->QueueClunk(fid);
    std::sort(Names.begin(), Names.end());
    return status;
}

static void AddNotify(std::vector<uint8_t> &Buffer,
                      ULONG &Used,
                      FSP_FILE_SYSTEM *FileSystem,
                      const std::wstring &Name,
                      UINT32 Action)
{
    union
    {
        FSP_FSCTL_NOTIFY_INFO V;
        UINT8 B[sizeof(FSP_FSCTL_NOTIFY_INFO) + MAX_PATH * 2 * sizeof(WCHAR)];
    } info;
    size_t len = Name.size();
    if (len > MAX_PATH * 2)
    {
        return;
    }
    info.V.Size = (UINT16)(sizeof(FSP_FSCTL_NOTIFY_INFO) + len * sizeof(WCHAR));
    info.V.Filter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME;
    info.V.Action = Action;
    memcpy(info.V.FileNameBuf, Name.data(), len * sizeof(WCHAR));
    if (!FspFileSystemAddNotifyInfo(&info.V, Buffer.data(), (ULONG)Buffer.size(), &Used))
    {
        FspFileSystemNotify(FileSystem, (FSP_FSCTL_NOTIFY_INFO *)Buffer.data(), Used);
        Used = 0;
        FspFileSystemAddNotifyInfo(&info.V, Buffer.data(), (ULONG)Buffer.size(), &Used);
    }
}

void VIO9P::PollOnce()
{
    struct ITEM
    {
        std::wstring Key;
        std::wstring Path;
    };
    std::vector<ITEM> due;
    ULONGLONG now = GetTickCount64();

    AcquireSRWLockExclusive(&WatchLock);
    for (auto it = Watches.begin(); it != Watches.end();)
    {
        WATCH &w = it->second;
        if (w.Handles == 0 && now - w.Touched > WATCH_KEEP_MS)
        {
            it = Watches.erase(it);
            continue;
        }
        if (w.Handles > 0 && now - w.Since >= POLL_MIN_OPEN_MS && due.size() < POLL_MAX_DIRS)
        {
            due.push_back(ITEM{it->first, w.Path});
        }
        ++it;
    }
    ReleaseSRWLockExclusive(&WatchLock);

    for (const ITEM &item : due)
    {
        if (WaitForSingleObject(PollStopEvent, 0) == WAIT_OBJECT_0)
        {
            return;
        }

        std::vector<std::string> names;
        if (!NT_SUCCESS(ReadNames(this, item.Path, names)))
        {
            continue;
        }

        std::vector<std::string> added, removed;
        bool baseline = false;
        AcquireSRWLockShared(&WatchLock);
        auto it = Watches.find(item.Key);
        if (it == Watches.end())
        {
            ReleaseSRWLockShared(&WatchLock);
            continue;
        }
        if (it->second.HaveNames)
        {
            const auto &old = it->second.Names;
            std::set_difference(names.begin(), names.end(), old.begin(), old.end(), std::back_inserter(added));
            std::set_difference(old.begin(), old.end(), names.begin(), names.end(), std::back_inserter(removed));
        }
        else
        {
            baseline = true;
        }
        ReleaseSRWLockShared(&WatchLock);

        if (!baseline && added.empty() && removed.empty())
        {
            continue;
        }

        if (!baseline)
        {
            // Our caches and WinFsp's (FspFileSystemNotify invalidates the
            // kernel's) must not serve the old listing when Windows re-reads it.
            CacheClear();

            if (FspFileSystemNotifyBegin(FileSystem, 500) != STATUS_SUCCESS)
            {
                continue; // a rename is in progress: try again next time
            }
            std::vector<uint8_t> buffer(16384);
            ULONG used = 0;
            std::wstring prefix = (item.Key == L"\\") ? item.Key : item.Key + L"\\";
            for (int pass = 0; pass < 2; pass++)
            {
                for (const std::string &n : pass == 0 ? removed : added)
                {
                    WCHAR wname[256];
                    int len = Utf8ToWide(n, wname, 255);
                    if (len <= 0)
                    {
                        continue;
                    }
                    // WinFsp wants upper-case names from a file system that
                    // does not normalize the case of names.
                    CharUpperBuffW(wname, (DWORD)len);
                    AddNotify(buffer,
                              used,
                              FileSystem,
                              prefix + std::wstring(wname, len),
                              pass == 0 ? FILE_ACTION_REMOVED : FILE_ACTION_ADDED);
                }
            }
            if (used != 0)
            {
                FspFileSystemNotify(FileSystem, (FSP_FSCTL_NOTIFY_INFO *)buffer.data(), used);
            }
            FspFileSystemNotifyEnd(FileSystem);
            DBG("'%S': %u added, %u removed on the host", item.Path.c_str(), (unsigned)added.size(), (unsigned)removed.size());
        }

        AcquireSRWLockExclusive(&WatchLock);
        it = Watches.find(item.Key);
        if (it != Watches.end())
        {
            it->second.Names.swap(names);
            it->second.HaveNames = true;
        }
        ReleaseSRWLockExclusive(&WatchLock);
    }
}

DWORD WINAPI VIO9P::PollThreadProc(PVOID Param)
{
    VIO9P *fs = (VIO9P *)Param;
    while (WaitForSingleObject(fs->PollStopEvent, POLL_INTERVAL_MS) == WAIT_TIMEOUT)
    {
        try
        {
            fs->PollOnce();
        }
        catch (std::bad_alloc &)
        {
        }
    }
    return 0;
}

void VIO9P::StartPoller()
{
    if (PollStopEvent == NULL)
    {
        PollStopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    }
    if (PollStopEvent != NULL)
    {
        ResetEvent(PollStopEvent);
        PollThread = CreateThread(NULL, 0, PollThreadProc, this, 0, NULL);
    }
}

void VIO9P::StopPoller()
{
    if (PollThread != NULL)
    {
        SetEvent(PollStopEvent);
        WaitForSingleObject(PollThread, INFINITE);
        CloseHandle(PollThread);
        PollThread = NULL;
    }
    AcquireSRWLockExclusive(&WatchLock);
    Watches.clear();
    ReleaseSRWLockExclusive(&WatchLock);
}

// Mount thread, with Lock held: mounts the share if it is not mounted yet.
VOID VIO9P::TryMount()
{
    if (FileSystem != NULL)
    {
        return;
    }

    if (Device == INVALID_HANDLE_VALUE)
    {
        DWORD error = FindDeviceInterface();
        if (error != ERROR_SUCCESS)
        {
            if (!LoggedWaiting)
            {
                FspServiceLog(EVENTLOG_INFORMATION_TYPE,
                              (PWSTR)L"The %s service is waiting for the VirtFS device.",
                              FS_SERVICE_NAME);
                LoggedWaiting = true;
            }
            return;
        }
        HandleNotificationRegistered =
            (DevHandleNotification.Register(DeviceNotificationCallback, this, Device) == ERROR_SUCCESS);
    }

    NTSTATUS status = Start();
    if (NT_SUCCESS(status))
    {
        LoggedWaiting = false;
        LastLoggedStatus = STATUS_SUCCESS;
        return;
    }

    if (status != LastLoggedStatus)
    {
        FspServiceLog(EVENTLOG_WARNING_TYPE,
                      (PWSTR)L"Mounting the VirtFS share failed (Status=%lx); retrying every %u seconds.",
                      status,
                      MOUNT_RETRY_MS / 1000);
        LastLoggedStatus = status;
    }

    // Reopen the device on the next attempt. Unregistering asynchronously
    // keeps us from waiting on a notification callback while holding Lock.
    if (HandleNotificationRegistered)
    {
        DevHandleNotification.AsyncUnregister();
        HandleNotificationRegistered = false;
    }
    CloseDeviceInterface();
}

static DWORD WINAPI MountThreadProc(PVOID Param)
{
    VIO9P *fs = (VIO9P *)Param;

    for (;;)
    {
        // Outside Lock: may wait for a removal callback that needs Lock.
        fs->DevHandleNotification.WaitForUnregWork();

        EnterCriticalSection(&fs->Lock);
        fs->TryMount();
        bool mounted = (fs->FileSystem != NULL);
        LeaveCriticalSection(&fs->Lock);

        HANDLE events[2] = {fs->StopEvent, fs->WakeEvent};
        if (WaitForMultipleObjects(2, events, FALSE, mounted ? INFINITE : MOUNT_RETRY_MS) == WAIT_OBJECT_0)
        {
            break;
        }
    }
    return 0;
}

VOID VIO9P::CloseDeviceInterface()
{
    if (Device != INVALID_HANDLE_VALUE)
    {
        CloseHandle(Device);
        Device = INVALID_HANDLE_VALUE;
    }
}

VOID VIO9P::DevArrival()
{
    SetEvent(WakeEvent);
}

// Device is about to go away (or went away): unmount and let go of it.
VOID VIO9P::DevQueryRemove()
{
    EnterCriticalSection(&Lock);
    Stop();
    if (HandleNotificationRegistered)
    {
        DevHandleNotification.AsyncUnregister();
        HandleNotificationRegistered = false;
    }
    CloseDeviceInterface();
    LeaveCriticalSection(&Lock);

    SetEvent(WakeEvent); // retry until the device is back
}

static void DeviceNotificationCallback(PVOID Context, DeviceNotifyAction Action)
{
    auto fs = static_cast<VIO9P *>(Context);

    switch (Action)
    {
        case DeviceNotifyAction::InterfaceArrival:
        case DeviceNotifyAction::QueryRemoveFailed:
            fs->DevArrival();
            break;
        case DeviceNotifyAction::QueryRemove:
        case DeviceNotifyAction::RemoveComplete:
            fs->DevQueryRemove();
            break;
        default:
            break;
    }
}

//
// Service
//

static void ReadRegistry(ULONG &DebugFlags, std::wstring &DebugLogFile, std::wstring &MountPoint, std::wstring &Tag)
{
    std::wstring value;
    DWORD flags;

    if (RegistryGetVal(FS_SERVICE_REGKEY, L"DebugFlags", flags) == ERROR_SUCCESS)
    {
        DebugFlags = flags;
    }
    if (RegistryGetVal(FS_SERVICE_REGKEY, L"DebugLogFile", value) == ERROR_SUCCESS)
    {
        DebugLogFile = value;
    }
    if (RegistryGetVal(FS_SERVICE_REGKEY, L"MountPoint", value) == ERROR_SUCCESS)
    {
        MountPoint = value;
    }
    if (RegistryGetVal(FS_SERVICE_REGKEY, L"Tag", value) == ERROR_SUCCESS)
    {
        Tag = value;
    }
    if (RegistryGetVal(FS_SERVICE_REGKEY, L"TraceRpc", flags) == ERROR_SUCCESS)
    {
        p9::TraceRpc = (flags != 0);
    }
}

static NTSTATUS ParseArgs(ULONG argc,
                          PWSTR *argv,
                          ULONG &DebugFlags,
                          std::wstring &DebugLogFile,
                          std::wstring &MountPoint,
                          std::wstring &Tag)
{
    for (ULONG i = 1; i < argc; i++)
    {
        PWSTR arg = argv[i];
        if (arg[0] != L'-' || arg[1] == L'\0' || arg[2] != L'\0' || i + 1 >= argc)
        {
            goto usage;
        }
        PWSTR value = argv[++i];
        switch (arg[1])
        {
            case L'd':
                DebugFlags = wcstoul(value, NULL, 0);
                break;
            case L'D':
                DebugLogFile = value;
                break;
            case L'm':
                MountPoint = value;
                break;
            case L't':
                Tag = value;
                break;
            default:
                goto usage;
        }
    }
    return STATUS_SUCCESS;

usage:
    FspServiceLog(EVENTLOG_ERROR_TYPE,
                  (PWSTR)L"usage: %s [-d DebugFlags] [-D DebugLogFile] [-m MountPoint] [-t Tag]",
                  FS_SERVICE_NAME);
    return STATUS_UNSUCCESSFUL;
}

static NTSTATUS SvcStart(FSP_SERVICE *Service, ULONG argc, PWSTR *argv)
{
    ULONG debugFlags = 0;
    std::wstring debugLogFile, mountPoint = L"*", tag;
    NTSTATUS status;
    DWORD error;

    ReadRegistry(debugFlags, debugLogFile, mountPoint, tag);
    status = ParseArgs(argc, argv, debugFlags, debugLogFile, mountPoint, tag);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    if (!debugLogFile.empty())
    {
        HANDLE logHandle = (debugLogFile == L"-")
                               ? GetStdHandle(STD_ERROR_HANDLE)
                               : CreateFileW(debugLogFile.c_str(),
                                             FILE_APPEND_DATA,
                                             FILE_SHARE_READ | FILE_SHARE_WRITE,
                                             NULL,
                                             OPEN_ALWAYS,
                                             FILE_ATTRIBUTE_NORMAL,
                                             NULL);
        if (logHandle != INVALID_HANDLE_VALUE)
        {
            FspDebugLogSetHandle(logHandle);
        }
    }

    VIO9P *fs = new (std::nothrow) VIO9P;
    if (fs == nullptr)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    fs->DebugFlags = debugFlags;
    fs->MountPoint = mountPoint;
    fs->Tag = tag;

    ULONG sdSize = 0;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(DEFAULT_SDDL, SDDL_REVISION_1, &fs->DefaultSd, &sdSize))
    {
        status = FspNtStatusFromWin32(GetLastError());
        delete fs;
        return status;
    }
    fs->DefaultSdSize = sdSize;

    fs->StopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    fs->WakeEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (fs->StopEvent == NULL || fs->WakeEvent == NULL || !fs->DevHandleNotification.CreateUnregWork())
    {
        delete fs;
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    // Only an optimization: the mount thread also polls for the device.
    error = fs->DevInterfaceNotification.Register(DeviceNotificationCallback, fs, GUID_DEVINTERFACE_VIO9P);
    if (error != ERROR_SUCCESS)
    {
        DBG("device arrival notification unavailable: %lu", error);
    }

    fs->MountThread = CreateThread(NULL, 0, MountThreadProc, fs, 0, NULL);
    if (fs->MountThread == NULL)
    {
        status = FspNtStatusFromWin32(GetLastError());
        fs->DevInterfaceNotification.Unregister();
        delete fs;
        return status;
    }

    Service->UserContext = fs;
    return STATUS_SUCCESS;
}

static NTSTATUS SvcStop(FSP_SERVICE *Service)
{
    VIO9P *fs = (VIO9P *)Service->UserContext;
    bool unregister;

    if (fs == nullptr)
    {
        return STATUS_SUCCESS;
    }

    SetEvent(fs->StopEvent);
    WaitForSingleObject(fs->MountThread, INFINITE);

    fs->DevInterfaceNotification.Unregister();

    EnterCriticalSection(&fs->Lock);
    fs->Stop();
    unregister = fs->HandleNotificationRegistered;
    fs->HandleNotificationRegistered = false;
    LeaveCriticalSection(&fs->Lock);

    // Outside Lock: waits for a removal callback that may need Lock.
    if (unregister)
    {
        fs->DevHandleNotification.Unregister();
    }
    fs->DevHandleNotification.WaitForUnregWork();

    fs->CloseDeviceInterface();
    Service->UserContext = nullptr;
    delete fs;

    return STATUS_SUCCESS;
}

// Installer helper: stages vio9p.inf in the driver store and installs it on
// every virtio-9p device, even when a driver with the same version is already
// installed (Windows 7's pnputil cannot force that). Prints
// "PUBLISHED=oemNN.inf". Returns 0, or 3010 if a restart is required.
static int InstallDriverPackage(PCWSTR InfPath)
{
    static const PCWSTR HardwareIds[] = {L"PCI\\VEN_1AF4&DEV_1009", L"PCI\\VEN_1AF4&DEV_1049"};
    WCHAR fullInf[MAX_PATH], published[MAX_PATH];
    PWSTR publishedName = NULL;
    BOOL reboot = FALSE;

    if (GetFullPathNameW(InfPath, MAX_PATH, fullInf, NULL) == 0)
    {
        fwprintf(stderr, L"bad INF path: %lu\n", GetLastError());
        return 1;
    }
    if (!SetupCopyOEMInfW(fullInf, NULL, SPOST_PATH, 0, published, MAX_PATH, NULL, &publishedName))
    {
        fwprintf(stderr, L"SetupCopyOEMInf failed: %lu\n", GetLastError());
        return 1;
    }
    wprintf(L"PUBLISHED=%s\n", publishedName ? publishedName : published);

    for (PCWSTR id : HardwareIds)
    {
        BOOL needReboot = FALSE;
        if (UpdateDriverForPlugAndPlayDevicesW(NULL, id, fullInf, INSTALLFLAG_FORCE, &needReboot))
        {
            reboot |= needReboot;
        }
        else if (GetLastError() != ERROR_NO_SUCH_DEVINST)
        {
            fwprintf(stderr, L"UpdateDriverForPlugAndPlayDevices(%s) failed: %lu\n", id, GetLastError());
            return 1;
        }
    }
    return reboot ? ERROR_SUCCESS_REBOOT_REQUIRED : 0;
}

// Uninstaller helper: removes every virtio-9p device node, which unloads
// vio9p.sys so its driver packages are no longer in use. Run --rescan after
// deleting the packages so the device comes back without a driver right away
// instead of after the next restart. Returns the number of devices removed.
static int RemoveVio9pDevices()
{
    HDEVINFO devs = SetupDiGetClassDevsW(NULL, L"PCI", NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    int removed = 0;

    if (devs == INVALID_HANDLE_VALUE)
    {
        return 0;
    }

    SP_DEVINFO_DATA dev;
    dev.cbSize = sizeof(dev);
    for (DWORD i = 0; SetupDiEnumDeviceInfo(devs, i, &dev); i++)
    {
        WCHAR ids[2048];
        ZeroMemory(ids, sizeof(ids));
        if (!SetupDiGetDeviceRegistryPropertyW(devs, &dev, SPDRP_HARDWAREID, NULL, (PBYTE)ids, sizeof(ids) - 2 * sizeof(WCHAR), NULL))
        {
            continue;
        }
        bool match = false;
        for (PCWSTR id = ids; *id; id += wcslen(id) + 1)
        {
            if (_wcsnicmp(id, L"PCI\\VEN_1AF4&DEV_1009", 21) == 0 || _wcsnicmp(id, L"PCI\\VEN_1AF4&DEV_1049", 21) == 0)
            {
                match = true;
                break;
            }
        }
        if (match && SetupDiCallClassInstaller(DIF_REMOVE, devs, &dev))
        {
            removed++;
        }
    }
    SetupDiDestroyDeviceInfoList(devs);

    wprintf(L"Removed %d device(s).\n", removed);
    return removed;
}

static void RescanDevices()
{
    DEVINST root;
    if (CM_Locate_DevNodeW(&root, NULL, CM_LOCATE_DEVNODE_NORMAL) == CR_SUCCESS)
    {
        CM_Reenumerate_DevNode(root, CM_REENUMERATE_SYNCHRONOUS);
    }
}

// A user logging on after the share was mounted gets the notification too.
static NTSTATUS SvcControl(FSP_SERVICE *Service, ULONG Control, ULONG EventType, PVOID EventData)
{
    VIO9P *fs = (VIO9P *)Service->UserContext;

    if (Control == SERVICE_CONTROL_SESSIONCHANGE && EventType == WTS_SESSION_LOGON && fs != nullptr &&
        EventData != nullptr)
    {
        WCHAR drive = fs->MountedDrive;
        if (drive != 0)
        {
            NotifyDriveInUserSessions(drive, true, ((PWTSSESSION_NOTIFICATION)EventData)->dwSessionId);
        }
    }
    return STATUS_SUCCESS;
}

int wmain(int argc, wchar_t **argv)
{
    // Installer helpers.
    if (argc == 3 && wcscmp(argv[1], L"--install-driver") == 0)
    {
        return InstallDriverPackage(argv[2]);
    }
    if (argc == 2 && wcscmp(argv[1], L"--remove-devices") == 0)
    {
        RemoveVio9pDevices();
        return 0;
    }
    if (argc == 2 && wcscmp(argv[1], L"--rescan") == 0)
    {
        RescanDevices();
        return 0;
    }

    // Helper mode, started by the service inside a user session.
    if (argc == 4 && wcscmp(argv[1], L"--notify-drive") == 0)
    {
        WCHAR drive = MountPointDrive(argv[2]);
        return drive ? NotifyDriveInThisSession(drive, wcscmp(argv[3], L"add") == 0) : 1;
    }

    NTSTATUS result = FspLoad(0);
    if (!NT_SUCCESS(result))
    {
        fwprintf(stderr, L"The service %s failed to load WinFsp DLL (Status=%lx).", FS_SERVICE_NAME, result);
        return ERROR_DELAY_LOAD_FAILED;
    }

    FSP_SERVICE *service;
    result = FspServiceCreate((PWSTR)FS_SERVICE_NAME, SvcStart, SvcStop, SvcControl, &service);
    if (!NT_SUCCESS(result))
    {
        FspServiceLog(EVENTLOG_ERROR_TYPE, (PWSTR)L"The service %s cannot be created (Status=%lx).", FS_SERVICE_NAME, result);
        return FspWin32FromNtStatus(result);
    }
    FspServiceAllowConsoleMode(service);
    FspServiceAcceptControl(service, SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN | SERVICE_ACCEPT_SESSIONCHANGE);
    result = FspServiceLoop(service);
    ULONG exitCode = FspServiceGetExitCode(service);
    FspServiceDelete(service);

    if (!NT_SUCCESS(result))
    {
        FspServiceLog(EVENTLOG_ERROR_TYPE, (PWSTR)L"The service %s has failed to run (Status=%lx).", FS_SERVICE_NAME, result);
        return FspWin32FromNtStatus(result);
    }
    return exitCode;
}
