#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"

#include "dhcpserver/dhcpserver.h"
#include "lwip/inet.h"
#include "lwip/ip_addr.h"
#include "lwip/sockets.h"

#include "typen_wifi.h"

#ifndef CONFIG_TYPEN_WIFI_CHANNEL
#define CONFIG_TYPEN_WIFI_CHANNEL 1
#endif

#ifndef CONFIG_TYPEN_WIFI_MAX_CLIENTS
#define CONFIG_TYPEN_WIFI_MAX_CLIENTS 2
#endif

#define WIFI_AP_STARTED_BIT BIT0
#define WIFI_START_TIMEOUT_MS 12000
#define DNS_PORT 53
#define DNS_PACKET_MAX 512
#define TYPEN_SETUP_CHANNEL 6
#define TYPEN_BEACON_INTERVAL 100

static const char *TAG = "typen_wifi";

/*
 * Current product baseline is intentionally an open setup AP.
 * Do not source these from an old sdkconfig yet: existing sdkconfig files may
 * still contain obsolete setup credentials from earlier bring-up builds.
 *
 * A runtime password option can replace this later without changing Hosted.
 */
static const char *TYPEN_SETUP_SSID = "TypeN Setup";

static EventGroupHandle_t s_wifi_events = NULL;
static esp_netif_t *s_ap_netif = NULL;
static TaskHandle_t s_dns_task = NULL;
static char s_ip[32] = "192.168.4.1";
static char s_captive_uri[64] = "http://192.168.4.1/";
static bool s_started = false;
static bool s_captive_started = false;

/*
 * Match the proven TypeD XL setup behavior: the setup network owns a fixed
 * 192.168.4.1/24 address before the radio starts advertising the AP.
 */
static esp_err_t configure_ap_netif(void)
{
    esp_netif_ip_info_t info = {0};

    IP4_ADDR(&info.ip, 192, 168, 4, 1);
    IP4_ADDR(&info.gw, 192, 168, 4, 1);
    IP4_ADDR(&info.netmask, 255, 255, 255, 0);

    esp_err_t err = esp_netif_dhcps_stop(s_ap_netif);
    if (err != ESP_OK &&
        err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        ESP_LOGE(TAG, "Unable to stop SoftAP DHCP: %s", esp_err_to_name(err));
        return err;
    }

    ESP_RETURN_ON_ERROR(
        esp_netif_set_ip_info(s_ap_netif, &info),
        TAG,
        "Unable to set SoftAP IP"
    );

    ESP_RETURN_ON_ERROR(
        esp_netif_dhcps_start(s_ap_netif),
        TAG,
        "Unable to start SoftAP DHCP"
    );

    snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&info.ip));
    return ESP_OK;
}

static void log_ap_readback(void)
{
    wifi_mode_t mode = WIFI_MODE_NULL;
    wifi_config_t readback = {0};
    uint8_t mac[6] = {0};

    esp_err_t mode_err = esp_wifi_get_mode(&mode);
    esp_err_t cfg_err = esp_wifi_get_config(WIFI_IF_AP, &readback);
    esp_err_t mac_err = esp_wifi_get_mac(WIFI_IF_AP, mac);

    if (mode_err == ESP_OK) {
        ESP_LOGI(TAG, "C6 readback mode=%d (AP=%d)", (int)mode, (int)WIFI_MODE_AP);
    } else {
        ESP_LOGW(TAG, "C6 mode readback failed: %s", esp_err_to_name(mode_err));
    }

    if (cfg_err == ESP_OK) {
        ESP_LOGI(
            TAG,
            "C6 AP readback: SSID='%.*s' len=%u ch=%u auth=%d hidden=%u beacon=%u",
            (int)readback.ap.ssid_len,
            (const char *)readback.ap.ssid,
            (unsigned)readback.ap.ssid_len,
            (unsigned)readback.ap.channel,
            (int)readback.ap.authmode,
            (unsigned)readback.ap.ssid_hidden,
            (unsigned)readback.ap.beacon_interval
        );
    } else {
        ESP_LOGW(TAG, "C6 AP config readback failed: %s", esp_err_to_name(cfg_err));
    }

    if (mac_err == ESP_OK) {
        ESP_LOGI(TAG, "C6 AP MAC: " MACSTR, MAC2STR(mac));
    } else {
        ESP_LOGW(TAG, "C6 AP MAC readback failed: %s", esp_err_to_name(mac_err));
    }
}

