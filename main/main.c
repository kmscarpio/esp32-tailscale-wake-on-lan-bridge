/**
 * ESP32 Wake-on-LAN via Tailscale
 * Board : NodeMCU-32S / ESP32S  (no PSRAM)
 *
 * The device joins your Tailscale network and listens on a UDP port that is
 * bound to the Tailscale interface only. A valid trigger sends one WoL magic
 * packet on the local LAN.
 *
 * Trigger from an allowed Tailscale peer:
 *   echo "wake <token>" | nc -u <esp32-tailscale-ip> 9999 -w1
 *
 * Security model (see README "Security"):
 *   1. Tailnet ACLs decide which peers can reach this node at all.
 *   2. This firmware does NOT enforce ACL *port* rules, so it applies its own
 *      checks: source-IP allow-list, shared token, strict payload format and a
 *      cooldown. It refuses to start with none of the first two configured.
 *   3. Invalid triggers are dropped silently (no reply, rate-limited logs,
 *      payload never logged).
 *
 * Configure via:  idf.py menuconfig  ->  "Wake-on-LAN Configuration"
 */

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"

#include "ts.h"
#include "wol_core.h"

static const char *TAG = "wol";

/* ---- Compile-time sanity checks on Kconfig values ---------------------- */
_Static_assert(sizeof(CONFIG_WOL_WIFI_SSID) - 1 >= 1 && sizeof(CONFIG_WOL_WIFI_SSID) - 1 <= 32,
               "WOL_WIFI_SSID must be 1..32 characters");
_Static_assert(sizeof(CONFIG_WOL_WIFI_PASSWORD) - 1 == 0 ||
               (sizeof(CONFIG_WOL_WIFI_PASSWORD) - 1 >= 8 && sizeof(CONFIG_WOL_WIFI_PASSWORD) - 1 <= 64),
               "WOL_WIFI_PASSWORD must be empty (open network) or 8..64 characters");

/* ============================================================================
 * Runtime configuration (validated once at boot, fail closed)
 * ========================================================================== */

typedef struct {
    uint8_t         mac[6];
    uint32_t        bcast_host_order;
    wol_allowlist_t allow;
    bool            have_token;
} wol_cfg_t;

static wol_cfg_t s_cfg;

static bool load_config(wol_cfg_t *c)
{
    memset(c, 0, sizeof(*c));
    bool ok = true;

    if (!wol_parse_mac(CONFIG_WOL_TARGET_MAC, c->mac) || !wol_mac_is_usable(c->mac)) {
        ESP_LOGE(TAG, "Invalid or placeholder target MAC '%s' (need AA:BB:CC:DD:EE:FF form,"
                      " not the example value)", CONFIG_WOL_TARGET_MAC);
        ok = false;
    }

    if (!wol_parse_ipv4(CONFIG_WOL_BROADCAST_IP, &c->bcast_host_order)) {
        ESP_LOGE(TAG, "Invalid broadcast IP '%s' (need strict a.b.c.d)", CONFIG_WOL_BROADCAST_IP);
        ok = false;
    }

    if (!wol_allowlist_parse(CONFIG_WOL_ALLOWED_SRC_IPS, &c->allow)) {
        ESP_LOGE(TAG, "WOL_ALLOWED_SRC_IPS is malformed (max %d IPv4 addresses, comma separated)",
                 WOL_MAX_ALLOWED_SRC);
        ok = false;
    }

    if (!wol_token_is_valid(CONFIG_WOL_AUTH_TOKEN)) {
        ESP_LOGE(TAG, "WOL_AUTH_TOKEN must be %d-%d printable ASCII characters without spaces",
                 WOL_TOKEN_MIN_LEN, WOL_TOKEN_MAX_LEN);
        ok = false;
    }
    c->have_token = (strlen(CONFIG_WOL_AUTH_TOKEN) > 0);

    if (!c->have_token && c->allow.count == 0) {
#if CONFIG_WOL_ALLOW_UNAUTHENTICATED
        ESP_LOGW(TAG, "!!! No token and no allow-list: ANY reachable tailnet peer can wake the PC !!!");
#else
        ESP_LOGE(TAG, "Refusing to start: set WOL_AUTH_TOKEN and/or WOL_ALLOWED_SRC_IPS"
                      " (or explicitly enable WOL_ALLOW_UNAUTHENTICATED)");
        ok = false;
#endif
    }
    return ok;
}

