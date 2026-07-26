// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Minimal ACPI EC driver for the MECHREVO WUJIE 14 GX4HRXL.
 *
 * The firmware exposes byte EC access through INOU0000.ECRR/ECRW. This
 * driver intentionally contains no dGPU, lightbar or multi-model code. It
 * owns the AP-exists lifecycle, exposes a narrow root-only EC device for the
 * userspace controller, and registers the small set of stable Linux-native
 * interfaces used by this machine.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/acpi.h>
#include <linux/atomic.h>
#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/dmi.h>
#include <linux/fs.h>
#include <linux/hwmon.h>
#include <linux/input.h>
#include <linux/input/sparse-keymap.h>
#include <linux/leds.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/platform_profile.h>
#include <linux/pm.h>
#include <linux/uaccess.h>
#include <linux/wmi.h>

#include "mechrevo_ec_uapi.h"

#define DRIVER_NAME			"mechrevo-ec"
#define DEVICE_NAME			"mechrevo-ec"
#define MECHREVO_EVENT_GUID		"ABBC0F72-8EA1-11D1-00A0-C90629100000"

#define EC_MAX_ADDR			0x0fff
#define EC_DELAY_US			6000

#define EC_ADDR_CPU_TEMP		0x043e
#define EC_ADDR_MAIN_FAN_RPM_HI		0x0464
#define EC_ADDR_MAIN_FAN_RPM_LO		0x0465
/* GX4HRXL stores the secondary fan as low byte first, then high byte. */
#define EC_ADDR_SECOND_FAN_RPM_LO	0x046b
#define EC_ADDR_SECOND_FAN_RPM_HI	0x046c
#define EC_ADDR_PROJECT_ID		0x0740
#define EC_ADDR_AP_OEM			0x0741
#define EC_ADDR_MODE_CTL		0x0751
#define EC_ADDR_MAIN_FAN_DUTY		0x075b
#define EC_ADDR_SECOND_FAN_DUTY		0x075c
#define EC_ADDR_KBD_BACKLIGHT		0x078c

#define AP_EXISTS			BIT(0)
#define MODE_MASK			0xb0
#define FAN_BOOST			BIT(6)
#define MODE_OFFICE			0xa0
#define MODE_GAMING			0x00
#define MODE_TURBO			0x10

#define KBD_LEVEL_MASK			GENMASK(7, 5)
#define KBD_APPLY			BIT(4)
#define KBD_POWER_OFF			BIT(1)
#define KBD_MAX_LEVEL			4

#define OSD_KB_LED_LEVEL0		0x3b
#define OSD_KB_LED_LEVEL1		0x3c
#define OSD_KB_LED_LEVEL2		0x3d
#define OSD_KB_LED_LEVEL3		0x3e
#define OSD_KB_LED_LEVEL4		0x3f
#define OSD_PERFORMANCE_MODE_TOGGLE	0xb0
#define OSD_KBDILLUMDOWN		0xb1
#define OSD_KBDILLUMUP			0xb2
#define OSD_BACKLIGHT_LEVEL_CHANGE	0xb3
#define OSD_KBDILLUMTOGGLE		0xb9
#define OSD_KBD_BACKLIGHT_CHANGED	0xf0

struct mechrevo_ec {
	struct device *dev;
	acpi_handle handle;
	struct mutex io_lock;
	atomic_t misc_open;
	struct miscdevice miscdev;
	struct led_classdev kbd_backlight;
	struct input_dev *input;
	struct device *profile_dev;
	u8 suspend_mode;
	u8 suspend_backlight;
	bool suspend_mode_valid;
	bool suspend_backlight_valid;
};

static DEFINE_MUTEX(global_data_lock);
static struct mechrevo_ec *global_data;