static void wifi_event_handler(
    void *arg,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data)
{
    (void)arg;
    (void)event_base;

    switch (event_id) {
        case WIFI_EVENT_AP_START:
            ESP_LOGI(TAG, "SoftAP start event");
            if (s_wifi_events) {
                xEventGroupSetBits(
                    s_wifi_events,
                    WIFI_AP_STARTED_BIT
                );
            }
            break;

        case WIFI_EVENT_AP_STACONNECTED: {
            wifi_event_ap_staconnected_t *event =
                (wifi_event_ap_staconnected_t *)event_data;

            ESP_LOGI(
                TAG,
                "station " MACSTR " joined, AID=%d",
                MAC2STR(event->mac),
                event->aid
            );
            break;
        }

        case WIFI_EVENT_AP_STADISCONNECTED: {
            wifi_event_ap_stadisconnected_t *event =
                (wifi_event_ap_stadisconnected_t *)event_data;

            ESP_LOGI(
                TAG,
                "station " MACSTR " left, AID=%d",
                MAC2STR(event->mac),
                event->aid
            );
            break;
        }

        default:
            break;
    }
}

static size_t dns_question_end(
    const uint8_t *packet,
    size_t length)
{
    if (packet == NULL || length < 17U) {
        return 0;
    }

    size_t pos = 12U;

    while (pos < length) {
        uint8_t label_len = packet[pos++];

        if (label_len == 0U) {
            break;
        }

        /* Compressed names are not expected in normal client questions. */
        if ((label_len & 0xC0U) != 0U ||
            label_len > 63U ||
            pos + label_len > length) {
            return 0;
        }

        pos += label_len;
    }

    if (pos + 4U > length) {
        return 0;
    }

    return pos + 4U;
}

