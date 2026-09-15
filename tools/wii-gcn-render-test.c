// SPDX-License-Identifier: GPL-2.0-only

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#include <drm/gcn_drm.h>

static bool system_profile;
static bool system_sched;
static int system_trace_on = -1, system_trace_marker = -1;

#define TEST_WIDTH 256U
#define TEST_HEIGHT 256U
#define UNEQUAL_SRC_WIDTH 320U
#define UNEQUAL_SRC_HEIGHT 192U
#define WIDE_SRC_WIDTH 320U
#define WIDE_SRC_HEIGHT 120U
#define WIDE_DST_WIDTH 640U
#define WIDE_DST_HEIGHT 240U
#define WIDE_REDUCE_SRC_WIDTH 640U
#define WIDE_REDUCE_SRC_HEIGHT 240U
#define WIDE_REDUCE_DST_WIDTH 320U
#define WIDE_REDUCE_DST_HEIGHT 120U
#define FULL_SRC_WIDTH 320U
#define FULL_SRC_HEIGHT 240U
#define FULL_DST_WIDTH 640U
#define FULL_DST_HEIGHT 480U
#define MAX_OBJECTS 1024U
#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))

static int failures;

static void fail(const char *what);

static size_t tiled_rgb565_index(unsigned int x, unsigned int y,
				 unsigned int width)
{
	return ((size_t)(y >> 2) * (width >> 2) + (x >> 2)) * 16 +
	       (y & 3) * 4 + (x & 3);
}

static size_t rgb565_index(unsigned int x, unsigned int y,
			   unsigned int width, uint32_t layout)
{
	if (layout == DRM_GCN_GEM_LAYOUT_LINEAR)
		return (size_t)y * width + x;

	return tiled_rgb565_index(x, y, width);
}

static void tiled_rgba8_store(uint8_t *map, unsigned int x, unsigned int y,
			      unsigned int width, uint32_t rgba)
{
	size_t tile = ((size_t)(y >> 2) * (width >> 2) + (x >> 2)) * 64;
	size_t pixel = (y & 3) * 4 + (x & 3);

	map[tile + pixel * 2] = rgba;
	map[tile + pixel * 2 + 1] = rgba >> 24;
	map[tile + 32 + pixel * 2] = rgba >> 16;
	map[tile + 32 + pixel * 2 + 1] = rgba >> 8;
}

static uint16_t source_pattern(unsigned int x, unsigned int y)
{
	return ((x & 0x1f) << 11) | ((y & 0x3f) << 5) |
	       ((x ^ y) & 0x1f);
}

static uint16_t unequal_source_pattern(unsigned int x, unsigned int y)
{
	return y * UNEQUAL_SRC_WIDTH + x;
}

static uint16_t same_object_pattern(unsigned int x, unsigned int y)
{
	return y * TEST_WIDTH + x;
}

static uint32_t xrgb8888_source_pattern(unsigned int x, unsigned int y)
{
	uint32_t alpha = (x * 13 + y * 29) & 0xff;
	uint32_t red = (x * 37 + y * 11 + 3) & 0xff;
	uint32_t green = (x * 17 + y * 43 + 5) & 0xff;
	uint32_t blue = (x * 53 + y * 7 + 9) & 0xff;

	return (alpha << 24) | (red << 16) | (green << 8) | blue;
}

static uint16_t xrgb8888_to_rgb565(uint32_t pixel)
{
	return ((pixel >> 8) & 0xf800) |
	       ((pixel >> 5) & 0x07e0) |
	       ((pixel >> 3) & 0x001f);
}

struct same_object_blit_case {
	const char *name;
	uint16_t src_x;
	uint16_t src_y;
	uint16_t dst_x;
	uint16_t dst_y;
	uint16_t width;
	uint16_t height;
};

struct scaled_blit_case {
	const char *name;
	uint16_t src_x;
	uint16_t src_y;
	uint16_t src_width;
	uint16_t src_height;
	uint16_t dst_x;
	uint16_t dst_y;
	uint16_t dst_width;
	uint16_t dst_height;
	bool same_object;
};

static unsigned int scaled_source_offset(unsigned int dst_offset,
					 unsigned int src_extent,
					 unsigned int dst_extent)
{
	uint64_t numerator = (uint64_t)(2 * dst_offset + 1) * src_extent;
	unsigned int offset = numerator / (2 * dst_extent);

	return offset < src_extent ? offset : src_extent - 1;
}

static int test_scaled_blit_case(int fd, uint32_t ctx_id,
				 uint32_t syncobj, uint32_t src_handle,
				 uint32_t dst_handle, uint16_t *src_map,
				 uint16_t *dst_map,
				 const struct scaled_blit_case *test)
{
	struct drm_gcn_blit_scaled blit = {
		.ctx_id = ctx_id,
		.src_handle = test->same_object ? dst_handle : src_handle,
		.dst_handle = dst_handle,
		.out_syncobj = syncobj,
		.src_x = test->src_x,
		.src_y = test->src_y,
		.src_width = test->src_width,
		.src_height = test->src_height,
		.dst_x = test->dst_x,
		.dst_y = test->dst_y,
		.dst_width = test->dst_width,
		.dst_height = test->dst_height,
	};

	for (unsigned int y = 0; y < TEST_HEIGHT; y++) {
		for (unsigned int x = 0; x < TEST_WIDTH; x++) {
			size_t pixel = tiled_rgb565_index(x, y, TEST_WIDTH);

			src_map[pixel] = same_object_pattern(x, y);
			dst_map[pixel] = test->same_object ?
				same_object_pattern(x, y) : 0x18e3;
		}
	}

	if (ioctl(fd, DRM_IOCTL_GCN_BLIT_SCALED, &blit)) {
		fail(test->name);
		return -1;
	}

	for (unsigned int y = 0; y < TEST_HEIGHT; y++) {
		for (unsigned int x = 0; x < TEST_WIDTH; x++) {
			uint16_t expected = test->same_object ?
				same_object_pattern(x, y) : 0x18e3;
			size_t pixel = tiled_rgb565_index(x, y, TEST_WIDTH);

			if (x >= test->dst_x &&
			    x < test->dst_x + test->dst_width &&
			    y >= test->dst_y &&
			    y < test->dst_y + test->dst_height) {
				unsigned int source_x = test->src_x +
					scaled_source_offset(x - test->dst_x,
							     test->src_width,
							     test->dst_width);
				unsigned int source_y = test->src_y +
					scaled_source_offset(y - test->dst_y,
							     test->src_height,
							     test->dst_height);

				expected = same_object_pattern(source_x, source_y);
			}
			if (dst_map[pixel] != expected) {
				fprintf(stderr,
					"FAIL: %s mismatch at (%u,%u): got=0x%04x expected=0x%04x\n",
					test->name, x, y, dst_map[pixel], expected);
				failures++;
				return -1;
			}
		}
	}

	printf("SCALED: %s preserved all sampled and outside pixels\n",
	       test->name);
	return 0;
}

static void test_scaled_blit_rejections(int fd, uint32_t ctx_id,
					uint32_t src_handle,
					uint32_t dst_handle)
{
	struct drm_gcn_blit_scaled blit = {
		.ctx_id = ctx_id,
		.src_handle = src_handle,
		.dst_handle = dst_handle,
		.src_x = 17,
		.src_y = 19,
		.src_width = 73,
		.src_height = 61,
		.dst_x = 29,
		.dst_y = 31,
		.dst_width = 113,
		.dst_height = 97,
	};

	blit.src_width = 0;
	errno = 0;
	if (!ioctl(fd, DRM_IOCTL_GCN_BLIT_SCALED, &blit) || errno != EINVAL)
		fail("zero-width scaled blit should return EINVAL");
	blit.src_width = 240;
	errno = 0;
	if (!ioctl(fd, DRM_IOCTL_GCN_BLIT_SCALED, &blit) || errno != EINVAL)
		fail("source-out-of-bounds scaled blit should return EINVAL");
	blit.src_width = 73;
	blit.dst_height = 226;
	errno = 0;
	if (!ioctl(fd, DRM_IOCTL_GCN_BLIT_SCALED, &blit) || errno != EINVAL)
		fail("destination-out-of-bounds scaled blit should return EINVAL");
	blit.dst_height = 97;
	blit.flags = 1;
	errno = 0;
	if (!ioctl(fd, DRM_IOCTL_GCN_BLIT_SCALED, &blit) || errno != EINVAL)
		fail("flagged scaled blit should return EINVAL");
	blit.flags = 0;
	blit.pad = 1;
	errno = 0;
	if (!ioctl(fd, DRM_IOCTL_GCN_BLIT_SCALED, &blit) || errno != EINVAL)
		fail("padded scaled blit should return EINVAL");
	blit.pad = 0;
	blit.ctx_id = 0xffffffffU;
	errno = 0;
	if (!ioctl(fd, DRM_IOCTL_GCN_BLIT_SCALED, &blit) || errno != ENOENT)
		fail("unknown-context scaled blit should return ENOENT");
	blit.ctx_id = ctx_id;
	blit.src_handle = 0;
	errno = 0;
	if (!ioctl(fd, DRM_IOCTL_GCN_BLIT_SCALED, &blit) || errno != EINVAL)
		fail("missing-source scaled blit should return EINVAL");
}

static int test_same_object_blit_case(int fd, struct drm_gcn_submit *submit,
				      uint16_t *map,
				      const struct same_object_blit_case *test)
{
	for (unsigned int y = 0; y < TEST_HEIGHT; y++) {
		for (unsigned int x = 0; x < TEST_WIDTH; x++) {
			size_t pixel = tiled_rgb565_index(x, y, TEST_WIDTH);

			map[pixel] = same_object_pattern(x, y);
		}
	}

	submit->data =
		DRM_GCN_BLIT_RECT_DATA(test->src_x, test->src_y,
				       test->dst_x, test->dst_y,
				       test->width, test->height);
	if (ioctl(fd, DRM_IOCTL_GCN_SUBMIT, submit)) {
		fail(test->name);
		return -1;
	}

	for (unsigned int y = 0; y < TEST_HEIGHT; y++) {
		for (unsigned int x = 0; x < TEST_WIDTH; x++) {
			uint16_t expected = same_object_pattern(x, y);
			size_t pixel = tiled_rgb565_index(x, y, TEST_WIDTH);

			if (x >= test->dst_x && x < test->dst_x + test->width &&
			    y >= test->dst_y && y < test->dst_y + test->height) {
				unsigned int source_x = test->src_x + x - test->dst_x;
				unsigned int source_y = test->src_y + y - test->dst_y;

				expected = same_object_pattern(source_x, source_y);
			}
			if (map[pixel] != expected) {
				fprintf(stderr,
					"FAIL: %s mismatch at (%u,%u): got=0x%04x expected=0x%04x\n",
					test->name, x, y, map[pixel], expected);
				failures++;
				return -1;
			}
		}
	}

	printf("SUBMIT: %s preserved all source and outside pixels\n",
	       test->name);
	return 0;
}

static void fail(const char *what)
{
	fprintf(stderr, "FAIL: %s: %s\n", what, strerror(errno));
	failures++;
}

static void fail_value(const char *what, uint64_t got, uint64_t expected)
{
	fprintf(stderr, "FAIL: %s: got=%llu expected=%llu\n", what,
		(unsigned long long)got, (unsigned long long)expected);
	failures++;
}

static int get_param(int fd, uint32_t param, uint64_t *value)
{
	struct drm_gcn_get_param args = {
		.param = param,
	};

	if (ioctl(fd, DRM_IOCTL_GCN_GET_PARAM, &args))
		return -1;
	*value = args.value;
	return 0;
}

static int
create_bo_size_format_layout_flags(int fd, struct drm_gcn_gem_create *args,
				   uint32_t width, uint32_t height,
				   uint32_t format, uint32_t layout,
				   uint32_t flags)
{
	memset(args, 0, sizeof(*args));
	args->width = width;
	args->height = height;
	args->format = format;
	args->layout = layout;
	args->flags = flags;
	return ioctl(fd, DRM_IOCTL_GCN_GEM_CREATE, args);
}

static int create_bo_size_layout_flags(int fd,
				       struct drm_gcn_gem_create *args,
				       uint32_t width, uint32_t height,
				       uint32_t layout, uint32_t flags)
{
	return create_bo_size_format_layout_flags(fd, args, width, height,
						  DRM_GCN_GEM_FORMAT_RGB565,
						  layout, flags);
}

static int create_bo_size_flags(int fd, struct drm_gcn_gem_create *args,
				uint32_t width, uint32_t height,
				uint32_t flags)
{
	return create_bo_size_layout_flags(fd, args, width, height,
					   DRM_GCN_GEM_LAYOUT_TILED_4X4,
					   flags);
}

static int create_bo_size(int fd, struct drm_gcn_gem_create *args,
			  uint32_t width, uint32_t height)
{
	return create_bo_size_flags(fd, args, width, height, 0);
}

static int create_bo(int fd, struct drm_gcn_gem_create *args)
{
	return create_bo_size(fd, args, TEST_WIDTH, TEST_HEIGHT);
}

static void *map_bo(int fd, const struct drm_gcn_gem_create *bo)
{
	struct drm_gcn_gem_mmap args = {
		.handle = bo->handle,
	};

	if (ioctl(fd, DRM_IOCTL_GCN_GEM_MMAP, &args))
		return MAP_FAILED;
	return mmap(NULL, bo->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
		    args.offset);
}

static int close_bo(int fd, uint32_t handle)
{
	struct drm_gem_close args = {
		.handle = handle,
	};

	return ioctl(fd, DRM_IOCTL_GEM_CLOSE, &args);
}

static uint64_t monotonic_ns(void)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now)) {
		fail("clock_gettime");
		return 0;
	}
	return (uint64_t)now.tv_sec * 1000000000ULL + now.tv_nsec;
}

static void test_provider_absent(int fd)
{
	struct drm_gcn_gem_create bo;
	struct drm_gcn_ctx_create ctx = {};
	struct drm_gcn_submit submit = {
		.ctx_id = 1,
		.op = DRM_GCN_RENDER_OP_COPY_RGB565,
		.src_handle = 1,
		.dst_handle = 2,
	};
	uint64_t value;

	errno = 0;
	if (!get_param(fd, DRM_GCN_PARAM_MEM1_TOTAL_BYTES, &value) ||
	    errno != ENODEV)
		fail("provider-specific GET_PARAM should return ENODEV");
	errno = 0;
	if (!create_bo(fd, &bo) || errno != ENODEV)
		fail("GEM_CREATE without provider should return ENODEV");
	errno = 0;
	if (!ioctl(fd, DRM_IOCTL_GCN_CTX_CREATE, &ctx) || errno != ENODEV)
		fail("CTX_CREATE without provider should return ENODEV");
	errno = 0;
	if (!ioctl(fd, DRM_IOCTL_GCN_SUBMIT, &submit) || errno != ENOENT)
		fail("SUBMIT without context should return ENOENT");
}

static void test_contexts(int fd, int other_fd)
{
	struct drm_gcn_ctx_create first = {};
	struct drm_gcn_ctx_create second = {};
	struct drm_gcn_ctx_free free_args;

	if (ioctl(fd, DRM_IOCTL_GCN_CTX_CREATE, &first)) {
		fail("create first context");
		return;
	}
	if (ioctl(fd, DRM_IOCTL_GCN_CTX_CREATE, &second)) {
		fail("create second context");
		goto free_first;
	}
	if (!first.id || !second.id || first.id == second.id)
		fail_value("context IDs must be nonzero and unique", second.id,
			   first.id + 1);

	free_args.id = first.id;
	free_args.pad = 0;
	errno = 0;
	if (!ioctl(other_fd, DRM_IOCTL_GCN_CTX_FREE, &free_args) ||
	    errno != ENOENT)
		fail("context must be isolated to its DRM file");

	free_args.id = second.id;
	if (ioctl(fd, DRM_IOCTL_GCN_CTX_FREE, &free_args))
		fail("free second context");
free_first:
	free_args.id = first.id;
	free_args.pad = 0;
	if (ioctl(fd, DRM_IOCTL_GCN_CTX_FREE, &free_args))
		fail("free first context");
}

static void test_mapping(int fd, const struct drm_gcn_gem_create *bo,
			 void **mapping_out)
{
	struct drm_gcn_gem_mmap mmap_args = {
		.handle = bo->handle,
	};
	uint8_t *mapping;
	void *oversized;
	size_t i;

	if (ioctl(fd, DRM_IOCTL_GCN_GEM_MMAP, &mmap_args)) {
		fail("GEM_MMAP offset query");
		return;
	}
	mapping = mmap(NULL, bo->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
		       mmap_args.offset);
	if (mapping == MAP_FAILED) {
		fail("mmap MEM1 object");
		return;
	}
	for (i = 0; i < bo->size; i++) {
		if (mapping[i]) {
			fprintf(stderr, "FAIL: new MEM1 object not zero at %zu\n", i);
			failures++;
			break;
		}
	}
	for (i = 0; i < bo->size; i++)
		mapping[i] = (uint8_t)(i * 37U + 11U);
	for (i = 0; i < bo->size; i++) {
		if (mapping[i] != (uint8_t)(i * 37U + 11U)) {
			fprintf(stderr, "FAIL: MEM1 readback mismatch at %zu\n", i);
			failures++;
			break;
		}
	}

	errno = 0;
	oversized = mmap(NULL, bo->size + 4096, PROT_READ | PROT_WRITE,
			 MAP_SHARED, fd, mmap_args.offset);
	if (oversized != MAP_FAILED) {
		munmap(oversized, bo->size + 4096);
		errno = 0;
		fail("oversized MEM1 mmap unexpectedly succeeded");
	} else if (errno != EINVAL) {
		fail("oversized MEM1 mmap should return EINVAL");
	}
	*mapping_out = mapping;
}

static void test_wait_and_prime(int fd, uint32_t handle)
{
	struct drm_gcn_wait wait_args = {
		.handle = handle,
		.flags = DRM_GCN_WAIT_WRITE,
		.timeout_ns = monotonic_ns() + 1000000000ULL,
	};
	struct drm_prime_handle prime = {
		.handle = handle,
		.flags = DRM_CLOEXEC | DRM_RDWR,
	};

	if (ioctl(fd, DRM_IOCTL_GCN_WAIT, &wait_args))
		fail("wait on idle MEM1 object");
	errno = 0;
	if (!ioctl(fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime) ||
	    errno != EOPNOTSUPP)
		fail("PRIME export should return EOPNOTSUPP");
}

static void test_syncobj(int fd)
{
	struct drm_syncobj_create create = {
		.flags = DRM_SYNCOBJ_CREATE_SIGNALED,
	};
	struct drm_syncobj_destroy destroy;

	if (ioctl(fd, DRM_IOCTL_SYNCOBJ_CREATE, &create)) {
		fail("create core DRM sync object");
		return;
	}
	if (!create.handle)
		fail_value("sync object handle", create.handle, 1);
	destroy.handle = create.handle;
	destroy.pad = 0;
	if (ioctl(fd, DRM_IOCTL_SYNCOBJ_DESTROY, &destroy))
		fail("destroy core DRM sync object");
}

