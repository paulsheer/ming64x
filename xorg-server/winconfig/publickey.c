/* Copyright (C) Sara Golemon <sarag@libssh2.org>
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

#include "libssh2_priv.h"
#include "libssh2_publickey.h"
#include "channel.h"
#include "session.h"

/* coroutine I/O seam: START/END/CALL/GETCHAR/APPEND_BLOCK/WAIT_WRITE. */
#include "corout.h"
/* corout.h leaks socket helper macros that would rewrite libssh2's own
   ioctl/perror calls; keep only the coroutine macros. */
#undef ioctl
#undef perror
#undef RETRY

#define SSH2_PUBLICKEY_VERSION 2

/* Numericised response codes -- Not IETF, but local representation */
#define SSH2_PUBLICKEY_RESPONSE_STATUS    0
#define SSH2_PUBLICKEY_RESPONSE_VERSION   1
#define SSH2_PUBLICKEY_RESPONSE_PUBLICKEY 2

struct publickey_code_list {
    const char *name;
    int name_len;
    int code;
};

#define STRLEN(s) s, sizeof(s) - 1

static const struct publickey_code_list publickey_response_codes[] = {
    { STRLEN("status"), SSH2_PUBLICKEY_RESPONSE_STATUS },
    { STRLEN("version"), SSH2_PUBLICKEY_RESPONSE_VERSION },
    { STRLEN("publickey"), SSH2_PUBLICKEY_RESPONSE_PUBLICKEY },
    { NULL, 0, 0 }
};

/* PUBLICKEY status codes -- IETF defined */
#define SSH2_PUBLICKEY_SUCCESS               0
#define SSH2_PUBLICKEY_ACCESS_DENIED         1
#define SSH2_PUBLICKEY_STORAGE_EXCEEDED      2
#define SSH2_PUBLICKEY_VERSION_NOT_SUPPORTED 3
#define SSH2_PUBLICKEY_KEY_NOT_FOUND         4
#define SSH2_PUBLICKEY_KEY_NOT_SUPPORTED     5
#define SSH2_PUBLICKEY_KEY_ALREADY_PRESENT   6
#define SSH2_PUBLICKEY_GENERAL_FAILURE       7
#define SSH2_PUBLICKEY_REQUEST_NOT_SUPPORTED 8

#define SSH2_PUBLICKEY_STATUS_CODE_MAX       8

static const struct publickey_code_list publickey_status_codes[] = {
    { STRLEN("success"), SSH2_PUBLICKEY_SUCCESS },
    { STRLEN("access denied"), SSH2_PUBLICKEY_ACCESS_DENIED },
    { STRLEN("storage exceeded"), SSH2_PUBLICKEY_STORAGE_EXCEEDED },
    { STRLEN("version not supported"), SSH2_PUBLICKEY_VERSION_NOT_SUPPORTED },
    { STRLEN("key not found"), SSH2_PUBLICKEY_KEY_NOT_FOUND },
    { STRLEN("key not supported"), SSH2_PUBLICKEY_KEY_NOT_SUPPORTED },
    { STRLEN("key already present"), SSH2_PUBLICKEY_KEY_ALREADY_PRESENT },
    { STRLEN("general failure"), SSH2_PUBLICKEY_GENERAL_FAILURE },
    { STRLEN("request not supported"), SSH2_PUBLICKEY_REQUEST_NOT_SUPPORTED },
    { NULL, 0, 0 }
};

#undef STRLEN

/*
 * Format an error message from a status code
 */
static void publickey_status_error(const LIBSSH2_PUBLICKEY *pkey,
                                   LIBSSH2_SESSION *session,
                                   unsigned long status)
{
    const char *msg;

    /* GENERAL_FAILURE got remapped between version 1 and 2 */
    if(status == 6 && pkey && pkey->version == 1)
        status = 7;

    if(status > SSH2_PUBLICKEY_STATUS_CODE_MAX)
        msg = "unknown";
    else
        msg = publickey_status_codes[status].name;

    ssh2_err(session, LIBSSH2_ERROR_PUBLICKEY_PROTOCOL, msg);
}

/*
 * Read a packet from the subsystem
 */
static void publickey_packet_receive(LIBSSH2_PUBLICKEY *pkey,
                                     unsigned char **data, size_t *data_len)
{
    LIBSSH2_CHANNEL *channel = pkey->channel;
    LIBSSH2_SESSION *session = channel->session;
    struct corout_item *state = session->corout_state;
    unsigned char buffer[4];

    *data = NULL; /* default to nothing returned */
    *data_len = 0;

    START();

    if(pkey->receive_state == ssh2_NB_state_idle) {
        CALL(ssh2_channel_read(channel, 0, (char *)buffer, 4));
        if(channel->read_bytes != 4) {
            ssh2_err(session, LIBSSH2_ERROR_PUBLICKEY_PROTOCOL,
                     "Invalid response from publickey subsystem");
            return;
        }

        pkey->receive_packet_len = ssh2_ntohu32(buffer);
        if(pkey->receive_packet_len > LIBSSH2_PACKET_MAXPAYLOAD) {
            ssh2_err(session, LIBSSH2_ERROR_OUT_OF_BOUNDARY,
                     "Too large publickey packet");
            return;
        }
        pkey->receive_packet = SSH2_ALLOC(session, pkey->receive_packet_len);
        if(!pkey->receive_packet) {
            ssh2_err(session, LIBSSH2_ERROR_ALLOC,
                     "Unable to allocate publickey response buffer");
            return;
        }

        pkey->receive_state = ssh2_NB_state_sent;
    }

    if(pkey->receive_state == ssh2_NB_state_sent) {
        CALL(ssh2_channel_read(channel, 0, (char *)pkey->receive_packet,
                               pkey->receive_packet_len));
        if(channel->read_bytes != (ssize_t)pkey->receive_packet_len) {
            SSH2_SAFEFREE(session, pkey->receive_packet);
            pkey->receive_state = ssh2_NB_state_idle;
            ssh2_err(session, LIBSSH2_ERROR_SOCKET_TIMEOUT,
                     "Timeout waiting for publickey subsystem "
                     "response packet");
            return;
        }

        *data = pkey->receive_packet;
        *data_len = pkey->receive_packet_len;
        pkey->receive_packet = NULL;
        pkey->receive_packet_len = 0;
    }

    pkey->receive_state = ssh2_NB_state_idle;

    END();
}

