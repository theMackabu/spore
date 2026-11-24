pub const ELF: &[u8] = include_bytes!(concat!(env!("OUT_DIR"), "/kernel-boot.elf"));
pub const DIRECT: &[u8] = include_bytes!(concat!(env!("OUT_DIR"), "/spore.bin"));
pub const EFI: &[u8] = include_bytes!(concat!(env!("OUT_DIR"), "/BOOTAA64.EFI"));
pub const LICENSE: &[u8] = include_bytes!("../boot/LIMINE-LICENSE");

pub const X86_64_ELF: &[u8] = include_bytes!(concat!(env!("OUT_DIR"), "/x86_64.elf"));
pub const X86_64_EFI: &[u8] = include_bytes!(concat!(env!("OUT_DIR"), "/BOOTX64.EFI"));
pub const RISCV64_ELF: &[u8] = include_bytes!(concat!(env!("OUT_DIR"), "/riscv64.elf"));
pub const RISCV64_EFI: &[u8] = include_bytes!(concat!(env!("OUT_DIR"), "/BOOTRISCV64.EFI"));
