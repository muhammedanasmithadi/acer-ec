// SPDX-License-Identifier: GPL-2.0-only
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/debugfs.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include "acer_ec_core.h"

static struct dentry *df_root;

#define EC_SIZE 0x100

/*
 * Read-only EC SystemMemory explorer. Each register file carries its
 * offset in the dentry's i_private (set at creation via the data
 * pointer); nothing parses dentries. Offsets mirror the fanctl map —
 * see docs/reverse-engineering.md.
 */
struct ec_dbg_reg {
	const char *name;
	u16 offset;
	bool wide;
};

static const struct ec_dbg_reg ec_dbg_regs[] = {
	{ "reg8_00", 0x00, false },
	{ "reg8_07", 0x07, false },	/* TMP: ACPI temperature */
	{ "reg8_CE", 0xCE, false },	/* DUT1: fan 1 EC state */
	{ "reg8_CF", 0xCF, false },	/* DUT2: fan 2 EC state */
	{ "reg8_D7", 0xD7, false },
	{ "reg8_D8", 0xD8, false },
	{ "reg8_D9", 0xD9, false },
	{ "reg8_DA", 0xDA, false },
	{ "reg8_DB", 0xDB, false },
	{ "reg16_D0", 0xD0, true },	/* RPM1 tach period */
	{ "reg16_D2", 0xD2, true },	/* RPM2 tach period */
	{ "reg16_E0", 0xE0, true },	/* RPM3 (unused) */
	{ "reg16_D4", 0xD4, true },	/* RPM4 (unused) */
	/*
	 * Battery block (0x10-0x3F) — _BIF mirror, see
	 * docs/reverse-engineering.md. All 16-bit words little-endian.
	 */
	{ "reg16_16", 0x16, true },	/* Design capacity, mAh */
	{ "reg16_1A", 0x1A, true },	/* Full capacity, mAh */
	{ "reg16_22", 0x22, true },	/* Pack voltage, mV */
	{ "reg16_2A", 0x2A, true },	/* Charge current, mA */
	{ "reg16_2E", 0x2E, true },	/* Present charge, mAh */
	{ "reg8_38", 0x38, false },	/* State of charge, % */
	{ "reg16_66", 0x66, true },	/* Charge cutoff, mV */
};

static ssize_t reg_read(struct file *filp, char __user *buf,
			size_t count, loff_t *pos, bool wide)
{
	char tmp[16];
	/*
	 * NOTE: use file_inode()->i_private, not filp->private_data.
	 * Verified live on 7.2.7: debugfs does not propagate the
	 * create_file() data pointer into private_data for custom
	 * fops here (every file silently read offset 0x00 while the
	 * dump path showed the true values). i_private is authoritative.
	 */
	u16 off = (u16)(uintptr_t)file_inode(filp)->i_private;
	size_t len;

	if (off >= (wide ? EC_SIZE - 1 : EC_SIZE))
		return -ERANGE;

	if (wide) {
		u16 val = ec_core_read16(off);

		len = scnprintf(tmp, sizeof(tmp), "0x%04x (%u)\n", val, val);
	} else {
		u8 val = ec_core_read8(off);

		len = scnprintf(tmp, sizeof(tmp), "0x%02x (%u)\n", val, val);
	}
	return simple_read_from_buffer(buf, count, pos, tmp, len);
}

static ssize_t reg8_read(struct file *filp, char __user *buf,
			 size_t count, loff_t *pos)
{
	return reg_read(filp, buf, count, pos, false);
}

static ssize_t reg16_read(struct file *filp, char __user *buf,
			  size_t count, loff_t *pos)
{
	return reg_read(filp, buf, count, pos, true);
}

