/*
 * ASoC Driver for Dacberry400 soundcard
 * Author:
 *      Ashish Vara<ashishhvara@gmail.com>
 *      Copyright 2022
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */

#include <linux/module.h>
#include <linux/gpio/consumer.h>
#include <linux/platform_device.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/jack.h>
#include <linux/i2c.h>
#include <linux/acpi.h>
#include <linux/slab.h>
#include "tlv320aic3x.h"
#include <linux/delay.h>
#include <linux/regmap.h>
#include <linux/workqueue.h>

/*
 * Step 1 (pops only):
 *  - keep_bias: keep the codec in STANDBY while idle. aic3x lets DAPM take it
 *    to BIAS_OFF ~5 s after every close, which soft-resets the codec with the
 *    outputs biased (pop) and replays the whole register cache on the next
 *    open (pop again).
 *  - pop_delay/pop_ramp: HP driver power-on delay and common-mode ramp
 *    (reg 42), left at 0 us / 0 ms by the codec driver.
 */
static bool keep_bias = true;
module_param(keep_bias, bool, 0444);
MODULE_PARM_DESC(keep_bias, "Keep the codec biased while idle (no reset on close). Default: Y");

/*
 * Step 4a: keep the HP drivers (and HPxCOM, the capless virtual ground) on
 * while idle, so they are not ramped up/down around every stream.
 */
static bool always_on = true;
module_param(always_on, bool, 0444);
MODULE_PARM_DESC(always_on, "Keep HP output drivers powered while idle. Default: Y");

/* Step 6: diagnostic switches, each one undoes one of steps 4b/4c/5 */
static bool gate_pll = true;
module_param(gate_pll, bool, 0644);
MODULE_PARM_DESC(gate_pll, "Gate the codec PLL when the last stream closes (step 4b). Default: Y");

static bool recover = true;
module_param(recover, bool, 0644);
MODULE_PARM_DESC(recover, "Soft-reset the codec if a DAC is stuck after close (step 4c). Default: Y");

static bool route_ramp = true;
module_param(route_ramp, bool, 0644);
MODULE_PARM_DESC(route_ramp, "Attenuate the DAC->HP path around DAC power changes (step 5). Default: Y");

/*
 * Step 9: order the power-up of the capless output at driver load. HPxCOM
 * (constant VCM) is the virtual ground of the jack sleeve; if it and
 * HPL/HPR do not reach VCM together the headphone sees a step (pop).
 */
static int hp_seq;
module_param(hp_seq, int, 0444);
MODULE_PARM_DESC(hp_seq, "HP power-up order at load: 0=all together, 1=HPxCOM (sleeve) first, 2=HPL/HPR first. Default: 0");

static int hp_seq_delay = 1000;
module_param(hp_seq_delay, int, 0444);
MODULE_PARM_DESC(hp_seq_delay, "Delay between the two power-up phases in ms. Default: 1000");

static int pop_cm = 1;
module_param(pop_cm, int, 0444);
MODULE_PARM_DESC(pop_cm, "Reg 42 bit 1 (powered-down output voltage option): -1=leave, 0 or 1. Default: 1 (softer boot pop on the DBR400C)");

static int pop_delay = -1;
module_param(pop_delay, int, 0444);
MODULE_PARM_DESC(pop_delay, "HP driver power-on delay: 0=0us 1=10us 2=100us 3=1ms 4=10ms 5=50ms 6=100ms 7=200ms 8=400ms 9=800ms 10=2s 11=4s. Default: 9 (800 ms, boot only) with always_on, else 6");

static int pop_ramp = 3;
module_param(pop_ramp, int, 0444);
MODULE_PARM_DESC(pop_ramp, "HP driver ramp-up step: 0=0ms 1=1ms 2=2ms 3=4ms. Default: 3");

static const struct snd_kcontrol_new dacberry400_controls[] = {
	SOC_DAPM_PIN_SWITCH("MIC Jack"),
	SOC_DAPM_PIN_SWITCH("Line In"),
	SOC_DAPM_PIN_SWITCH("Line Out"),
	SOC_DAPM_PIN_SWITCH("Headphone Jack"),
};

