// rdp.c — an RDP server that serves the session it is running inside.
//
// One peer at a time on purpose. This is a desktop letting its owner back in,
// not a terminal server, and a second connection to the same screen is a
// question ("whose input wins?") nobody asked to be answered.

#define _GNU_SOURCE
#include <errno.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <freerdp/freerdp.h>
#include <freerdp/listener.h>
#include <freerdp/peer.h>
#include <freerdp/input.h>
#include <freerdp/update.h>
#include <freerdp/constants.h>
#include <freerdp/codec/color.h>
#include <freerdp/codec/rfx.h>
#include <freerdp/crypto/certificate.h>
#include <freerdp/crypto/privatekey.h>
#include <freerdp/channels/audin.h>
#include <freerdp/channels/channels.h>
#include <freerdp/channels/drdynvc.h>
#include <freerdp/channels/rdpsnd.h>
#include <freerdp/channels/wtsvc.h>
#include <freerdp/codec/audio.h>
#include <freerdp/server/audin.h>
#include <freerdp/server/rdpsnd.h>
#include <winpr/sspi.h>
#include <winpr/stream.h>
#include <winpr/synch.h>
#include <winpr/sysinfo.h>
#include <winpr/wtsapi.h>

#include "audio.h"
#include "password.h"
#include "rdp.h"

struct wr_server {
    freerdp_listener *listener;
    struct wr_wayland *wayland;
    struct wr_config config;

    freerdp_peer *peer;          // the one client, or NULL
    bool peer_activated;
    bool peer_authenticated;

    // Sound. The two channels belong to the peer and are torn down with it;
    // the PipeWire side belongs to the process and outlives every client, so a
    // reconnect does not have to find the audio server again.
    struct wr_audio *audio;
    HANDLE vcm;
    RdpsndServerContext *rdpsnd;
    audin_server_context *audin;
    volatile bool speaker_ready;   // set on the sound channel's own thread
    bool mic_tried;

    // RemoteFX encodes from a 32-bit surface, so the compositor's 24-bit
    // frames are kept mirrored in the shape the codec wants. Only damaged
    // rectangles are refreshed into it, which is the whole point of damage.
    uint8_t *mirror;
    size_t mirror_size;
    uint32_t mirror_width, mirror_height;

    RFX_CONTEXT *rfx;
    wStream *stream;
};

typedef struct {
    rdpContext context;
    struct wr_server *server;
} wrPeerContext;

// --- keyboard --------------------------------------------------------------

// RDP carries PC/AT set 1 scancodes, and evdev numbers the main block with the
// same values: A is 0x1E in set 1 and KEY_A is 30. The familiar "+8" is the
// X11 keycode offset, and X11 is not in this path at all -- adding it here
// shifted every key by eight, so A typed L and nothing came out right.
//
// Only the keys behind the E0 prefix need naming, because there the two sets
// stop agreeing.
static uint32_t extended_scancode_to_evdev(uint8_t code) {
    switch (code) {
    case 0x1C: return KEY_KPENTER;
    case 0x1D: return KEY_RIGHTCTRL;
    case 0x35: return KEY_KPSLASH;
    case 0x38: return KEY_RIGHTALT;
    case 0x47: return KEY_HOME;
    case 0x48: return KEY_UP;
    case 0x49: return KEY_PAGEUP;
    case 0x4B: return KEY_LEFT;
    case 0x4D: return KEY_RIGHT;
    case 0x4F: return KEY_END;
    case 0x50: return KEY_DOWN;
    case 0x51: return KEY_PAGEDOWN;
    case 0x52: return KEY_INSERT;
    case 0x53: return KEY_DELETE;
    case 0x5B: return KEY_LEFTMETA;
    case 0x5C: return KEY_RIGHTMETA;
    case 0x5D: return KEY_COMPOSE;
    default:   return 0;
    }
}

static BOOL peer_keyboard(rdpInput *input, UINT16 flags, UINT8 code) {
    wrPeerContext *ctx = (wrPeerContext *)input->context;
    struct wr_server *s = ctx->server;
    if (!s->peer_activated) return TRUE;

    uint32_t key = (flags & KBD_FLAGS_EXTENDED)
        ? extended_scancode_to_evdev(code)
        : (uint32_t)code;

    // WAYRDP_DEBUG_INPUT=1 prints what arrives and what it becomes. Keyboard
    // faults look identical from the outside -- nothing types -- and the only
    // way to tell "the client sent nothing" from "the mapping is wrong" is to
    // see the numbers.
    static int debug = -1;
    if (debug < 0) debug = getenv("WAYRDP_DEBUG_INPUT") ? 1 : 0;
    if (debug)
        fprintf(stderr, "wayrdp: key scancode 0x%02X%s -> evdev %u %s\n",
                code, (flags & KBD_FLAGS_EXTENDED) ? " (extended)" : "", key,
                (flags & KBD_FLAGS_RELEASE) ? "up" : "down");

    if (key == 0) return TRUE;
    wr_keyboard_key(s->wayland, key, !(flags & KBD_FLAGS_RELEASE));
    return TRUE;
}

// --- pointer ---------------------------------------------------------------

