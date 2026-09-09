/*
 * drivers/video/fbdev/gcn-gx.c
 *
 * Nintendo GameCube/Wii GX GPU minimal driver
 *
 * Provides hardware EFB->XFB copy to replace the per-vsync software
 * RGB->YUV conversion done in gcnfb.c. The GX has a fixed-function
 * EFB->XFB blit unit that performs this conversion in hardware, freeing
 * the 729MHz Broadway CPU from doing it every frame.
 *
 * Pipeline (called once per vsync from vi_irq_handler):
 *   1. Tile the linear virtual framebuffer into GX texture format
 *   2. Point texture map 0 at the tiled buffer
 *   3. Draw a fullscreen textured quad to the EFB
 *   4. Trigger EFB->XFB copy (hardware RGB->YUV)
 *
 * The GX command processor (CP) is fed via the Write-Gather Pipe (wgPipe),
 * a 32-byte MMIO window at 0xCC008000 that batches writes into a FIFO.
 * Commands:
 *   0x08 + regidx(u8) + val(u32)             → CP register write
 *   0x10 + ((n-1)<<16|addr)(u32) + n×u32     → XF register write
 *   0x61 + val(u32)                           → BP register write
 *   0x80|vtxfmt + count(u16) + vertices       → draw primitive
 *
 * Register reference derived from libogc (devkitPro/libogc, MIT licence)
 * and YAGCD (Yet Another GameCube Documentation).
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/mutex.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/wait.h>
#include <linux/workqueue.h>
#include <linux/debugfs.h>
#include <linux/fs.h>
#include <linux/vmalloc.h>
#include <linux/mfd/syscon.h>
#include <linux/of_address.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <asm/cacheflush.h>
#include <asm/div64.h>
#include <asm/page.h>

#if IS_ENABLED(CONFIG_DRM_GCN_GX)
#include <drm/gcn_gx_mem1.h>
#include <uapi/drm/gcn_drm.h>
#endif

#include "gcn-gx.h"
#if IS_ENABLED(CONFIG_FB_GAMECUBE_GX)
#include "gcnfb-accel.h"
#endif
#if IS_ENABLED(CONFIG_DRM_GCN_GX)
#include <linux/gcn_drm_accel.h>
#endif

static u16 __iomem *cp_regs;
static u16 __iomem *pe_regs;
static struct regmap *pi_regmap;
static phys_addr_t gx_fifo_phys;
static phys_addr_t gx_tex_phys;

#if IS_ENABLED(CONFIG_DRM_GCN_GX)
enum gx_mem1_layout {
	GX_MEM1_LAYOUT_TILED_RGB565,
};

enum gx_mem1_access {
	GX_MEM1_ACCESS_IDLE,
};

struct gx_mem1_buffer {
	void *cpu_addr;
	phys_addr_t phys_addr;
	size_t size;
	enum gx_mem1_layout layout;
	enum gx_mem1_access access;
};

struct gx_mem1_allocation {
	struct drm_mm_node node;
	void *cpu_addr;
	size_t size;
};

static struct gcn_gx_mem1_allocator gx_mem1_allocator;
static struct gx_mem1_buffer gx_tex_workspace[2];
static DEFINE_MUTEX(gx_mem1_lock);
static unsigned int gx_mem1_user_allocations;
static bool gx_mem1_shutdown;
static void gx_mem1_free_workspaces_locked(void);
static unsigned int gx_mem1_total_bytes;
static unsigned int gx_mem1_used_bytes;
static unsigned int gx_mem1_free_bytes;
module_param_named(mem1_total_bytes, gx_mem1_total_bytes, uint, 0444);
MODULE_PARM_DESC(mem1_total_bytes, "Total bytes in the GX render-object pool");
module_param_named(mem1_used_bytes, gx_mem1_used_bytes, uint, 0444);
MODULE_PARM_DESC(mem1_used_bytes, "Bytes allocated from the GX render-object pool");
module_param_named(mem1_free_bytes, gx_mem1_free_bytes, uint, 0444);
MODULE_PARM_DESC(mem1_free_bytes, "Unallocated bytes in the GX render-object pool");
#endif

/* Byte offset of the next GX command byte within gx_fifo_buf.
 * GX commands are written here directly; gx_submit_cmds() advances
 * PI_FIFO_WPTR so the CP picks them up — bypasses the wgPipe entirely.
 */
static u32 fifo_pos;

/* FIFO buffer — must be in memory the GPU can DMA, 32-byte aligned */
static void *gx_fifo_buf_raw;
static void *gx_fifo_buf;

/* Texture tile buffer: virtual FB converted to GX 4×4 tiled format */
static void *gx_tex_raw;
static void *gx_tex_buf;
static void *gx_tex_buf_alt;
static u16 gx_expected_token;
static unsigned int gx_pe_finish_irq;
static bool gx_pe_finish_irq_requested;
static u32 gx_pe_finish_count;
static DECLARE_WAIT_QUEUE_HEAD(gx_pe_finish_wait);
static DEFINE_MUTEX(gx_submit_lock);

#define GX_DRM_FRAME_PE_FINISHES	2

enum gx_finish_diag_phase {
	GX_DIAG_SEED,
	GX_DIAG_WAIT_SEED,
	GX_DIAG_WAIT_BLUE,
	GX_DIAG_WAIT_INIT,
	GX_DIAG_WAIT_DRAW,
	GX_DIAG_DONE,
};

static enum gx_finish_diag_phase gx_diag_phase;
static u32 gx_diag_finish_baseline;

enum gx_vfb_format {
	GX_VFB_RGB565,
	GX_VFB_XRGB8888,
};

struct gx_frame_work {
	struct work_struct work;
	const void *vfb;
	u32 xfb_phys;
	u16 width;
	u16 height;
	enum gx_vfb_format format;
	u32 source_generation;
};

static struct gx_frame_work gx_frame_work;
#if IS_ENABLED(CONFIG_FB_GAMECUBE_GX)
static DEFINE_SPINLOCK(gx_frame_work_lock);
#endif
static u32 gx_live_texture_frame;
static u32 gx_frame_ready_xfb;
static const void *gx_frame_ready_vfb;
static bool gx_frame_work_busy;
static bool gx_frame_boot_deferred;
static bool gx_frame_publish_xfb;
static bool gx_frame_hold;
static u64 gx_rgb888_tile_total_ns;
static u64 gx_rgb888_tile_max_ns;
static u64 gx_rgb888_flush_total_ns;
static u64 gx_rgb888_flush_max_ns;
static u32 gx_rgb888_timing_frames;

#if IS_ENABLED(CONFIG_DRM_GCN_GX)
static bool gx_scale_trace;
static bool gx_scale_clear_color;
module_param_named(scale_clear_color, gx_scale_clear_color, bool, 0444);
MODULE_PARM_DESC(scale_clear_color,
		 "Produce the focused uniform final EFB with copy-clear only");
static bool gx_scale_split_reduce = true;
module_param_named(scale_split_reduce, gx_scale_split_reduce, bool, 0444);
MODULE_PARM_DESC(scale_split_reduce,
		 "Split long primitives for full-surface MEM1 640x240 to 320x120 reduction");
static bool gx_scale_gpu_split;
module_param_named(scale_gpu_split, gx_scale_gpu_split, bool, 0444);
MODULE_PARM_DESC(scale_gpu_split,
		 "Split both focused texture stages without CPU fixture correction");
static bool gx_scale_texture_half_rows;
module_param_named(scale_texture_half_rows, gx_scale_texture_half_rows, bool, 0444);
MODULE_PARM_DESC(scale_texture_half_rows,
		 "Split focused textured vertical runs into half-width quads");
static bool gx_scale_half_rows;
module_param_named(scale_half_rows, gx_scale_half_rows, bool, 0444);
MODULE_PARM_DESC(scale_half_rows,
		 "Split focused one-row rectangles into two half-width quads");
static bool gx_scale_row_triangles;
module_param_named(scale_row_triangles, gx_scale_row_triangles, bool, 0444);
MODULE_PARM_DESC(scale_row_triangles,
		 "Emit focused one-row rectangles as explicit triangle pairs");
static bool gx_scale_reverse_rows;
module_param_named(scale_reverse_rows, gx_scale_reverse_rows, bool, 0444);
MODULE_PARM_DESC(scale_reverse_rows,
		 "Submit focused one-row rectangles from bottom to top");
static bool gx_scale_columns;
module_param_named(scale_columns, gx_scale_columns, bool, 0444);
MODULE_PARM_DESC(scale_columns,
		 "Use 120 vertical strips in the focused direct-color draw");
static bool gx_scale_degenerate_first;
module_param_named(scale_degenerate_first, gx_scale_degenerate_first, bool, 0444);
MODULE_PARM_DESC(scale_degenerate_first,
		 "Place focused coincident-vertex quads before the real rectangles");
static unsigned int gx_scale_degenerate_quads;
module_param_named(scale_degenerate_quads, gx_scale_degenerate_quads, uint, 0444);
MODULE_PARM_DESC(scale_degenerate_quads,
		 "Append up to 117 coincident-vertex quads to focused split draw");
static unsigned int gx_scale_pad_bytes;
module_param_named(scale_pad_bytes, gx_scale_pad_bytes, uint, 0444);
MODULE_PARM_DESC(scale_pad_bytes,
		 "Append focused direct-color NOP bytes before final completion");
static unsigned int gx_scale_split_second;
module_param_named(scale_split_second, gx_scale_split_second, uint, 0444);
MODULE_PARM_DESC(scale_split_second,
		 "Optional second focused split row, above scale_split and below 120");
static unsigned int gx_scale_split;
module_param_named(scale_split, gx_scale_split, uint, 0444);
MODULE_PARM_DESC(scale_split,
		 "Split focused direct-color draw into two rectangles at row 1..119");
static unsigned int gx_scale_band_height = 1;
module_param_named(scale_band_height, gx_scale_band_height, uint, 0444);
MODULE_PARM_DESC(scale_band_height,
		 "Height of focused direct-color bands, from 1 to 120 rows");
static bool gx_scale_row_fence;
module_param_named(scale_row_fence, gx_scale_row_fence, bool, 0444);
MODULE_PARM_DESC(scale_row_fence,
		 "Submit and finish each focused direct-color row separately");
static bool gx_scale_single_quad;
module_param_named(scale_single_quad, gx_scale_single_quad, bool, 0444);
MODULE_PARM_DESC(scale_single_quad,
		 "Use one rectangle in the focused direct-color final draw");
static bool gx_scale_direct_color;
module_param_named(scale_direct_color, gx_scale_direct_color, bool, 0444);
MODULE_PARM_DESC(scale_direct_color,
		 "Use untextured row quads for the focused uniform final draw");
static bool gx_scale_cpu_uniform;
module_param_named(scale_cpu_uniform, gx_scale_cpu_uniform, bool, 0444);
MODULE_PARM_DESC(scale_cpu_uniform,
		 "Validate uniform source and fill the entire focused texture extent");
static bool gx_scale_cpu_rgba8;
module_param_named(scale_cpu_rgba8, gx_scale_cpu_rgba8, bool, 0444);
MODULE_PARM_DESC(scale_cpu_rgba8,
		 "Encode the focused CPU texture source as equivalent tiled RGBA8");
static bool gx_scale_cpu_alt;
module_param_named(scale_cpu_alt, gx_scale_cpu_alt, bool, 0444);
MODULE_PARM_DESC(scale_cpu_alt,
		 "Place the focused CPU texture source in the alternate MEM1 slot");
static bool gx_scale_cpu_source;
module_param_named(scale_cpu_source, gx_scale_cpu_source, bool, 0444);
MODULE_PARM_DESC(scale_cpu_source,
		 "Build an exact CPU horizontal texture in the focused scale trace");
static bool gx_scale_cpu_publish;
module_param_named(scale_cpu_publish, gx_scale_cpu_publish, bool, 0444);
MODULE_PARM_DESC(scale_cpu_publish,
		 "CPU-republish the horizontal texture in the focused scale trace");
static bool gx_scale_system_split;
module_param_named(scale_system_split, gx_scale_system_split, bool, 0444);
MODULE_PARM_DESC(scale_system_split,
		 "Halve horizontal primitive height for RGB565 linear 2x system upscale");
static bool gx_scale_offset_split;
module_param_named(scale_offset_split, gx_scale_offset_split, bool, 0444);
MODULE_PARM_DESC(scale_offset_split,
		 "Halve final primitive width for the focused tiled offset enlargement");
static bool gx_scale_native_preserve_fence;
module_param_named(scale_native_preserve_fence, gx_scale_native_preserve_fence, bool, 0444);
MODULE_PARM_DESC(scale_native_preserve_fence,
		 "Fence and snapshot destination preservation in the native trace");
static bool gx_scale_native_horizontal_split;
module_param_named(scale_native_horizontal_split, gx_scale_native_horizontal_split, bool, 0444);
MODULE_PARM_DESC(scale_native_horizontal_split,
		 "Halve horizontal primitive height in the native rectangle trace");
static bool gx_scale_native_split;
module_param_named(scale_native_split, gx_scale_native_split, bool, 0444);
MODULE_PARM_DESC(scale_native_split,
		 "Halve active row width in the focused native rectangle trace");
static bool gx_scale_native_trace;
module_param_named(scale_native_trace, gx_scale_native_trace, bool, 0444);
MODULE_PARM_DESC(scale_native_trace,
		 "Compare native XRGB8888 320x240 rectangles in linear 640x480 objects");
static bool gx_scale_offset_trace;
module_param_named(scale_offset_trace, gx_scale_offset_trace, bool, 0444);
MODULE_PARM_DESC(scale_offset_trace,
		 "Compare the focused tiled offset 255x79-to-256x79 enlargement");
static bool gx_scale_system_trace;
module_param_named(scale_system_trace, gx_scale_system_trace, bool, 0444);
MODULE_PARM_DESC(scale_system_trace,
		 "Compare RGB565 linear 320x240-to-640x480 upscale stages");
static bool gx_scale_efb_full;
module_param_named(scale_efb_full, gx_scale_efb_full, bool, 0444);
MODULE_PARM_DESC(scale_efb_full,
		 "Snapshot the full final EFB during the focused scale trace");
static bool gx_scale_efb_peek;
module_param_named(scale_efb_peek, gx_scale_efb_peek, bool, 0444);
MODULE_PARM_DESC(scale_efb_peek,
		 "Read four final EFB colors during the focused scale trace");
static u32 gx_scale_trace_sequence;
module_param_named(scale_trace, gx_scale_trace, bool, 0444);
MODULE_PARM_DESC(scale_trace,
		 "Hash each stage of the focused 640x240-to-320x120 tiled scale");
#endif

#define GX_XFB_SNAPSHOT_MAX	(640 * 480 * 2)
static void *gx_xfb_snapshot;
static struct dentry *gx_debugfs_dir;
static struct dentry *gx_xfb_debugfs_file;
static size_t gx_xfb_snapshot_size;
static void *gx_vfb_snapshot;
static struct dentry *gx_vfb_debugfs_file;
static size_t gx_vfb_snapshot_size;
static u32 gx_xfb_snapshot_width;
static u32 gx_xfb_snapshot_height;
static u32 gx_xfb_snapshot_phys;

static char *gx_renderer = "generated";
module_param_named(renderer, gx_renderer, charp, 0444);
MODULE_PARM_DESC(renderer, "Framebuffer command path: generated, reference, or direct");

module_param_named(frames, gx_live_texture_frame, uint, 0444);
MODULE_PARM_DESC(frames, "Number of submitted live texture frames");

module_param_named(pe_finishes, gx_pe_finish_count, uint, 0444);
MODULE_PARM_DESC(pe_finishes, "Number of completed PE finish interrupts");

module_param_named(xrgb8888_frames, gx_rgb888_timing_frames, uint, 0444);
MODULE_PARM_DESC(xrgb8888_frames,
		 "Number of submitted XRGB8888 texture frames");

#if IS_ENABLED(CONFIG_FB_GAMECUBE_GX)
static const struct gcnfb_accel_ops gcn_gx_accel_ops;
#endif

static unsigned int gx_hold_frame;
module_param_named(hold_frame, gx_hold_frame, uint, 0444);
MODULE_PARM_DESC(hold_frame, "Publish this frame once, then hold output (0=continuous)");

static bool gx_debug_capture;
module_param_named(debug_capture, gx_debug_capture, bool, 0444);
MODULE_PARM_DESC(debug_capture,
		 "Allocate debugfs VFB/XFB capture buffers (default: false)");

static bool gx_render_only;
module_param_named(render_only, gx_render_only, bool, 0444);
MODULE_PARM_DESC(render_only,
		 "Disable GX scanout while retaining the DRM render provider");

static bool gx_offscreen_probe;
static bool gx_offscreen_texture_ready;
static u32 gx_offscreen_copies;
static u32 gx_offscreen_replays;
static u32 gx_offscreen_changed_words;
module_param_named(offscreen_probe, gx_offscreen_probe, bool, 0444);
MODULE_PARM_DESC(offscreen_probe,
		 "Probe EFB-to-RGB565-texture copy and visible replay");
module_param_named(offscreen_copies, gx_offscreen_copies, uint, 0444);
MODULE_PARM_DESC(offscreen_copies, "Completed EFB-to-texture probe copies");
module_param_named(offscreen_replays, gx_offscreen_replays, uint, 0444);
MODULE_PARM_DESC(offscreen_replays, "Completed offscreen texture replays");
module_param_named(offscreen_changed_words, gx_offscreen_changed_words, uint,
		   0444);
MODULE_PARM_DESC(offscreen_changed_words,
		 "Probe destination words changed from the sentinel value");

static char *gx_texture_source = "console";
module_param_named(texture_source, gx_texture_source, charp, 0444);
MODULE_PARM_DESC(texture_source, "RGB565 texture source: console, pattern, or probe");

static unsigned int gx_probe_seed;
module_param_named(probe_seed, gx_probe_seed, uint, 0444);
MODULE_PARM_DESC(probe_seed, "Seed mixed into the deterministic texture probe");

static int gx_texel_bias_eighths = -2;
module_param_named(texel_bias_eighths, gx_texel_bias_eighths, int, 0444);
MODULE_PARM_DESC(texel_bias_eighths,
		 "Texture-coordinate translation in eighths of a texel");

static char *gx_texcoord_space = "normalized";
module_param_named(texcoord_space, gx_texcoord_space, charp, 0444);
MODULE_PARM_DESC(texcoord_space,
		 "Position-derived texture coordinates: normalized or texel");

static char *gx_texcoord_source = "position";
module_param_named(texcoord_source, gx_texcoord_source, charp, 0444);
MODULE_PARM_DESC(texcoord_source,
		 "Texture-coordinate source: position or direct TEX0");

static char *gx_direct_primitive = "quad";
module_param_named(direct_primitive, gx_direct_primitive, charp, 0444);
MODULE_PARM_DESC(direct_primitive, "Direct-TEX0 primitive: quad or triangle");

static char *gx_direct_pattern_name = "grid";
module_param_named(direct_pattern, gx_direct_pattern_name, charp, 0444);
MODULE_PARM_DESC(direct_pattern, "Texture-free direct-colour pattern: grid or vstripes");

static char *gx_texcoord_mapping = "affine";
module_param_named(texcoord_mapping, gx_texcoord_mapping, charp, 0444);
MODULE_PARM_DESC(texcoord_mapping, "Direct TEX0 mapping: affine or constant");

static bool gx_use_reference;
static bool gx_use_direct;
static bool gx_use_pattern;
static bool gx_use_probe;
static bool gx_use_texel_space;
static bool gx_use_direct_texcoord;
static bool gx_use_direct_triangle;
static bool gx_use_direct_vstripes;
static bool gx_use_constant_texcoord;

static inline u16 pe_read(int reg)
{
	return in_be16(pe_regs + reg);
}

static inline void pe_write(int reg, u16 val)
{
	out_be16(pe_regs + reg, val);
}

static irqreturn_t gx_pe_finish_handler(int irq, void *data)
{
	u16 status = pe_read(PE_REG_INTR_STATUS);
	u32 count;

	/* PE status bits are write-one-to-clear; preserve both enable bits. */
	pe_write(PE_REG_INTR_STATUS, (status & 0x0003) | PE_FINISH_BIT);
	count = READ_ONCE(gx_pe_finish_count) + 1;
	WRITE_ONCE(gx_pe_finish_count, count);
	wake_up_all(&gx_pe_finish_wait);

	return IRQ_HANDLED;
}

/* Set after hardware initialization; guards callbacks during module exit. */
static bool gx_accel_ready;

/* ------------------------------------------------------------------ */
/* Low-level CP / PI / wgPipe helpers                                  */
/* ------------------------------------------------------------------ */

static inline void cp_write(int reg, u16 val)
{
	out_be16(cp_regs + reg, val);
}

static inline u16 cp_read(int reg)
{
	return in_be16(cp_regs + reg);
}

/*
 * PI FIFO registers (u32, big-endian) at 0x0C003000 + offset:
 *   index 3 (0x0C): FIFO_BASE  — physical start of FIFO buffer
 *   index 4 (0x10): FIFO_END   — physical end of FIFO buffer
 *   index 5 (0x14): FIFO_WPTR  — write pointer; wgPipe DMA bursts here
 * All read back as 0x00000000 after mini, meaning wgPipe WPTR = 0
 * (physical address 0x00000000 = kernel exception vectors = crash on first burst).
 */
#define PI_REG_FIFO_BASE	3
#define PI_REG_FIFO_END		4
#define PI_REG_FIFO_WPTR	5

static inline void pi_write(int reg, u32 val)
{
	regmap_write(pi_regmap, reg * sizeof(u32), val);
}

static inline u32 pi_read(int reg)
{
	unsigned int val = 0;

	regmap_read(pi_regmap, reg * sizeof(u32), &val);
	return val;
}

/*
 * GX command byte writers — append to gx_fifo_buf at fifo_pos.
 * gx_submit_cmds() later flushes dcache and advances PI_FIFO_WPTR
 * so the CP picks up the commands, bypassing the wgPipe entirely.
 * (Write-through PTEs for the wgPipe address cause the CPU to attempt
 * a cache-line-fill READ from the write-only wgPipe hardware, hanging
 * the bus; this approach avoids the problem.)
 */
static inline void gx_wr8(u8 val)
{
	((u8 *)gx_fifo_buf)[fifo_pos++] = val;
}

static inline void gx_wr16be(u16 val)
{
	gx_wr8(val >> 8);
	gx_wr8(val & 0xff);
}

static inline void gx_wr32be(u32 val)
{
	gx_wr8(val >> 24);
	gx_wr8((val >> 16) & 0xff);
	gx_wr8((val >> 8) & 0xff);
	gx_wr8(val & 0xff);
}

static inline void gx_load_bp_reg(u32 val)
{
	gx_wr8(GX_CMD_LOAD_BP_REG);
	gx_wr32be(val);
}

static inline void gx_load_cp_reg(u8 reg, u32 val)
{
	gx_wr8(0x08);
	gx_wr8(reg);
	gx_wr32be(val);
}

static inline void gx_load_xf_reg(u32 addr, u32 val)
{
	gx_wr8(0x10);
	gx_wr32be(addr & 0xffff);
	gx_wr32be(val);
}

static inline void gx_load_xf_regs_n(u32 addr, u32 count)
{
	gx_wr8(0x10);
	gx_wr32be(((count - 1) << 16) | (addr & 0xffff));
}

static inline void wg_f32_bits(u32 bits)
{
	gx_wr32be(bits);
}

/* IEEE 754 constants */
#define F32_ZERO	0x00000000U
#define F32_ONE		0x3F800000U
#define F32_NEG_ONE	0xBF800000U
#define F32_16M		0x4B7FFFFFU	/* 16777215.0 */
#define F32_NEG(b)	((b) ^ 0x80000000U)
#define GX_RASTER_DEPTH_MAX	0x00fffffeU

/* f32_from_u16 - encode a u16 integer as IEEE 754 single-precision bits */
static u32 f32_from_u16(u16 n)
{
	u32 msb;

	if (!n)
		return F32_ZERO;
	msb = 31 - __builtin_clz((u32)n);
	return ((127 + msb) << 23) | (((u32)n << (23 - msb)) & 0x7FFFFF);
}

/*
 * f32_div_u32 - compute num/den as IEEE 754 bits using 64-bit fixed-point.
 * Precision: ~40 significant bits; error < 2^-17 relative (adequate for
 * GPU viewport and projection math).
 */
static u32 f32_div_u32(u32 num, u32 den)
{
	u64 q;
	int msb, exp;
	u32 mant;

	if (!num)
		return F32_ZERO;
	q = (u64)num << 40;
	do_div(q, (u32)den);	/* avoids __udivdi3 on 32-bit PowerPC */
	if (!q)
		return F32_ZERO;
	msb = 63 - __builtin_clzll(q);
	exp = 127 + msb - 40;
	if (exp <= 0 || exp >= 255)
		return F32_ZERO;
	if (msb >= 23)
		mant = (u32)((q >> (msb - 23)) & 0x7FFFFF);
	else
		mant = (u32)((q << (23 - msb)) & 0x7FFFFF);
	return ((u32)exp << 23) | mant;
}

static u32 f32_div_u16(u16 num, u16 den)
{
	return f32_div_u32(num, den);
}

static void gx_load_identity_pos_mtx0(void)
{
	/* GX_LoadPosMtxImm(identity, GX_PNMTX0): XF 0x0000–0x000B */
	gx_load_xf_regs_n(0x0000, 12);
	wg_f32_bits(F32_ONE);  wg_f32_bits(F32_ZERO);
	wg_f32_bits(F32_ZERO); wg_f32_bits(F32_ZERO);
	wg_f32_bits(F32_ZERO); wg_f32_bits(F32_ONE);
	wg_f32_bits(F32_ZERO); wg_f32_bits(F32_ZERO);
	wg_f32_bits(F32_ZERO); wg_f32_bits(F32_ZERO);
	wg_f32_bits(F32_ONE);  wg_f32_bits(F32_ZERO);

	/* GX_SetCurrentMtx(GX_PNMTX0): libogc writes CP 0x30 and XF 0x1018. */
	gx_load_cp_reg(0x30, 0);
	gx_load_xf_reg(0x1018, 0);

	/*
	 * GX_LoadTexMtxImm(identity, GX_TEXMTX0, GX_MTX2x4): XF 0x0078–0x007F
	 *
	 * TEXMTX0 starts immediately after PNMTX9 (10 matrices × 12 regs =
	 * 0x78 regs).  Hardware reset value is undefined; garbage here causes
	 * the XF to produce a q ≈ 0 (or NaN) homogeneous texcoord when using
	 * GX_TG_MTX3x4, which permanently stalls the rasterizer perspective-
	 * divide unit when actual vertex TEX0 data drives the transform.
	 *
	 * Identity 2×4 maps (S, T, 1, 0) → (S, T) unchanged.
	 */
	gx_load_xf_regs_n(0x0078, 8);
	wg_f32_bits(F32_ONE);  wg_f32_bits(F32_ZERO);
	wg_f32_bits(F32_ZERO); wg_f32_bits(F32_ZERO);
	wg_f32_bits(F32_ZERO); wg_f32_bits(F32_ONE);
	wg_f32_bits(F32_ZERO); wg_f32_bits(F32_ZERO);
}

static void gx_load_pos_to_tex_mtx0_offset(u16 width, u16 height,
					   int x_offset, int y_offset)
{
	int s_numerator = x_offset * 8 + gx_texel_bias_eighths;
	int t_numerator = y_offset * 8 + gx_texel_bias_eighths;
	u32 s_scale, t_scale, s_bias, t_bias;

	if (gx_use_texel_space) {
		s_scale = F32_ONE;
		t_scale = F32_ONE;
		s_bias = f32_div_u16(abs(s_numerator), 8);
		t_bias = f32_div_u16(abs(t_numerator), 8);
	} else {
		s_scale = f32_div_u16(1, width);
		t_scale = f32_div_u16(1, height);
		s_bias = f32_div_u16(abs(s_numerator), width * 8);
		t_bias = f32_div_u16(abs(t_numerator), height * 8);
	}

	if (s_numerator < 0)
		s_bias = F32_NEG(s_bias);
	if (t_numerator < 0)
		t_bias = F32_NEG(t_bias);

	/*
	 * TEXMTX0 for GX_TG_POS maps object-space quad positions to either
	 * normalized or texel-space coordinates. The configurable translation
	 * preserves the same fraction-of-a-texel phase in both forms.
	 * Rectangle paths keep position-derived coordinates so their sampling
	 * phase remains independent of vertex payloads. Semantic textured
	 * primitives configure the validated direct TEX0 path separately.
	 */
	gx_load_xf_regs_n(0x0078, 8);
	wg_f32_bits(s_scale);  wg_f32_bits(F32_ZERO);
	wg_f32_bits(F32_ZERO); wg_f32_bits(s_bias);
	wg_f32_bits(F32_ZERO); wg_f32_bits(t_scale);
	wg_f32_bits(F32_ZERO); wg_f32_bits(t_bias);

	/*
	 * GX_SetTexCoordGen(..., GX_TEXMTX0) records GX_TEXMTX0 (30) in the
	 * texcoord0 matrix-index field.  libogc writes this to both CP reg 0x30
	 * and XF 0x1018.  Without this, texcoord0 uses matrix index 0 and ignores
	 * TEXMTX0 entirely.
	 */
	gx_load_cp_reg(0x30, 30 << 6);
	gx_load_xf_reg(0x1018, 30 << 6);
}

static void gx_load_pos_to_tex_mtx0(u16 width, u16 height)
{
	gx_load_pos_to_tex_mtx0_offset(width, height, 0, 0);
}

static u32 gx_direct_texcoord_bits(u16 extent, u16 multiple)
{
	int numerator = multiple * extent * 8 + gx_texel_bias_eighths;
	u16 denominator = gx_use_texel_space ? 8 : extent * 8;
	u32 bits = f32_div_u16(abs(numerator), denominator);

	return numerator < 0 ? F32_NEG(bits) : bits;
}

static u32 gx_direct_center_texcoord_bits(u16 extent)
{
	int numerator = extent * 4 + gx_texel_bias_eighths;
	u16 denominator = gx_use_texel_space ? 8 : extent * 8;
	u32 bits = f32_div_u16(abs(numerator), denominator);

	return numerator < 0 ? F32_NEG(bits) : bits;
}

static u32 gx_semantic_texcoord_bits_phase(u16 coordinate, u16 extent,
					   s32 phase_eighths)
{
	s32 numerator = (u32)coordinate * 8 + phase_eighths;
	u32 bits = f32_div_u32(abs(numerator), (u32)extent * 8);

	return numerator < 0 ? F32_NEG(bits) : bits;
}

static u32 gx_semantic_texcoord_bits(u16 coordinate, u16 extent)
{
	return gx_semantic_texcoord_bits_phase(coordinate, extent, -2);
}

static u16 gx_nearest_source_index(u16 dst_index, u16 src_extent,
				   u16 dst_extent)
{
	u32 numerator = (2 * (u32)dst_index + 1) * src_extent;

	return min_t(u32, numerator / (2 * dst_extent), src_extent - 1);
}

static u16 gx_power_of_two_extent(u16 extent)
{
	u16 padded = 4;

	while (padded < extent)
		padded <<= 1;
	return padded;
}

static void gx_load_identity_post_mtx(void)
{
	/* GX_LoadTexMtxImm(identity, GX_DTTIDENTITY, GX_MTX3x4). */
	gx_load_xf_regs_n(0x05f4, 12);
	wg_f32_bits(F32_ONE);  wg_f32_bits(F32_ZERO);
	wg_f32_bits(F32_ZERO); wg_f32_bits(F32_ZERO);
	wg_f32_bits(F32_ZERO); wg_f32_bits(F32_ONE);
	wg_f32_bits(F32_ZERO); wg_f32_bits(F32_ZERO);
	wg_f32_bits(F32_ZERO); wg_f32_bits(F32_ZERO);
	wg_f32_bits(F32_ONE);  wg_f32_bits(F32_ZERO);
}

/* gx_wait_idle - wait for the GP to finish processing the FIFO */
static void gx_wait_idle(void)
{
	int timeout = 1000;

	/*
	 * With endian-correct CP access, SR bit 3 is observed after CP_CTRL=0
	 * and SR bit 2 is observed after the GP has consumed the submitted
	 * FIFO.  Either state is quiescent enough to reprogram the FIFO.
	 */
	while (timeout--) {
		u16 sr = cp_read(CP_REG_STATUS);
		u32 rd, wt;

		if (sr & 0x000c)
			return;
		rd = ((u32)cp_read(CP_REG_RD_HI) << 16) |
			cp_read(CP_REG_RD_LO);
		wt = ((u32)cp_read(CP_REG_WT_HI) << 16) |
			cp_read(CP_REG_WT_LO);
		if (rd == wt)
			return;
		udelay(10);
	}
	pr_warn_once("gcn-gx: timed out waiting for GP idle (SR=0x%04x)\n",
		     cp_read(CP_REG_STATUS));
}

static void __maybe_unused gx_wait_fifo_empty(void)
{
	int timeout = 1000;

	while (timeout--) {
		u32 rd = ((u32)cp_read(CP_REG_RD_HI) << 16) |
			cp_read(CP_REG_RD_LO);
		u32 wt = ((u32)cp_read(CP_REG_WT_HI) << 16) |
			cp_read(CP_REG_WT_LO);

		if (rd == wt)
			return;
		udelay(10);
	}
	pr_warn_once("gcn-gx: timed out waiting for FIFO empty (SR=0x%04x)\n",
		     cp_read(CP_REG_STATUS));
}

/* ------------------------------------------------------------------ */
/* CP / FIFO initialisation                                             */
/* ------------------------------------------------------------------ */

static int gx_fifo_init(void)
{
	/*
	 * Disable CP reads.  Do not write CP BASE/END/WT/RD — writing those
	 * registers causes deferred bus errors or immediate GP faults on this
	 * hardware (bisected over many boots).
	 *
	 * Do not enable GPRESET here either: the first test showed that with
	 * GPRESET active and an empty (zeroed) FIFO, a VI retrace wgPipe burst
	 * feeds zero-bytes to the GP as invalid GX opcodes → crash.  GPRESET
	 * is enabled by gx_submit_cmds() only after valid commands are queued.
	 */
	cp_write(CP_REG_CTRL, 0);

	/*
	 * All remaining setup (PI BASE/END/WPTR, LINKEN, GPRESET) is
	 * deferred to gx_submit_cmds().  LINKEN causes a deferred CP error
	 * when set here because CP_BASE/END still hold mini's invalid values;
	 * in interrupt context at submit time that error can't propagate.
	 */

	return 0;
}

/* ------------------------------------------------------------------ */
/* Texture tiling                                                       */
/* ------------------------------------------------------------------ */

/*
 * gx_tile_rgb565 - convert linear RGB565 to GX 4×4 tiled format.
 *
 * GX stores GX_TF_RGB565 textures as 4×4 pixel blocks (32 bytes each),
 * tiled left-to-right then top-to-bottom. Within a block, pixels are
 * row-major (4 pixels × 2 bytes = 8 bytes/row, 4 rows per block).
 */
static void gx_tile_rgb565(const u16 *src, u16 *dst, u32 width, u32 height,
			   u32 src_pitch)
{
	u32 bw = width >> 2;	/* blocks wide */
	u32 bh = height >> 2;	/* blocks tall */
	u32 tx, ty, row;

	for (ty = 0; ty < bh; ty++) {
		for (tx = 0; tx < bw; tx++) {
			u16 *tile = dst + (ty * bw + tx) * 16;

			for (row = 0; row < 4; row++) {
				const u16 *sl = (const u16 *)
					((const u8 *)src +
					 (ty * 4 + row) * src_pitch) + tx * 4;

				tile[row * 4 + 0] = sl[0];
				tile[row * 4 + 1] = sl[1];
				tile[row * 4 + 2] = sl[2];
				tile[row * 4 + 3] = sl[3];
			}
		}
	}
}

static size_t gx_tiled_rgb565_index(u16 x, u16 y, u16 width)
{
	return ((size_t)(y >> 2) * (width >> 2) + (x >> 2)) * 16 +
	       (y & 3) * 4 + (x & 3);
}

static u32 gx_hash_tiled_region(const u16 *pixels, u16 stride, u16 width,
				u16 height)
{
	u32 hash = 2166136261U;
	u16 x;
	u16 y;

	for (y = 0; y < height; y++) {
		for (x = 0; x < width; x++) {
			hash ^= pixels[gx_tiled_rgb565_index(x, y, stride)];
			hash *= 16777619U;
		}
	}

	return hash;
}

/* Fingerprint authored commands before submission adds its changing token. */
static u32 gx_hash_pending_commands_seed(u32 hash)
{
	const u8 *bytes = gx_fifo_buf;
	u32 i;

	for (i = 0; i < fifo_pos; i++) {
		hash ^= bytes[i];
		hash *= 16777619U;
	}

	return hash;
}

static u32 gx_hash_pending_commands(void)
{
	return gx_hash_pending_commands_seed(2166136261U);
}

struct gx_scale_efb_sample {
	u16 x;
	u16 y;
	u32 argb;
};

/* libogc GX_PeekARGB: physical EFB color aperture, y[21:12], x[11:2]. */
static int gx_peek_scale_colors(struct gx_scale_efb_sample *samples, size_t count)
{
	size_t i;

	for (i = 0; i < count; i++) {
		phys_addr_t phys = 0x08000000 | ((u32)samples[i].y << 12) |
				   ((u32)samples[i].x << 2);
		void __iomem *pixel = ioremap(phys, sizeof(u32));

		if (!pixel)
			return -ENOMEM;
		samples[i].argb = ioread32be(pixel);
		iounmap(pixel);
	}

	return 0;
}

static u16 gx_argb_to_rgb565(u32 argb)
{
	return ((argb >> 8) & 0xf800) | ((argb >> 5) & 0x07e0) |
	       ((argb >> 3) & 0x001f);
}

