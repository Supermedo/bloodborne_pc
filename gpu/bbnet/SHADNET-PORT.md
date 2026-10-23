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
itself to the server as this port (`BB_CLIENT_VERSION`), not as shadPS4. The P2P transport
also carries the port's voice chat: it remembers the peers it sends game traffic to and hands
`[0xbb]['V']` datagrams to `runtime_voice.c` before decoding (`VoicePeerCount`, `SendVoice`).

The renderer, shaders and upscalers were not imported. `bbnet_glue.cpp` and `include/`
implement what these libraries need from the rest of shadPS4 (kernel threads, user
service, settings) on this project's runtime. Online play is off unless `BB_ONLINE=1`.

## The Hunter's Dream backend (`thd/`)

`thd/` is the co-op pool for The Hunter's Dream (`online_pool=huntersdream`, which sets
`BB_NET_BACKEND=thd`). It adapts droogie/bbhost's network side
(https://github.com/droogie/bbhost/tree/566184a74f071166299bb656c4af479e5bbe2771):
`src/hle/net.cpp`, `src/hle/np_matching2.cpp`, `src/hle/np_signaling.cpp`, the NP parts of
`src/hle/system.cpp`, and `src/net/{http,account,session,stun}.cpp`. These portions are
GPL-3.0-or-later; copyright remains with the bbhost contributors. With that pool the game's NP and
sceNet imports are these, ahead of the shadPS4 ones; the shadNet pool is unchanged. sceHttp stays
shadPS4's, with two `bbport` hooks that act only in that pool: `*.example.com` host-override keys
and the account token and ruleset headers.
