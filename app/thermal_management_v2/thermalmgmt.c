/*
 * Copyright (c) 2020 Intel Corporation
 * Copyright (c) 2023 Silicom Connectivity Solutions, Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include "thermalmgmt.h"
#include "rpmfan.h"
#include "thermal_sensor.h"
#include "board_config.h"
#include "smc.h"
#include "sci.h"
#include "scicodes.h"
#include "peci_hub.h"
#include "pwrplane.h"
#include "smchost.h"
#include "espioob_mngr.h"
#include "memops.h"
#include "gpio_ec.h"
#include "task_handler.h"

LOG_MODULE_REGISTER(thermal, CONFIG_THERMAL_MGMT_LOG_LEVEL_V2);

/* PECI is prone to fail during initial boot to S0.
 * Hence allow 1 sec time before reading temperature from peci.
 * Till then report fail safe temperature of 28C.
 */
#define CPU_TEMP_ACCESS_DELAY_SEC		1U

/* In CS, temperature needs to be checked only once every 8 seconds.
 * This is because the CPU is anyway not running and any transaction
 * to query its temperature will wake the CPU.
 */
#define CPU_TEMP_CS_ACCESS_PERIOD_SEC		8U

#define CPU_FAIL_SAFE_TEMPERATURE		28U

/* CPU fail critical temperature value is 72C */
#define CPU_FAIL_CRITICAL_TEMPERATURE		72U

/* GPU fail critical temperature value is 72C */
#define GPU_FAIL_CRITICAL_TEMPERATURE		72U

/* To reduce polling frequency of PCH Temperature over OOB channel, read only
 * once out of 10 times.
 */
#define PCH_TEMP_POLLING_CNT_TIME_DIVISION	10U
#define PCH_TEMP_BUF_SIZE			6U

/*
 * EC Self control fan speed based on temperature info.
 *
 */
#define GET_FAN_SPEED_FOR_TEMP(temp)		(temp & (~0x7))

struct therm_sensor *therm_sensor_tbl;
struct fan_dev *fan_dev_tbl;
static uint8_t max_adc_sensors;
static uint8_t max_fan_dev;
static bool thermal_initialized;
static bool peci_initialized;
/* Using module property, before introducing straps handler */
static bool fan_override;
static bool bios_fan_override;
static bool ec_fan_control;
static uint16_t bios_fan_rpm;
static uint16_t fan_rpm[2];
static bool fan_rpm_change;
static int cpu_temp;

struct fan_lookup {
	int16_t temp;
	uint16_t rpm;
};

static const struct fan_lookup fan_lookup_tbl[] = {
	{15, 1500},
	{35, 2500},
	{42, 4000},
	{49, 5500},
	{56, 7000},
	{63, 8000},
	{70, 9000},
	{75, 10000}
};

static uint16_t get_fan_speed_for_temp(int16_t temp)
{
	static int16_t old_temp = 0;
	static uint16_t speed = 1000;
	int idx;

	if (temp < fan_lookup_tbl[0].temp) {
		old_temp = temp;
		speed = 0;
		return speed;
	} else if (temp >= fan_lookup_tbl[ARRAY_SIZE(fan_lookup_tbl)-1].temp) {
		old_temp = temp;
		speed = 10000;
		return speed;
	} else {
		if (temp > old_temp) {
			for (idx = 0; idx < ARRAY_SIZE(fan_lookup_tbl); idx++) {
				if ((temp >= fan_lookup_tbl[idx].temp)) {
					old_temp = fan_lookup_tbl[idx].temp;
					speed = fan_lookup_tbl[idx].rpm;
				}
			}
		} else {
			for (idx = ARRAY_SIZE(fan_lookup_tbl)-1; idx >= 0; idx--) {
				if (temp <= fan_lookup_tbl[idx].temp) {
					old_temp = fan_lookup_tbl[idx].temp;
					speed = fan_lookup_tbl[idx].rpm;
				}
			}
		}
	}

	return speed;
}

