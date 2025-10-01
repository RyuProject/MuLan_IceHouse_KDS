#include "order_ui.h"
#include "font/lv_symbol_def.h"
#include "lvgl.h"
#include "esp_log.h"
#include "bsp/display.h"
#include "bsp/esp-bsp.h"
#include "font/fonts.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "sys/queue.h"
#include "cJSON.h"

// 外部声明send_notification函数
extern int send_notification(const char *json_str);

static const char *TAG = "OrderUI";
static lv_obj_t *orders_container = NULL;
static lv_obj_t *waiting_label = NULL; // 保存等待标签指针
static lv_obj_t *bluetooth_label = NULL; // 保存蓝牙标签指针
static lv_obj_t *time_label = NULL; // 保存时间标签指针
static bool is_bluetooth_connected = false; // 蓝牙连接状态

// 订单状态枚举
typedef enum {
    ORDER_STATUS_PENDING,    // 等待中
    ORDER_STATUS_COMPLETED,  // 已完成
    ORDER_STATUS_REMOVED     // 已移除
} order_status_t;

// 订单数据结构
typedef struct order_info {
    char *order_id;          // 订单ID
    int order_num;           // 订单号
    char *dishes;            // 菜品信息
    lv_obj_t *row_widget;    // 订单行UI对象
    lv_obj_t *dish_label;   // 菜品标签指针
    lv_obj_t *ready_btn;    // “出餐”按钮指针
    order_status_t status;   // 订单状态
    STAILQ_ENTRY(order_info) entries;
} order_info_t;

// 订单队列
STAILQ_HEAD(order_list_head, order_info);
static struct order_list_head order_list = STAILQ_HEAD_INITIALIZER(order_list);

// 按钮点击事件：已出餐 → 修改按钮状态、文字、颜色
// 通知特性UUID
#define NOTIFY_CHAR_UUID "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

static void btn_ready_cb(lv_event_t *e)
{
    bsp_display_lock(portMAX_DELAY);
    
    lv_obj_t *btn = lv_event_get_target(e);
    
    // 获取订单ID (从按钮的父对象中获取)
    lv_obj_t *row = lv_obj_get_parent(btn);
    if (row) {
        order_info_t *order = NULL;
        STAILQ_FOREACH(order, &order_list, entries) {
            if (order->row_widget == row) {
                // 构建JSON通知消息
                char notify_msg[128];
                snprintf(notify_msg, sizeof(notify_msg), 
                        "{\"orderId\":\"%s\",\"status\":true}", 
                        order->order_id);
                
                // 通过蓝牙通知发送
                send_notification(notify_msg);
                
                ESP_LOGI(TAG, "已发送出餐通知: %s", notify_msg);
                order->ready_btn = NULL; // 按钮将被删除，清空指针
                break;
            }
        }
    }

    // 销毁按钮元素释放内存
    lv_obj_del(btn);

    ESP_LOGI(TAG, "✅ 已出餐按钮被点击并已销毁");
    bsp_display_unlock();
}

// 更新时间显示
void update_time_display(long long timestamp) {
    if (!time_label || !lv_obj_is_valid(time_label)) {
        ESP_LOGE(TAG, "时间标签无效");
        return;
    }
    
    // 将Unix时间戳转换为本地时间
    time_t ts = (time_t)(timestamp / 1000); // 转换为秒（去掉毫秒）
    struct tm *timeinfo = localtime(&ts);
    
    if (!timeinfo) {
        ESP_LOGE(TAG, "时间转换失败");
        return;
    }
    
    // 格式化时间为"PM 6:00"格式
    char time_str[10]; // "PM 6:00" + null terminator
    char am_pm[3] = "AM";
    int hour = timeinfo->tm_hour;
    
    if (hour >= 12) {
        strcpy(am_pm, "PM");
        if (hour > 12) hour -= 12;
    }
    if (hour == 0) hour = 12;
    
    snprintf(time_str, sizeof(time_str), "%s %d:%02d", am_pm, hour, timeinfo->tm_min);
    
    bsp_display_lock(portMAX_DELAY);
    lv_label_set_text(time_label, time_str);
    bsp_display_unlock();
    
    ESP_LOGI(TAG, "更新时间显示: %s", time_str);
}

