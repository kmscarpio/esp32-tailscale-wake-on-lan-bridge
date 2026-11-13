/* Host unit tests for main/wol_core.c.  Build & run:  make -C tests/host */
#include <stdio.h>
#include <string.h>
#include "../../main/wol_core.h"

static int g_fail, g_total;
#define CHECK(c) do { g_total++; if (!(c)) { g_fail++; \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

static wol_verdict_t chk(const char *s, const char *tok) {
    return wol_check_payload((const uint8_t *)s, strlen(s), tok);
}

static void test_mac(void) {
    uint8_t m[6];
    CHECK(wol_parse_mac("12:34:56:78:9A:bc", m));
    CHECK(m[0]==0x12 && m[1]==0x34 && m[2]==0x56 && m[3]==0x78 && m[4]==0x9A && m[5]==0xBC);
    CHECK(wol_parse_mac("12-34-56-78-9a-bc", m));
    /* rejects */
    CHECK(!wol_parse_mac("", m));
    CHECK(!wol_parse_mac(NULL, m));
    CHECK(!wol_parse_mac("1:2:3:4:5:6", m));                 /* sscanf("%hhx") would accept this */
    CHECK(!wol_parse_mac("12:34:56:78:9A:BC:", m));
    CHECK(!wol_parse_mac("12:34:56:78:9A:BC garbage", m));
    CHECK(!wol_parse_mac("12:34:56:78:9A:BG", m));
    CHECK(!wol_parse_mac("12:34-56:78:9A:BC", m));           /* mixed separators */
    CHECK(!wol_parse_mac("12 34 56 78 9A BC", m));
    CHECK(!wol_parse_mac("123456789ABC", m));
    /* failed parse must not clobber output */
    uint8_t keep[6] = {1,2,3,4,5,6}, k2[6]; memcpy(k2, keep, 6);
    CHECK(!wol_parse_mac("12:34:56:78:9A:BG", keep));
    CHECK(memcmp(keep, k2, 6) == 0);
}

static void test_mac_usable(void) {
    uint8_t ok[6]   = {0x04,0xD9,0xF5,0x7E,0x2B,0xE4};
    uint8_t zero[6] = {0};
    uint8_t ff[6]   = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    uint8_t mc[6]   = {0x01,0x00,0x5E,0x00,0x00,0x01};
    uint8_t ph[6]   = {0xAA,0xBB,0xCC,0xDD,0xEE,0xFF};
    CHECK(wol_mac_is_usable(ok));
    CHECK(!wol_mac_is_usable(zero));
    CHECK(!wol_mac_is_usable(ff));
    CHECK(!wol_mac_is_usable(mc));
    CHECK(!wol_mac_is_usable(ph));
    CHECK(!wol_mac_is_usable(NULL));
}

static void test_ip(void) {
    uint32_t v;
    CHECK(wol_parse_ipv4("100.64.0.1", &v) && v == 0x64400001u);
    CHECK(wol_parse_ipv4("255.255.255.255", &v) && v == 0xFFFFFFFFu);
    CHECK(wol_parse_ipv4("0.0.0.0", &v) && v == 0);
    CHECK(wol_parse_ipv4("192.168.1.255", &v) && v == 0xC0A801FFu);
    CHECK(!wol_parse_ipv4("192.168.1", &v));        /* inet_addr() accepts this! */
    CHECK(!wol_parse_ipv4("192.168.1.256", &v));
    CHECK(!wol_parse_ipv4("192.168.01.1", &v));     /* octal ambiguity */
    CHECK(!wol_parse_ipv4("1.2.3.4.5", &v));
    CHECK(!wol_parse_ipv4("1.2.3.4 ", &v));
    CHECK(!wol_parse_ipv4(" 1.2.3.4", &v));
    CHECK(!wol_parse_ipv4("1..3.4", &v));
    CHECK(!wol_parse_ipv4("a.b.c.d", &v));
    CHECK(!wol_parse_ipv4("", &v));
    CHECK(!wol_parse_ipv4(NULL, &v));
    CHECK(!wol_parse_ipv4("1.2.3.4", NULL));
    CHECK(!wol_parse_ipv4("1111.2.3.4", &v));
}

