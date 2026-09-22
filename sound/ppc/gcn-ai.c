// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * GameCube/Wii audio DMA playback.
 * Hardware register definitions derived from the GameCube Linux AI driver
 * (2004-2009, The GameCube Linux Team / Albert Herranz).
 *
 * AID signals the START of a latched buffer, including the initial start.
 * Keep DMA enabled and queue the following period at each interrupt. The
 * first interrupt must not be reported to ALSA as a completed period.
 */
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <sound/core.h>
#include <sound/info.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>

#define DSP_CSR		0x0a
#define DSP_PIINT	BIT(1)
#define DSP_AID		BIT(3)
#define DSP_AID_MASK	BIT(4)
#define DSP_ARAM		BIT(5)
#define DSP_INT		BIT(7)
#define DSP_ADDR_HI	0x30
#define DSP_ADDR_LO	0x32
#define DSP_LEN		0x36
#define DSP_LEFT		0x3a
#define DSP_PLAY		BIT(15)
#define AI_CONTROL	0x00
#define AI_RATE_32K	BIT(6)

struct gcn_ai {
	struct snd_card *card;
	struct snd_pcm_substream *substream;
	void __iomem *dsp;
	void __iomem *ai;
	spinlock_t lock;
	int irq;
	bool running;
	bool first;
	dma_addr_t dma;
	u32 period_bytes;
	u32 buffer_bytes;
	u32 current;
	u32 queued;
	u64 interrupts;
	u64 periods;
	u64 starts;
	u64 stops;
	u64 period_ns;
	u64 last_irq_ns;
	u64 max_irq_gap_ns;
	u64 late_irqs;
	u64 stream_periods;
	u64 last_pointer;
	u64 pointer_rewinds;
};

static const struct snd_pcm_hardware gcn_ai_hardware = {
	.info = SNDRV_PCM_INFO_INTERLEAVED | SNDRV_PCM_INFO_BLOCK_TRANSFER |
		SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_MMAP_VALID,
	.formats = SNDRV_PCM_FMTBIT_S16_BE,
	.rates = SNDRV_PCM_RATE_32000 | SNDRV_PCM_RATE_48000,
	.rate_min = 32000,
	.rate_max = 48000,
	.channels_min = 2,
	.channels_max = 2,
	.buffer_bytes_max = 32768,
	.period_bytes_min = 1024,
	.period_bytes_max = 16384,
	.periods_min = 2,
	.periods_max = 32,
};

/* W1C status bits belonging to the DSP/ARAM must never be acknowledged here. */
static void gcn_ai_irq_control(struct gcn_ai *chip, bool enable, bool ack)
{
	u16 csr = ioread16be(chip->dsp + DSP_CSR);

	csr &= ~(DSP_AID | DSP_ARAM | DSP_INT | DSP_PIINT | DSP_AID_MASK);
	if (enable)
		csr |= DSP_AID_MASK;
	if (ack)
		csr |= DSP_AID;
	iowrite16be(csr, chip->dsp + DSP_CSR);
}

/* Caller holds chip->lock with local IRQs disabled. */
static void gcn_ai_stop(struct gcn_ai *chip)
{
	iowrite16be(ioread16be(chip->dsp + DSP_LEN) & ~DSP_PLAY,
		    chip->dsp + DSP_LEN);
	gcn_ai_irq_control(chip, false, true);
	if (chip->running)
		chip->stops++;
	chip->running = false;
	chip->first = false;
}

static void gcn_ai_queue(struct gcn_ai *chip, u32 offset, bool play)
{
	dma_addr_t address = chip->dma + offset;

	/* The ring is dma_alloc_coherent memory, including on noncoherent PPC. */
	dma_wmb();
	iowrite16be(upper_16_bits(address), chip->dsp + DSP_ADDR_HI);
	iowrite16be(lower_16_bits(address), chip->dsp + DSP_ADDR_LO);
	iowrite16be((chip->period_bytes >> 5) | (play ? DSP_PLAY : 0),
		    chip->dsp + DSP_LEN);
	chip->queued = offset;
}

