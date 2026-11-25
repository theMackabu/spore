use super::cpu::{Cpu, Fault};

impl Cpu {
    pub fn atomic(&mut self, w: u32) -> Result<(), Fault> {
        let base = self.base((w >> 5) & 31);
        let t = w & 31;
        let s = (w >> 16) & 31;
        let scale = w >> 30;
        let size = 1usize << scale;
        if w & 0x3f200c00 == 0x38200000 {
            let op = (w >> 12) & 15;
            if op > 8 {
                return Err(Fault::Instruction(w));
            }
            Self::aligned(base, size)?;
            let mask = u64::MAX >> (64 - size * 8);
            let arg = self.reg(s) & mask;
            let old = self.memory.load(base, size)? as u64;
            let sign = 1 << (size * 8 - 1);
            let new = match op {
                0 => old.wrapping_add(arg),
                1 => old & !arg,
                2 => old ^ arg,
                3 => old | arg,
                4 => (old ^ sign).max(arg ^ sign) ^ sign,
                5 => (old ^ sign).min(arg ^ sign) ^ sign,
                6 => old.max(arg),
                7 => old.min(arg),
                _ => arg,
            };
            self.memory.store(base, size, u128::from(new))?;
            self.reservation = None;
            self.set(t, old);
            return Ok(());
        }
        let pair = w & (1 << 21) != 0;
        let ordered = w & (1 << 23) != 0;
        let load = w & (1 << 22) != 0;
        let t2 = (w >> 10) & 31;
        if pair && (ordered || scale < 2) {
            if t2 != 31 {
                return Err(Fault::Instruction(w));
            }
            let two = !ordered;
            if two && (s | t) & 1 != 0 {
                return Err(Fault::Instruction(w));
            }
            let elem = if two { size * 4 } else { size };
            let total = if two { elem * 2 } else { elem };
            Self::aligned(base, total)?;
            let mask = (1u128 << (elem * 8)) - 1;
            let operand = |reg| {
                let low = u128::from(self.reg(reg)) & mask;
                if two {
                    low | ((u128::from(self.reg(reg + 1)) & mask) << (elem * 8))
                } else {
                    low
                }
            };
            let expected = operand(s);
            let replacement = operand(t);
            self.memory.require_write(base)?;
            let old = self.memory.load(base, total)?;
            if old == expected {
                self.memory.store(base, total, replacement)?;
            }
            self.reservation = None;
            self.set(s, (old & mask) as u64);
            if two {
                self.set(s + 1, (old >> (elem * 8)) as u64);
            }
            return Ok(());
        }
        if (!pair && t2 != 31) || ((load || ordered) && s != 31) || (ordered && w & (1 << 15) == 0)
        {
            return Err(Fault::Instruction(w));
        }
        let total = if pair { size * 2 } else { size };
        Self::aligned(base, total)?;
        if load {
            let value = self.memory.load(base, total)?;
            let mask = (1u128 << (size * 8)) - 1;
            self.set(t, (value & mask) as u64);
            if pair {
                self.set(t2, (value >> (size * 8)) as u64);
            }
            if !ordered {
                self.reservation = Some((base, total));
            }
        } else {
            let reserved = self.reservation.take() == Some((base, total));
            if ordered || reserved {
                let mask = (1u128 << (size * 8)) - 1;
                let mut value = u128::from(self.reg(t)) & mask;
                if pair {
                    value |= u128::from(self.reg(t2)) << (size * 8);
                }
                self.memory.store(base, total, value)?;
            }
            if !ordered {
                self.set(s, u64::from(!reserved));
            }
        }
        Ok(())
    }

    fn aligned(addr: u64, size: usize) -> Result<(), Fault> {
        if addr & (size as u64 - 1) == 0 {
            Ok(())
        } else {
            Err(Fault::Alignment(addr))
        }
    }
}
