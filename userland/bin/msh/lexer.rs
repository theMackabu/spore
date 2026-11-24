#![no_std]

#[path = "../../lib/spore/panic.rs"]
mod panic;

use core::ffi::{CStr, c_char, c_int};
use core::fmt::Write as _;
use core::{fmt, ptr};

const WORD_CAP: usize = 4096;

#[repr(i32)]
#[derive(Clone, Copy)]
enum Kind {
    Word = 1,
    Background,
    And,
    Or,
    Pipe,
    Semicolon,
    Output,
    Append,
    Input,
    IoNumber,
}

#[repr(C)]
pub struct Token {
    kind: c_int,
    literal: bool,
    after_alias_blank: bool,
    expanded_aliases: u64,
    text: [u8; WORD_CAP],
}

struct Word {
    bytes: [u8; WORD_CAP],
    len: usize,
}

impl Word {
    fn push(&mut self, text: &[u8]) -> Result<(), ()> {
        if text.len() >= WORD_CAP - self.len {
            return Err(());
        }
        self.bytes[self.len..self.len + text.len()].copy_from_slice(text);
        self.len += text.len();
        Ok(())
    }

    fn tilde(&mut self) -> Result<(), ()> {
        if self.bytes[0] != b'~' || (self.len > 1 && self.bytes[1] != b'/') {
            return Ok(());
        }
        let home = environment(c"HOME");
        let home = if home.is_empty() { b"/" } else { home };
        if home.len() >= WORD_CAP - (self.len - 1) {
            return Err(());
        }
        self.bytes.copy_within(1..self.len, home.len());
        self.bytes[..home.len()].copy_from_slice(home);
        self.len += home.len() - 1;
        Ok(())
    }
}

impl fmt::Write for Word {
    fn write_str(&mut self, s: &str) -> fmt::Result {
        self.push(s.as_bytes()).map_err(|_| fmt::Error)
    }
}

fn environment(name: &CStr) -> &[u8] {
    unsafe extern "C" {
        fn getenv(name: *const c_char) -> *const c_char;
    }
    let value = unsafe { getenv(name.as_ptr()) };
    if value.is_null() {
        b""
    } else {
        unsafe { CStr::from_ptr(value) }.to_bytes()
    }
}

fn variable(word: &mut Word, input: &mut &[u8], status: c_int, quoted: bool) -> Result<(), ()> {
    let s = *input;
    let (name, consumed) = match s.first() {
        Some(b'?') => (&s[..1], 1),
        Some(b'{') => {
            let n = s[1..].iter().take(63).take_while(|&&c| c != b'}').count();
            (
                &s[1..1 + n],
                1 + n + usize::from(s.get(1 + n) == Some(&b'}')),
            )
        }
        _ => {
            let n = s
                .iter()
                .take(63)
                .take_while(|&&c| c == b'_' || c.is_ascii_alphanumeric())
                .count();
            (&s[..n], n)
        }
    };
    *input = &s[consumed..];
    if consumed == 0 && !quoted {
        return word.push(b"$");
    }
    if name == b"?" {
        return write!(word, "{status}").map_err(|_| ());
    }
    let mut buf = [0u8; 64];
    buf[..name.len()].copy_from_slice(name);
    word.push(environment(
        CStr::from_bytes_with_nul(&buf[..=name.len()]).map_err(|_| ())?,
    ))
}

fn whitespace(c: u8) -> bool {
    matches!(c, b' ' | b'\t' | b'\r' | b'\n')
}
fn separator(c: u8) -> bool {
    whitespace(c) || matches!(c, b'&' | b'|' | b';' | b'<' | b'>')
}

fn operator(s: &[u8]) -> Option<(Kind, usize)> {
    Some(match s {
        [b'&', b'&', ..] => (Kind::And, 2),
        [b'|', b'|', ..] => (Kind::Or, 2),
        [b'>', b'>', ..] => (Kind::Append, 2),
        [b'&', ..] => (Kind::Background, 1),
        [b'|', ..] => (Kind::Pipe, 1),
        [b';', ..] => (Kind::Semicolon, 1),
        [b'>', ..] => (Kind::Output, 1),
        [b'<', ..] => (Kind::Input, 1),
        _ => return None,
    })
}

fn lex(
    mut s: &[u8],
    status: c_int,
    mut emit: impl FnMut(Kind, bool, &[u8]) -> Result<(), ()>,
) -> Result<(), ()> {
    while let Some(&c) = s.first() {
        if whitespace(c) {
            s = &s[1..];
            continue;
        }
        if c == b'#' {
            break;
        }
        if let Some((kind, n)) = operator(s) {
            emit(kind, false, b"")?;
            s = &s[n..];
            continue;
        }
        if c.is_ascii_digit() {
            let n = s.iter().take_while(|c| c.is_ascii_digit()).count();
            if matches!(s.get(n), Some(b'>' | b'<')) {
                if n >= 16 {
                    return Err(());
                }
                emit(Kind::IoNumber, false, &s[..n])?;
                s = &s[n..];
                continue;
            }
        }
        let mut word = Word {
            bytes: [0; WORD_CAP],
            len: 0,
        };
        let mut literal = true;
        let mut tilde = false;
        while let Some((&c, tail)) = s.split_first() {
            if separator(c) {
                break;
            }
            s = tail;
            match c {
                b'\'' | b'"' => {
                    literal = false;
                    while let Some((&ch, tail)) = s.split_first() {
                        s = tail;
                        if ch == c {
                            break;
                        }
                        if c == b'"' && ch == b'$' {
                            variable(&mut word, &mut s, status, true)?;
                        } else if c == b'"' && ch == b'\\' && !s.is_empty() {
                            word.push(&s[..1])?;
                            s = &s[1..];
                        } else {
                            word.push(core::slice::from_ref(&ch))?;
                        }
                    }
                }
                b'\\' if !s.is_empty() => {
                    literal = false;
                    word.push(&s[..1])?;
                    s = &s[1..];
                }
                b'$' => {
                    literal = false;
                    variable(&mut word, &mut s, status, false)?;
                }
                _ => {
                    tilde |= word.len == 0 && c == b'~';
                    word.push(core::slice::from_ref(&c))?;
                }
            }
        }
        if tilde {
            word.tilde()?;
        }
        emit(Kind::Word, literal && !tilde, &word.bytes[..word.len])?;
    }
    Ok(())
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn sh_tokenize(
    line: *const c_char,
    tokens: *mut Token,
    capacity: usize,
    count: *mut usize,
    status: c_int,
) -> c_int {
    if line.is_null() || count.is_null() || (tokens.is_null() && capacity != 0) {
        return -1;
    }
    let mut n = 0;
    let result = lex(
        unsafe { CStr::from_ptr(line) }.to_bytes(),
        status,
        |kind, literal, text| {
            if n >= capacity {
                return Err(());
            }
            unsafe {
                let token = tokens.add(n);
                ptr::addr_of_mut!((*token).kind).write(kind as c_int);
                ptr::addr_of_mut!((*token).literal).write(literal);
                ptr::addr_of_mut!((*token).after_alias_blank).write(false);
                ptr::addr_of_mut!((*token).expanded_aliases).write(0);
                let dest = ptr::addr_of_mut!((*token).text).cast::<u8>();
                ptr::copy_nonoverlapping(text.as_ptr(), dest, text.len());
                dest.add(text.len()).write(0);
            }
            n += 1;
            Ok(())
        },
    );
    unsafe {
        count.write(n);
    }
    if result.is_ok() { 0 } else { -1 }
}
