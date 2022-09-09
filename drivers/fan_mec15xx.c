/*
 * Copyright (c) 2020 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr.h>
#include <drivers/pwm.h>
#include <drivers/sensor.h>
#include <logging/log.h>
#include "gpio_ec.h"
#include "board_config.h"
#include "fan.h"

LOG_MODULE_REGISTER(fan, CONFIG_FAN_LOG_LEVEL);

/*
 * PWM channel uses main clock frequency i.e. 48 MHz.
 * For desired frequency of 60 kHz, division should be:
 *
 * division = main clock freq / desired freq.
 *          = 48 MHz / 60 kHz = 800.
 *
 * PWM Duty cycle = pulse width / period.
 *
 * To calculate duty cycle in percentage, multiplier should be:
 * multiplier = division / 100
 *            = 800 / 100 = 8.
 */
#define	PWM_FREQ_MULT		8

#define MAX_DUTY_CYCLE		100u

#define DT_PWM_INST(x)		DT_NODELABEL(pwm##x)
#define DT_TACH_INST(x)		DT_NODELABEL(tach##x)
#define PWM_LABEL(x)		DT_LABEL(DT_PWM_INST(x))
#define TACH_LABEL(x)		DT_LABEL(DT_TACH_INST(x))

#if defined(CONFIG_SOC_MEC172x_NLJ)
#define PWM_DEV_LIST_SIZE	(PWM_CH_11 + 1)
#else
#define	PWM_DEV_LIST_SIZE	(PWM_CH_08 + 1)
#endif
#define	TACH_DEV_LIST_SIZE	(TACH_CH_03 + 1)

static const struct device *pwm_dev[PWM_DEV_LIST_SIZE];
static const struct device *tach_dev[TACH_DEV_LIST_SIZE];

static struct fan_dev fan_table[] = {
	{ PWM_CH_00,	TACH_CH_00	},
	{ PWM_CH_UNDEF,	TACH_CH_UNDEF	},
	{ PWM_CH_UNDEF,	TACH_CH_UNDEF	},
	{ PWM_CH_UNDEF,	TACH_CH_UNDEF	},
};

static void init_pwm_devices(void)
{
#if DT_NODE_HAS_STATUS(DT_PWM_INST(0), okay)
	pwm_dev[PWM_CH_00] = device_get_binding(PWM_LABEL(0));
#endif
#if DT_NODE_HAS_STATUS(DT_PWM_INST(1), okay)
	pwm_dev[PWM_CH_01] = device_get_binding(PWM_LABEL(1));
#endif
#if DT_NODE_HAS_STATUS(DT_PWM_INST(2), okay)
	pwm_dev[PWM_CH_02] = device_get_binding(PWM_LABEL(2));
#endif
#if DT_NODE_HAS_STATUS(DT_PWM_INST(3), okay)
	pwm_dev[PWM_CH_03] = device_get_binding(PWM_LABEL(3));
#endif
#if DT_NODE_HAS_STATUS(DT_PWM_INST(4), okay)
	pwm_dev[PWM_CH_04] = device_get_binding(PWM_LABEL(4));
#endif
#if DT_NODE_HAS_STATUS(DT_PWM_INST(5), okay)
	pwm_dev[PWM_CH_05] = device_get_binding(PWM_LABEL(5));
#endif
#if DT_NODE_HAS_STATUS(DT_PWM_INST(6), okay)
	pwm_dev[PWM_CH_06] = device_get_binding(PWM_LABEL(6));
#endif
#if DT_NODE_HAS_STATUS(DT_PWM_INST(7), okay)
	pwm_dev[PWM_CH_07] = device_get_binding(PWM_LABEL(7));
#endif
#if DT_NODE_HAS_STATUS(DT_PWM_INST(8), okay)
	pwm_dev[PWM_CH_08] = device_get_binding(PWM_LABEL(8));
#endif
#if defined(CONFIG_SOC_MEC172X_NLJ)
#if DT_NODE_HAS_STATUS(DT_PWM_INST(9), okay)
	pwm_dev[PWM_CH_09] = device_get_binding(PWM_LABEL(9));
#endif
#if DT_NODE_HAS_STATUS(DT_PWM_INST(10), okay)
	pwm_dev[PWM_CH_10] = device_get_binding(PWM_LABEL(10));
#endif
#if DT_NODE_HAS_STATUS(DT_PWM_INST(11), okay)
	pwm_dev[PWM_CH_11] = device_get_binding(PWM_LABEL(11));
#endif
#endif
}

