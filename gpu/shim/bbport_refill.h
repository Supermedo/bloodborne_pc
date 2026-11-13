// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: the refill cheat (bbport.ini cheat_refill, the in-game menu, the launcher's Cheats
// page). Each time the player spawns (a save loaded, a lamp travel, a death) with fewer than 20
// Blood Vials or Quicksilver Bullets, the carried ones are topped up to 20 (as far as the game
// lets the player carry them). Live: no game patch, no restart. Game 1.09.

#pragma once

#include <cstdint>

namespace BbRefill {

/// Hooks the game's per-frame step (once, before the game runs). The cheat itself follows the
/// setting every frame, so the hook is in even while it is off.
void Install(unsigned char* image, std::uint64_t size);

} // namespace BbRefill
