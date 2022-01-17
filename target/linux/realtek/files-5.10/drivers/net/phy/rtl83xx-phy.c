// SPDX-License-Identifier: GPL-2.0-only
/* Realtek RTL838X Ethernet MDIO interface driver
 *
 * Copyright (C) 2020 B. Koblitz
 */

#include <linux/module.h>
#include <linux/delay.h>
#include <linux/phy.h>
#include <linux/netdevice.h>
#include <linux/firmware.h>
#include <linux/crc32.h>

#include <asm/mach-rtl838x/mach-rtl83xx.h>
#include "rtl83xx-phy.h"

extern struct rtl83xx_soc_info soc_info;
extern struct mutex smi_lock;

#define PHY_CTRL_REG	0
#define PHY_POWER_BIT	11

#define PHY_PAGE_2	2
#define PHY_PAGE_4	4
#define PARK_PAGE	0x1f

#define RTL9300_PHY_ID_MASK 0xf0ffffff

/*
 * This lock protects the state of the SoC automatically polling the PHYs over the SMI
 * bus to detect e.g. link and media changes. For operations on the PHYs such as
 * patching or other configuration changes such as EEE, polling needs to be disabled
 * since otherwise these operations may fails or lead to unpredictable results.
 */
DEFINE_MUTEX(poll_lock);

static const struct firmware rtl838x_8380_fw;
static const struct firmware rtl838x_8214fc_fw;
static const struct firmware rtl838x_8218b_fw;

int rtl838x_read_mmd_phy(u32 port, u32 devnum, u32 regnum, u32 *val);
int rtl838x_write_mmd_phy(u32 port, u32 devnum, u32 reg, u32 val);
int rtl839x_read_mmd_phy(u32 port, u32 devnum, u32 regnum, u32 *val);
int rtl839x_write_mmd_phy(u32 port, u32 devnum, u32 reg, u32 val);
int rtl930x_read_mmd_phy(u32 port, u32 devnum, u32 regnum, u32 *val);
int rtl930x_write_mmd_phy(u32 port, u32 devnum, u32 reg, u32 val);
int rtl931x_read_mmd_phy(u32 port, u32 devnum, u32 regnum, u32 *val);
int rtl931x_write_mmd_phy(u32 port, u32 devnum, u32 reg, u32 val);

static int read_phy(u32 port, u32 page, u32 reg, u32 *val)
{	switch (soc_info.family) {
	case RTL8380_FAMILY_ID:
		return rtl838x_read_phy(port, page, reg, val);
	case RTL8390_FAMILY_ID:
		return rtl839x_read_phy(port, page, reg, val);
	case RTL9300_FAMILY_ID:
		return rtl930x_read_phy(port, page, reg, val);
	case RTL9310_FAMILY_ID:
		return rtl931x_read_phy(port, page, reg, val);
	}
	return -1;
}

static int write_phy(u32 port, u32 page, u32 reg, u32 val)
{
	switch (soc_info.family) {
	case RTL8380_FAMILY_ID:
		return rtl838x_write_phy(port, page, reg, val);
	case RTL8390_FAMILY_ID:
		return rtl839x_write_phy(port, page, reg, val);
	case RTL9300_FAMILY_ID:
		return rtl930x_write_phy(port, page, reg, val);
	case RTL9310_FAMILY_ID:
		return rtl931x_write_phy(port, page, reg, val);
	}
	return -1;
}

static int read_mmd_phy(u32 port, u32 devnum, u32 regnum, u32 *val)
{
	switch (soc_info.family) {
	case RTL8380_FAMILY_ID:
		return rtl838x_read_mmd_phy(port, devnum, regnum, val);
	case RTL8390_FAMILY_ID:
		return rtl839x_read_mmd_phy(port, devnum, regnum, val);
	case RTL9300_FAMILY_ID:
		return rtl930x_read_mmd_phy(port, devnum, regnum, val);
	case RTL9310_FAMILY_ID:
		return rtl931x_read_mmd_phy(port, devnum, regnum, val);
	}
	return -1;
}

int write_mmd_phy(u32 port, u32 devnum, u32 reg, u32 val)
{
	switch (soc_info.family) {
	case RTL8380_FAMILY_ID:
		return rtl838x_write_mmd_phy(port, devnum, reg, val);
	case RTL8390_FAMILY_ID:
		return rtl839x_write_mmd_phy(port, devnum, reg, val);
	case RTL9300_FAMILY_ID:
		return rtl930x_write_mmd_phy(port, devnum, reg, val);
	case RTL9310_FAMILY_ID:
		return rtl931x_write_mmd_phy(port, devnum, reg, val);
	}
	return -1;
}

static u64 disable_polling(int port)
{
	u64 saved_state;

	mutex_lock(&poll_lock);

	switch (soc_info.family) {
	case RTL8380_FAMILY_ID:
		saved_state = sw_r32(RTL838X_SMI_POLL_CTRL);
		sw_w32_mask(BIT(port), 0, RTL838X_SMI_POLL_CTRL);
		break;
	case RTL8390_FAMILY_ID:
		saved_state = sw_r32(RTL839X_SMI_PORT_POLLING_CTRL + 4);
		saved_state <<= 32;
		saved_state |= sw_r32(RTL839X_SMI_PORT_POLLING_CTRL);
		sw_w32_mask(BIT(port % 32), 0,
			    RTL839X_SMI_PORT_POLLING_CTRL + ((port >> 5) << 2));
		break;
	case RTL9300_FAMILY_ID:
		saved_state = sw_r32(RTL930X_SMI_POLL_CTRL);
		sw_w32_mask(BIT(port), 0, RTL930X_SMI_POLL_CTRL);
		break;
	case RTL9310_FAMILY_ID:
		pr_warn("%s not implemented for RTL931X\n", __func__);
		break;
	}

	mutex_unlock(&poll_lock);

	return saved_state;
}

static int resume_polling(u64 saved_state)
{
	mutex_lock(&poll_lock);

	switch (soc_info.family) {
	case RTL8380_FAMILY_ID:
		sw_w32(saved_state, RTL838X_SMI_POLL_CTRL);
		break;
	case RTL8390_FAMILY_ID:
		sw_w32(saved_state >> 32, RTL839X_SMI_PORT_POLLING_CTRL + 4);
		sw_w32(saved_state, RTL839X_SMI_PORT_POLLING_CTRL);
		break;
	case RTL9300_FAMILY_ID:
		sw_w32(saved_state, RTL930X_SMI_POLL_CTRL);
		break;
	case RTL9310_FAMILY_ID:
		pr_warn("%s not implemented for RTL931X\n", __func__);
		break;
	}

	mutex_unlock(&poll_lock);

	return 0;
}

static void rtl8380_int_phy_on_off(int mac, bool on)
{
	u32 val;

	read_phy(mac, 0, 0, &val);
	if (on)
		write_phy(mac, 0, 0, val & ~BIT(11));
	else
		write_phy(mac, 0, 0, val | BIT(11));
}

static void rtl8380_rtl8214fc_on_off(int mac, bool on)
{
	u32 val;

	/* fiber ports */
	write_phy(mac, 4095, 30, 3);
	read_phy(mac, 0, 16, &val);
	if (on)
		write_phy(mac, 0, 16, val & ~BIT(11));
	else
		write_phy(mac, 0, 16, val | BIT(11));

	/* copper ports */
	write_phy(mac, 4095, 30, 1);
	read_phy(mac, 0, 16, &val);
	if (on)
		write_phy(mac, 0xa40, 16, val & ~BIT(11));
	else
		write_phy(mac, 0xa40, 16, val | BIT(11));
}

static void rtl8380_phy_reset(int mac)
{
	u32 val;

	read_phy(mac, 0, 0, &val);
	write_phy(mac, 0, 0, val | BIT(15));
}

/*
 * Reset the SerDes by powering it off and set a new operations mode
 * of the SerDes. 0x1f is off. Other modes are
 * 0x01: QSGMII		0x04: 1000BX_FIBER	0x05: FIBER100
 * 0x06: QSGMII		0x09: RSGMII		0x0d: USXGMII
 * 0x10: XSGMII		0x12: HISGMII		0x16: 2500Base_X
 * 0x17: RXAUI_LITE	0x19: RXAUI_PLUS	0x1a: 10G Base-R
 * 0x1b: 10GR1000BX_AUTO			0x1f: OFF
 */
void rtl9300_sds_rst(int sds_num, u32 mode)
{
	// The access registers for SDS_MODE_SEL and the LSB for each SDS within
	u16 regs[] = { 0x0194, 0x0194, 0x0194, 0x0194, 0x02a0, 0x02a0, 0x02a0, 0x02a0,
		       0x02A4, 0x02A4, 0x0198, 0x0198 };
	u8  lsb[]  = { 0, 6, 12, 18, 0, 6, 12, 18, 0, 6, 0, 6};

	pr_info("%s %d\n", __func__, mode);
	if (sds_num < 0 || sds_num > 11) {
		pr_err("Wrong SerDes number: %d\n", sds_num);
		return;
	}

	sw_w32_mask(0x1f << lsb[sds_num], 0x1f << lsb[sds_num], regs[sds_num]);
	mdelay(10);

	sw_w32_mask(0x1f << lsb[sds_num], mode << lsb[sds_num], regs[sds_num]);
	mdelay(10);

	pr_debug("%s: 194:%08x 198:%08x 2a0:%08x 2a4:%08x\n", __func__,
		sw_r32(0x194), sw_r32(0x198), sw_r32(0x2a0), sw_r32(0x2a4));
}

/*
 * On the RTL839x family of SoCs with inbuilt SerDes, these SerDes are accessed through
 * a 2048 bit register that holds the contents of the PHY being simulated by the SoC.
 */
int rtl839x_read_sds_phy(int phy_addr, int phy_reg)
{
	int offset = 0;
	int reg;
	u32 val;

	if (phy_addr == 49)
		offset = 0x100;

	/*
	 * For the RTL8393 internal SerDes, we simulate a PHY ID in registers 2/3
	 * which would otherwise read as 0.
	 */
	if (soc_info.id == 0x8393) {
		if (phy_reg == 2)
			return 0x1c;
		if (phy_reg == 3)
			return 0x8393;
	}

	/*
	 * Register RTL839X_SDS12_13_XSG0 is 2048 bit broad, the MSB (bit 15) of the
	 * 0th PHY register is bit 1023 (in byte 0x80). Because PHY-registers are 16
	 * bit broad, we offset by reg << 1. In the SoC 2 registers are stored in
	 * one 32 bit register.
	 */
	reg = (phy_reg << 1) & 0xfc;
	val = sw_r32(RTL839X_SDS12_13_XSG0 + offset + 0x80 + reg);

	if (phy_reg & 1)
		val = (val >> 16) & 0xffff;
	else
		val &= 0xffff;
	return val;
}

/*
 * On the RTL930x family of SoCs, the internal SerDes are accessed through an IO
 * register which simulates commands to an internal MDIO bus.
 */
int rtl930x_read_sds_phy(int phy_addr, int page, int phy_reg)
{
	int i;
	u32 cmd = phy_addr << 2 | page << 7 | phy_reg << 13 | 1;

	pr_debug("%s: phy_addr %d, phy_reg: %d\n", __func__, phy_addr, phy_reg);
	sw_w32(cmd, RTL930X_SDS_INDACS_CMD);

	for (i = 0; i < 100; i++) {
		if (!(sw_r32(RTL930X_SDS_INDACS_CMD) & 0x1))
			break;
		mdelay(1);
	}

	if (i >= 100)
		return -EIO;

	pr_debug("%s: returning %04x\n", __func__, sw_r32(RTL930X_SDS_INDACS_DATA) & 0xffff);
	return sw_r32(RTL930X_SDS_INDACS_DATA) & 0xffff;
}

int rtl930x_write_sds_phy(int phy_addr, int page, int phy_reg, u16 v)
{
	int i;
	u32 cmd;

	sw_w32(v, RTL930X_SDS_INDACS_DATA);
	cmd = phy_addr << 2 | page << 7 | phy_reg << 13 | 0x3;

	for (i = 0; i < 100; i++) {
		if (!(sw_r32(RTL930X_SDS_INDACS_CMD) & 0x1))
			break;
		mdelay(1);
	}

	if (i >= 100)
		return -EIO;

	return 0;
}

int rtl931x_read_sds_phy(int phy_addr, int page, int phy_reg)
{
	int i;
	u32 cmd = phy_addr << 2 | page << 7 | phy_reg << 13 | 1;

	pr_debug("%s: phy_addr(SDS-ID) %d, phy_reg: %d\n", __func__, phy_addr, phy_reg);
	sw_w32(cmd, RTL931X_SERDES_INDRT_ACCESS_CTRL);

	for (i = 0; i < 100; i++) {
		if (!(sw_r32(RTL931X_SERDES_INDRT_ACCESS_CTRL) & 0x1))
			break;
		mdelay(1);
	}

	if (i >= 100)
		return -EIO;

	pr_debug("%s: returning %04x\n", __func__, sw_r32(RTL931X_SERDES_INDRT_DATA_CTRL) & 0xffff);
	return sw_r32(RTL931X_SERDES_INDRT_DATA_CTRL) & 0xffff;
}

int rtl931x_write_sds_phy(int phy_addr, int page, int phy_reg, u16 v)
{
	int i;
	u32 cmd;

	cmd = phy_addr << 2 | page << 7 | phy_reg << 13;
	sw_w32(cmd, RTL931X_SERDES_INDRT_ACCESS_CTRL);

	sw_w32(v, RTL931X_SERDES_INDRT_DATA_CTRL);
		
	cmd =  sw_r32(RTL931X_SERDES_INDRT_ACCESS_CTRL) | 0x3;
	sw_w32(cmd, RTL931X_SERDES_INDRT_ACCESS_CTRL);

	for (i = 0; i < 100; i++) {
		if (!(sw_r32(RTL931X_SERDES_INDRT_ACCESS_CTRL) & 0x1))
			break;
		mdelay(1);
	}

	if (i >= 100)
		return -EIO;

	return 0;
}

/*
 * On the RTL838x SoCs, the internal SerDes is accessed through direct access to
 * standard PHY registers, where a 32 bit register holds a 16 bit word as found
 * in a standard page 0 of a PHY
 */
