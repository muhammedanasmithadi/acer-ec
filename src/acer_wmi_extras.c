// SPDX-License-Identifier: GPL-2.0-only
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/wmi.h>
#include <linux/device.h>
#include <linux/input.h>
#include <linux/acpi.h>
#include <linux/string.h>

#define ACER_WMI_GUID_WEBCAM "ABBC0F6C-8EA1-11D1-00A0-C90629100000"
/* 0x6D is the WMI control/event channel (DSDT: WMBB dispatch). Its exact
 * event semantics are not decoded yet, so we only log its notifications
 * and never synthesize input for them. */
#define ACER_WMI_GUID_EVENT  "ABBC0F6D-8EA1-11D1-00A0-C90629100000"

static struct input_dev *acer_wmi_extras_input_dev;

/* The wmi_device whose notifications map to KEY_CAMERA, resolved at
 * probe time by matching the device name (which starts with the WMI
 * GUID, e.g. "ABBC0F6C-..."). Pointer comparison at event time avoids
 * any string parsing in the notify path. */
static struct wmi_device *webcam_wdev;

static void acer_wmi_extras_notify(struct wmi_device *wdev, union acpi_object *data)
{
	u32 val = 0;

	if (!data) {
		dev_dbg(&wdev->dev, "event with no data\n");
		return;
	}

	if (data->type == ACPI_TYPE_INTEGER)
		val = data->integer.value;

	dev_info(&wdev->dev, "event: type=%d val=%d\n", data->type, val);

	/*
	 * Only the webcam GUID's notifications map to KEY_CAMERA. The
	 * event channel (0x6D) carries unrelated EC notifications
	 * (thermal, AC, profile changes) — emitting a camera keypress
	 * for those would toggle the camera on unrelated events.
	 */
	if (wdev == webcam_wdev && acer_wmi_extras_input_dev) {
		input_report_key(acer_wmi_extras_input_dev, KEY_CAMERA, 1);
		input_sync(acer_wmi_extras_input_dev);
		input_report_key(acer_wmi_extras_input_dev, KEY_CAMERA, 0);
		input_sync(acer_wmi_extras_input_dev);
	}
}

static int acer_wmi_extras_probe(struct wmi_device *wdev, const void *context)
{
	dev_info(&wdev->dev, "claimed\n");
	if (!strncmp(dev_name(&wdev->dev), ACER_WMI_GUID_WEBCAM,
		     strlen(ACER_WMI_GUID_WEBCAM)))
		webcam_wdev = wdev;
	return 0;
}

static const struct wmi_device_id acer_wmi_extras_id_table[] = {
	{ ACER_WMI_GUID_WEBCAM, NULL },
	{ ACER_WMI_GUID_EVENT,  NULL },
	{ }
};

static struct wmi_driver acer_wmi_extras_driver = {
	.driver = {
		.name = "acer_wmi_extras",
	},
	.id_table = acer_wmi_extras_id_table,
	.probe = acer_wmi_extras_probe,
	.notify = acer_wmi_extras_notify,
};

static int __init acer_wmi_extras_init(void)
{
	int err;

	acer_wmi_extras_input_dev = input_allocate_device();
	if (!acer_wmi_extras_input_dev)
		return -ENOMEM;

	acer_wmi_extras_input_dev->name = "Acer WMI hotkeys";
	acer_wmi_extras_input_dev->phys = "wmi/input0";
	acer_wmi_extras_input_dev->id.bustype = BUS_HOST;
	acer_wmi_extras_input_dev->dev.parent = NULL;

	set_bit(EV_KEY, acer_wmi_extras_input_dev->evbit);
	set_bit(KEY_CAMERA, acer_wmi_extras_input_dev->keybit);

	err = input_register_device(acer_wmi_extras_input_dev);
	if (err) {
		input_free_device(acer_wmi_extras_input_dev);
		acer_wmi_extras_input_dev = NULL;
		return err;
	}

	err = wmi_driver_register(&acer_wmi_extras_driver);
	if (err) {
		input_unregister_device(acer_wmi_extras_input_dev);
		acer_wmi_extras_input_dev = NULL;
		return err;
	}

	pr_info("loaded\n");
	return 0;
}

static void __exit acer_wmi_extras_exit(void)
{
	wmi_driver_unregister(&acer_wmi_extras_driver);
	webcam_wdev = NULL;
	if (acer_wmi_extras_input_dev) {
		input_unregister_device(acer_wmi_extras_input_dev);
		acer_wmi_extras_input_dev = NULL;
	}
	pr_info("unloaded\n");
}

module_init(acer_wmi_extras_init);
module_exit(acer_wmi_extras_exit);

MODULE_VERSION("0.1");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Anas");
MODULE_DESCRIPTION("Acer WMI driver for unclaimed GUIDs");
