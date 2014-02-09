/*
 * sound/ppc/gcn-ai.c
 *
 * Nintendo GameCube/Wii Audio Interface (AI) driver
 * Copyright (C) 2004-2009 The GameCube Linux Team
 * Copyright (C) 2007,2008,2009 Albert Herranz
 *
 * Based on work from mist, kirin, groepaz, Steve_-, isobel and others.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/of_platform.h>
#include <linux/interrupt.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <sound/core.h>
#include <sound/pcm.h>
#define SNDRV_GET_ID
#include <sound/initval.h>


#define DRV_MODULE_NAME  "gcn-ai"
#define DRV_DESCRIPTION  "Nintendo GameCube/Wii Audio Interface (AI) driver"
#define DRV_AUTHOR       "Michael Steil, " \
			 "(kirin), " \
			 "(groepaz), " \
			 "Steven Looman, " \
			 "Albert Herranz"

static char ai_driver_version[] = "1.0i";

#define drv_printk(level, format, arg...) \
	 printk(level DRV_MODULE_NAME ": " format , ## arg)


/*
 * Hardware.
 *
 */

/*
 * DSP registers.
 */
#define AI_DSP_CSR		0x0a	/* 16 bits */
#define  AI_CSR_RES		(1<<0)
#define  AI_CSR_PIINT		(1<<1)
#define  AI_CSR_HALT		(1<<2)
#define  AI_CSR_AIDINT		(1<<3)
#define  AI_CSR_AIDINTMASK	(1<<4)
#define  AI_CSR_ARINT		(1<<5)
#define  AI_CSR_ARINTMASK	(1<<6)
#define  AI_CSR_DSPINT		(1<<7)
#define  AI_CSR_DSPINTMASK	(1<<8)
#define  AI_CSR_DSPDMA		(1<<9)
#define  AI_CSR_RESETXXX	(1<<11)

#define AI_DSP_DMA_ADDRH	0x30	/* 16 bits */

#define AI_DSP_DMA_ADDRL	0x32	/* 16 bits */

#define AI_DSP_DMA_CTLLEN	0x36	/* 16 bits */
#define  AI_CTLLEN_PLAY		(1<<15)

#define AI_DSP_DMA_LEFT		0x3a	/* 16 bits */

/*
 * AI registers.
 */
#define AI_AICR			0x00	/* 32 bits */
#define  AI_AICR_RATE       (1<<6)


/*
 * Sound chip.
 */
struct snd_gcn {
	struct snd_card			*card;
	struct snd_pcm			*pcm;
	struct snd_pcm_substream	*playback_substream;
	struct snd_pcm_substream	*capture_substream;

	int		start_play;
	int		stop_play;

	dma_addr_t	dma_addr;
	size_t		period_size;
	int		nperiods;
	int		cur_period;

	void __iomem	*dsp_base;
	void __iomem	*ai_base;
	unsigned int	irq;

	struct device	*dev;
};


/*
 * Hardware functions.
 *
 */

static void ai_dsp_load_sample(void __iomem *dsp_base,
			       void *addr, size_t size)
{
	u32 daddr = (unsigned long)addr;

	out_be16(dsp_base + AI_DSP_DMA_ADDRH, daddr >> 16);
	out_be16(dsp_base + AI_DSP_DMA_ADDRL, daddr & 0xffff);
	out_be16(dsp_base + AI_DSP_DMA_CTLLEN,
		 (in_be16(dsp_base + AI_DSP_DMA_CTLLEN) & AI_CTLLEN_PLAY) |
		 size >> 5);
}

static void ai_dsp_start_sample(void __iomem *dsp_base)
{
	out_be16(dsp_base + AI_DSP_DMA_CTLLEN,
		 in_be16(dsp_base + AI_DSP_DMA_CTLLEN) | AI_CTLLEN_PLAY);
}

static void ai_dsp_stop_sample(void __iomem *dsp_base)
{
	out_be16(dsp_base + AI_DSP_DMA_CTLLEN,
		 in_be16(dsp_base + AI_DSP_DMA_CTLLEN) & ~AI_CTLLEN_PLAY);
}

static int ai_dsp_get_remaining_byte_count(void __iomem *dsp_base)
{
	return in_be16(dsp_base + AI_DSP_DMA_LEFT) << 5;
}

static void ai_enable_interrupts(void __iomem *dsp_base)
{
	unsigned long flags;

	/* enable AI DMA and DSP interrupts */
	local_irq_save(flags);
	out_be16(dsp_base + AI_DSP_CSR,
		 in_be16(dsp_base + AI_DSP_CSR) |
		 AI_CSR_AIDINTMASK | AI_CSR_PIINT);
	local_irq_restore(flags);
}

