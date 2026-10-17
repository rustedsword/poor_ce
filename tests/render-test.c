/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include <assert.h>
#include <drm/drm_fourcc.h>
#include <poll.h>
#include <poor_poll.h>
#include <poor_stdio.h>
#include <poor_unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <vk_init.h>

#include "editor.h"
#include "keep-loaded.h"
#include "logger.h"
#include "render.h"
#include "scene.h"
#include "test-loop.h"
#include "test-runner.h"

static const uint64_t LINEAR[] = { DRM_FORMAT_MOD_LINEAR };

/* B8G8R8A8 pixels close to 'color', which is 0xRRGGBB */
static bool near(uint32_t pixel, uint32_t color)
{
	for (unsigned shift = 0; shift < 24; shift += 8) {
		int channel = pixel >> shift & 0xff, expected = color >> shift & 0xff;

		if (channel - expected > 1 || expected - channel > 1)
			return false;
	}
	return true;
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

/* The image comes back from the compositor's queue family, as if it had read it, so the test can copy it */
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
	struct quad quads[RENDER_QUADS];
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

static void target_draw(struct target *target, struct poor_loop *loop, uint32_t background, size_t count)
{
	struct poor_wl_swapchain_buffer *buffer = &target->swapchain.buffers[0];
	struct pollfd fence = { .events = POLLIN };

	fence.fd = renderer_draw(&target->renderer, buffer, background, arrview_first(count, target->quads));
	assert(fence.fd >= 0 && poll_array(array_ptr(&fence), 5000) == 1);
	poor_loop_close_fd_or_submit(loop, fence.fd, 0);
	read_image(&target->renderer, ((struct poor_wl_vulkan_image *)buffer->data)->image, buffer->width, buffer->height,
		   &target->readback);
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
	target.quads[0] = (struct quad){ .x = 20, .y = 10, .width = 30, .height = 40, .u = QUAD_SOLID, .v = 0,
					 .color = QUAD_RGB(0x3373d6) };
	target_draw(&target, &loop, QUAD_RGB(0xffffff), 1);

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
	target.quads[0] = (struct quad){ .x = 20, .y = 10, .width = 30, .height = 40, .u = QUAD_SOLID, .v = 0,
					 .color = QUAD_RGB(0x3373d6) };
	target.quads[1] = (struct quad){ .x = 100, .y = 10, .width = target.font.cell_width, .height = target.font.cell_height,
					 .u = u, .v = v, .color = QUAD_RGB(0x1e1e1e) };
	target_draw(&target, &loop, QUAD_RGB(0xffffff), 2);

	auto pixels = (const uint32_t (*)[100][200])target.readback.map;
	assert(white((*pixels)[99][199]) && white((*pixels)[9][20]) && white((*pixels)[10][50]));
	assert(near((*pixels)[10][20], 0x3373d6) && near((*pixels)[49][49], 0x3373d6));
	assert(cell_has(100, 200, pixels, 100, 10, &target.font, dark) && cell_has(100, 200, pixels, 100, 10, &target.font, white));
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

static bool teal(uint32_t pixel)
{
	return (pixel & 0xff) > (pixel >> 16 & 0xff) + 0x30;
}

/*
 * The editor with the tree of a directory and a C file in a tab: the panels and the caret have the colors of the theme,
 * the caret is at the start of the line it stands out on, and types are teal.
 */
TEST(render_editor)
{
	static struct target target;
	static struct editor editor;
	static struct editor_memory memory;
	static const char source[] = "int x;\n/* comment */\nstatic char *name = \"text\";\n";
	char root[] = "/tmp/render-test-XXXXXX", path[PATH_MAX], directory[PATH_MAX];
	const struct editor_tab *tab;
	struct poor_loop loop;
	int32_t x, y;

	if (!target_init(&target, &loop, 640, 320))
		return;
	assert(mkdtemp(root));
	write_file(root, "sample.c", source);
	snprintf_array(directory, "%s/include", root);
	assert(!mkdir(directory, 0755));
	write_file(directory, "sample.h", "#pragma once\n");
	snprintf_array(path, "%s/sample.c", root);
	editor_init(&editor, &loop, &editor_listener, &memory);
	editor_start(&editor, root, nullptr);
	editor_resize(&editor, 640, 320, target.font.cell_width, target.font.cell_height, 4, 120);
	editor_focus(&editor, true);
	editor_open(&editor, path);
	run_until(&loop, (tab = editor_tab(&editor)) && !tab->loading && editor.tree.nodes[0].loaded);
	editor_layout(&editor);
	target_draw(&target, &loop, SCENE_BACKGROUND, scene_build(&editor, &target.atlas, target.quads));

	auto pixels = (const uint32_t (*)[320][640])target.readback.map;
	snapshot(320, 640, pixels);
	x = editor.text_rect.x + editor.padding;
	y = editor.text_rect.y + editor.padding;
	assert(near((*pixels)[300 - 2 * target.font.cell_height][2], THEME_PANEL) && near((*pixels)[318][638], THEME_PANEL));
	assert(near((*pixels)[y + target.font.cell_height / 2][x], THEME_CARET));
	assert(near((*pixels)[y + target.font.cell_height / 2][630], THEME_CURRENT_LINE));
	assert(cell_has(320, 640, pixels, x + target.font.cell_width, y, &target.font, teal));
	editor_stop(&editor);
	run_until(&loop, !editor_busy(&editor));
	unlink(path);
	snprintf_array(path, "%s/include/sample.h", root);
	unlink(path);
	rmdir(directory);
	rmdir(root);
	target_fini(&target, &loop);
}