int rtl838x_read_sds_phy(int phy_addr, int phy_reg)
{
	int offset = 0;
	u32 val;

	if (phy_addr == 26)
		offset = 0x100;
	val = sw_r32(RTL838X_SDS4_FIB_REG0 + offset + (phy_reg << 2)) & 0xffff;

	return val;
}

int rtl839x_write_sds_phy(int phy_addr, int phy_reg, u16 v)
{
	int offset = 0;
	int reg;
	u32 val;

	if (phy_addr == 49)
		offset = 0x100;

	reg = (phy_reg << 1) & 0xfc;
	val = v;
	if (phy_reg & 1) {
		val = val << 16;
		sw_w32_mask(0xffff0000, val,
			    RTL839X_SDS12_13_XSG0 + offset + 0x80 + reg);
	} else {
		sw_w32_mask(0xffff, val,
			    RTL839X_SDS12_13_XSG0 + offset + 0x80 + reg);
	}

	return 0;
}

/* Read the link and speed status of the 2 internal SGMII/1000Base-X
 * ports of the RTL838x SoCs
 */
static int rtl8380_read_status(struct phy_device *phydev)
{
	int err;

	err = genphy_read_status(phydev);

	if (phydev->link) {
		phydev->speed = SPEED_1000;
		phydev->duplex = DUPLEX_FULL;
	}

	return err;
}

/* Read the link and speed status of the 2 internal SGMII/1000Base-X
 * ports of the RTL8393 SoC
 */
static int rtl8393_read_status(struct phy_device *phydev)
{
	int offset = 0;
	int err;
	int phy_addr = phydev->mdio.addr;
	u32 v;

	err = genphy_read_status(phydev);
	if (phy_addr == 49)
		offset = 0x100;

	if (phydev->link) {
		phydev->speed = SPEED_100;
		/* Read SPD_RD_00 (bit 13) and SPD_RD_01 (bit 6) out of the internal
		 * PHY registers
		 */
		v = sw_r32(RTL839X_SDS12_13_XSG0 + offset + 0x80);
		if (!(v & (1 << 13)) && (v & (1 << 6)))
			phydev->speed = SPEED_1000;
		phydev->duplex = DUPLEX_FULL;
	}

	return err;
}

static int rtl8226_read_page(struct phy_device *phydev)
{
	return __phy_read(phydev, 0x1f);
}

static int rtl8226_write_page(struct phy_device *phydev, int page)
{
	return __phy_write(phydev, 0x1f, page);
}

static int rtl8226_read_status(struct phy_device *phydev)
{
	int ret = 0, i;
	u32 val;
	int port = phydev->mdio.addr;

// TODO: ret = genphy_read_status(phydev);
// 	if (ret < 0) {
// 		pr_info("%s: genphy_read_status failed\n", __func__);
// 		return ret;
// 	}

	// Link status must be read twice
	for (i = 0; i < 2; i++) {
		read_mmd_phy(port, MMD_VEND2, 0xA402, &val);
	}
	phydev->link = val & BIT(2) ? 1 : 0;
	if (!phydev->link)
		goto out;

	// Read duplex status
	ret = read_mmd_phy(port, MMD_VEND2, 0xA434, &val);
	if (ret)
		goto out;
	phydev->duplex = !!(val & BIT(3));

	// Read speed
	ret = read_mmd_phy(port, MMD_VEND2, 0xA434, &val);
	switch (val & 0x0630) {
	case 0x0000:
		phydev->speed = SPEED_10;
		break;
	case 0x0010:
		phydev->speed = SPEED_100;
		break;
	case 0x0020:
		phydev->speed = SPEED_1000;
		break;
	case 0x0200:
		phydev->speed = SPEED_10000;
		break;
	case 0x0210:
		phydev->speed = SPEED_2500;
		break;
	case 0x0220:
		phydev->speed = SPEED_5000;
		break;
	default:
		break;
	}
out:
	return ret;
}

static int rtl8226_advertise_aneg(struct phy_device *phydev)
{
	int ret = 0;
	u32 v;
	int port = phydev->mdio.addr;

	pr_info("In %s\n", __func__);

	ret = read_mmd_phy(port, MMD_AN, 16, &v);
	if (ret)
		goto out;

	v |= BIT(5); // HD 10M
	v |= BIT(6); // FD 10M
	v |= BIT(7); // HD 100M
	v |= BIT(8); // FD 100M

	ret = write_mmd_phy(port, MMD_AN, 16, v);

	// Allow 1GBit
	ret = read_mmd_phy(port, MMD_VEND2, 0xA412, &v);
	if (ret)
		goto out;
	v |= BIT(9); // FD 1000M

	ret = write_mmd_phy(port, MMD_VEND2, 0xA412, v);
	if (ret)
		goto out;

	// Allow 2.5G
	ret = read_mmd_phy(port, MMD_AN, 32, &v);
	if (ret)
		goto out;

	v |= BIT(7);
	ret = write_mmd_phy(port, MMD_AN, 32, v);

out:
	return ret;
}

static int rtl8226_config_aneg(struct phy_device *phydev)
{
	int ret = 0;
	u32 v;
	int port = phydev->mdio.addr;

	pr_debug("In %s\n", __func__);
	if (phydev->autoneg == AUTONEG_ENABLE) {
		ret = rtl8226_advertise_aneg(phydev);
		if (ret)
			goto out;
		// AutoNegotiationEnable
		ret = read_mmd_phy(port, MMD_AN, 0, &v);
		if (ret)
			goto out;

		v |= BIT(12); // Enable AN
		ret = write_mmd_phy(port, MMD_AN, 0, v);
		if (ret)
			goto out;

		// RestartAutoNegotiation
		ret = read_mmd_phy(port, MMD_VEND2, 0xA400, &v);
		if (ret)
			goto out;
		v |= BIT(9);

		ret = write_mmd_phy(port, MMD_VEND2, 0xA400, v);
	}

//	TODO: ret = __genphy_config_aneg(phydev, ret);

out:
	return ret;
}

static int rtl8226_get_eee(struct phy_device *phydev,
				     struct ethtool_eee *e)
{
	u32 val;
	int addr = phydev->mdio.addr;

	pr_debug("In %s, port %d, was enabled: %d\n", __func__, addr, e->eee_enabled);

	read_mmd_phy(addr, MMD_AN, 60, &val);
	if (e->eee_enabled) {
		e->eee_enabled = !!(val & BIT(1));
		if (!e->eee_enabled) {
			read_mmd_phy(addr, MMD_AN, 62, &val);
			e->eee_enabled = !!(val & BIT(0));
		}
	}
	pr_debug("%s: enabled: %d\n", __func__, e->eee_enabled);

	return 0;
}

static int rtl8226_set_eee(struct phy_device *phydev, struct ethtool_eee *e)
{
	int port = phydev->mdio.addr;
	u64 poll_state;
	bool an_enabled;
	u32 val;

	pr_info("In %s, port %d, enabled %d\n", __func__, port, e->eee_enabled);

	poll_state = disable_polling(port);

	// Remember aneg state
	read_mmd_phy(port, MMD_AN, 0, &val);
	an_enabled = !!(val & BIT(12));

	// Setup 100/1000MBit
	read_mmd_phy(port, MMD_AN, 60, &val);
	if (e->eee_enabled)
		val |= 0x6;
	else
		val &= 0x6;
	write_mmd_phy(port, MMD_AN, 60, val);

	// Setup 2.5GBit
	read_mmd_phy(port, MMD_AN, 62, &val);
	if (e->eee_enabled)
		val |= 0x1;
	else
		val &= 0x1;
	write_mmd_phy(port, MMD_AN, 62, val);

	// RestartAutoNegotiation
	read_mmd_phy(port, MMD_VEND2, 0xA400, &val);
	val |= BIT(9);
	write_mmd_phy(port, MMD_VEND2, 0xA400, val);

	resume_polling(poll_state);

	return 0;
}

static struct fw_header *rtl838x_request_fw(struct phy_device *phydev,
					    const struct firmware *fw,
					    const char *name)
{
	struct device *dev = &phydev->mdio.dev;
	int err;
	struct fw_header *h;
	uint32_t checksum, my_checksum;

	err = request_firmware(&fw, name, dev);
	if (err < 0)
		goto out;

	if (fw->size < sizeof(struct fw_header)) {
		pr_err("Firmware size too small.\n");
		err = -EINVAL;
		goto out;
	}

	h = (struct fw_header *) fw->data;
	pr_info("Firmware loaded. Size %d, magic: %08x\n", fw->size, h->magic);

	if (h->magic != 0x83808380) {
		pr_err("Wrong firmware file: MAGIC mismatch.\n");
		goto out;
	}

	checksum = h->checksum;
	h->checksum = 0;
	my_checksum = ~crc32(0xFFFFFFFFU, fw->data, fw->size);
	if (checksum != my_checksum) {
		pr_err("Firmware checksum mismatch.\n");
		err = -EINVAL;
		goto out;
	}
	h->checksum = checksum;

	return h;
out:
	dev_err(dev, "Unable to load firmware %s (%d)\n", name, err);
	return NULL;
}

static int rtl8390_configure_generic(struct phy_device *phydev)
{
	u32 val, phy_id;
	int mac = phydev->mdio.addr;

	read_phy(mac, 0, 2, &val);
	phy_id = val << 16;
	read_phy(mac, 0, 3, &val);
	phy_id |= val;
	pr_debug("Phy on MAC %d: %x\n", mac, phy_id);

	/* Read internal PHY ID */
	write_phy(mac, 31, 27, 0x0002);
	read_phy(mac, 31, 28, &val);

	/* Internal RTL8218B, version 2 */
	phydev_info(phydev, "Detected unknown %x\n", val);
	return 0;
}

static int rtl8380_configure_int_rtl8218b(struct phy_device *phydev)
{
	u32 val, phy_id;
	int i, p, ipd_flag;
	int mac = phydev->mdio.addr;
	struct fw_header *h;
	u32 *rtl838x_6275B_intPhy_perport;
	u32 *rtl8218b_6276B_hwEsd_perport;


	read_phy(mac, 0, 2, &val);
	phy_id = val << 16;
	read_phy(mac, 0, 3, &val);
	phy_id |= val;
	pr_debug("Phy on MAC %d: %x\n", mac, phy_id);

	/* Read internal PHY ID */
	write_phy(mac, 31, 27, 0x0002);
	read_phy(mac, 31, 28, &val);
	if (val != 0x6275) {
		phydev_err(phydev, "Expected internal RTL8218B, found PHY-ID %x\n", val);
		return -1;
	}

	/* Internal RTL8218B, version 2 */
	phydev_info(phydev, "Detected internal RTL8218B\n");

	h = rtl838x_request_fw(phydev, &rtl838x_8380_fw, FIRMWARE_838X_8380_1);
	if (!h)
		return -1;

	if (h->phy != 0x83800000) {
		phydev_err(phydev, "Wrong firmware file: PHY mismatch.\n");
		return -1;
	}

	rtl838x_6275B_intPhy_perport = (void *)h + sizeof(struct fw_header)
			+ h->parts[8].start;

	rtl8218b_6276B_hwEsd_perport = (void *)h + sizeof(struct fw_header)
			+ h->parts[9].start;

	if (sw_r32(RTL838X_DMY_REG31) == 0x1)
		ipd_flag = 1;

	read_phy(mac, 0, 0, &val);
	if (val & (1 << 11))
		rtl8380_int_phy_on_off(mac, true);
	else
		rtl8380_phy_reset(mac);
	msleep(100);

	/* Ready PHY for patch */
	for (p = 0; p < 8; p++) {
		write_phy(mac + p, 0xfff, 0x1f, 0x0b82);
		write_phy(mac + p, 0xfff, 0x10, 0x0010);
	}
	msleep(500);
	for (p = 0; p < 8; p++) {
		for (i = 0; i < 100 ; i++) {
			read_phy(mac + p, 0x0b80, 0x10, &val);
			if (val & 0x40)
				break;
		}
		if (i >= 100) {
			phydev_err(phydev,
				   "ERROR: Port %d not ready for patch.\n",
				   mac + p);
			return -1;
		}
	}
	for (p = 0; p < 8; p++) {
		i = 0;
		while (rtl838x_6275B_intPhy_perport[i * 2]) {
			write_phy(mac + p, 0xfff,
				rtl838x_6275B_intPhy_perport[i * 2],
				rtl838x_6275B_intPhy_perport[i * 2 + 1]);
			i++;
		}
		i = 0;
		while (rtl8218b_6276B_hwEsd_perport[i * 2]) {
			write_phy(mac + p, 0xfff,
				rtl8218b_6276B_hwEsd_perport[i * 2],
				rtl8218b_6276B_hwEsd_perport[i * 2 + 1]);
			i++;
		}
	}
	return 0;
}

