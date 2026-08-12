# C# Game of Life on the GPU — feasibility trace

Goal: from a C# script, drive a compute shader that runs a Game-of-Life simulation into a texture, and get
that texture on screen (viewport / swapchain / RmlUi / material). This is a trace of what already exists,
what each route costs, and what is missing. Nothing here is implemented yet.

---

## 1. What already exists

### 1.1 The C# RHI surface

`Engine/Source/LuminaSharp/Renderer/` binds the engine RHI 1:1 (`RHI.cs`, `RHICommands.cs`,
`RHIStructs.cs`, `RHIEnums.cs`, `RHIHandles.cs`); native thunks in
`Engine/Source/Runtime/Source/Scripting/DotNet/DotNetRHI.cpp`. Everything the simulation needs is there:

| Need | C# call |
| --- | --- |
| Create the grid texture | `RHI.CreateTexture(FTextureDesc.Texture2D(W, H, EFormat.RGBA8_UNORM, Sampled\|Storage\|TransferSrc))` |
| Bindless UAV slot (compute writes) | `RHI.HeapWriteRWTexture(RHICore.GetGlobalHeap(), Tex)` |
| Bindless SRV slot (sampling / viewport) | `RHI.HeapWriteTexture(RHICore.GetGlobalHeap(), Tex)` |
| Compute pipeline from a named shader | `RHICore.CreateComputePipeline("GameOfLife.slang")` |
| Push-constant args (BDA) | `RHICore.AllocTransient(size)` → write to `.Cpu`, pass `.Gpu` |
| Record + dispatch | `RHI.OpenCommandList()` / `CmdSetTextureHeap` / `CmdSetPipeline` / `CmdDispatch(cl, args.Gpu, gx, gy, 1)` |
| Make writes visible | `RHI.Barriers.ComputeToAll(cl)` |
| Submit | `RHI.Submit(cl)` (+ `CreateSemaphore`/`WaitSemaphore` only if the CPU needs the result) |
| Seed / readback | `CmdCopyMemoryToTexture`, `CmdCopyTextureToMemory`, `CmdClearTexture` |

Deliberately **not** exposed (engine-owned, per the earlier RHI trim): device create/destroy, `TickFrame`,
`WaitDeviceIdle`, and the entire swapchain/present block. So a script can never touch the backbuffer
directly — handing the engine a finished texture is the sanctioned path.

### 1.2 The C# render-scene override — this is the big one

`Engine/Source/LuminaSharp/Renderer/RenderScene.cs` + `Runtime/Scripting/DotNet/ManagedRenderScene.cpp`:

- Declare one non-abstract `RenderScene` subclass in scripts. `ManagedRenderScenes::PostScriptLoad`
  installs `RenderSceneFactory::SetOverride(&CreateForWorld)`, so **every `EWorldType::Game` world** builds
  an `FManagedRenderScene` instead of `FDefaultSceneRenderer` (editor/preview/thumbnail worlds keep the
  engine renderer — `ManagedRenderScene.cpp:22-31`).
- Hooks: `OnInit` (game thread, world available) → `OnExtract(in SceneView)` per frame (game thread, camera
  snapshot) → `OnRender(int FrameIndex)` from `FWorldManager::RenderWorlds`, inside
  `FRenderManager::FrameEnd`, **before** the swapchain is acquired → `DisplayTexture` / `DisplayResourceID`
  / `RenderExtent` queried by whatever presents it. `OnShutdown` frees GPU resources; script hot reload
  tears every managed scene down and rebuilds it (`PreScriptUnload` / `PostScriptLoad`).
- Display, already wired both ways:
  - **Packaged**: `RenderManager.cpp:248` blits `Scene->GetDisplayTexture()` into the swapchain image.
  - **Editor**: `EditorUI.cpp:2537 / 2567` samples `SceneRenderer->GetDisplayResourceID()` bindlessly as the
    viewport image. PIE rebinds the tool to the PIE world (`WorldEditorTool.cpp:4094`
    `RebindToWorld(PIEWorld)`), and PIE worlds are `EWorldType::Game` — so **Play In Editor shows the C#
    renderer's output in the normal viewport**.

Status: fully implemented and compiles, but there is **no sample script and no docs page** — as far as the
repo shows, nobody has ever run a managed render scene. We would be the first user.

### 1.3 Shader conventions

- Push constants are a single BDA pointer: `[[vk::push_constant]] FRHIRoot gRHI; GetArgs<T>()`
  (`Includes/GlobalRHI.slang`). Args are scalar-layout (`-fvk-use-scalar-layout`) — keep push-constant
  structs to scalars/uint2 like `TexturePaint.slang` does, no `float4`.
