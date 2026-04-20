#include "lvgl.h"
#include "adc_driver.h"

#define CONSTRAIN(x, low, high) ((x) < (low) ? (low) : ((x) > (high) ? (high) : (x)))

LV_IMG_DECLARE(img_watermark);

// 1. 全局变量
lv_obj_t * slider_zoom_x;
lv_obj_t * my_chart;
lv_chart_series_t * ser1;
lv_chart_series_t * ser2;
lv_obj_t * page;
lv_timer_t * chart_timer;

bool is_paused = true;
bool ch1_visible = true;
bool ch2_visible = true;
lv_obj_t * btn_ch_select;

// 双通道独立缩放与偏移（10倍放大）
int32_t ch1_scale = 120;
int32_t ch1_offset = 0;
int32_t ch2_scale = 120;
int32_t ch2_offset = 0;
uint8_t current_ctrl_ch = 1;
lv_obj_t * btn_ctrl_select;
lv_obj_t * slider_v_scale;
lv_obj_t * slider_v_offset;
lv_obj_t * btn_auto;
lv_obj_t * label_meas_ch1;
lv_obj_t * label_meas_ch2;

// ADC 时基配置
#define ADC_SAMPLE_RATE 100000  // 100 kSPS
#define CHART_POINT_COUNT 200
uint32_t time_base_us_per_div = 1000;  // 1ms/div

// ==========================================
// 🌟 新增：图表绘图事件回调函数
// ==========================================
/**
 * @brief 这个函数会在图表绘制的每一步被调用。
 * 我们在这里拦截绘制过程，专门把 ser_trigger 改成虚线。
 */
void chart_draw_event_cb(lv_event_t * e) {
    lv_obj_draw_part_dsc_t * dsc = lv_event_get_draw_part_dsc(e);

    if(dsc->part == LV_PART_ITEMS) {
    }

    if(dsc->part == LV_PART_TICKS) {
        if(dsc->id == LV_CHART_AXIS_PRIMARY_Y || dsc->id == LV_CHART_AXIS_SECONDARY_Y) {
            if(dsc->text != NULL) {
                int32_t ip = dsc->value / 10;
                int32_t dp = dsc->value % 10;
                if(dp < 0) dp = -dp;
                if(dsc->value < 0 && ip == 0)
                    lv_snprintf(dsc->text, dsc->text_length, "-%d.%d", ip, dp);
                else
                    lv_snprintf(dsc->text, dsc->text_length, "%d.%d", ip, dp);
            }
        }
    }
}

void chart_touch_event_cb(lv_event_t * e) {
    static lv_point_t touch_last;
    lv_indev_t * indev = lv_indev_get_act();
    lv_point_t p;
    lv_indev_get_point(indev, &p);

    lv_event_code_t code = lv_event_get_code(e);
    if(code == LV_EVENT_PRESSED) {
        touch_last = p;
    } else if(code == LV_EVENT_PRESSING) {
        int16_t dy = touch_last.y - p.y;
        if(abs(dy) > 1) {
            lv_obj_invalidate(my_chart);
            touch_last = p;
        }
    }
}

void update_chart_from_adc_dma(uint16_t *dma_buffer, uint32_t buffer_size) {
    uint32_t screen_time_us = time_base_us_per_div * 10;
    uint32_t adc_points_in_screen = (screen_time_us * ADC_SAMPLE_RATE) / 1000000;
    uint32_t step = adc_points_in_screen / CHART_POINT_COUNT;

    if(step == 0) step = 1;

    lv_coord_t * y_array1 = lv_chart_get_y_array(my_chart, ser1);
    lv_coord_t * y_array2 = lv_chart_get_y_array(my_chart, ser2);

    for(int i = 0; i < CHART_POINT_COUNT; i++) {
        uint32_t start_idx = i * step;
        if(start_idx >= buffer_size) break;

        uint32_t end_idx = start_idx + step;
        if(end_idx > buffer_size) end_idx = buffer_size;

        uint16_t max_val = dma_buffer[start_idx];
        uint16_t min_val = dma_buffer[start_idx];

        for(uint32_t j = start_idx; j < end_idx; j++) {
            if(dma_buffer[j] > max_val) max_val = dma_buffer[j];
            if(dma_buffer[j] < min_val) min_val = dma_buffer[j];
        }

        int32_t peak_val = ((max_val + min_val) / 2 - 2048) * 10 / 40;
        y_array1[i] = peak_val;
        y_array2[i] = peak_val;
    }

    lv_chart_refresh(my_chart);
}