// 更新蓝牙状态显示
void update_bluetooth_status(bool connected) {
    bsp_display_lock(portMAX_DELAY);
    
    is_bluetooth_connected = connected;
    
    if (bluetooth_label && lv_obj_is_valid(bluetooth_label)) {
        if (connected) {
            lv_label_set_text(bluetooth_label, LV_SYMBOL_BLUETOOTH "OK");
            lv_obj_set_style_text_color(bluetooth_label, lv_color_hex(0x06C260), 0); // 绿色
        } else {
            lv_label_set_text(bluetooth_label, LV_SYMBOL_BLUETOOTH "Ready");
            lv_obj_set_style_text_color(bluetooth_label, lv_color_hex(0xfa5051), 0); // 红色
        }
    }
    
    bsp_display_unlock();
    ESP_LOGI(TAG, "蓝牙状态更新: %s", connected ? "已连接" : "未连接");
}

// 初始化订单UI容器
void order_ui_init(lv_obj_t *parent)
{
    bsp_display_lock(portMAX_DELAY);

    // 创建主容器
    lv_obj_t *main_container = lv_obj_create(parent);
    lv_obj_set_size(main_container, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(main_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(main_container, 0, 0);

    // 创建订单容器
    orders_container = lv_obj_create(main_container);
    lv_obj_set_size(orders_container, LV_PCT(100), LV_PCT(90));
    lv_obj_set_flex_grow(orders_container, 1);
    lv_obj_set_flex_flow(orders_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(orders_container, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scrollbar_mode(orders_container, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_pad_all(orders_container, 10, 0);
    lv_obj_set_style_border_width(orders_container, 0, 0); // 无边框
        lv_obj_set_style_border_width(orders_container, 0, 0); // 无边框

    // 初始显示等待数据
    waiting_label = lv_label_create(orders_container);
    set_font_style(waiting_label, FONT_TYPE_DEVICE, FONT_SIZE_LARGE); // 使用font_device_24.c字体
    lv_label_set_text(waiting_label, "等待订单数据...");
    lv_obj_center(waiting_label);

    // 创建底部状态栏
    lv_obj_t *status_bar = lv_obj_create(main_container);
    lv_obj_set_size(status_bar, LV_PCT(100), 36);
    lv_obj_set_style_bg_color(status_bar, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_style_border_width(status_bar, 0, 0);
    lv_obj_set_style_pad_all(status_bar, 5, 0);
    lv_obj_set_style_radius(status_bar, 0, 0);
    
    // 禁用状态栏的滑动功能
    lv_obj_clear_flag(status_bar, LV_OBJ_FLAG_SCROLLABLE);
    
    // 设置状态栏为flex布局，水平排列
    lv_obj_set_flex_flow(status_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(status_bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    
    // 最左边：版本信息、电量和蓝牙状态
    lv_obj_t *left_container = lv_obj_create(status_bar);
    lv_obj_set_size(left_container, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(left_container, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(left_container, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_SPACE_AROUND);
    lv_obj_set_style_bg_opa(left_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(left_container, 0, 0);
    lv_obj_set_style_pad_hor(left_container, 0, 0);
    
    // 版本信息
    lv_obj_t *version_label = lv_label_create(left_container);
    lv_label_set_text(version_label, "MuLanKDS ver0.1");
    lv_obj_set_style_text_font(version_label, &lv_font_montserrat_14, 0); // 使用LVGL自带的Montserrat 14字体
    lv_obj_set_style_text_color(version_label, lv_color_black(), 0);
        lv_obj_set_style_margin_right(version_label, 20, 0); // 增加100px右边距
    
    // 电量显示
    lv_obj_t *battery_label = lv_label_create(left_container);
    lv_label_set_text(battery_label, LV_SYMBOL_BATTERY_FULL "OK");
    lv_obj_set_style_text_font(battery_label, &lv_font_montserrat_14, 0); // 使用LVGL自带的Montserrat 14字体
    lv_obj_set_style_text_color(battery_label, lv_color_black(), 0);
            lv_obj_set_style_margin_right(battery_label, 20, 0); // 增加100px右边距
    
    // 蓝牙状态
    bluetooth_label = lv_label_create(left_container);
    // 初始显示蓝牙状态
    if (is_bluetooth_connected) {
        lv_label_set_text(bluetooth_label, LV_SYMBOL_BLUETOOTH "OK");
        lv_obj_set_style_text_color(bluetooth_label, lv_color_hex(0x0bc05f), 0); // 绿色
    } else {
        lv_label_set_text(bluetooth_label, LV_SYMBOL_BLUETOOTH "Ready");
        lv_obj_set_style_text_color(bluetooth_label, lv_color_hex(0xfa5050), 0);
    }
    lv_obj_set_style_text_font(bluetooth_label, &lv_font_montserrat_14, 0); // 使用LVGL自带的Montserrat 14字体
    
    // 最右边：时间和员工手册链接
    lv_obj_t *right_container = lv_obj_create(status_bar);
    lv_obj_set_size(right_container, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(right_container, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right_container, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_SPACE_AROUND);
    lv_obj_set_style_bg_opa(right_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(right_container, 0, 0);
    lv_obj_set_style_margin_right(right_container, 120, 0); // 增加100px右边距
    
    // 员工手册超链接
    lv_obj_t *manual_link = lv_label_create(right_container);
    lv_label_set_text(manual_link, "员工手册");
    lv_obj_set_style_text_font(manual_link, FONT_PUHUI_16, 0); // 使用font_puhui_16_4字体
    lv_obj_set_style_text_color(manual_link, lv_color_hex(0x0000FF), 0); // 蓝色
    lv_obj_set_style_text_decor(manual_link, LV_TEXT_DECOR_UNDERLINE, 0); // 下划线
    lv_obj_set_style_pad_right(manual_link, 15, 0); // 与时间间隔
    
    // 时间显示（最右边）
    time_label = lv_label_create(right_container);
    lv_label_set_text(time_label, "00:00");
    lv_obj_set_style_text_font(time_label, &lv_font_montserrat_14, 0); // 使用LVGL自带的Montserrat 14字体
    lv_obj_set_style_text_color(time_label, lv_color_black(), 0);

    bsp_display_unlock();
}



/* 弹出窗口定时器回调 */
static void popup_timer_cb(lv_timer_t *timer) {
    if (!timer) return;
    
    lv_obj_t *popup = (lv_obj_t *)lv_timer_get_user_data(timer);
    
    // 仅在需要访问LVGL对象时加锁
    if (popup && lv_obj_is_valid(popup)) {
        bsp_display_lock(portMAX_DELAY);
        if (lv_obj_is_valid(popup)) {
            lv_obj_del(popup);
        }
        bsp_display_unlock();
    }
    
    lv_timer_del(timer); // 最后删除定时器，且只删一次
}

void show_popup_message(const char *message, uint32_t duration_ms) {
    bsp_display_lock(portMAX_DELAY);
    
    // 创建弹窗
    lv_obj_t *popup = lv_obj_create(lv_scr_act());
    if (!popup) {
        bsp_display_unlock();
        ESP_LOGE(TAG, "创建弹窗失败");
        return;
    }

    // 基本样式设置
    lv_obj_set_size(popup, 280, 80);
    lv_obj_center(popup);
    lv_obj_set_style_bg_color(popup, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(popup, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(popup, 0, 0);
    lv_obj_set_style_pad_all(popup, 5, 0);
    
    // 应用font_puhui_16_4字体
    lv_obj_set_style_text_font(popup, FONT_PUHUI_16, 0);

    // 创建消息标签
    lv_obj_t *label = lv_label_create(popup);
    lv_label_set_text(label, message);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    // 应用font_puhui_16_4字体到标签
    lv_obj_set_style_text_font(label, FONT_PUHUI_16, 0);
    lv_obj_center(label);

    // 设置定时器自动关闭弹出窗口
    lv_timer_t *timer = lv_timer_create(popup_timer_cb, duration_ms, popup);
    if (!timer) {
        // 定时器创建失败时手动删除弹窗
        lv_obj_del(popup);
        bsp_display_unlock();
        ESP_LOGE(TAG, "创建弹窗定时器失败");
        return;
    }
    lv_timer_set_repeat_count(timer, 1);

    bsp_display_unlock();
}

// 根据订单ID查找订单
static order_info_t *find_order_by_id(const char *order_id) {
    order_info_t *order;
    STAILQ_FOREACH(order, &order_list, entries) {
        if (order->order_id && strcmp(order->order_id, order_id) == 0) {
            return order;
        }
    }
    return NULL;
}

// 创建订单行（带订单ID）
#define ORDER_UI_TASK_STACK_SIZE (8 * 1024)  // 增加栈空间到8KB

void create_dynamic_order_row_with_id(const char *order_id, int order_num, const char *dishes) {
    ESP_LOGI(TAG, "开始创建订单行, order_id=%s, dishes=%s", order_id, dishes);
    
    // 检查输入参数有效性
    if (!order_id || !dishes) {
        ESP_LOGE(TAG, "无效的输入参数");
        return;
    }
    
    bsp_display_lock(portMAX_DELAY);
    
    // 检查容器是否有效
    if (!orders_container || !lv_obj_is_valid(orders_container)) {
        ESP_LOGE(TAG, "订单容器无效");
        bsp_display_unlock();
        return;
    }
    
    // 首次添加订单时，清理"等待订单数据..."占位标签
    if (waiting_label && lv_obj_is_valid(waiting_label)) {
        ESP_LOGI(TAG, "删除等待标签");
        lv_obj_del(waiting_label);
        waiting_label = NULL;
    }

    // 创建订单行
    lv_obj_t *row = lv_obj_create(orders_container);
    if (!row) {
        bsp_display_unlock();
        ESP_LOGE(TAG, "创建订单行失败");
        return;
    }
    
    // 分配订单内存
    order_info_t *order = malloc(sizeof(order_info_t));
    if (!order) {
        lv_obj_del(row);
        bsp_display_unlock();
        ESP_LOGE(TAG, "分配订单内存失败");
        return;
    }
    
    order->order_id = strdup(order_id);
    if (!order->order_id) {
        free(order);
        lv_obj_del(row);
        bsp_display_unlock();
        ESP_LOGE(TAG, "复制订单ID失败");
        return;
    }
    
    order->order_num = order_num;
    order->dishes = strdup(dishes);
    if (!order->dishes) {
        free(order->order_id);
        free(order);
        lv_obj_del(row);
        bsp_display_unlock();
        ESP_LOGE(TAG, "复制菜品信息失败");
        return;
    }
    
    order->row_widget = row;
    order->status = ORDER_STATUS_PENDING;
    order->dish_label = NULL; // 初始化菜品标签指针
    order->ready_btn = NULL;  // 初始化“出餐”按钮指针
    // 设置订单行样式 - 参与容器布局
    lv_obj_set_size(row, LV_PCT(100), 96);
    lv_obj_add_flag(row, LV_OBJ_FLAG_EVENT_BUBBLE); // 启用事件冒泡
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_SPACE_BETWEEN);
    lv_obj_set_style_pad_all(row, 10, 0);
    lv_obj_set_style_radius(row, 5, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_opa(row, 25, 0);
    lv_obj_set_style_bg_color(row, lv_color_white(), 0); // 设置白色背景
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    
    // 字体引用已通过字体模块处理

    // 将新订单行移动到容器顶部
    lv_obj_move_to_index(row, 0);
    
    // 确保新订单可见 (修正API调用)
    lv_obj_scroll_to_view(row, LV_ANIM_ON);

    // 限制订单数量为30单
    if (STAILQ_FIRST(&order_list)) {
        int count = 0;
        order_info_t *order, *tmp;
        STAILQ_FOREACH_SAFE(order, &order_list, entries, tmp) {
            count++;
            if (count > 30) {
                // 移除超出限制的旧订单
                STAILQ_REMOVE(&order_list, order, order_info, entries);
                if (order->row_widget && lv_obj_is_valid(order->row_widget)) {
                    lv_obj_del(order->row_widget);
                }
                free(order->order_id);
                free(order->dishes);
                free(order);
                ESP_LOGI(TAG, "移除超出限制的旧订单");
            }
        }
    }
    lv_obj_update_layout(orders_container); // 强制更新布局

    // 左侧：订单信息（水平排列菜品）
    lv_obj_t *left_container = lv_obj_create(row);
    lv_obj_set_parent(left_container, row); // 显式设置父对象
    if (!left_container) {
        free(order->dishes);
        free(order->order_id);
        free(order);
        lv_obj_del(row);
        bsp_display_unlock();
        ESP_LOGE(TAG, "创建左侧容器失败");
        return;
    }
    
    // 设置左侧容器为水平布局，用于放置多个菜品卡片
    lv_obj_set_flex_flow(left_container, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(left_container, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_flex_grow(left_container, 1); // 左侧容器占据剩余空间
    // 设置左侧容器宽度为屏幕宽度的70%，确保有足够空间显示菜品
    lv_obj_set_width(left_container, LV_PCT(70));
    lv_obj_set_height(left_container, LV_SIZE_CONTENT);
    lv_obj_set_style_border_width(left_container, 0, 0);
    lv_obj_set_style_pad_all(left_container, 0, 0);
    // 设置菜品卡片之间的间距
    lv_obj_set_style_pad_gap(left_container, 5, 0);
    
    // 解析JSON格式的菜品信息
    cJSON *root = cJSON_Parse(dishes);
    if (root) {
        ESP_LOGI(TAG, "JSON解析成功");
        cJSON *items = cJSON_GetObjectItem(root, "items");
        if (items && cJSON_IsArray(items)) {
            ESP_LOGI(TAG, "找到items数组，数量=%d", cJSON_GetArraySize(items));
            cJSON *item = NULL;
            cJSON_ArrayForEach(item, items) {
                cJSON *name = cJSON_GetObjectItem(item, "name");
                if (name && cJSON_IsString(name)) {
                    const char *dish_name = name->valuestring;
                    ESP_LOGI(TAG, "处理菜品: %s", dish_name);
                    
                    // 创建菜品卡片 - 自适应宽度
                    lv_obj_t *dish_card = lv_obj_create(left_container);
                    lv_obj_set_size(dish_card, LV_SIZE_CONTENT, 39); // 自适应宽度，固定高度
                    lv_obj_set_style_bg_color(dish_card, lv_color_hex(0xF1F1F1), 0); // 灰色背景 #F1F1F1
                    lv_obj_set_style_radius(dish_card, 5, 0); // 圆角5px
                    lv_obj_set_style_pad_all(dish_card, 8, 0); // 内边距8px
                    lv_obj_set_style_border_width(dish_card, 0, 0); // 无边框
                    
                    // 创建菜品标签 - 根据Figma设计优化
                    lv_obj_t *dish_label = lv_label_create(dish_card);
                    lv_label_set_text(dish_label, dish_name);
                    set_font_style(dish_label, FONT_TYPE_DISHES, FONT_SIZE_LARGE); // 使用font_dishes_26.c字体
                    lv_obj_align(dish_label, LV_ALIGN_CENTER, 0, 0);
                    lv_obj_set_style_text_color(dish_label, lv_color_black(), 0); // 黑色文字
                    lv_obj_clear_flag(dish_label, LV_OBJ_FLAG_SCROLLABLE); // 禁用滚动
                    
                    // 延迟显示以避免刷新问题
                    lv_anim_t a;
                    lv_anim_init(&a);
                    lv_anim_set_var(&a, dish_label);
                    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_clear_flag);
                    lv_anim_set_values(&a, LV_OBJ_FLAG_HIDDEN, 0);
                    lv_anim_set_time(&a, 50);
                    lv_anim_set_delay(&a, 10);
                    lv_anim_start(&a);
                    
                    // 设置卡片之间的间距
                    lv_obj_set_style_margin_all(dish_card, 5, 0); // 所有方向都设置5px的外边距
                }
            }
        }
        cJSON_Delete(root);
    } else {
        // 如果JSON解析失败，回退到原来的字符串解析方式
        char *dishes_copy = strdup(dishes);
        if (dishes_copy) {
            char *token = strtok(dishes_copy, "、");
            (void)0; // 移除未使用的x_offset变量
            
            while (token != NULL) {
                // 创建菜品卡片 - 自适应宽度
                lv_obj_t *dish_card = lv_obj_create(left_container);
                lv_obj_set_size(dish_card, LV_SIZE_CONTENT, 39); // 自适应宽度，固定高度
                lv_obj_set_style_bg_color(dish_card, lv_color_hex(0xF1F1F1), 0);
                lv_obj_set_style_radius(dish_card, 5, 0);
                lv_obj_set_style_pad_all(dish_card, 8, 0); // 增加内边距
                lv_obj_clear_flag(dish_card, LV_OBJ_FLAG_SCROLLABLE); // 禁用卡片滚动
                lv_obj_clear_flag(dish_card, LV_OBJ_FLAG_SCROLLABLE); // 禁用卡片滚动
                
                // 创建带保护的菜品标签
                lv_obj_t *dish_label = lv_label_create(dish_card);
                lv_label_set_text(dish_label, token);
                set_font_style(dish_label, FONT_TYPE_DISHES, FONT_SIZE_LARGE); // 使用font_dishes_26.c字体
                lv_obj_align(dish_label, LV_ALIGN_CENTER, 0, 0);
                lv_obj_set_style_text_color(dish_label, lv_color_black(), 0);
                lv_obj_clear_flag(dish_label, LV_OBJ_FLAG_SCROLLABLE); // 禁用滚动
                
                // 延迟显示以避免刷新问题
                lv_anim_t a;
                lv_anim_init(&a);
                lv_anim_set_var(&a, dish_label);
                lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_clear_flag);
                lv_anim_set_values(&a, LV_OBJ_FLAG_HIDDEN, 0);
                lv_anim_set_time(&a, 50);
                lv_anim_set_delay(&a, 10);
                lv_anim_start(&a);
                
                // 设置卡片之间的间距
                lv_obj_set_style_margin_all(dish_card, 5, 0); // 所有方向都设置5px的外边距
                
                token = strtok(NULL, "、");
            }
            
            free(dishes_copy);
        }
    }
    
    // 保存第一个菜品标签指针到订单信息中
    // 注意：这里需要保存实际的标签对象，而不是容器
    if (lv_obj_get_child_cnt(left_container) > 0) {
        lv_obj_t *first_child = lv_obj_get_child(left_container, 0);
        if (first_child && lv_obj_check_type(first_child, &lv_label_class)) {
            order->dish_label = first_child;
        } else {
            order->dish_label = NULL;
        }
    } else {
        order->dish_label = NULL;
    }

    // 右侧：已出餐按钮 - 根据Figma设计调整
    lv_obj_t *btn_ready = lv_btn_create(row);
    if (!btn_ready) {
        free(order->dishes);
        free(order->order_id);
        free(order);
        lv_obj_del(left_container);
        lv_obj_del(row);
        bsp_display_unlock();
        ESP_LOGE(TAG, "创建按钮失败");
        return;
    }
    
    // 记录按钮指针到订单结构体
    order->ready_btn = btn_ready;

    // 按钮样式优化 - 根据Figma设计
    lv_obj_set_size(btn_ready, 143, 74); // 143x74px按钮
    lv_obj_set_style_bg_color(btn_ready, lv_color_hex(0x08c160), LV_PART_MAIN); // 绿色背景 #08c160
    lv_obj_set_style_radius(btn_ready, 5, 0); // 圆角5px
    lv_obj_set_style_border_width(btn_ready, 0, 0); // 无边框
    lv_obj_set_style_shadow_width(btn_ready, 0, 0); // 移除阴影
    lv_obj_set_style_shadow_spread(btn_ready, 0, 0); // 移除阴影扩散
    lv_obj_clear_flag(btn_ready, LV_OBJ_FLAG_SCROLLABLE);
    
    // 按钮字体引用已通过字体模块处理

    // 按钮文字
    lv_obj_t *btn_label = lv_label_create(btn_ready);
    if (!btn_label) {
        free(order->dishes);
        free(order->order_id);
        free(order);
        lv_obj_del(btn_ready);
        lv_obj_del(left_container);
        lv_obj_del(row);
        bsp_display_unlock();
        ESP_LOGE(TAG, "创建按钮标签失败");
        return;
    }
    
    lv_label_set_text(btn_label, "出餐");
    lv_obj_set_style_text_color(btn_label, lv_color_white(), 0); // 白色文字
    set_font_style(btn_label, FONT_TYPE_DEVICE, FONT_SIZE_LARGE); // 使用font_device_24.c字体
    lv_obj_center(btn_label);

    // 添加点击事件
    lv_obj_add_event_cb(btn_ready, btn_ready_cb, LV_EVENT_CLICKED, NULL);

    // UI构建成功，将订单插入队列
    STAILQ_INSERT_TAIL(&order_list, order, entries);
    
    // 添加调试信息
    order_info_t *tmp;
    int count = 0;
    STAILQ_FOREACH(tmp, &order_list, entries) {
        count++;
    }
    ESP_LOGI(TAG, "订单创建完成，当前订单数=%d", count);
    
    // 简化刷新逻辑
    if (lv_obj_is_valid(orders_container)) {
        lv_obj_invalidate(orders_container);
    }
    
    bsp_display_unlock();
    ESP_LOGI(TAG, "订单行创建流程结束");
    
    // 减少延迟时间
    vTaskDelay(pdMS_TO_TICKS(10));
}

// 创建订单行（动态添加到 UI）
void create_dynamic_order_row(int order_num, const char *dishes) {
    // 默认使用订单号作为ID
    char default_id[32];
    snprintf(default_id, sizeof(default_id), "order_%d", order_num);
    create_dynamic_order_row_with_id(default_id, order_num, dishes);
}

// 根据订单ID删除订单
void remove_order_by_id(const char *order_id) {
    bsp_display_lock(portMAX_DELAY);

    order_info_t *order = find_order_by_id(order_id);
    if (!order) {
        bsp_display_unlock();
        ESP_LOGW(TAG, "订单ID %s 不存在，无法删除", order_id);
        return;
    }

    // 从UI中移除订单行
    if (order->row_widget && lv_obj_is_valid(order->row_widget)) {
        lv_obj_del(order->row_widget);
        order->row_widget = NULL;
    }

    // 从队列中移除并释放内存
    STAILQ_REMOVE(&order_list, order, order_info, entries);
    free(order->order_id);
    free(order->dishes);
    free(order);

    // 如果删除后队列为空，恢复等待标签
    if (STAILQ_EMPTY(&order_list) && orders_container && lv_obj_is_valid(orders_container) && !waiting_label) {
        waiting_label = lv_label_create(orders_container);
        set_font_style(waiting_label, FONT_TYPE_DEVICE, FONT_SIZE_LARGE); // 使用font_device_24.c字体
        lv_label_set_text(waiting_label, "等待订单数据...");
        lv_obj_center(waiting_label);
    }

    bsp_display_unlock();
    ESP_LOGI(TAG, "已删除订单: %s", order_id);
}

// 根据订单ID更新订单信息
void update_order_by_id(const char *order_id, int order_num, const char *dishes) {
    bsp_display_lock(portMAX_DELAY);

    order_info_t *order = find_order_by_id(order_id);
    if (!order) {
        bsp_display_unlock();
        ESP_LOGW(TAG, "订单ID %s 不存在，无法更新", order_id);
        return;
    }

    // 更新订单信息
    order->order_num = order_num;
    
    // 安全更新菜品信息
    if (dishes && dishes[0] != '\0') {
        char *new_dishes = strdup(dishes);
        if (new_dishes) {
            if (order->dishes) {
                free(order->dishes);
            }
            order->dishes = new_dishes;
        }
    }
    
    // 更新UI显示 - 重新创建整个菜品容器
    if (order->row_widget && order->row_widget != NULL && lv_obj_is_valid(order->row_widget)) {
        // 找到左侧容器
        lv_obj_t *left_container = lv_obj_get_child(order->row_widget, 0);
        if (left_container && left_container != NULL && lv_obj_is_valid(left_container)) {
            // 如果菜品信息为空，跳过UI更新以避免空指针
            if (!order->dishes || order->dishes[0] == '\0') {
                ESP_LOGW(TAG, "订单 %s 菜品为空，跳过UI刷新", order_id);
            } else {
                // 删除现有的所有菜品卡片
                lv_obj_clean(left_container);

                // 优先尝试按 JSON 解析（与创建逻辑一致）
                cJSON *root = cJSON_Parse(order->dishes);
                if (root) {
                    cJSON *items = cJSON_GetObjectItem(root, "items");
                    if (items && cJSON_IsArray(items)) {
                        cJSON *item = NULL;
                        cJSON_ArrayForEach(item, items) {
                            cJSON *name = cJSON_GetObjectItem(item, "name");
                            if (name && cJSON_IsString(name)) {
                                const char *dish_name = name->valuestring;

                                // 创建菜品卡片 - 自适应宽度
                                lv_obj_t *dish_card = lv_obj_create(left_container);
                                lv_obj_set_size(dish_card, LV_SIZE_CONTENT, 39);
                                lv_obj_set_style_bg_color(dish_card, lv_color_hex(0xF1F1F1), 0);
                                lv_obj_set_style_radius(dish_card, 5, 0);
                                lv_obj_set_style_pad_all(dish_card, 8, 0);
                                lv_obj_set_style_border_width(dish_card, 0, 0);
                                lv_obj_clear_flag(dish_card, LV_OBJ_FLAG_SCROLLABLE);

                                // 创建菜品标签
                                lv_obj_t *dish_label = lv_label_create(dish_card);
                                lv_label_set_text(dish_label, dish_name);
                                set_font_style(dish_label, FONT_TYPE_DISHES, FONT_SIZE_LARGE);
                                lv_obj_align(dish_label, LV_ALIGN_CENTER, 0, 0);
                                lv_obj_set_style_text_color(dish_label, lv_color_black(), 0);
                                lv_obj_clear_flag(dish_label, LV_OBJ_FLAG_SCROLLABLE);

                                // 设置卡片间距
                                lv_obj_set_style_margin_all(dish_card, 5, 0);
                            }
                        }
                    }
                    cJSON_Delete(root);
                } else {
                    // 非 JSON：按 "、" 分隔解析
                    char *dishes_copy = strdup(order->dishes);
                    if (dishes_copy) {
                        char *token = strtok(dishes_copy, "、");
                        while (token != NULL) {
                            // 创建菜品卡片 - 自适应宽度
                            lv_obj_t *dish_card = lv_obj_create(left_container);
                            lv_obj_set_size(dish_card, LV_SIZE_CONTENT, 39);
                            lv_obj_set_style_bg_color(dish_card, lv_color_hex(0xF1F1F1), 0);
                            lv_obj_set_style_radius(dish_card, 5, 0);
                            lv_obj_set_style_pad_all(dish_card, 8, 0);
                            lv_obj_clear_flag(dish_card, LV_OBJ_FLAG_SCROLLABLE);

                            // 创建菜品标签
                            lv_obj_t *dish_label = lv_label_create(dish_card);
                            lv_label_set_text(dish_label, token);
                            set_font_style(dish_label, FONT_TYPE_DISHES, FONT_SIZE_LARGE);
                            lv_obj_align(dish_label, LV_ALIGN_CENTER, 0, 0);
                            lv_obj_set_style_text_color(dish_label, lv_color_black(), 0);
                            lv_obj_clear_flag(dish_label, LV_OBJ_FLAG_SCROLLABLE);

                            // 设置卡片间距
                            lv_obj_set_style_margin_all(dish_card, 5, 0);

                            token = strtok(NULL, "、");
                        }
                        free(dishes_copy);
                    }
                }

                // 更新保存的菜品标签指针为第一个标签
                if (lv_obj_get_child_cnt(left_container) > 0) {
                    lv_obj_t *first_card = lv_obj_get_child(left_container, 0);
                    if (first_card && lv_obj_is_valid(first_card)) {
                        lv_obj_t *first_label = lv_obj_get_child(first_card, 0);
                        if (first_label && lv_obj_check_type(first_label, &lv_label_class)) {
                            order->dish_label = first_label;
                        }
                    }
                }
            }
        }
    } else {
        ESP_LOGW(TAG, "订单 %s 的行控件无效或已被销毁，跳过UI更新", order_id);
    }

    // 隐藏“出餐”按钮：直接使用保存的按钮指针
    if (order->ready_btn && lv_obj_is_valid(order->ready_btn)) {
        lv_obj_add_flag(order->ready_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_state(order->ready_btn, LV_STATE_DISABLED);
        ESP_LOGI(TAG, "订单 %s 的“出餐”按钮已隐藏", order_id);
    }
    
    bsp_display_unlock();
}