/*
 * Copyright 2002-2005 Jason Edmeades
 * Copyright 2002-2005 Raphael Junqueira
 * Copyright 2004 Christian Costa
 * Copyright 2005 Oliver Stieber
 * Copyright 2007-2010 Stefan Dösinger for CodeWeavers
 * Copyright 2009-2010 Henri Verbeet for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 *
 */

#include "config.h"
#include "wine/port.h"

#include "wined3d_private.h"

WINE_DEFAULT_DEBUG_CHANNEL(d3d);

#define VB_MAXDECLCHANGES     100     /* After that number of decl changes we stop converting */
#define VB_RESETDECLCHANGE    1000    /* Reset the decl changecount after that number of draws */
#define VB_MAXFULLCONVERSIONS 5       /* Number of full conversions before we stop converting */
#define VB_RESETFULLCONVS     20      /* Reset full conversion counts after that number of draws */

static inline BOOL buffer_add_dirty_area(struct wined3d_buffer *This, UINT offset, UINT size)
{
    if (!This->buffer_object) return TRUE;

    if (This->maps_size <= This->modified_areas)
    {
        void *new = HeapReAlloc(GetProcessHeap(), 0, This->maps,
                                This->maps_size * 2 * sizeof(*This->maps));
        if (!new)
        {
            ERR("Out of memory\n");
            return FALSE;
        }
        else
        {
            This->maps = new;
            This->maps_size *= 2;
        }
    }

    if(offset > This->resource.size || offset + size > This->resource.size)
    {
        WARN("Invalid range dirtified, marking entire buffer dirty\n");
        offset = 0;
        size = This->resource.size;
    }
    else if(!offset && !size)
    {
        size = This->resource.size;
    }

    This->maps[This->modified_areas].offset = offset;
    This->maps[This->modified_areas].size = size;
    This->modified_areas++;
    return TRUE;
}

static inline void buffer_clear_dirty_areas(struct wined3d_buffer *This)
{
    This->modified_areas = 0;
}

static BOOL buffer_is_dirty(const struct wined3d_buffer *buffer)
{
    return !!buffer->modified_areas;
}

static BOOL buffer_is_fully_dirty(const struct wined3d_buffer *buffer)
{
    unsigned int i;

    for (i = 0; i < buffer->modified_areas; ++i)
    {
        if (!buffer->maps[i].offset && buffer->maps[i].size == buffer->resource.size)
            return TRUE;
    }
    return FALSE;
}

/* Context activation is done by the caller */
static void delete_gl_buffer(struct wined3d_buffer *This, const struct wined3d_gl_info *gl_info)
{
    if(!This->buffer_object) return;

    GL_EXTCALL(glDeleteBuffersARB(1, &This->buffer_object));
    checkGLcall("glDeleteBuffersARB");
    This->buffer_object = 0;

    if(This->query)
    {
        wined3d_event_query_destroy(This->query);
        This->query = NULL;
    }
    This->flags &= ~WINED3D_BUFFER_APPLESYNC;
}

/* Context activation is done by the caller. */
static void buffer_create_buffer_object(struct wined3d_buffer *This, const struct wined3d_gl_info *gl_info)
{
    GLenum gl_usage = GL_STATIC_DRAW_ARB;
    GLenum error;

    TRACE("Creating an OpenGL vertex buffer object for wined3d_buffer %p with usage %s.\n",
            This, debug_d3dusage(This->resource.usage));

    /* Make sure that the gl error is cleared. Do not use checkGLcall
    * here because checkGLcall just prints a fixme and continues. However,
    * if an error during VBO creation occurs we can fall back to non-vbo operation
    * with full functionality(but performance loss)
    */
    while (gl_info->gl_ops.gl.p_glGetError() != GL_NO_ERROR);

    /* Basically the FVF parameter passed to CreateVertexBuffer is no good.
     * The vertex declaration from the device determines how the data in the
     * buffer is interpreted. This means that on each draw call the buffer has
     * to be verified to check if the rhw and color values are in the correct
     * format. */

    GL_EXTCALL(glGenBuffersARB(1, &This->buffer_object));
    error = gl_info->gl_ops.gl.p_glGetError();
    if (!This->buffer_object || error != GL_NO_ERROR)
    {
        ERR("Failed to create a VBO with error %s (%#x)\n", debug_glerror(error), error);
        goto fail;
    }

    if (This->buffer_type_hint == GL_ELEMENT_ARRAY_BUFFER_ARB)
        device_invalidate_state(This->resource.device, STATE_INDEXBUFFER);
    GL_EXTCALL(glBindBufferARB(This->buffer_type_hint, This->buffer_object));
    error = gl_info->gl_ops.gl.p_glGetError();
    if (error != GL_NO_ERROR)
    {
        ERR("Failed to bind the VBO with error %s (%#x)\n", debug_glerror(error), error);
        goto fail;
    }

    if (This->resource.usage & WINED3DUSAGE_DYNAMIC)
    {
        TRACE("Buffer has WINED3DUSAGE_DYNAMIC set.\n");
        gl_usage = GL_STREAM_DRAW_ARB;

        if(gl_info->supported[APPLE_FLUSH_BUFFER_RANGE])
        {
            GL_EXTCALL(glBufferParameteriAPPLE(This->buffer_type_hint, GL_BUFFER_FLUSHING_UNMAP_APPLE, GL_FALSE));
            checkGLcall("glBufferParameteriAPPLE(This->buffer_type_hint, GL_BUFFER_FLUSHING_UNMAP_APPLE, GL_FALSE)");
            This->flags |= WINED3D_BUFFER_FLUSH;

            GL_EXTCALL(glBufferParameteriAPPLE(This->buffer_type_hint, GL_BUFFER_SERIALIZED_MODIFY_APPLE, GL_FALSE));
            checkGLcall("glBufferParameteriAPPLE(This->buffer_type_hint, GL_BUFFER_SERIALIZED_MODIFY_APPLE, GL_FALSE)");
            This->flags |= WINED3D_BUFFER_APPLESYNC;
        }
        /* No setup is needed here for GL_ARB_map_buffer_range */
    }

    /* Reserve memory for the buffer. The amount of data won't change
     * so we are safe with calling glBufferData once and
     * calling glBufferSubData on updates. Upload the actual data in case
     * we're not double buffering, so we can release the heap mem afterwards
     */
    GL_EXTCALL(glBufferDataARB(This->buffer_type_hint, This->resource.size, This->resource.allocatedMemory, gl_usage));
    error = gl_info->gl_ops.gl.p_glGetError();
    if (error != GL_NO_ERROR)
    {
        ERR("glBufferDataARB failed with error %s (%#x)\n", debug_glerror(error), error);
        goto fail;
    }

    This->buffer_object_size = This->resource.size;
    This->buffer_object_usage = gl_usage;

    if(This->flags & WINED3D_BUFFER_DOUBLEBUFFER)
    {
        if(!buffer_add_dirty_area(This, 0, 0))
        {
            ERR("buffer_add_dirty_area failed, this is not expected\n");
            goto fail;
        }
    }
    else
    {
        HeapFree(GetProcessHeap(), 0, This->resource.heapMemory);
        This->resource.allocatedMemory = NULL;
        This->resource.heapMemory = NULL;
    }

    return;

fail:
    /* Clean up all vbo init, but continue because we can work without a vbo :-) */
    ERR("Failed to create a vertex buffer object. Continuing, but performance issues may occur\n");
    delete_gl_buffer(This, gl_info);
    buffer_clear_dirty_areas(This);
}

