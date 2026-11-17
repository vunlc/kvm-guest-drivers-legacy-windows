/*
 * vio9p.sys - IOCTL handling: one IOCTL_VIO9P_REQUEST carries one 9P
 * T-message to the device and returns the R-message. Derived from viofs.
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
#include "ioctl.tmh"

EVT_WDF_REQUEST_CANCEL Vio9pEvtRequestCancel;

// Takes ownership of the WDF request back from the cancel machinery.
// Returns TRUE if the caller must complete it; FALSE if the cancel routine
// has completed or will complete it.
static BOOLEAN Vio9pReclaimWdfRequest(WDFREQUEST WdfRequest)
{
    if (WdfRequest == NULL)
    {
        return FALSE;
    }
    return WdfRequestUnmarkCancelable(WdfRequest) != STATUS_CANCELLED;
}

// Fails a request that was put on RequestsList but never reached the ring.
static void Vio9pFailRequest(IN PDEVICE_CONTEXT Context, IN PVIO9P_REQUEST Request, IN NTSTATUS Status)
{
    WDFREQUEST wdfReq;

    TraceEvents(TRACE_LEVEL_ERROR, DBG_IOCTL, "Failing request %p: %!STATUS!", Request, Status);

    if (Request->H2D_Params.transaction)
    {
        VirtIOWdfDeviceDmaTxComplete(&Context->VDevice.VIODevice, Request->H2D_Params.transaction);
        Request->H2D_Params.transaction = NULL;
    }
    if (Request->D2H_Params.transaction)
    {
        VirtIOWdfDeviceDmaRxComplete(&Context->VDevice.VIODevice, Request->D2H_Params.transaction, 0);
        Request->D2H_Params.transaction = NULL;
    }

    wdfReq = Vio9pDequeueRequest(Context, Request);
    if (Vio9pReclaimWdfRequest(wdfReq))
    {
        WdfRequestComplete(wdfReq, Status);
    }

    Vio9pFreeRequest(Request);
}

void Vio9pCompleteRequest(PDEVICE_CONTEXT Context, PVIO9P_REQUEST Request, ULONG Length)
{
    WDFREQUEST wdfReq = Vio9pDequeueRequest(Context, Request);
    BOOLEAN own = Vio9pReclaimWdfRequest(wdfReq);

    // The device cannot have written more than the buffers it was given.
    if (Length > Request->D2H_Params.size)
    {
        Length = Request->D2H_Params.size;
    }

    VirtIOWdfDeviceDmaTxComplete(&Context->VDevice.VIODevice, Request->H2D_Params.transaction);
    // Only copy the reply into the caller's buffer while we still own the request;
    // after a cancel that buffer may already be gone.
    VirtIOWdfDeviceDmaRxComplete(&Context->VDevice.VIODevice, Request->D2H_Params.transaction, own ? Length : 0);

    if (own)
    {
        TraceEvents(TRACE_LEVEL_VERBOSE, DBG_DPC, "Complete %p, %u bytes", wdfReq, Length);
        WdfRequestCompleteWithInformation(wdfReq, STATUS_SUCCESS, Length);
    }

    Vio9pFreeRequest(Request);
}

static FORCEINLINE ULONG FragmentSize(PHYSICAL_ADDRESS Addr, ULONG Length)
{
    ULONG offset = Addr.LowPart & (PAGE_SIZE - 1);
    return min(PAGE_SIZE - offset, Length);
}

// Copies SrcSG into DestSG, splitting elements at page boundaries.
// With DestSG == NULL only counts the resulting elements.
static ULONG PopulateSG(struct VirtIOBufferDescriptor *DestSG, PSCATTER_GATHER_LIST SrcSG)
{
    ULONG i, n = 0;

    for (i = 0; i < SrcSG->NumberOfElements; ++i)
    {
        PHYSICAL_ADDRESS pa = SrcSG->Elements[i].Address;
        ULONG len = SrcSG->Elements[i].Length;

        while (len)
        {
            ULONG current = FragmentSize(pa, len);
            if (DestSG)
            {
                DestSG[n].physAddr = pa;
                DestSG[n].length = current;
            }
            n++;
            len -= current;
            pa.QuadPart += current;
        }
    }
    return n;
}

static BOOLEAN Vio9pRxTransactionCallback(PVIRTIO_DMA_TRANSACTION_PARAMS Params)
{
    PDEVICE_CONTEXT context = Params->param1;
    PVIO9P_REQUEST req = Params->param2;
    ULONG outNum, inNum, limit;
    int ret = -1;

    req->D2H_Params.sgList = Params->sgList;
    req->D2H_Params.transaction = Params->transaction;

    outNum = PopulateSG(NULL, req->H2D_Params.sgList);
    inNum = PopulateSG(NULL, req->D2H_Params.sgList);

    limit = min(context->QueueSize, VIO9P_MAX_SG);
    if (outNum + inNum > limit)
    {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_IOCTL, "%u + %u fragments exceed the limit of %u", outNum, inNum, limit);
        Vio9pFailRequest(context, req, STATUS_INVALID_BUFFER_SIZE);
        return TRUE;
    }

    PopulateSG(req->SGTable, req->H2D_Params.sgList);
    PopulateSG(req->SGTable + outNum, req->D2H_Params.sgList);

    WdfSpinLockAcquire(context->VirtQueueLock);
    if (context->VirtQueue != NULL)
    {
        ret = virtqueue_add_buf(context->VirtQueue, req->SGTable, outNum, inNum, req, NULL, 0);
        if (ret >= 0)
        {
            virtqueue_kick(context->VirtQueue);
        }
    }
    WdfSpinLockRelease(context->VirtQueueLock);

    if (ret < 0)
    {
        // Ring full: tell the service to send it again (STATUS_RETRY -> ERROR_RETRY).
        Vio9pFailRequest(context, req, STATUS_RETRY);
    }
    return TRUE;
}

static BOOLEAN Vio9pTxTransactionCallback(PVIRTIO_DMA_TRANSACTION_PARAMS Params)
{
    PDEVICE_CONTEXT context = Params->param1;
    PVIO9P_REQUEST req = Params->param2;

    req->H2D_Params.sgList = Params->sgList;
    req->H2D_Params.transaction = Params->transaction;

    if (!VirtIOWdfDeviceDmaRxAsync(&context->VDevice.VIODevice, &req->D2H_Params, Vio9pRxTransactionCallback))
    {
        Vio9pFailRequest(context, req, STATUS_INSUFFICIENT_RESOURCES);
    }
    return TRUE;
}

static VOID HandleGetMountTag(IN PDEVICE_CONTEXT Context, IN WDFREQUEST Request)
{
    WCHAR *outBuf;
    size_t outLen;
    ULONG written = 0;
    NTSTATUS status;

    status = WdfRequestRetrieveOutputBuffer(Request, sizeof(WCHAR), (PVOID *)&outBuf, &outLen);
    if (!NT_SUCCESS(status))
    {
        WdfRequestComplete(Request, status);
        return;
    }

    RtlZeroMemory(outBuf, outLen);
    if (outLen < sizeof(WCHAR) * (strlen(Context->Tag) + 1))
    {
        WdfRequestComplete(Request, STATUS_BUFFER_TOO_SMALL);
        return;
    }

    status = RtlUTF8ToUnicodeN(outBuf, (ULONG)(outLen - sizeof(WCHAR)), &written, Context->Tag, (ULONG)strlen(Context->Tag));
    if (!NT_SUCCESS(status))
    {
        WdfRequestComplete(Request, status);
        return;
    }

    WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, written + sizeof(WCHAR));
}

static VOID HandleRequest(IN PDEVICE_CONTEXT Context,
                          IN WDFREQUEST Request,
                          IN size_t OutputBufferLength,
                          IN size_t InputBufferLength)
{
    NTSTATUS status;
    PVIO9P_REQUEST req = NULL;
    PVOID inBuf = NULL, outBuf = NULL;

    if (InputBufferLength < VIO9P_MIN_MESSAGE_SIZE || OutputBufferLength < VIO9P_MIN_MESSAGE_SIZE ||
        InputBufferLength > MAXULONG || OutputBufferLength > MAXULONG)
    {
        WdfRequestComplete(Request, STATUS_INVALID_BUFFER_SIZE);
        return;
    }

    status = WdfRequestRetrieveInputBuffer(Request, InputBufferLength, &inBuf, NULL);
    if (NT_SUCCESS(status))
    {
        status = WdfRequestRetrieveOutputBuffer(Request, OutputBufferLength, &outBuf, NULL);
    }
    if (NT_SUCCESS(status))
    {
        status = Vio9pAllocateRequest(Context, &req);
    }
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_IOCTL, "Request setup failed: %!STATUS!", status);
        WdfRequestComplete(Request, status);
        return;
    }

    req->Request = Request;

    // buffer != NULL and req == NULL: the library copies the data into its own
    // nonpaged buffers for DMA, which works with every KMDF version.
    req->H2D_Params.allocationTag = VIO9P_MEMORY_TAG;
    req->H2D_Params.buffer = inBuf;
    req->H2D_Params.size = (ULONG)InputBufferLength;
    req->H2D_Params.param1 = Context;
    req->H2D_Params.param2 = req;

    req->D2H_Params.allocationTag = VIO9P_MEMORY_TAG;
    req->D2H_Params.buffer = outBuf;
    req->D2H_Params.size = (ULONG)OutputBufferLength;
    req->D2H_Params.param1 = Context;
    req->D2H_Params.param2 = req;

    status = WdfRequestMarkCancelableEx(Request, Vio9pEvtRequestCancel);
    if (!NT_SUCCESS(status))
    {
        Vio9pFreeRequest(req);
        WdfRequestComplete(Request, status);
        return;
    }

    WdfSpinLockAcquire(Context->RequestsLock);
    PushEntryList(&Context->RequestsList, &req->ListEntry);
    WdfSpinLockRelease(Context->RequestsLock);

    if (!VirtIOWdfDeviceDmaTxAsync(&Context->VDevice.VIODevice, &req->H2D_Params, Vio9pTxTransactionCallback))
    {
        Vio9pFailRequest(Context, req, STATUS_INSUFFICIENT_RESOURCES);
    }
}

VOID Vio9pEvtIoDeviceControl(IN WDFQUEUE Queue,
                             IN WDFREQUEST Request,
                             IN size_t OutputBufferLength,
                             IN size_t InputBufferLength,
                             IN ULONG IoControlCode)
{
    PDEVICE_CONTEXT context = GetDeviceContext(WdfIoQueueGetDevice(Queue));

    TraceEvents(TRACE_LEVEL_VERBOSE,
                DBG_IOCTL,
                "--> %!FUNC! Request: %p IoCtrl: %x InLen: %Iu OutLen: %Iu",
                Request,
                IoControlCode,
                InputBufferLength,
                OutputBufferLength);

    switch (IoControlCode)
    {
        case IOCTL_VIO9P_GET_MOUNT_TAG:
            HandleGetMountTag(context, Request);
            break;

        case IOCTL_VIO9P_REQUEST:
            HandleRequest(context, Request, OutputBufferLength, InputBufferLength);
            break;

        default:
            WdfRequestComplete(Request, STATUS_INVALID_DEVICE_REQUEST);
            break;
    }
}

VOID Vio9pEvtIoStop(IN WDFQUEUE Queue, IN WDFREQUEST Request, IN ULONG ActionFlags)
{
    PDEVICE_CONTEXT context = GetDeviceContext(WdfIoQueueGetDevice(Queue));
    PSINGLE_LIST_ENTRY iter;

    TraceEvents(TRACE_LEVEL_VERBOSE, DBG_IOCTL, "--> %!FUNC! Request: %p ActionFlags: 0x%08x", Request, ActionFlags);

    if (ActionFlags & WdfRequestStopRequestCancelable)
    {
        if (WdfRequestUnmarkCancelable(Request) == STATUS_CANCELLED)
        {
            // The cancel routine owns the request now.
            return;
        }
    }

    if (ActionFlags & WdfRequestStopActionSuspend)
    {
        // The request is on the ring and will be completed by the DPC.
        WdfRequestStopAcknowledge(Request, FALSE);
        return;
    }

    // Purge (device going away): detach the request from its ring entry, which
    // the DPC then frees without touching the WDF request, and complete it here.
    WdfSpinLockAcquire(context->RequestsLock);
    for (iter = context->RequestsList.Next; iter != NULL; iter = iter->Next)
    {
        PVIO9P_REQUEST entry = CONTAINING_RECORD(iter, VIO9P_REQUEST, ListEntry);
        if (entry->Request == Request)
        {
            entry->Request = NULL;
            break;
        }
    }
    WdfSpinLockRelease(context->RequestsLock);

    WdfRequestComplete(Request, STATUS_CANCELLED);
}

VOID Vio9pEvtRequestCancel(IN WDFREQUEST Request)
{
    PDEVICE_CONTEXT context = GetDeviceContext(WdfIoQueueGetDevice(WdfRequestGetIoQueue(Request)));
    PSINGLE_LIST_ENTRY iter;

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_IOCTL, "Cancel %p", Request);

    // The ring entry stays until the device returns it; only forget the WDF request.
    WdfSpinLockAcquire(context->RequestsLock);
    for (iter = context->RequestsList.Next; iter != NULL; iter = iter->Next)
    {
        PVIO9P_REQUEST entry = CONTAINING_RECORD(iter, VIO9P_REQUEST, ListEntry);
        if (entry->Request == Request)
        {
            entry->Request = NULL;
            break;
        }
    }
    WdfSpinLockRelease(context->RequestsLock);

    WdfRequestComplete(Request, STATUS_CANCELLED);
}

WDFREQUEST Vio9pDequeueRequest(PDEVICE_CONTEXT Context, PVIO9P_REQUEST Request)
{
    PSINGLE_LIST_ENTRY iter;
    WDFREQUEST wdfReq = NULL;

    WdfSpinLockAcquire(Context->RequestsLock);
    iter = &Context->RequestsList;
    while (iter->Next != NULL)
    {
        PVIO9P_REQUEST entry = CONTAINING_RECORD(iter->Next, VIO9P_REQUEST, ListEntry);
        if (entry == Request)
        {
            iter->Next = entry->ListEntry.Next;
            wdfReq = entry->Request;
            entry->Request = NULL;
            break;
        }
        iter = iter->Next;
    }
    WdfSpinLockRelease(Context->RequestsLock);

    return wdfReq;
}
