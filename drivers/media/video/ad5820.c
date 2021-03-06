/*
 * drivers/media/video/ad5820.c
 *
 * AD5820 DAC driver for camera voice coil focus.
 *
 * Copyright (C) 2008 Nokia Corporation
 * Copyright (C) 2007 Texas Instruments
 *
 * Contact: Tuukka Toivonen <tuukka.o.toivonen@nokia.com>
 *          Sakari Ailus <sakari.ailus@nokia.com>
 *
 * Based on af_d88.c by Texas Instruments.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 */

#include <linux/module.h>
#include <linux/errno.h>
#include <linux/i2c.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/delay.h>
#include <linux/bitops.h>
#include <linux/kernel.h>
#include <linux/regulator/consumer.h>

#include <mach/io.h>
#include <mach/gpio.h>

#include <media/ad5820.h>
#include <media/v4l2-chip-ident.h>
#include <media/v4l2-device.h>

#define CODE_TO_RAMP_US(s)	((s) == 0 ? 0 : (1 << ((s) - 1)) * 50)
#define RAMP_US_TO_CODE(c)	fls(((c) + ((c)>>1)) / 50)

/**
 * @brief I2C write using i2c_transfer().
 * @param coil - the driver data structure
 * @param data - register value to be written
 * @returns nonnegative on success, negative if failed
 */
static int ad5820_write(struct ad5820_device *coil, u16 data)
{
	struct i2c_client *client = v4l2_get_subdevdata(&coil->subdev);
	struct i2c_msg msg;
	int r;

	if (!client->adapter)
		return -ENODEV;

	data = cpu_to_be16(data);
	msg.addr  = client->addr;
	msg.flags = 0;
	msg.len   = 2;
	msg.buf   = (u8 *)&data;

	r = i2c_transfer(client->adapter, &msg, 1);
	if (r < 0) {
		dev_err(&client->dev, "write failed, error %d\n", r);
		return r;
	}

	return 0;
}

/**
 * @brief I2C read using i2c_transfer().
 * @param coil - the driver data structure
 * @returns unsigned 16-bit register value on success, negative if failed
 */
static int ad5820_read(struct ad5820_device *coil)
{
	struct i2c_client *client = v4l2_get_subdevdata(&coil->subdev);
	struct i2c_msg msg;
	int r;
	u16 data = 0;

	if (!client->adapter)
		return -ENODEV;

	msg.addr  = client->addr;
	msg.flags = I2C_M_RD;
	msg.len   = 2;
	msg.buf   = (u8 *)&data;

	r = i2c_transfer(client->adapter, &msg, 1);
	if (r < 0) {
		dev_err(&client->dev, "read failed, error %d\n", r);
		return r;
	}

	return be16_to_cpu(data);
}

/* Calculate status word and write it to the device based on current
 * values of V4L2 controls. It is assumed that the stored V4L2 control
 * values are properly limited and rounded. */
static int ad5820_update_hw(struct ad5820_device *coil)
{
	u16 status;

	if (!coil->power)
		return 0;

	status = RAMP_US_TO_CODE(coil->focus_ramp_time);
	status |= coil->focus_ramp_mode
		? AD5820_RAMP_MODE_64_16 : AD5820_RAMP_MODE_LINEAR;
	status |= coil->focus_absolute << AD5820_DAC_SHIFT;

	if (coil->standby)
		status |= AD5820_POWER_DOWN;

	return ad5820_write(coil, status);
}

static int ad5820_power_off(struct ad5820_device *coil, int standby)
{
	int ret = 0;

	/* Go to standby first as real power off my be denied by the hardware
	 * (single power line control for both coil and sensor).
	 */
	if (standby) {
		coil->standby = 1;
		ret = ad5820_update_hw(coil);
	}

	ret |= coil->platform_data->set_xshutdown(&coil->subdev, 0);
	ret |= regulator_disable(coil->vana);

	coil->power = 0;
	return ret;
}

static int ad5820_power_on(struct ad5820_device *coil, int restore)
{
	int ret;

	ret = regulator_enable(coil->vana);
	if (ret < 0)
		return ret;

	ret = coil->platform_data->set_xshutdown(&coil->subdev, 1);
	if (ret)
		goto fail;

	coil->power = 1;

	if (restore) {
		/* Restore the hardware settings. */
		coil->standby = 0;
		ret = ad5820_update_hw(coil);
		if (ret)
			goto fail;
	}

	return 0;

fail:
	coil->power = 0;
	coil->standby = 1;

	coil->platform_data->set_xshutdown(&coil->subdev, 0);
	regulator_disable(coil->vana);

	return ret;
}

