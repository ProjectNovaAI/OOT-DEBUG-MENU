// The debug menu's View overlays drawn into the scene in 3D (dbg3d.h). vk_dlaa.c evaluate calls vk_dbg3d_draw after the
// motion pass, which has written each pixel's view distance (metres along the view, 0 where the sky shows): the shapes
// are drawn into the light target the upscaler then reads, so they meet the scene per pixel and the upscaler smooths
// them with it. The scene's depth is not used: its convention varies by pipeline. Instead each fragment compares its own
// view distance (clip w) with the scene's at its pixel, with some slack, and a depth buffer of the pass's own, whose
// depth is 1 - NEAR_W / w (dbg3d_common.glsl out_pos), orders the shapes among themselves.
//
// The camera is the one the scene's draws used this frame (vk_debug_cam.c vk_dbgcam_vp), the viewport theirs: the y
// flip and the upscaler's jitter. Three kinds of draw:
//   the collision mesh (Show collision): a depth pass, then the colour pass at LEQUAL, so a see-through mesh shows only
//     its nearest faces and the edges of what lies behind a face are hidden. Uploaded once an area (debug_hook.c fills
//     a staging buffer on its own thread, vk_dbg3d_mesh_alloc), drawn whole: the shader drops what is beyond the
//     Collision distance from the player. Its corners are 8 bytes, in steps from an origin (dbg3d.h Dbg3dMeshVert),
//     and it goes to the GPU only with room to spare in the game's video memory (mesh_room): the game runs close to
//     its budget, and a 229 MB mesh of Kakariko once pushed its textures out into system memory, where the GPU reads
//     them slowly (80 fps down to 17, for as long as the game ran). Without room, or when the room runs out while it
//     is shown, the mesh is left off and the menu draws the nearest polygons through vk_dbg3d_submit instead.
//   the menu's triangles and lines (vk_dbg3d_submit): blended, tested against the mesh's depth and the scene's
//     distances while Only what you can see is on, not written, so they never hide one another.
// Settings read here: NX2NAVI_DEBUG_VIEW_COLLISION, _HIDE_MESHES, _VIEW_OCCLUDE, _VIEW_OPACITY, _VIEW_STYLE and
// _COLLISION_RANGE.
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "debug_tools.h"
#include "host.h"
#include "platform/platform.h"
#include "vk.h"
#include "vk_debug_cam.h"
#include "vk_rec.h"
#include "dbg3d.h"
#include "dbg3d_mesh_vs_spv.h"
#include "dbg3d_mesh_fs_spv.h"
#include "dbg3d_tri_vs_spv.h"
#include "dbg3d_tri_fs_spv.h"
#include "dbg3d_line_vs_spv.h"
#include "dbg3d_line_fs_spv.h"

#define READY_NS 250000000ull     // the path counts as running this long after it last ran
#define SHAPES_NS 250000000ull    // the menu's shapes are drawn this long after they came
#define FORMATS 4                 // light target formats with pipelines made
#define ROOM_UPLOAD_MB 384        // the mesh goes to the GPU only with this much of the video memory budget left after it
#define ROOM_KEEP_MB 128          // and is taken off it again when less than this is left
#define ROOM_CHECK_NS 1000000000ull

typedef struct {
    float vp[16];
    float eye[4], player[4], test[4], view[4];
} Push;   // dbg3d_common.glsl Push: 128 bytes

typedef struct {
    VkFormat fmt;
    VkPipeline mesh_depth, mesh, tri, line;
    VkPipeline tri_z;   // the triangles with a depth of their own (the clips' second view: opaque faces)
} Pipes;

static struct {
    int tried, ok;
    PFN_vkCmdPushDescriptorSetKHR push;
    VkDescriptorSetLayout dsl;
    VkPipelineLayout layout;
    Pipes pipes[FORMATS];
    int npipes;
    VkImage depth;   // the pass's own depth (D32F), the light target's size
    VmaAllocation depth_mem;
    VkImageView depth_view;
    uint32_t dw, dh;
    VkBuffer mesh;   // the collision mesh on the GPU
    VmaAllocation mesh_mem;
    uint32_t mesh_verts;
    float mesh_origin[3], mesh_scale;
    uint64_t room_ns;   // when the room was last checked
} G;

static volatile uint64_t g_ran_ns;   // when vk_dbg3d_draw last ran
// G's layout and pipelines are made by the first of two threads to need them: the replay's (vk_dbg3d_draw) and the
// present's (vk_dbg3d_view2)
static PlatMutex plock = PLAT_MUTEX_INIT;

// handed over from other threads
static PlatMutex lock = PLAT_MUTEX_INIT;
static struct {
    VkBuffer buf;   // the staging buffer being filled (filling) or filled (ready)
    VmaAllocation mem;
    void *map;
    uint32_t verts;
    float origin[3], scale;
    int filling, ready, drop;
} MS;
static volatile uint32_t g_mesh_tris, g_no_room_mb;
static struct {
    Dbg3dVert *tri;
    Dbg3dLine *line;
    int ntri, nline, cap_tri, cap_line, occlude;
    uint64_t ns;
} SH;

int vk_dbg3d_ready(void) {
    uint64_t t = g_ran_ns;
    return t && plat_now_ns() - t < READY_NS;
}

uint32_t vk_dbg3d_mesh_triangles(void) { return g_mesh_tris; }
uint32_t vk_dbg3d_mesh_no_room_mb(void) { return g_no_room_mb; }

Dbg3dMeshVert *vk_dbg3d_mesh_alloc(uint32_t verts) {
    if (!verts) return NULL;
    const VkCtx *c = vk_ctx();
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = (VkDeviceSize)verts * sizeof(Dbg3dMeshVert),
                             .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                  .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
    VkBuffer buf;
    VmaAllocation mem;
    VmaAllocationInfo info;
    if (vmaCreateBuffer(c->vma, &bi, &ai, &buf, &mem, &info) != VK_SUCCESS) {
        rt_log("dbg3d: no room for the collision mesh (%u corners)", verts);
        return NULL;
    }
    plat_mutex_lock(&lock);
    if (MS.buf) vmaDestroyBuffer(c->vma, MS.buf, MS.mem);   // one no frame took yet: never used by the GPU
    MS.buf = buf, MS.mem = mem, MS.map = info.pMappedData, MS.verts = verts, MS.filling = 1, MS.ready = 0;
    plat_mutex_unlock(&lock);
    return info.pMappedData;
}

void vk_dbg3d_mesh_publish(const float origin[3], float scale) {
    plat_mutex_lock(&lock);
    if (MS.filling) {
        vmaFlushAllocation(vk_ctx()->vma, MS.mem, 0, VK_WHOLE_SIZE);
        memcpy(MS.origin, origin, sizeof MS.origin), MS.scale = scale;
        MS.filling = 0, MS.ready = 1, MS.drop = 0;
    }
    plat_mutex_unlock(&lock);
}

void vk_dbg3d_mesh_drop(void) {
    plat_mutex_lock(&lock);
    if (MS.buf) vmaDestroyBuffer(vk_ctx()->vma, MS.buf, MS.mem);
    MS.buf = VK_NULL_HANDLE, MS.filling = MS.ready = 0, MS.drop = 1;
    g_no_room_mb = 0;
    plat_mutex_unlock(&lock);
}