static int mechrevo_ec_read_unlocked(struct mechrevo_ec *ec, u16 addr, u8 *value)
{
	union acpi_object param = {
		.integer = {
			.type = ACPI_TYPE_INTEGER,
			.value = addr,
		},
	};
	struct acpi_object_list input = {
		.count = 1,
		.pointer = &param,
	};
	unsigned long long output;
	acpi_status status;

	if (addr > EC_MAX_ADDR)
		return -EINVAL;

	status = acpi_evaluate_integer(ec->handle, "ECRR", &input, &output);
	if (ACPI_FAILURE(status))
		return -EIO;
	if (output > U8_MAX)
		return -ERANGE;

	usleep_range(EC_DELAY_US, EC_DELAY_US * 2);
	*value = output;
	return 0;
}

static int mechrevo_ec_write_unlocked(struct mechrevo_ec *ec, u16 addr, u8 value)
{
	union acpi_object params[2] = {
		{
			.integer = {
				.type = ACPI_TYPE_INTEGER,
				.value = addr,
			},
		},
		{
			.integer = {
				.type = ACPI_TYPE_INTEGER,
				.value = value,
			},
		},
	};
	struct acpi_object_list input = {
		.count = ARRAY_SIZE(params),
		.pointer = params,
	};
	acpi_status status;

	if (addr > EC_MAX_ADDR)
		return -EINVAL;

	status = acpi_evaluate_object(ec->handle, "ECRW", &input, NULL);
	if (ACPI_FAILURE(status))
		return -EIO;

	usleep_range(EC_DELAY_US, EC_DELAY_US * 2);
	return 0;
}

static int mechrevo_ec_read(struct mechrevo_ec *ec, u16 addr, u8 *value)
{
	int ret;

	mutex_lock(&ec->io_lock);
	ret = mechrevo_ec_read_unlocked(ec, addr, value);
	mutex_unlock(&ec->io_lock);
	return ret;
}

static int mechrevo_ec_update_bits_unlocked(struct mechrevo_ec *ec, u16 addr,
					     u8 mask, u8 value, u8 *result)
{
	u8 current_value;
	u8 updated;
	int ret;

	ret = mechrevo_ec_read_unlocked(ec, addr, &current_value);
	if (ret < 0)
		return ret;

	updated = (current_value & ~mask) | (value & mask);
	if (updated != current_value) {
		ret = mechrevo_ec_write_unlocked(ec, addr, updated);
		if (ret < 0)
			return ret;
	}

	if (result)
		*result = updated;
	return 0;
}

static int mechrevo_set_ap_exists_unlocked(struct mechrevo_ec *ec, bool exists)
{
	u8 value;
	int ret;

	ret = mechrevo_ec_update_bits_unlocked(ec, EC_ADDR_AP_OEM, AP_EXISTS,
					       exists ? AP_EXISTS : 0, NULL);
	if (ret < 0)
		return ret;

	ret = mechrevo_ec_read_unlocked(ec, EC_ADDR_AP_OEM, &value);
	if (ret < 0)
		return ret;

	if (!!(value & AP_EXISTS) != exists)
		return -EIO;

	return 0;
}

static int mechrevo_set_ap_exists(struct mechrevo_ec *ec, bool exists)
{
	int ret;

	mutex_lock(&ec->io_lock);
	ret = mechrevo_set_ap_exists_unlocked(ec, exists);
	mutex_unlock(&ec->io_lock);
	return ret;
}

static void mechrevo_clear_ap_exists(void *context)
{
	struct mechrevo_ec *ec = context;
	int ret;

	ret = mechrevo_set_ap_exists(ec, false);
	if (ret < 0)
		dev_warn(ec->dev, "failed to clear ApExistFlag: %d\n", ret);
}

/* -------------------------------------------------------------------------- */
/* Root-only userspace EC bridge                                               */

static int mechrevo_misc_open(struct inode *inode, struct file *file)
{
	struct miscdevice *misc = file->private_data;
	struct mechrevo_ec *ec = container_of(misc, struct mechrevo_ec, miscdev);

	if (atomic_cmpxchg(&ec->misc_open, 0, 1))
		return -EBUSY;

	file->private_data = ec;
	return nonseekable_open(inode, file);
}

