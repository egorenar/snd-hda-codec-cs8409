/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * HD audio interface patch for Cirrus Logic CS8409 HDA bridge chip
 *
 * Copyright (C) 2021 Cirrus Logic, Inc. and
 *                    Cirrus Logic International Semiconductor Ltd.
 */

#ifndef __CS8409_PATCH_H
#define __CS8409_PATCH_H

#include <linux/pci.h>
#include <sound/tlv.h>
#include <linux/workqueue.h>
#include <sound/cs42l42.h>
#include <sound/hda_codec.h>
#include "hda_local.h"
#include "hda_auto_parser.h"
#include "hda_jack.h"
#include "hda_generic.h"

/* CS8409 Specific Definitions */

enum cs8409_pins {
	CS8409_PIN_ROOT,
	CS8409_PIN_AFG,
	CS8409_PIN_ASP1_OUT_A,
	CS8409_PIN_ASP1_OUT_B,
	CS8409_PIN_ASP1_OUT_C,
	CS8409_PIN_ASP1_OUT_D,
	CS8409_PIN_ASP1_OUT_E,
	CS8409_PIN_ASP1_OUT_F,
	CS8409_PIN_ASP1_OUT_G,
	CS8409_PIN_ASP1_OUT_H,
	CS8409_PIN_ASP2_OUT_A,
	CS8409_PIN_ASP2_OUT_B,
	CS8409_PIN_ASP2_OUT_C,
	CS8409_PIN_ASP2_OUT_D,
	CS8409_PIN_ASP2_OUT_E,
	CS8409_PIN_ASP2_OUT_F,
	CS8409_PIN_ASP2_OUT_G,
	CS8409_PIN_ASP2_OUT_H,
	CS8409_PIN_ASP1_IN_A,
	CS8409_PIN_ASP1_IN_B,
	CS8409_PIN_ASP1_IN_C,
	CS8409_PIN_ASP1_IN_D,
	CS8409_PIN_ASP1_IN_E,
	CS8409_PIN_ASP1_IN_F,
	CS8409_PIN_ASP1_IN_G,
	CS8409_PIN_ASP1_IN_H,
	CS8409_PIN_ASP2_IN_A,
	CS8409_PIN_ASP2_IN_B,
	CS8409_PIN_ASP2_IN_C,
	CS8409_PIN_ASP2_IN_D,
	CS8409_PIN_ASP2_IN_E,
	CS8409_PIN_ASP2_IN_F,
	CS8409_PIN_ASP2_IN_G,
	CS8409_PIN_ASP2_IN_H,
	CS8409_PIN_DMIC1,
	CS8409_PIN_DMIC2,
	CS8409_PIN_ASP1_TRANSMITTER_A,
	CS8409_PIN_ASP1_TRANSMITTER_B,
	CS8409_PIN_ASP1_TRANSMITTER_C,
	CS8409_PIN_ASP1_TRANSMITTER_D,
	CS8409_PIN_ASP1_TRANSMITTER_E,
	CS8409_PIN_ASP1_TRANSMITTER_F,
	CS8409_PIN_ASP1_TRANSMITTER_G,
	CS8409_PIN_ASP1_TRANSMITTER_H,
	CS8409_PIN_ASP2_TRANSMITTER_A,
	CS8409_PIN_ASP2_TRANSMITTER_B,
	CS8409_PIN_ASP2_TRANSMITTER_C,
	CS8409_PIN_ASP2_TRANSMITTER_D,
	CS8409_PIN_ASP2_TRANSMITTER_E,
	CS8409_PIN_ASP2_TRANSMITTER_F,
	CS8409_PIN_ASP2_TRANSMITTER_G,
	CS8409_PIN_ASP2_TRANSMITTER_H,
	CS8409_PIN_ASP1_RECEIVER_A,
	CS8409_PIN_ASP1_RECEIVER_B,
	CS8409_PIN_ASP1_RECEIVER_C,
	CS8409_PIN_ASP1_RECEIVER_D,
	CS8409_PIN_ASP1_RECEIVER_E,
	CS8409_PIN_ASP1_RECEIVER_F,
	CS8409_PIN_ASP1_RECEIVER_G,
	CS8409_PIN_ASP1_RECEIVER_H,
	CS8409_PIN_ASP2_RECEIVER_A,
	CS8409_PIN_ASP2_RECEIVER_B,
	CS8409_PIN_ASP2_RECEIVER_C,
	CS8409_PIN_ASP2_RECEIVER_D,
	CS8409_PIN_ASP2_RECEIVER_E,
	CS8409_PIN_ASP2_RECEIVER_F,
	CS8409_PIN_ASP2_RECEIVER_G,
	CS8409_PIN_ASP2_RECEIVER_H,
	CS8409_PIN_DMIC1_IN,
	CS8409_PIN_DMIC2_IN,
	CS8409_PIN_BEEP_GEN,
	CS8409_PIN_VENDOR_WIDGET
};