void vk_dbg3d_submit(const Dbg3dVert *tri, int ntri_verts, const Dbg3dLine *lines, int nlines, int occlude) {
    plat_mutex_lock(&lock);
    if (ntri_verts > SH.cap_tri) {
        Dbg3dVert *p = realloc(SH.tri, (size_t)ntri_verts * sizeof *p);
        if (p) SH.tri = p, SH.cap_tri = ntri_verts;
    }
    if (nlines > SH.cap_line) {
        Dbg3dLine *p = realloc(SH.line, (size_t)nlines * sizeof *p);
        if (p) SH.line = p, SH.cap_line = nlines;
    }
    SH.ntri = ntri_verts <= SH.cap_tri ? ntri_verts - ntri_verts % 3 : 0;
    SH.nline = nlines <= SH.cap_line ? nlines : 0;
    if (SH.ntri) memcpy(SH.tri, tri, (size_t)SH.ntri * sizeof *SH.tri);
    if (SH.nline) memcpy(SH.line, lines, (size_t)SH.nline * sizeof *SH.line);
    SH.occlude = occlude, SH.ns = plat_now_ns();
    plat_mutex_unlock(&lock);
}

// ---- pipelines

static VkShaderModule module(const uint32_t *code, size_t size) {
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = size, .pCode = code};
    VkShaderModule m = VK_NULL_HANDLE;
    return vkCreateShaderModule(vk_ctx()->device, &mi, NULL, &m) == VK_SUCCESS ? m : VK_NULL_HANDLE;
}

static int init(void) {
    G.tried = 1;
    const VkCtx *c = vk_ctx();
    G.push = (PFN_vkCmdPushDescriptorSetKHR)vk_dpa(c->device, "vkCmdPushDescriptorSetKHR");
    if (!G.push) return 0;
    VkDescriptorSetLayoutBinding b = {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                      .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT};
    VkDescriptorSetLayoutCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
                                           .bindingCount = 1, .pBindings = &b};
    if (vkCreateDescriptorSetLayout(c->device, &dci, NULL, &G.dsl) != VK_SUCCESS) return 0;
    VkPushConstantRange pr = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push)};
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                     .pSetLayouts = &G.dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
    if (vkCreatePipelineLayout(c->device, &li, NULL, &G.layout) != VK_SUCCESS) return 0;
    G.ok = 1;
    return 1;
}

enum { P_MESH_DEPTH, P_MESH, P_TRI, P_LINE, P_TRI_Z };

static VkPipeline pipeline(VkFormat fmt, int kind) {
    const VkCtx *c = vk_ctx();
    VkShaderModule vs, fs;
    if (kind == P_MESH_DEPTH || kind == P_MESH)
        vs = module(dbg3d_mesh_vs_spv, sizeof dbg3d_mesh_vs_spv), fs = module(dbg3d_mesh_fs_spv, sizeof dbg3d_mesh_fs_spv);
    else if (kind == P_TRI || kind == P_TRI_Z)
        vs = module(dbg3d_tri_vs_spv, sizeof dbg3d_tri_vs_spv), fs = module(dbg3d_tri_fs_spv, sizeof dbg3d_tri_fs_spv);
    else vs = module(dbg3d_line_vs_spv, sizeof dbg3d_line_vs_spv), fs = module(dbg3d_line_fs_spv, sizeof dbg3d_line_fs_spv);
    VkPipelineShaderStageCreateInfo st[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs,
         .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs,
         .pName = "main"}};
    VkVertexInputBindingDescription bind;
    VkVertexInputAttributeDescription attr[4];
    uint32_t nattr;
    if (kind == P_MESH_DEPTH || kind == P_MESH) {
        bind = (VkVertexInputBindingDescription){0, sizeof(Dbg3dMeshVert), VK_VERTEX_INPUT_RATE_VERTEX};
        attr[0] = (VkVertexInputAttributeDescription){0, 0, VK_FORMAT_R32G32_UINT, offsetof(Dbg3dMeshVert, q)};
        nattr = 1;
    } else if (kind == P_TRI || kind == P_TRI_Z) {
        bind = (VkVertexInputBindingDescription){0, sizeof(Dbg3dVert), VK_VERTEX_INPUT_RATE_VERTEX};
        attr[0] = (VkVertexInputAttributeDescription){0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Dbg3dVert, p)};
        attr[1] = (VkVertexInputAttributeDescription){1, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(Dbg3dVert, rgba)};
        nattr = 2;
    } else {
        bind = (VkVertexInputBindingDescription){0, sizeof(Dbg3dLine), VK_VERTEX_INPUT_RATE_INSTANCE};
        attr[0] = (VkVertexInputAttributeDescription){0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Dbg3dLine, a)};
        attr[1] = (VkVertexInputAttributeDescription){1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Dbg3dLine, b)};
        attr[2] = (VkVertexInputAttributeDescription){2, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(Dbg3dLine, rgba)};
        attr[3] = (VkVertexInputAttributeDescription){3, 0, VK_FORMAT_R32_SFLOAT, offsetof(Dbg3dLine, width)};
        nattr = 4;
    }
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
                                               .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &bind,
                                               .vertexAttributeDescriptionCount = nattr, .pVertexAttributeDescriptions = attr};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vp = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
                                                 .lineWidth = 1.0f};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    // the mesh's depth pass writes the depth only; its colour pass draws where it is the nearest; the shapes test it
    // (or not: Only what you can see off, a dynamic state) and never write it; the second view's faces write theirs
    int zw = kind == P_MESH_DEPTH || kind == P_TRI_Z;
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
                                                .depthTestEnable = VK_TRUE, .depthWriteEnable = zw,
                                                .depthCompareOp = zw ? VK_COMPARE_OP_LESS : VK_COMPARE_OP_LESS_OR_EQUAL};
    // blended over the scene; the light target's alpha (if it has one) is left as the scene wrote it
    VkPipelineColorBlendAttachmentState ba = {
        .blendEnable = kind != P_MESH_DEPTH, .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO, .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .alphaBlendOp = VK_BLEND_OP_ADD,
        .colorWriteMask = kind == P_MESH_DEPTH ? 0 : VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT};
    VkPipelineColorBlendStateCreateInfo cbs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                               .attachmentCount = 1, .pAttachments = &ba};
    VkDynamicState dyn[3] = {VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT, VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT,
                             VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE};
    VkPipelineDynamicStateCreateInfo dsi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                            .dynamicStateCount = 3, .pDynamicStates = dyn};
    VkFormat zf = VK_FORMAT_D32_SFLOAT;
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                                        .colorAttachmentCount = 1, .pColorAttachmentFormats = &fmt,
                                        .depthAttachmentFormat = zf};
    VkGraphicsPipelineCreateInfo gi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri,
                                       .stageCount = 2, .pStages = st, .pVertexInputState = &vi,
                                       .pInputAssemblyState = &ia, .pViewportState = &vp, .pRasterizationState = &rs,
                                       .pMultisampleState = &ms, .pDepthStencilState = &ds, .pColorBlendState = &cbs,
                                       .pDynamicState = &dsi, .layout = G.layout};
    VkPipeline p = VK_NULL_HANDLE;
    if (!vs || !fs || vkCreateGraphicsPipelines(c->device, VK_NULL_HANDLE, 1, &gi, NULL, &p) != VK_SUCCESS) p = VK_NULL_HANDLE;
    if (vs) vkDestroyShaderModule(c->device, vs, NULL);
    if (fs) vkDestroyShaderModule(c->device, fs, NULL);
    return p;
}

