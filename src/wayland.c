// wayland.c — capture and input against a wlroots compositor.

#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <linux/input-event-codes.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <linux/uinput.h>
#include <fcntl.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-client-protocol.h"
#include "wlr-screencopy-unstable-v1-client-protocol.h"
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"
#include "virtual-keyboard-unstable-v1-client-protocol.h"

#include "wayland.h"

struct wr_wayland {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_shm *shm;
    struct wl_seat *seat;
    struct wl_output *output;

    struct ext_output_image_capture_source_manager_v1 *sources;
    struct ext_image_copy_capture_manager_v1 *capture_manager;
    struct zwlr_screencopy_manager_v1 *screencopy;
    struct zwlr_virtual_pointer_manager_v1 *pointer_manager;
    struct zwp_virtual_keyboard_manager_v1 *keyboard_manager;

    struct ext_image_copy_capture_session_v1 *session;
    struct zwlr_virtual_pointer_v1 *pointer;
    struct zwp_virtual_keyboard_v1 *keyboard;

    bool use_uinput;
    int uinput_fd;

    // Negotiated once, at session done.
    uint32_t width, height, format;
    int bpp;
    bool have_size, have_format, session_done, session_stopped;

    // The capture buffer is allocated once and reused: a new wl_buffer per
    // frame would mean a new pool and an mmap per frame at 60Hz.
    struct wl_buffer *buffer;
    void *pixels;
    size_t size, stride;

    struct wr_frame frame;
    bool frame_ready, frame_failed;
    uint32_t fail_reason;

    // The on-demand path owns its own buffer: screencopy names its own format
    // and stride, and they need not match what the capture session chose.
    // Modifier state, tracked here because nothing else does it.
    uint32_t mods_depressed, mods_locked;

    struct wl_buffer *now_buffer;
    void *now_pixels;
    size_t now_size, now_stride;
    uint32_t now_format, now_width, now_height;
    struct wr_frame now_frame;
    bool now_have_buffer, now_ready, now_failed, now_described;
};

// --- helpers ---------------------------------------------------------------

static int bytes_per_pixel(uint32_t format) {
    switch (format) {
    case WL_SHM_FORMAT_BGR888:
    case WL_SHM_FORMAT_RGB888:
        return 3;
    case WL_SHM_FORMAT_XRGB8888:
    case WL_SHM_FORMAT_ARGB8888:
    case WL_SHM_FORMAT_XBGR8888:
    case WL_SHM_FORMAT_ABGR8888:
        return 4;
    default:
        return 0;
    }
}

// DRM names a format by the bit layout of a little-endian word, so the byte
// order in memory is the reverse of the name: XR24 is B,G,R,X on the wire while
// BG24 is R,G,B.
void wr_read_rgb(const uint8_t *p, uint32_t format, uint8_t out[3]) {
    switch (format) {
    case WL_SHM_FORMAT_RGB888:
    case WL_SHM_FORMAT_XRGB8888:
    case WL_SHM_FORMAT_ARGB8888:
        out[0] = p[2]; out[1] = p[1]; out[2] = p[0]; break;
    default: // BG24, XB24, AB24: red comes first in memory
        out[0] = p[0]; out[1] = p[1]; out[2] = p[2]; break;
    }
}

static uint32_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static int anonymous_shm(size_t size) {
    int fd = memfd_create("wayrdp", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0) return -1;
    if (ftruncate(fd, (off_t)size) < 0) { close(fd); return -1; }
    return fd;
}

// Dispatch until a flag flips or the deadline passes. Returns false on timeout.
static bool pump(struct wr_wayland *w, const bool *a, const bool *b, int timeout_ms) {
    struct pollfd pfd = { .fd = wl_display_get_fd(w->display), .events = POLLIN };

    while (!*a && (!b || !*b)) {
        while (wl_display_prepare_read(w->display) != 0)
            wl_display_dispatch_pending(w->display);
        wl_display_flush(w->display);

        int n = poll(&pfd, 1, timeout_ms);
        if (n <= 0) { wl_display_cancel_read(w->display); return false; }

        if (wl_display_read_events(w->display) < 0) return false;
        if (wl_display_dispatch_pending(w->display) < 0) return false;
    }
    return true;
}

