/*
 * OpenPOWER Palmetto BMC
 *
 * Andrew Jeffery <andrew@aj.id.au>
 *
 * Copyright 2016 IBM Corp.
 *
 * This code is licensed under the GPL version 2 or later.  See
 * the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "cpu.h"
#include "exec/address-spaces.h"
#include "hw/arm/boot.h"
#include "hw/arm/aspeed.h"
#include "hw/arm/aspeed_soc.h"
#include "hw/boards.h"
#include "hw/i2c/smbus_eeprom.h"
#include "hw/misc/pca9552.h"
#include "hw/misc/tmp105.h"
#include "hw/qdev-properties.h"
#include "qemu/log.h"
#include "sysemu/block-backend.h"
#include "sysemu/sysemu.h"
#include "sysemu/reset.h"
#include "hw/loader.h"
#include "qemu/error-report.h"
#include "qemu/units.h"

static struct arm_boot_info aspeed_board_binfo = {
    .board_id = -1, /* device-tree-only board */
};

struct AspeedBoardState {
    AspeedSoCState soc;
    MemoryRegion ram_container;
    MemoryRegion ram;
    MemoryRegion max_ram;
};

/* Palmetto hardware value: 0x120CE416 */
#define PALMETTO_BMC_HW_STRAP1 (                                        \
        SCU_AST2400_HW_STRAP_DRAM_SIZE(DRAM_SIZE_256MB) |               \
        SCU_AST2400_HW_STRAP_DRAM_CONFIG(2 /* DDR3 with CL=6, CWL=5 */) | \
        SCU_AST2400_HW_STRAP_ACPI_DIS |                                 \
        SCU_AST2400_HW_STRAP_SET_CLK_SOURCE(AST2400_CLK_48M_IN) |       \
        SCU_HW_STRAP_VGA_CLASS_CODE |                                   \
        SCU_HW_STRAP_LPC_RESET_PIN |                                    \
        SCU_HW_STRAP_SPI_MODE(SCU_HW_STRAP_SPI_M_S_EN) |                \
        SCU_AST2400_HW_STRAP_SET_CPU_AHB_RATIO(AST2400_CPU_AHB_RATIO_2_1) | \
        SCU_HW_STRAP_SPI_WIDTH |                                        \
        SCU_HW_STRAP_VGA_SIZE_SET(VGA_16M_DRAM) |                       \
        SCU_AST2400_HW_STRAP_BOOT_MODE(AST2400_SPI_BOOT))

/* AST2500 evb hardware value: 0xF100C2E6 */
#define AST2500_EVB_HW_STRAP1 ((                                        \
        AST2500_HW_STRAP1_DEFAULTS |                                    \
        SCU_AST2500_HW_STRAP_SPI_AUTOFETCH_ENABLE |                     \
        SCU_AST2500_HW_STRAP_GPIO_STRAP_ENABLE |                        \
        SCU_AST2500_HW_STRAP_UART_DEBUG |                               \
        SCU_AST2500_HW_STRAP_DDR4_ENABLE |                              \
        SCU_HW_STRAP_MAC1_RGMII |                                       \
        SCU_HW_STRAP_MAC0_RGMII) &                                      \
        ~SCU_HW_STRAP_2ND_BOOT_WDT)

/* Romulus hardware value: 0xF10AD206 */
#define ROMULUS_BMC_HW_STRAP1 (                                         \
        AST2500_HW_STRAP1_DEFAULTS |                                    \
        SCU_AST2500_HW_STRAP_SPI_AUTOFETCH_ENABLE |                     \
        SCU_AST2500_HW_STRAP_GPIO_STRAP_ENABLE |                        \
        SCU_AST2500_HW_STRAP_UART_DEBUG |                               \
        SCU_AST2500_HW_STRAP_DDR4_ENABLE |                              \
        SCU_AST2500_HW_STRAP_ACPI_ENABLE |                              \
        SCU_HW_STRAP_SPI_MODE(SCU_HW_STRAP_SPI_MASTER))

/* Swift hardware value: 0xF11AD206 */
#define SWIFT_BMC_HW_STRAP1 (                                           \
        AST2500_HW_STRAP1_DEFAULTS |                                    \
        SCU_AST2500_HW_STRAP_SPI_AUTOFETCH_ENABLE |                     \
        SCU_AST2500_HW_STRAP_GPIO_STRAP_ENABLE |                        \
        SCU_AST2500_HW_STRAP_UART_DEBUG |                               \
        SCU_AST2500_HW_STRAP_DDR4_ENABLE |                              \
        SCU_H_PLL_BYPASS_EN |                                           \
        SCU_AST2500_HW_STRAP_ACPI_ENABLE |                              \
        SCU_HW_STRAP_SPI_MODE(SCU_HW_STRAP_SPI_MASTER))

/* Witherspoon hardware value: 0xF10AD216 (but use romulus definition) */
#define WITHERSPOON_BMC_HW_STRAP1 ROMULUS_BMC_HW_STRAP1