void update_chart_y_range(void) {
    int32_t ch1_min = -ch1_scale + ch1_offset;
    int32_t ch1_max = ch1_scale + ch1_offset;
    int32_t ch2_min = -ch2_scale + ch2_offset;
    int32_t ch2_max = ch2_scale + ch2_offset;

    lv_chart_set_range(my_chart, LV_CHART_AXIS_PRIMARY_Y, ch1_min, ch1_max);
    lv_chart_set_range(my_chart, LV_CHART_AXIS_SECONDARY_Y, ch2_min, ch2_max);
    lv_chart_refresh(my_chart);
}

void slider_v_scale_event_cb(lv_event_t * e) {
    int32_t value = lv_slider_get_value(slider_v_scale);
    if(current_ctrl_ch == 1) {
        ch1_scale = value;
    } else {
        ch2_scale = value;
    }
    update_chart_y_range();
}

void slider_v_offset_event_cb(lv_event_t * e) {
    int32_t value = lv_slider_get_value(slider_v_offset);
    if(current_ctrl_ch == 1) {
        ch1_offset = value;
    } else {
        ch2_offset = value;
    }
    update_chart_y_range();
}

void btn_ctrl_select_event_cb(lv_event_t * e) {
    current_ctrl_ch = (current_ctrl_ch == 1) ? 2 : 1;
    if(current_ctrl_ch == 1) {
        lv_label_set_text(lv_obj_get_child(btn_ctrl_select, 0), "Ctrl:CH1");
        lv_slider_set_value(slider_v_scale, ch1_scale, LV_ANIM_OFF);
        lv_slider_set_value(slider_v_offset, ch1_offset, LV_ANIM_OFF);
    } else {
        lv_label_set_text(lv_obj_get_child(btn_ctrl_select, 0), "Ctrl:CH2");
        lv_slider_set_value(slider_v_scale, ch2_scale, LV_ANIM_OFF);
        lv_slider_set_value(slider_v_offset, ch2_offset, LV_ANIM_OFF);
    }
}

static lv_obj_t * btn_trig;

void btn_trig_cb(lv_event_t * e) {
    static adc_trigger_mode_t mode = ADC_TRIG_RISING;
    mode = (adc_trigger_mode_t)((mode + 1) % 3);
    adc_driver_set_trigger(mode);
    lv_obj_t * label = lv_obj_get_child(btn_trig, 0);
    if(mode == ADC_TRIG_NONE)    lv_label_set_text(label, "Trig:Off");
    else if(mode == ADC_TRIG_RISING)  lv_label_set_text(label, "Trig:Rise");
    else                         lv_label_set_text(label, "Trig:Fall");
}

void btn_ch_select_cb(lv_event_t * e) {
    lv_obj_t * label = lv_obj_get_child(btn_ch_select, 0);
    if(ch1_visible && ch2_visible) {
        ch1_visible = true; ch2_visible = false;
        lv_label_set_text(label, "CH1");
        lv_chart_hide_series(my_chart, ser1, false);
        lv_chart_hide_series(my_chart, ser2, true);
    } else if(ch1_visible && !ch2_visible) {
        ch1_visible = false; ch2_visible = true;
        lv_label_set_text(label, "CH2");
        lv_chart_hide_series(my_chart, ser1, true);
        lv_chart_hide_series(my_chart, ser2, false);
    } else {
        ch1_visible = true; ch2_visible = true;
        lv_label_set_text(label, "Both");
        lv_chart_hide_series(my_chart, ser1, false);
        lv_chart_hide_series(my_chart, ser2, false);
    }
}