static int rtl8380_configure_ext_rtl8218b(struct phy_device *phydev)
{
	u32 val, ipd, phy_id;
	int i, l;
	int mac = phydev->mdio.addr;
	struct fw_header *h;
	u32 *rtl8380_rtl8218b_perchip;
	u32 *rtl8218B_6276B_rtl8380_perport;
	u32 *rtl8380_rtl8218b_perport;

	if (soc_info.family == RTL8380_FAMILY_ID && mac != 0 && mac != 16) {
		phydev_err(phydev, "External RTL8218B must have PHY-IDs 0 or 16!\n");
		return -1;
	}
	read_phy(mac, 0, 2, &val);
	phy_id = val << 16;
	read_phy(mac, 0, 3, &val);
	phy_id |= val;
	pr_info("Phy on MAC %d: %x\n", mac, phy_id);

	/* Read internal PHY ID */
	write_phy(mac, 31, 27, 0x0002);
	read_phy(mac, 31, 28, &val);
	if (val != 0x6276) {
		phydev_err(phydev, "Expected external RTL8218B, found PHY-ID %x\n", val);
		return -1;
	}
	phydev_info(phydev, "Detected external RTL8218B\n");

	h = rtl838x_request_fw(phydev, &rtl838x_8218b_fw, FIRMWARE_838X_8218b_1);
	if (!h)
		return -1;

	if (h->phy != 0x8218b000) {
		phydev_err(phydev, "Wrong firmware file: PHY mismatch.\n");
		return -1;
	}

	rtl8380_rtl8218b_perchip = (void *)h + sizeof(struct fw_header)
			+ h->parts[0].start;

	rtl8218B_6276B_rtl8380_perport = (void *)h + sizeof(struct fw_header)
			+ h->parts[1].start;

	rtl8380_rtl8218b_perport = (void *)h + sizeof(struct fw_header)
			+ h->parts[2].start;

	read_phy(mac, 0, 0, &val);
	if (val & (1 << 11))
		rtl8380_int_phy_on_off(mac, true);
	else
		rtl8380_phy_reset(mac);
	msleep(100);

	/* Get Chip revision */
	write_phy(mac, 0xfff, 0x1f, 0x0);
	write_phy(mac,  0xfff, 0x1b, 0x4);
	read_phy(mac, 0xfff, 0x1c, &val);

	i = 0;
	while (rtl8380_rtl8218b_perchip[i * 3]
		&& rtl8380_rtl8218b_perchip[i * 3 + 1]) {
		write_phy(mac + rtl8380_rtl8218b_perchip[i * 3],
					  0xfff, rtl8380_rtl8218b_perchip[i * 3 + 1],
					  rtl8380_rtl8218b_perchip[i * 3 + 2]);
		i++;
	}

	/* Enable PHY */
	for (i = 0; i < 8; i++) {
		write_phy(mac + i, 0xfff, 0x1f, 0x0000);
		write_phy(mac + i, 0xfff, 0x00, 0x1140);
	}
	mdelay(100);

	/* Request patch */
	for (i = 0; i < 8; i++) {
		write_phy(mac + i,  0xfff, 0x1f, 0x0b82);
		write_phy(mac + i,  0xfff, 0x10, 0x0010);
	}
	mdelay(300);

	/* Verify patch readiness */
	for (i = 0; i < 8; i++) {
		for (l = 0; l < 100; l++) {
			read_phy(mac + i, 0xb80, 0x10, &val);
			if (val & 0x40)
				break;
		}
		if (l >= 100) {
			phydev_err(phydev, "Could not patch PHY\n");
			return -1;
		}
	}

	/* Use Broadcast ID method for patching */
	write_phy(mac, 0xfff, 0x1f, 0x0000);
	write_phy(mac, 0xfff, 0x1d, 0x0008);
	write_phy(mac, 0xfff, 0x1f, 0x0266);
	write_phy(mac, 0xfff, 0x16, 0xff00 + mac);
	write_phy(mac, 0xfff, 0x1f, 0x0000);
	write_phy(mac, 0xfff, 0x1d, 0x0000);
	mdelay(1);

	write_phy(mac, 0xfff, 30, 8);
	write_phy(mac, 0x26e, 17, 0xb);
	write_phy(mac, 0x26e, 16, 0x2);
	mdelay(1);
	read_phy(mac, 0x26e, 19, &ipd);
	write_phy(mac, 0, 30, 0);
	ipd = (ipd >> 4) & 0xf;

	i = 0;
	while (rtl8218B_6276B_rtl8380_perport[i * 2]) {
		write_phy(mac, 0xfff, rtl8218B_6276B_rtl8380_perport[i * 2],
				  rtl8218B_6276B_rtl8380_perport[i * 2 + 1]);
		i++;
	}

	/*Disable broadcast ID*/
	write_phy(mac, 0xfff, 0x1f, 0x0000);
	write_phy(mac, 0xfff, 0x1d, 0x0008);
	write_phy(mac, 0xfff, 0x1f, 0x0266);
	write_phy(mac, 0xfff, 0x16, 0x00 + mac);
	write_phy(mac, 0xfff, 0x1f, 0x0000);
	write_phy(mac, 0xfff, 0x1d, 0x0000);
	mdelay(1);

	return 0;
}

static int rtl8218b_ext_match_phy_device(struct phy_device *phydev)
{
	int addr = phydev->mdio.addr;

	/* Both the RTL8214FC and the external RTL8218B have the same
	 * PHY ID. On the RTL838x, the RTL8218B can only be attached_dev
	 * at PHY IDs 0-7, while the RTL8214FC must be attached via
	 * the pair of SGMII/1000Base-X with higher PHY-IDs
	 */
	if (soc_info.family == RTL8380_FAMILY_ID)
		return phydev->phy_id == PHY_ID_RTL8218B_E && addr < 8;
	else
		return phydev->phy_id == PHY_ID_RTL8218B_E;
}

static int rtl8218b_read_mmd(struct phy_device *phydev,
				     int devnum, u16 regnum)
{
	int ret;
	u32 val;
	int addr = phydev->mdio.addr;

	ret = read_mmd_phy(addr, devnum, regnum, &val);
	if (ret)
		return ret;
	return val;
}

static int rtl8218b_write_mmd(struct phy_device *phydev,
				      int devnum, u16 regnum, u16 val)
{
	int addr = phydev->mdio.addr;

	return rtl838x_write_mmd_phy(addr, devnum, regnum, val);
}

static int rtl8226_read_mmd(struct phy_device *phydev, int devnum, u16 regnum)
{
	int port = phydev->mdio.addr;  // the SoC translates port addresses to PHY addr
	int err;
	u32 val;

	err = read_mmd_phy(port, devnum, regnum, &val);
	if (err)
		return err;
	return val;
}

static int rtl8226_write_mmd(struct phy_device *phydev, int devnum, u16 regnum, u16 val)
{
	int port = phydev->mdio.addr; // the SoC translates port addresses to PHY addr

	return write_mmd_phy(port, devnum, regnum, val);
}

static void rtl8380_rtl8214fc_media_set(int mac, bool set_fibre)
{
	int base = mac - (mac % 4);
	static int reg[] = {16, 19, 20, 21};
	int val, media, power;

	pr_info("%s: port %d, set_fibre: %d\n", __func__, mac, set_fibre);
	write_phy(base, 0xfff, 29, 8);
	read_phy(base, 0x266, reg[mac % 4], &val);

	media = (val >> 10) & 0x3;
	pr_info("Current media %x\n", media);
	if (media & 0x2) {
		pr_info("Powering off COPPER\n");
		write_phy(base, 0xfff, 29, 1);
		/* Ensure power is off */
		read_phy(base, 0xa40, 16, &power);
		if (!(power & (1 << 11)))
			write_phy(base, 0xa40, 16, power | (1 << 11));
	} else {
		pr_info("Powering off FIBRE");
		write_phy(base, 0xfff, 29, 3);
		/* Ensure power is off */
		read_phy(base, 0xa40, 16, &power);
		if (!(power & (1 << 11)))
			write_phy(base, 0xa40, 16, power | (1 << 11));
	}

	if (set_fibre) {
		val |= 1 << 10;
		val &= ~(1 << 11);
	} else {
		val |= 1 << 10;
		val |= 1 << 11;
	}
	write_phy(base, 0xfff, 29, 8);
	write_phy(base, 0x266, reg[mac % 4], val);
	write_phy(base, 0xfff, 29, 0);

	if (set_fibre) {
		pr_info("Powering on FIBRE");
		write_phy(base, 0xfff, 29, 3);
		/* Ensure power is off */
		read_phy(base, 0xa40, 16, &power);
		if (power & (1 << 11))
			write_phy(base, 0xa40, 16, power & ~(1 << 11));
	} else {
		pr_info("Powering on COPPER\n");
		write_phy(base, 0xfff, 29, 1);
		/* Ensure power is off */
		read_phy(base, 0xa40, 16, &power);
		if (power & (1 << 11))
			write_phy(base, 0xa40, 16, power & ~(1 << 11));
	}

	write_phy(base, 0xfff, 29, 0);
}

static bool rtl8380_rtl8214fc_media_is_fibre(int mac)
{
	int base = mac - (mac % 4);
	static int reg[] = {16, 19, 20, 21};
	u32 val;

	write_phy(base, 0xfff, 29, 8);
	read_phy(base, 0x266, reg[mac % 4], &val);
	write_phy(base, 0xfff, 29, 0);
	if (val & (1 << 11))
		return false;
	return true;
}

static int rtl8214fc_set_port(struct phy_device *phydev, int port)
{
	bool is_fibre = (port == PORT_FIBRE ? true : false);
	int addr = phydev->mdio.addr;

	pr_debug("%s port %d to %d\n", __func__, addr, port);

	rtl8380_rtl8214fc_media_set(addr, is_fibre);
	return 0;
}

static int rtl8214fc_get_port(struct phy_device *phydev)
{
	int addr = phydev->mdio.addr;

	pr_debug("%s: port %d\n", __func__, addr);
	if (rtl8380_rtl8214fc_media_is_fibre(addr))
		return PORT_FIBRE;
	return PORT_MII;
}

/*
 * Enable EEE on the RTL8218B PHYs
 * The method used is not the preferred way (which would be based on the MAC-EEE state,
 * but the only way that works since the kernel first enables EEE in the MAC
 * and then sets up the PHY. The MAC-based approach would require the oppsite.
 */
void rtl8218d_eee_set(int port, bool enable)
{
	u32 val;
	bool an_enabled;

	pr_debug("In %s %d, enable %d\n", __func__, port, enable);
	/* Set GPHY page to copper */
	write_phy(port, 0xa42, 30, 0x0001);

	read_phy(port, 0, 0, &val);
	an_enabled = val & BIT(12);

	/* Enable 100M (bit 1) / 1000M (bit 2) EEE */
	read_mmd_phy(port, 7, 60, &val);
	val |= BIT(2) | BIT(1);
	write_mmd_phy(port, 7, 60, enable ? 0x6 : 0);

	/* 500M EEE ability */
	read_phy(port, 0xa42, 20, &val);
	if (enable)
		val |= BIT(7);
	else
		val &= ~BIT(7);
	write_phy(port, 0xa42, 20, val);

	/* Restart AN if enabled */
	if (an_enabled) {
		read_phy(port, 0, 0, &val);
		val |= BIT(9);
		write_phy(port, 0, 0, val);
	}

	/* GPHY page back to auto*/
	write_phy(port, 0xa42, 30, 0);
}

static int rtl8218b_get_eee(struct phy_device *phydev,
				     struct ethtool_eee *e)
{
	u32 val;
	int addr = phydev->mdio.addr;

	pr_debug("In %s, port %d, was enabled: %d\n", __func__, addr, e->eee_enabled);

	/* Set GPHY page to copper */
	write_phy(addr, 0xa42, 29, 0x0001);

	read_phy(addr, 7, 60, &val);
	if (e->eee_enabled) {
		// Verify vs MAC-based EEE
		e->eee_enabled = !!(val & BIT(7));
		if (!e->eee_enabled) {
			read_phy(addr, 0x0A43, 25, &val);
			e->eee_enabled = !!(val & BIT(4));
		}
	}
	pr_debug("%s: enabled: %d\n", __func__, e->eee_enabled);

	/* GPHY page to auto */
	write_phy(addr, 0xa42, 29, 0x0000);

	return 0;
}

static int rtl8218d_get_eee(struct phy_device *phydev,
				     struct ethtool_eee *e)
{
	u32 val;
	int addr = phydev->mdio.addr;

	pr_debug("In %s, port %d, was enabled: %d\n", __func__, addr, e->eee_enabled);

	/* Set GPHY page to copper */
	write_phy(addr, 0xa42, 30, 0x0001);

	read_phy(addr, 7, 60, &val);
	if (e->eee_enabled)
		e->eee_enabled = !!(val & BIT(7));
	pr_debug("%s: enabled: %d\n", __func__, e->eee_enabled);

	/* GPHY page to auto */
	write_phy(addr, 0xa42, 30, 0x0000);

	return 0;
}

static int rtl8214fc_set_eee(struct phy_device *phydev,
				     struct ethtool_eee *e)
{
	u32 poll_state;
	int port = phydev->mdio.addr;
	bool an_enabled;
	u32 val;

	pr_debug("In %s port %d, enabled %d\n", __func__, port, e->eee_enabled);

	if (rtl8380_rtl8214fc_media_is_fibre(port)) {
		netdev_err(phydev->attached_dev, "Port %d configured for FIBRE", port);
		return -ENOTSUPP;
	}

	poll_state = disable_polling(port);

	/* Set GPHY page to copper */
	write_phy(port, 0xa42, 29, 0x0001);

	// Get auto-negotiation status
	read_phy(port, 0, 0, &val);
	an_enabled = val & BIT(12);

	pr_info("%s: aneg: %d\n", __func__, an_enabled);
	read_phy(port, 0x0A43, 25, &val);
	val &= ~BIT(5);  // Use MAC-based EEE
	write_phy(port, 0x0A43, 25, val);

	/* Enable 100M (bit 1) / 1000M (bit 2) EEE */
	write_phy(port, 7, 60, e->eee_enabled ? 0x6 : 0);

	/* 500M EEE ability */
	read_phy(port, 0xa42, 20, &val);
	if (e->eee_enabled)
		val |= BIT(7);
	else
		val &= ~BIT(7);
	write_phy(port, 0xa42, 20, val);

	/* Restart AN if enabled */
	if (an_enabled) {
		pr_info("%s: doing aneg\n", __func__);
		read_phy(port, 0, 0, &val);
		val |= BIT(9);
		write_phy(port, 0, 0, val);
	}

	/* GPHY page back to auto*/
	write_phy(port, 0xa42, 29, 0);

	resume_polling(poll_state);

	return 0;
}

static int rtl8214fc_get_eee(struct phy_device *phydev,
				      struct ethtool_eee *e)
{
	int addr = phydev->mdio.addr;

	pr_debug("In %s port %d, enabled %d\n", __func__, addr, e->eee_enabled);
	if (rtl8380_rtl8214fc_media_is_fibre(addr)) {
		netdev_err(phydev->attached_dev, "Port %d configured for FIBRE", addr);
		return -ENOTSUPP;
	}

	return rtl8218b_get_eee(phydev, e);
}

