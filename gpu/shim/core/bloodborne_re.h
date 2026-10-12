// bbport co-op: the Bloodborne summon helpers from shadp2p's bloodborne_re.h. Seamless
// co-op (host placement) is not ported yet; these keep the header value between requests.
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace Core::Bloodborne {
std::optional<std::string> GetSeamlessHostPlacementHeader();
bool SetSeamlessHostPlacementHeader(std::string_view value);
void ClearSeamlessHostPlacementHeader();
void TraceMatching2LeaveRoom(std::uintptr_t return_address, std::uint64_t room_id);
} // namespace Core::Bloodborne
