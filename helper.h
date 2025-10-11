#pragma once

#include <stdint.h>

uint32_t get_u24(const uint8_t *c, int offset);
uint16_t get_u16(const uint8_t *c, int offset);
uint8_t get_u8(const uint8_t *c, int offset);
int32_t get_s24(const uint8_t *c, int offset);
int16_t get_s16(const uint8_t *c, int offset);
int8_t get_s8(const uint8_t *c, int offset);
