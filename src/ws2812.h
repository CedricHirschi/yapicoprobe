#ifndef WS2812_H
#define WS2812_H

#include <stdint.h>

void ws2812_init(void);
void ws2812_set(uint8_t r, uint8_t g, uint8_t b);

#endif // WS2812_H