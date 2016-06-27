/*
 * Copyright 2013-2016, Bromium, Inc.
 * SPDX-License-Identifier: ISC
 */

#include "channel.h"
#include <windows.h>
#include <dm/clipboard-protocol.h>
#include <iprt/err.h>

static struct clip_ctx *clipboard_ctx;
static struct clip_ctx *notify_ctx;

int ChannelConnect()
{
    /*
    If we open connection to shared-clipboard first and then to
    shared-clipboard-hostmsg, then during ns_open of the latter, the host
    will try to send its clipboard formats. It will fail because of
    ns_uclip_hostmsg_open not finished yet.
    So, arrange for ns_uclip_hostmsg_open being called (and completed) before
    ns_uclip_open - meaning, 44446 before 44445.
    */
    if (!notify_ctx)
        if (!(notify_ctx = clip_open(0, CLIP_NOTIFY_PORT, malloc, free)))
            return -1;
    if (!clipboard_ctx)
        if (!(clipboard_ctx = clip_open(0, CLIP_PORT, malloc, free)))
            return -1;
    return 0;
}

int ChannelSendNotify(char *buffer, int count)
{
    return clip_send_bytes(notify_ctx, buffer, count);
}

int ChannelSend(char* buffer, int count)
{
    return clip_send_bytes(clipboard_ctx, buffer, count);
}

int ChannelRecvNotify(void **buffer, int *count)
{
    return clip_recv_bytes(notify_ctx, buffer, count);
}

int ChannelRecv(void **buffer, int *count)
{
    return clip_recv_bytes(clipboard_ctx, buffer, count);
}

    