void host_update_crit_temp(uint8_t crit_temp)
{
	g_acpi_tbl.acpi_crit_temp = host_req[1] == 0 ?
		THERM_SHTDWN_THRSD : (host_req[1] + THERM_SHTDWN_EC_TOLERANCE);
}

/* Set default BSOD thermal thresholds */
struct therm_bsod_override_thrsd_acpi therm_bsod_override_acpi = {
	TEMP_OVERRIDE_ACPI, FAN_OVERRIDE_ACPI, false, false };

void init_dtt_threshold_limits(void)
{
}

void smc_update_dtt_threshold_limits(enum acpi_thrm_sens_idx acpi_sen_idx,
				     struct dtt_threshold thrd)
{
}

void sys_therm_sensor_trip(void)
{
}

void get_hw_peripherals_status(uint8_t *hw_peripherals_sts)
{
	uint8_t idx = 0x0;

	/* First byte contains fan status whereas second byte contains therm
	 * sensors status. Each bit in the byte signifies presence of the HW.
	 * EC and BIOS agreed to align with below fan and sensors indexing.
	 * Fan index:
	 *	Bit0: CPU fan
	 *	Bit1: Rear fan
	 *	Bit2: Graphics fan
	 *	Bit3: PCH fan
	 *	Bit4:7: Reserved
	 * Thermal sensor index:
	 *	Bit0: PCH sensor
	 *	Bit1: Skin sensor
	 *	Bit2: Ambient sensor
	 *	Bit3: VR sensor
	 *	Bit4: DDR sensor
	 *	Bit5:7: Reserved
	 * For instance, in the first byte, if Bit0 & Bit2 are set, means the
	 * board has CPU & graphics fans.
	 */

	/* Update fans status */
	for (idx = 0; idx < max_fan_dev; idx++) {
		if (fan_dev_tbl[idx].rpm2pwm_ch != RPM2PWM_CH_UNDEF) {
			hw_peripherals_sts[0] |= BIT(idx);
		}
	}

	/* Update thermal sensors status */
	for (idx = 0; idx < max_adc_sensors; idx++) {
		hw_peripherals_sts[1] |= BIT(therm_sensor_tbl[idx].acpi_loc);
	}
}

static void init_fans(void)
{
	int ret;
	int level;

	/* Initialize override */
	level = gpio_read_pin(THERM_STRAP);
	if (level < 0) {
		LOG_ERR("Fail to read thermal strap");
	} else {
		fan_override = !level;
		LOG_WRN("Fan HW override enable: %d", fan_override);
	}

#ifdef CONFIG_THERMAL_FAN_OVERRIDE
	fan_override = true;
	LOG_WRN("Fan SW override enable: %d", fan_override);
#endif

	/* Get the list of fan devices supported for the board */
	board_fan_dev_tbl_init(&max_fan_dev, &fan_dev_tbl);
	LOG_DBG("Board has %d fans", max_fan_dev);

	ret = fan_init(max_fan_dev, fan_dev_tbl);
	if (ret) {
		LOG_ERR("Failed to init fan");
	}

	fan_rpm[FAN_LEFT] = CONFIG_THERMAL_FAN_OVERRIDE_VALUE_V2;
#ifdef CONFIG_BOARD_MEC172X_AZBEACH
	fan_rpm[FAN_RIGHT] = CONFIG_THERMAL_FAN_OVERRIDE_VALUE_V2;
#endif
	fan_rpm_change = 1;

}

