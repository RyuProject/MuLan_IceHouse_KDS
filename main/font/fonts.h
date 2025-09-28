#ifndef FONTS_H
#define FONTS_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// 字体声明
extern const lv_font_t lv_font_mulan_14;
extern const lv_font_t lv_font_mulan_24;
extern const lv_font_t font_puhui_16_4;

// 字体定义宏
#define FONT_MULAN_14 &lv_font_mulan_14
#define FONT_MULAN_24 &lv_font_mulan_24
#define FONT_PUHUI_16 &font_puhui_16_4

// 字体大小枚举
typedef enum {
    FONT_SIZE_SMALL = 14,    // 小号字体
    FONT_SIZE_MEDIUM = 16,   // 中号字体  
    FONT_SIZE_LARGE = 24     // 大号字体
} font_size_t;

// 字体类型枚举
typedef enum {
    FONT_TYPE_MULAN,         // 木兰字体
    FONT_TYPE_PUHUI          // 普惠字体
} font_type_t;

/**
 * @brief 获取字体指针
 * @param type 字体类型
 * @param size 字体大小
 * @return lv_font_t* 字体指针，如果找不到返回默认字体
 */
lv_font_t* get_font(font_type_t type, font_size_t size);

/**
 * @brief 设置对象的字体样式
 * @param obj LVGL对象
 * @param type 字体类型
 * @param size 字体大小
 */
void set_font_style(lv_obj_t *obj, font_type_t type, font_size_t size);

#ifdef __cplusplus
}
#endif

#endif /* FONTS_H */