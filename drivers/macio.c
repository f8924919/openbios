/*
 *   derived from mol/mol.c,
 *   Copyright (C) 2003, 2004 Samuel Rydh (samuel@ibrium.se)
 *
 *   This program is free software; you can redistribute it and/or
 *   modify it under the terms of the GNU General Public License
 *   version 2
 *
 */

#include "config.h"
#include "arch/common/nvram.h"
#include "packages/nvram.h"
#include "libopenbios/bindings.h"
#include "libc/byteorder.h"
#include "libc/vsprintf.h"

#include "drivers/drivers.h"
#include "macio.h"
#include "cuda.h"
#include "pmu.h"
#include "escc.h"
#include "drivers/pci.h"

#define OW_IO_NVRAM_SIZE   0x00020000
#define OW_IO_NVRAM_OFFSET 0x00060000
#define OW_IO_NVRAM_SHIFT  4

#define NW_IO_NVRAM_SIZE   0x00004000
#define NW_IO_NVRAM_OFFSET 0xfff04000

#define IO_OPENPIC_SIZE    0x00040000
#define IO_OPENPIC_OFFSET  0x00040000

/* The Keywest i2c sits inside U3, one 4KB page above the U3 registers */
#define IO_U3_BASE         0xf8000000
#define IO_U3_I2C_OFFSET   0x00001000
#define IO_U3_I2C_SIZE     0x00001000

static char *nvram;

static int macio_nvram_shift(void)
{
	int nvram_flat;

        if (is_oldworld())
                return OW_IO_NVRAM_SHIFT;

	nvram_flat = fw_cfg_read_i32(FW_CFG_PPC_NVRAM_FLAT);
	return nvram_flat ? 0 : 1;
}

int
macio_get_nvram_size(void)
{
	int shift = macio_nvram_shift();
        if (is_oldworld())
                return OW_IO_NVRAM_SIZE >> shift;
        else
                return NW_IO_NVRAM_SIZE >> shift;
}

static unsigned long macio_nvram_offset(void)
{
	unsigned long r;

	/* Hypervisor tells us where NVRAM lies */
	r = fw_cfg_read_i32(FW_CFG_PPC_NVRAM_ADDR);
	if (r)
		return r;

	/* Fall back to hardcoded addresses */
	if (is_oldworld())
		return OW_IO_NVRAM_OFFSET;

	return NW_IO_NVRAM_OFFSET;
}

static unsigned long macio_nvram_size(void)
{
	if (is_oldworld())
		return OW_IO_NVRAM_SIZE;
	else
		return NW_IO_NVRAM_SIZE;
}

/* #address-cells of the node the child hangs off (1 unless overridden) */
static int parent_address_cells(const char *path)
{
	phandle_t parent = find_dev(path);

	if (parent && get_int_property(parent, "#address-cells", NULL) == 2)
		return 2;
	return 1;
}

void macio_nvram_init(const char *path, phys_addr_t addr)
{
	phandle_t chosen, aliases;
	phandle_t dnode;
	int props[3];
	int n = 0;
	char buf[64];
        unsigned long nvram_size, nvram_offset;

        nvram_offset = macio_nvram_offset();
        nvram_size = macio_nvram_size();

	nvram = (char*)addr + nvram_offset;
	nvconf_init();
	snprintf(buf, sizeof(buf), "%s", path);
	dnode = nvram_init(buf);
	set_int_property(dnode, "#bytes", arch_nvram_size() );
	if (parent_address_cells(path) == 2)
		props[n++] = 0;
	props[n++] = __cpu_to_be32(nvram_offset);
	props[n++] = __cpu_to_be32(nvram_size);
	set_property(dnode, "reg", (char *)&props, n * sizeof(props[0]));
	set_property(dnode, "device_type", "nvram", 6);
	NEWWORLD(set_property(dnode, "compatible", "nvram,flash", 12));

	chosen = find_dev("/chosen");
	snprintf(buf, sizeof(buf), "%s", get_path_from_ph(dnode));
	push_str(buf);
	fword("open-dev");
	set_int_property(chosen, "nvram", POP());

	aliases = find_dev("/aliases");
	set_property(aliases, "nvram", buf, strlen(buf) + 1);
}

#ifdef DUMP_NVRAM
static void
dump_nvram(void)
{
  int i, j;
  for (i = 0; i < 10; i++)
    {
      for (j = 0; j < 16; j++)
      printk ("%02x ", nvram[(i*16+j)<<4]);
      printk (" ");
      for (j = 0; j < 16; j++)
        if (isprint(nvram[(i*16+j)<<4]))
            printk("%c", nvram[(i*16+j)<<4]);
        else
          printk(".");
      printk ("\n");
      }
}
#endif


