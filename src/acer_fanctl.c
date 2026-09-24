// SPDX-License-Identifier: GPL-2.0-only
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/sysfs.h>
#include <linux/kobject.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/err.h>
#include "acer_ec_core.h"

#define DRV_NAME "acer_fanctl"

static struct kobject *fan_kobj;
static struct device *hwmon_dev;

/*
 * Tach constant 120,000,000 — the EC counts both edges of the tach
 * signal (dual-transition), so the raw period is halved vs the naive
 * 60M. Calibrated Sep 2026 against Acer's Windows utility (~5000 RPM
 * at max): CoolerBoost raw≈25104 -> ~4780, all-core ramp raw≈24327 ->
 * ~4934 — two independent max-state points converging. Settled-state
 * readings verified 35/35 stable; readings taken during fast RPM slews
 * can tear (the EC updates the 16-bit tach non-atomically) and inflated
 * transition values must not be trusted.
 */
static inline u16 raw_to_rpm(u16 raw)
{
	if (raw < 500)
		return 0;
	if (raw > 60000)
		return 0;
	return min_t(unsigned int, 120000000U / raw, 65535U);
}

/* --- sysfs read helpers --- */
#define FAN_SHOW8(name, off) \
static ssize_t name##_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf) \
{ return sysfs_emit(buf, "%d\n", (int)ec_core_read8(off)); }

#define FAN_SHOW16_RPM(name, off) \
static ssize_t name##_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf) \
{ return sysfs_emit(buf, "%u\n", (unsigned)raw_to_rpm(ec_core_read16(off))); }

FAN_SHOW8(fan1_duty, 0xCE)
FAN_SHOW8(fan2_duty, 0xCF)
FAN_SHOW16_RPM(fan1_rpm, 0xD0)
FAN_SHOW16_RPM(fan2_rpm, 0xD2)
FAN_SHOW16_RPM(fan4_rpm, 0xD4)
FAN_SHOW16_RPM(fan3_rpm, 0xE0)
FAN_SHOW8(dthl_val, 0xD7)
FAN_SHOW8(dtbp_val, 0xD8)
FAN_SHOW8(airp_val, 0xD9)
FAN_SHOW8(winf_val, 0xDA)
FAN_SHOW8(rinf_val, 0xDB)
FAN_SHOW8(tmp_temp, 0x07)

/* --- sysfs: profile (1-4) --- */
/*
 * Last profile successfully requested via SCMD (0 = unknown, e.g. the
 * init-time SCMD failed). The show side reports this value — not a
 * static legend — so userspace can verify a switch landed in the EC.
 */
static int current_profile;

static ssize_t profile_store(struct kobject *kobj, struct kobj_attribute *attr,
			     const char *buf, size_t count)
{
	u32 val;
	int ret = kstrtou32(buf, 0, &val);
	if (ret) return ret;
	if (val < 1 || val > 4) return -EINVAL;
	ret = ec_core_scmd(0x69, BIT(val - 1), 0, 0, 0);
	if (ret) return ret;
	current_profile = val;
	return count;
}

static ssize_t profile_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", current_profile);
}

/* --- sysfs: fan2_duty_set (0-255) --- */
/*
 * Read-modify-write is MANDATORY here: SCMD 0x68 programs both fan
 * channels in one call. A previous revision sent byte0=0, which latched
 * the CPU channel to 0 and stopped the CPU fan (verified live — package
 * kept climbing with rpm1=0 until CoolerBoost was cycled). Always
 * preserve the channel we are not changing.
 */
static ssize_t fan2_duty_set_store(struct kobject *kobj, struct kobj_attribute *attr,
				   const char *buf, size_t count)
{
	u32 val;
	int ret = kstrtou32(buf, 0, &val);
	if (ret) return ret;
	if (val > 255) return -EINVAL;
	ret = ec_core_scmd(0x68, ec_core_read8(0xCE), val, 0, 0);
	if (ret) return ret;
	return count;
}

/*
 * fan1_duty_set — DANGEROUS, not just experimental.
 * Verified live: SCMD 0x68 byte0 writes LATCH (they are not overwritten
 * by the EC within 500ms — that assumption was wrong). Writing 0 stops
 * the CPU fan and it stays stopped: profile switching does NOT restore
 * auto control. Recovery is Fn+1 CoolerBoost on/off, or writing a sane
 * duty here. Provided for research; understand the above first.
 */