/*
 * Step 4b: the codec PLL runs from BCLK and the Pi stops BCLK when the PCM
 * is closed. The DAC needs a clock to finish its power-down (datasheet: keep
 * the clock until the DAC power-down flag is set), otherwise it stays half
 * powered (reg 94) and its noise reaches the always-on HP drivers. So:
 * ignore_pmdown_time (DAC off at stream stop, BCLK still running), wait for
 * the soft-mute ramp before DAPM powers anything down, poll reg 94, and only
 * then gate the PLL.
 */
static unsigned int db400_rate = 48000;
static bool db400_dac_unmuted;

/*
 * Step 5: the DAC itself steps its output when it is powered up or down,
 * and the HP drivers (always on) reproduce that step as a pop. Keep the
 * DAC -> HP mixer path attenuated (-78 dB) whenever the DAC changes state
 * or is off, and ramp it back to the user's level once the DAC is running.
 * Registers 47/64: D7 route on, D6-D0 attenuation in ~0.5 dB steps.
 */
#define DB400_ROUTE_ATT_MAX	0x76
static struct snd_soc_component *db400_comp;
static unsigned int db400_route_att;	/* user's attenuation, 0 = 0 dB */
static bool db400_route_muted;

static void db400_route_set(unsigned int att)
{
	snd_soc_component_update_bits(db400_comp, DACL1_2_HPLOUT_VOL, 0x7f, att);
	snd_soc_component_update_bits(db400_comp, DACR1_2_HPROUT_VOL, 0x7f, att);
}

static void db400_route_mute(void)
{
	unsigned int cur;

	if (!route_ramp || !db400_comp || db400_route_muted)
		return;
	cur = snd_soc_component_read(db400_comp, DACL1_2_HPLOUT_VOL) & 0x7f;
	if (cur < DB400_ROUTE_ATT_MAX)
		db400_route_att = cur;		/* remember the user's level */
	db400_route_set(DB400_ROUTE_ATT_MAX);
	db400_route_muted = true;
}

static void db400_route_ramp_up(void)
{
	int att;

	if (!db400_comp || !db400_route_muted)
		return;
	for (att = DB400_ROUTE_ATT_MAX; att > (int)db400_route_att; att -= 4) {
		db400_route_set(att);
		usleep_range(1000, 1500);
	}
	db400_route_set(db400_route_att);
	db400_route_muted = false;
}

/*
 * On the Pi 5 the I2S block only starts BCLK at TRIGGER_START, so the DAC
 * really powers up only then (DAPM switched it on in prepare). Ramp the
 * route up from a delayed work scheduled at trigger time, once the DAC has
 * had a clock for a while.
 */
static void db400_ramp_work_fn(struct work_struct *work)
{
	if (db400_dac_unmuted)
		db400_route_ramp_up();
}
static DECLARE_DELAYED_WORK(db400_ramp_work, db400_ramp_work_fn);

/*
 * Step 7: the DAC widgets are kept disconnected in DAPM and only connected
 * once a playback stream has really started (TRIGGER_START), i.e. with BCLK
 * running. A PCM that is opened/prepared and closed without being started
 * (WirePlumber probing at boot / session start) therefore never powers the
 * DAC, so it cannot get stuck and the step 4c reset (a pop) is not needed.
 */
static const char * const db400_dac_pins[] = { "Left DAC", "Right DAC" };
/* step 8: same for the ADCs, WirePlumber probes the capture side too */
static const char * const db400_adc_pins[] = { "Left ADC", "Right ADC" };
static bool db400_dac_was_on;

static void db400_pins_connect(const char * const *pins, bool on)
{
	struct snd_soc_dapm_context *dapm;
	int i;

	if (!db400_comp)
		return;
	dapm = snd_soc_component_get_dapm(db400_comp);
	for (i = 0; i < 2; i++) {
		if (on)
			snd_soc_dapm_enable_pin(dapm, pins[i]);
		else
			snd_soc_dapm_disable_pin(dapm, pins[i]);
	}
	if (on)
		snd_soc_dapm_sync(dapm);
}

static void db400_dac_connect(bool on)
{
	db400_pins_connect(db400_dac_pins, on);
}

