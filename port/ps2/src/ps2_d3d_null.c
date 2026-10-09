/*
PS2_D3D_NULL.C

A Direct3D 8 that draws nothing, for bringing the PS2 build up: the game's
Xbox rasterizer (source/rasterizer/xbox) runs on it until the PS2's own
renderer replaces that rasterizer (docs/ps2_port_plan.md, "Renderer").

What the game can observe behaves as the Linux build's Direct3D
(port/linux/src/d3d8_gl.c, d3d8_resources.c), which this file follows:
resources keep their Xbox headers (Common with its reference count, Data,
Format, Size) and the memory the game locks and writes, laid out as the
Xbox lays it out; render and texture stage states are kept in
D3D__RenderState and D3D__TextureState (ps2_xdk_data.c); the vertical blank
callback is called at each of the PS2's vertical blanks (ps2_vblank.c).
Drawing, clearing and presenting do nothing, and every visibility test
reports nothing visible.

The PS2 has no physical address window (the Xbox's 0x80000000 + P): a
resource's Data holds the address of its memory.
*/

#include "platform.h"
#include "ps2_vblank.h"

#include <malloc.h>
#include <stdlib.h>
#include <string.h>

#define VERTEX_SHADER_SIGNATURE 0x76736864UL /* 'vshd', as d3d8_gl.c */
#define BACK_BUFFER_WIDTH 640
#define BACK_BUFFER_HEIGHT 480

/* ---------- texture layout (port/linux/src/xbox_textures.c) */

struct format_information
{
	unsigned char bytes; /* per texel; per 4x4 block for DXT */
	unsigned char linear;
	unsigned char compressed;
};

static struct format_information format_information(DWORD format)
{
	static const struct format_information table[0x42] =
	{
		[0x00] = { 1, 0, 0 }, [0x01] = { 1, 0, 0 }, [0x02] = { 2, 0, 0 }, [0x03] = { 2, 0, 0 },
		[0x04] = { 2, 0, 0 }, [0x05] = { 2, 0, 0 }, [0x06] = { 4, 0, 0 }, [0x07] = { 4, 0, 0 },
		[0x0b] = { 1, 0, 0 }, [0x0c] = { 8, 0, 1 }, [0x0e] = { 16, 0, 1 }, [0x0f] = { 16, 0, 1 },
		[0x10] = { 2, 1, 0 }, [0x11] = { 2, 1, 0 }, [0x12] = { 4, 1, 0 }, [0x13] = { 1, 1, 0 },
		[0x16] = { 2, 1, 0 }, [0x17] = { 2, 1, 0 }, [0x19] = { 1, 0, 0 }, [0x1a] = { 2, 0, 0 },
		[0x1b] = { 1, 1, 0 }, [0x1c] = { 2, 1, 0 }, [0x1d] = { 2, 1, 0 }, [0x1e] = { 4, 1, 0 },
		[0x1f] = { 1, 1, 0 }, [0x20] = { 2, 1, 0 }, [0x24] = { 2, 1, 0 }, [0x25] = { 2, 1, 0 },
		[0x27] = { 2, 0, 0 }, [0x28] = { 2, 0, 0 }, [0x29] = { 2, 0, 0 }, [0x2a] = { 4, 0, 0 },
		[0x2b] = { 4, 0, 0 }, [0x2c] = { 2, 0, 0 }, [0x2d] = { 2, 0, 0 }, [0x2e] = { 4, 1, 0 },
		[0x2f] = { 4, 1, 0 }, [0x30] = { 2, 1, 0 }, [0x31] = { 2, 1, 0 }, [0x32] = { 2, 0, 0 },
		[0x33] = { 4, 0, 0 }, [0x35] = { 2, 1, 0 }, [0x36] = { 4, 1, 0 }, [0x37] = { 2, 1, 0 },
		[0x38] = { 2, 0, 0 }, [0x39] = { 2, 0, 0 }, [0x3a] = { 4, 0, 0 }, [0x3b] = { 4, 0, 0 },
		[0x3c] = { 4, 0, 0 }, [0x3d] = { 2, 1, 0 }, [0x3e] = { 2, 1, 0 }, [0x3f] = { 4, 1, 0 },
		[0x40] = { 4, 1, 0 }, [0x41] = { 4, 1, 0 },
	};
	struct format_information unknown = { 4, 0, 0 };

	if (format < sizeof(table) / sizeof(table[0]) && table[format].bytes)
		return table[format];
	return unknown;
}

struct texture_description
{
	unsigned long format, width, height, depth, levels, pitch;
	BOOL linear, compressed, cube_map;
};

static unsigned long floor_log2(unsigned long value)
{
	unsigned long result = 0;

	while (value > 1)
	{
		value >>= 1;
		result++;
	}
	return result;
}

static unsigned long level_dimension(unsigned long base, unsigned long level)
{
	unsigned long value = base >> level;

	return value ? value : 1;
}

static void texture_describe(DWORD format_word, DWORD size_word, struct texture_description *description)
{
	struct format_information information;

	memset(description, 0, sizeof(*description));
	description->format = (format_word & D3DFORMAT_FORMAT_MASK) >> D3DFORMAT_FORMAT_SHIFT;
	information = format_information(description->format);
	description->cube_map = (format_word & D3DFORMAT_CUBEMAP) != 0;
	description->compressed = information.compressed;
	if (size_word)
	{
		description->width = (size_word & D3DSIZE_WIDTH_MASK) + 1;
		description->height = ((size_word & D3DSIZE_HEIGHT_MASK) >> D3DSIZE_HEIGHT_SHIFT) + 1;
		description->depth = 1;
		description->levels = 1;
		description->pitch = (((size_word & D3DSIZE_PITCH_MASK) >> D3DSIZE_PITCH_SHIFT) + 1) * D3DTEXTURE_PITCH_ALIGNMENT;
		description->linear = TRUE;
	}
	else
	{
		description->width = 1UL << ((format_word & D3DFORMAT_USIZE_MASK) >> D3DFORMAT_USIZE_SHIFT);
		description->height = 1UL << ((format_word & D3DFORMAT_VSIZE_MASK) >> D3DFORMAT_VSIZE_SHIFT);
		description->depth = 1UL << ((format_word & D3DFORMAT_PSIZE_MASK) >> D3DFORMAT_PSIZE_SHIFT);
		description->levels = (format_word & D3DFORMAT_MIPMAP_MASK) >> D3DFORMAT_MIPMAP_SHIFT;
		if (!description->levels)
			description->levels = 1;
		description->linear = information.linear;
		description->pitch = description->width * information.bytes;
	}
	if ((format_word & D3DFORMAT_DIMENSION_MASK) >> D3DFORMAT_DIMENSION_SHIFT != 3)
		description->depth = 1;
}

