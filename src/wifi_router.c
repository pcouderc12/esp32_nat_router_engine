
/* 

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <pthread.h>
#include "esp_system.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_wpa2.h"
#include "esp_event.h"

#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "dhcpserver/dhcpserver.h"

//#include "lwip/err.h"
#include "lwip/sys.h"

#include "lwip/dns.h"
#include "esp_mac.h"

#if !IP_NAPT
#error "IP_NAPT must be defined"
#endif
#include "lwip/lwip_napt.h"




/* FreeRTOS event group to signal when we are connected*/
static EventGroupHandle_t wifi_event_group;

/* The event group allows multiple bits for each event, but we only care about one event
 * - are we connected to the AP with an IP? */
const int WIFI_CONNECTED_BIT = BIT0;

bool ap_connect = false;

uint32_t my_ip;
static uint32_t my_ap_ip,my_dns;


esp_netif_t *wifiAP;
esp_netif_t *wifiSTA;


esp_err_t delete_portmap_tab();
esp_err_t apply_portmap_tab();

static const char *TAG = "ESP32 NAT router";

/* Console command history can be stored to and loaded from a file.
 * The easiest way to do this is to use FATFS filesystem on top of
 * wear_levelling library.
 */

void fillDNS(esp_ip_addr_t *dnsserver, esp_ip_addr_t *fallback)
{

    if (my_dns == -1)
    {
        ESP_LOGI(TAG, "Setting DNS server to upstream DNS");
        dnsserver->u_addr.ip4.addr = fallback->u_addr.ip4.addr;
    }
    else
    {
        ESP_LOGI(TAG, "Setting custom DNS server to  customDNS");
        dnsserver->u_addr.ip4.addr = my_dns;
    }
}
void setDnsServer(esp_netif_t *network, esp_ip_addr_t *dnsIP)
{

    esp_netif_dns_info_t dns_info = {0};
    memset(&dns_info, 8, sizeof(dns_info));
    dns_info.ip = *dnsIP;
    dns_info.ip.type = IPADDR_TYPE_V4;

    esp_netif_dhcps_stop(network);
    ESP_ERROR_CHECK(esp_netif_set_dns_info(wifiAP, ESP_NETIF_DNS_MAIN, &dns_info));
    dhcps_offer_t opt_val = OFFER_DNS; // supply a dns server via dhcps
    esp_netif_dhcps_option(network, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &opt_val, sizeof(opt_val));
    esp_netif_dhcps_start(network);
    ESP_LOGI(TAG, "set dns to: " IPSTR, IP2STR(&(dns_info.ip.u_addr.ip4)));
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        ESP_LOGI(TAG, "disconnected - retry to connect to the AP");
        ap_connect = false;
        esp_wifi_connect();
        ESP_LOGI(TAG, "retry to connect to the AP");
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        ap_connect = true;
        my_ip = event->ip_info.ip.addr;
        delete_portmap_tab();
        apply_portmap_tab();
        esp_netif_dns_info_t dns;
        if (esp_netif_get_dns_info(wifiSTA, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK)
        {
            esp_ip_addr_t newDns;
            fillDNS(&newDns, &dns.ip);
            setDnsServer(wifiAP, &newDns); // Set the correct DNS server for the AP clients
        }
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED)
    {
        ESP_LOGI(TAG, "Station connected");
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED)
    {
        ESP_LOGI(TAG, "Station disconnected");
    }
}
const int CONNECTED_BIT = BIT0;
#define JOIN_TIMEOUT_MS (2000)

void setWpaEnterprise(const char *sta_identity, const char *sta_user, const char *password)
{
#ifndef WPA_ENTERPRISE 
// TODO this needs nvs
	ESP_ERROR_CHECK(ESP_ERR_NOT_SUPPORTED);
#else
    if (sta_identity != NULL && strlen(sta_identity) > 0)
    {
        ESP_ERROR_CHECK(esp_wifi_sta_wpa2_ent_set_identity((uint8_t *)sta_identity, strlen(sta_identity)));
    }

    if (sta_user != NULL && strlen(sta_user) != 0)
    {
        ESP_ERROR_CHECK(esp_wifi_sta_wpa2_ent_set_username((uint8_t *)sta_user, strlen(sta_user)));
    }
    if (password != NULL && strlen(password) > 0)
    {
        ESP_ERROR_CHECK(esp_wifi_sta_wpa2_ent_set_password((uint8_t *)password, strlen(password)));
    }
    ESP_LOGI(TAG, "Reading WPA certificate");

    char *cer = NULL;
    size_t len = 0;

    get_config_param_blob("cer", &cer, &len);
    if (cer != NULL && strlen(cer) != 0)
    {
        ESP_LOGI(TAG, "Setting WPA certificate with length %d\n%s", len, cer);
        ESP_ERROR_CHECK(esp_wifi_sta_wpa2_ent_set_ca_cert((uint8_t *)cer, strlen(cer)));
    }
    else
    {
        ESP_LOGI(TAG, "No certificate used");
    }

    ESP_ERROR_CHECK(esp_wifi_sta_wpa2_ent_enable());
}

