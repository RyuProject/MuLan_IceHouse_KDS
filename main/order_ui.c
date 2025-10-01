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

static const char *TAG = "OrderUI-Focus";

// UI对象
static lv_obj_t *main_container = NULL;
static lv_obj_t *current_order_container = NULL;    // 当前订单容器
static lv_obj_t *waiting_orders_container = NULL;  // 等待订单容器
static lv_obj_t *status_bar = NULL;                // 状态栏
static lv_obj_t *bluetooth_label = NULL;
static lv_obj_t *time_label = NULL;
static lv_obj_t *waiting_count_label = NULL;        // 等待订单数量显示

// 订单数据结构
typedef enum {
    ORDER_STATUS_PENDING,      // 等待处理
    ORDER_STATUS_PROCESSING,   // 处理中（当前焦点）
    ORDER_STATUS_COMPLETED     // 已完成
} order_status_t;

typedef struct order_info {
    char *order_id;
    int order_num;
    char *dishes;
    order_status_t status;
    lv_obj_t *ui_widget;       // UI控件
    STAILQ_ENTRY(order_info) entries;
} order_info_t;

// 订单队列
STAILQ_HEAD(order_list_head, order_info);
static struct order_list_head order_list = STAILQ_HEAD_INITIALIZER(order_list);
static order_info_t *current_processing_order = NULL;  // 当前处理的订单

static bool is_bluetooth_connected = false;

// 按钮点击回调 - 完成当前订单
static void btn_complete_cb(lv_event_t *e)
{
    bsp_display_lock(portMAX_DELAY);
    
    if (current_processing_order) {
        // 发送完成通知
        char notify_msg[128];
        snprintf(notify_msg, sizeof(notify_msg), 
                "{\"orderId\":\"%s\",\"status\":true}", 
                current_processing_order->order_id);
        
        send_notification(notify_msg);
        ESP_LOGI(TAG, "订单完成: %s", current_processing_order->order_id);
        
        // 保存订单ID用于后续处理
        char *completed_order_id = strdup(current_processing_order->order_id);
        
        // 标记为已完成并从UI移除
        current_processing_order->status = ORDER_STATUS_COMPLETED;
        if (current_processing_order->ui_widget && lv_obj_is_valid(current_processing_order->ui_widget)) {
            lv_obj_del(current_processing_order->ui_widget);
            current_processing_order->ui_widget = NULL;
        }
        
        // 切换到下一个订单
        complete_current_order(completed_order_id);
        free(completed_order_id);
    }
    
    bsp_display_unlock();
}

