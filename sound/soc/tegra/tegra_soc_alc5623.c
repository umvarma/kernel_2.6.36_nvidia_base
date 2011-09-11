/*
 * tegra_soc_alc5623.c  --  SoC audio for tegra (glue logic)
 *
 * (c) 2010-2011 Nvidia Graphics Pvt. Ltd.
 *  http://www.nvidia.com
 * (C) 2011 Eduardo José Tagle <ejtagle@tutopia.com>
 *
 * Copyright 2007 Wolfson Microelectronics PLC.
 * Author: Graeme Gregory
 *         graeme.gregory@wolfsonmicro.com or linux@wolfsonmicro.com
 *
 *  This program is free software; you can redistribute  it and/or modify it
 *  under  the terms of  the GNU General  Public License as published by the
 *  Free Software Foundation;  either version 2 of the  License, or (at your
 *  option) any later version.
 *
 */
 
/* #define DEBUG */
 
#include "tegra_soc.h"
#include "../codecs/alc5623.h"
#include <sound/soc-dapm.h>
#include <linux/regulator/consumer.h>

#include <linux/types.h>
#include <sound/jack.h>
#include <linux/switch.h>
#include <mach/gpio.h>
#include <mach/audio.h>
#include <linux/delay.h>

#define DRV_NAME "tegra-snd-alc5623"
#define TEGRA_GPIO_PW2     178

static struct snd_soc_jack* tegra_jack = NULL;

static struct platform_device *tegra_snd_device;
static struct regulator* alc5623_reg;

extern struct snd_soc_dai tegra_i2s_dai[];
extern struct snd_soc_dai tegra_spdif_dai;
extern struct snd_soc_dai tegra_generic_codec_dai[];
extern struct snd_soc_platform tegra_soc_platform;

/* exported by the ALC5623 codec */
extern struct snd_soc_dai alc5623_dai;
extern struct snd_soc_codec_device soc_codec_dev_alc5623;

/* mclk required for each sampling frequency */
static const struct {
	unsigned int mclk;
	unsigned short srate;
} clocktab[] = {
        /* 8k */
        { 8192000,  8000},
        {12288000,  8000},
        {24576000,  8000},

        /* 11.025k */
        {11289600, 11025},
        {16934400, 11025},
        {22579200, 11025},

        /* 16k */
        {12288000, 16000},
        {16384000, 16000},
        {24576000, 16000},

        /* 22.05k */
        {11289600, 22050},
        {16934400, 22050},
        {22579200, 22050},

        /* 32k */
        {12288000, 32000},
        {16384000, 32000},
        {24576000, 32000},

        /* 44.1k */
        {11289600, 44100},
        {22579200, 44100},

        /* 48k */
        {12288000, 48000},
        {24576000, 48000},
};


/* --------- Digital audio interfase ------------ */

