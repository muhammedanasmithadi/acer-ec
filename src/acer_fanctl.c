// SPDX-License-Identifier: GPL-2.0-only
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/sysfs.h>
#include <linux/kobject.h>
#include <linux/mutex.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/err.h>
#include "acer_ec_core.h"

/*
 * EC SystemMemory map (base 0xFE0B0100, see acer_ec_core).
 * Offsets from DSDT + live register testing; full layout in
 * docs/reverse-engineering.md. RPM3 sits above RPM4 in the address
 * space — that matches the firmware layout, not a typo.
 */
#define EC_REG_TMP	0x07
#define EC_REG_DUT1	0xCE
#define EC_REG_DUT2	0xCF
#define EC_REG_RPM1	0xD0
#define EC_REG_RPM2	0xD2
#define EC_REG_RPM4	0xD4
#define EC_REG_RPM3	0xE0

/* SCMD command IDs (\\_SB.WMI SCMD dispatcher) */
#define SCMD_WRITE_DUTY	0x68
#define SCMD_SET_PROFILE	0x69

static struct kobject *fan_kobj;
static struct device *hwmon_dev;

/*
 * Serializes profile switches and duty read-modify-writes against each
 * other, and guards current_profile. (ec_core_scmd has its own inner
 * lock; acquisition order is always fanctl_lock -> scmd_lock.)
 */
static DEFINE_MUTEX(fanctl_lock);

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
	/* Raw 500-1831 would compute above u16 range (tear values, not real
	 * fans) — saturate at 65535 rather than wrapping. */
	return min_t(unsigned int, 120000000U / raw, 65535U);
}

/* --- sysfs read helpers --- */
#define FAN_SHOW8(name, off) \
static ssize_t name##_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf) \
{ return sysfs_emit(buf, "%d\n", (int)ec_core_read8(off)); }

#define FAN_SHOW16_RPM(name, off) \
static ssize_t name##_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf) \
{ return sysfs_emit(buf, "%u\n", (unsigned int)raw_to_rpm(ec_core_read16(off))); }

FAN_SHOW8(fan1_duty, EC_REG_DUT1)
FAN_SHOW8(fan2_duty, EC_REG_DUT2)
FAN_SHOW16_RPM(fan1_rpm, EC_REG_RPM1)
FAN_SHOW16_RPM(fan2_rpm, EC_REG_RPM2)
FAN_SHOW16_RPM(fan4_rpm, EC_REG_RPM4)
FAN_SHOW16_RPM(fan3_rpm, EC_REG_RPM3)
FAN_SHOW8(dthl_val, 0xD7)
FAN_SHOW8(dtbp_val, 0xD8)
FAN_SHOW8(airp_val, 0xD9)
FAN_SHOW8(winf_val, 0xDA)
FAN_SHOW8(rinf_val, 0xDB)
FAN_SHOW8(tmp_temp, EC_REG_TMP)

/* --- sysfs: profile (1-4) --- */
/*
 * Last profile successfully requested via SCMD (0 = unknown, e.g. the
 * init-time SCMD failed). The show side reports this value — not a
 * static legend — so userspace can verify a switch landed in the EC.
 * Guarded by fanctl_lock.
 */
static int current_profile;

static ssize_t profile_store(struct kobject *kobj, struct kobj_attribute *attr,
			     const char *buf, size_t count)
{
	u32 val;
	int ret;

	ret = kstrtou32(buf, 0, &val);
	if (ret)
		return ret;
	if (val < 1 || val > 4)
		return -EINVAL;

	mutex_lock(&fanctl_lock);
	ret = ec_core_scmd(SCMD_SET_PROFILE, BIT(val - 1), 0, 0, 0);
	if (ret) {
		mutex_unlock(&fanctl_lock);
		return ret;
	}
	current_profile = val;
	mutex_unlock(&fanctl_lock);

	return count;
}

static ssize_t profile_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	int profile;

	mutex_lock(&fanctl_lock);
	profile = current_profile;
	mutex_unlock(&fanctl_lock);

	return sysfs_emit(buf, "%d\n", profile);
}

/* --- sysfs: fan2_duty_set (0-255, GPU fan only) --- */
/*
 * Read-modify-write is MANDATORY here: SCMD 0x68 programs both fan
 * channels in one call. A previous revision sent byte0=0, which latched
 * the CPU channel to 0 and stopped the CPU fan (verified live — package
 * kept climbing with rpm1=0 until CoolerBoost was cycled). Always
 * preserve the CPU channel.
 *
 * There is deliberately no fan1_duty_set: SCMD 0x68 byte0 writes latch
 * the CPU fan off persistently (profile switching does NOT restore
 * auto control). Removed rather than documented — see git history.
 *
 * The read and the SCMD run under fanctl_lock so two concurrent
 * writers cannot interleave into a torn channel pair.
 */
static int ec_set_gpu_duty(u8 val)
{
	u8 dut1;
	int ret;

	mutex_lock(&fanctl_lock);
	dut1 = ec_core_read8(EC_REG_DUT1);
	ret = ec_core_scmd(SCMD_WRITE_DUTY, dut1, val, 0, 0);
	mutex_unlock(&fanctl_lock);

	return ret;
}

static ssize_t fan2_duty_set_store(struct kobject *kobj, struct kobj_attribute *attr,
				   const char *buf, size_t count)
{
	u32 val;
	int ret;

	ret = kstrtou32(buf, 0, &val);
	if (ret)
		return ret;
	if (val > 255)
		return -EINVAL;

	ret = ec_set_gpu_duty(val);
	if (ret)
		return ret;

	return count;
}

