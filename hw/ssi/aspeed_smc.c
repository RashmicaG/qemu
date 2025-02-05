/*
 * ASPEED AST2400 SMC Controller (SPI Flash Only)
 *
 * Copyright (C) 2016 IBM Corp.
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
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "exec/address-spaces.h"
#include "qemu/units.h"

#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/ssi/aspeed_smc.h"

/* CE Type Setting Register */
#define R_CONF            (0x00 / 4)
#define   CONF_LEGACY_DISABLE  (1 << 31)
#define   CONF_ENABLE_W4       20
#define   CONF_ENABLE_W3       19
#define   CONF_ENABLE_W2       18
#define   CONF_ENABLE_W1       17
#define   CONF_ENABLE_W0       16
#define   CONF_FLASH_TYPE4     8
#define   CONF_FLASH_TYPE3     6
#define   CONF_FLASH_TYPE2     4
#define   CONF_FLASH_TYPE1     2
#define   CONF_FLASH_TYPE0     0
#define      CONF_FLASH_TYPE_NOR   0x0
#define      CONF_FLASH_TYPE_NAND  0x1
#define      CONF_FLASH_TYPE_SPI   0x2 /* AST2600 is SPI only */

/* CE Control Register */
#define R_CE_CTRL            (0x04 / 4)
#define   CTRL_EXTENDED4       4  /* 32 bit addressing for SPI */
#define   CTRL_EXTENDED3       3  /* 32 bit addressing for SPI */
#define   CTRL_EXTENDED2       2  /* 32 bit addressing for SPI */
#define   CTRL_EXTENDED1       1  /* 32 bit addressing for SPI */
#define   CTRL_EXTENDED0       0  /* 32 bit addressing for SPI */

/* Interrupt Control and Status Register */
#define R_INTR_CTRL       (0x08 / 4)
#define   INTR_CTRL_DMA_STATUS            (1 << 11)
#define   INTR_CTRL_CMD_ABORT_STATUS      (1 << 10)
#define   INTR_CTRL_WRITE_PROTECT_STATUS  (1 << 9)
#define   INTR_CTRL_DMA_EN                (1 << 3)
#define   INTR_CTRL_CMD_ABORT_EN          (1 << 2)
#define   INTR_CTRL_WRITE_PROTECT_EN      (1 << 1)

/* CEx Control Register */
#define R_CTRL0           (0x10 / 4)
#define   CTRL_IO_QPI              (1 << 31)
#define   CTRL_IO_QUAD_DATA        (1 << 30)
#define   CTRL_IO_DUAL_DATA        (1 << 29)
#define   CTRL_IO_DUAL_ADDR_DATA   (1 << 28) /* Includes dummies */
#define   CTRL_IO_QUAD_ADDR_DATA   (1 << 28) /* Includes dummies */
#define   CTRL_CMD_SHIFT           16
#define   CTRL_CMD_MASK            0xff
#define   CTRL_DUMMY_HIGH_SHIFT    14
#define   CTRL_AST2400_SPI_4BYTE   (1 << 13)
#define CE_CTRL_CLOCK_FREQ_SHIFT   8
#define CE_CTRL_CLOCK_FREQ_MASK    0xf
#define CE_CTRL_CLOCK_FREQ(div)                                         \
    (((div) & CE_CTRL_CLOCK_FREQ_MASK) << CE_CTRL_CLOCK_FREQ_SHIFT)
#define   CTRL_DUMMY_LOW_SHIFT     6 /* 2 bits [7:6] */
#define   CTRL_CE_STOP_ACTIVE      (1 << 2)
#define   CTRL_CMD_MODE_MASK       0x3
#define     CTRL_READMODE          0x0
#define     CTRL_FREADMODE         0x1
#define     CTRL_WRITEMODE         0x2
#define     CTRL_USERMODE          0x3
#define R_CTRL1           (0x14 / 4)
#define R_CTRL2           (0x18 / 4)
#define R_CTRL3           (0x1C / 4)
#define R_CTRL4           (0x20 / 4)

/* CEx Segment Address Register */
#define R_SEG_ADDR0       (0x30 / 4)
#define   SEG_END_SHIFT        24   /* 8MB units */
#define   SEG_END_MASK         0xff
#define   SEG_START_SHIFT      16   /* address bit [A29-A23] */
#define   SEG_START_MASK       0xff
#define R_SEG_ADDR1       (0x34 / 4)
#define R_SEG_ADDR2       (0x38 / 4)
#define R_SEG_ADDR3       (0x3C / 4)
#define R_SEG_ADDR4       (0x40 / 4)

/* Misc Control Register #1 */
#define R_MISC_CTRL1      (0x50 / 4)

/* SPI dummy cycle data */
#define R_DUMMY_DATA      (0x54 / 4)

/* DMA Control/Status Register */
#define R_DMA_CTRL        (0x80 / 4)
#define   DMA_CTRL_DELAY_MASK   0xf
#define   DMA_CTRL_DELAY_SHIFT  8
#define   DMA_CTRL_FREQ_MASK    0xf
#define   DMA_CTRL_FREQ_SHIFT   4
#define   DMA_CTRL_CALIB        (1 << 3)
#define   DMA_CTRL_CKSUM        (1 << 2)
#define   DMA_CTRL_WRITE        (1 << 1)
#define   DMA_CTRL_ENABLE       (1 << 0)

/* DMA Flash Side Address */
#define R_DMA_FLASH_ADDR  (0x84 / 4)

/* DMA DRAM Side Address */
#define R_DMA_DRAM_ADDR   (0x88 / 4)

/* DMA Length Register */
#define R_DMA_LEN         (0x8C / 4)

/* Checksum Calculation Result */
#define R_DMA_CHECKSUM    (0x90 / 4)

/* Misc Control Register #2 */
#define R_TIMINGS         (0x94 / 4)

/* SPI controller registers and bits (AST2400) */
#define R_SPI_CONF        (0x00 / 4)
#define   SPI_CONF_ENABLE_W0   0
#define R_SPI_CTRL0       (0x4 / 4)
#define R_SPI_MISC_CTRL   (0x10 / 4)
#define R_SPI_TIMINGS     (0x14 / 4)

#define ASPEED_SMC_R_SPI_MAX (0x20 / 4)
#define ASPEED_SMC_R_SMC_MAX (0x20 / 4)

#define ASPEED_SOC_SMC_FLASH_BASE   0x10000000
#define ASPEED_SOC_FMC_FLASH_BASE   0x20000000
#define ASPEED_SOC_SPI_FLASH_BASE   0x30000000
#define ASPEED_SOC_SPI2_FLASH_BASE  0x38000000

/*
 * DMA DRAM addresses should be 4 bytes aligned and the valid address
 * range is 0x40000000 - 0x5FFFFFFF (AST2400)
 *          0x80000000 - 0xBFFFFFFF (AST2500)
 *
 * DMA flash addresses should be 4 bytes aligned and the valid address
 * range is 0x20000000 - 0x2FFFFFFF.
 *
 * DMA length is from 4 bytes to 32MB
 *   0: 4 bytes
 *   0x7FFFFF: 32M bytes
 */