static void init_therm_sensors(void)
{
	uint32_t adc_ch_bits = 0;		/* MEC172X has > 8 sensors */

	board_therm_sensor_tbl_init(&max_adc_sensors, &therm_sensor_tbl);

	LOG_INF("Num of thermal sensors: %d", max_adc_sensors);

	for (uint8_t idx = 0; idx < max_adc_sensors; idx++) {
		LOG_INF("adc ch: %d, for acpi sen: %d",
			therm_sensor_tbl[idx].adc_ch,
			therm_sensor_tbl[idx].acpi_loc);

		adc_ch_bits |= BIT(therm_sensor_tbl[idx].adc_ch);
	}

	LOG_INF("adc ch sensors bit: %x", adc_ch_bits);
	if (thermal_sensors_init(adc_ch_bits)) {
		LOG_WRN("Thermal Sensor module init failed!");
	} else {
		thermal_initialized = true;
	}

	init_dtt_threshold_limits();
}

bool is_fan_controlled_by_host(void)
{
	if (bios_fan_override) {
		return 1;
	}

	if (!is_system_in_acpi_mode()) {
		LOG_DBG("Fan control is over-ridden when not in ACPI mode.");
		return 0;
	}

	return 1;
}

bool is_fan_controlled_by_ec(void)
{
	if (ec_fan_control) {
		return 1;
	}

	return 0;
}

void host_set_bios_fan_override(bool en, uint16_t speed)
{
	LOG_INF("BIOS set over ride control to %d", en);
	bios_fan_override = en;
	bios_fan_rpm = speed;
}

void host_update_fan_speed(enum fan_type idx, uint16_t rpm)
{
	if (!is_fan_controlled_by_host()) {
		LOG_INF("Fan is not in host control.");
		return;
	}
	if (idx == FAN_LEFT && bios_fan_override) {
		fan_rpm[FAN_LEFT] = bios_fan_rpm;
		fan_rpm_change = 1;
		return;
	}
	if (idx < max_fan_dev) {
		fan_rpm[idx] = rpm;
		fan_rpm_change = 1;
		LOG_INF("Updating fan speed to %d", rpm);
	} else {
		LOG_WRN("Invalid fan index");
	}
}

static void manage_fan(void)
{
	/* Disable power to fan in S5/4/3 and in CS,
	 * else continue with fan management.
	 */
	if ((pwrseq_system_state() != SYSTEM_S0_STATE) ||
		(smchost_is_system_in_cs())) {
		fan_power_set(false);
		return;
	}
	/* Enable power to fan when system is in S0 and not in CS */
	fan_power_set(true);

	if (!is_fan_controlled_by_host() || is_fan_controlled_by_ec()) {
		/* EC Self control fan based on CPU thermal info */
		uint16_t rpm = get_fan_speed_for_temp(adc_temp_val[3]);
		LOG_INF("%s: board CPU temp: %d, setting rpm to %d", __func__,  adc_temp_val[3], rpm);
		if (fan_rpm[FAN_LEFT] != rpm) {
			fan_rpm[FAN_LEFT] = rpm;
			fan_rpm_change = 1;
		}
		if (fan_rpm[FAN_RIGHT] != rpm) {
			fan_rpm[FAN_RIGHT] = rpm;
			fan_rpm_change = 1;
		}
	}

	/* HW/KConfig override takes precedence over every control method
	 * This is mostly used for PO entry on PO team request
	 */
	if (fan_override) {
		fan_rpm[FAN_LEFT] = CONFIG_THERMAL_FAN_OVERRIDE_VALUE_V2;
		fan_rpm[FAN_RIGHT] = CONFIG_THERMAL_FAN_OVERRIDE_VALUE_V2;
		fan_rpm_change = 1;
	}

	if (fan_rpm_change) {
		fan_rpm_change = 0;

		for (uint8_t idx = 0; idx < max_fan_dev; idx++) {
			fan_set_rpm(idx, fan_rpm[idx]);
		}
	}

	for (uint8_t idx = 0; idx < max_fan_dev; idx++) {
		uint16_t rpm;

		fan_read_rpm(idx, &rpm);
		smc_update_fan_tach(idx, rpm);
//		smc_update_fan_rpm2pwm(idx, fan_rpm[idx]); /* store fan rpm for hwmon */
	}

	/* EC assumes OS is hung/BSOD occurred and takes override actions
	 * if current CPU temperature crossed above and fan running below
	 * override thresholds defined by the BIOS.
	 */
	if (therm_bsod_override_acpi.is_bsod_setting_en) {
		if ((cpu_temp > therm_bsod_override_acpi.temp_bsod_override) &&
			(g_acpi_tbl.acpi_pwm_end_val <
			therm_bsod_override_acpi.fan_bsod_override)) {
			fan_set_rpm(FAN_LEFT,
				therm_bsod_override_acpi.fan_bsod_override);
			therm_bsod_override_acpi.is_bsod_temp_crossed = true;
		} else if ((cpu_temp < TEMP_BSOD_FAN_OFF) &&
				therm_bsod_override_acpi.is_bsod_temp_crossed) {
			fan_set_rpm(FAN_LEFT, 0);
			therm_bsod_override_acpi.is_bsod_temp_crossed = false;
		}
	}
}

