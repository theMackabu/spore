#![no_std]

use core::ffi::{c_char, c_int, c_void};
use core::panic::PanicInfo;

unsafe extern "C" {
    fn mycelium_unit_apply_key(unit: *mut c_void, section: *const c_char, key: *const c_char, value: *const c_char);
}

unsafe fn len(mut p: *const u8) -> usize { let mut n=0; while *p != 0 { n+=1; p=p.add(1); } n }
unsafe fn trim(mut p: *const u8, mut n: usize) -> (*const u8, usize) { while n>0 && (*p==b' '||*p==b'\t'||*p==b'\r'||*p==b'\n'){p=p.add(1);n-=1;} while n>0 {let c=*p.add(n-1);if c==b' '||c==b'\t'||c==b'\r'||c==b'\n'{n-=1}else{break}} (p,n) }

#[panic_handler]
fn panic(_: &PanicInfo) -> ! { loop {} }

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mycelium_parse_unit_line(unit: *mut c_void, section: *mut c_char, section_cap: usize, line: *const c_char) -> c_int {
    if unit.is_null() || section.is_null() || section_cap == 0 || line.is_null() { return 0; }
    let (p,n)=trim(line as *const u8,len(line as *const u8));
    if n==0 || *p==b'#' { return 1; }
    if *p==b'[' { let mut end=1; while end<n && *p.add(end)!=b']' {end+=1;} if end==n {return 1;} let (s,l)=trim(p.add(1),end-1); let out=section as *mut u8; let copy=if l+1<section_cap {l}else{section_cap-1}; core::ptr::copy_nonoverlapping(s,out,copy);*out.add(copy)=0; return 1; }
    let mut eq=0; while eq<n && *p.add(eq)!=b'=' {eq+=1;} if eq==n{return 1;}
    let (key,klen)=trim(p,eq); let (value,vlen)=trim(p.add(eq+1),n-eq-1); if klen==0{return 1;}
    let mut k=[0u8; 256]; let mut v=[0u8; 512]; let kc=if klen<255{klen}else{255}; let vc=if vlen<511{vlen}else{511}; core::ptr::copy_nonoverlapping(key,k.as_mut_ptr(),kc); core::ptr::copy_nonoverlapping(value,v.as_mut_ptr(),vc); mycelium_unit_apply_key(unit,section,k.as_ptr() as *const c_char,v.as_ptr() as *const c_char); 1
}