static const Pipes *pipes_for(VkFormat fmt) {
    for (int i = 0; i < G.npipes; i++)
        if (G.pipes[i].fmt == fmt) return G.pipes[i].mesh ? &G.pipes[i] : NULL;
    if (G.npipes == FORMATS) return NULL;
    Pipes *p = &G.pipes[G.npipes++];
    p->fmt = fmt;
    p->mesh_depth = pipeline(fmt, P_MESH_DEPTH), p->mesh = pipeline(fmt, P_MESH);
    p->tri = pipeline(fmt, P_TRI), p->line = pipeline(fmt, P_LINE), p->tri_z = pipeline(fmt, P_TRI_Z);
    if (!p->mesh_depth || !p->mesh || !p->tri || !p->line || !p->tri_z) {
        rt_log("dbg3d: the pipelines for light target format %d could not be made; the menu draws its overlay flat",
               (int)fmt);
        p->mesh = VK_NULL_HANDLE;
        return NULL;
    }
    rt_log("dbg3d: pipelines made for light target format %d", (int)fmt);
    return p;
}

static int depth_for(uint32_t w, uint32_t h) {
    if (G.depth && G.dw == w && G.dh == h) return 1;
    if (G.depth) {   // the light target changed size: the old one may still be in use by a frame in flight
        vk_defer_view(G.depth_view);
        vk_defer_image(G.depth, G.depth_mem);
        G.depth = VK_NULL_HANDLE, G.depth_view = VK_NULL_HANDLE;
    }
    const VkCtx *c = vk_ctx();
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                            .format = VK_FORMAT_D32_SFLOAT, .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(c->vma, &ii, &ai, &G.depth, &G.depth_mem, NULL) != VK_SUCCESS) {
        G.depth = VK_NULL_HANDLE;
        return 0;
    }
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = G.depth,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_D32_SFLOAT,
                                .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1}};
    if (vkCreateImageView(c->device, &vi, NULL, &G.depth_view) != VK_SUCCESS) {
        vmaDestroyImage(c->vma, G.depth, G.depth_mem);
        G.depth = VK_NULL_HANDLE;
        return 0;
    }
    G.dw = w, G.dh = h;
    return 1;
}

static void memory_barrier(VkCommandBuffer cb) {
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                           .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                           .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &di);
}

// Whether the game's video memory has more than keep_mb of its budget left besides need bytes (vk_mem_vram_live: the
// driver's use less the free space inside VMA's blocks). Room is assumed when the budget is not known.
static int mesh_room(uint64_t need, uint64_t keep_mb, uint64_t *used, uint64_t *budget) {
    *used = *budget = 0;
    if (!vk_mem_vram_live(used, budget) || !*budget) return 1;
    return *used + need + (keep_mb << 20) <= *budget;
}

static void mesh_free(void) {
    if (G.mesh) vk_defer_buffer(G.mesh, G.mesh_mem);
    G.mesh = VK_NULL_HANDLE, G.mesh_verts = 0, g_mesh_tris = 0;
}

// A staged mesh onto the GPU (the copy recorded here, before the draws), or the mesh dropped.
static void mesh_take(VkCommandBuffer cb) {
    plat_mutex_lock(&lock);
    int drop = MS.drop, ready = MS.ready;
    VkBuffer sb = MS.buf;
    VmaAllocation sm = MS.mem;
    uint32_t verts = MS.verts;
    float origin[3] = {MS.origin[0], MS.origin[1], MS.origin[2]}, scale = MS.scale;
    if (ready) MS.buf = VK_NULL_HANDLE, MS.ready = 0;
    MS.drop = 0;
    plat_mutex_unlock(&lock);
    if (!drop && !ready) return;
    mesh_free();
    if (!ready) return;
    const VkCtx *c = vk_ctx();
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = (VkDeviceSize)verts * sizeof(Dbg3dMeshVert),
                             .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    uint64_t used, budget;
    if (!mesh_room(bi.size, ROOM_UPLOAD_MB, &used, &budget)) {
        g_no_room_mb = (uint32_t)((bi.size + (1 << 20) - 1) >> 20);
        rt_log("dbg3d: the collision mesh (%u triangles, %u MB) stays off the GPU: %llu of the %llu MB video memory "
               "budget in use; the menu draws the nearest polygons", verts / 3, g_no_room_mb,
               (unsigned long long)(used >> 20), (unsigned long long)(budget >> 20));
        vk_defer_buffer(sb, sm);
        return;
    }
    g_no_room_mb = 0;
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateBuffer(c->vma, &bi, &ai, &G.mesh, &G.mesh_mem, NULL) != VK_SUCCESS) {
        rt_log("dbg3d: no video memory for the collision mesh (%u triangles)", verts / 3);
        G.mesh = VK_NULL_HANDLE;
        vk_defer_buffer(sb, sm);
        return;
    }
    VkBufferCopy r = {0, 0, bi.size};
    vkCmdCopyBuffer(cb, sb, G.mesh, 1, &r);
    memory_barrier(cb);
    vk_defer_buffer(sb, sm);   // once the copy ran
    G.mesh_verts = verts, g_mesh_tris = verts / 3;
    memcpy(G.mesh_origin, origin, sizeof G.mesh_origin), G.mesh_scale = scale;
    G.room_ns = plat_now_ns();
    rt_log("dbg3d: collision mesh of %u triangles (%.0f MB) on the GPU", verts / 3, (double)bi.size / (1 << 20));
}

// This frame's menu shapes into a buffer that lives until the frame's submission is done.
static VkBuffer shapes_buffer(VkDeviceSize *tri_off, VkDeviceSize *line_off, int *ntri, int *nline, int *occlude) {
    *ntri = *nline = 0;
    plat_mutex_lock(&lock);
    if (!SH.ns || plat_now_ns() - SH.ns > SHAPES_NS || (!SH.ntri && !SH.nline)) {
        plat_mutex_unlock(&lock);
        return VK_NULL_HANDLE;
    }
    size_t tb = (size_t)SH.ntri * sizeof(Dbg3dVert), lb = (size_t)SH.nline * sizeof(Dbg3dLine);
    const VkCtx *c = vk_ctx();
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = tb + lb + 16,
                             .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                  .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
    VkBuffer buf;
    VmaAllocation mem;
    VmaAllocationInfo info;
    if (vmaCreateBuffer(c->vma, &bi, &ai, &buf, &mem, &info) != VK_SUCCESS) {
        plat_mutex_unlock(&lock);
        return VK_NULL_HANDLE;
    }
    size_t lo = (tb + 15) & ~(size_t)15;
    memcpy(info.pMappedData, SH.tri, tb);
    memcpy((char *)info.pMappedData + lo, SH.line, lb);
    *ntri = SH.ntri, *nline = SH.nline, *occlude = SH.occlude;
    plat_mutex_unlock(&lock);
    vmaFlushAllocation(c->vma, mem, 0, VK_WHOLE_SIZE);
    vk_defer_buffer(buf, mem);   // freed once the submission being recorded is done
    *tri_off = 0, *line_off = lo;
    return buf;
}