enum cs8409_coefficient_index_registers {
	CS8409_DEV_CFG1,
	CS8409_DEV_CFG2,
	CS8409_DEV_CFG3,
	CS8409_ASP1_CLK_CTRL1,
	CS8409_ASP1_CLK_CTRL2,
	CS8409_ASP1_CLK_CTRL3,
	CS8409_ASP2_CLK_CTRL1,
	CS8409_ASP2_CLK_CTRL2,
	CS8409_ASP2_CLK_CTRL3,
	CS8409_DMIC_CFG,
	CS8409_BEEP_CFG,
	ASP1_RX_NULL_INS_RMV,
	ASP1_Rx_RATE1,
	ASP1_Rx_RATE2,
	ASP1_Tx_NULL_INS_RMV,
	ASP1_Tx_RATE1,
	ASP1_Tx_RATE2,
	ASP2_Rx_NULL_INS_RMV,
	ASP2_Rx_RATE1,
	ASP2_Rx_RATE2,
	ASP2_Tx_NULL_INS_RMV,
	ASP2_Tx_RATE1,
	ASP2_Tx_RATE2,
	ASP1_SYNC_CTRL,
	ASP2_SYNC_CTRL,
	ASP1_A_TX_CTRL1,
	ASP1_A_TX_CTRL2,
	ASP1_B_TX_CTRL1,
	ASP1_B_TX_CTRL2,
	ASP1_C_TX_CTRL1,
	ASP1_C_TX_CTRL2,
	ASP1_D_TX_CTRL1,
	ASP1_D_TX_CTRL2,
	ASP1_E_TX_CTRL1,
	ASP1_E_TX_CTRL2,
	ASP1_F_TX_CTRL1,
	ASP1_F_TX_CTRL2,
	ASP1_G_TX_CTRL1,
	ASP1_G_TX_CTRL2,
	ASP1_H_TX_CTRL1,
	ASP1_H_TX_CTRL2,
	ASP2_A_TX_CTRL1,
	ASP2_A_TX_CTRL2,
	ASP2_B_TX_CTRL1,
	ASP2_B_TX_CTRL2,
	ASP2_C_TX_CTRL1,
	ASP2_C_TX_CTRL2,
	ASP2_D_TX_CTRL1,
	ASP2_D_TX_CTRL2,
	ASP2_E_TX_CTRL1,
	ASP2_E_TX_CTRL2,
	ASP2_F_TX_CTRL1,
	ASP2_F_TX_CTRL2,
	ASP2_G_TX_CTRL1,
	ASP2_G_TX_CTRL2,
	ASP2_H_TX_CTRL1,
	ASP2_H_TX_CTRL2,
	ASP1_A_RX_CTRL1,
	ASP1_A_RX_CTRL2,
	ASP1_B_RX_CTRL1,
	ASP1_B_RX_CTRL2,
	ASP1_C_RX_CTRL1,
	ASP1_C_RX_CTRL2,
	ASP1_D_RX_CTRL1,
	ASP1_D_RX_CTRL2,
	ASP1_E_RX_CTRL1,
	ASP1_E_RX_CTRL2,
	ASP1_F_RX_CTRL1,
	ASP1_F_RX_CTRL2,
	ASP1_G_RX_CTRL1,
	ASP1_G_RX_CTRL2,
	ASP1_H_RX_CTRL1,
	ASP1_H_RX_CTRL2,
	ASP2_A_RX_CTRL1,
	ASP2_A_RX_CTRL2,
	ASP2_B_RX_CTRL1,
	ASP2_B_RX_CTRL2,
	ASP2_C_RX_CTRL1,
	ASP2_C_RX_CTRL2,
	ASP2_D_RX_CTRL1,
	ASP2_D_RX_CTRL2,
	ASP2_E_RX_CTRL1,
	ASP2_E_RX_CTRL2,
	ASP2_F_RX_CTRL1,
	ASP2_F_RX_CTRL2,
	ASP2_G_RX_CTRL1,
	ASP2_G_RX_CTRL2,
	ASP2_H_RX_CTRL1,
	ASP2_H_RX_CTRL2,
	CS8409_I2C_ADDR,
	CS8409_I2C_DATA,
	CS8409_I2C_CTRL,
	CS8409_I2C_STS,
	CS8409_I2C_QWRITE,
	CS8409_I2C_QREAD,
	CS8409_SPI_CTRL,
	CS8409_SPI_TX_DATA,
	CS8409_SPI_RX_DATA,
	CS8409_SPI_STS,
	CS8409_PFE_COEF_W1, /* Parametric filter engine coefficient write 1*/
	CS8409_PFE_COEF_W2,
	CS8409_PFE_CTRL1,
	CS8409_PFE_CTRL2,
	CS8409_PRE_SCALE_ATTN1,
	CS8409_PRE_SCALE_ATTN2,
	CS8409_PFE_COEF_MON1, /* Parametric filter engine coefficient monitor 1*/
	CS8409_PFE_COEF_MON2,
	CS8409_ASP1_INTRN_STS,
	CS8409_ASP2_INTRN_STS,
	CS8409_ASP1_RX_SCLK_COUNT,
	CS8409_ASP1_TX_SCLK_COUNT,
	CS8409_ASP2_RX_SCLK_COUNT,
	CS8409_ASP2_TX_SCLK_COUNT,
	CS8409_ASP_UNS_RESP_MASK,
	CS8409_LOOPBACK_CTRL = 0x80,
	CS8409_PAD_CFG_SLW_RATE_CTRL = 0x82, /* Pad Config and Slew Rate Control (CIR = 0x0082) */
};