static unsigned long level_bytes(const struct texture_description *description, unsigned long level)
{
	unsigned long bytes = format_information(description->format).bytes;
	unsigned long width = level_dimension(description->width, level);
	unsigned long height = level_dimension(description->height, level);
	unsigned long depth = level_dimension(description->depth, level);

	if (description->compressed)
		return ((width + 3) / 4) * ((height + 3) / 4) * bytes * depth;
	if (description->linear)
		return description->pitch * height;
	return width * height * depth * bytes;
}

static unsigned long level_offset(const struct texture_description *description, unsigned long level)
{
	unsigned long offset = 0;
	unsigned long index;

	for (index = 0; index < level && index < description->levels; index++)
		offset += level_bytes(description, index);
	return offset;
}

static unsigned long face_size(const struct texture_description *description)
{
	unsigned long size = level_offset(description, description->levels);

	if (description->cube_map)
		size = (size + D3DTEXTURE_CUBEFACE_ALIGNMENT - 1) & ~(unsigned long)(D3DTEXTURE_CUBEFACE_ALIGNMENT - 1);
	return size;
}

static unsigned long level_pitch(const struct texture_description *description, unsigned long level)
{
	unsigned long bytes = format_information(description->format).bytes;

	if (description->linear)
		return description->pitch;
	if (description->compressed)
		return ((level_dimension(description->width, level) + 3) / 4) * bytes;
	return level_dimension(description->width, level) * bytes;
}

static unsigned long bytes_per_texel(D3DFORMAT format)
{
	return format_information((DWORD)format).bytes;
}

static unsigned long linear_pitch(D3DFORMAT format, unsigned long width)
{
	return (width * bytes_per_texel(format) + D3DTEXTURE_PITCH_ALIGNMENT - 1) &
		~(unsigned long)(D3DTEXTURE_PITCH_ALIGNMENT - 1);
}

static void *allocate_resource_memory(unsigned long size)
{
	void *memory = memalign(D3DTEXTURE_ALIGNMENT, size ? size : 1);

	if (memory)
		memset(memory, 0, size ? size : 1);
	else
		platform_log("Direct3D: out of memory for a %lu byte resource", size);
	return memory;
}

/* ---------- the device */

struct vertex_shader_object
{
	unsigned long signature;
	unsigned long instruction_count;
};

static struct
{
	BOOL created;
	D3DSurface back_buffer;
	D3DSurface depth_buffer;
	D3DSurface *render_target;
	D3DSurface *depth_stencil;
	D3DMATRIX transforms[D3DTS_MAX];
	D3DVIEWPORT8 viewport;
	D3DPRESENT_PARAMETERS presentation;
} device;

static void surface_initialize(D3DSurface *surface, D3DFORMAT format, unsigned long width, unsigned long height)
{
	unsigned long pitch = linear_pitch(format, width);
	void *memory = allocate_resource_memory(pitch * height);

	memset(surface, 0, sizeof(*surface));
	/* owned by the device: without D3DCOMMON_D3DCREATED, Release never frees it */
	surface->Common = D3DCOMMON_TYPE_SURFACE | 1;
	surface->Data = (DWORD)memory;
	surface->Format = ((DWORD)format << D3DFORMAT_FORMAT_SHIFT) | (2 << D3DFORMAT_DIMENSION_SHIFT) | D3DFORMAT_DMACHANNEL_A;
	surface->Size = ((pitch / D3DTEXTURE_PITCH_ALIGNMENT - 1) << D3DSIZE_PITCH_SHIFT) |
		((height - 1) << D3DSIZE_HEIGHT_SHIFT) | (width - 1);
}

Direct3D *WINAPI Direct3DCreate8(UINT sdk_version)
{
	(void)sdk_version;
	return (Direct3D *)1;
}

void WINAPI Direct3D_SetPushBufferSize(DWORD push_buffer_size, DWORD segment_count)
{
	(void)push_buffer_size;
	(void)segment_count;
}

