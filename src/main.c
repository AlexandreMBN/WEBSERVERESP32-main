#include <stdio.h>
#include <string.h>
#include <esp_log.h>
#include <esp_err.h>
#include <esp_spiffs.h>
#include <esp_wifi.h>
#include <esp_event.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <esp_netif.h>
#include <esp_http_server.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define MAX_APs 20

static const char *TAG = "ESP32_AP_CONFIG";

char config_ssid[32] = "";
char config_pass[64] = "";
char config_ip[16] = "";
char config_gateway[16] = "";
char config_mask[16] = "";
char config_dns[16] = "";
char config_user[32] = "";
char config_userpass[32] = "";

wifi_ap_record_t ap_records[MAX_APs];
uint16_t ap_count = 0;
static bool logged_in = false;

// Funções para salvar/carregar estado de login na NVS
void save_login_state(bool state) {
    nvs_handle_t handle;
    if (nvs_open("storage", NVS_READWRITE, &handle) == ESP_OK) {
        nvs_set_u8(handle, "login_state", state ? 1 : 0);
        nvs_commit(handle);
        nvs_close(handle);
    }
}

bool load_login_state() {
    nvs_handle_t handle;
    uint8_t state = 0;
    if (nvs_open("storage", NVS_READONLY, &handle) == ESP_OK) {
        nvs_get_u8(handle, "login_state", &state);
        nvs_close(handle);
    }
    return state == 1;
}

void init_spiffs(void) {
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = NULL,
        .max_files = 5,
        .format_if_mount_failed = true
    };
    esp_vfs_spiffs_register(&conf);
}

void wifi_scan() {
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();

    wifi_scan_config_t scan_config = { .ssid = NULL, .bssid = NULL, .channel = 0, .show_hidden = true };
    esp_wifi_scan_start(&scan_config, true);

    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count > MAX_APs) ap_count = MAX_APs;
    esp_wifi_scan_get_ap_records(&ap_count, ap_records);

    esp_wifi_stop();
    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_start();
}

const char* css_style =
    "<style>"
    "body { font-family: Arial; background:#f2f2f2; text-align:center; margin-top:50px; }"
    "form { background:white; padding:20px; border-radius:10px; display:inline-block; box-shadow:0 2px 8px rgba(0,0,0,0.2); }"
    "input[type=text], input[type=password] { width:80%%; padding:10px; margin:5px; border-radius:5px; border:1px solid #ccc; }"
    "input[type=submit], a.button { background:#03A9F4; color:white; padding:10px 20px; text-decoration:none; border-radius:5px; display:inline-block; margin:10px; }"
    "input[type=submit]:hover, a.button:hover { background:#0288D1; }"
    "</style>";

esp_err_t root_get_handler(httpd_req_t *req) {
    char resp[1024];
    snprintf(resp, sizeof(resp),
        "<html><head><meta charset='UTF-8'>%s</head><body>"
        "<h1>WiFi Manager</h1>"
        "<a class='button' href='/login'>Configurar WiFi</a>"
        "<a class='button' href='/exit'>Sair</a>"
        "</body></html>", css_style);
    return httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
}

esp_err_t login_get_handler(httpd_req_t *req) {
    char page[1024];
    snprintf(page, sizeof(page),
        "<html><head><meta charset='UTF-8'>%s</head><body>"
        "<h2>Login</h2>"
        "<form action='/do_login' method='get'>"
        "Usuário:<br><input type='text' name='user'><br><br>"
        "Senha:<br><input type='password' name='pass'><br><br>"
        "<input type='submit' value='Entrar'>"
        "</form></body></html>", css_style);
    return httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
}

esp_err_t do_login_handler(httpd_req_t *req) {
    char buf[128];
    size_t len = httpd_req_get_url_query_len(req) + 1;
    char user[32] = "", pass[32] = "";

    if (len > 1 && len < sizeof(buf)) {
        httpd_req_get_url_query_str(req, buf, len);
        httpd_query_key_value(buf, "user", user, sizeof(user));
        httpd_query_key_value(buf, "pass", pass, sizeof(pass));

        if (strcmp(user, "admin") == 0 && strcmp(pass, "admin123") == 0) {
            logged_in = true;
            save_login_state(true);
            httpd_resp_set_status(req, "302 Found");
            httpd_resp_set_hdr(req, "Location", "/config");
            return httpd_resp_send(req, NULL, 0);
        }
    }

    const char *resp = "<html><head><meta charset='UTF-8'></head><body><h3>Login inválido!</h3><a class='button' href='/login'>Tentar novamente</a></body></html>";
    return httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
}

esp_err_t logout_handler(httpd_req_t *req) {
    logged_in = false;
    save_login_state(false);
    const char *resp =
        "<html><head><meta charset='UTF-8'>"
        "<title>Logout</title>"
        "</head><body>"
        "<h2>Logout realizado com sucesso!</h2>"
        "<a class='button' href='/login'>Ir para Login</a>"
        "</body></html>";
    return httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
}

