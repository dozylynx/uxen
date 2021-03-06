/*
 * Copyright 2013-2017, Bromium, Inc.
 * Author: Julian Pidancet <julian@pidancet.net>
 * SPDX-License-Identifier: ISC
 */

#ifndef _UXENDISP_RESIZE_H_
#define _UXENDISP_RESIZE_H_

int display_resize(int w, int h, unsigned int flags);
int display_init(void);
void display_blank(int blank);
void display_border_windows_on_top();
void display_scratchify_process(DWORD pid, int enable);

#endif  /* _UXENDISP_RESIZE_H_ */
