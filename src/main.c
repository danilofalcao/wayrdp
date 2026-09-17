// main.c — wayrdp, an RDP server for the session it runs inside.

#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include <openssl/crypto.h>

#include "audio.h"
#include "config.h"
#include "password.h"
#include "rdp.h"
#include "wayland.h"

static volatile bool running = true;

static void on_signal(int sig) { (void)sig; running = false; }

static void usage(const char *argv0) {
    printf("usage: %s [--check] [--hash-password]\n"
           "\n"
           "Serves the Wayland session it is started in over RDP. Port, username\n"
           "and credential come from ~/.config/wayrdp/wayrdp.conf.\n"
           "\n"
           "  --check           report whether this compositor and this configuration\n"
           "                    can serve, and exit without listening\n"
           "  --hash-password   read a password from stdin and print the value to\n"
           "                    store as password_hash in the configuration\n", argv0);
}

// Reads one line of secret, without echo on a terminal, so it can also be piped
// from the panel.
static bool read_password(char *out, size_t capacity) {
    bool tty = isatty(STDIN_FILENO) == 1;
    struct termios saved;
    if (tty) {
        if (tcgetattr(STDIN_FILENO, &saved) != 0) return false;
        struct termios hidden = saved;
        hidden.c_lflag &= ~(tcflag_t)ECHO;
        if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &hidden) != 0) return false;
        fprintf(stderr, "Password: ");
        fflush(stderr);
    }

    bool read = fgets(out, (int)capacity, stdin) != NULL;

    if (tty) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved);
        fprintf(stderr, "\n");
    }
    if (!read) return false;

    size_t n = strlen(out);
    while (n && (out[n - 1] == '\n' || out[n - 1] == '\r')) out[--n] = 0;
    return n > 0;
}

static int hash_password(void) {
    char password[WR_MAX_FIELD];
    char encoded[WR_PASSWORD_ENCODED_MAX];
    if (!read_password(password, sizeof(password))) {
        fprintf(stderr, "wayrdp: could not read a password\n");
        return 1;
    }
    bool ok = wr_password_hash(password, encoded, sizeof(encoded));
    OPENSSL_cleanse(password, sizeof(password));
    if (!ok) {
        fprintf(stderr, "wayrdp: could not hash the password\n");
        return 1;
    }
    printf("%s\n", encoded);
    return 0;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    bool check_only = false;
    bool hash_only = false;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--check")) check_only = true;
        else if (!strcmp(argv[i], "--hash-password")) hash_only = true;
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { usage(argv[0]); return 0; }
        else { fprintf(stderr, "wayrdp: unknown argument '%s'\n", argv[i]); return 1; }
    }

    if (hash_only) {
        if (check_only) {
            fprintf(stderr, "wayrdp: --check and --hash-password are mutually exclusive\n");
            return 1;
        }
        return hash_password();
    }

    const char *error = NULL;

    struct wr_config config;
    if (!wr_config_load(&config, &error)) {
        fprintf(stderr, "wayrdp: %s: %s\n", config.path, error);
        return 1;
    }

    struct wr_wayland *wayland = wr_open(&error);
    if (!wayland) { fprintf(stderr, "wayrdp: %s\n", error); return 1; }

    if (!wr_capture_open(wayland, &error)) { fprintf(stderr, "wayrdp: %s\n", error); return 1; }
    if (!wr_input_open(wayland, &error))   { fprintf(stderr, "wayrdp: %s\n", error); return 1; }

    const char *why = NULL;
    bool usable = wr_config_usable(&config, &why);

    // Sound is the one part that is allowed to be missing. A machine with no
    // audio server still has a screen and a keyboard worth reaching.
    const char *audio_error = NULL;
    struct wr_audio *audio = wr_audio_open(&audio_error);

    if (check_only) {
        printf("compositor: capture and input available, %ux%u\n",
               wr_width(wayland), wr_height(wayland));
        printf("audio:      %s\n", audio ? "PipeWire, sound and microphone"
                                          : audio_error);
        printf("config:     %s\n", config.path);
        if (usable) printf("ready:      yes, port %d as '%s'\n", config.port, config.username);
        else        printf("ready:      no (%s)\n", why);
        wr_audio_close(audio);
        wr_close(wayland);
        return usable ? 0 : 3;
    }

    // Refusing to start is the right answer to a half-filled form: a desktop
    // listening with no password is worse than one that is not listening.
    if (!usable) {
        fprintf(stderr, "wayrdp: not starting -- %s. Set it in %s\n",
                why, config.path);
        wr_audio_close(audio);
        wr_close(wayland);
        return 1;
    }

    if (!audio)
        fprintf(stderr, "wayrdp: no sound -- %s\n", audio_error);

    if (!wr_config_ensure_certificate(&config, &error)) {
        fprintf(stderr, "wayrdp: %s\n", error);
        wr_audio_close(audio);
        wr_close(wayland);
        return 1;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);   // a client that vanishes mid-frame is routine

    struct wr_server *server = wr_server_new(wayland, audio, &config, &error);
    if (!server) {
        fprintf(stderr, "wayrdp: %s\n", error);
        wr_audio_close(audio);
        wr_close(wayland);
        return 1;
    }

    printf("wayrdp: listening on %s:%d as '%s', serving %ux%u\n",
           config.bind_address, config.port, config.username,
           wr_width(wayland), wr_height(wayland));

    bool ok = wr_server_run(server, &running);

    wr_server_free(server);
    wr_audio_close(audio);
    wr_close(wayland);
    return ok ? 0 : 1;
}