/*
 * Translate a string response name to a numeric code
 * Data is incremented by 4 + response_len on success only
 */
static int publickey_response_id(unsigned char **pdata, size_t data_len)
{
    size_t response_len;
    unsigned char *data = *pdata;
    const struct publickey_code_list *codes = publickey_response_codes;

    if(data_len < 4)
        return -1;  /* Malformed response */
    response_len = ssh2_ntohu32(data);
    data += 4;
    data_len -= 4;
    if(data_len < response_len)
        return -1;  /* Malformed response */

    while(codes->name) {
        if((unsigned long)codes->name_len == response_len &&
           !strncmp(codes->name, (const char *)data, response_len)) {
            *pdata = data + response_len;
            return codes->code;
        }
        codes++;
    }

    return -1;
}

/*
 * Generic helper routine to wait for success response and nothing else
 */
static void publickey_response_success(LIBSSH2_PUBLICKEY *pkey)
{
    LIBSSH2_SESSION *session = pkey->channel->session;
    struct corout_item *state = session->corout_state;
    unsigned char *data, *s;
    size_t data_len;
    int response;

    START();

    for(;;) {
        CALL(publickey_packet_receive(pkey, &data, &data_len));

        if(data_len < 4) {
            SSH2_FREE(session, data);
            ssh2_err(session, LIBSSH2_ERROR_BUFFER_TOO_SMALL,
                     "Publickey response too small");
            return;
        }

        s = data;
        response = publickey_response_id(&s, data_len);

        switch(response) {
        case SSH2_PUBLICKEY_RESPONSE_STATUS: {
            /* Error, or processing complete */
            unsigned long status = 0;

            if(data_len < (size_t)(s - data) + 4) {
                SSH2_FREE(session, data);
                ssh2_err(session, LIBSSH2_ERROR_BUFFER_TOO_SMALL,
                         "Publickey response too small");
                return;
            }

            status = ssh2_ntohu32(s);

            SSH2_FREE(session, data);

            if(status == SSH2_PUBLICKEY_SUCCESS)
                return;

            publickey_status_error(pkey, session, status);
            goto err_exit;
        }
        default:
            SSH2_FREE(session, data);
            if(response < 0) {
                ssh2_err(session, LIBSSH2_ERROR_PUBLICKEY_PROTOCOL,
                         "Invalid publickey subsystem response");
                return;
            }
            /* Unknown/Unexpected */
            ssh2_err(session, LIBSSH2_ERROR_PUBLICKEY_PROTOCOL,
                     "Unexpected publickey subsystem response");
            data = NULL;
        }
    }
err_exit:
    END();
}

/* ***************
 * Publickey API *
 *************** */

/*
 * Startup the publickey subsystem
 */
