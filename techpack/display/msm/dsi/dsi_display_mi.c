/*
 * Copyright (c) 2019, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#define pr_fmt(fmt)	"mi-dsi-display:[%s] " fmt, __func__

#include <linux/list.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/err.h>

#include "msm_drv.h"
#include "sde_connector.h"
#include "msm_mmu.h"
#include "dsi_display.h"
#include "dsi_panel.h"
#include "dsi_panel_mi.h"
#include "dsi_ctrl.h"
#include "dsi_ctrl_hw.h"
#include "dsi_drm.h"
#include "dsi_clk.h"
#include "dsi_pwr.h"
#include "sde_dbg.h"
#include "dsi_parser.h"
#include "dsi_mi_feature.h"
#include "../msm_drv.h"

#define to_dsi_bridge(x)     container_of((x), struct dsi_bridge, base)

static atomic64_t g_param = ATOMIC64_INIT(0);

int dsi_display_set_disp_param(struct drm_connector *connector,
			u32 param_type)
{
	struct dsi_display *display = NULL;
	struct dsi_bridge *c_bridge = NULL;
	int ret = 0;

	if (!connector || !connector->encoder || !connector->encoder->bridge) {
		pr_err("Invalid connector/encoder/bridge ptr\n");
		return -EINVAL;
	}

	c_bridge =  to_dsi_bridge(connector->encoder->bridge);
	display = c_bridge->display;
	if (!display || !display->panel) {
		pr_err("Invalid display/panel ptr\n");
		return -EINVAL;
	}

	atomic64_set(&g_param, param_type);
	if (sde_kms_is_suspend_blocked(display->drm_dev) &&
		dsi_panel_is_need_tx_cmd(param_type)) {
		pr_err("sde_kms is suspended, skip to set_disp_param\n");
		return -EBUSY;
	}

	ret = dsi_panel_set_disp_param(display->panel, param_type);

	return ret;
}

int dsi_display_hbm_set_disp_param(struct drm_connector *connector,
				u32 op_code)
{
	int rc;
	struct sde_connector *c_conn;

	c_conn = to_sde_connector(connector);

	pr_debug("%s fod hbm command:0x%x \n", __func__, op_code);

	if (op_code == DISPPARAM_HBM_FOD_ON) {
		rc = dsi_display_set_disp_param(connector, DISPPARAM_HBM_FOD_ON);
	} else if (op_code == DISPPARAM_HBM_FOD_OFF) {
		/* close HBM and restore DC */
		rc = dsi_display_set_disp_param(connector, DISPPARAM_HBM_FOD_OFF);
	} else if(op_code == DISPPARAM_DIMMING_OFF) {
		rc = dsi_display_set_disp_param(connector, DISPPARAM_DIMMING_OFF);
	} else if (op_code == DISPPARAM_HBM_BACKLIGHT_RESEND) {
		rc = dsi_display_set_disp_param(connector, DISPPARAM_HBM_BACKLIGHT_RESEND);
	}

	return rc;
}

int dsi_display_esd_irq_ctrl(struct dsi_display *display,
		bool enable)
{
	int rc = 0;

	if (!display) {
		DSI_ERR("Invalid params\n");
		return -EINVAL;
	}

	mutex_lock(&display->display_lock);

	rc = dsi_panel_esd_irq_ctrl(display->panel, enable);
	if (rc)
		pr_err("[%s] failed to set esd irq, rc=%d\n",
				display->name, rc);

	mutex_unlock(&display->display_lock);

	return rc;
}

int mi_dsi_panel_dc_switch(struct dsi_panel *panel, bool enabled)
{
	int rc = 0;

	if (!panel) {
		DSI_ERR("Invalid params\n");
		return -EINVAL;
	}

	mutex_lock(&panel->panel_lock);

	if (enabled) {
		rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_DC_ON);
		if (rc)
			DSI_ERR("[%s] failed to send DSI_CMD_SET_MI_DC_ON cmd, rc=%d\n",
					panel->name, rc);
	} else {
		rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_DC_OFF);
		if (rc)
			DSI_ERR("[%s] failed to send DSI_CMD_SET_MI_DC_OFF cmd, rc=%d\n",
				panel->name, rc);
	}
	pr_debug("[%s] tx dc success, dc status %d",
		panel->name, enabled);

	mutex_unlock(&panel->panel_lock);
	display_utc_time_marker("DSI_CMD_SET_DC_CMD");
	return rc;
}

