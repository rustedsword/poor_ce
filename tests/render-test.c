/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include <assert.h>
#include <drm/drm_fourcc.h>
#include <poll.h>
#include <poor_damage_buffers.h>
#include <poor_poll.h>
#include <poor_stdio.h>
#include <poor_unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <vk_init.h>

#include "damage.h"
#include "editor.h"
#include "keep-loaded.h"
#include "logger.h"
#include "render.h"
#include "scene.h"
#include "test-loop.h"
#include "test-runner.h"

static const uint64_t LINEAR[] = { DRM_FORMAT_MOD_LINEAR };

/* B8G8R8A8 pixels at most 'tolerance' off 'color', which is 0xRRGGBB, in each channel */
static bool within(uint32_t pixel, uint32_t color, int tolerance)
{
	for (unsigned shift = 0; shift < 24; shift += 8) {
		int channel = pixel >> shift & 0xff, expected = color >> shift & 0xff;

		if (channel - expected > tolerance || expected - channel > tolerance)
			return false;
	}
	return true;
}

static bool near(uint32_t pixel, uint32_t color)
{
	return within(pixel, color, 1);
}

/* Whether some pixel of the cell at 'x', 'y' of 'width' x 'height' pixels passes 'test' */
static bool cell_has(size_t height, size_t width, const uint32_t (*pixels)[height][width], int32_t x, int32_t y,
		     const struct font *font, bool (*test)(uint32_t))
{
	foreach_array_const_ref(arrview(y, font->cell_height, pixels), row)
		foreach_array_const_ref(arrview(x, font->cell_width, row), pixel)
			if (test(*pixel))
				return true;
	return false;
}

static bool dark(uint32_t pixel)
{
	return (pixel & 0xff) < 0x60;
}

static bool white(uint32_t pixel)
{
	return near(pixel, 0xffffff);
}

/* Pixels that the CPU reads, in host visible memory */
struct readback {
	VkBuffer buffer;
	VkDeviceMemory memory;
	void *map;
};

static void readback_init(struct readback *readback, const struct renderer *renderer, VkDeviceSize size)
{
	auto info = xVkBufferCreateInfo(
		.pNext = &xVkBufferUsageFlags2CreateInfo(.usage = VK_BUFFER_USAGE_2_TRANSFER_DST_BIT),
		.size = size,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	);
	auto requirements = xVkMemoryRequirements2();
	auto properties = xVkPhysicalDeviceMemoryProperties2();
	const VkPhysicalDeviceMemoryProperties *memory = &properties.memoryProperties;
	auto allocate = xVkMemoryAllocateInfo();
	VkMemoryPropertyFlags flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

	assert(vkCreateBuffer(renderer->device, &info, nullptr, &readback->buffer) == VK_SUCCESS);
	vkGetBufferMemoryRequirements2(renderer->device, &xVkBufferMemoryRequirementsInfo2(.buffer = readback->buffer),
				       &requirements);
	vkGetPhysicalDeviceMemoryProperties2(renderer->physical, &properties);
	allocate.allocationSize = requirements.memoryRequirements.size;
	allocate.memoryTypeIndex = UINT32_MAX;
	foreach_array_index(arrview_first(memory->memoryTypeCount, memory->memoryTypes), i)
		if (allocate.memoryTypeIndex == UINT32_MAX && requirements.memoryRequirements.memoryTypeBits & 1u << i &&
		    (memory->memoryTypes[i].propertyFlags & flags) == flags)
			allocate.memoryTypeIndex = i;
	assert(vkAllocateMemory(renderer->device, &allocate, nullptr, &readback->memory) == VK_SUCCESS);
	auto bind = xVkBindBufferMemoryInfo(.buffer = readback->buffer, .memory = readback->memory);
	auto map = xVkMemoryMapInfo(.memory = readback->memory, .size = VK_WHOLE_SIZE);
	assert(vkBindBufferMemory2(renderer->device, 1, &bind) == VK_SUCCESS &&
	       vkMapMemory2(renderer->device, &map, &readback->map) == VK_SUCCESS);
}

static void readback_fini(struct readback *readback, const struct renderer *renderer)
{
	vkDestroyBuffer(renderer->device, readback->buffer, nullptr);
	vkFreeMemory(renderer->device, readback->memory, nullptr);
}

