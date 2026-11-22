{ pkgs }:

let
  musl = pkgs.pkgsCross.aarch64-multiplatform-musl;
  guestTools = pkgs.lib.makeBinPath [
    pkgs.llvmPackages.clang-unwrapped musl.stdenv.cc musl.buildPackages.binutils
  ];
in
pkgs.mkShellNoCC {
  packages = with pkgs; [
    cmake dosfstools e2fsprogs git gnumake meson mtools ninja
    patch pkg-config python3 qemu zstd
    llvmPackages.clang llvmPackages.lld llvmPackages.llvm
  ];

  hardeningDisable = [ "all" ];
  shellHook = ''
    export PATH="${guestTools}:$PATH"
    export CC="${pkgs.llvmPackages.clang}/bin/clang"
    export CC_FOR_BUILD="$CC"
  '';
}
