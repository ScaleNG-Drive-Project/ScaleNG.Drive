# ReShade Integration Lessons for ScaleNG.Drive

> **Date:** 2026-10-02
> **Status:** Analysis complete - ready for porting phase

---

## Executive Summary

ReShade is a **working, proven** D3D12/DXGI injection framework that:
- ✅ Loads into BeamNG.drive without crashes
- ✅ Hooks DXGI factory and swap chain creation cleanly
- ✅ Intercepts texture operations (depth, motion vectors, color)
- ✅ Provides ImGui overlay system
- ❌ No built-in DLSS/upscaling (but provides all necessary hooks to implement it)

---

## ReShade Architecture Deep Dive

### 1. Injection Point: DXGI Proxy DLL
```
ReShade64.dll renamed to dxgi.dll
  → Placed in game Bin64 folder
  → Windows loads it BEFORE system dxgi.dll
  → Hooks CreateDXGIFactory / CreateSwapChainForHwnd
```

**Why this works for BeamNG:**
- BeamNG creates swapchain through DXGI
- ReShade's dxgi.dll intercepts BEFORE game's internal hooks
- Clean, stable injection point - no vtable patching of game's D3D12 device

### 2. Addon System (Event-Based Architecture)

```cpp
// Register addon
reshade::register_addon(hModule);

// Hook events
reshade::register_event<addon_event::init_device>(on_init_device);
reshade::register_event<addon_event::init_swapchain>(on_init_swapchain);
reshade::register_event<addon_event::init_resource>(on_init_resource);
reshade::register_event<addon_event::update_texture_region>(on_update_texture);
reshade::register_event<addon_event::copy_buffer_to_texture>(on_copy_buffer_to_texture);
reshade::register_event<addon_event::copy_texture_region>(on_copy_texture_region);
reshade::register_event<addon_event::init_effect_runtime>(on_init_effect_runtime);
```

**Key Events for DLSS:**
| Event | Purpose | DLSS Relevance |
|-------|---------|----------------|
| `init_device` | Capture device, create resources | Get ID3D12Device for NGX |
| `init_swapchain` | Get swapchain, back buffer count | Know presentation chain |
| `init_resource` | Intercept texture creation | Find depth/MV/color at creation |
| `update_texture_region` | Track CPU→GPU updates | Detect depth/MV updates |
| `copy_buffer_to_texture` | Track upload heap→texture | Detect motion vector uploads |
| `copy_texture_region` | Track GPU→GPU copies | Trace render pipeline |
| `init_effect_runtime` | Get effect_runtime pointer | Access back buffer, command queue |

### 3. Resource Interception (Texture Discovery)

```cpp
static bool filter_texture(device *dev, const resource_desc &desc) {
    // Filter for 2D textures, non-MSAA, minimum size
    if (desc.type != resource_type::texture_2d || desc.texture.samples != 1)
        return false;
    if (desc.texture.width < 100 || desc.texture.height < 100)
        return false;
    
    // Identify by format
    if (is_depth_format(desc.texture.format)) return true;
    if (is_motion_vector_format(desc.texture.format)) return true;
    if (is_color_format(desc.texture.format)) return true;
    
    return false;
}
```

**Format Detection:**
| Resource Type | Formats |
|---------------|---------|
| Depth | `D16_UNORM`, `D24_UNORM_S8_UINT`, `D32_FLOAT`, `R32_TYPELESS`, `R32_FLOAT` |
| Motion Vectors | `R16G16_FLOAT`, `R32G32_FLOAT` |
| Color (HDR) | `R16G16B16A16_FLOAT`, `R10G10B10A2_UNORM` |
| Color (LDR) | `R8G8B8A8_UNORM`, `R8G8B8A8_SRGB`, `B8G8R8A8_UNORM` |

### 4. Resource Access API

```cpp
// Get back buffer
resource bb = swapchain->get_back_buffer(0);

// Get device from swapchain
device *dev = swapchain->get_device();

// Get command queue for executing commands
command_queue *queue = effect_runtime->get_command_queue();

// Get device from command list
device *dev = cmd_list->get_device();

// Get resource description
resource_desc desc = dev->get_resource_desc(resource);

// Get native handle for direct D3D12 access
uint64_t native = resource.get_native(); // ID3D12Resource*
```

### 5. ImGui Overlay (UI Integration)

```cpp
reshade::register_overlay("DLSS Overlay", [](effect_runtime *runtime) {
    ImGui::Begin("DLSS Status");
    ImGui::Text("DLSS: %s", enabled ? "ACTIVE" : "INACTIVE");
    ImGui::Text("Render: %ux%u -> %ux%u", renderW, renderH, displayW, displayH);
    ImGui::End();
});
```

---

## ReShade vs ScaleNG.Drive Comparison

| Aspect | ReShade (Working) | ScaleNG.Drive (Blocked) |
|--------|-------------------|-------------------------|
| Injection | `dxgi.dll` proxy | `D3D12CreateDevice` detour + D3D12 vtable hooks |
| NGX Location | In-process (could be) | Cross-process helper (required) |
| Hook Point | DXGI factory/swapchain | D3D12 device + command list vtables |
| Resource Discovery | `init_resource` event | `CreateRenderTargetView` hook |
| MV/Depth Access | `init_resource`/`update_texture_region` | `CreateRenderTargetView`/`CreateShaderResourceView` hooks |
| Overlay | ImGui via `register_overlay` | Custom HUD (broken) |
| NGX Evaluation | Could be in-process | Cross-process via named pipes |
| Stability | ✅ Proven | ❌ NGX rejection / crashes |

