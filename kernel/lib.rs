#![no_std]

#[path = "exec/elf.rs"]
mod elf;
#[path = "proc/manifest.rs"]
mod manifest;
#[path = "net/packet.rs"]
mod packet;
#[path = "fs/ext2/records.rs"]
mod records;

#[panic_handler]
fn panic(_: &core::panic::PanicInfo<'_>) -> ! {
    unsafe extern "C" {
        #[cfg_attr(target_os = "none", link_name = "aarch64_halt")]
        #[cfg_attr(not(target_os = "none"), link_name = "abort")]
        fn halt() -> !;
    }
    unsafe { halt() }
}