static void publickey_init(LIBSSH2_SESSION *session)
{
    struct corout_item *state = session->corout_state;
    int response;

    START();

    if(session->pkeyInit_state == ssh2_NB_state_idle) {
        session->pkeyInit_data = NULL;
        session->pkeyInit_pkey = NULL;
        session->pkeyInit_channel = NULL;

        ssh2_deb((session, LIBSSH2_TRACE_PUBLICKEY,
                  "Initializing publickey subsystem"));

        session->pkeyInit_state = ssh2_NB_state_allocated;
    }

    if(session->pkeyInit_state == ssh2_NB_state_allocated) {
        CALL(ssh2_channel_open(session, "session", sizeof("session") - 1,
                               LIBSSH2_CHANNEL_WINDOW_DEFAULT,
                               LIBSSH2_CHANNEL_PACKET_DEFAULT, NULL, 0));
        session->pkeyInit_channel = session->open_channel;
        if(!session->pkeyInit_channel) {
            ssh2_err(session, LIBSSH2_ERROR_CHANNEL_FAILURE,
                     "Unable to startup channel");
            goto err_exit;
        }

        session->pkeyInit_state = ssh2_NB_state_sent;
    }

    if(session->pkeyInit_state == ssh2_NB_state_sent) {
        CALL(ssh2_channel_process_startup(session->pkeyInit_channel,
                                          "subsystem",
                                          sizeof("subsystem") - 1,
                                          "publickey",
                                          sizeof("publickey") - 1));

        session->pkeyInit_state = ssh2_NB_state_sent1;
    }

    if(session->pkeyInit_state == ssh2_NB_state_sent1) {
        unsigned char *s;
        CALL(ssh2_channel_extended_data(session->pkeyInit_channel,
                                        LIBSSH2_CHANNEL_EXTENDED_DATA_IGNORE));

        session->pkeyInit_pkey =
            SSH2_CALLOC(session, sizeof(LIBSSH2_PUBLICKEY));
        if(!session->pkeyInit_pkey) {
            ssh2_err(session, LIBSSH2_ERROR_ALLOC,
                     "Unable to allocate a new publickey structure");
            goto err_exit;
        }
        session->pkeyInit_pkey->channel = session->pkeyInit_channel;
        session->pkeyInit_pkey->version = 0;

        s = session->pkeyInit_buffer;
        ssh2_htonu32(s, 4 + (sizeof("version") - 1) + 4);
        s += 4;
        ssh2_htonu32(s, sizeof("version") - 1);
        s += 4;
        memcpy(s, "version", sizeof("version") - 1);
        s += sizeof("version") - 1;
        ssh2_htonu32(s, SSH2_PUBLICKEY_VERSION);

        session->pkeyInit_buffer_sent = 0;

        ssh2_deb((session, LIBSSH2_TRACE_PUBLICKEY,
                  "Sending publickey advertising version %d support",
                  (int)SSH2_PUBLICKEY_VERSION));

        session->pkeyInit_state = ssh2_NB_state_sent2;
    }

    if(session->pkeyInit_state == ssh2_NB_state_sent2) {
        CALL(ssh2_channel_write(session->pkeyInit_channel, 0,
                                session->pkeyInit_buffer,
                                sizeof(session->pkeyInit_buffer) -
                                session->pkeyInit_buffer_sent));
        session->pkeyInit_buffer_sent += session->pkeyInit_channel->write_bytes;
        session->pkeyInit_state = ssh2_NB_state_sent3;
    }

    if(session->pkeyInit_state == ssh2_NB_state_sent3) {
        for(;;) {
            unsigned char *s;
            CALL(publickey_packet_receive(session->pkeyInit_pkey,
                                          &session->pkeyInit_data,
                                          &session->pkeyInit_data_len));

            s = session->pkeyInit_data;
            response = publickey_response_id(&s, session->pkeyInit_data_len);
            if(response < 0) {
                ssh2_err(session, LIBSSH2_ERROR_PUBLICKEY_PROTOCOL,
                         "Invalid publickey subsystem response code");
                goto err_exit;
            }

            switch(response) {
            case SSH2_PUBLICKEY_RESPONSE_STATUS: {
                /* Error */
                unsigned long status, descr_len, lang_len;

                if(session->pkeyInit_data_len <
                   (size_t)(s - session->pkeyInit_data) + 8) {
                    ssh2_err(session, LIBSSH2_ERROR_BUFFER_TOO_SMALL,
                             "Public key init data too small");
                    goto err_exit;
                }
                status = ssh2_ntohu32(s);
                s += 4;
                descr_len = ssh2_ntohu32(s);
                s += 4;

                if(descr_len > LIBSSH2_PACKET_MAXPAYLOAD) {
                    ssh2_err(session, LIBSSH2_ERROR_OUT_OF_BOUNDARY,
                             "Public key description too large");
                    goto err_exit;
                }
                if(session->pkeyInit_data_len <
                   (size_t)(s - session->pkeyInit_data) + descr_len + 4) {
                    ssh2_err(session, LIBSSH2_ERROR_BUFFER_TOO_SMALL,
                             "Public key init data too small");
                    goto err_exit;
                }
                /* description starts here */
                s += descr_len;
                lang_len = ssh2_ntohu32(s);
                s += 4;

                if(lang_len > LIBSSH2_PACKET_MAXPAYLOAD) {
                    ssh2_err(session, LIBSSH2_ERROR_OUT_OF_BOUNDARY,
                             "Public key language too large");
                    goto err_exit;
                }
                if(session->pkeyInit_data_len <
                   (size_t)(s - session->pkeyInit_data) + lang_len) {
                    ssh2_err(session, LIBSSH2_ERROR_BUFFER_TOO_SMALL,
                             "Public key init data too small");
                    goto err_exit;
                }
                /* lang starts here */
                s += lang_len;

                if(session->pkeyInit_data_len <
                   (size_t)(s - session->pkeyInit_data)) {
                    ssh2_err(session, LIBSSH2_ERROR_PUBLICKEY_PROTOCOL,
                             "Malformed publickey subsystem packet");
                    goto err_exit;
                }

                publickey_status_error(NULL, session, status);

                goto err_exit;
            }

            case SSH2_PUBLICKEY_RESPONSE_VERSION:
                /* What we want */
                if(session->pkeyInit_data_len <
                   (size_t)(s - session->pkeyInit_data) + 4) {
                    ssh2_err(session, LIBSSH2_ERROR_BUFFER_TOO_SMALL,
                             "Public key version response too small");
                    goto err_exit;
                }
                session->pkeyInit_pkey->version = ssh2_ntohu32(s);
                if(session->pkeyInit_pkey->version > SSH2_PUBLICKEY_VERSION) {
                    ssh2_deb((session, LIBSSH2_TRACE_PUBLICKEY,
                              "Truncate remote publickey version from %u",
                              session->pkeyInit_pkey->version));
                    session->pkeyInit_pkey->version = SSH2_PUBLICKEY_VERSION;
                }
                ssh2_deb((session, LIBSSH2_TRACE_PUBLICKEY,
                          "Enabling publickey subsystem version %u",
                          session->pkeyInit_pkey->version));
                SSH2_SAFEFREE(session, session->pkeyInit_data);
                session->pkeyInit_state = ssh2_NB_state_idle;
                return;

            default:
                /* Unknown/Unexpected */
                ssh2_err(session, LIBSSH2_ERROR_PUBLICKEY_PROTOCOL,
                         "Unexpected publickey subsystem response, ignoring");
                SSH2_SAFEFREE(session, session->pkeyInit_data);
            }
        }
    }

    /* Never reached except by direct goto */
err_exit:
    if(session->pkeyInit_channel)
        CALL(ssh2_channel_close(session->pkeyInit_channel));
    if(session->pkeyInit_pkey)
        SSH2_SAFEFREE(session, session->pkeyInit_pkey);
    if(session->pkeyInit_data)
        SSH2_SAFEFREE(session, session->pkeyInit_data);
    session->pkeyInit_pkey = NULL;
    session->pkeyInit_state = ssh2_NB_state_idle;
    END();
}

/*
 * Startup the publickey subsystem
 */
void libssh2_publickey_init(LIBSSH2_SESSION *session)
{
    struct corout_item *state;

    if(!session)
        return;

    state = session->corout_state;
    START();
    CALL(publickey_init(session));
    END();
}

#define PUBLICKEY_ATTRS_MAX  1024

/*
 * Add a new public key entry
 */
