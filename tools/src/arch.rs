use goblin::elf::header::{EM_AARCH64, EM_RISCV, EM_X86_64};
use std::{
    fs,
    io::{self, Read},
    path::Path,
};

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum Arch {
    Aarch64,
    Riscv64,
    X86_64,
}
impl Arch {
    pub fn parse(name: &str) -> io::Result<Self> {
        match name {
            "aarch64" => Ok(Self::Aarch64),
            "x86_64" => Ok(Self::X86_64),
            "riscv64" => Ok(Self::Riscv64),
            _ => Err(io::Error::other(format!(
                "unknown guest architecture {name}"
            ))),
        }
    }
    pub fn executable(path: &Path) -> io::Result<Self> {
        let mut h = [0; 20];
        fs::File::open(path)?.read_exact(&mut h)?;
        if &h[..7] != b"\x7fELF\x02\x01\x01" {
            return Err(io::Error::other(
                "expected a little-endian ELF64 executable",
            ));
        }
        match u16::from_le_bytes([h[18], h[19]]) {
            EM_AARCH64 => Ok(Self::Aarch64),
            EM_X86_64 => Ok(Self::X86_64),
            EM_RISCV => Ok(Self::Riscv64),
            n => Err(io::Error::other(format!("unsupported ELF machine {n}"))),
        }
    }
    pub fn name(self) -> &'static str {
        match self {
            Self::Aarch64 => "aarch64",
            Self::Riscv64 => "riscv64",
            Self::X86_64 => "x86_64",
        }
    }
    pub fn machine(self) -> u16 {
        match self {
            Self::Aarch64 => EM_AARCH64,
            Self::Riscv64 => EM_RISCV,
            Self::X86_64 => EM_X86_64,
        }
    }
    pub fn boot_files(self) -> (&'static str, &'static [u8], &'static [u8]) {
        use spore_kernel::*;
        match self {
            Self::Aarch64 => ("BOOTAA64.EFI", EFI, ELF),
            Self::Riscv64 => ("BOOTRISCV64.EFI", RISCV64_EFI, RISCV64_ELF),
            Self::X86_64 => ("BOOTX64.EFI", X86_64_EFI, X86_64_ELF),
        }
    }
    pub fn firmware(self) -> (&'static str, &'static str) {
        match self {
            Self::Aarch64 => ("edk2-aarch64-code.fd", "edk2-arm-vars.fd"),
            Self::Riscv64 => ("edk2-riscv-code.fd", "edk2-riscv-vars.fd"),
            Self::X86_64 => ("edk2-x86_64-code.fd", "edk2-i386-vars.fd"),
        }
    }
}
