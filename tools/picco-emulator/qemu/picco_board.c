/*
 * Onyx Boox Picco board models for the esp32s3 machine.
 *
 * Adds the parts the stock firmware talks to that the generic ESP32-S3
 * machine lacks: a working GPIO block (buttons, e-ink control pins and a
 * bit-banged 3-wire panel bus), the SSD1677 e-ink panel (frames are dumped
 * as PBM files), GPSPI2, both I2C controllers with the board's I2C chips,
 * the SAR ADC result registers and a USB-Serial-JTAG console.
 *
 * Board control from the QEMU monitor:
 *   qom-set /machine/picco gpio-low <mask>   pins held low (buttons pressed)
 *   qom-set /machine/picco touch "x y"       touch down at portrait x,y
 *   qom-set /machine/picco touch ""          touch up
 *
 * Environment: PICCO_OUT=<dir> for frames and console log (default ".").
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "exec/memory.h"
#include "hw/irq.h"
#include "hw/qdev-core.h"
#include "hw/dma/esp_gdma.h"
#include "hw/misc/picco_board.h"

/* Board pins (FreeInk SDK BoardConfig PICCO) */
#define PIN_EPD_SCLK 48
#define PIN_EPD_MOSI 47
#define PIN_EPD_CS 18
#define PIN_EPD_DC 17
#define PIN_EPD_RST 16
#define PIN_EPD_BUSY 15
#define PIN_TOUCH_INT 11
#define PIN_SD_CD 38

#define EPD_W 800
#define EPD_H 480

typedef struct I2cChip {
    uint8_t addr;
    uint8_t regs[256];
    uint8_t ptr;
    bool first;
} I2cChip;

typedef struct PiccoI2c {
    struct PiccoBoard *b;
    MemoryRegion mr;
    qemu_irq irq;
    uint32_t regs[0x200 / 4];
    uint8_t tx[64], rx[64];
    int txn, txr, rxn, rxr;
    I2cChip *cur;
    bool cur_read;
    bool addr_phase;
    int idx;
} PiccoI2c;

typedef struct PiccoBoard {
    Object parent;
    /* GPIO */
    MemoryRegion gpio_mr;
    qemu_irq gpio_irq;
    uint64_t out, en, in_level, status, low_mask;
    uint32_t pin[49];
    uint32_t gpio_regs[0x800 / 4];
    /* bit-banged panel bus */
    int bb_bits;
    uint8_t bb_byte;
    int rd_bit;
    /* SSD1677 */
    uint8_t cmd;
    uint8_t params[16];
    int nparam;
    bool reading;
    uint8_t rd_buf[16];
    int rd_len, rd_pos;
    uint8_t ram_bw[EPD_H][EPD_W / 8];
    uint8_t ram_red[EPD_H][EPD_W / 8];
    int entry_mode;
    int xs, xe, ys, ye, xc, yc;
    int frame;
    uint64_t panel_bytes;
    QEMUTimer *busy_timer;
    bool busy;
    /* GPSPI2 */
    MemoryRegion spi_mr;
    qemu_irq spi_irq;
    uint32_t spi_regs[0x100 / 4];
    ESPGdmaState *gdma;
    /* I2C */
    PiccoI2c i2c[2];
    I2cChip chips[5];
    int nchips;
    /* touch: TMA525C (PIP protocol) */
    int touch_x, touch_y;
    bool touch_down;
    bool touch_lift_pending;  /* deliver one count-0 report after a release */
    uint8_t touch_rpt[64];
    uint8_t tma_cmd_resp[64];  /* one-shot reply to an init command (HID/sysinfo) */
    int tma_cmd_resp_len;
    bool tma_cmd_consumed;     /* cleared after a full read of the one-shot reply */
    uint8_t tma_wr[16];        /* bytes written in the current I2C write */
    int tma_wr_n;
    /* SENS (SAR ADC) */
    MemoryRegion sens_mr;
    uint32_t sens_regs[0x400 / 4];
    /* USB serial JTAG */
    MemoryRegion usj_mr;
    qemu_irq usj_irq;
    uint32_t usj_int_ena;
    FILE *console;
    bool trace;
    char outdir[256];
} PiccoBoard;

#define TYPE_PICCO_BOARD "picco-board"
OBJECT_DECLARE_SIMPLE_TYPE(PiccoBoard, PICCO_BOARD)

/* ---------------------------------------------------------------- panel -- */