static int tegra_hifi_hw_params(struct snd_pcm_substream *substream,
					struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *codec_dai 	= rtd->dai->codec_dai;
	struct snd_soc_dai *cpu_dai 	= rtd->dai->cpu_dai;
	struct snd_soc_codec *codec = codec_dai->codec;	
	int dai_flag = 0, sys_clk, codec_is_master;
        unsigned int srate, value;
        int i, err;
        enum dac_dap_data_format data_fmt;

        /* Get the requested sampling rate */
        srate = params_rate(params);

	/* I2S <-> DAC <-> DAS <-> DAP <-> CODEC
	   -If DAP is master, codec will be slave */
	codec_is_master = !tegra_das_is_port_master(tegra_audio_codec_type_hifi);
	if (codec_is_master)
                dai_flag |= SND_SOC_DAIFMT_CBS_CFS;
        else
                dai_flag |= SND_SOC_DAIFMT_CBM_CFM;

        data_fmt = tegra_das_get_codec_data_fmt(tegra_audio_codec_type_hifi);

	/* We are supporting DSP and I2s format for now */
	if (data_fmt & dac_dap_data_format_i2s)
		dai_flag |= SND_SOC_DAIFMT_I2S;
	else
		dai_flag |= SND_SOC_DAIFMT_DSP_A;
	
	pr_debug("%s(): format: 0x%08x\n", __FUNCTION__,params_format(params));

	err = snd_soc_dai_set_fmt(codec_dai, dai_flag);
	if (err < 0) {
		pr_err("codec_dai fmt not set \n");
		return err;
	}

	/* Set the CPU dai format. This will also set the clock rate in master mode */
	err = snd_soc_dai_set_fmt(cpu_dai, dai_flag);
	if (err < 0) {
		pr_err("cpu_dai fmt not set \n");
		return err;
	}

        sys_clk = tegra_das_get_mclk_rate();
        err = snd_soc_dai_set_sysclk(codec_dai, 0, sys_clk, SND_SOC_CLOCK_IN);
        if (err < 0) {
                pr_err("codec_dai clock not set\n");
                return err;
        }

	if (codec_is_master) {
		pr_debug("%s(): codec in master mode\n",__FUNCTION__);
		
		/* If using port as slave (=codec as master), then we can use the
		   codec PLL to get the other sampling rates */
		
		/* Try each one until success */
		for (i = 0; i < ARRAY_SIZE(clocktab); i++) {
		
			if (clocktab[i].srate != srate) 
				continue;
				
			if (snd_soc_dai_set_pll(codec_dai, 0, 0, sys_clk, clocktab[i].mclk) >= 0) {
				/* Codec PLL is synthetizing this new clock */
				sys_clk = clocktab[i].mclk;
				break;
			}
		}
		
		if (i >= ARRAY_SIZE(clocktab)) {
			pr_err("%s(): unable to set required MCLK for SYSCLK of %d, sampling rate: %d\n",__FUNCTION__,sys_clk,srate);
			return -EINVAL;
		}
		
	} else {
		pr_debug("%s(): codec in slave mode\n",__FUNCTION__);

		/* Disable codec PLL */
		err = snd_soc_dai_set_pll(codec_dai, 0, 0, sys_clk, sys_clk);
		if (err < 0) {
			pr_err("%s(): unable to disable codec PLL\n",__FUNCTION__);
			return err;
		}
		
		/* Check this sampling rate can be achieved with this sysclk */
		for (i = 0; i < ARRAY_SIZE(clocktab); i++) {
		
			if (clocktab[i].srate != srate) 
				continue;
				
			if (sys_clk == clocktab[i].mclk)
				break;
		}
		
		if (i >= ARRAY_SIZE(clocktab)) {
			pr_err("%s(): unable to get required %d hz sampling rate of %d hz SYSCLK\n",__FUNCTION__,srate,sys_clk);
			return -EINVAL;
		}
	}

	/* Set CODEC sysclk */
	err = snd_soc_dai_set_sysclk(codec_dai, 0, sys_clk, SND_SOC_CLOCK_IN);
	if (err < 0) {
		pr_err("codec_dai clock not set\n");
		return err;
	}
	
	return 0;
}

static int tegra_voice_hw_params(struct snd_pcm_substream *substream,
					struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *codec_dai 	= rtd->dai->codec_dai;
	struct snd_soc_dai *cpu_dai 	= rtd->dai->cpu_dai;
	int dai_flag = 0, sys_clk;
	int err;

	/* Get DAS dataformat and master flag */
	int codec_is_master = !tegra_das_is_port_master(tegra_audio_codec_type_bluetooth);
	enum dac_dap_data_format data_fmt = tegra_das_get_codec_data_fmt(tegra_audio_codec_type_bluetooth);

	/* We are supporting DSP and I2s format for now */
	if (data_fmt & dac_dap_data_format_dsp)
		dai_flag |= SND_SOC_DAIFMT_DSP_A;
	else
		dai_flag |= SND_SOC_DAIFMT_I2S;

	if (codec_is_master)
		dai_flag |= SND_SOC_DAIFMT_CBM_CFM; /* codec is master */
	else
		dai_flag |= SND_SOC_DAIFMT_CBS_CFS;


	pr_debug("%s(): format: 0x%08x\n", __FUNCTION__,params_format(params));

	/* Set the CPU dai format. This will also set the clock rate in master mode */
	err = snd_soc_dai_set_fmt(cpu_dai, dai_flag);
	if (err < 0) {
		pr_err("cpu_dai fmt not set \n");
		return err;
	}

	/* Bluetooth Codec is always slave here */
	err = snd_soc_dai_set_fmt(codec_dai, dai_flag);
	if (err < 0) {
		pr_err("codec_dai fmt not set \n");
		return err;
	}
	
	/* Get system clock */
	sys_clk = tegra_das_get_mclk_rate();

	/* Set CPU sysclock as the same - in Tegra, seems to be a NOP */
	err = snd_soc_dai_set_sysclk(cpu_dai, 0, sys_clk, SND_SOC_CLOCK_IN);
	if (err < 0) {
		pr_err("cpu_dai clock not set\n");
		return err;
	}
	
	/* Set CODEC sysclk */
	err = snd_soc_dai_set_sysclk(codec_dai, 0, sys_clk, SND_SOC_CLOCK_IN);
	if (err < 0) {
		pr_err("cpu_dai clock not set\n");
		return err;
	}
	
	return 0;
}

