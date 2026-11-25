use super::cpu::{Cpu, Fault, sx};

impl Cpu {
    fn transfer_one(
        &mut self,
        reg: u32,
        addr: u64,
        size: usize,
        load: bool,
        vector: bool,
        sign_bits: u32,
    ) -> Result<(), Fault> {
        if load {
            let v = self.memory.load(addr, size)?;
            if vector {
                self.context.v[reg as usize] = v;
            } else {
                let mut v = v as u64;
                if sign_bits != 0 {
                    v = sx(v, size as u32 * 8);
                    if sign_bits == 32 {
                        v = v as u32 as u64;
                    }
                }
                self.set(reg, v);
            }
        } else {
            let v = if vector {
                self.context.v[reg as usize]
            } else {
                self.reg(reg) as u128
            };
            self.reservation = None;
            self.memory.store(addr, size, v)?;
        }
        Ok(())
    }

    fn structure(&mut self, w: u32, base: u64) -> Result<(), Fault> {
        let (registers, interleaved) = match (w >> 12) & 15 {
            7 => (1, false),
            10 => (2, false),
            6 => (3, false),
            2 => (4, false),
            8 => (2, true),
            4 => (3, true),
            0 => (4, true),
            _ => return Err(Fault::Instruction(w)),
        };
        let bytes = if w & (1 << 30) == 0 { 8 } else { 16 };
        let width = 1usize << ((w >> 10) & 3);
        let lanes = bytes / width;
        let post = w & (1 << 23) != 0;
        let m = (w >> 16) & 31;
        if (!post && m != 0) || (interleaved && bytes == 8 && width == 8) {
            return Err(Fault::Instruction(w));
        }
        let load = w & (1 << 22) != 0;
        let mut next = [0u128; 4];
        for (r, dst) in next[..registers].iter_mut().enumerate() {
            let reg = ((w as usize & 31) + r) % 32;
            for lane in 0..lanes {
                let offset = if interleaved {
                    lane * registers + r
                } else {
                    r * lanes + lane
                };
                let addr = base.wrapping_add((offset * width) as u64);
                let shift = lane * width * 8;
                if load {
                    *dst |= self.memory.load(addr, width)? << shift;
                } else {
                    self.reservation = None;
                    self.memory
                        .store(addr, width, self.context.v[reg] >> shift)?;
                }
            }
        }
        if load {
            for (r, v) in next[..registers].iter().enumerate() {
                self.context.v[((w as usize & 31) + r) % 32] = *v;
            }
        }
        if post {
            let delta = if m == 31 {
                (registers * bytes) as u64
            } else {
                self.reg(m)
            };
            self.set_base((w >> 5) & 31, base.wrapping_add(delta));
        }
        Ok(())
    }

    fn structure_lane(&mut self, w: u32, base: u64) -> Result<(), Fault> {
        let q = (w >> 30) & 1;
        let op = (w >> 14) & 3;
        let size = (w >> 10) & 3;
        let s = (w >> 12) & 1;
        let load = w & (1 << 22) != 0;
        let count = 1 + ((w >> 21) & 1) + ((w >> 12) & 2);
        let m = (w >> 16) & 31;
        let post = w & (1 << 23) != 0;
        if !post && m != 0 {
            return Err(Fault::Instruction(w));
        }
        let index = (q << 3) | (s << 2) | size;
        let (bytes, lane) = match op {
            0 => (1, index),
            1 if size & 1 == 0 => (2, index >> 1),
            2 if size == 0 => (4, index >> 2),
            2 if size == 1 && s == 0 => (8, q),
            3 if load && s == 0 => (1 << size, 0),
            _ => return Err(Fault::Instruction(w)),
        };
        let width = bytes * 8;
        let mask = (1u128 << width) - 1;
        let shift = lane * width;
        for r in 0..count {
            let reg = ((w + r) & 31) as usize;
            let addr = base.wrapping_add((r * bytes) as u64);
            let prior = self.context.v[reg];
            if load {
                let val = self.memory.load(addr, bytes as usize)?;
                self.context.v[reg] = if op == 3 {
                    (val * (u128::MAX / mask)) & if q != 0 { u128::MAX } else { u64::MAX as u128 }
                } else {
                    (prior & !(mask << shift)) | (val << shift)
                };
            } else {
                self.reservation = None;
                self.memory.store(addr, bytes as usize, prior >> shift)?;
            }
        }
        if post {
            let delta = if m == 31 {
                (count * bytes) as u64
            } else {
                self.reg(m)
            };
            self.set_base((w >> 5) & 31, base.wrapping_add(delta));
        }
        Ok(())
    }

