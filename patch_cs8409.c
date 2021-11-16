// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * HD audio interface patch for Cirrus Logic CS8409 HDA bridge chip
 *
 * Copyright (C) 2021 Cirrus Logic, Inc. and
 *                    Cirrus Logic International Semiconductor Ltd.
 */

#include <linux/init.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <sound/core.h>
#include <linux/mutex.h>
#include <linux/iopoll.h>

#include "patch_cs8409.h"

/******************************************************************************
 *                        CS8409 Specific Functions
 ******************************************************************************/

static int cs8409_parse_auto_config(struct hda_codec *codec)
{
	struct cs8409_spec *spec = codec->spec;
	int err;
	int i;

	err = snd_hda_parse_pin_defcfg(codec, &spec->gen.autocfg, NULL, 0);
	if (err < 0)
		return err;

	err = snd_hda_gen_parse_auto_config(codec, &spec->gen.autocfg);
	if (err < 0)
		return err;

	/* keep the ADCs powered up when it's dynamically switchable */
	if (spec->gen.dyn_adc_switch) {
		unsigned int done = 0;

		for (i = 0; i < spec->gen.input_mux.num_items; i++) {
			int idx = spec->gen.dyn_adc_idx[i];

			if (done & (1 << idx))
				continue;
			snd_hda_gen_fix_pin_power(codec, spec->gen.adc_nids[idx]);
			done |= 1 << idx;
		}
	}

	return 0;
}

static void cs8409_disable_i2c_clock_worker(struct work_struct *work);

static struct cs8409_spec *cs8409_alloc_spec(struct hda_codec *codec)
{
	struct cs8409_spec *spec;

	spec = kzalloc(sizeof(*spec), GFP_KERNEL);
	if (!spec)
		return NULL;
	codec->spec = spec;
	spec->codec = codec;
	codec->power_save_node = 1;
	mutex_init(&spec->i2c_mux);
	INIT_DELAYED_WORK(&spec->i2c_clk_work, cs8409_disable_i2c_clock_worker);
	snd_hda_gen_spec_init(&spec->gen);

	return spec;
}

static inline int cs8409_vendor_coef_get(struct hda_codec *codec, unsigned int idx)
{
	snd_hda_codec_write(codec, CS8409_PIN_VENDOR_WIDGET, 0, AC_VERB_SET_COEF_INDEX, idx);
	return snd_hda_codec_read(codec, CS8409_PIN_VENDOR_WIDGET, 0, AC_VERB_GET_PROC_COEF, 0);
}

static inline void cs8409_vendor_coef_set(struct hda_codec *codec, unsigned int idx,
					  unsigned int coef)
{
	snd_hda_codec_write(codec, CS8409_PIN_VENDOR_WIDGET, 0, AC_VERB_SET_COEF_INDEX, idx);
	snd_hda_codec_write(codec, CS8409_PIN_VENDOR_WIDGET, 0, AC_VERB_SET_PROC_COEF, coef);
}

/*
 * cs8409_enable_i2c_clock - Disable I2C clocks
 * @codec: the codec instance
 * Disable I2C clocks.
 * This must be called when the i2c mutex is unlocked.
 */
static void cs8409_disable_i2c_clock(struct hda_codec *codec)
{
	struct cs8409_spec *spec = codec->spec;

	mutex_lock(&spec->i2c_mux);
	if (spec->i2c_clck_enabled) {
		cs8409_vendor_coef_set(spec->codec, 0x0,
			       cs8409_vendor_coef_get(spec->codec, 0x0) & 0xfffffff7);
		spec->i2c_clck_enabled = 0;
	}
	mutex_unlock(&spec->i2c_mux);
}

/*
 * cs8409_disable_i2c_clock_worker - Worker that disable the I2C Clock after 25ms without use
 */
static void cs8409_disable_i2c_clock_worker(struct work_struct *work)
{
	struct cs8409_spec *spec = container_of(work, struct cs8409_spec, i2c_clk_work.work);

	cs8409_disable_i2c_clock(spec->codec);
}

/*
 * cs8409_enable_i2c_clock - Enable I2C clocks
 * @codec: the codec instance
 * Enable I2C clocks.
 * This must be called when the i2c mutex is locked.
 */
static void cs8409_enable_i2c_clock(struct hda_codec *codec)
{
	struct cs8409_spec *spec = codec->spec;

	/* Cancel the disable timer, but do not wait for any running disable functions to finish.
	 * If the disable timer runs out before cancel, the delayed work thread will be blocked,
	 * waiting for the mutex to become unlocked. This mutex will be locked for the duration of
	 * any i2c transaction, so the disable function will run to completion immediately
	 * afterwards in the scenario. The next enable call will re-enable the clock, regardless.
	 */
	cancel_delayed_work(&spec->i2c_clk_work);

	if (!spec->i2c_clck_enabled) {
		cs8409_vendor_coef_set(codec, 0x0, cs8409_vendor_coef_get(codec, 0x0) | 0x8);
		spec->i2c_clck_enabled = 1;
	}
	queue_delayed_work(system_power_efficient_wq, &spec->i2c_clk_work, msecs_to_jiffies(25));
}

/**
 * cs8409_i2c_wait_complete - Wait for I2C transaction
 * @codec: the codec instance
 *
 * Wait for I2C transaction to complete.
 * Return -ETIMEDOUT if transaction wait times out.
 */
static int cs8409_i2c_wait_complete(struct hda_codec *codec)
{
	unsigned int retval;

	return read_poll_timeout(cs8409_vendor_coef_get, retval, retval & 0x18,
		CS42L42_I2C_SLEEP_US, CS42L42_I2C_TIMEOUT_US, false, codec, CS8409_I2C_STS);
}

/**
 * cs8409_set_i2c_dev_addr - Set i2c address for transaction
 * @codec: the codec instance
 * @addr: I2C Address
 */
static void cs8409_set_i2c_dev_addr(struct hda_codec *codec, unsigned int addr)
{
	struct cs8409_spec *spec = codec->spec;

	if (spec->dev_addr != addr) {
		cs8409_vendor_coef_set(codec, CS8409_I2C_ADDR, addr);
		spec->dev_addr = addr;
	}
}

/**
 * cs8409_i2c_set_page - CS8409 I2C set page register.
 * @scodec: the codec instance
 * @i2c_reg: Page register
 *
 * Returns negative on error.
 */
static int cs8409_i2c_set_page(struct sub_codec *scodec, unsigned int i2c_reg)
{
	struct hda_codec *codec = scodec->codec;

	if (scodec->paged && (scodec->last_page != (i2c_reg >> 8))) {
		cs8409_vendor_coef_set(codec, CS8409_I2C_QWRITE, i2c_reg >> 8);
		if (cs8409_i2c_wait_complete(codec) < 0)
			return -EIO;
		scodec->last_page = i2c_reg >> 8;
	}

	return 0;
}

/**
 * cs8409_i2c_read - CS8409 I2C Read.
 * @scodec: the codec instance
 * @addr: Register to read
 *
 * Returns negative on error, otherwise returns read value in bits 0-7.
 */
static int cs8409_i2c_read(struct sub_codec *scodec, unsigned int addr)
{
	struct hda_codec *codec = scodec->codec;
	struct cs8409_spec *spec = codec->spec;
	unsigned int i2c_reg_data;
	unsigned int read_data;

	if (scodec->suspended)
		return -EPERM;

	mutex_lock(&spec->i2c_mux);
	cs8409_enable_i2c_clock(codec);
	cs8409_set_i2c_dev_addr(codec, scodec->addr);

	if (cs8409_i2c_set_page(scodec, addr))
		goto error;

	i2c_reg_data = (addr << 8) & 0x0ffff;
	cs8409_vendor_coef_set(codec, CS8409_I2C_QREAD, i2c_reg_data);
	if (cs8409_i2c_wait_complete(codec) < 0)
		goto error;

	/* Register in bits 15-8 and the data in 7-0 */
	read_data = cs8409_vendor_coef_get(codec, CS8409_I2C_QREAD);

	mutex_unlock(&spec->i2c_mux);

	return read_data & 0x0ff;

error:
	mutex_unlock(&spec->i2c_mux);
	codec_err(codec, "%s() Failed 0x%02x : 0x%04x\n", __func__, scodec->addr, addr);
	return -EIO;
}

/**
 * cs8409_i2c_bulk_read - CS8409 I2C Read Sequence.
 * @scodec: the codec instance
 * @seq: Register Sequence to read
 * @count: Number of registeres to read
 *
 * Returns negative on error, values are read into value element of cs8409_i2c_param sequence.
 */
static int cs8409_i2c_bulk_read(struct sub_codec *scodec, struct cs8409_i2c_param *seq, int count)
{
	struct hda_codec *codec = scodec->codec;
	struct cs8409_spec *spec = codec->spec;
	unsigned int i2c_reg_data;
	int i;

	if (scodec->suspended)
		return -EPERM;

	mutex_lock(&spec->i2c_mux);
	cs8409_set_i2c_dev_addr(codec, scodec->addr);

	for (i = 0; i < count; i++) {
		cs8409_enable_i2c_clock(codec);
		if (cs8409_i2c_set_page(scodec, seq[i].addr))
			goto error;

		i2c_reg_data = (seq[i].addr << 8) & 0x0ffff;
		cs8409_vendor_coef_set(codec, CS8409_I2C_QREAD, i2c_reg_data);

		if (cs8409_i2c_wait_complete(codec) < 0)
			goto error;

		seq[i].value = cs8409_vendor_coef_get(codec, CS8409_I2C_QREAD) & 0xff;
	}

	mutex_unlock(&spec->i2c_mux);

	return 0;

error:
	mutex_unlock(&spec->i2c_mux);
	codec_err(codec, "I2C Bulk Write Failed 0x%02x\n", scodec->addr);
	return -EIO;
}

/**
 * cs8409_i2c_write - CS8409 I2C Write.
 * @scodec: the codec instance
 * @addr: Register to write to
 * @value: Data to write
 *
 * Returns negative on error, otherwise returns 0.
 */
static int cs8409_i2c_write(struct sub_codec *scodec, unsigned int addr, unsigned int value)
{
	struct hda_codec *codec = scodec->codec;
	struct cs8409_spec *spec = codec->spec;
	unsigned int i2c_reg_data;

	if (scodec->suspended)
		return -EPERM;

	mutex_lock(&spec->i2c_mux);

	cs8409_enable_i2c_clock(codec);
	cs8409_set_i2c_dev_addr(codec, scodec->addr);

	if (cs8409_i2c_set_page(scodec, addr))
		goto error;

	i2c_reg_data = ((addr << 8) & 0x0ff00) | (value & 0x0ff);
	cs8409_vendor_coef_set(codec, CS8409_I2C_QWRITE, i2c_reg_data);

	if (cs8409_i2c_wait_complete(codec) < 0)
		goto error;

	mutex_unlock(&spec->i2c_mux);
	return 0;

error:
	mutex_unlock(&spec->i2c_mux);
	codec_err(codec, "%s() Failed 0x%02x : 0x%04x\n", __func__, scodec->addr, addr);
	return -EIO;
}

/**
 * cs8409_i2c_bulk_write - CS8409 I2C Write Sequence.
 * @scodec: the codec instance
 * @seq: Register Sequence to write
 * @count: Number of registeres to write
 *
 * Returns negative on error.
 */
static int cs8409_i2c_bulk_write(struct sub_codec *scodec, const struct cs8409_i2c_param *seq,
				 int count)
{
	struct hda_codec *codec = scodec->codec;
	struct cs8409_spec *spec = codec->spec;
	unsigned int i2c_reg_data;
	int i;

	if (scodec->suspended)
		return -EPERM;

	mutex_lock(&spec->i2c_mux);
	cs8409_set_i2c_dev_addr(codec, scodec->addr);

	for (i = 0; i < count; i++) {
		cs8409_enable_i2c_clock(codec);
		if (cs8409_i2c_set_page(scodec, seq[i].addr))
			goto error;

		i2c_reg_data = ((seq[i].addr << 8) & 0x0ff00) | (seq[i].value & 0x0ff);
		cs8409_vendor_coef_set(codec, CS8409_I2C_QWRITE, i2c_reg_data);

		if (cs8409_i2c_wait_complete(codec) < 0)
			goto error;
	}

	mutex_unlock(&spec->i2c_mux);

	return 0;

error:
	mutex_unlock(&spec->i2c_mux);
	codec_err(codec, "I2C Bulk Write Failed 0x%02x\n", scodec->addr);
	return -EIO;
}

static int cs8409_init(struct hda_codec *codec)
{
	int ret = snd_hda_gen_init(codec);

	if (!ret)
		snd_hda_apply_fixup(codec, HDA_FIXUP_ACT_INIT);

	return ret;
}

static int cs8409_build_controls(struct hda_codec *codec)
{
	int err;

	err = snd_hda_gen_build_controls(codec);
	if (err < 0)
		return err;
	snd_hda_apply_fixup(codec, HDA_FIXUP_ACT_BUILD);

	return 0;
}

