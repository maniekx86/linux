// SPDX-License-Identifier: GPL-2.0-or-later
#include <drm/drm_fourcc.h>
#include <linux/clk.h>
#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/of_device.h>
#include <linux/of_graph.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/slab.h>

#include <uapi/drm/mstar_ge.h>

#define DRIVER_NAME "mstar-ge"

#define REG_CTRL		0x0
#define REG_CTRL1		0x4
#define REG_CMQ_STATUS		0x1c
#define REG_IRQ			0x78
#define REG_SRCL		0x80
#define REG_SRCH		0x84
#define REG_DSTL		0x98
#define REG_DSTH		0x9c
/* This seems to be for descriptors in DRAM */
#define REG_VCMQ_BASE		0xa0
#define REG_BLENDFLAGS		0xac
#define REG_CONST		0xb0
#define REG_P256_GB		0xb4
#define REG_P256_AR		0xb8
#define REG_P256_INDEX		0xbc
#define REG_SRCPITCH		0xc0
#define REG_TAGL		0xc4
#define REG_TAGH		0xc8
#define REG_DSTPITCH		0xcc
#define REG_COLORFMT		0xd0
#define REG_I0_C		0xd4
#define REG_I1_C		0xdc
#define REG_CLIP_LEFT		0x154
#define REG_CLIP_RIGHT		0x158
#define REG_CLIP_TOP		0x15c
#define REG_CLIP_BOTTOM		0x160
#define REG_ROT			0x164
#define REG_CMD			0x180
#define REG_LINE_CTRL0		0x184
#define REG_LINE_CTRL1		0x188
#define REG_LINE_LENGTH		0x18c
#define REG_X0			0x1a0
#define REG_Y0			0x1a4
#define REG_X1			0x1a8
#define REG_Y1			0x1ac
#define REG_X2			0x1b0
#define REG_Y2			0x1b4
#define REG_BITBLT_SRCWIDTH	0x1b8
#define REG_BITBLT_SRCHEIGHT	0x1bc
#define REG_BG_ST		0x1c0
#define REG_RA_ST		0x1c4

static const struct reg_field en_field = REG_FIELD(REG_CTRL, 0, 0);
static const struct reg_field abl_field = REG_FIELD(REG_CTRL, 2, 2);
static const struct reg_field dfb_field = REG_FIELD(REG_CTRL, 10, 10);
static const struct reg_field clk_en_field = REG_FIELD(REG_CTRL1, 15, 15);

static const struct reg_field gebusy_field = REG_FIELD(REG_CMQ_STATUS, 0, 0);
static const struct reg_field cmq_free_field = REG_FIELD(REG_CMQ_STATUS, 3, 7);
static const struct reg_field cmq2_free_field = REG_FIELD(REG_CMQ_STATUS, 11, 15);

static const struct reg_field irq_mask_field = REG_FIELD(REG_IRQ, 6, 7);
static const struct reg_field irq_force_field = REG_FIELD(REG_IRQ, 8, 9);
static const struct reg_field irq_clr_field = REG_FIELD(REG_IRQ, 10, 11);
static const struct reg_field irq_status_field = REG_FIELD(REG_IRQ, 12, 13);

/* src buffer */
static const struct reg_field srcl_field = REG_FIELD(REG_SRCL, 0, 15);
static const struct reg_field srch_field = REG_FIELD(REG_SRCH, 0, 12);

/* dst buffer */
static const struct reg_field dstl_field = REG_FIELD(REG_DSTL, 0, 15);
static const struct reg_field dsth_field = REG_FIELD(REG_DSTH, 0, 12);

/* blend flags */
static const struct reg_field bld_coloralpha_field	= REG_FIELD(REG_BLENDFLAGS, 0, 0);
static const struct reg_field bld_alphachan_field	= REG_FIELD(REG_BLENDFLAGS, 1, 1);
static const struct reg_field bld_colorize_field	= REG_FIELD(REG_BLENDFLAGS, 2, 2);
static const struct reg_field bld_srcpremul_field	= REG_FIELD(REG_BLENDFLAGS, 3, 3);
static const struct reg_field bld_srcpremulcol_field	= REG_FIELD(REG_BLENDFLAGS, 4, 4);
static const struct reg_field bld_dstpremul_field	= REG_FIELD(REG_BLENDFLAGS, 5, 5);
static const struct reg_field bld_xor_field		= REG_FIELD(REG_BLENDFLAGS, 6, 6);
static const struct reg_field bld_demultiply_field	= REG_FIELD(REG_BLENDFLAGS, 7, 7);

/* const */
static const struct reg_field const_b_field		= REG_FIELD(REG_BLENDFLAGS, 8, 15);
static const struct reg_field const_g_field		= REG_FIELD(REG_CONST, 0, 8);
static const struct reg_field const_r_field		= REG_FIELD(REG_CONST, 8, 15);

static const struct reg_field srcpitch_field = REG_FIELD(REG_SRCPITCH, 0, 13);
static const struct reg_field dstpitch_field = REG_FIELD(REG_DSTPITCH, 0, 13);

static const struct reg_field src_colorfmt_field = REG_FIELD(REG_COLORFMT, 0, 4);
static const struct reg_field dst_colorfmt_field = REG_FIELD(REG_COLORFMT, 8, 12);
#define COLOR_FORMAT_RGB565	0x8
#define COLOR_FORMAT_ARGB8888	0xf

static const struct reg_field clip_left_field = REG_FIELD(REG_CLIP_LEFT, 0, 11);
static const struct reg_field clip_right_field = REG_FIELD(REG_CLIP_RIGHT, 0, 11);
static const struct reg_field clip_top_field = REG_FIELD(REG_CLIP_TOP, 0, 11);
static const struct reg_field clip_bottom_field = REG_FIELD(REG_CLIP_BOTTOM, 0, 11);

static const struct reg_field rot_field = REG_FIELD(REG_ROT, 0, 1);

static const struct reg_field prim_type_field = REG_FIELD(REG_CMD, 4, 6);
#define PRIM_TYPE_LINE		1
#define PRIM_TYPE_RECTFILL	3
#define PRIM_TYPE_BITBLT	4
#define PRIM_TYPE_FREEZE	7

