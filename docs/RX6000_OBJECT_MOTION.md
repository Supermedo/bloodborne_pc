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

Short gameplay checks of an earlier combined prototype passed on RX 6800 XT with
Adrenalin 26.10.1, with cache off/on and FSR 4.1.2 Vulkan/DX12. Further RX 6000 gameplay
coverage is still needed; this does not establish reliability on every model/driver.

[Implementation and validation details](https://github.com/rattensuppe/bloodborne_pc/blob/fix/rx6000-object-motion/docs/development/RX6000_OBJECT_MOTION.md)
