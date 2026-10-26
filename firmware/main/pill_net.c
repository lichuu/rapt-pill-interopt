#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdarg.h>

#include <math.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "nvs.h"
#include "pill_settings.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board.h"
#include "gravity.h"
#include "pill_net.h"
#include "pill_mqtt.h"
#include "pill_ota.h"
#include "pill_upload.h"
#include "web_theme.h"

static const char *TAG = "pill_net";
static pill_sample_t g_sample;

/* Stock's gravity velocity, in RAM, over the grav_vel_time window. The ring and
 * its arithmetic live in pill_gv.h so tests/test_gv.c can drive them; this is
 * the one instance. */
static pill_gv_ring_t g_gv;

void pill_gv_push(uint32_t uptime_s, float points)
{
    pill_gv_add(&g_gv, uptime_s, points);
}

pill_gv_t pill_gv_get(void)
{
    return pill_gv_reduce(&g_gv, (uint32_t)(esp_timer_get_time() / 1000000),
                          pill_settings()->grav_vel_time);
}
static bool g_connected;
static bool g_has_creds;      /* a STA config with an SSID in it was applied */
static esp_timer_handle_t g_reconnect;
static uint32_t g_backoff_ms = 500;
static int g_attempt;
static char g_version[16] = "cfw";
static char g_ip[16] = "";
static float g_cal_deg;
static char g_expr[192];            /* installed expression, empty = built-in */
static void cal_apply(float deg);   /* degrees, not milli: see the writer below */
static void url_decode(char *s);   /* the pill's angle offset, degrees */
static gravity_model_t g_model = GRAVITY_MODEL_DEFAULT;

static char *json_dump(cJSON *root)
{
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return s;
}

static esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
    char *body = json_dump(root);
    if (!body) return ESP_ERR_NO_MEM;
    httpd_resp_set_type(req, "application/json");
    int rc = httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
    free(body);
    return rc;
}

static void set_hdr(const char *name, char *out, size_t len, httpd_req_t *req)
{
    if (httpd_req_get_hdr_value_str(req, name, out, len) != ESP_OK) out[0] = 0;
}

/* Stock's reported tilt is the calibrated angle: the live capture showed
 * tilt 71.6819 = atan2(y,x) 71.2266 + offset 0.4553. Report it the same way.
 * temp_f is ours; stock only ever speaks Celsius. */
static float tilt_deg(void) { return pill_angle_deg(g_sample.ax, g_sample.ay) + g_cal_deg; }
/* Every temperature the pill publishes carries stock's temp_offset. */
static float temp_c(void) { return g_sample.temp_c + pill_settings()->temp_offset; }
static float temp_f(void) { return temp_c() * 1.8f + 32.0f; }

static void sensors_json(cJSON *o)
{
    /* Field names and units match stock /sensors.json so existing parsers and
     * Home Assistant templates keep working: x/y/z in mg, tilt in degrees,
     * gravity as specific gravity, temp in C. */
    cJSON_AddNumberToObject(o, "x", g_sample.ax * 1000.0f);
    cJSON_AddNumberToObject(o, "y", g_sample.ay * 1000.0f);
    cJSON_AddNumberToObject(o, "z", g_sample.az * 1000.0f);
    cJSON_AddNumberToObject(o, "tilt", tilt_deg());
    cJSON_AddNumberToObject(o, "gravity", g_sample.gravity);
    cJSON_AddNumberToObject(o, "temp", temp_c());
    cJSON_AddNumberToObject(o, "temp_f", temp_f());
    /* No battery front-end yet: stock reports adc=-112/charge=-1 when it has
     * nothing to say, so do the same rather than inventing a number. */
    cJSON_AddNumberToObject(o, "adc", -112);
    cJSON_AddNumberToObject(o, "charge", -1);
    cJSON_AddNumberToObject(o, "charger", 0);
    cJSON_AddNumberToObject(o, "reps", 0);
    cJSON_AddNumberToObject(o, "rept", 0);
}

static esp_err_t h_sensors(httpd_req_t *req)
{
    cJSON *o = cJSON_CreateObject();
    sensors_json(o);
    return send_json(req, o);
}

static esp_err_t h_static_info(httpd_req_t *req)
{
    cJSON *o = cJSON_CreateObject();
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char macs[18], ssid[24];
    snprintf(macs, sizeof(macs), "%02x-%02x-%02x-%02x-%02x-%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    snprintf(ssid, sizeof(ssid), "RAPT-Pill-%02X%02X%02X", mac[3], mac[4], mac[5]);
    cJSON_AddStringToObject(o, "version", g_version);
    cJSON_AddStringToObject(o, "mac", macs);
    cJSON_AddStringToObject(o, "softAp", ssid);
    cJSON_AddStringToObject(o, "pillType", "PillHydrometer");
    cJSON_AddStringToObject(o, "hardwareVersion", "1.0");
    cJSON_AddBoolToObject(o, "legacy", false);
    cJSON_AddBoolToObject(o, "migrated", true);
    cJSON_AddBoolToObject(o, "provisioningMode", !g_connected);
    return send_json(req, o);
}

/* Stock firmware keeps STA credentials in NVS via esp_wifi (the "ssid" and
 * "password" items in the flash dump). Using esp_wifi's own flash storage means
 * a pill provisioned by the Rapt app joins the network with no CFW step. */
static bool save_creds(const char *ssid, const char *pass)
{
    wifi_config_t st = { .sta = { .threshold = { .authmode = WIFI_AUTH_WPA2_PSK } } };
    snprintf((char *)st.sta.ssid, sizeof(st.sta.ssid), "%s", ssid);
    snprintf((char *)st.sta.password, sizeof(st.sta.password), "%s", pass);
    esp_wifi_set_storage(WIFI_STORAGE_FLASH);
    bool ok = esp_wifi_set_config(WIFI_IF_STA, &st) == ESP_OK;
    ESP_LOGI(TAG, "saved Wi-Fi credentials for \"%s\": %d", ssid, ok);
    return ok;
}

static esp_err_t h_set_wifi(httpd_req_t *req)
{
    char ssid[33] = "", pass[65] = "";
    httpd_req_get_hdr_value_str(req, "X-Custom-ssid", ssid, sizeof(ssid));
    httpd_req_get_hdr_value_str(req, "X-Custom-pass", pass, sizeof(pass));
    if (!ssid[0]) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"error\":\"X-Custom-ssid required\"}");
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "success", save_creds(ssid, pass));
    cJSON_AddStringToObject(o, "next", "POST /reboot.json");
    return send_json(req, o);
}

/* ---------- stock settings surface --------------------------------------- */

/* Stock's setters read an X-Custom-* header and answer 200 with the value they
 * ended up with: a missing header is not an error, the current value comes
 * back. Our own pages post a form body, accepted on the same terms. */