static int gx_snapshot_scale_colors(u32 *pixels, u16 width, u16 height)
{
	void __iomem *efb = ioremap(0x08000000, (size_t)height << 12);
	u16 x;
	u16 y;

	if (!efb)
		return -ENOMEM;
	for (y = 0; y < height; y++) {
		for (x = 0; x < width; x++)
			pixels[(size_t)y * width + x] =
				ioread32be(efb + ((u32)y << 12) + ((u32)x << 2));
	}
	iounmap(efb);
	return 0;
}

/* Only the full-surface linear RGB565 2x system upscale calls this. */
static void gx_compare_system_scale(const u16 *source, const u16 *output,
				    u16 stride, u16 width, u16 height,
				    const u32 *efb, u32 sequence,
				    const char *stage)
{
	u32 mismatches = 0;
	u32 efb_mismatches = 0;
	u32 copy_mismatches = 0;
	u16 x;
	u16 y;

	for (y = 0; y < height; y++) {
		for (x = 0; x < width; x++) {
			u16 expected = source[(y * 240 / height) * 320 +
					      x * 320 / width];
			u16 actual = output[gx_tiled_rgb565_index(x, y, stride)];
			u32 argb = efb ? efb[(size_t)y * width + x] : 0;
			u16 rendered = efb ? gx_argb_to_rgb565(argb) : actual;

			if ((actual != expected || rendered != expected) &&
			    !mismatches && !efb_mismatches)
				pr_info("gcn-gx: system-scale-first seq=%u stage=%s x=%u y=%u actual=%04x expected=%04x argb=%08x efb=%04x\n",
					sequence, stage, x, y, actual, expected,
					argb, rendered);
			mismatches += actual != expected;
			efb_mismatches += efb && rendered != expected;
			copy_mismatches += efb && rendered != actual;
		}
	}
	pr_info("gcn-gx: system-scale seq=%u stage=%s pixels=%u mismatches=%u efb_mismatches=%u copy_mismatches=%u\n",
		sequence, stage, width * height, mismatches, efb_mismatches,
		copy_mismatches);
}

/* Exact offset test: tiled 256x256 surfaces, source y=43, destination y=97. */
static void gx_compare_offset_scale(const u16 *source, const u16 *prior,
				    const u16 *output, const u32 *efb,
				    u32 sequence, unsigned int stage)
{
	const char *name = stage == 0 ? "crop" : stage == 1 ? "horizontal" : "final";
	u16 width = stage == 0 ? 255 : 256;
	u16 height = stage == 2 ? 256 : 79;
	u32 mismatches = 0;
	u32 efb_mismatches = 0;
	u32 copy_mismatches = 0;
	u16 x;
	u16 y;

	for (y = 0; y < height; y++) {
		for (x = 0; x < width; x++) {
			size_t index = gx_tiled_rgb565_index(x, y, 256);
			u16 sx = stage == 0 ? x : (2 * x + 1) * 255 / 512;
			u16 sy = stage == 2 ? y - 97 : y;
			u16 expected;
			u16 actual = output[index];
			u32 argb = efb[(size_t)y * width + x];
			u16 rendered = gx_argb_to_rgb565(argb);

			if (stage == 2 && (y < 97 || y >= 176))
				expected = prior[index];
			else
				expected = source[gx_tiled_rgb565_index(sx, sy + 43, 256)];
			if ((actual != expected || rendered != expected) &&
			    !mismatches && !efb_mismatches)
				pr_info("gcn-gx: offset-first seq=%u stage=%s x=%u y=%u actual=%04x expected=%04x argb=%08x efb=%04x\n",
					sequence, name, x, y, actual, expected,
					argb, rendered);
			mismatches += actual != expected;
			efb_mismatches += rendered != expected;
			copy_mismatches += rendered != actual;
		}
	}
	pr_info("gcn-gx: offset-scale seq=%u stage=%s pixels=%u mismatches=%u efb_mismatches=%u copy_mismatches=%u\n",
		sequence, name, width * height, mismatches, efb_mismatches,
		copy_mismatches);
}

static void gx_compare_native_prior(const u16 *prior, const u16 *texture,
				    const u32 *efb, u32 sequence)
{
	u32 texture_mismatches = 0;
	u32 efb_mismatches = 0;
	u16 x;
	u16 y;

	for (y = 0; y < 480; y++) {
		for (x = 0; x < 640; x++) {
			size_t linear = (size_t)y * 640 + x;
			u16 expected = prior[linear];
			u16 uploaded = texture[gx_tiled_rgb565_index(x, y, 640)];
			u32 argb = efb ? efb[linear] : 0;
			u16 rendered = efb ? gx_argb_to_rgb565(argb) : expected;

			if ((uploaded != expected || rendered != expected) &&
			    !texture_mismatches && !efb_mismatches)
				pr_info("gcn-gx: native-prior-first seq=%u snapshot=%u x=%u y=%u uploaded=%04x expected=%04x argb=%08x efb=%04x\n",
					sequence, !!efb, x, y, uploaded, expected,
					argb, rendered);
			texture_mismatches += uploaded != expected;
			efb_mismatches += rendered != expected;
		}
	}
	pr_info("gcn-gx: native-prior seq=%u snapshot=%u pixels=307200 texture_mismatches=%u efb_mismatches=%u\n",
		sequence, !!efb, texture_mismatches, efb_mismatches);
}

/* Four native-resolution rectangles; prior destination is linear RGB565. */
static void gx_compare_native_scale(const u32 *source, const u16 *prior,
				    const u16 *output, const u32 *efb,
				    u16 origin_x, u16 origin_y, u32 sequence,
				    unsigned int stage)
{
	const char *name = stage == 0 ? "crop" : stage == 1 ? "horizontal" : "final";
	u16 width = stage == 2 ? 640 : 320;
	u16 height = stage == 2 ? 480 : 240;
	u16 stride = stage == 2 ? 640 : 512;
	u32 mismatches = 0;
	u32 efb_mismatches = 0;
	u32 copy_mismatches = 0;
	u16 x;
	u16 y;

	for (y = 0; y < height; y++) {
		for (x = 0; x < width; x++) {
			u16 sx = stage == 2 ? x : x + origin_x;
			u16 sy = stage == 2 ? y : y + origin_y;
			u16 expected;
			u16 actual = output[gx_tiled_rgb565_index(x, y, stride)];
			u32 argb = efb ? efb[(size_t)y * width + x] : 0;
			u16 rendered = efb ? gx_argb_to_rgb565(argb) : actual;

			if (stage == 2 && (x < origin_x || x >= origin_x + 320 ||
					   y < origin_y || y >= origin_y + 240))
				expected = prior[(size_t)y * 640 + x];
			else
				expected = gx_argb_to_rgb565(source[(size_t)sy * 640 + sx]);
			if ((actual != expected || rendered != expected) &&
			    !mismatches && !efb_mismatches)
				pr_info("gcn-gx: native-first seq=%u origin=%u,%u stage=%s x=%u y=%u actual=%04x expected=%04x argb=%08x efb=%04x\n",
					sequence, origin_x, origin_y, name, x, y,
					actual, expected, argb, rendered);
			mismatches += actual != expected;
			efb_mismatches += efb && rendered != expected;
			copy_mismatches += efb && rendered != actual;
		}
	}
	pr_info("gcn-gx: native-scale seq=%u origin=%u,%u stage=%s pixels=%u mismatches=%u efb_mismatches=%u copy_mismatches=%u\n",
		sequence, origin_x, origin_y, name, width * height, mismatches,
		efb_mismatches, copy_mismatches);
}

static void gx_compare_scale_colors(const u32 *pixels, const u16 *output,
				    const u16 *source, u32 sequence)
{
	u32 copy_mismatches = 0;
	u32 source_mismatches = 0;
	u16 x;
	u16 y;

	/* Only called within the exact 640x240-to-320x120 trace gate. */
	for (y = 0; y < 120; y++) {
		for (x = 0; x < 320; x++) {
			u32 argb = pixels[(size_t)y * 320 + x];
			u16 efb = gx_argb_to_rgb565(argb);
			u16 copied = output[gx_tiled_rgb565_index(x, y, 320)];
			u16 expected = source[gx_tiled_rgb565_index(2 * x + 1,
								  2 * y + 1, 640)];

			if ((efb != copied || efb != expected) &&
			    !copy_mismatches && !source_mismatches)
				pr_info("gcn-gx: scale-efb-first seq=%u x=%u y=%u argb=%08x rgb565=%04x copied=%04x expected=%04x\n",
					sequence, x, y, argb, efb, copied, expected);
			copy_mismatches += efb != copied;
			source_mismatches += efb != expected;
		}
	}
	pr_info("gcn-gx: scale-efb-full seq=%u pixels=38400 copy_mismatches=%u source_mismatches=%u\n",
		sequence, copy_mismatches, source_mismatches);
}

static void gx_cpu_republish_texture(void *pixels, size_t bytes)
{
	u32 *words = pixels;
	size_t i;

	invalidate_dcache_range((unsigned long)pixels,
				(unsigned long)pixels + bytes);
	/* Force real CPU stores even though each word retains its value. */
	for (i = 0; i < bytes / sizeof(*words); i++) {
		u32 value = READ_ONCE(words[i]);

		WRITE_ONCE(words[i], value);
	}
	flush_dcache_range((unsigned long)pixels,
			   (unsigned long)pixels + bytes);
	invalidate_dcache_range((unsigned long)pixels,
				(unsigned long)pixels + bytes);
}

/* GX RGBA8 uses separate 32-byte A/R and G/B planes in each 4x4 tile. */
static size_t gx_scale_rgba8_offset(u16 x, u16 y, u16 width)
{
	size_t index = gx_tiled_rgb565_index(x, y, width);

	return (index / 16) * 64 + (index % 16) * 2;
}

static void gx_scale_store_rgba8(u8 *pixels, u16 x, u16 y, u16 width, u16 color)
{
	size_t offset = gx_scale_rgba8_offset(x, y, width);
	u8 r = (color >> 11) & 31;
	u8 g = (color >> 5) & 63;
	u8 b = color & 31;

	pixels[offset] = 255;
	pixels[offset + 1] = (r << 3) | (r >> 2);
	pixels[offset + 32] = (g << 2) | (g >> 4);
	pixels[offset + 33] = (b << 3) | (b >> 2);
}

static u32 gx_hash_scale_rgba8(const u8 *pixels, u16 stride, u16 width,
			       u16 height)
{
	u32 hash = 2166136261U;
	u16 x;
	u16 y;

	for (y = 0; y < height; y++) {
		for (x = 0; x < width; x++) {
			size_t offset = gx_scale_rgba8_offset(x, y, stride);
			u32 argb = ((u32)pixels[offset + 1] << 16) |
				   ((u32)pixels[offset + 32] << 8) |
				   pixels[offset + 33];

			hash ^= gx_argb_to_rgb565(argb);
			hash *= 16777619U;
		}
	}
	return hash;
}

static size_t gx_rgb565_index(u16 x, u16 y, u16 width, u32 layout)
{
	if (layout == DRM_GCN_GEM_LAYOUT_LINEAR)
		return (size_t)y * width + x;

	return gx_tiled_rgb565_index(x, y, width);
}

static void gx_copy_rect_to_tiled(const u16 *src, u16 src_width,
				  u16 src_x, u16 src_y, u32 src_layout,
				  u16 *dst, u16 dst_width, u16 dst_height,
				  u16 rect_width, u16 rect_height)
{
	u16 x;
	u16 y;

	memset(dst, 0, (size_t)dst_width * dst_height * sizeof(*dst));
	for (y = 0; y < rect_height; y++) {
		for (x = 0; x < rect_width; x++) {
			dst[gx_tiled_rgb565_index(x, y, dst_width)] =
				src[gx_rgb565_index(src_x + x, src_y + y,
						      src_width, src_layout)];
		}
	}
}

static void
gx_copy_xrgb8888_rect_to_tiled(const u32 *src, u16 src_width,
			       u16 src_x, u16 src_y, u16 *dst,
			       u16 dst_width, u16 dst_height,
			       u16 rect_width, u16 rect_height)
{
	u16 x;
	u16 y;

	memset(dst, 0, (size_t)dst_width * dst_height * sizeof(*dst));
	for (y = 0; y < rect_height; y++) {
		for (x = 0; x < rect_width; x++) {
			u32 pixel = src[(size_t)(src_y + y) * src_width + src_x + x];

			dst[gx_tiled_rgb565_index(x, y, dst_width)] =
				((pixel >> 8) & 0xf800) |
				((pixel >> 5) & 0x07e0) |
				((pixel >> 3) & 0x001f);
		}
	}
}

static void gx_copy_tiled_to_layout(const u16 *src, u16 *dst, u16 width,
				    u16 height, u32 layout)
{
	u16 x;
	u16 y;

	if (layout == DRM_GCN_GEM_LAYOUT_TILED_4X4) {
		memcpy(dst, src, (size_t)width * height * sizeof(*dst));
		return;
	}

	for (y = 0; y < height; y++) {
		for (x = 0; x < width; x++)
			dst[(size_t)y * width + x] =
				src[gx_tiled_rgb565_index(x, y, width)];
	}
}

static u16 gx_reference_rgb565_pixel(u32 x, u32 y, u32 width, u32 height)
{
	u16 color;

	if (x < width / 2 && y < height / 2)
		color = 0xf800;
	else if (x >= width / 2 && y < height / 2)
		color = 0x0410;
	else if (x < width / 2)
		color = 0x001f;
	else
		color = 0xffff;

	if ((x % 32) == 0 || (y % 32) == 0)
		color = 0x0000;
	if (x == y || x + y == width - 1)
		color = 0xffe0;

	return color;
}

static void gx_fill_reference_rgb565(u16 *dst, u32 width, u32 height)
{
	u32 bw = width >> 2;
	u32 bh = height >> 2;
	u32 tx, ty, x, y;

	for (ty = 0; ty < bh; ty++) {
		for (tx = 0; tx < bw; tx++) {
			u16 *tile = dst + (ty * bw + tx) * 16;

			for (y = 0; y < 4; y++) {
				for (x = 0; x < 4; x++) {
					u32 px = tx * 4 + x;
					u32 py = ty * 4 + y;

					tile[y * 4 + x] =
						gx_reference_rgb565_pixel(px, py,
									  width, height);
				}
			}
		}
	}
}

static u16 gx_probe_rgb565_pixel(u32 x, u32 y)
{
	u32 hash = x * 0x1f123bb5U ^ y * 0x5f356495U;
	u32 level, c5, c6;

	hash ^= gx_probe_seed * 0x9e3779b9U;
	hash ^= hash >> 15;
	hash *= 0x2c1b3c6dU;
	hash ^= hash >> 12;
	level = hash >> 29;
	c5 = (level * 31 + 3) / 7;
	c6 = level * 9;
	return (c5 << 11) | (c6 << 5) | c5;
}

static void gx_fill_probe_rgb565(u16 *dst, u32 width, u32 height)
{
	u32 bw = width >> 2;
	u32 bh = height >> 2;
	u32 tx, ty, x, y;

	for (ty = 0; ty < bh; ty++) {
		for (tx = 0; tx < bw; tx++) {
			u16 *tile = dst + (ty * bw + tx) * 16;

			for (y = 0; y < 4; y++) {
				for (x = 0; x < 4; x++)
					tile[y * 4 + x] =
						gx_probe_rgb565_pixel(tx * 4 + x,
								      ty * 4 + y);
			}
		}
	}
}

static void gx_tile_rgb888(const u32 *src, u16 *dst, u32 width, u32 height,
			   u32 src_pitch);

static void gx_prepare_texture(const void *vfb, u16 *dst, u32 width,
			       u32 height, u32 src_pitch,
			       enum gx_vfb_format format)
{
	if (gx_use_pattern)
		gx_fill_reference_rgb565(dst, width, height);
	else if (gx_use_probe)
		gx_fill_probe_rgb565(dst, width, height);
	else if (format == GX_VFB_XRGB8888)
		gx_tile_rgb888(vfb, dst, width, height, src_pitch);
	else
		gx_tile_rgb565((const u16 *)vfb, dst, width, height, src_pitch);
}

static void __maybe_unused gx_invert_rgb565_texture(u16 *buf, u32 width,
					      u32 height)
{
	u32 i;

	for (i = 0; i < width * height; i++)
		buf[i] ^= 0xffff;
}

/*
 * gx_tile_rgb888 - convert linear RGB888 (packed u32) to GX RGB565 tiles.
 *
 * Converts to RGB565 during tiling to avoid the complex GX_TF_RGBA8
 * interleaved block layout. Minor quality loss (5-6-5 truncation).
 */
static void gx_tile_rgb888(const u32 *src, u16 *dst, u32 width, u32 height,
			   u32 src_pitch)
{
	u32 bw = width >> 2;
	u32 bh = height >> 2;
	u32 tx, ty, row;

	for (ty = 0; ty < bh; ty++) {
		for (tx = 0; tx < bw; tx++) {
			u16 *tile = dst + (ty * bw + tx) * 16;

			for (row = 0; row < 4; row++) {
				const u32 *sl = (const u32 *)
					((const u8 *)src +
					 (ty * 4 + row) * src_pitch) + tx * 4;
				int col;

				for (col = 0; col < 4; col++) {
					u32 p = sl[col];

					tile[row * 4 + col] =
						((p >> 8) & 0xf800) |
						((p >> 5) & 0x07e0) |
						((p >> 3) & 0x001f);
				}
			}
		}
	}
}

/* ------------------------------------------------------------------ */
/* GX 2-D rendering state                                              */
/* ------------------------------------------------------------------ */

/*
 * gx_setup_2d_state - configure GX for a fullscreen textured 2-D blit.
 *
 * Programs BP, XF, and CP registers for single-TEV-stage passthrough
 * render from texture map 0 to the full EFB viewport.  Values derived
 * from libogc gx.c and YAGCD.
 */
static void __maybe_unused gx_setup_2d_state(u16 width, u16 height)
{
	u32 xo, yo;

	/* ---- BP 0x40: zMode — disable Z compare and Z write for 2D ----
	 * bit 0 = enable, bit 7 = update enable; both 0 = fully disabled.
	 * If mini left Z-compare enabled (e.g. GX_LEQUAL), all pixels rendered
	 * to a fresh EFB would fail the test and nothing would reach the EFB.
	 */
	gx_load_bp_reg(0x40000000);

	/* ---- BP 0x41: blendMode ----
	 * Hardware reset value is 0x00: colorupdate=0, alphaupdate=0 — the PE
	 * silently drops every rasterized pixel without writing the EFB.
	 * bit[3] = colorupdate = 1, bit[4] = alphaupdate = 1, blend disabled.
	 */
	gx_load_bp_reg(0x41000018);

	/*
	 * BP 0x43: PE control.  Match libogc's GX_SetZCompLoc(GX_TRUE) +
	 * GX_SetPixelFmt(GX_PF_RGB8_Z24, GX_ZC_LINEAR): RGB8/Z24 EFB, linear Z,
	 * and Z compare before texture.  If mini left a different EFB pixel
	 * format behind, rasterized pixels can be stored/copied incorrectly.
	 */
	gx_load_bp_reg(0x43000040);

	/* ---- BP 0xF3: alphaCompare ----
	 * Hardware reset value is 0x00: comp0 = NEVER (0), comp1 = NEVER (0),
	 * logic = AND (0).  NEVER AND NEVER = ALWAYS_FAIL — every rasterized
	 * fragment is discarded by the alpha test before it can reach the PE,
	 * making colorupdate meaningless.
	 *
	 * Bit layout (Dolphin BPMemory.h AlphaTest):
	 *   [7:0]   ref0   = 0
	 *   [15:8]  ref1   = 0
	 *   [18:16] comp0  = 7 (GX_ALWAYS)
	 *   [21:19] comp1  = 7 (GX_ALWAYS)
	 *   [23:22] logic  = 0 (GX_AOP_AND)
	 * 0xF33F0000 → ref0=0, ref1=0, comp0=ALWAYS, comp1=ALWAYS, logic=AND
	 * Result: every fragment passes, no pixels are discarded.
	 */
	gx_load_bp_reg(0xF33F0000);

	/* ---- BP 0x00: genMode ----
	 * [2:0]   numtexgens = 1
	 * [6:4]   numcolchans = 0
	 * [13:10] numtevstages - 1 = 0
	 */
	gx_load_bp_reg(0x00000001);

	/* ---- BP 0x20/0x21: scissor ----
	 * GX adds 0x156 (342) to all coordinates internally.
	 */
	xo = 0x156;
	yo = 0x156;
	gx_load_bp_reg(0x20000000 | ((xo & 0x7ff) << 12) | (yo & 0x7ff));
	gx_load_bp_reg(0x21000000 |
		       (((xo + width  - 1) & 0x7ff) << 12) |
		       ((yo + height - 1) & 0xfff));

	/* ---- BP 0xC0: TEV stage 0 colour input ----
	 * formula: result = (a*(1-c) + b*c + d) * scale
	 * a=[15:12], b=[11:8], c=[7:4], d=[3:0]  (CC_TEXC=8, CC_ZERO=15)
	 * a=b=c=ZERO(15), d=TEXC(8) → output = texture colour
	 * clamp=[19]=1, all other op bits = 0
	 */
	gx_load_bp_reg(0xC008FFF8);

	/* ---- BP 0xC1: TEV stage 0 alpha input ----
	 * a=[15:13], b=[12:10], c=[9:7], d=[6:4]  (CA_TEXA=4, CA_ZERO=7)
	 * a=b=c=ZERO(7), d=TEXA(4) → output = texture alpha
	 * clamp=[19]=1
	 */
	gx_load_bp_reg(0xC108FFC0);

	/* ---- BP 0x25: TEV order stages 0/1 (tevRasOrder[0]) ----
	 * [2:0] texmap=0, [5:3] texcoord=0, [6] texenable=1,
	 * [9:7] rascolor=GX_ALPHA_BUMP=7
	 */
	gx_load_bp_reg(0x250003C0);

	/* ---- XF 0x103f: numtexcoord generators = 1 ---- */
	gx_load_xf_reg(0x103f, 1);

	/* ---- XF 0x1040: texCoordGen[0] ----
	 * Hardware TexMtxInfo bit layout (Dolphin XFMemory.h):
	 *   bit[0]    = projection (0=ST output/no divide, 1=STQ output/perspective divide)
	 *   bits[3:1] = inputform+texgentype (0 = regular matrix multiply)
	 *   bits[11:7] = sourcerow (4 = GX_TG_TEX0)
	 *
	 * CRITICAL: bit[0]=1 (projection=1) tells XF to produce 3 components (STQ)
	 * for perspective divide.  With only a 2×4 matrix at TEXMTX0 (2 rows, no
	 * Q row), the hardware reads a garbage/zero Q from the uninitialized 3rd
	 * row and the rasterizer hangs on ST/Q divide-by-zero from frame 2 onward
	 * (SR=0x0004, CmdIdle stall).
	 *
	 * 0x200 = (srcrow=4 << 7) | 0 → projection=0 (ST), type=regular, src=TEX0.
	 * Matches the 2×4 identity matrix at XF 0x0078 (2-row, no Q component).
	 */
	gx_load_xf_reg(0x1040, 0x200);

	/* ---- XF 0x1050: texCoordGen2[0] ----
	 * normalize=0, postmtx=GX_DTTIDENTITY=63
	 */
	gx_load_xf_reg(0x1050, 0x3F);

	gx_load_identity_pos_mtx0();

	/* ---- XF 0x101a-0x101f: viewport ----
	 * GX_SetViewport(0, 0, w, h, 0, 1):
	 *   x0=w/2, y0=-h/2, z=16777215, x1=w/2+342, y1=h/2+342, f=16777215
	 */
	gx_load_xf_regs_n(0x101a, 6);
	wg_f32_bits(f32_from_u16(width >> 1));
	wg_f32_bits(F32_NEG(f32_from_u16(height >> 1)));
	wg_f32_bits(F32_16M);
	wg_f32_bits(f32_from_u16((width >> 1) + 342));
	wg_f32_bits(f32_from_u16((height >> 1) + 342));
	wg_f32_bits(F32_16M);

	/* ---- XF 0x1020-0x1026: orthographic projection ----
	 * Maps pixel coords [0,w]×[0,h] to NDC [-1,1]×[-1,1] (Y flipped):
	 *   mt[0][0]=2/w, mt[0][3]=-1, mt[1][1]=-2/h, mt[1][3]=1,
	 *   mt[2][2]=-1,  mt[2][3]=0,  type=GX_ORTHOGRAPHIC=1
	 */
	gx_load_xf_regs_n(0x1020, 7);
	wg_f32_bits(f32_div_u16(2, width));
	wg_f32_bits(F32_NEG_ONE);
	wg_f32_bits(F32_NEG(f32_div_u16(2, height)));
	wg_f32_bits(F32_ONE);
	wg_f32_bits(F32_NEG_ONE);
	wg_f32_bits(F32_ZERO);
	gx_wr32be(1);			/* GX_ORTHOGRAPHIC */

	/* ---- CP 0x50/0x60: vertex descriptor ----
	 * VCD_LO [10:9] = GX_VA_POS = GX_DIRECT(1) → 0x200
	 * VCD_HI [1:0]  = GX_VA_TEX0 = GX_DIRECT(1) → 0x001
	 */
	gx_load_cp_reg(0x50, 0x200);
	gx_load_cp_reg(0x60, 0x001);

	/* ---- CP 0x70-0x90: VTXFMT0 attribute format ----
	 * VAT0: pos cnt=[0]=GX_POS_XY(0), type=[3:1]=GX_F32(4) → 0x08
	 *       tex0 cnt=[21]=GX_TEX_ST(1), type=[24:22]=GX_F32(4) → 0x1200000
	 *       bit[30] = libogc validity marker = 0x40000000
	 * VAT1: 0x80000000 (libogc __GX_InitRevBits default)
	 * VAT2: 0x00000000
	 */
	gx_load_cp_reg(0x70, 0x41200008);
	gx_load_cp_reg(0x80, 0x80000000);
	gx_load_cp_reg(0x90, 0x00000000);
}

static void __maybe_unused gx_setup_texcoord_parse_state(u16 width, u16 height)
{
	u32 xo, yo;

	gx_load_bp_reg(0x40000000);	/* Z disabled */
	gx_load_bp_reg(0x41000018);	/* colour/alpha update enabled */
	gx_load_bp_reg(0x43000040);	/* RGB8/Z24 EFB, linear Z, zcomp before tex */
	gx_load_bp_reg(0x44000003);	/* GX_SetFieldMask(GX_TRUE, GX_TRUE) */
	gx_load_bp_reg(0x68000000);	/* GX_SetFieldMode(GX_FALSE, GX_FALSE) */
	gx_load_bp_reg(0xF33F0000);	/* alpha test always passes */

	/* genMode: 1 texgen, 0 colour channels, 1 TEV stage */
	gx_load_bp_reg(0x00000001);

	xo = 0x156;
	yo = 0x156;
	gx_load_bp_reg(0x20000000 | ((xo & 0x7ff) << 12) | (yo & 0x7ff));
	gx_load_bp_reg(0x21000000 |
		       (((xo + width  - 1) & 0x7ff) << 12) |
		       ((yo + height - 1) & 0xfff));

	/* TEV stage 0: output texture colour/alpha.
	 * raschan=7 (GX_COLOR_NULL, bits[9:7]=0b111 → 0x380): with numcolchans=0
	 * there is no raster colour token in the pipeline; raschan=0 (GX_COLOR0A0)
	 * causes the TEV to wait forever for a colour that never arrives, stalling
	 * the entire backend from frame 2 onwards (SR stays 0x0004).
	 * texenable=1 (bit[6]=1): TMU fetch from texmap 0.
	 */
	gx_load_bp_reg(0xC008FFF8);
	gx_load_bp_reg(0xC108FFC0);
	gx_load_bp_reg(0x250003C0);

	/*
	 * DIAGNOSTIC: source texcoord 0 from position, not direct TEX0 payload.
	 * TEXMTX0 scales pixel XY into normalized ST for texture fetch.
	 */
	gx_load_xf_reg(0x103f, 1);
	gx_load_xf_reg(0x1040, 0x004);
	gx_load_xf_reg(0x1050, 0x3F);
	gx_load_identity_pos_mtx0();
	gx_load_pos_to_tex_mtx0(width, height);

	gx_load_xf_regs_n(0x101a, 6);
	wg_f32_bits(f32_from_u16(width >> 1));
	wg_f32_bits(F32_NEG(f32_from_u16(height >> 1)));
	wg_f32_bits(F32_16M);
	wg_f32_bits(f32_from_u16((width >> 1) + 342));
	wg_f32_bits(f32_from_u16((height >> 1) + 342));
	wg_f32_bits(F32_16M);

	gx_load_xf_regs_n(0x1020, 7);
	wg_f32_bits(F32_ONE);
	wg_f32_bits(F32_ZERO);
	wg_f32_bits(F32_ONE);
	wg_f32_bits(F32_ZERO);
	wg_f32_bits(F32_ONE);
	wg_f32_bits(F32_ZERO);
	gx_wr32be(1);

	/*
	 * BP 0x30/0x31: suSsize/suTsize for texcoord 0.
	 * The rasterizer uses these to compute per-pixel texcoord stepping and
	 * LOD derivatives even when TEV texture fetch is disabled (texenable=0).
	 * Without explicit values the rasterizer uses whatever mini left, which
	 * may not match our large unnormalized texcoords (0..width, 0..height),
	 * causing it to permanently stall on the first non-zero vertex (frame 2).
	 */
	gx_load_bp_reg(0x30000000 | (u32)(width  - 1));
	gx_load_bp_reg(0x31000000 | (u32)(height - 1));

	/* VCD/VAT: direct XY position only; no TEX0 attribute in the FIFO. */
	gx_load_cp_reg(0x50, 0x200);
	gx_load_cp_reg(0x60, 0x000);
	gx_load_cp_reg(0x70, 0x40000008);
	gx_load_cp_reg(0x80, 0x80000000);
	gx_load_cp_reg(0x90, 0x00000000);
}

static void __maybe_unused gx_setup_constant_white_state(u16 width, u16 height)
{
	u32 xo, yo;

	/*
	 * DIAGNOSTIC: the actual devkitPro wii-examples GX triangle demo
	 * (graphics/gx/triangle/source/triangle.c, fetched as a known-working
	 * reference) enables Z-testing -- GX_SetZMode(GX_TRUE, GX_LEQUAL,
	 * GX_TRUE) -- with GX_POS_XYZ vertices, unlike our long-standing
	 * Z-disabled/XY-only setup.  Our copy-clear already writes BP 0x51 =
	 * 0x00ffffff (Z-clear to far/max), so a Z=0 (near) vertex should pass
	 * LEQUAL cleanly -- no obvious confound.  This exact combination (Z
	 * enabled + XYZ together) has not been tried; earlier XYZ-position
	 * tests in this project used Z disabled.  bit0=enable(1),
	 * bits[3:1]=func(GX_LEQUAL=3)<<1=0x6, bit4=update(1)<<4=0x10 -> 0x17.
	 */
	gx_load_bp_reg(0x40000017);	/* GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE) */
	gx_load_bp_reg(0x41000018);	/* colour/alpha update enabled */
	/*
	 * DIAGNOSTIC (PE control-block gap test): BP 0x42 (peCMode1 / dst-alpha)
	 * sits between 0x41 (BLENDMODE, a confirmed pixel-discard trap -- see
	 * Known pitfalls) and 0x43 (PE_CONTROL, which we do set), but this
	 * driver has never written it anywhere.  libogc's GX_Init() explicitly
	 * sets it via GX_SetDstAlpha(GX_DISABLE, 0).  Every raster-state
	 * permutation tried so far shares this same gap; this fills it with
	 * the libogc default (dst-alpha disabled, value 0) to test whether an
	 * uninitialised "mini" leftover here is gating pixel writes.
	 */
	gx_load_bp_reg(0x42000000);	/* GX_SetDstAlpha(GX_DISABLE, 0) */
	gx_load_bp_reg(0x43000040);	/* RGB8/Z24 EFB, linear Z, zcomp before tex */
	gx_load_bp_reg(0x44000003);	/* GX_SetFieldMask(GX_TRUE, GX_TRUE) */
	gx_load_bp_reg(0x68000000);	/* GX_SetFieldMode(GX_FALSE, GX_FALSE) */
	gx_load_bp_reg(0xF33F0000);	/* alpha test always passes */

	/* genMode: 0 texgens, 0 colour channels, 1 TEV stage */
	gx_load_bp_reg(0x00000000);

	/* Full-screen primitive coverage. */
	xo = 0x156;
	yo = 0x156;
	gx_load_bp_reg(0x20000000 | ((xo & 0x7ff) << 12) | (yo & 0x7ff));
	gx_load_bp_reg(0x21000000 |
		       (((xo + width  - 1) & 0x7ff) << 12) |
		       ((yo + height - 1) & 0xfff));
	gx_load_bp_reg(0x59000000);	/* GX_SetScissorBoxOffset(0, 0) */

	/*
	 * DIAGNOSTIC (control/sanity check): d=ONE gave near-black, d=HALF
	 * scale=1x gave mid-tone, d=HALF scale=2x gave complete invisibility
	 * (still-green, no write) -- three non-obviously-related outcomes.
	 * Before trusting any of them further, re-deploy the exact d=HALF
	 * scale=1x configuration unchanged to confirm the mid-tone result is
	 * still reproducible (rules out build/card-state drift as a
	 * confound). See docs/gx-accel-handoff-2026-07-02.md.
	 */
	gx_load_bp_reg(0xC008FFFD);	/* a=b=c=ZERO, d=GX_CC_HALF, scale=1x */
	gx_load_bp_reg(0xC108FFF0);	/* alpha = ZERO */
	gx_load_bp_reg(0x25000380);	/* raschan = GX_COLOR_NULL, tex disabled */

	/*
	 * XF: zero colour channels and zero texcoord generators.
	 */
	gx_load_xf_reg(0x1008, 0x00000000);
	gx_load_xf_reg(0x1009, 0x00000000);
	gx_load_xf_reg(0x100e, 0x00000401);
	gx_load_xf_reg(0x1010, 0x00000401);
	/*
	 * GX_CLIP_DISABLE was tested here (oversized clip-space geometry vs.
	 * the XF clipper) and ruled out: frame 2 still read back the known
	 * "still green" signature. Reverted to the normal GX_CLIP_ENABLE
	 * baseline; see docs/gx-accel-handoff-2026-07-02.md.
	 */
	gx_load_xf_reg(0x1005, 0);	/* GX_SetClipMode(GX_CLIP_ENABLE) */
	gx_load_xf_reg(0x103f, 0);

	gx_load_identity_pos_mtx0();

	gx_load_xf_regs_n(0x101a, 6);
	wg_f32_bits(f32_from_u16(width >> 1));
	wg_f32_bits(F32_NEG(f32_from_u16(height >> 1)));
	wg_f32_bits(F32_16M);
	wg_f32_bits(f32_from_u16((width >> 1) + 342));
	wg_f32_bits(f32_from_u16((height >> 1) + 342));
	wg_f32_bits(F32_16M);

	gx_load_xf_regs_n(0x1020, 7);
	wg_f32_bits(F32_ONE);
	wg_f32_bits(F32_ZERO);
	wg_f32_bits(F32_ONE);
	wg_f32_bits(F32_ZERO);
	wg_f32_bits(F32_ONE);
	wg_f32_bits(F32_ZERO);
	gx_wr32be(1);

	/*
	 * VCD/VAT: direct XYZ position (matching the Z-enable test above).
	 * VAT0 bit0=GX_POS_XYZ(1), bits[3:1]=GX_F32(4)<<1=0x08 -> 0x09.
	 */
	gx_load_cp_reg(0x50, 0x0200);
	gx_load_cp_reg(0x60, 0x0000);
	gx_load_cp_reg(0x70, 0x40000009);
	gx_load_cp_reg(0x80, 0x80000000);
	gx_load_cp_reg(0x90, 0x00000000);
}

#if IS_ENABLED(CONFIG_DRM_GCN_GX)
static void gx_set_viewport(u16 x, u16 y, u16 width, u16 height);
#endif
static void gx_set_scissor(u16 x, u16 y, u16 width, u16 height);

