/* Copyright (C) Daniel Stenberg
 * Copyright (C) Sara Golemon <sarag@libssh2.org>
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

#include "channel.h"
#include "session.h"
#include "corout.h"

/* Max. length of a quoted string after scp_shell_quotearg() processing */
#define shell_quotedsize(s)  (3 * strlen(s) + 2)

/* Cap on basename bytes drained after the fixed response buffer is full */
#define SCP_C_NAME_DRAIN_MAX  (64 * 1024)

/* ssh2_scp_parse_c_fields() results */
#define SCP_C_FIELDS_OK          0
#define SCP_C_FIELDS_INCOMPLETE  1
#define SCP_C_FIELDS_MALFORMED   (-1)

/*
 * Parse mode and size from an SCP "C" response line fragment.
 *
 * Format: C<octal-mode> <decimal-size> <name>\n
 *
 * The basename (and trailing newline) need not be present yet. Once the size
 * field is terminated by a space or end-of-line character, mode and size are
 * considered complete. libssh2 does not use the basename.
 *
 * Returns SCP_C_FIELDS_OK, SCP_C_FIELDS_INCOMPLETE, or SCP_C_FIELDS_MALFORMED.
 */
int ssh2_scp_parse_c_fields(const char *buf, const size_t len,
                            long *mode_out, libssh2_int64_t *size_out)
{
    size_t i;
    size_t mode_start, mode_len, size_start, size_len;
    char tmp[32];
    const char *end = NULL;
    libssh2_int64_t num;

    if(!buf || len < 1 || buf[0] != 'C')
        return SCP_C_FIELDS_MALFORMED;

    i = 1;
    mode_start = i;
    while(i < len && buf[i] >= '0' && buf[i] <= '7')
        i++;
    mode_len = i - mode_start;
    if(!mode_len)
        return (i >= len) ? SCP_C_FIELDS_INCOMPLETE : SCP_C_FIELDS_MALFORMED;
    if(i >= len)
        return SCP_C_FIELDS_INCOMPLETE;
    if(buf[i] != ' ')
        return SCP_C_FIELDS_MALFORMED;
    i++; /* skip space after mode */

    size_start = i;
    while(i < len && buf[i] >= '0' && buf[i] <= '9')
        i++;
    size_len = i - size_start;
    if(!size_len)
        return (i >= len) ? SCP_C_FIELDS_INCOMPLETE : SCP_C_FIELDS_MALFORMED;
    if(i >= len)
        return SCP_C_FIELDS_INCOMPLETE; /* size digits may still continue */
    if(buf[i] != ' ' && buf[i] != '\r' && buf[i] != '\n')
        return SCP_C_FIELDS_MALFORMED;

    if(mode_len >= sizeof(tmp) || size_len >= sizeof(tmp))
        return SCP_C_FIELDS_MALFORMED;

    memcpy(tmp, buf + mode_start, mode_len);
    tmp[mode_len] = '\0';

    end = tmp;
    if(ssh2_str_number(&end, &num, LONG_MAX, 8) || *end)
        return SCP_C_FIELDS_MALFORMED;
    *mode_out = (long)num;

    memcpy(tmp, buf + size_start, size_len);
    tmp[size_len] = '\0';
    end = tmp;
    if(ssh2_str_number(&end, size_out, INT64_MAX, 10) || *end)
        return SCP_C_FIELDS_MALFORMED;

    return SCP_C_FIELDS_OK;
}

