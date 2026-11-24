use super::{build_esp, build_image, gcc_file};
use std::{
    collections::{BTreeMap, BTreeSet},
    fs::{self, File},
    io::{self, BufWriter, Seek, Write},
    net::Ipv4Addr,
    path::{Path, PathBuf},
};

fn bad(s: impl Into<String>) -> io::Error {
    io::Error::other(s.into())
}
fn guest(s: &str) -> io::Result<String> {
    if !s.starts_with('/')
        || s.len() >= 256
        || s != "/" && s[1..].split('/').any(|p| matches!(p, "" | "." | ".."))
    {
        return Err(bad(format!("invalid guest path {s:?}")));
    }
    Ok(s.into())
}
struct Package {
    machine: u16,
    files: BTreeMap<String, PathBuf>,
    dirs: BTreeSet<String>,
    libs: Vec<PathBuf>,
    scanned: BTreeSet<String>,
}
impl Package {
    fn dir(&mut self, s: &str) -> io::Result<()> {
        guest(s)?;
        if s == "/" {
            return Ok(());
        }
        if self.files.contains_key(s)
            || matches!(
                s.split('/').nth(1),
                Some("dev" | "run" | "sys" | "tmp" | "proc" | "data")
            )
        {
            return Err(bad(format!("reserved or occupied path {s}")));
        }
        self.dir(Path::new(s).parent().unwrap().to_str().unwrap())?;
        self.dirs.insert(s.into());
        Ok(())
    }
    fn file(&mut self, src: &Path, dst: &str) -> io::Result<()> {
        guest(dst)?;
        if dst == "/"
            || self.dirs.contains(dst)
            || matches!(&dst[1..], "dev" | "run" | "sys" | "tmp" | "proc" | "data")
        {
            return Err(bad(format!("occupied path {dst}")));
        }
        self.dir(Path::new(dst).parent().unwrap().to_str().unwrap())?;
        let src = src
            .canonicalize()
            .map_err(|e| bad(format!("{}: {e}", src.display())))?;
        if !src.is_file() {
            return Err(bad(format!("expected a file: {}", src.display())));
        }
        if let Some(prev) = self.files.get(dst) {
            if prev != &src {
                return Err(bad(format!("conflicting sources for {dst}")));
            }
            return Ok(());
        }
        self.files.insert(dst.into(), src);
        Ok(())
    }
    fn library(&self, name: &str, extra: &[PathBuf]) -> io::Result<PathBuf> {
        self.libs
            .iter()
            .chain(extra)
            .map(|p| p.join(name))
            .find(|p| p.is_file())
            .or_else(|| {
                if self.machine != goblin::elf::header::EM_AARCH64 {
                    return None;
                }
                gcc_file(name)
                    .ok()
                    .filter(|p| p.is_absolute() && p.is_file())
            })
            .ok_or_else(|| bad(format!("missing guest library {name}")))
    }
    fn elf(&mut self, src: &Path, dst: &str) -> io::Result<()> {
        self.file(src, dst)?;
        if !self.scanned.insert(dst.into()) {
            return Ok(());
        }
        let bytes = fs::read(src)?;
        let elf =
            goblin::elf::Elf::parse(&bytes).map_err(|e| bad(format!("{}: {e}", src.display())))?;
        if !elf.is_64
            || !elf.little_endian
            || !matches!(elf.header.e_ident[7], 0 | 3 | 9)
            || elf.header.e_machine != self.machine
            || !matches!(
                elf.header.e_type,
                goblin::elf::header::ET_EXEC | goblin::elf::header::ET_DYN
            )
        {
            return Err(bad(format!(
                "unexpected ELF machine or executable type: {}",
                src.display()
            )));
        }
        let osabi = elf.header.e_ident[7];
        if elf.interpreter.is_some()
            && (osabi == goblin::elf::header::ELFOSABI_FREEBSD
                || osabi == 0
                    && elf
                        .iter_note_headers(&bytes)
                        .into_iter()
                        .flatten()
                        .map_while(Result::ok)
                        .any(|n| n.name == "FreeBSD" && n.n_type == 1 && n.desc.len() == 4))
        {
            return Err(bad("FreeBSD executables currently require static linking"));
        }
        let origin = src.canonicalize()?.parent().unwrap().to_owned();
        let mut search = Vec::new();
        for p in elf
            .runpaths
            .iter()
            .chain(&elf.rpaths)
            .flat_map(|p| p.split(':'))
        {
            if let Some(rel) = p
                .strip_prefix("${ORIGIN}")
                .or_else(|| p.strip_prefix("$ORIGIN"))
            {
                search.push(origin.join(rel.trim_start_matches('/')));
            }
        }
        search.push(origin);
        if let Some(interp) = elf.interpreter {
            let name = Path::new(interp)
                .file_name()
                .and_then(|s| s.to_str())
                .ok_or_else(|| bad("invalid interpreter"))?;
            let lib = self.library(name, &search).or_else(|error| {
                if name == "ld-musl-aarch64.so.1" {
                    self.library("libc.so", &search)
                } else {
                    Err(error)
                }
            })?;
            self.elf(&lib, interp)?;
        }
        for name in elf.libraries {
            if name.contains('/') {
                return Err(bad(format!("unsupported path in DT_NEEDED: {name}")));
            }
            let lib = self
                .library(name, &search)
                .map_err(|e| bad(format!("{} needs {name}: {e}", src.display())))?;
            self.elf(&lib, &format!("/lib/{name}"))?;
        }
        Ok(())
    }
}
fn string(out: &mut impl Write, s: &str) -> io::Result<()> {
    out.write_all(s.as_bytes())?;
    out.write_all(&[0])
}

