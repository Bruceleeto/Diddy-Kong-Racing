#ifndef DKR_NAT_GBI_H
#define DKR_NAT_GBI_H

// With the native renderer the game does not build display lists: each command
// the game's code gives becomes a call to dreamcast/native_state.c, which holds the
// state and turns draws into records for the frame (dreamcast/native.c). The `pkt`
// argument is not evaluated, so the game's list pointers stay where they are.
// Included at the end of include/f3ddkr.h. The gs* forms are left alone: they are
// the game's render tables, which are read as data.

#define NAT_DIRECT 1

void nat_color(u32 cmd, u32 rgba);
void nat_combine(u32 w0, u32 w1);
void nat_other(u32 high, u32 shift, u32 length, u32 data);
void nat_other_both(u32 high, u32 low);
void nat_geometry(u32 set, u32 clear);
void nat_moveword(u32 index, u32 data);
void nat_matrix(const void *matrix, u32 slot);
void nat_viewport(const void *viewport);
void nat_scissor(f32 x0, f32 y0, f32 x1, f32 y1);
void nat_vertices(const void *verts, s32 count, s32 append);
void nat_polygon(const void *tris, s32 count, s32 textured);
void nat_table(const void *entry, s32 count);
void nat_image(const void *image, s32 fmt, s32 siz, s32 width, s32 height, s32 cms, s32 cmt, s32 swapped);
void nat_texrect(s32 xl, s32 yl, s32 xh, s32 yh, s32 s, s32 t, s32 dsdx, s32 dtdy);
void nat_fillrect(s32 ulx, s32 uly, s32 lrx, s32 lry);
void nat_tag(u32 tag);
void nat_ignore(s32 ignore); // while the game prepares a texture or a sprite: what it would have put in a list
void nat_defer_begin(void);  // what follows is cut from here and drawn at nat_defer_flush
void nat_defer_end(void);
void nat_defer_flush(void);

#define NAT_PACK(r, g, b, a) \
    ((((u32) (r) & 0xFF) << 24) | (((u32) (g) & 0xFF) << 16) | (((u32) (b) & 0xFF) << 8) | ((u32) (a) & 0xFF))

#undef gDPNoParam
#define gDPNoParam(pkt, cmd) ((void) 0)
#undef gDPNoOpTag
#define gDPNoOpTag(pkt, tag) nat_tag(tag)
#undef gSPNoOp
#define gSPNoOp(pkt) ((void) 0)
#undef gSPEndDisplayList
#define gSPEndDisplayList(pkt) ((void) 0)
#undef gSPBranchList
#define gSPBranchList(pkt, dl) ((void) 0)
#undef gSPPerspNormalize
#define gSPPerspNormalize(pkt, s) ((void) 0)
#undef gSetImage
#define gSetImage(pkt, cmd, f, s, w, i) ((void) 0)
#undef gDPLoadTLUT_pal16
#define gDPLoadTLUT_pal16(pkt, pal, dram) ((void) 0)

#undef gDPSetEnvColor
#define gDPSetEnvColor(pkt, r, g, b, a) nat_color(G_SETENVCOLOR, NAT_PACK(r, g, b, a))
#undef gDPSetBlendColor
#define gDPSetBlendColor(pkt, r, g, b, a) nat_color(G_SETBLENDCOLOR, NAT_PACK(r, g, b, a))
#undef gDPSetFogColor
#define gDPSetFogColor(pkt, r, g, b, a) nat_color(G_SETFOGCOLOR, NAT_PACK(r, g, b, a))
#undef gDPSetColor
#define gDPSetColor(pkt, c, d) nat_color(c, d)
#undef gDPSetPrimColor
#define gDPSetPrimColor(pkt, m, l, r, g, b, a) nat_color(G_SETPRIMCOLOR, NAT_PACK(r, g, b, a))
#undef gDPSetPrimColorRGBA
#define gDPSetPrimColorRGBA(pkt, rgba) nat_color(G_SETPRIMCOLOR, rgba)
#undef gDPSetEnvColorRGBA
#define gDPSetEnvColorRGBA(pkt, rgba) nat_color(G_SETENVCOLOR, rgba)

