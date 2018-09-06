/*
 * Copyright (c) 2015 - 2018 Intel Corporation
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/unistd.h>

#include "tcti.h"
#include "tcti/tcti_device.h"
#define LOGMODULE tcti
#include "log/log.h"

#define TCTI_DEVICE_DEFAULT "/dev/tpm0"

TSS2_RC
tcti_device_transmit (
    TSS2_TCTI_CONTEXT *tctiContext,
    size_t command_size,
    const uint8_t *command_buffer)
{
    TSS2_TCTI_CONTEXT_INTEL *tcti_intel = tcti_context_intel_cast (tctiContext);
    TSS2_RC rval = TSS2_RC_SUCCESS;
    ssize_t size;

    rval = tcti_send_checks (tctiContext, command_buffer);
    if (rval != TSS2_RC_SUCCESS) {
        return rval;
    }
    LOGBLOB_DEBUG (command_buffer,
                   command_size,
                   "sending %zu byte command buffer:",
                   command_size);
    size = write_all (tcti_intel->devFile,
                      command_buffer,
                      command_size);
    if (size < 0) {
        return TSS2_TCTI_RC_IO_ERROR;
    } else if ((size_t)size != command_size) {
        LOG_ERROR ("wrong number of bytes written. Expected %zu, wrote %zd.",
                   command_size,
                   size);
        return TSS2_TCTI_RC_IO_ERROR;
    }

    tcti_intel->previousStage = TCTI_STAGE_SEND_COMMAND;
    tcti_intel->status.tagReceived = 0;
    tcti_intel->status.responseSizeReceived = 0;
    tcti_intel->status.protocolResponseSizeReceived = 0;

    return TSS2_RC_SUCCESS;
}

TSS2_RC
tcti_device_receive (
    TSS2_TCTI_CONTEXT *tctiContext,
    size_t *response_size,
    uint8_t *response_buffer,
    int32_t timeout)
{
    TSS2_TCTI_CONTEXT_INTEL *tcti_intel = tcti_context_intel_cast (tctiContext);
    TSS2_RC rval = TSS2_RC_SUCCESS;
    ssize_t  size;
    unsigned int i;

    rval = tcti_receive_checks (tctiContext, response_size, response_buffer);
    if (rval != TSS2_RC_SUCCESS) {
        goto retLocalTpmReceive;
    }
    if (timeout != TSS2_TCTI_TIMEOUT_BLOCK) {
        LOG_WARNING ("The underlying IPC mechanism does not support "
                     "asynchronous I/O. The 'timeout' parameter must be "
                     "TSS2_TCTI_TIMEOUT_BLOCK");
        return TSS2_TCTI_RC_BAD_VALUE;
    }

    if (tcti_intel->status.tagReceived == 0) {
        size = TEMP_RETRY (read (tcti_intel->devFile,
                                 tcti_intel->responseBuffer,
                                 4096));
        if (size < 0) {
            LOG_ERROR("send failed with error: %d", errno);
            rval = TSS2_TCTI_RC_IO_ERROR;
            goto retLocalTpmReceive;
        } else {
            tcti_intel->status.tagReceived = 1;
            tcti_intel->responseSize = size;
        }

        tcti_intel->responseSize = size;
    }

    if (response_buffer == NULL) {
        *response_size = tcti_intel->responseSize;
        goto retLocalTpmReceive;
    }

    if (*response_size < tcti_intel->responseSize) {
        rval = TSS2_TCTI_RC_INSUFFICIENT_BUFFER;
        *response_size = tcti_intel->responseSize;
        goto retLocalTpmReceive;
    }

    *response_size = tcti_intel->responseSize;

    for (i = 0; i < *response_size; i++) {
        response_buffer[i] = tcti_intel->responseBuffer[i];
    }

    LOGBLOB_DEBUG(response_buffer, tcti_intel->responseSize, "Response Received");

    tcti_intel->status.commandSent = 0;

retLocalTpmReceive:
    if (rval == TSS2_RC_SUCCESS && response_buffer != NULL ) {
        tcti_intel->previousStage = TCTI_STAGE_RECEIVE_RESPONSE;
    }

    return rval;
}

void
tcti_device_finalize (
    TSS2_TCTI_CONTEXT *tctiContext)
{
    TSS2_TCTI_CONTEXT_INTEL *tcti_intel = tcti_context_intel_cast (tctiContext);
    TSS2_RC rc;

    rc = tcti_common_checks (tctiContext);
    if (rc != TSS2_RC_SUCCESS) {
        return;
    }
    close (tcti_intel->devFile);
}

TSS2_RC
tcti_device_cancel (
    TSS2_TCTI_CONTEXT *tctiContext)
{
    /* Linux driver doesn't expose a mechanism to cancel commands. */
    return TSS2_TCTI_RC_NOT_IMPLEMENTED;
}

TSS2_RC
tcti_device_get_poll_handles (
    TSS2_TCTI_CONTEXT *tctiContext,
    TSS2_TCTI_POLL_HANDLE *handles,
    size_t *num_handles)
{
    /* Linux driver doesn't support polling. */
    return TSS2_TCTI_RC_NOT_IMPLEMENTED;
}

#define TPM_IOC_TYPE 0xFE

