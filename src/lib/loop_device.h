/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef BX_LOOP_DEVICE_H
#define BX_LOOP_DEVICE_H

/* Linux whole-loop-device open, returning an owned descriptor. */
int bx_loop_open(const char* path, int flags);

/* Borrow a read-only image fd. Return an owned loop-device fd on success.
 * Its association is released by Linux when the last user closes it. */
int bx_loop_attach_readonly(int image_fd, const char* name);

#endif
