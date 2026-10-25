/* JSRF for PS5 - Vulkan phase 0: what a draw costs on Mihawk's RADV port (decision log D32).
 *
 * The OpenGL SDK costs ~76 us a draw (D29), which keeps JSRF's title menu at ~17 fps. This title draws a frame
 * shaped like that menu, on the 3840x2160 VideoOut swapchain RADV offers: N indexed draws of ~170 triangles, a
 * pipeline bound every 5 draws from 20, 8 vec4 of constants per draw (push constants, then a dynamic uniform
 * buffer offset), and fresh vertex data per draw, as JSRF uploads it. For each N (100, 550, 1000, 2000) and each
 * way of passing constants it runs three seconds and logs the CPU time per draw (recording and submitting) and the
 * frames a second. Built on radv/radv_smoke.c's set-up (Mihawk-99, GPL-3.0-or-later); no Vulkan loader: every
 * command comes from vk_icdGetInstanceProcAddr. */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "drawcost_shaders.h"

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance, const char *name);
int sceKernelDebugOutText(int channel, const char *text);

#define LOG_PATH "/app0/vk-drawcost.txt"
static FILE *g_log;

static void report(const char *format, ...)
{
	char line[512];
	va_list args;
	va_start(args, format);
	vsnprintf(line, sizeof line, format, args);
	va_end(args);
	if (!g_log)
		g_log = fopen(LOG_PATH, "w");
	if (g_log) {
		fprintf(g_log, "%s\n", line);
		fflush(g_log);
	}
	char k[540];
	snprintf(k, sizeof k, "[vk-drawcost] %s\n", line);
	sceKernelDebugOutText(0, k);
}

static uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

#define INSTANCE_COMMANDS(X)                                                                                       \
	X(DestroyInstance) X(EnumeratePhysicalDevices) X(GetPhysicalDeviceProperties2)                                \
	X(GetPhysicalDeviceQueueFamilyProperties) X(GetPhysicalDeviceMemoryProperties) X(CreateDevice)               \
	X(GetDeviceProcAddr) X(GetPhysicalDeviceDisplayPropertiesKHR) X(GetDisplayModePropertiesKHR)                 \
	X(CreateDisplayPlaneSurfaceKHR)
#define DEVICE_COMMANDS(X)                                                                                         \
	X(GetDeviceQueue) X(CreateCommandPool) X(AllocateCommandBuffers) X(BeginCommandBuffer) X(EndCommandBuffer)    \
	X(ResetCommandBuffer) X(CreateFence) X(WaitForFences) X(ResetFences) X(QueueSubmit) X(CreateBuffer)          \
	X(GetBufferMemoryRequirements) X(AllocateMemory) X(BindBufferMemory) X(MapMemory) X(CreateShaderModule)      \
	X(CreateDescriptorSetLayout) X(CreatePipelineLayout) X(CreateGraphicsPipelines) X(CreateDescriptorPool)      \
	X(AllocateDescriptorSets) X(UpdateDescriptorSets) X(CreateImageView) X(CreateSemaphore)                     \
	X(CreateSwapchainKHR) X(GetSwapchainImagesKHR) X(AcquireNextImageKHR) X(QueuePresentKHR)                    \
	X(CmdPipelineBarrier2) X(CmdBeginRendering) X(CmdEndRendering) X(CmdSetViewport) X(CmdSetScissor)          \
	X(CmdBindPipeline) X(CmdPushConstants) X(CmdBindDescriptorSets) X(CmdBindVertexBuffers)                     \
	X(CmdBindIndexBuffer) X(CmdDrawIndexed) X(DeviceWaitIdle)

#define DECLARE(name) static PFN_vk##name vk##name;
INSTANCE_COMMANDS(DECLARE)
DEVICE_COMMANDS(DECLARE)
static PFN_vkCreateInstance vkCreateInstance;