static void hdr_or_form(httpd_req_t *req, const char *hdr, const char *key,
                        char *out, size_t n)
{
    if (httpd_req_get_hdr_value_str(req, hdr, out, n) == ESP_OK && out[0]) return;
    out[0] = 0;
    char form[96] = "";
    int r = httpd_req_recv(req, form, sizeof(form) - 1);
    size_t kl = strlen(key);
    for (char *kv = r > 0 ? (form[r] = 0, strtok(form, "&")) : NULL; kv; kv = strtok(NULL, "&"))
        if (!strncmp(kv, key, kl)) snprintf(out, n, "%s", kv + kl);
}

static esp_err_t json_reply(httpd_req_t *req, const char *fmt, ...)
{
    char b[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, b);
}

static esp_err_t set_uint(httpd_req_t *req, const char *hdr, const char *key,
                          uint32_t *field, uint32_t lo, uint32_t hi,
                          const char *json_key)
{
    char v[24];
    hdr_or_form(req, hdr, key, v, sizeof v);
    if (v[0]) {
        double d = strtod(v, NULL);
        if (d >= (double)lo && d <= (double)hi) *field = (uint32_t)d;
    }
    return json_reply(req, "{\"%s\": %u}", json_key, (unsigned)*field);
}

static esp_err_t h_get_settings(httpd_req_t *req)
{
    const pill_settings_t *p = pill_settings();
    return json_reply(req,
        "{\"interval_wifi\": %u, \"interval_ble\": %u, \"telem_method\": %u,"
        " \"grav_vel_time\": %u, \"temp_offset\": %g, \"low_batt_shutdown\": %u,"
        " \"smart_mode\": %u}",
        (unsigned)p->interval_wifi, (unsigned)p->interval_ble,
        (unsigned)p->telem_method, (unsigned)p->grav_vel_time,
        (double)p->temp_offset, (unsigned)p->low_batt_shutdown,
        (unsigned)p->smart_mode);
}

#define SETTING_SETTER(fn, fld, hdr, lo, hi, jkey)                            \
    static esp_err_t fn(httpd_req_t *req)                                     \
    {                                                                         \
        pill_settings_t *p = pill_settings_edit();                            \
        pill_settings_t was = *p;                                             \
        esp_err_t e = set_uint(req, hdr, jkey "=", &p->fld, lo, hi, jkey);    \
        if (p->fld != was.fld) pill_settings_put(p);                          \
        return e;                                                            \
    }

SETTING_SETTER(h_set_interval_wifi, interval_wifi, "X-Custom-interval", 10, 86400, "interval")
SETTING_SETTER(h_set_interval_ble, interval_ble, "X-Custom-interval", 5, 86400, "interval")
SETTING_SETTER(h_set_grav_vel_time, grav_vel_time, "X-Custom-grav-vel-time", 60, 604800, "grav_vel_time")
SETTING_SETTER(h_set_smart_mode, smart_mode, "X-Custom-smart-mode", 0, 1, "smart_mode")
SETTING_SETTER(h_set_low_batt, low_batt_shutdown, "X-Custom-low-batt-shutdown", 0, 1, "low_batt_shutdown")

static esp_err_t h_set_temp_offset(httpd_req_t *req)
{
    pill_settings_t *p = pill_settings_edit();
    float was = p->temp_offset;
    char v[24];
    hdr_or_form(req, "X-Custom-temp-offset", "temp_offset=", v, sizeof v);
    if (v[0]) {
        float t = strtof(v, NULL);
        if (t >= -20.0f && t <= 20.0f) p->temp_offset = t;
    }
    if (p->temp_offset != was) pill_settings_put(p);
    return json_reply(req, "{\"temp_offset\": %g}", (double)p->temp_offset);
}

static esp_err_t h_get_dev_buttons(httpd_req_t *req)
{
    const pill_settings_t *p = pill_settings();
    return json_reply(req, "{\"dev_opts\": %s, \"beta\": %s, \"test_api\": %s}",
                      p->dev_opts ? "true" : "false", p->beta ? "true" : "false",
                      p->test_api ? "true" : "false");
}

static esp_err_t set_flag(httpd_req_t *req, uint8_t *field, const char *hdr)
{
    char v[8];
    hdr_or_form(req, hdr, "value=", v, sizeof v);
    if (v[0]) *field = (uint8_t)(strtol(v, NULL, 10) != 0);
    pill_settings_put(pill_settings_edit());
    return json_reply(req, "{\"success\": true}");
}

static esp_err_t h_set_dev_opts(httpd_req_t *req) { return set_flag(req, &pill_settings_edit()->dev_opts, "X-Custom-dev-opts"); }
static esp_err_t h_set_beta(httpd_req_t *req)     { return set_flag(req, &pill_settings_edit()->beta, "X-Custom-beta"); }
static esp_err_t h_set_test_api(httpd_req_t *req) { return set_flag(req, &pill_settings_edit()->test_api, "X-Custom-test-api"); }

/* Stock expires its AP session after a gesture-opened window; the CFW's AP is
 * permanent, so the countdown is reported as -1 = never, and the poke that
 * extends the window is a polite no-op. */
static esp_err_t h_ap_time(httpd_req_t *req) { return json_reply(req, "{\"ap-time\": %d}", -1); }
static esp_err_t h_poke_ap(httpd_req_t *req) { return json_reply(req, "{\"success\": true}"); }

/* No cloud by design. Stock uses this exact body when the network has nothing
 * to say, so an offline CFW answers the cloud endpoints the same way. */
static esp_err_t h_no_cloud(httpd_req_t *req) { return json_reply(req, "{\"success\": false}"); }

/* Stock's factory reset: calibration and credentials go, then reboot. */
static esp_err_t h_factory_reset(httpd_req_t *req)
{
    esp_err_t e = json_reply(req, "{\"success\": true}");
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    esp_wifi_disconnect();
    esp_wifi_restore();      /* the STA record esp_wifi keeps in its own flash */
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
    return e;
}

/* ---------- the rest of stock's calibration surface ---------------------- */

/* /set_calibration_state.json: the app (or the Rapt server) installs a fitted
 * polynomial. Same header, same reply, live from the next sample. */
static esp_err_t h_set_cal_state(httpd_req_t *req)
{
    char expr[192] = "";
    if (httpd_req_get_hdr_value_str(req, "X-Custom-expr", expr, sizeof expr) != ESP_OK)
        expr[0] = 0;

    gravity_model_t m = g_model;
    bool ok = gravity_parse_expr(expr, &m);
    if (ok) {
        m.angle_offset = g_cal_deg;
        g_model = m;
        snprintf(g_expr, sizeof g_expr, "%s", expr);
        nvs_handle_t h;
        if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_str(h, "cal_expr", g_expr);
            nvs_commit(h);
            nvs_close(h);
        }
        ESP_LOGI(TAG, "custom density model installed");
    }
    return json_reply(req, "{\"expr\": \"%s\", \"expr-valid\": %s}",
                      ok ? g_expr : "", ok ? "true" : "false");
}