/* CS42L42 Specific Definitions */

#define CS8409_MAX_CODECS			8
#define CS42L42_VOLUMES				(4U)
#define CS42L42_HP_VOL_REAL_MIN			(-63)
#define CS42L42_HP_VOL_REAL_MAX			(0)
#define CS42L42_AMIC_VOL_REAL_MIN		(-97)
#define CS42L42_AMIC_VOL_REAL_MAX		(12)
#define CS42L42_REG_AMIC_VOL_MASK		(0x00FF)
#define CS42L42_HSTYPE_MASK			(0x03)
#define CS42L42_I2C_TIMEOUT_US			(20000)
#define CS42L42_I2C_SLEEP_US			(2000)
#define CS42L42_PDN_TIMEOUT_US			(250000)
#define CS42L42_PDN_SLEEP_US			(2000)
#define CS42L42_ANA_MUTE_AB			(0x0C)
#define CS42L42_FULL_SCALE_VOL_MASK		(2)
#define CS42L42_FULL_SCALE_VOL_0DB		(0)
#define CS42L42_FULL_SCALE_VOL_MINUS6DB		(1)

/* Dell BULLSEYE / WARLOCK / CYBORG Specific Definitions */

#define CS42L42_I2C_ADDR			(0x48 << 1)
#define CS8409_CS42L42_RESET			GENMASK(5, 5) /* CS8409_GPIO5 */
#define CS8409_CS42L42_INT			GENMASK(4, 4) /* CS8409_GPIO4 */
#define CS8409_CYBORG_SPEAKER_PDN		GENMASK(2, 2) /* CS8409_GPIO2 */
#define CS8409_WARLOCK_SPEAKER_PDN		GENMASK(1, 1) /* CS8409_GPIO1 */
#define CS8409_CS42L42_HP_PIN_NID		CS8409_PIN_ASP1_TRANSMITTER_A
#define CS8409_CS42L42_SPK_PIN_NID		CS8409_PIN_ASP2_TRANSMITTER_A
#define CS8409_CS42L42_AMIC_PIN_NID		CS8409_PIN_ASP1_RECEIVER_A
#define CS8409_CS42L42_DMIC_PIN_NID		CS8409_PIN_DMIC1_IN
#define CS8409_CS42L42_DMIC_ADC_PIN_NID		CS8409_PIN_DMIC1

/* Dolphin */

#define DOLPHIN_C0_I2C_ADDR			(0x48 << 1)
#define DOLPHIN_C1_I2C_ADDR			(0x49 << 1)
#define DOLPHIN_HP_PIN_NID			CS8409_PIN_ASP1_TRANSMITTER_A
#define DOLPHIN_LO_PIN_NID			CS8409_PIN_ASP1_TRANSMITTER_B
#define DOLPHIN_AMIC_PIN_NID			CS8409_PIN_ASP1_RECEIVER_A

