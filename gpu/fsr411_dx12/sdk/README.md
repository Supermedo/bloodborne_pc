# AMD FSR API headers

These unmodified C headers are from AMD FSR SDK **v2.3.0**:
https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/tree/v2.3.0/Kits/FidelityFX

Only the API, DX12 backend and upscaler headers needed by `native_fsr411.cpp`
are vendored. Each carries AMD's MIT copyright and permission notice.

The runtime DLL is a separate binary under AMD's redistribution license; it is
not committed here. `packaging/windows/fetch_fsr411_dx12.py` downloads the
official, pinned release and verifies the archive and DLL hashes. The Windows
package includes the unmodified DLL and the full SDK license and notices from
`packaging/windows/licenses/AMD-FSR-SDK-*.md`.