/* Two points. The model shape is fixed, so the only freedom is the offset, and
 * a two-point calibration lands on the mean of the two offsets. The angles are
 * the tilts read at each known density; leave one out and the current reading
 * stands in for it. */
static esp_err_t h_cal_2p(httpd_req_t *req)
{
    static const char *const HDRS[4] = {
        "X-Custom-angle1", "X-Custom-dens1", "X-Custom-angle2", "X-Custom-dens2" };
    float v[4] = { NAN, NAN, NAN, NAN };
    char buf[24];

    for (int i = 0; i < 4; i++)
        if (httpd_req_get_hdr_value_str(req, HDRS[i], buf, sizeof buf) == ESP_OK)
            v[i] = strtof(buf, NULL);

    for (int i = 0; i < 2; i++) {          /* density is mandatory */
        float d = v[2 * i + 1];
        if (!(d >= 800.0f && d <= 1200.0f))
            return json_reply(req, "{\"status\": \"%s\"}", "error");
    }
    float measured[2] = { tilt_deg(), tilt_deg() };
    for (int i = 0; i < 2; i++)
        if (v[2 * i] >= -180.0f && v[2 * i] <= 180.0f) measured[i] = v[2 * i];

    float off = 0.0f;
    for (int i = 0; i < 2; i++)
        off += gravity_angle_for(v[2 * i + 1], &g_model) - measured[i];
    off *= 0.5f;

    cal_apply(off);
    ESP_LOGI(TAG, "two-point calibration: offset %+.4f deg", (double)off);
    return json_reply(req, "{\"status\": \"%s\"}", "ok");
}

/* Back to the factory model: offset gone, installed expression gone. */
static esp_err_t h_clear_cal(httpd_req_t *req)
{
    g_model = GRAVITY_MODEL_DEFAULT;
    g_model.angle_offset = 0.0f;
    g_cal_deg = 0.0f;
    g_expr[0] = 0;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, "cal_offset");
        nvs_erase_key(h, "cal_expr");
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "calibration cleared");
    return json_reply(req, "{\"success\": true}");
}

static esp_err_t h_grav_vel(httpd_req_t *req)
{
    pill_gv_t g = pill_gv_get();
    return json_reply(req,
        "{\"valid\": %d, \"latest\": %g, \"latest_time\": %u, \"now_time\": %u,"
        " \"oldest_time\": %u, \"num\": %u}",
        g.valid, (double)g.latest, (unsigned)g.latest_time, (unsigned)g.now_time,
        (unsigned)g.oldest_time, g.num);
}

static esp_err_t h_grav_vel_csv(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/csv");
    httpd_resp_sendstr_chunk(req, "time,gravity\n");
    char row[48];
    for (unsigned i = 0; i < g_gv.n; i++) {
        const pill_gv_pt_t *p = pill_gv_at(&g_gv, i);
        snprintf(row, sizeof row, "%u,%.5f\n", (unsigned)p->t, p->pts * 0.001);
        httpd_resp_sendstr_chunk(req, row);
    }
    return httpd_resp_sendstr_chunk(req, NULL);
}

/* ---------- the log stock serves at /get_log.json ------------------------ */

/* ESP_LOG lines are copied into a tail ring on their way to the UART, so the
 * LAN API can serve them. /get_log.json answers with a JSON string, like
 * stock. ponytail: unsynchronised tail - a torn line beats a lock in the
 * logging path, and the newest half is all anyone reads. */
#define LOG_RING 8192
static char g_log[LOG_RING];
static size_t g_log_len;
static int (*g_prev_vprintf)(const char *, va_list);

static int log_vprintf(const char *fmt, va_list args)
{
    char line[256];
    va_list copy;
    va_copy(copy, args);          /* the previous hook needs its own va_list */
    int n = vsnprintf(line, sizeof line, fmt, copy);
    va_end(copy);
    if (n > 0) {
        size_t len = n < (int)sizeof line - 1 ? (size_t)n : sizeof line - 1;
        if (g_log_len + len + 1 >= LOG_RING) {
            memmove(g_log, g_log + (g_log_len - LOG_RING / 2), LOG_RING / 2);
            g_log_len = LOG_RING / 2;
        }
        memcpy(g_log + g_log_len, line, len);
        g_log_len += len;
        g_log[g_log_len] = '\0';  /* terminate at the new end, not the old one */
    }
    return g_prev_vprintf ? g_prev_vprintf(fmt, args) : n;
}

static esp_err_t h_get_log(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    cJSON *s = cJSON_CreateString(g_log);
    char *out = cJSON_PrintUnformatted(s);
    esp_err_t e = out ? httpd_resp_sendstr(req, out) : httpd_resp_send_500(req);
    free(out);
    cJSON_Delete(s);
    return e;
}

/* The CFW has no gesture engine - the sessions are always open, so there is
 * nothing to tap - which makes the log empty rather than absent. */
static esp_err_t h_gesture_log(httpd_req_t *req) { return json_reply(req, "[]"); }

/* No cloud means nothing to register with: the pill is a LAN device and says
 * so, and clearing a registration that never happened succeeds. */
