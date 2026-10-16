/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include <assert.h>
#include <errno.h>
#include <poor_loop_fd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysmacros.h>

#include "logger.h"
#include "render.h"
#include "text.frag.h"
#include "text.vert.h"

/* A glyph: the cell at 'x', 'y' of the image shows the atlas cell at 'u', 'v' in 'color', which is RGBA */
struct instance {
	int16_t x, y;
	uint16_t u, v;
	uint32_t color;
};

static_assert(sizeof(struct instance) == 12);

struct push_constants {
	float viewport[2];
	float cell[2];
	float atlas[2];
};

static const VkClearColorValue BACKGROUND = { .float32 = { 1.0f, 1.0f, 1.0f, 1.0f } };
static const VkClearColorValue SELECTION = { .float32 = { 0.2f, 0.45f, 0.84f, 1.0f } };
static const VkClearColorValue SELECTION_INACTIVE = { .float32 = { 0.82f, 0.82f, 0.82f, 1.0f } };
constexpr uint32_t TEXT_COLOR = 0xff1e1e1e;
constexpr uint32_t SELECTED_TEXT_COLOR = 0xffffffff;

static bool check(VkResult result, const char *what)
{
	if (result >= 0)
		return true;
	log_printf("poor_text_viewer: %s: %d\n", what, result);
	return false;
}

static struct render_target *target_of(struct renderer *renderer, struct poor_wl_swapchain_buffer *buffer)
{
	return &renderer->targets[array_ref_index(buffer->swapchain->buffers, buffer)];
}

static uint32_t memory_type(const struct renderer *renderer, uint32_t types, VkMemoryPropertyFlags flags)
{
	VkPhysicalDeviceMemoryProperties properties;

	vkGetPhysicalDeviceMemoryProperties(renderer->physical, &properties);
	foreach_array_index(arrview_first(properties.memoryTypeCount, properties.memoryTypes), i)
		if (types & 1u << i && (properties.memoryTypes[i].propertyFlags & flags) == flags)
			return i;
	return UINT32_MAX;
}

static bool buffer_init(struct renderer *renderer, VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer *buffer,
			VkDeviceMemory *memory, void **map)
{
	VkBufferCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = size,
		.usage = usage,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	};
	VkMemoryAllocateInfo allocate = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	VkMemoryRequirements requirements;

	if (!check(vkCreateBuffer(renderer->device, &info, nullptr, buffer), "vkCreateBuffer"))
		return false;
	vkGetBufferMemoryRequirements(renderer->device, *buffer, &requirements);
	allocate.allocationSize = requirements.size;
	allocate.memoryTypeIndex = memory_type(renderer, requirements.memoryTypeBits,
					       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	if (allocate.memoryTypeIndex == UINT32_MAX) {
		log_printf("poor_text_viewer: no host visible memory\n");
		return false;
	}
	return check(vkAllocateMemory(renderer->device, &allocate, nullptr, memory), "vkAllocateMemory") &&
	       check(vkBindBufferMemory(renderer->device, *buffer, *memory, 0), "vkBindBufferMemory") &&
	       check(vkMapMemory(renderer->device, *memory, 0, VK_WHOLE_SIZE, 0, map), "vkMapMemory");
}

/* The last drawing is done by then, which Vulkan learns from the fence */
static void target_fini(struct renderer *renderer, struct render_target *target)
{
	if (target->done)
		vkGetFenceStatus(renderer->device, target->done);
	if (target->commands)
		vkFreeCommandBuffers(renderer->device, renderer->pool, 1, &target->commands);
	vkDestroyFence(renderer->device, target->done, nullptr);
	vkDestroyImageView(renderer->device, target->view, nullptr);
	vkDestroyBuffer(renderer->device, target->instances, nullptr);
	vkFreeMemory(renderer->device, target->instance_memory, nullptr);
	vkDestroyBuffer(renderer->device, target->staging, nullptr);
	vkFreeMemory(renderer->device, target->staging_memory, nullptr);
}

