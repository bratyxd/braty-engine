/* br_assets_compressed: DDS (legacy FourCC and DX10 header) and KTX1 textures, uploaded to
 * the GPU as they are (BC1-BC7, ASTC, RGBA8/BGRA8) when SDL_GPUTextureSupportsFormat says
 * the device can sample them. Registered as type "texture", so br_texture_load works.
 *
 * Split loader: decode (worker) validates the header and copies the mip levels into one
 * buffer; finish (main thread) checks GPU support, uploads every level and wraps the result
 * in a br_texture.
 *
 * Gotchas: compressed data cannot be premultiplied on upload, so author alpha textures
 * premultiplied. sRGB formats are uploaded as their UNORM twins, matching the PNG path (the
 * 2D pipeline does not linearise). */
#include "assets/br_assets_internal.h"
#include "braty/br_texture.h"

#define BR_COMPRESSED_MAX_LEVELS 16
#define BR_UPLOAD_ALIGN 512   /* D3D12 texture placement alignment; harmless elsewhere */

/* A GPU format and its block layout. */
typedef struct br_block_format {
    uint32_t             code;      /* DXGI_FORMAT (dds) or glInternalFormat (ktx) */
    SDL_GPUTextureFormat format;
    uint32_t             block_w;
    uint32_t             block_h;
    uint32_t             block_bytes;
    const char          *name;
} br_block_format;

static const br_block_format br_dxgi_formats[] = {
    { 28, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, 1, 1, 4, "RGBA8" },
    { 29, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, 1, 1, 4, "RGBA8 sRGB" },
    { 87, SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM, 1, 1, 4, "BGRA8" },
    { 91, SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM, 1, 1, 4, "BGRA8 sRGB" },
    { 71, SDL_GPU_TEXTUREFORMAT_BC1_RGBA_UNORM, 4, 4, 8, "BC1" },
    { 72, SDL_GPU_TEXTUREFORMAT_BC1_RGBA_UNORM, 4, 4, 8, "BC1 sRGB" },
    { 74, SDL_GPU_TEXTUREFORMAT_BC2_RGBA_UNORM, 4, 4, 16, "BC2" },
    { 75, SDL_GPU_TEXTUREFORMAT_BC2_RGBA_UNORM, 4, 4, 16, "BC2 sRGB" },
    { 77, SDL_GPU_TEXTUREFORMAT_BC3_RGBA_UNORM, 4, 4, 16, "BC3" },
    { 78, SDL_GPU_TEXTUREFORMAT_BC3_RGBA_UNORM, 4, 4, 16, "BC3 sRGB" },
    { 80, SDL_GPU_TEXTUREFORMAT_BC4_R_UNORM, 4, 4, 8, "BC4" },
    { 83, SDL_GPU_TEXTUREFORMAT_BC5_RG_UNORM, 4, 4, 16, "BC5" },
    { 95, SDL_GPU_TEXTUREFORMAT_BC6H_RGB_UFLOAT, 4, 4, 16, "BC6H unsigned" },
    { 96, SDL_GPU_TEXTUREFORMAT_BC6H_RGB_FLOAT, 4, 4, 16, "BC6H signed" },
    { 98, SDL_GPU_TEXTUREFORMAT_BC7_RGBA_UNORM, 4, 4, 16, "BC7" },
    { 99, SDL_GPU_TEXTUREFORMAT_BC7_RGBA_UNORM, 4, 4, 16, "BC7 sRGB" },
};

