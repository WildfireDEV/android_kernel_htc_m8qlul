/* Copyright (c) 2013-2014, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
#define pr_fmt(fmt)	"[BATT] CHG: %s: " fmt, __func__

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/err.h>
#include <linux/spmi.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/interrupt.h>
#include <linux/power_supply.h>
#include <linux/qpnp/qpnp-adc.h>
#include <linux/alarmtimer.h>
#include <linux/bitops.h>

#define CREATE_MASK(NUM_BITS, POS) \
	((unsigned char) (((1 << (NUM_BITS)) - 1) << (POS)))
#define LBC_MASK(MSB_BIT, LSB_BIT) \
	CREATE_MASK(MSB_BIT - LSB_BIT + 1, LSB_BIT)

#define INT_RT_STS_REG				0x10
#define FAST_CHG_ON_IRQ                         BIT(5)
#define OVERTEMP_ON_IRQ				BIT(4)
#define BAT_TEMP_OK_IRQ                         BIT(1)
#define BATT_PRES_IRQ                           BIT(0)

#define USB_PTH_STS_REG				0x09
#define USB_IN_VALID_MASK			LBC_MASK(7, 6)
#define USB_INT_RT_STS				0x10
#define USB_OVP_CTL_REG				0x42
#define USB_SUSP_REG				0x47
#define USB_SUSPEND_BIT				BIT(0)
#define USB_ENUM_TIMER_STOP_REG		0x4E
#define USB_ENUM_TIMER_REG			0x4F

#define CHG_OPTION_REG				0x08
#define CHG_OPTION_MASK				BIT(7)
#define CHG_STATUS_REG				0x09
#define CHG_VDD_LOOP_BIT			BIT(1)
#define CHG_INT_RT_STS				0x10
#define CHG_VDD_MAX_REG				0x40
#define CHG_VDD_SAFE_REG			0x41
#define CHG_IBAT_MAX_REG			0x44
#define CHG_IBAT_SAFE_REG			0x45
#define CHG_VIN_MIN_REG				0x47
#define CHG_CTRL_REG				0x49
#define CHG_ENABLE				BIT(7)
#define CHG_FORCE_BATT_ON			BIT(0)
#define CHG_EN_MASK				(BIT(7) | BIT(0))
#define CHG_FAILED_REG				0x4A
#define CHG_FAILED_BIT				BIT(7)
#define CHG_VBAT_WEAK_REG			0x52
#define CHG_IBATTERM_EN_REG			0x5B
#define CHG_USB_ENUM_T_STOP_REG			0x4E
#define CHG_TCHG_MAX_EN_REG			0x60
#define CHG_TCHG_MAX_EN_BIT			BIT(7)
#define CHG_TCHG_MAX_MASK			LBC_MASK(6, 0)
#define CHG_TCHG_MAX_REG			0x61
#define CHG_CHG_WDOG_TIME_REG		0x62
#define CHG_WDOG_EN_REG				0x65
#define CHG_FSM_STATE_REG           0xE7
#define CHG_PERPH_RESET_CTRL3_REG		0xDA
#define CHG_COMP_OVR1				0xEE
#define CHG_VBAT_DET_OVR_MASK			LBC_MASK(1, 0)
#define OVERRIDE_0				0x2
#define OVERRIDE_NONE				0x0

#define BAT_IF_PRES_STATUS_REG			0x08
#define BATT_PRES_MASK				BIT(7)
#define BAT_IF_TEMP_STATUS_REG			0x09
#define BATT_TEMP_HOT_MASK			BIT(6)
#define BATT_TEMP_COLD_MASK			LBC_MASK(7, 6)
#define BATT_TEMP_OK_MASK			BIT(7)
#define BAT_IF_INT_RT_STS_REG			0x10
#define BAT_IF_VREF_BAT_THM_CTRL_REG		0x4A
#define VREF_BATT_THERM_FORCE_ON		LBC_MASK(7, 6)
#define VREF_BAT_THM_ENABLED_FSM		BIT(7)
#define BAT_IF_BPD_CTRL_REG			0x48
#define BATT_BPD_CTRL_SEL_MASK			LBC_MASK(1, 0)
#define BATT_BPD_OFFMODE_EN			BIT(3)
#define BATT_THM_EN				BIT(1)
#define BATT_ID_EN				BIT(0)
#define BAT_IF_BTC_CTRL				0x49
#define BTC_COMP_EN_MASK			BIT(7)
#define BTC_COLD_MASK				BIT(1)
#define BTC_HOT_MASK				BIT(0)

#define MISC_REV2_REG				0x01
#define MISC_BOOT_DONE_REG			0x42
#define MISC_BOOT_DONE				BIT(7)
#define MISC_TRIM3_REG				0xF3
#define MISC_TRIM3_VDD_MASK			LBC_MASK(5, 4)
#define MISC_TRIM4_REG				0xF4
#define MISC_TRIM4_VDD_MASK			BIT(4)

#define PERP_SUBTYPE_REG			0x05
#define SEC_ACCESS                              0xD0

#define LBC_CHGR_SUBTYPE			0x15
#define LBC_BAT_IF_SUBTYPE			0x16
#define LBC_USB_PTH_SUBTYPE			0x17
#define LBC_MISC_SUBTYPE			0x18

#define QPNP_CHG_I_MAX_MIN_90                   90

#define VDD_TRIM_SUPPORTED			BIT(0)

#define QPNP_CHARGER_DEV_NAME	"qcom,qpnp-linear-charger"


struct qpnp_lbc_irq {
	int		irq;
	unsigned long	disabled;
	bool            is_wake;
};

enum {
	USBIN_VALID = 0,
	USB_OVER_TEMP,
	USB_CHG_GONE,
	BATT_PRES,
	BATT_TEMPOK,
	CHG_DONE,
	CHG_FAILED,
	CHG_FAST_CHG,
	CHG_VBAT_DET_LO,
	MAX_IRQS,
};

enum {
	USER	= BIT(0),
	THERMAL = BIT(1),
	CURRENT = BIT(2),
	SOC	= BIT(3),
};

enum bpd_type {
	BPD_TYPE_BAT_ID,
	BPD_TYPE_BAT_THM,
	BPD_TYPE_BAT_THM_BAT_ID,
};

static const char * const bpd_label[] = {
	[BPD_TYPE_BAT_ID] = "bpd_id",
	[BPD_TYPE_BAT_THM] = "bpd_thm",
	[BPD_TYPE_BAT_THM_BAT_ID] = "bpd_thm_id",
};

enum btc_type {
	HOT_THD_25_PCT = 25,
	HOT_THD_35_PCT = 35,
	COLD_THD_70_PCT = 70,
	COLD_THD_80_PCT = 80,
};

static u8 btc_value[] = {
	[HOT_THD_25_PCT] = 0x0,
	[HOT_THD_35_PCT] = BIT(0),
	[COLD_THD_70_PCT] = 0x0,
	[COLD_THD_80_PCT] = BIT(1),
};

static inline int get_bpd(const char *name)
{
	int i = 0;
	for (i = 0; i < ARRAY_SIZE(bpd_label); i++) {
		if (strcmp(bpd_label[i], name) == 0)
			return i;
	}
	return -EINVAL;
}

#ifndef CONFIG_QPNP_LINEAR_CHARGER_SMB1360
static enum power_supply_property msm_batt_power_props[] = {
	POWER_SUPPLY_PROP_CHARGING_ENABLED,
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_CHARGE_TYPE,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN,
	POWER_SUPPLY_PROP_VOLTAGE_MIN_DESIGN,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_COOL_TEMP,
	POWER_SUPPLY_PROP_WARM_TEMP,
	POWER_SUPPLY_PROP_SYSTEM_TEMP_LEVEL,
};

static char *pm_batt_supplied_to[] = {
	"bms",
};
#endif

struct vddtrim_map {
	int			trim_uv;
	int			trim_val;
};

#define TRIM_CENTER			4
#define MAX_VDD_EA_TRIM_CFG		8
#define VDD_TRIM3_MASK			LBC_MASK(2, 1)
#define VDD_TRIM3_SHIFT			3
#define VDD_TRIM4_MASK			BIT(0)
#define VDD_TRIM4_SHIFT			4
#define AVG(VAL1, VAL2)			((VAL1 + VAL2) / 2)

struct vddtrim_map vddtrim_map[] = {
	{36700,		0x00},
	{28000,		0x01},
	{19800,		0x02},
	{10760,		0x03},
	{0,		0x04},
	{-8500,		0x05},
	{-16800,	0x06},
	{-25440,	0x07},
};

struct qpnp_lbc_chip {
	struct device			*dev;
	struct spmi_device		*spmi;
	u16				chgr_base;
	u16				bat_if_base;
	u16				usb_chgpth_base;
	u16				misc_base;
	bool				bat_is_cool;
	bool				bat_is_warm;
	bool				chg_done;
	bool				usb_present;
	bool				batt_present;
	bool				cfg_charging_disabled;
	bool				cfg_btc_disabled;
	bool				cfg_use_fake_battery;
	bool				fastchg_on;
	bool				cfg_use_external_charger;
	unsigned int			cfg_warm_bat_chg_ma;
	unsigned int			cfg_cool_bat_chg_ma;
	unsigned int			cfg_safe_voltage_mv;
	unsigned int			cfg_max_voltage_mv;
	unsigned int			cfg_min_voltage_mv;
	unsigned int			cfg_charger_detect_eoc;
	unsigned int			cfg_disable_vbatdet_based_recharge;
	unsigned int			cfg_batt_weak_voltage_uv;
	unsigned int			cfg_warm_bat_mv;
	unsigned int			cfg_cool_bat_mv;
	unsigned int			cfg_hot_batt_p;
	unsigned int			cfg_cold_batt_p;
	unsigned int			cfg_thermal_levels;
	unsigned int			therm_lvl_sel;
	unsigned int			*thermal_mitigation;
	unsigned int			cfg_safe_current;
	unsigned int			cfg_tchg_mins;
	unsigned int			chg_failed_count;
	unsigned int			cfg_disable_follow_on_reset;
	unsigned int			supported_feature_flag;
	int				cfg_bpd_detection;
	int				cfg_warm_bat_decidegc;
	int				cfg_cool_bat_decidegc;
	int				fake_battery_soc;
	int				cfg_soc_resume_limit;
	int				cfg_float_charge;
	int				charger_disabled;
	int				prev_max_ma;
	int				usb_psy_ma;
	int				delta_vddmax_uv;
	int				init_trim_uv;
	struct alarm			vddtrim_alarm;
	struct work_struct		vddtrim_work;
	struct qpnp_lbc_irq		irqs[MAX_IRQS];
	struct mutex			jeita_configure_lock;
	struct mutex			chg_enable_lock;
	spinlock_t			ibat_change_lock;
	spinlock_t			hw_access_lock;
	spinlock_t			irq_lock;
	struct power_supply		*usb_psy;
	struct power_supply		*bms_psy;
	struct power_supply		batt_psy;
	struct qpnp_adc_tm_btm_param	adc_param;
	struct qpnp_vadc_chip		*vadc_dev;
	struct qpnp_adc_tm_chip		*adc_tm_dev;
};

#ifdef CONFIG_QPNP_LINEAR_CHARGER_SMB1360
static struct qpnp_lbc_chip *the_chip;

static void dump_reg(void);
#endif

#ifndef CONFIG_QPNP_LINEAR_CHARGER_SMB1360
static void qpnp_lbc_enable_irq(struct qpnp_lbc_chip *chip,
					struct qpnp_lbc_irq *irq)
{
	unsigned long flags;

	spin_lock_irqsave(&chip->irq_lock, flags);
	if (__test_and_clear_bit(0, &irq->disabled)) {
		pr_debug("number = %d\n", irq->irq);
		enable_irq(irq->irq);
		if (irq->is_wake)
			enable_irq_wake(irq->irq);
	}
	spin_unlock_irqrestore(&chip->irq_lock, flags);
}

static void qpnp_lbc_disable_irq(struct qpnp_lbc_chip *chip,
					struct qpnp_lbc_irq *irq)
{
	unsigned long flags;

	spin_lock_irqsave(&chip->irq_lock, flags);
	if (!__test_and_set_bit(0, &irq->disabled)) {
		pr_debug("number = %d\n", irq->irq);
		disable_irq_nosync(irq->irq);
		if (irq->is_wake)
			disable_irq_wake(irq->irq);
	}
	spin_unlock_irqrestore(&chip->irq_lock, flags);
}
#endif

static int __qpnp_lbc_read(struct spmi_device *spmi, u16 base,
			u8 *val, int count)
{
	int rc = 0;

	rc = spmi_ext_register_readl(spmi->ctrl, spmi->sid, base, val, count);
	if (rc)
		pr_err("SPMI read failed base=0x%02x sid=0x%02x rc=%d\n",
				base, spmi->sid, rc);

	return rc;
}

static int __qpnp_lbc_write(struct spmi_device *spmi, u16 base,
			u8 *val, int count)
{
	int rc;

	rc = spmi_ext_register_writel(spmi->ctrl, spmi->sid, base, val,
					count);
	if (rc)
		pr_err("SPMI write failed base=0x%02x sid=0x%02x rc=%d\n",
				base, spmi->sid, rc);

	return rc;
}

static int __qpnp_lbc_secure_write(struct spmi_device *spmi, u16 base,
				u16 offset, u8 *val, int count)
{
	int rc;
	u8 reg_val;

	reg_val = 0xA5;
	rc = __qpnp_lbc_write(spmi, base + SEC_ACCESS, &reg_val, 1);
	if (rc) {
		pr_err("SPMI read failed base=0x%02x sid=0x%02x rc=%d\n",
				base + SEC_ACCESS, spmi->sid, rc);
		return rc;
	}

	rc = __qpnp_lbc_write(spmi, base + offset, val, 1);
	if (rc)
		pr_err("SPMI write failed base=0x%02x sid=0x%02x rc=%d\n",
				base + SEC_ACCESS, spmi->sid, rc);

	return rc;
}

static int qpnp_lbc_read(struct qpnp_lbc_chip *chip, u16 base,
			u8 *val, int count)
{
	int rc = 0;
	struct spmi_device *spmi = chip->spmi;
	unsigned long flags;

	if (base == 0) {
		pr_err("base cannot be zero base=0x%02x sid=0x%02x rc=%d\n",
				base, spmi->sid, rc);
		return -EINVAL;
	}

	spin_lock_irqsave(&chip->hw_access_lock, flags);
	rc = __qpnp_lbc_read(spmi, base, val, count);
	spin_unlock_irqrestore(&chip->hw_access_lock, flags);

	return rc;
}

static int qpnp_lbc_write(struct qpnp_lbc_chip *chip, u16 base,
			u8 *val, int count)
{
	int rc = 0;
	struct spmi_device *spmi = chip->spmi;
	unsigned long flags;

	if (base == 0) {
		pr_err("base cannot be zero base=0x%02x sid=0x%02x rc=%d\n",
				base, spmi->sid, rc);
		return -EINVAL;
	}

	spin_lock_irqsave(&chip->hw_access_lock, flags);
	rc = __qpnp_lbc_write(spmi, base, val, count);
	spin_unlock_irqrestore(&chip->hw_access_lock, flags);

	return rc;
}

static int qpnp_lbc_masked_write(struct qpnp_lbc_chip *chip, u16 base,
				u8 mask, u8 val)
{
	int rc;
	u8 reg_val;
	struct spmi_device *spmi = chip->spmi;
	unsigned long flags;

	spin_lock_irqsave(&chip->hw_access_lock, flags);
	rc = __qpnp_lbc_read(spmi, base, &reg_val, 1);
	if (rc) {
		pr_err("spmi read failed: addr=%03X, rc=%d\n", base, rc);
		goto out;
	}
	pr_debug("addr = 0x%x read 0x%x\n", base, reg_val);

	reg_val &= ~mask;
	reg_val |= val & mask;

	pr_debug("writing to base=%x val=%x\n", base, reg_val);

	rc = __qpnp_lbc_write(spmi, base, &reg_val, 1);
	if (rc)
		pr_err("spmi write failed: addr=%03X, rc=%d\n", base, rc);

out:
	spin_unlock_irqrestore(&chip->hw_access_lock, flags);
	return rc;
}

static int __qpnp_lbc_secure_masked_write(struct spmi_device *spmi, u16 base,
				u16 offset, u8 mask, u8 val)
{
	int rc;
	u8 reg_val, reg_val1;

	rc = __qpnp_lbc_read(spmi, base + offset, &reg_val, 1);
	if (rc) {
		pr_err("spmi read failed: addr=%03X, rc=%d\n", base, rc);
		return rc;
	}
	pr_debug("addr = 0x%x read 0x%x\n", base, reg_val);

	reg_val &= ~mask;
	reg_val |= val & mask;
	pr_debug("writing to base=%x val=%x\n", base, reg_val);

	reg_val1 = 0xA5;
	rc = __qpnp_lbc_write(spmi, base + SEC_ACCESS, &reg_val1, 1);
	if (rc) {
		pr_err("SPMI read failed base=0x%02x sid=0x%02x rc=%d\n",
				base + SEC_ACCESS, spmi->sid, rc);
		return rc;
	}

	rc = __qpnp_lbc_write(spmi, base + offset, &reg_val, 1);
	if (rc) {
		pr_err("SPMI write failed base=0x%02x sid=0x%02x rc=%d\n",
				base + offset, spmi->sid, rc);
		return rc;
	}

	return rc;
}

static int qpnp_lbc_get_trim_voltage(u8 trim_reg)
{
	int i;

	for (i = 0; i < MAX_VDD_EA_TRIM_CFG; i++)
		if (trim_reg == vddtrim_map[i].trim_val)
			return vddtrim_map[i].trim_uv;

	pr_err("Invalid trim reg reg_val=%x\n", trim_reg);
	return -EINVAL;
}

static u8 qpnp_lbc_get_trim_val(struct qpnp_lbc_chip *chip)
{
	int i, sign;
	int delta_uv;

	sign = (chip->delta_vddmax_uv >= 0) ? -1 : 1;

	switch (sign) {
	case -1:
		for (i = TRIM_CENTER; i >= 0; i--) {
			if (vddtrim_map[i].trim_uv > chip->delta_vddmax_uv) {
				delta_uv = AVG(vddtrim_map[i].trim_uv,
						vddtrim_map[i + 1].trim_uv);
				if (chip->delta_vddmax_uv >= delta_uv)
					return vddtrim_map[i].trim_val;
				else
					return vddtrim_map[i + 1].trim_val;
			}
		}
		break;
	case 1:
		for (i = TRIM_CENTER; i <= 7; i++) {
			if (vddtrim_map[i].trim_uv < chip->delta_vddmax_uv) {
				delta_uv = AVG(vddtrim_map[i].trim_uv,
						vddtrim_map[i - 1].trim_uv);
				if (chip->delta_vddmax_uv >= delta_uv)
					return vddtrim_map[i - 1].trim_val;
				else
					return vddtrim_map[i].trim_val;
			}
		}
		break;
	}

	return vddtrim_map[i].trim_val;
}

static int qpnp_lbc_is_usb_chg_plugged_in(struct qpnp_lbc_chip *chip)
{
	u8 usbin_valid_rt_sts;
	int rc;

	rc = qpnp_lbc_read(chip, chip->usb_chgpth_base + USB_PTH_STS_REG,
				&usbin_valid_rt_sts, 1);
	if (rc) {
		pr_err("spmi read failed: addr=%03X, rc=%d\n",
				chip->usb_chgpth_base + USB_PTH_STS_REG, rc);
		return rc;
	}

	pr_debug("usb_path_sts 0x%x\n", usbin_valid_rt_sts);

	return (usbin_valid_rt_sts & USB_IN_VALID_MASK) ? 1 : 0;
}

static int qpnp_lbc_charger_enable(struct qpnp_lbc_chip *chip, int reason,
					int enable)
{
	int disabled = chip->charger_disabled;
	u8 reg_val;
	int rc = 0;

	pr_debug("reason=%d requested_enable=%d disabled_status=%d\n",
					reason, enable, disabled);
	if (enable)
		disabled &= ~reason;
	else
		disabled |= reason;

	if (!!chip->charger_disabled == !!disabled)
		goto skip;

	reg_val = !!disabled ? CHG_FORCE_BATT_ON : CHG_ENABLE;
	rc = qpnp_lbc_masked_write(chip, chip->chgr_base + CHG_CTRL_REG,
				CHG_EN_MASK, reg_val);
	if (rc) {
		pr_err("Failed to %s charger rc=%d\n",
				reg_val ? "enable" : "disable", rc);
		return rc;
	}
skip:
	chip->charger_disabled = disabled;
	return rc;
}

#ifndef CONFIG_QPNP_LINEAR_CHARGER_SMB1360
static int qpnp_lbc_is_batt_present(struct qpnp_lbc_chip *chip)
{
	u8 batt_pres_rt_sts;
	int rc;

	rc = qpnp_lbc_read(chip, chip->bat_if_base + INT_RT_STS_REG,
				&batt_pres_rt_sts, 1);
	if (rc) {
		pr_err("spmi read failed: addr=%03X, rc=%d\n",
				chip->bat_if_base + INT_RT_STS_REG, rc);
		return rc;
	}

	return (batt_pres_rt_sts & BATT_PRES_IRQ) ? 1 : 0;
}
#endif

static int qpnp_lbc_bat_if_configure_btc(struct qpnp_lbc_chip *chip)
{
	u8 btc_cfg = 0, mask = 0, rc;

	
	if (!chip->bat_if_base)
		return 0;

	if ((chip->cfg_hot_batt_p == HOT_THD_25_PCT)
			|| (chip->cfg_hot_batt_p == HOT_THD_35_PCT)) {
		btc_cfg |= btc_value[chip->cfg_hot_batt_p];
		mask |= BTC_HOT_MASK;
	}

	if ((chip->cfg_cold_batt_p == COLD_THD_70_PCT) ||
			(chip->cfg_cold_batt_p == COLD_THD_80_PCT)) {
		btc_cfg |= btc_value[chip->cfg_cold_batt_p];
		mask |= BTC_COLD_MASK;
	}

#ifndef CONFIG_QPNP_LINEAR_CHARGER_SMB1360
	if (!chip->cfg_btc_disabled) {
		mask |= BTC_COMP_EN_MASK;
		btc_cfg |= BTC_COMP_EN_MASK;
	}
#else
	
	if (chip->cfg_btc_disabled) {
		
		mask |= BTC_COMP_EN_MASK;
	} else {
		mask |= BTC_COMP_EN_MASK;
		btc_cfg |= BTC_COMP_EN_MASK;
	}
#endif

	pr_debug("BTC configuration mask=%x\n", btc_cfg);

	rc = qpnp_lbc_masked_write(chip,
			chip->bat_if_base + BAT_IF_BTC_CTRL,
			mask, btc_cfg);
	if (rc)
		pr_err("Failed to configure BTC rc=%d\n", rc);

	return rc;
}

#define QPNP_LBC_VBATWEAK_MIN_UV        3000000
#define QPNP_LBC_VBATWEAK_MAX_UV        3581250
#define QPNP_LBC_VBATWEAK_STEP_UV       18750
static int qpnp_lbc_vbatweak_set(struct qpnp_lbc_chip *chip, int voltage)
{
	u8 reg_val;
	int rc;

	if (voltage < QPNP_LBC_VBATWEAK_MIN_UV ||
			voltage > QPNP_LBC_VBATWEAK_MAX_UV) {
		rc = -EINVAL;
	} else {
		reg_val = (voltage - QPNP_LBC_VBATWEAK_MIN_UV) /
					QPNP_LBC_VBATWEAK_STEP_UV;
		pr_debug("VBAT_WEAK=%d setting %02x\n",
				chip->cfg_batt_weak_voltage_uv, reg_val);
		rc = qpnp_lbc_write(chip, chip->chgr_base + CHG_VBAT_WEAK_REG,
					&reg_val, 1);
		if (rc)
			pr_err("Failed to set VBAT_WEAK rc=%d\n", rc);
	}

	return rc;
}

#define QPNP_LBC_VBAT_MIN_MV		4000
#define QPNP_LBC_VBAT_MAX_MV		4775
#define QPNP_LBC_VBAT_STEP_MV		25
static int qpnp_lbc_vddsafe_set(struct qpnp_lbc_chip *chip, int voltage)
{
	u8 reg_val;
	int rc;

	if (voltage < QPNP_LBC_VBAT_MIN_MV
			|| voltage > QPNP_LBC_VBAT_MAX_MV) {
		pr_err("bad mV=%d asked to set\n", voltage);
		return -EINVAL;
	}
	reg_val = (voltage - QPNP_LBC_VBAT_MIN_MV) / QPNP_LBC_VBAT_STEP_MV;
	pr_debug("voltage=%d setting %02x\n", voltage, reg_val);
	rc = qpnp_lbc_write(chip, chip->chgr_base + CHG_VDD_SAFE_REG,
				&reg_val, 1);
	if (rc)
		pr_err("Failed to set VDD_SAFE rc=%d\n", rc);

	return rc;
}

static int qpnp_lbc_vddmax_set(struct qpnp_lbc_chip *chip, int voltage)
{
	u8 reg_val;
	int rc, trim_val;
	unsigned long flags;

	if (voltage < QPNP_LBC_VBAT_MIN_MV
			|| voltage > QPNP_LBC_VBAT_MAX_MV) {
		pr_err("bad mV=%d asked to set\n", voltage);
		return -EINVAL;
	}

	spin_lock_irqsave(&chip->hw_access_lock, flags);
	reg_val = (voltage - QPNP_LBC_VBAT_MIN_MV) / QPNP_LBC_VBAT_STEP_MV;
	pr_debug("voltage=%d setting %02x\n", voltage, reg_val);
	rc = __qpnp_lbc_write(chip->spmi, chip->chgr_base + CHG_VDD_MAX_REG,
				&reg_val, 1);
	if (rc) {
		pr_err("Failed to set VDD_MAX rc=%d\n", rc);
		goto out;
	}

	
	if (chip->supported_feature_flag & VDD_TRIM_SUPPORTED) {
		trim_val = qpnp_lbc_get_trim_val(chip);
		reg_val = (trim_val & VDD_TRIM3_MASK) << VDD_TRIM3_SHIFT;
		rc = __qpnp_lbc_secure_masked_write(chip->spmi,
				chip->misc_base, MISC_TRIM3_REG,
				MISC_TRIM3_VDD_MASK, reg_val);
		if (rc) {
			pr_err("Failed to set MISC_TRIM3_REG rc=%d\n", rc);
			goto out;
		}

		reg_val = (trim_val & VDD_TRIM4_MASK) << VDD_TRIM4_SHIFT;
		rc = __qpnp_lbc_secure_masked_write(chip->spmi,
				chip->misc_base, MISC_TRIM4_REG,
				MISC_TRIM4_VDD_MASK, reg_val);
		if (rc) {
			pr_err("Failed to set MISC_TRIM4_REG rc=%d\n", rc);
			goto out;
		}

		chip->delta_vddmax_uv = qpnp_lbc_get_trim_voltage(trim_val);
		if (chip->delta_vddmax_uv == -EINVAL) {
			pr_err("Invalid trim voltage=%d\n",
					chip->delta_vddmax_uv);
			rc = -EINVAL;
			goto out;
		}

		pr_debug("VDD_MAX delta=%d trim value=%x\n",
				chip->delta_vddmax_uv, trim_val);
	}

out:
	spin_unlock_irqrestore(&chip->hw_access_lock, flags);
	return rc;
}

#ifndef CONFIG_QPNP_LINEAR_CHARGER_SMB1360
static int qpnp_lbc_set_appropriate_vddmax(struct qpnp_lbc_chip *chip)
{
	int rc;

	if (chip->bat_is_cool)
		rc = qpnp_lbc_vddmax_set(chip, chip->cfg_cool_bat_mv);
	else if (chip->bat_is_warm)
		rc = qpnp_lbc_vddmax_set(chip, chip->cfg_warm_bat_mv);
	else
		rc = qpnp_lbc_vddmax_set(chip, chip->cfg_max_voltage_mv);
	if (rc)
		pr_err("Failed to set appropriate vddmax rc=%d\n", rc);

	return rc;
}

#define QPNP_LBC_MIN_DELTA_UV			13000
static void qpnp_lbc_adjust_vddmax(struct qpnp_lbc_chip *chip, int vbat_uv)
{
	int delta_uv, prev_delta_uv, rc;

	prev_delta_uv =  chip->delta_vddmax_uv;
	delta_uv = (int)(chip->cfg_max_voltage_mv * 1000) - vbat_uv;

	if (delta_uv > 0 && delta_uv < QPNP_LBC_MIN_DELTA_UV) {
		pr_debug("vbat is not low enough to increase vdd\n");
		return;
	}

	pr_debug("vbat=%d current delta_uv=%d prev delta_vddmax_uv=%d\n",
			vbat_uv, delta_uv, chip->delta_vddmax_uv);
	chip->delta_vddmax_uv = delta_uv + chip->delta_vddmax_uv;
	pr_debug("new delta_vddmax_uv  %d\n", chip->delta_vddmax_uv);
	rc = qpnp_lbc_set_appropriate_vddmax(chip);
	if (rc) {
		pr_err("Failed to set appropriate vddmax rc=%d\n", rc);
		chip->delta_vddmax_uv = prev_delta_uv;
	}
}
#endif

#define QPNP_LBC_VINMIN_MIN_MV		4200
#define QPNP_LBC_VINMIN_MAX_MV		5037
#define QPNP_LBC_VINMIN_STEP_MV		27
static int qpnp_lbc_vinmin_set(struct qpnp_lbc_chip *chip, int voltage)
{
	u8 reg_val;
	int rc;

	if ((voltage < QPNP_LBC_VINMIN_MIN_MV)
			|| (voltage > QPNP_LBC_VINMIN_MAX_MV)) {
		pr_err("bad mV=%d asked to set\n", voltage);
		return -EINVAL;
	}

	reg_val = (voltage - QPNP_LBC_VINMIN_MIN_MV) / QPNP_LBC_VINMIN_STEP_MV;
	pr_debug("VIN_MIN=%d setting %02x\n", voltage, reg_val);
	rc = qpnp_lbc_write(chip, chip->chgr_base + CHG_VIN_MIN_REG,
				&reg_val, 1);
	if (rc)
		pr_err("Failed to set VIN_MIN rc=%d\n", rc);

	return rc;
}

#define QPNP_LBC_IBATSAFE_MIN_MA	90
#define QPNP_LBC_IBATSAFE_MAX_MA	1440
#define QPNP_LBC_I_STEP_MA		90
static int qpnp_lbc_ibatsafe_set(struct qpnp_lbc_chip *chip, int safe_current)
{
	u8 reg_val;
	int rc;

	if (safe_current < QPNP_LBC_IBATSAFE_MIN_MA
			|| safe_current > QPNP_LBC_IBATSAFE_MAX_MA) {
		pr_err("bad mA=%d asked to set\n", safe_current);
		return -EINVAL;
	}

	reg_val = (safe_current - QPNP_LBC_IBATSAFE_MIN_MA)
			/ QPNP_LBC_I_STEP_MA;
	pr_debug("Ibate_safe=%d setting %02x\n", safe_current, reg_val);

	rc = qpnp_lbc_write(chip, chip->chgr_base + CHG_IBAT_SAFE_REG,
				&reg_val, 1);
	if (rc)
		pr_err("Failed to set IBAT_SAFE rc=%d\n", rc);

	return rc;
}

#define QPNP_LBC_IBATMAX_MIN	90
#define QPNP_LBC_IBATMAX_MAX	1440
static int qpnp_lbc_ibatmax_set(struct qpnp_lbc_chip *chip, int chg_current)
{
	u8 reg_val;
	int rc;

	if (chg_current > QPNP_LBC_IBATMAX_MAX)
		pr_debug("bad mA=%d clamping current\n", chg_current);

	chg_current = clamp(chg_current, QPNP_LBC_IBATMAX_MIN,
						QPNP_LBC_IBATMAX_MAX);
	reg_val = (chg_current - QPNP_LBC_IBATMAX_MIN) / QPNP_LBC_I_STEP_MA;
	pr_info("pm8916 CHG_IBAT_MAX_REG write reg=0x%02X\n", reg_val);

	rc = qpnp_lbc_write(chip, chip->chgr_base + CHG_IBAT_MAX_REG,
				&reg_val, 1);
	if (rc)
		pr_err("Failed to set IBAT_MAX rc=%d\n", rc);
	else
		chip->prev_max_ma = chg_current;

	reg_val = 0;
	qpnp_lbc_read(chip, chip->chgr_base + CHG_IBAT_MAX_REG,
				&reg_val, 1);
	pr_info("pm8916 CHG_IBAT_MAX_REG read reg=0x%02X\n", reg_val);

	return rc;
}

#define QPNP_LBC_TCHG_MIN	4
#define QPNP_LBC_TCHG_MAX	512
#define QPNP_LBC_TCHG_STEP	4
static int qpnp_lbc_tchg_max_set(struct qpnp_lbc_chip *chip, int minutes)
{
	u8 reg_val = 0;
	int rc;

	minutes = clamp(minutes, QPNP_LBC_TCHG_MIN, QPNP_LBC_TCHG_MAX);

	
	rc = qpnp_lbc_masked_write(chip, chip->chgr_base + CHG_TCHG_MAX_EN_REG,
						CHG_TCHG_MAX_EN_BIT, 0);
	if (rc) {
		pr_err("Failed to write tchg_max_en rc=%d\n", rc);
		return rc;
	}

	return rc;

	reg_val = (minutes / QPNP_LBC_TCHG_STEP) - 1;

	pr_debug("TCHG_MAX=%d mins setting %x\n", minutes, reg_val);
	rc = qpnp_lbc_masked_write(chip, chip->chgr_base + CHG_TCHG_MAX_REG,
						CHG_TCHG_MAX_MASK, reg_val);
	if (rc) {
		pr_err("Failed to write tchg_max_reg rc=%d\n", rc);
		return rc;
	}

	
	rc = qpnp_lbc_masked_write(chip, chip->chgr_base + CHG_TCHG_MAX_EN_REG,
				CHG_TCHG_MAX_EN_BIT, CHG_TCHG_MAX_EN_BIT);
	if (rc) {
		pr_err("Failed to write tchg_max_en rc=%d\n", rc);
		return rc;
	}

	return rc;
}

static int qpnp_lbc_vbatdet_override(struct qpnp_lbc_chip *chip, int ovr_val)
{
	int rc;
	u8 reg_val;
	struct spmi_device *spmi = chip->spmi;
	unsigned long flags;

	spin_lock_irqsave(&chip->hw_access_lock, flags);

	rc = __qpnp_lbc_read(spmi, chip->chgr_base + CHG_COMP_OVR1,
				&reg_val, 1);
	if (rc) {
		pr_err("spmi read failed: addr=%03X, rc=%d\n",
						chip->chgr_base, rc);
		goto out;
	}
	pr_debug("addr = 0x%x read 0x%x\n", chip->chgr_base, reg_val);

	reg_val &= ~CHG_VBAT_DET_OVR_MASK;
	reg_val |= ovr_val & CHG_VBAT_DET_OVR_MASK;

	pr_debug("writing to base=%x val=%x\n", chip->chgr_base, reg_val);

	rc = __qpnp_lbc_secure_write(spmi, chip->chgr_base, CHG_COMP_OVR1,
					&reg_val, 1);
	if (rc)
		pr_err("spmi write failed: addr=%03X, rc=%d\n",
						chip->chgr_base, rc);

out:
	spin_unlock_irqrestore(&chip->hw_access_lock, flags);
	return rc;
}

static int get_prop_battery_voltage_now(struct qpnp_lbc_chip *chip)
{
	int rc = 0;
	struct qpnp_vadc_result results;

	rc = qpnp_vadc_read(chip->vadc_dev, VBAT_SNS, &results);
	if (rc) {
		pr_err("Unable to read vbat rc=%d\n", rc);
		return 0;
	}

	return results.physical;
}

static int get_prop_batt_present(struct qpnp_lbc_chip *chip)
{
	u8 reg_val;
	int rc;

	rc = qpnp_lbc_read(chip, chip->bat_if_base + BAT_IF_PRES_STATUS_REG,
				&reg_val, 1);
	if (rc) {
		pr_err("Failed to read battery status read failed rc=%d\n",
				rc);
		return 0;
	}

	return (reg_val & BATT_PRES_MASK) ? 1 : 0;
}

#ifndef CONFIG_QPNP_LINEAR_CHARGER_SMB1360
static int get_prop_batt_health(struct qpnp_lbc_chip *chip)
{
	u8 reg_val;
	int rc;

	rc = qpnp_lbc_read(chip, chip->bat_if_base + BAT_IF_TEMP_STATUS_REG,
				&reg_val, 1);
	if (rc) {
		pr_err("Failed to read battery health rc=%d\n", rc);
		return POWER_SUPPLY_HEALTH_UNKNOWN;
	}

	if (BATT_TEMP_HOT_MASK & reg_val)
		return POWER_SUPPLY_HEALTH_OVERHEAT;
	if (!(BATT_TEMP_COLD_MASK & reg_val))
		return POWER_SUPPLY_HEALTH_COLD;
	if (chip->bat_is_cool)
		return POWER_SUPPLY_HEALTH_COOL;
	if (chip->bat_is_warm)
		return POWER_SUPPLY_HEALTH_WARM;

	return POWER_SUPPLY_HEALTH_GOOD;
}

static int get_prop_charge_type(struct qpnp_lbc_chip *chip)
{
	int rc;
	u8 reg_val;

	if (!get_prop_batt_present(chip))
		return POWER_SUPPLY_CHARGE_TYPE_NONE;

	rc = qpnp_lbc_read(chip, chip->chgr_base + INT_RT_STS_REG,
				&reg_val, 1);
	if (rc) {
		pr_err("Failed to read interrupt sts %d\n", rc);
		return POWER_SUPPLY_CHARGE_TYPE_NONE;
	}

	if (reg_val & FAST_CHG_ON_IRQ)
		return POWER_SUPPLY_CHARGE_TYPE_FAST;

	return POWER_SUPPLY_CHARGE_TYPE_NONE;
}
#endif

static int get_prop_batt_status(struct qpnp_lbc_chip *chip)
{
	int rc;
	u8 reg_val;

	if (qpnp_lbc_is_usb_chg_plugged_in(chip) && chip->chg_done)
		return POWER_SUPPLY_STATUS_FULL;

	rc = qpnp_lbc_read(chip, chip->chgr_base + INT_RT_STS_REG,
				&reg_val, 1);
	if (rc) {
		pr_err("Failed to read interrupt sts rc= %d\n", rc);
		return POWER_SUPPLY_CHARGE_TYPE_NONE;
	}

	if (reg_val & FAST_CHG_ON_IRQ)
		return POWER_SUPPLY_STATUS_CHARGING;

	return POWER_SUPPLY_STATUS_DISCHARGING;
}

#ifndef CONFIG_QPNP_LINEAR_CHARGER_SMB1360
static int get_prop_current_now(struct qpnp_lbc_chip *chip)
{
	union power_supply_propval ret = {0,};

	if (chip->bms_psy) {
		chip->bms_psy->get_property(chip->bms_psy,
			  POWER_SUPPLY_PROP_CURRENT_NOW, &ret);
		return ret.intval;
	} else {
		pr_debug("No BMS supply registered return 0\n");
	}

	return 0;
}
#endif

#define DEFAULT_CAPACITY	50
static int get_prop_capacity(struct qpnp_lbc_chip *chip)
{
	union power_supply_propval ret = {0,};
	int soc, battery_status, charger_in;

	if (chip->fake_battery_soc >= 0)
		return chip->fake_battery_soc;

	if (chip->cfg_use_fake_battery || !get_prop_batt_present(chip))
		return DEFAULT_CAPACITY;

	if (chip->bms_psy) {
		chip->bms_psy->get_property(chip->bms_psy,
				POWER_SUPPLY_PROP_CAPACITY, &ret);
		mutex_lock(&chip->chg_enable_lock);
		if (chip->chg_done)
			chip->bms_psy->get_property(chip->bms_psy,
					POWER_SUPPLY_PROP_CAPACITY, &ret);
		battery_status = get_prop_batt_status(chip);
		charger_in = qpnp_lbc_is_usb_chg_plugged_in(chip);

		
		if (ret.intval < 100 && chip->chg_done) {
			chip->chg_done = false;
			power_supply_changed(&chip->batt_psy);
		}

		if (battery_status != POWER_SUPPLY_STATUS_CHARGING
				&& charger_in
				&& !chip->cfg_charging_disabled
				&& chip->cfg_soc_resume_limit
				&& ret.intval <= chip->cfg_soc_resume_limit) {
			pr_debug("resuming charging at %d%% soc\n",
					ret.intval);
			if (!chip->cfg_disable_vbatdet_based_recharge)
				qpnp_lbc_vbatdet_override(chip, OVERRIDE_0);
			qpnp_lbc_charger_enable(chip, SOC, 1);
		}
		mutex_unlock(&chip->chg_enable_lock);

		soc = ret.intval;
		if (soc == 0) {
			if (!qpnp_lbc_is_usb_chg_plugged_in(chip))
				pr_warn_ratelimited("Batt 0, CHG absent\n");
		}
		return soc;
	} else {
		pr_debug("No BMS supply registered return %d\n",
							DEFAULT_CAPACITY);
	}

	return DEFAULT_CAPACITY;
}

#define DEFAULT_TEMP		250
#ifndef CONFIG_QPNP_LINEAR_CHARGER_SMB1360
static int get_prop_batt_temp(struct qpnp_lbc_chip *chip)
{
	int rc = 0;
	struct qpnp_vadc_result results;

	if (chip->cfg_use_fake_battery || !get_prop_batt_present(chip))
		return DEFAULT_TEMP;

	rc = qpnp_vadc_read(chip->vadc_dev, LR_MUX1_BATT_THERM, &results);
	if (rc) {
		pr_debug("Unable to read batt temperature rc=%d\n", rc);
		return DEFAULT_TEMP;
	}
	pr_debug("get_bat_temp %d, %lld\n", results.adc_code,
							results.physical);

	return (int)results.physical;
}

static void qpnp_lbc_set_appropriate_current(struct qpnp_lbc_chip *chip)
{
	unsigned int chg_current = chip->usb_psy_ma;

	if (chip->bat_is_cool && chip->cfg_cool_bat_chg_ma)
		chg_current = min(chg_current, chip->cfg_cool_bat_chg_ma);
	if (chip->bat_is_warm && chip->cfg_warm_bat_chg_ma)
		chg_current = min(chg_current, chip->cfg_warm_bat_chg_ma);
	if (chip->therm_lvl_sel != 0 && chip->thermal_mitigation)
		chg_current = min(chg_current,
			chip->thermal_mitigation[chip->therm_lvl_sel]);

	pr_debug("setting charger current %d mA\n", chg_current);
	qpnp_lbc_ibatmax_set(chip, chg_current);
}

static void qpnp_batt_external_power_changed(struct power_supply *psy)
{
	struct qpnp_lbc_chip *chip = container_of(psy, struct qpnp_lbc_chip,
								batt_psy);
	union power_supply_propval ret = {0,};
	int current_ma;
	unsigned long flags;

	spin_lock_irqsave(&chip->ibat_change_lock, flags);
	if (!chip->bms_psy)
		chip->bms_psy = power_supply_get_by_name("bms");

	if (qpnp_lbc_is_usb_chg_plugged_in(chip)) {
		chip->usb_psy->get_property(chip->usb_psy,
				POWER_SUPPLY_PROP_CURRENT_MAX, &ret);
		current_ma = ret.intval / 1000;

		if (current_ma == chip->prev_max_ma)
			goto skip_current_config;

		
		if (current_ma <= 2 && !chip->cfg_use_fake_battery
				&& get_prop_batt_present(chip)) {
			qpnp_lbc_charger_enable(chip, CURRENT, 0);
			chip->usb_psy_ma = QPNP_CHG_I_MAX_MIN_90;
			qpnp_lbc_set_appropriate_current(chip);
		} else {
			chip->usb_psy_ma = current_ma;
			qpnp_lbc_set_appropriate_current(chip);
			qpnp_lbc_charger_enable(chip, CURRENT, 1);
		}
	}

skip_current_config:
	spin_unlock_irqrestore(&chip->ibat_change_lock, flags);
	pr_debug("power supply changed batt_psy\n");
	power_supply_changed(&chip->batt_psy);
}

static int qpnp_lbc_system_temp_level_set(struct qpnp_lbc_chip *chip,
								int lvl_sel)
{
	int rc = 0;
	int prev_therm_lvl;
	unsigned long flags;

	if (!chip->thermal_mitigation) {
		pr_err("Thermal mitigation not supported\n");
		return -EINVAL;
	}

	if (lvl_sel < 0) {
		pr_err("Unsupported level selected %d\n", lvl_sel);
		return -EINVAL;
	}

	if (lvl_sel >= chip->cfg_thermal_levels) {
		pr_err("Unsupported level selected %d forcing %d\n", lvl_sel,
				chip->cfg_thermal_levels - 1);
		lvl_sel = chip->cfg_thermal_levels - 1;
	}

	if (lvl_sel == chip->therm_lvl_sel)
		return 0;

	spin_lock_irqsave(&chip->ibat_change_lock, flags);
	prev_therm_lvl = chip->therm_lvl_sel;
	chip->therm_lvl_sel = lvl_sel;
	if (chip->therm_lvl_sel == (chip->cfg_thermal_levels - 1)) {
		
		rc = qpnp_lbc_charger_enable(chip, THERMAL, 0);
		if (rc < 0)
			dev_err(chip->dev,
				"Failed to set disable charging rc %d\n", rc);
		goto out;
	}

	qpnp_lbc_set_appropriate_current(chip);

	if (prev_therm_lvl == chip->cfg_thermal_levels - 1) {
		rc = qpnp_lbc_charger_enable(chip, THERMAL, 1);
		if (rc < 0) {
			dev_err(chip->dev,
				"Failed to enable charging rc %d\n", rc);
		}
	}
out:
	spin_unlock_irqrestore(&chip->ibat_change_lock, flags);
	return rc;
}

#define MIN_COOL_TEMP		-300
#define MAX_WARM_TEMP		1000
#define HYSTERISIS_DECIDEGC	20

static int qpnp_lbc_configure_jeita(struct qpnp_lbc_chip *chip,
			enum power_supply_property psp, int temp_degc)
{
	int rc = 0;

	if ((temp_degc < MIN_COOL_TEMP) || (temp_degc > MAX_WARM_TEMP)) {
		pr_err("Bad temperature request %d\n", temp_degc);
		return -EINVAL;
	}

	mutex_lock(&chip->jeita_configure_lock);
	switch (psp) {
	case POWER_SUPPLY_PROP_COOL_TEMP:
		if (temp_degc >=
			(chip->cfg_warm_bat_decidegc - HYSTERISIS_DECIDEGC)) {
			pr_err("Can't set cool %d higher than warm %d - hysterisis %d\n",
					temp_degc,
					chip->cfg_warm_bat_decidegc,
					HYSTERISIS_DECIDEGC);
			rc = -EINVAL;
			goto mutex_unlock;
		}
		if (chip->bat_is_cool)
			chip->adc_param.high_temp =
				temp_degc + HYSTERISIS_DECIDEGC;
		else if (!chip->bat_is_warm)
			chip->adc_param.low_temp = temp_degc;

		chip->cfg_cool_bat_decidegc = temp_degc;
		break;
	case POWER_SUPPLY_PROP_WARM_TEMP:
		if (temp_degc <=
		(chip->cfg_cool_bat_decidegc + HYSTERISIS_DECIDEGC)) {
			pr_err("Can't set warm %d higher than cool %d + hysterisis %d\n",
					temp_degc,
					chip->cfg_warm_bat_decidegc,
					HYSTERISIS_DECIDEGC);
			rc = -EINVAL;
			goto mutex_unlock;
		}
		if (chip->bat_is_warm)
			chip->adc_param.low_temp =
				temp_degc - HYSTERISIS_DECIDEGC;
		else if (!chip->bat_is_cool)
			chip->adc_param.high_temp = temp_degc;

		chip->cfg_warm_bat_decidegc = temp_degc;
		break;
	default:
		rc = -EINVAL;
		goto mutex_unlock;
	}

	if (qpnp_adc_tm_channel_measure(chip->adc_tm_dev, &chip->adc_param))
		pr_err("request ADC error\n");

mutex_unlock:
	mutex_unlock(&chip->jeita_configure_lock);
	return rc;
}

static int qpnp_batt_property_is_writeable(struct power_supply *psy,
					enum power_supply_property psp)
{
	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
	case POWER_SUPPLY_PROP_CAPACITY:
	case POWER_SUPPLY_PROP_CHARGING_ENABLED:
	case POWER_SUPPLY_PROP_COOL_TEMP:
	case POWER_SUPPLY_PROP_VOLTAGE_MIN:
	case POWER_SUPPLY_PROP_WARM_TEMP:
	case POWER_SUPPLY_PROP_SYSTEM_TEMP_LEVEL:
		return 1;
	default:
		break;
	}

	return 0;
}

static int qpnp_batt_power_set_property(struct power_supply *psy,
				enum power_supply_property psp,
				const union power_supply_propval *val)
{
	struct qpnp_lbc_chip *chip = container_of(psy, struct qpnp_lbc_chip,
								batt_psy);
	int rc = 0;

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		if (val->intval == POWER_SUPPLY_STATUS_FULL &&
				!chip->cfg_float_charge) {
			mutex_lock(&chip->chg_enable_lock);

			
			rc = qpnp_lbc_charger_enable(chip, SOC, 0);
			if (rc)
				pr_err("Failed to disable charging rc=%d\n",
						rc);
			else
				chip->chg_done = true;

			if (!chip->cfg_disable_vbatdet_based_recharge) {
				
				rc = qpnp_lbc_vbatdet_override(chip,
							OVERRIDE_NONE);
				if (rc)
					pr_err("Failed to override VBAT_DET rc=%d\n",
							rc);
				else
					qpnp_lbc_enable_irq(chip,
						&chip->irqs[CHG_VBAT_DET_LO]);
			}

			mutex_unlock(&chip->chg_enable_lock);
		}
		break;
	case POWER_SUPPLY_PROP_COOL_TEMP:
		rc = qpnp_lbc_configure_jeita(chip, psp, val->intval);
		break;
	case POWER_SUPPLY_PROP_WARM_TEMP:
		rc = qpnp_lbc_configure_jeita(chip, psp, val->intval);
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		chip->fake_battery_soc = val->intval;
		pr_debug("power supply changed batt_psy\n");
		break;
	case POWER_SUPPLY_PROP_CHARGING_ENABLED:
		chip->cfg_charging_disabled = !(val->intval);
		rc = qpnp_lbc_charger_enable(chip, USER,
						!chip->cfg_charging_disabled);
		if (rc)
			pr_err("Failed to disable charging rc=%d\n", rc);
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MIN:
		qpnp_lbc_vinmin_set(chip, val->intval / 1000);
		break;
	case POWER_SUPPLY_PROP_SYSTEM_TEMP_LEVEL:
		qpnp_lbc_system_temp_level_set(chip, val->intval);
		break;
	default:
		return -EINVAL;
	}

	power_supply_changed(&chip->batt_psy);
	return rc;
}

static int qpnp_batt_power_get_property(struct power_supply *psy,
				       enum power_supply_property psp,
				       union power_supply_propval *val)
{
	struct qpnp_lbc_chip *chip =
		container_of(psy, struct qpnp_lbc_chip, batt_psy);

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		val->intval = get_prop_batt_status(chip);
		break;
	case POWER_SUPPLY_PROP_CHARGE_TYPE:
		val->intval = get_prop_charge_type(chip);
		break;
	case POWER_SUPPLY_PROP_HEALTH:
		val->intval = get_prop_batt_health(chip);
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = get_prop_batt_present(chip);
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN:
		val->intval = chip->cfg_max_voltage_mv * 1000;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MIN_DESIGN:
		val->intval = chip->cfg_min_voltage_mv * 1000;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		val->intval = get_prop_battery_voltage_now(chip);
		break;
	case POWER_SUPPLY_PROP_TEMP:
		val->intval = get_prop_batt_temp(chip);
		break;
	case POWER_SUPPLY_PROP_COOL_TEMP:
		val->intval = chip->cfg_cool_bat_decidegc;
		break;
	case POWER_SUPPLY_PROP_WARM_TEMP:
		val->intval = chip->cfg_warm_bat_decidegc;
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		val->intval = get_prop_capacity(chip);
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		val->intval = get_prop_current_now(chip);
		break;
	case POWER_SUPPLY_PROP_CHARGING_ENABLED:
		val->intval = !(chip->cfg_charging_disabled);
		break;
	case POWER_SUPPLY_PROP_SYSTEM_TEMP_LEVEL:
		val->intval = chip->therm_lvl_sel;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static void qpnp_lbc_jeita_adc_notification(enum qpnp_tm_state state, void *ctx)
{
	struct qpnp_lbc_chip *chip = ctx;
	bool bat_warm = 0, bat_cool = 0;
	int temp;
	unsigned long flags;

	if (state >= ADC_TM_STATE_NUM) {
		pr_err("invalid notification %d\n", state);
		return;
	}

	temp = get_prop_batt_temp(chip);

	pr_debug("temp = %d state = %s\n", temp,
			state == ADC_TM_WARM_STATE ? "warm" : "cool");

	if (state == ADC_TM_WARM_STATE) {
		if (temp >= chip->cfg_warm_bat_decidegc) {
			
			bat_warm = true;
			bat_cool = false;
			chip->adc_param.low_temp =
					chip->cfg_warm_bat_decidegc
					- HYSTERISIS_DECIDEGC;
			chip->adc_param.state_request =
				ADC_TM_COOL_THR_ENABLE;
		} else if (temp >=
			chip->cfg_cool_bat_decidegc + HYSTERISIS_DECIDEGC) {
			
			bat_warm = false;
			bat_cool = false;

			chip->adc_param.low_temp =
					chip->cfg_cool_bat_decidegc;
			chip->adc_param.high_temp =
					chip->cfg_warm_bat_decidegc;
			chip->adc_param.state_request =
					ADC_TM_HIGH_LOW_THR_ENABLE;
		}
	} else {
		if (temp <= chip->cfg_cool_bat_decidegc) {
			
			bat_warm = false;
			bat_cool = true;
			chip->adc_param.high_temp =
					chip->cfg_cool_bat_decidegc
					+ HYSTERISIS_DECIDEGC;
			chip->adc_param.state_request =
					ADC_TM_WARM_THR_ENABLE;
		} else if (temp <= (chip->cfg_warm_bat_decidegc -
					HYSTERISIS_DECIDEGC)){
			
			bat_warm = false;
			bat_cool = false;

			chip->adc_param.low_temp =
					chip->cfg_cool_bat_decidegc;
			chip->adc_param.high_temp =
					chip->cfg_warm_bat_decidegc;
			chip->adc_param.state_request =
					ADC_TM_HIGH_LOW_THR_ENABLE;
		}
	}

	if (chip->bat_is_cool ^ bat_cool || chip->bat_is_warm ^ bat_warm) {
		spin_lock_irqsave(&chip->ibat_change_lock, flags);
		chip->bat_is_cool = bat_cool;
		chip->bat_is_warm = bat_warm;
		qpnp_lbc_set_appropriate_vddmax(chip);
		qpnp_lbc_set_appropriate_current(chip);
		spin_unlock_irqrestore(&chip->ibat_change_lock, flags);
	}

	pr_debug("warm %d, cool %d, low = %d deciDegC, high = %d deciDegC\n",
			chip->bat_is_warm, chip->bat_is_cool,
			chip->adc_param.low_temp, chip->adc_param.high_temp);

	if (qpnp_adc_tm_channel_measure(chip->adc_tm_dev, &chip->adc_param))
		pr_err("request ADC error\n");
}
#endif

#define IBAT_TERM_EN_MASK		BIT(3)
static int qpnp_lbc_chg_init(struct qpnp_lbc_chip *chip)
{
	int rc;
	u8 reg_val;

	qpnp_lbc_vbatweak_set(chip, chip->cfg_batt_weak_voltage_uv);
	rc = qpnp_lbc_vinmin_set(chip, chip->cfg_min_voltage_mv);
	if (rc) {
		pr_err("Failed  to set  vin_min rc=%d\n", rc);
		return rc;
	}
	rc = qpnp_lbc_vddsafe_set(chip, chip->cfg_safe_voltage_mv);
	if (rc) {
		pr_err("Failed to set vdd_safe rc=%d\n", rc);
		return rc;
	}
	rc = qpnp_lbc_vddmax_set(chip, chip->cfg_max_voltage_mv);
	if (rc) {
		pr_err("Failed to set vdd_safe rc=%d\n", rc);
		return rc;
	}
	rc = qpnp_lbc_ibatsafe_set(chip, chip->cfg_safe_current);
	if (rc) {
		pr_err("Failed to set ibat_safe rc=%d\n", rc);
		return rc;
	}

#ifndef CONFIG_QPNP_LINEAR_CHARGER_SMB1360
	if (of_property_read_bool(chip->spmi->dev.of_node, "qcom,tchg-mins")) {
#endif
		rc = qpnp_lbc_tchg_max_set(chip, chip->cfg_tchg_mins);
		if (rc) {
			pr_err("Failed to set tchg_mins rc=%d\n", rc);
			return rc;
		}
#ifndef CONFIG_QPNP_LINEAR_CHARGER_SMB1360
	}
#endif

	rc = qpnp_lbc_vbatdet_override(chip, OVERRIDE_0);
	if (rc) {
		pr_err("Failed to override comp rc=%d\n", rc);
		return rc;
	}

	if (!chip->cfg_charger_detect_eoc || chip->cfg_float_charge) {
		rc = qpnp_lbc_masked_write(chip,
				chip->chgr_base + CHG_IBATTERM_EN_REG,
				IBAT_TERM_EN_MASK, 0);
		if (rc) {
			pr_err("Failed to disable EOC comp rc=%d\n", rc);
			return rc;
		}
	}

	
	reg_val = 0;
	rc = qpnp_lbc_write(chip, chip->chgr_base + CHG_WDOG_EN_REG,
				&reg_val, 1);

	return rc;
}

#ifndef CONFIG_QPNP_LINEAR_CHARGER_SMB1360
static int qpnp_lbc_bat_if_init(struct qpnp_lbc_chip *chip)
{
	u8 reg_val;
	int rc;

	
	switch (chip->cfg_bpd_detection) {
	case BPD_TYPE_BAT_THM:
		reg_val = BATT_THM_EN;
		break;
	case BPD_TYPE_BAT_ID:
		reg_val = BATT_ID_EN;
		break;
	case BPD_TYPE_BAT_THM_BAT_ID:
		reg_val = BATT_THM_EN | BATT_ID_EN;
		break;
	default:
		reg_val = BATT_THM_EN;
		break;
	}

	rc = qpnp_lbc_masked_write(chip,
			chip->bat_if_base + BAT_IF_BPD_CTRL_REG,
			BATT_BPD_CTRL_SEL_MASK, reg_val);
	if (rc) {
		pr_err("Failed to choose BPD rc=%d\n", rc);
		return rc;
	}

	
	reg_val = VREF_BATT_THERM_FORCE_ON;
	rc = qpnp_lbc_write(chip,
			chip->bat_if_base + BAT_IF_VREF_BAT_THM_CTRL_REG,
			&reg_val, 1);
	if (rc) {
		pr_err("Failed to force on VREF_BAT_THM rc=%d\n", rc);
		return rc;
	}

	return 0;
}

static int qpnp_lbc_usb_path_init(struct qpnp_lbc_chip *chip)
{
	int rc;
	u8 reg_val;

	if (qpnp_lbc_is_usb_chg_plugged_in(chip)) {
		reg_val = 0;
		rc = qpnp_lbc_write(chip,
			chip->usb_chgpth_base + CHG_USB_ENUM_T_STOP_REG,
			&reg_val, 1);
		if (rc) {
			pr_err("Failed to write enum stop rc=%d\n", rc);
			return -ENXIO;
		}
	}

	if (chip->cfg_charging_disabled) {
		rc = qpnp_lbc_charger_enable(chip, USER, 0);
		if (rc)
			pr_err("Failed to disable charging rc=%d\n", rc);
	} else {
		reg_val = CHG_ENABLE;
		rc = qpnp_lbc_masked_write(chip, chip->chgr_base + CHG_CTRL_REG,
					CHG_EN_MASK, reg_val);
		if (rc)
			pr_err("Failed to enable charger rc=%d\n", rc);
	}

	return rc;
}
#endif

#define LBC_MISC_DIG_VERSION_1			0x01
static int qpnp_lbc_misc_init(struct qpnp_lbc_chip *chip)
{
	int rc;
	u8 reg_val, reg_val1, trim_center;

	
	rc = qpnp_lbc_read(chip, chip->misc_base + MISC_REV2_REG,
			&reg_val, 1);
	if (rc) {
		pr_err("Failed to read VDD_EA TRIM3 reg rc=%d\n", rc);
		return rc;
	}

	if (reg_val >= LBC_MISC_DIG_VERSION_1) {
		chip->supported_feature_flag |= VDD_TRIM_SUPPORTED;
		
		rc = qpnp_lbc_read(chip, chip->misc_base + MISC_TRIM3_REG,
				&reg_val, 1);
		if (rc) {
			pr_err("Failed to read VDD_EA TRIM3 reg rc=%d\n", rc);
			return rc;
		}

		rc = qpnp_lbc_read(chip, chip->misc_base + MISC_TRIM4_REG,
				&reg_val1, 1);
		if (rc) {
			pr_err("Failed to read VDD_EA TRIM3 reg rc=%d\n", rc);
			return rc;
		}

		trim_center = ((reg_val & MISC_TRIM3_VDD_MASK)
					>> VDD_TRIM3_SHIFT)
					| ((reg_val1 & MISC_TRIM4_VDD_MASK)
					>> VDD_TRIM4_SHIFT);
		chip->init_trim_uv = qpnp_lbc_get_trim_voltage(trim_center);
		chip->delta_vddmax_uv = chip->init_trim_uv;
		pr_debug("Initial trim center %x trim_uv %d\n",
				trim_center, chip->init_trim_uv);
	}

	pr_info("Setting BOOT_DONE\n");
	reg_val = MISC_BOOT_DONE;
	rc = qpnp_lbc_write(chip, chip->misc_base + MISC_BOOT_DONE_REG,
				&reg_val, 1);

	return rc;
}

#define OF_PROP_READ(chip, prop, qpnp_dt_property, retval, optional)	\
do {									\
	if (retval)							\
		break;							\
									\
	retval = of_property_read_u32(chip->spmi->dev.of_node,		\
					"qcom," qpnp_dt_property,	\
					&chip->prop);			\
									\
	if ((retval == -EINVAL) && optional)				\
		retval = 0;						\
	else if (retval)						\
		pr_err("Error reading " #qpnp_dt_property		\
				" property rc = %d\n", rc);		\
} while (0)

static int qpnp_charger_read_dt_props(struct qpnp_lbc_chip *chip)
{
	int rc = 0;
	const char *bpd;

	OF_PROP_READ(chip, cfg_max_voltage_mv, "vddmax-mv", rc, 0);
chip->cfg_max_voltage_mv = 4400;
	OF_PROP_READ(chip, cfg_safe_voltage_mv, "vddsafe-mv", rc, 0);
chip->cfg_safe_voltage_mv = 4400;
	OF_PROP_READ(chip, cfg_min_voltage_mv, "vinmin-mv", rc, 0);
chip->cfg_min_voltage_mv = 4200;
	OF_PROP_READ(chip, cfg_safe_current, "ibatsafe-ma", rc, 0);
chip->cfg_safe_current = 990;
	if (rc)
		pr_err("Error reading required property rc=%d\n", rc);

#ifndef CONFIG_QPNP_LINEAR_CHARGER_SMB1360
	OF_PROP_READ(chip, cfg_tchg_mins, "tchg-mins", rc, 1);
	OF_PROP_READ(chip, cfg_warm_bat_decidegc, "warm-bat-decidegc", rc, 1);
	OF_PROP_READ(chip, cfg_cool_bat_decidegc, "cool-bat-decidegc", rc, 1);
	OF_PROP_READ(chip, cfg_hot_batt_p, "batt-hot-percentage", rc, 1);
	OF_PROP_READ(chip, cfg_cold_batt_p, "batt-cold-percentage", rc, 1);
	OF_PROP_READ(chip, cfg_batt_weak_voltage_uv, "vbatweak-uv", rc, 1);
	OF_PROP_READ(chip, cfg_soc_resume_limit, "resume-soc", rc, 1);
	if (rc) {
		pr_err("Error reading optional property rc=%d\n", rc);
		return rc;
	}
#endif

	rc = of_property_read_string(chip->spmi->dev.of_node,
						"qcom,bpd-detection", &bpd);
	if (rc) {

		chip->cfg_bpd_detection = BPD_TYPE_BAT_THM;
		rc = 0;
	} else {
		chip->cfg_bpd_detection = get_bpd(bpd);
		if (chip->cfg_bpd_detection < 0) {
			pr_err("Failed to determine bpd schema rc=%d\n", rc);
			return -EINVAL;
		}
	}

	if (chip->cfg_cool_bat_decidegc || chip->cfg_warm_bat_decidegc) {
		chip->adc_tm_dev = qpnp_get_adc_tm(chip->dev, "chg");
		if (IS_ERR(chip->adc_tm_dev)) {
			rc = PTR_ERR(chip->adc_tm_dev);
			if (rc != -EPROBE_DEFER)
				pr_err("Failed to get adc-tm rc=%d\n", rc);
			return rc;
		}

		OF_PROP_READ(chip, cfg_warm_bat_chg_ma, "ibatmax-warm-ma",
				rc, 1);
		OF_PROP_READ(chip, cfg_cool_bat_chg_ma, "ibatmax-cool-ma",
				rc, 1);
		OF_PROP_READ(chip, cfg_warm_bat_mv, "warm-bat-mv", rc, 1);
		OF_PROP_READ(chip, cfg_cool_bat_mv, "cool-bat-mv", rc, 1);
		if (rc) {
			pr_err("Error reading battery temp prop rc=%d\n", rc);
			return rc;
		}
	}

	
	chip->cfg_btc_disabled = of_property_read_bool(
			chip->spmi->dev.of_node, "qcom,btc-disabled");

chip->cfg_btc_disabled = true;

	
	chip->cfg_charging_disabled =
		of_property_read_bool(chip->spmi->dev.of_node,
					"qcom,charging-disabled");
chip->cfg_charging_disabled = false;

	
	chip->cfg_use_fake_battery =
			of_property_read_bool(chip->spmi->dev.of_node,
					"qcom,use-default-batt-values");

	
	chip->cfg_disable_follow_on_reset =
			of_property_read_bool(chip->spmi->dev.of_node,
					"qcom,disable-follow-on-reset");

	
	chip->cfg_float_charge =
			of_property_read_bool(chip->spmi->dev.of_node,
					"qcom,float-charge");

	
	chip->cfg_charger_detect_eoc =
			of_property_read_bool(chip->spmi->dev.of_node,
					"qcom,charger-detect-eoc");

	
	chip->cfg_disable_vbatdet_based_recharge =
			of_property_read_bool(chip->spmi->dev.of_node,
					"qcom,disable-vbatdet-based-recharge");
	
	if (chip->cfg_use_fake_battery)
		chip->cfg_charging_disabled = true;

	chip->cfg_use_external_charger = of_property_read_bool(
			chip->spmi->dev.of_node, "qcom,use-external-charger");

	if (of_find_property(chip->spmi->dev.of_node,
					"qcom,thermal-mitigation",
					&chip->cfg_thermal_levels)) {
		chip->thermal_mitigation = devm_kzalloc(chip->dev,
			chip->cfg_thermal_levels,
			GFP_KERNEL);

		if (chip->thermal_mitigation == NULL) {
			pr_err("thermal mitigation kzalloc() failed.\n");
			return -ENOMEM;
		}

		chip->cfg_thermal_levels /= sizeof(int);
		rc = of_property_read_u32_array(chip->spmi->dev.of_node,
				"qcom,thermal-mitigation",
				chip->thermal_mitigation,
				chip->cfg_thermal_levels);
		if (rc) {
			pr_err("Failed to read threm limits rc = %d\n", rc);
			return rc;
		}
	}

	return rc;
}

#ifndef CONFIG_QPNP_LINEAR_CHARGER_SMB1360
static irqreturn_t qpnp_lbc_usbin_valid_irq_handler(int irq, void *_chip)
{
	struct qpnp_lbc_chip *chip = _chip;
	int usb_present;
	unsigned long flags;

	usb_present = qpnp_lbc_is_usb_chg_plugged_in(chip);
	pr_debug("usbin-valid triggered: %d\n", usb_present);

	if (chip->usb_present ^ usb_present) {
		chip->usb_present = usb_present;
		if (!usb_present) {
			qpnp_lbc_charger_enable(chip, CURRENT, 0);
			spin_lock_irqsave(&chip->ibat_change_lock, flags);
			chip->usb_psy_ma = QPNP_CHG_I_MAX_MIN_90;
			qpnp_lbc_set_appropriate_current(chip);
			spin_unlock_irqrestore(&chip->ibat_change_lock,
								flags);
		} else {
			if (!chip->cfg_disable_vbatdet_based_recharge)
				qpnp_lbc_vbatdet_override(chip, OVERRIDE_0);

			qpnp_lbc_charger_enable(chip, SOC, 1);
		}

		pr_debug("Updating usb_psy PRESENT property\n");
		power_supply_set_present(chip->usb_psy, chip->usb_present);
	}

	return IRQ_HANDLED;
}

static int qpnp_lbc_is_batt_temp_ok(struct qpnp_lbc_chip *chip)
{
	u8 reg_val;
	int rc;

	rc = qpnp_lbc_read(chip, chip->bat_if_base + INT_RT_STS_REG,
				&reg_val, 1);
	if (rc) {
		pr_err("spmi read failed: addr=%03X, rc=%d\n",
				chip->bat_if_base + INT_RT_STS_REG, rc);
		return rc;
	}

	return (reg_val & BAT_TEMP_OK_IRQ) ? 1 : 0;
}

#ifndef CONFIG_QPNP_LINEAR_CHARGER_SMB1360
static irqreturn_t qpnp_lbc_batt_temp_irq_handler(int irq, void *_chip)
{
	struct qpnp_lbc_chip *chip = _chip;
	int batt_temp_good;

	batt_temp_good = qpnp_lbc_is_batt_temp_ok(chip);
	pr_debug("batt-temp triggered: %d\n", batt_temp_good);

	pr_debug("power supply changed batt_psy\n");
	power_supply_changed(&chip->batt_psy);
	return IRQ_HANDLED;
}
#endif

static irqreturn_t qpnp_lbc_batt_pres_irq_handler(int irq, void *_chip)
{
	struct qpnp_lbc_chip *chip = _chip;
	int batt_present;

	batt_present = qpnp_lbc_is_batt_present(chip);
	pr_debug("batt-pres triggered: %d\n", batt_present);

	if (chip->batt_present ^ batt_present) {
		chip->batt_present = batt_present;
		pr_debug("power supply changed batt_psy\n");
		power_supply_changed(&chip->batt_psy);

		if ((chip->cfg_cool_bat_decidegc
					|| chip->cfg_warm_bat_decidegc)
					&& batt_present) {
			pr_debug("enabling vadc notifications\n");
			if (qpnp_adc_tm_channel_measure(chip->adc_tm_dev,
						&chip->adc_param))
				pr_err("request ADC error\n");
		} else if ((chip->cfg_cool_bat_decidegc
					|| chip->cfg_warm_bat_decidegc)
					&& !batt_present) {
			qpnp_adc_tm_disable_chan_meas(chip->adc_tm_dev,
					&chip->adc_param);
			pr_debug("disabling vadc notifications\n");
		}
	}
	return IRQ_HANDLED;
}

static irqreturn_t qpnp_lbc_chg_failed_irq_handler(int irq, void *_chip)
{
	struct qpnp_lbc_chip *chip = _chip;
	int rc;
	u8 reg_val = CHG_FAILED_BIT;

	pr_debug("chg_failed triggered count=%u\n", ++chip->chg_failed_count);
	rc = qpnp_lbc_write(chip, chip->chgr_base + CHG_FAILED_REG,
				&reg_val, 1);
	if (rc)
		pr_err("Failed to write chg_fail clear bit rc=%d\n", rc);

	if (chip->bat_if_base) {
		pr_debug("power supply changed batt_psy\n");
		power_supply_changed(&chip->batt_psy);
	}

	return IRQ_HANDLED;
}

static int qpnp_lbc_is_fastchg_on(struct qpnp_lbc_chip *chip)
{
	u8 reg_val;
	int rc;

	rc = qpnp_lbc_read(chip, chip->chgr_base + INT_RT_STS_REG,
				&reg_val, 1);
	if (rc) {
		pr_err("Failed to read interrupt status rc=%d\n", rc);
		return rc;
	}
	pr_debug("charger status %x\n", reg_val);
	return (reg_val & FAST_CHG_ON_IRQ) ? 1 : 0;
}

#define TRIM_PERIOD_NS			(50LL * NSEC_PER_SEC)
static irqreturn_t qpnp_lbc_fastchg_irq_handler(int irq, void *_chip)
{
	ktime_t kt;
	struct qpnp_lbc_chip *chip = _chip;
	bool fastchg_on = false;

	fastchg_on = qpnp_lbc_is_fastchg_on(chip);

	pr_debug("FAST_CHG IRQ triggered, fastchg_on: %d\n", fastchg_on);

	if (chip->fastchg_on ^ fastchg_on) {
		chip->fastchg_on = fastchg_on;
		if (fastchg_on) {
			mutex_lock(&chip->chg_enable_lock);
			chip->chg_done = false;
			mutex_unlock(&chip->chg_enable_lock);
			if (chip->supported_feature_flag &
						VDD_TRIM_SUPPORTED) {
				kt = ns_to_ktime(TRIM_PERIOD_NS);
				alarm_start_relative(&chip->vddtrim_alarm,
							kt);
			}
		}

		if (chip->bat_if_base) {
			pr_debug("power supply changed batt_psy\n");
			power_supply_changed(&chip->batt_psy);
		}
	}

	return IRQ_HANDLED;
}

static irqreturn_t qpnp_lbc_chg_done_irq_handler(int irq, void *_chip)
{
	struct qpnp_lbc_chip *chip = _chip;

	pr_debug("charging done triggered\n");

	chip->chg_done = true;
	pr_debug("power supply changed batt_psy\n");
	power_supply_changed(&chip->batt_psy);

	return IRQ_HANDLED;
}

static irqreturn_t qpnp_lbc_vbatdet_lo_irq_handler(int irq, void *_chip)
{
	struct qpnp_lbc_chip *chip = _chip;
	int rc;

	pr_debug("vbatdet-lo triggered\n");

	qpnp_lbc_disable_irq(chip, &chip->irqs[CHG_VBAT_DET_LO]);

	qpnp_lbc_vbatdet_override(chip, OVERRIDE_0);

	rc = qpnp_lbc_charger_enable(chip, SOC, 1);
	if (rc)
		pr_err("Failed to enable charging\n");

	return IRQ_HANDLED;
}

static int qpnp_lbc_is_overtemp(struct qpnp_lbc_chip *chip)
{
	u8 reg_val;
	int rc;

	rc = qpnp_lbc_read(chip, chip->usb_chgpth_base + INT_RT_STS_REG,
				&reg_val, 1);
	if (rc) {
		pr_err("Failed to read interrupt status rc=%d\n", rc);
		return rc;
	}

	pr_debug("OVERTEMP rt status %x\n", reg_val);
	return (reg_val & OVERTEMP_ON_IRQ) ? 1 : 0;
}

static irqreturn_t qpnp_lbc_usb_overtemp_irq_handler(int irq, void *_chip)
{
	struct qpnp_lbc_chip *chip = _chip;
	int overtemp = qpnp_lbc_is_overtemp(chip);

	pr_warn_ratelimited("charger %s temperature limit !!!\n",
					overtemp ? "exceeds" : "within");

	return IRQ_HANDLED;
}
#endif

static int qpnp_disable_lbc_charger(struct qpnp_lbc_chip *chip)
{
	int rc;
	u8 reg;

	reg = CHG_FORCE_BATT_ON;
	rc = qpnp_lbc_masked_write(chip, chip->chgr_base + CHG_CTRL_REG,
							CHG_EN_MASK, reg);
	
	rc |= qpnp_lbc_masked_write(chip, chip->bat_if_base + BAT_IF_BTC_CTRL,
							BTC_COMP_EN_MASK, 0);
	
	reg = BATT_ID_EN | BATT_BPD_OFFMODE_EN;
	rc |= qpnp_lbc_write(chip, chip->bat_if_base + BAT_IF_BPD_CTRL_REG,
								&reg, 1);
	return rc;
}

#define SPMI_REQUEST_IRQ(chip, idx, rc, irq_name, threaded, flags, wake)\
do {									\
	if (rc)								\
		break;							\
	if (chip->irqs[idx].irq) {					\
		if (threaded)						\
			rc = devm_request_threaded_irq(chip->dev,	\
				chip->irqs[idx].irq, NULL,		\
				qpnp_lbc_##irq_name##_irq_handler,	\
				flags, #irq_name, chip);		\
		else							\
			rc = devm_request_irq(chip->dev,		\
				chip->irqs[idx].irq,			\
				qpnp_lbc_##irq_name##_irq_handler,	\
				flags, #irq_name, chip);		\
		if (rc < 0) {						\
			pr_err("Unable to request " #irq_name " %d\n",	\
								rc);	\
		} else {						\
			rc = 0;						\
			if (wake) {					\
				enable_irq_wake(chip->irqs[idx].irq);	\
				chip->irqs[idx].is_wake = true;		\
			}						\
		}							\
	}								\
} while (0)

#define SPMI_GET_IRQ_RESOURCE(chip, rc, resource, idx, name)		\
do {									\
	if (rc)								\
		break;							\
									\
	rc = spmi_get_irq_byname(chip->spmi, resource, #name);		\
	if (rc < 0) {							\
		pr_err("Unable to get irq resource " #name "%d\n", rc);	\
	} else {							\
		chip->irqs[idx].irq = rc;				\
		rc = 0;							\
	}								\
} while (0)

#ifndef CONFIG_QPNP_LINEAR_CHARGER_SMB1360
static int qpnp_lbc_request_irqs(struct qpnp_lbc_chip *chip)
{
	int rc = 0;

	SPMI_REQUEST_IRQ(chip, CHG_FAILED, rc, chg_failed, 0,
			IRQF_TRIGGER_RISING, 1);

	SPMI_REQUEST_IRQ(chip, CHG_FAST_CHG, rc, fastchg, 1,
			IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING
			| IRQF_ONESHOT, 1);

	SPMI_REQUEST_IRQ(chip, CHG_DONE, rc, chg_done, 0,
			IRQF_TRIGGER_RISING, 0);

	SPMI_REQUEST_IRQ(chip, CHG_VBAT_DET_LO, rc, vbatdet_lo, 0,
			IRQF_TRIGGER_FALLING, 1);

	SPMI_REQUEST_IRQ(chip, BATT_PRES, rc, batt_pres, 1,
			IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING
			| IRQF_ONESHOT, 1);

	SPMI_REQUEST_IRQ(chip, BATT_TEMPOK, rc, batt_temp, 0,
			IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING, 1);

	SPMI_REQUEST_IRQ(chip, USBIN_VALID, rc, usbin_valid, 0,
			IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING, 1);

	SPMI_REQUEST_IRQ(chip, USB_OVER_TEMP, rc, usb_overtemp, 0,
			IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING, 0);

	return 0;
}
#endif

static int qpnp_lbc_get_irqs(struct qpnp_lbc_chip *chip, u8 subtype,
					struct spmi_resource *spmi_resource)
{
	int rc = 0;

	switch (subtype) {
	case LBC_CHGR_SUBTYPE:
		SPMI_GET_IRQ_RESOURCE(chip, rc, spmi_resource,
						CHG_FAST_CHG, fast-chg-on);
		SPMI_GET_IRQ_RESOURCE(chip, rc, spmi_resource,
						CHG_FAILED, chg-failed);
		if (!chip->cfg_disable_vbatdet_based_recharge)
			SPMI_GET_IRQ_RESOURCE(chip, rc, spmi_resource,
						CHG_VBAT_DET_LO, vbat-det-lo);
		if (chip->cfg_charger_detect_eoc)
			SPMI_GET_IRQ_RESOURCE(chip, rc, spmi_resource,
						CHG_DONE, chg-done);
		break;
	case LBC_BAT_IF_SUBTYPE:
		SPMI_GET_IRQ_RESOURCE(chip, rc, spmi_resource,
						BATT_PRES, batt-pres);
		SPMI_GET_IRQ_RESOURCE(chip, rc, spmi_resource,
						BATT_TEMPOK, bat-temp-ok);
		break;
	case LBC_USB_PTH_SUBTYPE:
		SPMI_GET_IRQ_RESOURCE(chip, rc, spmi_resource,
						USBIN_VALID, usbin-valid);
		SPMI_GET_IRQ_RESOURCE(chip, rc, spmi_resource,
						USB_OVER_TEMP, usb-over-temp);
		break;
	};

	return 0;
}

#ifndef CONFIG_QPNP_LINEAR_CHARGER_SMB1360
static void determine_initial_status(struct qpnp_lbc_chip *chip)
{
	u8 reg_val;
	int rc;

	chip->usb_present = qpnp_lbc_is_usb_chg_plugged_in(chip);
	power_supply_set_present(chip->usb_psy, chip->usb_present);
	if (chip->usb_present)
		power_supply_set_online(chip->usb_psy, 1);

	if (chip->cfg_disable_follow_on_reset) {
		reg_val = 0x0;
		rc = __qpnp_lbc_secure_write(chip->spmi, chip->chgr_base,
				CHG_PERPH_RESET_CTRL3_REG, &reg_val, 1);
		if (rc)
			pr_err("Failed to configure PERPH_CTRL3 rc=%d\n", rc);
		else
			pr_warn("Charger is not following PMIC reset\n");
	}
}

#define IBAT_TRIM			-300
static void qpnp_lbc_vddtrim_work_fn(struct work_struct *work)
{
	int rc, vbat_now_uv, ibat_now;
	u8 reg_val;
	ktime_t kt;
	struct qpnp_lbc_chip *chip = container_of(work, struct qpnp_lbc_chip,
						vddtrim_work);

	vbat_now_uv = get_prop_battery_voltage_now(chip);
	ibat_now = get_prop_current_now(chip) / 1000;
	pr_debug("vbat %d ibat %d capacity %d\n",
			vbat_now_uv, ibat_now, get_prop_capacity(chip));

	if (!qpnp_lbc_is_fastchg_on(chip) ||
			!qpnp_lbc_is_usb_chg_plugged_in(chip)) {
		pr_debug("stop trim charging stopped\n");
		goto exit;
	} else {
		rc = qpnp_lbc_read(chip, chip->chgr_base + CHG_STATUS_REG,
					&reg_val, 1);
		if (rc) {
			pr_err("Failed to read chg status rc=%d\n", rc);
			goto out;
		}

		if ((reg_val & CHG_VDD_LOOP_BIT) &&
				((ibat_now < 0) && (ibat_now > IBAT_TRIM)))
			qpnp_lbc_adjust_vddmax(chip, vbat_now_uv);
	}

out:
	kt = ns_to_ktime(TRIM_PERIOD_NS);
	alarm_start_relative(&chip->vddtrim_alarm, kt);
exit:
	pm_relax(chip->dev);
}

static enum alarmtimer_restart vddtrim_callback(struct alarm *alarm,
					ktime_t now)
{
	struct qpnp_lbc_chip *chip = container_of(alarm, struct qpnp_lbc_chip,
						vddtrim_alarm);

	pm_stay_awake(chip->dev);
	schedule_work(&chip->vddtrim_work);

	return ALARMTIMER_NORESTART;
}
#endif

#ifdef CONFIG_QPNP_LINEAR_CHARGER_SMB1360
int pm8916_charger_enable(bool enable, int chg_current)
{
	int rc = 0;

	pr_info("enable=%d, chg_current=%d\n", enable, chg_current);

	if (!the_chip) {
		pr_err("called before init\n");
		return -EINVAL;
	}

	if (enable) {
		qpnp_lbc_ibatmax_set(the_chip, chg_current);
		rc = qpnp_lbc_charger_enable(the_chip, CURRENT, 1);
	} else {
		rc = qpnp_lbc_charger_enable(the_chip, CURRENT, 0);
	}

	if (rc)
		pr_err("Failed to set enable/disable charging rc=%d\n", rc);

	dump_reg();

	return rc;
}
EXPORT_SYMBOL(pm8916_charger_enable);

int pm8916_get_chgr_fsm_state(struct qpnp_lbc_chip *chip)
{
	int rc;
	u8 fsm, reg;

	
	rc = qpnp_lbc_masked_write(chip,
			chip->chgr_base + SEC_ACCESS,
			0xFF,
			0xA5);
	if (rc) {
		pr_err("failed to write SEC_ACCESS rc=%d\n", rc);
		return rc;
	}

	reg = 0x01;
	rc = qpnp_lbc_write(chip, chip->chgr_base + 0xE6, &reg, 1);
	if (rc)
		pr_err("failed to unsecure charger fsm rc=%d\n", rc);

	rc = qpnp_lbc_read(chip, chip->chgr_base + CHG_FSM_STATE_REG, &fsm, 1);
	if (rc) {
		pr_err("failed to read charger fsm state %d\n", rc);
		return rc;
	}

	return fsm;
}

static int get_chgr_reg(u16 data, u64 *val)
{
	u16 addr = data;
	int rc;
	u8 chgr_sts;

	if (!the_chip) {
		pr_err("called before init\n");
		return -EINVAL;
	}

	rc = qpnp_lbc_read(the_chip, the_chip->chgr_base + addr,
						&chgr_sts, 1);
	if (rc) {
		pr_err("failed to read chgr_base register sts %d\n", rc);
		return -EAGAIN;
	}
	pr_debug("addr:0x%X, val:0x%X\n", (the_chip->chgr_base + addr), chgr_sts);
	*val = chgr_sts;
	return 0;
}

static int get_bat_if_reg(u16 data, u64 *val)
{
	u16 addr = data;
	int rc;
	u8 bat_if_sts;

	if (!the_chip) {
		pr_err("called before init\n");
		return -EINVAL;
	}

	rc = qpnp_lbc_read(the_chip, the_chip->bat_if_base + addr,
						&bat_if_sts, 1);
	if (rc) {
		pr_err("failed to read bat_if_base register sts %d\n", rc);
		return -EAGAIN;
	}
	pr_debug("addr:0x%X, val:0x%X\n", (the_chip->bat_if_base + addr), bat_if_sts);
	*val = bat_if_sts;
	return 0;
}

static int get_usb_chgpth_reg(u16 data, u64 *val)
{
	u16 addr = data;
	int rc;
	u8 chgpth_sts;

	if (!the_chip) {
		pr_err("called before init\n");
		return -EINVAL;
	}

	rc = qpnp_lbc_read(the_chip, the_chip->usb_chgpth_base + addr,
						&chgpth_sts, 1);
	if (rc) {
		pr_err("failed to read chgpth_sts register sts %d\n", rc);
		return -EAGAIN;
	}
	pr_debug("addr:0x%X, val:0x%X\n", (the_chip->usb_chgpth_base + addr), chgpth_sts);
	*val = chgpth_sts;
	return 0;
}

static int get_misc_reg(u16 data, u64 *val)
{
	u16 addr = data;
	int rc;
	u8 misc_sts;

	if (!the_chip) {
		pr_err("called before init\n");
		return -EINVAL;
	}

	rc = qpnp_lbc_read(the_chip, the_chip->misc_base + addr,
						&misc_sts, 1);
	if (rc) {
		pr_err("failed to read misc_sts register sts %d\n", rc);
		return -EAGAIN;
	}
	pr_debug("addr:0x%X, val:0x%X\n", (the_chip->misc_base + addr), misc_sts);
	*val = misc_sts;
	return 0;
}

#define BATT_LOG_BUF_LEN (1024)
static char batt_log_buf[BATT_LOG_BUF_LEN];
static void dump_reg(void)
{
	u64 val;
	unsigned int len =0;

	if (!the_chip) {
		pr_err("called before init\n");
		return;
	}

	memset(batt_log_buf, 0, sizeof(BATT_LOG_BUF_LEN));
	
	len += scnprintf(batt_log_buf + len, BATT_LOG_BUF_LEN - len, "FSM_STATE=%d,",
						pm8916_get_chgr_fsm_state(the_chip));
	get_chgr_reg(CHG_VDD_MAX_REG, &val);
	len += scnprintf(batt_log_buf + len, BATT_LOG_BUF_LEN - len, "VDD_MAX=0x%llX,", val);
	get_chgr_reg(CHG_VDD_SAFE_REG, &val);
	len += scnprintf(batt_log_buf + len, BATT_LOG_BUF_LEN - len, "VDD_SAFE=0x%llX,", val);
	get_chgr_reg(CHG_IBAT_MAX_REG, &val);
	len += scnprintf(batt_log_buf + len, BATT_LOG_BUF_LEN - len, "IBAT_MAX=0x%llX,", val);
	get_chgr_reg(CHG_IBAT_SAFE_REG, &val);
	len += scnprintf(batt_log_buf + len, BATT_LOG_BUF_LEN - len, "IBAT_SAFE=0x%llX,", val);
	get_chgr_reg(CHG_VIN_MIN_REG, &val);
	len += scnprintf(batt_log_buf + len, BATT_LOG_BUF_LEN - len, "VIN_MIN=0x%llX,", val);
	get_chgr_reg(CHG_CTRL_REG, &val);
	len += scnprintf(batt_log_buf + len, BATT_LOG_BUF_LEN - len, "CHG_CTRL=0x%llX,", val);
	get_chgr_reg(CHG_IBATTERM_EN_REG, &val);
	len += scnprintf(batt_log_buf + len, BATT_LOG_BUF_LEN - len, "IBAT_TERM_CHGR=0x%llX,", val);
	get_chgr_reg(CHG_TCHG_MAX_EN_REG, &val);
	len += scnprintf(batt_log_buf + len, BATT_LOG_BUF_LEN - len, "TCHG_MAX_EN=0x%llX,", val);
	get_chgr_reg(CHG_TCHG_MAX_REG, &val);
	len += scnprintf(batt_log_buf + len, BATT_LOG_BUF_LEN - len, "TCHG_MAX=0x%llX,", val);
	get_chgr_reg(CHG_CHG_WDOG_TIME_REG, &val);
	len += scnprintf(batt_log_buf + len, BATT_LOG_BUF_LEN - len, "WDOG_TIME=0x%llX,", val);
	get_chgr_reg(CHG_WDOG_EN_REG, &val);
	len += scnprintf(batt_log_buf + len, BATT_LOG_BUF_LEN - len, "WDOG_EN=0x%llX,", val);
	get_chgr_reg(CHG_STATUS_REG, &val);
	len += scnprintf(batt_log_buf + len, BATT_LOG_BUF_LEN - len, "CHG_STATUS=0x%llX,", val);
	get_chgr_reg(CHG_INT_RT_STS, &val);
	len += scnprintf(batt_log_buf + len, BATT_LOG_BUF_LEN - len, "CHG_INT_RT_STS=0x%llX,", val);

	
	get_bat_if_reg(BAT_IF_PRES_STATUS_REG, &val);
	len += scnprintf(batt_log_buf + len, BATT_LOG_BUF_LEN - len, "PRES_STATUS=0x%llX,", val);
	get_bat_if_reg(BAT_IF_TEMP_STATUS_REG, &val);
	len += scnprintf(batt_log_buf + len, BATT_LOG_BUF_LEN - len, "TEMP_STATUS=0x%llX,", val);
	get_bat_if_reg(BAT_IF_BPD_CTRL_REG, &val);
	len += scnprintf(batt_log_buf + len, BATT_LOG_BUF_LEN - len, "BPD_CTRL=0x%llX,", val);
	get_bat_if_reg(BAT_IF_BTC_CTRL, &val);
	len += scnprintf(batt_log_buf + len, BATT_LOG_BUF_LEN - len, "BTC_CTRL=0x%llX,", val);
	get_bat_if_reg(BAT_IF_INT_RT_STS_REG, &val);
	len += scnprintf(batt_log_buf + len, BATT_LOG_BUF_LEN - len, "IF_INT_RT_STS=0x%llX,", val);

	
	get_usb_chgpth_reg(USB_PTH_STS_REG, &val);
	len += scnprintf(batt_log_buf + len, BATT_LOG_BUF_LEN - len, "USB_CHG_PTH_STS=0x%llX,", val);
	get_usb_chgpth_reg(USB_OVP_CTL_REG, &val);
	len += scnprintf(batt_log_buf + len, BATT_LOG_BUF_LEN - len, "USB_OVP_CTL=0x%llX,", val);
	get_usb_chgpth_reg(USB_SUSP_REG, &val);
	len += scnprintf(batt_log_buf + len, BATT_LOG_BUF_LEN - len, "USB_SUSP=0x%llX,", val);
	get_usb_chgpth_reg(USB_ENUM_TIMER_STOP_REG, &val);
	len += scnprintf(batt_log_buf + len, BATT_LOG_BUF_LEN - len, "ENUM_TIMER_STOP=0x%llX,", val);
	get_usb_chgpth_reg(USB_ENUM_TIMER_REG, &val);
	len += scnprintf(batt_log_buf + len, BATT_LOG_BUF_LEN - len, "ENUM_TIMER=0x%llX,", val);
	get_usb_chgpth_reg(USB_INT_RT_STS, &val);
	len += scnprintf(batt_log_buf + len, BATT_LOG_BUF_LEN - len, "USB_INT_RT_STS=0x%llX,", val);

	
	get_misc_reg(MISC_BOOT_DONE_REG, &val);
	len += scnprintf(batt_log_buf + len, BATT_LOG_BUF_LEN - len, "BOOT_DONE=0x%llX,", val);
	get_misc_reg(MISC_TRIM3_REG, &val);
	len += scnprintf(batt_log_buf + len, BATT_LOG_BUF_LEN - len, "TRIM3_REG=0x%llX,", val);
	get_misc_reg(MISC_TRIM4_REG, &val);
	len += scnprintf(batt_log_buf + len, BATT_LOG_BUF_LEN - len, "TRIM4_REG=0x%llX", val);

	if(BATT_LOG_BUF_LEN - len <= 1)
		pr_warn("batt log length maybe out of buffer range!!!");

	pr_info("%s\n", batt_log_buf);
}
#endif

static int qpnp_lbc_probe(struct spmi_device *spmi)
{
	u8 subtype;
#ifndef CONFIG_QPNP_LINEAR_CHARGER_SMB1360
	ktime_t kt;
#endif
	struct qpnp_lbc_chip *chip;
	struct resource *resource;
	struct spmi_resource *spmi_resource;
#ifndef CONFIG_QPNP_LINEAR_CHARGER_SMB1360
	struct power_supply *usb_psy;
#endif
	int rc = 0;

#ifndef CONFIG_QPNP_LINEAR_CHARGER_SMB1360
	usb_psy = power_supply_get_by_name("usb");
	if (!usb_psy) {
		pr_err("usb supply not found deferring probe\n");
		return -EPROBE_DEFER;
	}
#endif

	chip = devm_kzalloc(&spmi->dev, sizeof(struct qpnp_lbc_chip),
				GFP_KERNEL);
	if (!chip) {
		pr_err("memory allocation failed.\n");
		return -ENOMEM;
	}

#ifndef CONFIG_QPNP_LINEAR_CHARGER_SMB1360
	chip->usb_psy = usb_psy;
#endif
	chip->dev = &spmi->dev;
	chip->spmi = spmi;
	chip->fake_battery_soc = -EINVAL;
	dev_set_drvdata(&spmi->dev, chip);
	device_init_wakeup(&spmi->dev, 1);
	mutex_init(&chip->jeita_configure_lock);
	mutex_init(&chip->chg_enable_lock);
	spin_lock_init(&chip->hw_access_lock);
	spin_lock_init(&chip->ibat_change_lock);
	spin_lock_init(&chip->irq_lock);
#ifndef CONFIG_QPNP_LINEAR_CHARGER_SMB1360
	INIT_WORK(&chip->vddtrim_work, qpnp_lbc_vddtrim_work_fn);
	alarm_init(&chip->vddtrim_alarm, ALARM_REALTIME, vddtrim_callback);
#endif

	
	rc = qpnp_charger_read_dt_props(chip);
	if (rc) {
		pr_err("Failed to read DT properties rc=%d\n", rc);
		return rc;
	}

	spmi_for_each_container_dev(spmi_resource, spmi) {
		if (!spmi_resource) {
			pr_err("spmi resource absent\n");
			rc = -ENXIO;
			goto fail_chg_enable;
		}

		resource = spmi_get_resource(spmi, spmi_resource,
							IORESOURCE_MEM, 0);
		if (!(resource && resource->start)) {
			pr_err("node %s IO resource absent!\n",
						spmi->dev.of_node->full_name);
			rc = -ENXIO;
			goto fail_chg_enable;
		}

		rc = qpnp_lbc_read(chip, resource->start + PERP_SUBTYPE_REG,
					&subtype, 1);
		if (rc) {
			pr_err("Peripheral subtype read failed rc=%d\n", rc);
			goto fail_chg_enable;
		}

		switch (subtype) {
		case LBC_CHGR_SUBTYPE:
			chip->chgr_base = resource->start;

			
			rc = qpnp_lbc_get_irqs(chip, subtype, spmi_resource);
			if (rc) {
				pr_err("Failed to get CHGR irqs rc=%d\n", rc);
				goto fail_chg_enable;
			}
			break;
		case LBC_USB_PTH_SUBTYPE:
			chip->usb_chgpth_base = resource->start;
			rc = qpnp_lbc_get_irqs(chip, subtype, spmi_resource);
			if (rc) {
				pr_err("Failed to get USB_PTH irqs rc=%d\n",
						rc);
				goto fail_chg_enable;
			}
			break;
		case LBC_BAT_IF_SUBTYPE:
			chip->bat_if_base = resource->start;
			chip->vadc_dev = qpnp_get_vadc(chip->dev, "chg");
			if (IS_ERR(chip->vadc_dev)) {
				rc = PTR_ERR(chip->vadc_dev);
				if (rc != -EPROBE_DEFER)
					pr_err("vadc prop missing rc=%d\n",
							rc);
				goto fail_chg_enable;
			}
			
			rc = qpnp_lbc_get_irqs(chip, subtype, spmi_resource);
			if (rc) {
				pr_err("Failed to get BAT_IF irqs rc=%d\n", rc);
				goto fail_chg_enable;
			}
			break;
		case LBC_MISC_SUBTYPE:
			chip->misc_base = resource->start;
			break;
		default:
			pr_err("Invalid peripheral subtype=0x%x\n", subtype);
			rc = -EINVAL;
		}
	}

	if (chip->cfg_use_external_charger) {
		pr_warn("Disabling Linear Charger (e-external-charger = 1)\n");
		rc = qpnp_disable_lbc_charger(chip);
		if (rc)
			pr_err("Unable to disable charger rc=%d\n", rc);
		return -ENODEV;
	}

	
	rc = qpnp_lbc_misc_init(chip);
	if (rc) {
		pr_err("unable to initialize LBC MISC rc=%d\n", rc);
		return rc;
	}
	rc = qpnp_lbc_chg_init(chip);
	if (rc) {
		pr_err("unable to initialize LBC charger rc=%d\n", rc);
		return rc;
	}
#ifndef CONFIG_QPNP_LINEAR_CHARGER_SMB1360
	rc = qpnp_lbc_bat_if_init(chip);
	if (rc) {
		pr_err("unable to initialize LBC BAT_IF rc=%d\n", rc);
		return rc;
	}
	rc = qpnp_lbc_usb_path_init(chip);
	if (rc) {
		pr_err("unable to initialize LBC USB path rc=%d\n", rc);
		return rc;
	}

	if (chip->bat_if_base) {
		chip->batt_present = qpnp_lbc_is_batt_present(chip);
		chip->batt_psy.name = "battery";
		chip->batt_psy.type = POWER_SUPPLY_TYPE_BATTERY;
		chip->batt_psy.properties = msm_batt_power_props;
		chip->batt_psy.num_properties =
			ARRAY_SIZE(msm_batt_power_props);
		chip->batt_psy.get_property = qpnp_batt_power_get_property;
		chip->batt_psy.set_property = qpnp_batt_power_set_property;
		chip->batt_psy.property_is_writeable =
			qpnp_batt_property_is_writeable;
		chip->batt_psy.external_power_changed =
			qpnp_batt_external_power_changed;
		chip->batt_psy.supplied_to = pm_batt_supplied_to;
		chip->batt_psy.num_supplicants =
			ARRAY_SIZE(pm_batt_supplied_to);
		rc = power_supply_register(chip->dev, &chip->batt_psy);
		if (rc < 0) {
			pr_err("batt failed to register rc=%d\n", rc);
			goto fail_chg_enable;
		}
	}

	if ((chip->cfg_cool_bat_decidegc || chip->cfg_warm_bat_decidegc)
			&& chip->bat_if_base) {
		chip->adc_param.low_temp = chip->cfg_cool_bat_decidegc;
		chip->adc_param.high_temp = chip->cfg_warm_bat_decidegc;
		chip->adc_param.timer_interval = ADC_MEAS1_INTERVAL_1S;
		chip->adc_param.state_request = ADC_TM_HIGH_LOW_THR_ENABLE;
		chip->adc_param.btm_ctx = chip;
		chip->adc_param.threshold_notification =
			qpnp_lbc_jeita_adc_notification;
		chip->adc_param.channel = LR_MUX1_BATT_THERM;

		if (get_prop_batt_present(chip)) {
			rc = qpnp_adc_tm_channel_measure(chip->adc_tm_dev,
					&chip->adc_param);
			if (rc) {
				pr_err("request ADC error rc=%d\n", rc);
				goto unregister_batt;
			}
		}
	}
#endif

	rc = qpnp_lbc_bat_if_configure_btc(chip);
	if (rc) {
		pr_err("Failed to configure btc rc=%d\n", rc);
		goto unregister_batt;
	}

#ifndef CONFIG_QPNP_LINEAR_CHARGER_SMB1360
	
	determine_initial_status(chip);

	rc = qpnp_lbc_request_irqs(chip);
	if (rc) {
		pr_err("unable to initialize LBC MISC rc=%d\n", rc);
		goto unregister_batt;
	}

	if (chip->cfg_charging_disabled && !get_prop_batt_present(chip))
		pr_info("Battery absent and charging disabled !!!\n");

	
	if ((chip->supported_feature_flag & VDD_TRIM_SUPPORTED) &&
			qpnp_lbc_is_fastchg_on(chip)) {
		kt = ns_to_ktime(TRIM_PERIOD_NS);
		alarm_start_relative(&chip->vddtrim_alarm, kt);
	}
#endif

#ifdef CONFIG_QPNP_LINEAR_CHARGER_SMB1360
	the_chip = chip;
#endif

	pr_info("Probe chg_dis=%d bpd=%d usb=%d batt_pres=%d batt_volt=%d soc=%d\n",
			chip->cfg_charging_disabled,
			chip->cfg_bpd_detection,
			qpnp_lbc_is_usb_chg_plugged_in(chip),
			get_prop_batt_present(chip),
			get_prop_battery_voltage_now(chip),
			get_prop_capacity(chip));

	return 0;
unregister_batt:
	if (chip->bat_if_base)
		power_supply_unregister(&chip->batt_psy);
fail_chg_enable:
	dev_set_drvdata(&spmi->dev, NULL);
	return rc;
}

static int qpnp_lbc_remove(struct spmi_device *spmi)
{
	struct qpnp_lbc_chip *chip = dev_get_drvdata(&spmi->dev);

	if (chip->supported_feature_flag & VDD_TRIM_SUPPORTED) {
		alarm_cancel(&chip->vddtrim_alarm);
		cancel_work_sync(&chip->vddtrim_work);
	}
	if (chip->bat_if_base)
		power_supply_unregister(&chip->batt_psy);
	mutex_destroy(&chip->jeita_configure_lock);
	mutex_destroy(&chip->chg_enable_lock);
	dev_set_drvdata(&spmi->dev, NULL);
	return 0;
}

static struct of_device_id qpnp_lbc_match_table[] = {
	{ .compatible = QPNP_CHARGER_DEV_NAME, },
	{}
};

static struct spmi_driver qpnp_lbc_driver = {
	.probe		= qpnp_lbc_probe,
	.remove		= qpnp_lbc_remove,
	.driver		= {
		.name		= QPNP_CHARGER_DEV_NAME,
		.owner		= THIS_MODULE,
		.of_match_table	= qpnp_lbc_match_table,
	},
};

static int __init qpnp_lbc_init(void)
{
	return spmi_driver_register(&qpnp_lbc_driver);
}
module_init(qpnp_lbc_init);

static void __exit qpnp_lbc_exit(void)
{
	spmi_driver_unregister(&qpnp_lbc_driver);
}
module_exit(qpnp_lbc_exit);

MODULE_DESCRIPTION("QPNP Linear charger driver");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:" QPNP_CHARGER_DEV_NAME);
