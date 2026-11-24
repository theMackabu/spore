pub struct SerialWindow {
    buffer: Vec<u8>,
    limit: usize,
    fresh: usize,
}

impl SerialWindow {
    pub fn new(limit: usize) -> Self {
        Self {
            buffer: Vec::new(),
            limit,
            fresh: 0,
        }
    }

    pub fn bytes(&self) -> &[u8] {
        &self.buffer[self.buffer.len().saturating_sub(self.limit)..]
    }

    pub fn push(&mut self, data: &[u8]) {
        let data = &data[data.len().saturating_sub(self.limit)..];
        if self.buffer.len() + data.len() > self.limit * 2 {
            let drop = self.buffer.len() - (self.limit - data.len());
            self.buffer.drain(..drop);
        }
        self.buffer.extend_from_slice(data);
        self.fresh = data.len();
    }

    pub fn clear(&mut self) {
        self.buffer.clear();
        self.fresh = 0;
    }

    pub fn find_new(&self, needle: &[u8]) -> Option<usize> {
        let bytes = self.bytes();
        let start = bytes.len().saturating_sub(self.fresh + needle.len() - 1);
        bytes[start..]
            .windows(needle.len())
            .position(|w| w == needle)
            .map(|i| i + start)
    }

    pub fn prompt(&self) -> Option<usize> {
        let data = self.bytes();
        let from = data.len().saturating_sub(self.fresh + 1);
        for (i, window) in data[from..].windows(2).enumerate() {
            let i = from + i;
            if window != b"$ " && window != b"# " {
                continue;
            }
            let start = data[..i]
                .iter()
                .rposition(|&c| matches!(c, b'\r' | b'\n'))
                .map_or(0, |j| j + 1);
            let line = &data[start..i];
            if line.ends_with(b" ")
                || line
                    .iter()
                    .position(|&c| c == b'@')
                    .is_some_and(|j| line[j + 1..].contains(&b':'))
                || data[i.saturating_sub(160)..i]
                    .windows(b"spore.local".len())
                    .any(|w| w == b"spore.local")
            {
                return Some(start);
            }
        }
        None
    }
}
