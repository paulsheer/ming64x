/* Copyright (C) Sara Golemon <sarag@libssh2.org>
 * Copyright (C) Mikhail Gusarov
 * Copyright (C) Daniel Stenberg
 * Copyright (C) Simon Josefsson
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

#include "libssh2_priv.h"

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include "transport.h"
#include "channel.h"
#include "packet.h"

/* coroutine I/O seam: START/END/CALL/READ_SOME/APPEND_BLOCK/WAIT_WRITE. */
#include "corout.h"
/* corout.h leaks socket helper macros that would rewrite libssh2's own
   ioctl/perror calls; keep only the coroutine macros. */
#undef ioctl
#undef perror
#undef RETRY

/*
 * Queue a connection request for a listener.
 *
 * Coroutine form: the request is parsed and the listener matched as pure CPU
 * work, then the response (open confirmation or failure) is sent with a single
 * CALL(ssh2_transport_send()). listen_state holds the parsed fields across the
 * send yield; whether to link the new channel after the send is decided from
 * listen_state->channel (non-NULL only on success).
 */
static void packet_queue_listener(
    LIBSSH2_SESSION *session,
    unsigned char *data, const size_t datalen,
    struct packet_queue_listener_state *listen_state)
{
    struct corout_item *state = session->corout_state;
    /* 17 = packet_type(1) + channel(4) + reason(4) + descr(4) + lang(4) */
    size_t packet_len = 17 + strlen(FwdNotReq);
    unsigned char *p;
    LIBSSH2_LISTENER *listn;
    uint32_t failure_code = SSH_OPEN_ADMINISTRATIVELY_PROHIBITED;

    START();

    {
        size_t offset = sizeof("forwarded-tcpip") - 1 + 5;
        size_t temp_len = 0;
        struct string_buf buf;
        buf.data = data;
        buf.dataptr = buf.data;
        buf.len = datalen;

        if(datalen < offset + 12) { /* 3 * 4-byte */
            ssh2_err(session, LIBSSH2_ERROR_OUT_OF_BOUNDARY,
                     "Unexpected packet size");
            COROUT_EXIT();
        }

        buf.dataptr += offset;

        if(ssh2_get_u32(&buf, &listen_state->sender_channel)) {
            ssh2_err(session, LIBSSH2_ERROR_BUFFER_TOO_SMALL,
                     "Data too short extracting channel");
            COROUT_EXIT();
        }
        if(ssh2_get_u32(&buf, &listen_state->initial_window_size)) {
            ssh2_err(session, LIBSSH2_ERROR_BUFFER_TOO_SMALL,
                     "Data too short extracting window size");
            COROUT_EXIT();
        }
        if(ssh2_get_u32(&buf, &listen_state->packet_size)) {
            ssh2_err(session, LIBSSH2_ERROR_BUFFER_TOO_SMALL,
                     "Data too short extracting packet");
            COROUT_EXIT();
        }
        if(ssh2_get_string(&buf, &listen_state->host, &temp_len)) {
            ssh2_err(session, LIBSSH2_ERROR_BUFFER_TOO_SMALL,
                     "Data too short extracting host");
            COROUT_EXIT();
        }
        listen_state->host_len = (uint32_t)temp_len;

        if(ssh2_get_u32(&buf, &listen_state->port)) {
            ssh2_err(session, LIBSSH2_ERROR_BUFFER_TOO_SMALL,
                     "Data too short extracting port");
            COROUT_EXIT();
        }
        if(ssh2_get_string(&buf, &listen_state->shost, &temp_len)) {
            ssh2_err(session, LIBSSH2_ERROR_BUFFER_TOO_SMALL,
                     "Data too short extracting shost");
            COROUT_EXIT();
        }
        listen_state->shost_len = (uint32_t)temp_len;

        if(ssh2_get_u32(&buf, &listen_state->sport)) {
            ssh2_err(session, LIBSSH2_ERROR_BUFFER_TOO_SMALL,
                     "Data too short extracting sport");
            COROUT_EXIT();
        }

        ssh2_deb((session, LIBSSH2_TRACE_CONN,
                  "Remote received connection from %.*s:%u to %.*s:%u",
                  (int)listen_state->shost_len, listen_state->shost,
                  listen_state->sport,
                  (int)listen_state->host_len, listen_state->host,
                  listen_state->port));
    }

    listen_state->channel = NULL;
    listn = ssh2_list_first(&session->listeners);

    while(listn) {
        if(listn->port == (int)listen_state->port &&
           strlen(listn->host) == listen_state->host_len &&
           !memcmp(listn->host, listen_state->host,
                   listen_state->host_len)) {
            /* This is our listener */
            LIBSSH2_CHANNEL *channel;

            if(listn->queue_maxsize &&
               (listn->queue_maxsize <= listn->queue_size)) {
                /* Queue is full */
                failure_code = SSH_OPEN_RESOURCE_SHORTAGE;
                ssh2_deb((session, LIBSSH2_TRACE_CONN,
                          "Listener queue full, ignoring"));
                break;
            }

            if(!listen_state->packet_size) {
                ssh2_deb((session, LIBSSH2_TRACE_CONN,
                          "Invalid packet size received from server"));
                failure_code = SSH_OPEN_CONNECT_FAILED;
                break;
            }

            channel = SSH2_CALLOC(session, sizeof(LIBSSH2_CHANNEL));
            if(!channel) {
                ssh2_err(session, LIBSSH2_ERROR_ALLOC,
                         "Unable to allocate a channel for "
                         "new connection");
                failure_code = SSH_OPEN_RESOURCE_SHORTAGE;
                break;
            }
            listen_state->channel = channel;

            channel->session = session;
            channel->channel_type_len = sizeof("forwarded-tcpip") - 1;
            channel->channel_type =
                SSH2_ALLOC(session, channel->channel_type_len + 1);
            if(!channel->channel_type) {
                ssh2_err(session, LIBSSH2_ERROR_ALLOC,
                         "Unable to allocate a channel for "
                         "new connection");
                SSH2_FREE(session, channel);
                listen_state->channel = NULL;
                failure_code = SSH_OPEN_RESOURCE_SHORTAGE;
                break;
            }
            memcpy(channel->channel_type, "forwarded-tcpip",
                   channel->channel_type_len + 1);

            channel->remote.id = listen_state->sender_channel;
            channel->remote.window_size_initial =
                LIBSSH2_CHANNEL_WINDOW_DEFAULT;
            channel->remote.window_size =
                LIBSSH2_CHANNEL_WINDOW_DEFAULT;
            channel->remote.packet_size =
                LIBSSH2_CHANNEL_PACKET_DEFAULT;

            channel->local.id = ssh2_channel_nextid(session);
            channel->local.window_size_initial =
                listen_state->initial_window_size;
            channel->local.window_size =
                listen_state->initial_window_size;
            channel->local.packet_size = listen_state->packet_size;

            ssh2_deb((session, LIBSSH2_TRACE_CONN,
                      "Connection queued: channel %u/%u "
                      "win %u/%u packet %u/%u",
                      channel->local.id, channel->remote.id,
                      channel->local.window_size,
                      channel->remote.window_size,
                      channel->local.packet_size,
                      channel->remote.packet_size));

            p = listen_state->packet;
            *(p++) = SSH_MSG_CHANNEL_OPEN_CONFIRMATION;
            ssh2_store_u32(&p, channel->remote.id);
            ssh2_store_u32(&p, channel->local.id);
            ssh2_store_u32(&p, channel->remote.window_size_initial);
            ssh2_store_u32(&p, channel->remote.packet_size);

            break;
        }

        listn = ssh2_list_next(&listn->node);
    }