/* Enable/Disable Unsolicited Response */
static void cs8409_enable_ur(struct hda_codec *codec, int flag)
{
	struct cs8409_spec *spec = codec->spec;
	unsigned int ur_gpios = 0;
	int i;

	for (i = 0; i < spec->num_scodecs; i++)
		ur_gpios |= spec->scodecs[i]->irq_mask;

	snd_hda_codec_write(codec, CS8409_PIN_AFG, 0, AC_VERB_SET_GPIO_UNSOLICITED_RSP_MASK,
			    flag ? ur_gpios : 0);

	snd_hda_codec_write(codec, CS8409_PIN_AFG, 0, AC_VERB_SET_UNSOLICITED_ENABLE,
			    flag ? AC_UNSOL_ENABLED : 0);
}

static void cs8409_fix_caps(struct hda_codec *codec, unsigned int nid)
{
	int caps;

	/* CS8409 is simple HDA bridge and intended to be used with a remote
	 * companion codec. Most of input/output PIN(s) have only basic
	 * capabilities. Receive and Transmit NID(s) have only OUTC and INC
	 * capabilities and no presence detect capable (PDC) and call to
	 * snd_hda_gen_build_controls() will mark them as non detectable
	 * phantom jacks. However, a companion codec may be
	 * connected to these pins which supports jack detect
	 * capabilities. We have to override pin capabilities,
	 * otherwise they will not be created as input devices.
	 */
	caps = snd_hdac_read_parm(&codec->core, nid, AC_PAR_PIN_CAP);
	if (caps >= 0)
		snd_hdac_override_parm(&codec->core, nid, AC_PAR_PIN_CAP,
				       (caps | (AC_PINCAP_IMP_SENSE | AC_PINCAP_PRES_DETECT)));

	snd_hda_override_wcaps(codec, nid, (get_wcaps(codec, nid) | AC_WCAP_UNSOL_CAP));
}

/******************************************************************************
 *                        CS42L42 Specific Functions
 ******************************************************************************/

int cs42l42_volume_info(struct snd_kcontrol *kctrl, struct snd_ctl_elem_info *uinfo)
{
	unsigned int ofs = get_amp_offset(kctrl);
	u8 chs = get_amp_channels(kctrl);

	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->value.integer.step = 1;
	uinfo->count = chs == 3 ? 2 : 1;

	switch (ofs) {
	case CS42L42_VOL_DAC:
		uinfo->value.integer.min = CS42L42_HP_VOL_REAL_MIN;
		uinfo->value.integer.max = CS42L42_HP_VOL_REAL_MAX;
		break;
	case CS42L42_VOL_ADC:
		uinfo->value.integer.min = CS42L42_AMIC_VOL_REAL_MIN;
		uinfo->value.integer.max = CS42L42_AMIC_VOL_REAL_MAX;
		break;
	default:
		break;
	}

	return 0;
}

int cs42l42_volume_get(struct snd_kcontrol *kctrl, struct snd_ctl_elem_value *uctrl)
{
	struct hda_codec *codec = snd_kcontrol_chip(kctrl);
	struct cs8409_spec *spec = codec->spec;
	struct sub_codec *cs42l42 = spec->scodecs[get_amp_index(kctrl)];
	int chs = get_amp_channels(kctrl);
	unsigned int ofs = get_amp_offset(kctrl);
	long *valp = uctrl->value.integer.value;

	switch (ofs) {
	case CS42L42_VOL_DAC:
		if (chs & BIT(0))
			*valp++ = cs42l42->vol[ofs];
		if (chs & BIT(1))
			*valp = cs42l42->vol[ofs+1];
		break;
	case CS42L42_VOL_ADC:
		if (chs & BIT(0))
			*valp = cs42l42->vol[ofs];
		break;
	default:
		break;
	}

	return 0;
}

static void cs42l42_mute(struct sub_codec *cs42l42, int vol_type,
	unsigned int chs, bool mute)
{
	if (mute) {
		if (vol_type == CS42L42_VOL_DAC) {
			if (chs & BIT(0))
				cs8409_i2c_write(cs42l42, CS42L42_REG_HS_VOL_CHA, 0x3f);
			if (chs & BIT(1))
				cs8409_i2c_write(cs42l42, CS42L42_REG_HS_VOL_CHB, 0x3f);
		} else if (vol_type == CS42L42_VOL_ADC) {
			if (chs & BIT(0))
				cs8409_i2c_write(cs42l42, CS42L42_REG_AMIC_VOL, 0x9f);
		}
	} else {
		if (vol_type == CS42L42_VOL_DAC) {
			if (chs & BIT(0))
				cs8409_i2c_write(cs42l42, CS42L42_REG_HS_VOL_CHA,
					-(cs42l42->vol[CS42L42_DAC_CH0_VOL_OFFSET])
					& CS42L42_REG_HS_VOL_MASK);
			if (chs & BIT(1))
				cs8409_i2c_write(cs42l42, CS42L42_REG_HS_VOL_CHB,
					-(cs42l42->vol[CS42L42_DAC_CH1_VOL_OFFSET])
					& CS42L42_REG_HS_VOL_MASK);
		} else if (vol_type == CS42L42_VOL_ADC) {
			if (chs & BIT(0))
				cs8409_i2c_write(cs42l42, CS42L42_REG_AMIC_VOL,
					cs42l42->vol[CS42L42_ADC_VOL_OFFSET]
					& CS42L42_REG_AMIC_VOL_MASK);
		}
	}
}

int cs42l42_volume_put(struct snd_kcontrol *kctrl, struct snd_ctl_elem_value *uctrl)
{
	struct hda_codec *codec = snd_kcontrol_chip(kctrl);
	struct cs8409_spec *spec = codec->spec;
	struct sub_codec *cs42l42 = spec->scodecs[get_amp_index(kctrl)];
	int chs = get_amp_channels(kctrl);
	unsigned int ofs = get_amp_offset(kctrl);
	long *valp = uctrl->value.integer.value;

	switch (ofs) {
	case CS42L42_VOL_DAC:
		if (chs & BIT(0))
			cs42l42->vol[ofs] = *valp;
		if (chs & BIT(1)) {
			valp++;
			cs42l42->vol[ofs + 1] = *valp;
		}
		if (spec->playback_started)
			cs42l42_mute(cs42l42, CS42L42_VOL_DAC, chs, false);
		break;
	case CS42L42_VOL_ADC:
		if (chs & BIT(0))
			cs42l42->vol[ofs] = *valp;
		if (spec->capture_started)
			cs42l42_mute(cs42l42, CS42L42_VOL_ADC, chs, false);
		break;
	default:
		break;
	}

	return 0;
}

static void cs42l42_playback_pcm_hook(struct hda_pcm_stream *hinfo,
				   struct hda_codec *codec,
				   struct snd_pcm_substream *substream,
				   int action)
{
	struct cs8409_spec *spec = codec->spec;
	struct sub_codec *cs42l42;
	int i;
	bool mute;

	switch (action) {
	case HDA_GEN_PCM_ACT_PREPARE:
		mute = false;
		spec->playback_started = 1;
		break;
	case HDA_GEN_PCM_ACT_CLEANUP:
		mute = true;
		spec->playback_started = 0;
		break;
	default:
		return;
	}

	for (i = 0; i < spec->num_scodecs; i++) {
		cs42l42 = spec->scodecs[i];
		cs42l42_mute(cs42l42, CS42L42_VOL_DAC, 0x3, mute);
	}
}

static void cs42l42_capture_pcm_hook(struct hda_pcm_stream *hinfo,
				   struct hda_codec *codec,
				   struct snd_pcm_substream *substream,
				   int action)
{
	struct cs8409_spec *spec = codec->spec;
	struct sub_codec *cs42l42;
	int i;
	bool mute;

	switch (action) {
	case HDA_GEN_PCM_ACT_PREPARE:
		mute = false;
		spec->capture_started = 1;
		break;
	case HDA_GEN_PCM_ACT_CLEANUP:
		mute = true;
		spec->capture_started = 0;
		break;
	default:
		return;
	}

	for (i = 0; i < spec->num_scodecs; i++) {
		cs42l42 = spec->scodecs[i];
		cs42l42_mute(cs42l42, CS42L42_VOL_ADC, 0x3, mute);
	}
}

/* Configure CS42L42 slave codec for jack autodetect */
static void cs42l42_enable_jack_detect(struct sub_codec *cs42l42)
{
	cs8409_i2c_write(cs42l42, 0x1b70, cs42l42->hsbias_hiz);
	/* Clear WAKE# */
	cs8409_i2c_write(cs42l42, 0x1b71, 0x00C1);
	/* Wait ~2.5ms */
	usleep_range(2500, 3000);
	/* Set mode WAKE# output follows the combination logic directly */
	cs8409_i2c_write(cs42l42, 0x1b71, 0x00C0);
	/* Clear interrupts status */
	cs8409_i2c_read(cs42l42, 0x130f);
	/* Enable interrupt */
	cs8409_i2c_write(cs42l42, 0x1320, 0xF3);
}

/* Enable and run CS42L42 slave codec jack auto detect */
static void cs42l42_run_jack_detect(struct sub_codec *cs42l42)
{
	/* Clear interrupts */
	cs8409_i2c_read(cs42l42, 0x1308);
	cs8409_i2c_read(cs42l42, 0x1b77);
	cs8409_i2c_write(cs42l42, 0x1320, 0xFF);
	cs8409_i2c_read(cs42l42, 0x130f);

	cs8409_i2c_write(cs42l42, 0x1102, 0x87);
	cs8409_i2c_write(cs42l42, 0x1f06, 0x86);
	cs8409_i2c_write(cs42l42, 0x1b74, 0x07);
	cs8409_i2c_write(cs42l42, 0x131b, 0xFD);
	cs8409_i2c_write(cs42l42, 0x1120, 0x80);
	/* Wait ~100us*/
	usleep_range(100, 200);
	cs8409_i2c_write(cs42l42, 0x111f, 0x77);
	cs8409_i2c_write(cs42l42, 0x1120, 0xc0);
}

static int cs42l42_handle_tip_sense(struct sub_codec *cs42l42, unsigned int reg_ts_status)
{
	int status_changed = 0;

	/* TIP_SENSE INSERT/REMOVE */
	switch (reg_ts_status) {
	case CS42L42_JACK_INSERTED:
		if (!cs42l42->hp_jack_in) {
			if (cs42l42->no_type_dect) {
				status_changed = 1;
				cs42l42->hp_jack_in = 1;
				cs42l42->mic_jack_in = 0;
			} else {
				cs42l42_run_jack_detect(cs42l42);
			}
		}
		break;

	case CS42L42_JACK_REMOVED:
		if (cs42l42->hp_jack_in || cs42l42->mic_jack_in) {
			status_changed = 1;
			cs42l42->hp_jack_in = 0;
			cs42l42->mic_jack_in = 0;
		}
		break;
	default:
		/* jack in transition */
		break;
	}

	return status_changed;
}

static int cs42l42_jack_unsol_event(struct sub_codec *cs42l42)
{
	int status_changed = 0;
	int reg_cdc_status;
	int reg_hs_status;
	int reg_ts_status;
	int type;

	/* Read jack detect status registers */
	reg_cdc_status = cs8409_i2c_read(cs42l42, 0x1308);
	reg_hs_status = cs8409_i2c_read(cs42l42, 0x1124);
	reg_ts_status = cs8409_i2c_read(cs42l42, 0x130f);

	/* If status values are < 0, read error has occurred. */
	if (reg_cdc_status < 0 || reg_hs_status < 0 || reg_ts_status < 0)
		return -EIO;

	/* HSDET_AUTO_DONE */
	if (reg_cdc_status & CS42L42_HSDET_AUTO_DONE) {

		/* Disable HSDET_AUTO_DONE */
		cs8409_i2c_write(cs42l42, 0x131b, 0xFF);

		type = ((reg_hs_status & CS42L42_HSTYPE_MASK) + 1);

		if (cs42l42->no_type_dect) {
			status_changed = cs42l42_handle_tip_sense(cs42l42, reg_ts_status);
		} else if (type == 4) {
			/* Type 4 not supported	*/
			status_changed = cs42l42_handle_tip_sense(cs42l42, CS42L42_JACK_REMOVED);
		} else {
			if (!cs42l42->hp_jack_in) {
				status_changed = 1;
				cs42l42->hp_jack_in = 1;
			}
			/* type = 3 has no mic */
			if ((!cs42l42->mic_jack_in) && (type != 3)) {
				status_changed = 1;
				cs42l42->mic_jack_in = 1;
			}
		}
		/* Configure the HSDET mode. */
		cs8409_i2c_write(cs42l42, 0x1120, 0x80);
		/* Enable the HPOUT ground clamp and configure the HP pull-down */
		cs8409_i2c_write(cs42l42, 0x1F06, 0x02);
		/* Re-Enable Tip Sense Interrupt */
		cs8409_i2c_write(cs42l42, 0x1320, 0xF3);
	} else {
		status_changed = cs42l42_handle_tip_sense(cs42l42, reg_ts_status);
	}

	return status_changed;
}