static esp_err_t h_reg_status(httpd_req_t *req)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    return json_reply(req, "{\"state\": \"%s\", \"mac-address\": \"%02x-%02x-%02x-%02x-%02x-%02x\"}",
                      "local", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static esp_err_t h_clear_reg(httpd_req_t *req) { return json_reply(req, "{\"successful\": true}"); }

/* MQTT: form fields for the page, X-Custom headers for the headless crowd.
 * The broker string is the only one that gets a character filter - it becomes
 * a URL the MQTT client parses. */
static const char *form_val(const char *form, const char *key, char *out, size_t n)
{
    size_t kl = strlen(key);
    for (const char *kv = form; kv && *kv; ) {
        if (!strncmp(kv, key, kl)) {
            snprintf(out, n, "%s", kv + kl);
            return out;
        }
        kv = strchr(kv, '&');
        if (kv) kv++;
    }
    return NULL;
}

static esp_err_t h_set_mqtt(httpd_req_t *req)
{
    pill_settings_t *p = pill_settings_edit();
    char form[320] = "";
    int r = httpd_req_recv(req, form, sizeof form - 1);
    if (r > 0) form[r] = 0;

    struct { const char *hdr, *key; char *dst; size_t n; } f[] = {
        { "X-Custom-broker", "broker=", p->broker, sizeof p->broker },
        { "X-Custom-mqtt-prefix", "prefix=", p->prefix, sizeof p->prefix },
        { "X-Custom-mqtt-user", "user=", p->mqtt_user, sizeof p->mqtt_user },
        { "X-Custom-mqtt-pass", "pass=", p->mqtt_pass, sizeof p->mqtt_pass },
    };
    char v[96];
    for (unsigned i = 0; i < 4; i++) {
        if (httpd_req_get_hdr_value_str(req, f[i].hdr, v, sizeof v) == ESP_OK)
            snprintf(f[i].dst, f[i].n, "%s", v);
        else if (form_val(form, f[i].key, v, sizeof v)) {
            url_decode(v);
            snprintf(f[i].dst, f[i].n, "%s", v);
        }
    }
    for (char *q = p->broker; *q; q++)
        if (!isalnum((unsigned char)*q) && !strchr("._:-/", *q)) { *q = 0; break; }

    if (form_val(form, "port=", v, sizeof v) ||
        httpd_req_get_hdr_value_str(req, "X-Custom-mqtt-port", v, sizeof v) == ESP_OK) {
        long n = strtol(v, NULL, 10);
        if (n > 0 && n < 65536) p->port = (uint16_t)n;
    }
    if (form_val(form, "enable=", v, sizeof v) ||
        httpd_req_get_hdr_value_str(req, "X-Custom-mqtt-enable", v, sizeof v) == ESP_OK)
        p->mqtt_enable = (uint8_t)(strtol(v, NULL, 10) != 0);

    pill_settings_put(p);
    pill_mqtt_stop();
    pill_mqtt_start();
    ESP_LOGI(TAG, "mqtt %s, broker \"%s\", prefix \"%s\"",
             p->mqtt_enable ? "enabled" : "disabled", p->broker,
             p->prefix[0] ? p->prefix : "rapt");
    return json_reply(req, "{\"success\": true}");
}

/* Stock's 404 body, so a client that greps for it keeps working. */
static esp_err_t h_not_found(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    httpd_resp_set_status(req, "404 Not Found");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, "Nothing matches the given URI");
}

static esp_err_t h_reboot(httpd_req_t *req)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "success", true);
    send_json(req, o);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

static esp_err_t h_ota_url(httpd_req_t *req)
{
    char url[320] = "", sha[65] = "";
    set_hdr("X-Custom-url", url, sizeof(url), req);
    set_hdr("X-Custom-sha256", sha, sizeof(sha), req);
    cJSON *o = cJSON_CreateObject();
    if (!url[0]) {
        httpd_resp_set_status(req, "400 Bad Request");
        cJSON_Delete(o);
        return httpd_resp_sendstr(req, "{\"error\":\"X-Custom-url required\"}");
    }
    cJSON_AddBoolToObject(o, "success", pill_ota_start(url, sha));
    cJSON_AddStringToObject(o, "status", pill_ota_status());
    return send_json(req, o);
}

/* One writer for the calibration: live model, display copy, NVS. */
static void cal_apply(float deg)
{
    g_cal_deg = g_model.angle_offset = deg;
    nvs_handle_t h;
    bool ok = nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK;
    if (ok) {
        ok = nvs_set_i32(h, KEY_ANGLE_OFFSET, (int32_t)lroundf(deg * 1000.0f)) == ESP_OK;
        if (ok) ok = nvs_commit(h) == ESP_OK;
        nvs_close(h);
    }
    ESP_LOGI(TAG, "calibration offset %+.4f deg (nvs %s)", deg, ok ? "saved" : "FAILED");
}

const gravity_model_t *pill_net_model(void) { return &g_model; }

const char *pill_net_version(void) { return g_version; }
const char *pill_net_ip(void) { return g_connected ? g_ip : "172.217.28.1"; }
float pill_net_tilt(void) { return tilt_deg(); }
float pill_net_temp_c(void) { return temp_c(); }
float pill_net_temp_f(void) { return temp_f(); }

static esp_err_t h_set_cal(httpd_req_t *req)
{
    char v[24] = "";
    set_hdr("X-Custom-angle", v, sizeof(v), req);
    if (!v[0]) {                     /* the settings page posts a form body */
        char form[64] = "";
        int n = httpd_req_recv(req, form, sizeof(form) - 1);
        for (char *kv = n > 0 ? (form[n] = 0, strtok(form, "&")) : NULL; kv;
             kv = strtok(NULL, "&"))
            if (!strncmp(kv, "angle=", 6)) snprintf(v, sizeof(v), "%s", kv + 6);
    }
    float deg = strtof(v, NULL);
    httpd_resp_set_type(req, "application/json");
    if (!v[0] || deg < -30.0f || deg > 30.0f) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req,
            "{\"error\":\"X-Custom-angle must be a tilt offset in degrees, -30..30\"}");
    }
    cal_apply(deg);
    char body[96];
    snprintf(body, sizeof(body), "{\"success\":true,\"angleDeg\":%.4f}", deg);
    return httpd_resp_sendstr(req, body);
}

/* Stock's one-point calibration (POST /cal_1p.json, X-Custom-dens): tell the
 * pill what density it is sitting in and it moves the angle so that density
 * reads true. Density is in stock points — specific gravity x 1000, water =
 * 1000 — and a value below 100 is taken as specific gravity. X-Custom-angle
 * overrides the measured angle, which is how you calibrate from a bench jig. */
static esp_err_t h_cal_1p(httpd_req_t *req)
{
    char dens[24] = "", ang[24] = "";
    set_hdr("X-Custom-dens", dens, sizeof(dens), req);
    set_hdr("X-Custom-angle", ang, sizeof(ang), req);
    if (!dens[0]) {                     /* the settings page posts a form body */
        char form[96] = "";
        int n = httpd_req_recv(req, form, sizeof(form) - 1);
        for (char *kv = n > 0 ? (form[n] = 0, strtok(form, "&")) : NULL; kv;
             kv = strtok(NULL, "&")) {
            if (!strncmp(kv, "dens=", 5)) snprintf(dens, sizeof(dens), "%s", kv + 5);
            if (!strncmp(kv, "angle=", 6)) snprintf(ang, sizeof(ang), "%s", kv + 6);
        }
    }
    float points = strtof(dens, NULL);
    if (points > 0.0f && points < 100.0f) points *= 1000.0f;
    float angle = ang[0] ? strtof(ang, NULL)
                         : pill_angle_deg(g_sample.ax, g_sample.ay);
    httpd_resp_set_type(req, "application/json");
    if (!dens[0] || points < 800.0f || points > 1200.0f) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req,
            "{\"error\":\"density must be 800..1200 points (1000 = water)\"}");
    }
    gravity_model_t m = GRAVITY_MODEL_DEFAULT;   /* factory quintic, no offset */
    float ideal = gravity_angle_for(points, &m);
    float deg = ideal - angle;
    cal_apply(deg);
    ESP_LOGI(TAG, "calibrated at %.4f deg for %.2f points", angle, points);
    char body[160];
    snprintf(body, sizeof(body),
             "{\"success\":true,\"measuredDeg\":%.4f,\"cal_a1\":%.4f,\"cal_d1\":%.2f,"
             "\"angleOffset\":%.4f}",
             angle, ideal, points, deg);
    return httpd_resp_sendstr(req, body);
}