static int rtl8218b_set_eee(struct phy_device *phydev, struct ethtool_eee *e)
{
	int port = phydev->mdio.addr;
	u64 poll_state;
	u32 val;
	bool an_enabled;

	pr_info("In %s, port %d, enabled %d\n", __func__, port, e->eee_enabled);

	poll_state = disable_polling(port);

	/* Set GPHY page to copper */
	write_phy(port, 0, 30, 0x0001);
	read_phy(port, 0, 0, &val);
	an_enabled = val & BIT(12);

	if (e->eee_enabled) {
		/* 100/1000M EEE Capability */
		write_phy(port, 0, 13, 0x0007);
		write_phy(port, 0, 14, 0x003C);
		write_phy(port, 0, 13, 0x4007);
		write_phy(port, 0, 14, 0x0006);

		read_phy(port, 0x0A43, 25, &val);
		val |= BIT(4);
		write_phy(port, 0x0A43, 25, val);
	} else {
		/* 100/1000M EEE Capability */
		write_phy(port, 0, 13, 0x0007);
		write_phy(port, 0, 14, 0x003C);
		write_phy(port, 0, 13, 0x0007);
		write_phy(port, 0, 14, 0x0000);

		read_phy(port, 0x0A43, 25, &val);
		val &= ~BIT(4);
		write_phy(port, 0x0A43, 25, val);
	}

	/* Restart AN if enabled */
	if (an_enabled) {
		read_phy(port, 0, 0, &val);
		val |= BIT(9);
		write_phy(port, 0, 0, val);
	}

	/* GPHY page back to auto*/
	write_phy(port, 0xa42, 30, 0);

	pr_info("%s done\n", __func__);
	resume_polling(poll_state);

	return 0;
}

static int rtl8218d_set_eee(struct phy_device *phydev, struct ethtool_eee *e)
{
	int addr = phydev->mdio.addr;
	u64 poll_state;

	pr_info("In %s, port %d, enabled %d\n", __func__, addr, e->eee_enabled);

	poll_state = disable_polling(addr);

	rtl8218d_eee_set(addr, (bool) e->eee_enabled);

	resume_polling(poll_state);

	return 0;
}

static int rtl8214c_match_phy_device(struct phy_device *phydev)
{
	return phydev->phy_id == PHY_ID_RTL8214C;
}

static int rtl8380_configure_rtl8214c(struct phy_device *phydev)
{
	u32 phy_id, val;
	int mac = phydev->mdio.addr;

	read_phy(mac, 0, 2, &val);
	phy_id = val << 16;
	read_phy(mac, 0, 3, &val);
	phy_id |= val;
	pr_debug("Phy on MAC %d: %x\n", mac, phy_id);

	phydev_info(phydev, "Detected external RTL8214C\n");

	/* GPHY auto conf */
	write_phy(mac, 0xa42, 29, 0);
	return 0;
}

static int rtl8380_configure_rtl8214fc(struct phy_device *phydev)
{
	u32 phy_id, val, page = 0;
	int i, l;
	int mac = phydev->mdio.addr;
	struct fw_header *h;
	u32 *rtl8380_rtl8214fc_perchip;
	u32 *rtl8380_rtl8214fc_perport;

	read_phy(mac, 0, 2, &val);
	phy_id = val << 16;
	read_phy(mac, 0, 3, &val);
	phy_id |= val;
	pr_debug("Phy on MAC %d: %x\n", mac, phy_id);

	/* Read internal PHY id */
	write_phy(mac, 0, 30, 0x0001);
	write_phy(mac, 0, 31, 0x0a42);
	write_phy(mac, 31, 27, 0x0002);
	read_phy(mac, 31, 28, &val);
	if (val != 0x6276) {
		phydev_err(phydev, "Expected external RTL8214FC, found PHY-ID %x\n", val);
		return -1;
	}
	phydev_info(phydev, "Detected external RTL8214FC\n");

	h = rtl838x_request_fw(phydev, &rtl838x_8214fc_fw, FIRMWARE_838X_8214FC_1);
	if (!h)
		return -1;

	if (h->phy != 0x8214fc00) {
		phydev_err(phydev, "Wrong firmware file: PHY mismatch.\n");
		return -1;
	}

	rtl8380_rtl8214fc_perchip = (void *)h + sizeof(struct fw_header)
		   + h->parts[0].start;

	rtl8380_rtl8214fc_perport = (void *)h + sizeof(struct fw_header)
		   + h->parts[1].start;

	/* detect phy version */
	write_phy(mac, 0xfff, 27, 0x0004);
	read_phy(mac, 0xfff, 28, &val);

	read_phy(mac, 0, 16, &val);
	if (val & (1 << 11))
		rtl8380_rtl8214fc_on_off(mac, true);
	else
		rtl8380_phy_reset(mac);

	msleep(100);
	write_phy(mac, 0, 30, 0x0001);

	i = 0;
	while (rtl8380_rtl8214fc_perchip[i * 3]
	       && rtl8380_rtl8214fc_perchip[i * 3 + 1]) {
		if (rtl8380_rtl8214fc_perchip[i * 3 + 1] == 0x1f)
			page = rtl8380_rtl8214fc_perchip[i * 3 + 2];
		if (rtl8380_rtl8214fc_perchip[i * 3 + 1] == 0x13 && page == 0x260) {
			read_phy(mac + rtl8380_rtl8214fc_perchip[i * 3], 0x260, 13, &val);
			val = (val & 0x1f00) | (rtl8380_rtl8214fc_perchip[i * 3 + 2]
				& 0xe0ff);
			write_phy(mac + rtl8380_rtl8214fc_perchip[i * 3],
					  0xfff, rtl8380_rtl8214fc_perchip[i * 3 + 1], val);
		} else {
			write_phy(mac + rtl8380_rtl8214fc_perchip[i * 3],
					  0xfff, rtl8380_rtl8214fc_perchip[i * 3 + 1],
					  rtl8380_rtl8214fc_perchip[i * 3 + 2]);
		}
		i++;
	}

	/* Force copper medium */
	for (i = 0; i < 4; i++) {
		write_phy(mac + i, 0xfff, 0x1f, 0x0000);
		write_phy(mac + i, 0xfff, 0x1e, 0x0001);
	}

	/* Enable PHY */
	for (i = 0; i < 4; i++) {
		write_phy(mac + i, 0xfff, 0x1f, 0x0000);
		write_phy(mac + i, 0xfff, 0x00, 0x1140);
	}
	mdelay(100);

	/* Disable Autosensing */
	for (i = 0; i < 4; i++) {
		for (l = 0; l < 100; l++) {
			read_phy(mac + i, 0x0a42, 0x10, &val);
			if ((val & 0x7) >= 3)
				break;
		}
		if (l >= 100) {
			phydev_err(phydev, "Could not disable autosensing\n");
			return -1;
		}
	}

	/* Request patch */
	for (i = 0; i < 4; i++) {
		write_phy(mac + i,  0xfff, 0x1f, 0x0b82);
		write_phy(mac + i,  0xfff, 0x10, 0x0010);
	}
	mdelay(300);

	/* Verify patch readiness */
	for (i = 0; i < 4; i++) {
		for (l = 0; l < 100; l++) {
			read_phy(mac + i, 0xb80, 0x10, &val);
			if (val & 0x40)
				break;
		}
		if (l >= 100) {
			phydev_err(phydev, "Could not patch PHY\n");
			return -1;
		}
	}

	/* Use Broadcast ID method for patching */
	write_phy(mac, 0xfff, 0x1f, 0x0000);
	write_phy(mac, 0xfff, 0x1d, 0x0008);
	write_phy(mac, 0xfff, 0x1f, 0x0266);
	write_phy(mac, 0xfff, 0x16, 0xff00 + mac);
	write_phy(mac, 0xfff, 0x1f, 0x0000);
	write_phy(mac, 0xfff, 0x1d, 0x0000);
	mdelay(1);

	i = 0;
	while (rtl8380_rtl8214fc_perport[i * 2]) {
		write_phy(mac, 0xfff, rtl8380_rtl8214fc_perport[i * 2],
				  rtl8380_rtl8214fc_perport[i * 2 + 1]);
		i++;
	}

	/*Disable broadcast ID*/
	write_phy(mac, 0xfff, 0x1f, 0x0000);
	write_phy(mac, 0xfff, 0x1d, 0x0008);
	write_phy(mac, 0xfff, 0x1f, 0x0266);
	write_phy(mac, 0xfff, 0x16, 0x00 + mac);
	write_phy(mac, 0xfff, 0x1f, 0x0000);
	write_phy(mac, 0xfff, 0x1d, 0x0000);
	mdelay(1);

	/* Auto medium selection */
	for (i = 0; i < 4; i++) {
		write_phy(mac + i, 0xfff, 0x1f, 0x0000);
		write_phy(mac + i, 0xfff, 0x1e, 0x0000);
	}

	return 0;
}

static int rtl8214fc_match_phy_device(struct phy_device *phydev)
{
	int addr = phydev->mdio.addr;

	return phydev->phy_id == PHY_ID_RTL8214FC && addr >= 24;
}

static int rtl8380_configure_serdes(struct phy_device *phydev)
{
	u32 v;
	u32 sds_conf_value;
	int i;
	struct fw_header *h;
	u32 *rtl8380_sds_take_reset;
	u32 *rtl8380_sds_common;
	u32 *rtl8380_sds01_qsgmii_6275b;
	u32 *rtl8380_sds23_qsgmii_6275b;
	u32 *rtl8380_sds4_fiber_6275b;
	u32 *rtl8380_sds5_fiber_6275b;
	u32 *rtl8380_sds_reset;
	u32 *rtl8380_sds_release_reset;

	phydev_info(phydev, "Detected internal RTL8380 SERDES\n");

	h = rtl838x_request_fw(phydev, &rtl838x_8218b_fw, FIRMWARE_838X_8380_1);
	if (!h)
		return -1;

	if (h->magic != 0x83808380) {
		phydev_err(phydev, "Wrong firmware file: magic number mismatch.\n");
		return -1;
	}

	rtl8380_sds_take_reset = (void *)h + sizeof(struct fw_header)
		   + h->parts[0].start;

	rtl8380_sds_common = (void *)h + sizeof(struct fw_header)
		   + h->parts[1].start;

	rtl8380_sds01_qsgmii_6275b = (void *)h + sizeof(struct fw_header)
		   + h->parts[2].start;

	rtl8380_sds23_qsgmii_6275b = (void *)h + sizeof(struct fw_header)
		   + h->parts[3].start;

	rtl8380_sds4_fiber_6275b = (void *)h + sizeof(struct fw_header)
		   + h->parts[4].start;

	rtl8380_sds5_fiber_6275b = (void *)h + sizeof(struct fw_header)
		   + h->parts[5].start;

	rtl8380_sds_reset = (void *)h + sizeof(struct fw_header)
		   + h->parts[6].start;

	rtl8380_sds_release_reset = (void *)h + sizeof(struct fw_header)
		   + h->parts[7].start;

	/* Back up serdes power off value */
	sds_conf_value = sw_r32(RTL838X_SDS_CFG_REG);
	pr_info("SDS power down value: %x\n", sds_conf_value);

	/* take serdes into reset */
	i = 0;
	while (rtl8380_sds_take_reset[2 * i]) {
		sw_w32(rtl8380_sds_take_reset[2 * i + 1], rtl8380_sds_take_reset[2 * i]);
		i++;
		udelay(1000);
	}

	/* apply common serdes patch */
	i = 0;
	while (rtl8380_sds_common[2 * i]) {
		sw_w32(rtl8380_sds_common[2 * i + 1], rtl8380_sds_common[2 * i]);
		i++;
		udelay(1000);
	}

	/* internal R/W enable */
	sw_w32(3, RTL838X_INT_RW_CTRL);

	/* SerDes ports 4 and 5 are FIBRE ports */
	sw_w32_mask(0x7 | 0x38, 1 | (1 << 3), RTL838X_INT_MODE_CTRL);

	/* SerDes module settings, SerDes 0-3 are QSGMII */
	v = 0x6 << 25 | 0x6 << 20 | 0x6 << 15 | 0x6 << 10;
	/* SerDes 4 and 5 are 1000BX FIBRE */
	v |= 0x4 << 5 | 0x4;
	sw_w32(v, RTL838X_SDS_MODE_SEL);

	pr_info("PLL control register: %x\n", sw_r32(RTL838X_PLL_CML_CTRL));
	sw_w32_mask(0xfffffff0, 0xaaaaaaaf & 0xf, RTL838X_PLL_CML_CTRL);
	i = 0;
	while (rtl8380_sds01_qsgmii_6275b[2 * i]) {
		sw_w32(rtl8380_sds01_qsgmii_6275b[2 * i + 1],
			rtl8380_sds01_qsgmii_6275b[2 * i]);
		i++;
	}

	i = 0;
	while (rtl8380_sds23_qsgmii_6275b[2 * i]) {
		sw_w32(rtl8380_sds23_qsgmii_6275b[2 * i + 1], rtl8380_sds23_qsgmii_6275b[2 * i]);
		i++;
	}

	i = 0;
	while (rtl8380_sds4_fiber_6275b[2 * i]) {
		sw_w32(rtl8380_sds4_fiber_6275b[2 * i + 1], rtl8380_sds4_fiber_6275b[2 * i]);
		i++;
	}

	i = 0;
	while (rtl8380_sds5_fiber_6275b[2 * i]) {
		sw_w32(rtl8380_sds5_fiber_6275b[2 * i + 1], rtl8380_sds5_fiber_6275b[2 * i]);
		i++;
	}

	i = 0;
	while (rtl8380_sds_reset[2 * i]) {
		sw_w32(rtl8380_sds_reset[2 * i + 1], rtl8380_sds_reset[2 * i]);
		i++;
	}

	i = 0;
	while (rtl8380_sds_release_reset[2 * i]) {
		sw_w32(rtl8380_sds_release_reset[2 * i + 1], rtl8380_sds_release_reset[2 * i]);
		i++;
	}

	pr_info("SDS power down value now: %x\n", sw_r32(RTL838X_SDS_CFG_REG));
	sw_w32(sds_conf_value, RTL838X_SDS_CFG_REG);

	pr_info("Configuration of SERDES done\n");
	return 0;
}

static int rtl8390_configure_serdes(struct phy_device *phydev)
{
	phydev_info(phydev, "Detected internal RTL8390 SERDES\n");

	/* In autoneg state, force link, set SR4_CFG_EN_LINK_FIB1G */
	sw_w32_mask(0, 1 << 18, RTL839X_SDS12_13_XSG0 + 0x0a);

	/* Disable EEE: Clear FRE16_EEE_RSG_FIB1G, FRE16_EEE_STD_FIB1G,
	 * FRE16_C1_PWRSAV_EN_FIB1G, FRE16_C2_PWRSAV_EN_FIB1G
	 * and FRE16_EEE_QUIET_FIB1G
	 */
	sw_w32_mask(0x1f << 10, 0, RTL839X_SDS12_13_XSG0 + 0xe0);

	return 0;
}