// The eye: where clip x, y and w are all 0 (rows 0, 1 and 3 of the view-projection), by Cramer's rule.
static int eye_of(const float *vp, float e[3]) {
    const float *a = vp, *b = vp + 4, *c = vp + 12;
    float det = a[0] * (b[1] * c[2] - b[2] * c[1]) - a[1] * (b[0] * c[2] - b[2] * c[0]) + a[2] * (b[0] * c[1] - b[1] * c[0]);
    if (fabsf(det) < 1e-9f) return 0;
    float r[3] = {-a[3], -b[3], -c[3]};
    e[0] = (r[0] * (b[1] * c[2] - b[2] * c[1]) - a[1] * (r[1] * c[2] - b[2] * r[2]) + a[2] * (r[1] * c[1] - b[1] * r[2])) / det;
    e[1] = (a[0] * (r[1] * c[2] - b[2] * r[2]) - r[0] * (b[0] * c[2] - b[2] * c[0]) + a[2] * (b[0] * r[2] - r[1] * c[0])) / det;
    e[2] = (a[0] * (b[1] * r[2] - r[1] * c[1]) - a[1] * (b[0] * r[2] - r[1] * c[0]) + r[0] * (b[0] * c[1] - b[1] * c[0])) / det;
    return 1;
}

void vk_dbg3d_draw(VkCommandBuffer cb, VkImageView cv, VkFormat fmt, uint32_t w, uint32_t h, uint32_t ow, VkImageView dv,
                   int dist_ok, float jx, float jy) {
    if (!cv || !dv || w < 2 || h < 2) return;
    g_ran_ns = plat_now_ns();
    plat_mutex_lock(&plock);
    if (!G.tried) init();
    plat_mutex_unlock(&plock);
    if (!G.ok) return;
    mesh_take(cb);
    if (G.mesh && plat_now_ns() - G.room_ns > ROOM_CHECK_NS) {   // the game came to need the room: give it back
        G.room_ns = plat_now_ns();
        uint64_t used, budget;
        if (!mesh_room(0, ROOM_KEEP_MB, &used, &budget)) {
            g_no_room_mb = (uint32_t)(((uint64_t)G.mesh_verts * sizeof(Dbg3dMeshVert) + (1 << 20) - 1) >> 20);
            rt_log("dbg3d: the collision mesh (%u MB) taken off the GPU: %llu of the %llu MB video memory budget in use; "
                   "the menu draws the nearest polygons", g_no_room_mb, (unsigned long long)(used >> 20),
                   (unsigned long long)(budget >> 20));
            mesh_free();
        }
    }
    int coll = cfg_bool("NX2NAVI_DEBUG_VIEW_COLLISION") && G.mesh;
    VkDeviceSize toff = 0, loff = 0;
    int ntri = 0, nline = 0, occlude = 0;
    VkBuffer sb = shapes_buffer(&toff, &loff, &ntri, &nline, &occlude);
    if (!coll && !sb) return;
    float aspect;
    Push pc;
    memset(&pc, 0, sizeof pc);
    if (!vk_dbgcam_vp(pc.vp, &aspect) || !eye_of(pc.vp, pc.eye)) return;
    plat_mutex_lock(&plock);
    const Pipes *P = pipes_for(fmt);
    plat_mutex_unlock(&plock);
    if (!P || !depth_for(w, h)) return;
    pc.eye[3] = ow ? (float)w / (float)ow : 1.0f;
    pc.view[0] = (float)w, pc.view[1] = (float)h;

    VkImageMemoryBarrier2 zb = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                .dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                                .dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                .image = G.depth, .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1}};
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                           .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                           .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb,
                           .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &zb};
    vkCmdPipelineBarrier2(cb, &di);

    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = cv,
                                    .imageLayout = VK_IMAGE_LAYOUT_GENERAL, .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                                    .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingAttachmentInfo za = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = G.depth_view,
                                    .imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                                    .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                    .clearValue = {.depthStencil = {1.0f, 0}}};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {w, h}}, .layerCount = 1,
                          .colorAttachmentCount = 1, .pColorAttachments = &ca, .pDepthAttachment = &za};
    vkCmdBeginRendering(cb, &ri);
    VkViewport v = {jx, (float)h + jy, (float)w, -(float)h, 0.0f, 1.0f};   // the scene's: the y flip and the jitter
    VkRect2D sc = {{0, 0}, {w, h}};
    vkCmdSetViewportWithCount(cb, 1, &v);
    vkCmdSetScissorWithCount(cb, 1, &sc);
    VkDescriptorImageInfo dii = {VK_NULL_HANDLE, dv, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet wr = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1,
                               .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &dii};
    rec_push(G.push, cb, VK_PIPELINE_BIND_POINT_GRAPHICS, G.layout, 0, 1, &wr);
    const VkShaderStageFlags stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    int hidden = cfg_bool("NX2NAVI_DEBUG_HIDE_MESHES") != 0, occ = cfg_bool("NX2NAVI_DEBUG_VIEW_OCCLUDE") != 0;
    int opacity = cfg_int("NX2NAVI_DEBUG_VIEW_OPACITY");
    opacity = opacity < 10 ? 10 : opacity > 100 ? 100 : opacity;
    const char *st = cfg_value("NX2NAVI_DEBUG_VIEW_STYLE");
    int style = !strcmp(st, "solid") ? 2 : !strcmp(st, "both") ? 3 : 1;

    if (coll) {
        // With the meshes hidden the scene holds the player alone: the mesh is opaque and cut only by the player, closely.
        // Otherwise see-through at the overlay's opacity and, with Only what you can see, hidden where the scene is
        // nearer by more than a little (the ground's collision lies at or just under the ground).
        // The mesh's corners are steps from its origin: the view-projection takes them as they are (each row times the
        // step, plus the row at the origin), and the eye and the player are given in steps too.
        Push m = pc;
        const float *o = G.mesh_origin, k = G.mesh_scale;
        for (int r = 0; r < 4; r++) {
            const float *row = pc.vp + 4 * r;
            double at = (double)row[0] * o[0] + (double)row[1] * o[1] + (double)row[2] * o[2] + row[3];
            m.vp[4 * r + 0] = row[0] * k, m.vp[4 * r + 1] = row[1] * k, m.vp[4 * r + 2] = row[2] * k;
            m.vp[4 * r + 3] = (float)at;
        }
        for (int a = 0; a < 3; a++) m.eye[a] = (pc.eye[a] - o[a]) / k;
        if (dbg_player_pos(m.player)) {
            for (int a = 0; a < 3; a++) m.player[a] = (m.player[a] - o[a]) / k;
            m.player[3] = (float)cfg_int("NX2NAVI_DEBUG_COLLISION_RANGE") / k;
        }
        if (hidden) m.test[0] = 1, m.test[1] = 0.03f, m.test[2] = 0.002f, m.test[3] = 1.0f;
        else m.test[0] = occ ? 1.0f : 0.0f, m.test[1] = 0.3f, m.test[2] = 0.02f, m.test[3] = (float)opacity / 100.0f;
        if (!dist_ok) m.test[0] = 0;
        m.view[2] = (float)(hidden ? (style | 2) : style);
        VkDeviceSize zero = 0;
        vkCmdBindVertexBuffers2(cb, 0, 1, &G.mesh, &zero, NULL, NULL);
        vkCmdSetDepthTestEnable(cb, VK_TRUE);
        m.view[3] = 1;
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, P->mesh_depth);
        vkCmdPushConstants(cb, G.layout, stages, 0, sizeof m, &m);
        vkCmdDraw(cb, G.mesh_verts, 1, 0, 0);
        m.view[3] = 0;
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, P->mesh);
        vkCmdPushConstants(cb, G.layout, stages, 0, sizeof m, &m);
        vkCmdDraw(cb, G.mesh_verts, 1, 0, 0);
    }
    if (sb) {
        // With the meshes hidden the scene holds the player alone, so the shapes are cut by him closely, as the mesh is.
        Push s = pc;
        s.test[0] = occlude && dist_ok ? 1.0f : 0.0f, s.test[1] = 0.3f, s.test[2] = 0.02f, s.test[3] = 1.0f;
        if (hidden && dist_ok) s.test[0] = 1.0f, s.test[1] = 0.03f, s.test[2] = 0.002f;
        vkCmdSetDepthTestEnable(cb, occlude || hidden ? VK_TRUE : VK_FALSE);
        if (ntri) {
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, P->tri);
            vkCmdPushConstants(cb, G.layout, stages, 0, sizeof s, &s);
            vkCmdBindVertexBuffers2(cb, 0, 1, &sb, &toff, NULL, NULL);
            vkCmdDraw(cb, (uint32_t)ntri, 1, 0, 0);
        }
        if (nline) {
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, P->line);
            vkCmdPushConstants(cb, G.layout, stages, 0, sizeof s, &s);
            vkCmdBindVertexBuffers2(cb, 0, 1, &sb, &loff, NULL, NULL);
            vkCmdDraw(cb, 6, (uint32_t)nline, 0, 0);
        }
    }
    vkCmdEndRendering(cb);
    memory_barrier(cb);
}