void libssh2_publickey_add_ex(LIBSSH2_PUBLICKEY *pkey,
                              const unsigned char *name,
                              const unsigned long name_len,
                              const unsigned char *blob,
                              const unsigned long blob_len,
                              const char overwrite,
                              const unsigned long num_attrs,
                              const libssh2_publickey_attribute attrs[])
{
    LIBSSH2_CHANNEL *channel;
    LIBSSH2_SESSION *session;
    struct corout_item *state;
    unsigned long i, packet_len;
    const char *comment = NULL;
    unsigned long comment_len = 0;

    if(!pkey)
        return;

    channel = pkey->channel;
    session = channel->session;
    state = session->corout_state;

    if(!name || !blob || (num_attrs && !attrs)) {
        ssh2_err(session, LIBSSH2_ERROR_BAD_USE, "Invalid argument");
        return;
    }

    if(name_len > LIBSSH2_PACKET_MAXPAYLOAD ||
       blob_len > LIBSSH2_PACKET_MAXPAYLOAD ||
       num_attrs > PUBLICKEY_ATTRS_MAX) {
        ssh2_err(session, LIBSSH2_ERROR_OUT_OF_BOUNDARY,
                 "Argument out of bounds");
        return;
    }

    /* 19 = packet_len(4) + add_len(4) + "add"(3) + name_len(4) + {name}
       blob_len(4) + {blob} */
    packet_len = 19 + name_len + blob_len;

    START();

    if(pkey->add_state == ssh2_NB_state_idle) {
        pkey->add_packet = NULL;

        ssh2_deb((session, LIBSSH2_TRACE_PUBLICKEY, "Adding %.*s publickey",
                  (int)name_len, name));

        if(pkey->version == 1) {
            for(i = 0; i < num_attrs; i++) {
                /* Search for a comment attribute */
                if(attrs[i].name_len == (sizeof("comment") - 1) &&
                   attrs[i].name &&
                   !strncmp(attrs[i].name, "comment", sizeof("comment") - 1)) {
                    if(!attrs[i].value) {
                        ssh2_err(session, LIBSSH2_ERROR_BAD_USE,
                                 "Invalid argument");
                        return;
                    }
                    if(attrs[i].value_len > LIBSSH2_PACKET_MAXPAYLOAD) {
                        ssh2_err(session, LIBSSH2_ERROR_OUT_OF_BOUNDARY,
                                 "Argument out of bounds");
                        return;
                    }
                    comment = attrs[i].value;
                    comment_len = attrs[i].value_len;
                    break;
                }
            }
            packet_len += 4 + comment_len;
        }
        else {
            packet_len += 5; /* overwrite(1) + attribute_count(4) */
            for(i = 0; i < num_attrs; i++) {
                if(!attrs[i].name ||
                   !attrs[i].value) {
                    ssh2_err(session, LIBSSH2_ERROR_BAD_USE,
                             "Invalid argument");
                    return;
                }
                if(attrs[i].name_len > LIBSSH2_PACKET_MAXPAYLOAD ||
                   attrs[i].value_len > LIBSSH2_PACKET_MAXPAYLOAD) {
                    ssh2_err(session, LIBSSH2_ERROR_OUT_OF_BOUNDARY,
                             "Argument out of bounds");
                    return;
                }
                /* name_len(4) + value_len(4) + mandatory(1) */
                packet_len += 9 + attrs[i].name_len + attrs[i].value_len;
            }
        }

        if((packet_len - 4) > LIBSSH2_PACKET_MAXPAYLOAD) {
            ssh2_err(session, LIBSSH2_ERROR_OUT_OF_BOUNDARY,
                     "Packet too large");
            return;
        }

        pkey->add_packet = SSH2_ALLOC(session, packet_len);
        if(!pkey->add_packet) {
            ssh2_err(session, LIBSSH2_ERROR_ALLOC,
                     "Unable to allocate memory for "
                     "publickey 'add' packet");
            return;
        }

        pkey->add_s = pkey->add_packet;
        ssh2_htonu32(pkey->add_s, (uint32_t)(packet_len - 4));
        pkey->add_s += 4;
        ssh2_htonu32(pkey->add_s, sizeof("add") - 1);
        pkey->add_s += 4;
        memcpy(pkey->add_s, "add", sizeof("add") - 1);
        pkey->add_s += sizeof("add") - 1;
        if(pkey->version == 1) {
            ssh2_htonu32(pkey->add_s, (uint32_t)comment_len);
            pkey->add_s += 4;
            if(comment) {
                memcpy(pkey->add_s, comment, comment_len);
                pkey->add_s += comment_len;
            }

            ssh2_htonu32(pkey->add_s, (uint32_t)name_len);
            pkey->add_s += 4;
            memcpy(pkey->add_s, name, name_len);
            pkey->add_s += name_len;
            ssh2_htonu32(pkey->add_s, (uint32_t)blob_len);
            pkey->add_s += 4;
            memcpy(pkey->add_s, blob, blob_len);
            pkey->add_s += blob_len;
        }
        else {
            /* Version == 2 */

            ssh2_htonu32(pkey->add_s, (uint32_t)name_len);
            pkey->add_s += 4;
            memcpy(pkey->add_s, name, name_len);
            pkey->add_s += name_len;
            ssh2_htonu32(pkey->add_s, (uint32_t)blob_len);
            pkey->add_s += 4;
            memcpy(pkey->add_s, blob, blob_len);
            pkey->add_s += blob_len;
            *(pkey->add_s++) = overwrite ? 0x01 : 0;
            ssh2_htonu32(pkey->add_s, (uint32_t)num_attrs);
            pkey->add_s += 4;
            for(i = 0; i < num_attrs; i++) {
                ssh2_htonu32(pkey->add_s, (uint32_t)attrs[i].name_len);
                pkey->add_s += 4;
                memcpy(pkey->add_s, attrs[i].name, attrs[i].name_len);
                pkey->add_s += attrs[i].name_len;
                ssh2_htonu32(pkey->add_s, (uint32_t)attrs[i].value_len);
                pkey->add_s += 4;
                memcpy(pkey->add_s, attrs[i].value, attrs[i].value_len);
                pkey->add_s += attrs[i].value_len;
                *(pkey->add_s++) = attrs[i].mandatory ? 0x01 : 0;
            }
        }

        ssh2_deb((session, LIBSSH2_TRACE_PUBLICKEY,
                  "Sending publickey 'add' packet: "
                  "type=%.*s blob_len=%lu num_attrs=%lu",
                  (int)name_len, name, blob_len, num_attrs));

        pkey->add_state = ssh2_NB_state_created;
    }

    if(pkey->add_state == ssh2_NB_state_created) {
        CALL(ssh2_channel_write(channel, 0, pkey->add_packet,
                                (pkey->add_s - pkey->add_packet)));
        if((pkey->add_s - pkey->add_packet) != channel->write_bytes) {
            SSH2_SAFEFREE(session, pkey->add_packet);
            ssh2_err(session, LIBSSH2_ERROR_SOCKET_SEND,
                     "Unable to send publickey add packet");
            return;
        }
        SSH2_SAFEFREE(session, pkey->add_packet);
        pkey->add_state = ssh2_NB_state_sent;
    }

    CALL(publickey_response_success(pkey));

    pkey->add_state = ssh2_NB_state_idle;

    ssh2_err(session, LIBSSH2_ERROR_NONE, "No error");

    END();
}