static BOOL peer_mouse(rdpInput *input, UINT16 flags, UINT16 x, UINT16 y) {
    wrPeerContext *ctx = (wrPeerContext *)input->context;
    struct wr_server *s = ctx->server;
    if (!s->peer_activated) return TRUE;

    if (flags & PTR_FLAGS_MOVE)
        wr_pointer_motion(s->wayland, x, y);

    // A button event carries the position too, and clients send the two
    // together: moving first means the click lands where the user aimed rather
    // than wherever the pointer was left.
    if (flags & (PTR_FLAGS_BUTTON1 | PTR_FLAGS_BUTTON2 | PTR_FLAGS_BUTTON3)) {
        wr_pointer_motion(s->wayland, x, y);

        uint32_t button = 0;
        if (flags & PTR_FLAGS_BUTTON1) button = BTN_LEFT;
        else if (flags & PTR_FLAGS_BUTTON2) button = BTN_RIGHT;
        else if (flags & PTR_FLAGS_BUTTON3) button = BTN_MIDDLE;

        wr_pointer_button(s->wayland, button, (flags & PTR_FLAGS_DOWN) != 0);
    }

    if (flags & PTR_FLAGS_WHEEL) {
        // The low byte is the distance, and RDP's 120 units are one notch.
        int delta = flags & 0xFF;
        if (flags & PTR_FLAGS_WHEEL_NEGATIVE) delta = -delta;
        wr_pointer_axis(s->wayland, false, -(double)delta / 120.0 * 15.0);
    }
    return TRUE;
}

// --- authentication --------------------------------------------------------

// Constant time, and length-aware: the identity strings arrive with a length
// and no promise of a terminator, and a comparison that returns early on the
// first wrong byte tells a patient attacker how much of the password was right.
static bool equal_secret(const char *given, UINT32 given_len, const char *expected) {
    if (!given || !expected) return false;

    size_t expected_len = strlen(expected);
    unsigned char diff = (given_len == expected_len) ? 0 : 1;

    size_t n = given_len < expected_len ? given_len : expected_len;
    for (size_t i = 0; i < n; i++)
        diff |= (unsigned char)(given[i] ^ expected[i]);

    return diff == 0;
}

// A configured hash wins over a stored secret, so a config that carries both
// migrates on its own and a plaintext one keeps working until it is rewritten.
static bool password_matches(const struct wr_config *config,
                             const char *given, size_t given_len) {
    if (config->password_hash[0])
        return wr_password_verify(given, given_len, config->password_hash);
    return equal_secret(given, (UINT32)given_len, config->password);
}

// Fires once the tunnel is up. With NLA the identity is filled in and this is
// the check; with a plain TLS tunnel -- which is what this server offers --
// it fires *before* the client has sent anything, and the identity is empty.
// Refusing here would refuse everyone, so an empty identity is passed through
// and the real check happens at PostConnect, where the Client Info PDU has
// arrived. That empty username in the log was this, not a client fault.
static BOOL peer_logon(freerdp_peer *peer, const SEC_WINNT_AUTH_IDENTITY *identity,
                       BOOL automatic) {
    wrPeerContext *ctx = (wrPeerContext *)peer->context;
    struct wr_server *s = ctx->server;
    (void)automatic;

    const char *user = NULL, *domain = NULL, *password = NULL;
    UINT32 user_len = 0, domain_len = 0, password_len = 0;

    if (!sspi_GetAuthIdentityUserDomainA(identity, &user, &user_len, &domain, &domain_len) ||
        !sspi_GetAuthIdentityPasswordA(identity, &password, &password_len) ||
        user_len == 0) {
        // Anonymous tunnel: nothing to check yet.
        (void)s;
        return TRUE;
    }

    bool ok = equal_secret(user, user_len, s->config.username) &&
              password_matches(&s->config, password, password_len);

    fprintf(stderr, "wayrdp: %s for '%.*s' from %s\n",
            ok ? "login accepted" : "login refused",
            (int)user_len, user ? user : "", peer->hostname);

    s->peer_authenticated = ok;
    return ok ? TRUE : FALSE;
}

// --- frames ----------------------------------------------------------------

// Bring the mirror up to date for one rectangle.
//
// The compositor hands over BG24 and RemoteFX wants BGRX32, so the conversion
// has to happen somewhere. Doing it per damaged rectangle rather than per frame
// is what keeps a blinking cursor from costing a full-screen repack.
//
// The format is chosen once per rectangle and the byte order is resolved by a
// tight loop: the compositor offers a single format for the whole session, so
// this runs millions of times per frame and must not re-decide per pixel. The
// old version called wr_read_rgb -- a function in another translation unit, so
// never inlined -- for every pixel, with a switch inside it.
static void mirror_rect(struct wr_server *s, const struct wr_frame *f,
                        int32_t x, int32_t y, int32_t width, int32_t height) {
    const int bpp = f->bytes_per_pixel;
    const size_t src_stride = f->stride;
    const size_t dst_stride = (size_t)f->width * 4;

    // On the wire BGR888/XRGB8888/ARGB8888 are already B,G,R in memory, which
    // is what RemoteFX wants; those copy straight across. XBGR/ABGR8888 are
    // R,G,B and need the red and blue halves exchanged.
    const bool mem_bgr = wr_format_is_bgr(f->format);

    for (int32_t row = 0; row < height; row++) {
        const uint8_t *src = f->pixels + (size_t)(y + row) * src_stride
                           + (size_t)x * (size_t)bpp;
        uint8_t *dst = s->mirror + (size_t)(y + row) * dst_stride
                     + (size_t)x * 4;

        if (bpp == 4) {
            for (int32_t col = 0; col < width; col++) {
                uint32_t px;
                memcpy(&px, src, sizeof(px));
                if (!mem_bgr)
                    px = (px & 0xFF00FF00u) | ((px & 0x000000FFu) << 16)
                       | ((px & 0x00FF0000u) >> 16);
                px |= 0xFF000000u;              // RemoteFX ignores this byte
                memcpy(dst, &px, sizeof(px));
                src += 4;
                dst += 4;
            }
        } else if (mem_bgr) {
            for (int32_t col = 0; col < width; col++) {
                uint32_t px = 0xFF000000u
                            | (uint32_t)src[0]
                            | ((uint32_t)src[1] << 8)
                            | ((uint32_t)src[2] << 16);
                memcpy(dst, &px, sizeof(px));
                src += 3;
                dst += 4;
            }
        } else {
            for (int32_t col = 0; col < width; col++) {
                uint32_t px = 0xFF000000u
                            | (uint32_t)src[2]
                            | ((uint32_t)src[1] << 8)
                            | ((uint32_t)src[0] << 16);
                memcpy(dst, &px, sizeof(px));
                src += 3;
                dst += 4;
            }
        }
    }
}

