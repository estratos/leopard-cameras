/*
 * raa462113.c - raa462113 sensor driver
 *
 * Copyright (c) 2016-2017, NVIDIA CORPORATION.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#define DEBUG 1
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/gpio.h>
#include <linux/module.h>

#include <linux/seq_file.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <linux/seq_file.h>
#include <linux/debugfs.h>

#include <media/tegra_v4l2_camera.h>
#include <media/camera_common.h>
#include "raa462113_mode_tbls.h"
/* default mode */
#define RAA462113_DEFAULT_MODE	RAA462113_MODE_1920X1080_CROP_30FPS
/* default image data format */
#define RAA462113_DEFAULT_DATAFMT	MEDIA_BUS_FMT_SRGGB12_1X12
/* minimum frame length */
#define RAA462113_MIN_FRAME_LENGTH	(2224)
/* maximum frame length */
#define RAA462113_MAX_FRAME_LENGTH	(0x1FFFF)

/* msb frame length add */
#define RAA462113_FRAME_LENGTH_ADDR_MSB		0x301A
/* mid frame length add */
#define RAA462113_FRAME_LENGTH_ADDR_MID		0x3019
/* lsb frame length add */
#define RAA462113_FRAME_LENGTH_ADDR_LSB		0x3018
/* shs1 msb coarse time */
#define RAA462113_COARSE_TIME_SHS1_ADDR_MSB	0x000A


#define RAA462113_GAIN_ADDR			0x3014 /* GAIN ADDR */
#define RAA462113_GROUP_HOLD_ADDR			0x3001 /* REG HOLD */
#define RAA462113_SW_RESET_ADDR			0x3003 /* SW RESET */


/* default image output width */
#define RAA462113_DEFAULT_WIDTH	3872
/* default image output height */
#define RAA462113_DEFAULT_HEIGHT	2192
/* default output clk frequency for camera */
#define RAA462113_DEFAULT_CLK_FREQ	28000000





/*
 * struct raa462113 - raa462113 structure
 * @power: Camera common power rail structure
 * @numctrls: The num of V4L2 control
 * @i2c_client: Pointer to I2C client
 * @subdev: Pointer to V4L2 subdevice structure
 * @pad: Media pad structure
 * @group_hold_prev: Group hold status
 * @group_hold_en: Enable/Disable group hold
 * @regmap: Pointer to regmap structure
 * @s_data: Pointer to camera common data structure
 * @p_data: Pointer to camera common pdata structure
 * @ctrls: Pointer to V4L2 control list
 */
struct raa462113 {
	struct camera_common_power_rail	power;
	int				numctrls;
	struct v4l2_ctrl_handler	ctrl_handler;
	struct i2c_client		*i2c_client;
	struct v4l2_subdev		*subdev;
	struct media_pad		pad;
	u32				frame_length;
	s32				group_hold_prev;
	bool				group_hold_en;
	s64				last_wdr_et_val;
	struct regmap			*regmap;
	struct camera_common_data	*s_data;
	struct camera_common_pdata	*pdata;
	struct v4l2_ctrl		*ctrls[];
};

/*
 * struct sensror_regmap_config - sensor regmap config structure
 * @reg_bits: Sensor register address width
 * @val_bits: Sensor register value width
 * @cache_type: Cache type
 * @use_single_rw: Indicate only read or write a single time
 */
static const struct regmap_config sensor_regmap_config = {
	.reg_bits	= 16,
	.val_bits	= 16,
	.cache_type	= REGCACHE_RBTREE,
	.use_single_rw	= true,
};
/*
 * Function declaration
 */
static int raa462113_s_ctrl(struct v4l2_ctrl *ctrl);

/*
 * raa462113 V4L2 control operator
 */
static const struct v4l2_ctrl_ops raa462113_ctrl_ops = {
	.s_ctrl = raa462113_s_ctrl,
};




/*
 * V4L2 control configuration list
 * the control Items includes gain, exposure,
 * frame rate, group hold and HDR
 */
