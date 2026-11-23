// Layout view over renderer storage, not a C++ base class. A second vptr would shift fields.
// [RETIRE-WHEN: RW-56] Replace the overlay after the base vtable is fully typed and
// inheritance preserves the validated offsets.


#ifndef BF2_RENDDX9_RENDDX9BASE_H
#define BF2_RENDDX9_RENDDX9BASE_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d9.h>

#include <list>
#include <string>
#include "../../RendDX9Abi/DisplayModeAbi.h"

#include <vector>

namespace dice {
namespace hfe {
class IVideoOptions;
struct Mat4;
class Frustum;

namespace dx9rend {

struct EngineTexture;
class MovieRenderer;     // MovieRenderer.h - the demo movie recorder at +0x3F4

// The two listener contracts embedded throughout RendDX9.dll.  Neither interface has a virtual
// destructor in the retail image: the device listener table is exactly three slots and the render
// path listener table is exactly six.  Owners destroy concrete objects through their own type.
class DeviceLostResetListener
{
public:
    virtual void onDeviceLost() = 0;
    virtual void onDeviceReset() = 0;
    virtual void onDeviceDestroy() = 0;
};

class RenderPathListener
{
public:
    virtual void onRenderPathActivated(int path) = 0;
    virtual void onRenderPathDeactivated() = 0;
    virtual void onRenderPathChanged(int path) = 0;
    virtual void onRenderPathNotification(int path) = 0;
    virtual int  getCurrentRenderPath() const = 0;
    // The base table 0x101DFFDC carries a body for this slot (every constructor replaces that
    // vptr at once). PE 0x10027880.
    virtual std::string getRenderPathListenerName() const { return "Unspecified client"; }
};

#if defined(_M_IX86)
static_assert(sizeof(DeviceLostResetListener) == 4,
              "device-lost listener is one three-slot vptr");
static_assert(sizeof(RenderPathListener) == 4,
              "render-path listener is one six-slot vptr");
#endif

// The device-lost/reset listeners (retail +0x5C: a std::list of {priority, listener}, then the lost
// and sort-dirty bytes). Kept in registration order and sorted by unsigned priority, stably, before
// each broadcast. Constructed in place by constructD3DHalf.
struct DeviceLostResetEntry
{
    int                      priority;
    DeviceLostResetListener* listener;
};

struct DeviceLostResetList
{
    DeviceLostResetList() : deviceLost(0), dirty(0) {}

    std::list<DeviceLostResetEntry> entries;
    unsigned char                   deviceLost;
    unsigned char                   dirty;
};

// The render-path listeners (retail +0x6C: the active path, a std::list of listeners, the changed
// byte). Constructed in place by renderPathListCtor. `constructed` is [CHOSEN, not recovered]: the
// Renderer zeroes this storage first, and the paths that can run before construction report it.
struct RenderPathList
{
    RenderPathList() : activeRenderPath(0), changed(0), constructed(1) {}

    int                            activeRenderPath;
    std::list<RenderPathListener*> listeners;
    unsigned char                  changed;
    unsigned char                  constructed;
};

// HemiMapManager.cpp's editor/offline bake is deliberately routed through typed owner-TU seams.
// The renderer owns all of the byte-offset state below; keeping that knowledge in BF2Render.cpp
// prevents the recovered Hemi class from becoming another raw-vtable/offset facade.
struct HemiRenderState
{
    unsigned char bytes[0x2C];
};


// The display-mode records the enumeration subsystem builds, held in std::vectors (issue #7).


// An 8-byte multisample descriptor from CheckDeviceMultiSampleType: the D3DMULTISAMPLE_TYPE and the
// (quality-count - 1). Held per-mode and, parallel to the AA string list, inside VideoOptions.
typedef renddx9abi::DisplayModeMultiSample MultiSample8;

// The display-mode record (retail: 40 bytes - the sub_1000CEE0 header plus the two nested lists
// sub_100091E0 deep-copies and sub_10009510 frees). Shared with BF2.exe, which reads the list
// Renderer +0x3C returns; see DisplayModeAbi.h.
typedef renddx9abi::DisplayModeRecord Mode40;

static_assert(sizeof(MultiSample8) == 8, "multisample descriptor is 8 bytes");


// VideoOptions - constructed inside the base object at retail +0x3F8 (1016) and reached through
// the base's vtable slot +0x74, which is one instruction: `return this + 1016`.

// Five parallel eight-entry rows. getQuality(i) - VideoOptions vtable +0x04, `sub_100112B0` - is
// `return this[i + 10]`, so it reads `quality[]` and nothing else.

struct VideoOptions
{
    /* 0x000 */ void*         vptr;              // VideoOptions_vftable_primary, 15 slots
    /* 0x004 */ void*         vptrEventHandler;  // the ECCoreEventHandler subobject
    /* 0x008 */ int           rowA[8];           // all 4          [UNRESOLVED] role
    /* 0x028 */ int           quality[8];        // all 2   <- the one getQuality reads
    /* 0x048 */ int           rowC[8];           // 1,1,1,0,0,1,1,1   [UNRESOLVED] role
    /* 0x068 */ int           rowD[8];           // all 3          [UNRESOLVED] role
    /* 0x088 */ int           maxQuality[8];     // all 3, then clamped by hardware caps
    /* 0x0A8 */ unsigned char flags[8];          // all 0
    // The two dynamic-option string lists. setDynamicOption (sub_1000FB70) and the readback slot
    // +0x20 (sub_10011350) read their retail begin pointers at +0xB4/+0xC4.
    /* 0x0B0 */ std::vector<std::string> aaList;    // anti-aliasing option strings ("Off", "%dSamples")
    /* 0x0C0 */ std::vector<std::string> resList;   // resolution strings ("%ix%i@%iHz")
    // AND THE correction IS load-bearing. The initialize tail writes -1 to
    // 0xD0, 0xD4, 0xD8 AND 0xDC. apply guards its resolution-change path on `*(this+212)!=*(this+220)`
    // (0xD4 vs 0xDC); four equal -1 values make the guard false, so the device-reset path is correctly
    // skipped on a first boot - which is what makes apply writable at all.

    // 0xD8/0xDC are also the selected dynamic-option indices: setDynamicOption writes this[optIdx+54]
    // (0xD8 for AA, 0xDC for resolution) and slot +0x20 reads them. -1 means "nothing selected yet".
    /* 0x0D0 */ int           minusOneD0;        // dword 52
    /* 0x0D4 */ int           minusOneD4;        // dword 53   <- apply's guard, left side
    /* 0x0D8 */ int           minusOneD8;        // dword 54   <- selected AA index (init -1)
    /* 0x0DC */ int           minusOneDC;        // dword 55   <- apply's guard right / selected resolution index
    // The Mode40 kept parallel to resList (sub_10010710 pushes here as it pushes each resolution
    // string; setDynamicOption reads *(this+0xE4)+40*resIndex to hand the chosen mode to the AA
    // builder), and the MultiSample8 descriptors kept parallel to aaList.
    /* 0x0E0 */ std::vector<Mode40>       modeCache;
    /* 0x0F0 */ std::vector<MultiSample8> aaDescriptors;
    /* 0x100 */ float         dynFloat;          // the float slot +0x24 clamps into [0.5,1.0]
    /* 0x104 */ int           windowSizeX;       // dword 65, GSWindowSizeX
    /* 0x108 */ int           windowSizeY;       // dword 66, GSWindowSizeY
    /* 0x10C */ int           refreshHz;         // dword 67, GSRefresh
    /* 0x110 */ int           skipMipLevels;     // dword 68, GSSkipMipLevels (default -1)
    // The dirty flags apply sets when a setting actually changes. +0x119 is the one apply's first
    // statement tests - "Not allowed to apply video options when a game is loaded!".
    /* 0x114 */ unsigned char dirty114;
    /* 0x115 */ unsigned char dirty115;
    /* 0x116 */ unsigned char dirty116;
    /* 0x117 */ unsigned char dirty117;
    /* 0x118 */ unsigned char dirty118;
    /* 0x119 */ unsigned char gameLoaded;
    /* 0x11A */ unsigned char opaque11A[0x11C - 0x11A];
};

struct RendDX9Base
{
    /* 0x000 */ void*                  vptr;              // 0x101DDA38 in the shipped image
    /* 0x004 */ int                    refCount;          // base ctor: 1
    /* 0x008 */ IDirect3D9*            d3d9;              // Direct3DCreate9(0x20)
    /* 0x00C */ IDirect3DDevice9*      device;            // CreateDevice's out-parameter
    // [INFERRED] name. The three words rendererSetDebugTextColour (PE 0x10009FA0, unreachable in 1.50)
    // copies from its argument; no instruction reads them back.
    /* 0x010 */ unsigned int           debugTextRgb[3];
    // The colour the frame-time overlay (sub_10029C50) sets on the DebugTextWriter; the base
    // constructor stores 0xFFFF0000. [INFERRED] name.
    /* 0x01C */ unsigned int           debugTextColour;
    // [INFERRED] name. The mode rendererSetHemiRenderMode stores (0..5; 2 and 3 select the
    // wireframe fill mode) and rendererGetHemiRenderMode returns.
    /* 0x020 */ int                    renderMode;

    // The present parameters live inside the object, at +0x24, and CreateDevice is handed `this+36`
    // directly. sub_1000A500 memsets exactly 0x38 bytes here, which is sizeof(D3DPRESENT_PARAMETERS)
    // and the check that the offset is right.
    /* 0x024 */ D3DPRESENT_PARAMETERS  pp;

    /* 0x05C */ DeviceLostResetList    deviceLostList;
    /* 0x06C */ RenderPathList         renderPathList;