- Bindless storage images: `#include "Includes/GlobalRHIStorage.slang"` → `gRWTextures2D[slot][px]`.
  Sampling: `gTextures2D[slot]` + `gSamplers[SAMPLER_POINT_CLAMP]`.
- `TexturePaint.slang` is the exact template for the shader we want to write.
- Named shaders resolve through `FShaderLibrary::Get`, which compiles
  `Paths::GetEngineShadersDirectory() + "/" + Name` on demand (`ShaderLibrary.cpp:183`). Dropping a new
  `.slang` into `Engine/Resources/Shaders/` is all that's needed — `Shaders.Build.cs` compiles nothing and
  has no manifest.
- Image layouts: with `VK_KHR_unified_image_layouts` every image lives in `GENERAL`, so there are no layout
  transitions — but barriers are still required for *visibility*. Don't skip `Barriers.ComputeToAll`.

### 1.4 Render-target assets and how they reach the screen

`CTextureRenderTarget : CTexture` (`Assets/AssetTypes/Textures/TextureRenderTarget.h`) — an asset you create
from the content browser. `BuildResource` allocates a `bStorage = true` image, so it has both a bindless SRV
(`CTexture::GetResourceID()`) and a lazily created mip-0 UAV slot (`RHI::Textures::StorageSlot`).

Display paths that already work with **zero new code** once the pixels are there:

- RmlUi: `<img src="/Game/MyRT">` → `FRmlUiRenderer::LoadTexture` → `LoadTextureAsset`
  (`RmlUiRenderer.cpp:641-679`) resolves the asset and samples its bindless slot every frame. Live compute
  writes appear immediately; the handle is cached by string, so no per-frame churn.
- Any material slot: it *is* a `CTexture`, so assign it and sample it on a mesh, or in a UI-domain material
  brush.

Precedent for driving it: `CWorld::PaintRenderTarget` (`World.cpp:215`) enqueues an `FTexturePaintOp`
(target image + UAV slot + params); `FForwardRenderScene::Extract` drains it; `TexturePaintPass`
(`DefaultSceneRenderer.cpp:5727`) replays them as compute dispatches inside the renderer's frame, before the
passes that sample the result. This is exactly the shape of "script-requested compute into a texture asset",
except it is currently C++-only (it was Lua-exposed; Lua is gone).

---

## 2. The three routes

### Route A — C# `RenderScene`: the world *is* the Game of Life  ★ recommended first

The whole world renders as the simulation. Nothing in the engine changes; only a new `.slang` file plus a
script.

```
OnInit:
  TexA/TexB = RHI.CreateTexture(Texture2D(W, H, RGBA8_UNORM, Sampled|Storage|TransferSrc))
  uavA/uavB = RHI.HeapWriteRWTexture(heap, Tex*)      // compute writes
  srvA/srvB = RHI.HeapWriteTexture(heap, Tex*)        // viewport samples this
  Pipeline  = RHICore.CreateComputePipeline("GameOfLife.slang")
  seed:  CmdClearTexture / CmdCopyMemoryToTexture from a transient upload, or a Seed flag in the shader

OnRender(frameIndex):
  cl = RHI.OpenCommandList()
  RHI.CmdSetTextureHeap(cl, heap); RHI.CmdSetPipeline(cl, Pipeline)
  args = RHICore.AllocTransient(sizeof(PC));  *(PC*)args.Cpu = { SrcUAV, DstUAV, W, H, ... }
  RHI.CmdDispatch(cl, args.Gpu, (W+7)/8, (H+7)/8, 1)
  RHI.Barriers.ComputeToAll(cl)
  RHI.Submit(cl)
  swap(A, B)

DisplayTexture    => current texture       (packaged: blitted to the swapchain)
DisplayResourceID => current SRV slot      (editor: sampled by the viewport)
RenderExtent      => (W, H), never zero
```

No semaphore wait is needed: same graphics queue, submitted before the present command list, with a barrier
at the end of ours.

**What you give up:** the world renders nothing else. No meshes, no debug lines (`GetImmediateLines()`
returns null for a managed scene, so `World.Draw*` is dead), and **no world RmlUi HUD** —
`RmlUi::RenderWorldUI` is only ever called by `FDefaultSceneRenderer` (`DefaultSceneRenderer.cpp:1387`), and
it renders into `Scene->GetDisplayTexture()` as a color attachment. Getting a HUD over the simulation needs
one new export (see gaps) and `ColorAttachment` usage on the texture.

**Cost:** one `.slang` file, one script. Zero engine changes. Highest information per hour, because it
exercises the entire untested managed-render-scene path.