static void db400_dac_on_work_fn(struct work_struct *work)
{
	db400_dac_was_on = true;
	db400_dac_connect(true);	/* DAPM powers the DAC now, BCLK runs */
	schedule_delayed_work(&db400_ramp_work, msecs_to_jiffies(30));
}
static DECLARE_WORK(db400_dac_on_work, db400_dac_on_work_fn);

static void db400_adc_on_work_fn(struct work_struct *work)
{
	db400_pins_connect(db400_adc_pins, true);
}
static DECLARE_WORK(db400_adc_on_work, db400_adc_on_work_fn);

static int db400_pre_event(struct snd_soc_dapm_widget *w,
			   struct snd_kcontrol *kcontrol, int event)
{
	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		if (db400_dac_unmuted)		/* a playback stream is starting */
			db400_route_mute();
		break;
	case SND_SOC_DAPM_PRE_PMD:
		cancel_delayed_work_sync(&db400_ramp_work);
		if (db400_dac_unmuted) {
			msleep(DIV_ROUND_UP(2 * 127 * 1000, db400_rate) + 1);
			db400_dac_unmuted = false;
			db400_route_mute();	/* before DAPM powers the DAC down */
		}
		break;
	}
	return 0;
}


static const struct snd_soc_dapm_widget dacberry400_widgets[] = {
	SND_SOC_DAPM_PRE("DB400 DAC Mute Wait", db400_pre_event),
	SND_SOC_DAPM_HP("Headphone Jack", NULL),
	SND_SOC_DAPM_MIC("MIC Jack", NULL),
	SND_SOC_DAPM_LINE("Line In", NULL),
	SND_SOC_DAPM_LINE("Line Out", NULL),
};

static const struct snd_soc_dapm_route dacberry400_audio_map[] = {
	{"Headphone Jack", NULL, "HPLOUT"},
	{"Headphone Jack", NULL, "HPROUT"},

	{"LINE1L", NULL, "Line In"},
	{"LINE1R", NULL, "Line In"},

	{"Line Out", NULL, "LLOUT"},
	{"Line Out", NULL, "RLOUT"},

	{"MIC3L", NULL, "MIC Jack"},
	{"MIC3R", NULL, "MIC Jack"},
};

static int snd_rpi_dacberry400_init(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_dai *codec_dai = snd_soc_rtd_to_codec(rtd, 0);
	struct snd_soc_component *component = codec_dai->component;
	int ret;

	ret = snd_soc_dai_set_sysclk(codec_dai, 2, 12000000,
					SND_SOC_CLOCK_OUT);

	if (ret && ret != -ENOTSUPP)
		goto err;

	if (pop_delay < 0)
		pop_delay = always_on ? 9 : 6;
	snd_soc_component_update_bits(component, HPOUT_POP_REDUCTION, 0xfc,
				      (clamp(pop_delay, 0, 11) << 4) |
				      (clamp(pop_ramp, 0, 3) << 2));
	if (pop_cm == 0 || pop_cm == 1)
		snd_soc_component_update_bits(component, HPOUT_POP_REDUCTION,
					      0x02, pop_cm << 1);

	/*
	 * Step 3b (HPxCOM, capless headphone output): HPLCOM (pin 20) and
	 * HPRCOM (pin 22) are tied together and drive the jack sleeve. TI's
	 * capless configuration: HPLCOM = constant VCM (virtual ground for the
	 * sleeve), HPRCOM = external feedback (senses the sleeve). The old code
	 * switched HPLCOM to "single-ended" (left channel on the sleeve ->
	 * crosstalk) on every stream and back to differential in STANDBY.
	 */
	snd_soc_component_update_bits(component, HPLCOM_CFG, 0x30, 0x10);
	snd_soc_component_write(component, HPRCOM_CFG, 0x20);
	snd_soc_component_write(component, DACL1_2_HPLOUT_VOL, 0x80);
	snd_soc_component_write(component, DACR1_2_HPROUT_VOL, 0x80);
	db400_comp = component;
	db400_route_att = 0;		/* 0 dB, as before */
	db400_route_mute();		/* DAC is off until the first stream */
err:
	return ret;
}