/* The image comes back from the compositor's queue family and returns to it, as if the compositor had read it */
static void read_image(struct renderer *renderer, VkImage image, uint32_t width, uint32_t height, struct readback *readback)
{
	auto commands_info = xVkCommandBufferAllocateInfo(
		.commandPool = renderer->pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
	);
	auto begin = xVkCommandBufferBeginInfo(.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);
	auto acquire = xVkImageMemoryBarrier2(
		.srcStageMask = VK_PIPELINE_STAGE_2_NONE,
		.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
		.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
		.oldLayout = VK_IMAGE_LAYOUT_GENERAL,
		.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT,
		.dstQueueFamilyIndex = renderer->family,
		.image = image,
		.subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1 },
	);
	auto copy = xVkCopyImageToBufferInfo2(
		.srcImage = image,
		.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		.dstBuffer = readback->buffer,
		.regionCount = 1,
		.pRegions = &xVkBufferImageCopy2(
			.imageSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1 },
			.imageExtent = { width, height, 1 },
		),
	);
	auto release = xVkImageMemoryBarrier2(
		.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
		.dstStageMask = VK_PIPELINE_STAGE_2_NONE,
		.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		.newLayout = VK_IMAGE_LAYOUT_GENERAL,
		.srcQueueFamilyIndex = renderer->family,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT,
		.image = image,
		.subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1 },
	);
	auto host = xVkBufferMemoryBarrier2(
		.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
		.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
		.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
		.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.buffer = readback->buffer,
		.size = VK_WHOLE_SIZE,
	);
	VkCommandBuffer commands;
	VkFence done;

	assert(vkAllocateCommandBuffers(renderer->device, &commands_info, &commands) == VK_SUCCESS &&
	       vkCreateFence(renderer->device, &xVkFenceCreateInfo(), nullptr, &done) == VK_SUCCESS &&
	       vkBeginCommandBuffer(commands, &begin) == VK_SUCCESS);
	vkCmdPipelineBarrier2(commands, &xVkDependencyInfo(.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &acquire));
	vkCmdCopyImageToBuffer2(commands, &copy);
	vkCmdPipelineBarrier2(commands, &xVkDependencyInfo(.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &release));
	vkCmdPipelineBarrier2(commands, &xVkDependencyInfo(.bufferMemoryBarrierCount = 1, .pBufferMemoryBarriers = &host));
	auto submit = xVkSubmitInfo2(
		.commandBufferInfoCount = 1,
		.pCommandBufferInfos = &xVkCommandBufferSubmitInfo(.commandBuffer = commands),
	);
	assert(vkEndCommandBuffer(commands) == VK_SUCCESS && vkQueueSubmit2(renderer->queue, 1, &submit, done) == VK_SUCCESS &&
	       vkWaitForFences(renderer->device, 1, &done, VK_TRUE, UINT64_MAX) == VK_SUCCESS);
	vkDestroyFence(renderer->device, done, nullptr);
	vkFreeCommandBuffers(renderer->device, renderer->pool, 1, &commands);
}

/* A renderer with a buffer that a swapchain would present, and host memory that its image is copied to */
struct target {
	struct renderer renderer;
	struct poor_wl_swapchain swapchain;
	struct readback readback;
	struct font font;
	struct atlas atlas;
	struct frame frame;
};

/* Without a GPU, as in CI, there is nothing to draw with. The validation layer keeps its log open from the creation of
 * the instance, also when there is no device. */
static bool target_init(struct target *target, struct poor_loop *loop, uint32_t width, uint32_t height)
{
	struct poor_wl_swapchain_buffer *buffer = &target->swapchain.buffers[0];
	uint32_t minor;
	bool ready;

	loop_init(loop);
	log_init(loop, "render-test");
	assert(font_init(&target->font, 15));
	atlas_init(&target->atlas, &target->font);
	DISABLE_LEAK_CHECKS;
	ready = renderer_init(&target->renderer, loop, &target->atlas, makedev(226, 128), &minor);
	keep_loaded();
	if (!ready) {
		renderer_fini(&target->renderer);
		font_fini(&target->font);
		run_until(loop, !log_pending());
		loop_fini(loop);
		return false;
	}
	target->renderer.vulkan.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	buffer->swapchain = &target->swapchain;
	buffer->width = width;
	buffer->height = height;
	assert(!renderer_allocator.allocate(&target->renderer, buffer, RENDER_DRM_FORMAT, ARRAY_SIZE(LINEAR), &LINEAR, false));
	assert(buffer->modifier == DRM_FORMAT_MOD_LINEAR && buffer->plane_count == 1);
	readback_init(&target->readback, &target->renderer, width * height * 4);
	return true;
}

