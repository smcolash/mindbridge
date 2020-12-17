#include <sys/param.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

#include "nvs_flash.h"
#include "esp_wifi.h"
#include "mdns.h"
#include "esp_spiffs.h"
#include "esp_http_server.h"
#include "driver/gpio.h"
#include "esp_camera.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_bt_device.h"
#include "esp_spp_api.h"

#include "LED.h"
#include "Robot.h"

// -------
// logging
// -------
static const char *TAG = "mindbridge";

LED *led;
Robot *robot;

// ----
// wifi
// ----

//#define EXAMPLE_ESP_WIFI_SSID       "mcolash"
//#define EXAMPLE_ESP_WIFI_PASS       "candyraisins"
//#define EXAMPLE_ESP_MAXIMUM_RETRY   5

// ------
// camera
// ------

#define WHITE_LED               (gpio_num_t) 4
#define GPIO_OUTPUT_PIN_SEL     (1ULL << WHITE_LED)

// ESP32Cam (AiThinker) PIN Map

#define CAM_PIN_PWDN 32
#define CAM_PIN_RESET -1 //software reset will be performed
#define CAM_PIN_XCLK 0
#define CAM_PIN_SIOD 26
#define CAM_PIN_SIOC 27

#define CAM_PIN_D7 35
#define CAM_PIN_D6 34
#define CAM_PIN_D5 39
#define CAM_PIN_D4 36
#define CAM_PIN_D3 21
#define CAM_PIN_D2 19
#define CAM_PIN_D1 18
#define CAM_PIN_D0 5
#define CAM_PIN_VSYNC 25
#define CAM_PIN_HREF 23
#define CAM_PIN_PCLK 22

// global status variables
static int streaming = 0;
static bool light = false;
static bool active = false;
static int left = 0;
static int right = 0;
static unsigned int token = 0;
static int64_t last = 0;

// ---------
// bluetooth
// ---------

static const esp_spp_mode_t esp_spp_mode = ESP_SPP_MODE_CB;
static const esp_spp_sec_t sec_mask = ESP_SPP_SEC_AUTHENTICATE;
static const esp_spp_role_t role_master = ESP_SPP_ROLE_MASTER;

static esp_bd_addr_t peer_bd_addr;
static uint8_t peer_bdname_len;
static char peer_bdname[ESP_BT_GAP_MAX_BDNAME_LEN + 1];
static const char remote_device_name[] = "Chad";
static const esp_bt_inq_mode_t inq_mode = ESP_BT_INQ_MODE_GENERAL_INQUIRY;
static const uint8_t inq_len = 30;
static const uint8_t inq_num_rsps = 0;

static bool get_name_from_eir (uint8_t *eir, char *bdname, uint8_t *bdname_len)
{
    uint8_t *rmt_bdname = NULL;
    uint8_t rmt_bdname_len = 0;

    if (!eir)
    {
        ESP_LOGI (TAG, "error");
        return false;
    }

    rmt_bdname = esp_bt_gap_resolve_eir_data (eir, ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME, &rmt_bdname_len);
    if (!rmt_bdname)
    {
        rmt_bdname = esp_bt_gap_resolve_eir_data (eir, ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME, &rmt_bdname_len);
    }
    if (!rmt_bdname)
    {
        rmt_bdname = eir;
        rmt_bdname_len = strlen ((char *) eir);
    }

    if (rmt_bdname)
    {
        if (rmt_bdname_len > ESP_BT_GAP_MAX_BDNAME_LEN)
        {
            rmt_bdname_len = ESP_BT_GAP_MAX_BDNAME_LEN;
        }

        if (bdname)
        {
            memcpy (bdname, rmt_bdname, rmt_bdname_len);
            bdname[rmt_bdname_len] = '\0';
        }
        if (bdname_len)
        {
            *bdname_len = rmt_bdname_len;
        }
        return true;
    }

    ESP_LOGI (TAG, "error");
    return false;
}

