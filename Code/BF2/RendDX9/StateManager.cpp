// Path: [INFERRED]; StateManager is named by RendDX9Init.cpp:232.
// Deferred D3D state cache, recovered from RendDX9.dll (0x10025980..0x10027790).
// Setters queue changes; draw wrappers apply them. Pop schedules restoration for
// the next draw. Transparency-AA globals begin at zero and are set by the later probe.

// See Code/BF2/RendDX9/BF2Render.cpp for the BF2_IMAGE_RENDDX9 guard and why it is here.
#ifdef BF2_IMAGE_RENDDX9

#include "../../RendDX9Abi/ThunkAbi.h"
#include "../../RendDX9Abi/HfeAbi.h"
#include "../../RendDX9Abi/ModuleCommon.h"
#include "../../RendDX9Abi/BfMemory.h"
#include "../../RendDX9Abi/Frontier.h"
#include "RendDX9Base.h"
#include "DrawStats.h"   // [DEV] BF2_DRAW_STATS

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <intrin.h>
#include <stddef.h>
#include <stdint.h>

#if defined(_MSC_VER)
extern "C" int __cdecl _purecall();
#else
extern "C" void __cxa_pure_virtual();
#endif

namespace dice {
namespace hfe {
namespace dx9rend {

extern void* g_rendererBase;    // dword_1023B970 - RendDX9Init.cpp; device at +0x0C
extern void* g_shaderManager;   // dword_1023B97C - ShaderManager.cpp

// ShaderManager.cpp - the pre-draw CommitChanges walk over the three current-effect slots
// (+0xA0 raw ID3DXEffect, +0x98 Effect wrapper, +0x9C the states-effect arm).
void shaderManagerCommitCurrentEffects();

// dword_1023B984. Everything below publishes and consumes it through this name.
void* g_stateManager = 0;

// dword_1023D08C, char[4] at 0x1023D090, char[4] at all zero until the vendor probe
// runs (see the header note). The on/off buffers are adjacent in the shipped image. Declared in
// RendDX9Base.h: the probe writes them, the ShaderManager and the Ra
// EffectManager print them.
unsigned int  g_transparencyAAState = 0;
unsigned char g_transparencyAAOn[4]  = { 0, 0, 0, 0 };
unsigned char g_transparencyAAOff[4] = { 0, 0, 0, 0 };

// [RENDER] Reversed depth. Every RenderView projection maps near to 1 and far to 0
// (RendMisc/RenderView.cpp). While any depth buffer other than a shadow map is bound, the issued
// ZFUNC is mirrored (LESS <-> GREATER) and DEPTHBIAS / SLOPESCALEDEPTHBIAS are negated, and depth
// clears write 1 - z. The mirror cells keep the effects' logical (forward-Z) values, so shaders and
// callers stay unchanged. The sun cascades (SunShadows.cpp) mark their atlas depth standard (it
// uses the cascades' own light matrices) and clear the mark when they rebind the scene depth.
namespace {
bool g_standardDepthBound = false;   // a shadow-map depth buffer is the wanted depth-stencil
}

bool stateManagerReversedDepthBound()
{
    return !g_standardDepthBound;
}

void stateManagerMarkStandardDepth(bool standard)
{
    g_standardDepthBound = standard;
}

float stateManagerClearDepth(float depth)
{
    return stateManagerReversedDepthBound() ? 1.0f - depth : depth;
}

// [RENDER] Prepassed draws (milestone 8). The depth prepass has already written a prepassed draw's
// depth into the scene depth, so its lit passes test EQUAL and write nothing, whatever the
// effect states: each pixel then shades only the samples the prepass left it. The same holds for
// coverage: the prepass's alpha test (with alpha-to-coverage) decided which samples the draw
// owns, so its lit passes run no alpha test and need not agree with the prepass on alpha. The
// effects' own ZFUNC / ZWRITEENABLE / ALPHATESTENABLE stay in the mirror cells; only the issued
// values change.
namespace {
bool g_prepassedDraw = false;
}

unsigned int stateManagerMapDepthState(unsigned int state, unsigned int value)
{
    if (g_prepassedDraw)
    {
        if (state == 23u)   // D3DRS_ZFUNC
            return 3u;      // D3DCMP_EQUAL, the same in either depth convention
        if (state == 14u)   // D3DRS_ZWRITEENABLE
            return 0u;
        if (state == 15u)   // D3DRS_ALPHATESTENABLE
            return 0u;
    }
    if (!stateManagerReversedDepthBound())
        return value;
    if (state == 23u)   // D3DRS_ZFUNC
    {
        switch (value)
        {
        case 2u: return 5u;   // LESS -> GREATER
        case 4u: return 7u;   // LESSEQUAL -> GREATEREQUAL
        case 5u: return 2u;
        case 7u: return 4u;
        default: return value;
        }
    }
    if (state == 175u || state == 195u)   // D3DRS_SLOPESCALEDEPTHBIAS, D3DRS_DEPTHBIAS
    {
        float f;
        std::memcpy(&f, &value, 4);
        f = -f;
        std::memcpy(&value, &f, 4);
    }
    return value;
}


// Cached state value and dirty tracking, with fixed-size mirror blocks per state category.

// One mirrored value. The texture block's cells carry the raw IDirect3DBaseTexture9* the setter is
// handed, so the value is pointer-sized: uintptr_t is unsigned int on x86 (the retail 4-byte cell
// word) and 64 bits on x64, where the render-state, sampler and TTF cells hold their 32-bit D3D
// values zero-extended. [CHOSEN, not recovered] x64 cell width (issue #7).
typedef uintptr_t StateValue;

static_assert(sizeof(StateValue) == sizeof(void*), "a cell value holds a texture pointer");

struct StateCell
{
    StateValue    f0Device;    // what the device has been told         (issue walk writes)
    StateValue    f1Base;      // what pop restores to                  (reset seeds; D3D defaults)
    StateValue    f2Wanted;    // what the next draw will issue         (setters write)
    unsigned char flags;       // bit0 touched-list, bit1 pending-list, bit2 base-seeded
    unsigned char pad[3];
};

// Two-slot block vtable. Both ends of every dispatch are in this repository, so the slots use the
// front's __fastcall shim shape (ecx -> self, edx dead, stack args, callee-clean) which is
// call-compatible with the shipped __thiscall sites.
//     slot 0 = issue(index, value)  - the one D3D call this block family makes
//     slot 1 = copy(dst, src)       - *dst = *src; the texture block overrides it refcounted
typedef void (__thiscall* BlockIssueFn)(void* self, unsigned int index,
                                        StateValue value);
typedef void (__thiscall* BlockCopyFn)(void* self, StateValue* dst,
                                       StateValue* src);

template <unsigned int N>
struct StateMirrorBlock
{
    void* const*  vptr;
    StateCell     cells[N];
    unsigned char listTouched[N];
    unsigned char countTouched;
    unsigned char listPending[N];
    unsigned char countPending;
};

// The two indexed families carry their D3D stage in a trailing dword the issuer reads
// (sub_10027740 reads this[74] of the 300-byte block; sub_10027770 reads this[6] of the 28-byte
// one). The constructor stamps 0..7 into them.
struct SamplerBlock : StateMirrorBlock<16> { unsigned int stage; };
struct TtfBlock     : StateMirrorBlock<1>  { unsigned int stage; };

typedef StateMirrorBlock<255> RenderStateBlock;
typedef StateMirrorBlock<16>  TextureBlock;

// The retail x86 layout. x64 cells and blocks are native; nothing reads them by byte offset.
#if defined(_M_IX86)
static_assert(sizeof(StateCell)        == 16,   "the mirror cell is 16 bytes");
static_assert(sizeof(RenderStateBlock) == 4596, "render-state block: this+0x10..+0x1203");
static_assert(sizeof(TextureBlock)     == 296,  "texture block: this+0x1204..+0x132B");
static_assert(sizeof(SamplerBlock)     == 300,  "one sampler block per stage");
static_assert(sizeof(TtfBlock)         == 28,   "one texture-transform block per stage");
static_assert(offsetof(RenderStateBlock, listTouched) == 4084, "touched list at block+4084");
static_assert(offsetof(RenderStateBlock, countTouched) == 4339, "touched count at block+4339");
static_assert(offsetof(RenderStateBlock, listPending) == 4340, "pending list at block+4340");
static_assert(offsetof(RenderStateBlock, countPending) == 4595, "pending count at block+4595");
static_assert(offsetof(SamplerBlock, stage) == 296, "sampler stage index at block+296");
static_assert(offsetof(TtfBlock, stage)     == 24,  "TTF stage index at block+24");
#endif


// The StateManager object - 16032 (0x3EA0) bytes, every field placed by the constructor
// (0x10026AB0), the reset (0x10025D00) and the consumers that read them back.

typedef HRESULT (__stdcall* ConstantIssueFn)(IDirect3DDevice9*, unsigned int,
                                             const float*, unsigned int);

struct StateManager
{
    /* 0x0000 */ void* const*     vptr;            // 26 slots at off_101DFEA8
    /* 0x0004 */ void* const*     listenerVptr;    // 3 slots at off_101DFE98 (lost/reset/destroy)
    /* 0x0008 */ int              refCount;        // ctor: 1
    /* 0x000C */ IDirect3DDevice9* device;         // AddRef'd by the ctor
    /* 0x0010 */ RenderStateBlock renderStates;
    /* 0x1204 */ TextureBlock     textures;        // 16 stages; refcounted copy override
    /* 0x132C */ SamplerBlock     samplers[8];
    /* 0x1C8C */ TtfBlock         textureTransforms[8];
    /* 0x1D6C */ unsigned int     pushDepth;       // this[1883]; pop at zero runs the restore walk