#define W 3840
#define H 2160
#define PIPELINES 20
#define FRAMES 2              /* in flight */
#define TRIS 170              /* per draw */
#define VSLOT 2048            /* bytes of vertex data a draw gets: (TRIS / 2 + 1) * 2 vertices of 8 bytes fit */
#define MAX_DRAWS 2000
#define CONSTS (8 * 16)       /* bytes of constants per draw */

struct frame {
	VkCommandBuffer cmd;
	VkFence done;
	VkSemaphore acquired, rendered;
	VkBuffer vbuf, ubuf;
	uint8_t *vmap, *umap;
	VkDescriptorSet set;
};

static VkInstance g_instance;
static VkPhysicalDevice g_physical;
static VkDevice g_device;
static VkQueue g_queue;
static uint32_t g_family;
static VkPhysicalDeviceMemoryProperties g_memory;
static VkSwapchainKHR g_swapchain;
static VkImage g_images[8];
static VkImageView g_views[8];
static uint32_t g_image_count;
static VkPipeline g_pipe[2][PIPELINES];
static VkPipelineLayout g_layout;
static struct frame g_frame[FRAMES];
static VkBuffer g_ibuf;
static uint32_t g_index_count;

static uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags wanted)
{
	for (uint32_t i = 0; i < g_memory.memoryTypeCount; i++)
		if ((bits & (1u << i)) && (g_memory.memoryTypes[i].propertyFlags & wanted) == wanted)
			return i;
	return UINT32_MAX;
}

static bool host_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer *buffer, uint8_t **map)
{
	VkBufferCreateInfo info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage};
	if (vkCreateBuffer(g_device, &info, NULL, buffer) != VK_SUCCESS)
		return false;
	VkMemoryRequirements req;
	vkGetBufferMemoryRequirements(g_device, *buffer, &req);
	uint32_t type = memory_type(req.memoryTypeBits,
	                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	VkMemoryAllocateInfo alloc = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
	                              .memoryTypeIndex = type};
	VkDeviceMemory memory;
	if (type == UINT32_MAX || vkAllocateMemory(g_device, &alloc, NULL, &memory) != VK_SUCCESS ||
	    vkBindBufferMemory(g_device, *buffer, memory, 0) != VK_SUCCESS)
		return false;
	return vkMapMemory(g_device, memory, 0, size, 0, (void **)map) == VK_SUCCESS;
}

static bool create_context(void)
{
	vkCreateInstance = (PFN_vkCreateInstance)vk_icdGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance");
	const char *const exts[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_DISPLAY_EXTENSION_NAME};
	VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "jsrf-vk-drawcost",
	                         .apiVersion = VK_API_VERSION_1_3};
	VkInstanceCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
	                           .enabledExtensionCount = 2, .ppEnabledExtensionNames = exts};
	if (!vkCreateInstance || vkCreateInstance(&ii, NULL, &g_instance) != VK_SUCCESS) {
		report("no instance");
		return false;
	}
#define LOAD_I(name) vk##name = (PFN_vk##name)vk_icdGetInstanceProcAddr(g_instance, "vk" #name);
	INSTANCE_COMMANDS(LOAD_I)
	uint32_t count = 1;
	vkEnumeratePhysicalDevices(g_instance, &count, &g_physical);
	VkPhysicalDeviceDriverProperties driver = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
	VkPhysicalDeviceProperties2 props = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &driver};
	vkGetPhysicalDeviceProperties2(g_physical, &props);
	report("device: %s, %s %s", props.properties.deviceName, driver.driverName, driver.driverInfo);
	VkQueueFamilyProperties families[8];
	count = 8;
	vkGetPhysicalDeviceQueueFamilyProperties(g_physical, &count, families);
	g_family = 0;
	for (uint32_t i = 0; i < count; i++)
		if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
			g_family = i;
			break;
		}
	vkGetPhysicalDeviceMemoryProperties(g_physical, &g_memory);
	const float priority = 1.0f;
	VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = g_family,
	                              .queueCount = 1, .pQueuePriorities = &priority};
	VkPhysicalDeviceVulkan13Features f13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
	                                        .synchronization2 = VK_TRUE, .dynamicRendering = VK_TRUE};
	const char *const dexts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
	VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f13, .queueCreateInfoCount = 1,
	                         .pQueueCreateInfos = &qi, .enabledExtensionCount = 1, .ppEnabledExtensionNames = dexts};
	if (vkCreateDevice(g_physical, &di, NULL, &g_device) != VK_SUCCESS) {
		report("no device");
		return false;
	}