static const struct reg_field line_delta_field = REG_FIELD(REG_LINE_CTRL0, 1, 14);
static const struct reg_field line_major_field = REG_FIELD(REG_LINE_CTRL0, 15, 15);
static const struct reg_field line_last_field = REG_FIELD(REG_LINE_CTRL1, 9, 9);
static const struct reg_field line_length_field = REG_FIELD(REG_LINE_LENGTH, 0, 11);

static const struct reg_field x0_field = REG_FIELD(REG_X0, 0, 11);
static const struct reg_field y0_field = REG_FIELD(REG_Y0, 0, 11);
static const struct reg_field x1_field = REG_FIELD(REG_X1, 0, 11);
static const struct reg_field y1_field = REG_FIELD(REG_Y1, 0, 11);
static const struct reg_field x2_field = REG_FIELD(REG_X2, 0, 11);
static const struct reg_field y2_field = REG_FIELD(REG_Y2, 0, 11);

static const struct reg_field bitblt_srcwidth_field = REG_FIELD(REG_BITBLT_SRCWIDTH, 0, 11);
static const struct reg_field bitblt_srcheight_field = REG_FIELD(REG_BITBLT_SRCHEIGHT, 0, 11);

static const struct reg_field b_st_field = REG_FIELD(REG_BG_ST, 0, 7);
static const struct reg_field g_st_field = REG_FIELD(REG_BG_ST, 8, 15);
static const struct reg_field r_st_field = REG_FIELD(REG_RA_ST, 0, 7);
static const struct reg_field a_st_field = REG_FIELD(REG_RA_ST, 8, 15);

static const struct reg_field tagh_field = REG_FIELD(REG_TAGL, 0, 15);
static const struct reg_field tagl_field = REG_FIELD(REG_TAGH, 0, 15);

static const struct reg_field p256_b_field = REG_FIELD(REG_P256_GB, 0, 7);
static const struct reg_field p256_g_field = REG_FIELD(REG_P256_GB, 8, 15);
static const struct reg_field p256_r_field = REG_FIELD(REG_P256_AR, 0, 7);
static const struct reg_field p256_a_field = REG_FIELD(REG_P256_AR, 8, 15);
static const struct reg_field p256_index_field = REG_FIELD(REG_P256_INDEX, 0, 7);
static const struct reg_field p256_rw_field = REG_FIELD(REG_P256_INDEX, 8, 8);

struct mstar_ge {
	struct device *dev;
	struct drm_device *drm_device;
	struct clk *clk;
	u32 tag;
	struct regmap_field *en, *abl, *dfb, *clk_en, *busy;
	struct regmap_field *cmq_free, *cmq2_free;
	struct regmap_field *irq_mask, *irq_force, *irq_clr, *irq_status;
	struct regmap_field *srcl, *srch;
	struct regmap_field *dstl, *dsth;

	struct regmap_field *bld_alphachan;

	struct regmap_field *srcpitch, *dstpitch;
	struct regmap_field *srcclrfmt, *dstclrfmt;
	struct regmap_field *clip_left, *clip_right, *clip_top, *clip_bottom;
	struct regmap_field *rot;
	struct regmap_field *prim_type;
	struct regmap_field *line_delta, *line_major, *line_last, *line_length;
	struct regmap_field *x0, *y0, *x1, *y1, *x2, *y2;
	struct regmap_field *bitblt_src_width, *bitblt_src_height;
	struct regmap_field *tagl, *tagh;

	/* p256 */
	struct regmap_field *p256_b, *p256_g, *p256_r, *p256_a, *p256_index, *p256_rw;

	struct regmap_field *b_st, *g_st, *r_st, *a_st;

	struct list_head queue;

	struct miscdevice ge_dev;

	struct kmem_cache *jobs;
};

#define mstar_ge_buf_sz(b) (b->cfg.pitch * b->cfg.height)

static bool mstar_ge_volatile_reg(struct device *dev, unsigned int reg)
{
	//printk("%s:%x\n", __func__, reg);

	switch(reg) {
	case REG_IRQ:
	case REG_CMD:
		return true;
	default:
		return false;
	}
}

static const struct regmap_config mstar_ge_regmap_config = {
	.reg_bits = 16,
	.val_bits = 16,
	.reg_stride = 4,
	/*
	 * Using the cache works around the fact that
	 * some registers cannot be written and readback.
	 * This allows fields to work properly.
	 */
	.volatile_reg = mstar_ge_volatile_reg,
	.max_register = 0x200,
	.cache_type = REGCACHE_RBTREE,
};

struct mstar_ge_dma_buf {
	struct dma_buf *dma_buf;
	struct dma_buf_attachment *dma_attachment;
};

/* Everything needed to do a single operation */
struct mstar_ge_job {
	unsigned long tag;

	struct mstar_ge_opdata opdata;

	/* src and dst buffer handling */
	struct mstar_ge_buf_cfg src_cfg, dst_cfg;
	dma_addr_t src_addr, dst_addr;

	/* DMA_BUF stuff */
	struct mstar_ge_dma_buf src_dma_buf, dst_dma_buf;

	struct list_head queue;
	wait_queue_head_t dma_wait;
	bool dma_done;
};

static inline void mstar_ge_job_init(void *job)
{
	struct mstar_ge_job *ge_job = job;

	INIT_LIST_HEAD(&ge_job->queue);
	init_waitqueue_head(&ge_job->dma_wait);
}

static int mstar_ge_cmq_free(struct mstar_ge *ge)
{
	unsigned int tmp, cmqfree;

	regmap_field_read(ge->cmq_free, &cmqfree);
	regmap_field_read(ge->cmq2_free, &tmp);
	cmqfree += tmp;

	return cmqfree;
}

static void mstar_ge_asciiart(void *bm, int width, int height)
{
	int y, x;
	unsigned int a, r, g, b;

	for(y = 0; y < height; y++){
		void *line = ((u16*) bm) + (width * y);
		printk("%16ph\n", line);
		//for (x = 0; x < width; x++) {
		//	u16 *p = ((u16*) bm) + (width * y) + x;
		//	pr_cont("%02x\n", (unsigned) *p);
		//}

	}
}

static void mstar_ge_tag(struct mstar_ge *ge)
{
	ge->tag++;
	regmap_field_force_write(ge->tagl, ge->tag);
	regmap_field_force_write(ge->tagh, ge->tag >> 16);
	dev_info(ge->dev, "tag is: %d\n", (unsigned) ge->tag);
}