    /* 0x080 */ unsigned int           adapter;           // base ctor: 0, set in sub_1000B0D0
    /* 0x084 */ unsigned int           deviceType;        // GSDeviceType,     default 1
    /* 0x088 */ unsigned int           behaviorFlags;     // GSDeviceBehavior, default 80 (0x50)
    // Numeric prefix of DxDiag adapter 0's English display-memory string; copied to DeviceCaps.
    /* 0x08C */ unsigned int           opaque08C;
    // The display-mode list slot_0x3C (sub_10007420) builds and returns (as `this+0x90`). Its retail
    // begin is at +0x94, which is what sub_10007420 and its caller sub_10010710 read.
    /* 0x090 */ std::vector<Mode40>    modeList;

    // the display mode, copied wholesale out of BF2's hWndBf2TempSlt
    // sub_1000CA40 targets +0xA0 (`lea ecx, [esi+0A0h]`), sub_10009250 lays out the scalars.
    /* 0x0A0 */ unsigned char          fullscreen;
    /* 0x0A1 */ unsigned char          padA1[3];
    /* 0x0A4 */ unsigned short         modeWidth;
    /* 0x0A6 */ unsigned short         modeHeight;
    /* 0x0A8 */ unsigned short         modeRefresh;
    /* 0x0AA */ unsigned char          modeBpp;
    /* 0x0AB */ unsigned char          padAB;

    // The depth-stencil candidates. BF2 pushes exactly one, D3DFMT_D24S8 (75).
    /* 0x0AC */ std::vector<unsigned int> depthFormats;

    // The multisample candidates, 8-byte entries. BF2 leaves this empty; VideoOptions' resolution
    // change (sub_1000F8D0, 0x1000F92C..0x1000F961) replaces it with the ONE selected AA descriptor,
    // which buildPresentParameters then hands to the Reset. Retail clears it by freeing its storage,
    // so an empty list holds no storage (data() is null); the helpers below keep that.
    /* 0x0BC */ std::vector<MultiSample8> multiSamples;

    /* 0x0CC */ unsigned char          vsync;
    /* 0x0CD */ unsigned char          padCD[3];

    // The window list. hDeviceWindow is its first entry.
    /* 0x0D0 */ std::vector<void*>     windows;

    // The swap chains: the device's chain 0, then one additional chain per further window. present
    // and the render-target copy index it by currentSwapChain.
    /* 0x0E0 */ std::vector<IDirect3DSwapChain9*> swapChains;
    /* 0x0F0 */ unsigned int           currentSwapChain;  // base ctor: 0
    // The temporary back buffer the render-target selector sub_10005920 prefers over the Renderer's
    // back buffer (+0x76C); the 2006 build's assert names it m_tempBackBuffer.
    /* 0x0F4 */ IDirect3DSurface9*     tempBackBuffer;
    /* 0x0F8 */ void*                  renderView;        // zeroed here, filled by Renderer's ctor
    // A second render view, and it is a different object from the one above. The constructor builds
    // renderView directly; this one comes from createInstance(820301, 820201) in initialize step 8.
    // Same class, two instances - getRenderView() returns the first, never this.
    /* 0x0FC */ void*                  renderView2;
    // The frame bracket's two bytes (RendDX9Draw.cpp). [INFERRED] names, from their writers:
    // beginScene sets +0x100 after a successful BeginScene, endScene clears both, present's
    // D3DERR_DEVICELOST arm clears +0x100; present(3) and a successful present set +0x101, present
    // refuses to run while it is set, and beginScene primes a present only while it is clear. The
    // base constructor stores 0 and 1 (0x1000B9F6 / 0x1000B9FC).
    /* 0x100 */ unsigned char          inScene;
    /* 0x101 */ unsigned char          framePresented;
    // [UNRESOLVED] role. Renderer::initialize step 14, the hemi top-down capture and RaShader's
    // permutation creation test it; nothing recovered writes it.
    /* 0x102 */ unsigned char          flag102;
    /* 0x103 */ unsigned char          pad103;
    // The frame ordinal Renderer::update increments and Renderer +0x54 returns.
    /* 0x104 */ unsigned int           frameOrdinal;
    // The draw/resource statistics block Renderer slot +0x6C returns (`lea eax, [ecx+108h]`): +0x1C
    // counts vertex buffers, +0x2C textures, +448..+472 the draw counters gated by the byte at +644.
    // [INFERRED] extent: it runs to the next known member.
    /* 0x108 */ unsigned char          renderStats[0x39C - 0x108];

    // Adapter identifiers populated during device creation; each raw D3D record is 1100 bytes.
    /* 0x39C */ std::vector<D3DADAPTER_IDENTIFIER9> adapterInfo;

    // The Present flags byte present(mode 2) substitutes and the wasstilldrawing-retry byte, set
    // only by the `renderer.presentAsync` / `renderer.presentSpinIfBusy` console properties
    // (0x10009E30 / 0x10009E50; attested names from the bindings). Base ctor: both 0.
    /* 0x3AC */ unsigned char          presentAsync;
    /* 0x3AD */ unsigned char          presentSpinIfBusy;
    /* 0x3AE */ unsigned char          opaque3AE[0x3B0 - 0x3AE];
    // The screenshot directory (`renderer.screenDumpPath`; the base constructor stores
    // "@HOME@/Screenshots", 0x1000BA38) and the requested file name (`renderer.dumpScreen`'s
    // argument, "" by default, 0x1000BA55). Constructed in place by constructD3DHalf.
    /* 0x3B0 */ std::string            screenDumpPath;
    /* 0x3CC */ std::string            screenDumpName;
    // Set by the screenshot request (0x10004F89); endScene captures and clears it.
    /* 0x3E8 */ unsigned char          screenDumpRequest;
    /* 0x3E9 */ unsigned char          opaque3E9[3];
    // Console property names; both default to one in the base constructor.
    /* 0x3EC */ int                    occlusionTerrainMaxFrames;
    /* 0x3F0 */ int                    occlusionObjectsMaxFrames;
    // The demo movie recorder (MovieRenderer.cpp): Renderer slot +0x64 builds it, slot +0x68
    // destroys it; renderFrame and slot +0x30 capture through it. Null unless a demo is rendered.
    /* 0x3F4 */ MovieRenderer*         movieRenderer;

    // VideoOptions_ctor(this + 1016), running to retail +0x513. Constructed in place by
    // videoOptionsConstruct and destroyed by destructD3DHalf.
    /* 0x3F8 */ VideoOptions           videoOptions;

    dice::hfe::IVideoOptions* embeddedVideoOptions()
    {
        return reinterpret_cast<dice::hfe::IVideoOptions*>(&videoOptions);
    }