    // plain value+dirty mirrors, flushed by the pre-draw apply
    /* 0x1D70 */ void*            streamVB[16];      // reset: 0
    /* 0x1DB0 */ unsigned int     streamOffset[16];  // reset: -1
    /* 0x1DF0 */ unsigned int     streamStride[16];  // reset: -1
    /* 0x1E30 */ unsigned int     streamDirty[16];   // reset: 0
    /* 0x1E70 */ void*            indices;
    /* 0x1E74 */ unsigned int     indicesDirty;
    /* 0x1E78 */ void*            vertexDecl;
    /* 0x1E7C */ unsigned int     vertexDeclDirty;
    /* 0x1E80 */ void*            vertexShader;
    /* 0x1E84 */ unsigned int     vertexShaderDirty;
    /* 0x1E88 */ void*            pixelShader;
    /* 0x1E8C */ unsigned int     pixelShaderDirty;
    /* 0x1E90 */ IDirect3DSurface9* renderTarget;   // refcounted (slot +0x60)
    /* 0x1E94 */ unsigned char    renderTargetDirty;
    /* 0x1E95 */ unsigned char    pad1E95[3];
    /* 0x1E98 */ IDirect3DSurface9* depthStencil;   // refcounted (slot +0x64)
    /* 0x1E9C */ unsigned char    depthStencilDirty;
    /* 0x1E9D */ unsigned char    pad1E9D[3];
    /* 0x1EA0 */ unsigned int     maxStreamTouched;  // this[1960]; SetStreamSource raises it
    /* 0x1EA4 */ unsigned int     maxStageTouched;   // this[1961]; SetSamplerState raises it;
                                                     // pop AND pre-draw loop stages 0..this value

    // the shader-constant register files
    // Batched flush: each contiguous dirty run is issued as ONE Set*ShaderConstantF, clearing as it
    // goes. [RENDER] The dirty registers are a bit mask walked a 64-bit word at a time. Retail kept
    // a dirty byte per register and scanned the bytes between the lowest and highest register
    // written; with material constants from c0 and the LOD dither at c222, every draw scanned
    // about 210 clean bytes (two thirds of the pre-draw apply).
    /* 0x1EA8 */ float            vsConst[256][4];
    /*        */ uint64_t         vsConstDirty[4];
    /*        */ ConstantIssueFn  vsConstIssueFn;  // sub_100272C0 - device SetVertexShaderConstantF
    /*        */ float            psConst[224][4];
    /*        */ uint64_t         psConstDirty[4];
    /*        */ ConstantIssueFn  psConstIssueFn;  // sub_100272B0 - device SetPixelShaderConstantF

    // [RENDER] Render targets 1..3 (MRT). The shipped object mirrors only RT0 and
    // every shipped caller passes index 0; these sit past the retail fields so its layout holds.
    /* 0x3EA0 */ IDirect3DSurface9* extraRenderTarget[3];   // refcounted, RT1..RT3
    /*        */ unsigned char    extraRenderTargetDirty[3];