    if(listen_state->channel) {
        CALL(ssh2_transport_send(session, listen_state->packet, 17, NULL, 0));

        /* Re-find the listener (listn is a stack local lost across the send
           yield) and link the new channel into its queue. */
        listn = ssh2_list_first(&session->listeners);
        while(listn) {
            if(listn->port == (int)listen_state->port &&
               strlen(listn->host) == listen_state->host_len &&
               !memcmp(listn->host, listen_state->host,
                       listen_state->host_len)) {
                ssh2_list_add(&listn->queue, &listen_state->channel->node);
                listn->queue_size++;
                break;
            }
            listn = ssh2_list_next(&listn->node);
        }
    }
    else {
        /* We are not listening to you */
        p = listen_state->packet;
        *(p++) = SSH_MSG_CHANNEL_OPEN_FAILURE;
        ssh2_store_u32(&p, listen_state->sender_channel);
        ssh2_store_u32(&p, failure_code);
        ssh2_store_str(&p, FwdNotReq, strlen(FwdNotReq));
        ssh2_htonu32(p, 0);

        CALL(ssh2_transport_send(session, listen_state->packet,
                                 packet_len, NULL, 0));
    }

    END();
}

/*
 * Accept a forwarded X11 connection.
 *
 * Coroutine form: as with packet_queue_listener(), parse + decision are pure
 * CPU and the response is sent with a single CALL(ssh2_transport_send()).
 * x11open_state->channel (non-NULL on success) decides the post-send bookkeeping
 * (channel link + callback) which happens only after the send yields.
 */
static void packet_x11_open(
    LIBSSH2_SESSION *session,
    unsigned char *data, const size_t datalen,
    struct packet_x11_open_state *x11open_state)
{
    struct corout_item *state = session->corout_state;
    uint32_t failure_code = SSH_OPEN_CONNECT_FAILED;
    /* 17 = packet_type(1) + channel(4) + reason(4) + descr(4) + lang(4) */
    size_t packet_len = 17 + strlen(X11FwdUnAvil);
    unsigned char *p;
    LIBSSH2_CHANNEL *channel = NULL;

    START();

    x11open_state->channel = NULL;
    x11open_state->shost = NULL;

    {
        size_t offset = sizeof("x11") - 1 + 5;
        size_t temp_len = 0;
        unsigned char *temp_buf = NULL;
        struct string_buf buf;
        buf.data = data;
        buf.dataptr = buf.data;
        buf.len = datalen;

        if(datalen >= offset + 12) { /* 3 * 4-byte */
            buf.dataptr += offset;

            if(ssh2_get_u32(&buf, &x11open_state->sender_channel))
                ssh2_err(session, LIBSSH2_ERROR_INVAL,
                         "unexpected sender channel size");
            else if(ssh2_get_u32(&buf, &x11open_state->initial_window_size))
                ssh2_err(session, LIBSSH2_ERROR_INVAL,
                         "unexpected window size");
            else if(ssh2_get_u32(&buf, &x11open_state->packet_size))
                ssh2_err(session, LIBSSH2_ERROR_INVAL,
                         "unexpected packet size");
            else if(ssh2_get_string(&buf, &temp_buf, &temp_len))
                ssh2_err(session, LIBSSH2_ERROR_INVAL,
                         "unexpected host size");
            else {
                x11open_state->shost_len = (uint32_t)temp_len;
                x11open_state->shost =
                    SSH2_ALLOC(session, x11open_state->shost_len + 1);
                if(!x11open_state->shost)
                    ssh2_err(session, LIBSSH2_ERROR_ALLOC,
                             "Unable to allocate memory for shost");
                else {
                    memcpy(x11open_state->shost, temp_buf,
                           x11open_state->shost_len);
                    x11open_state->shost[x11open_state->shost_len] = '\0';

                    if(ssh2_get_u32(&buf, &x11open_state->sport))
                        ssh2_err(session, LIBSSH2_ERROR_INVAL,
                                 "unexpected port size");
                    else {
                        ssh2_deb((session, LIBSSH2_TRACE_CONN,
                                  "X11 Connection Received from %s:%u on "
                                  "channel %u",
                                  x11open_state->shost, x11open_state->sport,
                                  x11open_state->sender_channel));
                        x11open_state->channel = (LIBSSH2_CHANNEL *)1;
                    }
                }
            }
        }
        else {
            ssh2_err(session, LIBSSH2_ERROR_INVAL,
                     "unexpected data length");
        }
    }