static int tegra_spdif_hw_params(struct snd_pcm_substream *substream,
					struct snd_pcm_hw_params *params)
{
	pr_debug("%s(): format: 0x%08x\n", __FUNCTION__,params_format(params));
	return 0;
}

static int tegra_codec_startup(struct snd_pcm_substream *substream)
{
	tegra_das_power_mode(true);

	return 0;
}

static void tegra_codec_shutdown(struct snd_pcm_substream *substream)
{
	tegra_das_power_mode(false);
}

static int tegra_soc_suspend_pre(struct platform_device *pdev, pm_message_t state)
{
	return 0;
}

static int tegra_soc_suspend_post(struct platform_device *pdev, pm_message_t state)
{
	tegra_das_disable_mclk();

	return 0;
}

static int tegra_soc_resume_pre(struct platform_device *pdev)
{
	tegra_das_enable_mclk();

	return 0;
}

static int tegra_soc_resume_post(struct platform_device *pdev)
{
	return 0;
}

static struct snd_soc_ops tegra_hifi_ops = {
	.hw_params = tegra_hifi_hw_params,
	.startup = tegra_codec_startup,
	.shutdown = tegra_codec_shutdown,
};

static struct snd_soc_ops tegra_voice_ops = {
	.hw_params = tegra_voice_hw_params,
	.startup = tegra_codec_startup,
	.shutdown = tegra_codec_shutdown,
};

static struct snd_soc_ops tegra_spdif_ops = {
	.hw_params = tegra_spdif_hw_params,
};

/* ------- Tegra audio routing using DAS -------- */

void tegra_ext_control(struct snd_soc_codec *codec, int new_con)
{
        struct tegra_audio_data *audio_data = codec->socdev->codec_data;

        pr_info("%s: new_con 0x%X #####################\n", __func__, new_con);

        /* Disconnect old codec routes and connect new routes*/
        if (new_con & TEGRA_HEADPHONE)
                snd_soc_dapm_enable_pin(codec, "Headphone Jack");
        else
                snd_soc_dapm_disable_pin(codec, "Headphone Jack");

        if (new_con & (TEGRA_SPK | TEGRA_EAR_SPK))
                snd_soc_dapm_enable_pin(codec, "Internal Speaker");
        else
                snd_soc_dapm_disable_pin(codec, "Internal Speaker");

        if (new_con & TEGRA_INT_MIC)
                snd_soc_dapm_enable_pin(codec, "MIC1");
        else
                snd_soc_dapm_disable_pin(codec, "MIC1");

        audio_data->codec_con = new_con;

        /* signal a DAPM event */
        snd_soc_dapm_sync(codec);

}

#if 0
static void tegra_audio_route(struct adam_audio_priv* ctx,
			      int play_device, int capture_device)
{
	
	int is_bt_sco_mode = (play_device    & ADAM_AUDIO_DEVICE_BLUETOOTH) ||
						 (capture_device & ADAM_AUDIO_DEVICE_BLUETOOTH);
	int is_call_mode   = (play_device    & ADAM_AUDIO_DEVICE_VOICE) ||
						 (capture_device & ADAM_AUDIO_DEVICE_VOICE);

	pr_debug("%s(): is_bt_sco_mode: %d, is_call_mode: %d\n", __FUNCTION__, is_bt_sco_mode, is_call_mode);
	
	if (is_call_mode && is_bt_sco_mode) {
		tegra_das_set_connection(tegra_das_port_con_id_voicecall_with_bt);
	}
	else if (is_call_mode && !is_bt_sco_mode) {
		tegra_das_set_connection(tegra_das_port_con_id_voicecall_no_bt);
	}
	else if (!is_call_mode && is_bt_sco_mode) {
		tegra_das_set_connection(tegra_das_port_con_id_bt_codec);
	}
	else {
		tegra_das_set_connection(tegra_das_port_con_id_hifi);
	}
	ctx->play_device = play_device;
	ctx->capture_device = capture_device;
}

