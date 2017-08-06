/*
 * sound/ppc/gcn-mic.c
 *
 * Nintendo Microphone (DOL-022) driver
 * Copyright (C) 2006-2009 The GameCube Linux Team
 * Copyright (C) 2006,2007,2008,2009 Albert Herranz
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 */

#define MIC_DEBUG

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/freezer.h>
#include <linux/proc_fs.h>
#include <linux/exi.h>
#include <linux/slab.h>

#include <sound/core.h>
#include <sound/pcm.h>
#define SNDRV_GET_ID
#include <sound/initval.h>

#define DRV_MODULE_NAME "gcn-mic"
#define DRV_DESCRIPTION "Nintendo Microphone (DOL-022) driver"
#define DRV_AUTHOR      "Albert Herranz"

MODULE_AUTHOR(DRV_AUTHOR);
MODULE_DESCRIPTION(DRV_DESCRIPTION);
MODULE_LICENSE("GPL");

static char mic_driver_version[] = "0.1i";

#define mic_printk(level, format, arg...) \
	printk(level DRV_MODULE_NAME ": " format , ## arg)

#ifdef MIC_DEBUG
#  define DBG(fmt, args...) \
	   printk(KERN_ERR "%s: " fmt, __func__ , ## args)
#else
#  define DBG(fmt, args...)
#endif


#define MIC_EXI_ID		0x0a000000

#define MIC_SLOTA_CHANNEL	0	/* EXI0xxx */
#define MIC_SLOTA_DEVICE	0	/* chip select, EXI0CSB0 */

#define MIC_SLOTB_CHANNEL	1	/* EXI1xxx */
#define MIC_SLOTB_DEVICE	0	/* chip select, EXI1CSB0 */

#define MIC_SPI_CLK_IDX		EXI_CLK_16MHZ


struct mic_device {
	spinlock_t lock;
	unsigned long flags;

	u16 status;
	u16 control;
#define MIC_CTL_RATE_MASK	(0x3<<11)
#define MIC_CTL_RATE_11025	(0x0<<11)
#define MIC_CTL_RATE_22050	(0x1<<11)
#define MIC_CTL_RATE_44100	(0x2<<11)
#define MIC_CTL_PERIOD_MASK	(0x3<<13)
#define MIC_CTL_PERIOD_32	(0x0<<13)
#define MIC_CTL_PERIOD_64	(0x1<<13)
#define MIC_CTL_PERIOD_128	(0x2<<13)
#define MIC_CTL_START_SAMPLING	(1<<15)

	struct task_struct      *io_thread;
	wait_queue_head_t       io_waitq;
	atomic_t		io_pending;

	struct snd_card *card;
	struct snd_pcm *pcm;

	struct snd_pcm_substream *c_substream;
	u8	*c_orig, *c_cur;
	int	c_left;

	int running;

#ifdef CONFIG_PROC_FS
	struct proc_dir_entry           *proc;
#endif /* CONFIG_PROC_FS */

	int refcnt;
	struct exi_device *exi_device;
};


/*
 *
 */
static void mic_hey(struct mic_device *dev)
{
	struct exi_device *exi_device = dev->exi_device;
	u8 cmd = 0xff;

	exi_dev_select(exi_device);
	exi_dev_write(exi_device, &cmd, sizeof(cmd));
	exi_dev_deselect(exi_device);
}

/*
 *
 */
static int mic_get_status(struct mic_device *dev)
{
	struct exi_device *exi_device = dev->exi_device;
	u8 cmd = 0x40;

	exi_dev_select(exi_device);
	exi_dev_write(exi_device, &cmd, sizeof(cmd));
	exi_dev_read(exi_device, &dev->status, sizeof(dev->status));
	exi_dev_deselect(exi_device);

	return dev->status;
}

/*
 *
 */
static void mic_control(struct mic_device *dev)
{
	struct exi_device *exi_device = dev->exi_device;
	u8 cmd[3];

	cmd[0] = 0x80;
	cmd[1] = dev->control >> 8;
	cmd[2] = dev->control & 0xff;

	DBG("control 0x80%02x%02x\n", cmd[1], cmd[2]);

	exi_dev_select(exi_device);
	exi_dev_write(exi_device, cmd, sizeof(cmd));
	exi_dev_deselect(exi_device);

}

/*
 *
 */
static void mic_read_period(struct mic_device *dev, void *buf, size_t len)
{
	struct exi_device *exi_device = dev->exi_device;
	u8 cmd = 0x20;

	exi_dev_select(exi_device);
	exi_dev_write(exi_device, &cmd, sizeof(cmd));
	exi_dev_read(exi_device, buf, len);
	exi_dev_deselect(exi_device);

/*	DBG("mic cmd 0x20\n"); */
}

/*
 *
 */
static void mic_enable_sampling(struct mic_device *dev, int enable)
{
	if (enable)
		dev->control |= MIC_CTL_START_SAMPLING;
	else
		dev->control &= ~MIC_CTL_START_SAMPLING;
}

/*
 *
 */
static int mic_set_sample_rate(struct mic_device *dev, int rate)
{
	u16 control;

	switch (rate) {
	case 11025:
		control = MIC_CTL_RATE_11025;
		break;
	case 22050:
		control = MIC_CTL_RATE_22050;
		break;
	case 44100:
		control = MIC_CTL_RATE_44100;
		break;
	default:
		mic_printk(KERN_ERR, "unsupported rate: %d\n", rate);
		return -EINVAL;
	}
	dev->control &= ~MIC_CTL_RATE_MASK;
	dev->control |= control;
	return 0;
}

/*
 *
 */
static int mic_set_period(struct mic_device *dev, int period_bytes)
{
	u16 control;

	switch (period_bytes) {
	case 32:
		control = MIC_CTL_PERIOD_32;
		break;
	case 64:
		control = MIC_CTL_PERIOD_64;
		break;
	case 128:
		control = MIC_CTL_PERIOD_128;
		break;
	default:
		mic_printk(KERN_ERR, "unsupported period: %d bytes\n",
			   period_bytes);
		return -EINVAL;
	}
	dev->control &= ~MIC_CTL_PERIOD_MASK;
	dev->control |= control;
	return 0;
}

/*
 * /proc support
 *
 */

/*
 *
 */
static int mic_init_proc(struct mic_device *dev)
{
	return 0;
}

/*
 *
 */
static void mic_exit_proc(struct mic_device *dev)
{
}



/*
 * Driver
 *
 */

static int index = SNDRV_DEFAULT_IDX1;
static char *id = SNDRV_DEFAULT_STR1;

static struct snd_pcm_hardware mic_snd_capture = {
#if 0
	.info = (SNDRV_PCM_INFO_MMAP |
		SNDRV_PCM_INFO_INTERLEAVED | SNDRV_PCM_INFO_NONINTERLEAVED |
		SNDRV_PCM_INFO_BLOCK_TRANSFER |
		SNDRV_PCM_INFO_MMAP_VALID),
#endif
	.info = (SNDRV_PCM_INFO_INTERLEAVED | SNDRV_PCM_INFO_NONINTERLEAVED),
	.formats = SNDRV_PCM_FMTBIT_S16_BE,
	.rates = SNDRV_PCM_RATE_11025 | SNDRV_PCM_RATE_22050 |
		SNDRV_PCM_RATE_44100,
	.rate_min = 11025,
	.rate_max = 44100,
	.channels_min = 1,
	.channels_max = 1,
	.buffer_bytes_max = 32768,
	.period_bytes_min = 32,
	.period_bytes_max = 128,
	.periods_min = 1,
	.periods_max = 1024,
};

#if 0
static unsigned int period_bytes[] = { 32, 64, 128 };
static struct snd_pcm_hw_constraint_list constraints_period_bytes = {
	.count = ARRAY_SIZE(period_bytes),
	.list = period_bytes,
	.mask = 0,
};
#endif

/*
 *
 */
static void mic_wakeup_io_thread(struct mic_device *dev)
{
	if (!IS_ERR(dev->io_thread)) {
		atomic_inc(&dev->io_pending);
		wake_up(&dev->io_waitq);
	}
}

/*
 *
 */
static void mic_stop_io_thread(struct mic_device *dev)
{
	if (!IS_ERR(dev->io_thread)) {
		atomic_inc(&dev->io_pending);
		kthread_stop(dev->io_thread);
	}
}

/*
 * Input/Output thread. Receives audio samples from the microphone.
 */
static int mic_io_thread(void *param)
{
	struct mic_device *dev = param;
	struct snd_pcm_substream *substream;
	int period_bytes;
	u16 status;

	set_user_nice(current, -20);
	set_current_state(TASK_RUNNING);

	for (;;) {
		wait_event(dev->io_waitq, atomic_read(&dev->io_pending) > 0);
		atomic_dec(&dev->io_pending);

		if (kthread_should_stop())
			break;

		if (try_to_freeze())
			continue;

		exi_dev_take(dev->exi_device);
		status = mic_get_status(dev);
		if (dev->running) {
			substream = dev->c_substream;

			if (!dev->c_left) {
				dev->c_cur = dev->c_orig;
				dev->c_left =
					snd_pcm_lib_buffer_bytes(substream);
			}

			period_bytes = snd_pcm_lib_period_bytes(substream);
			if (period_bytes > dev->c_left)
				period_bytes = dev->c_left;
			mic_read_period(dev, dev->c_cur, period_bytes);
			dev->c_cur += period_bytes;
			dev->c_left -= period_bytes;

			exi_dev_give(dev->exi_device);
			snd_pcm_period_elapsed(substream);
			exi_dev_take(dev->exi_device);

			if (status & 0x0200) {
				DBG("0x0200\n");
				mic_hey(dev);
				mic_enable_sampling(dev, 1);
				mic_control(dev);
			}
		} else {
			/* mic_enable_sampling(dev, 0); */
			dev->control = 0;
			mic_control(dev);
		}
		exi_dev_give(dev->exi_device);
	}
	return 0;
}

/*
 *
 */
static int mic_event_handler(struct exi_channel *exi_channel,
			     unsigned int event, void *dev0)
{
	struct mic_device *dev = (struct mic_device *)dev0;

	/* exi channel is not taken, no exi operations here please */
	mic_wakeup_io_thread(dev);

	return 0;
}

static int hw_rule_period_bytes_by_rate(struct snd_pcm_hw_params *params,
					struct snd_pcm_hw_rule *rule)
{
	struct snd_interval *period_bytes =
		hw_param_interval(params, SNDRV_PCM_HW_PARAM_PERIOD_BYTES);
	struct snd_interval *rate =
		hw_param_interval(params, SNDRV_PCM_HW_PARAM_RATE);

	DBG("rate: min %d, max %d\n", rate->min, rate->max);

	if (rate->min == rate->max) {
		if (rate->min >= 44100) {
			struct snd_interval t = {
				.min = 128,
				.max = 128,
				.integer = 1,
			};
			return snd_interval_refine(period_bytes, &t);
		} else if (rate->min >= 22050) {
			struct snd_interval t = {
				.min = 32,
				.max = 32,
				.integer = 1,
			};
			return snd_interval_refine(period_bytes, &t);
		} else {
			struct snd_interval t = {
				.min = 32,
				.max = 32,
				.integer = 1,
			};
			return snd_interval_refine(period_bytes, &t);
		}
	}
	return 0;
}

static int mic_snd_pcm_capture_open(struct snd_pcm_substream *substream)
{
	struct mic_device *dev = snd_pcm_substream_chip(substream);
	struct snd_pcm_runtime *runtime = substream->runtime;
	unsigned long flags;
	int retval;

	DBG("enter\n");

	spin_lock_irqsave(&dev->lock, flags);
	dev->running = 0;
	dev->c_substream = substream;
	spin_unlock_irqrestore(&dev->lock, flags);

	runtime->hw = mic_snd_capture;

#if 0
	/* only 32, 64 and 128 */
	retval = snd_pcm_hw_constraint_list(runtime, 0,
					    SNDRV_PCM_HW_PARAM_PERIOD_BYTES,
					    &constraints_period_bytes);
	if (retval < 0)
		return retval;
#endif
	snd_pcm_hw_rule_add(runtime, 0,
			    SNDRV_PCM_HW_PARAM_PERIOD_BYTES,
			    hw_rule_period_bytes_by_rate, 0,
			    SNDRV_PCM_HW_PARAM_RATE, -1);

	/* align to 32 bytes */
	retval = snd_pcm_hw_constraint_step(runtime, 0,
					    SNDRV_PCM_HW_PARAM_BUFFER_BYTES,
					    32);
	return retval;

}

static int mic_snd_pcm_capture_close(struct snd_pcm_substream *substream)
{
	struct mic_device *dev = snd_pcm_substream_chip(substream);
	unsigned long flags;

DBG("enter\n");

	spin_lock_irqsave(&dev->lock, flags);
	dev->running = 0;
	dev->c_substream = NULL;
	spin_unlock_irqrestore(&dev->lock, flags);

	mic_wakeup_io_thread(dev);

	return 0;
}

static int mic_snd_pcm_hw_params(struct snd_pcm_substream *substream,
			     struct snd_pcm_hw_params *hw_params)
{
DBG("enter\n");

	return snd_pcm_lib_malloc_pages(substream,
					params_buffer_bytes(hw_params));
}

static int mic_snd_pcm_hw_free(struct snd_pcm_substream *substream)
{
DBG("enter\n");

	snd_pcm_lib_free_pages(substream);
	return 0;
}

static int mic_snd_pcm_prepare(struct snd_pcm_substream *substream)
{
	struct mic_device *dev = snd_pcm_substream_chip(substream);
	struct snd_pcm_runtime *runtime = substream->runtime;
	unsigned long flags;
	int retval;

DBG("enter\n");

	mic_printk(KERN_INFO, "rate=%d, channels=%d, sample_bits=%d\n",
			runtime->rate, runtime->channels,
			runtime->sample_bits);
	mic_printk(KERN_INFO, "format=%d, access=%d\n",
			runtime->format, runtime->access);
	mic_printk(KERN_INFO, "buffer_bytes=%d, period_bytes=%d\n",
			snd_pcm_lib_buffer_bytes(substream),
			snd_pcm_lib_period_bytes(substream));

	spin_lock_irqsave(&dev->lock, flags);
	dev->c_orig = runtime->dma_area;
	dev->c_left = 0;
	spin_unlock_irqrestore(&dev->lock, flags);

	retval = mic_set_sample_rate(dev, runtime->rate);
	if (retval < 0)
		return retval;

	retval = mic_set_period(dev, snd_pcm_lib_period_bytes(substream));

	return retval;
}

static int mic_snd_pcm_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct mic_device *dev = snd_pcm_substream_chip(substream);

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		if (!dev->running) {
			DBG("trigger start\n");
			dev->running = 1;
			exi_dev_take(dev->exi_device);
			mic_hey(dev);
			mic_enable_sampling(dev, 1);
			mic_control(dev);
			exi_dev_give(dev->exi_device);
		}
		break;
	case SNDRV_PCM_TRIGGER_STOP:
		DBG("trigger stop\n");
		dev->running = 0;
		break;
	}
	return 0;
}