int mi_dsi_pwr_enable_vregs(struct dsi_regulator_info *regs, bool enable, int index)
{
	int rc = 0;
	struct dsi_vreg *vreg;
	int num_of_v = 0;
	u32 pre_on_ms, post_on_ms;
	u32 pre_off_ms, post_off_ms;
	if (enable) {
		vreg = &regs->vregs[index];
		pre_on_ms = vreg->pre_on_sleep;
		post_on_ms = vreg->post_on_sleep;
		DSI_INFO("mi_dsi_pwr_enable_vregs for %s, enable:%d\n",vreg->vreg_name,enable);
		if (vreg->pre_on_sleep)
			usleep_range((pre_on_ms * 1000),
					(pre_on_ms * 1000) + 10);

		rc = regulator_set_load(vreg->vreg,
					vreg->enable_load);
		if (rc < 0) {
			DSI_ERR("Setting optimum mode failed for %s\n",
					vreg->vreg_name);
			goto error;
		}
		num_of_v = regulator_count_voltages(vreg->vreg);
		if (num_of_v > 0) {
			rc = regulator_set_voltage(vreg->vreg,
							vreg->min_voltage,
							vreg->max_voltage);
			if (rc) {
				DSI_ERR("Set voltage(%s) fail, rc=%d\n",
						vreg->vreg_name, rc);
				goto error_disable_opt_mode;
			}
		}

		rc = regulator_enable(vreg->vreg);
		if (rc) {
			DSI_ERR("enable failed for %s, rc=%d\n",
					vreg->vreg_name, rc);
			goto error_disable_voltage;
		}

		if (vreg->post_on_sleep)
			usleep_range((post_on_ms * 1000),
					(post_on_ms * 1000) + 10);
	} else {
		vreg = &regs->vregs[index];
		pre_off_ms = vreg->pre_off_sleep;
		post_off_ms = vreg->post_off_sleep;
		DSI_ERR("mi_dsi_pwr_enable_vregs for %s, enable:%d\n",vreg->vreg_name,enable,index);
		if (pre_off_ms)
			usleep_range((pre_off_ms * 1000),
					(pre_off_ms * 1000) + 10);

		if (regs->vregs[index].off_min_voltage)
			(void)regulator_set_voltage(regs->vregs[index].vreg,
					regs->vregs[index].off_min_voltage,
					regs->vregs[index].max_voltage);

		(void)regulator_set_load(regs->vregs[index].vreg,
					regs->vregs[index].disable_load);
		(void)regulator_disable(regs->vregs[index].vreg);

		if (post_off_ms)
			usleep_range((post_off_ms * 1000),
					(post_off_ms * 1000) + 10);
	}

	return 0;
error_disable_opt_mode:
	(void)regulator_set_load(regs->vregs[index].vreg,
				 regs->vregs[index].disable_load);

error_disable_voltage:
	if (num_of_v > 0)
		(void)regulator_set_voltage(regs->vregs[index].vreg,
					    0, regs->vregs[index].max_voltage);
error:
	vreg = &regs->vregs[index];
	pre_off_ms = vreg->pre_off_sleep;
	post_off_ms = vreg->post_off_sleep;

	if (pre_off_ms)
		usleep_range((pre_off_ms * 1000),
				(pre_off_ms * 1000) + 10);

	(void)regulator_set_load(regs->vregs[index].vreg,
					regs->vregs[index].disable_load);

	num_of_v = regulator_count_voltages(regs->vregs[index].vreg);
	if (num_of_v > 0)
		(void)regulator_set_voltage(regs->vregs[index].vreg,
					0, regs->vregs[index].max_voltage);

	(void)regulator_disable(regs->vregs[index].vreg);

	if (post_off_ms)
		usleep_range((post_off_ms * 1000),
				(post_off_ms * 1000) + 10);

	return rc;
}