    if(x11open_state->channel && session->x11) {
        if(!x11open_state->packet_size) {
            ssh2_deb((session, LIBSSH2_TRACE_CONN,
                      "Invalid packet size received from server"));
            failure_code = SSH_OPEN_CONNECT_FAILED;
            SSH2_SAFEFREE(session, x11open_state->shost);
            x11open_state->channel = NULL;
        }
        else {
            channel = SSH2_CALLOC(session, sizeof(LIBSSH2_CHANNEL));
            if(!channel) {
                ssh2_err(session, LIBSSH2_ERROR_ALLOC,
                         "allocate a channel for new connection");
                failure_code = SSH_OPEN_RESOURCE_SHORTAGE;
                SSH2_SAFEFREE(session, x11open_state->shost);
                x11open_state->channel = NULL;
            }
            else {
                channel->session = session;
                channel->channel_type_len = sizeof("x11") - 1;
                channel->channel_type =
                    SSH2_ALLOC(session, channel->channel_type_len + 1);
                if(!channel->channel_type) {
                    ssh2_err(session, LIBSSH2_ERROR_ALLOC,
                             "allocate a channel for new connection");
                    SSH2_FREE(session, channel);
                    failure_code = SSH_OPEN_RESOURCE_SHORTAGE;
                    SSH2_SAFEFREE(session, x11open_state->shost);
                    x11open_state->channel = NULL;
                }
                else {
                    memcpy(channel->channel_type, "x11",
                           channel->channel_type_len + 1);

                    channel->remote.id = x11open_state->sender_channel;
                    channel->remote.window_size_initial =
                        LIBSSH2_CHANNEL_WINDOW_DEFAULT;
                    channel->remote.window_size =
                        LIBSSH2_CHANNEL_WINDOW_DEFAULT;
                    channel->remote.packet_size =
                        LIBSSH2_CHANNEL_PACKET_DEFAULT;

                    channel->local.id = ssh2_channel_nextid(session);
                    channel->local.window_size_initial =
                        x11open_state->initial_window_size;
                    channel->local.window_size =
                        x11open_state->initial_window_size;
                    channel->local.packet_size = x11open_state->packet_size;

                    ssh2_deb((session, LIBSSH2_TRACE_CONN,
                              "X11 Connection established: channel %u/%u "
                              "win %u/%u packet %u/%u",
                              channel->local.id, channel->remote.id,
                              channel->local.window_size,
                              channel->remote.window_size,
                              channel->local.packet_size,
                              channel->remote.packet_size));

                    p = x11open_state->packet;
                    *(p++) = SSH_MSG_CHANNEL_OPEN_CONFIRMATION;
                    ssh2_store_u32(&p, channel->remote.id);
                    ssh2_store_u32(&p, channel->local.id);
                    ssh2_store_u32(&p, channel->remote.window_size_initial);
                    ssh2_store_u32(&p, channel->remote.packet_size);

                    x11open_state->channel = channel;
                }
            }
        }
    }

    if(x11open_state->channel && x11open_state->channel == channel) {
        CALL(ssh2_transport_send(session, x11open_state->packet, 17,
                                 NULL, 0));

        /* Link the channel into the session */
        ssh2_list_add(&session->channels, &x11open_state->channel->node);

        SSH2_X11_OPEN(x11open_state->channel, (char *)x11open_state->shost,
                      x11open_state->sport);

        SSH2_SAFEFREE(session, x11open_state->shost);
        x11open_state->channel = NULL;
    }
    else {
        /* Send a failure reply */
        p = x11open_state->packet;
        *(p++) = SSH_MSG_CHANNEL_OPEN_FAILURE;
        ssh2_store_u32(&p, x11open_state->sender_channel);
        ssh2_store_u32(&p, failure_code);
        ssh2_store_str(&p, X11FwdUnAvil, strlen(X11FwdUnAvil));
        ssh2_htonu32(p, 0);

        SSH2_SAFEFREE(session, x11open_state->shost);

        CALL(ssh2_transport_send(session, x11open_state->packet,
                                 packet_len, NULL, 0));
    }

    END();
}

/*
 * Open a connection to authentication agent.
 *
 * Coroutine form: parse + decision are pure CPU; the response is sent with a
 * single CALL(ssh2_transport_send()). authagent_state->channel (non-NULL on
 * success) decides the post-send bookkeeping after the send yield.
 */
static void packet_authagent_open(
    LIBSSH2_SESSION *session,
    unsigned char *data, const size_t datalen,
    struct packet_authagent_state *authagent_state)
{
    struct corout_item *state = session->corout_state;
    uint32_t failure_code = SSH_OPEN_CONNECT_FAILED;
    /* 17 = packet_type(1) + channel(4) + reason(4) + descr(4) + lang(4) */
    size_t packet_len = 17 + strlen(AuthAgentUnavail);
    unsigned char *p;
    LIBSSH2_CHANNEL *channel = NULL;
    struct string_buf buf;
    size_t offset = sizeof("auth-agent@openssh.com") - 1 + 5;

    START();

    buf.data = data;
    buf.dataptr = buf.data;
    buf.len = datalen;

    authagent_state->channel = NULL;

    if(datalen < offset + 12) { /* 27-byte header + 3 * 4-byte */
        ssh2_err(session, LIBSSH2_ERROR_OUT_OF_BOUNDARY,
                 "Unexpected packet size");
        COROUT_EXIT();
    }

    buf.dataptr += offset;

    if(ssh2_get_u32(&buf, &authagent_state->sender_channel)) {
        ssh2_err(session, LIBSSH2_ERROR_BUFFER_TOO_SMALL,
                 "Data too short extracting channel");
        COROUT_EXIT();
    }
    if(ssh2_get_u32(&buf, &authagent_state->initial_window_size)) {
        ssh2_err(session, LIBSSH2_ERROR_BUFFER_TOO_SMALL,
                 "Data too short extracting window size");
        COROUT_EXIT();
    }
    if(ssh2_get_u32(&buf, &authagent_state->packet_size)) {
        ssh2_err(session, LIBSSH2_ERROR_BUFFER_TOO_SMALL,
                 "Data too short extracting packet");
        COROUT_EXIT();
    }

    ssh2_deb((session, LIBSSH2_TRACE_CONN,
              "Auth Agent Connection Received on channel %u",
              authagent_state->sender_channel));

