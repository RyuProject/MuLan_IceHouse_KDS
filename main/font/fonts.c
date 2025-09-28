#include "fonts.h"

const lv_font_t* get_font(font_type_t type, font_size_t size)
{
    switch (type) {
        case FONT_TYPE_MULAN:
            switch (size) {
                case FONT_SIZE_SMALL:
                    return FONT_MULAN_14;
                case FONT_SIZE_LARGE:
                    return FONT_MULAN_24;
                default:
                    return FONT_MULAN_14; // 默认返回小号木兰字体
            }
        case FONT_TYPE_PUHUI:
            if (size == FONT_SIZE_MEDIUM) {
                return FONT_PUHUI_16;
            }
            return FONT_MULAN_14; // 默认返回木兰字体
        case FONT_TYPE_DISHES:
            return FONT_DISHES_26; // 使用font_dishes_26.c字体
        case FONT_TYPE_DEVICE:
            return FONT_DEVICE_24; // 使用font_device_24.c字体
        default:
            return FONT_MULAN_14; // 默认返回木兰字体
    }
}

void set_font_style(lv_obj_t *obj, font_type_t type, font_size_t size)
{
    const lv_font_t *font = get_font(type, size);
    if (font) {
        lv_obj_set_style_text_font(obj, font, LV_PART_MAIN);
    }
}