static void cs42l42_resume(struct sub_codec *cs42l42)
{
	struct hda_codec *codec = cs42l42->codec;
	unsigned int gpio_data;
	struct cs8409_i2c_param irq_regs[] = {
		{ 0x1308, 0x00 },
		{ 0x1309, 0x00 },
		{ 0x130A, 0x00 },
		{ 0x130F, 0x00 },
	};

	/* Bring CS42L42 out of Reset */
	gpio_data = snd_hda_codec_read(codec, CS8409_PIN_AFG, 0, AC_VERB_GET_GPIO_DATA, 0);
	gpio_data |= cs42l42->reset_gpio;
	snd_hda_codec_write(codec, CS8409_PIN_AFG, 0, AC_VERB_SET_GPIO_DATA, gpio_data);
	usleep_range(10000, 15000);

	cs42l42->suspended = 0;

	/* Initialize CS42L42 companion codec */
	cs8409_i2c_bulk_write(cs42l42, cs42l42->init_seq, cs42l42->init_seq_num);
	usleep_range(20000, 25000);

	/* Clear interrupts, by reading interrupt status registers */
	cs8409_i2c_bulk_read(cs42l42, irq_regs, ARRAY_SIZE(irq_regs));

	if (cs42l42->full_scale_vol)
		cs8409_i2c_write(cs42l42, 0x2001, 0x01);

	cs42l42_enable_jack_detect(cs42l42);
}

#ifdef CONFIG_PM
static void cs42l42_suspend(struct sub_codec *cs42l42)
{
	struct hda_codec *codec = cs42l42->codec;
	unsigned int gpio_data;
	int reg_cdc_status = 0;
	const struct cs8409_i2c_param cs42l42_pwr_down_seq[] = {
		{ 0x1F06, 0x02 },
		{ 0x1129, 0x00 },
		{ 0x2301, 0x3F },
		{ 0x2302, 0x3F },
		{ 0x2303, 0x3F },
		{ 0x2001, 0x0F },
		{ 0x2A01, 0x00 },
		{ 0x1207, 0x00 },
		{ 0x1101, 0xFE },
		{ 0x1102, 0x8C },
		{ 0x1101, 0xFF },
	};

	cs8409_i2c_bulk_write(cs42l42, cs42l42_pwr_down_seq, ARRAY_SIZE(cs42l42_pwr_down_seq));

	if (read_poll_timeout(cs8409_i2c_read, reg_cdc_status,
			(reg_cdc_status & 0x1), CS42L42_PDN_SLEEP_US, CS42L42_PDN_TIMEOUT_US,
			true, cs42l42, 0x1308) < 0)
		codec_warn(codec, "Timeout waiting for PDN_DONE for CS42L42\n");

	/* Power down CS42L42 ASP/EQ/MIX/HP */
	cs8409_i2c_write(cs42l42, 0x1102, 0x9C);
	cs42l42->suspended = 1;
	cs42l42->last_page = 0;
	cs42l42->hp_jack_in = 0;
	cs42l42->mic_jack_in = 0;

	/* Put CS42L42 into Reset */
	gpio_data = snd_hda_codec_read(codec, CS8409_PIN_AFG, 0, AC_VERB_GET_GPIO_DATA, 0);
	gpio_data &= ~cs42l42->reset_gpio;
	snd_hda_codec_write(codec, CS8409_PIN_AFG, 0, AC_VERB_SET_GPIO_DATA, gpio_data);
}
#endif

static void cs8409_free(struct hda_codec *codec)
{
	struct cs8409_spec *spec = codec->spec;

	/* Cancel i2c clock disable timer, and disable clock if left enabled */
	cancel_delayed_work_sync(&spec->i2c_clk_work);
	cs8409_disable_i2c_clock(codec);

	snd_hda_gen_free(codec);
}

/******************************************************************************
 *                   BULLSEYE / WARLOCK / CYBORG Specific Functions
 *                               CS8409/CS42L42
 ******************************************************************************/

/*
 * In the case of CS8409 we do not have unsolicited events from NID's 0x24
 * and 0x34 where hs mic and hp are connected. Companion codec CS42L42 will
 * generate interrupt via gpio 4 to notify jack events. We have to overwrite
 * generic snd_hda_jack_unsol_event(), read CS42L42 jack detect status registers
 * and then notify status via generic snd_hda_jack_unsol_event() call.
 */
static void cs8409_cs42l42_jack_unsol_event(struct hda_codec *codec, unsigned int res)
{
	struct cs8409_spec *spec = codec->spec;
	struct sub_codec *cs42l42 = spec->scodecs[CS8409_CODEC0];
	struct hda_jack_tbl *jk;

	/* jack_unsol_event() will be called every time gpio line changing state.
	 * In this case gpio4 line goes up as a result of reading interrupt status
	 * registers in previous cs8409_jack_unsol_event() call.
	 * We don't need to handle this event, ignoring...
	 */
	if (res & cs42l42->irq_mask)
		return;

	if (cs42l42_jack_unsol_event(cs42l42)) {
		snd_hda_set_pin_ctl(codec, CS8409_CS42L42_SPK_PIN_NID,
				    cs42l42->hp_jack_in ? 0 : PIN_OUT);
		/* Report jack*/
		jk = snd_hda_jack_tbl_get_mst(codec, CS8409_CS42L42_HP_PIN_NID, 0);
		if (jk)
			snd_hda_jack_unsol_event(codec, (jk->tag << AC_UNSOL_RES_TAG_SHIFT) &
							AC_UNSOL_RES_TAG);
		/* Report jack*/
		jk = snd_hda_jack_tbl_get_mst(codec, CS8409_CS42L42_AMIC_PIN_NID, 0);
		if (jk)
			snd_hda_jack_unsol_event(codec, (jk->tag << AC_UNSOL_RES_TAG_SHIFT) &
							 AC_UNSOL_RES_TAG);
	}
}

#ifdef CONFIG_PM
/* Manage PDREF, when transition to D3hot */
static int cs8409_cs42l42_suspend(struct hda_codec *codec)
{
	struct cs8409_spec *spec = codec->spec;
	int i;

	spec->init_done = 0;

	cs8409_enable_ur(codec, 0);

	for (i = 0; i < spec->num_scodecs; i++)
		cs42l42_suspend(spec->scodecs[i]);

	/* Cancel i2c clock disable timer, and disable clock if left enabled */
	cancel_delayed_work_sync(&spec->i2c_clk_work);
	cs8409_disable_i2c_clock(codec);

	snd_hda_shutup_pins(codec);

	return 0;
}
#endif

/* Vendor specific HW configuration
 * PLL, ASP, I2C, SPI, GPIOs, DMIC etc...
 */
static void cs8409_cs42l42_hw_init(struct hda_codec *codec)
{
	const struct cs8409_cir_param *seq = cs8409_cs42l42_hw_cfg;
	const struct cs8409_cir_param *seq_bullseye = cs8409_cs42l42_bullseye_atn;
	struct cs8409_spec *spec = codec->spec;
	struct sub_codec *cs42l42 = spec->scodecs[CS8409_CODEC0];

	if (spec->gpio_mask) {
		snd_hda_codec_write(codec, CS8409_PIN_AFG, 0, AC_VERB_SET_GPIO_MASK,
			spec->gpio_mask);
		snd_hda_codec_write(codec, CS8409_PIN_AFG, 0, AC_VERB_SET_GPIO_DIRECTION,
			spec->gpio_dir);
		snd_hda_codec_write(codec, CS8409_PIN_AFG, 0, AC_VERB_SET_GPIO_DATA,
			spec->gpio_data);
	}

	for (; seq->nid; seq++)
		cs8409_vendor_coef_set(codec, seq->cir, seq->coeff);

	if (codec->fixup_id == CS8409_BULLSEYE) {
		for (; seq_bullseye->nid; seq_bullseye++)
			cs8409_vendor_coef_set(codec, seq_bullseye->cir, seq_bullseye->coeff);
	}

	/* DMIC1_MO=00b, DMIC1/2_SR=1 */
	if (codec->fixup_id == CS8409_WARLOCK || codec->fixup_id == CS8409_CYBORG)
		cs8409_vendor_coef_set(codec, 0x09, 0x0003);

	cs42l42_resume(cs42l42);

	/* Enable Unsolicited Response */
	cs8409_enable_ur(codec, 1);
}

static const struct hda_codec_ops cs8409_cs42l42_patch_ops = {
	.build_controls = cs8409_build_controls,
	.build_pcms = snd_hda_gen_build_pcms,
	.init = cs8409_init,
	.free = cs8409_free,
	.unsol_event = cs8409_cs42l42_jack_unsol_event,
#ifdef CONFIG_PM
	.suspend = cs8409_cs42l42_suspend,
#endif
};

static int cs8409_cs42l42_exec_verb(struct hdac_device *dev, unsigned int cmd, unsigned int flags,
				    unsigned int *res)
{
	struct hda_codec *codec = container_of(dev, struct hda_codec, core);
	struct cs8409_spec *spec = codec->spec;
	struct sub_codec *cs42l42 = spec->scodecs[CS8409_CODEC0];

	unsigned int nid = ((cmd >> 20) & 0x07f);
	unsigned int verb = ((cmd >> 8) & 0x0fff);

	/* CS8409 pins have no AC_PINSENSE_PRESENCE
	 * capabilities. We have to intercept 2 calls for pins 0x24 and 0x34
	 * and return correct pin sense values for read_pin_sense() call from
	 * hda_jack based on CS42L42 jack detect status.
	 */
	switch (nid) {
	case CS8409_CS42L42_HP_PIN_NID:
		if (verb == AC_VERB_GET_PIN_SENSE) {
			*res = (cs42l42->hp_jack_in) ? AC_PINSENSE_PRESENCE : 0;
			return 0;
		}
		break;
	case CS8409_CS42L42_AMIC_PIN_NID:
		if (verb == AC_VERB_GET_PIN_SENSE) {
			*res = (cs42l42->mic_jack_in) ? AC_PINSENSE_PRESENCE : 0;
			return 0;
		}
		break;
	default:
		break;
	}

	return spec->exec_verb(dev, cmd, flags, res);
}

void cs8409_cs42l42_fixups(struct hda_codec *codec, const struct hda_fixup *fix, int action)
{
	struct cs8409_spec *spec = codec->spec;

	switch (action) {
	case HDA_FIXUP_ACT_PRE_PROBE:
		snd_hda_add_verbs(codec, cs8409_cs42l42_init_verbs);
		/* verb exec op override */
		spec->exec_verb = codec->core.exec_verb;
		codec->core.exec_verb = cs8409_cs42l42_exec_verb;

		spec->scodecs[CS8409_CODEC0] = &cs8409_cs42l42_codec;
		spec->num_scodecs = 1;
		spec->scodecs[CS8409_CODEC0]->codec = codec;
		codec->patch_ops = cs8409_cs42l42_patch_ops;

		spec->gen.suppress_auto_mute = 1;
		spec->gen.no_primary_hp = 1;
		spec->gen.suppress_vmaster = 1;

		/* GPIO 5 out, 3,4 in */
		spec->gpio_dir = spec->scodecs[CS8409_CODEC0]->reset_gpio;
		spec->gpio_data = 0;
		spec->gpio_mask = 0x03f;

		/* Basic initial sequence for specific hw configuration */
		snd_hda_sequence_write(codec, cs8409_cs42l42_init_verbs);

		cs8409_fix_caps(codec, CS8409_CS42L42_HP_PIN_NID);
		cs8409_fix_caps(codec, CS8409_CS42L42_AMIC_PIN_NID);

		/* Set TIP_SENSE_EN for analog front-end of tip sense.
		 * Additionally set HSBIAS_SENSE_EN and Full Scale volume for some variants.
		 */
		switch (codec->fixup_id) {
		case CS8409_WARLOCK:
			spec->scodecs[CS8409_CODEC0]->hsbias_hiz = 0x0020;
			spec->scodecs[CS8409_CODEC0]->full_scale_vol = 1;
			break;
		case CS8409_BULLSEYE:
			spec->scodecs[CS8409_CODEC0]->hsbias_hiz = 0x0020;
			spec->scodecs[CS8409_CODEC0]->full_scale_vol = 0;
			break;
		case CS8409_CYBORG:
			spec->scodecs[CS8409_CODEC0]->hsbias_hiz = 0x00a0;
			spec->scodecs[CS8409_CODEC0]->full_scale_vol = 1;
			break;
		default:
			spec->scodecs[CS8409_CODEC0]->hsbias_hiz = 0x0003;
			spec->scodecs[CS8409_CODEC0]->full_scale_vol = 1;
			break;
		}

		break;
	case HDA_FIXUP_ACT_PROBE:
		/* Fix Sample Rate to 48kHz */
		spec->gen.stream_analog_playback = &cs42l42_48k_pcm_analog_playback;
		spec->gen.stream_analog_capture = &cs42l42_48k_pcm_analog_capture;
		/* add hooks */
		spec->gen.pcm_playback_hook = cs42l42_playback_pcm_hook;
		spec->gen.pcm_capture_hook = cs42l42_capture_pcm_hook;
		/* Set initial DMIC volume to -26 dB */
		snd_hda_codec_amp_init_stereo(codec, CS8409_CS42L42_DMIC_ADC_PIN_NID,
					      HDA_INPUT, 0, 0xff, 0x19);
		snd_hda_gen_add_kctl(&spec->gen, "Headphone Playback Volume",
				&cs42l42_dac_volume_mixer);
		snd_hda_gen_add_kctl(&spec->gen, "Mic Capture Volume",
				&cs42l42_adc_volume_mixer);
		/* Disable Unsolicited Response during boot */
		cs8409_enable_ur(codec, 0);
		snd_hda_codec_set_name(codec, "CS8409/CS42L42");
		break;
	case HDA_FIXUP_ACT_INIT:
		cs8409_cs42l42_hw_init(codec);
		spec->init_done = 1;
		if (spec->init_done && spec->build_ctrl_done
			&& !spec->scodecs[CS8409_CODEC0]->hp_jack_in)
			cs42l42_run_jack_detect(spec->scodecs[CS8409_CODEC0]);
		break;
	case HDA_FIXUP_ACT_BUILD:
		spec->build_ctrl_done = 1;
		/* Run jack auto detect first time on boot
		 * after controls have been added, to check if jack has
		 * been already plugged in.
		 * Run immediately after init.
		 */
		if (spec->init_done && spec->build_ctrl_done
			&& !spec->scodecs[CS8409_CODEC0]->hp_jack_in)
			cs42l42_run_jack_detect(spec->scodecs[CS8409_CODEC0]);
		break;
	default:
		break;
	}
}

