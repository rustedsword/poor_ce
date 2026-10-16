/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include <assert.h>
#include <drm/drm_fourcc.h>
#include <poll.h>
#include <poor_poll.h>
#include <string.h>
#include <sys/sysmacros.h>

#include "keep-loaded.h"
#include "logger.h"
#include "render.h"
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

/* Whether some pixel of the cell at 'column' of the first row passes 'test' */
static bool cell_has(size_t height, size_t width, const uint32_t (*pixels)[height][width], const struct view *view, size_t column,
		     bool (*test)(uint32_t))
{
	auto rows = arrview(view->padding, view->cell_height, pixels);

	foreach_array_const_ref(rows, row)
		foreach_array_const_ref(arrview(view->padding + column * view->cell_width, view->cell_width, row), pixel)
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
	VkBufferUsageFlags2CreateInfo usage = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_USAGE_FLAGS_2_CREATE_INFO,
		.usage = VK_BUFFER_USAGE_2_TRANSFER_DST_BIT,
	};
	VkBufferCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.pNext = &usage,
		.size = size,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	};
	VkBufferMemoryRequirementsInfo2 requirements_info = { .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2 };
	VkMemoryRequirements2 requirements = { .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2 };
	VkPhysicalDeviceMemoryProperties2 properties = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2 };
	const VkPhysicalDeviceMemoryProperties *memory = &properties.memoryProperties;
	VkMemoryAllocateInfo allocate = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	VkMemoryPropertyFlags flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	VkBindBufferMemoryInfo bind = { .sType = VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_INFO };
	VkMemoryMapInfo map = { .sType = VK_STRUCTURE_TYPE_MEMORY_MAP_INFO, .size = VK_WHOLE_SIZE };

	assert(vkCreateBuffer(renderer->device, &info, nullptr, &readback->buffer) == VK_SUCCESS);
	requirements_info.buffer = bind.buffer = readback->buffer;
	vkGetBufferMemoryRequirements2(renderer->device, &requirements_info, &requirements);
	vkGetPhysicalDeviceMemoryProperties2(renderer->physical, &properties);
	allocate.allocationSize = requirements.memoryRequirements.size;
	allocate.memoryTypeIndex = UINT32_MAX;
	foreach_array_index(arrview_first(memory->memoryTypeCount, memory->memoryTypes), i)
		if (allocate.memoryTypeIndex == UINT32_MAX && requirements.memoryRequirements.memoryTypeBits & 1u << i &&
		    (memory->memoryTypes[i].propertyFlags & flags) == flags)
			allocate.memoryTypeIndex = i;
	assert(vkAllocateMemory(renderer->device, &allocate, nullptr, &readback->memory) == VK_SUCCESS);
	bind.memory = map.memory = readback->memory;
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
	VkCommandBufferAllocateInfo commands_info = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = renderer->pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
	};
	VkCommandBufferBeginInfo begin = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};
	VkImageMemoryBarrier2 acquire = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
		.srcStageMask = VK_PIPELINE_STAGE_2_NONE,
		.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
		.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
		.oldLayout = VK_IMAGE_LAYOUT_GENERAL,
		.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT,
		.dstQueueFamilyIndex = renderer->family,
		.image = image,
		.subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1 },
	};
	VkDependencyInfo acquire_dependency = {
		.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
		.imageMemoryBarrierCount = 1,
		.pImageMemoryBarriers = &acquire,
	};
	VkBufferImageCopy2 region = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
		.imageSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1 },
		.imageExtent = { width, height, 1 },
	};
	VkCopyImageToBufferInfo2 copy = {
		.sType = VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2,
		.srcImage = image,
		.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		.dstBuffer = readback->buffer,
		.regionCount = 1,
		.pRegions = &region,
	};
	VkBufferMemoryBarrier2 host = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
		.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
		.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
		.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
		.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.buffer = readback->buffer,
		.size = VK_WHOLE_SIZE,
	};
	VkDependencyInfo host_dependency = {
		.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
		.bufferMemoryBarrierCount = 1,
		.pBufferMemoryBarriers = &host,
	};
	VkFenceCreateInfo fence_info = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	VkCommandBufferSubmitInfo command_info = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
	VkSubmitInfo2 submit = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
		.commandBufferInfoCount = 1,
		.pCommandBufferInfos = &command_info,
	};
	VkCommandBuffer commands;
	VkFence done;

	assert(vkAllocateCommandBuffers(renderer->device, &commands_info, &commands) == VK_SUCCESS &&
	       vkCreateFence(renderer->device, &fence_info, nullptr, &done) == VK_SUCCESS &&
	       vkBeginCommandBuffer(commands, &begin) == VK_SUCCESS);
	vkCmdPipelineBarrier2(commands, &acquire_dependency);
	vkCmdCopyImageToBuffer2(commands, &copy);
	vkCmdPipelineBarrier2(commands, &host_dependency);
	command_info.commandBuffer = commands;
	assert(vkEndCommandBuffer(commands) == VK_SUCCESS && vkQueueSubmit2(renderer->queue, 1, &submit, done) == VK_SUCCESS &&
	       vkWaitForFences(renderer->device, 1, &done, VK_TRUE, UINT64_MAX) == VK_SUCCESS);
	vkDestroyFence(renderer->device, done, nullptr);
	vkFreeCommandBuffers(renderer->device, renderer->pool, 1, &commands);
}