// --- session ---------------------------------------------------------------

static void on_buffer_size(void *data, struct ext_image_copy_capture_session_v1 *s,
                           uint32_t width, uint32_t height) {
    struct wr_wayland *w = data; (void)s;
    w->width = width; w->height = height; w->have_size = true;
}

static void on_shm_format(void *data, struct ext_image_copy_capture_session_v1 *s,
                          uint32_t format) {
    struct wr_wayland *w = data; (void)s;
    int bpp = bytes_per_pixel(format);
    if (bpp == 0) return;

    // 32-bit first where it exists, because RDP encodes from it without
    // repacking. Wayfire offers only BG24, so the 24-bit path is the normal
    // one here rather than a fallback.
    if (!w->have_format || (w->bpp != 4 && bpp == 4)) {
        w->format = format; w->bpp = bpp; w->have_format = true;
    }
}

static void on_dmabuf_device(void *d, struct ext_image_copy_capture_session_v1 *s,
                             struct wl_array *a) { (void)d; (void)s; (void)a; }
static void on_dmabuf_format(void *d, struct ext_image_copy_capture_session_v1 *s,
                             uint32_t f, struct wl_array *m) { (void)d; (void)s; (void)f; (void)m; }

static void on_session_done(void *data, struct ext_image_copy_capture_session_v1 *s) {
    struct wr_wayland *w = data; (void)s; w->session_done = true;
}
static void on_session_stopped(void *data, struct ext_image_copy_capture_session_v1 *s) {
    struct wr_wayland *w = data; (void)s; w->session_stopped = true;
}

static const struct ext_image_copy_capture_session_v1_listener session_listener = {
    .buffer_size = on_buffer_size, .shm_format = on_shm_format,
    .dmabuf_device = on_dmabuf_device, .dmabuf_format = on_dmabuf_format,
    .done = on_session_done, .stopped = on_session_stopped,
};

// --- frame -----------------------------------------------------------------

static void on_frame_transform(void *d, struct ext_image_copy_capture_frame_v1 *f,
                               uint32_t t) { (void)d; (void)f; (void)t; }

static void on_frame_damage(void *data, struct ext_image_copy_capture_frame_v1 *f,
                            int32_t x, int32_t y, int32_t width, int32_t height) {
    struct wr_wayland *w = data; (void)f;
    if (w->frame.damage_count < WR_MAX_DAMAGE) {
        w->frame.damage[w->frame.damage_count++] =
            (struct wr_rect){ x, y, width, height };
    } else {
        // More rectangles than we kept. Sending the whole screen is worse than
        // sending sixty-four rectangles and better than sending the wrong ones.
        w->frame.damage_overflowed = true;
    }
}

static void on_frame_time(void *d, struct ext_image_copy_capture_frame_v1 *f,
                          uint32_t hi, uint32_t lo, uint32_t n) {
    (void)d; (void)f; (void)hi; (void)lo; (void)n;
}

static void on_frame_ready(void *data, struct ext_image_copy_capture_frame_v1 *f) {
    struct wr_wayland *w = data; (void)f; w->frame_ready = true;
}

static void on_frame_failed(void *data, struct ext_image_copy_capture_frame_v1 *f,
                            uint32_t reason) {
    struct wr_wayland *w = data; (void)f;
    w->frame_failed = true; w->fail_reason = reason;
}

static const struct ext_image_copy_capture_frame_v1_listener frame_listener = {
    .transform = on_frame_transform, .damage = on_frame_damage,
    .presentation_time = on_frame_time, .ready = on_frame_ready,
    .failed = on_frame_failed,
};

// --- registry --------------------------------------------------------------

