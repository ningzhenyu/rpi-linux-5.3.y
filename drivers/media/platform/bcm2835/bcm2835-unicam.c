// SPDX-License-Identifier: GPL-2.0-only
/*
 * BCM2835 Unicam capture Driver
 *
 * Copyright (C) 2017 - Raspberry Pi (Trading) Ltd.
 *
 * Dave Stevenson <dave.stevenson@raspberrypi.org>
 *
 * Based on TI am437x driver by Benoit Parrot and Lad, Prabhakar and
 * TI CAL camera interface driver by Benoit Parrot.
 *
 *
 * There are two camera drivers in the kernel for BCM283x - this one
 * and bcm2835-camera (currently in staging).
 *
 * This driver directly controls the Unicam peripheral - there is no
 * involvement with the VideoCore firmware. Unicam receives CSI-2 or
 * CCP2 data and writes it into SDRAM.
 * The only potential processing options are to repack Bayer data into an
 * alternate format, and applying windowing.
 * The repacking does not shift the data, so can repack V4L2_PIX_FMT_Sxxxx10P
 * to V4L2_PIX_FMT_Sxxxx10, or V4L2_PIX_FMT_Sxxxx12P to V4L2_PIX_FMT_Sxxxx12,
 * but not generically up to V4L2_PIX_FMT_Sxxxx16. The driver will add both
 * formats where the relevant formats are defined, and will automatically
 * configure the repacking as required.
 * Support for windowing may be added later.
 *
 * It should be possible to connect this driver to any sensor with a
 * suitable output interface and V4L2 subdevice driver.
 *
 * bcm2835-camera uses the VideoCore firmware to control the sensor,
 * Unicam, ISP, and all tuner control loops. Fully processed frames are
 * delivered to the driver by the firmware. It only has sensor drivers
 * for Omnivision OV5647, and Sony IMX219 sensors.
 *
 * The two drivers are mutually exclusive for the same Unicam instance.
 * The VideoCore firmware checks the device tree configuration during boot.
 * If it finds device tree nodes called csi0 or csi1 it will block the
 * firmware from accessing the peripheral, and bcm2835-camera will
 * not be able to stream data.
 *
 *
 * This program is free software; you may redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/of_graph.h>
#include <linux/pinctrl/consumer.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/videodev2.h>

#include <media/v4l2-common.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-dev.h>
#include <media/v4l2-device.h>
#include <media/v4l2-dv-timings.h>
#include <media/v4l2-event.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-fwnode.h>
#include <media/videobuf2-dma-contig.h>

#include "vc4-regs-unicam.h"

#define UNICAM_MODULE_NAME	"unicam"
#define UNICAM_VERSION		"0.1.0"

static int debug;
module_param(debug, int, 0644);
MODULE_PARM_DESC(debug, "Debug level 0-3");

#define unicam_dbg(level, dev, fmt, arg...)	\
		v4l2_dbg(level, debug, &(dev)->v4l2_dev, fmt, ##arg)
#define unicam_info(dev, fmt, arg...)	\
		v4l2_info(&(dev)->v4l2_dev, fmt, ##arg)
#define unicam_err(dev, fmt, arg...)	\
		v4l2_err(&(dev)->v4l2_dev, fmt, ##arg)

/* To protect against a dodgy sensor driver never returning an error from
 * enum_mbus_code, set a maximum index value to be used.
 */
#define MAX_ENUM_MBUS_CODE	128

/*
 * Stride is a 16 bit register, but also has to be a multiple of 16.
 */
#define BPL_ALIGNMENT		16
#define MAX_BYTESPERLINE	((1 << 16) - BPL_ALIGNMENT)
/*
 * Max width is therefore determined by the max stride divided by
 * the number of bits per pixel. Take 32bpp as a
 * worst case.
 * No imposed limit on the height, so adopt a square image for want
 * of anything better.
 */
#define MAX_WIDTH	(MAX_BYTESPERLINE / 4)
#define MAX_HEIGHT	MAX_WIDTH
/* Define a nominal minimum image size */
#define MIN_WIDTH	16
#define MIN_HEIGHT	16
/*
 * Whilst Unicam doesn't require any additional padding on the image
 * height, various other parts of the BCM283x frameworks require a multiple
 * of 16.
 * Seeing as image buffers are significantly larger than this extra
 * padding, add it in order to simplify integration.
 */
#define HEIGHT_ALIGNMENT	16

/*
 * struct unicam_fmt - Unicam media bus format information
 * @pixelformat: V4L2 pixel format FCC identifier. 0 if n/a.
 * @repacked_fourcc: V4L2 pixel format FCC identifier if the data is expanded
 * out to 16bpp. 0 if n/a.
 * @code: V4L2 media bus format code.
 * @depth: Bits per pixel as delivered from the source.
 * @csi_dt: CSI data type.
 * @check_variants: Flag to denote that there are multiple mediabus formats
 *		still in the list that could match this V4L2 format.
 */
struct unicam_fmt {
	u32	fourcc;
	u32	repacked_fourcc;
	u32	code;
	u8	depth;
	u8	csi_dt;
	u8	check_variants;
};

static const struct unicam_fmt formats[] = {
	/* YUV Formats */
	{
		.fourcc		= V4L2_PIX_FMT_YUYV,
		.code		= MEDIA_BUS_FMT_YUYV8_2X8,
		.depth		= 16,
		.csi_dt		= 0x1e,
		.check_variants = 1,
	}, {
		.fourcc		= V4L2_PIX_FMT_UYVY,
		.code		= MEDIA_BUS_FMT_UYVY8_2X8,
		.depth		= 16,
		.csi_dt		= 0x1e,
		.check_variants = 1,
	}, {
		.fourcc		= V4L2_PIX_FMT_YVYU,
		.code		= MEDIA_BUS_FMT_YVYU8_2X8,
		.depth		= 16,
		.csi_dt		= 0x1e,
		.check_variants = 1,
	}, {
		.fourcc		= V4L2_PIX_FMT_VYUY,
		.code		= MEDIA_BUS_FMT_VYUY8_2X8,
		.depth		= 16,
		.csi_dt		= 0x1e,
		.check_variants = 1,
	}, {
		.fourcc		= V4L2_PIX_FMT_YUYV,
		.code		= MEDIA_BUS_FMT_YUYV8_1X16,
		.depth		= 16,
		.csi_dt		= 0x1e,
	}, {
		.fourcc		= V4L2_PIX_FMT_UYVY,
		.code		= MEDIA_BUS_FMT_UYVY8_1X16,
		.depth		= 16,
		.csi_dt		= 0x1e,
	}, {
		.fourcc		= V4L2_PIX_FMT_YVYU,
		.code		= MEDIA_BUS_FMT_YVYU8_1X16,
		.depth		= 16,
		.csi_dt		= 0x1e,
	}, {
		.fourcc		= V4L2_PIX_FMT_VYUY,
		.code		= MEDIA_BUS_FMT_VYUY8_1X16,
		.depth		= 16,
		.csi_dt		= 0x1e,
	}, {
	/* RGB Formats */
		.fourcc		= V4L2_PIX_FMT_RGB565, /* gggbbbbb rrrrrggg */
		.code		= MEDIA_BUS_FMT_RGB565_2X8_LE,
		.depth		= 16,
		.csi_dt		= 0x22,
	}, {
		.fourcc		= V4L2_PIX_FMT_RGB565X, /* rrrrrggg gggbbbbb */
		.code		= MEDIA_BUS_FMT_RGB565_2X8_BE,
		.depth		= 16,
		.csi_dt		= 0x22
	}, {
		.fourcc		= V4L2_PIX_FMT_RGB555, /* gggbbbbb arrrrrgg */
		.code		= MEDIA_BUS_FMT_RGB555_2X8_PADHI_LE,
		.depth		= 16,
		.csi_dt		= 0x21,
	}, {
		.fourcc		= V4L2_PIX_FMT_RGB555X, /* arrrrrgg gggbbbbb */
		.code		= MEDIA_BUS_FMT_RGB555_2X8_PADHI_BE,
		.depth		= 16,
		.csi_dt		= 0x21,
	}, {
		.fourcc		= V4L2_PIX_FMT_RGB24, /* rgb */
		.code		= MEDIA_BUS_FMT_RGB888_1X24,
		.depth		= 24,
		.csi_dt		= 0x24,
	}, {
		.fourcc		= V4L2_PIX_FMT_BGR24, /* bgr */
		.code		= MEDIA_BUS_FMT_BGR888_1X24,
		.depth		= 24,
		.csi_dt		= 0x24,
	}, {
		.fourcc		= V4L2_PIX_FMT_RGB32, /* argb */
		.code		= MEDIA_BUS_FMT_ARGB8888_1X32,
		.depth		= 32,
		.csi_dt		= 0x0,
	}, {
	/* Bayer Formats */
		.fourcc		= V4L2_PIX_FMT_SBGGR8,
		.code		= MEDIA_BUS_FMT_SBGGR8_1X8,
		.depth		= 8,
		.csi_dt		= 0x2a,
	}, {
		.fourcc		= V4L2_PIX_FMT_SGBRG8,
		.code		= MEDIA_BUS_FMT_SGBRG8_1X8,
		.depth		= 8,
		.csi_dt		= 0x2a,
	}, {
		.fourcc		= V4L2_PIX_FMT_SGRBG8,
		.code		= MEDIA_BUS_FMT_SGRBG8_1X8,
		.depth		= 8,
		.csi_dt		= 0x2a,
	}, {
		.fourcc		= V4L2_PIX_FMT_SRGGB8,
		.code		= MEDIA_BUS_FMT_SRGGB8_1X8,
		.depth		= 8,
		.csi_dt		= 0x2a,
	}, {
		.fourcc		= V4L2_PIX_FMT_SBGGR10P,
		.repacked_fourcc = V4L2_PIX_FMT_SBGGR10,
		.code		= MEDIA_BUS_FMT_SBGGR10_1X10,
		.depth		= 10,
		.csi_dt		= 0x2b,
	}, {
		.fourcc		= V4L2_PIX_FMT_SGBRG10P,
		.repacked_fourcc = V4L2_PIX_FMT_SGBRG10,
		.code		= MEDIA_BUS_FMT_SGBRG10_1X10,
		.depth		= 10,
		.csi_dt		= 0x2b,
	}, {
		.fourcc		= V4L2_PIX_FMT_SGRBG10P,
		.repacked_fourcc = V4L2_PIX_FMT_SGRBG10,
		.code		= MEDIA_BUS_FMT_SGRBG10_1X10,
		.depth		= 10,
		.csi_dt		= 0x2b,
	}, {
		.fourcc		= V4L2_PIX_FMT_SRGGB10P,
		.repacked_fourcc = V4L2_PIX_FMT_SRGGB10,
		.code		= MEDIA_BUS_FMT_SRGGB10_1X10,
		.depth		= 10,
		.csi_dt		= 0x2b,
	}, {
		.fourcc		= V4L2_PIX_FMT_SBGGR12P,
		.repacked_fourcc = V4L2_PIX_FMT_SBGGR12,
		.code		= MEDIA_BUS_FMT_SBGGR12_1X12,
		.depth		= 12,
		.csi_dt		= 0x2c,
	}, {
		.fourcc		= V4L2_PIX_FMT_SGBRG12P,
		.repacked_fourcc = V4L2_PIX_FMT_SGBRG12,
		.code		= MEDIA_BUS_FMT_SGBRG12_1X12,
		.depth		= 12,
		.csi_dt		= 0x2c,
	}, {
		.fourcc		= V4L2_PIX_FMT_SGRBG12P,
		.repacked_fourcc = V4L2_PIX_FMT_SGRBG12,
		.code		= MEDIA_BUS_FMT_SGRBG12_1X12,
		.depth		= 12,
		.csi_dt		= 0x2c,
	}, {
		.fourcc		= V4L2_PIX_FMT_SRGGB12P,
		.repacked_fourcc = V4L2_PIX_FMT_SRGGB12,
		.code		= MEDIA_BUS_FMT_SRGGB12_1X12,
		.depth		= 12,
		.csi_dt		= 0x2c,
	}, {
		.fourcc		= V4L2_PIX_FMT_SBGGR14P,
		.code		= MEDIA_BUS_FMT_SBGGR14_1X14,
		.depth		= 14,
		.csi_dt		= 0x2d,
	}, {
		.fourcc		= V4L2_PIX_FMT_SGBRG14P,
		.code		= MEDIA_BUS_FMT_SGBRG14_1X14,
		.depth		= 14,
		.csi_dt		= 0x2d,
	}, {
		.fourcc		= V4L2_PIX_FMT_SGRBG14P,
		.code		= MEDIA_BUS_FMT_SGRBG14_1X14,
		.depth		= 14,
		.csi_dt		= 0x2d,
	}, {
		.fourcc		= V4L2_PIX_FMT_SRGGB14P,
		.code		= MEDIA_BUS_FMT_SRGGB14_1X14,
		.depth		= 14,
		.csi_dt		= 0x2d,
	}, {
	/*
	 * 16 bit Bayer formats could be supported, but there is no CSI2
	 * data_type defined for raw 16, and no sensors that produce it at
	 * present.
	 */

	/* Greyscale formats */
		.fourcc		= V4L2_PIX_FMT_GREY,
		.code		= MEDIA_BUS_FMT_Y8_1X8,
		.depth		= 8,
		.csi_dt		= 0x2a,
	}, {
		.fourcc		= V4L2_PIX_FMT_Y10P,
		.repacked_fourcc = V4L2_PIX_FMT_Y10,
		.code		= MEDIA_BUS_FMT_Y10_1X10,
		.depth		= 10,
		.csi_dt		= 0x2b,
	}, {
		/* NB There is no packed V4L2 fourcc for this format. */
		.repacked_fourcc = V4L2_PIX_FMT_Y12,
		.code		= MEDIA_BUS_FMT_Y12_1X12,
		.depth		= 12,
		.csi_dt		= 0x2c,
	},
};

