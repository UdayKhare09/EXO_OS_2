//! exo_rust — Rust components for the EXO_OS kernel
//!
//! Rules for everything in this crate
//! ------------------------------------
//! * `no_std`   — no standard library; the kernel supplies its own allocator
//!                and runtime helpers via C.
//! * `no_main`  — C's `kmain` is the entry point; Rust is a library here.
//! * All public symbols exposed to C must be `#[no_mangle] pub extern "C"`.
//! * No heap allocation yet — `kmalloc` / `kfree` will be wired in a later
//!   PR via a custom `GlobalAllocator`.

#![no_std]
// Deny a handful of footguns that are easy to hit in kernel code.
#![deny(unsafe_op_in_unsafe_fn)]

// ── Sub-modules ──────────────────────────────────────────────────────────────
mod panic;   // mandatory panic handler (abort)

pub mod path; // path normalisation — rewrite of fs/path.c

// ── C-callable interface ──────────────────────────────────────────────────────
// Every function in this section is callable from C with a normal C ABI.
// The symbol names are stable: do NOT add `#[cfg]` guards that would silently
// drop a symbol the C side expects.
