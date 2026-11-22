{
  description = "Spore development shell";
  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";

  outputs = { nixpkgs, ... }: {
    devShells = nixpkgs.lib.genAttrs [
      "aarch64-darwin" "x86_64-darwin" "aarch64-linux" "x86_64-linux"
    ] (system: {
      default = import ./tools/shell.nix { pkgs = nixpkgs.legacyPackages.${system}; };
    });
  };
}
