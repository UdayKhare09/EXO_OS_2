//! build.zig — EXO_OS Zig addon (Zig 0.15+)
//!
//! Produces libexo_zig.a: a freestanding staticlib for x86_64 kernel code.

const std = @import("std");

pub fn build(b: *std.Build) void {
    // ── Target: bare-metal x86_64 ────────────────────────────────────────────
    const target = b.resolveTargetQuery(.{
        .cpu_arch = .x86_64,
        .os_tag   = .freestanding,
        .abi      = .none,
    });

    // Always ReleaseFast for kernel code — avoids the -Doptimize CLI flag
    // whose API changed between Zig 0.13→0.15.
    const optimize: std.builtin.OptimizeMode = .ReleaseFast;

    // ── Root module ───────────────────────────────────────────────────────────
    const mod = b.createModule(.{
        .root_source_file = b.path("src/gpt.zig"),
        .target           = target,
        .optimize         = optimize,
        .code_model       = .kernel,
        .red_zone         = false,
        .stack_protector  = false,
    });

    // ── Static library ────────────────────────────────────────────────────────
    const lib = b.addLibrary(.{
        .name     = "exo_zig",
        .root_module = mod,
        .linkage  = .static,
    });

    b.installArtifact(lib);
}
