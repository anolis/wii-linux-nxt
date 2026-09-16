// SPDX-License-Identifier: GPL-2.0-only
/* Render alternating linear GCN objects and page-flip them through KMS. */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <drm/gcn_drm.h>

#define SCALED_SRC_WIDTH 320U
#define SCALED_SRC_HEIGHT 240U
#define DST_WIDTH 640U
#define DST_HEIGHT 480U
#define NATIVE_TILE_WIDTH 320U
#define NATIVE_TILE_HEIGHT 240U
#define DEFAULT_FLIPS 120U
#define MAX_FLIPS 10000U
#define FLIP_TIMEOUT_MS 2000
#define TEST_DRM_MODE_CONNECTED 1

static bool swap_red_white;
static bool rotate_rgb;
static bool content_cycle;
static bool boundary_checks;
static unsigned int final_frame, verified_frames;

struct render_buffer {
	struct drm_gcn_gem_create bo;
	struct drm_mode_fb_cmd fb;
	void *map;
};

static int xioctl(int fd, unsigned long request, void *arg)
{
	int ret;

	do {
		ret = ioctl(fd, request, arg);
	} while (ret < 0 && errno == EINTR);
	return ret;
}

static __u64 user_ptr(const void *ptr)
{
	return (__u64)(uintptr_t)ptr;
}

static void *xcalloc(size_t count, size_t size)
{
	void *ptr = calloc(count, size);

	if (!ptr && count) {
		perror("calloc");
		exit(EXIT_FAILURE);
	}
	return ptr;
}

static uint64_t monotonic_ns(void)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
		return 0;
	return (uint64_t)now.tv_sec * 1000000000ULL + now.tv_nsec;
}

static uint32_t content_xrgb8888(unsigned int x, unsigned int y, unsigned int frame)
{
	static const uint16_t solid[] = { 0, 0xffff, 0xf800, 0x07e0, 0x001f };
	unsigned int pattern = frame % 8;
	uint32_t value;
	uint16_t pixel;
	uint32_t r, g, b;

	if (pattern < 5)
		pixel = solid[pattern];
	else if (pattern == 5)
		pixel = ((x + y + frame / 8) & 1) ? 0xffff : 0;
	else if (pattern == 6)
		pixel = 1U << ((x + y + frame / 8) & 15);
	else {
		value = (y * DST_WIDTH + x) ^ ((frame + 1U) * 0x9e3779b9U);
		value ^= value >> 16;
		value *= 0x7feb352dU;
		value ^= value >> 15;
		value *= 0x846ca68bU;
		pixel = (uint16_t)(value ^ (value >> 16));
	}
	r = (pixel >> 11) & 31;
	g = (pixel >> 5) & 63;
	b = pixel & 31;
	return (((r << 3) | (r >> 2)) << 16) |
	       (((g << 2) | (g >> 4)) << 8) | (b << 3) | (b >> 2);
}

static uint32_t pattern_xrgb8888(unsigned int x, unsigned int y,
				 unsigned int frame, unsigned int width,
				 unsigned int height)
{
	static const uint32_t colors[] = {
		0x00ff0000, 0x0000ff00, 0x000000ff, 0x00ffffff,
	};
	unsigned int border_x = width / 80;
	unsigned int border_y = height / 60;
	unsigned int checker_x = width / 64;
	unsigned int checker_y = height / 48;
	unsigned int marker_width = width / 40;
	unsigned int marker_step = width * 7U / SCALED_SRC_WIDTH;
	unsigned int marker_margin = height / 15;
	unsigned int marker = frame * marker_step % (width - marker_width);
	unsigned int quadrant = (y >= height / 2) * 2 + (x >= width / 2);
	unsigned int color_index = quadrant;
	uint32_t pixel;

	if (content_cycle)
		return content_xrgb8888(x, y, frame);
	if (swap_red_white && (color_index == 0 || color_index == 3))
		color_index = 3 - color_index;
	pixel = colors[color_index];

	if (x < border_x || x >= width - border_x || y < border_y ||
	    y >= height - border_y)
		pixel = 0x00ffffff;
	else if (!(x % (width / 8)) || !(y % (height / 8)))
		pixel = 0;
	if (x >= width * 3 / 8 && x < width * 5 / 8 &&
	    y >= height * 3 / 8 && y < height * 5 / 8)
		pixel = ((x / checker_x) ^ (y / checker_y)) & 1 ?
			0x00ff00ff : 0x00ffff00;
	if (x >= marker && x < marker + marker_width && y >= marker_margin &&
	    y < height - marker_margin)
		pixel = 0x0000ffff;

	/* Rotate the complete pattern: red -> green -> blue -> red. */
	if (rotate_rgb)
		pixel = ((pixel & 0xff) << 16) | ((pixel >> 8) & 0xffff);
	return pixel;
}

