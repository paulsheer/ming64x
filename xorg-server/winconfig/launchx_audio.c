#include "launchx_audio.h"

#include <mmsystem.h>
#include <bcrypt.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>

static const int audio_rate_values[] = { 0, 44100, 48000, 96000, 192000 };

static const char *audio_format_values[] = { "", "s16le", "s24le", "s32le" };

static const int audio_channels_values[] = { 0, 1, 2 };

static const int audio_latency_fragments[] = { 0, 3, 5, 8 };
static const int audio_latency_fragment_size[] = { 0, 4096, 8192, 16384 };

#define MAX_AUDIO_DEVICES 16
static char audio_output_devices[MAX_AUDIO_DEVICES][MAXPNAMELEN];
static int audio_output_device_count;
static char audio_input_devices[MAX_AUDIO_DEVICES][MAXPNAMELEN];
static int audio_input_device_count;

void
enumerate_audio_devices(void)
{
    UINT i, n;

    audio_output_device_count = 0;
    n = waveOutGetNumDevs();
    if (n > MAX_AUDIO_DEVICES)
        n = MAX_AUDIO_DEVICES;
    for (i = 0; i < n; i++) {
        WAVEOUTCAPS caps;
        if (waveOutGetDevCaps(i, &caps, sizeof caps) == MMSYSERR_NOERROR) {
            strncpy(audio_output_devices[audio_output_device_count], caps.szPname, MAXPNAMELEN - 1);
            audio_output_devices[audio_output_device_count][MAXPNAMELEN - 1] = '\0';
            audio_output_device_count++;
        }
    }

    audio_input_device_count = 0;
    n = waveInGetNumDevs();
    if (n > MAX_AUDIO_DEVICES)
        n = MAX_AUDIO_DEVICES;
    for (i = 0; i < n; i++) {
        WAVEINCAPS caps;
        if (waveInGetDevCaps(i, &caps, sizeof caps) == MMSYSERR_NOERROR) {
            strncpy(audio_input_devices[audio_input_device_count], caps.szPname, MAXPNAMELEN - 1);
            audio_input_devices[audio_input_device_count][MAXPNAMELEN - 1] = '\0';
            audio_input_device_count++;
        }
    }
}

static void
device_option(struct nk_context *ctx, const char *label, char *device,
    char names[][MAXPNAMELEN], int nnames, const char *tooltip)
{
    const char *items[MAX_AUDIO_DEVICES + 1];
    int count = nnames + 1;
    int sel = 0;
    int i;

    items[0] = "Auto (system default)";
    for (i = 0; i < nnames; i++)
        items[i + 1] = names[i];
    for (i = 1; i < count; i++) {
        if (strcmp(device, items[i]) == 0) {
            sel = i;
            break;
        }
    }

    combobox_option(ctx, label, items, count, &sel, tooltip);

    if (sel <= 0)
        device[0] = '\0';
    else
        strncpy(device, items[sel], 127), device[127] = '\0';
}

/* Resolve the host's primary IPv4 address for the client env-var hint shown
   when listening on all interfaces. Cached: the dialog re-renders at 60 Hz. */