static const br_block_format br_gl_formats[] = {
    { 0x8058, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, 1, 1, 4, "RGBA8" },
    { 0x8C43, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, 1, 1, 4, "RGBA8 sRGB" },
    { 0x83F0, SDL_GPU_TEXTUREFORMAT_BC1_RGBA_UNORM, 4, 4, 8, "BC1 RGB" },
    { 0x83F1, SDL_GPU_TEXTUREFORMAT_BC1_RGBA_UNORM, 4, 4, 8, "BC1" },
    { 0x83F2, SDL_GPU_TEXTUREFORMAT_BC2_RGBA_UNORM, 4, 4, 16, "BC2" },
    { 0x83F3, SDL_GPU_TEXTUREFORMAT_BC3_RGBA_UNORM, 4, 4, 16, "BC3" },
    { 0x8C4C, SDL_GPU_TEXTUREFORMAT_BC1_RGBA_UNORM, 4, 4, 8, "BC1 RGB sRGB" },
    { 0x8C4D, SDL_GPU_TEXTUREFORMAT_BC1_RGBA_UNORM, 4, 4, 8, "BC1 sRGB" },
    { 0x8C4E, SDL_GPU_TEXTUREFORMAT_BC2_RGBA_UNORM, 4, 4, 16, "BC2 sRGB" },
    { 0x8C4F, SDL_GPU_TEXTUREFORMAT_BC3_RGBA_UNORM, 4, 4, 16, "BC3 sRGB" },
    { 0x8DBB, SDL_GPU_TEXTUREFORMAT_BC4_R_UNORM, 4, 4, 8, "BC4" },
    { 0x8DBD, SDL_GPU_TEXTUREFORMAT_BC5_RG_UNORM, 4, 4, 16, "BC5" },
    { 0x8E8C, SDL_GPU_TEXTUREFORMAT_BC7_RGBA_UNORM, 4, 4, 16, "BC7" },
    { 0x8E8D, SDL_GPU_TEXTUREFORMAT_BC7_RGBA_UNORM, 4, 4, 16, "BC7 sRGB" },
    { 0x8E8E, SDL_GPU_TEXTUREFORMAT_BC6H_RGB_FLOAT, 4, 4, 16, "BC6H signed" },
    { 0x8E8F, SDL_GPU_TEXTUREFORMAT_BC6H_RGB_UFLOAT, 4, 4, 16, "BC6H unsigned" },
    { 0x93B0, SDL_GPU_TEXTUREFORMAT_ASTC_4x4_UNORM, 4, 4, 16, "ASTC 4x4" },
    { 0x93B1, SDL_GPU_TEXTUREFORMAT_ASTC_5x4_UNORM, 5, 4, 16, "ASTC 5x4" },
    { 0x93B2, SDL_GPU_TEXTUREFORMAT_ASTC_5x5_UNORM, 5, 5, 16, "ASTC 5x5" },
    { 0x93B3, SDL_GPU_TEXTUREFORMAT_ASTC_6x5_UNORM, 6, 5, 16, "ASTC 6x5" },
    { 0x93B4, SDL_GPU_TEXTUREFORMAT_ASTC_6x6_UNORM, 6, 6, 16, "ASTC 6x6" },
    { 0x93B5, SDL_GPU_TEXTUREFORMAT_ASTC_8x5_UNORM, 8, 5, 16, "ASTC 8x5" },
    { 0x93B6, SDL_GPU_TEXTUREFORMAT_ASTC_8x6_UNORM, 8, 6, 16, "ASTC 8x6" },
    { 0x93B7, SDL_GPU_TEXTUREFORMAT_ASTC_8x8_UNORM, 8, 8, 16, "ASTC 8x8" },
    { 0x93B8, SDL_GPU_TEXTUREFORMAT_ASTC_10x5_UNORM, 10, 5, 16, "ASTC 10x5" },
    { 0x93B9, SDL_GPU_TEXTUREFORMAT_ASTC_10x6_UNORM, 10, 6, 16, "ASTC 10x6" },
    { 0x93BA, SDL_GPU_TEXTUREFORMAT_ASTC_10x8_UNORM, 10, 8, 16, "ASTC 10x8" },
    { 0x93BB, SDL_GPU_TEXTUREFORMAT_ASTC_10x10_UNORM, 10, 10, 16, "ASTC 10x10" },
    { 0x93BC, SDL_GPU_TEXTUREFORMAT_ASTC_12x10_UNORM, 12, 10, 16, "ASTC 12x10" },
    { 0x93BD, SDL_GPU_TEXTUREFORMAT_ASTC_12x12_UNORM, 12, 12, 16, "ASTC 12x12" },
};

/* What decode hands to finish: the format and all mip levels, each at an aligned offset. */
typedef struct br_compressed_image {
    br_block_format format;
    uint32_t        width;
    uint32_t        height;
    uint32_t        levels;
    uint32_t        level_offset[BR_COMPRESSED_MAX_LEVELS];
    uint32_t        level_size[BR_COMPRESSED_MAX_LEVELS];
    uint32_t        data_size;
    uint8_t        *data;
} br_compressed_image;