#define LOAD_D(name) vk##name = (PFN_vk##name)vkGetDeviceProcAddr(g_device, "vk" #name);
	DEVICE_COMMANDS(LOAD_D)
	vkGetDeviceQueue(g_device, g_family, 0, &g_queue);
	return true;
}

static bool create_swapchain(void)
{
	VkDisplayPropertiesKHR display;
	uint32_t count = 1;
	if (vkGetPhysicalDeviceDisplayPropertiesKHR(g_physical, &count, &display) != VK_SUCCESS || count != 1)
		return false;
	VkDisplayModePropertiesKHR modes[2];
	count = 2;
	vkGetDisplayModePropertiesKHR(g_physical, display.display, &count, modes);
	report("display %ux%u, %.3f Hz", display.physicalResolution.width, display.physicalResolution.height,
	       modes[0].parameters.refreshRate / 1000.0);
	VkDisplaySurfaceCreateInfoKHR si = {.sType = VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR,
	                                    .displayMode = modes[0].displayMode,
	                                    .transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
	                                    .alphaMode = VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR, .imageExtent = {W, H}};
	VkSurfaceKHR surface;
	if (vkCreateDisplayPlaneSurfaceKHR(g_instance, &si, NULL, &surface) != VK_SUCCESS)
		return false;
	VkSwapchainCreateInfoKHR sc = {.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = surface,
	                               .minImageCount = 3, .imageFormat = VK_FORMAT_B8G8R8A8_UNORM,
	                               .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR, .imageExtent = {W, H},
	                               .imageArrayLayers = 1, .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
	                               .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
	                               .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
	                               .presentMode = VK_PRESENT_MODE_FIFO_KHR, .clipped = VK_TRUE};
	if (vkCreateSwapchainKHR(g_device, &sc, NULL, &g_swapchain) != VK_SUCCESS)
		return false;
	g_image_count = 8;
	vkGetSwapchainImagesKHR(g_device, g_swapchain, &g_image_count, g_images);
	for (uint32_t i = 0; i < g_image_count; i++) {
		VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = g_images[i],
		                            .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_B8G8R8A8_UNORM,
		                            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
		if (vkCreateImageView(g_device, &vi, NULL, &g_views[i]) != VK_SUCCESS)
			return false;
	}
	return true;
}

static VkShaderModule module(const uint32_t *code, size_t bytes)
{
	VkShaderModuleCreateInfo info = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = bytes,
	                                 .pCode = code};
	VkShaderModule m = VK_NULL_HANDLE;
	vkCreateShaderModule(g_device, &info, NULL, &m);
	return m;
}