static void mstar_ge_set_src(struct mstar_ge *ge, dma_addr_t dmaaddr, unsigned int pitch)
{
	regmap_field_write(ge->srcl, dmaaddr);
	regmap_field_write(ge->srch, dmaaddr >> 16);
	regmap_field_write(ge->srcpitch, pitch);
}

static void mstar_ge_set_dst(struct mstar_ge *ge, dma_addr_t dmaaddr, unsigned int pitch)
{
	regmap_field_write(ge->dstl, dmaaddr);
	regmap_field_write(ge->dsth, dmaaddr >> 16);
	regmap_field_write(ge->dstpitch, pitch);
}

static void mstar_ge_set_clip(struct mstar_ge *ge, unsigned int left, unsigned int top,
		unsigned int right, unsigned int bottom)
{
	regmap_field_write(ge->clip_left, left);
	regmap_field_write(ge->clip_top, top);
	regmap_field_write(ge->clip_right, right);
	regmap_field_write(ge->clip_bottom, bottom);
}

static void mstar_ge_set_priv0(struct mstar_ge *ge, unsigned int x, unsigned int y)
{
	regmap_field_write(ge->x0, x);
	regmap_field_write(ge->y0, y);
}

static void mstar_ge_set_priv1(struct mstar_ge *ge, unsigned int x, unsigned int y)
{
	regmap_field_write(ge->x1, x);
	regmap_field_write(ge->y1, y);
}

static void mstar_ge_set_priv2(struct mstar_ge *ge, unsigned int x, unsigned int y)
{
	regmap_field_write(ge->x2, x);
	regmap_field_write(ge->y2, y);
}

static void mstar_ge_set_start_color(struct mstar_ge *ge,
				      struct mstar_ge_color *start_color)
{
	regmap_field_write(ge->b_st, start_color->b);
	regmap_field_write(ge->g_st, start_color->g);
	regmap_field_write(ge->r_st, start_color->r);
	regmap_field_write(ge->a_st, start_color->a);
}

static int mstar_ge_do_line(struct mstar_ge *ge,
			    unsigned int x0,
			    unsigned int y0,
			    unsigned int x1,
			    unsigned int y1)
{
	dev_info(ge->dev, "doing line draw from %d,%d to %d,%d, free %d\n",
			x0, y0, x1, y1, mstar_ge_cmq_free(ge));

	mstar_ge_set_priv0(ge, x0, y0);
	mstar_ge_set_priv1(ge, x1, y1);

	/* Do it! */
	regmap_field_force_write(ge->prim_type, PRIM_TYPE_LINE);
	//regmap_field_write(ge->en, 0);

	return 0;
}

static int mstar_ge_do_rectfill(struct mstar_ge *ge,
			        unsigned int left,
				unsigned int top,
				unsigned int right,
				unsigned int bottom)
{
	dev_info(ge->dev, "doing rect fill, %d %d -> %d %d, free %d\n",
			left, top, right, bottom, mstar_ge_cmq_free(ge));

	mstar_ge_set_priv0(ge, left, top);
	mstar_ge_set_priv1(ge, right, bottom);

	regmap_field_force_write(ge->prim_type, PRIM_TYPE_RECTFILL);

	return 0;
}

static int mstar_ge_drm_color_to_gop(u32 fourcc)
{
	switch(fourcc) {
	// need to ignore the alpha for this?
	case DRM_FORMAT_XRGB8888:
	case DRM_FORMAT_ARGB8888:
		return COLOR_FORMAT_ARGB8888;
	case DRM_FORMAT_RGB565:
		return COLOR_FORMAT_RGB565;
	};

	return -ENOTSUPP;
}

static int mstar_ge_do_bitblt(struct mstar_ge *ge, unsigned int width,
		unsigned int height, struct mstar_ge_bitblt *bitblt)
{
	dev_info(ge->dev, "doing bitblt %d, %d -> %d:%d,%d:%d free %d\n",
			bitblt->src_x0, bitblt->src_y0,
			bitblt->dst_x0, bitblt->dst_y0,
			bitblt->dst_x1, bitblt->dst_y1,
			mstar_ge_cmq_free(ge));

	regmap_field_write(ge->dfb, 1);
	regmap_field_write(ge->bld_alphachan, 1);

	regmap_field_write(ge->bitblt_src_width, width);
	regmap_field_write(ge->bitblt_src_height, height);

	regmap_field_write(ge->rot, bitblt->rotation);

	/* set the region to copy to */
	switch(bitblt->rotation) {
		case MSTAR_GE_ROTATION_0:
			mstar_ge_set_priv0(ge, bitblt->dst_x0, bitblt->dst_y0);
			mstar_ge_set_priv1(ge, bitblt->dst_x1, bitblt->dst_y1);
			break;
		case MSTAR_GE_ROTATION_90:
			mstar_ge_set_priv0(ge, bitblt->dst_x1, bitblt->dst_y0);
			mstar_ge_set_priv1(ge, (bitblt->dst_x1 * 2) - 1, bitblt->dst_y1);
			break;
		case MSTAR_GE_ROTATION_180:
			mstar_ge_set_priv0(ge, bitblt->dst_x1, bitblt->dst_y1);
			mstar_ge_set_priv1(ge, (bitblt->dst_x1 * 2) - 1, (bitblt->dst_y1 * 2) - 1);
			break;
		case MSTAR_GE_ROTATION_270:
			mstar_ge_set_priv0(ge, bitblt->dst_x1, bitblt->dst_y1);
			mstar_ge_set_priv1(ge, bitblt->dst_x0, bitblt->dst_y0);
			break;
	}

	/* set the top left corner of the source */
	mstar_ge_set_priv2(ge, bitblt->src_x0, bitblt->src_y0);

	regmap_field_force_write(ge->prim_type, PRIM_TYPE_BITBLT);

	return 0;
}

#define P256_ENTRIES 0xff

static void mstar_ge_write_p256(struct mstar_ge *ge)
{
	int i;

	for(i = 0; i < P256_ENTRIES; i++){
		regmap_field_write(ge->p256_index, i);
		regmap_field_write(ge->p256_r, 0xff);
		regmap_field_write(ge->p256_g, 0xaa);
		regmap_field_write(ge->p256_b, 0x55);
		regmap_field_write(ge->p256_a, 0xff);
		regmap_field_force_write(ge->p256_rw, 1);
	}
}

