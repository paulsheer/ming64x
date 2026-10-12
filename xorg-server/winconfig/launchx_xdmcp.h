#ifndef LAUNCHX_XDMCP_H
#define LAUNCHX_XDMCP_H

#include "launchx_common.h"

struct options_xdmcp {
    int xdmcp_enabled;
    char query_host[256];
    int broadcast_enabled;
    char indirect_host[256];
    int multicast_enabled;
    char port_string[16];
    char from_address[256];
    int once_enabled;
    char display_class[256];
    char cookie[256];
    char display_id[256];
};

void tab_xdmcp(struct nk_context *ctx, struct options_xdmcp *opt);

#endif /* LAUNCHX_XDMCP_H */
