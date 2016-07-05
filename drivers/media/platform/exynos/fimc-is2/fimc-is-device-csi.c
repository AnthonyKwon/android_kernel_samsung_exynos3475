/*
 * Samsung Exynos5 SoC series FIMC-IS driver
 *
 * exynos5 fimc-is/mipi-csi functions
 *
 * Copyright (c) 2011 Samsung Electronics Co., Ltd
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#include <mach/map.h>
#include <mach/regs-clock.h>
#include <mach/irqs.h>
#include <mach/exynos5-mipiphy.h>

#include "fimc-is-config.h"
#include "fimc-is-regs.h"
#include "fimc-is-hw.h"
#include "fimc-is-device-csi.h"
#include "fimc-is-device-sensor.h"
#include "fimc-is-hw-csi.h"

static u32 get_hsync_settle(struct fimc_is_sensor_cfg *cfg,
	const u32 cfgs, u32 width, u32 height, u32 framerate)
{
	u32 settle;
	u32 max_settle;
	u32 proximity_framerate, proximity_settle;
	u32 i;

	settle = 0;
	max_settle = 0;
	proximity_framerate = 0;
	proximity_settle = 0;

	for (i = 0; i < cfgs; i++) {
		if ((cfg[i].width == width) &&
		    (cfg[i].height == height) &&
		    (cfg[i].framerate == framerate)) {
			settle = cfg[i].settle;
			break;
		}

		if ((cfg[i].width == width) &&
		    (cfg[i].height == height) &&
		    (cfg[i].framerate > proximity_framerate)) {
			proximity_settle = cfg[i].settle;
			proximity_framerate = cfg[i].framerate;
		}

		if (cfg[i].settle > max_settle)
			max_settle = cfg[i].settle;
	}

	if (!settle) {
		if (proximity_settle) {
			settle = proximity_settle;
		} else {
			/*
			 * return a max settle time value in above table
			 * as a default depending on the channel
			 */
			settle = max_settle;

			warn("could not find proper settle time: %dx%d@%dfps",
				width, height, framerate);
		}
	}

	return settle;
}

static u32 get_vci_channel(struct fimc_is_vci *vci,
	const u32 vcis, u32 pixelformat)
{
	u32 i;
	u32 index = vcis;

	BUG_ON(!vci);

	for (i = 0; i < vcis; i++) {
		if (vci[i].pixelformat == pixelformat) {
			index = i;
			break;
		}
	}

	if (index == vcis) {
		err("invalid vc setting(foramt : %d)", pixelformat);
		BUG();
	}

	return index;
}