static int mechrevo_misc_release(struct inode *inode, struct file *file)
{
	struct mechrevo_ec *ec = file->private_data;

	atomic_set(&ec->misc_open, 0);
	return 0;
}

static long mechrevo_misc_ioctl(struct file *file, unsigned int cmd,
				unsigned long arg)
{
	struct mechrevo_ec *ec = file->private_data;
	struct mechrevo_ec_io io;
	u8 value;
	int ret;

	if (_IOC_TYPE(cmd) != MECHREVO_EC_IOC_MAGIC)
		return -ENOTTY;
	if (copy_from_user(&io, (void __user *)arg, sizeof(io)))
		return -EFAULT;
	if (io.addr > EC_MAX_ADDR)
		return -EINVAL;

	mutex_lock(&ec->io_lock);
	switch (cmd) {
	case MECHREVO_EC_IOC_READ:
		ret = mechrevo_ec_read_unlocked(ec, io.addr, &io.value);
		break;
	case MECHREVO_EC_IOC_WRITE:
		ret = mechrevo_ec_write_unlocked(ec, io.addr, io.value);
		break;
	case MECHREVO_EC_IOC_UPDATE_BITS:
		ret = mechrevo_ec_update_bits_unlocked(ec, io.addr, io.mask,
						       io.value, &value);
		if (!ret)
			io.value = value;
		break;
	default:
		ret = -ENOTTY;
		break;
	}
	mutex_unlock(&ec->io_lock);
	if (ret < 0)
		return ret;

	if ((_IOC_DIR(cmd) & _IOC_READ) &&
	    copy_to_user((void __user *)arg, &io, sizeof(io)))
		return -EFAULT;

	return 0;
}

static const struct file_operations mechrevo_misc_fops = {
	.owner = THIS_MODULE,
	.open = mechrevo_misc_open,
	.release = mechrevo_misc_release,
	.unlocked_ioctl = mechrevo_misc_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = compat_ptr_ioctl,
#endif
	.llseek = noop_llseek,
};

static void mechrevo_misc_deregister(void *context)
{
	struct mechrevo_ec *ec = context;

	misc_deregister(&ec->miscdev);
}

/* -------------------------------------------------------------------------- */
/* platform_profile: Office / Gaming / Turbo                                  */

static int mechrevo_mode_get_unlocked(struct mechrevo_ec *ec,
				      enum platform_profile_option *profile)
{
	u8 value;
	int ret;

	ret = mechrevo_ec_read_unlocked(ec, EC_ADDR_MODE_CTL, &value);
	if (ret < 0)
		return ret;

