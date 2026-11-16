/* Vulkan renderer for the shared SDL player (macOS via MoltenVK, and
 * elsewhere). GLSL is compiled to SPIR-V at run time with shaderc. */

#include "tools/bink_vulkan_renderer.h"

#ifdef BINK_USE_SDL3
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#else
#include <SDL.h>
#include <SDL_vulkan.h>
#endif

#include <shaderc/shaderc.h>
#include <vulkan/vulkan.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bink.h"

#define BINK_VK_MAX_IMAGES 8

struct BinkRenderer {
    VkInstance instance;
    VkSurfaceKHR surface;
    VkPhysicalDevice physical;
    VkDevice device;
    uint32_t queue_family;
    VkQueue queue;
    VkSwapchainKHR swapchain;
    VkFormat format;
    VkExtent2D extent;
    uint32_t image_count;
    VkImage images[BINK_VK_MAX_IMAGES];
    VkImageView views[BINK_VK_MAX_IMAGES];
    VkFramebuffer framebuffers[BINK_VK_MAX_IMAGES];
    VkCommandBuffer cmds[BINK_VK_MAX_IMAGES];
    VkDescriptorSet sets[BINK_VK_MAX_IMAGES];
    VkRenderPass render_pass;
    VkCommandPool pool;
    VkSemaphore sem_available;
    VkSemaphore sem_rendered;
    VkFence fence;
    VkDescriptorSetLayout set_layout;
    VkDescriptorPool desc_pool;
    VkPipelineLayout pipeline_layout;
    VkPipeline pipe_present;
    VkPipeline pipe_interp;
    VkShaderModule vs;
    VkShaderModule fs_present;
    VkShaderModule fs_interp;
    VkImage planes[2][3];
    VkDeviceMemory plane_mem[2][3];
    VkImageView plane_view[2][3];
    VkImage motion;
    VkDeviceMemory motion_mem;
    VkImageView motion_view;
    VkSampler sampler_linear;
    VkSampler sampler_nearest;
    VkImage capture;
    VkDeviceMemory capture_mem;
    VkImageView capture_view;
    VkFramebuffer capture_fb;
    VkBuffer readback;
    VkDeviceMemory readback_mem;
    unsigned long w;
    unsigned long h;
};

static const char *vk_vert =
    "#version 450\n"
    "layout(location = 0) out vec2 v_uv;\n"
    "void main() {\n"
    "    vec2 pos[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0),\n"
    "                         vec2(-1.0, 3.0));\n"
    "    vec2 p = pos[gl_VertexIndex];\n"
    "    v_uv = vec2((p.x + 1.0) * 0.5, (p.y + 1.0) * 0.5);\n"
    "    gl_Position = vec4(p, 0.0, 1.0);\n"
    "}\n";

static const char *vk_common =
    "layout(push_constant) uniform Params {\n"
    "    vec4 row0; vec4 row1; vec4 row2;\n"
    "    vec4 offset; vec4 luma; vec4 blocks;\n"
    "} pc;\n"
    "vec3 to_rgb(vec3 yuv) {\n"
    "    return vec3(dot(pc.row0.xyz, yuv), dot(pc.row1.xyz, yuv),\n"
    "                dot(pc.row2.xyz, yuv)) + pc.offset.xyz;\n"
    "}\n";

static const char *vk_frag_present =
    "layout(binding = 0) uniform sampler2D y_tex;\n"
    "layout(binding = 1) uniform sampler2D u_tex;\n"
    "layout(binding = 2) uniform sampler2D v_tex;\n"
    "layout(location = 0) in vec2 v_uv;\n"
    "layout(location = 0) out vec4 o_color;\n"
    "void main() {\n"
    "    vec3 yuv = vec3(texture(y_tex, v_uv).r, texture(u_tex, v_uv).r,\n"
    "                    texture(v_tex, v_uv).r);\n"
    "    o_color = vec4(clamp(to_rgb(yuv), 0.0, 1.0), 1.0);\n"
    "}\n";

static const char *vk_frag_interp =
    "layout(binding = 0) uniform sampler2D y0;\n"
    "layout(binding = 1) uniform sampler2D u0;\n"
    "layout(binding = 2) uniform sampler2D v0;\n"
    "layout(binding = 3) uniform sampler2D y1;\n"
    "layout(binding = 4) uniform sampler2D u1;\n"
    "layout(binding = 5) uniform sampler2D v1;\n"
    "layout(binding = 6) uniform sampler2D mv;\n"
    "layout(location = 0) in vec2 v_uv;\n"
    "layout(location = 0) out vec4 o_color;\n"
    "void main() {\n"
    "    float t = pc.luma.z;\n"
    "    ivec2 b = ivec2(floor(v_uv * pc.luma.xy / 8.0));\n"
    "    b = clamp(b, ivec2(0), ivec2(pc.blocks.xy) - 1);\n"
    "    vec2 px = texelFetch(mv, b, 0).rg * 255.0 - 128.0;\n"
    "    vec2 d = px / pc.luma.xy;\n"
    "    vec2 uv0 = v_uv + t * d;\n"
    "    vec2 uv1 = v_uv - (1.0 - t) * d;\n"
    "    vec3 a = vec3(texture(y0, uv0).r, texture(u0, uv0).r,\n"
    "                  texture(v0, uv0).r);\n"
    "    vec3 c = vec3(texture(y1, uv1).r, texture(u1, uv1).r,\n"
    "                  texture(v1, uv1).r);\n"
    "    o_color = vec4(clamp(to_rgb(mix(a, c, t)), 0.0, 1.0), 1.0);\n"
    "}\n";

typedef struct {
    float row0[4];
    float row1[4];
    float row2[4];
    float offset[4];
    float luma[4];
    float blocks[4];
} PushParams;

static const char *stage_name(shaderc_shader_kind kind)
{
    return kind == shaderc_glsl_vertex_shader ? "vertex" : "fragment";
}

