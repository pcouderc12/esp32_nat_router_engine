FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.*)

idf_component_register(SRCS ${app_sources} INCLUDE_DIRS "." REQUIRES     nvs_flash  esp_wifi  wpa_supplicant  esp_netif     )