static BOOL buffer_process_converted_attribute(struct wined3d_buffer *This,
        const enum wined3d_buffer_conversion_type conversion_type,
        const struct wined3d_stream_info_element *attrib, DWORD *stride_this_run
#ifdef VBOX_WITH_WINE_FIX_BUFOFFSET
        , DWORD *offset_this_run
#endif
        )
{
    DWORD attrib_size;
    BOOL ret = FALSE;
    unsigned int i;
#ifndef VBOX_WITH_WINE_FIX_BUFOFFSET
    DWORD_PTR data;
#else
    UINT stream_offset = (UINT)(attrib->data.addr - attrib->offset);
#endif

    /* Check for some valid situations which cause us pain. One is if the buffer is used for
     * constant attributes(stride = 0), the other one is if the buffer is used on two streams
     * with different strides. In the 2nd case we might have to drop conversion entirely,
     * it is possible that the same bytes are once read as FLOAT2 and once as UBYTE4N.
     */
    if (!attrib->stride)
    {
        FIXME("%s used with stride 0, let's hope we get the vertex stride from somewhere else\n",
                debug_d3dformat(attrib->format->id));
    }
#ifndef VBOX_WITH_WINE_FIX_BUFOFFSET
    else if(attrib->stride != *stride_this_run && *stride_this_run)
    {
        FIXME("Got two concurrent strides, %d and %d\n", attrib->stride, *stride_this_run);
    }
#else
    else if((attrib->stride != *stride_this_run && *stride_this_run)
            || (stream_offset != *offset_this_run && *offset_this_run))
    {
        ERR("Got two concurrent strides, %d and %d\n", attrib->stride, *stride_this_run);
    }
#endif
    else
    {
        *stride_this_run = attrib->stride;
#ifdef VBOX_WITH_WINE_FIX_BUFOFFSET
        *offset_this_run = stream_offset;
        if (This->stride != *stride_this_run
                || This->offset != *offset_this_run)
#else
        if (This->stride != *stride_this_run)
#endif
        {
            /* We rely that this happens only on the first converted attribute that is found,
             * if at all. See above check
             */
            TRACE("Reconverting because converted attributes occur, and the stride changed\n");
            This->stride = *stride_this_run;
#ifdef VBOX_WITH_WINE_FIX_BUFOFFSET
            This->offset = *offset_this_run;
#endif
            HeapFree(GetProcessHeap(), HEAP_ZERO_MEMORY, This->conversion_map);
            This->conversion_map = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                    sizeof(*This->conversion_map) * This->stride);
            ret = TRUE;
        }
    }

#ifndef VBOX_WITH_WINE_FIX_BUFOFFSET
    data = ((DWORD_PTR)attrib->data.addr) % This->stride;
#endif
    attrib_size = attrib->format->component_count * attrib->format->component_size;
    for (i = 0; i < attrib_size; ++i)
    {
#ifndef VBOX_WITH_WINE_FIX_BUFOFFSET
        DWORD_PTR idx = (data + i) % This->stride;
#else
        DWORD_PTR idx = attrib->offset;
#endif
        if (This->conversion_map[idx] != conversion_type)
        {
            TRACE("Byte %ld in vertex changed\n", idx);
            TRACE("It was type %d, is %d now\n", This->conversion_map[idx], conversion_type);
            ret = TRUE;
            This->conversion_map[idx] = conversion_type;
        }
    }

    return ret;
}

static BOOL buffer_check_attribute(struct wined3d_buffer *This, const struct wined3d_stream_info *si,
        UINT attrib_idx, const BOOL check_d3dcolor, const BOOL is_ffp_position, const BOOL is_ffp_color,
        DWORD *stride_this_run
#ifdef VBOX_WITH_WINE_FIX_BUFOFFSET
        , DWORD *offset_this_run
#endif
        )
{
    const struct wined3d_stream_info_element *attrib = &si->elements[attrib_idx];
    enum wined3d_format_id format;
    BOOL ret = FALSE;

    /* Ignore attributes that do not have our vbo. After that check we can be sure that the attribute is
     * there, on nonexistent attribs the vbo is 0.
     */
    if (!(si->use_map & (1 << attrib_idx))
            || attrib->data.buffer_object != This->buffer_object)
        return FALSE;

    format = attrib->format->id;
    /* Look for newly appeared conversion */
    if (check_d3dcolor && format == WINED3DFMT_B8G8R8A8_UNORM)
    {
        ret = buffer_process_converted_attribute(This, CONV_D3DCOLOR, attrib, stride_this_run
#ifdef VBOX_WITH_WINE_FIX_BUFOFFSET
                , offset_this_run
#endif
                );

        if (!is_ffp_color) FIXME("Test for non-color fixed function WINED3DFMT_B8G8R8A8_UNORM format\n");
    }
    else if (is_ffp_position && si->position_transformed)
    {
        if (format != WINED3DFMT_R32G32B32A32_FLOAT)
        {
            FIXME("Unexpected format %s for transformed position.\n", debug_d3dformat(format));
            return FALSE;
        }

        ret = buffer_process_converted_attribute(This, CONV_POSITIONT, attrib, stride_this_run
#ifdef VBOX_WITH_WINE_FIX_BUFOFFSET
                , offset_this_run
#endif
                );
    }
    else if (This->conversion_map)
    {
        ret = buffer_process_converted_attribute(This, CONV_NONE, attrib, stride_this_run
#ifdef VBOX_WITH_WINE_FIX_BUFOFFSET
                , offset_this_run
#endif
                );
    }

    return ret;
}