static struct v4l2_ctrl_config ctrl_config_list[] = {
/* Do not change the name field for the controls! */
	{
		.ops = &raa462113_ctrl_ops,
		.id = TEGRA_CAMERA_CID_GAIN,
		.name = "Gain",
		.type = V4L2_CTRL_TYPE_INTEGER64,
		.flags = V4L2_CTRL_FLAG_SLIDER,
		.min = 1 * FIXED_POINT_SCALING_FACTOR,
		.max = 512 * FIXED_POINT_SCALING_FACTOR,
		.def = 7 * FIXED_POINT_SCALING_FACTOR,
		.step = 1,
	},
	{
		.ops = &raa462113_ctrl_ops,
		.id = TEGRA_CAMERA_CID_EXPOSURE,
		.name = "Exposure",
		.type = V4L2_CTRL_TYPE_INTEGER64,
		.flags = V4L2_CTRL_FLAG_SLIDER,
		.min = 30 * FIXED_POINT_SCALING_FACTOR / 1000000,
		.max = 1000000LL * FIXED_POINT_SCALING_FACTOR / 1000000,
		.def = 30 * FIXED_POINT_SCALING_FACTOR / 1000000,
		.step = 1,
	},
	{
		.ops = &raa462113_ctrl_ops,
		.id = TEGRA_CAMERA_CID_FRAME_RATE,
		.name = "Frame Rate",
		.type = V4L2_CTRL_TYPE_INTEGER64,
		.flags = V4L2_CTRL_FLAG_SLIDER,
		.min = 1 * FIXED_POINT_SCALING_FACTOR,
		.max = 50 * FIXED_POINT_SCALING_FACTOR,
		.def = 50 * FIXED_POINT_SCALING_FACTOR,
		.step = 1,
	},
	{
		.ops = &raa462113_ctrl_ops,
		.id = TEGRA_CAMERA_CID_GROUP_HOLD,
		.name = "Group Hold",
		.type = V4L2_CTRL_TYPE_INTEGER_MENU,
		.min = 0,
		.max = ARRAY_SIZE(switch_ctrl_qmenu) - 1,
		.menu_skip_mask = 0,
		.def = 0,
		.qmenu_int = switch_ctrl_qmenu,
	},
	{
		.ops = &raa462113_ctrl_ops,
		.id = TEGRA_CAMERA_CID_HDR_EN,
		.name = "HDR enable",
		.type = V4L2_CTRL_TYPE_INTEGER_MENU,
		.min = 0,
		.max = ARRAY_SIZE(switch_ctrl_qmenu) - 1,
		.menu_skip_mask = 0,
		.def = 0,
		.qmenu_int = switch_ctrl_qmenu,
	},
};
/*
 * raa462113_get_frame_legnth_regs - Function for get frame length
 * register value
 * @regs: Pointer to raa462113 reg structure
 * @frame_length: Frame length value
 *
 * This is used to get the frame length value for frame length register
 */
static inline void raa462113_get_frame_length_regs(raa462113_reg *regs,
				u32 frame_length)
{
	regs->addr = RAA462113_FRAME_LENGTH_ADDR_MSB;
	regs->val = (frame_length >> 16) & 0x01;

	(regs + 1)->addr = RAA462113_FRAME_LENGTH_ADDR_MID;
	(regs + 1)->val = (frame_length >> 8) & 0xff;

	(regs + 2)->addr = RAA462113_FRAME_LENGTH_ADDR_LSB;
	(regs + 2)->val = (frame_length) & 0xff;
}


/*
 * raa462113_get_coarse_time_regs_shs1 - Function for get coarse time
 * register value
 * @regs: Pointer to get reg structure
 * @coarse_time: Coarse time value
 *
 * This is used to get the coarse time value by shs1 for coarse time register
 */
static inline void raa462113_get_coarse_time_regs_shs1(raa462113_reg *regs,
				u32 coarse_time)
{
	regs->addr = RAA462113_COARSE_TIME_SHS1_ADDR_MSB;
	regs->val = (coarse_time & 0xffff);


}


static int test_mode;
module_param(test_mode, int, 0644);

/*
 * raa462113_read_reg - Function for reading register value
 * @s_data: Pointer to camera common data structure
 * @addr: Registr address
 * @val: Pointer to register value
 *
 * This function is used to read a register value for raa462113
 *
 * Return: 0 on success, errors otherwise
 */
static inline int raa462113_read_reg(struct camera_common_data *s_data,
				u16 addr, u16 *val)
{
	struct raa462113 *priv = (struct raa462113 *)s_data->priv;
	int err = 0;
	u32 reg_val = 0;

	err = regmap_read(priv->regmap, addr, &reg_val);
	*val = reg_val & 0xFFFF;

	return err;
}

/*
 * raa462113_write_reg - Function for writing register value
 * @s_data: Pointer to camera common data structure
 * @addr: Registr address
 * @val: Register value
 *
 * This function is used to write a register value for raa462113
 *
 * Return: 0 on success, errors otherwise
 */
static int raa462113_write_reg(struct camera_common_data *s_data,
				u16 addr, u16 val)
{
	int err;
	struct raa462113 *priv = (struct raa462113 *)s_data->priv;

	err = regmap_write(priv->regmap, addr, val);
	if (err)
		pr_err("%s:i2c write failed, 0x%x = %x\n",
			__func__, addr, val);

	return err;
}
/*
 * regmap_util_write_table_8 - Function for writing register table
 * @priv: Pointer to raa462113 structure
 * @table: Table containing register values
 *
 * This is used to write register table into sensor's reg map.
 *
 * Return: 0 on success, errors otherwise
 */
static int raa462113_write_table(struct raa462113 *priv,
				const raa462113_reg table[])
{

	int ret = 0;
	int i = 0;
    while(table[i].addr != 0xffff)
    {
		if(table[i].addr == 0xefff)
			msleep(table[i].val);
		else
            ret = raa462113_write_reg(priv->s_data, table[i].addr, table[i].val);

        i++;
    }
	return 0;
}
/*
 * raa462113_power_on - Function to power on the camera
 * @s_data: Pointer to camera common data
 *
 * This is used to power on raa462113 camera board
 *
 * Return: 0 on success, errors otherwise
 */
static int raa462113_power_on(struct camera_common_data *s_data)
{
	int err = 0;
	struct raa462113 *priv = (struct raa462113 *)s_data->priv;
	struct camera_common_power_rail *pw = &priv->power;

	dev_dbg(&priv->i2c_client->dev, "%s: power on\n", __func__);
	if (priv->pdata && priv->pdata->power_on) {
		err = priv->pdata->power_on(pw);
		if (err)
			pr_err("%s failed.\n", __func__);
		else
			pw->state = SWITCH_ON;
		return err;
	}
	/*exit reset mode: XCLR */
	if (pw->reset_gpio) {
		gpio_direction_output(pw->reset_gpio, 0);
		usleep_range(30, 50);
		gpio_direction_output(pw->reset_gpio, 1);
		usleep_range(30, 50);
	}

	pw->state = SWITCH_ON;
	return 0;

}
/*
 * raa462113_power_off - Function to power off the camera
 * @s_data: Pointer to camera common data
 *
 * This is used to power off raa462113 camera board
 *
 * Return: 0 on success, errors otherwise
 */