#undef gSPSetOtherMode
#define gSPSetOtherMode(pkt, cmd, sft, len, data) nat_other((cmd) == G_SETOTHERMODE_H, sft, len, data)
#undef gDPSetOtherMode
#define gDPSetOtherMode(pkt, mode0, mode1) nat_other_both(mode0, mode1)
#undef gDPSetCombineLERP
#define gDPSetCombineLERP(pkt, a0, b0, c0, d0, Aa0, Ab0, Ac0, Ad0, a1, b1, c1, d1, Aa1, Ab1, Ac1, Ad1) \
    do {                                                                                                \
        Gfx _c = gsDPSetCombineLERP(a0, b0, c0, d0, Aa0, Ab0, Ac0, Ad0, a1, b1, c1, d1, Aa1, Ab1, Ac1, Ad1); \
        nat_combine(_c.words.w0, _c.words.w1);                                                          \
    } while (0)
#undef gSPSetGeometryMode
#define gSPSetGeometryMode(pkt, word) nat_geometry(word, 0)
#undef gSPClearGeometryMode
#define gSPClearGeometryMode(pkt, word) nat_geometry(0, word)

#undef gMoveWd
#define gMoveWd(pkt, index, offset, data) nat_moveword(index, data)
#undef gSPMatrix
#define gSPMatrix(pkt, m, p) nat_matrix(m, ((p) >> 6) & 3)
#undef gSPViewport
#define gSPViewport(pkt, v) nat_viewport(v)
#undef gDPSetScissor
#define gDPSetScissor(pkt, mode, ulx, uly, lrx, lry) nat_scissor(ulx, uly, lrx, lry)

#undef gSPVertexDKR
#define gSPVertexDKR(pkt, v, n, v0) nat_vertices(v, n, v0)
#undef gSPPolygon
#define gSPPolygon(dl, ptr, numTris, texEnabled) nat_polygon(ptr, numTris, texEnabled)
#undef gDkrDmaDisplayList
#define gDkrDmaDisplayList(pkt, address, numberOfCommands) nat_table((const void *) (address), numberOfCommands)
// A static list of modes is a function of its own; see the _nat functions next to them.
#undef gSPDisplayList
#define gSPDisplayList(pkt, dl) dl##_nat()

#undef gDPFillRectangle
#define gDPFillRectangle(pkt, ulx, uly, lrx, lry) nat_fillrect(ulx, uly, lrx, lry)
#undef gSPTextureRectangle
#define gSPTextureRectangle(pkt, xl, yl, xh, yh, tile, s, t, dsdx, dtdy) nat_texrect(xl, yl, xh, yh, s, t, dsdx, dtdy)

#undef gDPLoadTextureBlock
#define gDPLoadTextureBlock(pkt, timg, fmt, siz, width, height, pal, cms, cmt, masks, maskt, shifts, shiftt) \
    nat_image(timg, fmt, siz, width, height, cms, cmt, FALSE)
#undef gDPLoadTextureBlockS
#define gDPLoadTextureBlockS(pkt, timg, fmt, siz, width, height, pal, cms, cmt, masks, maskt, shifts, shiftt) \
    nat_image(timg, fmt, siz, width, height, cms, cmt, TRUE)
#undef gDPLoadTextureBlock_4b
#define gDPLoadTextureBlock_4b(pkt, timg, fmt, width, height, pal, cms, cmt, masks, maskt, shifts, shiftt) \
    nat_image(timg, fmt, G_IM_SIZ_4b, width, height, cms, cmt, FALSE)
#undef gDPLoadTextureBlock_4bS
#define gDPLoadTextureBlock_4bS(pkt, timg, fmt, width, height, pal, cms, cmt, masks, maskt, shifts, shiftt) \
    nat_image(timg, fmt, G_IM_SIZ_4b, width, height, cms, cmt, TRUE)
#undef gDPLoadMultiBlock
#define gDPLoadMultiBlock(pkt, timg, tmem, rtile, fmt, siz, width, height, pal, cms, cmt, masks, maskt, shifts, shiftt) \
    do {                                                                                                                \
        if ((rtile) == G_TX_RENDERTILE) {                                                                               \
            nat_image(timg, fmt, siz, width, height, cms, cmt, FALSE);                                                  \
        }                                                                                                               \
    } while (0)
#undef gDPLoadMultiBlock_4bS
#define gDPLoadMultiBlock_4bS(pkt, timg, tmem, rtile, fmt, width, height, pal, cms, cmt, masks, maskt, shifts, shiftt) \
    do {                                                                                                                \
        if ((rtile) == G_TX_RENDERTILE) {                                                                               \
            nat_image(timg, fmt, G_IM_SIZ_4b, width, height, cms, cmt, TRUE);                                           \
        }                                                                                                               \
    } while (0)

#endif