static irqreturn_t gcn_ai_interrupt(int irq, void *data)
{
	struct gcn_ai *chip = data;
	struct snd_pcm_substream *elapsed = NULL;
	unsigned long flags;
	u32 next;

	spin_lock_irqsave(&chip->lock, flags);
	if (!(ioread16be(chip->dsp + DSP_CSR) & DSP_AID)) {
		spin_unlock_irqrestore(&chip->lock, flags);
		return IRQ_NONE;
	}
	chip->interrupts++;
	gcn_ai_irq_control(chip, chip->running, true);
	if (chip->running) {
		u64 now = ktime_get_ns();

		if (chip->last_irq_ns) {
			u64 gap = now - chip->last_irq_ns;

			chip->max_irq_gap_ns = max(chip->max_irq_gap_ns, gap);
			if (gap > chip->period_ns + chip->period_ns / 2)
				chip->late_irqs++;
		}
		chip->last_irq_ns = now;
		if (chip->first) {
			chip->first = false;
		} else {
			chip->current = chip->queued;
			chip->periods++;
			chip->stream_periods++;
			elapsed = chip->substream;
		}
		next = chip->current + chip->period_bytes;
		if (next >= chip->buffer_bytes)
			next = 0;
		gcn_ai_queue(chip, next, true);
	}
	spin_unlock_irqrestore(&chip->lock, flags);
	/* ALSA takes its stream lock, which .pointer/.trigger hold before ours. */
	if (elapsed)
		snd_pcm_period_elapsed(elapsed);
	return IRQ_HANDLED;
}

static int gcn_ai_open(struct snd_pcm_substream *substream)
{
	struct gcn_ai *chip = snd_pcm_substream_chip(substream);
	int ret;

	substream->runtime->hw = gcn_ai_hardware;
	ret = snd_pcm_hw_constraint_step(substream->runtime, 0,
					 SNDRV_PCM_HW_PARAM_PERIOD_BYTES, 32);
	if (ret < 0)
		return ret;
	ret = snd_pcm_hw_constraint_integer(substream->runtime,
					    SNDRV_PCM_HW_PARAM_PERIODS);
	if (ret < 0)
		return ret;
	chip->substream = substream;
	return 0;
}

static int gcn_ai_close(struct snd_pcm_substream *substream)
{
	struct gcn_ai *chip = snd_pcm_substream_chip(substream);
	unsigned long flags;

	spin_lock_irqsave(&chip->lock, flags);
	gcn_ai_stop(chip);
	spin_unlock_irqrestore(&chip->lock, flags);
	synchronize_irq(chip->irq);
	chip->substream = NULL;
	return 0;
}

static int gcn_ai_prepare(struct snd_pcm_substream *substream)
{
	struct gcn_ai *chip = snd_pcm_substream_chip(substream);
	struct snd_pcm_runtime *runtime = substream->runtime;
	unsigned long flags;
	u32 rate;

	if ((runtime->dma_addr & 31) ||
	    runtime->dma_addr + snd_pcm_lib_buffer_bytes(substream) - 1 >
		*chip->card->dev->dma_mask)
		return -EINVAL;
	spin_lock_irqsave(&chip->lock, flags);
	gcn_ai_stop(chip);
	chip->dma = runtime->dma_addr;
	chip->period_bytes = snd_pcm_lib_period_bytes(substream);
	chip->buffer_bytes = snd_pcm_lib_buffer_bytes(substream);
	chip->current = 0;
	chip->queued = 0;
	chip->period_ns = div_u64((u64)runtime->period_size * NSEC_PER_SEC,
				 runtime->rate);
	chip->last_irq_ns = 0;
	chip->max_irq_gap_ns = 0;
	chip->late_irqs = 0;
	chip->stream_periods = 0;
	chip->last_pointer = 0;
	chip->pointer_rewinds = 0;
	rate = ioread32be(chip->ai + AI_CONTROL);
	if (runtime->rate == 32000)
		rate |= AI_RATE_32K;
	else
		rate &= ~AI_RATE_32K;
	iowrite32be(rate, chip->ai + AI_CONTROL);
	spin_unlock_irqrestore(&chip->lock, flags);
	return 0;
}

static int gcn_ai_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct gcn_ai *chip = snd_pcm_substream_chip(substream);
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&chip->lock, flags);
	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		chip->current = 0;
		chip->first = true;
		chip->running = true;
		chip->starts++;
		gcn_ai_queue(chip, 0, false);
		gcn_ai_irq_control(chip, true, true);
		iowrite16be((chip->period_bytes >> 5) | DSP_PLAY,
			    chip->dsp + DSP_LEN);
		break;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
		gcn_ai_stop(chip);
		break;
	default:
		ret = -EINVAL;
	}
	spin_unlock_irqrestore(&chip->lock, flags);
	return ret;
}