static uint16_t xrgb8888_to_rgb565(uint32_t pixel)
{
	return ((pixel >> 8) & 0xf800) |
	       ((pixel >> 5) & 0x07e0) |
	       ((pixel >> 3) & 0x001f);
}

static unsigned int scaled_source(unsigned int dst, unsigned int src_extent,
				  unsigned int dst_extent)
{
	uint64_t numerator = (uint64_t)(2 * dst + 1) * src_extent;
	unsigned int source = numerator / (2 * dst_extent);

	return source < src_extent ? source : src_extent - 1;
}

static int create_linear_bo(int fd, struct drm_gcn_gem_create *bo,
			    unsigned int width, unsigned int height,
			    __u32 format, void **map)
{
	struct drm_gcn_gem_mmap mmap_args = {};

	*bo = (struct drm_gcn_gem_create) {
		.width = width,
		.height = height,
		.format = format,
		.layout = DRM_GCN_GEM_LAYOUT_LINEAR,
		.flags = DRM_GCN_GEM_CREATE_SYSTEM,
	};
	if (xioctl(fd, DRM_IOCTL_GCN_GEM_CREATE, bo) < 0)
		return -1;

	mmap_args.handle = bo->handle;
	if (xioctl(fd, DRM_IOCTL_GCN_GEM_MMAP, &mmap_args) < 0)
		return -1;
	*map = mmap(NULL, bo->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
		    mmap_args.offset);
	return *map == MAP_FAILED ? -1 : 0;
}

static int close_bo(int fd, struct drm_gcn_gem_create *bo, void *map)
{
	struct drm_gem_close close_args = { .handle = bo->handle };

	int ret = 0;

	if (map != MAP_FAILED && munmap(map, bo->size))
		ret = -1;
	if (bo->handle && xioctl(fd, DRM_IOCTL_GEM_CLOSE, &close_args))
		ret = -1;
	return ret;
}

static int get_resources(int fd, struct drm_mode_card_res *res,
			 __u32 **crtcs, __u32 **connectors)
{
	memset(res, 0, sizeof(*res));
	if (xioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, res) < 0)
		return -1;
	*crtcs = xcalloc(res->count_crtcs, sizeof(**crtcs));
	*connectors = xcalloc(res->count_connectors, sizeof(**connectors));
	res->crtc_id_ptr = user_ptr(*crtcs);
	res->connector_id_ptr = user_ptr(*connectors);
	res->count_fbs = 0;
	res->count_encoders = 0;
	return xioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, res);
}

static int get_connector(int fd, __u32 id,
			 struct drm_mode_get_connector *connector,
			 struct drm_mode_modeinfo **modes)
{
	__u32 *encoders;
	__u32 *props;
	__u64 *values;

	memset(connector, 0, sizeof(*connector));
	connector->connector_id = id;
	if (xioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, connector) < 0)
		return -1;

	*modes = xcalloc(connector->count_modes, sizeof(**modes));
	encoders = xcalloc(connector->count_encoders, sizeof(*encoders));
	props = xcalloc(connector->count_props, sizeof(*props));
	values = xcalloc(connector->count_props, sizeof(*values));
	connector->modes_ptr = user_ptr(*modes);
	connector->encoders_ptr = user_ptr(encoders);
	connector->props_ptr = user_ptr(props);
	connector->prop_values_ptr = user_ptr(values);
	if (xioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, connector) < 0) {
		free(*modes);
		*modes = NULL;
		free(encoders);
		free(props);
		free(values);
		return -1;
	}
	free(encoders);
	free(props);
	free(values);
	return 0;
}

