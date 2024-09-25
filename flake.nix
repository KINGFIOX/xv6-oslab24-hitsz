{
  description = "A flake to provide an environment for xv6 riscv";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils, ... }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = import nixpkgs { inherit system; };
        libPath = pkgs.lib.makeLibraryPath [ ];
      in {
        devShell = pkgs.mkShell {
          buildInputs = with pkgs; [
            qemu
            cling
            (with pkgsCross.riscv64-embedded; [
              buildPackages.gcc
              buildPackages.gdb
            ])
            libxml2
            pkg-config
            clang_16
            llvm_16
          ];
          MAKEFLAGS = "-j$(nproc)";
          RUST_GDB = "riscv64-none-elf-gdb";
        };
      });
}