static void gx_setup_vertex_color_state(u16 width, u16 height)
{
	u32 xo = 0x156;
	u32 yo = 0x156;

	/* Match the validated libogc capture's exact draw-time PE state. */
	gx_load_bp_reg(0x4000000E);
	/* Keep PE output bit-exact; do not inherit libogc's default dithering. */
	gx_load_bp_reg(0x41003118);
	gx_load_bp_reg(0x42000000);
	gx_load_bp_reg(0x43000040);
	gx_load_bp_reg(0x44000003);
	gx_load_bp_reg(0x68000000);
	gx_load_bp_reg(0xF33F0000);

	/* One colour channel, no texgens, one TEV stage, culling disabled. */
	gx_load_bp_reg(0x00000010);
	gx_load_bp_reg(0x20000000 | ((xo & 0x7ff) << 12) | (yo & 0x7ff));
	gx_load_bp_reg(0x21000000 |
		       (((xo + width - 1) & 0x7ff) << 12) |
		       ((yo + height - 1) & 0xfff));
	/* GX_SetScissorBoxOffset(0, 0): hardware stores (axis + 342) >> 1. */
	gx_load_bp_reg(0x5902ACAB);

	/* GX_SetCopyFilter(aa=false): center all twelve raster samples. */
	gx_load_bp_reg(0x01666666);
	gx_load_bp_reg(0x02666666);
	gx_load_bp_reg(0x03666666);
	gx_load_bp_reg(0x04666666);

	/* TEV stage 0 = rasterized vertex colour/alpha (GX_PASSCLR). */
	gx_load_bp_reg(0xC008FFFA);
	gx_load_bp_reg(0xC108FFD0);
	/* TEV swap table 0 = identity RGBA mapping. */
	gx_load_bp_reg(0xF6000004);
	gx_load_bp_reg(0xF700000E);
	gx_load_bp_reg(0x28000000);
	gx_load_bp_reg(0x30000000 | (u32)(width - 1));
	gx_load_bp_reg(0x31000000 | (u32)(height - 1));

	/* Match libogc's unconditional XF initialization before vertex state. */
	gx_load_xf_reg(0x1000, 0x0000003F);

	/* One direct colour channel, no texcoord generators. */
	gx_load_xf_reg(0x1008, 0x00000001);
	gx_load_xf_reg(0x1009, 0x00000001);
	gx_load_xf_reg(0x100e, 0x00000401);
	gx_load_xf_reg(0x1010, 0x00000401);
	gx_load_xf_reg(0x1005, 0);
	gx_load_xf_reg(0x103f, 0);
	gx_load_identity_pos_mtx0();

	/* Pixel-space viewport and orthographic projection. */
	gx_load_xf_regs_n(0x101a, 6);
	wg_f32_bits(f32_from_u16(width >> 1));
	wg_f32_bits(F32_NEG(f32_from_u16(height >> 1)));
	wg_f32_bits(F32_16M);
	wg_f32_bits(f32_from_u16((width >> 1) + 342));
	wg_f32_bits(f32_from_u16((height >> 1) + 342));
	wg_f32_bits(F32_16M);

	gx_load_xf_regs_n(0x1020, 7);
	wg_f32_bits(f32_div_u16(2, width));
	wg_f32_bits(F32_NEG_ONE);
	wg_f32_bits(F32_NEG(f32_div_u16(2, height)));
	wg_f32_bits(F32_ONE);
	wg_f32_bits(F32_NEG_ONE);
	wg_f32_bits(F32_NEG_ONE);
	gx_wr32be(1);

	/* VTXFMT0: direct XY/F32 position followed by direct RGBA8 colour. */
	gx_load_cp_reg(0x50, 0x00002200);
	gx_load_cp_reg(0x60, 0x00000000);
	gx_load_cp_reg(0x70, 0x40016008);
	gx_load_cp_reg(0x80, 0x80000000);
	gx_load_cp_reg(0x90, 0x00000000);
}

#if IS_ENABLED(CONFIG_DRM_GCN_GX)
static void
gx_setup_vertex_color_state_semantic(u16 width, u16 height,
				     const struct gcn_drm_draw_state *state)
{
	static const u8 cull_to_hw[] = { 0, 2, 1, 3 };

	gx_setup_vertex_color_state(width, height);
	gx_load_bp_reg(0x00000010 |
		       ((u32)cull_to_hw[state->cull_mode] << 14));
	gx_set_viewport(state->viewport_x, state->viewport_y,
			state->viewport_width, state->viewport_height);
	gx_set_scissor(state->scissor_x, state->scissor_y,
		       state->scissor_width, state->scissor_height);
	if (state->blend_mode == DRM_GCN_BLEND_SRC_ALPHA)
		gx_load_bp_reg(0x410034B9);
}

static void
gx_setup_vertex_color_indexed_state(u16 width, u16 height,
				    const struct gcn_drm_draw_state *state)
{
	gx_setup_vertex_color_state_semantic(width, height, state);
	/* VTXFMT0: indexed8 XY/F32 position and indexed8 RGBA8 colour. */
	gx_load_cp_reg(0x50, 0x00004400);
	gx_load_cp_reg(0x70, 0x40016008);
}

static void
gx_setup_vertex_color_depth_state(u16 width, u16 height,
				  const struct gcn_drm_draw_state *state,
				  const struct gcn_drm_depth_state *depth)
{
	u32 z_mode;

	gx_setup_vertex_color_state_semantic(width, height, state);
	z_mode = (depth->test_enable ? BIT(0) : 0) |
		 ((depth->compare & 7) << 1) |
		 (depth->write_enable ? BIT(4) : 0);
	gx_load_bp_reg(0x40000000 | z_mode);
	/* VTXFMT0: indexed8 XYZ/F32 position and indexed8 RGBA8 colour. */
	gx_load_cp_reg(0x50, 0x00004400);
	gx_load_cp_reg(0x70, 0x40016009);
}

#endif

/* Add one position-derived texcoord and make TEV stage 0 sample texmap 0. */
static void gx_setup_rgb565_texture_state_mode(u16 width, u16 height,
					       bool direct_texcoord)
{
	gx_setup_vertex_color_state(width, height);

	/* GX_Init enables the post-transform selected by texCoordGen2. */
	gx_load_xf_reg(0x1012, 0x00000001);

	/* Keep the proven colour channel and add one texture-coordinate generator. */
	gx_load_bp_reg(0x00000011);
	gx_load_bp_reg(0xC008FFF8);
	gx_load_bp_reg(0xC108FFC0);
	gx_load_bp_reg(0x28000040);

	/* GX_SetTevDirect(GX_TEVSTAGE0): disable inherited indirect offsets. */
	gx_load_bp_reg(0x10000000);

	/* GX_TG_MTX2x4 through GX_TEXMTX0. */
	gx_load_xf_reg(0x103f, 0x00000001);
	if (direct_texcoord) {
		/* GX_TG_TEX0 is XF source row 5, not row 4. */
		gx_load_xf_reg(0x1040, 0x00000280);
		gx_load_identity_pos_mtx0();
		gx_load_cp_reg(0x30, 30 << 6);
		gx_load_xf_reg(0x1018, 30 << 6);

		/* Append direct TEX0 ST/F32 after the existing POS and CLR0. */
		gx_load_xf_reg(0x1008, 0x00000011);
		gx_load_cp_reg(0x60, 0x00000001);
		gx_load_cp_reg(0x70, 0x41216008);
	} else {
		gx_load_xf_reg(0x1040, 0x00000004);
		gx_load_pos_to_tex_mtx0(width, height);
	}
	/* GX_DTTIDENTITY - GX_DTTMTX0 = 125 - 64 = 61 (0x3d). */
	gx_load_xf_reg(0x1050, 0x0000003D);
	gx_load_identity_post_mtx();
}

static void
gx_setup_indexed_rgb565_texture_state(u16 width, u16 height,
				      const struct gcn_drm_draw_state *state)
{
	static const u8 cull_to_hw[] = { 0, 2, 1, 3 };

	gx_setup_rgb565_texture_state_mode(width, height, true);
	gx_load_bp_reg(0x00000011 |
		       ((u32)cull_to_hw[state->cull_mode] << 14));
	gx_set_viewport(state->viewport_x, state->viewport_y,
			state->viewport_width, state->viewport_height);
	gx_set_scissor(state->scissor_x, state->scissor_y,
		       state->scissor_width, state->scissor_height);
	/* Indexed8 XY/F32 position, RGBA8 colour, and ST/F32 TEX0. */
	gx_load_cp_reg(0x50, 0x00004400);
	gx_load_cp_reg(0x60, 0x00000002);
	gx_load_cp_reg(0x70, 0x41216008);
}

static void
gx_setup_itex_depth_state(u16 width, u16 height,
			  const struct gcn_drm_draw_state *state,
			  const struct gcn_drm_depth_state *depth)
{
	u32 z_mode;

	gx_setup_indexed_rgb565_texture_state(width, height, state);
	z_mode = (depth->test_enable ? BIT(0) : 0) |
		 ((depth->compare & 7) << 1) |
		 (depth->write_enable ? BIT(4) : 0);
	gx_load_bp_reg(0x40000000 | z_mode);
	/* Indexed8 XYZ/F32 position, RGBA8 colour, and ST/F32 TEX0. */
	gx_load_cp_reg(0x70, 0x41216009);
}

static void
gx_setup_fixed_state(u16 width, u16 height, u32 tev_mode,
		     const struct gcn_drm_draw_state *state,
		     const struct gcn_drm_depth_state *depth)
{
	if (tev_mode == DRM_GCN_TEV_PASS_COLOR) {
		gx_setup_vertex_color_depth_state(width, height, state, depth);
		return;
	}

	gx_setup_itex_depth_state(width, height, state, depth);
	if (tev_mode == DRM_GCN_TEV_MODULATE) {
		/* GX_MODULATE: raster colour and alpha multiplied by TEX0. */
		gx_load_bp_reg(0xC008F8AF);
		gx_load_bp_reg(0xC108F2F0);
	}
	if (state->blend_mode == DRM_GCN_BLEND_SRC_ALPHA)
		gx_load_bp_reg(0x410034B9);
}

static void gx_setup_rgb565_texture_state(u16 width, u16 height)
{
	gx_setup_rgb565_texture_state_mode(width, height,
					   gx_use_direct_texcoord);
}

#define GX_TF_RGB565	4U
#define GX_TF_RGBA8	6U

static void gx_invalidate_texture_cache(void)
{
	/* Exact libogc GX_InvalidateTexAll() sequence. */
	gx_load_bp_reg(0x0F000000);
	gx_load_bp_reg(0x66001000);
	gx_load_bp_reg(0x66001100);
	gx_load_bp_reg(0x0F000000);
}

/* Bind a tiled RGB565 or RGBA8 buffer to texmap 0. */
static void gx_setup_texture(void *tile_buf, u16 width, u16 height, u32 format,
			     u32 filter)
{
	u32 phys = virt_to_phys(tile_buf);
	u32 img0;

	/*
	 * BP 0x80 texMode0: CLAMP wrapping, no mipmaps, edge LOD disabled.
	 * Linear sets libogc's mag-filter bit 4 and non-mipmapped min-filter
	 * value 4 in bits 5..7.
	 */
	gx_load_bp_reg(0x80000100 |
		       (filter == DRM_GCN_TEXTURE_FILTER_LINEAR ? 0x90 : 0));

	/* BP 0x84 texMode1: LOD disabled */
	gx_load_bp_reg(0x84000000);

	/* BP 0x88 texImage0: dimensions and GX texture format. */
	img0 = ((u32)(width  - 1) & 0x3ff) |
	       (((u32)(height - 1) & 0x3ff) << 10) |
	       (format << 20);
	gx_load_bp_reg(0x88000000 | img0);

	/*
	 * BP 0x8C/0x90 texImage1/2: RVL libogc texRegion[mapid+8], selected
	 * for RGB565 and RGBA8. GX_InitTexCacheRegion(..., even=0x00000,
	 * odd=0x80000,
	 * size_even=size_odd=GX_TEXCACHE_32K) encodes both TMEM bases and
	 * cache-size fields.
	 */
	gx_load_bp_reg(0x8C0D8000);
	gx_load_bp_reg(0x900DC000);

	/* BP 0x94 texImage3: physical address >> 5 */
	gx_load_bp_reg(0x94000000 | ((phys >> 5) & 0x00ffffff));

	gx_invalidate_texture_cache();

	/* BP 0x30/0x31 suSsize/suTsize for texcoord 0:
	 * [15:0] = coordinate scale - 1: dimension - 1 for normalized
	 * coordinates, or 0 for coordinates already expressed in texels.
	 * [16] = wrap = 0 (GX_CLAMP)
	 */
	gx_load_bp_reg(0x30000000 |
		       (gx_use_texel_space ? 0 : (u32)(width - 1)));
	gx_load_bp_reg(0x31000000 |
		       (gx_use_texel_space ? 0 : (u32)(height - 1)));
}

static void gx_setup_texture_rgb565(void *tile_buf, u16 width, u16 height)
{
	gx_setup_texture(tile_buf, width, height, GX_TF_RGB565,
			 DRM_GCN_TEXTURE_FILTER_NEAREST);
}

static void gx_setup_texture_coordinate_scale(u16 width, u16 height,
					      bool s_texel, bool t_texel)
{
	gx_load_bp_reg(0x30000000 | (s_texel ? 0 : (u32)(width - 1)));
	gx_load_bp_reg(0x31000000 | (t_texel ? 0 : (u32)(height - 1)));
}

/*
 * gx_draw_fullscreen_quad - render a textured quad covering the whole EFB.
 *
 * GX_QUADS=0x80 | VTXFMT0=0, 4 vertices, each with XY pos + ST texcoord.
 */
static void __maybe_unused gx_draw_fullscreen_quad(u16 width, u16 height)
{
	u32 fw = f32_from_u16(width);
	u32 fh = f32_from_u16(height);

	gx_wr8(0x80);			/* GX_QUADS | vtxfmt 0 */
	gx_wr16be(4);

	/*
	 * Texcoords are normalized [0,1] — NOT pixel coords.
	 * GX rasterizer interpolation stalls permanently when texcoords exceed
	 * ~1.0 (e.g. raw pixel values 0..576); suSsize/suTsize scale [0,1] to
	 * texel addresses at TMU sample time.
	 */
	/*
	 * DIAGNOSTIC: all texcoords = (0,0). Zero gradient → LOD = -∞ (safe).
	 * Tests whether non-zero texcoord derivatives are what stalls the
	 * rasterizer's LOD unit at frame 2 with XF=1.
	 */
	/* top-left:     pos=(0, 0),   tex=(0, 0) */
	wg_f32_bits(F32_ZERO); wg_f32_bits(F32_ZERO);
	wg_f32_bits(F32_ZERO); wg_f32_bits(F32_ZERO);

	/* top-right:    pos=(w, 0),   tex=(0, 0) */
	wg_f32_bits(fw);       wg_f32_bits(F32_ZERO);
	wg_f32_bits(F32_ZERO); wg_f32_bits(F32_ZERO);

	/* bottom-right: pos=(w, h),   tex=(0, 0) */
	wg_f32_bits(fw);       wg_f32_bits(fh);
	wg_f32_bits(F32_ZERO); wg_f32_bits(F32_ZERO);

	/* bottom-left:  pos=(0, h),   tex=(0, 0) */
	wg_f32_bits(F32_ZERO); wg_f32_bits(fh);
	wg_f32_bits(F32_ZERO); wg_f32_bits(F32_ZERO);
}

static void __maybe_unused gx_draw_pos_quad(u16 width, u16 height)
{
	/*
	 * DIAGNOSTIC: oversized clip-space triangles, both windings.  This avoids
	 * pixel-space projection/ortho uncertainty and stale cull winding state.
	 * Z=0 (near plane) per vertex, matching VAT0's GX_POS_XYZ format and
	 * the Z-enable test in gx_setup_constant_white_state -- see there for
	 * why Z=0 should cleanly pass GX_LEQUAL against the far/max Z-clear.
	 */
	gx_wr8(0x90);			/* GX_TRIANGLES | vtxfmt 0 */
	gx_wr16be(6);

	wg_f32_bits(0xC0800000); wg_f32_bits(0xC0800000); wg_f32_bits(F32_ZERO); /* (-4, -4, 0) */
	wg_f32_bits(0x40800000); wg_f32_bits(0xC0800000); wg_f32_bits(F32_ZERO); /* ( 4, -4, 0) */
	wg_f32_bits(F32_ZERO);   wg_f32_bits(0x40800000); wg_f32_bits(F32_ZERO); /* ( 0,  4, 0) */

	wg_f32_bits(0xC0800000); wg_f32_bits(0xC0800000); wg_f32_bits(F32_ZERO); /* (-4, -4, 0) */
	wg_f32_bits(F32_ZERO);   wg_f32_bits(0x40800000); wg_f32_bits(F32_ZERO); /* ( 0,  4, 0) */
	wg_f32_bits(0x40800000); wg_f32_bits(0xC0800000); wg_f32_bits(F32_ZERO); /* ( 4, -4, 0) */
}

static void gx_emit_color_rect(u16 x0, u16 y0, u16 x1, u16 y1,
			       u8 r, u8 g, u8 b)
{
	u32 fx0 = f32_from_u16(x0);
	u32 fy0 = f32_from_u16(y0);
	u32 fx1 = f32_from_u16(x1);
	u32 fy1 = f32_from_u16(y1);

	wg_f32_bits(fx0); wg_f32_bits(fy0);
	gx_wr8(r); gx_wr8(g); gx_wr8(b); gx_wr8(0xff);

	wg_f32_bits(fx1); wg_f32_bits(fy0);
	gx_wr8(r); gx_wr8(g); gx_wr8(b); gx_wr8(0xff);

	wg_f32_bits(fx1); wg_f32_bits(fy1);
	gx_wr8(r); gx_wr8(g); gx_wr8(b); gx_wr8(0xff);

	wg_f32_bits(fx0); wg_f32_bits(fy1);
	gx_wr8(r); gx_wr8(g); gx_wr8(b); gx_wr8(0xff);
}

static void gx_draw_color_rect(u16 x0, u16 y0, u16 x1, u16 y1,
			       u8 r, u8 g, u8 b)
{
	gx_wr8(0x80); /* GX_QUADS | vtxfmt 0 */
	gx_wr16be(4);
	gx_emit_color_rect(x0, y0, x1, y1, r, g, b);
}

#if IS_ENABLED(CONFIG_DRM_GCN_GX)
static void gx_emit_color_row_triangles(u16 width, u16 y, u8 r, u8 g, u8 b)
{
	const u16 xy[6][2] = {
		{ 0, y }, { width, y }, { width, y + 1 },
		{ 0, y }, { width, y + 1 }, { 0, y + 1 },
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(xy); i++) {
		wg_f32_bits(f32_from_u16(xy[i][0]));
		wg_f32_bits(f32_from_u16(xy[i][1]));
		gx_wr8(r);
		gx_wr8(g);
		gx_wr8(b);
		gx_wr8(0xff);
	}
}

static void
gx_draw_color_triangles(const struct gcn_drm_color_vertex *vertices,
			unsigned int triangle_count)
{
	unsigned int vertex_count = triangle_count * 3;
	unsigned int i;

	gx_wr8(0x90); /* GX_TRIANGLES | vtxfmt 0 */
	gx_wr16be(vertex_count);

	for (i = 0; i < vertex_count; i++) {
		wg_f32_bits(f32_from_u16(vertices[i].x));
		wg_f32_bits(f32_from_u16(vertices[i].y));
		gx_wr8(vertices[i].r);
		gx_wr8(vertices[i].g);
		gx_wr8(vertices[i].b);
		gx_wr8(vertices[i].a);
	}
}

static void
gx_draw_color_depth_triangles(const struct gcn_drm_color_depth_vertex *vertices,
			      unsigned int triangle_count)
{
	unsigned int vertex_count = triangle_count * 3;
	__be32 *positions = gx_tex_buf_alt;
	u8 *colours = (u8 *)positions + ALIGN(vertex_count * 12, 32);
	unsigned int i;
	u32 z;

	for (i = 0; i < vertex_count; i++) {
		positions[i * 3] = cpu_to_be32(f32_from_u16(vertices[i].x));
		positions[i * 3 + 1] = cpu_to_be32(f32_from_u16(vertices[i].y));
		/* GX clips z == 1.0; map the inclusive UAPI below that endpoint. */
		z = f32_div_u32(vertices[i].z, DRM_GCN_DEPTH_MAX + 1U);
		positions[i * 3 + 2] = cpu_to_be32(F32_NEG(z));
		colours[i * 4] = vertices[i].r;
		colours[i * 4 + 1] = vertices[i].g;
		colours[i * 4 + 2] = vertices[i].b;
		colours[i * 4 + 3] = vertices[i].a;
	}
	flush_dcache_range((unsigned long)positions,
			   (unsigned long)colours + vertex_count * 4);
	gx_wr8(0x48); /* GX_InvVtxCache after modifying indexed arrays. */

	/* GX_SetArray(GX_VA_POS/CLR0) with bounded index8 arrays. */
	gx_load_cp_reg(0xa0, (u32)virt_to_phys(positions));
	gx_load_cp_reg(0xb0, 12);
	gx_load_cp_reg(0xa2, (u32)virt_to_phys(colours));
	gx_load_cp_reg(0xb2, 4);

	gx_wr8(0x90); /* GX_TRIANGLES | vtxfmt 0 */
	gx_wr16be(vertex_count);

	for (i = 0; i < vertex_count; i++) {
		gx_wr8(i);
		gx_wr8(i);
	}
}

static void
gx_draw_textured_triangles(const struct gcn_drm_texture_vertex *vertices,
			   unsigned int triangle_count,
			   u16 texture_width, u16 texture_height)
{
	unsigned int vertex_count = triangle_count * 3;
	unsigned int i;

	gx_wr8(0x90); /* GX_TRIANGLES | vtxfmt 0 */
	gx_wr16be(vertex_count);

	for (i = 0; i < vertex_count; i++) {
		wg_f32_bits(f32_from_u16(vertices[i].x));
		wg_f32_bits(f32_from_u16(vertices[i].y));
		gx_wr8(0xff);
		gx_wr8(0xff);
		gx_wr8(0xff);
		gx_wr8(0xff);
		wg_f32_bits(gx_semantic_texcoord_bits(vertices[i].s,
						      texture_width));
		wg_f32_bits(gx_semantic_texcoord_bits(vertices[i].t,
						      texture_height));
	}
}

static void
gx_draw_indexed_color_triangles(const struct gcn_drm_color_vertex *vertices,
				u32 vertex_count, const u8 *indices,
				u32 triangle_count)
{
	unsigned int index_count = triangle_count * 3;
	__be32 *positions = gx_tex_buf_alt;
	u8 *colours = (u8 *)positions + ALIGN(vertex_count * 8, 32);
	unsigned int i;

	for (i = 0; i < vertex_count; i++) {
		positions[i * 2] = cpu_to_be32(f32_from_u16(vertices[i].x));
		positions[i * 2 + 1] = cpu_to_be32(f32_from_u16(vertices[i].y));
		colours[i * 4] = vertices[i].r;
		colours[i * 4 + 1] = vertices[i].g;
		colours[i * 4 + 2] = vertices[i].b;
		colours[i * 4 + 3] = vertices[i].a;
	}
	flush_dcache_range((unsigned long)positions,
			   (unsigned long)colours + vertex_count * 4);
	gx_wr8(0x48); /* GX_InvVtxCache after modifying indexed arrays. */

	gx_load_cp_reg(0xa0, (u32)virt_to_phys(positions));
	gx_load_cp_reg(0xb0, 8);
	gx_load_cp_reg(0xa2, (u32)virt_to_phys(colours));
	gx_load_cp_reg(0xb2, 4);

	gx_wr8(0x90); /* GX_TRIANGLES | vtxfmt 0 */
	gx_wr16be(index_count);
	for (i = 0; i < index_count; i++) {
		gx_wr8(indices[i]);
		gx_wr8(indices[i]);
	}
}

static void gx_draw_itex(const struct gcn_drm_texture_vertex *vertices,
			 u32 vertex_count,
		const u8 *indices, u32 triangle_count,
		u16 texture_width, u16 texture_height)
{
	unsigned int index_count = triangle_count * 3;
	__be32 *positions = gx_tex_buf_alt;
	__be32 *texcoords = (__be32 *)((u8 *)positions +
					      ALIGN(vertex_count * 8, 32));
	u8 *colours = (u8 *)texcoords + ALIGN(vertex_count * 8, 32);
	unsigned int i;

	for (i = 0; i < vertex_count; i++) {
		u32 s = gx_semantic_texcoord_bits(vertices[i].s, texture_width);
		u32 t = gx_semantic_texcoord_bits(vertices[i].t, texture_height);

		positions[i * 2] = cpu_to_be32(f32_from_u16(vertices[i].x));
		positions[i * 2 + 1] = cpu_to_be32(f32_from_u16(vertices[i].y));
		texcoords[i * 2] = cpu_to_be32(s);
		texcoords[i * 2 + 1] = cpu_to_be32(t);
		colours[i * 4] = 0xff;
		colours[i * 4 + 1] = 0xff;
		colours[i * 4 + 2] = 0xff;
		colours[i * 4 + 3] = 0xff;
	}
	flush_dcache_range((unsigned long)positions,
			   (unsigned long)colours + vertex_count * 4);
	gx_wr8(0x48); /* GX_InvVtxCache after modifying indexed arrays. */

	gx_load_cp_reg(0xa0, (u32)virt_to_phys(positions));
	gx_load_cp_reg(0xb0, 8);
	gx_load_cp_reg(0xa2, (u32)virt_to_phys(colours));
	gx_load_cp_reg(0xb2, 4);
	gx_load_cp_reg(0xa4, (u32)virt_to_phys(texcoords));
	gx_load_cp_reg(0xb4, 8);

	gx_wr8(0x90); /* GX_TRIANGLES | vtxfmt 0 */
	gx_wr16be(index_count);
	for (i = 0; i < index_count; i++) {
		gx_wr8(indices[i]);
		gx_wr8(indices[i]);
		gx_wr8(indices[i]);
	}
}

static void
gx_draw_itex_depth(const struct gcn_drm_itex_depth_vertex *vertices,
		   u32 vertex_count, const u8 *indices,
		   u32 triangle_count, u16 texture_width,
		   u16 texture_height)
{
	unsigned int index_count = triangle_count * 3;
	__be32 *positions = gx_tex_buf_alt;
	__be32 *texcoords = (__be32 *)((u8 *)positions +
					      ALIGN(vertex_count * 12, 32));
	u8 *colours = (u8 *)texcoords + ALIGN(vertex_count * 8, 32);
	unsigned int i;

	for (i = 0; i < vertex_count; i++) {
		u32 z = f32_div_u32(vertices[i].z, DRM_GCN_DEPTH_MAX + 1U);
		u32 s = gx_semantic_texcoord_bits(vertices[i].s, texture_width);
		u32 t = gx_semantic_texcoord_bits(vertices[i].t, texture_height);

		positions[i * 3] = cpu_to_be32(f32_from_u16(vertices[i].x));
		positions[i * 3 + 1] = cpu_to_be32(f32_from_u16(vertices[i].y));
		positions[i * 3 + 2] = cpu_to_be32(F32_NEG(z));
		texcoords[i * 2] = cpu_to_be32(s);
		texcoords[i * 2 + 1] = cpu_to_be32(t);
		colours[i * 4] = 0xff;
		colours[i * 4 + 1] = 0xff;
		colours[i * 4 + 2] = 0xff;
		colours[i * 4 + 3] = 0xff;
	}
	flush_dcache_range((unsigned long)positions,
			   (unsigned long)colours + vertex_count * 4);
	gx_wr8(0x48); /* GX_InvVtxCache after modifying indexed arrays. */

	gx_load_cp_reg(0xa0, (u32)virt_to_phys(positions));
	gx_load_cp_reg(0xb0, 12);
	gx_load_cp_reg(0xa2, (u32)virt_to_phys(colours));
	gx_load_cp_reg(0xb2, 4);
	gx_load_cp_reg(0xa4, (u32)virt_to_phys(texcoords));
	gx_load_cp_reg(0xb4, 8);

	gx_wr8(0x90); /* GX_TRIANGLES | vtxfmt 0 */
	gx_wr16be(index_count);
	for (i = 0; i < index_count; i++) {
		gx_wr8(indices[i]);
		gx_wr8(indices[i]);
		gx_wr8(indices[i]);
	}
}

static void
gx_draw_fixed(const struct gcn_drm_fixed_vertex *vertices,
	      u32 vertex_count, const u8 *indices, u32 triangle_count,
	      u16 texture_width, u16 texture_height, bool textured,
	      void *vertex_workspace)
{
	unsigned int index_count = triangle_count * 3;
	__be32 *positions = vertex_workspace;
	__be32 *texcoords = (__be32 *)((u8 *)positions +
					      ALIGN(vertex_count * 12, 32));
	u8 *colours = textured ?
		(u8 *)texcoords + ALIGN(vertex_count * 8, 32) :
		(u8 *)positions + ALIGN(vertex_count * 12, 32);
	unsigned int i;

	for (i = 0; i < vertex_count; i++) {
		u32 z = f32_div_u32(vertices[i].z, DRM_GCN_DEPTH_MAX + 1U);
		u32 s;
		u32 t;

		positions[i * 3] = cpu_to_be32(f32_from_u16(vertices[i].x));
		positions[i * 3 + 1] = cpu_to_be32(f32_from_u16(vertices[i].y));
		positions[i * 3 + 2] = cpu_to_be32(F32_NEG(z));
		if (textured) {
			s = gx_semantic_texcoord_bits(vertices[i].s, texture_width);
			t = gx_semantic_texcoord_bits_phase(vertices[i].t, texture_height, -1);
			texcoords[i * 2] = cpu_to_be32(s);
			texcoords[i * 2 + 1] = cpu_to_be32(t);
		}
		colours[i * 4] = vertices[i].r;
		colours[i * 4 + 1] = vertices[i].g;
		colours[i * 4 + 2] = vertices[i].b;
		colours[i * 4 + 3] = vertices[i].a;
	}
	flush_dcache_range((unsigned long)positions,
			   (unsigned long)colours + vertex_count * 4);
	gx_wr8(0x48); /* GX_InvVtxCache after modifying indexed arrays. */

	gx_load_cp_reg(0xa0, (u32)virt_to_phys(positions));
	gx_load_cp_reg(0xb0, 12);
	gx_load_cp_reg(0xa2, (u32)virt_to_phys(colours));
	gx_load_cp_reg(0xb2, 4);
	if (textured) {
		gx_load_cp_reg(0xa4, (u32)virt_to_phys(texcoords));
		gx_load_cp_reg(0xb4, 8);
	}

	gx_wr8(0x90); /* GX_TRIANGLES | vtxfmt 0 */
	gx_wr16be(index_count);
	for (i = 0; i < index_count; i++) {
		gx_wr8(indices[i]);
		gx_wr8(indices[i]);
		if (textured)
			gx_wr8(indices[i]);
	}
}

#endif

static void gx_draw_color_quad(u16 width, u16 height, u8 r, u8 g, u8 b)
{
	gx_draw_color_rect(0, 0, width, height, r, g, b);
}

static void gx_emit_textured_rect(u16 x0, u16 y0, u16 x1, u16 y1,
				  u32 s0, u32 t0, u32 s1, u32 t1)
{
	u32 fx0 = f32_from_u16(x0);
	u32 fy0 = f32_from_u16(y0);
	u32 fx1 = f32_from_u16(x1);
	u32 fy1 = f32_from_u16(y1);

	wg_f32_bits(fx0); wg_f32_bits(fy0);
	gx_wr32be(0xffffffff);
	wg_f32_bits(s0); wg_f32_bits(t0);

	wg_f32_bits(fx1); wg_f32_bits(fy0);
	gx_wr32be(0xffffffff);
	wg_f32_bits(s1); wg_f32_bits(t0);

	wg_f32_bits(fx1); wg_f32_bits(fy1);
	gx_wr32be(0xffffffff);
	wg_f32_bits(s1); wg_f32_bits(t1);

	wg_f32_bits(fx0); wg_f32_bits(fy1);
	gx_wr32be(0xffffffff);
	wg_f32_bits(s0); wg_f32_bits(t1);
}

static u16 gx_nearest_run_count(u16 src_extent, u16 dst_extent)
{
	u16 dst_start = 0;
	u16 runs = 0;

	while (dst_start < dst_extent) {
		u16 src_index = gx_nearest_source_index(dst_start, src_extent,
						       dst_extent);

		do {
			dst_start++;
		} while (dst_start < dst_extent &&
			 gx_nearest_source_index(dst_start, src_extent,
						 dst_extent) == src_index);
		runs++;
	}
	return runs;
}

static void gx_draw_nearest_horizontal_runs(u16 src_width,
					    u16 texture_width, u16 height,
					    u16 texture_height,
					    u16 dst_width, bool split)
{
	u32 t0 = gx_semantic_texcoord_bits_phase(0, texture_height, -2);
	u32 t1 = gx_semantic_texcoord_bits_phase(height, texture_height, -2);
	u32 middle = gx_semantic_texcoord_bits_phase(height / 2, texture_height, -2);
	u16 dst_start = 0;

	gx_wr8(0x80); /* GX_QUADS | vtxfmt 0 */
	gx_wr16be((split ? 8 : 4) * gx_nearest_run_count(src_width, dst_width));

	while (dst_start < dst_width) {
		u16 src_index = gx_nearest_source_index(dst_start, src_width,
						       dst_width);
		u16 dst_end = dst_start + 1;
		u32 s;

		while (dst_end < dst_width &&
		       gx_nearest_source_index(dst_end, src_width, dst_width) ==
		       src_index)
			dst_end++;
		s = gx_semantic_texcoord_bits_phase(src_index, texture_width, 2);
		if (split) {
			gx_emit_textured_rect(dst_start, 0, dst_end, height / 2,
					      s, t0, s, middle);
			gx_emit_textured_rect(dst_start, height / 2, dst_end, height,
					      s, middle, s, t1);
		} else {
			gx_emit_textured_rect(dst_start, 0, dst_end, height,
					      s, t0, s, t1);
		}
		dst_start = dst_end;
	}
}

static void gx_draw_nearest_vertical_runs(u16 x, u16 y, u16 width,
					  u16 texture_width,
					  u16 src_height,
					  u16 texture_height,
					  u16 dst_height, bool split)
{
	u32 s0 = gx_semantic_texcoord_bits_phase(0, texture_width, -2);
	u32 s1 = gx_semantic_texcoord_bits_phase(width, texture_width, -2);
	u32 middle = gx_semantic_texcoord_bits_phase(width / 2, texture_width, -2);
	u16 dst_start = 0;

	gx_wr8(0x80); /* GX_QUADS | vtxfmt 0 */
	gx_wr16be((split ? 8 : 4) * gx_nearest_run_count(src_height, dst_height));

	while (dst_start < dst_height) {
		u16 src_index = gx_nearest_source_index(dst_start, src_height,
						       dst_height);
		u16 dst_end = dst_start + 1;
		u32 t;

		while (dst_end < dst_height &&
		       gx_nearest_source_index(dst_end, src_height, dst_height) ==
		       src_index)
			dst_end++;
		t = gx_semantic_texcoord_bits_phase(src_index, texture_height, 2);
		if (split) {
			gx_emit_textured_rect(x, y + dst_start, x + width / 2,
					      y + dst_end, s0, t, middle, t);
			gx_emit_textured_rect(x + width / 2, y + dst_start,
					      x + width, y + dst_end, middle, t, s1, t);
		} else {
			gx_emit_textured_rect(x, y + dst_start, x + width,
					      y + dst_end, s0, t, s1, t);
		}
		dst_start = dst_end;
	}
}

static void gx_set_scissor(u16 x, u16 y, u16 width, u16 height)
{
	u32 x0 = x + 342;
	u32 y0 = y + 342;
	u32 x1 = x0 + width - 1;
	u32 y1 = y0 + height - 1;

	gx_load_bp_reg(0x20000000 | ((x0 & 0x7ff) << 12) | (y0 & 0xfff));
	gx_load_bp_reg(0x21000000 | ((x1 & 0x7ff) << 12) | (y1 & 0xfff));
}

#if IS_ENABLED(CONFIG_DRM_GCN_GX)
static void gx_set_viewport(u16 x, u16 y, u16 width, u16 height)
{
	/* GX_SetViewport with near=0 and far=1, including the 342 EFB bias. */
	gx_load_xf_regs_n(0x101a, 6);
	wg_f32_bits(f32_div_u32(width, 2));
	wg_f32_bits(F32_NEG(f32_div_u32(height, 2)));
	wg_f32_bits(F32_16M);
	wg_f32_bits(f32_div_u32(2 * (u32)x + width + 684, 2));
	wg_f32_bits(f32_div_u32(2 * (u32)y + height + 684, 2));
	wg_f32_bits(F32_16M);
}
#endif

static void gx_draw_textured_color_quad(u16 width, u16 height,
					u8 r, u8 g, u8 b)
{
	u32 fw = f32_from_u16(width);
	u32 fh = f32_from_u16(height);
	u32 s0 = gx_direct_texcoord_bits(width, 0);
	u32 s1 = gx_direct_texcoord_bits(width, 1);
	u32 t0 = gx_direct_texcoord_bits(height, 0);
	u32 t1 = gx_direct_texcoord_bits(height, 1);

	if (gx_use_constant_texcoord) {
		s0 = s1 = gx_direct_center_texcoord_bits(width);
		t0 = t1 = gx_direct_center_texcoord_bits(height);
	}

	gx_wr8(0x80); /* GX_QUADS | vtxfmt 0 */
	gx_wr16be(4);

	wg_f32_bits(F32_ZERO); wg_f32_bits(F32_ZERO);
	gx_wr8(r); gx_wr8(g); gx_wr8(b); gx_wr8(0xff);
	wg_f32_bits(s0); wg_f32_bits(t0);

	wg_f32_bits(fw); wg_f32_bits(F32_ZERO);
	gx_wr8(r); gx_wr8(g); gx_wr8(b); gx_wr8(0xff);
	wg_f32_bits(s1); wg_f32_bits(t0);

	wg_f32_bits(fw); wg_f32_bits(fh);
	gx_wr8(r); gx_wr8(g); gx_wr8(b); gx_wr8(0xff);
	wg_f32_bits(s1); wg_f32_bits(t1);

	wg_f32_bits(F32_ZERO); wg_f32_bits(fh);
	gx_wr8(r); gx_wr8(g); gx_wr8(b); gx_wr8(0xff);
	wg_f32_bits(s0); wg_f32_bits(t1);
}

static void gx_draw_textured_color_triangle(u16 width, u16 height,
					    u8 r, u8 g, u8 b)
{
	u32 fw2 = f32_from_u16(width * 2);
	u32 fh2 = f32_from_u16(height * 2);
	u32 s0 = gx_direct_texcoord_bits(width, 0);
	u32 s2 = gx_direct_texcoord_bits(width, 2);
	u32 t0 = gx_direct_texcoord_bits(height, 0);
	u32 t2 = gx_direct_texcoord_bits(height, 2);

	if (gx_use_constant_texcoord) {
		s0 = s2 = gx_direct_center_texcoord_bits(width);
		t0 = t2 = gx_direct_center_texcoord_bits(height);
	}

	gx_wr8(0x90); /* GX_TRIANGLES | vtxfmt 0 */
	gx_wr16be(3);

	wg_f32_bits(F32_ZERO); wg_f32_bits(F32_ZERO);
	gx_wr8(r); gx_wr8(g); gx_wr8(b); gx_wr8(0xff);
	wg_f32_bits(s0); wg_f32_bits(t0);

	wg_f32_bits(fw2); wg_f32_bits(F32_ZERO);
	gx_wr8(r); gx_wr8(g); gx_wr8(b); gx_wr8(0xff);
	wg_f32_bits(s2); wg_f32_bits(t0);

	wg_f32_bits(F32_ZERO); wg_f32_bits(fh2);
	gx_wr8(r); gx_wr8(g); gx_wr8(b); gx_wr8(0xff);
	wg_f32_bits(s0); wg_f32_bits(t2);
}

