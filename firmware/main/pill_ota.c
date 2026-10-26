/* Self-update over the LAN. This is the piece stock firmware refuses to have:
 * a stock pill only ever fetches from Rapt's API (TLS-validated, checksummed,
 * and not forgeable), so installing anything else needs a cable. Once the CFW
 * is in, an install is a URL.
 *
 * Safety: the image goes into the *other* OTA slot, the bootloader is told to
 * boot it once, and the new build marks itself valid only after Wi-Fi has come
 * up. With CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y a build that panics, fails
 * to boot, or never associates rolls back by itself.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"

#include "pill_ota.h"

static const char *TAG = "pill_ota";
char g_want_sha[65];
static char g_status[160] = "idle";
static bool g_busy;

static void set_status(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_status, sizeof(g_status), fmt, ap);
    va_end(ap);
    ESP_LOGI(TAG, "%s", g_status);
}

static void ota_task(void *arg)
{
    char *url = arg;
    esp_http_client_config_t cfg = { .url = url, .timeout_ms = 15000 };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) { set_status("client init failed"); goto out; }

    esp_err_t e = esp_http_client_open(c, 0);
    if (e != ESP_OK) { set_status("open failed: %s", esp_err_to_name(e)); goto out; }
    int total = esp_http_client_fetch_headers(c);
    int status = esp_http_client_get_status_code(c);
    if (status != 200) { set_status("HTTP %d for %s", status, url); goto out; }

    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) { set_status("no free OTA slot"); goto out; }
    esp_ota_handle_t ota;
    e = esp_ota_begin(part, OTA_SIZE_UNKNOWN, &ota);
    if (e != ESP_OK) { set_status("esp_ota_begin: %s", esp_err_to_name(e)); goto out; }

    uint8_t buf[1024];
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);
    int got = 0;
    for (;;) {
        int n = esp_http_client_read(c, (char *)buf, sizeof(buf));
        if (n < 0) { set_status("read at %d bytes", got); break; }
        if (n == 0) break;
        mbedtls_sha256_update(&sha, buf, n);
        e = esp_ota_write(ota, buf, n);
        if (e != ESP_OK) { set_status("esp_ota_write: %s", esp_err_to_name(e)); break; }
        got += n;
        if (total > 0 && got <= total) {
            int pct = got * 100 / total;
            if (pct % 20 == 0) set_status("downloading %d%% (%d B)", pct, got);
        }
    }
    uint8_t digest[32];
    mbedtls_sha256_finish(&sha, digest);
    mbedtls_sha256_free(&sha);
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    c = NULL;

    char hex[65] = {0};
    for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", digest[i]);
    ESP_LOGI(TAG, "sha256 %s  (%d bytes into %s)", hex, got, part->label);

    if (g_want_sha[0] && strcmp(hex, g_want_sha)) {
        esp_ota_abort(ota);
        set_status("checksum mismatch, refused");
        goto out;
    }
    e = esp_ota_end(ota);
    if (e != ESP_OK) { set_status("esp_ota_end: %s", esp_err_to_name(e)); goto out; }
    e = esp_ota_set_boot_partition(part);
    if (e != ESP_OK) { set_status("set_boot_partition: %s", esp_err_to_name(e)); goto out; }

    set_status("installed %d bytes to %s, rebooting", got, part->label);
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();

out:
    if (c) { esp_http_client_close(c); esp_http_client_cleanup(c); }
    free(url);
    g_busy = false;
    vTaskDelete(NULL);
}

bool pill_ota_start(const char *url, const char *sha256_hex)
{
    if (g_busy) { set_status("already updating"); return false; }
    if (strncmp(url, "http://", 7) && strncmp(url, "https://", 8)) {
        set_status("url must be http(s)");
        return false;
    }
    g_want_sha[0] = 0;
    if (sha256_hex && strlen(sha256_hex) == 64) snprintf(g_want_sha, 65, "%s", sha256_hex);
    g_busy = true;
    set_status("fetching %s", url);
    xTaskCreate(ota_task, "ota", 8192, strdup(url), 6, NULL);
    return true;
}

const char *pill_ota_status(void) { return g_status; }

void pill_ota_confirm_boot(void)
{
    /* Only called once the network is up, which is as close to "this build
     * works" as an unattended device gets. */
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "marking %s valid, rollback disarmed", running->label);
        esp_ota_mark_app_valid_cancel_rollback();
    }
}
