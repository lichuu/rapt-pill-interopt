/* Browser firmware upload: GET /firmware shows the form, POST /firmware streams
 * the .bin into the next OTA slot and reboots into it. */
#pragma once
#include "esp_http_server.h"
#include "web_theme.h"

/* Themed one-shot result page: opens a panel, the caller fills in and closes it. */
#define PILL_RESULT_HTML(title) \
    PILL_PAGE_HEAD(title, "") \
    "<body><main><div class=panel><h1>" title "</h1>"

esp_err_t pill_upload_handler(httpd_req_t *req);
