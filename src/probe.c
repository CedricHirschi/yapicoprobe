/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2021 Raspberry Pi (Trading) Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 */

#include <pico/stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>

#include <hardware/clocks.h>
#include <hardware/gpio.h>

#include "led.h"
#include "picoprobe_config.h"
#include "probe.pio.h"
#include "tusb.h"

#include "DAP_config.h"
#include "DAP.h"


// Only want to set / clear one gpio per event so go up in powers of 2
enum _dbg_pins {
    DBG_PIN_WRITE_REQ = 1,
    DBG_PIN_WRITE = 2,
    DBG_PIN_WAIT = 4,
};


CU_REGISTER_DEBUG_PINS(probe_timing)

// Uncomment to enable debug
CU_SELECT_DEBUG_PINS(probe_timing)


struct _probe {
    // PIO offset
    uint offset;
    bool initted;
};

static struct _probe probe;



void probe_set_swclk_freq(uint32_t freq_khz)
{
    uint32_t clk_sys_freq_khz = clock_get_hz(clk_sys) / 1000;
    uint32_t div_256;
    uint32_t div_int;
    uint32_t div_frac;

    if (freq_khz > PROBE_MAX_KHZ) {
        freq_khz = PROBE_MAX_KHZ;
    }

    div_256 = (256 * clk_sys_freq_khz + freq_khz) / (6 * freq_khz);      // SWDCLK goes with PIOCLK / 6
    div_int  = div_256 >> 8;
    div_frac = div_256 & 0xff;

    picoprobe_debug("Set sysclk %lukHz swclk freq %lukHz, divider %lu + %lu/256\n", clk_sys_freq_khz, freq_khz, div_int, div_frac);
    if (div_int == 0) {
        picoprobe_error("probe_set_swclk_freq: underflow of clock setup, setting clock to maximum.\n");
        div_int  = 1;
        div_frac = 0;
    }

    // Worked out with pulseview
    pio_sm_set_clkdiv_int_frac(pio0, PROBE_SM, div_int, div_frac);
}   // probe_set_swclk_freq



void probe_assert_reset(bool state)
{
    /* Change the direction to out to drive pin to 0 or to in to emulate open drain */
    gpio_set_dir(PROBE_PIN_RESET, state);
}   // probe_assert_reset



#define CTRL_WORD_WRITE(CNT)                (                 ((CNT) << 8) + (probe.offset + probe_offset_output))
#define CTRL_WORD_WRITE_SHORT(CNT, DATA)    (((DATA) << 13) + ((CNT) << 8) + (probe.offset + probe_offset_short_output))
#define CTRL_WORD_READ(CNT)                 (                 ((CNT) << 8) + (probe.offset + probe_offset_input))



void __no_inline_not_in_flash_func(probe_write_bits)(uint bit_count, uint32_t data)
{
    if (bit_count <= 16) {
        pio_sm_put_blocking(pio0, PROBE_SM, CTRL_WORD_WRITE_SHORT(bit_count - 1, data));
    }
    else {
        pio_sm_put_blocking(pio0, PROBE_SM, CTRL_WORD_WRITE_SHORT(16 - 1, data & 0xffff));
        pio_sm_put_blocking(pio0, PROBE_SM, CTRL_WORD_WRITE_SHORT(bit_count - 17, data >> 16));
    }
    picoprobe_dump("Write %u bits 0x%lx\n", bit_count, data);
}   // probe_write_bits



uint32_t __no_inline_not_in_flash_func(probe_read_bits)(uint bit_count)
{
    uint32_t data;
    uint32_t data_shifted;

    pio_sm_put_blocking(pio0, PROBE_SM, CTRL_WORD_READ(bit_count - 1));
    data = pio_sm_get_blocking(pio0, PROBE_SM);
    data_shifted = data;
    if (bit_count < 32) {
        data_shifted = data >> (32 - bit_count);
    }
    picoprobe_dump("Read %u bits 0x%lx (shifted 0x%lx)\n", bit_count, data, data_shifted);
    return data_shifted;
}   // probe_read_bits



uint32_t __no_inline_not_in_flash_func(probe_send_cmd_ack)(uint8_t cmd)
{
    const uint32_t bits_read = DAP_Data.swd_conf.turnaround + 3;  // 4..7
    uint32_t ack;

    picoprobe_dump("probe_send_cmd_ack %02x\n", cmd);
    DEBUG_PINS_SET(probe_timing, DBG_PIN_WRITE_REQ);
    probe_write_bits(8, cmd);

    ack = probe_read_bits(bits_read);
    DEBUG_PINS_SET(probe_timing, DBG_PIN_WRITE);

    return ack >> DAP_Data.swd_conf.turnaround;
}   // probe_send_cmd_ack



void probe_gpio_init()
{
	static bool initialized;

	if ( !initialized) {
		initialized = true;
		picoprobe_debug("probe_gpio_init()\n");

		// Funcsel pins
		pio_gpio_init(pio0, PROBE_PIN_SWCLK);
		pio_gpio_init(pio0, PROBE_PIN_SWDIO);
		// Make sure SWDIO has a pullup on it. Idle state is high
		gpio_pull_up(PROBE_PIN_SWDIO);

		gpio_debug_pins_init();
	}
}   // probe_gpio_init



void probe_init()
{
    //    picoprobe_info("probe_init()\n");

    // Target reset pin: pull up, input to emulate open drain pin
    gpio_pull_up(PROBE_PIN_RESET);
    // gpio_init will leave the pin cleared and set as input
    gpio_init(PROBE_PIN_RESET);
    if ( !probe.initted) {
        // picoprobe_info("     2. probe_init()\n");
        uint offset = pio_add_program(pio0, &probe_program);
        probe.offset = offset;

        pio_sm_config sm_config = probe_program_get_default_config(offset);

        // Set SWCLK as a sideset pin
        sm_config_set_sideset_pins(&sm_config, PROBE_PIN_SWCLK);

        // Set SWDIO offset
        sm_config_set_out_pins(&sm_config, PROBE_PIN_SWDIO, 1);
        sm_config_set_set_pins(&sm_config, PROBE_PIN_SWDIO, 1);
        sm_config_set_in_pins(&sm_config, PROBE_PIN_SWDIO);

        // Set SWD and SWDIO pins as output to start. This will be set in the sm
        pio_sm_set_consecutive_pindirs(pio0, PROBE_SM, PROBE_PIN_OFFSET, 2, true);

        // shift output right, autopull on, autopull threshold
        sm_config_set_out_shift(&sm_config, true, true, 32);
        // shift input right as swd data is lsb first, autopush off
        sm_config_set_in_shift(&sm_config, true, false, 0);

        // Init SM with config
        pio_sm_init(pio0, PROBE_SM, offset, &sm_config);

        // Set up divisor
        probe_set_swclk_freq(DAP_DEFAULT_SWJ_CLOCK / 1000);

        // Enable SM
        pio_sm_set_enabled(pio0, PROBE_SM, true);
        probe.initted = true;
    }
}   // probe_init



void probe_deinit(void)
{
    if (probe.initted) {
        pio_sm_set_enabled(pio0, PROBE_SM, 0);
        pio_remove_program(pio0, &probe_program, probe.offset);
        probe.initted = false;
    }
}   // prebe_deinit