    if(session->authagent) {
        if(!authagent_state->packet_size) {
            ssh2_deb((session, LIBSSH2_TRACE_CONN,
                      "Invalid packet size received from server"));
            failure_code = SSH_OPEN_CONNECT_FAILED;
        }
        else {
            channel = SSH2_CALLOC(session, sizeof(LIBSSH2_CHANNEL));
            authagent_state->channel = channel;

            if(!channel) {
                ssh2_err(session, LIBSSH2_ERROR_ALLOC,
                         "allocate a channel for new connection");
                failure_code = SSH_OPEN_RESOURCE_SHORTAGE;
            }
            else {
                channel->session = session;
                channel->channel_type_len = sizeof("auth agent") - 1;
                channel->channel_type =
                    SSH2_ALLOC(session, channel->channel_type_len + 1);
                if(!channel->channel_type) {
                    ssh2_err(session, LIBSSH2_ERROR_ALLOC,
                             "allocate a channel for new connection");
                    SSH2_FREE(session, channel);
                    authagent_state->channel = NULL;
                    failure_code = SSH_OPEN_RESOURCE_SHORTAGE;
                }
                else {
                    memcpy(channel->channel_type, "auth agent",
                           channel->channel_type_len + 1);

                    channel->remote.id = authagent_state->sender_channel;
                    channel->remote.window_size_initial =
                        LIBSSH2_CHANNEL_WINDOW_DEFAULT;
                    channel->remote.window_size =
                        LIBSSH2_CHANNEL_WINDOW_DEFAULT;
                    channel->remote.packet_size =
                        LIBSSH2_CHANNEL_PACKET_DEFAULT;

                    channel->local.id = ssh2_channel_nextid(session);
                    channel->local.window_size_initial =
                        authagent_state->initial_window_size;
                    channel->local.window_size =
                        authagent_state->initial_window_size;
                    channel->local.packet_size = authagent_state->packet_size;

                    ssh2_deb((session, LIBSSH2_TRACE_CONN,
                              "Auth Agent Connection established: channel "
                              "%u/%u win %u/%u packet %u/%u",
                              channel->local.id, channel->remote.id,
                              channel->local.window_size,
                              channel->remote.window_size,
                              channel->local.packet_size,
                              channel->remote.packet_size));

                    p = authagent_state->packet;
                    *(p++) = SSH_MSG_CHANNEL_OPEN_CONFIRMATION;
                    ssh2_store_u32(&p, channel->remote.id);
                    ssh2_store_u32(&p, channel->local.id);
                    ssh2_store_u32(&p, channel->remote.window_size_initial);
                    ssh2_store_u32(&p, channel->remote.packet_size);

                    authagent_state->channel = channel;
                }
            }
        }
    }
    else
        failure_code = SSH_OPEN_RESOURCE_SHORTAGE;

    if(authagent_state->channel) {
        CALL(ssh2_transport_send(session, authagent_state->packet, 17,
                                 NULL, 0));

        /* Link the channel into the session */
        ssh2_list_add(&session->channels, &authagent_state->channel->node);

        SSH2_AUTHAGENT(authagent_state->channel);

        authagent_state->channel = NULL;
    }
    else {
        /* Send a failure reply */
        p = authagent_state->packet;
        *(p++) = SSH_MSG_CHANNEL_OPEN_FAILURE;
        ssh2_store_u32(&p, authagent_state->sender_channel);
        ssh2_store_u32(&p, failure_code);
        ssh2_store_str(&p, AuthAgentUnavail, strlen(AuthAgentUnavail));
        ssh2_htonu32(p, 0);

        CALL(ssh2_transport_send(session, authagent_state->packet,
                                 packet_len, NULL, 0));
    }

    END();
}

/*
 * Create a new packet and attach it to the brigade. Called from the transport
 * layer when it has received a packet.
 *
 * The input pointer 'data' points to allocated data that this function owns:
 * it is freed (on error or when the packet is ignored/handled) or attached to
 * the brigade (on success). 'datalen' is always greater than zero.
 *
 * Coroutine form: the only yields are inside ssh2_transport_send() (failure
 * replies), ssh2_channel_receive_window_adjust() and ssh2_kex_exchange(). The
 * packet is parsed and decided upon as pure CPU work before those yields; the
 * resume labels skip the parse on re-entry.
 */
