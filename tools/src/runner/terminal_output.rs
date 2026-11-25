use std::io::{self, Write};

// A UART read can end anywhere in a UTF-8 character or a cursor/colour sequence.
fn complete_prefix(bytes: &[u8]) -> usize {
    let tail = bytes.len().saturating_sub(129);
    if let Some(i) = bytes[tail..].iter().rposition(|&b| b == 0x1b) {
        let start = tail + i;
        let escape = &bytes[start + 1..];
        if escape.is_empty()
            || (escape.len() < 128
                && escape[0] == b'['
                && escape[1..].iter().all(|b| (0x20..=0x3f).contains(b)))
        {
            return start;
        }
    }
    let tail = bytes.len().saturating_sub(3);
    if let Some(i) = bytes[tail..].iter().rposition(|&b| b & 0xc0 != 0x80) {
        let start = tail + i;
        if bytes[start] >= 0xc2
            && std::str::from_utf8(&bytes[start..]).is_err_and(|err| err.error_len().is_none())
        {
            return start;
        }
    }
    bytes.len()
}

#[derive(Default)]
pub struct TerminalOutput {
    pending: Vec<u8>,
}

impl TerminalOutput {
    pub fn relay(&mut self, out: &mut impl Write, bytes: &[u8]) -> io::Result<()> {
        if self.pending.is_empty() {
            let end = complete_prefix(bytes);
            out.write_all(&bytes[..end])?;
            self.pending.extend_from_slice(&bytes[end..]);
        } else {
            self.pending.extend_from_slice(bytes);
            let end = complete_prefix(&self.pending);
            out.write_all(&self.pending[..end])?;
            self.pending.drain(..end);
        }
        Ok(())
    }

    pub fn finish(&mut self, out: &mut impl Write) -> io::Result<()> {
        out.write_all(&self.pending)?;
        self.pending.clear();
        Ok(())
    }
}