static void mstar_ge_read_p256(struct mstar_ge *ge)
{
	unsigned r, g, b, a;
	int i;

	regmap_field_write(ge->p256_rw, 0);

	for(i = 0; i < P256_ENTRIES; i++){
		regmap_field_force_write(ge->p256_index, i);
		regmap_field_read(ge->p256_r, &r);
		regmap_field_read(ge->p256_g, &g);
		regmap_field_read(ge->p256_b, &b);
		regmap_field_read(ge->p256_a, &a);
		dev_warn(ge->dev, "p256 read %x, r: %x, g: %x, b: %x, a: %x\n",
				i, r, g, b, a);
	}
}

static int mstar_ge_queue_job(struct mstar_ge *ge, struct mstar_ge_job *job)
{
	int src_fmt;
	int dst_fmt = mstar_ge_drm_color_to_gop(job->dst_cfg.fourcc);

	/* dst is mandatory */
	if (!job->dst_addr)
		return -EINVAL;

	list_add_tail(&job->queue, &ge->queue);

	/* src is optional */
	if(job->src_addr) {
		src_fmt = mstar_ge_drm_color_to_gop(job->src_cfg.fourcc);
		if (src_fmt < 0)
			return src_fmt;

		dev_info(ge->dev, "Setting source %d x %d (%d)\n",
				job->src_cfg.width, job->src_cfg.height, job->src_cfg.pitch);
		mstar_ge_set_src(ge, job->src_addr, job->src_cfg.pitch);
		regmap_field_write(ge->srcclrfmt, src_fmt);
	}

	/* dst is required */
	if (dst_fmt < 0)
		return dst_fmt;

	dev_info(ge->dev, "Setting destination %d x %d (%d)\n",
			job->dst_cfg.width, job->dst_cfg.height, job->dst_cfg.pitch);
	mstar_ge_set_dst(ge, job->dst_addr, job->dst_cfg.pitch);
	regmap_field_write(ge->dstclrfmt, dst_fmt);

	/* */
	mstar_ge_tag(ge);
	regmap_field_write(ge->en, 1);

	/* set the clip */
	mstar_ge_set_clip(ge, 0, 0,
			job->dst_cfg.width - 1,
			job->dst_cfg.height - 1);

	switch (job->opdata.op) {
	case MSTAR_GE_OP_LINE:
		regmap_field_write(ge->rot, 0);
		mstar_ge_set_start_color(ge, &job->opdata.line.start_color);
		regmap_field_write(ge->line_last, 1);
		regmap_field_write(ge->line_length, 5);
		regmap_field_write(ge->line_major, 1);
		// should set the angle I guess but it does nothing
		regmap_field_write(ge->line_delta, 50);
		mstar_ge_do_line(ge,
				 job->opdata.line.x0,
				 job->opdata.line.y0,
				 job->opdata.line.x1,
				 job->opdata.line.y1);
		break;
	case MSTAR_GE_OP_RECTFILL:
		regmap_field_write(ge->rot, 0);
		mstar_ge_set_start_color(ge, &job->opdata.rectfill.start_color);
		mstar_ge_do_rectfill(ge,
				     job->opdata.rectfill.x0,
				     job->opdata.rectfill.y0,
				     min(job->opdata.rectfill.x1, job->dst_cfg.width - 1),
				     min(job->opdata.rectfill.y1, job->dst_cfg.height - 1));
		break;
	case MSTAR_GE_OP_BITBLT:
		mstar_ge_do_bitblt(ge,
				   job->src_cfg.width,
				   job->src_cfg.height,
				   &job->opdata.bitblt);
		break;
	default:
		return -EINVAL;
	}

	if (!wait_event_timeout(job->dma_wait, job->dma_done, HZ * 10)) {
		dev_err(ge->dev, "timeout waiting for dma to finish\n");
	}

	return 0;
}

static irqreturn_t mstar_ge_irq(int irq, void *data)
{
	struct mstar_ge *ge = data;
	struct mstar_ge_job *job;
	unsigned int status;

	regmap_field_read(ge->irq_status, &status);
	regmap_field_write(ge->irq_force, 0);


	/*
	 * To clear the irq the clear bits need to be
	 * set, but apparently the hardware doesn't clear
	 * them so if we don't clear them no more interrupts
	 * happen..
	 */
	regmap_field_force_write(ge->irq_clr, ~0);
	regmap_field_force_write(ge->irq_clr, 0);

	dev_dbg(ge->dev, "interrupt, %x\n", status);

	if (list_empty(&ge->queue)) {
		dev_err(ge->dev, "Interrupt when no jobs queued!\n");
		return IRQ_NONE;
	}

	job = list_first_entry(&ge->queue, struct mstar_ge_job, queue);
	job->dma_done = true;
	list_del(&job->queue);

	wake_up(&job->dma_wait);

	return IRQ_HANDLED;
}

static void mstar_ge_filltestbuf(const struct mstar_ge_buf *buf)
{
	u32 i;

	for(i = 0; i < buf->cfg.height; i++)
		memset(buf->buf + (buf->cfg.pitch * i), ~i & 0xff, buf->cfg.pitch);
}

static void mstar_ge_cleartestbuf(const struct mstar_ge_buf *buf)
{
	memset(buf->buf, 0, mstar_ge_buf_sz(buf));
}

static int mstar_ge_test_allocbuffers(struct mstar_ge *ge,
		struct mstar_ge_buf *src,
		struct mstar_ge_buf *dst,
		void **src_alloc,
		void **dst_alloc)
{
	src->cfg.width = 8;
	src->cfg.height = 8;
	src->cfg.pitch = src->cfg.width * 2;
	src->cfg.fourcc = DRM_FORMAT_RGB565;

	memcpy(dst, src, sizeof(*dst));

	*src_alloc = kzalloc(mstar_ge_buf_sz(src) * 3, GFP_KERNEL);
	if (!src->buf)
		return -ENOMEM;

	*dst_alloc = kzalloc(mstar_ge_buf_sz(dst) * 3, GFP_KERNEL);
	if (!dst->buf) {
		kfree(src->buf);
		return -ENOMEM;
	}

	/*
	 * Drawable area in the middle of the buffer so we
	 * can detect dma memory corruption
	 */
	src->buf = *src_alloc + mstar_ge_buf_sz(src);
	dst->buf = *dst_alloc + mstar_ge_buf_sz(dst);

	return 0;
}