static int snd_rpi_dacberry400_hw_params(struct snd_pcm_substream *substream,
					   struct snd_pcm_hw_params *params)
{
	int ret = 0;
	u8 data;
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *codec_dai = snd_soc_rtd_to_codec(rtd, 0);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	struct snd_soc_component *component = codec_dai->component;
	int fsref = (params_rate(params) % 11025 == 0) ? 44100 : 48000;
	int channels = params_channels(params);
	int width = 32;
	u8 clock = 0;

	data = (LDAC2LCH | RDAC2RCH);
	data |= (fsref == 44100) ? FSREF_44100 : FSREF_48000;
	if (params_rate(params) >= 64000)
		data |= DUAL_RATE_MODE;
	ret = snd_soc_component_write(component, 0x7, data);
	width = params_width(params);

	clock = snd_soc_component_read(component, 2);

	ret = snd_soc_dai_set_bclk_ratio(cpu_dai, channels*width);
	if (ret)
		return ret;

	db400_rate = params_rate(params);

	/*
	 * Step 2 (clock): the codec PLL is fed from BCLK (clk_id 2), but init()
	 * declared a fixed 12 MHz while the real BCLK is rate*channels*width
	 * (1.5..3.1 MHz), so aic3x programmed the PLL for the wrong input.
	 * Tell the codec the real BCLK; its hw_params (called after this one)
	 * then computes a correct P/J/D/R.
	 */
	ret = snd_soc_dai_set_sysclk(codec_dai, 2,
				     params_rate(params) * channels * width,
				     SND_SOC_CLOCK_IN);
	if (ret == -ENOTSUPP)
		ret = 0;

	return ret;
}

static int snd_rpi_dacberry400_trigger(struct snd_pcm_substream *substream,
				       int cmd)
{
	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
			schedule_work(&db400_dac_on_work);
		else
			schedule_work(&db400_adc_on_work);
		break;
	}
	return 0;
}

static int snd_rpi_dacberry400_prepare(struct snd_pcm_substream *substream)
{
	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		db400_dac_unmuted = true;
	return 0;
}

static bool db400_wait_dac_off(struct snd_soc_component *component)
{
	struct i2c_client *client = i2c_verify_client(component->dev);
	int i, v;

	if (!client)
		return true;
	for (i = 0; i < 50; i++) {
		v = i2c_smbus_read_byte_data(client, 94);	/* module power status */
		if (v < 0 || !(v & 0xc0))			/* D7/D6: L/R DAC */
			return true;
		usleep_range(1000, 1500);
	}
	return false;
}

/*
 * Step 4c: a PCM that was prepared but closed without ever being started
 * (WirePlumber probing the card at boot) powered the DAC up while the Pi 5
 * I2S block was not yet outputting BCLK (it only does from TRIGGER_START).
 * Such a DAC can neither finish powering up nor down, and only a reset
 * clears it: soft-reset the codec and replay the register cache, exactly
 * what a driver reload did (which always left the outputs quiet).
 */
static void db400_recover_codec(struct snd_soc_component *component)
{
	struct regmap *map = component->regmap;
	int ret;

	if (!map)
		return;

	dev_info(component->dev, "DAC stuck without BCLK, resetting codec\n");

	regcache_cache_bypass(map, true);
	regmap_write(map, AIC3X_RESET, SOFT_RESET);
	regcache_cache_bypass(map, false);

	regcache_mark_dirty(map);
	ret = regcache_sync(map);
	if (ret)
		dev_err(component->dev, "register restore failed: %d\n", ret);
}

static int snd_rpi_dacberry400_hw_free(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_dai *codec_dai = snd_soc_rtd_to_codec(rtd, 0);
	bool check_dac = false;

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		cancel_work_sync(&db400_dac_on_work);
		db400_dac_connect(false);
		check_dac = db400_dac_was_on;	/* only if we powered the DAC */
		db400_dac_was_on = false;
	} else {
		cancel_work_sync(&db400_adc_on_work);
		db400_pins_connect(db400_adc_pins, false);
	}

	/* DAPM already powered the DAC down (with BCLK) at stream stop */
	/* reg 94 bit 6 also follows the capture path: only judge with no capture */
	if (check_dac &&
	    !snd_soc_dai_stream_active(codec_dai, SNDRV_PCM_STREAM_CAPTURE)) {
		if (!db400_wait_dac_off(codec_dai->component)) {
			if (recover)
				db400_recover_codec(codec_dai->component);
			else
				dev_info(codec_dai->component->dev,
					 "DAC power-down not completed (no recovery)\n");
		}
	}

	/* last active stream: gate the PLL while BCLK still runs */
	if (snd_soc_dai_active(codec_dai) == 1) {
		if (gate_pll)
			snd_soc_component_update_bits(codec_dai->component,
						      AIC3X_PLL_PROGA_REG, PLL_ENABLE, 0);
	}
	return 0;
}

