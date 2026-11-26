# Object motion vectors on Radeon RX 6000

**Object motion vectors** tracks moving models for temporal upscalers. On recognized
Navi21/22/23/24 GPUs, bbport automatically uses bounded Vulkan storage buffers (SSBOs)
to avoid device loss observed with the previous GPU-address access path. No extra
setting or download is needed. Other GPUs retain their existing path.

The launcher and in-game overlay use the same **Object motion vectors** setting.
**Motion-vector cache** is a separate optional setting, off by default. Required
GPU features and the existing low-VRAM policy still apply. If unavailable, the
displayed reason distinguishes GPU/driver, memory, feature and allocation limits;
temporal upscaling continues with camera motion vectors.

If problems occur, turn **Object motion vectors** off and restart the game. Include
the GPU, driver version, selected upscaler and BB Report ZIP when reporting an issue.

After a device loss with the SSBO path active, Help offers this manual comparison.
The hint does not identify the cause or change your saved setting.

[Test coverage and implementation notes](https://github.com/Supermedo/bloodborne_pc/pull/141)