/* ============================================================================
 * WiFi
 * ========================================================================== */

static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1

#define WIFI_INITIAL_RETRIES 20

static volatile bool s_wifi_got_ip_once = false;
static volatile bool s_wifi_connected = false;

/* Fixed-size, low-overhead runtime diagnostics. */
typedef struct {
    char last_command[12];
    char last_auth[16];
    char last_wol[20];
    char last_error[32];
    char last_ack[12];
    uint32_t ack_count;
} wol_diag_t;

static wol_diag_t s_diag = {
    .last_command = "NONE",
    .last_auth = "NONE",
    .last_wol = "NOT_SENT",
    .last_error = "NONE",
    .last_ack = "NONE",
};
static volatile uint32_t s_udp_rx_count = 0;

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    static int retries = 0;

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_wifi_connected = false;
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        if (!s_wifi_got_ip_once) {
            /* First boot attempt: give up after N tries so wifi_init() can reboot. */
            if (retries < WIFI_INITIAL_RETRIES) {
                esp_wifi_connect();
                retries++;
                ESP_LOGW(TAG, "WiFi retry %d/%d...", retries, WIFI_INITIAL_RETRIES);
            } else {
                xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            }
        } else {
            /* Already been online: a router reboot / AP outage must never leave the
             * device permanently offline, so keep trying forever. Escalation to a
             * reboot is handled by the supervisor in wol_task(). */
            esp_wifi_connect();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "WiFi connected: " IPSTR, IP2STR(&ev->ip_info.ip));
        retries = 0;
        s_wifi_got_ip_once = true;
        s_wifi_connected = true;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid     = CONFIG_WOL_WIFI_SSID,
            .password = CONFIG_WOL_WIFI_PASSWORD,
            /* Never silently downgrade to an open / WEP / WPA1 look-alike AP
             * ("evil twin"). An empty password means the user chose an open network. */
            .threshold.authmode = (sizeof(CONFIG_WOL_WIFI_PASSWORD) - 1 == 0)
                                      ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = { .capable = true, .required = false },
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    /* Always-on, mains powered: prefer reliable inbound delivery over modem sleep. */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE, portMAX_DELAY);
    if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "WiFi connection failed after %d retries, restarting...", WIFI_INITIAL_RETRIES);
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
    }
}

/* ============================================================================
 * Wake-on-LAN
 * ========================================================================== */

/**
 * Build and broadcast a WoL magic packet on the local LAN.
 *
 * Uses a plain BSD socket bound to the WiFi STA address so the packet leaves
 * through WiFi and never through the WireGuard interface. If we cannot bind to
 * the STA address we refuse to send rather than risk leaking the broadcast
 * into the tunnel or dropping it silently.
 */