#ifdef ENABLE_CSIISR
static irqreturn_t fimc_is_csi_isr(int irq, void *data)
{
	u32 status;
	struct fimc_is_device_csi *csi;

	csi = data;

#ifndef ENABLE_A5
	/* Below function get and clear interrupts */
	status = csi_hw_g_interrupt(csi->base_reg);

	if (status & S5PCSIS_INTSRC_FRAME_END) {
		u32 vsync_cnt = 0;

		/* Expecting vvalid is 1 in case of NO_ERROR */
		if (likely(atomic_read(&csi->vvalid) == 1)) {
			atomic_set(&csi->vvalid, 0);
		} else if (atomic_read(&csi->vvalid) > 1) {
			/* How many times occured FRAME START without END */
			err("CSI%d : Frame start without Frame end %d times\n",
					csi->instance, atomic_read(&csi->vvalid));
		} else {
			/* When vvalid is less than or equal ZERO */
			pr_debug("CSI%d : Frame end without Frame start\n", csi->instance);
			atomic_dec(&csi->vvalid);
		}

		atomic_set(&csi->vblank_count, atomic_read(&csi->vsync_count));

		vsync_cnt = atomic_read(&csi->vsync_count);
		v4l2_subdev_notify(*csi->subdev, CSI_NOTIFY_ACTUATOR, &vsync_cnt);
	}

	if (status & S5PCSIS_INTSRC_FRAME_START) {
		u32 vsync_cnt = 0;

		/* Expecting vvalid is 0 in case of NO_ERROR */
		if (likely(atomic_read(&csi->vvalid) == 0)) {
			atomic_set(&csi->vvalid, 1);
		} else if (atomic_read(&csi->vvalid) < 0) {
			/* How many times occured FRAME END without START */
			err("CSI%d : Frame end without Frame start %ld times\n",
					csi->instance, abs(atomic_read(&csi->vvalid)));
		} else {
			/* When vvalid is bigger than ZERO */
			pr_debug("CSI%d : Frame start without Frame end\n", csi->instance);
			atomic_inc(&csi->vvalid);
		}

		atomic_inc(&csi->vsync_count);

		vsync_cnt = atomic_read(&csi->vsync_count);
		v4l2_subdev_notify(*csi->subdev, CSI_NOTIFY_VSYNC, &vsync_cnt);
		v4l2_subdev_notify(*csi->subdev, CSI_NOTIFY_FLASH, &vsync_cnt);
	}

	if (unlikely(status & S5PCSIS_INTSRC_ERRORS))
		goto err_status;

	return IRQ_HANDLED;

err_status:
	/* TODO: Belows are Error interrupt handling. */
	if (status & S5PCSIS_INTSRC_ERR_ID) {
		err("CSI%d : ERR_ID, irq%d(%#X)\n",csi->instance, irq, status);
	}

	if (status & S5PCSIS_INTSRC_ERR_CRC) {
		err("CSI%d : ERR_CRC, irq%d(%#X)\n",csi->instance, irq, status);
	}

	if (status & S5PCSIS_INTSRC_ERR_ECC) {
		err("CSI%d : ERR_ECC, irq%d(%#X)\n",csi->instance, irq, status);
	}

	if (status & S5PCSIS_INTSRC_ERR_OVER) {
		err("CSI%d : ERR_OVER, irq%d(%#X)\n",csi->instance, irq, status);
	}

	if (status & S5PCSIS_INTSRC_ERR_LOST_FE) {
		err("CSI%d : ERR_LOST_FE, irq%d(%#X)\n",csi->instance, irq, status);
	}

	if (status & S5PCSIS_INTSRC_ERR_LOST_FS) {
		err("CSI%d : ERR_LOST_FS, irq%d(%#X)\n",csi->instance, irq, status);
	}

	if (status & S5PCSIS_INTSRC_ERR_SOT_HS) {
		err("CSI%d : ERR_SOT_HS, irq%d(%#X)\n",csi->instance, irq, status);
	}

	return IRQ_HANDLED;
#else
	status = csi_hw_g_interrupt(csi->base_reg);
	info("CSI%d : irq%d(%X)\n",csi->instance, irq, status);

	return IRQ_HANDLED;
#endif

}
#endif

