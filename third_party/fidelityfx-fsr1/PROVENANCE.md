# AMD FidelityFX Super Resolution 1 (vendored)

**In oranges:** the GPU headers FSR 1 needs, copied unchanged from the AMD FidelityFX SDK
(https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK), v1.1.4, commit
`c6efa6bf7f2027b3ec94f28578bb5965eabb9e55`, as Longshot vendors that SDK (`third_party/fidelityfx-sdk` there, for its
FSR 3.1 passes), 2026-11. Do not edit them here.

- `include/FidelityFX/gpu/fsr1/ffx_fsr1.h`: EASU (the edge-adaptive upscaler) and RCAS (the sharpener).
- `include/FidelityFX/gpu/ffx_core*.h`, `ffx_common_types.h`: the SDK's GPU core they are written on (types and math,
  GLSL flavour with `FFX_GPU` and `FFX_GLSL` defined).
- `LICENSE.txt`: AMD's MIT notice. Keep it with any distribution.

oranges' own passes are `host/vk/shaders/fsr1_easu.comp` and `fsr1_rcas.comp`, which include these headers;
`python -m oranges.shaders` compiles them into the checked-in `host/vk/shaders/*_spv.h` (so a build needs no glslang),
and `host/vk/vk_fsr.cpp` runs them at the present (the Graphics tab's Scaling: FSR 1).

SHA-256 of each file:

```
a19560f837c09f0c990e98b5925b731135d908e372af41c5895cbfa70cc9e5c8  include/FidelityFX/gpu/ffx_common_types.h
01afe5b65549b2c0951b494ca4bf570ee2aa89a64344e4c7d98ef86e6613ab42  include/FidelityFX/gpu/ffx_core_glsl.h
0e3c50cb0b4592c051d87e028a1dfa54fbd097942ebb3146964db801dec095d0  include/FidelityFX/gpu/ffx_core_gpu_common.h
d10466cbf027c8c23858a25f8e8cbbbf521d29f373db663c9354221651e9a498  include/FidelityFX/gpu/ffx_core_gpu_common_half.h
e439aa5f516b2d2ebf070ff1d24561f04fa5f00262a2e4f760764a7d07de7997  include/FidelityFX/gpu/ffx_core.h
a7c3a1c213ec083bec401181036cffe7a222af2c430c0bab6bd8c14e83e8a567  include/FidelityFX/gpu/ffx_core_portability.h
5d608d9d0645da03a3aa6c729c5ad990c6f222053eab850b8e218607c1baaa9d  include/FidelityFX/gpu/fsr1/ffx_fsr1.h
```