static void manage_thermal_sensors(void)
{
	/* Do not attempt to update if no sensors were detected */
	if (!thermal_initialized) {
		return;
	}

	thermal_sensors_update();

	for (uint8_t idx = 0; idx < max_adc_sensors; idx++) {
		smc_update_thermal_sensor(
			therm_sensor_tbl[idx].acpi_loc,
#if CONFIG_BOARD_MEC172X_AZBEACH
			adc_temp_val[idx]);
#else
			adc_temp_val[therm_sensor_tbl[idx].adc_ch]);
#endif
	}

	sys_therm_sensor_trip();
}

K_TIMER_DEFINE(peci_delay_timer, NULL, NULL);

void peci_start_delay_timer(void)
{
	/* start a one-shot timer for prescribed seconds */
	k_timer_start(&peci_delay_timer, K_SECONDS(CPU_TEMP_ACCESS_DELAY_SEC),
		      K_NO_WAIT);
	smc_update_cpu_temperature(CPU_FAIL_SAFE_TEMPERATURE);
	LOG_DBG("PECI delay timer started");
}

void host_set_bios_bsod_override(uint8_t temp_bsod_override_val,
	uint8_t fan_bsod_override_val)
{
	therm_bsod_override_acpi.is_bsod_setting_en = true;
	therm_bsod_override_acpi.temp_bsod_override = temp_bsod_override_val;
	therm_bsod_override_acpi.fan_bsod_override = fan_bsod_override_val;
}

static void manage_cpu_thermal(void)
{
	int temp, ret, temp_change;
	static int prev_notify_temp;

	/* Manage CPU thermal only in S0 state */
	if (!peci_initialized || k_timer_remaining_get(&peci_delay_timer) ||
	    (pwrseq_system_state() != SYSTEM_S0_STATE)) {
		return;
	}

	/* Read CPU temperature using peci */
	ret = peci_get_temp(CPU, &temp);
	if (ret) {
		LOG_ERR("Failed to get cpu temperature, ret-%x", ret);
		temp = CPU_FAIL_CRITICAL_TEMPERATURE;
	}

	cpu_temp = temp;

	/* Update the CPU temperature to acpi offset */
	smc_update_cpu_temperature(temp);
	LOG_INF("%s: Cpu Temp=%d", __func__, temp);

	/* Trigger shutdown if temp crosses above critical threshold */
	if (cpu_temp >= g_acpi_tbl.acpi_crit_temp) {
		LOG_DBG("EC thermal shutdown");
		therm_shutdown();
		return;
	}

	/* Read GPU temperature using peci if the GPU is in an active state */
	if ((gpio_read_pin(DG2_PRESENT) == HIGH) &&
	    (gpio_read_pin(PEG_RTD3_COLD_MOD_SW_R) == HIGH)) {
		ret = peci_get_temp(GPU, &temp);
		if (ret) {
			LOG_ERR("Failed to get GPU temperature, ret-%x", ret);
			temp = GPU_FAIL_CRITICAL_TEMPERATURE;
		}

		/* Update the GPU temperature to acpi offset */
		smc_update_gpu_temperature(temp);
		LOG_WRN("%s: GPU Temp=%d", __func__, temp);
	}

	/* Check temperature change and alert OS */
	temp_change = cpu_temp - prev_notify_temp;

	if (temp_change < 0) {
		temp_change = -temp_change;
	}

	if (temp_change > CPU_TEMP_ALERT_DELTA) {
		enqueue_sci(SCI_THERMAL);
		prev_notify_temp = cpu_temp;
	}
}

