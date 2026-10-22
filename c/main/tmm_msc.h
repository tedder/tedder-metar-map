#pragma once

#include "esp_err.h"
#include "tinyusb_msc.h"

/**
 * Init MSC stack + WL on partition "storage", mount FAT for application at base_path.
 * Call before fopen on /fatfs/...
 */
esp_err_t tmm_msc_storage_init(const char *base_path);

/** Start USB device stack (MSC visible to host after mount switch). */
esp_err_t tmm_usb_device_start(void);

tinyusb_msc_storage_handle_t tmm_msc_handle(void);

esp_err_t tmm_msc_set_mount_app(void);
esp_err_t tmm_msc_set_mount_usb(void);

bool tmm_msc_is_usb_mode(void);
