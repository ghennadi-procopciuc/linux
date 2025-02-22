// SPDX-License-Identifier: GPL-2.0
/*
 * NXP S32 SAR-ADC driver (adapted from Freescale Vybrid vf610 ADC
 * driver by Fugang Duan <B38611@freescale.com>)
 *
 * Copyright 2013 Freescale Semiconductor, Inc.
 * Copyright 2017, 2020-2022 NXP
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/regulator/consumer.h>
#include <linux/of_platform.h>
#include <linux/err.h>
#include <linux/iopoll.h>
#include <linux/version.h>

#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>
#include <linux/iio/buffer.h>
#include <linux/iio/trigger.h>
#include <linux/iio/triggered_buffer.h>
#include <linux/iio/trigger_consumer.h>

/* This will be the driver name the kernel reports */
#define DRIVER_NAME "s32-adc"

/* S32 ADC registers */
#define REG_ADC_MCR			0x00
#define REG_ADC_MSR			0x04
#define REG_ADC_ISR			0x10
#define REG_ADC_CEOCFR(g)	(0x14 + ((g) << 2))
#define REG_ADC_IMR			0x20
#define REG_ADC_CIMR(g)		(0x24 + ((g) << 2))
#define REG_ADC_CTR(g)		(0x94 + ((g) << 2))
#define REG_ADC_NCMR(g)		(0xa4 + ((g) << 2))
#define REG_ADC_CDR(c)		(0x100 + ((c) << 2))
#define REG_ADC_CALSTAT		0x39c

/* Main Configuration Register field define */
#define ADC_PWDN			0x01
#define ADC_ACKO			0x20
#define ADC_ADCLKSEL		0x100
#define ADC_TSAMP_MASK		0x600
#define ADC_NRSMPL_32		0X800
#define ADC_NRSMPL_128		0X1000
#define ADC_NRSMPL_512		0X1800
#define ADC_NRSMPL_MASK		0x1800
#define ADC_AVGEN			0x2000
#define ADC_CALSTART		0x4000
#define ADC_NSTART			0x1000000
#define ADC_MODE			0x20000000
#define ADC_OWREN			0x80000000

/* Main Status Register field define */
#define ADC_CALBUSY		BIT(29)
#define ADC_CALFAIL		BIT(30)

/* Interrupt Status Register field define */
#define ADC_ECH			0x01
#define ADC_EOC			0x02

/* Channel Pending Register field define */
#define ADC_EOC_CH(c)		(1 << (c) % 32)

/* Interrupt Mask Register field define */
#define ADC_MSKECH			0x01

/* Channel Interrupt Mask Register field define */
#define ADC_CIM(c)			(1 << (c) % 32)
#define ADC_CIM_MASK		0xFF

/* Conversion Timing Register field define */
#define ADC_INPSAMP_MIN		8
#define ADC_INPSAMP_MAX		0xFF

/* Normal Conversion Mask Register field define */
#define ADC_CH(c)			(1 << (c) % 32)
#define ADC_CH_MASK			0xFF

/* Channel Data Register field define */
#define ADC_CDATA_MASK		0xFFF
#define ADC_VALID			0x80000

/* Calibration Status Register field define */
#define ADC_TEST_RESULT(x)		((x) >> 16)
#define ADC_STAT_n(x, n)		((x) & 1 << ((n) - 1))

/* Other field define */
#define ADC_CLK_FREQ_40MHz		40000000
#define ADC_CLK_FREQ_80MHz		80000000
#define ADC_CLK_FREQ_160MHz		160000000
#define ADC_CONV_TIMEOUT		100 /* ms */
#define ADC_CAL_TIMEOUT				100000 /* us */
#define ADC_WAIT				2000   /* us */
#define ADC_NSEC_PER_SEC		1000000000
#define ADC_NUM_CAL_STEPS		14
#define ADC_NUM_GROUPS			2
#define ADC_RESOLUTION			12

/* Duration of conversion phases */
#define ADC_TPT			2
#define ADC_CT			((ADC_RESOLUTION + 2) * 4)
#define ADC_DP			2

