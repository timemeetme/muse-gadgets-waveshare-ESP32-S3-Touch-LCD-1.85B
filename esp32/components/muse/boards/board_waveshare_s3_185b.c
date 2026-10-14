/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Waveshare ESP32-S3-Touch-LCD-1.85B: round 360 px ST77916 QSPI panel with
 * CST816 touch, ES8311 speaker DAC + ES7210 mic ADC, BQ27220 battery fuel
 * gauge on I2C. Unlike the 1.85C there is no TCA9554 I/O expander: the panel
 * and touch resets are plain GPIOs (3 and 1) and the amp enable is GPIO9.
 * BOOT (GPIO0) is the talk button; holding a finger on the face tile also
 * talks.
 */
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "es8311_codec.h"
#include "es7210_adc.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "driver/usb_serial_jtag.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "muse_audio.h"
#include "muse_board.h"
#include "muse_ui.h"
#include "muse_lcd_bands.h"
#include "muse_mem.h"

#include "esp_lcd_st77916.h"
#include "esp_lcd_touch_cst816s.h"
#include "waveshare_s3_185b/st77916_init_185b.h"

static const char *TAG = "board";

/* Pins (board schematic / vendor demo). */
#define LCD_SCLK GPIO_NUM_40
#define LCD_D0 GPIO_NUM_46
#define LCD_D1 GPIO_NUM_45
#define LCD_D2 GPIO_NUM_42
#define LCD_D3 GPIO_NUM_41
#define LCD_CS GPIO_NUM_21
#define LCD_RST GPIO_NUM_3
#define LCD_TE GPIO_NUM_18   /* torn-effect output; read by the panel driver */
#define LCD_BL GPIO_NUM_5
#define LCD_RES 360

#define I2C_SDA GPIO_NUM_11
#define I2C_SCL GPIO_NUM_10
#define TOUCH_INT GPIO_NUM_4
#define TOUCH_RST GPIO_NUM_1

#define I2S_MCLK GPIO_NUM_2
#define I2S_BCLK GPIO_NUM_48
#define I2S_WS GPIO_NUM_38
#define I2S_DOUT GPIO_NUM_47   /* to the ES8311 DAC */
#define I2S_DIN GPIO_NUM_39    /* from the mic */
#define PA_EN GPIO_NUM_9

/* BQ27220 fuel gauge standard commands (TI SLUUBD4A), 16-bit little-endian. */
#define BQ27220_ADDR 0x55
#define BQ27220_REG_VOLTAGE 0x08        /* mV */
#define BQ27220_REG_CURRENT 0x0C        /* mA, signed: >0 charging, <0 draining */
#define BQ27220_REG_REL_SOC 0x2C        /* % */

#define DRAW_BUF_LINES 72      /* five bands to the screen (muse_lcd_bands.h) */
#define LCD_CHUNK_BYTES (LCD_RES * 8 * 2)

static i2c_master_bus_handle_t s_i2c;
static i2c_master_dev_handle_t s_gauge;
static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_touch_handle_t s_tp;
static lv_indev_t *s_touch_indev;
static muse_gpio_button_t s_boot;

/* The ST77916 needs even-aligned update windows over the QSPI command path. */
static void round_area(lv_event_t *e)
{
    lv_area_t *a = lv_event_get_param(e);
    a->x1 &= ~1;
    a->y1 &= ~1;
    a->x2 |= 1;
    a->y2 |= 1;
}

static esp_err_t init(void)
{
    const i2c_master_bus_config_t i2c_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_cfg, &s_i2c), TAG, "i2c");

    /* The fuel gauge sits on the battery; without a cell it stays silent.
     * read_power() reports "no battery" when it doesn't answer. */
    const i2c_device_config_t gauge_cfg = {
        .device_address = BQ27220_ADDR,
        .scl_speed_hz = 400000,
    };
    if (i2c_master_bus_add_device(s_i2c, &gauge_cfg, &s_gauge) != ESP_OK) {
        ESP_LOGW(TAG, "BQ27220 not registered; battery readings disabled");
        s_gauge = NULL;
    }

    ESP_RETURN_ON_ERROR(muse_gpio_button_init(&s_boot, GPIO_NUM_0), TAG, "boot button");
    /* BOOT is held at flashing time; don't count a held button as a press. */
    s_boot.pressed = gpio_get_level(GPIO_NUM_0) == 0;

    const gpio_config_t pa = {
        .pin_bit_mask = 1ULL << PA_EN,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&pa), TAG, "pa en");
    ESP_RETURN_ON_ERROR(gpio_set_level(PA_EN, 1), TAG, "pa on");

    return ESP_OK;
}