static void esp_spp_cb (esp_spp_cb_event_t event, esp_spp_cb_param_t *param)
{
    switch (event)
    {
        case ESP_SPP_INIT_EVT:
            ESP_LOGI (TAG, "ESP_SPP_INIT_EVT");
            esp_bt_dev_set_device_name (CONFIG_MINDBRIDGE_MDNS_HOSTNAME);
            esp_bt_gap_set_scan_mode (ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
            //FIXME: remove for direct connect?
            esp_bt_gap_start_discovery (inq_mode, inq_len, inq_num_rsps);
            robot->connected (false, 0);

            break;
        case ESP_SPP_DISCOVERY_COMP_EVT:
            ESP_LOGI (TAG, "ESP_SPP_DISCOVERY_COMP_EVT status=%d scn_num=%d",
                    param->disc_comp.status, param->disc_comp.scn_num);
            if (param->disc_comp.status == ESP_SPP_SUCCESS)
            {
                ESP_LOGI (TAG, "SUCCCESS");
                esp_spp_connect (sec_mask, role_master, param->disc_comp.scn[0], peer_bd_addr);
                ESP_LOGI (TAG, "channel: %d", param->disc_comp.scn[0]);
                esp_log_buffer_hex (TAG, peer_bd_addr, sizeof (peer_bd_addr));
                esp_spp_connect (sec_mask, role_master, param->disc_comp.scn[0], peer_bd_addr);
            }
            break;
        case ESP_SPP_OPEN_EVT:
            ESP_LOGI (TAG, "ESP_SPP_OPEN_EVT");
            robot->connected (true, param->srv_open.handle);
            led->on ();
            break;
        case ESP_SPP_CLOSE_EVT:
            ESP_LOGI (TAG, "ESP_SPP_CLOSE_EVT");
            robot->connected (false, 0);
            esp_bt_gap_start_discovery (inq_mode, inq_len, inq_num_rsps);
            break;
        case ESP_SPP_START_EVT:
            ESP_LOGI (TAG, "ESP_SPP_START_EVT");
            break;
        case ESP_SPP_CL_INIT_EVT:
            ESP_LOGI (TAG, "ESP_SPP_CL_INIT_EVT");
            break;
        case ESP_SPP_DATA_IND_EVT:
            if (!robot->process (param->data_ind.handle, param->data_ind.len, param->data_ind.data))
            {
                ESP_LOGI (TAG, "ESP_SPP_DATA_IND_EVT");
                esp_log_buffer_hex (TAG, param->data_ind.data, param->data_ind.len);
            }
            break;
        case ESP_SPP_CONG_EVT:
            ESP_LOGI (TAG, "ESP_SPP_CONG_EVT cong=%d", param->cong.cong);
            break;
        case ESP_SPP_WRITE_EVT:
            //ESP_LOGI (TAG, "ESP_SPP_WRITE_EVT len=%d cong=%d", param->write.len , param->write.cong);
            break;
        case ESP_SPP_SRV_OPEN_EVT:
            ESP_LOGI (TAG, "ESP_SPP_SRV_OPEN_EVT");
            break;
        default:
            break;
    }
}

static void esp_bt_gap_cb (esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    switch (event)
    {
        case ESP_BT_GAP_DISC_RES_EVT:
            ESP_LOGI (TAG, "ESP_BT_GAP_DISC_RES_EVT");
            esp_log_buffer_hex (TAG, param->disc_res.bda, ESP_BD_ADDR_LEN);
            for (int i = 0; i < param->disc_res.num_prop; i++){
                esp_log_buffer_hex (TAG, param->disc_res.prop[i].val, param->disc_res.prop[i].len);

                //ESP_LOGI (TAG, "type = %d", param->disc_res.prop[i].type);
                if (param->disc_res.prop[i].type == ESP_BT_GAP_DEV_PROP_BDNAME)
                {
                    ESP_LOGI (TAG, "++ name: %s %d",
                            (char *) param->disc_res.prop[i].val,
                            param->disc_res.prop[i].len);
                }

                //if (param->disc_res.prop[i].type == ESP_BT_GAP_DEV_PROP_EIR
                if (param->disc_res.prop[i].type == ESP_BT_GAP_DEV_PROP_BDNAME
                        && get_name_from_eir ((uint8_t*) param->disc_res.prop[i].val, peer_bdname, &peer_bdname_len))
                {
                    //ESP_LOGI (TAG, "====================");

                    esp_log_buffer_char (TAG, peer_bdname, peer_bdname_len);
                    //ESP_LOGI (TAG, "** name: %s", peer_bdname);

                    if (strlen(robot->name ().c_str ()) == peer_bdname_len
                            && strncmp(peer_bdname, robot->name ().c_str (), peer_bdname_len) == 0)
                    {

                        ESP_LOGI (TAG, "CONNECTED AND READY");

                        memcpy (peer_bd_addr, param->disc_res.bda, ESP_BD_ADDR_LEN);
                        esp_spp_start_discovery (peer_bd_addr);
                        esp_bt_gap_cancel_discovery ();
                        ESP_LOGI (TAG, "NEXT STEP");
                    }
                }
            }
            break;
        case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
            ESP_LOGI (TAG, "ESP_BT_GAP_DISC_STATE_CHANGED_EVT");
            break;
        case ESP_BT_GAP_RMT_SRVCS_EVT:
            ESP_LOGI (TAG, "ESP_BT_GAP_RMT_SRVCS_EVT");
            break;
        case ESP_BT_GAP_RMT_SRVC_REC_EVT:
            ESP_LOGI (TAG, "ESP_BT_GAP_RMT_SRVC_REC_EVT");
            break;
        case ESP_BT_GAP_AUTH_CMPL_EVT:
            {
                if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS)
                {
                    ESP_LOGI (TAG, "authentication success: %s", param->auth_cmpl.device_name);
                    esp_log_buffer_hex (TAG, param->auth_cmpl.bda, ESP_BD_ADDR_LEN);
                }
                else
                {
                    ESP_LOGE (TAG, "authentication failed, status:%d", param->auth_cmpl.stat);
                }
                break;
            }
        case ESP_BT_GAP_PIN_REQ_EVT:
            {
                ESP_LOGI (TAG, "ESP_BT_GAP_PIN_REQ_EVT min_16_digit:%d", param->pin_req.min_16_digit);
                if (param->pin_req.min_16_digit)
                {
                    ESP_LOGI (TAG, "Input pin code: 0000 0000 0000 0000");
                    esp_bt_pin_code_t pin_code = {0};
                    esp_bt_gap_pin_reply(param->pin_req.bda, true, 16, pin_code);
                }
                else
                {
                    ESP_LOGI (TAG, "Input pin code: 1234");
                    esp_bt_pin_code_t pin_code;
                    pin_code[0] = '1';
                    pin_code[1] = '2';
                    pin_code[2] = '3';
                    pin_code[3] = '4';
                    esp_bt_gap_pin_reply(param->pin_req.bda, true, 4, pin_code);
                }
                break;
            }

        case ESP_BT_GAP_CFM_REQ_EVT:
            ESP_LOGI (TAG, "ESP_BT_GAP_CFM_REQ_EVT Please compare the numeric value: %d", param->cfm_req.num_val);
            esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
            break;
        case ESP_BT_GAP_KEY_NOTIF_EVT:
            ESP_LOGI (TAG, "ESP_BT_GAP_KEY_NOTIF_EVT passkey:%d", param->key_notif.passkey);
            break;
        case ESP_BT_GAP_KEY_REQ_EVT:
            ESP_LOGI (TAG, "ESP_BT_GAP_KEY_REQ_EVT Please enter passkey!");
            break;

        default:
            ESP_LOGI (TAG, "CB default: %s %d %d", __FUNCTION__, __LINE__, event);
            break;
    }
}

