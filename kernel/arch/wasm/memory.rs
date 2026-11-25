unsafe extern "C" {
    static wasm_tlb_generation: u64;
    fn wasm_lookup_page(page: u64, access: u32) -> *mut u8;
    fn wasm_translate(page: u64, access: u32) -> *mut u8;
}

#[derive(Clone, Copy, PartialEq, Debug)]
#[repr(u32)]
pub enum Access {
    Read,
    Write,
    Execute,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub(super) struct Translation {
    page: u64,
    physical: *mut u8,
}

const EMPTY: Translation = Translation {
    page: u64::MAX,
    physical: std::ptr::null_mut(),
};
pub const SLOTS: usize = 512;
const _: () = assert!(
    std::mem::size_of::<Translation>() == 16 && std::mem::offset_of!(Translation, physical) == 8
);

thread_local! {
    static CODE_EPOCH: std::cell::Cell<u64> = const { std::cell::Cell::new(1) };
}

pub fn code_epoch() -> u64 {
    CODE_EPOCH.get()
}

pub struct Memory {
    code_pages: Vec<u64>,
    populated: Vec<u16>,
    generation: u64,
    translations: [[Translation; SLOTS]; 3],
}
impl Memory {
    pub fn new() -> Self {
        Self {
            generation: 0,
            code_pages: Default::default(),
            populated: Vec::new(),
            translations: [[EMPTY; SLOTS]; 3],
        }
    }
    pub(super) fn table(&self) -> *const Translation {
        self.translations[0].as_ptr()
    }
    pub fn sync(&mut self) {
        CODE_EPOCH.set(code_epoch() + 1);
        self.sync_mappings();
    }
    fn sync_mappings(&mut self) {
        let generation = unsafe { (&raw const wasm_tlb_generation).read_volatile() };
        if self.generation != generation {
            CODE_EPOCH.set(code_epoch() + 1);
            if self.populated.len() <= 64 {
                for key in self.populated.drain(..) {
                    self.translations[key as usize / SLOTS][key as usize % SLOTS] = EMPTY;
                }
            } else {
                self.translations = [[EMPTY; SLOTS]; 3];
                self.populated.clear();
            }
            self.generation = generation;
        }
    }
    fn cache(&mut self, access: Access, page: u64, physical: *mut u8) {
        let index = page as usize % SLOTS;
        let slot = &mut self.translations[access as usize][index];
        if slot.physical.is_null() {
            self.populated
                .push((access as usize * SLOTS + index) as u16);
        }
        *slot = Translation { page, physical };
    }
    #[inline]
    fn pointer(&mut self, addr: u64, access: Access) -> Result<*mut u8, super::cpu::Fault> {
        let page = addr >> 12;
        let slot = page as usize % SLOTS;
        let cached = self.translations[access as usize][slot];
        let page_ptr = if cached.page == page {
            cached.physical
        } else {
            self.translate(page, addr, access)?
        };
        Ok(unsafe { page_ptr.add(addr as usize & 4095) })
    }
    #[cold]
    fn translate(
        &mut self,
        page: u64,
        addr: u64,
        access: Access,
    ) -> Result<*mut u8, super::cpu::Fault> {
        let physical = unsafe { wasm_translate(page << 12, access as u32) };
        self.sync_mappings();
        if physical.is_null() {
            return Err(super::cpu::Fault::Memory(access, addr));
        }
        let index = physical as usize >> 12;
        if access == Access::Write
            && self
                .code_pages
                .get(index / 64)
                .is_some_and(|bits| bits & (1 << (index % 64)) != 0)
        {
            CODE_EPOCH.set(code_epoch() + 1);
        } else {
            self.cache(access, page, physical);
        }
        Ok(physical)
    }

    pub fn require_write(&mut self, addr: u64) -> Result<(), super::cpu::Fault> {
        self.pointer(addr, Access::Write).map(|_| ())
    }

    pub fn trace_words(&mut self, pc: u64) -> Option<&[u32]> {
        if pc & 3 != 0 {
            return None;
        }
        let page = pc >> 12;
        let entry = self.translations[Access::Execute as usize][page as usize % SLOTS];
        let physical = if entry.page == page {
            entry.physical
        } else {
            let physical = unsafe { wasm_lookup_page(pc & !4095, Access::Execute as u32) };
            if physical.is_null() {
                return None;
            }
            self.cache(Access::Execute, page, physical);
            physical
        };
        let index = physical as usize >> 12;
        let bit = 1u64 << (index % 64);
        if self.code_pages.len() <= index / 64 {
            self.code_pages.resize(index / 64 + 1, 0);
        }
        if self.code_pages[index / 64] & bit == 0 {
            self.code_pages[index / 64] |= bit;
            for slot in &mut self.translations[Access::Write as usize] {
                if slot.physical == physical {
                    slot.page = EMPTY.page;
                }
            }
        }
        let off = pc as usize & 4095;
        Some(unsafe { std::slice::from_raw_parts(physical.add(off).cast(), (4096 - off) / 4) })
    }

    pub fn fetch(&mut self, pc: u64) -> Result<u32, super::cpu::Fault> {
        if pc & 3 != 0 {
            return Err(super::cpu::Fault::Alignment(pc));
        }
        let p = self.pointer(pc, Access::Execute)?;
        Ok(u32::from_le(unsafe { p.cast::<u32>().read_unaligned() }))
    }

    pub fn load(&mut self, addr: u64, size: usize) -> Result<u128, super::cpu::Fault> {
        let p = self.pointer(addr, Access::Read)?;
        if (addr as usize & 4095) + size <= 4096 {
            return Ok(unsafe {
                match size {
                    1 => u128::from(p.read()),
                    2 => u128::from(p.cast::<u16>().read_unaligned()),
                    4 => u128::from(p.cast::<u32>().read_unaligned()),
                    8 => u128::from(p.cast::<u64>().read_unaligned()),
                    16 => p.cast::<u128>().read_unaligned(),
                    _ => unreachable!(),
                }
            });
        }
        let first = 4096 - (addr as usize & 4095);
        let mut bytes = [0; 16];
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
        let mut p = self.pointer(addr, Access::Write)?;
        if (addr as usize & 4095) + size <= 4096 {
            unsafe {
                match size {
                    1 => p.write(value as u8),
                    2 => p.cast::<u16>().write_unaligned(value as u16),
                    4 => p.cast::<u32>().write_unaligned(value as u32),
                    8 => p.cast::<u64>().write_unaligned(value as u64),
                    16 => p.cast::<u128>().write_unaligned(value),
                    _ => unreachable!(),
                }
            }
            return Ok(());
        }
        let bytes = value.to_le_bytes();
        let first = 4096 - (addr as usize & 4095);
        if first < size {
            let generation = self.generation;
            let next = self.pointer(addr.wrapping_add(first as u64), Access::Write)?;
            if self.generation != generation {
                let page = unsafe { wasm_lookup_page(addr & !4095, Access::Write as u32) };
                if page.is_null() {
                    return Err(super::cpu::Fault::Memory(Access::Write, addr));
                }
                p = unsafe { page.add(addr as usize & 4095) };
            }
            unsafe { std::ptr::copy_nonoverlapping(bytes[first..].as_ptr(), next, size - first) };
        }
        unsafe { std::ptr::copy_nonoverlapping(bytes.as_ptr(), p, first) };
        Ok(())
    }
}
