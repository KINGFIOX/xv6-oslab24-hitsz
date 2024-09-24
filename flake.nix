{
  description = "A flake to provide an environment for xv6 riscv";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs";
    flake-utils.url = "github:numtide/flake-utils";
    rust-overlay = {
      url = "github:oxalica/rust-overlay";
      inputs.nixpkgs.follows = "nixpkgs";
    };
  };

  outputs = { self, nixpkgs, flake-utils, rust-overlay, ... }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        overlays = [ (import rust-overlay) ];
        pkgs = import nixpkgs { inherit system overlays; };
        libPath = pkgs.lib.makeLibraryPath [ ];
        toolchain = pkgs.rust-bin.fromRustupToolchainFile ./rust-toolchain.toml;
      in {
        devShell = pkgs.mkShell {
          buildInputs = with pkgs; [
            qemu
            cling
            (with pkgsCross.riscv64-embedded; [
              buildPackages.gcc
              buildPackages.gdb
            ])
            toolchain
            cargo-binutils
          ];
          MAKEFLAGS = "-j$(nproc)";
          RUST_SRC_PATH = "${toolchain}/lib/rustlib/src/rust/library";
          RUST_GDB = "riscv64-none-elf-gdb";
        };
      });
}