// ---- the clips' second view (vk_dbg3d_view2)
// A camera of its own beside the game's (above the player, to one side or in front, as NX2NAVI_DEBUG_CLIP_VIEW2 says)
// sees the area's collision near the player as opaque faces coloured as the collision overlay colours them, the player
// as an orange block in a ring with a line where they face, the game's camera as a white frustum, and the menu's 3D shapes of
// the frame (vk_dbg3d_submit) over them. What lies between the camera and the player (a cave's ceiling seen from
// above, a wall from the side) is cut away: faces wholly nearer the camera than V2_CUT metres short of the player are
// left out. It is drawn into an image of its own on the present's command buffer and
// blitted into the corner of each clip frame. The polygons come from the debug hook's grid (debug_hook.c
// dbg_hook_polys, which debug_view.c dbg_view_kinds keeps wanted while the view is on), read again every
// V2_POLL_NS; the vertices go into a host buffer per present fence slot, reused once that slot's fence has signalled.
#define V2_W 640
#define V2_H 360
#define V2_SLOTS 4
#define V2_POLYS 12000
#define V2_RANGE 45.0f
#define V2_POLL_NS 150000000ull
#define V2_BORDER 3
#define V2_CUT 2.5f
#define V2_RING 12

static struct {
    int tried, ok;
    VkImage img;   // R8G8B8A8_UNORM, drawn through an sRGB view so the blit copies the encoded colours as they are
    VmaAllocation img_mem;
    VkImageView img_view;
    VkImage depth;
    VmaAllocation depth_mem;
    VkImageView depth_view;
    VkImage dist;   // the view distances the shaders declare: never read here (test.x 0), but bound
    VmaAllocation dist_mem;
    VkImageView dist_view;
    struct {
        VkBuffer buf;
        VmaAllocation mem;
        void *map;
        VkDeviceSize size;
    } vb[V2_SLOTS];
    DbgHookPoly *poly;
    Dbg3dVert *tri;   // the collision's faces, world space, made again with the polygons
    int ntri, cap_tri;
    uint64_t poly_ns;
} V2;

static void v2_image(VkFormat fmt, VkImageUsageFlags usage, VkImageCreateFlags flags, uint32_t w, uint32_t h,
                     VkImage *img, VmaAllocation *mem, VkImageView *view, VkFormat view_fmt, VkImageAspectFlags aspect,
                     int *ok) {
    if (!*ok) return;
    const VkCtx *c = vk_ctx();
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = flags, .imageType = VK_IMAGE_TYPE_2D,
                            .format = fmt, .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(c->vma, &ii, &ai, img, mem, NULL) != VK_SUCCESS) {
        *img = VK_NULL_HANDLE, *ok = 0;
        return;
    }
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = *img,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = view_fmt,
                                .subresourceRange = {aspect, 0, 1, 0, 1}};
    if (vkCreateImageView(c->device, &vi, NULL, view) != VK_SUCCESS) *view = VK_NULL_HANDLE, *ok = 0;
}

static int v2_init(void) {
    V2.tried = 1;
    int ok = 1;
    v2_image(VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
             VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT, V2_W, V2_H, &V2.img, &V2.img_mem, &V2.img_view, VK_FORMAT_R8G8B8A8_SRGB,
             VK_IMAGE_ASPECT_COLOR_BIT, &ok);
    v2_image(VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, 0, V2_W, V2_H, &V2.depth, &V2.depth_mem,
             &V2.depth_view, VK_FORMAT_D32_SFLOAT, VK_IMAGE_ASPECT_DEPTH_BIT, &ok);
    v2_image(VK_FORMAT_R32_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT, 0, 1, 1, &V2.dist, &V2.dist_mem, &V2.dist_view,
             VK_FORMAT_R32_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, &ok);
    V2.poly = malloc(V2_POLYS * sizeof *V2.poly);
    if (!V2.poly) ok = 0;
    if (!ok) rt_log("dbg3d: the clips' second view could not be set up");
    return V2.ok = ok;
}

static void view2_release(void) {
    const VkCtx *c = vk_ctx();
    if (V2.img_view) vkDestroyImageView(c->device, V2.img_view, NULL);
    if (V2.img) vmaDestroyImage(c->vma, V2.img, V2.img_mem);
    if (V2.depth_view) vkDestroyImageView(c->device, V2.depth_view, NULL);
    if (V2.depth) vmaDestroyImage(c->vma, V2.depth, V2.depth_mem);
    if (V2.dist_view) vkDestroyImageView(c->device, V2.dist_view, NULL);
    if (V2.dist) vmaDestroyImage(c->vma, V2.dist, V2.dist_mem);
    for (int i = 0; i < V2_SLOTS; i++)
        if (V2.vb[i].buf) vmaDestroyBuffer(c->vma, V2.vb[i].buf, V2.vb[i].mem);
    free(V2.poly);
    free(V2.tri);
    memset(&V2, 0, sizeof V2);
}

