#include "pill_mqtt.h"
#include "board.h"
#include "pill_net.h"
#include "pill_settings.h"

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "mqtt_client.h"

static const char *TAG = "mqtt";

static esp_mqtt_client_handle_t c;
static char base[64];      /* <prefix>/<mac tail>, e.g. rapt/aabbcc */
static char dev_ids[24];   /* rapt-aabbcc  - the HA device id */
static char dev_name[24];  /* Pill AABBCC  - what the dashboard shows */
static char avail[96];

/* Six readings, all of them retained, so a fresh Home Assistant picks up the
 * last known values the moment it subscribes. No icons, no exp_aft: the pill
 * has no clock, and the LWT already marks it unavailable. */
typedef struct {
    const char *leaf;
    const char *name;
    const char *unit;
    const char *cls;        /* HA device_class, empty for none */
} sens_t;

static const sens_t SENS[] = {
    { "gravity",  "gravity",          "SG",       "" },
    { "temp_c",   "temperature",      "°C",      "temperature" },
    { "temp_f",   "temperature f",    "°F",      "temperature" },
    { "tilt",     "tilt",             "°",       "" },
    { "grav_vel", "gravity velocity", "pts/day",  "" },
    { "battery",  "battery",          "V",        "voltage" },
};
#define N_SENS (sizeof SENS / sizeof SENS[0])

static void ident(void)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    const char *p = pill_settings()->prefix;
    if (!p[0]) p = "rapt";
    snprintf(base, sizeof base, "%s/%02x%02x%02x", p, mac[3], mac[4], mac[5]);
    snprintf(dev_ids, sizeof dev_ids, "rapt-%02x%02x%02x", mac[3], mac[4], mac[5]);
    snprintf(dev_name, sizeof dev_name, "Pill %02X%02X%02X", mac[3], mac[4], mac[5]);
    snprintf(avail, sizeof avail, "%s/availability", base);
}

static void publish_config(const char *avail_topic)
{
    char dev[200];
    snprintf(dev, sizeof dev,
             "\"dev\":{\"ids\":[\"%s\"],\"name\":\"%s\",\"mf\":\"RAPT (CFW)\","
             "\"mdl\":\"PillHydrometer\",\"sw\":\"%s\",\"cu\":\"http://%s/\"}",
             dev_ids, dev_name, pill_net_version(), pill_net_ip());

    for (unsigned i = 0; i < N_SENS; i++) {
        char dc[48] = "";
        if (SENS[i].cls[0]) snprintf(dc, sizeof dc, ",\"dev_cla\":\"%s\"", SENS[i].cls);
        char top[128], stat[96], cfg[768];
        snprintf(top, sizeof top, "homeassistant/sensor/%s_%s/config",
                 dev_ids, SENS[i].leaf);
        snprintf(stat, sizeof stat, "%s/%s", base, SENS[i].leaf);
        snprintf(cfg, sizeof cfg,
                 "{\"obj_id\":\"%s_%s\",\"uniq_id\":\"%s_%s\",\"name\":\"%s %s\","
                 "\"stat_t\":\"%s\",\"avty_t\":\"%s\",\"pl_avail\":\"online\","
                 "\"pl_offl\":\"offline\",\"unit_of_meas\":\"%s\"%s,%s}",
                 dev_ids, SENS[i].leaf, dev_ids, SENS[i].leaf,
                 dev_name, SENS[i].name, stat, avail_topic,
                 SENS[i].unit, dc, dev);
        esp_mqtt_client_publish(c, top, cfg, 0, 1, 1);
    }
}

static void publish_one(int i, const pill_sample_t *s)
{
    static const char *FMT[6] = { "%.3f", "%.2f", "%.2f", "%.2f", "%.2f", "%.2f" };
    double v;
    switch (i) {
        case 0: v = s->gravity; break;
        case 1: v = pill_net_temp_c(); break;
        case 2: v = pill_net_temp_f(); break;
        case 3: v = pill_net_tilt(); break;
        case 4: v = s->gravity_velocity; break;
        default: v = s->battery_v; break;
    }
    char topic[96], val[32];
    snprintf(topic, sizeof topic, "%s/%s", base, SENS[i].leaf);
    snprintf(val, sizeof val, FMT[i], v);
    esp_mqtt_client_publish(c, topic, val, 0, 1, 1);
}

static void handler(void *arg, esp_event_base_t base_, int32_t id, void *data)
{
    (void)arg; (void)base_; (void)id;
    if (((esp_mqtt_event_t *)data)->event_id == MQTT_EVENT_CONNECTED) {
        ident();
        esp_mqtt_client_publish(c, avail, "online", 0, 1, 1);
        publish_config(avail);
        ESP_LOGI(TAG, "connected; %u entities announced under %s/",
                 (unsigned)N_SENS, base);
    }
}

void pill_mqtt_start(void)
{
    const pill_settings_t *p = pill_settings();
    if (!p->mqtt_enable || !p->broker[0]) return;
    ident();

    char uri[112];
    if (strstr(p->broker, "://")) snprintf(uri, sizeof uri, "%s", p->broker);
    else snprintf(uri, sizeof uri, "mqtt://%s:%u", p->broker, (unsigned)p->port);

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = uri,
        .credentials.username = p->mqtt_user[0] ? p->mqtt_user : NULL,
        .credentials.authentication.password = p->mqtt_pass[0] ? p->mqtt_pass : NULL,
        .session.keepalive = 60,
        .session.last_will.topic = avail,
        .session.last_will.msg = "offline",
        .session.last_will.qos = 1,
        .session.last_will.retain = 1,
    };
    c = esp_mqtt_client_init(&cfg);
    if (!c) {
        ESP_LOGE(TAG, "init failed for %s", uri);
        return;
    }
    esp_mqtt_client_register_event(c, ESP_EVENT_ANY_ID, handler, NULL);
    esp_mqtt_client_start(c);
    ESP_LOGI(TAG, "dialling %s, topics under %s/", uri, base);
}

void pill_mqtt_stop(void)
{
    if (!c) return;
    esp_mqtt_client_publish(c, avail, "offline", 0, 1, 1);
    esp_mqtt_client_stop(c);
    esp_mqtt_client_destroy(c);
    c = NULL;
}

void pill_mqtt_publish(const pill_sample_t *s)
{
    if (!c) return;
    for (unsigned i = 0; i < N_SENS; i++) publish_one(i, s);
}