static int tegra_play_route_info(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 1;
	uinfo->value.integer.min = ADAM_AUDIO_DEVICE_NONE;
	uinfo->value.integer.max = ADAM_AUDIO_DEVICE_MAX;
	return 0;
}

static int tegra_play_route_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct adam_audio_priv* ctx = snd_kcontrol_chip(kcontrol);

	ucontrol->value.integer.value[0] = ADAM_AUDIO_DEVICE_NONE;
	if (ctx) {
		ucontrol->value.integer.value[0] = ctx->play_device;
		return 0;
	}
	return -EINVAL;
}

static int tegra_play_route_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct adam_audio_priv* ctx = snd_kcontrol_chip(kcontrol);

	if (ctx) {
		int play_device_new = ucontrol->value.integer.value[0];

		if (ctx->play_device != play_device_new) {
			tegra_audio_route(ctx, play_device_new, ctx->capture_device);
			return 1;
		}
		return 0;
	}
	return -EINVAL;
}

struct snd_kcontrol_new tegra_play_route_control = {
	.access = SNDRV_CTL_ELEM_ACCESS_READWRITE,
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "Pcm Playback Route",
	.private_value = 0xffff,
	.info = tegra_play_route_info,
	.get = tegra_play_route_get,
	.put = tegra_play_route_put
};

static int tegra_capture_route_info(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 1;
	uinfo->value.integer.min = ADAM_AUDIO_DEVICE_NONE;
	uinfo->value.integer.max = ADAM_AUDIO_DEVICE_MAX;
	return 0;
}

static int tegra_capture_route_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct adam_audio_priv* ctx = snd_kcontrol_chip(kcontrol);

	ucontrol->value.integer.value[0] = TEGRA_AUDIO_DEVICE_NONE;
	if (ctx) {
		ucontrol->value.integer.value[0] = ctx->capture_device;
		return 0;
	}
	return -EINVAL;
}

static int tegra_capture_route_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct adam_audio_priv* ctx = snd_kcontrol_chip(kcontrol);

	if (ctx) {
		int capture_device_new = ucontrol->value.integer.value[0];

		if (ctx->capture_device != capture_device_new) {
			tegra_audio_route(ctx,
				ctx->play_device , capture_device_new);
			return 1;
		}
		return 0;
	}
	return -EINVAL;
}

static struct snd_kcontrol_new tegra_capture_route_control = {
	.access = SNDRV_CTL_ELEM_ACCESS_READWRITE,
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "Pcm Capture Route",
	.private_value = 0xffff,
	.info = tegra_capture_route_info,
	.get = tegra_capture_route_get,
	.put = tegra_capture_route_put
};

static int tegra_call_mode_info(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 1;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = 1;
	return 0;
}

static int tegra_call_mode_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct adam_audio_priv* ctx = snd_kcontrol_chip(kcontrol);

	ucontrol->value.integer.value[0] = 0;
	if (ctx) {
		int is_call_mode   = (ctx->play_device    & ADAM_AUDIO_DEVICE_VOICE) ||
							 (ctx->capture_device & ADAM_AUDIO_DEVICE_VOICE);
	
		ucontrol->value.integer.value[0] = is_call_mode ? 1 : 0;
		return 0;
	}
	return -EINVAL;
}

static int tegra_call_mode_put(struct snd_kcontrol *kcontrol,
				   struct snd_ctl_elem_value *ucontrol)
{
	struct adam_audio_priv* ctx = snd_kcontrol_chip(kcontrol);

