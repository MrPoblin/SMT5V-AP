#pragma once
#include <cstdint>

namespace DeathFunctions {

// Registers the engine-tick-pre callback used to safely perform the death-link
// game-over. Call once during mod init.
void Setup();

// Triggers SMTVV's real "you died" game over by forcing the battle into
// E_BTL_STEP_GAMEOVER (defeat cutscene -> title). Used for manual testing (F6)
// and the incoming death-link kill. The transition is deferred to the
// engine-tick PRE callback so battle state is mutated at tick start, not mid
// engine-tick.
bool KillLocalPlayer(bool retry = false);

}
