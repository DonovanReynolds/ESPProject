/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */
/* Includes */
#include "gatt_svc.h"
#include "common.h"
#include "led.h"
#include <inttypes.h>
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_log.h"

#define FILE_BUF_SIZE (200 * 1024)
#define SEND_BUFFER 517
static uint32_t file_read_offset = 0;
static uint16_t file_offset_chr_val_handle;
static esp_ota_handle_t ota_handle = 0;
static const esp_partition_t *ota_partition = NULL;
static bool is_ota_active = false;
static bool first_chunk = true;

/* Private function declarations */
static int led_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg);

/* Private variables */
static uint16_t led_chr_val_handle;
static uint16_t file_rw_chr_val_handle;

static const ble_uuid16_t auto_io_svc_uuid = BLE_UUID16_INIT(0x1815);
static const ble_uuid128_t led_chr_uuid =
    BLE_UUID128_INIT(0x23, 0xd1, 0xbc, 0xea, 0x5f, 0x78, 0x23, 0x15,
                     0xde, 0xef, 0x12, 0x12, 0x25, 0x15, 0x00, 0x00);
static const ble_uuid128_t file_rw_chr_uuid =
    BLE_UUID128_INIT(0x23, 0xd1, 0xbc, 0xea, 0x5f, 0x78, 0x23, 0x15,
                     0xde, 0xef, 0x12, 0x12, 0x26, 0x15, 0x00, 0x00);
static const ble_uuid128_t file_offset_chr_uuid =
    BLE_UUID128_INIT(0x23, 0xd1, 0xbc, 0xea, 0x5f, 0x78, 0x23, 0x15,
                     0xde, 0xef, 0x12, 0x12, 0x27, 0x15, 0x00, 0x00);

/* Helper functions */
void gatt_complete_ota(void)
{
    if (is_ota_active)
    {
        esp_err_t err = esp_ota_end(ota_handle);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
            return;
        }

        err = esp_ota_set_boot_partition(ota_partition);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
            return;
        }

        ESP_LOGI(TAG, "OTA complete. Rebooting...");
        vTaskDelay(1000 / portTICK_PERIOD_MS);
        esp_restart();
    }
}


void gatt_reset_file_buffer(void)
{
    // Reset state
    is_ota_active = false;
    first_chunk = true; 
    if (ota_handle) {
        esp_ota_end(ota_handle);
        ota_handle = 0;
    }
    // Check extension
    const char *ota_path = "/spiffs/upload.bin";
    const char *ext = strrchr(ota_path, '.');

    if (ext && strcmp(ext, ".bin") == 0)
    {
        ota_partition = esp_ota_get_next_update_partition(NULL);
        if (!ota_partition)
        {
            ESP_LOGE(TAG, "Failed to get OTA partition");
            return;
        }

        esp_err_t err = esp_ota_begin(ota_partition, OTA_SIZE_UNKNOWN, &ota_handle);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
            return;
        }

        is_ota_active = true;
        ESP_LOGI(TAG, "OTA upload started to partition: %s", ota_partition->label);
    }
    else
    {
        FILE *f = fopen("/spiffs/upload.txt", "w");
        if (f)
        {
            fclose(f);
            ESP_LOGI(TAG, "File reset (truncated)");
        }
    }
}

/* Private functions */
static int led_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    /* Local variables */
    int rc;

    /* Handle access events */
    /* Note: LED characteristic is write only */
    switch (ctxt->op)
    {

    /* Write characteristic event */
    case BLE_GATT_ACCESS_OP_WRITE_CHR:
        /* Verify connection handle */
        if (conn_handle != BLE_HS_CONN_HANDLE_NONE)
        {
            ESP_LOGI(TAG, "characteristic write; conn_handle=%d attr_handle=%d",
                     conn_handle, attr_handle);
        }
        else
        {
            ESP_LOGI(TAG,
                     "characteristic write by nimble stack; attr_handle=%d",
                     attr_handle);
        }

        /* Verify attribute handle */
        if (attr_handle == led_chr_val_handle)
        {
            /* Verify access buffer length */
            if (ctxt->om->om_len == 1)
            {
                /* Turn the LED on or off according to the operation bit */
                if (ctxt->om->om_data[0])
                {
                    led_on();
                    ESP_LOGI(TAG, "led turned on!");
                }
                else
                {
                    led_off();
                    ESP_LOGI(TAG, "led turned off!");
                }
            }
            else
            {
                goto error;
            }
            return rc;
        }
        goto error;

    /* Unknown event */
    default:
        goto error;
    }

