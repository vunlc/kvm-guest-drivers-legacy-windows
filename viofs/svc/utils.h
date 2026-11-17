/*
 * Copyright (C) 2022 Daynix Computing Ltd.
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

#include <Windows.h>
#include <setupapi.h>
#include <cfgmgr32.h>

#include <functional>
#include <memory>
#include <string>
#include "scope_exit.h"

template <typename T> static DWORD RegistryGetVal(PCWSTR SubKey, PCWSTR ValueName, T &Value)
{
    LSTATUS Status;
    DWORD Val;
    DWORD ValSize = sizeof(Val);

    Status = RegGetValueW(HKEY_LOCAL_MACHINE, SubKey, ValueName, RRF_RT_REG_DWORD, NULL, &Val, &ValSize);
    if (Status == ERROR_SUCCESS)
    {
        Value = Val;
    }

    return Status;
}

static DWORD RegistryGetVal(PCWSTR SubKey, PCWSTR ValueName, std::wstring &Value)
{
    LSTATUS Status;
    DWORD BufSize = 0;
    std::unique_ptr<WCHAR[]> Buf;

    // Determine required buffer size
    Status = RegGetValueW(HKEY_LOCAL_MACHINE, SubKey, ValueName, RRF_RT_REG_SZ, NULL, NULL, &BufSize);
    if (Status != ERROR_SUCCESS)
    {
        return Status;
    }

    try
    {
        Buf = std::make_unique<WCHAR[]>(BufSize / sizeof(WCHAR));
    }
    catch (std::bad_alloc)
    {
        return ERROR_NO_SYSTEM_RESOURCES;
    }

    Status = RegGetValueW(HKEY_LOCAL_MACHINE, SubKey, ValueName, RRF_RT_REG_SZ, NULL, Buf.get(), &BufSize);
    if (Status == ERROR_SUCCESS)
    {
        Value.assign(Buf.get());
    }

    return Status;
}

static DWORD FindDeviceInterface(const GUID *ClassGuid, PHANDLE Device, DWORD MemberIndex)
{
    HDEVINFO DevInfo;
    SECURITY_ATTRIBUTES SecurityAttributes;
    SP_DEVICE_INTERFACE_DATA DevIfaceData;
    PSP_DEVICE_INTERFACE_DETAIL_DATA DevIfaceDetail = NULL;
    ULONG Length, RequiredLength = 0;

    DevInfo = SetupDiGetClassDevs(ClassGuid, NULL, NULL, (DIGCF_PRESENT | DIGCF_DEVICEINTERFACE));
    if (DevInfo == INVALID_HANDLE_VALUE)
    {
        return GetLastError();
    }
    SCOPE_EXIT(DevInfo, { SetupDiDestroyDeviceInfoList(DevInfo); });

    DevIfaceData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);

    if (!SetupDiEnumDeviceInterfaces(DevInfo, 0, ClassGuid, MemberIndex, &DevIfaceData))
    {
        return GetLastError();
    }

    SetupDiGetDeviceInterfaceDetail(DevInfo, &DevIfaceData, NULL, 0, &RequiredLength, NULL);

    DevIfaceDetail = (PSP_DEVICE_INTERFACE_DETAIL_DATA)LocalAlloc(LMEM_FIXED, RequiredLength);
    if (DevIfaceDetail == NULL)
    {
        return GetLastError();
    }
    SCOPE_EXIT(DevIfaceDetail, { LocalFree(DevIfaceDetail); });

    DevIfaceDetail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);
    Length = RequiredLength;

    if (!SetupDiGetDeviceInterfaceDetail(DevInfo, &DevIfaceData, DevIfaceDetail, Length, &RequiredLength, NULL))
    {
        return GetLastError();
    }

    SecurityAttributes.nLength = sizeof(SecurityAttributes);
    SecurityAttributes.lpSecurityDescriptor = NULL;
    SecurityAttributes.bInheritHandle = FALSE;

    *Device = CreateFile(DevIfaceDetail->DevicePath,
                         GENERIC_READ | GENERIC_WRITE,
                         0,
                         &SecurityAttributes,
                         OPEN_EXISTING,
                         0L,
                         NULL);
    if (*Device == INVALID_HANDLE_VALUE)
    {
        return GetLastError();
    }

    return ERROR_SUCCESS;
}

static DWORD FindDeviceInterface(const GUID *ClassGuid, PHANDLE Device, std::function<BOOLEAN(HANDLE Device)> cmp_fn)
{
    for (DWORD MemberIndex = 0;; MemberIndex++)
    {
        DWORD Error = FindDeviceInterface(ClassGuid, Device, MemberIndex);
        if (Error != ERROR_SUCCESS)
        {
            return Error;
        }

        if (cmp_fn(*Device))
        {
            return ERROR_SUCCESS;
        }
        else
        {
            CloseHandle(*Device);
        }
    }
}

static bool FileNameIgnoreCaseCompare(const char *a, const char *b)
{
    WCHAR wide_a[MAX_PATH];
    WCHAR wide_b[MAX_PATH];

    if (!MultiByteToWideChar(CP_UTF8, 0, a, -1, wide_a, MAX_PATH) ||
        !MultiByteToWideChar(CP_UTF8, 0, b, -1, wide_b, MAX_PATH))
    {
        return false;
    }

    return (CompareStringW(LOCALE_INVARIANT, NORM_IGNORECASE, wide_a, -1, wide_b, -1) == CSTR_EQUAL);
}

static bool FileNameIgnoreCaseCompare(PCWSTR a, const char *b, uint32_t b_len)
{
    WCHAR wide_b[MAX_PATH];

    int wide_b_len = MultiByteToWideChar(CP_UTF8, 0, b, b_len, wide_b, MAX_PATH);
    if (wide_b_len == 0)
    {
        return false;
    }

    return (CompareStringW(LOCALE_INVARIANT, NORM_IGNORECASE, a, -1, wide_b, wide_b_len) == CSTR_EQUAL);
}

// Device arrival / removal notifications.
//
// Windows 8 and later provide CM_Register_Notification. It does not exist on
// Windows 7, where the same events are delivered as WM_DEVICECHANGE messages to
// a window registered with RegisterDeviceNotification. Both back ends report
// events through DeviceNotifyCallback with a DeviceNotifyAction.

enum class DeviceNotifyAction
{
    InterfaceArrival,
    QueryRemove,
    QueryRemoveFailed,
    RemoveComplete,
    Other,
};

typedef void (*DeviceNotifyCallback)(PVOID Context, DeviceNotifyAction Action);

struct DeviceNotifyTarget
{
    DeviceNotifyCallback Callback{nullptr};
    PVOID Context{nullptr};

    void Invoke(DeviceNotifyAction Action)
    {
        if (Callback != nullptr)
        {
            Callback(Context, Action);
        }
    }
};

#if (_WIN32_WINNT >= _WIN32_WINNT_WIN8)

typedef HCMNOTIFICATION DEVICE_NOTIFY_HANDLE;

static DWORD CALLBACK CmNotifyCallback(HCMNOTIFICATION Notify,
                                       PVOID Context,
                                       CM_NOTIFY_ACTION Action,
                                       PCM_NOTIFY_EVENT_DATA EventData,
                                       DWORD EventDataSize)
{
    DeviceNotifyAction NotifyAction;

    UNREFERENCED_PARAMETER(Notify);
    UNREFERENCED_PARAMETER(EventData);
    UNREFERENCED_PARAMETER(EventDataSize);

    switch (Action)
    {
        case CM_NOTIFY_ACTION_DEVICEINTERFACEARRIVAL:
            NotifyAction = DeviceNotifyAction::InterfaceArrival;
            break;
        case CM_NOTIFY_ACTION_DEVICEQUERYREMOVE:
            NotifyAction = DeviceNotifyAction::QueryRemove;
            break;
        case CM_NOTIFY_ACTION_DEVICEQUERYREMOVEFAILED:
            NotifyAction = DeviceNotifyAction::QueryRemoveFailed;
            break;
        case CM_NOTIFY_ACTION_DEVICEREMOVECOMPLETE:
            NotifyAction = DeviceNotifyAction::RemoveComplete;
            break;
        default:
            NotifyAction = DeviceNotifyAction::Other;
            break;
    }

    static_cast<DeviceNotifyTarget *>(Context)->Invoke(NotifyAction);

    return ERROR_SUCCESS;
}

static DWORD DeviceNotifyRegister(DeviceNotifyTarget *Target, CM_NOTIFY_FILTER &Filter, DEVICE_NOTIFY_HANDLE *Handle)
{
    HCMNOTIFICATION NotifyHandle = nullptr;
    CONFIGRET ConfigRet = CM_Register_Notification(&Filter, Target, CmNotifyCallback, &NotifyHandle);

    if (ConfigRet == CR_SUCCESS)
    {
        *Handle = NotifyHandle;
    }

    return CM_MapCrToWin32Err(ConfigRet, ERROR_NOT_SUPPORTED);
}

static DWORD DeviceNotifyRegisterInterface(DeviceNotifyTarget *Target, const GUID &ClassGuid, DEVICE_NOTIFY_HANDLE *Handle)
{
    CM_NOTIFY_FILTER Filter;

    ZeroMemory(&Filter, sizeof(Filter));
    Filter.cbSize = sizeof(Filter);
    Filter.FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE;
    Filter.u.DeviceInterface.ClassGuid = ClassGuid;

    return DeviceNotifyRegister(Target, Filter, Handle);
}

static DWORD DeviceNotifyRegisterHandle(DeviceNotifyTarget *Target, HANDLE DeviceHandle, DEVICE_NOTIFY_HANDLE *Handle)
{
    CM_NOTIFY_FILTER Filter;

    ZeroMemory(&Filter, sizeof(Filter));
    Filter.cbSize = sizeof(Filter);
    Filter.FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEHANDLE;
    Filter.u.DeviceHandle.hTarget = DeviceHandle;

    return DeviceNotifyRegister(Target, Filter, Handle);
}

static void DeviceNotifyUnregister(DEVICE_NOTIFY_HANDLE Handle)
{
    CM_Unregister_Notification(Handle);
}

#else // Windows 7: RegisterDeviceNotification + hidden message-only window

#include <dbt.h>
#include <new>
#include <vector>

#pragma comment(lib, "user32.lib")

// Owns a message-only window on a dedicated thread that receives WM_DEVICECHANGE
// for every registration of the process and forwards it to the registered target.
//
// To mirror CM_Unregister_Notification, Unregister() does not return while a
// callback for that registration is still running (unless called from inside
// that very callback). Callbacks run one at a time on the window thread.
class DeviceNotifyWindow
{
  public:
    struct Registration
    {
        HDEVNOTIFY DevNotify{nullptr};
        DeviceNotifyTarget *Target{nullptr};
        bool IsInterface{false};
        GUID ClassGuid{};
        // Held by the window thread while the target's callback runs.
        CRITICAL_SECTION DispatchLock;
        bool InCallback{false};
        bool DeletePending{false};

        Registration()
        {
            InitializeCriticalSection(&DispatchLock);
        }
        ~Registration()
        {
            DeleteCriticalSection(&DispatchLock);
        }
    };

    static DeviceNotifyWindow &Instance()
    {
        static DeviceNotifyWindow Singleton;
        return Singleton;
    }

    DWORD RegisterInterface(DeviceNotifyTarget *Target, const GUID &ClassGuid, Registration **Reg)
    {
        DEV_BROADCAST_DEVICEINTERFACE_W Filter;

        ZeroMemory(&Filter, sizeof(Filter));
        Filter.dbcc_size = sizeof(Filter);
        Filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
        Filter.dbcc_classguid = ClassGuid;

        return Register(Target, &Filter, true, ClassGuid, Reg);
    }

    DWORD RegisterHandle(DeviceNotifyTarget *Target, HANDLE DeviceHandle, Registration **Reg)
    {
        DEV_BROADCAST_HANDLE Filter;

        ZeroMemory(&Filter, sizeof(Filter));
        Filter.dbch_size = sizeof(Filter);
        Filter.dbch_devicetype = DBT_DEVTYP_HANDLE;
        Filter.dbch_handle = DeviceHandle;

        return Register(Target, &Filter, false, GUID{}, Reg);
    }

    void Unregister(Registration *Reg)
    {
        if (Reg == nullptr)
        {
            return;
        }

        AcquireSRWLockExclusive(&Lock);
        for (auto it = Registrations.begin(); it != Registrations.end(); ++it)
        {
            if (*it == Reg)
            {
                Registrations.erase(it);
                break;
            }
        }
        ReleaseSRWLockExclusive(&Lock);

        UnregisterDeviceNotification(Reg->DevNotify);

        // Wait for a callback of this registration that may still be running.
        // If we are that callback (the lock is recursive), let Dispatch free it.
        EnterCriticalSection(&Reg->DispatchLock);
        bool InOwnCallback = Reg->InCallback;
        if (InOwnCallback)
        {
            Reg->DeletePending = true;
        }
        LeaveCriticalSection(&Reg->DispatchLock);

        if (!InOwnCallback)
        {
            delete Reg;
        }
    }

  private:
    SRWLOCK Lock = SRWLOCK_INIT;
    std::vector<Registration *> Registrations;
    INIT_ONCE StartOnce = INIT_ONCE_STATIC_INIT;
    HWND Window{nullptr};
    HANDLE ReadyEvent{nullptr};
    DWORD StartError{ERROR_SUCCESS};

    DeviceNotifyWindow() = default;
    DeviceNotifyWindow(const DeviceNotifyWindow &) = delete;
    DeviceNotifyWindow &operator=(const DeviceNotifyWindow &) = delete;

    DWORD Start()
    {
        auto StartFn = [](PINIT_ONCE InitOnce, PVOID Parameter, PVOID *Context) -> BOOL {
            auto Self = static_cast<DeviceNotifyWindow *>(Parameter);

            UNREFERENCED_PARAMETER(InitOnce);
            UNREFERENCED_PARAMETER(Context);

            Self->ReadyEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
            if (Self->ReadyEvent == NULL)
            {
                Self->StartError = GetLastError();
                return TRUE;
            }

            HANDLE Thread = CreateThread(NULL, 0, ThreadProc, Self, 0, NULL);
            if (Thread == NULL)
            {
                Self->StartError = GetLastError();
                return TRUE;
            }
            CloseHandle(Thread);

            WaitForSingleObject(Self->ReadyEvent, INFINITE);
            return TRUE;
        };

        InitOnceExecuteOnce(&StartOnce, StartFn, this, NULL);

        return StartError;
    }

    DWORD Register(DeviceNotifyTarget *Target, PVOID Filter, bool IsInterface, const GUID &ClassGuid, Registration **Reg)
    {
        DWORD Error = Start();
        if (Error != ERROR_SUCCESS)
        {
            return Error;
        }

        Registration *NewReg = new (std::nothrow) Registration();
        if (NewReg == nullptr)
        {
            return ERROR_NOT_ENOUGH_MEMORY;
        }
        NewReg->Target = Target;
        NewReg->IsInterface = IsInterface;
        NewReg->ClassGuid = ClassGuid;

        // Not under Lock: this may be called on the window thread from a callback.
        NewReg->DevNotify = RegisterDeviceNotificationW(Window, Filter, DEVICE_NOTIFY_WINDOW_HANDLE);
        if (NewReg->DevNotify == NULL)
        {
            Error = GetLastError();
            delete NewReg;
            return Error;
        }

        AcquireSRWLockExclusive(&Lock);
        try
        {
            Registrations.push_back(NewReg);
        }
        catch (std::bad_alloc)
        {
            ReleaseSRWLockExclusive(&Lock);
            UnregisterDeviceNotification(NewReg->DevNotify);
            delete NewReg;
            return ERROR_NOT_ENOUGH_MEMORY;
        }
        ReleaseSRWLockExclusive(&Lock);

        *Reg = NewReg;
        return ERROR_SUCCESS;
    }

    void Dispatch(PDEV_BROADCAST_HDR Hdr, DeviceNotifyAction Action)
    {
        Registration *Match = nullptr;

        AcquireSRWLockShared(&Lock);
        for (auto Reg : Registrations)
        {
            if (Hdr->dbch_devicetype == DBT_DEVTYP_DEVICEINTERFACE && Reg->IsInterface &&
                IsEqualGUID(reinterpret_cast<PDEV_BROADCAST_DEVICEINTERFACE_W>(Hdr)->dbcc_classguid,
                            Reg->ClassGuid))
            {
                Match = Reg;
                break;
            }
            if (Hdr->dbch_devicetype == DBT_DEVTYP_HANDLE && !Reg->IsInterface &&
                reinterpret_cast<PDEV_BROADCAST_HANDLE>(Hdr)->dbch_hdevnotify == Reg->DevNotify)
            {
                Match = Reg;
                break;
            }
        }
        if (Match != nullptr)
        {
            EnterCriticalSection(&Match->DispatchLock);
        }
        ReleaseSRWLockShared(&Lock);

        if (Match != nullptr)
        {
            Match->InCallback = true;
            Match->Target->Invoke(Action);
            Match->InCallback = false;
            bool Delete = Match->DeletePending;
            LeaveCriticalSection(&Match->DispatchLock);
            if (Delete)
            {
                delete Match;
            }
        }
    }

    static LRESULT CALLBACK WndProc(HWND Wnd, UINT Msg, WPARAM WParam, LPARAM LParam)
    {
        if (Msg != WM_DEVICECHANGE)
        {
            return DefWindowProcW(Wnd, Msg, WParam, LParam);
        }

        auto Hdr = reinterpret_cast<PDEV_BROADCAST_HDR>(LParam);
        if (Hdr == nullptr)
        {
            return TRUE;
        }

        switch (WParam)
        {
            case DBT_DEVICEARRIVAL:
                if (Hdr->dbch_devicetype == DBT_DEVTYP_DEVICEINTERFACE)
                {
                    Instance().Dispatch(Hdr, DeviceNotifyAction::InterfaceArrival);
                }
                break;
            case DBT_DEVICEQUERYREMOVE:
                Instance().Dispatch(Hdr, DeviceNotifyAction::QueryRemove);
                break;
            case DBT_DEVICEQUERYREMOVEFAILED:
                Instance().Dispatch(Hdr, DeviceNotifyAction::QueryRemoveFailed);
                break;
            case DBT_DEVICEREMOVECOMPLETE:
                if (Hdr->dbch_devicetype == DBT_DEVTYP_HANDLE)
                {
                    Instance().Dispatch(Hdr, DeviceNotifyAction::RemoveComplete);
                }
                break;
            default:
                break;
        }

        // TRUE grants DBT_DEVICEQUERYREMOVE; the handle has been closed by then.
        return TRUE;
    }

    static DWORD WINAPI ThreadProc(LPVOID Parameter)
    {
        auto Self = static_cast<DeviceNotifyWindow *>(Parameter);
        WNDCLASSEXW WndClass;
        MSG Msg;

        ZeroMemory(&WndClass, sizeof(WndClass));
        WndClass.cbSize = sizeof(WndClass);
        WndClass.lpfnWndProc = WndProc;
        WndClass.hInstance = GetModuleHandleW(NULL);
        WndClass.lpszClassName = L"VirtioFsDeviceNotify";

        if (!RegisterClassExW(&WndClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        {
            Self->StartError = GetLastError();
            SetEvent(Self->ReadyEvent);
            return Self->StartError;
        }

        Self->Window = CreateWindowExW(0,
                                       WndClass.lpszClassName,
                                       L"",
                                       0,
                                       0,
                                       0,
                                       0,
                                       0,
                                       HWND_MESSAGE,
                                       NULL,
                                       WndClass.hInstance,
                                       NULL);
        if (Self->Window == NULL)
        {
            Self->StartError = GetLastError();
            SetEvent(Self->ReadyEvent);
            return Self->StartError;
        }

        SetEvent(Self->ReadyEvent);

        while (GetMessageW(&Msg, NULL, 0, 0) > 0)
        {
            TranslateMessage(&Msg);
            DispatchMessageW(&Msg);
        }

        return ERROR_SUCCESS;
    }
};

typedef DeviceNotifyWindow::Registration *DEVICE_NOTIFY_HANDLE;

static DWORD DeviceNotifyRegisterInterface(DeviceNotifyTarget *Target, const GUID &ClassGuid, DEVICE_NOTIFY_HANDLE *Handle)
{
    return DeviceNotifyWindow::Instance().RegisterInterface(Target, ClassGuid, Handle);
}

static DWORD DeviceNotifyRegisterHandle(DeviceNotifyTarget *Target, HANDLE DeviceHandle, DEVICE_NOTIFY_HANDLE *Handle)
{
    return DeviceNotifyWindow::Instance().RegisterHandle(Target, DeviceHandle, Handle);
}

static void DeviceNotifyUnregister(DEVICE_NOTIFY_HANDLE Handle)
{
    DeviceNotifyWindow::Instance().Unregister(Handle);
}

#endif // _WIN32_WINNT >= _WIN32_WINNT_WIN8

class DeviceInterfaceNotification : private DeviceNotifyTarget
{
    DEVICE_NOTIFY_HANDLE Handle{nullptr};

    DeviceInterfaceNotification(const DeviceInterfaceNotification &) = delete;
    DeviceInterfaceNotification &operator=(const DeviceInterfaceNotification &) = delete;
    DeviceInterfaceNotification(DeviceInterfaceNotification &&) = delete;
    DeviceInterfaceNotification &operator=(DeviceInterfaceNotification &&) = delete;

  public:
    DeviceInterfaceNotification() = default;

    DWORD Register(DeviceNotifyCallback pCallback, PVOID pContext, const GUID &ClassGuid)
    {
        DEVICE_NOTIFY_HANDLE NotifyHandle = nullptr;
        DWORD Error;

        Callback = pCallback;
        Context = pContext;

        Error = DeviceNotifyRegisterInterface(this, ClassGuid, &NotifyHandle);
        if (Error == ERROR_SUCCESS)
        {
            Handle = NotifyHandle;
        }

        return Error;
    }

    void Unregister()
    {
        DeviceNotifyUnregister(Handle);
        Handle = nullptr;
    }
};

class DeviceHandleNotification : private DeviceNotifyTarget
{
    DEVICE_NOTIFY_HANDLE Handle{nullptr};
    CRITICAL_SECTION Lock;
    BOOL UnregInProgress{FALSE};
    PTP_WORK UnregWork{nullptr};

    DeviceHandleNotification(const DeviceHandleNotification &) = delete;
    DeviceHandleNotification &operator=(const DeviceHandleNotification &) = delete;
    DeviceHandleNotification(DeviceHandleNotification &&) = delete;
    DeviceHandleNotification &operator=(DeviceHandleNotification &&) = delete;

    void DoUnregister()
    {
        DeviceNotifyUnregister(Handle);
        Handle = nullptr;
    }

  public:
    DeviceHandleNotification()
    {
        InitializeCriticalSection(&Lock);
    }

    ~DeviceHandleNotification()
    {
        if (UnregWork != nullptr)
        {
            CloseThreadpoolWork(UnregWork);
        }

        DeleteCriticalSection(&Lock);
    }

    DWORD Register(DeviceNotifyCallback pCallback, PVOID pContext, HANDLE DeviceHandle)
    {
        DEVICE_NOTIFY_HANDLE NotifyHandle = nullptr;
        DWORD Error;

        Callback = pCallback;
        Context = pContext;

        Error = DeviceNotifyRegisterHandle(this, DeviceHandle, &NotifyHandle);
        if (Error == ERROR_SUCCESS)
        {
            Handle = NotifyHandle;
            UnregInProgress = FALSE;
        }

        return Error;
    }

    void Unregister()
    {
        BOOL ShouldUnregister = FALSE;

        EnterCriticalSection(&Lock);

        if (!UnregInProgress)
        {
            UnregInProgress = TRUE;
            ShouldUnregister = TRUE;
        }

        LeaveCriticalSection(&Lock);

        if (ShouldUnregister)
        {
            DoUnregister();
        }
        else
        {
            WaitForThreadpoolWorkCallbacks(UnregWork, FALSE);
        }
    }

    // Must be used instead of Unregister() when unregistering
    // device notification callback from itself.
    void AsyncUnregister()
    {
        EnterCriticalSection(&Lock);

        if (!UnregInProgress)
        {
            UnregInProgress = TRUE;
            SubmitThreadpoolWork(UnregWork);
        }

        LeaveCriticalSection(&Lock);
    }

    bool CreateUnregWork()
    {
        auto cb = [](PTP_CALLBACK_INSTANCE Instance, PVOID Param, PTP_WORK Work) {
            auto Notification = static_cast<DeviceHandleNotification *>(Param);

            UNREFERENCED_PARAMETER(Instance);
            UNREFERENCED_PARAMETER(Work);

            Notification->DoUnregister();
        };
        UnregWork = CreateThreadpoolWork(cb, this, nullptr);

        return (UnregWork != nullptr);
    }

    void WaitForUnregWork()
    {
        WaitForThreadpoolWorkCallbacks(UnregWork, FALSE);
    }
};

static bool ParseIds(const std::wstring &ids, uint32_t &uid, uint32_t &gid)
{
    return (swscanf_s(ids.c_str(), L"%u:%u", &uid, &gid) == 2);
}

static bool CheckIds(const std::wstring &ids)
{
    uint32_t uid, gid;

    return ParseIds(ids, uid, gid);
}
