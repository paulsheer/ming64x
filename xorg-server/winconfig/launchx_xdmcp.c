#include "launchx_xdmcp.h"

void
tab_xdmcp(struct nk_context *ctx, struct options_xdmcp *opt)
{
    heading(ctx, "XDMCP");

    checkbox_option(ctx, "Enable XDMCP", &opt->xdmcp_enabled,
        "Enable the XDMCP protocol so the server contacts a display manager.\nAll XDMCP settings below are disabled while this is off.");

    if (!opt->xdmcp_enabled)
        nk_widget_disable_begin(ctx);

    if (opt->xdmcp_enabled && opt->broadcast_enabled)
        nk_widget_disable_begin(ctx);
    text_option(ctx, "Query host (-query)", opt->query_host, (int)sizeof(opt->query_host), "Enable XDMCP and send Query packets to this host.");
    if (opt->xdmcp_enabled && opt->broadcast_enabled)
        nk_widget_disable_end(ctx);

    if (opt->xdmcp_enabled && !opt->broadcast_enabled && opt->query_host[0])
        nk_widget_disable_begin(ctx);
    checkbox_option(ctx, "Broadcast for XDMCP (-broadcast)", &opt->broadcast_enabled, "Enable XDMCP and broadcast a query to the network. The first\ndisplay manager to answer hosts the session.");
    if (opt->xdmcp_enabled && !opt->broadcast_enabled && opt->query_host[0])
        nk_widget_disable_end(ctx);

    text_option(ctx, "Indirect host (-indirect)", opt->indirect_host, (int)sizeof(opt->indirect_host), "Enable XDMCP and send IndirectQuery packets to this host.");

    checkbox_option(ctx, "IPv6 multicast (-multicast)", &opt->multicast_enabled, "Enable XDMCP and multicast a query to the network (IPv6 multicast).");

    text_option(ctx, "UDP port (-port)", opt->port_string, (int)sizeof(opt->port_string), "UDP port used for XDMCP packets (default 177).");

    text_option(ctx, "Local address (-from)", opt->from_address, (int)sizeof(opt->from_address), "Local address to connect from, useful when the machine has several\nnetwork interfaces.");

    checkbox_option(ctx, "Terminate after one session (-once)", &opt->once_enabled, "Exit the server when the XDMCP session ends, instead of resetting.");

    text_option(ctx, "Display class (-class)", opt->display_class, (int)sizeof(opt->display_class), "XDMCP display qualifier used when looking up display-specific\noptions (default MIT-unspecified).");

    text_option(ctx, "Magic cookie (-cookie)", opt->cookie, (int)sizeof(opt->cookie), "Private key shared with the display manager for XDM-AUTHORIZATION-1.");

    text_option(ctx, "Display ID (-displayID)", opt->display_id, (int)sizeof(opt->display_id), "Identifier the display manager uses to locate this display's\nshared key.");

    if (!opt->xdmcp_enabled)
        nk_widget_disable_end(ctx);
}
