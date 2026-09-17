// config.h — port, username and credential, as the panel writes them.

#ifndef WAYRDP_CONFIG_H
#define WAYRDP_CONFIG_H

#include <stdbool.h>

#include "password.h"

#define WR_MAX_FIELD 128

struct wr_config {
    int port;
    char username[WR_MAX_FIELD];
    // The panel may write either the secret itself, or (preferred) a scrypt
    // hash of it. The hash wins when both are present.
    char password[WR_MAX_FIELD];
    char password_hash[WR_PASSWORD_ENCODED_MAX];
    char bind_address[WR_MAX_FIELD];

    char cert_path[512];
    char key_path[512];
    char path[512];                 // where this was read from
};

// Reads ~/.config/wayrdp/wayrdp.conf, or $WAYRDP_CONFIG.
//
// Missing file is not an error: the defaults are a server nobody can reach
// (no username, no password), which is the right thing to be before anyone has
// said otherwise.
bool wr_config_load(struct wr_config *cfg, const char **error);

// True when a client could actually get in. A blank password is not a password.
bool wr_config_usable(const struct wr_config *cfg, const char **why);

// Makes sure a TLS certificate exists, generating a self-signed one on first
// run. RDP will not talk without one.
bool wr_config_ensure_certificate(struct wr_config *cfg, const char **error);

#endif