struct unicam_dmaqueue {
	struct list_head	active;
};

struct unicam_buffer {
	struct vb2_v4l2_buffer vb;
	struct list_head list;
};

struct unicam_cfg {
	/* peripheral base address */
	void __iomem *base;
	/* clock gating base address */
	void __iomem *clk_gate_base;
};

#define MAX_POSSIBLE_PIX_FMTS (ARRAY_SIZE(formats))

struct unicam_device {
	/* V4l2 specific parameters */
	/* Identifies video device for this channel */
	struct video_device video_dev;
	struct v4l2_ctrl_handler ctrl_handler;

	struct v4l2_fwnode_endpoint endpoint;

	struct v4l2_async_subdev asd;

	/* unicam cfg */
	struct unicam_cfg cfg;
	/* clock handle */
	struct clk *clock;
	/* V4l2 device */
	struct v4l2_device v4l2_dev;
	struct media_device mdev;
	struct media_pad pad;

	/* parent device */
	struct platform_device *pdev;
	/* subdevice async Notifier */
	struct v4l2_async_notifier notifier;
	unsigned int sequence;

	/* ptr to  sub device */
	struct v4l2_subdev *sensor;
	/* Pad config for the sensor */
	struct v4l2_subdev_pad_config *sensor_config;
	/* current input at the sub device */
	int current_input;

	/* Pointer pointing to current v4l2_buffer */
	struct unicam_buffer *cur_frm;
	/* Pointer pointing to next v4l2_buffer */
	struct unicam_buffer *next_frm;

	/* video capture */
	const struct unicam_fmt	*fmt;
	/* Used to store current pixel format */
	struct v4l2_format v_fmt;
	/* Used to store current mbus frame format */
	struct v4l2_mbus_framefmt m_fmt;

	unsigned int virtual_channel;
	enum v4l2_mbus_type bus_type;
	/*
	 * Stores bus.mipi_csi2.flags for CSI2 sensors, or
	 * bus.mipi_csi1.strobe for CCP2.
	 */
	unsigned int bus_flags;
	unsigned int max_data_lanes;
	unsigned int active_data_lanes;

	struct v4l2_rect crop;

	/* Currently selected input on subdev */
	int input;

	/* Buffer queue used in video-buf */
	struct vb2_queue buffer_queue;
	/* Queue of filled frames */
	struct unicam_dmaqueue dma_queue;
	/* IRQ lock for DMA queue */
	spinlock_t dma_queue_lock;
	/* lock used to access this structure */
	struct mutex lock;
	/* Flag to denote that we are processing buffers */
	int streaming;
};

/* Hardware access */
#define clk_write(dev, val) writel((val) | 0x5a000000, (dev)->clk_gate_base)
#define clk_read(dev) readl((dev)->clk_gate_base)

#define reg_read(dev, offset) readl((dev)->base + (offset))
#define reg_write(dev, offset, val) writel(val, (dev)->base + (offset))

#define reg_read_field(dev, offset, mask) get_field(reg_read((dev), (offset), \
						    mask))

static inline int get_field(u32 value, u32 mask)
{
	return (value & mask) >> __ffs(mask);
}

static inline void set_field(u32 *valp, u32 field, u32 mask)
{
	u32 val = *valp;

	val &= ~mask;
	val |= (field << __ffs(mask)) & mask;
	*valp = val;
}

static inline void reg_write_field(struct unicam_cfg *dev, u32 offset,
				   u32 field, u32 mask)
{
	u32 val = reg_read((dev), (offset));

	set_field(&val, field, mask);
	reg_write((dev), (offset), val);
}

/* Power management functions */
static inline int unicam_runtime_get(struct unicam_device *dev)
{
	int r;

	r = pm_runtime_get_sync(&dev->pdev->dev);

	return r;
}

static inline void unicam_runtime_put(struct unicam_device *dev)
{
	pm_runtime_put_sync(&dev->pdev->dev);
}

/* Format setup functions */
static const struct unicam_fmt *find_format_by_code(u32 code)
{
	unsigned int k;

	for (k = 0; k < ARRAY_SIZE(formats); k++) {
		if (formats[k].code == code)
			return &formats[k];
	}

	return NULL;
}

static int check_mbus_format(struct unicam_device *dev,
			     const struct unicam_fmt *format)
{
	struct v4l2_subdev_mbus_code_enum mbus_code;
	int ret = 0;
	int i;

	for (i = 0; !ret && i < MAX_ENUM_MBUS_CODE; i++) {
		memset(&mbus_code, 0, sizeof(mbus_code));
		mbus_code.index = i;

		ret = v4l2_subdev_call(dev->sensor, pad, enum_mbus_code,
				       NULL, &mbus_code);

		if (!ret && mbus_code.code == format->code)
			return 1;
	}

	return 0;
}

static const struct unicam_fmt *find_format_by_pix(struct unicam_device *dev,
						   u32 pixelformat)
{
	unsigned int k;

	for (k = 0; k < ARRAY_SIZE(formats); k++) {
		if (formats[k].fourcc == pixelformat ||
		    formats[k].repacked_fourcc == pixelformat) {
			if (formats[k].check_variants &&
			    !check_mbus_format(dev, &formats[k]))
				continue;
			return &formats[k];
		}
	}

	return NULL;
}

static inline unsigned int bytes_per_line(u32 width,
					  const struct unicam_fmt *fmt,
					  u32 v4l2_fourcc)
{
	if (v4l2_fourcc == fmt->repacked_fourcc)
		/* Repacking always goes to 16bpp */
		return ALIGN(width << 1, BPL_ALIGNMENT);
	else
		return ALIGN((width * fmt->depth) >> 3, BPL_ALIGNMENT);
}

static int __subdev_get_format(struct unicam_device *dev,
			       struct v4l2_mbus_framefmt *fmt)
{
	struct v4l2_subdev_format sd_fmt = {0};
	struct v4l2_mbus_framefmt *mbus_fmt = &sd_fmt.format;
	int ret;

	sd_fmt.which = V4L2_SUBDEV_FORMAT_ACTIVE;
	sd_fmt.pad = 0;

	ret = v4l2_subdev_call(dev->sensor, pad, get_fmt, dev->sensor_config,
			       &sd_fmt);
	if (ret < 0)
		return ret;

	*fmt = *mbus_fmt;

	unicam_dbg(1, dev, "%s %dx%d code:%04x\n", __func__,
		   fmt->width, fmt->height, fmt->code);

	return 0;
}

static int __subdev_set_format(struct unicam_device *dev,
			       struct v4l2_mbus_framefmt *fmt)
{
	struct v4l2_subdev_format sd_fmt = {
		.which = V4L2_SUBDEV_FORMAT_ACTIVE,
	};
	struct v4l2_mbus_framefmt *mbus_fmt = &sd_fmt.format;
	int ret;

	*mbus_fmt = *fmt;

	ret = v4l2_subdev_call(dev->sensor, pad, set_fmt, dev->sensor_config,
			       &sd_fmt);
	if (ret < 0)
		return ret;

	unicam_dbg(1, dev, "%s %dx%d code:%04x\n", __func__,
		   fmt->width, fmt->height, fmt->code);

	return 0;
}

static int unicam_calc_format_size_bpl(struct unicam_device *dev,
				       const struct unicam_fmt *fmt,
				       struct v4l2_format *f)
{
	unsigned int min_bytesperline;

	v4l_bound_align_image(&f->fmt.pix.width, MIN_WIDTH, MAX_WIDTH, 2,
			      &f->fmt.pix.height, MIN_HEIGHT, MAX_HEIGHT, 0,
			      0);

	min_bytesperline = bytes_per_line(f->fmt.pix.width, fmt,
					  f->fmt.pix.pixelformat);

