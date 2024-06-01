#include <stdio.h>

#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "ws2812.pio.h"

#include "picoprobe_config.h"

#define IS_RGBW false

#ifdef PICO_DEFAULT_WS2812_PIN
#define WS2812_PIN PICO_DEFAULT_WS2812_PIN
#else
// default to pin 2 if the board doesn't have a default WS2812 pin defined
#define WS2812_PIN 16
#endif

#ifdef PICO_DEFAULT_WS2812_PIO
#define WS2812_PIO PICO_DEFAULT_WS2812_PIO
#else
// default to PIO0 if the board doesn't have a default WS2812 PIO defined
#define WS2812_PIO pio1
#endif

#ifdef PICO_DEFAULT_WS2812_SM
#define WS2812_SM PICO_DEFAULT_WS2812_SM
#else
// default to sm0 if the board doesn't have a default WS2812 state machine defined
#define WS2812_SM 0
#endif

uint32_t urgb_u32(uint8_t r, uint8_t g, uint8_t b)
{
    return ((uint32_t)(r) << 8) |
           ((uint32_t)(g) << 16) |
           (uint32_t)(b);
}

void ws2812_set(uint8_t r, uint8_t g, uint8_t b)
{
    uint32_t pixel_grb = urgb_u32(r, g, b);
    pio_sm_put_blocking(WS2812_PIO, WS2812_SM, pixel_grb << 8u);
}

void ws2812_init()
{
    PIO pio = WS2812_PIO;
    int sm = WS2812_SM;
    picoprobe_info("ws2812: initializing pio %u with sm %d\n", pio == pio0 ? 0 : 1, sm);
    uint offset = pio_add_program(pio, &ws2812_program);
    ws2812_program_init(pio, sm, offset, WS2812_PIN, 800000, IS_RGBW);
    picoprobe_info("ws2812: initialized\n");
}