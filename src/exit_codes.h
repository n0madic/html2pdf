#pragma once

namespace html2pdf {

// Process exit codes. Single source of truth shared by main.cpp and renderer.cpp
// (also documented in --help and in the README "Exit codes" table).
constexpr int kOk = 0;           // success
constexpr int kArgsError = 1;    // invalid arguments
constexpr int kFileError = 2;    // file I/O error (reading input or writing output)
constexpr int kNetworkError = 3; // network error (remote input, rejected scheme, SSRF block)
constexpr int kRenderError = 4;  // render / graphics error

}  // namespace html2pdf
