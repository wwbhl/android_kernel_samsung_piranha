/*
 * ALSA SoC TWL6040 codec driver
 *
 * Author:	 Misael Lopez Cruz <x0052729@ti.com>
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
 *
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/pm.h>
#include <linux/i2c.h>
#include <linux/gpio.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/wakelock.h>
#include <linux/i2c/twl.h>
#include <linux/mfd/twl6040-codec.h>
#ifdef CONFIG_REGULATOR_TPS6130X
#include <linux/regulator/consumer.h>
#endif

#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/initval.h>
#include <sound/tlv.h>

#include "twl6040.h"

#define TWL6040_RATES		SNDRV_PCM_RATE_8000_96000
#define TWL6040_FORMATS	(SNDRV_PCM_FMTBIT_S32_LE)

#define TWL6040_OUTHS_0dB 0x00
#define TWL6040_OUTHS_M30dB 0x0F
#define TWL6040_OUTHF_0dB 0x03
#define TWL6040_OUTHF_M52dB 0x1D

#define TWL6040_RAMP_NONE	0
#define TWL6040_RAMP_UP		1
#define TWL6040_RAMP_DOWN	2

#define TWL6040_HSL_VOL_MASK	0x0F
#define TWL6040_HSL_VOL_SHIFT	0
#define TWL6040_HSR_VOL_MASK	0xF0
#define TWL6040_HSR_VOL_SHIFT	4
#define TWL6040_HF_VOL_MASK	0x1F
#define TWL6040_HF_VOL_SHIFT	0
#define TWL6040_EP_VOL_MASK	0x1E
#define TWL6040_EP_VOL_SHIFT	1

#undef TI_HS_JACK

struct twl6040_output {
	u16 active;
	u16 left_vol;
	u16 right_vol;
	u16 left_step;
	u16 right_step;
	unsigned int step_delay;
	u16 ramp;
	struct delayed_work work;
	struct completion ramp_done;
};

struct twl6040_jack_data {
	struct snd_soc_jack *jack;
	struct delayed_work work;
	int report;
};

/* codec private data */
struct twl6040_data {
#ifdef TI_HS_JACK
	struct wake_lock wake_lock;
#endif
	int codec_powered;
	int pll;
	int power_mode_forced;
	int hs_power_mode;
	int hs_power_mode_locked;
	unsigned int clk_in;
	unsigned int sysclk;
#ifdef CONFIG_REGULATOR_TPS6130X
	struct regulator *vddhf_reg;
#endif
	u16 hs_left_step;
	u16 hs_right_step;
	u16 hf_left_step;
	u16 hf_right_step;
	u16 ep_step;
	struct snd_pcm_hw_constraint_list *sysclk_constraints;
	struct twl6040_jack_data hs_jack;
	struct snd_soc_codec *codec;
	struct workqueue_struct *workqueue;
	struct mutex mutex;
#ifdef CONFIG_REGULATOR_TPS6130X
	int hfdrv;
#endif
	struct twl6040_output headset;
	struct twl6040_output earphone;
	struct twl6040_output handsfree;
};

/*
 * twl6040 register cache & default register settings
 */
static const u8 twl6040_reg[TWL6040_CACHEREGNUM] = {
	0x00, /* not used		0x00	*/
	0x4B, /* TWL6040_ASICID (ro)	0x01	*/
	0x00, /* TWL6040_ASICREV (ro)	0x02	*/
	0x00, /* TWL6040_INTID		0x03	*/
	0x00, /* TWL6040_INTMR		0x04	*/
	0x00, /* TWL6040_NCPCTRL	0x05	*/
	0x00, /* TWL6040_LDOCTL		0x06	*/
	0x60, /* TWL6040_HPPLLCTL	0x07	*/
	0x00, /* TWL6040_LPPLLCTL	0x08	*/
	0x4A, /* TWL6040_LPPLLDIV	0x09	*/
	0x00, /* TWL6040_AMICBCTL	0x0A	*/
	0x00, /* TWL6040_DMICBCTL	0x0B	*/
	0x18, /* TWL6040_MICLCTL	0x0C	- No input selected on Left Mic */
	0x18, /* TWL6040_MICRCTL	0x0D	- No input selected on Right Mic */
	0x00, /* TWL6040_MICGAIN	0x0E	*/
	0x1B, /* TWL6040_LINEGAIN	0x0F	*/
	0x00, /* TWL6040_HSLCTL		0x10	*/
	0x00, /* TWL6040_HSRCTL		0x11	*/
	0xFF, /* TWL6040_HSGAIN		0x12	*/
	0x1E, /* TWL6040_EARCTL		0x13	*/
	0x00, /* TWL6040_HFLCTL		0x14	*/
	0x1D, /* TWL6040_HFLGAIN	0x15	*/
	0x00, /* TWL6040_HFRCTL		0x16	*/
	0x1D, /* TWL6040_HFRGAIN	0x17	*/
	0x00, /* TWL6040_VIBCTLL	0x18	*/
	0x00, /* TWL6040_VIBDATL	0x19	*/
	0x00, /* TWL6040_VIBCTLR	0x1A	*/
	0x00, /* TWL6040_VIBDATR	0x1B	*/
	0x00, /* TWL6040_HKCTL1		0x1C	*/
	0x00, /* TWL6040_HKCTL2		0x1D	*/
	0x00, /* TWL6040_GPOCTL		0x1E	*/
	0x00, /* TWL6040_ALB		0x1F	*/
	0x00, /* TWL6040_DLB		0x20	*/
	0x00, /* not used		0x21	*/
	0x00, /* not used		0x22	*/
	0x00, /* not used		0x23	*/
	0x00, /* not used		0x24	*/
	0x00, /* not used		0x25	*/
	0x00, /* not used		0x26	*/
	0x00, /* not used		0x27	*/
	0x00, /* TWL6040_TRIM1		0x28	*/
	0x00, /* TWL6040_TRIM2		0x29	*/
	0x00, /* TWL6040_TRIM3		0x2A	*/
	0x00, /* TWL6040_HSOTRIM	0x2B	*/
	0x00, /* TWL6040_HFOTRIM	0x2C	*/
	0x09, /* TWL6040_ACCCTL		0x2D	*/
	0x00, /* TWL6040_STATUS (ro)	0x2E	*/
};


/* twl6040 vio/gnd registers: registers under vio/gnd supply can be accessed
 * twl6040 vdd/vss registers: registers under vdd/vss supplies can only be
 * accessed after the power-up sequence */

static const u8 twl6040_reg_supply[TWL6040_CACHEREGNUM] = {
	TWL6040_NO_SUPPLY,  /* not used			*/
	TWL6040_VIO_SUPPLY, /* TWL6040_ASICID (ro)	*/
	TWL6040_VIO_SUPPLY, /* TWL6040_ASICREV (ro)	*/
	TWL6040_VIO_SUPPLY, /* TWL6040_INTID		*/
	TWL6040_VIO_SUPPLY, /* TWL6040_INTMR		*/
	TWL6040_VIO_SUPPLY, /* TWL6040_NCPCTRL		*/
	TWL6040_VIO_SUPPLY, /* TWL6040_LDOCTL		*/
	TWL6040_VDD_SUPPLY, /* TWL6040_HPPLLCTL		*/
	TWL6040_VDD_SUPPLY, /* TWL6040_LPPLLCTL		*/
	TWL6040_VDD_SUPPLY, /* TWL6040_LPPLLDIV		*/
	TWL6040_VIO_SUPPLY, /* TWL6040_AMICBCTL		*/
	TWL6040_VIO_SUPPLY, /* TWL6040_DMICBCTL		*/
	TWL6040_VDD_SUPPLY, /* TWL6040_MICLCTL		*/
	TWL6040_VDD_SUPPLY, /* TWL6040_MICRCTL		*/
	TWL6040_VDD_SUPPLY, /* TWL6040_MICGAIN		*/
	TWL6040_VDD_SUPPLY, /* TWL6040_LINEGAIN		*/
	TWL6040_VDD_SUPPLY, /* TWL6040_HSLCTL		*/
	TWL6040_VDD_SUPPLY, /* TWL6040_HSRCTL		*/
	TWL6040_VDD_SUPPLY, /* TWL6040_HSGAIN		*/
	TWL6040_VDD_SUPPLY, /* TWL6040_EARCTL		*/
	TWL6040_VDD_SUPPLY, /* TWL6040_HFLCTL		*/
	TWL6040_VDD_SUPPLY, /* TWL6040_HFLGAIN		*/
	TWL6040_VDD_SUPPLY, /* TWL6040_HFRCTL		*/
	TWL6040_VDD_SUPPLY, /* TWL6040_HFRGAIN		*/
	TWL6040_VDD_SUPPLY, /* TWL6040_VIBCTLL		*/
	TWL6040_VDD_SUPPLY, /* TWL6040_VIBDATL		*/
	TWL6040_VDD_SUPPLY, /* TWL6040_VIBCTLR		*/
	TWL6040_VDD_SUPPLY, /* TWL6040_VIBDATR		*/
	TWL6040_VIO_SUPPLY, /* TWL6040_HKCTL1		*/
	TWL6040_VIO_SUPPLY, /* TWL6040_HKCTL2		*/
	TWL6040_VIO_SUPPLY, /* TWL6040_GPOCTL		*/
	TWL6040_VDD_SUPPLY, /* TWL6040_ALB		*/
	TWL6040_VDD_SUPPLY, /* TWL6040_DLB		*/
	TWL6040_NO_SUPPLY,  /* not used			*/
	TWL6040_NO_SUPPLY,  /* not used			*/
	TWL6040_NO_SUPPLY,  /* not used			*/
	TWL6040_NO_SUPPLY,  /* not used			*/
	TWL6040_NO_SUPPLY,  /* not used			*/
	TWL6040_NO_SUPPLY,  /* not used			*/
	TWL6040_NO_SUPPLY,  /* not used			*/
	TWL6040_VIO_SUPPLY, /* TWL6040_TRIM1		*/
	TWL6040_VIO_SUPPLY, /* TWL6040_TRIM2		*/
	TWL6040_VIO_SUPPLY, /* TWL6040_TRIM3		*/
	TWL6040_VIO_SUPPLY, /* TWL6040_HSOTRIM		*/
	TWL6040_VIO_SUPPLY, /* TWL6040_HFOTRIM		*/
	TWL6040_VIO_SUPPLY, /* TWL6040_ACCCTL		*/
	TWL6040_VIO_SUPPLY, /* TWL6040_STATUS (ro)	*/
};

