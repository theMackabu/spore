use crate::{fixtures, modes};
use std::io::{self, Read, Write};
use std::process::{Command, Stdio};
use std::time::{Duration, Instant};

pub fn find_shell_prompt(data: &[u8]) -> Option<usize> {
    for i in 0..data.len().saturating_sub(1) {
        if !matches!(data[i], b'$' | b'#') || data[i + 1] != b' ' { continue; }
        let start = data[..i].iter().rposition(|c| matches!(c, b'\n' | b'\r')).map_or(0, |n| n + 1);
        let line = &data[start..i];
        if i > 0 && data[i - 1] == b' ' || line.windows(2).any(|p| p == b"@") && line.contains(&b':') || data[i.saturating_sub(160)..i].windows(11).any(|p| p == b"spore.local") { return Some(start); }
    }
    None
}

pub fn run(argv: Vec<String>, mode: &str, timings: bool) -> i32 {
    let _ = fixtures::start_udp(); let _ = fixtures::start_http();
    let mut child = match Command::new(&argv[0]).args(&argv[1..]).stdin(Stdio::piped()).stdout(Stdio::piped()).stderr(Stdio::piped()).spawn() { Ok(v)=>v, Err(e)=>{eprintln!("spore-run: {e}");return 1} };
    let mut input = child.stdin.take().unwrap(); let mut output = child.stdout.take().unwrap();
    let mut buf=Vec::new(); let start=Instant::now(); let deadline=if mode=="plain"{None}else{Some(Duration::from_secs(if mode=="shell"{300}else{120}))};
    let mut shell=modes::ShellCheck::new(); let mut bench=modes::Bench::default(); let mut rng=modes::Rng::default(); let mut stdin_sent=false;
    loop { let mut chunk=[0u8;4096]; match output.read(&mut chunk) { Ok(0)=>break, Ok(n)=>{buf.extend_from_slice(&chunk[..n]); print!("{}",String::from_utf8_lossy(&chunk[..n])); let _=io::stdout().flush(); if modes::answer_guest_stdin(&mut input,&buf).unwrap_or(false){stdin_sent=true} if mode=="shell"&&!shell.output(&mut input,&mut buf).unwrap_or(false){let _=child.kill();break} if find_shell_prompt(&buf).is_some(){if mode=="bench"{let _=bench.prompt(&mut input)}if mode=="rng"{let _=rng.prompt(&mut input,&buf)}}}, Err(_)=>break } if deadline.is_some_and(|d|start.elapsed()>d){let _=child.kill();return 124} }
    let rc=child.wait().ok().and_then(|s|s.code()).unwrap_or(128); if mode=="filter"||mode=="integration"{modes::print_guest_checks(&buf)} if mode=="integration"&&(!stdin_sent||!buf.windows(34).any(|p|p==b"[spore] integration regression: PASS")){1}else{rc}
}