#define DMA_DRAM_ADDR(s, val)   ((s)->sdram_base | \
                                 ((val) & (s)->ctrl->dma_dram_mask))
#define DMA_FLASH_ADDR(s, val)  ((s)->ctrl->flash_window_base | \
                                ((val) & (s)->ctrl->dma_flash_mask))
#define DMA_LENGTH(val)         ((val) & 0x01FFFFFC)

/* Flash opcodes. */
#define SPI_OP_READ       0x03    /* Read data bytes (low frequency) */

#define SNOOP_OFF         0xFF
#define SNOOP_START       0x0

/*
 * Default segments mapping addresses and size for each slave per
 * controller. These can be changed when board is initialized with the
 * Segment Address Registers.
 */
static const AspeedSegments aspeed_segments_legacy[] = {
    { 0x10000000, 32 * 1024 * 1024 },
};

static const AspeedSegments aspeed_segments_fmc[] = {
    { 0x20000000, 64 * 1024 * 1024 }, /* start address is readonly */
    { 0x24000000, 32 * 1024 * 1024 },
    { 0x26000000, 32 * 1024 * 1024 },
    { 0x28000000, 32 * 1024 * 1024 },
    { 0x2A000000, 32 * 1024 * 1024 }
};

static const AspeedSegments aspeed_segments_spi[] = {
    { 0x30000000, 64 * 1024 * 1024 },
};

static const AspeedSegments aspeed_segments_ast2500_fmc[] = {
    { 0x20000000, 128 * 1024 * 1024 }, /* start address is readonly */
    { 0x28000000,  32 * 1024 * 1024 },
    { 0x2A000000,  32 * 1024 * 1024 },
};

static const AspeedSegments aspeed_segments_ast2500_spi1[] = {
    { 0x30000000, 32 * 1024 * 1024 }, /* start address is readonly */
    { 0x32000000, 96 * 1024 * 1024 }, /* end address is readonly */
};

static const AspeedSegments aspeed_segments_ast2500_spi2[] = {
    { 0x38000000, 32 * 1024 * 1024 }, /* start address is readonly */
    { 0x3A000000, 96 * 1024 * 1024 }, /* end address is readonly */
};
static uint32_t aspeed_smc_segment_to_reg(const AspeedSMCState *s,
                                          const AspeedSegments *seg);
static void aspeed_smc_reg_to_segment(const AspeedSMCState *s, uint32_t reg,
                                      AspeedSegments *seg);

/*
 * AST2600 definitions
 */
#define ASPEED26_SOC_FMC_FLASH_BASE   0x20000000
#define ASPEED26_SOC_SPI_FLASH_BASE   0x30000000
#define ASPEED26_SOC_SPI2_FLASH_BASE  0x50000000

static const AspeedSegments aspeed_segments_ast2600_fmc[] = {
    { 0x0, 128 * MiB }, /* start address is readonly */
    { 0x0, 0 }, /* disabled */
    { 0x0, 0 }, /* disabled */
};

static const AspeedSegments aspeed_segments_ast2600_spi1[] = {
    { 0x0, 128 * MiB }, /* start address is readonly */
    { 0x0, 0 }, /* disabled */
};

static const AspeedSegments aspeed_segments_ast2600_spi2[] = {
    { 0x0, 128 * MiB }, /* start address is readonly */
    { 0x0, 0 }, /* disabled */
    { 0x0, 0 }, /* disabled */
};

static uint32_t aspeed_2600_smc_segment_to_reg(const AspeedSMCState *s,
                                               const AspeedSegments *seg);
static void aspeed_2600_smc_reg_to_segment(const AspeedSMCState *s, uint32_t reg,
                                           AspeedSegments *seg);