HRESULT WINAPI Direct3D_CreateDevice(UINT adapter, D3DDEVTYPE device_type, void *unused, DWORD behavior_flags,
	D3DPRESENT_PARAMETERS *presentation_parameters, D3DDevice **returned_device)
{
	unsigned long width, height;
	int index;

	(void)adapter;
	(void)device_type;
	(void)unused;
	(void)behavior_flags;
	if (!device.created)
	{
		memset(&device, 0, sizeof(device));
		if (presentation_parameters)
			device.presentation = *presentation_parameters;
		width = device.presentation.BackBufferWidth ? device.presentation.BackBufferWidth : BACK_BUFFER_WIDTH;
		height = device.presentation.BackBufferHeight ? device.presentation.BackBufferHeight : BACK_BUFFER_HEIGHT;
		surface_initialize(&device.back_buffer, D3DFMT_LIN_A8R8G8B8, width, height);
		surface_initialize(&device.depth_buffer, D3DFMT_LIN_D24S8, width, height);
		device.render_target = &device.back_buffer;
		device.depth_stencil = &device.depth_buffer;
		for (index = 0; index < D3DTS_MAX; index++)
		{
			device.transforms[index]._11 = 1.0f;
			device.transforms[index]._22 = 1.0f;
			device.transforms[index]._33 = 1.0f;
			device.transforms[index]._44 = 1.0f;
		}
		device.viewport.Width = width;
		device.viewport.Height = height;
		device.viewport.MaxZ = 1.0f;
		D3D__RenderState[D3DRS_ZENABLE] = TRUE;
		D3D__RenderState[D3DRS_ZWRITEENABLE] = TRUE;
		D3D__RenderState[D3DRS_ZFUNC] = D3DCMP_LESSEQUAL;
		D3D__RenderState[D3DRS_COLORWRITEENABLE] = D3DCOLORWRITEENABLE_ALL;
		D3D__RenderState[D3DRS_SRCBLEND] = D3DBLEND_ONE;
		D3D__RenderState[D3DRS_DESTBLEND] = D3DBLEND_ZERO;
		D3D__RenderState[D3DRS_BLENDOP] = D3DBLENDOP_ADD;
		D3D__RenderState[D3DRS_CULLMODE] = D3DCULL_CCW;
		D3D__RenderState[D3DRS_FRONTFACE] = D3DFRONT_CW;
		D3D__RenderState[D3DRS_FILLMODE] = D3DFILL_SOLID;
		D3D__RenderState[D3DRS_ALPHAFUNC] = D3DCMP_ALWAYS;
		D3D__RenderState[D3DRS_STENCILFUNC] = D3DCMP_ALWAYS;
		D3D__RenderState[D3DRS_STENCILMASK] = 0xff;
		D3D__RenderState[D3DRS_STENCILWRITEMASK] = 0xff;
		D3D__RenderState[D3DRS_STENCILFAIL] = D3DSTENCILOP_KEEP;
		D3D__RenderState[D3DRS_STENCILZFAIL] = D3DSTENCILOP_KEEP;
		D3D__RenderState[D3DRS_STENCILPASS] = D3DSTENCILOP_KEEP;
		for (index = 0; index < D3DTSS_MAXSTAGES; index++)
		{
			D3D__TextureState[index][D3DTSS_ADDRESSU] = D3DTADDRESS_WRAP;
			D3D__TextureState[index][D3DTSS_ADDRESSV] = D3DTADDRESS_WRAP;
			D3D__TextureState[index][D3DTSS_ADDRESSW] = D3DTADDRESS_WRAP;
			D3D__TextureState[index][D3DTSS_MAGFILTER] = D3DTEXF_POINT;
			D3D__TextureState[index][D3DTSS_MINFILTER] = D3DTEXF_POINT;
			D3D__TextureState[index][D3DTSS_MAXANISOTROPY] = 1;
		}
		platform_log("Direct3D: the PS2's null device (nothing is drawn yet)");
		device.created = TRUE;
	}
	*returned_device = (D3DDevice *)&device;
	return S_OK;
}

ULONG WINAPI D3DDevice_Release(void)
{
	return 1;
}

void WINAPI D3DDevice_GetDeviceCaps(D3DCAPS8 *caps)
{
	/* as d3d8_gl.c reports the Xbox's */
	memset(caps, 0, sizeof(*caps));
	caps->DeviceType = D3DDEVTYPE_HAL;
	caps->MaxTextureWidth = 4096;
	caps->MaxTextureHeight = 4096;
	caps->MaxVolumeExtent = 512;
	caps->MaxTextureRepeat = 8192;
	caps->MaxTextureAspectRatio = 4096;
	caps->MaxAnisotropy = 4;
	caps->MaxTextureBlendStages = 4;
	caps->MaxSimultaneousTextures = 4;
	caps->MaxActiveLights = 8;
	caps->MaxVertexBlendMatrices = 4;
	caps->MaxPointSize = 64.0f;
	caps->MaxPrimitiveCount = 0xfffff;
	caps->MaxVertexIndex = 0xffff;
	caps->MaxStreams = 16;
	caps->MaxStreamStride = 255;
	caps->VertexShaderVersion = D3DVS_VERSION(1, 1);
	caps->MaxVertexShaderConst = 192;
	caps->PixelShaderVersion = D3DPS_VERSION(1, 1);
	caps->MaxPixelShaderValue = 1.0f;
}

void WINAPI D3DDevice_GetBackBuffer(INT back_buffer, D3DBACKBUFFER_TYPE type, D3DSurface **result)
{
	(void)back_buffer;
	(void)type;
	/* like Direct3D, the caller gets a reference it must release */
	device.back_buffer.Common++;
	*result = &device.back_buffer;
}

HRESULT WINAPI D3DDevice_GetDepthStencilSurface(D3DSurface **result)
{
	*result = device.depth_stencil;
	if (!*result)
		return D3DERR_NOTFOUND;
	(*result)->Common++;
	return S_OK;
}

void WINAPI D3DDevice_SetRenderTarget(D3DSurface *render_target, D3DSurface *depth_stencil)
{
	if (render_target)
		device.render_target = render_target;
	device.depth_stencil = depth_stencil;
}

void WINAPI D3DDevice_SetTransform(D3DTRANSFORMSTATETYPE state, CONST D3DMATRIX *matrix)
{
	if ((unsigned long)state < D3DTS_MAX && matrix)
		device.transforms[state] = *matrix;
}

void WINAPI D3DDevice_GetTransform(D3DTRANSFORMSTATETYPE state, D3DMATRIX *matrix)
{
	if ((unsigned long)state < D3DTS_MAX && matrix)
		*matrix = device.transforms[state];
}

void WINAPI D3DDevice_SetViewport(CONST D3DVIEWPORT8 *viewport)
{
	if (viewport)
		device.viewport = *viewport;
}

/* ---------- the vertical blank and presenting */

void WINAPI D3DDevice_SetVerticalBlankCallback(D3DCALLBACK callback)
{
	ps2_vblank_set_callback((ps2_vblank_callback)callback);
}

