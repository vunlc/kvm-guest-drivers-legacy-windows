/*
 * vio9p.sys - virtio-9p (QEMU VirtFS) transport driver.
 *
 * The driver only moves 9P messages between the vio9p service and the
 * device's single "requests" virtqueue; the 9P2000.L file system client
 * lives in the service. Derived from the viofs driver.
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

#define INITGUID

#include <ntddk.h>
#include <wdf.h>
#include <wdmguid.h> // GUID_BUS_INTERFACE_STANDARD

#include "trace.h"
#include "osdep.h"
#include "vio9p_ioctl.h"
#include "virtio_pci.h"
#include "virtio.h"
#include "VirtIOWdf.h"

#define VIO9P_MEMORY_TAG        ((ULONG)'p9oV')

// virtio-9p feature bit: the device config space carries a mount tag.
#define VIRTIO_9P_MOUNT_TAG     0

// Upper bound for scatter-gather elements of one request. The real limit is
// the virtqueue size, checked per request (QEMU uses 128 entries).
#define VIO9P_MAX_SG            1024

// Device config space: u16 tag_len followed by tag_len bytes of tag.
#define VIO9P_CONFIG_TAG_LEN_OFFSET 0
#define VIO9P_CONFIG_TAG_OFFSET     2

typedef struct _VIO9P_REQUEST
{
    SINGLE_LIST_ENTRY ListEntry;

    // Memory object backing this structure (allocated from a lookaside list).
    WDFMEMORY Handle;

    // The IOCTL being served; set to NULL by the cancel routine.
    WDFREQUEST Request;

    VIRTIO_DMA_TRANSACTION_PARAMS H2D_Params; // T-message, driver -> device
    VIRTIO_DMA_TRANSACTION_PARAMS D2H_Params; // R-message, device -> driver
    struct VirtIOBufferDescriptor SGTable[VIO9P_MAX_SG];
} VIO9P_REQUEST, *PVIO9P_REQUEST;

typedef struct _DEVICE_CONTEXT
{
    VIRTIO_WDF_DRIVER VDevice;

    struct virtqueue *VirtQueue;
    WDFSPINLOCK VirtQueueLock;
    ULONG QueueSize;

    WDFINTERRUPT WdfInterrupt;

    // Safety net against a lost interrupt or notification: while the device is
    // in D0 this periodic timer re-notifies the device and collects finished
    // requests whenever some are outstanding.
    WDFTIMER WatchdogTimer;

    WDFLOOKASIDE RequestsLookaside;
    SINGLE_LIST_ENTRY RequestsList;
    WDFSPINLOCK RequestsLock;

    // Mount tag from the device config space (UTF-8, NUL-terminated).
    CHAR Tag[VIO9P_MAX_TAG_LEN + 1];
} DEVICE_CONTEXT, *PDEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, GetDeviceContext);

#ifndef _IRQL_requires_
#define _IRQL_requires_(level)
#endif

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD Vio9pEvtDeviceAdd;
EVT_WDF_OBJECT_CONTEXT_CLEANUP Vio9pEvtDeviceContextCleanup;

EVT_WDF_OBJECT_CONTEXT_CLEANUP
_IRQL_requires_(PASSIVE_LEVEL) Vio9pEvtDriverContextCleanup;

EVT_WDF_DEVICE_PREPARE_HARDWARE Vio9pEvtDevicePrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE Vio9pEvtDeviceReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY Vio9pEvtDeviceD0Entry;
EVT_WDF_DEVICE_D0_EXIT Vio9pEvtDeviceD0Exit;

EVT_WDF_INTERRUPT_ISR Vio9pEvtInterruptIsr;
EVT_WDF_INTERRUPT_DPC Vio9pEvtInterruptDpc;
EVT_WDF_INTERRUPT_ENABLE Vio9pEvtInterruptEnable;
EVT_WDF_INTERRUPT_DISABLE Vio9pEvtInterruptDisable;
EVT_WDF_TIMER Vio9pEvtWatchdogTimer;

#define VIO9P_WATCHDOG_PERIOD_MS 200

EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL Vio9pEvtIoDeviceControl;
EVT_WDF_IO_QUEUE_IO_STOP Vio9pEvtIoStop;

NTSTATUS Vio9pAllocateRequest(IN PDEVICE_CONTEXT Context, OUT PVIO9P_REQUEST *Request);
void Vio9pFreeRequest(IN PVIO9P_REQUEST Request);

// Removes Request from the in-flight list. Returns the WDF request it was
// serving at that moment (NULL if it was already cancelled).
WDFREQUEST Vio9pDequeueRequest(PDEVICE_CONTEXT Context, PVIO9P_REQUEST Request);

// Finishes a request whose reply arrived (called from the DPC).
void Vio9pCompleteRequest(PDEVICE_CONTEXT Context, PVIO9P_REQUEST Request, ULONG Length);

// Completes every request the device has returned. Callable at <= DISPATCH_LEVEL.
void Vio9pDrainQueue(PDEVICE_CONTEXT Context);
