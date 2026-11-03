// SPDX-License-Identifier: GPL-2.0-only
/*
 * smcfan - userspace access to the SMC fan keys on kernels 7.3+.
 *
 * macsmc-core publishes a raw SMC key interface as /dev/smc, but its ioctl
 * handler refuses writes to a hardcoded list of keys that includes every key
 * needed to drive the fans (F<N>Tg, F<N>Md, FS! ). That list only exists in
 * the ioctl path: the in-kernel fan driver (macsmc-hwmon) writes the very same
 * keys through apple_smc_write(), which is exported to modules.
 *
 * This module exposes that kernel API as /dev/smcfan (root only) so mbpfan can
 * keep using the fan keys the way applesmc did. Reads are passed through for
 * any key; writes are limited to fan targets and the manual-mode keys, so the
 * battery/charging keys stay unreachable even for root.
 *
 * Nothing in the macsmc stack is replaced or modified; the module only calls
 * the interface it exports.
 */

#include <linux/fs.h>
#include <linux/hwmon.h>
#include <linux/ioctl.h>
#include <linux/mfd/macsmc.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/uaccess.h>

/*
 * The key ioctl ABI arrived with the 7.3 driver: 6.18 LTS ships a macsmc.h
 * that declares apple_smc_read()/apple_smc_write() but no IOCTL_SMC_*
 * commands. Fail with one readable line instead of a page of
 * implicit-declaration errors, and let dkms.conf skip those kernels.
 */
#if !defined(IOCTL_SMC_READ) || !defined(IOCTL_SMC_WRITE)
#error "smcfan needs the SMC key ioctl ABI added in Linux 7.3's macsmc.h"
#endif

/*
 * Speak the same ioctl ABI as macsmc-core's /dev/smc (see
 * include/linux/mfd/macsmc.h), so one userspace implementation drives both
 * devices and callers only differ in the path they open.
 */

static struct apple_smc *smcfan_smc;

/*
 * The SMC object is the drvdata of the device macsmc-acpi binds to
 * (APP0001:00), which is the parent of the MFD children. Any of those
 * children can be used to reach it, regardless of whether its own driver
 * probed successfully.
 */
static const char *const smcfan_child_names[] = {
	"macsmc-hwmon",
	"macsmc-power",
	"macsmc-wdt",
};

static struct apple_smc *smcfan_find_smc(void)
{
	struct apple_smc *smc = NULL;
	struct device *dev;
	int i;

	for (i = 0; i < ARRAY_SIZE(smcfan_child_names) && smc == NULL; i++) {
		dev = bus_find_device_by_name(&platform_bus_type, NULL,
					      smcfan_child_names[i]);
		if (dev == NULL)
			continue;

		smc = dev_get_drvdata(dev->parent);
		put_device(dev);
	}

	return smc;
}

/* Decode a smc_key (0xAABBCCDD = "ABCD") into its four letters. */
static void smcfan_key_name(smc_key key, char out[5])
{
	out[0] = (key >> 24) & 0xff;
	out[1] = (key >> 16) & 0xff;
	out[2] = (key >> 8) & 0xff;
	out[3] = key & 0xff;
	out[4] = '\0';
}

static bool smcfan_key_writable(smc_key key)
{
	char name[5];

	smcfan_key_name(key, name);

	if (name[0] != 'F')
		return false;

	/* "FS! ": manual-mode bitmask for all fans on older SMCs. */
	if (name[1] == 'S' && name[2] == '!' && name[3] == ' ')
		return true;

	if (name[1] < '0' || name[1] > '9')
		return false;

	/* F<N>Tg: target speed. */
	if (name[2] == 'T' && name[3] == 'g')
		return true;

	/* F<N>Md: per-fan manual flag on newer SMCs. */
	if (name[2] == 'M' && name[3] == 'd')
		return true;

	return false;
}

static long smcfan_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct ioctl_smc_rw_payload rw;
	int ret;

	if (smcfan_smc == NULL)
		return -ENODEV;

	if (copy_from_user(&rw, (void __user *)arg, sizeof(rw)))
		return -EFAULT;

	if (rw.size == 0 || rw.size > sizeof(rw.buf))
		return -EINVAL;

	switch (cmd) {
	case IOCTL_SMC_READ:
		ret = apple_smc_read(smcfan_smc, rw.key, rw.buf, rw.size);
		if (ret < 0)
			return ret;

		rw.size = ret;
		if (copy_to_user((void __user *)arg, &rw, sizeof(rw)))
			return -EFAULT;
		return 0;

	case IOCTL_SMC_WRITE:
		if (!smcfan_key_writable(rw.key))
			return -EPERM;

		ret = apple_smc_write(smcfan_smc, rw.key, rw.buf, rw.size);
		return ret < 0 ? ret : 0;

	default:
		return -ENOTTY;
	}
}

static const struct file_operations smcfan_fops = {
	.owner		= THIS_MODULE,
	.unlocked_ioctl	= smcfan_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
	.llseek		= noop_llseek,
};

static struct miscdevice smcfan_device = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= "smcfan",
	.fops	= &smcfan_fops,
	.mode	= 0600,
};

