/*
 * vio9p.sys - interrupt handling. Derived from the viofs driver.
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
#include "isrdpc.tmh"

NTSTATUS Vio9pEvtInterruptEnable(IN WDFINTERRUPT Interrupt, IN WDFDEVICE AssociatedDevice)
{
    PDEVICE_CONTEXT context = GetDeviceContext(WdfInterruptGetDevice(Interrupt));

    UNREFERENCED_PARAMETER(AssociatedDevice);

    TraceEvents(TRACE_LEVEL_VERBOSE, DBG_INTERRUPT, "<-> %!FUNC!");

    if (context->VirtQueue != NULL)
    {
        virtqueue_enable_cb(context->VirtQueue);
        virtqueue_kick(context->VirtQueue);
    }

    return STATUS_SUCCESS;
}

NTSTATUS Vio9pEvtInterruptDisable(IN WDFINTERRUPT Interrupt, IN WDFDEVICE AssociatedDevice)
{
    PDEVICE_CONTEXT context = GetDeviceContext(WdfInterruptGetDevice(Interrupt));

    UNREFERENCED_PARAMETER(AssociatedDevice);

    TraceEvents(TRACE_LEVEL_VERBOSE, DBG_INTERRUPT, "<-> %!FUNC!");

    if (context->VirtQueue != NULL)
    {
        virtqueue_disable_cb(context->VirtQueue);
    }

    return STATUS_SUCCESS;
}

BOOLEAN Vio9pEvtInterruptIsr(IN WDFINTERRUPT Interrupt, IN ULONG MessageId)
{
    PDEVICE_CONTEXT context = GetDeviceContext(WdfInterruptGetDevice(Interrupt));
    WDF_INTERRUPT_INFO info;

    UNREFERENCED_PARAMETER(MessageId);

    WDF_INTERRUPT_INFO_INIT(&info);
    WdfInterruptGetInfo(Interrupt, &info);

    // MSI messages are never shared; a line interrupt is ours only if the
    // device's ISR status says so (reading it also acknowledges it).
    if (info.MessageSignaled || VirtIOWdfGetISRStatus(&context->VDevice))
    {
        WdfInterruptQueueDpcForIsr(Interrupt);
        return TRUE;
    }

    return FALSE;
}

void Vio9pDrainQueue(PDEVICE_CONTEXT Context)
{
    PVIO9P_REQUEST req;
    unsigned int length = 0;

    for (;;)
    {
        WdfSpinLockAcquire(Context->VirtQueueLock);
        req = (Context->VirtQueue != NULL) ? virtqueue_get_buf(Context->VirtQueue, &length) : NULL;
        WdfSpinLockRelease(Context->VirtQueueLock);

        if (req == NULL)
        {
            break;
        }

        Vio9pCompleteRequest(Context, req, (ULONG)length);
    }
}

VOID Vio9pEvtInterruptDpc(IN WDFINTERRUPT Interrupt, IN WDFOBJECT AssociatedObject)
{
    UNREFERENCED_PARAMETER(AssociatedObject);

    TraceEvents(TRACE_LEVEL_VERBOSE, DBG_DPC, "<-> %!FUNC!");

    Vio9pDrainQueue(GetDeviceContext(WdfInterruptGetDevice(Interrupt)));
}

VOID Vio9pEvtWatchdogTimer(IN WDFTIMER Timer)
{
    PDEVICE_CONTEXT context = GetDeviceContext(WdfTimerGetParentObject(Timer));
    BOOLEAN outstanding;

    WdfSpinLockAcquire(context->RequestsLock);
    outstanding = (context->RequestsList.Next != NULL);
    WdfSpinLockRelease(context->RequestsLock);

    if (!outstanding)
    {
        return;
    }

    // Collect replies whose interrupt was lost, then re-notify the device in
    // case a notification for a still-pending request was lost.
    Vio9pDrainQueue(context);

    WdfSpinLockAcquire(context->VirtQueueLock);
    if (context->VirtQueue != NULL)
    {
        virtqueue_kick(context->VirtQueue);
    }
    WdfSpinLockRelease(context->VirtQueueLock);
}