error:
    ESP_LOGE(TAG,
             "unexpected access operation to led characteristic, opcode: %d",
             ctxt->op);
    return BLE_ATT_ERR_UNLIKELY;
}
static int file_rw_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    switch (ctxt->op)
    {
    case BLE_GATT_ACCESS_OP_WRITE_CHR:
    {
        size_t len = OS_MBUF_PKTLEN(ctxt->om);

        // Convert incoming BLE data to a flat buffer
        uint8_t temp_buf[SEND_BUFFER]; // enough for each BLE chunk
        if (len > sizeof(temp_buf)) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }

        int rc = ble_hs_mbuf_to_flat(ctxt->om, temp_buf, len, NULL);
        if (rc != 0) {
            return BLE_ATT_ERR_UNLIKELY;
        }

        // Only check for OTA on the first chunk
        if (first_chunk) {
            first_chunk = false;

            if (temp_buf[0] == 0xE9) {
                ESP_LOGI(TAG, "Detected OTA image by magic byte");

                ota_partition = esp_ota_get_next_update_partition(NULL);
                if (!ota_partition) {
                    ESP_LOGE(TAG, "Failed to get OTA partition");
                    return BLE_ATT_ERR_UNLIKELY;
                }

                esp_err_t err = esp_ota_begin(ota_partition, OTA_SIZE_UNKNOWN, &ota_handle);
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
                    return BLE_ATT_ERR_UNLIKELY;
                }

                is_ota_active = true;
                ESP_LOGI(TAG, "OTA upload started to partition: %s", ota_partition->label);
            } else {
                ESP_LOGI(TAG, "Not OTA image, defaulting to file write mode");
        
                FILE *f = fopen("/spiffs/upload.txt", "w");
                if (f) {
                    fclose(f);
                    ESP_LOGI(TAG, "Created upload.txt for writing");
                } else {
                    ESP_LOGE(TAG, "Failed to create upload.txt");
                    return BLE_ATT_ERR_UNLIKELY;
                }
                is_ota_active = false;
            }
        }

        if (is_ota_active && ota_handle != 0) {
            if (len == 7 && memcmp(temp_buf, "OTA_END", 7) == 0) {
                ESP_LOGI(TAG, "Received OTA_END signal from client");
                gatt_complete_ota();  // reboot and finish
                return 0;
            }
        
            rc = esp_ota_write(ota_handle, temp_buf, len);
            if (rc != ESP_OK) {
                ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(rc));
                return BLE_ATT_ERR_UNLIKELY;
            }
        
            ESP_LOGI(TAG, "OTA chunk written: %zu bytes", len);
        } else {
            FILE *f = fopen("/spiffs/upload.txt", "ab");
            if (!f) {
                ESP_LOGE(TAG, "Failed to open file for writing");
                return BLE_ATT_ERR_UNLIKELY;
            }

            size_t written = fwrite(temp_buf, 1, len, f);
            fclose(f);

            if (written != len) {
                ESP_LOGE(TAG, "Failed to write all data to file");
                return BLE_ATT_ERR_UNLIKELY;
            }

            ESP_LOGI(TAG, "Wrote %zu bytes to file", len);
        }

        return 0;
    }

    case BLE_GATT_ACCESS_OP_READ_CHR:
    {
        FILE *f = fopen("/spiffs/upload.txt", "rb");
        if (!f)
        {
            ESP_LOGE(TAG, "Failed to open file for reading");
            return BLE_ATT_ERR_UNLIKELY;
        }

        if (fseek(f, file_read_offset, SEEK_SET) != 0)
        {
            ESP_LOGE(TAG, "Failed to seek to offset %" PRIu32, file_read_offset);
            fclose(f);
            return BLE_ATT_ERR_UNLIKELY;
        }

        uint8_t read_buf[500];
        size_t len = fread(read_buf, 1, sizeof(read_buf), f);
        fclose(f);

        if (len == 0)
        {
            ESP_LOGI(TAG, "EOF reached");
            return 0;
        }        

        if (os_mbuf_append(ctxt->om, read_buf, len) != 0)
        {
            return BLE_ATT_ERR_INSUFFICIENT_RES;
        }

        return 0;
    }

    default:
        return BLE_ATT_ERR_UNLIKELY;
    }
}
static int file_offset_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                                  struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR)
        return BLE_ATT_ERR_UNLIKELY;

    if (ctxt->om->om_len < 4)
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;

    uint32_t offset;
    int rc = ble_hs_mbuf_to_flat(ctxt->om, &offset, sizeof(offset), NULL);
    if (rc != 0)
        return BLE_ATT_ERR_UNLIKELY;

    file_read_offset = offset;
    ESP_LOGI(TAG, "Set read offset to %" PRIu32, file_read_offset);
    return 0;
}