static bool clamp_rect(const struct wr_frame *f, int32_t *x, int32_t *y,
                       int32_t *width, int32_t *height) {
    if (*x < 0) { *width += *x; *x = 0; }
    if (*y < 0) { *height += *y; *y = 0; }
    if (*x + *width > (int32_t)f->width)   *width  = (int32_t)f->width - *x;
    if (*y + *height > (int32_t)f->height) *height = (int32_t)f->height - *y;
    return *width > 0 && *height > 0;
}

// Surface bits carrying a RemoteFX message.
//
// Raw pixels in a surface command (codec NONE) are legal on paper and refused
// in practice: the first attempt sent them, the send failed, and the client
// crashed. Clients negotiate a codec and RemoteFX is the one they all have.
static bool send_frame(struct wr_server *s, const struct wr_frame *f, bool full) {
    if (!s->peer || !s->peer_activated || !s->rfx) return true;

    size_t needed = (size_t)f->width * f->height * 4;
    if (needed != s->mirror_size) {
        uint8_t *grown = realloc(s->mirror, needed);
        if (!grown) return false;
        s->mirror = grown;
        s->mirror_size = needed;
        s->mirror_width = f->width;
        s->mirror_height = f->height;
        full = true;    // a resized mirror holds nothing the client has seen
        // The encoder is sized to the same resolution: a changed output needs a
        // fresh context or rfx_compose_message refuses the frame.
        if (!rfx_context_reset(s->rfx, f->width, f->height)) return false;
    }

    RFX_RECT rects[WR_MAX_DAMAGE];
    size_t count = 0;

    if (!full && !f->damage_overflowed && f->damage_count == 0)
        return true;    // the frame carries no damage: nothing changed
    if (full || f->damage_overflowed) {
        mirror_rect(s, f, 0, 0, (int32_t)f->width, (int32_t)f->height);
        rects[count++] = (RFX_RECT){ 0, 0, (UINT16)f->width, (UINT16)f->height };
    } else {
        for (int i = 0; i < f->damage_count; i++) {
            int32_t x = f->damage[i].x, y = f->damage[i].y;
            int32_t w = f->damage[i].width, h = f->damage[i].height;
            if (!clamp_rect(f, &x, &y, &w, &h)) continue;
            mirror_rect(s, f, x, y, w, h);
            rects[count++] = (RFX_RECT){ (UINT16)x, (UINT16)y, (UINT16)w, (UINT16)h };
        }
        if (count == 0) return true;
    }

    Stream_SetPosition(s->stream, 0);
    if (!rfx_compose_message(s->rfx, s->stream, rects, count, s->mirror,
                             f->width, f->height, (UINT32)f->width * 4))
        return false;

    rdpSettings *settings = s->peer->context->settings;

    SURFACE_BITS_COMMAND cmd = { 0 };
    cmd.cmdType = CMDTYPE_SET_SURFACE_BITS;
    cmd.destLeft = 0;
    cmd.destTop = 0;
    cmd.destRight = f->width;
    cmd.destBottom = f->height;
    cmd.bmp.bpp = 32;
    cmd.bmp.codecID = (UINT16)freerdp_settings_get_uint32(settings, FreeRDP_RemoteFxCodecId);
    cmd.bmp.width = (UINT16)f->width;
    cmd.bmp.height = (UINT16)f->height;
    cmd.bmp.bitmapDataLength = (UINT32)Stream_GetPosition(s->stream);
    cmd.bmp.bitmapData = Stream_Buffer(s->stream);
    cmd.skipCompression = TRUE;

    rdpUpdate *update = s->peer->context->update;
    if (!update->BeginPaint(s->peer->context)) return false;
    if (!update->SurfaceBits(s->peer->context, &cmd)) return false;
    return update->EndPaint(s->peer->context);
}

// --- sound -----------------------------------------------------------------

// What the ring buffer holds, and what is fed to the channel: 16-bit PCM at
// 48 kHz in stereo. The client never sees this one; it is the source the
// channel converts from.
static AUDIO_FORMAT speaker_format = {
    .wFormatTag      = WAVE_FORMAT_PCM,
    .nChannels       = WR_SPEAKER_CHANNELS,
    .nSamplesPerSec  = WR_SPEAKER_RATE,
    .nAvgBytesPerSec = WR_SPEAKER_RATE * WR_SPEAKER_FRAME,
    .nBlockAlign     = WR_SPEAKER_FRAME,
    .wBitsPerSample  = 16,
    .cbSize          = 0,
    .data            = NULL,
};