#define DOLPHIN_C0_INT				GENMASK(4, 4)
#define DOLPHIN_C1_INT				GENMASK(0, 0)
#define DOLPHIN_C0_RESET			GENMASK(5, 5)
#define DOLPHIN_C1_RESET			GENMASK(1, 1)
#define DOLPHIN_WAKE				(DOLPHIN_C0_INT | DOLPHIN_C1_INT)

enum {
	CS8409_BULLSEYE,
	CS8409_WARLOCK,
	CS8409_WARLOCK_MLK,
	CS8409_WARLOCK_MLK_DUAL_MIC,
	CS8409_CYBORG,
	CS8409_FIXUPS,
	CS8409_DOLPHIN,
	CS8409_DOLPHIN_FIXUPS,
	CS8409_ODIN,
};

enum {
	CS8409_CODEC0,
	CS8409_CODEC1
};

enum {
	CS42L42_VOL_ADC,
	CS42L42_VOL_DAC,
};

#define CS42L42_ADC_VOL_OFFSET			(CS42L42_VOL_ADC)
#define CS42L42_DAC_CH0_VOL_OFFSET		(CS42L42_VOL_DAC)
#define CS42L42_DAC_CH1_VOL_OFFSET		(CS42L42_VOL_DAC + 1)

struct cs8409_i2c_param {
	unsigned int addr;
	unsigned int value;
	unsigned int delay;
};

struct cs8409_cir_param {
	unsigned int nid;
	unsigned int cir;
	unsigned int coeff;
};


#ifdef APPLE_CODECS
struct unsol_item {
	struct list_head list;
	unsigned int idx;
	unsigned int res;
};
struct hda_cvt_setup_apple {
	hda_nid_t nid;
	u8 stream_tag;
	u8 channel_id;
	u16 format_id;
	unsigned char active;   /* cvt is currently used */
	unsigned char dirty;    /* setups should be cleared */
};
#endif

struct sub_codec {
	struct hda_codec *codec;
	unsigned int addr;
	unsigned int reset_gpio;
	unsigned int irq_mask;
	const struct cs8409_i2c_param *init_seq;
	unsigned int init_seq_num;

	unsigned int hp_jack_in:1;
	unsigned int mic_jack_in:1;
#ifdef APPLE_CODECS
	unsigned int linein_jack_in:1;
#endif
	unsigned int suspended:1;
	unsigned int paged:1;
	unsigned int last_page;
	unsigned int hsbias_hiz;
	unsigned int full_scale_vol:1;
	unsigned int no_type_dect:1;

	s8 vol[CS42L42_VOLUMES];
};

struct cs8409_spec {
	struct hda_gen_spec gen;
	struct hda_codec *codec;

	struct sub_codec *scodecs[CS8409_MAX_CODECS];
	unsigned int num_scodecs;

	unsigned int gpio_mask;
	unsigned int gpio_dir;
	unsigned int gpio_data;

	int speaker_pdn_gpio;

	struct mutex i2c_mux;
	unsigned int i2c_clck_enabled;
	unsigned int dev_addr;
	struct delayed_work i2c_clk_work;

	unsigned int playback_started:1;
	unsigned int capture_started:1;
	unsigned int init_done:1;
	unsigned int build_ctrl_done:1;

#ifdef APPLE_CODECS

	// additional data for Apple 8409 system

	unsigned int spdif_detect:1;
	unsigned int spdif_present:1;
	unsigned int sense_b:1;
	hda_nid_t vendor_nid;

	/* digital beep */
	hda_nid_t beep_nid;

	/* for MBP SPDIF control */
	int (*spdif_sw_put)(struct snd_kcontrol *kcontrol,
			    struct snd_ctl_elem_value *ucontrol);

	// so it appears we have "concurrency" in the linux HDA code
	// in that if unsolicited responses occur which perform extensive verbs
	// the hda verbs are intermixed with eg extensive start playback verbs
	// on OSX we appear to have blocks of verbs during which unsolicited responses
	// are logged but the unsolicited verbs occur after the verb block
	// this flag is used to flag such verb blocks and the list will store the
	// responses
	// we use a pre-allocated list - if we have more than 10 outstanding unsols
	// we will drop
	// not clear if mutexes would be the way to go
	int block_unsol;
	struct list_head unsol_list;
	struct unsol_item unsol_items_prealloc[10];
	int unsol_items_prealloc_used[10];