static ssize_t fan1_duty_set_store(struct kobject *kobj, struct kobj_attribute *attr,
				   const char *buf, size_t count)
{
	u32 val;
	int ret = kstrtou32(buf, 0, &val);
	if (ret) return ret;
	if (val > 255) return -EINVAL;
	ret = ec_core_scmd(0x68, val, ec_core_read8(0xCF), 0, 0);
	if (ret) return ret;
	return count;
}

/* --- sysfs: dump all values at once --- */
static ssize_t all_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	u8 dut1 = ec_core_read8(0xCE);
	u8 dut2 = ec_core_read8(0xCF);
	u16 raw1 = ec_core_read16(0xD0);
	u16 raw2 = ec_core_read16(0xD2);
	u16 raw3 = ec_core_read16(0xE0);
	u16 raw4 = ec_core_read16(0xD4);
	u16 rpm1 = raw_to_rpm(raw1);
	u16 rpm2 = raw_to_rpm(raw2);
	u16 rpm3 = raw_to_rpm(raw3);
	u16 rpm4 = raw_to_rpm(raw4);
	u8 dthl = ec_core_read8(0xD7);
	u8 dtbp = ec_core_read8(0xD8);
	u8 airp = ec_core_read8(0xD9);
	u8 winf = ec_core_read8(0xDA);
	u8 rinf = ec_core_read8(0xDB);
	u8 tmp  = ec_core_read8(0x07);
	int len = 0;

	/*
	 * NOTE: multi-part output must use sysfs_emit_at(), not
	 * sysfs_emit(buf + len). The latter trips a WARN_ON inside
	 * sysfs_emit (fs/sysfs/file.c) on every read — observed live
	 * as "WARNING at sysfs_emit" from cat(1).
	 */
	len += sysfs_emit(buf, "dut1=%d dut2=%d\n", dut1, dut2);
	len += sysfs_emit_at(buf, len, "raw1=%u raw2=%u raw3=%u raw4=%u\n",
			      raw1, raw2, raw3, raw4);
	len += sysfs_emit_at(buf, len, "rpm1=%u rpm2=%u rpm3=%u rpm4=%u\n",
			      rpm1, rpm2, rpm3, rpm4);
	len += sysfs_emit_at(buf, len, "dthl=%d dtbp=%d airp=%d winf=%d rinf=%d\n",
			      dthl, dtbp, airp, winf, rinf);
	len += sysfs_emit_at(buf, len, "tmp=%d\n", tmp);
	len += sysfs_emit_at(buf, len, "profile=%d\n", current_profile);
	return len;
}

static struct kobj_attribute fan1_duty_attr = __ATTR_RO(fan1_duty);
static struct kobj_attribute fan2_duty_attr = __ATTR_RO(fan2_duty);
static struct kobj_attribute fan1_rpm_attr = __ATTR_RO(fan1_rpm);
static struct kobj_attribute fan2_rpm_attr = __ATTR_RO(fan2_rpm);
static struct kobj_attribute fan3_rpm_attr = __ATTR_RO(fan3_rpm);
static struct kobj_attribute fan4_rpm_attr = __ATTR_RO(fan4_rpm);
static struct kobj_attribute dthl_val_attr = __ATTR_RO(dthl_val);
static struct kobj_attribute dtbp_val_attr = __ATTR_RO(dtbp_val);
static struct kobj_attribute airp_val_attr = __ATTR_RO(airp_val);
static struct kobj_attribute winf_val_attr = __ATTR_RO(winf_val);
static struct kobj_attribute rinf_val_attr = __ATTR_RO(rinf_val);
static struct kobj_attribute tmp_temp_attr = __ATTR_RO(tmp_temp);
static struct kobj_attribute profile_attr = __ATTR_RW(profile);
static struct kobj_attribute fan2_duty_set_attr = __ATTR_WO(fan2_duty_set);
static struct kobj_attribute fan1_duty_set_attr = __ATTR_WO(fan1_duty_set);
static struct kobj_attribute all_attr = __ATTR_RO(all);