    pub fn transfer(&mut self, w: u32) -> Result<(), Fault> {
        let (t, n) = (w & 31, (w >> 5) & 31);
        let base = self.base(n);
        if n == 31 && base & 15 != 0 {
            return Err(Fault::Alignment(base));
        }
        if w & 0xbf000000 == 0x0d000000 {
            return self.structure_lane(w, base);
        }
        if w & 0xbf200000 == 0x0c000000 {
            return self.structure(w, base);
        }
        let vector = w & (1 << 26) != 0;
        let scale = w >> 30;
        if w & 0x3f000000 == 0x08000000 || w & 0x3f200c00 == 0x38200000 {
            return self.atomic(w);
        }
        if w & 0x3a000000 == 0x28000000 {
            let mode = (w >> 23) & 3;
            let load = w & (1 << 22) != 0;
            let size = if vector && scale < 3 {
                4 << scale
            } else if !vector && scale == 2 {
                8
            } else if !vector && (scale == 0 || (scale == 1 && load && mode != 0)) {
                4
            } else {
                return Err(Fault::Instruction(w));
            };
            let off = sx(((w >> 15) & 127) as u64, 7).wrapping_mul(size as u64);
            let updated = base.wrapping_add(off);
            let addr = if mode == 1 { base } else { updated };
            let sign = if !vector && scale == 1 { 64 } else { 0 };
            self.transfer_one(t, addr, size, load, vector, sign)?;
            self.transfer_one(
                (w >> 10) & 31,
                addr.wrapping_add(size as u64),
                size,
                load,
                vector,
                sign,
            )?;
            if mode & 1 != 0 {
                self.set_base(n, updated);
            }
            return Ok(());
        }
        if w & 0x3a000000 != 0x38000000 {
            return Err(Fault::Instruction(w));
        }
        let op = (w >> 22) & 3;
        let wide = vector && op >= 2;
        if wide && scale != 0 || !vector && op >= 2 && (scale == 3 || (op == 3 && scale == 2)) {
            return Err(Fault::Instruction(w));
        }
        let log_size = if wide { 4 } else { scale };
        let size = 1usize << log_size;
        let mut writeback = false;
        let mut post = false;
        let off = if w & (1 << 24) != 0 {
            (((w >> 10) & 4095) as u64) << log_size
        } else if w & (1 << 21) != 0 {
            if (w >> 10) & 3 != 2 {
                return Err(Fault::Instruction(w));
            }
            let m = self.reg((w >> 16) & 31);
            let ext = match (w >> 13) & 7 {
                2 => m as u32 as u64,
                3 | 7 => m,
                6 => sx(m, 32),
                _ => return Err(Fault::Instruction(w)),
            };
            ext << (if w & (1 << 12) != 0 { log_size } else { 0 })
        } else {
            let mode = (w >> 10) & 3;
            if mode == 2 {
                return Err(Fault::Instruction(w));
            }
            writeback = mode != 0;
            post = mode == 1;
            sx(((w >> 12) & 511) as u64, 9)
        };
        let updated = base.wrapping_add(off);
        let sign = if !vector && op >= 2 {
            if op == 2 { 64 } else { 32 }
        } else {
            0
        };
        self.transfer_one(
            t,
            if post { base } else { updated },
            size,
            if vector { op & 1 != 0 } else { op != 0 },
            vector,
            sign,
        )?;
        if writeback {
            self.set_base(n, updated);
        }
        Ok(())
    }
}
