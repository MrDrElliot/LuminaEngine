# NsightPerf

Editor-only plugin that integrates the **NVIDIA Nsight Perf SDK** as a live GPU metrics HUD
(periodic sampler) inside the Lumina editor.

## What it does

- Adds a **Tools → Plugins → Nsight Perf HUD** window: a continuously-updating dashboard of GPU
  metrics (SM occupancy, throughputs, memory bandwidth, ...) sourced from NvPerf's *periodic
  sampler* and rendered with the engine's ImPlot.
- Sampling runs only while the window is open.

## How it's toggled

The plugin **is** the unit of opt-in. It lives in `Engine/Plugins/NsightPerf/`, always builds, and
each project enables/disables it in its `.lproject`:

```json
"Plugins": [ { "Name": "NsightPerf", "Enabled": false } ]
```

It is `EnabledByDefault: true`. There is no global build flag.

## Why it loads at the Core phase

NvPerf's periodic sampler requires specific Vulkan **instance and device extensions enabled at
device creation**. The engine creates the device during startup, before most plugins load, so this
plugin's module loads at the **Core** loading phase (before `RHI::CreateDevice`) and registers the
required extensions via `RHI::Native::RegisterDeviceCreationRequest` (see
`Engine/Source/Runtime/Source/Renderer/RHINative.h`). It also grabs the live device through
`RHI::Native::GetNativeDeviceHandles()`, which returns *opaque* handles (no Vulkan types leak out of
the backend) that this plugin reinterprets to `Vk*` itself.

## Requirements

- An **NVIDIA GPU** with a driver that supports Nsight Perf. On other GPUs the window opens and
  explains that it's unavailable.
- The vendored SDK under `Engine/Source/ThirdParty/NsightPerf/` (headers + `nvperf_grfx_host`
  lib/DLL). The DLL is copied next to the executable at build time.

## Threading

The periodic sampler shares the engine's graphics queue, but the engine submits frames on the render
thread while this tool's per-frame work runs on the game thread. Concurrent `vkQueueSubmit` on one
queue from two threads loses the device. The tool therefore wraps every NvPerf call that submits
(init, the per-frame `OnFrameEnd`, teardown) in `RHI::Native::FScopedSubmitLock`, which takes the
same mutex the RHI uses for its own submits, so they're serialized and never race. The ImGui render
stays outside the lock.

Cost: while the tool is open, holding that lock around `OnFrameEnd` can briefly stall the render
thread each frame (a profiling-only hitch). A lower-overhead design would marshal the sampler onto
the render thread via a per-frame RHI hook; the submit-lock approach was chosen for simplicity.

## NVIDIA permission

`BeginSession` fails with `NVPA_STATUS_INSUFFICIENT_PRIVILEGE` until GPU performance counters are
allowed for non-admins: NVIDIA Control Panel → Developer → "Allow access to the GPU performance
counters to all users" (or run the editor elevated). The tool reports this state instead of failing.
