/* BLE advertising on the NimBLE host (Bluedroid is off in sdkconfig.defaults —
 * an advert-only device has no use for it). Payload is the stock Rapt layout,
 * so Home Assistant and raptpill2mqtt keep parsing us. */
#include <string.h>

#include "board.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_store.h"
#include "nimble/nimble_port.h"
#include "services/gap/ble_svc_gap.h"

#include "pill_ble.h"
#include "pill_wire.h"

static const char *TAG = "pill_ble";
static uint8_t g_adv[2 + 2 + PILL_ADV_BODY_LEN];
static pill_sample_t g_latest;
static bool g_synced;

static void adv_build(const pill_sample_t *s)
{
    uint8_t body[PILL_ADV_BODY_LEN];
    pill_adv_build(body, s);
    g_adv[0] = 2 + PILL_ADV_BODY_LEN + 2;   /* len: type + company id + body */
    g_adv[1] = 0xff;                        /* manufacturer specific data */
    pill_adv_prefix(g_adv + 2);             /* company id LE + "PT" */
    memcpy(g_adv + 4, body + 2, PILL_ADV_BODY_LEN - 2);
}

static void adv_start(void)
{
    struct ble_gap_adv_params p = {
        .conn_mode = BLE_GAP_CONN_MODE_NON,      /* non-connectable beacon */
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
        .itvl_min = PILL_ADV_INTERVAL_UNITS,
        .itvl_max = PILL_ADV_INTERVAL_UNITS,
        /* channel_map 0 = all three advertising channels (stack default) */
    };
    int rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER, &p, NULL, NULL);
    if (rc) ESP_LOGW(TAG, "ble_gap_adv_start: %d", rc);
}

static void host_sync(void)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char name[24];
    snprintf(name, sizeof(name), "RAPT-Pill-%02X%02X%02X", mac[3], mac[4], mac[5]);
    ble_svc_gap_device_name_set(name);
    g_synced = true;
    adv_start();
}

static void host_reset(int reason)
{
    ESP_LOGW(TAG, "nimble reset, reason %d", reason);
    g_synced = false;
}

static void host_task(void *param)
{
    nimble_port_run();
    vTaskDelete(NULL);
}

void pill_ble_start(const pill_sample_t *s)
{
    g_latest = *s;
    adv_build(&g_latest);
    ESP_ERROR_CHECK(nimble_port_init());
    ble_svc_gap_init();
    ble_hs_cfg.sync_cb = host_sync;
    ble_hs_cfg.reset_cb = host_reset;
    xTaskCreate(host_task, "nimble_host", 4096, NULL, 5, NULL);
}

/* NimBLE will not swap the payload mid-advertise: stop, set, start. At a
 * 15-minute sample cycle the few hundred ms gap is invisible. */
void pill_ble_update(const pill_sample_t *s)
{
    g_latest = *s;
    if (!g_synced) return;
    ble_gap_adv_stop();
    adv_build(&g_latest);
    int rc = ble_gap_adv_set_data(g_adv, sizeof(g_adv));
    if (rc) ESP_LOGW(TAG, "ble_gap_adv_set_data: %d", rc);
    adv_start();
}