static void test_submit(int fd, int other_fd)
{
	static const uint16_t fill_colors[] = {
		0x0000, 0xffff, 0x5aa5, 0xa55a,
	};
	static const struct same_object_blit_case same_object_cases[] = {
		{ "same-object identical-region blit", 37, 41, 37, 41, 83, 71 },
		{ "same-object non-overlap blit", 9, 11, 171, 177, 53, 47 },
		{ "same-object right-overlap blit", 17, 31, 43, 31, 113, 79 },
		{ "same-object left-overlap blit", 43, 31, 17, 31, 113, 79 },
		{ "same-object down-overlap blit", 29, 19, 29, 47, 91, 117 },
		{ "same-object up-overlap blit", 29, 47, 29, 19, 91, 117 },
		{ "same-object diagonal-overlap blit", 23, 27, 47, 51, 129, 111 },
	};
	static const struct scaled_blit_case scaled_cases[] = {
		{ "no-scale control", 37, 41, 83, 71, 113, 127, 83, 71, false },
		{ "two-times upscale", 17, 19, 53, 47, 101, 109, 106, 94, false },
		{ "two-times downscale", 29, 31, 122, 98, 11, 13, 61, 49, false },
		{ "mixed-axis scale", 43, 23, 67, 106, 131, 17, 113, 53, false },
		{ "odd-ratio scale", 59, 61, 73, 67, 7, 9, 119, 101, false },
		{ "one-pixel replication", 211, 199, 1, 1, 31, 37, 89, 79, false },
		{ "same-object overlapping scale", 17, 31, 73, 67, 43, 47, 113, 101, true },
		{ "small prime upscale", 13, 17, 3, 5, 31, 29, 7, 11, false },
		{ "prime upscale", 19, 23, 7, 11, 41, 43, 19, 23, false },
		{ "prime downscale", 17, 19, 127, 113, 131, 137, 101, 89, false },
		{ "prime enlargement", 23, 29, 101, 89, 109, 103, 127, 113, false },
		{ "opposed prime down-up", 31, 37, 127, 61, 17, 19, 89, 131, false },
		{ "opposed prime up-down", 37, 31, 61, 127, 19, 17, 131, 89, false },
		{ "near-identity reduction", 29, 31, 127, 131, 83, 79, 126, 130, false },
		{ "near-identity enlargement", 31, 29, 126, 130, 79, 83, 127, 131, false },
		{ "full-width reduction", 0, 47, 256, 73, 0, 101, 255, 73, false },
		{ "full-width enlargement", 0, 43, 255, 79, 0, 97, 256, 79, false },
		{ "tall odd mixed-axis scale", 1, 1, 255, 255, 0, 61, 256, 127, false },
		{ "horizontal one-pixel replication", 173, 71, 1, 83, 0, 151, 256, 83, false },
		{ "vertical one-pixel replication", 67, 211, 97, 1, 151, 0, 97, 256, false },
		{ "extreme horizontal reduction", 0, 53, 255, 71, 101, 113, 2, 71, false },
		{ "extreme vertical reduction", 59, 0, 73, 255, 109, 101, 73, 2, false },
		{ "same-object near-identity overlap", 17, 19, 127, 113, 31, 37, 126, 112, true },
		{ "same-object opposed scale", 43, 47, 61, 127, 17, 23, 131, 89, true },
	};
	struct drm_syncobj_create sync = {};
	struct drm_syncobj_destroy destroy;
	struct drm_gcn_ctx_create ctx = {};
	struct drm_gcn_ctx_free free_ctx;
	struct drm_gcn_gem_create src;
	struct drm_gcn_gem_create dst;
	struct drm_gcn_gem_create unequal_src = {};
	struct drm_gcn_submit submit;
	struct drm_gcn_wait wait_args;
	struct drm_syncobj_wait sync_wait;
	uint32_t sync_handle;
	uint16_t *src_map = MAP_FAILED;
	uint16_t *dst_map = MAP_FAILED;
	uint16_t *unequal_src_map = MAP_FAILED;
	size_t pixels;
	size_t i;

	if (create_bo(fd, &src) || create_bo(fd, &dst)) {
		fail("create render-copy objects");
		return;
	}
	src_map = map_bo(fd, &src);
	dst_map = map_bo(fd, &dst);
	if (src_map == MAP_FAILED || dst_map == MAP_FAILED) {
		fail("map render-copy objects");
		goto out;
	}
	pixels = TEST_WIDTH * TEST_HEIGHT;
	for (i = 0; i < pixels; i++) {
		src_map[i] = 0xf81f;
		dst_map[i] = 0x07e0;
	}

	if (ioctl(fd, DRM_IOCTL_GCN_CTX_CREATE, &ctx)) {
		fail("create submit context");
		goto out;
	}
	if (ioctl(fd, DRM_IOCTL_SYNCOBJ_CREATE, &sync)) {
		fail("create submit syncobj");
		goto out_ctx;
	}

	memset(&submit, 0, sizeof(submit));
	submit.ctx_id = ctx.id;
	submit.op = DRM_GCN_RENDER_OP_COPY_RGB565;
	submit.src_handle = src.handle;
	submit.dst_handle = dst.handle;
	submit.out_syncobj = sync.handle;

	submit.ctx_id++;
	errno = 0;
	if (!ioctl(fd, DRM_IOCTL_GCN_SUBMIT, &submit) || errno != ENOENT)
		fail("submit with unknown context should return ENOENT");
	submit.ctx_id = ctx.id;
	submit.dst_handle = src.handle;
	errno = 0;
	if (!ioctl(fd, DRM_IOCTL_GCN_SUBMIT, &submit) || errno != EINVAL)
		fail("submit with aliased objects should return EINVAL");
	submit.dst_handle = dst.handle;
	submit.src_handle = 0x7fffffffU;
	errno = 0;
	if (!ioctl(other_fd, DRM_IOCTL_GCN_SUBMIT, &submit) || errno != ENOENT)
		fail("submit context must be isolated to its DRM file");
	submit.src_handle = src.handle;

	if (ioctl(fd, DRM_IOCTL_GCN_SUBMIT, &submit)) {
		fail("submit RGB565 texture copy");
		goto out_sync;
	}

	wait_args.handle = dst.handle;
	wait_args.flags = DRM_GCN_WAIT_WRITE;
	wait_args.timeout_ns = monotonic_ns() + 1000000000ULL;
	if (ioctl(fd, DRM_IOCTL_GCN_WAIT, &wait_args))
		fail("wait for render-copy destination");

	sync_handle = sync.handle;
	memset(&sync_wait, 0, sizeof(sync_wait));
	sync_wait.handles = (uintptr_t)&sync_handle;
	sync_wait.timeout_nsec = (int64_t)(monotonic_ns() + 1000000000ULL);
	sync_wait.count_handles = 1;
	if (ioctl(fd, DRM_IOCTL_SYNCOBJ_WAIT, &sync_wait))
		fail("wait for render-copy syncobj");

	for (i = 0; i < pixels; i++) {
		if (dst_map[i] != 0xf81f) {
			fprintf(stderr,
				"FAIL: render-copy mismatch at %zu: got=0x%04x expected=0xf81f\n",
				i, dst_map[i]);
			failures++;
			break;
		}
	}
	if (i == pixels)
		printf("SUBMIT: copied %zu tiled RGB565 pixels byte-exactly\n",
		       pixels);

	submit.op = DRM_GCN_RENDER_OP_FILL_RGB565;
	submit.src_handle = 0;
	submit.data = 1ULL << 16;
	errno = 0;
	if (!ioctl(fd, DRM_IOCTL_GCN_SUBMIT, &submit) || errno != EINVAL)
		fail("fill with out-of-range colour should return EINVAL");
	submit.data = fill_colors[0];
	submit.src_handle = src.handle;
	errno = 0;
	if (!ioctl(fd, DRM_IOCTL_GCN_SUBMIT, &submit) || errno != EINVAL)
		fail("fill with source object should return EINVAL");
	submit.src_handle = 0;

	for (i = 0; i < ARRAY_SIZE(fill_colors); i++) {
		size_t pixel;

		submit.data = fill_colors[i];
		if (ioctl(fd, DRM_IOCTL_GCN_SUBMIT, &submit)) {
			fail("submit RGB565 solid fill");
			break;
		}
		for (pixel = 0; pixel < pixels; pixel++) {
			if (dst_map[pixel] != fill_colors[i]) {
				fprintf(stderr,
					"FAIL: render-fill mismatch at %zu: got=0x%04x expected=0x%04x\n",
					pixel, dst_map[pixel], fill_colors[i]);
				failures++;
				break;
			}
		}
		if (pixel != pixels)
			break;
	}
	if (i == ARRAY_SIZE(fill_colors))
		printf("SUBMIT: filled %zu tiled RGB565 pixels byte-exactly across %zu colours\n",
		       pixels, i);

	wait_args.timeout_ns = monotonic_ns() + 1000000000ULL;
	if (ioctl(fd, DRM_IOCTL_GCN_WAIT, &wait_args))
		fail("wait for render-fill destination");
	sync_wait.timeout_nsec = (int64_t)(monotonic_ns() + 1000000000ULL);
	if (ioctl(fd, DRM_IOCTL_SYNCOBJ_WAIT, &sync_wait))
		fail("wait for render-fill syncobj");

	submit.op = DRM_GCN_RENDER_OP_FILL_RECT_RGB565;
	submit.data = DRM_GCN_RECT_DATA(0xf800, 255, 255, 2, 1);
	errno = 0;
	if (!ioctl(fd, DRM_IOCTL_GCN_SUBMIT, &submit) || errno != EINVAL)
		fail("out-of-bounds rectangle should return EINVAL");
	submit.data = DRM_GCN_RECT_DATA(0xf800, 13, 17, 73, 61) |
		      DRM_GCN_RECT_RESERVED_MASK;
	errno = 0;
	if (!ioctl(fd, DRM_IOCTL_GCN_SUBMIT, &submit) || errno != EINVAL)
		fail("rectangle with reserved bits should return EINVAL");
	submit.data = DRM_GCN_RECT_DATA(0xf800, 13, 17, 73, 61);
	submit.src_handle = src.handle;
	errno = 0;
	if (!ioctl(fd, DRM_IOCTL_GCN_SUBMIT, &submit) || errno != EINVAL)
		fail("rectangle fill with source object should return EINVAL");
	submit.src_handle = 0;

	/* Establish an exact background, then verify every inside/outside pixel. */
	submit.op = DRM_GCN_RENDER_OP_FILL_RGB565;
	submit.data = 0x07e0;
	if (ioctl(fd, DRM_IOCTL_GCN_SUBMIT, &submit)) {
		fail("seed rectangle-fill background");
		goto out_sync;
	}
	submit.op = DRM_GCN_RENDER_OP_FILL_RECT_RGB565;
	submit.data = DRM_GCN_RECT_DATA(0xf800, 13, 17, 73, 61);
	if (ioctl(fd, DRM_IOCTL_GCN_SUBMIT, &submit)) {
		fail("submit interior RGB565 rectangle fill");
		goto out_sync;
	}
	for (unsigned int y = 0; y < TEST_HEIGHT; y++) {
		for (unsigned int x = 0; x < TEST_WIDTH; x++) {
			uint16_t expected = x >= 13 && x < 86 &&
					    y >= 17 && y < 78 ? 0xf800 : 0x07e0;
			size_t pixel = tiled_rgb565_index(x, y, TEST_WIDTH);

			if (dst_map[pixel] != expected) {
				fprintf(stderr,
					"FAIL: rectangle mismatch at (%u,%u) tile=%zu: got=0x%04x expected=0x%04x\n",
					x, y, pixel, dst_map[pixel], expected);
				failures++;
				goto rect_done;
			}
		}
	}
	printf("SUBMIT: filled 73x61 RGB565 rectangle with all outside pixels preserved\n");

	/* The final representable coordinate must update exactly one pixel. */
	submit.data = DRM_GCN_RECT_DATA(0x001f, 255, 255, 1, 1);
	if (ioctl(fd, DRM_IOCTL_GCN_SUBMIT, &submit)) {
		fail("submit bottom-right one-pixel rectangle");
		goto out_sync;
	}
	for (unsigned int y = 0; y < TEST_HEIGHT; y++) {
		for (unsigned int x = 0; x < TEST_WIDTH; x++) {
			uint16_t expected;
			size_t pixel = tiled_rgb565_index(x, y, TEST_WIDTH);

			if (x == 255 && y == 255)
				expected = 0x001f;
			else if (x >= 13 && x < 86 && y >= 17 && y < 78)
				expected = 0xf800;
			else
				expected = 0x07e0;
			if (dst_map[pixel] != expected) {
				fprintf(stderr,
					"FAIL: one-pixel rectangle mismatch at (%u,%u): got=0x%04x expected=0x%04x\n",
					x, y, dst_map[pixel], expected);
				failures++;
				goto rect_done;
			}
		}
	}
	puts("SUBMIT: filled bottom-right RGB565 pixel with prior surface preserved");

rect_done:
	wait_args.timeout_ns = monotonic_ns() + 1000000000ULL;
	if (ioctl(fd, DRM_IOCTL_GCN_WAIT, &wait_args))
		fail("wait for rectangle-fill destination");
	sync_wait.timeout_nsec = (int64_t)(monotonic_ns() + 1000000000ULL);
	if (ioctl(fd, DRM_IOCTL_SYNCOBJ_WAIT, &sync_wait))
		fail("wait for rectangle-fill syncobj");

	/* Validate translated source sampling and exact outside preservation. */
	for (unsigned int y = 0; y < TEST_HEIGHT; y++) {
		for (unsigned int x = 0; x < TEST_WIDTH; x++) {
			size_t pixel = tiled_rgb565_index(x, y, TEST_WIDTH);

			src_map[pixel] = source_pattern(x, y);
			dst_map[pixel] = 0x39e7;
		}
	}
	submit.op = DRM_GCN_RENDER_OP_BLIT_RECT_RGB565;
	submit.src_handle = src.handle;
	submit.data = DRM_GCN_BLIT_RECT_DATA(255, 255, 0, 0, 2, 1);
	errno = 0;
	if (!ioctl(fd, DRM_IOCTL_GCN_SUBMIT, &submit) || errno != EINVAL)
		fail("source-out-of-bounds rectangle blit should return EINVAL");
	submit.data = DRM_GCN_BLIT_RECT_DATA(0, 0, 255, 255, 1, 2);
	errno = 0;
	if (!ioctl(fd, DRM_IOCTL_GCN_SUBMIT, &submit) || errno != EINVAL)
		fail("destination-out-of-bounds rectangle blit should return EINVAL");
	submit.data = DRM_GCN_BLIT_RECT_DATA(101, 29, 11, 97, 67, 53) |
		      DRM_GCN_BLIT_RESERVED_MASK;
	errno = 0;
	if (!ioctl(fd, DRM_IOCTL_GCN_SUBMIT, &submit) || errno != EINVAL)
		fail("rectangle blit with reserved bits should return EINVAL");
	submit.data = DRM_GCN_BLIT_RECT_DATA(101, 29, 11, 97, 67, 53);
	submit.dst_handle = src.handle;
	submit.dst_handle = dst.handle;
	if (ioctl(fd, DRM_IOCTL_GCN_SUBMIT, &submit)) {
		fail("submit interior RGB565 rectangle blit");
		goto out_sync;
	}
	for (unsigned int y = 0; y < TEST_HEIGHT; y++) {
		for (unsigned int x = 0; x < TEST_WIDTH; x++) {
			uint16_t expected = 0x39e7;
			size_t pixel = tiled_rgb565_index(x, y, TEST_WIDTH);

			if (x >= 11 && x < 78 && y >= 97 && y < 150)
				expected = source_pattern(x - 11 + 101,
							  y - 97 + 29);
			if (dst_map[pixel] != expected) {
				fprintf(stderr,
					"FAIL: rectangle blit mismatch at (%u,%u): got=0x%04x expected=0x%04x\n",
					x, y, dst_map[pixel], expected);
				failures++;
				goto blit_done;
			}
		}
	}
	puts("SUBMIT: blitted translated 67x53 RGB565 rectangle with all outside pixels preserved");

	submit.data = DRM_GCN_BLIT_RECT_DATA(255, 255, 255, 255, 1, 1);
	if (ioctl(fd, DRM_IOCTL_GCN_SUBMIT, &submit)) {
		fail("submit bottom-right one-pixel rectangle blit");
		goto out_sync;
	}
	for (unsigned int y = 0; y < TEST_HEIGHT; y++) {
		for (unsigned int x = 0; x < TEST_WIDTH; x++) {
			uint16_t expected = 0x39e7;
			size_t pixel = tiled_rgb565_index(x, y, TEST_WIDTH);

			if (x == 255 && y == 255)
				expected = source_pattern(255, 255);
			else if (x >= 11 && x < 78 && y >= 97 && y < 150)
				expected = source_pattern(x - 11 + 101,
							  y - 97 + 29);
			if (dst_map[pixel] != expected) {
				fprintf(stderr,
					"FAIL: one-pixel blit mismatch at (%u,%u): got=0x%04x expected=0x%04x\n",
					x, y, dst_map[pixel], expected);
				failures++;
				goto blit_done;
			}
		}
	}
	puts("SUBMIT: blitted bottom-right RGB565 pixel with prior surface preserved");

	if (create_bo_size(fd, &unequal_src, UNEQUAL_SRC_WIDTH,
			   UNEQUAL_SRC_HEIGHT)) {
		fail("create unequal-dimension blit source");
		goto blit_done;
	}
	unequal_src_map = map_bo(fd, &unequal_src);
	if (unequal_src_map == MAP_FAILED) {
		fail("map unequal-dimension blit source");
		goto blit_done;
	}
	for (unsigned int y = 0; y < UNEQUAL_SRC_HEIGHT; y++) {
		for (unsigned int x = 0; x < UNEQUAL_SRC_WIDTH; x++) {
			size_t pixel = tiled_rgb565_index(x, y,
						   UNEQUAL_SRC_WIDTH);

			unequal_src_map[pixel] = unequal_source_pattern(x, y);
		}
	}
	submit.op = DRM_GCN_RENDER_OP_COPY_RGB565;
	submit.src_handle = unequal_src.handle;
	submit.data = 0;
	errno = 0;
	if (!ioctl(fd, DRM_IOCTL_GCN_SUBMIT, &submit) || errno != EINVAL)
		fail("full copy with unequal dimensions should return EINVAL");
	submit.op = DRM_GCN_RENDER_OP_BLIT_RECT_RGB565;
	for (unsigned int y = 0; y < TEST_HEIGHT; y++) {
		for (unsigned int x = 0; x < TEST_WIDTH; x++) {
			size_t pixel = tiled_rgb565_index(x, y, TEST_WIDTH);

			dst_map[pixel] = 0x2104;
		}
	}
	submit.src_handle = unequal_src.handle;
	submit.data = DRM_GCN_BLIT_RECT_DATA(241, 103, 11, 97, 67, 53);
	if (ioctl(fd, DRM_IOCTL_GCN_SUBMIT, &submit)) {
		fail("submit unequal-dimension RGB565 rectangle blit");
		goto blit_done;
	}
	for (unsigned int y = 0; y < TEST_HEIGHT; y++) {
		for (unsigned int x = 0; x < TEST_WIDTH; x++) {
			uint16_t expected = 0x2104;
			size_t pixel = tiled_rgb565_index(x, y, TEST_WIDTH);

			if (x >= 11 && x < 78 && y >= 97 && y < 150)
				expected = unequal_source_pattern(x + 230, y + 6);
			if (dst_map[pixel] != expected) {
				fprintf(stderr,
					"FAIL: unequal-dimension blit mismatch at (%u,%u): got=0x%04x expected=0x%04x\n",
					x, y, dst_map[pixel], expected);
				failures++;
				goto blit_done;
			}
		}
	}
	puts("SUBMIT: blitted from 320x192 source into 256x256 destination");
	puts("SUBMIT: unequal-dimension outside pixels preserved");

	submit.data = DRM_GCN_BLIT_RECT_DATA(319, 191, 255, 255, 1, 1);
	if (ioctl(fd, DRM_IOCTL_GCN_SUBMIT, &submit)) {
		fail("submit unequal-dimension bottom-right pixel blit");
		goto blit_done;
	}
	for (unsigned int y = 0; y < TEST_HEIGHT; y++) {
		for (unsigned int x = 0; x < TEST_WIDTH; x++) {
			uint16_t expected = 0x2104;
			size_t pixel = tiled_rgb565_index(x, y, TEST_WIDTH);

			if (x == 255 && y == 255)
				expected = unequal_source_pattern(319, 191);
			else if (x >= 11 && x < 78 && y >= 97 && y < 150)
				expected = unequal_source_pattern(x + 230, y + 6);
			if (dst_map[pixel] != expected) {
				fprintf(stderr,
					"FAIL: unequal-dimension edge blit mismatch at (%u,%u): got=0x%04x expected=0x%04x\n",
					x, y, dst_map[pixel], expected);
				failures++;
				goto blit_done;
			}
		}
	}
	puts("SUBMIT: blitted unequal source and destination bottom-right pixels");
	puts("SUBMIT: unequal-dimension prior surface preserved");

	submit.src_handle = dst.handle;
	submit.dst_handle = dst.handle;
	for (i = 0; i < ARRAY_SIZE(same_object_cases); i++) {
		if (test_same_object_blit_case(fd, &submit, dst_map,
					       &same_object_cases[i]))
			goto blit_done;
	}
	puts("SUBMIT: same-object RGB565 blits passed in all overlap directions");

	test_scaled_blit_rejections(fd, ctx.id, src.handle, dst.handle);
	for (i = 0; i < ARRAY_SIZE(scaled_cases); i++) {
		if (test_scaled_blit_case(fd, ctx.id, sync.handle, src.handle,
					  dst.handle, src_map, dst_map,
					  &scaled_cases[i]))
			goto blit_done;
	}
	puts("SCALED: RGB565 nearest blits passed all scale and alias cases");

blit_done:
	wait_args.timeout_ns = monotonic_ns() + 1000000000ULL;
	if (ioctl(fd, DRM_IOCTL_GCN_WAIT, &wait_args))
		fail("wait for rectangle-blit destination");
	sync_wait.timeout_nsec = (int64_t)(monotonic_ns() + 1000000000ULL);
	if (ioctl(fd, DRM_IOCTL_SYNCOBJ_WAIT, &sync_wait))
		fail("wait for rectangle-blit syncobj");

out_sync:
	destroy.handle = sync.handle;
	destroy.pad = 0;
	if (ioctl(fd, DRM_IOCTL_SYNCOBJ_DESTROY, &destroy))
		fail("destroy submit syncobj");
out_ctx:
	free_ctx.id = ctx.id;
	free_ctx.pad = 0;
	if (ioctl(fd, DRM_IOCTL_GCN_CTX_FREE, &free_ctx))
		fail("free submit context");
out:
	if (unequal_src_map != MAP_FAILED &&
	    munmap(unequal_src_map, unequal_src.size))
		fail("unmap unequal-dimension blit source");
	if (dst_map != MAP_FAILED && munmap(dst_map, dst.size))
		fail("unmap render-copy destination");
	if (src_map != MAP_FAILED && munmap(src_map, src.size))
		fail("unmap render-copy source");
	if (close_bo(fd, dst.handle))
		fail("close render-copy destination");
	if (close_bo(fd, src.handle))
		fail("close render-copy source");
	if (unequal_src.handle && close_bo(fd, unequal_src.handle))
		fail("close unequal-dimension blit source");
}