#define BUFFER_ECH_NUM_OK	2
#define ADC_NUM_CHANNELS	8
#define CHANNEL_UNASSIGNED		-2
#define ADC_DATA_READ_ERROR		-1
#define ADC_DATA_READ_SUCCESS	0

enum freq_sel {
	ADC_BUSCLK_EQUAL,
	ADC_BUSCLK_HALF,
	ADC_BUSCLK_FOURTH,
};

enum average_sel {
	ADC_SAMPLE_16,
	ADC_SAMPLE_32,
	ADC_SAMPLE_128,
	ADC_SAMPLE_512,
};

enum {
	SAR_ADC_CONTINUOUS = 0,
	SAR_ADC_ONESHOT
};

struct s32cc_adc_feature {
	enum freq_sel	freq_sel;

	int	sampling_duration[ADC_NUM_GROUPS];
	int	sample_num;

	bool	auto_clk_off;
	bool	calibration;
	bool	ovwren;
};

struct s32cc_adc {
	struct device *dev;
	void __iomem *regs;
	struct clk *clk;

	u16 value;
	u32 vref;
	int current_channel;
	int channels_used;
	int buffer_ech_num;
	struct s32cc_adc_feature adc_feature;

	struct completion completion;

	u16 buffer[ADC_NUM_CHANNELS];
	u16 buffered_chan[ADC_NUM_CHANNELS];
};

#define ADC_CHAN(_idx, _chan_type) {			\
	.type = (_chan_type),					\
	.indexed = 1,						\
	.channel = (_idx),					\
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),		\
	.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE) |	\
				BIT(IIO_CHAN_INFO_SAMP_FREQ),	\
	.scan_index = (_idx),	\
	.scan_type = {			\
		.sign = 'u',		\
		.realbits = 12,		\
		.storagebits = 16,	\
	},						\
}

static const struct iio_chan_spec s32cc_adc_iio_channels[] = {
	ADC_CHAN(0, IIO_VOLTAGE),
	ADC_CHAN(1, IIO_VOLTAGE),
	ADC_CHAN(2, IIO_VOLTAGE),
	ADC_CHAN(3, IIO_VOLTAGE),
	ADC_CHAN(4, IIO_VOLTAGE),
	ADC_CHAN(5, IIO_VOLTAGE),
	ADC_CHAN(6, IIO_VOLTAGE),
	ADC_CHAN(7, IIO_VOLTAGE),
	IIO_CHAN_SOFT_TIMESTAMP(32),
};

static inline int group_idx(int channel)
{
	if (channel >= 0 && channel <= 7)
		return 0;
	if (channel >= 32 && channel <= 38)
		return 1;

	return -ECHRNG;
}

static inline unsigned long s32cc_adc_clk_rate(struct s32cc_adc *info)
{
	unsigned long ret = clk_get_rate(info->clk);
	struct s32cc_adc_feature *adc_feature = &info->adc_feature;

	if (adc_feature->freq_sel == ADC_BUSCLK_HALF)
		ret >>= 1;
	else if (adc_feature->freq_sel == ADC_BUSCLK_FOURTH)
		ret >>= 2;

	return ret;
}

static inline void s32cc_adc_cfg_init(struct s32cc_adc *info)
{
	struct s32cc_adc_feature *adc_feature = &info->adc_feature;

	/* set default Configuration for ADC controller */
	adc_feature->freq_sel = ADC_BUSCLK_EQUAL;

	adc_feature->calibration = true;
	adc_feature->ovwren = false;

	adc_feature->sampling_duration[0] =
		adc_feature->sampling_duration[1] = 20;
	adc_feature->sample_num = ADC_SAMPLE_512;
}