	if (f->fmt.pix.bytesperline > min_bytesperline &&
	    f->fmt.pix.bytesperline <= MAX_BYTESPERLINE)
		f->fmt.pix.bytesperline = ALIGN(f->fmt.pix.bytesperline,
						BPL_ALIGNMENT);
	else
		f->fmt.pix.bytesperline = min_bytesperline;

	/* Align height up for compatibility with other hardware blocks */
	f->fmt.pix.sizeimage = ALIGN(f->fmt.pix.height, HEIGHT_ALIGNMENT) *
			       f->fmt.pix.bytesperline;

	unicam_dbg(3, dev, "%s: fourcc: " V4L2_FOURCC_CONV " size: %dx%d bpl:%d img_size:%d\n",
		   __func__,
		   V4L2_FOURCC_CONV_ARGS(f->fmt.pix.pixelformat),
		   f->fmt.pix.width, f->fmt.pix.height,
		   f->fmt.pix.bytesperline, f->fmt.pix.sizeimage);

	return 0;
}

static int unicam_reset_format(struct unicam_device *dev)
{
	struct v4l2_mbus_framefmt mbus_fmt;
	int ret;

	ret = __subdev_get_format(dev, &mbus_fmt);
	if (ret) {
		unicam_err(dev, "Failed to get_format - ret %d\n", ret);
		return ret;
	}

	if (mbus_fmt.code != dev->fmt->code) {
		unicam_err(dev, "code mismatch - fmt->code %08x, mbus_fmt.code %08x\n",
			   dev->fmt->code, mbus_fmt.code);
		return ret;
	}

	v4l2_fill_pix_format(&dev->v_fmt.fmt.pix, &mbus_fmt);
	dev->v_fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

	unicam_calc_format_size_bpl(dev, dev->fmt, &dev->v_fmt);

	dev->m_fmt = mbus_fmt;

	return 0;
}

static void unicam_wr_dma_addr(struct unicam_device *dev, unsigned int dmaaddr)
{
	unicam_dbg(1, dev, "wr_dma_addr %08x-%08x\n",
		   dmaaddr, dmaaddr + dev->v_fmt.fmt.pix.sizeimage);
	reg_write(&dev->cfg, UNICAM_IBSA0, dmaaddr);
	reg_write(&dev->cfg, UNICAM_IBEA0,
		  dmaaddr + dev->v_fmt.fmt.pix.sizeimage);
}

static inline void unicam_schedule_next_buffer(struct unicam_device *dev)
{
	struct unicam_dmaqueue *dma_q = &dev->dma_queue;
	struct unicam_buffer *buf;
	dma_addr_t addr;

	buf = list_entry(dma_q->active.next, struct unicam_buffer, list);
	dev->next_frm = buf;
	list_del(&buf->list);

	addr = vb2_dma_contig_plane_dma_addr(&buf->vb.vb2_buf, 0);
	unicam_wr_dma_addr(dev, addr);
}

static inline void unicam_process_buffer_complete(struct unicam_device *dev)
{
	dev->cur_frm->vb.field = dev->m_fmt.field;
	dev->cur_frm->vb.sequence = dev->sequence++;

	vb2_buffer_done(&dev->cur_frm->vb.vb2_buf, VB2_BUF_STATE_DONE);
	dev->cur_frm = dev->next_frm;
}

/*
 * unicam_isr : ISR handler for unicam capture
 * @irq: irq number
 * @dev_id: dev_id ptr
 *
 * It changes status of the captured buffer, takes next buffer from the queue
 * and sets its address in unicam registers
 */
static irqreturn_t unicam_isr(int irq, void *dev)
{
	struct unicam_device *unicam = (struct unicam_device *)dev;
	struct unicam_cfg *cfg = &unicam->cfg;
	struct unicam_dmaqueue *dma_q = &unicam->dma_queue;
	int ista, sta;

	/*
	 * Don't service interrupts if not streaming.
	 * Avoids issues if the VPU should enable the
	 * peripheral without the kernel knowing (that
	 * shouldn't happen, but causes issues if it does).
	 */
	if (!unicam->streaming)
		return IRQ_HANDLED;

	sta = reg_read(cfg, UNICAM_STA);
	/* Write value back to clear the interrupts */
	reg_write(cfg, UNICAM_STA, sta);

	ista = reg_read(cfg, UNICAM_ISTA);
	/* Write value back to clear the interrupts */
	reg_write(cfg, UNICAM_ISTA, ista);

	if (!(sta && (UNICAM_IS | UNICAM_PI0)))
		return IRQ_HANDLED;

	if (ista & UNICAM_FSI) {
		/*
		 * Timestamp is to be when the first data byte was captured,
		 * aka frame start.
		 */
		if (unicam->cur_frm)
			unicam->cur_frm->vb.vb2_buf.timestamp = ktime_get_ns();
	}
	if (ista & UNICAM_FEI || sta & UNICAM_PI0) {
		/*
		 * Ensure we have swapped buffers already as we can't
		 * stop the peripheral. Overwrite the frame we've just
		 * captured instead.
		 */
		if (unicam->cur_frm && unicam->cur_frm != unicam->next_frm)
			unicam_process_buffer_complete(unicam);
	}

	if (ista & (UNICAM_FSI | UNICAM_LCI)) {
		spin_lock(&unicam->dma_queue_lock);
		if (!list_empty(&dma_q->active) &&
		    unicam->cur_frm == unicam->next_frm)
			unicam_schedule_next_buffer(unicam);
		spin_unlock(&unicam->dma_queue_lock);
	}

	if (reg_read(&unicam->cfg, UNICAM_ICTL) & UNICAM_FCM) {
		/* Switch out of trigger mode if selected */
		reg_write_field(&unicam->cfg, UNICAM_ICTL, 1, UNICAM_TFC);
		reg_write_field(&unicam->cfg, UNICAM_ICTL, 0, UNICAM_FCM);
	}
	return IRQ_HANDLED;
}

static int unicam_querycap(struct file *file, void *priv,
			   struct v4l2_capability *cap)
{
	struct unicam_device *dev = video_drvdata(file);

	strlcpy(cap->driver, UNICAM_MODULE_NAME, sizeof(cap->driver));
	strlcpy(cap->card, UNICAM_MODULE_NAME, sizeof(cap->card));

	snprintf(cap->bus_info, sizeof(cap->bus_info),
		 "platform:%s", dev->v4l2_dev.name);

	return 0;
}

static int unicam_enum_fmt_vid_cap(struct file *file, void  *priv,
				   struct v4l2_fmtdesc *f)
{
	struct unicam_device *dev = video_drvdata(file);
	struct v4l2_subdev_mbus_code_enum mbus_code;
	const struct unicam_fmt *fmt = NULL;
	int index = 0;
	int ret = 0;
	int i;

	for (i = 0; !ret && i < MAX_ENUM_MBUS_CODE; i++) {
		memset(&mbus_code, 0, sizeof(mbus_code));
		mbus_code.index = i;

		ret = v4l2_subdev_call(dev->sensor, pad, enum_mbus_code,
				       NULL, &mbus_code);
		if (ret < 0) {
			unicam_dbg(2, dev,
				   "subdev->enum_mbus_code idx %d returned %d - index invalid\n",
				   i, ret);
			return -EINVAL;
		}

		fmt = find_format_by_code(mbus_code.code);
		if (fmt) {
			if (fmt->fourcc) {
				if (index == f->index) {
					f->pixelformat = fmt->fourcc;
					break;
				}
				index++;
			}
			if (fmt->repacked_fourcc) {
				if (index == f->index) {
					f->pixelformat = fmt->repacked_fourcc;
					break;
				}
				index++;
			}
		}
	}

	return 0;
}

static int unicam_g_fmt_vid_cap(struct file *file, void *priv,
				struct v4l2_format *f)
{
	struct unicam_device *dev = video_drvdata(file);

	*f = dev->v_fmt;

	return 0;
}

static
const struct unicam_fmt *get_first_supported_format(struct unicam_device *dev)
{
	struct v4l2_subdev_mbus_code_enum mbus_code;
	const struct unicam_fmt *fmt = NULL;
	int ret;
	int j;

	for (j = 0; ret != -EINVAL && ret != -ENOIOCTLCMD; ++j) {
		memset(&mbus_code, 0, sizeof(mbus_code));
		mbus_code.index = j;
		ret = v4l2_subdev_call(dev->sensor, pad, enum_mbus_code, NULL,
				       &mbus_code);
		if (ret < 0) {
			unicam_dbg(2, dev,
				   "subdev->enum_mbus_code idx %d returned %d - continue\n",
				   j, ret);
			continue;
		}

		unicam_dbg(2, dev, "subdev %s: code: %04x idx: %d\n",
			   dev->sensor->name, mbus_code.code, j);

		fmt = find_format_by_code(mbus_code.code);
		unicam_dbg(2, dev, "fmt %04x returned as %p, V4L2 FOURCC %04x, csi_dt %02X\n",
			   mbus_code.code, fmt, fmt ? fmt->fourcc : 0,
			   fmt ? fmt->csi_dt : 0);
		if (fmt)
			return fmt;
	}

	return NULL;
}
static int unicam_try_fmt_vid_cap(struct file *file, void *priv,
				  struct v4l2_format *f)
{
	struct unicam_device *dev = video_drvdata(file);
	const struct unicam_fmt *fmt;
	struct v4l2_subdev_format sd_fmt = {
		.which = V4L2_SUBDEV_FORMAT_TRY,
	};
	struct v4l2_mbus_framefmt *mbus_fmt = &sd_fmt.format;
	int ret;

	fmt = find_format_by_pix(dev, f->fmt.pix.pixelformat);
	if (!fmt) {
		/* Pixel format not supported by unicam. Choose the first
		 * supported format, and let the sensor choose something else.
		 */
		unicam_dbg(3, dev, "Fourcc format (0x%08x) not found. Use first format.\n",
			   f->fmt.pix.pixelformat);

		fmt = &formats[0];
		f->fmt.pix.pixelformat = fmt->fourcc;
	}

	v4l2_fill_mbus_format(mbus_fmt, &f->fmt.pix, fmt->code);
	/*
	 * No support for receiving interlaced video, so never
	 * request it from the sensor subdev.
	 */
	mbus_fmt->field = V4L2_FIELD_NONE;

	ret = v4l2_subdev_call(dev->sensor, pad, set_fmt, dev->sensor_config,
			       &sd_fmt);
	if (ret && ret != -ENOIOCTLCMD && ret != -ENODEV)
		return ret;

	if (mbus_fmt->field != V4L2_FIELD_NONE)
		unicam_info(dev, "Sensor trying to send interlaced video - results may be unpredictable\n");