static int
local_ipv4(char *out, size_t outsize)
{
    static char cached[64];
    static int resolved;            /* 0 = not tried, 1 = ok, -1 = failed */
    int found = 0;

    if (resolved) {
        if (resolved < 0)
            return -1;
        snprintf(out, outsize, "%s", cached);
        return 0;
    }

    /* Preferred: ask the routing table which interface reaches the default
       gateway (0.0.0.0), then take that adapter's IPv4 unicast address. */
    {
        DWORD ifindex = 0;
        IP_ADAPTER_ADDRESSES *list = NULL, *a;

        if (GetBestInterface(0, &ifindex) == NO_ERROR) {
            ULONG buflen = 0;

            if (GetAdaptersAddresses(AF_INET, 0, NULL, NULL, &buflen) ==
                    ERROR_BUFFER_OVERFLOW &&
                (list = (IP_ADAPTER_ADDRESSES *) malloc(buflen)) != NULL) {
                if (GetAdaptersAddresses(AF_INET, 0, NULL, list, &buflen) ==
                        NO_ERROR) {
                    for (a = list; a; a = a->Next) {
                        IP_ADAPTER_UNICAST_ADDRESS *u;

                        if (a->IfIndex != ifindex)
                            continue;
                        for (u = a->FirstUnicastAddress; u; u = u->Next) {
                            struct sockaddr_in *sa;
                            unsigned long addr;

                            if (!u->Address.lpSockaddr ||
                                u->Address.lpSockaddr->sa_family != AF_INET)
                                continue;
                            sa = (struct sockaddr_in *) u->Address.lpSockaddr;
                            addr = ntohl(sa->sin_addr.s_addr);
                            if ((addr >> 24) == 127)          /* loopback */
                                continue;
                            if ((addr & 0xffff0000UL) == 0xa9fe0000UL) /* 169.254 */
                                continue;
                            if (inet_ntop(AF_INET, &sa->sin_addr,
                                          cached, sizeof(cached))) {
                                found = 1;
                                break;
                            }
                        }
                        if (found)
                            break;
                    }
                }
                free(list);
            }
        }
    }

    /* Fallback: resolve the hostname and take the first usable IPv4 address. */
    if (!found) {
        WSADATA wsa;
        char host[256];
        struct addrinfo hints, *res = NULL, *p;

        if (WSAStartup(MAKEWORD(2, 2), &wsa) == 0) {
            if (gethostname(host, sizeof(host)) == 0) {
                memset(&hints, 0, sizeof(hints));
                hints.ai_family = AF_INET;
                hints.ai_socktype = SOCK_STREAM;
                if (getaddrinfo(host, NULL, &hints, &res) == 0) {
                    for (p = res; p != NULL; p = p->ai_next) {
                        struct sockaddr_in *sa = (struct sockaddr_in *)p->ai_addr;
                        unsigned long a = ntohl(sa->sin_addr.s_addr);
                        if ((a >> 24) == 127)                   /* loopback */
                            continue;
                        if ((a & 0xffff0000UL) == 0xa9fe0000UL) /* link-local 169.254 */
                            continue;
                        if (inet_ntop(AF_INET, &sa->sin_addr,
                                      cached, sizeof(cached))) {
                            found = 1;
                            break;
                        }
                    }
                    freeaddrinfo(res);
                }
            }
        }
    }

    if (found) {
        resolved = 1;
        snprintf(out, outsize, "%s", cached);
        return 0;
    }
    resolved = -1;
    return -1;
}

