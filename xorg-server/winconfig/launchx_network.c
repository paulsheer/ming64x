#include "launchx_network.h"

#include <winreg.h>
#include <shellapi.h>

/* Full-row read-only text field: no label, selectable but not editable.
   Used for the Hyper-V registry path, which mirrors the vsock port. */
static void
readonly_option_fullrow(struct nk_context *ctx, const char *value, const char *tooltip)
{
    struct nk_rect eb;
    static char buf[512];

    nk_layout_row_dynamic(ctx, 30, 1);
    eb = nk_widget_bounds(ctx);
    snprintf(buf, sizeof(buf), "%s", value);
    nk_edit_string_zero_terminated(ctx,
        NK_EDIT_SELECTABLE | NK_EDIT_CLIPBOARD | NK_EDIT_AUTO_SELECT,
        buf, (int)sizeof(buf), readonly_filter);
    option_tooltip(ctx, eb, tooltip);
    field_context_menu(ctx, eb, buf, (int)sizeof(buf), 1, 0, "hyperv_registry_path");
}

/* Compute the GuestCommunicationServices registry key name from the VSock
   port. The service GUID is HV_GUID_VSOCK_TEMPLATE with the port encoded in
   Data1 (the first eight hex digits). */
static void
hyperv_registry_path(const char *vsockport, char *out, size_t outsz)
{
    unsigned int port = (unsigned int)strtoul(vsockport, NULL, 10);

    snprintf(out, outsz,
        "HKLM\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Virtualization\\GuestCommunicationServices\\%08X-FACB-11E6-BD58-64006A7986D3",
        port);
}

/* Create/update the GuestCommunicationServices registry key for the current
   VSock port so the Hyper-V host will route incoming guest connections to
   the listener.  Requires elevation (HKLM).  Returns 0 on success, else the
   Win32 error code. */
int
hyperv_update_registry(const struct options_network_and_access_control *net)
{
    char full[512];
    char element[64];
    unsigned int port;
    const char *subkey;
    HKEY hk = NULL;
    LONG rc;
    DWORD disp;

    port = (unsigned int)strtoul(net->vsockport, NULL, 10);
    hyperv_registry_path(net->vsockport, full, sizeof(full));
    subkey = full + 5;                  /* skip the "HKLM\" prefix */

    snprintf(element, sizeof(element), "ming64x Hyper-V VSock port %u", port);

    rc = RegCreateKeyExA(HKEY_LOCAL_MACHINE, subkey, 0, NULL, 0,
        KEY_SET_VALUE | KEY_WOW64_64KEY, NULL, &hk, &disp);
    if (rc != ERROR_SUCCESS)
        return (int)rc;

    rc = RegSetValueExA(hk, "ElementName", 0, REG_SZ,
        (const BYTE *)element, (DWORD)(strlen(element) + 1));
    RegCloseKey(hk);
    return (rc == ERROR_SUCCESS) ? 0 : (int)rc;
}

/* Relaunch launchx.exe elevated (UAC) to write the Hyper-V registry key,
   then wait for it to finish.  Returns the helper's exit code (0 on success,
   else a Win32 error code). */
static int
hyperv_self_elevate_write(const char *vsockport)
{
    char self[MAX_PATH];
    char args[160];
    SHELLEXECUTEINFOA sei;
    DWORD exit_code = ERROR_ACCESS_DENIED;

    if (GetModuleFileNameA(NULL, self, sizeof(self)) == 0)
        return ERROR_FILE_NOT_FOUND;

    snprintf(args, sizeof(args), "--write-hyperv-reg \"%s\"", vsockport);

    ZeroMemory(&sei, sizeof(sei));
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    sei.lpVerb = "runas";
    sei.lpFile = self;
    sei.lpParameters = args;
    sei.nShow = SW_HIDE;

    if (!ShellExecuteExA(&sei))
        return (int)GetLastError();

    WaitForSingleObject(sei.hProcess, INFINITE);
    if (!GetExitCodeProcess(sei.hProcess, &exit_code))
        exit_code = GetLastError();
    CloseHandle(sei.hProcess);
    return (int)exit_code;
}

/* Write the Hyper-V registry key, self-elevating via UAC when the direct
   write is denied.  Returns 0 on success, else a Win32 error code. */
static int
hyperv_registry_write_elevated(const char *vsockport)
{
    struct options_network_and_access_control net;
    int rc;

    memset(&net, 0, sizeof(net));
    snprintf(net.vsockport, sizeof(net.vsockport), "%s", vsockport);

    rc = hyperv_update_registry(&net);
    if (rc == 0 || rc != ERROR_ACCESS_DENIED)
        return rc;

    return hyperv_self_elevate_write(vsockport);
}

/* Write the Hyper-V service GUID to the registry (elevating if necessary),
   surfacing any failure via the shared error modal.  Returns 0 on success,
   else the Win32 error code.  Skips a rewrite of the same port so the Start
   handler does not prompt for UAC again after the checkbox already wrote it. */