/* The asset: a br_texture first, so br_texture_load can read it as a br_texture *. */
typedef struct br_compressed_asset {
    br_texture texture;
    size_t     gpu_bytes;
} br_compressed_asset;

/* Little-endian u32 at byte offset `at` (no unaligned reads). */
static uint32_t br_read_u32(const uint8_t *p, size_t at)
{
    return (uint32_t)p[at] | ((uint32_t)p[at + 1] << 8) | ((uint32_t)p[at + 2] << 16) | ((uint32_t)p[at + 3] << 24);
}

static uint32_t br_fourcc(const char *s)
{
    return (uint32_t)(uint8_t)s[0] | ((uint32_t)(uint8_t)s[1] << 8) | ((uint32_t)(uint8_t)s[2] << 16) |
           ((uint32_t)(uint8_t)s[3] << 24);
}

static const br_block_format *br_find_format(const br_block_format *table, size_t count, uint32_t code)
{
    for (size_t i = 0; i < count; i++) {
        if (table[i].code == code) {
            return &table[i];
        }
    }
    return NULL;
}

static uint32_t br_level_size(const br_block_format *f, uint32_t width, uint32_t height, uint32_t level)
{
    uint32_t w = width >> level;
    uint32_t h = height >> level;
    w = w > 0 ? w : 1;
    h = h > 0 ? h : 1;
    uint32_t blocks_x = (w + f->block_w - 1) / f->block_w;
    uint32_t blocks_y = (h + f->block_h - 1) / f->block_h;
    return blocks_x * blocks_y * f->block_bytes;
}

/* Checks sizes and lays the levels out at aligned offsets. Data is copied in by the caller. */
static br_compressed_image *br_image_create(const char *path, const br_block_format *f, uint32_t width,
                                            uint32_t height, uint32_t levels)
{
    if (width == 0 || height == 0 || width > 16384 || height > 16384) {
        br_log_error("texture '%s': bad size %ux%u", path, width, height);
        return NULL;
    }
    if (width % f->block_w != 0 || height % f->block_h != 0) {
        br_log_error("texture '%s': %ux%u is not a multiple of the %s block size", path, width, height, f->name);
        return NULL;
    }
    if (levels == 0) {
        levels = 1;
    }
    if (levels > BR_COMPRESSED_MAX_LEVELS) {
        levels = BR_COMPRESSED_MAX_LEVELS;
    }
    br_compressed_image *img = br_alloc(sizeof(br_compressed_image));
    if (img == NULL) {
        return NULL;
    }
    img->format = *f;
    img->width = width;
    img->height = height;
    img->levels = levels;
    uint32_t offset = 0;
    for (uint32_t i = 0; i < levels; i++) {
        img->level_offset[i] = offset;
        img->level_size[i] = br_level_size(f, width, height, i);
        offset += (img->level_size[i] + BR_UPLOAD_ALIGN - 1) / BR_UPLOAD_ALIGN * BR_UPLOAD_ALIGN;
    }
    img->data_size = offset;
    img->data = br_alloc(offset);
    if (img->data == NULL) {
        br_free(img);
        return NULL;
    }
    return img;
}

static void br_image_free(br_compressed_image *img)
{
    if (img != NULL) {
        br_free(img->data);
        br_free(img);
    }
}

/* ---- DDS */

#define BR_DDS_HEADER 128        /* "DDS " + 124-byte header */
#define BR_DDS_DX10_HEADER 20
#define BR_DDSD_MIPMAPCOUNT 0x20000u
#define BR_DDPF_FOURCC 0x4u
#define BR_DDPF_RGB 0x40u
#define BR_DDSCAPS2_CUBEMAP 0x200u
#define BR_DDSCAPS2_VOLUME 0x200000u