static void init_tach_devices(void)
{
#if DT_NODE_HAS_STATUS(DT_TACH_INST(0), okay)
	tach_dev[TACH_CH_00] = device_get_binding(TACH_LABEL(0));
#endif
#if DT_NODE_HAS_STATUS(DT_TACH_INST(1), okay)
	tach_dev[TACH_CH_01] = device_get_binding(TACH_LABEL(1));
#endif
#if DT_NODE_HAS_STATUS(DT_TACH_INST(2), okay)
	tach_dev[TACH_CH_02] = device_get_binding(TACH_LABEL(2));
#endif
#if DT_NODE_HAS_STATUS(DT_TACH_INST(3), okay)
	tach_dev[TACH_CH_03] = device_get_binding(TACH_LABEL(3));
#endif
}

int fan_init(int size, struct fan_dev *fan_tbl)
{
	init_pwm_devices();
	init_tach_devices();

	if (size > ARRAY_SIZE(fan_table)) {
		return -ENOTSUP;
	}

	for (int idx = 0; idx < size; idx++) {

		if (!pwm_dev[fan_tbl[idx].pwm_ch]) {
			LOG_ERR("PWM ch %d not found", fan_tbl[idx].pwm_ch);
			return -ENODEV;
		}

		if (!tach_dev[fan_tbl[idx].tach_ch]) {
			LOG_ERR("Tach ch %d not found", fan_tbl[idx].tach_ch);
			return -ENODEV;
		}

		fan_table[idx].pwm_ch = fan_tbl[idx].pwm_ch;
		fan_table[idx].tach_ch = fan_tbl[idx].tach_ch;
	}

	return 0;
}

int fan_power_set(bool power_state)
{
	/* fan disable circuitry is active low. */
	return gpio_write_pin(FAN_PWR_DISABLE_N, power_state);
}

int fan_set_duty_cycle(enum fan_type fan_idx, uint8_t duty_cycle)
{
	int ret;

	if (fan_idx > ARRAY_SIZE(fan_table)) {
		return -ENOTSUP;
	}

	struct fan_dev *fan = &fan_table[fan_idx];

	if (fan->pwm_ch >= PWM_DEV_LIST_SIZE) {
		return -EINVAL;
	}

	const struct device *pwm = pwm_dev[fan->pwm_ch];

	if (!pwm) {
		return -ENODEV;
	}

	if (duty_cycle > MAX_DUTY_CYCLE) {
		duty_cycle = MAX_DUTY_CYCLE;
	}

	ret = pwm_pin_set_cycles(pwm, 0, PWM_FREQ_MULT * MAX_DUTY_CYCLE,
		PWM_FREQ_MULT * duty_cycle, 0);
	if (ret) {
		LOG_WRN("Fan setting error: %d", ret);
		return ret;
	}
	return 0;
}

int fan_read_rpm(enum fan_type fan_idx, uint16_t *rpm)
{
	int ret;
	struct sensor_value val;

	if (fan_idx > ARRAY_SIZE(fan_table)) {
		return -ENOTSUP;
	}

	struct fan_dev *fan = &fan_table[fan_idx];

	if (fan->tach_ch >= TACH_DEV_LIST_SIZE) {
		return -EINVAL;
	}

	const struct device *tach = tach_dev[fan->tach_ch];

	if (!tach) {
		return -ENODEV;
	}

	ret = sensor_sample_fetch_chan(tach, SENSOR_CHAN_RPM);
	if (ret) {
		return ret;
	}

	ret = sensor_channel_get(tach, SENSOR_CHAN_RPM, &val);
	if (ret) {
		return ret;
	}

	*rpm = (uint16_t) val.val1;

	return 0;
}
