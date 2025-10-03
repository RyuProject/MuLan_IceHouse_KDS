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
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

static const char *TAG = "TimeSync";

// 全局互斥锁，保护共享资源
static SemaphoreHandle_t g_json_mutex = NULL;
static SemaphoreHandle_t g_time_mutex = NULL;

// 外部声明update_time_display函数
extern void update_time_display(long long timestamp);

// 将 "9/28/2025, 6:00:26 PM" 格式转换为Unix时间戳
static long long parse_timestamp_string(const char* timestamp_str) {
    if (!timestamp_str) return 0;
    
    struct tm tm = {0};
    char am_pm[3] = {0};
    int parsed_fields;
    
    // 解析格式: "9/28/2025, 6:00:26 PM"
    parsed_fields = sscanf(timestamp_str, "%d/%d/%d, %d:%d:%d %2s", 
                          &tm.tm_mon, &tm.tm_mday, &tm.tm_year,
                          &tm.tm_hour, &tm.tm_min, &tm.tm_sec, am_pm);
    
    if (parsed_fields == 7) {
        // 验证输入数据的有效性
        if (tm.tm_mon < 1 || tm.tm_mon > 12 || 
            tm.tm_mday < 1 || tm.tm_mday > 31 ||
            tm.tm_year < 2020 || tm.tm_year > 2100 ||
            tm.tm_hour < 0 || tm.tm_hour > 23 ||
            tm.tm_min < 0 || tm.tm_min > 59 ||
            tm.tm_sec < 0 || tm.tm_sec > 59) {
            ESP_LOGE(TAG, "无效的时间戳格式: %s", timestamp_str);
            return 0;
        }
        
        // 调整年份和月份格式
        tm.tm_year -= 1900;  // 年份从1900开始
        tm.tm_mon -= 1;      // 月份从0开始
        
        // 处理AM/PM
        if (strcmp(am_pm, "PM") == 0 && tm.tm_hour < 12) {
            tm.tm_hour += 12;
        } else if (strcmp(am_pm, "AM") == 0 && tm.tm_hour == 12) {
            tm.tm_hour = 0;
        }
        
        // 转换为Unix时间戳
        time_t ts = mktime(&tm);
        if (ts == -1) {
            ESP_LOGE(TAG, "时间戳转换失败: %s", timestamp_str);
            return 0;
        }
        
        return (long long)ts * 1000;  // 转换为毫秒
    }
    
    ESP_LOGE(TAG, "时间戳解析失败，期望7个字段，实际解析%d个: %s", parsed_fields, timestamp_str);
    return 0;
}

// 保存时间到NVS
static void save_time_to_nvs(long long timestamp) {
    if (timestamp <= 0) {
        ESP_LOGE(TAG, "无效的时间戳: %lld", timestamp);
        return;
    }
    
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "打开NVS失败: %s", esp_err_to_name(err));
        return;
    }
    
    // 检查时间戳是否合理（不能是未来的时间）
    time_t current_time = time(NULL);
    time_t timestamp_sec = (time_t)(timestamp / 1000);
    
    if (timestamp_sec > current_time + 3600) { // 如果时间戳比当前时间晚1小时以上
        ESP_LOGW(TAG, "时间戳可能无效，比当前时间晚: %lld", timestamp);
    }
    
    err = nvs_set_i64(nvs_handle, "system_time", timestamp);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "保存时间到NVS失败: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return;
    }
    
    err = nvs_commit(nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS提交失败: %s", esp_err_to_name(err));
    } else {
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
        // 验证保存的时间是否合理
        time_t current_time = time(NULL);
        time_t saved_time_sec = (time_t)(saved_time / 1000);
        
        if (saved_time_sec > current_time + 86400) { // 如果保存的时间比当前时间晚1天以上
            ESP_LOGW(TAG, "保存的时间可能无效: %lld", saved_time);
            return;
        }
        
        ESP_LOGI(TAG, "从NVS恢复时间: %lld", saved_time);
        update_time_display(saved_time);
        
        // 设置系统时间
        time_t ts = (time_t)(saved_time / 1000);
        struct timeval tv = { .tv_sec = ts, .tv_usec = 0 };
        if (settimeofday(&tv, NULL) != 0) {
            ESP_LOGE(TAG, "设置系统时间失败");
        }
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

// 函数声明
static int bleprph_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                             struct ble_gatt_access_ctxt *ctxt, void *arg);

