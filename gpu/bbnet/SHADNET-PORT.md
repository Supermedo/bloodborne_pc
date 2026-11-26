# Native shadNet bridge

The network-only source in `gpu/bbnet/vendor/shadps4/` is shadPS4's own, unchanged except
where marked `bbport`:
https://github.com/shadps4-emu/shadPS4/tree/v.0.20.0
(529ceb32f5546b820588f194b903cc39d08015fe): `src/core/libraries/net`, `network`, `np`
(without trophies, commerce and the profile and Facebook dialogs), `src/core/net` and
`src/shadnet`. These portions are GPL-2.0-or-later, with their original notices.
`third_party/httplib/httplib.h` and `third_party/wepoll/` retain their embedded
third-party license notices.

The bbport changes: the shadNet session starts once the game is loaded
(`NpManager::InitializeRuntime`, called by `bbnet_initialize`), and the client reports
itself to the server as this port (`BB_CLIENT_VERSION`), not as shadPS4.

The renderer, shaders and upscalers were not imported. `bbnet_glue.cpp` and `include/`
implement what these libraries need from the rest of shadPS4 (kernel threads, user
service, settings) on this project's runtime. Online play is off unless `BB_ONLINE=1`.