/* This function quotes a string in a way suitable to be used with a
   shell, e.g. the filename
   one two
   becomes
   'one two'

   The resulting output string is crafted in a way that makes it usable
   with the two most common shell types: Bourne Shell derived shells
   (sh, ksh, ksh93, bash, zsh) and C-Shell derivates (csh, tcsh).

   The following special cases are handled:
   o  If the string contains an apostrophy itself, the apostrophy
   character is written in quotation marks, e.g. "'".
   The shell cannot handle the syntax 'doesn\'t', so we close the
   current argument word, add the apostrophe in quotation marks "",
   and open a new argument word instead (_ indicate the input
   string characters):
    _____   _   _
   'doesn' "'" 't'

   Sequences of apostrophes are combined in one pair of quotation marks:
   a'''b
   becomes
    _  ___  _
   'a'"'''"'b'

   o  If the string contains an exclamation mark (!), the C-Shell
   interprets it as an event number. Using \! (not within quotation
   marks or single quotation marks) is a mechanism understood by
   both Bourne Shell and C-Shell.

   If a quotation was already started, the argument word is closed
   first:
   a!b

   become
    _  _ _
   'a'\!'b'

   The result buffer must be large enough for the expanded result. A
   bad case regarding expansion is alternating characters and
   apostrophes:

   a'b'c'd'                   (length 8) gets converted to
   'a'"'"'b'"'"'c'"'"'d'"'"   (length 24)

   This is the worst case.

   Maximum length of the result:
   1 + 6 * (length(input) + 1) / 2) + 1

   => 3 * length(input) + 2

   Explanation:
   o  leading apostrophe
   o  one character / apostrophe pair (two characters) can get
   represented as 6 characters: a' -> a'"'"'
   o  String terminator (+1)

   A result buffer three times the size of the input buffer + 2
   characters should be safe.

   References:
   o  csh-compatible quotation (special handling for '!' etc.), see
   https://www.grymoire.com/Unix/Csh.html#toc-uh-10

   Return value:
   Length of the resulting string (not counting the null-terminator),
   or 0 in case of errors, e.g. result buffer too small

   Note: this function could possible be used elsewhere within libssh2, but
   until then it is kept static and in this source file.
 */
static size_t scp_shell_quotearg(const char *path,
                                 char *buf, const size_t bufsize)
{
    const char *src;
    char *dst, *endp;

    /*
     * Processing States:
     *  UQSTRING:       unquoted string: ... -- used for quoting exclamation
     *                  marks. This is the initial state
     *  SQSTRING:       single-quoted-string: '... -- any character may follow
     *  QSTRING:        quoted string: "... -- only apostrophes may follow
     */
    enum {
        UQSTRING,
        SQSTRING,
        QSTRING
    } state = UQSTRING;

    endp = &buf[bufsize];
    src = path;
    dst = buf;
    while(*src && dst < endp - 1) {

        switch(*src) {
            /*
             * Special handling for apostrophe.
             * An apostrophe is always written in quotation marks, e.g.
             * ' -> "'".
             */

        case '\'':
            switch(state) {
            case UQSTRING:      /* Unquoted string */
                if(dst + 1 >= endp)
                    return 0;
                *dst++ = '"';
                break;
            case QSTRING:       /* Continue quoted string */
                break;
            case SQSTRING:      /* Close single quoted string */
                if(dst + 2 >= endp)
                    return 0;
                *dst++ = '\'';
                *dst++ = '"';
                break;
            default:
                break;
            }
            state = QSTRING;
            break;

            /*
             * Special handling for exclamation marks. CSH interprets
             * exclamation marks even when quoted with apostrophes. We convert
             * it to the plain string \!, because both Bourne Shell and CSH
             * interpret that as a verbatim exclamation mark.
             */

        case '!':
            switch(state) {
            case UQSTRING:
                if(dst + 1 >= endp)
                    return 0;
                *dst++ = '\\';
                break;
            case QSTRING:
                if(dst + 2 >= endp)
                    return 0;
                *dst++ = '"';           /* Closing quotation mark */
                *dst++ = '\\';
                break;
            case SQSTRING:              /* Close single quoted string */
                if(dst + 2 >= endp)
                    return 0;
                *dst++ = '\'';
                *dst++ = '\\';
                break;
            default:
                break;
            }
            state = UQSTRING;
            break;

            /*
             * Ordinary character: prefer single-quoted string
             */

        default:
            switch(state) {
            case UQSTRING:
                if(dst + 1 >= endp)
                    return 0;
                *dst++ = '\'';
                break;
            case QSTRING:
                if(dst + 2 >= endp)
                    return 0;
                *dst++ = '"';           /* Closing quotation mark */
                *dst++ = '\'';
                break;
            case SQSTRING:      /* Continue single quoted string */
                break;
            default:
                break;
            }
            state = SQSTRING;   /* Start single-quoted string */
            break;
        }

        if(dst + 1 >= endp)
            return 0;
        *dst++ = *src++;
    }

    switch(state) {
    case UQSTRING:
        break;
    case QSTRING:           /* Close quoted string */
        if(dst + 1 >= endp)
            return 0;
        *dst++ = '"';
        break;
    case SQSTRING:          /* Close single quoted string */
        if(dst + 1 >= endp)
            return 0;
        *dst++ = '\'';
        break;
    default:
        break;
    }

    if(dst + 1 >= endp)
        return 0;
    *dst = '\0';

    /* The result cannot be larger than 3 * strlen(path) + 2 */
    /* assert((dst - buf) <= (3 * (src - path) + 2)); */

    return dst - buf;
}