static bool target_init(struct renderer *renderer, struct render_target *target, VkImage image)
{
	VkImageViewCreateInfo view_info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = image,
		.viewType = VK_IMAGE_VIEW_TYPE_2D,
		.format = RENDER_FORMAT,
		.subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1 },
	};
	VkCommandBufferAllocateInfo commands_info = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = renderer->pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
	};
	VkFenceCreateInfo fence_info = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT };

	target->view = VK_NULL_HANDLE;
	target->commands = VK_NULL_HANDLE;
	target->done = VK_NULL_HANDLE;
	target->instances = target->staging = VK_NULL_HANDLE;
	target->instance_memory = target->staging_memory = VK_NULL_HANDLE;
	return check(vkCreateImageView(renderer->device, &view_info, nullptr, &target->view), "vkCreateImageView") &&
	       check(vkAllocateCommandBuffers(renderer->device, &commands_info, &target->commands), "vkAllocateCommandBuffers") &&
	       check(vkCreateFence(renderer->device, &fence_info, nullptr, &target->done), "vkCreateFence") &&
	       buffer_init(renderer, RENDER_INSTANCES * sizeof(struct instance), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
			   &target->instances, &target->instance_memory, &target->instance_map) &&
	       buffer_init(renderer, ATLAS_SIZE * ATLAS_SIZE, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &target->staging,
			   &target->staging_memory, &target->staging_map);
}

/* poor_wl_vulkan allocates, the targets come and go with the buffers */
static int renderer_allocate(void *data, struct poor_wl_swapchain_buffer *buffer, uint32_t format, unsigned count,
			     const uint64_t (*modifiers)[count], bool scanout)
{
	struct renderer *renderer = data;
	struct render_target *target = target_of(renderer, buffer);
	int ret = poor_wl_vulkan_allocator.allocate(&renderer->vulkan, buffer, format, count, modifiers, scanout);

	if (ret < 0)
		return ret;
	assert(((struct poor_wl_vulkan_image *)buffer->data)->format == RENDER_FORMAT);
	if (!target_init(renderer, target, ((struct poor_wl_vulkan_image *)buffer->data)->image)) {
		target_fini(renderer, target);
		foreach_array_const_ref(arrview_first(buffer->plane_count, buffer->planes), plane)
			poor_loop_close_fd_or_submit(renderer->vulkan.loop, plane->fd, 0);
		poor_wl_vulkan_allocator.free(&renderer->vulkan, buffer);
		return -ENOMEM;
	}
	return 0;
}

static void renderer_free(void *data, struct poor_wl_swapchain_buffer *buffer)
{
	struct renderer *renderer = data;

	target_fini(renderer, target_of(renderer, buffer));
	poor_wl_vulkan_allocator.free(&renderer->vulkan, buffer);
}

const struct poor_wl_swapchain_allocator renderer_allocator = {
	.allocate = renderer_allocate,
	.free = renderer_free,
};

static bool device_queue_family(VkPhysicalDevice physical, uint32_t *family)
{
	VkQueueFamilyProperties families[16];
	uint32_t count = ARRAY_SIZE(families);

	vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families);
	foreach_array_index(arrview_first(count, families), i) {
		if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
			*family = i;
			return true;
		}
	}
	return false;
}

static bool device_exports_fences(VkPhysicalDevice physical)
{
	VkPhysicalDeviceExternalSemaphoreInfo info = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO,
		.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
	};
	VkExternalSemaphoreProperties properties = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES };

	vkGetPhysicalDeviceExternalSemaphoreProperties(physical, &info, &properties);
	return properties.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT;
}