static br_compressed_image *br_dds_decode(const char *path, const uint8_t *p, size_t size)
{
    if (size < BR_DDS_HEADER || br_read_u32(p, 0) != br_fourcc("DDS ") || br_read_u32(p, 4) != 124) {
        br_log_error("texture '%s': not a DDS file", path);
        return NULL;
    }
    uint32_t flags = br_read_u32(p, 8);
    uint32_t height = br_read_u32(p, 12);
    uint32_t width = br_read_u32(p, 16);
    uint32_t mips = (flags & BR_DDSD_MIPMAPCOUNT) != 0 ? br_read_u32(p, 28) : 1;
    uint32_t pf_flags = br_read_u32(p, 80);
    uint32_t fourcc = br_read_u32(p, 84);
    uint32_t caps2 = br_read_u32(p, 112);
    size_t data_start = BR_DDS_HEADER;
    if ((caps2 & (BR_DDSCAPS2_CUBEMAP | BR_DDSCAPS2_VOLUME)) != 0) {
        br_log_error("texture '%s': cube maps and volume DDS files are not supported (2D only)", path);
        return NULL;
    }
    const br_block_format *f = NULL;
    if ((pf_flags & BR_DDPF_FOURCC) != 0) {
        if (fourcc == br_fourcc("DX10")) {
            if (size < BR_DDS_HEADER + BR_DDS_DX10_HEADER) {
                br_log_error("texture '%s': truncated DX10 header", path);
                return NULL;
            }
            uint32_t dxgi = br_read_u32(p, 128);
            uint32_t dimension = br_read_u32(p, 132);
            uint32_t array_size = br_read_u32(p, 140);
            data_start += BR_DDS_DX10_HEADER;
            if (dimension != 3 || array_size > 1) {
                br_log_error("texture '%s': only single 2D DX10 textures are supported", path);
                return NULL;
            }
            f = br_find_format(br_dxgi_formats, BR_ARRAY_COUNT(br_dxgi_formats), dxgi);
            if (f == NULL) {
                br_log_error("texture '%s': DXGI format %u is not supported", path, dxgi);
                return NULL;
            }
        } else {
            uint32_t dxgi = 0;
            if (fourcc == br_fourcc("DXT1")) {
                dxgi = 71;
            } else if (fourcc == br_fourcc("DXT2") || fourcc == br_fourcc("DXT3")) {
                dxgi = 74;
            } else if (fourcc == br_fourcc("DXT4") || fourcc == br_fourcc("DXT5")) {
                dxgi = 77;
            } else if (fourcc == br_fourcc("ATI1") || fourcc == br_fourcc("BC4U")) {
                dxgi = 80;
            } else if (fourcc == br_fourcc("ATI2") || fourcc == br_fourcc("BC5U")) {
                dxgi = 83;
            }
            f = br_find_format(br_dxgi_formats, BR_ARRAY_COUNT(br_dxgi_formats), dxgi);
            if (f == NULL) {
                br_log_error("texture '%s': DDS FourCC '%c%c%c%c' is not supported", path, p[84], p[85], p[86],
                             p[87]);
                return NULL;
            }
        }
    } else if ((pf_flags & BR_DDPF_RGB) != 0 && br_read_u32(p, 88) == 32) {
        uint32_t rmask = br_read_u32(p, 92);
        uint32_t dxgi = rmask == 0x000000ffu ? 28 : rmask == 0x00ff0000u ? 87 : 0;
        f = br_find_format(br_dxgi_formats, BR_ARRAY_COUNT(br_dxgi_formats), dxgi);
    }
    if (f == NULL) {
        br_log_error("texture '%s': unsupported DDS pixel format", path);
        return NULL;
    }
    br_compressed_image *img = br_image_create(path, f, width, height, mips);
    if (img == NULL) {
        return NULL;
    }
    size_t at = data_start;
    for (uint32_t i = 0; i < img->levels; i++) {
        if (at + img->level_size[i] > size) {
            br_log_error("texture '%s': truncated (mip %u of %u)", path, i, img->levels);
            br_image_free(img);
            return NULL;
        }
        SDL_memcpy(img->data + img->level_offset[i], p + at, img->level_size[i]);
        at += img->level_size[i];
    }
    return img;
}

/* ---- KTX 1 */

#define BR_KTX_HEADER 64