static BOOL buffer_find_decl(struct wined3d_buffer *This)
{
    struct wined3d_device *device = This->resource.device;
    const struct wined3d_gl_info *gl_info = &device->adapter->gl_info;
    const struct wined3d_stream_info *si = &device->stream_info;
    const struct wined3d_state *state = &device->stateBlock->state;
    UINT stride_this_run = 0;
#ifdef VBOX_WITH_WINE_FIX_BUFOFFSET
    UINT offset_this_run = 0;
#endif
    BOOL ret = FALSE;
    BOOL support_d3dcolor = gl_info->supported[ARB_VERTEX_ARRAY_BGRA];

    /* In d3d7 the vertex buffer declaration NEVER changes because it is stored in the d3d7 vertex buffer.
     * Once we have our declaration there is no need to look it up again. Index buffers also never need
     * conversion, so once the (empty) conversion structure is created don't bother checking again
     */
    if (This->flags & WINED3D_BUFFER_HASDESC)
    {
        if(This->resource.usage & WINED3DUSAGE_STATICDECL) return FALSE;
    }

    if (use_vs(state))
    {
        TRACE("Vertex shaders used, no VBO conversion is needed\n");
        if(This->conversion_map)
        {
            HeapFree(GetProcessHeap(), 0, This->conversion_map);
            This->conversion_map = NULL;
            This->stride = 0;
            return TRUE;
        }

        return FALSE;
    }

    TRACE("Finding vertex buffer conversion information\n");
    /* Certain declaration types need some fixups before we can pass them to
     * opengl. This means D3DCOLOR attributes with fixed function vertex
     * processing, FLOAT4 POSITIONT with fixed function, and FLOAT16 if
     * GL_ARB_half_float_vertex is not supported.
     *
     * Note for d3d8 and d3d9:
     * The vertex buffer FVF doesn't help with finding them, we have to use
     * the decoded vertex declaration and pick the things that concern the
     * current buffer. A problem with this is that this can change between
     * draws, so we have to validate the information and reprocess the buffer
     * if it changes, and avoid false positives for performance reasons.
     * WineD3D doesn't even know the vertex buffer any more, it is managed
     * by the client libraries and passed to SetStreamSource and ProcessVertices
     * as needed.
     *
     * We have to distinguish between vertex shaders and fixed function to
     * pick the way we access the strided vertex information.
     *
     * This code sets up a per-byte array with the size of the detected
     * stride of the arrays in the buffer. For each byte we have a field
     * that marks the conversion needed on this byte. For example, the
     * following declaration with fixed function vertex processing:
     *
     *      POSITIONT, FLOAT4
     *      NORMAL, FLOAT3
     *      DIFFUSE, FLOAT16_4
     *      SPECULAR, D3DCOLOR
     *
     * Will result in
     * {                 POSITIONT                    }{             NORMAL                }{    DIFFUSE          }{SPECULAR }
     * [P][P][P][P][P][P][P][P][P][P][P][P][P][P][P][P][0][0][0][0][0][0][0][0][0][0][0][0][F][F][F][F][F][F][F][F][C][C][C][C]
     *
     * Where in this example map P means 4 component position conversion, 0
     * means no conversion, F means FLOAT16_2 conversion and C means D3DCOLOR
     * conversion (red / blue swizzle).
     *
     * If we're doing conversion and the stride changes we have to reconvert
     * the whole buffer. Note that we do not mind if the semantic changes,
     * we only care for the conversion type. So if the NORMAL is replaced
     * with a TEXCOORD, nothing has to be done, or if the DIFFUSE is replaced
     * with a D3DCOLOR BLENDWEIGHT we can happily dismiss the change. Some
     * conversion types depend on the semantic as well, for example a FLOAT4
     * texcoord needs no conversion while a FLOAT4 positiont needs one
     */

    ret = buffer_check_attribute(This, si, WINED3D_FFP_POSITION,
            TRUE, TRUE,  FALSE, &stride_this_run
#ifdef VBOX_WITH_WINE_FIX_BUFOFFSET
                , &offset_this_run
#endif
            ) || ret;
    ret = buffer_check_attribute(This, si, WINED3D_FFP_NORMAL,
            TRUE, FALSE, FALSE, &stride_this_run
#ifdef VBOX_WITH_WINE_FIX_BUFOFFSET
                , &offset_this_run
#endif
            ) || ret;
    ret = buffer_check_attribute(This, si, WINED3D_FFP_DIFFUSE,
            !support_d3dcolor, FALSE, TRUE,  &stride_this_run
#ifdef VBOX_WITH_WINE_FIX_BUFOFFSET
                , &offset_this_run
#endif
            ) || ret;
    ret = buffer_check_attribute(This, si, WINED3D_FFP_SPECULAR,
            !support_d3dcolor, FALSE, TRUE,  &stride_this_run
#ifdef VBOX_WITH_WINE_FIX_BUFOFFSET
                , &offset_this_run
#endif
            ) || ret;
    ret = buffer_check_attribute(This, si, WINED3D_FFP_TEXCOORD0,
            TRUE, FALSE, FALSE, &stride_this_run
#ifdef VBOX_WITH_WINE_FIX_BUFOFFSET
                , &offset_this_run
#endif
            ) || ret;
    ret = buffer_check_attribute(This, si, WINED3D_FFP_TEXCOORD1,
            TRUE, FALSE, FALSE, &stride_this_run
#ifdef VBOX_WITH_WINE_FIX_BUFOFFSET
                , &offset_this_run
#endif
            ) || ret;
    ret = buffer_check_attribute(This, si, WINED3D_FFP_TEXCOORD2,
            TRUE, FALSE, FALSE, &stride_this_run
#ifdef VBOX_WITH_WINE_FIX_BUFOFFSET
                , &offset_this_run
#endif
            ) || ret;
    ret = buffer_check_attribute(This, si, WINED3D_FFP_TEXCOORD3,
            TRUE, FALSE, FALSE, &stride_this_run
#ifdef VBOX_WITH_WINE_FIX_BUFOFFSET
                , &offset_this_run
#endif
            ) || ret;
    ret = buffer_check_attribute(This, si, WINED3D_FFP_TEXCOORD4,
            TRUE, FALSE, FALSE, &stride_this_run
#ifdef VBOX_WITH_WINE_FIX_BUFOFFSET
                , &offset_this_run
#endif
            ) || ret;
    ret = buffer_check_attribute(This, si, WINED3D_FFP_TEXCOORD5,
            TRUE, FALSE, FALSE, &stride_this_run
#ifdef VBOX_WITH_WINE_FIX_BUFOFFSET
                , &offset_this_run
#endif
            ) || ret;
    ret = buffer_check_attribute(This, si, WINED3D_FFP_TEXCOORD6,
            TRUE, FALSE, FALSE, &stride_this_run
#ifdef VBOX_WITH_WINE_FIX_BUFOFFSET
                , &offset_this_run
#endif
            ) || ret;
    ret = buffer_check_attribute(This, si, WINED3D_FFP_TEXCOORD7,
            TRUE, FALSE, FALSE, &stride_this_run
#ifdef VBOX_WITH_WINE_FIX_BUFOFFSET
                , &offset_this_run
#endif
            ) || ret;

    if (!stride_this_run && This->conversion_map)
    {
        /* Sanity test */
        if (!ret) ERR("no converted attributes found, old conversion map exists, and no declaration change?\n");
        HeapFree(GetProcessHeap(), 0, This->conversion_map);
        This->conversion_map = NULL;
        This->stride = 0;
    }

    if (ret) TRACE("Conversion information changed\n");

    return ret;
}

static inline void fixup_d3dcolor(DWORD *dst_color)
{
    DWORD src_color = *dst_color;

    /* Color conversion like in drawStridedSlow. watch out for little endianity
     * If we want that stuff to work on big endian machines too we have to consider more things
     *
     * 0xff000000: Alpha mask
     * 0x00ff0000: Blue mask
     * 0x0000ff00: Green mask
     * 0x000000ff: Red mask
     */
    *dst_color = 0;
    *dst_color |= (src_color & 0xff00ff00);         /* Alpha Green */
    *dst_color |= (src_color & 0x00ff0000) >> 16;   /* Red */
    *dst_color |= (src_color & 0x000000ff) << 16;   /* Blue */
}

static inline void fixup_transformed_pos(float *p)
{
    /* rhw conversion like in position_float4(). */
    if (p[3] != 1.0f && p[3] != 0.0f)
    {
        float w = 1.0f / p[3];
        p[0] *= w;
        p[1] *= w;
        p[2] *= w;
        p[3] = w;
    }
}

/* Context activation is done by the caller. */
void buffer_get_memory(struct wined3d_buffer *buffer, const struct wined3d_gl_info *gl_info,
        struct wined3d_bo_address *data)
{
    data->buffer_object = buffer->buffer_object;
    if (!buffer->buffer_object)
    {
        if ((buffer->flags & WINED3D_BUFFER_CREATEBO) && !buffer->resource.map_count)
        {
            buffer_create_buffer_object(buffer, gl_info);
            buffer->flags &= ~WINED3D_BUFFER_CREATEBO;
            if (buffer->buffer_object)
            {
                data->buffer_object = buffer->buffer_object;
                data->addr = NULL;
                return;
            }
        }
        data->addr = buffer->resource.allocatedMemory;
    }
    else
    {
        data->addr = NULL;
    }
}