void
macio_nvram_put(char *buf)
{
	int i;
        unsigned int it_shift = macio_nvram_shift();

	for (i=0; i < arch_nvram_size(); i++)
		nvram[i << it_shift] = buf[i];
#ifdef DUMP_NVRAM
	printk("new nvram:\n");
	dump_nvram();
#endif
}

void
macio_nvram_get(char *buf)
{
	int i;
        unsigned int it_shift = macio_nvram_shift();

	for (i=0; i< arch_nvram_size(); i++)
                buf[i] = nvram[i << it_shift];

#ifdef DUMP_NVRAM
	printk("current nvram:\n");
	dump_nvram();
#endif
}

static void
openpic_init(const char *path, const char *name)
{
        phandle_t dnode;
        int props[2];
        char buf[128];

        fword("new-device");
        push_str(name);
        fword("device-name");

        snprintf(buf, sizeof(buf), "%s/%s", path, name);
        dnode = find_dev(buf);
        set_property(dnode, "device_type", "open-pic", 9);
        set_property(dnode, "compatible", "chrp,open-pic", 14);
        set_property(dnode, "built-in", "", 0);
        props[0] = __cpu_to_be32(IO_OPENPIC_OFFSET);
        props[1] = __cpu_to_be32(IO_OPENPIC_SIZE);
        set_property(dnode, "reg", (char *)&props, sizeof(props));
        set_int_property(dnode, "#interrupt-cells", 2);
        set_int_property(dnode, "#address-cells", 0);
        set_property(dnode, "interrupt-controller", "", 0);
        set_int_property(dnode, "clock-frequency", 4166666);

        fword("finish-device");
}

/*
 * PowerMac7,3 exposes the mpic twice, just like the real machine does: once
 * as a child of mac-io and once under /u3.  Both describe the same
 * registers, because the K2 BAR covers 0xf8000000 and the mpic sits at
 * BAR + 0x40000.
 *
 * The real machine makes the mac-io one the root PIC and leaves /u3/mpic as
 * its secondary, which is why Mac OS X hardcodes "mac-io/mpic": its
 * AppleMacRISC4PE platform expert resolves that path with
 * IORegistryEntry::fromPath() and dereferences the result without a NULL
 * check.  This is the mirror, the one that is only there to be looked at.
 *
 * Deliberately leave out "device_type" and "interrupt-controller": Linux
 * would pick a second open-pic node up as a cascaded slave and re-initialise
 * the very same MMIO behind the master's back.  Leave out "interrupts" as
 * well -- the real secondary cascades into the root PIC, but there is only
 * one emulated openpic here, so there is nothing to cascade from.
 * "compatible" stays, because the real node has it.
 *
 * The node is called "mpic-mirror" rather than "mpic", which is what the
 * real machine calls it.  Mac OS X 10.5 refuses to register a nub for the
 * second node of that name: the mirror is published first, below /u3, and
 * when AppleK2 later publishes its own "mpic" the nub attaches but is never
 * registered, so mac-io ends up with no interrupt controller at all.  Every
 * driver below it that needs an interrupt then stalls -- KeyLargoATA never
 * scans the bus and AppleVIA never finishes the PMU handshake, which is how
 * this was found.  10.4 tolerates the duplicate.  Renaming the mirror is
 * invisible to Linux, which looks the controller up by device_type
 * ("open-pic") and by the "interrupt-controller" property, neither of which
 * the mirror has.
 */
static void
mpic_mirror_init(const char *path)
{
        phandle_t dnode;
        int props[2];
        char buf[128];

        fword("new-device");
        push_str("mpic-mirror");
        fword("device-name");

        snprintf(buf, sizeof(buf), "%s/mpic-mirror", path);
        dnode = find_dev(buf);
        set_property(dnode, "compatible", "chrp,open-pic", 14);
        set_property(dnode, "built-in", "", 0);
        props[0] = __cpu_to_be32(IO_OPENPIC_OFFSET);
        props[1] = __cpu_to_be32(IO_OPENPIC_SIZE);
        set_property(dnode, "reg", (char *)&props, sizeof(props));
        set_int_property(dnode, "#interrupt-cells", 2);
        set_int_property(dnode, "#address-cells", 0);
        set_int_property(dnode, "clock-frequency", 4166666);

        fword("finish-device");
}

/*
 * The Keywest i2c controller inside U3, and the clock generator that hangs
 * off it.  Three different consumers read this node and each wants
 * something the others do not:
 *
 *  - Darwin's AppleI2C matches on the name "i2c" alone, but insists that
 *    AAPL,driver-name *begins* with ".i2c-uni-n" (strncmp of 10 bytes,
 *    dot included).  Without it the PPCI2CInterface.i2c-uni-n resource is
 *    never published and MacRISC4CPU cannot find the bus.
 *  - Linux low_i2c.c only looks at nodes whose device_type is "i2c" and
 *    whose compatible contains "keywest-i2c".
 *  - Linux smp.c walks to i2c-hwclock and requires the *parent* to be
 *    compatible with "uni-n-i2c" before it will sync the timebase.
 *
 * reg is u3-relative because /u3 carries a ranges entry, while AAPL,address
 * is the absolute physical address -- the two are deliberately different
 * and both are needed.
 */
