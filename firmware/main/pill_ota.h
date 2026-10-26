/* LAN self-update: flash a URL into the other OTA slot and reboot into it. */
#pragma once
#include <stdbool.h>

/* Expected SHA-256, lowercase hex, 64 chars; empty string means "not checked".
 * Exported so the HTTP handler can set it before starting the transfer. */
extern char g_want_sha[65];

/* Starts the transfer in the background. Returns false if it refused. */
bool pill_ota_start(const char *url, const char *sha256_hex);

/* One-line progress string for /ota_status.json. */
const char *pill_ota_status(void);

/* Disarms bootloader rollback once the network is up. Call every boot. */
void pill_ota_confirm_boot(void);