static void on_global(void *data, struct wl_registry *registry, uint32_t name,
                      const char *interface, uint32_t version) {
    struct wr_wayland *w = data;
    (void)version;

    if (!strcmp(interface, wl_shm_interface.name))
        w->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    else if (!strcmp(interface, wl_seat_interface.name) && !w->seat)
        w->seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
    else if (!strcmp(interface, wl_output_interface.name) && !w->output)
        w->output = wl_registry_bind(registry, name, &wl_output_interface, 1);
    else if (!strcmp(interface, ext_output_image_capture_source_manager_v1_interface.name))
        w->sources = wl_registry_bind(registry, name,
            &ext_output_image_capture_source_manager_v1_interface, 1);
    else if (!strcmp(interface, ext_image_copy_capture_manager_v1_interface.name))
        w->capture_manager = wl_registry_bind(registry, name,
            &ext_image_copy_capture_manager_v1_interface, 1);
    else if (!strcmp(interface, zwlr_screencopy_manager_v1_interface.name))
        w->screencopy = wl_registry_bind(registry, name,
            &zwlr_screencopy_manager_v1_interface, 3);
    else if (!strcmp(interface, zwlr_virtual_pointer_manager_v1_interface.name))
        w->pointer_manager = wl_registry_bind(registry, name,
            &zwlr_virtual_pointer_manager_v1_interface, 1);
    else if (!strcmp(interface, zwp_virtual_keyboard_manager_v1_interface.name))
        w->keyboard_manager = wl_registry_bind(registry, name,
            &zwp_virtual_keyboard_manager_v1_interface, 1);
}

static void on_global_remove(void *d, struct wl_registry *r, uint32_t n) {
    (void)d; (void)r; (void)n;
}

static const struct wl_registry_listener registry_listener = {
    .global = on_global, .global_remove = on_global_remove,
};

// The compositor's socket, when the environment does not name it.
//
// The oldest socket wins, not the newest. The session's own compositor started
// before anything else in it, and a nested one -- a test, a screen recorder's
// helper -- comes later. Picking the newest served a 1280x720 test session
// instead of the 2560x1440 desktop, which looked like it worked.
static const char *find_wayland_socket(void) {
    static char found[NAME_MAX + 1];
    const char *dir_path = getenv("XDG_RUNTIME_DIR");
    if (!dir_path) return NULL;

    DIR *dir = opendir(dir_path);
    if (!dir) return NULL;

    found[0] = 0;
    time_t oldest = 0;
    struct dirent *entry;

    while ((entry = readdir(dir))) {
        if (strncmp(entry->d_name, "wayland-", 8) != 0) continue;
        if (strstr(entry->d_name, ".lock")) continue;

        char path[512];
        snprintf(path, sizeof(path), "%s/%s", dir_path, entry->d_name);

        struct stat st;
        if (stat(path, &st) != 0 || !S_ISSOCK(st.st_mode)) continue;
        if (found[0] && st.st_mtime >= oldest) continue;

        oldest = st.st_mtime;
        snprintf(found, sizeof(found), "%s", entry->d_name);
    }
    closedir(dir);

    // Say which one, because a guess that is wrong should be visible in the
    // log rather than only in the resolution somebody notices later.
    if (found[0])
        fprintf(stderr, "wayrdp: WAYLAND_DISPLAY unset; using %s\n", found);
    return found[0] ? found : NULL;
}

// --- lifecycle -------------------------------------------------------------

struct wr_wayland *wr_open(const char **error) {
    struct wr_wayland *w = calloc(1, sizeof(*w));
    if (!w) { *error = "out of memory"; return NULL; }
    w->uinput_fd = -1;

    w->display = wl_display_connect(NULL);
    if (!w->display) {
        // Started by systemd rather than from a terminal, WAYLAND_DISPLAY may
        // never have reached the user manager's environment. The socket is
        // still where it always is, so look rather than fail: this is the
        // difference between a unit that works on a fresh machine and one that
        // works only where somebody ran `systemctl --user import-environment`.
        const char *name = find_wayland_socket();
        if (name) w->display = wl_display_connect(name);
    }
    if (!w->display) {
        *error = "no Wayland display, and no wayland-* socket in XDG_RUNTIME_DIR";
        free(w); return NULL;
    }

    w->registry = wl_display_get_registry(w->display);
    wl_registry_add_listener(w->registry, &registry_listener, w);
    wl_display_roundtrip(w->display);

    if (!w->shm)    { *error = "compositor has no wl_shm"; goto fail; }
    if (!w->seat)   { *error = "compositor has no wl_seat"; goto fail; }
    if (!w->output) { *error = "compositor has no wl_output"; goto fail; }
    return w;

fail:
    wl_display_disconnect(w->display);
    free(w);
    return NULL;
}

