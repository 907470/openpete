// SPDX-FileCopyrightText: 2026 Tyson Cleary
// SPDX-License-Identifier: MIT

#pragma once

namespace OpenpeteRegtest {

/// Prints the openpete options' help lines to stderr.
void PrintHelp();

/// Parses the argument at argv[i] when it is an openpete option, advancing i past its value.
/// Returns 1 when consumed, 0 when the argument is not an openpete option, -1 on an invalid value.
int ParseArgument(int argc, char* argv[], int& i);

/// Starts the recorders, the movie player, the card import and the motor log after the system booted; false when
/// one fails (the reason is logged).
bool Start();

/// Advances the pad script, the movie and the card export by one frame; true when the run must end now.
bool OnFrame();

/// Writes the end-of-run card exports; called when the run's last frame shuts the system down.
void OnSystemShutdown();

/// Closes every recorder and log.
void Shutdown();

} // namespace OpenpeteRegtest