static void s32cc_adc_cfg_post_set(struct s32cc_adc *info)
{
	struct s32cc_adc_feature *adc_feature = &info->adc_feature;
	int mcr_data = 0, imr_data = 0;

	/* auto-clock-off mode enable */
	if (adc_feature->auto_clk_off)
		mcr_data |= ADC_ACKO;

	/* data overwrite enable */
	if (adc_feature->ovwren)
		mcr_data |= ADC_OWREN;

	writel(mcr_data, info->regs + REG_ADC_MCR);

	/* End of Conversion Chain interrupt enable */
	imr_data |= ADC_MSKECH;
	writel(imr_data, info->regs + REG_ADC_IMR);
}

static void s32cc_adc_calibration(struct s32cc_adc *info)
{
	struct s32cc_adc_feature *adc_feature = &info->adc_feature;
	int mcr_data, msr_data, calstat_data;
	int step;
	unsigned long clk_rate;

	if (!info->adc_feature.calibration)
		return;

	mcr_data = readl(info->regs + REG_ADC_MCR);

	/* default sample period (22 cycles of ADC clk) */
	mcr_data &= ~ADC_TSAMP_MASK;

	/* update hardware average selection */
	mcr_data |= ADC_AVGEN;
	mcr_data &= ~ADC_NRSMPL_MASK;
	switch (adc_feature->sample_num) {
	case ADC_SAMPLE_16:
		break;
	case ADC_SAMPLE_32:
		mcr_data |= ADC_NRSMPL_32;
		break;
	case ADC_SAMPLE_128:
		mcr_data |= ADC_NRSMPL_128;
		break;
	case ADC_SAMPLE_512:
		mcr_data |= ADC_NRSMPL_512;
		break;
	default:
		dev_err(info->dev,
			"error hardware sample average select\n");
	}

	/* set AD_clk frequency to 40 MHz */
	mcr_data &= ~ADC_ADCLKSEL;
	clk_rate = clk_get_rate(info->clk);
	if (clk_rate == ADC_CLK_FREQ_40MHz) {
		/* AD_CLK frequency is equal to bus clock frequency */
		mcr_data |= ADC_ADCLKSEL;
	} else if (clk_rate != ADC_CLK_FREQ_80MHz) {
		dev_err(info->dev, "Bad bus clock frequency\n");
	}

	mcr_data &= ~ADC_PWDN;
	writel(mcr_data, info->regs + REG_ADC_MCR);

	mcr_data |= ADC_CALSTART;
	writel(mcr_data, info->regs + REG_ADC_MCR);

	if (read_poll_timeout(readl, msr_data, !(msr_data & ADC_CALBUSY),
			      ADC_WAIT, ADC_CAL_TIMEOUT, true, info->regs + REG_ADC_MSR))
		dev_err(info->dev, "SAR ADC Calibration failed\n");

	if (msr_data & ADC_CALBUSY) {
		dev_err(info->dev, "Timeout for adc calibration\n");
	} else if (msr_data & ADC_CALFAIL) {
		dev_err(info->dev, "ADC calibration failed\nStep status:\n");
		calstat_data = readl(info->regs + REG_ADC_CALSTAT);
		for (step = 1; step <= ADC_NUM_CAL_STEPS; step++)
			dev_err(info->dev, "Step %d: %s\n", step,
				ADC_STAT_n(calstat_data, step) ?
				"failed" : "passed");
		dev_err(info->dev, "Result for the last failed test: %d\n",
			ADC_TEST_RESULT(calstat_data));
	}

	info->adc_feature.calibration = false;
}

static void s32cc_adc_sample_set(struct s32cc_adc *info)
{
	struct s32cc_adc_feature *adc_feature = &info->adc_feature;
	enum freq_sel freq_sel = adc_feature->freq_sel;
	int mcr_data, ctr_data = 0, group;

	/* configure AD_clk frequency */
	mcr_data = readl(info->regs + REG_ADC_MCR);
	mcr_data |= ADC_PWDN;
	writel(mcr_data, info->regs + REG_ADC_MCR);

	/* restore preferred AD_clk frequency */
	if (freq_sel == ADC_BUSCLK_EQUAL)
		mcr_data |= ADC_ADCLKSEL;
	else if (freq_sel != ADC_BUSCLK_HALF)
		dev_err(info->dev, "error frequency selection\n");

	writel(mcr_data, info->regs + REG_ADC_MCR);

	mcr_data &= ~ADC_PWDN;

	writel(mcr_data, info->regs + REG_ADC_MCR);

	/* sampling phase duration set */
	for (group = 0; group < ADC_NUM_GROUPS; group++) {
		ctr_data |= min(adc_feature->sampling_duration[group],
			ADC_INPSAMP_MAX);
		writel(ctr_data, info->regs + REG_ADC_CTR(group));
	}
}