void wr_close(struct wr_wayland *w) {
    if (!w) return;
    if (w->uinput_fd >= 0) {
        ioctl(w->uinput_fd, UI_DEV_DESTROY);
        close(w->uinput_fd);
    }
    if (w->pixels) munmap(w->pixels, w->size);
    if (w->display) wl_display_disconnect(w->display);
    free(w);
}

int wr_fd(const struct wr_wayland *w) { return wl_display_get_fd(w->display); }
bool wr_flush(struct wr_wayland *w) { return wl_display_flush(w->display) >= 0; }
bool wr_dispatch_pending(struct wr_wayland *w) {
    return wl_display_dispatch_pending(w->display) >= 0;
}

uint32_t wr_width(const struct wr_wayland *w)  { return w->width; }
uint32_t wr_height(const struct wr_wayland *w) { return w->height; }

// --- capture ---------------------------------------------------------------

bool wr_capture_open(struct wr_wayland *w, const char **error) {
    if (!w->sources || !w->capture_manager) {
        *error = "compositor does not offer ext-image-copy-capture "
                 "(on Wayfire, add copy-capture to core/plugins)";
        return false;
    }

    struct ext_image_capture_source_v1 *source =
        ext_output_image_capture_source_manager_v1_create_source(w->sources, w->output);

    w->session = ext_image_copy_capture_manager_v1_create_session(
        w->capture_manager, source, 0);
    ext_image_copy_capture_session_v1_add_listener(w->session, &session_listener, w);

    if (!pump(w, &w->session_done, &w->session_stopped, 5000)) {
        *error = "the compositor never finished describing the capture session";
        return false;
    }
    if (w->session_stopped || !w->have_size || !w->have_format) {
        *error = "the compositor stopped the session before describing it";
        return false;
    }

    w->stride = (size_t)w->width * (size_t)w->bpp;
    w->size = w->stride * w->height;

    int fd = anonymous_shm(w->size);
    if (fd < 0) { *error = "could not allocate shared memory"; return false; }

    w->pixels = mmap(NULL, w->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (w->pixels == MAP_FAILED) { close(fd); *error = "could not map the frame buffer"; return false; }

    struct wl_shm_pool *pool = wl_shm_create_pool(w->shm, fd, (int32_t)w->size);
    w->buffer = wl_shm_pool_create_buffer(pool, 0, (int32_t)w->width, (int32_t)w->height,
                                          (int32_t)w->stride, w->format);
    wl_shm_pool_destroy(pool);
    close(fd);

    w->frame.pixels = w->pixels;
    w->frame.width = w->width;
    w->frame.height = w->height;
    w->frame.stride = w->stride;
    w->frame.format = w->format;
    w->frame.bytes_per_pixel = w->bpp;
    return true;
}

const struct wr_frame *wr_capture_frame(struct wr_wayland *w, int timeout_ms) {
    w->frame_ready = w->frame_failed = false;
    w->frame.damage_count = 0;
    w->frame.damage_overflowed = false;

    // One frame object per capture: that is the protocol's shape, not a
    // limitation, and it is what lets the compositor answer only when the
    // screen has actually changed.
    struct ext_image_copy_capture_frame_v1 *frame =
        ext_image_copy_capture_session_v1_create_frame(w->session);
    ext_image_copy_capture_frame_v1_add_listener(frame, &frame_listener, w);
    ext_image_copy_capture_frame_v1_attach_buffer(frame, w->buffer);
    ext_image_copy_capture_frame_v1_damage_buffer(frame, 0, 0,
        (int32_t)w->width, (int32_t)w->height);
    ext_image_copy_capture_frame_v1_capture(frame);

    bool got = pump(w, &w->frame_ready, &w->frame_failed, timeout_ms);
    ext_image_copy_capture_frame_v1_destroy(frame);

    if (!got || w->frame_failed) return NULL;
    return &w->frame;
}

// --- on-demand capture (screencopy) ----------------------------------------

static void now_buffer_ev(void *data, struct zwlr_screencopy_frame_v1 *f,
                          uint32_t format, uint32_t width, uint32_t height, uint32_t stride) {
    struct wr_wayland *w = data; (void)f;
    if (bytes_per_pixel(format) == 0) return;
    w->now_format = format; w->now_width = width;
    w->now_height = height; w->now_stride = stride;
    w->now_described = true;
}

static void now_flags(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t flags) {
    (void)d; (void)f; (void)flags;
}

static void now_ready_ev(void *data, struct zwlr_screencopy_frame_v1 *f,
                         uint32_t hi, uint32_t lo, uint32_t nsec) {
    struct wr_wayland *w = data; (void)f; (void)hi; (void)lo; (void)nsec;
    w->now_ready = true;
}

static void now_failed_ev(void *data, struct zwlr_screencopy_frame_v1 *f) {
    struct wr_wayland *w = data; (void)f; w->now_failed = true;
}

static void now_damage_ev(void *d, struct zwlr_screencopy_frame_v1 *f,
                          uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    (void)d; (void)f; (void)x; (void)y; (void)width; (void)height;
}

static void now_dmabuf(void *d, struct zwlr_screencopy_frame_v1 *f,
                       uint32_t format, uint32_t width, uint32_t height) {
    (void)d; (void)f; (void)format; (void)width; (void)height;
}

static void now_buffer_done(void *d, struct zwlr_screencopy_frame_v1 *f) {
    (void)d; (void)f;
}

static const struct zwlr_screencopy_frame_v1_listener now_listener = {
    .buffer = now_buffer_ev, .flags = now_flags, .ready = now_ready_ev,
    .failed = now_failed_ev, .damage = now_damage_ev,
    .linux_dmabuf = now_dmabuf, .buffer_done = now_buffer_done,
};

const struct wr_frame *wr_capture_now(struct wr_wayland *w, int timeout_ms) {
    if (!w->screencopy) return NULL;

    w->now_ready = w->now_failed = w->now_described = false;

    struct zwlr_screencopy_frame_v1 *frame =
        zwlr_screencopy_manager_v1_capture_output(w->screencopy, 0, w->output);
    zwlr_screencopy_frame_v1_add_listener(frame, &now_listener, w);

    // The buffer parameters arrive first and the copy cannot be asked for
    // before they do.
    if (!pump(w, &w->now_described, &w->now_failed, timeout_ms) || w->now_failed) {
        zwlr_screencopy_frame_v1_destroy(frame);
        return NULL;
    }

    size_t needed = w->now_stride * w->now_height;
    if (!w->now_have_buffer || needed != w->now_size) {
        if (w->now_pixels) munmap(w->now_pixels, w->now_size);
        if (w->now_buffer) wl_buffer_destroy(w->now_buffer);

        int fd = anonymous_shm(needed);
        if (fd < 0) { zwlr_screencopy_frame_v1_destroy(frame); return NULL; }

        w->now_pixels = mmap(NULL, needed, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (w->now_pixels == MAP_FAILED) {
            close(fd); w->now_pixels = NULL;
            zwlr_screencopy_frame_v1_destroy(frame);
            return NULL;
        }

        struct wl_shm_pool *pool = wl_shm_create_pool(w->shm, fd, (int32_t)needed);
        w->now_buffer = wl_shm_pool_create_buffer(pool, 0, (int32_t)w->now_width,
            (int32_t)w->now_height, (int32_t)w->now_stride, w->now_format);
        wl_shm_pool_destroy(pool);
        close(fd);

        w->now_size = needed;
        w->now_have_buffer = true;
    }

    zwlr_screencopy_frame_v1_copy(frame, w->now_buffer);

    bool got = pump(w, &w->now_ready, &w->now_failed, timeout_ms);
    zwlr_screencopy_frame_v1_destroy(frame);
    if (!got || w->now_failed) return NULL;

    w->now_frame = (struct wr_frame){
        .pixels = w->now_pixels,
        .width = w->now_width, .height = w->now_height,
        .stride = w->now_stride, .format = w->now_format,
        .bytes_per_pixel = bytes_per_pixel(w->now_format),
        .damage_count = 0, .damage_overflowed = true,   // treat as a full repaint
    };
    return &w->now_frame;
}

// --- input -----------------------------------------------------------------

// systemd-localed writes the session's keyboard here, and it is the same file
// that told the compositor what layout to use. Reading it is how a virtual
// keyboard ends up speaking the layout the user actually types.
//
// Without this the fallback is "us", and on a br-abnt2 desktop that types
// gibberish: the keys arrive, the layout turns them into the wrong characters,
// and nothing anywhere reports an error.
#define KEYBOARD_CONF "/etc/X11/xorg.conf.d/00-keyboard.conf"

static bool read_xkb_option(const char *name, char *out, size_t size) {
    FILE *f = fopen(KEYBOARD_CONF, "r");
    if (!f) return false;

    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\"", name);

    char line[512];
    bool found = false;
    while (fgets(line, sizeof(line), f)) {
        char *at = strstr(line, needle);
        if (!at) continue;

        // Option "XkbLayout" "br"  -- the value is the quoted string after it.
        char *open = strchr(at + strlen(needle), '"');
        if (!open) continue;
        char *close = strchr(open + 1, '"');
        if (!close) continue;

        size_t len = (size_t)(close - open - 1);
        if (len == 0 || len >= size) continue;
        memcpy(out, open + 1, len);
        out[len] = 0;
        found = true;
        break;
    }
    fclose(f);
    return found;
}

// The compositor applies the keymap belonging to the virtual device, not the
// one the user is typing on, so the layout has to be handed over explicitly.
static bool send_keymap(struct wr_wayland *w, const char **error) {
    struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!ctx) { *error = "could not create an xkb context"; return false; }

    // The environment first, because that is the documented way to say it --
    // but a service started by systemd has none of it, which is exactly the
    // case this runs in.
    static char layout[64], model[64], variant[64], options[128];
    const char *env_layout = getenv("XKB_DEFAULT_LAYOUT");

    if (!env_layout || !*env_layout) {
        if (read_xkb_option("XkbLayout", layout, sizeof(layout)))
            env_layout = layout;
    }
    if (!read_xkb_option("XkbModel", model, sizeof(model)))     model[0] = 0;
    if (!read_xkb_option("XkbVariant", variant, sizeof(variant))) variant[0] = 0;
    if (!read_xkb_option("XkbOptions", options, sizeof(options))) options[0] = 0;

    struct xkb_rule_names names = {
        .rules   = getenv("XKB_DEFAULT_RULES"),
        .model   = getenv("XKB_DEFAULT_MODEL")  ?: (model[0] ? model : NULL),
        .layout  = env_layout,
        .variant = getenv("XKB_DEFAULT_VARIANT") ?: (variant[0] ? variant : NULL),
        .options = getenv("XKB_DEFAULT_OPTIONS") ?: (options[0] ? options : NULL),
    };

    fprintf(stderr, "wayrdp: keyboard layout %s%s%s\n",
            names.layout ? names.layout : "us (default)",
            names.model ? ", model " : "", names.model ? names.model : "");

    struct xkb_keymap *keymap = xkb_keymap_new_from_names(ctx, &names,
        XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!keymap) {
        xkb_context_unref(ctx);
        *error = "could not compile a keymap for the virtual keyboard";
        return false;
    }

    char *text = xkb_keymap_get_as_string(keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
    xkb_keymap_unref(keymap);
    xkb_context_unref(ctx);
    if (!text) { *error = "could not serialise the keymap"; return false; }

    size_t size = strlen(text) + 1;
    int fd = anonymous_shm(size);
    if (fd < 0) { free(text); *error = "could not allocate the keymap"; return false; }

    void *map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) { close(fd); free(text); *error = "could not map the keymap"; return false; }
    memcpy(map, text, size);
    munmap(map, size);
    free(text);

    zwp_virtual_keyboard_v1_keymap(w->keyboard,
        WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, (uint32_t)size);
    close(fd);
    return true;
}

