/* SPDX-License-Identifier: MIT */
#ifndef RENDER_H
#define RENDER_H

#include <drm/drm_fourcc.h>
#include <poor_wayland_swapchain_vulkan.h>
#include <sys/types.h>
#include <vulkan/vulkan.h>

#include "atlas.h"
#include "view.h"

/* The pipeline draws into images of the Vulkan format of the DRM format of the swapchain */
constexpr uint32_t RENDER_DRM_FORMAT = DRM_FORMAT_XRGB8888;
constexpr VkFormat RENDER_FORMAT = VK_FORMAT_B8G8R8A8_UNORM;
constexpr size_t RENDER_INSTANCES = 1 << 18;
constexpr size_t RENDER_ROWS = 2048;

/* How the image of a buffer of the swapchain is drawn: its glyphs, and the atlas rows that its frame uploads */
struct render_target {
	VkImageView view;
	VkCommandBuffer commands;
	VkFence done;
	VkBuffer instances, staging;
	VkDeviceMemory instance_memory, staging_memory;
	void *instance_map, *staging_map;
};

struct renderer {
	struct poor_wl_vulkan vulkan;
	struct atlas *atlas;
	VkInstance instance;
	VkPhysicalDevice physical;
	VkDevice device;
	VkQueue queue;
	uint32_t family;
	VkCommandPool pool;
	VkSemaphore rendered, idle;
	VkFence drained;
	VkSampler sampler;
	VkDescriptorSetLayout set_layout;
	VkDescriptorPool descriptor_pool;
	VkDescriptorSet set;
	VkPipelineLayout layout;
	VkPipeline pipeline;
	VkImage atlas_image;
	VkDeviceMemory atlas_memory;
	VkImageView atlas_view;
	bool atlas_valid;
	PFN_vkGetSemaphoreFdKHR get_semaphore_fd;
	struct render_target targets[POOR_WL_SWAPCHAIN_BUFFERS];
	VkClearRect rects[RENDER_ROWS];
	char device_name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
};

/* A Vulkan 1.3 device on 'device', which the dmabuf feedback names, and the minor of its render node */
[[nodiscard]] bool renderer_init(struct renderer *renderer, struct poor_loop *loop, struct atlas *atlas, dev_t device,
				 uint32_t *render_minor);
/* The drawing has to be done, see renderer_idle_fence() */
void renderer_fini(struct renderer *renderer);
/* Draw 'view' into 'buffer'. Returns the fence of the drawing, or -1. */
[[nodiscard]] int renderer_draw(struct renderer *renderer, struct poor_wl_swapchain_buffer *buffer, const struct view *view);
/* A fence that signals once all drawing is done, or -1 */
[[nodiscard]] int renderer_idle_fence(struct renderer *renderer);

/* 'data' is the struct renderer */
extern const struct poor_wl_swapchain_allocator renderer_allocator;

#endif
