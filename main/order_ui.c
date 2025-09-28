#include "order_ui.h"
#include "lvgl.h"
#include "esp_log.h"
#include "bsp/display.h"
#include "bsp/esp-bsp.h"
#include "font/fonts.h"
#include <string.h>
#include <stdlib.h>
#include "sys/queue.h"
#include "cJSON.h"

// 外部声明send_notification函数
extern int send_notification(const char *json_str);

static const char *TAG = "OrderUI";
static lv_obj_t *orders_container = NULL;
static lv_obj_t *waiting_label = NULL; // 保存等待标签指针

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
    lv_obj_t *label = lv_obj_get_child(btn, 0);  // 获取按钮内的 label
    if (label) {
        lv_label_set_text(label, "✅ 已出餐");
    }

    // 修改按钮背景色为灰色（针对MAIN部分）
    lv_obj_set_style_bg_color(btn, lv_color_hex(0xCCCCCC), LV_PART_MAIN);

    // 禁用按钮交互
    lv_obj_add_state(btn, LV_STATE_DISABLED);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_CLICKABLE);

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
                break;
            }
        }
    }

    ESP_LOGI(TAG, "✅ 已出餐按钮被点击并已禁用");
    bsp_display_unlock();
}

// 初始化订单UI容器
void order_ui_init(lv_obj_t *parent)
{
    bsp_display_lock(portMAX_DELAY);
    
    orders_container = lv_obj_create(parent);
    lv_obj_set_size(orders_container, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(orders_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(orders_container, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scrollbar_mode(orders_container, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_pad_all(orders_container, 10, 0);

    // 初始显示等待数据
    waiting_label = lv_label_create(orders_container);
    set_font_style(waiting_label, FONT_TYPE_MULAN, FONT_SIZE_SMALL);
    lv_label_set_text(waiting_label, "等待订单数据...");
    lv_obj_center(waiting_label);

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
    set_font_style(popup, FONT_TYPE_MULAN, FONT_SIZE_SMALL);

    // 创建消息标签
    lv_obj_t *label = lv_label_create(popup);
    lv_label_set_text(label, message);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
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
void create_dynamic_order_row_with_id(const char *order_id, int order_num, const char *dishes) {
    bsp_display_lock(portMAX_DELAY);
    
    // 首次添加订单时，清理"等待订单数据..."占位标签
    if (waiting_label && lv_obj_is_valid(waiting_label)) {
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
    // 设置订单行样式 - 带刷新保护
    lv_obj_set_size(row, LV_PCT(100), 96);
    lv_obj_add_flag(row, LV_OBJ_FLAG_EVENT_BUBBLE); // 启用事件冒泡
    lv_obj_add_flag(row, LV_OBJ_FLAG_IGNORE_LAYOUT); // 忽略自动布局
    lv_obj_set_style_transform_zoom(row, 256, 0); // 防止缩放导致的渲染问题
    lv_obj_set_style_transform_angle(row, 0, 0); // 重置旋转角度
    lv_obj_set_style_clip_corner(row, true, 0); // 启用角落裁剪
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

    // 左侧：订单信息（水平排列菜品）
    lv_obj_t *left_container = lv_obj_create(row);
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
    lv_obj_set_flex_flow(left_container, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(left_container, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    // 设置左侧容器宽度为屏幕宽度的70%，确保有足够空间显示菜品
    lv_obj_set_width(left_container, LV_PCT(70));
    lv_obj_set_height(left_container, LV_SIZE_CONTENT);
    lv_obj_set_style_border_width(left_container, 0, 0);
    lv_obj_set_style_pad_all(left_container, 0, 0);
    // 允许容器内容换行显示，当一行放不下时
    lv_obj_set_style_flex_flow(left_container, LV_FLEX_FLOW_ROW_WRAP, 0);
    
    // 解析JSON格式的菜品信息
    cJSON *root = cJSON_Parse(dishes);
    if (root) {
        cJSON *items = cJSON_GetObjectItem(root, "items");
        if (items && cJSON_IsArray(items)) {
            cJSON *item = NULL;
            cJSON_ArrayForEach(item, items) {
                cJSON *name = cJSON_GetObjectItem(item, "name");
                if (name && cJSON_IsString(name)) {
                    const char *dish_name = name->valuestring;
                    
                    // 创建菜品卡片 - 根据Figma设计优化
                    lv_obj_t *dish_card = lv_obj_create(left_container);
                    int text_len = strlen(dish_name);
                    int card_width = LV_MAX(text_len * 20 + 20, 70); // 更精确的宽度计算
                    lv_obj_set_size(dish_card, card_width, 39);
                    lv_obj_set_style_bg_color(dish_card, lv_color_hex(0xF1F1F1), 0); // 灰色背景 #F1F1F1
                    lv_obj_set_style_radius(dish_card, 5, 0); // 圆角5px
                    lv_obj_set_style_pad_all(dish_card, 8, 0); // 内边距8px
                    lv_obj_set_style_border_width(dish_card, 0, 0); // 无边框
                    
                    // 创建菜品标签 - 根据Figma设计优化
                    lv_obj_t *dish_label = lv_label_create(dish_card);
                    lv_label_set_text(dish_label, dish_name);
                    set_font_style(dish_label, FONT_TYPE_MULAN, FONT_SIZE_LARGE); // 24px字体
                    lv_obj_align(dish_label, LV_ALIGN_CENTER, 0, 0);
                    lv_obj_set_style_text_color(dish_label, lv_color_black(), 0); // 黑色文字
                    lv_obj_add_flag(dish_label, LV_OBJ_FLAG_HIDDEN); // 初始隐藏
                    lv_obj_add_flag(dish_label, LV_OBJ_FLAG_FLOATING); // 浮动模式
                    lv_obj_clear_flag(dish_label, LV_OBJ_FLAG_SCROLLABLE); // 禁用滚动
                    lv_obj_set_style_transform_zoom(dish_label, 256, 0);
                    lv_obj_set_style_transform_angle(dish_label, 0, 0);
                    
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
            char *token = strtok(dishes_copy, " ");
            (void)0; // 移除未使用的x_offset变量
            
            while (token != NULL) {
                // 创建菜品卡片 - 优化版
                lv_obj_t *dish_card = lv_obj_create(left_container);
                int text_len = strlen(token);
                int card_width = LV_MAX(text_len * 20 + 20, 70); // 更精确的宽度计算
                lv_obj_set_size(dish_card, card_width, 39);
                lv_obj_set_style_bg_color(dish_card, lv_color_hex(0xF1F1F1), 0);
                lv_obj_set_style_radius(dish_card, 5, 0);
                lv_obj_set_style_pad_all(dish_card, 8, 0); // 增加内边距
                
                // 创建带保护的菜品标签
                lv_obj_t *dish_label = lv_label_create(dish_card);
                lv_label_set_text(dish_label, token);
                set_font_style(dish_label, FONT_TYPE_MULAN, FONT_SIZE_LARGE);
                lv_obj_align(dish_label, LV_ALIGN_CENTER, 0, 0);
                lv_obj_set_style_text_color(dish_label, lv_color_black(), 0);
                lv_obj_add_flag(dish_label, LV_OBJ_FLAG_HIDDEN); // 初始隐藏
                lv_obj_add_flag(dish_label, LV_OBJ_FLAG_FLOATING); // 浮动模式
                lv_obj_clear_flag(dish_label, LV_OBJ_FLAG_SCROLLABLE); // 禁用滚动
                lv_obj_set_style_transform_zoom(dish_label, 256, 0);
                lv_obj_set_style_transform_angle(dish_label, 0, 0);
                
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
                
                token = strtok(NULL, " ");
            }
            
            free(dishes_copy);
        }
    }
    
    // 保存菜品容器指针到订单信息中（而不是单个标签）
    order->dish_label = left_container;

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
    
    // 按钮样式优化 - 根据Figma设计
    lv_obj_set_size(btn_ready, 143, 74); // 143x74px按钮
    lv_obj_align(btn_ready, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(btn_ready, lv_color_hex(0x52E011), LV_PART_MAIN); // 绿色背景 #52E011
    lv_obj_set_style_radius(btn_ready, 5, 0); // 圆角5px
    lv_obj_set_style_border_width(btn_ready, 0, 0); // 无边框
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
    
    lv_label_set_text(btn_label, "已出餐");
    lv_obj_set_style_text_color(btn_label, lv_color_white(), 0); // 白色文字
    set_font_style(btn_label, FONT_TYPE_MULAN, FONT_SIZE_LARGE); // 24px字体
    lv_obj_center(btn_label);

    // 添加点击事件
    lv_obj_add_event_cb(btn_ready, btn_ready_cb, LV_EVENT_CLICKED, NULL);

    // UI构建成功，将订单插入队列
    STAILQ_INSERT_TAIL(&order_list, order, entries);

    bsp_display_unlock();
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
        set_font_style(waiting_label, FONT_TYPE_MULAN, FONT_SIZE_SMALL);
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
    char *new_dishes = strdup(dishes);
    if (new_dishes) {
        free(order->dishes);
        order->dishes = new_dishes;
    }
    
    // 更新UI显示 - 使用保存的菜品标签指针
    if (order->dish_label && lv_obj_is_valid(order->dish_label)) {
        lv_label_set_text_fmt(order->dish_label, "%s", order->dishes);
    }
    
    bsp_display_unlock();
}