// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Vladimir Avtsenov
// Based on driver.c from notro/gud-pico, Copyright (c) 2021-2024 Noralf Trønnes
/*
 * GUD (Generic USB Display) device side for TinyUSB on ESP32-S3.
 * Derived from notro/gud-pico (driver.c), adapted to:
 *  - a composite device (vendor interface is not necessarily interface 0),
 *  - receiving rectangles straight into the panel framebuffer when possible,
 *  - running LZ4 decompression / rect copy in task context (tud_task()),
 *  - decompressing through an internal-SRAM window (see lz4_decode_to_fb()).
 */
#include <string.h>
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_timer.h"

#include "tusb.h"
#include "device/usbd_pvt.h"

#include "gud.h"
#include "gud_driver.h"
#include "usb_glue.h"

static const char *TAG = "gud";

#define GUD_CTRL_REQ_BUF_SIZE   128 /* fits EDID (128 bytes) */
#define GUD_BULK_EP_SIZE        64
/*
 * ESP32-S3 OTG has a 7-bit DOEPTSIZ packet counter (OTG_PACKET_COUNT_WIDTH), and the DWC2 port
 * doesn't clamp it: an xfer over 127 packets wraps the counter and the endpoint stalls mid-transfer
 * (host sees -ETIMEDOUT). Keep each xfer within 127 max-size packets.
 */
#define GUD_EDPT_MAX_PACKETS    127
#define GUD_EDPT_XFER_MAX_SIZE  (GUD_EDPT_MAX_PACKETS * GUD_BULK_EP_SIZE)

#define min(a, b) (((a) < (b)) ? (a) : (b))

/* LZ4 match offsets are 16-bit, so a 64 KB window holds everything a back-reference can reach */
#define LZ4_WINDOW_MASK         (GUD_LZ4_WINDOW_SIZE - 1)
#define LZ4_FLUSH_SIZE          (16 * 1024)

typedef struct {
    uint8_t  itf_num;
    uint8_t  ep_out;
    bool     opened;

    uint8_t *buf;        /* where the bulk data lands */
    uint32_t xfer_len;   /* bytes on the wire (compressed_length or length) */
    uint32_t len;        /* raw length */
    uint32_t offset;
    bool     direct;     /* buf points into the panel framebuffer */
    struct gud_set_buffer_req req;
} gud_interface_t;

static gud_interface_t _gud;

static const struct gud_display *_display;
static uint8_t *_rx_buf;
static size_t   _buf_size;
static uint8_t *_window;

static uint8_t _ctrl_req_buf[GUD_CTRL_REQ_BUF_SIZE] TU_ATTR_ALIGNED(4);
static uint8_t _status TU_ATTR_ALIGNED(4);

void gud_driver_setup(const struct gud_display *disp, void *rx_buf, size_t buf_size, void *lz4_window)
{
    _display = disp;
    _rx_buf = rx_buf;
    _buf_size = buf_size;
    _window = lz4_window;
}

/* ------------------------------------------------------------------------- */

static void gud_driver_init(void)
{
    memset(&_gud, 0, sizeof(_gud));
}

static bool gud_driver_deinit(void)
{
    return true;
}

static void gud_driver_reset(uint8_t rhport)
{
    (void) rhport;
    _gud.opened = false;
    _status = 0;
}

static uint16_t gud_driver_open(uint8_t rhport, tusb_desc_interface_t const *itf_desc, uint16_t max_len)
{
    /* Only claim the vendor-class interface; HID etc. are handled by built-in drivers */
    TU_VERIFY(itf_desc->bInterfaceClass == TUSB_CLASS_VENDOR_SPECIFIC, 0);
    TU_VERIFY(itf_desc->bNumEndpoints == 1, 0);

    uint16_t const drv_len = sizeof(tusb_desc_interface_t) + sizeof(tusb_desc_endpoint_t);
    TU_VERIFY(max_len >= drv_len, 0);

    tusb_desc_endpoint_t const *desc_ep = (tusb_desc_endpoint_t const *) tu_desc_next(itf_desc);
    TU_ASSERT(usbd_edpt_open(rhport, desc_ep), 0);

    _gud.ep_out = desc_ep->bEndpointAddress;
    _gud.itf_num = itf_desc->bInterfaceNumber;
    _gud.opened = true;

    ESP_EARLY_LOGI(TAG, "GUD interface %u opened, EP OUT 0x%02x", _gud.itf_num, _gud.ep_out);
    return drv_len;
}

static bool gud_bulk_xfer(uint8_t rhport, uint8_t *buf, uint32_t xfer_len, uint32_t len)
{
    TU_ASSERT(!usbd_edpt_busy(rhport, _gud.ep_out));

    if (buf) {
        TU_ASSERT(xfer_len && len);
        _gud.offset = 0;
        _gud.buf = buf;
        _gud.len = len;
        _gud.xfer_len = xfer_len;
    } else {
        _gud.offset += GUD_EDPT_XFER_MAX_SIZE;
        buf = _gud.buf + _gud.offset;
        xfer_len = _gud.xfer_len - _gud.offset;
    }

    if (xfer_len > GUD_EDPT_XFER_MAX_SIZE)
        xfer_len = GUD_EDPT_XFER_MAX_SIZE;

    return usbd_edpt_xfer(rhport, _gud.ep_out, buf, (uint16_t) xfer_len, false);
}

