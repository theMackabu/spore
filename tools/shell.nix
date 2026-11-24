{ pkgs }:

let
  rust = pkgs.rust-bin.stable."1.100.0".minimal.override {
    extensions = [ "rustfmt" ];
    targets = [ "aarch64-unknown-none-softfloat" "aarch64-unknown-linux-musl" "riscv64gc-unknown-none-elf" "x86_64-unknown-none" ];
  };
  musl = pkgs.pkgsCross.aarch64-multiplatform-musl;
  guestTools = pkgs.lib.makeBinPath [
    pkgs.llvmPackages.clang-unwrapped musl.stdenv.cc musl.buildPackages.binutils
  ];
in
pkgs.mkShellNoCC {
  packages = with pkgs; [
    rust cmake curl dosfstools e2fsprogs git gnumake libarchive mtools
    patch pkg-config python3 qemu zig zstd
    llvmPackages.clang llvmPackages.lld llvmPackages.llvm
  ];

  hardeningDisable = [ "all" ];
  shellHook = ''
    export PATH="${guestTools}:$PATH"
    export CC="${pkgs.llvmPackages.clang}/bin/clang"
    export CC_FOR_BUILD="$CC"
  '';
}