/* AST2600 evb hardware value */
#define AST2600_EVB_HW_STRAP1 0x000000C0
#define AST2600_EVB_HW_STRAP2 0x00000003

/* Tacoma hardware value: (QEMU prototype) */
#define TACOMA_BMC_HW_STRAP1 AST2500_EVB_HW_STRAP1

/* Rainier hardware value: (QEMU prototype) */
#define RAINIER_BMC_HW_STRAP1 AST2500_EVB_HW_STRAP1

/*
 * The max ram region is for firmwares that scan the address space
 * with load/store to guess how much RAM the SoC has.
 */
static uint64_t max_ram_read(void *opaque, hwaddr offset, unsigned size)
{
    return 0;
}

static void max_ram_write(void *opaque, hwaddr offset, uint64_t value,
                           unsigned size)
{
    /* Discard writes */
}

static const MemoryRegionOps max_ram_ops = {
    .read = max_ram_read,
    .write = max_ram_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

/*
 *       SMP mailbox
 * +----------------------+
 * |                      |
 * | mailbox insn. for    |
 * | cpuN polling SMP go  |
 * |                      |
 * +----------------------+ 0xC
 * | mailbox ready signal |
 * +----------------------+ 0x8
 * | cpuN GO signal       |
 * +----------------------+ 0x4
 * | cpuN entrypoint      |
 * +----------------------+ AST_SMP_MAILBOX_BASE
 */

#define AST_SMP_MAILBOX_BASE            0x1E6E2180
#define AST_SMP_MBOX_FIELD_ENTRY        (AST_SMP_MAILBOX_BASE + 0x0)
#define AST_SMP_MBOX_FIELD_GOSIGN       (AST_SMP_MAILBOX_BASE + 0x4)
#define AST_SMP_MBOX_FIELD_READY        (AST_SMP_MAILBOX_BASE + 0x8)
#define AST_SMP_MBOX_FIELD_POLLINSN     (AST_SMP_MAILBOX_BASE + 0xc)

static void aspeed_write_smpboot(ARMCPU *cpu, const struct arm_boot_info *info)
{
    AddressSpace *as = arm_boot_address_space(cpu, info);

    static const uint32_t poll_mailbox_ready[] = {
        0xe320f002,	// wfe
        0xe59f0020,	// ldr	r0, [pc, #32]	; 2c <poll_mailbox_ready+0x2c>
        0xe59f1020,	// ldr	r1, [pc, #32]	; 30 <poll_mailbox_ready+0x30>
        0xe5902000,	// ldr	r2, [r0]
        0xe1510002,	// cmp	r1, r2
        0x1afffff9,	// bne	0 <poll_mailbox_ready>
        0xe59f0014,	// ldr	r0, [pc, #20]	; 34 <poll_mailbox_ready+0x34>
        0xe59f1014,	// ldr	r1, [pc, #20]	; 38 <poll_mailbox_ready+0x38>
        0xe59f2014,	// ldr	r2, [pc, #20]	; 3c <poll_mailbox_ready+0x3c>
        0xe59f3014,	// ldr	r3, [pc, #20]	; 40 <poll_mailbox_ready+0x40>
        0xe59ff014,	// ldr	pc, [pc, #20]	; 44 <poll_mailbox_ready+0x44>
        0x1e6e2188,	// .word	0x1e6e2188
        0xbabecafe,	// .word	0xbabecafe
        0x1e6e2184,	// .word	0x1e6e2184
        0x1e6e2180,	// .word	0x1e6e2180
        0xabbaadda,	// .word	0xabbaadda
        0x1e784000,	// .word	0x1e784000
        0x1e6e218c,	// .word	0x1e6e218c
    };

    printf("%s: loading to %08lx\n", __func__, info->smp_loader_start);

    if (rom_add_blob_fixed_as("ast2600_smpboot", poll_mailbox_ready,
                       sizeof(poll_mailbox_ready),
                       info->smp_loader_start, as) < 0) {
        printf("%s: failed to set SMP boot rom", __func__);
        return;
    }
}

#define FIRMWARE_ADDR 0x0

static void write_boot_rom(DriveInfo *dinfo, hwaddr addr, size_t rom_size,
                           Error **errp)
{
    BlockBackend *blk = blk_by_legacy_dinfo(dinfo);
    uint8_t *storage;
    int64_t size;

    /* The block backend size should have already been 'validated' by
     * the creation of the m25p80 object.
     */
    size = blk_getlength(blk);
    if (size <= 0) {
        error_setg(errp, "failed to get flash size");
        return;
    }

    if (rom_size > size) {
        rom_size = size;
    }

    storage = g_new0(uint8_t, rom_size);
    if (blk_pread(blk, 0, storage, rom_size) < 0) {
        error_setg(errp, "failed to read the initial flash content");
        return;
    }

    rom_add_blob_fixed("aspeed.boot_rom", storage, rom_size, addr);
    g_free(storage);
}

static void ast2600_evb_reset(MachineState *machine)
{
    Error *err = NULL;
    Object *obj;
    AspeedSoCState *s;

    qemu_devices_reset();

    obj = object_property_get_link(OBJECT(machine), "soc", &err);

    if (obj == NULL) {
        error_set(&err, ERROR_CLASS_DEVICE_NOT_FOUND,
                  "SoC not found");
        return;
    }
    s = ASPEED_SOC(obj);
    /* init gpios */
    object_property_set_bool(OBJECT(&s->gpio), true, "gpioA0", &error_abort);
    object_property_set_bool(OBJECT(&s->gpio), true, "gpioA7", &error_abort);
}

static void aspeed_board_init_flashes(AspeedSMCState *s, const char *flashtype,
                                      Error **errp)
{
    int i ;

    for (i = 0; i < s->num_cs; ++i) {
        AspeedSMCFlash *fl = &s->flashes[i];
        DriveInfo *dinfo = drive_get_next(IF_MTD);
        qemu_irq cs_line;

        fl->flash = ssi_create_slave_no_init(s->spi, flashtype);
        if (dinfo) {
            qdev_prop_set_drive(fl->flash, "drive", blk_by_legacy_dinfo(dinfo),
                                errp);
        }
        qdev_init_nofail(fl->flash);

        cs_line = qdev_get_gpio_in_named(fl->flash, SSI_GPIO_CS, 0);
        sysbus_connect_irq(SYS_BUS_DEVICE(s), i + 1, cs_line);
    }
}

static void aspeed_board_init(MachineState *machine)
{
    AspeedBoardState *bmc;
    AspeedSoCClass *sc;
    DriveInfo *drive0 = drive_get(IF_MTD, 0, 0);
    AspeedMachineClass *amc = ASPEED_MACHINE_GET_CLASS(machine);
    ram_addr_t max_ram_size;
    int i;

    bmc = g_new0(AspeedBoardState, 1);

    memory_region_init(&bmc->ram_container, NULL, "aspeed-ram-container",
                       UINT32_MAX);

    object_initialize_child(OBJECT(machine), "soc", &bmc->soc,
                            (sizeof(bmc->soc)), amc->soc_name, &error_abort,
                            NULL);

    sc = ASPEED_SOC_GET_CLASS(&bmc->soc);

    object_property_set_uint(OBJECT(&bmc->soc), ram_size, "ram-size",
                             &error_abort);
    object_property_set_int(OBJECT(&bmc->soc), amc->hw_strap1, "hw-strap1",
                            &error_abort);
    object_property_set_int(OBJECT(&bmc->soc), amc->hw_strap2, "hw-strap2",
                            &error_abort);
    object_property_set_int(OBJECT(&bmc->soc), amc->num_cs, "num-cs",
                            &error_abort);
    object_property_set_int(OBJECT(&bmc->soc), machine->smp.cpus, "num-cpus",
                            &error_abort);
    object_property_set_link(OBJECT(&bmc->soc), OBJECT(&bmc->ram_container),
                             "dram", &error_abort);
    if (machine->kernel_filename) {
        /*
         * When booting with a -kernel command line there is no u-boot
         * that runs to unlock the SCU. In this case set the default to
         * be unlocked as the kernel expects
         */
        object_property_set_int(OBJECT(&bmc->soc), ASPEED_SCU_PROT_KEY,
                                "hw-prot-key", &error_abort);
    }
    object_property_set_bool(OBJECT(&bmc->soc), true, "realized",
                             &error_abort);

    /*
     * Allocate RAM after the memory controller has checked the size
     * was valid. If not, a default value is used.
     */
    ram_size = object_property_get_uint(OBJECT(&bmc->soc), "ram-size",
                                        &error_abort);

    memory_region_allocate_system_memory(&bmc->ram, NULL, "ram", ram_size);
    memory_region_add_subregion(&bmc->ram_container, 0, &bmc->ram);
    memory_region_add_subregion(get_system_memory(),
                                sc->memmap[ASPEED_SDRAM],
                                &bmc->ram_container);

    max_ram_size = object_property_get_uint(OBJECT(&bmc->soc), "max-ram-size",
                                            &error_abort);
    memory_region_init_io(&bmc->max_ram, NULL, &max_ram_ops, NULL,
                          "max_ram", max_ram_size  - ram_size);
    memory_region_add_subregion(&bmc->ram_container, ram_size, &bmc->max_ram);

    aspeed_board_init_flashes(&bmc->soc.fmc, amc->fmc_model, &error_abort);
    aspeed_board_init_flashes(&bmc->soc.spi[0], amc->spi_model, &error_abort);

    /* Install first FMC flash content as a boot rom. */
    if (drive0) {
        AspeedSMCFlash *fl = &bmc->soc.fmc.flashes[0];
        MemoryRegion *boot_rom = g_new(MemoryRegion, 1);

        /*
         * create a ROM region using the default mapping window size of
         * the flash module. The window size is 64MB for the AST2400
         * SoC and 128MB for the AST2500 SoC, which is twice as big as
         * needed by the flash modules of the Aspeed machines.
         */
        if (ASPEED_MACHINE(machine)->mmio_exec) {
            memory_region_init_alias(boot_rom, OBJECT(bmc), "aspeed.boot_rom",
                                     &fl->mmio, 0, fl->size);
            memory_region_add_subregion(get_system_memory(), FIRMWARE_ADDR,
                                        boot_rom);
        } else {
            memory_region_init_rom(boot_rom, OBJECT(bmc), "aspeed.boot_rom",
                                   fl->size, &error_abort);
            memory_region_add_subregion(get_system_memory(), FIRMWARE_ADDR,
                                        boot_rom);
            write_boot_rom(drive0, FIRMWARE_ADDR, fl->size, &error_abort);
        }
    }

    aspeed_board_binfo.ram_size = ram_size;
    aspeed_board_binfo.loader_start = sc->memmap[ASPEED_SDRAM];
    aspeed_board_binfo.nb_cpus = bmc->soc.num_cpus;
    aspeed_board_binfo.write_secondary_boot = aspeed_write_smpboot;

    if (amc->i2c_init) {
        amc->i2c_init(bmc);
    }

    for (i = 0; i < ARRAY_SIZE(bmc->soc.sdhci.slots); i++) {
        SDHCIState *sdhci = &bmc->soc.sdhci.slots[i];
        DriveInfo *dinfo = drive_get_next(IF_SD);
        BlockBackend *blk;
        DeviceState *card;

        blk = dinfo ? blk_by_legacy_dinfo(dinfo) : NULL;
        card = qdev_create(qdev_get_child_bus(DEVICE(sdhci), "sd-bus"),
                           TYPE_SD_CARD);
        qdev_prop_set_drive(card, "drive", blk, &error_fatal);
        object_property_set_bool(OBJECT(card), true, "realized", &error_fatal);
    }

    arm_load_kernel(ARM_CPU(first_cpu), machine, &aspeed_board_binfo);
}

static void palmetto_bmc_i2c_init(AspeedBoardState *bmc)
{
    AspeedSoCState *soc = &bmc->soc;
    DeviceState *dev;
    uint8_t *eeprom_buf = g_malloc0(32 * 1024);

    /* The palmetto platform expects a ds3231 RTC but a ds1338 is
     * enough to provide basic RTC features. Alarms will be missing */
    i2c_create_slave(aspeed_i2c_get_bus(DEVICE(&soc->i2c), 0), "ds1338", 0x68);

    smbus_eeprom_init_one(aspeed_i2c_get_bus(DEVICE(&soc->i2c), 0), 0x50,
                          eeprom_buf);

    /* add a TMP423 temperature sensor */
    dev = i2c_create_slave(aspeed_i2c_get_bus(DEVICE(&soc->i2c), 2),
                           "tmp423", 0x4c);
    object_property_set_int(OBJECT(dev), 31000, "temperature0", &error_abort);
    object_property_set_int(OBJECT(dev), 28000, "temperature1", &error_abort);
    object_property_set_int(OBJECT(dev), 20000, "temperature2", &error_abort);
    object_property_set_int(OBJECT(dev), 110000, "temperature3", &error_abort);
}

static void ast2500_evb_i2c_init(AspeedBoardState *bmc)
{
    AspeedSoCState *soc = &bmc->soc;
    uint8_t *eeprom_buf = g_malloc0(8 * 1024);

    smbus_eeprom_init_one(aspeed_i2c_get_bus(DEVICE(&soc->i2c), 3), 0x50,
                          eeprom_buf);

    /* The AST2500 EVB expects a LM75 but a TMP105 is compatible */
    i2c_create_slave(aspeed_i2c_get_bus(DEVICE(&soc->i2c), 7),
                     TYPE_TMP105, 0x4d);

    /* The AST2500 EVB does not have an RTC. Let's pretend that one is
     * plugged on the I2C bus header */
    i2c_create_slave(aspeed_i2c_get_bus(DEVICE(&soc->i2c), 11), "ds1338", 0x32);
}

static void ast2600_evb_i2c_init(AspeedBoardState *bmc)
{
    /* Start with some devices on our I2C busses */
    ast2500_evb_i2c_init(bmc);
}

static void romulus_bmc_i2c_init(AspeedBoardState *bmc)
{
    AspeedSoCState *soc = &bmc->soc;

    /* The romulus board expects Epson RX8900 I2C RTC but a ds1338 is
     * good enough */
    i2c_create_slave(aspeed_i2c_get_bus(DEVICE(&soc->i2c), 11), "ds1338", 0x32);
}

static void swift_bmc_i2c_init(AspeedBoardState *bmc)
{
    AspeedSoCState *soc = &bmc->soc;

    i2c_create_slave(aspeed_i2c_get_bus(DEVICE(&soc->i2c), 3), "pca9552", 0x60);

    /* The swift board expects a TMP275 but a TMP105 is compatible */
    i2c_create_slave(aspeed_i2c_get_bus(DEVICE(&soc->i2c), 7), "tmp105", 0x48);
    /* The swift board expects a pca9551 but a pca9552 is compatible */
    i2c_create_slave(aspeed_i2c_get_bus(DEVICE(&soc->i2c), 7), "pca9552", 0x60);

    /* The swift board expects an Epson RX8900 RTC but a ds1338 is compatible */
    i2c_create_slave(aspeed_i2c_get_bus(DEVICE(&soc->i2c), 8), "ds1338", 0x32);
    i2c_create_slave(aspeed_i2c_get_bus(DEVICE(&soc->i2c), 8), "pca9552", 0x60);

    i2c_create_slave(aspeed_i2c_get_bus(DEVICE(&soc->i2c), 9), "tmp423", 0x4c);
    /* The swift board expects a pca9539 but a pca9552 is compatible */
    i2c_create_slave(aspeed_i2c_get_bus(DEVICE(&soc->i2c), 9), "pca9552", 0x74);

    i2c_create_slave(aspeed_i2c_get_bus(DEVICE(&soc->i2c), 10), "tmp423", 0x4c);
    /* The swift board expects a pca9539 but a pca9552 is compatible */
    i2c_create_slave(aspeed_i2c_get_bus(DEVICE(&soc->i2c), 10), "pca9552",
                     0x74);

    /* The swift board expects a TMP275 but a TMP105 is compatible */
    i2c_create_slave(aspeed_i2c_get_bus(DEVICE(&soc->i2c), 12), "tmp105", 0x48);
    i2c_create_slave(aspeed_i2c_get_bus(DEVICE(&soc->i2c), 12), "tmp105", 0x4a);
}

static void witherspoon_bmc_i2c_init(AspeedBoardState *bmc)
{
    AspeedSoCState *soc = &bmc->soc;
    uint8_t *eeprom_buf = g_malloc0(8 * 1024);

    i2c_create_slave(aspeed_i2c_get_bus(DEVICE(&soc->i2c), 3), TYPE_PCA9552,
                     0x60);
    i2c_create_slave(aspeed_i2c_get_bus(DEVICE(&soc->i2c), 3), "ibm-cffps", 0x68);
    i2c_create_slave(aspeed_i2c_get_bus(DEVICE(&soc->i2c), 3), "ibm-cffps", 0x69);

    i2c_create_slave(aspeed_i2c_get_bus(DEVICE(&soc->i2c), 4), "tmp423", 0x4c);
    i2c_create_slave(aspeed_i2c_get_bus(DEVICE(&soc->i2c), 4), "ir35221", 0x70);
    i2c_create_slave(aspeed_i2c_get_bus(DEVICE(&soc->i2c), 4), "ir35221", 0x71);

    i2c_create_slave(aspeed_i2c_get_bus(DEVICE(&soc->i2c), 5), "tmp423", 0x4c);
    i2c_create_slave(aspeed_i2c_get_bus(DEVICE(&soc->i2c), 5), "ir35221", 0x70);
    i2c_create_slave(aspeed_i2c_get_bus(DEVICE(&soc->i2c), 5), "ir35221", 0x71);

    /* The Witherspoon expects a TMP275 but a TMP105 is compatible */
    i2c_create_slave(aspeed_i2c_get_bus(DEVICE(&soc->i2c), 9), TYPE_TMP105,
                     0x4a);

    /* The witherspoon board expects Epson RX8900 I2C RTC but a ds1338 is
     * good enough */
    i2c_create_slave(aspeed_i2c_get_bus(DEVICE(&soc->i2c), 11), "ds1338", 0x32);

    smbus_eeprom_init_one(aspeed_i2c_get_bus(DEVICE(&soc->i2c), 11), 0x51,
                          eeprom_buf);
    i2c_create_slave(aspeed_i2c_get_bus(DEVICE(&soc->i2c), 11), TYPE_PCA9552,
                     0x60);
}

static void tacoma_bmc_i2c_init(AspeedBoardState *bmc)
{
    AspeedSoCState *soc = &bmc->soc;
    uint8_t *eeprom_buf = g_malloc0(8 * 1024);

    /* Bus 3: TODO bmp280@77 */
    /* Bus 3: TODO max31785@52 */
    /* Bus 3: TODO dps310@76 */
    i2c_create_slave(aspeed_i2c_get_bus(DEVICE(&soc->i2c), 3), "pca9552", 0x60);
    i2c_create_slave(aspeed_i2c_get_bus(DEVICE(&soc->i2c), 3), "ibm-cffps",
                     0x68);
    i2c_create_slave(aspeed_i2c_get_bus(DEVICE(&soc->i2c), 3), "ibm-cffps",
                     0x69);

    i2c_create_slave(aspeed_i2c_get_bus(DEVICE(&soc->i2c), 4), "tmp423", 0x4c);
    i2c_create_slave(aspeed_i2c_get_bus(DEVICE(&soc->i2c), 4), "ir35221", 0x70);
    i2c_create_slave(aspeed_i2c_get_bus(DEVICE(&soc->i2c), 4), "ir35221", 0x71);

    i2c_create_slave(aspeed_i2c_get_bus(DEVICE(&soc->i2c), 5), "tmp423", 0x4c);
    i2c_create_slave(aspeed_i2c_get_bus(DEVICE(&soc->i2c), 5), "ir35221", 0x70);
    i2c_create_slave(aspeed_i2c_get_bus(DEVICE(&soc->i2c), 5), "ir35221", 0x71);

    /* The tacoma expects a TMP275 but a TMP105 is compatible */
    i2c_create_slave(aspeed_i2c_get_bus(DEVICE(&soc->i2c), 9), TYPE_TMP105,
                     0x4a);

    i2c_create_slave(aspeed_i2c_get_bus(DEVICE(&soc->i2c), 11), "pca9552",
                     0x60);
    /* The tacoma expects Epson RX8900 RTC but a ds1338 is compatible */
    i2c_create_slave(aspeed_i2c_get_bus(DEVICE(&soc->i2c), 11), "ds1338",
                     0x32);
    smbus_eeprom_init_one(aspeed_i2c_get_bus(DEVICE(&soc->i2c), 11), 0x51,
                          eeprom_buf);
    /* Bus 11: TODO ucd90160@64 */
}

static void rainier_bmc_i2c_init(AspeedBoardState *bmc)
{
    AspeedSoCState *soc = &bmc->soc;

    i2c_create_slave(aspeed_i2c_get_bus(DEVICE(&soc->i2c), 3), "ibm-cffps",
                     0x68);
    i2c_create_slave(aspeed_i2c_get_bus(DEVICE(&soc->i2c), 3), "ibm-cffps",
                     0x69);
    i2c_create_slave(aspeed_i2c_get_bus(DEVICE(&soc->i2c), 3), "ibm-cffps",
                     0x6a);
    i2c_create_slave(aspeed_i2c_get_bus(DEVICE(&soc->i2c), 3), "ibm-cffps",
                     0x6b);

    /* The rainier expects a TMP275 but a TMP105 is compatible */
    i2c_create_slave(aspeed_i2c_get_bus(DEVICE(&soc->i2c), 4), TYPE_TMP105,
                     0x48);
    i2c_create_slave(aspeed_i2c_get_bus(DEVICE(&soc->i2c), 4), TYPE_TMP105,
                     0x49);
    i2c_create_slave(aspeed_i2c_get_bus(DEVICE(&soc->i2c), 4), TYPE_TMP105,
                     0x4a);

    i2c_create_slave(aspeed_i2c_get_bus(DEVICE(&soc->i2c), 5), TYPE_TMP105,
                     0x48);
    i2c_create_slave(aspeed_i2c_get_bus(DEVICE(&soc->i2c), 5), TYPE_TMP105,
                     0x49);

    i2c_create_slave(aspeed_i2c_get_bus(DEVICE(&soc->i2c), 6), TYPE_TMP105,
                     0x48);
    i2c_create_slave(aspeed_i2c_get_bus(DEVICE(&soc->i2c), 6), TYPE_TMP105,
                     0x4a);
    i2c_create_slave(aspeed_i2c_get_bus(DEVICE(&soc->i2c), 6), TYPE_TMP105,
                     0x4b);

    /* Bus 7: TODO dps310@76 */
    /* Bus 7: TODO max31785@52 */
    i2c_create_slave(aspeed_i2c_get_bus(DEVICE(&soc->i2c), 7), "pca9552", 0x60);
    /* Bus 7: TODO si7021-a20@20 */
    i2c_create_slave(aspeed_i2c_get_bus(DEVICE(&soc->i2c), 7), TYPE_TMP105,
                     0x48);

    i2c_create_slave(aspeed_i2c_get_bus(DEVICE(&soc->i2c), 8), TYPE_TMP105,
                     0x48);
    i2c_create_slave(aspeed_i2c_get_bus(DEVICE(&soc->i2c), 8), TYPE_TMP105,
                     0x4a);
    /* Bus 8: ucd90320@11 */
    /* Bus 8: ucd90320@b */
    /* Bus 8: ucd90320@c */

    i2c_create_slave(aspeed_i2c_get_bus(DEVICE(&soc->i2c), 9), "ir35221", 0x42);
    i2c_create_slave(aspeed_i2c_get_bus(DEVICE(&soc->i2c), 9), "ir35221", 0x43);
    i2c_create_slave(aspeed_i2c_get_bus(DEVICE(&soc->i2c), 9), "ir35221", 0x44);
    i2c_create_slave(aspeed_i2c_get_bus(DEVICE(&soc->i2c), 9), "ir35221", 0x72);
    i2c_create_slave(aspeed_i2c_get_bus(DEVICE(&soc->i2c), 9), "ir35221", 0x73);
    i2c_create_slave(aspeed_i2c_get_bus(DEVICE(&soc->i2c), 9), "ir35221", 0x74);
    i2c_create_slave(aspeed_i2c_get_bus(DEVICE(&soc->i2c), 9), "tmp423", 0x4c);
    i2c_create_slave(aspeed_i2c_get_bus(DEVICE(&soc->i2c), 9), "tmp423", 0x4d);

    i2c_create_slave(aspeed_i2c_get_bus(DEVICE(&soc->i2c), 10), "ir35221", 0x42);
    i2c_create_slave(aspeed_i2c_get_bus(DEVICE(&soc->i2c), 10), "ir35221", 0x43);
    i2c_create_slave(aspeed_i2c_get_bus(DEVICE(&soc->i2c), 10), "ir35221", 0x44);
    i2c_create_slave(aspeed_i2c_get_bus(DEVICE(&soc->i2c), 10), "ir35221", 0x72);
    i2c_create_slave(aspeed_i2c_get_bus(DEVICE(&soc->i2c), 10), "ir35221", 0x73);
    i2c_create_slave(aspeed_i2c_get_bus(DEVICE(&soc->i2c), 10), "ir35221", 0x74);
    i2c_create_slave(aspeed_i2c_get_bus(DEVICE(&soc->i2c), 10), "tmp423", 0x4c);
    i2c_create_slave(aspeed_i2c_get_bus(DEVICE(&soc->i2c), 10), "tmp423", 0x4d);

    i2c_create_slave(aspeed_i2c_get_bus(DEVICE(&soc->i2c), 11), TYPE_TMP105,
                     0x48);
    i2c_create_slave(aspeed_i2c_get_bus(DEVICE(&soc->i2c), 11), TYPE_TMP105,
                     0x49);
}

static void aspeed_machine_init(MachineState *machine)
{
    aspeed_board_init(machine);
}

static bool aspeed_get_mmio_exec(Object *obj, Error **errp)
{
    return ASPEED_MACHINE(obj)->mmio_exec;
}

static void aspeed_set_mmio_exec(Object *obj, bool value, Error **errp)
{
    ASPEED_MACHINE(obj)->mmio_exec = value;
}

static void aspeed_machine_instance_init(Object *obj)
{
    ASPEED_MACHINE(obj)->mmio_exec = false;
}

static void aspeed_machine_class_props_init(ObjectClass *oc)
{
    object_class_property_add_bool(oc, "execute-in-place",
                                   aspeed_get_mmio_exec,
                                   aspeed_set_mmio_exec, &error_abort);
    object_class_property_set_description(oc, "execute-in-place",
                           "boot directly from CE0 flash device", &error_abort);
}

static void aspeed_machine_class_init(ObjectClass *oc, void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    AspeedMachineClass *amc = ASPEED_MACHINE_CLASS(oc);

    mc->desc = amc->desc;
    mc->init = aspeed_machine_init;
    mc->reset = amc->reset;
    mc->max_cpus = ASPEED_CPUS_NUM;
    mc->no_floppy = 1;
    mc->no_cdrom = 1;
    mc->no_parallel = 1;
    if (amc->ram) {
        mc->default_ram_size = amc->ram;
    }

    aspeed_machine_class_props_init(oc);
}

static const TypeInfo aspeed_machine_type = {
    .name = TYPE_ASPEED_MACHINE,
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(AspeedMachine),
    .class_size = sizeof(AspeedMachineClass),
    .instance_init = aspeed_machine_instance_init,
    .abstract = true,
};

static void palmetto_bmc_init(ObjectClass *oc, void *data)
{
    AspeedMachineClass *amc = ASPEED_MACHINE_CLASS(oc);

    amc->name      = MACHINE_TYPE_NAME("palmetto-bmc");
    amc->desc      = "OpenPOWER Palmetto BMC (ARM926EJ-S)";
    amc->soc_name  = "ast2400-a1";
    amc->hw_strap1 = PALMETTO_BMC_HW_STRAP1;
    amc->fmc_model = "n25q256a";
    amc->spi_model = "mx25l25635e";
    amc->num_cs    = 1;
    amc->i2c_init  = palmetto_bmc_i2c_init;
    amc->ram       = 256 * MiB;
    aspeed_machine_class_init(oc, data);
}

static void ast2500_evb_init(ObjectClass *oc, void *data)
{
    AspeedMachineClass *amc = ASPEED_MACHINE_CLASS(oc);

    amc->desc      = "Aspeed AST2500 EVB (ARM1176)";
    amc->soc_name  = "ast2500-a1";
    amc->hw_strap1 = AST2500_EVB_HW_STRAP1;
    amc->fmc_model = "w25q256";
    amc->spi_model = "mx25l25635e";
    amc->num_cs    = 1;
    amc->i2c_init  = ast2500_evb_i2c_init;
    amc->ram       = 512 * MiB;
    aspeed_machine_class_init(oc, data);
}

static void romulus_bmc_init(ObjectClass *oc, void *data)
{
    AspeedMachineClass *amc = ASPEED_MACHINE_CLASS(oc);

    amc->desc      = "OpenPOWER Romulus BMC (ARM1176)";
    amc->soc_name  = "ast2500-a1";
    amc->hw_strap1 = ROMULUS_BMC_HW_STRAP1;
    amc->fmc_model = "n25q256a";
    amc->spi_model = "mx66l1g45g";
    amc->num_cs    = 2;
    amc->i2c_init  = romulus_bmc_i2c_init;
    amc->ram       = 512 * MiB;
    aspeed_machine_class_init(oc, data);
}

static void swift_bmc_init(ObjectClass *oc, void *data)
{
    AspeedMachineClass *amc = ASPEED_MACHINE_CLASS(oc);

    amc->desc      = "OpenPOWER Swift BMC (ARM1176)";
    amc->soc_name  = "ast2500-a1";
    amc->hw_strap1 = SWIFT_BMC_HW_STRAP1;
    amc->fmc_model = "mx66l1g45g";
    amc->spi_model = "mx66l1g45g";
    amc->num_cs    = 2;
    amc->i2c_init  = swift_bmc_i2c_init;
    amc->ram       = 512 * MiB;
    aspeed_machine_class_init(oc, data);
};

static void witherspoon_bmc_init(ObjectClass *oc, void *data)
{
    AspeedMachineClass *amc = ASPEED_MACHINE_CLASS(oc);

    amc->desc      = "OpenPOWER Witherspoon BMC (ARM1176)";
    amc->soc_name  = "ast2500-a1";
    amc->hw_strap1 = WITHERSPOON_BMC_HW_STRAP1;
    amc->fmc_model = "mx25l25635e";
    amc->spi_model = "mx66l1g45g";
    amc->num_cs    = 2;
    amc->i2c_init  = witherspoon_bmc_i2c_init;
    amc->ram       = 512 * MiB;

    aspeed_machine_class_init(oc, data);
}

static void ast2600_evb_init(ObjectClass *oc, void *data)
{
    AspeedMachineClass *amc = ASPEED_MACHINE_CLASS(oc);

    amc->desc      = "Aspeed AST2600 EVB (Cortex A7)";
    amc->soc_name  = "ast2600-a0";
    amc->hw_strap1 = AST2600_EVB_HW_STRAP1;
    amc->fmc_model = "mx25l25635e";
    amc->spi_model = "mx25l25635e";
    amc->num_cs    = 1;
    amc->i2c_init  = ast2600_evb_i2c_init;
    amc->reset     = ast2600_evb_reset;
    aspeed_machine_class_init(oc, data);
}

static void tacoma_bmc_init(ObjectClass *oc, void *data)
{
    AspeedMachineClass *amc = ASPEED_MACHINE_CLASS(oc);

    amc->desc      = "Aspeed AST2600 EVB (Cortex A7)";
    amc->soc_name  = "ast2600-a0";
    amc->hw_strap1 = TACOMA_BMC_HW_STRAP1;
    amc->fmc_model = "mx25l25635e";
    amc->spi_model = "mx25l25635e";
    amc->num_cs    = 2;
    amc->i2c_init  = tacoma_bmc_i2c_init;
    amc->ram       = 2 * GiB;
    aspeed_machine_class_init(oc, data);
}

static void rainier_bmc_init(ObjectClass *oc, void *data)
{
    AspeedMachineClass *amc = ASPEED_MACHINE_CLASS(oc);

    amc->desc      = "Aspeed AST2600 EVB (Cortex A7)";
    amc->soc_name  = "ast2600-a0";
    amc->hw_strap1 = RAINIER_BMC_HW_STRAP1;
    amc->fmc_model = "mx25l25635e";
    amc->spi_model = "mx25l25635e";
    amc->num_cs    = 2;
    amc->i2c_init  = rainier_bmc_i2c_init;
    amc->ram       = 2 * GiB;
    aspeed_machine_class_init(oc, data);
}

#define DEFINE_ASPEED_MACHINE(type, _name)                \
	static const TypeInfo type##_info = {                 \
		    .name           = MACHINE_TYPE_NAME(_name),   \
		    .parent         = TYPE_ASPEED_MACHINE,        \
		    .class_init     = type##_init,                \
	};                                                    \
	static void aspeed_machine_register_##type(void)      \
    {                                                     \
	        type_register(&type##_info);                  \
    }                                                     \
    type_init(aspeed_machine_register_##type)

DEFINE_ASPEED_MACHINE(palmetto_bmc, "palmetto-bmc");
DEFINE_ASPEED_MACHINE(ast2500_evb, "ast2500-evb");
DEFINE_ASPEED_MACHINE(romulus_bmc, "romulus-bmc");
DEFINE_ASPEED_MACHINE(swift_bmc, "swift-bmc");
DEFINE_ASPEED_MACHINE(witherspoon_bmc, "witherspoon-bmc");
DEFINE_ASPEED_MACHINE(ast2600_evb, "ast2600-evb");
DEFINE_ASPEED_MACHINE(tacoma_bmc, "tacoma-bmc");
DEFINE_ASPEED_MACHINE(rainier_bmc, "rainier-bmc");

static void aspeed_machine_types(void)
{
    type_register_static(&aspeed_machine_type);
}

type_init(aspeed_machine_types)