/* Stock's shape, so stock tooling parses it: {state, expr, expr-valid}. */
static esp_err_t h_cal_state(httpd_req_t *req)
{
    const char *expr = g_expr[0] ? g_expr : GRAVITY_MODEL_EXPR;
    char out[512];
    snprintf(out, sizeof(out),
             "{\"state\":\"%s\",\"expr\":\"%s\",\"expr-valid\":true,"
             "\"angle-offset\":%.4f}",
             g_cal_deg != 0.0f ? "Calibrated" : "Standard", expr, g_cal_deg);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, out);
}

static esp_err_t h_ota_status(httpd_req_t *req)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "status", pill_ota_status());
    cJSON_AddStringToObject(o, "version", g_version);
    const esp_partition_t *run = esp_ota_get_running_partition();
    if (run) cJSON_AddStringToObject(o, "partition", run->label);
    return send_json(req, o);
}

/* ------------------------------------------------------------------ web UI
 *
 * FermentDial's shape: a hero readout, a card grid, /api/status for machines,
 * /metrics for Prometheus, /settings for config, /firmware for the browser
 * upload. Stock JSON endpoints stay where they are for existing integrations. */

static const char *state_of(void)
{
    if (g_sample.gravity < 1.005f) return "fault";
    if (g_sample.gravity_velocity_valid && g_sample.gravity_velocity < -0.3f) return "active";
    if (g_sample.gravity < 1.010f) return "dry";
    return "idle";
}

static const char *state_text(void)
{
    const char *s = state_of();
    if (!strcmp(s, "active")) return "Fermenting";
    if (!strcmp(s, "dry")) return "Dry";
    if (!strcmp(s, "fault")) return "No sensor";
    return "Stable";
}

static void uptime(char *out, size_t n)
{
    int64_t s = esp_timer_get_time() / 1000000;
    snprintf(out, n, "%lldd %02lld:%02lld", (long long)(s / 86400),
             (long long)(s / 3600) % 24, (long long)(s / 60) % 60);
}

#define CARD(label) \
    "<div class=card><div class=label>" label "</div><div class=value>%s</div></div>"

static void reconnect_cb(void *arg) { (void)arg; esp_wifi_connect(); }

static esp_err_t h_index(httpd_req_t *req)
{
    char up[24], name[24], batt[16], gvel[20], xyz[24], tilt[16], temp[24];
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(name, sizeof(name), "Pill %02X%02X%02X", mac[3], mac[4], mac[5]);
    uptime(up, sizeof(up));
    snprintf(temp, sizeof(temp), "%.2f °C / %.1f °F", temp_c(), temp_f());
    snprintf(tilt, sizeof(tilt), "%.2f°", tilt_deg());
    snprintf(gvel, sizeof(gvel), g_sample.gravity_velocity_valid ? "%.2f/day" : "—",
             (double)g_sample.gravity_velocity);
    snprintf(xyz, sizeof(xyz), "%.0f %.0f %.0f", g_sample.ax * 1000.0f,
             g_sample.ay * 1000.0f, g_sample.az * 1000.0f);
    snprintf(batt, sizeof(batt), g_sample.battery_v > 0 ? "%.2f V" : "—",
             g_sample.battery_v);

    /* static, not stack: the httpd connection task runs on 4 KB, and two
     * concurrent dashboards would only be a cosmetic race. */
    static char html[2048];
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req, PILL_PAGE_HEAD("RAPT Pill", PILL_REFRESH_15));
    snprintf(html, sizeof(html),
        "<body data-state=%s><header><h1><a href=/>%s</a></h1>"
        "<div class=statusbar><span class=pill>v%s</span><span class=pill>%s</span>"
        "<span class=pill>%s</span></div></header>"
        "<section class=hero><div class=heroTop><span>Specific gravity</span>"
        "<span>%s</span></div><div class=readout><div>"
        "<div class=num>%.3f</div><div class=state>%s</div>"
        "<div class=sub>tilt %s · %s · up %s</div></div>"
        "<div class=unit>SG</div></div></section>"
        "<div class=grid>"
        CARD("Temperature")
        CARD("Tilt")
        CARD("Gravity velocity")
        CARD("X · Y · Z (mg)")
        CARD("Battery")
        CARD("Uptime")
        "</div>"
        "<div class=panel><div class=panelHead><h2>Update</h2><span>no cables</span></div>"
        "<p class=hint>Pick a <code>pill_fw.bin</code> and the pill flashes the other "
        "slot, boots it once, and reverts if it does not come up.</p>"
        "<div class=btns><a href=/firmware>Upload firmware</a>"
        "<a href=/settings>Settings</a>"
        "<a href=/api/status>JSON</a>"
        "<a href=/metrics>metrics</a>"
        "<a href=/sensors.json>stock JSON</a></div></div></body></html>",
        state_of(), name, g_version, g_connected ? g_ip : "AP only", state_text(),
        state_text(), g_sample.gravity, state_text(), tilt, temp, up,
        temp, tilt, gvel, xyz, batt, up);
    httpd_resp_sendstr_chunk(req, html);
    return httpd_resp_sendstr_chunk(req, NULL);
}

