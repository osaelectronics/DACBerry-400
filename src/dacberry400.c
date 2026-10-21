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

static int db400_pre_event(struct snd_soc_dapm_widget *w,
			   struct snd_kcontrol *kcontrol, int event)
{
	if (event == SND_SOC_DAPM_PRE_PMD && db400_dac_unmuted) {
		msleep(DIV_ROUND_UP(2 * 127 * 1000, db400_rate) + 1);
		db400_dac_unmuted = false;
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
err:
	return ret;
}

static int snd_rpi_dacberry400_set_bias_level(struct snd_soc_card *card,
	struct snd_soc_dapm_context *dapm, enum snd_soc_bias_level level)
{
	struct snd_soc_pcm_runtime *rtd;
	struct snd_soc_dai *codec_dai;
	struct snd_soc_component *component;
	struct dacberry_priv *aic3x;

	rtd = snd_soc_get_pcm_runtime(card, &card->dai_link[0]);
	codec_dai = snd_soc_rtd_to_codec(rtd, 0);
	component = codec_dai->component;
	aic3x = snd_soc_component_get_drvdata(component);
	if (dapm->dev != codec_dai->dev)
		return 0;

	switch (level) {
	case SND_SOC_BIAS_PREPARE:
		if (dapm->bias_level != SND_SOC_BIAS_STANDBY)
			break;
		/* UNMUTE ADC/DAC */
		snd_soc_component_write(component, LINE1R_2_RADC_CTRL, 0x04);
		snd_soc_component_write(component, LINE1L_2_LADC_CTRL, 0x04);
		snd_soc_component_write(component, LADC_VOL, 0x00);
		snd_soc_component_write(component, RADC_VOL, 0x00);
		pr_info("%s: unmute ADC/DAC\n", __func__);
		break;

	case SND_SOC_BIAS_STANDBY:
		if (dapm->bias_level != SND_SOC_BIAS_PREPARE)
			break;
		/* MUTE ADC/DAC */
		snd_soc_component_write(component, LDAC_VOL, 0x80);
		snd_soc_component_write(component, RDAC_VOL, 0x80);
		snd_soc_component_write(component, LADC_VOL, 0x80);
		snd_soc_component_write(component, RADC_VOL, 0x80);
		snd_soc_component_write(component, LINE1R_2_RADC_CTRL, 0x00);
		snd_soc_component_write(component, LINE1L_2_LADC_CTRL, 0x00);
		pr_info("%s: mute ADC/DAC\n", __func__);
		break;
	default:
		break;
	}

	return 0;
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

static int snd_rpi_dacberry400_prepare(struct snd_pcm_substream *substream)
{
	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		db400_dac_unmuted = true;
	return 0;
}

static void db400_wait_dac_off(struct snd_soc_component *component)
{
	struct i2c_client *client = i2c_verify_client(component->dev);
	int i, v = 0;

	if (!client)
		return;
	for (i = 0; i < 50; i++) {
		v = i2c_smbus_read_byte_data(client, 94);	/* module power status */
		if (v < 0 || !(v & 0xc0))			/* D7/D6: L/R DAC */
			return;
		usleep_range(1000, 1500);
	}
	dev_info(component->dev, "DAC power-down not completed (reg94=0x%02x)\n", v);
}

static int snd_rpi_dacberry400_hw_free(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_dai *codec_dai = snd_soc_rtd_to_codec(rtd, 0);

	/* last active stream: DAPM already powered the DAC down (with BCLK) */
	if (snd_soc_dai_active(codec_dai) == 1) {
		db400_wait_dac_off(codec_dai->component);
		snd_soc_component_update_bits(codec_dai->component,
					      AIC3X_PLL_PROGA_REG, PLL_ENABLE, 0);
	}
	return 0;
}

static const struct snd_soc_ops snd_rpi_dacberry400_ops = {
	.prepare = snd_rpi_dacberry400_prepare,
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

static int snd_rpi_dacberry400_late_probe(struct snd_soc_card *card)
{
	struct snd_soc_pcm_runtime *rtd =
		snd_soc_get_pcm_runtime(card, &card->dai_link[0]);
	struct snd_soc_component *component =
		snd_soc_rtd_to_codec(rtd, 0)->component;

	static const char * const hp_widgets[] = {
		"Left HP Out", "Right HP Out", "Left HP Com", "Right HP Com",
	};
	struct snd_soc_dapm_context *dapm = snd_soc_component_get_dapm(component);
	int i;

	if (keep_bias)
		dapm->idle_bias_off = 0;

	if (always_on)
		for (i = 0; i < ARRAY_SIZE(hp_widgets); i++)
			snd_soc_dapm_force_enable_pin(dapm, hp_widgets[i]);

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
	.set_bias_level		= snd_rpi_dacberry400_set_bias_level,
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

static void snd_rpi_dacberry400_remove(struct platform_device *pdev)
{
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
};

module_platform_driver(snd_rpi_dacberry400_driver);

MODULE_AUTHOR("Ashish Vara");
MODULE_DESCRIPTION("Dacberry400 sound card driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:dacberry400");
MODULE_SOFTDEP("pre: snd-soc-tlv320aic3x");