static void ai_disable_interrupts(void __iomem *dsp_base)
{
	unsigned long flags;

	/* disable AI interrupts */
	local_irq_save(flags);
	out_be16(dsp_base + AI_DSP_CSR,
		 in_be16(dsp_base + AI_DSP_CSR) & ~AI_CSR_AIDINTMASK);
	local_irq_restore(flags);
}

static void ai_set_rate(void __iomem *ai_base, int fortyeight)
{
	/* set rate to 48KHz or 32KHz */
	if (fortyeight)
		out_be32(ai_base + AI_AICR,
			 in_be32(ai_base + AI_AICR) & ~AI_AICR_RATE);
	else
		out_be32(ai_base + AI_AICR,
			 in_be32(ai_base + AI_AICR) | AI_AICR_RATE);
}


static int index = SNDRV_DEFAULT_IDX1;	/* index 0-MAX */
static char *id = SNDRV_DEFAULT_STR1;	/* ID for this card */

static struct snd_gcn *gcn_audio;

static struct snd_pcm_hardware snd_gcn_playback = {
	.info = (SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_INTERLEAVED |
		 SNDRV_PCM_INFO_BLOCK_TRANSFER | SNDRV_PCM_INFO_MMAP_VALID),
	.formats = SNDRV_PCM_FMTBIT_S16_BE,
	.rates = SNDRV_PCM_RATE_32000 | SNDRV_PCM_RATE_48000,
	.rate_min = 32000,
	.rate_max = 48000,
	.channels_min = 2,
	.channels_max = 2,
	.buffer_bytes_max = 32768,
	.period_bytes_min = 32,
	.period_bytes_max = 32768,
	.periods_min = 1,
	.periods_max = 1024,
};

static int snd_gcn_open(struct snd_pcm_substream *substream)
{
	struct snd_gcn *chip = snd_pcm_substream_chip(substream);
	struct snd_pcm_runtime *runtime = substream->runtime;

	chip->playback_substream = substream;
	runtime->hw = snd_gcn_playback;

	/* align to 32 bytes */
	snd_pcm_hw_constraint_step(runtime, 0, SNDRV_PCM_HW_PARAM_BUFFER_BYTES,
				   32);
	snd_pcm_hw_constraint_step(runtime, 0, SNDRV_PCM_HW_PARAM_PERIOD_BYTES,
				   32);

	return 0;
}

static int snd_gcn_close(struct snd_pcm_substream *substream)
{
	struct snd_gcn *chip = snd_pcm_substream_chip(substream);

	chip->playback_substream = NULL;
	return 0;
}

static int snd_gcn_hw_params(struct snd_pcm_substream *substream,
				  struct snd_pcm_hw_params *hw_params)
{
	return snd_pcm_lib_malloc_pages(substream,
					params_buffer_bytes(hw_params));
}

static int snd_gcn_hw_free(struct snd_pcm_substream *substream)
{
	return snd_pcm_lib_free_pages(substream);
}

static int snd_gcn_prepare(struct snd_pcm_substream *substream)
{
	struct snd_gcn *chip = snd_pcm_substream_chip(substream);
	struct snd_pcm_runtime *runtime = substream->runtime;

	/* set requested sample rate */
	switch (runtime->rate) {
	case 32000:
		ai_set_rate(chip->ai_base, 0);
		break;
	case 48000:
		ai_set_rate(chip->ai_base, 1);
		break;
	default:
		drv_printk(KERN_ERR, "unsupported rate %i\n", runtime->rate);
		return -EINVAL;
	}

	return 0;
}

static int snd_gcn_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct snd_gcn *chip = snd_pcm_substream_chip(substream);
	struct snd_pcm_runtime *runtime = substream->runtime;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		/* do something to start the PCM engine */
		if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
			chip->period_size = snd_pcm_lib_period_bytes(substream);
			chip->nperiods = snd_pcm_lib_buffer_bytes(substream) /
					 chip->period_size;
			chip->cur_period = 0;
			chip->stop_play = 0;
			chip->start_play = 1;

			chip->dma_addr = dma_map_single(chip->dev,
							runtime->dma_area,
							chip->period_size,
							DMA_TO_DEVICE);
			ai_dsp_load_sample(chip->dsp_base, runtime->dma_area,
					   chip->period_size);
			ai_dsp_start_sample(chip->dsp_base);
		}
		break;
	case SNDRV_PCM_TRIGGER_STOP:
		chip->stop_play = 1;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static snd_pcm_uframes_t snd_gcn_pointer(struct snd_pcm_substream *substream)
{
	struct snd_gcn *chip = snd_pcm_substream_chip(substream);
	struct snd_pcm_runtime *runtime = substream->runtime;
	int left, bytes;

	left = ai_dsp_get_remaining_byte_count(chip->dsp_base);
	bytes = chip->period_size * (chip->cur_period + 1);

	return bytes_to_frames(runtime, bytes - left);
}