	if (ctx) {
		int is_call_mode   = (ctx->play_device    & ADAM_AUDIO_DEVICE_VOICE) ||
							 (ctx->capture_device & ADAM_AUDIO_DEVICE_VOICE);
	
		int is_call_mode_new = ucontrol->value.integer.value[0];

		if (is_call_mode != is_call_mode_new) {
			if (is_call_mode_new) {
				ctx->play_device 	|= ADAM_AUDIO_DEVICE_VOICE;
				ctx->capture_device |= ADAM_AUDIO_DEVICE_VOICE;
				ctx->play_device 	&= ~ADAM_AUDIO_DEVICE_HIFI;
				ctx->capture_device &= ~ADAM_AUDIO_DEVICE_HIFI;
			} else {
				ctx->play_device 	&= ~ADAM_AUDIO_DEVICE_VOICE;
				ctx->capture_device &= ~ADAM_AUDIO_DEVICE_VOICE;
				ctx->play_device 	|= ADAM_AUDIO_DEVICE_HIFI;
				ctx->capture_device |= ADAM_AUDIO_DEVICE_HIFI;
			}
			tegra_audio_route(ctx,
				ctx->play_device,
				ctx->capture_device);
			return 1;
		}
		return 0;
	}
	return -EINVAL;
}

static struct snd_kcontrol_new tegra_call_mode_control = {
	.access = SNDRV_CTL_ELEM_ACCESS_READWRITE,
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "Call Mode Switch",
	.private_value = 0xffff,
	.info = tegra_call_mode_info,
	.get = tegra_call_mode_get,
	.put = tegra_call_mode_put
};

#endif

#ifndef ADAM_MANUAL_CONTROL_OF_OUTPUTDEVICE

/* ------- Headphone jack autodetection  -------- */
static struct snd_soc_jack_pin tegra_jack_pins[] = {
	/* Disable speaker when headphone is plugged in */
	{
		.pin = "Internal Speaker",
		.mask = SND_JACK_HEADPHONE,
		.invert = 0, /* Enable pin when status is reported */
	},
	/* Enable headphone when status is reported */
	{
		.pin = "Headphone Jack",
		.mask = SND_JACK_HEADPHONE,
		.invert = 1, /* Enable pin when status is not reported */
	},
};

static struct snd_soc_jack_gpio tegra_jack_gpios[] = {
	{
		.name = "headphone detect",
		.report = SND_JACK_HEADPHONE,
		.debounce_time = 150,
		.gpio = TEGRA_GPIO_PW2,
	}
};

#endif

/*tegra machine dapm widgets */
static const struct snd_soc_dapm_widget tegra_dapm_widgets[] = {
	SND_SOC_DAPM_SPK("Internal Speaker", NULL),
	SND_SOC_DAPM_HP("Headphone Jack", NULL),
	SND_SOC_DAPM_MIC("Internal Mic", NULL),
};

/* Tegra machine audio map (connections to the codec pins) */
static const struct snd_soc_dapm_route audio_map[] = {
	{"Headphone Jack", NULL, "HPR"},
        {"Headphone Jack", NULL, "HPL"},
        {"Internal Speaker", NULL, "AUXOUTL"},
        {"Internal Speaker", NULL, "AUXOUTR"},
        {"Mic Bias1", NULL, "Internal Mic"},
        {"MIC1", NULL, "Mic Bias1"},
};

#ifdef ADAM_MANUAL_CONTROL_OF_OUTPUTDEVICE
static const struct snd_kcontrol_new tegra_controls[] = {
	SOC_DAPM_PIN_SWITCH("Internal Speaker"),
	SOC_DAPM_PIN_SWITCH("Headphone Jack"),
	SOC_DAPM_PIN_SWITCH("Internal Mic"),
};
#endif