static void gx_draw_direct_grid(u16 width, u16 height)
{
	u16 x, y;

	gx_draw_color_rect(0, 0, width / 2, height / 2, 0xff, 0x00, 0x00);
	gx_draw_color_rect(width / 2, 0, width, height / 2, 0x00, 0x80, 0x00);
	gx_draw_color_rect(0, height / 2, width / 2, height, 0x00, 0x00, 0xff);
	gx_draw_color_rect(width / 2, height / 2, width, height,
			   0xff, 0xff, 0xff);

	for (x = 0; x < width; x += 32)
		gx_draw_color_rect(x, 0, min_t(u16, x + 1, width), height,
				   0x00, 0x00, 0x00);
	for (y = 0; y < height; y += 32)
		gx_draw_color_rect(0, y, width, min_t(u16, y + 1, height),
				   0x00, 0x00, 0x00);
}

static void gx_draw_direct_vertical_stripes(u16 width, u16 height)
{
	u16 x;

	/* Monochrome keeps shared YUYV chroma neutral; luma must alternate. */
	gx_draw_color_quad(width, height, 0xff, 0xff, 0xff);
	for (x = 0; x < width; x += 2)
		gx_draw_color_rect(x, 0, x + 1, height, 0x00, 0x00, 0x00);
}

static void gx_draw_direct_pattern(u16 width, u16 height)
{
	if (gx_use_direct_vstripes)
		gx_draw_direct_vertical_stripes(width, height);
	else
		gx_draw_direct_grid(width, height);
}

/* ------------------------------------------------------------------ */
/* EFB -> XFB display copy                                             */
/* ------------------------------------------------------------------ */

static void gx_set_copy_clear_rgb(u8 r, u8 g, u8 b)
{
	/* GX_SetCopyClear({r,g,b,255}, 0x00ffffff) */
	gx_load_bp_reg(0x4F000000 | (0xff << 8) | r);
	gx_load_bp_reg(0x50000000 | ((u32)g << 8) | b);
	gx_load_bp_reg(0x51000000 | 0x00ffffff);
}

static void gx_setup_display_copy_state(void)
{
	/* Remove display-copy state inherited from Mini before the first copy. */
	gx_load_bp_reg(0x42000000); /* destination alpha disabled */
	gx_load_bp_reg(0x43000040); /* RGB8/Z24 EFB, linear Z */
	gx_load_bp_reg(0x44000003); /* update both fields */
	gx_load_bp_reg(0x68000000); /* field mode disabled */

	/* GX_SetCopyFilter(aa=false, vf=false): center samples, narrow filter. */
	gx_load_bp_reg(0x01666666);
	gx_load_bp_reg(0x02666666);
	gx_load_bp_reg(0x03666666);
	gx_load_bp_reg(0x04666666);
	gx_load_bp_reg(0x53595000);
	gx_load_bp_reg(0x54000015);

	/* GX_SetDispCopyYScale(1.0). */
	gx_load_bp_reg(0x4E000100);
}

/*
 * gcn_gx_copy_efb_to_xfb - trigger hardware EFB->XFB blit.
 *
 * The GX fixed-function copy unit reads from the EFB, converts RGB to
 * YUYV, and writes into the XFB for the VI to scan out.  Replaces
 * vi_transcode_RGB565 / vi_transcode_RGB888 when gx_accel_ready is set.
 */
static void gx_copy_efb_to_xfb(u32 xfb_phys, u16 width, u16 height, bool clear)
{
	u32 ctrl;

	if (clear) {
		/*
		 * Match libogc GX_CopyDisp(clear=GX_TRUE): temporarily force
		 * ALWAYS comparison and Z update so copy-clear writes depth, then
		 * set the clear bit in copy control.
		 */
		gx_load_bp_reg(0x4000001F);
		gx_load_bp_reg(0x41000018);
	}

	/* BP 0x49: copy source top-left = (0, 0) */
	gx_load_bp_reg((BP_DISP_COPY_TL << 24) | 0);

	/* BP 0x4a: source width-1, height-1 */
	gx_load_bp_reg((BP_DISP_COPY_WH << 24) |
		       (((u32)(height - 1) & 0x3ff) << 10) |
		       ((u32)(width  - 1) & 0x3ff));

	/* BP 0x4d: dest stride in units of 32 bytes (one cache line) */
	gx_load_bp_reg((BP_DISP_COPY_DST << 24) | ((width * 2) >> 5));

	/* BP 0x4b: dest physical address (right-shifted 5) */
	gx_load_bp_reg((BP_DISP_COPY_ADDR << 24) | ((xfb_phys >> 5) & 0xffffff));

	/* BP 0x52: copy control -- gamma 1.0, optional clear, XFB target. */
	ctrl = (BP_DISP_COPY_CTRL << 24) |
	       (GX_GM_1_0 << COPY_CTRL_GAMMA_SHIFT) |
	       (clear ? COPY_CTRL_CLEAR : 0) |
	       COPY_CTRL_TO_XFB;
	gx_load_bp_reg(ctrl);

	/*
	 * BP 0x45 = 2: PE draw-done trigger (libogc GX_DrawDone/GX_SetDrawDone).
	 * Queued behind the copy command and observed through the PE finish IRQ.
	 */
	gx_load_bp_reg(0x45000002);
}

static void gx_copy_efb_rect_to_rgb565_texture_stride(void *dest, u16 left,
						      u16 top, u16 width,
						      u16 height,
						      u16 dest_width,
						      bool clear)
{
	u32 ctrl;

	if (clear) {
		/* GX_CopyTex clears depth only while Z update is enabled. */
		gx_load_bp_reg(0x4000001F);
		gx_load_bp_reg(0x41000018);
	}

	/* GX_SetTexCopySrc(left, top, width, height). */
	gx_load_bp_reg((BP_DISP_COPY_TL << 24) |
		       (((u32)top & 0x3ff) << 10) |
		       ((u32)left & 0x3ff));
	gx_load_bp_reg((BP_DISP_COPY_WH << 24) |
		       (((u32)(height - 1) & 0x3ff) << 10) |
		       ((u32)(width - 1) & 0x3ff));

	/* RGB565 uses 4x4 tiles; texture-copy stride is tiles, not bytes. */
	gx_load_bp_reg((BP_DISP_COPY_DST << 24) |
		       DIV_ROUND_UP(dest_width, 4));
	gx_load_bp_reg((BP_DISP_COPY_ADDR << 24) |
		       ((virt_to_phys(dest) >> 5) & 0x00ffffff));

	/* GX_SetTexCopyDst(..., GX_TF_RGB565, false), then GX_CopyTex(). */
	ctrl = (BP_DISP_COPY_CTRL << 24) | BIT(16) | (4U << 4) |
	       (clear ? COPY_CTRL_CLEAR : 0);
	gx_load_bp_reg(ctrl);

	/* GX_PixModeSync: order texture-copy writes before later consumers. */
	gx_load_bp_reg(0x43000040);
	gx_load_bp_reg(0x45000002);
}

static void gx_copy_efb_rect_to_rgb565_texture(void *dest, u16 left, u16 top,
					       u16 width, u16 height,
					       bool clear)
{
	gx_copy_efb_rect_to_rgb565_texture_stride(dest, left, top, width,
						  height, width, clear);
}

static void gx_copy_efb_to_rgb565_texture(void *dest, u16 width, u16 height,
					  bool clear)
{
	gx_copy_efb_rect_to_rgb565_texture(dest, 0, 0, width, height,
					   clear);
}

static void __maybe_unused gcn_gx_copy_efb_to_xfb(u32 xfb_phys, u16 width,
						  u16 height)
{
	gx_copy_efb_to_xfb(xfb_phys, width, height, false);
}

static void gx_capture_xfb(u32 xfb_phys, u16 width, u16 height)
{
	void *xfb;
	size_t bytes = (size_t)width * height * 2;

	if (!gx_xfb_snapshot || bytes > GX_XFB_SNAPSHOT_MAX)
		return;

	xfb = memremap(xfb_phys, bytes, MEMREMAP_WB);
	if (!xfb) {
		pr_warn_once("gcn-gx: failed to map XFB snapshot at 0x%08x\n",
			     xfb_phys);
		return;
	}

	/* The PE wrote this WB-mapped RAM after any CPU cache allocation. */
	invalidate_dcache_range((unsigned long)xfb,
				(unsigned long)xfb + bytes);
	memcpy(gx_xfb_snapshot, xfb, bytes);
	memunmap(xfb);

	gx_xfb_snapshot_width = width;
	gx_xfb_snapshot_height = height;
	gx_xfb_snapshot_phys = xfb_phys;
	smp_wmb();
	WRITE_ONCE(gx_xfb_snapshot_size, bytes);
	if (gx_xfb_debugfs_file)
		i_size_write(d_inode(gx_xfb_debugfs_file), bytes);
	pr_info("gcn-gx: captured XFB phys=%08x size=%zu %ux%u\n",
		xfb_phys, bytes, width, height);
}

static ssize_t gx_xfb_snapshot_read(struct file *file, char __user *buf,
				    size_t count, loff_t *ppos)
{
	size_t size = READ_ONCE(gx_xfb_snapshot_size);

	smp_rmb();
	return simple_read_from_buffer(buf, count, ppos, gx_xfb_snapshot, size);
}

static const struct file_operations gx_xfb_snapshot_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = gx_xfb_snapshot_read,
	.llseek = default_llseek,
};

static ssize_t gx_vfb_snapshot_read(struct file *file, char __user *buf,
				    size_t count, loff_t *ppos)
{
	size_t size = READ_ONCE(gx_vfb_snapshot_size);

	smp_rmb();
	return simple_read_from_buffer(buf, count, ppos, gx_vfb_snapshot, size);
}

static const struct file_operations gx_vfb_snapshot_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = gx_vfb_snapshot_read,
	.llseek = default_llseek,
};

static void gx_capture_vfb(const void *vfb, u16 width, u16 height)
{
	size_t bytes = (size_t)width * height * 2;
	u16 *dst = gx_vfb_snapshot;
	u32 x, y;

	if (!gx_vfb_snapshot || bytes > GX_XFB_SNAPSHOT_MAX)
		return;

	if (!gx_use_pattern && !gx_use_probe) {
		memcpy(dst, vfb, bytes);
	} else {
		for (y = 0; y < height; y++) {
			for (x = 0; x < width; x++) {
				dst[y * width + x] = gx_use_pattern ?
					gx_reference_rgb565_pixel(x, y, width, height) :
					gx_probe_rgb565_pixel(x, y);
			}
		}
	}
	smp_wmb();
	WRITE_ONCE(gx_vfb_snapshot_size, bytes);
	if (gx_vfb_debugfs_file)
		i_size_write(d_inode(gx_vfb_debugfs_file), bytes);
}

/* ------------------------------------------------------------------ */
/* Public blit API — called from vi_irq_handler in gcnfb.c            */
/* ------------------------------------------------------------------ */

/*
 * gx_submit_cmds - submit commands in gx_fifo_buf to the CP.
 *
 * Pads to 32-byte alignment, flushes dcache so GP DMA sees the writes,
 * then configures CP BASE/END/RD/WT and enables GP reads.
 *
 * Called from the framebuffer worker. The VI DI1 hard IRQ only queues work,
 * keeping the 640x480 tiling pass and hardware polling out of IRQ context.
 *
 * Critical: flush gx_fifo_buf BEFORE setting WT or enabling the GP.
 * The setup functions write commands into CPU cache; without the flush
 * the GP's DMA bus reads stale zeros from physical RAM.
 */
static int gx_submit_cmds(const char *phase)
{
	static bool logged_first_slow;
	static bool logged_first_stall;
	bool token_seen;
	u32 phys_start = (u32)virt_to_phys(gx_fifo_buf);
	u32 phys_end   = phys_start + GX_FIFO_SIZE - 4;
	u32 phys_wt;
	u32 cp_rd, cp_wt;
	u16 pe_status, pe_token;
	int pe_timeout;
	int timeout;
	int ret = 0;

	/* End every submission with a unique, directly readable PE marker. */
	gx_expected_token++;
	if (!gx_expected_token)
		gx_expected_token++;
	gx_load_bp_reg(0x48000000 | gx_expected_token);
	gx_load_bp_reg(0x47000000 | gx_expected_token);

	/* Pad to 32-byte boundary (GP DMA requires 32-byte alignment) */
	while (fifo_pos & 0x1f)
		gx_wr8(0);
	phys_wt = phys_start + fifo_pos;

	cp_write(CP_REG_CTRL, 0);

	flush_dcache_range((unsigned long)gx_fifo_buf,
			   (unsigned long)gx_fifo_buf + fifo_pos);

	/* Program CP FIFO extent and read/write pointers */
	cp_write(CP_REG_FIFO_BASE_HI, phys_start >> 16);
	cp_write(CP_REG_FIFO_BASE_LO, phys_start & 0xffff);
	cp_write(CP_REG_FIFO_END_HI,  phys_end   >> 16);
	cp_write(CP_REG_FIFO_END_LO,  phys_end   & 0xffff);
	cp_write(CP_REG_RD_HI, phys_start >> 16);
	cp_write(CP_REG_RD_LO, phys_start & 0xffff);
	cp_write(CP_REG_WT_HI, phys_wt >> 16);
	cp_write(CP_REG_WT_LO, phys_wt & 0xffff);

	pi_write(PI_REG_FIFO_BASE, phys_start & ~0x1fu);
	pi_write(PI_REG_FIFO_END,  phys_end   & ~0x1fu);
	pi_write(PI_REG_FIFO_WPTR, phys_wt);

	/* Enable PE events and acknowledge stale token/finish status. */
	pe_write(PE_REG_INTR_STATUS,
		 PE_TOKEN_ENABLE | PE_FINISH_ENABLE |
		 PE_TOKEN_BIT | PE_FINISH_BIT);
	cp_write(CP_REG_CTRL, CP_CR_GPRESET | CP_CR_LINKEN);

	/*
	 * Positive control for PE event delivery. The command stream contains
	 * libogc's exact BP 0x48/BP 0x47 draw-sync sequence with a new token each
	 * frame. Require the token-value register to match this submission; the
	 * status bit alone does not identify which token asserted it.
	 */
	pe_timeout = 2000;
	do {
		pe_status = pe_read(PE_REG_INTR_STATUS);
		pe_token = pe_read(PE_REG_TOKEN);
		if (pe_token == gx_expected_token)
			break;
		udelay(10);
	} while (--pe_timeout);
	token_seen = pe_token == gx_expected_token;

	if (pe_status & PE_TOKEN_BIT) {
		/* Leave finish asserted for the PE IRQ completion handler. */
		pe_write(PE_REG_INTR_STATUS,
			 (pe_status & 0x0003) |
			 PE_TOKEN_BIT);
	}
	if (!token_seen) {
		pr_warn_once("gcn-gx: PE token positive control timed out (PE=%04x token=%04x expected=%04x)\n",
			     pe_status, pe_token, gx_expected_token);
		ret = -ETIMEDOUT;
	}
	/* Read back RD after delay: confirms GP consumed commands */
	cp_rd = ((u32)cp_read(CP_REG_RD_HI) << 16) |
		cp_read(CP_REG_RD_LO);
	cp_wt = ((u32)cp_read(CP_REG_WT_HI) << 16) |
		cp_read(CP_REG_WT_LO);
	if (cp_rd != cp_wt) {
		if (!logged_first_slow) {
			pr_warn("gcn-gx: first slow %s submit SR=%04x RDoff=%04x WToff=%04x PIoff=%04x pos=%u\n",
				phase, cp_read(CP_REG_STATUS),
				cp_rd - phys_start, cp_wt - phys_start,
				pi_read(PI_REG_FIFO_WPTR) - phys_start, fifo_pos);
			logged_first_slow = true;
		}

		/*
		 * Do not stop CP while it is mid-FIFO.  Frame 13 has been seen at
		 * RD=0x120/WT=0x180 after the fixed 2 ms delay; disabling CP there
		 * truncates the command stream and leaves later frames unrestartable.
		 */
		timeout = 800;
		while (timeout-- && cp_rd != cp_wt) {
			udelay(10);
			cp_rd = ((u32)cp_read(CP_REG_RD_HI) << 16) |
				cp_read(CP_REG_RD_LO);
			cp_wt = ((u32)cp_read(CP_REG_WT_HI) << 16) |
				cp_read(CP_REG_WT_LO);
		}
	}
	if (!logged_first_stall && cp_rd != cp_wt) {
		u32 off = cp_rd - phys_start;
		u8 *fifo = (u8 *)gx_fifo_buf;
		u32 dump = off >= 16 ? off - 16 : 0;

		pr_warn("gcn-gx: first stalled %s submit SR=%04x RDoff=%04x WToff=%04x PIoff=%04x pos=%u\n",
			phase, cp_read(CP_REG_STATUS),
			off, cp_wt - phys_start,
			pi_read(PI_REG_FIFO_WPTR) - phys_start, fifo_pos);
		pr_warn("gcn-gx: stall_bytes @%04x: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
			dump,
			fifo[dump + 0], fifo[dump + 1], fifo[dump + 2], fifo[dump + 3],
			fifo[dump + 4], fifo[dump + 5], fifo[dump + 6], fifo[dump + 7],
			fifo[dump + 8], fifo[dump + 9], fifo[dump + 10], fifo[dump + 11],
			fifo[dump + 12], fifo[dump + 13], fifo[dump + 14], fifo[dump + 15]);
		dump = off;
		pr_warn("gcn-gx: stall_bytes @%04x: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
			dump,
			fifo[dump + 0], fifo[dump + 1], fifo[dump + 2], fifo[dump + 3],
			fifo[dump + 4], fifo[dump + 5], fifo[dump + 6], fifo[dump + 7],
			fifo[dump + 8], fifo[dump + 9], fifo[dump + 10], fifo[dump + 11],
			fifo[dump + 12], fifo[dump + 13], fifo[dump + 14], fifo[dump + 15]);
		logged_first_stall = true;
	}
	if (cp_rd != cp_wt)
		ret = -ETIMEDOUT;

	cp_write(CP_REG_CTRL, 0);

	/*
	 * Poll for GP command-idle (SR bit 3 = 0x0008) before returning.
	 * The rasterizer/TEV/PE backend keeps running after the CP stops
	 * reading the FIFO.  Without this wait, reprogramming CP BASE/END/
	 * RD/WT for the next frame races the still-active downstream pipeline,
	 * leaving SR=0x0000 at the next pre-log and causing the CP to never
	 * start reading (RDoff=0x0000).  Observed after ~16 TEX0 frames.
	 */
	{
		int t = 2000;

		while (t-- && !(cp_read(CP_REG_STATUS) & 0x0008))
			udelay(10);
		if (!(cp_read(CP_REG_STATUS) & 0x0008)) {
			pr_warn_once("gcn-gx: pipeline did not go idle after submit (SR=0x%04x)\n",
				     cp_read(CP_REG_STATUS));
			ret = -ETIMEDOUT;
		}
	}

	return ret;
}

/*
 * One complete red 640x480 frame captured from libogc and independently
 * replayed red in Dolphin FIFO Player. Only BP 0x4b's XFB address is patched
 * before submission. Capture SHA-256:
 * 8b42cc84e28b8ab09e53f981f4c1b197ec57fa0ee0029704f78d1a819f1aa302
 */
static const u8 gx_reference_red_frame[] = {
	0x61, 0x40, 0x00, 0x00, 0x0e, 0x61, 0x41, 0x00, 0x31, 0x1c, 0x61, 0x43,
	0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x61,
	0x4f, 0x00, 0xff, 0x00, 0x61, 0x50, 0x00, 0xff, 0x00, 0x61, 0x51, 0xff,
	0xff, 0xff, 0x10, 0x00, 0x05, 0x10, 0x1a, 0x43, 0xa0, 0x00, 0x00, 0xc3,
	0x70, 0x00, 0x00, 0x4b, 0x7f, 0xff, 0xff, 0x44, 0x25, 0x80, 0x00, 0x44,
	0x11, 0x80, 0x00, 0x4b, 0x7f, 0xff, 0xff, 0x61, 0x4e, 0x00, 0x01, 0x00,
	0x61, 0x20, 0x15, 0x61, 0x56, 0x61, 0x21, 0x3d, 0x53, 0x35, 0x61, 0x01,
	0x66, 0x66, 0x66, 0x61, 0x02, 0x66, 0x66, 0x66, 0x61, 0x03, 0x66, 0x66,
	0x66, 0x61, 0x04, 0x66, 0x66, 0x66, 0x61, 0x53, 0x30, 0xa2, 0x08, 0x61,
	0x54, 0x00, 0x82, 0x0a, 0x61, 0x22, 0x00, 0x06, 0x06, 0x61, 0x0f, 0x00,
	0x00, 0x00, 0x61, 0x68, 0x00, 0x00, 0x00, 0x61, 0x0f, 0x00, 0x00, 0x00,
	0x61, 0x28, 0x04, 0x90, 0x00, 0x61, 0xc0, 0x08, 0xff, 0xfa, 0x61, 0xc1,
	0x08, 0xff, 0xd0, 0x61, 0xc0, 0x08, 0xff, 0xfa, 0x61, 0xc1, 0x08, 0xff,
	0xd0, 0x61, 0xc1, 0x08, 0xff, 0xd0, 0x61, 0xf6, 0x01, 0x80, 0x64, 0x61,
	0xf7, 0x01, 0x80, 0x6e, 0x10, 0x00, 0x00, 0x10, 0x05, 0x00, 0x00, 0x00,
	0x00, 0x61, 0x40, 0x00, 0x00, 0x0e, 0x61, 0x41, 0x00, 0x31, 0x1c, 0x61,
	0x41, 0x00, 0x31, 0x1c, 0x61, 0x41, 0x00, 0x31, 0x1c, 0x61, 0x42, 0x00,
	0x00, 0x00, 0x61, 0xf3, 0x3f, 0x00, 0x00, 0x61, 0x43, 0x00, 0x00, 0x40,
	0x10, 0x00, 0x0b, 0x00, 0x00, 0x3f, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x3f, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3f, 0x80, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x06, 0x10, 0x20, 0x3b, 0x4c,
	0xcc, 0xcd, 0xbf, 0x80, 0x00, 0x00, 0xbb, 0x88, 0x88, 0x89, 0x3f, 0x80,
	0x00, 0x00, 0xbf, 0x80, 0x00, 0x00, 0xbf, 0x80, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x01, 0x61, 0x00, 0x00, 0x00, 0x10, 0x08, 0x50, 0x00, 0x00, 0x22,
	0x00, 0x08, 0x60, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x10, 0x08,
	0x00, 0x00, 0x00, 0x01, 0x08, 0x70, 0x40, 0x01, 0x60, 0x08, 0x08, 0x80,
	0x80, 0x00, 0x00, 0x00, 0x08, 0x90, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
	0x00, 0x10, 0x09, 0x00, 0x00, 0x00, 0x01, 0x10, 0x00, 0x00, 0x10, 0x0e,
	0x00, 0x00, 0x04, 0x01, 0x10, 0x00, 0x00, 0x10, 0x10, 0x00, 0x00, 0x04,
	0x01, 0x10, 0x00, 0x00, 0x10, 0x3f, 0x00, 0x00, 0x00, 0x00, 0x08, 0x30,
	0x3c, 0xf3, 0xcf, 0x00, 0x10, 0x00, 0x00, 0x10, 0x18, 0x3c, 0xf3, 0xcf,
	0x00, 0x08, 0x40, 0x00, 0xf3, 0xcf, 0x3c, 0x10, 0x00, 0x00, 0x10, 0x19,
	0x00, 0xf3, 0xcf, 0x3c, 0x80, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0xff, 0x44, 0x20, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0xff, 0x44, 0x20, 0x00, 0x00, 0x43,
	0xf0, 0x00, 0x00, 0xff, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0x43,
	0xf0, 0x00, 0x00, 0xff, 0x00, 0x00, 0xff, 0x61, 0x45, 0x00, 0x00, 0x02,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x61, 0x40, 0x00, 0x00,
	0x0f, 0x61, 0x41, 0x00, 0x31, 0x1c, 0x61, 0x43, 0x00, 0x00, 0x00, 0x61,
	0x49, 0x00, 0x00, 0x00, 0x61, 0x4a, 0x07, 0x7e, 0x7f, 0x61, 0x4d, 0x00,
	0x00, 0x28, 0x61, 0x4b, 0x00, 0x31, 0xd4, 0x61, 0x52, 0x00, 0x48, 0x03,
};

/*
 * One complete RGB565 texture frame captured from libogc and validated in
 * Dolphin FIFO Player. BP 0x94 and BP 0x4b are patched before submission.
 * Capture SHA-256:
 * df9d2ee358b625886d0fd76ffe60f2c5e4b48f5aaed095a9c0d4062c70bea09c
 * Extracted FIFO SHA-256:
 * 3f0ee8c9029abfd2e718c7503e1bd6a8d1c31fe2aa283b184c18364062df7a32
 */
static const u8 gx_reference_texture_frame[] = {
  0x61, 0x40, 0x00, 0x00, 0x0e, 0x61, 0x41, 0x00, 0x31, 0x1c, 0x61, 0x43,
  0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x61,
  0x4f, 0x00, 0xff, 0x00, 0x61, 0x50, 0x00, 0xff, 0x00, 0x61, 0x51, 0xff,
  0xff, 0xff, 0x10, 0x00, 0x05, 0x10, 0x1a, 0x43, 0xa0, 0x00, 0x00, 0xc3,
  0x70, 0x00, 0x00, 0x4b, 0x7f, 0xff, 0xff, 0x44, 0x25, 0x80, 0x00, 0x44,
  0x11, 0x80, 0x00, 0x4b, 0x7f, 0xff, 0xff, 0x61, 0x4e, 0x00, 0x01, 0x00,
  0x61, 0x20, 0x15, 0x61, 0x56, 0x61, 0x21, 0x3d, 0x53, 0x35, 0x61, 0x01,
  0x66, 0x66, 0x66, 0x61, 0x02, 0x66, 0x66, 0x66, 0x61, 0x03, 0x66, 0x66,
  0x66, 0x61, 0x04, 0x66, 0x66, 0x66, 0x61, 0x53, 0x30, 0xa2, 0x08, 0x61,
  0x54, 0x00, 0x82, 0x0a, 0x61, 0x22, 0x00, 0x06, 0x06, 0x61, 0x0f, 0x00,
  0x00, 0x00, 0x61, 0x68, 0x00, 0x00, 0x00, 0x61, 0x0f, 0x00, 0x00, 0x00,
  0x10, 0x00, 0x07, 0x00, 0x78, 0x3a, 0xcc, 0xcc, 0xcd, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x3b, 0x08, 0x88, 0x89, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x61, 0x80, 0x00, 0x01, 0x00, 0x61, 0x84, 0x00, 0x00, 0x00, 0x61,
  0x88, 0x47, 0x7e, 0x7f, 0x61, 0x8c, 0x0d, 0x80, 0x00, 0x61, 0x90, 0x0d,
  0xc0, 0x00, 0x61, 0x94, 0x00, 0x7d, 0x44, 0x61, 0x0f, 0x00, 0x00, 0x00,
  0x61, 0x66, 0x00, 0x10, 0x00, 0x61, 0x66, 0x00, 0x11, 0x00, 0x61, 0x0f,
  0x00, 0x00, 0x00, 0x61, 0x28, 0x04, 0x90, 0x40, 0x61, 0xc0, 0x08, 0xff,
  0xf8, 0x61, 0xc1, 0x08, 0xff, 0xc0, 0x61, 0xc0, 0x08, 0xff, 0xf8, 0x61,
  0xc1, 0x08, 0xff, 0xc0, 0x61, 0xc1, 0x08, 0xff, 0xc0, 0x61, 0xf6, 0x01,
  0x80, 0x64, 0x61, 0xf7, 0x01, 0x80, 0x6e, 0x10, 0x00, 0x00, 0x10, 0x05,
  0x00, 0x00, 0x00, 0x00, 0x61, 0x40, 0x00, 0x00, 0x0e, 0x61, 0x41, 0x00,
  0x31, 0x1c, 0x61, 0x41, 0x00, 0x31, 0x1c, 0x61, 0x41, 0x00, 0x31, 0x1c,
  0x61, 0x42, 0x00, 0x00, 0x00, 0x61, 0xf3, 0x3f, 0x00, 0x00, 0x61, 0x43,
  0x00, 0x00, 0x40, 0x10, 0x00, 0x0b, 0x00, 0x00, 0x3f, 0x80, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x3f, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x3f, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x06, 0x10,
  0x20, 0x3b, 0x4c, 0xcc, 0xcd, 0xbf, 0x80, 0x00, 0x00, 0xbb, 0x88, 0x88,
  0x89, 0x3f, 0x80, 0x00, 0x00, 0xbf, 0x80, 0x00, 0x00, 0xbf, 0x80, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x01, 0x61, 0x30, 0x00, 0x02, 0x7f, 0x61, 0x31,
  0x00, 0x01, 0xdf, 0x61, 0x00, 0x00, 0x00, 0x11, 0x08, 0x50, 0x00, 0x00,
  0x22, 0x00, 0x08, 0x60, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x10,
  0x08, 0x00, 0x00, 0x00, 0x01, 0x08, 0x70, 0x40, 0x01, 0x60, 0x08, 0x08,
  0x80, 0x80, 0x00, 0x00, 0x00, 0x08, 0x90, 0x00, 0x00, 0x00, 0x00, 0x10,
  0x00, 0x00, 0x10, 0x09, 0x00, 0x00, 0x00, 0x01, 0x10, 0x00, 0x00, 0x10,
  0x0e, 0x00, 0x00, 0x04, 0x01, 0x10, 0x00, 0x00, 0x10, 0x10, 0x00, 0x00,
  0x04, 0x01, 0x10, 0x00, 0x00, 0x10, 0x3f, 0x00, 0x00, 0x00, 0x01, 0x10,
  0x00, 0x00, 0x10, 0x40, 0x00, 0x00, 0x00, 0x04, 0x10, 0x00, 0x00, 0x10,
  0x50, 0x00, 0x00, 0x00, 0x3d, 0x08, 0x30, 0x3c, 0xf3, 0xc7, 0x80, 0x10,
  0x00, 0x00, 0x10, 0x18, 0x3c, 0xf3, 0xc7, 0x80, 0x08, 0x40, 0x00, 0xf3,
  0xcf, 0x3c, 0x10, 0x00, 0x00, 0x10, 0x19, 0x00, 0xf3, 0xcf, 0x3c, 0x80,
  0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0x00,
  0x00, 0xff, 0x44, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0x00,
  0x00, 0xff, 0x44, 0x20, 0x00, 0x00, 0x43, 0xf0, 0x00, 0x00, 0xff, 0x00,
  0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0x43, 0xf0, 0x00, 0x00, 0xff, 0x00,
  0x00, 0xff, 0x61, 0x45, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x61, 0x40, 0x00, 0x00, 0x0f, 0x61, 0x41, 0x00, 0x31,
  0x1c, 0x61, 0x43, 0x00, 0x00, 0x00, 0x61, 0x49, 0x00, 0x00, 0x00, 0x61,
  0x4a, 0x07, 0x7e, 0x7f, 0x61, 0x4d, 0x00, 0x00, 0x28, 0x61, 0x4b, 0x00,
  0x32, 0x43, 0x61, 0x52, 0x00, 0x48, 0x03
};

#define GX_REFERENCE_FRAME_SIZE		564
#define GX_REFERENCE_XFB_ADDR_OFFSET	0x22c
#define GX_REFERENCE_TEXTURE_FRAME_SIZE	679
#define GX_REFERENCE_TEXTURE_ADDR_OFFSET	0x0dc
#define GX_REFERENCE_TEXTURE_XFB_ADDR_OFFSET	0x29f

static void gx_load_libogc_init_preamble(void)
{
	u8 i;

	/* Low-level command preamble from Wii libogc GX_Init(), in order. */
	gx_load_bp_reg(0x0F0000FF);
	gx_load_bp_reg(0x690004ED);
	gx_load_bp_reg(0x0F0000FF);
	gx_load_bp_reg(0x46000273);

	for (i = 0; i < 8; i++)
		gx_load_cp_reg(0x80 | i, 0x80000000);
	gx_load_xf_reg(0x1000, 0x0000003F);
	gx_load_xf_reg(0x1012, 0x00000001);
	gx_load_bp_reg(0x5800000F);

	gx_load_cp_reg(0x20, 0x00000000);
	gx_load_xf_reg(0x1006, 0x00000000);
	gx_load_bp_reg(0x23000000);
	gx_load_bp_reg(0x24000000);
	gx_load_bp_reg(0x67000000);
	gx_load_bp_reg(0x0F000000);

	/* Wii __GX_SetTmemConfig(2). */
	gx_load_bp_reg(0x8C0D8000);
	gx_load_bp_reg(0x900DC000);
	gx_load_bp_reg(0x8D0D8800);
	gx_load_bp_reg(0x910DC800);
	gx_load_bp_reg(0x8E0D9000);
	gx_load_bp_reg(0x920DD000);
	gx_load_bp_reg(0x8F0D9800);
	gx_load_bp_reg(0x930DD800);
	gx_load_bp_reg(0xAC0DA000);
	gx_load_bp_reg(0xB00DC400);
	gx_load_bp_reg(0xAD0DA800);
	gx_load_bp_reg(0xB10DCC00);
	gx_load_bp_reg(0xAE0DB000);
	gx_load_bp_reg(0xB20DD400);
	gx_load_bp_reg(0xAF0DB800);
	gx_load_bp_reg(0xB30DDC00);
}

static void __maybe_unused gx_load_reference_red_frame(u32 xfb_phys,
					       u16 width, u16 height)
{
	u32 copy_addr = (xfb_phys >> 5) & 0x00ffffff;
	u8 *fifo = gx_fifo_buf;
	u32 start = fifo_pos;

	if (WARN_ON_ONCE(width != 640 || height != 480))
		return;
	BUILD_BUG_ON(sizeof(gx_reference_red_frame) != GX_REFERENCE_FRAME_SIZE);

	memcpy(fifo + start, gx_reference_red_frame,
	       sizeof(gx_reference_red_frame));
	fifo[start + GX_REFERENCE_XFB_ADDR_OFFSET + 0] = copy_addr >> 16;
	fifo[start + GX_REFERENCE_XFB_ADDR_OFFSET + 1] = copy_addr >> 8;
	fifo[start + GX_REFERENCE_XFB_ADDR_OFFSET + 2] = copy_addr;
	fifo_pos += sizeof(gx_reference_red_frame);
}

static void gx_load_reference_texture_frame(void *tile_buf, u32 xfb_phys,
					    u16 width, u16 height)
{
	u32 texture_addr = (virt_to_phys(tile_buf) >> 5) & 0x00ffffff;
	u32 copy_addr = (xfb_phys >> 5) & 0x00ffffff;
	u8 *fifo = gx_fifo_buf;
	u32 start = fifo_pos;

	if (WARN_ON_ONCE(width != 640 || height != 480))
		return;
	BUILD_BUG_ON(sizeof(gx_reference_texture_frame) !=
		     GX_REFERENCE_TEXTURE_FRAME_SIZE);

	memcpy(fifo + start, gx_reference_texture_frame,
	       sizeof(gx_reference_texture_frame));
	fifo[start + GX_REFERENCE_TEXTURE_ADDR_OFFSET + 0] = texture_addr >> 16;
	fifo[start + GX_REFERENCE_TEXTURE_ADDR_OFFSET + 1] = texture_addr >> 8;
	fifo[start + GX_REFERENCE_TEXTURE_ADDR_OFFSET + 2] = texture_addr;
	fifo[start + GX_REFERENCE_TEXTURE_XFB_ADDR_OFFSET + 0] = copy_addr >> 16;
	fifo[start + GX_REFERENCE_TEXTURE_XFB_ADDR_OFFSET + 1] = copy_addr >> 8;
	fifo[start + GX_REFERENCE_TEXTURE_XFB_ADDR_OFFSET + 2] = copy_addr;
	fifo_pos += sizeof(gx_reference_texture_frame);
}

static void gx_submit_reference(const void *vfb, u32 xfb_phys,
				u16 width, u16 height, const char *phase,
				u32 src_pitch, enum gx_vfb_format format)
{
	u32 live_frame = gx_live_texture_frame++;
	void *tex_buf = (live_frame & 1) ?
		gx_tex_buf_alt : gx_tex_buf;
	u32 pixel_count = (u32)width * height;

	gx_prepare_texture(vfb, tex_buf, width, height, src_pitch, format);
	flush_dcache_range((unsigned long)tex_buf,
			   (unsigned long)tex_buf +
			   pixel_count * sizeof(u16));

	fifo_pos = 0;
	if (!live_frame)
		gx_load_libogc_init_preamble();
	/* Prerequisites held in libogc's initial state, not its frame FIFO. */
	gx_load_xf_reg(0x1012, 0x00000001);
	gx_load_identity_post_mtx();
	gx_load_bp_reg(0x5902ACAB);
	gx_load_reference_texture_frame(tex_buf, xfb_phys, width, height);
	gx_submit_cmds(phase);
}