/*
 * Fan telemetry.
 *
 * applesmc used to publish F<N>Ac/F<N>Mn/F<N>Mx as fan<N>_input/min/max.
 * macsmc-hwmon only registers a fan when the SMC has the full fan-control key
 * set, so on models like the MacBookPro11,2 the fans disappear from sysfs
 * entirely and nothing (sensors, desktop widgets, monitoring) can read RPM.
 * The keys are readable through apple_smc_read(), so expose them read-only
 * here. mbpfan keeps using /dev/smcfan for control.
 */
#define SMCFAN_MAX_FANS 8

static struct device *smcfan_hwmon;
static u32 *smcfan_fan_config;

static int smcfan_fan_value(int fan, const char *suffix, long *val)
{
	smc_key key = __SMC_KEY('F', '0' + fan, suffix[0], suffix[1]);
	u8 raw[2];
	int ret;

	if (smcfan_smc == NULL)
		return -ENODEV;

	ret = apple_smc_read(smcfan_smc, key, raw, sizeof(raw));
	if (ret < 0)
		return ret;
	if (ret != sizeof(raw))
		return -EIO;

	/* fpe2: big-endian value with two fractional bits, i.e. RPM = raw / 4. */
	*val = ((raw[0] << 8) | raw[1]) >> 2;
	return 0;
}

static int smcfan_hwmon_read(struct device *dev, enum hwmon_sensor_types type,
			     u32 attr, int channel, long *val)
{
	switch (attr) {
	case hwmon_fan_input:
		return smcfan_fan_value(channel, "Ac", val);
	case hwmon_fan_min:
		return smcfan_fan_value(channel, "Mn", val);
	case hwmon_fan_max:
		return smcfan_fan_value(channel, "Mx", val);
	default:
		return -EOPNOTSUPP;
	}
}

static umode_t smcfan_hwmon_is_visible(const void *data,
				       enum hwmon_sensor_types type,
				       u32 attr, int channel)
{
	if (type != hwmon_fan)
		return 0;

	switch (attr) {
	case hwmon_fan_input:
	case hwmon_fan_min:
	case hwmon_fan_max:
		return 0444;
	default:
		return 0;
	}
}

static const struct hwmon_ops smcfan_hwmon_ops = {
	.is_visible	= smcfan_hwmon_is_visible,
	.read		= smcfan_hwmon_read,
};

static struct hwmon_channel_info smcfan_fan_info;
static const struct hwmon_channel_info *smcfan_hwmon_info[] = {
	&smcfan_fan_info,
	NULL,
};
static const struct hwmon_chip_info smcfan_chip_info = {
	.ops	= &smcfan_hwmon_ops,
	.info	= smcfan_hwmon_info,
};

static int smcfan_register_hwmon(void)
{
	u8 count;
	int ret, i;

	ret = apple_smc_read(smcfan_smc, SMC_KEY(FNum), &count, sizeof(count));
	if (ret < 0)
		return ret;
	if (ret != sizeof(count) || count == 0)
		return -ENODEV;

	if (count > SMCFAN_MAX_FANS)
		count = SMCFAN_MAX_FANS;

	/*
	 * The hwmon core walks this array until it sees a zero entry; without
	 * the terminator it reads past the end and invents fan5..fan27.
	 */
	smcfan_fan_config = kcalloc(count + 1, sizeof(*smcfan_fan_config),
				    GFP_KERNEL);
	if (smcfan_fan_config == NULL)
		return -ENOMEM;

	for (i = 0; i < count; i++)
		smcfan_fan_config[i] = HWMON_F_INPUT | HWMON_F_MIN | HWMON_F_MAX;

	smcfan_fan_info.type = hwmon_fan;
	smcfan_fan_info.config = smcfan_fan_config;

	smcfan_hwmon = hwmon_device_register_with_info(smcfan_device.this_device,
						       "smcfan", NULL,
						       &smcfan_chip_info, NULL);
	if (IS_ERR(smcfan_hwmon)) {
		ret = PTR_ERR(smcfan_hwmon);
		smcfan_hwmon = NULL;
		kfree(smcfan_fan_config);
		smcfan_fan_config = NULL;
		return ret;
	}

	return 0;
}

static int __init smcfan_init(void)
{
	int ret;

	smcfan_smc = smcfan_find_smc();
	if (smcfan_smc == NULL) {
		pr_err("smcfan: no Apple SMC device found; is macsmc-core loaded?\n");
		return -ENODEV;
	}

	ret = misc_register(&smcfan_device);
	if (ret)
		return ret;

	/* Telemetry is a bonus: fan control keeps working without it. */
	ret = smcfan_register_hwmon();
	if (ret)
		pr_warn("smcfan: fan telemetry unavailable: %d\n", ret);

	return 0;
}

static void __exit smcfan_exit(void)
{
	if (smcfan_hwmon != NULL)
		hwmon_device_unregister(smcfan_hwmon);

	kfree(smcfan_fan_config);
	misc_deregister(&smcfan_device);
}

module_init(smcfan_init);
module_exit(smcfan_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Fan key access and telemetry for Apple SMC (macsmc) machines");
MODULE_AUTHOR("Eisa Ahmed");
