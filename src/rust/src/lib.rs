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

pub mod math; // saturating / checked integer helpers
pub mod path; // path normalisation — rewrite of fs/path.c

// ── C-callable interface ──────────────────────────────────────────────────────
// Every function in this section is callable from C with a normal C ABI.
// The symbol names are stable: do NOT add `#[cfg]` guards that would silently
// drop a symbol the C side expects.

/// rust_hello — smoke-test: returns 42.
///
/// Call from C:
/// ```c
/// extern uint32_t rust_hello(void);
/// uint32_t v = rust_hello();   /* v == 42 */
/// ```
#[no_mangle]
pub extern "C" fn rust_hello() -> u32 {
    42
}

/// rust_add_u64 — wrapping 64-bit addition.
///
/// Demonstrates passing primitive types across the FFI boundary.
///
/// ```c
/// extern uint64_t rust_add_u64(uint64_t a, uint64_t b);
/// ```
#[no_mangle]
pub extern "C" fn rust_add_u64(a: u64, b: u64) -> u64 {
    a.wrapping_add(b)
}

/// rust_clz32 — count leading zeros of a 32-bit value.
///
/// Returns 32 when `x == 0` (matches the behaviour that most kernel code
/// expects, unlike the undefined-behaviour of the C `__builtin_clz(0)`).
///
/// ```c
/// extern uint32_t rust_clz32(uint32_t x);
/// ```
#[no_mangle]
pub extern "C" fn rust_clz32(x: u32) -> u32 {
    x.leading_zeros()
}

/// rust_popcount64 — population count (number of set bits) of a 64-bit value.
///
/// ```c
/// extern uint32_t rust_popcount64(uint64_t x);
/// ```
#[no_mangle]
pub extern "C" fn rust_popcount64(x: u64) -> u32 {
    x.count_ones()
}