static bool device_init(struct renderer *renderer, struct poor_loop *loop, dev_t device, uint32_t *render_minor)
{
	static const float priority = 1.0f;
	VkApplicationInfo application = {
		.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
		.pApplicationName = "poor_text_viewer",
		.apiVersion = VK_API_VERSION_1_3,
	};
	VkInstanceCreateInfo instance_info = {
		.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pApplicationInfo = &application,
	};
	VkPhysicalDeviceVulkan13Features features = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
		.dynamicRendering = VK_TRUE,
	};
	VkDeviceQueueCreateInfo queue_info = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
		.queueCount = 1,
		.pQueuePriorities = &priority,
	};
	VkDeviceCreateInfo device_info = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		.pNext = &features,
		.queueCreateInfoCount = 1,
		.pQueueCreateInfos = &queue_info,
		.enabledExtensionCount = ARRAY_SIZE(poor_wl_vulkan_device_extensions),
		.ppEnabledExtensionNames = poor_wl_vulkan_device_extensions,
	};
	VkCommandPoolCreateInfo pool_info = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
	};
	VkExportSemaphoreCreateInfo export = {
		.sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
		.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
	};
	VkSemaphoreCreateInfo semaphore_info = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &export };
	VkPhysicalDeviceProperties properties;

	/* Mesa's device-select layer would connect to the compositor with the system libwayland and block, to sort GPUs */
	setenv("NODEVICE_SELECT", "1", 1);
	if (!check(vkCreateInstance(&instance_info, nullptr, &renderer->instance), "vkCreateInstance"))
		return false;
	renderer->physical = poor_wl_vulkan_find_device(vkGetInstanceProcAddr, renderer->instance, device, render_minor);
	if (renderer->physical) {
		vkGetPhysicalDeviceProperties(renderer->physical, &properties);
		copy_array(renderer->device_name, properties.deviceName);
	}
	if (!renderer->physical || properties.apiVersion < VK_API_VERSION_1_3 ||
	    !device_queue_family(renderer->physical, &renderer->family) || !device_exports_fences(renderer->physical)) {
		log_printf("poor_text_viewer: no Vulkan 1.3 device with dmabufs, modifiers and fences for %u:%u\n", major(device),
			   minor(device));
		return false;
	}
	queue_info.queueFamilyIndex = renderer->family;
	pool_info.queueFamilyIndex = renderer->family;
	if (!check(vkCreateDevice(renderer->physical, &device_info, nullptr, &renderer->device), "vkCreateDevice"))
		return false;
	vkGetDeviceQueue(renderer->device, renderer->family, 0, &renderer->queue);
	renderer->get_semaphore_fd = (PFN_vkGetSemaphoreFdKHR)vkGetDeviceProcAddr(renderer->device, "vkGetSemaphoreFdKHR");
	poor_wl_vulkan_init(&renderer->vulkan, loop, vkGetInstanceProcAddr, renderer->instance, renderer->physical,
			    renderer->device, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
	return check(vkCreateCommandPool(renderer->device, &pool_info, nullptr, &renderer->pool), "vkCreateCommandPool") &&
	       check(vkCreateSemaphore(renderer->device, &semaphore_info, nullptr, &renderer->rendered), "vkCreateSemaphore");
}

/* The atlas stays in the general layout, the frames that add glyphs upload them before they draw */
static bool atlas_image_init(struct renderer *renderer)
{
	VkImageCreateInfo image_info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = VK_FORMAT_R8_UNORM,
		.extent = { ATLAS_SIZE, ATLAS_SIZE, 1 },
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};
	VkImageViewCreateInfo view_info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.viewType = VK_IMAGE_VIEW_TYPE_2D,
		.format = VK_FORMAT_R8_UNORM,
		.subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1 },
	};
	VkMemoryAllocateInfo allocate = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	VkMemoryRequirements requirements;

	if (!check(vkCreateImage(renderer->device, &image_info, nullptr, &renderer->atlas_image), "vkCreateImage"))
		return false;
	vkGetImageMemoryRequirements(renderer->device, renderer->atlas_image, &requirements);
	allocate.allocationSize = requirements.size;
	allocate.memoryTypeIndex = memory_type(renderer, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (allocate.memoryTypeIndex == UINT32_MAX)
		allocate.memoryTypeIndex = memory_type(renderer, requirements.memoryTypeBits, 0);
	view_info.image = renderer->atlas_image;
	return check(vkAllocateMemory(renderer->device, &allocate, nullptr, &renderer->atlas_memory), "vkAllocateMemory") &&
	       check(vkBindImageMemory(renderer->device, renderer->atlas_image, renderer->atlas_memory, 0), "vkBindImageMemory") &&
	       check(vkCreateImageView(renderer->device, &view_info, nullptr, &renderer->atlas_view), "vkCreateImageView");
}