    void setRenderStatePersistent(unsigned int state, unsigned int value);
    void setTransparencyAA(bool enabled);
};

// The retail x86 layout. createStateManager allocates sizeof(StateManager); every other TU reaches
// the object through the seams below (the listener and RT0 accessors included), so the x64 object is
// native.
#if defined(_M_IX86)
// RendDX9Init.cpp:232 allocates 0x3EA0; the MRT slots extend the object past it.
static_assert(offsetof(StateManager, extraRenderTarget) == 0x3EA0, "MRT slots follow the retail object");
static_assert(offsetof(StateManager, renderStates)      == 0x0010, "RS block at +0x10");
static_assert(offsetof(StateManager, textures)          == 0x1204, "texture block at +0x1204");
static_assert(offsetof(StateManager, samplers)          == 0x132C, "sampler blocks at +0x132C");
static_assert(offsetof(StateManager, textureTransforms) == 0x1C8C, "TTF blocks at +0x1C8C");
static_assert(offsetof(StateManager, pushDepth)         == 0x1D6C, "push depth at +0x1D6C");
static_assert(offsetof(StateManager, streamVB)          == 0x1D70, "stream VBs at +0x1D70");
static_assert(offsetof(StateManager, indices)           == 0x1E70, "indices mirror at +0x1E70");
static_assert(offsetof(StateManager, renderTarget)      == 0x1E90, "RT mirror at +0x1E90");
static_assert(offsetof(StateManager, maxStageTouched)   == 0x1EA4, "stage watermark at +0x1EA4");
static_assert(offsetof(StateManager, vsConst)           == 0x1EA8, "VS constants at +0x1EA8");
static_assert(offsetof(StateManager, listenerVptr)      == 0x0004, "listener subobject at +0x04");
static_assert(offsetof(StateManager, renderTargetDirty) == 0x1E94, "RT dirty byte at +0x1E94");
#endif

// The two vtable arrays, defined below the slot bodies; smRelease restores them at destruction.
extern void* const kStateManagerVtable[26];
extern void* const kStateManagerListenerVtable[3];

namespace {

inline IDirect3DDevice9* rendererDevice()
{
    return static_cast<RendDX9Base*>(g_rendererBase)->device;
}

inline StateManager* sm() { return static_cast<StateManager*>(g_stateManager); }


// The block vtable slot bodies.


// Shared copy, 0x10027350 (comdat-folded into every non-texture vtable): *dst = *src.
void RD9_THUNK blockCopyPlain(void* RD9_THUNK_EDX, StateValue* dst, StateValue* src)
{
    *dst = *src;
}

// The texture block's override, Release the outgoing reference, AddRef the incoming.
// The shipped body calls vtable +0x08 to release and +0x04 to add - IUnknown's Release/AddRef
// slots, __stdcall with the object pushed - because what travels through these cells is the RAW
// IDirect3DBaseTexture9 the setter was handed (see smSetTexture).
// PE 0x100276F0
void RD9_THUNK blockCopyTextureRef(void* RD9_THUNK_EDX, StateValue* dst, StateValue* src)
{
    if (*dst)
        reinterpret_cast<IUnknown*>(*dst)->Release();
    *dst = *src;
    if (*src)
        reinterpret_cast<IUnknown*>(*src)->AddRef();
}

// Applies render state with vendor-specific transparency AA codes.
// Any nonzero unsigned request enables it when multisample AA is active.
// PE 0x10027610
void RD9_THUNK rsIssue(void* self RD9_THUNK_EDX, unsigned int state, StateValue value)
{
    RenderStateBlock* block = static_cast<RenderStateBlock*>(self);

    if (state != g_transparencyAAState)
    {
        rendererDevice()->SetRenderState(static_cast<D3DRENDERSTATETYPE>(state),
            static_cast<DWORD>(stateManagerMapDepthState(state, static_cast<unsigned int>(value))));
        return;
    }

    if (block->cells[161].f0Device != 1)   // D3DRS_MULTISAMPLEANTIALIAS
        rendererDevice()->SetRenderState(D3DRS_MULTISAMPLEANTIALIAS, 1);

    unsigned int onCode, offCode;
    ::memcpy(&onCode, g_transparencyAAOn, 4);
    ::memcpy(&offCode, g_transparencyAAOff, 4);

    unsigned int snapped = static_cast<unsigned int>(value);   // a render-state value is 32-bit
    if (value != onCode && value != offCode)
        snapped = (static_cast<double>(value) >= 0.5) ? onCode : offCode;
    rendererDevice()->SetRenderState(static_cast<D3DRENDERSTATETYPE>(state), snapped);

    if (block->cells[161].f0Device != 1)
        rendererDevice()->SetRenderState(D3DRS_MULTISAMPLEANTIALIAS, 0);
}

// Texture issue, 0x10027720. The cell value is an EngineTexture wrapper; the D3D base texture is
// unwrapped by the pre-draw's own SetTexture caller? No - it is not: the shipped issuer hands the
// value straight to the device, and the setter (slot +0x20) is given the D3D base texture already
// unwrapped by its callers. What travels through the texture cells is therefore the RAW
// IDirect3DBaseTexture9 on the issue side and the refcounted wrapper never reaches the device.
// See smSetTexture below - D3DX calls it with the raw pointer it got from FXParameter's unwrap.
void RD9_THUNK texIssue(void* RD9_THUNK_EDX, unsigned int stage, StateValue value)
{
    rendererDevice()->SetTexture(stage, reinterpret_cast<IDirect3DBaseTexture9*>(value));
}

// Sampler issue, the stage rides in the block's own tail dword.
// PE 0x10027740.
void RD9_THUNK samplerIssue(void* self RD9_THUNK_EDX, unsigned int type, StateValue value)
{
    rendererDevice()->SetSamplerState(static_cast<SamplerBlock*>(self)->stage,
                                      static_cast<D3DSAMPLERSTATETYPE>(type),
                                      static_cast<DWORD>(value));
}

// Texture-transform issue, always D3DTSS 24 (texturetransformflags), index ignored.
// PE 0x10027770.
void RD9_THUNK ttfIssue(void* self RD9_THUNK_EDX, unsigned int, StateValue value)
{
    rendererDevice()->SetTextureStageState(static_cast<TtfBlock*>(self)->stage,
                                           D3DTSS_TEXTURETRANSFORMFLAGS,
                                           static_cast<DWORD>(value));
}

// PE 0x101DFF10 points to 0x101C450E, an import jump to MSVCR71!_purecall.
// Preserve the existing adapter and the runtime's pure-call stop.
void RD9_THUNK blockIssuePure(void* RD9_THUNK_EDX, unsigned int, StateValue)
{
#if defined(_MSC_VER)
    _purecall();
#else
    // [CHOSEN, not recovered] The native C++ runtime supplies the corresponding pure-call entry.
    __cxa_pure_virtual();
#endif
}

void* const kBlockVtableBase[2]    = { reinterpret_cast<void*>(&blockIssuePure),
                                       reinterpret_cast<void*>(&blockCopyPlain) };
void* const kBlockVtableRs[2]      = { reinterpret_cast<void*>(&rsIssue),
                                       reinterpret_cast<void*>(&blockCopyPlain) };
void* const kBlockVtableTexture[2] = { reinterpret_cast<void*>(&texIssue),
                                       reinterpret_cast<void*>(&blockCopyTextureRef) };
void* const kBlockVtableSampler[2] = { reinterpret_cast<void*>(&samplerIssue),
                                       reinterpret_cast<void*>(&blockCopyPlain) };
void* const kBlockVtableTtf[2]     = { reinterpret_cast<void*>(&ttfIssue),
                                       reinterpret_cast<void*>(&blockCopyPlain) };

inline BlockIssueFn blockIssueSlot(void* block)
{
    return reinterpret_cast<BlockIssueFn>((*static_cast<void* const**>(block))[0]);
}
inline BlockCopyFn blockCopySlot(void* block)
{
    return reinterpret_cast<BlockCopyFn>((*static_cast<void* const**>(block))[1]);
}


// The block algorithms - each written once over the template, exactly as the shipped three
// instantiations behave.


// Block constructor body (0x100272D0 / 0x10027310 / the ctor's inline loops via sub_10001000 +
// sub_10027290): zero all three values, clear the low three flag bits, zero both list counts.
// PE 0x10027310
template <unsigned int N>
void blockConstruct(StateMirrorBlock<N>* b, void* const* vtbl)
{
    b->vptr = vtbl;
    for (unsigned int i = 0; i < N; ++i)
    {
        b->cells[i].f0Device = 0;
        b->cells[i].f1Base   = 0;
        b->cells[i].f2Wanted = 0;
        b->cells[i].flags   &= 0xF8u;
    }
    b->countTouched = 0;
    b->countPending = 0;
}

// The setter body shared by SetRenderState/SetTexture/SetTextureStageState/SetSamplerState
// (0x10026500 / 0x10026580 / 0x10026600 / 0x10026690): redundant-set filter against the device
// value, wanted <- value through the copy slot, file on both lists.
template <unsigned int N>
void blockSet(StateMirrorBlock<N>* b, unsigned int index, StateValue value)
{
    StateCell& cell = b->cells[index];
    if ((cell.flags & 2u) == 0)
    {
        if (cell.f0Device == value)
            return;
        b->listPending[b->countPending++] = static_cast<unsigned char>(index);
        cell.flags |= 2u;
    }
    blockCopySlot(b)(b, &cell.f2Wanted, &value);
    if ((cell.flags & 1u) == 0)
    {
        b->listTouched[b->countTouched++] = static_cast<unsigned char>(index);
        cell.flags |= 1u;
    }
}

// The pre-draw issue walk (0x10026DD0 / 0x10026ED0 / 0x10027200): for every pending cell whose
// wanted value differs from the device value, adopt it (f0 <- f2, refcounted for textures) and
// make the one real D3D call. Returns the number of issues, as the shipped bodies do.
// PE 0x10026ED0, 0x10027200
template <unsigned int N>
int blockIssuePending(StateMirrorBlock<N>* b)
{
    const unsigned char count = b->countPending;
    if (!count)
        return 0;
    int issued = 0;
    for (unsigned char i = 0; i < count; ++i)
    {
        const unsigned char index = b->listPending[i];
        StateCell& cell = b->cells[index];
        cell.flags &= ~2u;
        if (cell.f2Wanted != cell.f0Device)
        {
            blockCopySlot(b)(b, &cell.f0Device, &cell.f2Wanted);
            blockIssueSlot(b)(b, index, cell.f0Device);
            ++issued;
        }
    }
    b->countPending = 0;
    return issued;
}

// The POP restore walk (0x10026C90 / 0x10026FD0 / 0x10027100). Three phases:
//   A. drain the pending list, remembering entries that are neither freshly touched nor converged
//      (still diverged from base with no new order standing);
//   B. for every touched cell whose device value differs from its base, schedule the restore
//      (wanted <- base) and re-file it pending;
//   C. re-file the remembered stragglers.
// No D3D calls happen here - the next pre-draw apply issues the restores.
// PE 0x10026FD0, 0x10027100
template <unsigned int N>
void blockScheduleRestore(StateMirrorBlock<N>* b)
{
    if (!b->countTouched)
        return;

    unsigned char saved[N];
    unsigned char savedCount = 0;

    const unsigned char pending = b->countPending;
    if (pending)
    {
        for (unsigned char i = 0; i < pending; ++i)
        {
            const unsigned char index = b->listPending[i];
            StateCell& cell = b->cells[index];
            cell.flags &= ~2u;
            if ((cell.flags & 1u) == 0 && cell.f0Device != cell.f1Base)
                saved[savedCount++] = index;
        }
        b->countPending = 0;
    }

    const unsigned char touched = b->countTouched;
    if (touched)
    {
        for (unsigned char i = 0; i < touched; ++i)
        {
            const unsigned char index = b->listTouched[i];
            StateCell& cell = b->cells[index];
            cell.flags &= ~1u;
            if (cell.f0Device != cell.f1Base)
            {
                b->listPending[b->countPending++] = index;
                cell.flags |= 2u;
                blockCopySlot(b)(b, &cell.f2Wanted, &cell.f1Base);
            }
        }
    }
    b->countTouched = 0;

    for (unsigned char i = 0; i < savedCount; ++i)
    {
        const unsigned char index = saved[i];
        b->cells[index].flags |= 2u;
        b->listPending[b->countPending++] = index;
    }
}

// The reset reissue walk (0x10026E60 / 0x10026F60): for every cell holding a base value, adopt it
// as the device value and push it at the device, clearing both list bits and both lists. Run after
// the reset seeds so the fresh device and all three mirrors agree.
// PE 0x10026F60
template <unsigned int N>
void blockReissueSeeded(StateMirrorBlock<N>* b)
{
    for (unsigned int i = 0; i < N; ++i)
    {
        StateCell& cell = b->cells[i];
        if (cell.flags & 4u)
        {
            blockCopySlot(b)(b, &cell.f0Device, &cell.f1Base);
            cell.flags &= 0xFCu;
            blockIssueSlot(b)(b, i, cell.f0Device);
        }
    }
    b->countPending = 0;
    b->countTouched = 0;
}

// Seed one cell's base value and mark it held - the reset's `|= 4; f1 = value` store pair.
template <unsigned int N>
inline void blockSeedBase(StateMirrorBlock<N>* b, unsigned int index, StateValue value)
{
    b->cells[index].flags |= 4u;
    b->cells[index].f1Base = value;
}


// Seeds and reissues defaults, then clears mirrors. Before vendor probing, zero AA globals
// intentionally cause a harmless rejected SetRenderState(0, 0).

// PE 0x10025D00
void resetToDefaults(StateManager* self)
{
    RenderStateBlock& rs = self->renderStates;

    blockSeedBase(&rs,   7u, 1u);            // D3DRS_ZENABLE            = true
    blockSeedBase(&rs,   8u, 3u);            // D3DRS_FILLMODE           = solid
    blockSeedBase(&rs,   9u, 2u);            // D3DRS_SHADEMODE          = gouraud
    blockSeedBase(&rs,  14u, 1u);            // D3DRS_ZWRITEENABLE       = true
    blockSeedBase(&rs,  15u, 0u);            // D3DRS_ALPHATESTENABLE    = false
    blockSeedBase(&rs,  19u, 2u);            // D3DRS_SRCBLEND           = ONE
    blockSeedBase(&rs,  20u, 1u);            // D3DRS_DESTBLEND          = zero
    blockSeedBase(&rs,  22u, 3u);            // D3DRS_CULLMODE           = CCW
    blockSeedBase(&rs,  23u, 4u);            // D3DRS_ZFUNC              = lessequal
    blockSeedBase(&rs,  24u, 0u);            // D3DRS_ALPHAREF           = 0
    blockSeedBase(&rs,  25u, 5u);            // D3DRS_ALPHAFUNC          = greater (dice's, not D3D's 8)
    blockSeedBase(&rs,  27u, 0u);            // D3DRS_ALPHABLENDENABLE   = false
    blockSeedBase(&rs,  52u, 0u);            // D3DRS_STENCILENABLE      = false
    blockSeedBase(&rs,  53u, 1u);            // D3DRS_STENCILFAIL        = keep
    blockSeedBase(&rs,  54u, 1u);            // D3DRS_STENCILZFAIL       = keep
    blockSeedBase(&rs,  55u, 1u);            // D3DRS_STENCILPASS        = keep
    blockSeedBase(&rs,  56u, 8u);            // D3DRS_STENCILFUNC        = always
    blockSeedBase(&rs,  57u, 0u);            // D3DRS_STENCILREF         = 0
    blockSeedBase(&rs,  58u, 0xFFFFFFFFu);   // D3DRS_STENCILMASK
    blockSeedBase(&rs,  59u, 0xFFFFFFFFu);   // D3DRS_STENCILWRITEMASK
    blockSeedBase(&rs,  60u, 0xFFFFFFFFu);   // D3DRS_TEXTUREFACTOR
    for (unsigned int wrap = 128u; wrap <= 135u; ++wrap)
        blockSeedBase(&rs, wrap, 0u);        // D3DRS_WRAP0..WRAP7
    blockSeedBase(&rs, 152u, 0u);            // D3DRS_CLIPPLANEENABLE
    blockSeedBase(&rs, 168u, 15u);           // D3DRS_COLORWRITEENABLE
    blockSeedBase(&rs, 171u, 1u);            // D3DRS_BLENDOP            = ADD
    blockSeedBase(&rs, 174u, 0u);            // D3DRS_SCISSORTESTENABLE
    blockSeedBase(&rs, 175u, 0u);            // D3DRS_SLOPESCALEDEPTHBIAS
    blockSeedBase(&rs, 185u, 0u);            // D3DRS_TWOSIDEDSTENCILMODE
    blockSeedBase(&rs, 186u, 1u);            // D3DRS_CCW_STENCILFAIL    = keep
    blockSeedBase(&rs, 187u, 1u);            // D3DRS_CCW_STENCILZFAIL   = keep
    blockSeedBase(&rs, 188u, 1u);            // D3DRS_CCW_STENCILPASS    = keep
    blockSeedBase(&rs, 189u, 8u);            // D3DRS_CCW_STENCILFUNC    = always
    blockSeedBase(&rs, 190u, 15u);           // D3DRS_COLORWRITEENABLE1
    blockSeedBase(&rs, 191u, 15u);           // D3DRS_COLORWRITEENABLE2
    blockSeedBase(&rs, 192u, 15u);           // D3DRS_COLORWRITEENABLE3
    blockSeedBase(&rs, 193u, 0xFFFFFFFFu);   // D3DRS_BLENDFACTOR
    blockSeedBase(&rs, 195u, 0u);            // D3DRS_DEPTHBIAS

    // The transparency-AA state, seeded to its vendor OFF code - raw, unguarded, exactly as
    // shipped (see the function comment for the zero-globals shape).
    {
        unsigned int offCode;
        ::memcpy(&offCode, g_transparencyAAOff, 4);
        blockSeedBase(&rs, g_transparencyAAState, offCode);
    }

    blockSeedBase(&rs,  28u, 0u);            // D3DRS_FOGENABLE          = false
    blockSeedBase(&rs,  34u, 0xFF00FF00u);   // D3DRS_FOGCOLOR           = opaque green
    blockSeedBase(&rs,  36u, 0u);            // D3DRS_FOGSTART           = 0.0f
    blockSeedBase(&rs,  37u, 0x3F800000u);   // D3DRS_FOGEND             = 1.0f

    blockReissueSeeded(&rs);

    // Texture stages 0..7 revert to "no texture".
    for (unsigned int stage = 0; stage < 8; ++stage)
        blockSeedBase(&self->textures, stage, 0u);
    blockReissueSeeded(&self->textures);

    // Sampler defaults - the D3D9 runtime's own - then reissue per stage. The TTF blocks reproduce
    // the shipped inline shape: force the device value to 0 (a fresh device's default), fold the
    // base onto it through the copy slot, clear the list bits, reissue, and drop both lists.
    for (unsigned int stage = 0; stage < 8; ++stage)
    {
        SamplerBlock& sb = self->samplers[stage];
        blockSeedBase(&sb,  1u, 1u);         // D3DSAMP_ADDRESSU   = wrap
        blockSeedBase(&sb,  2u, 1u);         // D3DSAMP_ADDRESSV   = wrap
        blockSeedBase(&sb,  3u, 1u);         // D3DSAMP_ADDRESSW   = wrap
        blockSeedBase(&sb,  5u, 1u);         // D3DSAMP_MAGFILTER  = point
        blockSeedBase(&sb,  6u, 1u);         // D3DSAMP_MINFILTER  = point
        blockSeedBase(&sb,  7u, 0u);         // D3DSAMP_MIPFILTER  = none
        blockSeedBase(&sb,  8u, 0u);         // D3DSAMP_MIPMAPLODBIAS
        blockSeedBase(&sb,  9u, 0u);         // D3DSAMP_MAXMIPLEVEL
        blockSeedBase(&sb, 10u, 1u);         // D3DSAMP_MAXANISOTROPY
        blockSeedBase(&sb, 12u, 0u);         // D3DSAMP_ELEMENTINDEX
        blockReissueSeeded(&sb);

        TtfBlock& tb = self->textureTransforms[stage];
        StateCell& cell = tb.cells[0];
        cell.flags |= 4u;
        cell.f1Base = 0;
        blockCopySlot(&tb)(&tb, &cell.f0Device, &cell.f1Base);
        const StateValue value = cell.f0Device;
        cell.flags &= 0xFCu;
        blockIssueSlot(&tb)(&tb, 0, value);
        tb.countTouched = 0;
        tb.countPending = 0;
    }

    // The plain mirrors: streams to {0,-1,-1,clean}, everything else null and clean.
    for (unsigned int i = 0; i < 16; ++i)
    {
        self->streamVB[i]     = 0;
        self->streamOffset[i] = 0xFFFFFFFFu;
        self->streamStride[i] = 0xFFFFFFFFu;
        self->streamDirty[i]  = 0;
    }
    self->indices           = 0;  self->indicesDirty      = 0;
    self->vertexDecl        = 0;  self->vertexDeclDirty   = 0;
    self->vertexShader      = 0;  self->vertexShaderDirty = 0;
    self->pixelShader       = 0;  self->pixelShaderDirty  = 0;
    self->renderTarget      = 0;  self->renderTargetDirty = 0;
    for (unsigned int i = 0; i < 3; ++i)
    {
        self->extraRenderTarget[i]      = 0;
        self->extraRenderTargetDirty[i] = 0;
    }
    self->depthStencil      = 0;  self->depthStencilDirty = 0;
}


// The shader-constant register files (sub_10027370 / sub_100274C0 mirrors, sub_100273F0 /
// sub_10027540 flushes). One body each, parameterized by the file the caller names.

// PE 0x10027370, 0x100274C0
void constFileWrite(float (*data)[4], uint64_t* dirty, unsigned int reg, const void* src,
                    unsigned int count)
{
    ::memcpy(data[reg], src, 16u * count);
    for (unsigned int r = reg, end = reg + count; r < end; )
    {
        const unsigned int bit = r & 63u;
        const unsigned int n = (end - r < 64u - bit) ? end - r : 64u - bit;
        const uint64_t ones = n == 64u ? ~0ull : ((1ull << n) - 1u);
        dirty[r >> 6] |= ones << bit;
        r += n;
    }
}

// PE 0x100272C0
HRESULT __stdcall issueVertexConstants(IDirect3DDevice9* device, unsigned int reg,
                                       const float* data, unsigned int count)
{
    return device->SetVertexShaderConstantF(reg, data, count);
}

// PE 0x100272B0
HRESULT __stdcall issuePixelConstants(IDirect3DDevice9* device, unsigned int reg,
                                      const float* data, unsigned int count)
{
    return device->SetPixelShaderConstantF(reg, data, count);
}

inline unsigned int lowestSetBit(uint64_t m)
{
    unsigned long index;
    _BitScanForward64(&index, m);
    return static_cast<unsigned int>(index);
}

// PE 0x100273F0, 0x10027540 (the run batching; the walk is the bit mask's, see StateManager).
void constFileFlush(float (*data)[4], uint64_t* dirty, unsigned int words, ConstantIssueFn issue)
{
    const unsigned int kNone = 0xFFFFFFFFu;
    unsigned int runStart = kNone;
    for (unsigned int w = 0; w < words; ++w)
    {
        const uint64_t m = dirty[w];
        const unsigned int base = w << 6;
        if (!m)
        {
            if (runStart != kNone)
            {
                issue(rendererDevice(), runStart, data[runStart], base - runStart);
                runStart = kNone;
            }
            continue;
        }
        dirty[w] = 0;
        unsigned int bit = 0;
        while (bit < 64u)
        {
            // Looking for a run's start among the set bits, or its end among the clear ones.
            const uint64_t rest = (runStart == kNone ? m : ~m) >> bit;
            if (!rest)
                break;
            bit += lowestSetBit(rest);
            if (runStart == kNone)
                runStart = base + bit;
            else
            {
                issue(rendererDevice(), runStart, data[runStart], base + bit - runStart);
                runStart = kNone;
            }
        }
    }
    if (runStart != kNone)
        issue(rendererDevice(), runStart, data[runStart], (words << 6) - runStart);
}


// The 26 primary vtable slots. COM shape: __stdcall with `this` as the first stack argument, which
// is exactly what the shipped bodies decompile to and what D3DX will call through the vtable.


const unsigned char kIID_IUnknown[16] =
{ 0x00,0x00,0x00,0x00, 0x00,0x00, 0x00,0x00, 0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x46 };

// +0x00 QueryInterface, 0x10025B20. Answers only IID_IUnknown - not ID3DXEffectStateManager's
// own IID. D3DX takes the interface pointer without QI'ing for the derived IID, so this is enough.
int __stdcall smQueryInterface(StateManager* self, const void* iid, void** out)
{
    *out = 0;
    if (::memcmp(iid, kIID_IUnknown, 16) != 0)
        return static_cast<int>(0x80004002);   // E_NOINTERFACE
    *out = self;
    return 0;
}

// +0x04 AddRef, 0x10026490 / +0x08 Release, 0x100264B0. Release restores both vtables, detaches
// the device-lost listener, releases the device and frees through the engine allocator. It does
// NOT release the texture/RT/DS references still held in the mirrors - shipped behaviour.
int __stdcall smAddRef(StateManager* self) { return ++self->refCount; }

// PE 0x100259D0 (listing read). The destructor body: restore both vtables, detach the device-lost
// listener (`lea eax, [esi+4]; mov ecx, dword_1023B970; push eax; call sub_10009D70`, now that the
// constructor registers, see createStateManager) and release the device. It does not free the object.
void smDestruct(StateManager* self)
{
    self->vptr = kStateManagerVtable;
    self->listenerVptr = kStateManagerListenerVtable;
    unregisterDeviceLostResetListener(static_cast<void*>(&self->listenerVptr));
    self->device->Release();
}

// PE 0x100264B0.
int __stdcall smRelease(StateManager* self)
{
    const int remaining = --self->refCount;
    if (!remaining && self)
    {
        smDestruct(self);
        ::renddx9abi::engineFree(self);
    }
    return remaining;
}

// +0x0C SetTransform / +0x10 SetMaterial / +0x14 SetLight / +0x18 LightEnable / +0x2C SetNPatchMode
// / +0x30 SetFVF - all E_NOTIMPL one-liners in the shipped image (0x10025B60..0x10025BE0). The
// fixed-function paths simply are not routed through the StateManager.
int __stdcall smSetTransform(StateManager*, unsigned int, const void*) { return static_cast<int>(0x80004001); }
int __stdcall smSetMaterial(StateManager*, const void*)                { return static_cast<int>(0x80004001); }
int __stdcall smSetLight(StateManager*, unsigned int, const void*)     { return static_cast<int>(0x80004001); }
int __stdcall smLightEnable(StateManager*, unsigned int, int)          { return static_cast<int>(0x80004001); }
int __stdcall smSetNPatchMode(StateManager*, float)                    { return static_cast<int>(0x80004001); }
int __stdcall smSetFVF(StateManager*, unsigned int)                    { return static_cast<int>(0x80004001); }

// +0x1C SetRenderState, 0x10026500.
int __stdcall smSetRenderState(StateManager* self, unsigned int state, unsigned int value)
{
    blockSet(&self->renderStates, state, value);
    return 0;
}

// +0x20 SetTexture, 0x10026580. The value D3DX passes is the RAW IDirect3DBaseTexture9 (FXParameter
// unwraps the EngineTexture before SetTexture reaches the effect), so the refcounted copy slot
// AddRef/Releases D3D objects here - their IUnknown slots 1/2 line up with the wrapper convention.
int __stdcall smSetTexture(StateManager* self, unsigned int stage, void* texture)
{
    // [FIX] Only stages 0-15 are mirrored. D3DX hands vertex-texture stages through the same call
    // (D3DVERTEXTEXTURESAMPLER0..3 = 257..260); the unchecked index wrote past the block. Those go
    // straight to the device, like the stage states this manager does not mirror.
    if (stage >= sizeof(self->textures.cells) / sizeof(self->textures.cells[0]))
        return self->device->SetTexture(stage, static_cast<IDirect3DBaseTexture9*>(texture));
    blockSet(&self->textures, stage, reinterpret_cast<StateValue>(texture));
    return 0;
}

// +0x24 SetTextureStageState, 0x10026600. Only D3DTSS_TEXTURETRANSFORMFLAGS (24) is mirrored;
// every other stage state goes straight through to the device.
int __stdcall smSetTextureStageState(StateManager* self, unsigned int stage, unsigned int type,
                                     unsigned int value)
{
    if (type != 24u)
    {
        self->device->SetTextureStageState(stage, static_cast<D3DTEXTURESTAGESTATETYPE>(type),
                                           value);
        return 0;
    }
    blockSet(&self->textureTransforms[stage], 0u, value);
    return 0;
}

// +0x28 SetSamplerState, raises the stage watermark the pop and pre-draw loops share.
// PE 0x10026690 (blockSet inlined).
int __stdcall smSetSamplerState(StateManager* self, unsigned int stage, unsigned int type,
                                unsigned int value)
{
    // [FIX] Only stages 0-7 are mirrored; a higher stage (vertex-texture stages 257..260 included)
    // indexed past samplers[] and the watermark loops. It goes straight to the device.
    if (stage >= sizeof(self->samplers) / sizeof(self->samplers[0]))
        return self->device->SetSamplerState(stage, static_cast<D3DSAMPLERSTATETYPE>(type), value);
    if (self->maxStageTouched < stage)
        self->maxStageTouched = stage;
    blockSet(&self->samplers[stage], type, value);
    return 0;
}

// +0x34 SetVertexShader, 0x10025BF0 / +0x44 SetPixelShader, 0x10025C20.
int __stdcall smSetVertexShader(StateManager* self, void* shader)
{
    if (self->vertexShader != shader)
    {
        self->vertexShader = shader;
        self->vertexShaderDirty = 1;
    }
    return 0;
}
// PE 0x10025C20.
int __stdcall smSetPixelShader(StateManager* self, void* shader)
{
    if (self->pixelShader != shader)
    {
        self->pixelShader = shader;
        self->pixelShaderDirty = 1;
    }
    return 0;
}

// +0x38 SetVertexShaderConstantF, 0x10026A50 -> mirror. +0x48 SetPixelShaderConstantF, 0x10026A80.
int __stdcall smSetVertexShaderConstantF(StateManager* self, unsigned int reg, const float* data,
                                         unsigned int count)
{
    constFileWrite(self->vsConst, self->vsConstDirty, reg, data, count);
    return 0;
}
// PE 0x10026A80.
// [RENDER] The registers kEngineRegisterBegin..kEngineRegisterEnd hold the frame's sun cascade
// constants (RASM, stateManagerSetSunCascadeConstants), the entry's LOD dither (RaLodDither,
// renderEntryBindLodDither), the aerial-perspective lookup (RaAerial, stateManagerSetAerial), the
// surfaces' sun factor (RaSunFactor, stateManagerSetSunFactor)
// the sky's irradiance (RaSkySH, stateManagerSetSkySh), water's reflection switch (RaWaterSsr, stateManagerSetWaterSsr),
// water's foam (RaWaterFoam, stateManagerSetWaterFoam), water's medium (RaWaterMedium, stateManagerSetWaterMedium)
// the cloud shadow lookup (RaCloudShadow, stateManagerSetCloudShadow)
// and the run-time sun past the cascades (RaSunStatic, stateManagerSetSunStatic). The effects that read them declare them at those registers, and D3DX
// uploads the parameters' own zeros at each pass; that part of a write is dropped, so the range
// keeps the engine's values. Retail mirrors every write.
int __stdcall smSetPixelShaderConstantF(StateManager* self, unsigned int reg, const float* data,
                                        unsigned int count)
{
    const unsigned int first = kEngineRegisterBegin;
    const unsigned int end = kEngineRegisterEnd;
    if (reg < end && reg + count > first)
    {
        if (reg < first)
            constFileWrite(self->psConst, self->psConstDirty, reg, data, first - reg);
        if (reg + count > end)
            constFileWrite(self->psConst, self->psConstDirty, end, data + 4u * (end - reg),
                           reg + count - end);
        return 0;
    }
    constFileWrite(self->psConst, self->psConstDirty, reg, data, count);
    return 0;
}

// The I and B constant slots pass straight through to the device (0x10026730 / 0x10026760 /
// 0x10026790 / 0x100267C0) - no mirror, no dirty tracking.
int __stdcall smSetVertexShaderConstantI(StateManager* self, unsigned int reg, const int* data,
                                         unsigned int count)
{
    self->device->SetVertexShaderConstantI(reg, data, count);
    return 0;
}
// PE 0x10026760.
int __stdcall smSetVertexShaderConstantB(StateManager* self, unsigned int reg, const BOOL* data,
                                         unsigned int count)
{
    self->device->SetVertexShaderConstantB(reg, data, count);
    return 0;
}
// PE 0x10026790.
int __stdcall smSetPixelShaderConstantI(StateManager* self, unsigned int reg, const int* data,
                                        unsigned int count)
{
    self->device->SetPixelShaderConstantI(reg, data, count);
    return 0;
}
// PE 0x100267C0.
int __stdcall smSetPixelShaderConstantB(StateManager* self, unsigned int reg, const BOOL* data,
                                        unsigned int count)
{
    self->device->SetPixelShaderConstantB(reg, data, count);
    return 0;
}

// the dice extension slots

// +0x54 SetIndices, 0x10025A10.
int __stdcall smSetIndices(StateManager* self, void* ib)
{
    if (self->indices != ib)
    {
        self->indices = ib;
        self->indicesDirty = 1;
    }
    return 0;
}

// Suppresses redundant stream bindings without comparing stride; stride-only changes are ignored.
// PE 0x10026430.
int __stdcall smSetStreamSource(StateManager* self, unsigned int stream, void* vb,
                                unsigned int offset, unsigned int stride)
{
    if (self->maxStreamTouched < stream)
        self->maxStreamTouched = stream;
    if (self->streamVB[stream] != vb || self->streamOffset[stream] != offset)
    {
        self->streamVB[stream]     = vb;
        self->streamOffset[stream] = offset;
        self->streamStride[stream] = stride;
        self->streamDirty[stream]  = 1;
    }
    return 0;
}

// +0x5C SetVertexDeclaration, 0x10025A40.
int __stdcall smSetVertexDeclaration(StateManager* self, void* decl)
{
    if (self->vertexDecl != decl)
    {
        self->vertexDecl = decl;
        self->vertexDeclDirty = 1;
    }
    return 0;
}

// Maintains a refcounted render-target mirror. Only index 0 has valid storage;
// higher indices overlap the dirty bytes. The shipped body addresses `this + 0x1E90 + 4 * index`
// and `this + 0x1E94 + index`; measuring from the named members keeps that aliasing on x64.
int __stdcall smSetRenderTarget(StateManager* self, unsigned int index, IDirect3DSurface9* surface)
{
    // [RENDER] Indices 1..3 address the MRT slots (the shipped body indexes past
    // RT0 into the depth-stencil mirror).
    if (index > 3)
        return static_cast<int>(D3DERR_INVALIDCALL);
    IDirect3DSurface9** value = index ? &self->extraRenderTarget[index - 1] : &self->renderTarget;
    unsigned char* dirtyByte = index ? &self->extraRenderTargetDirty[index - 1]
                                     : &self->renderTargetDirty;
    if (*value != surface)
    {
        if (*value)
            (*value)->Release();
        *value = surface;
        if (surface)
            surface->AddRef();
        *dirtyByte = 1;
    }
    return 0;
}

// +0x64 SetDepthStencilSurface, refcounted mirror.
// PE 0x10025AE0.
int __stdcall smSetDepthStencilSurface(StateManager* self, IDirect3DSurface9* surface)
{
    if (self->depthStencil != surface)
    {
        if (self->depthStencil)
            self->depthStencil->Release();
        self->depthStencil = surface;
        if (surface)
            surface->AddRef();
        self->depthStencilDirty = 1;
    }
    g_standardDepthBound = false;   // [RENDER] reversed-Z: shadow callers re-mark after this
    return 0;
}


// The render-target flush, sub_10026240. A null set for target 0 goes straight out; a non-null set
// first walks all 16 texture cells and unbinds any texture (wanted or device-bound) whose level-0
// surface IS the incoming render target - the make-a-texture-a-render-target hazard - then issues
// SetRenderTarget. Depth-stencil last.

IDirect3DSurface9* topSurfaceOf(void* baseTexture)
{
    IDirect3DBaseTexture9* base = static_cast<IDirect3DBaseTexture9*>(baseTexture);
    IDirect3DSurface9* surface = 0;
    const D3DRESOURCETYPE type = base->GetType();
    if (type == D3DRTYPE_TEXTURE)
        static_cast<IDirect3DTexture9*>(baseTexture)->GetSurfaceLevel(0, &surface);
    if (type == D3DRTYPE_CUBETEXTURE)
        static_cast<IDirect3DCubeTexture9*>(baseTexture)
            ->GetCubeMapSurface(D3DCUBEMAP_FACE_POSITIVE_X, 0, &surface);
    if (surface)
        surface->Release();     // the shipped body drops the GetSurfaceLevel reference immediately
    return surface;             // compared by identity only - never dereferenced
}

// A texture bound on any stage that is about to become a render target is unbound first (the
// device must not sample what it writes).
void unbindTexturesOfTarget(StateManager* self, IDirect3DSurface9* target)
{
    for (unsigned int stage = 0; stage < 16; ++stage)
    {
        StateCell& cell = self->textures.cells[stage];
        if ((cell.flags & 4u) == 0)
            continue;
        if ((cell.flags & 2u) != 0 && cell.f2Wanted)
        {
            if (topSurfaceOf(reinterpret_cast<void*>(cell.f2Wanted)) == target)
                cell.f2Wanted = 0;
        }
        if (cell.f0Device)
        {
            if (topSurfaceOf(reinterpret_cast<void*>(cell.f0Device)) == target)
            {
                reinterpret_cast<IUnknown*>(cell.f0Device)->Release();
                cell.f0Device = 0;
                self->device->SetTexture(stage, 0);
            }
        }
    }
}

void flushRenderTargets(StateManager* self)
{
    if (self->renderTargetDirty)
    {
        if (!self->renderTarget)
        {
            // The shipped null arm issues SetRenderTarget(0, NULL) - which D3D rejects for target
            // 0 - and clears the dirty flag either way.
            self->device->SetRenderTarget(0, 0);
            self->renderTargetDirty = 0;
        }
        else
        {
            unbindTexturesOfTarget(self, self->renderTarget);
            self->device->SetRenderTarget(0, self->renderTarget);
            self->renderTargetDirty = 0;
        }
    }
    // [RENDER] RT1..RT3 (MRT); null unbinds, which D3D allows above target 0.
    for (unsigned int i = 0; i < 3; ++i)
    {
        if (!self->extraRenderTargetDirty[i])
            continue;
        if (self->extraRenderTarget[i])
            unbindTexturesOfTarget(self, self->extraRenderTarget[i]);
        self->device->SetRenderTarget(i + 1, self->extraRenderTarget[i]);
        self->extraRenderTargetDirty[i] = 0;
    }
    if (self->depthStencilDirty)
    {
        self->device->SetDepthStencilSurface(self->depthStencil);
        self->depthStencilDirty = 0;
        // [RENDER] reversed depth: the depth convention follows the buffer, so the depth states
        // the device holds are re-sent for the new one.
        static const unsigned int kDepthStates[3] = { 23u, 175u, 195u };
        for (unsigned int i = 0; i < 3; ++i)
        {
            const unsigned int st = kDepthStates[i];
            self->device->SetRenderState(static_cast<D3DRENDERSTATETYPE>(st),
                stateManagerMapDepthState(st,
                    static_cast<unsigned int>(self->renderStates.cells[st].f0Device)));
        }
    }
}


// The pre-draw apply, sub_10026880 - the ONE function that issues deferred state, called by the
// base renderer's DrawPrimitive/DrawIndexedPrimitive wrappers immediately before the device draw.

void preDrawApply(StateManager* self)
{
    // First the ShaderManager's current effects get their D3DX-side CommitChanges - so parameter
    // sets made since BeginPass land in this draw.
    shaderManagerCommitCurrentEffects();

    blockIssuePending(&self->renderStates);
    flushRenderTargets(self);
    blockIssuePending(&self->textures);
    for (unsigned int stage = 0; stage <= self->maxStageTouched; ++stage)
    {
        blockIssuePending(&self->samplers[stage]);
        blockIssuePending(&self->textureTransforms[stage]);
    }

    for (unsigned int stream = 0; stream <= self->maxStreamTouched; ++stream)
    {
        if (self->streamDirty[stream])
        {
            self->device->SetStreamSource(
                stream, static_cast<IDirect3DVertexBuffer9*>(self->streamVB[stream]),
                self->streamOffset[stream], self->streamStride[stream]);
            self->streamDirty[stream] = 0;
        }
    }
    if (self->indicesDirty)
    {
        self->device->SetIndices(static_cast<IDirect3DIndexBuffer9*>(self->indices));
        self->indicesDirty = 0;
    }
    if (self->vertexDeclDirty)
    {
        self->device->SetVertexDeclaration(
            static_cast<IDirect3DVertexDeclaration9*>(self->vertexDecl));
        self->vertexDeclDirty = 0;
    }
    if (self->vertexShaderDirty)
    {
        self->device->SetVertexShader(static_cast<IDirect3DVertexShader9*>(self->vertexShader));
        self->vertexShaderDirty = 0;
    }
    if (self->pixelShaderDirty)
    {
        self->device->SetPixelShader(static_cast<IDirect3DPixelShader9*>(self->pixelShader));
        self->pixelShaderDirty = 0;
    }

    // The constant-run flushes.
    constFileFlush(self->vsConst, self->vsConstDirty, 4u, self->vsConstIssueFn);
    constFileFlush(self->psConst, self->psConstDirty, 4u, self->psConstIssueFn);
}


// push (sub_100261A0) / pop (sub_100261B0). Pop's zero-depth arm schedules the restore-to-base of
// everything touched; the D3D calls happen at the next pre-draw apply.

void pushBracket(StateManager* self) { ++self->pushDepth; }

void popBracket(StateManager* self)
{
    if (self->pushDepth-- == 1)
    {
        blockScheduleRestore(&self->renderStates);
        blockScheduleRestore(&self->textures);
        for (unsigned int stage = 0; stage <= self->maxStageTouched; ++stage)
        {
            blockScheduleRestore(&self->samplers[stage]);
            blockScheduleRestore(&self->textureTransforms[stage]);
        }
    }
}


// Listener addresses are relative to the StateManager subobject at +4.
// Loss clears held resources and mirrors; reset reissues base state; destruction delegates to loss.

inline StateManager* fromListener(void* listener)
{
    return reinterpret_cast<StateManager*>(static_cast<unsigned char*>(listener)
                                           - offsetof(StateManager, listenerVptr));
}

// sub_10025980 - release all three references of all 16 texture cells.
void dropTextureBlockRefs(TextureBlock* block)
{
    for (unsigned int stage = 0; stage < 16; ++stage)
    {
        StateCell& cell = block->cells[stage];
        if (cell.f0Device) reinterpret_cast<IUnknown*>(cell.f0Device)->Release();
        cell.f0Device = 0;
        if (cell.f1Base) reinterpret_cast<IUnknown*>(cell.f1Base)->Release();
        cell.f1Base = 0;
        if (cell.f2Wanted) reinterpret_cast<IUnknown*>(cell.f2Wanted)->Release();
        cell.f2Wanted = 0;
    }
}

// PE 0x10025C50
void RD9_THUNK listenerOnDeviceLost(void* listener RD9_THUNK_EDX)
{
    StateManager* self = fromListener(listener);
    dropTextureBlockRefs(&self->textures);
    if (self->renderTarget)
        self->renderTarget->Release();
    self->renderTarget = 0;
    for (unsigned int i = 0; i < 3; ++i)
    {
        if (self->extraRenderTarget[i])
            self->extraRenderTarget[i]->Release();
        self->extraRenderTarget[i] = 0;
    }
    if (self->depthStencil)
        self->depthStencil->Release();
    self->depthStencil = 0;
    for (unsigned int i = 0; i < 16; ++i)
    {
        self->streamVB[i]     = 0;
        self->streamOffset[i] = 0xFFFFFFFFu;
        self->streamStride[i] = 0xFFFFFFFFu;
        self->streamDirty[i]  = 0;
    }
    self->indices      = 0;  self->indicesDirty      = 0;
    self->vertexDecl   = 0;  self->vertexDeclDirty   = 0;
    self->vertexShader = 0;  self->vertexShaderDirty = 0;
    self->pixelShader  = 0;  self->pixelShaderDirty  = 0;
}

// PE 0x100267F0
void RD9_THUNK listenerOnDeviceReset(void* listener RD9_THUNK_EDX)
{
    StateManager* self = fromListener(listener);
    blockReissueSeeded(&self->renderStates);
    blockReissueSeeded(&self->textures);
    for (unsigned int stage = 0; stage < 8; ++stage)
    {
        blockReissueSeeded(&self->samplers[stage]);
        TtfBlock& tb = self->textureTransforms[stage];
        StateCell& cell = tb.cells[0];
        if (cell.flags & 4u)
        {
            blockCopySlot(&tb)(&tb, &cell.f0Device, &cell.f1Base);
            const StateValue value = cell.f0Device;
            cell.flags &= 0xFCu;
            blockIssueSlot(&tb)(&tb, 0, value);
        }
        tb.countPending = 0;
        tb.countTouched = 0;
    }
}

void RD9_THUNK listenerOnDeviceDestroy(void* listener RD9_THUNK_EDX)
{
    listenerOnDeviceLost(listener RD9_THUNK_ARG);
}

} // anonymous namespace

// The primary vtable, off_101DFEA8 - ID3DXEffectStateManager's 21 slots then the five dice
// extensions, in the shipped order.
void* const kStateManagerVtable[26] =
{
    reinterpret_cast<void*>(&smQueryInterface),           // +0x00
    reinterpret_cast<void*>(&smAddRef),                   // +0x04
    reinterpret_cast<void*>(&smRelease),                  // +0x08
    reinterpret_cast<void*>(&smSetTransform),             // +0x0C  0x10025B60  E_NOTIMPL
    reinterpret_cast<void*>(&smSetMaterial),              // +0x10  0x10025B70  E_NOTIMPL
    reinterpret_cast<void*>(&smSetLight),                 // +0x14  0x10025B80  E_NOTIMPL
    reinterpret_cast<void*>(&smLightEnable),              // +0x18  0x10025B90  E_NOTIMPL
    reinterpret_cast<void*>(&smSetRenderState),           // +0x1C
    reinterpret_cast<void*>(&smSetTexture),               // +0x20
    reinterpret_cast<void*>(&smSetTextureStageState),     // +0x24
    reinterpret_cast<void*>(&smSetSamplerState),          // +0x28
    reinterpret_cast<void*>(&smSetNPatchMode),            // +0x2C  0x10025BD0  E_NOTIMPL
    reinterpret_cast<void*>(&smSetFVF),                   // +0x30  0x10025BE0  E_NOTIMPL
    reinterpret_cast<void*>(&smSetVertexShader),          // +0x34
    reinterpret_cast<void*>(&smSetVertexShaderConstantF), // +0x38
    reinterpret_cast<void*>(&smSetVertexShaderConstantI), // +0x3C  0x10026730  passthrough
    reinterpret_cast<void*>(&smSetVertexShaderConstantB), // +0x40  0x10026760  passthrough
    reinterpret_cast<void*>(&smSetPixelShader),           // +0x44
    reinterpret_cast<void*>(&smSetPixelShaderConstantF),  // +0x48
    reinterpret_cast<void*>(&smSetPixelShaderConstantI),  // +0x4C  0x10026790  passthrough
    reinterpret_cast<void*>(&smSetPixelShaderConstantB),  // +0x50  0x100267C0  passthrough
    reinterpret_cast<void*>(&smSetIndices),               // +0x54  0x10025A10  [DICE]
    reinterpret_cast<void*>(&smSetStreamSource),          // +0x58  0x10026430  [DICE]
    reinterpret_cast<void*>(&smSetVertexDeclaration),     // +0x5C  0x10025A40  [DICE]
    reinterpret_cast<void*>(&smSetRenderTarget),          // +0x60  0x10025A90  [DICE]
    reinterpret_cast<void*>(&smSetDepthStencilSurface)    // +0x64  0x10025AE0  [DICE]
};

void* const kStateManagerListenerVtable[3] =
{
    reinterpret_cast<void*>(&listenerOnDeviceLost),
    reinterpret_cast<void*>(&listenerOnDeviceReset),
    reinterpret_cast<void*>(&listenerOnDeviceDestroy)
};


// Construction - sub_10026AB0 wrapped by the RendDX9Init.cpp:232 call site (0x1000B31E..0x1000B35C:
// allocate 0x3EA0, run the constructor, publish dword_1023B984, log the failure line on null).

// PE 0x10026AB0 constructor inlined in the allocation wrapper.
bool createStateManager(void* device)
{
    StateManager* self = static_cast<StateManager*>(
        ::renddx9abi::engineAlloc(sizeof(StateManager)));
    if (!self)
    {
        g_stateManager = 0;
        ::renddx9abi::logf("RendDX9Init.cpp:232 Failed to create StateManager.");
        return false;
    }

    self->vptr = kStateManagerVtable;
    self->listenerVptr = kStateManagerListenerVtable;   // the ctor's double-write collapses to one
    self->refCount = 1;
    self->device = static_cast<IDirect3DDevice9*>(device);

    blockConstruct(&self->renderStates, kBlockVtableBase);
    self->renderStates.vptr = kBlockVtableRs;
    blockConstruct(&self->textures, kBlockVtableBase);
    self->textures.vptr = kBlockVtableTexture;
    for (unsigned int stage = 0; stage < 8; ++stage)
    {
        blockConstruct(static_cast<StateMirrorBlock<16>*>(&self->samplers[stage]),
                       kBlockVtableBase);
        self->samplers[stage].vptr = kBlockVtableSampler;
    }
    for (unsigned int stage = 0; stage < 8; ++stage)
    {
        blockConstruct(static_cast<StateMirrorBlock<1>*>(&self->textureTransforms[stage]),
                       kBlockVtableBase);
        self->textureTransforms[stage].vptr = kBlockVtableTtf;
    }

    self->pushDepth = 0;
    self->maxStreamTouched = 0;
    self->maxStageTouched = 0;

    self->vsConstIssueFn = &issueVertexConstants;
    ::memset(self->vsConstDirty, 0, sizeof(self->vsConstDirty));
    self->psConstIssueFn = &issuePixelConstants;
    ::memset(self->psConstDirty, 0, sizeof(self->psConstDirty));

    self->device->AddRef();
    for (unsigned int stage = 0; stage < 8; ++stage)
    {
        self->samplers[stage].stage = stage;
        self->textureTransforms[stage].stage = stage;
    }

    resetToDefaults(self);

    // the constructor's last act, 0x10026C70..0x10026C7B:
    //     mov ecx, dword_1023B970; push edi (= 2); lea eax, [esi+4]; push eax; call sub_10009D60
    // i.e. RendDX9_registerDeviceLostResetListener(this + 4, 2). Priority 2 puts the StateManager's
    // loss after every priority-3..5 listener has let go of its resources (so the texture / RT / DS
    // references its mirrors still hold are the last to drop) and its reset re-issue before them.
    registerDeviceLostResetListener(static_cast<void*>(&self->listenerVptr), 2);
    g_stateManager = self;

#if defined(_M_IX86)
    ::renddx9abi::logf("StateManager: up at 0x%08X (16032 bytes, RendDX9Init.cpp:232) - device "
                       "0x%08X, defaults seeded and reissued",
                       reinterpret_cast<unsigned int>(self),
                       reinterpret_cast<unsigned int>(device));
#else
    ::renddx9abi::logf("StateManager: up at 0x%p (%u bytes, RendDX9Init.cpp:232) - device "
                       "0x%p, defaults seeded and reissued", static_cast<void*>(self),
                       static_cast<unsigned int>(sizeof(StateManager)), device);
#endif
    return true;
}


// Cross-TU seams. The shipped callers reach these as direct near calls with ecx = the object; here
// they are plain functions over the published global, in the front's seam style.

// [CHOSEN, not recovered] Owner-TU seam for PE 0x1000AFF1..0x1000B000.
void releaseStateManager()
{
    if (g_stateManager)
        static_cast<IUnknown*>(g_stateManager)->Release();
}

void stateManagerPush()
{
    pushBracket(sm());
}

void stateManagerPop()
{
    popBracket(sm());
}

// Returns the mirrored pixel shader used to identify the effect's bound shader model.
void* stateManagerCurrentPixelShader()
{
    return sm()->pixelShader;
}

// PE 0x10026500 / primary slot +0x1C.  LightManager uses this ordinary deferred setter for
// D3DRS_COLORWRITEENABLE-style shadow-pass state; it is not the persistent/base-changing helper.
void stateManagerSetRenderState(unsigned int state, unsigned int value)
{
    smSetRenderState(sm(), state, value);
}

// PE 0x10025A90 / +0x60 and 0x10025AE0 / +0x64.  The mirror owns COM references and defers the
// device calls until flushRenderTargets, exactly like the primary-vtable entry points.
void stateManagerSetRenderTarget(unsigned int index, IDirect3DSurface9* surface)
{
    smSetRenderTarget(sm(), index, surface);
}

void stateManagerSetDepthStencilSurface(IDirect3DSurface9* surface)
{
    smSetDepthStencilSurface(sm(), surface);
}

// [RENDER] Prepassed draws, see stateManagerMapDepthState.
void stateManagerSetPrepassedDraw(bool prepassed)
{
    if (g_prepassedDraw == prepassed)
        return;
    g_prepassedDraw = prepassed;
    if (!g_stateManager)
        return;
    // The device holds the mapped values of the cells' device words; re-send them under the new
    // mapping (a pending wanted value is issued through the same mapping at the next draw).
    StateManager* const self = sm();
    static const unsigned int kStates[3] = { 23u, 14u, 15u };   // ZFUNC, ZWRITEENABLE, ALPHATESTENABLE
    for (unsigned int i = 0; i < 3; ++i)
    {
        const unsigned int st = kStates[i];
        self->device->SetRenderState(static_cast<D3DRENDERSTATETYPE>(st),
            stateManagerMapDepthState(st,
                static_cast<unsigned int>(self->renderStates.cells[st].f0Device)));
    }
}

unsigned int stateManagerWantedRenderState(unsigned int state)
{
    return static_cast<unsigned int>(sm()->renderStates.cells[state].f2Wanted);
}

// [RENDER] The desired depth-stencil surface, without an AddRef (depth prepass).
IDirect3DSurface9* stateManagerGetDepthStencilSurface()
{
    return sm()->depthStencil;
}

// PE 0x10025A70, a non-virtual member: the desired render target at `index`, without an AddRef.
// The shipped body addresses `this + 0x1E90 + 4 * index` (only index 0 has storage there); 1..3 read
// the MRT slots.
IDirect3DSurface9* smGetRenderTarget(StateManager* self, unsigned int index)
{
    if (index > 3)
        return 0;
    return index ? self->extraRenderTarget[index - 1] : self->renderTarget;
}

// PE 0x10005790: loads the state manager singleton and tail-jumps to 0x10025A70 with the caller's
// index. The shipped readers (gas mask, flashbang, the FSQuadDrawer passes, post-production) call
// 0x10025A70(0) on the singleton directly; other TUs reach it through this function.
IDirect3DSurface9* stateManagerGetRenderTarget(unsigned int index)
{
    return smGetRenderTarget(sm(), index);
}

// The DeviceLostResetListener subobject (`dword_1023B984 + 4`) that Renderer slot +0xB8 drives
// directly. [CHOSEN, not recovered] seam (issue #7): the subobject's native offset differs on x64.
DeviceLostResetListener* stateManagerDeviceLostListener()
{
    return reinterpret_cast<DeviceLostResetListener*>(&sm()->listenerVptr);
}

// PE 0x100057E0. The retail helper takes six arguments (flags, colour, depth, stencil, rectangle count,
// rectangle array) and every retail caller passes zero for the last two; the defaults here are those zeros.
// It first commits deferred render-target/depth state through the state manager singleton, then calls
// the D3D device Clear (vtable +0xAC) with the count and array leading, the flags after them.
void stateManagerClear(unsigned int flags, unsigned int colour, float depth,
                       unsigned int stencil, unsigned int rectCount, const D3DRECT* rects)
{
    flushRenderTargets(sm());
    rendererDevice()->Clear(rectCount, rects, flags, colour, stateManagerClearDepth(depth), stencil);
}

// sub_10025A00 - true when no push bracket is open (renderFrame's :991 sanity check).
bool stateManagerIsFullyRestored()
{
    return sm()->pushDepth == 0;
}

// sub_10026220 - collapse any open bracket to depth 1 and pop it, scheduling every restore.
void stateManagerForceRestore()
{
    StateManager* self = sm();
    if (self->pushDepth)
    {
        self->pushDepth = 1;
        popBracket(self);
    }
}

// sub_10025BA0 - the persistent render-state set: write the device mirror AND the base value, mark
// the cell held, and issue immediately - bypassing the pending machinery.  The concrete block is
// known to be RenderStateBlock here, so call its recovered issuer body directly instead of
// reconstructing the PE's hand-indexed virtual dispatch. renderFrame uses this for the
// multisampleantialias force and the atoc brackets.
void StateManager::setRenderStatePersistent(unsigned int state, unsigned int value)
{
    StateCell& cell = renderStates.cells[state];
    cell.flags |= 4u;
    cell.f0Device = value;
    cell.f1Base   = value;
    rsIssue(&renderStates RD9_THUNK_ARG, state, value);
}

void stateManagerSetRenderStatePersistent(unsigned int state, unsigned int value)
{
    sm()->setRenderStatePersistent(state, value);
}

// PE renderFrame 0x1003027D/0x100302E9/0x1003032D/0x10030388.  The caller decides whether the
// transparency-AA probe succeeded; this owner operation preserves the probe-selected state and
// exact four-byte vendor code while keeping the StateManager's persistent update internal.
void StateManager::setTransparencyAA(bool enabled)
{
    unsigned int code;
    ::memcpy(&code, enabled ? g_transparencyAAOn : g_transparencyAAOff, sizeof(code));
    setRenderStatePersistent(g_transparencyAAState, code);
}

void stateManagerSetTransparencyAA(bool enabled)
{
    sm()->setTransparencyAA(enabled);
}

// sub_10026240 as a seam - RendDX9Draw.cpp's beginScene flushes the render-target mirror before a
// frame clear so the clear lands on the current target.
void flushRenderTargetsSeam()
{
    flushRenderTargets(sm());
}

void stateManagerPreDrawApply()
{
    preDrawApply(sm());
}

// Owner-TU entry points for the three dice binding slots.  Their retail callers dispatch through
// the primary vtable, but the concrete owner and exact slot bodies are recovered here; direct
// calls avoid duplicating an x86-only raw-vtable ABI at every wrapper boundary.
void stateManagerSetIndices(IDirect3DIndexBuffer9* indices)
{
    smSetIndices(sm(), indices);
}

void stateManagerSetStreamSource(unsigned int stream, IDirect3DVertexBuffer9* vertexBuffer,
                                 unsigned int offset, unsigned int stride)
{
    smSetStreamSource(sm(), stream, vertexBuffer, offset, stride);
}

void* stateManagerVertexDeclaration()
{
    return sm()->vertexDecl;
}

void stateManagerSetVertexDeclaration(IDirect3DVertexDeclaration9* declaration)
{
    smSetVertexDeclaration(sm(), declaration);
}

// the +0x34 / +0x44 slots (0x10025BF0 / 0x10025C20) for the display-mode render
// probe (RendDX9Init.cpp, sub_10009FF0: 0x1000A150 `call [ecx+34h]`, 0x1000A15F `call [ecx+44h]`),
// which binds the shaders it has just assembled. Same deferred mirror as every other caller: the
// next pre-draw apply issues them.
void stateManagerSetVertexShader(IDirect3DVertexShader9* shader)
{
    smSetVertexShader(sm(), shader);
}

void stateManagerSetPixelShader(IDirect3DPixelShader9* shader)
{
    smSetPixelShader(sm(), shader);
}

// [RENDER] RASM into its reserved pixel registers, past the filter above; flushed at the next draw.
void stateManagerSetSunCascadeConstants(const float* constants)
{
    StateManager* const self = sm();
    constFileWrite(self->psConst, self->psConstDirty, kSunCascadeRegister, constants,
                   kSunCascadeVec4s);
}

// [RENDER] RaLodDither into its reserved pixel register, past the filter above.
void stateManagerSetLodDither(float a, float b)
{
    StateManager* const self = sm();
    const float value[4] = { a, b, 0.0f, 0.0f };
    constFileWrite(self->psConst, self->psConstDirty, kLodDitherRegister, value, 1u);
}

// [RENDER] RaAerial into its reserved pixel register, past the filter above.
void stateManagerSetAerial(const float* value)
{
    StateManager* const self = sm();
    constFileWrite(self->psConst, self->psConstDirty, kAerialRegister, value, 1u);
}

// [RENDER] RaSunFactor into its reserved pixel register, past the filter above.
void stateManagerSetSunFactor(const float* value)
{
    StateManager* const self = sm();
    constFileWrite(self->psConst, self->psConstDirty, kSunFactorRegister, value, 1u);
}

// [RENDER] RaSkySH into its reserved pixel registers, past the filter above.
void stateManagerSetSkySh(const float* value)
{
    StateManager* const self = sm();
    constFileWrite(self->psConst, self->psConstDirty, kSkyShRegister, value, kSkyShVec4s);
}

// [RENDER] RaWaterSsr into its reserved pixel register (contract 12).
void stateManagerSetWaterSsr(const float* value)
{
    StateManager* const self = sm();
    constFileWrite(self->psConst, self->psConstDirty, kWaterSsrRegister, value, 1u);
}

// [RENDER] RaWaterMedium into its reserved pixel registers (contract 42).
void stateManagerSetWaterMedium(const float* value)
{
    StateManager* const self = sm();
    constFileWrite(self->psConst, self->psConstDirty, kWaterMediumRegister, value, kWaterMediumVec4s);
}

// [RENDER] RaWaterFoam into its reserved pixel register (contract 18).
void stateManagerSetWaterFoam(const float* value)
{
    StateManager* const self = sm();
    constFileWrite(self->psConst, self->psConstDirty, kWaterFoamRegister, value, 1u);
}

// [RENDER] RaCloudShadow into its reserved pixel registers (contract 16).
void stateManagerSetCloudShadow(const float* value)
{
    StateManager* const self = sm();
    constFileWrite(self->psConst, self->psConstDirty, kCloudShadowRegister, value, kCloudShadowVec4s);
}

// [RENDER] RaSunStatic into its reserved pixel registers (contract 36).
void stateManagerSetSunStatic(const float* value)
{
    StateManager* const self = sm();
    constFileWrite(self->psConst, self->psConstDirty, kSunStaticRegister, value, kSunStaticVec4s);
}

// FXParameter.cpp's sparse-matrix fast path uploads bone rows through vslot +0x38.
void stateManagerSetVertexShaderConstantF(unsigned int reg, const float* data, unsigned int count)
{
    smSetVertexShaderConstantF(sm(), reg, data, count);
}

// [RENDER] The submission layer (Submit.cpp) binds what a D3DX pass would, through the same
// mirror: textures, sampler states, and the pixel, integer and boolean constants (the pixel
// constants keep the engine's reserved registers, see smSetPixelShaderConstantF).
void stateManagerSetTexture(unsigned int stage, IDirect3DBaseTexture9* texture)
{
    smSetTexture(sm(), stage, texture);
}

void stateManagerSetSamplerState(unsigned int stage, unsigned int type, unsigned int value)
{
    smSetSamplerState(sm(), stage, type, value);
}

void stateManagerSetPixelShaderConstantF(unsigned int reg, const float* data, unsigned int count)
{
    smSetPixelShaderConstantF(sm(), reg, data, count);
}

void stateManagerSetShaderConstantI(bool pixel, unsigned int reg, const int* data,
                                    unsigned int count)
{
    if (pixel)
        smSetPixelShaderConstantI(sm(), reg, data, count);
    else
        smSetVertexShaderConstantI(sm(), reg, data, count);
}

void stateManagerSetShaderConstantB(bool pixel, unsigned int reg, const BOOL* data,
                                    unsigned int count)
{
    if (pixel)
        smSetPixelShaderConstantB(sm(), reg, data, count);
    else
        smSetVertexShaderConstantB(sm(), reg, data, count);
}


// The base renderer's draw wrappers, 0x10005870 (DrawPrimitive) and 0x10005820
// (DrawIndexedPrimitive). Both live in the base TU in the shipped image but belong to this
// subsystem's story: pre-draw apply, the device draw, then the frame-stats bump (base+0x108 block:
// enable byte +644; DIP counters +452/+456, DP counters +460/+464, shared prim total +448 -
// sub_1000D5B0 / sub_1000D620). A zero primitive count skips everything, wrappers included.

// PE 0x1000D620 statistics helper is inlined in this draw wrapper.
void rendererDrawPrimitive(unsigned int primCount, unsigned int type, unsigned int startVertex)
{
    if (!primCount)
        return;
    preDrawApply(sm());
    rendererDevice()->DrawPrimitive(static_cast<D3DPRIMITIVETYPE>(type), startVertex, primCount);
    drawStatsDraw(primCount);   // [DEV] BF2_DRAW_STATS
    unsigned char* stats = static_cast<RendDX9Base*>(g_rendererBase)->renderStats;
    if (stats[644])
    {
        ++*reinterpret_cast<unsigned int*>(stats + 460);
        *reinterpret_cast<unsigned int*>(stats + 464) += primCount;
        *reinterpret_cast<unsigned int*>(stats + 448) += primCount;
    }
}

// Submits an indexed draw. Argument order follows this declaration, which differs from
// the original helper's call convention: keep type and numVertices in their named positions.
// PE 0x1000D5B0 statistics helper is inlined in this draw wrapper.
void rendererDrawIndexedPrimitive(unsigned int primCount, unsigned int type, int baseVertex,
                                  unsigned int minIndex, unsigned int numVertices,
                                  unsigned int startIndex)
{
    if (!primCount)
        return;
    preDrawApply(sm());
    rendererDevice()->DrawIndexedPrimitive(static_cast<D3DPRIMITIVETYPE>(type), baseVertex,
                                           minIndex, numVertices, startIndex, primCount);
    drawStatsDraw(primCount);   // [DEV] BF2_DRAW_STATS
    rendererCountIndexedDraw(primCount);
}

// PE 0x1000D5B0, the indexed wrapper's statistics helper (inlined there in the shipped image).
void rendererCountIndexedDraw(unsigned int primCount)
{
    unsigned char* stats = static_cast<RendDX9Base*>(g_rendererBase)->renderStats;
    if (stats[644])
    {
        ++*reinterpret_cast<unsigned int*>(stats + 452);
        *reinterpret_cast<unsigned int*>(stats + 456) += primCount;
        *reinterpret_cast<unsigned int*>(stats + 448) += primCount;
    }
}

// sub_100058B0 - the UP (user-pointer) variant, for SwiffRender's line strips.
// Read in full: clear stream 0 through the StateManager first (D3D requires no bound stream for a
// UP draw), pre-draw apply, DrawPrimitiveUP, then the UP stat pair (+468/+472; sub_1000D650) and
// the shared prim total (+448).
// PE 0x1000D650: the statistics helper (++(+0x1D4), (+0x1D8) += count, (+0x1C0) += count while +0x284 is set), inlined here.
void rendererDrawPrimitiveUP(unsigned int primCount, const void* data,
                             unsigned int stride, unsigned int type)
{
    if (!primCount)
        return;
    stateManagerSetStreamSource(0, 0, 0, 0);
    preDrawApply(sm());
    rendererDevice()->DrawPrimitiveUP(static_cast<D3DPRIMITIVETYPE>(type), primCount, data,
                                      stride);
    drawStatsDraw(primCount);   // [DEV] BF2_DRAW_STATS
    unsigned char* stats = static_cast<RendDX9Base*>(g_rendererBase)->renderStats;
    if (stats[644])
    {
        ++*reinterpret_cast<unsigned int*>(stats + 468);
        *reinterpret_cast<unsigned int*>(stats + 472) += primCount;
        *reinterpret_cast<unsigned int*>(stats + 448) += primCount;
    }
}

} // namespace dx9rend
} // namespace hfe
} // namespace dice

#endif // BF2_IMAGE_RENDDX9
