#include "tmm_msc.h"

#include <string.h>

#include "esp_partition.h"
#include "sdkconfig.h"
#include "soc/soc_caps.h"
#include "wear_levelling.h"

#if SOC_USB_OTG_SUPPORTED
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_msc.h"

static wl_handle_t s_wl = WL_INVALID_HANDLE;
static tinyusb_msc_storage_handle_t s_msc_hdl = NULL;
static char s_base_path[32];

static void msc_event_cb(tinyusb_msc_storage_handle_t h, tinyusb_msc_event_t *ev, void *arg)
{
    (void)h;
    (void)arg;
    (void)ev;
}

esp_err_t tmm_msc_storage_init(const char *base_path)
{
    tinyusb_msc_driver_config_t dcfg = {
        .callback = msc_event_cb,
        .callback_arg = NULL,
    };
    esp_err_t err = tinyusb_msc_install_driver(&dcfg);
    if (err != ESP_OK) {
        return err;
    }

    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, "storage");
    if (!part) {
        return ESP_ERR_NOT_FOUND;
    }

    err = wl_mount(part, &s_wl);
    if (err != ESP_OK) {
        return err;
    }

    strncpy(s_base_path, base_path, sizeof(s_base_path) - 1);
    s_base_path[sizeof(s_base_path) - 1] = '\0';

    tinyusb_msc_storage_config_t scfg = {
        .medium.wl_handle = s_wl,
        .fat_fs = {
            .base_path = s_base_path,
            .config = {
                .max_files = 8,
                .format_if_mount_failed = true,
                .allocation_unit_size = CONFIG_WL_SECTOR_SIZE,
                .use_one_fat = false,
            },
            .do_not_format = false,
            .format_flags = 0,
        },
        .mount_point = TINYUSB_MSC_STORAGE_MOUNT_APP,
    };

    err = tinyusb_msc_new_storage_spiflash(&scfg, &s_msc_hdl);
    return err;
}

esp_err_t tmm_usb_device_start(void)
{
    tinyusb_config_t cfg = TINYUSB_DEFAULT_CONFIG();
    return tinyusb_driver_install(&cfg);
}

tinyusb_msc_storage_handle_t tmm_msc_handle(void)
{
    return s_msc_hdl;
}

esp_err_t tmm_msc_set_mount_app(void)
{
    if (!s_msc_hdl) {
        return ESP_ERR_INVALID_STATE;
    }
    return tinyusb_msc_set_storage_mount_point(s_msc_hdl, TINYUSB_MSC_STORAGE_MOUNT_APP);
}

esp_err_t tmm_msc_set_mount_usb(void)
{
    if (!s_msc_hdl) {
        return ESP_ERR_INVALID_STATE;
    }
    return tinyusb_msc_set_storage_mount_point(s_msc_hdl, TINYUSB_MSC_STORAGE_MOUNT_USB);
}

bool tmm_msc_is_usb_mode(void)
{
    tinyusb_msc_mount_point_t mp = TINYUSB_MSC_STORAGE_MOUNT_APP;
    if (!s_msc_hdl) {
        return false;
    }
    if (tinyusb_msc_get_storage_mount_point(s_msc_hdl, &mp) != ESP_OK) {
        return false;
    }
    return mp == TINYUSB_MSC_STORAGE_MOUNT_USB;
}

#else /* !SOC_USB_OTG_SUPPORTED */

esp_err_t tmm_msc_storage_init(const char *base_path)
{
    (void)base_path;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t tmm_usb_device_start(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

tinyusb_msc_storage_handle_t tmm_msc_handle(void)
{
    return NULL;
}

esp_err_t tmm_msc_set_mount_app(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t tmm_msc_set_mount_usb(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

bool tmm_msc_is_usb_mode(void)
{
    return false;
}

#endif
