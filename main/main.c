#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_err.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "cJSON.h"
#include "order_ui.h"
#include "hex_utils.h"
#include "utf8_validator.h"
#include "font/fonts.h"
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>

static const char *TAG = "TimeSync";

// 外部声明update_time_display函数
extern void update_time_display(long long timestamp);

// 保存时间到NVS
static void save_time_to_nvs(long long timestamp) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "打开NVS失败: %s", esp_err_to_name(err));
        return;
    }
    
    err = nvs_set_i64(nvs_handle, "system_time", timestamp);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "保存时间到NVS失败: %s", esp_err_to_name(err));
    } else {
        nvs_commit(nvs_handle);
        ESP_LOGI(TAG, "时间已保存到NVS: %lld", timestamp);
    }
    
    nvs_close(nvs_handle);
}

// 从NVS恢复时间
static void restore_time_from_nvs(void) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("storage", NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "打开NVS失败或没有保存的时间");
        return;
    }
    
    int64_t saved_time = 0;
    err = nvs_get_i64(nvs_handle, "system_time", &saved_time);
    nvs_close(nvs_handle);
    
    if (err == ESP_OK && saved_time > 0) {
        ESP_LOGI(TAG, "从NVS恢复时间: %lld", saved_time);
        update_time_display(saved_time);
        
        // 设置系统时间
        time_t ts = (time_t)(saved_time / 1000);
        struct timeval tv = { .tv_sec = ts, .tv_usec = 0 };
        settimeofday(&tv, NULL);
    } else {
        ESP_LOGI(TAG, "没有找到保存的时间数据");
    }
}

static void create_order_ui(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0xf5f5f5), 0);
    order_ui_init(scr);
}

static ble_uuid16_t gatt_svc_uuid = BLE_UUID16_INIT(0xABCD);
static ble_uuid16_t gatt_chr_uuid = BLE_UUID16_INIT(0x1234);
static ble_uuid16_t gatt_notify_uuid = BLE_UUID16_INIT(0x5678);
static uint16_t g_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t g_notify_handle = 0;

static int bleprph_gap_event(struct ble_gap_event *event, void *arg);
static void bleprph_advertise(void);
static void bleprph_on_sync(void);
static void bleprph_on_reset(int reason);
static void bleprph_host_task(void *param);
int send_notification(const char *json_str)
{
    if (g_conn_handle == BLE_HS_CONN_HANDLE_NONE || g_notify_handle == 0) {
        return -1;
    }

    struct os_mbuf *om = ble_hs_mbuf_from_flat(json_str, strlen(json_str));
    if (!om) {
        return -1;
    }

    int rc = ble_gattc_notify_custom(g_conn_handle, g_notify_handle, om);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to send notification: %d", rc);
        os_mbuf_free_chain(om);
        return rc;
    }

    return 0;
}

static int bleprph_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                             struct ble_gatt_access_ctxt *ctxt, void *arg);

static const struct ble_gatt_svc_def gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = (ble_uuid_t *)&gatt_svc_uuid,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = (ble_uuid_t *)&gatt_chr_uuid,
                .access_cb = bleprph_chr_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_READ,
                .val_handle = 0,
            },
            {
                .uuid = (ble_uuid_t *)&gatt_notify_uuid,
                .access_cb = bleprph_chr_access,
                .flags = BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_READ,
                .val_handle = &g_notify_handle,
            },
            {0}
        },
    },
    {0}
};

// 解码十六进制字符串到ASCII
static char* decode_hex_content(const char* hex_content, char* buffer, size_t buffer_size) {
    if (!hex_content || !buffer || buffer_size == 0) return NULL;
    
    int hex_len = strlen(hex_content);
    if (hex_len % 2 != 0 || !hex_is_valid(hex_content)) return NULL;
    
    int decoded_len = hex_to_ascii(hex_content, buffer, buffer_size);
    return decoded_len > 0 ? buffer : NULL;
}

// 处理系统消息
static void handle_system_message(cJSON* root) {
    // 检查命令类型
    cJSON *command = cJSON_GetObjectItem(root, "command");
    if (command && cJSON_IsString(command)) {
        const char *command_str = command->valuestring;
        
        // 处理display_test命令的时间戳同步
        if (strcmp(command_str, "display_test") == 0) {
            cJSON *timestamp = cJSON_GetObjectItem(root, "timestamp");
            if (timestamp && cJSON_IsNumber(timestamp)) {
                long long ts = (long long)timestamp->valuedouble;
                ESP_LOGI(TAG, "收到时间戳: %lld", ts);
                
                // 保存时间到NVS
                save_time_to_nvs(ts);
                
                // 更新时间显示
                update_time_display(ts);
            }
        }
    }
    
    cJSON *content = cJSON_GetObjectItem(root, "content");
    if (!content || !cJSON_IsString(content)) return;
    
    char *content_str = content->valuestring;
    char decoded_content[256] = {0};
    
    // 尝试解码十六进制内容
    if (decode_hex_content(content_str, decoded_content, sizeof(decoded_content))) {
        ESP_LOGI(TAG, "解码系统消息: %s", decoded_content);
        show_popup_message(decoded_content, 3000);
    } else {
        ESP_LOGI(TAG, "系统消息: %s", content_str);
        show_popup_message(content_str, 3000);
    }
}