void btn_auto_event_cb(lv_event_t * e) {
    lv_coord_t * y_array1 = lv_chart_get_y_array(my_chart, ser1);
    lv_coord_t * y_array2 = lv_chart_get_y_array(my_chart, ser2);

    int32_t max_val1 = -32767, min_val1 = 32767;
    int32_t max_val2 = -32767, min_val2 = 32767;

    for(int i = 0; i < 100; i++) {
        if(y_array1[i] != LV_CHART_POINT_NONE) {
            if(y_array1[i] > max_val1) max_val1 = y_array1[i];
            if(y_array1[i] < min_val1) min_val1 = y_array1[i];
        }
        if(y_array2[i] != LV_CHART_POINT_NONE) {
            if(y_array2[i] > max_val2) max_val2 = y_array2[i];
            if(y_array2[i] < min_val2) min_val2 = y_array2[i];
        }
    }

    int32_t center1 = (max_val1 + min_val1) / 2;
    int32_t amplitude1 = (max_val1 - min_val1) / 2;
    ch1_offset = center1;
    ch1_scale = (amplitude1 < 20) ? 340 : amplitude1 * 3 / 2;

    int32_t center2 = (max_val2 + min_val2) / 2;
    int32_t amplitude2 = (max_val2 - min_val2) / 2;
    ch2_offset = center2;
    ch2_scale = (amplitude2 < 20) ? 340 : amplitude2 * 3 / 2;

    lv_chart_set_zoom_x(my_chart, 256);
    lv_slider_set_value(slider_zoom_x, 256, LV_ANIM_OFF);

    current_ctrl_ch = 1;
    lv_label_set_text(lv_obj_get_child(btn_ctrl_select, 0), "Ctrl:CH1");
    lv_slider_set_value(slider_v_scale, ch1_scale, LV_ANIM_OFF);
    lv_slider_set_value(slider_v_offset, ch1_offset, LV_ANIM_OFF);

    update_chart_y_range();
    lv_chart_refresh(my_chart);
}

void slider_zoom_x_event_cb(lv_event_t * e) {
    lv_obj_t * slider = lv_event_get_target(e);
    int32_t zoom_val = lv_slider_get_value(slider);
    lv_chart_set_zoom_x(my_chart, (uint16_t)zoom_val);
}

void update_task_cb(lv_timer_t * timer) {
    static int32_t adc_ch1[ADC_SAMPLE_COUNT];
    static int32_t adc_ch2[ADC_SAMPLE_COUNT];

    if (!adc_driver_is_ready()) return;
    adc_driver_get_data(adc_ch1, adc_ch2, ADC_SAMPLE_COUNT);
    adc_driver_start();

    lv_coord_t * y1 = lv_chart_get_y_array(my_chart, ser1);
    lv_coord_t * y2 = lv_chart_get_y_array(my_chart, ser2);

    for(int i = 1; i < ADC_SAMPLE_COUNT - 1; i++) {
        int32_t a = adc_ch2[i-1], b = adc_ch2[i], c = adc_ch2[i+1];
        if (a > b) { int32_t t = a; a = b; b = t; }
        if (b > c) { int32_t t = b; b = c; c = t; }
        if (a > b) { int32_t t = a; a = b; b = t; }
        y2[i] = b;
    }
    y2[0] = adc_ch2[0];
    y2[ADC_SAMPLE_COUNT-1] = adc_ch2[ADC_SAMPLE_COUNT-1];
    ser1->start_point = 0;
    ser2->start_point = 0;
    lv_chart_refresh(my_chart);

    // 测量计算（每10帧更新一次）
    static int meas_cnt = 0;
    if(++meas_cnt >= 10) {
        meas_cnt = 0;
        lv_coord_t * y1 = lv_chart_get_y_array(my_chart, ser1);
        lv_coord_t * y2 = lv_chart_get_y_array(my_chart, ser2);

        // CH1 测量
        int32_t min1 = 32767, max1 = -32767;
        for(int i = 0; i < ADC_SAMPLE_COUNT; i++) {
            if(y1[i] < min1) min1 = y1[i];
            if(y1[i] > max1) max1 = y1[i];
        }
        int32_t vpp1 = max1 - min1;
        int32_t mid1 = (max1 + min1) / 2;
        int cross1 = 0;
        for(int i = 1; i < ADC_SAMPLE_COUNT; i++) {
            if(y1[i-1] < mid1 && y1[i] >= mid1) cross1++;
        }
        char buf[48];
        int32_t vpp1_i = vpp1 / 10, vpp1_d = vpp1 % 10;
        if(cross1 >= 2) {
            int32_t freq1 = 1000 / (ADC_SAMPLE_COUNT / cross1);
            lv_snprintf(buf, sizeof(buf), "1 Vpp:%d.%dV F:%dHz", vpp1_i, vpp1_d, freq1);
        } else {
            lv_snprintf(buf, sizeof(buf), "1 Vpp:%d.%dV F:---", vpp1_i, vpp1_d);
        }
        lv_label_set_text(label_meas_ch1, buf);

        // CH2 测量
        int32_t min2 = 32767, max2 = -32767;
        for(int i = 0; i < ADC_SAMPLE_COUNT; i++) {
            if(y2[i] < min2) min2 = y2[i];
            if(y2[i] > max2) max2 = y2[i];
        }
        int32_t vpp2 = max2 - min2;
        int32_t mid2 = (max2 + min2) / 2;
        int cross2 = 0;
        for(int i = 1; i < ADC_SAMPLE_COUNT; i++) {
            if(y2[i-1] < mid2 && y2[i] >= mid2) cross2++;
        }
        int32_t vpp2_i = vpp2 / 10, vpp2_d = vpp2 % 10;
        if(cross2 >= 2) {
            int32_t freq2 = 1000 / (ADC_SAMPLE_COUNT / cross2);
            lv_snprintf(buf, sizeof(buf), "2 Vpp:%d.%dV F:%dHz", vpp2_i, vpp2_d, freq2);
        } else {
            lv_snprintf(buf, sizeof(buf), "2 Vpp:%d.%dV F:---", vpp2_i, vpp2_d);
        }
        lv_label_set_text(label_meas_ch2, buf);
    }
}

