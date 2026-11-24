#[repr(C)]
pub struct SporeExt2DirRecord {
    pub ino: u32,
    pub rec_len: u16,
    pub name_len: u8,
    pub kind: u8,
}

#[repr(C)] pub struct SporeExt2DirSlot { pub offset:u32, pub rec_len:u16, pub used:u16 }
#[repr(C)] pub struct SporeExt2DirMatch { pub offset:u32, pub prev_offset:u32, pub ino:u32, pub rec_len:u16 }
fn record(b:&[u8], off:usize)->Option<SporeExt2DirRecord>{if off+8>b.len(){return None}let ino=u32::from_le_bytes(b[off..off+4].try_into().ok()?);let r=u16::from_le_bytes(b[off+4..off+6].try_into().ok()?) as usize;let n=b[off+6]as usize;if r<8||r&3!=0||off+r>b.len()||(ino!=0&&(n==0||n+8>r)){return None}Some(SporeExt2DirRecord{ino,rec_len:r as u16,name_len:n as u8,kind:b[off+7]})}
#[unsafe(no_mangle)] pub unsafe extern "C" fn spore_ext2_find_dir_name(data:*const u8,len:usize,name:*const u8,name_len:usize,out:*mut SporeExt2DirMatch)->bool{if data.is_null()||name.is_null()||out.is_null()||name_len>255{return false}let b=unsafe{core::slice::from_raw_parts(data,len)};let n=unsafe{core::slice::from_raw_parts(name,name_len)};let(mut off,mut prev)=(0usize,u32::MAX);while off<len{let r=match record(b,off){Some(x)=>x,None=>return false};if r.ino!=0&&r.name_len as usize==name_len&&b[off+8..off+8+name_len]==*n{unsafe{*out=SporeExt2DirMatch{offset:off as u32,prev_offset:prev,ino:r.ino,rec_len:r.rec_len}};return true}prev=off as u32;off+=r.rec_len as usize}false}
#[unsafe(no_mangle)] pub unsafe extern "C" fn spore_ext2_find_dir_slot(data:*const u8,len:usize,need:u16,out:*mut SporeExt2DirSlot)->bool{if data.is_null()||out.is_null(){return false}let b=unsafe{core::slice::from_raw_parts(data,len)};let mut off=0;while off<len{let r=match record(b,off){Some(x)=>x,None=>return false};let used=if r.ino==0{0}else{((8+r.name_len as usize+3)&!3)as u16};if r.rec_len>=used.saturating_add(need){unsafe{*out=SporeExt2DirSlot{offset:off as u32,rec_len:r.rec_len,used}};return true}off+=r.rec_len as usize}false}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn spore_ext2_parse_dir_record(
    data: *const u8,
    available: usize,
    out: *mut SporeExt2DirRecord,
) -> bool {
    if data.is_null() || out.is_null() || available < 8 { return false; }
    let bytes = unsafe { core::slice::from_raw_parts(data, available) };
    let ino = u32::from_le_bytes([bytes[0], bytes[1], bytes[2], bytes[3]]);
    let rec_len = u16::from_le_bytes([bytes[4], bytes[5]]) as usize;
    let name_len = bytes[6] as usize;
    if rec_len < 8 || rec_len & 3 != 0 || rec_len > available || (ino != 0 && (name_len == 0 || name_len + 8 > rec_len)) {
        return false;
    }
    unsafe { *out = SporeExt2DirRecord { ino, rec_len: rec_len as u16, name_len: name_len as u8, kind: bytes[7] }; }
    true
}