static void s32cc_adc_hw_init(struct s32cc_adc *info)
{
	/* CFG: Feature set */
	s32cc_adc_cfg_post_set(info);

	/* adc calibration */
	s32cc_adc_calibration(info);

	/* sampling speed set */
	s32cc_adc_sample_set(info);
}

static int s32cc_adc_read_data(struct s32cc_adc *info,
			       unsigned int chan)
{
	int group, ceocfr_data, cdr_data;

	group = group_idx(chan);
	ceocfr_data = readl(info->regs + REG_ADC_CEOCFR(group));
	if (!(ceocfr_data & ADC_EOC_CH(chan)))
		return ADC_DATA_READ_ERROR;

	writel(ADC_EOC_CH(chan), info->regs + REG_ADC_CEOCFR(group));

	cdr_data = readl(info->regs + REG_ADC_CDR(chan));
	if (!(cdr_data & ADC_VALID)) {
		dev_err(info->dev, "error invalid data\n");
		return ADC_DATA_READ_ERROR;
	}

	info->value = cdr_data & ADC_CDATA_MASK;
	return ADC_DATA_READ_SUCCESS;
}

static irqreturn_t s32cc_adc_isr(int irq, void *dev_id)
{
	struct iio_dev *indio_dev = (struct iio_dev *)dev_id;
	struct s32cc_adc *info = iio_priv(indio_dev);
	int isr_data, i;

	isr_data = readl(info->regs + REG_ADC_ISR);
	if (isr_data & ADC_ECH) {
		writel(ADC_ECH | ADC_EOC,
		       info->regs + REG_ADC_ISR);

		if (iio_buffer_enabled(indio_dev)) {
			info->buffer_ech_num++;
			if (info->buffer_ech_num < BUFFER_ECH_NUM_OK)
				return IRQ_HANDLED;
			info->buffer_ech_num = 0;

			for (i = 0; i < info->channels_used; i++) {
				if (s32cc_adc_read_data(info,
							info->buffered_chan[i]) ==
							ADC_DATA_READ_ERROR)
					return IRQ_HANDLED;
				info->buffer[i] = info->value;
			}

			iio_push_to_buffers_with_timestamp(indio_dev,
							   info->buffer,
							   iio_get_time_ns(indio_dev));
			iio_trigger_notify_done(indio_dev->trig);
		} else {
			if (s32cc_adc_read_data(info, info->current_channel) ==
					ADC_DATA_READ_ERROR)
				return IRQ_HANDLED;
			complete(&info->completion);
		}
	}

	return IRQ_HANDLED;
}

static void s32cc_adc_configure_read(struct s32cc_adc *info,
				     unsigned int chan, int group, bool mode,
				     int *ncmr_data, int *cimr_data)
{
	int i;

	for (i = 0; i < ADC_NUM_GROUPS; i++) {
		(*ncmr_data) = readl(info->regs + REG_ADC_NCMR(i));
		(*cimr_data) = readl(info->regs + REG_ADC_CIMR(i));

		if (mode == SAR_ADC_ONESHOT) {
			(*ncmr_data) &= ~ADC_CH_MASK;
			(*cimr_data) &= ~ADC_CIM_MASK;
		}

		if (i == group) {
			(*ncmr_data) |= ADC_CH(chan);
			(*cimr_data) |= ADC_CIM(chan);
		}

		writel((*ncmr_data), info->regs + REG_ADC_NCMR(i));
		writel((*cimr_data), info->regs + REG_ADC_CIMR(i));
	}
}