/******************************************************************************
 *                          Dolphin Specific Functions
 *                               CS8409/ 2 X CS42L42
 ******************************************************************************/

/*
 * In the case of CS8409 we do not have unsolicited events when
 * hs mic and hp are connected. Companion codec CS42L42 will
 * generate interrupt via irq_mask to notify jack events. We have to overwrite
 * generic snd_hda_jack_unsol_event(), read CS42L42 jack detect status registers
 * and then notify status via generic snd_hda_jack_unsol_event() call.
 */
static void dolphin_jack_unsol_event(struct hda_codec *codec, unsigned int res)
{
	struct cs8409_spec *spec = codec->spec;
	struct sub_codec *cs42l42;
	struct hda_jack_tbl *jk;

	cs42l42 = spec->scodecs[CS8409_CODEC0];
	if (!cs42l42->suspended && (~res & cs42l42->irq_mask) &&
	    cs42l42_jack_unsol_event(cs42l42)) {
		jk = snd_hda_jack_tbl_get_mst(codec, DOLPHIN_HP_PIN_NID, 0);
		if (jk)
			snd_hda_jack_unsol_event(codec,
						 (jk->tag << AC_UNSOL_RES_TAG_SHIFT) &
						  AC_UNSOL_RES_TAG);

		jk = snd_hda_jack_tbl_get_mst(codec, DOLPHIN_AMIC_PIN_NID, 0);
		if (jk)
			snd_hda_jack_unsol_event(codec,
						 (jk->tag << AC_UNSOL_RES_TAG_SHIFT) &
						  AC_UNSOL_RES_TAG);
	}

	cs42l42 = spec->scodecs[CS8409_CODEC1];
	if (!cs42l42->suspended && (~res & cs42l42->irq_mask) &&
	    cs42l42_jack_unsol_event(cs42l42)) {
		jk = snd_hda_jack_tbl_get_mst(codec, DOLPHIN_LO_PIN_NID, 0);
		if (jk)
			snd_hda_jack_unsol_event(codec,
						 (jk->tag << AC_UNSOL_RES_TAG_SHIFT) &
						  AC_UNSOL_RES_TAG);
	}
}

/* Vendor specific HW configuration
 * PLL, ASP, I2C, SPI, GPIOs, DMIC etc...
 */
static void dolphin_hw_init(struct hda_codec *codec)
{
	const struct cs8409_cir_param *seq = dolphin_hw_cfg;
	struct cs8409_spec *spec = codec->spec;
	struct sub_codec *cs42l42;
	int i;

	if (spec->gpio_mask) {
		snd_hda_codec_write(codec, CS8409_PIN_AFG, 0, AC_VERB_SET_GPIO_MASK,
				    spec->gpio_mask);
		snd_hda_codec_write(codec, CS8409_PIN_AFG, 0, AC_VERB_SET_GPIO_DIRECTION,
				    spec->gpio_dir);
		snd_hda_codec_write(codec, CS8409_PIN_AFG, 0, AC_VERB_SET_GPIO_DATA,
				    spec->gpio_data);
	}

	for (; seq->nid; seq++)
		cs8409_vendor_coef_set(codec, seq->cir, seq->coeff);

	for (i = 0; i < spec->num_scodecs; i++) {
		cs42l42 = spec->scodecs[i];
		cs42l42_resume(cs42l42);
	}

	/* Enable Unsolicited Response */
	cs8409_enable_ur(codec, 1);
}

static const struct hda_codec_ops cs8409_dolphin_patch_ops = {
	.build_controls = cs8409_build_controls,
	.build_pcms = snd_hda_gen_build_pcms,
	.init = cs8409_init,
	.free = cs8409_free,
	.unsol_event = dolphin_jack_unsol_event,
#ifdef CONFIG_PM
	.suspend = cs8409_cs42l42_suspend,
#endif
};

static int dolphin_exec_verb(struct hdac_device *dev, unsigned int cmd, unsigned int flags,
			     unsigned int *res)
{
	struct hda_codec *codec = container_of(dev, struct hda_codec, core);
	struct cs8409_spec *spec = codec->spec;
	struct sub_codec *cs42l42 = spec->scodecs[CS8409_CODEC0];

	unsigned int nid = ((cmd >> 20) & 0x07f);
	unsigned int verb = ((cmd >> 8) & 0x0fff);

	/* CS8409 pins have no AC_PINSENSE_PRESENCE
	 * capabilities. We have to intercept calls for CS42L42 pins
	 * and return correct pin sense values for read_pin_sense() call from
	 * hda_jack based on CS42L42 jack detect status.
	 */
	switch (nid) {
	case DOLPHIN_HP_PIN_NID:
	case DOLPHIN_LO_PIN_NID:
		if (nid == DOLPHIN_LO_PIN_NID)
			cs42l42 = spec->scodecs[CS8409_CODEC1];
		if (verb == AC_VERB_GET_PIN_SENSE) {
			*res = (cs42l42->hp_jack_in) ? AC_PINSENSE_PRESENCE : 0;
			return 0;
		}
		break;
	case DOLPHIN_AMIC_PIN_NID:
		if (verb == AC_VERB_GET_PIN_SENSE) {
			*res = (cs42l42->mic_jack_in) ? AC_PINSENSE_PRESENCE : 0;
			return 0;
		}
		break;
	default:
		break;
	}

	return spec->exec_verb(dev, cmd, flags, res);
}

void dolphin_fixups(struct hda_codec *codec, const struct hda_fixup *fix, int action)
{
	struct cs8409_spec *spec = codec->spec;
	struct snd_kcontrol_new *kctrl;
	int i;

	switch (action) {
	case HDA_FIXUP_ACT_PRE_PROBE:
		snd_hda_add_verbs(codec, dolphin_init_verbs);
		/* verb exec op override */
		spec->exec_verb = codec->core.exec_verb;
		codec->core.exec_verb = dolphin_exec_verb;

		spec->scodecs[CS8409_CODEC0] = &dolphin_cs42l42_0;
		spec->scodecs[CS8409_CODEC0]->codec = codec;
		spec->scodecs[CS8409_CODEC1] = &dolphin_cs42l42_1;
		spec->scodecs[CS8409_CODEC1]->codec = codec;
		spec->num_scodecs = 2;

		codec->patch_ops = cs8409_dolphin_patch_ops;

		/* GPIO 1,5 out, 0,4 in */
		spec->gpio_dir = spec->scodecs[CS8409_CODEC0]->reset_gpio |
				 spec->scodecs[CS8409_CODEC1]->reset_gpio;
		spec->gpio_data = 0;
		spec->gpio_mask = 0x03f;

		/* Basic initial sequence for specific hw configuration */
		snd_hda_sequence_write(codec, dolphin_init_verbs);

		snd_hda_jack_add_kctl(codec, DOLPHIN_LO_PIN_NID, "Line Out", true,
				      SND_JACK_HEADPHONE, NULL);

		snd_hda_jack_add_kctl(codec, DOLPHIN_AMIC_PIN_NID, "Microphone", true,
				      SND_JACK_MICROPHONE, NULL);

		cs8409_fix_caps(codec, DOLPHIN_HP_PIN_NID);
		cs8409_fix_caps(codec, DOLPHIN_LO_PIN_NID);
		cs8409_fix_caps(codec, DOLPHIN_AMIC_PIN_NID);

		break;
	case HDA_FIXUP_ACT_PROBE:
		/* Fix Sample Rate to 48kHz */
		spec->gen.stream_analog_playback = &cs42l42_48k_pcm_analog_playback;
		spec->gen.stream_analog_capture = &cs42l42_48k_pcm_analog_capture;
		/* add hooks */
		spec->gen.pcm_playback_hook = cs42l42_playback_pcm_hook;
		spec->gen.pcm_capture_hook = cs42l42_capture_pcm_hook;
		snd_hda_gen_add_kctl(&spec->gen, "Headphone Playback Volume",
				     &cs42l42_dac_volume_mixer);
		snd_hda_gen_add_kctl(&spec->gen, "Mic Capture Volume", &cs42l42_adc_volume_mixer);
		kctrl = snd_hda_gen_add_kctl(&spec->gen, "Line Out Playback Volume",
					     &cs42l42_dac_volume_mixer);
		/* Update Line Out kcontrol template */
		kctrl->private_value = HDA_COMPOSE_AMP_VAL_OFS(DOLPHIN_HP_PIN_NID, 3, CS8409_CODEC1,
				       HDA_OUTPUT, CS42L42_VOL_DAC) | HDA_AMP_VAL_MIN_MUTE;
		cs8409_enable_ur(codec, 0);
		snd_hda_codec_set_name(codec, "CS8409/CS42L42");
		break;
	case HDA_FIXUP_ACT_INIT:
		dolphin_hw_init(codec);
		spec->init_done = 1;
		if (spec->init_done && spec->build_ctrl_done) {
			for (i = 0; i < spec->num_scodecs; i++) {
				if (!spec->scodecs[i]->hp_jack_in)
					cs42l42_run_jack_detect(spec->scodecs[i]);
			}
		}
		break;
	case HDA_FIXUP_ACT_BUILD:
		spec->build_ctrl_done = 1;
		/* Run jack auto detect first time on boot
		 * after controls have been added, to check if jack has
		 * been already plugged in.
		 * Run immediately after init.
		 */
		if (spec->init_done && spec->build_ctrl_done) {
			for (i = 0; i < spec->num_scodecs; i++) {
				if (!spec->scodecs[i]->hp_jack_in)
					cs42l42_run_jack_detect(spec->scodecs[i]);
			}
		}
		break;
	default:
		break;
	}
}

#if 0
static int patch_cs8409(struct hda_codec *codec)
{
	int err;

	if (!cs8409_alloc_spec(codec))
		return -ENOMEM;

	snd_hda_pick_fixup(codec, cs8409_models, cs8409_fixup_tbl, cs8409_fixups);

	codec_dbg(codec, "Picked ID=%d, VID=%08x, DEV=%08x\n", codec->fixup_id,
			 codec->bus->pci->subsystem_vendor,
			 codec->bus->pci->subsystem_device);

	snd_hda_apply_fixup(codec, HDA_FIXUP_ACT_PRE_PROBE);

	err = cs8409_parse_auto_config(codec);
	if (err < 0) {
		cs8409_free(codec);
		return err;
	}

	snd_hda_apply_fixup(codec, HDA_FIXUP_ACT_PROBE);
	return 0;
}
#endif

static void cs_8409_pcm_playback_pre_prepare_hook(struct hda_pcm_stream *hinfo, struct hda_codec *codec,
                               unsigned int stream_tag, unsigned int format, struct snd_pcm_substream *substream,
                               int action);

// this is a copy from playback_pcm_prepare in hda_generic.c
// initially I needed to do the Apple setup BEFORE the snd_hda_multi_out_analog_prepare
// in order to overwrite the Apple setup with the actual format/stream id
// NOTA BENE - if playback_pcm_prepare is changed in hda_generic.c then
// those changes must be re-implemented here
// we need this order because snd_hda_multi_out_analog_prepare writes the
// the format and stream id's to the audio nodes
//// so far we have left the Apple setup of the nodes format and stream id's in
// now updated to set the actual format where Apple does the format/stream id setup
// Apples format is very specifically S24_3LE (24 bit), 4 channel, 44.1 kHz
// S24_3LE seems to be very difficult to create so best Ive done is
// S24_LE (24 in 32 bits) or S32_LE
// it seems the digital setup is able to handle this with the Apple TDM
// setup but if we use the normal prepare hook order this overrwites
// the node linux 0x2, 0x3 setup with the Apple setup which leads to noise
// (the HDA specs say the node format setup must match the data)
// if we do the Apple setup and then the snd_hda_multi_out_analog_prepare
// the nodes will have the slightly different but working format
// with proper update of stream format at same point as in Apple log we need to pass
// the actual playback format as passed to this routine to our new "hook"
// cs_8409_pcm_playback_pre_prepare_hook
// to define the cached format correctly in that routine
// so far my analysis is that hinfo stores the stream format in the kernel format style
// but what is passed to cs_8409_playback_pcm_prepare is the format in HDA style
// not yet figured how to convert from kernel format style to HDA style

static int cs_8409_playback_pcm_prepare(struct hda_pcm_stream *hinfo,
                                struct hda_codec *codec,
                                unsigned int stream_tag,
                                unsigned int format,
                                struct snd_pcm_substream *substream)
{
        struct hda_gen_spec *spec = codec->spec;
        int err;
        codec_dbg(codec, "cs_8409_playback_pcm_prepare\n");