static br_compressed_image *br_ktx_decode(const char *path, const uint8_t *p, size_t size)
{
    static const uint8_t magic[12] = { 0xAB, 0x4B, 0x54, 0x58, 0x20, 0x31, 0x31, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A };
    if (size < BR_KTX_HEADER || SDL_memcmp(p, magic, sizeof(magic)) != 0) {
        br_log_error("texture '%s': not a KTX 1 file (KTX2 is not supported)", path);
        return NULL;
    }
    if (br_read_u32(p, 12) != 0x04030201u) {
        br_log_error("texture '%s': big-endian KTX files are not supported", path);
        return NULL;
    }
    uint32_t internal_format = br_read_u32(p, 28);
    uint32_t width = br_read_u32(p, 36);
    uint32_t height = br_read_u32(p, 40);
    uint32_t depth = br_read_u32(p, 44);
    uint32_t array_elements = br_read_u32(p, 48);
    uint32_t faces = br_read_u32(p, 52);
    uint32_t mips = br_read_u32(p, 56);
    uint32_t kv_bytes = br_read_u32(p, 60);
    if (depth > 1 || array_elements > 1 || faces != 1) {
        br_log_error("texture '%s': only 2D KTX textures are supported", path);
        return NULL;
    }
    const br_block_format *f = br_find_format(br_gl_formats, BR_ARRAY_COUNT(br_gl_formats), internal_format);
    if (f == NULL) {
        bool etc = internal_format == 0x8D64 || (internal_format >= 0x9270 && internal_format <= 0x9279);
        br_log_error("texture '%s': KTX internal format 0x%04X is not supported%s", path, internal_format,
                     etc ? " (ETC1/ETC2/EAC: SDL_GPU has no such formats)" : "");
        return NULL;
    }
    br_compressed_image *img = br_image_create(path, f, width, height, mips);
    if (img == NULL) {
        return NULL;
    }
    size_t at = (size_t)BR_KTX_HEADER + kv_bytes;
    for (uint32_t i = 0; i < img->levels; i++) {
        if (at + 4 > size || br_read_u32(p, at) < img->level_size[i] || at + 4 + img->level_size[i] > size) {
            br_log_error("texture '%s': truncated or bad size (mip %u of %u)", path, i, img->levels);
            br_image_free(img);
            return NULL;
        }
        uint32_t image_size = br_read_u32(p, at);
        SDL_memcpy(img->data + img->level_offset[i], p + at + 4, img->level_size[i]);
        at += 4 + ((size_t)image_size + 3) / 4 * 4; /* mipPadding */
    }
    return img;
}

/* ---- loader callbacks */

static void *br_compressed_decode(br_engine *e, const char *path, const void *bytes, size_t size)
{
    (void)e;
    const char *ext = br_path_extension(path);
    if (SDL_strcasecmp(ext, "ktx") == 0) {
        return br_ktx_decode(path, bytes, size);
    }
    return br_dds_decode(path, bytes, size);
}