esp_err_t config_get_handler(httpd_req_t *req) {
    if (!logged_in) {
        const char *resp = "<html><body><h3>Acesso negado! Faça login primeiro.</h3><a class='button' href='/login'>Ir para Login</a></body></html>";
        return httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    }

    char *form = malloc(8192);
    if (!form) return ESP_FAIL;

    snprintf(form, 8192,
        "<html><head><meta charset='UTF-8'>%s"
        "<script>function fillSSID(ssid) { document.getElementById('ssid').value = ssid; }</script>"
        "</head><body><h2>Configurar WiFi</h2>"
        "<form action='/submit' method='get'>"
        "Redes WiFi disponíveis:<br>", css_style);

    for (int i = 0; i < ap_count; i++) {
        char line[256];
        snprintf(line, sizeof(line),
            "<input type='radio' name='ssid_select' onclick=\"fillSSID('%s')\">%s (RSSI:%d)<br>",
            ap_records[i].ssid, ap_records[i].ssid, ap_records[i].rssi);
        strcat(form, line);
    }

    strcat(form,
        "<br>SSID: <input type='text' id='ssid' name='ssid'><br><br>"
        "Senha WiFi: <input type='password' name='pass'><br><br>"
        "IP Estático: <input type='text' name='ip'><br><br>"
        "Gateway: <input type='text' name='gateway'><br><br>"
        "Máscara: <input type='text' name='mask'><br><br>"
        "DNS: <input type='text' name='dns'><br><br>"
        "<input type='submit' value='Salvar'>"
        "</form><br>"
        "<a class='button' href='/logout'>Logout</a>"
        "</body></html>");

    esp_err_t result = httpd_resp_send(req, form, HTTPD_RESP_USE_STRLEN);
    free(form);
    return result;
}

esp_err_t form_handler(httpd_req_t *req) {
    char buf[512];
    size_t len = httpd_req_get_url_query_len(req) + 1;
    if (len > 1 && len < sizeof(buf)) {
        httpd_req_get_url_query_str(req, buf, len);
        httpd_query_key_value(buf, "ssid", config_ssid, sizeof(config_ssid));
        httpd_query_key_value(buf, "pass", config_pass, sizeof(config_pass));
        httpd_query_key_value(buf, "ip", config_ip, sizeof(config_ip));
        httpd_query_key_value(buf, "gateway", config_gateway, sizeof(config_gateway));
        httpd_query_key_value(buf, "mask", config_mask, sizeof(config_mask));
        httpd_query_key_value(buf, "dns", config_dns, sizeof(config_dns));
        httpd_query_key_value(buf, "user", config_user, sizeof(config_user));
        httpd_query_key_value(buf, "userpass", config_userpass, sizeof(config_userpass));
    }

    const char *resp = "<html><head><meta charset='UTF-8'></head><body><h3>Configurações salvas!</h3><a class='button' href='/'>Voltar</a></body></html>";
    return httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
}

esp_err_t exit_get_handler(httpd_req_t *req) {
    const char *resp = "<html><head><meta charset='UTF-8'></head><body><h2>Saindo do modo de configuração</h2><p>Obrigado!</p></body></html>";
    return httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
}

void start_web_server() {
    wifi_scan();
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_register_uri_handler(server, &(httpd_uri_t){ .uri = "/", .method = HTTP_GET, .handler = root_get_handler });
        httpd_register_uri_handler(server, &(httpd_uri_t){ .uri = "/login", .method = HTTP_GET, .handler = login_get_handler });
        httpd_register_uri_handler(server, &(httpd_uri_t){ .uri = "/do_login", .method = HTTP_GET, .handler = do_login_handler });
        httpd_register_uri_handler(server, &(httpd_uri_t){ .uri = "/logout", .method = HTTP_GET, .handler = logout_handler });
        httpd_register_uri_handler(server, &(httpd_uri_t){ .uri = "/config", .method = HTTP_GET, .handler = config_get_handler });
        httpd_register_uri_handler(server, &(httpd_uri_t){ .uri = "/submit", .method = HTTP_GET, .handler = form_handler });
        httpd_register_uri_handler(server, &(httpd_uri_t){ .uri = "/exit", .method = HTTP_GET, .handler = exit_get_handler });
    }
}

void start_wifi_ap() {
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    wifi_config_t ap_config = {
        .ap = {
            .ssid = "ESP32Config",
            .ssid_len = strlen("ESP32Config"),
            .password = "12345678",
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK
        }
    };
    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    esp_wifi_start();
    start_web_server();
}

void app_main(void) {
    nvs_flash_init();
    logged_in = load_login_state();
    init_spiffs();
    start_wifi_ap();
}