void rtl9300_sds_field_w(int sds, u32 page, u32 reg, int end_bit, int start_bit, u32 v)
{
	int l = end_bit - start_bit - 1;
	u32 data = v;

	if (l < 32) {
		u32 mask = BIT(l) - 1;

		data = rtl930x_read_sds_phy(sds, page, reg);
		data &= ~(mask << start_bit);
		data |= (v & mask) << start_bit;
	}

	rtl930x_write_sds_phy(sds, page, reg, data);
}


u32 rtl9300_sds_field_r(int sds, u32 page, u32 reg, int end_bit, int start_bit)
{
	int l = end_bit - start_bit - 1;
	u32 v = rtl930x_read_sds_phy(sds, page, reg);

	if (l >= 32)
		return v;

	return (v >> start_bit) & (BIT(l) - 1);
}

/*
 * Force PHY modes on 10GBit Serdes
 */
void rtl9300_force_sds_mode(int sds, phy_interface_t phy_if)
{
	int sds_mode;
	bool lc_on;
	int i, lc_value;
	int lane_0 = (sds % 2) ? sds - 1 : sds;
	u32 v, cr_0, cr_1, cr_2;
	u32 m_bit, l_bit;

	pr_info("%s: SDS: %d, mode %d\n", __func__, sds, phy_if);
	switch (phy_if) {
	case PHY_INTERFACE_MODE_SGMII:
		sds_mode = 0x2;
		lc_on = false;
		lc_value = 0x1;
		break;

	case PHY_INTERFACE_MODE_HSGMII:
		sds_mode = 0x12;
		lc_value = 0x3;
		// Configure LC
		break;

	case PHY_INTERFACE_MODE_1000BASEX:
		sds_mode = 0x04;
		lc_on = false;
		break;

	case PHY_INTERFACE_MODE_2500BASEX:
		sds_mode = 0x16;
		lc_value = 0x3;
		// Configure LC
		break;

	case PHY_INTERFACE_MODE_10GBASER:
		sds_mode = 0x1a;
		lc_on = true;
		lc_value = 0x5;
		break;

	case PHY_INTERFACE_MODE_NA:
		// This will disable SerDes
		break;

	default:
		pr_err("%s: unknown serdes mode: %s\n",
		       __func__, phy_modes(phy_if));
		return;
	}

	// Power down SerDes
	rtl9300_sds_field_w(sds, 0x0, 0, 7, 6, 0x3);

	// Force mode enable
	rtl9300_sds_field_w(sds, 0x1f, 9, 6, 6, 0x1);

	/* SerDes off */
	rtl9300_sds_field_w(sds, 0x1f, 9, 11, 7, 0x1f);

	if (phy_if == PHY_INTERFACE_MODE_NA)
		return;

	// Enable LC and ring
	rtl9300_sds_field_w(lane_0, 0x20, 18, 3, 0, 0xf);

	if (sds == lane_0)
		rtl9300_sds_field_w(lane_0, 0x20, 18, 5, 4, 0x1);
	else
		rtl9300_sds_field_w(lane_0, 0x20, 18, 7, 6, 0x1);

	rtl9300_sds_field_w(sds, 0x20, 0, 5, 4, 0x3);

	if(lc_on)
		rtl9300_sds_field_w(lane_0, 0x20, 18, 11, 8, lc_value);
	else
		rtl9300_sds_field_w(lane_0, 0x20, 18, 15, 12, lc_value);

	// Force analog LC & ring on
	rtl9300_sds_field_w(lane_0, 0x21, 11, 3, 0, 0xf);

	v = lc_on ? 0x3 : 0x1;

	if(sds == lane_0)
		rtl9300_sds_field_w(lane_0, 0x20, 18, 5, 4, v);
	else
		rtl9300_sds_field_w(lane_0, 0x20, 18, 7, 6, v);

	// Force SerDes mode
	rtl9300_sds_field_w(sds, 0x1f, 9, 6, 6, 1);
	rtl9300_sds_field_w(sds, 0x1f, 9, 11, 7, sds_mode);

	// Toggle LC or Ring
	for (i = 0; i < 20; i++) {
		mdelay(200);

		rtl930x_write_sds_phy(lane_0, 0x1f, 2, 53);

		m_bit = (lane_0 == sds) ? (4) : (5);
		l_bit = (lane_0 == sds) ? (4) : (5);

		cr_0 = rtl9300_sds_field_r(lane_0, 0x1f, 20, m_bit, l_bit);
		mdelay(10);
		cr_1 = rtl9300_sds_field_r(lane_0, 0x1f, 20, m_bit, l_bit);
		mdelay(10);
		cr_2 = rtl9300_sds_field_r(lane_0, 0x1f, 20, m_bit, l_bit);

		if(cr_0 && cr_1 && cr_2) {
			u32 t;
			if (phy_if != PHY_INTERFACE_MODE_10GBASER)
				break;

			t = rtl9300_sds_field_r(sds, 0x6, 0x1, 2, 2);
			rtl9300_sds_field_w(sds, 0x6, 0x1, 2, 2, 0x1);

			// Reset FSM
			rtl9300_sds_field_w(sds, 0x6, 0x2, 12, 12, 0x1);
			mdelay(10);
			rtl9300_sds_field_w(sds, 0x6, 0x2, 12, 12, 0x0);
			mdelay(10);

			// Need to read this twice
			v = rtl9300_sds_field_r(sds, 0x5, 0, 12, 12);
			v = rtl9300_sds_field_r(sds, 0x5, 0, 12, 12);

			rtl9300_sds_field_w(sds, 0x6, 0x1, 2, 2, t);

			// Reset FSM again
			rtl9300_sds_field_w(sds, 0x6, 0x2, 12, 12, 0x1);
			mdelay(10);
			rtl9300_sds_field_w(sds, 0x6, 0x2, 12, 12, 0x0);
			mdelay(10);

			if (v == 1)
				break;
		}

		m_bit = (phy_if == PHY_INTERFACE_MODE_10GBASER) ? 3 : 1;
		l_bit = (phy_if == PHY_INTERFACE_MODE_10GBASER) ? 2 : 0;

		rtl9300_sds_field_w(lane_0, 0x21, 11, m_bit, l_bit, 0x2);
		mdelay(10);
		rtl9300_sds_field_w(lane_0, 0x21, 11, m_bit, l_bit, 0x3);
	}

	// Re-enable power
	rtl9300_sds_field_w(sds, 0x20, 0, 7, 6, 0);

	// Reset SerDes RX
	rtl9300_sds_field_w(sds, 0x2e, 0x15, 4, 4, 0x1);
	mdelay(5);
	rtl9300_sds_field_w(sds, 0x2e, 0x15, 4, 4, 0x0);
}

void rtl9300_sds_tx_config(int sds, phy_interface_t phy_if)
{
	// parameters: rtl9303_80G_txParam_s2
	int impedance = 0x8;
	int pre_amp = 0x2;
	int main_amp = 0x9;
	int post_amp = 0x2;
	int pre_en = 0x1;
	int post_en = 0x1;
	int page;

	switch(phy_if) {
	case PHY_INTERFACE_MODE_1000BASEX:
		page = 0x25;
		break;
	case PHY_INTERFACE_MODE_HSGMII:
	case PHY_INTERFACE_MODE_2500BASEX:
		page = 0x29;
		break;
	case PHY_INTERFACE_MODE_10GBASER:
		page = 0x2f;
		break;
	default:
		pr_err("%s: unsupported PHY mode\n", __func__);
		return;
	}

	rtl9300_sds_field_w(sds, page, 0x1, 15, 11, pre_amp);
	rtl9300_sds_field_w(sds, page, 0x7, 0, 0, pre_en);
	rtl9300_sds_field_w(sds, page, 0x7, 8, 4, main_amp);
	rtl9300_sds_field_w(sds, page, 0x6, 4, 0, post_amp);
	rtl9300_sds_field_w(sds, page, 0x7, 3, 3, post_en);
	rtl9300_sds_field_w(sds, page, 0x18, 15, 12, impedance);
}

/*
 * Wait for clock ready, this assumes the SerDes is in XGMII mode
 * timeout is in ms
 */
int rtl9300_sds_clock_wait(int timeout)
{
	u32 v;
	unsigned long start = jiffies;

	do {
		rtl9300_sds_field_w(2, 0x1f, 0x2, 15, 0, 53);
		v = rtl9300_sds_field_r(2, 0x1f, 20, 5, 4);
		if (v == 3)
			return 0;
	} while (jiffies < start + (HZ / 1000) * timeout);

	return 1;
}

void rtl9300_serdes_mac_link_config(int sds, bool tx_normal, bool rx_normal)
{
	u32 v10, v1;

	v10 = rtl930x_read_sds_phy(sds, 6, 2); // 10GBit, page 6, reg 2
	v1 = rtl930x_read_sds_phy(sds, 0, 0); // 1GBit, page 0, reg 0
	pr_info("%s: registers before %08x %08x\n", __func__, v10, v1);

	v10 &= ~(BIT(13) | BIT(14));
	v1 &= ~(BIT(8) | BIT(9));

	v10 |= rx_normal ? 0 : BIT(13);
	v1 |= rx_normal ? 0 : BIT(9);

	v10 |= tx_normal ? 0 : BIT(14);
	v1 |= tx_normal ? 0 : BIT(8);

	rtl930x_write_sds_phy(sds, 6, 2, v10);
	rtl930x_write_sds_phy(sds, 0, 0, v1);

	v10 = rtl930x_read_sds_phy(sds, 6, 2);
	v1 = rtl930x_read_sds_phy(sds, 0, 0);
	pr_info("%s: registers after %08x %08x\n", __func__, v10, v1);
}

// phy_mode = PHY_INTERFACE_MODE_10GBASER, sds_mode = 0x1a
int rtl9300_serdes_setup(int sds_num, int phy_mode)
{
	u32 v;
	int sds_mode;

	switch (phy_mode) {
	case PHY_INTERFACE_MODE_HSGMII:
		sds_mode = 0x12;
		break;
	case PHY_INTERFACE_MODE_1000BASEX:
		sds_mode = 0x04;
		break;
	case PHY_INTERFACE_MODE_XGMII:
		sds_mode = 0x10;
		break;
	case PHY_INTERFACE_MODE_10GBASER:
		sds_mode = 0x1a;
		break;
	case PHY_INTERFACE_MODE_USXGMII:
		sds_mode = 0x0d;
		break;
	default:
		pr_err("%s: unknown serdes mode: %s\n", __func__, phy_modes(phy_mode));
		return -EINVAL;
	}

	// Maybe use dal_longan_sds_init

	// ----> dal_longan_sds_mode_set
	pr_info("%s: Configuring RTL9300 SERDES %d, mode %02x\n", __func__, sds_num, sds_mode);
	// Set default Medium to fibre ????
	v = rtl930x_read_sds_phy(sds_num, 0x1f, 11);
	if (v < 0) {
		pr_err("Cannot access SerDes PHY %d\n", sds_num);
		return -EINVAL;
	}
	pr_info("%s set medium: %08x\n", __func__, v);
	v |= BIT(1);
	rtl930x_write_sds_phy(sds_num, 0x1f, 11, v);
	pr_info("%s set medium after: %08x\n", __func__, v);

	// Enable SerDes for configuration
	rtl9300_sds_rst(sds_num, sds_mode);

	// Configure link to MAC
	rtl9300_serdes_mac_link_config(sds_num, true, true);

	// Enable 1GBit PHY
	v = rtl930x_read_sds_phy(sds_num, PHY_PAGE_2, PHY_CTRL_REG);
	pr_info("%s 1gbit phy: %08x\n", __func__, v);
	v &= ~BIT(PHY_POWER_BIT);
	rtl930x_write_sds_phy(sds_num, PHY_PAGE_2, PHY_CTRL_REG, v);
	pr_info("%s 1gbit phy enabled: %08x\n", __func__, v);

	// Enable 10GBit PHY
	v = rtl930x_read_sds_phy(sds_num, PHY_PAGE_4, PHY_CTRL_REG);
	pr_info("%s 10gbit phy: %08x\n", __func__, v);
	v &= ~BIT(PHY_POWER_BIT);
	rtl930x_write_sds_phy(sds_num, PHY_PAGE_4, PHY_CTRL_REG, v);
	pr_info("%s 10gbit phy after: %08x\n", __func__, v);

	rtl9300_force_sds_mode(sds_num, PHY_INTERFACE_MODE_NA);

	// Do RX calibration
	// Select rtl9300_rxCaliConf_serdes_myParam if SERDES
	// otherwise rtl9300_rxCaliConf_phy_myParam
//	rtl9300_do_rx_calibration(sds_num);

	rtl9300_sds_tx_config(sds_num, phy_mode);

	rtl9300_force_sds_mode(sds_num, phy_mode);

	// The clock needs only to be configured on the FPGA implementation

	// <----- dal_longan_sds_mode_set
	/* Set default Medium to fibre */
// 	v = rtl930x_read_sds_phy(sds_num, 0x1f, 11);
// 	if (v < 0) {
// 		dev_err(dev, "Cannot access SerDes PHY %d\n", phy_addr);
// 		return -EINVAL;
// 	}
// 	pr_info("%s set medium: %08x\n", __func__, v);
// 	v |= BIT(2);
// 	rtl930x_write_sds_phy(sds_num, 0x1f, 11, v);
// 	pr_info("%s set medium after: %08x\n", __func__, v);

	// TODO: this needs to be configurable via ethtool/.dts
	pr_info("%s: setting 1/10G fibre medium, mode %02x\n", __func__, sds_mode);
	rtl9300_sds_rst(sds_num, sds_mode);

	// TODO: Apply patch set for fibre type

	return 0;
}
int rtl9300_configure_serdes(struct phy_device *phydev)
{
	struct device *dev = &phydev->mdio.dev;
	int phy_addr = phydev->mdio.addr, phy_mode = PHY_INTERFACE_MODE_10GBASER, sds_mode = 0x1a;
	struct device_node *dn;
	u32 sds_num = 0;
	u32 v;

	if (dev->of_node) {
		dn = dev->of_node;
		
		if (of_property_read_u32(dn, "sds", &sds_num))
			sds_num = -1;
		pr_info("%s: Port %d, SerDes is %d\n", __func__, phy_addr, sds_num);
	} else {
		dev_err(dev, "No DT node.\n");
		return -EINVAL;
	}

	if (sds_num < 0)
		return 0;

	sds_mode = (phy_mode == PHY_INTERFACE_MODE_10GBASER) ? 0x1a : 0x4;
	// Maybe use dal_longan_sds_init

	// ----> dal_longan_sds_mode_set
	phydev_info(phydev, "Configuring internal RTL9300 SERDES %d\n", sds_num);
	/* Set default Medium to fibre */
	v = rtl930x_read_sds_phy(sds_num, 0x1f, 11);
	if (v < 0) {
		dev_err(dev, "Cannot access SerDes PHY %d\n", phy_addr);
		return -EINVAL;
	}
//	pr_info("%s set medium: %08x\n", __func__, v);
// 	v |= BIT(2);
// 	rtl930x_write_sds_phy(sds_num, 0x1f, 11, v);
// 	pr_info("%s set medium after: %08x\n", __func__, v);

	pr_info("%s: enabling link as speed 10G, link down\n", __func__);
	v = sw_r32(RTL930X_MAC_FORCE_MODE_CTRL + 4 * phy_addr);
	pr_info("%s, RTL930X_MAC_FORCE_MODE_CTRL : %08x\n", __func__, v);
	v |= BIT(0);		// MAC enabled, makes link impossible
	v &= ~(7 << 3);
	if (phy_mode == PHY_INTERFACE_MODE_10GBASER)
		v |= 4 << 3;  	// Speed = 10G, 1G is 2
	else
		v |= 2 << 3; 
	v &= ~BIT(1);	// Link is down
	sw_w32(v, RTL930X_MAC_FORCE_MODE_CTRL + 4 * phy_addr);
	pr_info("%s, RTL930X_MAC_FORCE_MODE_CTRL after: %08x\n", __func__, v);
	mdelay(20);

	// Enable SerDes for configuration
	rtl9300_sds_rst(sds_num, sds_mode);

	// Configure link to MAC
	rtl9300_serdes_mac_link_config(sds_num, true, true);

	// Enable 1GBit PHY
	v = rtl930x_read_sds_phy(sds_num, PHY_PAGE_2, PHY_CTRL_REG);
	pr_info("%s 1gbit phy: %08x\n", __func__, v);
	v &= ~BIT(PHY_POWER_BIT);
	rtl930x_write_sds_phy(sds_num, PHY_PAGE_2, PHY_CTRL_REG, v);
	pr_info("%s 1gbit phy enabled: %08x\n", __func__, v);

	// Enable 10GBit PHY
	v = rtl930x_read_sds_phy(sds_num, PHY_PAGE_4, PHY_CTRL_REG);
	pr_info("%s 10gbit phy: %08x\n", __func__, v);
	v &= ~BIT(PHY_POWER_BIT);
	rtl930x_write_sds_phy(sds_num, PHY_PAGE_4, PHY_CTRL_REG, v);
	pr_info("%s 10gbit phy after: %08x\n", __func__, v);

	rtl9300_force_sds_mode(sds_num, PHY_INTERFACE_MODE_NA);

	// Do RX calibration
	// Select rtl9300_rxCaliConf_serdes_myParam if SERDES
	// otherwise rtl9300_rxCaliConf_phy_myParam
//	rtl9300_do_rx_calibration(sds_num);

	rtl9300_sds_tx_config(sds_num, phy_mode);

	rtl9300_force_sds_mode(sds_num, phy_mode);

	// The clock needs only to be configured on the FPGA implementation

	// TODO: this needs to be configurable via ethtool/.dts
	pr_info("%s: setting 1/10G fibre medium, mode %02x\n", __func__, sds_mode);
	rtl9300_sds_rst(sds_num, sds_mode);

	// TODO: Apply patch set for fibre type

	return 0;
}