static int tegra_codec_init(struct snd_soc_codec *codec)
{
	struct tegra_audio_data* audio_data = codec->socdev->codec_data;

	int ret = 0;

	if (!audio_data->init_done) {
		
#if 0
		/* Get and enable the DAP clock */
		ctx->dap_mclk = tegra_das_get_dap_mclk();
		if (!ctx->dap_mclk) {
			pr_err("Failed to get dap mclk \n");
			return -ENODEV;
		}
		clk_enable(ctx->dap_mclk);

		/* Store the GPIO used to detect headphone */
		tegra_jack_gpios[0].gpio = ctx->gpio_hp_det;
#else

                ret = tegra_das_open();
                if (ret) {
                        pr_err(" Failed get dap mclk \n");
                        ret = -ENODEV;
                        goto alc5623_init_fail;
                }

                ret = tegra_das_enable_mclk();
                if (ret) {
                        pr_err(" Failed to enable dap mclk \n");
                        ret = -ENODEV;
                        goto alc5623_init_fail;
                }


#endif
		/* Add tegra specific widgets */
		snd_soc_dapm_new_controls(codec, tegra_dapm_widgets,
					ARRAY_SIZE(tegra_dapm_widgets));

#ifdef ADAM_MANUAL_CONTROL_OF_OUTPUTDEVICE
		/* Add specific adam controls */
		ret = snd_soc_add_controls(codec, tegra_controls,
					ARRAY_SIZE(tegra_controls));
		if (ret < 0) {
			pr_err("Failed to register controls\n");
                        goto alc5623_init_fail;
		}
#endif
					
		/* Set up tegra specific audio path audio_map */
		snd_soc_dapm_add_routes(codec, audio_map,
					ARRAY_SIZE(audio_map));


		/* Add jack detection */
                ret = tegra_jack_init(codec);
                if (ret < 0) {
                        pr_err("Failed in jack init \n");
                        goto alc5623_init_fail;
                }

                /* Default to OFF */
                tegra_ext_control(codec, TEGRA_AUDIO_OFF);

                ret = tegra_controls_init(codec);
                if (ret < 0) {
                        pr_err("Failed in controls init \n");
                        goto alc5623_init_fail;
                }
	
#ifndef ADAM_MANUAL_CONTROL_OF_OUTPUTDEVICE
		/* Headphone jack detection */
		if(!tegra_jack) {
                        tegra_jack = kzalloc(sizeof(*tegra_jack), GFP_KERNEL);
                        if (!tegra_jack) {
                                pr_err("failed to allocate tegra-jack\n");
                                ret = -ENOMEM;
                                goto alc5623_init_fail;
                        }

			ret = snd_soc_jack_new(codec->socdev->card, "Headphone Jack", SND_JACK_HEADPHONE,
				 tegra_jack);
			if (ret)
				goto failed;
			 
			ret = snd_soc_jack_add_pins(tegra_jack,
				      ARRAY_SIZE(tegra_jack_pins),
				      tegra_jack_pins);
			if (ret)
				goto failed;
				  
			ret = snd_soc_jack_add_gpios(tegra_jack,
				       ARRAY_SIZE(tegra_jack_gpios),
				       tegra_jack_gpios);
			if (ret)
				goto failed;
		}
#endif
		
		audio_data->codec = codec;
		audio_data->init_done = 1;
	}

	return ret;

failed:
        kfree(tegra_jack);

alc5623_init_fail:
        tegra_das_disable_mclk();
        tegra_das_close();
        tegra_jack = NULL;
        return ret;

}

#define TEGRA_CREATE_SOC_DAI_LINK(xname, xstreamname,   \
                        xcpudai, xcodecdai, xops)       \
{                                                       \
        .name = xname,                                  \
        .stream_name = xstreamname,                     \
        .cpu_dai = xcpudai,                             \
        .codec_dai = xcodecdai,                         \
        .init = tegra_codec_init,                       \
        .ops = xops,                                    \
}


static struct snd_soc_dai_link tegra_soc_dai[] = {
#if defined(CONFIG_ARCH_TEGRA_2x_SOC)
        TEGRA_CREATE_SOC_DAI_LINK("ALC5623", "ALC5623 HiFi",
                &tegra_i2s_dai[0], &alc5623_dai,
                &tegra_hifi_ops),
        TEGRA_CREATE_SOC_DAI_LINK("Tegra-generic", "",
                &tegra_i2s_dai[1], &tegra_generic_codec_dai[0],
                &tegra_voice_ops),
        TEGRA_CREATE_SOC_DAI_LINK("Tegra-spdif", "Tegra Spdif",
                &tegra_spdif_dai, &tegra_generic_codec_dai[1],
                &tegra_spdif_ops),
#endif
};

static struct tegra_audio_data audio_data = {
        .init_done = 0,
        .play_device = TEGRA_AUDIO_DEVICE_NONE,
        .capture_device = TEGRA_AUDIO_DEVICE_NONE,
        .is_call_mode = false,
        .codec_con = TEGRA_AUDIO_OFF,
};