static void test_magic(void) {
    uint8_t mac[6] = {1,2,3,4,5,6}, p[WOL_PACKET_LEN];
    wol_build_magic(mac, p);
    for (int i = 0; i < 6; i++) CHECK(p[i] == 0xFF);
    for (int r = 0; r < 16; r++) CHECK(memcmp(p + 6 + r*6, mac, 6) == 0);
    CHECK(sizeof(p) == 102);
}

static void test_allowlist(void) {
    wol_allowlist_t al;
    CHECK(wol_allowlist_parse("", &al) && al.count == 0);
    CHECK(wol_allowlist_parse(NULL, &al) && al.count == 0);
    CHECK(wol_allowlist_parse("100.64.0.1", &al) && al.count == 1);
    CHECK(wol_allowlist_parse(" 100.64.0.1 , 100.64.0.2;100.64.0.3  ", &al) && al.count == 3);
    CHECK(wol_allowlist_permits(&al, 0x64400002u));
    CHECK(!wol_allowlist_permits(&al, 0x64400009u));
    CHECK(!wol_allowlist_parse("100.64.0.1,bogus", &al));
    CHECK(!wol_allowlist_parse("100.64.0.1,100.64.0.", &al));
    CHECK(!wol_allowlist_parse("1.1.1.1,1.1.1.2,1.1.1.3,1.1.1.4,1.1.1.5,1.1.1.6,1.1.1.7,1.1.1.8,1.1.1.9", &al));
    CHECK(wol_allowlist_parse("1.1.1.1,1.1.1.2,1.1.1.3,1.1.1.4,1.1.1.5,1.1.1.6,1.1.1.7,1.1.1.8", &al) && al.count == 8);
    CHECK(!wol_allowlist_parse("100.64.0.1,12345678901234567890", &al));
    wol_allowlist_t empty = {0};
    CHECK(wol_allowlist_permits(&empty, 0x01020304u));   /* empty list = caller policy */
}

static void test_token(void) {
    CHECK(wol_token_is_valid(""));
    CHECK(wol_token_is_valid(NULL));
    CHECK(wol_token_is_valid("0123456789ab"));
    CHECK(!wol_token_is_valid("short"));
    CHECK(!wol_token_is_valid("has space inside!!"));
    CHECK(!wol_token_is_valid("tab\there12345678"));
    CHECK(!wol_token_is_valid("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0"));
    CHECK(wol_token_is_valid("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"));
}

