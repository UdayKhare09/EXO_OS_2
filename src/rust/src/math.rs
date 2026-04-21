//! math.rs — safe integer utilities for the EXO_OS kernel.
//!
//! These are thin wrappers / re-exports of Rust's built-in numeric methods,
//! collected here so the C side can call them by stable ABI names and so that
//! future Rust kernel code has a single import path.

/// Saturating add for 64-bit unsigned values.
///
/// ```c
/// extern uint64_t rust_sat_add_u64(uint64_t a, uint64_t b);
/// ```
#[no_mangle]
pub extern "C" fn rust_sat_add_u64(a: u64, b: u64) -> u64 {
    a.saturating_add(b)
}

/// Saturating sub for 64-bit unsigned values.
///
/// ```c
/// extern uint64_t rust_sat_sub_u64(uint64_t a, uint64_t b);
/// ```
#[no_mangle]
pub extern "C" fn rust_sat_sub_u64(a: u64, b: u64) -> u64 {
    a.saturating_sub(b)
}

/// Align `value` up to the next multiple of `align` (must be a power of 2).
/// Returns `value` unchanged if already aligned.
///
/// ```c
/// extern uint64_t rust_align_up(uint64_t value, uint64_t align);
/// ```
#[no_mangle]
pub extern "C" fn rust_align_up(value: u64, align: u64) -> u64 {
    debug_assert!(align.is_power_of_two(), "align must be a power of two");
    (value + align - 1) & !(align - 1)
}

/// Align `value` down to the previous multiple of `align` (must be power of 2).
///
/// ```c
/// extern uint64_t rust_align_down(uint64_t value, uint64_t align);
/// ```
#[no_mangle]
pub extern "C" fn rust_align_down(value: u64, align: u64) -> u64 {
    debug_assert!(align.is_power_of_two(), "align must be a power of two");
    value & !(align - 1)
}

/// Returns `true` (1) if `x` is a power of two, `false` (0) otherwise.
/// `0` is not considered a power of two.
///
/// ```c
/// extern bool rust_is_power_of_two(uint64_t x);
/// ```
#[no_mangle]
pub extern "C" fn rust_is_power_of_two(x: u64) -> bool {
    x != 0 && x.is_power_of_two()
}

/// Next power of two ≥ `x`.  Returns 1 when `x == 0`.
/// Saturates to `u64::MAX` (which is not a power of two) on overflow —
/// callers should check the result with `rust_is_power_of_two`.
///
/// ```c
/// extern uint64_t rust_next_power_of_two(uint64_t x);
/// ```
#[no_mangle]
pub extern "C" fn rust_next_power_of_two(x: u64) -> u64 {
    x.next_power_of_two()
}