static esp_err_t wol_send_magic_packet(const wol_cfg_t *c)
{
    uint8_t pkt[WOL_PACKET_LEN];
    wol_build_magic(c->mac, pkt);

    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip_info;
    if (!sta || esp_netif_get_ip_info(sta, &ip_info) != ESP_OK || ip_info.ip.addr == 0) {
        ESP_LOGE(TAG, "No WiFi STA address - cannot send magic packet");
        return ESP_ERR_INVALID_STATE;
    }

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket() failed: errno %d", errno);
        return ESP_FAIL;
    }

    esp_err_t result = ESP_FAIL;

    int bcast = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &bcast, sizeof(bcast)) != 0) {
        ESP_LOGE(TAG, "SO_BROADCAST failed: errno %d", errno);
        goto out;
    }

    struct sockaddr_in src = {
        .sin_family      = AF_INET,
        .sin_port        = htons(0),
        .sin_addr.s_addr = ip_info.ip.addr,          /* already network byte order */
    };
    if (bind(sock, (struct sockaddr *)&src, sizeof(src)) != 0) {
        ESP_LOGE(TAG, "bind() to WiFi address failed: errno %d", errno);
        goto out;
    }

    struct sockaddr_in dest = {
        .sin_family      = AF_INET,
        .sin_port        = htons(9),                  /* WoL discard port */
        .sin_addr.s_addr = htonl(c->bcast_host_order),
    };

    ssize_t sent = sendto(sock, pkt, sizeof(pkt), 0, (struct sockaddr *)&dest, sizeof(dest));
    if (sent != (ssize_t)sizeof(pkt)) {
        ESP_LOGE(TAG, "sendto() sent %d/%d bytes (errno %d)", (int)sent, (int)sizeof(pkt), errno);
        goto out;
    }

    ESP_LOGI(TAG, "Magic packet sent -> %02X:%02X:%02X:%02X:%02X:%02X (via %s:9)",
             c->mac[0], c->mac[1], c->mac[2], c->mac[3], c->mac[4], c->mac[5],
             CONFIG_WOL_BROADCAST_IP);
    result = ESP_OK;

out:
    close(sock);
    return result;
}

/* ============================================================================
 * Trigger reception: callback -> queue -> wol_task
 *
 * The previous version polled ts_udp_recv() from wol_task. ts_udp_recv() calls
 * ts_update() internally, and app_main() calls ts_update() as well, so the
 * Tailscale state machine, DERP TLS context and TX queue were driven from two
 * tasks at once with no locking. Receiving through the callback keeps
 * ts_update() single-threaded (app_main only).
 * ========================================================================== */

typedef struct {
    uint32_t src_ip;
    uint16_t src_port;
    uint8_t  len;
    uint8_t  data[WOL_MAX_PAYLOAD];
} wol_msg_t;

static QueueHandle_t       s_rx_queue;
static volatile uint32_t   s_dropped_rx;   /* oversize or queue-full datagrams */

static void on_udp_rx(ts_udp_socket_t *sock, uint32_t src_ip, uint16_t src_port,
                      const uint8_t *data, size_t len, void *user_arg)
{
    (void)sock;
    (void)user_arg;

    s_udp_rx_count++;
    if (!data || len == 0 || len > WOL_MAX_PAYLOAD) {
        s_dropped_rx++;
        return;
    }

    wol_msg_t m;
    m.src_ip   = src_ip;
    m.src_port = src_port;
    m.len      = (uint8_t)len;
    memcpy(m.data, data, len);

    if (xQueueSend(s_rx_queue, &m, 0) != pdTRUE) {
        s_dropped_rx++;
    }
}

static inline int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

/* Log at most one "rejected" line per 5 s so a hostile peer cannot flood the log. */
static void log_reject(uint32_t src_ip, const char *why)
{
    static int64_t last_ms = INT64_MIN;
    static uint32_t suppressed = 0;
    int64_t t = now_ms();

    if (last_ms != INT64_MIN && t - last_ms >= 0 && t - last_ms < 5000) {
        suppressed++;
        return;
    }
    char ip[16];
    ESP_LOGW(TAG, "Rejected trigger from %s: %s (%lu more suppressed)",
             ts_vpn_ip_to_str(src_ip, ip), why, (unsigned long)suppressed);
    suppressed = 0;
    last_ms = t;
}

/* Trim one optional LF or CRLF without copying the UDP payload. */
static size_t payload_content_len(const uint8_t *data, size_t len)
{
    if (len >= 2 && data[len - 2] == '\r' && data[len - 1] == '\n') return len - 2;
    if (len >= 1 && data[len - 1] == '\n') return len - 1;
    return len;
}

static bool payload_is_data(const wol_msg_t *m)
{
    size_t len = payload_content_len(m->data, m->len);
    return len == 4 && memcmp(m->data, "data", 4) == 0;
}

