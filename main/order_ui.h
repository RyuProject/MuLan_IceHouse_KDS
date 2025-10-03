#ifndef ORDER_UI_H
#define ORDER_UI_H

#include "lvgl.h"

// 订单焦点模式配置
#define MAX_WAITING_ORDERS_DISPLAY 5  // 最大显示等待订单数量

// 初始化订单UI容器（单订单焦点模式）
void order_ui_init(lv_obj_t *parent);

/**
 * @brief 添加新订单到系统（单订单焦点模式）
 * 
 * @param order_id 订单ID
 * @param order_num 订单号
 * @param dishes 菜品信息
 */
void add_new_order(const char *order_id, int order_num, const char *dishes);

/**
 * @brief 完成当前订单并显示下一个
 * 
 * @param order_id 完成的订单ID
 */
void complete_current_order(const char *order_id);

/**
 * @brief 获取当前处理中的订单ID
 * 
 * @return const char* 当前订单ID，NULL表示无订单
 */
const char* get_current_order_id(void);

/**
 * @brief 获取等待订单数量
 * 
 * @return int 等待处理的订单数量
 */
int get_waiting_orders_count(void);

/**
 * @brief 显示弹出消息
 * 
 * @param message 要显示的消息内容
 * @param duration_ms 显示持续时间(毫秒)
 */
void show_popup_message(const char *message, uint32_t duration_ms);

/**
 * @brief 发送蓝牙通知
 * 
 * @param json_str JSON格式的字符串
 * @return int 成功返回0，失败返回-1
 */
int send_notification(const char *json_str);

/**
 * @brief 更新蓝牙连接状态显示
 * 
 * @param connected 蓝牙连接状态，true表示已连接，false表示未连接
 */
void update_bluetooth_status(bool connected);

// 向后兼容的接口（逐步淘汰）
void create_dynamic_order_row(int order_num, const char *dishes);
void create_dynamic_order_row_with_id(const char *order_id, int order_num, const char *dishes);
void remove_order_by_id(const char *order_id);
void update_order_by_id(const char *order_id, int order_num, const char *dishes);

/**
 * @brief 清空所有订单并重置系统状态
 */
void clear_all_orders(void);

#endif // ORDER_UI_H