/*
 * The image of a buffer that a swapchain would present. "b c" is selected, so its cells are blue and its letters
 * white, while "a" is dark on white. Without a GPU, as in CI, there is nothing to draw with. The validation layer
 * keeps its log open from the creation of the instance, also when there is no device.
 */
TEST(render_selection)
{
	static struct renderer renderer;
	static struct atlas atlas;
	static struct poor_wl_swapchain swapchain;
	static const char contents[] = "ab cd\nsecond";
	struct poor_wl_swapchain_buffer *buffer = &swapchain.buffers[0];
	struct pollfd fence = { .events = POLLIN };
	struct readback readback;
	uint32_t lines[4], minor;
	struct poor_loop loop;
	struct font font;
	struct text text;
	struct view view;
	bool ready;

	loop_init(&loop);
	log_init(&loop, "render-test");
	assert(font_init(&font, 15));
	atlas_init(&atlas, &font);
	DISABLE_LEAK_CHECKS;
	ready = renderer_init(&renderer, &loop, &atlas, makedev(226, 128), &minor);
	keep_loaded();
	if (!ready) {
		renderer_fini(&renderer);
		font_fini(&font);
		run_until(&loop, !log_pending());
		loop_fini(&loop);
		return;
	}
	renderer.vulkan.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	buffer->swapchain = &swapchain;
	buffer->width = 200;
	buffer->height = 100;
	assert(!renderer_allocator.allocate(&renderer, buffer, RENDER_DRM_FORMAT, ARRAY_SIZE(LINEAR), &LINEAR, false));
	assert(buffer->modifier == DRM_FORMAT_MOD_LINEAR && buffer->plane_count == 1);
	readback_init(&readback, &renderer, buffer->width * buffer->height * 4);
	text_init(&text, arrview_str(contents), lines);
	view_init(&view, &text);
	view_resize(&view, buffer->width, buffer->height, font.cell_width, font.cell_height, 4);
	view.focused = true;
	view_select(&view, 4, 1);
	fence.fd = renderer_draw(&renderer, buffer, &view);
	assert(fence.fd >= 0 && poll_array(array_ptr(&fence), 5000) == 1);
	poor_loop_close_fd_or_submit(&loop, fence.fd, 0);

	read_image(&renderer, ((struct poor_wl_vulkan_image *)buffer->data)->image, buffer->width, buffer->height, &readback);

	auto pixels = (const uint32_t (*)[buffer->height][buffer->width])readback.map;
	assert(white((*pixels)[buffer->height - 1][buffer->width - 1]));
	assert(near((*pixels)[view.padding + view.cell_height / 2][view.padding + view.cell_width * 2 + view.cell_width / 2],
		    0x3373d6));
	assert(cell_has(buffer->height, buffer->width, pixels, &view, 0, dark));
	assert(!cell_has(buffer->height, buffer->width, pixels, &view, 1, dark) &&
	       cell_has(buffer->height, buffer->width, pixels, &view, 1, white));
	assert(!cell_has(buffer->height, buffer->width, pixels, &view, 2, white));
	assert(cell_has(buffer->height, buffer->width, pixels, &view, 4, dark));
	readback_fini(&readback, &renderer);
	poor_loop_close_fd_or_submit(&loop, buffer->planes[0].fd, 0);
	renderer_allocator.free(&renderer, buffer);
	renderer_fini(&renderer);
	font_fini(&font);
	run_until(&loop, !log_pending());
	loop_fini(&loop);
}
