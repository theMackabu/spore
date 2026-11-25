#![cfg(target_arch = "wasm32")]

#[path = "../../src/storage/merge.rs"]
mod merge;
#[path = "../../src/storage/packages.rs"]
mod packages;
#[path = "../../src/storage/volume.rs"]
mod volume;

use std::alloc::{Layout, alloc_zeroed, dealloc};
use std::ffi::c_void;

#[link(wasm_import_module = "storage")]
unsafe extern "C" {
    fn message(ptr: *const u8, len: usize);
}

pub fn report(text: &str) {
    unsafe {
        message(text.as_ptr(), text.len());
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn upgrade() -> bool {
    let apply = || {
        let old = volume::Volume::device(0, false)?;
        let new = volume::Volume::device(1, false)?;
        let local = volume::Volume::device(2, true)?;
        merge::merge_volumes(Some(&old), &new, &local)
    };
    match apply() {
        Ok(()) => true,
        Err(err) => {
            report(&err.to_string());
            false
        }
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn table_alloc(count: usize, size: usize) -> *mut c_void {
    let Some(layout) = count
        .checked_mul(size)
        .filter(|&n| n != 0)
        .and_then(|n| Layout::from_size_align(n, 16).ok())
    else {
        return std::ptr::null_mut();
    };
    unsafe { alloc_zeroed(layout).cast() }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn table_free(ptr: *mut u8, count: usize, size: usize) {
    if !ptr.is_null() {
        unsafe { dealloc(ptr, Layout::from_size_align(count * size, 16).unwrap()) }
    }
}