/* --------------------------------------------------------------------------
 * V4L2 controls
 */
static int ad5820_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct ad5820_device *coil =
		container_of(ctrl->handler, struct ad5820_device, ctrls);
	u32 code;
	int r = 0;

	switch (ctrl->id) {
	case V4L2_CID_FOCUS_ABSOLUTE:
		coil->focus_absolute = ctrl->val;
		return ad5820_update_hw(coil);

	case V4L2_CID_FOCUS_AD5820_RAMP_TIME:
		code = RAMP_US_TO_CODE(ctrl->val);
		ctrl->val = CODE_TO_RAMP_US(code);
		coil->focus_ramp_time = ctrl->val;
		break;

	case V4L2_CID_FOCUS_AD5820_RAMP_MODE:
		coil->focus_ramp_mode = ctrl->val;
		break;
	}

	return r;
}

static const struct v4l2_ctrl_ops ad5820_ctrl_ops = {
	.s_ctrl = ad5820_set_ctrl,
};

static const char *ad5820_focus_menu[] = {
	"Linear ramp",
	"64/16 ramp",
};

static const struct v4l2_ctrl_config ad5820_ctrls[] = {
	{
		.ops		= &ad5820_ctrl_ops,
		.id		= V4L2_CID_FOCUS_AD5820_RAMP_TIME,
		.type		= V4L2_CTRL_TYPE_INTEGER,
		.name		= "Focus ramping time [us]",
		.min		= 0,
		.max		= 3200,
		.step		= 50,
		.def		= 0,
		.flags		= 0,
	},
	{
		.ops		= &ad5820_ctrl_ops,
		.id		= V4L2_CID_FOCUS_AD5820_RAMP_MODE,
		.type		= V4L2_CTRL_TYPE_MENU,
		.name		= "Focus ramping mode",
		.min		= 0,
		.max		= ARRAY_SIZE(ad5820_focus_menu),
		.step		= 0,
		.def		= 0,
		.flags		= 0,
		.qmenu		= ad5820_focus_menu,
	},
};


static int ad5820_init_controls(struct ad5820_device *coil)
{
	unsigned int i;

	v4l2_ctrl_handler_init(&coil->ctrls, ARRAY_SIZE(ad5820_ctrls) + 1);

	/* V4L2_CID_FOCUS_ABSOLUTE
	 *
	 * Minimum current is 0 mA, maximum is 100 mA. Thus, 1 code is
	 * equivalent to 100/1023 = 0.0978 mA. Nevertheless, we do not use [mA]
	 * for focus position, because it is meaningless for user. Meaningful
	 * would be to use focus distance or even its inverse, but since the
	 * driver doesn't have sufficiently knowledge to do the conversion, we
	 * will just use abstract codes here. In any case, smaller value = focus
	 * position farther from camera. The default zero value means focus at
	 * infinity, and also least current consumption.
	 */
	v4l2_ctrl_new_std(&coil->ctrls, &ad5820_ctrl_ops,
			  V4L2_CID_FOCUS_ABSOLUTE, 0, 1023, 1, 0);

	/* V4L2_CID_TEST_PATTERN and V4L2_CID_MODE_* */
	for (i = 0; i < ARRAY_SIZE(ad5820_ctrls); ++i)
		v4l2_ctrl_new_custom(&coil->ctrls, &ad5820_ctrls[i], NULL);

	if (coil->ctrls.error)
		return coil->ctrls.error;

	coil->focus_absolute = 0;
	coil->focus_ramp_time = 0;
	coil->focus_ramp_mode = 0;

	coil->subdev.ctrl_handler = &coil->ctrls;
	return 0;
}

/* --------------------------------------------------------------------------
 * V4L2 subdev operations
 */
static int
ad5820_get_chip_ident(struct v4l2_subdev *subdev,
		      struct v4l2_dbg_chip_ident *chip)
{
	struct i2c_client *client = v4l2_get_subdevdata(subdev);

	return v4l2_chip_ident_i2c_client(client, chip, V4L2_IDENT_AD5820, 0);
}