/* GATT services table */
static const struct ble_gatt_svc_def gatt_svr_svcs[] = {
    {.type = BLE_GATT_SVC_TYPE_PRIMARY,
     .uuid = &auto_io_svc_uuid.u,
     .characteristics = (struct ble_gatt_chr_def[]){
         {
             .uuid = &led_chr_uuid.u,
             .access_cb = led_chr_access,
             .flags = BLE_GATT_CHR_F_WRITE,
             .val_handle = &led_chr_val_handle,
         },
         {
             .uuid = &file_rw_chr_uuid.u,
             .access_cb = file_rw_chr_access,
             .val_handle = &file_rw_chr_val_handle,
             .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_READ,
         },
         {
             .uuid = &file_offset_chr_uuid.u,
             .access_cb = file_offset_chr_access,
             .val_handle = &file_offset_chr_val_handle,
             .flags = BLE_GATT_CHR_F_WRITE,
         },
         {0}, // Null terminator
     }},
    {0} // End of service list
};

/* Public functions */
/*
 *  Handle GATT attribute register events
 *      - Service register event
 *      - Characteristic register event
 *      - Descriptor register event
 */
void gatt_svr_register_cb(struct ble_gatt_register_ctxt *ctxt, void *arg)
{
    /* Local variables */
    char buf[BLE_UUID_STR_LEN];

    /* Handle GATT attributes register events */
    switch (ctxt->op)
    {

    /* Service register event */
    case BLE_GATT_REGISTER_OP_SVC:
        ESP_LOGD(TAG, "registered service %s with handle=%d",
                 ble_uuid_to_str(ctxt->svc.svc_def->uuid, buf),
                 ctxt->svc.handle);
        break;

    /* Characteristic register event */
    case BLE_GATT_REGISTER_OP_CHR:
        ESP_LOGD(TAG,
                 "registering characteristic %s with "
                 "def_handle=%d val_handle=%d",
                 ble_uuid_to_str(ctxt->chr.chr_def->uuid, buf),
                 ctxt->chr.def_handle, ctxt->chr.val_handle);
        break;

    /* Descriptor register event */
    case BLE_GATT_REGISTER_OP_DSC:
        ESP_LOGD(TAG, "registering descriptor %s with handle=%d",
                 ble_uuid_to_str(ctxt->dsc.dsc_def->uuid, buf),
                 ctxt->dsc.handle);
        break;

    /* Unknown event */
    default:
        assert(0);
        break;
    }
}

/*
 *  GATT server subscribe event callback
 */

void gatt_svr_subscribe_cb(struct ble_gap_event *event)
{
    /* Check connection handle */
    if (event->subscribe.conn_handle != BLE_HS_CONN_HANDLE_NONE)
    {
        ESP_LOGI(TAG, "subscribe event; conn_handle=%d attr_handle=%d",
                 event->subscribe.conn_handle, event->subscribe.attr_handle);
    }
    else
    {
        ESP_LOGI(TAG, "subscribe by nimble stack; attr_handle=%d",
                 event->subscribe.attr_handle);
    }
}

/*
 *  GATT server initialization
 *      1. Initialize GATT service
 *      2. Update NimBLE host GATT services counter
 *      3. Add GATT services to server
 */
int gatt_svc_init(void)
{
    /* Local variables */
    int rc;

    /* 1. GATT service initialization */
    ble_svc_gatt_init();

    /* 2. Update GATT services counter */
    rc = ble_gatts_count_cfg(gatt_svr_svcs);
    if (rc != 0)
    {
        return rc;
    }

    /* 3. Add GATT services */
    rc = ble_gatts_add_svcs(gatt_svr_svcs);
    if (rc != 0)
    {
        return rc;
    }

    return 0;
}