static uint32_t rgba(int r, int g, int b, int a) { return (uint32_t)r | (uint32_t)g << 8 | (uint32_t)b << 16 | (uint32_t)a << 24; }

static void v3sub(const float *a, const float *b, float *o) { o[0] = a[0] - b[0], o[1] = a[1] - b[1], o[2] = a[2] - b[2]; }
static float v3dot(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static void v3cross(const float *a, const float *b, float *o) {
    o[0] = a[1] * b[2] - a[2] * b[1], o[1] = a[2] * b[0] - a[0] * b[2], o[2] = a[0] * b[1] - a[1] * b[0];
}
static int v3norm(float *v) {
    float l = sqrtf(v3dot(v, v));
    if (!(l > 1e-6f)) return 0;
    v[0] /= l, v[1] /= l, v[2] /= l;
    return 1;
}

// The collision near the player as faces: ground green, slopes yellow, walls blue, ceilings red (the mesh shader's
// colours), lit from one side so neighbouring faces part.
static void v2_faces(void) {
    int n = dbg_hook_polys(V2.poly, V2_POLYS, V2_RANGE, DBG_POLY_COLLISION), need = 0;
    for (int i = 0; i < n; i++) need += 3 * (V2.poly[i].n - 2);
    if (need > V2.cap_tri) {
        Dbg3dVert *p = realloc(V2.tri, (size_t)need * sizeof *p);
        if (!p) return;
        V2.tri = p, V2.cap_tri = need;
    }
    static const float light[3] = {0.42f, 0.82f, 0.39f};
    int m = 0;
    for (int i = 0; i < n; i++) {
        const DbgHookPoly *q = &V2.poly[i];
        float e1[3], e2[3], nr[3];
        v3sub(q->p[1], q->p[0], e1), v3sub(q->p[2], q->p[0], e2), v3cross(e1, e2, nr);
        if (!v3norm(nr)) continue;
        if (nr[1] < 0 && nr[1] > -0.25f) nr[0] = -nr[0], nr[1] = -nr[1], nr[2] = -nr[2];   // a wall either way round
        float y = nr[1], k = 0.5f + 0.5f * fabsf(v3dot(nr, light));
        int r, g, b;
        if (y > 0.7f) r = 70, g = 220, b = 110;
        else if (y > 0.25f) r = 235, g = 205, b = 70;
        else if (y > -0.25f) r = 80, g = 150, b = 255;
        else r = 240, g = 80, b = 80;
        uint32_t col = rgba((int)(r * k), (int)(g * k), (int)(b * k), 255);
        for (int f = 1; f + 1 < q->n; f++) {
            const int corner[3] = {0, f, f + 1};
            for (int c = 0; c < 3; c++) {
                memcpy(V2.tri[m].p, q->p[corner[c]], sizeof V2.tri[m].p);
                V2.tri[m++].rgba = col;
            }
        }
    }
    V2.ntri = m;
}

// The box from lo to hi as 12 triangles.
static int v2_box(Dbg3dVert *o, const float lo[3], const float hi[3], uint32_t top, uint32_t side) {
    static const int face[6][4] = {{0, 1, 3, 2}, {4, 6, 7, 5}, {0, 4, 5, 1}, {2, 3, 7, 6}, {0, 2, 6, 4}, {1, 5, 7, 3}};
    int n = 0;
    for (int f = 0; f < 6; f++) {
        const int k[6] = {face[f][0], face[f][1], face[f][2], face[f][0], face[f][2], face[f][3]};
        for (int j = 0; j < 6; j++) {
            int c = k[j];
            o[n].p[0] = c & 1 ? hi[0] : lo[0], o[n].p[1] = c & 2 ? hi[1] : lo[1], o[n].p[2] = c & 4 ? hi[2] : lo[2];
            o[n++].rgba = f == 3 ? top : side;
        }
    }
    return n;
}

static void v2_line(Dbg3dLine *l, const float a[3], const float b[3], uint32_t col, float width) {
    memcpy(l->a, a, sizeof l->a), memcpy(l->b, b, sizeof l->b);
    l->rgba = col, l->width = width;
}

// The second camera's view-projection, as vk_dbgcam_vp gives the game's (row-major, clip w the distance along the
// view): mode 1 above the player looking down, the picture's top where the game's camera looks; 2 to the right of the
// game's camera; 3 in front of the player, facing the game's camera.
static int v2_camera(int mode, const float player[3], const float game_eye[3], const float fwd[3], float vp[16]) {
    float t[3] = {player[0], player[1] + 1.0f, player[2]}, fh[3] = {fwd[0], 0, fwd[2]};
    if (!v3norm(fh)) fh[0] = 0, fh[2] = 1;
    float right[3] = {-fh[2], 0, fh[0]}, eye[3], f[3], up[3] = {0, 1, 0};
    (void)game_eye;
    if (mode == 1) {
        eye[0] = t[0], eye[1] = t[1] + 20.0f, eye[2] = t[2];
        f[0] = 0, f[1] = -1, f[2] = 0;
        memcpy(up, fh, sizeof up);
    } else {
        const float *d = mode == 2 ? right : fh;
        float back = mode == 2 ? 13.0f : 10.0f;
        eye[0] = t[0] + d[0] * back, eye[1] = t[1] + 3.5f, eye[2] = t[2] + d[2] * back;
        v3sub(t, eye, f);
        if (!v3norm(f)) return 0;
    }
    float s[3], u[3];
    v3cross(f, up, s);
    if (!v3norm(s)) return 0;
    v3cross(s, f, u);
    const float fy = 1.0f / tanf(0.5f * 1.05f), fx = fy * (float)V2_H / (float)V2_W;   // 60 degrees high
    const float *rows[3] = {s, u, f};
    const float k[3] = {fx, fy, 1.0f};
    for (int r = 0; r < 3; r++) {
        float *o = vp + 4 * (r == 2 ? 3 : r);
        for (int a = 0; a < 3; a++) o[a] = rows[r][a] * k[r];
        o[3] = -v3dot(rows[r], eye) * k[r];
    }
    memcpy(vp + 8, vp + 12, 4 * sizeof *vp);   // the depth comes from w (dbg3d_common.glsl out_pos)
    return 1;
}

static VkBuffer v2_vertices(int slot, const void *a, size_t an, const void *b, size_t bn, const void *c, size_t cn,
                            VkDeviceSize off[3]) {
    VkDeviceSize o1 = (an + 15) & ~(size_t)15, o2 = o1 + ((bn + 15) & ~(size_t)15), need = o2 + cn + 16;
    const VkCtx *ctx = vk_ctx();
    if (V2.vb[slot].size < need) {   // this slot's last submission is done: its buffer can go now
        if (V2.vb[slot].buf) vmaDestroyBuffer(ctx->vma, V2.vb[slot].buf, V2.vb[slot].mem);
        V2.vb[slot].buf = VK_NULL_HANDLE, V2.vb[slot].size = 0;
        VkDeviceSize size = need + need / 2;
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
                                 .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                      .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
        VmaAllocationInfo info;
        if (vmaCreateBuffer(ctx->vma, &bi, &ai, &V2.vb[slot].buf, &V2.vb[slot].mem, &info) != VK_SUCCESS) {
            V2.vb[slot].buf = VK_NULL_HANDLE;
            return VK_NULL_HANDLE;
        }
        V2.vb[slot].map = info.pMappedData, V2.vb[slot].size = size;
    }
    char *m = V2.vb[slot].map;
    if (an) memcpy(m, a, an);
    if (bn) memcpy(m + o1, b, bn);
    if (cn) memcpy(m + o2, c, cn);
    vmaFlushAllocation(ctx->vma, V2.vb[slot].mem, 0, need);
    off[0] = 0, off[1] = o1, off[2] = o2;
    return V2.vb[slot].buf;
}

static float lin(int c) { return powf((float)c / 255.0f, 2.2f); }

int vk_dbg3d_view2(VkCommandBuffer cb, unsigned slot, int mode, VkImage dst, VkFormat dst_fmt, int dw, int dh) {
    if (mode < 1 || mode > 3 || slot >= V2_SLOTS || dw < 64 || dh < 64) return 0;
    // the clip frame holds the swapchain's 8-bit colours: an HDR one's are PQ-coded, which this view does not make
    if (dst_fmt != VK_FORMAT_B8G8R8A8_UNORM && dst_fmt != VK_FORMAT_R8G8B8A8_UNORM) return 0;
    plat_mutex_lock(&plock);
    if (!G.tried) init();
    const Pipes *P = G.ok ? pipes_for(VK_FORMAT_R8G8B8A8_SRGB) : NULL;
    plat_mutex_unlock(&plock);
    if (!P) return 0;
    if (!V2.tried) v2_init();
    if (!V2.ok) return 0;
    float player[3], vp[16], game_vp[16], game_eye[3], fwd[3], aspect;
    if (!dbg_player_pos(player)) return 0;
    int have_game = vk_dbgcam_vp(game_vp, &aspect) && eye_of(game_vp, game_eye);
    if (have_game) memcpy(fwd, game_vp + 12, sizeof fwd);
    else if (!dbg_player_facing(fwd)) fwd[0] = 0, fwd[1] = 0, fwd[2] = 1;
    if (!v3norm(fwd)) fwd[0] = 0, fwd[1] = 0, fwd[2] = 1;
    if (!v2_camera(mode, player, game_eye, fwd, vp)) return 0;
    uint64_t now = plat_now_ns();
    if (!V2.poly_ns || now - V2.poly_ns > V2_POLL_NS) V2.poly_ns = now, v2_faces();

    // the player, where they face, and the game's camera
    Dbg3dVert extra[36];
    Dbg3dLine marks[9 + V2_RING];
    int nextra, nmarks = 0;
    float lo[3] = {player[0] - 0.35f, player[1], player[2] - 0.35f}, hi[3] = {player[0] + 0.35f, player[1] + 1.8f, player[2] + 0.35f};
    nextra = v2_box(extra, lo, hi, rgba(255, 190, 90, 255), rgba(255, 130, 30, 255));
    for (int i = 0; i < V2_RING; i++) {   // a ring round the feet, 1.2 m out, to find the player at a glance
        float a0 = 6.2831853f * (float)i / V2_RING, a1 = 6.2831853f * (float)(i + 1) / V2_RING;
        float p0[3] = {player[0] + 1.2f * cosf(a0), player[1] + 0.05f, player[2] + 1.2f * sinf(a0)};
        float p1[3] = {player[0] + 1.2f * cosf(a1), player[1] + 0.05f, player[2] + 1.2f * sinf(a1)};
        v2_line(&marks[nmarks++], p0, p1, rgba(255, 150, 40, 255), 3.0f);
    }
    float face[3], chest[3] = {player[0], player[1] + 1.1f, player[2]}, tip[3];
    if (dbg_player_facing(face)) {
        face[1] = 0;
        if (v3norm(face)) {
            for (int a = 0; a < 3; a++) tip[a] = chest[a] + face[a] * 1.6f;
            v2_line(&marks[nmarks++], chest, tip, rgba(255, 240, 120, 255), 3.0f);
        }
    }
    if (have_game) {
        // the frustum's far corners 1.5 m out, from the inverse of the game's rows: the eye plus the view's own axes
        float s[3] = {game_vp[0], game_vp[1], game_vp[2]}, u[3] = {game_vp[4], game_vp[5], game_vp[6]}, f[3];
        memcpy(f, fwd, sizeof f);
        float kx = v3norm(s) ? 1.0f : 0.0f, ky = v3norm(u) ? 1.0f : 0.0f;
        if (kx > 0 && ky > 0) {
            float hw = 0.9f * (aspect > 0.1f && aspect < 10 ? aspect : 16.0f / 9.0f) * 0.5f, hh = 0.9f * 0.5f, c[4][3];
            for (int i = 0; i < 4; i++) {
                float sx = i & 1 ? hw : -hw, sy = i & 2 ? hh : -hh;
                for (int a = 0; a < 3; a++) c[i][a] = game_eye[a] + f[a] * 1.5f + s[a] * sx + u[a] * sy;
                v2_line(&marks[nmarks++], game_eye, c[i], rgba(255, 255, 255, 230), 2.0f);
            }
            const int edge[4][2] = {{0, 1}, {1, 3}, {3, 2}, {2, 0}};
            for (int i = 0; i < 4; i++) v2_line(&marks[nmarks++], c[edge[i][0]], c[edge[i][1]], rgba(255, 255, 255, 230), 2.0f);
        }
    }
    // the menu's shapes of this frame, appended to the marks' lines and drawn after the faces
    plat_mutex_lock(&lock);
    int fresh = SH.ns && now - SH.ns <= SHAPES_NS;
    int sh_tri = fresh ? SH.ntri : 0, sh_line = fresh ? SH.nline : 0;
    size_t ftb = (size_t)V2.ntri * sizeof(Dbg3dVert), xtb = (size_t)nextra * sizeof(Dbg3dVert);
    size_t stb = (size_t)sh_tri * sizeof(Dbg3dVert), lb = (size_t)(nmarks + sh_line) * sizeof(Dbg3dLine);
    // faces then the player's block (one draw with the depth written), the shapes' triangles, then the lines
    Dbg3dVert *opaque = malloc(ftb + xtb + 16);
    Dbg3dLine *lines = malloc(lb + 16);
    int nface = 0;
    if (opaque && lines) {
        // the cut: clip w (row 3) is the distance along the view
        const float *f = vp + 12, cut = f[0] * player[0] + f[1] * (player[1] + 1.0f) + f[2] * player[2] + f[3] - V2_CUT;
        for (int i = 0; i + 2 < V2.ntri; i += 3) {
            const Dbg3dVert *q = &V2.tri[i];
            float far = -1e30f;
            for (int c = 0; c < 3; c++) far = fmaxf(far, f[0] * q[c].p[0] + f[1] * q[c].p[1] + f[2] * q[c].p[2] + f[3]);
            if (far < cut) continue;
            memcpy(opaque + nface, q, 3 * sizeof *q);
            nface += 3;
        }
        ftb = (size_t)nface * sizeof(Dbg3dVert);
        memcpy((char *)opaque + ftb, extra, xtb);
        memcpy(lines, marks, (size_t)nmarks * sizeof *marks);
        if (sh_line) memcpy(lines + nmarks, SH.line, (size_t)sh_line * sizeof *lines);
    }
    VkDeviceSize off[3];
    VkBuffer vb = opaque && lines ? v2_vertices((int)slot, opaque, ftb + xtb, SH.tri, stb, lines, lb, off) : VK_NULL_HANDLE;
    plat_mutex_unlock(&lock);
    free(opaque);
    free(lines);
    if (!vb) return 0;

    VkImageMemoryBarrier2 ib[3] = {
        {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
         .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
         .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT,
         .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = V2.img,
         .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}},
        {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
         .dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
         .dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
         .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = V2.depth,
         .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1}},
        {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
         .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
         .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = V2.dist,
         .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}}};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 3, .pImageMemoryBarriers = ib};
    vkCmdPipelineBarrier2(cb, &di);

    // a light frame round a dark ground (the clear values are linear: the view encodes them)
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = V2.img_view,
                                    .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                    .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                    .clearValue = {.color = {.float32 = {lin(235), lin(235), lin(235), 1}}}};
    VkRenderingAttachmentInfo za = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = V2.depth_view,
                                    .imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                                    .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                    .clearValue = {.depthStencil = {1.0f, 0}}};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {V2_W, V2_H}}, .layerCount = 1,
                          .colorAttachmentCount = 1, .pColorAttachments = &ca, .pDepthAttachment = &za};
    vkCmdBeginRendering(cb, &ri);
    VkClearAttachment clr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, {.color = {.float32 = {lin(22), lin(25), lin(32), 1}}}};
    VkClearRect cr = {{{V2_BORDER, V2_BORDER}, {V2_W - 2 * V2_BORDER, V2_H - 2 * V2_BORDER}}, 0, 1};
    vkCmdClearAttachments(cb, 1, &clr, 1, &cr);
    VkViewport v = {0, (float)V2_H, (float)V2_W, -(float)V2_H, 0.0f, 1.0f};   // clip y up, as the game's view is drawn
    vkCmdSetViewportWithCount(cb, 1, &v);
    vkCmdSetScissorWithCount(cb, 1, &cr.rect);
    VkDescriptorImageInfo dii = {VK_NULL_HANDLE, V2.dist_view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet wr = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1,
                               .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &dii};
    rec_push(G.push, cb, VK_PIPELINE_BIND_POINT_GRAPHICS, G.layout, 0, 1, &wr);
    Push pc;
    memset(&pc, 0, sizeof pc);
    memcpy(pc.vp, vp, sizeof pc.vp);
    pc.eye[3] = 1.0f;   // the lines' widths: one render pixel an output pixel
    pc.test[3] = 1.0f;
    pc.view[0] = (float)V2_W, pc.view[1] = (float)V2_H, pc.view[2] = 2;
    const VkShaderStageFlags stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    vkCmdSetDepthTestEnable(cb, VK_TRUE);
    uint32_t nop = (uint32_t)(nface + nextra);
    if (nop) {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, P->tri_z);
        vkCmdPushConstants(cb, G.layout, stages, 0, sizeof pc, &pc);
        vkCmdBindVertexBuffers2(cb, 0, 1, &vb, &off[0], NULL, NULL);
        vkCmdDraw(cb, nop, 1, 0, 0);
    }
    if (sh_tri) {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, P->tri);
        vkCmdPushConstants(cb, G.layout, stages, 0, sizeof pc, &pc);
        vkCmdBindVertexBuffers2(cb, 0, 1, &vb, &off[1], NULL, NULL);
        vkCmdDraw(cb, (uint32_t)sh_tri, 1, 0, 0);
    }
    if (nmarks + sh_line) {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, P->line);
        vkCmdPushConstants(cb, G.layout, stages, 0, sizeof pc, &pc);
        vkCmdBindVertexBuffers2(cb, 0, 1, &vb, &off[2], NULL, NULL);
        vkCmdDraw(cb, 6, (uint32_t)(nmarks + sh_line), 0, 0);
    }
    vkCmdEndRendering(cb);

    // into the clip frame's lower right corner, a third of its width
    VkImageMemoryBarrier2 tb[2] = {
        {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
         .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
         .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT, .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
         .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = V2.img, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}},
        {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
         .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
         .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
         .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = dst, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}}};
    VkDependencyInfo td = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 2, .pImageMemoryBarriers = tb};
    vkCmdPipelineBarrier2(cb, &td);
    int pw = (dw / 3) & ~1, ph = (pw * V2_H / V2_W) & ~1, margin = dw / 64;
    int x1 = dw - margin, y1 = dh - margin, x0 = x1 - pw, y0 = y1 - ph;
    if (x0 < 0 || y0 < 0) return 0;
    VkImageBlit region = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .srcOffsets = {{0, 0, 0}, {V2_W, V2_H, 1}},
                          .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstOffsets = {{x0, y0, 0}, {x1, y1, 1}}};
    vkCmdBlitImage(cb, V2.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region,
                   VK_FILTER_LINEAR);
    return 1;
}