static int64_t triangle_edge(const struct drm_gcn_color_vertex *a,
			     const struct drm_gcn_color_vertex *b,
			     unsigned int x, unsigned int y)
{
	int64_t ax = 2 * a->x;
	int64_t ay = 2 * a->y;
	int64_t bx = 2 * b->x;
	int64_t by = 2 * b->y;
	int64_t px = 2 * x + 1;
	int64_t py = 2 * y + 1;

	return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

static int triangle_classify(const struct drm_gcn_color_triangle *triangle,
			     unsigned int x, unsigned int y, int64_t margin)
{
	int64_t area = triangle_edge(&triangle->vertices[0],
				     &triangle->vertices[1],
				     triangle->vertices[2].x,
				     triangle->vertices[2].y);
	int64_t edges[3] = {
		triangle_edge(&triangle->vertices[0], &triangle->vertices[1],
			      x, y),
		triangle_edge(&triangle->vertices[1], &triangle->vertices[2],
			      x, y),
		triangle_edge(&triangle->vertices[2], &triangle->vertices[0],
			      x, y),
	};

	if (area < 0) {
		edges[0] = -edges[0];
		edges[1] = -edges[1];
		edges[2] = -edges[2];
	}
	if (edges[0] > margin && edges[1] > margin && edges[2] > margin)
		return 1;
	if (edges[0] < -margin || edges[1] < -margin ||
	    edges[2] < -margin)
		return -1;
	return 0;
}

static int submit_triangle_state(int fd,
				 struct drm_gcn_draw_triangles_state *draw,
				 uint32_t sync_handle)
{
	struct drm_syncobj_wait wait = {
		.handles = (uintptr_t)&sync_handle,
		.timeout_nsec = (int64_t)(monotonic_ns() + 1000000000ULL),
		.count_handles = 1,
	};

	if (ioctl(fd, DRM_IOCTL_GCN_DRAW_TRIANGLES_STATE, draw))
		return -1;
	return ioctl(fd, DRM_IOCTL_SYNCOBJ_WAIT, &wait);
}

static int submit_triangle_depth(int fd,
				 struct drm_gcn_draw_triangles_depth *draw,
				 uint32_t sync_handle)
{
	struct drm_syncobj_wait wait = {
		.handles = (uintptr_t)&sync_handle,
		.timeout_nsec = (int64_t)(monotonic_ns() + 1000000000ULL),
		.count_handles = 1,
	};

	if (ioctl(fd, DRM_IOCTL_GCN_DRAW_TRIANGLES_DEPTH, draw))
		return -1;
	return ioctl(fd, DRM_IOCTL_SYNCOBJ_WAIT, &wait);
}

static int
submit_itex_depth(int fd, struct drm_gcn_draw_indexed_textured_depth *draw,
		  uint32_t sync_handle)
{
	struct drm_syncobj_wait wait = {
		.handles = (uintptr_t)&sync_handle,
		.timeout_nsec = (int64_t)(monotonic_ns() + 1000000000ULL),
		.count_handles = 1,
	};

	if (ioctl(fd, DRM_IOCTL_GCN_DRAW_INDEXED_TEXTURED_DEPTH, draw))
		return -1;
	return ioctl(fd, DRM_IOCTL_SYNCOBJ_WAIT, &wait);
}

static int submit_fixed(int fd, struct drm_gcn_draw_indexed_fixed *draw,
			uint32_t sync_handle)
{
	struct drm_syncobj_wait wait = {
		.handles = (uintptr_t)&sync_handle,
		.timeout_nsec = (int64_t)(monotonic_ns() + 1000000000ULL),
		.count_handles = 1,
	};

	if (ioctl(fd, DRM_IOCTL_GCN_DRAW_INDEXED_FIXED, draw))
		return -1;
	return ioctl(fd, DRM_IOCTL_SYNCOBJ_WAIT, &wait);
}

static void test_draw_triangles_state(int fd)
{
	struct drm_syncobj_create sync = {};
	struct drm_syncobj_destroy destroy = {};
	struct drm_gcn_ctx_create ctx = {};
	struct drm_gcn_ctx_free free_ctx = {};
	struct drm_gcn_gem_create dst = {};
	struct drm_gcn_submit fill = {
		.op = DRM_GCN_RENDER_OP_FILL_RGB565,
		.data = 0x07e0,
	};
	struct drm_gcn_color_triangle triangle = {
		.vertices = {
			{ 32, 32, DRM_GCN_RGBA8(0xff, 0, 0, 0xff) },
			{ 224, 48, DRM_GCN_RGBA8(0xff, 0, 0, 0xff) },
			{ 112, 224, DRM_GCN_RGBA8(0xff, 0, 0, 0xff) },
		},
	};
	struct drm_gcn_draw_triangles_state draw = {
		.triangle_count = 1,
		.triangles_ptr = (uintptr_t)&triangle,
		.state = {
			.viewport_width = TEST_WIDTH,
			.viewport_height = TEST_HEIGHT,
			.scissor_width = TEST_WIDTH,
			.scissor_height = TEST_HEIGHT,
			.blend_mode = DRM_GCN_BLEND_NONE,
		},
	};
	struct drm_gcn_color_triangle transformed;
	uint16_t *map = MAP_FAILED;
	unsigned int disabled_inside = 0;
	unsigned int viewport_inside = 0;
	unsigned int scissor_inside = 0;
	unsigned int blend_inside = 0;
	uint16_t blend_sample = 0;
	const int64_t margin = 1024;

	if (create_bo(fd, &dst)) {
		fail("create stateful-triangle destination");
		return;
	}
	map = map_bo(fd, &dst);
	if (map == MAP_FAILED) {
		fail("map stateful-triangle destination");
		goto out;
	}
	if (ioctl(fd, DRM_IOCTL_GCN_CTX_CREATE, &ctx)) {
		fail("create stateful-triangle context");
		goto out;
	}
	if (ioctl(fd, DRM_IOCTL_SYNCOBJ_CREATE, &sync)) {
		fail("create stateful-triangle syncobj");
		goto out_ctx;
	}

	fill.ctx_id = ctx.id;
	fill.dst_handle = dst.handle;
	draw.ctx_id = ctx.id;
	draw.dst_handle = dst.handle;
	draw.out_syncobj = sync.handle;

	draw.state.blend_mode = DRM_GCN_BLEND_SRC_ALPHA + 1;
	errno = 0;
	if (!ioctl(fd, DRM_IOCTL_GCN_DRAW_TRIANGLES_STATE, &draw) ||
	    errno != EINVAL)
		fail("invalid stateful blend mode should return EINVAL");
	draw.state.blend_mode = DRM_GCN_BLEND_NONE;
	draw.state.viewport_width = 0;
	errno = 0;
	if (!ioctl(fd, DRM_IOCTL_GCN_DRAW_TRIANGLES_STATE, &draw) ||
	    errno != EINVAL)
		fail("empty viewport should return EINVAL");
	draw.state.viewport_width = TEST_WIDTH;
	draw.state.scissor_x = TEST_WIDTH - 1;
	draw.state.scissor_width = 2;
	errno = 0;
	if (!ioctl(fd, DRM_IOCTL_GCN_DRAW_TRIANGLES_STATE, &draw) ||
	    errno != EINVAL)
		fail("overflowing scissor should return EINVAL");
	draw.state.scissor_x = 0;
	draw.state.scissor_width = TEST_WIDTH;
	triangle.vertices[0].rgba &= ~0xffU;
	errno = 0;
	if (!ioctl(fd, DRM_IOCTL_GCN_DRAW_TRIANGLES_STATE, &draw) ||
	    errno != EINVAL)
		fail("transparent unblended triangle should return EINVAL");
	triangle.vertices[0].rgba |= 0xff;

	if (ioctl(fd, DRM_IOCTL_GCN_SUBMIT, &fill) ||
	    submit_triangle_state(fd, &draw, sync.handle)) {
		fail("draw disabled state-equivalence triangle");
		goto out_sync;
	}
	for (unsigned int y = 0; y < TEST_HEIGHT; y++) {
		for (unsigned int x = 0; x < TEST_WIDTH; x++) {
			int classified = triangle_classify(&triangle, x, y, margin);
			uint16_t expected;
			size_t pixel;

			if (classified > 0) {
				expected = 0xf800;
				disabled_inside++;
			} else if (classified < 0) {
				expected = 0x07e0;
			} else {
				continue;
			}
			pixel = tiled_rgb565_index(x, y, TEST_WIDTH);
			if (map[pixel] != expected) {
				fprintf(stderr,
					"FAIL: disabled state mismatch at (%u,%u): got=0x%04x expected=0x%04x\n",
					x, y, map[pixel], expected);
				failures++;
				goto out_sync;
			}
		}
	}

	triangle = (struct drm_gcn_color_triangle) {
		.vertices = {
			{ 0, 0, DRM_GCN_RGBA8(0xff, 0, 0, 0xff) },
			{ TEST_WIDTH, 0, DRM_GCN_RGBA8(0xff, 0, 0, 0xff) },
			{ 0, TEST_HEIGHT, DRM_GCN_RGBA8(0xff, 0, 0, 0xff) },
		},
	};
	transformed = (struct drm_gcn_color_triangle) {
		.vertices = {
			{ 64, 64, 0 }, { 192, 64, 0 }, { 64, 192, 0 },
		},
	};
	draw.state.viewport_x = 64;
	draw.state.viewport_y = 64;
	draw.state.viewport_width = 128;
	draw.state.viewport_height = 128;
	if (ioctl(fd, DRM_IOCTL_GCN_SUBMIT, &fill) ||
	    submit_triangle_state(fd, &draw, sync.handle)) {
		fail("draw viewport triangle");
		goto out_sync;
	}
	for (unsigned int y = 0; y < TEST_HEIGHT; y++) {
		for (unsigned int x = 0; x < TEST_WIDTH; x++) {
			int classified = triangle_classify(&transformed, x, y,
							   margin);
			uint16_t expected;
			size_t pixel;

			if (classified > 0) {
				expected = 0xf800;
				viewport_inside++;
			} else if (classified < 0) {
				expected = 0x07e0;
			} else {
				continue;
			}
			pixel = tiled_rgb565_index(x, y, TEST_WIDTH);
			if (map[pixel] != expected) {
				fprintf(stderr,
					"FAIL: viewport mismatch at (%u,%u): got=0x%04x expected=0x%04x\n",
					x, y, map[pixel], expected);
				failures++;
				goto out_sync;
			}
		}
	}

	draw.state.viewport_x = 0;
	draw.state.viewport_y = 0;
	draw.state.viewport_width = TEST_WIDTH;
	draw.state.viewport_height = TEST_HEIGHT;
	draw.state.scissor_x = 80;
	draw.state.scissor_y = 80;
	draw.state.scissor_width = 64;
	draw.state.scissor_height = 64;
	if (ioctl(fd, DRM_IOCTL_GCN_SUBMIT, &fill) ||
	    submit_triangle_state(fd, &draw, sync.handle)) {
		fail("draw scissored triangle");
		goto out_sync;
	}
	for (unsigned int y = 0; y < TEST_HEIGHT; y++) {
		for (unsigned int x = 0; x < TEST_WIDTH; x++) {
			int classified = triangle_classify(&triangle, x, y, margin);
			bool inside_scissor = x >= 82 && x < 142 &&
				y >= 82 && y < 142;
			bool outside_scissor = x < 78 || x >= 146 ||
				y < 78 || y >= 146;
			uint16_t expected;
			size_t pixel;

			if (classified > 0 && inside_scissor) {
				expected = 0xf800;
				scissor_inside++;
			} else if (outside_scissor || classified < 0) {
				expected = 0x07e0;
			} else {
				continue;
			}
			pixel = tiled_rgb565_index(x, y, TEST_WIDTH);
			if (map[pixel] != expected) {
				fprintf(stderr,
					"FAIL: scissor mismatch at (%u,%u): got=0x%04x expected=0x%04x\n",
					x, y, map[pixel], expected);
				failures++;
				goto out_sync;
			}
		}
	}

	draw.state.scissor_x = 0;
	draw.state.scissor_y = 0;
	draw.state.scissor_width = TEST_WIDTH;
	draw.state.scissor_height = TEST_HEIGHT;
	draw.state.blend_mode = DRM_GCN_BLEND_SRC_ALPHA;
	for (unsigned int i = 0; i < 3; i++)
		triangle.vertices[i].rgba = DRM_GCN_RGBA8(0xff, 0, 0, 0x80);
	fill.data = 0x001f;
	if (ioctl(fd, DRM_IOCTL_GCN_SUBMIT, &fill) ||
	    submit_triangle_state(fd, &draw, sync.handle)) {
		fail("draw source-alpha triangle");
		goto out_sync;
	}
	for (unsigned int y = 0; y < TEST_HEIGHT; y++) {
		for (unsigned int x = 0; x < TEST_WIDTH; x++) {
			int classified = triangle_classify(&triangle, x, y, margin);
			size_t pixel = tiled_rgb565_index(x, y, TEST_WIDTH);
			uint16_t value = map[pixel];

			if (classified > 0) {
				unsigned int r = value >> 11;
				unsigned int g = (value >> 5) & 0x3f;
				unsigned int b = value & 0x1f;

				if (r < 14 || r > 17 || g > 1 || b < 14 || b > 17) {
					fprintf(stderr,
						"FAIL: blend mismatch at (%u,%u): got=0x%04x\n",
						x, y, value);
					failures++;
					goto out_sync;
				}
				blend_sample = value;
				blend_inside++;
			} else if (classified < 0 && value != 0x001f) {
				fprintf(stderr,
					"FAIL: blend exterior mismatch at (%u,%u): got=0x%04x\n",
					x, y, value);
				failures++;
				goto out_sync;
			}
		}
	}

	if (!disabled_inside || !viewport_inside || !scissor_inside ||
	    !blend_inside)
		fail("stateful triangle oracle did not classify pixels");
	else
		printf("DRAW state: disabled=%u viewport=%u scissor=%u blend=%u sample=%04x\n",
		       disabled_inside, viewport_inside, scissor_inside,
		       blend_inside, blend_sample);

out_sync:
	destroy.handle = sync.handle;
	if (ioctl(fd, DRM_IOCTL_SYNCOBJ_DESTROY, &destroy))
		fail("destroy stateful-triangle syncobj");
out_ctx:
	free_ctx.id = ctx.id;
	if (ioctl(fd, DRM_IOCTL_GCN_CTX_FREE, &free_ctx))
		fail("free stateful-triangle context");
out:
	if (map != MAP_FAILED && munmap(map, dst.size))
		fail("unmap stateful-triangle destination");
	if (dst.handle && close_bo(fd, dst.handle))
		fail("close stateful-triangle destination");
}

static void test_draw_triangles_depth(int fd)
{
	struct drm_syncobj_create sync = {};
	struct drm_syncobj_destroy destroy = {};
	struct drm_gcn_ctx_create ctx = {};
	struct drm_gcn_ctx_free free_ctx = {};
	struct drm_gcn_gem_create dst = {};
	struct drm_gcn_submit fill = {
		.op = DRM_GCN_RENDER_OP_FILL_RGB565,
		.data = 0x07e0,
	};
	struct drm_gcn_color_triangle coverage = {
		.vertices = {
			{ 32, 32, 0 }, { 224, 48, 0 }, { 112, 224, 0 },
		},
	};
	struct drm_gcn_color_depth_triangle triangles[2] = {
		{
			.vertices = {
				{ 32, 32, DRM_GCN_DEPTH_MAX / 4,
				  DRM_GCN_RGBA8(0, 0, 0xff, 0xff) },
				{ 224, 48, DRM_GCN_DEPTH_MAX / 4,
				  DRM_GCN_RGBA8(0, 0, 0xff, 0xff) },
				{ 112, 224, DRM_GCN_DEPTH_MAX / 4,
				  DRM_GCN_RGBA8(0, 0, 0xff, 0xff) },
			},
		},
		{
			.vertices = {
				{ 32, 32, DRM_GCN_DEPTH_MAX * 3 / 4,
				  DRM_GCN_RGBA8(0xff, 0, 0, 0xff) },
				{ 224, 48, DRM_GCN_DEPTH_MAX * 3 / 4,
				  DRM_GCN_RGBA8(0xff, 0, 0, 0xff) },
				{ 112, 224, DRM_GCN_DEPTH_MAX * 3 / 4,
				  DRM_GCN_RGBA8(0xff, 0, 0, 0xff) },
			},
		},
	};
	struct drm_gcn_draw_triangles_depth draw = {
		.triangle_count = ARRAY_SIZE(triangles),
		.triangles_ptr = (uintptr_t)triangles,
		.state = {
			.viewport_width = TEST_WIDTH,
			.viewport_height = TEST_HEIGHT,
			.scissor_width = TEST_WIDTH,
			.scissor_height = TEST_HEIGHT,
			.blend_mode = DRM_GCN_BLEND_NONE,
		},
		.depth = {
			.test_enable = 1,
			.compare = DRM_GCN_DEPTH_LESS,
			.write_enable = 1,
		},
	};
	static const struct {
		const char *name;
		uint32_t compare;
		uint32_t blue_depth;
		uint32_t red_depth;
		uint32_t write_enable;
		uint16_t expected;
	} depth_cases[] = {
		{ "never", DRM_GCN_DEPTH_NEVER,
		  DRM_GCN_DEPTH_MAX / 4, DRM_GCN_DEPTH_MAX * 3 / 4, 1,
		  0x07e0 },
		{ "equal", DRM_GCN_DEPTH_EQUAL,
		  DRM_GCN_DEPTH_MAX, DRM_GCN_DEPTH_MAX, 1, 0xf800 },
		{ "lequal", DRM_GCN_DEPTH_LEQUAL,
		  DRM_GCN_DEPTH_MAX / 4, DRM_GCN_DEPTH_MAX * 3 / 4, 1,
		  0x001f },
		{ "greater", DRM_GCN_DEPTH_GREATER,
		  DRM_GCN_DEPTH_MAX / 4, DRM_GCN_DEPTH_MAX * 3 / 4, 1,
		  0x07e0 },
		{ "nequal", DRM_GCN_DEPTH_NEQUAL,
		  DRM_GCN_DEPTH_MAX / 4, DRM_GCN_DEPTH_MAX * 3 / 4, 1,
		  0xf800 },
		{ "gequal", DRM_GCN_DEPTH_GEQUAL,
		  DRM_GCN_DEPTH_MAX, DRM_GCN_DEPTH_MAX, 1, 0xf800 },
		{ "always", DRM_GCN_DEPTH_ALWAYS,
		  DRM_GCN_DEPTH_MAX, DRM_GCN_DEPTH_MAX, 1, 0xf800 },
		{ "less-no-write", DRM_GCN_DEPTH_LESS,
		  DRM_GCN_DEPTH_MAX / 4, DRM_GCN_DEPTH_MAX * 3 / 4, 0,
		  0xf800 },
	};
	uint16_t *map = MAP_FAILED;
	unsigned int depth_inside = 0;
	unsigned int painter_inside = 0;
	const int64_t margin = 1024;

	if (create_bo(fd, &dst)) {
		fail("create depth-triangle destination");
		return;
	}
	map = map_bo(fd, &dst);
	if (map == MAP_FAILED) {
		fail("map depth-triangle destination");
		goto out;
	}
	if (ioctl(fd, DRM_IOCTL_GCN_CTX_CREATE, &ctx)) {
		fail("create depth-triangle context");
		goto out;
	}
	if (ioctl(fd, DRM_IOCTL_SYNCOBJ_CREATE, &sync)) {
		fail("create depth-triangle syncobj");
		goto out_ctx;
	}

	fill.ctx_id = ctx.id;
	fill.dst_handle = dst.handle;
	draw.ctx_id = ctx.id;
	draw.dst_handle = dst.handle;
	draw.out_syncobj = sync.handle;

	triangles[0].vertices[0].z = DRM_GCN_DEPTH_MAX + 1;
	errno = 0;
	if (!ioctl(fd, DRM_IOCTL_GCN_DRAW_TRIANGLES_DEPTH, &draw) ||
	    errno != EINVAL)
		fail("out-of-range triangle depth should return EINVAL");
	triangles[0].vertices[0].z = DRM_GCN_DEPTH_MAX / 4;
	draw.depth.compare = DRM_GCN_DEPTH_ALWAYS + 1;
	errno = 0;
	if (!ioctl(fd, DRM_IOCTL_GCN_DRAW_TRIANGLES_DEPTH, &draw) ||
	    errno != EINVAL)
		fail("invalid depth compare should return EINVAL");
	draw.depth.compare = DRM_GCN_DEPTH_LESS;
	draw.depth.test_enable = 2;
	errno = 0;
	if (!ioctl(fd, DRM_IOCTL_GCN_DRAW_TRIANGLES_DEPTH, &draw) ||
	    errno != EINVAL)
		fail("nonboolean depth enable should return EINVAL");
	draw.depth.test_enable = 1;
	draw.depth.write_enable = 2;
	errno = 0;
	if (!ioctl(fd, DRM_IOCTL_GCN_DRAW_TRIANGLES_DEPTH, &draw) ||
	    errno != EINVAL)
		fail("nonboolean depth write should return EINVAL");
	draw.depth.write_enable = 1;
	draw.depth.pad = 1;
	errno = 0;
	if (!ioctl(fd, DRM_IOCTL_GCN_DRAW_TRIANGLES_DEPTH, &draw) ||
	    errno != EINVAL)
		fail("nonzero depth padding should return EINVAL");
	draw.depth.pad = 0;

	if (ioctl(fd, DRM_IOCTL_GCN_SUBMIT, &fill) ||
	    submit_triangle_depth(fd, &draw, sync.handle)) {
		fail("draw depth-tested overlapping triangles");
		goto out_sync;
	}
	for (unsigned int y = 0; y < TEST_HEIGHT; y++) {
		for (unsigned int x = 0; x < TEST_WIDTH; x++) {
			int classified = triangle_classify(&coverage, x, y, margin);
			uint16_t expected;
			size_t pixel;

			if (classified > 0) {
				expected = 0x001f;
				depth_inside++;
			} else if (classified < 0) {
				expected = 0x07e0;
			} else {
				continue;
			}
			pixel = tiled_rgb565_index(x, y, TEST_WIDTH);
			if (map[pixel] != expected) {
				fprintf(stderr,
					"FAIL: depth mismatch at (%u,%u): got=0x%04x expected=0x%04x\n",
					x, y, map[pixel], expected);
				failures++;
				goto out_sync;
			}
		}
	}

	draw.depth.test_enable = 0;
	draw.depth.compare = DRM_GCN_DEPTH_ALWAYS;
	draw.depth.write_enable = 0;
	if (ioctl(fd, DRM_IOCTL_GCN_SUBMIT, &fill) ||
	    submit_triangle_depth(fd, &draw, sync.handle)) {
		fail("draw depth-disabled painter-order control");
		goto out_sync;
	}
	for (unsigned int y = 0; y < TEST_HEIGHT; y++) {
		for (unsigned int x = 0; x < TEST_WIDTH; x++) {
			int classified = triangle_classify(&coverage, x, y, margin);
			uint16_t expected;
			size_t pixel;

			if (classified > 0) {
				expected = 0xf800;
				painter_inside++;
			} else if (classified < 0) {
				expected = 0x07e0;
			} else {
				continue;
			}
			pixel = tiled_rgb565_index(x, y, TEST_WIDTH);
			if (map[pixel] != expected) {
				fprintf(stderr,
					"FAIL: painter-order mismatch at (%u,%u): got=0x%04x expected=0x%04x\n",
					x, y, map[pixel], expected);
				failures++;
				goto out_sync;
			}
		}
	}

	if (!depth_inside || !painter_inside)
		fail("depth triangle oracle did not classify pixels");
	else
		printf("DRAW depth: blue-near=%u red-painter=%u\n",
		       depth_inside, painter_inside);

	for (unsigned int test = 0; test < ARRAY_SIZE(depth_cases); test++) {
		unsigned int mode_inside = 0;
		bool mode_failed = false;

		for (unsigned int vertex = 0; vertex < 3; vertex++) {
			triangles[0].vertices[vertex].z =
				depth_cases[test].blue_depth;
			triangles[1].vertices[vertex].z =
				depth_cases[test].red_depth;
		}
		draw.depth.test_enable = 1;
		draw.depth.compare = depth_cases[test].compare;
		draw.depth.write_enable = depth_cases[test].write_enable;
		if (ioctl(fd, DRM_IOCTL_GCN_SUBMIT, &fill) ||
		    submit_triangle_depth(fd, &draw, sync.handle)) {
			fail("draw depth compare-mode coverage");
			goto out_sync;
		}

		for (unsigned int y = 0; y < TEST_HEIGHT; y++) {
			for (unsigned int x = 0; x < TEST_WIDTH; x++) {
				int classified = triangle_classify(&coverage, x, y,
							   margin);
				uint16_t expected;
				size_t pixel;

				if (classified > 0) {
					expected = depth_cases[test].expected;
					mode_inside++;
				} else if (classified < 0) {
					expected = 0x07e0;
				} else {
					continue;
				}
				pixel = tiled_rgb565_index(x, y, TEST_WIDTH);
				if (map[pixel] != expected) {
					fprintf(stderr,
						"FAIL: depth %s mismatch at (%u,%u): got=0x%04x expected=0x%04x\n",
						depth_cases[test].name, x, y,
						map[pixel], expected);
					failures++;
					mode_failed = true;
					break;
				}
			}
			if (mode_failed)
				break;
		}
		if (mode_failed)
			continue;
		if (!mode_inside) {
			fail("depth compare-mode oracle did not classify pixels");
			goto out_sync;
		}
		printf("DRAW depth %s: matched %u interior pixels\n",
		       depth_cases[test].name, mode_inside);
	}

out_sync:
	destroy.handle = sync.handle;
	if (ioctl(fd, DRM_IOCTL_SYNCOBJ_DESTROY, &destroy))
		fail("destroy depth-triangle syncobj");
out_ctx:
	free_ctx.id = ctx.id;
	if (ioctl(fd, DRM_IOCTL_GCN_CTX_FREE, &free_ctx))
		fail("free depth-triangle context");
out:
	if (map != MAP_FAILED && munmap(map, dst.size))
		fail("unmap depth-triangle destination");
	if (dst.handle && close_bo(fd, dst.handle))
		fail("close depth-triangle destination");
}

static void test_draw_triangle(int fd)
{
	struct drm_syncobj_create sync = {};
	struct drm_syncobj_destroy destroy = {};
	struct drm_gcn_ctx_create ctx = {};
	struct drm_gcn_ctx_free free_ctx = {};
	struct drm_gcn_gem_create dst = {};
	struct drm_gcn_submit fill = {
		.op = DRM_GCN_RENDER_OP_FILL_RGB565,
		.data = 0x07e0,
	};
	struct drm_syncobj_wait sync_wait = {};
	struct drm_gcn_draw_triangle draw = {
		.vertices = {
			{ 32, 32, DRM_GCN_RGBA8(0xff, 0, 0, 0xff) },
			{ 224, 48, DRM_GCN_RGBA8(0xff, 0, 0, 0xff) },
			{ 112, 224, DRM_GCN_RGBA8(0xff, 0, 0, 0xff) },
		},
	};
	uint16_t *map = MAP_FAILED;
	unsigned int checked_inside = 0;
	unsigned int checked_outside = 0;
	const int64_t edge_margin = 1024;
	int64_t area;

	if (create_bo(fd, &dst)) {
		fail("create triangle destination");
		return;
	}
	map = map_bo(fd, &dst);
	if (map == MAP_FAILED) {
		fail("map triangle destination");
		goto out;
	}
	if (ioctl(fd, DRM_IOCTL_GCN_CTX_CREATE, &ctx)) {
		fail("create triangle context");
		goto out;
	}
	if (ioctl(fd, DRM_IOCTL_SYNCOBJ_CREATE, &sync)) {
		fail("create triangle syncobj");
		goto out_ctx;
	}

	fill.ctx_id = ctx.id;
	fill.dst_handle = dst.handle;
	if (ioctl(fd, DRM_IOCTL_GCN_SUBMIT, &fill)) {
		fail("seed triangle background");
		goto out_sync;
	}

	draw.ctx_id = ctx.id;
	draw.dst_handle = dst.handle;
	draw.out_syncobj = sync.handle;
	draw.vertices[0].rgba &= ~0xffU;
	errno = 0;
	if (!ioctl(fd, DRM_IOCTL_GCN_DRAW_TRIANGLE, &draw) || errno != EINVAL)
		fail("transparent triangle should return EINVAL");
	draw.vertices[0].rgba |= 0xff;
	draw.vertices[1].x = TEST_WIDTH + 1;
	errno = 0;
	if (!ioctl(fd, DRM_IOCTL_GCN_DRAW_TRIANGLE, &draw) || errno != EINVAL)
		fail("out-of-bounds triangle should return EINVAL");
	draw.vertices[1].x = 224;
	draw.pad[0] = 1;
	errno = 0;
	if (!ioctl(fd, DRM_IOCTL_GCN_DRAW_TRIANGLE, &draw) || errno != EINVAL)
		fail("padded triangle should return EINVAL");
	draw.pad[0] = 0;
	draw.vertices[2] = draw.vertices[0];
	errno = 0;
	if (!ioctl(fd, DRM_IOCTL_GCN_DRAW_TRIANGLE, &draw) || errno != EINVAL)
		fail("degenerate triangle should return EINVAL");
	draw.vertices[2] = (struct drm_gcn_color_vertex) {
		112, 224, DRM_GCN_RGBA8(0xff, 0, 0, 0xff),
	};

	if (ioctl(fd, DRM_IOCTL_GCN_DRAW_TRIANGLE, &draw)) {
		fail("draw solid RGB565 triangle");
		goto out_sync;
	}
	{
		uint32_t sync_handle = sync.handle;

		sync_wait.handles = (uintptr_t)&sync_handle;
		sync_wait.timeout_nsec = (int64_t)(monotonic_ns() +
							  1000000000ULL);
		sync_wait.count_handles = 1;
		if (ioctl(fd, DRM_IOCTL_SYNCOBJ_WAIT, &sync_wait)) {
			fail("wait for triangle syncobj");
			goto out_sync;
		}
	}

	area = triangle_edge(&draw.vertices[0], &draw.vertices[1],
			     draw.vertices[2].x, draw.vertices[2].y);
	for (unsigned int y = 0; y < TEST_HEIGHT; y++) {
		for (unsigned int x = 0; x < TEST_WIDTH; x++) {
			int64_t edge0 = triangle_edge(&draw.vertices[0],
						      &draw.vertices[1], x, y);
			int64_t edge1 = triangle_edge(&draw.vertices[1],
						      &draw.vertices[2], x, y);
			int64_t edge2 = triangle_edge(&draw.vertices[2],
						      &draw.vertices[0], x, y);
			uint16_t expected;
			size_t pixel;

			if (area < 0) {
				edge0 = -edge0;
				edge1 = -edge1;
				edge2 = -edge2;
			}
			if (edge0 > edge_margin && edge1 > edge_margin &&
			    edge2 > edge_margin) {
				expected = 0xf800;
				checked_inside++;
			} else if (edge0 < -edge_margin || edge1 < -edge_margin ||
				   edge2 < -edge_margin) {
				expected = 0x07e0;
				checked_outside++;
			} else {
				continue;
			}

			pixel = tiled_rgb565_index(x, y, TEST_WIDTH);
			if (map[pixel] != expected) {
				fprintf(stderr,
					"FAIL: triangle mismatch at (%u,%u): got=0x%04x expected=0x%04x\n",
					x, y, map[pixel], expected);
				failures++;
				goto out_sync;
			}
		}
	}
	if (!checked_inside || !checked_outside)
		fail("triangle oracle did not classify pixels");
	else
		printf("DRAW: triangle matched %u interior and %u exterior RGB565 pixels\n",
		       checked_inside, checked_outside);

out_sync:
	destroy.handle = sync.handle;
	if (ioctl(fd, DRM_IOCTL_SYNCOBJ_DESTROY, &destroy))
		fail("destroy triangle syncobj");
out_ctx:
	free_ctx.id = ctx.id;
	if (ioctl(fd, DRM_IOCTL_GCN_CTX_FREE, &free_ctx))
		fail("free triangle context");
out:
	if (map != MAP_FAILED && munmap(map, dst.size))
		fail("unmap triangle destination");
	if (dst.handle && close_bo(fd, dst.handle))
		fail("close triangle destination");
}

static void test_draw_triangles(int fd)
{
	struct drm_syncobj_create sync = {};
	struct drm_syncobj_destroy destroy = {};
	struct drm_gcn_ctx_create ctx = {};
	struct drm_gcn_ctx_free free_ctx = {};
	struct drm_gcn_gem_create dst = {};
	struct drm_gcn_submit fill = {
		.op = DRM_GCN_RENDER_OP_FILL_RGB565,
		.data = 0x07e0,
	};
	struct drm_gcn_color_triangle triangles[2] = {
		{
			.vertices = {
				{ 16, 16, DRM_GCN_RGBA8(0xff, 0, 0, 0xff) },
				{ 112, 16, DRM_GCN_RGBA8(0xff, 0, 0, 0xff) },
				{ 16, 112, DRM_GCN_RGBA8(0xff, 0, 0, 0xff) },
			},
		},
		{
			.vertices = {
				{ 144, 144, DRM_GCN_RGBA8(0, 0, 0xff, 0xff) },
				{ 240, 144, DRM_GCN_RGBA8(0, 0, 0xff, 0xff) },
				{ 240, 240, DRM_GCN_RGBA8(0, 0, 0xff, 0xff) },
			},
		},
	};
	struct drm_gcn_draw_triangles draw = {
		.triangle_count = ARRAY_SIZE(triangles),
		.triangles_ptr = (uintptr_t)triangles,
	};
	struct drm_syncobj_wait sync_wait = {};
	uint16_t *map = MAP_FAILED;
	unsigned int checked_red = 0;
	unsigned int checked_blue = 0;
	unsigned int checked_background = 0;
	const int64_t edge_margin = 512;

	if (create_bo(fd, &dst)) {
		fail("create triangle-batch destination");
		return;
	}
	map = map_bo(fd, &dst);
	if (map == MAP_FAILED) {
		fail("map triangle-batch destination");
		goto out;
	}
	if (ioctl(fd, DRM_IOCTL_GCN_CTX_CREATE, &ctx)) {
		fail("create triangle-batch context");
		goto out;
	}
	if (ioctl(fd, DRM_IOCTL_SYNCOBJ_CREATE, &sync)) {
		fail("create triangle-batch syncobj");
		goto out_ctx;
	}

	fill.ctx_id = ctx.id;
	fill.dst_handle = dst.handle;
	if (ioctl(fd, DRM_IOCTL_GCN_SUBMIT, &fill)) {
		fail("seed triangle-batch background");
		goto out_sync;
	}

	draw.ctx_id = ctx.id;
	draw.dst_handle = dst.handle;
	draw.out_syncobj = sync.handle;
	draw.triangle_count = 0;
	errno = 0;
	if (!ioctl(fd, DRM_IOCTL_GCN_DRAW_TRIANGLES, &draw) || errno != EINVAL)
		fail("empty triangle batch should return EINVAL");
	draw.triangle_count = DRM_GCN_MAX_TRIANGLES + 1;
	errno = 0;
	if (!ioctl(fd, DRM_IOCTL_GCN_DRAW_TRIANGLES, &draw) || errno != EINVAL)
		fail("oversized triangle batch should return EINVAL");
	draw.triangle_count = ARRAY_SIZE(triangles);
	draw.triangles_ptr = 0;
	errno = 0;
	if (!ioctl(fd, DRM_IOCTL_GCN_DRAW_TRIANGLES, &draw) || errno != EINVAL)
		fail("null triangle-batch pointer should return EINVAL");
	draw.triangles_ptr = 1;
	errno = 0;
	if (!ioctl(fd, DRM_IOCTL_GCN_DRAW_TRIANGLES, &draw) || errno != EFAULT)
		fail("bad triangle-batch pointer should return EFAULT");
	draw.triangles_ptr = (uintptr_t)triangles;
	draw.pad[0] = 1;
	errno = 0;
	if (!ioctl(fd, DRM_IOCTL_GCN_DRAW_TRIANGLES, &draw) || errno != EINVAL)
		fail("padded triangle batch should return EINVAL");
	draw.pad[0] = 0;
	triangles[1].vertices[0].rgba &= ~0xffU;
	errno = 0;
	if (!ioctl(fd, DRM_IOCTL_GCN_DRAW_TRIANGLES, &draw) || errno != EINVAL)
		fail("invalid later triangle should reject complete batch");
	triangles[1].vertices[0].rgba |= 0xff;

	if (ioctl(fd, DRM_IOCTL_GCN_DRAW_TRIANGLES, &draw)) {
		fail("draw RGB565 triangle batch");
		goto out_sync;
	}
	{
		uint32_t sync_handle = sync.handle;

		sync_wait.handles = (uintptr_t)&sync_handle;
		sync_wait.timeout_nsec = (int64_t)(monotonic_ns() +
							  1000000000ULL);
		sync_wait.count_handles = 1;
		if (ioctl(fd, DRM_IOCTL_SYNCOBJ_WAIT, &sync_wait)) {
			fail("wait for triangle-batch syncobj");
			goto out_sync;
		}
	}

	for (unsigned int y = 0; y < TEST_HEIGHT; y++) {
		for (unsigned int x = 0; x < TEST_WIDTH; x++) {
			int red = triangle_classify(&triangles[0], x, y,
						    edge_margin);
			int blue = triangle_classify(&triangles[1], x, y,
						     edge_margin);
			uint16_t expected;
			size_t pixel;

			if (red > 0) {
				expected = 0xf800;
				checked_red++;
			} else if (blue > 0) {
				expected = 0x001f;
				checked_blue++;
			} else if (red < 0 && blue < 0) {
				expected = 0x07e0;
				checked_background++;
			} else {
				continue;
			}

			pixel = tiled_rgb565_index(x, y, TEST_WIDTH);
			if (map[pixel] != expected) {
				fprintf(stderr,
					"FAIL: triangle-batch mismatch at (%u,%u): got=0x%04x expected=0x%04x\n",
					x, y, map[pixel], expected);
				failures++;
				goto out_sync;
			}
		}
	}
	if (!checked_red || !checked_blue || !checked_background)
		fail("triangle-batch oracle did not classify pixels");
	else
		printf("DRAW: batch matched %u red, %u blue, and %u background RGB565 pixels\n",
		       checked_red, checked_blue, checked_background);

out_sync:
	destroy.handle = sync.handle;
	if (ioctl(fd, DRM_IOCTL_SYNCOBJ_DESTROY, &destroy))
		fail("destroy triangle-batch syncobj");
out_ctx:
	free_ctx.id = ctx.id;
	if (ioctl(fd, DRM_IOCTL_GCN_CTX_FREE, &free_ctx))
		fail("free triangle-batch context");
out:
	if (map != MAP_FAILED && munmap(map, dst.size))
		fail("unmap triangle-batch destination");
	if (dst.handle && close_bo(fd, dst.handle))
		fail("close triangle-batch destination");
}

static void test_draw_textured_triangles(int fd)
{
	const uint16_t sentinel = 0x39e7;
	struct drm_syncobj_create sync = {};
	struct drm_syncobj_destroy destroy = {};
	struct drm_gcn_ctx_create ctx = {};
	struct drm_gcn_ctx_free free_ctx = {};
	struct drm_gcn_gem_create src = {};
	struct drm_gcn_gem_create dst = {};
	struct drm_gcn_texture_triangle triangles[2] = {
		{
			.vertices = {
				{ 0, 0, 0, 0 },
				{ TEST_WIDTH, 0, TEST_WIDTH, 0 },
				{ 0, TEST_HEIGHT, 0, TEST_HEIGHT },
			},
		},
		{
			.vertices = {
				{ TEST_WIDTH, 0, TEST_WIDTH, 0 },
				{ TEST_WIDTH, TEST_HEIGHT,
				  TEST_WIDTH, TEST_HEIGHT },
				{ 0, TEST_HEIGHT, 0, TEST_HEIGHT },
			},
		},
	};
	struct drm_gcn_draw_textured_triangles draw = {
		.triangle_count = ARRAY_SIZE(triangles),
		.triangles_ptr = (uintptr_t)triangles,
		.state = {
			.viewport_width = TEST_WIDTH,
			.viewport_height = TEST_HEIGHT,
			.scissor_x = 16,
			.scissor_y = 20,
			.scissor_width = TEST_WIDTH - 32,
			.scissor_height = TEST_HEIGHT - 40,
			.blend_mode = DRM_GCN_BLEND_NONE,
		},
	};
	struct drm_syncobj_wait sync_wait = {};
	uint16_t *src_map = MAP_FAILED;
	uint16_t *dst_map = MAP_FAILED;
	unsigned int checked_texture = 0;
	unsigned int checked_preserved = 0;

	if (create_bo(fd, &src) || create_bo(fd, &dst)) {
		fail("create textured-triangle objects");
		goto out;
	}
	src_map = map_bo(fd, &src);
	dst_map = map_bo(fd, &dst);
	if (src_map == MAP_FAILED || dst_map == MAP_FAILED) {
		fail("map textured-triangle objects");
		goto out;
	}
	if (ioctl(fd, DRM_IOCTL_GCN_CTX_CREATE, &ctx)) {
		fail("create textured-triangle context");
		goto out;
	}
	if (ioctl(fd, DRM_IOCTL_SYNCOBJ_CREATE, &sync)) {
		fail("create textured-triangle syncobj");
		goto out_ctx;
	}

	for (unsigned int y = 0; y < TEST_HEIGHT; y++) {
		for (unsigned int x = 0; x < TEST_WIDTH; x++) {
			size_t pixel = tiled_rgb565_index(x, y, TEST_WIDTH);

			src_map[pixel] = source_pattern(x, y);
			dst_map[pixel] = sentinel;
		}
	}

	draw.ctx_id = ctx.id;
	draw.src_handle = src.handle;
	draw.dst_handle = dst.handle;
	draw.out_syncobj = sync.handle;

	draw.src_handle = dst.handle;
	errno = 0;
	if (!ioctl(fd, DRM_IOCTL_GCN_DRAW_TEXTURED_TRIANGLES, &draw) ||
	    errno != EINVAL)
		fail("aliased textured draw should return EINVAL");
	draw.src_handle = src.handle;
	draw.state.blend_mode = DRM_GCN_BLEND_SRC_ALPHA;
	errno = 0;
	if (!ioctl(fd, DRM_IOCTL_GCN_DRAW_TEXTURED_TRIANGLES, &draw) ||
	    errno != EINVAL)
		fail("blended textured draw should return EINVAL");
	draw.state.blend_mode = DRM_GCN_BLEND_NONE;
	triangles[1].vertices[1].s = TEST_WIDTH + 1;
	errno = 0;
	if (!ioctl(fd, DRM_IOCTL_GCN_DRAW_TEXTURED_TRIANGLES, &draw) ||
	    errno != EINVAL)
		fail("out-of-range texture coordinate should return EINVAL");
	triangles[1].vertices[1].s = TEST_WIDTH;
	draw.pad = 1;
	errno = 0;
	if (!ioctl(fd, DRM_IOCTL_GCN_DRAW_TEXTURED_TRIANGLES, &draw) ||
	    errno != EINVAL)
		fail("padded textured draw should return EINVAL");
	draw.pad = 0;
	draw.triangles_ptr = 1;
	errno = 0;
	if (!ioctl(fd, DRM_IOCTL_GCN_DRAW_TEXTURED_TRIANGLES, &draw) ||
	    errno != EFAULT)
		fail("bad textured-triangle pointer should return EFAULT");
	draw.triangles_ptr = (uintptr_t)triangles;

	if (ioctl(fd, DRM_IOCTL_GCN_DRAW_TEXTURED_TRIANGLES, &draw)) {
		fail("draw textured RGB565 triangle batch");
		goto out_sync;
	}
	{
		uint32_t sync_handle = sync.handle;

		sync_wait.handles = (uintptr_t)&sync_handle;
		sync_wait.timeout_nsec = (int64_t)(monotonic_ns() +
							  1000000000ULL);
		sync_wait.count_handles = 1;
		if (ioctl(fd, DRM_IOCTL_SYNCOBJ_WAIT, &sync_wait)) {
			fail("wait for textured-triangle syncobj");
			goto out_sync;
		}
	}

	for (unsigned int y = 0; y < TEST_HEIGHT; y++) {
		for (unsigned int x = 0; x < TEST_WIDTH; x++) {
			bool inside = x >= draw.state.scissor_x &&
				x < draw.state.scissor_x + draw.state.scissor_width &&
				y >= draw.state.scissor_y &&
				y < draw.state.scissor_y + draw.state.scissor_height;
			uint16_t expected = inside ? source_pattern(x, y) : sentinel;
			size_t pixel = tiled_rgb565_index(x, y, TEST_WIDTH);

			if (inside)
				checked_texture++;
			else
				checked_preserved++;
			if (dst_map[pixel] != expected) {
				fprintf(stderr,
					"FAIL: textured-triangle mismatch at (%u,%u): got=0x%04x expected=0x%04x\n",
					x, y, dst_map[pixel], expected);
				failures++;
				goto out_sync;
			}
		}
	}
	printf("DRAW: textured batch matched %u sampled and %u preserved RGB565 pixels\n",
	       checked_texture, checked_preserved);

out_sync:
	destroy.handle = sync.handle;
	if (ioctl(fd, DRM_IOCTL_SYNCOBJ_DESTROY, &destroy))
		fail("destroy textured-triangle syncobj");
out_ctx:
	free_ctx.id = ctx.id;
	if (ioctl(fd, DRM_IOCTL_GCN_CTX_FREE, &free_ctx))
		fail("free textured-triangle context");
out:
	if (dst_map != MAP_FAILED && munmap(dst_map, dst.size))
		fail("unmap textured-triangle destination");
	if (src_map != MAP_FAILED && munmap(src_map, src.size))
		fail("unmap textured-triangle source");
	if (dst.handle && close_bo(fd, dst.handle))
		fail("close textured-triangle destination");
	if (src.handle && close_bo(fd, src.handle))
		fail("close textured-triangle source");
}

static void test_draw_indexed_triangles(int fd)
{
	const uint16_t sentinel = 0x07e0;
	const uint16_t fill = 0xf800;
	struct drm_syncobj_create sync = {};
	struct drm_syncobj_destroy destroy = {};
	struct drm_gcn_ctx_create ctx = {};
	struct drm_gcn_ctx_free free_ctx = {};
	struct drm_gcn_gem_create dst = {};
	struct drm_gcn_color_vertex vertices[5] = {
		{ 0, 0, DRM_GCN_RGBA8(0, 0, 255, 255) },
		{ 32, 40, DRM_GCN_RGBA8(255, 0, 0, 255) },
		{ 224, 40, DRM_GCN_RGBA8(255, 0, 0, 255) },
		{ 224, 216, DRM_GCN_RGBA8(255, 0, 0, 255) },
		{ 32, 216, DRM_GCN_RGBA8(255, 0, 0, 255) },
	};
	uint16_t indices[6] = { 1, 2, 3, 1, 3, 4 };
	struct drm_gcn_draw_indexed_triangles draw = {
		.vertex_count = ARRAY_SIZE(vertices),
		.triangle_count = 2,
		.vertices_ptr = (uintptr_t)vertices,
		.indices_ptr = (uintptr_t)indices,
		.state = {
			.viewport_width = TEST_WIDTH,
			.viewport_height = TEST_HEIGHT,
			.scissor_width = TEST_WIDTH,
			.scissor_height = TEST_HEIGHT,
			.blend_mode = DRM_GCN_BLEND_NONE,
		},
	};
	struct drm_syncobj_wait sync_wait = {};
	uint16_t *dst_map = MAP_FAILED;
	unsigned int checked_fill = 0;
	unsigned int checked_preserved = 0;

	if (create_bo(fd, &dst)) {
		fail("create indexed-triangle destination");
		goto out;
	}
	dst_map = map_bo(fd, &dst);
	if (dst_map == MAP_FAILED) {
		fail("map indexed-triangle destination");
		goto out;
	}
	if (ioctl(fd, DRM_IOCTL_GCN_CTX_CREATE, &ctx)) {
		fail("create indexed-triangle context");
		goto out;
	}
	if (ioctl(fd, DRM_IOCTL_SYNCOBJ_CREATE, &sync)) {
		fail("create indexed-triangle syncobj");
		goto out_ctx;
	}

	for (unsigned int y = 0; y < TEST_HEIGHT; y++) {
		for (unsigned int x = 0; x < TEST_WIDTH; x++)
			dst_map[tiled_rgb565_index(x, y, TEST_WIDTH)] = sentinel;
	}
	draw.ctx_id = ctx.id;
	draw.dst_handle = dst.handle;
	draw.out_syncobj = sync.handle;

	indices[0] = ARRAY_SIZE(vertices);
	errno = 0;
	if (!ioctl(fd, DRM_IOCTL_GCN_DRAW_INDEXED_TRIANGLES, &draw) ||
	    errno != EINVAL)
		fail("out-of-range triangle index should return EINVAL");
	indices[0] = 1;
	indices[1] = 1;
	errno = 0;
	if (!ioctl(fd, DRM_IOCTL_GCN_DRAW_INDEXED_TRIANGLES, &draw) ||
	    errno != EINVAL)
		fail("degenerate indexed triangle should return EINVAL");
	indices[1] = 2;
	draw.pad = 1;
	errno = 0;
	if (!ioctl(fd, DRM_IOCTL_GCN_DRAW_INDEXED_TRIANGLES, &draw) ||
	    errno != EINVAL)
		fail("padded indexed draw should return EINVAL");
	draw.pad = 0;
	draw.vertices_ptr = 1;
	errno = 0;
	if (!ioctl(fd, DRM_IOCTL_GCN_DRAW_INDEXED_TRIANGLES, &draw) ||
	    errno != EFAULT)
		fail("bad indexed vertex pointer should return EFAULT");
	draw.vertices_ptr = (uintptr_t)vertices;
	draw.indices_ptr = 1;
	errno = 0;
	if (!ioctl(fd, DRM_IOCTL_GCN_DRAW_INDEXED_TRIANGLES, &draw) ||
	    errno != EFAULT)
		fail("bad triangle-index pointer should return EFAULT");
	draw.indices_ptr = (uintptr_t)indices;

	if (ioctl(fd, DRM_IOCTL_GCN_DRAW_INDEXED_TRIANGLES, &draw)) {
		fail("draw indexed RGB565 triangle batch");
		goto out_sync;
	}
	{
		uint32_t sync_handle = sync.handle;

		sync_wait.handles = (uintptr_t)&sync_handle;
		sync_wait.timeout_nsec = (int64_t)(monotonic_ns() +
							  1000000000ULL);
		sync_wait.count_handles = 1;
		if (ioctl(fd, DRM_IOCTL_SYNCOBJ_WAIT, &sync_wait)) {
			fail("wait for indexed-triangle syncobj");
			goto out_sync;
		}
	}

	for (unsigned int y = 0; y < TEST_HEIGHT; y++) {
		for (unsigned int x = 0; x < TEST_WIDTH; x++) {
			bool inside = x >= 34 && x < 222 && y >= 42 && y < 214;
			bool outside = x < 30 || x >= 226 || y < 38 || y >= 218;
			uint16_t expected;
			size_t pixel;

			if (!inside && !outside)
				continue;
			expected = inside ? fill : sentinel;
			pixel = tiled_rgb565_index(x, y, TEST_WIDTH);
			if (inside)
				checked_fill++;
			else
				checked_preserved++;
			if (dst_map[pixel] != expected) {
				fprintf(stderr,
					"FAIL: indexed-triangle mismatch at (%u,%u): got=0x%04x expected=0x%04x\n",
					x, y, dst_map[pixel], expected);
				failures++;
				goto out_sync;
			}
		}
	}
	printf("DRAW: indexed batch matched %u filled and %u preserved RGB565 pixels\n",
	       checked_fill, checked_preserved);

out_sync:
	destroy.handle = sync.handle;
	if (ioctl(fd, DRM_IOCTL_SYNCOBJ_DESTROY, &destroy))
		fail("destroy indexed-triangle syncobj");
out_ctx:
	free_ctx.id = ctx.id;
	if (ioctl(fd, DRM_IOCTL_GCN_CTX_FREE, &free_ctx))
		fail("free indexed-triangle context");
out:
	if (dst_map != MAP_FAILED && munmap(dst_map, dst.size))
		fail("unmap indexed-triangle destination");
	if (dst.handle && close_bo(fd, dst.handle))
		fail("close indexed-triangle destination");
}

static void test_draw_indexed_textured_triangles(int fd)
{
	const uint16_t sentinel = 0x39e7;
	struct drm_syncobj_create sync = {};
	struct drm_syncobj_destroy destroy = {};
	struct drm_gcn_ctx_create ctx = {};
	struct drm_gcn_ctx_free free_ctx = {};
	struct drm_gcn_gem_create src = {};
	struct drm_gcn_gem_create dst = {};
	struct drm_gcn_texture_vertex vertices[5] = {
		{ TEST_WIDTH / 2, TEST_HEIGHT / 2, TEST_WIDTH, 0 },
		{ 0, 0, 0, 0 },
		{ TEST_WIDTH, 0, TEST_WIDTH, 0 },
		{ TEST_WIDTH, TEST_HEIGHT, TEST_WIDTH, TEST_HEIGHT },
		{ 0, TEST_HEIGHT, 0, TEST_HEIGHT },
	};
	uint16_t indices[6] = { 1, 2, 3, 1, 3, 4 };
	struct drm_gcn_draw_indexed_textured draw = {
		.vertex_count = ARRAY_SIZE(vertices),
		.triangle_count = 2,
		.vertices_ptr = (uintptr_t)vertices,
		.indices_ptr = (uintptr_t)indices,
		.state = {
			.viewport_width = TEST_WIDTH,
			.viewport_height = TEST_HEIGHT,
			.scissor_x = 24,
			.scissor_y = 28,
			.scissor_width = TEST_WIDTH - 48,
			.scissor_height = TEST_HEIGHT - 56,
			.blend_mode = DRM_GCN_BLEND_NONE,
		},
	};
	struct drm_syncobj_wait sync_wait = {};
	uint16_t *src_map = MAP_FAILED;
	uint16_t *dst_map = MAP_FAILED;
	unsigned int checked_texture = 0;
	unsigned int checked_preserved = 0;

	if (create_bo(fd, &src) || create_bo(fd, &dst)) {
		fail("create indexed-textured objects");
		goto out;
	}
	src_map = map_bo(fd, &src);
	dst_map = map_bo(fd, &dst);
	if (src_map == MAP_FAILED || dst_map == MAP_FAILED) {
		fail("map indexed-textured objects");
		goto out;
	}
	if (ioctl(fd, DRM_IOCTL_GCN_CTX_CREATE, &ctx)) {
		fail("create indexed-textured context");
		goto out;
	}
	if (ioctl(fd, DRM_IOCTL_SYNCOBJ_CREATE, &sync)) {
		fail("create indexed-textured syncobj");
		goto out_ctx;
	}

	for (unsigned int y = 0; y < TEST_HEIGHT; y++) {
		for (unsigned int x = 0; x < TEST_WIDTH; x++) {
			size_t pixel = tiled_rgb565_index(x, y, TEST_WIDTH);

			src_map[pixel] = source_pattern(x, y);
			dst_map[pixel] = sentinel;
		}
	}

	draw.ctx_id = ctx.id;
	draw.src_handle = src.handle;
	draw.dst_handle = dst.handle;
	draw.out_syncobj = sync.handle;

	draw.src_handle = dst.handle;
	errno = 0;
	if (!ioctl(fd, DRM_IOCTL_GCN_DRAW_INDEXED_TEXTURED_TRIANGLES, &draw) ||
	    errno != EINVAL)
		fail("aliased indexed-textured draw should return EINVAL");
	draw.src_handle = src.handle;
	draw.state.blend_mode = DRM_GCN_BLEND_SRC_ALPHA;
	errno = 0;
	if (!ioctl(fd, DRM_IOCTL_GCN_DRAW_INDEXED_TEXTURED_TRIANGLES, &draw) ||
	    errno != EINVAL)
		fail("blended indexed-textured draw should return EINVAL");
	draw.state.blend_mode = DRM_GCN_BLEND_NONE;
	indices[0] = ARRAY_SIZE(vertices);
	errno = 0;
	if (!ioctl(fd, DRM_IOCTL_GCN_DRAW_INDEXED_TEXTURED_TRIANGLES, &draw) ||
	    errno != EINVAL)
		fail("out-of-range indexed-textured index should return EINVAL");
	indices[0] = 1;
	indices[1] = 1;
	errno = 0;
	if (!ioctl(fd, DRM_IOCTL_GCN_DRAW_INDEXED_TEXTURED_TRIANGLES, &draw) ||
	    errno != EINVAL)
		fail("degenerate indexed-textured triangle should return EINVAL");
	indices[1] = 2;
	vertices[4].t = TEST_HEIGHT + 1;
	errno = 0;
	if (!ioctl(fd, DRM_IOCTL_GCN_DRAW_INDEXED_TEXTURED_TRIANGLES, &draw) ||
	    errno != EINVAL)
		fail("out-of-range indexed texcoord should return EINVAL");
	vertices[4].t = TEST_HEIGHT;
	draw.pad0 = 1;
	errno = 0;
	if (!ioctl(fd, DRM_IOCTL_GCN_DRAW_INDEXED_TEXTURED_TRIANGLES, &draw) ||
	    errno != EINVAL)
		fail("pad0 indexed-textured draw should return EINVAL");
	draw.pad0 = 0;
	draw.pad1 = 1;
	errno = 0;
	if (!ioctl(fd, DRM_IOCTL_GCN_DRAW_INDEXED_TEXTURED_TRIANGLES, &draw) ||
	    errno != EINVAL)
		fail("pad1 indexed-textured draw should return EINVAL");
	draw.pad1 = 0;
	draw.vertices_ptr = 1;
	errno = 0;
	if (!ioctl(fd, DRM_IOCTL_GCN_DRAW_INDEXED_TEXTURED_TRIANGLES, &draw) ||
	    errno != EFAULT)
		fail("bad indexed-textured vertex pointer should return EFAULT");
	draw.vertices_ptr = (uintptr_t)vertices;
	draw.indices_ptr = 1;
	errno = 0;
	if (!ioctl(fd, DRM_IOCTL_GCN_DRAW_INDEXED_TEXTURED_TRIANGLES, &draw) ||
	    errno != EFAULT)
		fail("bad indexed-textured index pointer should return EFAULT");
	draw.indices_ptr = (uintptr_t)indices;

	if (ioctl(fd, DRM_IOCTL_GCN_DRAW_INDEXED_TEXTURED_TRIANGLES, &draw)) {
		fail("draw indexed-textured RGB565 triangle batch");
		goto out_sync;
	}
	{
		uint32_t sync_handle = sync.handle;

		sync_wait.handles = (uintptr_t)&sync_handle;
		sync_wait.timeout_nsec = (int64_t)(monotonic_ns() +
							  1000000000ULL);
		sync_wait.count_handles = 1;
		if (ioctl(fd, DRM_IOCTL_SYNCOBJ_WAIT, &sync_wait)) {
			fail("wait for indexed-textured syncobj");
			goto out_sync;
		}
	}

	for (unsigned int y = 0; y < TEST_HEIGHT; y++) {
		for (unsigned int x = 0; x < TEST_WIDTH; x++) {
			bool inside = x >= draw.state.scissor_x &&
				x < draw.state.scissor_x + draw.state.scissor_width &&
				y >= draw.state.scissor_y &&
				y < draw.state.scissor_y + draw.state.scissor_height;
			uint16_t expected = inside ? source_pattern(x, y) : sentinel;
			size_t pixel = tiled_rgb565_index(x, y, TEST_WIDTH);

			if (inside)
				checked_texture++;
			else
				checked_preserved++;
			if (dst_map[pixel] != expected) {
				fprintf(stderr,
					"FAIL: indexed-textured mismatch at (%u,%u): got=0x%04x expected=0x%04x\n",
					x, y, dst_map[pixel], expected);
				failures++;
				goto out_sync;
			}
		}
	}
	printf("DRAW: indexed textured batch matched %u sampled and %u preserved RGB565 pixels\n",
	       checked_texture, checked_preserved);

out_sync:
	destroy.handle = sync.handle;
	if (ioctl(fd, DRM_IOCTL_SYNCOBJ_DESTROY, &destroy))
		fail("destroy indexed-textured syncobj");
out_ctx:
	free_ctx.id = ctx.id;
	if (ioctl(fd, DRM_IOCTL_GCN_CTX_FREE, &free_ctx))
		fail("free indexed-textured context");
out:
	if (dst_map != MAP_FAILED && munmap(dst_map, dst.size))
		fail("unmap indexed-textured destination");
	if (src_map != MAP_FAILED && munmap(src_map, src.size))
		fail("unmap indexed-textured source");
	if (dst.handle && close_bo(fd, dst.handle))
		fail("close indexed-textured destination");
	if (src.handle && close_bo(fd, src.handle))
		fail("close indexed-textured source");
}

static void test_draw_indexed_textured_depth(int fd)
{
	const uint16_t sentinel = 0x39e7;
	const uint16_t red = 0xf800;
	const uint16_t blue = 0x001f;
	struct drm_syncobj_create sync = {};
	struct drm_syncobj_destroy destroy = {};
	struct drm_gcn_ctx_create ctx = {};
	struct drm_gcn_ctx_free free_ctx = {};
	struct drm_gcn_gem_create src = {};
	struct drm_gcn_gem_create dst = {};
	struct drm_gcn_texture_depth_vertex vertices[9] = {
		{ TEST_WIDTH / 2, TEST_HEIGHT / 2, DRM_GCN_DEPTH_MAX, 0, 0 },
		{ 0, 0, DRM_GCN_DEPTH_MAX / 8, 64, 64 },
		{ TEST_WIDTH, 0, DRM_GCN_DEPTH_MAX / 8, 64, 64 },
		{ TEST_WIDTH, TEST_HEIGHT, DRM_GCN_DEPTH_MAX / 8, 64, 64 },
		{ 0, TEST_HEIGHT, DRM_GCN_DEPTH_MAX / 8, 64, 64 },
		{ 0, 0, DRM_GCN_DEPTH_MAX * 3 / 4, 192, 64 },
		{ TEST_WIDTH, 0, DRM_GCN_DEPTH_MAX * 3 / 4, 192, 64 },
		{ TEST_WIDTH, TEST_HEIGHT, DRM_GCN_DEPTH_MAX * 3 / 4,
		  192, 64 },
		{ 0, TEST_HEIGHT, DRM_GCN_DEPTH_MAX * 3 / 4, 192, 64 },
	};
	uint16_t indices[12] = { 1, 2, 3, 1, 3, 4, 5, 6, 7, 5, 7, 8 };
	struct drm_gcn_draw_indexed_textured_depth draw = {
		.vertex_count = ARRAY_SIZE(vertices),
		.triangle_count = 4,
		.vertices_ptr = (uintptr_t)vertices,
		.indices_ptr = (uintptr_t)indices,
		.state = {
			.viewport_width = TEST_WIDTH,
			.viewport_height = TEST_HEIGHT,
			.scissor_x = 20,
			.scissor_y = 24,
			.scissor_width = 216,
			.scissor_height = 208,
			.blend_mode = DRM_GCN_BLEND_NONE,
		},
		.depth = {
			.test_enable = 1,
			.compare = DRM_GCN_DEPTH_LESS,
			.write_enable = 1,
		},
	};
	uint16_t *src_map = MAP_FAILED;
	uint16_t *dst_map = MAP_FAILED;
	unsigned int checked_depth = 0;
	unsigned int checked_painter = 0;
	unsigned int checked_preserved = 0;

	if (create_bo(fd, &src) || create_bo(fd, &dst)) {
		fail("create indexed-textured-depth objects");
		goto out;
	}
	src_map = map_bo(fd, &src);
	dst_map = map_bo(fd, &dst);
	if (src_map == MAP_FAILED || dst_map == MAP_FAILED) {
		fail("map indexed-textured-depth objects");
		goto out;
	}
	if (ioctl(fd, DRM_IOCTL_GCN_CTX_CREATE, &ctx)) {
		fail("create indexed-textured-depth context");
		goto out;
	}
	if (ioctl(fd, DRM_IOCTL_SYNCOBJ_CREATE, &sync)) {
		fail("create indexed-textured-depth syncobj");
		goto out_ctx;
	}

	for (unsigned int y = 0; y < TEST_HEIGHT; y++) {
		for (unsigned int x = 0; x < TEST_WIDTH; x++) {
			size_t pixel = tiled_rgb565_index(x, y, TEST_WIDTH);

			src_map[pixel] = x < TEST_WIDTH / 2 ? red : blue;
			dst_map[pixel] = sentinel;
		}
	}

	draw.ctx_id = ctx.id;
	draw.src_handle = src.handle;
	draw.dst_handle = dst.handle;
	draw.out_syncobj = sync.handle;

	draw.src_handle = dst.handle;
	errno = 0;
	if (!ioctl(fd, DRM_IOCTL_GCN_DRAW_INDEXED_TEXTURED_DEPTH, &draw) ||
	    errno != EINVAL)
		fail("aliased indexed-textured-depth draw should return EINVAL");
	draw.src_handle = src.handle;
	draw.state.blend_mode = DRM_GCN_BLEND_SRC_ALPHA;
	errno = 0;
	if (!ioctl(fd, DRM_IOCTL_GCN_DRAW_INDEXED_TEXTURED_DEPTH, &draw) ||
	    errno != EINVAL)
		fail("blended indexed-textured-depth draw should return EINVAL");
	draw.state.blend_mode = DRM_GCN_BLEND_NONE;
	indices[0] = ARRAY_SIZE(vertices);
	errno = 0;
	if (!ioctl(fd, DRM_IOCTL_GCN_DRAW_INDEXED_TEXTURED_DEPTH, &draw) ||
	    errno != EINVAL)
		fail("out-of-range indexed-textured-depth index should return EINVAL");
	indices[0] = 1;
	indices[1] = 1;
	errno = 0;
	if (!ioctl(fd, DRM_IOCTL_GCN_DRAW_INDEXED_TEXTURED_DEPTH, &draw) ||
	    errno != EINVAL)
		fail("degenerate indexed-textured-depth draw should return EINVAL");
	indices[1] = 2;
	vertices[8].z = DRM_GCN_DEPTH_MAX + 1;
	errno = 0;
	if (!ioctl(fd, DRM_IOCTL_GCN_DRAW_INDEXED_TEXTURED_DEPTH, &draw) ||
	    errno != EINVAL)
		fail("out-of-range indexed-textured depth should return EINVAL");
	vertices[8].z = DRM_GCN_DEPTH_MAX * 3 / 4;
	vertices[8].s = TEST_WIDTH + 1;
	errno = 0;
	if (!ioctl(fd, DRM_IOCTL_GCN_DRAW_INDEXED_TEXTURED_DEPTH, &draw) ||
	    errno != EINVAL)
		fail("out-of-range indexed-textured-depth texcoord should return EINVAL");
	vertices[8].s = 192;
	draw.depth.test_enable = 2;
	errno = 0;
	if (!ioctl(fd, DRM_IOCTL_GCN_DRAW_INDEXED_TEXTURED_DEPTH, &draw) ||
	    errno != EINVAL)
		fail("nonboolean indexed-textured depth enable should return EINVAL");
	draw.depth.test_enable = 1;
	draw.depth.compare = DRM_GCN_DEPTH_ALWAYS + 1;
	errno = 0;
	if (!ioctl(fd, DRM_IOCTL_GCN_DRAW_INDEXED_TEXTURED_DEPTH, &draw) ||
	    errno != EINVAL)
		fail("invalid indexed-textured depth compare should return EINVAL");
	draw.depth.compare = DRM_GCN_DEPTH_LESS;
	draw.depth.write_enable = 2;
	errno = 0;
	if (!ioctl(fd, DRM_IOCTL_GCN_DRAW_INDEXED_TEXTURED_DEPTH, &draw) ||
	    errno != EINVAL)
		fail("nonboolean indexed-textured depth write should return EINVAL");
	draw.depth.write_enable = 1;
	draw.depth.pad = 1;
	errno = 0;
	if (!ioctl(fd, DRM_IOCTL_GCN_DRAW_INDEXED_TEXTURED_DEPTH, &draw) ||
	    errno != EINVAL)
		fail("indexed-textured depth padding should return EINVAL");
	draw.depth.pad = 0;
	draw.pad0 = 1;
	errno = 0;
	if (!ioctl(fd, DRM_IOCTL_GCN_DRAW_INDEXED_TEXTURED_DEPTH, &draw) ||
	    errno != EINVAL)
		fail("pad0 indexed-textured-depth draw should return EINVAL");
	draw.pad0 = 0;
	draw.pad1 = 1;
	errno = 0;
	if (!ioctl(fd, DRM_IOCTL_GCN_DRAW_INDEXED_TEXTURED_DEPTH, &draw) ||
	    errno != EINVAL)
		fail("pad1 indexed-textured-depth draw should return EINVAL");
	draw.pad1 = 0;
	draw.vertices_ptr = 1;
	errno = 0;
	if (!ioctl(fd, DRM_IOCTL_GCN_DRAW_INDEXED_TEXTURED_DEPTH, &draw) ||
	    errno != EFAULT)
		fail("bad indexed-textured-depth vertex pointer should return EFAULT");
	draw.vertices_ptr = (uintptr_t)vertices;
	draw.indices_ptr = 1;
	errno = 0;
	if (!ioctl(fd, DRM_IOCTL_GCN_DRAW_INDEXED_TEXTURED_DEPTH, &draw) ||
	    errno != EFAULT)
		fail("bad indexed-textured-depth index pointer should return EFAULT");
	draw.indices_ptr = (uintptr_t)indices;

	if (submit_itex_depth(fd, &draw, sync.handle)) {
		fail("draw indexed-textured depth-tested batch");
		goto out_sync;
	}
	for (unsigned int y = 0; y < TEST_HEIGHT; y++) {
		for (unsigned int x = 0; x < TEST_WIDTH; x++) {
			bool inside = x >= draw.state.scissor_x &&
				x < draw.state.scissor_x + draw.state.scissor_width &&
				y >= draw.state.scissor_y &&
				y < draw.state.scissor_y + draw.state.scissor_height;
			uint16_t expected = inside ? red : sentinel;
			size_t pixel = tiled_rgb565_index(x, y, TEST_WIDTH);

			if (inside)
				checked_depth++;
			else
				checked_preserved++;
			if (dst_map[pixel] != expected) {
				fprintf(stderr,
					"FAIL: indexed-textured depth mismatch at (%u,%u): got=0x%04x expected=0x%04x\n",
					x, y, dst_map[pixel], expected);
				failures++;
				goto out_sync;
			}
		}
	}

	for (unsigned int y = 0; y < TEST_HEIGHT; y++)
		for (unsigned int x = 0; x < TEST_WIDTH; x++)
			dst_map[tiled_rgb565_index(x, y, TEST_WIDTH)] = sentinel;
	draw.depth.test_enable = 0;
	draw.depth.compare = DRM_GCN_DEPTH_ALWAYS;
	draw.depth.write_enable = 0;
	if (submit_itex_depth(fd, &draw, sync.handle)) {
		fail("draw indexed-textured painter-order control");
		goto out_sync;
	}
	for (unsigned int y = draw.state.scissor_y;
	     y < draw.state.scissor_y + draw.state.scissor_height; y++) {
		for (unsigned int x = draw.state.scissor_x;
		     x < draw.state.scissor_x + draw.state.scissor_width; x++) {
			size_t pixel = tiled_rgb565_index(x, y, TEST_WIDTH);

			checked_painter++;
			if (dst_map[pixel] != blue) {
				fprintf(stderr,
					"FAIL: indexed-textured painter mismatch at (%u,%u): got=0x%04x expected=0x%04x\n",
					x, y, dst_map[pixel], blue);
				failures++;
				goto out_sync;
			}
		}
	}
	printf("DRAW itex depth: near=%u painter=%u preserved=%u\n",
	       checked_depth, checked_painter, checked_preserved);

out_sync:
	destroy.handle = sync.handle;
	if (ioctl(fd, DRM_IOCTL_SYNCOBJ_DESTROY, &destroy))
		fail("destroy indexed-textured-depth syncobj");
out_ctx:
	free_ctx.id = ctx.id;
	if (ioctl(fd, DRM_IOCTL_GCN_CTX_FREE, &free_ctx))
		fail("free indexed-textured-depth context");
out:
	if (dst_map != MAP_FAILED && munmap(dst_map, dst.size))
		fail("unmap indexed-textured-depth destination");
	if (src_map != MAP_FAILED && munmap(src_map, src.size))
		fail("unmap indexed-textured-depth source");
	if (dst.handle && close_bo(fd, dst.handle))
		fail("close indexed-textured-depth destination");
	if (src.handle && close_bo(fd, src.handle))
		fail("close indexed-textured-depth source");
}

static void test_draw_indexed_fixed(int fd, bool cull_supported)
{
	const uint16_t sentinel = 0x39e7;
	const uint16_t cyan = 0x07ff;
	const uint16_t yellow = 0xffe0;
	const uint16_t green = 0x07e0;
	const uint16_t red = 0xf800;
	struct drm_syncobj_create sync = {};
	struct drm_syncobj_destroy destroy = {};
	struct drm_gcn_ctx_create ctx = {};
	struct drm_gcn_ctx_free free_ctx = {};
	struct drm_gcn_gem_create src = {};
	struct drm_gcn_gem_create dst = {};
	struct drm_gcn_fixed_vertex vertices[9] = {
		{ TEST_WIDTH / 2, TEST_HEIGHT / 2, DRM_GCN_DEPTH_MAX,
		  DRM_GCN_RGBA8(0, 0, 0xff, 0xff), 64, 64 },
		{ 0, 0, DRM_GCN_DEPTH_MAX / 8,
		  DRM_GCN_RGBA8(0, 0xff, 0xff, 0xff), 0, 0 },
		{ TEST_WIDTH, 0, DRM_GCN_DEPTH_MAX / 8,
		  DRM_GCN_RGBA8(0, 0xff, 0xff, 0xff), TEST_WIDTH, 0 },
		{ TEST_WIDTH, TEST_HEIGHT, DRM_GCN_DEPTH_MAX / 8,
		  DRM_GCN_RGBA8(0, 0xff, 0xff, 0xff),
		  TEST_WIDTH, TEST_HEIGHT },
		{ 0, TEST_HEIGHT, DRM_GCN_DEPTH_MAX / 8,
		  DRM_GCN_RGBA8(0, 0xff, 0xff, 0xff), 0, TEST_HEIGHT },
		{ 0, 0, DRM_GCN_DEPTH_MAX * 3 / 4,
		  DRM_GCN_RGBA8(0xff, 0, 0xff, 0xff), 0, 0 },
		{ TEST_WIDTH, 0, DRM_GCN_DEPTH_MAX * 3 / 4,
		  DRM_GCN_RGBA8(0xff, 0, 0xff, 0xff), TEST_WIDTH, 0 },
		{ TEST_WIDTH, TEST_HEIGHT, DRM_GCN_DEPTH_MAX * 3 / 4,
		  DRM_GCN_RGBA8(0xff, 0, 0xff, 0xff),
		  TEST_WIDTH, TEST_HEIGHT },
		{ 0, TEST_HEIGHT, DRM_GCN_DEPTH_MAX * 3 / 4,
		  DRM_GCN_RGBA8(0xff, 0, 0xff, 0xff), 0, TEST_HEIGHT },
	};
	uint16_t indices[12] = { 1, 2, 3, 1, 3, 4, 5, 6, 7, 5, 7, 8 };
	struct drm_gcn_draw_indexed_fixed draw = {
		.vertex_count = ARRAY_SIZE(vertices),
		.triangle_count = 4,
		.tev_mode = DRM_GCN_TEV_MODULATE,
		.vertices_ptr = (uintptr_t)vertices,
		.indices_ptr = (uintptr_t)indices,
		.state = {
			.viewport_width = TEST_WIDTH,
			.viewport_height = TEST_HEIGHT,
			.scissor_x = 20,
			.scissor_y = 24,
			.scissor_width = 216,
			.scissor_height = 208,
			.blend_mode = DRM_GCN_BLEND_NONE,
		},
		.depth = {
			.test_enable = 1,
			.compare = DRM_GCN_DEPTH_LESS,
			.write_enable = 1,
		},
	};
	uint16_t *src_map = MAP_FAILED;
	uint16_t *dst_map = MAP_FAILED;
	unsigned int checked_pass = 0;
	unsigned int checked_replace = 0;
	unsigned int checked_depth = 0;
	unsigned int checked_painter = 0;
	unsigned int checked_preserved = 0;
	unsigned int checked_culled = 0;

	if (create_bo(fd, &src) || create_bo(fd, &dst)) {
		fail("create fixed-draw objects");
		goto out;
	}
	src_map = map_bo(fd, &src);
	dst_map = map_bo(fd, &dst);
	if (src_map == MAP_FAILED || dst_map == MAP_FAILED) {
		fail("map fixed-draw objects");
		goto out;
	}
	if (ioctl(fd, DRM_IOCTL_GCN_CTX_CREATE, &ctx)) {
		fail("create fixed-draw context");
		goto out;
	}
	if (ioctl(fd, DRM_IOCTL_SYNCOBJ_CREATE, &sync)) {
		fail("create fixed-draw syncobj");
		goto out_ctx;
	}

	for (unsigned int y = 0; y < TEST_HEIGHT; y++) {
		for (unsigned int x = 0; x < TEST_WIDTH; x++) {
			size_t pixel = tiled_rgb565_index(x, y, TEST_WIDTH);

			src_map[pixel] = yellow;
			dst_map[pixel] = sentinel;
		}
	}

	draw.ctx_id = ctx.id;
	draw.src_handle = src.handle;
	draw.dst_handle = dst.handle;
	draw.out_syncobj = sync.handle;

	draw.src_handle = 0;
	errno = 0;
	if (!ioctl(fd, DRM_IOCTL_GCN_DRAW_INDEXED_FIXED, &draw) ||
	    errno != EINVAL)
		fail("texture fixed draw without source should return EINVAL");
	draw.src_handle = src.handle;
	draw.tev_mode = DRM_GCN_TEV_REPLACE_TEXTURE;
	draw.state.blend_mode = DRM_GCN_BLEND_SRC_ALPHA;
	errno = 0;
	if (!ioctl(fd, DRM_IOCTL_GCN_DRAW_INDEXED_FIXED, &draw) ||
	    errno != EINVAL)
		fail("blended replace fixed draw should return EINVAL");
	draw.state.blend_mode = DRM_GCN_BLEND_NONE;
	draw.tev_mode = DRM_GCN_TEV_MODULATE;
	vertices[8].rgba &= ~0x7fU;
	errno = 0;
	if (!ioctl(fd, DRM_IOCTL_GCN_DRAW_INDEXED_FIXED, &draw) ||
	    errno != EINVAL)
		fail("nonopaque unblended fixed draw should return EINVAL");
	vertices[8].rgba |= 0xff;
	indices[0] = ARRAY_SIZE(vertices);
	errno = 0;
	if (!ioctl(fd, DRM_IOCTL_GCN_DRAW_INDEXED_FIXED, &draw) ||
	    errno != EINVAL)
		fail("out-of-range fixed-draw index should return EINVAL");
	indices[0] = 1;
	indices[1] = 1;
	errno = 0;
	if (!ioctl(fd, DRM_IOCTL_GCN_DRAW_INDEXED_FIXED, &draw) ||
	    errno != EINVAL)
		fail("degenerate fixed draw should return EINVAL");
	indices[1] = 2;
	vertices[8].z = DRM_GCN_DEPTH_MAX + 1;
	errno = 0;
	if (!ioctl(fd, DRM_IOCTL_GCN_DRAW_INDEXED_FIXED, &draw) ||
	    errno != EINVAL)
		fail("out-of-range fixed depth should return EINVAL");
	vertices[8].z = DRM_GCN_DEPTH_MAX * 3 / 4;
	draw.vertices_ptr = 1;
	errno = 0;
	if (!ioctl(fd, DRM_IOCTL_GCN_DRAW_INDEXED_FIXED, &draw) ||
	    errno != EFAULT)
		fail("bad fixed-draw vertex pointer should return EFAULT");
	draw.vertices_ptr = (uintptr_t)vertices;
	draw.indices_ptr = 1;
	errno = 0;
	if (!ioctl(fd, DRM_IOCTL_GCN_DRAW_INDEXED_FIXED, &draw) ||
	    errno != EFAULT)
		fail("bad fixed-draw index pointer should return EFAULT");
	draw.indices_ptr = (uintptr_t)indices;
	draw.state.cull_mode = DRM_GCN_CULL_ALL + 1;
	errno = 0;
	if (!ioctl(fd, DRM_IOCTL_GCN_DRAW_INDEXED_FIXED, &draw) ||
	    errno != EINVAL)
		fail("invalid fixed-draw cull mode should return EINVAL");
	draw.state.cull_mode = DRM_GCN_CULL_NONE;

	draw.tev_mode = DRM_GCN_TEV_PASS_COLOR;
	draw.src_handle = 0;
	for (unsigned int i = 0; i < ARRAY_SIZE(vertices); i++) {
		vertices[i].s = 0;
		vertices[i].t = 0;
	}
	if (cull_supported) {
		draw.state.cull_mode = DRM_GCN_CULL_ALL;
		if (submit_fixed(fd, &draw, sync.handle)) {
			fail("draw fixed cull-all control");
			goto out_sync;
		}
		for (unsigned int y = 0; y < TEST_HEIGHT; y++) {
			for (unsigned int x = 0; x < TEST_WIDTH; x++) {
				size_t pixel = tiled_rgb565_index(x, y, TEST_WIDTH);

				checked_culled++;
				if (dst_map[pixel] != sentinel) {
					fprintf(stderr,
						"FAIL: fixed cull-all mismatch at (%u,%u): got=0x%04x expected=0x%04x\n",
						x, y, dst_map[pixel], sentinel);
					failures++;
					goto out_sync;
				}
			}
		}
		draw.state.cull_mode = DRM_GCN_CULL_NONE;
	}
	if (submit_fixed(fd, &draw, sync.handle)) {
		fail("draw fixed pass-color control");
		goto out_sync;
	}
	for (unsigned int y = 0; y < TEST_HEIGHT; y++) {
		for (unsigned int x = 0; x < TEST_WIDTH; x++) {
			bool inside = x >= draw.state.scissor_x &&
				x < draw.state.scissor_x + draw.state.scissor_width &&
				y >= draw.state.scissor_y &&
				y < draw.state.scissor_y + draw.state.scissor_height;
			uint16_t expected = inside ? cyan : sentinel;
			size_t pixel = tiled_rgb565_index(x, y, TEST_WIDTH);

			if (inside)
				checked_pass++;
			else
				checked_preserved++;
			if (dst_map[pixel] != expected) {
				fprintf(stderr,
					"FAIL: fixed pass-color mismatch at (%u,%u): got=0x%04x expected=0x%04x\n",
					x, y, dst_map[pixel], expected);
				failures++;
				goto out_sync;
			}
		}
	}

	for (unsigned int y = 0; y < TEST_HEIGHT; y++)
		for (unsigned int x = 0; x < TEST_WIDTH; x++)
			dst_map[tiled_rgb565_index(x, y, TEST_WIDTH)] = sentinel;
	draw.tev_mode = DRM_GCN_TEV_REPLACE_TEXTURE;
	draw.src_handle = src.handle;
	if (submit_fixed(fd, &draw, sync.handle)) {
		fail("draw fixed replace-texture control");
		goto out_sync;
	}
	for (unsigned int y = draw.state.scissor_y;
	     y < draw.state.scissor_y + draw.state.scissor_height; y++) {
		for (unsigned int x = draw.state.scissor_x;
		     x < draw.state.scissor_x + draw.state.scissor_width; x++) {
			size_t pixel = tiled_rgb565_index(x, y, TEST_WIDTH);

			checked_replace++;
			if (dst_map[pixel] != yellow) {
				fprintf(stderr,
					"FAIL: fixed replace mismatch at (%u,%u): got=0x%04x expected=0x%04x\n",
					x, y, dst_map[pixel], yellow);
				failures++;
				goto out_sync;
			}
		}
	}

	for (unsigned int y = 0; y < TEST_HEIGHT; y++)
		for (unsigned int x = 0; x < TEST_WIDTH; x++)
			dst_map[tiled_rgb565_index(x, y, TEST_WIDTH)] = sentinel;
	vertices[0].s = 64;
	vertices[0].t = 64;
	vertices[2].s = TEST_WIDTH;
	vertices[3].s = TEST_WIDTH;
	vertices[3].t = TEST_HEIGHT;
	vertices[4].t = TEST_HEIGHT;
	vertices[6].s = TEST_WIDTH;
	vertices[7].s = TEST_WIDTH;
	vertices[7].t = TEST_HEIGHT;
	vertices[8].t = TEST_HEIGHT;
	draw.tev_mode = DRM_GCN_TEV_MODULATE;
	if (submit_fixed(fd, &draw, sync.handle)) {
		fail("draw fixed modulated depth batch");
		goto out_sync;
	}
	for (unsigned int y = 0; y < TEST_HEIGHT; y++) {
		for (unsigned int x = 0; x < TEST_WIDTH; x++) {
			bool inside = x >= draw.state.scissor_x &&
				x < draw.state.scissor_x + draw.state.scissor_width &&
				y >= draw.state.scissor_y &&
				y < draw.state.scissor_y + draw.state.scissor_height;
			uint16_t expected = inside ? green : sentinel;
			size_t pixel = tiled_rgb565_index(x, y, TEST_WIDTH);

			if (inside)
				checked_depth++;
			if (dst_map[pixel] != expected) {
				fprintf(stderr,
					"FAIL: fixed modulate-depth mismatch at (%u,%u): got=0x%04x expected=0x%04x\n",
					x, y, dst_map[pixel], expected);
				failures++;
				goto out_sync;
			}
		}
	}

	for (unsigned int y = 0; y < TEST_HEIGHT; y++)
		for (unsigned int x = 0; x < TEST_WIDTH; x++)
			dst_map[tiled_rgb565_index(x, y, TEST_WIDTH)] = sentinel;
	draw.depth.test_enable = 0;
	draw.depth.compare = DRM_GCN_DEPTH_ALWAYS;
	draw.depth.write_enable = 0;
	if (submit_fixed(fd, &draw, sync.handle)) {
		fail("draw fixed modulated painter-order control");
		goto out_sync;
	}
	for (unsigned int y = draw.state.scissor_y;
	     y < draw.state.scissor_y + draw.state.scissor_height; y++) {
		for (unsigned int x = draw.state.scissor_x;
		     x < draw.state.scissor_x + draw.state.scissor_width; x++) {
			size_t pixel = tiled_rgb565_index(x, y, TEST_WIDTH);

			checked_painter++;
			if (dst_map[pixel] != red) {
				fprintf(stderr,
					"FAIL: fixed modulate-painter mismatch at (%u,%u): got=0x%04x expected=0x%04x\n",
					x, y, dst_map[pixel], red);
				failures++;
				goto out_sync;
			}
		}
	}
	printf("DRAW fixed: pass=%u replace=%u near=%u painter=%u preserved=%u culled=%u\n",
	       checked_pass, checked_replace, checked_depth, checked_painter,
	       checked_preserved, checked_culled);

out_sync:
	destroy.handle = sync.handle;
	if (ioctl(fd, DRM_IOCTL_SYNCOBJ_DESTROY, &destroy))
		fail("destroy fixed-draw syncobj");
out_ctx:
	free_ctx.id = ctx.id;
	if (ioctl(fd, DRM_IOCTL_GCN_CTX_FREE, &free_ctx))
		fail("free fixed-draw context");
out:
	if (dst_map != MAP_FAILED && munmap(dst_map, dst.size))
		fail("unmap fixed-draw destination");
	if (src_map != MAP_FAILED && munmap(src_map, src.size))
		fail("unmap fixed-draw source");
	if (dst.handle && close_bo(fd, dst.handle))
		fail("close fixed-draw destination");
	if (src.handle && close_bo(fd, src.handle))
		fail("close fixed-draw source");
}

static void test_rgba8_texture_alpha(int fd)
{
	const unsigned int src_width = 4;
	const unsigned int src_height = 4;
	const unsigned int dst_width = 8;
	const unsigned int dst_height = 8;
	const uint16_t blue = 0x001f;
	const uint16_t red = 0xf800;
	const uint16_t white = 0xffff;
	struct drm_syncobj_create sync = {};
	struct drm_syncobj_destroy destroy = {};
	struct drm_gcn_ctx_create ctx = {};
	struct drm_gcn_ctx_free free_ctx = {};
	struct drm_gcn_gem_create src = {};
	struct drm_gcn_gem_create dst = {};
	struct drm_gcn_gem_create invalid = {};
	struct drm_gcn_fixed_vertex vertices[4] = {
		{ 0, 0, DRM_GCN_DEPTH_MAX,
		  DRM_GCN_RGBA8(0xff, 0xff, 0xff, 0xff), 0, 0 },
		{ dst_width, 0, DRM_GCN_DEPTH_MAX,
		  DRM_GCN_RGBA8(0xff, 0xff, 0xff, 0xff), src_width, 0 },
		{ dst_width, dst_height, DRM_GCN_DEPTH_MAX,
		  DRM_GCN_RGBA8(0xff, 0xff, 0xff, 0xff), src_width, src_height },
		{ 0, dst_height, DRM_GCN_DEPTH_MAX,
		  DRM_GCN_RGBA8(0xff, 0xff, 0xff, 0xff), 0, src_height },
	};
	uint16_t indices[6] = { 0, 1, 2, 0, 2, 3 };
	struct drm_gcn_draw_indexed_fixed draw = {
		.vertex_count = ARRAY_SIZE(vertices),
		.triangle_count = 2,
		.tev_mode = DRM_GCN_TEV_MODULATE,
		.vertices_ptr = (uintptr_t)vertices,
		.indices_ptr = (uintptr_t)indices,
		.state = {
			.viewport_width = dst_width,
			.viewport_height = dst_height,
			.scissor_width = dst_width,
			.scissor_height = dst_height,
			.blend_mode = DRM_GCN_BLEND_SRC_ALPHA,
		},
		.depth = {
			.compare = DRM_GCN_DEPTH_ALWAYS,
		},
	};
	uint8_t *src_map = MAP_FAILED;
	uint16_t *dst_map = MAP_FAILED;
	uint64_t free_before = 0;
	uint64_t free_after = 0;
	unsigned int checked = 0;
	int ret;

	errno = 0;
	ret = create_bo_size_format_layout_flags(fd, &invalid, src_width,
						 src_height,
						 DRM_GCN_GEM_FORMAT_RGBA8,
						 DRM_GCN_GEM_LAYOUT_LINEAR, 0);
	if (!ret) {
		fail("linear RGBA8 texture should return EINVAL");
		if (invalid.handle && close_bo(fd, invalid.handle))
			fail("close unexpected linear RGBA8 texture");
	} else if (errno != EINVAL) {
		fail("linear RGBA8 texture returned wrong error");
	}
	errno = 0;
	ret = create_bo_size_format_layout_flags(fd, &invalid, src_width,
						 src_height,
						 DRM_GCN_GEM_FORMAT_RGBA8,
						 DRM_GCN_GEM_LAYOUT_TILED_4X4,
						 DRM_GCN_GEM_CREATE_SYSTEM);
	if (!ret) {
		fail("system RGBA8 texture should return EINVAL");
		if (invalid.handle && close_bo(fd, invalid.handle))
			fail("close unexpected system RGBA8 texture");
	} else if (errno != EINVAL) {
		fail("system RGBA8 texture returned wrong error");
	}

	if (get_param(fd, DRM_GCN_PARAM_MEM1_FREE_BYTES, &free_before)) {
		fail("query MEM1 before RGBA8 texture test");
		return;
	}
	ret = create_bo_size_format_layout_flags(fd, &src, src_width, src_height,
						 DRM_GCN_GEM_FORMAT_RGBA8,
						 DRM_GCN_GEM_LAYOUT_TILED_4X4, 0);
	if (ret ||
	    create_bo_size(fd, &dst, dst_width, dst_height)) {
		fail("create RGBA8 texture objects");
		goto out;
	}
	src_map = map_bo(fd, &src);
	dst_map = map_bo(fd, &dst);
	if (src_map == MAP_FAILED || dst_map == MAP_FAILED) {
		fail("map RGBA8 texture objects");
		goto out;
	}
	if (ioctl(fd, DRM_IOCTL_GCN_CTX_CREATE, &ctx)) {
		fail("create RGBA8 texture context");
		goto out;
	}
	if (ioctl(fd, DRM_IOCTL_SYNCOBJ_CREATE, &sync)) {
		fail("create RGBA8 texture syncobj");
		goto out_ctx;
	}

	for (unsigned int y = 0; y < src_height; y++) {
		for (unsigned int x = 0; x < src_width; x++) {
			bool left = x < src_width / 2;
			bool top = y < src_height / 2;
			uint32_t rgba;

			if (top && left)
				rgba = DRM_GCN_RGBA8(0xff, 0, 0, 0xff);
			else if (top)
				rgba = DRM_GCN_RGBA8(0, 0xff, 0, 0);
			else if (left)
				rgba = DRM_GCN_RGBA8(0xff, 0xff, 0xff, 0xff);
			else
				rgba = DRM_GCN_RGBA8(0xff, 0, 0xff, 0);
			tiled_rgba8_store(src_map, x, y, src_width, rgba);
		}
	}
	for (unsigned int y = 0; y < dst_height; y++)
		for (unsigned int x = 0; x < dst_width; x++)
			dst_map[tiled_rgb565_index(x, y, dst_width)] = blue;

	draw.ctx_id = ctx.id;
	draw.src_handle = src.handle;
	draw.dst_handle = dst.handle;
	draw.out_syncobj = sync.handle;
	if (submit_fixed(fd, &draw, sync.handle)) {
		fail("draw RGBA8 texture-alpha quad");
		goto out_sync;
	}
	for (unsigned int y = 0; y < dst_height; y++) {
		for (unsigned int x = 0; x < dst_width; x++) {
			uint16_t expected;
			size_t pixel = tiled_rgb565_index(x, y, dst_width);

			if (x < dst_width / 2 && y < dst_height / 2)
				expected = red;
			else if (x < dst_width / 2 && y >= dst_height / 2)
				expected = white;
			else
				expected = blue;
			checked++;
			if (dst_map[pixel] != expected) {
				fprintf(stderr,
					"FAIL: RGBA8 texture-alpha mismatch at (%u,%u): got=0x%04x expected=0x%04x\n",
					x, y, dst_map[pixel], expected);
				failures++;
				goto out_sync;
			}
		}
	}
	printf("RGBA8 TEXTURE: %u alpha-composited pixels matched\n", checked);

out_sync:
	destroy.handle = sync.handle;
	if (ioctl(fd, DRM_IOCTL_SYNCOBJ_DESTROY, &destroy))
		fail("destroy RGBA8 texture syncobj");
out_ctx:
	free_ctx.id = ctx.id;
	if (ioctl(fd, DRM_IOCTL_GCN_CTX_FREE, &free_ctx))
		fail("free RGBA8 texture context");
out:
	if (dst_map != MAP_FAILED && munmap(dst_map, dst.size))
		fail("unmap RGBA8 texture destination");
	if (src_map != MAP_FAILED && munmap(src_map, src.size))
		fail("unmap RGBA8 texture source");
	if (dst.handle && close_bo(fd, dst.handle))
		fail("close RGBA8 texture destination");
	if (src.handle && close_bo(fd, src.handle))
		fail("close RGBA8 texture source");
	if (get_param(fd, DRM_GCN_PARAM_MEM1_FREE_BYTES, &free_after))
		fail("query MEM1 after RGBA8 texture test");
	else if (free_after != free_before)
		fail_value("RGBA8 texture MEM1 recovery", free_after, free_before);
}

static void test_linear_texture_filter(int fd)
{
	const unsigned int src_width = 4;
	const unsigned int src_height = 4;
	const unsigned int dst_width = 8;
	const unsigned int dst_height = 8;
	const uint16_t expected_row[8] = {
		0x0000, 0x0000, 0x7bef, 0xffdf,
		0x7bcf, 0x0000, 0x8430, 0xffff,
	};
	struct drm_syncobj_create sync = {};
	struct drm_syncobj_destroy destroy = {};
	struct drm_gcn_ctx_create ctx = {};
	struct drm_gcn_ctx_free free_ctx = {};
	struct drm_gcn_gem_create src = {};
	struct drm_gcn_gem_create dst = {};
	struct drm_gcn_fixed_vertex vertices[4] = {
		{ 0, 0, DRM_GCN_DEPTH_MAX,
		  DRM_GCN_RGBA8(0xff, 0xff, 0xff, 0xff), 0, 0 },
		{ dst_width, 0, DRM_GCN_DEPTH_MAX,
		  DRM_GCN_RGBA8(0xff, 0xff, 0xff, 0xff), src_width, 0 },
		{ dst_width, dst_height, DRM_GCN_DEPTH_MAX,
		  DRM_GCN_RGBA8(0xff, 0xff, 0xff, 0xff), src_width, src_height },
		{ 0, dst_height, DRM_GCN_DEPTH_MAX,
		  DRM_GCN_RGBA8(0xff, 0xff, 0xff, 0xff), 0, src_height },
	};
	uint16_t indices[6] = { 0, 1, 2, 0, 2, 3 };
	struct drm_gcn_draw_indexed_fixed draw = {
		.vertex_count = ARRAY_SIZE(vertices),
		.triangle_count = 2,
		.tev_mode = DRM_GCN_TEV_REPLACE_TEXTURE,
		.vertices_ptr = (uintptr_t)vertices,
		.indices_ptr = (uintptr_t)indices,
		.state = {
			.viewport_width = dst_width,
			.viewport_height = dst_height,
			.scissor_width = dst_width,
			.scissor_height = dst_height,
			.blend_mode = DRM_GCN_BLEND_NONE,
		},
		.depth = {
			.compare = DRM_GCN_DEPTH_ALWAYS,
		},
		.texture_filter = DRM_GCN_TEXTURE_FILTER_LINEAR,
	};
	uint8_t *src_map = MAP_FAILED;
	uint16_t *dst_map = MAP_FAILED;
	uint64_t free_before = 0;
	uint64_t free_after = 0;
	unsigned int checked = 0;

	if (get_param(fd, DRM_GCN_PARAM_MEM1_FREE_BYTES, &free_before)) {
		fail("query MEM1 before linear-filter test");
		return;
	}
	if (create_bo_size_format_layout_flags(fd, &src, src_width, src_height,
					       DRM_GCN_GEM_FORMAT_RGBA8,
					       DRM_GCN_GEM_LAYOUT_TILED_4X4, 0) ||
	    create_bo_size(fd, &dst, dst_width, dst_height)) {
		fail("create linear-filter objects");
		goto out;
	}
	src_map = map_bo(fd, &src);
	dst_map = map_bo(fd, &dst);
	if (src_map == MAP_FAILED || dst_map == MAP_FAILED) {
		fail("map linear-filter objects");
		goto out;
	}
	if (ioctl(fd, DRM_IOCTL_GCN_CTX_CREATE, &ctx)) {
		fail("create linear-filter context");
		goto out;
	}
	if (ioctl(fd, DRM_IOCTL_SYNCOBJ_CREATE, &sync)) {
		fail("create linear-filter syncobj");
		goto out_ctx;
	}

	for (unsigned int y = 0; y < src_height; y++) {
		for (unsigned int x = 0; x < src_width; x++) {
			uint32_t rgba = (x & 1) ?
				DRM_GCN_RGBA8(0xff, 0xff, 0xff, 0xff) :
				DRM_GCN_RGBA8(0, 0, 0, 0xff);

			tiled_rgba8_store(src_map, x, y, src_width, rgba);
		}
	}
	for (unsigned int y = 0; y < dst_height; y++)
		for (unsigned int x = 0; x < dst_width; x++)
			dst_map[tiled_rgb565_index(x, y, dst_width)] = 0xf81f;

	draw.ctx_id = ctx.id;
	draw.src_handle = src.handle;
	draw.dst_handle = dst.handle;
	draw.out_syncobj = sync.handle;
	if (submit_fixed(fd, &draw, sync.handle)) {
		fail("draw linear-filter quad");
		goto out_sync;
	}
	printf("LINEAR FILTER row 0:");
	for (unsigned int x = 0; x < dst_width; x++)
		printf(" %04x", dst_map[tiled_rgb565_index(x, 0, dst_width)]);
	putchar('\n');
	for (unsigned int y = 0; y < dst_height; y++) {
		for (unsigned int x = 0; x < dst_width; x++) {
			size_t pixel = tiled_rgb565_index(x, y, dst_width);

			checked++;
			if (dst_map[pixel] != expected_row[x]) {
				fprintf(stderr,
					"FAIL: linear-filter mismatch at (%u,%u): got=0x%04x expected=0x%04x\n",
					x, y, dst_map[pixel], expected_row[x]);
				failures++;
				goto out_sync;
			}
		}
	}
	printf("LINEAR FILTER: %u bilinear pixels matched\n", checked);

out_sync:
	destroy.handle = sync.handle;
	if (ioctl(fd, DRM_IOCTL_SYNCOBJ_DESTROY, &destroy))
		fail("destroy linear-filter syncobj");
out_ctx:
	free_ctx.id = ctx.id;
	if (ioctl(fd, DRM_IOCTL_GCN_CTX_FREE, &free_ctx))
		fail("free linear-filter context");
out:
	if (dst_map != MAP_FAILED && munmap(dst_map, dst.size))
		fail("unmap linear-filter destination");
	if (src_map != MAP_FAILED && munmap(src_map, src.size))
		fail("unmap linear-filter source");
	if (dst.handle && close_bo(fd, dst.handle))
		fail("close linear-filter destination");
	if (src.handle && close_bo(fd, src.handle))
		fail("close linear-filter source");
	if (get_param(fd, DRM_GCN_PARAM_MEM1_FREE_BYTES, &free_after))
		fail("query MEM1 after linear-filter test");
	else if (free_after != free_before)
		fail_value("linear-filter MEM1 recovery", free_after, free_before);
}

static void test_full_system_render(int fd)
{
	const uint16_t blue = 0x001f;
	const uint16_t green = 0x07e0;
	const uint16_t red = 0xf800;
	struct drm_syncobj_create sync = {};
	struct drm_syncobj_destroy destroy = {};
	struct drm_gcn_ctx_create ctx = {};
	struct drm_gcn_ctx_free free_ctx = {};
	struct drm_gcn_gem_create dst = {};
	struct drm_gcn_gem_create wrong_format = {};
	struct drm_gcn_fixed_vertex vertices[4] = {
		{ 0, 0, DRM_GCN_DEPTH_MAX, DRM_GCN_RGBA8(0xff, 0, 0, 0xff), 0, 0 },
		{ FULL_DST_WIDTH, 0, DRM_GCN_DEPTH_MAX,
		  DRM_GCN_RGBA8(0xff, 0, 0, 0xff), 0, 0 },
		{ FULL_DST_WIDTH, FULL_DST_HEIGHT, DRM_GCN_DEPTH_MAX,
		  DRM_GCN_RGBA8(0xff, 0, 0, 0xff), 0, 0 },
		{ 0, FULL_DST_HEIGHT, DRM_GCN_DEPTH_MAX,
		  DRM_GCN_RGBA8(0xff, 0, 0, 0xff), 0, 0 },
	};
	uint16_t indices[6] = { 0, 1, 2, 0, 2, 3 };
	struct drm_gcn_draw_indexed_fixed draw = {
		.vertex_count = ARRAY_SIZE(vertices),
		.triangle_count = 2,
		.tev_mode = DRM_GCN_TEV_PASS_COLOR,
		.vertices_ptr = (uintptr_t)vertices,
		.indices_ptr = (uintptr_t)indices,
		.state = {
			.viewport_width = FULL_DST_WIDTH,
			.viewport_height = FULL_DST_HEIGHT,
			.scissor_x = 96,
			.scissor_y = 80,
			.scissor_width = 448,
			.scissor_height = 320,
			.blend_mode = DRM_GCN_BLEND_NONE,
		},
		.depth = {
			.compare = DRM_GCN_DEPTH_ALWAYS,
		},
	};
	struct drm_gcn_submit fill = {
		.op = DRM_GCN_RENDER_OP_FILL_RGB565,
		.data = blue,
	};
	uint16_t *map = MAP_FAILED;
	uint64_t free_before = 0;
	uint64_t free_during = 0;
	uint64_t free_after = 0;
	unsigned int checked;

	if (get_param(fd, DRM_GCN_PARAM_MEM1_FREE_BYTES, &free_before)) {
		fail("query MEM1 before full-screen system render");
		return;
	}
	if (create_bo_size_layout_flags(fd, &dst, FULL_DST_WIDTH,
					FULL_DST_HEIGHT,
					DRM_GCN_GEM_LAYOUT_LINEAR,
					DRM_GCN_GEM_CREATE_SYSTEM)) {
		fail("create full-screen system render destination");
		goto out;
	}
	if (get_param(fd, DRM_GCN_PARAM_MEM1_FREE_BYTES, &free_during)) {
		fail("query MEM1 during full-screen system render");
		goto out;
	}
	if (free_during != free_before) {
		fail_value("system render object changed MEM1 free bytes",
			   free_during, free_before);
		goto out;
	}
	map = map_bo(fd, &dst);
	if (map == MAP_FAILED) {
		fail("map full-screen system render destination");
		goto out;
	}
	if (ioctl(fd, DRM_IOCTL_GCN_CTX_CREATE, &ctx)) {
		fail("create full-screen system render context");
		goto out;
	}
	if (ioctl(fd, DRM_IOCTL_SYNCOBJ_CREATE, &sync)) {
		fail("create full-screen system render syncobj");
		goto out_ctx;
	}

	fill.ctx_id = ctx.id;
	fill.dst_handle = dst.handle;
	fill.out_syncobj = sync.handle;
	if (ioctl(fd, DRM_IOCTL_GCN_SUBMIT, &fill)) {
		fail("fill full-screen linear system RGB565 destination");
		goto out_sync;
	}
	for (checked = 0; checked < FULL_DST_WIDTH * FULL_DST_HEIGHT; checked++) {
		if (map[checked] != blue) {
			fprintf(stderr,
				"FAIL: system full fill mismatch at %u: got=0x%04x expected=0x%04x\n",
				checked, map[checked], blue);
			failures++;
			goto out_sync;
		}
	}

	fill.op = DRM_GCN_RENDER_OP_FILL_RECT_RGB565;
	fill.data = DRM_GCN_RECT_DATA(green, 64, 48, 128, 96);
	if (ioctl(fd, DRM_IOCTL_GCN_SUBMIT, &fill)) {
		fail("fill rectangle in linear system RGB565 destination");
		goto out_sync;
	}
	for (unsigned int y = 0; y < FULL_DST_HEIGHT; y++) {
		for (unsigned int x = 0; x < FULL_DST_WIDTH; x++) {
			uint16_t expected = x >= 64 && x < 192 && y >= 48 && y < 144 ?
					    green : blue;

			if (map[y * FULL_DST_WIDTH + x] != expected) {
				fprintf(stderr,
					"FAIL: system rect fill mismatch at (%u,%u): got=0x%04x expected=0x%04x\n",
					x, y, map[y * FULL_DST_WIDTH + x], expected);
				failures++;
				goto out_sync;
			}
		}
	}

	draw.ctx_id = ctx.id;
	draw.dst_handle = dst.handle;
	draw.out_syncobj = sync.handle;
	if (submit_fixed(fd, &draw, sync.handle)) {
		fail("draw pass-color quad into linear system RGB565 destination");
		goto out_sync;
	}
	for (unsigned int y = 0; y < FULL_DST_HEIGHT; y++) {
		for (unsigned int x = 0; x < FULL_DST_WIDTH; x++) {
			bool inside = x >= draw.state.scissor_x &&
				x < draw.state.scissor_x + draw.state.scissor_width &&
				y >= draw.state.scissor_y &&
				y < draw.state.scissor_y + draw.state.scissor_height;
			uint16_t expected;

			if (inside)
				expected = red;
			else if (x >= 64 && x < 192 && y >= 48 && y < 144)
				expected = green;
			else
				expected = blue;
			if (map[y * FULL_DST_WIDTH + x] != expected) {
				fprintf(stderr,
					"FAIL: system fixed draw mismatch at (%u,%u): got=0x%04x expected=0x%04x\n",
					x, y, map[y * FULL_DST_WIDTH + x], expected);
				failures++;
				goto out_sync;
			}
		}
	}

	if (create_bo_size_format_layout_flags(fd, &wrong_format, 64, 64,
					       DRM_GCN_GEM_FORMAT_XRGB8888,
					       DRM_GCN_GEM_LAYOUT_LINEAR,
					       DRM_GCN_GEM_CREATE_SYSTEM)) {
		fail("create wrong-format system render control");
		goto out_sync;
	}
	fill.op = DRM_GCN_RENDER_OP_FILL_RGB565;
	fill.dst_handle = wrong_format.handle;
	fill.data = red;
	errno = 0;
	if (!ioctl(fd, DRM_IOCTL_GCN_SUBMIT, &fill) || errno != EINVAL)
		fail("RGB565 fill into XRGB8888 system object should return EINVAL");

	puts("SYSTEM RENDER: 640x480 linear fill, rectangle, and fixed draw matched every pixel");

out_sync:
	if (wrong_format.handle && close_bo(fd, wrong_format.handle))
		fail("close wrong-format system render control");
	destroy.handle = sync.handle;
	if (ioctl(fd, DRM_IOCTL_SYNCOBJ_DESTROY, &destroy))
		fail("destroy full-screen system render syncobj");
out_ctx:
	free_ctx.id = ctx.id;
	if (ioctl(fd, DRM_IOCTL_GCN_CTX_FREE, &free_ctx))
		fail("free full-screen system render context");
out:
	if (map != MAP_FAILED && munmap(map, dst.size))
		fail("unmap full-screen system render destination");
	if (dst.handle && close_bo(fd, dst.handle))
		fail("close full-screen system render destination");
	if (get_param(fd, DRM_GCN_PARAM_MEM1_FREE_BYTES, &free_after)) {
		fail("query MEM1 after full-screen system render");
	} else if (free_after != free_before) {
		fail_value("full-screen system render leaked MEM1", free_after,
			   free_before);
	}
}

static void test_wide_scaled_blit(int fd)
{
	struct drm_gcn_ctx_create ctx = {};
	struct drm_gcn_ctx_free free_ctx;
	struct drm_gcn_gem_create src = {};
	struct drm_gcn_gem_create dst = {};
	struct drm_gcn_blit_scaled blit;
	uint16_t *src_map = MAP_FAILED;
	uint16_t *dst_map = MAP_FAILED;

	if (create_bo_size(fd, &src, WIDE_SRC_WIDTH, WIDE_SRC_HEIGHT) ||
	    create_bo_size(fd, &dst, WIDE_DST_WIDTH, WIDE_DST_HEIGHT)) {
		fail("create 640-wide scaled-blit objects");
		goto out;
	}
	src_map = map_bo(fd, &src);
	dst_map = map_bo(fd, &dst);
	if (src_map == MAP_FAILED || dst_map == MAP_FAILED) {
		fail("map 640-wide scaled-blit objects");
		goto out;
	}

	for (unsigned int y = 0; y < WIDE_SRC_HEIGHT; y++) {
		for (unsigned int x = 0; x < WIDE_SRC_WIDTH; x++) {
			size_t pixel = tiled_rgb565_index(x, y,
							 WIDE_SRC_WIDTH);

			src_map[pixel] = y * WIDE_SRC_WIDTH + x;
		}
	}
	for (unsigned int y = 0; y < WIDE_DST_HEIGHT; y++) {
		for (unsigned int x = 0; x < WIDE_DST_WIDTH; x++) {
			size_t pixel = tiled_rgb565_index(x, y,
							 WIDE_DST_WIDTH);

			dst_map[pixel] = 0x5aa5;
		}
	}

	if (ioctl(fd, DRM_IOCTL_GCN_CTX_CREATE, &ctx)) {
		fail("create 640-wide scaled-blit context");
		goto out;
	}
	blit = (struct drm_gcn_blit_scaled) {
		.ctx_id = ctx.id,
		.src_handle = src.handle,
		.dst_handle = dst.handle,
		.src_width = WIDE_SRC_WIDTH,
		.src_height = WIDE_SRC_HEIGHT,
		.dst_width = WIDE_DST_WIDTH,
		.dst_height = WIDE_DST_HEIGHT,
	};
	if (ioctl(fd, DRM_IOCTL_GCN_BLIT_SCALED, &blit)) {
		fail("submit 320x120 to 640x240 scaled blit");
		goto out_ctx;
	}

	for (unsigned int y = 0; y < WIDE_DST_HEIGHT; y++) {
		for (unsigned int x = 0; x < WIDE_DST_WIDTH; x++) {
			unsigned int source_x = scaled_source_offset(x,
					WIDE_SRC_WIDTH, WIDE_DST_WIDTH);
			unsigned int source_y = scaled_source_offset(y,
					WIDE_SRC_HEIGHT, WIDE_DST_HEIGHT);
			uint16_t expected = source_y * WIDE_SRC_WIDTH + source_x;
			size_t pixel = tiled_rgb565_index(x, y,
							 WIDE_DST_WIDTH);

			if (dst_map[pixel] != expected) {
				fprintf(stderr,
					"FAIL: 640-wide scale mismatch at (%u,%u): got=0x%04x expected=0x%04x\n",
					x, y, dst_map[pixel], expected);
				failures++;
				goto out_ctx;
			}
		}
	}
	puts("SCALED: 320x120 to 640x240 preserved all 153600 pixels");

out_ctx:
	free_ctx.id = ctx.id;
	free_ctx.pad = 0;
	if (ioctl(fd, DRM_IOCTL_GCN_CTX_FREE, &free_ctx))
		fail("free 640-wide scaled-blit context");
out:
	if (dst_map != MAP_FAILED && munmap(dst_map, dst.size))
		fail("unmap 640-wide scaled-blit destination");
	if (src_map != MAP_FAILED && munmap(src_map, src.size))
		fail("unmap 640-wide scaled-blit source");
	if (dst.handle && close_bo(fd, dst.handle))
		fail("close 640-wide scaled-blit destination");
	if (src.handle && close_bo(fd, src.handle))
		fail("close 640-wide scaled-blit source");
}

static void test_offset_enlarge(int fd, unsigned int iterations)
{
	const struct scaled_blit_case test = {
		"full-width enlargement", 0, 43, 255, 79, 0, 97, 256, 79, false,
	};
	struct drm_gcn_ctx_create ctx = {};
	struct drm_gcn_ctx_free free_ctx = {};
	struct drm_syncobj_create sync = {};
	struct drm_syncobj_destroy destroy = {};
	struct drm_gcn_gem_create src = {};
	struct drm_gcn_gem_create dst = {};
	uint16_t *src_map = MAP_FAILED;
	uint16_t *dst_map = MAP_FAILED;
	uint64_t free_before;
	uint64_t free_after;

	if (get_param(fd, DRM_GCN_PARAM_MEM1_FREE_BYTES, &free_before)) {
		fail("query offset-enlargement initial memory");
		return;
	}
	if (create_bo(fd, &src) || create_bo(fd, &dst)) {
		fail("create offset-enlargement objects");
		goto out;
	}
	src_map = map_bo(fd, &src);
	dst_map = map_bo(fd, &dst);
	if (src_map == MAP_FAILED || dst_map == MAP_FAILED) {
		fail("map offset-enlargement objects");
		goto out;
	}
	if (ioctl(fd, DRM_IOCTL_GCN_CTX_CREATE, &ctx) ||
	    ioctl(fd, DRM_IOCTL_SYNCOBJ_CREATE, &sync)) {
		fail("create offset-enlargement context or syncobj");
		goto out;
	}
	for (unsigned int frame = 0; frame < iterations && !failures; frame++) {
		printf("OFFSET ENLARGE: frame=%u\n", frame);
		if (test_scaled_blit_case(fd, ctx.id, sync.handle, src.handle,
					  dst.handle, src_map, dst_map, &test))
			break;
		for (unsigned int y = 0; y < TEST_HEIGHT; y++) {
			for (unsigned int x = 0; x < TEST_WIDTH; x++) {
				size_t pixel = tiled_rgb565_index(x, y, TEST_WIDTH);

				if (src_map[pixel] != same_object_pattern(x, y)) {
					fail("offset-enlargement source changed");
					goto out;
				}
			}
		}
		printf("OFFSET: %u/%u iterations\n", frame + 1, iterations);
	}
out:
	if (sync.handle) {
		destroy.handle = sync.handle;
		if (ioctl(fd, DRM_IOCTL_SYNCOBJ_DESTROY, &destroy))
			fail("destroy offset-enlargement syncobj");
	}
	if (ctx.id) {
		free_ctx.id = ctx.id;
		if (ioctl(fd, DRM_IOCTL_GCN_CTX_FREE, &free_ctx))
			fail("free offset-enlargement context");
	}
	if (dst_map != MAP_FAILED && munmap(dst_map, dst.size))
		fail("unmap offset-enlargement destination");
	if (src_map != MAP_FAILED && munmap(src_map, src.size))
		fail("unmap offset-enlargement source");
	if (dst.handle && close_bo(fd, dst.handle))
		fail("close offset-enlargement destination");
	if (src.handle && close_bo(fd, src.handle))
		fail("close offset-enlargement source");
	if (get_param(fd, DRM_GCN_PARAM_MEM1_FREE_BYTES, &free_after))
		fail("query offset-enlargement final memory");
	else if (free_after != free_before)
		fail_value("offset-enlargement memory recovery", free_after,
			   free_before);
	else
		printf("OFFSET ENLARGE: recovered %llu MEM1 bytes\n",
		       (unsigned long long)free_after);
}

static uint16_t reduction_content(unsigned int x, unsigned int y,
				  int pattern, unsigned int iteration)
{
	uint32_t value;

	switch (pattern) {
	case -2: return 0x9f1d;
	case -1: return y * WIDE_REDUCE_SRC_WIDTH + x;
	case 0: return 0;
	case 1: return 0xffff;
	case 2: return 0xf800;
	case 3: return 0x07e0;
	case 4: return 0x001f;
	case 5: return ((x / 2 + y / 2 + iteration / 8) & 1) ? 0xffff : 0;
	case 6: return 1U << ((x / 2 + y / 2 + iteration / 8) & 15);
	default:
		value = (iteration + 1) * 0x9e3779b9U ^ (y * 640 + x);
		value ^= value >> 16;
		value *= 0x7feb352dU;
		value ^= value >> 15;
		value *= 0x846ca68bU;
		return (value ^ (value >> 16)) & 0xffff;
	}
}

static void test_wide_scaled_reduce(int fd, int pattern, unsigned int iteration)
{
	struct drm_gcn_ctx_create ctx = {};
	struct drm_gcn_ctx_free free_ctx;
	struct drm_gcn_gem_create src = {};
	struct drm_gcn_gem_create dst = {};
	struct drm_gcn_blit_scaled blit;
	uint16_t *src_map = MAP_FAILED;
	uint16_t *dst_map = MAP_FAILED;

	if (create_bo_size(fd, &src, WIDE_REDUCE_SRC_WIDTH,
			   WIDE_REDUCE_SRC_HEIGHT) ||
	    create_bo_size(fd, &dst, WIDE_REDUCE_DST_WIDTH,
			   WIDE_REDUCE_DST_HEIGHT)) {
		fail("create 640-wide scaled-reduction objects");
		goto out;
	}
	src_map = map_bo(fd, &src);
	dst_map = map_bo(fd, &dst);
	if (src_map == MAP_FAILED || dst_map == MAP_FAILED) {
		fail("map 640-wide scaled-reduction objects");
		goto out;
	}

	for (unsigned int y = 0; y < WIDE_REDUCE_SRC_HEIGHT; y++) {
		for (unsigned int x = 0; x < WIDE_REDUCE_SRC_WIDTH; x++) {
			size_t pixel = tiled_rgb565_index(x, y,
						 WIDE_REDUCE_SRC_WIDTH);

			src_map[pixel] = reduction_content(x, y, pattern, iteration);
		}
	}
	for (unsigned int y = 0; y < WIDE_REDUCE_DST_HEIGHT; y++) {
		for (unsigned int x = 0; x < WIDE_REDUCE_DST_WIDTH; x++) {
			size_t pixel = tiled_rgb565_index(x, y,
						 WIDE_REDUCE_DST_WIDTH);

			dst_map[pixel] = 0xa55a;
		}
	}

	if (ioctl(fd, DRM_IOCTL_GCN_CTX_CREATE, &ctx)) {
		fail("create 640-wide scaled-reduction context");
		goto out;
	}
	blit = (struct drm_gcn_blit_scaled) {
		.ctx_id = ctx.id,
		.src_handle = src.handle,
		.dst_handle = dst.handle,
		.src_width = WIDE_REDUCE_SRC_WIDTH,
		.src_height = WIDE_REDUCE_SRC_HEIGHT,
		.dst_width = WIDE_REDUCE_DST_WIDTH,
		.dst_height = WIDE_REDUCE_DST_HEIGHT,
	};
	if (ioctl(fd, DRM_IOCTL_GCN_BLIT_SCALED, &blit)) {
		fail("submit 640x240 to 320x120 scaled reduction");
		goto out_ctx;
	}

	for (unsigned int y = 0; y < WIDE_REDUCE_DST_HEIGHT; y++) {
		for (unsigned int x = 0; x < WIDE_REDUCE_DST_WIDTH; x++) {
			unsigned int source_x = scaled_source_offset(x,
					WIDE_REDUCE_SRC_WIDTH,
					WIDE_REDUCE_DST_WIDTH);
			unsigned int source_y = scaled_source_offset(y,
					WIDE_REDUCE_SRC_HEIGHT,
					WIDE_REDUCE_DST_HEIGHT);
			uint16_t expected = reduction_content(source_x, source_y, pattern, iteration);
			size_t pixel = tiled_rgb565_index(x, y,
						 WIDE_REDUCE_DST_WIDTH);

			if (dst_map[pixel] != expected) {
				fprintf(stderr,
					"FAIL: 640-wide reduction mismatch at (%u,%u): got=0x%04x expected=0x%04x\n",
					x, y, dst_map[pixel], expected);
				failures++;
				goto out_ctx;
			}
		}
	}
	puts("SCALED: 640x240 to 320x120 preserved all 38400 pixels");

out_ctx:
	free_ctx.id = ctx.id;
	free_ctx.pad = 0;
	if (ioctl(fd, DRM_IOCTL_GCN_CTX_FREE, &free_ctx))
		fail("free 640-wide scaled-reduction context");
out:
	if (dst_map != MAP_FAILED && munmap(dst_map, dst.size))
		fail("unmap 640-wide scaled-reduction destination");
	if (src_map != MAP_FAILED && munmap(src_map, src.size))
		fail("unmap 640-wide scaled-reduction source");
	if (dst.handle && close_bo(fd, dst.handle))
		fail("close 640-wide scaled-reduction destination");
	if (src.handle && close_bo(fd, src.handle))
		fail("close 640-wide scaled-reduction source");
}

static int system_trace_event(unsigned int iteration, bool begin)
{
	char marker[80];
	int length;

	if (begin && pwrite(system_trace_on, "1", 1, 0) != 1)
		return -1;
	length = snprintf(marker, sizeof(marker), "GCN %s iteration=%u\n", begin ? "begin" : "end", iteration);
	if (write(system_trace_marker, marker, length) != length) {
		if (pwrite(system_trace_on, "0", 1, 0) != 1)
			fail("disable scheduler trace");
		return -1;
	}
	if (!begin && pwrite(system_trace_on, "0", 1, 0) != 1)
		return -1;
	return 0;
}

static void test_full_system_layout(int fd, uint32_t layout,
				    const char *layout_name, int pattern,
				    unsigned int iteration, bool timed)
{
	struct drm_gcn_ctx_create ctx = {};
	struct drm_gcn_ctx_free free_ctx;
	struct drm_gcn_gem_create src = {};
	struct drm_gcn_gem_create dst = {};
	struct drm_gcn_blit_scaled blit;
	uint16_t *src_map = MAP_FAILED;
	uint16_t *dst_map = MAP_FAILED;
	uint64_t free_before = 0;
	uint64_t free_during = 0;
	uint64_t free_after = 0;
	uint64_t started, elapsed, cpu_elapsed = 0;
	struct timespec cpu_start, cpu_end, cpu_resolution;
	struct rusage usage_start, usage_end;
	int submit_status, submit_errno;

	if (get_param(fd, DRM_GCN_PARAM_MEM1_FREE_BYTES, &free_before)) {
		fail("query MEM1 before full-screen system scale");
		return;
	}
	if (create_bo_size_layout_flags(fd, &src, FULL_SRC_WIDTH,
					FULL_SRC_HEIGHT, layout,
					DRM_GCN_GEM_CREATE_SYSTEM) ||
	    create_bo_size_layout_flags(fd, &dst, FULL_DST_WIDTH,
					FULL_DST_HEIGHT, layout,
					DRM_GCN_GEM_CREATE_SYSTEM)) {
		fail("create full-screen system scaled-blit objects");
		goto out;
	}
	if (get_param(fd, DRM_GCN_PARAM_MEM1_FREE_BYTES, &free_during)) {
		fail("query MEM1 during full-screen system scale");
		goto out;
	}
	if (free_during != free_before) {
		fail_value("system objects changed MEM1 free bytes", free_during,
			   free_before);
		goto out;
	}

	src_map = map_bo(fd, &src);
	dst_map = map_bo(fd, &dst);
	if (src_map == MAP_FAILED || dst_map == MAP_FAILED) {
		fail("map full-screen system scaled-blit objects");
		goto out;
	}

	for (unsigned int y = 0; y < FULL_SRC_HEIGHT; y++) {
		for (unsigned int x = 0; x < FULL_SRC_WIDTH; x++) {
			size_t pixel = rgb565_index(x, y, FULL_SRC_WIDTH,
						   layout);

			src_map[pixel] = pattern < 0 ? y * FULL_SRC_WIDTH + x :
				reduction_content(x * 2, y * 2, pattern, iteration);
		}
	}
	for (unsigned int y = 0; y < FULL_DST_HEIGHT; y++) {
		for (unsigned int x = 0; x < FULL_DST_WIDTH; x++) {
			size_t pixel = rgb565_index(x, y, FULL_DST_WIDTH,
						   layout);

			dst_map[pixel] = 0xc33c;
		}
	}

	if (ioctl(fd, DRM_IOCTL_GCN_CTX_CREATE, &ctx)) {
		fail("create full-screen system scaled-blit context");
		goto out;
	}
	blit = (struct drm_gcn_blit_scaled) {
		.ctx_id = ctx.id,
		.src_handle = src.handle,
		.dst_handle = dst.handle,
		.src_width = FULL_SRC_WIDTH,
		.src_height = FULL_SRC_HEIGHT,
		.dst_width = FULL_DST_WIDTH,
		.dst_height = FULL_DST_HEIGHT,
	};
	if (system_sched && system_trace_event(iteration, true)) {
		fail("start scheduler trace window");
		goto out_ctx;
	}
	started = monotonic_ns();
	if (system_profile && (clock_getres(CLOCK_THREAD_CPUTIME_ID, &cpu_resolution) ||
	    getrusage(RUSAGE_SELF, &usage_start) ||
	    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu_start))) {
		fail("read system CPU profile start");
		goto out_ctx;
	}
	submit_status = ioctl(fd, DRM_IOCTL_GCN_BLIT_SCALED, &blit);
	submit_errno = errno;
	if (system_profile) {
		if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu_end) ||
		    getrusage(RUSAGE_SELF, &usage_end)) {
			fail("read system CPU profile end");
			goto out_ctx;
		}
		cpu_elapsed = ((uint64_t)cpu_end.tv_sec * 1000000000ULL + cpu_end.tv_nsec) -
			      ((uint64_t)cpu_start.tv_sec * 1000000000ULL + cpu_start.tv_nsec);
	}
	elapsed = monotonic_ns() - started;
	if (system_sched && system_trace_event(iteration, false)) {
		fail("end scheduler trace window");
		goto out_ctx;
	}
	if (timed)
		printf("SYSTEM TIMING: iteration=%u ns=%llu status=%d\n", iteration,
		       (unsigned long long)elapsed, submit_status);
	if (system_profile)
		printf("SYSTEM CPU: iteration=%u ns=%llu voluntary=%ld involuntary=%ld resolution_ns=%llu\n",
		       iteration, (unsigned long long)cpu_elapsed,
		       usage_end.ru_nvcsw - usage_start.ru_nvcsw,
		       usage_end.ru_nivcsw - usage_start.ru_nivcsw,
		       (unsigned long long)cpu_resolution.tv_sec * 1000000000ULL + cpu_resolution.tv_nsec);
	if (submit_status) {
		errno = submit_errno;
		fail("submit 320x240 to 640x480 system scaled blit");
		goto out_ctx;
	}

	for (unsigned int y = 0; y < FULL_DST_HEIGHT; y++) {
		for (unsigned int x = 0; x < FULL_DST_WIDTH; x++) {
			unsigned int source_x = scaled_source_offset(x,
					FULL_SRC_WIDTH, FULL_DST_WIDTH);
			unsigned int source_y = scaled_source_offset(y,
					FULL_SRC_HEIGHT, FULL_DST_HEIGHT);
			uint16_t expected = pattern < 0 ? source_y * FULL_SRC_WIDTH + source_x :
				reduction_content(source_x * 2, source_y * 2, pattern, iteration);
			size_t pixel = rgb565_index(x, y, FULL_DST_WIDTH,
						   layout);

			if (dst_map[pixel] != expected) {
				fprintf(stderr,
					"FAIL: full-screen system scale mismatch at (%u,%u): got=0x%04x expected=0x%04x\n",
					x, y, dst_map[pixel], expected);
				failures++;
				goto out_ctx;
			}
		}
	}
	printf("SCALED: %s system 320x240 to 640x480 preserved all 307200 pixels\n",
	       layout_name);