static void s32cc_adc_enable_conversion(struct s32cc_adc *info, bool mode)
{
	int mcr_data;

	mcr_data = readl(info->regs + REG_ADC_MCR);
	if (mode == SAR_ADC_ONESHOT)
		mcr_data &= ~ADC_MODE;
	else
		mcr_data |= ADC_MODE;
	mcr_data &= ~ADC_PWDN;
	writel(mcr_data, info->regs + REG_ADC_MCR);

	/* Ensure there are at least three cycles between the
	 * configuration of NCMR and the setting of NSTART
	 */
	ndelay(ADC_NSEC_PER_SEC / s32cc_adc_clk_rate(info) * 3);

	mcr_data |= ADC_NSTART;
	writel(mcr_data, info->regs + REG_ADC_MCR);
}

static int s32cc_read_raw(struct iio_dev *indio_dev,
			  struct iio_chan_spec const *chan,
			  int *val,
			  int *val2,
			  long mask)
{
	struct s32cc_adc *info = iio_priv(indio_dev);
	int ncmr_data, cimr_data;
	int mcr_data;
	long group, ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		mutex_lock(&indio_dev->mlock);
		if (iio_buffer_enabled(indio_dev)) {
			mutex_unlock(&indio_dev->mlock);
			return -EBUSY;
		}

		reinit_completion(&info->completion);

		group = group_idx(chan->channel);
		if (group < 0) {
			mutex_unlock(&indio_dev->mlock);
			return group;
		}

		s32cc_adc_configure_read(info, chan->channel, group,
					 SAR_ADC_ONESHOT, &ncmr_data,
					 &cimr_data);
		info->current_channel = chan->channel;
		s32cc_adc_enable_conversion(info, SAR_ADC_ONESHOT);

		ret = wait_for_completion_interruptible_timeout
			(&info->completion,
			msecs_to_jiffies(ADC_CONV_TIMEOUT));

		ncmr_data &= ~ADC_CH(info->current_channel);
		cimr_data &= ~ADC_CIM(info->current_channel);
		writel(ncmr_data, info->regs + REG_ADC_NCMR(group));
		writel(cimr_data, info->regs + REG_ADC_CIMR(group));

		mcr_data = readl(info->regs + REG_ADC_MCR);
		mcr_data |= ADC_PWDN;
		writel(mcr_data, info->regs + REG_ADC_MCR);

		if (ret == 0) {
			mutex_unlock(&indio_dev->mlock);
			return -ETIMEDOUT;
		}
		if (ret < 0) {
			mutex_unlock(&indio_dev->mlock);
			return ret;
		}

		*val = info->value;

		mutex_unlock(&indio_dev->mlock);
		return IIO_VAL_INT;

	case IIO_CHAN_INFO_SCALE:
		*val = info->vref;
		*val2 = ADC_RESOLUTION;
		return IIO_VAL_FRACTIONAL_LOG2;

	case IIO_CHAN_INFO_SAMP_FREQ:
		*val = s32cc_adc_clk_rate(info) / (ADC_TPT +
				info->adc_feature.sampling_duration[0] +
				ADC_CT +
				ADC_DP);
		return IIO_VAL_INT;

	default:
		break;
	}

	return -EINVAL;
}

static int s32cc_write_raw(struct iio_dev *indio_dev,
			   struct iio_chan_spec const *chan,
			   int val,
			   int val2,
			   long mask)
{
	struct s32cc_adc *info = iio_priv(indio_dev);
	int samp_time;

	switch (mask) {
	case IIO_CHAN_INFO_SAMP_FREQ:
		samp_time = s32cc_adc_clk_rate(info) / val - (ADC_TPT +
				ADC_CT +
				ADC_DP);
		samp_time = max(samp_time, ADC_INPSAMP_MIN);
		samp_time = min(samp_time, ADC_INPSAMP_MAX);

		info->adc_feature.sampling_duration[0] = samp_time;
		s32cc_adc_sample_set(info);
		return 0;

	default:
		break;
	}

	return -EINVAL;
}