static int raa462113_power_off(struct camera_common_data *s_data)
{
	int err = 0;
	struct raa462113 *priv = (struct raa462113 *)s_data->priv;
	struct camera_common_power_rail *pw = &priv->power;

	dev_dbg(&priv->i2c_client->dev, "%s: power off\n", __func__);

	if (priv->pdata && priv->pdata->power_off) {
		err = priv->pdata->power_off(pw);
		if (!err)
			goto power_off_done;
		else
			pr_err("%s failed.\n", __func__);
		return err;
	}
	/* enter reset mode: XCLR */
	usleep_range(1, 2);
	if (pw->reset_gpio)
		gpio_direction_output(pw->reset_gpio, 0);

power_off_done:
	pw->state = SWITCH_OFF;

	return 0;
}

/*
 * raa462113_power_get - Function to get power
 * @priv: Pointer to raa462113 structure
 *
 * This is used to get power from tegra
 *
 * Return: 0 on success, errors otherwise
 */
static int raa462113_power_get(struct raa462113 *priv)
{
	struct camera_common_power_rail *pw = &priv->power;
	struct camera_common_pdata *pdata = priv->pdata;
	const char *mclk_name;
	struct clk *parent;
	int err = 0;

	mclk_name = priv->pdata->mclk_name ?
		    priv->pdata->mclk_name : "extperiph1";
	pw->mclk = devm_clk_get(&priv->i2c_client->dev, mclk_name);
	if (IS_ERR(pw->mclk)) {
		dev_err(&priv->i2c_client->dev,
			"unable to get clock %s\n", mclk_name);
		return PTR_ERR(pw->mclk);
	}

	parent = devm_clk_get(&priv->i2c_client->dev, "pllp_grtba");
	if (IS_ERR(parent))
		dev_err(&priv->i2c_client->dev, "devm_clk_get failed for pllp_grtba");
	else
		clk_set_parent(pw->mclk, parent);

	pw->reset_gpio = pdata->reset_gpio;

	pw->state = SWITCH_OFF;
	return err;
}
/*
 * Function declaration
 */
static int raa462113_set_coarse_time(struct raa462113 *priv, s64 val);
static int raa462113_set_gain(struct raa462113 *priv, s64 val);
static int raa462113_set_frame_rate(struct raa462113 *priv, s64 val);
static int raa462113_set_exposure(struct raa462113 *priv, s64 val);

/*
 * raa462113_s_stream - It is used to start/stop the streaming.
 * @sd: V4L2 Sub device
 * @enable: Flag (True / False)
 *
 * This function controls the start or stop of streaming for the
 * raa462113 sensor.
 *
 * Return: 0 on success, errors otherwise
 */
static int raa462113_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct camera_common_data *s_data = to_camera_common_data(&client->dev);
	struct raa462113 *priv = (struct raa462113 *)s_data->priv;
	struct v4l2_ext_controls ctrls;
	struct v4l2_ext_control control[3];
	int err;

	dev_dbg(&client->dev, "%s++ enable %d\n", __func__, enable);

	if (!enable) {
		err =  raa462113_write_table(priv,
			mode_table[RAA462113_MODE_STOP_STREAM]);

		if (err)
			return err;

		return 0;
	}
	err = raa462113_write_table(priv, mode_table[s_data->mode]);
	if (err)
		goto exit;

    #if 1
	if (s_data->override_enable) {
		/* write list of override regs for the asking gain, */
		/* frame rate and exposure time    */
		memset(&ctrls, 0, sizeof(ctrls));
		ctrls.ctrl_class = V4L2_CTRL_ID2CLASS(TEGRA_CAMERA_CID_GAIN);
		ctrls.count = 3;
		ctrls.controls = control;

		control[0].id = TEGRA_CAMERA_CID_GAIN;
		control[1].id = TEGRA_CAMERA_CID_FRAME_RATE;
		control[2].id = TEGRA_CAMERA_CID_EXPOSURE;

		err = v4l2_g_ext_ctrls(&priv->ctrl_handler, &ctrls);
		if (err == 0) {
			err |= raa462113_set_gain(priv, control[0].value64);
			if (err)
				dev_err(&client->dev,
					"%s: error gain override\n", __func__);

			err |= raa462113_set_frame_rate(priv, control[1].value64);
			if (err)
				dev_err(&client->dev,
					"%s: error frame length override\n",
					__func__);

			err |= raa462113_set_exposure(priv, control[2].value64);
			if (err)
				dev_err(&client->dev,
					"%s: error exposure override\n",
					__func__);

		} else {
			dev_err(&client->dev, "%s: faile to get overrides\n",
				__func__);
		}
	}
#endif


	err = raa462113_write_table(priv, mode_table[RAA462113_MODE_START_STREAM]);
	if (err)
		goto exit;

	return 0;
exit:
	dev_err(&client->dev, "%s: error setting stream\n", __func__);
	return err;
}