/*
 * read twl6040 register cache
 */
static inline unsigned int twl6040_read_reg_cache(struct snd_soc_codec *codec,
						unsigned int reg)
{
	u8 *cache = codec->reg_cache;

	if (reg >= TWL6040_CACHEREGNUM)
		return -EIO;

	return cache[reg];
}

/*
 * write twl6040 register cache
 */
static inline void twl6040_write_reg_cache(struct snd_soc_codec *codec,
						u8 reg, u8 value)
{
	u8 *cache = codec->reg_cache;

	if (reg >= TWL6040_CACHEREGNUM)
		return;
	cache[reg] = value;
}

/*
 * read from twl6040 hardware register
 */
static int twl6040_read_reg_volatile(struct snd_soc_codec *codec,
					unsigned int reg)
{
	struct twl6040 *twl6040 = codec->control_data;
	struct twl6040_data *priv = snd_soc_codec_get_drvdata(codec);
	u8 value = 0;

	if (reg >= TWL6040_CACHEREGNUM)
		return -EIO;

	/* read access not supported while in sleep state */
	if ((twl6040_reg_supply[reg] == TWL6040_VDD_SUPPLY) &&
		!priv->codec_powered)
		return -EINVAL;

	value = twl6040_reg_read(twl6040, reg);
	twl6040_write_reg_cache(codec, reg, value);

	return value;
}

/*
 * write to the twl6040 register space
 */
static int twl6040_write(struct snd_soc_codec *codec,
			unsigned int reg, unsigned int value)
{
	struct twl6040 *twl6040 = codec->control_data;
	struct twl6040_data *priv = snd_soc_codec_get_drvdata(codec);
	int ret = 0;

	if (reg >= TWL6040_CACHEREGNUM)
		return -EIO;

	twl6040_write_reg_cache(codec, reg, value);

	if ((twl6040_reg_supply[reg] == TWL6040_VIO_SUPPLY) ||
		priv->codec_powered)
		ret = twl6040_reg_write(twl6040, reg, value);
	else
		dev_dbg(codec->dev, "deferring register 0x%02x write: %02x\n",
			reg, value);

	return ret;
}

static void twl6040_init_vio_regs(struct snd_soc_codec *codec)
{
	u8 *cache = codec->reg_cache;
	int reg;

	for (reg = 0; reg < TWL6040_CACHEREGNUM; reg++) {
		if (twl6040_reg_supply[reg] != TWL6040_VIO_SUPPLY)
			continue;
		/*
		 * skip read-only registers (ASICID, ASICREV, STATUS)
		 * and registers shared among MFD children
		 */
		switch (reg) {
		case TWL6040_REG_ASICID:
		case TWL6040_REG_ASICREV:
		case TWL6040_REG_INTID:
		case TWL6040_REG_INTMR:
		case TWL6040_REG_NCPCTL:
		case TWL6040_REG_LDOCTL:
		case TWL6040_REG_GPOCTL:
		case TWL6040_REG_ACCCTL:
		case TWL6040_REG_STATUS:
			continue;
		case TWL6040_REG_HSOTRIM:
		case TWL6040_REG_HFOTRIM:
			twl6040_read_reg_volatile(codec, reg);
			continue;
		default:
			break;
		}
		twl6040_write(codec, reg, cache[reg]);
	}
}

static void twl6040_init_vdd_regs(struct snd_soc_codec *codec)
{
	u8 *cache = codec->reg_cache;
	int reg;

	for (reg = 0; reg < TWL6040_CACHEREGNUM; reg++) {
		if (twl6040_reg_supply[reg] != TWL6040_VDD_SUPPLY)
			continue;
		/* skip vibra and pll registers */
		switch (reg) {
		case TWL6040_REG_VIBCTLL:
		case TWL6040_REG_VIBDATL:
		case TWL6040_REG_VIBCTLR:
		case TWL6040_REG_VIBDATR:
		case TWL6040_REG_HPPLLCTL:
		case TWL6040_REG_LPPLLCTL:
		case TWL6040_REG_LPPLLDIV:
			continue;
		default:
			break;
		}
		twl6040_write(codec, reg, cache[reg]);
	}
}

/*
 * Ramp HS PGA volume to minimise pops at stream startup and shutdown.
 */
static inline int twl6040_hs_ramp_step(struct snd_soc_codec *codec,
			unsigned int left_step, unsigned int right_step)
{

	struct twl6040_data *priv = snd_soc_codec_get_drvdata(codec);
	struct twl6040_output *headset = &priv->headset;
	int left_complete = 0, right_complete = 0;
	u8 reg, val;

	/* left channel */
	left_step = (left_step > 0xF) ? 0xF : left_step;
	reg = twl6040_read_reg_cache(codec, TWL6040_REG_HSGAIN);
	val = (~reg & TWL6040_HSL_VOL_MASK);

	if (headset->ramp == TWL6040_RAMP_UP) {
		/* ramp step up */
		if (val != headset->left_vol) {
			if (val + left_step > headset->left_vol)
				val = headset->left_vol;
			else
				val += left_step;

			reg &= ~TWL6040_HSL_VOL_MASK;
			twl6040_write(codec, TWL6040_REG_HSGAIN,
					(reg | (~val & TWL6040_HSL_VOL_MASK)));
		} else {
			left_complete = 1;
		}
	} else if (headset->ramp == TWL6040_RAMP_DOWN) {
		/* ramp step down */
		if (val != 0x0) {
			if ((int)val - (int)left_step < 0)
				val = 0;
			else
				val -= left_step;

			reg &= ~TWL6040_HSL_VOL_MASK;
			twl6040_write(codec, TWL6040_REG_HSGAIN, reg |
						(~val & TWL6040_HSL_VOL_MASK));
		} else {
			left_complete = 1;
		}
	}

	/* right channel */
	right_step = (right_step > 0xF) ? 0xF : right_step;
	reg = twl6040_read_reg_cache(codec, TWL6040_REG_HSGAIN);
	val = (~reg & TWL6040_HSR_VOL_MASK) >> TWL6040_HSR_VOL_SHIFT;

	if (headset->ramp == TWL6040_RAMP_UP) {
		/* ramp step up */
		if (val != headset->right_vol) {
			if (val + right_step > headset->right_vol)
				val = headset->right_vol;
			else
				val += right_step;

			reg &= ~TWL6040_HSR_VOL_MASK;
			twl6040_write(codec, TWL6040_REG_HSGAIN,
				(reg | (~val << TWL6040_HSR_VOL_SHIFT)));
		} else {
			right_complete = 1;
		}
	} else if (headset->ramp == TWL6040_RAMP_DOWN) {
		/* ramp step down */
		if (val != 0x0) {
			if ((int)val - (int)right_step < 0)
				val = 0;
			else
				val -= right_step;

			reg &= ~TWL6040_HSR_VOL_MASK;
			twl6040_write(codec, TWL6040_REG_HSGAIN,
					 reg | (~val << TWL6040_HSR_VOL_SHIFT));
		} else {
			right_complete = 1;
		}
	}

	return left_complete & right_complete;
}

/*
 * Ramp HF PGA volume to minimise pops at stream startup and shutdown.
 */