static const AspeedSMCController controllers[] = {
    {
        .name              = "aspeed.smc-ast2400",
        .r_conf            = R_CONF,
        .r_ce_ctrl         = R_CE_CTRL,
        .r_ctrl0           = R_CTRL0,
        .r_timings         = R_TIMINGS,
        .conf_enable_w0    = CONF_ENABLE_W0,
        .max_slaves        = 5,
        .segments          = aspeed_segments_legacy,
        .flash_window_base = ASPEED_SOC_SMC_FLASH_BASE,
        .flash_window_size = 0x6000000,
        .has_dma           = false,
        .nregs             = ASPEED_SMC_R_SMC_MAX,
        .segment_to_reg    = aspeed_smc_segment_to_reg,
        .reg_to_segment    = aspeed_smc_reg_to_segment,
    }, {
        .name              = "aspeed.fmc-ast2400",
        .r_conf            = R_CONF,
        .r_ce_ctrl         = R_CE_CTRL,
        .r_ctrl0           = R_CTRL0,
        .r_timings         = R_TIMINGS,
        .conf_enable_w0    = CONF_ENABLE_W0,
        .max_slaves        = 5,
        .segments          = aspeed_segments_fmc,
        .flash_window_base = ASPEED_SOC_FMC_FLASH_BASE,
        .flash_window_size = 0x10000000,
        .has_dma           = true,
        .dma_flash_mask    = 0x0FFFFFFC,
        .dma_dram_mask     = 0x1FFFFFFC,
        .nregs             = ASPEED_SMC_R_MAX,
        .segment_to_reg    = aspeed_smc_segment_to_reg,
        .reg_to_segment    = aspeed_smc_reg_to_segment,
    }, {
        .name              = "aspeed.spi1-ast2400",
        .r_conf            = R_SPI_CONF,
        .r_ce_ctrl         = 0xff,
        .r_ctrl0           = R_SPI_CTRL0,
        .r_timings         = R_SPI_TIMINGS,
        .conf_enable_w0    = SPI_CONF_ENABLE_W0,
        .max_slaves        = 1,
        .segments          = aspeed_segments_spi,
        .flash_window_base = ASPEED_SOC_SPI_FLASH_BASE,
        .flash_window_size = 0x10000000,
        .has_dma           = false,
        .nregs             = ASPEED_SMC_R_SPI_MAX,
        .segment_to_reg    = aspeed_smc_segment_to_reg,
        .reg_to_segment    = aspeed_smc_reg_to_segment,
    }, {
        .name              = "aspeed.fmc-ast2500",
        .r_conf            = R_CONF,
        .r_ce_ctrl         = R_CE_CTRL,
        .r_ctrl0           = R_CTRL0,
        .r_timings         = R_TIMINGS,
        .conf_enable_w0    = CONF_ENABLE_W0,
        .max_slaves        = 3,
        .segments          = aspeed_segments_ast2500_fmc,
        .flash_window_base = ASPEED_SOC_FMC_FLASH_BASE,
        .flash_window_size = 0x10000000,
        .has_dma           = true,
        .dma_flash_mask    = 0x0FFFFFFC,
        .dma_dram_mask     = 0x3FFFFFFC,
        .nregs             = ASPEED_SMC_R_MAX,
        .segment_to_reg    = aspeed_smc_segment_to_reg,
        .reg_to_segment    = aspeed_smc_reg_to_segment,
    }, {
        .name              = "aspeed.spi1-ast2500",
        .r_conf            = R_CONF,
        .r_ce_ctrl         = R_CE_CTRL,
        .r_ctrl0           = R_CTRL0,
        .r_timings         = R_TIMINGS,
        .conf_enable_w0    = CONF_ENABLE_W0,
        .max_slaves        = 2,
        .segments          = aspeed_segments_ast2500_spi1,
        .flash_window_base = ASPEED_SOC_SPI_FLASH_BASE,
        .flash_window_size = 0x8000000,
        .has_dma           = false,
        .nregs             = ASPEED_SMC_R_MAX,
        .segment_to_reg    = aspeed_smc_segment_to_reg,
        .reg_to_segment    = aspeed_smc_reg_to_segment,
    }, {
        .name              = "aspeed.spi2-ast2500",
        .r_conf            = R_CONF,
        .r_ce_ctrl         = R_CE_CTRL,
        .r_ctrl0           = R_CTRL0,
        .r_timings         = R_TIMINGS,
        .conf_enable_w0    = CONF_ENABLE_W0,
        .max_slaves        = 2,
        .segments          = aspeed_segments_ast2500_spi2,
        .flash_window_base = ASPEED_SOC_SPI2_FLASH_BASE,
        .flash_window_size = 0x8000000,
        .has_dma           = false,
        .nregs             = ASPEED_SMC_R_MAX,
        .segment_to_reg    = aspeed_smc_segment_to_reg,
        .reg_to_segment    = aspeed_smc_reg_to_segment,
    }, {
        .name              = "aspeed.fmc-ast2600",
        .r_conf            = R_CONF,
        .r_ce_ctrl         = R_CE_CTRL,
        .r_ctrl0           = R_CTRL0,
        .r_timings         = R_TIMINGS,
        .conf_enable_w0    = CONF_ENABLE_W0,
        .max_slaves        = 3,
        .segments          = aspeed_segments_ast2600_fmc,
        .flash_window_base = ASPEED26_SOC_FMC_FLASH_BASE,
        .flash_window_size = 0x10000000,
        .has_dma           = true,
        .nregs             = ASPEED_SMC_R_MAX,
        .segment_to_reg    = aspeed_2600_smc_segment_to_reg,
        .reg_to_segment    = aspeed_2600_smc_reg_to_segment,
    }, {
        .name              = "aspeed.spi1-ast2600",
        .r_conf            = R_CONF,
        .r_ce_ctrl         = R_CE_CTRL,
        .r_ctrl0           = R_CTRL0,
        .r_timings         = R_TIMINGS,
        .conf_enable_w0    = CONF_ENABLE_W0,
        .max_slaves        = 2,
        .segments          = aspeed_segments_ast2600_spi1,
        .flash_window_base = ASPEED26_SOC_SPI_FLASH_BASE,
        .flash_window_size = 0x10000000,
        .has_dma           = false,
        .nregs             = ASPEED_SMC_R_MAX,
        .segment_to_reg    = aspeed_2600_smc_segment_to_reg,
        .reg_to_segment    = aspeed_2600_smc_reg_to_segment,
    }, {
        .name              = "aspeed.spi2-ast2600",
        .r_conf            = R_CONF,
        .r_ce_ctrl         = R_CE_CTRL,
        .r_ctrl0           = R_CTRL0,
        .r_timings         = R_TIMINGS,
        .conf_enable_w0    = CONF_ENABLE_W0,
        .max_slaves        = 3,
        .segments          = aspeed_segments_ast2600_spi2,
        .flash_window_base = ASPEED26_SOC_SPI2_FLASH_BASE,
        .flash_window_size = 0x10000000,
        .has_dma           = false,
        .nregs             = ASPEED_SMC_R_MAX,
        .segment_to_reg    = aspeed_2600_smc_segment_to_reg,
        .reg_to_segment    = aspeed_2600_smc_reg_to_segment,
    },
};

/*
 * The Segment Registers of the AST2400 and AST2500 have a 8MB
 * unit. The address range of a flash SPI slave is encoded with
 * absolute addresses which should be part of the overall controller
 * window.
 */
static uint32_t aspeed_smc_segment_to_reg(const AspeedSMCState *s,
                                          const AspeedSegments *seg)
{
    uint32_t reg = 0;
    reg |= ((seg->addr >> 23) & SEG_START_MASK) << SEG_START_SHIFT;
    reg |= (((seg->addr + seg->size) >> 23) & SEG_END_MASK) << SEG_END_SHIFT;
    return reg;
}

static void aspeed_smc_reg_to_segment(const AspeedSMCState *s,
                                      uint32_t reg, AspeedSegments *seg)
{
    seg->addr = ((reg >> SEG_START_SHIFT) & SEG_START_MASK) << 23;
    seg->size = (((reg >> SEG_END_SHIFT) & SEG_END_MASK) << 23) - seg->addr;
}

/*
 * The Segment Registers of the AST2600 have a 1MB unit. The address
 * range of a flash SPI slave is encoded with offsets in the overall
 * controller window. The previous SoC AST2400 and AST2500 used
 * absolute addresses. Only bits [27:20] are relevant and the end
 * address is an upper bound limit.
 */
#define AST2600_SEG_ADDR_MASK 0x0ff00000

static uint32_t aspeed_2600_smc_segment_to_reg(const AspeedSMCState *s,
                                               const AspeedSegments *seg)
{
    uint32_t reg = 0;

    /* Disabled segments have a nil register */
    if (!seg->size) {
        return 0;
    }

    reg |= (seg->addr & AST2600_SEG_ADDR_MASK) >> 16; /* start offset */
    reg |= (seg->addr + seg->size - 1) & AST2600_SEG_ADDR_MASK; /* end offset */
    return reg;
}

static void aspeed_2600_smc_reg_to_segment(const AspeedSMCState *s,
                                           uint32_t reg, AspeedSegments *seg)
{
    uint32_t start_offset = (reg << 16) & AST2600_SEG_ADDR_MASK;
    uint32_t end_offset = reg & AST2600_SEG_ADDR_MASK;

    seg->addr = s->ctrl->flash_window_base + start_offset;
    seg->size = end_offset + MiB - start_offset;
}

static bool aspeed_smc_flash_overlap(const AspeedSMCState *s,
                                     const AspeedSegments *new,
                                     int cs)
{
    AspeedSegments seg;
    int i;

    for (i = 0; i < s->ctrl->max_slaves; i++) {
        if (i == cs) {
            continue;
        }

        s->ctrl->reg_to_segment(s, s->regs[R_SEG_ADDR0 + i], &seg);

        if (new->addr + new->size > seg.addr &&
            new->addr < seg.addr + seg.size) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: new segment CS%d [ 0x%"
                          HWADDR_PRIx" - 0x%"HWADDR_PRIx" ] overlaps with "
                          "CS%d [ 0x%"HWADDR_PRIx" - 0x%"HWADDR_PRIx" ]\n",
                          s->ctrl->name, cs, new->addr, new->addr + new->size,
                          i, seg.addr, seg.addr + seg.size);
            return true;
        }
    }
    return false;
}