/* status is authenticated with the same configured shared token as wake. If
 * no token is configured, the existing source-IP allow-list remains the gate. */
static bool payload_is_status(const wol_msg_t *m)
{
    size_t len = payload_content_len(m->data, m->len);
    size_t token_len = strlen(CONFIG_WOL_AUTH_TOKEN);
    static const char prefix[] = "status ";

    if (token_len == 0) return len == 6 && memcmp(m->data, "status", 6) == 0;
    if (len != (sizeof(prefix) - 1) + token_len) return false;
    if (memcmp(m->data, prefix, sizeof(prefix) - 1) != 0) return false;
    return wol_ct_equal(m->data + sizeof(prefix) - 1,
                        CONFIG_WOL_AUTH_TOKEN, token_len);
}

static bool send_reply(const wol_msg_t *m, ts_udp_socket_t *sock,
                       const char *payload, size_t len)
{
    esp_err_t err = ts_udp_send(sock, m->src_ip, m->src_port, payload, len);
    if (err == ESP_OK) {
        s_diag.ack_count++;
        snprintf(s_diag.last_ack, sizeof(s_diag.last_ack), "SENT");
        return true;
    }

    snprintf(s_diag.last_ack, sizeof(s_diag.last_ack), "FAILED");
    snprintf(s_diag.last_error, sizeof(s_diag.last_error), "UDP_ACK_FAILED");
    ESP_LOGW(TAG, "UDP reply failed (err=%s)", esp_err_to_name(err));
    return false;
}

static void send_status(const wol_msg_t *m, ts_udp_socket_t *sock, ts_t *ml)
{
    char response[256];
    int n = snprintf(response, sizeof(response),
                     "ESP32: ONLINE\nWIFI: %s\nTAILSCALE: %s\n"
                     "LAST_COMMAND: %s\nLAST_AUTH: %s\nLAST_WOL: %s\n"
                     "LAST_ERROR: %s\nLAST_ACK: %s\nUDP_RX: %lu\nUDP_ACK: %lu",
                     s_wifi_connected ? "CONNECTED" : "DISCONNECTED",
                     ts_is_connected(ml) ? "CONNECTED" : "DISCONNECTED",
                     s_diag.last_command, s_diag.last_auth, s_diag.last_wol,
                     s_diag.last_error, s_diag.last_ack,
                     (unsigned long)s_udp_rx_count,
                     (unsigned long)s_diag.ack_count);

    if (n < 0) {
        snprintf(s_diag.last_error, sizeof(s_diag.last_error), "STATUS_FORMAT_FAILED");
        return;
    }
    size_t len = (size_t)n < sizeof(response) ? (size_t)n : sizeof(response) - 1;
    send_reply(m, sock, response, len);
}