/* muse_lcd_bands_register takes over the panel IO's color-done callback and,
 * with the bands, needs the bus set up on the same core as the UI task. */
static lv_display_t *display_start(lv_indev_t **touch)
{
    const ledc_timer_config_t bl_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 20000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    const ledc_channel_config_t bl_ch = {
        .gpio_num = LCD_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
    };
    if (ledc_timer_config(&bl_timer) != ESP_OK || ledc_channel_config(&bl_ch) != ESP_OK) {
        ESP_LOGE(TAG, "backlight pwm");
        return NULL;
    }

    const spi_bus_config_t bus = {
        .sclk_io_num = LCD_SCLK,
        .data0_io_num = LCD_D0,
        .data1_io_num = LCD_D1,
        .data2_io_num = LCD_D2,
        .data3_io_num = LCD_D3,
        .max_transfer_sz = LCD_CHUNK_BYTES,
    };
    if (spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) {
        ESP_LOGE(TAG, "spi bus");
        return NULL;
    }
    const esp_lcd_panel_io_spi_config_t io_cfg = ST77916_PANEL_IO_QSPI_CONFIG(LCD_CS, NULL, NULL);
    if (esp_lcd_new_panel_io_spi(SPI2_HOST, &io_cfg, &s_io) != ESP_OK) {
        ESP_LOGE(TAG, "panel io");
        return NULL;
    }
    st77916_vendor_config_t vendor_cfg = {
        .init_cmds = ST77916_INIT_185B_CMDS,
        .init_cmds_size = ST77916_INIT_185B_SIZE,
        .flags = { .use_qspi_interface = 1 },
    };
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_cfg,
    };
    if (esp_lcd_new_panel_st77916(s_io, &panel_cfg, &s_panel) != ESP_OK) {
        ESP_LOGE(TAG, "st77916");
        return NULL;
    }
    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    esp_lcd_panel_disp_on_off(s_panel, true);

    esp_lv_adapter_config_t adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    adapter_cfg.task_core_id = MUSE_UI_CORE;
    adapter_cfg.task_priority = MUSE_UI_PRIORITY;
    if (esp_lv_adapter_init(&adapter_cfg) != ESP_OK) {
        return NULL;
    }
    const esp_lv_adapter_display_config_t disp_cfg = {
        .panel = s_panel,
        .panel_io = s_io,
        .profile = {
            .interface = ESP_LV_ADAPTER_PANEL_IF_OTHER,
            .rotation = ESP_LV_ADAPTER_ROTATE_0,
            .hor_res = LCD_RES,
            .ver_res = LCD_RES,
        },
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
    };
    lv_display_t *disp = muse_lcd_bands_register(disp_cfg, DRAW_BUF_LINES, LCD_CHUNK_BYTES);
    if (!disp) {
        return NULL;
    }
    lv_display_add_event_cb(disp, round_area, LV_EVENT_INVALIDATE_AREA, NULL);

    esp_lcd_panel_io_handle_t tp_io;
    const esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_CST816S_CONFIG();
    if (esp_lcd_new_panel_io_i2c(s_i2c, &tp_io_cfg, &tp_io) != ESP_OK) {
        ESP_LOGE(TAG, "touch io");
        return NULL;
    }
    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = LCD_RES,
        .y_max = LCD_RES,
        .rst_gpio_num = TOUCH_RST,
        .int_gpio_num = TOUCH_INT,
        .flags = {
            .swap_xy = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };
    if (esp_lcd_touch_new_i2c_cst816s(tp_io, &tp_cfg, &s_tp) != ESP_OK) {
        ESP_LOGE(TAG, "cst816");
        return NULL;
    }
    const esp_lv_adapter_touch_config_t tp_lvgl_cfg = ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(disp, s_tp);
    *touch = esp_lv_adapter_register_touch(&tp_lvgl_cfg);
    if (!*touch || esp_lv_adapter_start() != ESP_OK) {
        return NULL;
    }
    s_touch_indev = *touch;
    return disp;
}

static bool display_lock(int timeout_ms)
{
    return esp_lv_adapter_lock(timeout_ms) == ESP_OK;
}

static void set_brightness(int pct)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, pct * 1023 / 100);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static void panel_sleep(bool sleep)
{
    esp_lcd_panel_disp_sleep(s_panel, sleep);   /* SLPIN/SLPOUT; GRAM is kept */
    vTaskDelay(pdMS_TO_TICKS(120));             /* settle before the next command */
}

static void display_pause(bool pause)
{
    if (pause) {
        esp_lv_adapter_pause(-1);
    } else {
        esp_lv_adapter_resume();
    }
}