void ssh2_packet_add(LIBSSH2_SESSION *session, unsigned char *data,
                     size_t datalen, const int macstate, const uint32_t seq,
                     const uint32_t fullpacket_required_type)
{
    struct corout_item *state = session->corout_state;
    char *message = NULL;
    char *language = NULL;
    size_t message_len = 0;
    size_t language_len = 0;
    LIBSSH2_CHANNEL *channelp = NULL;
    size_t data_head = 0;
    int rc = 0;
    uint32_t channel = 0;
    uint32_t len = 0;
    unsigned char want_reply = 0;

#define msg()   (state->stack[state->depth].msg)
    msg() = data[0];

    START();

    session->packAdd_rc = 0;

    ssh2_deb((session, LIBSSH2_TRACE_TRANS,
              "Packet type %u received, length=%ld",
              (unsigned int)msg(), (long)datalen));

    if(macstate == SSH2_MAC_INVALID &&
       (!session->macerror ||
        SSH2_MACERROR(session, (char *)data, datalen))) {
        /* Bad MAC input, but no callback set or non-zero return from the
           callback */
        SSH2_FREE(session, data);
        ssh2_err(session, LIBSSH2_ERROR_INVALID_MAC, "Invalid MAC received");
        COROUT_EXIT();
    }

    if(session->state & SSH2_STATE_INITIAL_KEX) {
        if(msg() == SSH_MSG_KEXINIT) {
            if(!session->kex_strict) {
                if(datalen < 17) {
                    SSH2_FREE(session, data);
                    ssh2_err(session, LIBSSH2_ERROR_BUFFER_TOO_SMALL,
                             "Data too short extracting kex");
                    COROUT_EXIT();
                }
                else {
                    static const char strict[] =
                        "kex-strict-s-v00@openssh.com";
                    struct string_buf buf;
                    char *algs = NULL;
                    size_t algs_len = 0;

                    buf.data = data;
                    buf.dataptr = buf.data;
                    buf.len = datalen;
                    buf.dataptr += 17; /* advance past type and cookie */

                    if(ssh2_get_chars(&buf, &algs, &algs_len)) {
                        SSH2_FREE(session, data);
                        ssh2_err(session, LIBSSH2_ERROR_BUFFER_TOO_SMALL,
                                 "Algs too short");
                        COROUT_EXIT();
                    }

                    if(algs_len == 0 ||
                       ssh2_kex_agree_instr(algs, algs_len,
                                            strict, sizeof(strict) - 1)) {
                        session->kex_strict = 1;
                    }
                }
            }

            if(session->kex_strict && seq) {
                SSH2_FREE(session, data);
                session->socket_state = SSH2_SOCKET_DISCONNECTED;
                libssh2_session_disconnect(session, "strict KEX violation: "
                                           "KEXINIT was not the first packet");
                ssh2_err(session, LIBSSH2_ERROR_SOCKET_DISCONNECT,
                         "strict KEX violation: "
                         "KEXINIT was not the first packet");
                COROUT_EXIT();
            }
        }

        if(session->kex_strict && fullpacket_required_type &&
           fullpacket_required_type != msg()) {
            SSH2_FREE(session, data);
            session->socket_state = SSH2_SOCKET_DISCONNECTED;
            libssh2_session_disconnect(session, "strict KEX violation: "
                                       "unexpected packet type");
            ssh2_err(session, LIBSSH2_ERROR_SOCKET_DISCONNECT,
                     "strict KEX violation: unexpected packet type");
            COROUT_EXIT();
        }
    }

    if(msg() == SSH_MSG_DISCONNECT) {

        /*
           byte      SSH_MSG_DISCONNECT
           uint32    reason code
           string    description in ISO-10646 UTF-8 encoding [RFC3629]
           string    language tag [RFC3066]
         */

        if(datalen >= 5) {
            uint32_t reason = 0;
            struct string_buf buf;
            buf.data = data;
            buf.dataptr = buf.data;
            buf.len = datalen;
            buf.dataptr++; /* advance past type */

            ssh2_get_u32(&buf, &reason);
            ssh2_get_chars(&buf, &message, &message_len);
            ssh2_get_chars(&buf, &language, &language_len);

            if(session->ssh_msg_disconnect)
                SSH2_DISCONNECT(session, reason, message, message_len,
                                language, language_len);

            ssh2_deb((session, LIBSSH2_TRACE_TRANS,
                      "Disconnect(%u): %.*s(%.*s)", reason,
                      (int)message_len, message, (int)language_len,
                      language));
        }

        SSH2_FREE(session, data);
        session->socket_state = SSH2_SOCKET_DISCONNECTED;
        ssh2_err(session, LIBSSH2_ERROR_SOCKET_DISCONNECT,
                 "socket disconnect");
        COROUT_EXIT();

        /*
           byte      SSH_MSG_IGNORE
           string    data
         */

    }
    else if(msg() == SSH_MSG_IGNORE) {
        if(datalen >= 2) {
            if(session->ssh_msg_ignore)
                SSH2_IGNORE(session, (char *)data + 1, datalen - 1);
        }
        else if(session->ssh_msg_ignore) {
            SSH2_IGNORE(session, "", 0);
        }
        SSH2_FREE(session, data);
        return;

        /*
           byte      SSH_MSG_DEBUG
           boolean   always_display
           string    message in ISO-10646 UTF-8 encoding [RFC3629]
           string    language tag [RFC3066]
         */

    }
    else if(msg() == SSH_MSG_DEBUG) {
        if(datalen >= 2) {
            int always_display = data[1];

            if(datalen >= 6) {
                struct string_buf buf;
                buf.data = data;
                buf.dataptr = buf.data;
                buf.len = datalen;
                buf.dataptr += 2; /* advance past type & always display */

                ssh2_get_chars(&buf, &message, &message_len);
                ssh2_get_chars(&buf, &language, &language_len);
            }

            if(session->ssh_msg_debug)
                SSH2_DEBUG(session, always_display, message, message_len,
                           language, language_len);
        }

        ssh2_deb((session, LIBSSH2_TRACE_TRANS, "Debug Packet: %.*s",
                  (int)message_len, message));
        SSH2_FREE(session, data);
        return;

        /*
           byte      SSH_MSG_EXT_INFO
           uint32    nr-extensions
           [repeat   "nr-extensions" times]
           string    extension-name  [RFC8308]
           string    extension-value (binary)
         */

    }
    else if(msg() == SSH_MSG_EXT_INFO) {
        if(datalen >= 5) {
            uint32_t nr_extensions = 0;
            struct string_buf buf;
            buf.data = data;
            buf.dataptr = buf.data;
            buf.len = datalen;
            buf.dataptr += 1; /* advance past type */

            if(ssh2_get_u32(&buf, &nr_extensions) != 0 ||
               nr_extensions >= 1024)
                rc = ssh2_err(session, LIBSSH2_ERROR_PROTO,
                              "Invalid extension info received");

            while(rc == 0 && nr_extensions > 0) {

                size_t name_len = 0;
                size_t value_len = 0;
                unsigned char *name = NULL;
                unsigned char *value = NULL;

                nr_extensions -= 1;

                if(ssh2_get_string(&buf, &name, &name_len))
                    break;
                if(ssh2_get_string(&buf, &value, &value_len))
                    break;

                if(name && value)
                    ssh2_deb((session, LIBSSH2_TRACE_KEX,
                              "Server to Client extension %.*s: %.*s",
                              (int)name_len, name, (int)value_len, value));

                if(SSH2_IS_LITERAL(name, name_len, "server-sig-algs")) {
                    if(session->server_sign_algorithms)
                        SSH2_FREE(session, session->server_sign_algorithms);

                    session->server_sign_algorithms =
                        SSH2_ALLOC(session, value_len + 1);

                    if(value && session->server_sign_algorithms) {
                        memcpy(session->server_sign_algorithms,
                               value, value_len);
                        session->server_sign_algorithms[value_len] = '\0';
                    }
                    else
                        rc = ssh2_err(session, LIBSSH2_ERROR_ALLOC,
                                      "memory for server sign algo");
                }
            }
        }

        SSH2_FREE(session, data);
        if(rc)
            COROUT_EXIT();
        return;

        /*
           byte      SSH_MSG_GLOBAL_REQUEST
           string    request name in US-ASCII only
           boolean   want reply
           ....      request-specific data follows
         */

    }
    else if(msg() == SSH_MSG_GLOBAL_REQUEST) {
        if(datalen >= 5) {
            want_reply = 0;
            len = ssh2_ntohu32(data + 1);
            if(len <= (UINT_MAX - 6) && datalen >= (6 + len)) {
                want_reply = data[5 + len];
                ssh2_deb((session, LIBSSH2_TRACE_CONN,
                          "Received global request type %.*s (wr %X)",
                          (int)len, data + 5, want_reply));
            }

            if(want_reply) {
                static const unsigned char packet =
                    SSH_MSG_REQUEST_FAILURE;
                CALL(ssh2_transport_send(session, &packet, 1, NULL, 0));
            }
        }
        SSH2_FREE(session, data);
        return;

        /*
           byte      SSH_MSG_CHANNEL_EXTENDED_DATA
           uint32    recipient channel
           uint32    data_type_code
           string    data
         */

    }
    else if(msg() == SSH_MSG_CHANNEL_EXTENDED_DATA ||
            msg() == SSH_MSG_CHANNEL_DATA) {
        if(msg() == SSH_MSG_CHANNEL_EXTENDED_DATA) {
            /* streamid(4) */
            data_head += 4;
        }

        /* packet_type(1) + channelno(4) + datalen(4) */
        data_head += 9;

        if(datalen >= data_head)
            channelp =
                ssh2_channel_locate(session, ssh2_ntohu32(data + 1));

        if(!channelp) {
            ssh2_err(session, LIBSSH2_ERROR_CHANNEL_UNKNOWN,
                     "Packet received for unknown channel");
            SSH2_FREE(session, data);
            return;
        }
#ifdef LIBSSH2DEBUG
        {
            uint32_t stream_id = 0;
            if(msg() == SSH_MSG_CHANNEL_EXTENDED_DATA)
                stream_id = ssh2_ntohu32(data + 5);

            ssh2_deb((session, LIBSSH2_TRACE_CONN,
                      "%ld bytes packet_add() for %u/%u/%u",
                      (long)(datalen - data_head),
                      channelp->local.id,
                      channelp->remote.id,
                      stream_id));
        }
#endif
        if(channelp->remote.extended_data_ignore_mode ==
           LIBSSH2_CHANNEL_EXTENDED_DATA_IGNORE &&
           msg() == SSH_MSG_CHANNEL_EXTENDED_DATA) {
            /* Pretend we did not receive this */
            ssh2_deb((session, LIBSSH2_TRACE_CONN,
                      "Ignoring extended data and refunding %ld bytes",
                      (long)(datalen - 13)));
            if(channelp->read_avail + datalen - data_head >=
               channelp->remote.window_size)
                datalen = channelp->remote.window_size -
                    channelp->read_avail + data_head;

            channelp->remote.window_size -=
                (uint32_t)(datalen - data_head);
            ssh2_deb((session, LIBSSH2_TRACE_CONN,
                      "shrinking window size by %ld bytes to %u, "
                      "read_avail %ld",
                      (long)(datalen - data_head),
                      channelp->remote.window_size,
                      (long)channelp->read_avail));

            /* Keep channel and the (possibly truncated) datalen across the
               window-adjust yield: both are stack locals. */
            session->packAdd_channelp = channelp;
            session->packAdd_datalen = datalen;

            CALL(ssh2_channel_receive_window_adjust(session->packAdd_channelp,
                                                    (uint32_t)(
                                                    session->packAdd_datalen
                                                    - 13), 1, NULL));

            /* free only now that the window adjust is done */
            SSH2_FREE(session, data);
            return;
        }

        /*
         * REMEMBER! remote means remote as source of data,
         * NOT remote window!
         */
        if(channelp->remote.packet_size < (datalen - data_head)) {
            /*
             * Spec says we MAY ignore bytes sent beyond
             * packet_size
             */
            ssh2_err(session, LIBSSH2_ERROR_CHANNEL_PACKET_EXCEEDED,
                     "Packet contains more data than we offered"
                     " to receive, truncating");
            datalen = channelp->remote.packet_size + data_head;
        }
        if(channelp->remote.window_size <= channelp->read_avail) {
            /*
             * Spec says we MAY ignore bytes sent beyond
             * window_size
             */
            ssh2_err(session, LIBSSH2_ERROR_CHANNEL_WINDOW_EXCEEDED,
                     "The current receive window is full, data ignored");
            SSH2_FREE(session, data);
            return;
        }
        /* Reset EOF status */
        channelp->remote.eof = 0;

        if(channelp->read_avail + datalen - data_head >
           channelp->remote.window_size) {
            ssh2_err(session, LIBSSH2_ERROR_CHANNEL_WINDOW_EXCEEDED,
                     "Remote sent more data than current "
                     "window allows, truncating");
            datalen = channelp->remote.window_size -
                channelp->read_avail + data_head;
        }

        /* Update the read_avail counter. The window size is
         * updated once the data is actually read from the queue
         * from an upper layer */
        channelp->read_avail += datalen - data_head;

        ssh2_deb((session, LIBSSH2_TRACE_CONN,
                  "increasing read_avail by %ld bytes to %ld/%u",
                  (long)(datalen - data_head),
                  (long)channelp->read_avail,
                  channelp->remote.window_size));
    }

        /*
           byte      SSH_MSG_CHANNEL_EOF
           uint32    recipient channel
         */

    else if(msg() == SSH_MSG_CHANNEL_EOF) {
        if(datalen >= 5)
            channelp =
                ssh2_channel_locate(session, ssh2_ntohu32(data + 1));
        if(!channelp)
            /* We may have freed already, quietly ignore this... */
            ;
        else {
            ssh2_deb((session, LIBSSH2_TRACE_CONN,
                      "EOF received for channel %u/%u",
                      channelp->local.id, channelp->remote.id));
            channelp->remote.eof = 1;
        }
        SSH2_FREE(session, data);
        return;

        /*
           byte      SSH_MSG_CHANNEL_REQUEST
           uint32    recipient channel
           string    request type in US-ASCII characters only
           boolean   want reply
           ....      type-specific data follows
         */

    }
    else if(msg() == SSH_MSG_CHANNEL_REQUEST) {
        if(datalen >= 9) {
            unsigned char *request;
            size_t r_len;
            struct string_buf buf;
            buf.data = data;
            buf.dataptr = buf.data;
            buf.len = datalen;

            buf.dataptr++; /* Advance past packet type */

            if(ssh2_get_u32(&buf, &channel)) {
                SSH2_FREE(session, data);
                ssh2_err(session, LIBSSH2_ERROR_PROTO,
                         "Unexpected channel value.");
                COROUT_EXIT();
            }

            if(ssh2_get_string(&buf, &request, &r_len)) {
                SSH2_FREE(session, data);
                ssh2_err(session, LIBSSH2_ERROR_PROTO,
                         "Unexpected request value.");
                COROUT_EXIT();
            }

            len = (uint32_t)r_len;

            if(ssh2_get_byte(&buf, &want_reply)) {
                SSH2_FREE(session, data);
                ssh2_err(session, LIBSSH2_ERROR_PROTO,
                         "Unexpected want reply value.");
                COROUT_EXIT();
            }

            ssh2_deb((session, LIBSSH2_TRACE_CONN,
                      "Channel %u received request type %.*s (wr %X)",
                      channel, (int)len, request, want_reply));

            if(SSH2_IS_LITERAL(request, len, "exit-status")) {
                /* we have got "exit-status" packet. Set the session value.
                 */
                if(datalen >= 20)
                    channelp = ssh2_channel_locate(session, channel);

                if(channelp) {

                    uint32_t status = 0;
                    if(ssh2_get_u32(&buf, &status))
                        session->packAdd_rc = ssh2_err(session,
                                                       LIBSSH2_ERROR_PROTO,
                                                       "exit-signal status error");
                    else
                        channelp->exit_status_received = 1;

                    channelp->exit_status = (int)status;

                    ssh2_deb((session, LIBSSH2_TRACE_CONN,
                              "Exit status %d received for channel %u/%u",
                              channelp->exit_status,
                              channelp->local.id,
                              channelp->remote.id));
                }
            }
            else if(SSH2_IS_LITERAL(request, len, "exit-signal")) {
                /* command terminated due to signal */
                if(datalen >= 20)
                    channelp = ssh2_channel_locate(session, channel);

                if(channelp) {
                    /* signal name (without SIG prefix) */
                    unsigned char *sig_name = NULL;
                    size_t sig_len = 0;
                    if(ssh2_get_string(&buf, &sig_name, &sig_len))
                        session->packAdd_rc = ssh2_err(session,
                                                       LIBSSH2_ERROR_PROTO,
                                                       "signal name protocol error");

                    if(sig_len > UINT32_MAX - 1)
                        session->packAdd_rc = ssh2_err(session,
                                                       LIBSSH2_ERROR_PROTO,
                                                       "signal name out of bounds");
                    else if(sig_len > 0) {
                        if(channelp->exit_signal)
                            SSH2_FREE(session, channelp->exit_signal);
                        channelp->exit_signal =
                            SSH2_ALLOC(session, sig_len + 1);
                        if(channelp->exit_signal) {
                            memcpy(channelp->exit_signal,
                                   sig_name, sig_len);
                            channelp->exit_signal[sig_len] = '\0';

                            ssh2_deb((session, LIBSSH2_TRACE_CONN,
                                      "Exit signal %s received for "
                                      "channel %u/%u",
                                      channelp->exit_signal,
                                      channelp->local.id,
                                      channelp->remote.id));
                        }
                        else
                            session->packAdd_rc = ssh2_err(session,
                                                           LIBSSH2_ERROR_ALLOC,
                                                           "exit signal alloc error");
                    }
                    else if(channelp->exit_signal)
                        SSH2_SAFEFREE(session, channelp->exit_signal);
                }
            }

            if(want_reply) {
                session->packAdd_reply[0] = SSH_MSG_CHANNEL_FAILURE;
                memcpy(&session->packAdd_reply[1], data + 1, 4);
                CALL(ssh2_transport_send(session, session->packAdd_reply,
                                         5, NULL, 0));
            }
        }

        SSH2_FREE(session, data);
        if(session->packAdd_rc)
            COROUT_EXIT();
        return;

        /*
           byte      SSH_MSG_CHANNEL_CLOSE
           uint32    recipient channel
         */

    }
    else if(msg() == SSH_MSG_CHANNEL_CLOSE) {
        if(datalen >= 5)
            channelp =
                ssh2_channel_locate(session, ssh2_ntohu32(data + 1));
        if(!channelp) {
            /* We may have freed already, quietly ignore this... */
            SSH2_FREE(session, data);
            return;
        }
        ssh2_deb((session, LIBSSH2_TRACE_CONN,
                  "Close received for channel %u/%u",
                  channelp->local.id, channelp->remote.id));

        channelp->remote.close = 1;
        channelp->remote.eof = 1;

        SSH2_FREE(session, data);
        return;

        /*
           byte      SSH_MSG_CHANNEL_OPEN
           string    "session"
           uint32    sender channel
           uint32    initial window size
           uint32    maximum packet size
         */

    }
    else if(msg() == SSH_MSG_CHANNEL_OPEN) {
        if(datalen >= (sizeof("forwarded-tcpip") - 1 + 5) &&
           ssh2_ntohu32(data + 1) == sizeof("forwarded-tcpip") - 1 &&
           !memcmp(data + 5, "forwarded-tcpip",
                   sizeof("forwarded-tcpip") - 1)) {

            memset(&session->packAdd_Qlstn_state, 0,
                   sizeof(session->packAdd_Qlstn_state));

            CALL(packet_queue_listener(session, data, datalen,
                                       &session->packAdd_Qlstn_state));
        }
        else if(datalen >= (sizeof("x11") - 1 + 5) &&
                ssh2_ntohu32(data + 1) == sizeof("x11") - 1 &&
                !memcmp(data + 5, "x11", sizeof("x11") - 1)) {

            memset(&session->packAdd_x11open_state, 0,
                   sizeof(session->packAdd_x11open_state));

            CALL(packet_x11_open(session, data, datalen,
                                 &session->packAdd_x11open_state));
        }
        else if(datalen >= (sizeof("auth-agent@openssh.com") - 1 + 5) &&
                ssh2_ntohu32(data + 1) ==
                    sizeof("auth-agent@openssh.com") - 1 &&
                !memcmp(data + 5, "auth-agent@openssh.com",
                        sizeof("auth-agent@openssh.com") - 1)) {

            memset(&session->packAdd_authagent_state, 0,
                   sizeof(session->packAdd_authagent_state));

            CALL(packet_authagent_open(session, data, datalen,
                                       &session->packAdd_authagent_state));
        }

        SSH2_FREE(session, data);
        return;

        /*
           byte      SSH_MSG_CHANNEL_WINDOW_ADJUST
           uint32    recipient channel
           uint32    bytes to add
         */
    }
    else if(msg() == SSH_MSG_CHANNEL_WINDOW_ADJUST) {
        if(datalen >= 9) {
            uint32_t bytestoadd = ssh2_ntohu32(data + 5);
            channelp =
                ssh2_channel_locate(session, ssh2_ntohu32(data + 1));
            if(channelp) {
                if(bytestoadd > UINT32_MAX - channelp->local.window_size)
                    rc = ssh2_err(session, LIBSSH2_ERROR_PROTO,
                                  "Window adjust out of bounds");
                else {
                    channelp->local.window_size += bytestoadd;

                    ssh2_deb((session, LIBSSH2_TRACE_CONN,
                              "Window adjust for channel %u/%u, "
                              "adding %u bytes, new window_size=%u",
                              channelp->local.id,
                              channelp->remote.id,
                              bytestoadd,
                              channelp->local.window_size));
                }
            }
        }

        SSH2_FREE(session, data);
        if(rc)
            COROUT_EXIT();
        return;
    }

    /* The packet was not handled above: queue it into the brigade. */
    {
        struct packet *packetp = SSH2_ALLOC(session, sizeof(struct packet));
        if(!packetp) {
            SSH2_FREE(session, data);
            ssh2_err(session, LIBSSH2_ERROR_ALLOC, "memory for packet");
            COROUT_EXIT();
        }
        packetp->data = data;
        packetp->data_len = datalen;
        packetp->data_head = data_head;

        ssh2_list_add(&session->packets, &packetp->node);
    }

    if(msg() == SSH_MSG_KEXINIT &&
       !(session->state & SSH2_STATE_EXCHANGING_KEYS)) {
        /*
         * The KEXINIT message has been added to the queue. Reset the kex
         * state and drive the key exchange, which (eventually) calls back into
         * ssh2_transport_read() for the rest of the conversation.
         */
        ssh2_deb((session, LIBSSH2_TRACE_TRANS, "Renegotiating Keys"));

        memset(&session->startup_key_state, 0,
               sizeof(session->startup_key_state));

        CALL(ssh2_kex_exchange(session, 1, &session->startup_key_state));
    }

    END();
#undef msg
}

