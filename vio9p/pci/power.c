/*
 * vio9p.sys - PnP and power callbacks. Derived from the viofs driver.
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
#include "power.tmh"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, Vio9pEvtDevicePrepareHardware)
#pragma alloc_text(PAGE, Vio9pEvtDeviceReleaseHardware)
#pragma alloc_text(PAGE, Vio9pEvtDeviceD0Entry)
#pragma alloc_text(PAGE, Vio9pEvtDeviceD0Exit)
#endif

static VOID Vio9pReadMountTag(PDEVICE_CONTEXT Context, BOOLEAN HasMountTag)
{
    USHORT tagLen = 0;

    RtlZeroMemory(Context->Tag, sizeof(Context->Tag));

    if (!HasMountTag)
    {
        TraceEvents(TRACE_LEVEL_WARNING, DBG_POWER, "Device does not offer a mount tag");
        return;
    }

    VirtIOWdfDeviceGet(&Context->VDevice, VIO9P_CONFIG_TAG_LEN_OFFSET, &tagLen, sizeof(tagLen));
    if (tagLen > VIO9P_MAX_TAG_LEN)
    {
        TraceEvents(TRACE_LEVEL_WARNING, DBG_POWER, "Mount tag length %u truncated", tagLen);
        tagLen = VIO9P_MAX_TAG_LEN;
    }
    if (tagLen != 0)
    {
        VirtIOWdfDeviceGet(&Context->VDevice, VIO9P_CONFIG_TAG_OFFSET, Context->Tag, tagLen);
    }

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_POWER, "Mount tag: '%s'", Context->Tag);
}

NTSTATUS Vio9pEvtDevicePrepareHardware(IN WDFDEVICE Device,
                                       IN WDFCMRESLIST Resources,
                                       IN WDFCMRESLIST ResourcesTranslated)
{
    PDEVICE_CONTEXT context = GetDeviceContext(Device);
    NTSTATUS status;
    u64 hostFeatures, guestFeatures = 0;

    UNREFERENCED_PARAMETER(Resources);

    PAGED_CODE();

    TraceEvents(TRACE_LEVEL_VERBOSE, DBG_POWER, "--> %!FUNC! Device: %p", Device);

    status = VirtIOWdfInitialize(&context->VDevice, Device, ResourcesTranslated, NULL, VIO9P_MEMORY_TAG);
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_POWER, "VirtIOWdfInitialize failed: %!STATUS!", status);
        return status;
    }

    hostFeatures = VirtIOWdfGetDeviceFeatures(&context->VDevice);
    if (virtio_is_feature_enabled(hostFeatures, VIRTIO_9P_MOUNT_TAG))
    {
        virtio_feature_enable(guestFeatures, VIRTIO_9P_MOUNT_TAG);
    }

    // Indirect descriptors are deliberately not negotiated: a request needs far
    // fewer descriptors than the queue holds, and per-request indirect tables
    // would have to be managed separately for requests that outlive a cancel.
    status = VirtIOWdfSetDriverFeatures(&context->VDevice, guestFeatures, 0);
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_POWER, "VirtIOWdfSetDriverFeatures failed: %!STATUS!", status);
        VirtIOWdfShutdown(&context->VDevice);
        return status;
    }

    Vio9pReadMountTag(context, virtio_is_feature_enabled(guestFeatures, VIRTIO_9P_MOUNT_TAG));

    TraceEvents(TRACE_LEVEL_VERBOSE, DBG_POWER, "<-- %!FUNC!");

    return STATUS_SUCCESS;
}

NTSTATUS Vio9pEvtDeviceReleaseHardware(IN WDFDEVICE Device, IN WDFCMRESLIST ResourcesTranslated)
{
    PDEVICE_CONTEXT context = GetDeviceContext(Device);

    UNREFERENCED_PARAMETER(ResourcesTranslated);

    PAGED_CODE();

    TraceEvents(TRACE_LEVEL_VERBOSE, DBG_POWER, "<-> %!FUNC!");

    VirtIOWdfShutdown(&context->VDevice);

    return STATUS_SUCCESS;
}

NTSTATUS Vio9pEvtDeviceD0Entry(IN WDFDEVICE Device, IN WDF_POWER_DEVICE_STATE PreviousState)
{
    PDEVICE_CONTEXT context = GetDeviceContext(Device);
    VIRTIO_WDF_QUEUE_PARAM param;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(PreviousState);

    PAGED_CODE();

    TraceEvents(TRACE_LEVEL_VERBOSE, DBG_POWER, "--> %!FUNC! Device: %p", Device);

    param.Interrupt = context->WdfInterrupt;

    status = VirtIOWdfInitQueues(&context->VDevice, 1, &context->VirtQueue, &param);
    if (NT_SUCCESS(status))
    {
        context->QueueSize = virtio_get_queue_size(context->VirtQueue);
        TraceEvents(TRACE_LEVEL_INFORMATION, DBG_POWER, "Queue size %u", context->QueueSize);
        VirtIOWdfSetDriverOK(&context->VDevice);
        WdfTimerStart(context->WatchdogTimer, WDF_REL_TIMEOUT_IN_MS(VIO9P_WATCHDOG_PERIOD_MS));
    }
    else
    {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_POWER, "VirtIOWdfInitQueues failed: %!STATUS!", status);
        VirtIOWdfSetDriverFailed(&context->VDevice);
    }

    TraceEvents(TRACE_LEVEL_VERBOSE, DBG_POWER, "<-- %!FUNC!");

    return status;
}

NTSTATUS Vio9pEvtDeviceD0Exit(IN WDFDEVICE Device, IN WDF_POWER_DEVICE_STATE TargetState)
{
    PDEVICE_CONTEXT context = GetDeviceContext(Device);

    UNREFERENCED_PARAMETER(TargetState);

    PAGED_CODE();

    TraceEvents(TRACE_LEVEL_VERBOSE, DBG_POWER, "<-> %!FUNC! Device: %p", Device);

    // Stop the watchdog (waiting for a running callback), then hide the queue
    // from DPCs before tearing it down.
    WdfTimerStop(context->WatchdogTimer, TRUE);

    WdfSpinLockAcquire(context->VirtQueueLock);
    context->VirtQueue = NULL;
    context->QueueSize = 0;
    WdfSpinLockRelease(context->VirtQueueLock);

    VirtIOWdfDestroyQueues(&context->VDevice);

    return STATUS_SUCCESS;
}