int fimc_is_csi_open(struct v4l2_subdev *subdev)
{
	int ret = 0;
	u32 instance = 0;
	struct fimc_is_device_csi *csi;
	struct fimc_is_device_sensor *device;

	BUG_ON(!subdev);

	csi = v4l2_get_subdevdata(subdev);
	if (!csi) {
		err("csi is NULL");
		ret = -EINVAL;
		goto p_err;
	}

	device = v4l2_get_subdev_hostdata(subdev);
	if (!device) {
		err("device is NULL");
		ret = -EINVAL;
		goto p_err;
	}

	csi->sensor_cfgs = 0;
	csi->sensor_cfg = NULL;
	memset(&csi->image, 0, sizeof(struct fimc_is_image));

#ifndef ENABLE_A5
	atomic_set(&csi->vsync_count, 0);
	atomic_set(&csi->vblank_count, 0);

	atomic_set(&csi->vvalid, 0);
#endif

	instance = csi->instance;
	switch (instance) {
	case CSI_ID_A:
		csi->base_reg = (u32 *)MIPICSI0_REG_BASE;
		csi->irq[CSI_ID_A] = platform_get_irq(device->pdev, 1);
		if (csi->irq[CSI_ID_A] < 0) {
			dev_err(&device->pdev->dev, "Failed to get irq\n");
			goto p_err;
		}
#ifdef ENABLE_CSIISR
		ret = request_irq(csi->irq[CSI_ID_A],
			fimc_is_csi_isr,
			IRQF_GIC_MULTI_TARGET,
			"mipi-csi0",
			csi);
		if (ret) {
			err("request_irq(CSI_ID_A) is fail(%d)", ret);
			goto p_err;
		}
#endif
		break;
	case CSI_ID_B:
		csi->base_reg = (u32 *)MIPICSI1_REG_BASE;
		csi->irq[CSI_ID_B] = platform_get_irq(device->pdev, 1);
		if (csi->irq[CSI_ID_B] < 0) {
			dev_err(&device->pdev->dev, "Failed to get irq\n");
			goto p_err;
		}
#ifdef ENABLE_CSIISR
		ret = request_irq(csi->irq[CSI_ID_B],
			fimc_is_csi_isr,
			IRQF_GIC_MULTI_TARGET,
			"mipi-csi1",
			csi);
		if (ret) {
			err("request_irq(CSI_ID_B) is fail(%d)", ret);
			goto p_err;
		}
#endif
		break;
	case CSI_ID_C:
		csi->base_reg = (u32 *)MIPICSI2_REG_BASE;
		csi->irq[CSI_ID_C] = platform_get_irq(device->pdev, 1);
		if (csi->irq[CSI_ID_C] < 0) {
			dev_err(&device->pdev->dev, "Failed to get irq\n");
			goto p_err;
		}
#ifdef ENABLE_CSIISR
		ret = request_irq(csi->irq[CSI_ID_C],
			fimc_is_csi_isr,
			IRQF_GIC_MULTI_TARGET,
			"mipi-csi1",
			csi);
		if (ret) {
			err("request_irq(CSI_ID_C) is fail(%d)", ret);
			goto p_err;
		}
#endif
		break;
	case CSI_ID_D:
		csi->base_reg = (u32 *)MIPICSI3_REG_BASE;
		csi->irq[CSI_ID_D] = platform_get_irq(device->pdev, 1);
		if (csi->irq[CSI_ID_D] < 0) {
			dev_err(&device->pdev->dev, "Failed to get irq\n");
			goto p_err;
		}
#ifdef ENABLE_CSIISR
		ret = request_irq(csi->irq[CSI_ID_D],
			fimc_is_csi_isr,
			IRQF_GIC_MULTI_TARGET,
			"mipi-csi1",
			csi);
		if (ret) {
			err("request_irq(CSI_ID_D) is fail(%d)", ret);
			goto p_err;
		}
#endif
		break;
	default:
		err("instance is invalid(%d)", instance);
		ret = -EINVAL;
		goto p_err;
	}

p_err:
	return ret;
}

int fimc_is_csi_close(struct v4l2_subdev *subdev)
{
	int ret = 0;
	struct fimc_is_device_csi *csi;

	BUG_ON(!subdev);

	csi = v4l2_get_subdevdata(subdev);
	if (!csi) {
		err("csi is NULL");
		ret = -EINVAL;
		goto p_err;
	}

#ifdef ENABLE_CSIISR
	switch(csi->instance) {
	case CSI_ID_A:
		free_irq(csi->irq[CSI_ID_A], csi);
		break;
	case CSI_ID_B:
		free_irq(csi->irq[CSI_ID_B], csi);
		break;
	case CSI_ID_C:
		free_irq(csi->irq[CSI_ID_C], csi);
		break;
	case CSI_ID_D:
		free_irq(csi->irq[CSI_ID_D], csi);
		break;
	default:
		err("instance is invalid(%d)", csi->instance);
		ret = -EINVAL;
	}
#endif

p_err:
	return ret;
}

/* value : module enum */
static int csi_init(struct v4l2_subdev *subdev, u32 value)
{
	int ret = 0;
	struct fimc_is_device_csi *csi;
	struct fimc_is_module_enum *module;
	struct fimc_is_device_sensor *device;

	BUG_ON(!subdev);

	csi = v4l2_get_subdevdata(subdev);
	if (!csi) {
		err("csi is NULL");
		ret = -EINVAL;
		goto p_err;
	}

	device = container_of(csi->subdev, struct fimc_is_device_sensor, subdev_csi);
	module = &device->module_enum[value];

	csi->sensor_cfgs = module->cfgs;
	csi->sensor_cfg = module->cfg;
	csi->vcis = module->vcis;
	csi->vci = module->vci;
	csi->image.framerate = SENSOR_DEFAULT_FRAMERATE; /* default frame rate */
	csi->mode = module->mode;
	csi->lanes = module->lanes;

p_err:
	return ret;
}

