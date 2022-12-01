{
  description = "Flake utils demo";

  inputs.nixpkgs = {
    # url = "github:nixos/nixpkgs/17.03";
    url = "/home/josh/dev/nix-repl/nixpkgs-17.03";
    flake = false;
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let pkgs = import nixpkgs { inherit system; }; in
      {
        packages = rec {
          nix-repl = pkgs.stdenv.mkDerivation {
            name = "nix-repl";
            src = ./.;
            buildInputs = with pkgs; [ pkgconfig readline nixUnstable boehmgc ];
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
          default = nix-repl;
        };
      }
    );
}