static const struct snd_soc_ops snd_rpi_dacberry400_ops = {
	.prepare = snd_rpi_dacberry400_prepare,
	.trigger = snd_rpi_dacberry400_trigger,
	.hw_free = snd_rpi_dacberry400_hw_free,
	.hw_params = snd_rpi_dacberry400_hw_params,
};


SND_SOC_DAILINK_DEFS(rpi_dacberry400,
	DAILINK_COMP_ARRAY(COMP_CPU("bcm2835-i2s.0")),
	DAILINK_COMP_ARRAY(COMP_CODEC("tlv320aic3x.1-0018", "tlv320aic3x-hifi")),
	DAILINK_COMP_ARRAY(COMP_PLATFORM("bcm2835-i2s.0")));

static struct snd_soc_dai_link snd_rpi_dacberry400_dai[] = {
{
	.dai_fmt		= SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF |
					SND_SOC_DAIFMT_CBS_CFS,
	.init			= snd_rpi_dacberry400_init,
	.ops			= &snd_rpi_dacberry400_ops,
	.symmetric_rate		= 1,
	.ignore_pmdown_time	= 1,
	SND_SOC_DAILINK_REG(rpi_dacberry400),
},
};

static const char * const db400_hp_out[] = { "Left HP Out", "Right HP Out" };
static const char * const db400_hp_com[] = { "Left HP Com", "Right HP Com" };

static void db400_force_pair(const char * const *pins)
{
	struct snd_soc_dapm_context *dapm;
	int i;

	if (!db400_comp)
		return;
	dapm = snd_soc_component_get_dapm(db400_comp);
	for (i = 0; i < 2; i++)
		snd_soc_dapm_force_enable_pin(dapm, pins[i]);
}

static void db400_hp_phase2_fn(struct work_struct *work)
{
	db400_force_pair(hp_seq == 2 ? db400_hp_com : db400_hp_out);
	snd_soc_dapm_sync(snd_soc_component_get_dapm(db400_comp));
}
static DECLARE_DELAYED_WORK(db400_hp_phase2, db400_hp_phase2_fn);

static int snd_rpi_dacberry400_late_probe(struct snd_soc_card *card)
{
	struct snd_soc_pcm_runtime *rtd =
		snd_soc_get_pcm_runtime(card, &card->dai_link[0]);
	struct snd_soc_component *component =
		snd_soc_rtd_to_codec(rtd, 0)->component;

	struct snd_soc_dapm_context *dapm = snd_soc_component_get_dapm(component);

	if (keep_bias)
		dapm->idle_bias_off = 0;

	db400_dac_connect(false);	/* step 7: no DAC until a stream starts */
	db400_pins_connect(db400_adc_pins, false);	/* step 8: same for ADCs */

	if (always_on) {
		if (hp_seq == 1 || hp_seq == 2) {
			/* phase 1 now (card core syncs after late_probe), phase 2 later */
			db400_force_pair(hp_seq == 1 ? db400_hp_com : db400_hp_out);
			schedule_delayed_work(&db400_hp_phase2,
					      msecs_to_jiffies(clamp(hp_seq_delay, 0, 10000)));
		} else {
			db400_force_pair(db400_hp_com);
			db400_force_pair(db400_hp_out);
		}
	}

	return 0;
}