static void dns_server_task(void *arg)
{
    (void)arg;

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

    if (sock < 0) {
        ESP_LOGE(TAG, "Captive DNS socket create failed");
        s_dns_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in bind_addr = {0};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(DNS_PORT);
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(
            sock,
            (struct sockaddr *)&bind_addr,
            sizeof(bind_addr)) < 0) {
        ESP_LOGE(TAG, "Captive DNS bind failed");
        close(sock);
        s_dns_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    struct in_addr portal_addr = {0};

    if (inet_pton(
            AF_INET,
            s_ip,
            &portal_addr) != 1) {
        ESP_LOGE(TAG, "Captive DNS invalid portal IP: %s", s_ip);
        close(sock);
        s_dns_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Captive DNS listening on UDP/53 -> %s", s_ip);

    uint8_t query[DNS_PACKET_MAX];
    uint8_t response[DNS_PACKET_MAX];

    for (;;) {
        struct sockaddr_storage source_addr;
        socklen_t source_len = sizeof(source_addr);

        int received = recvfrom(
            sock,
            query,
            sizeof(query),
            0,
            (struct sockaddr *)&source_addr,
            &source_len
        );

        if (received <= 0) {
            continue;
        }

        size_t q_end = dns_question_end(
            query,
            (size_t)received
        );

        if (q_end == 0U || q_end > sizeof(response)) {
            continue;
        }

        const size_t qtype_pos = q_end - 4U;
        const uint16_t qtype =
            ((uint16_t)query[qtype_pos] << 8) |
            query[qtype_pos + 1U];
        const uint16_t qclass =
            ((uint16_t)query[qtype_pos + 2U] << 8) |
            query[qtype_pos + 3U];

        memcpy(response, query, q_end);

        /* Standard response, recursion desired/available, no error. */
        response[2] = 0x81;
        response[3] = 0x80;
        response[4] = 0x00;
        response[5] = 0x01;
        response[6] = 0x00;
        response[7] = 0x00;
        response[8] = 0x00;
        response[9] = 0x00;
        response[10] = 0x00;
        response[11] = 0x00;

        size_t response_len = q_end;

        if (qtype == 1U && qclass == 1U &&
            response_len + 16U <= sizeof(response)) {
            /* One A-record answer using a pointer to QNAME at offset 12. */
            response[6] = 0x00;
            response[7] = 0x01;

            response[response_len++] = 0xC0;
            response[response_len++] = 0x0C;
            response[response_len++] = 0x00;
            response[response_len++] = 0x01;
            response[response_len++] = 0x00;
            response[response_len++] = 0x01;
            response[response_len++] = 0x00;
            response[response_len++] = 0x00;
            response[response_len++] = 0x00;
            response[response_len++] = 0x3C;
            response[response_len++] = 0x00;
            response[response_len++] = 0x04;

            memcpy(
                &response[response_len],
                &portal_addr.s_addr,
                4U
            );
            response_len += 4U;
        }

        sendto(
            sock,
            response,
            response_len,
            0,
            (struct sockaddr *)&source_addr,
            source_len
        );
    }
}

esp_err_t typen_wifi_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

    /* NVS is already initialized by typen_settings_init(). */
    esp_err_t err = esp_netif_init();

    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_event_loop_create_default();

    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event loop init failed: %s", esp_err_to_name(err));
        return err;
    }

    if (!s_wifi_events) {
        s_wifi_events = xEventGroupCreate();
        if (!s_wifi_events) {
            return ESP_ERR_NO_MEM;
        }
    }

    xEventGroupClearBits(s_wifi_events, WIFI_AP_STARTED_BIT);

    if (!s_ap_netif) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
        if (!s_ap_netif) {
            ESP_LOGE(TAG, "Failed to create SoftAP netif");
            return ESP_FAIL;
        }
    }

    ESP_RETURN_ON_ERROR(
        configure_ap_netif(),
        TAG,
        "SoftAP netif configuration failed"
    );

    /*
     * On ESP32-P4 this esp_wifi_* API is supplied by esp_wifi_remote and
     * routed through ESP-Hosted to the onboard ESP32-C6.
     */
    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();

    ESP_LOGI(TAG, "Initializing ESP-Hosted remote Wi-Fi");

    err = esp_wifi_init(&init_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "esp_wifi_init failed: %s (0x%x)",
            esp_err_to_name(err),
            (unsigned)err
        );
        return err;
    }

    /*
     * Keep setup AP configuration in RAM. This prevents stale radio settings
     * stored on the C6 from winning over the current TypeN setup profile.
     */
    ESP_RETURN_ON_ERROR(
        esp_wifi_set_storage(WIFI_STORAGE_RAM),
        TAG,
        "Wi-Fi RAM storage selection failed"
    );

    err = esp_event_handler_instance_register(
        WIFI_EVENT,
        ESP_EVENT_ANY_ID,
        wifi_event_handler,
        NULL,
        NULL
    );

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi event registration failed: %s", esp_err_to_name(err));
        return err;
    }

    wifi_config_t cfg = {0};
    const size_t ssid_len = strlen(TYPEN_SETUP_SSID);

    if (ssid_len == 0U || ssid_len >= sizeof(cfg.ap.ssid)) {
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(cfg.ap.ssid, TYPEN_SETUP_SSID, ssid_len);
    cfg.ap.ssid[ssid_len] = '\0';
    cfg.ap.ssid_len = (uint8_t)ssid_len;

    /* Intentionally open for the current setup flow. */
    cfg.ap.password[0] = '\0';
    cfg.ap.channel = TYPEN_SETUP_CHANNEL;
    cfg.ap.max_connection = CONFIG_TYPEN_WIFI_MAX_CLIENTS;
    cfg.ap.authmode = WIFI_AUTH_OPEN;
    cfg.ap.ssid_hidden = 0;
    cfg.ap.beacon_interval = TYPEN_BEACON_INTERVAL;
    cfg.ap.pmf_cfg.capable = false;
    cfg.ap.pmf_cfg.required = false;

    ESP_RETURN_ON_ERROR(
        esp_wifi_set_mode(WIFI_MODE_AP),
        TAG,
        "SoftAP mode failed"
    );

    ESP_RETURN_ON_ERROR(
        esp_wifi_set_config(WIFI_IF_AP, &cfg),
        TAG,
        "SoftAP config failed"
    );

    ESP_LOGI(
        TAG,
        "Starting visible open SoftAP '%s' on channel %u",
        TYPEN_SETUP_SSID,
        (unsigned)cfg.ap.channel
    );

    ESP_RETURN_ON_ERROR(
        esp_wifi_start(),
        TAG,
        "SoftAP start command failed"
    );

    const EventBits_t bits = xEventGroupWaitBits(
        s_wifi_events,
        WIFI_AP_STARTED_BIT,
        pdFALSE,
        pdTRUE,
        pdMS_TO_TICKS(WIFI_START_TIMEOUT_MS)
    );

    if ((bits & WIFI_AP_STARTED_BIT) == 0U) {
        ESP_LOGE(TAG, "Timed out waiting for remote SoftAP start");
        return ESP_ERR_TIMEOUT;
    }

    /* Give the remote C6 one beacon interval before reading its AP state. */
    vTaskDelay(pdMS_TO_TICKS(120));
    log_ap_readback();

    esp_netif_ip_info_t info = {0};

    if (esp_netif_get_ip_info(s_ap_netif, &info) == ESP_OK) {
        snprintf(
            s_ip,
            sizeof(s_ip),
            IPSTR,
            IP2STR(&info.ip)
        );
    }

    snprintf(
        s_captive_uri,
        sizeof(s_captive_uri),
        "http://%s/",
        s_ip
    );

    s_started = true;

    ESP_LOGI(TAG, "Local management AP ready");
    ESP_LOGI(TAG, "SSID: %s", TYPEN_SETUP_SSID);
    ESP_LOGI(TAG, "Security: OPEN");
    ESP_LOGI(TAG, "IP: %s", s_ip);

    return ESP_OK;
}