---

## Porting Plan: ReShade Patterns → ScaleNG.Drive

### Phase 1: Switch to DXGI Proxy Injection
```
BEFORE: ScaleNG.asi + UAL + D3D12CreateDevice detour
AFTER:  ScaleNG.dll renamed to dxgi.dll (or d3d12.dll) in Bin64
```
- Remove UAL dependency
- Hook `CreateDXGIFactory1` → intercept swapchain creation
- This avoids BeamNG's device wrapper issues entirely

### Phase 2: Adopt Addon Event System
```cpp
// ScaleNG events mirror ReShade
enum ScaleNgEvent {
    INIT_DEVICE,
    INIT_SWAPCHAIN,
    INIT_RESOURCE,
    UPDATE_TEXTURE_REGION,
    COPY_BUFFER_TO_TEXTURE,
    COPY_TEXTURE_REGION,
    INIT_EFFECT_RUNTIME,  // = "we have swapchain + device + queue"
    PRESENT,
};
```

### Phase 3: Resource Discovery via Events
```cpp
// Instead of hooking CreateRenderTargetView, hook at resource creation
static void on_init_resource(device *dev, const resource_desc &desc, ...) {
    if (is_depth_format(desc.format))      g_depth_res = res;
    if (is_motion_vector_format(desc.format)) g_mv_res = res;
    if (is_color_format(desc.format))      g_color_res = res;
}
```

### Phase 4: In-Process NGX (Like ReShade Could Do)
```cpp
// At init_effect_runtime:
g_device = swapchain->get_device();
g_queue = effect_runtime->get_command_queue();
g_backbuffer = swapchain->get_back_buffer(0);

// Initialize NGX on g_device (same process, same device as game)
// No cross-process pipes, no shared handles, no fences
g_upscaler->Init({g_device, renderW, renderH, displayW, displayH, ...});
```

### Phase 5: NGX Evaluation at Present
```cpp
static void on_present(swapchain *swapchain) {
    // Get current back buffer
    resource bb = swapchain->get_back_buffer(0);
    
    // Barrier: backbuffer PRESENT → COPY_SOURCE
    // Copy bb → g_color (our NGX input)
    // Barrier: g_color COPY_DEST → SHADER_RESOURCE
    
    // Run NGX evaluate
    g_upscaler->Evaluate({g_queue, g_color, g_depth, g_mv, g_output, ...});
    
    // Barrier: g_output UAV → COPY_SOURCE
    // Copy g_output → bb
    // Barrier: bb COPY_DEST → PRESENT
}
```

---

## ReShade Addon We Built: `19-depth_motion_dump`

**Purpose:** Dump all depth/motion vector/color textures to identify DLSS integration points

**Events Hooked:**
- `init_resource` - Texture creation
- `update_texture_region` - CPU→GPU updates
- `copy_buffer_to_texture` - Upload heap → texture
- `copy_texture_region` - GPU→GPU copies (render pipeline tracing)
- `init_swapchain` - Swapchain creation/resize

**Output:** `reshade_depth_motion_dump/depth_motion_log.txt`
```
[init_resource] DEPTH      res=0x1234 fmt=D32_FLOAT size=1920x1080
[init_resource] MOTION_VECTOR res=0x5678 fmt=R16G16_FLOAT size=1920x1080
[copy_texture_region] COLOR (src) → COLOR (dest)  -- render pipeline trace
```

---

## Next Steps for ScaleNG.Drive

### Immediate (This Session)
1. ✅ Document ReShade architecture (DONE)
2. ⏳ Update CACHE.md with ReShade lessons
3. ⏳ Update ARCHITECTURE_REWRITE_PLAN.md with ReShade patterns

### Next Session: ScaleNG.Drive Refactor
1. **M0:** Create `src/dxgi_hooks.cpp` - DXGI proxy hooks
2. **M1:** Implement `ScaleNgEvent` system mirroring ReShade
3. **M2:** Resource tracker using `init_resource` pattern
4. **M3:** In-process NGX initialization at `init_effect_runtime` equivalent
5. **M4:** Present-time NGX evaluation using back buffer
6. **M5:** ImGui overlay via `register_overlay` pattern

### Validation Test
```
1. Build ScaleNG as dxgi.dll
2. Place in BeamNG Bin64
3. Launch with -level GridMap -vehicle pickup
4. Verify: ReShade.log shows init_device → init_swapchain → init_resource (depth/MV/color)
5. Verify: NGX initializes and evaluates without rejection
6. Verify: ImGui overlay shows "DLSS ACTIVE"
```

---

## Files to Update

| File | Update |
|------|--------|
| `docs/CACHE.md` | Add ReShade architecture lessons |
| `docs/ARCHITECTURE_REWRITE_PLAN.md` | Add ReShade-based refactor phases |
| `docs/DLSS_INTEGRATION_PLAN.md` | Update with in-process NGX approach |
| `src/dxgi_hooks.cpp` | NEW - DXGI proxy hooks |
| `src/events.cpp` | NEW - Event system |
| `src/resource_tracker.cpp` | NEW - Resource tracker |
| `CMakeLists.txt` or `build.bat` | Update for new files |

---

## Key Takeaway

**ReShade proves:** DXGI-level hooking + event-based resource tracking + in-process evaluation = stable, working injection.

**ScaleNG.Drive must:** Abandon cross-process helper, D3D12 vtable hooks, and custom HUD. Adopt ReShade's proven patterns.

> "Don't reinvent the wheel - ReShade already invented the injection wheel. We just need to put DLSS tires on it."