out_ctx:
	free_ctx.id = ctx.id;
	free_ctx.pad = 0;
	if (ioctl(fd, DRM_IOCTL_GCN_CTX_FREE, &free_ctx))
		fail("free full-screen system scaled-blit context");
out:
	if (dst_map != MAP_FAILED && munmap(dst_map, dst.size))
		fail("unmap full-screen system scaled-blit destination");
	if (src_map != MAP_FAILED && munmap(src_map, src.size))
		fail("unmap full-screen system scaled-blit source");
	if (dst.handle && close_bo(fd, dst.handle))
		fail("close full-screen system scaled-blit destination");
	if (src.handle && close_bo(fd, src.handle))
		fail("close full-screen system scaled-blit source");
	if (get_param(fd, DRM_GCN_PARAM_MEM1_FREE_BYTES, &free_after)) {
		fail("query MEM1 after full-screen system scale");
	} else if (free_after != free_before) {
		fail_value("full-screen system scale leaked MEM1", free_after,
			   free_before);
	} else {
		printf("SCALED: system objects preserved all %llu MEM1 bytes\n",
		       (unsigned long long)free_after);
	}
}

static void test_full_scaled_system_blit(int fd)
{
	test_full_system_layout(fd, DRM_GCN_GEM_LAYOUT_TILED_4X4, "tiled", -1, 0, false);
	test_full_system_layout(fd, DRM_GCN_GEM_LAYOUT_LINEAR, "linear", -1, 0, false);
}