	v4l2_fill_pix_format(&f->fmt.pix, &sd_fmt.format);
	if (mbus_fmt->code != fmt->code) {
		/* Sensor has returned an alternate format */
		fmt = find_format_by_code(mbus_fmt->code);
		if (!fmt) {
			/* The alternate format is one unicam can't support.
			 * Find the first format that is supported by both, and
			 * then set that.
			 */
			fmt = get_first_supported_format(dev);
			mbus_fmt->code = fmt->code;

			ret = v4l2_subdev_call(dev->sensor, pad, set_fmt,
					       dev->sensor_config, &sd_fmt);
			if (ret && ret != -ENOIOCTLCMD && ret != -ENODEV)
				return ret;

			if (mbus_fmt->field != V4L2_FIELD_NONE)
				unicam_info(dev, "Sensor trying to send interlaced video - results may be unpredictable\n");

			v4l2_fill_pix_format(&f->fmt.pix, &sd_fmt.format);

			if (mbus_fmt->code != fmt->code) {
				/* We've set a format that the sensor reports
				 * as being supported, but it refuses to set it.
				 * Not much else we can do.
				 * Assume that the sensor driver may accept the
				 * format when it is set (rather than tried).
				 */
				unicam_err(dev, "Sensor won't accept default format, and Unicam can't support sensor default\n");
			}
		}

		if (fmt->fourcc)
			f->fmt.pix.pixelformat = fmt->fourcc;
		else
			f->fmt.pix.pixelformat = fmt->repacked_fourcc;
	}

	return unicam_calc_format_size_bpl(dev, fmt, f);
}

static int unicam_s_fmt_vid_cap(struct file *file, void *priv,
				struct v4l2_format *f)
{
	struct unicam_device *dev = video_drvdata(file);
	struct vb2_queue *q = &dev->buffer_queue;
	const struct unicam_fmt *fmt;
	struct v4l2_mbus_framefmt mbus_fmt = {0};
	int ret;

	if (vb2_is_busy(q))
		return -EBUSY;

	ret = unicam_try_fmt_vid_cap(file, priv, f);
	if (ret < 0)
		return ret;

	fmt = find_format_by_pix(dev, f->fmt.pix.pixelformat);
	if (!fmt) {
		/* Unknown pixel format - adopt a default.
		 * This shouldn't happen as try_fmt should have resolved any
		 * issues first.
		 */
		fmt = get_first_supported_format(dev);
		if (!fmt)
			/* It shouldn't be possible to get here with no
			 * supported formats
			 */
			return -EINVAL;
		f->fmt.pix.pixelformat = fmt->fourcc;
		return -EINVAL;
	}

	v4l2_fill_mbus_format(&mbus_fmt, &f->fmt.pix, fmt->code);

	ret = __subdev_set_format(dev, &mbus_fmt);
	if (ret) {
		unicam_dbg(3, dev, "%s __subdev_set_format failed %d\n",
			   __func__, ret);
		return ret;
	}

	/* Just double check nothing has gone wrong */
	if (mbus_fmt.code != fmt->code) {
		unicam_dbg(3, dev,
			   "%s subdev changed format on us, this should not happen\n",
			   __func__);
		return -EINVAL;
	}

	dev->fmt = fmt;
	dev->v_fmt.fmt.pix.pixelformat = f->fmt.pix.pixelformat;
	dev->v_fmt.fmt.pix.bytesperline = f->fmt.pix.bytesperline;
	unicam_reset_format(dev);

	unicam_dbg(3, dev, "%s %dx%d, mbus_fmt %08X, V4L2 pix " V4L2_FOURCC_CONV ".\n",
		   __func__, dev->v_fmt.fmt.pix.width,
		   dev->v_fmt.fmt.pix.height, mbus_fmt.code,
		   V4L2_FOURCC_CONV_ARGS(dev->v_fmt.fmt.pix.pixelformat));

	*f = dev->v_fmt;

	return 0;
}

static int unicam_queue_setup(struct vb2_queue *vq,
			      unsigned int *nbuffers,
			      unsigned int *nplanes,
			      unsigned int sizes[],
			      struct device *alloc_devs[])
{
	struct unicam_device *dev = vb2_get_drv_priv(vq);
	unsigned int size = dev->v_fmt.fmt.pix.sizeimage;

	if (vq->num_buffers + *nbuffers < 3)
		*nbuffers = 3 - vq->num_buffers;

	if (*nplanes) {
		if (sizes[0] < size) {
			unicam_err(dev, "sizes[0] %i < size %u\n", sizes[0],
				   size);
			return -EINVAL;
		}
		size = sizes[0];
	}

	*nplanes = 1;
	sizes[0] = size;

	return 0;
}

static int unicam_buffer_prepare(struct vb2_buffer *vb)
{
	struct unicam_device *dev = vb2_get_drv_priv(vb->vb2_queue);
	struct unicam_buffer *buf = container_of(vb, struct unicam_buffer,
					      vb.vb2_buf);
	unsigned long size;

	if (WARN_ON(!dev->fmt))
		return -EINVAL;

	size = dev->v_fmt.fmt.pix.sizeimage;
	if (vb2_plane_size(vb, 0) < size) {
		unicam_err(dev, "data will not fit into plane (%lu < %lu)\n",
			   vb2_plane_size(vb, 0), size);
		return -EINVAL;
	}

	vb2_set_plane_payload(&buf->vb.vb2_buf, 0, size);
	return 0;
}

static void unicam_buffer_queue(struct vb2_buffer *vb)
{
	struct unicam_device *dev = vb2_get_drv_priv(vb->vb2_queue);
	struct unicam_buffer *buf = container_of(vb, struct unicam_buffer,
					      vb.vb2_buf);
	struct unicam_dmaqueue *dma_queue = &dev->dma_queue;
	unsigned long flags = 0;

	/* recheck locking */
	spin_lock_irqsave(&dev->dma_queue_lock, flags);
	list_add_tail(&buf->list, &dma_queue->active);
	spin_unlock_irqrestore(&dev->dma_queue_lock, flags);
}

static void unicam_wr_dma_config(struct unicam_device *dev,
				 unsigned int stride)
{
	reg_write(&dev->cfg, UNICAM_IBLS, stride);
}

static void unicam_set_packing_config(struct unicam_device *dev)
{
	int pack, unpack;
	u32 val;

	if (dev->v_fmt.fmt.pix.pixelformat == dev->fmt->fourcc) {
		unpack = UNICAM_PUM_NONE;
		pack = UNICAM_PPM_NONE;
	} else {
		switch (dev->fmt->depth) {
		case 8:
			unpack = UNICAM_PUM_UNPACK8;
			break;
		case 10:
			unpack = UNICAM_PUM_UNPACK10;
			break;
		case 12:
			unpack = UNICAM_PUM_UNPACK12;
			break;
		case 14:
			unpack = UNICAM_PUM_UNPACK14;
			break;
		case 16:
			unpack = UNICAM_PUM_UNPACK16;
			break;
		default:
			unpack = UNICAM_PUM_NONE;
			break;
		}

		/* Repacking is always to 16bpp */
		pack = UNICAM_PPM_PACK16;
	}

	val = 0;
	set_field(&val, 2, UNICAM_DEBL_MASK);
	set_field(&val, unpack, UNICAM_PUM_MASK);
	set_field(&val, pack, UNICAM_PPM_MASK);
	reg_write(&dev->cfg, UNICAM_IPIPE, val);
}

static void unicam_cfg_image_id(struct unicam_device *dev)
{
	struct unicam_cfg *cfg = &dev->cfg;

	if (dev->bus_type == V4L2_MBUS_CSI2_DPHY) {
		/* CSI2 mode */
		reg_write(cfg, UNICAM_IDI0,
			  (dev->virtual_channel << 6) | dev->fmt->csi_dt);
	} else {
		/* CCP2 mode */
		reg_write(cfg, UNICAM_IDI0, (0x80 | dev->fmt->csi_dt));
	}
}

