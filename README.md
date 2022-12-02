this is just an experiment to see if I can get nix-repl compiling with the latest version of nix.

nowadays nix repl is inside Nix, but it would be super helpful to have an example
program that uses the nix library in C++.

currently runs on nixpkgs 22.11 (nix 2.11.0).

the code doesn't have all the improvements made over the years to nix-repl but it compiles
and is a standalone program.

generate compile flags for clangd
```sh
rm compile_flags.txt; nix build .#compile_flags -o compile_flags.txt
```