static void test_full_xrgb8888_to_rgb565_system_blit(int fd)
{
	struct drm_gcn_ctx_create ctx = {};
	struct drm_gcn_ctx_free free_ctx;
	struct drm_gcn_gem_create src = {};
	struct drm_gcn_gem_create dst = {};
	struct drm_gcn_blit_scaled blit;
	struct drm_gcn_wait wait_args;
	uint32_t *src_map = MAP_FAILED;
	uint16_t *dst_map = MAP_FAILED;
	uint64_t free_before = 0;
	uint64_t free_after = 0;

	if (get_param(fd, DRM_GCN_PARAM_MEM1_FREE_BYTES, &free_before)) {
		fail("query MEM1 before XRGB8888 system scale");
		return;
	}
	if (create_bo_size_format_layout_flags(fd, &src, FULL_SRC_WIDTH,
					       FULL_SRC_HEIGHT,
					       DRM_GCN_GEM_FORMAT_XRGB8888,
					       DRM_GCN_GEM_LAYOUT_LINEAR,
					       DRM_GCN_GEM_CREATE_SYSTEM) ||
	    create_bo_size_layout_flags(fd, &dst, FULL_DST_WIDTH,
					FULL_DST_HEIGHT,
					DRM_GCN_GEM_LAYOUT_LINEAR,
					DRM_GCN_GEM_CREATE_SYSTEM)) {
		fail("create XRGB8888-to-RGB565 system objects");
		goto out;
	}
	if (src.size != FULL_SRC_WIDTH * FULL_SRC_HEIGHT * sizeof(*src_map)) {
		fail_value("XRGB8888 source size", src.size,
			   FULL_SRC_WIDTH * FULL_SRC_HEIGHT * sizeof(*src_map));
		goto out;
	}

	src_map = map_bo(fd, &src);
	dst_map = map_bo(fd, &dst);
	if (src_map == MAP_FAILED || dst_map == MAP_FAILED) {
		fail("map XRGB8888-to-RGB565 system objects");
		goto out;
	}
	for (unsigned int y = 0; y < FULL_SRC_HEIGHT; y++) {
		for (unsigned int x = 0; x < FULL_SRC_WIDTH; x++)
			src_map[(size_t)y * FULL_SRC_WIDTH + x] =
				xrgb8888_source_pattern(x, y);
	}
	for (unsigned int y = 0; y < FULL_DST_HEIGHT; y++) {
		for (unsigned int x = 0; x < FULL_DST_WIDTH; x++)
			dst_map[(size_t)y * FULL_DST_WIDTH + x] = 0x39e7;
	}

	if (ioctl(fd, DRM_IOCTL_GCN_CTX_CREATE, &ctx)) {
		fail("create XRGB8888 system-scale context");
		goto out;
	}
	blit = (struct drm_gcn_blit_scaled) {
		.ctx_id = ctx.id,
		.src_handle = src.handle,
		.dst_handle = dst.handle,
		.src_width = FULL_SRC_WIDTH,
		.src_height = FULL_SRC_HEIGHT,
		.dst_width = FULL_DST_WIDTH,
		.dst_height = FULL_DST_HEIGHT,
	};
	if (ioctl(fd, DRM_IOCTL_GCN_BLIT_SCALED, &blit)) {
		fail("submit XRGB8888-to-RGB565 system scale");
		goto out_ctx;
	}
	wait_args = (struct drm_gcn_wait) {
		.handle = dst.handle,
		.flags = DRM_GCN_WAIT_WRITE,
		.timeout_ns = monotonic_ns() + 1000000000ULL,
	};
	if (ioctl(fd, DRM_IOCTL_GCN_WAIT, &wait_args)) {
		fail("wait for XRGB8888-to-RGB565 destination");
		goto out_ctx;
	}

	for (unsigned int y = 0; y < FULL_DST_HEIGHT; y++) {
		for (unsigned int x = 0; x < FULL_DST_WIDTH; x++) {
			unsigned int source_x = scaled_source_offset(x,
					FULL_SRC_WIDTH, FULL_DST_WIDTH);
			unsigned int source_y = scaled_source_offset(y,
					FULL_SRC_HEIGHT, FULL_DST_HEIGHT);
			uint32_t source_pixel =
				xrgb8888_source_pattern(source_x, source_y);
			uint16_t expected = xrgb8888_to_rgb565(source_pixel);
			uint16_t actual = dst_map[(size_t)y * FULL_DST_WIDTH + x];

			if (actual != expected) {
				fprintf(stderr,
					"FAIL: XRGB8888 scale mismatch at (%u,%u): got=0x%04x expected=0x%04x\n",
					x, y, actual, expected);
				failures++;
				goto out_ctx;
			}
		}
	}
	puts("XRGB8888: linear 320x240 to RGB565 640x480 matched all 307200 pixels");

out_ctx:
	free_ctx.id = ctx.id;
	free_ctx.pad = 0;
	if (ioctl(fd, DRM_IOCTL_GCN_CTX_FREE, &free_ctx))
		fail("free XRGB8888 system-scale context");
out:
	if (dst_map != MAP_FAILED && munmap(dst_map, dst.size))
		fail("unmap XRGB8888 scale destination");
	if (src_map != MAP_FAILED && munmap(src_map, src.size))
		fail("unmap XRGB8888 scale source");
	if (dst.handle && close_bo(fd, dst.handle))
		fail("close XRGB8888 scale destination");
	if (src.handle && close_bo(fd, src.handle))
		fail("close XRGB8888 scale source");
	if (get_param(fd, DRM_GCN_PARAM_MEM1_FREE_BYTES, &free_after)) {
		fail("query MEM1 after XRGB8888 system scale");
	} else if (free_after != free_before) {
		fail_value("XRGB8888 system scale leaked MEM1", free_after,
			   free_before);
	} else {
		printf("XRGB8888: system objects preserved all %llu MEM1 bytes\n",
		       (unsigned long long)free_after);
	}
}

