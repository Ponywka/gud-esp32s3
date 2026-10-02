#pragma once
#include <stdint.h>
#include "esp_err.h"

#define TOUCH_MAX_POINTS 5

typedef struct {
    uint8_t  id;      /* stable per contact while the finger is down */
    uint16_t x, y;    /* display pixels, already scaled/oriented */
} touch_point_t;

/* Called from the touch task with all currently pressed contacts (n may be 0 = all released). */
typedef void (*touch_cb_t)(const touch_point_t *pts, int n);

esp_err_t touch_init(void);
void touch_start(touch_cb_t cb);
