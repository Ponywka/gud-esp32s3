// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Vladimir Avtsenov
// Based on driver.c from notro/gud-pico, Copyright (c) 2021-2024 Noralf Trønnes
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "gud.h"

/* LZ4 decode window (internal SRAM), needed if disp->compression != 0 */
#define GUD_LZ4_WINDOW_SIZE  (64 * 1024)

/* Bind the display (disp->fb must be set), the bulk receive buffer (holds one compressed or raw
 * rectangle; size == disp->max_buffer_size) and a GUD_LZ4_WINDOW_SIZE decode window. */
void gud_driver_setup(const struct gud_display *disp, void *rx_buf, size_t buf_size, void *lz4_window);

/* Interface number the GUD vendor interface has in the configuration descriptor. */
#define GUD_ITF_NUM   1
