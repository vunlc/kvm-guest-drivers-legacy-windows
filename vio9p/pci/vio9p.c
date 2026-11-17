/*
 * vio9p.sys - driver and device setup. Derived from the viofs driver.
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

#include "vio9p.h"
#include "vio9p.tmh"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(INIT, DriverEntry)
#pragma alloc_text(PAGE, Vio9pEvtDeviceAdd)
#pragma alloc_text(PAGE, Vio9pEvtDriverContextCleanup)
#endif

NTSTATUS DriverEntry(IN PDRIVER_OBJECT DriverObject, IN PUNICODE_STRING RegistryPath)
{
    NTSTATUS status;
    WDF_DRIVER_CONFIG config;
    WDF_OBJECT_ATTRIBUTES attributes;

#if (NTDDI_VERSION > NTDDI_WIN7)
    ExInitializeDriverRuntime(DrvRtPoolNxOptIn);
#endif

    WPP_INIT_TRACING(DriverObject, RegistryPath);

    TraceEvents(TRACE_LEVEL_VERBOSE, DBG_INIT, "--> %!FUNC!");

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.EvtCleanupCallback = Vio9pEvtDriverContextCleanup;

    WDF_DRIVER_CONFIG_INIT(&config, Vio9pEvtDeviceAdd);

    status = WdfDriverCreate(DriverObject, RegistryPath, &attributes, &config, WDF_NO_HANDLE);
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_INIT, "WdfDriverCreate failed: %!STATUS!", status);
        WPP_CLEANUP(DriverObject);
    }

    TraceEvents(TRACE_LEVEL_VERBOSE, DBG_INIT, "<-- %!FUNC!");

    return status;
}

NTSTATUS Vio9pEvtDeviceAdd(IN WDFDRIVER Driver, IN PWDFDEVICE_INIT DeviceInit)
{
    NTSTATUS status;
    WDFDEVICE device;
    WDF_PNPPOWER_EVENT_CALLBACKS pnpPowerCallbacks;
    WDF_OBJECT_ATTRIBUTES attributes;
    WDFQUEUE queue;
    WDF_IO_QUEUE_CONFIG queueConfig;
    WDF_INTERRUPT_CONFIG interruptConfig;
    PDEVICE_CONTEXT context;

    UNREFERENCED_PARAMETER(Driver);

    PAGED_CODE();

    TraceEvents(TRACE_LEVEL_VERBOSE, DBG_INIT, "--> %!FUNC!");

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpPowerCallbacks);
    pnpPowerCallbacks.EvtDevicePrepareHardware = Vio9pEvtDevicePrepareHardware;
    pnpPowerCallbacks.EvtDeviceReleaseHardware = Vio9pEvtDeviceReleaseHardware;
    pnpPowerCallbacks.EvtDeviceD0Entry = Vio9pEvtDeviceD0Entry;
    pnpPowerCallbacks.EvtDeviceD0Exit = Vio9pEvtDeviceD0Exit;

    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpPowerCallbacks);
    WdfDeviceInitSetIoType(DeviceInit, WdfDeviceIoDirect);

    // The device accepts raw 9P messages for the host, so only SYSTEM (the
    // vio9p service) and Administrators may open it. That is enforced by the
    // Security and DeviceCharacteristics (FILE_DEVICE_SECURE_OPEN) values in
    // vio9p.inf, which the PnP manager applies to the whole device stack.
    // (Assigning an SDDL here makes AddDevice fail for this PnP device.)

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, DEVICE_CONTEXT);
    attributes.EvtCleanupCallback = Vio9pEvtDeviceContextCleanup;

    status = WdfDeviceCreate(&DeviceInit, &attributes, &device);
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_INIT, "WdfDeviceCreate failed: %!STATUS!", status);
        return status;
    }

    context = GetDeviceContext(device);

    // One interrupt: the single request queue. Configuration changes are not used.
    WDF_INTERRUPT_CONFIG_INIT(&interruptConfig, Vio9pEvtInterruptIsr, Vio9pEvtInterruptDpc);
    interruptConfig.EvtInterruptEnable = Vio9pEvtInterruptEnable;
    interruptConfig.EvtInterruptDisable = Vio9pEvtInterruptDisable;

    status = WdfInterruptCreate(device, &interruptConfig, WDF_NO_OBJECT_ATTRIBUTES, &context->WdfInterrupt);
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_INIT, "WdfInterruptCreate failed: %!STATUS!", status);
        return status;
    }

    {
        WDF_TIMER_CONFIG timerConfig;

        WDF_TIMER_CONFIG_INIT_PERIODIC(&timerConfig, Vio9pEvtWatchdogTimer, VIO9P_WATCHDOG_PERIOD_MS);
        timerConfig.AutomaticSerialization = FALSE;
        WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
        attributes.ParentObject = device;
        status = WdfTimerCreate(&timerConfig, &attributes, &context->WatchdogTimer);
        if (!NT_SUCCESS(status))
        {
            TraceEvents(TRACE_LEVEL_ERROR, DBG_INIT, "WdfTimerCreate failed: %!STATUS!", status);
            return status;
        }
    }

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = device;
    status = WdfSpinLockCreate(&attributes, &context->RequestsLock);
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_INIT, "WdfSpinLockCreate failed: %!STATUS!", status);
        return status;
    }

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = device;
    status = WdfSpinLockCreate(&attributes, &context->VirtQueueLock);
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_INIT, "WdfSpinLockCreate failed: %!STATUS!", status);
        return status;
    }

    status = WdfDeviceCreateDeviceInterface(device, &GUID_DEVINTERFACE_VIO9P, NULL);
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_INIT, "WdfDeviceCreateDeviceInterface failed: %!STATUS!", status);
        return status;
    }

    // Parallel dispatch: many 9P requests can be on the ring at once (the
    // service pipelines per-entry lookups when listing directories). Each
    // request has its own DMA transactions; the virtqueue and request list are
    // protected by spinlocks. A full ring fails the request with
    // STATUS_RETRY, which the service resends.
    WDF_IO_QUEUE_CONFIG_INIT(&queueConfig, WdfIoQueueDispatchParallel);
    queueConfig.EvtIoDeviceControl = Vio9pEvtIoDeviceControl;
    queueConfig.EvtIoStop = Vio9pEvtIoStop;
    queueConfig.AllowZeroLengthRequests = FALSE;

    status = WdfIoQueueCreate(device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, &queue);
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_INIT, "WdfIoQueueCreate failed: %!STATUS!", status);
        return status;
    }

    status = WdfDeviceConfigureRequestDispatching(device, queue, WdfRequestTypeDeviceControl);
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_INIT, "WdfDeviceConfigureRequestDispatching failed: %!STATUS!", status);
        return status;
    }

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = device;
    status = WdfLookasideListCreate(&attributes,
                                    sizeof(VIO9P_REQUEST),
                                    NonPagedPool,
                                    WDF_NO_OBJECT_ATTRIBUTES,
                                    VIO9P_MEMORY_TAG,
                                    &context->RequestsLookaside);
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_INIT, "WdfLookasideListCreate failed: %!STATUS!", status);
        return status;
    }

    TraceEvents(TRACE_LEVEL_VERBOSE, DBG_INIT, "<-- %!FUNC!");

    return status;
}

VOID Vio9pEvtDeviceContextCleanup(IN WDFOBJECT DeviceObject)
{
    PDEVICE_CONTEXT context = GetDeviceContext(DeviceObject);
    PSINGLE_LIST_ENTRY iter;

    TraceEvents(TRACE_LEVEL_VERBOSE, DBG_INIT, "--> %!FUNC!");

    if (context->RequestsLock == NULL)
    {
        return;
    }

    WdfSpinLockAcquire(context->RequestsLock);
    iter = PopEntryList(&context->RequestsList);
    while (iter != NULL)
    {
        Vio9pFreeRequest(CONTAINING_RECORD(iter, VIO9P_REQUEST, ListEntry));
        iter = PopEntryList(&context->RequestsList);
    }
    WdfSpinLockRelease(context->RequestsLock);

    TraceEvents(TRACE_LEVEL_VERBOSE, DBG_INIT, "<-- %!FUNC!");
}

VOID Vio9pEvtDriverContextCleanup(IN WDFOBJECT DriverObject)
{
    UNREFERENCED_PARAMETER(DriverObject);

    PAGED_CODE();

    TraceEvents(TRACE_LEVEL_VERBOSE, DBG_INIT, "<-> %!FUNC!");

    WPP_CLEANUP(WdfDriverWdmGetDriverObject(DriverObject));
}

NTSTATUS Vio9pAllocateRequest(IN PDEVICE_CONTEXT Context, OUT PVIO9P_REQUEST *Request)
{
    WDFMEMORY handle;
    NTSTATUS status = WdfMemoryCreateFromLookaside(Context->RequestsLookaside, &handle);

    if (!NT_SUCCESS(status))
    {
        *Request = NULL;
        TraceEvents(TRACE_LEVEL_ERROR, DBG_IOCTL, "WdfMemoryCreateFromLookaside failed: %!STATUS!", status);
        return status;
    }

    *Request = WdfMemoryGetBuffer(handle, NULL);
    RtlZeroMemory(*Request, FIELD_OFFSET(VIO9P_REQUEST, SGTable));
    (*Request)->Handle = handle;

    return STATUS_SUCCESS;
}

void Vio9pFreeRequest(IN PVIO9P_REQUEST Request)
{
    if (Request->Handle != NULL)
    {
        WDFMEMORY handle = Request->Handle;
        Request->Handle = NULL;
        WdfObjectDelete(handle);
    }
}