static int hold_mapping(const char *node)
{
	struct drm_gcn_gem_mmap mmap_args = {};
	struct drm_gcn_gem_create bo;
	uint8_t *mapping;
	uint64_t provider;
	int fd;

	fd = open(node, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		fail("open render node for hold");
		return EXIT_FAILURE;
	}
	if (get_param(fd, DRM_GCN_PARAM_PROVIDER_AVAILABLE, &provider) ||
	    !provider) {
		errno = ENODEV;
		fail("provider required for hold");
		close(fd);
		return EXIT_FAILURE;
	}
	if (create_bo(fd, &bo)) {
		fail("create held MEM1 object");
		close(fd);
		return EXIT_FAILURE;
	}
	mmap_args.handle = bo.handle;
	if (ioctl(fd, DRM_IOCTL_GCN_GEM_MMAP, &mmap_args)) {
		fail("query held MEM1 mmap offset");
		close_bo(fd, bo.handle);
		close(fd);
		return EXIT_FAILURE;
	}
	mapping = mmap(NULL, bo.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
		       mmap_args.offset);
	if (mapping == MAP_FAILED) {
		fail("mmap held MEM1 object");
		close_bo(fd, bo.handle);
		close(fd);
		return EXIT_FAILURE;
	}
	mapping[0] = 0x5a;
	if (mapping[0] != 0x5a) {
		errno = EIO;
		fail("held MEM1 mapping readback");
		return EXIT_FAILURE;
	}
	if (close_bo(fd, bo.handle)) {
		fail("close held MEM1 handle");
		return EXIT_FAILURE;
	}

	printf("READY: holding MEM1 VMA pid=%ld size=%llu\n", (long)getpid(),
	       (unsigned long long)bo.size);
	fflush(stdout);
	for (;;)
		pause();
}