int
hyperv_registry_apply(const struct options_network_and_access_control *net)
{
    static char written_port[16];
    int rc;

    if (strcmp(written_port, net->vsockport) == 0)
        return 0;

    rc = hyperv_registry_write_elevated(net->vsockport);
    if (rc != 0) {
        char path[512];

        hyperv_registry_path(net->vsockport, path, sizeof(path));
        snprintf(g_hyperv_error, sizeof(g_hyperv_error),
            "Could not write the Hyper-V service GUID to the registry:\n"
            "%s\n\n"
            "Incoming Hyper-V connections will be rejected.\n"
            "Approve the administrator prompt, or create the key manually.\n"
            "(Registry error %d)",
            path, rc);
    } else {
        snprintf(written_port, sizeof(written_port), "%s", net->vsockport);
    }
    return rc;
}

void
tab_network_and_access_control(struct nk_context *ctx,
    struct options_network_and_access_control *opt)
{
    struct nk_rect b;
    nk_flags vsockport_flags;
    int port_committed;
    static char prev_vsockport[16];
    static int prev_hyperv_enabled;
    static int hyperv_seen;

    heading(ctx, "Networking & access control");

    combobox_option(ctx, "Listening port", listeningport_items, (int)LISTENINGPORT_COUNT, &opt->listeningport_sel, "Display number the server runs as. Clients connect on TCP port\n6000 plus this number (default :0).");

    checkbox_option(ctx, "Disable access control (-ac)", &opt->ac_enabled, "Disable host-based access control, so any host may connect and\nchange the access list. Use with caution.");

    if (opt->ac_enabled)
        nk_widget_disable_begin(ctx);
    text_option(ctx, "Allowed IP addresses (-allow)", opt->allow_string, (int)sizeof(opt->allow_string), "allow connections whose address matches a ';'-separated IP list\n(e.g. 192.168.1.0/24;10.0.0.5-10.0.0.9;FE80::1). ',' is also accepted.");
    if (opt->ac_enabled)
        nk_widget_disable_end(ctx);

    text_option(ctx, "Authorization file (-auth)", opt->auth_file, (int)sizeof(opt->auth_file), "File of authorization records used to authenticate client access.");

    checkbox_option(ctx, "Allow different-endianness clients (+byteswappedclients)", &opt->byteswap_enabled, "Allow connections from clients with a different byte order\n(endianness) than the server.");

    combobox_option(ctx, "Max clients (-maxclients)", maxclients_items, (int)MAXCLIENTS_COUNT, &opt->maxclients_sel, "Maximum number of clients allowed to connect (a power of two).");

    nk_layout_row_dynamic(ctx, 30, 3);
    b = nk_widget_bounds(ctx);
    nk_label(ctx, "Max bigreq size (MB)", NK_TEXT_LEFT);
    option_tooltip(ctx, b, "Largest request the server will accept, in megabytes.");
    b = nk_widget_bounds(ctx);
    nk_slider_int(ctx, 1, &opt->maxbigreqsize, 127, 1);
    option_tooltip(ctx, b, "Largest request the server will accept, in megabytes.");
    nk_labelf(ctx, NK_TEXT_LEFT, "%d", opt->maxbigreqsize);

    text_option(ctx, "Hyper-V VM GUID (-vmid)", opt->vmid, (int)sizeof(opt->vmid),
        "Hyper-V virtual machine GUID to accept VSock connections from.");

    vsockport_flags = text_option(ctx, "Hyper-V VSock listen port (-vsockport)", opt->vsockport, (int)sizeof(opt->vsockport),
        "Port number to listen on for VSock connections. Default 106000.");

    {
        char path[512];
        hyperv_registry_path(opt->vsockport, path, sizeof(path));
        readonly_option_fullrow(ctx, path,
            "Full registry key under which the Hyper-V service GUID must be\nregistered to accept incoming Hyper-V connections.");
    }

    checkbox_option(ctx, "Update registry with Hyper-V GUID", &opt->hyperv_registry_enabled,
        "You need to check this to allow incoming Hyper-V connections");

    /* Write the registry entry when the box is checked, or when the vsockport
       edit commits (focus leaves the field), rather than on every keystroke. */
    if (!hyperv_seen) {
        snprintf(prev_vsockport, sizeof(prev_vsockport), "%s", opt->vsockport);
        prev_hyperv_enabled = opt->hyperv_registry_enabled;
        hyperv_seen = 1;
    } else {
        port_committed = (vsockport_flags & NK_EDIT_DEACTIVATED) &&
                         strcmp(prev_vsockport, opt->vsockport) != 0;
        if (port_committed)
            snprintf(prev_vsockport, sizeof(prev_vsockport), "%s", opt->vsockport);

        if (opt->hyperv_registry_enabled &&
            (prev_hyperv_enabled != opt->hyperv_registry_enabled || port_committed))
            hyperv_registry_apply(opt);

        prev_hyperv_enabled = opt->hyperv_registry_enabled;
    }
}
