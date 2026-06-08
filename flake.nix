{
  description = "Spore build and VM runner";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";
  };

  outputs = { self, nixpkgs }:
    let
      systems = [
        "aarch64-darwin"
        "x86_64-darwin"
        "aarch64-linux"
        "x86_64-linux"
      ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});
    in
    {
      packages = forAllSystems (pkgs:
        let
          crossGcc = pkgs.pkgsCross.aarch64-multiplatform-musl.stdenv.cc;
          crossGccUnwrapped = crossGcc.cc;
          crossGccLib = crossGcc.cc.lib;
          crossLibc = crossGcc.libc;
          crossLibcDev = crossGcc.libc_dev;
          crossBinutils = pkgs.pkgsCross.aarch64-multiplatform-musl.buildPackages.binutils;
          python = pkgs.python3.withPackages (ps: [
            ps.jinja2
            ps.jsonschema
          ]);

          buildInputs = with pkgs; [
            autoconf
            automake
            cmake
            dosfstools
            e2fsprogs
            git
            gnumake
            groff
            libtool
            mtools
            ncurses
            ninja
            patch
            pkg-config
            qemu
            texinfo
            meson
            python
            llvmPackages.clang
            llvmPackages.lld
            llvmPackages.llvm
            crossGcc
            crossBinutils
          ];
        in
        {
          vm-runner = pkgs.writeShellApplication {
            name = "spore-vm";
            runtimeInputs = buildInputs;
            text = ''
              set -eu

              build_dir="''${SPORE_BUILD_DIR:-build}"
              marker="$build_dir/.spore-nix-env"
              env_id="spore-nix-env-v4-cross-gcc-shim"

              tools_dir="$(mktemp -d)"
              trap 'rm -rf "$tools_dir"' EXIT INT TERM
              ln -s "${pkgs.llvmPackages.clang}/bin/clang" "$tools_dir/cc"
              ln -s "${pkgs.llvmPackages.clang}/bin/clang" "$tools_dir/clang"
              gcc_fixed="$(echo ${crossGccUnwrapped}/lib/gcc/aarch64-unknown-linux-musl/*/include-fixed)"
              for tool in gcc g++ cpp; do
                cat >"$tools_dir/aarch64-unknown-linux-musl-$tool" <<EOF
#!/bin/sh
exec "${crossGccUnwrapped}/bin/aarch64-unknown-linux-musl-$tool" \\
  -B"${crossLibc}/lib/" \\
  -idirafter "${crossLibcDev}/include" \\
  -idirafter "$gcc_fixed" \\
  -B"${crossGccLib}/aarch64-unknown-linux-musl/lib" \\
  "\$@"
EOF
                chmod +x "$tools_dir/aarch64-unknown-linux-musl-$tool"
              done
              ln -s "${crossBinutils}/bin/aarch64-unknown-linux-musl-ar" "$tools_dir/aarch64-unknown-linux-musl-ar"
              ln -s "${crossBinutils}/bin/aarch64-unknown-linux-musl-ranlib" "$tools_dir/aarch64-unknown-linux-musl-ranlib"
              ln -s "${crossBinutils}/bin/aarch64-unknown-linux-musl-strip" "$tools_dir/aarch64-unknown-linux-musl-strip"
              export PATH="$tools_dir:$PATH"
              export CC=clang

              git submodule update --init --recursive

              if [ -f "$build_dir/build.ninja" ] && { [ ! -f "$marker" ] || [ "$(cat "$marker")" != "$env_id" ]; }; then
                echo "reconfiguring $build_dir for the Nix toolchain"
                rm -rf "$build_dir"
              fi

              mkdir -p "$build_dir"
              printf '%s\n' "$env_id" >"$marker"

              qemu="$(command -v qemu-system-aarch64)"
              if [ -x /opt/homebrew/bin/qemu-system-aarch64 ]; then
                qemu=/opt/homebrew/bin/qemu-system-aarch64
              fi

              make BUILD_DIR="$build_dir" QEMU="$qemu" run-reset
            '';
          };
        });

      apps = forAllSystems (pkgs:
        let
          runVm = self.packages.${pkgs.stdenv.hostPlatform.system}.vm-runner;
        in
        {
          default = {
            type = "app";
            program = "${runVm}/bin/spore-vm";
          };
          vm = {
            type = "app";
            program = "${runVm}/bin/spore-vm";
          };
        });

      devShells = forAllSystems (pkgs:
        let
          crossGcc = pkgs.pkgsCross.aarch64-multiplatform-musl.stdenv.cc;
          crossBinutils = pkgs.pkgsCross.aarch64-multiplatform-musl.buildPackages.binutils;
          python = pkgs.python3.withPackages (ps: [
            ps.jinja2
            ps.jsonschema
          ]);
        in
        {
          default = pkgs.mkShell {
            packages = with pkgs; [
              autoconf
              automake
              cmake
              dosfstools
              e2fsprogs
              git
              gnumake
              groff
              libtool
              mtools
              ncurses
              ninja
              patch
              pkg-config
              qemu
              texinfo
              meson
              python
              llvmPackages.clang
              llvmPackages.lld
              llvmPackages.llvm
              crossGcc
              crossBinutils
            ];

            shellHook = ''
              export CC=clang
              export NIX_HARDENING_DISABLE=all
            '';
          };
        });
    };
}