static int gx_submit_generated(const void *vfb, u32 xfb_phys,
			       u16 width, u16 height, const char *phase,
			       u32 src_pitch, enum gx_vfb_format format)
{
	u32 live_frame = gx_live_texture_frame++;
	void *tex_buf = (live_frame & 1) ?
		gx_tex_buf_alt : gx_tex_buf;
	u32 pixel_count = (u32)width * height;
	u64 tile_start = 0;
	u64 tile_end = 0;
	u64 flush_end;
	u64 tile_ns;
	u64 flush_ns;
	int i;

	if (format == GX_VFB_RGB565)
		gx_capture_vfb(vfb, width, height);
	else
		tile_start = ktime_get_ns();
	gx_prepare_texture(vfb, tex_buf, width, height, src_pitch, format);
	if (format == GX_VFB_XRGB8888)
		tile_end = ktime_get_ns();
	flush_dcache_range((unsigned long)tex_buf,
			   (unsigned long)tex_buf +
			   pixel_count * sizeof(u16));
	if (format == GX_VFB_XRGB8888) {
		flush_end = ktime_get_ns();
		tile_ns = tile_end - tile_start;
		flush_ns = flush_end - tile_end;
		gx_rgb888_tile_total_ns += tile_ns;
		gx_rgb888_flush_total_ns += flush_ns;
		if (tile_ns > gx_rgb888_tile_max_ns)
			gx_rgb888_tile_max_ns = tile_ns;
		if (flush_ns > gx_rgb888_flush_max_ns)
			gx_rgb888_flush_max_ns = flush_ns;
		gx_rgb888_timing_frames++;
		if (!(gx_rgb888_timing_frames & 0xff))
			pr_info("gcn-gx: RGB888 timing frames=%u tile_avg_us=%llu tile_max_us=%llu flush_avg_us=%llu flush_max_us=%llu\n",
				gx_rgb888_timing_frames,
				div_u64(div_u64(gx_rgb888_tile_total_ns,
						gx_rgb888_timing_frames), 1000),
				div_u64(gx_rgb888_tile_max_ns, 1000),
				div_u64(div_u64(gx_rgb888_flush_total_ns,
						gx_rgb888_timing_frames), 1000),
				div_u64(gx_rgb888_flush_max_ns, 1000));
	}

	fifo_pos = 0;
	if (!live_frame) {
		gx_load_libogc_init_preamble();
		gx_setup_display_copy_state();
	}
	gx_setup_rgb565_texture_state(width, height);
	gx_setup_texture_rgb565(tex_buf, width, height);
	if (gx_use_direct_texcoord) {
		if (gx_use_direct_triangle)
			gx_draw_textured_color_triangle(width, height,
						       0xff, 0x00, 0x00);
		else
			gx_draw_textured_color_quad(width, height,
						   0xff, 0x00, 0x00);
	} else {
		gx_draw_color_quad(width, height, 0xff, 0x00, 0x00);
	}
	gx_load_bp_reg(0x45000002);
	for (i = 0; i < 32; i++)
		gx_wr8(0);
	if (!strcmp(phase, "live0"))
		gx_set_copy_clear_rgb(0x80, 0x00, 0x80);
	else
		gx_set_copy_clear_rgb(0x00, 0x80, 0x80);
	gx_copy_efb_to_xfb(xfb_phys, width, height, true);
	return gx_submit_cmds(phase);
}

static void gx_submit_direct_pattern(u32 xfb_phys, u16 width, u16 height,
				     const char *phase)
{
	u32 live_frame = gx_live_texture_frame++;
	int i;

	fifo_pos = 0;
	if (!live_frame)
		gx_load_libogc_init_preamble();
	gx_setup_vertex_color_state(width, height);
	gx_draw_direct_pattern(width, height);
	gx_load_bp_reg(0x45000002);
	for (i = 0; i < 32; i++)
		gx_wr8(0);
	if (!strcmp(phase, "live0"))
		gx_set_copy_clear_rgb(0x80, 0x00, 0x80);
	else
		gx_set_copy_clear_rgb(0x00, 0x80, 0x80);
	gx_copy_efb_to_xfb(xfb_phys, width, height, true);
	gx_submit_cmds(phase);
}

static void gx_submit_selected(const void *vfb, u32 xfb_phys,
			       u16 width, u16 height, const char *phase,
			       enum gx_vfb_format format)
{
	u32 src_pitch = width * (format == GX_VFB_RGB565 ? 2 : 4);

	if (gx_use_direct)
		gx_submit_direct_pattern(xfb_phys, width, height, phase);
	else if (gx_use_reference)
		gx_submit_reference(vfb, xfb_phys, width, height, phase,
				    src_pitch, format);
	else
		gx_submit_generated(vfb, xfb_phys, width, height, phase,
				    src_pitch, format);
}

/*
 * gx_process_frame - asynchronous PE-finish primitive diagnostic.
 *
 * Validate the real PE-finish IRQ with known copies, then continuously submit
 * a tiled RGB565 texture frame from process context.
 */
static bool __maybe_unused gx_process_frame(const void *vfb, u32 xfb_phys,
					    u16 width, u16 height,
					    enum gx_vfb_format format)
{
	u32 finish_count;
	bool submitted = false;

	finish_count = READ_ONCE(gx_pe_finish_count);

	switch (gx_diag_phase) {
	case GX_DIAG_SEED:
		gx_diag_finish_baseline = finish_count;
		fifo_pos = 0;
		gx_setup_display_copy_state();
		gx_set_copy_clear_rgb(0x00, 0x00, 0xff);
		gx_copy_efb_to_xfb(xfb_phys, width, height, true);
		gx_submit_cmds("seed");
		gx_diag_phase = GX_DIAG_WAIT_SEED;
		submitted = true;
		break;

	case GX_DIAG_WAIT_SEED:
		if (finish_count == gx_diag_finish_baseline)
			break;
		gx_diag_finish_baseline = finish_count;
		fifo_pos = 0;
		gx_set_copy_clear_rgb(0xff, 0x00, 0x00);
		gx_copy_efb_to_xfb(xfb_phys, width, height, true);
		gx_submit_cmds("blue");
		gx_diag_phase = GX_DIAG_WAIT_BLUE;
		submitted = true;
		break;

	case GX_DIAG_WAIT_BLUE:
		if (finish_count == gx_diag_finish_baseline)
			break;
		gx_diag_finish_baseline = finish_count;
		fifo_pos = 0;
		gx_load_libogc_init_preamble();
		gx_setup_display_copy_state();
		gx_set_copy_clear_rgb(0x80, 0x00, 0x80);
		gx_copy_efb_to_xfb(xfb_phys, width, height, true);
		gx_submit_cmds("init");
		gx_diag_phase = GX_DIAG_WAIT_INIT;
		submitted = true;
		break;

	case GX_DIAG_WAIT_INIT:
		if (finish_count == gx_diag_finish_baseline)
			break;
		gx_diag_finish_baseline = finish_count;
		gx_submit_selected(vfb, xfb_phys, width, height, "live0",
				   format);
		gx_diag_phase = GX_DIAG_WAIT_DRAW;
		submitted = true;
		break;

	case GX_DIAG_WAIT_DRAW:
		if (finish_count == gx_diag_finish_baseline)
			break;
		pr_info("gcn-gx: %s renderer active\n", gx_renderer);
		gx_diag_phase = GX_DIAG_DONE;
		break;

	case GX_DIAG_DONE:
		gx_submit_selected(vfb, xfb_phys, width, height, "live", format);
		gx_frame_publish_xfb = true;
		if (gx_hold_frame && gx_live_texture_frame >= gx_hold_frame) {
			gx_capture_xfb(xfb_phys, width, height);
			gx_frame_hold = true;
			pr_info("gcn-gx: publishing %s frame %u once and holding output\n",
				gx_renderer, gx_live_texture_frame);
		}
		submitted = true;
		break;
	}

	return submitted;
}

#if IS_ENABLED(CONFIG_FB_GAMECUBE_GX)
static void gx_frame_workfn(struct work_struct *work)
{
	const void *vfb;
	u32 xfb_phys;
	u16 width, height;
	enum gx_vfb_format format;
	u32 source_generation;
	unsigned long flags;
	bool submitted;

	(void)work;
	spin_lock_irqsave(&gx_frame_work_lock, flags);
	vfb = gx_frame_work.vfb;
	xfb_phys = gx_frame_work.xfb_phys;
	width = gx_frame_work.width;
	height = gx_frame_work.height;
	format = gx_frame_work.format;
	source_generation = gx_frame_work.source_generation;
	spin_unlock_irqrestore(&gx_frame_work_lock, flags);

	if (!vfb || !width || !height || !READ_ONCE(gx_accel_ready)) {
		spin_lock_irqsave(&gx_frame_work_lock, flags);
		gx_frame_work_busy = false;
		spin_unlock_irqrestore(&gx_frame_work_lock, flags);
		return;
	}

	mutex_lock(&gx_submit_lock);
	submitted = gx_process_frame(vfb, xfb_phys, width, height, format);
	mutex_unlock(&gx_submit_lock);
	/* gx_process_frame() has finished all CPU reads from this VFB page. */
	gcnfb_accel_source_consumed(&gcn_gx_accel_ops, vfb, source_generation);

	spin_lock_irqsave(&gx_frame_work_lock, flags);
	if (submitted && gx_frame_publish_xfb) {
		gx_frame_ready_xfb = xfb_phys;
		gx_frame_ready_vfb = vfb;
		gx_frame_publish_xfb = false;
	} else {
		gx_frame_work_busy = false;
	}
	spin_unlock_irqrestore(&gx_frame_work_lock, flags);
}

static bool gcn_gx_take_completed(u32 *xfb_phys, const void **vfb)
{
	unsigned long flags;
	bool ready = false;

	if (!READ_ONCE(gx_accel_ready))
		return false;

	spin_lock_irqsave(&gx_frame_work_lock, flags);
	if (gx_frame_ready_xfb) {
		*xfb_phys = gx_frame_ready_xfb;
		*vfb = gx_frame_ready_vfb;
		gx_frame_ready_xfb = 0;
		gx_frame_ready_vfb = NULL;
		gx_frame_work_busy = false;
		ready = true;
	}
	spin_unlock_irqrestore(&gx_frame_work_lock, flags);

	return ready;
}

static void gcn_gx_queue_frame(const void *vfb, u32 xfb_phys,
			       u16 width, u16 height,
			       enum gx_vfb_format format,
			       u32 source_generation)
{
	unsigned long flags;

	if (!READ_ONCE(gx_accel_ready))
		return;

	/* Do not contend with built-in driver init on this single-core system. */
	if (system_state != SYSTEM_RUNNING) {
		if (!gx_frame_boot_deferred) {
			gx_frame_boot_deferred = true;
			pr_info("gcn-gx: deferring framebuffer worker until SYSTEM_RUNNING\n");
		}
		return;
	}
	if (gx_frame_boot_deferred) {
		gx_frame_boot_deferred = false;
		pr_info("gcn-gx: SYSTEM_RUNNING; enabling framebuffer worker\n");
	}
	if (gx_frame_hold)
		return;

	spin_lock_irqsave(&gx_frame_work_lock, flags);
	if (gx_frame_work_busy) {
		spin_unlock_irqrestore(&gx_frame_work_lock, flags);
		return;
	}
	gx_frame_work.vfb = vfb;
	gx_frame_work.xfb_phys = xfb_phys;
	gx_frame_work.width = width;
	gx_frame_work.height = height;
	gx_frame_work.format = format;
	gx_frame_work.source_generation = source_generation;
	gx_frame_work_busy = true;
	spin_unlock_irqrestore(&gx_frame_work_lock, flags);

	if (!schedule_work(&gx_frame_work.work)) {
		spin_lock_irqsave(&gx_frame_work_lock, flags);
		gx_frame_work_busy = false;
		spin_unlock_irqrestore(&gx_frame_work_lock, flags);
		pr_warn_once("gcn-gx: failed to queue idle framebuffer work\n");
	}
}

static void gcn_gx_blit_fb_rgb565(const void *vfb, u32 xfb_phys,
				  u16 width, u16 height,
				  u32 source_generation)
{
	gcn_gx_queue_frame(vfb, xfb_phys, width, height, GX_VFB_RGB565,
			   source_generation);
}

/*
 * gcn_gx_blit_fb_rgb888 - blit a linear RGB888 (packed u32) FB to the XFB.
 * Converts to RGB565 during tiling to avoid GX_TF_RGBA8's complex layout.
 */
static void gcn_gx_blit_fb_rgb888(const void *vfb, u32 xfb_phys,
				   u16 width, u16 height,
				   u32 source_generation)
{
	gcn_gx_queue_frame(vfb, xfb_phys, width, height, GX_VFB_XRGB8888,
			   source_generation);
}
#endif

#if IS_ENABLED(CONFIG_DRM_GCN_GX)
static long gx_wait_for_pe_finishes(u32 baseline, u32 required)
{
	return wait_event_timeout(gx_pe_finish_wait,
				  (u32)(READ_ONCE(gx_pe_finish_count) - baseline) >=
				  required,
				  msecs_to_jiffies(50));
}

static int gx_submit_and_wait_finish(const char *phase)
{
	u32 finish_count = READ_ONCE(gx_pe_finish_count);
	int ret;

	ret = gx_submit_cmds(phase);
	if (ret)
		return ret;
	if (!gx_wait_for_pe_finishes(finish_count, 1)) {
		pr_warn_ratelimited("gcn-gx: %s timed out waiting for PE finish\n",
				    phase);
		return -ETIMEDOUT;
	}

	return 0;
}

static int gx_submit_fenced_color_rows(u16 width, u16 height,
				       u8 r, u8 g, u8 b, u32 *commands)
{
	u32 hash = 2166136261U;
	u16 y;
	int ret;

	/* The caller has authored state for row zero; later rows retain it. */
	for (y = 0; y < height; y++) {
		gx_draw_color_rect(0, y, width, y + 1, r, g, b);
		gx_load_bp_reg(0x45000002);
		hash = gx_hash_pending_commands_seed(hash);
		ret = gx_submit_and_wait_finish("render-blit-scaled-row");
		if (ret) {
			pr_warn("gcn-gx: fenced final row %u failed: %d\n", y, ret);
			return ret;
		}
		fifo_pos = 0;
	}
	*commands = hash;
	return 0;
}

static int gx_drm_offscreen_capture(u16 width, u16 height)
{
	const u32 sentinel = 0xa55aa55a;
	u32 finish_count;
	u32 changed = 0;
	u32 *dest = gx_tex_buf_alt;
	size_t bytes = (size_t)width * height * sizeof(u16);
	size_t words = bytes / sizeof(*dest);
	size_t i;
	long completed;
	int ret;

	for (i = 0; i < words; i++)
		dest[i] = sentinel;
	flush_dcache_range((unsigned long)dest,
			   (unsigned long)dest + bytes);

	finish_count = READ_ONCE(gx_pe_finish_count);
	fifo_pos = 0;
	gx_load_libogc_init_preamble();
	gx_setup_display_copy_state();
	gx_setup_vertex_color_state(width, height);
	gx_draw_direct_grid(width, height);
	gx_load_bp_reg(0x45000002);
	for (i = 0; i < 32; i++)
		gx_wr8(0);

	/* A cleared EFB prevents the direct draw from masquerading as replay. */
	gx_set_copy_clear_rgb(0x80, 0x00, 0x80);
	gx_copy_efb_to_rgb565_texture(dest, width, height, true);
	ret = gx_submit_cmds("offscreen-copy");
	if (ret)
		return ret;

	completed = gx_wait_for_pe_finishes(finish_count,
					    GX_DRM_FRAME_PE_FINISHES);
	if (!completed) {
		pr_warn("gcn-gx: offscreen texture copy timed out waiting for final PE finish\n");
		return -ETIMEDOUT;
	}

	invalidate_dcache_range((unsigned long)dest,
				(unsigned long)dest + bytes);
	for (i = 0; i < words; i++) {
		if (dest[i] != sentinel)
			changed++;
	}
	WRITE_ONCE(gx_offscreen_changed_words, changed);
	if (changed != words) {
		pr_warn("gcn-gx: offscreen texture copy changed %u/%zu words\n",
			changed, words);
		return -EIO;
	}

	WRITE_ONCE(gx_offscreen_copies,
		   READ_ONCE(gx_offscreen_copies) + 1);
	WRITE_ONCE(gx_offscreen_texture_ready, true);
	pr_info("gcn-gx: offscreen RGB565 texture copy complete changed=%u/%zu\n",
		changed, words);
	return 0;
}

static int gx_drm_offscreen_replay(u32 xfb_phys, u16 width, u16 height)
{
	u32 finish_count = READ_ONCE(gx_pe_finish_count);
	long completed;
	int ret;
	int i;

	fifo_pos = 0;
	gx_setup_rgb565_texture_state(width, height);
	gx_setup_texture_rgb565(gx_tex_buf_alt, width, height);
	gx_draw_color_quad(width, height, 0xff, 0x00, 0x00);
	gx_load_bp_reg(0x45000002);
	for (i = 0; i < 32; i++)
		gx_wr8(0);
	gx_set_copy_clear_rgb(0x00, 0x80, 0x80);
	gx_copy_efb_to_xfb(xfb_phys, width, height, true);
	ret = gx_submit_cmds("offscreen-replay");
	if (ret)
		return ret;

	completed = gx_wait_for_pe_finishes(finish_count,
					    GX_DRM_FRAME_PE_FINISHES);
	if (!completed) {
		pr_warn_ratelimited("gcn-gx: offscreen replay timed out waiting for final PE finish\n");
		return -ETIMEDOUT;
	}

	WRITE_ONCE(gx_live_texture_frame,
		   READ_ONCE(gx_live_texture_frame) + 1);
	WRITE_ONCE(gx_offscreen_replays,
		   READ_ONCE(gx_offscreen_replays) + 1);
	return 0;
}

static int gx_drm_offscreen_probe(u32 xfb_phys, u16 width, u16 height)
{
	int ret;

	if (!READ_ONCE(gx_offscreen_texture_ready)) {
		ret = gx_drm_offscreen_capture(width, height);
		if (ret)
			return ret;
	}

	return gx_drm_offscreen_replay(xfb_phys, width, height);
}

static int gcn_gx_drm_blit(const void *src, u32 src_pitch, u32 xfb_phys,
			   u16 width, u16 height, enum gx_vfb_format format)
{
	u32 bytes_per_pixel;
	u32 finish_count;
	long completed;
	int ret;

	if (READ_ONCE(gx_render_only))
		return -ENODEV;

	if (format == GX_VFB_RGB565)
		bytes_per_pixel = sizeof(u16);
	else if (format == GX_VFB_XRGB8888)
		bytes_per_pixel = sizeof(u32);
	else
		return -EINVAL;

	if (!src || !width || !height || (width & 3) || (height & 3) ||
	    src_pitch < width * bytes_per_pixel || (xfb_phys & 0x1f))
		return -EINVAL;
	if ((u32)width * height * sizeof(u16) > GX_TEX_BUF_SIZE)
		return -E2BIG;
	if (!READ_ONCE(gx_accel_ready))
		return -ENODEV;
	if (gx_use_reference || gx_use_direct || gx_use_pattern || gx_use_probe)
		return -EOPNOTSUPP;

	mutex_lock(&gx_submit_lock);
	if (gx_offscreen_probe) {
		ret = gx_drm_offscreen_probe(xfb_phys, width, height);
		if (!ret && gx_debug_capture &&
		    !READ_ONCE(gx_xfb_snapshot_size))
			gx_capture_xfb(xfb_phys, width, height);
		goto out_unlock;
	}

	finish_count = READ_ONCE(gx_pe_finish_count);
	ret = gx_submit_generated(src, xfb_phys, width, height, "drm",
				  src_pitch, format);
	if (!ret) {
		/*
		 * Generated frames signal once after rasterization and again after
		 * the EFB-to-XFB copy.  Do not publish the page at the first marker.
		 */
		/* The final token orders copyback; finish IRQs may coalesce. */
		completed = gx_wait_for_pe_finishes(finish_count, 1);
		if (!completed) {
			pr_warn_ratelimited("gcn-gx: DRM frame timed out waiting for final PE finish\n");
			ret = -ETIMEDOUT;
		} else if (gx_debug_capture &&
			   !READ_ONCE(gx_xfb_snapshot_size))
			gx_capture_xfb(xfb_phys, width, height);
	}

out_unlock:
	mutex_unlock(&gx_submit_lock);

	return ret;
}

static int gcn_gx_drm_blit_rgb565(const void *src, u32 src_pitch,
				  u32 xfb_phys, u16 width, u16 height)
{
	return gcn_gx_drm_blit(src, src_pitch, xfb_phys, width, height,
			       GX_VFB_RGB565);
}

static int gcn_gx_drm_blit_xrgb8888(const void *src, u32 src_pitch,
				    u32 xfb_phys, u16 width, u16 height)
{
	return gcn_gx_drm_blit(src, src_pitch, xfb_phys, width, height,
			       GX_VFB_XRGB8888);
}

static int gcn_gx_drm_mem1_info(struct gcn_drm_mem1_info *info)
{
	if (!info)
		return -EINVAL;

	mutex_lock(&gx_mem1_lock);
	if (!gx_mem1_allocator.initialized || gx_mem1_shutdown) {
		mutex_unlock(&gx_mem1_lock);
		return -ENODEV;
	}

	info->total_bytes = gx_mem1_total_bytes;
	info->free_bytes = gx_mem1_free_bytes;
	info->alignment = PAGE_SIZE;
	info->max_width = 640;
	info->max_height = 576;
	info->formats = DRM_GCN_FORMAT_RGB565 | DRM_GCN_FORMAT_XRGB8888 |
			DRM_GCN_FORMAT_RGBA8;
	info->layouts = DRM_GCN_LAYOUT_TILED_4X4 | DRM_GCN_LAYOUT_LINEAR;
	info->features = DRM_GCN_FEATURE_SUBMIT_RGB565 |
			 DRM_GCN_FEATURE_FILL_RGB565 |
			 DRM_GCN_FEATURE_FILL_RECT_RGB565 |
			 DRM_GCN_FEATURE_BLIT_RECT_RGB565 |
			 DRM_GCN_FEATURE_BLIT_RECT_RGB565_UNEQUAL_DIMS |
			 DRM_GCN_FEATURE_BLIT_RECT_RGB565_SAME_OBJECT |
			 DRM_GCN_FEATURE_BLIT_SCALED_RGB565 |
			 DRM_GCN_FEATURE_SYSTEM_GEM |
			 DRM_GCN_FEATURE_BLIT_SCALED_SYSTEM_RGB565 |
			 DRM_GCN_FEATURE_SYSTEM_GEM_LINEAR |
			 DRM_GCN_FEATURE_BLIT_SCALED_SYSTEM_XRGB8888_TO_RGB565 |
			 DRM_GCN_FEATURE_DRAW_TRIANGLE_RGB565 |
			 DRM_GCN_FEATURE_DRAW_TRIANGLES_RGB565 |
			 DRM_GCN_FEATURE_DRAW_TRIANGLES_STATE_RGB565 |
			 DRM_GCN_FEATURE_DRAW_TRIANGLES_DEPTH_RGB565 |
			 DRM_GCN_FEATURE_DRAW_TEXTURED_TRIANGLES_RGB565 |
			 DRM_GCN_FEATURE_DRAW_INDEXED_TRIANGLES_RGB565 |
			 DRM_GCN_FEATURE_DRAW_INDEXED_TEXTURED_TRIANGLES_RGB565 |
			 DRM_GCN_FEATURE_DRAW_INDEXED_TEXTURED_DEPTH_RGB565 |
			 DRM_GCN_FEATURE_DRAW_INDEXED_FIXED_RGB565 |
			 DRM_GCN_FEATURE_RASTER_CULL |
			 DRM_GCN_FEATURE_SYSTEM_RENDER_RGB565 |
			 DRM_GCN_FEATURE_TEXTURE_RGBA8 |
			 DRM_GCN_FEATURE_TEXTURE_LINEAR;
	mutex_unlock(&gx_mem1_lock);
	return 0;
}

static int gcn_gx_drm_mem1_alloc(size_t size, void **allocation)
{
	struct gx_mem1_allocation *mem;
	int ret;

	if (!allocation || !size || !IS_ALIGNED(size, PAGE_SIZE))
		return -EINVAL;
	*allocation = NULL;

	mem = kzalloc(sizeof(*mem), GFP_KERNEL);
	if (!mem)
		return -ENOMEM;

	mutex_lock(&gx_mem1_lock);
	if (!gx_mem1_allocator.initialized || gx_mem1_shutdown) {
		ret = -ENODEV;
		goto err_unlock;
	}
	ret = gcn_gx_mem1_insert(&gx_mem1_allocator, &mem->node, size,
				  PAGE_SIZE);
	if (ret)
		goto err_unlock;
	if (!gcn_gx_mem1_contains(&gx_mem1_allocator, &mem->node)) {
		ret = -EINVAL;
		gcn_gx_mem1_remove(&mem->node);
		goto err_unlock;
	}
	gx_mem1_used_bytes += size;
	gx_mem1_free_bytes -= size;
	gx_mem1_user_allocations++;
	mutex_unlock(&gx_mem1_lock);

	mem->cpu_addr = (void *)__va(mem->node.start);
	mem->size = size;
	memset(mem->cpu_addr, 0, size);
	flush_dcache_range((unsigned long)mem->cpu_addr,
			   (unsigned long)mem->cpu_addr + size);
	*allocation = mem;
	return 0;

err_unlock:
	mutex_unlock(&gx_mem1_lock);
	kfree(mem);
	return ret;
}

static void gcn_gx_drm_mem1_free(void *allocation)
{
	struct gx_mem1_allocation *mem = allocation;

	if (!mem)
		return;

	mutex_lock(&gx_mem1_lock);
	if (drm_mm_node_allocated(&mem->node)) {
		gx_mem1_used_bytes -= mem->size;
		gx_mem1_free_bytes += mem->size;
		gcn_gx_mem1_remove(&mem->node);
		gx_mem1_user_allocations--;
		if (gx_mem1_shutdown && !gx_mem1_user_allocations)
			gx_mem1_free_workspaces_locked();
	}
	mutex_unlock(&gx_mem1_lock);
	kfree(mem);
}

static int gcn_gx_drm_mem1_mmap(void *allocation,
				struct vm_area_struct *vma)
{
	struct gx_mem1_allocation *mem = allocation;
	unsigned long size = vma->vm_end - vma->vm_start;

	if (!mem || size != mem->size)
		return -EINVAL;

	vm_flags_set(vma, VM_IO | VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP);
	return remap_pfn_range(vma, vma->vm_start,
			       PHYS_PFN(mem->node.start), size,
			       vma->vm_page_prot);
}

static int gcn_gx_drm_submit_rgb565(void *src_allocation,
				    void *dst_allocation,
				    u16 width, u16 height)
{
	struct gx_mem1_allocation *src = src_allocation;
	struct gx_mem1_allocation *dst = dst_allocation;
	u32 finish_count;
	size_t bytes;
	long completed;
	int ret;
	int i;

	if (!src || !dst || src == dst || !width || !height ||
	    (width & 3) || (height & 3))
		return -EINVAL;
	bytes = (size_t)width * height * sizeof(u16);
	if (bytes > src->size || bytes > dst->size)
		return -E2BIG;
	if (!READ_ONCE(gx_accel_ready))
		return -ENODEV;

	mutex_lock(&gx_submit_lock);
	flush_dcache_range((unsigned long)src->cpu_addr,
			   (unsigned long)src->cpu_addr + bytes);
	flush_dcache_range((unsigned long)dst->cpu_addr,
			   (unsigned long)dst->cpu_addr + bytes);

	finish_count = READ_ONCE(gx_pe_finish_count);
	fifo_pos = 0;
	gx_load_libogc_init_preamble();
	gx_setup_display_copy_state();
	gx_setup_rgb565_texture_state(width, height);
	gx_setup_texture_rgb565(src->cpu_addr, width, height);
	gx_draw_color_quad(width, height, 0xff, 0xff, 0xff);
	gx_load_bp_reg(0x45000002);
	for (i = 0; i < 32; i++)
		gx_wr8(0);
	gx_set_copy_clear_rgb(0x00, 0x00, 0x00);
	gx_copy_efb_to_rgb565_texture(dst->cpu_addr, width, height, true);
	ret = gx_submit_cmds("render-copy");
	if (ret)
		goto out_unlock;

	/* The unique final token orders copyback; finish IRQs may coalesce. */
	completed = gx_wait_for_pe_finishes(finish_count, 1);
	if (!completed) {
		pr_warn_ratelimited("gcn-gx: render copy timed out waiting for final PE finish\n");
		ret = -ETIMEDOUT;
		goto out_unlock;
	}

	invalidate_dcache_range((unsigned long)dst->cpu_addr,
				(unsigned long)dst->cpu_addr + bytes);

out_unlock:
	mutex_unlock(&gx_submit_lock);
	return ret;
}

static int gcn_gx_drm_fill_rgb565(void *dst_allocation, u16 width,
				  u16 height, u16 color)
{
	struct gx_mem1_allocation *dst = dst_allocation;
	u8 r5 = (color >> 11) & 0x1f;
	u8 g6 = (color >> 5) & 0x3f;
	u8 b5 = color & 0x1f;
	u8 r = (r5 << 3) | (r5 >> 2);
	u8 g = (g6 << 2) | (g6 >> 4);
	u8 b = (b5 << 3) | (b5 >> 2);
	u32 finish_count;
	size_t bytes;
	long completed;
	int ret;
	int i;

	if (!dst || !width || !height || (width & 3) || (height & 3))
		return -EINVAL;
	bytes = (size_t)width * height * sizeof(u16);
	if (bytes > dst->size)
		return -E2BIG;
	if (!READ_ONCE(gx_accel_ready))
		return -ENODEV;

	mutex_lock(&gx_submit_lock);
	flush_dcache_range((unsigned long)dst->cpu_addr,
			   (unsigned long)dst->cpu_addr + bytes);

	finish_count = READ_ONCE(gx_pe_finish_count);
	fifo_pos = 0;
	gx_load_libogc_init_preamble();
	gx_setup_display_copy_state();
	gx_setup_vertex_color_state(width, height);
	gx_draw_color_quad(width, height, r, g, b);
	gx_load_bp_reg(0x45000002);
	for (i = 0; i < 32; i++)
		gx_wr8(0);
	gx_set_copy_clear_rgb(0x00, 0x00, 0x00);
	gx_copy_efb_to_rgb565_texture(dst->cpu_addr, width, height, true);
	ret = gx_submit_cmds("render-fill");
	if (ret)
		goto out_unlock;

	/* The unique final token orders copyback; finish IRQs may coalesce. */
	completed = gx_wait_for_pe_finishes(finish_count, 1);
	if (!completed) {
		pr_warn_ratelimited("gcn-gx: render fill timed out waiting for final PE finish\n");
		ret = -ETIMEDOUT;
		goto out_unlock;
	}

	invalidate_dcache_range((unsigned long)dst->cpu_addr,
				(unsigned long)dst->cpu_addr + bytes);

out_unlock:
	mutex_unlock(&gx_submit_lock);
	return ret;
}

static int gcn_gx_drm_fill_rect_rgb565(void *dst_allocation, u16 width,
				       u16 height, u16 x, u16 y,
				       u16 rect_width, u16 rect_height,
				       u16 color)
{
	struct gx_mem1_allocation *dst = dst_allocation;
	u8 r5 = (color >> 11) & 0x1f;
	u8 g6 = (color >> 5) & 0x3f;
	u8 b5 = color & 0x1f;
	u8 r = (r5 << 3) | (r5 >> 2);
	u8 g = (g6 << 2) | (g6 >> 4);
	u8 b = (b5 << 3) | (b5 >> 2);
	u32 finish_count;
	size_t bytes;
	long completed;
	int ret;
	int i;

	if (!dst || !width || !height || (width & 3) || (height & 3) ||
	    !rect_width || !rect_height || x >= width || y >= height ||
	    rect_width > width - x || rect_height > height - y)
		return -EINVAL;
	bytes = (size_t)width * height * sizeof(u16);
	if (bytes > dst->size)
		return -E2BIG;
	if (!READ_ONCE(gx_accel_ready))
		return -ENODEV;

	mutex_lock(&gx_submit_lock);
	flush_dcache_range((unsigned long)dst->cpu_addr,
			   (unsigned long)dst->cpu_addr + bytes);

	finish_count = READ_ONCE(gx_pe_finish_count);
	fifo_pos = 0;
	gx_load_libogc_init_preamble();
	gx_setup_display_copy_state();

	/* Restore the destination into EFB before drawing the bounded overlay. */
	gx_setup_rgb565_texture_state(width, height);
	gx_setup_texture_rgb565(dst->cpu_addr, width, height);
	gx_draw_color_quad(width, height, 0xff, 0xff, 0xff);
	gx_load_bp_reg(0x45000002);
	for (i = 0; i < 32; i++)
		gx_wr8(0);

	gx_setup_vertex_color_state(width, height);
	gx_set_scissor(x, y, rect_width, rect_height);
	gx_draw_color_quad(width, height, r, g, b);
	gx_load_bp_reg(0x45000002);
	for (i = 0; i < 32; i++)
		gx_wr8(0);

	gx_set_copy_clear_rgb(0x00, 0x00, 0x00);
	gx_copy_efb_to_rgb565_texture(dst->cpu_addr, width, height, true);
	ret = gx_submit_cmds("render-fill-rect");
	if (ret)
		goto out_unlock;

	/*
	 * BP finish status is level-triggered, so the three closely spaced
	 * fences above may coalesce into one IRQ. gx_submit_cmds() has already
	 * observed its unique final PE token, which orders after the copyback.
	 */
	completed = gx_wait_for_pe_finishes(finish_count, 1);
	if (!completed) {
		pr_warn_ratelimited("gcn-gx: rectangle fill timed out waiting for final PE finish\n");
		ret = -ETIMEDOUT;
		goto out_unlock;
	}

	invalidate_dcache_range((unsigned long)dst->cpu_addr,
				(unsigned long)dst->cpu_addr + bytes);

out_unlock:
	mutex_unlock(&gx_submit_lock);
	return ret;
}

static int gcn_gx_drm_fill_system_rgb565(void *dst, u16 width, u16 height,
					 u32 dst_layout, u16 x, u16 y,
					 u16 rect_width, u16 rect_height,
					 u16 color)
{
	u8 r5 = (color >> 11) & 0x1f;
	u8 g6 = (color >> 5) & 0x3f;
	u8 b5 = color & 0x1f;
	u8 r = (r5 << 3) | (r5 >> 2);
	u8 g = (g6 << 2) | (g6 >> 4);
	u8 b = (b5 << 3) | (b5 >> 2);
	bool full = !x && !y && rect_width == width && rect_height == height;
	void *render_dst = gx_tex_buf_alt;
	u32 finish_count;
	size_t bytes;
	long completed;
	int ret;
	int i;

	if (!dst || dst_layout != DRM_GCN_GEM_LAYOUT_LINEAR || !width ||
	    !height || (width & 3) || (height & 3) || !rect_width ||
	    !rect_height || x >= width || y >= height ||
	    rect_width > width - x || rect_height > height - y)
		return -EINVAL;
	bytes = (size_t)width * height * sizeof(u16);
	if (bytes > GX_TEX_BUF_SLOT_SIZE)
		return -E2BIG;
	if (!READ_ONCE(gx_accel_ready))
		return -ENODEV;

	mutex_lock(&gx_submit_lock);
	if (full)
		memset(render_dst, 0, bytes);
	else
		gx_copy_rect_to_tiled(dst, width, 0, 0, dst_layout, render_dst,
				      width, height, width, height);
	flush_dcache_range((unsigned long)render_dst,
			   (unsigned long)render_dst + bytes);

	finish_count = READ_ONCE(gx_pe_finish_count);
	fifo_pos = 0;
	gx_load_libogc_init_preamble();
	gx_setup_display_copy_state();

	if (!full) {
		gx_setup_rgb565_texture_state(width, height);
		gx_setup_texture_rgb565(render_dst, width, height);
		gx_draw_color_quad(width, height, 0xff, 0xff, 0xff);
		gx_load_bp_reg(0x45000002);
		for (i = 0; i < 32; i++)
			gx_wr8(0);
	}

	gx_setup_vertex_color_state(width, height);
	gx_set_scissor(x, y, rect_width, rect_height);
	gx_draw_color_quad(width, height, r, g, b);
	gx_load_bp_reg(0x45000002);
	for (i = 0; i < 32; i++)
		gx_wr8(0);

	gx_set_copy_clear_rgb(0x00, 0x00, 0x00);
	gx_copy_efb_to_rgb565_texture(render_dst, width, height, true);
	ret = gx_submit_cmds(full ? "render-fill-system" :
					"render-fill-rect-system");
	if (ret)
		goto out_unlock;

	completed = gx_wait_for_pe_finishes(finish_count, 1);
	if (!completed) {
		pr_warn_ratelimited("gcn-gx: system fill timed out waiting for final PE finish\n");
		ret = -ETIMEDOUT;
		goto out_unlock;
	}

	invalidate_dcache_range((unsigned long)render_dst,
				(unsigned long)render_dst + bytes);
	gx_copy_tiled_to_layout(render_dst, dst, width, height, dst_layout);

out_unlock:
	mutex_unlock(&gx_submit_lock);
	return ret;
}

