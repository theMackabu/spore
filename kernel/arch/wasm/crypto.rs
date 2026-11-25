use super::cpu::{Cpu, Fault};
use aes::hazmat::{self, Block};

fn words(v: u128) -> [u32; 4] {
    std::array::from_fn(|i| (v >> (32 * i)) as u32)
}
fn pack(v: [u32; 4]) -> u128 {
    v.into_iter()
        .enumerate()
        .fold(0, |out, (i, x)| out | (u128::from(x) << (32 * i)))
}
fn sha1(mut state: [u32; 4], mut e: u32, wk: [u32; 4], op: u32) -> u128 {
    for k in wk {
        let [a, b, c, d] = state;
        let f = match op {
            0 => (b & c) ^ (!b & d),
            1 => b ^ c ^ d,
            _ => (b & c) | (d & (b | c)),
        };
        state = [
            a.rotate_left(5)
                .wrapping_add(f)
                .wrapping_add(e)
                .wrapping_add(k),
            a,
            b.rotate_left(30),
            c,
        ];
        e = d;
    }
    pack(state)
}
fn sha256(mut low: [u32; 4], mut high: [u32; 4], wk: [u32; 4], second: bool) -> u128 {
    for k in wk {
        let [a, b, c, d] = low;
        let [e, f, g, h] = high;
        let s1 = e.rotate_right(6) ^ e.rotate_right(11) ^ e.rotate_right(25);
        let s0 = a.rotate_right(2) ^ a.rotate_right(13) ^ a.rotate_right(22);
        let t = h
            .wrapping_add(s1)
            .wrapping_add((e & f) ^ (!e & g))
            .wrapping_add(k);
        low = [
            t.wrapping_add(s0).wrapping_add((a & b) ^ (a & c) ^ (b & c)),
            a,
            b,
            c,
        ];
        high = [d.wrapping_add(t), e, f, g];
    }
    pack(if second { high } else { low })
}

fn polynomial(mut a: u128, b: u64, bits: usize) -> u128 {
    let mut product = 0;
    for i in 0..bits {
        product ^= a & 0u128.wrapping_sub(u128::from((b >> i) & 1));
        a <<= 1;
    }
    product
}

const fn crc_table(poly: u32) -> [u32; 256] {
    let mut table = [0; 256];
    let mut i = 0;
    while i < 256 {
        let mut crc = i as u32;
        let mut j = 0;
        while j < 8 {
            crc = (crc >> 1) ^ (poly & 0u32.wrapping_sub(crc & 1));
            j += 1;
        }
        table[i] = crc;
        i += 1;
    }
    table
}
const CRC32: [u32; 256] = crc_table(0xedb88320);
const CRC32C: [u32; 256] = crc_table(0x82f63b78);

impl Cpu {
    pub fn crc(&mut self, w: u32) -> Result<(), Fault> {
        let size = 1 << ((w >> 10) & 3);
        if (size == 8) != (w >> 31 != 0) {
            return Err(Fault::Instruction(w));
        }
        let table = if w & 0x1000 == 0 { &CRC32 } else { &CRC32C };
        let mut crc = self.reg((w >> 5) & 31) as u32;
        let v = self.reg((w >> 16) & 31).to_le_bytes();
        for &x in &v[..size] {
            crc = (crc >> 8) ^ table[((crc as u8) ^ x) as usize];
        }
        self.set(w & 31, u64::from(crc));
        Ok(())
    }

    pub fn crypto(&mut self, w: u32) -> Result<(), Fault> {
        let vec = &mut self.context.v;
        let d = (w & 31) as usize;
        let n = vec[((w >> 5) & 31) as usize];
        let m = vec[((w >> 16) & 31) as usize];
        let old = vec[d];
        if w & 0xbfe0fc00 == 0x2e209c00 {
            let mut out = 0;
            for i in 0..if w & (1 << 30) != 0 { 16 } else { 8 } {
                out |= (polynomial((n >> (8 * i)) & 255, (m >> (8 * i)) as u8 as u64, 8) & 255)
                    << (8 * i);
            }
            vec[d] = out;
            return Ok(());
        }
        if w & 0xffffcc00 == 0x4e284800 {
            let op = (w >> 12) & 3;
            let mut block: Block = (if op < 2 { old ^ n } else { n }).to_le_bytes().into();
            match op {
                0 => {
                    hazmat::cipher_round(&mut block, &Block::default());
                    hazmat::inv_mix_columns(&mut block);
                }
                1 => {
                    hazmat::equiv_inv_cipher_round(&mut block, &Block::default());
                    hazmat::mix_columns(&mut block);
                }
                2 => hazmat::mix_columns(&mut block),
                _ => hazmat::inv_mix_columns(&mut block),
            }
            vec[d] = u128::from_le_bytes(block.into());
            return Ok(());
        }
        if w & 0xbf20fc00 == 0x0e20e000 {
            let upper = (w >> 30) & 1;
            let a = (n >> (upper * 64)) as u64;
            let b = (m >> (upper * 64)) as u64;
            vec[d] = match (w >> 22) & 3 {
                0 => (0..8).fold(0, |v, i| {
                    v | polynomial(u128::from((a >> (8 * i)) as u8), (b >> (8 * i)) & 255, 8)
                        << (16 * i)
                }),
                3 => polynomial(u128::from(a), b, 64),
                _ => return Err(Fault::Instruction(w)),
            };
            return Ok(());
        }
        let (a, b, c) = (words(old), words(n), words(m));
        vec[d] = match w & 0xffe0fc00 {
            0x5e000000 | 0x5e001000 | 0x5e002000 => sha1(a, b[0], c, (w >> 12) & 3),
            0x5e003000 => pack([
                a[0] ^ a[2] ^ c[0],
                a[1] ^ a[3] ^ c[1],
                a[2] ^ b[0] ^ c[2],
                a[3] ^ b[1] ^ c[3],
            ]),
            0x5e004000 => sha256(a, b, c, false),
            0x5e005000 => sha256(b, a, c, true),
            0x5e006000 => {
                let sigma = |x: u32| x.rotate_right(17) ^ x.rotate_right(19) ^ (x >> 10);
                let mut out = [0; 4];
                for i in 0..4 {
                    let prev = if i < 2 { c[i + 2] } else { out[i - 2] };
                    out[i] = a[i]
                        .wrapping_add(if i == 3 { c[0] } else { b[i + 1] })
                        .wrapping_add(sigma(prev));
                }
                pack(out)
            }
            _ => match w & 0xfffffc00 {
                0x5e280800 => u128::from(b[0].rotate_left(30)),
                0x5e281800 => {
                    let first = (a[0] ^ b[1]).rotate_left(1);
                    pack([
                        first,
                        (a[1] ^ b[2]).rotate_left(1),
                        (a[2] ^ b[3]).rotate_left(1),
                        (a[3] ^ first).rotate_left(1),
                    ])
                }
                0x5e282800 => pack(std::array::from_fn(|i| {
                    let x = if i == 3 { b[0] } else { a[i + 1] };
                    a[i].wrapping_add(x.rotate_right(7) ^ x.rotate_right(18) ^ (x >> 3))
                })),
                _ => return Err(Fault::Instruction(w)),
            },
        };
        Ok(())
    }
}