/*
 * Open a channel and request a remote file via SCP
 */
static void scp_recv(LIBSSH2_SESSION *session,
                     const char *path, libssh2_struct_stat *sb)
{
    struct corout_item *state = session->corout_state;
    size_t cmd_len;

    if(!path) {
        ssh2_err(session, LIBSSH2_ERROR_INVAL,
                 "Path argument can not be null");
        session->scpRecv_channel = NULL;
        return;
    }

    START();

    session->scpRecv_mode = 0;
    session->scpRecv_size = 0;
    session->scpRecv_mtime = 0;
    session->scpRecv_atime = 0;

    session->scpRecv_command_len =
        shell_quotedsize(path) + sizeof("scp -f ") + (sb ? 1 : 0);

    session->scpRecv_command =
        SSH2_ALLOC(session, session->scpRecv_command_len);

    if(!session->scpRecv_command) {
        ssh2_err(session, LIBSSH2_ERROR_ALLOC,
                 "Unable to allocate a command buffer for SCP session");
        session->scpRecv_channel = NULL;
        return;
    }

    ssh2_snprintf(session->scpRecv_command,
                  session->scpRecv_command_len,
                  "scp -%sf ", sb ? "p" : "");

    cmd_len = strlen(session->scpRecv_command);

    if(!session->flag.quote_paths) {
        size_t path_len;

        path_len = strlen(path);

        /* no null-termination needed, so use memcpy */
        memcpy(&session->scpRecv_command[cmd_len], path, path_len);
        cmd_len += path_len;
    }
    else
        cmd_len += scp_shell_quotearg(path,
                                      &session->scpRecv_command[cmd_len],
                                      session->scpRecv_command_len -
                                          cmd_len);

    /* the command to exec should _not_ be null-terminated */
    session->scpRecv_command_len = cmd_len;

    ssh2_deb((session, LIBSSH2_TRACE_SCP,
              "Opening channel for SCP receive"));

    CALL(ssh2_channel_open(session, "session", sizeof("session") - 1,
                           LIBSSH2_CHANNEL_WINDOW_DEFAULT,
                           LIBSSH2_CHANNEL_PACKET_DEFAULT, NULL, 0));
    session->scpRecv_channel = session->open_channel;

    CALL(ssh2_channel_process_startup(session->scpRecv_channel,
                                      "exec", sizeof("exec") - 1,
                                      session->scpRecv_command,
                                      session->scpRecv_command_len));
    SSH2_SAFEFREE(session, session->scpRecv_command);

    ssh2_deb((session, LIBSSH2_TRACE_SCP, "Sending initial wakeup"));
    /* SCP ACK */
    session->scpRecv_response[0] = '\0';

    CALL(ssh2_channel_write(session->scpRecv_channel, 0,
                            session->scpRecv_response, 1));
    if(session->scpRecv_channel->write_bytes != 1)
        goto scp_recv_error;

    /* Parse SCP response */
    session->scpRecv_response_len = 0;

    if(sb) {
        while(session->scpRecv_response_len < SSH2_SCP_RESPONSE_BUFLEN) {
            unsigned char *s, *p;
            const char *end;
            libssh2_int64_t num;

            CALL(ssh2_channel_read(session->scpRecv_channel, 0,
                                   (char *)session->scpRecv_response +
                                   session->scpRecv_response_len, 1));
            if(session->scpRecv_channel->read_bytes == 0) {
                ssh2_err(session, LIBSSH2_ERROR_SCP_PROTOCOL,
                         "Unexpected channel close");
                goto scp_recv_error;
            }

            session->scpRecv_response_len++;

            if(session->scpRecv_response[0] != 'T') {
                /* there can be
                   01 for warnings
                   02 for errors

                   The following string MUST be newline terminated
                 */
                session->scpRecv_err_len =
                    ssh2_channel_packet_data_len(session->scpRecv_channel,
                                                 0);
                session->scpRecv_err_msg =
                    SSH2_ALLOC(session, session->scpRecv_err_len + 1);
                if(!session->scpRecv_err_msg) {
                    ssh2_err(session, LIBSSH2_ERROR_ALLOC,
                             "Failed to get memory ");
                    goto scp_recv_error;
                }

                /* Read the remote error message */
                CALL(ssh2_channel_read(session->scpRecv_channel, 0,
                                       session->scpRecv_err_msg,
                                       session->scpRecv_err_len));
                if(session->scpRecv_channel->read_bytes > 0) {
                    session->scpRecv_err_msg[
                        session->scpRecv_channel->read_bytes] = '\0';
                    ssh2_deb((session, LIBSSH2_TRACE_SCP, "got %02x %s",
                              session->scpRecv_response[0],
                              session->scpRecv_err_msg));
                }
                SSH2_FREE(session, session->scpRecv_err_msg);
                ssh2_err(session, LIBSSH2_ERROR_SCP_PROTOCOL,
                         "Failed to recv file");
                goto scp_recv_error;
            }

            if(session->scpRecv_response_len > 1 &&
               (session->scpRecv_response[session->scpRecv_response_len -
                                          1] < '0' ||
                session->scpRecv_response[session->scpRecv_response_len -
                                          1] > '9') &&
               session->scpRecv_response[session->scpRecv_response_len -
                                         1] != ' ' &&
               session->scpRecv_response[session->scpRecv_response_len -
                                         1] != '\r' &&
               session->scpRecv_response[session->scpRecv_response_len -
                                         1] != '\n') {
                ssh2_err(session, LIBSSH2_ERROR_SCP_PROTOCOL,
                         "Invalid data in SCP response");
                goto scp_recv_error;
            }

            if(session->scpRecv_response_len < 9 ||
               session->scpRecv_response[session->scpRecv_response_len -
                                         1] != '\n') {
                if(session->scpRecv_response_len == SSH2_SCP_RESPONSE_BUFLEN) {
                    /* You had your chance */
                    ssh2_err(session, LIBSSH2_ERROR_SCP_PROTOCOL,
                             "Unterminated response from SCP server");
                    goto scp_recv_error;
                }
                /* Way too short to be an SCP response, or not done yet,
                   short circuit */
                continue;
            }

            /* We are guaranteed not to go under response_len == 0 by the
               logic above */
            while(
                (session->scpRecv_response[session->scpRecv_response_len -
                                           1] == '\r') ||
                (session->scpRecv_response[session->scpRecv_response_len -
                                           1] == '\n'))
                session->scpRecv_response_len--;
            session->scpRecv_response[session->scpRecv_response_len] = '\0';

            if(session->scpRecv_response_len < 8) {
                /* EOL came too soon */
                ssh2_err(session, LIBSSH2_ERROR_SCP_PROTOCOL,
                         "Invalid response from SCP server, too short");
                goto scp_recv_error;
            }

            s = session->scpRecv_response + 1;

            p = (unsigned char *)strchr((char *)s, ' ');
            if(!p || (p - s) <= 0) {
                /* No spaces or space in the wrong spot */
                ssh2_err(session, LIBSSH2_ERROR_SCP_PROTOCOL,
                         "Invalid response from SCP server, "
                         "malformed mtime");
                goto scp_recv_error;
            }
            *(p++) = '\0';

            end = (const char *)s;
            (void)ssh2_str_number(&end, &num, INT64_MAX, 10);
            session->scpRecv_mtime = (time_t)num;

            s = (unsigned char *)strchr((char *)p, ' ');
            if(!s || (s - p) <= 0) {
                /* No spaces or space in the wrong spot */
                ssh2_err(session, LIBSSH2_ERROR_SCP_PROTOCOL,
                         "Invalid response from SCP server, "
                         "malformed mtime.usec");
                goto scp_recv_error;
            }

            /* Ignore mtime.usec */
            s++;
            p = (unsigned char *)strchr((char *)s, ' ');
            if(!p || (p - s) <= 0) {
                /* No spaces or space in the wrong spot */
                ssh2_err(session, LIBSSH2_ERROR_SCP_PROTOCOL,
                         "Invalid response from SCP server, "
                         "too short or malformed");
                goto scp_recv_error;
            }
            *p = '\0';

            end = (const char *)s;
            (void)ssh2_str_number(&end, &num, INT64_MAX, 10);
            session->scpRecv_atime = (time_t)num;

            /* SCP ACK */
            session->scpRecv_response[0] = '\0';
            break;
        }

        CALL(ssh2_channel_write(session->scpRecv_channel, 0,
                                session->scpRecv_response, 1));
        if(session->scpRecv_channel->write_bytes != 1)
            goto scp_recv_error;

        ssh2_deb((session, LIBSSH2_TRACE_SCP,
                  "mtime = %" SSH2_INT64_T_FORMAT ", "
                  "atime = %" SSH2_INT64_T_FORMAT,
                  (libssh2_int64_t)session->scpRecv_mtime,
                  (libssh2_int64_t)session->scpRecv_atime));
    }

    session->scpRecv_response_len = 0;

    /* Read the "C" response line: mode, size, basename */
    while(session->scpRecv_response_len < SSH2_SCP_RESPONSE_BUFLEN) {
        unsigned char last;

        CALL(ssh2_channel_read(session->scpRecv_channel, 0,
                               (char *)session->scpRecv_response +
                               session->scpRecv_response_len, 1));
        if(session->scpRecv_channel->read_bytes == 0) {
            ssh2_err(session, LIBSSH2_ERROR_SCP_PROTOCOL,
                     "Unexpected channel close");
            goto scp_recv_error;
        }

        session->scpRecv_response_len++;
        last = session->scpRecv_response[session->scpRecv_response_len - 1];

        if(session->scpRecv_response[0] != 'C') {
            ssh2_err(session, LIBSSH2_ERROR_SCP_PROTOCOL,
                     "Invalid response from SCP server");
            goto scp_recv_error;
        }

        if(session->scpRecv_response_len > 1 &&
           last != '\r' && last != '\n' && last < 32) {
            ssh2_err(session, LIBSSH2_ERROR_SCP_PROTOCOL,
                     "Invalid data in SCP response");
            goto scp_recv_error;
        }

        if(last == '\n') {
            long mode = 0;
            libssh2_int64_t size = 0;
            int prc;

            /* Complete line in the fixed buffer (common case). */
            prc = ssh2_scp_parse_c_fields(
                (const char *)session->scpRecv_response,
                session->scpRecv_response_len, &mode, &size);
            if(prc != SCP_C_FIELDS_OK) {
                ssh2_err(session, LIBSSH2_ERROR_SCP_PROTOCOL,
                         "Invalid response from SCP server");
                goto scp_recv_error;
            }
            session->scpRecv_mode = mode;
            session->scpRecv_size = size;
            /* SCP ACK */
            session->scpRecv_response[0] = '\0';
            goto scp_recv_send_ack;
        }

        if(session->scpRecv_response_len == SSH2_SCP_RESPONSE_BUFLEN) {
            long mode = 0;
            libssh2_int64_t size = 0;
            int prc;

            /*
             * Fixed buffer is full without a newline. Mode and size always
             * fit early; a long basename overflows the buffer. Parse what
             * we have and drain the rest of the name until newline
             * (basename is unused by libssh2).
             */
            prc = ssh2_scp_parse_c_fields(
                (const char *)session->scpRecv_response,
                session->scpRecv_response_len, &mode, &size);
            if(prc == SCP_C_FIELDS_OK) {
                session->scpRecv_mode = mode;
                session->scpRecv_size = size;
                /* Reuse response_len as drained-byte counter */
                session->scpRecv_response_len = 0;
                goto scp_recv_drain;
            }
            if(prc == SCP_C_FIELDS_MALFORMED)
                ssh2_err(session, LIBSSH2_ERROR_SCP_PROTOCOL,
                         "Invalid response from SCP server");
            else
                ssh2_err(session, LIBSSH2_ERROR_SCP_PROTOCOL,
                         "Unterminated response from SCP server");
            goto scp_recv_error;
        }
    }

    /* Not reached: the loop always exits via one of the gotos above. */
    ssh2_err(session, LIBSSH2_ERROR_SCP_PROTOCOL,
             "Unterminated response from SCP server");
    goto scp_recv_error;

    /* Drain remaining basename after the fixed buffer filled. */
scp_recv_drain:
    for(;;) {
        unsigned char discard;

        CALL(ssh2_channel_read(session->scpRecv_channel, 0,
                               (char *)&discard, 1));
        if(session->scpRecv_channel->read_bytes == 0) {
            ssh2_err(session, LIBSSH2_ERROR_SCP_PROTOCOL,
                     "Unexpected channel close");
            goto scp_recv_error;
        }

        if(discard == '\n') {
            session->scpRecv_response[0] = '\0';
            break;
        }
        if(discard == '\r')
            continue;
        if(discard < 32) {
            ssh2_err(session, LIBSSH2_ERROR_SCP_PROTOCOL,
                     "Invalid data in SCP response");
            goto scp_recv_error;
        }
        session->scpRecv_response_len++;
        if(session->scpRecv_response_len > SCP_C_NAME_DRAIN_MAX) {
            ssh2_err(session, LIBSSH2_ERROR_SCP_PROTOCOL,
                     "SCP response name too long");
            goto scp_recv_error;
        }
    }

scp_recv_send_ack:
    CALL(ssh2_channel_write(session->scpRecv_channel, 0,
                            session->scpRecv_response, 1));
    if(session->scpRecv_channel->write_bytes != 1)
        goto scp_recv_error;

    ssh2_deb((session, LIBSSH2_TRACE_SCP, "mode = 0%lo size = %ld",
              (unsigned long)session->scpRecv_mode,
              (long)session->scpRecv_size));

    if(sb) {
        memset(sb, 0, sizeof(libssh2_struct_stat));

        sb->st_mtime = session->scpRecv_mtime;
        sb->st_atime = session->scpRecv_atime;
        sb->st_size = (libssh2_struct_stat_size)session->scpRecv_size;
        sb->st_mode = (unsigned short)session->scpRecv_mode;
    }

    return;

scp_recv_error:
    if(session->scpRecv_channel)
        CALL(ssh2_channel_free(session->scpRecv_channel));
    session->scpRecv_channel = NULL;

    END();
}

