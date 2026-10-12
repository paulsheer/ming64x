#ifndef LAUNCHX_NETWORK_H
#define LAUNCHX_NETWORK_H

#include "launchx_common.h"

struct options_network_and_access_control {
    int ac_enabled;
    char allow_string[1024];
    char auth_file[1024];
    int byteswap_enabled;
    int maxclients_sel;
    int maxbigreqsize;
    int listeningport_sel;
    char vmid[256];
    char vsockport[16];
    int hyperv_registry_enabled;
};

static const char *maxclients_items[] = {"64", "128", "256", "512", "1024", "2048"};

#define MAXCLIENTS_COUNT (sizeof(maxclients_items) / sizeof(maxclients_items[0]))

static const char *listeningport_items[] = {
    "Display :0 on tcp port 6000",
    "Display :1 on tcp port 6001",
    "Display :2 on tcp port 6002",
    "Display :3 on tcp port 6003",
    "Display :4 on tcp port 6004",
    "Display :5 on tcp port 6005",
    "Display :6 on tcp port 6006",
    "Display :7 on tcp port 6007",
    "Display :8 on tcp port 6008",
    "Display :9 on tcp port 6009",
    "Display :10 on tcp port 6010",
    "Display :11 on tcp port 6011",
    "Display :12 on tcp port 6012"
};
#define LISTENINGPORT_COUNT (sizeof(listeningport_items) / sizeof(listeningport_items[0]))

void tab_network_and_access_control(struct nk_context *ctx,
    struct options_network_and_access_control *opt);
int hyperv_update_registry(const struct options_network_and_access_control *net);
int hyperv_registry_apply(const struct options_network_and_access_control *net);

#endif /* LAUNCHX_NETWORK_H */
