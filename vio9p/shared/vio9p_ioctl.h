/*
 * Interface between the vio9p driver and the vio9p service.
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

#include <guiddef.h>

// Device interface registered by vio9p.sys for every virtio-9p device.
// {c9a89dd1-d9b4-45d1-a518-027af6d28620}
DEFINE_GUID(GUID_DEVINTERFACE_VIO9P, 0xc9a89dd1, 0xd9b4, 0x45d1, 0xa5, 0x18, 0x02, 0x7a, 0xf6, 0xd2, 0x86, 0x20);

// Longest mount tag accepted from the device; longer tags are truncated.
#define VIO9P_MAX_TAG_LEN       255

// Returns the device's mount tag as a NUL-terminated UTF-16 string.
// Output buffer: WCHAR[VIO9P_MAX_TAG_LEN + 1].
#define IOCTL_VIO9P_GET_MOUNT_TAG CTL_CODE(FILE_DEVICE_UNKNOWN, 0x900, METHOD_BUFFERED, FILE_READ_ACCESS)

// Sends one 9P message to the device and waits for its reply.
// Input buffer:  the complete T-message (size[4] type[1] tag[2] ...).
// Output buffer: room for the R-message; the request completes with the
//                number of bytes the device wrote.
#define IOCTL_VIO9P_REQUEST                                                                                            \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x901, METHOD_OUT_DIRECT, FILE_READ_ACCESS | FILE_WRITE_ACCESS)

// Smallest buffer that can hold a 9P message header.
#define VIO9P_MIN_MESSAGE_SIZE  7