    // sub_1000A6B0 and the alpha-to-coverage probe use these three adjacent bytes directly.
    /* 0x514 */ unsigned char          forceMultisample;
    /* 0x515 */ unsigned char          disableMultisample;
    /* 0x516 */ unsigned char          hasAlphaToCoverage;
    // [INFERRED] name. The ONE writer after construction is the Renderer's ECCore handler, id 15
    // CEActivateApp, which copies the event's payload byte here (0x1002F758: `mov [esi-21h], cl`,
    // esi = the +0x538 handler subobject);
    // the one reader is Renderer +0xC0 (0x100055A0: `return this[0x517] == 0`), which +0xBC polls
    // to wait out a deactivated application. The base constructor stores 1 (0x1000BAA1).
    /* 0x517 */ unsigned char          appActive;
    // The performance-feature bitset rendererPerformanceFlag tests.
    /* 0x518 */ unsigned int           performanceFlags;
    // Adapter diagnostic summary written by the Windows DxDiag probe, overwritten for each
    // enumerated display so the last display's summary remains. Default-constructed by the base.
    /* 0x51C */ std::string            string51C;
};

// The editor host (Tools/BF2Editor EngineHost) reads the device through the head
// {vptr, int refCount, IDirect3D9*, IDirect3DDevice9*}: +0x0C on x86, +0x18 on x64 (issue #7).
static_assert(offsetof(RendDX9Base, device) == (sizeof(void*) == 4 ? 0x0C : 0x18),
              "the renderer head the editor host reads");

// The x86 offsets ahead of the first std container, kept as evidence. The members from the
// device-lost list on are native and no longer sit at their retail offsets. The D3D structure
// sizes are retail x86 evidence only: every consumer uses sizeof and the named members (issue #7).
#if defined(_M_IX86)
static_assert(sizeof(D3DPRESENT_PARAMETERS) == 0x38,
              "sub_1000A500 memsets 0x38 bytes over the present parameters");
static_assert(offsetof(RendDX9Base, d3d9)              == 0x008, "d3d9 at +0x08");
static_assert(offsetof(RendDX9Base, device)            == 0x00C, "device at +0x0C");
static_assert(offsetof(RendDX9Base, pp)                == 0x024, "present parameters at +0x24");
static_assert(offsetof(RendDX9Base, deviceLostList)    == 0x05C, "device-lost list at +0x5C");
static_assert(offsetof(D3DPRESENT_PARAMETERS, PresentationInterval) == 0x34,
              "the vsync setter writes base+0x58 == pp+0x34");
static_assert(sizeof(D3DADAPTER_IDENTIFIER9) == 1100,
              "sub_1000C920 strides by 1100 - the element IS the raw D3D structure");
#endif


// The multisample list's operations (RendDX9Base::multiSamples).

// Empties the list and frees its storage.
inline void clearMultiSampleList(RendDX9Base* b)
{
    std::vector<MultiSample8>().swap(b->multiSamples);
}

inline void pushMultiSample(RendDX9Base* b, const MultiSample8& value)
{
    b->multiSamples.push_back(value);
}

// Replaces the list with [first, last). The replacement is built before the old storage is freed;
// an empty range leaves no storage.
inline void assignMultiSampleList(RendDX9Base* b, const MultiSample8* first,
                                  const MultiSample8* last)
{
    std::vector<MultiSample8>(first, last).swap(b->multiSamples);
}

// The list half of sub_1000A6B0's multisample probe: a first descriptor whose type, read as a
// signed int, is positive.
inline bool multiSampleListActive(const RendDX9Base* b)
{
    return !b->multiSamples.empty() && static_cast<int>(b->multiSamples[0].type) > 0;
}


void videoOptionsConstruct(void* base);  // 10010590, before any D3D initialization
void videoOptionsInitialize(void* base);


// hWndBf2TempSlt - the host's display-mode carrier (retail: the recovered 0x40-byte record). See
// DisplayModeAbi.h.

typedef renddx9abi::HostDisplayMode HWndBf2TempSlt;

// Implemented in RendDX9Init.cpp.
void  constructD3DHalf(void* renderer);
// The base destructor (PE 0x1000BC20) past its vptr store: drops the movie recorder, releases the
// IDirect3D9 object unconditionally, unregisters the three renderer events (category 11, ids
// 1..3), releases the second render view, then destroys the base members in reverse order. The
// Renderer's deleting destructor runs it once, after the Renderer's own members are destroyed.
void  destructD3DHalf(void* renderer);
// PE 0x1000ADB0. Tears the D3D subsystems down in the shipped order and releases the device;
// false (after a fatal severity-6 diagnostic) when a manager release fails. Renderer::shutdown
// discards the result.
bool  RendDX9_shutdownD3DSubsystems(void* renderer);
// PE 0x100051D0 (Renderer slot +0x68, also called directly by the base destructor): destroy and
// free the movie recorder, then clear the slot. Defined in BF2Render.cpp beside the slot.
void  destroyMovieRenderer(RendDX9Base* base);
void  copyDisplayModeFrom(void* renderer, const HWndBf2TempSlt* src);
void  clearDisplayModeCopies(void* renderer);
void  renderPathListCtor(void* list);
void  rendererStatsConstruct(unsigned char* stats);   // PE 0x1000D900 / 0x1000D300; `stats` = base + 0x108
void  rendererStatsSetCategory(unsigned char* stats, unsigned int category);   // PE 0x1000D5E0
void  rendererSetStatsCategory(unsigned int category);                         // PE 0x10005910
void  setActiveRenderPath(RenderPathList& list, int path);
void  registerDeviceLostResetListener(DeviceLostResetListener* listener, int priority);
void  unregisterDeviceLostResetListener(DeviceLostResetListener* listener);
// Several recovered classes still expose their listener subobject only as an attested byte
// offset.  Keep that cast in these compatibility overloads; list storage and dispatch stay typed.
void  registerDeviceLostResetListener(void* listener, int priority);
void  unregisterDeviceLostResetListener(void* listener);
void  notifyDeviceLostResetListenersLost();      // 0x10002530 on the global renderer's list
void  notifyDeviceLostResetListenersReset();
void  notifyDeviceLostResetListenersDestroy();   // RendDX9_shutdownD3DSubsystems only
// Device reset and cooperative-level handling.
bool  resetDevice(void* renderer);               // sub_10009A60, RendDX9Init.cpp:401
bool  handleLostDevice(void* renderer);          // sub_10009BA0, the TestCooperativeLevel loop
// The `renderer.vsync` / `renderer.presentAsync` / `renderer.presentSpinIfBusy` properties' pairs.
void  rendererSetVSync(void* renderer, bool enabled);             // resets the device
bool  rendererGetVSync(const void* renderer);
void  rendererSetPresentAsync(void* renderer, bool enabled);
bool  rendererGetPresentAsync(const void* renderer);
void  rendererSetPresentSpinIfBusy(void* renderer, bool enabled);
bool  rendererGetPresentSpinIfBusy(const void* renderer);
void  registerRenderPathListener(RenderPathListener* listener);
void  unregisterRenderPathListener(RenderPathListener* listener);
void  registerRenderPathListener(void* listener);
void  unregisterRenderPathListener(void* listener);
bool  rendererPerformanceFlag(unsigned int bit);
unsigned int rendererFrameOrdinal();
int   readIntSetting(const char* name, int defaultValue);
bool  readBoolSetting(const char* name, bool defaultValue);
// Settings vslot +0x40 - const std::string& getString(name, default), read from
// TextureManager_loadTextureFromFile's two GSLoadLevel/GSLevelDirectory calls at 0x1001860C.
void  readStringSetting(const char* name, std::string& out);
// Same Settings +0x40 getter with the caller-provided default preserved.  HemiMapManager uses the
// retail non-empty default " ERROR ", so routing it through readStringSetting's empty default
// would be observable on a missing key.
void  readStringSettingDefault(const char* name, const char* defaultValue, std::string& out);
// sub_100010D0 on dword_1023B980 - BYTE1 of D3DCAPS9.PixelShaderVersion (+0xCC), the PS major.
int   deviceCapsPixelShaderMajor();
// sub_100010B0 on dword_1023B980 - BYTE1 of D3DCAPS9.VertexShaderVersion (+0xC4), the VS major.
// Terrain::init picks the low render style when it is below 2.
int   deviceCapsVertexShaderMajor();
bool  deviceCapsIsAti();
bool  deviceCapsIsNvidia();
bool  deviceCapsSupportsTextureFormat(unsigned int format);
bool  deviceCapsSupportsFullSizeRenderTargets();
// The DeviceCaps dword at retail +0x584 (RendDX9Init.cpp's DeviceCaps::unknown584). Requires the
// published DeviceCaps object.
unsigned int deviceCapsUnknown584();
unsigned int deviceCapsMaxPrimitiveCount();          // PE 0x10001100
unsigned int deviceCapsMaxVertexIndex();             // PE 0x10001110
unsigned int deviceCapsMaxTextureWidth();            // PE 0x10001160
bool deviceCapsSupportsCubeTextureFormat(unsigned int format);   // PE 0x10001400
long displayDriverStringCopy(char* destination, unsigned int capacity, const char* source);   // PE 0x10001030
GUID         deviceCapsDeviceIdentifier();           // PE 0x10001260
bool         deviceCapsSupportsDeclType(unsigned int declType);   // PE 0x100013A0


// TextureManager.cpp - the loader's cross-TU surface. The shipped callers reach these
// as __thiscall members on dword_1023B974 / direct calls; these seams carry the same behaviour.

// sub_10019260 - the by-name loader. Returns an EngineTexture* (never null once the manager
// exists: even a missing file yields the wantu.dds fallback wrapper), addRef'd for the caller.
void* textureManagerGetTexture(const std::string& name);
// sub_10017E00 / sub_10017E20 / sub_10017E40 - swap-style setters returning the OLD value.
// Consumers bracket loads with them: Meme forces class 1 + managed, Font::Ported forces managed=0
// for "_def", and Meme's createTexture forces recreate=1 so its render targets survive a reset.
int   textureManagerSwapDefaultMipClass(int value);
int   textureManagerSwapManaged(int value);
int   textureManagerSwapRecreateMode(int value);
// VideoOptions::apply's two stores into the manager's fourteen mip-class records (TM+0x28, 8 bytes
// each: an enabled byte, then the value). The first gives every enabled record `value`; the second
// sets record `index` regardless of its flag. Both require a live manager.
void  textureManagerSetEnabledMipClassValues(int value);
void  textureManagerSetMipClassValue(int index, int value);
// sub_10018550 - create a code-made 2D texture (depth 1) through the normal create path; `existing`
// non-null re-creates into that wrapper (the onDeviceReset arm).
void* textureManagerCreateEmpty(unsigned int w, unsigned int h, unsigned int format,
                                unsigned int mips, unsigned int usage, void* existing);
void* textureManagerCreateCube(unsigned int side, unsigned int format,
                               unsigned int mips, unsigned int usage, void* existing);
void* createRaShaderConstants(IDirect3DDevice9* device);
void  destroyRaShaderConstants();
// [CHOSEN, not recovered] The address of the RaShaderConstants block (g_raShaderConstants) word
// that sits at `retailByteOffset` in the retail x86 0xAFC-byte layout (issue #7). The x86 block
// keeps that layout, so this is block + offset there; the x64 block widens its pointer words, and
// this maps each retail offset onto the native one. A record's value words stay contiguous in both
// layouts. Requires the published block.
unsigned char* raShaderConstantsAt(unsigned int retailByteOffset);
float* raShaderConstantsWaterFogRange();                                  // PE 0x10037EA0
void   raShaderConstantsAmbientIlluminationRange(float out[4]);           // PE 0x100380E0
void   raShaderConstantsSetRoadFade(float fadeStart, float fadeEnd);      // PE 0x10038810
void  raShaderConstantsSetTexture(unsigned int holderOffset,
                                  IDirect3DBaseTexture9* texture);
void  raShaderConstantsSetEngineTexture(unsigned int holderOffset,
                                        EngineTexture* texture);
// Renderer::renderFrame's PE 0x100300A2..0x10030257 world-pass constants. These owner-TU
// operations retain the texture ownership semantics without exposing the private
// RaShaderConstants record layout to the renderer; the once-only projection pair stays in
// renderFrame as its statics, and the projection setter copies the two Vec4s (+0x294, +0x2A8).
void  raShaderConstantsSetTerrainLightAtlas(const float grid[4], const float gi[4]);
void  raShaderConstantsSetHemiMapTexture(EngineTexture* texture);
void  raShaderConstantsSetHemiMapInfo(const float constants[4]);
// The fog constants (RenderQueues.cpp). sub_10037F90 stores the FogColor holder (+0xBC..+0xC8)
// AND packs its xyz to an opaque D3DCOLOR for D3DRS_FOGCOLOR through the state manager's
// persistent setter - the fixed-function fog stage every Ra pass with `fogenable = true` blends
// by. sub_10037F60 / sub_10038110 / sub_10038140 are plain stores (FogRange +0xA8, WaterFogRange
// +0xF8, WaterFogColor +0x10C).
void  raShaderConstantsSetFogColor(const float rgbw[4]);
void  raShaderConstantsSetFogRange(const float range[4]);
void  raShaderConstantsSetWaterFogRange(const float range[4]);
void  raShaderConstantsSetWaterFogColor(const float rgb[3]);
// Terrain::setupWater's four float stores (+0x168 / +0x170 / +0x178 / +0x180): the value words of
// the holders the constructor names uvLevelAddX / uvLevelMulX / uvLevelAddY / uvLevelMulY.
void  raShaderConstantsSetUvLevel(float addX, float mulX, float addY, float mulY);
// The owned texture of an EngineTexture holder (holderOffset + 4) - Terrain::setupWater tests
// WaterMap (+0x1A0) before loading the water volume into it.
EngineTexture* raShaderConstantsGetEngineTexture(unsigned int holderOffset);
// PostProductionFrameWork.cpp - the owner-block fog record ("THE FOG RECORD" there). The console
// colours are 0..255 on the wire and stored normalised; the per-frame update derives the four
// constants above from the record. BF2Render.cpp's frame function calls the update where the
// shipped renderFrame does (PE 0x10030036..0x10030048).
void  postProductionSetFogColor255(const float rgb[3]);
void  postProductionGetFogColor255(float rgb[3]);
void  postProductionSetWaterFogColor255(const float rgb[3]);
void  postProductionGetWaterFogColor255(float rgb[3]);
void  postProductionSetWaterFogStartEndAndBase(const float range[4]);
void  postProductionGetWaterFogStartEndAndBase(float range[4]);
void  postProductionUpdateFog(float underwaterDepth);
// BF2Render.cpp - sub_100F7B00 / sub_100F7AD0: the camera's depth below the water plane (0 when
// above it) and the under-water test the fog update forks on.
float underwaterDepth();
bool  cameraIsUnderWater();
// sub_100191C0 - drop the cache's reference to `tex` and erase its node (destroys the texture if
// the cache held the last reference).
void  textureManagerEvictFromCache(void* tex);
// sub_10017F90 - save `texture` as "<name>.dds" through the FileManager's write path (D3DXIFF_DDS);
// true only on D3D_OK. A TextureManager member whose body never reads the manager (TextureManager.cpp).
bool  textureManagerSaveTexture(EngineTexture* texture, const std::string& name);
// Reads level `level` (face `face`) of render target `source` back into the same
// level of `destination` through GetRenderTargetData; a null `source` reads the current swap
// chain's back buffer. False when `destination` is null or the copy fails (logged).
bool  textureManagerCopyRenderTarget(EngineTexture* source, EngineTexture* destination,
                                     unsigned int level, unsigned int face);

// The menu atlas cache (dword_10249EF0, a 0x2C-byte object owned by TextureManager.cpp).
bool  atlasCacheExists();                                  // dword_10249EF0 != 0
void* atlasCacheEnsure();                                  // create-if-null; returns the cache
void  atlasCacheAddRef();                                  // vslot +0x00 - addRef-first table
bool  atlasCacheHasAtlases();                              // the byte at cache+0x18
bool  atlasCacheLoadAtlases(const std::string& prefix);
// sub_10017440 - first atlas whose map holds `name`; returns the 20-byte entry or null.
struct TextureAtlasEntry;
TextureAtlasEntry* atlasCacheFind(const std::string& name);
// The cache vtable's slot +0x08 (0x10017B50) - release; destroys the cache (and clears the global)
// at zero. Returns the remaining count, which is what Meme slot +0x60 keys on.
int   atlasCacheRelease();

// The shipped path/string helpers (bodies at 0x10074D10 / 0x10075730 / 0x10075A50; defined in
// TextureManager.cpp, used by the atlas and the Meme texture as well). The extension is Core's
// extractFileExt (Core/StringUtil.h).
void pathToLower(std::string& s);                             // tolower + the five Swedish maps
void pathReplaceSeparators(std::string& s, char with);        // '/' and '\\' -> `with`
void pathStripExtension(std::string& out, const std::string& in);

// The Y rotation by `degrees`: the whole matrix to identity, then m0 = cos, m2 = -sin, m8 = sin and
// m10 = cos of the float-rounded radians (defined in BF2Render.cpp).
void setRotationYDegrees(Mat4& matrix, float degrees);

// [CHOSEN] rebuild seam: std::string publishes no (ptr, len) assign, so substring builders route
// through this. Behaviour, not ABI - the original inlines MSVCP71's substr.
void pathAssignSubstring(std::string& out, const char* s, unsigned int len);

// Default priority assigned to new textures; atlas loading temporarily uses priority 15.
int  resourceManagerSwapDefaultPriority(int value);
int  resourceManagerDefaultPriority();
// The byte at RM+0x04 (sub_1000D950) - 1 defers file reads during init, 0 after step 16.
bool resourceManagerIsReady();
void resourceManagerSetReady(unsigned char value);   // sub_1000D960

// ShaderManager.cpp - what the effect compiler reads out of it. See those bodies for the offsets.
struct ShaderManagerDefines
{
    std::string* first;
    unsigned int  count;      // elements, not pairs - the vector alternates name, value, name, ...
    const char*   at(unsigned int i) const;
};
ShaderManagerDefines shaderManagerDefines(void* shaderManager);
void*         shaderManagerInclude(void* shaderManager);
unsigned long shaderManagerEffectFlags(void* shaderManager);
void*         renderDevice();
void*         compileEffectFromFile(const char* rawPath);   // Effect.cpp, sub_1001E010
void*         shaderManagerGetEffect(void* shaderManager, const char* rawPath);
class Effect;
class ShaderParameter;
void*         effectGetShaderParameter(Effect* effect, const std::string& name);
ShaderParameter* effectGetShaderParameterTyped(Effect* effect, const std::string& name);
ShaderParameter* effectGetShaderParameterBorrowedTyped(Effect* effect,
                                                       const std::string& name);
void          effectAddRef(Effect* effect);
int           effectRelease(Effect* effect);
// sub_1001EE90 - recompile `path` with the current defines and rebind the effect in place.
bool          effectReload(Effect* effect, const std::string& path);
// sub_10023540 - reload every cached effect (after the shader defines changed).
void          shaderManagerReloadAllEffects(void* shaderManager);
// Clears every cached effect's registered texture bindings during level teardown.
void          shaderManagerClearEffectTextureBindings(void* shaderManager);
void          effectClearTextureParameters(Effect* effect);
bool          effectSetTechniqueByName(void* effect, const std::string& name);
// BufferManager.cpp, sub_10012260 plus the inline release/0x10014790 tail.  Mesh-renderer
// constructors own these wrappers exactly like Effect's annotation-driven declarations do.
void*         vertexDeclarationCreate(const void* elements);
void          vertexDeclarationRelease(void* wrapper);
// These three calls form the shadow
// submission bracket: begin the selected technique, begin one pass, and end
// the effect (which also closes the active pass in the shipped wrapper).
unsigned int  effectBegin(void* effect);
void          effectBeginPass(void* effect, unsigned int index);
void          effectEnd(void* effect);
// [RENDER] Effect.cpp: the effect's passes draw through the submission layer from now on
// (Submit.h, render milestone 9): terrain and undergrowth.
void          effectEnableSubmit(void* effect);
// Effect.cpp - sub_1001DC20: a cached technique record with a live D3DX handle exists for `name`.
bool          effectHasTechnique(void* effect, const std::string& name);
const std::string& effectCurrentTechniqueName(void* effect);   // Effect +0x38
// Effect.cpp - sub_1001DB00: the effect's cached int parameter "ShaderVersion" (0 when absent).
int*          effectShaderVersion(void* effect);

// StateManager.cpp - narrow, typed forms of the ordinary deferred-state slots used by the
// LightManager shadow pass.  These are deliberately distinct from
// stateManagerSetRenderStatePersistent(): PE 0x101679A0/0x10160D50 dispatch through the normal
// +0x1C setter and let the next flush issue the state.

void stateManagerPush();
void stateManagerPop();
void stateManagerSetRenderState(unsigned int state, unsigned int value);
// sub_10025BA0 - the persistent set: device mirror AND base value, issued at once (StateManager.cpp).
void stateManagerSetRenderStatePersistent(unsigned int state, unsigned int value);
// sub_10026880 - the pre-draw apply, published for the Ra shader's draw (RaShader::draw calls it
// on the state manager before the device draw).  StateManager.cpp.
void stateManagerPreDrawApply();
// The indexed-draw statistics bump of the DrawIndexedPrimitive wrapper (StateManager.cpp), shared
// with RaShader::draw, whose retail draw counted nothing.
void rendererCountIndexedDraw(unsigned int primCount);
// sub_10022420 - ShaderManager +0xA0, the active Ra effect the pre-draw apply commits first.
// RaShader::begin installs it, RaShader::end clears it.  ShaderManager.cpp.
void shaderManagerSetActiveRaEffect(void* effect);
void stateManagerSetTransparencyAA(bool enabled);
void stateManagerSetRenderTarget(unsigned int index, IDirect3DSurface9* surface);
bool rendererSetRenderTarget(unsigned int index, IDirect3DSurface9* surface);   // PE 0x10005770 (RendDX9Draw.cpp)
// PE 0x10005790 -> StateManager 0x10025A70: the desired render target at `index` (no AddRef).
// Only index 0 has storage; every shipped reader passes 0.
IDirect3DSurface9* stateManagerGetRenderTarget(unsigned int index);
// StateManager +0x04, its DeviceLostResetListener subobject (dword_1023B984 + 4 on x86).
DeviceLostResetListener* stateManagerDeviceLostListener();
void stateManagerSetDepthStencilSurface(IDirect3DSurface9* surface);
IDirect3DSurface9* stateManagerGetDepthStencilSurface();   // [RENDER] no AddRef
// [RENDER] Reversed depth, see StateManager.cpp.
bool stateManagerReversedDepthBound();
void stateManagerMarkStandardDepth(bool standard);
float stateManagerClearDepth(float depth);
unsigned int stateManagerMapDepthState(unsigned int state, unsigned int value);
// [RENDER] While set, ZFUNC is issued as EQUAL and ZWRITEENABLE as FALSE: the draw's depth is
// already in the scene depth from the depth prepass (milestone 8). StateManager.cpp.
void stateManagerSetPrepassedDraw(bool prepassed);
// [RENDER] The value the next draw will issue for render state `state` (the wanted cell).
unsigned int stateManagerWantedRenderState(unsigned int state);
void stateManagerSetIndices(IDirect3DIndexBuffer9* indices);
void stateManagerSetStreamSource(unsigned int stream, IDirect3DVertexBuffer9* vertexBuffer,
                                 unsigned int offset, unsigned int stride);
void stateManagerSetVertexDeclaration(IDirect3DVertexDeclaration9* declaration);
void* stateManagerVertexDeclaration();   // [RENDER] the declaration wanted for the next draw
// the +0x34 SetVertexShader (0x10025BF0) and +0x44 SetPixelShader (0x10025C20)
// slots as owner-TU seams, like the three dice binding slots above. Their one new caller is the
// display-mode render probe sub_10009FF0 (RendDX9Init.cpp), which binds the shaders it assembles.
void stateManagerSetVertexShader(IDirect3DVertexShader9* shader);
void stateManagerSetPixelShader(IDirect3DPixelShader9* shader);
// [RENDER] The submission layer's binds, through the same deferred mirror (StateManager.cpp).
void stateManagerSetTexture(unsigned int stage, IDirect3DBaseTexture9* texture);
void stateManagerSetSamplerState(unsigned int stage, unsigned int type, unsigned int value);
void stateManagerSetVertexShaderConstantF(unsigned int reg, const float* data, unsigned int count);
void stateManagerSetPixelShaderConstantF(unsigned int reg, const float* data, unsigned int count);
void stateManagerSetShaderConstantI(bool pixel, unsigned int reg, const int* data,
                                    unsigned int count);
void stateManagerSetShaderConstantB(bool pixel, unsigned int reg, const BOOL* data,
                                    unsigned int count);
void stateManagerClear(unsigned int flags, unsigned int colour, float depth,
                       unsigned int stencil, unsigned int rectCount = 0, const D3DRECT* rects = 0);
void flushRenderTargetsSeam();
// StateManager.cpp - dword_1023D08C and the two 4-byte codes at the
// transparency-antialiasing render-state index and its on / off values. ONE set of globals with
// four readers in the image - the StateManager's issuer and reset (0x10025FBB, 0x10027618),
// renderFrame's atoc brackets (0x1003027D..0x10030388), ShaderManager::updateShaderDefines' "%X"
// pair (0x10023BE7 / 0x10023C3C) and the Ra EffectManager's A2MEnable / A2MDisable defines
// (0x100333A5 / 0x100333CF) - and ONE writer, the transparency-AA probe sub_1000A3E0
// (RendDX9Init.cpp). .bss, so all three read zero until that probe runs.
extern unsigned int  g_transparencyAAState;
extern unsigned char g_transparencyAAOn[4];
extern unsigned char g_transparencyAAOff[4];

// BF2Render.cpp, typed seams for Renderer methods owned by that TU.
class IRenderView;
IRenderView* rendererGetRenderView();
IRenderView* rendererGetRenderViewDup();
IVideoOptions* rendererGetVideoOptions();
// [CHOSEN, not recovered] Fields of the Renderer's unrecovered tail that other subsystems read and
// write, named through BF2Render.cpp rather than by raw byte offset (issue #7). The x86 Renderer
// keeps the retail layout, but the x64 one is native, so a raw `g_Renderer + offset` is wrong there.
// Each accessor names its field by the retail x86 offset and maps it onto the named member; an
// offset (or a value type) that names no mapped field stops with a diagnostic instead
// of reading a guess. Each requires the published Renderer (g_Renderer == g_rendererBase).
//
// The per-pass render switch byte at retail `retailOffset` (+0x69C..+0x6AB and +0x6B0..+0x6B7;
// +0x69D the world/object pass, +0x6A4 draws roads, +0x6B1 is renderer.drawHud).
unsigned char&     rendererRenderSwitch(unsigned int retailOffset);
// Any mapped byte, 32-bit integer, float or double field of the tail, by retail offset. The float
// tables (+0x70C/+0x71C/+0x72C level-of-detail distances, the FPS histogram) are addressed per
// element: `rendererTailFloat(0x72C + 4 * i)`.
unsigned char&     rendererTailByte(unsigned int retailOffset);
int&               rendererTailInt(unsigned int retailOffset);
float&             rendererTailFloat(unsigned int retailOffset);
double&            rendererTailDouble(unsigned int retailOffset);
unsigned int       rendererRaPath();                    // +0x6CC, the RAPATH shader define
void*              rendererFrameTimer();                // +0x5DC, the 48-byte QPC frame timer
double             rendererPostTime();                  // PE 0x100286E0, the frame timer's seconds
IDirect3DSurface9* rendererBackBufferSurface();         // +0x76C, the device back buffer reference
// [RENDER] HDR scene colour. The surface the scene draws into where retail draws into the back
// buffer: the multisampled float scene surface between rendererHdrBeginScene and
// rendererHdrEndScene, else the back buffer.
IDirect3DSurface9* rendererSceneColorSurface();
void rendererHdrBeginScene();
void rendererHdrEndScene();
void rendererHdrReleaseSurface();
// [RENDER] Depth prepass: before the world queues are
// drawn, every opaque Ra entry whose effect has the technique "RaDepthPrepass" draws into the
// scene depth, and with terrain (whose own first pass already wrote the scene depth) its linear
// view depth into an R32F target of the scene's sample count (0 where nothing was drawn), resolved
// into a single-sample texture that stays bound to sampler s15 for the rest of the frame. The
// world pass then draws each prepassed entry with depth EQUAL and no depth write
// (renderQueuesIsPrepassed, stateManagerSetPrepassedDraw), so it shades each sample once. Before the prepass the sun cascades
// render (s13) and their constants are published (rendererSetSunCascadeConstants); the lit shaders
// sample the cascades themselves. rendererDepthPrepassEndWorldPass zeroes the constants, so the
// first-person passes and every other view keep their baked sunlight.
// Samplers s9, s13 and s15 are reserved for these renderer frame resources (s9 the aerial-perspective
// atlas, rendererAtmosphereFrame): no effect may use them for anything else.
void rendererDepthPrepass(bool drawTerrain, float time);
void rendererDepthPrepassEndWorldPass();
void rendererDepthPrepassRelease();
IDirect3DTexture9* rendererLinearDepthTexture();
// [RENDER] Transparent depth rebuild (milestone 8). The 4x scene holds the opaque world, the sky,
// decals, the particles drawn before water, water and the first-person passes; water
// (rendererLinearDepthWater) and the first-person opaque queues (rendererFirstPersonPrepass, a
// prepass of their own after their depth clear) add their linear depth to the prepass's.
// rendererSceneResolveForTransparents then resolves the scene colour and the linear depth, and
// rebuilds a single-sample depth buffer from the linear depth for the world transparents
// (rendererSceneTransparentsEnd ends that phase). Without multisampling, or without the float
// scene, the rebuild overwrites the scene depth in place.
void rendererLinearDepthWater();
// [RENDER] Milestone 11 task 12, water's screen-space reflections (contract 12). Before the water
// queue draws: while the camera is above water, this frame has the
// prepass's linear depth and the queue has entries, the scene colour is copied into a
// single-sample texture of its size and format, bound to the reserved sampler kWaterSceneSampler
// (bilinear, clamp), and RaWaterSsr (kWaterSsrRegister) is set to (1, 1 / width, 1 / height, 0);
// otherwise, and after the water (rendererWaterSceneEnd), RaWaterSsr is zero. The copy is
// released with the depth prepass's targets.
const unsigned int kWaterSceneSampler = 12;
// [RENDER] Render milestone 16 task 2, water's waves (Water.cpp, contract 17): an FFT ocean of three
// cascades, drawn each frame before the depth prepass (rendererWaterFrame) into three 256 x 256
// A16B16G16R16F slope maps with mips (slope x, slope z and their squares). rendererWaterBindWaves
// binds them to kWaterWaveSampler .. + 2 (16x anisotropic, wrap) for the water queue and returns
// true, then RaWaterSsr.w is 1 (0 before the first frame builds them: a flat plane);
// renderer.waterWind (m/s) and renderer.waterFetch (km) set the sea, the level's WaterScroll its
// direction. The targets go at a device reset.
const unsigned int kWaterWaveSampler = 6;
// [RENDER] Render milestone 16 task 5, water's foam (contract 18): the two finer cascades' foam
// maps (256 x 256 G16R16F with mips, r coverage 0-1, g the compression in ramps) on kWaterFoamSampler .. + 1 (16x anisotropic,
// wrap) and RaWaterFoam in kWaterFoamRegister (1, the depth waves break at in metres, the waves'
// loop seconds, 0) while the water queue draws with waves; zero otherwise. renderer.waterFoam
// scales the crest foam's coverage (1 Monahan's for the wind; 0 no foam maps).
const unsigned int kWaterFoamSampler = 4;
const unsigned int kWaterFoamRegister = 188;
void stateManagerSetWaterFoam(const float* value /* RaWaterFoam, one float4 */);
float& rendererWaterFoam();
void rendererWaterFrame();
bool rendererWaterBindWaves();
void rendererWaterUnbindWaves();
void rendererWaterRelease();
float& rendererWaterWind();
float& rendererWaterFetch();
void rendererWaterSceneBegin();
void rendererWaterSceneEnd();
void rendererFirstPersonPrepass();
void rendererSceneResolveForTransparents();
void rendererSceneTransparentsEnd();
bool rendererSceneResolved();          // the scene colour was resolved this frame
// RenderQueues.cpp: whether this frame's depth prepass drew `entry` into the scene depth.
struct RenderQueueEntry;
bool renderQueuesIsPrepassed(const RenderQueueEntry* entry);
void renderQueuesClearPrepassed();
// The entries of the queues at `slotByteOffsets` (renderQueueFlush offsets) with technique `key`,
// without consuming them; `prepass` as renderQueuesDepthPrepass, else no depth-write filter and
// nothing recorded.
void renderQueuesDepthPass(unsigned int key, const unsigned int* slotByteOffsets,
                           unsigned int count, bool prepass);

// [RENDER] Submission context (RenderQueues.cpp). Geometry submission culls against
// rendererSubmitCullFrustum(): the camera's frustum, or the caster volume while a shadow pass
// captures. During a capture RenderQueue::add diverts every entry bound for an opaque world queue
// into the caster list and drops all others (transparent, first-person, glow, point-light passes),
// so the meshes' own draw() submits their casters with the LOD, dynamic parts and skinning the
// camera sees. Entries keep pointing at their object's parameters, which are per frame.
struct RenderQueueEntry;
struct ShadowCaster
{
    RenderQueueEntry* entry;
    float             sphere[4];   // world centre, radius; radius < 0 = unbounded
    const void*       shader;      // the entry's shader, object block and material, copied at
    const void*       object;      // capture: the draw order's keys, compared without visiting
    const void*       material;    // the entry
};
void rendererBeginCasterCapture(const Frustum* volume);
void rendererEndCasterCapture();
const Frustum* rendererSubmitCullFrustum();
bool rendererCasterCaptureActive();   // a shadow pass is capturing (draw() submits casters)
// [RENDER] Milestone 9 contribution culling (renderer.contributionCull): the projected diameter,
// in pixels, below which a static mesh is not drawn; 0 turns the cull off.
float rendererContributionCullPixels();
void rendererSetContributionCullPixels(float pixels);
std::vector<ShadowCaster>& rendererShadowCasters();
// Gives every caster captured from index `first` on the world sphere (centre, radius).
void rendererSetCasterBounds(size_t first, const float sphere[4]);
// One cascade's caster volume: the cascade's view slice swept toward the sun. In light coordinates
// (x = right, y = up, z = forward along the sunlight) it is the 2D convex hull of the slice's eight
// corners, between the light depth the cascade's projection starts at and the slice's farthest
// corner. Each edge (nx, ny, d) has a unit outward normal: a point is outside where
// nx * x + ny * y > d.
struct ShadowCascadeCull
{
    float right[3];
    float up[3];
    float forward[3];
    float edges[8][3];
    unsigned int edgeCount;
    float minZ, maxZ;
};
// Whether a world sphere (centre, radius; radius < 0 = unbounded) can reach the cascade box.
bool rendererShadowCascadeTouches(const ShadowCascadeCull& cull, const float sphere[4]);
// Draws the captured casters whose effect has technique `key` (RaShadowDepth) into the bound
// target, with the effect parameter RaShadowViewProj (semantic) set to `viewProjection`.
void renderQueuesDrawShadowCasters(unsigned int key, const float* viewProjection,
                                   const ShadowCascadeCull& cull);

// [RENDER] The renderer contract version, passed as the define RA_CONTRACT to every effect the
// engine compiles (ShaderManager's define table and the Ra profile defines), so a shader set that
// needs a newer contract stops with #error instead of running against an engine without it. Raised
// whenever the engine changes what the shaders may rely on; docs/lanes/render.md lists
// what each version adds.
#define RA_CONTRACT_VERSION 32
#define RA_CONTRACT_TEXT_(v) #v
#define RA_CONTRACT_TEXT(v) RA_CONTRACT_TEXT_(v)

// [RENDER] Sun cascaded shadow maps (SunShadows.cpp). Run from the
// depth prepass: submits the casters in the cascades' light volume, renders four cascades into an
// R32F atlas (light depth; the atlas is bound to the reserved sampler s13), and fills the cascade
// rows of RASM. Returns false when nothing was rendered (nothing is then covered: baked sunlight).
bool rendererSunShadowsRender(float time, float* constants /* kSunCascadeVec4s float4s */);
void rendererSunShadowsRelease();
// [RENDER] Atmosphere (Atmosphere.cpp, render milestones 11 and 18). One atmosphere whose aerosol
// the weather sets; its tables are built on a worker thread when the weather or the level's light
// changes (rendererAtmosphereFrame, run from the depth prepass after the sun cascades), and the
// aerial-perspective atlas is drawn every frame through Shaders/RaAtmosphere (contracts 6, 27).
// The weather (renderer.weather*): visibility (m: a level ray at the level's ground keeps 2 %
// contrast there), aerosol type (0 continental, 1 maritime, 2 dust, 3 smoke), mixing height (m
// over the ground), the level's ground above sea level (m; kAltitudeAuto: its far-ground bake's,
// else world y 0 at sea level).
struct AtmosphereWeather
{
    float visibility;
    int aerosol;
    float mixing;
    float altitude;
};
const float kAltitudeAuto = -100000.0f;
AtmosphereWeather& rendererAtmosphereWeather();
const char* rendererAtmosphereAerosolName(int type);
// The level's weather file (Weather.cpp): the weather variables back to their defaults, then
// <mod>/settings/eon64-weather/<level>.txt's lines for the loaded layer. Run at each level load.
void rendererWeatherLoad();
// What the last load applied: the file and its lines, or why the defaults stand (tools.atmosphere).
const char* rendererWeatherSource();
// How far the land past the view distance reaches (m): terrain and its surrounding ring are culled
// against the world view's frustum taken out to here.
float rendererSceneryDistance();
// This frame's view-distance cut (m): fog end while the shaders cut there (RaAerial.w 2), else 0.
// Bundled and skinned meshes wholly past it are not drawn; static meshes go on past it, culled by
// their size on screen (renderer.contributionCull).
float rendererViewDistanceCutRange();
void rendererAtmosphereFrame();
// Draws this frame's aerial-perspective atlas (contract 27), the air's sunlight shadowed by the
// clouds (RaCloudShadow's constants, rendererCloudShadowConstants), and binds it to s9.
void rendererAtmosphereDrawAtlas(const float cloudShadow[2][4]);
// [TOOLS] A weather build, or the sky cube it asks for, is under way (view.shot waits for it).
bool rendererAtmosphereSettling();
void rendererAtmosphereRelease();
// The aerial-perspective atlas is bound to the reserved sampler s9 once drawn. RaAerial (pixel
// register kAerialRegister, contract 8) for the world pass: 1 / the viewport's width and height,
// 1 / fog end, 2 (the view-distance cut, contract 9; 1 before it). All zero (the shaders' retail
// fog) while no atlas was drawn this frame, under water, or with a viewport off the target's origin.
const unsigned int kAerialSampler = 9;
void rendererAtmosphereAerialConstants(float c[4]);
// RaSunFactor (pixel register kSunFactorRegister, contract 26): the surfaces' sun over the
// authored one, rgb, w 1 once built (zero before: the shaders take 1). Published in every pass.
void rendererAtmosphereSunFactor(float c[4]);
// Whether the model draws the sky (the fit done and the sky-view table built). The sky is one
// full-screen pass (rendererAtmosphereDrawSky); before the first fit nothing draws there.
bool rendererAtmosphereDrawsSky();
// Draws the model's sky with the sky dome's effect (contract 7, technique RaSky) into the bound
// target, at the far plane; under water, the water's fog colour. False, drawing nothing, while the
// model does not draw the sky.
bool rendererAtmosphereDrawSky(void* effect, bool underWater);
// The fit and the tables, for the debug menu.
struct AtmosphereInfo
{
    bool fitted;
    bool atlasVertexFetch, atlasVertexFilter, atlasFloat32;
    IDirect3DTexture9* transmittance;   // 256 x 64
    IDirect3DTexture9* multiScatter;    // 32 x 32
    IDirect3DTexture9* skyView;         // 192 x 108
    IDirect3DTexture9* cloudLight;      // 64 x 24: 8 altitudes x (sun, sky, ground) tiles of 8 x 8
    IDirect3DTexture9* perspective;     // 2080 x 32: 65 blocks of 32 x 32 tiles (contract 27)
    float fitMs, bandMs, refHeight, fogEnd;
    AtmosphereWeather weather;          // as set (the live build may lag a frame or two)
    const char* aerosolName;
    const char* altitudeSource;
    float groundAltitude, mixingTop;    // m above sea level, as built
    float seaOffset;                    // m: world y 0's height above sea level
    float skyViewAltitude;              // m above sea level: the camera's band
    float aerosolExtinction[3], aerosolScattering[3];   // 1/km in the mixing layer
    float aerosolG;
    float sunFactor[3];                 // the surfaces' sun over the authored
    float toSun[3];                     // world, unit
    float skyGain;                      // the authored horizon over the model's (not applied)
    float sunTop[3];
    float zenithModel[3], horizonTarget[3], horizonModel[3];
    float groundAlbedo[3];
};
AtmosphereInfo rendererAtmosphereInfo();
// RaAtmosphere's rows 0-10 (layout in RaAtmosphere.fx), the camera rows zero: for passes that are
// not the camera's view (the cloud panorama).
void rendererAtmosphereConstants(float c[16][4]);
// [RENDER] Sky clouds' baked volumes (Clouds.cpp, render milestone 12 task 2, contract 13): the
// weather map, noise volumes, density field, its coarse copy and the light volume, built over
// frames after the atmosphere's (rendererCloudsFrame) into a double buffer and made live whole.
// renderer.cloudCoverage (0 clear .. 1 overcast), renderer.cloudPreset (0 desert, 1 maritime) and
// renderer.cloudBase, cloudTop (the low layer above the terrain's mean, m; 0 the preset's),
// cloudMidBase, cloudMidTop (the mid layer's), cloudMidDensity, cloudMidCoverage (< 0 the
// preset's) start a new build. The render targets go at a device reset and are rebuilt. Task 3: the
// panorama (contract 14), a band a frame from the live set; renderer.clouds 0 stops it all.
// Task 4 (contract 15): the panorama is the far field from the camera, past renderer.cloudNear
// metres; rendererCloudsNear marches the nearer clouds at half the view's size and composites
// them over the bound scene (after the sky). `allowed` false (under water) skips them.
void rendererCloudsFrame();
bool& rendererCloudsEnabled();
// Sets the sky effect's RaSkyClouds and panoramas (RaCloudPanorama.fxh); y 0 without them.
void rendererCloudsBindSky(void* d3dxEffect);
void rendererCloudsRelease();
float& rendererCloudCoverage();
int& rendererCloudPreset();
float& rendererCloudBase();
float& rendererCloudTop();
float& rendererCloudMidBase();
float& rendererCloudMidTop();
float& rendererCloudMidDensity();
float& rendererCloudMidCoverage();
void rendererCloudSettingsChanged();
// [TOOLS] A volume build started by a setting (or the first) is pending or under way.
bool rendererCloudsSettling();
void rendererCloudsNear(bool allowed);
float& rendererCloudNearDistance();
struct CloudsInfo
{
    bool live;
    IDirect3DTexture9* weather;         // 128 x 128 A8R8G8B8: low, mid coverage; cloud type
    IDirect3DVolumeTexture9* shape;     // 128 x 128 x 32 L8
    IDirect3DVolumeTexture9* detail;    // 32^3 L8
    IDirect3DTexture9* field;           // 2048^2 A8R8G8B8: 4 x 4 tiles, 4 slices each in rgba
    IDirect3DTexture9* coarse;          // 128 x 64 A8R8G8B8: 2 tiles, 4 slice groups each
    IDirect3DTexture9* light;           // 2048^2 A16B16G16R16F: 8 x 8 tiles, one slice each
    const char* preset;
    float coverage, seconds, baseM, topM, lwc, dropletUm;   // baseM, topM: the slab
    float lowM[2], midM[2], midDensity, midCoverage;         // the layers (m above the reference)
    float noiseMs, weatherMs;
    unsigned int buildFrames, builds;
    IDirect3DTexture9* panorama;        // the newest complete, 2048 x 1024 A16B16G16R16F; 0 none
    unsigned int panoramas;             // drawn so far
    IDirect3DTexture9* nearResolved;    // the near clouds' last resolve, A16B16G16R16F; 0 none
    float nearDistance;                 // m, renderer.cloudNear
    IDirect3DTexture9* shadow;          // the live cloud shadow map, 1024^2 A16B16G16R16F; 0 none
    float shadowCorner[2], shadowSize;  // its corner (world x, z at the reference height), size (m)
    unsigned int shadowMaps;            // drawn so far
    unsigned int skyLights;             // ground panoramas read back for the sky light
};
CloudsInfo rendererCloudsInfo();
// [RENDER] The frame's HDR post (HdrPost.cpp, render milestone 13, contract 19): bloom, the
// luminance histogram and auto-exposure on the resolved scene, run by the post-production
// composite just before the final copy, which it hands the exposure and the bloom (RaGrade on the
// FSQuadDrawer effect), and the sRGB encode. Thermals skip it (exposure 1, no bloom, the 2.2
// encode). renderer.bloomStrength, exposureBias, exposureMin, exposureMax (stops), exposureSpeed.
// The targets go at a device reset.
void rendererHdrPost(EngineTexture* scene);
// The particle effects' RaExposureParticle and RaExposureState (their emissives undo the exposure).
void rendererHdrPostBindParticles(void* effect);
// True while the bloom stands in for retail's glow passes (everywhere but thermals).
bool rendererBloomReplacesGlow();
void rendererHdrPostRelease();
// renderer.farGround (render milestone 17, Geom/FarGround.cpp): 0 the retail ring, 1 the far ground
// where the level has a bake, 2 the far ground everywhere.
int& rendererFarGround();
// renderer.farGroundScaled: the bake's heights scaled to the map's relief (1) or real (0).
bool& rendererFarGroundScaled();
// renderer.hexTiling (render milestone 19, Geom/TerrainDraw.cpp): the terrain's detail layers hex-tiled.
bool& rendererHexTiling();
float& rendererBloomStrength();
float& rendererExposureBias();
float& rendererExposureMin();
float& rendererExposureMax();
float& rendererExposureSpeed();
struct HdrPostInfo
{
    bool ran, thermal, lastSnap;
    float bloomStrength, exposureBias, exposureMin, exposureMax, exposureSpeed;
    float key, evLow, evRange, lowPercentile, highPercentile, rateLight, rateDark;
    unsigned int frames, sceneW, sceneH, levels;
    float lastDt;
    IDirect3DTexture9* state;       // 1 x 1 A32B32G32R32F: adapted EV, exposure, metered EV, 1; 0 none
    IDirect3DTexture9* histogram;   // 16 x 1 A32B32G32R32F, four bins to a texel
    IDirect3DTexture9* meter;       // 64 x 32 A16B16G16R16F: log2 luminance, centre weight
    IDirect3DTexture9* bloomTop;    // the bloom's level 0, half the scene
};
HdrPostInfo rendererHdrPostInfo();
// [RENDER] Milestone 12 task 8 (contract 16): the cloud shadow map (Clouds.cpp), a Beer shadow map
// over 32 km of the plane at the reference height around the camera, redrawn a band a frame about
// once a second; its receiver constants RaCloudShadow (2 float4s, RaSunShadow.fxh) go to the
// engine-owned pixel registers kCloudShadowRegister.. with RASM, and the map to the reserved
// sampler kCloudShadowSampler (bilinear, clamp). All zero (transmittance 1) without clouds.
const unsigned int kCloudShadowSampler = 14;
const unsigned int kCloudShadowVec4s = 2;
void rendererCloudShadowConstants(float c[2][4]);
IDirect3DTexture9* rendererCloudShadowMap();   // the live map, 0 none
// [RENDER] Milestone 12 task 7 (contract 16): a ground panorama of the clouds (256 x 128, rgb the
// light they add, a the transmittance, decoded), drawn from the level's origin and read back about
// every 30 s and at a change of settings, hands the clouded sky to the atmosphere, which rebuilds
// the sky cube and its irradiance from it and lights the air under the clouds (RaAtmosphere[8]).
// Null: clear.
void rendererAtmosphereCloudSky(const float* rgba, unsigned int w, unsigned int h);
// [RENDER] Milestone 11 task 7 (SkyCube.cpp, contract 10): the sky cube (prefiltered GGX, mip m
// roughness m / 5), the sky's irradiance as SH9 and the split-sum BRDF table, built on the CPU
// with the sky view into managed textures. Each frame the cube is bound to the reserved sampler
// kSkyCubeSampler (trilinear, clamp), the table to kSkyBrdfSampler (bilinear, clamp), and the
// irradiance to the pixel registers kSkyShRegister.. (RaSkySH, 7 float4s; [6].w 1 while live).
const unsigned int kSkyCubeSampler = 10;
const unsigned int kSkyBrdfSampler = 11;
struct SkyCubeSource
{
    const float* skyView;          // rgb per texel, the sky-view table's layout and scale
    unsigned int skyViewW, skyViewH;
    double groundRadiusKm, altitudeKm;   // the radius the sky view is seen from, as the table
    float toSun[3];                // world, unit
    float sun[3];                  // the surfaces' sun (irradiance / pi)
    float groundAlbedo[3];         // linear
    double groundExtinction;       // 1/m: the air's at the ground (550 nm)
    // [RENDER] Milestone 12 task 7: the clouds from the ground (rendererAtmosphereCloudSky's
    // panorama, RaCloudPanorama.fxh's mapping), composited over the sky; null for a clear sky.
    const float* clouds;           // rgba per texel: rgb the light added, a the transmittance
    unsigned int cloudsW, cloudsH;
};
// Built on a worker thread from a copy of `source`; the previous cube and irradiance stay bound
// until the new ones are uploaded (rendererSkyCubeFrame). A request during a build is kept and
// started when it ends.
void rendererSkyCubeBuild(const SkyCubeSource& source);
// The clouded sky's statistics from the last build: the clouds' mean transmittance S toward the
// sun's elevation (1 clear) and the light they give the air under them, (1 - S) times the clouded
// sky's mean radiance (its cosine-weighted mean over the upper hemisphere; 0 clear).
struct SkyCubeCloudLight
{
    float sunTransmittance;
    float hazeLight[3];
};
SkyCubeCloudLight rendererSkyCubeCloudLight();
// Binds the cube, the table and RaSkySH; with `use` false (or before a build) unbinds them and
// zeroes RaSkySH.
void rendererSkyCubeFrame(bool use);
// This frame's RaSkySH (kSkyShVec4s float4s; zeros while not live), for effects that read it in a
// vertex shader as a parameter (RaSkyShVertex: particles, rendererBindParticleLighting).
const float* rendererSkyShFrame();
struct SkyCubeInfo
{
    bool live;
    IDirect3DCubeTexture9* cube;
    IDirect3DTexture9* brdf;
    unsigned int cubeSize, cubeMips, brdfSize;
    float buildMs;
    float sh[7][4];
    float irradianceUp[3];
};
SkyCubeInfo rendererSkyCubeInfo();
// [TOOLS] A build is running or requested (view.shot waits for it).
bool rendererSkyCubeSettling();
// RASM, the sun cascade constants: 22 float4s, laid out in RaSunShadow.fxh. Pixel shaders read them
// from the fixed registers c200..c221, which the engine owns: it writes them once per frame
// (rendererSetSunCascadeConstants) and the StateManager drops every effect's write into the range,
// so no effect uploads them per draw.
const unsigned int kSunCascadeVec4s = 22;
const unsigned int kSunCascadeRegister = 200;
// Writes RASM to pixel registers kSunCascadeRegister.. through the StateManager's mirror.
void stateManagerSetSunCascadeConstants(const float* constants);
// [RENDER] Milestone 8 LOD crossfade: RaLodDither, one float4 in pixel register c222, engine-owned like
// RASM. While a static mesh is in the band around a LOD switch it submits both LODs, and each
// entry's lodDither (a, b) keeps the pixels where a * t + b >= 0 for the screen's 4x4 Bayer
// threshold t, so the two LODs cover complementary pixels. (0, 1) keeps every pixel. The queues
// write it before every entry draw (renderEntryBindLodDither).
const unsigned int kLodDitherRegister = 222;
// [RENDER] Milestone 11 aerial perspective: RaAerial, one float4 in pixel register c223 (the last
// ps_3_0 register), engine-owned like RASM and published with it (rendererAtmosphereAerialConstants).
const unsigned int kAerialRegister = 223;
// [RENDER] Milestone 18 (contract 26): RaSunFactor in c199, the surfaces' sun through this
// weather's air over the authored sun (rendererAtmosphereSunFactor), in every pass.
const unsigned int kSunFactorRegister = 199;
// [RENDER] Milestone 11 sky cube (contract 10): RaSkySH, the sky's irradiance as 7 float4s in
// c192..c198 (SkyCube.cpp), written each frame by rendererSkyCubeFrame.
const unsigned int kSkyShRegister = 192;
const unsigned int kSkyShVec4s = 7;
// [RENDER] Milestone 11 water reflections (contract 12): RaWaterSsr in c191 (rendererWaterSceneBegin).
const unsigned int kWaterSsrRegister = 191;
// [RENDER] Milestone 12 cloud shadows (contract 16): RaCloudShadow in c189..c190, published with RASM.
const unsigned int kCloudShadowRegister = 189;
const unsigned int kEngineRegisterBegin = 188;
const unsigned int kEngineRegisterEnd = 224;   // c189..c223 dropped from effect writes
void stateManagerSetLodDither(float a, float b);
void stateManagerSetAerial(const float* value /* RaAerial */);
void stateManagerSetSunFactor(const float* value /* RaSunFactor */);
void stateManagerSetSkySh(const float* value /* RaSkySH, kSkyShVec4s float4s */);
void stateManagerSetWaterSsr(const float* value /* RaWaterSsr, one float4 */);
void stateManagerSetCloudShadow(const float* value /* RaCloudShadow, kCloudShadowVec4s float4s */);
struct RenderQueueEntry;
void renderEntryBindLodDither(const RenderQueueEntry* entry);
// The sun shadow tuning, read each frame: the renderer.sunShadow* console variables
// (RendererConsole.cpp), which clamp to their ranges. A new resolution re-creates the atlas.
struct SunShadowSettings
{
    bool enabled;                 // off: no cascades drawn, receivers use the baked sun
    float distance;               // m: the view depth the cascades reach
    unsigned int resolution;      // texels per cascade side, a power of two
    float casterReach;            // m: how far toward the sun past a cascade casters still draw
    float splitLambda;            // 0 uniform .. 1 logarithmic splits
    float cascadeBlend;           // the part of each next slice a cascade also covers and blends over
    float normalOffset;           // cascade texels: receivers move along their normal
    float depthBias;              // cascade texels: receivers move toward the sun in depth
};
SunShadowSettings& sunShadowSettings();
unsigned int sunShadowCascadeDraws(unsigned int cascade);   // draw calls, last frame rendered
// The help line of a fork console variable (renderer.<name>), or 0. For the debug menu; retail
// console objects carry no help.
const char* rendererVariableHelp(const char* name);
// [RENDER] Sets the particle lighting parameters on a particle effect (Particles, Trail,
// NonScreenAlignedParticles, MeshParticleMesh) by semantic: RaSunColor and RaSkyColor (the level's
// one sun and sky, Lightmanager.staticSunColor / staticSkyColor as authored), RaSunDirection
// (unit, toward the sun), RASM and RaSunCascadeAtlas. Particles sample the cascades in the vertex
// shader, where neither the fixed pixel registers nor s13 reach, so RASM is an effect parameter here
// (zero outside the main world pass) and the atlas a texture parameter.
// Also RaSoftParticles (soft particles): .x 1 / softParticleDistance while the main world pass
// draws over this frame's prepass depth (s15), else 0 (no fade); .zw 1 / the depth target's size.
void rendererBindParticleLighting(void* effect);
// m: the depth over which a particle fades out in front of the scene (renderer.softParticleDistance).
float& softParticleDistance();
// The sun cascade atlas (R32F, 2 x 2 cascades), or 0 before the first cascade pass.
IDirect3DTexture9* rendererSunShadowAtlas();
IDirect3DSurface9* rendererDepthStencilSurface();       // +0x770, the depth-stencil reference
// The render-path target texture at retail `retailOffset` (+0x778..+0x7D4).
EngineTexture*     rendererPathTarget(unsigned int retailOffset);
// The Renderer's RenderPathListener subobject (retail +0x540).
RenderPathListener* rendererRenderPathListener();

// PE HemiMapManager::makeHemiMap collaborators.
IRenderView* rendererGetHemiPrimaryView();
IRenderView* rendererGetHemiSecondaryView();
void rendererInstallHemiView(IRenderView* view);
void rendererRestoreHemiViews(IRenderView* primary, IRenderView* secondary);
void rendererSetHemiTarget(EngineTexture* texture);
EngineTexture* rendererGetHemiScratchTexture();
void rendererSetHemiScratchTexture(EngineTexture* texture);
int rendererGetHemiRenderMode();
void rendererSetHemiRenderMode(int mode);
void rendererSetHemiRoadPass(bool enabled);
void rendererRenderHemiFrame();
void rendererPresentHemiResult();
void rendererCaptureHemiState(HemiRenderState& state);
void rendererApplyHemiState();
void rendererRestoreHemiState(const HemiRenderState& state);
void postProductionGetHemiState(float outState[4]);
void postProductionSetHemiState(const float state[4]);

// Ra/EffectManager.cpp and RenderQueues.cpp.  makeHemiMap changes the effect channel between its
// terrain and static-geometry renders, then invalidates every queue effect exactly as retail does.
void effectManagerSetShowChannel(unsigned int channel);
// Rebuilds effect-manager shader defines after resolution changes.
void effectManagerRebuildShaderDefines();


// [CHOSEN, not recovered] Reapplies the current display mode without copying its native
// carrier onto the incompatible internal layout. Returns the reset/loss-loop result.
bool  rendererApplyCurrentDisplayMode(void* renderer);
// Draws a probe triangle and scans rect (or the full surface) for a nonblack RGB value
// outside [colourLow, colourHigh]. Used by multisample and transparency-AA probes.
bool  rendererRenderProbe(void* renderer, const char* vertexShader, const char* pixelShader,
                          unsigned int colourLow, unsigned int colourHigh, const float* rect,
                          const char* dumpFile);
void  rendererProbeMultisample(void* renderer);
void  rendererProbeTransparencyAA(void* renderer);

// The metaFx cache, It hangs off the Ra EffectManager
// (`dword_1023D954`) at +0x54, NOT off the ShaderManager - both shipped call sites, 0x10115D08 and
// 0x10192204, load `dword_1023D954` straight into ecx in the instruction before the call. Neither
// guards the result, and neither does this seam.
namespace Ra { class MetaFx; }
Ra::MetaFx* effectManagerGetMetaFx(const std::string& name);

void renderQueuesResetEffects();
void renderQueuesFlushWorldPass();
bool  readBoolSetting(const char* name, bool defaultValue);
// Settings vslot +0x14 - the setter twin of readIntSetting's +0x10 getter.
void  writeIntSetting(const char* name, int value);
bool  readBoolSetting(const char* name, bool defaultValue);
void  buildPresentParameters(void* renderer);
bool  createPrimaryD3DDevice(void* renderer);
bool  createD3DDeviceAndSwapChains(void* renderer);
bool  createDeviceCaps(void* renderer);
bool  createD3DDevice(void* renderer);
bool  initializeD3DSubsystems(void* renderer);

} // namespace dx9rend
} // namespace hfe
} // namespace dice

#endif // BF2_RENDDX9_RENDDX9BASE_H