/* Creates the GPU texture and uploads every level in one copy pass. */
static br_gpu_texture *br_compressed_upload(br_gpu *g, const char *path, const br_compressed_image *img)
{
    SDL_GPUDevice *device = br_gpu_device(g);
    SDL_GPUTextureCreateInfo info;
    SDL_zero(info);
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = img->format.format;
    info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width = img->width;
    info.height = img->height;
    info.layer_count_or_depth = 1;
    info.num_levels = img->levels;
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    SDL_GPUTexture *handle = SDL_CreateGPUTexture(device, &info);
    if (handle == NULL) {
        br_log_error("texture '%s': cannot create the %s texture: %s", path, img->format.name, SDL_GetError());
        return NULL;
    }
    SDL_GPUTransferBufferCreateInfo tinfo;
    SDL_zero(tinfo);
    tinfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tinfo.size = img->data_size;
    SDL_GPUTransferBuffer *transfer = SDL_CreateGPUTransferBuffer(device, &tinfo);
    void *mapped = transfer != NULL ? SDL_MapGPUTransferBuffer(device, transfer, false) : NULL;
    SDL_GPUCommandBuffer *cmd = mapped != NULL ? SDL_AcquireGPUCommandBuffer(device) : NULL;
    if (cmd == NULL) {
        br_log_error("texture '%s': upload failed: %s", path, SDL_GetError());
        if (mapped != NULL) {
            SDL_UnmapGPUTransferBuffer(device, transfer);
        }
        SDL_ReleaseGPUTransferBuffer(device, transfer);
        SDL_ReleaseGPUTexture(device, handle);
        return NULL;
    }
    SDL_memcpy(mapped, img->data, img->data_size);
    SDL_UnmapGPUTransferBuffer(device, transfer);
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
    for (uint32_t i = 0; i < img->levels; i++) {
        SDL_GPUTextureTransferInfo src;
        SDL_zero(src);
        src.transfer_buffer = transfer;
        src.offset = img->level_offset[i];
        SDL_GPUTextureRegion dst;
        SDL_zero(dst);
        dst.texture = handle;
        dst.mip_level = i;
        dst.w = img->width >> i > 0 ? img->width >> i : 1;
        dst.h = img->height >> i > 0 ? img->height >> i : 1;
        dst.d = 1;
        SDL_UploadToGPUTexture(copy, &src, &dst, false);
    }
    SDL_EndGPUCopyPass(copy);
    SDL_SubmitGPUCommandBuffer(cmd);
    SDL_ReleaseGPUTransferBuffer(device, transfer);
    br_gpu_texture *t = br_alloc(sizeof(br_gpu_texture));
    if (t == NULL) {
        SDL_ReleaseGPUTexture(device, handle);
        return NULL;
    }
    t->handle = handle;
    t->width = img->width;
    t->height = img->height;
    t->mip_levels = img->levels;
    t->format = img->format.format;
    t->usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    t->sample_count = SDL_GPU_SAMPLECOUNT_1;
    return t;
}

static void *br_compressed_finish(br_engine *e, const char *path, void *decoded)
{
    br_compressed_image *img = decoded;
    br_texture t = { 0, 0 };
    br_gpu *g = br_engine_gpu(e);
    size_t gpu_bytes = 0;
    for (uint32_t i = 0; i < img->levels; i++) {
        gpu_bytes += img->level_size[i];
    }
    if (g == NULL) {
        t = br_texture_create_rgba8(e, img->width, img->height, NULL, NULL); /* headless: size only */
    } else if (!SDL_GPUTextureSupportsFormat(br_gpu_device(g), img->format.format, SDL_GPU_TEXTURETYPE_2D,
                                             SDL_GPU_TEXTUREUSAGE_SAMPLER)) {
        br_log_error("texture '%s': this GPU (%s driver) cannot sample %s textures", path, br_gpu_driver_name(g),
                     img->format.name);
    } else {
        br_gpu_texture *gpu = br_compressed_upload(g, path, img);
        if (gpu != NULL) {
            t = br_texture_wrap_gpu(e, gpu, true);
            if (t.generation == 0) {
                br_gpu_texture_destroy(g, gpu);
            }
        }
    }
    uint32_t levels = img->levels;
    br_image_free(img);
    if (t.generation == 0) {
        return NULL;
    }
    if (levels > 1) {
        br_texture_style style;
        SDL_zero(style);
        style.mipmaps = true;
        br_texture_set_style(e, t, &style);
    }
    br_compressed_asset *asset = br_alloc(sizeof(br_compressed_asset));
    if (asset == NULL) {
        br_texture_destroy(e, t);
        return NULL;
    }
    asset->texture = t;
    asset->gpu_bytes = gpu_bytes;
    return asset;
}

static void br_compressed_unload(br_engine *e, void *asset)
{
    br_compressed_asset *a = asset;
    br_texture_destroy(e, a->texture);
    br_free(a);
}

static size_t br_compressed_byte_size(br_engine *e, const void *asset)
{
    (void)e;
    const br_compressed_asset *a = asset;
    return a->gpu_bytes;
}

void br_assets_register_compressed_loader(br_engine *e)
{
    br_asset_loader_desc desc;
    SDL_zero(desc);
    desc.type_name = "texture";
    desc.extensions = "dds,ktx";
    desc.decode = br_compressed_decode;
    desc.finish = br_compressed_finish;
    desc.unload = br_compressed_unload;
    desc.byte_size = br_compressed_byte_size;
    br_assets_register_loader(e, &desc);
}