        codec_dbg(codec, "cs_8409_playback_pcm_prepare: NID=0x%x, stream=0x%x, format=0x%x\n",
                  hinfo->nid, stream_tag, format);

        cs_8409_pcm_playback_pre_prepare_hook(hinfo, codec, stream_tag, format, substream,
                               HDA_GEN_PCM_ACT_PREPARE);

        err = snd_hda_multi_out_analog_prepare(codec, &spec->multiout,
                                               stream_tag, format, substream);

	// we cant call directly as call_pcm_playback_hook is local to hda_generic.c
        //if (!err)
        //        call_pcm_playback_hook(hinfo, codec, substream,
        //                               HDA_GEN_PCM_ACT_PREPARE);
	// but its a trivial function - at least for the moment!!
	if (err)
                codec_dbg(codec, "cs_8409_playback_pcm_prepare err %d\n", err);
        if (!err)
                if (spec->pcm_playback_hook)
                        spec->pcm_playback_hook(hinfo, codec, substream, HDA_GEN_PCM_ACT_PREPARE);
        return err;
}


static void cs_8409_pcm_capture_pre_prepare_hook(struct hda_pcm_stream *hinfo, struct hda_codec *codec,
                               unsigned int stream_tag, unsigned int format, struct snd_pcm_substream *substream,
                               int action);


// this is a copy from capture_pcm_prepare in hda_generic.c
// NOTA BENE - if capture_pcm_prepare is changed in hda_generic.c then
// those changes must be re-implemented here
static int cs_8409_capture_pcm_prepare(struct hda_pcm_stream *hinfo,
                               struct hda_codec *codec,
                               unsigned int stream_tag,
                               unsigned int format,
                               struct snd_pcm_substream *substream)
{
        struct hda_gen_spec *spec = codec->spec;

        codec_dbg(codec, "cs_8409_capture_pcm_prepare\n");

        codec_dbg(codec, "cs_8409_capture_pcm_prepare: NID=0x%x, stream=0x%x, format=0x%x\n",
                  hinfo->nid, stream_tag, format);


        cs_8409_pcm_capture_pre_prepare_hook(hinfo, codec, stream_tag, format, substream,
                              HDA_GEN_PCM_ACT_PREPARE);

	// we have a problem - this has to handle 2 different types of stream - the internal mike
	// and the external headset mike (cs42l83)


	// NOTE - the following snd_hda_codec_stream no longer do anything
	//        we have already set the stream data in the pre prepare hook
	//        - so as the format here is same (or at least should be!!) as that setup there is no format difference to that
	//        cached and snd_hda_coded_setup_stream does nothing

	if (hinfo->nid == 0x22)
	{

	// so this is getting stranger and stranger
	// the most valid recording is S24_3LE (0x4031) - except that the data we get out is S32_LE (low byte 0)
	// - so it doesnt play right - and it messes with arecords vumeter
	// (S32_LE is officially 0x4041 - but using that format doesnt seem to have valid data - audio very low)
	//// so now try forcing the format here to 0x4031
	//// well that fails miserably - the format mismatch stops data totally
	// it now appears we get the same data with either 0x4031 or 0x4041 - both are low volume
	// - however scaling (normalizing) in audacity we get the right sound with similar quality to OSX
	// so now think the low volume is right - and OSX must be scaling/processing the data in CoreAudio
	// (is the internal mike a fake 24 bits - ie its actually 16 bits but stuffed in the low end of the
	//  24 bits - hence low volume - preliminary scaling attempts in audacity suggest this might be true!!)

        snd_hda_codec_setup_stream(codec, hinfo->nid, stream_tag, 0, format);

	}
	else if (hinfo->nid == 0x1a)
	{

	// do we need a pre-prepare function??
	// maybe for this the external mike ie cs42l83 input

        snd_hda_codec_setup_stream(codec, hinfo->nid, stream_tag, 0, format);

	}
	else
		dev_info(hda_codec_dev(codec), "cs_8409_capture_pcm_prepare - UNIMPLEMENTED input nid 0x%x\n",hinfo->nid);

	// we cant call directly as call_pcm_capture_hook is local to hda_generic.c
        //call_pcm_capture_hook(hinfo, codec, substream,
        //                      HDA_GEN_PCM_ACT_PREPARE);
	// but its a trivial function - at least for the moment!!
	// note this hook if defined also needs to switch between the 2 versions of input!!
        if (spec->pcm_capture_hook)
                spec->pcm_capture_hook(hinfo, codec, substream, HDA_GEN_PCM_ACT_PREPARE);

        return 0;
}



// another copied routine as this is local to hda_jack.c
static struct hda_jack_tbl *
cs_8409_hda_jack_tbl_new(struct hda_codec *codec, hda_nid_t nid)
{
        struct hda_jack_tbl *jack = snd_hda_jack_tbl_get(codec, nid);
        if (jack)
                return jack;
        jack = snd_array_new(&codec->jacktbl);
        if (!jack)
                return NULL;
        jack->nid = nid;
        jack->jack_dirty = 1;
        jack->tag = codec->jacktbl.used;
	// use this to prevent f09 verbs being sent - not seen in OSX logs
        jack->phantom_jack = 1;
        return jack;
}

// copy of snd_hda_jack_detect_enable_callback code
// there is no AC_VERB_SET_UNSOLICITED_ENABLE for 8409
// it appears unsolicited response is pre-enabled
// but we need to fix this to setup the callback on such responses
struct hda_jack_callback *
cs_8409_hda_jack_detect_enable_callback(struct hda_codec *codec, hda_nid_t nid, int tag,
				    hda_jack_callback_fn func)
{
	struct hda_jack_tbl *jack;
	struct hda_jack_callback *callback = NULL;
	int err;

	jack = cs_8409_hda_jack_tbl_new(codec, nid);
	if (!jack)
		return ERR_PTR(-ENOMEM);
	if (func) {
		callback = kzalloc(sizeof(*callback), GFP_KERNEL);
		if (!callback)
			return ERR_PTR(-ENOMEM);
		callback->func = func;
		callback->nid = jack->nid;
		callback->next = jack->callback;
		jack->callback = callback;
	}

	if (jack->jack_detect)
		return callback; /* already registered */
	jack->jack_detect = 1;
	// update the tag - linux code just counted the number of jacks set up
	// for a tag
	// jack->tag = codec->jacktbl.used;
	jack->tag = tag;
	if (codec->jackpoll_interval > 0)
		return callback; /* No unsol if we're polling instead */
	// apparently we dont need to send this
	//err = snd_hda_codec_write_cache(codec, nid, 0,
	//				 AC_VERB_SET_UNSOLICITED_ENABLE,
	//				 AC_USRSP_EN | jack->tag);
	//if (err < 0)
	//	return ERR_PTR(err);
	return callback;
}

#ifdef ADD_EXTENDED_VERB
static void cs_8409_set_extended_codec_verb(void);
#endif

static int cs_8409_init(struct hda_codec *codec)
{
	struct hda_pcm *info = NULL;
	struct hda_pcm_stream *hinfo = NULL;
	struct cs8409_spec *spec = NULL;
	//struct snd_kcontrol *kctl = NULL;
	int pcmcnt = 0;
	int ret_unsol_enable = 0;

	// so apparently if we do not define a resume function
	// then this init function will be called on resume
	// is that what we want here??
	// NOTE this is called for either playback or capture

        myprintk("snd_hda_intel: cs_8409_init\n");

	//if (spec->vendor_nid == CS420X_VENDOR_NID) {
	//	/* init_verb sequence for C0/C1/C2 errata*/
	//	snd_hda_sequence_write(codec, cs_errata_init_verbs);
	//	snd_hda_sequence_write(codec, cs_coef_init_verbs);
	//} else if (spec->vendor_nid == CS4208_VENDOR_NID) {
	//	snd_hda_sequence_write(codec, cs4208_coef_init_verbs);
	//}


	//// so it looks as tho we have an issue when using headsets
	//// - because the 8409 is totally messed up it does not switch the inputs
	//// when a headset is plugged in
	//// not sure about this here - maybe move to where disable internal mike nodes
	//if (spec->jack_present) {
	//}


	// so the following powers on all active nodes - but if we have just plugged
	// in a headset thats still the internal mike and amps

	snd_hda_gen_init(codec);

	// dump the rates/format of the afg node
	// so analog_playback_stream is still NULL here - maybe only defined when doing actual playback
	// the info stream is now defined
	spec = codec->spec;
        hinfo = spec->gen.stream_analog_playback;
	if (hinfo != NULL)
	{
		codec_dbg(codec, "hinfo stream nid 0x%02x rates 0x%08x formats 0x%016llx\n",hinfo->nid,hinfo->rates,hinfo->formats);
	}
	else
		codec_dbg(codec, "hinfo stream NULL\n");

	// think this is what I need to fixup

        list_for_each_entry(info, &codec->pcm_list_head, list) {
                int stream;

                codec_dbg(codec, "cs_8409_init pcm %d\n",pcmcnt);

                for (stream = 0; stream < 2; stream++) {
                        struct hda_pcm_stream *hinfo = &info->stream[stream];

			codec_dbg(codec, "cs_8409_init info stream %d pointer %p\n",stream,hinfo);

			if (hinfo != NULL)
			{
				codec_dbg(codec, "cs_8409_init info stream %d nid 0x%02x rates 0x%08x formats 0x%016llx\n",stream,hinfo->nid,hinfo->rates,hinfo->formats);
				codec_dbg(codec, "cs_8409_init        stream substreams %d\n",hinfo->substreams);
				codec_dbg(codec, "cs_8409_init        stream channels min %d\n",hinfo->channels_min);
				codec_dbg(codec, "cs_8409_init        stream channels max %d\n",hinfo->channels_max);
				codec_dbg(codec, "cs_8409_init        stream maxbps %d\n",hinfo->maxbps);
			}
			else
				codec_dbg(codec, "cs_8409_init info stream %d NULL\n", stream);
		}
		pcmcnt++;
	}

	// update the streams specifically by nid
	// we seem to have only 1 stream here with the nid of 0x02
	// (I still dont really understand the linux generic coding here)
	// with capture devices we seem to get 2 pcm streams (0 and 1)
	// each pcm stream has an output stream (0) and an input stream (1)
	// the 1st pcm stream (0) is assigned nid 0x02 for output and nid 0x22 for input (internal mike)
	// the 2nd pcm stream (1) has a dummy output stream and nid 0x1a for input (headset mike via cs42l83)
	// (NOTE this means the line input stream (0x45->0x32) is not assigned currently ie not useable)

        list_for_each_entry(info, &codec->pcm_list_head, list) {
                int stream;

                for (stream = 0; stream < 2; stream++) {
                        struct hda_pcm_stream *hinfo = &info->stream[stream];

			if (hinfo != NULL)
			{
				if (stream == SNDRV_PCM_STREAM_PLAYBACK)
				{
					if (hinfo->nid == 0x02)
					{
						codec_dbg(codec, "cs_8409_init info playback stream %d pointer %p\n",stream,hinfo);
						// so now we need to force the rates and formats to the single one Apple defines ie 44.1 kHz and S24_LE
						// probably can leave S32_LE
						// we can still handle 2/4 channel (what about 1 channel?)
						hinfo->rates = SNDRV_PCM_RATE_44100;
						hinfo->formats = SNDRV_PCM_FMTBIT_S32_LE | SNDRV_PCM_FMTBIT_S24_LE;
						codec_dbg(codec, "playback info stream forced nid 0x%02x rates 0x%08x formats 0x%016llx\n",hinfo->nid,hinfo->rates,hinfo->formats);

						// update the playback function
						hinfo->ops.prepare = cs_8409_playback_pcm_prepare;
					}
				}
				else if (stream == SNDRV_PCM_STREAM_CAPTURE)
				{
					if (hinfo->nid == 0x22)
					{
						// this is the internal mike
						// this is a bit weird - the output nodes are id'ed by output input pin nid
						// but the input nodes are done by the input (adc) nid - not the input pin nid
						codec_dbg(codec, "cs_8409_init info capture stream %d pointer %p\n",stream,hinfo);
						// so now we could force the rates and formats to the single one Apple defines ie 44.1 kHz and S24_LE
						// but this internal mike seems to be a standard HDA input setup so we could have any format here
						//hinfo->rates = SNDRV_PCM_RATE_44100;
						//hinfo->formats = SNDRV_PCM_FMTBIT_S32_LE | SNDRV_PCM_FMTBIT_S24_LE;
						hinfo->rates = SNDRV_PCM_RATE_44100;
						//hinfo->formats = SNDRV_PCM_FMTBIT_S24_3LE;
						hinfo->formats = SNDRV_PCM_FMTBIT_S32_LE | SNDRV_PCM_FMTBIT_S24_LE | SNDRV_PCM_FMTBIT_S24_3LE;
						//hinfo->maxbps = 24;
						codec_dbg(codec, "capture info stream forced nid 0x%02x rates 0x%08x formats 0x%016llx maxbps %d\n",hinfo->nid,hinfo->rates,hinfo->formats,hinfo->maxbps);
						// update the capture function
						hinfo->ops.prepare = cs_8409_capture_pcm_prepare;
					}
					else if (hinfo->nid == 0x1a)
					{
						// this is the external mike ie headset mike
						// this is a bit weird - the output nodes are id'ed by output input pin nid
						// but the input nodes are done by the input (adc) nid - not the input pin nid
						codec_dbg(codec, "cs_8409_init info capture stream %d pointer %p\n",stream,hinfo);
						// so now we force the rates and formats to the single one Apple defines ie 44.1 kHz and S24_LE
						// - because this format is the one being returned by the cs42l83 which is setup by undocumented i2c commands
						hinfo->rates = SNDRV_PCM_RATE_44100;
						//hinfo->formats = SNDRV_PCM_FMTBIT_S24_LE;
						hinfo->formats = SNDRV_PCM_FMTBIT_S32_LE | SNDRV_PCM_FMTBIT_S24_LE | SNDRV_PCM_FMTBIT_S24_3LE;
						//hinfo->maxbps = 24;
						codec_dbg(codec, "capture info stream forced nid 0x%02x rates 0x%08x formats 0x%016llx maxbps %d\n",hinfo->nid,hinfo->rates,hinfo->formats,hinfo->maxbps);
						// update the capture function
						hinfo->ops.prepare = cs_8409_capture_pcm_prepare;
					}
					// still not sure what we do about the linein nid
					// is this bidirectional - because we have no lineout as far as I can see
				}
			}
			else
				codec_dbg(codec, "cs_8409_init info pcm stream %d NULL\n", stream);
		}
		pcmcnt++;
	}


	//list_for_each_entry(kctl, &codec->card->controls, list) {
	//}


	// read UNSOL enable data to see what initial setup is
        //ret_unsol_enable = snd_hda_codec_read(codec, codec->core.afg, 0, AC_VERB_GET_UNSOLICITED_RESPONSE, 0);
	//codec_dbg(codec,"UNSOL event 0x01 boot setup is 0x%08x\n",ret_unsol_enable);
        //ret_unsol_enable = snd_hda_codec_read(codec, 0x47, 0, AC_VERB_GET_UNSOLICITED_RESPONSE, 0);
	//codec_dbg(codec,"UNSOL event 0x47 boot setup is 0x%08x\n",ret_unsol_enable);


	//if (spec->gpio_mask) {
	//	snd_hda_codec_write(codec, 0x01, 0, AC_VERB_SET_GPIO_MASK,
	//			    spec->gpio_mask);
	//	snd_hda_codec_write(codec, 0x01, 0, AC_VERB_SET_GPIO_DIRECTION,
	//			    spec->gpio_dir);
	//	snd_hda_codec_write(codec, 0x01, 0, AC_VERB_SET_GPIO_DATA,
	//			    spec->gpio_data);
	//}

	//if (spec->vendor_nid == CS420X_VENDOR_NID) {
	//	init_input_coef(codec);
	//	init_digital_coef(codec);
	//}

#ifdef ADD_EXTENDED_VERB
	cs_8409_set_extended_codec_verb();
#endif


        myprintk("snd_hda_intel: end cs_8409_init\n");

	return 0;
}