static void test_payload(void) {
    const char *T = "s3cretTokenValue1";
    /* with token */
    CHECK(chk("wake s3cretTokenValue1", T) == WOL_PKT_OK);
    CHECK(chk("wake s3cretTokenValue1\n", T) == WOL_PKT_OK);
    CHECK(chk("wake s3cretTokenValue1\r\n", T) == WOL_PKT_OK);
    CHECK(chk("wake", T) == WOL_PKT_BAD_TOKEN);
    CHECK(chk("wake\n", T) == WOL_PKT_BAD_TOKEN);
    CHECK(chk("wake wrongTokenValue11", T) == WOL_PKT_BAD_TOKEN);
    CHECK(chk("wake s3cretTokenValue2", T) == WOL_PKT_BAD_TOKEN);      /* last char differs */
    CHECK(chk("wake s3cretTokenValue", T) == WOL_PKT_BAD_TOKEN);       /* prefix only */
    CHECK(chk("wake s3cretTokenValue1x", T) == WOL_PKT_BAD_TOKEN);     /* extra char */
    CHECK(chk("wake  s3cretTokenValue1", T) == WOL_PKT_BAD_TOKEN);     /* double space */
    CHECK(chk("wake\ts3cretTokenValue1", T) == WOL_PKT_BAD_TOKEN);
    CHECK(chk("wakes3cretTokenValue1", T) == WOL_PKT_BAD_TOKEN);
    CHECK(chk("WAKE s3cretTokenValue1", T) == WOL_PKT_BAD_FORMAT);
    CHECK(chk("hello", T) == WOL_PKT_BAD_FORMAT);
    CHECK(chk("", T) == WOL_PKT_BAD_FORMAT);
    /* no token configured */
    CHECK(chk("wake", "") == WOL_PKT_OK);
    CHECK(chk("wake\n", NULL) == WOL_PKT_OK);
    CHECK(chk("wake\r\n", "") == WOL_PKT_OK);
    CHECK(chk("wake extra", "") == WOL_PKT_BAD_FORMAT);
    CHECK(chk("hello", "") == WOL_PKT_BAD_FORMAT);
    CHECK(chk("wak", "") == WOL_PKT_BAD_FORMAT);
    /* oversize + embedded NUL + null ptr */
    uint8_t big[WOL_MAX_PAYLOAD + 1]; memset(big, 'a', sizeof(big)); memcpy(big, "wake", 4);
    CHECK(wol_check_payload(big, sizeof(big), "") == WOL_PKT_BAD_FORMAT);
    CHECK(wol_check_payload(big, WOL_MAX_PAYLOAD, "") == WOL_PKT_BAD_FORMAT);
    const uint8_t nul[] = {'w','a','k','e',0,'x'};
    CHECK(wol_check_payload(nul, sizeof(nul), "") == WOL_PKT_BAD_FORMAT);
    CHECK(wol_check_payload(NULL, 4, "") == WOL_PKT_BAD_FORMAT);
    CHECK(wol_check_payload((const uint8_t *)"wake", 0, "") == WOL_PKT_BAD_FORMAT);
    /* token must not be accepted when it is followed by a NUL-extended payload */
    const uint8_t t2[] = "wake s3cretTokenValue1\0";      /* 22 chars + explicit NUL + implicit NUL */
    CHECK(wol_check_payload(t2, sizeof(t2) - 2, T) == WOL_PKT_OK);        /* exact token */
    CHECK(wol_check_payload(t2, sizeof(t2) - 1, T) == WOL_PKT_BAD_TOKEN); /* + 1 NUL byte */
    CHECK(wol_check_payload(t2, sizeof(t2),     T) == WOL_PKT_BAD_TOKEN); /* + 2 NUL bytes */
}

static void test_ct(void) {
    CHECK(wol_ct_equal("abc", "abc", 3));
    CHECK(!wol_ct_equal("abc", "abd", 3));
    CHECK(wol_ct_equal("abc", "abd", 2));
    CHECK(wol_ct_equal("", "", 0));
}

static void test_limiter(void) {
    wol_limiter_t l = {0};
    CHECK(wol_limiter_try(&l, 0, 10000));            /* first send always allowed, even at t=0 */
    CHECK(!wol_limiter_try(&l, 1, 10000));
    CHECK(!wol_limiter_try(&l, 9999, 10000));
    CHECK(wol_limiter_try(&l, 10000, 10000));
    CHECK(!wol_limiter_try(&l, 10001, 10000));
    CHECK(wol_limiter_try(&l, 5000, 10000));         /* clock went backwards -> resync, allow */
    CHECK(!wol_limiter_try(&l, 5001, 10000));
    CHECK(!wol_limiter_try(NULL, 0, 1));
    wol_limiter_t z = {0};
    CHECK(wol_limiter_try(&z, 100, 0) && wol_limiter_try(&z, 100, 0));  /* cooldown 0 = disabled */
}

int main(void) {
    test_mac(); test_mac_usable(); test_ip(); test_magic(); test_allowlist();
    test_token(); test_payload(); test_ct(); test_limiter();
    printf("%d checks, %d failed\n", g_total, g_fail);
    return g_fail ? 1 : 0;
}
