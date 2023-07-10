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

#include "router_globals.h"
#include "params.h"

static const char *TAG = "Test ESP32 NAT router";
/* WPA2 settings */
char *sta_identity = NULL;
char *sta_user = NULL;

char *param_set_default(const char *def_val)
{
    char *retval = malloc(strlen(def_val) + 1);
    strcpy(retval, def_val);
    return retval;
}

esp_err_t get_config_param_str(char *name, char **param)
{
    nvs_handle_t nvs;

    esp_err_t err = nvs_open(PARAM_NAMESPACE, NVS_READONLY, &nvs);
    if (err == ESP_OK)
    {
        size_t len;
        if ((err = nvs_get_str(nvs, name, NULL, &len)) == ESP_OK)
        {
            *param = (char *)malloc(len);
            err = nvs_get_str(nvs, name, *param, &len);
            ESP_LOGI(TAG, "%s %s", name, *param);
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
}

static void initialize_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

char *getDefaultIPByNetmask()
{
    char *netmask = getNetmask();
    if (strcmp(netmask, DEFAULT_NETMASK_CLASS_A) == 0)
    {
        return DEFAULT_AP_IP_CLASS_A;
    }
    else if (strcmp(netmask, DEFAULT_NETMASK_CLASS_B) == 0)
    {
        return DEFAULT_AP_IP_CLASS_B;
    }
    return DEFAULT_AP_IP_CLASS_C;
}

char *getNetmask()
{
    char *netmask = NULL;

    get_config_param_str("netmask", &netmask);

    if (netmask == NULL)
    {
        return DEFAULT_NETMASK_CLASS_C;
    }
    else
    {
        return netmask;
    }
}

struct portmap_table_entry
{
    u32_t daddr;
    u16_t mport;
    u16_t dport;
    u8_t proto;
    u8_t valid;
};
struct portmap_table_entry portmap_tab[IP_PORTMAP_MAX];
#ifdef USE_PORTMAP
esp_err_t apply_portmap_tab()
{
    for (int i = 0; i < IP_PORTMAP_MAX; i++)
    {
        if (portmap_tab[i].valid)
        {
            ip_portmap_add(portmap_tab[i].proto, my_ip, portmap_tab[i].mport, portmap_tab[i].daddr, portmap_tab[i].dport);
        }
    }
    return ESP_OK;
}

esp_err_t delete_portmap_tab()
{
    for (int i = 0; i < IP_PORTMAP_MAX; i++)
    {
        if (portmap_tab[i].valid)
        {
            ip_portmap_remove(portmap_tab[i].proto, portmap_tab[i].mport);
        }
    }
    return ESP_OK;
}

void print_portmap_tab()
{
    for (int i = 0; i < IP_PORTMAP_MAX; i++)
    {
        if (portmap_tab[i].valid)
        {
            ESP_LOGI(TAG, "%s", portmap_tab[i].proto == PROTO_TCP ? "TCP " : "UDP ");
            esp_ip4_addr_t addr;
            addr.addr = my_ip;
            ESP_LOGI(TAG, IPSTR ":%d -> ", IP2STR(&addr), portmap_tab[i].mport);
            addr.addr = portmap_tab[i].daddr;
            ESP_LOGI(TAG, IPSTR ":%d\n", IP2STR(&addr), portmap_tab[i].dport);
        }
    }
}


esp_err_t get_portmap_tab()
{
    esp_err_t err;
    nvs_handle_t nvs;
    size_t len;

    err = nvs_open(PARAM_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK)
    {
        return err;
    }
    err = nvs_get_blob(nvs, "portmap_tab", NULL, &len);
    if (err == ESP_OK)
    {
        if (len != sizeof(portmap_tab))
        {
            err = ESP_ERR_NVS_INVALID_LENGTH;
        }
        else
        {
            err = nvs_get_blob(nvs, "portmap_tab", portmap_tab, &len);
            if (err != ESP_OK)
            {
                memset(portmap_tab, 0, sizeof(portmap_tab));
            }
        }
    }
    nvs_close(nvs);

    return err;
}

esp_err_t add_portmap(u8_t proto, u16_t mport, u32_t daddr, u16_t dport)
{
    nvs_handle_t nvs;

    for (int i = 0; i < IP_PORTMAP_MAX; i++)
    {
        if (!portmap_tab[i].valid)
        {
            portmap_tab[i].proto = proto;
            portmap_tab[i].mport = mport;
            portmap_tab[i].daddr = daddr;
            portmap_tab[i].dport = dport;
            portmap_tab[i].valid = 1;

            ESP_ERROR_CHECK(nvs_open(PARAM_NAMESPACE, NVS_READWRITE, &nvs));
            ESP_ERROR_CHECK(nvs_set_blob(nvs, "portmap_tab", portmap_tab, sizeof(portmap_tab)));
            ESP_ERROR_CHECK(nvs_commit(nvs));
            ESP_LOGI(TAG, "New portmap table stored.");

            nvs_close(nvs);

            ip_portmap_add(proto, my_ip, mport, daddr, dport);

            return ESP_OK;
        }
    }
    return ESP_ERR_NO_MEM;
}

esp_err_t del_portmap(u8_t proto, u16_t mport)
{
    nvs_handle_t nvs;

    for (int i = 0; i < IP_PORTMAP_MAX; i++)
    {
        if (portmap_tab[i].valid && portmap_tab[i].mport == mport && portmap_tab[i].proto == proto)
        {
            portmap_tab[i].valid = 0;

            ESP_ERROR_CHECK(nvs_open(PARAM_NAMESPACE, NVS_READWRITE, &nvs));
            ESP_ERROR_CHECK(nvs_set_blob(nvs, "portmap_tab", portmap_tab, sizeof(portmap_tab)));
            ESP_ERROR_CHECK(nvs_commit(nvs));
            ESP_LOGI(TAG, "New portmap table stored.");

            nvs_close(nvs);

            ip_portmap_remove(proto, mport);
            return ESP_OK;
        }
    }
    return ESP_OK;
}
#else
esp_err_t delete_portmap_tab()
{
    return ESP_OK;
}
esp_err_t apply_portmap_tab()
{
    return ESP_OK;
}
esp_err_t get_portmap_tab()
{
    return ESP_OK;
}
#endif

void fillMac()
{
    char *customMac = NULL;
    get_config_param_str("custom_mac", &customMac);
    if (customMac != NULL)
    {

        if (strcmp("random", customMac) == 0)
        {
            uint8_t default_mac_addr[6] = {0};
            ESP_ERROR_CHECK(esp_efuse_mac_get_default(default_mac_addr));
            default_mac_addr[5] = (esp_random() % 254 + 1);
            ESP_LOGI(TAG, "Setting random MAC address: %x:%x:%x:%x:%x:%x", default_mac_addr[0], default_mac_addr[1], default_mac_addr[2], default_mac_addr[3], default_mac_addr[4], default_mac_addr[5]);
            esp_base_mac_addr_set(default_mac_addr);
        }
        else
        {
            uint8_t uintMacs[6] = {0};
            ESP_LOGI(TAG, "Setting custom MAC address: %s", customMac);
            int intMacs[6] = {0};

            sscanf(customMac, "%x:%x:%x:%x:%x:%x", &intMacs[0], &intMacs[1], &intMacs[2], &intMacs[3], &intMacs[4], &intMacs[5]);
            for (int i = 0; i < 6; ++i)
            {
                uintMacs[i] = intMacs[i];
            }
            esp_base_mac_addr_set(uintMacs);
        }
    }
}


void wifi_init(const char *ssid, const char *passwd, const uint32_t static_ip, const uint32_t subnet_mask, const uint32_t gateway_addr, const char *ap_ssid, const char *ap_passwd, const uint32_t ap_ip, const uint32_t ap_netmask, const char *sta_user, const char *sta_identity);


void app_main(void)
{
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_init();
    initialize_nvs();

    fillMac();
#ifdef USE_PORTMAP
    get_portmap_tab();
#endif
    // Setup WIFI
    wifi_init(STA_SSID, STA_PASSWORD, ipaddr_addr(STA_STATIC_IP), ipaddr_addr(STA_SUBNET_MASK), ipaddr_addr(STA_GATEWAY), 
			AP_SSID, AP_PASSWORD, ipaddr_addr(AP_IP), ipaddr_addr(AP_SUBNET_MASK), STA_USER, STA_IDENTITY);

    ESP_LOGI(TAG, "NAT is enabled");



    /* Main loop */
    while (true)
    {
		const TickType_t xDelay = 500 / portTICK_PERIOD_MS;
		 vTaskDelay( xDelay );
    }

}
