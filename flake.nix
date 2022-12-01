{
  description = "Flake utils demo";

  inputs.nixpkgs = {
    url = "github:nixos/nixpkgs/18.03";
    flake = false;
  };

  outputs = {
    self,
    nixpkgs,
    flake-utils,
  }:
    flake-utils.lib.eachDefaultSystem (
      system: let
        pkgs = import nixpkgs {inherit system;};
      in {
        packages = rec {
          nix-repl = pkgs.stdenv.mkDerivation {
            name = "nix-repl";
            src = ./.;
            buildInputs = with pkgs; [pkgconfig readline nixUnstable boehmgc];
            buildPhase = ''
              mkdir -p $out/bin
              g++ -O3 -Wall -std=c++14 \
                -o $out/bin/nix-repl $src/nix-repl.cc \
                $(pkg-config --cflags nix-main) \
                -lnixformat -lnixutil -lnixstore -lnixexpr -lnixmain -lreadline -lgc \
                -DNIX_VERSION=\"${(builtins.parseDrvName pkgs.nixUnstable.name).version}\"
            '';
            installPhase = "true";
          };
          compile_flags = pkgs.stdenv.mkDerivation {
            name = "nix-repl-src";
            src = ./.;
            buildInputs = with pkgs; [pkgconfig readline nixUnstable boehmgc];
            buildPhase = ''
              echo $NIX_CFLAGS_COMPILE $(pkg-config --cflags nix-main) \
                -DNIX_VERSION=\"${(builtins.parseDrvName pkgs.nixUnstable.name).version}\" \
                | tr " " "\n" > $out
            '';
            installPhase = ''

            '';
          };
          default = nix-repl;
        };
      }
    );
}
