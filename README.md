this is just an experiment to see if I can get nix-repl compiling with the latest version of nix.

nowadays nix repl is inside Nix, but it would be super helpful to have an example
program that uses the nix library in C++.

currently runs on nixpkgs 17.09 (nix 1.12pre5619_346aeee1).

generate compile flags
```sh
rm compile_flags.txt; nix build .#compile_flags -o compile_flags.txt
```