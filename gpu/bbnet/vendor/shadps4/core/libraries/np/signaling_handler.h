// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string_view>

#include "common/types.h"
#include "core/libraries/np/np_matching2/np_matching2.h"
#include "core/libraries/np/np_types.h"
#include "core/libraries/np/np_types2.h"

namespace Libraries::Np::NpMatching2 {
struct ContextObject;
}

namespace Libraries::Np::SignalingHandler {

void Start();
void Stop();
void AddContextRef();
void ReleaseContextRef();

s32 ActivateSig1(NpSignaling::OrbisNpSignalingContextId ctx_id, const OrbisNpId& peer_npid,
                 const OrbisNpOnlineId& peer_online_id,
                 NpSignaling::OrbisNpSignalingConnectionId* out_conn_id);
bool FindMatching2MemberNpId(OrbisNpAccountId account_id, OrbisNpId* out_npid);
void DeactivateSig1(NpSignaling::OrbisNpSignalingConnectionId conn_id);
void TerminateSig1(NpSignaling::OrbisNpSignalingConnectionId conn_id);
// shadNet's notice that a player wants to connect (addr/port network order), handled like
// that player's activation request, which our router drops until we have sent to them.
void HandlePeerConnectRequest(std::string_view peer_online_id, u32 addr, u16 port);

void StartMatching2(NpMatching2::ContextObject& ctx);
void StopMatching2(NpMatching2::ContextObject& ctx, s32 error_code);
void StopMatching2(NpMatching2::ContextObject& ctx, NpMatching2::OrbisNpMatching2RoomId room_id,
                   s32 error_code);

} // namespace Libraries::Np::SignalingHandler