static int cs_8409_build_controls(struct hda_codec *codec)
{
	int err;

        myprintk("snd_hda_intel: cs_8409_build_controls\n");

	err = snd_hda_gen_build_controls(codec);
	if (err < 0)
		return err;
	snd_hda_apply_fixup(codec, HDA_FIXUP_ACT_BUILD);

        myprintk("snd_hda_intel: end cs_8409_build_controls\n");
	return 0;
}

int cs_8409_build_pcms(struct hda_codec *codec)
{
	int retval;
	//struct cs8409_spec *spec = codec->spec;
	//struct hda_pcm *info = NULL;
	//struct hda_pcm_stream *hinfo = NULL;
        myprintk("snd_hda_intel: cs_8409_build_pcms\n");
	retval =  snd_hda_gen_build_pcms(codec);
	// we still dont have the pcm streams defined by here
	// ah this is all done in snd_hda_codec_build_pcms
	// which calls this patch routine or snd_hda_gen_build_pcms
	// but the query supported pcms is only done after this
        myprintk("snd_hda_intel: end cs_8409_build_pcms\n");
	return retval;
}


static void cs_8409_call_jack_callback(struct hda_codec *codec,
                               struct hda_jack_tbl *jack)
{
        struct hda_jack_callback *cb;

        for (cb = jack->callback; cb; cb = cb->next)
                cb->func(codec, cb);
        if (jack->gated_jack) {
                struct hda_jack_tbl *gated =
                        snd_hda_jack_tbl_get(codec, jack->gated_jack);
                if (gated) {
                        for (cb = gated->callback; cb; cb = cb->next)
                                cb->func(codec, cb);
                }
        }
}

// so I think this is what gets called for any unsolicited event - including jack plug events
// so anything we do to switch amp/headphone should be done from here

void cs_8409_jack_unsol_event(struct hda_codec *codec, unsigned int res)
{
        struct hda_jack_tbl *event;
        //int ret_unsol_enable = 0;
        int tag = (res >> AC_UNSOL_RES_TAG_SHIFT) & 0x7f;

	//// read UNSOL enable data to see what current setup is
        //ret_unsol_enable = snd_hda_codec_read(codec, codec->core.afg, 0, AC_VERB_GET_UNSOLICITED_RESPONSE, 0);
	//codec_dbg(codec,"UNSOL event 0x01 at unsol is 0x%08x\n",ret_unsol_enable);
        //ret_unsol_enable = snd_hda_codec_read(codec, 0x47, 0, AC_VERB_GET_UNSOLICITED_RESPONSE, 0);
	//codec_dbg(codec,"UNSOL event 0x47 at unsol is 0x%08x\n",ret_unsol_enable);

	// so it seems the low order byte of the res for the 8409 is a copy of the GPIO register state
	// - except that we dont seem to pass this to the callback functions!!

        mycodec_info(codec, "cs_8409_jack_unsol_event UNSOL 0x%08x tag 0x%02x\n",res,tag);

        event = snd_hda_jack_tbl_get_from_tag(codec, tag, 0);
        if (!event)
                return;
        event->jack_dirty = 1;

	// its the callback struct thats passed as an argument to the callback function
	// so stuff the res data in the private_data member which seems to be used for such a purpose
        event->callback->private_data = res;

        // leave this as is even tho so far have only 1 tag so not really needed
        // so could just call the callback routine directly here
        cs_8409_call_jack_callback(codec, event);

	// this is the code that generates the 0xf09 verb
	// however if we define the jack as a phantom_jack we do not send the 0xf09 verb
	// we need to call this even tho we only have 1 jack to reset jack_dirty
        snd_hda_jack_report_sync(codec);
}

// Im pretty convinced that Apple uses a timed event from the plugin event
// before performing further setup
// not clear how to set this up in linux
// timer might be way to go but there are some limitations on the timer function
// which is not clear is going to work here
// now think just using msleeps is the way to go - this is similar to code in patch_realtek.c
// for dealing with similar issues
//static struct timer_list cs_8409_hp_timer;

//static void cs_8409_hp_timer_callback(struct timer_list *tlist)
//{
//        myprintk("snd_hda_intel: cs_8409_hp_timer_callback\n");
//}

// have an explict one for 8409
// cs_free is just a definition
//#define cs_8409_free		snd_hda_gen_free

void cs_8409_free(struct hda_codec *codec)
{
	//del_timer(&cs_8409_hp_timer);

	snd_hda_gen_free(codec);
}


// note this must come after any function definitions used

static const struct hda_codec_ops cs_8409_patch_ops = {
	.build_controls = cs_8409_build_controls,
	.build_pcms = cs_8409_build_pcms,
	.init = cs_8409_init,
	.free = cs_8409_free,
	.unsol_event = cs_8409_jack_unsol_event,
};


static int cs_8409_create_input_ctls(struct hda_codec *codec);


static int cs_8409_parse_auto_config(struct hda_codec *codec)
{
	struct cs8409_spec *spec = codec->spec;
	int err;
	int i;

        myprintk("snd_hda_intel: cs_8409_parse_auto_config\n");

	err = snd_hda_parse_pin_defcfg(codec, &spec->gen.autocfg, NULL, 0);
	if (err < 0)
		return err;

	err = snd_hda_gen_parse_auto_config(codec, &spec->gen.autocfg);
	if (err < 0)
		return err;

	// note that create_input_ctls is called towards the end of snd_hda_gen_parse_auto_config

	// so it appears the auto config assumes that inputs are connected to ADCs
	// (not true for outputs)

	// I dont really get these - but they dont seem to be useful for the 8409 - seem to allocate nids that are never used
	// they dont seem to be line inputs either
	// well setting num_adc_nids to 0 doesnt work - no inputs defined
	// because it appears the auto config assumes the inputs are connected to an ADC (or audio input converter widget)
	// (NOTE - although these are labelled ADC nodes in the code they may not have an actual analog to digital
	//  converter - may just be a digital sample formatter eg S/PDIF input - for the 8409 the internal mike
	//  seems to be a standard ADC node (0x22) but the headphone input node (0x1a) is a digital input as digitization
	//  has already occurred in the cs42l83)
	// now recoding the input setup in separate function
	//spec->gen.num_adc_nids = 0;


	// new routine to setup inputs - based on the hda_generic code
	cs_8409_create_input_ctls(codec);


        // so do I keep this or not??
	/* keep the ADCs powered up when it's dynamically switchable */
	if (spec->gen.dyn_adc_switch) {
		unsigned int done = 0;
		for (i = 0; i < spec->gen.input_mux.num_items; i++) {
			int idx = spec->gen.dyn_adc_idx[i];
			if (done & (1 << idx))
				continue;
			snd_hda_gen_fix_pin_power(codec,
						  spec->gen.adc_nids[idx]);
			done |= 1 << idx;
		}
	}

        myprintk("snd_hda_intel: end cs_8409_parse_auto_config\n");

	return 0;
}

// pigs - we need a lot of hda_generic local functions
#include "patch_cs8409_hda_generic_copy.h"

// so we need to hack this code because we have more adcs than AUTO_CFG_MAX_INS
// adcs (8) - actual number is 18
// no good way to do this - except to check connection list for each adc and
// see if connected to nid we are looking at
// so define new function

static int cs_8409_add_adc_nid(struct hda_codec *codec, hda_nid_t pin)
{
	struct hda_gen_spec *spec = codec->spec;
	hda_nid_t nid;
	hda_nid_t *adc_nids = spec->adc_nids;
	int max_nums = ARRAY_SIZE(spec->adc_nids);
	int nums = 0;
	int itm = 0;

        myprintk("snd_hda_intel: cs_8409_add_adc_nid pin 0x%x\n",pin);

	for_each_hda_codec_node(nid, codec) {
		unsigned int caps = get_wcaps(codec, nid);
		int type = get_wcaps_type(caps);
		int fndnid = 0;

		if (type != AC_WID_AUD_IN || (caps & AC_WCAP_DIGITAL))
			continue;

		//myprintk("snd_hda_intel: cs_8409_add_adc_nid nid 0x%x\n",nid);

		{
		const hda_nid_t *connptr = NULL;
		int num_conns = snd_hda_get_conn_list(codec, nid, &connptr);
		int i;
		fndnid = 0;
		for (i = 0; i < num_conns; i++) {
			//myprintk("snd_hda_intel: cs_8409_add_adc_nid %d 0x%x\n",num_conns,connptr[i]);
			if (connptr[i] == pin) {
				fndnid = nid;
			}
		}
		}
		if (fndnid == 0)
			continue;

		// save only 1st one we match
		if (spec->num_adc_nids+1 >= max_nums)
			break;
		adc_nids[spec->num_adc_nids] = nid;
		spec->num_adc_nids += 1;
		break;
	}


	codec_dbg(codec, "snd_hda_intel: cs_8409_add_adc_nid num nids %d\n",nums);

	for (itm = 0; itm < spec->num_adc_nids; itm++) {
		myprintk("snd_hda_intel: cs_8409_add_adc_nid 0x%02x\n", spec->adc_nids[itm]);
	}

	myprintk("snd_hda_intel: end cs_8409_add_adc_nid\n");

	return nums;
}



// copied from parse_capture_source in hda_generic.c
// we need this although not changed (apart from printks) because local to hda_generic.c

/* parse capture source paths from the given pin and create imux items */
static int cs_8409_parse_capture_source(struct hda_codec *codec, hda_nid_t pin,
				int cfg_idx, int num_adcs,
				const char *label, int anchor)
{
	struct hda_gen_spec *spec = codec->spec;
	struct hda_input_mux *imux = &spec->input_mux;
	int imux_idx = imux->num_items;
	bool imux_added = false;
	int c;

	myprintk("snd_hda_intel: cs_8409_parse_capture_source pin 0x%x\n",pin);

	for (c = 0; c < num_adcs; c++) {
		struct nid_path *path;
		hda_nid_t adc = spec->adc_nids[c];

		myprintk("snd_hda_intel: cs_8409_parse_capture_source pin 0x%x adc 0x%x check reachable\n",pin,adc);

		if (!is_reachable_path(codec, pin, adc))
			continue;
		myprintk("snd_hda_intel: cs_8409_parse_capture_source pin 0x%x adc 0x%x reachable\n",pin,adc);
		path = snd_hda_add_new_path(codec, pin, adc, anchor);
		if (!path)
			continue;
		print_nid_path(codec, "input", path);
		spec->input_paths[imux_idx][c] =
			snd_hda_get_path_idx(codec, path);

		if (!imux_added) {
			if (spec->hp_mic_pin == pin)
				spec->hp_mic_mux_idx = imux->num_items;
			spec->imux_pins[imux->num_items] = pin;
			snd_hda_add_imux_item(codec, imux, label, cfg_idx, NULL);
			imux_added = true;
			if (spec->dyn_adc_switch)
				spec->dyn_adc_idx[imux_idx] = c;
		}
	}

        myprintk("snd_hda_intel: end cs_8409_parse_capture_source\n");

	return 0;
}


