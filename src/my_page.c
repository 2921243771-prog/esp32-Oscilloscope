#include "lvgl.h"
#include <math.h>

#define CONSTRAIN(x, low, high) ((x) < (low) ? (low) : ((x) > (high) ? (high) : (x)))

LV_IMG_DECLARE(img_watermark);

// 触发模式枚举
typedef enum {
    TRIG_NONE,
    TRIG_RISING,
    TRIG_FALLING
} TriggerMode;

// 1. 全局变量
lv_obj_t * slider_zoom_x;
lv_obj_t * my_chart;
lv_chart_series_t * ser1;
lv_chart_series_t * ser2;
lv_chart_series_t * ser_trigger; // 触发电平线
lv_obj_t * page;
lv_timer_t * chart_timer;

bool is_paused = true;
int32_t trigger_level = 0; // 触发阈值
TriggerMode ch1_trigger_mode = TRIG_NONE;
TriggerMode ch2_trigger_mode = TRIG_NONE;
lv_obj_t * btn_ch1_rise;
lv_obj_t * btn_ch2_rise;
lv_obj_t * btn_ch1_fall;
lv_obj_t * btn_ch2_fall;
lv_obj_t * btn_ch1;
lv_obj_t * btn_ch2;
static float angle_ch2 = 0.0f;

// 环形缓冲区（连续数据采集）
#define RING_BUFFER_SIZE 512
#define CHART_DISPLAY_POINTS 100
int32_t ring_buffer_ch1[RING_BUFFER_SIZE];
int32_t ring_buffer_ch2[RING_BUFFER_SIZE];
uint32_t write_idx = 0;

// 双通道独立触发状态
typedef struct {
    bool is_triggered;
    bool trigger_armed;
    int32_t trigger_idx;
    int32_t capture_count;
    int32_t last_val;
} ChTriggerState;
ChTriggerState ch1_trig = {false, true, -1, 0, 0};
ChTriggerState ch2_trig = {false, true, -1, 0, 0};

// 双通道独立缩放与偏移（10倍放大）
int32_t ch1_scale = 100;
int32_t ch1_offset = 0;
int32_t ch2_scale = 100;
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
        if(dsc->sub_part_ptr == ser_trigger) {
            dsc->line_dsc->dash_width = 6;
            dsc->line_dsc->dash_gap = 4;
        }
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
            trigger_level += dy * 2;
            trigger_level = CONSTRAIN(trigger_level, -200, 200);
            lv_coord_t * y_array = lv_chart_get_y_array(my_chart, ser_trigger);
            int32_t display_val = trigger_level + ch1_offset;
            for(int i = 0; i < 100; i++) y_array[i] = display_val;
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

    lv_coord_t * y_trig = lv_chart_get_y_array(my_chart, ser_trigger);
    int32_t display_val = trigger_level + ch1_offset;
    for(int i = 0; i < 100; i++) y_trig[i] = display_val;

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
    if(current_ctrl_ch == 1) {
        current_ctrl_ch = 2;
        lv_label_set_text(lv_obj_get_child(btn_ctrl_select, 0), "Ctrl:CH2");
        lv_slider_set_value(slider_v_scale, ch2_scale, LV_ANIM_OFF);
        lv_slider_set_value(slider_v_offset, ch2_offset, LV_ANIM_OFF);
    } else {
        current_ctrl_ch = 1;
        lv_label_set_text(lv_obj_get_child(btn_ctrl_select, 0), "Ctrl:CH1");
        lv_slider_set_value(slider_v_scale, ch1_scale, LV_ANIM_OFF);
        lv_slider_set_value(slider_v_offset, ch1_offset, LV_ANIM_OFF);
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
    ch1_scale = amplitude1 * 3 / 2;

    int32_t center2 = (max_val2 + min_val2) / 2;
    int32_t amplitude2 = (max_val2 - min_val2) / 2;
    ch2_offset = center2;
    ch2_scale = amplitude2 * 3 / 2;

    trigger_level = center1;
    lv_coord_t * trigger_array = lv_chart_get_y_array(my_chart, ser_trigger);
    for(int i = 0; i < 100; i++) {
        trigger_array[i] = trigger_level + ch1_offset;
    }

    lv_chart_set_zoom_x(my_chart, 256);
    lv_slider_set_value(slider_zoom_x, 256, LV_ANIM_OFF);

    current_ctrl_ch = 1;
    lv_label_set_text(lv_obj_get_child(btn_ctrl_select, 0), "Ctrl:CH1");
    lv_slider_set_value(slider_v_scale, ch1_scale, LV_ANIM_OFF);
    lv_slider_set_value(slider_v_offset, ch1_offset, LV_ANIM_OFF);

    update_chart_y_range();
    lv_chart_refresh(my_chart);
}