static void aspeed_smc_flash_set_segment(AspeedSMCState *s, int cs,
                                         uint64_t new)
{
    AspeedSMCFlash *fl = &s->flashes[cs];
    AspeedSegments seg;

    s->ctrl->reg_to_segment(s, new, &seg);

    /* The start address of CS0 is read-only */
    if (cs == 0 && seg.addr != s->ctrl->flash_window_base) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: Tried to change CS0 start address to 0x%"
                      HWADDR_PRIx "\n", s->ctrl->name, seg.addr);
        seg.addr = s->ctrl->flash_window_base;
        new = s->ctrl->segment_to_reg(s, &seg);
    }

    /*
     * The end address of the AST2500 spi controllers is also
     * read-only.
     */
    if ((s->ctrl->segments == aspeed_segments_ast2500_spi1 ||
         s->ctrl->segments == aspeed_segments_ast2500_spi2) &&
        cs == s->ctrl->max_slaves &&
        seg.addr + seg.size != s->ctrl->segments[cs].addr +
        s->ctrl->segments[cs].size) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: Tried to change CS%d end address to 0x%"
                      HWADDR_PRIx "\n", s->ctrl->name, cs, seg.addr + seg.size);
        seg.size = s->ctrl->segments[cs].addr + s->ctrl->segments[cs].size -
            seg.addr;
        new = s->ctrl->segment_to_reg(s, &seg);
    }

    /* Keep the segment in the overall flash window */
    if (seg.addr + seg.size <= s->ctrl->flash_window_base ||
        seg.addr > s->ctrl->flash_window_base + s->ctrl->flash_window_size) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: new segment for CS%d is invalid : "
                      "[ 0x%"HWADDR_PRIx" - 0x%"HWADDR_PRIx" ]\n",
                      s->ctrl->name, cs, seg.addr, seg.addr + seg.size);
        return;
    }

    /* Check start address vs. alignment */
    if (seg.size && !QEMU_IS_ALIGNED(seg.addr, seg.size)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: new segment for CS%d is not "
                      "aligned : [ 0x%"HWADDR_PRIx" - 0x%"HWADDR_PRIx" ]\n",
                      s->ctrl->name, cs, seg.addr, seg.addr + seg.size);
    }

    /* And segments should not overlap (in the specs) */
    aspeed_smc_flash_overlap(s, &seg, cs);

    /* All should be fine now to move the region */
    memory_region_transaction_begin();
    memory_region_set_size(&fl->mmio, seg.size);
    memory_region_set_address(&fl->mmio, seg.addr - s->ctrl->flash_window_base);
    memory_region_set_enabled(&fl->mmio, true);
    memory_region_transaction_commit();

    s->regs[R_SEG_ADDR0 + cs] = new;
}

static uint64_t aspeed_smc_flash_default_read(void *opaque, hwaddr addr,
                                              unsigned size)
{
    qemu_log_mask(LOG_GUEST_ERROR, "%s: To 0x%" HWADDR_PRIx " of size %u"
                  PRIx64 "\n", __func__, addr, size);
    return 0;
}

static void aspeed_smc_flash_default_write(void *opaque, hwaddr addr,
                                           uint64_t data, unsigned size)
{
    qemu_log_mask(LOG_GUEST_ERROR, "%s: To 0x%" HWADDR_PRIx " of size %u: 0x%"
                  PRIx64 "\n", __func__, addr, size, data);
}

static const MemoryRegionOps aspeed_smc_flash_default_ops = {
    .read = aspeed_smc_flash_default_read,
    .write = aspeed_smc_flash_default_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static inline int aspeed_smc_flash_mode(const AspeedSMCFlash *fl)
{
    const AspeedSMCState *s = fl->controller;

    return s->regs[s->r_ctrl0 + fl->id] & CTRL_CMD_MODE_MASK;
}

static inline bool aspeed_smc_is_writable(const AspeedSMCFlash *fl)
{
    const AspeedSMCState *s = fl->controller;

    return s->regs[s->r_conf] & (1 << (s->conf_enable_w0 + fl->id));
}

static inline int aspeed_smc_flash_cmd(const AspeedSMCFlash *fl)
{
    const AspeedSMCState *s = fl->controller;
    int cmd = (s->regs[s->r_ctrl0 + fl->id] >> CTRL_CMD_SHIFT) & CTRL_CMD_MASK;

    /*
     * In read mode, the default SPI command is READ (0x3). In other
     * modes, the command should necessarily be defined
     *
     * TODO: add support for READ4 (0x13) on AST2600
     */
    if (aspeed_smc_flash_mode(fl) == CTRL_READMODE) {
        cmd = SPI_OP_READ;
    }

    if (!cmd) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: no command defined for mode %d\n",
                      __func__, aspeed_smc_flash_mode(fl));
    }

    return cmd;
}

static inline int aspeed_smc_flash_is_4byte(const AspeedSMCFlash *fl)
{
    const AspeedSMCState *s = fl->controller;

    if (s->ctrl->segments == aspeed_segments_spi) {
        return s->regs[s->r_ctrl0] & CTRL_AST2400_SPI_4BYTE;
    } else {
        return s->regs[s->r_ce_ctrl] & (1 << (CTRL_EXTENDED0 + fl->id));
    }
}

static inline bool aspeed_smc_is_ce_stop_active(const AspeedSMCFlash *fl)
{
    const AspeedSMCState *s = fl->controller;

    return s->regs[s->r_ctrl0 + fl->id] & CTRL_CE_STOP_ACTIVE;
}

static void aspeed_smc_flash_select(AspeedSMCFlash *fl)
{
    AspeedSMCState *s = fl->controller;

    s->regs[s->r_ctrl0 + fl->id] &= ~CTRL_CE_STOP_ACTIVE;
    qemu_set_irq(s->cs_lines[fl->id], aspeed_smc_is_ce_stop_active(fl));
}

static void aspeed_smc_flash_unselect(AspeedSMCFlash *fl)
{
    AspeedSMCState *s = fl->controller;

    s->regs[s->r_ctrl0 + fl->id] |= CTRL_CE_STOP_ACTIVE;
    qemu_set_irq(s->cs_lines[fl->id], aspeed_smc_is_ce_stop_active(fl));
}

static uint32_t aspeed_smc_check_segment_addr(const AspeedSMCFlash *fl,
                                              uint32_t addr)
{
    const AspeedSMCState *s = fl->controller;
    AspeedSegments seg;

    s->ctrl->reg_to_segment(s, s->regs[R_SEG_ADDR0 + fl->id], &seg);
    if ((addr % seg.size) != addr) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: invalid address 0x%08x for CS%d segment : "
                      "[ 0x%"HWADDR_PRIx" - 0x%"HWADDR_PRIx" ]\n",
                      s->ctrl->name, addr, fl->id, seg.addr,
                      seg.addr + seg.size);
        addr %= seg.size;
    }

    return addr;
}