/*
 * Scan the brigade for a matching packet type, optionally poll the socket for
 * a packet first
 */
int ssh2_packet_ask(LIBSSH2_SESSION *session, const unsigned char packet_type,
                    unsigned char **data, size_t *data_len,
                    const int match_ofs, const unsigned char *match_buf,
                    const size_t match_len)
{
    struct packet *packet = ssh2_list_first(&session->packets);

    ssh2_deb((session, LIBSSH2_TRACE_TRANS, "Looking for packet of type: %u",
              (unsigned int)packet_type));

    while(packet) {
        if(packet->data[0] == packet_type &&
           packet->data_len >= (match_ofs + match_len) &&
           (!match_buf ||
            !memcmp(packet->data + match_ofs, match_buf, match_len))) {
            *data = packet->data;
            *data_len = packet->data_len;

            /* unlink struct from session->packets */
            ssh2_list_remove(&packet->node);

            SSH2_FREE(session, packet);

            return 0;
        }
        else if(session->kex_strict &&
                (session->state & SSH2_STATE_INITIAL_KEX)) {
            libssh2_session_disconnect(session, "strict KEX violation: "
                                       "unexpected packet type");

            return ssh2_err(session, LIBSSH2_ERROR_SOCKET_DISCONNECT,
                            "strict KEX violation: unexpected packet type");
        }
        packet = ssh2_list_next(&packet->node);
    }
    return -1;
}