/* Control SETUP stage (called from tud_task) */
static bool gud_control_request(uint8_t rhport, tusb_control_request_t const *req)
{
    uint16_t wLength = min(req->wLength, sizeof(_ctrl_req_buf));
    int ret;

    if (req->bmRequestType_bit.recipient != TUSB_REQ_RCPT_INTERFACE ||
        req->bmRequestType_bit.type != TUSB_REQ_TYPE_VENDOR ||
        (uint8_t) req->wIndex != _gud.itf_num)
        return false;

    if (req->bmRequestType_bit.direction == TUSB_DIR_IN) {
        if (req->bRequest == GUD_REQ_GET_STATUS)
            return tud_control_xfer(rhport, req, &_status, sizeof(_status));

        _status = 0;
        ret = gud_req_get(_display, req->bRequest, req->wValue, _ctrl_req_buf, wLength);
        if (ret < 0) {
            _status = -ret;
            return false;
        }
        return tud_control_xfer(rhport, req, _ctrl_req_buf, ret);
    }

    /* OUT */
    _status = 0;
    if (!wLength) {
        ret = gud_req_set(_display, req->bRequest, req->wValue, _ctrl_req_buf, 0);
        if (ret < 0) {
            _status = -ret;
            return false;
        }
    }
    return tud_control_xfer(rhport, req, _ctrl_req_buf, wLength);
}

/* Control DATA stage complete (OUT requests with payload) */
static bool gud_control_complete(uint8_t rhport, tusb_control_request_t const *req)
{
    if (req->bmRequestType_bit.direction != TUSB_DIR_OUT)
        return true;

    uint16_t wLength = min(req->wLength, sizeof(_ctrl_req_buf));
    int ret = gud_req_set(_display, req->bRequest, req->wValue, _ctrl_req_buf, wLength);
    if (ret < 0) {
        _status = -ret;
        return false;
    }

    if (req->bRequest == GUD_REQ_SET_BUFFER) {
        const struct gud_set_buffer_req *r = (const struct gud_set_buffer_req *) _ctrl_req_buf;
        const uint32_t stride = _display->width * 2; /* RGB565 only */

        _gud.req = *r;

        if (gud_current_format() != GUD_PIXEL_FORMAT_RGB565 || !_display->fb || r->length > _buf_size ||
            (r->compression && (r->compressed_length > _buf_size || !_window))) {
            _status = GUD_STATUS_INVALID_PARAMETER;
            return false;
        }

        /* Uncompressed full-width rectangles are contiguous in the framebuffer: receive in place */
        _gud.direct = !r->compression && r->x == 0 && r->width == _display->width;

        uint8_t *rx = _gud.direct ? _display->fb + (size_t) r->y * stride : _rx_buf;
        uint32_t xfer_len = r->compression ? r->compressed_length : r->length;

        if (!gud_bulk_xfer(rhport, rx, xfer_len, r->length)) {
            _status = GUD_STATUS_BUSY;
            return false;
        }
    }
    return true;
}

bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const *req)
{
    /* Device-recipient vendor request (outside GUD): reboot into the bootloader's USB download window */
    if (req->bmRequestType_bit.recipient == TUSB_REQ_RCPT_DEVICE) {
        if (req->bRequest != USB_REQ_VENDOR_REBOOT || req->bmRequestType_bit.direction != TUSB_DIR_OUT)
            return false;
        if (stage == CONTROL_STAGE_SETUP)
            return tud_control_status(rhport, req);
        if (stage == CONTROL_STAGE_ACK)
            usb_glue_reboot();
        return true;
    }

    if (stage == CONTROL_STAGE_SETUP)
        return gud_control_request(rhport, req);
    if (stage == CONTROL_STAGE_DATA)
        return gud_control_complete(rhport, req);
    return true;
}

static bool gud_class_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const *req)
{
    (void) rhport; (void) stage; (void) req;
    return false;
}

/* ---------------------- raw rectangle data -> framebuffer ------------------- */

static struct {
    uint8_t *row;        /* framebuffer address of the current rectangle row */
    uint32_t row_bytes;
    uint32_t col;        /* bytes already written into the current row */
    uint32_t stride;
} _sink;

static void sink_begin(const struct gud_set_buffer_req *r)
{
    _sink.stride = _display->width * 2;
    _sink.row = _display->fb + (size_t) r->y * _sink.stride + (size_t) r->x * 2;
    _sink.row_bytes = r->width * 2;
    _sink.col = 0;
}