static irqreturn_t snd_gcn_interrupt(int irq, void *dev)
{
	struct snd_gcn *chip = dev;
	void *addr;
	unsigned long flags;
	u16 csr;

	/*
	 * This is a shared interrupt. Do nothing if it ain't ours.
	 */
	csr = in_be16(chip->dsp_base + AI_DSP_CSR);
	if (!(csr & AI_CSR_AIDINT))
		return IRQ_NONE;

	if (chip->start_play) {
		chip->start_play = 0;
	} else {
		/* stop current sample */
		ai_dsp_stop_sample(chip->dsp_base);
		dma_unmap_single(chip->dev, chip->dma_addr, chip->period_size,
				 DMA_TO_DEVICE);

		/* load next sample if we are not stopping */
		if (!chip->stop_play) {
			if (chip->cur_period < (chip->nperiods - 1))
				chip->cur_period++;
			else
				chip->cur_period = 0;

			addr = chip->playback_substream->runtime->dma_area
				   + (chip->cur_period * chip->period_size);
			chip->dma_addr = dma_map_single(chip->dev,
							addr,
							chip->period_size,
							DMA_TO_DEVICE);
			ai_dsp_load_sample(chip->dsp_base, addr,
					   chip->period_size);
			ai_dsp_start_sample(chip->dsp_base);

			snd_pcm_period_elapsed(chip->playback_substream);
		}
	}
	/*
	 * Ack the AI DMA interrupt, going through lengths to only ack
	 * the audio part.
	 */
	local_irq_save(flags);
	csr = in_be16(chip->dsp_base + AI_DSP_CSR);
	csr &= ~(AI_CSR_PIINT | AI_CSR_ARINT | AI_CSR_DSPINT);
	out_be16(chip->dsp_base + AI_DSP_CSR, csr);
	local_irq_restore(flags);

	return IRQ_HANDLED;
}


static struct snd_pcm_ops snd_gcn_playback_ops = {
	.open = snd_gcn_open,
	.close = snd_gcn_close,
	.ioctl = snd_pcm_lib_ioctl,
	.hw_params = snd_gcn_hw_params,
	.hw_free = snd_gcn_hw_free,
	.prepare = snd_gcn_prepare,
	.trigger = snd_gcn_trigger,
	.pointer = snd_gcn_pointer,
};

static int __devinit snd_gcn_new_pcm(struct snd_gcn *chip)
{
	struct snd_pcm *pcm;
	int retval;

	retval = snd_pcm_new(chip->card, chip->card->shortname, 0, 1, 0, &pcm);
	if (retval < 0)
		return retval;

	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK,
			&snd_gcn_playback_ops);

	/* preallocate 64k buffer */
	snd_pcm_lib_preallocate_pages_for_all(pcm, SNDRV_DMA_TYPE_CONTINUOUS,
					      snd_dma_continuous_data
					      (GFP_KERNEL), 64 * 1024,
					      64 * 1024);

	pcm->info_flags = 0;
	pcm->private_data = chip;
	strcpy(pcm->name, chip->card->shortname);

	chip->pcm = pcm;

	return 0;
}

static void ai_shutdown(struct snd_gcn *chip)
{
	ai_dsp_stop_sample(chip->dsp_base);
	ai_disable_interrupts(chip->dsp_base);
}