static void
u3_i2c_init(const char *path)
{
        phandle_t dnode;
        uint32_t props[2];
        char buf[128];

        fword("new-device");
        push_str("i2c");
        fword("device-name");

        snprintf(buf, sizeof(buf), "%s/i2c", path);
        dnode = find_dev(buf);

        set_property(dnode, "device_type", "i2c", 4);
        set_property(dnode, "compatible", "keywest-i2c\0uni-n-i2c", 22);
        set_property(dnode, "AAPL,driver-name", ".i2c-uni-n", 11);

        props[0] = __cpu_to_be32(IO_U3_I2C_OFFSET);
        props[1] = __cpu_to_be32(IO_U3_I2C_SIZE);
        set_property(dnode, "reg", (char *)&props, sizeof(props));

        set_int_property(dnode, "AAPL,address",
                         IO_U3_BASE + IO_U3_I2C_OFFSET);
        set_int_property(dnode, "AAPL,address-step", 0x10);
        set_int_property(dnode, "AAPL,i2c-rate", 100);

        /*
         * The child unit address is synthesised from reg using these, and
         * Darwin looks the chip up by the path /u3/i2c/i2c-hwclock@d2.
         * Without the cells the "@d2" never appears and the lookup fails.
         */
        set_int_property(dnode, "#address-cells", 1);
        set_int_property(dnode, "#size-cells", 0);

        fword("new-device");
        push_str("i2c-hwclock");
        fword("device-name");

        snprintf(buf, sizeof(buf), "%s/i2c/i2c-hwclock", path);
        dnode = find_dev(buf);
        set_property(dnode, "compatible", "pulsar-legacy-slewing", 22);
        /*
         * 0xd2 is the 8-bit form.  Linux reads this one value three ways:
         * smp.c switches on it, i2c-powermac turns it into the 7-bit
         * address with (reg & 0xff) >> 1, and low_i2c takes reg >> 8 as
         * the channel number.
         */
        set_int_property(dnode, "reg", 0xd2);

        fword("finish-device");

        fword("finish-device");
}

DECLARE_UNNAMED_NODE(ob_macio, 0, sizeof(int));

/* ( str len -- addr ) */

static void
ob_macio_decode_unit(void *private)
{
	ucell addr;

	const char *arg = pop_fstr_copy();

	addr = strtol(arg, NULL, 16);

	free((char*)arg);

	PUSH(addr);
}

/*  ( addr -- str len ) */

static void
ob_macio_encode_unit(void *private)
{
	char buf[8];

	ucell addr = POP();

	snprintf(buf, sizeof(buf), "%x", addr);

	push_str(buf);
}

static void
ob_macio_dma_alloc(int *idx)
{
    call_parent_method("dma-alloc");
}

static void
ob_macio_dma_free(int *idx)
{
    call_parent_method("dma-free");
}

static void
ob_macio_dma_map_in(int *idx)
{
    call_parent_method("dma-map-in");
}

static void
ob_macio_dma_map_out(int *idx)
{
    call_parent_method("dma-map-out");
}

static void
ob_macio_dma_sync(int *idx)
{
    call_parent_method("dma-sync");
}

NODE_METHODS(ob_macio) = {
        { "decode-unit",	ob_macio_decode_unit	},
        { "encode-unit",	ob_macio_encode_unit	},
        { "dma-alloc",		ob_macio_dma_alloc	},
        { "dma-free",		ob_macio_dma_free		},
        { "dma-map-in",		ob_macio_dma_map_in	},
        { "dma-map-out",	ob_macio_dma_map_out	},
        { "dma-sync",		ob_macio_dma_sync		},
};

void
ob_unin_init(void)
{
        phandle_t dnode;
        int props[2];

        fword("new-device");
        push_str("uni-n");
        fword("device-name");

        dnode = find_dev("/uni-n");
        set_property(dnode, "device_type", "memory-controller", 18);
        set_property(dnode, "compatible", "uni-north", 10);
        set_int_property(dnode, "device-rev", 7);
        props[0] = __cpu_to_be32(0xf8000000);
        props[1] = __cpu_to_be32(0x1000000);
        set_property(dnode, "reg", (char *)&props, sizeof(props));

        fword("finish-device");
}

