// SPDX-License-Identifier: GPL-2.0-or-later

// Obfuscation for the tokens kept on disk, not security. The key ships with
// the client, so anyone holding both can recover them. It only keeps a token
// out of a screenshot or a shared save folder.
//
// Self contained: the Wii U links no ffmpeg and the Xbox builds it with a
// different calling convention, so a cipher from there does not travel.

#pragma once

#include <string>

namespace Secret {

// Base64 of the encrypted bytes, safe to put in a JSON string. Empty in,
// empty out.
std::string Hide(const std::string& plain);

// The inverse. Returns empty on anything that does not decrypt cleanly,
// including a value written by a different install.
std::string Show(const std::string& hidden);

}  // namespace Secret