static void epd_dump(PiccoBoard *b, const char *why)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/frame_%04d.pbm", b->outdir, b->frame++);
    FILE *f = fopen(path, "wb");
    if (!f) {
        return;
    }
    /* PBM: 1 = black. Panel RAM: 1 = white. The SSD1677 scans gates in reverse
     * on this panel (the driver maps y -> _h-y-h), so dump rows bottom-to-top to
     * get the logical landscape image. Rotate 90 deg for the portrait device. */
    fprintf(f, "P4\n%d %d\n", EPD_W, EPD_H);
    for (int y = EPD_H - 1; y >= 0; y--) {
        for (int x = 0; x < EPD_W / 8; x++) {
            fputc(~b->ram_bw[y][x] & 0xff, f);
        }
    }
    fclose(f);
    fprintf(stderr, "[picco] frame %d (%s, ctrl2=%02x)\n", b->frame - 1, why, b->params[0]);
}

static void epd_busy_done(void *opaque)
{
    PiccoBoard *b = opaque;
    b->busy = false;
}

static void epd_set_busy(PiccoBoard *b, int ms)
{
    b->busy = true;
    timer_mod(b->busy_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + ms);
}

static void epd_ram_write(PiccoBoard *b, uint8_t v, bool red)
{
    int xb = b->xc / 8;
    if (b->yc >= 0 && b->yc < EPD_H && xb >= 0 && xb < EPD_W / 8) {
        if (red) {
            b->ram_red[b->yc][xb] = v;
        } else {
            b->ram_bw[b->yc][xb] = v;
        }
    }
    int xdir = (b->entry_mode & 1) ? 8 : -8;
    int ydir = (b->entry_mode & 2) ? 1 : -1;
    if (b->entry_mode & 4) { /* Y first */
        b->yc += ydir;
        if ((ydir > 0 && b->yc > b->ye) || (ydir < 0 && b->yc < b->ye)) {
            b->yc = b->ys;
            b->xc += xdir;
        }
    } else {
        b->xc += xdir;
        if ((xdir > 0 && b->xc > b->xe) || (xdir < 0 && b->xc < b->xe)) {
            b->xc = b->xs;
            b->yc += ydir;
            if ((ydir > 0 && b->yc > b->ye) || (ydir < 0 && b->yc < b->ye)) {
                b->yc = b->ys;
            }
        }
    }
}

static void epd_command(PiccoBoard *b, uint8_t c)
{
    b->cmd = c;
    b->nparam = 0;
    b->rd_len = 0;
    b->rd_pos = 0;
    switch (c) {
    case 0x20: /* master activation */
        epd_dump(b, "activate");
        epd_set_busy(b, 30);
        break;
    case 0x12: /* soft reset */
        epd_set_busy(b, 5);
        break;
    case 0x2E: { /* read user ID: answer as a YRD0426 panel */
        static const uint8_t id[10] = {0xCA, 0xFE, 0x00, 0x16, 0x77, 0x02, 0x10, 0x01, 0x00, 0x91};
        memcpy(b->rd_buf, id, 10);
        b->rd_len = 10;
        break;
    }
    case 0x2F: /* status */
        b->rd_buf[0] = 0x01;
        b->rd_len = 1;
        break;
    case 0x1B: /* temperature register read */
        b->rd_buf[0] = 0x19;
        b->rd_buf[1] = 0x00;
        b->rd_len = 2;
        break;
    default:
        break;
    }
}

static void epd_data(PiccoBoard *b, uint8_t v)
{
    b->panel_bytes++;
    switch (b->cmd) {
    case 0x24:
        epd_ram_write(b, v, false);
        return;
    case 0x26:
        epd_ram_write(b, v, true);
        return;
    default:
        break;
    }
    if (b->nparam < (int)sizeof(b->params)) {
        b->params[b->nparam] = v;
    }
    b->nparam++;
    switch (b->cmd) {
    case 0x11:
        b->entry_mode = v & 7;
        break;
    case 0x44:
        if (b->nparam == 4) {
            b->xs = b->params[0] | ((b->params[1] & 3) << 8);
            b->xe = b->params[2] | ((b->params[3] & 3) << 8);
        }
        break;
    case 0x45:
        if (b->nparam == 4) {
            b->ys = b->params[0] | ((b->params[1] & 3) << 8);
            b->ye = b->params[2] | ((b->params[3] & 3) << 8);
        }
        break;
    case 0x4E:
        if (b->nparam == 2) {
            b->xc = b->params[0] | ((b->params[1] & 3) << 8);
        }
        break;
    case 0x4F:
        if (b->nparam == 2) {
            b->yc = b->params[0] | ((b->params[1] & 3) << 8);
        }
        break;
    case 0x46:
    case 0x47:
        if (b->nparam == 1) {
            memset(b->cmd == 0x46 ? (void *)b->ram_bw : (void *)b->ram_red, (v & 0x80) ? 0xff : 0x00,
                   sizeof(b->ram_bw));
            epd_set_busy(b, 2);
        }
        break;
    default:
        break;
    }
}