static esp_err_t h_settings(httpd_req_t *req)
{
    static char html[3200];   /* the httpd task has 8 KB; see stack_size below */
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req, PILL_PAGE_HEAD("Settings", ""));
    snprintf(html, sizeof(html),
        "<body><header><h1><a href=/>Settings</a></h1></header>"
        "<div class=panel><div class=panelHead><h2>Wi-Fi</h2><span>station</span></div>"
        "<form method=post action=/set_wifi>"
        "<div class=field><input name=ssid placeholder='Network name' required></div>"
        "<div class=field><input name=pass type=password placeholder='Passphrase'></div>"
        "<div class=btns><button>Save and reboot</button></div></form>"
        "<form method=post action=/reboot.json><button class=secondary>Reboot</button></form>"
        "</div></div>"
        "<div class=panel><div class=panelHead><h2>Calibration</h2>"
        "<span>offset %+.4f&deg;</span></div>"
        "<p class=hint>Stock's one-point calibration: set the pill in a known "
        "liquid and it moves the angle so that density reads true. The offset is "
        "the ideal angle for that density — 36.4475&deg; for water — minus where "
        "the pill sits. Quintic and factory constant are stock's.</p>"
        "<form method=post action=/cal_1p.json>"
        "<div class=field><input name=dens placeholder='Density, 1000 = water' required></div>"
        "<div class=btns><button>Calibrate here</button></div></form>"
        "<form method=post action=/set_cal.json>"
        "<div class=field><input name=angle placeholder='Angle offset, degrees'></div>"
        "<div class=btns><button>Set offset</button></div></form></div>"
        "<div class=panel><div class=panelHead><h2>MQTT</h2><span>%s</span></div>"
        "<p class=hint>Home Assistant discovery: six retained entities under "
        "<code>%s/</code>, and the LWT marks the pill offline when it drops. Off "
        "until a broker is set.</p>"
        "<form method=post action=/set_mqtt>"
        "<div class=field><input name=broker placeholder='Broker, e.g. 192.168.1.5' value='%s'></div>"
        "<div class=field><input name=port placeholder='1883' value='%u'></div>"
        "<div class=field><input name=prefix placeholder='rapt' value='%s'></div>"
        "<div class=field><input name=user placeholder='Username' value='%s'></div>"
        "<div class=field><input name=pass type=password placeholder='Password'></div>"
        "<div class=btns><button name=enable value=1>Save and connect</button>"
        "<button class=secondary name=enable value=0>Disconnect</button></div></form></div>"
        "<p class=hint>Headless: <code>curl -X POST --header 'X-Custom-dens: 1000' "
        "http://%s/cal_1p.json</code> · <code>curl -X POST --header 'X-Custom-ssid: NAME' "
        "--header 'X-Custom-pass: PASS' http://%s/set_wifi.json</code></p></body></html>",
        g_cal_deg,
        pill_settings()->mqtt_enable && pill_settings()->broker[0] ? "connected" : "off",
        pill_settings()->prefix[0] ? pill_settings()->prefix : "rapt",
        pill_settings()->broker, (unsigned)pill_settings()->port,
        pill_settings()->prefix, pill_settings()->mqtt_user,
        g_connected ? g_ip : "172.217.28.1",
        g_connected ? g_ip : "172.217.28.1");
    httpd_resp_sendstr_chunk(req, html);
    return httpd_resp_sendstr_chunk(req, NULL);
}

static esp_err_t h_firmware_page(httpd_req_t *req)
{
    static const char head[] = PILL_PAGE_HEAD("Firmware", "");
    static const char html[] =
        "<body><header><h1><a href=/>Firmware</a></h1></header>"
        "<div class=panel><div class=panelHead><h2>Upload</h2>"
        "<span>other OTA slot</span></div>"
        "<form method=post action=/firmware enctype=multipart/form-data>"
        "<div class=field><input type=file name=firmware accept=.bin required></div>"
        "<div class=btns><button>Upload and reboot</button></div></form>"
        "<p class=hint>The running slot is never touched, so a bad upload cannot "
        "brick the pill. Headless: <code>curl -F firmware=@build/pill_fw.bin "
        "http://pill.local/firmware</code> or "
        "<code>curl -X POST --header 'X-Custom-url: http://host/pill_fw.bin' "
        "http://pill.local/ota_url.json</code>.</p></body></html>";
        httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req, head);
    httpd_resp_sendstr_chunk(req, html);
    return httpd_resp_sendstr_chunk(req, NULL);
}

static void status_json(cJSON *o)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char macs[18];
    snprintf(macs, sizeof(macs), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    char nm[24];
    snprintf(nm, sizeof(nm), "Pill %02X%02X%02X", mac[3], mac[4], mac[5]);
    cJSON_AddStringToObject(o, "name", nm);
    cJSON_AddStringToObject(o, "address", macs);
    cJSON_AddStringToObject(o, "version", g_version);
    cJSON_AddBoolToObject(o, "connected", g_connected);
    cJSON_AddStringToObject(o, "ip", g_ip);
    cJSON_AddNumberToObject(o, "gravity", g_sample.gravity);
    cJSON_AddNumberToObject(o, "temperatureC", temp_c());
    cJSON_AddNumberToObject(o, "temperatureF", temp_f());
    cJSON_AddNumberToObject(o, "tilt", tilt_deg());
    cJSON_AddBoolToObject(o, "gravityVelocityValid", g_sample.gravity_velocity_valid);
    cJSON_AddNumberToObject(o, "gravityVelocity", g_sample.gravity_velocity);
    cJSON_AddNumberToObject(o, "batteryV", g_sample.battery_v);
    cJSON_AddNumberToObject(o, "x", g_sample.ax);
    cJSON_AddNumberToObject(o, "y", g_sample.ay);
    cJSON_AddNumberToObject(o, "z", g_sample.az);
    cJSON_AddNumberToObject(o, "calibrationOffsetDeg", g_cal_deg);
    cJSON_AddNumberToObject(o, "uptimeSeconds", (double)(esp_timer_get_time() / 1000000));
    const esp_partition_t *run = esp_ota_get_running_partition();
    if (run) cJSON_AddStringToObject(o, "partition", run->label);
    cJSON_AddStringToObject(o, "otaStatus", pill_ota_status());
}

static esp_err_t h_api_status(httpd_req_t *req)
{
    cJSON *o = cJSON_CreateObject();
    status_json(o);
    return send_json(req, o);
}

static esp_err_t h_metrics(httpd_req_t *req)
{
    char html[1024];
    int n = snprintf(html, sizeof(html),
        "# HELP pill_gravity Specific gravity\n# TYPE pill_gravity gauge\n"
        "pill_gravity %.5f\n"
        "# HELP pill_temperature_c Sensor temperature\n"
        "# TYPE pill_temperature_c gauge\npill_temperature_c %.4f\n"
        "# HELP pill_temperature_f Sensor temperature\n"
        "# TYPE pill_temperature_f gauge\npill_temperature_f %.4f\n"
        "# HELP pill_tilt_deg Tilt from the horizontal\n"
        "# TYPE pill_tilt_deg gauge\npill_tilt_deg %.4f\n"
        "# HELP pill_gravity_velocity Gravity change, points per day\n"
        "# TYPE pill_gravity_velocity gauge\npill_gravity_velocity %.4f\n"
        "# HELP pill_battery_v Battery voltage\n"
        "# TYPE pill_battery_v gauge\npill_battery_v %.3f\n"
        "# HELP pill_connected Wi-Fi station is associated\n"
        "# TYPE pill_connected gauge\npill_connected %d\n"
        "# HELP pill_uptime_seconds Seconds since boot\n"
        "# TYPE pill_uptime_seconds counter\npill_uptime_seconds %lld\n",
        g_sample.gravity, temp_c(), temp_f(), tilt_deg(),
        (double)g_sample.gravity_velocity, g_sample.battery_v, g_connected ? 1 : 0,
        (long long)(esp_timer_get_time() / 1000000));
    httpd_resp_set_type(req, "text/plain; version=0.0.4");
    return httpd_resp_send(req, html, n);
}

/* form-urlencoded values arrive with %XX and '+' escapes: an SSID with spaces
 * arrives as My+Kitchen, so '+' has to decode back to a space. */
static void url_decode(char *s)
{
    char *o = s;
    for (; *s; s++) {
        if (*s == '+') {
            *o++ = ' ';
        } else if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char b[3] = { s[1], s[2], 0 };
            *o++ = (char)strtol(b, NULL, 16);
            s += 2;
        } else {
            *o++ = *s;
        }
    }
    *o = 0;
}