void vk_dbg3d_release(void) {
    const VkCtx *c = vk_ctx();
    for (int i = 0; i < G.npipes; i++) {
        Pipes *p = &G.pipes[i];
        if (p->mesh_depth) vkDestroyPipeline(c->device, p->mesh_depth, NULL);
        if (p->mesh) vkDestroyPipeline(c->device, p->mesh, NULL);
        if (p->tri) vkDestroyPipeline(c->device, p->tri, NULL);
        if (p->line) vkDestroyPipeline(c->device, p->line, NULL);
        if (p->tri_z) vkDestroyPipeline(c->device, p->tri_z, NULL);
    }
    view2_release();
    if (G.depth_view) vkDestroyImageView(c->device, G.depth_view, NULL);
    if (G.depth) vmaDestroyImage(c->vma, G.depth, G.depth_mem);
    if (G.mesh) vmaDestroyBuffer(c->vma, G.mesh, G.mesh_mem);
    if (G.layout) vkDestroyPipelineLayout(c->device, G.layout, NULL);
    if (G.dsl) vkDestroyDescriptorSetLayout(c->device, G.dsl, NULL);
    memset(&G, 0, sizeof G);
    plat_mutex_lock(&lock);
    if (MS.buf) vmaDestroyBuffer(c->vma, MS.buf, MS.mem);
    MS.buf = VK_NULL_HANDLE, MS.filling = MS.ready = 0;
    plat_mutex_unlock(&lock);
}
