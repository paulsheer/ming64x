#ifndef LAUNCHX_SSHLOGIN_H
#define LAUNCHX_SSHLOGIN_H

#include "launchx_common.h"
#include "launchx_network.h"
#include "launchx_audio.h"

struct options_ssh_login {
    char host[128];
    char port[16];
    char username[128];
    char password[128];
    char password_mask[128];
    int x11_forwarding;
    int save_password;
};

void tab_ssh_login(struct nk_context *ctx, struct options_ssh_login *opt,
    terminal *term, ssh_session *ssh,
    struct options_network_and_access_control *net,
    struct options_audio *audio);

#endif /* LAUNCHX_SSHLOGIN_H */