static bool shader_init(struct renderer *renderer, size_t size, const uint32_t (*code)[size], VkShaderModule *module)
{
	VkShaderModuleCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = ARRAY_SIZE_BYTES(code),
		.pCode = *code,
	};

	return check(vkCreateShaderModule(renderer->device, &info, nullptr, module), "vkCreateShaderModule");
}

/* Each glyph is a strip of two triangles that covers its cell */
static bool pipeline_create(struct renderer *renderer, VkShaderModule vertex, VkShaderModule fragment)
{
	static const VkFormat format = RENDER_FORMAT;
	static const VkDynamicState dynamic_states[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
	VkPipelineShaderStageCreateInfo stages[] = {
		{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_VERTEX_BIT,
			.module = vertex,
			.pName = "main",
		},
		{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_FRAGMENT_BIT,
			.module = fragment,
			.pName = "main",
		},
	};
	VkVertexInputBindingDescription binding = {
		.binding = 0,
		.stride = sizeof(struct instance),
		.inputRate = VK_VERTEX_INPUT_RATE_INSTANCE,
	};
	VkVertexInputAttributeDescription attributes[] = {
		{ .location = 0, .format = VK_FORMAT_R16G16_SINT, .offset = offsetof(struct instance, x) },
		{ .location = 1, .format = VK_FORMAT_R16G16_UINT, .offset = offsetof(struct instance, u) },
		{ .location = 2, .format = VK_FORMAT_R8G8B8A8_UNORM, .offset = offsetof(struct instance, color) },
	};
	VkPipelineVertexInputStateCreateInfo vertex_input = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
		.vertexBindingDescriptionCount = 1,
		.pVertexBindingDescriptions = &binding,
		.vertexAttributeDescriptionCount = ARRAY_SIZE(attributes),
		.pVertexAttributeDescriptions = attributes,
	};
	VkPipelineInputAssemblyStateCreateInfo assembly = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
		.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
	};
	VkPipelineViewportStateCreateInfo viewport = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
		.viewportCount = 1,
		.scissorCount = 1,
	};
	VkPipelineRasterizationStateCreateInfo rasterization = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
		.polygonMode = VK_POLYGON_MODE_FILL,
		.cullMode = VK_CULL_MODE_NONE,
		.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
		.lineWidth = 1.0f,
	};
	VkPipelineMultisampleStateCreateInfo multisample = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
		.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
	};
	VkPipelineColorBlendAttachmentState blend_attachment = {
		.blendEnable = VK_TRUE,
		.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
		.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
		.colorBlendOp = VK_BLEND_OP_ADD,
		.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
		.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
		.alphaBlendOp = VK_BLEND_OP_ADD,
		.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
				  VK_COLOR_COMPONENT_A_BIT,
	};
	VkPipelineColorBlendStateCreateInfo blend = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
		.attachmentCount = 1,
		.pAttachments = &blend_attachment,
	};
	VkPipelineDynamicStateCreateInfo dynamic = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
		.dynamicStateCount = ARRAY_SIZE(dynamic_states),
		.pDynamicStates = dynamic_states,
	};
	VkPipelineRenderingCreateInfo rendering = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
		.colorAttachmentCount = 1,
		.pColorAttachmentFormats = &format,
	};
	VkGraphicsPipelineCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		.pNext = &rendering,
		.stageCount = ARRAY_SIZE(stages),
		.pStages = stages,
		.pVertexInputState = &vertex_input,
		.pInputAssemblyState = &assembly,
		.pViewportState = &viewport,
		.pRasterizationState = &rasterization,
		.pMultisampleState = &multisample,
		.pColorBlendState = &blend,
		.pDynamicState = &dynamic,
		.layout = renderer->layout,
	};

	return check(vkCreateGraphicsPipelines(renderer->device, VK_NULL_HANDLE, 1, &info, nullptr, &renderer->pipeline),
		     "vkCreateGraphicsPipelines");
}