static esp_err_t h_set_wifi_form(httpd_req_t *req)
{
    char body[256] = "", ssid[33] = "", pass[65] = "";
    int n = httpd_req_recv(req, body, sizeof(body) - 1);
    if (n > 0) {
        body[n] = 0;
        for (char *kv = strtok(body, "&"); kv; kv = strtok(NULL, "&")) {
            if (!strncmp(kv, "ssid=", 5)) snprintf(ssid, sizeof(ssid), "%s", kv + 5);
            if (!strncmp(kv, "pass=", 5)) snprintf(pass, sizeof(pass), "%s", kv + 5);
        }
        url_decode(ssid);
        url_decode(pass);
    }
    /* A header wins over the body, but only when it is present: set_hdr() zeroes
     * its buffer on a miss, and a browser form sends no X-Custom-* at all. */
    char hdr[33] = "", hpass[65] = "";
    set_hdr("X-Custom-ssid", hdr, sizeof(hdr), req);
    set_hdr("X-Custom-pass", hpass, sizeof(hpass), req);
    if (hdr[0]) snprintf(ssid, sizeof(ssid), "%s", hdr);
    if (hpass[0]) snprintf(pass, sizeof(pass), "%s", hpass);
    httpd_resp_set_type(req, "text/html");
    if (!ssid[0]) {
        ESP_LOGW(TAG, "Wi-Fi form: no ssid in %d body bytes", n);
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "no ssid");
    }
    if (!save_creds(ssid, pass)) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "esp_wifi_set_config failed");
    }
    httpd_resp_set_type(req, "text/html");
    esp_err_t e = httpd_resp_sendstr(req,
        "<p>Saved. Rebooting&hellip;</p><meta http-equiv=refresh content='6;url=/'>");
    vTaskDelay(pdMS_TO_TICKS(1500));   /* same shape as h_reboot: answer, then go */
    esp_restart();
    return e;}

static void wifi_event(void *arg, esp_event_base_t base, int id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED)
        ESP_LOGI(TAG, "config client joined");
    /* esp_wifi never dials on its own: setting a config is not connecting. */
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START && g_has_creds)
        esp_wifi_connect();
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        g_connected = false;
        if (g_has_creds) {
            wifi_event_sta_disconnected_t *d = data;
            /* Never block the event loop: a timer does the retry. A bad
             * passphrase stops after ten tries; the AP is up to re-provision. */
            if (g_attempt++ >= 10) {
                ESP_LOGW(TAG, "giving up on the AP after %d tries; AP is up for provisioning",
                         g_attempt);
            } else {
                ESP_LOGW(TAG, "lost the AP (reason %d), try %d in %u ms",
                         d ? d->reason : 0, g_attempt, (unsigned)g_backoff_ms);
                esp_timer_start_once(g_reconnect, g_backoff_ms * 1000);
                if (g_backoff_ms < 30000) g_backoff_ms *= 2;
            }
        }
    }
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        g_attempt = 0;
        g_backoff_ms = 500;
        g_connected = true;
        esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        esp_netif_ip_info_t ip;
        esp_netif_get_ip_info(sta, &ip);
        snprintf(g_ip, sizeof(g_ip), IPSTR, IP2STR(&ip.ip));
        ESP_LOGI(TAG, "connected, http://%s/", g_ip);
    }
    if (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP) g_connected = false;
}

esp_err_t pill_net_start(const char *version)
{
    snprintf(g_version, sizeof(g_version), "%s", version);

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        int32_t milli = 0;
        if (nvs_get_i32(h, KEY_ANGLE_OFFSET, &milli) == ESP_OK)
            g_cal_deg = g_model.angle_offset = (float)milli / 1000.0f;
        nvs_close(h);
    }
    ESP_LOGI(TAG, "calibration offset %+.4f deg", g_cal_deg);
    pill_settings_load();

    /* An installed density model outranks the built-in quintic. */
    nvs_handle_t eh;
    if (nvs_open(NVS_NS, NVS_READONLY, &eh) == ESP_OK) {
        size_t n = sizeof g_expr;
        if (nvs_get_str(eh, "cal_expr", g_expr, &n) == ESP_OK) {
            gravity_model_t m = GRAVITY_MODEL_DEFAULT;
            if (gravity_parse_expr(g_expr, &m)) {
                m.angle_offset = g_cal_deg;
                g_model = m;
                ESP_LOGI(TAG, "using installed density model");
            } else {
                g_expr[0] = 0;
            }
        }
        nvs_close(eh);
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    static const esp_timer_create_args_t reconnect_args = {
        .callback = reconnect_cb,
        .name = "wifi-retry",
    };
    ESP_ERROR_CHECK(esp_timer_create(&reconnect_args, &g_reconnect));
    esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));
#if !defined(PILL_BATTERY_MODE) || !PILL_BATTERY_MODE
    /* Modem sleep costs 100-300 ms per request; a bench pill with an HTTP API
     * people poll should answer promptly. Battery mode keeps the power save. */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