static VkShaderModule make_module(VkDevice device, const char *source,
                                  shaderc_shader_kind kind)
{
    shaderc_compiler_t compiler = shaderc_compiler_initialize();
    shaderc_compilation_result_t result;
    VkShaderModule module = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo info;

    if (compiler == NULL) {
        fprintf(stderr, "Cannot start the shader compiler.\n");
        return VK_NULL_HANDLE;
    }
    result = shaderc_compile_into_spv(compiler, source, strlen(source),
                                      kind, "bink", "main", NULL);
    if (result == NULL ||
        shaderc_result_get_compilation_status(result) !=
            shaderc_compilation_status_success) {
        fprintf(stderr, "%s shader failed to compile: %s\n", stage_name(kind),
                result != NULL ? shaderc_result_get_error_message(result)
                               : "unknown");
        if (result != NULL)
            shaderc_result_release(result);
        shaderc_compiler_release(compiler);
        return VK_NULL_HANDLE;
    }
    memset(&info, 0, sizeof(info));
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = shaderc_result_get_length(result);
    info.pCode = (const uint32_t *)(const void *)shaderc_result_get_bytes(
        result);
    if (vkCreateShaderModule(device, &info, NULL, &module) != VK_SUCCESS) {
        fprintf(stderr, "Cannot create the %s module.\n", stage_name(kind));
        module = VK_NULL_HANDLE;
    }
    shaderc_result_release(result);
    shaderc_compiler_release(compiler);
    return module;
}

static int create_image(BinkRenderer *r, unsigned long w, unsigned long h,
                        VkFormat format, VkImageUsageFlags usage,
                        VkImage *image, VkDeviceMemory *memory)
{
    VkImageCreateInfo info;
    VkMemoryRequirements requirements;
    VkMemoryAllocateInfo allocate;
    VkPhysicalDeviceMemoryProperties props;
    uint32_t type = 0;
    VkResult result;

    memset(&info, 0, sizeof(info));
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent.width = (uint32_t)w;
    info.extent.height = (uint32_t)h;
    info.extent.depth = 1;
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    result = vkCreateImage(r->device, &info, NULL, image);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkCreateImage failed: %d\n", (int)result);
        return -1;
    }
    vkGetImageMemoryRequirements(r->device, *image, &requirements);
    vkGetPhysicalDeviceMemoryProperties(r->physical, &props);
    {
        uint32_t i;

        for (i = 0; i < props.memoryTypeCount; i++) {
            if ((requirements.memoryTypeBits & (1u << i)) &&
                (props.memoryTypes[i].propertyFlags &
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
                type = i;
                break;
            }
        }
    }
    memset(&allocate, 0, sizeof(allocate));
    allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = type;
    result = vkAllocateMemory(r->device, &allocate, NULL, memory);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkAllocateMemory failed: %d\n", (int)result);
        return -1;
    }
    return vkBindImageMemory(r->device, *image, *memory, 0) == VK_SUCCESS ? 0
                                                                         : -1;
}

static VkImageView make_view(BinkRenderer *r, VkImage image, VkFormat format)
{
    VkImageViewCreateInfo info;
    VkImageView view = VK_NULL_HANDLE;

    memset(&info, 0, sizeof(info));
    info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    info.image = image;
    info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    info.format = format;
    info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    info.subresourceRange.levelCount = 1;
    info.subresourceRange.layerCount = 1;
    if (vkCreateImageView(r->device, &info, NULL, &view) != VK_SUCCESS) {
        fprintf(stderr, "Cannot create an image view.\n");
        return VK_NULL_HANDLE;
    }
    return view;
}

/* One-time command: upload pixels into a device-local image. */
static int upload_image(BinkRenderer *r, VkImage image, const unsigned char *data,
                        unsigned long w, unsigned long h, unsigned bytes_per_pixel)
{
    VkBufferCreateInfo buffer_info;
    VkMemoryRequirements requirements;
    VkMemoryAllocateInfo allocate;
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkCommandBuffer cmd;
    VkCommandBufferAllocateInfo cmd_info;
    VkCommandBufferBeginInfo begin;
    VkBufferImageCopy copy;
    VkSubmitInfo submit;
    VkImageMemoryBarrier barrier;
    void *mapped = NULL;
    size_t bytes = (size_t)w * (size_t)h * bytes_per_pixel;

    memset(&buffer_info, 0, sizeof(buffer_info));
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = bytes;
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (vkCreateBuffer(r->device, &buffer_info, NULL, &staging) != VK_SUCCESS)
        return -1;
    vkGetBufferMemoryRequirements(r->device, staging, &requirements);
    {
        VkPhysicalDeviceMemoryProperties props;
        uint32_t i;
        uint32_t type = 0xffffffffu;

        vkGetPhysicalDeviceMemoryProperties(r->physical, &props);
        for (i = 0; i < props.memoryTypeCount; i++) {
            if ((requirements.memoryTypeBits & (1u << i)) &&
                (props.memoryTypes[i].propertyFlags &
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
                type = i;
                break;
            }
        }
        if (type == 0xffffffffu) {
            vkDestroyBuffer(r->device, staging, NULL);
            return -1;
        }
        memset(&allocate, 0, sizeof(allocate));
        allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = type;
    }
    if (vkAllocateMemory(r->device, &allocate, NULL, &memory) != VK_SUCCESS) {
        vkDestroyBuffer(r->device, staging, NULL);
        return -1;
    }
    vkBindBufferMemory(r->device, staging, memory, 0);
    if (vkMapMemory(r->device, memory, 0, bytes, 0, &mapped) != VK_SUCCESS ||
        mapped == NULL) {
        vkFreeMemory(r->device, memory, NULL);
        vkDestroyBuffer(r->device, staging, NULL);
        return -1;
    }
    memcpy(mapped, data, bytes);
    vkUnmapMemory(r->device, memory);

    memset(&cmd_info, 0, sizeof(cmd_info));
    cmd_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmd_info.commandPool = r->pool;
    cmd_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmd_info.commandBufferCount = 1;
    vkAllocateCommandBuffers(r->device, &cmd_info, &cmd);
    memset(&begin, 0, sizeof(begin));
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);
    memset(&barrier, 0, sizeof(barrier));
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1,
                         &barrier);
    memset(&copy, 0, sizeof(copy));
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1;
    copy.imageExtent.width = (uint32_t)w;
    copy.imageExtent.height = (uint32_t)h;
    copy.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(cmd, staging, image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0,
                         NULL, 1, &barrier);
    vkEndCommandBuffer(cmd);
    memset(&submit, 0, sizeof(submit));
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    vkQueueSubmit(r->queue, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(r->queue);
    vkFreeCommandBuffers(r->device, r->pool, 1, &cmd);
    vkDestroyBuffer(r->device, staging, NULL);
    vkFreeMemory(r->device, memory, NULL);
    return 0;
}