static void target_redraw_impl(struct target *target, struct poor_loop *loop, uint32_t background, const struct frame *frame,
			       size_t rect_count, const struct poor_damage_rect (*rects)[rect_count])
{
	struct poor_wl_swapchain_buffer *buffer = &target->swapchain.buffers[0];
	struct pollfd fence = { .events = POLLIN };

	fence.fd = renderer_draw(&target->renderer, buffer, background, arrview_first(frame->count, frame->quads), rects);
	assert(fence.fd >= 0 && poll_array(array_ptr(&fence), 5000) == 1);
	poor_loop_close_fd_or_submit(loop, fence.fd, 0);
	read_image(&target->renderer, ((struct poor_wl_vulkan_image *)buffer->data)->image, buffer->width, buffer->height,
		   &target->readback);
}

#define target_redraw(target, loop, background, frame, rects) \
	target_redraw_impl(target, loop, background, frame, ARRAY_SIZE(rects), &auto_arr(rects))

static void target_draw(struct target *target, struct poor_loop *loop, uint32_t background, const struct frame *frame)
{
	const struct poor_wl_swapchain_buffer *buffer = &target->swapchain.buffers[0];
	const struct poor_damage_rect whole[] = { { 0, 0, buffer->width, buffer->height } };

	target_redraw(target, loop, background, frame, whole);
}

static void target_fini(struct target *target, struct poor_loop *loop)
{
	struct poor_wl_swapchain_buffer *buffer = &target->swapchain.buffers[0];

	readback_fini(&target->readback, &target->renderer);
	poor_loop_close_fd_or_submit(loop, buffer->planes[0].fd, 0);
	renderer_allocator.free(&target->renderer, buffer);
	renderer_fini(&target->renderer);
	font_fini(&target->font);
	run_until(loop, !log_pending());
	loop_fini(loop);
}

/* SNAPSHOT names a PPM file that gets the image, to look at it */
static void snapshot(size_t height, size_t width, const uint32_t (*pixels)[height][width])
{
	const char *path = getenv("SNAPSHOT");
	FILE *file;

	if (!path)
		return;
	assert((file = fopen(path, "wb")));
	fprintf(file, "P6\n%zu %zu\n255\n", width, height);
	foreach_array_const_ref(pixels, row)
		foreach_array_const_ref(row, pixel)
			fwrite_array(((const uint8_t[]){ *pixel >> 16 & 0xff, *pixel >> 8 & 0xff, *pixel & 0xff }), file);
	fclose(file);
}

/* A blue quad over white, and a dark glyph that covers a cell partly */
/* A first frame of solid quads only uses the atlas too, which has nothing yet */
TEST(render_solid_quads)
{
	static struct target target;
	struct poor_loop loop;

	if (!target_init(&target, &loop, 200, 100))
		return;
	target.frame.quads[0] = (struct quad){ .x = 20, .y = 10, .width = 30, .height = 40, .u = QUAD_SOLID, .v = 0,
					       .color = QUAD_RGB(0x3373d6) };
	target.frame.count = 1;
	target_draw(&target, &loop, QUAD_RGB(0xffffff), &target.frame);

	auto pixels = (const uint32_t (*)[100][200])target.readback.map;
	assert(white((*pixels)[99][199]) && near((*pixels)[10][20], 0x3373d6) && near((*pixels)[49][49], 0x3373d6));
	target_fini(&target, &loop);
}

TEST(render_quads)
{
	static struct target target;
	struct poor_loop loop;
	uint16_t u, v;

	if (!target_init(&target, &loop, 200, 100))
		return;
	assert(atlas_find(&target.atlas, 'a', &u, &v));
	target.frame.quads[0] = (struct quad){ .x = 20, .y = 10, .width = 30, .height = 40, .u = QUAD_SOLID, .v = 0,
					       .color = QUAD_RGB(0x3373d6) };
	target.frame.quads[1] = (struct quad){ .x = 100, .y = 10, .width = target.font.cell_width,
					       .height = target.font.cell_height, .u = u, .v = v, .color = QUAD_RGB(0x1e1e1e) };
	target.frame.count = 2;
	target_draw(&target, &loop, QUAD_RGB(0xffffff), &target.frame);

	auto pixels = (const uint32_t (*)[100][200])target.readback.map;
	assert(white((*pixels)[99][199]) && white((*pixels)[9][20]) && white((*pixels)[10][50]));
	assert(near((*pixels)[10][20], 0x3373d6) && near((*pixels)[49][49], 0x3373d6));
	assert(cell_has(100, 200, pixels, 100, 10, &target.font, dark) && cell_has(100, 200, pixels, 100, 10, &target.font, white));
	target_fini(&target, &loop);
}

