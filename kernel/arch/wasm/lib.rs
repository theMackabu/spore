#[cfg(target_arch = "wasm32")]
mod alu;
#[cfg(target_arch = "wasm32")]
mod cpu;
#[cfg(target_arch = "wasm32")]
mod decode;
#[cfg(target_arch = "wasm32")]
mod machine;
#[cfg(target_arch = "wasm32")]
mod memory;
#[cfg(target_arch = "wasm32")]
#[path = "../../shared.rs"]
mod shared;
#[cfg(target_arch = "wasm32")]
mod simd;
#[cfg(target_arch = "wasm32")]
mod transfer;