#ifndef LIBSSH2_NO_DEPRECATED
/*
 * DEPRECATED, DO NOT USE!
 *
 * Open a channel and request a remote file via SCP.  This receives files
 * larger than 2 GB, but is unable to report the proper size on platforms
 * where the st_size member of struct stat is limited to 2 GB (e.g. windows).
 */
void libssh2_scp_recv(LIBSSH2_SESSION *session, const char *path,
                      struct stat *sb)
{
    struct corout_item *state;

    /* scp_recv uses libssh2_struct_stat, so pass one if the caller gave us a
       struct to populate... */
    libssh2_struct_stat sb_intl;
    libssh2_struct_stat *sb_ptr;

    if(!session)
        return;

    memset(&sb_intl, 0, sizeof(sb_intl));
    sb_ptr = sb ? &sb_intl : NULL;

    state = session->corout_state;
    START();
    CALL(scp_recv(session, path, sb_ptr));
    END();

    /* ...and populate the caller's with as much info as fits. */
    if(sb) {
        memset(sb, 0, sizeof(struct stat));

        sb->st_mtime = sb_intl.st_mtime;
        sb->st_atime = sb_intl.st_atime;
        /* NOLINTNEXTLINE(readability-redundant-casting) */
        sb->st_size = (off_t)sb_intl.st_size;
        sb->st_mode = sb_intl.st_mode;
    }
}
#endif