void rtl9310_sds_field_w(int sds, u32 page, u32 reg, int end_bit, int start_bit, u32 v)
{
	int l = end_bit - start_bit - 1;
	u32 data = v;

	if (l < 32) {
		u32 mask = BIT(l) - 1;

		data = rtl930x_read_sds_phy(sds, page, reg);
		data &= ~(mask << start_bit);
		data |= (v & mask) << start_bit;
	}

	rtl931x_write_sds_phy(sds, page, reg, data);
}


u32 rtl9310_sds_field_r(int sds, u32 page, u32 reg, int end_bit, int start_bit)
{
	int l = end_bit - start_bit - 1;
	u32 v = rtl931x_read_sds_phy(sds, page, reg);

	if (l >= 32)
		return v;

	return (v >> start_bit) & (BIT(l) - 1);
}

static void rtl931x_sds_rst(u32 sds)
{
	u32 o, v, o_mode;
	int shift = ((sds & 0x3) << 3);

	// TODO: We need to lock this!
	
	o = sw_r32(RTL931X_PS_SERDES_OFF_MODE_CTRL_ADDR);
	v = o | BIT(sds);
	sw_w32(v, RTL931X_PS_SERDES_OFF_MODE_CTRL_ADDR);

	o_mode = sw_r32(RTL931X_SERDES_MODE_CTRL + 4 * (sds >> 2));
	v = BIT(7) | 0x1F;
	sw_w32_mask(0xff << shift, v << shift, RTL931X_SERDES_MODE_CTRL + 4 * (sds >> 2));
	sw_w32(o_mode, RTL931X_SERDES_MODE_CTRL + 4 * (sds >> 2));

	sw_w32(o, RTL931X_PS_SERDES_OFF_MODE_CTRL_ADDR);
}

static void rtl931x_symerr_clear(u32 sds, phy_interface_t mode)
{
	u32 i;
	u32 xsg_sdsid_0, xsg_sdsid_1;

	switch (mode) {
	case PHY_INTERFACE_MODE_NA:
		break;
	case PHY_INTERFACE_MODE_XGMII:
		if (sds < 2)
			xsg_sdsid_0 = sds;
		else
			xsg_sdsid_0 = (sds - 1) * 2;
		xsg_sdsid_1 = xsg_sdsid_0 + 1;

		for (i = 0; i < 4; ++i) {
			rtl9310_sds_field_w(xsg_sdsid_0, 0x1, 24, 2, 0, i);
			rtl9310_sds_field_w(xsg_sdsid_0, 0x1, 3, 15, 8, 0x0);
			rtl9310_sds_field_w(xsg_sdsid_0, 0x1, 2, 15, 0, 0x0);
		}

		for (i = 0; i < 4; ++i) {
			rtl9310_sds_field_w(xsg_sdsid_1, 0x1, 24, 2, 0, i);
			rtl9310_sds_field_w(xsg_sdsid_1, 0x1, 3, 15, 8, 0x0);
			rtl9310_sds_field_w(xsg_sdsid_1, 0x1, 2, 15, 0, 0x0);
		}

		rtl9310_sds_field_w(xsg_sdsid_0, 0x1, 0, 15, 0, 0x0);
		rtl9310_sds_field_w(xsg_sdsid_0, 0x1, 1, 15, 8, 0x0);
		rtl9310_sds_field_w(xsg_sdsid_1, 0x1, 0, 15, 0, 0x0);
		rtl9310_sds_field_w(xsg_sdsid_1, 0x1, 1, 15, 8, 0x0);
		break;
	default:
		break;
	}

	return;
}

static u32 rtl931x_get_analog_sds(u32 sds)
{
	u32 sds_map[] = { 0, 1, 2, 3, 6, 7, 10, 11, 14, 15, 18, 19, 22, 23 };

	if (sds < 14)
		return sds_map[sds];
	return sds;
}

void rtl931x_sds_fiber_disable(u32 sds)
{
	u32 v = 0x3F;
	u32 asds = rtl931x_get_analog_sds(sds);

	rtl9310_sds_field_w(asds, 0x1F, 0x9, 11, 6, v);
}

static void rtl931x_sds_fiber_mode_set(u32 sds, phy_interface_t mode)
{
	u32 val, asds = rtl931x_get_analog_sds(sds);

	/* clear symbol error count before changing mode */
	rtl931x_symerr_clear(sds, mode);

	val = 0x9F;
	sw_w32(val, RTL931X_SERDES_MODE_CTRL + 4 * (sds >> 2));

	switch (mode) {
	case PHY_INTERFACE_MODE_SGMII:
		val = 0x5;
		break;

	case PHY_INTERFACE_MODE_1000BASEX:
		/* serdes mode FIBER1G */
		val = 0x9;
		break;

	case PHY_INTERFACE_MODE_10GBASER:
	case PHY_INTERFACE_MODE_10GKR:
		val = 0x35;
		break;
/*	case MII_10GR1000BX_AUTO:
		val = 0x39;
		break; */


	case PHY_INTERFACE_MODE_USXGMII:
		val = 0x1B;
		break;
	default:
		val = 0x25;
	}

	pr_info("%s writing analog SerDes Mode value %02x\n", __func__, val);
	rtl9310_sds_field_w(asds, 0x1F, 0x9, 11, 6, val);

	return;
}

static int rtl931x_sds_cmu_page_get(phy_interface_t mode)
{
	switch (mode) {
	case PHY_INTERFACE_MODE_SGMII:
	case PHY_INTERFACE_MODE_1000BASEX:	// MII_1000BX_FIBER / 100BX_FIBER / 1000BX100BX_AUTO
		return 0x24;
	case PHY_INTERFACE_MODE_HSGMII:
	case PHY_INTERFACE_MODE_2500BASEX:	// MII_2500Base_X:
		return 0x28;
//	case MII_HISGMII_5G:
//		return 0x2a;
	case PHY_INTERFACE_MODE_QSGMII:
		return 0x2a;			// Code also has 0x34
	case PHY_INTERFACE_MODE_XAUI:		// MII_RXAUI_LITE:
		return 0x2c;
	case PHY_INTERFACE_MODE_XGMII:		// MII_XSGMII
	case PHY_INTERFACE_MODE_10GKR:
	case PHY_INTERFACE_MODE_10GBASER:	// MII_10GR
		return 0x2e;
	default:
		return -1;
	}
	return -1;
}

static void rtl931x_cmu_type_set(u32 asds, phy_interface_t mode, int chiptype)
{
	int cmu_type = 0; // Clock Management Unit
	u32 cmu_page = 0;
	u32 frc_cmu_spd;
	u32 evenSds;
	u32 lane, frc_lc_mode_bitnum, frc_lc_mode_val_bitnum;

	switch (mode) {
	case PHY_INTERFACE_MODE_NA:
	case PHY_INTERFACE_MODE_10GKR:
	case PHY_INTERFACE_MODE_XGMII:
	case PHY_INTERFACE_MODE_10GBASER:
	case PHY_INTERFACE_MODE_USXGMII:
		return;

/*	case MII_10GR1000BX_AUTO:
		if (chiptype)
			rtl9310_sds_field_w(asds, 0x24, 0xd, 14, 14, 0);
		return; */

	case PHY_INTERFACE_MODE_QSGMII:
		cmu_type = 1;
		frc_cmu_spd = 0;
		break;

	case PHY_INTERFACE_MODE_HSGMII:
		cmu_type = 1;
		frc_cmu_spd = 1;
		break;

	case PHY_INTERFACE_MODE_1000BASEX:
		cmu_type = 1;
		frc_cmu_spd = 0;
		break;

/*	case MII_1000BX100BX_AUTO:
		cmu_type = 1;
		frc_cmu_spd = 0;
		break; */

	case PHY_INTERFACE_MODE_SGMII:
		cmu_type = 1;
		frc_cmu_spd = 0;
		break;

	case PHY_INTERFACE_MODE_2500BASEX:
		cmu_type = 1;
		frc_cmu_spd = 1;
		break;

	default:
		pr_info("SerDes %d mode is invalid\n", asds);
		return;
	}

	if (cmu_type == 1)
		cmu_page = rtl931x_sds_cmu_page_get(mode);

	lane = asds % 2;

	if (!lane) {
		frc_lc_mode_bitnum = 4;
		frc_lc_mode_val_bitnum = 5;
	} else {
		frc_lc_mode_bitnum = 6;
		frc_lc_mode_val_bitnum = 7;
	}

	evenSds = asds - lane;

	pr_info("%s: cmu_type %0d cmu_page %x frc_cmu_spd %d lane %d asds %d\n",
		__func__, cmu_type, cmu_page, frc_cmu_spd, lane, asds);

	if (cmu_type == 1) {
		pr_info("%s A CMU page 0x28 0x7 %08x\n", __func__, rtl931x_read_sds_phy(asds, 0x28, 0x7));
		rtl9310_sds_field_w(asds, cmu_page, 0x7, 15, 15, 0);
		pr_info("%s B CMU page 0x28 0x7 %08x\n", __func__, rtl931x_read_sds_phy(asds, 0x28, 0x7));
		if (chiptype) {
			rtl9310_sds_field_w(asds, cmu_page, 0xd, 14, 14, 0);
		}

		rtl9310_sds_field_w(evenSds, 0x20, 0x12, 3, 2, 0x3);
		rtl9310_sds_field_w(evenSds, 0x20, 0x12, frc_lc_mode_bitnum, frc_lc_mode_bitnum, 1);
		rtl9310_sds_field_w(evenSds, 0x20, 0x12, frc_lc_mode_val_bitnum, frc_lc_mode_val_bitnum, 0);
		rtl9310_sds_field_w(evenSds, 0x20, 0x12, 12, 12, 1);
		rtl9310_sds_field_w(evenSds, 0x20, 0x12, 15, 13, frc_cmu_spd);
	}

	pr_info("%s CMU page 0x28 0x7 %08x\n", __func__, rtl931x_read_sds_phy(asds, 0x28, 0x7));
	return;
}

static void rtl931x_sds_rx_rst(u32 sds)
{
	u32 asds = rtl931x_get_analog_sds(sds);

	if (sds < 2)
		return;

	rtl931x_write_sds_phy(asds, 0x2e, 0x12, 0x2740);
	rtl931x_write_sds_phy(asds, 0x2f, 0x0, 0x0);
	rtl931x_write_sds_phy(asds, 0x2f, 0x2, 0x2010);
	rtl931x_write_sds_phy(asds, 0x20, 0x0, 0xc10);

	rtl931x_write_sds_phy(asds, 0x2e, 0x12, 0x27c0);
	rtl931x_write_sds_phy(asds, 0x2f, 0x0, 0xc000);
	rtl931x_write_sds_phy(asds, 0x2f, 0x2, 0x6010);
	rtl931x_write_sds_phy(asds, 0x20, 0x0, 0xc30);

	mdelay(50);
}