// 蓝牙服务定义
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

static int bleprph_gap_event(struct ble_gap_event *event, void *arg);
static void bleprph_advertise(void);
static void bleprph_on_sync(void);
static void bleprph_on_reset(int reason);
static void bleprph_host_task(void *param);

// 发送通知函数
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
            if (timestamp) {
                long long ts = 0;
                
                if (cJSON_IsNumber(timestamp)) {
                    // 旧格式：数字时间戳
                    ts = (long long)timestamp->valuedouble;
                } else if (cJSON_IsString(timestamp)) {
                    // 新格式：字符串时间戳 "9/28/2025, 6:00:26 PM"
                    ts = parse_timestamp_string(timestamp->valuestring);
                }
                
                if (ts > 0) {
                    ESP_LOGI(TAG, "收到时间戳: %lld", ts);
                    
                    // 保存时间到NVS
                    save_time_to_nvs(ts);
                    
                    // 更新时间显示
                    update_time_display(ts);
                }
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

// 构建菜品字符串（优化内存管理和错误处理）- 支持新旧两种格式
static char* build_dishes_string(cJSON* items) {
    if (!items || !cJSON_IsArray(items)) return NULL;
    
    size_t capacity = 512; // 增加初始容量
    char *dishes_str = malloc(capacity);
    if (!dishes_str) {
        ESP_LOGE(TAG, "内存分配失败");
        return NULL;
    }
    
    dishes_str[0] = '\0';
    size_t dishes_len = 0;
    int item_count = 0;
    int max_items = 20; // 限制最大菜品数量防止内存溢出
    
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, items) {
        if (item_count >= max_items) {
            ESP_LOGW(TAG, "菜品数量超过限制(%d)，已截断", max_items);
            break;
        }
        
        const char *name_str = NULL;
        char decoded_name[128] = {0};
        const char *display_name = NULL;
        
        // 支持新旧两种格式：
        // 旧格式: {"name": "菜品名"} 或 {"name": "十六进制编码"}
        // 新格式: 直接字符串 "菜品名"
        if (cJSON_IsObject(item)) {
            // 旧格式：包含name字段的对象
            cJSON *name = cJSON_GetObjectItem(item, "name");
            if (!cJSON_IsString(name) || !name->valuestring) {
                continue;
            }
            name_str = name->valuestring;
        } else if (cJSON_IsString(item)) {
            // 新格式：直接字符串
            name_str = item->valuestring;
        } else {
            continue; // 无效格式
        }
        
        display_name = name_str;
        
        // 尝试解码十六进制菜品名
        if (decode_hex_content(name_str, decoded_name, sizeof(decoded_name))) {
            display_name = decoded_name;
        }
        
        size_t name_len = strlen(display_name);
        size_t separator_len = (item_count > 0) ? 3 : 0; // "、"的长度
        size_t needed_len = dishes_len + separator_len + name_len + 1;
        
        if (needed_len > capacity) {
            capacity = needed_len * 2;
            char *new_dishes = realloc(dishes_str, capacity);
            if (!new_dishes) {
                ESP_LOGE(TAG, "内存重新分配失败");
                free(dishes_str);
                return NULL;
            }
            dishes_str = new_dishes;
        }
        
        // 安全地拼接字符串
        if (item_count > 0) {
            strncat(dishes_str, "、", capacity - dishes_len - 1);
            dishes_len += 3;
        }
        
        strncat(dishes_str, display_name, capacity - dishes_len - 1);
        dishes_len += name_len;
        item_count++;
    }
    
    if (item_count == 0) {
        free(dishes_str);
        return NULL;
    }
    
    ESP_LOGI(TAG, "构建菜品字符串成功，包含%d个菜品", item_count);
    return dishes_str;
}