/*
 * Scan for any of a list of packet types in the brigade, optionally poll the
 * socket for a packet first
 */
static int packet_askv(LIBSSH2_SESSION *session,
                       const unsigned char *packet_types,
                       unsigned char **data, size_t *data_len,
                       const int match_ofs,
                       const unsigned char *match_buf, const size_t match_len)
{
    size_t i, packet_types_len = strlen((const char *)packet_types);

    for(i = 0; i < packet_types_len; i++) {
        if(ssh2_packet_ask(session, packet_types[i], data,
                           data_len, match_ofs,
                           match_buf, match_len) == 0)
            return 0;
    }

    return -1;
}

/*
 * Loops ssh2_transport_read() until the packet requested is available.
 * SSH_DISCONNECT or a SOCKET_DISCONNECTED causes a bailout.
 *
 * Coroutine form: ssh2_transport_read() is CALLed in a loop until the requested
 * packet is found in the brigade. Errors propagate via COROUT_EXIT().
 */
void ssh2_packet_require(LIBSSH2_SESSION *session,
                         const unsigned char packet_type,
                                unsigned char **data, size_t *data_len,
                                const int match_ofs,
                                const unsigned char *match_buf,
                                const size_t match_len)
{
    struct corout_item *state = session->corout_state;

    START();

    if(ssh2_packet_ask(session, packet_type, data, data_len,
                       match_ofs, match_buf, match_len) == 0)
        return;  /* A packet was available in the packet brigade */

    while(session->socket_state == SSH2_SOCKET_CONNECTED) {
        CALL(ssh2_transport_read(session, packet_type));

        /* Be lazy, let packet_ask pull it out of the brigade */
        if(ssh2_packet_ask(session, packet_type, data, data_len,
                           match_ofs, match_buf, match_len) == 0)
            return;
    }

    /* Only reached if the socket died */
    ssh2_err(session, LIBSSH2_ERROR_SOCKET_DISCONNECT, "socket disconnect");
    COROUT_EXIT();
    END();
}

