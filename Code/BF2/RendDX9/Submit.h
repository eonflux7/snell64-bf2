// [RENDER] Draw submission (render milestone 9, docs/lanes/render.md). Not part of retail BF2.
//
// A program is one pass of one technique of a Ra effect, taken from the effect file once: its
// vertex and pixel shaders, its fixed render and sampler states, and where each effect parameter
// it reads lives in the shaders' registers and samplers. A draw then binds the program's states
// once per run of draws and per draw uploads only the constants the program reads, straight into
// the state manager's mirror. D3DX is not called while drawing.
//
// Parameter values still come from the Ra parameter holders: each holder applies its value to a
// sink with the ID3DXEffect shape, which turns it into register writes. So every holder kind
// works unchanged, and the effect files stay the one source of shaders and constant layout.
//
// The contract (renderer-pipeline.md, shader contract version 2): the passes drawn here set only
// constant render states, except the three a material decides, which the engine sets per draw from
// the parameters with semantic RaAlphaTest, RaAlphaRef and RaDepthWrite.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dice {
namespace hfe {
namespace dx9rend {

namespace Ra { class RaShader; }
struct RenderQueueEntry;
struct SubmitProgram;

// The program for `shader`'s technique `key`, built on first use; null when the effect has no
// such technique or it cannot be drawn here: more than one pass, or fixed-function state (logged
// once). Key 0, or with `orDefault` a key the effect lacks, takes the effect's default technique,
// as RaShader::begin does for the queue walks.
SubmitProgram* submitProgram(Ra::RaShader* shader, unsigned int key, bool orDefault = false);

// Binds `program`'s shaders and fixed states inside a state-manager bracket and uploads the
// effect-wide parameters for `entry` (the global set). submitEnd closes the bracket.
void submitBegin(SubmitProgram* program, RenderQueueEntry* entry);
void submitEnd(SubmitProgram* program);

// Sets the parameter with `semantic` (a float4x4, row-major) for the draws that follow; false when
// the program has no such parameter.
bool submitSetMatrix(SubmitProgram* program, const char* semantic, const float* matrix);
bool submitHasParameter(SubmitProgram* program, const char* semantic);

// Uploads `entry`'s material and object parameters and sets its material's states. Returns
// whether the draw writes depth (with the depth test on), for the prepass's filter.
bool submitEntry(SubmitProgram* program, RenderQueueEntry* entry);

// Drops every program (effects reset or reloaded).
void submitReset();

// Instancing (shader contract 3): a program whose vertex shader reads the instance stream draws
// every entry as an instance, in groups of entries that share the material (or a material run),
// LOD dither and every instance parameter the program reads; a group of one included.
bool submitInstanced(const SubmitProgram* program);
// A group: begun at an entry submitEntry has uploaded, joined by later entries that can share its
// draw (each records its instance), bound before the one draw (the instance records on the stream after the material's last,
// the bound declaration merged with the instance elements, the stream frequencies) and ended after
// it. Bind fails (nothing to draw) when the declaration or the record buffer cannot be made.
// A 64-bit hash of what decides whether two entries can share a group (their instance parameters
// and LOD dither). Entries are joined to a group on equal hashes: submitGroupJoin checks only the
// dither, clip planes and the group's size.
std::uint64_t submitInstanceKeyHash(SubmitProgram* program, RenderQueueEntry* entry);
void submitGroupBegin(SubmitProgram* program, RenderQueueEntry* entry);
bool submitGroupJoin(RenderQueueEntry* entry);
unsigned int submitGroupSize();
bool submitGroupBind();
void submitGroupEnd();
// The record buffer (D3DPOOL_DEFAULT) before a device reset.
void submitInstancesRelease();

// Effect programs: the passes of an engine Effect (Effect.cpp) that opted in (effectEnableSubmit),
// drawn the same way. Terrain and undergrowth.
//
// While opted in, the effect's parameter writes go to a value store (submitEffectParameterTarget
// answers the sink in place of the D3DX effect), uploaded straight into the registers of the
// active program; the store is written to D3DX only before a pass that still draws through D3DX
// (a pass refused here), during which writes also go to D3DX.
struct SubmitEffect;
SubmitEffect* submitEffectCreate(void* d3dxEffect, const std::string& name);
void submitEffectDestroy(SubmitEffect* effect);
// The store into D3DX (before it is read or let go).
void submitEffectFlush(SubmitEffect* effect);
// Flushes, then drops the store and the programs and takes `d3dxEffect` (a rebound effect, or the
// same one before a device loss, so the store holds no texture through the reset).
void submitEffectRebind(SubmitEffect* effect, void* d3dxEffect);
// Binds the program of `technique`'s pass `pass` (a D3DX technique handle) and uploads the
// store's values into it; false when the pass cannot be drawn here.
bool submitEffectBeginPass(SubmitEffect* effect, void* technique, unsigned int pass);
// The caller begins a D3DX pass instead: the store is flushed and writes go to D3DX too.
void submitEffectBeginD3dxPass(SubmitEffect* effect);
void submitEffectEndPass(SubmitEffect* effect);
// What a parameter write of `d3dxParameter` goes to: the sink (an ID3DXEffect), or the D3DX effect
// for a parameter the store does not keep.
void* submitEffectParameterTarget(SubmitEffect* effect, void* d3dxParameter);

} // namespace dx9rend
} // namespace hfe
} // namespace dice