// 构建菜品字符串
static char* build_dishes_string(cJSON* items) {
    if (!items || !cJSON_IsArray(items)) return NULL;
    
    size_t capacity = 256;
    char *dishes_str = malloc(capacity);
    if (!dishes_str) return NULL;
    
    dishes_str[0] = '\0';
    size_t dishes_len = 0;
    int item_count = 0;
    
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, items) {
        cJSON *name = cJSON_GetObjectItem(item, "name");
        if (!cJSON_IsString(name)) continue;
        
        char *name_str = name->valuestring;
        char decoded_name[128] = {0};
        
        // 尝试解码十六进制菜品名
        if (decode_hex_content(name_str, decoded_name, sizeof(decoded_name))) {
            name_str = decoded_name;
        }
        
        size_t needed_len = dishes_len + (item_count > 0 ? 3 : 0) + strlen(name_str) + 1;
        if (needed_len > capacity) {
            capacity = needed_len * 2;
            char *new_dishes = realloc(dishes_str, capacity);
            if (!new_dishes) {
                free(dishes_str);
                return NULL;
            }
            dishes_str = new_dishes;
        }
        
        if (item_count > 0) {
            strcat(dishes_str, "、");
            dishes_len += 3;
        }
        strcat(dishes_str, name_str);
        dishes_len += strlen(name_str);
        item_count++;
    }
    
    return item_count > 0 ? dishes_str : NULL;
}

// 从订单ID生成订单号
static int generate_order_number(const char* order_id) {
    if (!order_id) return 1;
    
    int len = strlen(order_id);
    int order_num = 1;
    
    if (len > 4) {
        order_num = atoi(order_id + len - 4);
    } else {
        order_num = atoi(order_id);
    }
    
    return order_num > 0 ? order_num : 1;
}

static int bleprph_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                             struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    switch (ctxt->op) {
    case BLE_GATT_ACCESS_OP_WRITE_CHR: {
        uint8_t buf[512];
        uint16_t out_len = 0;
        int rc = ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &out_len);
        if (rc != 0) {
            ESP_LOGE(TAG, "ble_hs_mbuf_to_flat failed: %d", rc);
            return BLE_ATT_ERR_UNLIKELY;
        }
        
        buf[out_len < sizeof(buf) ? out_len : sizeof(buf) - 1] = '\0';
        ESP_LOGI(TAG, "收到蓝牙JSON信息: %s", (char *)buf);
        
        cJSON *root = cJSON_Parse((char *)buf);
        if (!root) {
            // 尝试处理非标准JSON格式
            char *content_start = strstr((char *)buf, "content");
            if (content_start) {
                char *quote_start = strchr(content_start, '"');
                if (quote_start) {
                    char *quote_end = strchr(quote_start + 1, '"');
                    if (quote_end) {
                        *quote_end = '\0';
                        char *hex_content = quote_start + 1;
                        
                        char decoded_content[256] = {0};
                        if (decode_hex_content(hex_content, decoded_content, sizeof(decoded_content))) {
                            ESP_LOGW(TAG, "解码内容: %s", decoded_content);
                            show_popup_message(decoded_content, 3000);
                        }
                        *quote_end = '"';
                    }
                }
            }
            return BLE_ATT_ERR_UNLIKELY;
        }

        // 检查操作类型
        cJSON *type = cJSON_GetObjectItem(root, "type");
        if (type && cJSON_IsString(type)) {
            const char *type_str = type->valuestring;
            
            if (strcmp(type_str, "info") == 0) {
                handle_system_message(root);
            } else if (strcmp(type_str, "add") == 0 || strcmp(type_str, "update") == 0 || strcmp(type_str, "remove") == 0) {
                bsp_display_lock(portMAX_DELAY);
                
                cJSON *id = cJSON_GetObjectItem(root, "orderId");
                if (!id || !cJSON_IsString(id)) {
                    ESP_LOGE(TAG, "无效的订单ID");
                    bsp_display_unlock();
                    cJSON_Delete(root);
                    return 0;
                }
                
                char *order_id = id->valuestring;
                ESP_LOGI(TAG, "处理订单: type=%s, orderId=%s", type_str, order_id);
                
                if (strcmp(type_str, "remove") == 0) {
                    remove_order_by_id(order_id);
                    show_popup_message("订单已删除", 2000);
                } else {
                    char *dishes_str = build_dishes_string(cJSON_GetObjectItem(root, "items"));
                    int order_num = generate_order_number(order_id);
                    
                    if (strcmp(type_str, "add") == 0) {
                        create_dynamic_order_row_with_id(order_id, order_num, dishes_str ? dishes_str : "无菜品");
                        show_popup_message("订单已添加", 2000);
                    } else {
                        update_order_by_id(order_id, order_num, dishes_str ? dishes_str : "无菜品");
                        show_popup_message("订单已更新", 2000);
                    }
                    
                    if (dishes_str) free(dishes_str);
                }
                
                bsp_display_unlock();
            }
        }

        cJSON_Delete(root);
        return 0;
    }
    case BLE_GATT_ACCESS_OP_READ_CHR: {
        const char *resp = "OK";
        int rc = os_mbuf_append(ctxt->om, resp, strlen(resp));
        return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    default:
        return BLE_ATT_ERR_UNLIKELY;
    }
}