/*
 * Open a channel and request a remote file via SCP.  This supports files > 2GB
 * on platforms that support it.
 */
void libssh2_scp_recv2(LIBSSH2_SESSION *session, const char *path,
                       libssh2_struct_stat *sb)
{
    struct corout_item *state;

    if(!session)
        return;

    state = session->corout_state;
    START();
    CALL(scp_recv(session, path, sb));
    END();
}

/*
 * Send a file using SCP
 */
static void scp_send(LIBSSH2_SESSION *session,
                     const char *path, const int mode,
                     const libssh2_int64_t size,
                     const time_t mtime, const time_t atime)
{
    struct corout_item *state = session->corout_state;
    size_t cmd_len;

    if(!path) {
        ssh2_err(session, LIBSSH2_ERROR_INVAL,
                 "Path argument can not be null");
        session->scpSend_channel = NULL;
        return;
    }

    START();

    session->scpSend_command_len =
        shell_quotedsize(path) + sizeof("scp -t ") +
        ((mtime || atime) ? 1 : 0);

    session->scpSend_command =
        SSH2_ALLOC(session, session->scpSend_command_len);

    if(!session->scpSend_command) {
        ssh2_err(session, LIBSSH2_ERROR_ALLOC,
                 "Unable to allocate a command buffer for SCP session");
        session->scpSend_channel = NULL;
        return;
    }

    ssh2_snprintf(session->scpSend_command,
                  session->scpSend_command_len,
                  "scp -%st ", (mtime || atime) ? "p" : "");

    cmd_len = strlen(session->scpSend_command);

    if(!session->flag.quote_paths) {
        size_t path_len;

        path_len = strlen(path);

        /* no null-termination needed, so use memcpy */
        memcpy(&session->scpSend_command[cmd_len], path, path_len);
        cmd_len += path_len;
    }
    else
        cmd_len += scp_shell_quotearg(path,
                                      &session->scpSend_command[cmd_len],
                                      session->scpSend_command_len -
                                          cmd_len);

    /* the command to exec should _not_ be null-terminated */
    session->scpSend_command_len = cmd_len;

    ssh2_deb((session, LIBSSH2_TRACE_SCP, "Opening channel for SCP send"));

    CALL(ssh2_channel_open(session, "session", sizeof("session") - 1,
                           LIBSSH2_CHANNEL_WINDOW_DEFAULT,
                           LIBSSH2_CHANNEL_PACKET_DEFAULT, NULL, 0));
    session->scpSend_channel = session->open_channel;

    CALL(ssh2_channel_process_startup(session->scpSend_channel,
                                      "exec", sizeof("exec") - 1,
                                      session->scpSend_command,
                                      session->scpSend_command_len));
    SSH2_SAFEFREE(session, session->scpSend_command);

    /* Wait for ACK */
    CALL(ssh2_channel_read(session->scpSend_channel, 0,
                           (char *)session->scpSend_response, 1));
    if(session->scpSend_channel->read_bytes == 0) {
        ssh2_err(session, LIBSSH2_ERROR_SCP_PROTOCOL,
                 "Unexpected channel close");
        goto scp_send_error;
    }
    else if(session->scpSend_response[0]) {
        ssh2_err(session, LIBSSH2_ERROR_SCP_PROTOCOL,
                 "Invalid ACK response from remote");
        goto scp_send_error;
    }

    if(mtime || atime) {
        /* Send mtime and atime to be used for file */
        session->scpSend_response_len =
            ssh2_snprintf((char *)session->scpSend_response,
                          SSH2_SCP_RESPONSE_BUFLEN, "T%ld 0 %ld 0\n",
                          (long)mtime, (long)atime);
        ssh2_deb((session, LIBSSH2_TRACE_SCP, "Sent %s",
                  session->scpSend_response));

        CALL(ssh2_channel_write(session->scpSend_channel, 0,
                                session->scpSend_response,
                                session->scpSend_response_len));
        if(session->scpSend_channel->write_bytes !=
           (int)session->scpSend_response_len) {
            ssh2_err(session, LIBSSH2_ERROR_SOCKET_SEND,
                     "Unable to send time data for SCP file");
            goto scp_send_error;
        }

        /* Wait for ACK */
        CALL(ssh2_channel_read(session->scpSend_channel, 0,
                               (char *)session->scpSend_response, 1));
        if(session->scpSend_channel->read_bytes == 0) {
            ssh2_err(session, LIBSSH2_ERROR_SCP_PROTOCOL,
                     "Unexpected channel close");
            goto scp_send_error;
        }
        else if(session->scpSend_response[0]) {
            ssh2_err(session, LIBSSH2_ERROR_SCP_PROTOCOL,
                     "Invalid SCP ACK response");
            goto scp_send_error;
        }
    }

    /* Send mode, size, and basename */
    {
        int len;
        const char *base = strrchr(path, '/');
        if(base)
            base++;
        else
            base = path;

        len = ssh2_snprintf((char *)session->scpSend_response,
                            sizeof(session->scpSend_response),
                            "C0%o %" SSH2_INT64_T_FORMAT " %s\n",
                            (unsigned int)mode, size, base);
        if(len < 0 || (size_t)len >= sizeof(session->scpSend_response)) {
            ssh2_err(session, LIBSSH2_ERROR_SCP_PROTOCOL,
                     "SCP file path too long for response buffer");
            goto scp_send_error;
        }
        session->scpSend_response_len = (size_t)len;
        ssh2_deb((session, LIBSSH2_TRACE_SCP, "Sent %s",
                  session->scpSend_response));
    }

    CALL(ssh2_channel_write(session->scpSend_channel, 0,
                            session->scpSend_response,
                            session->scpSend_response_len));
    if(session->scpSend_channel->write_bytes !=
       (int)session->scpSend_response_len) {
        ssh2_err(session, LIBSSH2_ERROR_SOCKET_SEND,
                 "Unable to send core file data for SCP file");
        goto scp_send_error;
    }

    /* Wait for ACK */
    CALL(ssh2_channel_read(session->scpSend_channel, 0,
                           (char *)session->scpSend_response, 1));
    if(session->scpSend_channel->read_bytes == 0) {
        ssh2_err(session, LIBSSH2_ERROR_SCP_PROTOCOL,
                 "Unexpected channel close");
        goto scp_send_error;
    }

    if(session->scpSend_response[0]) {
        session->scpSend_err_len =
            ssh2_channel_packet_data_len(session->scpSend_channel, 0);
        session->scpSend_err_msg =
            SSH2_ALLOC(session, session->scpSend_err_len + 1);
        if(!session->scpSend_err_msg) {
            ssh2_err(session, LIBSSH2_ERROR_ALLOC, "failed to get memory");
            goto scp_send_error;
        }

        /* Read the remote error message */
        CALL(ssh2_channel_read(session->scpSend_channel, 0,
                               session->scpSend_err_msg,
                               session->scpSend_err_len));
        if(session->scpSend_channel->read_bytes > 0) {
            session->scpSend_err_msg[
                session->scpSend_channel->read_bytes] = '\0';
            ssh2_deb((session, LIBSSH2_TRACE_SCP, "got %02x %s",
                      session->scpSend_response[0], session->scpSend_err_msg));
        }
        SSH2_FREE(session, session->scpSend_err_msg);
        ssh2_err(session, LIBSSH2_ERROR_SCP_PROTOCOL, "failed to send file");
        goto scp_send_error;
    }

    return;

scp_send_error:
    if(session->scpSend_channel)
        CALL(ssh2_channel_free(session->scpSend_channel));
    session->scpSend_channel = NULL;

    END();
}

#ifndef LIBSSH2_NO_DEPRECATED
/*
 * DEPRECATED, DO NOT USE!
 *
 * Send a file using SCP. Old API.
 */
void libssh2_scp_send_ex(LIBSSH2_SESSION *session,
                         const char *path, const int mode,
                         const size_t size,
                         const long mtime, const long atime)
{
    struct corout_item *state;

    if(!session)
        return;

    state = session->corout_state;
    START();
    CALL(scp_send(session, path, mode, size,
                  (time_t)mtime, (time_t)atime));
    END();
}
#endif

/*
 * Send a file using SCP
 */
void libssh2_scp_send64(LIBSSH2_SESSION *session,
                        const char *path, const int mode,
                        const libssh2_int64_t size,
                        const time_t mtime, const time_t atime)
{
    struct corout_item *state;

    if(!session)
        return;

    state = session->corout_state;
    START();
    CALL(scp_send(session, path, mode, size, mtime, atime));
    END();
}