static struct attribute *attrs[] = {
	&fan1_duty_attr.attr,
	&fan2_duty_attr.attr,
	&fan1_rpm_attr.attr,
	&fan2_rpm_attr.attr,
	&fan3_rpm_attr.attr,
	&fan4_rpm_attr.attr,
	&dthl_val_attr.attr,
	&dtbp_val_attr.attr,
	&airp_val_attr.attr,
	&winf_val_attr.attr,
	&rinf_val_attr.attr,
	&tmp_temp_attr.attr,
	&profile_attr.attr,
	&fan2_duty_set_attr.attr,
	&fan1_duty_set_attr.attr,
	&all_attr.attr,
	NULL,
};

static struct attribute_group attr_group = {
	.attrs = attrs,
};

/* --- hwmon interface (lm_sensors) --- */
static ssize_t acer_hwmon_val_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	long val = 0;

	if (strcmp(attr->attr.name, "temp1_input") == 0)
		val = ec_core_read8(0x07) * 1000;
	else if (strcmp(attr->attr.name, "fan1_input") == 0)
		val = raw_to_rpm(ec_core_read16(0xD0));
	else if (strcmp(attr->attr.name, "fan2_input") == 0)
		val = raw_to_rpm(ec_core_read16(0xD2));

	return sprintf(buf, "%ld\n", val);
}

static SENSOR_DEVICE_ATTR_RO(temp1_input, acer_hwmon_val, 0);
static SENSOR_DEVICE_ATTR_RO(fan1_input, acer_hwmon_val, 0);
static SENSOR_DEVICE_ATTR_RO(fan2_input, acer_hwmon_val, 0);

static struct attribute *acer_hwmon_attrs[] = {
	&sensor_dev_attr_temp1_input.dev_attr.attr,
	&sensor_dev_attr_fan1_input.dev_attr.attr,
	&sensor_dev_attr_fan2_input.dev_attr.attr,
	NULL
};

static const struct attribute_group acer_hwmon_group = {
	.attrs = acer_hwmon_attrs,
};

static const struct attribute_group *acer_hwmon_groups[] = {
	&acer_hwmon_group,
	NULL
};

static int profile_param = 2;
module_param(profile_param, int, 0444);
MODULE_PARM_DESC(profile_param, "Default fan profile (1=quiet 2=balanced 3=performance 4=gaming)");

static int __init acer_fanctl_init(void)
{
	int ret;

	fan_kobj = kobject_create_and_add("acer_fanctl", kernel_kobj);
	if (!fan_kobj)
		return -ENOMEM;

	ret = sysfs_create_group(fan_kobj, &attr_group);
	if (ret) {
		kobject_put(fan_kobj);
		return ret;
	}

	if (profile_param >= 1 && profile_param <= 4) {
		ret = ec_core_scmd(0x69, BIT(profile_param - 1), 0, 0, 0);
		if (ret) {
			pr_err("failed to apply initial profile %d (err %d), EC keeps firmware default\n",
			       profile_param, ret);
			current_profile = 0;
		} else {
			current_profile = profile_param;
		}
	} else {
		pr_warn("invalid profile_param=%d, EC keeps firmware default\n",
			profile_param);
		current_profile = 0;
	}

	hwmon_dev = hwmon_device_register_with_groups(NULL, "acer_ec",
						      NULL, acer_hwmon_groups);
	if (IS_ERR(hwmon_dev)) {
		pr_warn("hwmon registration failed (%ld), sensors won't see it\n",
			PTR_ERR(hwmon_dev));
		hwmon_dev = NULL;
	}

	pr_info("loaded (profile=%d) - see /sys/kernel/acer_fanctl/\n", current_profile);
	return 0;
}

static void __exit acer_fanctl_exit(void)
{
	if (hwmon_dev)
		hwmon_device_unregister(hwmon_dev);
	sysfs_remove_group(fan_kobj, &attr_group);
	kobject_put(fan_kobj);
	pr_info("unloaded\n");
}

module_init(acer_fanctl_init);
module_exit(acer_fanctl_exit);

MODULE_VERSION("0.1");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Acer Aspire A715-79G community");
MODULE_DESCRIPTION("Acer A715-79G fan control via acer_ec_core");

MODULE_SOFTDEP("pre: acer_ec_core");
