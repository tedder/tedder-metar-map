#pragma once

#include "esp_err.h"

#define OLED_I2C_ADDR    0x3C
#define OLED_PIN_SDA     7
#define OLED_PIN_SCL     6
#define OLED_H_RES       128
#define OLED_V_RES       32

esp_err_t oled_init(void);
void oled_show_three_lines(const char *line1, const char *line2, const char *line3);