static snd_pcm_uframes_t
mic_snd_pcm_pointer(struct snd_pcm_substream *substream)
{
	struct mic_device *dev = snd_pcm_substream_chip(substream);
	size_t ptr;

	if (!dev->running || !dev->c_left)
		return 0;

	ptr = dev->c_cur - dev->c_orig;
	return bytes_to_frames(substream->runtime, ptr);
}


static struct snd_pcm_ops mic_snd_pcm_capture_ops = {
	.open =        mic_snd_pcm_capture_open,
	.close =       mic_snd_pcm_capture_close,
	.ioctl =       snd_pcm_lib_ioctl,
	.hw_params =   mic_snd_pcm_hw_params,
	.hw_free =     mic_snd_pcm_hw_free,
	.prepare =     mic_snd_pcm_prepare,
	.trigger =     mic_snd_pcm_trigger,
	.pointer =     mic_snd_pcm_pointer,
};

/*
 *
 */
static int mic_snd_new_pcm(struct mic_device *dev)
{
	struct snd_pcm *pcm;
	int retval;

DBG("enter\n");

	retval = snd_pcm_new(dev->card, dev->card->shortname, 0, 0, 1, &pcm);
	if (retval < 0)
		return retval;

	pcm->private_data = dev;
	strcpy(pcm->name, dev->card->shortname);
	dev->pcm = pcm;

	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE,
			&mic_snd_pcm_capture_ops);

	snd_pcm_lib_preallocate_pages_for_all(pcm, SNDRV_DMA_TYPE_CONTINUOUS,
					      snd_dma_continuous_data
					      (GFP_KERNEL),
					      32*1024, 32*1024);
	return 0;
}

