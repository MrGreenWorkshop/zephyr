/*
 * Copyright (c) 2022, Commonwealth Scientific and Industrial Research
 * Organisation (CSIRO) ABN 41 687 119 230.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT power_domain_gpio

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/pm/device.h>
#include <zephyr/pm/device_runtime.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(power_domain_gpio, CONFIG_POWER_DOMAIN_LOG_LEVEL);

struct pd_gpio_config {
	struct gpio_dt_spec enable;
	uint32_t startup_delay_us;
	uint32_t off_on_delay_us;
	bool enable_high_drive;
};

struct pd_gpio_data {
	k_timeout_t next_boot;
};

static int pd_gpio_pm_action(const struct device *dev,
			     enum pm_device_action action)
{
	const struct pd_gpio_config *cfg = dev->config;
	struct pd_gpio_data *data = dev->data;
	int64_t next_boot_ticks;
	gpio_flags_t flags = 0;
	int rc = 0;

	/* Validate that blocking API's can be used */
	if (!k_can_yield()) {
		LOG_ERR("Blocking actions cannot run in this context");
		return -ENOTSUP;
	}

	switch (action) {
	case PM_DEVICE_ACTION_RESUME:
		/* Wait until we can boot again */
		k_sleep(data->next_boot);
		/* Switch power on */
		gpio_pin_set_dt(&cfg->enable, 1);
		LOG_INF("%s is now ON", dev->name);
		/* Wait for domain to come up */
		k_sleep(K_USEC(cfg->startup_delay_us));
		/* Notify supported devices they are now powered */
		pm_device_children_action_run(dev, PM_DEVICE_ACTION_TURN_ON, NULL);
		break;
	case PM_DEVICE_ACTION_SUSPEND:
		/* Notify supported devices power is going down */
		pm_device_children_action_run(dev, PM_DEVICE_ACTION_TURN_OFF, NULL);
		/* Switch power off */
		gpio_pin_set_dt(&cfg->enable, 0);
		LOG_INF("%s is now OFF", dev->name);
		/* Store next time we can boot */
		next_boot_ticks = k_uptime_ticks() + k_us_to_ticks_ceil32(cfg->off_on_delay_us);
		data->next_boot = K_TIMEOUT_ABS_TICKS(next_boot_ticks);
		break;
	case PM_DEVICE_ACTION_TURN_ON:
		/* DS_ALT is the highest drive strength for both directions */
		if (cfg->enable_high_drive) {
			flags |= GPIO_DS_ALT;
		}
		/* Actively control the enable pin now that the device is powered */
		gpio_pin_configure_dt(&cfg->enable, GPIO_OUTPUT_INACTIVE | flags);
		LOG_DBG("%s is OFF and powered", dev->name);
		break;
	case PM_DEVICE_ACTION_TURN_OFF:
		/* Let the enable pin float while device is not powered */
		gpio_pin_configure_dt(&cfg->enable, GPIO_DISCONNECTED);
		LOG_DBG("%s is OFF and not powered", dev->name);
		break;
	default:
		rc = -ENOTSUP;
	}

	return rc;
}

static int pd_gpio_init(const struct device *dev)
{
	const struct pd_gpio_config *cfg = dev->config;
	struct pd_gpio_data *data = dev->data;

	if (!device_is_ready(cfg->enable.port)) {
		LOG_ERR("GPIO port %s is not ready", cfg->enable.port->name);
		return -ENODEV;
	}
	if (dev->pm->domain && !device_is_ready(dev->pm->domain)) {
		LOG_ERR("Invalid domain sequencing! %s depends on %s",
			dev->name, dev->pm->domain->name);
		return -EINVAL;
	}

	/* We can't know how long the domain has been off for before boot */
	data->next_boot = K_TIMEOUT_ABS_US(cfg->off_on_delay_us);

	/* Configure control pin for OFF */
	pd_gpio_pm_action(dev, PM_DEVICE_ACTION_TURN_OFF);

	/* Boot into appropriate power mode */
	return pm_device_driver_init(dev, pd_gpio_pm_action);
}

#define POWER_DOMAIN_DEVICE(id)								\
	static const struct pd_gpio_config pd_gpio_##id##_cfg = {			\
		.enable = GPIO_DT_SPEC_INST_GET(id, enable_gpios),			\
		.startup_delay_us = DT_INST_PROP(id, startup_delay_us),			\
		.off_on_delay_us = DT_INST_PROP(id, off_on_delay_us),			\
		.enable_high_drive = DT_INST_PROP_OR(id, enable_pin_high_drive, false),	\
	};										\
	static struct pd_gpio_data pd_gpio_##id##_data;					\
	PM_DEVICE_DT_INST_DEFINE(id, pd_gpio_pm_action);				\
	DEVICE_DT_INST_DEFINE(id, pd_gpio_init, PM_DEVICE_DT_INST_GET(id),		\
			      &pd_gpio_##id##_data, &pd_gpio_##id##_cfg,		\
			      POST_KERNEL, DT_INST_PROP(id, init_priority),		\
			      NULL);

DT_INST_FOREACH_STATUS_OKAY(POWER_DOMAIN_DEVICE)