#define TPM_IOC_SET_LOCALITY_NR 0x20
#define TPM_IOC_GET_LOCALITY_NR 0x21
#define TPM_IOC_SKM_DELETE_KEY_SLOT_NR 0x22

#define TPM_IOC_SET_LOCALITY _IOW(TPM_IOC_TYPE, TPM_IOC_SET_LOCALITY_NR, unsigned long)
#define TPM_IOC_GET_LOCALITY _IOR(TPM_IOC_TYPE, TPM_IOC_GET_LOCALITY_NR, unsigned long)
#define TPM_IOC_SKM_DELETE_KEY_SLOT _IOW(TPM_IOC_TYPE, TPM_IOC_SKM_DELETE_KEY_SLOT_NR, unsigned long)

TSS2_RC
tcti_skm_delete_key_slot (
    TSS2_TCTI_CONTEXT *tctiContext,
    uint32_t index)
{
    TSS2_TCTI_CONTEXT_INTEL *tcti_intel = tcti_context_intel_cast (tctiContext);
    TSS2_RC rc;

    rc = tcti_common_checks (tctiContext);
    if (rc != TSS2_RC_SUCCESS) {
        return TSS2_TCTI_RC_IO_ERROR;
    }

    if (ioctl(tcti_intel->devFile, TPM_IOC_SKM_DELETE_KEY_SLOT, index) < 0) {
        LOG_ERROR("ioctl failed: %s", strerror(errno));
        return TSS2_TCTI_RC_IO_ERROR;
    }

    return TSS2_RC_SUCCESS;
}

TSS2_RC
tcti_device_set_locality (
    TSS2_TCTI_CONTEXT *tctiContext,
    uint8_t locality)
{
    /*
     * Linux driver doesn't expose a mechanism for user space applications
     * to set locality.
     */

    TSS2_TCTI_CONTEXT_INTEL *tcti_intel = tcti_context_intel_cast (tctiContext);
    TSS2_RC rc;

    rc = tcti_common_checks (tctiContext);
    if (rc != TSS2_RC_SUCCESS) {
        return TSS2_TCTI_RC_IO_ERROR;
    }

    if (ioctl(tcti_intel->devFile, TPM_IOC_SET_LOCALITY, locality) < 0) {
        LOG_ERROR("ioctl failed: %s", strerror(errno));
        return TSS2_TCTI_RC_IO_ERROR;
    }

    return TSS2_RC_SUCCESS;
}

TSS2_RC
Tss2_Tcti_Device_Init (
    TSS2_TCTI_CONTEXT *tctiContext,
    size_t *size,
    const char *conf)
{
    TSS2_TCTI_CONTEXT_INTEL *tcti_intel = tcti_context_intel_cast (tctiContext);
    const char *dev_path = conf != NULL ? conf : TCTI_DEVICE_DEFAULT;

    if (tctiContext == NULL && size == NULL) {
        return TSS2_TCTI_RC_BAD_VALUE;
    } else if (tctiContext == NULL) {
        *size = sizeof (TSS2_TCTI_CONTEXT_INTEL);
        return TSS2_RC_SUCCESS;
    }

    /* Init TCTI context */
    TSS2_TCTI_MAGIC (tctiContext) = TCTI_MAGIC;
    TSS2_TCTI_VERSION (tctiContext) = TCTI_VERSION;
    TSS2_TCTI_TRANSMIT (tctiContext) = tcti_device_transmit;
    TSS2_TCTI_RECEIVE (tctiContext) = tcti_device_receive;
    TSS2_TCTI_FINALIZE (tctiContext) = tcti_device_finalize;
    TSS2_TCTI_CANCEL (tctiContext) = tcti_device_cancel;
    TSS2_TCTI_GET_POLL_HANDLES (tctiContext) = tcti_device_get_poll_handles;
    TSS2_TCTI_SET_LOCALITY (tctiContext) = tcti_device_set_locality;
    TSS2_TCTI_MAKE_STICKY (tctiContext) = tcti_make_sticky_not_implemented;
    TSS2_TCTI_SKM_DELETE_KEY_SLOT (tctiContext) = tcti_skm_delete_key_slot;

    tcti_intel->status.locality = 3;
    tcti_intel->status.commandSent = 0;
    tcti_intel->currentTctiContext = 0;
    tcti_intel->previousStage = TCTI_STAGE_INITIALIZE;

    tcti_intel->devFile = open (dev_path, O_RDWR);
    if (tcti_intel->devFile < 0) {
        LOG_ERROR ("Failed to open device file %s: %s",
                   dev_path, strerror (errno));
        return TSS2_TCTI_RC_IO_ERROR;
    }

    return TSS2_RC_SUCCESS;
}

const static TSS2_TCTI_INFO tss2_tcti_info = {
    .version = {
        .magic = TCTI_MAGIC,
        .version = TCTI_VERSION,
    },
    .name = "tcti-device",
    .description = "TCTI module for communication with Linux kernel interface.",
    .config_help = "Path to TPM character device. Default value is: "
        "TCTI_DEVICE_DEFAULT",
    .init = Tss2_Tcti_Device_Init,
};

const TSS2_TCTI_INFO*
Tss2_Tcti_Info (void)
{
    return &tss2_tcti_info;
}