// Two formats offered, AAC first, and the order is not about bandwidth.
//
// FreeRDP built against ffmpeg -- which is every Linux client worth naming --
// decodes 16-bit WAVE_FORMAT_PCM as AV_CODEC_ID_PCM_U16LE. RDP's 16-bit PCM is
// signed, so every sample comes out with its sign bit flipped: silence plays as
// full-scale, and anything else wraps. Measured here as a constant -32768 on a
// stream this server had put +16384 into.
//
// AAC goes through a codec that agrees with itself in both directions, so it is
// offered first and taken whenever the client has it. PCM stays as the fallback
// for the clients that do not, where it is also correct.
static const AUDIO_FORMAT speaker_offers[] = {
    { WAVE_FORMAT_AAC_MS, WR_SPEAKER_CHANNELS, WR_SPEAKER_RATE, 24000, 4, 16, 0, NULL },
    { WAVE_FORMAT_PCM, WR_SPEAKER_CHANNELS, WR_SPEAKER_RATE,
      WR_SPEAKER_RATE * WR_SPEAKER_FRAME, WR_SPEAKER_FRAME, 16, 0, NULL },
};

// Inward the client chooses, because the microphone is its hardware and not
// ours. All four are PCM on purpose: a compressed format would arrive needing a
// decoder this server does not have, and the negotiation is the honest place to
// say so rather than the first packet.
static const AUDIO_FORMAT mic_formats[] = {
    { WAVE_FORMAT_PCM, 1, 48000, 48000 * 2, 2, 16, 0, NULL },
    { WAVE_FORMAT_PCM, 2, 48000, 48000 * 4, 4, 16, 0, NULL },
    { WAVE_FORMAT_PCM, 1, 44100, 44100 * 2, 2, 16, 0, NULL },
    { WAVE_FORMAT_PCM, 2, 44100, 44100 * 4, 4, 16, 0, NULL },
};

// 20 ms of stereo. The loop comes back about this often, so one block per turn
// is the natural size, and small blocks are also what keeps a client's buffer
// from being handed a second of audio at once after a slow frame.
#define SPEAKER_CHUNK (960 * WR_SPEAKER_FRAME)

// WAYRDP_DEBUG_AUDIO=1 says how much moved, each way, once a second. Silence
// has the same shape as a broken channel from the outside -- both are nothing
// coming out of the speaker -- and this is what tells the two apart.
static bool audio_debug(void) {
    static int on = -1;
    if (on < 0) on = getenv("WAYRDP_DEBUG_AUDIO") ? 1 : 0;
    return on == 1;
}

// The loudest sample in the block, so that "nothing is coming through" can be
// told from "silence is coming through", which look the same in a byte count.
static int peak_of(const void *buf, size_t bytes) {
    const int16_t *s = buf;
    int peak = 0;
    for (size_t i = 0; i < bytes / 2; i++) {
        int v = s[i] < 0 ? -s[i] : s[i];
        if (v > peak) peak = v;
    }
    return peak;
}

static void audio_tally(const char *direction, const void *buf, size_t bytes) {
    static size_t out_bytes, in_bytes;
    static int out_peak, in_peak;
    static DWORD out_at, in_at;

    bool out = direction[0] == 'o';
    size_t *total = out ? &out_bytes : &in_bytes;
    int *peak = out ? &out_peak : &in_peak;
    DWORD *at = out ? &out_at : &in_at;

    *total += bytes;
    int p = peak_of(buf, bytes);
    if (p > *peak) *peak = p;

    DWORD now = GetTickCount();
    if (*at == 0) *at = now;
    if (now - *at < 1000) return;

    fprintf(stderr, "wayrdp: audio %s %zu bytes in %u ms, peak %d\n",
            direction, *total, (unsigned)(now - *at), *peak);
    *total = 0;
    *peak = 0;
    *at = now;
}

// Fires on the sound channel's own thread, once the client has said what it can
// play. Nothing is captured until then: a desktop nobody is listening to should
// not be holding a capture stream open.
static void speaker_activated(RdpsndServerContext *snd) {
    struct wr_server *s = snd->data;

    s->speaker_ready = false;

    if (audio_debug()) {
        fprintf(stderr, "wayrdp: client version 0x%04X, caps 0x%08X, %u format%s\n",
                snd->clientVersion, snd->capsFlags, snd->num_client_formats,
                snd->num_client_formats == 1 ? "" : "s");
        for (UINT16 i = 0; i < snd->num_client_formats; i++) {
            const AUDIO_FORMAT *f = &snd->client_formats[i];
            fprintf(stderr, "wayrdp:   [%u] tag 0x%04X %u Hz %u ch %u bits\n",
                    i, f->wFormatTag, f->nSamplesPerSec, f->nChannels,
                    f->wBitsPerSample);
        }
    }

    // Anything the client has that is not raw PCM, first; raw PCM only when
    // that is all it has. See speaker_offers for why the order matters.
    UINT16 pick = snd->num_client_formats;
    for (UINT16 i = 0; i < snd->num_client_formats; i++) {
        if (snd->client_formats[i].wFormatTag == WAVE_FORMAT_PCM) continue;
        pick = i;
        break;
    }
    if (pick == snd->num_client_formats) {
        for (UINT16 i = 0; i < snd->num_client_formats; i++) {
            if (snd->client_formats[i].wFormatTag != WAVE_FORMAT_PCM) continue;
            pick = i;
            break;
        }
    }

    if (pick == snd->num_client_formats ||
        snd->SelectFormat(snd, pick) != CHANNEL_RC_OK) {
        fprintf(stderr, "wayrdp: the client offered no format this can send; "
                        "no sound\n");
        return;
    }

    const char *error = NULL;
    if (!wr_speaker_start(s->audio, &error)) {
        fprintf(stderr, "wayrdp: no sound -- %s\n", error);
        return;
    }

    // Unattenuated, and said out loud. MS-RDPEA has the server set the level,
    // and a client that is never told picks its own.
    if (snd->SetVolume(snd, 0xFFFF, 0xFFFF) != CHANNEL_RC_OK)
        fprintf(stderr, "wayrdp: the client refused a volume\n");

    s->speaker_ready = true;
    fprintf(stderr, "wayrdp: sound on, %s\n",
            snd->client_formats[pick].wFormatTag == WAVE_FORMAT_AAC_MS
                ? "AAC" : "16-bit PCM");
}