/* The Tegra card definition */
static struct snd_soc_card tegra_snd_soc = {
	.name = "tegra-alc5623",
	.platform 	= &tegra_soc_platform,
	.dai_link 	= tegra_soc_dai,
	.num_links 	= ARRAY_SIZE(tegra_soc_dai),
	.suspend_pre = tegra_soc_suspend_pre,
	.suspend_post = tegra_soc_suspend_post,
	.resume_pre = tegra_soc_resume_pre,
	.resume_post = tegra_soc_resume_post,
};

/* A sound device is composed of a card (in this case, Tegra, and
   a codec, in this case ALC5623 */
static struct snd_soc_device tegra_snd_devdata = {
	.card = &tegra_snd_soc,
	.codec_dev = &soc_codec_dev_alc5623,
	.codec_data = &audio_data,
};


#if 0
static __devinit int tegra_snd_adam_probe(struct platform_device *pdev)
{
	struct adam_audio_platform_data* pdata;
	struct adam_audio_priv *ctx;
	int ret = 0;
	
	/* Get platform data */
	pdata = pdev->dev.platform_data;
	if (!pdata) {
		dev_err(&pdev->dev, "no platform data supplied\n");
		return -EINVAL;
	}

	/* Allocate private context */
	ctx = kzalloc(sizeof(struct adam_audio_priv), GFP_KERNEL);
	if (!ctx) {
		dev_err(&pdev->dev, "Can't allocate tegra_adam\n");
		return -ENOMEM;
	}
	platform_set_drvdata(pdev, ctx); /* Store it as platform data */
	//tegra_snd_devdata.codec_data = ctx;
	
	/* Fill in the GPIO used to detect the headphone */
	ctx->gpio_hp_det = pdata->gpio_hp_det;
	
	/* Reserve space for the soc-audio device */
	tegra_snd_device = platform_device_alloc("soc-audio", -1);
	if (!tegra_snd_device) {
		dev_err(&pdev->dev, "failed to allocate soc-audio \n");
		kfree(ctx);
		return -ENOMEM;
	}

	/* Set soc-audio platform data to our descriptor */
	platform_set_drvdata(tegra_snd_device, &tegra_snd_devdata);
	tegra_snd_devdata.dev = &tegra_snd_device->dev;

	/* Add the device */
	ret = platform_device_add(tegra_snd_device);
	if (ret) {
		dev_err(&pdev->dev, "audio device could not be added \n");
		kfree(ctx);
		goto fail;
	}

	return 0;

fail:
	if (tegra_snd_device) {
		platform_device_put(tegra_snd_device);
		tegra_snd_device = 0;
	}

	return ret;
}
#endif

static int __init tegra_init(void)
{
        int ret = 0;

        tegra_snd_device = platform_device_alloc("soc-audio", -1);
        if (!tegra_snd_device) {
                pr_err("failed to allocate soc-audio \n");
                return ENOMEM;
        }

        platform_set_drvdata(tegra_snd_device, &tegra_snd_devdata);
        tegra_snd_devdata.dev = &tegra_snd_device->dev;
        ret = platform_device_add(tegra_snd_device);
        if (ret) {
                pr_err("audio device could not be added \n");
                goto fail;
        }

#if 0
        alc5623_reg = regulator_get(NULL, "avddio_audio");
        if (IS_ERR(alc5623_reg)) {
                ret = PTR_ERR(alc5623_reg);
                pr_err("unable to get alc5623 regulator\n");
                goto fail;
        }

        ret = regulator_enable(alc5623_reg);
        if (ret) {
                pr_err("alc5623 regulator enable failed\n");
                goto err_put_regulator;
        }
#endif
        return 0;

fail:
        if (tegra_snd_device) {
                platform_device_put(tegra_snd_device);
                tegra_snd_device = 0;
        }

        return ret;

err_put_regulator:
        regulator_put(alc5623_reg);
        return ret;
}


static void __exit tegra_exit(void)
{
        platform_device_unregister(tegra_snd_device);

#if 0
        regulator_disable(alc5623_reg);
        regulator_put(alc5623_reg);
#endif

        if (tegra_jack) {
                kfree(tegra_jack);
                tegra_jack = NULL;
        }
}

module_init(tegra_init);
module_exit(tegra_exit);

MODULE_DESCRIPTION("Adam machine ASoC driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:" DRV_NAME);