static bool create_pipelines(void)
{
	VkDescriptorSetLayoutBinding b = {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
	                                  .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_VERTEX_BIT};
	VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
	                                      .bindingCount = 1, .pBindings = &b};
	VkDescriptorSetLayout set_layout;
	vkCreateDescriptorSetLayout(g_device, &li, NULL, &set_layout);
	VkPushConstantRange push = {VK_SHADER_STAGE_VERTEX_BIT, 0, CONSTS};
	VkPipelineLayoutCreateInfo pl = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
	                                 .pSetLayouts = &set_layout, .pushConstantRangeCount = 1,
	                                 .pPushConstantRanges = &push};
	if (vkCreatePipelineLayout(g_device, &pl, NULL, &g_layout) != VK_SUCCESS)
		return false;

	VkShaderModule vs = module(drawcost_vert, sizeof drawcost_vert), fs = module(drawcost_frag, sizeof drawcost_frag);
	VkVertexInputBindingDescription vb = {0, 8, VK_VERTEX_INPUT_RATE_VERTEX};
	VkVertexInputAttributeDescription va = {0, 0, VK_FORMAT_R32G32_SFLOAT, 0};
	VkPipelineVertexInputStateCreateInfo vin = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
	                                            .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &vb,
	                                            .vertexAttributeDescriptionCount = 1,
	                                            .pVertexAttributeDescriptions = &va};
	VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
	                                             .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
	VkPipelineViewportStateCreateInfo vp = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
	                                        .viewportCount = 1, .scissorCount = 1};
	VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
	                                             .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
	                                             .lineWidth = 1.0f};
	VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
	                                           .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
	VkPipelineColorBlendAttachmentState att = {.colorWriteMask = 0xF};
	VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
	                                          .attachmentCount = 1, .pAttachments = &att};
	VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
	VkPipelineDynamicStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
	                                       .dynamicStateCount = 2, .pDynamicStates = dyn};
	VkFormat format = VK_FORMAT_B8G8R8A8_UNORM;
	VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
	                                    .colorAttachmentCount = 1, .pColorAttachmentFormats = &format};
	uint64_t t0 = now_ns();
	for (int ubo = 0; ubo < 2; ubo++)
		for (int p = 0; p < PIPELINES; p++) {
			struct { int32_t from_ubo; float tint; } spec_data = {ubo, 0.5f + 0.5f * p / PIPELINES};
			VkSpecializationMapEntry entries[2] = {{0, 0, 4}, {1, 4, 4}};
			VkSpecializationInfo vspec = {1, &entries[0], sizeof spec_data, &spec_data};
			VkSpecializationInfo fspec = {1, &entries[1], sizeof spec_data, &spec_data};
			VkPipelineShaderStageCreateInfo stages[2] = {
				{.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
				 .module = vs, .pName = "main", .pSpecializationInfo = &vspec},
				{.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
				 .module = fs, .pName = "main", .pSpecializationInfo = &fspec},
			};
			VkGraphicsPipelineCreateInfo gi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
			                                   .pNext = &ri, .stageCount = 2, .pStages = stages,
			                                   .pVertexInputState = &vin, .pInputAssemblyState = &ia,
			                                   .pViewportState = &vp, .pRasterizationState = &rs,
			                                   .pMultisampleState = &ms, .pColorBlendState = &cb,
			                                   .pDynamicState = &ds, .layout = g_layout};
			if (vkCreateGraphicsPipelines(g_device, VK_NULL_HANDLE, 1, &gi, NULL, &g_pipe[ubo][p]) != VK_SUCCESS)
				return false;
		}
	report("%d pipelines in %.1f ms", 2 * PIPELINES, (now_ns() - t0) / 1e6);

	VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, FRAMES};
	VkDescriptorPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = FRAMES,
	                                 .poolSizeCount = 1, .pPoolSizes = &ps};
	VkDescriptorPool pool;
	vkCreateDescriptorPool(g_device, &pi, NULL, &pool);
	for (int f = 0; f < FRAMES; f++) {
		VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		                                  .descriptorPool = pool, .descriptorSetCount = 1, .pSetLayouts = &set_layout};
		vkAllocateDescriptorSets(g_device, &ai, &g_frame[f].set);
		VkDescriptorBufferInfo bi = {g_frame[f].ubuf, 0, CONSTS};
		VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = g_frame[f].set,
		                          .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
		                          .pBufferInfo = &bi};
		vkUpdateDescriptorSets(g_device, 1, &w, 0, NULL);
	}
	return true;
}

/* A strip of quads, (TRIS / 2) of them, as the vertex data a draw re-uploads, and its indices once. */
static float g_mesh[(TRIS / 2 + 1) * 4];
static uint16_t g_indices[TRIS * 3 + 6];