TEST(render_redraws_rects)
{
	static struct target target;
	static struct frame moved;
	static uint32_t redrawn[100][200];
	const struct poor_damage_rect rects[] = { { 10, 10, 20, 20 }, { 120, 60, 20, 20 } };
	struct poor_loop loop;

	if (!target_init(&target, &loop, 200, 100))
		return;
	target.frame.quads[0] = (struct quad){ .x = 0, .y = 0, .width = 200, .height = 100, .u = QUAD_SOLID, .v = 0,
					       .color = QUAD_RGB(0xeeeeee) };
	target.frame.quads[1] = (struct quad){ .x = 10, .y = 10, .width = 20, .height = 20, .u = QUAD_SOLID, .v = 0,
					       .color = QUAD_RGB(0x3373d6) };
	target.frame.count = 2;
	target_draw(&target, &loop, QUAD_RGB(0xffffff), &target.frame);
	moved = target.frame;
	moved.quads[1].x = 120;
	moved.quads[1].y = 60;
	target_redraw(&target, &loop, QUAD_RGB(0xffffff), &moved, rects);
	copy_array(redrawn, *(const uint32_t (*)[100][200])target.readback.map);
	target_draw(&target, &loop, QUAD_RGB(0xffffff), &moved);
	assert(near(redrawn[15][15], 0xeeeeee) && near(redrawn[65][125], 0x3373d6));
	assert(!memcmp(redrawn, target.readback.map, sizeof(redrawn)));
	target_fini(&target, &loop);
}

static void changed(struct editor *)
{
}

static void title(struct editor *, const char *)
{
}

static bool copy(struct editor *, bool, size_t size, const char (*)[size])
{
	return true;
}

static bool paste(struct editor *, bool)
{
	return false;
}

static void cursor(struct editor *, uint32_t)
{
}

static void quit(struct editor *)
{
}

static const struct editor_listener editor_listener = {
	.changed = changed,
	.title = title,
	.copy = copy,
	.paste = paste,
	.cursor = cursor,
	.quit = quit,
};

static void write_file(const char *directory, const char *name, const char *contents)
{
	char path[PATH_MAX];
	int fd;

	snprintf_array(path, "%s/%s", directory, name);
	assert((fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644)) >= 0);
	assert(write_array(fd, array_ptr(contents, strlen(contents))) == (ssize_t)strlen(contents));
	close(fd);
}

/* No pixel of a glyph covers all of it, so none has quite the color */
static bool type_color(uint32_t pixel)
{
	return within(pixel, THEME_TYPE, 0x10);
}

struct sample {
	char root[sizeof("/tmp/render-test-XXXXXX")];
	char path[PATH_MAX], directory[PATH_MAX];
};

static void sample_open(struct sample *sample, struct editor *editor, struct editor_memory *memory, struct target *target,
			struct poor_loop *loop)
{
	static const char source[] = "int x;\n/* comment */\nstatic char *name = \"text\";\n";
	const struct editor_tab *tab;

	snprintf_array(sample->root, "/tmp/render-test-XXXXXX");
	assert(mkdtemp(sample->root));
	write_file(sample->root, "sample.c", source);
	snprintf_array(sample->directory, "%s/include", sample->root);
	assert(!mkdir(sample->directory, 0755));
	write_file(sample->directory, "sample.h", "#pragma once\n");
	snprintf_array(sample->path, "%s/sample.c", sample->root);
	editor_init(editor, loop, &editor_listener, memory);
	editor_start(editor, sample->root, nullptr);
	editor_resize(editor, 640, 320, target->font.cell_width, target->font.cell_height, 4, 120);
	editor_focus(editor, true);
	editor_open(editor, sample->path);
	run_until(loop, (tab = editor_tab(editor)) && !tab->loading && editor->tree.nodes[0].loaded);
	editor_layout(editor);
}

