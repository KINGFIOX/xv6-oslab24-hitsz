{
  description = "A flake to provide an environment for xv6 riscv";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = import nixpkgs { inherit system; };
        libPath = pkgs.lib.makeLibraryPath [ ]; # 外部库用在 nix 环境中
      in {
        devShell = pkgs.mkShell {
          buildInputs = with pkgs; [
            qemu
            cling
            (with pkgsCross.riscv64-embedded; [
              buildPackages.gcc
              buildPackages.gdb
            ])
          ];
          MAKEFLAGS = "-j$(nproc)";
        };
      });
}