static int create_swapchain(BinkRenderer *r)
{
    VkSwapchainCreateInfoKHR info;
    VkSurfaceCapabilitiesKHR caps;
    VkResult result;

    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(r->physical, r->surface,
                                                  &caps) != VK_SUCCESS)
        return -1;
    r->format = VK_FORMAT_B8G8R8A8_UNORM;
    r->extent = caps.currentExtent;
    if (r->extent.width == 0xffffffffu) {
        r->extent.width = (uint32_t)r->w;
        r->extent.height = (uint32_t)r->h;
    }
    {
        uint32_t count = 0;
        uint32_t i;
        int have = 0;

        vkGetPhysicalDeviceSurfaceFormatsKHR(r->physical, r->surface, &count,
                                             NULL);
        if (count > 0) {
            VkSurfaceFormatKHR *formats =
                (VkSurfaceFormatKHR *)malloc(count * sizeof(*formats));

            if (formats != NULL) {
                vkGetPhysicalDeviceSurfaceFormatsKHR(r->physical, r->surface,
                                                     &count, formats);
                for (i = 0; i < count; i++) {
                    if (formats[i].format == VK_FORMAT_B8G8R8A8_UNORM) {
                        have = 1;
                        break;
                    }
                }
                if (!have)
                    r->format = formats[0].format;
                free(formats);
            }
        }
    }
    memset(&info, 0, sizeof(info));
    info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    info.surface = r->surface;
    info.minImageCount = caps.minImageCount < 3 ? 3 : caps.minImageCount;
    if (caps.maxImageCount > 0 && info.minImageCount > caps.maxImageCount)
        info.minImageCount = caps.maxImageCount;
    info.imageFormat = r->format;
    info.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    info.imageExtent = r->extent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = caps.currentTransform;
    info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    info.clipped = VK_TRUE;
    result = vkCreateSwapchainKHR(r->device, &info, NULL, &r->swapchain);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkCreateSwapchainKHR failed: %d\n", (int)result);
        return -1;
    }
    r->image_count = 0;
    vkGetSwapchainImagesKHR(r->device, r->swapchain, &r->image_count, NULL);
    if (r->image_count == 0 || r->image_count > BINK_VK_MAX_IMAGES)
        return -1;
    vkGetSwapchainImagesKHR(r->device, r->swapchain, &r->image_count,
                            r->images);
    return 0;
}

static int create_render_pass(BinkRenderer *r)
{
    VkAttachmentDescription color;
    VkAttachmentReference ref;
    VkSubpassDescription subpass;
    VkSubpassDependency dependency;
    VkRenderPassCreateInfo info;

    memset(&color, 0, sizeof(color));
    color.format = r->format;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    memset(&ref, 0, sizeof(ref));
    ref.attachment = 0;
    ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    memset(&subpass, 0, sizeof(subpass));
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &ref;
    memset(&dependency, 0, sizeof(dependency));
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    memset(&info, 0, sizeof(info));
    info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info.attachmentCount = 1;
    info.pAttachments = &color;
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    info.dependencyCount = 1;
    info.pDependencies = &dependency;
    return vkCreateRenderPass(r->device, &info, NULL, &r->render_pass) ==
                   VK_SUCCESS
               ? 0
               : -1;
}

int bink_vulkan_prepare(void)
{
    return 0;
}