static ssize_t dump_read(struct file *filp, char __user *buf,
			 size_t count, loff_t *pos)
{
	char *tmp;
	int i, n = 0;
	ssize_t ret;

	tmp = kmalloc(4096, GFP_KERNEL);
	if (!tmp)
		return -ENOMEM;
	for (i = 0; i < EC_SIZE; i += 16) {
		int j, w;
		size_t avail;

		if ((size_t)n >= 4096 - 64)
			break;
		avail = 4096 - n;
		w = snprintf(tmp + n, avail, "%02x: ", i);
		if (w < 0 || (size_t)w >= avail)
			break;
		n += w;
		for (j = 0; j < 16 && i + j < EC_SIZE; j++) {
			avail = 4096 - n;
			w = snprintf(tmp + n, avail, "%02x ",
				     ec_core_read8(i + j));
			if (w < 0 || (size_t)w >= avail)
				break;
			n += w;
		}
		if ((size_t)n >= 4096 - 2)
			break;
		tmp[n++] = '\n';
	}
	ret = simple_read_from_buffer(buf, count, pos, tmp, n);
	kfree(tmp);
	return ret;
}

/*
 * Decoded battery block in one read — the stuck-vs-charging diff unit.
 * Offsets from docs/reverse-engineering.md; read-only, no EC command.
 */
static ssize_t batt_read(struct file *filp, char __user *buf,
			 size_t count, loff_t *pos)
{
	char tmp[192];
	char chem[5];
	u16 design = ec_core_read16(0x16);
	u16 full = ec_core_read16(0x1A);
	u16 pack_mv = ec_core_read16(0x22);
	u16 charge_ma = ec_core_read16(0x2A);
	u16 present = ec_core_read16(0x2E);
	u8 soc = ec_core_read8(0x38);
	u16 cutoff = ec_core_read16(0x66);
	size_t len;

	chem[0] = ec_core_read8(0x5E);
	chem[1] = ec_core_read8(0x5F);
	chem[2] = ec_core_read8(0x60);
	chem[3] = ec_core_read8(0x61);
	chem[4] = '\0';

	len = scnprintf(tmp, sizeof(tmp),
			"design_mah=%u full_mah=%u pack_mv=%u charge_ma=%u\n"
			"present_mah=%u soc_pct=%u cutoff_mv=%u chem=%s\n",
			design, full, pack_mv, charge_ma,
			present, soc, cutoff, chem);
	return simple_read_from_buffer(buf, count, pos, tmp, len);
}

static const struct file_operations reg8_fops = {
	.owner = THIS_MODULE,
	.read = reg8_read,
};

static const struct file_operations reg16_fops = {
	.owner = THIS_MODULE,
	.read = reg16_read,
};

static const struct file_operations dump_fops = {
	.owner = THIS_MODULE,
	.read = dump_read,
};

static const struct file_operations batt_fops = {
	.owner = THIS_MODULE,
	.read = batt_read,
};

static int __init acer_ec_debug_init(void)
{
	struct dentry *f;
	int i;

	df_root = debugfs_create_dir("acer_ec", NULL);
	if (IS_ERR_OR_NULL(df_root))
		return -ENOMEM;

	for (i = 0; i < ARRAY_SIZE(ec_dbg_regs); i++) {
		f = debugfs_create_file(ec_dbg_regs[i].name, 0444, df_root,
					(void *)(uintptr_t)ec_dbg_regs[i].offset,
					ec_dbg_regs[i].wide ? &reg16_fops : &reg8_fops);
		if (IS_ERR(f)) {
			debugfs_remove_recursive(df_root);
			return PTR_ERR(f);
		}
	}

	f = debugfs_create_file("dump", 0444, df_root, NULL, &dump_fops);
	if (IS_ERR(f)) {
		debugfs_remove_recursive(df_root);
		return PTR_ERR(f);
	}

	f = debugfs_create_file("batt", 0444, df_root, NULL, &batt_fops);
	if (IS_ERR(f)) {
		debugfs_remove_recursive(df_root);
		return PTR_ERR(f);
	}

	pr_info("loaded — see /sys/kernel/debug/acer_ec/\n");
	return 0;
}

static void __exit acer_ec_debug_exit(void)
{
	debugfs_remove_recursive(df_root);
	pr_info("unloaded\n");
}

module_init(acer_ec_debug_init);
module_exit(acer_ec_debug_exit);

MODULE_VERSION("1.0");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Acer Aspire A715-79G community");
MODULE_DESCRIPTION("Acer EC debug — read-only EC SystemMemory explorer via debugfs");
MODULE_SOFTDEP("pre: acer_ec_core");