/*
 * Loops ssh2_transport_read() until any packet is available and promptly
 * discards it.
 * Used during KEX exchange to discard badly guessed KEX_INIT packets.
 *
 * Coroutine form: reads packets until one is available to burn.
 */
void ssh2_packet_burn(LIBSSH2_SESSION *session)
{
    struct corout_item *state = session->corout_state;
    unsigned char *data;
    size_t data_len;
    unsigned char i, all_packets[255];

    START();

    for(i = 1; i < 255; i++)
        all_packets[i - 1] = i;
    all_packets[254] = 0;

    if(packet_askv(session, all_packets, &data, &data_len, 0,
                   NULL, 0) == 0) {
        i = data[0];
        SSH2_FREE(session, data);
        ssh2_deb((session, LIBSSH2_TRACE_TRANS,
                  "Burnt packet of type: %02x", (unsigned int)i));
        return;
    }

    while(session->socket_state == SSH2_SOCKET_CONNECTED) {
        CALL(ssh2_transport_read(session, 0));

        /* all_packets is a stack local lost across the yield, rebuild it */
        for(i = 1; i < 255; i++)
            all_packets[i - 1] = i;
        all_packets[254] = 0;

        if(packet_askv(session, all_packets, &data, &data_len, 0,
                       NULL, 0) == 0) {
            i = data[0];
            SSH2_FREE(session, data);
            ssh2_deb((session, LIBSSH2_TRACE_TRANS,
                      "Burnt packet of type: %02x", (unsigned int)i));
            return;
        }
    }

    /* Only reached if the socket died */
    ssh2_err(session, LIBSSH2_ERROR_SOCKET_DISCONNECT, "socket disconnect");
    COROUT_EXIT();
    END();
}

/*
 * Loops ssh2_transport_read() until one of a list of packet types
 * requested is available. SSH_DISCONNECT or a SOCKET_DISCONNECTED causes
 * a bailout. packet_types is a null-terminated list of packet_type numbers.
 *
 * Coroutine form: as ssh2_packet_require() but for a list of types.
 */
void ssh2_packet_requirev(LIBSSH2_SESSION *session,
                          const unsigned char *packet_types,
                                 unsigned char **data, size_t *data_len,
                                 const int match_ofs,
                                 const unsigned char *match_buf,
                                 const size_t match_len)
{
    struct corout_item *state = session->corout_state;

    START();

    if(packet_askv(session, packet_types, data, data_len,
                   match_ofs, match_buf, match_len) == 0)
        return;  /* One of the packets listed was available */

    while(session->socket_state != SSH2_SOCKET_DISCONNECTED) {
        CALL(ssh2_transport_read(session, 0));

        /* Be lazy, let packet_askv() pull it out of the brigade */
        if(packet_askv(session, packet_types, data, data_len,
                       match_ofs, match_buf, match_len) == 0)
            return;
    }

    /* Only reached if the socket died */
    ssh2_err(session, LIBSSH2_ERROR_SOCKET_DISCONNECT, "socket disconnect");
    COROUT_EXIT();
    END();
}