static void epd_byte(PiccoBoard *b, uint8_t v, bool dc)
{
    if (dc) {
        epd_data(b, v);
    } else {
        epd_command(b, v);
    }
}

/* ----------------------------------------------------------------- GPIO -- */

static uint64_t gpio_pad_level(PiccoBoard *b)
{
    uint64_t lvl = b->in_level & ~b->low_mask;
    if (b->busy) {
        lvl |= 1ULL << PIN_EPD_BUSY;
    } else {
        lvl &= ~(1ULL << PIN_EPD_BUSY);
    }
    if (b->touch_down || b->touch_lift_pending || (b->tma_cmd_resp_len && !b->tma_cmd_consumed)) {
        lvl &= ~(1ULL << PIN_TOUCH_INT);
    }
    /* Panel drives SDA during a 3-wire read */
    if (b->rd_len && !(b->en & (1ULL << PIN_EPD_MOSI))) {
        int byte = b->rd_bit / 8, bit = 7 - (b->rd_bit % 8);
        int v = byte < b->rd_len ? (b->rd_buf[byte] >> bit) & 1 : 1;
        lvl = (lvl & ~(1ULL << PIN_EPD_MOSI)) | ((uint64_t)v << PIN_EPD_MOSI);
    }
    return (lvl & ~b->en) | (b->out & b->en);
}

static void gpio_update_irq(PiccoBoard *b)
{
    uint64_t lvl = gpio_pad_level(b);
    for (int i = 0; i < 49; i++) {
        int type = (b->pin[i] >> 7) & 7;
        if (type == 4 && !(lvl >> i & 1)) {
            b->status |= 1ULL << i;
        } else if (type == 5 && (lvl >> i & 1)) {
            b->status |= 1ULL << i;
        }
    }
    uint64_t ena = 0;
    for (int i = 0; i < 49; i++) {
        if ((b->pin[i] >> 13) & 0x1f) {
            ena |= 1ULL << i;
        }
    }
    qemu_set_irq(b->gpio_irq, (b->status & ena) != 0);
}

static void gpio_edges(PiccoBoard *b, uint64_t before, uint64_t after)
{
    uint64_t ch = before ^ after;
    for (int i = 0; ch && i < 49; i++) {
        if (!(ch >> i & 1)) {
            continue;
        }
        int type = (b->pin[i] >> 7) & 7;
        bool rise = after >> i & 1;
        if (type == 3 || (type == 1 && rise) || (type == 2 && !rise)) {
            b->status |= 1ULL << i;
        }
    }
    gpio_update_irq(b);
}

/* Called after every output change: decodes the bit-banged panel bus. */
static void gpio_outputs_changed(PiccoBoard *b, uint64_t old_out)
{
    uint64_t o = b->out;
    bool cs = o >> PIN_EPD_CS & 1, ocs = old_out >> PIN_EPD_CS & 1;
    bool clk = o >> PIN_EPD_SCLK & 1, oclk = old_out >> PIN_EPD_SCLK & 1;
    if (cs && !ocs) {
        b->bb_bits = 0;
        b->rd_bit = 0;
        b->rd_len = 0;
    }
    if (!cs && clk && !oclk && (b->en >> PIN_EPD_SCLK & 1)) {
        if (b->en >> PIN_EPD_MOSI & 1) {
            b->bb_byte = (b->bb_byte << 1) | (o >> PIN_EPD_MOSI & 1);
            if (++b->bb_bits == 8) {
                epd_byte(b, b->bb_byte, o >> PIN_EPD_DC & 1);
                b->bb_bits = 0;
                b->rd_bit = 0;
            }
        }
    }
    if (!cs && !clk && oclk && !(b->en >> PIN_EPD_MOSI & 1) && b->rd_len) {
        b->rd_bit++;
    }
}

static uint64_t gpio_read(void *opaque, hwaddr addr, unsigned size)
{
    PiccoBoard *b = opaque;
    uint64_t lvl;
    switch (addr) {
    case 0x04: return (uint32_t)b->out;
    case 0x10: return (uint32_t)(b->out >> 32);
    case 0x20: return (uint32_t)b->en;
    case 0x2C: return (uint32_t)(b->en >> 32);
    case 0x38: return 0x8; /* strap: SPI boot */
    case 0x3C: lvl = gpio_pad_level(b); return (uint32_t)lvl;
    case 0x40: lvl = gpio_pad_level(b); return (uint32_t)(lvl >> 32);
    case 0x44: return (uint32_t)b->status;
    case 0x50: return (uint32_t)(b->status >> 32);
    case 0x5C: case 0x64: return (uint32_t)b->status;
    case 0x68: case 0x70: return (uint32_t)(b->status >> 32);
    default:
        if (addr >= 0x74 && addr < 0x74 + 49 * 4) {
            return b->pin[(addr - 0x74) / 4];
        }
        return b->gpio_regs[addr / 4];
    }
}