/* 蓝牙GAP事件处理 */
static int bleprph_gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            g_conn_handle = event->connect.conn_handle;
            ESP_LOGI(TAG, "Connected, handle=%d", event->connect.conn_handle);
            update_bluetooth_status(true); // 更新蓝牙状态为已连接
        } else {
            ESP_LOGI(TAG, "Connect failed; status=%d", event->connect.status);
            update_bluetooth_status(false); // 更新蓝牙状态为未连接
            bleprph_advertise();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        g_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        ESP_LOGI(TAG, "Disconnected; reason=%d", event->disconnect.reason);
        update_bluetooth_status(false); // 更新蓝牙状态为未连接
        bleprph_advertise();
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        ESP_LOGI(TAG, "Advertising complete");
        bleprph_advertise();
        return 0;

    default:
        return 0;
    }
}

/* 蓝牙广播 */
static void bleprph_advertise(void)
{
    struct ble_gap_adv_params adv_params = {0};
    struct ble_hs_adv_fields fields = {0};
    struct ble_hs_adv_fields rsp_fields = {0};
    const char *name = "MuLan";
    int rc;
    uint8_t own_addr_type;

    ble_hs_util_ensure_addr(0);
    rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "infer addr type failed; rc=%d", rc);
        return;
    }

    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    fields.name = (uint8_t *)name;
    fields.name_len = strlen(name);
    fields.name_is_complete = 1;
    fields.uuids16 = (ble_uuid16_t[]){ BLE_UUID16_INIT(0xABCD) };
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;

    rsp_fields.uuids16 = (ble_uuid16_t[]){ BLE_UUID16_INIT(0xABCD) };
    rsp_fields.num_uuids16 = 1;
    rsp_fields.uuids16_is_complete = 1;

    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv set fields failed; rc=%d", rc);
        return;
    }
    rc = ble_gap_adv_rsp_set_fields(&rsp_fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv rsp set fields failed; rc=%d", rc);
        return;
    }

    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER,
                           &adv_params, bleprph_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv start failed; rc=%d", rc);
        return;
    }
    ESP_LOGI(TAG, "Advertising started: %s", name);
}

/* 蓝牙同步回调 */
static void bleprph_on_sync(void)
{
    uint8_t own_addr_type;
    uint8_t addr_val[6];
    int rc;

    ble_hs_util_ensure_addr(0);
    rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc == 0 && ble_hs_id_copy_addr(own_addr_type, addr_val, NULL) == 0) {
        ESP_LOGI(TAG, "Device Address: %02x:%02x:%02x:%02x:%02x:%02x",
                 addr_val[5], addr_val[4], addr_val[3],
                 addr_val[2], addr_val[1], addr_val[0]);
    }

    bleprph_advertise();
}

/* 蓝牙重置回调 */
static void bleprph_on_reset(int reason)
{
    ESP_LOGE(TAG, "Resetting state; reason=%d", reason);
}

/* 蓝牙主机任务 */
static void bleprph_host_task(void *param)
{
    ESP_LOGI(TAG, "BLE Host Task Started");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void app_main(void)
{
    // 初始化NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 初始化蓝牙
    ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %d", ret);
        return;
    }

    ble_svc_gap_init();
    ble_svc_gatt_init();

    ble_hs_cfg.reset_cb = bleprph_on_reset;
    ble_hs_cfg.sync_cb = bleprph_on_sync;

    int rc = ble_svc_gap_device_name_set("MuLan");
    if (rc != 0) {
        ESP_LOGE(TAG, "set device name failed; rc=%d", rc);
    }

    rc = ble_gatts_count_cfg(gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_count_cfg failed; rc=%d", rc);
        return;
    }
    rc = ble_gatts_add_svcs(gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_add_svcs failed; rc=%d", rc);
        return;
    }

    nimble_port_freertos_init(bleprph_host_task);

    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size = BSP_LCD_DRAW_BUFF_SIZE,
        .double_buffer = BSP_LCD_DRAW_BUFF_DOUBLE,
        .flags = {
            .buff_dma = true,
            .buff_spiram = true,
            .sw_rotate = false,
        }
    };
    bsp_display_start_with_config(&cfg);
    bsp_display_backlight_on();

    bsp_display_lock(0);
    order_ui_init(lv_scr_act());
    bsp_display_unlock();
    
    // 从NVS恢复保存的时间
    restore_time_from_nvs();
}