/*
 * Remove an existing publickey so that authentication can no longer be
 * performed using it
 */
void libssh2_publickey_remove_ex(LIBSSH2_PUBLICKEY *pkey,
                                 const unsigned char *name,
                                 const unsigned long name_len,
                                 const unsigned char *blob,
                                 const unsigned long blob_len)
{
    LIBSSH2_CHANNEL *channel;
    LIBSSH2_SESSION *session;
    struct corout_item *state;
    unsigned long packet_len;

    if(!pkey)
        return;

    channel = pkey->channel;
    session = channel->session;
    state = session->corout_state;

    if(!name || !blob) {
        ssh2_err(session, LIBSSH2_ERROR_BAD_USE, "Invalid argument");
        return;
    }

    if(name_len > LIBSSH2_PACKET_MAXPAYLOAD ||
       blob_len > LIBSSH2_PACKET_MAXPAYLOAD) {
        ssh2_err(session, LIBSSH2_ERROR_OUT_OF_BOUNDARY,
                 "Argument out of bounds");
        return;
    }

    /* 22 = packet_len(4) + remove_len(4) + "remove"(6) + name_len(4) + {name}
       + blob_len(4) + {blob} */
    packet_len = 22 + name_len + blob_len;

    START();

    if(pkey->remove_state == ssh2_NB_state_idle) {
        pkey->remove_packet = NULL;

        if((packet_len - 4) > LIBSSH2_PACKET_MAXPAYLOAD) {
            ssh2_err(session, LIBSSH2_ERROR_OUT_OF_BOUNDARY,
                     "Packet too large");
            return;
        }

        pkey->remove_packet = SSH2_ALLOC(session, packet_len);
        if(!pkey->remove_packet) {
            ssh2_err(session, LIBSSH2_ERROR_ALLOC,
                     "Unable to allocate memory for "
                     "publickey 'remove' packet");
            return;
        }

        pkey->remove_s = pkey->remove_packet;
        ssh2_htonu32(pkey->remove_s, (uint32_t)(packet_len - 4));
        pkey->remove_s += 4;
        ssh2_htonu32(pkey->remove_s, sizeof("remove") - 1);
        pkey->remove_s += 4;
        memcpy(pkey->remove_s, "remove", sizeof("remove") - 1);
        pkey->remove_s += sizeof("remove") - 1;
        ssh2_htonu32(pkey->remove_s, (uint32_t)name_len);
        pkey->remove_s += 4;
        memcpy(pkey->remove_s, name, name_len);
        pkey->remove_s += name_len;
        ssh2_htonu32(pkey->remove_s, (uint32_t)blob_len);
        pkey->remove_s += 4;
        memcpy(pkey->remove_s, blob, blob_len);
        pkey->remove_s += blob_len;

        ssh2_deb((session, LIBSSH2_TRACE_PUBLICKEY,
                  "Sending publickey 'remove' packet: type=%.*s blob_len=%lu",
                  (int)name_len, name, blob_len));

        pkey->remove_state = ssh2_NB_state_created;
    }

    if(pkey->remove_state == ssh2_NB_state_created) {
        CALL(ssh2_channel_write(channel, 0, pkey->remove_packet,
                                (pkey->remove_s - pkey->remove_packet)));
        if((pkey->remove_s - pkey->remove_packet) != channel->write_bytes) {
            SSH2_SAFEFREE(session, pkey->remove_packet);
            pkey->remove_state = ssh2_NB_state_idle;
            ssh2_err(session, LIBSSH2_ERROR_SOCKET_SEND,
                     "Unable to send publickey remove packet");
            return;
        }
        SSH2_SAFEFREE(session, pkey->remove_packet);
        pkey->remove_state = ssh2_NB_state_sent;
    }

    CALL(publickey_response_success(pkey));

    pkey->remove_state = ssh2_NB_state_idle;

    ssh2_err(session, LIBSSH2_ERROR_NONE, "No error");

    END();
}

/*
 * Fetch a list of supported public keys from a server
 */
