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

// -------
// logging
// -------
static const char *TAG = "mindbridge";

// ----
// wifi
// ----

#define EXAMPLE_ESP_WIFI_SSID       "mcolash"
#define EXAMPLE_ESP_WIFI_PASS       "candyraisins"
#define EXAMPLE_ESP_MAXIMUM_RETRY   5

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
static bool connected = false;
static int left = 0;
static int right = 0;
static unsigned int token = 0;
static int64_t last = 0;

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
    if ((esp_timer_get_time () - last) > (5 * 1e6))
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
            "}", active, connected, temp, streaming, light, left, right);
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
                        //robot->motor (LEFT_MOTOR, speed);
                        left = speed;
                    }

                    if (httpd_query_key_value (buffer, "R", parameter, sizeof (parameter)) == ESP_OK)
                    {
                        int8_t speed = (int8_t) MIN (100, MAX (-100, strtol (parameter, &temp, 10)));
                        //robot->motor (RIGHT_MOTOR, speed);
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
        if (s_retry_num < EXAMPLE_ESP_MAXIMUM_RETRY)
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
    ESP_LOGI (TAG, "%s %d", __FUNCTION__, __LINE__);
    s_wifi_event_group = xEventGroupCreate ();

    ESP_ERROR_CHECK (esp_netif_init ());

    ESP_ERROR_CHECK (esp_event_loop_create_default ());
    esp_netif_create_default_wifi_sta ();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT ();
    ESP_ERROR_CHECK (esp_wifi_init (&cfg));

    ESP_LOGI (TAG, "%s %d", __FUNCTION__, __LINE__);
    ESP_ERROR_CHECK (esp_event_handler_register (WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK (esp_event_handler_register (IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));

    ESP_LOGI (TAG, "%s %d", __FUNCTION__, __LINE__);
    wifi_config_t wifi_config;
    memset (&wifi_config, 0x00, sizeof (wifi_config));
    sprintf ((char *) wifi_config.sta.ssid, EXAMPLE_ESP_WIFI_SSID);
    sprintf ((char *) wifi_config.sta.password, EXAMPLE_ESP_WIFI_PASS);
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
        ESP_LOGI (TAG, "connected to ap SSID:%s", EXAMPLE_ESP_WIFI_SSID);
    }
    else if (bits & WIFI_FAIL_BIT)
    {
        ESP_LOGI (TAG, "failed to connect to SSID:%s", EXAMPLE_ESP_WIFI_SSID);
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
    ESP_LOGI (TAG, "%s %d", __FUNCTION__, __LINE__);
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
    ESP_LOGI (TAG, "%s %d", __FUNCTION__, __LINE__);
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
    .fb_count = 2 // 1       //if more than one, i2s runs in continuous mode. Use only with JPEG
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

    //configure GPIO
    gpio_config_t io_conf;
    io_conf.intr_type = (gpio_int_type_t) GPIO_PIN_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = GPIO_OUTPUT_PIN_SEL;
    io_conf.pull_down_en = (gpio_pulldown_t) 0;
    io_conf.pull_up_en = (gpio_pullup_t) 0;

    gpio_config (&io_conf);

    ESP_ERROR_CHECK (init_camera ());
    //camera = new Camera ();

    while (1)
    {
        vTaskDelay(5000 / portTICK_RATE_MS);
    }
}