/* The atlas is sampled at the centers of its texels, so that glyphs show as they were drawn */
static bool pipeline_init(struct renderer *renderer)
{
	VkSamplerCreateInfo sampler_info = {
		.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		.magFilter = VK_FILTER_NEAREST,
		.minFilter = VK_FILTER_NEAREST,
		.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
		.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
	};
	VkDescriptorSetLayoutBinding binding = {
		.binding = 0,
		.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		.descriptorCount = 1,
		.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
		.pImmutableSamplers = &renderer->sampler,
	};
	VkDescriptorSetLayoutCreateInfo set_layout_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = 1,
		.pBindings = &binding,
	};
	VkDescriptorPoolSize pool_size = { .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1 };
	VkDescriptorPoolCreateInfo pool_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = 1,
		.poolSizeCount = 1,
		.pPoolSizes = &pool_size,
	};
	VkDescriptorSetAllocateInfo set_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorSetCount = 1,
		.pSetLayouts = &renderer->set_layout,
	};
	VkDescriptorImageInfo image_info = { .imageView = renderer->atlas_view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL };
	VkWriteDescriptorSet write = {
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.dstBinding = 0,
		.descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		.pImageInfo = &image_info,
	};
	VkPushConstantRange push = {
		.stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
		.size = sizeof(struct push_constants),
	};
	VkPipelineLayoutCreateInfo layout_info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 1,
		.pSetLayouts = &renderer->set_layout,
		.pushConstantRangeCount = 1,
		.pPushConstantRanges = &push,
	};
	VkShaderModule vertex = VK_NULL_HANDLE, fragment = VK_NULL_HANDLE;
	bool ok;

	if (!check(vkCreateSampler(renderer->device, &sampler_info, nullptr, &renderer->sampler), "vkCreateSampler") ||
	    !check(vkCreateDescriptorSetLayout(renderer->device, &set_layout_info, nullptr, &renderer->set_layout),
		   "vkCreateDescriptorSetLayout") ||
	    !check(vkCreateDescriptorPool(renderer->device, &pool_info, nullptr, &renderer->descriptor_pool),
		   "vkCreateDescriptorPool"))
		return false;
	set_info.descriptorPool = renderer->descriptor_pool;
	if (!check(vkAllocateDescriptorSets(renderer->device, &set_info, &renderer->set), "vkAllocateDescriptorSets") ||
	    !check(vkCreatePipelineLayout(renderer->device, &layout_info, nullptr, &renderer->layout), "vkCreatePipelineLayout"))
		return false;
	write.dstSet = renderer->set;
	vkUpdateDescriptorSets(renderer->device, 1, &write, 0, nullptr);
	ok = shader_init(renderer, ARRAY_SIZE(text_vert), &text_vert, &vertex) &&
	     shader_init(renderer, ARRAY_SIZE(text_frag), &text_frag, &fragment) && pipeline_create(renderer, vertex, fragment);
	vkDestroyShaderModule(renderer->device, vertex, nullptr);
	vkDestroyShaderModule(renderer->device, fragment, nullptr);
	return ok;
}

bool renderer_init(struct renderer *renderer, struct poor_loop *loop, struct atlas *atlas, dev_t device, uint32_t *render_minor)
{
	renderer->atlas = atlas;
	renderer->instance = nullptr;
	renderer->physical = nullptr;
	renderer->device = nullptr;
	renderer->queue = nullptr;
	renderer->pool = VK_NULL_HANDLE;
	renderer->rendered = renderer->idle = VK_NULL_HANDLE;
	renderer->drained = VK_NULL_HANDLE;
	renderer->sampler = VK_NULL_HANDLE;
	renderer->set_layout = VK_NULL_HANDLE;
	renderer->descriptor_pool = VK_NULL_HANDLE;
	renderer->layout = VK_NULL_HANDLE;
	renderer->pipeline = VK_NULL_HANDLE;
	renderer->atlas_image = VK_NULL_HANDLE;
	renderer->atlas_memory = VK_NULL_HANDLE;
	renderer->atlas_view = VK_NULL_HANDLE;
	renderer->atlas_valid = false;
	return device_init(renderer, loop, device, render_minor) && atlas_image_init(renderer) && pipeline_init(renderer);
}