// 从订单ID生成订单号（增强错误处理）
static int generate_order_number(const char* order_id) {
    if (!order_id || strlen(order_id) == 0) {
        ESP_LOGW(TAG, "无效的订单ID，使用默认值1");
        return 1;
    }
    
    int order_num = 1;
    int len = strlen(order_id);
    
    // 尝试从订单ID末尾提取数字
    if (len > 4) {
        const char *num_start = order_id + len - 4;
        order_num = atoi(num_start);
        
        // 验证提取的数字是否有效
        if (order_num <= 0) {
            // 如果末尾提取失败，尝试整个字符串
            order_num = atoi(order_id);
        }
    } else {
        order_num = atoi(order_id);
    }
    
    // 确保订单号在合理范围内
    if (order_num <= 0 || order_num > 999999) {
        ESP_LOGW(TAG, "订单号超出范围(%d)，使用默认值1", order_num);
        order_num = 1;
    }
    
    return order_num;
}

static int bleprph_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                             struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    switch (ctxt->op) {
    case BLE_GATT_ACCESS_OP_WRITE_CHR: {
        // 获取互斥锁保护共享资源
        if (g_json_mutex && xSemaphoreTake(g_json_mutex, pdMS_TO_TICKS(1000)) == pdFALSE) {
            ESP_LOGE(TAG, "获取JSON互斥锁超时");
            return BLE_ATT_ERR_UNLIKELY;
        }
        
        uint8_t buf[1024]; // 增加缓冲区大小
        uint16_t out_len = 0;
        int rc = ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf) - 1, &out_len);
        if (rc != 0) {
            ESP_LOGE(TAG, "ble_hs_mbuf_to_flat failed: %d", rc);
            if (g_json_mutex) xSemaphoreGive(g_json_mutex);
            return BLE_ATT_ERR_UNLIKELY;
        }
        
        // 确保字符串以null结尾
        buf[out_len] = '\0';
        
        // 验证数据长度
        if (out_len == 0 || out_len >= sizeof(buf) - 1) {
            ESP_LOGE(TAG, "无效的数据长度: %d", out_len);
            if (g_json_mutex) xSemaphoreGive(g_json_mutex);
            return BLE_ATT_ERR_UNLIKELY;
        }
        
        ESP_LOGI(TAG, "收到蓝牙JSON信息，长度: %d", out_len);
        ESP_LOGI(TAG, "原始JSON数据: %.*s", out_len, buf);
        
        cJSON *root = cJSON_Parse((char *)buf);
        if (!root) {
            ESP_LOGE(TAG, "JSON解析失败");
            
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
            if (g_json_mutex) xSemaphoreGive(g_json_mutex);
            return BLE_ATT_ERR_UNLIKELY;
        }

        // 检查操作类型 - 支持新旧两种格式
        cJSON *type = cJSON_GetObjectItem(root, "t");
        if (!type) {
            // 向后兼容：如果没有t字段，检查旧的type字段
            type = cJSON_GetObjectItem(root, "type");
        }
        
        if (type && cJSON_IsString(type)) {
            const char *type_str = type->valuestring;
            
            // 支持新旧类型标识符
            if (strcmp(type_str, "info") == 0 || strcmp(type_str, "i") == 0) {
                handle_system_message(root);
            } else if (strcmp(type_str, "add") == 0 || strcmp(type_str, "a") == 0 || 
                       strcmp(type_str, "update") == 0 || strcmp(type_str, "u") == 0 || 
                       strcmp(type_str, "remove") == 0 || strcmp(type_str, "r") == 0) {
                bsp_display_lock(portMAX_DELAY);
                
                // 获取订单ID - 支持新旧两种格式
                cJSON *id = cJSON_GetObjectItem(root, "o");
                if (!id) {
                    // 向后兼容：如果没有o字段，检查旧的orderId字段
                    id = cJSON_GetObjectItem(root, "orderId");
                }
                
                if (!id || !cJSON_IsString(id) || !id->valuestring) {
                    ESP_LOGE(TAG, "无效的订单ID");
                    bsp_display_unlock();
                    cJSON_Delete(root);
                    if (g_json_mutex) xSemaphoreGive(g_json_mutex);
                    return 0;
                }
                
                const char *order_id = id->valuestring;
                ESP_LOGI(TAG, "处理订单: type=%s, orderId=%s", type_str, order_id);
                
                if (strcmp(type_str, "remove") == 0 || strcmp(type_str, "r") == 0) {
                    remove_order_by_id(order_id);
                    show_popup_message("订单已删除", 2000);
                } else {
                    char *dishes_str = NULL;
                    // 获取菜品数据 - 支持新旧两种格式
                    cJSON *items = cJSON_GetObjectItem(root, "c");
                    if (!items) {
                        // 向后兼容：如果没有c字段，检查旧的items字段
                        items = cJSON_GetObjectItem(root, "items");
                    }
                    
                    if (items && cJSON_IsArray(items)) {
                        dishes_str = build_dishes_string(items);
                    }
                    
                    int order_num = generate_order_number(order_id);
                    
                    if (strcmp(type_str, "add") == 0 || strcmp(type_str, "a") == 0) {
                        add_new_order(order_id, order_num, dishes_str ? dishes_str : "无菜品");
                        show_popup_message("新订单已接收", 2000);
                    } else if (strcmp(type_str, "update") == 0 || strcmp(type_str, "u") == 0) {
                        // 检查是出餐完成还是订单编辑
                        cJSON *status = cJSON_GetObjectItem(root, "status");
                        ESP_LOGI(TAG, "解析status字段: %s", status ? "存在" : "不存在");
                        
                        if (status && cJSON_IsBool(status)) {
                            ESP_LOGI(TAG, "status字段类型正确，值为: %d", status->valueint);
                            if (status->valueint) {
                                // status: true - 出餐完成
                                ESP_LOGI(TAG, "检测到出餐完成消息，订单ID: %s", order_id);
                                if (order_id && strlen(order_id) > 0) {
                                    complete_current_order(order_id);
                                    show_popup_message("订单已完成", 2000);
                                } else {
                                    ESP_LOGE("TimeSync", "无效的order_id，无法完成订单");
                                }
                            } else {
                                // status: false - 订单编辑
                                ESP_LOGI(TAG, "检测到订单编辑消息，订单ID: %s", order_id);
                                update_order_by_id(order_id, order_num, dishes_str ? dishes_str : "无菜品");
                                show_popup_message("订单已更新", 2000);
                            }
                        } else {
                            ESP_LOGW(TAG, "status字段无效或缺失，默认处理为订单编辑");
                            // 默认处理为订单编辑
                            update_order_by_id(order_id, order_num, dishes_str ? dishes_str : "无菜品");
                            show_popup_message("订单已更新", 2000);
                        }
                    }
                    
                    if (dishes_str) {
                        free(dishes_str);
                    }
                }
                
                bsp_display_unlock();
            } else {
                ESP_LOGW(TAG, "未知的操作类型: %s", type_str);
            }
        } else {
            ESP_LOGW(TAG, "缺少或无效的type字段");
        }

        cJSON_Delete(root);
        if (g_json_mutex) xSemaphoreGive(g_json_mutex);
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

