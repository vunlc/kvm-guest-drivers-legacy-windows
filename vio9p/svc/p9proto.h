/*
 * Minimal 9P2000.L client used by the vio9p service. Messages go to the
 * device through vio9p.sys (IOCTL_VIO9P_REQUEST), one request at a time.
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

#pragma once

// Include after winfsp/winfsp.h (NTSTATUS values, FspNtStatusFromWin32) and
// vio9p_ioctl.h (IOCTL_VIO9P_REQUEST).
#include <stdint.h>
#include <string.h>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace p9
{

// Message types (9P2000.L subset plus the 9P2000 core messages it reuses).
enum : uint8_t
{
    Rlerror = 7,
    Tstatfs = 8,
    Rstatfs = 9,
    Tlopen = 12,
    Rlopen = 13,
    Tlcreate = 14,
    Rlcreate = 15,
    Tgetattr = 24,
    Rgetattr = 25,
    Tsetattr = 26,
    Rsetattr = 27,
    Treaddir = 40,
    Rreaddir = 41,
    Tfsync = 50,
    Rfsync = 51,
    Tmkdir = 72,
    Rmkdir = 73,
    Trenameat = 74,
    Rrenameat = 75,
    Tunlinkat = 76,
    Runlinkat = 77,
    Tversion = 100,
    Rversion = 101,
    Tattach = 104,
    Rattach = 105,
    Twalk = 110,
    Rwalk = 111,
    Tread = 116,
    Rread = 117,
    Twrite = 118,
    Rwrite = 119,
    Tclunk = 120,
    Rclunk = 121,
};

// When set, every request's type, size and duration go to the debug log.
inline volatile bool TraceRpc = false;

constexpr uint16_t NOTAG = 0xFFFF;
constexpr uint32_t NOFID = 0xFFFFFFFF;
constexpr uint32_t MAXWELEM = 16;
constexpr uint32_t HEADER_SIZE = 7;    // size[4] type[1] tag[2]
constexpr uint32_t IOHDR_SIZE = 24;    // largest of Tread/Twrite/Rread headers, rounded up
constexpr uint32_t SMALL_REPLY = 4096; // enough for every reply that carries no data

// Linux open(2) flags, as used by Tlopen / Tlcreate.
constexpr uint32_t L_O_RDONLY = 00000000;
constexpr uint32_t L_O_RDWR = 00000002;
constexpr uint32_t L_O_EXCL = 00000200;
constexpr uint32_t L_AT_REMOVEDIR = 0x200;

constexpr uint32_t L_S_IFMT = 0170000;
constexpr uint32_t L_S_IFDIR = 0040000;
constexpr uint32_t L_S_IFLNK = 0120000;

constexpr uint64_t GETATTR_ALL = 0x00003fffULL;
constexpr uint64_t GETATTR_BTIME = 0x00000800ULL;

constexpr uint32_t SETATTR_MODE = 0x001;
constexpr uint32_t SETATTR_SIZE = 0x008;
constexpr uint32_t SETATTR_ATIME = 0x010;
constexpr uint32_t SETATTR_MTIME = 0x020;
constexpr uint32_t SETATTR_ATIME_SET = 0x080;
constexpr uint32_t SETATTR_MTIME_SET = 0x100;

struct Qid
{
    uint8_t type = 0;
    uint32_t version = 0;
    uint64_t path = 0;
};

struct Attr
{
    uint64_t valid = 0;
    Qid qid;
    uint32_t mode = 0, uid = 0, gid = 0;
    uint64_t nlink = 0, rdev = 0, size = 0, blksize = 0, blocks = 0;
    uint64_t atime_sec = 0, atime_nsec = 0, mtime_sec = 0, mtime_nsec = 0;
    uint64_t ctime_sec = 0, ctime_nsec = 0, btime_sec = 0, btime_nsec = 0;
    uint64_t gen = 0, data_version = 0;

    bool IsDir() const
    {
        return (mode & L_S_IFMT) == L_S_IFDIR;
    }
};

struct StatFs
{
    uint32_t type = 0, bsize = 0;
    uint64_t blocks = 0, bfree = 0, bavail = 0, files = 0, ffree = 0, fsid = 0;
    uint32_t namelen = 0;
};

struct DirEntry
{
    Qid qid;
    uint64_t offset = 0;
    uint8_t type = 0;
    std::string name;
};

class Writer
{
  public:
    explicit Writer(uint8_t type) : Buf(HEADER_SIZE, 0)
    {
        Buf[4] = type;
    }
    void u8(uint8_t v)
    {
        Buf.push_back(v);
    }
    void u16(uint16_t v)
    {
        put(&v, 2);
    }
    void u32(uint32_t v)
    {
        put(&v, 4);
    }
    void u64(uint64_t v)
    {
        put(&v, 8);
    }
    void str(const std::string &s)
    {
        u16((uint16_t)s.size());
        put(s.data(), s.size());
    }
    void put(const void *p, size_t n)
    {
        const uint8_t *b = (const uint8_t *)p;
        Buf.insert(Buf.end(), b, b + n);
    }
    std::vector<uint8_t> &Finish(uint16_t tag)
    {
        uint32_t size = (uint32_t)Buf.size();
        memcpy(&Buf[0], &size, 4);
        memcpy(&Buf[5], &tag, 2);
        return Buf;
    }

  private:
    std::vector<uint8_t> Buf;
};

class Reader
{
  public:
    Reader() = default;
    Reader(const uint8_t *p, size_t n) : P(p), N(n)
    {
    }
    bool Ok() const
    {
        return OkFlag;
    }
    size_t Remaining() const
    {
        return OkFlag ? N - Off : 0;
    }
    uint8_t u8()
    {
        uint8_t v = 0;
        get(&v, 1);
        return v;
    }
    uint16_t u16()
    {
        uint16_t v = 0;
        get(&v, 2);
        return v;
    }
    uint32_t u32()
    {
        uint32_t v = 0;
        get(&v, 4);
        return v;
    }
    uint64_t u64()
    {
        uint64_t v = 0;
        get(&v, 8);
        return v;
    }
    std::string str()
    {
        uint16_t n = u16();
        if (!OkFlag || N - Off < n)
        {
            OkFlag = false;
            return std::string();
        }
        std::string s((const char *)P + Off, n);
        Off += n;
        return s;
    }
    Qid qid()
    {
        Qid q;
        q.type = u8();
        q.version = u32();
        q.path = u64();
        return q;
    }
    const uint8_t *data(size_t n)
    {
        if (!OkFlag || N - Off < n)
        {
            OkFlag = false;
            return nullptr;
        }
        const uint8_t *p = P + Off;
        Off += n;
        return p;
    }

  private:
    void get(void *v, size_t n)
    {
        if (!OkFlag || N - Off < n)
        {
            OkFlag = false;
            return;
        }
        memcpy(v, P + Off, n);
        Off += n;
    }
    const uint8_t *P = nullptr;
    size_t N = 0;
    size_t Off = 0;
    bool OkFlag = true;
};

// DeviceIoControl on a handle opened with FILE_FLAG_OVERLAPPED, giving up after
// TimeoutMs: the request is then cancelled and STATUS_IO_TIMEOUT returned, so a
// reply that never arrives cannot block the caller forever.
inline NTSTATUS IoctlWithTimeout(HANDLE Device,
                                 DWORD Code,
                                 const void *In,
                                 DWORD InLen,
                                 void *Out,
                                 DWORD OutLen,
                                 DWORD *Returned,
                                 DWORD TimeoutMs)
{
    OVERLAPPED ov;
    ZeroMemory(&ov, sizeof(ov));
    ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (ov.hEvent == NULL)
    {
        return FspNtStatusFromWin32(GetLastError());
    }

    NTSTATUS status = STATUS_SUCCESS;
    *Returned = 0;
    if (!DeviceIoControl(Device, Code, (LPVOID)In, InLen, Out, OutLen, Returned, &ov))
    {
        DWORD error = GetLastError();
        if (error != ERROR_IO_PENDING)
        {
            status = (error == ERROR_RETRY) ? STATUS_RETRY : FspNtStatusFromWin32(error);
        }
        else if (WaitForSingleObject(ov.hEvent, TimeoutMs) != WAIT_OBJECT_0)
        {
            CancelIoEx(Device, &ov);
            GetOverlappedResult(Device, &ov, Returned, TRUE); // wait for the cancel to finish
            *Returned = 0;
            status = STATUS_IO_TIMEOUT;
        }
        else if (!GetOverlappedResult(Device, &ov, Returned, FALSE))
        {
            DWORD error2 = GetLastError();
            status = (error2 == ERROR_RETRY) ? STATUS_RETRY : FspNtStatusFromWin32(error2);
        }
    }

    CloseHandle(ov.hEvent);
    return status;
}

// Maps a Linux errno from Rlerror to an NTSTATUS.
inline NTSTATUS ErrnoToNtStatus(uint32_t e)
{
    switch (e)
    {
        case 1:  // EPERM
        case 13: // EACCES
            return STATUS_ACCESS_DENIED;
        case 2: // ENOENT
            return STATUS_OBJECT_NAME_NOT_FOUND;
        case 5: // EIO
            return STATUS_IO_DEVICE_ERROR;
        case 9: // EBADF
            return STATUS_INVALID_HANDLE;
        case 12: // ENOMEM
            return STATUS_INSUFFICIENT_RESOURCES;
        case 16: // EBUSY
            return STATUS_SHARING_VIOLATION;
        case 17: // EEXIST
            return STATUS_OBJECT_NAME_COLLISION;
        case 18: // EXDEV
            return STATUS_NOT_SAME_DEVICE;
        case 20: // ENOTDIR
            return STATUS_NOT_A_DIRECTORY;
        case 21: // EISDIR
            return STATUS_FILE_IS_A_DIRECTORY;
        case 22: // EINVAL
            return STATUS_INVALID_PARAMETER;
        case 23: // ENFILE
        case 24: // EMFILE
            return STATUS_TOO_MANY_OPENED_FILES;
        case 27: // EFBIG
        case 28: // ENOSPC
            return STATUS_DISK_FULL;
        case 30: // EROFS
            return STATUS_MEDIA_WRITE_PROTECTED;
        case 31: // EMLINK
            return STATUS_TOO_MANY_LINKS;
        case 36: // ENAMETOOLONG
            return STATUS_NAME_TOO_LONG;
        case 38: // ENOSYS
            return STATUS_NOT_IMPLEMENTED;
        case 39: // ENOTEMPTY
            return STATUS_DIRECTORY_NOT_EMPTY;
        case 40: // ELOOP
            return STATUS_STOPPED_ON_SYMLINK;
        case 95: // EOPNOTSUPP
            return STATUS_NOT_SUPPORTED;
        case 122: // EDQUOT
            return STATUS_DISK_QUOTA_EXCEEDED;
        default:
            return STATUS_UNSUCCESSFUL;
    }
}

class Client
{
  public:
    HANDLE Device = INVALID_HANDLE_VALUE; // opened with FILE_FLAG_OVERLAPPED
    uint32_t Msize = 0;
    // How long one request may wait for its reply.
    DWORD TimeoutMs = 60000;
    // Group given to files and directories we create (see VIO9P::Start).
    uint32_t Gid = 0;

    // Starts a new session on Dev (Tversion resets all fids on the server).
    void Reset(HANDLE Dev)
    {
        Device = Dev;
        Msize = 0;
        Gid = 0;
        InterlockedExchange(&NextFidValue, 0);
        InterlockedExchange(&NextTagValue, 0);
    }

    uint32_t NewFid()
    {
        for (;;)
        {
            uint32_t fid = (uint32_t)InterlockedIncrement(&NextFidValue);
            if (fid != NOFID)
            {
                return fid;
            }
        }
    }

    // Largest payload of one Tread / Twrite.
    uint32_t MaxIo() const
    {
        return Msize > IOHDR_SIZE ? Msize - IOHDR_SIZE : 0;
    }

    NTSTATUS Version(uint32_t WantedMsize)
    {
        Writer w(Tversion);
        w.u32(WantedMsize);
        w.str("9P2000.L");
        std::vector<uint8_t> resp;
        Reader r;
        NTSTATUS st = Rpc(w, NOTAG, Rversion, SMALL_REPLY, resp, r);
        if (!NT_SUCCESS(st))
        {
            return st;
        }
        uint32_t msize = r.u32();
        std::string version = r.str();
        if (!r.Ok() || version != "9P2000.L" || msize < 4096)
        {
            return STATUS_NOT_SUPPORTED;
        }
        Msize = msize < WantedMsize ? msize : WantedMsize;
        return STATUS_SUCCESS;
    }

    NTSTATUS Attach(uint32_t Fid, uint32_t Uid, Qid *Q)
    {
        Writer w(Tattach);
        w.u32(Fid);
        w.u32(NOFID);
        w.str("");
        w.str("");
        w.u32(Uid);
        return Simple(w, Rattach, [&](Reader &r) { *Q = r.qid(); });
    }

    // Walks Names from Fid into NewFid (which must be unused). On failure
    // NewFid is not left allocated and *FailedAt is the index of the name
    // that could not be walked.
    NTSTATUS Walk(uint32_t Fid, uint32_t NewFid, const std::vector<std::string> &Names, Qid *Last, size_t *FailedAt)
    {
        size_t done = 0;
        bool created = false;
        uint32_t from = Fid;

        *FailedAt = 0;
        do
        {
            size_t n = Names.size() - done;
            if (n > MAXWELEM)
            {
                n = MAXWELEM;
            }

            Writer w(Twalk);
            w.u32(from);
            w.u32(NewFid);
            w.u16((uint16_t)n);
            for (size_t i = 0; i < n; i++)
            {
                w.str(Names[done + i]);
            }

            std::vector<uint8_t> resp;
            Reader r;
            NTSTATUS st = Rpc(w, Tag(), Rwalk, SMALL_REPLY, resp, r);
            if (!NT_SUCCESS(st))
            {
                if (created)
                {
                    Clunk(NewFid);
                }
                *FailedAt = done;
                return st;
            }

            uint16_t nwqid = r.u16();
            Qid q;
            for (uint16_t i = 0; i < nwqid && r.Ok(); i++)
            {
                q = r.qid();
            }
            if (!r.Ok())
            {
                if (created || nwqid == n)
                {
                    Clunk(NewFid);
                }
                return STATUS_INVALID_NETWORK_RESPONSE;
            }
            if (nwqid < n)
            {
                if (created)
                {
                    Clunk(NewFid);
                }
                *FailedAt = done + nwqid;
                return STATUS_OBJECT_NAME_NOT_FOUND;
            }

            if (Last != nullptr && n != 0)
            {
                *Last = q;
            }
            created = true;
            from = NewFid;
            done += n;
        } while (done < Names.size());

        return STATUS_SUCCESS;
    }

    NTSTATUS Clunk(uint32_t Fid)
    {
        Writer w(Tclunk);
        w.u32(Fid);
        return Simple(w, Rclunk, nullptr);
    }

    NTSTATUS Getattr(uint32_t Fid, Attr *A)
    {
        Writer w(Tgetattr);
        w.u32(Fid);
        w.u64(GETATTR_ALL);
        return Simple(w, Rgetattr, [&](Reader &r) {
            A->valid = r.u64();
            A->qid = r.qid();
            A->mode = r.u32();
            A->uid = r.u32();
            A->gid = r.u32();
            A->nlink = r.u64();
            A->rdev = r.u64();
            A->size = r.u64();
            A->blksize = r.u64();
            A->blocks = r.u64();
            A->atime_sec = r.u64();
            A->atime_nsec = r.u64();
            A->mtime_sec = r.u64();
            A->mtime_nsec = r.u64();
            A->ctime_sec = r.u64();
            A->ctime_nsec = r.u64();
            A->btime_sec = r.u64();
            A->btime_nsec = r.u64();
            A->gen = r.u64();
            A->data_version = r.u64();
        });
    }

    NTSTATUS Setattr(uint32_t Fid,
                     uint32_t Valid,
                     uint32_t Mode,
                     uint64_t Size,
                     uint64_t AtimeSec,
                     uint64_t AtimeNsec,
                     uint64_t MtimeSec,
                     uint64_t MtimeNsec)
    {
        Writer w(Tsetattr);
        w.u32(Fid);
        w.u32(Valid);
        w.u32(Mode);
        w.u32(0); // uid
        w.u32(0); // gid
        w.u64(Size);
        w.u64(AtimeSec);
        w.u64(AtimeNsec);
        w.u64(MtimeSec);
        w.u64(MtimeNsec);
        return Simple(w, Rsetattr, nullptr);
    }

    NTSTATUS Lopen(uint32_t Fid, uint32_t Flags, uint32_t *Iounit)
    {
        Writer w(Tlopen);
        w.u32(Fid);
        w.u32(Flags);
        return Simple(w, Rlopen, [&](Reader &r) {
            r.qid();
            *Iounit = r.u32();
        });
    }

    NTSTATUS Lcreate(uint32_t Fid, const std::string &Name, uint32_t Flags, uint32_t Mode, uint32_t *Iounit)
    {
        Writer w(Tlcreate);
        w.u32(Fid);
        w.str(Name);
        w.u32(Flags);
        w.u32(Mode);
        w.u32(Gid);
        return Simple(w, Rlcreate, [&](Reader &r) {
            r.qid();
            *Iounit = r.u32();
        });
    }

    NTSTATUS Mkdir(uint32_t DirFid, const std::string &Name, uint32_t Mode)
    {
        Writer w(Tmkdir);
        w.u32(DirFid);
        w.str(Name);
        w.u32(Mode);
        w.u32(Gid);
        return Simple(w, Rmkdir, [&](Reader &r) { r.qid(); });
    }

    NTSTATUS Read(uint32_t Fid, uint64_t Offset, uint32_t Count, void *Buffer, uint32_t *Got)
    {
        Writer w(Tread);
        w.u32(Fid);
        w.u64(Offset);
        w.u32(Count);
        std::vector<uint8_t> resp;
        Reader r;
        NTSTATUS st = Rpc(w, Tag(), Rread, (size_t)Count + IOHDR_SIZE, resp, r);
        if (!NT_SUCCESS(st))
        {
            return st;
        }
        uint32_t n = r.u32();
        const uint8_t *p = r.data(n);
        if (p == nullptr || n > Count)
        {
            return STATUS_INVALID_NETWORK_RESPONSE;
        }
        memcpy(Buffer, p, n);
        *Got = n;
        return STATUS_SUCCESS;
    }

    NTSTATUS Write(uint32_t Fid, uint64_t Offset, const void *Buffer, uint32_t Count, uint32_t *Wrote)
    {
        Writer w(Twrite);
        w.u32(Fid);
        w.u64(Offset);
        w.u32(Count);
        w.put(Buffer, Count);
        return Simple(w, Rwrite, [&](Reader &r) { *Wrote = r.u32(); });
    }

    NTSTATUS Readdir(uint32_t Fid, uint64_t Offset, std::vector<DirEntry> &Entries)
    {
        uint32_t count = Msize - IOHDR_SIZE;
        Writer w(Treaddir);
        w.u32(Fid);
        w.u64(Offset);
        w.u32(count);
        std::vector<uint8_t> resp;
        Reader r;
        Entries.clear();
        NTSTATUS st = Rpc(w, Tag(), Rreaddir, Msize, resp, r);
        if (!NT_SUCCESS(st))
        {
            return st;
        }
        uint32_t n = r.u32();
        const uint8_t *p = r.data(n);
        if (p == nullptr)
        {
            return STATUS_INVALID_NETWORK_RESPONSE;
        }
        Reader e(p, n);
        while (e.Remaining() > 0)
        {
            DirEntry d;
            d.qid = e.qid();
            d.offset = e.u64();
            d.type = e.u8();
            d.name = e.str();
            if (!e.Ok())
            {
                return STATUS_INVALID_NETWORK_RESPONSE;
            }
            Entries.push_back(std::move(d));
        }
        return STATUS_SUCCESS;
    }

    NTSTATUS Statfs(uint32_t Fid, StatFs *S)
    {
        Writer w(Tstatfs);
        w.u32(Fid);
        return Simple(w, Rstatfs, [&](Reader &r) {
            S->type = r.u32();
            S->bsize = r.u32();
            S->blocks = r.u64();
            S->bfree = r.u64();
            S->bavail = r.u64();
            S->files = r.u64();
            S->ffree = r.u64();
            S->fsid = r.u64();
            S->namelen = r.u32();
        });
    }

    NTSTATUS Unlinkat(uint32_t DirFid, const std::string &Name, uint32_t Flags)
    {
        Writer w(Tunlinkat);
        w.u32(DirFid);
        w.str(Name);
        w.u32(Flags);
        return Simple(w, Runlinkat, nullptr);
    }

    NTSTATUS Renameat(uint32_t OldDirFid, const std::string &OldName, uint32_t NewDirFid, const std::string &NewName)
    {
        Writer w(Trenameat);
        w.u32(OldDirFid);
        w.str(OldName);
        w.u32(NewDirFid);
        w.str(NewName);
        return Simple(w, Rrenameat, nullptr);
    }

    NTSTATUS Fsync(uint32_t Fid)
    {
        Writer w(Tfsync);
        w.u32(Fid);
        w.u32(0); // datasync
        return Simple(w, Rfsync, nullptr);
    }

  private:
    volatile LONG NextFidValue = 0;
    volatile LONG NextTagValue = 0;

    uint16_t Tag()
    {
        // Any value but NOTAG; the driver matches replies by ring slot, not tag.
        return (uint16_t)((uint32_t)InterlockedIncrement(&NextTagValue) % 0xFFFF);
    }

    template <typename Parse> NTSTATUS Simple(Writer &W, uint8_t RType, Parse ParseReply)
    {
        std::vector<uint8_t> resp;
        Reader r;
        NTSTATUS st = Rpc(W, Tag(), RType, SMALL_REPLY, resp, r);
        if (!NT_SUCCESS(st))
        {
            return st;
        }
        if constexpr (!std::is_same_v<Parse, std::nullptr_t>)
        {
            ParseReply(r);
        }
        return r.Ok() ? STATUS_SUCCESS : STATUS_INVALID_NETWORK_RESPONSE;
    }

    NTSTATUS Rpc(Writer &W, uint16_t Tag, uint8_t RType, size_t ReplyCap, std::vector<uint8_t> &Resp, Reader &R)
    {
        std::vector<uint8_t> &msg = W.Finish(Tag);
        DWORD returned = 0;

        if (Msize != 0 && msg.size() > Msize)
        {
            return STATUS_INVALID_PARAMETER;
        }

        Resp.resize(ReplyCap < SMALL_REPLY ? SMALL_REPLY : ReplyCap);
        NTSTATUS st;
        LARGE_INTEGER t0 = {}, t1, freq;
        if (TraceRpc)
        {
            QueryPerformanceCounter(&t0);
        }
        // STATUS_RETRY: the virtqueue was full and the request never reached
        // the device, so it is safe to send it again.
        for (int attempt = 0;; attempt++)
        {
            st = IoctlWithTimeout(Device,
                                  IOCTL_VIO9P_REQUEST,
                                  msg.data(),
                                  (DWORD)msg.size(),
                                  Resp.data(),
                                  (DWORD)Resp.size(),
                                  &returned,
                                  TimeoutMs);
            if (st != STATUS_RETRY || attempt >= 2000)
            {
                break;
            }
            Sleep(attempt < 10 ? 0 : 1);
        }
        if (TraceRpc)
        {
            QueryPerformanceCounter(&t1);
            QueryPerformanceFrequency(&freq);
            FspDebugLog("RPC %u %lu %lu us tid %lu\n",
                        (unsigned)msg[4],
                        (unsigned long)msg.size(),
                        (unsigned long)((t1.QuadPart - t0.QuadPart) * 1000000 / freq.QuadPart),
                        GetCurrentThreadId());
        }
        if (!NT_SUCCESS(st))
        {
            FspDebugLog("*** 9P: request type %u failed: 0x%08lx\n", (unsigned)msg[4], (unsigned long)st);
            return st;
        }

        Reader hdr(Resp.data(), returned);
        uint32_t size = hdr.u32();
        uint8_t type = hdr.u8();
        uint16_t tag = hdr.u16();
        if (!hdr.Ok() || size < HEADER_SIZE || size > returned || tag != Tag)
        {
            return STATUS_INVALID_NETWORK_RESPONSE;
        }

        R = Reader(Resp.data() + HEADER_SIZE, size - HEADER_SIZE);
        if (type == Rlerror)
        {
            uint32_t ecode = R.u32();
            FspDebugLog("*** 9P: request type %u failed with errno %u\n", (unsigned)msg[4], (unsigned)ecode);
            return R.Ok() ? ErrnoToNtStatus(ecode) : STATUS_INVALID_NETWORK_RESPONSE;
        }
        if (type != RType)
        {
            return STATUS_INVALID_NETWORK_RESPONSE;
        }
        return STATUS_SUCCESS;
    }
};

} // namespace p9