void renderer_fini(struct renderer *renderer)
{
	if (renderer->device) {
		vkDestroyFence(renderer->device, renderer->drained, nullptr);
		vkDestroySemaphore(renderer->device, renderer->idle, nullptr);
		vkDestroySemaphore(renderer->device, renderer->rendered, nullptr);
		vkDestroyPipeline(renderer->device, renderer->pipeline, nullptr);
		vkDestroyPipelineLayout(renderer->device, renderer->layout, nullptr);
		vkDestroyDescriptorPool(renderer->device, renderer->descriptor_pool, nullptr);
		vkDestroyDescriptorSetLayout(renderer->device, renderer->set_layout, nullptr);
		vkDestroySampler(renderer->device, renderer->sampler, nullptr);
		vkDestroyImageView(renderer->device, renderer->atlas_view, nullptr);
		vkDestroyImage(renderer->device, renderer->atlas_image, nullptr);
		vkFreeMemory(renderer->device, renderer->atlas_memory, nullptr);
		vkDestroyCommandPool(renderer->device, renderer->pool, nullptr);
		vkDestroyDevice(renderer->device, nullptr);
	}
	if (renderer->instance)
		vkDestroyInstance(renderer->instance, nullptr);
}

/* Lines that show from 'top' of 'view', none after the end of the text */
static size_t shown_lines(const struct view *view)
{
	size_t rows = view_rows(view), left = view->text->line_count - view->top;

	return left < rows ? left : rows;
}

/*
 * The glyphs that show, from the atlas. A full atlas starts over once, if the glyphs still don't fit the rest is left
 * out.
 */
static size_t build_glyphs(struct renderer *renderer, struct instance (*instances)[RENDER_INSTANCES], const struct view *view,
			   size_t start, size_t end)
{
	const struct text *text = view->text;
	size_t count = 0, columns = view_columns(view);
	bool cleared = false;
	struct text_char c;
	uint16_t u, v;

again:
	foreach_array_index(arrview(view->top, shown_lines(view), array_ptr(text->lines, text->line_count)), row) {
		text_chars_init(text, view->top + row, &c);
		while (text_chars_next(text, &c)) {
			size_t column = c.width || !c.column ? c.column : c.column - 1;

			if (column < view->left || c.codepoint == ' ')
				continue;
			if (c.column >= view->left + columns || count == RENDER_INSTANCES)
				break;
			if (!atlas_find(renderer->atlas, c.codepoint, &u, &v)) {
				if (cleared)
					return count;
				atlas_clear(renderer->atlas);
				cleared = true;
				count = 0;
				goto again;
			}
			(*instances)[count++] = (struct instance){
				.x = view->padding + (column - view->left) * view->cell_width,
				.y = view->padding + row * view->cell_height,
				.u = u,
				.v = v,
				.color = c.offset >= start && c.offset < end && view->focused ? SELECTED_TEXT_COLOR : TEXT_COLOR,
			};
		}
	}
	return count;
}