static u16 val_read = 0;

static int
raa462113_s_parm(struct v4l2_subdev *sd, struct v4l2_streamparm *a)
{
        struct i2c_client *client = v4l2_get_subdevdata(sd);
    //    struct camera_common_data *s_data = to_camera_common_data(client);
	struct camera_common_data *s_data = to_camera_common_data(&client->dev);
        struct raa462113 *priv = (struct raa462113 *)s_data->priv;
	u16 addr = 0;
	u16 value = 0;

        if (a == NULL)
                return -EINVAL;
	if (a->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
                /* only capture is supported */
                return -EINVAL;
	if(a->parm.raw_data[0] == 0)   //write value
        {
		addr = a->parm.raw_data[1] << 8 | a->parm.raw_data[2];
		value = a->parm.raw_data[3] << 8 | a->parm.raw_data[4];

		printk("write:addr = 0x%x, value = 0x%x\n", addr, value);
		raa462113_write_reg(priv->s_data, addr, value);

        } else if(a->parm.raw_data[0] == 1)  // read value
	{
		addr = a->parm.raw_data[1] << 8 | a->parm.raw_data[2];
		printk("read:addr = 0x%x\n", addr);
		raa462113_read_reg(priv->s_data, addr, &val_read);
	}
	
	return 0;
}

static int
raa462113_g_parm(struct v4l2_subdev *sd, struct v4l2_streamparm *a)
{
    //    struct i2c_client *client = v4l2_get_subdevdata(sd);
    //    struct camera_common_data *s_data = to_camera_common_data(client);
//        struct camera_common_data *s_data = to_camera_common_data(&client->dev);
  //      struct raa462113 *priv = (struct raa462113 *)s_data->priv;

        if (a == NULL)
                return -EINVAL;
        if (a->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
                /* only capture is supported */
                return -EINVAL;

	a->parm.raw_data[5] = val_read >> 8;
	a->parm.raw_data[6] = val_read & 0xff;
	printk("val:0x%x, 0x%x\n", a->parm.raw_data[5], a->parm.raw_data[6]);
        return 0;




}


/*
 * raa462113_g_input_status - This is used to get input status
 * @sd: Pointer to V4L2 Sub device structure
 * @status: Pointer to status
 *
 * This function is used to get input status
 *
 * Return: 0 on success
 */
static int raa462113_g_input_status(struct v4l2_subdev *sd, u32 *status)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct camera_common_data *s_data = to_camera_common_data(&client->dev);
	struct raa462113 *priv = (struct raa462113 *)s_data->priv;
	struct camera_common_power_rail *pw = &priv->power;

	*status = pw->state == SWITCH_ON;
	return 0;
}
/*
 * Media operations
 */
static struct v4l2_subdev_video_ops raa462113_subdev_video_ops = {
	.s_stream	= raa462113_s_stream,
	.g_mbus_config	= camera_common_g_mbus_config,
	.g_input_status = raa462113_g_input_status,
	.s_parm = raa462113_s_parm,
	.g_parm = raa462113_g_parm,

};

static struct v4l2_subdev_core_ops raa462113_subdev_core_ops = {
	.s_power	= camera_common_s_power,
};
/*
 * raa462113_get_fmt - Get the pad format
 * @sd: Pointer to V4L2 Sub device structure
 * @cfg: Pointer to sub device pad information structure
 * @fmt: Pointer to pad level media bus format
 *
 * This function is used to get the pad format information
 *
 * Return: 0 on success, errors otherwise
 */
static int raa462113_get_fmt(struct v4l2_subdev *sd,
		struct v4l2_subdev_pad_config *cfg,
		struct v4l2_subdev_format *format)
{
	return camera_common_g_fmt(sd, &format->format);
}
/*
 * raa462113_set_fmt - This is used to set the pad format
 * @sd: Pointer to V4L2 Sub device structure
 * @cfg: Pointer to sub device pad information structure
 * @format: Pointer to pad level media bus format
 *
 * This function is used to set the pad format
 *
 * Return: 0 on success, errors otherwise
 */
static int raa462113_set_fmt(struct v4l2_subdev *sd,
		struct v4l2_subdev_pad_config *cfg,
	struct v4l2_subdev_format *format)
{
	int ret;

	if (format->which == V4L2_SUBDEV_FORMAT_TRY)
		ret = camera_common_try_fmt(sd, &format->format);
	else
		ret = camera_common_s_fmt(sd, &format->format);

	return ret;
}
/*
 * Media operations
 */
static struct v4l2_subdev_pad_ops raa462113_subdev_pad_ops = {
	.set_fmt		= raa462113_set_fmt,
	.get_fmt		= raa462113_get_fmt,
	.enum_mbus_code		= camera_common_enum_mbus_code,
	.enum_frame_size	= camera_common_enum_framesizes,
	.enum_frame_interval	= camera_common_enum_frameintervals,
};

static struct v4l2_subdev_ops raa462113_subdev_ops = {
	.core	= &raa462113_subdev_core_ops,
	.video	= &raa462113_subdev_video_ops,
	.pad	= &raa462113_subdev_pad_ops,
};

const struct of_device_id raa462113_of_match[] = {
	{ .compatible = "nvidia,raa462113",},
	{ },
};


static struct camera_common_sensor_ops raa462113_common_ops = {
	.power_on  = raa462113_power_on,
	.power_off = raa462113_power_off,
	//.write_reg = raa462113_write_reg_8,
	//.read_reg  = raa462113_read_reg_8,
};

/*
 * im185_set_group_hold - Function to hold the sensor register
 * @priv: Pinter to raa462113 structure
 *
 * This is used to hold the raa462113 sensor register
 *
 * Return: 0 on success, errors otherwise
 */
static int raa462113_set_group_hold(struct raa462113 *priv, s32 val)
{
	int err;
	int gh_en = switch_ctrl_qmenu[val];

	return 0;
	priv->group_hold_prev = val;
	if (gh_en == SWITCH_ON) {

		err = raa462113_write_reg(priv->s_data,
				       RAA462113_GROUP_HOLD_ADDR, 0x1);
		if (err)
			goto fail;
	} else if (gh_en == SWITCH_OFF) {
		err = raa462113_write_reg(priv->s_data,
				       RAA462113_GROUP_HOLD_ADDR, 0x0);
		if (err)
			goto fail;
	}
	return 0;
fail:
	dev_dbg(&priv->i2c_client->dev,
		 "%s: Group hold control error\n", __func__);
	return err;
}
/*
 * raa462113_set_gain - Function called when setting analog gain
 * @priv: Pointer to device structure
 * @val: Value for gain
 *
 * Set the analog gain based on input value.
 *
 * Return: 0 on success, errors otherwise
 */
static int raa462113_set_gain(struct raa462113 *priv, s64 val)
{
	s64 gain64;
	u32 gain;
	u32 reg1 = 0;
	u32 reg2 = 0;


	/* translate value */
	gain64 = (s64)(val * 1000 / FIXED_POINT_SCALING_FACTOR);
	gain = (u32)gain64;
	dev_dbg(&priv->i2c_client->dev,
		"%s:  gain reg: %d, db: %lld\n",  __func__, gain, gain64);


	if(gain64 >=1000 && gain64 <= 1324) {
		gain = (u32)gain64;
		reg1 = 616000 / gain;
		reg2 = (reg1 * 684) / 736;
		raa462113_write_reg(priv->s_data, 0x000E, (u16)reg1 << 5 | 0x1e);
		raa462113_write_reg(priv->s_data, 0x0016, (u16)reg2);
		raa462113_write_reg(priv->s_data, 0x0012, 0x0);
		


	} else if(gain64 >=1324 && gain64 <= 1999) {
		 
		gain = (u32)gain64 * 1000/ 1324;
		reg1 = 616000 / gain;
		reg2 = (reg1 * 684) / 736;
		raa462113_write_reg(priv->s_data, 0x000E, (u16)reg1 << 5 | 0x1f);
		raa462113_write_reg(priv->s_data, 0x0016, (u16)reg2);
		raa462113_write_reg(priv->s_data, 0x0012, 0x0);


	} else if(gain64 >=1999 && gain64 <= 3999) {

		gain = (u32)gain64 * 1000/ 1324;
		reg1 = 616000 / gain;
		reg2 = (reg1 * 684) / 736;
		raa462113_write_reg(priv->s_data, 0x000E, (u16)reg1 << 5 | 0x1f);
		raa462113_write_reg(priv->s_data, 0x0016, (u16)reg2);
		raa462113_write_reg(priv->s_data, 0x0012, 0x0);


	} else if(gain64 >=3999 && gain64 <= 7998) {

		gain = (u32)gain64 * 1000/ 2648;
		reg1 = 616000 / gain;
		reg2 = (reg1 * 684) / 736;
		raa462113_write_reg(priv->s_data, 0x000E, (u16)reg1 << 5 | 0x0f);
		raa462113_write_reg(priv->s_data, 0x0016, (u16)reg2);
		raa462113_write_reg(priv->s_data, 0x0012, 0x0);



	} else if(gain64 >=7998 && gain64 <= 15995) {

		gain = (u32)gain64 * 1000/ 5296;
		reg1 = 616000 / gain;
		reg2 = (reg1 * 684) / 736;
		raa462113_write_reg(priv->s_data, 0x000E, (u16)reg1 << 5 | 0x07);
		raa462113_write_reg(priv->s_data, 0x0016, (u16)reg2);
		raa462113_write_reg(priv->s_data, 0x0012, 0x0);


	} else if(gain64 >=15995 && gain64 <= 32026) {

		gain = (u32)gain64 * 1000/ 10568 ;
		reg1 = 616000 / gain;
		reg2 = (reg1 * 684) / 736;
		raa462113_write_reg(priv->s_data, 0x000E, (u16)reg1 << 5 | 0x03);
		raa462113_write_reg(priv->s_data, 0x0016, (u16)reg2);
		raa462113_write_reg(priv->s_data, 0x0012, 0x0);


	} else if(gain64 >=32026 && gain64 <= 64047) {

		gain = (u32)gain64 * 1000/ 21183 ;
		reg1 = 616000 / gain;
		reg2 = (reg1 * 684) / 736;
		raa462113_write_reg(priv->s_data, 0x000E, (u16)reg1 << 5 | 0x03);
		raa462113_write_reg(priv->s_data, 0x0016, (u16)reg2);
		raa462113_write_reg(priv->s_data, 0x0012, 0x1);


	} else if(gain64 >=64047 && gain64 <= 128086) {

		gain = (u32)gain64 * 1000/ 42364 ;
		reg1 = 616000 / gain;
		reg2 = (reg1 * 684) / 736;
		raa462113_write_reg(priv->s_data, 0x000E, (u16)reg1 << 5 | 0x03);
		raa462113_write_reg(priv->s_data, 0x0016, (u16)reg2);
		raa462113_write_reg(priv->s_data, 0x0012, 0x2);

	} else if(gain64 >=128086 && gain64 <= 256153) {

		gain = (u32)gain64 * 1000/ 84723 ;
		reg1 = 616000 / gain;
		reg2 = (reg1 * 684) / 736;
		raa462113_write_reg(priv->s_data, 0x000E, (u16)reg1 << 5 | 0x03);
		raa462113_write_reg(priv->s_data, 0x0016, (u16)reg2);
		raa462113_write_reg(priv->s_data, 0x0012, 0x3);

	} else if(gain64 >=256153 && gain64 <= 512000) {
		gain = (u32)gain64 * 1000/ 169434 ;
		reg1 = 616000 / gain;
		reg2 = (reg1 * 684) / 736;
		raa462113_write_reg(priv->s_data, 0x000E, (u16)reg1 << 5 | 0x03);
		raa462113_write_reg(priv->s_data, 0x0016, (u16)reg2);
		raa462113_write_reg(priv->s_data, 0x0012, 0x4);

	}


	return 0;

}
/*
 * raa462113_set_frame_rate - Function called when setting frame rate
 * @priv: Pointer to device structure
 * @val: Value for rate
 *
 * Set the frame rate based on input value.
 *
 * Return: 0 on success, errors otherwise
 */
static int raa462113_set_frame_rate(struct raa462113 *priv, s64 val)
{
	raa462113_reg reg_list[3];
	int err;
	s64 frame_length;
	struct camera_common_data *s_data = priv->s_data;
	const struct sensor_mode_properties *mode =
		&s_data->sensor_props.sensor_modes[s_data->mode];
	int i = 0;

	return 0;
	frame_length = mode->signal_properties.pixel_clock.val *
		FIXED_POINT_SCALING_FACTOR /
		mode->image_properties.line_length / val;

	priv->frame_length = (u32) frame_length;
	if (priv->frame_length > RAA462113_MAX_FRAME_LENGTH)
		priv->frame_length = RAA462113_MAX_FRAME_LENGTH;

	dev_dbg(&priv->i2c_client->dev,
		"%s: val: %lld, , frame_length: %d\n", __func__,
		val, priv->frame_length);

	raa462113_get_frame_length_regs(reg_list, priv->frame_length);

	for (i = 0; i < 3; i++) {
		err = raa462113_write_reg(priv->s_data, reg_list[i].addr,
			 reg_list[i].val);
		if (err)
			goto fail;
	}

	return 0;

fail:
	dev_dbg(&priv->i2c_client->dev,
		 "%s: FRAME_LENGTH control error\n", __func__);
	return err;
}
/*
 * raa462113_set_exposure - Function called when setting exposure
 * @priv: Pointer to device structure
 * @val: Value for exposure
 *
 * Set the exposure based on input value.
 *
 * Return: 0 on success, errors otherwise
 */
static int raa462113_set_exposure(struct raa462113 *priv, s64 val)
{
	int err;

	dev_dbg(&priv->i2c_client->dev,
		 "%s: val: %lld\n", __func__, val);

		err = raa462113_set_coarse_time(priv, val);
		if (err)
			dev_dbg(&priv->i2c_client->dev,
			"%s: error coarse time SHS1 override\n", __func__);
	return err;
}
/*
 * raa462113_set_coarse_time - Function called when setting SHR value
 * @priv: Pointer to raa462113 structure
 * @val: Value for exposure time in number of line_length, or [HMAX]
 *
 * Set SHR value based on input value.
 *
 * Return: 0 on success, errors otherwise
 */
static int raa462113_set_coarse_time(struct raa462113 *priv, s64 val)
{
	struct camera_common_data *s_data = priv->s_data;
	const struct sensor_mode_properties *mode =
		&s_data->sensor_props.sensor_modes[s_data->mode];
	raa462113_reg reg_list[1];
	int err;
	u32 coarse_time_shs1;
	u32 reg_shs1;
	int i = 0;

	coarse_time_shs1 = mode->signal_properties.pixel_clock.val *
		val / mode->image_properties.line_length /
		FIXED_POINT_SCALING_FACTOR;

	if (priv->frame_length == 0)
		priv->frame_length = RAA462113_MIN_FRAME_LENGTH;

	if(coarse_time_shs1 < 4)
		coarse_time_shs1 = 4;
	else if(coarse_time_shs1 > priv->frame_length)
		coarse_time_shs1 = priv->frame_length - 4;

	reg_shs1 =  coarse_time_shs1;

	dev_dbg(&priv->i2c_client->dev,
		 "%s: coarse1:%d, shs1:%d, FL:%d\n", __func__,
		 coarse_time_shs1, reg_shs1, priv->frame_length);

	raa462113_get_coarse_time_regs_shs1(reg_list, reg_shs1);

	for (i = 0; i < 1; i++) {
		err = raa462113_write_reg(priv->s_data, reg_list[i].addr,
			 reg_list[i].val);
		if (err)
			goto fail;
	}

	return 0;

fail:
	dev_dbg(&priv->i2c_client->dev,
		 "%s: set coarse time error\n", __func__);
	return err;
}

/*
 * raa462113_s_ctrl - Function called for setting V4L2 control operations
 * @ctrl: Pointer to V4L2 control structure
 *
 * This is used to set V4L2 control operations
 *
 * Return: 0 on success, errors otherwise
 */
static int raa462113_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct raa462113 *priv =
		container_of(ctrl->handler, struct raa462113, ctrl_handler);
	int err = 0;

	if (priv->power.state == SWITCH_OFF)
		return 0;

	switch (ctrl->id) {
	case TEGRA_CAMERA_CID_GAIN:
		err = raa462113_set_gain(priv, *ctrl->p_new.p_s64);
		break;
	case TEGRA_CAMERA_CID_EXPOSURE:
		err = raa462113_set_exposure(priv, *ctrl->p_new.p_s64);
		break;
	case TEGRA_CAMERA_CID_FRAME_RATE:
		err = raa462113_set_frame_rate(priv, *ctrl->p_new.p_s64);
		break;
	case TEGRA_CAMERA_CID_GROUP_HOLD:
		err = raa462113_set_group_hold(priv, ctrl->val);
		break;
	case TEGRA_CAMERA_CID_HDR_EN:
		break;
	default:
		pr_err("%s: unknown ctrl id.\n", __func__);
		return -EINVAL;
	}

	return err;
}
/*
 * raa462113_ctrls_init - Function to initialize V4L2 controls
 * @priv: Pointer to raa462113 structure
 *
 * This is used to initialize V4L2 controls for raa462113 sensor
 *
 * Return: 0 on success, errors otherwise
 */