static void handle_trigger(const wol_msg_t *m, ts_udp_socket_t *sock,
                           wol_limiter_t *limiter, ts_t *ml)
{
    char ip[16];
    static const char legacy_ack[] = "ok";
    static const char received_ack[] = "ESP32_RECEIVED";
    static const char accepted_ack[] = "COMMAND_ACCEPTED";
    static const char sent_ack[] = "WOL_PACKET_SENT";
    static const char failed_ack[] = "WOL_SEND_FAILED";

    snprintf(s_diag.last_command, sizeof(s_diag.last_command), "UNKNOWN");

    if (!wol_allowlist_permits(&s_cfg.allow, m->src_ip)) {
        snprintf(s_diag.last_auth, sizeof(s_diag.last_auth), "REJECTED");
        snprintf(s_diag.last_error, sizeof(s_diag.last_error), "SOURCE_DENIED");
        log_reject(m->src_ip, "source not in allow-list");
        return;                                            /* silent drop */
    }

    /* A liveness probe cannot trigger WoL. The source allow-list still applies. */
    if (payload_is_data(m)) {
        snprintf(s_diag.last_command, sizeof(s_diag.last_command), "DATA");
        snprintf(s_diag.last_auth, sizeof(s_diag.last_auth), "NOT_REQUIRED");
        send_reply(m, sock, received_ack, sizeof(received_ack) - 1);
        return;
    }

    if (payload_is_status(m)) {
        snprintf(s_diag.last_command, sizeof(s_diag.last_command), "STATUS");
        snprintf(s_diag.last_auth, sizeof(s_diag.last_auth), "ACCEPTED");
        send_status(m, sock, ml);
        return;
    }

    wol_verdict_t v = wol_check_payload(m->data, m->len, CONFIG_WOL_AUTH_TOKEN);
    if (v != WOL_PKT_OK) {
        snprintf(s_diag.last_auth, sizeof(s_diag.last_auth), "REJECTED");
        snprintf(s_diag.last_error, sizeof(s_diag.last_error),
                 v == WOL_PKT_BAD_TOKEN ? "BAD_WAKE_TOKEN" : "BAD_COMMAND_FORMAT");
        log_reject(m->src_ip, v == WOL_PKT_BAD_TOKEN ? "bad token" : "bad format");
        return;                                            /* silent drop */
    }

    snprintf(s_diag.last_command, sizeof(s_diag.last_command), "WAKE");
    snprintf(s_diag.last_auth, sizeof(s_diag.last_auth), "ACCEPTED");
    ts_vpn_ip_to_str(m->src_ip, ip);

    /* Keep the original exact "ok" response for scripts/wake-pc compatibility. */
    send_reply(m, sock, accepted_ack, sizeof(accepted_ack) - 1);

    if (!wol_limiter_try(limiter, now_ms(),
                         (uint32_t)CONFIG_WOL_COOLDOWN_SECONDS * 1000u)) {
        ESP_LOGI(TAG, "Trigger from %s:%u inside cooldown - ACK only",
                 ip, (unsigned)m->src_port);
        send_reply(m, sock, legacy_ack, sizeof(legacy_ack) - 1);
        snprintf(s_diag.last_wol, sizeof(s_diag.last_wol), "COOLDOWN");
        return;
    }

    ESP_LOGI(TAG, "Authenticated trigger from %s:%u - firing WoL",
             ip, (unsigned)m->src_port);
    snprintf(s_diag.last_wol, sizeof(s_diag.last_wol), "ATTEMPTED");
    if (wol_send_magic_packet(&s_cfg) == ESP_OK) {
        snprintf(s_diag.last_wol, sizeof(s_diag.last_wol), "SENT");
        send_reply(m, sock, legacy_ack, sizeof(legacy_ack) - 1);
        send_reply(m, sock, sent_ack, sizeof(sent_ack) - 1);
    } else {
        /* Allow an immediate retry: a failed send must not consume cooldown. */
        limiter->used = false;
        snprintf(s_diag.last_wol, sizeof(s_diag.last_wol), "FAILED");
        snprintf(s_diag.last_error, sizeof(s_diag.last_error), "WOL_SEND_FAILED");
        send_reply(m, sock, failed_ack, sizeof(failed_ack) - 1);
    }
}

/* ============================================================================
 * Supervisor: self-heal from permanent faults
 * ========================================================================== */

static void supervise(bool connected)
{
    static int64_t disconnected_since = -1;
    static int     low_heap_seconds   = 0;
    static int     heap_log_countdown = 0;

    int64_t t = now_ms();

    if (connected) {
        disconnected_since = -1;
    } else if (disconnected_since < 0) {
        disconnected_since = t;
    } else if (CONFIG_WOL_DISCONNECT_RESTART_MIN > 0 &&
               t - disconnected_since > (int64_t)CONFIG_WOL_DISCONNECT_RESTART_MIN * 60 * 1000) {
        ESP_LOGE(TAG, "Tailscale down for >%d min - restarting", CONFIG_WOL_DISCONNECT_RESTART_MIN);
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
    }

    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);

    if (CONFIG_WOL_MIN_FREE_BLOCK_BYTES > 0) {
        if (largest < (size_t)CONFIG_WOL_MIN_FREE_BLOCK_BYTES) {
            if (++low_heap_seconds >= 60) {
                ESP_LOGE(TAG, "Heap exhausted (largest free block %u B) - restarting", (unsigned)largest);
                vTaskDelay(pdMS_TO_TICKS(200));
                esp_restart();
            }
        } else {
            low_heap_seconds = 0;
        }
    }

    if (heap_log_countdown-- <= 0) {
        heap_log_countdown = 300;   /* every ~5 min; use this to tune WOL_MIN_FREE_BLOCK_BYTES */
        ESP_LOGI(TAG, "heap: free=%u min_ever=%u largest_block=%u dropped_rx=%lu",
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)esp_get_minimum_free_heap_size(),
                 (unsigned)largest,
                 (unsigned long)s_dropped_rx);
    }
}