esp_err_t get_config_param_blob(char *name, char **param, size_t *blob_len)
{
    nvs_handle_t nvs;

    esp_err_t err = nvs_open(PARAM_NAMESPACE, NVS_READONLY, &nvs);
    if (err == ESP_OK)
    {
        size_t len;
        if ((err = nvs_get_blob(nvs, name, NULL, &len)) == ESP_OK)
        {
            *param = (char *)malloc(len);
            *blob_len = len;

            err = nvs_get_blob(nvs, name, *param, &len);
        }
        else
        {
            return err;
        }
        nvs_close(nvs);
    }
    else
    {
        return err;
    }
    return ESP_OK;

#endif
}
void wifi_init(const char *ssid, const char *passwd, const uint32_t static_ip, const uint32_t subnet_mask, const uint32_t gateway_addr, const char *ap_ssid, const char *ap_passwd, const uint32_t ap_ip, const uint32_t ap_netmask, const uint32_t custom_dns, const char *sta_user, const char *sta_identity)
{

    wifi_event_group = xEventGroupCreate();
    wifiAP = esp_netif_create_default_wifi_ap();
    wifiSTA = esp_netif_create_default_wifi_sta();

    esp_netif_ip_info_t ipInfo_sta;
    if ((strlen(ssid) > 0) && ((static_ip) !=-1) && ((subnet_mask) != -1) && ((gateway_addr) != -1))
    {
        my_ip = ipInfo_sta.ip.addr = (static_ip);
        ipInfo_sta.gw.addr = (gateway_addr);
        ipInfo_sta.netmask.addr = (subnet_mask);
        esp_netif_dhcpc_stop(wifiSTA); // Don't run a DHCP client
        esp_netif_set_ip_info(wifiSTA, &ipInfo_sta);
        apply_portmap_tab();
    }

    my_ap_ip = (ap_ip);
	my_dns=custom_dns;

    esp_netif_ip_info_t ipInfo_ap;
    ipInfo_ap.ip.addr = my_ap_ip;
    ipInfo_ap.gw.addr = my_ap_ip;


    ipInfo_ap.netmask.addr = (ap_netmask);


    esp_netif_dhcps_stop(wifiAP); // stop before setting ip WifiAP
    esp_netif_set_ip_info(wifiAP, &ipInfo_ap);
    esp_netif_dhcps_start(wifiAP);

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    /* ESP WIFI CONFIG */
    wifi_config_t wifi_config = {0};
    wifi_config_t ap_config = {
        .ap = {
            .authmode = WIFI_AUTH_WPA2_PSK, // Check WIFI_AUTH_WPA2_WPA3_PSK with ESP-IDF 5.1
            .ssid_hidden = 0,
            // .channel =
            .max_connection = 10,
            .beacon_interval = 100,
            .pairwise_cipher = WIFI_CIPHER_TYPE_CCMP}};

    strlcpy((char *)ap_config.sta.ssid, ap_ssid, sizeof(ap_config.sta.ssid));

    if (strlen(ap_passwd) < 8)
    {
        ap_config.ap.authmode = WIFI_AUTH_OPEN;
    }
    else
    {
        strlcpy((char *)ap_config.sta.password, ap_passwd, sizeof(ap_config.sta.password));
    }

    if (strlen(ssid) > 0)
    {
        strlcpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
        bool isWpaEnterprise = (sta_identity != NULL && strlen(sta_identity) != 0) || (sta_user != NULL && strlen(sta_user) != 0);
        if (!isWpaEnterprise)
        {
            strlcpy((char *)wifi_config.sta.password, passwd, sizeof(wifi_config.sta.password));
        }
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
        ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_AP, &ap_config));
        ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));

        if (isWpaEnterprise)
        {
            ESP_LOGI(TAG, "WPA enterprise settings found!");
            setWpaEnterprise(sta_identity, sta_user, passwd);
        }
    }
    else
    {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
        ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_AP, &ap_config));
    }
    xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT,
                        pdFALSE, pdTRUE, JOIN_TIMEOUT_MS / portTICK_PERIOD_MS);
    ESP_ERROR_CHECK(esp_wifi_start());

    if (strlen(ssid) > 0)
    {
        ESP_LOGI(TAG, "wifi_init_apsta finished.");
        ESP_LOGI(TAG, "connect to ap SSID: %s Password: %s", ssid, passwd);
    }
    else
    {
        ESP_LOGI(TAG, "wifi_init_ap with default finished.");
    }
    ip_napt_enable(my_ap_ip, 1);
    // ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(80));
    // int8_t j;
    // ESP_ERROR_CHECK(esp_wifi_get_max_tx_power(&j));
    // ESP_LOGI(TAG, "\t\t\tWifi power get is %d.", j);
}