ULONG CDECL wined3d_buffer_incref(struct wined3d_buffer *buffer)
{
    ULONG refcount = InterlockedIncrement(&buffer->resource.ref);

    TRACE("%p increasing refcount to %u.\n", buffer, refcount);

    return refcount;
}

/* Context activation is done by the caller. */
BYTE *buffer_get_sysmem(struct wined3d_buffer *This, const struct wined3d_gl_info *gl_info)
{
    /* AllocatedMemory exists if the buffer is double buffered or has no buffer object at all */
    if(This->resource.allocatedMemory) return This->resource.allocatedMemory;

    This->resource.heapMemory = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, This->resource.size + RESOURCE_ALIGNMENT);
    This->resource.allocatedMemory = (BYTE *)(((ULONG_PTR)This->resource.heapMemory + (RESOURCE_ALIGNMENT - 1)) & ~(RESOURCE_ALIGNMENT - 1));

    if (This->buffer_type_hint == GL_ELEMENT_ARRAY_BUFFER_ARB)
        device_invalidate_state(This->resource.device, STATE_INDEXBUFFER);

    GL_EXTCALL(glBindBufferARB(This->buffer_type_hint, This->buffer_object));
    GL_EXTCALL(glGetBufferSubDataARB(This->buffer_type_hint, 0, This->resource.size, This->resource.allocatedMemory));
    This->flags |= WINED3D_BUFFER_DOUBLEBUFFER;

    return This->resource.allocatedMemory;
}

/* Do not call while under the GL lock. */
static void buffer_unload(struct wined3d_resource *resource)
{
    struct wined3d_buffer *buffer = buffer_from_resource(resource);

    TRACE("buffer %p.\n", buffer);

    if (buffer->buffer_object)
    {
        struct wined3d_device *device = resource->device;
        struct wined3d_context *context;

        context = context_acquire(device, NULL);

        /* Download the buffer, but don't permanently enable double buffering */
        if (!(buffer->flags & WINED3D_BUFFER_DOUBLEBUFFER))
        {
            buffer_get_sysmem(buffer, context->gl_info);
            buffer->flags &= ~WINED3D_BUFFER_DOUBLEBUFFER;
        }

        delete_gl_buffer(buffer, context->gl_info);
        buffer->flags |= WINED3D_BUFFER_CREATEBO; /* Recreate the buffer object next load */
        buffer_clear_dirty_areas(buffer);

        context_release(context);

        HeapFree(GetProcessHeap(), 0, buffer->conversion_map);
        buffer->conversion_map = NULL;
        buffer->stride = 0;
        buffer->conversion_stride = 0;
        buffer->flags &= ~WINED3D_BUFFER_HASDESC;
    }

    resource_unload(resource);
}

/* Do not call while under the GL lock. */
ULONG CDECL wined3d_buffer_decref(struct wined3d_buffer *buffer)
{
    ULONG refcount = InterlockedDecrement(&buffer->resource.ref);

    TRACE("%p decreasing refcount to %u.\n", buffer, refcount);

    if (!refcount)
    {
        buffer_unload(&buffer->resource);
        resource_cleanup(&buffer->resource);
        buffer->resource.parent_ops->wined3d_object_destroyed(buffer->resource.parent);
        HeapFree(GetProcessHeap(), 0, buffer->maps);
        HeapFree(GetProcessHeap(), 0, buffer);
    }

    return refcount;
}

void * CDECL wined3d_buffer_get_parent(const struct wined3d_buffer *buffer)
{
    TRACE("buffer %p.\n", buffer);

    return buffer->resource.parent;
}

DWORD CDECL wined3d_buffer_set_priority(struct wined3d_buffer *buffer, DWORD priority)
{
    return resource_set_priority(&buffer->resource, priority);
}

DWORD CDECL wined3d_buffer_get_priority(const struct wined3d_buffer *buffer)
{
    return resource_get_priority(&buffer->resource);
}

/* The caller provides a context and binds the buffer */
static void buffer_sync_apple(struct wined3d_buffer *This, DWORD flags, const struct wined3d_gl_info *gl_info)
{
    enum wined3d_event_query_result ret;

    /* No fencing needs to be done if the app promises not to overwrite
     * existing data. */
    if (flags & WINED3D_MAP_NOOVERWRITE)
        return;

    if (flags & WINED3D_MAP_DISCARD)
    {
        GL_EXTCALL(glBufferDataARB(This->buffer_type_hint, This->resource.size, NULL, This->buffer_object_usage));
        checkGLcall("glBufferDataARB\n");
        return;
    }

    if(!This->query)
    {
        TRACE("Creating event query for buffer %p\n", This);

        if (!wined3d_event_query_supported(gl_info))
        {
            FIXME("Event queries not supported, dropping async buffer locks.\n");
            goto drop_query;
        }

        This->query = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*This->query));
        if (!This->query)
        {
            ERR("Failed to allocate event query memory, dropping async buffer locks.\n");
            goto drop_query;
        }

        /* Since we don't know about old draws a glFinish is needed once */
        gl_info->gl_ops.gl.p_glFinish();
        return;
    }
    TRACE("Synchronizing buffer %p\n", This);
    ret = wined3d_event_query_finish(This->query, This->resource.device);
    switch(ret)
    {
        case WINED3D_EVENT_QUERY_NOT_STARTED:
        case WINED3D_EVENT_QUERY_OK:
            /* All done */
            return;

        case WINED3D_EVENT_QUERY_WRONG_THREAD:
            WARN("Cannot synchronize buffer lock due to a thread conflict\n");
            goto drop_query;

        default:
            ERR("wined3d_event_query_finish returned %u, dropping async buffer locks\n", ret);
            goto drop_query;
    }

drop_query:
    if(This->query)
    {
        wined3d_event_query_destroy(This->query);
        This->query = NULL;
    }

    gl_info->gl_ops.gl.p_glFinish();
    GL_EXTCALL(glBufferParameteriAPPLE(This->buffer_type_hint, GL_BUFFER_SERIALIZED_MODIFY_APPLE, GL_TRUE));
    checkGLcall("glBufferParameteriAPPLE(This->buffer_type_hint, GL_BUFFER_SERIALIZED_MODIFY_APPLE, GL_TRUE)");
    This->flags &= ~WINED3D_BUFFER_APPLESYNC;
}