static void manage_pch_temperature(void)
{
	static uint8_t temp_poll_cnt = PCH_TEMP_POLLING_CNT_TIME_DIVISION;

	if (pwrseq_system_state() != SYSTEM_S0_STATE) {
		return;
	}

	/* Do not fetch PCH temperature in CS */
	if (smchost_is_system_in_cs()) {
		return;
	}
	/* To slow down polling on PCH Temperature, read only once per/sec. */
	if (temp_poll_cnt--) {
		return;
	}

	temp_poll_cnt = PCH_TEMP_POLLING_CNT_TIME_DIVISION;

	uint8_t pchtemp[PCH_TEMP_BUF_SIZE] = {
		OOB_DST_ADDR(OOB_MASTER_ADDR_HW),
		OOB_CMD_CODE_HW_TEMP,
		OOB_BYTE_CNT_HW_REQ_MSG,
		OOB_SRC_ADDR(OOB_SLAVE_ADDR_EC)
	};

	struct espi_oob_packet req = {.buf = pchtemp, .len = 4};
	struct espi_oob_packet resp = {.buf = pchtemp, .len = sizeof(pchtemp)};

	if (!oob_send_sync(&req, &resp, OOB_MSG_SYNC_WAIT_TIME_DFLT)) {
		struct oob_msg_str *msg = (struct oob_msg_str *) resp.buf;

		LOG_DBG("PCH Temp = %d", msg->payload[0]);
		smc_update_pch_dts_temperature(msg->payload[0]);
	}
}

void thermalmgmt_handle_cs_exit(void)
{
	LOG_DBG("CS Exit: Wake thermal thread from sleep");
	/*In CS mode, thread will be in sleep and may take
	 * up to 'CPU_TEMP_CS_ACCESS_PERIOD_SEC' sec to
	 * wake up. Hence, this trigger to force wake up
	 * & avoid delay
	 */
	wake_task((const char *)THRML_MGMT_TASK_NAME);
}

void thermalmgmt_thread(void *p1, void *p2, void *p3)
{
	uint32_t normal_period = *(uint32_t *)p1;
	g_acpi_tbl.acpi_crit_temp = THERM_SHTDWN_THRSD;
	int err;

	init_fans();
	init_therm_sensors();
	err = peci_init();
	if (!err) {
		peci_initialized = true;
	}

#ifdef CONFIG_EC_FAN_CONTROL
	ec_fan_control = 1;
#endif
	
	while (true) {
		/* Each thread is aware of CS
		 * Thread uses different sleep time during CS
		 * This required to enter Zephyr-LPM
		 */
		if (smchost_is_system_in_cs()) {
			k_sleep(K_SECONDS(CPU_TEMP_CS_ACCESS_PERIOD_SEC));
		} else {
			k_msleep(normal_period);
		}

		manage_fan();

		/* To achieve infinite C10 residency in connected standby
		 * and ps_on, EC should not send peci cpu & pch temperature
		 * read commands in CS to avoid SOC wake.
		 */
		if (smchost_is_system_in_cs()) {
			continue;
		}

		manage_thermal_sensors();
		manage_cpu_thermal();
		manage_pch_temperature();
	}
}
