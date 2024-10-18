{
  description = "A flake to provide an environment for xv6 riscv";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs";
    flake-utils.url = "github:numtide/flake-utils";
    qemu5.url = "github:NixOS/nixpkgs/c8dff328e51f62760bf646bc345e3aabcfd82046";
  };

  outputs = { self, nixpkgs, flake-utils, qemu5, ... }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = import nixpkgs { inherit system; };
        libPath = pkgs.lib.makeLibraryPath [ ];
        qemu-pinned = import qemu5 { inherit system; };
      in {
        devShell = pkgs.mkShell {
          buildInputs = with pkgs; [
            qemu-pinned.qemu
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