// ----------
// web server
// ----------

static const char *filetype (const char *path)
{
    const char *dot = strrchr (path, '.');

    if (!dot || (dot == path))
    {
        return "";
    }

    return (dot + 1);
}

// handler for statc file content
static esp_err_t file_get_handler (httpd_req_t *req)
{
    // set response headers
    httpd_resp_set_hdr (req, "Access-Control-Allow-Origin", "*");

    const char* path = (const char*) req->user_ctx;
    const char *ext = filetype (path);
    const char *type = "text/plain";

    if (!strcmp (ext, "html"))
    {
        type = "text/html";
    }
    else if (!strcmp (ext, "ico"))
    {
        type = "image/x-icon";
    }
    else if (!strcmp (ext, "manifest"))
    {
        type = "application/manifest+json";
    }
    else if (!strcmp (ext, "png"))
    {
        type = "image/png";
    }
    else if (!strcmp (ext, "css"))
    {
        type = "text/css";
    }
    else if (!strcmp (ext, "js"))
    {
        type = "text/javascript";
    }

    httpd_resp_set_type (req, type);

    FILE *file = fopen (path, "r");
    if (file == NULL)
    {
        ESP_LOGE (TAG, "failed to open %s for reading", path);
        return (ESP_OK);
    }

    char buffer[256];
    while (true)
    {
        size_t bytes = fread (buffer, 1, sizeof (buffer), file);
        httpd_resp_send_chunk (req, buffer, bytes);

        if (bytes == 0)
        {
            break;
        }

    }

    fclose (file);

    // all done
    return (ESP_OK);
}