static int s32cc_adc_buffer_postenable(struct iio_dev *indio_dev)
{
	struct s32cc_adc *info = iio_priv(indio_dev);
	unsigned int channel, first_channel = CHANNEL_UNASSIGNED;
	int ncmr_data, cimr_data;
	int group, pos = 0;

	for_each_set_bit(channel, indio_dev->active_scan_mask,
			 ADC_NUM_CHANNELS) {
		if (first_channel == CHANNEL_UNASSIGNED)
			first_channel = channel;

		info->buffered_chan[pos++] = channel;
		group = group_idx(channel);
		if (group < 0)
			return group;

		s32cc_adc_configure_read(info, channel, group,
					 SAR_ADC_CONTINUOUS, &ncmr_data,
					 &cimr_data);
	}

	info->channels_used = pos;
	info->current_channel = first_channel;
	s32cc_adc_enable_conversion(info, SAR_ADC_CONTINUOUS);

	return 0;
}

static int s32cc_adc_buffer_predisable(struct iio_dev *indio_dev)
{
	struct s32cc_adc *info = iio_priv(indio_dev);
	int mcr_data, ncmr_data, cimr_data;
	int i;

	for (i = 0; i < ADC_NUM_GROUPS; i++) {
		ncmr_data = readl(info->regs + REG_ADC_NCMR(i));
		cimr_data = readl(info->regs + REG_ADC_CIMR(i));

		ncmr_data &= ~ADC_CH_MASK;
		cimr_data &= ~ADC_CIM_MASK;

		writel(ncmr_data, info->regs + REG_ADC_NCMR(i));
		writel(cimr_data, info->regs + REG_ADC_CIMR(i));
	}

	mcr_data = readl(info->regs + REG_ADC_MCR);
	mcr_data &= ~ADC_NSTART;
	mcr_data |= ADC_PWDN;
	writel(mcr_data, info->regs + REG_ADC_MCR);

	return 0;
}

static bool s32cc_adc_validate_scan_mask(struct iio_dev *indio_dev,
					 const unsigned long *mask)
{
	/* SAR_ADC permits any combination of the
	 * available channels to be active
	 */
	return true;
}

static const struct iio_buffer_setup_ops iio_triggered_buffer_setup_ops = {
	.postenable = &s32cc_adc_buffer_postenable,
	.predisable = &s32cc_adc_buffer_predisable,
	.validate_scan_mask = &s32cc_adc_validate_scan_mask,
};

static const struct iio_info s32cc_adc_iio_info = {
	.read_raw = &s32cc_read_raw,
	.write_raw = &s32cc_write_raw,
};