/*
 *
 */
static int mic_init_snd(struct mic_device *dev)
{
	struct snd_card *card;
	int retval = -ENOMEM;

DBG("enter\n");

	retval = snd_card_create(index, id, THIS_MODULE, 0, &card);
	if (retval < 0) {
		mic_printk(KERN_ERR, "unable to create sound card\n");
		goto err_card;
	}

	strcpy(card->driver, DRV_MODULE_NAME);
	strcpy(card->shortname, DRV_MODULE_NAME);
	strcpy(card->longname, "Nintendo GameCube Microphone");

	dev->card = card;

	retval = mic_snd_new_pcm(dev);
	if (retval < 0)
		goto err_new_pcm;

	retval = snd_card_register(card);
	if (retval) {
		mic_printk(KERN_ERR, "unable to register sound card\n");
		goto err_card_register;
	}

	return 0;

err_card_register:
err_new_pcm:
	snd_card_free(card);
	dev->card = NULL;
err_card:
	return retval;
}

/*
 *
 */
static void mic_exit_snd(struct mic_device *dev)
{
DBG("enter\n");

	if (dev->card) {
		snd_card_disconnect(dev->card);
		snd_card_free_when_closed(dev->card);

		dev->card = NULL;
		dev->pcm = NULL;
		dev->c_substream = NULL;
	}
}