	switch (value & MODE_MASK) {
	case MODE_OFFICE:
		*profile = PLATFORM_PROFILE_LOW_POWER;
		return 0;
	case MODE_GAMING:
		*profile = PLATFORM_PROFILE_BALANCED;
		return 0;
	case MODE_TURBO:
		*profile = PLATFORM_PROFILE_PERFORMANCE;
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static int mechrevo_mode_set_unlocked(struct mechrevo_ec *ec,
				      enum platform_profile_option profile)
{
	u8 current_value;
	u8 requested;
	u8 mode;
	int ret;

	switch (profile) {
	case PLATFORM_PROFILE_LOW_POWER:
		mode = MODE_OFFICE;
		break;
	case PLATFORM_PROFILE_BALANCED:
		mode = MODE_GAMING;
		break;
	case PLATFORM_PROFILE_PERFORMANCE:
		mode = MODE_TURBO;
		break;
	default:
		return -EOPNOTSUPP;
	}

	ret = mechrevo_set_ap_exists_unlocked(ec, true);
	if (ret < 0)
		return ret;

	ret = mechrevo_ec_read_unlocked(ec, EC_ADDR_MODE_CTL, &current_value);
	if (ret < 0)
		return ret;

	/* Match src/mode.py: preserve FanBoost, replace the base mode. */
	requested = mode | (current_value & FAN_BOOST);
	ret = mechrevo_ec_write_unlocked(ec, EC_ADDR_MODE_CTL, requested);
	if (ret < 0)
		return ret;

	ret = mechrevo_ec_read_unlocked(ec, EC_ADDR_MODE_CTL, &current_value);
	if (ret < 0)
		return ret;
	if ((current_value & MODE_MASK) != (requested & MODE_MASK))
		return -EIO;

	return 0;
}

static int mechrevo_profile_probe(void *drvdata, unsigned long *choices)
{
	*choices = BIT(PLATFORM_PROFILE_LOW_POWER) |
		   BIT(PLATFORM_PROFILE_BALANCED) |
		   BIT(PLATFORM_PROFILE_PERFORMANCE);
	return 0;
}

static int mechrevo_profile_get(struct device *dev,
				enum platform_profile_option *profile)
{
	struct mechrevo_ec *ec = dev_get_drvdata(dev);
	int ret;

	mutex_lock(&ec->io_lock);
	ret = mechrevo_mode_get_unlocked(ec, profile);
	mutex_unlock(&ec->io_lock);
	return ret;
}

static int mechrevo_profile_set(struct device *dev,
				enum platform_profile_option profile)
{
	struct mechrevo_ec *ec = dev_get_drvdata(dev);
	int ret;

	mutex_lock(&ec->io_lock);
	ret = mechrevo_mode_set_unlocked(ec, profile);
	mutex_unlock(&ec->io_lock);
	return ret;
}

static const struct platform_profile_ops mechrevo_profile_ops = {
	.probe = mechrevo_profile_probe,
	.profile_get = mechrevo_profile_get,
	.profile_set = mechrevo_profile_set,
};

/* -------------------------------------------------------------------------- */
/* Keyboard backlight                                                         */

static int mechrevo_kbd_set(struct led_classdev *led_cdev,
			    enum led_brightness brightness)
{
	struct mechrevo_ec *ec = container_of(led_cdev, struct mechrevo_ec,
					      kbd_backlight);
	u8 value;
	int ret;

	if (brightness > KBD_MAX_LEVEL)
		return -EINVAL;

	mutex_lock(&ec->io_lock);
	ret = mechrevo_ec_read_unlocked(ec, EC_ADDR_KBD_BACKLIGHT, &value);
	if (!ret) {
		value |= KBD_APPLY;
		value &= ~KBD_LEVEL_MASK;
		value |= brightness << 5;
		value &= ~KBD_POWER_OFF;
		ret = mechrevo_ec_write_unlocked(ec, EC_ADDR_KBD_BACKLIGHT, value);
	}
	mutex_unlock(&ec->io_lock);
	return ret;
}

static enum led_brightness mechrevo_kbd_get(struct led_classdev *led_cdev)
{
	struct mechrevo_ec *ec = container_of(led_cdev, struct mechrevo_ec,
					      kbd_backlight);
	u8 value;
	int ret;

	ret = mechrevo_ec_read(ec, EC_ADDR_KBD_BACKLIGHT, &value);
	if (ret < 0 || (value & KBD_POWER_OFF))
		return LED_OFF;

	value = FIELD_GET(KBD_LEVEL_MASK, value);
	return value <= KBD_MAX_LEVEL ? value : LED_OFF;
}

static int mechrevo_kbd_init(struct mechrevo_ec *ec)
{
	struct led_init_data init_data = {
		.devicename = "mechrevo",
		.default_label = ":kbd_backlight",
	};

	ec->kbd_backlight.max_brightness = KBD_MAX_LEVEL;
	ec->kbd_backlight.flags = LED_REJECT_NAME_CONFLICT | LED_BRIGHT_HW_CHANGED;
	ec->kbd_backlight.brightness_set_blocking = mechrevo_kbd_set;
	ec->kbd_backlight.brightness_get = mechrevo_kbd_get;

	return devm_led_classdev_register_ext(ec->dev, &ec->kbd_backlight,
					       &init_data);
}

static void mechrevo_kbd_notify(struct mechrevo_ec *ec)
{
	enum led_brightness brightness = mechrevo_kbd_get(&ec->kbd_backlight);

	led_classdev_notify_brightness_hw_changed(&ec->kbd_backlight, brightness);
}

/* -------------------------------------------------------------------------- */
/* CPU/fan hwmon                                                              */

static int mechrevo_hwmon_read(struct device *dev, enum hwmon_sensor_types type,
			       u32 attr, int channel, long *value)
{
	struct mechrevo_ec *ec = dev_get_drvdata(dev);
	u16 high_addr;
	u16 low_addr;
	u8 high;
	u8 low;
	int ret;

	mutex_lock(&ec->io_lock);
	switch (type) {
	case hwmon_temp:
		if (channel != 0) {
			ret = -EOPNOTSUPP;
			break;
		}
		ret = mechrevo_ec_read_unlocked(ec, EC_ADDR_CPU_TEMP, &low);
		if (!ret)
			*value = low * 1000L;
		break;
	case hwmon_fan:
		if (channel == 0) {
			high_addr = EC_ADDR_MAIN_FAN_RPM_HI;
			low_addr = EC_ADDR_MAIN_FAN_RPM_LO;
		} else if (channel == 1) {
			high_addr = EC_ADDR_SECOND_FAN_RPM_HI;
			low_addr = EC_ADDR_SECOND_FAN_RPM_LO;
		} else {
			ret = -EOPNOTSUPP;
			break;
		}
		ret = mechrevo_ec_read_unlocked(ec, high_addr, &high);
		if (!ret)
			ret = mechrevo_ec_read_unlocked(ec, low_addr, &low);
		if (!ret)
			*value = (high << 8) | low;
		break;
	case hwmon_pwm:
		if (channel == 0)
			low_addr = EC_ADDR_MAIN_FAN_DUTY;
		else if (channel == 1)
			low_addr = EC_ADDR_SECOND_FAN_DUTY;
		else {
			ret = -EOPNOTSUPP;
			break;
		}
		ret = mechrevo_ec_read_unlocked(ec, low_addr, &low);
		if (!ret)
			*value = DIV_ROUND_CLOSEST(min_t(unsigned int, low, 200) * 255,
						   200);
		break;
	default:
		ret = -EOPNOTSUPP;
		break;
	}
	mutex_unlock(&ec->io_lock);
	return ret;
}

static int mechrevo_hwmon_read_string(struct device *dev,
				      enum hwmon_sensor_types type, u32 attr,
				      int channel, const char **str)
{
	static const char * const fan_labels[] = { "Main", "Secondary" };

	if (type == hwmon_temp && channel == 0) {
		*str = "CPU";
		return 0;
	}
	if (type == hwmon_fan && channel >= 0 && channel < ARRAY_SIZE(fan_labels)) {
		*str = fan_labels[channel];
		return 0;
	}
	return -EOPNOTSUPP;
}

static const struct hwmon_ops mechrevo_hwmon_ops = {
	.visible = 0444,
	.read = mechrevo_hwmon_read,
	.read_string = mechrevo_hwmon_read_string,
};

static const struct hwmon_channel_info * const mechrevo_hwmon_info[] = {
	HWMON_CHANNEL_INFO(chip, HWMON_C_REGISTER_TZ),
	HWMON_CHANNEL_INFO(temp, HWMON_T_INPUT | HWMON_T_LABEL),
	HWMON_CHANNEL_INFO(fan,
			   HWMON_F_INPUT | HWMON_F_LABEL,
			   HWMON_F_INPUT | HWMON_F_LABEL),
	HWMON_CHANNEL_INFO(pwm, HWMON_PWM_INPUT, HWMON_PWM_INPUT),
	NULL,
};

static const struct hwmon_chip_info mechrevo_hwmon_chip_info = {
	.ops = &mechrevo_hwmon_ops,
	.info = mechrevo_hwmon_info,
};

/* -------------------------------------------------------------------------- */
/* Minimal WMI/input support for mode and keyboard-backlight OSD               */

static const struct key_entry mechrevo_keymap[] = {
	{ KE_KEY, OSD_KBDILLUMDOWN, { KEY_KBDILLUMDOWN } },
	{ KE_KEY, OSD_KBDILLUMUP, { KEY_KBDILLUMUP } },
	{ KE_KEY, OSD_KBDILLUMTOGGLE, { KEY_KBDILLUMTOGGLE } },
	{ KE_END },
};

static void mechrevo_cycle_mode(struct mechrevo_ec *ec)
{
	enum platform_profile_option current_profile;
	enum platform_profile_option next;
	int ret;

	mutex_lock(&ec->io_lock);
	ret = mechrevo_mode_get_unlocked(ec, &current_profile);
	if (!ret) {
		switch (current_profile) {
		case PLATFORM_PROFILE_LOW_POWER:
			next = PLATFORM_PROFILE_BALANCED;
			break;
		case PLATFORM_PROFILE_BALANCED:
			next = PLATFORM_PROFILE_PERFORMANCE;
			break;
		default:
			next = PLATFORM_PROFILE_LOW_POWER;
			break;
		}
		ret = mechrevo_mode_set_unlocked(ec, next);
	}
	mutex_unlock(&ec->io_lock);

	if (ret < 0)
		dev_warn_ratelimited(ec->dev, "failed to cycle platform profile: %d\n",
				     ret);
	else
		platform_profile_notify(ec->profile_dev);
}

static void mechrevo_wmi_notify(struct wmi_device *wdev, union acpi_object *obj)
{
	struct mechrevo_ec *ec;
	u32 event;

	if (!obj || obj->type != ACPI_TYPE_INTEGER)
		return;
	event = obj->integer.value;

	mutex_lock(&global_data_lock);
	ec = global_data;
	if (!ec) {
		mutex_unlock(&global_data_lock);
		return;
	}

	switch (event) {
	case OSD_PERFORMANCE_MODE_TOGGLE:
		mechrevo_cycle_mode(ec);
		break;
	case OSD_KBDILLUMDOWN:
	case OSD_KBDILLUMUP:
	case OSD_KBDILLUMTOGGLE:
		sparse_keymap_report_event(ec->input, event, 1, true);
		mechrevo_kbd_notify(ec);
		break;
	case OSD_BACKLIGHT_LEVEL_CHANGE:
	case OSD_KB_LED_LEVEL0:
	case OSD_KB_LED_LEVEL1:
	case OSD_KB_LED_LEVEL2:
	case OSD_KB_LED_LEVEL3:
	case OSD_KB_LED_LEVEL4:
	case OSD_KBD_BACKLIGHT_CHANGED:
		mechrevo_kbd_notify(ec);
		break;
	default:
		break;
	}
	mutex_unlock(&global_data_lock);
}

static const struct wmi_device_id mechrevo_wmi_id_table[] = {
	{ MECHREVO_EVENT_GUID, NULL },
	{ }
};

static struct wmi_driver mechrevo_wmi_driver = {
	.driver = {
		.name = "mechrevo-wmi",
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	},
	.id_table = mechrevo_wmi_id_table,
	.notify = mechrevo_wmi_notify,
	.no_singleton = true,
};

static int mechrevo_input_init(struct mechrevo_ec *ec)
{
	int ret;

	ec->input = devm_input_allocate_device(ec->dev);
	if (!ec->input)
		return -ENOMEM;

	ec->input->name = "Mechrevo WMI hotkeys";
	ec->input->phys = "wmi/input0";
	ec->input->id.bustype = BUS_HOST;

	ret = sparse_keymap_setup(ec->input, mechrevo_keymap, NULL);
	if (ret < 0)
		return ret;

	return input_register_device(ec->input);
}

/* -------------------------------------------------------------------------- */
/* Power management and platform driver                                       */

static int mechrevo_suspend(struct device *dev)
{
	struct mechrevo_ec *ec = dev_get_drvdata(dev);

	mutex_lock(&ec->io_lock);
	ec->suspend_mode_valid =
		!mechrevo_ec_read_unlocked(ec, EC_ADDR_MODE_CTL, &ec->suspend_mode);
	ec->suspend_backlight_valid =
		!mechrevo_ec_read_unlocked(ec, EC_ADDR_KBD_BACKLIGHT,
					    &ec->suspend_backlight);
	mutex_unlock(&ec->io_lock);
	return 0;
}

static int mechrevo_resume(struct device *dev)
{
	struct mechrevo_ec *ec = dev_get_drvdata(dev);
	int ret;

	mutex_lock(&ec->io_lock);
	ret = mechrevo_set_ap_exists_unlocked(ec, true);
	if (!ret && ec->suspend_mode_valid)
		ret = mechrevo_ec_write_unlocked(ec, EC_ADDR_MODE_CTL,
						 ec->suspend_mode);
	if (!ret && ec->suspend_backlight_valid)
		ret = mechrevo_ec_write_unlocked(ec, EC_ADDR_KBD_BACKLIGHT,
						 ec->suspend_backlight);
	mutex_unlock(&ec->io_lock);

	if (ret < 0)
		dev_err(ec->dev, "failed to restore EC state after resume: %d\n", ret);
	return ret;
}

static DEFINE_SIMPLE_DEV_PM_OPS(mechrevo_pm_ops, mechrevo_suspend,
				mechrevo_resume);

static int mechrevo_probe(struct platform_device *pdev)
{
	struct mechrevo_ec *ec;
	struct device *hwmon;
	u8 project_id;
	int ret;

	if (!dmi_match(DMI_SYS_VENDOR, "MECHREVO") ||
	    !dmi_match(DMI_BOARD_NAME, "GX4HRXL"))
		return -ENODEV;

	ec = devm_kzalloc(&pdev->dev, sizeof(*ec), GFP_KERNEL);
	if (!ec)
		return -ENOMEM;

	ec->dev = &pdev->dev;
	ec->handle = ACPI_HANDLE(&pdev->dev);
	if (!ec->handle)
		return -ENODEV;
	if (!acpi_has_method(ec->handle, "ECRR") ||
	    !acpi_has_method(ec->handle, "ECRW"))
		return dev_err_probe(&pdev->dev, -ENODEV,
				     "INOU0000 lacks ECRR/ECRW\n");

	mutex_init(&ec->io_lock);
	atomic_set(&ec->misc_open, 0);
	platform_set_drvdata(pdev, ec);

	ret = mechrevo_ec_read(ec, EC_ADDR_PROJECT_ID, &project_id);
	if (ret < 0)
		return dev_err_probe(&pdev->dev, ret, "failed to read project ID\n");

	ret = mechrevo_set_ap_exists(ec, true);
	if (ret < 0)
		return dev_err_probe(&pdev->dev, ret, "failed to establish ApExistFlag\n");

	ret = devm_add_action_or_reset(&pdev->dev, mechrevo_clear_ap_exists, ec);
	if (ret < 0)
		return ret;

	ec->miscdev.minor = MISC_DYNAMIC_MINOR;
	ec->miscdev.name = DEVICE_NAME;
	ec->miscdev.fops = &mechrevo_misc_fops;
	ec->miscdev.parent = &pdev->dev;
	ec->miscdev.mode = 0600;
	ret = misc_register(&ec->miscdev);
	if (ret < 0)
		return dev_err_probe(&pdev->dev, ret, "failed to register EC device\n");

	ret = devm_add_action_or_reset(&pdev->dev, mechrevo_misc_deregister, ec);
	if (ret < 0)
		return ret;

	ret = mechrevo_kbd_init(ec);
	if (ret < 0)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to register keyboard backlight\n");

	hwmon = devm_hwmon_device_register_with_info(&pdev->dev, "mechrevo", ec,
						      &mechrevo_hwmon_chip_info,
						      NULL);
	if (IS_ERR(hwmon))
		return dev_err_probe(&pdev->dev, PTR_ERR(hwmon),
				     "failed to register hwmon\n");

	ec->profile_dev = devm_platform_profile_register(&pdev->dev, "mechrevo",
							 ec,
							 &mechrevo_profile_ops);
	if (IS_ERR(ec->profile_dev))
		return dev_err_probe(&pdev->dev, PTR_ERR(ec->profile_dev),
				     "failed to register platform profile\n");

	ret = mechrevo_input_init(ec);
	if (ret < 0)
		return dev_err_probe(&pdev->dev, ret, "failed to register WMI input\n");

	mutex_lock(&global_data_lock);
	if (global_data) {
		mutex_unlock(&global_data_lock);
		return dev_err_probe(&pdev->dev, -EBUSY,
				     "another GX4HRXL EC instance is active\n");
	}
	global_data = ec;
	mutex_unlock(&global_data_lock);

	dev_info(&pdev->dev,
		 "project ID %u, ApExistFlag established, /dev/%s ready\n",
		 project_id, DEVICE_NAME);
	return 0;
}

static void mechrevo_remove(struct platform_device *pdev)
{
	struct mechrevo_ec *ec = platform_get_drvdata(pdev);

	mutex_lock(&global_data_lock);
	if (global_data == ec)
		global_data = NULL;
	mutex_unlock(&global_data_lock);
}

static void mechrevo_shutdown(struct platform_device *pdev)
{
	struct mechrevo_ec *ec = platform_get_drvdata(pdev);
	int ret;

	ret = mechrevo_set_ap_exists(ec, false);
	if (ret < 0)
		dev_warn(ec->dev, "failed to clear ApExistFlag at shutdown: %d\n", ret);
}

static const struct acpi_device_id mechrevo_acpi_ids[] = {
	{ "INOU0000" },
	{ }
};
MODULE_DEVICE_TABLE(acpi, mechrevo_acpi_ids);

static struct platform_driver mechrevo_platform_driver = {
	.driver = {
		.name = DRIVER_NAME,
		.acpi_match_table = mechrevo_acpi_ids,
		.pm = pm_sleep_ptr(&mechrevo_pm_ops),
	},
	.probe = mechrevo_probe,
	.remove = mechrevo_remove,
	.shutdown = mechrevo_shutdown,
};

static int __init mechrevo_init(void)
{
	int ret;

	if (!dmi_match(DMI_SYS_VENDOR, "MECHREVO") ||
	    !dmi_match(DMI_BOARD_NAME, "GX4HRXL"))
		return -ENODEV;

	ret = platform_driver_register(&mechrevo_platform_driver);
	if (ret < 0)
		return ret;

	ret = wmi_driver_register(&mechrevo_wmi_driver);
	if (ret < 0) {
		platform_driver_unregister(&mechrevo_platform_driver);
		return ret;
	}

	return 0;
}
module_init(mechrevo_init);

static void __exit mechrevo_exit(void)
{
	wmi_driver_unregister(&mechrevo_wmi_driver);
	platform_driver_unregister(&mechrevo_platform_driver);
}
module_exit(mechrevo_exit);

MODULE_AUTHOR("minortex and mech-forza-control contributors");
MODULE_DESCRIPTION("Minimal MECHREVO GX4HRXL ACPI EC driver");
MODULE_LICENSE("GPL");