/* Append raw pixel bytes; consecutive calls continue where the previous one stopped */
static void sink_write(const uint8_t *src, uint32_t n)
{
    while (n) {
        uint32_t c = min(n, _sink.row_bytes - _sink.col);
        memcpy(_sink.row + _sink.col, src, c);
        _sink.col += c;
        src += c;
        n -= c;
        if (_sink.col == _sink.row_bytes) {
            _sink.col = 0;
            _sink.row += _sink.stride;
        }
    }
}

/* ------------------------- streaming LZ4 block decoder ---------------------- */

/*
 * Decoding straight into the PSRAM framebuffer made every back-reference a random PSRAM read, and the
 * burst of traffic starved the RGB panel's bounce-buffer refills (stale line fragments on screen).
 * Here the output goes through a 64 KB ring in internal SRAM; back-references are served from it and
 * the framebuffer only sees sequential writes, LZ4_FLUSH_SIZE bytes at a time.
 */
static uint32_t _wpos, _wflushed;

static void win_flush(void)
{
    while (_wflushed != _wpos) {
        uint32_t start = _wflushed & LZ4_WINDOW_MASK;
        uint32_t n = min(_wpos - _wflushed, GUD_LZ4_WINDOW_SIZE - start);
        sink_write(_window + start, n);
        _wflushed += n;
    }
}

static inline void win_put(uint8_t b)
{
    _window[_wpos & LZ4_WINDOW_MASK] = b;
    if (++_wpos - _wflushed >= LZ4_FLUSH_SIZE)
        win_flush();
}

static inline bool lz4_read_len(const uint8_t **ip, const uint8_t *iend, uint32_t *len)
{
    uint8_t b;
    do {
        if (*ip >= iend)
            return false;
        b = *(*ip)++;
        *len += b;
    } while (b == 255);
    return true;
}

static bool lz4_decode_to_fb(const uint8_t *src, uint32_t src_len, uint32_t out_len)
{
    const uint8_t *ip = src, *iend = src + src_len;

    _wpos = _wflushed = 0;
    while (ip < iend) {
        uint8_t token = *ip++;

        uint32_t lit = token >> 4;
        if (lit == 15 && !lz4_read_len(&ip, iend, &lit))
            return false;
        if ((uint32_t) (iend - ip) < lit || _wpos + lit > out_len)
            return false;
        while (lit--)
            win_put(*ip++);

        if (ip == iend)
            break;          /* the last sequence has literals only */

        if (iend - ip < 2)
            return false;
        uint32_t off = ip[0] | ((uint32_t) ip[1] << 8);
        ip += 2;
        if (!off || off > _wpos)
            return false;

        uint32_t mlen = token & 15;
        if (mlen == 15 && !lz4_read_len(&ip, iend, &mlen))
            return false;
        mlen += 4;
        if (_wpos + mlen > out_len)
            return false;
        while (mlen--)
            win_put(_window[(_wpos - off) & LZ4_WINDOW_MASK]);
    }
    win_flush();
    return _wpos == out_len;
}

static bool gud_class_xfer_cb(uint8_t rhport, uint8_t ep_addr, xfer_result_t result, uint32_t xferred_bytes)
{
    (void) ep_addr;
    TU_VERIFY(result == XFER_RESULT_SUCCESS);

    if (xferred_bytes != (_gud.xfer_len - _gud.offset)) {
        if (xferred_bytes != GUD_EDPT_XFER_MAX_SIZE) {
            /* short transfer: host sent less than announced */
            ESP_EARLY_LOGW(TAG, "short bulk: got %u, expected %u", (unsigned) xferred_bytes,
                           (unsigned) (_gud.xfer_len - _gud.offset));
            return false;
        }
        return gud_bulk_xfer(rhport, NULL, 0, 0);
    }

    if (_gud.direct)
        return true;

    sink_begin(&_gud.req);
    if (!_gud.req.compression) {
        sink_write(_gud.buf, _gud.len);
    } else if (!lz4_decode_to_fb(_gud.buf, _gud.xfer_len, _gud.len)) {
        ESP_EARLY_LOGW(TAG, "LZ4 failed: xfer=%u len=%u", (unsigned) _gud.xfer_len, (unsigned) _gud.len);
        return false;
    }
    return true;
}

static usbd_class_driver_t const _gud_class_driver[] = {
    {
        .name            = "GUD",
        .init            = gud_driver_init,
        .deinit          = gud_driver_deinit,
        .reset           = gud_driver_reset,
        .open            = gud_driver_open,
        .control_xfer_cb = gud_class_control_xfer_cb,
        .xfer_cb         = gud_class_xfer_cb,
        .xfer_isr        = NULL,
        .sof             = NULL,
    },
};

usbd_class_driver_t const *usbd_app_driver_get_cb(uint8_t *driver_count)
{
    *driver_count = TU_ARRAY_SIZE(_gud_class_driver);
    return _gud_class_driver;
}