// Called once per turn round the loop. Everything the desktop has played since
// the last turn, in blocks, oldest first.
static void pump_speaker(struct wr_server *s) {
    if (!s->speaker_ready || !s->rdpsnd) return;

    uint8_t chunk[SPEAKER_CHUNK];
    for (;;) {
        size_t got = wr_speaker_read(s->audio, chunk, sizeof(chunk));
        if (got < WR_SPEAKER_FRAME) return;

        if (audio_debug()) audio_tally("out", chunk, got);

        if (s->rdpsnd->SendSamples(s->rdpsnd, chunk, got / WR_SPEAKER_FRAME,
                                   (UINT16)GetTickCount()) != CHANNEL_RC_OK) {
            fprintf(stderr, "wayrdp: the sound channel refused a block; sound off\n");
            s->speaker_ready = false;
            return;
        }
        if (got < sizeof(chunk)) return;   // the ring is empty; a quiet desktop
    }
}

static void open_sound(struct wr_server *s, freerdp_peer *peer) {
    if (s->rdpsnd || !s->audio) return;

    if (!WTSVirtualChannelManagerIsChannelJoined(s->vcm, RDPSND_CHANNEL_NAME)) {
        fprintf(stderr, "wayrdp: the client did not ask for sound\n");
        return;
    }

    s->rdpsnd = rdpsnd_server_context_new(s->vcm);
    if (!s->rdpsnd) return;

    // server_formats is freed by the channel, so it has to be its own
    // allocation; src_format is only ever read, and points at the static.
    size_t offers = sizeof(speaker_offers) / sizeof(speaker_offers[0]);
    AUDIO_FORMAT *offered = calloc(offers, sizeof(*offered));
    if (!offered) { rdpsnd_server_context_free(s->rdpsnd); s->rdpsnd = NULL; return; }
    memcpy(offered, speaker_offers, offers * sizeof(*offered));

    s->rdpsnd->data = s;
    s->rdpsnd->rdpcontext = peer->context;
    s->rdpsnd->server_formats = offered;
    s->rdpsnd->num_server_formats = offers;
    s->rdpsnd->src_format = &speaker_format;
    s->rdpsnd->latency = 50;
    s->rdpsnd->Activated = speaker_activated;

    // Its own thread: the channel spends its life waiting for block confirms,
    // and giving it one keeps that wait out of the frame loop.
    if (s->rdpsnd->Initialize(s->rdpsnd, TRUE) != CHANNEL_RC_OK) {
        fprintf(stderr, "wayrdp: could not open the sound channel\n");
        rdpsnd_server_context_free(s->rdpsnd);
        s->rdpsnd = NULL;
    }
}

// The client agreed a format for its microphone; publish a node in that exact
// shape so nothing between here and the application has to resample.
static UINT mic_open_reply(audin_server_context *audin,
                           const SNDIN_OPEN_REPLY *reply) {
    struct wr_server *s = audin->userdata;

    if (reply->Result != 0) {
        fprintf(stderr, "wayrdp: the client refused to open its microphone\n");
        return CHANNEL_RC_OK;
    }

    const AUDIO_FORMAT *f = audin_server_get_negotiated_format(audin);
    if (!f) return CHANNEL_RC_OK;

    const char *error = NULL;
    if (!wr_mic_start(s->audio, f->nSamplesPerSec, f->nChannels, &error)) {
        fprintf(stderr, "wayrdp: no microphone -- %s\n", error);
        return CHANNEL_RC_OK;
    }

    fprintf(stderr, "wayrdp: microphone on, %u Hz %u channel%s, as "
                    "\"Remote microphone\"\n",
            f->nSamplesPerSec, f->nChannels, f->nChannels == 1 ? "" : "s");
    return CHANNEL_RC_OK;
}

// On the channel's thread. The stream holds raw PCM in the negotiated format,
// which is why only PCM was ever offered.
static UINT mic_data(audin_server_context *audin, const SNDIN_DATA *data) {
    struct wr_server *s = audin->userdata;

    size_t n = Stream_GetRemainingLength(data->Data);
    if (n) wr_mic_write(s->audio, Stream_ConstPointer(data->Data), n);
    if (audio_debug()) audio_tally("in ", Stream_ConstPointer(data->Data), n);
    return CHANNEL_RC_OK;
}