static const struct of_device_id s32cc_adc_match[] = {
	{ .compatible = "nxp,s32cc-adc", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, s32cc_adc_match);

static int s32cc_adc_probe(struct platform_device *pdev)
{
	struct s32cc_adc *info;
	struct iio_dev *indio_dev;
	struct resource *mem;
	int irq;
	int ret;

	indio_dev = devm_iio_device_alloc(&pdev->dev, sizeof(struct s32cc_adc));
	if (!indio_dev) {
		dev_err(&pdev->dev, "Failed allocating iio device\n");
		return -ENOMEM;
	}

	info = iio_priv(indio_dev);
	info->dev = &pdev->dev;

	mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	info->regs = devm_ioremap_resource(&pdev->dev, mem);
	if (IS_ERR(info->regs))
		return PTR_ERR(info->regs);

	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		dev_err(&pdev->dev, "no irq resource?\n");
		return irq;
	}

	ret = devm_request_irq(info->dev, irq,
			       s32cc_adc_isr, 0,
			       dev_name(&pdev->dev), indio_dev);
	if (ret < 0) {
		dev_err(&pdev->dev, "failed requesting irq, irq = %d\n", irq);
		return ret;
	}

	info->clk = devm_clk_get(&pdev->dev, "adc");
	if (IS_ERR(info->clk)) {
		dev_err(&pdev->dev, "failed getting clock, err = %ld\n",
			PTR_ERR(info->clk));
		return PTR_ERR(info->clk);
	}

	if (!pdev->dev.of_node)
		return -EINVAL;

	ret = of_property_read_u32(pdev->dev.of_node, "vref", &info->vref);
	if (ret) {
		dev_err(&pdev->dev, "no vref property in device tree\n");
		return ret;
	}

	platform_set_drvdata(pdev, indio_dev);

	init_completion(&info->completion);

	indio_dev->name = dev_name(&pdev->dev);
	indio_dev->dev.of_node = pdev->dev.of_node;
	indio_dev->info = &s32cc_adc_iio_info;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = s32cc_adc_iio_channels;
	indio_dev->num_channels = ARRAY_SIZE(s32cc_adc_iio_channels);
	info->buffer_ech_num = 0;
	info->channels_used = 0;

	memset(info->buffer, 0, sizeof(info->buffer));
	memset(info->buffered_chan, 0, sizeof(info->buffered_chan));

	ret = clk_prepare_enable(info->clk);
	if (ret) {
		dev_err(&pdev->dev,
			"Could not prepare or enable the clock.\n");
		return ret;
	}

	s32cc_adc_cfg_init(info);
	s32cc_adc_hw_init(info);

	ret = devm_iio_triggered_buffer_setup(&pdev->dev, indio_dev,
					      &iio_pollfunc_store_time, NULL,
					      &iio_triggered_buffer_setup_ops);
	if (ret < 0) {
		dev_err(&pdev->dev, "Couldn't initialise the buffer\n");
		return ret;
	}

	ret = iio_device_register(indio_dev);
	if (ret) {
		dev_err(&pdev->dev, "Couldn't register the device.\n");
		goto error_iio_device_register;
	}

	dev_info(&pdev->dev, "Device initialized successfully.\n");

	return 0;

error_iio_device_register:
	clk_disable_unprepare(info->clk);

	return ret;
}

static int s32cc_adc_remove(struct platform_device *pdev)
{
	struct iio_dev *indio_dev = platform_get_drvdata(pdev);
	struct s32cc_adc *info = iio_priv(indio_dev);

	iio_device_unregister(indio_dev);
	clk_disable_unprepare(info->clk);

	return 0;
}

#ifdef CONFIG_PM_SLEEP
static int s32cc_adc_suspend(struct device *dev)
{
	struct iio_dev *indio_dev = dev_get_drvdata(dev);
	struct s32cc_adc *info = iio_priv(indio_dev);
	int mcr_data;

	/* ADC controller and analog part enter to stop mode */
	mcr_data = readl(info->regs + REG_ADC_MCR);
	mcr_data |= ADC_PWDN;
	writel(mcr_data, info->regs + REG_ADC_MCR);

	clk_disable_unprepare(info->clk);

	return 0;
}

static int s32cc_adc_resume(struct device *dev)
{
	struct iio_dev *indio_dev = dev_get_drvdata(dev);
	struct s32cc_adc *info = iio_priv(indio_dev);
	int ret;

	ret = clk_prepare_enable(info->clk);
	if (ret)
		return ret;

	s32cc_adc_hw_init(info);

	return 0;
}
#endif

static SIMPLE_DEV_PM_OPS(s32cc_adc_pm_ops, s32cc_adc_suspend, s32cc_adc_resume);

static struct platform_driver s32cc_adc_driver = {
	.probe          = s32cc_adc_probe,
	.remove         = s32cc_adc_remove,
	.driver         = {
		.name   = DRIVER_NAME,
		.of_match_table = s32cc_adc_match,
		.pm     = &s32cc_adc_pm_ops,
	},
};

module_platform_driver(s32cc_adc_driver);

MODULE_AUTHOR("NXP");
MODULE_DESCRIPTION("NXP S32CC SAR-ADC driver");
MODULE_LICENSE("GPL v2");