static void gpio_write(void *opaque, hwaddr addr, uint64_t v, unsigned size)
{
    PiccoBoard *b = opaque;
    uint64_t old_out = b->out, old_en = b->en, before = gpio_pad_level(b);
    switch (addr) {
    case 0x04: b->out = (b->out & ~0xffffffffULL) | v; break;
    case 0x08: b->out |= v; break;
    case 0x0C: b->out &= ~v; break;
    case 0x10: b->out = (b->out & 0xffffffffULL) | (v << 32); break;
    case 0x14: b->out |= v << 32; break;
    case 0x18: b->out &= ~(v << 32); break;
    case 0x20: b->en = (b->en & ~0xffffffffULL) | v; break;
    case 0x24: b->en |= v; break;
    case 0x28: b->en &= ~v; break;
    case 0x2C: b->en = (b->en & 0xffffffffULL) | (v << 32); break;
    case 0x30: b->en |= v << 32; break;
    case 0x34: b->en &= ~(v << 32); break;
    case 0x44: b->status = (b->status & ~0xffffffffULL) | v; break;
    case 0x48: b->status |= v; break;
    case 0x4C: b->status &= ~v; break;
    case 0x50: b->status = (b->status & 0xffffffffULL) | (v << 32); break;
    case 0x54: b->status |= v << 32; break;
    case 0x58: b->status &= ~(v << 32); break;
    default:
        if (addr >= 0x74 && addr < 0x74 + 49 * 4) {
            b->pin[(addr - 0x74) / 4] = v;
        } else {
            b->gpio_regs[addr / 4] = v;
        }
        break;
    }
    if (b->out != old_out || b->en != old_en) {
        if (b->trace && ((b->out ^ old_out) | (b->en ^ old_en)) & ~((1ULL << PIN_EPD_SCLK) | (1ULL << PIN_EPD_MOSI))) {
            fprintf(stderr, "[gpio %" PRId64 "ms] out=%012" PRIx64 " en=%012" PRIx64 "\n",
                    qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL), b->out, b->en);
        }
        gpio_outputs_changed(b, old_out);
    }
    gpio_edges(b, before, gpio_pad_level(b));
}

