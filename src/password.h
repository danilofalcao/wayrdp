// password.h — salted scrypt credentials, so the config never holds the secret.

#ifndef WAYRDP_PASSWORD_H
#define WAYRDP_PASSWORD_H

#include <stdbool.h>
#include <stddef.h>

// Longest encoded hash this build writes or accepts.
#define WR_PASSWORD_ENCODED_MAX 256

// Derives a fresh scrypt hash of `password`, encoded as
// "scrypt$ln=..,r=..,p=..$<salt>$<key>" into `out`. False when the platform
// RNG or the KDF fails, which callers must treat as "no credential written".
bool wr_password_hash(const char *password, char *out, size_t capacity);

// True when `encoded` carries this build's scrypt scheme, so a config error can
// be reported instead of silently refusing every login.
bool wr_password_is_hash(const char *encoded);

// Verifies a client-supplied password against an encoded hash. The given length
// is explicit because the RDP identity string carries no terminator.
bool wr_password_verify(const char *given, size_t given_len, const char *encoded);

#endif