// 创建当前订单显示区域
static void create_current_order_display(order_info_t *order)
{
    if (!current_order_container || !order) return;
    
    // 清空当前容器
    lv_obj_clean(current_order_container);
    
    // 创建订单卡片
    lv_obj_t *order_card = lv_obj_create(current_order_container);
    lv_obj_set_size(order_card, LV_PCT(95), 280);  // 大尺寸，突出显示
    lv_obj_set_style_bg_color(order_card, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_color(order_card, lv_color_hex(0x08C160), 0);
    lv_obj_set_style_border_width(order_card, 3, 0);
    lv_obj_set_style_radius(order_card, 10, 0);
    lv_obj_set_style_shadow_width(order_card, 20, 0);
    lv_obj_set_style_shadow_color(order_card, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_opa(order_card, LV_OPA_30, 0);
    lv_obj_center(order_card);
    
    // 订单号标题
    lv_obj_t *title_label = lv_label_create(order_card);
    char title_text[32];
    snprintf(title_text, sizeof(title_text), "订单 #%d", order->order_num);
    lv_label_set_text(title_label, title_text);
    set_font_style(title_label, FONT_TYPE_DEVICE, FONT_SIZE_LARGE);
    lv_obj_set_style_text_color(title_label, lv_color_hex(0x08C160), 0);
    lv_obj_align(title_label, LV_ALIGN_TOP_MID, 0, 15);
    
    // 菜品显示区域 - 优化布局
    lv_obj_t *dishes_container = lv_obj_create(order_card);
    lv_obj_set_size(dishes_container, LV_PCT(90), 180);
    lv_obj_set_flex_flow(dishes_container, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(dishes_container, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_gap(dishes_container, 10, 0);
    lv_obj_set_style_pad_all(dishes_container, 10, 0);
    lv_obj_set_style_border_width(dishes_container, 0, 0); // 移除边框
    lv_obj_set_style_bg_color(dishes_container, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(dishes_container, LV_ALIGN_TOP_MID, 0, 60);
    
    // 解析并显示菜品 - 支持字符串格式（已解码的中文菜品名称）
    ESP_LOGI(TAG, "解析菜品数据: %s", order->dishes);
    
    // 菜品数据已经是解码后的中文字符串，如"陈醋、沙棘、黄花"
    // 按"、"分隔符分割菜品
    char *dishes_copy = strdup(order->dishes);
    if (!dishes_copy) {
        ESP_LOGE(TAG, "内存分配失败");
        return;
    }
    
    char *token = strtok(dishes_copy, "、");
    int displayed_count = 0;
    
    while (token != NULL) {
        ESP_LOGI(TAG, "显示菜品: %s", token);
        
        // 创建菜品卡片 - 借鉴旧版本设计
        lv_obj_t *dish_card = lv_obj_create(dishes_container);
        lv_obj_set_size(dish_card, LV_SIZE_CONTENT, 39); // 自适应宽度，固定高度
        lv_obj_set_style_bg_color(dish_card, lv_color_hex(0xF1F1F1), 0); // 灰色背景 #F1F1F1
        lv_obj_set_style_radius(dish_card, 5, 0); // 圆角5px
        lv_obj_set_style_pad_all(dish_card, 8, 0); // 内边距8px
        lv_obj_set_style_border_width(dish_card, 0, 0); // 无边框
        lv_obj_set_style_margin_all(dish_card, 5, 0); // 设置卡片间距
        
        lv_obj_t *dish_label = lv_label_create(dish_card);
        lv_obj_set_style_text_color(dish_label, lv_color_hex(0x333333), 0);
        lv_label_set_text(dish_label, token);
        set_font_style(dish_label, FONT_TYPE_DISHES, FONT_SIZE_LARGE);
        lv_obj_center(dish_label);
        
        displayed_count++;
        token = strtok(NULL, "、");
    }
    
    free(dishes_copy);
    ESP_LOGI(TAG, "成功显示 %d 个菜品", displayed_count);
    
    // 完成按钮
    lv_obj_t *complete_btn = lv_btn_create(order_card);
    // lv_obj_set_size(complete_btn, 200, 60);
    lv_obj_set_size(complete_btn, LV_PCT(90), 100);
    lv_obj_set_style_bg_color(complete_btn, lv_color_hex(0x08C160), 0);
    lv_obj_set_style_radius(complete_btn, 8, 0);
    lv_obj_align(complete_btn, LV_ALIGN_BOTTOM_MID, 0, -20);
    
    lv_obj_t *btn_label = lv_label_create(complete_btn);
    lv_label_set_text(btn_label, "出餐完成");
    set_font_style(btn_label, FONT_TYPE_DEVICE, FONT_SIZE_LARGE);
    lv_obj_set_style_text_color(btn_label, lv_color_white(), 0);
    lv_obj_center(btn_label);
    
    lv_obj_add_event_cb(complete_btn, btn_complete_cb, LV_EVENT_CLICKED, NULL);
    
    order->ui_widget = order_card;
}

// 更新等待订单显示
static void update_waiting_orders_display(void)
{
    if (!waiting_orders_container) return;
    
    // 清空等待容器
    lv_obj_clean(waiting_orders_container);
    
    // 计算等待订单数量
    int waiting_count = 0;
    order_info_t *order;
    STAILQ_FOREACH(order, &order_list, entries) {
        if (order->status == ORDER_STATUS_PENDING) {
            waiting_count++;
        }
    }
    
    // 更新等待数量显示
    if (waiting_count_label) {
        char count_text[32];
        snprintf(count_text, sizeof(count_text), "等待订单: %d", waiting_count);
        lv_label_set_text(waiting_count_label, count_text);
    }
    
    // 显示前几个等待订单的缩略信息
    int display_count = 0;
    STAILQ_FOREACH(order, &order_list, entries) {
        if (order->status == ORDER_STATUS_PENDING && display_count < MAX_WAITING_ORDERS_DISPLAY) {
            // 创建等待订单项
            lv_obj_t *waiting_item = lv_obj_create(waiting_orders_container);
            lv_obj_set_size(waiting_item, LV_PCT(95), 50);
            lv_obj_set_style_bg_color(waiting_item, lv_color_hex(0xF8F9FA), 0);
            lv_obj_set_style_border_width(waiting_item, 1, 0);
            lv_obj_set_style_border_color(waiting_item, lv_color_hex(0xDDDDDD), 0);
            lv_obj_set_style_radius(waiting_item, 5, 0);
            
            // 订单号显示
            lv_obj_t *order_label = lv_label_create(waiting_item);
            char order_text[20];
            snprintf(order_text, sizeof(order_text), "#%d", order->order_num);
            lv_label_set_text(order_label, order_text);
            set_font_style(order_label, FONT_TYPE_DEVICE, FONT_SIZE_MEDIUM);
            lv_obj_align(order_label, LV_ALIGN_LEFT_MID, 10, 0);
            
            // 状态指示
            lv_obj_t *status_label = lv_label_create(waiting_item);
            lv_label_set_text(status_label, "等待中");
            set_font_style(status_label, FONT_TYPE_DEVICE, FONT_SIZE_SMALL);
            lv_obj_set_style_text_color(status_label, lv_color_hex(0x666666), 0);
            lv_obj_align(status_label, LV_ALIGN_RIGHT_MID, -10, 0);
            
            display_count++;
        }
    }
    
    // 如果没有等待订单，显示提示
    if (waiting_count == 0) {
        lv_obj_t *hint_label = lv_label_create(waiting_orders_container);
        lv_label_set_text(hint_label, "暂无等待订单");
        set_font_style(hint_label, FONT_TYPE_DEVICE, FONT_SIZE_MEDIUM);
        lv_obj_set_style_text_color(hint_label, lv_color_hex(0x999999), 0);
        lv_obj_center(hint_label);
    }
}

// 初始化UI（单订单焦点模式）
void order_ui_init(lv_obj_t *parent)
{
    bsp_display_lock(portMAX_DELAY);
    
    // 创建主容器 - 允许滚动，但状态栏固定在底部
    main_container = lv_obj_create(parent);
    lv_obj_set_size(main_container, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(main_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(main_container, 0, 0);
    lv_obj_set_style_border_width(main_container, 0, 0);
    lv_obj_set_scrollbar_mode(main_container, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_flex_align(main_container, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    
    // 等待订单区域（20%高度）
    waiting_orders_container = lv_obj_create(main_container);
    lv_obj_set_size(waiting_orders_container, LV_PCT(100), LV_PCT(20));
    lv_obj_set_flex_flow(waiting_orders_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(waiting_orders_container, 10, 0);
    lv_obj_set_style_border_width(waiting_orders_container, 0, 0);
    lv_obj_set_style_bg_color(waiting_orders_container, lv_color_white(), 0);
    
    // 等待订单标题
    lv_obj_t *waiting_title = lv_label_create(waiting_orders_container);
    lv_label_set_text(waiting_title, "等待订单");
    set_font_style(waiting_title, FONT_TYPE_DEVICE, FONT_SIZE_MEDIUM);
    lv_obj_set_style_text_color(waiting_title, lv_color_hex(0x333333), 0);
    
    // 当前订单区域（自动填充剩余高度）
    current_order_container = lv_obj_create(main_container);
    lv_obj_set_size(current_order_container, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_border_width(current_order_container, 0, 0);
    lv_obj_set_style_bg_color(current_order_container, lv_color_hex(0xF0F2F5), 0);
    // 设置flex_grow为1，自动填充剩余空间
    lv_obj_set_flex_grow(current_order_container, 1);
    
    // 初始显示提示
    lv_obj_t *hint_label = lv_label_create(current_order_container);
    lv_label_set_text(hint_label, "等待新订单...");
    set_font_style(hint_label, FONT_TYPE_DEVICE, FONT_SIZE_LARGE);
    lv_obj_set_style_text_color(hint_label, lv_color_hex(0x999999), 0);
    lv_obj_center(hint_label);
    
    // 状态栏（固定高度）- 绝对固定在底部
    status_bar = lv_obj_create(main_container);
    lv_obj_set_size(status_bar, LV_PCT(100), 40);
    lv_obj_set_style_bg_color(status_bar, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_style_border_width(status_bar, 0, 0);
    lv_obj_set_flex_flow(status_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(status_bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    // 设置状态栏不可滚动且固定在底部
    lv_obj_set_scrollbar_mode(status_bar, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(status_bar, LV_OBJ_FLAG_SCROLLABLE);
    // 确保状态栏固定在底部：设置flex_grow为0，避免被拉伸
    lv_obj_set_flex_grow(status_bar, 0);
    lv_obj_set_style_align(status_bar, LV_ALIGN_BOTTOM_MID, 0);
    
    // 左侧信息
    lv_obj_t *left_container = lv_obj_create(status_bar);
    lv_obj_set_size(left_container, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(left_container, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_bg_opa(left_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(left_container, 0, 0);
    
    // 版本信息
    lv_obj_t *version_label = lv_label_create(left_container);
    lv_label_set_text(version_label, "MuLanKDS Focus");
    lv_obj_set_style_text_font(version_label, &lv_font_montserrat_14, 0);
    
    // 等待订单数量
    waiting_count_label = lv_label_create(left_container);
    lv_label_set_text(waiting_count_label, "等待订单: 0");
    lv_obj_set_style_text_font(waiting_count_label, &font_puhui_16_4, 0);
    lv_obj_set_style_margin_left(waiting_count_label, 20, 0);
    
    // 蓝牙状态
    bluetooth_label = lv_label_create(left_container);
    lv_label_set_text(bluetooth_label, LV_SYMBOL_BLUETOOTH "Ready");
    lv_obj_set_style_text_font(bluetooth_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(bluetooth_label, lv_color_hex(0xfa5050), 0);
    lv_obj_set_style_margin_left(bluetooth_label, 20, 0);
    
    // 右侧时间
    lv_obj_t *right_container = lv_obj_create(status_bar);
    lv_obj_set_size(right_container, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    
    time_label = lv_label_create(right_container);
    lv_label_set_text(time_label, "00:00");
    lv_obj_set_style_text_font(time_label, &lv_font_montserrat_14, 0);
    
    bsp_display_unlock();
}

// 添加新订单
void add_new_order(const char *order_id, int order_num, const char *dishes)
{
    if (!order_id || !dishes) return;
    
    bsp_display_lock(portMAX_DELAY);
    
    // 创建新订单
    order_info_t *new_order = malloc(sizeof(order_info_t));
    if (!new_order) {
        bsp_display_unlock();
        return;
    }
    
    new_order->order_id = strdup(order_id);
    new_order->order_num = order_num;
    new_order->dishes = strdup(dishes);
    new_order->status = ORDER_STATUS_PENDING;
    new_order->ui_widget = NULL;
    
    if (!new_order->order_id || !new_order->dishes) {
        free(new_order->order_id);
        free(new_order->dishes);
        free(new_order);
        bsp_display_unlock();
        return;
    }
    
    // 添加到队列
    STAILQ_INSERT_TAIL(&order_list, new_order, entries);
    
    // 如果没有当前处理的订单，立即显示这个订单
    if (!current_processing_order) {
        new_order->status = ORDER_STATUS_PROCESSING;
        current_processing_order = new_order;
        create_current_order_display(new_order);
        show_popup_message("新订单开始处理", 2000);
    }
    
    // 更新等待订单显示
    update_waiting_orders_display();
    
    bsp_display_unlock();
    ESP_LOGI(TAG, "新订单添加: %s", order_id);
}

// 完成当前订单并显示下一个
void complete_current_order(const char *order_id)
{
    if (!order_id || strlen(order_id) == 0) {
        ESP_LOGE(TAG, "complete_current_order: 无效的order_id");
        return;
    }
    
    bsp_display_lock(portMAX_DELAY);
    
    ESP_LOGI(TAG, "开始完成订单: %s", order_id);
    
    // 首先设置订单状态为完成
    order_info_t *order;
    bool found_order = false;
    STAILQ_FOREACH(order, &order_list, entries) {
        if (order && order->order_id && strcmp(order->order_id, order_id) == 0) {
            ESP_LOGI(TAG, "找到订单 %s，设置状态为COMPLETED", order_id);
            order->status = ORDER_STATUS_COMPLETED;
            found_order = true;
            break;
        }
    }
    
    if (!found_order) {
        ESP_LOGW(TAG, "未找到订单: %s", order_id);
        bsp_display_unlock();
        return;
    }
    
    // 查找并移除已完成的订单
    order_info_t *tmp;
    bool removed = false;
    STAILQ_FOREACH_SAFE(order, &order_list, entries, tmp) {
        if (order && order->order_id && strcmp(order->order_id, order_id) == 0 && order->status == ORDER_STATUS_COMPLETED) {
            ESP_LOGI(TAG, "移除已完成订单: %s", order_id);
            // 从队列中移除已完成订单
            STAILQ_REMOVE(&order_list, order, order_info, entries);
            if (order->order_id) free(order->order_id);
            if (order->dishes) free(order->dishes);
            free(order);
            removed = true;
            break;
        }
    }
    
    if (!removed) {
        ESP_LOGW(TAG, "未能移除订单: %s", order_id);
    }
    
    // 如果当前处理的订单是完成的订单，清空当前订单
    if (current_processing_order && current_processing_order->order_id && strcmp(current_processing_order->order_id, order_id) == 0) {
        current_processing_order = NULL;
    }
    
    // 查找下一个等待订单
    order_info_t *next_order = NULL;
    bool found_next = false;
    STAILQ_FOREACH(next_order, &order_list, entries) {
        if (next_order->status == ORDER_STATUS_PENDING) {
            next_order->status = ORDER_STATUS_PROCESSING;
            current_processing_order = next_order;
            create_current_order_display(next_order);
            show_popup_message("开始处理下一个订单", 2000);
            ESP_LOGI(TAG, "切换到下一个订单: %s", next_order->order_id);
            found_next = true;
            break;
        }
    }
    
    // 如果没有更多订单，显示等待提示
    if (!found_next) {
        ESP_LOGI(TAG, "没有更多订单，显示等待提示");
        current_processing_order = NULL;
        lv_obj_clean(current_order_container);
        lv_obj_t *hint_label = lv_label_create(current_order_container);
        lv_label_set_text(hint_label, "等待新订单...");
        set_font_style(hint_label, FONT_TYPE_DEVICE, FONT_SIZE_LARGE);
        lv_obj_set_style_text_color(hint_label, lv_color_hex(0x999999), 0);
        lv_obj_center(hint_label);
    }
    
    // 更新等待订单显示
    update_waiting_orders_display();
    
    bsp_display_unlock();
}

// 获取当前订单ID
const char* get_current_order_id(void)
{
    return current_processing_order ? current_processing_order->order_id : NULL;
}

// 获取等待订单数量
int get_waiting_orders_count(void)
{
    int count = 0;
    order_info_t *order;
    
    bsp_display_lock(portMAX_DELAY);
    STAILQ_FOREACH(order, &order_list, entries) {
        if (order->status == ORDER_STATUS_PENDING) {
            count++;
        }
    }
    bsp_display_unlock();
    
    return count;
}

// 向后兼容的函数
void create_dynamic_order_row(int order_num, const char *dishes)
{
    char default_id[32];
    snprintf(default_id, sizeof(default_id), "order_%d", order_num);
    add_new_order(default_id, order_num, dishes);
}

void create_dynamic_order_row_with_id(const char *order_id, int order_num, const char *dishes)
{
    add_new_order(order_id, order_num, dishes);
}

void remove_order_by_id(const char *order_id)
{
    // 在单订单焦点模式下，移除订单需要特殊处理
    bsp_display_lock(portMAX_DELAY);
    
    order_info_t *order, *tmp;
    STAILQ_FOREACH_SAFE(order, &order_list, entries, tmp) {
        if (strcmp(order->order_id, order_id) == 0) {
            if (order == current_processing_order) {
                // 如果是当前订单，完成它
                complete_current_order(order_id);
            } else {
                // 如果是等待订单，直接移除
                STAILQ_REMOVE(&order_list, order, order_info, entries);
                if (order->ui_widget && lv_obj_is_valid(order->ui_widget)) {
                    lv_obj_del(order->ui_widget);
                }
                free(order->order_id);
                free(order->dishes);
                free(order);
                update_waiting_orders_display();
            }
            break;
        }
    }
    
    bsp_display_unlock();
}

void update_order_by_id(const char *order_id, int order_num, const char *dishes)
{
    bsp_display_lock(portMAX_DELAY);
    
    order_info_t *order;
    STAILQ_FOREACH(order, &order_list, entries) {
        if (strcmp(order->order_id, order_id) == 0) {
            order->order_num = order_num;
            free(order->dishes);
            order->dishes = strdup(dishes);
            
            // 如果是当前订单，更新显示
            if (order == current_processing_order) {
                create_current_order_display(order);
            }
            break;
        }
    }
    
    bsp_display_unlock();
}

// 其他现有函数保持不变
void update_time_display(long long timestamp) {
    if (!time_label) return;
    
    time_t ts = (time_t)(timestamp / 1000);
    struct tm *timeinfo = localtime(&ts);
    
    if (timeinfo) {
        char time_str[10];
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
    }
}

void update_bluetooth_status(bool connected) {
    bsp_display_lock(portMAX_DELAY);
    
    is_bluetooth_connected = connected;
    
    if (bluetooth_label) {
        if (connected) {
            lv_label_set_text(bluetooth_label, LV_SYMBOL_BLUETOOTH "OK");
            lv_obj_set_style_text_color(bluetooth_label, lv_color_hex(0x06C260), 0);
        } else {
            lv_label_set_text(bluetooth_label, LV_SYMBOL_BLUETOOTH "Ready");
            lv_obj_set_style_text_color(bluetooth_label, lv_color_hex(0xfa5051), 0);
        }
    }
    
    bsp_display_unlock();
}

// 弹窗功能保持不变
static void popup_timer_cb(lv_timer_t *timer) {
    lv_obj_t *popup = (lv_obj_t *)lv_timer_get_user_data(timer);
    if (popup && lv_obj_is_valid(popup)) {
        bsp_display_lock(portMAX_DELAY);
        lv_obj_del(popup);
        bsp_display_unlock();
    }
    lv_timer_del(timer);
}

void show_popup_message(const char *message, uint32_t duration_ms) {
    bsp_display_lock(portMAX_DELAY);
    
    lv_obj_t *popup = lv_obj_create(lv_scr_act());
    if (!popup) {
        bsp_display_unlock();
        return;
    }

    lv_obj_set_size(popup, 280, 80);
    lv_obj_center(popup);
    lv_obj_set_style_bg_color(popup, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(popup, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(popup, 0, 0);
    
    lv_obj_t *label = lv_label_create(popup);
    lv_label_set_text(label, message);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_font(label, &font_puhui_16_4, 0);
    lv_obj_center(label);

    lv_timer_t *timer = lv_timer_create(popup_timer_cb, duration_ms, popup);
    if (timer) {
        lv_timer_set_repeat_count(timer, 1);
    }
    
    bsp_display_unlock();
}