void libssh2_publickey_list_fetch(LIBSSH2_PUBLICKEY *pkey,
                                  unsigned long *num_keys,
                                  libssh2_publickey_list **pkey_list)
{
    LIBSSH2_CHANNEL *channel;
    LIBSSH2_SESSION *session;
    struct corout_item *state;
    libssh2_publickey_list *list;
    /* 12 = packet_len(4) + list_len(4) + "list"(4) */
    unsigned long buffer_len = 12, keys, max_keys, i;
    int response;

    if(!pkey)
        return;

    channel = pkey->channel;
    session = channel->session;
    state = session->corout_state;

    START();

    if(pkey->listFetch_state == ssh2_NB_state_idle) {
        pkey->listFetch_data = NULL;
        pkey->listFetch_list = NULL;
        pkey->listFetch_keys = 0;
        pkey->listFetch_max_keys = 0;

        pkey->listFetch_s = pkey->listFetch_buffer;
        ssh2_htonu32(pkey->listFetch_s, (uint32_t)(buffer_len - 4));
        pkey->listFetch_s += 4;
        ssh2_htonu32(pkey->listFetch_s, sizeof("list") - 1);
        pkey->listFetch_s += 4;
        memcpy(pkey->listFetch_s, "list", sizeof("list") - 1);
        pkey->listFetch_s += sizeof("list") - 1;

        ssh2_deb((session, LIBSSH2_TRACE_PUBLICKEY,
                  "Sending publickey 'list' packet"));

        pkey->listFetch_state = ssh2_NB_state_created;
    }

    if(pkey->listFetch_state == ssh2_NB_state_created) {
        CALL(ssh2_channel_write(channel, 0,
                                pkey->listFetch_buffer,
                                (pkey->listFetch_s -
                                 pkey->listFetch_buffer)));
        if((pkey->listFetch_s - pkey->listFetch_buffer) !=
           channel->write_bytes) {
            pkey->listFetch_state = ssh2_NB_state_idle;
            ssh2_err(session, LIBSSH2_ERROR_SOCKET_SEND,
                     "Unable to send publickey list packet");
            return;
        }

        pkey->listFetch_state = ssh2_NB_state_sent;
    }

    list = pkey->listFetch_list;
    keys = pkey->listFetch_keys;
    max_keys = pkey->listFetch_max_keys;

    for(;;) {
        /* Persist the (possibly realloc'd) list across the receive yield. */
        pkey->listFetch_list = list;
        pkey->listFetch_keys = keys;
        pkey->listFetch_max_keys = max_keys;

        CALL(publickey_packet_receive(pkey, &pkey->listFetch_data,
                                      &pkey->listFetch_data_len));

        list = pkey->listFetch_list;
        keys = pkey->listFetch_keys;
        max_keys = pkey->listFetch_max_keys;

        pkey->listFetch_s = pkey->listFetch_data;
        response = publickey_response_id(&pkey->listFetch_s,
                                         pkey->listFetch_data_len);
        if(response < 0) {
            ssh2_err(session, LIBSSH2_ERROR_PUBLICKEY_PROTOCOL,
                     "Invalid publickey subsystem response code");
            goto err_exit;
        }

        switch(response) {
        case SSH2_PUBLICKEY_RESPONSE_STATUS: {
            /* Error, or processing complete */
            unsigned long status, descr_len, lang_len;

            if(pkey->listFetch_data_len <
               (size_t)(pkey->listFetch_s - pkey->listFetch_data) + 8) {
                ssh2_err(session, LIBSSH2_ERROR_BUFFER_TOO_SMALL,
                         "ListFetch data too short");
                goto err_exit;
            }
            status = ssh2_ntohu32(pkey->listFetch_s);
            pkey->listFetch_s += 4;
            descr_len = ssh2_ntohu32(pkey->listFetch_s);
            pkey->listFetch_s += 4;

            if(descr_len > LIBSSH2_PACKET_MAXPAYLOAD) {
                ssh2_err(session, LIBSSH2_ERROR_OUT_OF_BOUNDARY,
                         "Public key description too large");
                goto err_exit;
            }
            if(pkey->listFetch_data_len <
               (size_t)(pkey->listFetch_s - pkey->listFetch_data) +
               descr_len + 4) {
                ssh2_err(session, LIBSSH2_ERROR_BUFFER_TOO_SMALL,
                         "ListFetch data too short");
                goto err_exit;
            }
            /* description starts at pkey->listFetch_s */
            pkey->listFetch_s += descr_len;
            lang_len = ssh2_ntohu32(pkey->listFetch_s);
            pkey->listFetch_s += 4;

            if(lang_len > LIBSSH2_PACKET_MAXPAYLOAD) {
                ssh2_err(session, LIBSSH2_ERROR_OUT_OF_BOUNDARY,
                         "Public key language too large");
                goto err_exit;
            }
            if(pkey->listFetch_data_len <
               (size_t)(pkey->listFetch_s - pkey->listFetch_data) + lang_len) {
                ssh2_err(session, LIBSSH2_ERROR_BUFFER_TOO_SMALL,
                         "ListFetch data too short");
                goto err_exit;
            }
            /* lang starts at pkey->listFetch_s */
            pkey->listFetch_s += lang_len;

            if(pkey->listFetch_data_len <
               (size_t)(pkey->listFetch_s - pkey->listFetch_data)) {
                ssh2_err(session, LIBSSH2_ERROR_PUBLICKEY_PROTOCOL,
                         "Malformed publickey subsystem packet");
                goto err_exit;
            }

            if(status == SSH2_PUBLICKEY_SUCCESS) {
                SSH2_SAFEFREE(session, pkey->listFetch_data);
                *pkey_list = list;
                *num_keys = keys;
                pkey->listFetch_list = NULL;
                pkey->listFetch_keys = 0;
                pkey->listFetch_max_keys = 0;
                pkey->listFetch_state = ssh2_NB_state_idle;
                return;
            }

            publickey_status_error(pkey, session, status);
            goto err_exit;
        }
        case SSH2_PUBLICKEY_RESPONSE_PUBLICKEY:
            /* What we want */
            if(keys >= 32768) {
                ssh2_err(session, LIBSSH2_ERROR_OUT_OF_BOUNDARY,
                         "Too many public keys");
                goto err_exit;
            }
            if(keys >= max_keys) {
                libssh2_publickey_list *newlist;
                /* Grow the key list if necessary */
                max_keys += 8;
                newlist = SSH2_REALLOC(session, list,
                                       (max_keys + 1) *
                                           sizeof(libssh2_publickey_list));
                if(!newlist) {
                    ssh2_err(session, LIBSSH2_ERROR_ALLOC,
                             "Unable to allocate memory for publickey list");
                    goto err_exit;
                }
                list = newlist;
                memset(&list[keys], 0,
                       (max_keys - keys + 1) * sizeof(list[keys]));
            }
            if(pkey->version == 1) {
                unsigned long comment_len;

                if(pkey->listFetch_data_len <
                   (size_t)(pkey->listFetch_s - pkey->listFetch_data) + 4) {
                    ssh2_err(session, LIBSSH2_ERROR_BUFFER_TOO_SMALL,
                             "ListFetch data too short");
                    goto err_exit;
                }
                comment_len = ssh2_ntohu32(pkey->listFetch_s);
                pkey->listFetch_s += 4;

                if(comment_len) {
                    if(comment_len > LIBSSH2_PACKET_MAXPAYLOAD) {
                        ssh2_err(session, LIBSSH2_ERROR_OUT_OF_BOUNDARY,
                                 "Public key comment too large");
                        goto err_exit;
                    }
                    if(pkey->listFetch_data_len <
                       (size_t)(pkey->listFetch_s - pkey->listFetch_data) +
                       comment_len) {
                        ssh2_err(session, LIBSSH2_ERROR_BUFFER_TOO_SMALL,
                                 "ListFetch data too short");
                        goto err_exit;
                    }

                    list[keys].num_attrs = 1;
                    list[keys].attrs =
                        SSH2_ALLOC(session,
                                   sizeof(libssh2_publickey_attribute));
                    if(!list[keys].attrs) {
                        ssh2_err(session, LIBSSH2_ERROR_ALLOC,
                                 "Unable to allocate memory for "
                                 "publickey attributes");
                        goto err_exit;
                    }
                    list[keys].attrs[0].name = "comment";
                    list[keys].attrs[0].name_len = sizeof("comment") - 1;
                    list[keys].attrs[0].value =
                        (const char *)pkey->listFetch_s;
                    list[keys].attrs[0].value_len = comment_len;
                    list[keys].attrs[0].mandatory = 0;

                    pkey->listFetch_s += comment_len;
                }
                else {
                    list[keys].num_attrs = 0;
                    list[keys].attrs = NULL;
                }

                if(pkey->listFetch_data_len <
                   (size_t)(pkey->listFetch_s - pkey->listFetch_data) + 4) {
                    ssh2_err(session, LIBSSH2_ERROR_BUFFER_TOO_SMALL,
                             "ListFetch data too short");
                    goto err_exit;
                }
                list[keys].name_len = ssh2_ntohu32(pkey->listFetch_s);
                pkey->listFetch_s += 4;

                if(list[keys].name_len > LIBSSH2_PACKET_MAXPAYLOAD) {
                    ssh2_err(session, LIBSSH2_ERROR_OUT_OF_BOUNDARY,
                             "Public key name too large");
                    goto err_exit;
                }
                if(pkey->listFetch_data_len <
                   (size_t)(pkey->listFetch_s - pkey->listFetch_data) +
                   list[keys].name_len) {
                    ssh2_err(session, LIBSSH2_ERROR_BUFFER_TOO_SMALL,
                             "ListFetch data too short");
                    goto err_exit;
                }
                list[keys].name = pkey->listFetch_s;
                pkey->listFetch_s += list[keys].name_len;

                if(pkey->listFetch_data_len <
                   (size_t)(pkey->listFetch_s - pkey->listFetch_data) + 4) {
                    ssh2_err(session, LIBSSH2_ERROR_BUFFER_TOO_SMALL,
                             "ListFetch data too short");
                    goto err_exit;
                }
                list[keys].blob_len = ssh2_ntohu32(pkey->listFetch_s);
                pkey->listFetch_s += 4;

                if(list[keys].blob_len > LIBSSH2_PACKET_MAXPAYLOAD) {
                    ssh2_err(session, LIBSSH2_ERROR_OUT_OF_BOUNDARY,
                             "Public key blob too large");
                    goto err_exit;
                }
                if(pkey->listFetch_data_len <
                   (size_t)(pkey->listFetch_s - pkey->listFetch_data) +
                   list[keys].blob_len) {
                    ssh2_err(session, LIBSSH2_ERROR_BUFFER_TOO_SMALL,
                             "ListFetch data too short");
                    goto err_exit;
                }
                list[keys].blob = pkey->listFetch_s;
                pkey->listFetch_s += list[keys].blob_len;
            }
            else {
                /* Version == 2 */

                if(pkey->listFetch_data_len <
                   (size_t)(pkey->listFetch_s - pkey->listFetch_data) + 4) {
                    ssh2_err(session, LIBSSH2_ERROR_BUFFER_TOO_SMALL,
                             "ListFetch data too short");
                    goto err_exit;
                }
                list[keys].name_len = ssh2_ntohu32(pkey->listFetch_s);
                pkey->listFetch_s += 4;

                if(list[keys].name_len > LIBSSH2_PACKET_MAXPAYLOAD) {
                    ssh2_err(session, LIBSSH2_ERROR_OUT_OF_BOUNDARY,
                             "Public key name too large");
                    goto err_exit;
                }
                if(pkey->listFetch_data_len <
                   (size_t)(pkey->listFetch_s - pkey->listFetch_data) +
                   list[keys].name_len) {
                    ssh2_err(session, LIBSSH2_ERROR_BUFFER_TOO_SMALL,
                             "ListFetch data too short");
                    goto err_exit;
                }
                list[keys].name = pkey->listFetch_s;
                pkey->listFetch_s += list[keys].name_len;

                if(pkey->listFetch_data_len <
                   (size_t)(pkey->listFetch_s - pkey->listFetch_data) + 4) {
                    ssh2_err(session, LIBSSH2_ERROR_BUFFER_TOO_SMALL,
                             "ListFetch data too short");
                    goto err_exit;
                }
                list[keys].blob_len = ssh2_ntohu32(pkey->listFetch_s);
                pkey->listFetch_s += 4;

                if(list[keys].blob_len > LIBSSH2_PACKET_MAXPAYLOAD) {
                    ssh2_err(session, LIBSSH2_ERROR_OUT_OF_BOUNDARY,
                             "Public key blob too large");
                    goto err_exit;
                }
                if(pkey->listFetch_data_len <
                   (size_t)(pkey->listFetch_s - pkey->listFetch_data) +
                   list[keys].blob_len) {
                    ssh2_err(session, LIBSSH2_ERROR_BUFFER_TOO_SMALL,
                             "ListFetch data too short");
                    goto err_exit;
                }
                list[keys].blob = pkey->listFetch_s;
                pkey->listFetch_s += list[keys].blob_len;

                if(pkey->listFetch_data_len <
                   (size_t)(pkey->listFetch_s - pkey->listFetch_data) + 4) {
                    ssh2_err(session, LIBSSH2_ERROR_BUFFER_TOO_SMALL,
                             "ListFetch data too short");
                    goto err_exit;
                }
                list[keys].num_attrs = ssh2_ntohu32(pkey->listFetch_s);
                pkey->listFetch_s += 4;

                if(list[keys].num_attrs) {
                    if(list[keys].num_attrs > PUBLICKEY_ATTRS_MAX) {
                        ssh2_err(session, LIBSSH2_ERROR_OUT_OF_BOUNDARY,
                                 "Too many publickey attributes");
                        goto err_exit;
                    }
                    list[keys].attrs =
                        SSH2_ALLOC(session,
                                   list[keys].num_attrs *
                                       sizeof(libssh2_publickey_attribute));
                    if(!list[keys].attrs) {
                        ssh2_err(session, LIBSSH2_ERROR_ALLOC,
                                 "Unable to allocate memory for "
                                 "publickey attributes");
                        goto err_exit;
                    }
                    for(i = 0; i < list[keys].num_attrs; i++) {
                        if(pkey->listFetch_data_len <
                           (size_t)(pkey->listFetch_s - pkey->listFetch_data) +
                           4) {
                            ssh2_err(session, LIBSSH2_ERROR_BUFFER_TOO_SMALL,
                                     "ListFetch data too short");
                            goto err_exit;
                        }
                        list[keys].attrs[i].name_len =
                            ssh2_ntohu32(pkey->listFetch_s);
                        pkey->listFetch_s += 4;

                        if(list[keys].attrs[i].name_len >
                           LIBSSH2_PACKET_MAXPAYLOAD) {
                            ssh2_err(session, LIBSSH2_ERROR_OUT_OF_BOUNDARY,
                                     "Public key attribute name too large");
                            goto err_exit;
                        }
                        if(pkey->listFetch_data_len <
                           (size_t)(pkey->listFetch_s - pkey->listFetch_data) +
                           list[keys].attrs[i].name_len) {
                            ssh2_err(session, LIBSSH2_ERROR_BUFFER_TOO_SMALL,
                                     "ListFetch data too short");
                            goto err_exit;
                        }
                        list[keys].attrs[i].name = (char *)pkey->listFetch_s;
                        pkey->listFetch_s += list[keys].attrs[i].name_len;

                        if(pkey->listFetch_data_len <
                           (size_t)(pkey->listFetch_s - pkey->listFetch_data) +
                           4) {
                            ssh2_err(session, LIBSSH2_ERROR_BUFFER_TOO_SMALL,
                                     "ListFetch data too short");
                            goto err_exit;
                        }
                        list[keys].attrs[i].value_len =
                            ssh2_ntohu32(pkey->listFetch_s);
                        pkey->listFetch_s += 4;

                        if(list[keys].attrs[i].value_len >
                           LIBSSH2_PACKET_MAXPAYLOAD) {
                            ssh2_err(session, LIBSSH2_ERROR_OUT_OF_BOUNDARY,
                                     "Public key attribute value too large");
                            goto err_exit;
                        }
                        if(pkey->listFetch_data_len <
                           (size_t)(pkey->listFetch_s - pkey->listFetch_data) +
                           list[keys].attrs[i].value_len) {
                            ssh2_err(session, LIBSSH2_ERROR_BUFFER_TOO_SMALL,
                                     "ListFetch data too short");
                            goto err_exit;
                        }
                        list[keys].attrs[i].value = (char *)pkey->listFetch_s;
                        pkey->listFetch_s += list[keys].attrs[i].value_len;

                        /* actually an ignored value */
                        list[keys].attrs[i].mandatory = 0;
                    }
                }
                else
                    list[keys].attrs = NULL;
            }
            /* To be FREEd in libssh2_publickey_list_free() */
            list[keys].packet = pkey->listFetch_data;
            pkey->listFetch_data = NULL;

            keys++;
            break;
        default:
            /* Unknown/Unexpected */
            ssh2_err(session, LIBSSH2_ERROR_PUBLICKEY_PROTOCOL,
                     "Unexpected publickey subsystem response");
            SSH2_SAFEFREE(session, pkey->listFetch_data);
        }
    }

    /* Only reached via explicit goto */
err_exit:
    if(pkey->listFetch_data)
        SSH2_SAFEFREE(session, pkey->listFetch_data);
    if(list)
        libssh2_publickey_list_free(pkey, list);
    pkey->listFetch_list = NULL;
    pkey->listFetch_keys = 0;
    pkey->listFetch_max_keys = 0;
    pkey->listFetch_state = ssh2_NB_state_idle;
    END();
}

