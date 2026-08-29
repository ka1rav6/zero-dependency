const std = @import("std");

// A build.zig with an install artifact and nothing else: no run step, no test
// step, no check step. This is what `zig init-lib` used to produce.
pub fn build(b: *std.Build) void {
    const lib = b.addStaticLibrary(.{
        .name = "bare",
        .target = b.standardTargetOptions(.{}),
        .optimize = b.standardOptimizeOption(.{}),
    });
    b.installArtifact(lib);
}