static void unicam_start_rx(struct unicam_device *dev, unsigned long addr)
{
	struct unicam_cfg *cfg = &dev->cfg;
	int line_int_freq = dev->v_fmt.fmt.pix.height >> 2;
	unsigned int i;
	u32 val;

	if (line_int_freq < 128)
		line_int_freq = 128;

	/* Enable lane clocks */
	val = 1;
	for (i = 0; i < dev->active_data_lanes; i++)
		val = val << 2 | 1;
	clk_write(cfg, val);

	/* Basic init */
	reg_write(cfg, UNICAM_CTRL, UNICAM_MEM);

	/* Enable analogue control, and leave in reset. */
	val = UNICAM_AR;
	set_field(&val, 7, UNICAM_CTATADJ_MASK);
	set_field(&val, 7, UNICAM_PTATADJ_MASK);
	reg_write(cfg, UNICAM_ANA, val);
	usleep_range(1000, 2000);

	/* Come out of reset */
	reg_write_field(cfg, UNICAM_ANA, 0, UNICAM_AR);

	/* Peripheral reset */
	reg_write_field(cfg, UNICAM_CTRL, 1, UNICAM_CPR);
	reg_write_field(cfg, UNICAM_CTRL, 0, UNICAM_CPR);

	reg_write_field(cfg, UNICAM_CTRL, 0, UNICAM_CPE);

	/* Enable Rx control. */
	val = reg_read(cfg, UNICAM_CTRL);
	if (dev->bus_type == V4L2_MBUS_CSI2_DPHY) {
		set_field(&val, UNICAM_CPM_CSI2, UNICAM_CPM_MASK);
		set_field(&val, UNICAM_DCM_STROBE, UNICAM_DCM_MASK);
	} else {
		set_field(&val, UNICAM_CPM_CCP2, UNICAM_CPM_MASK);
		set_field(&val, dev->bus_flags, UNICAM_DCM_MASK);
	}
	/* Packet framer timeout */
	set_field(&val, 0xf, UNICAM_PFT_MASK);
	set_field(&val, 128, UNICAM_OET_MASK);
	reg_write(cfg, UNICAM_CTRL, val);

	reg_write(cfg, UNICAM_IHWIN, 0);
	reg_write(cfg, UNICAM_IVWIN, 0);

	/* AXI bus access QoS setup */
	val = reg_read(&dev->cfg, UNICAM_PRI);
	set_field(&val, 0, UNICAM_BL_MASK);
	set_field(&val, 0, UNICAM_BS_MASK);
	set_field(&val, 0xe, UNICAM_PP_MASK);
	set_field(&val, 8, UNICAM_NP_MASK);
	set_field(&val, 2, UNICAM_PT_MASK);
	set_field(&val, 1, UNICAM_PE);
	reg_write(cfg, UNICAM_PRI, val);

	reg_write_field(cfg, UNICAM_ANA, 0, UNICAM_DDL);

	/* Always start in trigger frame capture mode (UNICAM_FCM set) */
	val = UNICAM_FSIE | UNICAM_FEIE | UNICAM_FCM;
	set_field(&val,  line_int_freq, UNICAM_LCIE_MASK);
	reg_write(cfg, UNICAM_ICTL, val);
	reg_write(cfg, UNICAM_STA, UNICAM_STA_MASK_ALL);
	reg_write(cfg, UNICAM_ISTA, UNICAM_ISTA_MASK_ALL);

	/* tclk_term_en */
	reg_write_field(cfg, UNICAM_CLT, 2, UNICAM_CLT1_MASK);
	/* tclk_settle */
	reg_write_field(cfg, UNICAM_CLT, 6, UNICAM_CLT2_MASK);
	/* td_term_en */
	reg_write_field(cfg, UNICAM_DLT, 2, UNICAM_DLT1_MASK);
	/* ths_settle */
	reg_write_field(cfg, UNICAM_DLT, 6, UNICAM_DLT2_MASK);
	/* trx_enable */
	reg_write_field(cfg, UNICAM_DLT, 0, UNICAM_DLT3_MASK);

	reg_write_field(cfg, UNICAM_CTRL, 0, UNICAM_SOE);

	/* Packet compare setup - required to avoid missing frame ends */
	val = 0;
	set_field(&val, 1, UNICAM_PCE);
	set_field(&val, 1, UNICAM_GI);
	set_field(&val, 1, UNICAM_CPH);
	set_field(&val, 0, UNICAM_PCVC_MASK);
	set_field(&val, 1, UNICAM_PCDT_MASK);
	reg_write(cfg, UNICAM_CMP0, val);

	/* Enable clock lane and set up terminations */
	val = 0;
	if (dev->bus_type == V4L2_MBUS_CSI2_DPHY) {
		/* CSI2 */
		set_field(&val, 1, UNICAM_CLE);
		set_field(&val, 1, UNICAM_CLLPE);
		if (dev->bus_flags & V4L2_MBUS_CSI2_CONTINUOUS_CLOCK) {
			set_field(&val, 1, UNICAM_CLTRE);
			set_field(&val, 1, UNICAM_CLHSE);
		}
	} else {
		/* CCP2 */
		set_field(&val, 1, UNICAM_CLE);
		set_field(&val, 1, UNICAM_CLHSE);
		set_field(&val, 1, UNICAM_CLTRE);
	}
	reg_write(cfg, UNICAM_CLK, val);

	/*
	 * Enable required data lanes with appropriate terminations.
	 * The same value needs to be written to UNICAM_DATn registers for
	 * the active lanes, and 0 for inactive ones.
	 */
	val = 0;
	if (dev->bus_type == V4L2_MBUS_CSI2_DPHY) {
		/* CSI2 */
		set_field(&val, 1, UNICAM_DLE);
		set_field(&val, 1, UNICAM_DLLPE);
		if (dev->bus_flags & V4L2_MBUS_CSI2_CONTINUOUS_CLOCK) {
			set_field(&val, 1, UNICAM_DLTRE);
			set_field(&val, 1, UNICAM_DLHSE);
		}
	} else {
		/* CCP2 */
		set_field(&val, 1, UNICAM_DLE);
		set_field(&val, 1, UNICAM_DLHSE);
		set_field(&val, 1, UNICAM_DLTRE);
	}
	reg_write(cfg, UNICAM_DAT0, val);

	if (dev->active_data_lanes == 1)
		val = 0;
	reg_write(cfg, UNICAM_DAT1, val);

	if (dev->max_data_lanes > 2) {
		/*
		 * Registers UNICAM_DAT2 and UNICAM_DAT3 only valid if the
		 * instance supports more than 2 data lanes.
		 */
		if (dev->active_data_lanes == 2)
			val = 0;
		reg_write(cfg, UNICAM_DAT2, val);

		if (dev->active_data_lanes == 3)
			val = 0;
		reg_write(cfg, UNICAM_DAT3, val);
	}

	unicam_wr_dma_config(dev, dev->v_fmt.fmt.pix.bytesperline);
	unicam_wr_dma_addr(dev, addr);
	unicam_set_packing_config(dev);
	unicam_cfg_image_id(dev);

	/* Disabled embedded data */
	val = 0;
	set_field(&val, 0, UNICAM_EDL_MASK);
	reg_write(cfg, UNICAM_DCS, val);

	val = reg_read(cfg, UNICAM_MISC);
	set_field(&val, 1, UNICAM_FL0);
	set_field(&val, 1, UNICAM_FL1);
	reg_write(cfg, UNICAM_MISC, val);

	/* Enable peripheral */
	reg_write_field(cfg, UNICAM_CTRL, 1, UNICAM_CPE);

	/* Load image pointers */
	reg_write_field(cfg, UNICAM_ICTL, 1, UNICAM_LIP_MASK);

	/*
	 * Enable trigger only for the first frame to
	 * sync correctly to the FS from the source.
	 */
	reg_write_field(cfg, UNICAM_ICTL, 1, UNICAM_TFC);
}

static void unicam_disable(struct unicam_device *dev)
{
	struct unicam_cfg *cfg = &dev->cfg;

	/* Analogue lane control disable */
	reg_write_field(cfg, UNICAM_ANA, 1, UNICAM_DDL);

	/* Stop the output engine */
	reg_write_field(cfg, UNICAM_CTRL, 1, UNICAM_SOE);

	/* Disable the data lanes. */
	reg_write(cfg, UNICAM_DAT0, 0);
	reg_write(cfg, UNICAM_DAT1, 0);

	if (dev->max_data_lanes > 2) {
		reg_write(cfg, UNICAM_DAT2, 0);
		reg_write(cfg, UNICAM_DAT3, 0);
	}

	/* Peripheral reset */
	reg_write_field(cfg, UNICAM_CTRL, 1, UNICAM_CPR);
	usleep_range(50, 100);
	reg_write_field(cfg, UNICAM_CTRL, 0, UNICAM_CPR);

	/* Disable peripheral */
	reg_write_field(cfg, UNICAM_CTRL, 0, UNICAM_CPE);

	/* Disable all lane clocks */
	clk_write(cfg, 0);
}

static int unicam_start_streaming(struct vb2_queue *vq, unsigned int count)
{
	struct unicam_device *dev = vb2_get_drv_priv(vq);
	struct unicam_dmaqueue *dma_q = &dev->dma_queue;
	struct unicam_buffer *buf, *tmp;
	unsigned long addr = 0;
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&dev->dma_queue_lock, flags);
	buf = list_entry(dma_q->active.next, struct unicam_buffer, list);
	dev->cur_frm = buf;
	dev->next_frm = buf;
	list_del(&buf->list);
	spin_unlock_irqrestore(&dev->dma_queue_lock, flags);

	addr = vb2_dma_contig_plane_dma_addr(&dev->cur_frm->vb.vb2_buf, 0);
	dev->sequence = 0;

	ret = unicam_runtime_get(dev);
	if (ret < 0) {
		unicam_dbg(3, dev, "unicam_runtime_get failed\n");
		goto err_release_buffers;
	}

	dev->active_data_lanes = dev->max_data_lanes;
	if (dev->bus_type == V4L2_MBUS_CSI2_DPHY &&
	    v4l2_subdev_has_op(dev->sensor, video, g_mbus_config)) {
		struct v4l2_mbus_config mbus_config;

		ret = v4l2_subdev_call(dev->sensor, video, g_mbus_config,
				       &mbus_config);
		if (ret < 0) {
			unicam_dbg(3, dev, "g_mbus_config failed\n");
			goto err_pm_put;
		}

		dev->active_data_lanes =
			(mbus_config.flags & V4L2_MBUS_CSI2_LANE_MASK) >>
					__ffs(V4L2_MBUS_CSI2_LANE_MASK);
		if (!dev->active_data_lanes)
			dev->active_data_lanes = dev->max_data_lanes;
	}
	if (dev->active_data_lanes > dev->max_data_lanes) {
		unicam_err(dev, "Device has requested %u data lanes, which is >%u configured in DT\n",
			   dev->active_data_lanes, dev->max_data_lanes);
		ret = -EINVAL;
		goto err_pm_put;
	}

	unicam_dbg(1, dev, "Running with %u data lanes\n",
		   dev->active_data_lanes);

	ret = clk_set_rate(dev->clock, 100 * 1000 * 1000);
	if (ret) {
		unicam_err(dev, "failed to set up clock\n");
		goto err_pm_put;
	}

	ret = clk_prepare_enable(dev->clock);
	if (ret) {
		unicam_err(dev, "Failed to enable CSI clock: %d\n", ret);
		goto err_pm_put;
	}
	dev->streaming = 1;

	unicam_start_rx(dev, addr);

	ret = v4l2_subdev_call(dev->sensor, video, s_stream, 1);
	if (ret < 0) {
		unicam_err(dev, "stream on failed in subdev\n");
		goto err_disable_unicam;
	}

	return 0;

err_disable_unicam:
	unicam_disable(dev);
	clk_disable_unprepare(dev->clock);
err_pm_put:
	unicam_runtime_put(dev);
err_release_buffers:
	list_for_each_entry_safe(buf, tmp, &dma_q->active, list) {
		list_del(&buf->list);
		vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_QUEUED);
	}
	if (dev->cur_frm != dev->next_frm)
		vb2_buffer_done(&dev->next_frm->vb.vb2_buf,
				VB2_BUF_STATE_QUEUED);
	vb2_buffer_done(&dev->cur_frm->vb.vb2_buf, VB2_BUF_STATE_QUEUED);
	dev->next_frm = NULL;
	dev->cur_frm = NULL;

	return ret;
}

static void unicam_stop_streaming(struct vb2_queue *vq)
{
	struct unicam_device *dev = vb2_get_drv_priv(vq);
	struct unicam_dmaqueue *dma_q = &dev->dma_queue;
	struct unicam_buffer *buf, *tmp;
	unsigned long flags;

	if (v4l2_subdev_call(dev->sensor, video, s_stream, 0) < 0)
		unicam_err(dev, "stream off failed in subdev\n");

	unicam_disable(dev);

	/* Release all active buffers */
	spin_lock_irqsave(&dev->dma_queue_lock, flags);
	list_for_each_entry_safe(buf, tmp, &dma_q->active, list) {
		list_del(&buf->list);
		vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_ERROR);
	}

	if (dev->cur_frm == dev->next_frm) {
		vb2_buffer_done(&dev->cur_frm->vb.vb2_buf, VB2_BUF_STATE_ERROR);
	} else {
		vb2_buffer_done(&dev->cur_frm->vb.vb2_buf, VB2_BUF_STATE_ERROR);
		vb2_buffer_done(&dev->next_frm->vb.vb2_buf,
				VB2_BUF_STATE_ERROR);
	}
	dev->cur_frm = NULL;
	dev->next_frm = NULL;
	spin_unlock_irqrestore(&dev->dma_queue_lock, flags);

	clk_disable_unprepare(dev->clock);
	unicam_runtime_put(dev);
}

static int unicam_enum_input(struct file *file, void *priv,
			     struct v4l2_input *inp)
{
	struct unicam_device *dev = video_drvdata(file);

	if (inp->index != 0)
		return -EINVAL;