static int
gcn_gx_drm_draw_triangles_rgb565(void *dst_allocation, u16 width, u16 height,
				 const struct gcn_drm_color_vertex *vertices,
				 u32 triangle_count)
{
	struct gx_mem1_allocation *dst = dst_allocation;
	u32 finish_count;
	size_t bytes;
	long completed;
	unsigned int vertex_count;
	unsigned int i;
	int ret;

	if (!dst || !vertices || !triangle_count ||
	    triangle_count > DRM_GCN_MAX_TRIANGLES || !width || !height ||
	    (width & 3) || (height & 3))
		return -EINVAL;
	vertex_count = triangle_count * 3;
	for (i = 0; i < vertex_count; i++) {
		if (vertices[i].x > width || vertices[i].y > height ||
		    vertices[i].a != 0xff)
			return -EINVAL;
	}

	bytes = (size_t)width * height * sizeof(u16);
	if (bytes > dst->size)
		return -E2BIG;
	if (!READ_ONCE(gx_accel_ready))
		return -ENODEV;

	mutex_lock(&gx_submit_lock);
	flush_dcache_range((unsigned long)dst->cpu_addr,
			   (unsigned long)dst->cpu_addr + bytes);

	finish_count = READ_ONCE(gx_pe_finish_count);
	fifo_pos = 0;
	gx_load_libogc_init_preamble();
	gx_setup_display_copy_state();

	/* Preserve destination pixels around the triangle. */
	gx_setup_rgb565_texture_state(width, height);
	gx_setup_texture_rgb565(dst->cpu_addr, width, height);
	gx_draw_color_quad(width, height, 0xff, 0xff, 0xff);
	gx_load_bp_reg(0x45000002);
	for (i = 0; i < 32; i++)
		gx_wr8(0);

	gx_setup_vertex_color_state(width, height);
	gx_draw_color_triangles(vertices, triangle_count);
	gx_load_bp_reg(0x45000002);
	for (i = 0; i < 32; i++)
		gx_wr8(0);

	gx_set_copy_clear_rgb(0x00, 0x00, 0x00);
	gx_copy_efb_to_rgb565_texture(dst->cpu_addr, width, height, true);
	ret = gx_submit_cmds("render-draw-triangle");
	if (ret)
		goto out_unlock;

	/* The final token orders copyback; closely spaced finish IRQs may merge. */
	completed = gx_wait_for_pe_finishes(finish_count, 1);
	if (!completed) {
		pr_warn_ratelimited("gcn-gx: triangle batch timed out waiting for final PE finish\n");
		ret = -ETIMEDOUT;
		goto out_unlock;
	}

	invalidate_dcache_range((unsigned long)dst->cpu_addr,
				(unsigned long)dst->cpu_addr + bytes);

out_unlock:
	mutex_unlock(&gx_submit_lock);
	return ret;
}

static int
gcn_gx_drm_draw_triangles_state_rgb565(void *dst_allocation, u16 width,
				       u16 height,
				       const struct gcn_drm_color_vertex *vertices,
				       u32 triangle_count,
				       const struct gcn_drm_draw_state *state)
{
	struct gx_mem1_allocation *dst = dst_allocation;
	u32 finish_count;
	size_t bytes;
	long completed;
	unsigned int vertex_count;
	unsigned int i;
	int ret;

	if (!dst || !vertices || !state || !triangle_count ||
	    triangle_count > DRM_GCN_MAX_TRIANGLES || !width || !height ||
	    (width & 3) || (height & 3) || !state->viewport_width ||
	    !state->viewport_height || state->viewport_x >= width ||
	    state->viewport_y >= height ||
	    state->viewport_width > width - state->viewport_x ||
	    state->viewport_height > height - state->viewport_y ||
	    !state->scissor_width || !state->scissor_height ||
	    state->scissor_x >= width || state->scissor_y >= height ||
	    state->scissor_width > width - state->scissor_x ||
	    state->scissor_height > height - state->scissor_y ||
	    state->blend_mode > DRM_GCN_BLEND_SRC_ALPHA ||
	    state->cull_mode > DRM_GCN_CULL_ALL)
		return -EINVAL;
	vertex_count = triangle_count * 3;
	for (i = 0; i < vertex_count; i++) {
		if (vertices[i].x > width || vertices[i].y > height ||
		    (state->blend_mode == DRM_GCN_BLEND_NONE &&
		     vertices[i].a != 0xff))
			return -EINVAL;
	}

	bytes = (size_t)width * height * sizeof(u16);
	if (bytes > dst->size)
		return -E2BIG;
	if (!READ_ONCE(gx_accel_ready))
		return -ENODEV;

	mutex_lock(&gx_submit_lock);
	flush_dcache_range((unsigned long)dst->cpu_addr,
			   (unsigned long)dst->cpu_addr + bytes);

	finish_count = READ_ONCE(gx_pe_finish_count);
	fifo_pos = 0;
	gx_load_libogc_init_preamble();
	gx_setup_display_copy_state();

	/* Restore the destination before applying the requested raster state. */
	gx_setup_rgb565_texture_state(width, height);
	gx_setup_texture_rgb565(dst->cpu_addr, width, height);
	gx_draw_color_quad(width, height, 0xff, 0xff, 0xff);
	gx_load_bp_reg(0x45000002);
	for (i = 0; i < 32; i++)
		gx_wr8(0);

	gx_setup_vertex_color_state_semantic(width, height, state);
	gx_draw_color_triangles(vertices, triangle_count);
	gx_load_bp_reg(0x45000002);
	for (i = 0; i < 32; i++)
		gx_wr8(0);

	gx_set_copy_clear_rgb(0x00, 0x00, 0x00);
	gx_copy_efb_to_rgb565_texture(dst->cpu_addr, width, height, true);
	ret = gx_submit_cmds("render-draw-state");
	if (ret)
		goto out_unlock;

	completed = gx_wait_for_pe_finishes(finish_count, 1);
	if (!completed) {
		pr_warn_ratelimited("gcn-gx: stateful triangle batch timed out waiting for final PE finish\n");
		ret = -ETIMEDOUT;
		goto out_unlock;
	}

	invalidate_dcache_range((unsigned long)dst->cpu_addr,
				(unsigned long)dst->cpu_addr + bytes);

out_unlock:
	mutex_unlock(&gx_submit_lock);
	return ret;
}

static int gcn_gx_drm_draw_depth_rgb565(void *dst_allocation, u16 width,
					u16 height,
					const struct gcn_drm_color_depth_vertex *vertices,
					u32 triangle_count,
					const struct gcn_drm_draw_state *state,
					const struct gcn_drm_depth_state *depth)
{
	struct gx_mem1_allocation *dst = dst_allocation;
	u32 finish_count;
	size_t bytes;
	long completed;
	unsigned int vertex_count;
	unsigned int i;
	int ret;

	if (!dst || !vertices || !state || !depth || !triangle_count ||
	    triangle_count > DRM_GCN_MAX_TRIANGLES || !width || !height ||
	    (width & 3) || (height & 3) || !state->viewport_width ||
	    !state->viewport_height || state->viewport_x >= width ||
	    state->viewport_y >= height ||
	    state->viewport_width > width - state->viewport_x ||
	    state->viewport_height > height - state->viewport_y ||
	    !state->scissor_width || !state->scissor_height ||
	    state->scissor_x >= width || state->scissor_y >= height ||
	    state->scissor_width > width - state->scissor_x ||
	    state->scissor_height > height - state->scissor_y ||
	    state->blend_mode > DRM_GCN_BLEND_SRC_ALPHA ||
	    state->cull_mode > DRM_GCN_CULL_ALL ||
	    depth->compare > DRM_GCN_DEPTH_ALWAYS)
		return -EINVAL;
	vertex_count = triangle_count * 3;
	for (i = 0; i < vertex_count; i++) {
		if (vertices[i].x > width || vertices[i].y > height ||
		    vertices[i].z > DRM_GCN_DEPTH_MAX ||
		    (state->blend_mode == DRM_GCN_BLEND_NONE &&
		     vertices[i].a != 0xff))
			return -EINVAL;
	}

	bytes = (size_t)width * height * sizeof(u16);
	if (bytes > dst->size)
		return -E2BIG;
	if (!READ_ONCE(gx_accel_ready))
		return -ENODEV;

	mutex_lock(&gx_submit_lock);
	flush_dcache_range((unsigned long)dst->cpu_addr,
			   (unsigned long)dst->cpu_addr + bytes);

	finish_count = READ_ONCE(gx_pe_finish_count);
	fifo_pos = 0;
	gx_load_libogc_init_preamble();
	gx_setup_display_copy_state();

	/* Use the established copy engine to initialize EFB depth. */
	gx_set_copy_clear_rgb(0x00, 0x00, 0x00);
	gx_load_bp_reg(0x51000000 | GX_RASTER_DEPTH_MAX);
	gx_copy_efb_to_rgb565_texture(gx_tex_buf, width, height, true);
	for (i = 0; i < 32; i++)
		gx_wr8(0);
	ret = gx_submit_cmds("render-depth-clear");
	if (ret)
		goto out_unlock;
	completed = gx_wait_for_pe_finishes(finish_count, 1);
	if (!completed) {
		pr_warn_ratelimited("gcn-gx: depth clear timed out waiting for PE finish\n");
		ret = -ETIMEDOUT;
		goto out_unlock;
	}

	finish_count = READ_ONCE(gx_pe_finish_count);
	fifo_pos = 0;

	/* Restore destination colour after copy-clear initialized depth. */
	gx_setup_rgb565_texture_state(width, height);
	gx_setup_texture_rgb565(dst->cpu_addr, width, height);
	gx_draw_color_quad(width, height, 0xff, 0xff, 0xff);
	gx_load_bp_reg(0x45000002);
	for (i = 0; i < 32; i++)
		gx_wr8(0);

	gx_setup_vertex_color_depth_state(width, height, state, depth);
	gx_draw_color_depth_triangles(vertices, triangle_count);
	gx_load_bp_reg(0x45000002);
	for (i = 0; i < 32; i++)
		gx_wr8(0);

	gx_set_copy_clear_rgb(0x00, 0x00, 0x00);
	gx_copy_efb_to_rgb565_texture(dst->cpu_addr, width, height, true);
	ret = gx_submit_cmds("render-draw-depth");
	if (ret)
		goto out_unlock;

	completed = gx_wait_for_pe_finishes(finish_count, 1);
	if (!completed) {
		pr_warn_ratelimited("gcn-gx: depth triangle batch timed out waiting for final PE finish\n");
		ret = -ETIMEDOUT;
		goto out_unlock;
	}

	invalidate_dcache_range((unsigned long)dst->cpu_addr,
				(unsigned long)dst->cpu_addr + bytes);

out_unlock:
	mutex_unlock(&gx_submit_lock);
	return ret;
}

static int
gcn_gx_drm_draw_textured_rgb565(void *src_allocation,
				void *dst_allocation,
				u16 src_width, u16 src_height,
				u16 dst_width, u16 dst_height,
				const struct gcn_drm_texture_vertex *vertices,
				u32 triangle_count,
				const struct gcn_drm_draw_state *state)
{
	struct gx_mem1_allocation *src = src_allocation;
	struct gx_mem1_allocation *dst = dst_allocation;
	unsigned int vertex_count;
	u32 finish_count;
	size_t src_bytes;
	size_t dst_bytes;
	long completed;
	unsigned int i;
	int ret;

	if (!src || !dst || src == dst || !vertices || !state ||
	    !triangle_count || triangle_count > DRM_GCN_MAX_TRIANGLES ||
	    !src_width || !src_height || !dst_width || !dst_height ||
	    (src_width & 3) || (src_height & 3) ||
	    (dst_width & 3) || (dst_height & 3) ||
	    !state->viewport_width || !state->viewport_height ||
	    state->viewport_x >= dst_width || state->viewport_y >= dst_height ||
	    state->viewport_width > dst_width - state->viewport_x ||
	    state->viewport_height > dst_height - state->viewport_y ||
	    !state->scissor_width || !state->scissor_height ||
	    state->scissor_x >= dst_width || state->scissor_y >= dst_height ||
	    state->scissor_width > dst_width - state->scissor_x ||
	    state->scissor_height > dst_height - state->scissor_y ||
	    state->blend_mode != DRM_GCN_BLEND_NONE ||
	    state->cull_mode > DRM_GCN_CULL_ALL)
		return -EINVAL;

	vertex_count = triangle_count * 3;
	for (i = 0; i < vertex_count; i++) {
		if (vertices[i].x > dst_width || vertices[i].y > dst_height ||
		    vertices[i].s > src_width || vertices[i].t > src_height)
			return -EINVAL;
	}
	src_bytes = (size_t)src_width * src_height * sizeof(u16);
	dst_bytes = (size_t)dst_width * dst_height * sizeof(u16);
	if (src_bytes > src->size || dst_bytes > dst->size)
		return -E2BIG;
	if (!READ_ONCE(gx_accel_ready))
		return -ENODEV;

	mutex_lock(&gx_submit_lock);
	flush_dcache_range((unsigned long)src->cpu_addr,
			   (unsigned long)src->cpu_addr + src_bytes);
	flush_dcache_range((unsigned long)dst->cpu_addr,
			   (unsigned long)dst->cpu_addr + dst_bytes);

	finish_count = READ_ONCE(gx_pe_finish_count);
	fifo_pos = 0;
	gx_load_libogc_init_preamble();
	gx_setup_display_copy_state();

	/* Restore destination colour before applying the textured primitive. */
	gx_setup_rgb565_texture_state_mode(dst_width, dst_height, false);
	gx_setup_texture_rgb565(dst->cpu_addr, dst_width, dst_height);
	gx_setup_texture_coordinate_scale(dst_width, dst_height, false, false);
	gx_draw_color_quad(dst_width, dst_height, 0xff, 0xff, 0xff);
	gx_load_bp_reg(0x45000002);
	for (i = 0; i < 32; i++)
		gx_wr8(0);

	/* Direct TEX0 uses normalized texel-edge coordinates with fixed phase. */
	gx_setup_rgb565_texture_state_mode(dst_width, dst_height, true);
	gx_setup_texture_rgb565(src->cpu_addr, src_width, src_height);
	gx_setup_texture_coordinate_scale(src_width, src_height, false, false);
	gx_set_viewport(state->viewport_x, state->viewport_y,
			state->viewport_width, state->viewport_height);
	gx_set_scissor(state->scissor_x, state->scissor_y,
		       state->scissor_width, state->scissor_height);
	gx_draw_textured_triangles(vertices, triangle_count,
				   src_width, src_height);
	gx_load_bp_reg(0x45000002);
	for (i = 0; i < 32; i++)
		gx_wr8(0);

	gx_set_copy_clear_rgb(0x00, 0x00, 0x00);
	gx_copy_efb_to_rgb565_texture(dst->cpu_addr, dst_width, dst_height, true);
	ret = gx_submit_cmds("render-draw-textured");
	if (ret)
		goto out_unlock;

	completed = gx_wait_for_pe_finishes(finish_count, 1);
	if (!completed) {
		pr_warn_ratelimited("gcn-gx: textured triangle batch timed out waiting for final PE finish\n");
		ret = -ETIMEDOUT;
		goto out_unlock;
	}

	invalidate_dcache_range((unsigned long)dst->cpu_addr,
				(unsigned long)dst->cpu_addr + dst_bytes);

out_unlock:
	mutex_unlock(&gx_submit_lock);
	return ret;
}

static int
gcn_gx_drm_draw_indexed_rgb565(void *dst_allocation, u16 width, u16 height,
			       const struct gcn_drm_color_vertex *vertices,
			       u32 vertex_count, const u8 *indices,
			       u32 triangle_count,
			       const struct gcn_drm_draw_state *state)
{
	struct gx_mem1_allocation *dst = dst_allocation;
	unsigned int index_count = triangle_count * 3;
	u32 finish_count;
	size_t bytes;
	long completed;
	unsigned int i;
	int ret;

	if (!dst || !vertices || !indices || !state || vertex_count < 3 ||
	    vertex_count > DRM_GCN_MAX_VERTICES || !triangle_count ||
	    triangle_count > DRM_GCN_MAX_TRIANGLES || !width || !height ||
	    (width & 3) || (height & 3) || !state->viewport_width ||
	    !state->viewport_height || state->viewport_x >= width ||
	    state->viewport_y >= height ||
	    state->viewport_width > width - state->viewport_x ||
	    state->viewport_height > height - state->viewport_y ||
	    !state->scissor_width || !state->scissor_height ||
	    state->scissor_x >= width || state->scissor_y >= height ||
	    state->scissor_width > width - state->scissor_x ||
	    state->scissor_height > height - state->scissor_y ||
	    state->blend_mode > DRM_GCN_BLEND_SRC_ALPHA ||
	    state->cull_mode > DRM_GCN_CULL_ALL)
		return -EINVAL;
	for (i = 0; i < vertex_count; i++) {
		if (vertices[i].x > width || vertices[i].y > height ||
		    (state->blend_mode == DRM_GCN_BLEND_NONE &&
		     vertices[i].a != 0xff))
			return -EINVAL;
	}
	for (i = 0; i < index_count; i++) {
		if (indices[i] >= vertex_count)
			return -EINVAL;
	}
	for (i = 0; i < index_count; i += 3) {
		const struct gcn_drm_color_vertex *a = &vertices[indices[i]];
		const struct gcn_drm_color_vertex *b = &vertices[indices[i + 1]];
		const struct gcn_drm_color_vertex *c = &vertices[indices[i + 2]];
		s64 area = (s64)(b->x - a->x) * (c->y - a->y) -
			   (s64)(c->x - a->x) * (b->y - a->y);

		if (!area)
			return -EINVAL;
	}

	bytes = (size_t)width * height * sizeof(u16);
	if (bytes > dst->size)
		return -E2BIG;
	if (!READ_ONCE(gx_accel_ready))
		return -ENODEV;

	mutex_lock(&gx_submit_lock);
	flush_dcache_range((unsigned long)dst->cpu_addr,
			   (unsigned long)dst->cpu_addr + bytes);

	finish_count = READ_ONCE(gx_pe_finish_count);
	fifo_pos = 0;
	gx_load_libogc_init_preamble();
	gx_setup_display_copy_state();

	/* Restore destination colour before applying the indexed primitive. */
	gx_setup_rgb565_texture_state_mode(width, height, false);
	gx_setup_texture_rgb565(dst->cpu_addr, width, height);
	gx_setup_texture_coordinate_scale(width, height, false, false);
	gx_draw_color_quad(width, height, 0xff, 0xff, 0xff);
	gx_load_bp_reg(0x45000002);
	for (i = 0; i < 32; i++)
		gx_wr8(0);

	gx_setup_vertex_color_indexed_state(width, height, state);
	gx_draw_indexed_color_triangles(vertices, vertex_count, indices,
					triangle_count);
	gx_load_bp_reg(0x45000002);
	for (i = 0; i < 32; i++)
		gx_wr8(0);

	gx_set_copy_clear_rgb(0x00, 0x00, 0x00);
	gx_copy_efb_to_rgb565_texture(dst->cpu_addr, width, height, true);
	ret = gx_submit_cmds("render-draw-indexed");
	if (ret)
		goto out_unlock;

	completed = gx_wait_for_pe_finishes(finish_count, 1);
	if (!completed) {
		pr_warn_ratelimited("gcn-gx: indexed triangle batch timed out waiting for final PE finish\n");
		ret = -ETIMEDOUT;
		goto out_unlock;
	}

	invalidate_dcache_range((unsigned long)dst->cpu_addr,
				(unsigned long)dst->cpu_addr + bytes);

out_unlock:
	mutex_unlock(&gx_submit_lock);
	return ret;
}

static int gcn_gx_drm_draw_itex(void *src_allocation, void *dst_allocation,
				u16 src_width, u16 src_height,
				u16 dst_width, u16 dst_height,
		const struct gcn_drm_texture_vertex *vertices, u32 vertex_count,
		const u8 *indices, u32 triangle_count,
		const struct gcn_drm_draw_state *state)
{
	struct gx_mem1_allocation *src = src_allocation;
	struct gx_mem1_allocation *dst = dst_allocation;
	unsigned int index_count = triangle_count * 3;
	u32 finish_count;
	size_t src_bytes;
	size_t dst_bytes;
	long completed;
	unsigned int i;
	int ret;

	if (!src || !dst || src == dst || !vertices || !indices || !state ||
	    vertex_count < 3 || vertex_count > DRM_GCN_MAX_VERTICES ||
	    !triangle_count || triangle_count > DRM_GCN_MAX_TRIANGLES ||
	    !src_width || !src_height || !dst_width || !dst_height ||
	    (src_width & 3) || (src_height & 3) ||
	    (dst_width & 3) || (dst_height & 3) ||
	    !state->viewport_width || !state->viewport_height ||
	    state->viewport_x >= dst_width || state->viewport_y >= dst_height ||
	    state->viewport_width > dst_width - state->viewport_x ||
	    state->viewport_height > dst_height - state->viewport_y ||
	    !state->scissor_width || !state->scissor_height ||
	    state->scissor_x >= dst_width || state->scissor_y >= dst_height ||
	    state->scissor_width > dst_width - state->scissor_x ||
	    state->scissor_height > dst_height - state->scissor_y ||
	    state->blend_mode != DRM_GCN_BLEND_NONE ||
	    state->cull_mode > DRM_GCN_CULL_ALL)
		return -EINVAL;
	for (i = 0; i < vertex_count; i++) {
		if (vertices[i].x > dst_width || vertices[i].y > dst_height ||
		    vertices[i].s > src_width || vertices[i].t > src_height)
			return -EINVAL;
	}
	for (i = 0; i < index_count; i++) {
		if (indices[i] >= vertex_count)
			return -EINVAL;
	}
	for (i = 0; i < index_count; i += 3) {
		const struct gcn_drm_texture_vertex *a = &vertices[indices[i]];
		const struct gcn_drm_texture_vertex *b = &vertices[indices[i + 1]];
		const struct gcn_drm_texture_vertex *c = &vertices[indices[i + 2]];
		s64 area = (s64)(b->x - a->x) * (c->y - a->y) -
			   (s64)(c->x - a->x) * (b->y - a->y);

		if (!area)
			return -EINVAL;
	}

	src_bytes = (size_t)src_width * src_height * sizeof(u16);
	dst_bytes = (size_t)dst_width * dst_height * sizeof(u16);
	if (src_bytes > src->size || dst_bytes > dst->size)
		return -E2BIG;
	if (!READ_ONCE(gx_accel_ready))
		return -ENODEV;

	mutex_lock(&gx_submit_lock);
	flush_dcache_range((unsigned long)src->cpu_addr,
			   (unsigned long)src->cpu_addr + src_bytes);
	flush_dcache_range((unsigned long)dst->cpu_addr,
			   (unsigned long)dst->cpu_addr + dst_bytes);

	finish_count = READ_ONCE(gx_pe_finish_count);
	fifo_pos = 0;
	gx_load_libogc_init_preamble();
	gx_setup_display_copy_state();

	/* Restore destination colour before applying the indexed primitive. */
	gx_setup_rgb565_texture_state_mode(dst_width, dst_height, false);
	gx_setup_texture_rgb565(dst->cpu_addr, dst_width, dst_height);
	gx_setup_texture_coordinate_scale(dst_width, dst_height, false, false);
	gx_draw_color_quad(dst_width, dst_height, 0xff, 0xff, 0xff);
	gx_load_bp_reg(0x45000002);
	for (i = 0; i < 32; i++)
		gx_wr8(0);

	gx_setup_indexed_rgb565_texture_state(dst_width, dst_height, state);
	gx_setup_texture_rgb565(src->cpu_addr, src_width, src_height);
	gx_setup_texture_coordinate_scale(src_width, src_height, false, false);
	gx_draw_itex(vertices, vertex_count, indices, triangle_count,
		     src_width, src_height);
	gx_load_bp_reg(0x45000002);
	for (i = 0; i < 32; i++)
		gx_wr8(0);

	gx_set_copy_clear_rgb(0x00, 0x00, 0x00);
	gx_copy_efb_to_rgb565_texture(dst->cpu_addr, dst_width, dst_height, true);
	ret = gx_submit_cmds("render-draw-indexed-textured");
	if (ret)
		goto out_unlock;

	completed = gx_wait_for_pe_finishes(finish_count, 1);
	if (!completed) {
		pr_warn_ratelimited("gcn-gx: indexed textured batch timed out waiting for final PE finish\n");
		ret = -ETIMEDOUT;
		goto out_unlock;
	}

	invalidate_dcache_range((unsigned long)dst->cpu_addr,
				(unsigned long)dst->cpu_addr + dst_bytes);

out_unlock:
	mutex_unlock(&gx_submit_lock);
	return ret;
}

static int
gcn_gx_drm_draw_itex_depth(void *src_allocation, void *dst_allocation,
			   u16 src_width, u16 src_height,
			   u16 dst_width, u16 dst_height,
			   const struct gcn_drm_itex_depth_vertex *vertices,
			   u32 vertex_count, const u8 *indices,
			   u32 triangle_count,
			   const struct gcn_drm_draw_state *state,
			   const struct gcn_drm_depth_state *depth)
{
	struct gx_mem1_allocation *src = src_allocation;
	struct gx_mem1_allocation *dst = dst_allocation;
	unsigned int index_count = triangle_count * 3;
	u32 finish_count;
	size_t src_bytes;
	size_t dst_bytes;
	long completed;
	unsigned int i;
	int ret;

	if (!src || !dst || src == dst || !vertices || !indices || !state ||
	    !depth || vertex_count < 3 ||
	    vertex_count > DRM_GCN_MAX_VERTICES || !triangle_count ||
	    triangle_count > DRM_GCN_MAX_TRIANGLES || !src_width ||
	    !src_height || !dst_width || !dst_height || (src_width & 3) ||
	    (src_height & 3) || (dst_width & 3) || (dst_height & 3) ||
	    !state->viewport_width || !state->viewport_height ||
	    state->viewport_x >= dst_width || state->viewport_y >= dst_height ||
	    state->viewport_width > dst_width - state->viewport_x ||
	    state->viewport_height > dst_height - state->viewport_y ||
	    !state->scissor_width || !state->scissor_height ||
	    state->scissor_x >= dst_width || state->scissor_y >= dst_height ||
	    state->scissor_width > dst_width - state->scissor_x ||
	    state->scissor_height > dst_height - state->scissor_y ||
	    state->blend_mode != DRM_GCN_BLEND_NONE ||
	    state->cull_mode > DRM_GCN_CULL_ALL ||
	    depth->compare > DRM_GCN_DEPTH_ALWAYS)
		return -EINVAL;
	for (i = 0; i < vertex_count; i++) {
		if (vertices[i].x > dst_width || vertices[i].y > dst_height ||
		    vertices[i].z > DRM_GCN_DEPTH_MAX ||
		    vertices[i].s > src_width || vertices[i].t > src_height)
			return -EINVAL;
	}
	for (i = 0; i < index_count; i++) {
		if (indices[i] >= vertex_count)
			return -EINVAL;
	}
	for (i = 0; i < index_count; i += 3) {
		const struct gcn_drm_itex_depth_vertex *a;
		const struct gcn_drm_itex_depth_vertex *b;
		const struct gcn_drm_itex_depth_vertex *c;
		s64 area;

		a = &vertices[indices[i]];
		b = &vertices[indices[i + 1]];
		c = &vertices[indices[i + 2]];
		area = (s64)(b->x - a->x) * (c->y - a->y) -
		       (s64)(c->x - a->x) * (b->y - a->y);
		if (!area)
			return -EINVAL;
	}

	src_bytes = (size_t)src_width * src_height * sizeof(u16);
	dst_bytes = (size_t)dst_width * dst_height * sizeof(u16);
	if (src_bytes > src->size || dst_bytes > dst->size)
		return -E2BIG;
	if (!READ_ONCE(gx_accel_ready))
		return -ENODEV;

	mutex_lock(&gx_submit_lock);
	flush_dcache_range((unsigned long)src->cpu_addr,
			   (unsigned long)src->cpu_addr + src_bytes);
	flush_dcache_range((unsigned long)dst->cpu_addr,
			   (unsigned long)dst->cpu_addr + dst_bytes);

	finish_count = READ_ONCE(gx_pe_finish_count);
	fifo_pos = 0;
	gx_load_libogc_init_preamble();
	gx_setup_display_copy_state();

	/* Initialize EFB depth to far before restoring destination colour. */
	gx_set_copy_clear_rgb(0x00, 0x00, 0x00);
	gx_load_bp_reg(0x51000000 | GX_RASTER_DEPTH_MAX);
	gx_copy_efb_to_rgb565_texture(gx_tex_buf, dst_width, dst_height, true);
	for (i = 0; i < 32; i++)
		gx_wr8(0);
	ret = gx_submit_cmds("render-itex-depth-clear");
	if (ret)
		goto out_unlock;
	completed = gx_wait_for_pe_finishes(finish_count, 1);
	if (!completed) {
		pr_warn_ratelimited("gcn-gx: indexed textured depth clear timed out\n");
		ret = -ETIMEDOUT;
		goto out_unlock;
	}

	finish_count = READ_ONCE(gx_pe_finish_count);
	fifo_pos = 0;

	gx_setup_rgb565_texture_state_mode(dst_width, dst_height, false);
	gx_setup_texture_rgb565(dst->cpu_addr, dst_width, dst_height);
	gx_setup_texture_coordinate_scale(dst_width, dst_height, false, false);
	gx_draw_color_quad(dst_width, dst_height, 0xff, 0xff, 0xff);
	gx_load_bp_reg(0x45000002);
	for (i = 0; i < 32; i++)
		gx_wr8(0);

	gx_setup_itex_depth_state(dst_width, dst_height, state, depth);
	gx_setup_texture_rgb565(src->cpu_addr, src_width, src_height);
	gx_setup_texture_coordinate_scale(src_width, src_height, false, false);
	gx_draw_itex_depth(vertices, vertex_count, indices, triangle_count,
			   src_width, src_height);
	gx_load_bp_reg(0x45000002);
	for (i = 0; i < 32; i++)
		gx_wr8(0);

	gx_set_copy_clear_rgb(0x00, 0x00, 0x00);
	gx_copy_efb_to_rgb565_texture(dst->cpu_addr, dst_width, dst_height, true);
	ret = gx_submit_cmds("render-draw-itex-depth");
	if (ret)
		goto out_unlock;

	completed = gx_wait_for_pe_finishes(finish_count, 1);
	if (!completed) {
		pr_warn_ratelimited("gcn-gx: indexed textured depth draw timed out\n");
		ret = -ETIMEDOUT;
		goto out_unlock;
	}

	invalidate_dcache_range((unsigned long)dst->cpu_addr,
				(unsigned long)dst->cpu_addr + dst_bytes);

out_unlock:
	mutex_unlock(&gx_submit_lock);
	return ret;
}

static int
gcn_gx_drm_draw_fixed_core(const void *src_addr, size_t src_size,
			   void *dst_addr, size_t dst_size, bool system_dst,
			   u32 src_format, u32 dst_layout,
			   u16 src_width, u16 src_height,
			   u16 dst_width, u16 dst_height,
			   const struct gcn_drm_fixed_vertex *vertices,
			   u32 vertex_count, const u8 *indices,
			   u32 triangle_count, u32 tev_mode,
			   u32 texture_filter,
			   const struct gcn_drm_draw_state *state,
			   const struct gcn_drm_depth_state *depth)
{
	unsigned int index_count = triangle_count * 3;
	bool textured = tev_mode != DRM_GCN_TEV_PASS_COLOR;
	void *render_dst;
	u32 finish_count;
	size_t src_bytes = 0;
	size_t dst_bytes;
	long completed;
	unsigned int i;
	int ret;

	if (!dst_addr || !vertices || !indices || !state || !depth ||
	    vertex_count < 3 || vertex_count > DRM_GCN_MAX_VERTICES ||
	    !triangle_count || triangle_count > DRM_GCN_MAX_TRIANGLES ||
	    !dst_width || !dst_height || (dst_width & 3) || (dst_height & 3) ||
	    tev_mode > DRM_GCN_TEV_MODULATE ||
	    texture_filter > DRM_GCN_TEXTURE_FILTER_LINEAR ||
	    !state->viewport_width || !state->viewport_height ||
	    state->viewport_x >= dst_width || state->viewport_y >= dst_height ||
	    state->viewport_width > dst_width - state->viewport_x ||
	    state->viewport_height > dst_height - state->viewport_y ||
	    !state->scissor_width || !state->scissor_height ||
	    state->scissor_x >= dst_width || state->scissor_y >= dst_height ||
	    state->scissor_width > dst_width - state->scissor_x ||
	    state->scissor_height > dst_height - state->scissor_y ||
	    state->blend_mode > DRM_GCN_BLEND_SRC_ALPHA ||
	    state->cull_mode > DRM_GCN_CULL_ALL ||
	    depth->compare > DRM_GCN_DEPTH_ALWAYS)
		return -EINVAL;
	if (textured) {
		if (!src_addr || src_addr == dst_addr || !src_width || !src_height ||
		    (src_width & 3) || (src_height & 3) ||
		    (src_format != DRM_GCN_GEM_FORMAT_RGB565 &&
		     src_format != DRM_GCN_GEM_FORMAT_RGBA8))
			return -EINVAL;
	} else if (src_addr || src_format || src_width || src_height ||
		   texture_filter != DRM_GCN_TEXTURE_FILTER_NEAREST) {
		return -EINVAL;
	}
	if (system_dst && dst_layout != DRM_GCN_GEM_LAYOUT_LINEAR)
		return -EINVAL;
	if (tev_mode == DRM_GCN_TEV_REPLACE_TEXTURE &&
	    state->blend_mode != DRM_GCN_BLEND_NONE)
		return -EINVAL;

	for (i = 0; i < vertex_count; i++) {
		if (vertices[i].x > dst_width || vertices[i].y > dst_height ||
		    vertices[i].z > DRM_GCN_DEPTH_MAX)
			return -EINVAL;
		if (textured) {
			if (vertices[i].s > src_width ||
			    vertices[i].t > src_height)
				return -EINVAL;
		} else if (vertices[i].s || vertices[i].t) {
			return -EINVAL;
		}
		if (tev_mode != DRM_GCN_TEV_REPLACE_TEXTURE &&
		    state->blend_mode == DRM_GCN_BLEND_NONE &&
		    vertices[i].a != 0xff)
			return -EINVAL;
	}
	for (i = 0; i < index_count; i++) {
		if (indices[i] >= vertex_count)
			return -EINVAL;
	}
	for (i = 0; i < index_count; i += 3) {
		const struct gcn_drm_fixed_vertex *a = &vertices[indices[i]];
		const struct gcn_drm_fixed_vertex *b = &vertices[indices[i + 1]];
		const struct gcn_drm_fixed_vertex *c = &vertices[indices[i + 2]];
		s64 area = (s64)(b->x - a->x) * (c->y - a->y) -
			   (s64)(c->x - a->x) * (b->y - a->y);

		if (!area)
			return -EINVAL;
	}

	if (textured) {
		src_bytes = (size_t)src_width * src_height *
			    (src_format == DRM_GCN_GEM_FORMAT_RGBA8 ? sizeof(u32) :
								       sizeof(u16));
		if (src_bytes > src_size)
			return -E2BIG;
	}
	dst_bytes = (size_t)dst_width * dst_height * sizeof(u16);
	if (dst_bytes > dst_size || (system_dst && dst_bytes > GX_TEX_BUF_SLOT_SIZE))
		return -E2BIG;
	if (!READ_ONCE(gx_accel_ready))
		return -ENODEV;

	mutex_lock(&gx_submit_lock);
	render_dst = dst_addr;
	if (system_dst) {
		render_dst = gx_tex_buf_alt;
		gx_copy_rect_to_tiled(dst_addr, dst_width, 0, 0, dst_layout,
				      render_dst, dst_width, dst_height,
				      dst_width, dst_height);
	}
	if (textured)
		flush_dcache_range((unsigned long)src_addr,
				   (unsigned long)src_addr + src_bytes);
	flush_dcache_range((unsigned long)render_dst,
			   (unsigned long)render_dst + dst_bytes);

	finish_count = READ_ONCE(gx_pe_finish_count);
	fifo_pos = 0;
	gx_load_libogc_init_preamble();
	gx_setup_display_copy_state();

	/* Initialize EFB depth to far before restoring destination colour. */
	gx_set_copy_clear_rgb(0x00, 0x00, 0x00);
	gx_load_bp_reg(0x51000000 | GX_RASTER_DEPTH_MAX);
	gx_copy_efb_to_rgb565_texture(gx_tex_buf, dst_width, dst_height, true);
	for (i = 0; i < 32; i++)
		gx_wr8(0);
	ret = gx_submit_cmds("render-fixed-depth-clear");
	if (ret)
		goto out_unlock;
	completed = gx_wait_for_pe_finishes(finish_count, 1);
	if (!completed) {
		pr_warn_ratelimited("gcn-gx: fixed depth clear timed out\n");
		ret = -ETIMEDOUT;
		goto out_unlock;
	}

	finish_count = READ_ONCE(gx_pe_finish_count);
	fifo_pos = 0;

	gx_setup_rgb565_texture_state_mode(dst_width, dst_height, false);
	gx_setup_texture_rgb565(render_dst, dst_width, dst_height);
	gx_setup_texture_coordinate_scale(dst_width, dst_height, false, false);
	gx_draw_color_quad(dst_width, dst_height, 0xff, 0xff, 0xff);
	gx_load_bp_reg(0x45000002);
	for (i = 0; i < 32; i++)
		gx_wr8(0);

	gx_setup_fixed_state(dst_width, dst_height, tev_mode, state, depth);
	if (textured) {
		gx_setup_texture((void *)src_addr, src_width, src_height,
				 src_format == DRM_GCN_GEM_FORMAT_RGBA8 ?
				 GX_TF_RGBA8 : GX_TF_RGB565, texture_filter);
		gx_setup_texture_coordinate_scale(src_width, src_height,
						  false, false);
	}
	gx_draw_fixed(vertices, vertex_count, indices, triangle_count,
		      src_width, src_height, textured,
		      system_dst ? gx_tex_buf : gx_tex_buf_alt);
	gx_load_bp_reg(0x45000002);
	for (i = 0; i < 32; i++)
		gx_wr8(0);

	gx_set_copy_clear_rgb(0x00, 0x00, 0x00);
	gx_copy_efb_to_rgb565_texture(render_dst, dst_width, dst_height, true);
	ret = gx_submit_cmds("render-draw-fixed");
	if (ret)
		goto out_unlock;

	completed = gx_wait_for_pe_finishes(finish_count, 1);
	if (!completed) {
		pr_warn_ratelimited("gcn-gx: fixed draw timed out\n");
		ret = -ETIMEDOUT;
		goto out_unlock;
	}

	invalidate_dcache_range((unsigned long)render_dst,
				(unsigned long)render_dst + dst_bytes);
	if (system_dst)
		gx_copy_tiled_to_layout(render_dst, dst_addr, dst_width,
					dst_height, dst_layout);

out_unlock:
	mutex_unlock(&gx_submit_lock);
	return ret;
}

