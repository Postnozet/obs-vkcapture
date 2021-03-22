/*
OBS Linux Vulkan game capture
Copyright (C) 2021 David Rosca <nowrep@gmail.com>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include "vklayer.h"
#include "utils.h"

#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <vulkan/vk_layer.h>

// Based on obs-studio/plugins/win-capture/graphics-hook/vulkan-capture.c

/* ======================================================================== */
/* defs/statics                                                             */

/* use the loader's dispatch table pointer as a key for internal data maps */
#define GET_LDT(x) (*(void **)x)

static bool vulkan_seen = false;

/* ======================================================================== */
/* hook data                                                                */

struct vk_obj_node {
    uint64_t obj;
    struct vk_obj_node *next;
};

struct vk_obj_list {
    struct vk_obj_node *root;
    pthread_mutex_t mutex;
};

struct vk_swap_data {
    struct vk_obj_node node;

    VkExtent2D image_extent;
    VkFormat format;
    VkImage export_image;
    VkDeviceMemory export_mem;
    VkSubresourceLayout export_layout;
    VkImage *swap_images;
    uint32_t image_count;

    int dmabuf_fd;
    bool captured;
};

struct vk_queue_data {
    struct vk_obj_node node;

    uint32_t fam_idx;
    bool supports_transfer;
    struct vk_frame_data *frames;
    uint32_t frame_index;
    uint32_t frame_count;
};

struct vk_frame_data {
    VkCommandPool cmd_pool;
    VkCommandBuffer cmd_buffer;
    VkFence fence;
    bool cmd_buffer_busy;
};

struct vk_inst_data {
    struct vk_obj_node node;

    VkInstance instance;

    bool valid;

    struct vk_inst_funcs funcs;
};

struct vk_data {
    struct vk_obj_node node;

    VkDevice device;

    bool valid;

    struct vk_device_funcs funcs;
    VkPhysicalDevice phy_device;
    struct vk_obj_list swaps;
    struct vk_swap_data *cur_swap;

    struct vk_obj_list queues;

    VkExternalMemoryProperties external_mem_props;

    struct vk_inst_data *inst_data;

    VkAllocationCallbacks ac_storage;
    const VkAllocationCallbacks *ac;
};

// ------------------------------

struct {
    int connfd;
    bool capturing;
} capture_data;

static void capture_init()
{
    capture_data.connfd = -1;
    capture_data.capturing = false;
}

static bool capture_try_connect()
{
    const char *sockname = "/tmp/obs-vkcapture.sock";

    struct sockaddr_un addr;
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, sockname);
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    int ret = connect(sock, (const struct sockaddr *)&addr, sizeof(addr));
    if (ret == -1) {
        close(sock);
        return false;
    }

    os_socket_block(sock, false);
    capture_data.connfd = sock;
    return true;
}

static void capture_update_socket()
{
    static int limiter = 0;
    if (++limiter < 60) {
        return;
    }
    limiter = 0;

    if (capture_data.connfd < 0 && !capture_try_connect()) {
        return;
    }

    char buf[1];
    ssize_t n = recv(capture_data.connfd, buf, 1, 0);
    if (n == -1) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;
        }
        if (errno != ECONNRESET) {
            hlog("Socket recv error: %s", strerror(errno));
        }
    }
    if (n <= 0) {
        close(capture_data.connfd);
        capture_data.connfd = -1;
    }
}

static void capture_init_shtex(struct vk_swap_data *swap)
{
    struct msg_texture_data {
        int width;
        int height;
        int fourcc;
        int stride;
        int offset;
        uint64_t modifiers;
    };

    struct msg_texture_data td;
    td.width = swap->image_extent.width;
    td.height = swap->image_extent.height;
    td.fourcc = 0;
    td.stride = swap->export_layout.rowPitch;
    td.offset = swap->export_layout.offset;
    td.modifiers = 0;

    struct msghdr msg = {0};

    struct iovec io = {
        .iov_base = &td,
        .iov_len = sizeof(struct msg_texture_data),
    };
    msg.msg_iov = &io;
    msg.msg_iovlen = 1;

    char cmsg_buf[CMSG_SPACE(sizeof(int))];
    msg.msg_control = cmsg_buf;
    msg.msg_controllen = CMSG_SPACE(sizeof(int));
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &swap->dmabuf_fd, sizeof(int));

    const ssize_t sent = sendmsg(capture_data.connfd, &msg, 0);

    if (sent < 0) {
        perror("cannot sendmsg");
    }

    swap->captured = true;
    capture_data.capturing = true;
}

static bool capture_should_stop()
{
    return capture_data.capturing && capture_data.connfd < 0;
}

static bool capture_should_init()
{
    return !capture_data.capturing && capture_data.connfd >= 0;
}

static bool capture_ready()
{
    return capture_data.capturing;
}

/* ------------------------------------------------------------------------- */

static void *vk_alloc(const VkAllocationCallbacks *ac, size_t size,
        size_t alignment, enum VkSystemAllocationScope scope)
{
    return ac ? ac->pfnAllocation(ac->pUserData, size, alignment, scope)
        : aligned_alloc(alignment, size);
}

static void vk_free(const VkAllocationCallbacks *ac, void *memory)
{
    if (ac)
        ac->pfnFree(ac->pUserData, memory);
    else
        free(memory);
}

static void add_obj_data(struct vk_obj_list *list, uint64_t obj, void *data)
{
    pthread_mutex_lock(&list->mutex);

    struct vk_obj_node *const node = (struct vk_obj_node*)data;
    node->obj = obj;
    node->next = list->root;
    list->root = node;

    pthread_mutex_unlock(&list->mutex);
}

static struct vk_obj_node *get_obj_data(struct vk_obj_list *list, uint64_t obj)
{
    struct vk_obj_node *data = NULL;

    pthread_mutex_lock(&list->mutex);

    struct vk_obj_node *node = list->root;
    while (node) {
        if (node->obj == obj) {
            data = node;
            break;
        }

        node = node->next;
    }

    pthread_mutex_unlock(&list->mutex);

    return data;
}

