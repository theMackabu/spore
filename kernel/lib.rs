#![no_std]

mod shared;

#[panic_handler]
fn panic(_: &core::panic::PanicInfo<'_>) -> ! {
    unsafe extern "C" {
        #[cfg_attr(target_os = "none", link_name = "arch_halt")]
        #[cfg_attr(not(target_os = "none"), link_name = "abort")]
        fn halt() -> !;
    }
    unsafe { halt() }
}
