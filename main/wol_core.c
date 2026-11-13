#include "wol_core.h"

#include <string.h>

/* ---------------------------------------------------------------------- */

/* Bounded strlen (strnlen is POSIX, not ISO C). */
static size_t bounded_len(const char *s, size_t max)
{
    size_t n = 0;
    while (n < max && s[n] != '\0') n++;
    return n;
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool wol_parse_mac(const char *s, uint8_t out[6])
{
    if (!s || !out) return false;
    if (bounded_len(s, 18) != 17) return false;

    const char sep = s[2];
    if (sep != ':' && sep != '-') return false;

    uint8_t tmp[6];
    for (int i = 0; i < 6; i++) {
        const char *p = s + i * 3;
        int hi = hex_val(p[0]);
        int lo = hex_val(p[1]);
        if (hi < 0 || lo < 0) return false;
        if (i < 5 && p[2] != sep) return false;
        tmp[i] = (uint8_t)((hi << 4) | lo);
    }
    memcpy(out, tmp, 6);
    return true;
}

bool wol_mac_is_usable(const uint8_t mac[6])
{
    if (!mac) return false;

    static const uint8_t placeholder[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    uint8_t all_zero = 0, all_ff = 0xFF;
    for (int i = 0; i < 6; i++) {
        all_zero |= mac[i];
        all_ff   &= mac[i];
    }
    if (all_zero == 0)    return false;   /* 00:00:00:00:00:00 */
    if (all_ff == 0xFF)   return false;   /* FF:FF:FF:FF:FF:FF */
    if (mac[0] & 0x01)    return false;   /* multicast / group address */
    if (memcmp(mac, placeholder, 6) == 0) return false;
    return true;
}

bool wol_parse_ipv4(const char *s, uint32_t *host_order)
{
    if (!s || !host_order) return false;

    uint32_t result = 0;
    const char *p = s;
    for (int octet = 0; octet < 4; octet++) {
        if (*p < '0' || *p > '9') return false;

        uint32_t val = 0;
        int digits = 0;
        const char *start = p;
        while (*p >= '0' && *p <= '9') {
            val = val * 10 + (uint32_t)(*p - '0');
            if (++digits > 3 || val > 255) return false;
            p++;
        }
        if (digits > 1 && *start == '0') return false;   /* no leading zeros */

        result = (result << 8) | val;
        if (octet < 3) {
            if (*p != '.') return false;
            p++;
        }
    }
    if (*p != '\0') return false;
    *host_order = result;
    return true;
}

void wol_build_magic(const uint8_t mac[6], uint8_t pkt[WOL_PACKET_LEN])
{
    memset(pkt, 0xFF, 6);
    for (int i = 0; i < 16; i++) {
        memcpy(pkt + 6 + i * 6, mac, 6);
    }
}

bool wol_ct_equal(const void *a, const void *b, size_t n)
{
    const volatile uint8_t *pa = (const volatile uint8_t *)a;
    const volatile uint8_t *pb = (const volatile uint8_t *)b;
    uint8_t diff = 0;
    for (size_t i = 0; i < n; i++) {
        diff |= (uint8_t)(pa[i] ^ pb[i]);
    }
    return diff == 0;
}

/* ---------------------------------------------------------------------- */

bool wol_allowlist_parse(const char *csv, wol_allowlist_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!csv) return true;

    char tok[16];
    size_t tlen = 0;

    for (const char *p = csv;; p++) {
        char c = *p;
        bool is_sep = (c == ',' || c == ';' || c == ' ' || c == '\t' || c == '\0');
        if (!is_sep) {
            if (tlen >= sizeof(tok) - 1) return false;      /* too long for an IPv4 */
            tok[tlen++] = c;
        } else if (tlen > 0) {
            tok[tlen] = '\0';
            if (out->count >= WOL_MAX_ALLOWED_SRC) return false;
            uint32_t ip;
            if (!wol_parse_ipv4(tok, &ip)) return false;
            out->ip[out->count++] = ip;
            tlen = 0;
        }
        if (c == '\0') break;
    }
    return true;
}

bool wol_allowlist_permits(const wol_allowlist_t *al, uint32_t src)
{
    if (!al || al->count == 0) return true;
    for (size_t i = 0; i < al->count; i++) {
        if (al->ip[i] == src) return true;
    }
    return false;
}

/* ---------------------------------------------------------------------- */

bool wol_token_is_valid(const char *token)
{
    if (!token) return true;                    /* NULL == none */
    size_t n = bounded_len(token, WOL_TOKEN_MAX_LEN + 1);
    if (n == 0) return true;                    /* empty == none */
    if (n < WOL_TOKEN_MIN_LEN || n > WOL_TOKEN_MAX_LEN) return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)token[i];
        if (c <= 0x20 || c >= 0x7F) return false;
    }
    return true;
}

wol_verdict_t wol_check_payload(const uint8_t *data, size_t len, const char *token)
{
    if (!data || len == 0 || len > WOL_MAX_PAYLOAD) return WOL_PKT_BAD_FORMAT;

    /* Strip one optional line terminator. */
    if (len >= 2 && data[len - 2] == '\r' && data[len - 1] == '\n') len -= 2;
    else if (len >= 1 && data[len - 1] == '\n')                       len -= 1;

    static const char kw[] = "wake";
    const size_t kw_len = sizeof(kw) - 1;

    if (len < kw_len || memcmp(data, kw, kw_len) != 0) return WOL_PKT_BAD_FORMAT;

    const size_t tok_len = token ? bounded_len(token, WOL_TOKEN_MAX_LEN + 1) : 0;

    if (tok_len == 0) {
        return (len == kw_len) ? WOL_PKT_OK : WOL_PKT_BAD_FORMAT;
    }

    /* Expect "wake" + ' ' + token */
    if (len != kw_len + 1 + tok_len) return WOL_PKT_BAD_TOKEN;
    if (data[kw_len] != ' ')          return WOL_PKT_BAD_TOKEN;
    return wol_ct_equal(data + kw_len + 1, token, tok_len) ? WOL_PKT_OK : WOL_PKT_BAD_TOKEN;
}

/* ---------------------------------------------------------------------- */

bool wol_limiter_try(wol_limiter_t *l, int64_t now_ms, uint32_t cooldown_ms)
{
    if (!l) return false;
    if (l->used && now_ms >= l->last_ms) {
        /* Unsigned subtraction avoids signed-overflow UB for extreme inputs. */
        uint64_t elapsed = (uint64_t)now_ms - (uint64_t)l->last_ms;
        if (elapsed < (uint64_t)cooldown_ms) return false;
    }
    /* A backwards clock jump allows one packet and resynchronizes the limiter. */
    l->used = true;
    l->last_ms = now_ms;
    return true;
}