void
tab_audio(struct nk_context *ctx, struct options_audio *opt)
{
    heading(ctx, "Audio");

    {
        char env[128];
        char listen[64] = "127.0.0.1";
        if (opt->listen_sel >= 1)
            local_ipv4(listen, sizeof(listen));
        snprintf(env, sizeof(env), "export PULSE_SERVER=tcp:%s:%s",
            listen, opt->pulseport[0] ? opt->pulseport : "4713");
        readonly_option(ctx, "Client environment variable", env,
            "On Linux, run this command to set the Ming64x as your audio device.");
    }

    {
        char dir[512] = "", path[512] = "";
        if (!config_dir_real(dir, sizeof dir))
            snprintf(path, sizeof path, "%s\\pulse-cookie", dir);
        readonly_option(ctx, "Data for PULSE_COOKIE file", path,
            "On Linux, run:  export PULSE_COOKIE=~/.ming64x-pulse-cookie\n"
            "Then copy the contents of this file to ~/.ming64x-pulse-cookie\n"
            "This sets up authentication, or set authentication to Anonymous");
    }

    checkbox_option(ctx, "Enable PulseAudio server", &opt->audio_enabled,
        "Start the embedded PulseAudio sound server when the X server launches.\nDisable with -noaudio.");

    if (!opt->audio_enabled)
        nk_widget_disable_begin(ctx);
    text_option(ctx, "PulseAudio listen port", opt->pulseport, sizeof(opt->pulseport),
        "TCP port the embedded PulseAudio server listens on (default 4713).");
    combobox_option(ctx, "Listen address", audio_listen_items, AUDIO_LISTEN_COUNT,
        &opt->listen_sel,
        "Bind the PulseAudio TCP server to 127.0.0.1 (loopback only, safest),\n0.0.0.0 (all IPv4 interfaces), or both IPv4 and IPv6. Loopback is\nsufficient when audio is forwarded over an SSH tunnel; choose an\nAll-interfaces option only for LAN clients.");
    combobox_option(ctx, "Authentication", audio_auth_items, AUDIO_AUTH_COUNT,
        &opt->auth_sel,
        "How remote clients authenticate. Cookie requires the shared\npulse-cookie (default); Anonymous accepts any client; IP allow-list\naccepts only the addresses listed below. Note that some client\napplications like FireFox do not support cookies.");
    if (opt->auth_sel == 2) {
        text_option(ctx, "Allowed clients", opt->auth_acl, sizeof(opt->auth_acl),
            "Semicolon-separated IP addresses or CIDR blocks allowed to connect when\nauthentication is set to IP allow-list (e.g. 192.168.1.0/24;10.0.0.5).");
    }
    checkbox_option(ctx, "Microphone", &opt->mic_enabled,
        "Expose the Windows recording device as the PulseAudio source (wavein).");
    checkbox_option(ctx, "Speaker", &opt->speaker_enabled,
        "Expose the Windows playback device as the PulseAudio sink (waveout).");
    device_option(ctx, "Output device", opt->output_device,
        audio_output_devices, audio_output_device_count,
        "Windows playback (waveOut) device to use. Auto selects the system default;\notherwise pick a specific device by name.");
    device_option(ctx, "Input device", opt->input_device,
        audio_input_devices, audio_input_device_count,
        "Windows recording (waveIn) device to use. Auto selects the system default;\notherwise pick a specific device by name.");
    combobox_option(ctx, "Sample rate", audio_rate_items, AUDIO_RATE_COUNT,
        &opt->rate_sel,
        "Sample rate for the waveOut/waveIn stream. Auto keeps PulseAudio's\ndefault (44100 Hz).");
    combobox_option(ctx, "Sample format", audio_format_items, AUDIO_FORMAT_COUNT,
        &opt->format_sel,
        "Bit depth for the waveOut/waveIn stream. Auto keeps PulseAudio's\ndefault (16-bit signed little-endian).");
    combobox_option(ctx, "Channels", audio_channels_items, AUDIO_CHANNELS_COUNT,
        &opt->channels_sel,
        "Channel count for the waveOut/waveIn stream. Auto keeps PulseAudio's\ndefault (stereo).");
    combobox_option(ctx, "Latency", audio_latency_items, AUDIO_LATENCY_COUNT,
        &opt->latency_sel,
        "Audio buffering. Lower latency reduces delay but risks underruns and\nglitches; higher latency is more robust. Maps to the waveOut fragment\ncount and size.");
    checkbox_option(ctx, "Null output", &opt->null_sink_enabled,
        "Load a null (silent) sink so PulseAudio always has a playback device,\neven when no real audio hardware is present.");
    checkbox_option(ctx, "Loopback (mic to speaker)", &opt->loopback_enabled,
        "Route the microphone straight to the speaker (module-loopback) for local\nmonitoring of the recording device.");
    if (!opt->audio_enabled)
        nk_widget_disable_end(ctx);
}