static bool create_buffers(void)
{
	int quads = TRIS / 2;
	for (int q = 0; q <= quads; q++) {
		g_mesh[q * 4 + 0] = (float)q / quads;
		g_mesh[q * 4 + 1] = 0.0f;
		g_mesh[q * 4 + 2] = (float)q / quads;
		g_mesh[q * 4 + 3] = 1.0f;
	}
	g_index_count = 0;
	for (int q = 0; q < quads; q++) {
		uint16_t a = (uint16_t)(q * 2), b = (uint16_t)(q * 2 + 1), c = (uint16_t)(q * 2 + 2), d = (uint16_t)(q * 2 + 3);
		g_indices[g_index_count++] = a; g_indices[g_index_count++] = b; g_indices[g_index_count++] = c;
		g_indices[g_index_count++] = b; g_indices[g_index_count++] = d; g_indices[g_index_count++] = c;
	}
	uint8_t *imap;
	if (!host_buffer(sizeof g_indices, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, &g_ibuf, &imap))
		return false;
	memcpy(imap, g_indices, sizeof g_indices);
	for (int f = 0; f < FRAMES; f++)
		if (!host_buffer((VkDeviceSize)MAX_DRAWS * VSLOT, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &g_frame[f].vbuf,
		                 &g_frame[f].vmap) ||
		    !host_buffer((VkDeviceSize)MAX_DRAWS * 256, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &g_frame[f].ubuf,
		                 &g_frame[f].umap))
			return false;
	return true;
}

static bool create_frames(void)
{
	VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
	                              .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
	                              .queueFamilyIndex = g_family};
	VkCommandPool pool;
	if (vkCreateCommandPool(g_device, &pi, NULL, &pool) != VK_SUCCESS)
		return false;
	for (int f = 0; f < FRAMES; f++) {
		VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
		                                  .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
		VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT};
		VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
		if (vkAllocateCommandBuffers(g_device, &ai, &g_frame[f].cmd) != VK_SUCCESS ||
		    vkCreateFence(g_device, &fi, NULL, &g_frame[f].done) != VK_SUCCESS ||
		    vkCreateSemaphore(g_device, &si, NULL, &g_frame[f].acquired) != VK_SUCCESS ||
		    vkCreateSemaphore(g_device, &si, NULL, &g_frame[f].rendered) != VK_SUCCESS)
			return false;
	}
	return true;
}

static void barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to)
{
	VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
	                           .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
	                           .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
	                           .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
	                           .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
	                           .oldLayout = from, .newLayout = to,
	                           .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	                           .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = image,
	                           .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
	VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1,
	                      .pImageMemoryBarriers = &b};
	vkCmdPipelineBarrier2(cmd, &d);
}