static void rtl931x_sds_disable(u32 sds)
{
	u32 v = 0x1f;

	v |= BIT(7);
	sw_w32(v, RTL931X_SERDES_MODE_CTRL + (sds >> 2) * 4);
}

static void rtl931x_sds_mii_mode_set(u32 sds, phy_interface_t mode)
{
	u32 val;

	switch (mode) {
	case PHY_INTERFACE_MODE_QSGMII:
		val = 0x6;
		break;
	case PHY_INTERFACE_MODE_XGMII:
		val = 0x10; // serdes mode XSGMII
		break;
	case PHY_INTERFACE_MODE_USXGMII:
	case PHY_INTERFACE_MODE_2500BASEX:
		val = 0xD;
		break;
	case PHY_INTERFACE_MODE_HSGMII:
		val = 0x12;
		break;
	case PHY_INTERFACE_MODE_SGMII:
		val = 0x2;
		break;
	default:
		return;
	}

	val |= (1 << 7);

	sw_w32(val, RTL931X_SERDES_MODE_CTRL + 4 * (sds >> 2));
}

typedef struct {
	u8 page;
	u8 reg;
	u16 data;
} sds_config;

static sds_config sds_config_10p3125g_type1[] = {
	{ 0x2E, 0x00, 0x0107 }, { 0x2E, 0x01, 0x01A3 }, { 0x2E, 0x02, 0x6A24 },
	{ 0x2E, 0x03, 0xD10D }, { 0x2E, 0x04, 0x8000 }, { 0x2E, 0x05, 0xA17E },
	{ 0x2E, 0x06, 0xE31D }, { 0x2E, 0x07, 0x800E }, { 0x2E, 0x08, 0x0294 },
	{ 0x2E, 0x09, 0x0CE4 }, { 0x2E, 0x0A, 0x7FC8 }, { 0x2E, 0x0B, 0xE0E7 },
	{ 0x2E, 0x0C, 0x0200 }, { 0x2E, 0x0D, 0xDF80 }, { 0x2E, 0x0E, 0x0000 },
	{ 0x2E, 0x0F, 0x1FC2 }, { 0x2E, 0x10, 0x0C3F }, { 0x2E, 0x11, 0x0000 },
	{ 0x2E, 0x12, 0x27C0 }, { 0x2E, 0x13, 0x7E1D }, { 0x2E, 0x14, 0x1300 },
	{ 0x2E, 0x15, 0x003F }, { 0x2E, 0x16, 0xBE7F }, { 0x2E, 0x17, 0x0090 },
	{ 0x2E, 0x18, 0x0000 }, { 0x2E, 0x19, 0x4000 }, { 0x2E, 0x1A, 0x0000 },
	{ 0x2E, 0x1B, 0x8000 }, { 0x2E, 0x1C, 0x011F }, { 0x2E, 0x1D, 0x0000 },
	{ 0x2E, 0x1E, 0xC8FF }, { 0x2E, 0x1F, 0x0000 }, { 0x2F, 0x00, 0xC000 },
	{ 0x2F, 0x01, 0xF000 }, { 0x2F, 0x02, 0x6010 }, { 0x2F, 0x12, 0x0EE7 },
	{ 0x2F, 0x13, 0x0000 }
};

static sds_config sds_config_10p3125g_cmu_type1[] = {
	{ 0x2F, 0x03, 0x4210 }, { 0x2F, 0x04, 0x0000 }, { 0x2F, 0x05, 0x0019 },
	{ 0x2F, 0x06, 0x18A6 }, { 0x2F, 0x07, 0x2990 }, { 0x2F, 0x08, 0xFFF4 },
	{ 0x2F, 0x09, 0x1F08 }, { 0x2F, 0x0A, 0x0000 }, { 0x2F, 0x0B, 0x8000 },
	{ 0x2F, 0x0C, 0x4224 }, { 0x2F, 0x0D, 0x0000 }, { 0x2F, 0x0E, 0x0000 },
	{ 0x2F, 0x0F, 0xA470 }, { 0x2F, 0x10, 0x8000 }, { 0x2F, 0x11, 0x037B }
};

void rtl931x_sds_init(u32 sds, phy_interface_t mode)
{

	u32 board_sds_tx_type1[] = { 0x1C3, 0x1C3, 0x1C3, 0x1A3, 0x1A3,
		0x1A3, 0x143, 0x143, 0x143, 0x143, 0x163, 0x163
	};

	u32 board_sds_tx[] = { 0x1A00, 0x1A00, 0x200, 0x200, 0x200,
		0x200, 0x1A3, 0x1A3, 0x1A3, 0x1A3, 0x1E3, 0x1E3
	};

	u32 board_sds_tx2[] = { 0xDC0, 0x1C0, 0x200, 0x180, 0x160,
		0x123, 0x123, 0x163, 0x1A3, 0x1A0, 0x1C3, 0x9C3
	};

	u32 asds, dSds, ori, model_info, val;
	int chiptype = 0;

	asds = rtl931x_get_analog_sds(sds);

	if (sds > 13)
		return;

	pr_info("%s: set sds %d to mode %d\n", __func__, sds, mode);
	val = rtl9310_sds_field_r(asds, 0x1F, 0x9, 11, 6);

	pr_info("%s: fibermode %08X stored mode 0x%x analog SDS %d", __func__,
			rtl931x_read_sds_phy(asds, 0x1f, 0x9), val, asds);
	pr_info("%s: SGMII mode %08X in 0x24 0x9 analog SDS %d", __func__,
			rtl931x_read_sds_phy(asds, 0x24, 0x9), asds);
	pr_info("%s: CMU mode %08X stored even SDS %d", __func__,
			rtl931x_read_sds_phy(asds & ~1, 0x20, 0x12), asds & ~1);
	pr_info("%s: serdes_mode_ctrl %08X", __func__,  RTL931X_SERDES_MODE_CTRL + 4 * (sds >> 2));
	pr_info("%s CMU page 0x24 0x7 %08x\n", __func__, rtl931x_read_sds_phy(asds, 0x24, 0x7));
	pr_info("%s CMU page 0x26 0x7 %08x\n", __func__, rtl931x_read_sds_phy(asds, 0x26, 0x7));
	pr_info("%s CMU page 0x28 0x7 %08x\n", __func__, rtl931x_read_sds_phy(asds, 0x28, 0x7));
	pr_info("%s XSG page 0x0 0xe %08x\n", __func__, rtl931x_read_sds_phy(dSds, 0x0, 0xe));
	pr_info("%s XSG2 page 0x0 0xe %08x\n", __func__, rtl931x_read_sds_phy(dSds + 1, 0x0, 0xe));

	model_info = sw_r32(RTL93XX_MODEL_NAME_INFO);
	if ((model_info >> 4) & 0x1) {
		pr_info("detected chiptype 1\n");
		chiptype = 1;
	} else {
		pr_info("detected chiptype 0\n");
	}

	if (sds < 2)
		dSds = sds;
	else
		dSds = (sds - 1) * 2;

	pr_info("%s: 2.5gbit %08X dsds %d", __func__,
		rtl931x_read_sds_phy(dSds, 0x1, 0x14), dSds);

	pr_info("%s: RTL931X_PS_SERDES_OFF_MODE_CTRL_ADDR 0x%08X\n", __func__, sw_r32(RTL931X_PS_SERDES_OFF_MODE_CTRL_ADDR));
	ori = sw_r32(RTL931X_PS_SERDES_OFF_MODE_CTRL_ADDR);
	val = ori | (1 << sds);
	sw_w32(val, RTL931X_PS_SERDES_OFF_MODE_CTRL_ADDR);

	switch (mode) {
	case PHY_INTERFACE_MODE_NA:
		break;

	case PHY_INTERFACE_MODE_XGMII: // MII_XSGMII

		if (chiptype) {
			u32 xsg_sdsid_1;
			xsg_sdsid_1 = dSds + 1;
			//fifo inv clk
			rtl9310_sds_field_w(dSds, 0x1, 0x1, 7, 4, 0xf);
			rtl9310_sds_field_w(dSds, 0x1, 0x1, 3, 0, 0xf);

			rtl9310_sds_field_w(xsg_sdsid_1, 0x1, 0x1, 7, 4, 0xf);
			rtl9310_sds_field_w(xsg_sdsid_1, 0x1, 0x1, 3, 0, 0xf);

		}

		rtl9310_sds_field_w(dSds, 0x0, 0xE, 12, 12, 1);
		rtl9310_sds_field_w(dSds + 1, 0x0, 0xE, 12, 12, 1);
		break;

	case PHY_INTERFACE_MODE_USXGMII: // MII_USXGMII_10GSXGMII/10GDXGMII/10GQXGMII:
		u32 i, evenSds;
		u32 op_code = 0x6003;

		if (chiptype) {
			rtl9310_sds_field_w(asds, 0x6, 0x2, 12, 12, 1);

			for (i = 0; i < sizeof(sds_config_10p3125g_type1) / sizeof(sds_config); ++i) {
				rtl931x_write_sds_phy(asds, sds_config_10p3125g_type1[i].page - 0x4, sds_config_10p3125g_type1[i].reg, sds_config_10p3125g_type1[i].data);
			}

			evenSds = asds - (asds % 2);

			for (i = 0; i < sizeof(sds_config_10p3125g_cmu_type1) / sizeof(sds_config); ++i) {
				rtl931x_write_sds_phy(evenSds,
						      sds_config_10p3125g_cmu_type1[i].page - 0x4, sds_config_10p3125g_cmu_type1[i].reg, sds_config_10p3125g_cmu_type1[i].data);
			}

			rtl9310_sds_field_w(asds, 0x6, 0x2, 12, 12, 0);
		} else {

			rtl9310_sds_field_w(asds, 0x2e, 0xd, 6, 0, 0x0);
			rtl9310_sds_field_w(asds, 0x2e, 0xd, 7, 7, 0x1);

			rtl9310_sds_field_w(asds, 0x2e, 0x1c, 5, 0, 0x1E);
			rtl9310_sds_field_w(asds, 0x2e, 0x1d, 11, 0, 0x00);
			rtl9310_sds_field_w(asds, 0x2e, 0x1f, 11, 0, 0x00);
			rtl9310_sds_field_w(asds, 0x2f, 0x0, 11, 0, 0x00);
			rtl9310_sds_field_w(asds, 0x2f, 0x1, 11, 0, 0x00);

			rtl9310_sds_field_w(asds, 0x2e, 0xf, 12, 6, 0x7F);
			rtl931x_write_sds_phy(asds, 0x2f, 0x12, 0xaaa);

			rtl931x_sds_rx_rst(sds);

			rtl931x_write_sds_phy(asds, 0x7, 0x10, op_code);
			rtl931x_write_sds_phy(asds, 0x6, 0x1d, 0x0480);
			rtl931x_write_sds_phy(asds, 0x6, 0xe, 0x0400);
		}
		break;

	case PHY_INTERFACE_MODE_10GBASER: // MII_10GR / MII_10GR1000BX_AUTO:
		// configure 10GR fiber mode=1
		rtl9310_sds_field_w(asds, 0x1f, 0xb, 1, 1, 1);

		// init fiber_1g
		rtl9310_sds_field_w(dSds, 0x3, 0x13, 15, 14, 0);

		rtl9310_sds_field_w(dSds, 0x2, 0x0, 12, 12, 1);
		rtl9310_sds_field_w(dSds, 0x2, 0x0, 6, 6, 1);
		rtl9310_sds_field_w(dSds, 0x2, 0x0, 13, 13, 0);

		// init auto
		rtl9310_sds_field_w(asds, 0x1f, 13, 15, 0, 0x109e);
		rtl9310_sds_field_w(asds, 0x1f, 0x6, 14, 10, 0x8);
		rtl9310_sds_field_w(asds, 0x1f, 0x7, 10, 4, 0x7f);
		break;

	case PHY_INTERFACE_MODE_HSGMII:
		rtl9310_sds_field_w(dSds, 0x1, 0x14, 8, 8, 1);
		break;

	case PHY_INTERFACE_MODE_1000BASEX: // MII_1000BX_FIBER
		rtl9310_sds_field_w(dSds, 0x3, 0x13, 15, 14, 0);

		rtl9310_sds_field_w(dSds, 0x2, 0x0, 12, 12, 1);
		rtl9310_sds_field_w(dSds, 0x2, 0x0, 6, 6, 1);
		rtl9310_sds_field_w(dSds, 0x2, 0x0, 13, 13, 0);
		break;

	case PHY_INTERFACE_MODE_SGMII:
		rtl9310_sds_field_w(asds, 0x24, 0x9, 15, 15, 0);
		break;

	case PHY_INTERFACE_MODE_2500BASEX:
		rtl9310_sds_field_w(dSds, 0x1, 0x14, 8, 8, 1);
		break;

	case PHY_INTERFACE_MODE_QSGMII:
	default:
		pr_info("%s: PHY mode %s not supported by SerDes %d\n",
			__func__, phy_modes(mode), sds);
		return;
	}

	rtl931x_cmu_type_set(asds, mode, chiptype);

	if (sds >= 2 && sds <= 13) {
		if (chiptype)
			rtl931x_write_sds_phy(asds, 0x2E, 0x1, board_sds_tx_type1[sds - 2]);
		else {
			val = 0xa0000;
			sw_w32(val, RTL931X_CHIP_INFO_ADDR);
			val = sw_r32(RTL931X_CHIP_INFO_ADDR);
			if (val & BIT(28)) // consider 9311 etc. RTL9313_CHIP_ID == HWP_CHIP_ID(unit))
			{
				rtl931x_write_sds_phy(asds, 0x2E, 0x1, board_sds_tx2[sds - 2]);
			} else {
				rtl931x_write_sds_phy(asds, 0x2E, 0x1, board_sds_tx[sds - 2]);
			}
			val = 0;
			sw_w32(val, RTL931X_CHIP_INFO_ADDR);
		}
	}

	val = ori & ~BIT(sds);
	sw_w32(val, RTL931X_PS_SERDES_OFF_MODE_CTRL_ADDR);
	pr_debug("%s: RTL931X_PS_SERDES_OFF_MODE_CTRL_ADDR 0x%08X\n", __func__, sw_r32(RTL931X_PS_SERDES_OFF_MODE_CTRL_ADDR));

	if (mode == PHY_INTERFACE_MODE_XGMII || mode == PHY_INTERFACE_MODE_QSGMII
	    || mode == PHY_INTERFACE_MODE_HSGMII || mode == PHY_INTERFACE_MODE_SGMII
	    || mode == PHY_INTERFACE_MODE_USXGMII) {
		if (mode == PHY_INTERFACE_MODE_XGMII)
			rtl931x_sds_mii_mode_set(sds, mode);
		else
			rtl931x_sds_fiber_mode_set(sds, mode);
	}
}