static inline int twl6040_hf_ramp_step(struct snd_soc_codec *codec,
			unsigned int left_step, unsigned int right_step)
{
	struct twl6040_data *priv = snd_soc_codec_get_drvdata(codec);
	struct twl6040_output *handsfree = &priv->handsfree;
	int left_complete = 0, right_complete = 0;
	u16 reg, val;

	/* left channel */
	left_step = (left_step > 0x1D) ? 0x1D : left_step;
	reg = twl6040_read_reg_cache(codec, TWL6040_REG_HFLGAIN);
	reg = 0x1D - reg;
	val = (reg & TWL6040_HF_VOL_MASK);
	if (handsfree->ramp == TWL6040_RAMP_UP) {
		/* ramp step up */
		if (val != handsfree->left_vol) {
			if (val + left_step > handsfree->left_vol)
				val = handsfree->left_vol;
			else
				val += left_step;

			reg &= ~TWL6040_HF_VOL_MASK;
			twl6040_write(codec, TWL6040_REG_HFLGAIN,
						reg | (0x1D - val));
		} else {
			left_complete = 1;
		}
	} else if (handsfree->ramp == TWL6040_RAMP_DOWN) {
		/* ramp step down */
		if (val != 0) {
			if ((int)val - (int)left_step < 0)
				val = 0;
			else
				val -= left_step;

			reg &= ~TWL6040_HF_VOL_MASK;
			twl6040_write(codec, TWL6040_REG_HFLGAIN,
						reg | (0x1D - val));
		} else {
			left_complete = 1;
		}
	}

	/* right channel */
	right_step = (right_step > 0x1D) ? 0x1D : right_step;
	reg = twl6040_read_reg_cache(codec, TWL6040_REG_HFRGAIN);
	reg = 0x1D - reg;
	val = (reg & TWL6040_HF_VOL_MASK);
	if (handsfree->ramp == TWL6040_RAMP_UP) {
		/* ramp step up */
		if (val != handsfree->right_vol) {
			if (val + right_step > handsfree->right_vol)
				val = handsfree->right_vol;
			else
				val += right_step;

			reg &= ~TWL6040_HF_VOL_MASK;
			twl6040_write(codec, TWL6040_REG_HFRGAIN,
						reg | (0x1D - val));
		} else {
			right_complete = 1;
		}
	} else if (handsfree->ramp == TWL6040_RAMP_DOWN) {
		/* ramp step down */
		if (val != 0) {
			if ((int)val - (int)right_step < 0)
				val = 0;
			else
				val -= right_step;

			reg &= ~TWL6040_HF_VOL_MASK;
			twl6040_write(codec, TWL6040_REG_HFRGAIN,
						reg | (0x1D - val));
		} else {
			right_complete = 1;
		}
	}

	return left_complete & right_complete;
}

/*
 * Ramp Earpiece PGA volume to minimise pops at stream startup and shutdown.
 */
static inline int twl6040_ep_ramp_step(struct snd_soc_codec *codec,
			unsigned int step)
{

	struct twl6040_data *priv = snd_soc_codec_get_drvdata(codec);
	struct twl6040_output *earphone = &priv->earphone;
	int complete = 0;
	u8 reg, val;

	step = (step > 0xF) ? 0xF : step;
	reg = twl6040_read_reg_cache(codec, TWL6040_REG_EARCTL);
	val = (~reg & TWL6040_EP_VOL_MASK) >> TWL6040_EP_VOL_SHIFT;

	if (earphone->ramp == TWL6040_RAMP_UP) {
		/* ramp step up */
		if (val != earphone->left_vol) {
			if (val + step > earphone->left_vol)
				val = earphone->left_vol;
			else
				val += step;

			reg &= ~TWL6040_EP_VOL_MASK;
			val = ~val << TWL6040_EP_VOL_SHIFT;
			twl6040_write(codec, TWL6040_REG_EARCTL,
				reg | (val & TWL6040_EP_VOL_MASK));
		} else {
			complete = 1;
		}
	} else if (earphone->ramp == TWL6040_RAMP_DOWN) {
		/* ramp step down */
		if (val != 0x0) {
			if ((int)val - (int)step < 0)
				val = 0;
			else
				val -= step;

			reg &= ~TWL6040_EP_VOL_MASK;
			val = ~val << TWL6040_EP_VOL_SHIFT;
			twl6040_write(codec, TWL6040_REG_EARCTL,
				reg | (val & TWL6040_EP_VOL_MASK));
		} else {
			complete = 1;
		}
	}

	return complete;
}

/*
 * This work ramps both output PGAs at stream start/stop time to
 * minimise pop associated with DAPM power switching.
 */
static void twl6040_pga_hs_work(struct work_struct *work)
{
	struct twl6040_data *priv =
		container_of(work, struct twl6040_data, headset.work.work);
	struct snd_soc_codec *codec = priv->codec;
	struct twl6040_output *headset = &priv->headset;
	int i, headset_complete;

	/* do we need to ramp at all ? */
	if (headset->ramp == TWL6040_RAMP_NONE)
		return;

	/* HS PGA gain range: 0x0 - 0xf (0 - 15) */
	for (i = 0; i < 16; i++) {
		headset_complete = twl6040_hs_ramp_step(codec,
						headset->left_step,
						headset->right_step);

		/* ramp finished ? */
		if (headset_complete)
			break;

		schedule_timeout_interruptible(
				msecs_to_jiffies(headset->step_delay));
	}

	if (headset->ramp == TWL6040_RAMP_DOWN) {
		headset->active = 0;
		complete(&headset->ramp_done);
	} else {
		headset->active = 1;
	}
	headset->ramp = TWL6040_RAMP_NONE;
}

static void twl6040_pga_hf_work(struct work_struct *work)
{
	struct twl6040_data *priv =
		container_of(work, struct twl6040_data, handsfree.work.work);
	struct snd_soc_codec *codec = priv->codec;
	struct twl6040_output *handsfree = &priv->handsfree;
	int i, handsfree_complete;

	/* do we need to ramp at all ? */
	if (handsfree->ramp == TWL6040_RAMP_NONE)
		return;

	/*
	 * HF PGA gain range: 0x00 - 0x1d (0 - 29) */
	for (i = 0; i < 30; i++) {
		handsfree_complete = twl6040_hf_ramp_step(codec,
						handsfree->left_step,
						handsfree->right_step);

		/* ramp finished ? */
		if (handsfree_complete)
			break;

		schedule_timeout_interruptible(
				msecs_to_jiffies(handsfree->step_delay));
	}


	if (handsfree->ramp == TWL6040_RAMP_DOWN) {
		handsfree->active = 0;
		complete(&handsfree->ramp_done);
	} else
		handsfree->active = 1;
	handsfree->ramp = TWL6040_RAMP_NONE;
}

static void twl6040_pga_ep_work(struct work_struct *work)
{
	struct twl6040_data *priv =
		container_of(work, struct twl6040_data, earphone.work.work);
	struct snd_soc_codec *codec = priv->codec;
	struct twl6040_output *earphone = &priv->earphone;
	int i, earphone_complete;

	/* do we need to ramp at all ? */
	if (earphone->ramp == TWL6040_RAMP_NONE)
		return;

	/* Earphone PGA gain range: 0x0 - 0xf (0 - 15) */
	for (i = 0; i < 16; i++) {
		earphone_complete = twl6040_ep_ramp_step(codec,
						earphone->left_step);

		/* ramp finished ? */
		if (earphone_complete)
			break;

		schedule_timeout_interruptible(
				msecs_to_jiffies(earphone->step_delay));
	}

	if (earphone->ramp == TWL6040_RAMP_DOWN) {
		earphone->active = 0;
		complete(&earphone->ramp_done);
	} else {
		earphone->active = 1;
	}
	earphone->ramp = TWL6040_RAMP_NONE;
}

static int out_drv_event(struct snd_soc_dapm_widget *w,
			struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;
	struct twl6040_data *priv = snd_soc_codec_get_drvdata(codec);
	struct twl6040_output *out;
	struct delayed_work *work;

	switch (w->shift) {
	case 0: /* Earphone driver */
		out = &priv->earphone;
		work = &out->work;
		/*
		 * Make sure, that we do not mess up variables for already
		 * executing work.
		 */
		cancel_delayed_work_sync(work);
		out->left_step = priv->ep_step;
		out->step_delay = 5;	/* 5 ms between volume ramp steps */
		break;
	case 2: /* Headset output driver */
		out = &priv->headset;
		work = &out->work;
		/*
		 * Make sure, that we do not mess up variables for already
		 * executing work.
		 */
		cancel_delayed_work_sync(work);

		out->left_step = priv->hs_left_step;
		out->right_step = priv->hs_right_step;
		out->step_delay = 5;	/* 5 ms between volume ramp steps */
		break;
	case 4: /* Handsfree output driver */
	case 6: /* Aux output driver */
		out = &priv->handsfree;
		work = &out->work;
		/*
		 * Make sure, that we do not mess up variables for already
		 * executing work.
		 */
		cancel_delayed_work_sync(work);

		out->left_step = priv->hf_left_step;
		out->right_step = priv->hf_right_step;
		out->step_delay = 5;	/* 5 ms between volume ramp steps */
		break;
	default:
		return -1;
	}

	switch (event) {
	case SND_SOC_DAPM_POST_PMU:
		if (out->active)
			break;

		/* don't use volume ramp for power-up */
		out->ramp = TWL6040_RAMP_UP;
		out->left_step = out->left_vol;
		out->right_step = out->right_vol;

		queue_delayed_work(priv->workqueue, work, msecs_to_jiffies(1));
		break;

	case SND_SOC_DAPM_PRE_PMD:
		if (!out->active)
			break;

		/* use volume ramp for power-down */
		out->ramp = TWL6040_RAMP_DOWN;
		INIT_COMPLETION(out->ramp_done);

		queue_delayed_work(priv->workqueue, work, msecs_to_jiffies(1));

		wait_for_completion_timeout(&out->ramp_done,
					    msecs_to_jiffies(2000));
		break;
	}

	/*
	 * 1 ms to sync with ramp and work with POST_PMU/PRE_PMD
	 * to ensure the out->active state will be updated properly.
	 */
	usleep_range(1000, 2000);
	return 0;
}