static int select_output(int fd, const struct drm_mode_card_res *res,
			 const __u32 *connector_ids, __u32 *connector_id,
			 struct drm_mode_modeinfo *mode)
{
	unsigned int i;

	for (i = 0; i < res->count_connectors; i++) {
		struct drm_mode_get_connector connector;
		struct drm_mode_modeinfo *modes = NULL;
		unsigned int j;

		if (get_connector(fd, connector_ids[i], &connector, &modes) < 0)
			continue;
		if (connector.connection != TEST_DRM_MODE_CONNECTED) {
			free(modes);
			continue;
		}
		for (j = 0; j < connector.count_modes; j++) {
			if (modes[j].hdisplay == DST_WIDTH &&
			    modes[j].vdisplay == DST_HEIGHT)
				break;
		}
		if (j < connector.count_modes) {
			*connector_id = connector.connector_id;
			*mode = modes[j];
			free(modes);
			return 0;
		}
		free(modes);
	}
	errno = ENODEV;
	return -1;
}

static int add_framebuffer(int fd, struct render_buffer *buffer)
{
	buffer->fb = (struct drm_mode_fb_cmd) {
		.width = DST_WIDTH,
		.height = DST_HEIGHT,
		.pitch = DST_WIDTH * sizeof(uint16_t),
		.bpp = 16,
		.depth = 16,
		.handle = buffer->bo.handle,
	};
	return xioctl(fd, DRM_IOCTL_MODE_ADDFB, &buffer->fb);
}

static int render_frame(int fd, __u32 ctx_id,
			struct drm_gcn_gem_create *src, void *src_map,
			struct render_buffer *dst, unsigned int frame,
			bool native, bool native_tiled, uint64_t *render_ns)
{
	struct drm_gcn_blit_scaled blit = {
		.ctx_id = ctx_id,
		.src_handle = src->handle,
		.dst_handle = dst->bo.handle,
	};
	struct drm_gcn_wait wait = {
		.handle = dst->bo.handle,
		.flags = DRM_GCN_WAIT_WRITE,
	};
	unsigned int x;
	unsigned int y;
	uint64_t start;

	for (y = 0; y < src->height; y++) {
		for (x = 0; x < src->width; x++) {
			uint32_t pixel = pattern_xrgb8888(x, y, frame,
						      src->width, src->height);
			size_t offset = (size_t)y * src->width + x;

			if (src->format == DRM_GCN_GEM_FORMAT_XRGB8888)
				((uint32_t *)src_map)[offset] = pixel;
			else
				((uint16_t *)src_map)[offset] =
					xrgb8888_to_rgb565(pixel);
		}
	}
	memset(dst->map, 0x5a,
	       DST_WIDTH * DST_HEIGHT * sizeof(uint16_t));
	start = monotonic_ns();
	if (native_tiled) {
		for (y = 0; y < DST_HEIGHT; y += NATIVE_TILE_HEIGHT) {
			for (x = 0; x < DST_WIDTH; x += NATIVE_TILE_WIDTH) {
				blit.src_x = x;
				blit.src_y = y;
				blit.src_width = NATIVE_TILE_WIDTH;
				blit.src_height = NATIVE_TILE_HEIGHT;
				blit.dst_x = x;
				blit.dst_y = y;
				blit.dst_width = NATIVE_TILE_WIDTH;
				blit.dst_height = NATIVE_TILE_HEIGHT;
				if (xioctl(fd, DRM_IOCTL_GCN_BLIT_SCALED, &blit) < 0)
					return -1;
			}
		}
	} else {
		blit.src_width = src->width;
		blit.src_height = src->height;
		blit.dst_width = DST_WIDTH;
		blit.dst_height = DST_HEIGHT;
		if (xioctl(fd, DRM_IOCTL_GCN_BLIT_SCALED, &blit) < 0)
			return -1;
	}
	wait.timeout_ns = monotonic_ns() + 5000000000ULL;
	if (xioctl(fd, DRM_IOCTL_GCN_WAIT, &wait) < 0)
		return -1;
	*render_ns = monotonic_ns() - start;

	if (boundary_checks && frame != 0 && frame != final_frame)
		return 0;

	for (y = 0; y < DST_HEIGHT; y++) {
		for (x = 0; x < DST_WIDTH; x++) {
			unsigned int sx = native ? x :
				scaled_source(x, src->width, DST_WIDTH);
			unsigned int sy = native ? y :
				scaled_source(y, src->height, DST_HEIGHT);
			uint32_t source = pattern_xrgb8888(sx, sy, frame,
						       src->width, src->height);
			uint16_t expected = xrgb8888_to_rgb565(source);
			uint16_t actual = ((uint16_t *)dst->map)
				[(size_t)y * DST_WIDTH + x];

			if (actual != expected) {
				fprintf(stderr,
					"frame %u mismatch at (%u,%u): got=%04x expected=%04x\n",
					frame, x, y, actual, expected);
				errno = EIO;
				return -1;
			}
		}
	}
	verified_frames++;
	if (boundary_checks)
		printf("gcn-kms-flip-test: verified frame=%u\n", frame);
	return 0;
}