// Unlike sound, this one cannot be opened at PostConnect: it rides on the
// dynamic channel, and that is still being set up. The loop asks each turn
// until it is ready, which is once.
static void open_microphone(struct wr_server *s) {
    if (s->audin || s->mic_tried || !s->audio || !s->peer) return;

    BYTE state = WTSVirtualChannelManagerGetDrdynvcState(s->vcm);
    if (state != DRDYNVC_STATE_READY) {
        if (audio_debug()) {
            static BYTE said = 0xFF;
            if (state != said) {
                fprintf(stderr, "wayrdp: dynamic channel state %u, drdynvc %s, "
                                "client audio capture %s\n", state,
                        WTSVirtualChannelManagerIsChannelJoined(
                            s->vcm, DRDYNVC_SVC_CHANNEL_NAME) ? "joined" : "absent",
                        freerdp_settings_get_bool(s->peer->context->settings,
                            FreeRDP_AudioCapture) ? "on" : "off");
                said = state;
            }
        }
        return;
    }

    s->mic_tried = true;

    s->audin = audin_server_context_new(s->vcm);
    if (!s->audin) {
        fprintf(stderr, "wayrdp: could not create the microphone channel\n");
        return;
    }

    s->audin->userdata = s;
    s->audin->rdpcontext = s->peer->context;
    s->audin->serverVersion = SNDIN_VERSION_Version_2;
    s->audin->OpenReply = mic_open_reply;
    s->audin->Data = mic_data;

    size_t count = sizeof(mic_formats) / sizeof(mic_formats[0]);
    if (!audin_server_set_formats(s->audin, (SSIZE_T)count, mic_formats)) {
        fprintf(stderr, "wayrdp: the microphone formats were refused\n");
        audin_server_context_free(s->audin);
        s->audin = NULL;
        return;
    }
    if (!s->audin->Open(s->audin)) {
        // A client with no microphone to share is the common case, not a fault.
        fprintf(stderr, "wayrdp: the client has no microphone to share\n");
        audin_server_context_free(s->audin);
        s->audin = NULL;
        return;
    }
    if (audio_debug()) fprintf(stderr, "wayrdp: microphone channel opened\n");
}

static void close_audio(struct wr_server *s) {
    if (s->audin) {
        (void)s->audin->Close(s->audin);
        audin_server_context_free(s->audin);
        s->audin = NULL;
    }
    if (s->rdpsnd) {
        (void)s->rdpsnd->Close(s->rdpsnd);
        rdpsnd_server_context_free(s->rdpsnd);
        s->rdpsnd = NULL;
    }
    wr_speaker_stop(s->audio);
    wr_mic_stop(s->audio);
    s->speaker_ready = false;
    s->mic_tried = false;
}

// --- peer lifecycle --------------------------------------------------------

static BOOL peer_capabilities(freerdp_peer *peer) {
    rdpSettings *settings = peer->context->settings;

    // Without these there is nowhere to put the pixels on this path, and a
    // client told plainly beats a client showing a white window.
    if (!freerdp_settings_get_bool(settings, FreeRDP_SurfaceCommandsEnabled)) {
        fprintf(stderr, "wayrdp: client does not support surface commands; refused\n");
        return FALSE;
    }
    if (!freerdp_settings_get_bool(settings, FreeRDP_RemoteFxCodec)) {
        fprintf(stderr, "wayrdp: client does not offer RemoteFX; refused\n");
        return FALSE;
    }
    return TRUE;
}

static BOOL peer_post_connect(freerdp_peer *peer) {
    wrPeerContext *ctx = (wrPeerContext *)peer->context;
    struct wr_server *s = ctx->server;
    rdpSettings *settings = peer->context->settings;

    // The Client Info PDU has landed by now, so this is where the credentials
    // actually are. Refusing here still happens before any pixel is sent.
    if (!s->peer_authenticated) {
        const char *user = freerdp_settings_get_string(settings, FreeRDP_Username);
        const char *password = freerdp_settings_get_string(settings, FreeRDP_Password);

        bool ok = user && password &&
                  equal_secret(user, (UINT32)strlen(user), s->config.username) &&
                  password_matches(&s->config, password, strlen(password));

        fprintf(stderr, "wayrdp: %s for '%s' from %s\n",
                ok ? "login accepted" : "login refused",
                user ? user : "(none)", peer->hostname);

        if (!ok) return FALSE;
        s->peer_authenticated = true;
    }

    // The static channels are joined by now, so this is the first moment the
    // question "did this client ask for sound?" has an answer.
    open_sound(s, peer);

    // The client asked for a size; it gets the screen's instead. Scaling the
    // desktop to fit a window is a feature, not a default, and a wrong-sized
    // framebuffer is how remote desktops end up blurry.
    uint32_t width = wr_width(s->wayland), height = wr_height(s->wayland);
    if (freerdp_settings_get_uint32(settings, FreeRDP_DesktopWidth) != width ||
        freerdp_settings_get_uint32(settings, FreeRDP_DesktopHeight) != height) {
        if (!freerdp_settings_set_uint32(settings, FreeRDP_DesktopWidth, width) ||
            !freerdp_settings_set_uint32(settings, FreeRDP_DesktopHeight, height))
            return FALSE;
        return peer->context->update->DesktopResize(peer->context);
    }
    return TRUE;
}