static int raa462113_ctrls_init(struct raa462113 *priv)
{
	struct i2c_client *client = priv->i2c_client;
	struct v4l2_ctrl *ctrl;
	int num_ctrls;
	int err;
	int i;

	dev_dbg(&client->dev, "%s++\n", __func__);

	num_ctrls = ARRAY_SIZE(ctrl_config_list);
	v4l2_ctrl_handler_init(&priv->ctrl_handler, num_ctrls);

	for (i = 0; i < num_ctrls; i++) {
		ctrl = v4l2_ctrl_new_custom(&priv->ctrl_handler,
			&ctrl_config_list[i], NULL);
		if (ctrl == NULL) {
			dev_err(&client->dev, "Failed to init %s ctrl\n",
				ctrl_config_list[i].name);
			continue;
		}

		if (ctrl_config_list[i].type == V4L2_CTRL_TYPE_STRING &&
			ctrl_config_list[i].flags & V4L2_CTRL_FLAG_READ_ONLY) {
			ctrl->p_new.p_char = devm_kzalloc(&client->dev,
				ctrl_config_list[i].max + 1, GFP_KERNEL);
		}
		priv->ctrls[i] = ctrl;
	}

	priv->numctrls = num_ctrls;
	priv->subdev->ctrl_handler = &priv->ctrl_handler;
	if (priv->ctrl_handler.error) {
		dev_err(&client->dev, "Error %d adding controls\n",
			priv->ctrl_handler.error);
		err = priv->ctrl_handler.error;
		goto error;
	}

	err = v4l2_ctrl_handler_setup(&priv->ctrl_handler);
	if (err) {
		dev_err(&client->dev,
			"Error %d setting default controls\n", err);
		goto error;
	}

	return 0;

error:
	v4l2_ctrl_handler_free(&priv->ctrl_handler);
	return err;
}