void WINAPI D3DDevice_BlockUntilVerticalBlank(void)
{
	ps2_vblank_wait();
}

void WINAPI D3DDevice_Present(CONST RECT *source_rectangle, CONST RECT *destination_rectangle, void *unused,
	void *unused2)
{
	(void)source_rectangle;
	(void)destination_rectangle;
	(void)unused;
	(void)unused2;
	ps2_vblank_present();
}

volatile unsigned int *d3d_find_flipcount(void)
{
	return ps2_vblank_flip_count();
}

HRESULT WINAPI D3DDevice_PersistDisplay(void)
{
	return S_OK;
}

void WINAPI D3DDevice_SetFlickerFilter(DWORD filter)
{
	(void)filter;
}

void WINAPI D3DDevice_SetSoftDisplayFilter(BOOL enable)
{
	(void)enable;
}

BOOL WINAPI D3DDevice_IsBusy(void)
{
	return FALSE;
}

void WINAPI D3DDevice_KickPushBuffer(void)
{
}

void WINAPI D3DDevice_InsertCallback(D3DCALLBACKTYPE type, D3DCALLBACK callback, DWORD context)
{
	(void)type;
	/* the "GPU" reaches the callback immediately, as on Linux */
	if (callback)
		callback(context);
}

/* ---------- drawing (nothing is drawn) */

void WINAPI D3DDevice_Clear(DWORD count, CONST D3DRECT *rectangles, DWORD flags, D3DCOLOR color, float z,
	DWORD stencil)
{
	(void)count;
	(void)rectangles;
	(void)flags;
	(void)color;
	(void)z;
	(void)stencil;
}

void WINAPI D3DDevice_DrawVertices(D3DPRIMITIVETYPE primitive_type, UINT start_vertex, UINT vertex_count)
{
	(void)primitive_type;
	(void)start_vertex;
	(void)vertex_count;
}

void WINAPI D3DDevice_DrawIndexedVertices(D3DPRIMITIVETYPE primitive_type, UINT vertex_count, CONST WORD *index_data)
{
	(void)primitive_type;
	(void)vertex_count;
	(void)index_data;
}

void WINAPI D3DDevice_Begin(D3DPRIMITIVETYPE primitive_type)
{
	(void)primitive_type;
}

void WINAPI D3DDevice_End(void)
{
}

void WINAPI D3DDevice_SetVertexData2f(INT reg, FLOAT a, FLOAT b)
{
	(void)reg;
	(void)a;
	(void)b;
}

void WINAPI D3DDevice_SetVertexData2s(INT reg, SHORT a, SHORT b)
{
	(void)reg;
	(void)a;
	(void)b;
}

void WINAPI D3DDevice_SetVertexData4f(INT reg, FLOAT a, FLOAT b, FLOAT c, FLOAT d)
{
	(void)reg;
	(void)a;
	(void)b;
	(void)c;
	(void)d;
}

void WINAPI D3DDevice_SetVertexData4ub(INT reg, BYTE a, BYTE b, BYTE c, BYTE d)
{
	(void)reg;
	(void)a;
	(void)b;
	(void)c;
	(void)d;
}

void WINAPI D3DDevice_SetVertexDataColor(INT reg, D3DCOLOR color)
{
	(void)reg;
	(void)color;
}

void WINAPI D3DDevice_SetStreamSource(UINT stream_number, D3DVertexBuffer *stream_data, UINT stride)
{
	(void)stream_number;
	(void)stream_data;
	(void)stride;
}

void WINAPI D3DDevice_SetIndices(D3DIndexBuffer *index_data, UINT base_vertex_index)
{
	(void)index_data;
	(void)base_vertex_index;
}

void WINAPI D3DDevice_SetTexture(DWORD stage, D3DBaseTexture *texture)
{
	(void)stage;
	(void)texture;
}

void WINAPI D3DDevice_SetPalette(DWORD stage, D3DPalette *palette)
{
	(void)stage;
	(void)palette;
}

void WINAPI D3DDevice_SetPixelShaderProgram(D3DPIXELSHADERDEF *definition)
{
	(void)definition;
}

void WINAPI D3DDevice_SetShaderConstantMode(D3DSHADERCONSTANTMODE mode)
{
	(void)mode;
}

/* ---------- visibility tests: nothing is drawn, so nothing is visible */

void WINAPI D3DDevice_BeginVisibilityTest(void)
{
}

HRESULT WINAPI D3DDevice_EndVisibilityTest(DWORD index)
{
	(void)index;
	return S_OK;
}

HRESULT WINAPI D3DDevice_GetVisibilityTestResult(DWORD index, UINT *result, ULONGLONG *time_stamp)
{
	(void)index;
	if (result)
		*result = 0;
	if (time_stamp)
		*time_stamp = 0;
	return S_OK;
}

/* ---------- vertex shaders: objects the game can ask the size of */

static struct vertex_shader_object *vertex_shader_from_handle(DWORD handle)
{
	struct vertex_shader_object *object = (struct vertex_shader_object *)handle;

	/* odd values are FVF codes; programmable shader handles are even */
	if (!handle || (handle & 1) || object->signature != VERTEX_SHADER_SIGNATURE)
		return NULL;
	return object;
}

HRESULT WINAPI D3DDevice_CreateVertexShader(CONST DWORD *declaration, CONST DWORD *function, DWORD *handle, DWORD usage)
{
	struct vertex_shader_object *object = calloc(1, sizeof(*object));

	(void)declaration;
	(void)usage;
	if (!object)
		return E_OUTOFMEMORY;
	object->signature = VERTEX_SHADER_SIGNATURE;
	/* header: program type in the low word, instruction count in the high */
	object->instruction_count = function ? function[0] >> 16 : 0;
	*handle = (DWORD)object;
	return S_OK;
}