/*
 *
 */
static int mic_init(struct mic_device *dev)
{
	struct exi_device *exi_device = dev->exi_device;
	struct exi_channel *exi_channel = exi_get_exi_channel(exi_device);
	int channel;
	int retval = -ENOMEM;

DBG("enter\n");

	spin_lock_init(&dev->lock);

	dev->running = 0;

	retval = mic_init_snd(dev);
	if (retval)
		goto err_init_snd;

	init_waitqueue_head(&dev->io_waitq);
	channel = to_channel(exi_get_exi_channel(dev->exi_device));
	dev->io_thread = kthread_run(mic_io_thread, dev, "kmicd/%d", channel);
	if (IS_ERR(dev->io_thread)) {
		mic_printk(KERN_ERR, "error creating io thread\n");
		goto err_io_thread;
	}

	retval = exi_event_register(exi_channel, EXI_EVENT_IRQ,
				    exi_device,
				    mic_event_handler, dev,
				    0 /*(1 << to_channel(exi_channel))*/);
	if (retval) {
		mic_printk(KERN_ERR, "error registering exi event\n");
		goto err_event_register;
	}

	retval = mic_init_proc(dev);
	if (retval)
		goto err_init_proc;

	return 0;

err_init_proc:
	exi_event_unregister(exi_channel, EXI_EVENT_IRQ);
err_event_register:
	mic_stop_io_thread(dev);
err_io_thread:
	mic_exit_snd(dev);
err_init_snd:
	return retval;

}

