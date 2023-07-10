# ESP32 NAT Router Engine 

This is a firmware to use the ESP32 as a  WiFi NAT router.

It is extracted from [dchristl](https://github.com/dchristl/esp32_nat_router_extended). Literally extracted : I have not written a single line of code...

It is an engine, I have removed from the code everything except the strict minimum engine, giving a very compact code.


## How to use it

Include wifi_router.c in your code - or create a component - ,  provide initialization code such as :

```
void app_main(void)
{
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_init();
    initialize_nvs();
    // Setup WIFI
    wifi_init(STA_SSID, STA_PASSWORD, ipaddr_addr(STA_STATIC_IP), ipaddr_addr(STA_SUBNET_MASK), ipaddr_addr(STA_GATEWAY), 
			AP_SSID, AP_PASSWORD, ipaddr_addr(AP_IP), ipaddr_addr(AP_SUBNET_MASK), STA_USER, STA_IDENTITY);
    /* Main loop */
    while (true)
    {
		const TickType_t xDelay = 500 / portTICK_PERIOD_MS;
		 vTaskDelay( xDelay );
    }

}
```

and provide 3 empty call back functions :
```
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
```
## How to test it

Fill src/params.h with your own data, build flash and run...

## Not implemented


Some advanced features are not provided or more exactly not tested :

   * WPA2 enterprise
   * Use of a portmap table see this feature in https://github.com/martin-ger/esp32_nat_router

Corresponding code has been kept in src/esp32_nat_router.c but has not been tried or tested.