static int csi_s_power(struct v4l2_subdev *subdev,
	int on)
{
	int ret = 0;
	struct fimc_is_device_sensor *sensor;
	struct fimc_is_device_csi *csi;

	BUG_ON(!subdev);

	sensor = (struct fimc_is_device_sensor *)v4l2_get_subdev_hostdata(subdev);
	if (!sensor) {
		err("sensor is NULL");
		return -EINVAL;
	}

	csi = (struct fimc_is_device_csi *)v4l2_get_subdevdata(subdev);
	if (!csi) {
		err("csi is NULL");
		return -EINVAL;
	}

	ret = exynos5_csis_phy_enable(csi->instance, sensor->instance, on);
	if (ret) {
		err("fail to csi%d power on", csi->instance);
		goto p_err;
	}

p_err:
	mdbgd_front("%s(%d, %d)\n", csi, __func__, on, ret);
	return ret;
}

static const struct v4l2_subdev_core_ops core_ops = {
	.init = csi_init,
	.s_power = csi_s_power
};

static int csi_stream_on(struct fimc_is_device_csi *csi)
{
	int ret = 0;
	u32 settle, index;
	u32 __iomem *base_reg;

	BUG_ON(!csi);
	BUG_ON(!csi->sensor_cfg);

	base_reg = csi->base_reg;

	settle = get_hsync_settle(
		csi->sensor_cfg,
		csi->sensor_cfgs,
		csi->image.window.width,
		csi->image.window.height,
		csi->image.framerate);
	minfo("[CSI] settle(%dx%d@%d) = %d\n", csi,
		csi->image.window.width,
		csi->image.window.height,
		csi->image.framerate,
		settle);

	index = get_vci_channel(csi->vci, csi->vcis, csi->image.format.pixelformat);
	minfo("[CSI] vci(0x%X) = %d\n", csi,
		csi->image.format.pixelformat,
		index);

	csi_hw_reset(base_reg);
	csi_hw_s_settle(base_reg, settle);
	csi_hw_s_control(base_reg, csi->image.format.pixelformat, csi->mode, csi->lanes);
	if (csi->mode == CSI_MODE_CH0_ONLY) {
		csi_hw_s_config(base_reg,
			CSI_VIRTUAL_CH_0,
			&csi->vci[index].config[CSI_VIRTUAL_CH_0],
			csi->image.window.width,
			csi->image.window.height);
	} else {
		csi_hw_s_config(base_reg,
			CSI_VIRTUAL_CH_0,
			&csi->vci[index].config[CSI_VIRTUAL_CH_0],
			csi->image.window.width,
			csi->image.window.height);
		csi_hw_s_config(base_reg,
			CSI_VIRTUAL_CH_1,
			&csi->vci[index].config[CSI_VIRTUAL_CH_1],
			csi->image.window.width,
			csi->image.window.height);
		csi_hw_s_config(base_reg,
			CSI_VIRTUAL_CH_2,
			&csi->vci[index].config[CSI_VIRTUAL_CH_2],
			csi->image.window.width,
			csi->image.window.height);
	}

	csi_hw_s_interrupt(base_reg, true);
	csi_hw_enable(base_reg, csi->lanes);

	return ret;
}

static int csi_stream_off(struct fimc_is_device_csi *csi)
{
	int ret = 0;
	u32 __iomem *base_reg;

	BUG_ON(!csi);

	base_reg = csi->base_reg;

	csi_hw_s_interrupt(base_reg, false);
	csi_hw_disable(base_reg);

	return ret;
}

static int csi_s_stream(struct v4l2_subdev *subdev, int enable)
{
	int ret = 0;
	struct fimc_is_device_csi *csi;

	BUG_ON(!subdev);

	csi = (struct fimc_is_device_csi *)v4l2_get_subdevdata(subdev);
	if (!csi) {
		err("csi is NULL");
		return -EINVAL;
	}

	if (enable) {
		ret = csi_stream_on(csi);
		if (ret) {
			err("csi_stream_on is fail(%d)", ret);
			goto p_err;
		}
	} else {
		ret = csi_stream_off(csi);
		if (ret) {
			err("csi_stream_off is fail(%d)", ret);
			goto p_err;
		}
#ifndef ENABLE_A5
		atomic_set(&csi->vvalid, 0);
#endif
	}

p_err:
	return 0;
}

