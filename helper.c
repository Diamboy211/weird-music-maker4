#include <stdint.h>

uint32_t get_u24(const uint8_t *c, int offset)
{
	return ((uint32_t)c[offset+2] << 16) | ((uint32_t)c[offset+1] << 8) | (uint32_t)c[offset];
}

uint16_t get_u16(const uint8_t *c, int offset)
{
	return ((uint16_t)c[offset+1] << 8) | (uint16_t)c[offset];
}

uint8_t get_u8(const uint8_t *c, int offset)
{
	return c[offset];
}

int32_t get_s24(const uint8_t *c, int offset)
{
	uint32_t r = ((uint32_t)c[offset+2] << 16) | ((uint32_t)c[offset+1] << 8) | (uint32_t)c[offset];
	if (r & 0x00800000) return (int32_t)(r | 0xFF000000);
	return r;
}

int16_t get_s16(const uint8_t *c, int offset)
{
	return ((uint16_t)c[offset+1] << 8) | (uint16_t)c[offset];
}

int8_t get_s8(const uint8_t *c, int offset)
{
	return c[offset];
}
