#![no_std]

mod elf;
mod ext2;
mod net;

#[panic_handler]
fn panic(_: &core::panic::PanicInfo<'_>) -> ! {
    unsafe extern "C" {
        #[cfg_attr(target_os = "none", link_name = "aarch64_halt")]
        #[cfg_attr(not(target_os = "none"), link_name = "abort")]
        fn halt() -> !;
    }
    unsafe { halt() }
}