/* 蓝牙GAP事件处理（增强错误处理） */
static int bleprph_gap_event(struct ble_gap_event *event, void *arg)
{
    if (!event) {
        ESP_LOGE(TAG, "无效的GAP事件");
        return 0;
    }
    
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            g_conn_handle = event->connect.conn_handle;
            ESP_LOGI(TAG, "蓝牙已连接, handle=%d", event->connect.conn_handle);
            update_bluetooth_status(true); // 更新蓝牙状态为已连接
        } else {
            ESP_LOGW(TAG, "蓝牙连接失败; status=%d", event->connect.status);
            update_bluetooth_status(false); // 更新蓝牙状态为未连接
            bleprph_advertise();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        g_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        ESP_LOGI(TAG, "蓝牙断开连接; reason=%d", event->disconnect.reason);
        update_bluetooth_status(false); // 更新蓝牙状态为未连接
        bleprph_advertise();
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        ESP_LOGI(TAG, "蓝牙广播完成");
        bleprph_advertise();
        return 0;
        
    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(TAG, "蓝牙订阅事件");
        return 0;
        
    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU更新: %d", event->mtu.value);
        return 0;

    default:
        ESP_LOGD(TAG, "未处理的GAP事件类型: %d", event->type);
        return 0;
    }
}

/* 蓝牙广播（增强错误处理） */
static void bleprph_advertise(void)
{
    struct ble_gap_adv_params adv_params = {0};
    struct ble_hs_adv_fields fields = {0};
    struct ble_hs_adv_fields rsp_fields = {0};
    const char *name = "MuLan";
    int rc;
    uint8_t own_addr_type;

    // 确保蓝牙地址可用
    ble_hs_util_ensure_addr(0);
    rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "推断地址类型失败; rc=%d", rc);
        return;
    }

    // 设置广播字段
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    fields.name = (uint8_t *)name;
    fields.name_len = strlen(name);
    fields.name_is_complete = 1;
    fields.uuids16 = (ble_uuid16_t[]){ BLE_UUID16_INIT(0xABCD) };
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;

    // 设置响应字段
    rsp_fields.uuids16 = (ble_uuid16_t[]){ BLE_UUID16_INIT(0xABCD) };
    rsp_fields.num_uuids16 = 1;
    rsp_fields.uuids16_is_complete = 1;

    // 设置广播字段
    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "设置广播字段失败; rc=%d", rc);
        return;
    }
    
    // 设置响应字段
    rc = ble_gap_adv_rsp_set_fields(&rsp_fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "设置响应字段失败; rc=%d", rc);
        return;
    }

    // 配置广播参数
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    adv_params.itvl_min = BLE_GAP_ADV_FAST_INTERVAL1_MIN;
    adv_params.itvl_max = BLE_GAP_ADV_FAST_INTERVAL1_MAX;

    // 开始广播
    rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER,
                           &adv_params, bleprph_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "启动广播失败; rc=%d", rc);
        return;
    }
    ESP_LOGI(TAG, "蓝牙广播已启动: %s", name);
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
    ESP_LOGI(TAG, "MuLan IceHouse KDS 单订单焦点模式启动中...");
    
    // 创建互斥锁
    g_json_mutex = xSemaphoreCreateMutex();
    g_time_mutex = xSemaphoreCreateMutex();
    
    if (!g_json_mutex || !g_time_mutex) {
        ESP_LOGE(TAG, "创建互斥锁失败");
        return;
    }

    // 初始化NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGI(TAG, "NVS需要擦除并重新初始化");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "NVS初始化完成");

    // 初始化蓝牙
    ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "蓝牙端口初始化失败: %d", ret);
        vSemaphoreDelete(g_json_mutex);
        vSemaphoreDelete(g_time_mutex);
        return;
    }

    // 初始化GAP和GATT服务
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ESP_LOGI(TAG, "蓝牙服务初始化完成");

    // 配置蓝牙回调
    ble_hs_cfg.reset_cb = bleprph_on_reset;
    ble_hs_cfg.sync_cb = bleprph_on_sync;

    // 设置设备名称
    int rc = ble_svc_gap_device_name_set("MuLan");
    if (rc != 0) {
        ESP_LOGE(TAG, "设置设备名称失败; rc=%d", rc);
    }

    // 配置GATT服务
    rc = ble_gatts_count_cfg(gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "GATT服务计数配置失败; rc=%d", rc);
        vSemaphoreDelete(g_json_mutex);
        vSemaphoreDelete(g_time_mutex);
        return;
    }
    
    rc = ble_gatts_add_svcs(gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "添加GATT服务失败; rc=%d", rc);
        vSemaphoreDelete(g_json_mutex);
        vSemaphoreDelete(g_time_mutex);
        return;
    }
    ESP_LOGI(TAG, "GATT服务配置完成");

    // 启动蓝牙主机任务
    nimble_port_freertos_init(bleprph_host_task);
    ESP_LOGI(TAG, "蓝牙主机任务已启动");

    // 配置并启动显示
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
    
    lv_display_t* disp = bsp_display_start_with_config(&cfg);
    if (disp == NULL) {
        ESP_LOGE(TAG, "显示启动失败");
        vSemaphoreDelete(g_json_mutex);
        vSemaphoreDelete(g_time_mutex);
        return;
    }
    
    bsp_display_backlight_on();
    ESP_LOGI(TAG, "显示初始化完成");

    // 初始化UI（单订单焦点模式）
    bsp_display_lock(portMAX_DELAY);
    order_ui_init(lv_scr_act());
    bsp_display_unlock();
    ESP_LOGI(TAG, "UI初始化完成");
    
    // 从NVS恢复保存的时间
    restore_time_from_nvs();
    
    ESP_LOGI(TAG, "MuLan IceHouse KDS 单订单焦点模式启动完成");
    
    // 保持任务运行
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