	inp->type = V4L2_INPUT_TYPE_CAMERA;
	if (v4l2_subdev_has_op(dev->sensor, video, s_dv_timings)) {
		inp->capabilities = V4L2_IN_CAP_DV_TIMINGS;
		inp->std = 0;
	} else if (v4l2_subdev_has_op(dev->sensor, video, s_std)) {
		inp->capabilities = V4L2_IN_CAP_STD;
		if (v4l2_subdev_call(dev->sensor, video, g_tvnorms, &inp->std)
					< 0)
			inp->std = V4L2_STD_ALL;
	} else {
		inp->capabilities = 0;
		inp->std = 0;
	}
	sprintf(inp->name, "Camera 0");
	return 0;
}

static int unicam_g_input(struct file *file, void *priv, unsigned int *i)
{
	*i = 0;

	return 0;
}

static int unicam_s_input(struct file *file, void *priv, unsigned int i)
{
	/*
	 * FIXME: Ideally we would like to be able to query the source
	 * subdevice for information over the input connectors it supports,
	 * and map that through in to a call to video_ops->s_routing.
	 * There is no infrastructure support for defining that within
	 * devicetree at present. Until that is implemented we can't
	 * map a user physical connector number to s_routing input number.
	 */
	if (i > 0)
		return -EINVAL;

	return 0;
}

static int unicam_querystd(struct file *file, void *priv,
			   v4l2_std_id *std)
{
	struct unicam_device *dev = video_drvdata(file);

	return v4l2_subdev_call(dev->sensor, video, querystd, std);
}

static int unicam_g_std(struct file *file, void *priv, v4l2_std_id *std)
{
	struct unicam_device *dev = video_drvdata(file);

	return v4l2_subdev_call(dev->sensor, video, g_std, std);
}

static int unicam_s_std(struct file *file, void *priv, v4l2_std_id std)
{
	struct unicam_device *dev = video_drvdata(file);
	int ret;
	v4l2_std_id current_std;

	ret = v4l2_subdev_call(dev->sensor, video, g_std, &current_std);
	if (ret)
		return ret;

	if (std == current_std)
		return 0;

	if (vb2_is_busy(&dev->buffer_queue))
		return -EBUSY;

	ret = v4l2_subdev_call(dev->sensor, video, s_std, std);

	/* Force recomputation of bytesperline */
	dev->v_fmt.fmt.pix.bytesperline = 0;

	unicam_reset_format(dev);

	return ret;
}

static int unicam_s_edid(struct file *file, void *priv, struct v4l2_edid *edid)
{
	struct unicam_device *dev = video_drvdata(file);

	return v4l2_subdev_call(dev->sensor, pad, set_edid, edid);
}

static int unicam_g_edid(struct file *file, void *priv, struct v4l2_edid *edid)
{
	struct unicam_device *dev = video_drvdata(file);

	return v4l2_subdev_call(dev->sensor, pad, get_edid, edid);
}

static int unicam_enum_framesizes(struct file *file, void *priv,
				  struct v4l2_frmsizeenum *fsize)
{
	struct unicam_device *dev = video_drvdata(file);
	const struct unicam_fmt *fmt;
	struct v4l2_subdev_frame_size_enum fse;
	int ret;

	/* check for valid format */
	fmt = find_format_by_pix(dev, fsize->pixel_format);
	if (!fmt) {
		unicam_dbg(3, dev, "Invalid pixel code: %x\n",
			   fsize->pixel_format);
		return -EINVAL;
	}

	fse.index = fsize->index;
	fse.pad = 0;
	fse.code = fmt->code;

	ret = v4l2_subdev_call(dev->sensor, pad, enum_frame_size, NULL, &fse);
	if (ret)
		return ret;

	unicam_dbg(1, dev, "%s: index: %d code: %x W:[%d,%d] H:[%d,%d]\n",
		   __func__, fse.index, fse.code, fse.min_width, fse.max_width,
		   fse.min_height, fse.max_height);

	fsize->type = V4L2_FRMSIZE_TYPE_DISCRETE;
	fsize->discrete.width = fse.max_width;
	fsize->discrete.height = fse.max_height;

	return 0;
}

static int unicam_enum_frameintervals(struct file *file, void *priv,
				      struct v4l2_frmivalenum *fival)
{
	struct unicam_device *dev = video_drvdata(file);
	const struct unicam_fmt *fmt;
	struct v4l2_subdev_frame_interval_enum fie = {
		.index = fival->index,
		.width = fival->width,
		.height = fival->height,
		.which = V4L2_SUBDEV_FORMAT_ACTIVE,
	};
	int ret;

	fmt = find_format_by_pix(dev, fival->pixel_format);
	if (!fmt)
		return -EINVAL;

	fie.code = fmt->code;
	ret = v4l2_subdev_call(dev->sensor, pad, enum_frame_interval,
			       NULL, &fie);
	if (ret)
		return ret;

	fival->type = V4L2_FRMIVAL_TYPE_DISCRETE;
	fival->discrete = fie.interval;

	return 0;
}

static int unicam_g_parm(struct file *file, void *fh, struct v4l2_streamparm *a)
{
	struct unicam_device *dev = video_drvdata(file);

	return v4l2_g_parm_cap(video_devdata(file), dev->sensor, a);
}

static int unicam_s_parm(struct file *file, void *fh, struct v4l2_streamparm *a)
{
	struct unicam_device *dev = video_drvdata(file);

	return v4l2_s_parm_cap(video_devdata(file), dev->sensor, a);
}

static int unicam_g_dv_timings(struct file *file, void *priv,
			       struct v4l2_dv_timings *timings)
{
	struct unicam_device *dev = video_drvdata(file);

	return v4l2_subdev_call(dev->sensor, video, g_dv_timings, timings);
}

static int unicam_s_dv_timings(struct file *file, void *priv,
			       struct v4l2_dv_timings *timings)
{
	struct unicam_device *dev = video_drvdata(file);
	struct v4l2_dv_timings current_timings;
	int ret;

	ret = v4l2_subdev_call(dev->sensor, video, g_dv_timings,
			       &current_timings);

	if (v4l2_match_dv_timings(timings, &current_timings, 0, false))
		return 0;

	if (vb2_is_busy(&dev->buffer_queue))
		return -EBUSY;

	ret = v4l2_subdev_call(dev->sensor, video, s_dv_timings, timings);

	/* Force recomputation of bytesperline */
	dev->v_fmt.fmt.pix.bytesperline = 0;

	unicam_reset_format(dev);

	return ret;
}

static int unicam_query_dv_timings(struct file *file, void *priv,
				   struct v4l2_dv_timings *timings)
{
	struct unicam_device *dev = video_drvdata(file);

	return v4l2_subdev_call(dev->sensor, video, query_dv_timings, timings);
}

static int unicam_enum_dv_timings(struct file *file, void *priv,
				  struct v4l2_enum_dv_timings *timings)
{
	struct unicam_device *dev = video_drvdata(file);

	return v4l2_subdev_call(dev->sensor, pad, enum_dv_timings, timings);
}

static int unicam_dv_timings_cap(struct file *file, void *priv,
				 struct v4l2_dv_timings_cap *cap)
{
	struct unicam_device *dev = video_drvdata(file);

	return v4l2_subdev_call(dev->sensor, pad, dv_timings_cap, cap);
}

static int unicam_subscribe_event(struct v4l2_fh *fh,
				  const struct v4l2_event_subscription *sub)
{
	switch (sub->type) {
	case V4L2_EVENT_SOURCE_CHANGE:
		return v4l2_event_subscribe(fh, sub, 4, NULL);
	}

	return v4l2_ctrl_subscribe_event(fh, sub);
}

static int unicam_log_status(struct file *file, void *fh)
{
	struct unicam_device *dev = video_drvdata(file);
	struct unicam_cfg *cfg = &dev->cfg;
	u32 reg;

	/* status for sub devices */
	v4l2_device_call_all(&dev->v4l2_dev, 0, core, log_status);

	unicam_info(dev, "-----Receiver status-----\n");
	unicam_info(dev, "V4L2 width/height:   %ux%u\n",
		    dev->v_fmt.fmt.pix.width, dev->v_fmt.fmt.pix.height);
	unicam_info(dev, "Mediabus format:     %08x\n", dev->fmt->code);
	unicam_info(dev, "V4L2 format:         " V4L2_FOURCC_CONV "\n",
		    V4L2_FOURCC_CONV_ARGS(dev->v_fmt.fmt.pix.pixelformat));
	reg = reg_read(&dev->cfg, UNICAM_IPIPE);
	unicam_info(dev, "Unpacking/packing:   %u / %u\n",
		    get_field(reg, UNICAM_PUM_MASK),
		    get_field(reg, UNICAM_PPM_MASK));
	unicam_info(dev, "----Live data----\n");
	unicam_info(dev, "Programmed stride:   %4u\n",
		    reg_read(cfg, UNICAM_IBLS));
	unicam_info(dev, "Detected resolution: %ux%u\n",
		    reg_read(cfg, UNICAM_IHSTA),
		    reg_read(cfg, UNICAM_IVSTA));
	unicam_info(dev, "Write pointer:       %08x\n",
		    reg_read(cfg, UNICAM_IBWP));

	return 0;
}

static void unicam_notify(struct v4l2_subdev *sd,
			  unsigned int notification, void *arg)
{
	struct unicam_device *dev =
		container_of(sd->v4l2_dev, struct unicam_device, v4l2_dev);

	switch (notification) {
	case V4L2_DEVICE_NOTIFY_EVENT:
		v4l2_event_queue(&dev->video_dev, arg);
		break;
	default:
		break;
	}
}

static const struct vb2_ops unicam_video_qops = {
	.wait_prepare		= vb2_ops_wait_prepare,
	.wait_finish		= vb2_ops_wait_finish,
	.queue_setup		= unicam_queue_setup,
	.buf_prepare		= unicam_buffer_prepare,
	.buf_queue		= unicam_buffer_queue,
	.start_streaming	= unicam_start_streaming,
	.stop_streaming		= unicam_stop_streaming,
};

/*
 * unicam_open : This function is based on the v4l2_fh_open helper function.
 * It has been augmented to handle sensor subdevice power management,
 */
static int unicam_open(struct file *file)
{
	struct unicam_device *dev = video_drvdata(file);
	int ret;

	mutex_lock(&dev->lock);

	ret = v4l2_fh_open(file);
	if (ret) {
		unicam_err(dev, "v4l2_fh_open failed\n");
		goto unlock;
	}

	if (!v4l2_fh_is_singular_file(file))
		goto unlock;

	ret = v4l2_subdev_call(dev->sensor, core, s_power, 1);
	if (ret < 0 && ret != -ENOIOCTLCMD) {
		v4l2_fh_release(file);
		goto unlock;
	}

unlock:
	mutex_unlock(&dev->lock);
	return ret;
}

static int unicam_release(struct file *file)
{
	struct unicam_device *dev = video_drvdata(file);
	struct v4l2_subdev *sd = dev->sensor;
	bool fh_singular;
	int ret;

	mutex_lock(&dev->lock);

	fh_singular = v4l2_fh_is_singular_file(file);

	ret = _vb2_fop_release(file, NULL);

	if (fh_singular)
		v4l2_subdev_call(sd, core, s_power, 0);

	mutex_unlock(&dev->lock);

	return ret;
}