static dma_addr_t mstar_ge_map_kalloc(struct mstar_ge *ge,
		const struct mstar_ge_buf *buf,
		enum dma_data_direction dir)
{
	return dma_map_single(ge->dev, buf->buf, mstar_ge_buf_sz(buf), dir);
}

static void mstar_ge_unmap_kalloc(
		const struct mstar_ge *ge,
		dma_addr_t addr,
		const struct mstar_ge_buf *buf,
		enum dma_data_direction dir)
{
	dma_unmap_single(ge->dev, addr, mstar_ge_buf_sz(buf), dir);
}

static int mstar_ge_test_mapbuffers(struct mstar_ge *ge,
		struct mstar_ge_job *j,
		const struct mstar_ge_buf *src,
		const struct mstar_ge_buf *dst)
{
	int ret;

	j->src_addr = mstar_ge_map_kalloc(ge, src, DMA_TO_DEVICE);
	ret = dma_mapping_error(ge->dev, j->src_addr);
	if (ret)
		return ret;

	j->dst_addr = mstar_ge_map_kalloc(ge, dst, DMA_FROM_DEVICE);
	ret = dma_mapping_error(ge->dev, j->dst_addr);
	if (ret)
		goto unmap_src;

	return ret;

unmap_src:
	mstar_ge_unmap_kalloc(ge, j->src_addr, src, DMA_TO_DEVICE);
	return ret;
}

static void mstar_ge_test_unmapbuffers(const struct mstar_ge *ge,
		struct mstar_ge_job *j,
		const struct mstar_ge_buf *src,
		const struct mstar_ge_buf *dst)
{
	mstar_ge_unmap_kalloc(ge, j->src_addr, src, DMA_TO_DEVICE);
	mstar_ge_unmap_kalloc(ge, j->dst_addr, dst, DMA_FROM_DEVICE);
}

static int mstar_ge_test_pretest(struct mstar_ge *ge,
		struct mstar_ge_job *j,
		const struct mstar_ge_buf *src,
		const struct mstar_ge_buf *dst)
{
	dev_info(ge->dev, "src before\n");
	mstar_ge_asciiart(src->buf, src->cfg.width, src->cfg.height);
	dev_info(ge->dev, "dst before\n");
	mstar_ge_asciiart(dst->buf, dst->cfg.width, dst->cfg.height);

	return mstar_ge_test_mapbuffers(ge, j, src, dst);
}

static void mstar_ge_test_posttest(
		const struct mstar_ge *ge,
		struct mstar_ge_job *j,
		const struct mstar_ge_buf *src,
		const struct mstar_ge_buf *dst,
		void *src_alloc, void *dst_alloc)
{
	int i;

	mstar_ge_test_unmapbuffers(ge, j, src, dst);

	for (i = 0; i < mstar_ge_buf_sz(dst); i++) {
		u8 *b = ((u8*)dst_alloc) + i;
		BUG_ON(*b);
	}

	for (i = 0; i < mstar_ge_buf_sz(dst); i++) {
		u8 *b = ((u8*)dst_alloc) + i + (mstar_ge_buf_sz(dst) * 2);
		BUG_ON(*b);
	}

	dev_info(ge->dev, "src after job\n");
	mstar_ge_asciiart(src->buf, src->cfg.width, src->cfg.height);
	dev_info(ge->dev, "dst after job\n");
	mstar_ge_asciiart(dst->buf, dst->cfg.width, dst->cfg.height);
}

static struct mstar_ge_job* mstar_ge_alloc_job(struct mstar_ge *ge)
{
	struct mstar_ge_job *j;

	j = kmem_cache_alloc(ge->jobs, GFP_KERNEL);
	if (!j)
		return NULL;

	j->dma_done = false;

	return j;
}

static void mstar_ge_reset_job(struct mstar_ge_job *j)
{
	j->dma_done = false;
}