MODULE_DEVICE_TABLE(of, raa462113_of_match);

/*
 * raa462113_parse_dt - Function to parse device tree
 * @client: Pointer to I2C client structure
 *
 * This is used to parse raa462113 device tree
 *
 * Return: Pointer to camera common pdata on success, NULL on error
 */
static struct camera_common_pdata *raa462113_parse_dt(struct raa462113 *priv,
				struct i2c_client *client,
				struct camera_common_data *s_data)
{
	struct device_node *np = client->dev.of_node;
	struct camera_common_pdata *board_priv_pdata;
	const struct of_device_id *match;
	int err;

	if (!np)
		return NULL;

	match = of_match_device(raa462113_of_match, &client->dev);
	if (!match) {
		dev_err(&client->dev, "Failed to find matching dt id\n");
		return NULL;
	}

	board_priv_pdata = devm_kzalloc(&client->dev,
			   sizeof(*board_priv_pdata), GFP_KERNEL);

	err = of_property_read_string(np, "mclk",
				      &board_priv_pdata->mclk_name);
	if (err)
		dev_err(&client->dev, "mclk not in DT\n");

	board_priv_pdata->reset_gpio = of_get_named_gpio(np, "reset-gpios", 0);
	if (err) {
		dev_err(&client->dev, "reset-gpios not found %d\n", err);
		board_priv_pdata->reset_gpio = 0;
	}

