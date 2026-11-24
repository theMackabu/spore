#![no_std]

#[path = "fs/ext2/bitmap.rs"]
mod bitmap;
#[path = "exec/elf.rs"]
mod elf;
#[path = "proc/policy/manifest.rs"]
mod manifest;
#[path = "net/packet.rs"]
mod packet;
#[path = "abi/linux/shared/fs/path.rs"]
mod path;
#[path = "fs/ext2/records.rs"]
mod records;

#[panic_handler]
fn panic(_: &core::panic::PanicInfo<'_>) -> ! {
    unsafe extern "C" {
        #[cfg_attr(all(target_os = "none", target_arch = "aarch64"), link_name = "arch_halt")]
        #[cfg_attr(all(target_os = "none", not(target_arch = "aarch64")), link_name = "platform_poweroff")]
        #[cfg_attr(not(target_os = "none"), link_name = "abort")]
        fn halt() -> !;
    }
    unsafe { halt() }
}