/* ============================================================================
 * WoL task
 * ========================================================================== */

static void wol_task(void *arg)
{
    ts_t *ml = (ts_t *)arg;

    ESP_LOGI(TAG, "Target MAC: %02X:%02X:%02X:%02X:%02X:%02X",
             s_cfg.mac[0], s_cfg.mac[1], s_cfg.mac[2],
             s_cfg.mac[3], s_cfg.mac[4], s_cfg.mac[5]);

    ESP_LOGI(TAG, "Waiting for Tailscale...");
    while (!ts_is_connected(ml)) {
        supervise(false);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    char vpn_ip[16];
    ts_vpn_ip_to_str(ts_get_vpn_ip(ml), vpn_ip);

    ESP_LOGI(TAG, "================================================");
    ESP_LOGI(TAG, " ESP32 Wake-on-LAN  -  READY");
    ESP_LOGI(TAG, " Tailscale IP : %s", vpn_ip);
    ESP_LOGI(TAG, " Listen port  : %d (Tailscale interface only)", CONFIG_WOL_LISTEN_PORT);
    ESP_LOGI(TAG, " Token        : %s", s_cfg.have_token ? "required" : "not set");
    ESP_LOGI(TAG, " Allow-list   : %u address(es)%s", (unsigned)s_cfg.allow.count,
             s_cfg.allow.count ? "" : " (any peer)");
    ESP_LOGI(TAG, " Cooldown     : %d s", CONFIG_WOL_COOLDOWN_SECONDS);
    ESP_LOGI(TAG, " Trigger      : wake-pc  (or: echo \"wake <token>\" | nc -u %s %d -w1)",
             vpn_ip, CONFIG_WOL_LISTEN_PORT);
    ESP_LOGI(TAG, "================================================");

    wol_limiter_t limiter = { .last_ms = 0, .used = false };
    ts_udp_socket_t *sock = NULL;
    bool prev_connected = true;   /* already confirmed connected above */

    for (;;) {
        bool connected = ts_is_connected(ml);
        supervise(connected);

        /* Coordination went down and came back: rebuild the socket so we get fresh
         * WireGuard sessions and resend CallMeMaybe to all peers. */
        if (connected && !prev_connected && sock) {
            ESP_LOGI(TAG, "Tailscale reconnected - refreshing UDP socket");
            ts_udp_close(sock);
            sock = NULL;
            xQueueReset(s_rx_queue);
        }
        prev_connected = connected;

        /* ts_udp_create() requires the connected state to bind to the WireGuard netif. */
        if (connected && !sock) {
            sock = ts_udp_create(ml, CONFIG_WOL_LISTEN_PORT);
            if (!sock) {
                ESP_LOGE(TAG, "Failed to create UDP socket, retrying in 2 s...");
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            }
            if (ts_udp_set_rx_callback(sock, on_udp_rx, NULL) != ESP_OK) {
                ESP_LOGE(TAG, "Failed to register RX callback, retrying...");
                ts_udp_close(sock);
                sock = NULL;
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            }
            ESP_LOGI(TAG, "UDP socket ready on Tailscale port %d", CONFIG_WOL_LISTEN_PORT);
        }

        /* Block until a trigger arrives, or 1 s passes (drives supervise()). */
        wol_msg_t m;
        if (xQueueReceive(s_rx_queue, &m, pdMS_TO_TICKS(1000)) == pdTRUE && sock) {
            handle_trigger(&m, sock, &limiter, ml);
        }
    }
}

/* ============================================================================
 * Tailscale callbacks
 * ========================================================================== */

static void on_connected(void)
{
    ESP_LOGI(TAG, "*** Tailscale CONNECTED ***");
}

static void on_disconnected(void)
{
    ESP_LOGW(TAG, "Tailscale disconnected");
}

static void on_state_change(ts_state_t old_s, ts_state_t new_s)
{
    ESP_LOGI(TAG, "Tailscale state: %s -> %s",
             ts_state_to_str(old_s),
             ts_state_to_str(new_s));
}

/* ============================================================================
 * app_main
 * ========================================================================== */

/* Possibly-transient failure: wait (so we do not hammer the control server in a
 * tight reboot loop) and reboot. */
static void fatal_restart(const char *why)
{
    ESP_LOGE(TAG, "%s - restarting in 30 s", why);
    vTaskDelay(pdMS_TO_TICKS(30000));
    esp_restart();
}

void app_main(void)
{
    ESP_LOGI(TAG, "ESP32 Wake-on-LAN  (Tailscale lib %s)", TS_VERSION);
    ESP_LOGI(TAG, "Free heap at boot: %lu B", (unsigned long)esp_get_free_heap_size());

    /* Validate configuration BEFORE touching the network: a misconfigured or
     * unauthenticated build must not join the tailnet. Config errors are not
     * transient, so do not reboot-loop; just stop here. */
    if (!load_config(&s_cfg)) {
        ESP_LOGE(TAG, "Configuration invalid - not starting. Run: idf.py menuconfig"
                      " -> Wake-on-LAN Configuration");
        return;
    }

    /* NVS - required by WiFi driver and used for the Tailscale machine key */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    s_rx_queue = xQueueCreate(4, sizeof(wol_msg_t));
    if (!s_rx_queue) {
        fatal_restart("Cannot allocate RX queue");
        return;
    }

    ESP_LOGI(TAG, "Connecting to WiFi '%s'...", CONFIG_WOL_WIFI_SSID);
    wifi_init();
    ESP_LOGI(TAG, "Free heap after WiFi: %lu B", (unsigned long)esp_get_free_heap_size());

    /* Tailscale - memory-optimised for no-PSRAM ESP32 */
    ts_config_t cfg;
    ts_get_default_config(&cfg);

    cfg.auth_key            = CONFIG_WOL_TAILSCALE_AUTH_KEY;
    cfg.device_name         = CONFIG_WOL_DEVICE_NAME;
    cfg.enable_derp         = true;   /* Required for remote NAT traversal */
    cfg.enable_disco        = true;
    cfg.enable_stun         = true;
    cfg.max_peers           = 4;      /* Low value saves SRAM */
    cfg.on_connected        = on_connected;
    cfg.on_disconnected     = on_disconnected;
    cfg.on_state_change     = on_state_change;

    ts_t *ml = ts_init(&cfg);
    if (!ml) {
        ESP_LOGE(TAG, "Free heap: %lu B", (unsigned long)esp_get_free_heap_size());
        ESP_LOGE(TAG, "Try reducing CONFIG_TS_COORD_BUFFER_SIZE_KB (minimum 16)");
        fatal_restart("Tailscale init failed - likely out of memory");
        return;
    }

    ESP_LOGI(TAG, "Free heap after Tailscale init: %lu B", (unsigned long)esp_get_free_heap_size());

    if (ts_connect(ml) != ESP_OK) {
        fatal_restart("ts_connect() failed");
        return;
    }

    /* WoL listener runs in its own task. */
    if (xTaskCreate(wol_task, "wol", 6144, ml, 5, NULL) != pdPASS) {
        fatal_restart("Cannot create wol task");
        return;
    }

    /* Drive the Tailscale state machine from this task ONLY. */
    while (1) {
        ts_update(ml);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