/* The caller provides a GL context */
static void buffer_direct_upload(struct wined3d_buffer *This, const struct wined3d_gl_info *gl_info, DWORD flags)
{
    BYTE *map;
    UINT start = 0, len = 0;

    /* This potentially invalidates the element array buffer binding, but the
     * caller always takes care of this. */
    GL_EXTCALL(glBindBufferARB(This->buffer_type_hint, This->buffer_object));
    checkGLcall("glBindBufferARB");
    if (gl_info->supported[ARB_MAP_BUFFER_RANGE])
    {
        GLbitfield mapflags;
        mapflags = GL_MAP_WRITE_BIT | GL_MAP_FLUSH_EXPLICIT_BIT;
        if (flags & WINED3D_BUFFER_DISCARD)
            mapflags |= GL_MAP_INVALIDATE_BUFFER_BIT;
        if (flags & WINED3D_BUFFER_NOSYNC)
            mapflags |= GL_MAP_UNSYNCHRONIZED_BIT;
        map = GL_EXTCALL(glMapBufferRange(This->buffer_type_hint, 0,
                    This->resource.size, mapflags));
        checkGLcall("glMapBufferRange");
    }
    else
    {
        if (This->flags & WINED3D_BUFFER_APPLESYNC)
        {
            DWORD syncflags = 0;
            if (flags & WINED3D_BUFFER_DISCARD)
                syncflags |= WINED3D_MAP_DISCARD;
            if (flags & WINED3D_BUFFER_NOSYNC)
                syncflags |= WINED3D_MAP_NOOVERWRITE;
            buffer_sync_apple(This, syncflags, gl_info);
        }
        map = GL_EXTCALL(glMapBufferARB(This->buffer_type_hint, GL_WRITE_ONLY_ARB));
        checkGLcall("glMapBufferARB");
    }
    if (!map)
    {
        ERR("Failed to map opengl buffer\n");
        return;
    }

    while (This->modified_areas)
    {
        This->modified_areas--;
        start = This->maps[This->modified_areas].offset;
        len = This->maps[This->modified_areas].size;

        memcpy(map + start, This->resource.allocatedMemory + start, len);

        if (gl_info->supported[ARB_MAP_BUFFER_RANGE])
        {
            GL_EXTCALL(glFlushMappedBufferRange(This->buffer_type_hint, start, len));
            checkGLcall("glFlushMappedBufferRange");
        }
        else if (This->flags & WINED3D_BUFFER_FLUSH)
        {
            GL_EXTCALL(glFlushMappedBufferRangeAPPLE(This->buffer_type_hint, start, len));
            checkGLcall("glFlushMappedBufferRangeAPPLE");
        }
    }
    GL_EXTCALL(glUnmapBufferARB(This->buffer_type_hint));
    checkGLcall("glUnmapBufferARB");
}

#ifdef VBOX_WITH_WINE_FIX_BUFOFFSET
static UINT wined3d_buffer_stream_offset_left(UINT x, UINT offset, UINT stride)
{
    UINT off = ((UINT)(RT_ABS((INT)(x - offset))) % stride);
    if (!off)
        return x;
    if (x > offset)
        return x - off;

    /* x < offset */
    return x  - stride + off;
}

static UINT wined3d_buffer_stream_offset_right(UINT x, UINT offset, UINT stride)
{
    UINT left = wined3d_buffer_stream_offset_left(x, offset, stride);

    if (left == x)
        return x;
    return left + stride;
}
#endif

/* Do not call while under the GL lock. */
void CDECL wined3d_buffer_preload(struct wined3d_buffer *buffer)
{
    DWORD flags = buffer->flags & (WINED3D_BUFFER_NOSYNC | WINED3D_BUFFER_DISCARD);
    struct wined3d_device *device = buffer->resource.device;
    UINT start = 0, end = 0, len = 0, vertices;
    const struct wined3d_gl_info *gl_info;
    struct wined3d_context *context;
    BOOL decl_changed = FALSE;
    unsigned int i, j;
    BYTE *data;
#ifdef VBOX_WITH_WINE_FIX_BUFOFFSET
    BYTE *cur_ptr;
    UINT conv_start, conv_end;
#endif

    TRACE("buffer %p.\n", buffer);

    if (buffer->resource.map_count)
    {
        WARN("Buffer is mapped, skipping preload.\n");
        return;
    }

    buffer->flags &= ~(WINED3D_BUFFER_NOSYNC | WINED3D_BUFFER_DISCARD);

    if (!buffer->buffer_object)
    {
        /* TODO: Make converting independent from VBOs */
        if (buffer->flags & WINED3D_BUFFER_CREATEBO)
        {
            context = context_acquire(device, NULL);
            buffer_create_buffer_object(buffer, context->gl_info);
            context_release(context);
            buffer->flags &= ~WINED3D_BUFFER_CREATEBO;
        }
        else
        {
            /* Not doing any conversion */
            return;
        }
    }

    /* Reading the declaration makes only sense if the stateblock is finalized and the buffer bound to a stream */
    if (device->isInDraw && buffer->resource.bind_count > 0)
    {
        decl_changed = buffer_find_decl(buffer);
        buffer->flags |= WINED3D_BUFFER_HASDESC;
    }

    if (!decl_changed && !(buffer->flags & WINED3D_BUFFER_HASDESC && buffer_is_dirty(buffer)))
    {
        ++buffer->draw_count;
        if (buffer->draw_count > VB_RESETDECLCHANGE)
            buffer->decl_change_count = 0;
        if (buffer->draw_count > VB_RESETFULLCONVS)
            buffer->full_conversion_count = 0;
        return;
    }

    /* If applications change the declaration over and over, reconverting all the time is a huge
     * performance hit. So count the declaration changes and release the VBO if there are too many
     * of them (and thus stop converting)
     */
    if (decl_changed)
    {
        ++buffer->decl_change_count;
        buffer->draw_count = 0;

        if (buffer->decl_change_count > VB_MAXDECLCHANGES
                || (buffer->conversion_map && (buffer->resource.usage & WINED3DUSAGE_DYNAMIC)))
        {
            FIXME("Too many declaration changes or converting dynamic buffer, stopping converting\n");

            buffer_unload(&buffer->resource);
            buffer->flags &= ~WINED3D_BUFFER_CREATEBO;

            /* The stream source state handler might have read the memory of
             * the vertex buffer already and got the memory in the vbo which
             * is not valid any longer. Dirtify the stream source to force a
             * reload. This happens only once per changed vertexbuffer and
             * should occur rather rarely. */
            device_invalidate_state(device, STATE_STREAMSRC);
            return;
        }

        /* The declaration changed, reload the whole buffer */
        WARN("Reloading buffer because of decl change\n");
        buffer_clear_dirty_areas(buffer);
        if (!buffer_add_dirty_area(buffer, 0, 0))
        {
            ERR("buffer_add_dirty_area failed, this is not expected\n");
            return;
        }
        /* Avoid unfenced updates, we might overwrite more areas of the buffer than the application
         * cleared for unsynchronized updates
         */
        flags = 0;
    }
    else
    {
        /* However, it is perfectly fine to change the declaration every now and then. We don't want a game that
         * changes it every minute drop the VBO after VB_MAX_DECL_CHANGES minutes. So count draws without
         * decl changes and reset the decl change count after a specific number of them
         */
        if (buffer->conversion_map && buffer_is_fully_dirty(buffer))
        {
            ++buffer->full_conversion_count;
            if (buffer->full_conversion_count > VB_MAXFULLCONVERSIONS)
            {
                FIXME("Too many full buffer conversions, stopping converting.\n");
                buffer_unload(&buffer->resource);
                buffer->flags &= ~WINED3D_BUFFER_CREATEBO;
                if (buffer->resource.bind_count)
                    device_invalidate_state(device, STATE_STREAMSRC);
                return;
            }
        }
        else
        {
            ++buffer->draw_count;
            if (buffer->draw_count > VB_RESETDECLCHANGE)
                buffer->decl_change_count = 0;
            if (buffer->draw_count > VB_RESETFULLCONVS)
                buffer->full_conversion_count = 0;
        }
    }

    if (buffer->buffer_type_hint == GL_ELEMENT_ARRAY_BUFFER_ARB)
        device_invalidate_state(device, STATE_INDEXBUFFER);

    if (!buffer->conversion_map)
    {
        /* That means that there is nothing to fixup. Just upload from
         * buffer->resource.allocatedMemory directly into the vbo. Do not
         * free the system memory copy because drawPrimitive may need it if
         * the stride is 0, for instancing emulation, vertex blending
         * emulation or shader emulation. */
        TRACE("No conversion needed.\n");

        /* Nothing to do because we locked directly into the vbo */
        if (!(buffer->flags & WINED3D_BUFFER_DOUBLEBUFFER))
        {
            return;
        }

        context = context_acquire(device, NULL);
        buffer_direct_upload(buffer, context->gl_info, flags);

        context_release(context);
        return;
    }

    context = context_acquire(device, NULL);
    gl_info = context->gl_info;

    if(!(buffer->flags & WINED3D_BUFFER_DOUBLEBUFFER))
    {
        buffer_get_sysmem(buffer, gl_info);
    }

    /* Now for each vertex in the buffer that needs conversion */
    vertices = buffer->resource.size / buffer->stride;

    data = HeapAlloc(GetProcessHeap(), 0, buffer->resource.size);

    while(buffer->modified_areas)
    {
        buffer->modified_areas--;
        start = buffer->maps[buffer->modified_areas].offset;
        len = buffer->maps[buffer->modified_areas].size;
        end = start + len;

#ifdef VBOX_WITH_WINE_FIX_BUFOFFSET
        if (buffer->offset > UINT32_MAX)
        {
            ERR("negative offset, unexpected!");
        }

        conv_start = wined3d_buffer_stream_offset_left(start, buffer->offset, buffer->stride);
        if (conv_start <= start)
        {
            start = conv_start;
        }
        else
        {
            /* <- < 0, add stride */
            conv_start += buffer->stride;
        }

        Assert(conv_start < UINT32_MAX/3);

        conv_end =  wined3d_buffer_stream_offset_right(end, buffer->offset, buffer->stride);
        if (conv_end > end)
        {
            if (conv_end <= buffer->resource.size)
                end = conv_end;
            else
                conv_end -= buffer->stride;
        }
        else if (conv_end < end)
        {
            ERR("unexpected!");
        }
        else
        {
            end = conv_end;
        }
#endif
        memcpy(data + start, buffer->resource.allocatedMemory + start, end - start);

#ifdef VBOX_WITH_WINE_FIX_BUFOFFSET
        for (cur_ptr = data + conv_start; cur_ptr < data + conv_end; cur_ptr += buffer->stride)
#else
        for (i = start / buffer->stride; i < min((end / buffer->stride) + 1, vertices); ++i)
#endif
        {
            for (j = 0; j < buffer->stride; ++j)
            {
                switch (buffer->conversion_map[j])
                {
                    case CONV_NONE:
                        /* Done already */
                        j += 3;
                        break;
                    case CONV_D3DCOLOR:
#ifdef VBOX_WITH_WINE_FIX_BUFOFFSET
                        fixup_d3dcolor((DWORD *) (cur_ptr + j));
#else
                        fixup_d3dcolor((DWORD *) (data + i * buffer->stride + j));
#endif
                        j += 3;
                        break;

                    case CONV_POSITIONT:
#ifdef VBOX_WITH_WINE_FIX_BUFOFFSET
                        fixup_transformed_pos((float *) (cur_ptr + j));
#else
                        fixup_transformed_pos((float *) (data + i * buffer->stride + j));
#endif
                        j += 15;
                        break;
                    default:
                        FIXME("Unimplemented conversion %d in shifted conversion\n", buffer->conversion_map[j]);
                }
            }
        }

        GL_EXTCALL(glBindBufferARB(buffer->buffer_type_hint, buffer->buffer_object));
        checkGLcall("glBindBufferARB");
        GL_EXTCALL(glBufferSubDataARB(buffer->buffer_type_hint, start, len, data + start));
        checkGLcall("glBufferSubDataARB");
    }

    HeapFree(GetProcessHeap(), 0, data);
    context_release(context);
}