BinkRenderer *bink_vulkan_renderer_create(void *window, unsigned long w,
                                          unsigned long h)
{
    BinkRenderer *r = (BinkRenderer *)calloc(1, sizeof(*r));
    VkApplicationInfo app;
    VkInstanceCreateInfo instance_info;
    uint32_t extension_count = 0;
    const char *extensions[8];
    VkResult result;
    int slot;
    int plane;

    if (r == NULL)
        return NULL;
    r->w = w;
    r->h = h;

    if (SDL_Vulkan_LoadLibrary(NULL) != 0) {
        /* SDL looks for a bare library name, which a Homebrew install does
         * not resolve without a path. */
        static const char *const fallbacks[] = {
            "/opt/homebrew/lib/libvulkan.dylib",
            "/usr/local/lib/libvulkan.dylib",
            NULL
        };
        const char *env = getenv("BINK_VULKAN_LIBRARY");
        int loaded = 0;
        int i;

        if (env != NULL && env[0] != '\0')
            loaded = SDL_Vulkan_LoadLibrary(env) == 0;
        for (i = 0; !loaded && fallbacks[i] != NULL; i++)
            loaded = SDL_Vulkan_LoadLibrary(fallbacks[i]) == 0;
        if (!loaded) {
            fprintf(stderr,
                    "Cannot load Vulkan; set BINK_VULKAN_LIBRARY to the "
                    "loader.\n");
            free(r);
            return NULL;
        }
    }
    {
        const char *names[8];
        uint32_t i;
#ifdef BINK_USE_SDL3
        Uint32 count = 0;
        const char *const *list = SDL_Vulkan_GetInstanceExtensions(&count);

        if (list == NULL) {
            fprintf(stderr, "Cannot query Vulkan extensions: %s\n",
                    SDL_GetError());
            free(r);
            return NULL;
        }
        for (i = 0; i < count && i < 8; i++)
            names[i] = list[i];
        extension_count = i;
#else
        unsigned count = 0;

        if (!SDL_Vulkan_GetInstanceExtensions((SDL_Window *)window, &count,
                                              NULL)) {
            fprintf(stderr, "Cannot query Vulkan extensions: %s\n",
                    SDL_GetError());
            free(r);
            return NULL;
        }
        if (count > 8)
            count = 8;
        if (!SDL_Vulkan_GetInstanceExtensions((SDL_Window *)window, &count,
                                              names)) {
            free(r);
            return NULL;
        }
        extension_count = (uint32_t)count;
#endif
        for (i = 0; i < extension_count; i++)
            extensions[i] = names[i];
    }

    {
        uint32_t count = extension_count > 8 ? 8 : extension_count;
        VkInstanceCreateFlags flags = 0;

#ifdef VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME
        if (count < 8) {
            extensions[count++] = VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME;
            flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
        }
#endif
        memset(&app, 0, sizeof(app));
        app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app.pApplicationName = "bink_sdlvulkan_player";
        app.apiVersion = VK_API_VERSION_1_0;
        memset(&instance_info, 0, sizeof(instance_info));
        instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        instance_info.flags = flags;
        instance_info.pApplicationInfo = &app;
        instance_info.enabledExtensionCount = count;
        instance_info.ppEnabledExtensionNames = extensions;
        result = vkCreateInstance(&instance_info, NULL, &r->instance);
        if (result != VK_SUCCESS) {
            fprintf(stderr, "vkCreateInstance failed: %d\n", (int)result);
            bink_vulkan_renderer_destroy(r);
            return NULL;
        }
    }
#ifdef BINK_USE_SDL3
    if (!SDL_Vulkan_CreateSurface((SDL_Window *)window, r->instance, NULL,
                                  &r->surface)) {
#else
    if (!SDL_Vulkan_CreateSurface((SDL_Window *)window, r->instance,
                                  &r->surface)) {
#endif
        fprintf(stderr, "Cannot create the Vulkan surface: %s\n",
                SDL_GetError());
        bink_vulkan_renderer_destroy(r);
        return NULL;
    }

    {
        uint32_t count = 0;
        vkEnumeratePhysicalDevices(r->instance, &count, NULL);
        if (count == 0) {
            fprintf(stderr, "No Vulkan device.\n");
            bink_vulkan_renderer_destroy(r);
            return NULL;
        }
        if (count > 1)
            count = 1;
        if (vkEnumeratePhysicalDevices(r->instance, &count, &r->physical) !=
            VK_SUCCESS) {
            bink_vulkan_renderer_destroy(r);
            return NULL;
        }
    }
    {
        uint32_t count = 0;
        VkQueueFamilyProperties *families;
        uint32_t i;

        vkGetPhysicalDeviceQueueFamilyProperties(r->physical, &count, NULL);
        families = (VkQueueFamilyProperties *)malloc(count *
                                                     sizeof(*families));
        if (families == NULL) {
            bink_vulkan_renderer_destroy(r);
            return NULL;
        }
        vkGetPhysicalDeviceQueueFamilyProperties(r->physical, &count, families);
        r->queue_family = 0xffffffffu;
        for (i = 0; i < count; i++) {
            VkBool32 present = VK_FALSE;

            vkGetPhysicalDeviceSurfaceSupportKHR(r->physical, i, r->surface,
                                                 &present);
            if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
                r->queue_family = i;
                break;
            }
        }
        free(families);
        if (r->queue_family == 0xffffffffu) {
            fprintf(stderr, "No graphics queue with present support.\n");
            bink_vulkan_renderer_destroy(r);
            return NULL;
        }
    }
    {
        float priority = 1.0f;
        VkDeviceQueueCreateInfo queue_info;
        VkDeviceCreateInfo device_info;
        const char *device_extensions[2];
        uint32_t device_extension_count = 1;
        uint32_t available = 0;
        int portability = 0;

        vkEnumerateDeviceExtensionProperties(r->physical, NULL, &available,
                                             NULL);
        {
            VkExtensionProperties *props =
                (VkExtensionProperties *)malloc(available * sizeof(*props));

            if (props != NULL) {
                uint32_t i;

                vkEnumerateDeviceExtensionProperties(r->physical, NULL,
                                                     &available, props);
                for (i = 0; i < available; i++) {
                    if (strcmp(props[i].extensionName,
                               "VK_KHR_portability_subset") == 0)
                        portability = 1;
                }
                free(props);
            }
        }
        device_extensions[0] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
        if (portability)
            device_extensions[device_extension_count++] =
                "VK_KHR_portability_subset";

        memset(&queue_info, 0, sizeof(queue_info));
        queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue_info.queueFamilyIndex = r->queue_family;
        queue_info.queueCount = 1;
        queue_info.pQueuePriorities = &priority;
        memset(&device_info, 0, sizeof(device_info));
        device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        device_info.queueCreateInfoCount = 1;
        device_info.pQueueCreateInfos = &queue_info;
        device_info.enabledExtensionCount = device_extension_count;
        device_info.ppEnabledExtensionNames = device_extensions;
        result = vkCreateDevice(r->physical, &device_info, NULL, &r->device);
        if (result != VK_SUCCESS) {
            fprintf(stderr, "vkCreateDevice failed: %d\n", (int)result);
            bink_vulkan_renderer_destroy(r);
            return NULL;
        }
        vkGetDeviceQueue(r->device, r->queue_family, 0, &r->queue);
    }

    if (create_swapchain(r) != 0 || create_render_pass(r) != 0) {
        bink_vulkan_renderer_destroy(r);
        return NULL;
    }
    {
        VkCommandPoolCreateInfo info;

        memset(&info, 0, sizeof(info));
        info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        info.queueFamilyIndex = r->queue_family;
        if (vkCreateCommandPool(r->device, &info, NULL, &r->pool) != VK_SUCCESS) {
            bink_vulkan_renderer_destroy(r);
            return NULL;
        }
    }
    {
        uint32_t i;

        for (i = 0; i < r->image_count; i++) {
            VkImageViewCreateInfo info;
            VkFramebufferCreateInfo fb;

            memset(&info, 0, sizeof(info));
            info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            info.image = r->images[i];
            info.viewType = VK_IMAGE_VIEW_TYPE_2D;
            info.format = r->format;
            info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            info.subresourceRange.levelCount = 1;
            info.subresourceRange.layerCount = 1;
            if (vkCreateImageView(r->device, &info, NULL, &r->views[i]) !=
                VK_SUCCESS)
                goto fail;
            memset(&fb, 0, sizeof(fb));
            fb.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            fb.renderPass = r->render_pass;
            fb.attachmentCount = 1;
            fb.pAttachments = &r->views[i];
            fb.width = r->extent.width;
            fb.height = r->extent.height;
            fb.layers = 1;
            if (vkCreateFramebuffer(r->device, &fb, NULL, &r->framebuffers[i]) !=
                VK_SUCCESS)
                goto fail;
        }
    }

    for (slot = 0; slot < 2; slot++) {
        for (plane = 0; plane < 3; plane++) {
            unsigned long pw = plane == 0 ? w : (w + 1) / 2;
            unsigned long ph = plane == 0 ? h : (h + 1) / 2;

            if (create_image(r, pw, ph, VK_FORMAT_R8_UNORM,
                             VK_IMAGE_USAGE_SAMPLED_BIT |
                                 VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                             &r->planes[slot][plane],
                             &r->plane_mem[slot][plane]) != 0)
                goto fail;
            r->plane_view[slot][plane] =
                make_view(r, r->planes[slot][plane], VK_FORMAT_R8_UNORM);
            if (r->plane_view[slot][plane] == VK_NULL_HANDLE)
                goto fail;
        }
    }
    if (create_image(r, (w + 7) / 8, (h + 7) / 8, VK_FORMAT_R8G8_UNORM,
                     VK_IMAGE_USAGE_SAMPLED_BIT |
                         VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                     &r->motion, &r->motion_mem) != 0)
        goto fail;
    r->motion_view = make_view(r, r->motion, VK_FORMAT_R8G8_UNORM);
    if (r->motion_view == VK_NULL_HANDLE)
        goto fail;

    if (create_image(r, w, h, r->format,
                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                         VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                     &r->capture, &r->capture_mem) != 0)
        goto fail;
    r->capture_view = make_view(r, r->capture, r->format);
    if (r->capture_view == VK_NULL_HANDLE)
        goto fail;
    {
        VkFramebufferCreateInfo fb;

        memset(&fb, 0, sizeof(fb));
        fb.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fb.renderPass = r->render_pass;
        fb.attachmentCount = 1;
        fb.pAttachments = &r->capture_view;
        fb.width = (uint32_t)w;
        fb.height = (uint32_t)h;
        fb.layers = 1;
        if (vkCreateFramebuffer(r->device, &fb, NULL, &r->capture_fb) !=
            VK_SUCCESS)
            goto fail;
    }

    {
        VkSamplerCreateInfo info;

        memset(&info, 0, sizeof(info));
        info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        info.magFilter = VK_FILTER_LINEAR;
        info.minFilter = VK_FILTER_LINEAR;
        info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.maxLod = 1.0f;
        if (vkCreateSampler(r->device, &info, NULL, &r->sampler_linear) !=
            VK_SUCCESS)
            goto fail;
        info.magFilter = VK_FILTER_NEAREST;
        info.minFilter = VK_FILTER_NEAREST;
        if (vkCreateSampler(r->device, &info, NULL, &r->sampler_nearest) !=
            VK_SUCCESS)
            goto fail;
    }

    {
        VkDescriptorSetLayoutBinding bindings[7];
        VkDescriptorSetLayoutCreateInfo info;
        VkDescriptorPoolSize size;
        VkDescriptorPoolCreateInfo pool_info;
        int i;

        memset(bindings, 0, sizeof(bindings));
        for (i = 0; i < 7; i++) {
            bindings[i].binding = (uint32_t)i;
            bindings[i].descriptorType =
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        memset(&info, 0, sizeof(info));
        info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        info.bindingCount = 7;
        info.pBindings = bindings;
        if (vkCreateDescriptorSetLayout(r->device, &info, NULL,
                                        &r->set_layout) != VK_SUCCESS)
            goto fail;
        size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        size.descriptorCount = 7 * BINK_VK_MAX_IMAGES;
        memset(&pool_info, 0, sizeof(pool_info));
        pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_info.maxSets = BINK_VK_MAX_IMAGES;
        pool_info.poolSizeCount = 1;
        pool_info.pPoolSizes = &size;
        if (vkCreateDescriptorPool(r->device, &pool_info, NULL, &r->desc_pool) !=
            VK_SUCCESS)
            goto fail;
        {
            VkDescriptorSetAllocateInfo alloc_info;
            VkDescriptorSetLayout layouts[BINK_VK_MAX_IMAGES];
            uint32_t i2;

            for (i2 = 0; i2 < r->image_count; i2++)
                layouts[i2] = r->set_layout;
            memset(&alloc_info, 0, sizeof(alloc_info));
            alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            alloc_info.descriptorPool = r->desc_pool;
            alloc_info.descriptorSetCount = r->image_count;
            alloc_info.pSetLayouts = layouts;
            if (vkAllocateDescriptorSets(r->device, &alloc_info, r->sets) !=
                VK_SUCCESS)
                goto fail;
        }
    }

    {
        VkPushConstantRange range;

        memset(&range, 0, sizeof(range));
        range.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        range.offset = 0;
        range.size = sizeof(PushParams);
        {
            VkPipelineLayoutCreateInfo info;

            memset(&info, 0, sizeof(info));
            info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
            info.setLayoutCount = 1;
            info.pSetLayouts = &r->set_layout;
            info.pushConstantRangeCount = 1;
            info.pPushConstantRanges = &range;
            if (vkCreatePipelineLayout(r->device, &info, NULL,
                                       &r->pipeline_layout) != VK_SUCCESS)
                goto fail;
        }
    }

    r->vs = make_module(r->device, vk_vert, shaderc_glsl_vertex_shader);
    if (r->vs == VK_NULL_HANDLE)
        goto fail;
    {
        char present_source[4096];
        char interp_source[4096];

        snprintf(present_source, sizeof(present_source), "#version 450\n%s\n%s",
                 vk_common, vk_frag_present);
        snprintf(interp_source, sizeof(interp_source), "#version 450\n%s\n%s",
                 vk_common, vk_frag_interp);
        r->fs_present =
            make_module(r->device, present_source, shaderc_glsl_fragment_shader);
        r->fs_interp =
            make_module(r->device, interp_source, shaderc_glsl_fragment_shader);
    }
    if (r->fs_present == VK_NULL_HANDLE || r->fs_interp == VK_NULL_HANDLE)
        goto fail;

    {
        VkPipelineShaderStageCreateInfo stages[2];
        VkPipelineVertexInputStateCreateInfo vertex_input;
        VkPipelineInputAssemblyStateCreateInfo assembly;
        VkPipelineViewportStateCreateInfo viewport;
        VkPipelineRasterizationStateCreateInfo raster;
        VkPipelineMultisampleStateCreateInfo multisample;
        VkPipelineColorBlendAttachmentState blend;
        VkPipelineColorBlendStateCreateInfo blend_info;
        VkDynamicState dynamic[2] = { VK_DYNAMIC_STATE_VIEWPORT,
                                      VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dynamic_info;
        VkGraphicsPipelineCreateInfo info;
        VkPipeline *targets[2];
        int which;

        memset(stages, 0, sizeof(stages));
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = r->vs;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].pName = "main";

        memset(&vertex_input, 0, sizeof(vertex_input));
        vertex_input.sType =
            VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        memset(&assembly, 0, sizeof(assembly));
        assembly.sType =
            VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        memset(&viewport, 0, sizeof(viewport));
        viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewport.viewportCount = 1;
        viewport.scissorCount = 1;
        memset(&raster, 0, sizeof(raster));
        raster.sType =
            VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = VK_CULL_MODE_NONE;
        raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.lineWidth = 1.0f;
        memset(&multisample, 0, sizeof(multisample));
        multisample.sType =
            VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        memset(&blend, 0, sizeof(blend));
        blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
                               VK_COLOR_COMPONENT_G_BIT |
                               VK_COLOR_COMPONENT_B_BIT |
                               VK_COLOR_COMPONENT_A_BIT;
        memset(&blend_info, 0, sizeof(blend_info));
        blend_info.sType =
            VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        blend_info.attachmentCount = 1;
        blend_info.pAttachments = &blend;
        memset(&dynamic_info, 0, sizeof(dynamic_info));
        dynamic_info.sType =
            VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamic_info.dynamicStateCount = 2;
        dynamic_info.pDynamicStates = dynamic;

        memset(&info, 0, sizeof(info));
        info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        info.stageCount = 2;
        info.pStages = stages;
        info.pVertexInputState = &vertex_input;
        info.pInputAssemblyState = &assembly;
        info.pViewportState = &viewport;
        info.pRasterizationState = &raster;
        info.pMultisampleState = &multisample;
        info.pColorBlendState = &blend_info;
        info.pDynamicState = &dynamic_info;
        info.layout = r->pipeline_layout;
        info.renderPass = r->render_pass;

        targets[0] = &r->pipe_present;
        targets[1] = &r->pipe_interp;
        for (which = 0; which < 2; which++) {
            stages[1].module = which == 0 ? r->fs_present : r->fs_interp;
            if (vkCreateGraphicsPipelines(r->device, VK_NULL_HANDLE, 1, &info,
                                          NULL, targets[which]) != VK_SUCCESS) {
                fprintf(stderr, "Cannot create a graphics pipeline.\n");
                goto fail;
            }
        }
    }

    {
        VkCommandBufferAllocateInfo info;

        memset(&info, 0, sizeof(info));
        info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        info.commandPool = r->pool;
        info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        info.commandBufferCount = r->image_count;
        if (vkAllocateCommandBuffers(r->device, &info, r->cmds) != VK_SUCCESS)
            goto fail;
    }
    {
        VkSemaphoreCreateInfo info;
        VkFenceCreateInfo fence_info;

        memset(&info, 0, sizeof(info));
        info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        memset(&fence_info, 0, sizeof(fence_info));
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if (vkCreateSemaphore(r->device, &info, NULL, &r->sem_available) !=
                VK_SUCCESS ||
            vkCreateSemaphore(r->device, &info, NULL, &r->sem_rendered) !=
                VK_SUCCESS ||
            vkCreateFence(r->device, &fence_info, NULL, &r->fence) !=
                VK_SUCCESS)
            goto fail;
    }
    {
        VkBufferCreateInfo info;
        VkMemoryRequirements requirements;
        VkMemoryAllocateInfo allocate;
        VkPhysicalDeviceMemoryProperties props;
        uint32_t type = 0;
        uint32_t i;
        size_t bytes = (size_t)w * (size_t)h * 4u;

        memset(&info, 0, sizeof(info));
        info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        info.size = bytes;
        info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (vkCreateBuffer(r->device, &info, NULL, &r->readback) != VK_SUCCESS)
            goto fail;
        vkGetBufferMemoryRequirements(r->device, r->readback, &requirements);
        vkGetPhysicalDeviceMemoryProperties(r->physical, &props);
        for (i = 0; i < props.memoryTypeCount; i++) {
            if ((requirements.memoryTypeBits & (1u << i)) &&
                (props.memoryTypes[i].propertyFlags &
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
                type = i;
                break;
            }
        }
        memset(&allocate, 0, sizeof(allocate));
        allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = type;
        if (vkAllocateMemory(r->device, &allocate, NULL, &r->readback_mem) !=
            VK_SUCCESS)
            goto fail;
        vkBindBufferMemory(r->device, r->readback, r->readback_mem, 0);
    }
    return r;

fail:
    bink_vulkan_renderer_destroy(r);
    return NULL;
}

int bink_vulkan_renderer_upload(BinkRenderer *r, bink object, int slot)
{
    const unsigned char *y;
    const unsigned char *u;
    const unsigned char *v;
    unsigned long cw;
    unsigned long ch;

    if (r == NULL || slot < 0 || slot > 1)
        return -1;
    y = bink_get_plane(object, BINK_PLANE_Y);
    u = bink_get_plane(object, BINK_PLANE_U);
    v = bink_get_plane(object, BINK_PLANE_V);
    if (y == NULL || u == NULL || v == NULL)
        return -1;
    cw = (r->w + 1) / 2;
    ch = (r->h + 1) / 2;
    /* The decoder's strides are 32-aligned, so copy tight rows first. */
    {
        unsigned char *tight = (unsigned char *)malloc((size_t)r->w * r->h);

        if (tight == NULL)
            return -1;
        {
            unsigned long row;

            for (row = 0; row < r->h; row++)
                memcpy(tight + row * r->w,
                       y + row * (unsigned long)bink_get_stride(object,
                                                                BINK_PLANE_Y),
                       r->w);
        }
        if (upload_image(r, r->planes[slot][0], tight, r->w, r->h, 1) != 0) {
            free(tight);
            return -1;
        }
        free(tight);
    }
    {
        unsigned char *tight = (unsigned char *)malloc((size_t)cw * ch);

        if (tight == NULL)
            return -1;
        {
            unsigned long row;

            for (row = 0; row < ch; row++)
                memcpy(tight + row * cw,
                       u + row * (unsigned long)bink_get_stride(object,
                                                                BINK_PLANE_U),
                       cw);
        }
        if (upload_image(r, r->planes[slot][1], tight, cw, ch, 1) != 0) {
            free(tight);
            return -1;
        }
        free(tight);
    }
    {
        unsigned char *tight = (unsigned char *)malloc((size_t)cw * ch);

        if (tight == NULL)
            return -1;
        {
            unsigned long row;

            for (row = 0; row < ch; row++)
                memcpy(tight + row * cw,
                       v + row * (unsigned long)bink_get_stride(object,
                                                                BINK_PLANE_V),
                       cw);
        }
        if (upload_image(r, r->planes[slot][2], tight, cw, ch, 1) != 0) {
            free(tight);
            return -1;
        }
        free(tight);
    }
    return 0;
}

int bink_vulkan_renderer_upload_motion(BinkRenderer *r, bink object,
                                       unsigned long *blocks_w,
                                       unsigned long *blocks_h)
{
    const signed char *mx = NULL;
    const signed char *my = NULL;
    unsigned long bw = 0;
    unsigned long bh = 0;
    unsigned char *texels;
    unsigned long i;
    int rc;

    if (r == NULL)
        return -1;
    *blocks_w = 0;
    *blocks_h = 0;
    if (!bink_get_motion(object, &bw, &bh, &mx, &my) || bw == 0 || bh == 0)
        return -1;
    texels = (unsigned char *)malloc((size_t)bw * (size_t)bh * 2u);
    if (texels == NULL)
        return -1;
    for (i = 0; i < bw * bh; i++) {
        texels[i * 2u] = (unsigned char)((int)mx[i] + 128);
        texels[i * 2u + 1u] = (unsigned char)((int)my[i] + 128);
    }
    rc = upload_image(r, r->motion, texels, bw, bh, 2);
    free(texels);
    *blocks_w = bw;
    *blocks_h = bh;
    return rc;
}

static void fill_params(PushParams *p, const BinkRenderParams *params,
                        unsigned long w, unsigned long h)
{
    memset(p, 0, sizeof(*p));
    p->row0[0] = 1.164f;
    p->row0[2] = 1.596f;
    p->row1[0] = 1.164f;
    p->row1[1] = -0.392f;
    p->row1[2] = -0.813f;
    p->row2[0] = 1.164f;
    p->row2[1] = 2.017f;
    {
        const float y0 = 16.0f / 255.0f;
        const float c0 = 128.0f / 255.0f;

        p->offset[0] = -1.164f * y0 - 1.596f * c0;
        p->offset[1] = -1.164f * y0 + 0.392f * c0 + 0.813f * c0;
        p->offset[2] = -1.164f * y0 - 2.017f * c0;
    }
    p->luma[0] = (float)w;
    p->luma[1] = (float)h;
    p->luma[2] = params->t;
    p->blocks[0] = (float)(params->blocks_w > 0 ? params->blocks_w : 1);
    p->blocks[1] = (float)(params->blocks_h > 0 ? params->blocks_h : 1);
}

static void write_set(BinkRenderer *r, VkDescriptorSet set,
                      const BinkRenderParams *params)
{
    VkDescriptorImageInfo images[7];
    VkWriteDescriptorSet writes[7];
    int cur = params->slot_cur & 1;
    int next = cur ^ 1;
    int interp = params->interp && params->have_next;
    int count = interp ? 7 : 3;
    int i;

    memset(images, 0, sizeof(images));
    images[0].imageView = r->plane_view[cur][0];
    images[1].imageView = r->plane_view[cur][1];
    images[2].imageView = r->plane_view[cur][2];
    images[3].imageView = r->plane_view[next][0];
    images[4].imageView = r->plane_view[next][1];
    images[5].imageView = r->plane_view[next][2];
    images[6].imageView = r->motion_view;
    for (i = 0; i < 7; i++) {
        images[i].sampler = i == 6 ? r->sampler_nearest : r->sampler_linear;
        images[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        memset(&writes[i], 0, sizeof(writes[i]));
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set;
        writes[i].dstBinding = (uint32_t)i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &images[i];
    }
    vkUpdateDescriptorSets(r->device, (uint32_t)count, writes, 0, NULL);
}

static void record(BinkRenderer *r, VkCommandBuffer cmd, VkFramebuffer fb,
                   const BinkRenderParams *params, int image_index)
{
    VkCommandBufferBeginInfo begin;
    VkRenderPassBeginInfo pass;
    VkClearValue clear;
    VkViewport viewport;
    VkRect2D scissor;
    PushParams values;

    memset(&begin, 0, sizeof(begin));
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(cmd, &begin);
    clear.color.float32[0] = 0.0f;
    clear.color.float32[1] = 0.0f;
    clear.color.float32[2] = 0.0f;
    clear.color.float32[3] = 1.0f;
    memset(&pass, 0, sizeof(pass));
    pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    pass.renderPass = r->render_pass;
    pass.framebuffer = fb;
    pass.renderArea.extent.width = (uint32_t)r->w;
    pass.renderArea.extent.height = (uint32_t)r->h;
    pass.clearValueCount = 1;
    pass.pClearValues = &clear;
    vkCmdBeginRenderPass(cmd, &pass, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      params->interp && params->have_next ? r->pipe_interp
                                                          : r->pipe_present);
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = (float)r->w;
    viewport.height = (float)r->h;
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    scissor.offset.x = 0;
    scissor.offset.y = 0;
    scissor.extent.width = (uint32_t)r->w;
    scissor.extent.height = (uint32_t)r->h;
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            r->pipeline_layout, 0, 1, &r->sets[image_index], 0,
                            NULL);
    fill_params(&values, params, r->w, r->h);
    vkCmdPushConstants(cmd, r->pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(values), &values);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);
    vkEndCommandBuffer(cmd);
}

int bink_vulkan_renderer_draw(BinkRenderer *r, const BinkRenderParams *params)
{
    uint32_t index = 0;
    VkResult result;
    VkSubmitInfo submit;
    VkPresentInfoKHR present;

    if (r == NULL || params == NULL)
        return -1;
    vkWaitForFences(r->device, 1, &r->fence, VK_TRUE, UINT64_MAX);
    vkResetFences(r->device, 1, &r->fence);

    if (params->capture) {
        write_set(r, r->sets[0], params);
        record(r, r->cmds[0], r->capture_fb, params, 0);
        memset(&submit, 0, sizeof(submit));
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &r->cmds[0];
        if (vkQueueSubmit(r->queue, 1, &submit, r->fence) != VK_SUCCESS)
            return -1;
        vkWaitForFences(r->device, 1, &r->fence, VK_TRUE, UINT64_MAX);
        return 0;
    }

    result = vkAcquireNextImageKHR(r->device, r->swapchain, UINT64_MAX,
                                   r->sem_available, VK_NULL_HANDLE, &index);
    if (result != VK_SUCCESS)
        return -1;
    if (index >= r->image_count)
        return -1;
    write_set(r, r->sets[index], params);
    record(r, r->cmds[index], r->framebuffers[index], params, (int)index);
    {
        VkPipelineStageFlags wait = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;

        memset(&submit, 0, sizeof(submit));
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &r->sem_available;
        submit.pWaitDstStageMask = &wait;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &r->cmds[index];
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &r->sem_rendered;
        if (vkQueueSubmit(r->queue, 1, &submit, r->fence) != VK_SUCCESS)
            return -1;
    }
    memset(&present, 0, sizeof(present));
    present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &r->sem_rendered;
    present.swapchainCount = 1;
    present.pSwapchains = &r->swapchain;
    present.pImageIndices = &index;
    if (vkQueuePresentKHR(r->queue, &present) != VK_SUCCESS)
        return -1;
    return 0;
}

int bink_vulkan_renderer_readback(BinkRenderer *r, unsigned char *rgb,
                                  unsigned long w, unsigned long h)
{
    VkCommandBuffer cmd;
    VkCommandBufferAllocateInfo cmd_info;
    VkCommandBufferBeginInfo begin;
    VkBufferImageCopy copy;
    VkSubmitInfo submit;
    VkImageMemoryBarrier barrier;
    VkMappedMemoryRange range;
    void *mapped = NULL;
    unsigned char *pixels;
    unsigned long y;
    unsigned long x;

    if (r == NULL || rgb == NULL || w == 0 || h == 0)
        return -1;
    memset(&cmd_info, 0, sizeof(cmd_info));
    cmd_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmd_info.commandPool = r->pool;
    cmd_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmd_info.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(r->device, &cmd_info, &cmd) != VK_SUCCESS)
        return -1;
    memset(&begin, 0, sizeof(begin));
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);
    memset(&barrier, 0, sizeof(barrier));
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = r->capture;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1,
                         &barrier);
    memset(&copy, 0, sizeof(copy));
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1;
    copy.imageExtent.width = (uint32_t)r->w;
    copy.imageExtent.height = (uint32_t)r->h;
    copy.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(cmd, r->capture, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           r->readback, 1, &copy);
    vkEndCommandBuffer(cmd);
    memset(&submit, 0, sizeof(submit));
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    vkQueueSubmit(r->queue, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(r->queue);
    vkFreeCommandBuffers(r->device, r->pool, 1, &cmd);

    memset(&range, 0, sizeof(range));
    range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    range.memory = r->readback_mem;
    range.size = VK_WHOLE_SIZE;
    vkInvalidateMappedMemoryRanges(r->device, 1, &range);
    if (vkMapMemory(r->device, r->readback_mem, 0,
                    (VkDeviceSize)r->w * r->h * 4u, 0, &mapped) != VK_SUCCESS)
        return -1;
    pixels = (unsigned char *)mapped;
    for (y = 0; y < h; y++) {
        const unsigned char *row =
            pixels + (size_t)(y * r->h / h) * (size_t)r->w * 4u;

        for (x = 0; x < w; x++) {
            const unsigned char *p = row + (size_t)(x * r->w / w) * 4u;

            rgb[(y * w + x) * 3u + 0] = p[2];
            rgb[(y * w + x) * 3u + 1] = p[1];
            rgb[(y * w + x) * 3u + 2] = p[0];
        }
    }
    vkUnmapMemory(r->device, r->readback_mem);
    return 0;
}