static int mstar_ge_test(struct mstar_ge *ge)
{
	struct mstar_ge_job *j;
	struct mstar_ge_buf src, dst;
	void *src_alloc, *dst_alloc;
	int ret;

	j = mstar_ge_alloc_job(ge);
	if (!j) {
		dev_err(ge->dev, "Failed to allocate test job\n");
		return -ENOMEM;
	}

	/* Setup the job with the common bits */
	ret = mstar_ge_test_allocbuffers(ge, &src, &dst, &src_alloc, &dst_alloc);
	if (ret) {
		dev_err(ge->dev, "Failed to allocate test buffers\n");
		goto free_job;
	}

	memcpy(&j->src_cfg, &src.cfg, sizeof(j->src_cfg));
	memcpy(&j->dst_cfg, &dst.cfg, sizeof(j->dst_cfg));

	/* Line */
	dev_info(ge->dev, "Test, line\n");
	j->opdata.op = MSTAR_GE_OP_LINE;
	j->opdata.line.x0 = 0;
	j->opdata.line.y0 = 0;
	j->opdata.line.x1 = dst.cfg.width - 1;
	j->opdata.line.y1 = dst.cfg.height - 1;
	j->opdata.line.start_color.r = 0xaa;
	j->opdata.line.start_color.g = 0x00;
	j->opdata.line.start_color.b = 0x00;
	j->opdata.line.start_color.a = 0xff;

	//mstar_get_filltestbuf(j->opdata.dst, j->opdata.dst_height, j->opdata.dst_pitch);
	ret = mstar_ge_test_pretest(ge, j, &src, &dst);
	if (ret)
		goto free_src;

	mstar_ge_queue_job(ge, j);

	mstar_ge_test_posttest(ge, j, &src, &dst, src_alloc, dst_alloc);

	/* Rectfill */
	dev_info(ge->dev, "Test, rect fill\n");
	mstar_ge_reset_job(j);
	j->opdata.op = MSTAR_GE_OP_RECTFILL;
	j->opdata.rectfill.x0 = 1;
	j->opdata.rectfill.y0 = 1;
	j->opdata.rectfill.x1 = j->opdata.rectfill.x0 + 3;
	j->opdata.rectfill.y1 = j->opdata.rectfill.y0 + 3;
	j->opdata.rectfill.start_color.r = 0x55;
	j->opdata.rectfill.start_color.g = 0x00;
	j->opdata.rectfill.start_color.b = 0x00;
	j->opdata.rectfill.start_color.a = 0xff;

	mstar_ge_filltestbuf(&dst);
	ret = mstar_ge_test_pretest(ge, j, &src, &dst);
	if (ret)
		goto free_src;

	mstar_ge_queue_job(ge, j);

	mstar_ge_test_posttest(ge, j, &src, &dst, src_alloc, dst_alloc);

	/* bitblt, 0 rotation */
	dev_info(ge->dev, "Test, bitblt, no rotation \n");
	mstar_ge_reset_job(j);
	j->opdata.op = MSTAR_GE_OP_BITBLT;
	j->opdata.bitblt.src_x0 = 2;
	j->opdata.bitblt.src_y0 = 2;
	j->opdata.bitblt.dst_x0 = 1;
	j->opdata.bitblt.dst_y0 = 1;
	j->opdata.bitblt.dst_x1 = 3;
	j->opdata.bitblt.dst_y1 = 3;
	j->opdata.bitblt.rotation = MSTAR_GE_ROTATION_0;

	mstar_ge_filltestbuf(&src);
	mstar_ge_cleartestbuf(&dst);
	ret = mstar_ge_test_pretest(ge, j, &src, &dst);
	if (ret)
		goto free_src;

	mstar_ge_queue_job(ge, j);

	mstar_ge_test_posttest(ge, j, &src, &dst, src_alloc, dst_alloc);

	goto free_src;

	/* bitblt, 90 rotation */
	dev_info(ge->dev, "Test, bitblt, rotation 90\n");
	mstar_ge_reset_job(j);
	j->opdata.bitblt.rotation = MSTAR_GE_ROTATION_90;

	mstar_ge_filltestbuf(&src);
	mstar_ge_cleartestbuf(&dst);
	ret = mstar_ge_test_pretest(ge, j, &src, &dst);
	if (ret)
		goto free_src;

	mstar_ge_queue_job(ge, j);

	mstar_ge_test_posttest(ge, j, &src, &dst, src_alloc, dst_alloc);

	/* bitblt, 180 rotation */
	dev_info(ge->dev, "Test, bitblt, rotation 180\n");
	mstar_ge_reset_job(j);
	j->opdata.bitblt.rotation = MSTAR_GE_ROTATION_180;

	mstar_ge_filltestbuf(&src);
	mstar_ge_cleartestbuf(&dst);
	ret = mstar_ge_test_pretest(ge, j, &src, &dst);
	if (ret)
		goto free_src;

	mstar_ge_queue_job(ge, j);

	mstar_ge_test_posttest(ge,j, &src, &dst, src_alloc, dst_alloc);

	/* bitblt, 270 rotation */
	dev_info(ge->dev, "Test, bitblt, rotation 270\n");
	mstar_ge_reset_job(j);
	j->opdata.bitblt.rotation = MSTAR_GE_ROTATION_270;

	mstar_ge_filltestbuf(&src);
	mstar_ge_cleartestbuf(&dst);
	ret = mstar_ge_test_pretest(ge, j, &src, &dst);
	if (ret)
		goto free_src;

	mstar_ge_queue_job(ge, j);

	mstar_ge_test_posttest(ge,j, &src, &dst, src_alloc, dst_alloc);

	/* clean up */
free_src:
	kfree(src_alloc);
	kfree(dst_alloc);
free_job:
	kmem_cache_free(ge->jobs, j);

	return ret;
}