pub fn payload(
    program: &Path,
    opts: &[PathBuf],
    default_port: Option<u16>,
    stage: &Path,
    bundle: &Path,
    machine: u16,
) -> io::Result<[Ipv4Addr; 4]> {
    let mut pkg = Package {
        machine,
        files: BTreeMap::new(),
        dirs: BTreeSet::new(),
        libs: Vec::new(),
        scanned: BTreeSet::new(),
    };
    let mut exe = String::from("/app/server");
    let mut cwd = String::from("/");
    let mut env = Vec::new();
    let mut argv = Vec::new();
    let mut argv0 = None;
    let mut resolver_requested = false;
    let mut net = [
        Ipv4Addr::new(10, 0, 2, 15),
        Ipv4Addr::new(10, 0, 2, 2),
        Ipv4Addr::new(255, 255, 255, 0),
        Ipv4Addr::new(10, 0, 2, 3),
    ];
    let mut iter = opts.iter();
    let utf = |p: &PathBuf| {
        p.to_str()
            .map(str::to_owned)
            .ok_or_else(|| bad("app options require UTF-8"))
    };
    while let Some(k) = iter.next() {
        if k.as_os_str() == "--" {
            for v in iter {
                argv.push(utf(v)?);
            }
            break;
        }
        let v = iter
            .next()
            .ok_or_else(|| bad(format!("missing value for {}", k.display())))?;
        match k.to_str() {
            Some("--exec") => exe = guest(&utf(v)?)?,
            Some("--cwd") => cwd = guest(&utf(v)?)?,
            Some("--argv0") => argv0 = Some(utf(v)?),
            Some("--env") => {
                let s = utf(v)?;
                if !s.split_once('=').is_some_and(|(k, _)| !k.is_empty()) {
                    return Err(bad("expected --env NAME=VALUE"));
                }
                env.push(s);
            }
            Some("--file") => {
                let s = utf(v)?;
                let (src, dst) = s
                    .rsplit_once('=')
                    .ok_or_else(|| bad("expected --file HOST=/GUEST"))?;
                pkg.file(Path::new(src), dst)?;
            }
            Some("--lib-dir") => pkg.libs.push(v.clone()),
            Some(k @ ("--ip" | "--gateway" | "--netmask" | "--dns")) => {
                resolver_requested |= k == "--dns";
                net[match k {
                    "--ip" => 0,
                    "--gateway" => 1,
                    "--netmask" => 2,
                    _ => 3,
                }] = utf(v)?.parse().map_err(bad_ip)?
            }
            _ => return Err(bad(format!("unknown app option {}", k.display()))),
        }
    }
    crate::app_network::user_network(net)?;
    if let Some(port) = default_port
        && !env.iter().any(|item| item.starts_with("PORT="))
    {
        env.push(format!("PORT={port}"));
    }
    argv.insert(0, argv0.unwrap_or_else(|| exe.clone()));
    if argv.len() > 128 || env.len() > 128 {
        return Err(bad("maximum 128 arguments and 128 environment entries"));
    }
    if !matches!(cwd.as_str(), "/tmp" | "/data" | "/run") {
        pkg.dir(&cwd)?;
    }
    pkg.elf(program, &exe)?;
    if resolver_requested && !pkg.files.contains_key("/etc/resolv.conf") {
        let resolv = stage.join("resolv.conf");
        fs::write(&resolv, format!("nameserver {}\n", net[3]))?;
        pkg.file(&resolv, "/etc/resolv.conf")?;
    }
    let mut out = BufWriter::new(File::create(bundle)?);
    out.write_all(b"SPOREAP2")?;
    for count in [argv.len(), env.len(), pkg.files.len() + pkg.dirs.len()] {
        out.write_all(
            &u32::try_from(count)
                .map_err(|_| bad("too many entries"))?
                .to_le_bytes(),
        )?;
    }
    for ip in net {
        out.write_all(&ip.octets())?;
    }
    for s in [&exe, &cwd].into_iter().chain(&argv).chain(&env) {
        string(&mut out, s)?;
    }
    for d in &pkg.dirs {
        string(&mut out, d)?;
        out.write_all(&u64::MAX.to_le_bytes())?;
    }
    let mut stored = BTreeMap::new();
    for (path, src) in &pkg.files {
        let mut f = File::open(src)?;
        let n = f.metadata()?.len();
        string(&mut out, path)?;
        if let Some(original) = stored.insert(src, path) {
            out.write_all(&(u64::MAX - 1).to_le_bytes())?;
            string(&mut out, original)?;
            continue;
        }
        out.write_all(&n.to_le_bytes())?;
        if n >= 4096 {
            let padding = out.stream_position()?.wrapping_neg() % 4096;
            out.write_all(&[0; 4096][..padding as usize])?;
        }
        if io::copy(&mut f, &mut out)? != n {
            return Err(bad(format!(
                "file changed while packaging: {}",
                src.display()
            )));
        }
    }
    out.flush()?;
    drop(out);
    Ok(net)
}

pub fn build(
    output: &Path,
    program: &Path,
    opts: &[PathBuf],
    default_port: Option<u16>,
    arch: crate::arch::Arch,
) -> io::Result<()> {
    build_image(output, |image| {
        let stage = image.with_extension("app-stage");
        fs::create_dir(&stage)?;
        let result = (|| {
            let bundle = stage.join("payload");
            let net = payload(program, opts, default_port, &stage, &bundle, arch.machine())?;
            let config = stage.join("limine.conf");
            fs::write(
                &config,
                format!(
                    "# spore-arch {}\n# spore-ipv4 {} {} {} {}\ntimeout: 0\ngraphics: no\n/Spore app\n    protocol: limine\n    path: boot():/boot/kernel.elf\n    module_path: boot():/boot/app.bin\n    module_string: /spore.app\n",
                    arch.name(),
                    net[0],
                    net[1],
                    net[2],
                    net[3]
                ),
            )?;
            build_esp(&config, image, Some(&bundle), arch)
        })();
        let cleanup = fs::remove_dir_all(stage);
        result.and(cleanup)
    })
}
fn bad_ip(e: std::net::AddrParseError) -> io::Error {
    bad(e.to_string())
}
