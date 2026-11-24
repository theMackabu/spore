pub const ELF: &[u8] = include_bytes!(concat!(env!("OUT_DIR"), "/kernel-boot.elf"));
pub const DIRECT: &[u8] = include_bytes!(concat!(env!("OUT_DIR"), "/spore.bin"));
pub const EFI: &[u8] = include_bytes!(concat!(env!("OUT_DIR"), "/BOOTAA64.EFI"));
pub const LICENSE: &[u8] = include_bytes!("../boot/LIMINE-LICENSE");
