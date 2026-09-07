// SPDX-License-Identifier: GPL-2.0-or-later

// log.h: one log line goes to both stderr and a file on disk.
//
// On the console stderr is only visible if something is watching Aroma's
// output, and the build machine is not attached to the TV. The file copy on
// the SD card is what actually gets read afterwards, over FTP.

#pragma once

namespace Log {

// Truncates the previous run's log. Call once, after Platform::Init.
void Init();
void Shutdown();

// printf-style. Newline is added automatically.
void Write(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

}  // namespace Log

#define LOGF(...) ::Log::Write(__VA_ARGS__)