	// add in specific nids for the intmike and linein as they seem to swap
	// between macbook pros (14,3) and imacs (18,3)
	int intmike_nid;
	int linein_nid;
	int intmike_adc_nid;
	int linein_amp_nid;

	// the following flag bits also need swapping
	int reg9_intmike_dmic_mo;
	int reg9_linein_dmic_mo;
	int reg82_intmike_dmic_scl;
	int reg82_linein_dmic_scl;


	// add explicit stream format store entries as per hda_codec using a local definition
	// of hda_cvt_setup (which is local to hda_codec.c)
	// also use explicit nid versions
	// (except that means either need explicit functions for each nid or have to lookup
	//  nid each time want to use in a generic function with nid argument)
	struct hda_cvt_setup_apple nid_0x02;
	struct hda_cvt_setup_apple nid_0x03;
	struct hda_cvt_setup_apple nid_0x0a;
	struct hda_cvt_setup_apple nid_0x22;
	struct hda_cvt_setup_apple nid_0x23;
	struct hda_cvt_setup_apple nid_0x1a;


	// new item to deal with jack presence as Apple (and now Dell) seems to have barfed
	// the HDA spec by using a separate headphone chip
	int jack_present;

	// save the type of headphone connected
	int headset_type;

	// if headphone has mike or not
	int have_mike;

	// if headphone has buttons or not
	int have_buttons;

        // current stream channel count
        int stream_channels;

	// set when playing for plug/unplug events while playing
	int playing;

	// set when capturing for plug/unplug events while capturing
	int capturing;

	// changing coding - OSX sets up the format on plugin
	// then does some minimal setup when start play
	// initial coding delayed any format setup till actually play
	// this works for no mike but not for mike - we need to initialize
	// the mike on plugin
	// this flag will be set when we have done the format setup
	// so know if need to do it on play or not
	// now need 2 flags - one for play and one for capture
	int headset_play_format_setup_needed;
	int headset_capture_format_setup_needed;

	int headset_presetup_done;


	int use_data;


	// this is new item for dealing with headset plugins
	// so can distinguish which phase we are in if have multiple interrupts
	// not really used now have analyzed interrupts properly
	int headset_phase;

	// another dirty hack item to manage the different headset enable codes
	int headset_enable;

	int play_init;
	int capture_init;


	// new item to limit times we redo unmute/play
	struct timespec64 last_play_time;
	// record the first play time - we have a problem there
	// some initial plays that I dont understand - so skip any setup
	// till sometime after the first play
	struct timespec64 first_play_time;


#endif


	/* verb exec op override */
	int (*exec_verb)(struct hdac_device *dev, unsigned int cmd, unsigned int flags,
			 unsigned int *res);
};

extern const struct snd_kcontrol_new cs42l42_dac_volume_mixer;
extern const struct snd_kcontrol_new cs42l42_adc_volume_mixer;

int cs42l42_volume_info(struct snd_kcontrol *kctrl, struct snd_ctl_elem_info *uinfo);
int cs42l42_volume_get(struct snd_kcontrol *kctrl, struct snd_ctl_elem_value *uctrl);
int cs42l42_volume_put(struct snd_kcontrol *kctrl, struct snd_ctl_elem_value *uctrl);

extern const struct hda_pcm_stream cs42l42_48k_pcm_analog_playback;
extern const struct hda_pcm_stream cs42l42_48k_pcm_analog_capture;
extern const struct hda_quirk cs8409_fixup_tbl[];
extern const struct hda_model_fixup cs8409_models[];
extern const struct hda_fixup cs8409_fixups[];
extern const struct hda_verb cs8409_cs42l42_init_verbs[];
extern const struct cs8409_cir_param cs8409_cs42l42_hw_cfg[];
extern const struct cs8409_cir_param cs8409_cs42l42_bullseye_atn[];
extern struct sub_codec cs8409_cs42l42_codec;

extern const struct hda_verb dolphin_init_verbs[];
extern const struct cs8409_cir_param dolphin_hw_cfg[];
extern struct sub_codec dolphin_cs42l42_0;
extern struct sub_codec dolphin_cs42l42_1;

void cs8409_cs42l42_fixups(struct hda_codec *codec, const struct hda_fixup *fix, int action);
void dolphin_fixups(struct hda_codec *codec, const struct hda_fixup *fix, int action);

#endif