esp_err_t typen_wifi_enable_captive_portal(void)
{
    if (!s_started || s_ap_netif == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_captive_started) {
        return ESP_OK;
    }

    esp_netif_ip_info_t info = {0};
    ESP_RETURN_ON_ERROR(
        esp_netif_get_ip_info(s_ap_netif, &info),
        TAG,
        "Unable to read SoftAP IP"
    );

    /*
     * Reconfigure DHCP while no settings are being changed underneath it.
     * Clients joining after this point receive TypeN itself as DNS plus
     * RFC 8910 / DHCP option 114 pointing at the portal.
     */
    esp_err_t stop_err = esp_netif_dhcps_stop(s_ap_netif);
    if (stop_err != ESP_OK &&
        stop_err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        ESP_LOGW(TAG, "DHCP stop before captive config: %s", esp_err_to_name(stop_err));
    }

    dhcps_offer_t offer_dns = OFFER_DNS;
    ESP_RETURN_ON_ERROR(
        esp_netif_dhcps_option(
            s_ap_netif,
            ESP_NETIF_OP_SET,
            ESP_NETIF_DOMAIN_NAME_SERVER,
            &offer_dns,
            sizeof(offer_dns)
        ),
        TAG,
        "Unable to enable DHCP DNS offer"
    );

    esp_netif_dns_info_t dns = {0};
    dns.ip.type = IPADDR_TYPE_V4;
    dns.ip.u_addr.ip4.addr = info.ip.addr;

    ESP_RETURN_ON_ERROR(
        esp_netif_set_dns_info(
            s_ap_netif,
            ESP_NETIF_DNS_MAIN,
            &dns
        ),
        TAG,
        "Unable to set captive DNS address"
    );

    ESP_RETURN_ON_ERROR(
        esp_netif_dhcps_option(
            s_ap_netif,
            ESP_NETIF_OP_SET,
            ESP_NETIF_CAPTIVEPORTAL_URI,
            s_captive_uri,
            strlen(s_captive_uri)
        ),
        TAG,
        "Unable to set captive portal URI"
    );

    ESP_RETURN_ON_ERROR(
        esp_netif_dhcps_start(s_ap_netif),
        TAG,
        "Unable to restart DHCP server"
    );

    if (s_dns_task == NULL) {
        BaseType_t created = xTaskCreate(
            dns_server_task,
            "typen_dns",
            4096,
            NULL,
            4,
            &s_dns_task
        );

        if (created != pdPASS) {
            s_dns_task = NULL;
            return ESP_ERR_NO_MEM;
        }
    }

    s_captive_started = true;

    ESP_LOGI(TAG, "Captive portal enabled: %s", s_captive_uri);
    return ESP_OK;
}

const char *typen_wifi_ip_string(void)
{
    return s_ip;
}
