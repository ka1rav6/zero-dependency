#pragma once

// core/version.hpp
//
// Single source of truth for the kap binary's version number. Both the CLI
// banner (core/main.cpp) and the unit tests include this header, so the
// reported version can never drift from what the tests assert.
//
// Versioning follows SemVer. Milestones 0 through 10 are complete, so this is
// the v1.0 the design doc's roadmap targets: the CLI surface in §8, the KPL
// language in §5, the plugin manager in §6, and the on-disk layout in §6.4 are
// what a 1.x kap promises not to break.
//
// What a major bump would mean: a change to KPL that makes an existing plugin
// stop working (`api_version` exists to make that survivable), a change to the
// CommandSpec contract that invalidates committed golden files, or a change to
// where plugins and configuration live on disk.

namespace kap
{

// --- Version constants ---------------------------------------------------------
// Pieces first so a release bump is a single one-line edit in each spot.
inline constexpr int         kVersionMajor     = 1;
inline constexpr int         kVersionMinor     = 0;
inline constexpr int         kVersionPatch     = 0;
inline constexpr const char* kVersionString    = "1.0.0";
inline constexpr const char* kProgramName      = "kap";
inline constexpr const char* kVersionBannerUrl = "https://github.com/ka1rav6/zero-dependency";

} // namespace kap