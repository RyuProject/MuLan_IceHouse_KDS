set(LV_DEMO_DIR ../managed_components/lvgl__lvgl/demos)
file(GLOB_RECURSE LV_DEMOS_SOURCES ${LV_DEMO_DIR}/*.c)

idf_component_register(
    SRCS main.c order_ui.c hex_utils.c utf8_validator.c font/fonts.c font/lv_font_mulan_14.c font/lv_font_mulan_24.c font/font_puhui_16_4.c font/font_dishes_26.c font/font_device_24.c ${LV_DEMOS_SOURCES}
    INCLUDE_DIRS . ${LV_DEMO_DIR} font
    REQUIRES 
)

idf_component_get_property(LVGL_LIB lvgl__lvgl COMPONENT_LIB)
target_compile_options(
    ${LVGL_LIB} 
    PRIVATE
        -DLV_LVGL_H_INCLUDE_SIMPLE
        -DLV_USE_DEMO_MUSIC
)
