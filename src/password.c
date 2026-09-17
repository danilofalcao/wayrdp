// password.c — scrypt hashing for the configured RDP password.
//
// The server only ever has to *check* a password, never recover it, so the
// config stores a salted scrypt hash instead of the password itself. A leaked
// file then yields no usable secret, and there is no key to protect — which is
// what makes hashing a better fit here than reversible encryption.

#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include "password.h"

// N = 2^15 with r = 8 is 32 MiB of work, one thread: expensive enough to make
// guessing slow, cheap enough to pay once per connection.
#define WR_SCRYPT_LN 15
#define WR_SCRYPT_R 8
#define WR_SCRYPT_P 1
#define WR_SCRYPT_KEY 32
#define WR_SCRYPT_SALT 16
#define WR_SCRYPT_MAXMEM (64u * 1024u * 1024u)
#define WR_SCRYPT_SCHEME "scrypt"

static const char b64_alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// Encoded length of `len` bytes, including '=' padding.
static size_t b64_length(size_t len) {
    return 4 * ((len + 2) / 3);
}

static void b64_encode(const unsigned char *in, size_t len, char *out) {
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        unsigned int v = (unsigned int)in[i] << 16;
        if (i + 1 < len) v |= (unsigned int)in[i + 1] << 8;
        if (i + 2 < len) v |= (unsigned int)in[i + 2];
        out[o++] = b64_alphabet[(v >> 18) & 0x3F];
        out[o++] = b64_alphabet[(v >> 12) & 0x3F];
        out[o++] = (i + 1 < len) ? b64_alphabet[(v >> 6) & 0x3F] : '=';
        out[o++] = (i + 2 < len) ? b64_alphabet[v & 0x3F] : '=';
    }
    out[o] = 0;
}

static int b64_value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

// Decodes exactly `expected` bytes; any other length or a stray character fails.
static bool b64_decode(const char *in, unsigned char *out, size_t expected) {
    size_t len = strlen(in);
    if (len != b64_length(expected)) return false;

    size_t produced = 0;
    for (size_t i = 0; i < len; i += 4) {
        int a = b64_value(in[i]);
        int b = b64_value(in[i + 1]);
        int c = in[i + 2] == '=' ? -2 : b64_value(in[i + 2]);
        int d = in[i + 3] == '=' ? -2 : b64_value(in[i + 3]);
        if (a < 0 || b < 0 || c == -1 || d == -1) return false;
        if (c == -2 && d != -2) return false;   // '=' only terminates the tail

        unsigned int v = ((unsigned int)a << 18) | ((unsigned int)b << 12);
        if (c >= 0) v |= (unsigned int)c << 6;
        if (d >= 0) v |= (unsigned int)d;

        out[produced++] = (v >> 16) & 0xFF;
        if (produced < expected) out[produced++] = (v >> 8) & 0xFF;
        if (produced < expected) out[produced++] = v & 0xFF;
    }
    return produced == expected;
}

static bool derive(const char *password, size_t password_len,
                   const unsigned char *salt, size_t salt_len,
                   unsigned int ln, unsigned int r, unsigned int p,
                   unsigned char *key, size_t key_len) {
    // Reject anything but this build's parameters: a tampered config should not
    // be able to ask for an unbounded work factor, or a shift past the type.
    if (ln != WR_SCRYPT_LN || r != WR_SCRYPT_R || p != WR_SCRYPT_P) return false;
    if (salt_len != WR_SCRYPT_SALT || key_len != WR_SCRYPT_KEY) return false;
    return EVP_PBE_scrypt(password, password_len, salt, salt_len,
                          (uint64_t)1 << ln, r, p, WR_SCRYPT_MAXMEM,
                          key, key_len) == 1;
}

bool wr_password_hash(const char *password, char *out, size_t capacity) {
    if (!password || !out) return false;

    unsigned char salt[WR_SCRYPT_SALT];
    unsigned char key[WR_SCRYPT_KEY];
    if (RAND_bytes(salt, sizeof(salt)) != 1) return false;
    if (!derive(password, strlen(password), salt, sizeof(salt),
                WR_SCRYPT_LN, WR_SCRYPT_R, WR_SCRYPT_P, key, sizeof(key))) {
        OPENSSL_cleanse(key, sizeof(key));
        return false;
    }

    char salt_b64[b64_length(WR_SCRYPT_SALT) + 1];
    char key_b64[b64_length(WR_SCRYPT_KEY) + 1];
    b64_encode(salt, sizeof(salt), salt_b64);
    b64_encode(key, sizeof(key), key_b64);

    int written = snprintf(out, capacity, "%s$ln=%d,r=%d,p=%d$%s$%s",
                           WR_SCRYPT_SCHEME, WR_SCRYPT_LN, WR_SCRYPT_R, WR_SCRYPT_P,
                           salt_b64, key_b64);
    OPENSSL_cleanse(key, sizeof(key));
    return written > 0 && (size_t)written < capacity;
}

bool wr_password_is_hash(const char *encoded) {
    return encoded && strncmp(encoded, WR_SCRYPT_SCHEME "$", strlen(WR_SCRYPT_SCHEME) + 1) == 0;
}

bool wr_password_verify(const char *given, size_t given_len, const char *encoded) {
    if (!given || !encoded || strlen(encoded) >= WR_PASSWORD_ENCODED_MAX) return false;

    // Parse a private copy: the tokenizer writes terminators.
    char copy[WR_PASSWORD_ENCODED_MAX];
    snprintf(copy, sizeof(copy), "%s", encoded);

    char *save = NULL;
    char *scheme = strtok_r(copy, "$", &save);
    char *params = strtok_r(NULL, "$", &save);
    char *salt_b64 = strtok_r(NULL, "$", &save);
    char *key_b64 = strtok_r(NULL, "$", &save);
    bool trailing = strtok_r(NULL, "$", &save) != NULL;
    if (!scheme || !params || !salt_b64 || !key_b64 || trailing) return false;
    if (strcmp(scheme, WR_SCRYPT_SCHEME) != 0) return false;

    unsigned int ln = 0, r = 0, p = 0;
    if (sscanf(params, "ln=%u,r=%u,p=%u", &ln, &r, &p) != 3) return false;

    unsigned char salt[WR_SCRYPT_SALT];
    unsigned char expected[WR_SCRYPT_KEY];
    if (!b64_decode(salt_b64, salt, sizeof(salt))) return false;
    if (!b64_decode(key_b64, expected, sizeof(expected))) return false;

    unsigned char derived[WR_SCRYPT_KEY];
    bool ok = derive(given, given_len, salt, sizeof(salt), ln, r, p,
                     derived, sizeof(derived)) &&
              CRYPTO_memcmp(derived, expected, sizeof(expected)) == 0;
    OPENSSL_cleanse(derived, sizeof(derived));
    return ok;
}
