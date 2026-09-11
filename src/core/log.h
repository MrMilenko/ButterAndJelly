// SPDX-License-Identifier: GPL-2.0-or-later

// log.h: every line goes to both the platform's debug channel and a file.
//
// The file is what gets read afterwards, since no console has a terminal.

#pragma once

namespace Log {

// Truncates the previous run's log. Call once, after Platform::Init.
void Init();
void Shutdown();

// printf-style. Newline is added automatically.
void Write(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

}  // namespace Log

#define LOGF(...) ::Log::Write(__VA_ARGS__)
