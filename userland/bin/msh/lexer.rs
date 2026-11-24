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
    text: *mut c_char,
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

fn variable_name(s: &[u8]) -> (&[u8], usize) {
    match s.first() {
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
    }
}

fn expand_variable(word: &mut Word, name: &[u8], status: c_int) -> Result<(), ()> {
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

enum Piece<'a> {
    Text(&'a [u8]),
    Variable(&'a [u8]),
    Tilde,
}

fn walk_word<'a>(
    input: &'a [u8],
    mut emit: impl FnMut(Piece<'a>) -> Result<(), ()>,
) -> Result<(usize, bool), ()> {
    let mut i = 0;
    let mut start = 0;
    let mut quote = 0;
    let mut literal = true;
    while let Some(&c) = input.get(i) {
        if quote == 0 && separator(c) {
            break;
        }
        match c {
            b'\'' | b'"' if quote == 0 || quote == c => {
                emit(Piece::Text(&input[start..i]))?;
                quote = if quote == 0 { c } else { 0 };
                literal = false;
            }
            b'\\' if quote != b'\'' && i + 1 < input.len() => {
                emit(Piece::Text(&input[start..i]))?;
                i += 1;
                start = i;
                i += 1;
                literal = false;
                continue;
            }
            b'$' if quote != b'\'' => {
                emit(Piece::Text(&input[start..i]))?;
                let (name, consumed) = variable_name(&input[i + 1..]);
                if consumed == 0 && quote == 0 {
                    emit(Piece::Text(&input[i..i + 1]))?;
                } else {
                    emit(Piece::Variable(name))?;
                }
                i += consumed;
                literal = false;
            }
            b'~' if quote == 0 => {
                emit(Piece::Text(&input[start..i]))?;
                emit(Piece::Tilde)?;
                if i == 0 {
                    literal = false;
                }
            }
            _ => {
                i += 1;
                continue;
            }
        }
        i += 1;
        start = i;
    }
    emit(Piece::Text(&input[start..i]))?;
    Ok((i, literal))
}

fn lex(mut s: &[u8], mut emit: impl FnMut(Kind, bool, &[u8]) -> Result<(), ()>) -> Result<(), ()> {
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
        let (len, literal) = walk_word(s, |_| Ok(()))?;
        if literal && len >= WORD_CAP {
            return Err(());
        }
        emit(Kind::Word, literal, &s[..len])?;
        s = &s[len..];
    }
    Ok(())
}

unsafe extern "C" {
    fn malloc(size: usize) -> *mut c_char;
    fn free(ptr: *mut c_char);
}

fn copy_word(bytes: &[u8]) -> Result<*mut c_char, ()> {
    let dest = unsafe { malloc(bytes.len() + 1) };
    if dest.is_null() {
        return Err(());
    }
    unsafe {
        ptr::copy_nonoverlapping(bytes.as_ptr(), dest.cast(), bytes.len());
        dest.add(bytes.len()).write(0);
    }
    Ok(dest)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn sh_tokenize(
    line: *const c_char,
    tokens: *mut Token,
    capacity: usize,
    count: *mut usize,
) -> c_int {
    let mut n = 0;
    let result = lex(
        unsafe { CStr::from_ptr(line) }.to_bytes(),
        |kind, literal, raw| {
            if n >= capacity {
                return Err(());
            }
            let text = if matches!(kind, Kind::Word | Kind::IoNumber) {
                copy_word(raw)?
            } else {
                ptr::null_mut()
            };
            unsafe {
                tokens.add(n).write(Token {
                    kind: kind as c_int,
                    literal,
                    after_alias_blank: false,
                    expanded_aliases: 0,
                    text,
                });
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

fn expand(raw: &[u8], status: c_int) -> Result<*mut c_char, ()> {
    let mut word = Word {
        bytes: [0; WORD_CAP],
        len: 0,
    };
    let mut tilde = false;
    walk_word(raw, |piece| match piece {
        Piece::Text(bytes) => word.push(bytes),
        Piece::Variable(name) => expand_variable(&mut word, name, status),
        Piece::Tilde => {
            tilde |= word.len == 0;
            word.push(b"~")
        }
    })?;
    if tilde {
        word.tilde()?;
    }
    copy_word(&word.bytes[..word.len])
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn sh_expand_word(token: &mut Token, status: c_int) -> c_int {
    if token.literal || token.kind != Kind::Word as c_int {
        return 0;
    }
    let raw = unsafe { CStr::from_ptr(token.text) }.to_bytes();
    let Ok(text) = expand(raw, status) else {
        return -1;
    };
    unsafe {
        free(token.text);
    }
    token.text = text;
    0
}