void WINAPI D3DDevice_DeleteVertexShader(DWORD handle)
{
	struct vertex_shader_object *object = vertex_shader_from_handle(handle);

	if (object)
	{
		object->signature = 0;
		free(object);
	}
}

void WINAPI D3DDevice_GetVertexShaderSize(DWORD handle, UINT *size)
{
	struct vertex_shader_object *object = vertex_shader_from_handle(handle);

	*size = object ? object->instruction_count : 0;
}

void WINAPI D3DDevice_LoadVertexShader(DWORD handle, DWORD address)
{
	(void)handle;
	(void)address;
}

void WINAPI D3DDevice_SelectVertexShader(DWORD handle, DWORD address)
{
	(void)handle;
	(void)address;
}

void WINAPI D3DDevice_SetVertexShader(DWORD handle)
{
	(void)handle;
}

void WINAPI D3DDevice_SetVertexShaderConstant(INT reg, CONST void *constant_data, DWORD constant_count)
{
	(void)reg;
	(void)constant_data;
	(void)constant_count;
}

/* ---------- render and texture stage states (as d3d8_gl.c keeps them) */

void D3DFASTCALL D3DDevice_SetRenderState_Simple(DWORD method, DWORD value)
{
	/* callers also store the value in D3D__RenderState themselves */
	(void)method;
	(void)value;
}

void D3DFASTCALL D3DDevice_SetRenderState_Deferred(D3DRENDERSTATETYPE state, DWORD value)
{
	if ((unsigned long)state < D3DRS_MAX)
		D3D__RenderState[state] = value;
}

void WINAPI D3DDevice_SetRenderState_ZBias(DWORD value);

void WINAPI D3DDevice_SetRenderStateNotInline(D3DRENDERSTATETYPE state, DWORD value)
{
	if (state == D3DRS_ZBIAS)
		D3DDevice_SetRenderState_ZBias(value);
	else if ((unsigned long)state < D3DRS_MAX)
		D3D__RenderState[state] = value;
}

void WINAPI D3DDevice_SetRenderState_ZBias(DWORD value)
{
	float offset = -(float)value;
	float slope = offset * 0.25f;
	DWORD enable = value != 0;

	memcpy(&D3D__RenderState[D3DRS_POLYGONOFFSETZSLOPESCALE], &slope, sizeof(slope));
	memcpy(&D3D__RenderState[D3DRS_POLYGONOFFSETZOFFSET], &offset, sizeof(offset));
	D3D__RenderState[D3DRS_POINTOFFSETENABLE] = enable;
	D3D__RenderState[D3DRS_WIREFRAMEOFFSETENABLE] = enable;
	D3D__RenderState[D3DRS_SOLIDOFFSETENABLE] = enable;
	D3D__RenderState[D3DRS_ZBIAS] = value;
}

#define COMPLEX_RENDER_STATE(name, state) \
	void WINAPI D3DDevice_SetRenderState_##name(DWORD value) { D3D__RenderState[state] = value; }

COMPLEX_RENDER_STATE(PSTextureModes, D3DRS_PSTEXTUREMODES)
COMPLEX_RENDER_STATE(VertexBlend, D3DRS_VERTEXBLEND)
COMPLEX_RENDER_STATE(FogColor, D3DRS_FOGCOLOR)
COMPLEX_RENDER_STATE(FillMode, D3DRS_FILLMODE)
COMPLEX_RENDER_STATE(BackFillMode, D3DRS_BACKFILLMODE)
COMPLEX_RENDER_STATE(TwoSidedLighting, D3DRS_TWOSIDEDLIGHTING)
COMPLEX_RENDER_STATE(NormalizeNormals, D3DRS_NORMALIZENORMALS)
COMPLEX_RENDER_STATE(ZEnable, D3DRS_ZENABLE)
COMPLEX_RENDER_STATE(StencilEnable, D3DRS_STENCILENABLE)
COMPLEX_RENDER_STATE(StencilFail, D3DRS_STENCILFAIL)
COMPLEX_RENDER_STATE(FrontFace, D3DRS_FRONTFACE)
COMPLEX_RENDER_STATE(CullMode, D3DRS_CULLMODE)
COMPLEX_RENDER_STATE(TextureFactor, D3DRS_TEXTUREFACTOR)
COMPLEX_RENDER_STATE(LogicOp, D3DRS_LOGICOP)
COMPLEX_RENDER_STATE(EdgeAntiAlias, D3DRS_EDGEANTIALIAS)
COMPLEX_RENDER_STATE(MultiSampleAntiAlias, D3DRS_MULTISAMPLEANTIALIAS)
COMPLEX_RENDER_STATE(MultiSampleMask, D3DRS_MULTISAMPLEMASK)
COMPLEX_RENDER_STATE(MultiSampleType, D3DRS_MULTISAMPLETYPE)
COMPLEX_RENDER_STATE(ShadowFunc, D3DRS_SHADOWFUNC)
COMPLEX_RENDER_STATE(LineWidth, D3DRS_LINEWIDTH)
COMPLEX_RENDER_STATE(Dxt1NoiseEnable, D3DRS_DXT1NOISEENABLE)
COMPLEX_RENDER_STATE(YuvEnable, D3DRS_YUVENABLE)
COMPLEX_RENDER_STATE(OcclusionCullEnable, D3DRS_OCCLUSIONCULLENABLE)
COMPLEX_RENDER_STATE(StencilCullEnable, D3DRS_STENCILCULLENABLE)
COMPLEX_RENDER_STATE(RopZCmpAlwaysRead, D3DRS_ROPZCMPALWAYSREAD)
COMPLEX_RENDER_STATE(RopZRead, D3DRS_ROPZREAD)
COMPLEX_RENDER_STATE(DoNotCullUncompressed, D3DRS_DONOTCULLUNCOMPRESSED)

