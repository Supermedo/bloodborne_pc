# AMD FSR SDK 2.4 API headers

Unmodified headers from AMD FSR SDK **v2.4.0**, commit
[`7539ae8`](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/tree/7539ae8b6568ad29ed78a753404351daf3b5cca7/Kits/FidelityFX).

Vendored files:

- `api/include/ffx_api.h`
- `api/include/ffx_api_loader.h`
- `api/include/ffx_api_matrix_helpers.h`
- `api/include/ffx_api_types.h`
- `api/include/dx12/ffx_api_dx12.h`
- `api/include/vk/ffx_api_vk.h`
- `upscalers/include/ffx_upscale.h`

CMake selects these include directories only for SDK-upscaling translation
units and their tests. `sdk24_upscaling.h` rejects the older API and verifies
the backend IDs at compile time. The official frame-generation implementation
still uses the separate SDK 2.3 headers in `gpu/fsr411_dx12/sdk`; retain that
copy until its independent migration is complete.

Effect DLLs are fetched separately using
`packaging/windows/fsr24-upscaling.json` size/SHA256/Git-blob pins. The package
includes AMD's complete redistribution license from
`packaging/windows/licenses/AMD-FSR-SDK-2.4-LICENSE.md`; headers retain their
original copyright notices.