/* set headset dac and driver power mode */
static int headset_power_mode(struct snd_soc_codec *codec, int high_perf)
{
	int hslctl, hsrctl;
	int mask = TWL6040_HSDRVMODEL | TWL6040_HSDACMODEL;
	int val;

	hslctl = snd_soc_read(codec, TWL6040_REG_HSLCTL);
	hsrctl = snd_soc_read(codec, TWL6040_REG_HSRCTL);

	if ((hslctl & TWL6040_HSDACENAL) || (hsrctl & TWL6040_HSDACENAR)) {
		dev_err(codec->dev,
			"mode change not allowed when HSDACs are active\n");
		return -EPERM;
	}

	if (high_perf)
		val = 0;
	else
		val = mask;

	snd_soc_update_bits(codec, TWL6040_REG_HSLCTL, mask, val);
	snd_soc_update_bits(codec, TWL6040_REG_HSRCTL, mask, val);

	return 0;
}

static int twl6040_hs_dac_left_event(struct snd_soc_dapm_widget *w,
			struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;
	struct twl6040 *twl6040 = codec->control_data;
	int hsrctl;

	/* SW Workaround for DC Offset On EAR Differential Output Errata */
	if (twl6040_get_icrev(twl6040) <= TWL6041_REV_2_0) {
		hsrctl = twl6040_read_reg_cache(codec, TWL6040_REG_HSRCTL);
		switch (event) {
		case SND_SOC_DAPM_PRE_PMU:
			/* HSDACL reset is done when HSDACR is enabled */
			twl6040_reg_write(twl6040, TWL6040_REG_HSRCTL,
					  hsrctl | TWL6040_HSDACENAR);
			break;

		case SND_SOC_DAPM_POST_PMU:
			 /* Sync HSDACR with reg cache */
			twl6040_reg_write(twl6040, TWL6040_REG_HSRCTL, hsrctl);
			 /* Fall through */
		case SND_SOC_DAPM_POST_PMD:
			/* HSDAC settling time */
			usleep_range(80, 200);
			break;

		default:
			break;
		}
	}

	return 0;
}

static int twl6040_hs_dac_right_event(struct snd_soc_dapm_widget *w,
			struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;
	struct twl6040 *twl6040 = codec->control_data;
	int hslctl;

	/* SW Workaround for DC Offset On EAR Differential Output Errata */
	if (twl6040_get_icrev(twl6040) < TWL6040_REV_1_3) {
		hslctl = twl6040_read_reg_cache(codec, TWL6040_REG_HSLCTL);
		switch (event) {
		case SND_SOC_DAPM_PRE_PMD:
			/* HSDACR reset is done when HSDACL is enabled */
			twl6040_reg_write(twl6040, TWL6040_REG_HSLCTL,
					  hslctl | TWL6040_HSDACENAL);
			break;
		case SND_SOC_DAPM_POST_PMD:
			/* Sync HSDACL with reg cache */
			twl6040_reg_write(twl6040, TWL6040_REG_HSLCTL, hslctl);
			/* Fall through */
		case SND_SOC_DAPM_POST_PMU:
			/* HSDAC settling time */
			usleep_range(80, 200);
			break;

		default:
			break;
		}
	}

	return 0;
}

static int twl6040_hf_dac_event(struct snd_soc_dapm_widget *w,
			struct snd_kcontrol *kcontrol, int event)
{
	/* HFDAC settling time */
	usleep_range(80, 200);

	return 0;
}

static int twl6040_ep_mode_event(struct snd_soc_dapm_widget *w,
			struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;
	struct twl6040_data *priv = snd_soc_codec_get_drvdata(codec);
	int ret = 0;

	if (SND_SOC_DAPM_EVENT_ON(event)) {
		/* Earphone doesn't support low power mode */
		priv->hs_power_mode_locked = 1;
		ret = headset_power_mode(codec, 1);
	} else {
		priv->hs_power_mode_locked = 0;
		ret = headset_power_mode(codec, priv->hs_power_mode);
	}

	return ret;
}

#ifdef CONFIG_REGULATOR_TPS6130X

static int twl6040_hf_boost_event(struct snd_soc_dapm_widget *w,
			struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;
	struct twl6040_data *priv = snd_soc_codec_get_drvdata(codec);
	int ret;

	if (!priv->vddhf_reg)
		return 0;

	if (SND_SOC_DAPM_EVENT_ON(event)) {
		ret = regulator_enable(priv->vddhf_reg);
		if (ret) {
			dev_err(codec->dev, "failed to enable "
				"VDDHF regulator %d\n", ret);
			return ret;
		}
	} else {
		ret = regulator_disable(priv->vddhf_reg);
		if (ret) {
			dev_err(codec->dev, "failed to disable "
				"VDDHF regulator %d\n", ret);
			return ret;
		}
	}

	return ret;
}
#endif

static void twl6040_hs_jack_report(struct snd_soc_codec *codec,
				   struct snd_soc_jack *jack, int report)
{
	struct twl6040_data *priv = snd_soc_codec_get_drvdata(codec);
	int status;

	mutex_lock(&priv->mutex);

	/* Sync status */
	status = twl6040_read_reg_volatile(codec, TWL6040_REG_STATUS);
	if (status & TWL6040_PLUGCOMP)
		snd_soc_jack_report(jack, report, report);
	else
		snd_soc_jack_report(jack, 0, report);

	mutex_unlock(&priv->mutex);
}

void twl6040_hs_jack_detect(struct snd_soc_codec *codec,
				struct snd_soc_jack *jack, int report)
{
	struct twl6040_data *priv = snd_soc_codec_get_drvdata(codec);
	struct twl6040_jack_data *hs_jack = &priv->hs_jack;

	hs_jack->jack = jack;
	hs_jack->report = report;

	twl6040_hs_jack_report(codec, hs_jack->jack, hs_jack->report);
}
EXPORT_SYMBOL_GPL(twl6040_hs_jack_detect);

static void twl6040_accessory_work(struct work_struct *work)
{
	struct twl6040_data *priv = container_of(work,
					struct twl6040_data, hs_jack.work.work);
	struct snd_soc_codec *codec = priv->codec;
	struct twl6040_jack_data *hs_jack = &priv->hs_jack;

	twl6040_hs_jack_report(codec, hs_jack->jack, hs_jack->report);
}

/* audio interrupt handler */
static irqreturn_t twl6040_audio_handler(int irq, void *data)
{
	struct snd_soc_codec *codec = data;
	struct twl6040 *twl6040 = codec->control_data;
	struct twl6040_data *priv = snd_soc_codec_get_drvdata(codec);
	u8 intid , val;

	intid = twl6040_reg_read(twl6040, TWL6040_REG_INTID);

	if ((intid & TWL6040_PLUGINT) || (intid & TWL6040_UNPLUGINT)) {
#ifdef TI_HS_JACK
		wake_lock_timeout(&priv->wake_lock, 2 * HZ);
#endif
		queue_delayed_work(priv->workqueue, &priv->hs_jack.work,
				   msecs_to_jiffies(200));
	}
       if (intid & TWL6040_HFINT) {
               val = twl6040_read_reg_volatile(codec, TWL6040_REG_STATUS);
               if (val & TWL6040_HFLOCDET)
                       dev_err(codec->dev, "Left Handsfree overcurrent\n");
               if (val & TWL6040_HFROCDET)
                       dev_err(codec->dev, "Right Handsfree overcurrent\n");

               val = twl6040_read_reg_cache(codec, TWL6040_REG_HFLCTL);
               twl6040_write(codec, TWL6040_REG_HFLCTL,
                               val & ~TWL6040_HFDRVENAL);

               val = twl6040_read_reg_cache(codec, TWL6040_REG_HFRCTL);
               twl6040_write(codec, TWL6040_REG_HFRCTL,
                               val & ~TWL6040_HFDRVENAR);

               twl6040_report_event(twl6040, TWL6040_HFOC_EVENT);
       }

	return IRQ_HANDLED;
}