static const MemoryRegionOps gpio_ops = {
    .read = gpio_read,
    .write = gpio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

/* --------------------------------------------------------------- GPSPI2 -- */

static void spi_transfer(PiccoBoard *b)
{
    if (b->trace) {
        fprintf(stderr, "[spi2] user=%08x dlen=%u dma=%08x\n", b->spi_regs[0x10 / 4], b->spi_regs[0x1C / 4] + 1, b->spi_regs[0x30 / 4]);
    }
    uint32_t user = b->spi_regs[0x10 / 4];
    uint32_t user2 = b->spi_regs[0x18 / 4];
    uint32_t dlen = (b->spi_regs[0x1C / 4] & 0x3ffff) + 1;
    uint32_t dma = b->spi_regs[0x30 / 4];
    bool dc = b->out >> PIN_EPD_DC & 1;
    bool cs_low = !(b->en >> PIN_EPD_CS & 1) || !(b->out >> PIN_EPD_CS & 1);

    if (user & (1u << 31)) {
        int bits = ((user2 >> 28) & 0xf) + 1;
        uint16_t val = user2 & 0xffff;
        if (bits == 8 && cs_low) {
            epd_byte(b, val & 0xff, false);
        }
    }
    if (user & (1u << 27)) {
        int n = (dlen + 7) / 8;
        g_autofree uint8_t *buf = g_malloc0(n);
        if (dma & (1u << 28)) {
            uint32_t chan;
            if (esp_gdma_get_channel_periph(b->gdma, GDMA_SPI2, ESP_GDMA_OUT_IDX, &chan)) {
                esp_gdma_read_channel(b->gdma, chan, buf, n);
            }
        } else {
            memcpy(buf, (uint8_t *)&b->spi_regs[0x98 / 4], MIN(n, 64));
        }
        if (cs_low) {
            for (int i = 0; i < n; i++) {
                epd_byte(b, buf[i], dc);
            }
        }
    }
    if (user & (1u << 28)) { /* MISO: nothing drives it */
        int n = (dlen + 7) / 8;
        if (dma & (1u << 27)) {
            uint32_t chan;
            g_autofree uint8_t *buf = g_malloc(n);
            memset(buf, 0xff, n);
            if (esp_gdma_get_channel_periph(b->gdma, GDMA_SPI2, ESP_GDMA_IN_IDX, &chan)) {
                esp_gdma_write_channel(b->gdma, chan, buf, n);
            }
        } else {
            memset(&b->spi_regs[0x98 / 4], 0xff, MIN(n, 64));
        }
    }
    b->spi_regs[0x3C / 4] |= 1u << 12; /* trans_done */
    qemu_set_irq(b->spi_irq, (b->spi_regs[0x3C / 4] & b->spi_regs[0x34 / 4]) != 0);
}

static uint64_t spi_read(void *opaque, hwaddr addr, unsigned size)
{
    PiccoBoard *b = opaque;
    if (addr == 0x40) {
        return b->spi_regs[0x3C / 4] & b->spi_regs[0x34 / 4];
    }
    return b->spi_regs[addr / 4];
}

static void spi_write(void *opaque, hwaddr addr, uint64_t v, unsigned size)
{
    PiccoBoard *b = opaque;
    switch (addr) {
    case 0x00:
        b->spi_regs[0] = v & ~((1u << 23) | (1u << 24));
        if (v & (1u << 24)) {
            spi_transfer(b);
        }
        return;
    case 0x38:
        b->spi_regs[0x3C / 4] &= ~v;
        break;
    case 0x44:
        b->spi_regs[0x3C / 4] |= v;
        break;
    default:
        b->spi_regs[addr / 4] = v;
        break;
    }
    qemu_set_irq(b->spi_irq, (b->spi_regs[0x3C / 4] & b->spi_regs[0x34 / 4]) != 0);
}

static const MemoryRegionOps spi_ops = {
    .read = spi_read,
    .write = spi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

/* ------------------------------------------------------------------ I2C -- */

static I2cChip *i2c_find(PiccoBoard *b, uint8_t addr)
{
    for (int i = 0; i < b->nchips; i++) {
        if (b->chips[i].addr == addr) {
            return &b->chips[i];
        }
    }
    return NULL;
}

/* TMA525C PIP protocol. A read returns, in priority order: a one-shot reply to
 * the last init command (HID descriptor / sysinfo), a live touch report while a
 * finger is down, a single count-0 report right after a release, else a bare
 * 2-byte "empty" length word. piccoReadReport() reads the 2-byte length first,
 * then the whole report, so the same buffer must answer both reads. */
static int tma_select_report(PiccoBoard *b, uint8_t *out)
{
    if (b->tma_cmd_resp_len && !b->tma_cmd_consumed) {
        memcpy(out, b->tma_cmd_resp, b->tma_cmd_resp_len);
        return b->tma_cmd_resp_len;
    }
    if (b->touch_down) {
        memset(out, 0, 17);
        out[0] = 17; out[1] = 0;
        out[2] = 0x01;        /* report id: touch */
        out[5] = 1;           /* contact count */
        uint8_t *t = out + 7;
        t[0] = 0x00;          /* finger */
        t[1] = 0x20;          /* event bits 5-6 = 1: touch-down/move */
        t[2] = b->touch_x & 0xff; t[3] = b->touch_x >> 8;
        t[4] = b->touch_y & 0xff; t[5] = b->touch_y >> 8;
        t[6] = 0x40;
        return 17;
    }
    if (b->touch_lift_pending) {
        memset(out, 0, 7);
        out[0] = 7; out[1] = 0;
        out[2] = 0x01;        /* touch report, contact count 0 => release */
        return 7;
    }
    out[0] = 2; out[1] = 0;
    return 2;
}

static uint8_t touch_read(PiccoBoard *b, int idx)
{
    uint8_t rpt[64];
    int len = tma_select_report(b, rpt);
    return idx < len ? rpt[idx] : 0;
}

/* Queue a one-shot reply to a TMA525C init command written to register 0x0004
 * (sysinfo) or a HID-descriptor read of register 0x0001. */
static void tma_handle_write(PiccoBoard *b)
{
    const uint8_t *w = b->tma_wr;
    int n = b->tma_wr_n;
    uint8_t *r = b->tma_cmd_resp;
    if (n >= 2 && w[0] == 0x01 && w[1] == 0x00) {
        /* HID descriptor read: byte 2 of the reply is the controller mode. */
        memset(r, 0, 16);
        r[0] = 16; r[1] = 0;
        r[2] = 0xF7;          /* PICCO_MODE_APP */
        b->tma_cmd_resp_len = 16;
        b->tma_cmd_consumed = false;
    } else if (n >= 5 && w[0] == 0x04 && w[1] == 0x00 && w[4] == 0x2F) {
        /* sysinfo (GET_SYSINFO): byte 50 = max contacts per refresh. */
        memset(r, 0, 51);
        r[0] = 51; r[1] = 0;
        r[2] = 0x1F;
        r[4] = 0x02;
        r[50] = 1;
        b->tma_cmd_resp_len = 51;
        b->tma_cmd_consumed = false;
    }
    b->tma_wr_n = 0;
}

static void i2c_update_irq(PiccoI2c *c)
{
    qemu_set_irq(c->irq, (c->regs[0x20 / 4] & c->regs[0x28 / 4]) != 0);
}

static void i2c_exec(PiccoI2c *c)
{
    PiccoBoard *b = c->b;
    uint32_t *raw = &c->regs[0x20 / 4];
    for (; c->idx < 8; c->idx++) {
        uint32_t *cmdp = &c->regs[0x58 / 4 + c->idx];
        uint32_t cmd = *cmdp;
        int op = (cmd >> 11) & 7, n = cmd & 0xff;
        bool ack_chk = cmd >> 8 & 1;
        switch (op) {
        case 6: /* RSTART */
            c->addr_phase = true;
            c->cur = NULL;
            break;
        case 1: /* WRITE */
            for (int i = 0; i < n; i++) {
                uint8_t v = c->txr < c->txn ? c->tx[c->txr++] : 0xff;
                if (c->addr_phase) {
                    c->addr_phase = false;
                    c->cur = i2c_find(b, v >> 1);
                    c->cur_read = v & 1;
                    if (b->trace) {
                        fprintf(stderr, "[i2c%d] addr 0x%02x %s%s\n", (int)(c - b->i2c), v >> 1, v & 1 ? "R" : "W", c->cur ? "" : " NACK");
                    }
                    if (c->cur) {
                        c->cur->first = true;
                        if (c->cur->addr == 0x24) {
                            c->cur->ptr = 0;
                            if (!c->cur_read) {
                                b->tma_wr_n = 0;
                            }
                        }
                    }
                    if (!c->cur && ack_chk) {
                        *cmdp |= 1u << 31;
                        *raw |= (1u << 10) | (1u << 7); /* NACK + trans complete */
                        c->idx = 8;
                        i2c_update_irq(c);
                        return;
                    }
                } else if (c->cur && c->cur->addr == 0x24) {
                    if (b->tma_wr_n < (int)sizeof(b->tma_wr)) {
                        b->tma_wr[b->tma_wr_n++] = v;
                    }
                } else if (c->cur) {
                    I2cChip *ch = c->cur;
                    if (ch->first) {
                        ch->ptr = v;
                        ch->first = false;
                    } else {
                        ch->regs[ch->ptr++] = v;
                    }
                }
            }
            break;
        case 3: /* READ */
            for (int i = 0; i < n; i++) {
                uint8_t v = 0xff;
                if (c->cur) {
                    if (c->cur->addr == 0x24) {
                        v = touch_read(b, c->cur->ptr++);
                    } else {
                        v = c->cur->regs[c->cur->ptr++];
                    }
                }
                if (c->rxn < 64) {
                    c->rx[c->rxn++] = v;
                }
            }
            break;
        case 2: /* STOP */
            *cmdp |= 1u << 31;
            if (c->cur && c->cur->addr == 0x24) {
                if (!c->cur_read) {
                    tma_handle_write(b);
                } else {
                    /* A full read consumes the current one-shot report: the 2-byte
                     * length probe stops at ptr==2, the full read at ptr==len. */
                    uint8_t rpt[64];
                    int len = tma_select_report(b, rpt);
                    if (c->cur->ptr >= len) {
                        if (b->tma_cmd_resp_len && !b->tma_cmd_consumed) {
                            b->tma_cmd_consumed = true;
                            b->tma_cmd_resp_len = 0;
                        } else if (b->touch_lift_pending) {
                            b->touch_lift_pending = false;
                        }
                    }
                }
            }
            c->cur = NULL;
            *raw |= 1u << 7; /* trans complete */
            c->idx = 8;
            i2c_update_irq(c);
            return;
        case 4: /* END */
            *cmdp |= 1u << 31;
            *raw |= 1u << 3; /* end detect */
            c->idx = 0;
            i2c_update_irq(c);
            return;
        default:
            break;
        }
        *cmdp |= 1u << 31;
    }
    *raw |= 1u << 7;
    i2c_update_irq(c);
}

static uint64_t i2c_read(void *opaque, hwaddr addr, unsigned size)
{
    PiccoI2c *c = opaque;
    switch (addr) {
    case 0x08: /* SR */
        return ((uint32_t)(c->rxn - c->rxr) << 8) | ((uint32_t)(c->txn - c->txr) << 18);
    case 0x1C:
        return c->rxr < c->rxn ? c->rx[c->rxr++] : 0;
    case 0x2C:
        return c->regs[0x20 / 4] & c->regs[0x28 / 4];
    default:
        if (addr >= 0x180 && addr < 0x200) {
            int i = (addr - 0x180) / 4;
            return i < 64 ? c->rx[i] : 0;
        }
        return c->regs[addr / 4];
    }
}

static void i2c_write(void *opaque, hwaddr addr, uint64_t v, unsigned size)
{
    PiccoI2c *c = opaque;
    switch (addr) {
    case 0x04:
        c->regs[1] = v & ~((1u << 5) | (1u << 11) | (1u << 10));
        if (v & (1u << 5)) {
            if (c->idx >= 8) {
                c->idx = 0;
            }
            i2c_exec(c);
        }
        return;
    case 0x18:
        c->regs[0x18 / 4] = v & ~((1u << 12) | (1u << 13));
        if (v & (1u << 12)) {
            c->rxn = c->rxr = 0;
        }
        if (v & (1u << 13)) {
            c->txn = c->txr = 0;
        }
        return;
    case 0x1C:
        if (c->txn < 64) {
            c->tx[c->txn++] = v;
        }
        return;
    case 0x24:
        c->regs[0x20 / 4] &= ~v;
        i2c_update_irq(c);
        return;
    case 0x28:
        c->regs[0x28 / 4] = v;
        i2c_update_irq(c);
        return;
    default:
        if (addr >= 0x58 && addr < 0x78) {
            c->regs[addr / 4] = v;
            if (addr == 0x58) {
                c->idx = 0;
            }
            return;
        }
        if (addr >= 0x100 && addr < 0x180) {
            int i = (addr - 0x100) / 4;
            if (i < 64) {
                c->tx[i] = v;
                c->txn = MAX(c->txn, i + 1);
            }
            return;
        }
        c->regs[addr / 4] = v;
        return;
    }
}

static const MemoryRegionOps i2c_ops = {
    .read = i2c_read,
    .write = i2c_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

/* ----------------------------------------------------------------- SENS -- */

static uint64_t sens_read(void *opaque, hwaddr addr, unsigned size)
{
    PiccoBoard *b = opaque;
    if (addr == 0x0C || addr == 0x30) { /* MEAS1/MEAS2 CTRL2: done + data */
        return (b->sens_regs[addr / 4] & ~0x1ffffu) | (1u << 16) | 2600;
    }
    return b->sens_regs[addr / 4];
}

static void sens_write(void *opaque, hwaddr addr, uint64_t v, unsigned size)
{
    PiccoBoard *b = opaque;
    b->sens_regs[addr / 4] = v;
}

static const MemoryRegionOps sens_ops = {
    .read = sens_read,
    .write = sens_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

/* ---------------------------------------------------- USB serial JTAG -- */

static uint64_t usj_read(void *opaque, hwaddr addr, unsigned size)
{
    PiccoBoard *b = opaque;
    switch (addr) {
    case 0x04: return 0x2;            /* IN endpoint has room */
    case 0x08: return 1u << 3;        /* SERIAL_IN_EMPTY */
    case 0x0C: return (1u << 3) & b->usj_int_ena;
    case 0x10: return b->usj_int_ena;
    default: return 0;
    }
}

static void usj_write(void *opaque, hwaddr addr, uint64_t v, unsigned size)
{
    PiccoBoard *b = opaque;
    switch (addr) {
    case 0x00:
        fputc(v & 0xff, stderr);
        if (b->console) {
            fputc(v & 0xff, b->console);
        }
        break;
    case 0x04:
        if (b->console) {
            fflush(b->console);
        }
        break;
    case 0x10:
        b->usj_int_ena = v;
        break;
    }
    qemu_set_irq(b->usj_irq, ((1u << 3) & b->usj_int_ena) != 0);
}

static const MemoryRegionOps usj_ops = {
    .read = usj_read,
    .write = usj_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

/* ------------------------------------------------------------ control -- */

static void prop_get_low(Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    PiccoBoard *b = PICCO_BOARD(obj);
    visit_type_uint64(v, name, &b->low_mask, errp);
}

static void prop_set_low(Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    PiccoBoard *b = PICCO_BOARD(obj);
    uint64_t val, before = gpio_pad_level(b);
    if (!visit_type_uint64(v, name, &val, errp)) {
        return;
    }
    b->low_mask = val;
    gpio_edges(b, before, gpio_pad_level(b));
}

static char *prop_get_touch(Object *obj, Error **errp)
{
    PiccoBoard *b = PICCO_BOARD(obj);
    return b->touch_down ? g_strdup_printf("%d %d", b->touch_x, b->touch_y) : g_strdup("");
}

static void prop_set_touch(Object *obj, const char *str, Error **errp)
{
    PiccoBoard *b = PICCO_BOARD(obj);
    uint64_t before = gpio_pad_level(b);
    int x, y;
    if (str && sscanf(str, "%d %d", &x, &y) == 2) {
        b->touch_x = x;
        b->touch_y = y;
        b->touch_down = true;
        b->touch_lift_pending = false;
    } else if (b->touch_down) {
        b->touch_down = false;
        b->touch_lift_pending = true;  /* deliver one count-0 report */
    }
    gpio_edges(b, before, gpio_pad_level(b));
}

static void picco_board_class_init(ObjectClass *oc, void *data)
{
    object_class_property_add(oc, "gpio-low", "uint64", prop_get_low, prop_set_low, NULL, NULL);
    object_class_property_add_str(oc, "touch", prop_get_touch, prop_set_touch);
}

static const TypeInfo picco_board_info = {
    .name = TYPE_PICCO_BOARD,
    .parent = TYPE_OBJECT,
    .instance_size = sizeof(PiccoBoard),
    .class_init = picco_board_class_init,
};

static void picco_board_register_types(void)
{
    type_register_static(&picco_board_info);
}

type_init(picco_board_register_types)

static void add_chip(PiccoBoard *b, uint8_t addr)
{
    I2cChip *c = &b->chips[b->nchips++];
    memset(c, 0, sizeof(*c));
    c->addr = addr;
}

void picco_board_create(MemoryRegion *sys, qemu_irq *irqs, ESPGdmaState *gdma)
{
    PiccoBoard *b = PICCO_BOARD(object_new(TYPE_PICCO_BOARD));
    object_property_add_child(qdev_get_machine(), "picco", OBJECT(b));

    const char *out = getenv("PICCO_OUT");
    snprintf(b->outdir, sizeof(b->outdir), "%s", out ? out : ".");
    char path[512];
    snprintf(path, sizeof(path), "%s/console.log", b->outdir);
    b->console = fopen(path, "w");
    b->trace = getenv("PICCO_TRACE") != NULL;

    /* Inputs idle high (buttons, touch INT, pulled-up lines); card detect low. */
    b->in_level = ~0ULL & ~(1ULL << PIN_SD_CD);
    b->busy_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, epd_busy_done, b);
    b->entry_mode = 3;
    b->xe = EPD_W - 1;
    b->ye = EPD_H - 1;
    memset(b->ram_bw, 0xff, sizeof(b->ram_bw));
    memset(b->ram_red, 0xff, sizeof(b->ram_red));
    b->gdma = gdma;

    memory_region_init_io(&b->gpio_mr, OBJECT(b), &gpio_ops, b, "picco.gpio", 0x800);
    memory_region_add_subregion_overlap(sys, 0x60004000, &b->gpio_mr, 1);
    b->gpio_irq = irqs[PICCO_IRQ_GPIO];

    memory_region_init_io(&b->spi_mr, OBJECT(b), &spi_ops, b, "picco.spi2", 0x100);
    memory_region_add_subregion_overlap(sys, 0x60024000, &b->spi_mr, 1);
    b->spi_irq = irqs[PICCO_IRQ_SPI2];

    static const hwaddr i2c_base[2] = {0x60013000, 0x60027000};
    static const int i2c_irq[2] = {PICCO_IRQ_I2C0, PICCO_IRQ_I2C1};
    for (int i = 0; i < 2; i++) {
        PiccoI2c *c = &b->i2c[i];
        c->b = b;
        c->idx = 8;
        memory_region_init_io(&c->mr, OBJECT(b), &i2c_ops, c, "picco.i2c", 0x200);
        memory_region_add_subregion_overlap(sys, i2c_base[i], &c->mr, 1);
        c->irq = irqs[i2c_irq[i]];
    }
    add_chip(b, 0x24); /* TMA525C touch */
    add_chip(b, 0x03); /* SGM41562 charger */
    add_chip(b, 0x38); /* warm/cool frontlight driver */
    add_chip(b, 0x51); /* PCF8563-class RTC */
    add_chip(b, 0x6A); /* QMI8658 IMU */
    I2cChip *rtc = i2c_find(b, 0x51);
    static const uint8_t t[7] = {0x00, 0x00, 0x12, 0x07, 0x06, 0x11, 0x26};
    memcpy(&rtc->regs[2], t, 7);
    i2c_find(b, 0x6A)->regs[0] = 0x05;

    memory_region_init_io(&b->sens_mr, OBJECT(b), &sens_ops, b, "picco.sens", 0x400);
    memory_region_add_subregion_overlap(sys, 0x60008800, &b->sens_mr, 1);

    memory_region_init_io(&b->usj_mr, OBJECT(b), &usj_ops, b, "picco.usb_serial_jtag", 0x100);
    memory_region_add_subregion_overlap(sys, 0x60038000, &b->usj_mr, 1);
    b->usj_irq = irqs[PICCO_IRQ_USJ];
}
