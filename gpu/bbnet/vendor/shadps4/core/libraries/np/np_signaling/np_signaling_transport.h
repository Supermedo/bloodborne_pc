// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string_view>

#include "common/types.h"

namespace Libraries::Np::NpSignaling::Transport {

struct TransportHooks {
    int (*signaling_send)(const void* data, u32 len, u32 dest_addr, u16 dest_port) = nullptr;
    int (*signaling_recv)(void* buf, u32 len, u32* from_addr, u16* from_port) = nullptr;
    int (*control_send)(const void* data, u32 len, u32 dest_addr, u16 dest_port) = nullptr;
    int (*control_recv)(void* buf, u32 len, u32* from_addr, u16* from_port) = nullptr;
    bool (*transport_ready)() = nullptr;
    u16 (*configured_port)() = nullptr;
    u32 (*advertised_addr)() = nullptr;
    bool (*ensure_transport)() = nullptr;
    u32 (*local_addr)() = nullptr;
};
void SetTransportHooks(const TransportHooks& hooks);

int SignalingSendTo(const void* data, u32 len, u32 dest_addr, u16 dest_port);
int SignalingRecvFrom(void* buf, u32 len, u32* from_addr, u16* from_port);
int ControlSendTo(const void* data, u32 len, u32 dest_addr, u16 dest_port);
int ControlRecvFrom(void* buf, u32 len, u32* from_addr, u16* from_port);
bool TransportIsReady();
u16 ConfiguredPort();
u32 AdvertisedAddr();
bool EnsureTransport();
// The LAN address sent in STUN pings (network order). shadNet hands it to players behind the
// same public IP, since most routers don't loop packets for their own public IP back inside.
u32 LocalAddr();

using PeerResolver = bool (*)(std::string_view online_id, u32* out_addr, u16* out_port);
void SetPeerResolver(PeerResolver fn);
void SetMatching2Enabled(bool enabled);
void SetMmServerEndpoint(u32 addr, u16 udp_port);

bool ResolvePeer(std::string_view online_id, u32* out_addr, u16* out_port);
bool Matching2Enabled();
u32 MmServerAddr();
u16 MmServerUdpPort();

} // namespace Libraries::Np::NpSignaling::Transport