void produce_status (httpd_req_t *request, bool authorized = false)
{
    // allow 5 seconds of inactivity
    if ((esp_timer_get_time () - last) > (10 * 1e6))
    {
        active = false;
        token = 0;
        light = 0;
        left = 0;
        right = 0;
    }

    // set response headers
    httpd_resp_set_hdr (request, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_type (request, "application/json");

    // produce response
    int temp = 0;
    if (authorized)
    {
        temp = token;
    }

    char response[256];
    snprintf ((char *) response, sizeof (response),
            "{"
                "\"active\": %d, "
                "\"connected\": %d, "
                "\"token\": %d, "
                "\"streaming\": %d, "
                "\"light\": %d, "
                "\"left\": %d, "
                "\"right\": %d"
            "}", active, robot->connected (), temp, streaming, light, left, right);
    httpd_resp_send (request, response, strlen (response));

    return;
}

// session open URL
static esp_err_t open_get_handler (httpd_req_t *request)
{
    bool authorized = false;

    if (token == 0)
    {
        authorized = true;

        while (token == 0)
        {
            token = esp_random () % 1000;
        }
    }

    // get the parameters
    size_t length = httpd_req_get_url_query_len (request);
    if (length > 0)
    {
        char *buffer = (char *) malloc (length + 1);
        if (httpd_req_get_url_query_str (request, buffer, length + 1) == ESP_OK)
        {
            char parameter[32];
            char *temp;

            if (httpd_query_key_value (buffer, "T", parameter, sizeof (parameter)) == ESP_OK)
            {
                unsigned int requested = strtol (parameter, &temp, 10);
                if ((requested != 0) && (requested == token))
                {
                    authorized = true;
                }
            }
        }

        free (buffer);
    }

    if (authorized)
    {
        active = true;
        last = esp_timer_get_time ();
    }

    produce_status (request, authorized);

    // all done
    return (ESP_OK);
}

// status URL
static esp_err_t status_get_handler (httpd_req_t *request)
{
    produce_status (request);

    // all done
    return (ESP_OK);
}

// light control URL
static esp_err_t light_get_handler (httpd_req_t *request)
{
    // get the parameters
    size_t length = httpd_req_get_url_query_len (request);
    if (length > 0)
    {
        char *buffer = (char *) malloc (length + 1);
        if (httpd_req_get_url_query_str (request, buffer, length + 1) == ESP_OK)
        {
            char parameter[32];
            char *temp;

            if (httpd_query_key_value (buffer, "T", parameter, sizeof (parameter)) == ESP_OK)
            {
                unsigned int requested = strtol (parameter, &temp, 10);
                if ((requested != 0) && (requested == token))
                {
                    if (httpd_query_key_value (buffer, "L", parameter, sizeof (parameter)) == ESP_OK)
                    {
                        light = (((int8_t) strtol (parameter, &temp, 10)) != 0);
                        gpio_set_level (WHITE_LED, light);

                        last = esp_timer_get_time ();
                    }
                }
            }
        }

        free (buffer);
    }

    produce_status (request);

    // all done
    return (ESP_OK);
}

extern "C" int httpd_default_send (httpd_handle_t hd, int sockfd, const char *buf, size_t buf_len, int flags);

int httpd_default_send_str (httpd_handle_t hd, int sockfd, const char *buf, int flags)
{
    return (httpd_default_send (hd, sockfd, (const char *) buf, strlen (buf), flags));
}

struct video_resp_arg {
    httpd_handle_t hd;
    int fd;
};

#define BOUNDARY "ce3c8aac-21d4-4fa5-8c63-8c87fb2d0e27"

static void emit_video_frame (void *argument)
{
    struct video_resp_arg *parameters = (struct video_resp_arg *) argument;
    httpd_handle_t hd = parameters->hd;
    int fd = parameters->fd;

    camera_fb_t *fb = esp_camera_fb_get ();

    if (fb)
    {
        int bytes = 0;
        size_t length = fb->len;
        uint8_t * data = fb->buf;
        char buffer[256];

        bytes = httpd_default_send_str (hd, fd, "Content-Type: image/jpeg\r\n", 0);
        snprintf ((char *) buffer, 256, "Content-Length: %u\r\n", length);
        bytes = httpd_default_send_str (hd, fd, buffer, 0);
        bytes = httpd_default_send_str (hd, fd, "\r\n", 0);
        bytes = httpd_default_send (hd, fd, (const char *) data, length, 0);

        esp_camera_fb_return (fb);

        if (bytes > 0)
        {
            bytes = httpd_default_send_str (hd, fd, "\r\n--" BOUNDARY "\r\n", 0);
            httpd_queue_work (hd, emit_video_frame, argument);
            return;
        }
    }

    streaming = MAX (0, streaming - 1);
    httpd_default_send_str (hd, fd, "\r\n--" BOUNDARY "--\r\n", 0);

    free (argument);
}

static esp_err_t video_get_handler (httpd_req_t *request)
{
    struct video_resp_arg *argument = (struct video_resp_arg *) malloc (sizeof (struct video_resp_arg));

    argument->hd = request->handle;
    argument->fd = httpd_req_to_sockfd (request);

    if (argument->fd < 0) {
        return (ESP_FAIL);
    }

    httpd_default_send_str (argument->hd, argument->fd, "HTTP/1.1 200 OK\r\n", 0);
    httpd_default_send_str (argument->hd, argument->fd, "Content-Type: multipart/x-mixed-replace; boundary=" BOUNDARY "\r\n", 0);
    //httpd_default_send_str (argument->hd, argument->fd, "Transfer-Encoding: chunked\r\n", 0);
    httpd_default_send_str (argument->hd, argument->fd, "Access-Control-Allow-Origin: *\r\n", 0);
    httpd_default_send_str (argument->hd, argument->fd, "\r\n", 0);
    //httpd_default_send_str (argument->hd, argument->fd, "\r\n--" BOUNDARY "\r\n", 0);
    //httpd_default_send_str (argument->hd, argument->fd, "\r\n", 0);

    streaming++;
    httpd_queue_work (request->handle, emit_video_frame, argument);

    return (ESP_OK);
}

// handler for the motor control URL
static esp_err_t drive_get_handler (httpd_req_t *request)
{
    // get the parameters
    size_t length = httpd_req_get_url_query_len (request);
    if (length > 0)
    {
        char *buffer = (char *) malloc (length + 1);
        if (httpd_req_get_url_query_str (request, buffer, length + 1) == ESP_OK)
        {
            char parameter[32];
            char *temp;

            if (httpd_query_key_value (buffer, "T", parameter, sizeof (parameter)) == ESP_OK)
            {
                unsigned int requested = strtol (parameter, &temp, 10);
                if ((requested != 0) && (requested == token))
                {
                    if (httpd_query_key_value (buffer, "L", parameter, sizeof (parameter)) == ESP_OK)
                    {
                        int8_t speed = (int8_t) MIN (100, MAX (-100, strtol (parameter, &temp, 10)));
                        robot->motor (LEFT_MOTOR, speed);
                        left = speed;
                    }

                    if (httpd_query_key_value (buffer, "R", parameter, sizeof (parameter)) == ESP_OK)
                    {
                        int8_t speed = (int8_t) MIN (100, MAX (-100, strtol (parameter, &temp, 10)));
                        robot->motor (RIGHT_MOTOR, speed);
                        right = speed;
                    }
                }
            }

            free (buffer);
        }
    }

    ESP_LOGI (TAG, "left: %d, right: %d", left, right);

    produce_status (request);

    // all done
    return (ESP_OK);
}

static const httpd_uri_t icon_192_png_uri = {
    .uri        = "/icon-192.png",
    .method     = HTTP_GET,
    .handler    = file_get_handler,
    .user_ctx   = (void *) "/local/icon-192.png"
};

static const httpd_uri_t manifest_json_uri = {
    .uri        = "/manifest.json",
    .method     = HTTP_GET,
    .handler    = file_get_handler,
    .user_ctx   = (void *) "/local/manifest.json"
};

static const httpd_uri_t favicon_png_uri = {
    .uri        = "/favicon.png",
    .method     = HTTP_GET,
    .handler    = file_get_handler,
    .user_ctx   = (void *) "/local/favicon.png"
};

static const httpd_uri_t slash_uri = {
    .uri        = "/",
    .method     = HTTP_GET,
    .handler    = file_get_handler,
    .user_ctx   = (void *) "/local/index.html"
};

static const httpd_uri_t jquery_min_js_uri = {
    .uri        = "/jquery.min.js",
    .method     = HTTP_GET,
    .handler    = file_get_handler,
    .user_ctx   = (void *) "/local/jquery.min.js"
};

static const httpd_uri_t mindbridge_js_uri = {
    .uri        = "/mindbridge.js",
    .method     = HTTP_GET,
    .handler    = file_get_handler,
    .user_ctx   = (void *) "/local/mindbridge.js"
};

static const httpd_uri_t mindbridge_css_uri = {
    .uri        = "/mindbridge.css",
    .method     = HTTP_GET,
    .handler    = file_get_handler,
    .user_ctx   = (void *) "/local/mindbridge.css"
};

static const httpd_uri_t smpte_gif_uri = {
    .uri        = "/smpte.gif",
    .method     = HTTP_GET,
    .handler    = file_get_handler,
    .user_ctx   = (void *) "/local/smpte.gif"
};

static const httpd_uri_t open_uri = {
    .uri        = "/open",
    .method     = HTTP_GET,
    .handler    = open_get_handler,
    .user_ctx   = (void *) "open session"
};

static const httpd_uri_t status_uri = {
    .uri        = "/status",
    .method     = HTTP_GET,
    .handler    = status_get_handler,
    .user_ctx   = (void *) "status handler"
};

static const httpd_uri_t light_uri = {
    .uri        = "/light",
    .method     = HTTP_GET,
    .handler    = light_get_handler,
    .user_ctx   = (void *) "light handler"
};

static const httpd_uri_t video_uri = {
    .uri        = "/video",
    .method     = HTTP_GET,
    .handler    = video_get_handler,
    .user_ctx   = (void *) "async video handler"
};

static const httpd_uri_t drive_uri = {
    .uri        = "/drive",
    .method     = HTTP_GET,
    .handler    = drive_get_handler,
    .user_ctx   = (void *) "motor control handler"
};

static httpd_handle_t start_webserver (void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG ();
    config.max_uri_handlers = 16;
    config.backlog_conn = 16;

    // Start the httpd server
    ESP_LOGI (TAG, "starting httpd on port: '%d'", config.server_port);
    if (httpd_start (&server, &config) == ESP_OK)
    {
        // Set URI handlers
        ESP_LOGI (TAG, "registering URI handlers");

        // file access handlers
        httpd_register_uri_handler (server, &slash_uri);
        httpd_register_uri_handler (server, &favicon_png_uri);
        httpd_register_uri_handler (server, &icon_192_png_uri);
        httpd_register_uri_handler (server, &manifest_json_uri);
        httpd_register_uri_handler (server, &jquery_min_js_uri);
        httpd_register_uri_handler (server, &mindbridge_js_uri);
        httpd_register_uri_handler (server, &mindbridge_css_uri);
        httpd_register_uri_handler (server, &smpte_gif_uri);

        // control handlers
        httpd_register_uri_handler (server, &open_uri);
        httpd_register_uri_handler (server, &status_uri);
        httpd_register_uri_handler (server, &light_uri);
        httpd_register_uri_handler (server, &video_uri);
        httpd_register_uri_handler (server, &drive_uri);

        return (server);
    }

    ESP_LOGE (TAG, "failed to start httpd");
    return (NULL);
}

static void stop_webserver (httpd_handle_t server)
{
    // stop the httpd server
    httpd_stop (server);
}

// ----
// wifi
// ----

/* FreeRTOS event group to signal when we are connected*/
static EventGroupHandle_t s_wifi_event_group;

/* The event group allows multiple bits for each event, but we only care about two events:
 * - we are connected to the AP with an IP
 * - we failed to connect after the maximum amount of retries */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static int s_retry_num = 0;

static void event_handler(void* arg, esp_event_base_t event_base,
        int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        tcpip_adapter_set_hostname (TCPIP_ADAPTER_IF_STA, CONFIG_MINDBRIDGE_MDNS_HOSTNAME);
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        if (s_retry_num < CONFIG_ESP_MAXIMUM_RETRY)
        {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI (TAG, "retry to connect to the AP");
        }
        else
        {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI (TAG,"connect to the AP fail");
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI (TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits (s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void wifi_init_sta (void)
{
    s_wifi_event_group = xEventGroupCreate ();

    ESP_ERROR_CHECK (esp_netif_init ());

    ESP_ERROR_CHECK (esp_event_loop_create_default ());
    esp_netif_create_default_wifi_sta ();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT ();
    ESP_ERROR_CHECK (esp_wifi_init (&cfg));

    ESP_ERROR_CHECK (esp_event_handler_register (WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK (esp_event_handler_register (IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));

    wifi_config_t wifi_config;
    memset (&wifi_config, 0x00, sizeof (wifi_config));
    sprintf ((char *) wifi_config.sta.ssid, CONFIG_ESP_WIFI_SSID);
    sprintf ((char *) wifi_config.sta.password, CONFIG_ESP_WIFI_PASSWORD);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    ESP_ERROR_CHECK (esp_wifi_set_mode (WIFI_MODE_STA) );
    ESP_ERROR_CHECK (esp_wifi_set_config (ESP_IF_WIFI_STA, &wifi_config) );
    ESP_ERROR_CHECK (esp_wifi_start () );

    ESP_LOGI (TAG, "wifi_init_sta finished.");

    /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
     * number of re-tries (WIFI_FAIL_BIT). The bits are set by event_handler() (see above) */
    EventBits_t bits = xEventGroupWaitBits (s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

    /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
     * happened. */
    if (bits & WIFI_CONNECTED_BIT)
    {
        ESP_LOGI (TAG, "connected to ap SSID:%s", CONFIG_ESP_WIFI_SSID);
    }
    else if (bits & WIFI_FAIL_BIT)
    {
        ESP_LOGI (TAG, "failed to connect to SSID:%s", CONFIG_ESP_WIFI_SSID);
    }
    else
    {
        ESP_LOGE (TAG, "UNEXPECTED EVENT");
    }

    ESP_ERROR_CHECK (esp_event_handler_unregister (IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler));
    ESP_ERROR_CHECK (esp_event_handler_unregister (WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler));
    vEventGroupDelete (s_wifi_event_group);

    return;
}

static void disconnect_handler (void* arg, esp_event_base_t event_base,
        int32_t event_id, void* event_data)
{
    ESP_LOGI (TAG, "stopping network services");

    httpd_handle_t* server = (httpd_handle_t*) arg;
    if (*server)
    {
        stop_webserver (*server);
        *server = NULL;
    }
}

static void connect_handler (void* arg, esp_event_base_t event_base,
        int32_t event_id, void* event_data)
{
    ESP_LOGI (TAG, "starting network services");

    httpd_handle_t* server = (httpd_handle_t*) arg;
    if (*server == NULL)
    {
        *server = start_webserver ();
    }
}

// ------
// camera
// ------

static camera_config_t camera_config = {
    .pin_pwdn = CAM_PIN_PWDN,
    .pin_reset = CAM_PIN_RESET,
    .pin_xclk = CAM_PIN_XCLK,
    .pin_sscb_sda = CAM_PIN_SIOD,
    .pin_sscb_scl = CAM_PIN_SIOC,

    .pin_d7 = CAM_PIN_D7,
    .pin_d6 = CAM_PIN_D6,
    .pin_d5 = CAM_PIN_D5,
    .pin_d4 = CAM_PIN_D4,
    .pin_d3 = CAM_PIN_D3,
    .pin_d2 = CAM_PIN_D2,
    .pin_d1 = CAM_PIN_D1,
    .pin_d0 = CAM_PIN_D0,
    .pin_vsync = CAM_PIN_VSYNC,
    .pin_href = CAM_PIN_HREF,
    .pin_pclk = CAM_PIN_PCLK,

    //XCLK 20MHz or 10MHz for OV2640 double FPS (Experimental)
    .xclk_freq_hz = 20000000,
    .ledc_timer = LEDC_TIMER_0,
    .ledc_channel = LEDC_CHANNEL_0,

    .pixel_format = PIXFORMAT_JPEG, //YUV422,GRAYSCALE,RGB565,JPEG
    .frame_size = FRAMESIZE_VGA,    //QQVGA-UXGA Do not use sizes above QVGA when not JPEG

    .jpeg_quality = 63, // 12, //0-63 lower number means higher quality
    .fb_count = 1 // 1       //if more than one, i2s runs in continuous mode. Use only with JPEG
};

static esp_err_t init_camera ()
{
    //initialize the camera
    esp_err_t err = esp_camera_init (&camera_config);
    if (err != ESP_OK)
    {
        ESP_LOGE (TAG, "Camera Init Failed");
        return err;
    }

    sensor_t * s = esp_camera_sensor_get ();
    s->set_hmirror (s, true);
    s->set_vflip (s, true);

    return (ESP_OK);
}

extern "C" void app_main ()
{
    //
    // initialize NVS
    //
    esp_err_t ret = nvs_flash_init ();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK (nvs_flash_erase ());
        ret = nvs_flash_init ();
    }
    ESP_ERROR_CHECK (ret);

    // create the LED interface
    led = new LED ((gpio_num_t) CONFIG_MINDBRIDGE_ACTIVITY_LED);

    // create the robot interface
    robot = new Robot ("Chad", led);

    //
    // start WIFI and connect to the access point
    //
    ESP_LOGI (TAG, "ESP_WIFI_MODE_STA");
    wifi_init_sta ();

    static httpd_handle_t server = NULL;

    ESP_ERROR_CHECK (esp_event_handler_register (IP_EVENT, IP_EVENT_STA_GOT_IP, &connect_handler, &server));
    ESP_ERROR_CHECK (esp_event_handler_register (WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &disconnect_handler, &server));

    // configure mDNS
    ret = mdns_init ();
    ESP_ERROR_CHECK (ret);

    mdns_hostname_set (CONFIG_MINDBRIDGE_MDNS_HOSTNAME);
    mdns_instance_name_set ("LEGO Mindstorms control bridge");

    //add our services
    mdns_service_add (NULL, "_http", "_tcp", 80, NULL, 0);

    // ------------------------
    // configure the filesystem
    // ------------------------

    esp_vfs_spiffs_conf_t spiffs_conf =
    {
        .base_path = "/local",
        .partition_label = NULL,
        .max_files = 16,
        .format_if_mount_failed = false
    };

    ret = esp_vfs_spiffs_register (&spiffs_conf);

    if (ret != ESP_OK)
    {
        ESP_LOGE (TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name (ret));
        return;
    }

    size_t total = 0;
    size_t used = 0;

    ret = esp_spiffs_info (spiffs_conf.partition_label, &total, &used);
    if (ret != ESP_OK)
    {
        ESP_LOGE (TAG, "Failed to get SPIFFS partition information (%s)", esp_err_to_name (ret));
    }
    else
    {
        ESP_LOGI (TAG, "Partition size: total: %d, used: %d", total, used);
    }

    // ----------
    // web server
    // ----------
    server = start_webserver ();

#if 0
    //configure GPIO
    gpio_config_t io_conf;
    io_conf.intr_type = (gpio_int_type_t) GPIO_PIN_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = GPIO_OUTPUT_PIN_SEL;
    io_conf.pull_down_en = (gpio_pulldown_t) 0;
    io_conf.pull_up_en = (gpio_pullup_t) 0;

    gpio_config (&io_conf);
#endif

    ESP_ERROR_CHECK (esp_bt_controller_mem_release (ESP_BT_MODE_BLE));

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT ();
    if ((ret = esp_bt_controller_init (&bt_cfg)) != ESP_OK)
    {
        ESP_LOGE (TAG, "%s initialize controller failed: %s\n", __func__, esp_err_to_name (ret));
        return;
    }

    if ((ret = esp_bt_controller_enable (ESP_BT_MODE_CLASSIC_BT)) != ESP_OK)
    {
        ESP_LOGE (TAG, "%s enable controller failed: %s\n", __func__, esp_err_to_name (ret));
        return;
    }

    if ((ret = esp_bluedroid_init ()) != ESP_OK)
    {
        ESP_LOGE (TAG, "%s initialize bluedroid failed: %s\n", __func__, esp_err_to_name (ret));
        return;
    }

    if ((ret = esp_bluedroid_enable ()) != ESP_OK)
    {
        ESP_LOGE (TAG, "%s enable bluedroid failed: %s\n", __func__, esp_err_to_name (ret));
        return;
    }

    if ((ret = esp_bt_gap_register_callback (esp_bt_gap_cb)) != ESP_OK)
    {
        ESP_LOGE (TAG, "%s gap register failed: %s\n", __func__, esp_err_to_name (ret));
        return;
    }

    if ((ret = esp_spp_register_callback (esp_spp_cb)) != ESP_OK)
    {
        ESP_LOGE (TAG, "%s spp register failed: %s\n", __func__, esp_err_to_name (ret));
        return;
    }

    if ((ret = esp_spp_init (esp_spp_mode)) != ESP_OK)
    {
        ESP_LOGE (TAG, "%s spp init failed: %s\n", __func__, esp_err_to_name (ret));
        return;
    }

    /* Set default parameters for Secure Simple Pairing */
    esp_bt_sp_param_t param_type = ESP_BT_SP_IOCAP_MODE;
    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_IO;
    esp_bt_gap_set_security_param (param_type, &iocap, sizeof (uint8_t));

    /*
     * Set default parameters for Legacy Pairing
     * Use variable pin, input pin code when pairing
     */
    esp_bt_pin_type_t pin_type = ESP_BT_PIN_TYPE_VARIABLE;
    esp_bt_pin_code_t pin_code;
    esp_bt_gap_set_pin (pin_type, 0, pin_code);

    ESP_ERROR_CHECK (init_camera ());

    // use motor controls to keep the connection alive
    while (true)
    {
        static int idle = 0;

        if (!robot->connected ())
        {
            idle++;

            if (idle > (60.0 / 5))
            {
                ESP_LOGI (TAG, "restart discovery");
                esp_bt_gap_start_discovery (inq_mode, inq_len, inq_num_rsps);

                idle = 0;
            }
        }
        else
        {
            idle = 0;

            robot->keepalive ();
            robot->battery ();
        }

        vTaskDelay (5000 / portTICK_PERIOD_MS);
    }
}
