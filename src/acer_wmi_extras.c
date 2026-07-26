// SPDX-License-Identifier: GPL-2.0-only
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/wmi.h>
#include <linux/device.h>
#include <linux/input.h>
#include <linux/acpi.h>

#define ACER_WMI_GUID_WEBCAM "ABBC0F6C-8EA1-11D1-00A0-C90629100000"
#define ACER_WMI_GUID_TDP    "ABBC0F6D-8EA1-11D1-00A0-C90629100000"

static struct input_dev *acer_wmi_extras_input_dev;

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

	/* Report a KEY_CAMERA event so userspace can handle the toggle */
	if (acer_wmi_extras_input_dev) {
		input_report_key(acer_wmi_extras_input_dev, KEY_CAMERA, 1);
		input_sync(acer_wmi_extras_input_dev);
		input_report_key(acer_wmi_extras_input_dev, KEY_CAMERA, 0);
		input_sync(acer_wmi_extras_input_dev);
	}
}

static int acer_wmi_extras_probe(struct wmi_device *wdev, const void *context)
{
	dev_info(&wdev->dev, "claimed\n");
	return 0;
}

static const struct wmi_device_id acer_wmi_extras_id_table[] = {
	{ ACER_WMI_GUID_WEBCAM, NULL },
	{ ACER_WMI_GUID_TDP,    NULL },
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
	if (acer_wmi_extras_input_dev) {
		input_unregister_device(acer_wmi_extras_input_dev);
		acer_wmi_extras_input_dev = NULL;
	}
	pr_info("unloaded\n");
}

module_init(acer_wmi_extras_init);
module_exit(acer_wmi_extras_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Anas");
MODULE_DESCRIPTION("Acer WMI driver for unclaimed GUIDs");