static int wait_flip_event(int fd, __u64 expected, __u32 *sequence, uint64_t *event_ns)
{
	unsigned char data[256];
	struct pollfd poll_fd = { .fd = fd, .events = POLLIN };
	ssize_t length;
	size_t offset;
	int ret;

	do {
		ret = poll(&poll_fd, 1, FLIP_TIMEOUT_MS);
	} while (ret < 0 && errno == EINTR);
	if (ret <= 0) {
		if (!ret)
			errno = ETIMEDOUT;
		return -1;
	}
	length = read(fd, data, sizeof(data));
	if (length < 0)
		return -1;

	for (offset = 0; offset + sizeof(struct drm_event) <= (size_t)length;) {
		const struct drm_event *event = (const void *)(data + offset);
		const struct drm_event_vblank *vblank;

		if (event->length < sizeof(*event) ||
		    offset + event->length > (size_t)length) {
			errno = EPROTO;
			return -1;
		}
		if (event->type == DRM_EVENT_FLIP_COMPLETE) {
			if (event->length < sizeof(*vblank)) {
				errno = EPROTO;
				return -1;
			}
			vblank = (const void *)event;
			if (vblank->user_data != expected) {
				errno = EPROTO;
				return -1;
			}
			*sequence = vblank->sequence;
			*event_ns = (uint64_t)vblank->tv_sec * 1000000000ULL +
				    (uint64_t)vblank->tv_usec * 1000;
			return 0;
		}
		offset += event->length;
	}
	errno = EPROTO;
	return -1;
}

static unsigned int parse_flips(const char *value)
{
	char *end;
	unsigned long parsed;

	errno = 0;
	parsed = strtoul(value, &end, 10);
	if (errno || !*value || *end || !parsed || parsed > MAX_FLIPS) {
		fprintf(stderr, "invalid flip count: %s\n", value);
		exit(EXIT_FAILURE);
	}
	return parsed;
}

static __u32 parse_source_format(const char *value, bool *native,
				 bool *native_tiled)
{
	*native = false;
	*native_tiled = false;
	if (!strcmp(value, "rgb565"))
		return DRM_GCN_GEM_FORMAT_RGB565;
	if (!strcmp(value, "xrgb8888"))
		return DRM_GCN_GEM_FORMAT_XRGB8888;
	if (!strcmp(value, "xrgb8888-native")) {
		*native = true;
		return DRM_GCN_GEM_FORMAT_XRGB8888;
	}
	if (!strcmp(value, "xrgb8888-native-tiled")) {
		*native = true;
		*native_tiled = true;
		return DRM_GCN_GEM_FORMAT_XRGB8888;
	}
	fprintf(stderr, "invalid source format: %s\n", value);
	exit(EXIT_FAILURE);
}

static const char *source_format_name(__u32 format, bool native,
				      bool native_tiled)
{
	if (native_tiled)
		return "xrgb8888-native-tiled";
	if (native)
		return "xrgb8888-native";
	return format == DRM_GCN_GEM_FORMAT_XRGB8888 ? "xrgb8888" : "rgb565";
}