static struct vk_obj_node *remove_obj_data(struct vk_obj_list *list,
        uint64_t obj)
{
    struct vk_obj_node *data = NULL;

    pthread_mutex_lock(&list->mutex);

    struct vk_obj_node *prev = NULL;
    struct vk_obj_node *node = list->root;
    while (node) {
        if (node->obj == obj) {
            data = node;
            if (prev)
                prev->next = node->next;
            else
                list->root = node->next;
            break;
        }

        prev = node;
        node = node->next;
    }

    pthread_mutex_unlock(&list->mutex);

    return data;
}

static void init_obj_list(struct vk_obj_list *list)
{
    list->root = NULL;
    pthread_mutex_init(&list->mutex, NULL);
}

static struct vk_obj_node *obj_walk_begin(struct vk_obj_list *list)
{
    pthread_mutex_lock(&list->mutex);
    return list->root;
}

static struct vk_obj_node *obj_walk_next(struct vk_obj_node *node)
{
    return node->next;
}

static void obj_walk_end(struct vk_obj_list *list)
{
    pthread_mutex_unlock(&list->mutex);
}

/* ------------------------------------------------------------------------- */

static struct vk_obj_list devices;

static struct vk_data *alloc_device_data(const VkAllocationCallbacks *ac)
{
    struct vk_data *data = vk_alloc(ac, sizeof(struct vk_data),
            _Alignof(struct vk_data),
            VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
    return data;
}

static void init_device_data(struct vk_data *data, VkDevice device)
{
    add_obj_data(&devices, (uint64_t)GET_LDT(device), data);
    data->device = device;
}

static struct vk_data *get_device_data(VkDevice device)
{
    return (struct vk_data *)get_obj_data(&devices,
            (uint64_t)GET_LDT(device));
}

static struct vk_data *get_device_data_by_queue(VkQueue queue)
{
    return (struct vk_data *)get_obj_data(&devices,
            (uint64_t)GET_LDT(queue));
}

static struct vk_data *remove_device_data(VkDevice device)
{
    return (struct vk_data *)remove_obj_data(&devices,
            (uint64_t)GET_LDT(device));
}

static void free_device_data(struct vk_data *data,
        const VkAllocationCallbacks *ac)
{
    vk_free(ac, data);
}

/* ------------------------------------------------------------------------- */

static struct vk_queue_data *add_queue_data(struct vk_data *data, VkQueue queue,
        uint32_t fam_idx,
        bool supports_transfer,
        const VkAllocationCallbacks *ac)
{
    struct vk_queue_data *const queue_data =
        vk_alloc(ac, sizeof(struct vk_queue_data),
                _Alignof(struct vk_queue_data),
                VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
    add_obj_data(&data->queues, (uint64_t)queue, queue_data);
    queue_data->fam_idx = fam_idx;
    queue_data->supports_transfer = supports_transfer;
    queue_data->frames = NULL;
    queue_data->frame_index = 0;
    queue_data->frame_count = 0;
    return queue_data;
}

static struct vk_queue_data *get_queue_data(struct vk_data *data, VkQueue queue)
{
    return (struct vk_queue_data *)get_obj_data(&data->queues,
            (uint64_t)queue);
}

static VkQueue get_queue_key(const struct vk_queue_data *queue_data)
{
    return (VkQueue)(uintptr_t)queue_data->node.obj;
}

static void remove_free_queue_all(struct vk_data *data,
        const VkAllocationCallbacks *ac)
{
    struct vk_queue_data *queue_data =
        (struct vk_queue_data *)data->queues.root;
    while (data->queues.root) {
        remove_obj_data(&data->queues, queue_data->node.obj);
        vk_free(ac, queue_data);

        queue_data = (struct vk_queue_data *)data->queues.root;
    }
}

static struct vk_queue_data *queue_walk_begin(struct vk_data *data)
{
    return (struct vk_queue_data *)obj_walk_begin(&data->queues);
}

static struct vk_queue_data *queue_walk_next(struct vk_queue_data *queue_data)
{
    return (struct vk_queue_data *)obj_walk_next(
            (struct vk_obj_node *)queue_data);
}

static void queue_walk_end(struct vk_data *data)
{
    obj_walk_end(&data->queues);
}

/* ------------------------------------------------------------------------- */

static struct vk_swap_data *alloc_swap_data(const VkAllocationCallbacks *ac)
{
    struct vk_swap_data *const swap_data = vk_alloc(
            ac, sizeof(struct vk_swap_data), _Alignof(struct vk_swap_data),
            VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
    return swap_data;
}

static void init_swap_data(struct vk_swap_data *swap_data, struct vk_data *data,
        VkSwapchainKHR sc)
{
    add_obj_data(&data->swaps, (uint64_t)sc, swap_data);
}

static struct vk_swap_data *get_swap_data(struct vk_data *data,
        VkSwapchainKHR sc)
{
    return (struct vk_swap_data *)get_obj_data(&data->swaps, (uint64_t)sc);
}

static void remove_free_swap_data(struct vk_data *data, VkSwapchainKHR sc,
        const VkAllocationCallbacks *ac)
{
    struct vk_swap_data *const swap_data =
        (struct vk_swap_data *)remove_obj_data(&data->swaps,
                (uint64_t)sc);
    vk_free(ac, swap_data);
}

static struct vk_swap_data *swap_walk_begin(struct vk_data *data)
{
    return (struct vk_swap_data *)obj_walk_begin(&data->swaps);
}

static struct vk_swap_data *swap_walk_next(struct vk_swap_data *swap_data)
{
    return (struct vk_swap_data *)obj_walk_next(
            (struct vk_obj_node *)swap_data);
}

static void swap_walk_end(struct vk_data *data)
{
    obj_walk_end(&data->swaps);
}

/* ------------------------------------------------------------------------- */

static void vk_shtex_clear_fence(const struct vk_data *data,
        struct vk_frame_data *frame_data)
{
    const VkFence fence = frame_data->fence;
    if (frame_data->cmd_buffer_busy) {
        VkDevice device = data->device;
        const struct vk_device_funcs *funcs = &data->funcs;
        funcs->WaitForFences(device, 1, &fence, VK_TRUE, ~0ull);
        funcs->ResetFences(device, 1, &fence);
        frame_data->cmd_buffer_busy = false;
    }
}

static void vk_shtex_wait_until_pool_idle(struct vk_data *data,
        struct vk_queue_data *queue_data)
{
    return;
    for (uint32_t frame_idx = 0; frame_idx < queue_data->frame_count;
            frame_idx++) {
        struct vk_frame_data *frame_data =
            &queue_data->frames[frame_idx];
        if (frame_data->cmd_pool != VK_NULL_HANDLE)
            vk_shtex_clear_fence(data, frame_data);
    }
}

static void vk_shtex_wait_until_idle(struct vk_data *data)
{
    struct vk_queue_data *queue_data = queue_walk_begin(data);

    while (queue_data) {
        vk_shtex_wait_until_pool_idle(data, queue_data);

        queue_data = queue_walk_next(queue_data);
    }

    queue_walk_end(data);
}

static void vk_shtex_free(struct vk_data *data)
{
    vk_shtex_wait_until_idle(data);

    struct vk_swap_data *swap = swap_walk_begin(data);

    while (swap) {
        VkDevice device = data->device;
        if (swap->export_image)
            data->funcs.DestroyImage(device, swap->export_image,
                    data->ac);

        if (swap->dmabuf_fd >= 0) {
            close(swap->dmabuf_fd);
            swap->dmabuf_fd = -1;
        }

        if (swap->export_mem)
            data->funcs.FreeMemory(device, swap->export_mem, NULL);

        swap->export_mem = VK_NULL_HANDLE;
        swap->export_image = VK_NULL_HANDLE;

        swap->captured = false;

        swap = swap_walk_next(swap);
    }

    swap_walk_end(data);

    data->cur_swap = NULL;
    capture_data.capturing = false;

    hlog("------------------- vulkan capture freed -------------------");
}

/* ------------------------------------------------------------------------- */

static struct vk_obj_list instances;

static struct vk_inst_data *alloc_inst_data(const VkAllocationCallbacks *ac)
{
    struct vk_inst_data *idata = vk_alloc(
            ac, sizeof(struct vk_inst_data), _Alignof(struct vk_inst_data),
            VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
    return idata;
}

static void init_inst_data(struct vk_inst_data *idata, VkInstance instance)
{
    add_obj_data(&instances, (uint64_t)GET_LDT(instance), idata);
    idata->instance = instance;
}

static struct vk_inst_data *get_inst_data(VkInstance instance)
{
    return (struct vk_inst_data *)get_obj_data(&instances,
            (uint64_t)GET_LDT(instance));
}

static struct vk_inst_funcs *get_inst_funcs(VkInstance instance)
{
    struct vk_inst_data *idata =
        (struct vk_inst_data *)get_inst_data(instance);
    return &idata->funcs;
}

static struct vk_inst_data *
get_inst_data_by_physical_device(VkPhysicalDevice physicalDevice)
{
    return (struct vk_inst_data *)get_obj_data(
            &instances, (uint64_t)GET_LDT(physicalDevice));
}

static struct vk_inst_funcs *
get_inst_funcs_by_physical_device(VkPhysicalDevice physicalDevice)
{
    struct vk_inst_data *idata =
        (struct vk_inst_data *)get_inst_data_by_physical_device(
                physicalDevice);
    return &idata->funcs;
}

static void remove_free_inst_data(VkInstance inst,
        const VkAllocationCallbacks *ac)
{
    struct vk_inst_data *idata = (struct vk_inst_data *)remove_obj_data(
            &instances, (uint64_t)GET_LDT(inst));
    vk_free(ac, idata);
}

/* ======================================================================== */
/* capture                                                                  */

static inline bool vk_shtex_init_vulkan_tex(struct vk_data *data,
        struct vk_swap_data *swap)
{
    struct vk_device_funcs *funcs = &data->funcs;

    // create image (for dedicated allocation)
    VkImageCreateInfo img_info = {};
    img_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    img_info.imageType = VK_IMAGE_TYPE_2D;
    img_info.format = swap->format;
    img_info.mipLevels = 1;
    img_info.arrayLayers = 1;
    img_info.samples = VK_SAMPLE_COUNT_1_BIT;
    img_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    img_info.initialLayout = VK_IMAGE_LAYOUT_GENERAL;
    img_info.extent.width = swap->image_extent.width;
    img_info.extent.height = swap->image_extent.height;
    img_info.extent.depth = 1;
    img_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    img_info.tiling = VK_IMAGE_TILING_LINEAR;

    VkDevice device = data->device;

    VkResult res;
    res = funcs->CreateImage(device, &img_info, data->ac, &swap->export_image);
    if (VK_SUCCESS != res) {
        hlog("Failed to CreateImage %d", res);
        swap->export_image = VK_NULL_HANDLE;
        return false;
    }

    VkImageSubresource sbr = {};
    sbr.mipLevel = 0;
    sbr.arrayLayer = 0;
    sbr.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    funcs->GetImageSubresourceLayout(device, swap->export_image, &sbr, &swap->export_layout);

    VkImageMemoryRequirementsInfo2 memri = {};
    memri.image = swap->export_image;
    memri.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2;

    VkMemoryDedicatedRequirements mdr = {};
    mdr.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS;

    VkMemoryRequirements2 memr = {};
    memr.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;
    memr.pNext = &mdr;

    funcs->GetImageMemoryRequirements2(device, &memri, &memr);

    /* -------------------------------------------------------- */
    /* get memory type index                                    */

    struct vk_inst_funcs *ifuncs =
        get_inst_funcs_by_physical_device(data->phy_device);

    VkPhysicalDeviceMemoryProperties pdmp;
    ifuncs->GetPhysicalDeviceMemoryProperties(data->phy_device, &pdmp);

    uint32_t mem_type_idx = 0;
    uint32_t mem_req_bits = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    for (; mem_type_idx < pdmp.memoryTypeCount; mem_type_idx++) {
        if ((memr.memoryRequirements.memoryTypeBits & (1 << mem_type_idx)) &&
                (pdmp.memoryTypes[mem_type_idx].propertyFlags &
                 mem_req_bits) == mem_req_bits) {
            break;
        }
    }

    if (mem_type_idx == pdmp.memoryTypeCount) {
        hlog("failed to get memory type index");
        funcs->DestroyImage(device, swap->export_image, data->ac);
        swap->export_image = VK_NULL_HANDLE;
        return false;
    }

    VkMemoryAllocateInfo memi = {};
    memi.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    memi.allocationSize = memr.memoryRequirements.size;
    memi.memoryTypeIndex = mem_type_idx;

    res = funcs->AllocateMemory(device, &memi, NULL, &swap->export_mem);
    if (VK_SUCCESS != res) {
        hlog("failed to AllocateMemory: %d", res);
        funcs->DestroyImage(device, swap->export_image, data->ac);
        return false;
    }

    VkBindImageMemoryInfo bimi = {};
    bimi.sType = VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO;
    bimi.image = swap->export_image;
    bimi.memory = swap->export_mem;
    bimi.memoryOffset = 0;
    res = funcs->BindImageMemory2(device, 1, &bimi);
    if (VK_SUCCESS != res) {
        hlog("BindImageMemory2 failed %d", res);
        funcs->DestroyImage(device, swap->export_image, data->ac);
        swap->export_image = VK_NULL_HANDLE;
        return false;
    }

    VkMemoryGetFdInfoKHR gfdi = {};
    gfdi.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
    gfdi.memory = swap->export_mem;
    gfdi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    res = funcs->GetMemoryFdKHR(device, &gfdi, &swap->dmabuf_fd);
    if (VK_SUCCESS != res) {
        hlog("GetMemoryFdKHR failed %d", res);
        funcs->DestroyImage(device, swap->export_image, data->ac);
        swap->export_image = VK_NULL_HANDLE;
        return false;
    }

    return true;
}

static bool vk_shtex_init(struct vk_data *data, struct vk_swap_data *swap)
{
    if (!vk_shtex_init_vulkan_tex(data, swap)) {
        return false;
    }

    data->cur_swap = swap;

    capture_init_shtex(swap);

    if (!swap->captured)
        return false;

    hlog("------------------ vulkan capture started ------------------");
    return true;
}

static void vk_shtex_create_frame_objects(struct vk_data *data,
        struct vk_queue_data *queue_data,
        uint32_t image_count)
{
    queue_data->frames =
        vk_alloc(data->ac, image_count * sizeof(struct vk_frame_data),
                _Alignof(struct vk_frame_data),
                VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
    memset(queue_data->frames, 0, image_count * sizeof(struct vk_frame_data));
    queue_data->frame_index = 0;
    queue_data->frame_count = image_count;

    VkDevice device = data->device;
    for (uint32_t image_index = 0; image_index < image_count;
            image_index++) {
        struct vk_frame_data *frame_data =
            &queue_data->frames[image_index];

        VkCommandPoolCreateInfo cpci;
        cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        cpci.pNext = NULL;
        cpci.flags = 0;
        cpci.queueFamilyIndex = queue_data->fam_idx;

        VkResult res = data->funcs.CreateCommandPool(
                device, &cpci, data->ac, &frame_data->cmd_pool);
#ifdef MORE_DEBUGGING
        hlog("CreateCommandPool %d", res);
#endif

        VkCommandBufferAllocateInfo cbai;
        cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cbai.pNext = NULL;
        cbai.commandPool = frame_data->cmd_pool;
        cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbai.commandBufferCount = 1;

        res = data->funcs.AllocateCommandBuffers(
                device, &cbai, &frame_data->cmd_buffer);
#ifdef MORE_DEBUGGING
        hlog("AllocateCommandBuffers %d", res);
#endif
        GET_LDT(frame_data->cmd_buffer) = GET_LDT(device);

        VkFenceCreateInfo fci = {};
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fci.pNext = NULL;
        fci.flags = 0;
        res = data->funcs.CreateFence(device, &fci, data->ac,
               &frame_data->fence);
#ifdef MORE_DEBUGGING
        hlog("CreateFence %d", res);
#endif
    }
}

static void vk_shtex_destroy_fence(struct vk_data *data, bool *cmd_buffer_busy,
        VkFence *fence)
{
    VkDevice device = data->device;

    if (*cmd_buffer_busy) {
        data->funcs.WaitForFences(device, 1, fence, VK_TRUE, ~0ull);
        *cmd_buffer_busy = false;
    }

    data->funcs.DestroyFence(device, *fence, data->ac);
    *fence = VK_NULL_HANDLE;
}

static void vk_shtex_destroy_frame_objects(struct vk_data *data,
        struct vk_queue_data *queue_data)
{
    VkDevice device = data->device;

    for (uint32_t frame_idx = 0; frame_idx < queue_data->frame_count;
            frame_idx++) {
        struct vk_frame_data *frame_data =
            &queue_data->frames[frame_idx];
        bool *cmd_buffer_busy = &frame_data->cmd_buffer_busy;
        VkFence *fence = &frame_data->fence;
        vk_shtex_destroy_fence(data, cmd_buffer_busy, fence);

        data->funcs.DestroyCommandPool(device, frame_data->cmd_pool,
                data->ac);
        frame_data->cmd_pool = VK_NULL_HANDLE;
    }

    vk_free(data->ac, queue_data->frames);
    queue_data->frames = NULL;
    queue_data->frame_count = 0;
}

static void vk_shtex_capture(struct vk_data *data,
        struct vk_device_funcs *funcs,
        struct vk_swap_data *swap, uint32_t idx,
        VkQueue queue, const VkPresentInfoKHR *info)
{
    VkResult res = VK_SUCCESS;

    VkCommandBufferBeginInfo begin_info;
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.pNext = NULL;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    begin_info.pInheritanceInfo = NULL;

    VkImageMemoryBarrier mb[2];
    VkImageMemoryBarrier *src_mb = &mb[0];
    VkImageMemoryBarrier *dst_mb = &mb[1];

    /* ------------------------------------------------------ */
    /* do image copy                                          */

    const uint32_t image_index = info->pImageIndices[idx];
    VkImage cur_backbuffer = swap->swap_images[image_index];

    struct vk_queue_data *queue_data = get_queue_data(data, queue);
    uint32_t fam_idx = queue_data->fam_idx;

    const uint32_t image_count = swap->image_count;
    if (queue_data->frame_count < image_count) {
        if (queue_data->frame_count > 0)
            vk_shtex_destroy_frame_objects(data, queue_data);
        vk_shtex_create_frame_objects(data, queue_data, image_count);
    }

    const uint32_t frame_index = queue_data->frame_index;
    struct vk_frame_data *frame_data = &queue_data->frames[frame_index];
    queue_data->frame_index = (frame_index + 1) % queue_data->frame_count;
    vk_shtex_clear_fence(data, frame_data);

    VkDevice device = data->device;

    res = funcs->ResetCommandPool(device, frame_data->cmd_pool, 0);

#ifdef MORE_DEBUGGING
    hlog("ResetCommandPool %d", res);
#endif

    const VkCommandBuffer cmd_buffer = frame_data->cmd_buffer;
    res = funcs->BeginCommandBuffer(cmd_buffer, &begin_info);

#ifdef MORE_DEBUGGING
    hlog("BeginCommandBuffer %d", res);
#endif

    /* ------------------------------------------------------ */
    /* transition cur_backbuffer to transfer source state     */

    src_mb->sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    src_mb->pNext = NULL;
    src_mb->srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    src_mb->dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    src_mb->oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    src_mb->newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    src_mb->srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    src_mb->dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    src_mb->image = cur_backbuffer;
    src_mb->subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    src_mb->subresourceRange.baseMipLevel = 0;
    src_mb->subresourceRange.levelCount = 1;
    src_mb->subresourceRange.baseArrayLayer = 0;
    src_mb->subresourceRange.layerCount = 1;

    /* ------------------------------------------------------ */
    /* transition exportedTexture to transfer dest state      */

    dst_mb->sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    dst_mb->pNext = NULL;
    dst_mb->srcAccessMask = 0;
    dst_mb->dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    dst_mb->oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    dst_mb->newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    dst_mb->srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    dst_mb->dstQueueFamilyIndex = fam_idx;
    dst_mb->image = swap->export_image;
    dst_mb->subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    dst_mb->subresourceRange.baseMipLevel = 0;
    dst_mb->subresourceRange.levelCount = 1;
    dst_mb->subresourceRange.baseArrayLayer = 0;
    dst_mb->subresourceRange.layerCount = 1;

    funcs->CmdPipelineBarrier(cmd_buffer,
            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0,
            NULL, 2, mb);

    /* ------------------------------------------------------ */
    /* copy cur_backbuffer's content to our interop image     */

    VkImageCopy cpy;
    cpy.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    cpy.srcSubresource.mipLevel = 0;
    cpy.srcSubresource.baseArrayLayer = 0;
    cpy.srcSubresource.layerCount = 1;
    cpy.srcOffset.x = 0;
    cpy.srcOffset.y = 0;
    cpy.srcOffset.z = 0;
    cpy.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    cpy.dstSubresource.mipLevel = 0;
    cpy.dstSubresource.baseArrayLayer = 0;
    cpy.dstSubresource.layerCount = 1;
    cpy.dstOffset.x = 0;
    cpy.dstOffset.y = 0;
    cpy.dstOffset.z = 0;
    cpy.extent.width = swap->image_extent.width;
    cpy.extent.height = swap->image_extent.height;
    cpy.extent.depth = 1;
    funcs->CmdCopyImage(cmd_buffer, cur_backbuffer,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            swap->export_image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cpy);

    /* ------------------------------------------------------ */
    /* Restore the swap chain image layout to what it was
     * before.  This may not be strictly needed, but it is
     * generally good to restore things to their original
     * state.  */

    src_mb->srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    src_mb->dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    src_mb->oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    src_mb->newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    dst_mb->srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    dst_mb->dstAccessMask = 0;
    dst_mb->oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    dst_mb->newLayout = VK_IMAGE_LAYOUT_GENERAL;
    dst_mb->srcQueueFamilyIndex = fam_idx;
    dst_mb->dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;

    funcs->CmdPipelineBarrier(cmd_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT |
            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
            0, 0, NULL, 0, NULL, 2, mb);

    funcs->EndCommandBuffer(cmd_buffer);

    /* ------------------------------------------------------ */

    VkSubmitInfo submit_info;
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.pNext = NULL;
    submit_info.waitSemaphoreCount = 0;
    submit_info.pWaitSemaphores = NULL;
    submit_info.pWaitDstStageMask = NULL;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &cmd_buffer;
    submit_info.signalSemaphoreCount = 0;
    submit_info.pSignalSemaphores = NULL;

    const VkFence fence = frame_data->fence;
    res = funcs->QueueSubmit(queue, 1, &submit_info, fence);

#ifdef MORE_DEBUGGING
    hlog("QueueSubmit %d", res);
#endif

    if (res == VK_SUCCESS)
        frame_data->cmd_buffer_busy = true;
}

static inline bool valid_rect(struct vk_swap_data *swap)
{
    return !!swap->image_extent.width && !!swap->image_extent.height;
}

static void vk_capture(struct vk_data *data, VkQueue queue,
        const VkPresentInfoKHR *info)
{
    // Use first swapchain ??
    struct vk_swap_data *swap = get_swap_data(data, info->pSwapchains[0]);

    capture_update_socket();

    if (capture_should_stop()) {
        vk_shtex_free(data);
    }

    if (capture_should_init()) {
        if (valid_rect(swap) && !vk_shtex_init(data, swap)) {
            vk_shtex_free(data);
            data->valid = false;
            hlog("vk_shtex_init failed");
        }
    }

    if (capture_ready()) {
        if (swap != data->cur_swap) {
            vk_shtex_free(data);
            return;
        }

        vk_shtex_capture(data, &data->funcs, swap, 0, queue, info);
    }
}

static VkResult VKAPI_CALL OBS_QueuePresentKHR(VkQueue queue,
        const VkPresentInfoKHR *info)
{
    struct vk_data *const data = get_device_data_by_queue(queue);
    struct vk_queue_data *const queue_data = get_queue_data(data, queue);
    struct vk_device_funcs *const funcs = &data->funcs;

    if (data->valid && queue_data->supports_transfer) {
        vk_capture(data, queue, info);
    }

    return funcs->QueuePresentKHR(queue, info);
}

/* ======================================================================== */
/* setup hooks                                                              */

static inline bool is_inst_link_info(VkLayerInstanceCreateInfo *lici)
{
    return lici->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO &&
        lici->function == VK_LAYER_LINK_INFO;
}

static VkResult VKAPI_CALL OBS_CreateInstance(const VkInstanceCreateInfo *cinfo,
        const VkAllocationCallbacks *ac,
        VkInstance *p_inst)
{
    VkInstanceCreateInfo info = *cinfo;

    /* -------------------------------------------------------- */
    /* step through chain until we get to the link info         */

    VkLayerInstanceCreateInfo *lici = (VkLayerInstanceCreateInfo *)info.pNext;
    while (lici && !is_inst_link_info(lici)) {
        lici = (VkLayerInstanceCreateInfo *)lici->pNext;
    }

    if (lici == NULL) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    PFN_vkGetInstanceProcAddr gpa =
        lici->u.pLayerInfo->pfnNextGetInstanceProcAddr;

    /* -------------------------------------------------------- */
    /* move chain on for next layer                             */

    lici->u.pLayerInfo = lici->u.pLayerInfo->pNext;

    /* -------------------------------------------------------- */
    /* (HACK) Set api version to 1.2 if set to 1.0              */
    /* We do this to get our extensions working properly        */

    VkApplicationInfo ai;
    if (info.pApplicationInfo) {
        ai = *info.pApplicationInfo;
        if (ai.apiVersion < VK_API_VERSION_1_2)
            ai.apiVersion = VK_API_VERSION_1_2;
    } else {
        ai.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        ai.pNext = NULL;
        ai.pApplicationName = NULL;
        ai.applicationVersion = 0;
        ai.pEngineName = NULL;
        ai.engineVersion = 0;
        ai.apiVersion = VK_API_VERSION_1_2;
    }

    info.pApplicationInfo = &ai;

    /* -------------------------------------------------------- */
    /* allocate data node                                       */

    struct vk_inst_data *idata = alloc_inst_data(ac);
    if (!idata)
        return VK_ERROR_OUT_OF_HOST_MEMORY;

    /* -------------------------------------------------------- */
    /* create instance                                          */

    PFN_vkCreateInstance create = (PFN_vkCreateInstance)gpa(NULL, "vkCreateInstance");

    VkResult res = create(&info, ac, p_inst);
    bool valid = res == VK_SUCCESS;
    if (!valid) {
        /* try again with original arguments */
        res = create(cinfo, ac, p_inst);
        if (res != VK_SUCCESS) {
            vk_free(ac, idata);
            return res;
        }
    }

    VkInstance inst = *p_inst;
    init_inst_data(idata, inst);

    /* -------------------------------------------------------- */
    /* fetch the functions we need                              */

    struct vk_inst_funcs *ifuncs = &idata->funcs;

#define GETADDR(x)                                      \
    do {                                            \
        ifuncs->x = (PFN_vk##x)gpa(inst, "vk" #x); \
        if (!ifuncs->x) {                       \
            hlog("could not get instance "  \
                    "address for vk" #x);      \
            funcs_found = false;            \
        }                                       \
    } while (false)

    bool funcs_found = true;
    GETADDR(GetInstanceProcAddr);
    GETADDR(DestroyInstance);
    GETADDR(GetPhysicalDeviceQueueFamilyProperties);
    GETADDR(GetPhysicalDeviceMemoryProperties);
#undef GETADDR

    valid = valid && funcs_found;
    idata->valid = valid;

    return res;
}

static void VKAPI_CALL OBS_DestroyInstance(VkInstance instance,
        const VkAllocationCallbacks *ac)
{
    struct vk_inst_funcs *ifuncs = get_inst_funcs(instance);
    PFN_vkDestroyInstance destroy_instance = ifuncs->DestroyInstance;

    remove_free_inst_data(instance, ac);

    destroy_instance(instance, ac);
}

static inline bool is_device_link_info(VkLayerDeviceCreateInfo *lici)
{
    return lici->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO &&
        lici->function == VK_LAYER_LINK_INFO;
}

static VkResult VKAPI_CALL OBS_CreateDevice(VkPhysicalDevice phy_device,
        const VkDeviceCreateInfo *info,
        const VkAllocationCallbacks *ac,
        VkDevice *p_device)
{
    struct vk_inst_data *idata =
        get_inst_data_by_physical_device(phy_device);
    struct vk_inst_funcs *ifuncs = &idata->funcs;
    struct vk_data *data = NULL;

    bool add_ext = true;
    for (uint32_t i = 0; i < info->enabledExtensionCount; ++i) {
        if (!strcmp(info->ppEnabledExtensionNames[i], VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME)) {
            add_ext = false;
        }
    }
    if (add_ext) {
        hlog("Injecting %s extension", VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
        int new_count = info->enabledExtensionCount + 1;
        const char **exts = (const char**)malloc(sizeof(char*) * new_count);
        memcpy(exts, info->ppEnabledExtensionNames, sizeof(char*) * info->enabledExtensionCount);
        exts[new_count - 1] = VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME;
        VkDeviceCreateInfo *i = (VkDeviceCreateInfo*)info;
        i->enabledExtensionCount = new_count;
        i->ppEnabledExtensionNames = exts;
    }

    VkResult ret = VK_ERROR_INITIALIZATION_FAILED;

    VkLayerDeviceCreateInfo *ldci = (VkLayerDeviceCreateInfo*)info->pNext;

    /* -------------------------------------------------------- */
    /* step through chain until we get to the link info         */

    while (ldci && !is_device_link_info(ldci)) {
        ldci = (VkLayerDeviceCreateInfo *)ldci->pNext;
    }

    if (!ldci) {
        return ret;
    }

    PFN_vkGetInstanceProcAddr gipa;
    PFN_vkGetDeviceProcAddr gdpa;

    gipa = ldci->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    gdpa = ldci->u.pLayerInfo->pfnNextGetDeviceProcAddr;

    /* -------------------------------------------------------- */
    /* move chain on for next layer                             */

    ldci->u.pLayerInfo = ldci->u.pLayerInfo->pNext;

    /* -------------------------------------------------------- */
    /* allocate data node                                       */

    data = alloc_device_data(ac);
    if (!data)
        return VK_ERROR_OUT_OF_HOST_MEMORY;

    init_obj_list(&data->queues);

    /* -------------------------------------------------------- */
    /* create device and initialize hook data                   */

    PFN_vkCreateDevice createFunc =
        (PFN_vkCreateDevice)gipa(idata->instance, "vkCreateDevice");

    ret = createFunc(phy_device, info, ac, p_device);
    if (ret != VK_SUCCESS) {
        vk_free(ac, data);
        return ret;
    }

    VkDevice device = *p_device;
    init_device_data(data, device);

    data->valid = false; /* set true below if it doesn't go to fail */
    data->phy_device = phy_device;

    /* -------------------------------------------------------- */
    /* fetch the functions we need                              */

    struct vk_device_funcs *dfuncs = &data->funcs;
    bool funcs_found = true;

#define GETADDR(x)                                         \
    do {                                               \
        dfuncs->x = (PFN_vk##x)gdpa(device, "vk" #x); \
        if (!dfuncs->x) {                          \
            hlog("could not get device "       \
                    "address for vk" #x);         \
            funcs_found = false;               \
        }                                          \
    } while (false)

    GETADDR(GetDeviceProcAddr);
    GETADDR(DestroyDevice);
    GETADDR(CreateSwapchainKHR);
    GETADDR(DestroySwapchainKHR);
    GETADDR(QueuePresentKHR);
    GETADDR(AllocateMemory);
    GETADDR(FreeMemory);
    GETADDR(BindImageMemory2);
    GETADDR(GetSwapchainImagesKHR);
    GETADDR(CreateImage);
    GETADDR(DestroyImage);
    GETADDR(GetImageMemoryRequirements2);
    GETADDR(ResetCommandPool);
    GETADDR(BeginCommandBuffer);
    GETADDR(EndCommandBuffer);
    GETADDR(CmdCopyImage);
    GETADDR(CmdPipelineBarrier);
    GETADDR(GetDeviceQueue);
    GETADDR(QueueSubmit);
    GETADDR(CreateCommandPool);
    GETADDR(DestroyCommandPool);
    GETADDR(AllocateCommandBuffers);
    GETADDR(CreateFence);
    GETADDR(DestroyFence);
    GETADDR(WaitForFences);
    GETADDR(ResetFences);
    GETADDR(GetImageSubresourceLayout);
    GETADDR(GetMemoryFdKHR);

#undef GETADDR

    if (!funcs_found) {
        return ret;
    }

    if (!idata->valid) {
        hlog("instance not valid");
        return ret;
    }

    data->inst_data = idata;

    data->ac = NULL;
    if (ac) {
        data->ac_storage = *ac;
        data->ac = &data->ac_storage;
    }

    uint32_t queue_family_property_count = 0;
    ifuncs->GetPhysicalDeviceQueueFamilyProperties(
            phy_device, &queue_family_property_count, NULL);
    VkQueueFamilyProperties *queue_family_properties = (VkQueueFamilyProperties*)malloc(
            sizeof(VkQueueFamilyProperties) * queue_family_property_count);
    ifuncs->GetPhysicalDeviceQueueFamilyProperties(
            phy_device, &queue_family_property_count,
            queue_family_properties);

    for (uint32_t info_index = 0, info_count = info->queueCreateInfoCount;
            info_index < info_count; ++info_index) {
        const VkDeviceQueueCreateInfo *queue_info =
            &info->pQueueCreateInfos[info_index];
        for (uint32_t queue_index = 0,
                queue_count = queue_info->queueCount;
                queue_index < queue_count; ++queue_index) {
            const uint32_t family_index =
                queue_info->queueFamilyIndex;
            VkQueue queue;
            data->funcs.GetDeviceQueue(device, family_index,
                    queue_index, &queue);
            const bool supports_transfer =
                (queue_family_properties[family_index]
                 .queueFlags &
                 (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT |
                  VK_QUEUE_TRANSFER_BIT)) != 0;
            add_queue_data(data, queue, family_index,
                    supports_transfer, ac);
        }
    }

    free(queue_family_properties);

    init_obj_list(&data->swaps);
    data->cur_swap = NULL;

    data->valid = true;

    return ret;
}

static void VKAPI_CALL OBS_DestroyDevice(VkDevice device,
        const VkAllocationCallbacks *ac)
{
    struct vk_data *data = remove_device_data(device);

    if (data->valid) {
        struct vk_queue_data *queue_data = queue_walk_begin(data);

        while (queue_data) {
            vk_shtex_destroy_frame_objects(data, queue_data);

            queue_data = queue_walk_next(queue_data);
        }

        queue_walk_end(data);

        remove_free_queue_all(data, ac);
    }

    PFN_vkDestroyDevice destroy_device = data->funcs.DestroyDevice;

    vk_free(ac, data);

    destroy_device(device, ac);
}

static VkResult VKAPI_CALL
OBS_CreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR *cinfo,
        const VkAllocationCallbacks *ac, VkSwapchainKHR *p_sc)
{
    struct vk_data *data = get_device_data(device);
    struct vk_device_funcs *funcs = &data->funcs;
    if (!data->valid)
        return funcs->CreateSwapchainKHR(device, cinfo, ac, p_sc);

    VkSwapchainCreateInfoKHR info = *cinfo;
    info.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VkResult res = funcs->CreateSwapchainKHR(device, &info, ac, p_sc);
#ifdef MORE_DEBUGGING
    hlog("CreateSwapchainKHR %d", res);
#endif
    if (res != VK_SUCCESS) {
        /* try again with original imageUsage flags */
        return funcs->CreateSwapchainKHR(device, cinfo, ac, p_sc);
    }

    VkSwapchainKHR sc = *p_sc;
    uint32_t count = 0;
    res = funcs->GetSwapchainImagesKHR(device, sc, &count, NULL);
#ifdef MORE_DEBUGGING
    hlog("GetSwapchainImagesKHR %d", res);
#endif
    if ((res == VK_SUCCESS) && (count > 0)) {
        struct vk_swap_data *swap_data = alloc_swap_data(ac);
        if (swap_data) {
            init_swap_data(swap_data, data, sc);
            swap_data->swap_images = vk_alloc(
                    ac, count * sizeof(VkImage), _Alignof(VkImage),
                    VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
            res = funcs->GetSwapchainImagesKHR(
                    device, sc, &count, swap_data->swap_images);
#ifdef MORE_DEBUGGING
            hlog("GetSwapchainImagesKHR %d", res);
#endif
            swap_data->image_extent = cinfo->imageExtent;
            swap_data->format = cinfo->imageFormat;
            swap_data->export_image = VK_NULL_HANDLE;
            swap_data->export_mem = VK_NULL_HANDLE;
            swap_data->image_count = count;
            swap_data->dmabuf_fd = -1;
            swap_data->captured = false;
        }
    }

    return VK_SUCCESS;
}

static void VKAPI_CALL OBS_DestroySwapchainKHR(VkDevice device,
        VkSwapchainKHR sc,
        const VkAllocationCallbacks *ac)
{
    struct vk_data *data = get_device_data(device);
    struct vk_device_funcs *funcs = &data->funcs;
    PFN_vkDestroySwapchainKHR destroy_swapchain =
        funcs->DestroySwapchainKHR;

    if ((sc != VK_NULL_HANDLE) && data->valid) {
        struct vk_swap_data *swap = get_swap_data(data, sc);
        if (swap) {
            if (data->cur_swap == swap) {
                vk_shtex_free(data);
            }

            vk_free(ac, swap->swap_images);

            remove_free_swap_data(data, sc, ac);
        }
    }

    destroy_swapchain(device, sc, ac);
}

#define GETPROCADDR(func)               \
    if (!strcmp(pName, "vk" #func)) \
    return (PFN_vkVoidFunction)&OBS_##func;

#define GETPROCADDR_IF_SUPPORTED(func)  \
    if (!strcmp(pName, "vk" #func)) \
    return funcs->func ? (PFN_vkVoidFunction)&OBS_##func : NULL;

static PFN_vkVoidFunction VKAPI_CALL OBS_GetDeviceProcAddr(VkDevice device, const char *pName)
{
    struct vk_data *data = get_device_data(device);
    struct vk_device_funcs *funcs = &data->funcs;

    GETPROCADDR(GetDeviceProcAddr);
    GETPROCADDR(DestroyDevice);
    GETPROCADDR_IF_SUPPORTED(CreateSwapchainKHR);
    GETPROCADDR_IF_SUPPORTED(DestroySwapchainKHR);
    GETPROCADDR_IF_SUPPORTED(QueuePresentKHR);

    if (funcs->GetDeviceProcAddr == NULL)
        return NULL;
    return funcs->GetDeviceProcAddr(device, pName);
}

/* bad layers require spec violation */
#define RETURN_FP_FOR_NULL_INSTANCE 1

static PFN_vkVoidFunction VKAPI_CALL OBS_GetInstanceProcAddr(VkInstance instance, const char *pName)
{
    /* instance chain functions we intercept */
    GETPROCADDR(GetInstanceProcAddr);
    GETPROCADDR(CreateInstance);

#if RETURN_FP_FOR_NULL_INSTANCE
    /* other instance chain functions we intercept */
    GETPROCADDR(DestroyInstance);

    /* device chain functions we intercept */
    GETPROCADDR(GetDeviceProcAddr);
    GETPROCADDR(CreateDevice);
    GETPROCADDR(DestroyDevice);

    if (instance == NULL)
        return NULL;

    struct vk_inst_funcs *const funcs = get_inst_funcs(instance);
#else
    if (instance == NULL)
        return NULL;

    struct vk_inst_funcs *const funcs = get_inst_funcs(instance);

    /* other instance chain functions we intercept */
    GETPROCADDR(DestroyInstance);

    /* device chain functions we intercept */
    GETPROCADDR(GetDeviceProcAddr);
    GETPROCADDR(CreateDevice);
    GETPROCADDR(DestroyDevice);
#endif

    const PFN_vkGetInstanceProcAddr gipa = funcs->GetInstanceProcAddr;
    return gipa ? gipa(instance, pName) : NULL;
}

#undef GETPROCADDR

VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL OBS_Negotiate(VkNegotiateLayerInterface *nli)
{
    if (nli->loaderLayerInterfaceVersion >= 2) {
        nli->sType = LAYER_NEGOTIATE_INTERFACE_STRUCT;
        nli->pNext = NULL;
        nli->pfnGetInstanceProcAddr = OBS_GetInstanceProcAddr;
        nli->pfnGetDeviceProcAddr = OBS_GetDeviceProcAddr;
        nli->pfnGetPhysicalDeviceProcAddr = NULL;
    }

    const uint32_t cur_ver = CURRENT_LOADER_LAYER_INTERFACE_VERSION;

    if (nli->loaderLayerInterfaceVersion > cur_ver) {
        nli->loaderLayerInterfaceVersion = cur_ver;
    }

    if (!vulkan_seen) {
        hlog("Init");
        init_obj_list(&instances);
        init_obj_list(&devices);
        capture_init();

        vulkan_seen = true;
    }

    return VK_SUCCESS;
}