/*
 * hw_headless.c -- native tools only: an off-screen Vulkan device for the
 * enhanced renderer (the replay tool and the self-test draw with it), and
 * PNG writers for what they draw.
 */
#ifndef AC3_HW_WEBGPU
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#include "hw_glue_internal.h"

extern int ac3_write_png_rgb(const char *path, unsigned w, unsigned h,
                             const unsigned char *rgb);

hw_headless_t HV;
double hw_headless_flush_ms;

int headless_init(int scale) {
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "ac3_enhanced";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ci = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ci.pApplicationInfo = &app;
    if (vkCreateInstance(&ci, NULL, &HV.inst) != VK_SUCCESS) return 0;
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(HV.inst, &n, NULL);
    if (!n) return 0;
    VkPhysicalDevice devs[8];
    if (n > 8) n = 8;
    vkEnumeratePhysicalDevices(HV.inst, &n, devs);
    HV.phys = VK_NULL_HANDLE;
    for (uint32_t i = 0; i < n && !HV.phys; i++) {
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &qn, NULL);
        VkQueueFamilyProperties qs[16];
        if (qn > 16) qn = 16;
        vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &qn, qs);
        for (uint32_t q = 0; q < qn; q++)
            if (qs[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) { HV.phys = devs[i]; HV.qf = q; break; }
    }
    if (!HV.phys) return 0;
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qi.queueFamilyIndex = HV.qf;
    qi.queueCount = 1;
    qi.pQueuePriorities = &prio;
    VkPhysicalDeviceFeatures feat = { 0 };
    feat.independentBlend = VK_TRUE;
    VkDeviceCreateInfo di = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    di.queueCreateInfoCount = 1;
    di.pQueueCreateInfos = &qi;
    di.pEnabledFeatures = &feat;
    if (vkCreateDevice(HV.phys, &di, NULL, &HV.dev) != VK_SUCCESS) return 0;
    vkGetDeviceQueue(HV.dev, HV.qf, 0, &HV.q);
    VkCommandPoolCreateInfo pi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pi.queueFamilyIndex = HV.qf;
    if (vkCreateCommandPool(HV.dev, &pi, NULL, &HV.pool) != VK_SUCCESS) return 0;
    VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    ai.commandPool = HV.pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    vkAllocateCommandBuffers(HV.dev, &ai, &HV.cb);
    hw_config_t cfg = { scale, ac3_hd.perspective };
    return hw_init(HV.phys, HV.dev, HV.qf, &cfg);
}

/* records and submits the frame drawn so far, and waits for it */
void headless_flush(void) {
    LARGE_INTEGER f0, f1, fq;
    QueryPerformanceFrequency(&fq);
    QueryPerformanceCounter(&f0);
    vkResetCommandBuffer(HV.cb, 0);
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(HV.cb, &bi);
    hw_record(HV.cb, 0);
    vkEndCommandBuffer(HV.cb);
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers = &HV.cb;
    vkQueueSubmit(HV.q, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(HV.q);
    QueryPerformanceCounter(&f1);
    hw_headless_flush_ms = (double)(f1.QuadPart - f0.QuadPart) * 1000.0 / (double)fq.QuadPart;
}

void headless_shutdown(void) {
    hw_shutdown();
    if (HV.pool) vkDestroyCommandPool(HV.dev, HV.pool, NULL);
    if (HV.dev) vkDestroyDevice(HV.dev, NULL);
    if (HV.inst) vkDestroyInstance(HV.inst, NULL);
    memset(&HV, 0, sizeof HV);
}

void write_rgba_png(const char *path, const u8 *rgba, unsigned w, unsigned h) {
    u8 *rgb = malloc((size_t)w * h * 3);
    if (!rgb) return;
    for (size_t i = 0; i < (size_t)w * h; i++) {
        rgb[i * 3 + 0] = rgba[i * 4 + 0];
        rgb[i * 3 + 1] = rgba[i * 4 + 1];
        rgb[i * 3 + 2] = rgba[i * 4 + 2];
    }
    ac3_write_png_rgb(path, w, h, rgb);
    free(rgb);
}

/* the software GPU's VRAM (the game's own picture) */
void write_sw_png(const char *path, int x, int y, int w, int h) {
    u8 *rgb = malloc((size_t)w * h * 3);
    if (!rgb) return;
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            u16 p = *vram_at((x + i) & 1023, (y + j) & 511);
            u8 *o = rgb + ((size_t)j * w + i) * 3;
            o[0] = (u8)(((p & 31) << 3) | ((p & 31) >> 2));
            o[1] = (u8)((((p >> 5) & 31) << 3) | (((p >> 5) & 31) >> 2));
            o[2] = (u8)((((p >> 10) & 31) << 3) | (((p >> 10) & 31) >> 2));
        }
    ac3_write_png_rgb(path, (unsigned)w, (unsigned)h, rgb);
    free(rgb);
}

/* depth as grey, near is bright (blue: nothing drawn, red: the far plane) */
void write_depth_png(const char *path, const float *z, unsigned w, unsigned h) {
    float zmax = 1.0f;
    for (size_t i = 0; i < (size_t)w * h; i++) if (z[i] > zmax && z[i] < 1.0e8f) zmax = z[i];
    u8 *rgb = malloc((size_t)w * h * 3);
    if (!rgb) return;
    for (size_t i = 0; i < (size_t)w * h; i++) {
        u8 *o = rgb + i * 3;
        if (z[i] == 0.0f) { o[0] = 0; o[1] = 0; o[2] = 96; continue; }
        if (z[i] >= 1.0e8f) { o[0] = 64; o[1] = 0; o[2] = 0; continue; }
        float zz = z[i] < 0.0f ? -z[i] : z[i];
        float t = 1.0f - (float)(log((double)zz + 1.0) / log((double)zmax + 1.0));
        if (t < 0.0f) t = 0.0f;
        u8 v = (u8)(t * 255.0f);
        o[0] = z[i] < 0.0f ? 0 : v;
        o[1] = o[2] = v;
    }
    ac3_write_png_rgb(path, w, h, rgb);
    free(rgb);
}

#endif /* !AC3_HW_WEBGPU */
