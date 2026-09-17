// config.c — the four things a remote desktop needs to be told.

#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "config.h"

static void trim(char *s) {
    char *p = s;
    while (*p == ' ' || *p == '\t') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);

    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t'))
        s[--n] = 0;
}

static void state_dir(char *out, size_t size) {
    const char *home = getenv("HOME");
    snprintf(out, size, "%s/.local/share/wayrdp", home ? home : "/tmp");
}

bool wr_config_load(struct wr_config *cfg, const char **error) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->port = 3389;
    snprintf(cfg->bind_address, sizeof(cfg->bind_address), "0.0.0.0");

    char dir[256];
    state_dir(dir, sizeof(dir));
    snprintf(cfg->cert_path, sizeof(cfg->cert_path), "%s/tls.crt", dir);
    snprintf(cfg->key_path, sizeof(cfg->key_path), "%s/tls.key", dir);

    const char *explicit_path = getenv("WAYRDP_CONFIG");
    if (explicit_path) {
        snprintf(cfg->path, sizeof(cfg->path), "%s", explicit_path);
    } else {
        const char *home = getenv("HOME");
        const char *xdg = getenv("XDG_CONFIG_HOME");
        if (xdg && *xdg)
            snprintf(cfg->path, sizeof(cfg->path), "%s/wayrdp/wayrdp.conf", xdg);
        else
            snprintf(cfg->path, sizeof(cfg->path), "%s/.config/wayrdp/wayrdp.conf",
                     home ? home : "/tmp");
    }

    FILE *f = fopen(cfg->path, "r");
    if (!f) {
        // Nothing configured yet is a state, not a failure: the caller decides
        // whether a server with no password is allowed to start (it is not).
        if (errno == ENOENT) return true;
        *error = strerror(errno);
        return false;
    }

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;

        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;

        char *key = line, *value = eq + 1;
        trim(key); trim(value);
        if (!*key) continue;

        if (!strcmp(key, "port"))
            cfg->port = atoi(value);
        else if (!strcmp(key, "username"))
            snprintf(cfg->username, sizeof(cfg->username), "%s", value);
        else if (!strcmp(key, "password"))
            snprintf(cfg->password, sizeof(cfg->password), "%s", value);
        else if (!strcmp(key, "password_hash"))
            snprintf(cfg->password_hash, sizeof(cfg->password_hash), "%s", value);
        else if (!strcmp(key, "bind"))
            snprintf(cfg->bind_address, sizeof(cfg->bind_address), "%s", value);
    }
    fclose(f);
    return true;
}

bool wr_config_usable(const struct wr_config *cfg, const char **why) {
    if (cfg->port <= 0 || cfg->port > 65535) {
        *why = "port is outside 1-65535";
        return false;
    }
    if (!cfg->username[0]) {
        *why = "no username set";
        return false;
    }
    // A server anyone can walk into is worse than one that will not start.
    // Either the secret or a hash of it satisfies the requirement; a malformed
    // hash is treated as no credential so a typo cannot silently open the port.
    if (cfg->password_hash[0]) {
        if (!wr_password_is_hash(cfg->password_hash)) {
            *why = "password_hash is not a supported scrypt value";
            return false;
        }
    } else if (!cfg->password[0]) {
        *why = "no password set";
        return false;
    }
    return true;
}

static bool file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && st.st_size > 0;
}

bool wr_config_ensure_certificate(struct wr_config *cfg, const char **error) {
    if (file_exists(cfg->cert_path) && file_exists(cfg->key_path)) return true;

    char dir[256];
    state_dir(dir, sizeof(dir));
    if (mkdir(dir, 0700) != 0 && errno != EEXIST) {
        *error = "could not create ~/.local/share/wayrdp";
        return false;
    }

    // Self-signed, because there is nobody to sign it: the client will warn
    // once and remember the fingerprint, which is the same trust model as ssh
    // and the honest one for a desktop serving itself.
    char cmd[1600];
    snprintf(cmd, sizeof(cmd),
             "openssl req -x509 -newkey rsa:2048 -nodes -days 3650 "
             "-keyout '%s' -out '%s' -subj '/CN=wayrdp' >/dev/null 2>&1",
             cfg->key_path, cfg->cert_path);

    if (system(cmd) != 0 || !file_exists(cfg->cert_path)) {
        *error = "could not generate a TLS certificate (is openssl installed?)";
        return false;
    }

    chmod(cfg->key_path, 0600);
    return true;
}