static int twl6040_put_volsw(struct snd_kcontrol *kcontrol,
				  struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_codec *codec = snd_kcontrol_chip(kcontrol);
	struct twl6040_data *twl6040_priv = snd_soc_codec_get_drvdata(codec);
	struct twl6040_output *out = NULL;
	struct soc_mixer_control *mc =
		(struct soc_mixer_control *)kcontrol->private_value;
	int ret, type_2r;

	/* For HS, HF and EP we shadow the values and only actually write
	 * them out when active in order to ensure the amplifier comes on
	 * as quietly as possible. */
	switch (mc->reg) {
	case TWL6040_REG_HSGAIN:
		out = &twl6040_priv->headset;
		type_2r = 0;
		break;
	case TWL6040_REG_HFLGAIN:
		out = &twl6040_priv->handsfree;
		type_2r = 1;
		break;
	case TWL6040_REG_EARCTL:
		out = &twl6040_priv->earphone;
		type_2r = 0;
		break;
	default:
		dev_warn(codec->dev, "%s: Unexpected register: 0x%02x\n",
					__func__, mc->reg);
		return -EINVAL;
	}

	out->left_vol = ucontrol->value.integer.value[0];
	out->right_vol = ucontrol->value.integer.value[1];
	if (!out->active)
		return 1;

	if (type_2r)
		ret = snd_soc_put_volsw_2r(kcontrol, ucontrol);
	else
		ret = snd_soc_put_volsw(kcontrol, ucontrol);

	if (ret < 0)
		return ret;

	return 1;
}

static int twl6040_get_volsw(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_codec *codec = snd_kcontrol_chip(kcontrol);
	struct twl6040_data *twl6040_priv = snd_soc_codec_get_drvdata(codec);
	struct twl6040_output *out = &twl6040_priv->headset;
	struct soc_mixer_control *mc =
		(struct soc_mixer_control *)kcontrol->private_value;

	switch (mc->reg) {
	case TWL6040_REG_HSGAIN:
		out = &twl6040_priv->headset;
		ucontrol->value.integer.value[0] = out->left_vol;
		ucontrol->value.integer.value[1] = out->right_vol;
		break;
	case TWL6040_REG_HFLGAIN:
		out = &twl6040_priv->handsfree;
		ucontrol->value.integer.value[0] = out->left_vol;
		ucontrol->value.integer.value[1] = out->right_vol;
		break;
	case TWL6040_REG_EARCTL:
		out = &twl6040_priv->earphone;
		ucontrol->value.integer.value[0] = out->left_vol;
		break;
	default:
		dev_warn(codec->dev, "%s: Unexpected register: 0x%02x\n",
					__func__, mc->reg);
		return -EINVAL;
	}

	return 0;
}

/*
 * MICATT volume control:
 * from -6 to 0 dB in 6 dB steps
 */
static DECLARE_TLV_DB_SCALE(mic_preamp_tlv, -600, 600, 0);

/*
 * MICGAIN volume control:
 * from 6 to 30 dB in 6 dB steps
 */
static DECLARE_TLV_DB_SCALE(mic_amp_tlv, 600, 600, 0);

/*
 * AFMGAIN volume control:
 * from -18 to 24 dB in 6 dB steps
 */
static DECLARE_TLV_DB_SCALE(afm_amp_tlv, -1800, 600, 0);

/*
 * HSGAIN volume control:
 * from -30 to 0 dB in 2 dB steps
 */
static DECLARE_TLV_DB_SCALE(hs_tlv, -3000, 200, 0);

/*
 * HFGAIN volume control:
 * from -52 to 6 dB in 2 dB steps
 */
static DECLARE_TLV_DB_SCALE(hf_tlv, -5200, 200, 0);

/*
 * EPGAIN volume control:
 * from -24 to 6 dB in 2 dB steps
 */
static DECLARE_TLV_DB_SCALE(ep_tlv, -2400, 200, 0);

/* Left analog microphone selection */
static const char *twl6040_amicl_texts[] =
	{"Headset Mic", "Main Mic", "Aux/FM Left", "Off"};

/* Right analog microphone selection */
static const char *twl6040_amicr_texts[] =
	{"Headset Mic", "Sub Mic", "Aux/FM Right", "Off"};

static const struct soc_enum twl6040_enum[] = {
	SOC_ENUM_SINGLE(TWL6040_REG_MICLCTL, 3, 4, twl6040_amicl_texts),
	SOC_ENUM_SINGLE(TWL6040_REG_MICRCTL, 3, 4, twl6040_amicr_texts),
};

static const char *twl6040_hs_texts[] = {
	"Off", "HS DAC", "Line-In amp"
};

static const struct soc_enum twl6040_hs_enum[] = {
	SOC_ENUM_SINGLE(TWL6040_REG_HSLCTL, 5, ARRAY_SIZE(twl6040_hs_texts),
			twl6040_hs_texts),
	SOC_ENUM_SINGLE(TWL6040_REG_HSRCTL, 5, ARRAY_SIZE(twl6040_hs_texts),
			twl6040_hs_texts),
};

static const char *twl6040_hf_texts[] = {
	"Off", "HF DAC", "Line-In amp"
};

static const struct soc_enum twl6040_hf_enum[] = {
	SOC_ENUM_SINGLE(TWL6040_REG_HFLCTL, 2, ARRAY_SIZE(twl6040_hf_texts),
			twl6040_hf_texts),
	SOC_ENUM_SINGLE(TWL6040_REG_HFRCTL, 2, ARRAY_SIZE(twl6040_hf_texts),
			twl6040_hf_texts),
};

static const struct snd_kcontrol_new amicl_control =
	SOC_DAPM_ENUM("Route", twl6040_enum[0]);

static const struct snd_kcontrol_new amicr_control =
	SOC_DAPM_ENUM("Route", twl6040_enum[1]);

/* Headset DAC playback switches */
static const struct snd_kcontrol_new hsl_mux_controls =
	SOC_DAPM_ENUM("Route", twl6040_hs_enum[0]);

static const struct snd_kcontrol_new hsr_mux_controls =
	SOC_DAPM_ENUM("Route", twl6040_hs_enum[1]);

/* Handsfree DAC playback switches */
static const struct snd_kcontrol_new hfl_mux_controls =
	SOC_DAPM_ENUM("Route", twl6040_hf_enum[0]);

static const struct snd_kcontrol_new hfr_mux_controls =
	SOC_DAPM_ENUM("Route", twl6040_hf_enum[1]);

/* AUX DAC playback switches */
static const struct snd_kcontrol_new auxl_mux_controls =
	SOC_DAPM_ENUM("Route", twl6040_hf_enum[0]);

static const struct snd_kcontrol_new auxr_mux_controls =
	SOC_DAPM_ENUM("Route", twl6040_hf_enum[1]);

/* Earphone playback switch */
static const struct snd_kcontrol_new ep_driver_switch_controls =
	SOC_DAPM_SINGLE("Switch", TWL6040_REG_EARCTL, 0, 1, 0);

/* Headset power mode */
static const char *twl6040_headset_power_texts[] = {
	"Low-Power", "High-Performance",
};

static const struct soc_enum twl6040_headset_power_enum =
	SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(twl6040_headset_power_texts),
			twl6040_headset_power_texts);

static int twl6040_headset_power_get_enum(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_codec *codec = snd_kcontrol_chip(kcontrol);
	struct twl6040_data *priv = snd_soc_codec_get_drvdata(codec);

	ucontrol->value.enumerated.item[0] = priv->hs_power_mode;

	return 0;
}

static int twl6040_headset_power_put_enum(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_codec *codec = snd_kcontrol_chip(kcontrol);
	struct twl6040_data *priv = snd_soc_codec_get_drvdata(codec);
	int high_perf = ucontrol->value.enumerated.item[0];
	int ret;

	if (!priv->hs_power_mode_locked)
		ret = headset_power_mode(codec, high_perf);

	if (!ret)
		priv->hs_power_mode = high_perf;

	return ret;
}

static const struct snd_kcontrol_new twl6040_snd_controls[] = {
	/* Capture gains */
	SOC_DOUBLE_TLV("Capture Preamplifier Volume",
		TWL6040_REG_MICGAIN, 6, 7, 1, 1, mic_preamp_tlv),
	SOC_DOUBLE_TLV("Capture Volume",
		TWL6040_REG_MICGAIN, 0, 3, 4, 0, mic_amp_tlv),

	/* AFM gains */
	SOC_DOUBLE_TLV("Aux FM Volume",
		TWL6040_REG_LINEGAIN, 0, 3, 7, 0, afm_amp_tlv),

	/* Playback gains */
	SOC_DOUBLE_EXT_TLV("Headset Playback Volume",
		TWL6040_REG_HSGAIN, 0, 4, 0xF, 1,
		twl6040_get_volsw, twl6040_put_volsw, hs_tlv),
	SOC_DOUBLE_R_EXT_TLV("Handsfree Playback Volume",
		TWL6040_REG_HFLGAIN, TWL6040_REG_HFRGAIN, 0, 0x1D, 1,
		twl6040_get_volsw, twl6040_put_volsw, hf_tlv),
	SOC_SINGLE_EXT_TLV("Earphone Playback Volume",
		TWL6040_REG_EARCTL, 1, 0xF, 1,
		twl6040_get_volsw, twl6040_put_volsw, ep_tlv),

	SOC_ENUM_EXT("Headset Power Mode", twl6040_headset_power_enum,
		twl6040_headset_power_get_enum,
		twl6040_headset_power_put_enum),
};