void D3DFASTCALL D3DDevice_SetTextureState_Deferred(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value)
{
	if (stage < D3DTSS_MAXSTAGES && (unsigned long)type < D3DTSS_MAX)
		D3D__TextureState[stage][type] = value;
}

void WINAPI D3DDevice_SetTextureState_TexCoordIndex(DWORD stage, DWORD value)
{
	if (stage < D3DTSS_MAXSTAGES)
		D3D__TextureState[stage][D3DTSS_TEXCOORDINDEX] = value;
}

void WINAPI D3DDevice_SetTextureState_BorderColor(DWORD stage, DWORD value)
{
	if (stage < D3DTSS_MAXSTAGES)
		D3D__TextureState[stage][D3DTSS_BORDERCOLOR] = value;
}

void WINAPI D3DDevice_SetTextureState_ColorKeyColor(DWORD stage, DWORD value)
{
	if (stage < D3DTSS_MAXSTAGES)
		D3D__TextureState[stage][D3DTSS_COLORKEYCOLOR] = value;
}

void WINAPI D3DDevice_SetTextureState_BumpEnv(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value)
{
	if (stage < D3DTSS_MAXSTAGES && (unsigned long)type < D3DTSS_MAX)
		D3D__TextureState[stage][type] = value;
}

/* ---------- resources (as d3d8_resources.c) */

void WINAPI D3DResource_Register(D3DResource *resource, void *base)
{
	/* D3DResource is opaque in C; every resource starts Common, Data, Lock */
	DWORD *fields = (DWORD *)resource;

	fields[1] = (DWORD)base + fields[1];
}

ULONG WINAPI D3DResource_Release(D3DResource *resource)
{
	DWORD *fields = (DWORD *)resource;
	ULONG count = fields[0] & D3DCOMMON_REFCOUNT_MASK;

	if (count)
	{
		count--;
		fields[0] = (fields[0] & ~D3DCOMMON_REFCOUNT_MASK) | count;
	}
	/* resources the game built itself (not D3DCOMMON_D3DCREATED) are never freed here */
	if (!count && (fields[0] & D3DCOMMON_D3DCREATED))
	{
		free((void *)fields[1]);
		free(resource);
	}
	return count;
}

BOOL WINAPI D3DResource_IsBusy(D3DResource *resource)
{
	(void)resource;
	return FALSE;
}

void WINAPI D3DResource_BlockUntilNotBusy(D3DResource *resource)
{
	(void)resource;
}

static HRESULT create_texture(unsigned long width, unsigned long height, unsigned long depth, unsigned long levels,
	D3DFORMAT format, BOOL cube_map, D3DBaseTexture **result)
{
	D3DBaseTexture *texture = calloc(1, sizeof(*texture));
	struct texture_description description;
	unsigned long maximum_levels = floor_log2(width > height ? width : height) + 1;
	void *memory;

	if (!texture)
		return E_OUTOFMEMORY;
	if (!levels || levels > maximum_levels)
		levels = maximum_levels;
	texture->Common = D3DCOMMON_TYPE_TEXTURE | D3DCOMMON_D3DCREATED | 1;
	if (format_information((DWORD)format).linear)
	{
		unsigned long pitch = linear_pitch(format, width);

		texture->Format = ((DWORD)format << D3DFORMAT_FORMAT_SHIFT) | (1 << D3DFORMAT_MIPMAP_SHIFT) |
			(2 << D3DFORMAT_DIMENSION_SHIFT) | D3DFORMAT_DMACHANNEL_A;
		texture->Size = ((pitch / D3DTEXTURE_PITCH_ALIGNMENT - 1) << D3DSIZE_PITCH_SHIFT) |
			((height - 1) << D3DSIZE_HEIGHT_SHIFT) | (width - 1);
	}
	else
	{
		texture->Format = (floor_log2(depth) << D3DFORMAT_PSIZE_SHIFT) |
			(floor_log2(height) << D3DFORMAT_VSIZE_SHIFT) |
			(floor_log2(width) << D3DFORMAT_USIZE_SHIFT) |
			((DWORD)format << D3DFORMAT_FORMAT_SHIFT) |
			(levels << D3DFORMAT_MIPMAP_SHIFT) |
			((depth > 1 ? 3 : 2) << D3DFORMAT_DIMENSION_SHIFT) |
			(cube_map ? D3DFORMAT_CUBEMAP : 0) |
			D3DFORMAT_DMACHANNEL_A;
		texture->Size = 0;
	}
	texture_describe(texture->Format, texture->Size, &description);
	memory = allocate_resource_memory(face_size(&description) * (cube_map ? 6 : 1));
	if (!memory)
	{
		free(texture);
		return E_OUTOFMEMORY;
	}
	texture->Data = (DWORD)memory;
	*result = texture;
	return S_OK;
}

HRESULT WINAPI D3DDevice_CreateTexture(UINT width, UINT height, UINT levels, DWORD usage, D3DFORMAT format,
	D3DPOOL pool, D3DTexture **texture)
{
	(void)usage;
	(void)pool;
	return create_texture(width, height, 1, levels, format, FALSE, (D3DBaseTexture **)texture);
}

HRESULT WINAPI D3DDevice_CreateVolumeTexture(UINT width, UINT height, UINT depth, UINT levels, DWORD usage,
	D3DFORMAT format, D3DPOOL pool, D3DVolumeTexture **texture)
{
	(void)usage;
	(void)pool;
	return create_texture(width, height, depth, levels, format, FALSE, (D3DBaseTexture **)texture);
}

HRESULT WINAPI D3DDevice_CreateCubeTexture(UINT edge_length, UINT levels, DWORD usage, D3DFORMAT format,
	D3DPOOL pool, D3DCubeTexture **texture)
{
	(void)usage;
	(void)pool;
	return create_texture(edge_length, edge_length, 1, levels, format, TRUE, (D3DBaseTexture **)texture);
}

