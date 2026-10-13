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
#include "libopenbios/ofmem.h"
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

/*
 * The NewWorld part is a flash chip.  Raw stores are taken as commands, so
 * an image written byte by byte mostly vanishes: only the bytes that follow
 * a 0x40/0x10 in the data get programmed, and the checksums end up wrong.
 * Program it the way Mac OS X does instead.
 */
#define NV_CMD_ERASE_SETUP	0x20
#define NV_CMD_ERASE_CONFIRM	0xd0
#define NV_CMD_WRITE_SETUP	0x40
#define NV_CMD_READ_ARRAY	0xff
#define NV_STATUS_DONE		0x80
#define NV_STATUS_ERR		0x38
#define NW_NVRAM_BANK_SIZE	0x2000
#define NW_NVRAM_NBANKS		2
#define CORE99_HDR_SIZE		0x20
#define CORE99_ADLER_OFF	0x10
#define CORE99_GEN_OFF		0x14
#define CORE99_SIGNATURE	0x5a

#define NW_IO_NVRAM_SIZE   0x00004000
#define NW_IO_NVRAM_OFFSET 0xfff04000

#define IO_OPENPIC_SIZE    0x00040000
#define IO_OPENPIC_OFFSET  0x00040000

/* The Keywest i2c sits inside U3, one 4KB page above the U3 registers */
#define IO_U3_BASE         0xf8000000
#define IO_U3_I2C_OFFSET   0x00001000
#define IO_U3_DART_OFFSET  0x00033000
#define IO_U3_DART_SIZE    0x00007000
#define IO_U3_I2C_SIZE     0x00001000

/*
 * volatile: these are stores to a flash part, not to memory.  Without it
 * the compiler drops the erase setup as a dead store (it is overwritten
 * by the confirm at the same address) and folds the status read away.
 */
static volatile char *nvram;

static int macio_nvram_shift(void)
{
	int nvram_flat;

        if (is_oldworld())
                return OW_IO_NVRAM_SHIFT;

	nvram_flat = fw_cfg_read_i32(FW_CFG_PPC_NVRAM_FLAT);
	return nvram_flat ? 0 : 1;
}

/* Bytes of the part one core99 bank takes up, header included. */
static int macio_nvram_bank_size(void)
{
	return (NW_IO_NVRAM_SIZE >> macio_nvram_shift()) / NW_NVRAM_NBANKS;
}

/*
 * What the nvram package gets to see.  The part holds two banks, each a
 * complete image with its own generation and adler32, and the guests pick
 * the newer one.  Handing the package both of them makes it read the first
 * and write the last, so only ever show it the payload of a single bank.
 */