static struct snd_soc_card snd_rpi_dacberry400 = {
	.owner			= THIS_MODULE,
	.dai_link		= snd_rpi_dacberry400_dai,
	.num_links		= ARRAY_SIZE(snd_rpi_dacberry400_dai),
	.controls		= dacberry400_controls,
	.num_controls		= ARRAY_SIZE(dacberry400_controls),
	.dapm_widgets		= dacberry400_widgets,
	.num_dapm_widgets	= ARRAY_SIZE(dacberry400_widgets),
	.dapm_routes		= dacberry400_audio_map,
	.num_dapm_routes	= ARRAY_SIZE(dacberry400_audio_map),
	.late_probe		= snd_rpi_dacberry400_late_probe,
};

static int snd_rpi_dacberry400_probe(struct platform_device *pdev)
{
	int ret = 0;

	snd_rpi_dacberry400.dev = &pdev->dev;

	if (pdev->dev.of_node) {
		struct device_node *i2s_node;
		struct snd_soc_card *card = &snd_rpi_dacberry400;
		struct snd_soc_dai_link *dai = &snd_rpi_dacberry400_dai[0];

		i2s_node = of_parse_phandle(pdev->dev.of_node,
					    "i2s-controller", 0);
		if (i2s_node) {
			dai->cpus->dai_name = NULL;
			dai->cpus->of_node = i2s_node;
			dai->platforms->name = NULL;
			dai->platforms->of_node = i2s_node;
			of_node_put(i2s_node);
		}

		if (of_property_read_string(pdev->dev.of_node, "card_name",
					    &card->name))
			card->name = "tlvaudioCODEC";

		if (of_property_read_string(pdev->dev.of_node, "dai_name",
					    &dai->name))
			dai->name = "tlvaudio CODEC";

	}

	ret = snd_soc_register_card(&snd_rpi_dacberry400);
	if (ret) {
		if (ret != -EPROBE_DEFER)
			dev_err(&pdev->dev,
				"snd_soc_register_card() failed: %d\n", ret);
		return ret;
	}

	return 0;
}

/*
 * Step 10: orderly power-down on reboot/poweroff. Otherwise the HP drivers
 * and the capless VCM collapse together with the rails (pop). Mute the
 * drivers, then power them down while the codec is still supplied.
 */
static void db400_quiesce(void)
{
	struct snd_soc_dapm_context *dapm;
	int i;

	if (!db400_comp)
		return;
	cancel_delayed_work_sync(&db400_hp_phase2);
	dapm = snd_soc_component_get_dapm(db400_comp);

	db400_route_mute();
	snd_soc_component_update_bits(db400_comp, HPLOUT_CTRL, 0x08, 0);	/* mute */
	snd_soc_component_update_bits(db400_comp, HPROUT_CTRL, 0x08, 0);
	msleep(20);

	for (i = 0; i < 2; i++) {
		snd_soc_dapm_disable_pin(dapm, db400_hp_out[i]);
		snd_soc_dapm_disable_pin(dapm, db400_hp_com[i]);
	}
	snd_soc_dapm_sync(dapm);
	msleep(300);
}

static void snd_rpi_dacberry400_shutdown(struct platform_device *pdev)
{
	db400_quiesce();
}

static void snd_rpi_dacberry400_remove(struct platform_device *pdev)
{
	cancel_delayed_work_sync(&db400_hp_phase2);
	cancel_work_sync(&db400_dac_on_work);
	cancel_work_sync(&db400_adc_on_work);
	cancel_delayed_work_sync(&db400_ramp_work);
	snd_soc_unregister_card(&snd_rpi_dacberry400);
}

static const struct of_device_id dacberry400_match_id[] = {
	{ .compatible = "osaelectronics,dacberry400",},
	{},
};
MODULE_DEVICE_TABLE(of, dacberry400_match_id);

static struct platform_driver snd_rpi_dacberry400_driver = {
	.driver = {
		.name = "snd-rpi-dacberry400",
		.owner = THIS_MODULE,
		.of_match_table = dacberry400_match_id,
	},
	.probe = snd_rpi_dacberry400_probe,
	.remove = snd_rpi_dacberry400_remove,
	.shutdown = snd_rpi_dacberry400_shutdown,
};

module_platform_driver(snd_rpi_dacberry400_driver);

MODULE_AUTHOR("Ashish Vara");
MODULE_DESCRIPTION("Dacberry400 sound card driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:dacberry400");
MODULE_SOFTDEP("pre: snd-soc-tlv320aic3x");