static void lock_level(const DWORD *resource, unsigned long face, unsigned long level,
	D3DLOCKED_RECT *locked, CONST RECT *rectangle)
{
	struct texture_description description;
	unsigned long pitch;
	char *bits;

	texture_describe(resource[3], resource[4], &description);
	pitch = level_pitch(&description, level);
	bits = (char *)resource[1];
	if (bits)
		bits += face * face_size(&description) + level_offset(&description, level);
	if (rectangle && bits)
	{
		/* swizzled textures cannot be addressed by rectangle; only linear
		and compressed layouts have a meaningful row pitch */
		unsigned long bytes = pitch / level_dimension(description.width, level);

		if (description.compressed)
			bits += (rectangle->top / 4) * pitch + (rectangle->left / 4) * (pitch / ((level_dimension(description.width, level) + 3) / 4));
		else
			bits += rectangle->top * pitch + rectangle->left * bytes;
	}
	locked->Pitch = (INT)pitch;
	locked->pBits = bits;
}

void WINAPI D3DTexture_LockRect(D3DTexture *texture, UINT level, D3DLOCKED_RECT *locked, CONST RECT *rectangle,
	DWORD flags)
{
	(void)flags;
	lock_level((const DWORD *)texture, 0, level, locked, rectangle);
}

void WINAPI D3DCubeTexture_LockRect(D3DCubeTexture *texture, D3DCUBEMAP_FACES face, UINT level,
	D3DLOCKED_RECT *locked, CONST RECT *rectangle, DWORD flags)
{
	(void)flags;
	lock_level((const DWORD *)texture, (unsigned long)face, level, locked, rectangle);
}

void WINAPI D3DVolumeTexture_LockBox(D3DVolumeTexture *texture, UINT level, D3DLOCKED_BOX *locked,
	CONST D3DBOX *box, DWORD flags)
{
	const DWORD *resource = (const DWORD *)texture;
	struct texture_description description;
	unsigned long row_pitch, slice;
	char *bits;

	(void)flags;
	texture_describe(resource[3], resource[4], &description);
	row_pitch = level_pitch(&description, level);
	slice = row_pitch * level_dimension(description.height, level);
	bits = (char *)resource[1];
	if (bits)
		bits += level_offset(&description, level);
	if (box && bits)
		bits += box->Front * slice + box->Top * row_pitch + box->Left * (row_pitch / level_dimension(description.width, level));
	locked->RowPitch = (INT)row_pitch;
	locked->SlicePitch = (INT)slice;
	locked->pBits = bits;
}

static void describe_level(const DWORD *resource, unsigned long level, D3DSURFACE_DESC *description)
{
	struct texture_description texture;
	unsigned long width, height;

	texture_describe(resource[3], resource[4], &texture);
	width = level_dimension(texture.width, level);
	height = level_dimension(texture.height, level);
	memset(description, 0, sizeof(*description));
	description->Format = (D3DFORMAT)texture.format;
	description->Type = D3DRTYPE_SURFACE;
	description->Width = width;
	description->Height = height;
	description->Size = level_pitch(&texture, level) * (texture.compressed ? (height + 3) / 4 : height);
	description->MultiSampleType = D3DMULTISAMPLE_NONE;
}

void WINAPI D3DTexture_GetLevelDesc(D3DTexture *texture, UINT level, D3DSURFACE_DESC *description)
{
	describe_level((const DWORD *)texture, level, description);
}

HRESULT WINAPI D3DTexture_GetSurfaceLevel(D3DTexture *texture, UINT level, D3DSurface **result)
{
	D3DBaseTexture *base = (D3DBaseTexture *)texture;
	struct texture_description description;
	D3DSurface *surface = calloc(1, sizeof(*surface));
	unsigned long width, height;

	if (!surface)
		return E_OUTOFMEMORY;
	texture_describe(base->Format, base->Size, &description);
	width = level_dimension(description.width, level);
	height = level_dimension(description.height, level);
	surface->Common = D3DCOMMON_TYPE_SURFACE | 1;
	surface->Data = base->Data + level_offset(&description, level);
	surface->Format = (base->Format & ~(D3DFORMAT_USIZE_MASK | D3DFORMAT_VSIZE_MASK | D3DFORMAT_PSIZE_MASK | D3DFORMAT_MIPMAP_MASK)) |
		(floor_log2(width) << D3DFORMAT_USIZE_SHIFT) |
		(floor_log2(height) << D3DFORMAT_VSIZE_SHIFT) |
		(1 << D3DFORMAT_MIPMAP_SHIFT);
	surface->Size = base->Size;
	surface->Parent = base;
	*result = surface;
	return S_OK;
}

void WINAPI D3DSurface_GetDesc(D3DSurface *surface, D3DSURFACE_DESC *description)
{
	describe_level((const DWORD *)surface, 0, description);
}

void WINAPI D3DSurface_LockRect(D3DSurface *surface, D3DLOCKED_RECT *locked, CONST RECT *rectangle, DWORD flags)
{
	(void)flags;
	lock_level((const DWORD *)surface, 0, 0, locked, rectangle);
}

HRESULT WINAPI D3DDevice_CreateVertexBuffer(UINT length, DWORD usage, DWORD fvf, D3DPOOL pool, D3DVertexBuffer **result)
{
	D3DVertexBuffer *buffer = calloc(1, sizeof(*buffer));
	void *memory;

	(void)usage;
	(void)fvf;
	(void)pool;
	if (!buffer)
		return E_OUTOFMEMORY;
	memory = allocate_resource_memory(length);
	if (!memory)
	{
		free(buffer);
		return E_OUTOFMEMORY;
	}
	buffer->Common = D3DCOMMON_TYPE_VERTEXBUFFER | D3DCOMMON_D3DCREATED | 1;
	buffer->Data = (DWORD)memory;
	*result = buffer;
	return S_OK;
}

void WINAPI D3DVertexBuffer_Lock(D3DVertexBuffer *buffer, UINT offset, UINT size, BYTE **data, DWORD flags)
{
	(void)size;
	(void)flags;
	*data = buffer->Data ? (BYTE *)buffer->Data + offset : NULL;
}

