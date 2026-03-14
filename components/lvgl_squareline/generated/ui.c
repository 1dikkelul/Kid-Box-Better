#include "ui.h"

void ui_init(void)
{
    // Get the active screen
    lv_obj_t * screen = lv_screen_active();
    
    // Set background to Blue
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x0000FF), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, 255, LV_PART_MAIN);

    // Create a label with "Test"
    lv_obj_t * label = lv_label_create(screen);
    lv_label_set_text(label, "Test");
    
    // Style the label (White text, centered)
    lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
}