static void test_provider(int fd, int other_fd, uint64_t free_before)
{
	struct drm_gcn_gem_create objects[MAX_OBJECTS];
	void *mapping = NULL;
	uint64_t free_after;
	uint64_t expected;
	unsigned int count = 0;
	unsigned int i;

	test_contexts(fd, other_fd);

	if (create_bo(fd, &objects[count])) {
		fail("create first MEM1 object");
		return;
	}
	if (objects[count].size != TEST_WIDTH * TEST_HEIGHT * 2ULL)
		fail_value("MEM1 object size", objects[count].size,
			   TEST_WIDTH * TEST_HEIGHT * 2ULL);
	count++;
	test_mapping(fd, &objects[0], &mapping);
	test_wait_and_prime(fd, objects[0].handle);

	while (count < MAX_OBJECTS) {
		if (!create_bo(fd, &objects[count])) {
			count++;
			continue;
		}
		if (errno != ENOSPC)
			fail("allocator exhaustion should return ENOSPC");
		break;
	}
	if (count == MAX_OBJECTS)
		fail_value("allocator did not exhaust", count, MAX_OBJECTS - 1);

	if (get_param(fd, DRM_GCN_PARAM_MEM1_FREE_BYTES, &free_after)) {
		fail("query free bytes after allocation");
	} else {
		expected = free_before - count * objects[0].size;
		if (free_after != expected)
			fail_value("free-byte accounting after allocation", free_after,
				   expected);
	}

	if (mapping && munmap(mapping, objects[0].size))
		fail("munmap MEM1 object");
	for (i = 0; i < count; i++) {
		if (close_bo(fd, objects[i].handle))
			fail("close MEM1 object");
	}
	if (get_param(fd, DRM_GCN_PARAM_MEM1_FREE_BYTES, &free_after)) {
		fail("query free bytes after release");
	} else if (free_after != free_before) {
		fail_value("free-byte accounting after release", free_after,
			   free_before);
	}

	printf("MEM1: allocated %u x %llu bytes before ENOSPC\n", count,
	       (unsigned long long)objects[0].size);
}