/* One frame of `draws` draws; returns the CPU nanoseconds spent recording and submitting. */
static uint64_t frame(unsigned number, unsigned draws, int ubo)
{
	struct frame *fr = &g_frame[number % FRAMES];
	vkWaitForFences(g_device, 1, &fr->done, VK_TRUE, UINT64_MAX);
	uint32_t index;
	if (vkAcquireNextImageKHR(g_device, g_swapchain, UINT64_MAX, fr->acquired, VK_NULL_HANDLE, &index) != VK_SUCCESS)
		return 0;
	uint64_t t0 = now_ns();
	vkResetFences(g_device, 1, &fr->done);
	vkResetCommandBuffer(fr->cmd, 0);
	VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
	                               .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
	vkBeginCommandBuffer(fr->cmd, &bi);
	barrier(fr->cmd, g_images[index], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = g_views[index],
	                                .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	                                .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
	                                .clearValue = {.color = {.float32 = {0.05f, 0.05f, 0.08f, 1.0f}}}};
	VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {W, H}},
	                      .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &ca};
	vkCmdBeginRendering(fr->cmd, &ri);
	VkViewport v = {0, 0, W, H, 0, 1};
	VkRect2D s = {{0, 0}, {W, H}};
	vkCmdSetViewport(fr->cmd, 0, 1, &v);
	vkCmdSetScissor(fr->cmd, 0, 1, &s);
	vkCmdBindIndexBuffer(fr->cmd, g_ibuf, 0, VK_INDEX_TYPE_UINT16);
	size_t vbytes = (size_t)((TRIS / 2 + 1) * 2) * 8;
	for (unsigned d = 0; d < draws; d++) {
		if (d % 5 == 0)
			vkCmdBindPipeline(fr->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pipe[ubo][(d / 5) % PIPELINES]);
		/* fresh vertex data, as JSRF uploads a draw's vertices */
		VkDeviceSize voff = (VkDeviceSize)d * VSLOT;
		memcpy(fr->vmap + voff, g_mesh, vbytes);
		vkCmdBindVertexBuffers(fr->cmd, 0, 1, &fr->vbuf, &voff);
		float k[32];
		float x = (float)(d % 40) / 40.0f * 2.0f - 1.0f, y = (float)(d / 40 % 40) / 40.0f * 2.0f - 1.0f;
		for (int i = 0; i < 32; i++) k[i] = 0.5f;
		k[0] = 0.04f; k[1] = 0.04f; k[2] = x; k[3] = y;
		k[4] = (float)(d % 7) / 7.0f; k[5] = (float)(d % 11) / 11.0f; k[6] = (float)(d % 13) / 13.0f;
		if (ubo) {
			uint32_t uoff = d * 256;
			memcpy(fr->umap + uoff, k, CONSTS);
			vkCmdBindDescriptorSets(fr->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_layout, 0, 1, &fr->set, 1, &uoff);
		} else {
			vkCmdPushConstants(fr->cmd, g_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, CONSTS, k);
		}
		vkCmdDrawIndexed(fr->cmd, g_index_count, 1, 0, 0, 0);
	}
	vkCmdEndRendering(fr->cmd);
	barrier(fr->cmd, g_images[index], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
	vkEndCommandBuffer(fr->cmd);
	VkPipelineStageFlags wait = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	VkSubmitInfo sub = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1,
	                    .pWaitSemaphores = &fr->acquired, .pWaitDstStageMask = &wait, .commandBufferCount = 1,
	                    .pCommandBuffers = &fr->cmd, .signalSemaphoreCount = 1, .pSignalSemaphores = &fr->rendered};
	vkQueueSubmit(g_queue, 1, &sub, fr->done);
	uint64_t cpu = now_ns() - t0;
	VkPresentInfoKHR pr = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1,
	                       .pWaitSemaphores = &fr->rendered, .swapchainCount = 1, .pSwapchains = &g_swapchain,
	                       .pImageIndices = &index};
	vkQueuePresentKHR(g_queue, &pr);
	return cpu;
}

int main(void)
{
	report("JSRF Vulkan phase 0: the cost of a draw on RADV");
	if (!create_context() || !create_swapchain() || !create_buffers() || !create_frames() || !create_pipelines()) {
		report("set-up failed");
		return 1;
	}
	static const unsigned counts[] = {100, 550, 1000, 2000};
	unsigned number = 0;
	for (int ubo = 0; ubo < 2; ubo++)
		for (unsigned c = 0; c < sizeof counts / sizeof counts[0]; c++) {
			unsigned draws = counts[c], frames = 0;
			uint64_t cpu = 0, start = now_ns(), warm = start + 500000000ull, measured_from = 0;
			while (now_ns() - start < 3500000000ull) {
				uint64_t t = frame(number++, draws, ubo);
				if (now_ns() >= warm) {
					if (!measured_from) measured_from = now_ns();
					cpu += t;
					frames++;
				}
			}
			double secs = (now_ns() - measured_from) / 1e9;
			report("%s %4u draws a frame: %.2f us CPU a draw, %.2f ms CPU a frame, %.1f fps", ubo ? "ubo " : "push",
			       draws, frames ? cpu / 1000.0 / frames / draws : 0.0, frames ? cpu / 1e6 / frames : 0.0,
			       frames / secs);
		}
	vkDeviceWaitIdle(g_device);
	report("done");
	for (;;) { /* stays until closed */
		struct timespec w = {1, 0};
		nanosleep(&w, NULL);
	}
}
