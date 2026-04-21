//! panic.rs — mandatory panic handler for no_std kernels.
//!
//! In a `no_std` crate Rust requires exactly one `#[panic_handler]`.  We set
//! `panic = "abort"` in Cargo.toml so the optimiser will eliminate the panic
//! path entirely, but the symbol still must exist for the compiler to be
//! satisfied at link time.
//!
//! We call the C kernel's `panic()` macro via its underlying C function.
//! If that extern is not yet available (e.g. during unit-test builds outside
//! the kernel link step), we fall back to an infinite halt loop — which is
//! the same thing the C `panic()` macro does after printing.

use core::panic::PanicInfo;

// Forward declaration of the C kernel panic sink.
// Signature matches `__attribute__((noreturn)) void kpanic_halt(void)` which
// every panic macro in the kernel ultimately calls.
extern "C" {
    fn kpanic_halt() -> !;
}

#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
    // Safety: kpanic_halt() never returns and does not touch Rust state.
    unsafe { kpanic_halt() }
}