static const struct snd_soc_dapm_widget twl6040_dapm_widgets[] = {
	/* Inputs */
	SND_SOC_DAPM_INPUT("MAINMIC"),
	SND_SOC_DAPM_INPUT("HSMIC"),
	SND_SOC_DAPM_INPUT("SUBMIC"),
	SND_SOC_DAPM_INPUT("AFML"),
	SND_SOC_DAPM_INPUT("AFMR"),

	/* Outputs */
	SND_SOC_DAPM_OUTPUT("HSOL"),
	SND_SOC_DAPM_OUTPUT("HSOR"),
	SND_SOC_DAPM_OUTPUT("HFL"),
	SND_SOC_DAPM_OUTPUT("HFR"),
	SND_SOC_DAPM_OUTPUT("AUXL"),
	SND_SOC_DAPM_OUTPUT("AUXR"),
	SND_SOC_DAPM_OUTPUT("EP"),

	/* Analog input muxes for the capture amplifiers */
	SND_SOC_DAPM_MUX("Analog Left Capture Route",
			SND_SOC_NOPM, 0, 0, &amicl_control),
	SND_SOC_DAPM_MUX("Analog Right Capture Route",
			SND_SOC_NOPM, 0, 0, &amicr_control),

	/* Analog capture PGAs */
	SND_SOC_DAPM_PGA("MicAmpL",
			TWL6040_REG_MICLCTL, 0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("MicAmpR",
			TWL6040_REG_MICRCTL, 0, 0, NULL, 0),

	/* Auxiliary FM PGAs */
	SND_SOC_DAPM_PGA("AFMAmpL",
			TWL6040_REG_MICLCTL, 1, 0, NULL, 0),
	SND_SOC_DAPM_PGA("AFMAmpR",
			TWL6040_REG_MICRCTL, 1, 0, NULL, 0),

	/* ADCs */
	SND_SOC_DAPM_ADC("ADC Left", "Left Front Capture",
			TWL6040_REG_MICLCTL, 2, 0),
	SND_SOC_DAPM_ADC("ADC Right", "Right Front Capture",
			TWL6040_REG_MICRCTL, 2, 0),

	/* Microphone bias */
#ifdef CONFIG_TWL6040_CODEC_INTERNAL_MICBIAS
	SND_SOC_DAPM_MICBIAS("Headset Mic Bias",
			TWL6040_REG_AMICBCTL, 0, 0),
	SND_SOC_DAPM_MICBIAS("Main Mic Bias",
			TWL6040_REG_AMICBCTL, 4, 0),
#endif
	SND_SOC_DAPM_MICBIAS("Digital Mic1 Bias",
			TWL6040_REG_DMICBCTL, 0, 0),
	SND_SOC_DAPM_MICBIAS("Digital Mic2 Bias",
			TWL6040_REG_DMICBCTL, 4, 0),

	/* DACs */
	SND_SOC_DAPM_DAC_E("HSDAC Left", "Headset Playback",
			TWL6040_REG_HSLCTL, 0, 0,
			twl6040_hs_dac_left_event,
			SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMU |
			SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_DAC_E("HSDAC Right", "Headset Playback",
			TWL6040_REG_HSRCTL, 0, 0,
			twl6040_hs_dac_right_event,
			SND_SOC_DAPM_POST_PMU |
			SND_SOC_DAPM_PRE_PMD | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_DAC_E("HFDAC Left", "Handsfree Playback",
			TWL6040_REG_HFLCTL, 0, 0,
			twl6040_hf_dac_event,
			SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_DAC_E("HFDAC Right", "Handsfree Playback",
			TWL6040_REG_HFRCTL, 0, 0,
			twl6040_hf_dac_event,
			SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_POST_PMD),

	SND_SOC_DAPM_MUX("HF Left Playback",
			SND_SOC_NOPM, 0, 0, &hfl_mux_controls),
	SND_SOC_DAPM_MUX("HF Right Playback",
			SND_SOC_NOPM, 0, 0, &hfr_mux_controls),
	SND_SOC_DAPM_MUX("AUX Left Playback",
			SND_SOC_NOPM, 0, 0, &auxl_mux_controls),
	SND_SOC_DAPM_MUX("AUX Right Playback",
			SND_SOC_NOPM, 0, 0, &auxr_mux_controls),

	/* Analog playback Muxes */
	SND_SOC_DAPM_MUX("HS Left Playback",
			SND_SOC_NOPM, 0, 0, &hsl_mux_controls),
	SND_SOC_DAPM_MUX("HS Right Playback",
			SND_SOC_NOPM, 0, 0, &hsr_mux_controls),

	/* Analog playback drivers */
	SND_SOC_DAPM_OUT_DRV_E("Handsfree Left Driver",
			TWL6040_REG_HFLCTL, 4, 0, NULL, 0,
			out_drv_event,
			SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_PRE_PMD),
	SND_SOC_DAPM_OUT_DRV_E("Handsfree Right Driver",
			TWL6040_REG_HFRCTL, 4, 0, NULL, 0,
			out_drv_event,
			SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_PRE_PMD),
	SND_SOC_DAPM_OUT_DRV_E("Aux Left Driver",
			TWL6040_REG_HFLCTL, 6, 0, NULL, 0,
			out_drv_event,
			SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_PRE_PMD),
	SND_SOC_DAPM_OUT_DRV_E("Aux Right Driver",
			TWL6040_REG_HFRCTL, 6, 0, NULL, 0,
			out_drv_event,
			SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_PRE_PMD),
	SND_SOC_DAPM_OUT_DRV_E("Headset Left Driver",
			TWL6040_REG_HSLCTL, 2, 0, NULL, 0,
			out_drv_event,
			SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_PRE_PMD),
	SND_SOC_DAPM_OUT_DRV_E("Headset Right Driver",
			TWL6040_REG_HSRCTL, 2, 0, NULL, 0,
			out_drv_event,
			SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_PRE_PMD),
#ifdef CONFIG_REGULATOR_TPS6130X
	SND_SOC_DAPM_SUPPLY("Handsfree Left Boost Supply", SND_SOC_NOPM, 0, 0,
			 twl6040_hf_boost_event,
			 SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_SUPPLY("Handsfree Right Boost Supply", SND_SOC_NOPM, 0, 0,
			 twl6040_hf_boost_event,
			 SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),
#endif
	SND_SOC_DAPM_SWITCH("Earphone Playback",
			SND_SOC_NOPM, 0, 0, &ep_driver_switch_controls),
	SND_SOC_DAPM_SUPPLY("Earphone Power Mode", SND_SOC_NOPM, 0, 0,
			twl6040_ep_mode_event,
			SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_OUT_DRV_E("Earphone Driver",
			SND_SOC_NOPM, 0, 0, NULL, 0,
			out_drv_event,
			SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_PRE_PMD),

	/* Analog playback PGAs */
	SND_SOC_DAPM_PGA("HFDAC Left Aux PGA",
			TWL6040_REG_HFLCTL, 1, 0, NULL, 0),
	SND_SOC_DAPM_PGA("HFDAC Right Aux PGA",
			TWL6040_REG_HFRCTL, 1, 0, NULL, 0),
	SND_SOC_DAPM_PGA("HFDAC Left PGA",
			TWL6040_REG_HFLCTL, 1, 0, NULL, 0),
	SND_SOC_DAPM_PGA("HFDAC Right PGA",
			TWL6040_REG_HFRCTL, 1, 0, NULL, 0),

};

static const struct snd_soc_dapm_route intercon[] = {
	/* Capture path */
	{"Analog Left Capture Route", "Headset Mic", "HSMIC"},
	{"Analog Left Capture Route", "Main Mic", "MAINMIC"},
	{"Analog Left Capture Route", "Aux/FM Left", "AFML"},

	{"Analog Right Capture Route", "Headset Mic", "HSMIC"},
	{"Analog Right Capture Route", "Sub Mic", "SUBMIC"},
	{"Analog Right Capture Route", "Aux/FM Right", "AFMR"},

	{"MicAmpL", NULL, "Analog Left Capture Route"},
	{"MicAmpR", NULL, "Analog Right Capture Route"},

	{"ADC Left", NULL, "MicAmpL"},
	{"ADC Right", NULL, "MicAmpR"},

	/* AFM path */
	{"AFMAmpL", "NULL", "AFML"},
	{"AFMAmpR", "NULL", "AFMR"},

	{"HS Left Playback", "HS DAC", "HSDAC Left"},
	{"HS Left Playback", "Line-In amp", "AFMAmpL"},

	{"HS Right Playback", "HS DAC", "HSDAC Right"},
	{"HS Right Playback", "Line-In amp", "AFMAmpR"},

	{"Headset Left Driver", "NULL", "HS Left Playback"},
	{"Headset Right Driver", "NULL", "HS Right Playback"},

	{"HSOL", NULL, "Headset Left Driver"},
	{"HSOR", NULL, "Headset Right Driver"},

	/* Earphone playback path */
	{"Earphone Playback", "Switch", "HSDAC Left"},
	{"Earphone Playback", NULL, "Earphone Power Mode"},
	{"Earphone Driver", NULL, "Earphone Playback"},
	{"EP", NULL, "Earphone Driver"},

	/* Handsfree Playback path */
	{"HF Left Playback", "HF DAC", "HFDAC Left"},
	{"HF Left Playback", "Line-In amp", "AFMAmpL"},

	{"HF Right Playback", "HF DAC", "HFDAC Right"},
	{"HF Right Playback", "Line-In amp", "AFMAmpR"},

	{"HFDAC Left PGA", NULL, "HF Left Playback"},
	{"HFDAC Right PGA", NULL, "HF Right Playback"},

	{"Handsfree Left Driver", "NULL", "HFDAC Left PGA"},
	{"Handsfree Right Driver", "NULL", "HFDAC Right PGA"},

#ifdef CONFIG_REGULATOR_TPS6130X
	{"Handsfree Left Driver", NULL, "Handsfree Left Boost Supply"},
	{"Handsfree Right Driver", NULL, "Handsfree Right Boost Supply"},
#endif

	{"HFL", NULL, "Handsfree Left Driver"},
	{"HFR", NULL, "Handsfree Right Driver"},

	/* AUX Playback path */
	{"AUX Left Playback", "HF DAC", "HFDAC Left"},
	{"AUX Left Playback", "Line-In amp", "AFMAmpL"},

	{"AUX Right Playback", "HF DAC", "HFDAC Right"},
	{"AUX Right Playback", "Line-In amp", "AFMAmpR"},

	{"HFDAC Left Aux PGA", NULL, "AUX Left Playback"},
	{"HFDAC Right Aux PGA", NULL, "AUX Right Playback"},

	{"Aux Left Driver", "NULL", "HFDAC Left Aux PGA"},
	{"Aux Right Driver", "NULL", "HFDAC Right Aux PGA"},

	{"AUXL", NULL, "Aux Left Driver"},
	{"AUXR", NULL, "Aux Right Driver"},
};

static int twl6040_add_widgets(struct snd_soc_codec *codec)
{
	struct snd_soc_dapm_context *dapm = &codec->dapm;

	snd_soc_dapm_new_controls(dapm, twl6040_dapm_widgets,
				 ARRAY_SIZE(twl6040_dapm_widgets));
	snd_soc_dapm_add_routes(dapm, intercon, ARRAY_SIZE(intercon));
	snd_soc_dapm_new_widgets(dapm);

	return 0;
}

/* set of rates for each pll: low-power and high-performance */

static unsigned int lp_rates[] = {
	8000,
	11250,
	16000,
	22500,
	32000,
	44100,
	48000,
	88200,
	96000,
};

static struct snd_pcm_hw_constraint_list lp_constraints = {
	.count	= ARRAY_SIZE(lp_rates),
	.list	= lp_rates,
};

static unsigned int hp_rates[] = {
	8000,
	16000,
	32000,
	44100,
	48000,
	96000,
};

static struct snd_pcm_hw_constraint_list hp_constraints = {
	.count	= ARRAY_SIZE(hp_rates),
	.list	= hp_rates,
};

static int twl6040_set_bias_level(struct snd_soc_codec *codec,
				enum snd_soc_bias_level level)
{
	struct twl6040 *twl6040 = codec->control_data;
	struct twl6040_data *priv = snd_soc_codec_get_drvdata(codec);

	switch (level) {
	case SND_SOC_BIAS_ON:
		break;
	case SND_SOC_BIAS_PREPARE:
		break;
	case SND_SOC_BIAS_STANDBY:
		if (priv->codec_powered)
			break;

		twl6040_enable(twl6040);
		priv->codec_powered = 1;

		priv->sysclk_constraints = &lp_constraints;

		/* initialize vdd/vss registers with reg_cache */
		twl6040_init_vdd_regs(codec);

		break;
	case SND_SOC_BIAS_OFF:
		if (!priv->codec_powered)
			break;

		twl6040_disable(twl6040);
		priv->codec_powered = 0;
		break;
	}

	codec->dapm.bias_level = level;
	/* get pll and sysclk after power transition */
	priv->pll = twl6040_get_pll(twl6040);
	priv->sysclk = twl6040_get_sysclk(twl6040);

	return 0;
}

static int twl6040_startup(struct snd_pcm_substream *substream,
			struct snd_soc_dai *dai)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_codec *codec = rtd->codec;
	struct twl6040_data *priv = snd_soc_codec_get_drvdata(codec);

	snd_pcm_hw_constraint_list(substream->runtime, 0,
				SNDRV_PCM_HW_PARAM_RATE,
				priv->sysclk_constraints);

	return 0;
}

static int twl6040_hw_params(struct snd_pcm_substream *substream,
			struct snd_pcm_hw_params *params,
			struct snd_soc_dai *dai)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_codec *codec = rtd->codec;
	struct twl6040 *twl6040 = codec->control_data;
	struct twl6040_data *priv = snd_soc_codec_get_drvdata(codec);
	unsigned int sysclk;
	int rate;
	int ret;

	rate = params_rate(params);
	switch (rate) {
	case 11250:
	case 22500:
	case 88200:
		sysclk = 17640000;
		break;
	case 8000:
	case 16000:
	case 32000:
	case 44100:
	case 48000:
	case 96000:
		sysclk = 19200000;
		break;
	default:
		dev_err(codec->dev, "unsupported rate %d\n", rate);
		return -EINVAL;
	}

	ret = twl6040_set_pll(twl6040, priv->pll, priv->clk_in, sysclk);
	if (ret) {
		dev_err(codec->dev, "failed to configure PLL %d", ret);
		return ret;
	}

	priv->sysclk = sysclk;

	return 0;
}

static int twl6040_prepare(struct snd_pcm_substream *substream,
			struct snd_soc_dai *dai)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_codec *codec = rtd->codec;
	struct twl6040_data *priv = snd_soc_codec_get_drvdata(codec);

	if (!priv->sysclk) {
		dev_err(codec->dev,
			"no mclk configured, call set_sysclk() on init\n");
		return -EINVAL;
	}

	/*
	 * In the capture, the Analog path should be turn on and stabilized
	 * before McPDM prepare itself to avoid pop noises.
	 * So the codec startup event is sending through dapm in prepare itself
	 * to ensure that the codec analog path is up before McPDM Uplink FIFO
	 * is going to be activated.
	 */
	if (substream->stream == SNDRV_PCM_STREAM_CAPTURE) {
		snd_soc_dapm_codec_stream_event(dai->codec,
				dai->driver->capture.stream_name,
				SND_SOC_DAPM_STREAM_START);
		msleep(150);
	}

	return 0;
}

static int twl6040_set_dai_sysclk(struct snd_soc_dai *codec_dai,
		int clk_id, unsigned int freq, int dir)
{
	struct snd_soc_codec *codec = codec_dai->codec;
	struct twl6040_data *priv = snd_soc_codec_get_drvdata(codec);

	switch (clk_id) {
	case TWL6040_LPPLL_ID:
		priv->sysclk_constraints = &lp_constraints;
		break;
	case TWL6040_HPPLL_ID:
		priv->sysclk_constraints = &hp_constraints;
		break;
	default:
		dev_err(codec->dev, "unknown clk_id %d\n", clk_id);
		return -EINVAL;
	}

	priv->pll = clk_id;
	priv->clk_in = freq;

	return 0;
}

static int twl6040_digital_mute(struct snd_soc_dai *dai, int mute)
{
	/*
	 * pop-noise reduction sequence requires to shutdown
	 * analog side before CPU DAI
	 */
	if (mute)
		snd_soc_dapm_codec_stream_event(dai->codec,
				dai->driver->playback.stream_name,
				SND_SOC_DAPM_STREAM_STOP);

	return 0;
}

static struct snd_soc_dai_ops twl6040_dai_ops = {
	.startup	= twl6040_startup,
	.hw_params	= twl6040_hw_params,
	.prepare	= twl6040_prepare,
	.set_sysclk	= twl6040_set_dai_sysclk,
	.digital_mute	= twl6040_digital_mute,
};

static struct snd_soc_dai_driver twl6040_dai[] = {
{
	.name = "twl6040-ul",
	.capture = {
		.stream_name = "Capture",
		.channels_min = 1,
		.channels_max = 2,
		.rates = TWL6040_RATES,
		.formats = TWL6040_FORMATS,
	},
	.ops = &twl6040_dai_ops,
},
{
	.name = "twl6040-dl1",
	.playback = {
		.stream_name = "Headset Playback",
		.channels_min = 1,
		.channels_max = 2,
		.rates = TWL6040_RATES,
		.formats = TWL6040_FORMATS,
	},
	.ops = &twl6040_dai_ops,
},
{
	.name = "twl6040-dl2",
	.playback = {
		.stream_name = "Handsfree Playback",
		.channels_min = 1,
		.channels_max = 2,
		.rates = TWL6040_RATES,
		.formats = TWL6040_FORMATS,
	},
	.ops = &twl6040_dai_ops,
},
{
	.name = "twl6040-vib",
	.playback = {
		.stream_name = "Vibra Playback",
		.channels_min = 2,
		.channels_max = 2,
		.rates = SNDRV_PCM_RATE_CONTINUOUS,
		.formats = TWL6040_FORMATS,
	},
	.ops = &twl6040_dai_ops,
},
};

#ifdef CONFIG_PM
static int twl6040_suspend(struct snd_soc_codec *codec, pm_message_t state)
{
	twl6040_set_bias_level(codec, SND_SOC_BIAS_OFF);

	return 0;
}

static int twl6040_resume(struct snd_soc_codec *codec)
{
	if (codec->dapm.bias_level != codec->dapm.suspend_bias_level) {
		twl6040_set_bias_level(codec, SND_SOC_BIAS_STANDBY);
		twl6040_set_bias_level(codec, codec->dapm.suspend_bias_level);
	}

	return 0;
}
#else
#define twl6040_suspend NULL
#define twl6040_resume NULL
#endif

static int twl6040_probe(struct snd_soc_codec *codec)
{
	struct twl6040_data *priv;
	struct twl4030_codec_audio_data *pdata = dev_get_platdata(codec->dev);
	int ret = 0;

	priv = kzalloc(sizeof(struct twl6040_data), GFP_KERNEL);
	if (priv == NULL)
		return -ENOMEM;
	snd_soc_codec_set_drvdata(codec, priv);

	priv->codec = codec;
	codec->control_data = dev_get_drvdata(codec->dev->parent);
	codec->dapm.idle_bias_off = 1;

	if (pdata && pdata->hs_left_step && pdata->hs_right_step) {
		priv->hs_left_step = pdata->hs_left_step;
		priv->hs_right_step = pdata->hs_right_step;
	} else {
		priv->hs_left_step = 1;
		priv->hs_right_step = 1;
	}

	if (pdata && pdata->hf_left_step && pdata->hf_right_step) {
		priv->hf_left_step = pdata->hf_left_step;
		priv->hf_right_step = pdata->hf_right_step;
	} else {
		priv->hf_left_step = 1;
		priv->hf_right_step = 1;
	}

	if (pdata && pdata->ep_step)
		priv->ep_step = pdata->ep_step;
	else
		priv->ep_step = 1;

	/* default is low-power mode */
	priv->hs_power_mode = 1;
	priv->sysclk_constraints = &lp_constraints;
	priv->workqueue = alloc_workqueue("twl6040-codec", 0, 0);

	if (!priv->workqueue) {
		ret = -ENOMEM;
		goto work_err;
	}

	INIT_DELAYED_WORK(&priv->hs_jack.work, twl6040_accessory_work);
	INIT_DELAYED_WORK(&priv->headset.work, twl6040_pga_hs_work);
	INIT_DELAYED_WORK(&priv->handsfree.work, twl6040_pga_hf_work);
	INIT_DELAYED_WORK(&priv->earphone.work, twl6040_pga_ep_work);

	mutex_init(&priv->mutex);

#ifdef CONFIG_REGULATOR_TPS6130X
	priv->vddhf_reg = regulator_get(codec->dev, "vddhf");
	if (IS_ERR(priv->vddhf_reg)) {
		ret = PTR_ERR(priv->vddhf_reg);
		dev_warn(codec->dev, "couldn't get VDDHF regulator %d\n",
			 ret);
		priv->vddhf_reg = NULL;
	}

	if (priv->vddhf_reg) {
		ret = regulator_set_voltage(priv->vddhf_reg,
					    pdata->vddhf_uV, pdata->vddhf_uV);
		if (ret) {
			dev_warn(codec->dev, "failed to set VDDHF voltage %d\n",
				 ret);
			goto reg_err;
		}
	}
#endif

	init_completion(&priv->headset.ramp_done);
	init_completion(&priv->handsfree.ramp_done);
	init_completion(&priv->earphone.ramp_done);

#ifdef TI_HS_JACK
	wake_lock_init(&priv->wake_lock, WAKE_LOCK_SUSPEND, "twl6040");
	ret = twl6040_request_irq(codec->control_data, TWL6040_IRQ_PLUG,
				twl6040_audio_handler, IRQF_NO_SUSPEND,
				"twl6040_irq_plug", codec);
#else
	ret = twl6040_request_irq(codec->control_data, TWL6040_IRQ_PLUG,
				twl6040_audio_handler, 0,
				"twl6040_irq_plug", codec);
#endif
	if (ret) {
		dev_err(codec->dev, "PLUG IRQ request failed: %d\n", ret);
		goto irq_err;
	}

       ret = twl6040_request_irq(codec->control_data, TWL6040_IRQ_HF,
                               twl6040_audio_handler, 0,
                               "twl6040_irq_hf", codec);
       if (ret) {
               dev_err(codec->dev, "HF IRQ request failed: %d\n", ret);
               goto hfirq_err;
       }

	/* init vio registers */
	twl6040_init_vio_regs(codec);

	/* power on device */
	ret = twl6040_set_bias_level(codec, SND_SOC_BIAS_STANDBY);
	if (ret)
		goto bias_err;

	snd_soc_add_controls(codec, twl6040_snd_controls,
				ARRAY_SIZE(twl6040_snd_controls));
	twl6040_add_widgets(codec);

	return 0;

bias_err:
        twl6040_free_irq(codec->control_data, TWL6040_IRQ_HF, codec);
hfirq_err:
	twl6040_free_irq(codec->control_data, TWL6040_IRQ_PLUG, codec);
irq_err:
#ifdef TI_HS_JACK
	wake_lock_destroy(&priv->wake_lock);
#endif
	destroy_workqueue(priv->workqueue);
#ifdef CONFIG_REGULATOR_TPS6130X
reg_err:
	if (priv->vddhf_reg)
		regulator_put(priv->vddhf_reg);
#endif
work_err:
	kfree(priv);
	return ret;
}

static int twl6040_remove(struct snd_soc_codec *codec)
{
	struct twl6040_data *priv = snd_soc_codec_get_drvdata(codec);

	twl6040_set_bias_level(codec, SND_SOC_BIAS_OFF);
	twl6040_free_irq(codec->control_data, TWL6040_IRQ_PLUG, codec);
	twl6040_free_irq(codec->control_data, TWL6040_IRQ_HF, codec);
#ifdef CONFIG_REGULATOR_TPS6130X
	if (priv->vddhf_reg)
		regulator_put(priv->vddhf_reg);
#endif
#ifdef TI_HS_JACK
	wake_lock_destroy(&priv->wake_lock);
#endif
	destroy_workqueue(priv->workqueue);
	kfree(priv);

	return 0;
}

static struct snd_soc_codec_driver soc_codec_dev_twl6040 = {
	.probe = twl6040_probe,
	.remove = twl6040_remove,
	.suspend = twl6040_suspend,
	.resume = twl6040_resume,
	.read = twl6040_read_reg_cache,
	.write = twl6040_write,
	.set_bias_level = twl6040_set_bias_level,
	.reg_cache_size = ARRAY_SIZE(twl6040_reg),
	.reg_word_size = sizeof(u8),
	.reg_cache_default = twl6040_reg,
};

static int __devinit twl6040_codec_probe(struct platform_device *pdev)
{
#ifdef CONFIG_REGULATOR_TPS6130X
	struct twl4030_codec_audio_data *pdata = pdev->dev.platform_data;

	if (!pdata) {
		dev_err(&pdev->dev, "platform_data is missing\n");
		return -EINVAL;
	}
#endif
	return snd_soc_register_codec(&pdev->dev,
			&soc_codec_dev_twl6040, twl6040_dai, ARRAY_SIZE(twl6040_dai));
}

static int __devexit twl6040_codec_remove(struct platform_device *pdev)
{
	snd_soc_unregister_codec(&pdev->dev);
	return 0;
}

static struct platform_driver twl6040_codec_driver = {
	.driver = {
		.name = "twl6040-codec",
		.owner = THIS_MODULE,
	},
	.probe = twl6040_codec_probe,
	.remove = __devexit_p(twl6040_codec_remove),
};

static int __init twl6040_codec_init(void)
{
	return platform_driver_register(&twl6040_codec_driver);
}
module_init(twl6040_codec_init);

static void __exit twl6040_codec_exit(void)
{
	platform_driver_unregister(&twl6040_codec_driver);
}
module_exit(twl6040_codec_exit);

MODULE_DESCRIPTION("ASoC TWL6040 codec driver");
MODULE_AUTHOR("Misael Lopez Cruz");
MODULE_LICENSE("GPL");