static int
ad5820_set_config(struct v4l2_subdev *subdev, int irq, void *platform_data)
{
	static const int CHECK_VALUE = 0x3FF0;

	struct ad5820_device *coil = to_ad5820_device(subdev);
	struct i2c_client *client = v4l2_get_subdevdata(subdev);
	u16 status = AD5820_POWER_DOWN | CHECK_VALUE;
	int rval;

	if (platform_data == NULL)
		return -ENODEV;

	coil->vana = regulator_get(&client->dev, "VANA");
	if (IS_ERR(coil->vana)) {
		dev_err(&client->dev, "could not get regulator for vana\n");
		return -ENODEV;
	}

	coil->platform_data = platform_data;

	/* Detect that the chip is there */
	rval = ad5820_power_on(coil, 0);
	if (rval)
		goto not_detected;
	rval = ad5820_write(coil, status);
	if (rval)
		goto not_detected;
	rval = ad5820_read(coil);
	if (rval != status)
		goto not_detected;

	ad5820_power_off(coil, 1);

	return ad5820_init_controls(coil);

not_detected:
	dev_err(&client->dev, "not detected\n");
	ad5820_power_off(coil, 0);
	regulator_put(coil->vana);
	return -ENODEV;
}

static int
ad5820_set_power(struct v4l2_subdev *subdev, int on)
{
	struct ad5820_device *coil = to_ad5820_device(subdev);

	if (coil->power == on)
		return 0;

	return on ? ad5820_power_on(coil, 1) : ad5820_power_off(coil, 1);
}

static const struct v4l2_subdev_core_ops ad5820_core_ops = {
	.g_chip_ident = ad5820_get_chip_ident,
	.s_config = ad5820_set_config,
	.s_power = ad5820_set_power,
};

static const struct v4l2_subdev_ops ad5820_ops = {
	.core = &ad5820_core_ops,
};

/* --------------------------------------------------------------------------
 * I2C driver
 */
#ifdef CONFIG_PM

static int ad5820_suspend(struct i2c_client *client, pm_message_t mesg)
{
	struct v4l2_subdev *subdev = i2c_get_clientdata(client);
	struct ad5820_device *coil = to_ad5820_device(subdev);

	if (!coil->power)
		return 0;

	return coil->platform_data->set_xshutdown(subdev, 0);
}

static int ad5820_resume(struct i2c_client *client)
{
	struct v4l2_subdev *subdev = i2c_get_clientdata(client);
	struct ad5820_device *coil = to_ad5820_device(subdev);

	if (!coil->power)
		return 0;

	coil->power = 0;
	return ad5820_set_power(subdev, 1);
}

#else

#define ad5820_suspend	NULL
#define ad5820_resume	NULL

#endif /* CONFIG_PM */

static const struct media_entity_operations ad5820_entity_ops = {
	.set_power = v4l2_subdev_set_power,
};

static int ad5820_probe(struct i2c_client *client,
			const struct i2c_device_id *devid)
{
	struct ad5820_device *coil;
	int ret = 0;

	coil = kzalloc(sizeof(*coil), GFP_KERNEL);
	if (coil == NULL)
		return -ENOMEM;

	v4l2_i2c_subdev_init(&coil->subdev, client, &ad5820_ops);
	coil->subdev.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;

	coil->subdev.entity.ops = &ad5820_entity_ops;
	ret = media_entity_init(&coil->subdev.entity, 0, NULL, 0);
	if (ret < 0)
		kfree(coil);

	return ret;
}

static int __exit ad5820_remove(struct i2c_client *client)
{
	struct v4l2_subdev *subdev = i2c_get_clientdata(client);
	struct ad5820_device *coil = to_ad5820_device(subdev);

	v4l2_device_unregister_subdev(&coil->subdev);
	media_entity_cleanup(&coil->subdev.entity);
	if (coil->vana)
		regulator_put(coil->vana);

	kfree(coil);
	return 0;
}

static const struct i2c_device_id ad5820_id_table[] = {
	{ AD5820_NAME, 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, ad5820_id_table);

static struct i2c_driver ad5820_i2c_driver = {
	.driver		= {
		.name	= AD5820_NAME,
	},
	.probe		= ad5820_probe,
	.remove		= __exit_p(ad5820_remove),
	.suspend	= ad5820_suspend,
	.resume		= ad5820_resume,
	.id_table	= ad5820_id_table,
};

static int __init ad5820_init(void)
{
	int rval;

	rval = i2c_add_driver(&ad5820_i2c_driver);
	if (rval)
		printk(KERN_INFO "%s: failed registering " AD5820_NAME "\n",
		       __func__);

	return rval;
}

static void __exit ad5820_exit(void)
{
	i2c_del_driver(&ad5820_i2c_driver);
}


module_init(ad5820_init);
module_exit(ad5820_exit);

MODULE_AUTHOR("Tuukka Toivonen <tuukka.o.toivonen@nokia.com>");
MODULE_DESCRIPTION("AD5820 camera lens driver");
MODULE_LICENSE("GPL");