static int run_offscreen(int fd, __u32 ctx_id,
			 struct drm_gcn_gem_create *src, void *src_map,
			 struct render_buffer *buffers, unsigned int count,
			 bool native, bool native_tiled)
{
	unsigned int frame;

	for (frame = 0; frame <= count; frame++) {
		uint64_t render_ns;

		if (content_cycle) {
			printf("gcn-kms-flip-test: native-content frame=%u pattern=%u seed=%08x\n",
			       frame, frame % 8, (frame + 1U) * 0x9e3779b9U);
			fflush(stdout);
		}
		if (render_frame(fd, ctx_id, src, src_map, &buffers[frame & 1],
				 frame, native, native_tiled, &render_ns) < 0) {
			perror("render offscreen buffer");
			return EXIT_FAILURE;
		}
		if (!(frame % 30)) {
			printf("gcn-kms-flip-test: offscreen frame=%u render-us=%llu\n",
			       frame, (unsigned long long)(render_ns / 1000));
			fflush(stdout);
		}
	}
	printf("gcn-kms-flip-test: PASS offscreen format=%s frames=%u pixels=%llu\n",
	       source_format_name(src->format, native, native_tiled), count + 1,
	       (unsigned long long)(count + 1) * DST_WIDTH * DST_HEIGHT);
	return EXIT_SUCCESS;
}