void
write_default_pa(struct options_audio *a)
{
    char dir[512], path[512], cookie[512], cookie_esc[512];
    FILE *f;
    const char *s;
    char *d;

    if (config_dir(dir, sizeof dir))
        return;
    CreateDirectoryA(dir, NULL);
    snprintf(path, sizeof path, "%s\\default.pa", dir);
    snprintf(cookie, sizeof cookie, "%s\\pulse-cookie", dir);

    /* load-module args are unescaped by pa_unescape, which drops every
       backslash, so a Windows path must be written with each one doubled. */
    d = cookie_esc;
    for (s = cookie; *s && d < cookie_esc + sizeof cookie_esc - 1; s++) {
        if (*s == '\\')
            *d++ = '\\';
        *d++ = *s;
    }
    *d = '\0';

    f = fopen(path, "wb");
    if (!f)
        return;
    if (a->speaker_enabled || a->mic_enabled) {
        fputs("load-module module-waveout", f);
        fputs(a->speaker_enabled ? " playback=1" : " playback=0", f);
        fputs(" sink_name=waveout", f);
        fputs(a->mic_enabled ? " record=1" : " record=0", f);
        fputs(" source_name=wavein", f);
        if (a->output_device[0])
            fprintf(f, " output_device_name=\"%s\"", a->output_device);
        if (a->input_device[0])
            fprintf(f, " input_device_name=\"%s\"", a->input_device);
        if (a->rate_sel > 0)
            fprintf(f, " rate=%d", audio_rate_values[a->rate_sel]);
        if (a->format_sel > 0)
            fprintf(f, " format=%s", audio_format_values[a->format_sel]);
        if (a->channels_sel > 0)
            fprintf(f, " channels=%d", audio_channels_values[a->channels_sel]);
        if (a->latency_sel > 0)
            fprintf(f, " fragments=%d fragment_size=%d",
                audio_latency_fragments[a->latency_sel],
                audio_latency_fragment_size[a->latency_sel]);
        fputs("\r\n", f);
    }
    if (a->null_sink_enabled)
        fputs("load-module module-null-sink sink_name=null\r\n", f);
    if (a->loopback_enabled)
        fputs("load-module module-loopback\r\n", f);

    {
        const char *port = a->pulseport[0] ? a->pulseport : "4713";
        const char *listens[2];
        int nlisten = 0;
        int i;

        if (a->listen_sel == 0)
            listens[nlisten++] = "127.0.0.1";
        else if (a->listen_sel == 1)
            listens[nlisten++] = "0.0.0.0";
        else {
            listens[nlisten++] = "0.0.0.0";
            listens[nlisten++] = "::";
        }

        for (i = 0; i < nlisten; i++) {
            fputs("load-module module-native-protocol-tcp", f);
            fprintf(f, " port=%s listen=%s", port, listens[i]);
            if (a->auth_sel == 1)
                fputs(" auth-anonymous=1", f);
            else if (a->auth_sel == 2 && a->auth_acl[0])
                fprintf(f, " auth-ip-acl=%s auth-cookie-enabled=0", a->auth_acl);
            else
                fprintf(f, " auth-cookie=%s", cookie_esc);
            fputs("\r\n", f);
        }
    }

    fclose(f);
}

/* Generate the PulseAudio auth cookie at %APPDATA%\Ming64X\pulse-cookie.  The
   daemon reads this exact file (see write_default_pa) and launchx ships the
   same bytes to the remote over SSH.  The cookie is PA_NATIVE_COOKIE_LENGTH
   (256) printable [A-Za-z0-9] bytes; it is regenerated if missing or short. */
void
ensure_pulse_cookie(void)
{
    static const char charset[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    char dir[512], path[512];
    FILE *f;
    unsigned char cookie[256], raw[256];
    long sz;
    int i;

    if (config_dir(dir, sizeof dir))
        return;
    CreateDirectoryA(dir, NULL);
    snprintf(path, sizeof path, "%s\\pulse-cookie", dir);

    /* keep it only if a full-length cookie already exists */
    f = fopen(path, "rb");
    if (f) {
        if (fseek(f, 0, SEEK_END) == 0) {
            sz = ftell(f);
            if (sz == (long)sizeof cookie) {
                fclose(f);
                return;
            }
        }
        fclose(f);
    }

    if (BCryptGenRandom(NULL, raw, sizeof raw,
            BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        return;
    for (i = 0; i < (int)sizeof cookie; i++)
        cookie[i] = (unsigned char)charset[raw[i] % (sizeof charset - 1)];

    f = fopen(path, "wb");
    if (!f)
        return;
    fwrite(cookie, 1, sizeof cookie, f);
    fclose(f);
}

/* Read the 256-byte PulseAudio cookie back.  Returns 1 on success. */
int
read_pulse_cookie(unsigned char out[256])
{
    char dir[512], path[512];
    FILE *f;

    if (config_dir(dir, sizeof dir))
        return 0;
    snprintf(path, sizeof path, "%s\\pulse-cookie", dir);
    f = fopen(path, "rb");
    if (!f)
        return 0;
    if (fread(out, 1, 256, f) != 256) {
        fclose(f);
        return 0;
    }
    fclose(f);
    return 1;
}
