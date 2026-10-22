/* First 96 glyphs (0x20-0x7F), 5 bytes each — from Adafruit GFX glcdfont (BSD). */
#include <stdint.h>

const uint8_t font5x7_96[96 * 5] = {
#include "font5x7_data.inc"
};