static int aspeed_smc_flash_dummies(const AspeedSMCFlash *fl)
{
    const AspeedSMCState *s = fl->controller;
    uint32_t r_ctrl0 = s->regs[s->r_ctrl0 + fl->id];
    uint32_t dummy_high = (r_ctrl0 >> CTRL_DUMMY_HIGH_SHIFT) & 0x1;
    uint32_t dummy_low = (r_ctrl0 >> CTRL_DUMMY_LOW_SHIFT) & 0x3;
    uint32_t dummies = ((dummy_high << 2) | dummy_low) * 8;

    if (r_ctrl0 & CTRL_IO_DUAL_ADDR_DATA) {
        dummies /= 2;
    }

    return dummies;
}

static void aspeed_smc_flash_setup(AspeedSMCFlash *fl, uint32_t addr)
{
    const AspeedSMCState *s = fl->controller;
    uint8_t cmd = aspeed_smc_flash_cmd(fl);
    int i;

    /* Flash access can not exceed CS segment */
    addr = aspeed_smc_check_segment_addr(fl, addr);

    ssi_transfer(s->spi, cmd);

    if (aspeed_smc_flash_is_4byte(fl)) {
        ssi_transfer(s->spi, (addr >> 24) & 0xff);
    }
    ssi_transfer(s->spi, (addr >> 16) & 0xff);
    ssi_transfer(s->spi, (addr >> 8) & 0xff);
    ssi_transfer(s->spi, (addr & 0xff));

    /*
     * Use fake transfers to model dummy bytes. The value should
     * be configured to some non-zero value in fast read mode and
     * zero in read mode. But, as the HW allows inconsistent
     * settings, let's check for fast read mode.
     */
    if (aspeed_smc_flash_mode(fl) == CTRL_FREADMODE) {
        for (i = 0; i < aspeed_smc_flash_dummies(fl); i++) {
            ssi_transfer(fl->controller->spi, s->regs[R_DUMMY_DATA] & 0xff);
        }
    }
}

static uint64_t aspeed_smc_flash_read(void *opaque, hwaddr addr, unsigned size)
{
    AspeedSMCFlash *fl = opaque;
    AspeedSMCState *s = fl->controller;
    uint64_t ret = 0;
    int i;

    switch (aspeed_smc_flash_mode(fl)) {
    case CTRL_USERMODE:
        for (i = 0; i < size; i++) {
            ret |= ssi_transfer(s->spi, 0x0) << (8 * i);
        }
        break;
    case CTRL_READMODE:
    case CTRL_FREADMODE:
        aspeed_smc_flash_select(fl);
        aspeed_smc_flash_setup(fl, addr);

        for (i = 0; i < size; i++) {
            ret |= ssi_transfer(s->spi, 0x0) << (8 * i);
        }

        aspeed_smc_flash_unselect(fl);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: invalid flash mode %d\n",
                      __func__, aspeed_smc_flash_mode(fl));
    }

    return ret;
}

/*
 * TODO (clg@kaod.org): stolen from xilinx_spips.c. Should move to a
 * common include header.
 */
typedef enum {
    READ = 0x3,         READ_4 = 0x13,
    FAST_READ = 0xb,    FAST_READ_4 = 0x0c,
    DOR = 0x3b,         DOR_4 = 0x3c,
    QOR = 0x6b,         QOR_4 = 0x6c,
    DIOR = 0xbb,        DIOR_4 = 0xbc,
    QIOR = 0xeb,        QIOR_4 = 0xec,

    PP = 0x2,           PP_4 = 0x12,
    DPP = 0xa2,
    QPP = 0x32,         QPP_4 = 0x34,
} FlashCMD;

static int aspeed_smc_num_dummies(uint8_t command)
{
    switch (command) { /* check for dummies */
    case READ: /* no dummy bytes/cycles */
    case PP:
    case DPP:
    case QPP:
    case READ_4:
    case PP_4:
    case QPP_4:
        return 0;
    case FAST_READ:
    case DOR:
    case QOR:
    case DOR_4:
    case QOR_4:
        return 1;
    case DIOR:
    case FAST_READ_4:
    case DIOR_4:
        return 2;
    case QIOR:
    case QIOR_4:
        return 4;
    default:
        return -1;
    }
}

static bool aspeed_smc_do_snoop(AspeedSMCFlash *fl,  uint64_t data,
                                unsigned size)
{
    AspeedSMCState *s = fl->controller;
    uint8_t addr_width = aspeed_smc_flash_is_4byte(fl) ? 4 : 3;

    if (s->snoop_index == SNOOP_OFF) {
        return false; /* Do nothing */

    } else if (s->snoop_index == SNOOP_START) {
        uint8_t cmd = data & 0xff;
        int ndummies = aspeed_smc_num_dummies(cmd);

        /*
         * No dummy cycles are expected with the current command. Turn
         * off snooping and let the transfer proceed normally.
         */
        if (ndummies <= 0) {
            s->snoop_index = SNOOP_OFF;
            return false;
        }

        s->snoop_dummies = ndummies * 8;

    } else if (s->snoop_index >= addr_width + 1) {

        /* The SPI transfer has reached the dummy cycles sequence */
        for (; s->snoop_dummies; s->snoop_dummies--) {
            ssi_transfer(s->spi, s->regs[R_DUMMY_DATA] & 0xff);
        }

        /* If no more dummy cycles are expected, turn off snooping */
        if (!s->snoop_dummies) {
            s->snoop_index = SNOOP_OFF;
        } else {
            s->snoop_index += size;
        }

        /*
         * Dummy cycles have been faked already. Ignore the current
         * SPI transfer
         */
        return true;
    }

    s->snoop_index += size;
    return false;
}

static void aspeed_smc_flash_write(void *opaque, hwaddr addr, uint64_t data,
                                   unsigned size)
{
    AspeedSMCFlash *fl = opaque;
    AspeedSMCState *s = fl->controller;
    int i;

    if (!aspeed_smc_is_writable(fl)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: flash is not writable at 0x%"
                      HWADDR_PRIx "\n", __func__, addr);
        return;
    }

    switch (aspeed_smc_flash_mode(fl)) {
    case CTRL_USERMODE:
        if (aspeed_smc_do_snoop(fl, data, size)) {
            break;
        }

        for (i = 0; i < size; i++) {
            ssi_transfer(s->spi, (data >> (8 * i)) & 0xff);
        }
        break;
    case CTRL_WRITEMODE:
        aspeed_smc_flash_select(fl);
        aspeed_smc_flash_setup(fl, addr);

        for (i = 0; i < size; i++) {
            ssi_transfer(s->spi, (data >> (8 * i)) & 0xff);
        }

        aspeed_smc_flash_unselect(fl);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: invalid flash mode %d\n",
                      __func__, aspeed_smc_flash_mode(fl));
    }
}