static int __devinit ai_init(struct snd_gcn *chip,
		   struct resource *dsp, struct resource *ai,
		   unsigned int irq)
{
	struct snd_card *card;
	int retval;

	chip->dsp_base = ioremap(dsp->start, dsp->end - dsp->start + 1);
	chip->ai_base = ioremap(ai->start, ai->end - ai->start + 1);
	chip->irq = irq;

	chip->stop_play = 1;
	card = chip->card;

	strcpy(card->driver, DRV_MODULE_NAME);
	strcpy(card->shortname, card->driver);
	sprintf(card->longname, "Nintendo GameCube Audio Interface");

	/* PCM */
	retval = snd_gcn_new_pcm(chip);
	if (retval < 0)
		goto err_new_pcm;

	retval = request_irq(chip->irq, snd_gcn_interrupt,
			     IRQF_DISABLED | IRQF_SHARED,
			     card->shortname, chip);
	if (retval) {
		drv_printk(KERN_ERR, "unable to request IRQ %d\n", chip->irq);
		goto err_request_irq;
	}
	ai_enable_interrupts(chip->dsp_base);

	gcn_audio = chip;
	retval = snd_card_register(card);
	if (retval) {
		drv_printk(KERN_ERR, "failed to register card\n");
		goto err_card_register;
	}

	return 0;

err_card_register:
	ai_disable_interrupts(chip->dsp_base);
	free_irq(chip->irq, chip);
err_request_irq:
err_new_pcm:
	iounmap(chip->dsp_base);
	iounmap(chip->ai_base);
	return retval;
}

static void ai_exit(struct snd_gcn *chip)
{
	ai_dsp_stop_sample(chip->dsp_base);
	ai_disable_interrupts(chip->dsp_base);

	free_irq(chip->irq, chip);
	iounmap(chip->dsp_base);
	iounmap(chip->ai_base);
}


/*
 * Device interfaces.
 *
 */

static int ai_do_shutdown(struct device *dev)
{
	struct snd_gcn *chip;

	chip = dev_get_drvdata(dev);
	if (chip) {
		ai_shutdown(chip);
		return 0;
	}
	return -ENODEV;
}

static int __devinit ai_do_probe(struct device *dev,
		       struct resource *dsp, struct resource *ai,
		       unsigned int irq)
{
	struct snd_card *card;
	struct snd_gcn *chip;
	int retval;

	retval = snd_card_create(index, id, THIS_MODULE, sizeof(struct snd_gcn), &card);
	if (retval < 0) {
		drv_printk(KERN_ERR, "failed to allocate card\n");
		return -ENOMEM;
	}
	chip = (struct snd_gcn *)card->private_data;
	memset(chip, 0, sizeof(*chip));
	chip->card = card;
	dev_set_drvdata(dev, chip);
	chip->dev = dev;

	retval = ai_init(chip, dsp, ai, irq);
	if (retval)
		snd_card_free(card);

	return retval;
}

static int ai_do_remove(struct device *dev)
{
	struct snd_gcn *chip;

	chip = dev_get_drvdata(dev);
	if (chip) {
		ai_exit(chip);
		dev_set_drvdata(dev, NULL);
		snd_card_free(chip->card);
		return 0;
	}
	return -ENODEV;
}

/*
 * OF Platform device interfaces.
 *
 */

static int __init ai_of_probe(struct of_device *odev,
			      const struct of_device_id *match)
{
	struct resource dsp, ai;
	int retval;

	retval = of_address_to_resource(odev->node, 0, &dsp);
	if (retval) {
		drv_printk(KERN_ERR, "no dsp io memory range found\n");
		return -ENODEV;
	}
	retval = of_address_to_resource(odev->node, 1, &ai);
	if (retval) {
		drv_printk(KERN_ERR, "no ai io memory range found\n");
		return -ENODEV;
	}

	return ai_do_probe(&odev->dev,
			   &dsp, &ai, irq_of_parse_and_map(odev->node, 0));
}

static int __exit ai_of_remove(struct of_device *odev)
{
	return ai_do_remove(&odev->dev);
}

static int ai_of_shutdown(struct of_device *odev)
{
	return ai_do_shutdown(&odev->dev);
}


static struct of_device_id ai_of_match[] = {
	{ .compatible = "nintendo,flipper-audio" },
	{ .compatible = "nintendo,hollywood-audio" },
	{ },
};

MODULE_DEVICE_TABLE(of, ai_of_match);

static struct of_platform_driver ai_of_driver = {
	.owner = THIS_MODULE,
	.name = DRV_MODULE_NAME,
	.match_table = ai_of_match,
	.probe = ai_of_probe,
	.remove = ai_of_remove,
	.shutdown = ai_of_shutdown,
};

/*
 * Module interfaces.
 *
 */

static int __init ai_init_module(void)
{
	drv_printk(KERN_INFO, "%s - version %s\n", DRV_DESCRIPTION,
		   ai_driver_version);

	return of_register_platform_driver(&ai_of_driver);
}

static void __exit ai_exit_module(void)
{
	of_unregister_platform_driver(&ai_of_driver);
}

module_init(ai_init_module);
module_exit(ai_exit_module);

MODULE_DESCRIPTION(DRV_DESCRIPTION);
MODULE_AUTHOR(DRV_AUTHOR);
MODULE_LICENSE("GPL");