void start_update_task(void) {
    chart_timer = lv_timer_create(update_task_cb, 100, NULL);
}

void btn_pause_event_cb(lv_event_t * e) {
    lv_obj_t * btn = lv_event_get_target(e);
    lv_obj_t * label = lv_obj_get_child(btn, 0);
    is_paused = !is_paused;
    if(is_paused){
        lv_timer_resume(chart_timer);       
        lv_label_set_text(label, "Pause");  
        lv_obj_set_style_bg_color(btn, lv_palette_main(LV_PALETTE_RED), 0); 
    } else {
        lv_timer_pause(chart_timer);        
        lv_label_set_text(label, "Play");   
        lv_obj_set_style_bg_color(btn, lv_palette_main(LV_PALETTE_GREEN), 0); 
    }
}

void create_my_screen(void) {
    page = lv_obj_create(NULL);
    lv_scr_load(page);

    lv_obj_t * wp = lv_img_create(page);
    lv_img_set_src(wp, &img_watermark);
    lv_obj_center(wp);
    lv_obj_set_style_img_opa(wp, LV_OPA_30, 0);
}

void create_my_chart(void) {
    my_chart = lv_chart_create(page);
    lv_chart_set_type(my_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(my_chart, ADC_SAMPLE_COUNT);
    lv_chart_set_div_line_count(my_chart, 20, 20);
    lv_obj_set_size(my_chart, 200, 160);
    lv_obj_align(my_chart, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_chart_set_range(my_chart, LV_CHART_AXIS_PRIMARY_Y, -120, 120);
    lv_chart_set_range(my_chart, LV_CHART_AXIS_SECONDARY_Y, -120, 120);
    lv_chart_set_range(my_chart, LV_CHART_AXIS_PRIMARY_X, 0, 100);

    lv_obj_set_style_size(my_chart, 0, LV_PART_INDICATOR);

    lv_chart_set_axis_tick(my_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 0, 5, 5, false, 0);
    lv_chart_set_axis_tick(my_chart, LV_CHART_AXIS_SECONDARY_Y, 0, 0, 5, 5, false, 0);
    lv_chart_set_axis_tick(my_chart, LV_CHART_AXIS_PRIMARY_X, 0, 0, 5, 5, false, 0);

    lv_obj_clear_flag(my_chart, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(my_chart, LV_OPA_TRANSP, 0);

    ser1 = lv_chart_add_series(my_chart, lv_palette_main(LV_PALETTE_RED), LV_CHART_AXIS_PRIMARY_Y);
    ser2 = lv_chart_add_series(my_chart, lv_palette_main(LV_PALETTE_BLUE), LV_CHART_AXIS_SECONDARY_Y);

    lv_obj_add_event_cb(my_chart, chart_draw_event_cb, LV_EVENT_DRAW_PART_BEGIN, NULL);
    lv_obj_add_event_cb(my_chart, chart_touch_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(my_chart, chart_touch_event_cb, LV_EVENT_PRESSING, NULL);
}

void create_controls(void) {
    // ===== 右侧按钮区域 (200px 右边) =====
    #define BTN_W 110
    #define BTN_H 38

    // Pause 按钮
    lv_obj_t * btn = lv_btn_create(page);
    lv_obj_set_size(btn, BTN_W, BTN_H);
    lv_obj_align(btn, LV_ALIGN_TOP_RIGHT, -5, 5);
    lv_obj_set_style_bg_color(btn, lv_palette_main(LV_PALETTE_RED), 0);
    lv_obj_t * label_pause = lv_label_create(btn);
    lv_label_set_text(label_pause, "Pause");
    lv_obj_center(label_pause);
    lv_obj_set_style_text_font(label_pause, &lv_font_montserrat_14, 0);
    lv_obj_add_event_cb(btn, btn_pause_event_cb, LV_EVENT_CLICKED, NULL);

    // Auto 按钮
    btn_auto = lv_btn_create(page);
    lv_obj_set_size(btn_auto, BTN_W, BTN_H);
    lv_obj_align(btn_auto, LV_ALIGN_TOP_RIGHT, -5, 48);
    lv_obj_set_style_bg_color(btn_auto, lv_palette_main(LV_PALETTE_YELLOW), 0);
    lv_obj_t * label_auto = lv_label_create(btn_auto);
    lv_label_set_text(label_auto, "Auto");
    lv_obj_center(label_auto);
    lv_obj_set_style_text_color(label_auto, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(label_auto, &lv_font_montserrat_14, 0);
    lv_obj_add_event_cb(btn_auto, btn_auto_event_cb, LV_EVENT_CLICKED, NULL);

    // Trig 按钮
    btn_trig = lv_btn_create(page);
    lv_obj_set_size(btn_trig, BTN_W, BTN_H);
    lv_obj_align(btn_trig, LV_ALIGN_TOP_RIGHT, -5, 91);
    lv_obj_set_style_bg_color(btn_trig, lv_palette_main(LV_PALETTE_GREEN), 0);
    lv_obj_t * lb_trig = lv_label_create(btn_trig);
    lv_label_set_text(lb_trig, "Trig:Rise");
    lv_obj_center(lb_trig);
    lv_obj_set_style_text_font(lb_trig, &lv_font_montserrat_14, 0);
    lv_obj_add_event_cb(btn_trig, btn_trig_cb, LV_EVENT_CLICKED, NULL);

    // CH 按钮
    btn_ch_select = lv_btn_create(page);
    lv_obj_set_size(btn_ch_select, BTN_W, BTN_H);
    lv_obj_align(btn_ch_select, LV_ALIGN_TOP_RIGHT, -5, 177);
    lv_obj_set_style_bg_color(btn_ch_select, lv_palette_main(LV_PALETTE_GREY), 0);
    lv_obj_t * lb_ch = lv_label_create(btn_ch_select);
    lv_label_set_text(lb_ch, "Both");
    lv_obj_center(lb_ch);
    lv_obj_set_style_text_font(lb_ch, &lv_font_montserrat_14, 0);
    lv_obj_add_event_cb(btn_ch_select, btn_ch_select_cb, LV_EVENT_CLICKED, NULL);

    // ===== 底部控件区域 (图表下方) =====
    // Offset 滑块
    lv_obj_t * label_v_offset = lv_label_create(page);
    lv_label_set_text(label_v_offset, "Offset");
    lv_obj_align(label_v_offset, LV_ALIGN_TOP_LEFT, 5, 165);
    lv_obj_set_style_text_font(label_v_offset, &lv_font_montserrat_10, 0);
    slider_v_offset = lv_slider_create(page);
    lv_obj_set_size(slider_v_offset, 90, 15);
    lv_obj_align(slider_v_offset, LV_ALIGN_TOP_LEFT, 5, 180);
    lv_slider_set_range(slider_v_offset, -100, 100);
    lv_slider_set_value(slider_v_offset, 0, LV_ANIM_OFF);
    lv_obj_set_style_size(slider_v_offset, 8, LV_PART_KNOB);
    lv_obj_add_event_cb(slider_v_offset, slider_v_offset_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    // Scale 滑块
    lv_obj_t * label_v_scale = lv_label_create(page);
    lv_label_set_text(label_v_scale, "Scale");
    lv_obj_align(label_v_scale, LV_ALIGN_TOP_LEFT, 105, 165);
    lv_obj_set_style_text_font(label_v_scale, &lv_font_montserrat_10, 0);
    slider_v_scale = lv_slider_create(page);
    lv_obj_set_size(slider_v_scale, 90, 15);
    lv_obj_align(slider_v_scale, LV_ALIGN_TOP_LEFT, 105, 180);
    lv_slider_set_range(slider_v_scale, 10, 200);
    lv_slider_set_value(slider_v_scale, 100, LV_ANIM_OFF);
    lv_obj_set_style_size(slider_v_scale, 8, LV_PART_KNOB);
    lv_obj_add_event_cb(slider_v_scale, slider_v_scale_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    // Zoom X 滑块
    lv_obj_t * label_x_zoom = lv_label_create(page);
    lv_label_set_text(label_x_zoom, "Zoom");
    lv_obj_align(label_x_zoom, LV_ALIGN_TOP_LEFT, 5, 200);
    lv_obj_set_style_text_font(label_x_zoom, &lv_font_montserrat_10, 0);
    slider_zoom_x = lv_slider_create(page);
    lv_obj_set_size(slider_zoom_x, 90, 15);
    lv_obj_align(slider_zoom_x, LV_ALIGN_TOP_LEFT, 5, 215);
    lv_slider_set_range(slider_zoom_x, 256, 1024);
    lv_slider_set_value(slider_zoom_x, 256, LV_ANIM_OFF);
    lv_obj_set_style_size(slider_zoom_x, 8, LV_PART_KNOB);
    lv_obj_add_event_cb(slider_zoom_x, slider_zoom_x_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    // Ctrl 按钮
    btn_ctrl_select = lv_btn_create(page);
    lv_obj_set_size(btn_ctrl_select, 90, 30);
    lv_obj_align(btn_ctrl_select, LV_ALIGN_TOP_LEFT, 105, 205);
    lv_obj_set_style_bg_color(btn_ctrl_select, lv_palette_main(LV_PALETTE_GREY), 0);
    lv_obj_t * label_ctrl = lv_label_create(btn_ctrl_select);
    lv_label_set_text(label_ctrl, "Ctrl:CH1");
    lv_obj_center(label_ctrl);
    lv_obj_set_style_text_font(label_ctrl, &lv_font_montserrat_12, 0);
    lv_obj_add_event_cb(btn_ctrl_select, btn_ctrl_select_event_cb, LV_EVENT_CLICKED, NULL);

    // 测量标签 - 放在图表右侧空白区域
    label_meas_ch1 = lv_label_create(page);
    lv_label_set_text(label_meas_ch1, "CH1: ---");
    lv_obj_align(label_meas_ch1, LV_ALIGN_TOP_LEFT, 205, 5);
    lv_obj_set_style_text_font(label_meas_ch1, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(label_meas_ch1, lv_palette_main(LV_PALETTE_RED), 0);

    label_meas_ch2 = lv_label_create(page);
    lv_label_set_text(label_meas_ch2, "CH2: ---");
    lv_obj_align(label_meas_ch2, LV_ALIGN_TOP_LEFT, 205, 225);
    lv_obj_set_style_text_font(label_meas_ch2, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(label_meas_ch2, lv_palette_main(LV_PALETTE_BLUE), 0);
}

void ui_init(void) {
    create_my_screen();
    create_my_chart();
    create_controls();
    start_update_task();
}