#include <Arduino.h>
#include <lvgl.h>
#include <TFT_eSPI.h>
#include <Wire.h>
#include "my_page.h"

#define SCREEN_WIDTH  320
#define SCREEN_HEIGHT 240
#define FT6336_ADDR   0x38

TFT_eSPI tft = TFT_eSPI();
static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf[SCREEN_WIDTH * 80];
static lv_color_t buf2[SCREEN_WIDTH * 80];

void my_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p) {
    uint32_t w = area->x2 - area->x1 + 1;
    uint32_t h = area->y2 - area->y1 + 1;
    tft.startWrite();
    tft.setAddrWindow(area->x1, area->y1, w, h);
    tft.pushColors((uint16_t *)color_p, w * h, true);
    tft.endWrite();
    lv_disp_flush_ready(disp);
}

void my_touchpad_read(lv_indev_drv_t *indev, lv_indev_data_t *data) {
    Wire.beginTransmission(FT6336_ADDR);
    Wire.write(0x02);
    Wire.endTransmission();
    Wire.requestFrom(FT6336_ADDR, 11);
    uint8_t touches = Wire.read() & 0x0F;
    if (touches > 0) {
        uint16_t x1 = ((Wire.read() & 0x0F) << 8) | Wire.read();
        uint16_t y1 = ((Wire.read() & 0x0F) << 8) | Wire.read();
        int16_t sx = y1;
        int16_t sy = 239 - x1;
        if (sx < 0) sx = 0; if (sx > 319) sx = 319;
        if (sy < 0) sy = 0; if (sy > 239) sy = 239;
        data->state = LV_INDEV_STATE_PR;
        data->point.x = sx;
        data->point.y = sy;

    } else {
        data->state = LV_INDEV_STATE_REL;
    }
}

void setup() {
    tft.init();
    tft.setRotation(1);
    tft.fillScreen(TFT_BLACK);

    lv_init();
    lv_disp_draw_buf_init(&draw_buf, buf, buf2, SCREEN_WIDTH * 80);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = SCREEN_WIDTH;
    disp_drv.ver_res = SCREEN_HEIGHT;
    disp_drv.flush_cb = my_disp_flush;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    pinMode(TOUCH_RST, OUTPUT);
    digitalWrite(TOUCH_RST, LOW);
    delay(10);
    digitalWrite(TOUCH_RST, HIGH);
    delay(300);
    Wire.begin(TOUCH_SDA, TOUCH_SCL);

    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = my_touchpad_read;
    lv_indev_drv_register(&indev_drv);

    ui_init();
}

void loop() {
    lv_timer_handler();
    delay(5);
}