static long mstar_ge_ioctl_queue(struct mstar_ge *ge, unsigned long arg)
{
	struct mstar_ge_job_request req;
	struct mstar_ge_opdata *ops;
	struct mstar_ge_buf bufs[2];
	struct mstar_ge_job *job;
	enum dma_data_direction dma_dirs[2];
	struct dma_buf *dma_bufs[2];
	struct dma_buf_attachment *dma_attachs[2];
	struct sg_table *dma_mappings[2];
	dma_addr_t dma_addrs[2];
	unsigned long tag = 0xAA55;
	size_t opssz, bufssz;
	int ret;
	int i;

	ret = copy_from_user(&req, (void*) arg, sizeof(req));
	if (ret) {
		dev_err(ge->dev, "Failed to get job request: %d, %lx\n", ret, arg);
		return -EFAULT;
	}

	/* Need at least one op */
	if (req.num_ops < 1) {
		dev_err(ge->dev, "Invalid amount of ops in request: %d\n",
				req.num_ops);
		return -EINVAL;
	}

	/* For now we either need 1 or 2 buffers */
	if (req.num_bufs <= 0 || req.num_bufs > 2) {
		dev_err(ge->dev, "Invalid amount of buffers in request: %d\n",
				req.num_bufs);
		return -EINVAL;
	}

	opssz = sizeof(struct mstar_ge_opdata) * req.num_ops;
	ops = kzalloc(opssz, GFP_KERNEL);
	if (!ops)
		return -ENOMEM;

	ret = copy_from_user(ops, req.ops, opssz);
	if (ret) {
		dev_err(ge->dev, "Failed to copy ops from job request: %d\n", ret);
		return -EFAULT;
	}

	bufssz = sizeof(struct mstar_ge_buf) * req.num_bufs;
	ret = copy_from_user(bufs, req.bufs, bufssz);
	if (ret) {
		dev_err(ge->dev, "Failed to copy bufs from job request: %d\n", ret);
		goto free_ops;
	}

	/* Check all the buffers are sane */
	for (i = 0; i < req.num_bufs; i++) {
		struct mstar_ge_buf *buf = &bufs[i];
		dev_info(ge->dev, "buffer %d: fd %d %dpx x %dpx, pitch %d, format %p4cc\n",
			 i, buf->fd, buf->cfg.width, buf->cfg.height, buf->cfg.pitch,
			 &buf->cfg.fourcc);

		if (buf->fd < 0) {
			ret = -EINVAL;
			goto free_ops;
		}

		if (mstar_ge_drm_color_to_gop(buf->cfg.fourcc) < 0) {
			dev_warn(ge->dev, "Unhandled buffer type\n");
			ret = -EINVAL;
			goto free_ops;
		}
	}

	if (req.num_bufs == 1)
		dma_dirs[0] = DMA_FROM_DEVICE;
	else {
		dma_dirs[0] = DMA_TO_DEVICE;
		dma_dirs[1] = DMA_FROM_DEVICE;
	}

	/* Map the buffers */
	for (i = 0; i < req.num_bufs; i++) {
		struct mstar_ge_buf *buf = &bufs[i];

		dma_bufs[i] = dma_buf_get(buf->fd);
		if (IS_ERR(dma_bufs[i])) {
			dev_err(ge->dev, "failed to get dma_buf for buffer\n");
			return PTR_ERR(dma_bufs[i]);
		}

		dma_attachs[i] = dma_buf_attach(dma_bufs[i], ge->dev);
		if (IS_ERR(dma_attachs[i])) {
			dev_err(ge->dev, "failed to attach dma buf\n");
			return PTR_ERR(dma_attachs[i]);
		}

		dma_mappings[i] = dma_buf_map_attachment(dma_attachs[i], dma_dirs[i]);
		if (IS_ERR(dma_mappings[i])) {
			dev_err(ge->dev, "failed to map dma buf\n");
			return PTR_ERR(dma_mappings[i]);
		}

		dma_addrs[i] = sg_dma_address(dma_mappings[i]->sgl);
		dev_dbg(ge->dev, "buffer is mapped to 0x%x\n", dma_addrs[i]);
	}

	for (i = 0; i < req.num_ops; i++) {
		struct mstar_ge_opdata *op = &ops[i];
		/* Check the operation isn't garbage */
		switch (op->op) {
		case MSTAR_GE_OP_LINE:
			dev_info(ge->dev, "op %d: LINE\n", i);
			break;
		case MSTAR_GE_OP_RECTFILL:
			dev_info(ge->dev, "op %d: RECTFILL, %d:%d -> %d:%d\n",
					i,
					op->rectfill.x0, op->rectfill.y0,
					op->rectfill.x1, op->rectfill.y1);
			break;
		case MSTAR_GE_OP_BITBLT:
			dev_info(ge->dev, "op %d: BITBLT %d,%d -> %d:%d,%d,%d (rot %d)\n", i,
					op->bitblt.src_x0, op->bitblt.src_y0,
					op->bitblt.dst_x0, op->bitblt.dst_y0,
					op->bitblt.dst_x1, op->bitblt.dst_y1,
					op->bitblt.rotation);
			op->bitblt.rotation = 0;
			break;
		default:
			ret = -EFAULT;
			goto free_ops;
		}

		job = mstar_ge_alloc_job(ge);
		if (!job) {
			dev_err(ge->dev, "Failed to allocate job descriptor\n");
			ret = -ENOMEM;
			goto free_ops;
		}

		memcpy(&job->opdata, op, sizeof(job->opdata));
		if (req.num_bufs == 1) {
			job->src_addr = (dma_addr_t) NULL;
			job->dst_addr = dma_addrs[0];
			memcpy(&job->dst_cfg, &bufs[0].cfg, sizeof(job->dst_cfg));
		}
		else {
			job->src_addr = dma_addrs[0];
			job->dst_addr = dma_addrs[1];
			memcpy(&job->src_cfg, &bufs[0].cfg, sizeof(job->src_cfg));
			memcpy(&job->dst_cfg, &bufs[1].cfg, sizeof(job->dst_cfg));
		}

		dev_info(ge->dev, "Queuing job for op %d\n", i);
		mstar_ge_queue_job(ge, job);
		kmem_cache_free(ge->jobs, job);
	}

	/* unmap everything, this needs to move once things are actually async */
	for (i = 0; i < req.num_bufs; i++) {
		dma_buf_unmap_attachment(dma_attachs[i], dma_mappings[i], dma_dirs[i]);
		dma_buf_detach(dma_bufs[i], dma_attachs[i]);
		dma_buf_put(dma_bufs[i]);
	}

	ret = copy_to_user(req.tag, &tag, sizeof(tag));
	if (ret) {
		dev_err(ge->dev, "Failed to copy request tag to user: %d\n", ret);
		return -EFAULT;
	}

free_ops:
	kfree(ops);
	return ret;
}

static long mstar_ge_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	struct mstar_ge *ge = container_of(f->private_data, struct mstar_ge, ge_dev);
	int ret = 0;

	switch(cmd){
	case MSTAR_GE_IOCTL_INFO: {
		struct mstar_ge_info info;
		info.caps = 1;
		ret = copy_to_user((void *) arg, &info, sizeof(info));
	}
		break;
	case MSTAR_GE_IOCTL_QUEUE:
		return mstar_ge_ioctl_queue(ge, arg);
	case MSTAR_GE_IOCTL_QUERY: {
		unsigned long tag;

		printk("%s:%d - %lu\n", __func__, __LINE__, tag);

		if (copy_from_user((void*) arg, &tag, sizeof(tag)))
			return -EFAULT;

	}
		break;
	default:
		ret = -EINVAL;
	}

	return ret;
}

static const struct file_operations ge_fops = {
	.unlocked_ioctl = mstar_ge_ioctl,
};

