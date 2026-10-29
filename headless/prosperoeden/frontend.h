#pragma once

#include <string>
#include <vector>
#include "../multiplayer.h"

// The launcher: the game chosen, or empty when it was closed. With `question_first`, a forwarded
// game waits for the first start's question about ProsperoEden's data (migrate.h): the launcher asks
// it and closes once it is answered with Start fresh, and a move or a copy starts the app again with
// these launch arguments (forwarded_args.h), so that the game starts then.
std::string SelectProsperoEdenGame(Eden::Multiplayer::RoomClient& rooms, const std::string& launch_error = {},
                                   const std::vector<std::string>* question_first = nullptr);