#define CFG_IDX_MIX	99	/* a dummy cfg->input idx for stereo mix */

// copied from create_input_ctls in hda_generic.c

static int cs_8409_create_input_ctls(struct hda_codec *codec)
{
	struct hda_gen_spec *spec = codec->spec;
	const struct auto_pin_cfg *cfg = &spec->autocfg;
	hda_nid_t mixer = spec->mixer_nid;
	int num_adcs = 0;
	int i, err;
	unsigned int val;

        myprintk("snd_hda_intel: cs_8409_create_input_ctls\n");

	// we cannot do this
	//num_adcs = cs_8409_fill_adc_nids(codec);
	//if (num_adcs < 0)
	//	return 0;

	// clear out the auto config setup
	// hope that all_adcs is not different from adc_nids - doesnt seem to be for auto config only
	memset(spec->adc_nids, 0, sizeof(spec->adc_nids));
	memset(spec->all_adcs, 0, sizeof(spec->all_adcs));
	spec->num_adc_nids = 0;

	for (i = 0; i < cfg->num_inputs; i++) {
		hda_nid_t pin;
		int fndadc = 0;

		myprintk("snd_hda_intel: cs_8409_create_input_ctls - input %d\n",i);

		pin = cfg->inputs[i].pin;
		if (!is_input_pin(codec, pin))
			continue;

		myprintk("snd_hda_intel: cs_8409_create_input_ctls - input %d pin 0x%x\n",i,pin);

		// now scan all nodes for adc nodes and find one connected to this pin
		fndadc = cs_8409_add_adc_nid(codec, pin);
		if (!fndadc)
			continue;
	}

	num_adcs = spec->num_adc_nids;

	/* copy the detected ADCs to all_adcs[] */
	spec->num_all_adcs = spec->num_adc_nids;
	memcpy(spec->all_adcs, spec->adc_nids,  spec->num_adc_nids* sizeof(hda_nid_t));

	err = fill_input_pin_labels(codec);
	if (err < 0)
		return err;

	for (i = 0; i < cfg->num_inputs; i++) {
		hda_nid_t pin;
		int fndadc = 0;

		myprintk("snd_hda_intel: cs_8409_create_input_ctls - input %d\n",i);

		pin = cfg->inputs[i].pin;
		if (!is_input_pin(codec, pin))
			continue;

		myprintk("snd_hda_intel: cs_8409_create_input_ctls - input %d pin 0x%x\n",i,pin);

		//// now scan the adc nodes and find one connected to this pin
		//fndadc = cs_8409_add_adc_nid(codec, pin);
		//if (!fndadc)
		//	continue;

		val = PIN_IN;
		if (cfg->inputs[i].type == AUTO_PIN_MIC)
			val |= snd_hda_get_default_vref(codec, pin);

		myprintk("snd_hda_intel: cs_8409_create_input_ctls - input %d pin 0x%x val 0x%x\n",i,pin,val);

		if (pin != spec->hp_mic_pin &&
		    !snd_hda_codec_get_pin_target(codec, pin))
			set_pin_target(codec, pin, val, false);

		myprintk("snd_hda_intel: cs_8409_create_input_ctls - input %d pin 0x%x val 0x%x mixer 0x%x\n",i,pin,val,mixer);

		if (mixer) {
			if (is_reachable_path(codec, pin, mixer)) {
				err = new_analog_input(codec, i, pin,
						       spec->input_labels[i],
						       spec->input_label_idxs[i],
						       mixer);
				if (err < 0)
					return err;
			}
		}

		// so connections are from the adc nid to the input pin nid
		//{
		//const hda_nid_t conn[256];
		//const hda_nid_t *connptr = conn;
		//int num_conns = snd_hda_get_conn_list(codec, pin, &connptr);
		//int i;
		//myprintk("snd_hda_intel: cs_8409_create_input_ctls pin 0x%x num conn %d\n",pin,num_conns);
		//for (i = 0; i < num_conns; i++) {
		//	myprintk("snd_hda_intel: cs_8409_create_input_ctls pin 0x%x conn 0x%x\n",pin,conn[i]);
		//}
		//}


		// this is the problem routine - this loops over the adcs to do anything
		// so if num_adcs is 0 or none of the adc entries are used this does nothing

		err = cs_8409_parse_capture_source(codec, pin, i, num_adcs,
					   spec->input_labels[i], -mixer);
		if (err < 0)
			return err;

		// comment for the moment as needs lots of other functions
		//if (spec->add_jack_modes) {
		//	err = create_in_jack_mode(codec, pin);
		//	if (err < 0)
		//		return err;
		//}
	}

	/* add stereo mix when explicitly enabled via hint */
	if (mixer && spec->add_stereo_mix_input == HDA_HINT_STEREO_MIX_ENABLE) {
		err = cs_8409_parse_capture_source(codec, mixer, CFG_IDX_MIX, num_adcs,
					   "Stereo Mix", 0);
		if (err < 0)
			return err;
		else
			spec->suppress_auto_mic = 1;
	}

        myprintk("snd_hda_intel: end cs_8409_create_input_ctls\n");

	return 0;
}


/* do I need this for 8409 - I certainly need some gpio patching */
void cs_8409_fixup_gpio(struct hda_codec *codec,
			const struct hda_fixup *fix, int action)
{
       myprintk("snd_hda_intel: cs_8409_fixup_gpio\n");

       // allowable states
       // HDA_FIXUP_ACT_PRE_PROBE,
       // HDA_FIXUP_ACT_PROBE,
       // HDA_FIXUP_ACT_INIT,
       // HDA_FIXUP_ACT_BUILD,
       // HDA_FIXUP_ACT_FREE,

       // so inspection suggests no eapd usage on macs - no 0xf0c or 0x70c commands sent

       if (action == HDA_FIXUP_ACT_PRE_PROBE) {
               //struct cs8409_spec *spec = codec->spec;

               myprintk("snd_hda_intel: cs_8409_fixup_gpio pre probe\n");

               //myprintk("fixup gpio hp=0x%x speaker=0x%x\n", hp_out_mask, speaker_out_mask);
               //spec->gpio_eapd_hp = hp_out_mask;
               //spec->gpio_eapd_speaker = speaker_out_mask;
               //spec->gpio_mask = 0xff;
               //spec->gpio_data =
               //  spec->gpio_dir =
               //  spec->gpio_eapd_hp | spec->gpio_eapd_speaker;
       }
       else if (action == HDA_FIXUP_ACT_PROBE) {
               myprintk("snd_hda_intel: cs_8409_fixup_gpio probe\n");
       }
       else if (action == HDA_FIXUP_ACT_INIT) {
               myprintk("snd_hda_intel: cs_8409_fixup_gpio init\n");
       }
       else if (action == HDA_FIXUP_ACT_BUILD) {
               myprintk("snd_hda_intel: cs_8409_fixup_gpio build\n");
       }
       else if (action == HDA_FIXUP_ACT_FREE) {
               myprintk("snd_hda_intel: cs_8409_fixup_gpio free\n");
       }
       myprintk("snd_hda_intel: end cs_8409_fixup_gpio\n");
}

static void cs_8409_cs42l83_unsolicited_response(struct hda_codec *codec, unsigned int res);

static void cs_8409_cs42l83_callback(struct hda_codec *codec, struct hda_jack_callback *event)
{
	struct cs8409_spec *spec = codec->spec;

        mycodec_info(codec, "cs_8409_cs42l83_callback\n");

	// so we have confirmed that these unsol responses are not in linux kernel interrupt state
	//if (in_interrupt())
	//	mycodec_info(codec, "cs_8409_cs42l83_callback - INTERRUPT\n");
	//else
	//	mycodec_info(codec, "cs_8409_cs42l83_callback - not interrupt\n");

	// print the stored unsol res which seems to be the GPIO pins state
	mycodec_info(codec, "cs_8409_cs42l83_callback - event private data 0x%08x\n",event->private_data);


	cs_8409_cs42l83_unsolicited_response(codec, event->private_data);


	// now think timers not the way to go
	// patch_realtek.c has to deal with similar issues of plugin, headset detection
	// and just uses msleep calls
	//mod_timer(&cs_8409_hp_timer, jiffies + msecs_to_jiffies(250));

        // the delayed_work feature might be a way to go tho

        mycodec_info(codec, "cs_8409_cs42l83_callback end\n");
}


// dont know how to handle the headphone plug in/out yet
// unfortunately Im guessing these are based on the HDA spec pin event operation
// and not sure how to trigger the pin events from the logged OSX code of plug in/out events
// ah - the HDA spec says a jack plug event triggers an unsolicted response
// plus sets presence detect bits read by command 0xf09
// we have 4 automute hooks
// void (*automute_hook)(struct hda_codec *codec);
// void (*hp_automute_hook)(struct hda_codec *codec, struct hda_jack_callback *cb);
// void (*line_automute_hook)(struct hda_codec *codec, struct hda_jack_callback *cb);
// void (*mic_autoswitch_hook)(struct hda_codec *codec, struct hda_jack_callback *cb);

static void cs_8409_automute(struct hda_codec *codec)
{
	struct cs8409_spec *spec = codec->spec;
	dev_info(hda_codec_dev(codec), "cs_8409_automute called\n");
}

static int cs_8409_boot_setup(struct hda_codec *codec);


static void cs_8409_playback_pcm_hook(struct hda_pcm_stream *hinfo,
                                      struct hda_codec *codec,
                                      struct snd_pcm_substream *substream,
                                      int action);

static void cs_8409_capture_pcm_hook(struct hda_pcm_stream *hinfo,
                                     struct hda_codec *codec,
                                     struct snd_pcm_substream *substream,
                                     int action);