/* The selection of a line covers its line break too, if it goes on */
static size_t build_selection(struct renderer *renderer, const struct view *view, size_t start, size_t end)
{
	const struct text *text = view->text;
	size_t count = 0;

	foreach_array_index(arrview(view->top, shown_lines(view), array_ptr(text->lines, text->line_count)), row) {
		size_t line = view->top + row, first = text_line_start(text, line), last = text_line_end(text, line), from, to, x0, x1;
		int32_t y = view->padding + row * view->cell_height;

		if (end <= first || start > last || count == ARRAY_SIZE(renderer->rects))
			continue;
		from = start <= first ? 0 : text_column_of(text, line, start);
		to = end > last ? text_column_of(text, line, last) + 1 : text_column_of(text, line, end);
		if (to <= view->left)
			continue;
		x0 = view->padding + (from > view->left ? from - view->left : 0) * view->cell_width;
		x1 = view->padding + (to - view->left) * view->cell_width;
		if (x1 > (size_t)view->width)
			x1 = view->width;
		if (x0 >= x1)
			continue;
		renderer->rects[count++] = (VkClearRect){
			.rect = {
				.offset = { x0, y },
				.extent = { x1 - x0, y + view->cell_height > view->height ? view->height - y : view->cell_height },
			},
			.layerCount = 1,
		};
	}
	return count;
}

/* The rows that changed since the last upload go through the staging buffer of the target */
static void upload_atlas(struct renderer *renderer, struct render_target *target)
{
	struct atlas *atlas = renderer->atlas;
	auto staging = (uint8_t (*)[ATLAS_SIZE][ATLAS_SIZE])target->staging_map;
	size_t rows = atlas->dirty_bottom - atlas->dirty_top;
	VkImageMemoryBarrier barrier = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		.oldLayout = renderer->atlas_valid ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED,
		.newLayout = VK_IMAGE_LAYOUT_GENERAL,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = renderer->atlas_image,
		.subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1 },
	};
	VkBufferImageCopy region = {
		.imageSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1 },
		.imageOffset = { 0, atlas->dirty_top, 0 },
		.imageExtent = { ATLAS_SIZE, rows, 1 },
	};

	copy_array(arrview_first(rows, staging), arrview(atlas->dirty_top, rows, atlas->pixels));
	vkCmdPipelineBarrier(target->commands, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
			     0, nullptr, 1, &barrier);
	vkCmdCopyBufferToImage(target->commands, target->staging, renderer->atlas_image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
	barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
	vkCmdPipelineBarrier(target->commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr,
			     0, nullptr, 1, &barrier);
	renderer->atlas_valid = true;
	atlas_uploaded(atlas);
}

static void draw_glyphs(struct renderer *renderer, struct render_target *target, const struct view *view, size_t count)
{
	VkViewport viewport = { .width = view->width, .height = view->height, .maxDepth = 1.0f };
	VkRect2D scissor = { .extent = { view->width, view->height } };
	VkDeviceSize offset = 0;
	struct push_constants constants = {
		.viewport = { view->width, view->height },
		.cell = { view->cell_width, view->cell_height },
		.atlas = { ATLAS_SIZE, ATLAS_SIZE },
	};

	vkCmdBindPipeline(target->commands, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer->pipeline);
	vkCmdBindDescriptorSets(target->commands, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer->layout, 0, 1, &renderer->set, 0,
				nullptr);
	vkCmdSetViewport(target->commands, 0, 1, &viewport);
	vkCmdSetScissor(target->commands, 0, 1, &scissor);
	vkCmdPushConstants(target->commands, renderer->layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(constants), &constants);
	vkCmdBindVertexBuffers(target->commands, 0, 1, &target->instances, &offset);
	vkCmdDraw(target->commands, 4, count, 0, 0);
}

/*
 * The old contents of the image don't matter. It goes to the queue family of the compositor, which reads it once
 * the fence of the batch signals. The compositor was done with the buffer, and so was the last batch that drew it,
 * which Vulkan learns from the fence of that batch.
 */
