#pragma once

// core/version.hpp
//
// Single source of truth for the kap binary's version number. Both the CLI
// banner (core/main.cpp) and the unit tests include this header, so the
// reported version can never drift from what the tests assert.
//
// Versioning follows SemVer; the design doc's Milestone 10 targets a v1.0 tag,
// so we start at 0.y.z and bump per milestone, not per commit.

namespace kap
{

// --- Version constants ---------------------------------------------------------
// Pieces first so a release bump is a single one-line edit in each spot.
inline constexpr int         kVersionMajor     = 0;
inline constexpr int         kVersionMinor     = 1;
inline constexpr int         kVersionPatch     = 0;
inline constexpr const char* kVersionString    = "0.1.0";
inline constexpr const char* kProgramName      = "kap";
inline constexpr const char* kVersionBannerUrl = "https://github.com/kap-project/kap";

} // namespace kap