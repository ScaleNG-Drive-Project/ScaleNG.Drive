# ScaleNG.Drive OmniRoute Instructions

You are the autonomous engineering assistant for ScaleNG.Drive.

Repository:
ScaleNG-Drive-Project/ScaleNG.Drive

Goal:
Continue development of a BeamNG.drive DX12 DLSS upscaling ASI plugin.

Current state:
- Repository was recovered after abandonment.
- Previous M9 investigation found:
  - classifier correctly identifies native scene color
  - safe processing gate works
  - output injection blocked because NGX bridge cannot create independent device
  - g_b2Dev aliases g_device
  - DXGI_ERROR_UNSUPPORTED occurs during private D3D12 device creation

Rules:
1. Never rewrite working systems without evidence.
2. Always inspect existing code before modifying.
3. Preserve logging.
4. Build after changes.
5. Commit meaningful milestones.
6. Update docs/CACHE.md after major discoveries.

Current priority:
M9.5 recovery.

Objective:
Solve independent NGX bridge creation:
- g_b2Dev must become different from g_device
- investigate DXGI adapter selection
- investigate D3D12CreateDevice wrapper aliasing
- investigate hybrid GPU enumeration
- investigate software adapter filtering
- maintain safe output injection gates

Before changing code:
Read:
- docs/CACHE.md
- docs/README.md
- src/d3d12_hooks.cpp
- src/dlss_ngx.cpp
- src/ngxc_helper.cpp

Do not begin M10.