int
macio_get_nvram_size(void)
{
	int shift = macio_nvram_shift();
        if (is_oldworld())
                return OW_IO_NVRAM_SIZE >> shift;

	return macio_nvram_bank_size() - CORE99_HDR_SIZE;
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

	/*
	 * The NewWorld NVRAM lives in the top megabyte, which ea_to_phys()
	 * redirects to the ROM copy in RAM before it looks at a translation,
	 * so stores through a 1:1 pointer never reach the device.  Take a
	 * mapping out of the I/O range instead.  OldWorld keeps the plain
	 * pointer: its NVRAM hangs off the macio base, clear of that window.
	 */
	nvram = (volatile char *)((uintptr_t)addr + nvram_offset);
	if (!is_oldworld()) {
		phys_addr_t phys = (phys_addr_t)addr + nvram_offset;
		ucell virt = ofmem_claim_io(-1, nvram_size, PAGE_SIZE);

		if (virt != (ucell)-1) {
			ofmem_map(phys & PAGE_MASK, virt, nvram_size,
				  ofmem_arch_io_translation_mode(phys));
			nvram = (volatile char *)(uintptr_t)(virt + (phys & ~PAGE_MASK));
		} else {
			printk("macio: no I/O space for the NVRAM, "
			       "writes will not reach the device\n");
		}
	}
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


/* Wait out an erase.  Reads return the status register until the reset. */
static int nvram_wait(unsigned int it_shift, int off)
{
	unsigned char status;
	int i;

	for (i = 0; i < 100000; i++) {
		status = nvram[off << it_shift];
		if (status & NV_STATUS_DONE)
			return (status & NV_STATUS_ERR) ? -1 : 0;
	}
	return -1;
}

static unsigned int nv_be32(unsigned int it_shift, int off)
{
	unsigned int v = 0;
	int i;

	for (i = 0; i < 4; i++)
		v = (v << 8) | (unsigned char)nvram[(off + i) << it_shift];
	return v;
}

/*
 * The bank the guests would read: valid header, and the larger generation
 * wins.  Returns -1 when neither bank holds a usable image.
 */
static int macio_nvram_cur_bank(unsigned int *genp)
{
	unsigned int it_shift = macio_nvram_shift();
	int bank_size = macio_nvram_bank_size();
	unsigned char hdr[16];
	unsigned int gen, best_gen = 0;
	int b, i, base, best = -1;

	for (b = 0; b < NW_NVRAM_NBANKS; b++) {
		base = b * bank_size;
		for (i = 0; i < 16; i++)
			hdr[i] = nvram[(base + i) << it_shift];
		if (hdr[0] != CORE99_SIGNATURE)
			continue;
		if (nvpart_checksum_buf(hdr) != hdr[1])
			continue;
		gen = nv_be32(it_shift, base + CORE99_GEN_OFF);
		if (best < 0 || gen > best_gen) {
			best = b;
			best_gen = gen;
		}
	}
	*genp = (best < 0) ? 0 : best_gen;
	return best;
}

void
macio_nvram_put(char *buf)
{
	unsigned int it_shift = macio_nvram_shift();
	int size = arch_nvram_size();
	unsigned char hdr[CORE99_HDR_SIZE];
	unsigned int gen, adler;
	int bank_size, cur, tgt, base, i;

	if (is_oldworld()) {
		for (i=0; i < size; i++)
			nvram[i << it_shift] = buf[i];
#ifdef DUMP_NVRAM
		printk("new nvram:\n");
		dump_nvram();
#endif
		return;
	}

	bank_size = macio_nvram_bank_size();
	cur = macio_nvram_cur_bank(&gen);

	/*
	 * Leave the part alone when the bank in use already holds this
	 * image.  update_nvram() runs on every boot, and erasing a bank
	 * that does not have to change would put the image at risk for
	 * nothing.
	 */
	if (cur >= 0) {
		base = cur * bank_size + CORE99_HDR_SIZE;
		for (i = 0; i < size; i++)
			if (nvram[(base + i) << it_shift] != buf[i])
				break;
		if (i == size)
			return;
	}

	/*
	 * Write the bank that is not in use, so that a write cut short
	 * leaves the guests reading the one that still is.  The generation
	 * decides the winner, and 0 reads as "invalid" to Mac OS X, so it
	 * is skipped on wrap.
	 */
	tgt = (cur < 0) ? 0 : !cur;
	gen++;
	if (!gen)
		gen = 1;

	memset(hdr, 0, sizeof(hdr));
	hdr[0] = CORE99_SIGNATURE;
	hdr[2] = (CORE99_HDR_SIZE / 16) >> 8;
	hdr[3] = (CORE99_HDR_SIZE / 16) & 0xff;
	memcpy(&hdr[4], "nvram", 5);
	hdr[CORE99_GEN_OFF + 0] = gen >> 24;
	hdr[CORE99_GEN_OFF + 1] = gen >> 16;
	hdr[CORE99_GEN_OFF + 2] = gen >> 8;
	hdr[CORE99_GEN_OFF + 3] = gen;

	/* Covers everything from the generation on, so it comes last. */
	adler = adler32_buf(1, &hdr[CORE99_GEN_OFF],
			    CORE99_HDR_SIZE - CORE99_GEN_OFF);
	adler = adler32_buf(adler, (const unsigned char *)buf, size);
	hdr[CORE99_ADLER_OFF + 0] = adler >> 24;
	hdr[CORE99_ADLER_OFF + 1] = adler >> 16;
	hdr[CORE99_ADLER_OFF + 2] = adler >> 8;
	hdr[CORE99_ADLER_OFF + 3] = adler;

	/* The checksum only reaches the first 16 bytes. */
	hdr[1] = nvpart_checksum_buf(hdr);

	base = tgt * bank_size;
	nvram[base << it_shift] = NV_CMD_ERASE_SETUP;
	nvram[base << it_shift] = NV_CMD_ERASE_CONFIRM;
	if (nvram_wait(it_shift, base) < 0) {
		/*
		 * Leave the bank alone rather than program a half erased
		 * one: the other bank still holds a good image, and that is
		 * what the guests keep reading.
		 */
		printk("macio: nvram erase failed at %x, not programming\n",
		       base);
		nvram[base << it_shift] = NV_CMD_READ_ARRAY;
		return;
	}
	nvram[base << it_shift] = NV_CMD_READ_ARRAY;

	for (i = 0; i < CORE99_HDR_SIZE; i++) {
		if (hdr[i] == 0xff)
			continue;
		nvram[(base + i) << it_shift] = NV_CMD_WRITE_SETUP;
		nvram[(base + i) << it_shift] = hdr[i];
	}
	for (i = 0; i < size; i++) {
		if ((unsigned char)buf[i] == 0xff)
			continue;
		nvram[(base + CORE99_HDR_SIZE + i) << it_shift] =
			NV_CMD_WRITE_SETUP;
		nvram[(base + CORE99_HDR_SIZE + i) << it_shift] = buf[i];
	}

	/* Put the part back in read mode, or the next read returns status. */
	nvram[base << it_shift] = NV_CMD_READ_ARRAY;
#ifdef DUMP_NVRAM
	printk("new nvram:\n");
	dump_nvram();
#endif
}

void
macio_nvram_get(char *buf)
{
	unsigned int it_shift = macio_nvram_shift();
	int size = arch_nvram_size();
	unsigned int gen;
	int cur, base, i;

	if (is_oldworld()) {
		for (i=0; i< size; i++)
			buf[i] = nvram[i << it_shift];
#ifdef DUMP_NVRAM
		printk("current nvram:\n");
		dump_nvram();
#endif
		return;
	}

	cur = macio_nvram_cur_bank(&gen);
	base = ((cur < 0) ? 0 : cur * macio_nvram_bank_size())
		+ CORE99_HDR_SIZE;
	for (i = 0; i < size; i++)
		buf[i] = nvram[(base + i) << it_shift];
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

/*
 * The U3 DART (IOMMU), present only when QEMU has one (FW_CFG_PPC_DART):
 * that is when the RAM goes above 4 GiB, so that 32-bit DMA engines can
 * reach it.  Both Linux and Mac OS X use the DART as soon as this node
 * exists, so it must not appear otherwise.
 *
 *  - The name has to be "dart" directly below /u3: xnu and AppleMacRISC4PE
 *    look up "/u3/dart" by path to decide that a mapper is required, and
 *    AppleU3ART / AppleDART match on IONameMatch "dart".
 *  - compatible "u3-dart": Linux looks the DART up by it (with device_type
 *    "dart"), and Leopard's AppleDART drives the U4 register layout
 *    without it.
 *  - reg is u3-relative, like the i2c's: /u3 carries a ranges entry.
 */
static void
u3_dart_init(const char *path)
{
        phandle_t dnode;
        uint32_t props[2];
        char buf[128];

        if (!fw_cfg_read_i32(FW_CFG_PPC_DART)) {
                return;
        }

        fword("new-device");
        push_str("dart");
        fword("device-name");

        snprintf(buf, sizeof(buf), "%s/dart", path);
        dnode = find_dev(buf);

        set_property(dnode, "device_type", "dart", 5);
        set_property(dnode, "compatible", "u3-dart\0dart", 13);
        props[0] = __cpu_to_be32(IO_U3_DART_OFFSET);
        props[1] = __cpu_to_be32(IO_U3_DART_SIZE);
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
        u3_dart_init("/u3");

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

/* The K2's I2C bus, with the TAS3004 sound equalizer */
static void
k2_i2c_init(phys_addr_t addr, phandle_t mpic)
{
        phandle_t dnode;
        int props[2];

        fword("new-device");
        push_str("i2c");
        fword("device-name");
        dnode = get_cur_dev();
        set_property(dnode, "device_type", "i2c", 4);
        set_property(dnode, "compatible", "k2-i2c\0keywest-i2c", 19);
        set_property(dnode, "built-in", "", 0);
        props[0] = __cpu_to_be32(0x18000);
        props[1] = __cpu_to_be32(0x1000);
        set_property(dnode, "reg", (char *)&props, sizeof(props));
        props[0] = __cpu_to_be32(0x1a);
        props[1] = __cpu_to_be32(1);
        set_property(dnode, "interrupts", (char *)&props, sizeof(props));
        set_int_property(dnode, "interrupt-parent", mpic);
        set_int_property(dnode, "AAPL,address", addr + 0x18000);
        set_int_property(dnode, "AAPL,address-step", 0x10);
        set_int_property(dnode, "AAPL,i2c-rate", 100);
        set_property(dnode, "AAPL,driver-name", ".i2c-mac-io", 12);
        set_int_property(dnode, "#address-cells", 1);
        set_int_property(dnode, "#size-cells", 0);

        fword("new-device");
        push_str("deq");
        fword("device-name");
        dnode = get_cur_dev();
        set_property(dnode, "device_type", "deq", 4);
        set_int_property(dnode, "reg", 0x6a);
        set_int_property(dnode, "i2c-address", 0x6a);
        fword("finish-device");

        fword("finish-device");
}

static void
set_be32_property(phandle_t ph, const char *name, const uint32_t *v, int n)
{
        uint32_t buf[40];
        int i;

        for (i = 0; i < n; i++) {
                buf[i] = __cpu_to_be32(v[i]);
        }
        set_property(ph, name, (char *)buf, n * sizeof(uint32_t));
}

/* FCR1 I2S0 functions of the K2, as ON_DEMAND platform-do properties */
static const struct {
        const char *name;
        uint32_t cmd[5];
        int n;
} k2_i2s_pfuncs[] = {
        { "platform-do-enable",           { 0x08000000, 3, 0x3c, 0x2000, 0x2000 }, 5 },
        { "platform-do-disable",          { 0x08000000, 3, 0x3c, 0, 0x2000 }, 5 },
        { "platform-do-clock-enable",     { 0x08800000, 3, 0x3c, 0x1000, 0x1000 }, 5 },
        { "platform-do-clock-disable",    { 0x08800000, 3, 0x3c, 0, 0x1000 }, 5 },
        { "platform-do-sw-reset",         { 0x08000000, 3, 0x3c, 0x800, 0x800 }, 5 },
        { "platform-do-clear-sw-reset",   { 0x08000000, 3, 0x3c, 0, 0x800 }, 5 },
        { "platform-do-cell-enable",      { 0x08000000, 3, 0x3c, 0x400, 0x400 }, 5 },
        { "platform-do-cell-disable",     { 0x08000000, 3, 0x3c, 0, 0x400 }, 5 },
};

static const struct {
        const char *name;
        uint32_t mask, shift;
} k2_i2s_getfuncs[] = {
        { "platform-do-get-enable",       0x2000, 0xd },
        { "platform-do-get-clock-enable", 0x1000, 0xc },
        { "platform-do-get-sw-reset",     0x800,  0xb },
        { "platform-do-get-cell-enable",  0x400,  0xa },
};

static const char *k2_i2s_fnames[] = {
        "enable", "disable", "clock-enable", "clock-disable", "sw-reset",
        "clear-sw-reset", "cell-enable", "cell-disable", "get-enable",
        "get-clock-enable", "get-sw-reset", "get-cell-enable",
};

/* The K2's I2S-a sound cell, with a TAS3004 on the K2 I2C bus */
static void
k2_i2s_init(phys_addr_t addr, phandle_t mpic)
{
        static const uint32_t clock_data[] = {
                0x02b11000, 0x3c, 0x3400, 0x44, 0x4000, 0x44, 0x400, 0x44,
                4, 0x69303439, 0x6e756c6c, 0,
                0x02ee0000, 0x3c, 0x3400, 0x44, 0x4000, 0x44, 0x200, 0x44,
                2, 0x69303435, 0x6e756c6c, 0,
                0x01194000, 0x3c, 0x3000, 0x3c, 0x400, 0x44, 0x4000, 0,
                0, 0x6e756c6c, 0x6e756c6c, 0,
        };
        static const uint32_t clock_id[] = {
                0x69327330, 0x69303435, 0x69303439, 0x69303138,
        };
        static const uint32_t prio[] = { 2, 4, 4 };
        uint32_t props[8];
        phandle_t macio, i2s_a, dnode;
        char buf[40];
        unsigned int i;

        macio = get_cur_dev();

        fword("new-device");
        push_str("i2s");
        fword("device-name");
        dnode = get_cur_dev();
        set_property(dnode, "device_type", "i2s", 4);
        set_property(dnode, "built-in", "", 0);
        props[0] = 0x10000;
        props[1] = 0x1000;
        props[2] = 0x8000;
        props[3] = 0x400;
        set_be32_property(dnode, "reg", props, 4);
        set_int_property(dnode, "#address-cells", 1);
        set_property(dnode, "ranges", "", 0);
        set_int_property(dnode, "AAPL,address", addr + 0x10000);

        fword("new-device");
        push_str("i2s-a");
        fword("device-name");
        i2s_a = get_cur_dev();
        set_property(i2s_a, "device_type", "soundbus", 9);
        set_property(i2s_a, "compatible", "i2sbus", 7);
        set_property(i2s_a, "built-in", "", 0);
        props[0] = 0;
        props[1] = 0x1000;
        props[2] = 0;
        props[3] = 0x100;
        props[4] = 0x100;
        props[5] = 0x100;
        set_be32_property(i2s_a, "reg", props, 6);
        /*
         * Cell (level), then the TX and RX DMA (edge).  The real K2 uses 1
         * and 2 for the DMA, but QEMU's powermac7_3 keeps the mac-io IDE,
         * whose DMA takes 2 and 3, and the ESCC DMA takes 4..7: RX is on 8.
         * Must match K2_I2S_TX_DMA_IRQ / K2_I2S_RX_DMA_IRQ in QEMU.
         */
        props[0] = 0x1e;
        props[1] = 1;
        props[2] = 1;
        props[3] = 0;
        props[4] = 8;
        props[5] = 0;
        set_be32_property(i2s_a, "interrupts", props, 6);
        set_int_property(i2s_a, "interrupt-parent", mpic);
        set_be32_property(i2s_a, "AAPL,requested-priorities", prio, 3);
        set_be32_property(i2s_a, "AAPL,clock-id", clock_id, 4);
        set_be32_property(i2s_a, "AAPL,clock-data", clock_data,
                          sizeof(clock_data) / sizeof(clock_data[0]));
        for (i = 0; i < sizeof(k2_i2s_fnames) / sizeof(k2_i2s_fnames[0]); i++) {
                snprintf(buf, sizeof(buf), "platform-%s", k2_i2s_fnames[i]);
                set_int_property(i2s_a, buf, macio);
        }

        fword("new-device");
        push_str("sound");
        fword("device-name");
        dnode = get_cur_dev();
        set_property(dnode, "device_type", "soundchip", 10);
        set_property(dnode, "compatible", "AOAK2", 6);
        set_property(dnode, "built-in", "", 0);
        set_int_property(dnode, "layout-id", 36);
        set_int_property(dnode, "object-model-version", 2);
        set_int_property(dnode, "vendor-id", 0x106b);
        fword("finish-device");

        fword("finish-device");
        fword("finish-device");

        for (i = 0; i < sizeof(k2_i2s_pfuncs) / sizeof(k2_i2s_pfuncs[0]); i++) {
                props[0] = i2s_a;
                memcpy(&props[1], k2_i2s_pfuncs[i].cmd,
                       k2_i2s_pfuncs[i].n * sizeof(uint32_t));
                set_be32_property(macio, k2_i2s_pfuncs[i].name, props,
                                  k2_i2s_pfuncs[i].n + 1);
        }
        for (i = 0; i < sizeof(k2_i2s_getfuncs) / sizeof(k2_i2s_getfuncs[0]); i++) {
                props[0] = i2s_a;
                props[1] = 0x08000000;
                props[2] = 0x1a;
                props[3] = 0x3c;
                props[4] = k2_i2s_getfuncs[i].mask;
                props[5] = k2_i2s_getfuncs[i].shift;
                props[6] = 1;
                set_be32_property(macio, k2_i2s_getfuncs[i].name, props, 7);
        }
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
    if (find_dev("/u3")) {
        /*
         * The K2's sound cell and its i2c.  Both name the mpic as their
         * interrupt-parent, so this has to come after openpic_init; the
         * node is found by the PowerMac7,3 name.
         */
        char buf[128];
        phandle_t mpic;

        snprintf(buf, sizeof(buf), "%s/mpic", path);
        mpic = find_dev(buf);
        k2_i2s_init(addr, mpic);
        k2_i2c_init(addr, mpic);
    }

    aliases = find_dev("/aliases");
    set_property(aliases, "mac-io", path, strlen(path) + 1);
}