int main(int argc, char **argv)
{
	bool hold = argc > 1 && !strcmp(argv[1], "--hold");
	bool linear_only = argc > 1 && !strcmp(argv[1], "--linear-filter-only");
	bool wide_reduce_only = argc > 1 &&
				!strcmp(argv[1], "--wide-reduce-only");
	bool uniform_only = argc > 1 &&
			    !strcmp(argv[1], "--wide-reduce-uniform-only");
	bool reduce_content = argc > 2 && !strcmp(argv[1], "--reduce-content-repeat");
	bool reduce_repeat = reduce_content || (argc > 2 && !strcmp(argv[1], "--reduce-repeat"));
	bool sched_repeat = argc > 2 && !strcmp(argv[1], "--system-sched-repeat");
	bool profile_repeat = sched_repeat || (argc > 2 && !strcmp(argv[1], "--system-profile-repeat"));
	bool system_content = profile_repeat || (argc > 2 && !strcmp(argv[1], "--system-content-repeat"));
	bool system_repeat = system_content || (argc > 2 && !strcmp(argv[1], "--system-enlarge-repeat"));
	bool offset_repeat = argc > 2 && !strcmp(argv[1], "--offset-enlarge-repeat");
	unsigned int repeat_iterations = 1;
	bool offset_only = argc > 1 && !strcmp(argv[1], "--offset-enlarge-only");
	int mode_args = hold + linear_only + wide_reduce_only + uniform_only +
			offset_only + 2 * offset_repeat + 2 * system_repeat + 2 * reduce_repeat;
	const char *node = argc > 1 + mode_args ? argv[1 + mode_args] :
			   "/dev/dri/renderD128";
	uint64_t provider = 0;
	uint64_t abi = 0;
	uint64_t total = 0;
	uint64_t free_bytes = 0;
	uint64_t alignment = 0;
	uint64_t formats = 0;
	uint64_t layouts = 0;
	uint64_t features = 0;
	uint64_t max_width = 0;
	uint64_t max_height = 0;
	int other_fd;
	int fd;

	system_profile = profile_repeat;
	system_sched = sched_repeat;
	if (offset_repeat || system_repeat || reduce_repeat) {
		char *end;
		unsigned long count = strtoul(argv[2], &end, 10);

		if (!*argv[2] || *end || count < 1 || count > 1000)
			return EXIT_FAILURE;
		repeat_iterations = count;
	}
	setvbuf(stdout, NULL, _IOLBF, 0);

	if (hold)
		return hold_mapping(node);

	fd = open(node, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		fail("open render node");
		return EXIT_FAILURE;
	}
	other_fd = open(node, O_RDWR | O_CLOEXEC);
	if (other_fd < 0) {
		fail("open second render node fd");
		close(fd);
		return EXIT_FAILURE;
	}

	if (get_param(fd, DRM_GCN_PARAM_ABI_VERSION, &abi))
		fail("query ABI version");
	else if (abi != DRM_GCN_RENDER_ABI_VERSION)
		fail_value("ABI version", abi, DRM_GCN_RENDER_ABI_VERSION);
	if (get_param(fd, DRM_GCN_PARAM_PROVIDER_AVAILABLE, &provider))
		fail("query provider availability");
	if (get_param(fd, DRM_GCN_PARAM_FEATURES, &features))
		fail("query features");
	test_syncobj(fd);

	if (!provider) {
		if (features & DRM_GCN_FEATURE_MEM1_GEM)
			fail_value("MEM1 feature without provider", features, 0);
		printf("GCN render ABI %llu: provider absent\n",
		       (unsigned long long)abi);
		test_provider_absent(fd);
	} else {
		if (get_param(fd, DRM_GCN_PARAM_MEM1_TOTAL_BYTES, &total) ||
		    get_param(fd, DRM_GCN_PARAM_MEM1_FREE_BYTES, &free_bytes) ||
		    get_param(fd, DRM_GCN_PARAM_MEM1_ALIGNMENT, &alignment) ||
		    get_param(fd, DRM_GCN_PARAM_MAX_EFB_WIDTH, &max_width) ||
		    get_param(fd, DRM_GCN_PARAM_MAX_EFB_HEIGHT, &max_height) ||
		    get_param(fd, DRM_GCN_PARAM_FORMATS, &formats) ||
		    get_param(fd, DRM_GCN_PARAM_LAYOUTS, &layouts)) {
			fail("query provider capabilities");
		} else {
			printf("GCN render ABI %llu: MEM1 total=%llu free=%llu alignment=%llu\n",
			       (unsigned long long)abi,
			       (unsigned long long)total,
			       (unsigned long long)free_bytes,
			       (unsigned long long)alignment);
			if (!(features & DRM_GCN_FEATURE_MEM1_GEM) ||
			    !(formats & DRM_GCN_FORMAT_RGB565) ||
			    !(formats & DRM_GCN_FORMAT_XRGB8888) ||
			    !(layouts & DRM_GCN_LAYOUT_TILED_4X4) ||
			    !(layouts & DRM_GCN_LAYOUT_LINEAR))
				fail_value("provider capability bits", features, 0);
			if (!alignment || free_bytes > total)
				fail_value("provider capacity", free_bytes, total);
			if (max_width != 640)
				fail_value("provider EFB width", max_width, 640);
			if (max_height != 576)
				fail_value("provider EFB height", max_height, 576);
		}
		if (linear_only) {
			if ((features & DRM_GCN_FEATURE_TEXTURE_LINEAR) &&
			    (features & DRM_GCN_FEATURE_TEXTURE_RGBA8) &&
			    (formats & DRM_GCN_FORMAT_RGBA8))
				test_linear_texture_filter(fd);
			else
				fail_value("linear texture capability", features,
					   DRM_GCN_FEATURE_TEXTURE_LINEAR);
			goto out_close;
		}
		if (reduce_repeat) {
			if (features & DRM_GCN_FEATURE_BLIT_SCALED_RGB565) {
				for (unsigned int i = 0; i < repeat_iterations && !failures; i++) {
					if (reduce_content)
						printf("REDUCE CONTENT: iteration=%u pattern=%u seed=%u\n",
						       i, i % 8, (i + 1) * 0x9e3779b9U);
					test_wide_scaled_reduce(fd, reduce_content ? (int)(i % 8) : -1, i);
					if (!failures)
						printf("REDUCE: %u/%u iterations\n", i + 1, repeat_iterations);
				}
			} else {
				fail("scaled reduction feature unavailable");
			}
			goto out_close;
		}
		if (system_repeat) {
			if (system_sched) {
				system_trace_on = open("/sys/kernel/tracing/instances/wii-gcn-sched/tracing_on", O_WRONLY);
				system_trace_marker = open("/sys/kernel/tracing/instances/wii-gcn-sched/trace_marker", O_WRONLY);
				if (system_trace_on < 0 || system_trace_marker < 0) {
					fail("open isolated scheduler trace");
					goto out_close;
				}
			}
			if ((features & DRM_GCN_FEATURE_BLIT_SCALED_SYSTEM_RGB565) &&
			    (features & DRM_GCN_FEATURE_SYSTEM_GEM_LINEAR)) {
				for (unsigned int i = 0; i < repeat_iterations && !failures; i++) {
					if (system_content)
						printf("SYSTEM CONTENT: iteration=%u pattern=%u seed=%u\n",
						       i, i % 8, (i + 1U) * 0x9e3779b9U);
					test_full_system_layout(fd, DRM_GCN_GEM_LAYOUT_LINEAR, "linear",
								system_content ? (int)(i % 8) : -1, i, true);
					if (!failures)
						printf("SYSTEM: %u/%u iterations\n", i + 1, repeat_iterations);
				}
			} else {
				fail("system enlargement features unavailable");
			}
			goto out_close;
		}
		if (offset_only || offset_repeat) {
			if (features & DRM_GCN_FEATURE_BLIT_SCALED_RGB565) {
				test_offset_enlarge(fd, offset_repeat ? repeat_iterations : 2000);
			} else
				fail_value("scaled RGB565 capability", features,
					   DRM_GCN_FEATURE_BLIT_SCALED_RGB565);
			goto out_close;
		}
		if (wide_reduce_only || uniform_only) {
			if (features & DRM_GCN_FEATURE_BLIT_SCALED_RGB565) {
				for (unsigned int i = 1; i <= 100 && !failures; i++) {
					printf("WIDE REDUCE: iteration %u uniform=%u\n", i,
					       uniform_only);
					test_wide_scaled_reduce(fd, uniform_only ? -2 : -1, i);
				}
			} else {
				fail_value("scaled RGB565 capability", features,
					   DRM_GCN_FEATURE_BLIT_SCALED_RGB565);
			}
			goto out_close;
		}
		test_provider(fd, other_fd, free_bytes);
		if ((features & (DRM_GCN_FEATURE_SUBMIT_RGB565 |
				 DRM_GCN_FEATURE_FILL_RGB565 |
				 DRM_GCN_FEATURE_FILL_RECT_RGB565 |
				 DRM_GCN_FEATURE_BLIT_RECT_RGB565 |
				 DRM_GCN_FEATURE_BLIT_RECT_RGB565_UNEQUAL_DIMS |
				 DRM_GCN_FEATURE_BLIT_RECT_RGB565_SAME_OBJECT |
				 DRM_GCN_FEATURE_BLIT_SCALED_RGB565)) ==
		    (DRM_GCN_FEATURE_SUBMIT_RGB565 |
		     DRM_GCN_FEATURE_FILL_RGB565 |
		     DRM_GCN_FEATURE_FILL_RECT_RGB565 |
		     DRM_GCN_FEATURE_BLIT_RECT_RGB565 |
		     DRM_GCN_FEATURE_BLIT_RECT_RGB565_UNEQUAL_DIMS |
		     DRM_GCN_FEATURE_BLIT_RECT_RGB565_SAME_OBJECT |
			 DRM_GCN_FEATURE_BLIT_SCALED_RGB565))
			test_submit(fd, other_fd);
		else
			fail_value("RGB565 render features", features,
				   DRM_GCN_FEATURE_SUBMIT_RGB565 |
				   DRM_GCN_FEATURE_FILL_RGB565 |
				   DRM_GCN_FEATURE_FILL_RECT_RGB565 |
				   DRM_GCN_FEATURE_BLIT_RECT_RGB565 |
				   DRM_GCN_FEATURE_BLIT_RECT_RGB565_UNEQUAL_DIMS |
				   DRM_GCN_FEATURE_BLIT_RECT_RGB565_SAME_OBJECT |
				   DRM_GCN_FEATURE_BLIT_SCALED_RGB565);
		if (features & DRM_GCN_FEATURE_DRAW_TRIANGLE_RGB565)
			test_draw_triangle(fd);
		else
			fail_value("triangle render feature", features,
				   DRM_GCN_FEATURE_DRAW_TRIANGLE_RGB565);
		if (features & DRM_GCN_FEATURE_DRAW_TRIANGLES_RGB565)
			test_draw_triangles(fd);
		else
			fail_value("triangle-batch render feature", features,
				   DRM_GCN_FEATURE_DRAW_TRIANGLES_RGB565);
		if (features & DRM_GCN_FEATURE_DRAW_TRIANGLES_STATE_RGB565)
			test_draw_triangles_state(fd);
		else
			fail_value("stateful triangle-batch render feature", features,
				   DRM_GCN_FEATURE_DRAW_TRIANGLES_STATE_RGB565);
		if (features & DRM_GCN_FEATURE_DRAW_TRIANGLES_DEPTH_RGB565)
			test_draw_triangles_depth(fd);
		else
			fail_value("depth triangle-batch render feature", features,
				   DRM_GCN_FEATURE_DRAW_TRIANGLES_DEPTH_RGB565);
		if (features & DRM_GCN_FEATURE_DRAW_TEXTURED_TRIANGLES_RGB565)
			test_draw_textured_triangles(fd);
		else
			fail_value("textured triangle-batch render feature", features,
				   DRM_GCN_FEATURE_DRAW_TEXTURED_TRIANGLES_RGB565);
		if (features & DRM_GCN_FEATURE_DRAW_INDEXED_TRIANGLES_RGB565)
			test_draw_indexed_triangles(fd);
		else
			fail_value("indexed triangle-batch render feature", features,
				   DRM_GCN_FEATURE_DRAW_INDEXED_TRIANGLES_RGB565);
		if (features &
		    DRM_GCN_FEATURE_DRAW_INDEXED_TEXTURED_TRIANGLES_RGB565)
			test_draw_indexed_textured_triangles(fd);
		else
			fail_value("indexed textured render feature", features,
				   DRM_GCN_FEATURE_DRAW_INDEXED_TEXTURED_TRIANGLES_RGB565);
		if (features &
		    DRM_GCN_FEATURE_DRAW_INDEXED_TEXTURED_DEPTH_RGB565)
			test_draw_indexed_textured_depth(fd);
		else
			fail_value("indexed textured depth render feature", features,
				   DRM_GCN_FEATURE_DRAW_INDEXED_TEXTURED_DEPTH_RGB565);
		if (features & DRM_GCN_FEATURE_DRAW_INDEXED_FIXED_RGB565)
			test_draw_indexed_fixed(fd,
				features & DRM_GCN_FEATURE_RASTER_CULL);
		else
			fail_value("indexed fixed-function render feature", features,
				   DRM_GCN_FEATURE_DRAW_INDEXED_FIXED_RGB565);
		if ((features & DRM_GCN_FEATURE_TEXTURE_RGBA8) &&
		    (formats & DRM_GCN_FORMAT_RGBA8))
			test_rgba8_texture_alpha(fd);
		else
			fail_value("RGBA8 texture capability", features,
				   DRM_GCN_FEATURE_TEXTURE_RGBA8);
		if ((features & DRM_GCN_FEATURE_TEXTURE_LINEAR) &&
		    (features & DRM_GCN_FEATURE_TEXTURE_RGBA8) &&
		    (formats & DRM_GCN_FORMAT_RGBA8))
			test_linear_texture_filter(fd);
		else
			fail_value("linear texture capability", features,
				   DRM_GCN_FEATURE_TEXTURE_LINEAR);
		if (features & DRM_GCN_FEATURE_SYSTEM_RENDER_RGB565)
			test_full_system_render(fd);
		else
			fail_value("system RGB565 render feature", features,
				   DRM_GCN_FEATURE_SYSTEM_RENDER_RGB565);
		if (features & DRM_GCN_FEATURE_BLIT_SCALED_RGB565)
			test_wide_scaled_blit(fd);
		if (features & DRM_GCN_FEATURE_BLIT_SCALED_RGB565)
			test_wide_scaled_reduce(fd, -1, 0);
		if ((features & (DRM_GCN_FEATURE_SYSTEM_GEM |
				 DRM_GCN_FEATURE_BLIT_SCALED_SYSTEM_RGB565 |
				 DRM_GCN_FEATURE_SYSTEM_GEM_LINEAR |
				 DRM_GCN_FEATURE_BLIT_SCALED_SYSTEM_XRGB8888_TO_RGB565)) ==
		    (DRM_GCN_FEATURE_SYSTEM_GEM |
		     DRM_GCN_FEATURE_BLIT_SCALED_SYSTEM_RGB565 |
		     DRM_GCN_FEATURE_SYSTEM_GEM_LINEAR |
		     DRM_GCN_FEATURE_BLIT_SCALED_SYSTEM_XRGB8888_TO_RGB565)) {
			test_full_scaled_system_blit(fd);
			test_full_xrgb8888_to_rgb565_system_blit(fd);
		} else {
			fail_value("system-memory scaled features", features,
				   DRM_GCN_FEATURE_SYSTEM_GEM |
				   DRM_GCN_FEATURE_BLIT_SCALED_SYSTEM_RGB565 |
				   DRM_GCN_FEATURE_SYSTEM_GEM_LINEAR |
				   DRM_GCN_FEATURE_BLIT_SCALED_SYSTEM_XRGB8888_TO_RGB565);
		}
	}

out_close:
	if (system_trace_on >= 0) {
		if (pwrite(system_trace_on, "0", 1, 0) != 1)
			fail("disable scheduler trace");
		close(system_trace_on);
	}
	if (system_trace_marker >= 0)
		close(system_trace_marker);
	close(other_fd);
	close(fd);
	if (failures) {
		fprintf(stderr, "%d render UAPI test(s) failed\n", failures);
		return EXIT_FAILURE;
	}
	puts("PASS: GCN render UAPI");
	return EXIT_SUCCESS;
}