void bink_vulkan_renderer_destroy(BinkRenderer *r)
{
    int slot;
    int plane;
    uint32_t i;

    if (r == NULL)
        return;
    if (r->device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(r->device);
        if (r->sem_rendered != VK_NULL_HANDLE)
            vkDestroySemaphore(r->device, r->sem_rendered, NULL);
        if (r->sem_available != VK_NULL_HANDLE)
            vkDestroySemaphore(r->device, r->sem_available, NULL);
        if (r->fence != VK_NULL_HANDLE)
            vkDestroyFence(r->device, r->fence, NULL);
        if (r->readback != VK_NULL_HANDLE)
            vkDestroyBuffer(r->device, r->readback, NULL);
        if (r->readback_mem != VK_NULL_HANDLE)
            vkFreeMemory(r->device, r->readback_mem, NULL);
        if (r->capture_fb != VK_NULL_HANDLE)
            vkDestroyFramebuffer(r->device, r->capture_fb, NULL);
        if (r->capture_view != VK_NULL_HANDLE)
            vkDestroyImageView(r->device, r->capture_view, NULL);
        if (r->capture != VK_NULL_HANDLE)
            vkDestroyImage(r->device, r->capture, NULL);
        if (r->capture_mem != VK_NULL_HANDLE)
            vkFreeMemory(r->device, r->capture_mem, NULL);
        for (slot = 0; slot < 2; slot++) {
            for (plane = 0; plane < 3; plane++) {
                if (r->plane_view[slot][plane] != VK_NULL_HANDLE)
                    vkDestroyImageView(r->device, r->plane_view[slot][plane],
                                       NULL);
                if (r->planes[slot][plane] != VK_NULL_HANDLE)
                    vkDestroyImage(r->device, r->planes[slot][plane], NULL);
                if (r->plane_mem[slot][plane] != VK_NULL_HANDLE)
                    vkFreeMemory(r->device, r->plane_mem[slot][plane], NULL);
            }
        }
        if (r->motion_view != VK_NULL_HANDLE)
            vkDestroyImageView(r->device, r->motion_view, NULL);
        if (r->motion != VK_NULL_HANDLE)
            vkDestroyImage(r->device, r->motion, NULL);
        if (r->motion_mem != VK_NULL_HANDLE)
            vkFreeMemory(r->device, r->motion_mem, NULL);
        if (r->sampler_linear != VK_NULL_HANDLE)
            vkDestroySampler(r->device, r->sampler_linear, NULL);
        if (r->sampler_nearest != VK_NULL_HANDLE)
            vkDestroySampler(r->device, r->sampler_nearest, NULL);
        if (r->pipe_present != VK_NULL_HANDLE)
            vkDestroyPipeline(r->device, r->pipe_present, NULL);
        if (r->pipe_interp != VK_NULL_HANDLE)
            vkDestroyPipeline(r->device, r->pipe_interp, NULL);
        if (r->pipeline_layout != VK_NULL_HANDLE)
            vkDestroyPipelineLayout(r->device, r->pipeline_layout, NULL);
        if (r->desc_pool != VK_NULL_HANDLE)
            vkDestroyDescriptorPool(r->device, r->desc_pool, NULL);
        if (r->set_layout != VK_NULL_HANDLE)
            vkDestroyDescriptorSetLayout(r->device, r->set_layout, NULL);
        if (r->fs_interp != VK_NULL_HANDLE)
            vkDestroyShaderModule(r->device, r->fs_interp, NULL);
        if (r->fs_present != VK_NULL_HANDLE)
            vkDestroyShaderModule(r->device, r->fs_present, NULL);
        if (r->vs != VK_NULL_HANDLE)
            vkDestroyShaderModule(r->device, r->vs, NULL);
        for (i = 0; i < r->image_count; i++) {
            if (r->framebuffers[i] != VK_NULL_HANDLE)
                vkDestroyFramebuffer(r->device, r->framebuffers[i], NULL);
            if (r->views[i] != VK_NULL_HANDLE)
                vkDestroyImageView(r->device, r->views[i], NULL);
        }
        if (r->render_pass != VK_NULL_HANDLE)
            vkDestroyRenderPass(r->device, r->render_pass, NULL);
        if (r->pool != VK_NULL_HANDLE)
            vkDestroyCommandPool(r->device, r->pool, NULL);
        if (r->swapchain != VK_NULL_HANDLE)
            vkDestroySwapchainKHR(r->device, r->swapchain, NULL);
        vkDestroyDevice(r->device, NULL);
    }
    if (r->surface != VK_NULL_HANDLE && r->instance != VK_NULL_HANDLE)
        vkDestroySurfaceKHR(r->instance, r->surface, NULL);
    if (r->instance != VK_NULL_HANDLE)
        vkDestroyInstance(r->instance, NULL);
    SDL_Vulkan_UnloadLibrary();
    free(r);
}
