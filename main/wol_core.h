/**
 * wol_core - hardware-independent Wake-on-LAN logic.
 *
 * Everything security-relevant that does NOT need ESP-IDF lives here so it can
 * be unit-tested on a normal PC (see tests/host/).  No heap allocation, no
 * logging, no globals.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WOL_PACKET_LEN        102   /* 6 x 0xFF + 16 x MAC                     */
#define WOL_MAX_PAYLOAD       96    /* hard cap on accepted trigger datagrams  */
#define WOL_MAX_ALLOWED_SRC   8
#define WOL_TOKEN_MIN_LEN     12
#define WOL_TOKEN_MAX_LEN     64

/* "AA:BB:CC:DD:EE:FF" or "AA-BB-CC-DD-EE-FF" (one separator style, exactly 17 chars). */
bool wol_parse_mac(const char *s, uint8_t out[6]);

/* Rejects 00:..:00, FF:..:FF, multicast MACs and the Kconfig placeholder AA:BB:CC:DD:EE:FF. */
bool wol_mac_is_usable(const uint8_t mac[6]);

/* Strict dotted quad ("a.b.c.d", decimal, no leading zeros, no trailing junk).
 * Result is in HOST byte order (a<<24 | b<<16 | c<<8 | d), same as ts_parse_ip(). */
bool wol_parse_ipv4(const char *s, uint32_t *host_order);

void wol_build_magic(const uint8_t mac[6], uint8_t pkt[WOL_PACKET_LEN]);

/* Constant-time equality (no early exit). */
bool wol_ct_equal(const void *a, const void *b, size_t n);

/* ---- Source allow-list (Tailscale IPs, host byte order) ---------------- */
typedef struct {
    uint32_t ip[WOL_MAX_ALLOWED_SRC];
    size_t   count;                 /* 0 == no list configured */
} wol_allowlist_t;

/* Comma/semicolon/space separated list.  Empty string => count 0 (valid).
 * Returns false on any malformed entry or more than WOL_MAX_ALLOWED_SRC entries. */
bool wol_allowlist_parse(const char *csv, wol_allowlist_t *out);

/* If the list is empty this returns true (caller decides whether "empty" is acceptable). */
bool wol_allowlist_permits(const wol_allowlist_t *al, uint32_t src_host_order);

/* ---- Token validation -------------------------------------------------- */
/* Token must be WOL_TOKEN_MIN_LEN..WOL_TOKEN_MAX_LEN printable ASCII, no spaces.
 * An empty string is "no token" and is reported as valid here (policy is the caller's). */
bool wol_token_is_valid(const char *token);

typedef enum {
    WOL_PKT_OK = 0,
    WOL_PKT_BAD_FORMAT,
    WOL_PKT_BAD_TOKEN
} wol_verdict_t;

/* Accepts exactly "wake" (token == "" / NULL) or "wake <token>", optionally followed by
 * a single "\n" or "\r\n".  Anything else is rejected.  Never reads past len. */
wol_verdict_t wol_check_payload(const uint8_t *data, size_t len, const char *token);

/* ---- Cooldown ---------------------------------------------------------- */
typedef struct {
    int64_t last_ms;
    bool    used;
} wol_limiter_t;

/* Returns true if a magic packet may be sent now, and records the send.
 * Wrap-safe for a monotonic int64 millisecond clock. */
bool wol_limiter_try(wol_limiter_t *l, int64_t now_ms, uint32_t cooldown_ms);
