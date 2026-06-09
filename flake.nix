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
          clangWrapped = pkgs.llvmPackages.clang;
          clangUnwrapped = pkgs.llvmPackages.clang-unwrapped;
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
            clangWrapped
            clangUnwrapped
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
              env_id="spore-nix-env-v6-unwrapped-kernel-clang-shim"

              tools_dir="$PWD/.cache/spore-nix-tools"
              mkdir -p "$tools_dir"

              install_symlink() {
                link_path="$1"
                link_target="$2"
                if [ ! -L "$link_path" ] || [ "$(readlink "$link_path")" != "$link_target" ]; then
                  rm -f "$link_path"
                  ln -s "$link_target" "$link_path"
                fi
              }

              install_file_if_changed() {
                dst="$1"
                tmp="$2"
                if [ -f "$dst" ] && cmp -s "$tmp" "$dst"; then
                  rm -f "$tmp"
                  [ -x "$dst" ] || chmod +x "$dst"
                else
                  rm -f "$dst"
                  mv "$tmp" "$dst"
                  chmod +x "$dst"
                fi
              }

              clang_shim="$tools_dir/spore-clang"
              tmp="$(mktemp "$tools_dir/spore-clang.XXXXXX")"
              cat >"$tmp" <<EOF
#!/usr/bin/env bash
target_none_elf=false
args=()
for arg in "\$@"; do
  case "\$arg" in
    --target=aarch64-none-elf) target_none_elf=true ;;
    -fuse-ld=lld)
      args+=("-fuse-ld=${pkgs.llvmPackages.lld}/bin/ld.lld")
      continue
      ;;
  esac
  args+=("\$arg")
done
if [ "\$target_none_elf" = true ]; then
  exec "${clangUnwrapped}/bin/clang" "\''${args[@]}"
fi
exec "${clangWrapped}/bin/clang" "\''${args[@]}"
EOF
              install_file_if_changed "$clang_shim" "$tmp"
              install_symlink "$tools_dir/cc" "$clang_shim"
              install_symlink "$tools_dir/clang" "$clang_shim"

              gcc_fixed="$(echo ${crossGccUnwrapped}/lib/gcc/aarch64-unknown-linux-musl/*/include-fixed)"
              for tool in gcc g++ cpp; do
                wrapper="$tools_dir/aarch64-unknown-linux-musl-$tool"
                tmp="$(mktemp "$tools_dir/aarch64-unknown-linux-musl-$tool.XXXXXX")"
                cat >"$tmp" <<EOF
#!/bin/sh
exec "${crossGccUnwrapped}/bin/aarch64-unknown-linux-musl-$tool" \\
  -B"${crossLibc}/lib/" \\
  -idirafter "${crossLibcDev}/include" \\
  -idirafter "$gcc_fixed" \\
  -B"${crossGccLib}/aarch64-unknown-linux-musl/lib" \\
  "\$@"
EOF
                install_file_if_changed "$wrapper" "$tmp"
              done
              install_symlink "$tools_dir/aarch64-unknown-linux-musl-ar" "${crossBinutils}/bin/aarch64-unknown-linux-musl-ar"
              install_symlink "$tools_dir/aarch64-unknown-linux-musl-ranlib" "${crossBinutils}/bin/aarch64-unknown-linux-musl-ranlib"
              install_symlink "$tools_dir/aarch64-unknown-linux-musl-strip" "${crossBinutils}/bin/aarch64-unknown-linux-musl-strip"
              export PATH="$tools_dir:$PATH"
              export CC=clang

              git submodule update --init --recursive

              if [ -f "$build_dir/build.ninja" ] && { [ ! -f "$marker" ] || [ "$(cat "$marker")" != "$env_id" ]; }; then
                echo "reconfiguring $build_dir for the Nix toolchain"
                rm -rf "$build_dir"
              fi

              make BUILD_DIR="$build_dir" setup
              mkdir -p "$build_dir"
              printf '%s\n' "$env_id" >"$marker"

              qemu="$(command -v qemu-system-aarch64)"
              if [ -x /opt/homebrew/bin/qemu-system-aarch64 ]; then
                qemu=/opt/homebrew/bin/qemu-system-aarch64
              fi

              make BUILD_DIR="$build_dir" QEMU="$qemu" run
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