static BOOL peer_activate(freerdp_peer *peer) {
    wrPeerContext *ctx = (wrPeerContext *)peer->context;
    struct wr_server *s = ctx->server;

    uint32_t width = wr_width(s->wayland), height = wr_height(s->wayland);
    if (!s->rfx) {
        s->rfx = rfx_context_new_ex(TRUE, THREADING_FLAGS_DISABLE_THREADS);
        s->stream = Stream_New(NULL, 4 * 1024 * 1024);
        if (!s->rfx || !s->stream) {
            fprintf(stderr, "wayrdp: could not create the RemoteFX encoder\n");
            return FALSE;
        }
        rfx_context_set_pixel_format(s->rfx, PIXEL_FORMAT_BGRX32);
    }
    if (!rfx_context_reset(s->rfx, width, height)) return FALSE;

    s->peer_activated = true;
    fprintf(stderr, "wayrdp: %s connected, serving %ux%u\n",
            peer->hostname, width, height);

    // The client has an empty window and nothing is going to change on a still
    // desktop, so the first paint has to be asked for rather than waited for.
    const struct wr_frame *f = wr_capture_now(s->wayland, 2000);
    if (f && !send_frame(s, f, true))
        fprintf(stderr, "wayrdp: could not send the first frame\n");
    return TRUE;
}

static BOOL peer_context_new(freerdp_peer *peer, rdpContext *context) {
    (void)peer;
    wrPeerContext *ctx = (wrPeerContext *)context;
    ctx->server = NULL;   // filled in by the accept handler
    return TRUE;
}

static void peer_context_free(freerdp_peer *peer, rdpContext *context) {
    (void)peer; (void)context;
}

static BOOL peer_accepted(freerdp_listener *listener, freerdp_peer *peer) {
    struct wr_server *s = listener->info;

    if (s->peer) {
        // One screen, one keyboard: a second client would fight the first for
        // both. Refusing is clearer than sharing.
        fprintf(stderr, "wayrdp: refusing %s, already serving %s\n",
                peer->hostname, s->peer->hostname);
        return FALSE;
    }

    peer->ContextSize = sizeof(wrPeerContext);
    peer->ContextNew = peer_context_new;
    peer->ContextFree = peer_context_free;
    if (!freerdp_peer_context_new(peer)) return FALSE;

    wrPeerContext *ctx = (wrPeerContext *)peer->context;
    ctx->server = s;

    // Everything that is not pixels or keystrokes rides a virtual channel, and
    // nothing routes one without this.
    s->vcm = WTSOpenServerA((LPSTR)peer->context);
    if (!s->vcm || s->vcm == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "wayrdp: could not open the channel manager\n");
        s->vcm = NULL;
        return FALSE;
    }

    rdpSettings *settings = peer->context->settings;

    rdpCertificate *cert = freerdp_certificate_new_from_file(s->config.cert_path);
    rdpPrivateKey *key = freerdp_key_new_from_file(s->config.key_path);
    if (!cert || !key) {
        fprintf(stderr, "wayrdp: could not load the TLS certificate\n");
        return FALSE;
    }
    if (!freerdp_settings_set_pointer_len(settings, FreeRDP_RdpServerCertificate, cert, 1) ||
        !freerdp_settings_set_pointer_len(settings, FreeRDP_RdpServerRsaKey, key, 1))
        return FALSE;

    // TLS, and not NLA: NLA would have the server prove it knows the password
    // hash, which means a SAM file. The credentials arrive inside the TLS
    // tunnel instead and are checked in Logon, which is one moving part fewer
    // and works with every client without a Windows account behind it.
    if (!freerdp_settings_set_bool(settings, FreeRDP_RdpSecurity, FALSE) ||
        !freerdp_settings_set_bool(settings, FreeRDP_TlsSecurity, TRUE) ||
        !freerdp_settings_set_bool(settings, FreeRDP_NlaSecurity, FALSE) ||
        !freerdp_settings_set_uint32(settings, FreeRDP_ColorDepth, 32) ||
        !freerdp_settings_set_bool(settings, FreeRDP_SurfaceCommandsEnabled, TRUE) ||
        !freerdp_settings_set_bool(settings, FreeRDP_RemoteFxCodec, TRUE) ||
        !freerdp_settings_set_bool(settings, FreeRDP_UnicodeInput, TRUE) ||
        // The microphone is a dynamic channel, and a server that does not
        // announce drdynvc never gets offered one.
        !freerdp_settings_set_bool(settings, FreeRDP_SupportDynamicChannels, TRUE) ||
        !freerdp_settings_set_bool(settings, FreeRDP_RefreshRect, TRUE))
        return FALSE;

    peer->Capabilities = peer_capabilities;
    peer->PostConnect = peer_post_connect;
    peer->Activate = peer_activate;
    peer->Logon = peer_logon;

    if (!peer->Initialize(peer)) return FALSE;

    peer->context->input->KeyboardEvent = peer_keyboard;
    peer->context->input->MouseEvent = peer_mouse;

    s->peer = peer;
    s->peer_activated = false;
    s->peer_authenticated = false;
    return TRUE;
}