static int patch_cs8409(struct hda_codec *codec)
{
        struct cs8409_spec *spec;
        int err;
        int itm;
        //hda_nid_t *dac_nids_ptr = NULL;

        int explicit = 0;

        //struct hda_pcm *info = NULL;
        //struct hda_pcm_stream *hinfo = NULL;

        myprintk("snd_hda_intel: Patching for CS8409 explicit %d\n", explicit);
        //mycodec_info(codec, "Patching for CS8409 %d\n", explicit);

        //dump_stack();

        spec = cs8409_alloc_spec(codec);
        if (!spec)
                return -ENOMEM;

	spec->vendor_nid = CS8409_VENDOR_NID;
        spec->beep_nid = CS8409_BEEP_NID;

        spec->use_data = 0;

        if (explicit)
               {
               //codec->patch_ops = cs_8409_patch_ops_explicit;
               }
        else
               codec->patch_ops = cs_8409_patch_ops;

        spec->gen.pcm_playback_hook = cs_8409_playback_pcm_hook;

        spec->gen.pcm_capture_hook = cs_8409_capture_pcm_hook;

        spec->gen.automute_hook = cs_8409_automute;

        // so it appears we need to explicitly apply pre probe fixups here
        // note that if the pinconfigs lists are empty the pin config fixup
        // is effectively ignored

        //myprintk("cs8409 - 1\n");
        //snd_hda_pick_fixup(codec, cs8409_models, cs8409_fixup_tbl,
        //                   cs8409_fixups);
        //myprintk("cs8409 - 2\n");
        //snd_hda_apply_fixup(codec, HDA_FIXUP_ACT_PRE_PROBE);


        //timer_setup(&cs_8409_hp_timer, cs_8409_hp_timer_callback, 0);

        myprintk("snd_hda_intel: cs 8409 jack used %d\n",codec->jacktbl.used);

        // use this to cause unsolicited responses to be stored
        // but not run
        spec->block_unsol = 0;

        INIT_LIST_HEAD(&spec->unsol_list);

        for (itm=0; itm<10; itm++)
                { spec->unsol_items_prealloc_used[itm] = 0; }


        // for the moment set initial jack status to not present
        // we will detect if have jack plugged in on boot later
        spec->jack_present = 0;


        spec->headset_type = 0;

        spec->have_mike = 0;

        spec->have_buttons = 0;

        spec->playing = 0;
        spec->capturing = 0;

        spec->headset_play_format_setup_needed = 1;
        spec->headset_capture_format_setup_needed = 1;

        spec->headset_presetup_done = 0;


        // use this to distinguish which unsolicited phase we are in
        // for the moment - we only seem to get a tag of 0x37 and dont see any
        // different tags being setup in OSX logs
        spec->headset_phase = 0;

        spec->headset_enable = 0;


        // so it appears we dont get interrupts in the auto config stage

        // we need to figure out how to setup the jack detect callback
        // not clear what nid should be used - 0x01 or 0x47
        // added a tag argument because we seem to get a tag
        // so far the tag seems to be 0x37
        cs_8409_hda_jack_detect_enable_callback(codec, 0x01, 0x37, cs_8409_cs42l83_callback);

        myprintk("snd_hda_intel: cs 8409 jack used callback %d\n",codec->jacktbl.used);


        //      cs8409_pinmux_init(codec);

       if (!explicit)
       {

              myprintk("snd_hda_intel: pre cs_8409_parse_auto_config\n");

              err = cs_8409_parse_auto_config(codec);
              if (err < 0)
                      goto error;

              myprintk("snd_hda_intel: post cs_8409_parse_auto_config\n");
       }

       // dump headphone config
       myprintk("snd_hda_intel: headphone config hp_jack_present %d\n",spec->gen.hp_jack_present);
       myprintk("snd_hda_intel: headphone config line_jack_present %d\n",spec->gen.line_jack_present);
       myprintk("snd_hda_intel: headphone config speaker_muted %d\n",spec->gen.speaker_muted);
       myprintk("snd_hda_intel: headphone config line_out_muted %d\n",spec->gen.line_out_muted);
       myprintk("snd_hda_intel: headphone config auto_mic %d\n",spec->gen.auto_mic);
       myprintk("snd_hda_intel: headphone config automute_speaker %d\n",spec->gen.automute_speaker);
       myprintk("snd_hda_intel: headphone config automute_lo %d\n",spec->gen.automute_lo);
       myprintk("snd_hda_intel: headphone config detect_hp %d\n",spec->gen.detect_hp);
       myprintk("snd_hda_intel: headphone config detect_lo %d\n",spec->gen.detect_lo);
       myprintk("snd_hda_intel: headphone config keep_vref_in_automute %d\n",spec->gen.keep_vref_in_automute);
       myprintk("snd_hda_intel: headphone config line_in_auto_switch %d\n",spec->gen.line_in_auto_switch);
       myprintk("snd_hda_intel: headphone config auto_mute_via_amp %d\n",spec->gen.auto_mute_via_amp);
       myprintk("snd_hda_intel: headphone config suppress_auto_mute %d\n",spec->gen.suppress_auto_mute);
       myprintk("snd_hda_intel: headphone config suppress_auto_mic %d\n",spec->gen.suppress_auto_mic);

       myprintk("snd_hda_intel: headphone config hp_mic %d\n",spec->gen.hp_mic);

       myprintk("snd_hda_intel: headphone config suppress_hp_mic_detect %d\n",spec->gen.suppress_hp_mic_detect);


       myprintk("snd_hda_intel: auto config pins line_outs %d\n", spec->gen.autocfg.line_outs);
       myprintk("snd_hda_intel: auto config pins line_outs 0x%02x\n", spec->gen.autocfg.line_out_pins[0]);
       myprintk("snd_hda_intel: auto config pins line_outs 0x%02x\n", spec->gen.autocfg.line_out_pins[1]);
       myprintk("snd_hda_intel: auto config pins speaker_outs %d\n", spec->gen.autocfg.speaker_outs);
       myprintk("snd_hda_intel: auto config pins speaker_outs 0x%02x\n", spec->gen.autocfg.speaker_pins[0]);
       myprintk("snd_hda_intel: auto config pins speaker_outs 0x%02x\n", spec->gen.autocfg.speaker_pins[1]);
       myprintk("snd_hda_intel: auto config pins hp_outs %d\n", spec->gen.autocfg.hp_outs);
       myprintk("snd_hda_intel: auto config pins hp_outs 0x%02x\n", spec->gen.autocfg.hp_pins[0]);
       myprintk("snd_hda_intel: auto config pins inputs %d\n", spec->gen.autocfg.num_inputs);

       myprintk("snd_hda_intel: auto config pins inputs  pin 0x%02x\n", spec->gen.autocfg.inputs[0].pin);
       myprintk("snd_hda_intel: auto config pins inputs type %d\n", spec->gen.autocfg.inputs[0].type);
       myprintk("snd_hda_intel: auto config pins inputs is head set mic %d\n", spec->gen.autocfg.inputs[0].is_headset_mic);
       myprintk("snd_hda_intel: auto config pins inputs is head phn mic %d\n", spec->gen.autocfg.inputs[0].is_headphone_mic);
       myprintk("snd_hda_intel: auto config pins inputs is        boost %d\n", spec->gen.autocfg.inputs[0].has_boost_on_pin);

       myprintk("snd_hda_intel: auto config pins inputs  pin 0x%02x\n", spec->gen.autocfg.inputs[1].pin);
       myprintk("snd_hda_intel: auto config pins inputs type %d\n", spec->gen.autocfg.inputs[1].type);
       myprintk("snd_hda_intel: auto config pins inputs is head set mic %d\n", spec->gen.autocfg.inputs[1].is_headset_mic);
       myprintk("snd_hda_intel: auto config pins inputs is head phn mic %d\n", spec->gen.autocfg.inputs[1].is_headphone_mic);
       myprintk("snd_hda_intel: auto config pins inputs is        boost %d\n", spec->gen.autocfg.inputs[1].has_boost_on_pin);

       myprintk("snd_hda_intel: auto config inputs num_adc_nids %d\n", spec->gen.num_adc_nids);
       for (itm = 0; itm < spec->gen.num_adc_nids; itm++) {
               myprintk("snd_hda_intel: auto config inputs adc_nids 0x%02x\n", spec->gen.adc_nids[itm]);
       }

       myprintk("snd_hda_intel: auto config multiout is num_dacs %d\n", spec->gen.multiout.num_dacs);
       for (itm = 0; itm < spec->gen.multiout.num_dacs; itm++) {
               myprintk("snd_hda_intel: auto config multiout is    dac_nids 0x%02x\n", spec->gen.multiout.dac_nids[itm]);
       }

       myprintk("snd_hda_intel: auto config multiout is      hp_nid 0x%02x\n", spec->gen.multiout.hp_nid);

       for (itm = 0; itm < ARRAY_SIZE(spec->gen.multiout.hp_out_nid); itm++) {
               if (spec->gen.multiout.hp_out_nid[itm])
                       myprintk("snd_hda_intel: auto config multiout is  hp_out_nid 0x%02x\n", spec->gen.multiout.hp_out_nid[itm]);
       }
       for (itm = 0; itm < ARRAY_SIZE(spec->gen.multiout.extra_out_nid); itm++) {
               if (spec->gen.multiout.extra_out_nid[itm])
                       myprintk("snd_hda_intel: auto config multiout is xtr_out_nid 0x%02x\n", spec->gen.multiout.extra_out_nid[itm]);
       }

       myprintk("snd_hda_intel: auto config multiout is dig_out_nid 0x%02x\n", spec->gen.multiout.dig_out_nid);
       myprintk("snd_hda_intel: auto config multiout is slv_dig_out %p\n", spec->gen.multiout.slave_dig_outs);


       // dump the rates/format of the afg node
       // still havent figured out how the user space gets the allowed formats
       // ah - may have figured this
       // except that at this point this is NULL - we need to be after build pcms
       //info = spec->gen.pcm_rec[0];
       //if (info != NULL)
       //{
       //       hinfo = &(info->stream[SNDRV_PCM_STREAM_PLAYBACK]);
       //       if (hinfo != NULL)
       //              codec_dbg(codec, "playback info stream nid 0x%02x rates 0x%08x formats 0x%016llx\n",hinfo->nid,hinfo->rates,hinfo->formats);
       //       else
       //              codec_dbg(codec, "playback info stream NULL\n");
       //}
       //else
       //       codec_dbg(codec, "playback info NULL\n");


       // try removing the unused nodes
       //spec->gen.autocfg.line_outs = 0;
       //spec->gen.autocfg.hp_outs = 0;

       // I dont really get these - but they dont seem to be useful for the 8409 - seem to allocate nids that are never used
       // they dont seem to be line inputs either
       // well setting num_adc_nids to 0 doesnt work - no inputs defined
       // - because all input pin nodes need to be connected to an audio input converter node
       // - which in the hda_generic.c code are labelled as adc nodes/nids
       // now recoding the input setup in separate function
       //spec->gen.num_adc_nids = 0;

       // these seem to be the primary mike inputs ? maybe line inputs??
       //spec->gen.autocfg.num_inputs = 0;

       // to clobber the headphone output we would need to clear the hp_out_nid array
       //spec->gen.multiout.hp_out_nid[0] = 0x00;
       // do this to prevent copying to other streams
       // well this clobbers output!!
       //spec->gen.multiout.no_share_stream = 1;

       // see if using 0x03 only works
       // difficult - apparently dac_nids is a pointer to an array
       // and the spec struct is a const - so we cant change array elements
       // but we can change the pointer to a new list
       // - although we need to update the array elements
       // BEFORE changing the spec pointer - this is rather stupid
       // because we STILL cant update the array elements as an item of the struct
       // maybe if I copied the pointer to a local variable I could update the elements
       // yes that works - because the const qualifier is ignored
       //spec->gen.multiout.num_dacs = 1;
       //spec->gen.multiout.dac_nids = spec->cs_8409_dac_nids;
       //dac_nids_ptr = spec->gen.multiout.dac_nids;
       //dac_nids_ptr[0] = 0x03;
       //dac_nids_ptr[1] = 0x00;
       //spec->gen.multiout.dac_nids[0] = 0x03;
       //spec->gen.multiout.dac_nids[1] = 0x00;


       myprintk("snd_hda_intel: cs 8409 jack used post %d\n",codec->jacktbl.used);


       err = cs_8409_boot_setup(codec);
       if (err < 0)
               goto error;

       // update the headset phase
       spec->headset_phase = 1;

       spec->play_init = 0;
       spec->capture_init = 0;

       // init the last play time
       ktime_get_real_ts64(&(spec->last_play_time));

       ktime_get_real_ts64(&(spec->first_play_time));

       myprintk("snd_hda_intel: Post Patching for CS8409\n");
       //mycodec_info(codec, "Post Patching for CS8409\n");

       return 0;

 error:
       cs8409_free(codec);
       return err;
}


// for the moment split the new code into an include file

#include "patch_cs8409_new84.h"


// new function to use "vendor" defined commands to run
// a specific code
// has to be here to use functions defined in patch_cirrus_new84.h

static unsigned int
cs_8409_extended_codec_verb(struct hda_codec *codec, hda_nid_t nid,
                                int flags,
                                unsigned int verb, unsigned int parm)
{
	//static inline unsigned int cs_8409_vendor_i2cRead(struct hda_codec *codec, unsigned int i2c_address,
	//                                    unsigned int i2c_reg, unsigned int paged)
	unsigned int retval1 = 0;
	unsigned int retval2 = 0;
	unsigned int retval3 = 0;
	unsigned int retval4 = 0;
	unsigned int retval = 0;

        myprintk("snd_hda_intel: cs_8409_extended_codec_verb nid 0x%02x flags 0x%x verb 0x%03x parm 0x%04x\n", nid, flags, verb, parm);

	if ((verb & 0x0ff8) == 0xf78)
	{
		retval1 = cs_8409_vendor_i2cWrite(codec, 0x64, 0x2d, parm, 0);
		retval2 = cs_8409_vendor_i2cWrite(codec, 0x62, 0x2d, parm, 0);
		retval3 = cs_8409_vendor_i2cWrite(codec, 0x74, 0x2d, parm, 0);
		retval4 = cs_8409_vendor_i2cWrite(codec, 0x72, 0x2d, parm, 0);

		myprintk("snd_hda_intel: cs_8409_extended_codec_verb wr ret 1 0x%x\n",retval1);
		myprintk("snd_hda_intel: cs_8409_extended_codec_verb wr ret 2 0x%x\n",retval2);
		myprintk("snd_hda_intel: cs_8409_extended_codec_verb wr ret 3 0x%x\n",retval3);
		myprintk("snd_hda_intel: cs_8409_extended_codec_verb wr ret 4 0x%x\n",retval4);
	}
	else if ((verb & 0x0ff8) == 0xff8)
	{
		retval1 = cs_8409_vendor_i2cRead(codec, 0x64, 0x2d, 0);
		retval2 = cs_8409_vendor_i2cRead(codec, 0x62, 0x2d, 0);
		retval3 = cs_8409_vendor_i2cRead(codec, 0x74, 0x2d, 0);
		retval4 = cs_8409_vendor_i2cRead(codec, 0x72, 0x2d, 0);

		myprintk("snd_hda_intel: cs_8409_extended_codec_verb rd ret 1 0x%x\n",retval1);
		myprintk("snd_hda_intel: cs_8409_extended_codec_verb rd ret 2 0x%x\n",retval2);
		myprintk("snd_hda_intel: cs_8409_extended_codec_verb rd ret 3 0x%x\n",retval3);
		myprintk("snd_hda_intel: cs_8409_extended_codec_verb rd ret 4 0x%x\n",retval4);
	}


	retval = retval1;

	return retval;
}

#ifdef ADD_EXTENDED_VERB
static void cs_8409_set_extended_codec_verb(void)
{
	snd_hda_set_extended_codec_verb(cs_8409_extended_codec_verb);
}
#endif

static const struct hda_device_id snd_hda_id_cs8409[] = {
	HDA_CODEC_ENTRY(0x10138409, "CS8409", patch_cs8409),
	{} /* terminator */
};
MODULE_DEVICE_TABLE(hdaudio, snd_hda_id_cs8409);

static struct hda_codec_driver cs8409_driver = {
	.id = snd_hda_id_cs8409,
};
module_hda_codec_driver(cs8409_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Cirrus Logic HDA bridge");