/*
 *
 */
static void mic_exit(struct mic_device *dev)
{
	struct exi_device *exi_device = dev->exi_device;
	struct exi_channel *exi_channel = exi_get_exi_channel(exi_device);

DBG("enter\n");

	dev->running = 0;

	mic_exit_proc(dev);

	exi_event_unregister(exi_channel, EXI_EVENT_IRQ);

	if (!IS_ERR(dev->io_thread))
		mic_stop_io_thread(dev);

	mic_exit_snd(dev);
}

/*
 *
 */
static int mic_probe(struct exi_device *exi_device)
{
	struct mic_device *dev;
	int retval;

	/* we only care about the microphone */
	if (exi_device->eid.id != MIC_EXI_ID)
		return -ENODEV;

	DBG("Microphone inserted\n");

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	dev->exi_device = exi_device_get(exi_device);
	exi_set_drvdata(exi_device, dev);

	retval = mic_init(dev);
	if (retval) {
		exi_set_drvdata(exi_device, NULL);
		exi_device_put(exi_device);
		dev->exi_device = NULL;
		kfree(dev);
	}

	return retval;
}

/*
 *
 */
static void mic_remove(struct exi_device *exi_device)
{
	struct mic_device *dev = exi_get_drvdata(exi_device);

	DBG("Microphone removed\n");

	if (dev) {
		mic_exit(dev);
		if (dev->exi_device)
			exi_device_put(dev->exi_device);
		dev->exi_device = NULL;
		kfree(dev);
	}
	exi_set_drvdata(exi_device, NULL);
}

static struct exi_device_id mic_eid_table[] = {
	[0] = {
	       .channel = MIC_SLOTA_CHANNEL,
	       .device = MIC_SLOTA_DEVICE,
	       .id = MIC_EXI_ID,
	       },
	[1] = {
	       .channel = MIC_SLOTB_CHANNEL,
	       .device = MIC_SLOTB_DEVICE,
	       .id = MIC_EXI_ID,
	       },
	{.id = 0}
};

static struct exi_driver mic_driver = {
	.name = DRV_MODULE_NAME,
	.eid_table = mic_eid_table,
	.frequency = MIC_SPI_CLK_IDX,
	.probe = mic_probe,
	.remove = mic_remove,
};

static int __init mic_init_module(void)
{
	int retval = 0;

	mic_printk(KERN_INFO, "%s - version %s\n", DRV_DESCRIPTION,
		  mic_driver_version);

	retval = exi_driver_register(&mic_driver);

	return retval;
}

static void __exit mic_exit_module(void)
{
	exi_driver_unregister(&mic_driver);
}

module_init(mic_init_module);
module_exit(mic_exit_module);