/* unicam capture driver file operations */
static const struct v4l2_file_operations unicam_fops = {
	.owner		= THIS_MODULE,
	.open           = unicam_open,
	.release        = unicam_release,
	.read		= vb2_fop_read,
	.poll		= vb2_fop_poll,
	.unlocked_ioctl	= video_ioctl2,
	.mmap		= vb2_fop_mmap,
};

/* unicam capture ioctl operations */
static const struct v4l2_ioctl_ops unicam_ioctl_ops = {
	.vidioc_querycap		= unicam_querycap,
	.vidioc_enum_fmt_vid_cap	= unicam_enum_fmt_vid_cap,
	.vidioc_g_fmt_vid_cap		= unicam_g_fmt_vid_cap,
	.vidioc_s_fmt_vid_cap		= unicam_s_fmt_vid_cap,
	.vidioc_try_fmt_vid_cap		= unicam_try_fmt_vid_cap,

	.vidioc_enum_input		= unicam_enum_input,
	.vidioc_g_input			= unicam_g_input,
	.vidioc_s_input			= unicam_s_input,

	.vidioc_querystd		= unicam_querystd,
	.vidioc_s_std			= unicam_s_std,
	.vidioc_g_std			= unicam_g_std,

	.vidioc_g_edid			= unicam_g_edid,
	.vidioc_s_edid			= unicam_s_edid,

	.vidioc_enum_framesizes		= unicam_enum_framesizes,
	.vidioc_enum_frameintervals	= unicam_enum_frameintervals,

	.vidioc_g_parm			= unicam_g_parm,
	.vidioc_s_parm			= unicam_s_parm,

	.vidioc_s_dv_timings		= unicam_s_dv_timings,
	.vidioc_g_dv_timings		= unicam_g_dv_timings,
	.vidioc_query_dv_timings	= unicam_query_dv_timings,
	.vidioc_enum_dv_timings		= unicam_enum_dv_timings,
	.vidioc_dv_timings_cap		= unicam_dv_timings_cap,

	.vidioc_reqbufs			= vb2_ioctl_reqbufs,
	.vidioc_create_bufs		= vb2_ioctl_create_bufs,
	.vidioc_prepare_buf		= vb2_ioctl_prepare_buf,
	.vidioc_querybuf		= vb2_ioctl_querybuf,
	.vidioc_qbuf			= vb2_ioctl_qbuf,
	.vidioc_dqbuf			= vb2_ioctl_dqbuf,
	.vidioc_expbuf			= vb2_ioctl_expbuf,
	.vidioc_streamon		= vb2_ioctl_streamon,
	.vidioc_streamoff		= vb2_ioctl_streamoff,

	.vidioc_log_status		= unicam_log_status,
	.vidioc_subscribe_event		= unicam_subscribe_event,
	.vidioc_unsubscribe_event	= v4l2_event_unsubscribe,
};

static int
unicam_async_bound(struct v4l2_async_notifier *notifier,
		   struct v4l2_subdev *subdev,
		   struct v4l2_async_subdev *asd)
{
	struct unicam_device *unicam = container_of(notifier->v4l2_dev,
					       struct unicam_device, v4l2_dev);

	if (unicam->sensor) {
		unicam_info(unicam, "Rejecting subdev %s (Already set!!)",
			    subdev->name);
		return 0;
	}

	unicam->sensor = subdev;
	unicam_dbg(1, unicam, "Using sensor %s for capture\n", subdev->name);

	return 0;
}

static int unicam_probe_complete(struct unicam_device *unicam)
{
	struct video_device *vdev;
	struct vb2_queue *q;
	struct v4l2_mbus_framefmt mbus_fmt = {0};
	const struct unicam_fmt *fmt;
	int ret;

	v4l2_set_subdev_hostdata(unicam->sensor, unicam);

	unicam->v4l2_dev.notify = unicam_notify;

	unicam->sensor_config = v4l2_subdev_alloc_pad_config(unicam->sensor);
	if (!unicam->sensor_config)
		return -ENOMEM;

	ret = __subdev_get_format(unicam, &mbus_fmt);
	if (ret) {
		unicam_err(unicam, "Failed to get_format - ret %d\n", ret);
		return ret;
	}

	fmt = find_format_by_code(mbus_fmt.code);
	if (!fmt) {
		/* Find the first format that the sensor and unicam both
		 * support
		 */
		fmt = get_first_supported_format(unicam);

		if (!fmt)
			/* No compatible formats */
			return -EINVAL;

		mbus_fmt.code = fmt->code;
		ret = __subdev_set_format(unicam, &mbus_fmt);
		if (ret)
			return -EINVAL;
	}
	if (mbus_fmt.field != V4L2_FIELD_NONE) {
		/* Interlaced not supported - disable it now. */
		mbus_fmt.field = V4L2_FIELD_NONE;
		ret = __subdev_set_format(unicam, &mbus_fmt);
		if (ret)
			return -EINVAL;
	}

	unicam->fmt = fmt;
	if (fmt->fourcc)
		unicam->v_fmt.fmt.pix.pixelformat = fmt->fourcc;
	else
		unicam->v_fmt.fmt.pix.pixelformat = fmt->repacked_fourcc;

	/* Read current subdev format */
	unicam_reset_format(unicam);

	if (v4l2_subdev_has_op(unicam->sensor, video, s_std)) {
		v4l2_std_id tvnorms;

		if (WARN_ON(!v4l2_subdev_has_op(unicam->sensor, video,
						g_tvnorms)))
			/*
			 * Subdevice should not advertise s_std but not
			 * g_tvnorms
			 */
			return -EINVAL;

		ret = v4l2_subdev_call(unicam->sensor, video,
				       g_tvnorms, &tvnorms);
		if (WARN_ON(ret))
			return -EINVAL;
		unicam->video_dev.tvnorms |= tvnorms;
	}

	spin_lock_init(&unicam->dma_queue_lock);
	mutex_init(&unicam->lock);

	/* Add controls from the subdevice */
	ret = v4l2_ctrl_add_handler(&unicam->ctrl_handler,
				    unicam->sensor->ctrl_handler, NULL, true);
	if (ret < 0)
		return ret;

	q = &unicam->buffer_queue;
	q->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	q->io_modes = VB2_MMAP | VB2_DMABUF | VB2_READ;
	q->drv_priv = unicam;
	q->ops = &unicam_video_qops;
	q->mem_ops = &vb2_dma_contig_memops;
	q->buf_struct_size = sizeof(struct unicam_buffer);
	q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	q->lock = &unicam->lock;
	q->min_buffers_needed = 2;
	q->dev = &unicam->pdev->dev;

	ret = vb2_queue_init(q);
	if (ret) {
		unicam_err(unicam, "vb2_queue_init() failed\n");
		return ret;
	}

	INIT_LIST_HEAD(&unicam->dma_queue.active);

	vdev = &unicam->video_dev;
	strlcpy(vdev->name, UNICAM_MODULE_NAME, sizeof(vdev->name));
	vdev->release = video_device_release_empty;
	vdev->fops = &unicam_fops;
	vdev->ioctl_ops = &unicam_ioctl_ops;
	vdev->v4l2_dev = &unicam->v4l2_dev;
	vdev->vfl_dir = VFL_DIR_RX;
	vdev->queue = q;
	vdev->lock = &unicam->lock;
	vdev->device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING |
			    V4L2_CAP_READWRITE;

	/* If the source has no controls then remove our ctrl handler. */
	if (list_empty(&unicam->ctrl_handler.ctrls))
		unicam->v4l2_dev.ctrl_handler = NULL;

	video_set_drvdata(vdev, unicam);
	vdev->entity.flags |= MEDIA_ENT_FL_DEFAULT;

	ret = video_register_device(vdev, VFL_TYPE_GRABBER, -1);
	if (ret) {
		unicam_err(unicam, "Unable to register video device.\n");
		return ret;
	}

	if (!v4l2_subdev_has_op(unicam->sensor, video, s_std)) {
		v4l2_disable_ioctl(&unicam->video_dev, VIDIOC_S_STD);
		v4l2_disable_ioctl(&unicam->video_dev, VIDIOC_G_STD);
		v4l2_disable_ioctl(&unicam->video_dev, VIDIOC_ENUMSTD);
	}
	if (!v4l2_subdev_has_op(unicam->sensor, video, querystd))
		v4l2_disable_ioctl(&unicam->video_dev, VIDIOC_QUERYSTD);
	if (!v4l2_subdev_has_op(unicam->sensor, video, s_dv_timings)) {
		v4l2_disable_ioctl(&unicam->video_dev, VIDIOC_S_EDID);
		v4l2_disable_ioctl(&unicam->video_dev, VIDIOC_G_EDID);
		v4l2_disable_ioctl(&unicam->video_dev, VIDIOC_DV_TIMINGS_CAP);
		v4l2_disable_ioctl(&unicam->video_dev, VIDIOC_G_DV_TIMINGS);
		v4l2_disable_ioctl(&unicam->video_dev, VIDIOC_S_DV_TIMINGS);
		v4l2_disable_ioctl(&unicam->video_dev, VIDIOC_ENUM_DV_TIMINGS);
		v4l2_disable_ioctl(&unicam->video_dev, VIDIOC_QUERY_DV_TIMINGS);
	}
	if (!v4l2_subdev_has_op(unicam->sensor, pad, enum_frame_interval))
		v4l2_disable_ioctl(&unicam->video_dev,
				   VIDIOC_ENUM_FRAMEINTERVALS);
	if (!v4l2_subdev_has_op(unicam->sensor, video, g_frame_interval))
		v4l2_disable_ioctl(&unicam->video_dev, VIDIOC_G_PARM);
	if (!v4l2_subdev_has_op(unicam->sensor, video, s_frame_interval))
		v4l2_disable_ioctl(&unicam->video_dev, VIDIOC_S_PARM);

	if (!v4l2_subdev_has_op(unicam->sensor, pad, enum_frame_size))
		v4l2_disable_ioctl(&unicam->video_dev, VIDIOC_ENUM_FRAMESIZES);

	ret = v4l2_device_register_subdev_nodes(&unicam->v4l2_dev);
	if (ret) {
		unicam_err(unicam,
			   "Unable to register subdev nodes.\n");
		video_unregister_device(&unicam->video_dev);
		return ret;
	}

	ret = media_create_pad_link(&unicam->sensor->entity, 0,
				    &unicam->video_dev.entity, 0,
				    MEDIA_LNK_FL_ENABLED |
				    MEDIA_LNK_FL_IMMUTABLE);
	if (ret) {
		unicam_err(unicam, "Unable to create pad links.\n");
		video_unregister_device(&unicam->video_dev);
		return ret;
	}

	return 0;
}

