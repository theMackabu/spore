use spore_build::*;
use std::{
    env, fs,
    path::{Path, PathBuf},
    process::Command,
};

const CC: &str = "aarch64-unknown-linux-musl-gcc";
const PROGRAMS: &[&str] = &[
    "bin/logger",
    "bin/myc",
    "bin/reboot",
    "bin/shutdown",
    "demos/cpuburn",
    "demos/escalate",
    "demos/inodes",
    "demos/memhog",
    "demos/peeker",
    "demos/signal-crash",
    "demos/spinner",
    "demos/writer",
    "libexec/spore-test/pthread-demo",
    "libexec/spore-test/udp-echo",
    "libexec/spore-test/udp-recv",
    "libexec/spore-test/udp-send",
    "libexec/spore-test/uds-client",
    "libexec/spore-test/uds-server",
];

fn cc() -> Command {
    let mut cmd = Command::new(CC);
    cmd.args([
        "-std=c23",
        "-D_GNU_SOURCE",
        "-O2",
        "-s",
        "-ffunction-sections",
        "-fdata-sections",
        "-Ilib/spore",
        "-Wl,--gc-sections,-z,max-page-size=4096",
    ]);
    cmd
}

fn link(out: &Path, sources: &[PathBuf], extras: &[PathBuf], static_link: bool) {
    fs::create_dir_all(out.parent().unwrap()).unwrap();
    let mut cmd = cc();
    if static_link {
        cmd.arg("-static");
    } else {
        cmd.args([
            "-Wl,-dynamic-linker,/lib/ld-musl-aarch64.so.1",
            "-Wl,-rpath,/lib",
        ]);
    }
    run(cmd.args(sources).args(extras).arg("-o").arg(out));
}

fn main() {
    let root = Path::new(env!("CARGO_MANIFEST_DIR")).parent().unwrap();
    let out = output_dir();
    let guest = out.join("userland");
    for src in [
        "build.rs",
        "bin",
        "demos",
        "etc",
        "lib",
        "libexec",
        "prebuilt",
        "usr",
        "tests",
        "third_party/apk-tools",
        "third_party/curl",
    ] {
        watch(Path::new(src));
    }
    println!("cargo:rerun-if-env-changed=PATH");
    fs::create_dir_all(guest.join("etc")).unwrap();
    let release = fs::read_to_string("etc/os-release.in")
        .unwrap()
        .replace("@SPORE_RELEASE@", env!("CARGO_PKG_VERSION"));
    fs::write(guest.join("etc/os-release"), release).unwrap();
    spore_packages::prepare_userland(&root.join(".cache/packages"), &guest.join("prebuilt"))
        .unwrap();
    let util = guest.join("spore.o");
    run(cc()
        .args(["-fPIC", "-c", "lib/spore/util.c", "-o"])
        .arg(&util));
    for program in PROGRAMS {
        let dir = Path::new(program);
        link(
            &guest.join("programs").join(dir.file_name().unwrap()),
            &[dir.join("main.c")],
            std::slice::from_ref(&util),
            false,
        );
    }
    for (name, rust) in [
        ("init", "units.rs"),
        ("msh", "lexer.rs"),
        ("dhcpc", "main.rs"),
    ] {
        let dir = Path::new("bin").join(name);
        let lib = guest.join(format!("lib{name}.a"));
        rust_archive(&dir.join(rust), &lib, Some("aarch64-unknown-linux-musl"));
        link(
            &guest.join(&dir).join(name),
            &files(&dir, &["c"]),
            &[util.clone(), lib],
            name == "init",
        );
    }
    let tls = guest.join("prebuilt/tls-sdk/usr");
    let trustd = guest.join("libexec/trustd/trustd");
    fs::create_dir_all(trustd.parent().unwrap()).unwrap();
    run(cc()
        .arg("-static")
        .arg("-I")
        .arg(tls.join("include"))
        .arg("libexec/trustd/main.c")
        .args(
            ["libmbedtls.a", "libmbedx509.a", "libtfpsacrypto.a"]
                .map(|name| tls.join("lib").join(name)),
        )
        .arg("-o")
        .arg(trustd));
    let suite = Path::new("tests/integration");
    let child = suite.join("exec_child.c");
    let sources: Vec<_> = files(suite, &["c"])
        .into_iter()
        .filter(|p| p != &child)
        .collect();
    link(
        &guest.join(suite).join("integration"),
        &sources,
        &[util],
        false,
    );
    link(&guest.join(suite).join("exec_child"), &[child], &[], false);
    for (port, name) in [("curl", "curl"), ("apk", "spm")] {
        let src = Path::new("usr/local/bin").join(port);
        let dest = guest.join(&src);
        run(Command::new(src.join("build.sh"))
            .arg(root)
            .arg(&dest)
            .arg(&out)
            .arg(dest.join(name)));
    }
}