	return board_priv_pdata;
}
/*
 * raa462113_open - Function to open camera device
 * @fh: Pointer to V4L2 subdevice structure
 *
 * This function does nothing
 *
 * Return: 0 on success
 */
static int raa462113_open(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);

	dev_dbg(&client->dev, "%s:\n", __func__);

	return 0;
}
/*
 * Media operations
 */
static const struct v4l2_subdev_internal_ops raa462113_subdev_internal_ops = {
	.open = raa462113_open,
};

static const struct media_entity_operations raa462113_media_ops = {
	.link_validate = v4l2_subdev_link_validate,
};
/*
 * raa462113 probe - Function called for I2C driver
 * @client: Pointer to I2C client structure
 * @id: Pointer to I2C device id structure
 *
 * This is used to probe raa462113 sensor
 *
 * Return: 0 on success, errors otherwise
 */
static int raa462113_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	struct camera_common_data *common_data;
	struct raa462113 *priv;
	int err;

	dev_info(&client->dev, "[RAA462113]: probing v4l2 sensor at addr 0x%0x.\n",
		client->addr);

	if (!IS_ENABLED(CONFIG_OF) || !client->dev.of_node)
		return -EINVAL;

	common_data = devm_kzalloc(&client->dev,
			    sizeof(struct camera_common_data), GFP_KERNEL);

	priv = devm_kzalloc(&client->dev,
			    sizeof(struct raa462113) + sizeof(struct v4l2_ctrl *) *
			    ARRAY_SIZE(ctrl_config_list),
			    GFP_KERNEL);
	if (!priv) {
		dev_err(&client->dev, "unable to allocate memory!\n");
		return -ENOMEM;
	}

	priv->regmap = devm_regmap_init_i2c(client, &sensor_regmap_config);
	if (IS_ERR(priv->regmap)) {
		dev_err(&client->dev,
			"regmap init failed: %ld\n", PTR_ERR(priv->regmap));
		return -ENODEV;
	}

	if (client->dev.of_node)
		priv->pdata = raa462113_parse_dt(priv, client, common_data);
	if (!priv->pdata) {
		dev_err(&client->dev, "unable to get platform data\n");
		return -EFAULT;
	}

	common_data->ops		= &raa462113_common_ops;
	common_data->ctrl_handler	= &priv->ctrl_handler;
	common_data->dev		= &client->dev;
	common_data->frmfmt		= &raa462113_frmfmt[0];
	common_data->colorfmt		= camera_common_find_datafmt(
					  RAA462113_DEFAULT_DATAFMT);
	common_data->power		= &priv->power;
	common_data->ctrls		= priv->ctrls;
	common_data->priv		= (void *)priv;
	common_data->numctrls		= ARRAY_SIZE(ctrl_config_list);
	common_data->numfmts		= ARRAY_SIZE(raa462113_frmfmt);
	common_data->def_mode		= RAA462113_DEFAULT_MODE;
	common_data->def_width		= RAA462113_DEFAULT_WIDTH;
	common_data->def_height		= RAA462113_DEFAULT_HEIGHT;
	common_data->fmt_width		= common_data->def_width;
	common_data->fmt_height		= common_data->def_height;
	common_data->def_clk_freq	= RAA462113_DEFAULT_CLK_FREQ;

	priv->i2c_client		= client;
	priv->s_data			= common_data;
	priv->subdev			= &common_data->subdev;
	priv->subdev->dev		= &client->dev;

	err = raa462113_power_get(priv);
	if (err)
		return err;

	err = camera_common_initialize(common_data, "raa462113");
	if (err) {
		dev_err(&client->dev, "Failed to initialize raa462113.\n");
		return err;
	}

	v4l2_i2c_subdev_init(priv->subdev, client, &raa462113_subdev_ops);

	err = raa462113_ctrls_init(priv);
	if (err)
		return err;

	priv->subdev->internal_ops = &raa462113_subdev_internal_ops;
	priv->subdev->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE |
				V4L2_SUBDEV_FL_HAS_EVENTS;