static int csi_s_param(struct v4l2_subdev *subdev, struct v4l2_streamparm *param)
{
	int ret = 0;
	struct fimc_is_device_csi *csi;
	struct v4l2_captureparm *cp;
	struct v4l2_fract *tpf;

	BUG_ON(!subdev);
	BUG_ON(!param);

	cp = &param->parm.capture;
	tpf = &cp->timeperframe;

	csi = (struct fimc_is_device_csi *)v4l2_get_subdevdata(subdev);
	if (!csi) {
		err("csi is NULL");
		return -EINVAL;
	}

	csi->image.framerate = tpf->denominator / tpf->numerator;

	mdbgd_front("%s(%d, %d)\n", csi, __func__, csi->image.framerate, ret);
	return ret;
}

static int csi_s_format(struct v4l2_subdev *subdev, struct v4l2_mbus_framefmt *fmt)
{
	int ret = 0;
	struct fimc_is_device_csi *csi;

	BUG_ON(!subdev);
	BUG_ON(!fmt);

	csi = (struct fimc_is_device_csi *)v4l2_get_subdevdata(subdev);
	if (!csi) {
		err("csi is NULL");
		return -EINVAL;
	}

	csi->image.window.offs_h = 0;
	csi->image.window.offs_v = 0;
	csi->image.window.width = fmt->width;
	csi->image.window.height = fmt->height;
	csi->image.window.o_width = fmt->width;
	csi->image.window.o_height = fmt->height;
	csi->image.format.pixelformat = fmt->code;
	csi->image.format.field = fmt->field;

	mdbgd_front("%s(%dx%d, %X)\n", csi, __func__, fmt->width, fmt->height, fmt->code);
	return ret;
}

static const struct v4l2_subdev_video_ops video_ops = {
	.s_stream = csi_s_stream,
	.s_parm = csi_s_param,
	.s_mbus_fmt = csi_s_format
};

static const struct v4l2_subdev_ops subdev_ops = {
	.core = &core_ops,
	.video = &video_ops
};

int fimc_is_csi_probe(void *parent, u32 instance)
{
	int ret = 0;
	struct v4l2_subdev *subdev_csi;
	struct fimc_is_device_csi *csi;
	struct fimc_is_device_sensor *device = parent;

	BUG_ON(!device);

	subdev_csi = kzalloc(sizeof(struct v4l2_subdev), GFP_KERNEL);
	if (!subdev_csi) {
		merr("subdev_csi is NULL", device);
		ret = -ENOMEM;
		goto p_err;
	}
	device->subdev_csi = subdev_csi;

	csi = kzalloc(sizeof(struct fimc_is_device_csi), GFP_KERNEL);
	if (!csi) {
		merr("csi is NULL", device);
		ret = -ENOMEM;
		goto p_err_free1;
	}
	/* pointer to me from device sensor */
	csi->subdev = &device->subdev_csi;

	csi->instance = instance;

	v4l2_subdev_init(subdev_csi, &subdev_ops);
	v4l2_set_subdevdata(subdev_csi, csi);
	v4l2_set_subdev_hostdata(subdev_csi, device);
	snprintf(subdev_csi->name, V4L2_SUBDEV_NAME_SIZE, "csi-subdev.%d", instance);
	ret = v4l2_device_register_subdev(&device->v4l2_dev, subdev_csi);
	if (ret) {
		merr("v4l2_device_register_subdev is fail(%d)", device, ret);
		goto p_err_free2;
	}

	info("[%d][FRT:D] %s(%d)\n", instance, __func__, ret);
	return 0;

p_err_free2:
	kfree(csi);

p_err_free1:
	kfree(subdev_csi);
	device->subdev_csi = NULL;

p_err:
	err("[%d][FRT:D] %s(%d)\n", instance, __func__, ret);
	return ret;
}