void btn_ch1_rise_cb(lv_event_t * e) {
    if(lv_obj_has_state(btn_ch1_rise, LV_STATE_CHECKED)) {
        lv_obj_clear_state(btn_ch1_rise, LV_STATE_CHECKED);
        ch1_trigger_mode = TRIG_NONE;
    } else {
        lv_obj_add_state(btn_ch1_rise, LV_STATE_CHECKED);
        lv_obj_clear_state(btn_ch1_fall, LV_STATE_CHECKED);
        ch1_trigger_mode = TRIG_RISING;
    }
    ch1_trig = (ChTriggerState){false, true, -1, 0, 0};
}

void btn_ch2_rise_cb(lv_event_t * e) {
    if(lv_obj_has_state(btn_ch2_rise, LV_STATE_CHECKED)) {
        lv_obj_clear_state(btn_ch2_rise, LV_STATE_CHECKED);
        ch2_trigger_mode = TRIG_NONE;
    } else {
        lv_obj_add_state(btn_ch2_rise, LV_STATE_CHECKED);
        lv_obj_clear_state(btn_ch2_fall, LV_STATE_CHECKED);
        ch2_trigger_mode = TRIG_RISING;
    }
    ch2_trig = (ChTriggerState){false, true, -1, 0, 0};
}

void btn_ch1_fall_cb(lv_event_t * e) {
    if(lv_obj_has_state(btn_ch1_fall, LV_STATE_CHECKED)) {
        lv_obj_clear_state(btn_ch1_fall, LV_STATE_CHECKED);
        ch1_trigger_mode = TRIG_NONE;
    } else {
        lv_obj_add_state(btn_ch1_fall, LV_STATE_CHECKED);
        lv_obj_clear_state(btn_ch1_rise, LV_STATE_CHECKED);
        ch1_trigger_mode = TRIG_FALLING;
    }
    ch1_trig = (ChTriggerState){false, true, -1, 0, 0};
}

void btn_ch2_fall_cb(lv_event_t * e) {
    if(lv_obj_has_state(btn_ch2_fall, LV_STATE_CHECKED)) {
        lv_obj_clear_state(btn_ch2_fall, LV_STATE_CHECKED);
        ch2_trigger_mode = TRIG_NONE;
    } else {
        lv_obj_add_state(btn_ch2_fall, LV_STATE_CHECKED);
        lv_obj_clear_state(btn_ch2_rise, LV_STATE_CHECKED);
        ch2_trigger_mode = TRIG_FALLING;
    }
    ch2_trig = (ChTriggerState){false, true, -1, 0, 0};
}

void btn_ch1_event_cb(lv_event_t * e) {
    lv_obj_t * btn = lv_event_get_target(e);
    if(lv_obj_has_state(btn, LV_STATE_CHECKED)) {
        lv_chart_hide_series(my_chart, ser1, false);
    } else {
        lv_chart_hide_series(my_chart, ser1, true);
    }
}

void btn_ch2_event_cb(lv_event_t * e) {
    lv_obj_t * btn = lv_event_get_target(e);
    if(lv_obj_has_state(btn, LV_STATE_CHECKED)) {
        lv_chart_hide_series(my_chart, ser2, false);
    } else {
        lv_chart_hide_series(my_chart, ser2, true);
    }
}
void slider_zoom_x_event_cb(lv_event_t * e) {
    lv_obj_t * slider = lv_event_get_target(e);
    int32_t zoom_val = lv_slider_get_value(slider);
    lv_chart_set_zoom_x(my_chart, (uint16_t)zoom_val);

    int major_cnt;
    if(zoom_val > 750) {
        major_cnt = 101;
    } else if(zoom_val > 500) {
        major_cnt = 21;
    } else {
        major_cnt = 11;
    }
    lv_chart_set_axis_tick(my_chart, LV_CHART_AXIS_PRIMARY_X, 0, 0, major_cnt, major_cnt, true, 20);
}

