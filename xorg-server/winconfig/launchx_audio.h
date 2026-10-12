#ifndef LAUNCHX_AUDIO_H
#define LAUNCHX_AUDIO_H

#include "launchx_common.h"

struct options_audio {
    int audio_enabled;
    char pulseport[16];
    int mic_enabled;
    int speaker_enabled;

    int listen_sel;          /* 0 = loopback, 1 = all IPv4, 2 = IPv4 + IPv6 */
    int auth_sel;            /* 0 = cookie, 1 = anonymous, 2 = IP allow-list */
    char auth_acl[256];      /* semicolon-separated IPs for auth_sel == 2 */

    char output_device[128]; /* "" = system default (WAVE_MAPPER) */
    char input_device[128];

    int rate_sel;            /* 0 = auto, else index into audio_rate_values */
    int format_sel;          /* 0 = auto, else index into audio_format_values */
    int channels_sel;        /* 0 = auto, else index into audio_channels_values */
    int latency_sel;         /* 0 = default, else fragments/fragment_size preset */

    int null_sink_enabled;
    int loopback_enabled;
};

static const char *audio_listen_items[] = {
    "Loopback only (127.0.0.1)",
    "All interfaces (0.0.0.0)",
    "All interfaces (IPv4 + IPv6)"
};
#define AUDIO_LISTEN_COUNT (sizeof(audio_listen_items) / sizeof(audio_listen_items[0]))

static const char *audio_auth_items[] = {
    "Cookie (shared pulse-cookie)",
    "Anonymous",
    "IP allow-list"
};
#define AUDIO_AUTH_COUNT (sizeof(audio_auth_items) / sizeof(audio_auth_items[0]))

static const char *audio_rate_items[] = {
    "Auto (44100 Hz)", "44100 Hz", "48000 Hz", "96000 Hz", "192000 Hz"
};
#define AUDIO_RATE_COUNT (sizeof(audio_rate_items) / sizeof(audio_rate_items[0]))

static const char *audio_format_items[] = {
    "Auto (s16le)", "s16le (16-bit)", "s24le (24-bit)", "s32le (32-bit)"
};
#define AUDIO_FORMAT_COUNT (sizeof(audio_format_items) / sizeof(audio_format_items[0]))

static const char *audio_channels_items[] = {
    "Auto (stereo)", "Mono (1)", "Stereo (2)"
};
#define AUDIO_CHANNELS_COUNT (sizeof(audio_channels_items) / sizeof(audio_channels_items[0]))

static const char *audio_latency_items[] = {
    "Default", "Low", "Medium", "High"
};
#define AUDIO_LATENCY_COUNT (sizeof(audio_latency_items) / sizeof(audio_latency_items[0]))

void tab_audio(struct nk_context *ctx, struct options_audio *opt);
void enumerate_audio_devices(void);
void write_default_pa(struct options_audio *a);
void ensure_pulse_cookie(void);
int read_pulse_cookie(unsigned char out[256]);

#endif /* LAUNCHX_AUDIO_H */