static DWORD buffer_sanitize_flags(const struct wined3d_buffer *buffer, DWORD flags)
{
    /* Not all flags make sense together, but Windows never returns an error.
     * Catch the cases that could cause issues. */
    if (flags & WINED3D_MAP_READONLY)
    {
        if (flags & WINED3D_MAP_DISCARD)
        {
            WARN("WINED3D_MAP_READONLY combined with WINED3D_MAP_DISCARD, ignoring flags.\n");
            return 0;
        }
        if (flags & WINED3D_MAP_NOOVERWRITE)
        {
            WARN("WINED3D_MAP_READONLY combined with WINED3D_MAP_NOOVERWRITE, ignoring flags.\n");
            return 0;
        }
    }
    else if ((flags & (WINED3D_MAP_DISCARD | WINED3D_MAP_NOOVERWRITE))
            == (WINED3D_MAP_DISCARD | WINED3D_MAP_NOOVERWRITE))
    {
        WARN("WINED3D_MAP_DISCARD and WINED3D_MAP_NOOVERWRITE used together, ignoring.\n");
        return 0;
    }
    else if (flags & (WINED3D_MAP_DISCARD | WINED3D_MAP_NOOVERWRITE)
            && !(buffer->resource.usage & WINED3DUSAGE_DYNAMIC))
    {
        WARN("DISCARD or NOOVERWRITE map on non-dynamic buffer, ignoring.\n");
        return 0;
    }

    return flags;
}

static GLbitfield buffer_gl_map_flags(DWORD d3d_flags)
{
    GLbitfield ret = 0;

    if (!(d3d_flags & WINED3D_MAP_READONLY))
        ret |= GL_MAP_WRITE_BIT | GL_MAP_FLUSH_EXPLICIT_BIT;
    if (!(d3d_flags & (WINED3D_MAP_DISCARD | WINED3D_MAP_NOOVERWRITE)))
        ret |= GL_MAP_READ_BIT;

    if (d3d_flags & WINED3D_MAP_DISCARD)
        ret |= GL_MAP_INVALIDATE_BUFFER_BIT;
    if (d3d_flags & WINED3D_MAP_NOOVERWRITE)
        ret |= GL_MAP_UNSYNCHRONIZED_BIT;

    return ret;
}

struct wined3d_resource * CDECL wined3d_buffer_get_resource(struct wined3d_buffer *buffer)
{
    TRACE("buffer %p.\n", buffer);

    return &buffer->resource;
}