void update_task_cb(lv_timer_t * timer) {
    static float angle1 = 0.0f;
    static const float angle_step = 0.15f;
    static const float angle_max = 314.159f;

    for(int s = 0; s < 100; s++) {
        int32_t val_ch1 = ((int32_t)((sinf(angle1) * 6.0f) + 1.0f) + lv_rand(-1, 1)) * 10;
        int32_t val_ch2 = ((int32_t)((sinf(angle_ch2 + 1.5708f) * 6.0f) + 1.0f) + lv_rand(-1, 1)) * 10;

        angle1 += angle_step;
        if(angle1 > angle_max) angle1 = 0.0f;
        angle_ch2 += angle_step;
        if(angle_ch2 > angle_max) angle_ch2 = 0.0f;

        // CH1 处理
        if(ch1_trigger_mode == TRIG_NONE) {
            lv_chart_set_next_value(my_chart, ser1, val_ch1);
        } else {
            ring_buffer_ch1[write_idx] = val_ch1;
            if(!ch1_trig.is_triggered) {
                if(!ch1_trig.trigger_armed) {
                    if((ch1_trigger_mode == TRIG_RISING && val_ch1 < trigger_level - 15) ||
                       (ch1_trigger_mode == TRIG_FALLING && val_ch1 > trigger_level + 15)) {
                        ch1_trig.trigger_armed = true;
                    }
                } else {
                    bool edge = false;
                    if(ch1_trigger_mode == TRIG_RISING && ch1_trig.last_val < trigger_level && val_ch1 >= trigger_level) {
                        edge = true;
                    } else if(ch1_trigger_mode == TRIG_FALLING && ch1_trig.last_val > trigger_level && val_ch1 <= trigger_level) {
                        edge = true;
                    }
                    if(edge) {
                        ch1_trig.is_triggered = true;
                        ch1_trig.trigger_idx = write_idx;
                        ch1_trig.capture_count = 0;
                    }
                }
            } else {
                ch1_trig.capture_count++;
                if(ch1_trig.capture_count >= CHART_DISPLAY_POINTS) {
                    lv_coord_t * y1 = lv_chart_get_y_array(my_chart, ser1);
                    for(int i = 0; i < CHART_DISPLAY_POINTS; i++)
                        y1[i] = ring_buffer_ch1[(ch1_trig.trigger_idx + i) % RING_BUFFER_SIZE];
                    ser1->start_point = 0;
                    ch1_trig.is_triggered = false;
                    ch1_trig.trigger_armed = false;
                }
            }
        }

        // CH2 处理
        if(ch2_trigger_mode == TRIG_NONE) {
            lv_chart_set_next_value(my_chart, ser2, val_ch2);
        } else {
            ring_buffer_ch2[write_idx] = val_ch2;
            if(!ch2_trig.is_triggered) {
                if(!ch2_trig.trigger_armed) {
                    if((ch2_trigger_mode == TRIG_RISING && val_ch2 < trigger_level - 15) ||
                       (ch2_trigger_mode == TRIG_FALLING && val_ch2 > trigger_level + 15)) {
                        ch2_trig.trigger_armed = true;
                    }
                } else {
                    bool edge = false;
                    if(ch2_trigger_mode == TRIG_RISING && ch2_trig.last_val < trigger_level && val_ch2 >= trigger_level) {
                        edge = true;
                    } else if(ch2_trigger_mode == TRIG_FALLING && ch2_trig.last_val > trigger_level && val_ch2 <= trigger_level) {
                        edge = true;
                    }
                    if(edge) {
                        ch2_trig.is_triggered = true;
                        ch2_trig.trigger_idx = write_idx;
                        ch2_trig.capture_count = 0;
                    }
                }
            } else {
                ch2_trig.capture_count++;
                if(ch2_trig.capture_count >= CHART_DISPLAY_POINTS) {
                    lv_coord_t * y2 = lv_chart_get_y_array(my_chart, ser2);
                    for(int i = 0; i < CHART_DISPLAY_POINTS; i++)
                        y2[i] = ring_buffer_ch2[(ch2_trig.trigger_idx + i) % RING_BUFFER_SIZE];
                    ser2->start_point = 0;
                    ch2_trig.is_triggered = false;
                    ch2_trig.trigger_armed = false;
                }
            }
        }

        ch1_trig.last_val = val_ch1;
        ch2_trig.last_val = val_ch2;
    }

    lv_chart_refresh(my_chart);
    write_idx = (write_idx + 1) % RING_BUFFER_SIZE;

    // 测量计算（每10帧更新一次）
    static int meas_cnt = 0;
    if(++meas_cnt >= 10) {
        meas_cnt = 0;
        lv_coord_t * y1 = lv_chart_get_y_array(my_chart, ser1);
        lv_coord_t * y2 = lv_chart_get_y_array(my_chart, ser2);

        // CH1 测量
        int32_t min1 = 32767, max1 = -32767;
        for(int i = 0; i < CHART_DISPLAY_POINTS; i++) {
            if(y1[i] < min1) min1 = y1[i];
            if(y1[i] > max1) max1 = y1[i];
        }
        int32_t vpp1 = max1 - min1;
        int32_t mid1 = (max1 + min1) / 2;
        int cross1 = 0;
        for(int i = 1; i < CHART_DISPLAY_POINTS; i++) {
            if(y1[i-1] < mid1 && y1[i] >= mid1) cross1++;
        }
        char buf[40];
        int32_t vpp1_i = vpp1 / 10, vpp1_d = vpp1 % 10;
        if(cross1 >= 2) {
            int32_t period1 = CHART_DISPLAY_POINTS / cross1;
            lv_snprintf(buf, sizeof(buf), "1:Vpp%d.%d P%d F%d", vpp1_i, vpp1_d, period1, cross1);
        } else {
            lv_snprintf(buf, sizeof(buf), "1:Vpp%d.%d P-- F--", vpp1_i, vpp1_d);
        }
        lv_label_set_text(label_meas_ch1, buf);

        // CH2 测量
        int32_t min2 = 32767, max2 = -32767;
        for(int i = 0; i < CHART_DISPLAY_POINTS; i++) {
            if(y2[i] < min2) min2 = y2[i];
            if(y2[i] > max2) max2 = y2[i];
        }
        int32_t vpp2 = max2 - min2;
        int32_t mid2 = (max2 + min2) / 2;
        int cross2 = 0;
        for(int i = 1; i < CHART_DISPLAY_POINTS; i++) {
            if(y2[i-1] < mid2 && y2[i] >= mid2) cross2++;
        }
        int32_t vpp2_i = vpp2 / 10, vpp2_d = vpp2 % 10;
        if(cross2 >= 2) {
            int32_t period2 = CHART_DISPLAY_POINTS / cross2;
            lv_snprintf(buf, sizeof(buf), "2:Vpp%d.%d P%d F%d", vpp2_i, vpp2_d, period2, cross2);
        } else {
            lv_snprintf(buf, sizeof(buf), "2:Vpp%d.%d P-- F--", vpp2_i, vpp2_d);
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
    lv_chart_set_point_count(my_chart, 100);
    lv_chart_set_div_line_count(my_chart, 20, 20);
    lv_obj_set_size(my_chart, 240, 180);
    lv_obj_align(my_chart, LV_ALIGN_CENTER, -28, -60);

    lv_chart_set_range(my_chart, LV_CHART_AXIS_PRIMARY_Y, -100, 100);
    lv_chart_set_range(my_chart, LV_CHART_AXIS_SECONDARY_Y, -100, 100);
    lv_chart_set_range(my_chart, LV_CHART_AXIS_PRIMARY_X, 0, 100);

    lv_obj_set_style_text_font(my_chart, &lv_font_montserrat_10, LV_PART_TICKS);
    lv_obj_set_style_size(my_chart, 0, LV_PART_INDICATOR);

    lv_chart_set_axis_tick(my_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 0, 5, 5, true, 20);
    lv_chart_set_axis_tick(my_chart, LV_CHART_AXIS_SECONDARY_Y, 0, 0, 5, 5, true, 20);
    lv_chart_set_axis_tick(my_chart, LV_CHART_AXIS_PRIMARY_X, 0, 0, 5, 5, true, 20);

    lv_obj_clear_flag(my_chart, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(my_chart, LV_OPA_TRANSP, 0);

    ser1 = lv_chart_add_series(my_chart, lv_palette_main(LV_PALETTE_RED), LV_CHART_AXIS_PRIMARY_Y);
    ser2 = lv_chart_add_series(my_chart, lv_palette_main(LV_PALETTE_BLUE), LV_CHART_AXIS_SECONDARY_Y);

    ser_trigger = lv_chart_add_series(my_chart, lv_palette_main(LV_PALETTE_ORANGE), LV_CHART_AXIS_PRIMARY_Y);

    lv_coord_t * y_array = lv_chart_get_y_array(my_chart, ser_trigger);
    for(int i = 0; i < 100; i++) {
        y_array[i] = trigger_level + ch1_offset;
    }

    lv_obj_add_event_cb(my_chart, chart_draw_event_cb, LV_EVENT_DRAW_PART_BEGIN, NULL);
    lv_obj_add_event_cb(my_chart, chart_touch_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(my_chart, chart_touch_event_cb, LV_EVENT_PRESSING, NULL);
}

void create_controls(void) {
    // ===== 右侧按钮 2×4 网格 =====
    #define BTN_W 42
    #define BTN_H 28
    #define COL2_X -5
    #define COL1_X -44
    #define ROW_H 19

    // Row 0: Pause, Auto
    lv_obj_t * btn = lv_btn_create(page);
    lv_obj_set_size(btn, BTN_W, BTN_H);
    lv_obj_align(btn, LV_ALIGN_TOP_RIGHT, -10, 0);
    lv_obj_set_style_bg_color(btn, lv_palette_main(LV_PALETTE_RED), 0);
    lv_obj_t * label_pause = lv_label_create(btn);
    lv_label_set_text(label_pause, "Pause");
    lv_obj_center(label_pause);
    lv_obj_set_style_text_font(label_pause, &lv_font_montserrat_10, 0);
    lv_obj_add_event_cb(btn, btn_pause_event_cb, LV_EVENT_CLICKED, NULL);

    btn_auto = lv_btn_create(page);
    lv_obj_set_size(btn_auto, BTN_W, BTN_H);
    lv_obj_align(btn_auto, LV_ALIGN_TOP_RIGHT, -10, 30);
    lv_obj_set_style_bg_color(btn_auto, lv_palette_main(LV_PALETTE_YELLOW), 0);
    lv_obj_t * label_auto = lv_label_create(btn_auto);
    lv_label_set_text(label_auto, "Auto");
    lv_obj_center(label_auto);
    lv_obj_set_style_text_color(label_auto, lv_palette_main(LV_PALETTE_BLUE), 0);
    lv_obj_set_style_text_font(label_auto, &lv_font_montserrat_10, 0);
    lv_obj_add_event_cb(btn_auto, btn_auto_event_cb, LV_EVENT_CLICKED, NULL);

    // Row 1: 1Rise, 1Fall
    btn_ch1_rise = lv_btn_create(page);
    lv_obj_set_size(btn_ch1_rise, BTN_W, BTN_H);
    lv_obj_align(btn_ch1_rise, LV_ALIGN_TOP_RIGHT, -10, 60);
    lv_obj_set_style_bg_color(btn_ch1_rise, lv_color_hex(0x808080), 0);
    lv_obj_set_style_bg_color(btn_ch1_rise, lv_palette_main(LV_PALETTE_RED), LV_STATE_CHECKED);
    lv_obj_t * lb_c1r = lv_label_create(btn_ch1_rise);
    lv_label_set_text(lb_c1r, "1Rise");
    lv_obj_center(lb_c1r);
    lv_obj_set_style_text_font(lb_c1r, &lv_font_montserrat_10, 0);
    lv_obj_add_event_cb(btn_ch1_rise, btn_ch1_rise_cb, LV_EVENT_CLICKED, NULL);

    btn_ch1_fall = lv_btn_create(page);
    lv_obj_set_size(btn_ch1_fall, BTN_W, BTN_H);
    lv_obj_align(btn_ch1_fall, LV_ALIGN_TOP_RIGHT, -10, 90);
    lv_obj_set_style_bg_color(btn_ch1_fall, lv_color_hex(0x808080), 0);
    lv_obj_set_style_bg_color(btn_ch1_fall, lv_palette_main(LV_PALETTE_RED), LV_STATE_CHECKED);
    lv_obj_t * lb_c1f = lv_label_create(btn_ch1_fall);
    lv_label_set_text(lb_c1f, "1Fall");
    lv_obj_center(lb_c1f);
    lv_obj_set_style_text_font(lb_c1f, &lv_font_montserrat_10, 0);
    lv_obj_add_event_cb(btn_ch1_fall, btn_ch1_fall_cb, LV_EVENT_CLICKED, NULL);

    // Row 2: 2Rise, 2Fall
    btn_ch2_rise = lv_btn_create(page);
    lv_obj_set_size(btn_ch2_rise, BTN_W, BTN_H);
    lv_obj_align(btn_ch2_rise, LV_ALIGN_TOP_RIGHT, -10, 120);
    lv_obj_set_style_bg_color(btn_ch2_rise, lv_color_hex(0x808080), 0);
    lv_obj_set_style_bg_color(btn_ch2_rise, lv_palette_main(LV_PALETTE_BLUE), LV_STATE_CHECKED);
    lv_obj_t * lb_c2r = lv_label_create(btn_ch2_rise);
    lv_label_set_text(lb_c2r, "2Rise");
    lv_obj_center(lb_c2r);
    lv_obj_set_style_text_font(lb_c2r, &lv_font_montserrat_10, 0);
    lv_obj_add_event_cb(btn_ch2_rise, btn_ch2_rise_cb, LV_EVENT_CLICKED, NULL);

    btn_ch2_fall = lv_btn_create(page);
    lv_obj_set_size(btn_ch2_fall, BTN_W, BTN_H);
    lv_obj_align(btn_ch2_fall, LV_ALIGN_TOP_RIGHT, -10, 150);
    lv_obj_set_style_bg_color(btn_ch2_fall, lv_color_hex(0x808080), 0);
    lv_obj_set_style_bg_color(btn_ch2_fall, lv_palette_main(LV_PALETTE_BLUE), LV_STATE_CHECKED);
    lv_obj_t * lb_c2f = lv_label_create(btn_ch2_fall);
    lv_label_set_text(lb_c2f, "2Fall");
    lv_obj_center(lb_c2f);
    lv_obj_set_style_text_font(lb_c2f, &lv_font_montserrat_10, 0);
    lv_obj_add_event_cb(btn_ch2_fall, btn_ch2_fall_cb, LV_EVENT_CLICKED, NULL);

    // Row 3: CH1, CH2
    btn_ch1 = lv_btn_create(page);
    lv_obj_set_size(btn_ch1, BTN_W, BTN_H);
    lv_obj_align(btn_ch1, LV_ALIGN_TOP_RIGHT, -10, 180);
    lv_obj_set_style_bg_color(btn_ch1, lv_color_hex(0x808080), 0);
    lv_obj_set_style_bg_color(btn_ch1, lv_palette_main(LV_PALETTE_RED), LV_STATE_CHECKED);
    lv_obj_add_flag(btn_ch1, LV_OBJ_FLAG_CHECKABLE);
    lv_obj_add_state(btn_ch1, LV_STATE_CHECKED);
    lv_obj_t * label_ch1 = lv_label_create(btn_ch1);
    lv_label_set_text(label_ch1, "CH1");
    lv_obj_center(label_ch1);
    lv_obj_set_style_text_font(label_ch1, &lv_font_montserrat_10, 0);
    lv_obj_add_event_cb(btn_ch1, btn_ch1_event_cb, LV_EVENT_CLICKED, NULL);

    btn_ch2 = lv_btn_create(page);
    lv_obj_set_size(btn_ch2, BTN_W, BTN_H);
    lv_obj_align(btn_ch2, LV_ALIGN_TOP_RIGHT, -10, 210);
    lv_obj_set_style_bg_color(btn_ch2, lv_color_hex(0x808080), 0);
    lv_obj_set_style_bg_color(btn_ch2, lv_palette_main(LV_PALETTE_BLUE), LV_STATE_CHECKED);
    lv_obj_add_flag(btn_ch2, LV_OBJ_FLAG_CHECKABLE);
    lv_obj_add_state(btn_ch2, LV_STATE_CHECKED);
    lv_obj_t * label_ch2 = lv_label_create(btn_ch2);
    lv_label_set_text(label_ch2, "CH2");
    lv_obj_center(label_ch2);
    lv_obj_set_style_text_font(label_ch2, &lv_font_montserrat_10, 0);
    lv_obj_add_event_cb(btn_ch2, btn_ch2_event_cb, LV_EVENT_CLICKED, NULL);

    // ===== 底部控件 =====
    // Row 1: O slider, X slider
    lv_obj_t * label_v_offset = lv_label_create(page);
    lv_label_set_text(label_v_offset, "O");
    lv_obj_align(label_v_offset, LV_ALIGN_BOTTOM_LEFT, 5, -42);
    slider_v_offset = lv_slider_create(page);
    lv_obj_set_size(slider_v_offset, 60, 12);
    lv_obj_align(slider_v_offset, LV_ALIGN_BOTTOM_LEFT, 15, -42);
    lv_slider_set_range(slider_v_offset, -100, 100);
    lv_slider_set_value(slider_v_offset, 0, LV_ANIM_OFF);
    lv_obj_set_style_size(slider_v_offset, 6, LV_PART_KNOB);
    lv_obj_add_event_cb(slider_v_offset, slider_v_offset_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t * label_x_zoom = lv_label_create(page);
    lv_label_set_text(label_x_zoom, "X");
    lv_obj_align(label_x_zoom, LV_ALIGN_BOTTOM_LEFT, 85, -42);
    slider_zoom_x = lv_slider_create(page);
    lv_obj_set_size(slider_zoom_x, 60, 12);
    lv_obj_align(slider_zoom_x, LV_ALIGN_BOTTOM_LEFT, 95, -42);
    lv_slider_set_range(slider_zoom_x, 256, 1024);
    lv_slider_set_value(slider_zoom_x, 256, LV_ANIM_OFF);
    lv_obj_set_style_size(slider_zoom_x, 6, LV_PART_KNOB);
    lv_obj_add_event_cb(slider_zoom_x, slider_zoom_x_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    // Row 2: S slider, Ctrl button
    lv_obj_t * label_v_scale = lv_label_create(page);
    lv_label_set_text(label_v_scale, "S");
    lv_obj_align(label_v_scale, LV_ALIGN_BOTTOM_LEFT, 5, -24);
    slider_v_scale = lv_slider_create(page);
    lv_obj_set_size(slider_v_scale, 60, 12);
    lv_obj_align(slider_v_scale, LV_ALIGN_BOTTOM_LEFT, 15, -24);
    lv_slider_set_range(slider_v_scale, 10, 200);
    lv_slider_set_value(slider_v_scale, 100, LV_ANIM_OFF);
    lv_obj_set_style_size(slider_v_scale, 6, LV_PART_KNOB);
    lv_obj_add_event_cb(slider_v_scale, slider_v_scale_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    btn_ctrl_select = lv_btn_create(page);
    lv_obj_set_size(btn_ctrl_select, 55, 18);
    lv_obj_align(btn_ctrl_select, LV_ALIGN_BOTTOM_LEFT, 90, -22);
    lv_obj_set_style_bg_color(btn_ctrl_select, lv_palette_main(LV_PALETTE_GREY), 0);
    lv_obj_t * label_ctrl = lv_label_create(btn_ctrl_select);
    lv_label_set_text(label_ctrl, "Ctrl:CH1");
    lv_obj_center(label_ctrl);
    lv_obj_set_style_text_font(label_ctrl, &lv_font_montserrat_10, 0);
    lv_obj_add_event_cb(btn_ctrl_select, btn_ctrl_select_event_cb, LV_EVENT_CLICKED, NULL);

    // Row 3: 测量标签
    label_meas_ch1 = lv_label_create(page);
    lv_label_set_text(label_meas_ch1, "1:Vpp--");
    lv_obj_align(label_meas_ch1, LV_ALIGN_BOTTOM_LEFT, 5, -6);
    lv_obj_set_style_text_font(label_meas_ch1, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(label_meas_ch1, lv_palette_main(LV_PALETTE_RED), 0);

    label_meas_ch2 = lv_label_create(page);
    lv_label_set_text(label_meas_ch2, "2:Vpp--");
    lv_obj_align(label_meas_ch2, LV_ALIGN_BOTTOM_LEFT, 90, -6);
    lv_obj_set_style_text_font(label_meas_ch2, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(label_meas_ch2, lv_palette_main(LV_PALETTE_BLUE), 0);
}

void ui_init(void) {
    create_my_screen();
    create_my_chart();
    create_controls();
    start_update_task();
}