/* Browser firmware upload, the FermentDial way: a page with a file input, a
 * multipart POST, and the bytes streamed straight into the other OTA slot.
 *
 * ESP-IDF 5.4's http_server has no form parser (the file_serving example hand
 * rolls it), so this is a minimal streaming multipart reader: skip the part
 * headers, then write body bytes to esp_ota while holding back the last
 * boundary+4 bytes, because the terminating "\r\n--boundary" has to be cut off
 * before it reaches the flash.
 *
 * The slot boots once: with CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y the image
 * stays PENDING_VERIFY until app_main marks it valid, so a bad .bin reverts.
 */
#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"

#include "pill_upload.h"

static const char *TAG = "pill_upload";
#define CHUNK 4096   /* 512 measured 18-38 KB/s: per-chunk flash writes and TCP
                      * round trips dominate, and the buffers are static */

static bool boundary_of(httpd_req_t *req, char *out, size_t outlen)
{
    char ct[160] = "";
    if (httpd_req_get_hdr_value_str(req, "Content-Type", ct, sizeof(ct)) != ESP_OK)
        return false;
    char *b = strstr(ct, "boundary=");
    if (!b) return false;
    b += 9;
    if (*b == '"') b++;
    size_t i = 0;
    while (b[i] && b[i] != '"' && b[i] != ';' && i + 1 < outlen) {
        out[i] = b[i];
        i++;
    }
    out[i] = 0;
    return i >= 4;
}

static const uint8_t *find_bytes(const uint8_t *p, size_t n, const char *m, size_t ml)
{
    if (ml > n) return NULL;
    for (size_t i = 0; i + ml <= n; i++)
        if (!memcmp(p + i, m, ml)) return p + i;
    return NULL;
}

esp_err_t pill_upload_handler(httpd_req_t *req)
{
    char boundary[72];
    if (!boundary_of(req, boundary, sizeof(boundary))) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "multipart/form-data with a boundary expected");
    }
    char marker[80];
    int mlen = snprintf(marker, sizeof(marker), "\r\n--%s", boundary);

    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "no free OTA slot");
    }
    if (req->content_len > part->size) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "image larger than the OTA slot");
    }

    esp_ota_handle_t ota;
    esp_err_t e = esp_ota_begin(part, OTA_SIZE_UNKNOWN, &ota);
    if (e != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, esp_err_to_name(e));
    }

    /* Static, not stack: the httpd task runs on 4 KB and this handler owns the
     * connection for the whole upload anyway. */
    static uint8_t buf[CHUNK];
    static uint8_t pend[CHUNK + 128];
    int pend_len = 0;
    bool in_body = false;
    bool done = false;
    int written = 0;
    char why[64] = "";

    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);

    int remaining = req->content_len;
    while (remaining > 0 && !why[0]) {
        int n = httpd_req_recv(req, (char *)buf, remaining < CHUNK ? remaining : CHUNK);
        if (n <= 0) {
            if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
            snprintf(why, sizeof(why), "connection dropped at %d bytes", written);
            break;
        }
        remaining -= n;
        memcpy(pend + pend_len, buf, n);
        pend_len += n;

        if (!in_body) {
            /* Part headers end at the first \r\n\r\n; the file starts after it. */
            const uint8_t *hdr = find_bytes(pend, pend_len, "\r\n\r\n", 4);
            if (!hdr) continue;
            const uint8_t *start = hdr + 4;
            pend_len = pend + pend_len - start;
            memmove(pend, start, pend_len);
            in_body = true;
        }

        /* Emit everything except the last mlen+2 bytes, which may be the start
         * of the closing boundary. */
        const uint8_t *hit = find_bytes(pend, pend_len, marker, mlen);
        int emit = hit ? (int)(hit - pend) : pend_len - (mlen + 2);
        if (emit < 0) emit = 0;
        if (emit > 0) {
            mbedtls_sha256_update(&sha, pend, emit);
            e = esp_ota_write(ota, pend, emit);
            if (e != ESP_OK) {
                snprintf(why, sizeof(why), "esp_ota_write: %s", esp_err_to_name(e));
                break;
            }
            written += emit;
            memmove(pend, pend + emit, pend_len - emit);
            pend_len -= emit;
        }
        if (hit) { done = true; break; }
    }

    uint8_t digest[32];
    mbedtls_sha256_finish(&sha, digest);
    mbedtls_sha256_free(&sha);

    char hex[65] = {0};
    for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", digest[i]);

    if (!done && !why[0]) snprintf(why, sizeof(why), "no end of part found");

    if (!why[0]) {
        ESP_LOGI(TAG, "%d bytes into %s, sha256 %s", written, part->label, hex);
        e = esp_ota_end(ota);
        if (e != ESP_OK) snprintf(why, sizeof(why), "esp_ota_end: %s", esp_err_to_name(e));
    } else {
        esp_ota_abort(ota);
    }
    if (!why[0]) {
        e = esp_ota_set_boot_partition(part);
        if (e != ESP_OK) snprintf(why, sizeof(why), "set_boot_partition: %s", esp_err_to_name(e));
    }

    httpd_resp_set_type(req, "text/html");
    if (why[0]) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        char html[256];
        snprintf(html, sizeof(html), "<p>%s</p>"
                 "<p><a href=/firmware>Back</a></p></div></main></body></html>", why);
        httpd_resp_sendstr_chunk(req, PILL_RESULT_HTML("Update failed"));
        httpd_resp_sendstr_chunk(req, html);
        return httpd_resp_sendstr_chunk(req, NULL);
    }

    char app[80] = "?";
    esp_app_desc_t d;
    if (esp_ota_get_partition_description(part, &d) == ESP_OK)
        snprintf(app, sizeof(app), "%.31s %.31s", d.project_name, d.version);

    char html[512];
    snprintf(html, sizeof(html),
             "<p>%d bytes to <code>%s</code>.<br>sha256 <code>%.16s…</code><br>%s</p>"
             "<p>The new build boots once. If it does not reach the network it "
             "rolls itself back to <code>%s</code>. Rebooting…</p>"
             "<p><a href=/>Home</a></p></div></main></body></html>",
             written, part->label, hex, app,
             esp_ota_get_running_partition()->label);
    httpd_resp_sendstr_chunk(req, PILL_RESULT_HTML("Update complete"));
    httpd_resp_sendstr_chunk(req, html);
    ESP_LOGI(TAG, "rebooting into %s", part->label);
    esp_err_t r = httpd_resp_sendstr_chunk(req, NULL);
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
    return r;
}