HRESULT WINAPI D3DDevice_CreateIndexBuffer(UINT length, DWORD usage, D3DFORMAT format, D3DPOOL pool,
	D3DIndexBuffer **result)
{
	D3DIndexBuffer *buffer = calloc(1, sizeof(*buffer));
	void *memory;

	(void)usage;
	(void)format;
	(void)pool;
	if (!buffer)
		return E_OUTOFMEMORY;
	memory = calloc(1, length ? length : 1);
	if (!memory)
	{
		free(buffer);
		return E_OUTOFMEMORY;
	}
	buffer->Common = D3DCOMMON_TYPE_INDEXBUFFER | D3DCOMMON_D3DCREATED | 1;
	buffer->Data = (DWORD)memory;
	*result = buffer;
	return S_OK;
}

HRESULT WINAPI D3DDevice_CreatePalette(D3DPALETTESIZE size, D3DPalette **result)
{
	D3DPalette *palette = calloc(1, sizeof(*palette));
	void *memory;

	if (!palette)
		return E_OUTOFMEMORY;
	/* always room for 256 entries, as d3d8_resources.c */
	memory = allocate_resource_memory(256 * sizeof(D3DCOLOR));
	if (!memory)
	{
		free(palette);
		return E_OUTOFMEMORY;
	}
	palette->Common = D3DCOMMON_TYPE_PALETTE | D3DCOMMON_D3DCREATED | 1 | ((DWORD)size << D3DPALETTE_COMMON_PALETTESIZE_SHIFT);
	palette->Data = (DWORD)memory;
	*result = palette;
	return S_OK;
}

void WINAPI D3DPalette_Lock(D3DPalette *palette, D3DCOLOR **colors, DWORD flags)
{
	(void)flags;
	*colors = (D3DCOLOR *)palette->Data;
}

/* ---------- the port's display functions (port/linux/src/d3d8_gl.c,
xbox_textures.c; declared in port/linux/include/halo_linux_source_fixups.h)

The PS2 draws the Xbox's 640 by 480: no wider screen, no anti-aliasing,
the Xbox's shadow maps, no mouse pointer, and no Custom Edition maps. */

struct halo_ui_pointer;

long halo_screen_width(void)
{
	return BACK_BUFFER_WIDTH;
}

long halo_screen_commit(void)
{
	return BACK_BUFFER_WIDTH;
}

long halo_shadow_map_scale(void)
{
	return 1;
}

void halo_screen_ui_offset(unsigned char centered)
{
	(void)centered;
}

void halo_screen_anti_alias(short x0, short y0, short x1, short y1)
{
	(void)x0;
	(void)y0;
	(void)x1;
	(void)y1;
}

void halo_vertex_shader_lighting(unsigned long handle)
{
	(void)handle;
}

int halo_ui_pointer_update(int menus_active, struct halo_ui_pointer *pointer)
{
	(void)menus_active;
	(void)pointer;
	return 0;
}

int halo_scoreboard_pointer_update(int offered, struct halo_ui_pointer *pointer)
{
	(void)offered;
	(void)pointer;
	/* no pointer, as on Android */
	return -1;
}

void halo_custom_edition_texels_channels(const void *texels, unsigned char channel_order)
{
	(void)texels;
	(void)channel_order;
}

void halo_custom_edition_texels_forget(void)
{
}

/* ---------- D3DX (as d3d8_resources.c) */

D3DXMATRIX *WINAPI D3DXMatrixPerspectiveLH(D3DXMATRIX *out, FLOAT width, FLOAT height, FLOAT near_plane, FLOAT far_plane)
{
	memset(out, 0, sizeof(*out));
	out->_11 = 2.0f * near_plane / width;
	out->_22 = 2.0f * near_plane / height;
	out->_33 = far_plane / (far_plane - near_plane);
	out->_34 = 1.0f;
	out->_43 = near_plane * far_plane / (near_plane - far_plane);
	return out;
}

D3DXMATRIX *WINAPI D3DXMatrixOrthoLH(D3DXMATRIX *out, FLOAT width, FLOAT height, FLOAT near_plane, FLOAT far_plane)
{
	memset(out, 0, sizeof(*out));
	out->_11 = 2.0f / width;
	out->_22 = 2.0f / height;
	out->_33 = 1.0f / (far_plane - near_plane);
	out->_43 = near_plane / (near_plane - far_plane);
	out->_44 = 1.0f;
	return out;
}

D3DXVECTOR4 *WINAPI D3DXVec4Transform(D3DXVECTOR4 *out, CONST D3DXVECTOR4 *vector, CONST D3DXMATRIX *matrix)
{
	D3DXVECTOR4 result;

	result.x = vector->x * matrix->_11 + vector->y * matrix->_21 + vector->z * matrix->_31 + vector->w * matrix->_41;
	result.y = vector->x * matrix->_12 + vector->y * matrix->_22 + vector->z * matrix->_32 + vector->w * matrix->_42;
	result.z = vector->x * matrix->_13 + vector->y * matrix->_23 + vector->z * matrix->_33 + vector->w * matrix->_43;
	result.w = vector->x * matrix->_14 + vector->y * matrix->_24 + vector->z * matrix->_34 + vector->w * matrix->_44;
	*out = result;
	return out;
}

HRESULT WINAPI D3DXGetErrorStringA(HRESULT error, LPSTR buffer, UINT buffer_length)
{
	if (buffer && buffer_length)
	{
		const char *text = error == S_OK ? "S_OK" :
			error == E_OUTOFMEMORY ? "E_OUTOFMEMORY" :
			error == E_FAIL ? "E_FAIL" : "Direct3D error";

		strncpy(buffer, text, buffer_length - 1);
		buffer[buffer_length - 1] = '\0';
	}
	return S_OK;
}