int main(int argc, char **argv)
{
	const char *card = argc > 1 ? argv[1] : "/dev/dri/card0";
	unsigned int flip_count = argc > 2 ? parse_flips(argv[2]) : DEFAULT_FLIPS;
	const char *format_arg = argc > 3 ? argv[3] : "rgb565";
	bool offscreen = argc > 4 && !strcmp(argv[4], "--offscreen");
	bool native;
	bool native_tiled;
	__u32 src_format = parse_source_format(format_arg, &native,
					       &native_tiled);
	unsigned int src_width = native ? DST_WIDTH : SCALED_SRC_WIDTH;
	unsigned int src_height = native ? DST_HEIGHT : SCALED_SRC_HEIGHT;
	struct drm_gcn_gem_create src = {};
	struct render_buffer buffers[2] = {
		{ .map = MAP_FAILED },
		{ .map = MAP_FAILED },
	};
	struct drm_gcn_ctx_create ctx = {};
	struct drm_gcn_ctx_free free_ctx = {};
	struct drm_mode_card_res resources;
	struct drm_mode_modeinfo mode;
	struct drm_mode_crtc old_crtc = {};
	struct drm_mode_crtc set_crtc = {};
	__u32 *connector_ids = NULL;
	__u32 *crtc_ids = NULL;
	__u32 connector_id = 0;
	__u32 last_sequence = 0;
	struct drm_gcn_get_param free_before = { .param = DRM_GCN_PARAM_MEM1_FREE_BYTES };
	struct drm_gcn_get_param free_after = { .param = DRM_GCN_PARAM_MEM1_FREE_BYTES };
	bool memory_baseline = false;
	uint64_t last_event_ns = 0, interval_total = 0, interval_min = UINT64_MAX, interval_max = 0;
	unsigned int gaps[9] = {};
	uint64_t max_render_ns = 0;
	uint64_t total_render_ns = 0;
	void *src_map = MAP_FAILED;
	unsigned int completed = 0;
	unsigned int created = 0;
	int displayed = 0;
	int fd = -1;
	int ret = EXIT_FAILURE;

	boundary_checks = argc == 5 && !strcmp(argv[4], "--boundary-checks");
	final_frame = flip_count;
	if (argc > 6 || (argc > 4 && !offscreen && !boundary_checks) ||
	    (argc > 5 && strcmp(argv[5], "--swap-red-white") &&
	     strcmp(argv[5], "--rotate-rgb") && strcmp(argv[5], "--content-cycle"))) {
		fprintf(stderr,
			"usage: %s [card [count [format [--boundary-checks|--offscreen [--swap-red-white|--rotate-rgb|--content-cycle]]]]]\n",
			argv[0]);
		return EXIT_FAILURE;
	}
	content_cycle = argc > 5 && !strcmp(argv[5], "--content-cycle");
	if (content_cycle && (!offscreen || !native_tiled)) {
		fprintf(stderr, "content cycle requires offscreen xrgb8888-native-tiled\n");
		return EXIT_FAILURE;
	}
	swap_red_white = argc > 5 && !strcmp(argv[5], "--swap-red-white");
	rotate_rgb = argc > 5 && !strcmp(argv[5], "--rotate-rgb");
	if (swap_red_white || rotate_rgb) {
		printf("gcn-kms-flip-test: diagnostic palette=%s\n",
		       swap_red_white ? "swap-red-white" : "rotate-rgb");
		fflush(stdout);
	}
	fd = open(card, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		perror(card);
		goto out;
	}
	if (!offscreen && xioctl(fd, DRM_IOCTL_SET_MASTER, NULL) < 0 &&
	    errno != EINVAL) {
		perror("DRM_IOCTL_SET_MASTER");
		goto out;
	}
	if (xioctl(fd, DRM_IOCTL_GCN_GET_PARAM, &free_before)) {
		perror("query initial MEM1 accounting");
		goto out;
	}
	memory_baseline = true;
	if (create_linear_bo(fd, &src, src_width, src_height, src_format,
			     &src_map) < 0) {
		perror("create linear source");
		goto out;
	}
	for (created = 0; created < 2; created++) {
		if (create_linear_bo(fd, &buffers[created].bo, DST_WIDTH,
				     DST_HEIGHT, DRM_GCN_GEM_FORMAT_RGB565,
				     &buffers[created].map) < 0 ||
		    (!offscreen && add_framebuffer(fd, &buffers[created]) < 0)) {
			perror("create linear scanout buffer");
			created++;
			goto out;
		}
	}
	if (xioctl(fd, DRM_IOCTL_GCN_CTX_CREATE, &ctx) < 0) {
		perror("DRM_IOCTL_GCN_CTX_CREATE");
		goto out;
	}
	if (offscreen) {
		ret = run_offscreen(fd, ctx.id, &src, src_map, buffers, flip_count,
				    native, native_tiled);
		goto out;
	}
	if (get_resources(fd, &resources, &crtc_ids, &connector_ids) < 0 ||
	    !resources.count_crtcs ||
	    select_output(fd, &resources, connector_ids, &connector_id,
			  &mode) < 0) {
		perror("select DRM output");
		goto out;
	}
	old_crtc.crtc_id = crtc_ids[0];
	if (xioctl(fd, DRM_IOCTL_MODE_GETCRTC, &old_crtc) < 0) {
		perror("DRM_IOCTL_MODE_GETCRTC");
		goto out;
	}
	{
		uint64_t render_ns;

		if (render_frame(fd, ctx.id, &src, src_map, &buffers[0], 0,
				 native, native_tiled, &render_ns) < 0) {
			perror("render initial frame");
			goto out;
		}
		total_render_ns = render_ns;
		max_render_ns = render_ns;
	}
	set_crtc = (struct drm_mode_crtc) {
		.set_connectors_ptr = user_ptr(&connector_id),
		.count_connectors = 1,
		.crtc_id = crtc_ids[0],
		.fb_id = buffers[0].fb.fb_id,
		.mode_valid = 1,
		.mode = mode,
	};
	if (xioctl(fd, DRM_IOCTL_MODE_SETCRTC, &set_crtc) < 0) {
		perror("DRM_IOCTL_MODE_SETCRTC");
		goto out;
	}
	displayed = 1;
	printf("gcn-kms-flip-test: %s initial frame verified, running %u flips\n",
	       source_format_name(src_format, native, native_tiled), flip_count);
	fflush(stdout);

	for (completed = 0; completed < flip_count; completed++) {
		unsigned int frame = completed + 1;
		struct render_buffer *next = &buffers[frame & 1];
		struct drm_mode_crtc_page_flip flip = {
			.crtc_id = crtc_ids[0],
			.fb_id = next->fb.fb_id,
			.flags = DRM_MODE_PAGE_FLIP_EVENT,
			.user_data = frame,
		};
		__u32 sequence;
		uint64_t render_ns, event_ns;

		if (render_frame(fd, ctx.id, &src, src_map, next, frame,
				 native, native_tiled, &render_ns) < 0) {
			perror("render back buffer");
			goto out;
		}
		total_render_ns += render_ns;
		if (render_ns > max_render_ns)
			max_render_ns = render_ns;
		if (xioctl(fd, DRM_IOCTL_MODE_PAGE_FLIP, &flip) < 0) {
			perror("DRM_IOCTL_MODE_PAGE_FLIP");
			goto out;
		}
		if (wait_flip_event(fd, flip.user_data, &sequence, &event_ns) < 0) {
			perror("wait page-flip event");
			goto out;
		}
		if (last_sequence && (__u32)(sequence - last_sequence) == 0) {
			fprintf(stderr, "vblank sequence did not advance at frame %u\n",
				frame);
			goto out;
		}
		if (completed) {
			uint64_t interval;
			__u32 gap = sequence - last_sequence;

			if (!gap || event_ns <= last_event_ns) {
				fprintf(stderr, "non-monotonic presentation event\n");
				goto out;
			}
			interval = event_ns - last_event_ns;
			interval_total += interval;
			if (interval < interval_min) interval_min = interval;
			if (interval > interval_max) interval_max = interval;
			gaps[gap < 9 ? gap - 1 : 8]++;
		}
		last_event_ns = event_ns;
		last_sequence = sequence;
		if (!(frame % 30)) {
			printf("gcn-kms-flip-test: frame=%u vblank=%u render-us=%llu\n",
			       frame, sequence,
			       (unsigned long long)(render_ns / 1000));
			fflush(stdout);
		}
	}
	if (completed > 1) {
		printf("gcn-kms-flip-test: pacing intervals=%u total-ns=%llu min-ns=%llu max-ns=%llu gaps=",
		       completed - 1, (unsigned long long)interval_total,
		       (unsigned long long)interval_min, (unsigned long long)interval_max);
		for (unsigned int i = 0; i < 9; i++)
			printf("%s%u", i ? "," : "", gaps[i]);
		puts("");
	}
	if (boundary_checks)
		printf("gcn-kms-flip-test: verification mode=boundary frames=%u\n", verified_frames);
	printf("gcn-kms-flip-test: PASS format=%s frames=%u last-vblank=%u pixels=%u",
	       source_format_name(src_format, native, native_tiled), completed + 1,
	       last_sequence, verified_frames * DST_WIDTH * DST_HEIGHT);
	printf(" render-us-avg=%llu render-us-max=%llu\n",
	       (unsigned long long)(total_render_ns / (completed + 1) / 1000),
	       (unsigned long long)(max_render_ns / 1000));
	ret = EXIT_SUCCESS;

out:
	if (displayed) {
		old_crtc.set_connectors_ptr = user_ptr(&connector_id);
		old_crtc.count_connectors = 1;
		if (xioctl(fd, DRM_IOCTL_MODE_SETCRTC, &old_crtc) < 0) {
			perror("restore DRM CRTC");
			ret = EXIT_FAILURE;
		} else {
			puts("gcn-kms-flip-test: restored previous console framebuffer");
		}
	}
	while (created) {
		struct render_buffer *buffer = &buffers[--created];

		if (buffer->fb.fb_id && xioctl(fd, DRM_IOCTL_MODE_RMFB, &buffer->fb.fb_id))
			ret = EXIT_FAILURE;
		if (close_bo(fd, &buffer->bo, buffer->map))
			ret = EXIT_FAILURE;
	}
	if (ctx.id) {
		free_ctx.id = ctx.id;
		if (xioctl(fd, DRM_IOCTL_GCN_CTX_FREE, &free_ctx))
			ret = EXIT_FAILURE;
	}
	if (close_bo(fd, &src, src_map))
		ret = EXIT_FAILURE;
	if (memory_baseline) {
		if (xioctl(fd, DRM_IOCTL_GCN_GET_PARAM, &free_after) || free_after.value != free_before.value) {
			fprintf(stderr, "MEM1 accounting did not recover\n");
			ret = EXIT_FAILURE;
		} else {
			printf("gcn-kms-flip-test: MEM1 recovered bytes=%llu\n",
			       (unsigned long long)free_after.value);
		}
	}
	free(crtc_ids);
	free(connector_ids);
	if (fd >= 0)
		close(fd);
	return ret;
}
