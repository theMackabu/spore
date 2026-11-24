#[derive(Clone, Copy)]
struct Record { offset: usize, ino: u32, rec_len: u16, name_len: u8, kind: u8 }
struct Records<'a> { bytes: &'a [u8], offset: usize }
impl<'a> Records<'a> {
    fn new(bytes: &'a [u8]) -> Self { Self { bytes, offset: 0 } }
    fn next(&mut self) -> Result<Option<Record>, ()> {
        if self.offset == self.bytes.len() { return Ok(None); }
        if self.bytes.len() - self.offset < 8 { return Err(()); }
        let b = &self.bytes[self.offset..];
        let ino = u32::from_le_bytes(b[..4].try_into().map_err(|_| ())?);
        let rec_len = u16::from_le_bytes(b[4..6].try_into().map_err(|_| ())?);
        let name_len = b[6]; let size = rec_len as usize;
        if size < 8 || size & 3 != 0 || size > b.len() || (ino != 0 && (name_len == 0 || name_len as usize + 8 > size)) { return Err(()); }
        let r = Record { offset: self.offset, ino, rec_len, name_len, kind: b[7] };
        self.offset += size; Ok(Some(r))
    }
    fn name(&self, r: Record) -> &'a [u8] { if r.ino == 0 { &[] } else { &self.bytes[r.offset + 8..r.offset + 8 + r.name_len as usize] } }
}
#[repr(C)] pub struct SporeExt2DirSlot { pub offset:u32, pub rec_len:u16, pub used:u16 }
#[repr(C)] pub struct SporeExt2DirMatch { pub offset:u32, pub prev_offset:u32, pub ino:u32, pub rec_len:u16 }
#[repr(C)] pub struct SporeExt2DirNext { pub consumed:u32, pub ino:u32, pub name_offset:u32, pub name_len:u8, pub kind:u8, pub found:bool }
unsafe fn slice<'a>(p:*const u8,n:usize)->Option<&'a[u8]>{if p.is_null(){None}else{Some(unsafe{core::slice::from_raw_parts(p,n)})}}
#[unsafe(no_mangle)] pub unsafe extern "C" fn spore_ext2_find_dir_name(data:*const u8,len:usize,name:*const u8,name_len:usize,out:*mut SporeExt2DirMatch)->i32{if name.is_null()||out.is_null()||name_len>255{return -1}let b=match unsafe{slice(data,len)}{Some(x)=>x,None=>return -1};let n=unsafe{core::slice::from_raw_parts(name,name_len)};let mut it=Records::new(b);let mut prev=u32::MAX;loop{let r=match it.next(){Ok(Some(x))=>x,Ok(None)=>return 0,Err(_)=>return -1};if r.ino!=0&&it.name(r)==n{unsafe{*out=SporeExt2DirMatch{offset:r.offset as u32,prev_offset:prev,ino:r.ino,rec_len:r.rec_len}};return 1}prev=r.offset as u32}}
#[unsafe(no_mangle)] pub unsafe extern "C" fn spore_ext2_find_dir_slot(data:*const u8,len:usize,need:u16,out:*mut SporeExt2DirSlot)->i32{if out.is_null(){return -1}let b=match unsafe{slice(data,len)}{Some(x)=>x,None=>return -1};let mut it=Records::new(b);loop{let r=match it.next(){Ok(Some(x))=>x,Ok(None)=>return 0,Err(_)=>return -1};let used=if r.ino==0{0}else{(8+r.name_len as u16+3)&!3};if r.rec_len>=used.saturating_add(need){unsafe{*out=SporeExt2DirSlot{offset:r.offset as u32,rec_len:r.rec_len,used}};return 1}}}
#[unsafe(no_mangle)] pub unsafe extern "C" fn spore_ext2_next_visible_dirent(data:*const u8,len:usize,hidden:bool,out:*mut SporeExt2DirNext)->i32{if out.is_null(){return -1}let b=match unsafe{slice(data,len)}{Some(x)=>x,None=>return -1};let mut it=Records::new(b);loop{let r=match it.next(){Ok(Some(x))=>x,Ok(None)=>{unsafe{*out=SporeExt2DirNext{consumed:it.offset as u32,ino:0,name_offset:0,name_len:0,kind:0,found:false}};return 0},Err(_)=>return -1};let n=it.name(r);if r.ino==0||n==b"."||n==b".."||(hidden&&n==b".spore-inodes"){continue}unsafe{*out=SporeExt2DirNext{consumed:it.offset as u32,ino:r.ino,name_offset:(r.offset+8)as u32,name_len:r.name_len,kind:r.kind,found:true}};return 1}}