static int uinput_open(struct wr_wayland *w) {
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "wayrdp: uinput open failed: %s\n", strerror(errno));
        return -1;
    }
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_KEYBIT, BTN_LEFT);
    ioctl(fd, UI_SET_KEYBIT, BTN_RIGHT);
    ioctl(fd, UI_SET_KEYBIT, BTN_MIDDLE);
    ioctl(fd, UI_SET_EVBIT, EV_REL);
    ioctl(fd, UI_SET_RELBIT, REL_X);
    ioctl(fd, UI_SET_RELBIT, REL_Y);
    ioctl(fd, UI_SET_RELBIT, REL_WHEEL);
    ioctl(fd, UI_SET_RELBIT, REL_HWHEEL);
    ioctl(fd, UI_SET_EVBIT, EV_ABS);
    ioctl(fd, UI_SET_ABSBIT, ABS_X);
    ioctl(fd, UI_SET_ABSBIT, ABS_Y);

    struct uinput_setup setup = {0};
    snprintf(setup.name, UINPUT_MAX_NAME_SIZE, "wayrdp-virtual-pointer");
    setup.id.bustype = BUS_USB;
    setup.id.vendor = 0x1234;
    setup.id.product = 0x5679;

    if (ioctl(fd, UI_DEV_SETUP, &setup) < 0) {
        fprintf(stderr, "wayrdp: uinput setup failed: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    struct uinput_abs_setup abs_x = {0};
    abs_x.code = ABS_X;
    abs_x.absinfo.minimum = 0;
    abs_x.absinfo.maximum = w->width ? w->width : 1920;
    ioctl(fd, UI_ABS_SETUP, &abs_x);

    struct uinput_abs_setup abs_y = {0};
    abs_y.code = ABS_Y;
    abs_y.absinfo.minimum = 0;
    abs_y.absinfo.maximum = w->height ? w->height : 1080;
    ioctl(fd, UI_ABS_SETUP, &abs_y);

    if (ioctl(fd, UI_DEV_CREATE) < 0) {
        fprintf(stderr, "wayrdp: uinput create failed: %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    fprintf(stderr, "wayrdp: uinput virtual pointer created (%ux%u)\n", w->width, w->height);
    return fd;
}

static void uinput_emit(int fd, uint16_t type, uint16_t code, int32_t value) {
    struct input_event ie = {0};
    ie.type = type; ie.code = code; ie.value = value;
    (void)write(fd, &ie, sizeof(ie));
}

bool wr_input_open(struct wr_wayland *w, const char **error) {
    if (!w->keyboard_manager) {
        *error = "compositor does not offer virtual-keyboard "
                 "(without it a remote keyboard cannot type)";
        return false;
    }

    if (w->pointer_manager) {
        w->pointer = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(
            w->pointer_manager, w->seat);
    } else {
        w->uinput_fd = uinput_open(w);
        if (w->uinput_fd < 0) {
            *error = "compositor does not offer wlr-virtual-pointer and uinput fallback failed";
            return false;
        }
        w->use_uinput = true;
        fprintf(stderr, "wayrdp: using uinput for pointer injection\n");
    }

    w->keyboard = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(
        w->keyboard_manager, w->seat);

    if (!send_keymap(w, error)) return false;

    wl_display_roundtrip(w->display);
    return true;
}

void wr_pointer_motion(struct wr_wayland *w, uint32_t x, uint32_t y) {
    if (w->use_uinput && w->uinput_fd >= 0) {
        uinput_emit(w->uinput_fd, EV_ABS, ABS_X, x);
        uinput_emit(w->uinput_fd, EV_ABS, ABS_Y, y);
        uinput_emit(w->uinput_fd, EV_SYN, SYN_REPORT, 0);
        return;
    }
    if (!w->pointer) return;
    // Absolute, in output pixels: RDP sends a position, not a delta, and
    // converting to deltas would accumulate error and fight pointer
    // acceleration on the way through.
    zwlr_virtual_pointer_v1_motion_absolute(w->pointer, now_ms(), x, y,
                                            w->width, w->height);
    zwlr_virtual_pointer_v1_frame(w->pointer);
    wl_display_flush(w->display);
}

void wr_pointer_button(struct wr_wayland *w, uint32_t button, bool pressed) {
    if (w->use_uinput && w->uinput_fd >= 0) {
        uinput_emit(w->uinput_fd, EV_KEY, button, pressed ? 1 : 0);
        uinput_emit(w->uinput_fd, EV_SYN, SYN_REPORT, 0);
        return;
    }
    if (!w->pointer) return;
    zwlr_virtual_pointer_v1_button(w->pointer, now_ms(), button,
        pressed ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED);
    zwlr_virtual_pointer_v1_frame(w->pointer);
    wl_display_flush(w->display);
}

void wr_pointer_axis(struct wr_wayland *w, bool horizontal, double value) {
    if (w->use_uinput && w->uinput_fd >= 0) {
        int16_t ticks = (int16_t)(value > 0 ? 1 : -1);
        uinput_emit(w->uinput_fd, EV_REL, horizontal ? REL_HWHEEL : REL_WHEEL, ticks);
        uinput_emit(w->uinput_fd, EV_SYN, SYN_REPORT, 0);
        return;
    }
    if (!w->pointer) return;
    zwlr_virtual_pointer_v1_axis(w->pointer, now_ms(),
        horizontal ? WL_POINTER_AXIS_HORIZONTAL_SCROLL : WL_POINTER_AXIS_VERTICAL_SCROLL,
        wl_fixed_from_double(value));
    zwlr_virtual_pointer_v1_frame(w->pointer);
    wl_display_flush(w->display);
}

// xkb's modifier bits, in the order every keymap declares them.
#define MOD_SHIFT   (1u << 0)
#define MOD_CAPS    (1u << 1)
#define MOD_CTRL    (1u << 2)
#define MOD_ALT     (1u << 3)
#define MOD_NUM     (1u << 4)
#define MOD_LOGO    (1u << 6)
#define MOD_ALTGR   (1u << 7)

// Which bit a key holds down while it is held, if any.
static uint32_t modifier_bit(uint32_t keycode) {
    switch (keycode) {
    case KEY_LEFTSHIFT: case KEY_RIGHTSHIFT: return MOD_SHIFT;
    case KEY_LEFTCTRL:  case KEY_RIGHTCTRL:  return MOD_CTRL;
    case KEY_LEFTALT:                        return MOD_ALT;
    case KEY_RIGHTALT:                       return MOD_ALTGR;
    case KEY_LEFTMETA:  case KEY_RIGHTMETA:  return MOD_LOGO;
    default: return 0;
    }
}

// And which bit a key toggles until it is pressed again.
static uint32_t lock_bit(uint32_t keycode) {
    switch (keycode) {
    case KEY_CAPSLOCK: return MOD_CAPS;
    case KEY_NUMLOCK:  return MOD_NUM;
    default: return 0;
    }
}

void wr_keyboard_key(struct wr_wayland *w, uint32_t keycode, bool pressed) {
    if (!w->keyboard) return;

    // The compositor does not derive the modifier state from a virtual
    // keyboard's key events -- the protocol expects the client to say what is
    // held. Without this, keys type and shortcuts do not: SUPER+W arrived as a
    // W with nobody holding SUPER, and the compositor had no binding for W.
    uint32_t bit = modifier_bit(keycode);
    uint32_t lock = lock_bit(keycode);
    uint32_t before_depressed = w->mods_depressed, before_locked = w->mods_locked;

    if (bit) {
        if (pressed) w->mods_depressed |= bit;
        else         w->mods_depressed &= ~bit;
    }
    if (lock && pressed) w->mods_locked ^= lock;

    // Modifiers first on the way down, so the key lands with them held; and
    // after on the way up, so the release is not seen bare.
    if (pressed && (w->mods_depressed != before_depressed || w->mods_locked != before_locked))
        zwp_virtual_keyboard_v1_modifiers(w->keyboard, w->mods_depressed, 0, w->mods_locked, 0);

    zwp_virtual_keyboard_v1_key(w->keyboard, now_ms(), keycode, pressed ? 1 : 0);

    if (!pressed && w->mods_depressed != before_depressed)
        zwp_virtual_keyboard_v1_modifiers(w->keyboard, w->mods_depressed, 0, w->mods_locked, 0);

    wl_display_flush(w->display);
}

void wr_keyboard_modifiers(struct wr_wayland *w, uint32_t depressed, uint32_t locked) {
    if (!w->keyboard) return;
    zwp_virtual_keyboard_v1_modifiers(w->keyboard, depressed, 0, locked, 0);
    wl_display_flush(w->display);
}
