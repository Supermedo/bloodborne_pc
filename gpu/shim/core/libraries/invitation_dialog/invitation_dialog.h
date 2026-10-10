// bbport co-op: the session invitation event types from shadPS4's invitation_dialog.h
// (the dialog itself is not ported; Bloodborne's co-op uses bells, not invitations).
#pragma once
#include <cstddef>
#include "common/types.h"
#include "core/libraries/np/np_types.h"

namespace Libraries::InvitationDialog {

constexpr int ORBIS_NP_SESSION_ID_MAX_SIZE = 45;
constexpr int ORBIS_NP_INVITATION_ID_SIZE = 60;

struct OrbisNpSessionId {
    char data[ORBIS_NP_SESSION_ID_MAX_SIZE];
    char term;
    char padding[2];
};

struct OrbisNpInvitationId {
    char data[ORBIS_NP_INVITATION_ID_SIZE];
    char term;
};

using OrbisNpSessionInvitationEventFlag = s32;
constexpr OrbisNpSessionInvitationEventFlag ORBIS_NP_SESSION_INVITATION_EVENT_FLAG_INVITATION =
    0x01;

struct OrbisNpSessionInvitationEventParam {
    OrbisNpSessionId sessionId;
    OrbisNpInvitationId invitationId;
    OrbisNpSessionInvitationEventFlag flag;
    char padding[4];
    Libraries::Np::OrbisNpOnlineId onlineId;
};
static_assert(sizeof(OrbisNpSessionInvitationEventParam) == 0x8c);

} // namespace Libraries::InvitationDialog