static int
gcn_gx_drm_draw_fixed(void *src_allocation, void *dst_allocation,
		      u32 src_format,
		      u16 src_width, u16 src_height,
		      u16 dst_width, u16 dst_height,
		      const struct gcn_drm_fixed_vertex *vertices,
		      u32 vertex_count, const u8 *indices,
		      u32 triangle_count, u32 tev_mode,
		      u32 texture_filter,
		      const struct gcn_drm_draw_state *state,
		      const struct gcn_drm_depth_state *depth)
{
	struct gx_mem1_allocation *src = src_allocation;
	struct gx_mem1_allocation *dst = dst_allocation;

	return gcn_gx_drm_draw_fixed_core(src ? src->cpu_addr : NULL,
			src ? src->size : 0, dst ? dst->cpu_addr : NULL,
			dst ? dst->size : 0, false,
			src_format, DRM_GCN_GEM_LAYOUT_TILED_4X4,
			src_width, src_height,
			dst_width, dst_height, vertices, vertex_count, indices,
			triangle_count, tev_mode, texture_filter, state, depth);
}

static int
gcn_gx_drm_draw_fixed_system(void *src_allocation, void *dst,
			     u32 src_format,
			     u16 src_width, u16 src_height,
			     u16 dst_width, u16 dst_height, u32 dst_layout,
			     const struct gcn_drm_fixed_vertex *vertices,
			     u32 vertex_count, const u8 *indices,
			     u32 triangle_count, u32 tev_mode,
			     u32 texture_filter,
			     const struct gcn_drm_draw_state *state,
			     const struct gcn_drm_depth_state *depth)
{
	struct gx_mem1_allocation *src = src_allocation;
	size_t dst_size = (size_t)dst_width * dst_height * sizeof(u16);

	return gcn_gx_drm_draw_fixed_core(src ? src->cpu_addr : NULL,
			src ? src->size : 0, dst, dst_size, true, src_format,
			dst_layout, src_width, src_height, dst_width, dst_height,
			vertices, vertex_count, indices, triangle_count, tev_mode,
			texture_filter, state, depth);
}

static int
gcn_gx_drm_draw_triangle_rgb565(void *dst_allocation, u16 width, u16 height,
				const struct gcn_drm_color_vertex vertices[3])
{
	return gcn_gx_drm_draw_triangles_rgb565(dst_allocation, width, height,
						vertices, 1);
}

static int gcn_gx_drm_blit_rect_rgb565(void *src_allocation,
				       void *dst_allocation, u16 src_width,
				       u16 src_height, u16 dst_width,
				       u16 dst_height, u16 src_x, u16 src_y,
				       u16 dst_x, u16 dst_y, u16 rect_width,
				       u16 rect_height)
{
	struct gx_mem1_allocation *src = src_allocation;
	struct gx_mem1_allocation *dst = dst_allocation;
	u32 finish_count;
	size_t dst_bytes;
	size_t src_bytes;
	long completed;
	int ret;
	int i;

	if (!src || !dst || !src_width || !src_height ||
	    !dst_width || !dst_height || (src_width & 3) ||
	    (src_height & 3) || (dst_width & 3) || (dst_height & 3) ||
	    !rect_width || !rect_height || src_x >= src_width ||
	    src_y >= src_height || dst_x >= dst_width ||
	    dst_y >= dst_height || rect_width > src_width - src_x ||
	    rect_height > src_height - src_y ||
	    rect_width > dst_width - dst_x ||
	    rect_height > dst_height - dst_y)
		return -EINVAL;
	src_bytes = (size_t)src_width * src_height * sizeof(u16);
	dst_bytes = (size_t)dst_width * dst_height * sizeof(u16);
	if (src_bytes > src->size || dst_bytes > dst->size)
		return -E2BIG;
	if (!READ_ONCE(gx_accel_ready))
		return -ENODEV;

	mutex_lock(&gx_submit_lock);
	if (src != dst)
		flush_dcache_range((unsigned long)src->cpu_addr,
				   (unsigned long)src->cpu_addr + src_bytes);
	flush_dcache_range((unsigned long)dst->cpu_addr,
			   (unsigned long)dst->cpu_addr + dst_bytes);

	finish_count = READ_ONCE(gx_pe_finish_count);
	fifo_pos = 0;
	gx_load_libogc_init_preamble();
	gx_setup_display_copy_state();

	/* Restore the complete destination before overlaying the source region. */
	gx_setup_rgb565_texture_state(dst_width, dst_height);
	gx_setup_texture_rgb565(dst->cpu_addr, dst_width, dst_height);
	gx_draw_color_quad(dst_width, dst_height, 0xff, 0xff, 0xff);
	gx_load_bp_reg(0x45000002);
	for (i = 0; i < 32; i++)
		gx_wr8(0);

	/* Translate screen positions into source coordinates under dst scissor. */
	gx_setup_rgb565_texture_state(dst_width, dst_height);
	gx_load_pos_to_tex_mtx0_offset(src_width, src_height, src_x - dst_x,
				       src_y - dst_y);
	gx_setup_texture_rgb565(src->cpu_addr, src_width, src_height);
	gx_set_scissor(dst_x, dst_y, rect_width, rect_height);
	gx_draw_color_quad(dst_width, dst_height, 0xff, 0xff, 0xff);
	gx_load_bp_reg(0x45000002);
	for (i = 0; i < 32; i++)
		gx_wr8(0);

	gx_set_copy_clear_rgb(0x00, 0x00, 0x00);
	gx_copy_efb_to_rgb565_texture(dst->cpu_addr, dst_width, dst_height, true);
	ret = gx_submit_cmds("render-blit-rect");
	if (ret)
		goto out_unlock;

	/* The unique final token orders after copyback; finish IRQs may coalesce. */
	completed = gx_wait_for_pe_finishes(finish_count, 1);
	if (!completed) {
		pr_warn_ratelimited("gcn-gx: rectangle blit timed out waiting for final PE finish\n");
		ret = -ETIMEDOUT;
		goto out_unlock;
	}

	invalidate_dcache_range((unsigned long)dst->cpu_addr,
				(unsigned long)dst->cpu_addr + dst_bytes);

out_unlock:
	mutex_unlock(&gx_submit_lock);
	return ret;
}

static bool gx_valid_scaled_source(u32 format, u32 layout, bool system_memory)
{
	if (format == DRM_GCN_GEM_FORMAT_RGB565)
		return layout == DRM_GCN_GEM_LAYOUT_TILED_4X4 ||
		       layout == DRM_GCN_GEM_LAYOUT_LINEAR;

	return format == DRM_GCN_GEM_FORMAT_XRGB8888 && system_memory &&
	       layout == DRM_GCN_GEM_LAYOUT_LINEAR;
}

static int
gcn_gx_drm_blit_full_system_xrgb8888(const u32 *src, u16 src_width,
				     u16 src_x, u16 src_y, u16 *dst,
				     u16 width, u16 height, u32 dst_layout)
{
	u16 *capture = gx_tex_buf;
	u16 *texture = gx_tex_buf_alt;
	u32 finish_count;
	size_t bytes = (size_t)width * height * sizeof(*texture);
	long completed;
	int ret;
	int i;

	if (bytes > GX_TEX_BUF_SLOT_SIZE || width > 640 || height > 528)
		return -E2BIG;

	mutex_lock(&gx_submit_lock);
	gx_copy_xrgb8888_rect_to_tiled(src, src_width, src_x, src_y,
				       texture, width, height, width, height);
	flush_dcache_range((unsigned long)texture,
			   (unsigned long)texture + bytes);

	/* Prevent dirty CPU lines from overwriting the subsequent GX copy. */
	memset(capture, 0, bytes);
	flush_dcache_range((unsigned long)capture,
			   (unsigned long)capture + bytes);

	finish_count = READ_ONCE(gx_pe_finish_count);
	fifo_pos = 0;
	gx_load_libogc_init_preamble();
	gx_setup_display_copy_state();
	gx_setup_rgb565_texture_state(width, height);
	gx_setup_texture_rgb565(texture, width, height);
	gx_draw_color_quad(width, height, 0xff, 0xff, 0xff);
	gx_load_bp_reg(0x45000002);
	for (i = 0; i < 32; i++)
		gx_wr8(0);
	gx_set_copy_clear_rgb(0x00, 0x00, 0x00);
	gx_copy_efb_to_rgb565_texture(capture, width, height, true);
	ret = gx_submit_cmds("render-blit-xrgb8888-full");
	if (ret)
		goto out_unlock;

	completed = gx_wait_for_pe_finishes(finish_count, 1);
	if (!completed) {
		pr_warn_ratelimited("gcn-gx: full XRGB8888 blit timed out waiting for final PE finish\n");
		ret = -ETIMEDOUT;
		goto out_unlock;
	}

	invalidate_dcache_range((unsigned long)capture,
				(unsigned long)capture + bytes);
	gx_copy_tiled_to_layout(capture, dst, width, height, dst_layout);

out_unlock:
	mutex_unlock(&gx_submit_lock);
	return ret;
}

static int gcn_gx_drm_blit_scaled_rgb565_core(const void *src_addr,
					      void *dst_addr, u16 src_width,
					      u16 src_height, u16 dst_width,
					      u16 dst_height, u16 src_x,
					      u16 src_y, u16 src_rect_width,
					      u16 src_rect_height, u16 dst_x,
					      u16 dst_y, u16 dst_rect_width,
					      u16 dst_rect_height,
					      u32 src_format, u32 src_layout,
					      u32 dst_layout,
					      bool system_memory)
{
	struct gx_scale_efb_sample samples[] = {
		{ .x = 0, .y = 0 },
		{ .x = 166, .y = 113 },
		{ .x = 206, .y = 31 },
		{ .x = 319, .y = 119 },
	};
	u32 *efb_snapshot = NULL;
	u16 *prior_snapshot = NULL;
	void *crop = gx_tex_buf_alt;
	void *horizontal = gx_tex_buf;
	void *final_texture = horizontal;
	u32 final_texture_format = GX_TF_RGB565;
	u16 crop_height;
	u16 crop_copy_width;
	u16 crop_width;
	u16 horizontal_copy_width;
	u16 horizontal_width;
	size_t crop_bytes;
	size_t dst_bytes;
	size_t horizontal_bytes;
	size_t src_bytes;
	bool final_submitted = false;
	bool focused;
	bool native_trace;
	bool offset_focused;
	bool offset_trace;
	bool system_focused;
	bool system_trace;
	bool split_horizontal;
	bool split_vertical;
	bool trace;
	u32 crop_hash = 0;
	u32 final_command_hash = 0;
	u32 final_unpadded_bytes = 0;
	u32 final_padded_bytes = 0;
	u32 final_delayed_hash = 0;
	u32 final_hash = 0;
	u32 horizontal_command_hash = 0;
	u32 horizontal_command_bytes = 0;
	u32 horizontal_delayed_hash = 0;
	u32 horizontal_hash = 0;
	u32 source_hash = 0;
	u32 trace_sequence = 0;
	int ret;
	int i;

	if (!src_addr || !dst_addr || !src_width || !src_height || !dst_width ||
	    !dst_height || (src_width & 3) || (src_height & 3) ||
	    (dst_width & 3) || (dst_height & 3) || !src_rect_width ||
	    !src_rect_height || !dst_rect_width || !dst_rect_height ||
	    src_x >= src_width || src_y >= src_height || dst_x >= dst_width ||
	    dst_y >= dst_height || src_rect_width > src_width - src_x ||
	    src_rect_height > src_height - src_y ||
	    dst_rect_width > dst_width - dst_x ||
	    dst_rect_height > dst_height - dst_y)
		return -EINVAL;
	if (!gx_valid_scaled_source(src_format, src_layout, system_memory) ||
	    (dst_layout != DRM_GCN_GEM_LAYOUT_TILED_4X4 &&
	     dst_layout != DRM_GCN_GEM_LAYOUT_LINEAR) ||
	    (!system_memory &&
	     (src_layout != DRM_GCN_GEM_LAYOUT_TILED_4X4 ||
	      dst_layout != DRM_GCN_GEM_LAYOUT_TILED_4X4)))
		return -EINVAL;
	src_bytes = (size_t)src_width * src_height *
		    (src_format == DRM_GCN_GEM_FORMAT_XRGB8888 ? sizeof(u32) :
								     sizeof(u16));
	dst_bytes = (size_t)dst_width * dst_height * sizeof(u16);
	if (!READ_ONCE(gx_accel_ready))
		return -ENODEV;
	if (src_rect_width > 640 || dst_rect_width > 640 ||
	    src_rect_height > 528)
		return -E2BIG;
	if (system_memory &&
	    src_format == DRM_GCN_GEM_FORMAT_XRGB8888 &&
	    src_rect_width == dst_rect_width &&
	    src_rect_height == dst_rect_height && !dst_x && !dst_y &&
	    dst_rect_width == dst_width && dst_rect_height == dst_height)
		return gcn_gx_drm_blit_full_system_xrgb8888(src_addr, src_width,
					src_x, src_y, dst_addr, dst_width,
					dst_height, dst_layout);

	/* Exact binary slopes require power-of-two private texture extents. */
	crop_width = gx_power_of_two_extent(src_rect_width);
	crop_height = gx_power_of_two_extent(src_rect_height);
	horizontal_width = gx_power_of_two_extent(dst_rect_width);
	crop_copy_width = crop_width > 640 ? src_rect_width : crop_width;
	horizontal_copy_width = horizontal_width > 640 ?
				dst_rect_width : horizontal_width;
	crop_bytes = (size_t)crop_width * crop_height * sizeof(u16);
	horizontal_bytes = (size_t)horizontal_width * crop_height *
			   sizeof(u16);
	if (crop_width > 1024 || horizontal_width > 1024 ||
	    crop_height > 528 || crop_bytes > GX_TEX_BUF_SLOT_SIZE ||
	    horizontal_bytes > GX_TEX_BUF_SLOT_SIZE ||
	    (system_memory && dst_bytes > GX_TEX_BUF_SLOT_SIZE))
		return -E2BIG;

	mutex_lock(&gx_submit_lock);
	focused = !system_memory && !src_x && !src_y &&
		!dst_x && !dst_y && src_width == 640 && src_height == 240 &&
		dst_width == 320 && dst_height == 120 &&
		src_rect_width == 640 && src_rect_height == 240 &&
		dst_rect_width == 320 && dst_rect_height == 120;
	system_focused = system_memory &&
		src_format == DRM_GCN_GEM_FORMAT_RGB565 &&
		src_layout == DRM_GCN_GEM_LAYOUT_LINEAR &&
		dst_layout == DRM_GCN_GEM_LAYOUT_LINEAR &&
		!src_x && !src_y && !dst_x && !dst_y &&
		src_width == 320 && src_height == 240 &&
		dst_width == 640 && dst_height == 480 &&
		src_rect_width == 320 && src_rect_height == 240 &&
		dst_rect_width == 640 && dst_rect_height == 480;
	offset_focused = !system_memory &&
		src_addr != dst_addr &&
		src_format == DRM_GCN_GEM_FORMAT_RGB565 &&
		src_layout == DRM_GCN_GEM_LAYOUT_TILED_4X4 &&
		dst_layout == DRM_GCN_GEM_LAYOUT_TILED_4X4 &&
		src_width == 256 && src_height == 256 &&
		dst_width == 256 && dst_height == 256 &&
		!src_x && src_y == 43 && !dst_x && dst_y == 97 &&
		src_rect_width == 255 && src_rect_height == 79 &&
		dst_rect_width == 256 && dst_rect_height == 79;
	native_trace = READ_ONCE(gx_scale_native_trace) && system_memory &&
		src_format == DRM_GCN_GEM_FORMAT_XRGB8888 &&
		src_layout == DRM_GCN_GEM_LAYOUT_LINEAR &&
		dst_layout == DRM_GCN_GEM_LAYOUT_LINEAR &&
		src_width == 640 && src_height == 480 &&
		dst_width == 640 && dst_height == 480 &&
		src_x == dst_x && src_y == dst_y &&
		(src_x == 0 || src_x == 320) && (src_y == 0 || src_y == 240) &&
		src_rect_width == 320 && src_rect_height == 240 &&
		dst_rect_width == 320 && dst_rect_height == 240;
	offset_trace = READ_ONCE(gx_scale_offset_trace) && offset_focused;
	system_trace = READ_ONCE(gx_scale_system_trace) && system_focused;
	trace = READ_ONCE(gx_scale_trace) && focused;
	split_horizontal = (focused && gx_scale_split_reduce) ||
		(trace && gx_scale_gpu_split) ||
		(system_focused && gx_scale_system_split) ||
		(native_trace && gx_scale_native_horizontal_split);
	split_vertical = (focused && gx_scale_split_reduce) ||
		(trace && gx_scale_texture_half_rows) ||
		(offset_focused && gx_scale_offset_split) ||
		(native_trace && gx_scale_native_split);
	if (trace || system_trace || offset_trace || native_trace)
		trace_sequence = ++gx_scale_trace_sequence;
	if (trace && (gx_scale_direct_color || gx_scale_clear_color) &&
	    (!gx_scale_cpu_source || !gx_scale_cpu_uniform ||
	     (gx_scale_direct_color && gx_scale_clear_color))) {
		ret = -EINVAL;
		goto out_unlock;
	}

	if (trace && gx_scale_direct_color &&
	    (!gx_scale_band_height || gx_scale_band_height > dst_height ||
	     ((gx_scale_row_fence || gx_scale_single_quad) &&
	      gx_scale_band_height != 1))) {
		ret = -EINVAL;
		goto out_unlock;
	}

	if (trace && gx_scale_gpu_split &&
	    (!gx_scale_texture_half_rows || gx_scale_cpu_source || gx_scale_cpu_alt ||
	     gx_scale_cpu_rgba8 || gx_scale_cpu_uniform || gx_scale_cpu_publish)) {
		ret = -EINVAL;
		goto out_unlock;
	}

	if (trace && gx_scale_texture_half_rows &&
	    ((!gx_scale_cpu_source && !gx_scale_gpu_split) || gx_scale_cpu_uniform ||
	     gx_scale_direct_color ||
	     gx_scale_clear_color || gx_scale_half_rows || gx_scale_row_triangles ||
	     gx_scale_reverse_rows || gx_scale_columns || gx_scale_split ||
	     gx_scale_split_second || gx_scale_single_quad || gx_scale_row_fence ||
	     gx_scale_band_height != 1 || gx_scale_pad_bytes ||
	     gx_scale_degenerate_quads || gx_scale_degenerate_first)) {
		ret = -EINVAL;
		goto out_unlock;
	}

	if (trace && gx_scale_half_rows &&
	    (!gx_scale_direct_color || gx_scale_row_triangles ||
	     gx_scale_reverse_rows || gx_scale_columns || gx_scale_split ||
	     gx_scale_split_second || gx_scale_single_quad || gx_scale_row_fence ||
	     gx_scale_band_height != 1 || gx_scale_pad_bytes ||
	     gx_scale_degenerate_quads || gx_scale_degenerate_first)) {
		ret = -EINVAL;
		goto out_unlock;
	}

	if (trace && gx_scale_row_triangles &&
	    (!gx_scale_direct_color || gx_scale_reverse_rows || gx_scale_columns ||
	     gx_scale_split || gx_scale_split_second || gx_scale_single_quad ||
	     gx_scale_row_fence || gx_scale_band_height != 1 || gx_scale_pad_bytes ||
	     gx_scale_degenerate_quads || gx_scale_degenerate_first)) {
		ret = -EINVAL;
		goto out_unlock;
	}

	if (trace && gx_scale_reverse_rows &&
	    (!gx_scale_direct_color || gx_scale_columns || gx_scale_split ||
	     gx_scale_split_second || gx_scale_single_quad || gx_scale_row_fence ||
	     gx_scale_band_height != 1 || gx_scale_pad_bytes ||
	     gx_scale_degenerate_quads || gx_scale_degenerate_first)) {
		ret = -EINVAL;
		goto out_unlock;
	}

	if (trace && gx_scale_columns &&
	    (!gx_scale_direct_color || gx_scale_split || gx_scale_split_second ||
	     gx_scale_single_quad || gx_scale_row_fence ||
	     gx_scale_band_height != 1 || gx_scale_pad_bytes ||
	     gx_scale_degenerate_quads || gx_scale_degenerate_first)) {
		ret = -EINVAL;
		goto out_unlock;
	}

	if (trace && gx_scale_degenerate_first && !gx_scale_degenerate_quads) {
		ret = -EINVAL;
		goto out_unlock;
	}

	if (trace && gx_scale_degenerate_quads &&
	    (!gx_scale_direct_color || !gx_scale_split_second ||
	     gx_scale_pad_bytes || gx_scale_degenerate_quads > 117)) {
		ret = -EINVAL;
		goto out_unlock;
	}

	if (trace && gx_scale_pad_bytes &&
	    (!gx_scale_direct_color || gx_scale_row_fence ||
	     gx_scale_pad_bytes > GX_FIFO_SIZE - 256)) {
		ret = -EINVAL;
		goto out_unlock;
	}

	if (trace && gx_scale_split_second &&
	    (!gx_scale_split || gx_scale_split_second <= gx_scale_split ||
	     gx_scale_split_second >= dst_height)) {
		ret = -EINVAL;
		goto out_unlock;
	}

	if (trace && gx_scale_split &&
	    (!gx_scale_direct_color || gx_scale_split >= dst_height ||
	     gx_scale_band_height != 1 || gx_scale_single_quad ||
	     gx_scale_row_fence)) {
		ret = -EINVAL;
		goto out_unlock;
	}

	if (trace && gx_scale_row_fence &&
	    (!gx_scale_direct_color || gx_scale_single_quad)) {
		ret = -EINVAL;
		goto out_unlock;
	}

	if ((trace && gx_scale_efb_full) || system_trace || offset_trace || native_trace) {
		efb_snapshot = kvmalloc_array((size_t)dst_width * dst_height,
					      sizeof(*efb_snapshot),
					      GFP_KERNEL);
		if (!efb_snapshot) {
			ret = -ENOMEM;
			goto out_unlock;
		}
	}

	if (offset_trace || native_trace) {
		prior_snapshot = kvmalloc(dst_bytes, GFP_KERNEL);
		if (!prior_snapshot) {
			ret = -ENOMEM;
			goto out_unlock;
		}
		memcpy(prior_snapshot, dst_addr, dst_bytes);
	}

	if (!system_memory) {
		if (src_addr != dst_addr)
			flush_dcache_range((unsigned long)src_addr,
					   (unsigned long)src_addr + src_bytes);
		flush_dcache_range((unsigned long)dst_addr,
				   (unsigned long)dst_addr + dst_bytes);
		if (trace)
			source_hash = gx_hash_tiled_region(src_addr, src_width,
							   src_width, src_height);
	}
	flush_dcache_range((unsigned long)crop,
			   (unsigned long)crop + crop_bytes);
	flush_dcache_range((unsigned long)horizontal,
			   (unsigned long)horizontal + horizontal_bytes);

	/*
	 * Snapshot the requested source rectangle into a private texture first.
	 * GX_CLAMP applies to the bound texture, not an interior source rectangle;
	 * isolating the rectangle prevents nearest sampling from escaping its
	 * edges and preserves same-object overlap semantics.
	 */
	if (system_memory) {
		if (src_format == DRM_GCN_GEM_FORMAT_XRGB8888)
			gx_copy_xrgb8888_rect_to_tiled(src_addr, src_width,
						       src_x, src_y, crop, crop_width,
						       crop_height, src_rect_width,
						       src_rect_height);
		else
			gx_copy_rect_to_tiled(src_addr, src_width, src_x, src_y,
					      src_layout, crop, crop_width,
					      crop_height, src_rect_width,
					      src_rect_height);
		flush_dcache_range((unsigned long)crop,
				   (unsigned long)crop + crop_bytes);
	} else {
		fifo_pos = 0;
		gx_load_libogc_init_preamble();
		gx_setup_display_copy_state();
		gx_setup_rgb565_texture_state(src_width, src_height);
		gx_load_pos_to_tex_mtx0_offset(src_width, src_height,
					       src_x, src_y);
		gx_setup_texture_rgb565((void *)src_addr, src_width, src_height);
		gx_set_scissor(0, 0, src_rect_width, src_rect_height);
		gx_draw_color_quad(src_width, src_height, 0xff, 0xff, 0xff);
		gx_load_bp_reg(0x45000002);
		ret = gx_submit_and_wait_finish("render-blit-scaled-crop-draw");
		if (ret)
			goto out_unlock;
		if (offset_trace) {
			ret = gx_snapshot_scale_colors(efb_snapshot, 255, 79);
			if (ret)
				goto out_unlock;
		}

		fifo_pos = 0;
		gx_load_libogc_init_preamble();
		gx_setup_display_copy_state();
		gx_set_copy_clear_rgb(0x00, 0x00, 0x00);
		gx_copy_efb_rect_to_rgb565_texture_stride(crop, 0, 0,
							  crop_copy_width,
							  crop_height,
							  crop_width, true);
		ret = gx_submit_and_wait_finish("render-blit-scaled-crop-copy");
		if (ret)
			goto out_unlock;
		if (trace) {
			invalidate_dcache_range((unsigned long)crop,
						(unsigned long)crop + crop_bytes);
			crop_hash = gx_hash_tiled_region(crop, crop_width,
							 crop_copy_width,
							 src_rect_height);
		}
	}
	if (offset_trace) {
		invalidate_dcache_range((unsigned long)crop,
					(unsigned long)crop + crop_bytes);
		gx_compare_offset_scale(src_addr, prior_snapshot, crop, efb_snapshot,
					trace_sequence, 0);
	}
	if (native_trace)
		gx_compare_native_scale(src_addr, prior_snapshot, crop, NULL,
					src_x, src_y, trace_sequence, 0);
	if (system_trace)
		gx_compare_system_scale(src_addr, crop, crop_width, src_width,
					src_height, NULL, trace_sequence, "crop");
	/* Expand or reduce source columns exactly into a private intermediate. */
	fifo_pos = 0;
	gx_load_libogc_init_preamble();
	gx_setup_display_copy_state();
	gx_setup_rgb565_texture_state_mode(horizontal_width, crop_height, true);
	gx_setup_texture_rgb565(crop, crop_width, crop_height);
	gx_setup_texture_coordinate_scale(crop_width, crop_height,
					  false, false);
	gx_set_scissor(0, 0, dst_rect_width, src_rect_height);
	if (split_horizontal) {
		u32 vertex_bytes = 160 * gx_nearest_run_count(src_rect_width,
							     dst_rect_width);

		/* Two 80-byte quads per run, header and submission reserve. */
		if (vertex_bytes > GX_FIFO_SIZE - 256 - 3 ||
		    fifo_pos > GX_FIFO_SIZE - 256 - 3 - vertex_bytes) {
			ret = -E2BIG;
			goto out_unlock;
		}
	}
	gx_draw_nearest_horizontal_runs(src_rect_width, crop_width,
					src_rect_height, crop_height,
					dst_rect_width, split_horizontal);
	gx_load_bp_reg(0x45000002);
	if (trace) {
		horizontal_command_hash = gx_hash_pending_commands();
		horizontal_command_bytes = fifo_pos;
	}
	if (system_trace)
		pr_info("gcn-gx: system-horizontal seq=%u split=%u quads=%u bytes=%u hash=%08x\n",
			trace_sequence, split_horizontal,
			(split_horizontal ? 2 : 1) *
			gx_nearest_run_count(src_rect_width, dst_rect_width),
			fifo_pos, gx_hash_pending_commands());
	if (native_trace)
		pr_info("gcn-gx: native-horizontal seq=%u split=%u quads=%u bytes=%u hash=%08x\n",
			trace_sequence, split_horizontal,
			(split_horizontal ? 2 : 1) *
			gx_nearest_run_count(src_rect_width, dst_rect_width),
			fifo_pos, gx_hash_pending_commands());
	ret = gx_submit_and_wait_finish("render-blit-scaled-horizontal-draw");
	if (ret)
		goto out_unlock;
	if (system_trace || offset_trace || native_trace) {
		ret = gx_snapshot_scale_colors(efb_snapshot, dst_rect_width,
					       src_rect_height);
		if (ret)
			goto out_unlock;
	}

	fifo_pos = 0;
	gx_load_libogc_init_preamble();
	gx_setup_display_copy_state();
	gx_set_copy_clear_rgb(0x00, 0x00, 0x00);
	gx_copy_efb_rect_to_rgb565_texture_stride(horizontal, 0, 0,
						  horizontal_copy_width,
						  crop_height,
						  horizontal_width, true);
	ret = gx_submit_and_wait_finish("render-blit-scaled-horizontal-copy");
	if (ret)
		goto out_unlock;
	if (trace) {
		invalidate_dcache_range((unsigned long)horizontal,
					(unsigned long)horizontal + horizontal_bytes);
		horizontal_hash = gx_hash_tiled_region(horizontal,
						       horizontal_width,
						       dst_rect_width,
						       src_rect_height);
		usleep_range(5000, 6000);
		invalidate_dcache_range((unsigned long)horizontal,
					(unsigned long)horizontal + horizontal_bytes);
		horizontal_delayed_hash = gx_hash_tiled_region(horizontal,
							       horizontal_width,
							       dst_rect_width,
							       src_rect_height);
	}

	if (system_trace) {
		invalidate_dcache_range((unsigned long)horizontal,
					(unsigned long)horizontal + horizontal_bytes);
		gx_compare_system_scale(src_addr, horizontal, horizontal_width,
					dst_width, src_height, efb_snapshot, trace_sequence,
					"horizontal");
	}

	if (offset_trace) {
		invalidate_dcache_range((unsigned long)horizontal,
					(unsigned long)horizontal + horizontal_bytes);
		gx_compare_offset_scale(src_addr, prior_snapshot, horizontal,
					efb_snapshot, trace_sequence, 1);
	}

	if (native_trace) {
		invalidate_dcache_range((unsigned long)horizontal,
					(unsigned long)horizontal + horizontal_bytes);
		gx_compare_native_scale(src_addr, prior_snapshot, horizontal,
					efb_snapshot, src_x, src_y, trace_sequence, 1);
	}

	if (trace && gx_scale_cpu_publish) {
		u32 published_hash;

		gx_cpu_republish_texture(horizontal, horizontal_bytes);
		published_hash = gx_hash_tiled_region(horizontal, horizontal_width,
						      dst_rect_width, src_rect_height);
		pr_info("gcn-gx: scale-cpu-publish seq=%u before=%08x after=%08x bytes=%zu\n",
			trace_sequence, horizontal_delayed_hash, published_hash,
			horizontal_bytes);
		if (published_hash != horizontal_delayed_hash) {
			ret = -EIO;
			goto out_unlock;
		}
	}

	if (trace && gx_scale_cpu_source) {
		const u16 *source = src_addr;
		u16 *pixels;
		size_t fixture_bytes = horizontal_bytes;
		u32 expected_hash = 2166136261U;
		u32 published_hash;
		u16 hash_width = dst_rect_width;
		u16 hash_height = src_rect_height;
		u16 x;
		u16 y;

		/* The prior horizontal draw no longer needs the crop workspace. */
		if (gx_scale_cpu_alt)
			final_texture = crop;
		pixels = final_texture;
		if (gx_scale_cpu_rgba8) {
			final_texture_format = GX_TF_RGBA8;
			fixture_bytes *= 2;
		}
		if (fixture_bytes > GX_TEX_BUF_SLOT_SIZE) {
			ret = -E2BIG;
			goto out_unlock;
		}
		if (gx_scale_cpu_uniform) {
			for (y = 0; y < src_height; y++) {
				for (x = 0; x < src_width; x++) {
					if (source[gx_tiled_rgb565_index(x, y, src_width)] !=
					    source[0]) {
						ret = -EINVAL;
						goto out_unlock;
					}
				}
			}
			hash_width = horizontal_width;
			hash_height = crop_height;
		}
		/* Synthetic input control, not an accelerated scaler repair. */
		memset(final_texture, 0, fixture_bytes);
		for (y = 0; y < hash_height; y++) {
			for (x = 0; x < hash_width; x++) {
				size_t pixel = gx_tiled_rgb565_index(x, y, horizontal_width);
				u16 value = gx_scale_cpu_uniform ? source[0] :
					source[gx_tiled_rgb565_index(2 * x + 1, y, src_width)];

				if (gx_scale_cpu_rgba8)
					gx_scale_store_rgba8(final_texture, x, y,
							     horizontal_width, value);
				else
					pixels[pixel] = value;
				expected_hash ^= value;
				expected_hash *= 16777619U;
			}
		}
		flush_dcache_range((unsigned long)final_texture,
				   (unsigned long)final_texture + fixture_bytes);
		invalidate_dcache_range((unsigned long)final_texture,
					(unsigned long)final_texture + fixture_bytes);
		if (gx_scale_cpu_rgba8)
			published_hash = gx_hash_scale_rgba8(final_texture,
							     horizontal_width,
							     hash_width, hash_height);
		else
			published_hash = gx_hash_tiled_region(final_texture,
							      horizontal_width,
							      hash_width, hash_height);
		pr_info("gcn-gx: scale-cpu-source seq=%u producer=%08x expected=%08x published=%08x base=%08x format=%u bytes=%zu hash_extent=%ux%u\n",
			trace_sequence, horizontal_delayed_hash, expected_hash,
			published_hash, (u32)virt_to_phys(final_texture),
			final_texture_format, fixture_bytes, hash_width, hash_height);
		if (published_hash != expected_hash) {
			ret = -EIO;
			goto out_unlock;
		}
	}

	/* The crop workspace is free once the horizontal snapshot is complete. */
	if (system_memory) {
		gx_copy_rect_to_tiled(dst_addr, dst_width, 0, 0, dst_layout,
				      crop, dst_width, dst_height, dst_width,
				      dst_height);
		flush_dcache_range((unsigned long)crop,
				   (unsigned long)crop + dst_bytes);
	}
	if (native_trace)
		gx_compare_native_prior(prior_snapshot, crop, NULL, trace_sequence);
	fifo_pos = 0;
	gx_load_libogc_init_preamble();
	gx_setup_display_copy_state();
	if (dst_x || dst_y || dst_rect_width != dst_width ||
	    dst_rect_height != dst_height) {
		gx_setup_rgb565_texture_state(dst_width, dst_height);
		gx_setup_texture_rgb565(system_memory ? crop : dst_addr,
					dst_width, dst_height);
		gx_draw_color_quad(dst_width, dst_height, 0xff, 0xff, 0xff);
		for (i = 0; i < 32; i++)
			gx_wr8(0);
		if (native_trace && gx_scale_native_preserve_fence) {
			gx_load_bp_reg(0x45000002);
			ret = gx_submit_and_wait_finish("render-native-preserve");
			if (ret)
				goto out_unlock;
			ret = gx_snapshot_scale_colors(efb_snapshot, dst_width,
						       dst_height);
			if (ret)
				goto out_unlock;
			gx_compare_native_prior(prior_snapshot, crop, efb_snapshot,
						trace_sequence);
			/* Continue with the preserved GPU state in a fresh FIFO. */
			fifo_pos = 0;
		}
	}

	if (trace && (gx_scale_direct_color || gx_scale_clear_color)) {
		u16 color = ((const u16 *)src_addr)[0];
		u8 r = (color >> 11) & 31;
		u8 g = (color >> 5) & 63;
		u8 b = color & 31;
		u16 y;

		r = (r << 3) | (r >> 2);
		g = (g << 2) | (g >> 4);
		b = (b << 3) | (b >> 2);
		if (gx_scale_clear_color) {
			/* Copy old pixels only to trigger clear, then fence the clear. */
			gx_setup_display_copy_state();
			gx_set_copy_clear_rgb(r, g, b);
			gx_copy_efb_to_rgb565_texture(dst_addr, dst_width,
						      dst_height, true);
		} else {
			gx_setup_vertex_color_state(dst_width, dst_height);
			gx_set_scissor(0, 0, dst_width, dst_height);
			if (gx_scale_row_fence) {
				ret = gx_submit_fenced_color_rows(dst_width, dst_height,
								  r, g, b, &final_command_hash);
				if (ret)
					goto out_unlock;
				final_submitted = true;
				pr_info("gcn-gx: scale-row-fence seq=%u completed=%u commands=%08x\n",
					trace_sequence, dst_height, final_command_hash);
			} else if (gx_scale_split) {
				u16 end = gx_scale_split_second ?: dst_height;
				u16 quads = (gx_scale_split_second ? 3 : 2) +
					    gx_scale_degenerate_quads;

				/* Four twelve-byte vertices per quad, plus header. */
				if (fifo_pos > GX_FIFO_SIZE - 256 - 3 - 48 * quads) {
					ret = -E2BIG;
					goto out_unlock;
				}
				gx_wr8(0x80);
				gx_wr16be(4 * quads);
				if (gx_scale_degenerate_first) {
					for (y = 0; y < gx_scale_degenerate_quads; y++)
						gx_emit_color_rect(0, 0, 0, 0, r, g, b);
				}
				gx_emit_color_rect(0, 0, dst_width, gx_scale_split,
						   r, g, b);
				gx_emit_color_rect(0, gx_scale_split, dst_width,
						   end, r, g, b);
				if (gx_scale_split_second)
					gx_emit_color_rect(0, end, dst_width, dst_height,
							   r, g, b);
				if (!gx_scale_degenerate_first) {
					for (y = 0; y < gx_scale_degenerate_quads; y++)
						gx_emit_color_rect(0, 0, 0, 0, r, g, b);
				}
			} else if (gx_scale_half_rows) {
				u16 middle = dst_width / 2;

				if (fifo_pos > GX_FIFO_SIZE - 256 - 3 - 96 * dst_height) {
					ret = -E2BIG;
					goto out_unlock;
				}
				gx_wr8(0x80);
				gx_wr16be(8 * dst_height);
				for (y = 0; y < dst_height; y++) {
					gx_emit_color_rect(0, y, middle, y + 1, r, g, b);
					gx_emit_color_rect(middle, y, dst_width, y + 1,
							   r, g, b);
				}
			} else if (gx_scale_row_triangles) {
				gx_wr8(0x90); /* GX_TRIANGLES | vtxfmt 0 */
				gx_wr16be(6 * dst_height);
				for (y = 0; y < dst_height; y++)
					gx_emit_color_row_triangles(dst_width, y, r, g, b);
			} else if (gx_scale_reverse_rows) {
				gx_wr8(0x80);
				gx_wr16be(4 * dst_height);
				for (y = dst_height; y > 0; y--)
					gx_emit_color_rect(0, y - 1, dst_width, y, r, g, b);
			} else if (gx_scale_columns) {
				gx_wr8(0x80);
				gx_wr16be(4 * dst_height);
				for (y = 0; y < dst_height; y++)
					gx_emit_color_rect(y * dst_width / dst_height, 0,
							   (y + 1) * dst_width / dst_height,
							   dst_height, r, g, b);
			} else if (gx_scale_single_quad) {
				gx_draw_color_rect(0, 0, dst_width, dst_height, r, g, b);
			} else {
				u16 band = gx_scale_band_height;
				u16 quads = DIV_ROUND_UP(dst_height, band);

				gx_wr8(0x80);
				gx_wr16be(4 * quads);
				for (y = 0; y < dst_height; y += band)
					gx_emit_color_rect(0, y, dst_width,
							   min_t(u16, y + band, dst_height),
							   r, g, b);

			}
		}
	} else {
		gx_setup_rgb565_texture_state_mode(dst_width, dst_height, true);
		gx_setup_texture(final_texture, horizontal_width, crop_height,
				 final_texture_format, DRM_GCN_TEXTURE_FILTER_NEAREST);
		gx_setup_texture_coordinate_scale(horizontal_width, crop_height,
						  false, false);
		gx_set_scissor(dst_x, dst_y, dst_rect_width, dst_rect_height);
		if (split_vertical) {
			u32 vertex_bytes = 160 * gx_nearest_run_count(src_rect_height,
								     dst_rect_height);

			/* Two 80-byte quads per run, header and submission reserve. */
			if (vertex_bytes > GX_FIFO_SIZE - 256 - 3 ||
			    fifo_pos > GX_FIFO_SIZE - 256 - 3 - vertex_bytes) {
				ret = -E2BIG;
				goto out_unlock;
			}
		}
		gx_draw_nearest_vertical_runs(dst_x, dst_y, dst_rect_width,
					      horizontal_width, src_rect_height,
					      crop_height,
					      dst_rect_height,
					      split_vertical);
	}
	if (!final_submitted) {
		if (trace && (gx_scale_direct_color || split_vertical)) {
			/* Reserve the finish BP, submit token and alignment trailer. */
			if (fifo_pos > GX_FIFO_SIZE - 256 ||
			    gx_scale_pad_bytes > GX_FIFO_SIZE - 256 - fifo_pos) {
				ret = -E2BIG;
				goto out_unlock;
			}
			final_unpadded_bytes = fifo_pos + 5;
			for (i = 0; i < gx_scale_pad_bytes; i++)
				gx_wr8(0); /* GX_NOP */
			final_padded_bytes = fifo_pos + 5;
		}
		if (!(trace && gx_scale_clear_color))
			gx_load_bp_reg(0x45000002);
		if (trace)
			final_command_hash = gx_hash_pending_commands();
		if (native_trace)
			pr_info("gcn-gx: native-final seq=%u split=%u quads=%u bytes=%u hash=%08x\n",
				trace_sequence, split_vertical,
				(split_vertical ? 2 : 1) *
				gx_nearest_run_count(src_rect_height, dst_rect_height),
				fifo_pos, gx_hash_pending_commands());
		if (offset_trace)
			pr_info("gcn-gx: offset-final seq=%u split=%u quads=%u bytes=%u hash=%08x\n",
				trace_sequence, split_vertical,
				(split_vertical ? 2 : 1) *
				gx_nearest_run_count(src_rect_height, dst_rect_height),
				fifo_pos, gx_hash_pending_commands());
		ret = gx_submit_and_wait_finish(trace && gx_scale_clear_color ?
						"render-blit-scaled-final-clear" :
						"render-blit-scaled-final-draw");
		if (ret)
			goto out_unlock;
	}
	if (efb_snapshot) {
		ret = gx_snapshot_scale_colors(efb_snapshot, dst_width, dst_height);
		if (ret)
			goto out_unlock;
	}
	if (trace && gx_scale_efb_peek) {
		ret = gx_peek_scale_colors(samples, ARRAY_SIZE(samples));
		if (ret)
			goto out_unlock;
	}


	fifo_pos = 0;
	gx_load_libogc_init_preamble();
	gx_setup_display_copy_state();
	gx_set_copy_clear_rgb(0x00, 0x00, 0x00);
	gx_copy_efb_to_rgb565_texture(system_memory ? crop : dst_addr,
				      dst_width, dst_height, true);
	ret = gx_submit_and_wait_finish("render-blit-scaled-final-copy");
	if (ret)
		goto out_unlock;

	if (system_memory) {
		invalidate_dcache_range((unsigned long)crop,
					(unsigned long)crop + dst_bytes);
		if (system_trace)
			gx_compare_system_scale(src_addr, crop, dst_width, dst_width,
						dst_height, efb_snapshot, trace_sequence,
						"final");
		if (native_trace)
			gx_compare_native_scale(src_addr, prior_snapshot, crop,
						efb_snapshot, src_x, src_y,
						trace_sequence, 2);
		gx_copy_tiled_to_layout(crop, dst_addr, dst_width, dst_height, dst_layout);
	} else {
		invalidate_dcache_range((unsigned long)dst_addr,
					(unsigned long)dst_addr + dst_bytes);
		if (offset_trace)
			gx_compare_offset_scale(src_addr, prior_snapshot, dst_addr,
						efb_snapshot, trace_sequence, 2);
		if (trace) {
			final_hash = gx_hash_tiled_region(dst_addr, dst_width,
							  dst_width, dst_height);
			usleep_range(5000, 6000);
			invalidate_dcache_range((unsigned long)dst_addr,
						(unsigned long)dst_addr + dst_bytes);
			final_delayed_hash = gx_hash_tiled_region(dst_addr,
								  dst_width, dst_width,
								  dst_height);
			if (efb_snapshot)
				gx_compare_scale_colors(efb_snapshot, dst_addr, src_addr,
							trace_sequence);
			if (gx_scale_efb_peek) {
				const u16 *output = dst_addr;
				const u16 *source = src_addr;

				for (i = 0; i < ARRAY_SIZE(samples); i++) {
					u16 x = samples[i].x;
					u16 y = samples[i].y;
					u32 argb = samples[i].argb;
					u16 rgb565 = ((argb >> 8) & 0xf800) |
						     ((argb >> 5) & 0x07e0) |
						     ((argb >> 3) & 0x001f);
					u16 copied = output[gx_tiled_rgb565_index(x, y,
										      dst_width)];
					u16 expected = source[gx_tiled_rgb565_index(2 * x + 1,
											2 * y + 1,
											src_width)];

					pr_info("gcn-gx: scale-efb seq=%u x=%u y=%u argb=%08x rgb565=%04x copied=%04x expected=%04x\n",
						trace_sequence, x, y, argb, rgb565,
						copied, expected);
				}
			}
			if (final_unpadded_bytes)
				pr_info("gcn-gx: scale-bytes seq=%u unpadded=%u padded=%u nops=%u\n",
					trace_sequence, final_unpadded_bytes,
					final_padded_bytes, gx_scale_pad_bytes);
			if (gx_scale_degenerate_quads)
				pr_info("gcn-gx: scale-degenerate seq=%u real=3 extra=%u total=%u first=%u\n",
					trace_sequence, gx_scale_degenerate_quads,
					3 + gx_scale_degenerate_quads,
					gx_scale_degenerate_first);
			if (gx_scale_split)
				pr_info("gcn-gx: scale-split seq=%u row=%u second=%u quads=%u\n",
					trace_sequence, gx_scale_split,
					gx_scale_split_second,
					(gx_scale_split_second ? 3 : 2) +
					gx_scale_degenerate_quads);
			if (split_horizontal)
				pr_info("gcn-gx: scale-gpu-split seq=%u horizontal_quads=%u split_y=%u bytes=%u cpu_source=%u\n",
					trace_sequence, 2 * dst_rect_width,
					src_rect_height / 2, horizontal_command_bytes,
					gx_scale_cpu_source);
			if (split_vertical && !gx_scale_direct_color && !gx_scale_clear_color)
				pr_info("gcn-gx: scale-texture-half seq=%u rows=%u split=%u quads=%u\n",
					trace_sequence, dst_height, dst_width / 2,
					2 * dst_height);
			if (gx_scale_half_rows)
				pr_info("gcn-gx: scale-half-rows seq=%u rows=%u split=%u quads=%u\n",
					trace_sequence, dst_height, dst_width / 2,
					2 * dst_height);
			if (gx_scale_row_triangles)
				pr_info("gcn-gx: scale-triangles seq=%u rows=%u triangles=%u vertices=%u\n",
					trace_sequence, dst_height, 2 * dst_height,
					6 * dst_height);
			if (gx_scale_columns)
				pr_info("gcn-gx: scale-columns seq=%u width=%u height=%u quads=%u\n",
					trace_sequence, dst_width, dst_height, dst_height);
			if (gx_scale_direct_color && !gx_scale_row_fence &&
			    !gx_scale_single_quad && !gx_scale_split && !gx_scale_columns &&
			    !gx_scale_row_triangles && !gx_scale_half_rows)
				pr_info("gcn-gx: scale-bands seq=%u height=%u quads=%u reverse=%u\n",
					trace_sequence, gx_scale_band_height,
					DIV_ROUND_UP(dst_height, gx_scale_band_height),
					gx_scale_reverse_rows);
			pr_info("gcn-gx: scale-commands seq=%u horizontal=%08x final=%08x\n",
				trace_sequence, horizontal_command_hash,
				final_command_hash);
			pr_info("gcn-gx: scale-trace seq=%u src=%08x crop=%08x horizontal=%08x horizontal_delayed=%08x final=%08x final_delayed=%08x\n",
				trace_sequence, source_hash, crop_hash,
				horizontal_hash, horizontal_delayed_hash,
				final_hash, final_delayed_hash);
		}
	}

out_unlock:
	kvfree(prior_snapshot);
	kvfree(efb_snapshot);
	mutex_unlock(&gx_submit_lock);
	return ret;
}