HRESULT CDECL wined3d_buffer_map(struct wined3d_buffer *buffer, UINT offset, UINT size, BYTE **data, DWORD flags)
{
    BOOL dirty = buffer_is_dirty(buffer);
    LONG count;

    TRACE("buffer %p, offset %u, size %u, data %p, flags %#x\n", buffer, offset, size, data, flags);

    flags = buffer_sanitize_flags(buffer, flags);
        if (flags & WINED3D_MAP_DISCARD)
        {
        /* DISCARD invalidates the entire buffer, regardless of the specified
         * offset and size. Some applications also depend on the entire buffer
         * being uploaded in that case. Two such applications are Port Royale
         * and Darkstar One. */
            if (!buffer_add_dirty_area(buffer, 0, 0))
                return E_OUTOFMEMORY;
        }
    else if (!(flags & WINED3D_MAP_READONLY))
        {
            if (!buffer_add_dirty_area(buffer, offset, size))
                return E_OUTOFMEMORY;
        }

    count = ++buffer->resource.map_count;

    if (buffer->buffer_object)
    {
        if (!(buffer->flags & WINED3D_BUFFER_DOUBLEBUFFER))
        {
            if (count == 1)
            {
                struct wined3d_device *device = buffer->resource.device;
                struct wined3d_context *context;
                const struct wined3d_gl_info *gl_info;

                context = context_acquire(device, NULL);
                gl_info = context->gl_info;

                if (buffer->buffer_type_hint == GL_ELEMENT_ARRAY_BUFFER_ARB)
                    context_invalidate_state(context, STATE_INDEXBUFFER);
                GL_EXTCALL(glBindBufferARB(buffer->buffer_type_hint, buffer->buffer_object));

                if (gl_info->supported[ARB_MAP_BUFFER_RANGE])
                {
                    GLbitfield mapflags = buffer_gl_map_flags(flags);
                    buffer->resource.allocatedMemory = GL_EXTCALL(glMapBufferRange(buffer->buffer_type_hint,
                            0, buffer->resource.size, mapflags));
                    checkGLcall("glMapBufferRange");
                }
                else
                {
                    if (buffer->flags & WINED3D_BUFFER_APPLESYNC)
                        buffer_sync_apple(buffer, flags, gl_info);
                    buffer->resource.allocatedMemory = GL_EXTCALL(glMapBufferARB(buffer->buffer_type_hint,
                            GL_READ_WRITE_ARB));
                    checkGLcall("glMapBufferARB");
                }

                if (((DWORD_PTR)buffer->resource.allocatedMemory) & (RESOURCE_ALIGNMENT - 1))
                {
                    WARN("Pointer %p is not %u byte aligned.\n", buffer->resource.allocatedMemory, RESOURCE_ALIGNMENT);

                    GL_EXTCALL(glUnmapBufferARB(buffer->buffer_type_hint));
                    checkGLcall("glUnmapBufferARB");
                    buffer->resource.allocatedMemory = NULL;

                    if (buffer->resource.usage & WINED3DUSAGE_DYNAMIC)
                    {
                        /* The extra copy is more expensive than not using VBOs at
                         * all on the Nvidia Linux driver, which is the only driver
                         * that returns unaligned pointers
                         */
                        TRACE("Dynamic buffer, dropping VBO\n");
                        buffer_unload(&buffer->resource);
                        buffer->flags &= ~WINED3D_BUFFER_CREATEBO;
                        if (buffer->resource.bind_count)
                            device_invalidate_state(device, STATE_STREAMSRC);
                    }
                    else
                    {
                        TRACE("Falling back to doublebuffered operation\n");
                        buffer_get_sysmem(buffer, gl_info);
                    }
                    TRACE("New pointer is %p.\n", buffer->resource.allocatedMemory);
                }
                context_release(context);
            }
        }
        else
        {
            if (dirty)
            {
                if (buffer->flags & WINED3D_BUFFER_NOSYNC && !(flags & WINED3D_MAP_NOOVERWRITE))
                {
                    buffer->flags &= ~WINED3D_BUFFER_NOSYNC;
                }
            }
            else if(flags & WINED3D_MAP_NOOVERWRITE)
            {
                buffer->flags |= WINED3D_BUFFER_NOSYNC;
            }

            if (flags & WINED3D_MAP_DISCARD)
            {
                buffer->flags |= WINED3D_BUFFER_DISCARD;
            }
        }
    }

    *data = buffer->resource.allocatedMemory + offset;

    TRACE("Returning memory at %p (base %p, offset %u).\n", *data, buffer->resource.allocatedMemory, offset);
    /* TODO: check Flags compatibility with buffer->currentDesc.Usage (see MSDN) */

    return WINED3D_OK;
}

void CDECL wined3d_buffer_unmap(struct wined3d_buffer *buffer)
{
    ULONG i;

    TRACE("buffer %p.\n", buffer);

    /* In the case that the number of Unmap calls > the
     * number of Map calls, d3d returns always D3D_OK.
     * This is also needed to prevent Map from returning garbage on
     * the next call (this will happen if the lock_count is < 0). */
    if (!buffer->resource.map_count)
    {
        WARN("Unmap called without a previous map call.\n");
        return;
    }

    if (--buffer->resource.map_count)
    {
        /* Delay loading the buffer until everything is unlocked */
        TRACE("Ignoring unmap.\n");
        return;
    }

    if (!(buffer->flags & WINED3D_BUFFER_DOUBLEBUFFER) && buffer->buffer_object)
    {
        struct wined3d_device *device = buffer->resource.device;
        const struct wined3d_gl_info *gl_info;
        struct wined3d_context *context;

        context = context_acquire(device, NULL);
        gl_info = context->gl_info;

        if (buffer->buffer_type_hint == GL_ELEMENT_ARRAY_BUFFER_ARB)
            context_invalidate_state(context, STATE_INDEXBUFFER);
        GL_EXTCALL(glBindBufferARB(buffer->buffer_type_hint, buffer->buffer_object));

        if (gl_info->supported[ARB_MAP_BUFFER_RANGE])
        {
            for (i = 0; i < buffer->modified_areas; ++i)
            {
                GL_EXTCALL(glFlushMappedBufferRange(buffer->buffer_type_hint,
                        buffer->maps[i].offset, buffer->maps[i].size));
                checkGLcall("glFlushMappedBufferRange");
            }
        }
        else if (buffer->flags & WINED3D_BUFFER_FLUSH)
        {
            for (i = 0; i < buffer->modified_areas; ++i)
            {
                GL_EXTCALL(glFlushMappedBufferRangeAPPLE(buffer->buffer_type_hint,
                        buffer->maps[i].offset, buffer->maps[i].size));
                checkGLcall("glFlushMappedBufferRangeAPPLE");
            }
        }

        GL_EXTCALL(glUnmapBufferARB(buffer->buffer_type_hint));
        if (wined3d_settings.strict_draw_ordering)
            gl_info->gl_ops.gl.p_glFlush(); /* Flush to ensure ordering across contexts. */
        context_release(context);

        buffer->resource.allocatedMemory = NULL;
        buffer_clear_dirty_areas(buffer);
    }
    else if (buffer->flags & WINED3D_BUFFER_HASDESC)
    {
        wined3d_buffer_preload(buffer);
    }
}

static const struct wined3d_resource_ops buffer_resource_ops =
{
    buffer_unload,
};