/*
 * ES8311 @ 0x18 for speaker (DAC) + ES7210 @ 0x40 for mic (ADC), same audio
 * layout as the 1.85C. I2S_NUM_1, MCLK=2, BCLK=48, WS=38, DOUT=47 (to
 * ES8311), DIN=39 (from ES7210). Follows the Waveshare vendor demo: separate
 * I2S data interfaces for play/record.
 * NOTE: audio_codec_new_i2c_ctrl expects the 8-bit shifted address, so use
 * ES7210_CODEC_DEFAULT_ADDR (0x80), NOT literal 0x40.
 */
static esp_err_t audio_init(esp_codec_dev_handle_t *spk, esp_codec_dev_handle_t *mic)
{
    i2s_chan_handle_t tx, rx;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &tx, &rx), TAG, "i2s chan");
    const i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MUSE_AUDIO_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_MCLK,
            .bclk = I2S_BCLK,
            .ws = I2S_WS,
            .dout = I2S_DOUT,
            .din = I2S_DIN,
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(tx, &std_cfg), TAG, "i2s tx");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(rx, &std_cfg), TAG, "i2s rx");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(tx), TAG, "i2s tx on");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(rx), TAG, "i2s rx on");

    const audio_codec_data_if_t *spk_data = audio_codec_new_i2s_data(&(audio_codec_i2s_cfg_t){
        .port = I2S_NUM_1, .rx_handle = NULL, .tx_handle = tx });
    ESP_RETURN_ON_FALSE(spk_data, ESP_ERR_NO_MEM, TAG, "spk data if");
    const audio_codec_ctrl_if_t *spk_ctrl = audio_codec_new_i2c_ctrl(&(audio_codec_i2c_cfg_t){
        .port = I2C_NUM_0, .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = s_i2c });
    const audio_codec_gpio_if_t *spk_gpio = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(spk_ctrl && spk_gpio, ESP_ERR_NO_MEM, TAG, "spk ctrl");
    const audio_codec_if_t *spk_codec = es8311_codec_new(&(es8311_codec_cfg_t){
        .ctrl_if = spk_ctrl,
        .gpio_if = spk_gpio,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = PA_EN,
        .use_mclk = false,
    });
    ESP_RETURN_ON_FALSE(spk_codec, ESP_FAIL, TAG, "ES8311 not responding");
    *spk = esp_codec_dev_new(&(esp_codec_dev_cfg_t){
        .dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = spk_codec, .data_if = spk_data });
    ESP_RETURN_ON_FALSE(*spk, ESP_FAIL, TAG, "spk dev");

    const audio_codec_data_if_t *mic_data = audio_codec_new_i2s_data(&(audio_codec_i2s_cfg_t){
        .port = I2S_NUM_1, .rx_handle = rx, .tx_handle = NULL });
    ESP_RETURN_ON_FALSE(mic_data, ESP_ERR_NO_MEM, TAG, "mic data if");
    const audio_codec_ctrl_if_t *mic_ctrl = audio_codec_new_i2c_ctrl(&(audio_codec_i2c_cfg_t){
        .port = I2C_NUM_0, .addr = ES7210_CODEC_DEFAULT_ADDR, .bus_handle = s_i2c });
    ESP_RETURN_ON_FALSE(mic_ctrl, ESP_ERR_NO_MEM, TAG, "mic ctrl");
    const audio_codec_if_t *mic_codec = es7210_codec_new(&(es7210_codec_cfg_t){
        .ctrl_if = mic_ctrl,
        .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2,
    });
    ESP_RETURN_ON_FALSE(mic_codec, ESP_FAIL, TAG, "ES7210 not responding");
    *mic = esp_codec_dev_new(&(esp_codec_dev_cfg_t){
        .dev_type = ESP_CODEC_DEV_TYPE_IN, .codec_if = mic_codec, .data_if = mic_data });
    ESP_RETURN_ON_FALSE(*mic, ESP_FAIL, TAG, "mic dev");

    ESP_LOGI(TAG, "audio: ES8311 (spk) + ES7210 (mic) on I2S1");
    return ESP_OK;
}

/* Hold-to-talk on the face tile: a finger held on the avatar for
 * TOUCH_HOLD_MS acts as the talk button. Taps and touches on other tiles
 * (e.g. settings) never trigger talk. Touch state comes from LVGL's input
 * device, which owns the CST816 driver handle; reading the driver here
 * would race LVGL's own reads. */
#define TOUCH_HOLD_MS 500