static void drop_peer(struct wr_server *s) {
    if (!s->peer) return;
    fprintf(stderr, "wayrdp: %s disconnected\n", s->peer->hostname);

    // Channels first: they hold threads that are reading from a transport this
    // is about to close, and the microphone should disappear from the desktop's
    // device list the moment the person holding it is gone.
    close_audio(s);
    if (s->vcm) { WTSCloseServer(s->vcm); s->vcm = NULL; }

    s->peer->Disconnect(s->peer);
    freerdp_peer_context_free(s->peer);
    freerdp_peer_free(s->peer);
    s->peer = NULL;
    s->peer_activated = false;
    s->peer_authenticated = false;
}

// --- server ----------------------------------------------------------------

struct wr_server *wr_server_new(struct wr_wayland *wayland,
                                struct wr_audio *audio,
                                const struct wr_config *config,
                                const char **error) {
    struct wr_server *s = calloc(1, sizeof(*s));
    if (!s) { *error = "out of memory"; return NULL; }

    s->wayland = wayland;
    s->audio = audio;
    s->config = *config;

    // Without this, WinPR's channel manager goes looking for an external WTS
    // implementation, does not find one, and every virtual channel -- sound
    // included -- is refused before it is asked for.
    WTSRegisterWtsApiFunctionTable(FreeRDP_InitWtsApi());

    s->listener = freerdp_listener_new();
    if (!s->listener) { *error = "could not create the RDP listener"; free(s); return NULL; }

    s->listener->info = s;
    s->listener->PeerAccepted = peer_accepted;

    if (!s->listener->Open(s->listener, config->bind_address, (UINT16)config->port)) {
        *error = "could not bind the port (is something already on it?)";
        freerdp_listener_free(s->listener);
        free(s);
        return NULL;
    }
    return s;
}

bool wr_server_run(struct wr_server *s, const volatile bool *running) {
    while (*running) {
        // Ask FreeRDP for its handles rather than polling the socket: bytes it
        // has already read into its own buffer will never wake a poll on the
        // fd again, and the handshake stops halfway with the client still
        // connected and waiting. That is what "accepted, then silence" was.
        HANDLE handles[32];
        DWORD n = s->listener->GetEventHandles(s->listener, handles, 32);
        if (s->peer)
            n += s->peer->GetEventHandles(s->peer, handles + n, 32 - n);
        if (s->vcm && n < 32) {
            HANDLE h = WTSVirtualChannelManagerGetEventHandle(s->vcm);
            if (h && h != INVALID_HANDLE_VALUE) handles[n++] = h;
        }

        struct pollfd pfds[33];
        nfds_t count = 0;
        for (DWORD i = 0; i < n && count < 32; i++) {
            int fd = GetEventFileDescriptor(handles[i]);
            if (fd >= 0) pfds[count++] = (struct pollfd){ .fd = fd, .events = POLLIN };
        }
        pfds[count++] = (struct pollfd){ .fd = wr_fd(s->wayland), .events = POLLIN };

        wr_flush(s->wayland);

        // While a client is watching, wake often enough to send a frame; while
        // nobody is, sleep. The handshake needs many round trips, so it is not
        // throttled to the idle rate.
        int timeout = s->peer_activated ? 16 : 50;
        if (poll(pfds, count, timeout) < 0 && errno != EINTR) return false;

        // Both are cheap and both know better than poll does whether they have
        // work: buffered input does not show up in revents.
        if (!s->listener->CheckFileDescriptor(s->listener))
            return false;

        // Drain, do not sample. One call reads one PDU, and a microphone sends
        // a hundred a second; at the rate this loop turns, three in four would
        // sit in the buffer and the sound would arrive in quarters. The cap is
        // there so that a client which never stops talking cannot hold the
        // screen still.
        for (int pdu = 0; pdu < 64 && s->peer; pdu++) {
            if (!s->peer->CheckFileDescriptor(s->peer)) { drop_peer(s); break; }
            if (!s->peer->HasMoreToRead(s->peer)) break;
        }

        // The peer's read pulls channel bytes off the wire; this hands them to
        // the channel that asked for them. Skipping it is a sound channel that
        // opens and then never hears a confirm.
        //
        // The second argument is the dynamic channel, and it is not a detail:
        // the manager tries to open drdynvc exactly once, on the first call
        // that asks it to, and then never again. Asking before the client has
        // joined a single channel spends that one attempt on nothing, which is
        // a microphone that stays at "initialized" for the whole session.
        if (s->vcm && !WTSVirtualChannelManagerCheckFileDescriptorEx(
                          s->vcm, s->peer_activated))
            drop_peer(s);

        // A dispatch error means the compositor connection is gone. Stop here so
        // systemd restarts the unit against the current compositor, instead of
        // polling a dead socket forever while clients see a blank screen.
        if (!wr_dispatch_pending(s->wayland))
            return false;

        if (s->peer_activated) {
            open_microphone(s);
            pump_speaker(s);

            // The wait is the frame pacing: this is the loop's idle time.
            const struct wr_frame *f = wr_capture_frame(s->wayland, 16);
            if (f && !send_frame(s, f, false))
                drop_peer(s);
            if (wr_fatal(s->wayland))
                return false;
        }
    }
    return true;
}

void wr_server_free(struct wr_server *s) {
    if (!s) return;
    drop_peer(s);
    if (s->listener) {
        s->listener->Close(s->listener);
        freerdp_listener_free(s->listener);
    }
    if (s->stream) Stream_Free(s->stream, TRUE);
    if (s->rfx) rfx_context_free(s->rfx);
    free(s->mirror);
    free(s);
}