int renderer_draw(struct renderer *renderer, struct poor_wl_swapchain_buffer *buffer, const struct view *view)
{
	struct render_target *target = target_of(renderer, buffer);
	VkImage image = ((struct poor_wl_vulkan_image *)buffer->data)->image;
	VkCommandBufferBeginInfo begin = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};
	VkImageMemoryBarrier draw = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = image,
		.subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1 },
	};
	VkImageMemoryBarrier release = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		.newLayout = VK_IMAGE_LAYOUT_GENERAL,
		.srcQueueFamilyIndex = renderer->family,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT,
		.image = image,
		.subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1 },
	};
	VkRenderingAttachmentInfo attachment = {
		.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
		.imageView = target->view,
		.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
		.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
		.clearValue = { .color = BACKGROUND },
	};
	VkRenderingInfo rendering = {
		.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
		.renderArea = { .extent = { (uint32_t)buffer->width, (uint32_t)buffer->height } },
		.layerCount = 1,
		.colorAttachmentCount = 1,
		.pColorAttachments = &attachment,
	};
	VkClearAttachment selection = {
		.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
		.clearValue = { .color = view->focused ? SELECTION : SELECTION_INACTIVE },
	};
	VkSubmitInfo submit = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.commandBufferCount = 1,
		.pCommandBuffers = &target->commands,
		.signalSemaphoreCount = 1,
		.pSignalSemaphores = &renderer->rendered,
	};
	VkSemaphoreGetFdInfoKHR get = {
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
		.semaphore = renderer->rendered,
		.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
	};
	size_t start, end, glyphs, rects;
	int fence;

	assert(buffer->width == view->width && buffer->height == view->height);
	if (vkGetFenceStatus(renderer->device, target->done) != VK_SUCCESS) {
		log_printf("poor_text_viewer: a buffer came back before its drawing was done\n");
		return -1;
	}
	if (!view_selection(view, &start, &end))
		start = end = 0;
	glyphs = build_glyphs(renderer, (struct instance (*)[RENDER_INSTANCES])target->instance_map, view, start, end);
	rects = start < end ? build_selection(renderer, view, start, end) : 0;
	if (!check(vkResetFences(renderer->device, 1, &target->done), "vkResetFences") ||
	    !check(vkBeginCommandBuffer(target->commands, &begin), "vkBeginCommandBuffer"))
		return -1;
	if (renderer->atlas->dirty_top < renderer->atlas->dirty_bottom)
		upload_atlas(renderer, target);
	vkCmdPipelineBarrier(target->commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			     0, 0, nullptr, 0, nullptr, 1, &draw);
	vkCmdBeginRendering(target->commands, &rendering);
	if (rects)
		vkCmdClearAttachments(target->commands, 1, &selection, rects, renderer->rects);
	if (glyphs)
		draw_glyphs(renderer, target, view, glyphs);
	vkCmdEndRendering(target->commands);
	vkCmdPipelineBarrier(target->commands, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			     VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &release);
	if (!check(vkEndCommandBuffer(target->commands), "vkEndCommandBuffer") ||
	    !check(vkQueueSubmit(renderer->queue, 1, &submit, target->done), "vkQueueSubmit") ||
	    !check(renderer->get_semaphore_fd(renderer->device, &get, &fence), "vkGetSemaphoreFdKHR"))
		return -1;
	return fence;
}

/* An empty batch signals once all drawing before it is done, -1 means that it is or that the fence failed */
int renderer_idle_fence(struct renderer *renderer)
{
	VkExportSemaphoreCreateInfo export = {
		.sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
		.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
	};
	VkSemaphoreCreateInfo info = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &export };
	VkFenceCreateInfo fence_info = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	VkSubmitInfo submit = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.signalSemaphoreCount = 1,
		.pSignalSemaphores = &renderer->idle,
	};
	VkSemaphoreGetFdInfoKHR get = {
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
		.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
	};
	int fd = -1;

	if (!renderer->queue || !check(vkCreateSemaphore(renderer->device, &info, nullptr, &renderer->idle), "vkCreateSemaphore") ||
	    !check(vkCreateFence(renderer->device, &fence_info, nullptr, &renderer->drained), "vkCreateFence") ||
	    !check(vkQueueSubmit(renderer->queue, 1, &submit, renderer->drained), "vkQueueSubmit"))
		return -1;
	get.semaphore = renderer->idle;
	check(renderer->get_semaphore_fd(renderer->device, &get, &fd), "vkGetSemaphoreFdKHR");
	return fd;
}
