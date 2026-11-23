# eden's shader recompiler (vendored)

**In oranges:** copied unchanged from the Longshot toolchain v3.0.0 (https://git.magicbean.dev/MagicBeanDev/Longshot,
`gpu/vk/eden/shader_recompiler`, `gpu/vk/sirit`, `gpu/compat` and `gpu/vk/compat`, as the Longshot Launcher installs
them, 2026-11). Do not edit it here: `host/vk/vk_shader.cpp` drives it, and sets its switches (for example
`Shader::Backend::SPIRV::position_invariant`) from the outside.

- `shader_recompiler/`: the Maxwell shader recompiler of the yuzu emulator (GPL-2.0-or-later), as continued by the
  Eden Emulator Project (GPL-3.0-or-later), with Longshot's and its contributors' changes (marked `(longshot)`,
  `(amd-fix)`, `(Lichii ...)`, `(from Fritchii)` and so on, per file). The `frontend/ampere/` lowering (Switch 2
  programs to Maxwell) is part of it and unused here: the game's programs are Maxwell already.
- `sirit/`: the SPIR-V emitter (sirit, by ReinUsesLisp; `sirit/LICENSE*`).
- `video_core/textures/astc.{cpp,h}`, `workers.h`: suyu's (yuzu's) CPU ASTC decoder (GPL-3.0-or-later / GPL-2.0-or-later,
  with the University of North Carolina's Apache-2.0 parts), from the same toolchain's `gpu/third_party/suyu`: desktop
  GPUs sample no ASTC, so `host/vk/vk_texture.cpp` decodes it to RGBA8.
- `compat/`: Longshot's stand-ins for the parts of boost, {fmt}, ankerl and yuzu's `common/` the recompiler includes
  (on the C++20 standard library), and `longshot_env.h` (its switches' one rule).

Licensing: each file's SPDX line; the whole is GPL-3.0-or-later as oranges is.

`third_party/khronos/` (beside it) holds the C headers of Vulkan-Headers v1.4.345 (Apache-2.0 / MIT,
`LICENSE.vulkan-headers.md`) and SPIRV-Headers vulkan-sdk-1.4.341.0 (MIT, `LICENSE.spirv-headers`), from the same
toolchain's `cpm/`: the build needs them and the system has none.