static HRESULT buffer_init(struct wined3d_buffer *buffer, struct wined3d_device *device,
        UINT size, DWORD usage, enum wined3d_format_id format_id, enum wined3d_pool pool, GLenum bind_hint,
        const char *data, void *parent, const struct wined3d_parent_ops *parent_ops)
{
    const struct wined3d_gl_info *gl_info = &device->adapter->gl_info;
    const struct wined3d_format *format = wined3d_get_format(gl_info, format_id);
    HRESULT hr;
    BOOL dynamic_buffer_ok;

    if (!size)
    {
        WARN("Size 0 requested, returning WINED3DERR_INVALIDCALL\n");
        return WINED3DERR_INVALIDCALL;
    }

    hr = resource_init(&buffer->resource, device, WINED3D_RTYPE_BUFFER, format,
            WINED3D_MULTISAMPLE_NONE, 0, usage, pool, size, 1, 1, size,
            parent, parent_ops, &buffer_resource_ops
#ifdef VBOX_WITH_WDDM
            , NULL, NULL /* <- no need this info here so far */
#endif
            );
    if (FAILED(hr))
    {
        WARN("Failed to initialize resource, hr %#x\n", hr);
        return hr;
    }
    buffer->buffer_type_hint = bind_hint;

    TRACE("size %#x, usage %#x, format %s, memory @ %p, iface @ %p.\n", buffer->resource.size, buffer->resource.usage,
            debug_d3dformat(buffer->resource.format->id), buffer->resource.allocatedMemory, buffer);

    if (device->create_parms.flags & WINED3DCREATE_SOFTWARE_VERTEXPROCESSING)
    {
        /* SWvp always returns the same pointer in buffer maps and retains data in DISCARD maps.
         * Keep a system memory copy of the buffer to provide the same behavior to the application.
         * Still use a VBO to support OpenGL 3 core contexts. */
        TRACE("Using doublebuffer mode because of software vertex processing\n");
        buffer->flags |= WINED3D_BUFFER_DOUBLEBUFFER;
    }

    dynamic_buffer_ok = gl_info->supported[APPLE_FLUSH_BUFFER_RANGE] || gl_info->supported[ARB_MAP_BUFFER_RANGE];

    /* Observations show that drawStridedSlow is faster on dynamic VBs than converting +
     * drawStridedFast (half-life 2 and others).
     *
     * Basically converting the vertices in the buffer is quite expensive, and observations
     * show that drawStridedSlow is faster than converting + uploading + drawStridedFast.
     * Therefore do not create a VBO for WINED3DUSAGE_DYNAMIC buffers.
     */
    if (!gl_info->supported[ARB_VERTEX_BUFFER_OBJECT])
    {
        TRACE("Not creating a vbo because GL_ARB_vertex_buffer is not supported\n");
    }
#ifndef VBOX_WITH_WDDM /* @todo: extend this to VBOX in general */
    else if(buffer->resource.pool == WINED3D_POOL_SYSTEM_MEM)
    {
        TRACE("Not creating a vbo because the vertex buffer is in system memory\n");
    }
    else if(!dynamic_buffer_ok && (buffer->resource.usage & WINED3DUSAGE_DYNAMIC))
    {
        TRACE("Not creating a vbo because the buffer has dynamic usage and no GL support\n");
    }
#endif
    else
    {
        buffer->flags |= WINED3D_BUFFER_CREATEBO;
    }

    if (data)
    {
        BYTE *ptr;

        hr = wined3d_buffer_map(buffer, 0, size, &ptr, 0);
        if (FAILED(hr))
        {
            ERR("Failed to map buffer, hr %#x\n", hr);
            buffer_unload(&buffer->resource);
            resource_cleanup(&buffer->resource);
            return hr;
        }

        memcpy(ptr, data, size);

        wined3d_buffer_unmap(buffer);
    }

    buffer->maps = HeapAlloc(GetProcessHeap(), 0, sizeof(*buffer->maps));
    if (!buffer->maps)
    {
        ERR("Out of memory\n");
        buffer_unload(&buffer->resource);
        resource_cleanup(&buffer->resource);
        return E_OUTOFMEMORY;
    }
    buffer->maps_size = 1;

    return WINED3D_OK;
}

HRESULT CDECL wined3d_buffer_create(struct wined3d_device *device, struct wined3d_buffer_desc *desc, const void *data,
        void *parent, const struct wined3d_parent_ops *parent_ops, struct wined3d_buffer **buffer)
{
    struct wined3d_buffer *object;
    HRESULT hr;

    TRACE("device %p, desc %p, data %p, parent %p, buffer %p\n", device, desc, data, parent, buffer);

    object = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*object));
    if (!object)
        return E_OUTOFMEMORY;

    FIXME("Ignoring access flags (pool)\n");

    hr = buffer_init(object, device, desc->byte_width, desc->usage, WINED3DFMT_UNKNOWN,
            WINED3D_POOL_MANAGED, GL_ARRAY_BUFFER_ARB, data, parent, parent_ops);
    if (FAILED(hr))
    {
        WARN("Failed to initialize buffer, hr %#x.\n", hr);
        HeapFree(GetProcessHeap(), 0, object);
        return hr;
    }
    object->desc = *desc;

    TRACE("Created buffer %p.\n", object);

    *buffer = object;

    return WINED3D_OK;
}

HRESULT CDECL wined3d_buffer_create_vb(struct wined3d_device *device, UINT size, DWORD usage, enum wined3d_pool pool,
        void *parent, const struct wined3d_parent_ops *parent_ops, struct wined3d_buffer **buffer)
{
    struct wined3d_buffer *object;
    HRESULT hr;

    TRACE("device %p, size %u, usage %#x, pool %#x, parent %p, parent_ops %p, buffer %p.\n",
            device, size, usage, pool, parent, parent_ops, buffer);

    if (pool == WINED3D_POOL_SCRATCH)
    {
        /* The d3d9 tests shows that this is not allowed. It doesn't make much
         * sense anyway, SCRATCH buffers wouldn't be usable anywhere. */
        WARN("Vertex buffer in WINED3D_POOL_SCRATCH requested, returning WINED3DERR_INVALIDCALL.\n");
        *buffer = NULL;
        return WINED3DERR_INVALIDCALL;
    }

    object = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*object));
    if (!object)
    {
        *buffer = NULL;
        return WINED3DERR_OUTOFVIDEOMEMORY;
    }

    hr = buffer_init(object, device, size, usage, WINED3DFMT_VERTEXDATA,
            pool, GL_ARRAY_BUFFER_ARB, NULL, parent, parent_ops);
    if (FAILED(hr))
    {
        WARN("Failed to initialize buffer, hr %#x.\n", hr);
        HeapFree(GetProcessHeap(), 0, object);
        return hr;
    }

    TRACE("Created buffer %p.\n", object);
    *buffer = object;

    return WINED3D_OK;
}

HRESULT CDECL wined3d_buffer_create_ib(struct wined3d_device *device, UINT size, DWORD usage, enum wined3d_pool pool,
        void *parent, const struct wined3d_parent_ops *parent_ops, struct wined3d_buffer **buffer)
{
    struct wined3d_buffer *object;
    HRESULT hr;

    TRACE("device %p, size %u, usage %#x, pool %#x, parent %p, parent_ops %p, buffer %p.\n",
            device, size, usage, pool, parent, parent_ops, buffer);

    object = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*object));
    if (!object)
    {
        *buffer = NULL;
        return WINED3DERR_OUTOFVIDEOMEMORY;
    }

    hr = buffer_init(object, device, size, usage | WINED3DUSAGE_STATICDECL,
            WINED3DFMT_UNKNOWN, pool, GL_ELEMENT_ARRAY_BUFFER_ARB, NULL,
            parent, parent_ops);
    if (FAILED(hr))
    {
        WARN("Failed to initialize buffer, hr %#x\n", hr);
        HeapFree(GetProcessHeap(), 0, object);
        return hr;
    }

    TRACE("Created buffer %p.\n", object);
    *buffer = object;

    return WINED3D_OK;
}