#if defined(CONFIG_MEDIA_CONTROLLER)
	priv->pad.flags = MEDIA_PAD_FL_SOURCE;
	priv->subdev->entity.ops = &raa462113_media_ops;
	err = tegra_media_entity_init(&priv->subdev->entity, 1, &priv->pad, true, true);
	if (err < 0) {
		dev_err(&client->dev, "unable to init media entity\n");
		return err;
	}
#endif

	err = v4l2_async_register_subdev(priv->subdev);
	if (err)
		return err;

	dev_info(&client->dev, "Detected RAA462113 sensor\n");

	return 0;
}

/*
 * raa462113_remove - Function called for I2C driver
 * @client: Pointer to I2C client structure
 *
 * This is used to remove raa462113 sensor
 *
 * return: 0 one success
 */
static int raa462113_remove(struct i2c_client *client)
{
	struct camera_common_data *s_data = to_camera_common_data(&client->dev);
	struct raa462113 *priv = (struct raa462113 *)s_data->priv;

	v4l2_async_unregister_subdev(priv->subdev);
#if defined(CONFIG_MEDIA_CONTROLLER)
	media_entity_cleanup(&priv->subdev->entity);
#endif

	v4l2_ctrl_handler_free(&priv->ctrl_handler);
	camera_common_cleanup(s_data);

	return 0;
}

/*
 * Media related structure
 */
static const struct i2c_device_id raa462113_id[] = {
	{ "raa462113", 0 },
	{ }
};

MODULE_DEVICE_TABLE(i2c, raa462113_id);

static struct i2c_driver raa462113_i2c_driver = {
	.driver = {
		.name = "raa462113",
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(raa462113_of_match),
	},
	.probe = raa462113_probe,
	.remove = raa462113_remove,
	.id_table = raa462113_id,
};

module_i2c_driver(raa462113_i2c_driver);

MODULE_DESCRIPTION("Media Controller driver for Sony RAA462113");
MODULE_AUTHOR("NVIDIA Corporation");
MODULE_LICENSE("GPL v2");