static int gcn_gx_drm_blit_scaled_rgb565(void *src_allocation,
					 void *dst_allocation, u16 src_width,
					 u16 src_height, u16 dst_width,
					 u16 dst_height, u16 src_x, u16 src_y,
					 u16 src_rect_width,
					 u16 src_rect_height, u16 dst_x,
					 u16 dst_y, u16 dst_rect_width,
					 u16 dst_rect_height)
{
	struct gx_mem1_allocation *src = src_allocation;
	struct gx_mem1_allocation *dst = dst_allocation;
	size_t dst_bytes;
	size_t src_bytes;

	if (!src || !dst)
		return -EINVAL;
	if (src_rect_width == dst_rect_width &&
	    src_rect_height == dst_rect_height)
		return gcn_gx_drm_blit_rect_rgb565(src_allocation,
				dst_allocation, src_width, src_height,
				dst_width, dst_height, src_x, src_y,
				dst_x, dst_y, src_rect_width,
				src_rect_height);

	src_bytes = (size_t)src_width * src_height * sizeof(u16);
	dst_bytes = (size_t)dst_width * dst_height * sizeof(u16);
	if (src_bytes > src->size || dst_bytes > dst->size)
		return -E2BIG;

	return gcn_gx_drm_blit_scaled_rgb565_core(src->cpu_addr,
			dst->cpu_addr, src_width, src_height, dst_width,
			dst_height, src_x, src_y, src_rect_width,
			src_rect_height, dst_x, dst_y, dst_rect_width,
			dst_rect_height, DRM_GCN_GEM_FORMAT_RGB565,
			DRM_GCN_GEM_LAYOUT_TILED_4X4,
			DRM_GCN_GEM_LAYOUT_TILED_4X4, false);
}

static int gcn_gx_drm_blit_scaled_system_rgb565(const void *src, void *dst,
						u16 src_width, u16 src_height,
						u16 dst_width, u16 dst_height,
						u32 src_layout, u32 dst_layout,
						u16 src_x, u16 src_y,
						u16 src_rect_width,
						u16 src_rect_height,
						u16 dst_x, u16 dst_y,
						u16 dst_rect_width,
						u16 dst_rect_height)
{
	return gcn_gx_drm_blit_scaled_rgb565_core(src, dst, src_width,
			src_height, dst_width, dst_height, src_x, src_y,
			src_rect_width, src_rect_height, dst_x, dst_y,
			dst_rect_width, dst_rect_height,
			DRM_GCN_GEM_FORMAT_RGB565, src_layout, dst_layout, true);
}

static int
gcn_gx_drm_blit_scaled_system_xrgb8888(const void *src, void *dst,
				       u16 src_width, u16 src_height,
				       u16 dst_width, u16 dst_height,
				       u32 dst_layout, u16 src_x, u16 src_y,
				       u16 src_rect_width, u16 src_rect_height,
				       u16 dst_x, u16 dst_y,
				       u16 dst_rect_width, u16 dst_rect_height)
{
	return gcn_gx_drm_blit_scaled_rgb565_core(src, dst, src_width,
			src_height, dst_width, dst_height, src_x, src_y,
			src_rect_width, src_rect_height, dst_x, dst_y,
			dst_rect_width, dst_rect_height,
			DRM_GCN_GEM_FORMAT_XRGB8888,
			DRM_GCN_GEM_LAYOUT_LINEAR, dst_layout, true);
}

static const struct gcn_drm_accel_ops gcn_gx_drm_accel_ops = {
	.name = "gcn-gx",
	.owner = THIS_MODULE,
	.blit_rgb565 = gcn_gx_drm_blit_rgb565,
	.blit_xrgb8888 = gcn_gx_drm_blit_xrgb8888,
	.mem1_info = gcn_gx_drm_mem1_info,
	.mem1_alloc = gcn_gx_drm_mem1_alloc,
	.mem1_free = gcn_gx_drm_mem1_free,
	.mem1_mmap = gcn_gx_drm_mem1_mmap,
	.submit_rgb565 = gcn_gx_drm_submit_rgb565,
	.fill_rgb565 = gcn_gx_drm_fill_rgb565,
	.fill_rect_rgb565 = gcn_gx_drm_fill_rect_rgb565,
	.draw_triangle_rgb565 = gcn_gx_drm_draw_triangle_rgb565,
	.draw_triangles_rgb565 = gcn_gx_drm_draw_triangles_rgb565,
	.draw_triangles_state_rgb565 =
		gcn_gx_drm_draw_triangles_state_rgb565,
	.blit_rect_rgb565 = gcn_gx_drm_blit_rect_rgb565,
	.blit_scaled_rgb565 = gcn_gx_drm_blit_scaled_rgb565,
	.blit_scaled_system_rgb565 = gcn_gx_drm_blit_scaled_system_rgb565,
	.blit_scaled_system_xrgb8888 = gcn_gx_drm_blit_scaled_system_xrgb8888,
	.draw_triangles_depth_rgb565 =
		gcn_gx_drm_draw_depth_rgb565,
	.draw_textured_triangles_rgb565 =
		gcn_gx_drm_draw_textured_rgb565,
	.draw_indexed_triangles_rgb565 =
		gcn_gx_drm_draw_indexed_rgb565,
	.draw_indexed_textured_triangles_rgb565 =
		gcn_gx_drm_draw_itex,
	.draw_itex_depth_rgb565 =
		gcn_gx_drm_draw_itex_depth,
	.draw_fixed_rgb565 = gcn_gx_drm_draw_fixed,
	.draw_fixed_system_rgb565 = gcn_gx_drm_draw_fixed_system,
	.fill_system_rgb565 = gcn_gx_drm_fill_system_rgb565,
};
#endif

/* ------------------------------------------------------------------ */
/* Module lifecycle                                                     */
/* ------------------------------------------------------------------ */

#define GX_MEM_ALIGNMENT	32
#define GX_RENDER_POOL_SIZE	(512 * 1024)

#if IS_ENABLED(CONFIG_DRM_GCN_GX)
static void gx_mem1_free_workspaces_locked(void)
{
	int i;

	if (!gx_mem1_allocator.initialized || gx_mem1_user_allocations)
		return;

	for (i = ARRAY_SIZE(gx_tex_workspace) - 1; i >= 0; i--)
		memset(&gx_tex_workspace[i], 0,
		       sizeof(gx_tex_workspace[i]));

	gcn_gx_mem1_allocator_fini(&gx_mem1_allocator);
	gx_mem1_total_bytes = 0;
	gx_mem1_used_bytes = 0;
	gx_mem1_free_bytes = 0;
}

static void gx_mem1_free_workspaces(void)
{
	mutex_lock(&gx_mem1_lock);
	gx_mem1_shutdown = true;
	gx_mem1_free_workspaces_locked();
	mutex_unlock(&gx_mem1_lock);
}

static int gx_mem1_alloc_workspaces(const struct resource *texture_mem,
				    const struct resource *render_mem)
{
	u64 render_size = resource_size(render_mem);
	int i;
	int ret;

	if (resource_size(texture_mem) != 2 * GX_TEX_BUF_SLOT_SIZE ||
	    render_size > UINT_MAX)
		return -EINVAL;

	mutex_lock(&gx_mem1_lock);
	if (gx_mem1_allocator.initialized) {
		mutex_unlock(&gx_mem1_lock);
		return -EBUSY;
	}

	memset(gx_tex_workspace, 0, sizeof(gx_tex_workspace));
	gx_mem1_shutdown = false;
	gx_mem1_user_allocations = 0;
	ret = gcn_gx_mem1_allocator_init(&gx_mem1_allocator,
					 render_mem->start, render_size,
					 GX_MEM_ALIGNMENT);
	if (ret) {
		mutex_unlock(&gx_mem1_lock);
		return ret;
	}

	for (i = 0; i < ARRAY_SIZE(gx_tex_workspace); i++) {
		struct gx_mem1_buffer *buffer = &gx_tex_workspace[i];

		buffer->phys_addr = texture_mem->start +
					    i * GX_TEX_BUF_SLOT_SIZE;
		buffer->cpu_addr = (void *)__va(buffer->phys_addr);
		buffer->size = GX_TEX_BUF_SLOT_SIZE;
		buffer->layout = GX_MEM1_LAYOUT_TILED_RGB565;
		buffer->access = GX_MEM1_ACCESS_IDLE;
	}

	gx_mem1_total_bytes = render_size;
	gx_mem1_used_bytes = 0;
	gx_mem1_free_bytes = render_size;
	gx_tex_phys = gx_tex_workspace[0].phys_addr;
	gx_tex_buf = gx_tex_workspace[0].cpu_addr;
	gx_tex_buf_alt = gx_tex_workspace[1].cpu_addr;
	mutex_unlock(&gx_mem1_lock);
	return 0;
}
#endif

static bool gx_resources_overlap(const struct resource *a,
				 const struct resource *b)
{
	return a->start <= b->end && b->start <= a->end;
}

static int gx_get_reserved_region(struct platform_device *pdev,
				  const char *name, size_t min_size,
				  struct resource *res)
{
	struct device_node *memory;
	struct device_node *region;
	struct resource mem1;
	int index;
	int ret;

	index = of_property_match_string(pdev->dev.of_node,
					 "memory-region-names", name);
	if (index < 0)
		return index;

	region = of_parse_phandle(pdev->dev.of_node, "memory-region", index);
	if (!region)
		return -ENODEV;
	if (of_property_read_bool(region, "no-map")) {
		of_node_put(region);
		return -EINVAL;
	}
	of_node_put(region);

	ret = of_reserved_mem_region_to_resource_byname(pdev->dev.of_node,
							 name, res);
	if (ret)
		return ret;
	if (resource_size(res) < min_size ||
	    !IS_ALIGNED(res->start, GX_MEM_ALIGNMENT) ||
	    !IS_ALIGNED(resource_size(res), GX_MEM_ALIGNMENT))
		return -EINVAL;

	memory = of_find_node_by_type(NULL, "memory");
	if (!memory)
		return -ENODEV;
	ret = of_address_to_resource(memory, 0, &mem1);
	of_node_put(memory);
	if (ret)
		return ret;
	if (!resource_contains(&mem1, res))
		return -ERANGE;

	return 0;
}

static int gcn_gx_init(struct platform_device *pdev)
{
	struct resource fifo_mem;
	struct resource texture_mem;
#if IS_ENABLED(CONFIG_DRM_GCN_GX)
	struct resource render_mem;
#endif
	int irq;
	int ret;
	if (!strcmp(gx_renderer, "generated")) {
		gx_use_reference = false;
		gx_use_direct = false;
	} else if (!strcmp(gx_renderer, "reference")) {
		gx_use_reference = true;
		gx_use_direct = false;
	} else if (!strcmp(gx_renderer, "direct")) {
		gx_use_reference = false;
		gx_use_direct = true;
	} else {
		pr_err("gcn-gx: invalid renderer '%s'\n", gx_renderer);
		return -EINVAL;
	}
	if (!strcmp(gx_texture_source, "console"))
		gx_use_pattern = gx_use_probe = false;
	else if (!strcmp(gx_texture_source, "pattern")) {
		gx_use_pattern = true;
		gx_use_probe = false;
	} else if (!strcmp(gx_texture_source, "probe")) {
		gx_use_pattern = false;
		gx_use_probe = true;
	} else {
		pr_err("gcn-gx: invalid texture source '%s'\n",
		       gx_texture_source);
		return -EINVAL;
	}
	if (gx_texel_bias_eighths < -8 || gx_texel_bias_eighths > 8) {
		pr_err("gcn-gx: invalid texel_bias_eighths %d (expected -8..8)\n",
		       gx_texel_bias_eighths);
		return -EINVAL;
	}
	if (!strcmp(gx_texcoord_space, "normalized"))
		gx_use_texel_space = false;
	else if (!strcmp(gx_texcoord_space, "texel"))
		gx_use_texel_space = true;
	else {
		pr_err("gcn-gx: invalid texcoord_space '%s'\n",
		       gx_texcoord_space);
		return -EINVAL;
	}
	if (!strcmp(gx_texcoord_source, "position"))
		gx_use_direct_texcoord = false;
	else if (!strcmp(gx_texcoord_source, "direct"))
		gx_use_direct_texcoord = true;
	else {
		pr_err("gcn-gx: invalid texcoord_source '%s'\n",
		       gx_texcoord_source);
		return -EINVAL;
	}
	if (!strcmp(gx_direct_primitive, "quad"))
		gx_use_direct_triangle = false;
	else if (!strcmp(gx_direct_primitive, "triangle"))
		gx_use_direct_triangle = true;
	else {
		pr_err("gcn-gx: invalid direct_primitive '%s'\n",
		       gx_direct_primitive);
		return -EINVAL;
	}
	if (gx_use_direct_triangle && !gx_use_direct_texcoord) {
		pr_err("gcn-gx: direct_primitive=triangle requires texcoord_source=direct\n");
		return -EINVAL;
	}
	if (!strcmp(gx_direct_pattern_name, "grid"))
		gx_use_direct_vstripes = false;
	else if (!strcmp(gx_direct_pattern_name, "vstripes"))
		gx_use_direct_vstripes = true;
	else {
		pr_err("gcn-gx: invalid direct_pattern '%s'\n",
		       gx_direct_pattern_name);
		return -EINVAL;
	}
	if (!strcmp(gx_texcoord_mapping, "affine"))
		gx_use_constant_texcoord = false;
	else if (!strcmp(gx_texcoord_mapping, "constant"))
		gx_use_constant_texcoord = true;
	else {
		pr_err("gcn-gx: invalid texcoord_mapping '%s'\n",
		       gx_texcoord_mapping);
		return -EINVAL;
	}
	if (gx_use_constant_texcoord && !gx_use_direct_texcoord) {
		pr_err("gcn-gx: texcoord_mapping=constant requires texcoord_source=direct\n");
		return -EINVAL;
	}

	cp_regs = (u16 __iomem *)devm_platform_ioremap_resource_byname(pdev,
								 "cp");
	if (IS_ERR(cp_regs))
		return PTR_ERR(cp_regs);
	pe_regs = (u16 __iomem *)devm_platform_ioremap_resource_byname(pdev,
								 "pe");
	if (IS_ERR(pe_regs))
		return PTR_ERR(pe_regs);

	pi_regmap = syscon_regmap_lookup_by_phandle(pdev->dev.of_node,
						    "nintendo,processor-interface");
	if (IS_ERR(pi_regmap))
		return PTR_ERR(pi_regmap);

	ret = gx_get_reserved_region(pdev, "fifo", GX_FIFO_SIZE, &fifo_mem);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "invalid FIFO memory region\n");
	ret = gx_get_reserved_region(pdev, "texture",
				     2 * GX_TEX_BUF_SLOT_SIZE, &texture_mem);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "invalid texture memory region\n");
	if (gx_resources_overlap(&fifo_mem, &texture_mem))
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "FIFO and texture memory overlap\n");
#if IS_ENABLED(CONFIG_DRM_GCN_GX)
	ret = gx_get_reserved_region(pdev, "render", GX_RENDER_POOL_SIZE,
				     &render_mem);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "invalid render memory region\n");
	if (resource_size(&render_mem) != GX_RENDER_POOL_SIZE)
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "render memory region has unexpected size\n");
	if (gx_resources_overlap(&fifo_mem, &render_mem) ||
	    gx_resources_overlap(&texture_mem, &render_mem))
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "GX reserved memory regions overlap\n");
#endif

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;
	gx_pe_finish_irq = irq;

	/*
	 * Mini leaves PI_FIFO_WPTR=0x00000000.  VI hardware generates wgPipe
	 * bursts during retrace; those bursts DMA to PI_FIFO_WPTR.  With
	 * WPTR=0 they overwrite the exception vectors at physical 0.
	 *
	 * Order: resolve resources -> set WPTR -> THEN any successful-probe
	 * printk.  A single printk can trigger a VI retrace via console output.
	 */
	gx_fifo_buf_raw = NULL;
	gx_fifo_phys = fifo_mem.start;
	gx_fifo_buf = (void *)__va(gx_fifo_phys);
	memset(gx_fifo_buf, 0, GX_FIFO_SIZE);
	flush_dcache_range((unsigned long)gx_fifo_buf,
			   (unsigned long)gx_fifo_buf + GX_FIFO_SIZE);

	/* Clear stale PE events before the finish IRQ line is unmasked. */
	pe_write(PE_REG_INTR_STATUS, PE_TOKEN_BIT | PE_FINISH_BIT);

	/* Redirect wgPipe DMA bursts to our zeroed buffer (was addr 0 in mini) */
	pi_write(PI_REG_FIFO_WPTR, gx_fifo_phys);

	ret = gx_fifo_init();
	if (ret)
		goto err_hw;

	ret = request_irq(gx_pe_finish_irq, gx_pe_finish_handler, 0,
			  "gcn-gx-pe-finish", &gx_pe_finish_irq);
	if (ret) {
		pr_err("gcn-gx: failed to request PE finish IRQ %u: %d\n",
		       gx_pe_finish_irq, ret);
		goto err_hw;
	}
	gx_pe_finish_irq_requested = true;
	pe_write(PE_REG_INTR_STATUS,
		 PE_TOKEN_ENABLE | PE_FINISH_ENABLE |
		 PE_TOKEN_BIT | PE_FINISH_BIT);

	/*
	 * Texture tile buffer: must be in MEM1.  The GX texture unit is
	 * GameCube-era hardware; it cannot address MEM2 (0x10000000+).
	 * kmalloc returns MEM2 on Wii Linux because MEM1 and MEM2 are
	 * coalesced into one logical range.  Use the DTS-reserved region.
	 */
	gx_tex_raw = NULL;
#if IS_ENABLED(CONFIG_DRM_GCN_GX)
	ret = gx_mem1_alloc_workspaces(&texture_mem, &render_mem);
	if (ret)
		goto err_hw;
#else
	gx_tex_phys = texture_mem.start;
	gx_tex_buf = (void *)__va(gx_tex_phys);
	gx_tex_buf_alt = (void *)__va(gx_tex_phys + GX_TEX_BUF_SLOT_SIZE);
#endif
	memset(gx_tex_buf, 0, GX_TEX_BUF_SIZE);
	memset(gx_tex_buf_alt, 0, GX_TEX_BUF_SIZE);
#if IS_ENABLED(CONFIG_FB_GAMECUBE_GX)
	INIT_WORK(&gx_frame_work.work, gx_frame_workfn);
#endif
	gx_frame_work.vfb = NULL;
	gx_frame_work.format = GX_VFB_RGB565;
	gx_frame_work.source_generation = 0;
	gx_live_texture_frame = 0;
	gx_frame_ready_xfb = 0;
	gx_frame_ready_vfb = NULL;
	gx_frame_work_busy = false;
	gx_frame_boot_deferred = false;
	gx_frame_publish_xfb = false;
	gx_frame_hold = false;
	gx_rgb888_tile_total_ns = 0;
	gx_rgb888_tile_max_ns = 0;
	gx_rgb888_flush_total_ns = 0;
	gx_rgb888_flush_max_ns = 0;
	gx_rgb888_timing_frames = 0;
	gx_offscreen_texture_ready = false;
	gx_offscreen_copies = 0;
	gx_offscreen_replays = 0;
	gx_offscreen_changed_words = 0;
	gx_xfb_snapshot_size = 0;
	gx_vfb_snapshot_size = 0;
	gx_xfb_snapshot_width = 0;
	gx_xfb_snapshot_height = 0;
	gx_xfb_snapshot_phys = 0;
	gx_diag_phase = GX_DIAG_SEED;
	gx_diag_finish_baseline = 0;
	gx_xfb_snapshot = NULL;
	gx_vfb_snapshot = NULL;
	gx_debugfs_dir = NULL;
	gx_xfb_debugfs_file = NULL;
	gx_vfb_debugfs_file = NULL;
	if (gx_debug_capture) {
		gx_xfb_snapshot = vzalloc(GX_XFB_SNAPSHOT_MAX);
		if (!gx_xfb_snapshot) {
			ret = -ENOMEM;
			goto err_snapshot;
		}
		gx_vfb_snapshot = vzalloc(GX_XFB_SNAPSHOT_MAX);
		if (!gx_vfb_snapshot) {
			ret = -ENOMEM;
			goto err_vfb_snapshot;
		}
		gx_debugfs_dir = debugfs_create_dir("gcn_gx", NULL);
		if (IS_ERR_OR_NULL(gx_debugfs_dir)) {
			ret = gx_debugfs_dir ? PTR_ERR(gx_debugfs_dir) : -ENODEV;
			gx_debugfs_dir = NULL;
			goto err_debugfs;
		}
		gx_xfb_debugfs_file = debugfs_create_file("xfb_yuyv", 0400,
							 gx_debugfs_dir, NULL,
							 &gx_xfb_snapshot_fops);
		gx_vfb_debugfs_file = debugfs_create_file("vfb_rgb565be", 0400,
							 gx_debugfs_dir, NULL,
							 &gx_vfb_snapshot_fops);
		debugfs_create_u32("xfb_width", 0400, gx_debugfs_dir,
				   &gx_xfb_snapshot_width);
		debugfs_create_u32("xfb_height", 0400, gx_debugfs_dir,
				   &gx_xfb_snapshot_height);
		debugfs_create_x32("xfb_phys", 0400, gx_debugfs_dir,
				   &gx_xfb_snapshot_phys);
	}

#if IS_ENABLED(CONFIG_DRM_GCN_GX)
	pr_info("gcn-gx: ready fifo=%08x tex=%08x/%08x pool=%u/%u/%u irq=%u renderer=%s bias8=%d debug_capture=%u offscreen_probe=%u render_only=%u\n",
		(u32)gx_fifo_phys, (u32)gx_tex_phys,
		(u32)gx_tex_workspace[1].phys_addr,
		gx_mem1_total_bytes, gx_mem1_used_bytes, gx_mem1_free_bytes,
		gx_pe_finish_irq, gx_renderer, gx_texel_bias_eighths,
		gx_debug_capture, gx_offscreen_probe, gx_render_only);
#else
	pr_info("gcn-gx: ready fifo=%08x tex=%08x/%08x irq=%u renderer=%s bias8=%d debug_capture=%u offscreen_probe=%u\n",
		(u32)gx_fifo_phys, (u32)gx_tex_phys,
		(u32)(gx_tex_phys + GX_TEX_BUF_SLOT_SIZE),
		gx_pe_finish_irq, gx_renderer, gx_texel_bias_eighths,
		gx_debug_capture, gx_offscreen_probe);
#endif
	gx_accel_ready = true;
	return 0;

err_debugfs:
	vfree(gx_vfb_snapshot);
	gx_vfb_snapshot = NULL;
err_vfb_snapshot:
	vfree(gx_xfb_snapshot);
	gx_xfb_snapshot = NULL;
err_snapshot:
err_hw:
	cp_write(CP_REG_CTRL, 0);
	if (gx_pe_finish_irq_requested) {
		free_irq(gx_pe_finish_irq, &gx_pe_finish_irq);
		gx_pe_finish_irq_requested = false;
	}
	gx_pe_finish_irq = 0;
#if IS_ENABLED(CONFIG_DRM_GCN_GX)
	gx_mem1_free_workspaces();
#endif
	gx_fifo_buf_raw = NULL;
	gx_fifo_buf = NULL;
	gx_tex_buf = NULL;
	gx_tex_buf_alt = NULL;
	return ret;
}

static void gcn_gx_exit(void)
{
	WRITE_ONCE(gx_accel_ready, false);
#if IS_ENABLED(CONFIG_FB_GAMECUBE_GX)
	cancel_work_sync(&gx_frame_work.work);
#endif
	debugfs_remove_recursive(gx_debugfs_dir);
	gx_debugfs_dir = NULL;
	gx_xfb_debugfs_file = NULL;
	gx_vfb_debugfs_file = NULL;
	WRITE_ONCE(gx_xfb_snapshot_size, 0);
	WRITE_ONCE(gx_vfb_snapshot_size, 0);
	vfree(gx_vfb_snapshot);
	gx_vfb_snapshot = NULL;
	vfree(gx_xfb_snapshot);
	gx_xfb_snapshot = NULL;
	gx_wait_idle();
	cp_write(CP_REG_CTRL, 0);
	if (gx_pe_finish_irq_requested) {
		pe_write(PE_REG_INTR_STATUS, PE_TOKEN_BIT | PE_FINISH_BIT);
		free_irq(gx_pe_finish_irq, &gx_pe_finish_irq);
		gx_pe_finish_irq_requested = false;
		gx_pe_finish_irq = 0;
	}

	/* Texture and FIFO buffers are platform-owned MEM1 reservations. */
#if IS_ENABLED(CONFIG_DRM_GCN_GX)
	gx_mem1_free_workspaces();
#endif
	gx_tex_raw = NULL;
	gx_tex_buf = NULL;
	gx_tex_buf_alt = NULL;
	gx_fifo_buf_raw = NULL;
	gx_fifo_buf = NULL;
	cp_regs = NULL;
	pe_regs = NULL;
	pi_regmap = NULL;
}

#if IS_ENABLED(CONFIG_FB_GAMECUBE_GX)
static const struct gcnfb_accel_ops gcn_gx_accel_ops = {
	.name = "gcn-gx",
	.take_completed = gcn_gx_take_completed,
	.blit_rgb565 = gcn_gx_blit_fb_rgb565,
	.blit_rgb888 = gcn_gx_blit_fb_rgb888,
};
#endif

static int gcn_gx_probe(struct platform_device *pdev)
{
	int ret;

	ret = gcn_gx_init(pdev);
	if (ret)
		return ret;

#if IS_ENABLED(CONFIG_DRM_GCN_GX)
	ret = gcn_drm_register_accel_v12(&gcn_gx_drm_accel_ops);
#else
	ret = gcnfb_register_accel(&gcn_gx_accel_ops);
#endif
	if (ret) {
		pr_err("gcn-gx: failed to register accelerator: %d\n", ret);
		gcn_gx_exit();
		return ret;
	}

	return 0;
}

static void gcn_gx_remove(struct platform_device *pdev)
{
	/*
	 * Stop queued rendering before removing the callback table.  A callback
	 * which passed its ready check just before this store may still queue one
	 * final work item; that item observes ready=false and does no hardware IO.
	 * synchronize_rcu() in unregister then drains all direct IRQ callbacks.
	 */
	WRITE_ONCE(gx_accel_ready, false);
#if IS_ENABLED(CONFIG_FB_GAMECUBE_GX)
	cancel_work_sync(&gx_frame_work.work);
	gcnfb_unregister_accel(&gcn_gx_accel_ops);
#else
	gcn_drm_unregister_accel_v12(&gcn_gx_drm_accel_ops);
#endif
	gcn_gx_exit();
}

static const struct of_device_id gcn_gx_of_match[] = {
	{ .compatible = "nintendo,flipper-gx" },
	{ }
};
MODULE_DEVICE_TABLE(of, gcn_gx_of_match);

static struct platform_driver gcn_gx_driver = {
	.probe = gcn_gx_probe,
	.remove = gcn_gx_remove,
	.driver = {
		.name = "gcn-gx",
		.of_match_table = gcn_gx_of_match,
	},
};
module_platform_driver(gcn_gx_driver);

MODULE_DESCRIPTION("Nintendo GameCube/Wii GX framebuffer accelerator");
MODULE_AUTHOR("Bill Carson <anolisporcatus@gmail.com> and OpenAI");
MODULE_LICENSE("GPL");
