{
  description = "Spore development shell";
  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";

  inputs.rust-overlay.url = "github:oxalica/rust-overlay";
  inputs.rust-overlay.inputs.nixpkgs.follows = "nixpkgs";

  outputs = { nixpkgs, rust-overlay, ... }: {
    devShells = nixpkgs.lib.genAttrs [
      "aarch64-darwin" "x86_64-darwin" "aarch64-linux" "x86_64-linux"
    ] (system: {
      default = import ./tools/shell.nix { pkgs = nixpkgs.legacyPackages.${system}.extend rust-overlay.overlays.default; };
    });
  };
}