static snd_pcm_uframes_t gcn_ai_pointer(struct snd_pcm_substream *substream)
{
	struct gcn_ai *chip = snd_pcm_substream_chip(substream);
	unsigned long flags;
	u32 offset, left, pending;
	u64 absolute;

	spin_lock_irqsave(&chip->lock, flags);
	offset = chip->current;
	/* Before the first latch interrupt, DSP_LEFT can still be stale. */
	if (chip->running && !chip->first) {
		/* Re-read the count if a period boundary passed during the read. */
		pending = ioread16be(chip->dsp + DSP_CSR) & DSP_AID;
		left = (ioread16be(chip->dsp + DSP_LEFT) + 1) << 5;
		if (!pending && (ioread16be(chip->dsp + DSP_CSR) & DSP_AID)) {
			pending = DSP_AID;
			left = (ioread16be(chip->dsp + DSP_LEFT) + 1) << 5;
		}
		if (pending)
			offset = chip->queued;
		absolute = (chip->stream_periods + !!pending) * chip->period_bytes;
		if (left <= chip->period_bytes)
			offset += chip->period_bytes - left;
		if (left <= chip->period_bytes)
			absolute += chip->period_bytes - left;
		if (absolute < chip->last_pointer)
			chip->pointer_rewinds++;
		chip->last_pointer = absolute;
	}
	if (chip->buffer_bytes && offset >= chip->buffer_bytes)
		offset -= chip->buffer_bytes;
	spin_unlock_irqrestore(&chip->lock, flags);
	return bytes_to_frames(substream->runtime, offset);
}

static const struct snd_pcm_ops gcn_ai_ops = {
	.open = gcn_ai_open,
	.close = gcn_ai_close,
	.prepare = gcn_ai_prepare,
	.trigger = gcn_ai_trigger,
	.pointer = gcn_ai_pointer,
};

static void gcn_ai_proc_read(struct snd_info_entry *entry,
			     struct snd_info_buffer *buffer)
{
	struct gcn_ai *chip = entry->private_data;
	unsigned long flags;
	u64 interrupts, periods, starts, stops;
	u64 period_ns, max_irq_gap_ns, late_irqs, pointer_rewinds;
	u32 current, queued, period_bytes, buffer_bytes, rate;
	u16 csr, len, left;
	dma_addr_t dma;
	bool running, first;

	spin_lock_irqsave(&chip->lock, flags);
	interrupts = chip->interrupts;
	periods = chip->periods;
	starts = chip->starts;
	stops = chip->stops;
	period_ns = chip->period_ns;
	max_irq_gap_ns = chip->max_irq_gap_ns;
	late_irqs = chip->late_irqs;
	pointer_rewinds = chip->pointer_rewinds;
	current = chip->current;
	queued = chip->queued;
	period_bytes = chip->period_bytes;
	buffer_bytes = chip->buffer_bytes;
	dma = chip->dma;
	running = chip->running;
	first = chip->first;
	csr = ioread16be(chip->dsp + DSP_CSR);
	len = ioread16be(chip->dsp + DSP_LEN);
	left = ioread16be(chip->dsp + DSP_LEFT);
	rate = ioread32be(chip->ai + AI_CONTROL);
	spin_unlock_irqrestore(&chip->lock, flags);
	snd_iprintf(buffer, "running=%u first=%u irq=%d\n", running, first, chip->irq);
	snd_iprintf(buffer, "interrupts=%llu periods=%llu starts=%llu stops=%llu\n",
		    interrupts, periods, starts, stops);
	snd_iprintf(buffer, "dma=%pad buffer=%u period=%u current=%u queued=%u\n",
		    &dma, buffer_bytes, period_bytes, current, queued);
	snd_iprintf(buffer, "csr=%04x length=%04x left=%04x ai=%08x\n",
		    csr, len, left, rate);
	snd_iprintf(buffer, "period_ns=%llu max_irq_gap_ns=%llu late_irqs=%llu pointer_rewinds=%llu\n",
		    period_ns, max_irq_gap_ns, late_irqs, pointer_rewinds);
}

static void gcn_ai_card_free(struct snd_card *card)
{
	struct gcn_ai *chip = card->private_data;
	unsigned long flags;

	spin_lock_irqsave(&chip->lock, flags);
	gcn_ai_stop(chip);
	spin_unlock_irqrestore(&chip->lock, flags);
	free_irq(chip->irq, chip);
}