int rtl931x_sds_cmu_band_set(int sds, bool enable, u32 band, phy_interface_t mode)
{
	u32 asds;
	int page = rtl931x_sds_cmu_page_get(mode);

	sds -= (sds % 2);
	sds = sds & ~1;
	asds = rtl931x_get_analog_sds(sds);
	page += 1;

	if (enable) {
		rtl9310_sds_field_w(asds, page, 0x7, 13, 13, 0);
		rtl9310_sds_field_w(asds, page, 0x7, 11, 11, 0);
	} else {
		rtl9310_sds_field_w(asds, page, 0x7, 13, 13, 0);
		rtl9310_sds_field_w(asds, page, 0x7, 11, 11, 0);
	}
		
	rtl9310_sds_field_w(asds, page, 0x7, 4, 0, band);

	rtl931x_sds_rst(sds);

	return 0;
}

int rtl931x_sds_cmu_band_get(int sds, phy_interface_t mode)
{
	int page = rtl931x_sds_cmu_page_get(mode);
	u32 asds, band;

	sds -= (sds % 2);
	asds = rtl931x_get_analog_sds(sds);
	page += 1;
	rtl931x_write_sds_phy(asds, 0x1f, 0x02, 73);

	rtl9310_sds_field_w(asds, page, 0x5, 15, 15, 1);
	band = rtl9310_sds_field_r(asds, 0x1f, 0x15, 8, 3);
	pr_info("%s band is: %d\n", __func__, band);

	return band;
}


int rtl931x_link_sts_get(u32 sds)
{
	u32 sts, sts1, latch_sts, latch_sts1;
	if (0){
		u32 xsg_sdsid_0, xsg_sdsid_1;

		xsg_sdsid_0 = sds < 2 ? sds : (sds - 1) * 2;
		xsg_sdsid_1 = xsg_sdsid_0 + 1;

		sts = rtl9310_sds_field_r(xsg_sdsid_0, 0x1, 29, 8, 0);
		sts1 = rtl9310_sds_field_r(xsg_sdsid_1, 0x1, 29, 8, 0);
		latch_sts = rtl9310_sds_field_r(xsg_sdsid_0, 0x1, 30, 8, 0);
		latch_sts1 = rtl9310_sds_field_r(xsg_sdsid_1, 0x1, 30, 8, 0);
	} else {
		u32  asds, dsds;

		asds = rtl931x_get_analog_sds(sds);
		sts = rtl9310_sds_field_r(asds, 0x5, 0, 12, 12);
		latch_sts = rtl9310_sds_field_r(asds, 0x4, 1, 2, 2);

		dsds = sds < 2 ? sds : (sds - 1) * 2;
		latch_sts1 = rtl9310_sds_field_r(dsds, 0x2, 1, 2, 2);
		sts1 = rtl9310_sds_field_r(dsds, 0x2, 1, 2, 2);
	}

	pr_info("%s: serdes %d sts %d, sts1 %d, latch_sts %d, latch_sts1 %d\n", __func__,
		sds, sts, sts1, latch_sts, latch_sts1);
	return sts1;
}

static int rtl8214fc_phy_probe(struct phy_device *phydev)
{
	struct device *dev = &phydev->mdio.dev;
	struct rtl838x_phy_priv *priv;
	int addr = phydev->mdio.addr;

	/* 839x has internal SerDes */
	if (soc_info.id == 0x8393)
		return -ENODEV;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->name = "RTL8214FC";

	/* All base addresses of the PHYs start at multiples of 8 */
	if (!(addr % 8)) {
		/* Configuration must be done whil patching still possible */
		return rtl8380_configure_rtl8214fc(phydev);
	}
	return 0;
}

static int rtl8214c_phy_probe(struct phy_device *phydev)
{
	struct device *dev = &phydev->mdio.dev;
	struct rtl838x_phy_priv *priv;
	int addr = phydev->mdio.addr;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->name = "RTL8214C";

	/* All base addresses of the PHYs start at multiples of 8 */
	if (!(addr % 8)) {
		/* Configuration must be done whil patching still possible */
		return rtl8380_configure_rtl8214c(phydev);
	}
	return 0;
}

static int rtl8218b_ext_phy_probe(struct phy_device *phydev)
{
	struct device *dev = &phydev->mdio.dev;
	struct rtl838x_phy_priv *priv;
	int addr = phydev->mdio.addr;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->name = "RTL8218B (external)";

	/* All base addresses of the PHYs start at multiples of 8 */
	if (!(addr % 8) && soc_info.family == RTL8380_FAMILY_ID) {
		/* Configuration must be done while patching still possible */
		return rtl8380_configure_ext_rtl8218b(phydev);
	}
	return 0;
}

static int rtl8218b_int_phy_probe(struct phy_device *phydev)
{
	struct device *dev = &phydev->mdio.dev;
	struct rtl838x_phy_priv *priv;
	int addr = phydev->mdio.addr;

	if (soc_info.family != RTL8380_FAMILY_ID)
		return -ENODEV;
	if (addr >= 24)
		return -ENODEV;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->name = "RTL8218B (internal)";

	/* All base addresses of the PHYs start at multiples of 8 */
	if (!(addr % 8)) {
		/* Configuration must be done while patching still possible */
		return rtl8380_configure_int_rtl8218b(phydev);
	}
	return 0;
}

static int rtl8218d_phy_probe(struct phy_device *phydev)
{
	struct device *dev = &phydev->mdio.dev;
	struct rtl838x_phy_priv *priv;
	int addr = phydev->mdio.addr;

	pr_debug("%s: id: %d\n", __func__, addr);
	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->name = "RTL8218D";

	/* All base addresses of the PHYs start at multiples of 8 */
	if (!(addr % 8)) {
		/* Configuration must be done while patching still possible */
// TODO:		return configure_rtl8218d(phydev);
	}
	return 0;
}

static int rtl8226_phy_probe(struct phy_device *phydev)
{
	struct device *dev = &phydev->mdio.dev;
	struct rtl838x_phy_priv *priv;
	int addr = phydev->mdio.addr;

	pr_info("%s: id: %d\n", __func__, addr);
	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->name = "RTL8226";

	return 0;
}

static int rtl838x_serdes_probe(struct phy_device *phydev)
{
	struct device *dev = &phydev->mdio.dev;
	struct rtl838x_phy_priv *priv;
	int addr = phydev->mdio.addr;

	if (soc_info.family != RTL8380_FAMILY_ID)
		return -ENODEV;
	if (addr < 24)
		return -ENODEV;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->name = "RTL8380 Serdes";

	/* On the RTL8380M, PHYs 24-27 connect to the internal SerDes */
	if (soc_info.id == 0x8380) {
		if (addr == 24)
			return rtl8380_configure_serdes(phydev);
		return 0;
	}
	return -ENODEV;
}

static int rtl8393_serdes_probe(struct phy_device *phydev)
{
	struct device *dev = &phydev->mdio.dev;
	struct rtl838x_phy_priv *priv;
	int addr = phydev->mdio.addr;

	pr_info("%s: id: %d\n", __func__, addr);
	if (soc_info.family != RTL8390_FAMILY_ID)
		return -ENODEV;

	if (addr < 24)
		return -ENODEV;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->name = "RTL8393 Serdes";
	return rtl8390_configure_serdes(phydev);
}

static int rtl8390_serdes_probe(struct phy_device *phydev)
{
	struct device *dev = &phydev->mdio.dev;
	struct rtl838x_phy_priv *priv;
	int addr = phydev->mdio.addr;

	if (soc_info.family != RTL8390_FAMILY_ID)
		return -ENODEV;

	if (addr < 24)
		return -ENODEV;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->name = "RTL8390 Serdes";
	return rtl8390_configure_generic(phydev);
}

static int rtl9300_serdes_probe(struct phy_device *phydev)
{
	struct device *dev = &phydev->mdio.dev;
	struct rtl838x_phy_priv *priv;
	int addr = phydev->mdio.addr;

	if (soc_info.family != RTL9300_FAMILY_ID)
		return -ENODEV;

	if (addr < 24)
		return -ENODEV;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->name = "RTL9300 Serdes";
	return rtl9300_configure_serdes(phydev);
}

static struct phy_driver rtl83xx_phy_driver[] = {
	{
		PHY_ID_MATCH_MODEL(PHY_ID_RTL8214C),
		.name		= "Realtek RTL8214C",
		.features	= PHY_GBIT_FEATURES,
		.match_phy_device = rtl8214c_match_phy_device,
		.probe		= rtl8214c_phy_probe,
		.suspend	= genphy_suspend,
		.resume		= genphy_resume,
		.set_loopback	= genphy_loopback,
	},
	{
		PHY_ID_MATCH_MODEL(PHY_ID_RTL8214FC),
		.name		= "Realtek RTL8214FC",
		.features	= PHY_GBIT_FIBRE_FEATURES,
		.match_phy_device = rtl8214fc_match_phy_device,
		.probe		= rtl8214fc_phy_probe,
		.suspend	= genphy_suspend,
		.resume		= genphy_resume,
		.set_loopback	= genphy_loopback,
		.read_mmd	= rtl8218b_read_mmd,
		.write_mmd	= rtl8218b_write_mmd,
		.set_port	= rtl8214fc_set_port,
		.get_port	= rtl8214fc_get_port,
		.set_eee	= rtl8214fc_set_eee,
		.get_eee	= rtl8214fc_get_eee,
	},
	{
		PHY_ID_MATCH_MODEL(PHY_ID_RTL8218B_E),
		.name		= "Realtek RTL8218B (external)",
		.features	= PHY_GBIT_FEATURES,
		.match_phy_device = rtl8218b_ext_match_phy_device,
		.probe		= rtl8218b_ext_phy_probe,
		.suspend	= genphy_suspend,
		.resume		= genphy_resume,
		.set_loopback	= genphy_loopback,
		.read_mmd	= rtl8218b_read_mmd,
		.write_mmd	= rtl8218b_write_mmd,
		.set_eee	= rtl8218b_set_eee,
		.get_eee	= rtl8218b_get_eee,
	},
	{
		PHY_ID_MATCH_MODEL(PHY_ID_RTL8218D),
		.name		= "REALTEK RTL8218D",
		.features	= PHY_GBIT_FEATURES,
		.probe		= rtl8218d_phy_probe,
		.suspend	= genphy_suspend,
		.resume		= genphy_resume,
		.set_loopback	= genphy_loopback,
		.set_eee	= rtl8218d_set_eee,
		.get_eee	= rtl8218d_get_eee,
	},
	{
		PHY_ID_MATCH_MODEL(PHY_ID_RTL8226),
		.name		= "REALTEK RTL8226",
		.features	= PHY_GBIT_FEATURES,
		.probe		= rtl8226_phy_probe,
		.suspend	= genphy_suspend,
		.resume		= genphy_resume,
		.set_loopback	= genphy_loopback,
		.read_mmd	= rtl8226_read_mmd,
		.write_mmd	= rtl8226_write_mmd,
		.read_page	= rtl8226_read_page,
		.write_page	= rtl8226_write_page,
		.read_status	= rtl8226_read_status,
		.config_aneg	= rtl8226_config_aneg,
		.set_eee	= rtl8226_set_eee,
		.get_eee	= rtl8226_get_eee,
	},
	{
		PHY_ID_MATCH_MODEL(PHY_ID_RTL8218B_I),
		.name		= "Realtek RTL8218B (internal)",
		.features	= PHY_GBIT_FEATURES,
		.probe		= rtl8218b_int_phy_probe,
		.suspend	= genphy_suspend,
		.resume		= genphy_resume,
		.set_loopback	= genphy_loopback,
		.read_mmd	= rtl8218b_read_mmd,
		.write_mmd	= rtl8218b_write_mmd,
		.set_eee	= rtl8218b_set_eee,
		.get_eee	= rtl8218b_get_eee,
	},
	{
		PHY_ID_MATCH_MODEL(PHY_ID_RTL8218B_I),
		.name		= "Realtek RTL8380 SERDES",
		.features	= PHY_GBIT_FIBRE_FEATURES,
		.probe		= rtl838x_serdes_probe,
		.suspend	= genphy_suspend,
		.resume		= genphy_resume,
		.set_loopback	= genphy_loopback,
		.read_mmd	= rtl8218b_read_mmd,
		.write_mmd	= rtl8218b_write_mmd,
		.read_status	= rtl8380_read_status,
	},
	{
		PHY_ID_MATCH_MODEL(PHY_ID_RTL8393_I),
		.name		= "Realtek RTL8393 SERDES",
		.features	= PHY_GBIT_FIBRE_FEATURES,
		.probe		= rtl8393_serdes_probe,
		.suspend	= genphy_suspend,
		.resume		= genphy_resume,
		.set_loopback	= genphy_loopback,
		.read_status	= rtl8393_read_status,
	},
	{
		PHY_ID_MATCH_MODEL(PHY_ID_RTL8390_GENERIC),
		.name		= "Realtek RTL8390 Generic",
		.features	= PHY_GBIT_FIBRE_FEATURES,
		.probe		= rtl8390_serdes_probe,
		.suspend	= genphy_suspend,
		.resume		= genphy_resume,
		.set_loopback	= genphy_loopback,
	},
	{
		PHY_ID_MATCH_MODEL(PHY_ID_RTL9300_I),
		.name		= "REALTEK RTL9300 SERDES",
		.features	= PHY_GBIT_FIBRE_FEATURES,
		.probe		= rtl9300_serdes_probe,
		.suspend	= genphy_suspend,
		.resume		= genphy_resume,
		.set_loopback	= genphy_loopback,
	},
};

module_phy_driver(rtl83xx_phy_driver);

static struct mdio_device_id __maybe_unused rtl83xx_tbl[] = {
	{ PHY_ID_MATCH_MODEL(PHY_ID_RTL8214FC) },
	{ }
};

MODULE_DEVICE_TABLE(mdio, rtl83xx_tbl);

MODULE_AUTHOR("B. Koblitz");
MODULE_DESCRIPTION("RTL83xx PHY driver");
MODULE_LICENSE("GPL");