static unsigned poll_buttons(void)
{
    unsigned ev = muse_gpio_button_poll(&s_boot);

    static bool tp_down = false;          /* finger currently on screen */
    static bool tp_talking = false;       /* hold threshold reached */
    static TickType_t tp_press_tick = 0;  /* when the current press began */
    static bool tp_on_face = false;       /* press started on the face tile */

    bool pressed = false;
    if (s_touch_indev) {
        pressed = (lv_indev_get_state(s_touch_indev) == LV_INDEV_STATE_PRESSED);
    }

    if (pressed && !tp_down) {
        /* New press: only a hold that starts on the face tile can talk. */
        tp_down = true;
        tp_on_face = muse_ui_face_active();
        tp_press_tick = xTaskGetTickCount();
        tp_talking = false;
    } else if (!pressed && tp_down) {
        /* Release: end the talk if one started. */
        if (tp_talking) {
            ev |= MUSE_BTN_TALK_RELEASE;
        }
        tp_down = false;
        tp_talking = false;
    } else if (pressed && tp_down && tp_on_face && !tp_talking) {
        /* Still held on the face: start talking once the hold threshold passes. */
        if ((xTaskGetTickCount() - tp_press_tick) >= pdMS_TO_TICKS(TOUCH_HOLD_MS)) {
            tp_talking = true;
            ev |= MUSE_BTN_TALK_PRESS;
        }
    }
    return ev;
}

static void wait_buttons(int timeout_ms)
{
    muse_gpio_buttons_wait((muse_gpio_button_t *const[]){ &s_boot }, 1, timeout_ms);
}

/* One standard-command register of the fuel gauge, 16-bit little-endian. */
static esp_err_t gauge_read16(uint8_t reg, int16_t *out)
{
    uint8_t val[2];
    ESP_RETURN_ON_ERROR(i2c_master_transmit_receive(s_gauge, &reg, 1, val, 2, -1),
                        TAG, "gauge read");
    *out = (int16_t)(val[0] | (val[1] << 8));
    return ESP_OK;
}

/*
 * Battery through the BQ27220 fuel gauge on I2C: Voltage()/RelativeStateOfCharge()
 * and Current() for the charge state (positive = into the cell). The gauge is
 * powered from the cell; without one it NACKs and we report no battery.
 */
static esp_err_t read_power(muse_power_t *out)
{
    int16_t mv = 0, soc = 0, ma = 0;
    if (!s_gauge || gauge_read16(BQ27220_REG_VOLTAGE, &mv) != ESP_OK ||
        gauge_read16(BQ27220_REG_REL_SOC, &soc) != ESP_OK) {
        out->battery_mv = 0;
        out->battery_pct = -1;   /* no cell / no gauge */
        out->charging = false;
        out->usb = usb_serial_jtag_is_connected();
        return ESP_OK;
    }
    out->battery_mv = mv;
    out->battery_pct = soc;
    if (gauge_read16(BQ27220_REG_CURRENT, &ma) == ESP_OK) {
        out->charging = ma > 10;   /* hysteresis around the gauge's noise floor */
    }
    /* No VBUS sense: a USB host on the chip's own port is the closest. */
    out->usb = usb_serial_jtag_is_connected();
    return ESP_OK;
}

static esp_err_t power_off(void)
{
    set_brightness(0);
    esp_lcd_panel_disp_on_off(s_panel, false);
    esp_lcd_panel_disp_sleep(s_panel, true);
    /* No PMU on this board; deep sleep is the closest thing to off. BOOT
     * wakes it. */
    esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0);
    esp_deep_sleep_start();
    return ESP_FAIL;
}

static const muse_board_t s_board = {
    .name = "Waveshare ESP32-S3-Touch-LCD-1.85B",
    .width = LCD_RES,
    .height = LCD_RES,
    .round = true,
    .touch = true,
    .diagonal_in = 1.85f,
    .talk_button = "BOOT",
    /* BOOT is on the side edge, toward the top of the puck. */
    .talk_hint = { LV_ALIGN_TOP_LEFT, 20, 16 },
    .frame_ms = 40,
    .init = init,
    .display_start = display_start,
    .display_lock = display_lock,
    .display_unlock = esp_lv_adapter_unlock,
    .set_brightness = set_brightness,
    .panel_sleep = panel_sleep,
    .display_pause = display_pause,
    .audio_init = audio_init,
    .mic_slot = 0,   /* one mic */
    .set_mic_gain = NULL,   /* default: esp_codec_dev_set_in_gain */
    .poll_buttons = poll_buttons,
    .wait_buttons = wait_buttons,
    .read_power = read_power,
    .power_off = power_off,
};

/* Home Link's app_main starts Muse with this board (main/main.c). */
const muse_board_t *muse_board_get(void)
{
    return &s_board;
}