static int gcn_ai_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *dsp_node;
	struct snd_card *card;
	struct snd_pcm *pcm;
	struct resource dsp_res;
	struct gcn_ai *chip;
	int ret;
	bool wii = of_device_is_compatible(dev->of_node, "nintendo,hollywood-ai");

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(wii ? 29 : 26));
	if (ret)
		return ret;
	ret = snd_card_new(dev, -1, wii ? "WiiAI" : "GameCubeAI", THIS_MODULE,
			   sizeof(*chip), &card);
	if (ret)
		return ret;
	chip = card->private_data;
	chip->card = card;
	spin_lock_init(&chip->lock);
	chip->ai = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(chip->ai)) {
		ret = PTR_ERR(chip->ai);
		goto error;
	}
	/* Existing Wii DTBs describe DSP and AI as separate platform devices. */
	dsp_node = of_parse_phandle(dev->of_node, "nintendo,dsp", 0);
	if (!dsp_node)
		dsp_node = of_find_compatible_node(NULL, NULL, "nintendo,flipper-dsp");
	if (!dsp_node) {
		ret = -ENODEV;
		goto error;
	}
	ret = of_address_to_resource(dsp_node, 0, &dsp_res);
	of_node_put(dsp_node);
	if (ret)
		goto error;
	chip->dsp = devm_ioremap_resource(dev, &dsp_res);
	if (IS_ERR(chip->dsp)) {
		ret = PTR_ERR(chip->dsp);
		goto error;
	}
	chip->irq = platform_get_irq(pdev, 0);
	if (chip->irq < 0) {
		ret = chip->irq;
		goto error;
	}
	gcn_ai_stop(chip);
	ret = request_irq(chip->irq, gcn_ai_interrupt, IRQF_SHARED, "gcn-ai", chip);
	if (ret)
		goto error;
	card->private_free = gcn_ai_card_free;
	card->sync_irq = chip->irq;
	strscpy(card->driver, "gcn-ai");
	strscpy(card->shortname, wii ? "Wii Audio" : "GameCube Audio");
	strscpy(card->longname, "Nintendo GameCube/Wii audio DMA");
	ret = snd_pcm_new(card, "AI PCM", 0, 1, 0, &pcm);
	if (ret)
		goto error;
	pcm->private_data = chip;
	strscpy(pcm->name, "AI playback");
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &gcn_ai_ops);
	ret = snd_pcm_set_managed_buffer_all(pcm, SNDRV_DMA_TYPE_DEV, dev,
					     32768, 32768);
	if (ret)
		goto error;
	snd_card_ro_proc_new(card, "ai", chip, gcn_ai_proc_read);
	ret = snd_card_register(card);
	if (ret)
		goto error;
	platform_set_drvdata(pdev, card);
	dev_info(dev, "32/48 kHz stereo S16_BE playback, IRQ %d\n", chip->irq);
	return 0;
error:
	snd_card_free(card);
	return ret;
}

static void gcn_ai_shutdown(struct platform_device *pdev)
{
	struct snd_card *card = platform_get_drvdata(pdev);
	struct gcn_ai *chip = card->private_data;
	unsigned long flags;

	spin_lock_irqsave(&chip->lock, flags);
	gcn_ai_stop(chip);
	spin_unlock_irqrestore(&chip->lock, flags);
	synchronize_irq(chip->irq);
}

static void gcn_ai_remove(struct platform_device *pdev)
{
	struct snd_card *card = platform_get_drvdata(pdev);

	snd_card_disconnect(card);
	gcn_ai_shutdown(pdev);
	snd_card_free(card);
}

static const struct of_device_id gcn_ai_match[] = {
	{ .compatible = "nintendo,hollywood-ai" },
	{ .compatible = "nintendo,flipper-ai" },
	{ }
};
MODULE_DEVICE_TABLE(of, gcn_ai_match);

static struct platform_driver gcn_ai_driver = {
	.probe = gcn_ai_probe,
	.remove = gcn_ai_remove,
	.shutdown = gcn_ai_shutdown,
	.driver = {
		.name = "gcn-ai",
		.of_match_table = gcn_ai_match,
	},
};
module_platform_driver(gcn_ai_driver);
MODULE_DESCRIPTION("Nintendo GameCube/Wii audio DMA playback");
MODULE_AUTHOR("The GameCube Linux Team; Wii Linux contributors");
MODULE_LICENSE("GPL");