/*
 * Free a previously fetched list of public keys
 */
void libssh2_publickey_list_free(LIBSSH2_PUBLICKEY *pkey,
                                 libssh2_publickey_list *pkey_list)
{
    LIBSSH2_SESSION *session;
    libssh2_publickey_list *p = pkey_list;

    if(!pkey || !p)
        return;

    session = pkey->channel->session;

    while(p->attrs || p->packet) {
        if(p->attrs)
            SSH2_FREE(session, p->attrs);
        if(p->packet)
            SSH2_FREE(session, p->packet);
        p++;
    }

    SSH2_FREE(session, pkey_list);
}

/*
 * Shutdown the publickey subsystem
 */
void libssh2_publickey_shutdown(LIBSSH2_PUBLICKEY *pkey)
{
    LIBSSH2_SESSION *session;
    struct corout_item *state;
    unsigned long i;

    if(!pkey)
        return;

    session = pkey->channel->session;
    state = session->corout_state;

    /*
     * Make sure all memory used in the state variables are free
     */
    if(pkey->receive_packet)
        SSH2_SAFEFREE(session, pkey->receive_packet);
    if(pkey->add_packet)
        SSH2_SAFEFREE(session, pkey->add_packet);
    if(pkey->remove_packet)
        SSH2_SAFEFREE(session, pkey->remove_packet);
    if(pkey->listFetch_data)
        SSH2_SAFEFREE(session, pkey->listFetch_data);
    if(pkey->listFetch_list) {
        for(i = 0; i < pkey->listFetch_keys; i++) {
            if(pkey->listFetch_list[i].attrs)
                SSH2_FREE(session, pkey->listFetch_list[i].attrs);
            if(pkey->listFetch_list[i].packet)
                SSH2_FREE(session, pkey->listFetch_list[i].packet);
        }
        SSH2_SAFEFREE(session, pkey->listFetch_list);
        pkey->listFetch_keys = 0;
        pkey->listFetch_max_keys = 0;
    }

    START();

    CALL(ssh2_channel_free(pkey->channel));

    SSH2_FREE(session, pkey);
    ssh2_err(session, LIBSSH2_ERROR_NONE, "No error");
    END();
}