static void sample_close(struct sample *sample, struct editor *editor, struct poor_loop *loop)
{
	editor_stop(editor);
	run_until(loop, !editor_busy(editor));
	unlink(sample->path);
	snprintf_array(sample->path, "%s/include/sample.h", sample->root);
	unlink(sample->path);
	rmdir(sample->directory);
	rmdir(sample->root);
}

/*
 * The editor with the tree of a directory and a C file in a tab: the panels and the caret have the colors of the theme,
 * the caret is at the start of the line it stands out on, and types have their color.
 */
TEST(render_editor)
{
	static struct target target;
	static struct editor editor;
	static struct editor_memory memory;
	struct sample sample;
	struct poor_loop loop;
	int32_t x, y;

	if (!target_init(&target, &loop, 640, 320))
		return;
	sample_open(&sample, &editor, &memory, &target, &loop);
	scene_build(&editor, &target.atlas, &target.frame);
	target_draw(&target, &loop, SCENE_BACKGROUND, &target.frame);

	auto pixels = (const uint32_t (*)[320][640])target.readback.map;
	snapshot(320, 640, pixels);
	x = editor.text_rect.x + editor.padding;
	y = editor.text_rect.y + editor.padding;
	assert(near((*pixels)[300 - 2 * target.font.cell_height][2], THEME_PANEL) && near((*pixels)[318][638], THEME_PANEL));
	assert(near((*pixels)[y + target.font.cell_height / 2][x], THEME_CARET));
	assert(near((*pixels)[y + target.font.cell_height / 2][630], THEME_CURRENT_LINE));
	assert(cell_has(320, 640, pixels, x + target.font.cell_width, y, &target.font, type_color));
	sample_close(&sample, &editor, &loop);
	target_fini(&target, &loop);
}

poor_damage_buffers_declare(one_buffer, 1, DAMAGE_RECTS);

static bool has_rect(const struct damage_region *region, int32_t x, int32_t y, int32_t width, int32_t height)
{
	foreach_array_const_ref(arrview_first(region->count, region->rects), rect)
		if (rect->x == x && rect->y == y && rect->width == width && rect->height == height)
			return true;
	return false;
}

TEST(render_redraws_changes)
{
	static struct target target;
	static struct editor editor;
	static struct editor_memory memory;
	static struct frame shown;
	static uint32_t redrawn[320][640];
	const struct editor_rect *gutter = &editor.gutter_rect, *status = &editor.status_rect;
	struct damage_region changes, redraws;
	struct one_buffer damage;
	const char *none = "";
	struct sample sample;
	struct poor_loop loop;
	int32_t y, digit;

	if (!target_init(&target, &loop, 640, 320))
		return;
	sample_open(&sample, &editor, &memory, &target, &loop);
	poor_damage_buffers_init(&damage, 640, 320);
	scene_build(&editor, &target.atlas, &shown);
	damage_diff(nullptr, &shown, &changes);
	target_draw(&target, &loop, SCENE_BACKGROUND, &shown);
	poor_damage_buffers_drawn(&damage, 0, &changes);
	editor_key(&editor, XKB_KEY_Down, 0, strlen(none), array_ptr(none, strlen(none)));
	editor_layout(&editor);
	scene_build(&editor, &target.atlas, &target.frame);
	damage_diff(&shown, &target.frame, &changes);
	y = editor.text_rect.y + editor.padding;
	digit = status->x + status->width - editor.padding - (int32_t)strlen("Ln 2, Col 1") * editor.cell_width +
		3 * editor.cell_width;
	assert(changes.count == 2 && has_rect(&changes, gutter->x, y, gutter->width + editor.text_rect.width, 2 * editor.cell_height));
	assert(has_rect(&changes, digit, status->y + editor.padding, editor.cell_width, editor.cell_height));
	poor_damage_buffers_get(&damage, 0, &redraws);
	poor_damage_region_add_region(&redraws, &changes);
	target_redraw(&target, &loop, SCENE_BACKGROUND, &target.frame, arrview_first(redraws.count, redraws.rects));
	copy_array(redrawn, *(const uint32_t (*)[320][640])target.readback.map);
	target_draw(&target, &loop, SCENE_BACKGROUND, &target.frame);
	assert(!memcmp(redrawn, target.readback.map, sizeof(redrawn)));
	sample_close(&sample, &editor, &loop);
	target_fini(&target, &loop);
}