static const MemoryRegionOps aspeed_smc_flash_ops = {
    .read = aspeed_smc_flash_read,
    .write = aspeed_smc_flash_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static void aspeed_smc_flash_update_cs(AspeedSMCFlash *fl)
{
    AspeedSMCState *s = fl->controller;

    s->snoop_index = aspeed_smc_is_ce_stop_active(fl) ? SNOOP_OFF : SNOOP_START;

    qemu_set_irq(s->cs_lines[fl->id], aspeed_smc_is_ce_stop_active(fl));
}

static void aspeed_smc_reset(DeviceState *d)
{
    AspeedSMCState *s = ASPEED_SMC(d);
    int i;

    memset(s->regs, 0, sizeof s->regs);

    /* Unselect all slaves */
    for (i = 0; i < s->num_cs; ++i) {
        s->regs[s->r_ctrl0 + i] |= CTRL_CE_STOP_ACTIVE;
        qemu_set_irq(s->cs_lines[i], true);
    }

    /* setup default segment register values for all */
    for (i = 0; i < s->ctrl->max_slaves; ++i) {
        s->regs[R_SEG_ADDR0 + i] =
            s->ctrl->segment_to_reg(s, &s->ctrl->segments[i]);
    }

    /* HW strapping flash type for the AST2600 controllers  */
    if (s->ctrl->segments == aspeed_segments_ast2600_fmc) {
        /* flash type is fixed to SPI for all */
        s->regs[s->r_conf] |= (CONF_FLASH_TYPE_SPI << CONF_FLASH_TYPE0);
        s->regs[s->r_conf] |= (CONF_FLASH_TYPE_SPI << CONF_FLASH_TYPE1);
        s->regs[s->r_conf] |= (CONF_FLASH_TYPE_SPI << CONF_FLASH_TYPE2);
    }

    /* HW strapping flash type for FMC controllers  */
    if (s->ctrl->segments == aspeed_segments_ast2500_fmc) {
        /* flash type is fixed to SPI for CE0 and CE1 */
        s->regs[s->r_conf] |= (CONF_FLASH_TYPE_SPI << CONF_FLASH_TYPE0);
        s->regs[s->r_conf] |= (CONF_FLASH_TYPE_SPI << CONF_FLASH_TYPE1);
    }

    /* HW strapping for AST2400 FMC controllers (SCU70). Let's use the
     * configuration of the palmetto-bmc machine */
    if (s->ctrl->segments == aspeed_segments_fmc) {
        s->regs[s->r_conf] |= (CONF_FLASH_TYPE_SPI << CONF_FLASH_TYPE0);
    }

    s->snoop_index = SNOOP_OFF;
    s->snoop_dummies = 0;
}

static uint64_t aspeed_smc_read(void *opaque, hwaddr addr, unsigned int size)
{
    AspeedSMCState *s = ASPEED_SMC(opaque);

    addr >>= 2;

    if (addr == s->r_conf ||
        addr == s->r_timings ||
        addr == s->r_ce_ctrl ||
        addr == R_INTR_CTRL ||
        addr == R_DUMMY_DATA ||
        (s->ctrl->has_dma && addr == R_DMA_CTRL) ||
        (s->ctrl->has_dma && addr == R_DMA_FLASH_ADDR) ||
        (s->ctrl->has_dma && addr == R_DMA_DRAM_ADDR) ||
        (s->ctrl->has_dma && addr == R_DMA_LEN) ||
        (s->ctrl->has_dma && addr == R_DMA_CHECKSUM) ||
        (addr >= R_SEG_ADDR0 && addr < R_SEG_ADDR0 + s->ctrl->max_slaves) ||
        (addr >= s->r_ctrl0 && addr < s->r_ctrl0 + s->ctrl->max_slaves)) {
        return s->regs[addr];
    } else {
        qemu_log_mask(LOG_UNIMP, "%s: not implemented: 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        return -1;
    }
}

static uint8_t aspeed_smc_hclk_divisor(uint8_t hclk_mask)
{
    /* HCLK/1 .. HCLK/16 */
    const uint8_t hclk_divisors[] = {
        15, 7, 14, 6, 13, 5, 12, 4, 11, 3, 10, 2, 9, 1, 8, 0
    };
    int i;

    for (i = 0; i < ARRAY_SIZE(hclk_divisors); i++) {
        if (hclk_mask == hclk_divisors[i]) {
            return i + 1;
        }
    }

    qemu_log_mask(LOG_GUEST_ERROR, "invalid HCLK mask %x", hclk_mask);
    return 0;
}

/*
 * When doing calibration, the SPI clock rate in the CE0 Control
 * Register and the read delay cycles in the Read Timing Compensation
 * Register are set using bit[11:4] of the DMA Control Register.
 */
static void aspeed_smc_dma_calibration(AspeedSMCState *s)
{
    uint8_t delay =
        (s->regs[R_DMA_CTRL] >> DMA_CTRL_DELAY_SHIFT) & DMA_CTRL_DELAY_MASK;
    uint8_t hclk_mask =
        (s->regs[R_DMA_CTRL] >> DMA_CTRL_FREQ_SHIFT) & DMA_CTRL_FREQ_MASK;
    uint8_t hclk_div = aspeed_smc_hclk_divisor(hclk_mask);
    uint32_t hclk_shift = (hclk_div - 1) << 2;
    uint8_t cs;

    /*
     * The Read Timing Compensation Register values apply to all CS on
     * the SPI bus and only HCLK/1 - HCLK/5 can have tunable delays
     */
    if (hclk_div && hclk_div < 6) {
        s->regs[s->r_timings] &= ~(0xf << hclk_shift);
        s->regs[s->r_timings] |= delay << hclk_shift;
    }

    /*
     * TODO: compute the CS from the DMA address and the segment
     * registers. This is not really a problem for now because the
     * Timing Register values apply to all CS and software uses CS0 to
     * do calibration.
     */
    cs = 0;
    s->regs[s->r_ctrl0 + cs] &=
        ~(CE_CTRL_CLOCK_FREQ_MASK << CE_CTRL_CLOCK_FREQ_SHIFT);
    s->regs[s->r_ctrl0 + cs] |= CE_CTRL_CLOCK_FREQ(hclk_div);
}

/*
 * Emulate read errors in the DMA Checksum Register for high
 * frequencies and optimistic settings of the Read Timing Compensation
 * Register. This will help in tuning the SPI timing calibration
 * algorithm.
 */
static bool aspeed_smc_inject_read_failure(AspeedSMCState *s)
{
    uint8_t delay =
        (s->regs[R_DMA_CTRL] >> DMA_CTRL_DELAY_SHIFT) & DMA_CTRL_DELAY_MASK;
    uint8_t hclk_mask =
        (s->regs[R_DMA_CTRL] >> DMA_CTRL_FREQ_SHIFT) & DMA_CTRL_FREQ_MASK;

    /*
     * Typical values of a palmetto-bmc machine.
     */
    switch (aspeed_smc_hclk_divisor(hclk_mask)) {
    case 4 ... 16:
        return false;
    case 3: /* at least one HCLK cycle delay */
        return (delay & 0x7) < 1;
    case 2: /* at least two HCLK cycle delay */
        return (delay & 0x7) < 2;
    case 1: /* (> 100MHz) is above the max freq of the controller */
        return true;
    default:
        g_assert_not_reached();
    }
}

/*
 * Accumulate the result of the reads to provide a checksum that will
 * be used to validate the read timing settings.
 */
static void aspeed_smc_dma_checksum(AspeedSMCState *s)
{
    MemTxResult result;
    uint32_t data;

    if (s->regs[R_DMA_CTRL] & DMA_CTRL_WRITE) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: invalid direction for DMA checksum\n",  __func__);
        return;
    }

    if (s->regs[R_DMA_CTRL] & DMA_CTRL_CALIB) {
        aspeed_smc_dma_calibration(s);
    }

    while (s->regs[R_DMA_LEN]) {
        data = address_space_ldl_le(&s->flash_as, s->regs[R_DMA_FLASH_ADDR],
                                    MEMTXATTRS_UNSPECIFIED, &result);
        if (result != MEMTX_OK) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: Flash read failed @%08x\n",
                          __func__, s->regs[R_DMA_FLASH_ADDR]);
            return;
        }

        /*
         * When the DMA is on-going, the DMA registers are updated
         * with the current working addresses and length.
         */
        s->regs[R_DMA_CHECKSUM] += data;
        s->regs[R_DMA_FLASH_ADDR] += 4;
        s->regs[R_DMA_LEN] -= 4;
    }

    if (s->inject_failure && aspeed_smc_inject_read_failure(s)) {
        s->regs[R_DMA_CHECKSUM] = 0xbadc0de;
    }

}