/* --- sysfs: dump all values at once --- */
static ssize_t all_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	u8 dut1 = ec_core_read8(EC_REG_DUT1);
	u8 dut2 = ec_core_read8(EC_REG_DUT2);
	u16 raw1 = ec_core_read16(EC_REG_RPM1);
	u16 raw2 = ec_core_read16(EC_REG_RPM2);
	u16 raw3 = ec_core_read16(EC_REG_RPM3);
	u16 raw4 = ec_core_read16(EC_REG_RPM4);
	u16 rpm1 = raw_to_rpm(raw1);
	u16 rpm2 = raw_to_rpm(raw2);
	u16 rpm3 = raw_to_rpm(raw3);
	u16 rpm4 = raw_to_rpm(raw4);
	u8 dthl = ec_core_read8(0xD7);
	u8 dtbp = ec_core_read8(0xD8);
	u8 airp = ec_core_read8(0xD9);
	u8 winf = ec_core_read8(0xDA);
	u8 rinf = ec_core_read8(0xDB);
	u8 tmp  = ec_core_read8(EC_REG_TMP);
	int profile;
	int len = 0;

	mutex_lock(&fanctl_lock);
	profile = current_profile;
	mutex_unlock(&fanctl_lock);

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
	len += sysfs_emit_at(buf, len, "profile=%d\n", profile);
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
static struct kobj_attribute all_attr = __ATTR_RO(all);

static struct attribute *acer_fanctl_attrs[] = {
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
	&all_attr.attr,
	NULL,
};
ATTRIBUTE_GROUPS(acer_fanctl);

/* --- hwmon interface (lm_sensors) --- */
enum acer_hwmon_channel {
	ACER_HWMON_TEMP = 0,
	ACER_HWMON_FAN1,
	ACER_HWMON_FAN2,
};

static ssize_t acer_hwmon_val_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct sensor_device_attribute_2 *sattr = to_sensor_dev_attr_2(attr);
	long val;

	switch (sattr->nr) {
	case ACER_HWMON_TEMP:
		val = ec_core_read8(EC_REG_TMP) * 1000;
		break;
	case ACER_HWMON_FAN1:
		val = raw_to_rpm(ec_core_read16(EC_REG_RPM1));
		break;
	case ACER_HWMON_FAN2:
		val = raw_to_rpm(ec_core_read16(EC_REG_RPM2));
		break;
	default:
		return -EINVAL;
	}

	return sysfs_emit(buf, "%ld\n", val);
}

static SENSOR_DEVICE_ATTR_2_RO(temp1_input, acer_hwmon_val, ACER_HWMON_TEMP, 0);
static SENSOR_DEVICE_ATTR_2_RO(fan1_input, acer_hwmon_val, ACER_HWMON_FAN1, 0);
static SENSOR_DEVICE_ATTR_2_RO(fan2_input, acer_hwmon_val, ACER_HWMON_FAN2, 0);

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
MODULE_PARM_DESC(profile_param, "Default fan profile (1=quiet 2=balanced 3=performance 4=gaming, default 2; install.sh sets 4)");

static int __init acer_fanctl_init(void)
{
	int ret;

	fan_kobj = kobject_create_and_add("acer_fanctl", kernel_kobj);
	if (!fan_kobj)
		return -ENOMEM;

	ret = sysfs_create_groups(fan_kobj, acer_fanctl_groups);
	if (ret) {
		kobject_put(fan_kobj);
		return ret;
	}

	if (profile_param >= 1 && profile_param <= 4) {
		ret = ec_core_scmd(SCMD_SET_PROFILE, BIT(profile_param - 1), 0, 0, 0);
		mutex_lock(&fanctl_lock);
		if (ret) {
			pr_err("failed to apply initial profile %d (err %d), EC keeps firmware default\n",
			       profile_param, ret);
			current_profile = 0;
		} else {
			current_profile = profile_param;
		}
		ret = current_profile;
		mutex_unlock(&fanctl_lock);
	} else {
		pr_warn("invalid profile_param=%d, EC keeps firmware default\n",
			profile_param);
		mutex_lock(&fanctl_lock);
		current_profile = 0;
		ret = 0;
		mutex_unlock(&fanctl_lock);
	}

	hwmon_dev = hwmon_device_register_with_groups(NULL, "acer_ec",
						      NULL, acer_hwmon_groups);
	if (IS_ERR(hwmon_dev)) {
		pr_warn("hwmon registration failed (%ld), sensors won't see it\n",
			PTR_ERR(hwmon_dev));
		hwmon_dev = NULL;
	}

	pr_info("loaded (profile=%d) - see /sys/kernel/acer_fanctl/\n", ret);
	return 0;
}

static void __exit acer_fanctl_exit(void)
{
	if (hwmon_dev)
		hwmon_device_unregister(hwmon_dev);
	sysfs_remove_groups(fan_kobj, acer_fanctl_groups);
	kobject_put(fan_kobj);
	pr_info("unloaded\n");
}

module_init(acer_fanctl_init);
module_exit(acer_fanctl_exit);

MODULE_VERSION("1.0");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Acer Aspire A715-79G community");
MODULE_DESCRIPTION("Acer A715-79G fan control via acer_ec_core");

MODULE_SOFTDEP("pre: acer_ec_core");