#endif
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                        (esp_event_handler_t)wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                        (esp_event_handler_t)wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_LOST_IP,
                        (esp_event_handler_t)wifi_event, NULL, NULL));

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    wifi_config_t ap = { .ap = { .max_connection = 2, .authmode = WIFI_AUTH_WPA2_PSK } };
    snprintf((char *)ap.ap.ssid, sizeof(ap.ap.ssid), "RAPT-Pill-%02X%02X%02X",
             mac[3], mac[4], mac[5]);
    ap.ap.ssid_len = strlen((char *)ap.ap.ssid);
    snprintf((char *)ap.ap.password, sizeof(ap.ap.password), "%s", PILL_AP_PASS);

    /* With storage in flash, esp_wifi_start() loads and joins any STA config the
     * stock firmware left in NVS. */
    wifi_config_t st = {0};
    esp_wifi_get_config(WIFI_IF_STA, &st);

    /* The Rapt app saved the network as plain ssid/password strings, and the
     * esp_wifi record stock wrote under IDF 4.3 is not readable by 5.4 — so
     * esp_wifi_get_config comes back empty on a pill that is on a network.
     * Read stock's keys and hand them over; this is what lets a pill the app
     * provisioned join with no CFW provisioning step. The config is applied
     * after set_mode below, which is where esp_wifi accepts it. */
    bool from_app = false;
    if (!st.sta.ssid[0]) {
        char ssid[33] = "", pass[65] = "";
        size_t n = sizeof(ssid), m = sizeof(pass);   /* room for the NUL, per nvs */
        nvs_handle_t h;
        if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
            bool got = nvs_get_str(h, KEY_SSID, ssid, &n) == ESP_OK;
            if (got) got = nvs_get_str(h, KEY_PASS, pass, &m) == ESP_OK;
            nvs_close(h);
            if (got && ssid[0]) {
                strncpy((char *)st.sta.ssid, ssid, sizeof(st.sta.ssid) - 1);
                strncpy((char *)st.sta.password, pass, sizeof(st.sta.password) - 1);
                /* No idea what the network claims to be; accept anything. */
                st.sta.threshold.authmode = WIFI_AUTH_OPEN;
                from_app = true;
            }
        }
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    if (st.sta.ssid[0]) {
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &st));
        g_has_creds = true;
        ESP_LOGI(TAG, "joining \"%s\"%s", (char *)st.sta.ssid,
                 from_app ? " (saved by the Rapt app)" : "");
    } else {
        ESP_LOGW(TAG, "no saved credentials; join %s / %s and open http://172.217.28.1/",
                 (char *)ap.ap.ssid, (char *)ap.ap.password);
    }
    ESP_ERROR_CHECK(esp_wifi_start());

    httpd_handle_t srv;
    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.lru_purge_enable = true;
    hc.max_uri_handlers = 48;        /* HTTPD_DEFAULT_CONFIG gives 4; we have ~43 */
    hc.stack_size = 8192;            /* default 4096: the upload handler's success
                                      * page overflows it and crashes the server */
    /* The request header budget is CONFIG_HTTPD_MAX_REQ_HDR_LEN (512 default);
     * a browser POST needs more — see sdkconfig.defaults. */
    /* Capture the console for /get_log.json on the way to the UART. */
    g_prev_vprintf = esp_log_set_vprintf(log_vprintf);

    ESP_ERROR_CHECK(httpd_start(&srv, &hc));
    httpd_register_err_handler(srv, HTTPD_404_NOT_FOUND, h_not_found);
    const httpd_uri_t uris[] = {
        { .uri = "/",                  .method = HTTP_GET,  .handler = h_index },
        { .uri = "/sensors.json",      .method = HTTP_GET,  .handler = h_sensors },
        { .uri = "/sensors_cal.json",  .method = HTTP_GET,  .handler = h_sensors },
        { .uri = "/static_info.json",  .method = HTTP_GET,  .handler = h_static_info },
        { .uri = "/set_wifi.json",     .method = HTTP_POST, .handler = h_set_wifi },
        { .uri = "/set_wifi",          .method = HTTP_POST, .handler = h_set_wifi_form },
        { .uri = "/reboot.json",       .method = HTTP_POST, .handler = h_reboot },
        { .uri = "/ota_url.json",      .method = HTTP_POST, .handler = h_ota_url },
        { .uri = "/set_cal.json",      .method = HTTP_POST, .handler = h_set_cal },
        { .uri = "/cal_1p.json",       .method = HTTP_POST, .handler = h_cal_1p },
        { .uri = "/get_calibration_state.json", .method = HTTP_GET, .handler = h_cal_state },
        { .uri = "/set_calibration_state.json", .method = HTTP_POST, .handler = h_set_cal_state },
        { .uri = "/cal_2p.json",      .method = HTTP_POST, .handler = h_cal_2p },
        { .uri = "/clear_calibration.json", .method = HTTP_POST, .handler = h_clear_cal },
        { .uri = "/get_settings.json",  .method = HTTP_GET, .handler = h_get_settings },
        { .uri = "/set_interval_wifi.json", .method = HTTP_POST, .handler = h_set_interval_wifi },
        { .uri = "/set_interval_ble.json",  .method = HTTP_POST, .handler = h_set_interval_ble },
        { .uri = "/set_temp_offset.json",   .method = HTTP_POST, .handler = h_set_temp_offset },
        { .uri = "/set_grav_vel_time.json", .method = HTTP_POST, .handler = h_set_grav_vel_time },
        { .uri = "/set_smart_mode.json",    .method = HTTP_POST, .handler = h_set_smart_mode },
        { .uri = "/set_low_battery_shutdown.json", .method = HTTP_POST, .handler = h_set_low_batt },
        { .uri = "/get_dev_buttons.json",   .method = HTTP_GET, .handler = h_get_dev_buttons },
        { .uri = "/set_dev_opts.json",      .method = HTTP_POST, .handler = h_set_dev_opts },
        { .uri = "/set_beta.json",          .method = HTTP_POST, .handler = h_set_beta },
        { .uri = "/set_test_api.json",      .method = HTTP_POST, .handler = h_set_test_api },
        { .uri = "/grav_vel.json",      .method = HTTP_GET, .handler = h_grav_vel },
        { .uri = "/get_log.json",       .method = HTTP_GET, .handler = h_get_log },
        { .uri = "/gesture_log.json",   .method = HTTP_GET, .handler = h_gesture_log },
        { .uri = "/reg-status.json",    .method = HTTP_GET, .handler = h_reg_status },
        { .uri = "/clear-reg.json",     .method = HTTP_POST, .handler = h_clear_reg },
        { .uri = "/grav_vel_data.csv",  .method = HTTP_GET, .handler = h_grav_vel_csv },
        { .uri = "/ap_time.json",           .method = HTTP_GET, .handler = h_ap_time },
        { .uri = "/poke_ap_timeout.json",   .method = HTTP_POST, .handler = h_poke_ap },
        { .uri = "/set_mqtt",           .method = HTTP_POST, .handler = h_set_mqtt },
        { .uri = "/set_mqtt.json",      .method = HTTP_POST, .handler = h_set_mqtt },
        { .uri = "/factory_reset.json",     .method = HTTP_POST, .handler = h_factory_reset },
        { .uri = "/telem_now.json",         .method = HTTP_POST, .handler = h_no_cloud },
        { .uri = "/upload_log.json",        .method = HTTP_POST, .handler = h_no_cloud },
        { .uri = "/set_telem_method.json",  .method = HTTP_POST, .handler = h_no_cloud },
        { .uri = "/ota_now.json",           .method = HTTP_POST, .handler = h_no_cloud },
        { .uri = "/ota_status.json",   .method = HTTP_GET,  .handler = h_ota_status },
        { .uri = "/api/status",        .method = HTTP_GET,  .handler = h_api_status },
        { .uri = "/metrics",           .method = HTTP_GET,  .handler = h_metrics },
        { .uri = "/settings",          .method = HTTP_GET,  .handler = h_settings },
        { .uri = "/firmware",          .method = HTTP_GET,  .handler = h_firmware_page },
        { .uri = "/firmware",          .method = HTTP_POST, .handler = pill_upload_handler },
    };
    for (unsigned i = 0; i < sizeof(uris) / sizeof(uris[0]); i++)
        ESP_ERROR_CHECK(httpd_register_uri_handler(srv, &uris[i]));
    return ESP_OK;
}

void pill_net_update(const pill_sample_t *s) { g_sample = *s; }
bool pill_net_is_connected(void) { return g_connected; }
