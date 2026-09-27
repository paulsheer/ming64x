#ifndef LIBSSH2_TRANSPORT_H
#define LIBSSH2_TRANSPORT_H
/* Copyright (C) The Written Word, Inc.
 * Copyright (C) Daniel Stenberg
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived from this
 *    software without specific prior written permission.
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
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * This file handles reading and writing to the SECSH transport layer. RFC4253.
 */

#include "libssh2_priv.h"
#include "packet.h"

/*
 * Send a packet, encrypting it and adding a MAC code if necessary.
 *
 * The data is provided as _two_ data areas that are combined by this
 * function.  The 'data' part is sent immediately before 'data2'. 'data2' can
 * be set to NULL (or data2_len to 0) to only use a single part.
 *
 * Coroutine form: builds the packet into the session outbuf then flushes it
 * through the socket write buffer, yielding as needed. On error it records the
 * error with ssh2_err() and exits the coroutine (COROUT_ERROR).
 */
void ssh2_transport_send(LIBSSH2_SESSION *session,
                         const unsigned char *data, size_t data_len,
                         const unsigned char *data2, size_t data2_len);

/*
 * Collect a packet into the input queue.
 *
 * Coroutine form: reads the binary stream (RFC4253 chapter 6) one buffered
 * chunk at a time, yielding whenever more data is needed. On error it records
 * the error with ssh2_err() and exits the coroutine (COROUT_ERROR).
 */
void ssh2_transport_read(LIBSSH2_SESSION *session,
                         const uint32_t fullpacket_required_type);

#endif /* LIBSSH2_TRANSPORT_H */