static void aspeed_smc_dma_rw(AspeedSMCState *s)
{
    MemTxResult result;
    uint32_t data;

    while (s->regs[R_DMA_LEN]) {
        if (s->regs[R_DMA_CTRL] & DMA_CTRL_WRITE) {
            data = address_space_ldl_le(&s->dram_as, s->regs[R_DMA_DRAM_ADDR],
                                        MEMTXATTRS_UNSPECIFIED, &result);
            if (result != MEMTX_OK) {
                qemu_log_mask(LOG_GUEST_ERROR, "%s: DRAM read failed @%08x\n",
                              __func__, s->regs[R_DMA_DRAM_ADDR]);
                return;
            }

            address_space_stl_le(&s->flash_as, s->regs[R_DMA_FLASH_ADDR],
                                 data, MEMTXATTRS_UNSPECIFIED, &result);
            if (result != MEMTX_OK) {
                qemu_log_mask(LOG_GUEST_ERROR, "%s: Flash write failed @%08x\n",
                              __func__, s->regs[R_DMA_FLASH_ADDR]);
                return;
            }
        } else {
            data = address_space_ldl_le(&s->flash_as, s->regs[R_DMA_FLASH_ADDR],
                                        MEMTXATTRS_UNSPECIFIED, &result);
            if (result != MEMTX_OK) {
                qemu_log_mask(LOG_GUEST_ERROR, "%s: Flash read failed @%08x\n",
                              __func__, s->regs[R_DMA_FLASH_ADDR]);
                return;
            }

            address_space_stl_le(&s->dram_as, s->regs[R_DMA_DRAM_ADDR],
                                 data, MEMTXATTRS_UNSPECIFIED, &result);
            if (result != MEMTX_OK) {
                qemu_log_mask(LOG_GUEST_ERROR, "%s: DRAM write failed @%08x\n",
                              __func__, s->regs[R_DMA_DRAM_ADDR]);
                return;
            }
        }

        /*
         * When the DMA is on-going, the DMA registers are updated
         * with the current working addresses and length.
         */
        s->regs[R_DMA_FLASH_ADDR] += 4;
        s->regs[R_DMA_DRAM_ADDR] += 4;
        s->regs[R_DMA_LEN] -= 4;
        s->regs[R_DMA_CHECKSUM] += data;
    }
}

static void aspeed_smc_dma_stop(AspeedSMCState *s)
{
    /*
     * When the DMA is disabled, INTR_CTRL_DMA_STATUS=0 means the
     * engine is idle
     */
    s->regs[R_INTR_CTRL] &= ~INTR_CTRL_DMA_STATUS;
    s->regs[R_DMA_CHECKSUM] = 0;

    /*
     * Lower the DMA irq in any case. The IRQ control register could
     * have been cleared before disabling the DMA.
     */
    qemu_irq_lower(s->irq);
}

/*
 * When INTR_CTRL_DMA_STATUS=1, the DMA has completed and a new DMA
 * can start even if the result of the previous was not collected.
 */
static bool aspeed_smc_dma_in_progress(AspeedSMCState *s)
{
    return s->regs[R_DMA_CTRL] & DMA_CTRL_ENABLE &&
        !(s->regs[R_INTR_CTRL] & INTR_CTRL_DMA_STATUS);
}

static void aspeed_smc_dma_done(AspeedSMCState *s)
{
    s->regs[R_INTR_CTRL] |= INTR_CTRL_DMA_STATUS;
    if (s->regs[R_INTR_CTRL] & INTR_CTRL_DMA_EN) {
        qemu_irq_raise(s->irq);
    }
}

static void aspeed_smc_dma_ctrl(AspeedSMCState *s, uint64_t dma_ctrl)
{
    if (!(dma_ctrl & DMA_CTRL_ENABLE)) {
        s->regs[R_DMA_CTRL] = dma_ctrl;

        aspeed_smc_dma_stop(s);
        return;
    }

    if (aspeed_smc_dma_in_progress(s)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: DMA in progress\n",  __func__);
        return;
    }

    s->regs[R_DMA_CTRL] = dma_ctrl;

    if (s->regs[R_DMA_CTRL] & DMA_CTRL_CKSUM) {
        aspeed_smc_dma_checksum(s);
    } else {
        aspeed_smc_dma_rw(s);
    }

    aspeed_smc_dma_done(s);
}

