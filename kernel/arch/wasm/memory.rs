// Translations are valid until a syscall can change the guest's address space.
unsafe extern "C" {
    fn wasm_translate(page: u64, access: u32) -> *mut u8;
}

#[derive(Clone, Copy, PartialEq, Debug)]
#[repr(u32)]
pub enum Access {
    Read,
    Write,
    Execute,
}

#[derive(Clone, Copy)]
struct Translation {
    page: u64,
    access: Access,
    physical: *mut u8,
}

pub struct Memory {
    translations: [Option<Translation>; 64],
}

impl Memory {
    pub fn new() -> Self {
        Self {
            translations: [None; 64],
        }
    }
    pub fn invalidate(&mut self) {
        self.translations.fill(None);
    }

    fn pointer(&mut self, addr: u64, access: Access) -> Result<*mut u8, super::cpu::Fault> {
        let page = addr >> 12;
        let slot = &mut self.translations[page as usize % 64];
        let physical = match *slot {
            Some(t) if t.page == page && t.access == access => t.physical,
            _ => {
                let physical = unsafe { wasm_translate(page << 12, access as u32) };
                if physical.is_null() {
                    return Err(super::cpu::Fault::Memory(access, addr));
                }
                *slot = Some(Translation {
                    page,
                    access,
                    physical,
                });
                physical
            }
        };
        Ok(unsafe { physical.add(addr as usize & 4095) })
    }

    pub fn require_write(&mut self, addr: u64) -> Result<(), super::cpu::Fault> {
        self.pointer(addr, Access::Write).map(|_| ())
    }

    pub fn fetch(&mut self, pc: u64) -> Result<u32, super::cpu::Fault> {
        if pc & 3 != 0 {
            return Err(super::cpu::Fault::Alignment(pc));
        }
        let p = self.pointer(pc, Access::Execute)?;
        Ok(u32::from_le(unsafe { p.cast::<u32>().read_unaligned() }))
    }

    pub fn load(&mut self, addr: u64, size: usize) -> Result<u128, super::cpu::Fault> {
        let first = size.min(4096 - (addr as usize & 4095));
        let mut bytes = [0; 16];
        let p = self.pointer(addr, Access::Read)?;
        unsafe { std::ptr::copy_nonoverlapping(p, bytes.as_mut_ptr(), first) };
        if first < size {
            let next = self.pointer(addr.wrapping_add(first as u64), Access::Read)?;
            unsafe {
                std::ptr::copy_nonoverlapping(next, bytes[first..].as_mut_ptr(), size - first)
            };
        }
        Ok(u128::from_le_bytes(bytes))
    }

    pub fn store(&mut self, addr: u64, size: usize, value: u128) -> Result<(), super::cpu::Fault> {
        let bytes = value.to_le_bytes();
        let first = size.min(4096 - (addr as usize & 4095));
        let p = self.pointer(addr, Access::Write)?;
        if first < size {
            let next = self.pointer(addr.wrapping_add(first as u64), Access::Write)?;
            unsafe { std::ptr::copy_nonoverlapping(bytes[first..].as_ptr(), next, size - first) };
        }
        unsafe { std::ptr::copy_nonoverlapping(bytes.as_ptr(), p, first) };
        Ok(())
    }
}