/* U3 (PowerMac7,3): same memory controller, named u3 */
void
ob_u3_init(void)
{
        phandle_t dnode;
        int props[3];
        int n = 0;

        fword("new-device");
        push_str("u3");
        fword("device-name");

        dnode = find_dev("/u3");
        set_property(dnode, "device_type", "memory-controller", 18);
        set_property(dnode, "compatible", "u3", 3);
        set_int_property(dnode, "device-rev", 7);
        if (parent_address_cells("/") == 2)
                props[n++] = 0;
        props[n++] = __cpu_to_be32(0xf8000000);
        props[n++] = __cpu_to_be32(0x1000000);
        set_property(dnode, "reg", (char *)&props, n * sizeof(props[0]));

        /*
         * The mpic lives inside U3 on the real machine.  Linux needs
         * the cells and a ranges entry here: without ranges, the Apple
         * of_empty_ranges_quirk() silently treats child addresses as
         * 1:1, sending the mpic reg 0x40000 into RAM.
         */
        set_int_property(dnode, "#address-cells", 1);
        set_int_property(dnode, "#size-cells", 1);
        {
                uint32_t ranges[4];
                int rn = 0;

                ranges[rn++] = 0;               /* u3-internal offset 0 */
                if (parent_address_cells("/") == 2)
                        ranges[rn++] = 0;
                ranges[rn++] = __cpu_to_be32(0xf8000000);
                ranges[rn++] = __cpu_to_be32(0x1000000);
                set_property(dnode, "ranges", (char *)ranges,
                             rn * sizeof(ranges[0]));
        }
        mpic_mirror_init("/u3");
        u3_i2c_init("/u3");

        fword("finish-device");
}

static void macio_gpio_init(const char *path)
{
    fword("new-device");

    push_str("gpio");
    fword("device-name");

    push_str("gpio");
    fword("device-type");

    PUSH(1);
    fword("encode-int");
    push_str("#address-cells");
    fword("property");

    PUSH(0);
    fword("encode-int");
    push_str("#size-cells");
    fword("property");

    push_str("mac-io-gpio");
    fword("encode-string");
    push_str("compatible");
    fword("property");

    PUSH(0x50);
    fword("encode-int");
    PUSH(0x30);
    fword("encode-int");
    fword("encode+");
    push_str("reg");
    fword("property");

    /* Build the extint-gpio1 for the PMU */
    fword("new-device");
    push_str("extint-gpio1");
    fword("device-name");
    PUSH(0x2f);
    fword("encode-int");
    PUSH(0x1);
    fword("encode-int");
    fword("encode+");
    push_str("interrupts");
    fword("property");
    PUSH(0x9);
    fword("encode-int");
    push_str("reg");
    fword("property");
    push_str("keywest-gpio1");
    fword("encode-string");
    push_str("gpio");
    fword("encode-string");
    fword("encode+");
    push_str("compatible");
    fword("property");
    fword("finish-device");

    /* Build the programmer-switch */
    fword("new-device");
    push_str("programmer-switch");
    fword("device-name");
    push_str("programmer-switch");
    fword("encode-string");
    push_str("device_type");
    fword("property");
    PUSH(0x37);
    fword("encode-int");
    PUSH(0x0);
    fword("encode-int");
    fword("encode+");
    push_str("interrupts");
    fword("property");
    fword("finish-device");

    fword("finish-device");
}

void
ob_macio_heathrow_init(const char *path, phys_addr_t addr)
{
    phandle_t aliases;

    BIND_NODE_METHODS(get_cur_dev(), ob_macio);

    cuda_init(path, addr);
    macio_nvram_init(path, addr);
    escc_init(path, addr);
    macio_ide_init(path, addr, 2);

    aliases = find_dev("/aliases");
    set_property(aliases, "mac-io", path, strlen(path) + 1);
}

void
ob_macio_keylargo_init(const char *path, phys_addr_t addr)
{
    phandle_t aliases;

    BIND_NODE_METHODS(get_cur_dev(), ob_macio);

    if (has_pmu()) {
        macio_gpio_init(path);
        pmu_init(path, addr);
    } else {
        cuda_init(path, addr);
    }

    escc_init(path, addr);
    macio_ide_init(path, addr, 2);
    /*
     * The mpic below mac-io is the root PIC on both families; only its
     * node name differs.  Real PowerMac7,3 firmware calls it "mpic" and
     * mirrors it under /u3, while the mac99 KeyLargo machines call it
     * "interrupt-controller".  Key off /u3, which only the former builds,
     * and fall back to the mac99 name so that family cannot be dragged
     * along by a mistake in the test.
     */
    openpic_init(path, find_dev("/u3") ? "mpic" : "interrupt-controller");

    aliases = find_dev("/aliases");
    set_property(aliases, "mac-io", path, strlen(path) + 1);
}