### Route B — write into a `CTextureRenderTarget`, keep the normal renderer

The world renders normally; the simulation lives in an RT asset that RmlUi (`<img src="/Game/GoL">`) or a
material samples. This is the "present it through the UI" version of the ask.

Missing pieces (all small):

1. **C# has no handle on a texture asset's GPU identity.** The generated binding
   (`Intermediates/CSharpBindings/Runtime/TextureRenderTarget.generated.cs`) exposes only
   Width/Height/Format/ClearColor. Need three exports: `Texture_GetTextureHandle → FTextureH`,
   `Texture_GetResourceID → int32`, `Texture_GetStorageSlot(mip) → uint32`.
2. **Where the dispatch runs.** Two options:
   - *(a) Script submits its own command list* from a tick. Works with today's RHI surface, and is safer
     than it sounds now that the render thread is gone (everything is on the main thread) — but the work
     lands outside the world's render, ordered only by submission order.
   - *(b) Extend the paint-op queue* with a generic "script compute op" (pipeline name + target UAV +
     push-constant blob + group counts) replayed by a pass alongside `TexturePaintPass`. This is the
     pattern the engine already trusts for exactly this problem, ~80 lines, and gets the ordering right by
     construction. Preferred if we go this way.
3. `CWorld::PaintRenderTarget` / `ClearRenderTarget` are not exposed to C# at all. Worth adding regardless —
   it is a five-minute export that gives an immediately testable "paint into an RT from a script" before any
   Game-of-Life work.

### Route C — overwrite the live world render target

Not supported, and not a small change. `IRenderScene::GetDisplayTexture()` is C++-only (no export) and there
is **no render-extension / view-extension / custom-pass hook anywhere in the renderer**. It would need
either the display texture exposed to C# plus an "after scene render" callback, or a real view-extension
API. Recommend deferring until we actually want script-driven passes over the real scene.

---

## 3. Gap list

| # | Gap | Needed by | Size |
| --- | --- | --- | --- |
| 1 | No sample/doc/runtime test for managed `RenderScene` — we are the first caller | A | budget debugging, not code |
| 2 | Shaders resolve only under `Engine/Resources/Shaders` (`ShaderLibrary.cpp:183`); a game project or plugin cannot name its own `.slang` | A, B | none for a test (drop the file in the engine dir); MEDIUM to add project shader dirs or a compile-from-source export |
| 3 | No C# accessor for a `CTexture`'s `FTextureH` / SRV id / storage slot | B | SMALL (3 exports) |
| 4 | `PaintRenderTarget` / `ClearRenderTarget` not exposed to C# | B | SMALL |
| 5 | `RmlUi::RenderWorldUI` is default-renderer-only, so a managed scene gets no world HUD | A (only if we want a HUD) | SMALL (one export + `ColorAttachment` usage) |
| 6 | `IRenderScene::SetPrimaryViewSize` is a no-op default; the editor calls it every frame (`EditorUI.cpp:2533`) but a managed scene only hears `Resize`/`OnResize` | A | none — the grid is script-sized and stretched to the panel; note the aspect |
| 7 | Debug lines and entity picking are default-renderer-only | A | by design |
| 8 | Managed scenes only apply to `EWorldType::Game` — the non-PIE editor world keeps the default renderer | A | test in PIE or standalone |
| 9 | No script access to the default renderer's display texture, no custom-pass hook | C | LARGE |

Resource-lifetime notes for whichever route: free textures and heap slots in `OnShutdown`
(`RHI.FreeH` + `HeapFreeTexture`/`HeapFreeRWTexture`) — hot reload runs it — and never free-and-realloc a
bindless slot that an in-flight frame may still reference.

---

## 4. Suggested execution order

0. **Interop proof.** `GameOfLife.slang` that just writes a gradient, plus a C# `RenderScene` that creates
   one texture, dispatches it once, and returns it as `DisplayTexture`/`DisplayResourceID`. Hit Play; the
   PIE viewport should show it. This validates the entire untested managed-render-scene path in one shot —
   type discovery, factory override, extent, heap slots, dispatch, display, hot reload.
1. **Real simulation.** Ping-pong A/B, seeded random, one step per frame (optionally decoupled with a step
   accumulator), a couple of push-constant knobs (rule set, seed, step rate).
2. **UI half (optional, Route B).** Add gaps 3 + 4, create a `CTextureRenderTarget` asset, drive it via the
   generic script-compute-op, and display it with `<img src="/Game/GoL">` and/or a material on a mesh —
   which gets the simulation on screen *while the normal world renders*.