static void aspeed_smc_write(void *opaque, hwaddr addr, uint64_t data,
                             unsigned int size)
{
    AspeedSMCState *s = ASPEED_SMC(opaque);
    uint32_t value = data;

    addr >>= 2;

    if (addr == s->r_conf ||
        addr == s->r_timings ||
        addr == s->r_ce_ctrl) {
        s->regs[addr] = value;
    } else if (addr >= s->r_ctrl0 && addr < s->r_ctrl0 + s->num_cs) {
        int cs = addr - s->r_ctrl0;
        s->regs[addr] = value;
        aspeed_smc_flash_update_cs(&s->flashes[cs]);
    } else if (addr >= R_SEG_ADDR0 &&
               addr < R_SEG_ADDR0 + s->ctrl->max_slaves) {
        int cs = addr - R_SEG_ADDR0;

        if (value != s->regs[R_SEG_ADDR0 + cs]) {
            aspeed_smc_flash_set_segment(s, cs, value);
        }
    } else if (addr == R_DUMMY_DATA) {
        s->regs[addr] = value & 0xff;
    } else if (addr == R_INTR_CTRL) {
        s->regs[addr] = value;
    } else if (s->ctrl->has_dma && addr == R_DMA_CTRL) {
        aspeed_smc_dma_ctrl(s, value);
    } else if (s->ctrl->has_dma && addr == R_DMA_DRAM_ADDR) {
        s->regs[addr] = DMA_DRAM_ADDR(s, value);
    } else if (s->ctrl->has_dma && addr == R_DMA_FLASH_ADDR) {
        s->regs[addr] = DMA_FLASH_ADDR(s, value);
    } else if (s->ctrl->has_dma && addr == R_DMA_LEN) {
        s->regs[addr] = DMA_LENGTH(value);
    } else {
        qemu_log_mask(LOG_UNIMP, "%s: not implemented: 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        return;
    }
}

static const MemoryRegionOps aspeed_smc_ops = {
    .read = aspeed_smc_read,
    .write = aspeed_smc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.unaligned = true,
};


/*
 * Initialize the custom address spaces for DMAs
 */
static void aspeed_smc_dma_setup(AspeedSMCState *s, Error **errp)
{
    char *name;

    if (!s->dram_mr) {
        error_setg(errp, TYPE_ASPEED_SMC ": 'dram' link not set");
        return;
    }

    name = g_strdup_printf("%s-dma-flash", s->ctrl->name);
    address_space_init(&s->flash_as, &s->mmio_flash, name);
    g_free(name);

    name = g_strdup_printf("%s-dma-dram", s->ctrl->name);
    address_space_init(&s->dram_as, s->dram_mr, name);
    g_free(name);
}

static void aspeed_smc_realize(DeviceState *dev, Error **errp)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    AspeedSMCState *s = ASPEED_SMC(dev);
    AspeedSMCClass *mc = ASPEED_SMC_GET_CLASS(s);
    int i;
    char name[32];
    hwaddr offset = 0;

    s->ctrl = mc->ctrl;

    /* keep a copy under AspeedSMCState to speed up accesses */
    s->r_conf = s->ctrl->r_conf;
    s->r_ce_ctrl = s->ctrl->r_ce_ctrl;
    s->r_ctrl0 = s->ctrl->r_ctrl0;
    s->r_timings = s->ctrl->r_timings;
    s->conf_enable_w0 = s->ctrl->conf_enable_w0;

    /* Enforce some real HW limits */
    if (s->num_cs > s->ctrl->max_slaves) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: num_cs cannot exceed: %d\n",
                      __func__, s->ctrl->max_slaves);
        s->num_cs = s->ctrl->max_slaves;
    }

    /* DMA irq. Keep it first for the initialization in the SoC */
    sysbus_init_irq(sbd, &s->irq);

    s->spi = ssi_create_bus(dev, "spi");

    /* Setup cs_lines for slaves */
    s->cs_lines = g_new0(qemu_irq, s->num_cs);
    ssi_auto_connect_slaves(dev, s->cs_lines, s->spi);

    for (i = 0; i < s->num_cs; ++i) {
        sysbus_init_irq(sbd, &s->cs_lines[i]);
    }

    /* The memory region for the controller registers */
    memory_region_init_io(&s->mmio, OBJECT(s), &aspeed_smc_ops, s,
                          s->ctrl->name, s->ctrl->nregs * 4);
    sysbus_init_mmio(sbd, &s->mmio);

    /*
     * The container memory region representing the address space
     * window in which the flash modules are mapped. The size and
     * address depends on the SoC model and controller type.
     */
    snprintf(name, sizeof(name), "%s.flash", s->ctrl->name);

    memory_region_init_io(&s->mmio_flash, OBJECT(s),
                          &aspeed_smc_flash_default_ops, s, name,
                          s->ctrl->flash_window_size);
    sysbus_init_mmio(sbd, &s->mmio_flash);

    s->flashes = g_new0(AspeedSMCFlash, s->ctrl->max_slaves);

    /*
     * Let's create a sub memory region for each possible slave. All
     * have a configurable memory segment in the overall flash mapping
     * window of the controller but, there is not necessarily a flash
     * module behind to handle the memory accesses. This depends on
     * the board configuration.
     */
    for (i = 0; i < s->ctrl->max_slaves; ++i) {
        AspeedSMCFlash *fl = &s->flashes[i];

        snprintf(name, sizeof(name), "%s.%d", s->ctrl->name, i);

        fl->id = i;
        fl->controller = s;
        fl->size = s->ctrl->segments[i].size;
        memory_region_init_io(&fl->mmio, OBJECT(s), &aspeed_smc_flash_ops,
                              fl, name, fl->size);
        memory_region_add_subregion(&s->mmio_flash, offset, &fl->mmio);
        offset += fl->size;
    }

    /* DMA support */
    if (s->ctrl->has_dma) {
        aspeed_smc_dma_setup(s, errp);
    }
}

static const VMStateDescription vmstate_aspeed_smc = {
    .name = "aspeed.smc",
    .version_id = 2,
    .minimum_version_id = 2,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, AspeedSMCState, ASPEED_SMC_R_MAX),
        VMSTATE_UINT8(snoop_index, AspeedSMCState),
        VMSTATE_UINT8(snoop_dummies, AspeedSMCState),
        VMSTATE_END_OF_LIST()
    }
};

static Property aspeed_smc_properties[] = {
    DEFINE_PROP_UINT32("num-cs", AspeedSMCState, num_cs, 1),
    DEFINE_PROP_BOOL("inject-failure", AspeedSMCState, inject_failure, false),
    DEFINE_PROP_UINT64("sdram-base", AspeedSMCState, sdram_base, 0),
    DEFINE_PROP_LINK("dram", AspeedSMCState, dram_mr,
                     TYPE_MEMORY_REGION, MemoryRegion *),
    DEFINE_PROP_END_OF_LIST(),
};

static void aspeed_smc_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    AspeedSMCClass *mc = ASPEED_SMC_CLASS(klass);

    dc->realize = aspeed_smc_realize;
    dc->reset = aspeed_smc_reset;
    dc->props = aspeed_smc_properties;
    dc->vmsd = &vmstate_aspeed_smc;
    mc->ctrl = data;
}

static const TypeInfo aspeed_smc_info = {
    .name           = TYPE_ASPEED_SMC,
    .parent         = TYPE_SYS_BUS_DEVICE,
    .instance_size  = sizeof(AspeedSMCState),
    .class_size     = sizeof(AspeedSMCClass),
    .abstract       = true,
};

static void aspeed_smc_register_types(void)
{
    int i;

    type_register_static(&aspeed_smc_info);
    for (i = 0; i < ARRAY_SIZE(controllers); ++i) {
        TypeInfo ti = {
            .name       = controllers[i].name,
            .parent     = TYPE_ASPEED_SMC,
            .class_init = aspeed_smc_class_init,
            .class_data = (void *)&controllers[i],
        };
        type_register(&ti);
    }
}

type_init(aspeed_smc_register_types)