static int mstar_ge_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct regmap *regmap;
	struct mstar_ge *ge;
	void __iomem *base;
	int irq, ret;

	ge = devm_kzalloc(dev, sizeof(*ge), GFP_KERNEL);
	if (!ge)
		return -ENOMEM;

	ge->jobs = kmem_cache_create("gejobs", sizeof(struct mstar_ge_job),
			__alignof__(struct mstar_ge_job), 0, mstar_ge_job_init);
	if (!ge->jobs)
		return -ENOMEM;

	INIT_LIST_HEAD(&ge->queue);

	ge->dev = dev;

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);

	regmap = devm_regmap_init_mmio(dev, base, &mstar_ge_regmap_config);
	if (IS_ERR(regmap))
		return PTR_ERR(regmap);

	ge->en = devm_regmap_field_alloc(dev, regmap, en_field);
	ge->abl = devm_regmap_field_alloc(dev, regmap, abl_field);
	ge->dfb = devm_regmap_field_alloc(dev, regmap, dfb_field);
	ge->clk_en = devm_regmap_field_alloc(dev, regmap, clk_en_field);
	ge->busy = devm_regmap_field_alloc(dev, regmap, gebusy_field);
	ge->cmq_free = devm_regmap_field_alloc(dev, regmap, cmq_free_field);
	ge->cmq2_free = devm_regmap_field_alloc(dev, regmap, cmq2_free_field);
	ge->irq_mask = devm_regmap_field_alloc(dev, regmap, irq_mask_field);
	ge->irq_force = devm_regmap_field_alloc(dev, regmap, irq_force_field);
	ge->irq_clr = devm_regmap_field_alloc(dev, regmap, irq_clr_field);
	ge->irq_status = devm_regmap_field_alloc(dev, regmap, irq_status_field);

	/* src buffer */
	ge->srcl = devm_regmap_field_alloc(dev, regmap, srcl_field);
	ge->srch = devm_regmap_field_alloc(dev, regmap, srch_field);
	ge->srcpitch = devm_regmap_field_alloc(dev, regmap, srcpitch_field);
	ge->srcclrfmt = devm_regmap_field_alloc(dev, regmap, src_colorfmt_field);

	/* dst buffer */
	ge->dstl = devm_regmap_field_alloc(dev, regmap, dstl_field);
	ge->dsth = devm_regmap_field_alloc(dev, regmap, dsth_field);
	ge->dstpitch = devm_regmap_field_alloc(dev, regmap, dstpitch_field);
	ge->dstclrfmt = devm_regmap_field_alloc(dev, regmap, dst_colorfmt_field);

	ge->bld_alphachan = devm_regmap_field_alloc(dev, regmap, bld_alphachan_field);

	/* clipping window */
	ge->clip_left = devm_regmap_field_alloc(dev, regmap, clip_left_field);
	ge->clip_right = devm_regmap_field_alloc(dev, regmap, clip_right_field);
	ge->clip_top = devm_regmap_field_alloc(dev, regmap, clip_top_field);
	ge->clip_bottom = devm_regmap_field_alloc(dev, regmap, clip_bottom_field);

	ge->rot = devm_regmap_field_alloc(dev, regmap, rot_field);
	ge->prim_type = devm_regmap_field_alloc(dev, regmap, prim_type_field);

	/* Line controls */
	ge->line_delta = devm_regmap_field_alloc(dev, regmap, line_delta_field);
	ge->line_major = devm_regmap_field_alloc(dev, regmap, line_major_field);
	ge->line_last = devm_regmap_field_alloc(dev, regmap, line_last_field);
	ge->line_length = devm_regmap_field_alloc(dev, regmap, line_length_field);

	/* vertex */
	ge->x0 = devm_regmap_field_alloc(dev, regmap, x0_field);
	ge->y0 = devm_regmap_field_alloc(dev, regmap, y0_field);
	ge->x1 = devm_regmap_field_alloc(dev, regmap, x1_field);
	ge->y1 = devm_regmap_field_alloc(dev, regmap, y1_field);
	ge->x2 = devm_regmap_field_alloc(dev, regmap, x2_field);
	ge->y2 = devm_regmap_field_alloc(dev, regmap, y2_field);

	ge->bitblt_src_width = devm_regmap_field_alloc(dev, regmap, bitblt_srcwidth_field);
	ge->bitblt_src_height = devm_regmap_field_alloc(dev, regmap, bitblt_srcheight_field);

	/* start color */
	ge->b_st = devm_regmap_field_alloc(dev, regmap, b_st_field);
	ge->g_st = devm_regmap_field_alloc(dev, regmap, g_st_field);
	ge->r_st = devm_regmap_field_alloc(dev, regmap, r_st_field);
	ge->a_st = devm_regmap_field_alloc(dev, regmap, a_st_field);

	ge->tagl = devm_regmap_field_alloc(dev, regmap, tagl_field);
	ge->tagh = devm_regmap_field_alloc(dev, regmap, tagh_field);

	/* p256 */
	ge->p256_b = devm_regmap_field_alloc(dev, regmap, p256_b_field);
	ge->p256_g = devm_regmap_field_alloc(dev, regmap, p256_g_field);
	ge->p256_r = devm_regmap_field_alloc(dev, regmap, p256_r_field);
	ge->p256_a = devm_regmap_field_alloc(dev, regmap, p256_a_field);
	ge->p256_index = devm_regmap_field_alloc(dev, regmap, p256_index_field);
	ge->p256_rw = devm_regmap_field_alloc(dev, regmap, p256_rw_field);

	ge->clk = devm_clk_get(dev, "ge");
	if (IS_ERR(ge->clk))
		return PTR_ERR(ge->clk);

	clk_prepare_enable(ge->clk);

	irq = irq_of_parse_and_map(pdev->dev.of_node, 0);
	if (!irq)
		return -ENODEV;

	ret = devm_request_irq(dev, irq, mstar_ge_irq, IRQF_SHARED, dev_name(dev), ge);
	if (ret)
		return ret;

	regmap_field_write(ge->irq_mask, 0);
	regmap_field_write(ge->clk_en, 1);

	dev_set_drvdata(dev, ge);

	if (IS_ENABLED(CONFIG_DRM_MSTAR_GE_SELFTEST)) {
		int testret = mstar_ge_test(ge);

		if (testret)
			dev_err(dev, "Self test failed: %d\n", testret);
	}

	ge->ge_dev.minor = MISC_DYNAMIC_MINOR;
	ge->ge_dev.name	= DRIVER_NAME;
	ge->ge_dev.fops	= &ge_fops;

	return misc_register(&ge->ge_dev);
}

static int mstar_ge_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mstar_ge *ge = dev_get_drvdata(dev);

	misc_deregister(&ge->ge_dev);

	kmem_cache_destroy(ge->jobs);

	return 0;
}

static const struct of_device_id mstar_ge_ids[] = {
	{
		.compatible = "sstar,ssd20xd-ge",
	},
	{},
};
MODULE_DEVICE_TABLE(of, mstar_ge_ids);

static struct platform_driver mstar_ge_driver = {
	.probe = mstar_ge_probe,
	.remove = mstar_ge_remove,
	.driver = {
		.name = DRIVER_NAME,
		.of_match_table = mstar_ge_ids,
	},
};
module_platform_driver(mstar_ge_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION(DRIVER_NAME);
MODULE_AUTHOR("Daniel Palmer <daniel@0x0f.com>");
