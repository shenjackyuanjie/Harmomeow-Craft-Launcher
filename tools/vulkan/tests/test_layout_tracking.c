/* 直接编译生产 shim，用假的 Vulkan loader 验证布局记录；不连接 GPU 或设备。 */
#define _GNU_SOURCE
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fail_calloc, fail_realloc;
static void* test_calloc(size_t n, size_t size) {
    if (fail_calloc) { fail_calloc = 0; return NULL; }
    return calloc(n, size);
}
static void* test_realloc(void* p, size_t size) {
    if (fail_realloc) { fail_realloc = 0; return NULL; }
    return realloc(p, size);
}
#define calloc test_calloc
#define realloc test_realloc
#ifndef MEOW_VK_SOURCE
#define MEOW_VK_SOURCE "../../../libs/meowcraftlib/src/main/cpp/meowcraftbridge/meowvulkan.c"
#endif
#include MEOW_VK_SOURCE
#undef calloc
#undef realloc

static uintptr_t next_pipeline = 0x10000, next_dsl = 0x20000;
static unsigned destroyed_pipeline, destroyed_dsl;
static const void* last_allocator;
static int unavailable_swapchain;
static void fake_other(void) {}
static int fake_create_pipeline(void* dev, const void* ci, const void* allocator, void** out) {
    (void)dev; (void)ci; (void)allocator;
    *out = (void*)next_pipeline++;
    return VK_SUCCESS;
}
static int fake_create_dsl(void* dev, const void* ci, const void* allocator, void** out) {
    (void)dev; (void)ci; (void)allocator;
    *out = (void*)next_dsl++;
    return VK_SUCCESS;
}
static void fake_destroy_pipeline(VkDevice dev, VkPipelineLayout layout, const VkAllocationCallbacks* allocator) {
    (void)dev; (void)layout;
    last_allocator = allocator;
    ++destroyed_pipeline;
}
static void fake_destroy_dsl(VkDevice dev, VkDescriptorSetLayout layout, const VkAllocationCallbacks* allocator) {
    (void)dev; (void)layout;
    last_allocator = allocator;
    ++destroyed_dsl;
}
static PFN_vkVoidFunctionLocal fake_gdpa(VkDevice dev, const char* name) {
    (void)dev;
    if (!strcmp(name, "vkCreatePipelineLayout")) return (PFN_vkVoidFunctionLocal)fake_create_pipeline;
    if (!strcmp(name, "vkDestroyPipelineLayout")) return (PFN_vkVoidFunctionLocal)fake_destroy_pipeline;
    if (!strcmp(name, "vkCreateDescriptorSetLayout")) return (PFN_vkVoidFunctionLocal)fake_create_dsl;
    if (!strcmp(name, "vkDestroyDescriptorSetLayout")) return (PFN_vkVoidFunctionLocal)fake_destroy_dsl;
    return fake_other;
}
static VkResult fake_create_device(VkPhysicalDevice pdev, const VkDeviceCI* ci,
                                   const void* allocator, VkDevice* out) {
    (void)pdev; (void)ci; (void)allocator;
    *out = (VkDevice)1;
    return VK_SUCCESS;
}
static PFN_vkVoidFunctionLocal fake_gipa(VkInstance inst, const char* name) {
    (void)inst;
    if (unavailable_swapchain && (!strcmp(name, "vkCreateSwapchainKHR") || !strcmp(name, "vkCreateSwapchain"))) return NULL;
    if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunctionLocal)fake_gdpa;
    if (!strcmp(name, "vkCreateDevice")) return (PFN_vkVoidFunctionLocal)fake_create_device;
    return fake_gdpa((VkDevice)1, name);
}
static void reset_state(void) {
    meow_f72_shutdown();
    destroyed_pipeline = destroyed_dsl = 0;
    next_pipeline = 0x10000; next_dsl = 0x20000;
    fail_calloc = fail_realloc = 0;
    unavailable_swapchain = 0;
    g_real = (void*)1; /* 让 init_once 不打开真正的 /system loader。 */
    g_hooks = 1;
    g_gipa = fake_gipa; g_gdpa = fake_gdpa;
    g_dev_seen = (VkDevice)1;
    g_inst_seen = (VkInstance)2;
}
static void* create_pipeline(VkDescriptorSetLayout dsl) {
    VkPipelineLayoutCreateInfo ci = {0};
    ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    ci.setLayoutCount = 1; ci.pSetLayouts = &dsl;
    void* out = NULL;
    assert(log_vkCreatePipelineLayout((void*)1, &ci, NULL, &out) == VK_SUCCESS);
    return out;
}
static void* create_dsl(int push) {
    VkDescriptorSetLayoutBinding b = {0};
    b.binding = 0; b.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    b.descriptorCount = 1; b.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    VkDescriptorSetLayoutCreateInfo ci = {0};
    ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    ci.flags = push ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR : 0;
    ci.bindingCount = 1; ci.pBindings = &b;
    void* out = NULL;
    assert(log_vkCreateDescriptorSetLayout((void*)1, &ci, NULL, &out) == VK_SUCCESS);
    return out;
}
static void test_lookup_paths(void) {
    reset_state();
    const char* names[] = {
        "vkCreatePipelineLayout", "vkDestroyPipelineLayout", "vkCreateDescriptorSetLayout",
        "vkDestroyDescriptorSetLayout", "vkCreateSemaphore", "vkDestroySemaphore",
        "vkWaitSemaphores", "vkWaitSemaphoresKHR", "vkGetSemaphoreCounterValueKHR",
        "vkQueueSubmit", "vkQueueSubmit2KHR", "vkCmdPushDescriptorSetKHR",
        "vkCmdPushDescriptorSetWithTemplateKHR", "vkCreateDescriptorUpdateTemplateKHR",
        "vkCmdPipelineBarrier2KHR", "vkCmdBeginRenderingKHR", "vkCmdBindDescriptorSets",
        "vkCreateGraphicsPipelines", "vkDestroyDevice"
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        PFN_vkVoidFunctionLocal a = vkGetInstanceProcAddr((VkInstance)2, names[i]);
        PFN_vkVoidFunctionLocal b = vkGetDeviceProcAddr((VkDevice)1, names[i]);
        if (a != b) fprintf(stderr, "查找路径不一致：%s\n", names[i]);
        assert(a == b);
        assert(a != fake_gdpa((VkDevice)1, names[i]));
    }
    /* 经 GIPA 创建、经 GDPA 使用时仍必须捕获布局。 */
    typedef int (*Create)(void*, const void*, const void*, void**);
    Create create = (Create)vkGetInstanceProcAddr((VkInstance)2, "vkCreatePipelineLayout");
    VkDescriptorSetLayout dsl = (VkDescriptorSetLayout)0x3333;
    VkPipelineLayoutCreateInfo ci = {0};
    ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    ci.setLayoutCount = 1; ci.pSetLayouts = &dsl;
    void* layout = NULL;
    assert(create((void*)1, &ci, NULL, &layout) == VK_SUCCESS);
    assert(meow_f72_lookup_dsl(layout, 0) == dsl);
    assert(vkGetInstanceProcAddr((VkInstance)2, "vkUnknownTest") == fake_other);
    assert(vkGetDeviceProcAddr((VkDevice)1, "vkUnknownTest") == fake_other);
    g_hooks = 0;
    assert(vkGetInstanceProcAddr((VkInstance)2, "vkCreatePipelineLayout") ==
           (PFN_vkVoidFunctionLocal)fake_create_pipeline);
    assert(vkGetDeviceProcAddr((VkDevice)1, "vkCreatePipelineLayout") ==
           (PFN_vkVoidFunctionLocal)fake_create_pipeline);
    g_hooks = 1;
    meow_f72_shutdown();
}
static void test_unavailable_gipa_command(void) {
    reset_state();
    assert(vkGetInstanceProcAddr((VkInstance)2, "vkCreateSemaphoreKHR") == NULL);
    assert(vkGetDeviceProcAddr((VkDevice)1, "vkDestroySemaphoreKHR") == NULL);
    unavailable_swapchain = 1;
    assert(vkGetInstanceProcAddr((VkInstance)2, "vkCreateSwapchainKHR") == NULL);
    unavailable_swapchain = 0;
    assert(vkGetInstanceProcAddr((VkInstance)2, "vkCreateSwapchainKHR") ==
           (PFN_vkVoidFunctionLocal)log_vkCreateSwapchainKHR);
    meow_f72_shutdown();
}
static void test_gipa_only(void) {
    reset_state();
    g_gdpa = NULL; g_dev_seen = VK_NULL_HANDLE;
    PFN_vkCreateDevice create = (PFN_vkCreateDevice)vkGetInstanceProcAddr((VkInstance)2, "vkCreateDevice");
    VkDeviceCreateInfo dci = {0};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    VkDevice dev = VK_NULL_HANDLE;
    assert(create((VkPhysicalDevice)3, &dci, NULL, &dev) == VK_SUCCESS);
    assert(g_dev_seen == dev && g_gdpa == fake_gdpa);
    typedef int (*CreateLayout)(void*, const void*, const void*, void**);
    CreateLayout create_layout = (CreateLayout)vkGetInstanceProcAddr((VkInstance)2, "vkCreatePipelineLayout");
    VkDescriptorSetLayout dsl = (VkDescriptorSetLayout)0x8888;
    VkPipelineLayoutCreateInfo ci = {0};
    ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    ci.setLayoutCount = 1; ci.pSetLayouts = &dsl;
    void* layout = NULL;
    assert(create_layout(dev, &ci, NULL, &layout) == VK_SUCCESS);
    assert(meow_f72_lookup_dsl(layout, 0) == dsl);
    meow_f72_shutdown();
}
static void test_many_pipeline_layouts(void) {
    reset_state();
    void* layouts[1024];
    for (uintptr_t i = 0; i < 1024; ++i)
        layouts[i] = create_pipeline((VkDescriptorSetLayout)(0x30000 + i));
    for (uintptr_t i = 0; i < 1024; ++i) {
        assert(meow_f72_lookup_dsl(layouts[i], 0) == (VkDescriptorSetLayout)(0x30000 + i));
        assert(meow_f72_lookup_dsl(layouts[i], 1) == VK_NULL_HANDLE);
    }
    meow_f72_shutdown();
}
static void test_many_descriptor_layouts(void) {
    reset_state();
    void* layouts[1024];
    VkDescriptorSetLayout mirrors[1024];
    for (size_t i = 0; i < 1024; ++i) {
        layouts[i] = create_dsl(1);
        mirrors[i] = meow_f72_mirror_of((VkDescriptorSetLayout)layouts[i]);
        assert(mirrors[i] != VK_NULL_HANDLE);
    }
    /* 扩容不能销毁任何仍存活的 mirror。 */
    assert(destroyed_dsl == 0);
    for (size_t i = 0; i < 1024; ++i)
        assert(meow_f72_mirror_of((VkDescriptorSetLayout)layouts[i]) == mirrors[i]);
    for (size_t i = 0; i < 1024; i += 2)
        log_vkDestroyDescriptorSetLayout((void*)1, layouts[i], NULL);
    assert(destroyed_dsl == 1024); /* 每次分别销毁 mirror 和原始 DSL。 */
    for (size_t i = 1; i < 1024; i += 2)
        assert(meow_f72_mirror_of((VkDescriptorSetLayout)layouts[i]) == mirrors[i]);
    meow_f72_shutdown();
    assert(destroyed_dsl == 1536);
    meow_f72_shutdown();
    assert(destroyed_dsl == 1536); /* teardown 幂等、不能重复销毁。 */
}
static void test_pipeline_handle_reuse(void) {
    reset_state();
    void* layout = create_pipeline((VkDescriptorSetLayout)0x4444);
    assert(meow_f72_lookup_dsl(layout, 0) == (VkDescriptorSetLayout)0x4444);
    typedef void (*Destroy)(void*, void*, const void*);
    Destroy destroy = (Destroy)vkGetDeviceProcAddr((VkDevice)1, "vkDestroyPipelineLayout");
    VkAllocationCallbacks allocator = {0};
    destroy((void*)1, layout, &allocator);
    assert(destroyed_pipeline == 1 && last_allocator == &allocator);
    assert(meow_f72_lookup_dsl(layout, 0) == VK_NULL_HANDLE);
    next_pipeline = (uintptr_t)layout;
    void* reused = create_pipeline((VkDescriptorSetLayout)0x5555);
    assert(reused == layout);
    assert(meow_f72_lookup_dsl(reused, 0) == (VkDescriptorSetLayout)0x5555);
    meow_f72_shutdown();
    assert(meow_f72_lookup_dsl(reused, 0) == VK_NULL_HANDLE);
    /* 同一槽位/句柄在新设备周期中必须使用新映射。 */
    reset_state();
    reused = create_pipeline((VkDescriptorSetLayout)0x6666);
    assert(meow_f72_lookup_dsl(reused, 0) == (VkDescriptorSetLayout)0x6666);
    meow_f72_shutdown();
}
static void test_interned_handles(void) {
    reset_state();
    typedef void (*Destroy)(void*, void*, const void*);
    Destroy destroy = (Destroy)vkGetDeviceProcAddr((VkDevice)1, "vkDestroyPipelineLayout");
    void* same = create_pipeline((VkDescriptorSetLayout)0x3030);
    next_pipeline = (uintptr_t)same;
    assert(create_pipeline((VkDescriptorSetLayout)0x3030) == same);
    destroy((void*)1, same, NULL);
    assert(meow_f72_lookup_dsl(same, 0) == (VkDescriptorSetLayout)0x3030);
    destroy((void*)1, same, NULL);
    assert(meow_f72_lookup_dsl(same, 0) == VK_NULL_HANDLE);
    reset_state();
    void* hole = create_pipeline((VkDescriptorSetLayout)0x1010);
    void* shared = create_pipeline((VkDescriptorSetLayout)0x2020);
    destroy((void*)1, hole, NULL); /* 查重必须越过前方空槽。 */
    next_pipeline = (uintptr_t)shared;
    assert(create_pipeline((VkDescriptorSetLayout)0x2020) == shared);
    destroy((void*)1, shared, NULL);
    assert(meow_f72_lookup_dsl(shared, 0) == (VkDescriptorSetLayout)0x2020);
    destroy((void*)1, shared, NULL);
    assert(meow_f72_lookup_dsl(shared, 0) == VK_NULL_HANDLE);
    void* dsl = create_dsl(1);
    VkDescriptorSetLayout mirror = meow_f72_mirror_of((VkDescriptorSetLayout)dsl);
    next_dsl = (uintptr_t)dsl;
    assert(create_dsl(1) == dsl);
    log_vkDestroyDescriptorSetLayout((void*)1, dsl, NULL);
    assert(meow_f72_mirror_of((VkDescriptorSetLayout)dsl) == mirror && destroyed_dsl == 1);
    log_vkDestroyDescriptorSetLayout((void*)1, dsl, NULL);
    assert(meow_f72_mirror_of((VkDescriptorSetLayout)dsl) == VK_NULL_HANDLE && destroyed_dsl == 3);
    meow_f72_shutdown();
}
static void test_oom_rollback(void) {
    reset_state();
    VkDescriptorSetLayout dsl = (VkDescriptorSetLayout)0x7777;
    VkPipelineLayoutCreateInfo ci = {0};
    ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    ci.setLayoutCount = 1; ci.pSetLayouts = &dsl;
    VkAllocationCallbacks allocator = {0};
    void* out = NULL;
    fail_calloc = 1;
    assert(log_vkCreatePipelineLayout((void*)1, &ci, &allocator, &out) == VK_ERROR_OUT_OF_HOST_MEMORY);
    assert(out == NULL && destroyed_pipeline == 1 && last_allocator == &allocator);
    fail_realloc = 1;
    assert(log_vkCreatePipelineLayout((void*)1, &ci, &allocator, &out) == VK_ERROR_OUT_OF_HOST_MEMORY);
    assert(out == NULL && destroyed_pipeline == 2);
    void* live[256];
    for (size_t i = 0; i < 256; ++i) live[i] = create_pipeline(dsl);
    fail_realloc = 1;
    assert(log_vkCreatePipelineLayout((void*)1, &ci, &allocator, &out) == VK_ERROR_OUT_OF_HOST_MEMORY);
    assert(out == NULL && destroyed_pipeline == 3);
    for (size_t i = 0; i < 256; ++i) assert(meow_f72_lookup_dsl(live[i], 0) == dsl);
    VkDescriptorSetLayoutCreateInfo dci = {0};
    dci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    fail_realloc = 1;
    assert(log_vkCreateDescriptorSetLayout((void*)1, &dci, &allocator, &out) == VK_ERROR_OUT_OF_HOST_MEMORY);
    assert(out == NULL && destroyed_dsl == 1 && last_allocator == &allocator);
    void* push_layouts[256];
    VkDescriptorSetLayout mirrors[256];
    for (size_t i = 0; i < 256; ++i) {
        push_layouts[i] = create_dsl(1);
        mirrors[i] = meow_f72_mirror_of((VkDescriptorSetLayout)push_layouts[i]);
    }
    dci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    fail_realloc = 1;
    assert(log_vkCreateDescriptorSetLayout((void*)1, &dci, &allocator, &out) == VK_ERROR_OUT_OF_HOST_MEMORY);
    assert(out == NULL && destroyed_dsl == 2 && last_allocator == &allocator);
    for (size_t i = 0; i < 256; ++i)
        assert(meow_f72_mirror_of((VkDescriptorSetLayout)push_layouts[i]) == mirrors[i]);
    meow_f72_shutdown();
}
int main(int argc, char** argv) {
    const struct { const char* name; void (*run)(void); } cases[] = {
        {"lookup", test_lookup_paths}, {"gipa-only", test_gipa_only},
        {"gipa-unavailable", test_unavailable_gipa_command},
        {"pipeline-capacity", test_many_pipeline_layouts},
        {"descriptor-capacity", test_many_descriptor_layouts},
        {"handle-reuse", test_pipeline_handle_reuse}, {"interned-handles", test_interned_handles},
        {"oom", test_oom_rollback}
    };
    int ran = 0;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        if (argc > 1 && strcmp(argv[1], cases[i].name)) continue;
        cases[i].run();
        printf("通过：%s\n", cases[i].name);
        ++ran;
    }
    return ran ? 0 : 2;
}