static int unicam_async_complete(struct v4l2_async_notifier *notifier)
{
	struct unicam_device *unicam = container_of(notifier->v4l2_dev,
					struct unicam_device, v4l2_dev);

	return unicam_probe_complete(unicam);
}

static const struct v4l2_async_notifier_operations unicam_async_ops = {
	.bound = unicam_async_bound,
	.complete = unicam_async_complete,
};

static int of_unicam_connect_subdevs(struct unicam_device *dev)
{
	struct platform_device *pdev = dev->pdev;
	struct device_node *parent, *ep_node = NULL, *remote_ep = NULL,
			*sensor_node = NULL;
	struct v4l2_fwnode_endpoint *ep;
	struct v4l2_async_subdev *asd;
	unsigned int peripheral_data_lanes;
	int ret = -EINVAL;
	unsigned int lane;

	parent = pdev->dev.of_node;

	asd = &dev->asd;
	ep = &dev->endpoint;

	ep_node = of_graph_get_next_endpoint(parent, NULL);
	if (!ep_node) {
		unicam_dbg(3, dev, "can't get next endpoint\n");
		goto cleanup_exit;
	}

	unicam_dbg(3, dev, "ep_node is %s\n", ep_node->name);

	v4l2_fwnode_endpoint_parse(of_fwnode_handle(ep_node), ep);

	for (lane = 0; lane < ep->bus.mipi_csi2.num_data_lanes; lane++) {
		if (ep->bus.mipi_csi2.data_lanes[lane] != lane + 1) {
			unicam_err(dev, "Local endpoint - data lane reordering not supported\n");
			goto cleanup_exit;
		}
	}

	peripheral_data_lanes = ep->bus.mipi_csi2.num_data_lanes;

	sensor_node = of_graph_get_remote_port_parent(ep_node);
	if (!sensor_node) {
		unicam_dbg(3, dev, "can't get remote parent\n");
		goto cleanup_exit;
	}
	unicam_dbg(3, dev, "sensor_node is %s\n", sensor_node->name);
	asd->match_type = V4L2_ASYNC_MATCH_FWNODE;
	asd->match.fwnode = of_fwnode_handle(sensor_node);

	remote_ep = of_graph_get_remote_endpoint(ep_node);
	if (!remote_ep) {
		unicam_dbg(3, dev, "can't get remote-endpoint\n");
		goto cleanup_exit;
	}
	unicam_dbg(3, dev, "remote_ep is %s\n", remote_ep->name);
	v4l2_fwnode_endpoint_parse(of_fwnode_handle(remote_ep), ep);
	unicam_dbg(3, dev, "parsed remote_ep to endpoint. nr_of_link_frequencies %u, bus_type %u\n",
		   ep->nr_of_link_frequencies, ep->bus_type);

	switch (ep->bus_type) {
	case V4L2_MBUS_CSI2_DPHY:
		if (ep->bus.mipi_csi2.num_data_lanes >
				peripheral_data_lanes) {
			unicam_err(dev, "Subdevice %s wants too many data lanes (%u > %u)\n",
				   sensor_node->name,
				   ep->bus.mipi_csi2.num_data_lanes,
				   peripheral_data_lanes);
			goto cleanup_exit;
		}
		for (lane = 0;
		     lane < ep->bus.mipi_csi2.num_data_lanes;
		     lane++) {
			if (ep->bus.mipi_csi2.data_lanes[lane] != lane + 1) {
				unicam_err(dev, "Subdevice %s - incompatible data lane config\n",
					   sensor_node->name);
				goto cleanup_exit;
			}
		}
		dev->max_data_lanes = ep->bus.mipi_csi2.num_data_lanes;
		dev->bus_flags = ep->bus.mipi_csi2.flags;
		break;
	case V4L2_MBUS_CCP2:
		if (ep->bus.mipi_csi1.clock_lane != 0 ||
		    ep->bus.mipi_csi1.data_lane != 1) {
			unicam_err(dev, "Subdevice %s incompatible lane config\n",
				   sensor_node->name);
			goto cleanup_exit;
		}
		dev->max_data_lanes = 1;
		dev->bus_flags = ep->bus.mipi_csi1.strobe;
		break;
	default:
		/* Unsupported bus type */
		unicam_err(dev, "sub-device %s is not a CSI2 or CCP2 device %d\n",
			   sensor_node->name, ep->bus_type);
		goto cleanup_exit;
	}

	/* Store bus type - CSI2 or CCP2 */
	dev->bus_type = ep->bus_type;
	unicam_dbg(3, dev, "bus_type is %d\n", dev->bus_type);

	/* Store Virtual Channel number */
	dev->virtual_channel = ep->base.id;

	unicam_dbg(3, dev, "v4l2-endpoint: %s\n",
		   dev->bus_type == V4L2_MBUS_CSI2_DPHY ? "CSI2" : "CCP2");
	unicam_dbg(3, dev, "Virtual Channel=%d\n", dev->virtual_channel);
	if (dev->bus_type == V4L2_MBUS_CSI2_DPHY)
		unicam_dbg(3, dev, "flags=0x%08x\n", ep->bus.mipi_csi2.flags);
	unicam_dbg(3, dev, "num_data_lanes=%d\n", dev->max_data_lanes);

	unicam_dbg(1, dev, "found sub-device %s\n", sensor_node->name);

	v4l2_async_notifier_init(&dev->notifier);

	ret = v4l2_async_notifier_add_subdev(&dev->notifier, asd);
	if (ret) {
		unicam_err(dev, "Error adding subdevice - ret %d\n", ret);
		goto cleanup_exit;
	}

	dev->notifier.ops = &unicam_async_ops;
	ret = v4l2_async_notifier_register(&dev->v4l2_dev,
					   &dev->notifier);
	if (ret) {
		unicam_err(dev, "Error registering async notifier - ret %d\n",
			   ret);
		ret = -EINVAL;
	}

cleanup_exit:
	if (remote_ep)
		of_node_put(remote_ep);
	if (sensor_node)
		of_node_put(sensor_node);
	if (ep_node)
		of_node_put(ep_node);

	return ret;
}

static int unicam_probe(struct platform_device *pdev)
{
	struct unicam_cfg *unicam_cfg;
	struct unicam_device *unicam;
	struct v4l2_ctrl_handler *hdl;
	struct resource	*res;
	int ret;

	unicam = devm_kzalloc(&pdev->dev, sizeof(*unicam), GFP_KERNEL);
	if (!unicam)
		return -ENOMEM;

	unicam->pdev = pdev;
	unicam_cfg = &unicam->cfg;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	unicam_cfg->base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(unicam_cfg->base)) {
		unicam_err(unicam, "Failed to get main io block\n");
		return PTR_ERR(unicam_cfg->base);
	}

	res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	unicam_cfg->clk_gate_base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(unicam_cfg->clk_gate_base)) {
		unicam_err(unicam, "Failed to get 2nd io block\n");
		return PTR_ERR(unicam_cfg->clk_gate_base);
	}

	unicam->clock = devm_clk_get(&pdev->dev, "lp");
	if (IS_ERR(unicam->clock)) {
		unicam_err(unicam, "Failed to get clock\n");
		return PTR_ERR(unicam->clock);
	}

	ret = platform_get_irq(pdev, 0);
	if (ret <= 0) {
		dev_err(&pdev->dev, "No IRQ resource\n");
		return -ENODEV;
	}

	ret = devm_request_irq(&pdev->dev, ret, unicam_isr, 0,
			       "unicam_capture0", unicam);
	if (ret) {
		dev_err(&pdev->dev, "Unable to request interrupt\n");
		return -EINVAL;
	}

	unicam->mdev.dev = &pdev->dev;
	strscpy(unicam->mdev.model, UNICAM_MODULE_NAME,
		sizeof(unicam->mdev.model));
	strscpy(unicam->mdev.serial, "", sizeof(unicam->mdev.serial));
	snprintf(unicam->mdev.bus_info, sizeof(unicam->mdev.bus_info),
		 "platform:%s", pdev->name);
	unicam->mdev.hw_revision = 1;

	media_entity_pads_init(&unicam->video_dev.entity, 1, &unicam->pad);
	media_device_init(&unicam->mdev);

	unicam->v4l2_dev.mdev = &unicam->mdev;

	ret = v4l2_device_register(&pdev->dev, &unicam->v4l2_dev);
	if (ret) {
		unicam_err(unicam,
			   "Unable to register v4l2 device.\n");
		goto media_cleanup;
	}

	ret = media_device_register(&unicam->mdev);
	if (ret < 0) {
		unicam_err(unicam,
			   "Unable to register media-controller device.\n");
		goto probe_out_v4l2_unregister;
	}

	/* Reserve space for the controls */
	hdl = &unicam->ctrl_handler;
	ret = v4l2_ctrl_handler_init(hdl, 16);
	if (ret < 0)
		goto media_unregister;
	unicam->v4l2_dev.ctrl_handler = hdl;

	/* set the driver data in platform device */
	platform_set_drvdata(pdev, unicam);

	ret = of_unicam_connect_subdevs(unicam);
	if (ret) {
		dev_err(&pdev->dev, "Failed to connect subdevs\n");
		goto free_hdl;
	}

	/* Enable the block power domain */
	pm_runtime_enable(&pdev->dev);

	return 0;

free_hdl:
	v4l2_ctrl_handler_free(hdl);
media_unregister:
	media_device_unregister(&unicam->mdev);
probe_out_v4l2_unregister:
	v4l2_device_unregister(&unicam->v4l2_dev);
media_cleanup:
	media_device_cleanup(&unicam->mdev);

	return ret;
}

static int unicam_remove(struct platform_device *pdev)
{
	struct unicam_device *unicam = platform_get_drvdata(pdev);

	unicam_dbg(2, unicam, "%s\n", __func__);

	pm_runtime_disable(&pdev->dev);

	v4l2_async_notifier_unregister(&unicam->notifier);
	v4l2_ctrl_handler_free(&unicam->ctrl_handler);
	v4l2_device_unregister(&unicam->v4l2_dev);
	video_unregister_device(&unicam->video_dev);
	if (unicam->sensor_config)
		v4l2_subdev_free_pad_config(unicam->sensor_config);
	media_device_unregister(&unicam->mdev);
	media_device_cleanup(&unicam->mdev);

	return 0;
}

static const struct of_device_id unicam_of_match[] = {
	{ .compatible = "brcm,bcm2835-unicam", },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, unicam_of_match);

static struct platform_driver unicam_driver = {
	.probe		= unicam_probe,
	.remove		= unicam_remove,
	.driver = {
		.name	= UNICAM_MODULE_NAME,
		.of_match_table = of_match_ptr(unicam_of_match),
	},
};

module_platform_driver(unicam_driver);

MODULE_AUTHOR("Dave Stevenson <dave.stevenson@raspberrypi.org>");
MODULE_DESCRIPTION("BCM2835 Unicam driver");
MODULE_LICENSE("GPL");
MODULE_VERSION(UNICAM_VERSION);
