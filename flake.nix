{
  description = "provides the libvfn package for NixOS and Nix environments";
  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-24.05";
    unstable.url = "github:Joelgranados/nixpkgs/master";
  };

  outputs = { self, nixpkgs, unstable }:
    let
      libvfnVersion = "5.1.0";
      # lists supported systems
      allSystems = [ "x86_64-linux" "aarch64-linux" ];

      forAllSystems = fn:
        nixpkgs.lib.genAttrs allSystems
        (system: fn {
          pkgs = import nixpkgs { inherit system; };
          uPkgs = import unstable { inherit system; };
          });
    in {
      formatter = forAllSystems ({ pkgs }: pkgs.nixfmt);
      packages = forAllSystems ({ pkgs, uPkgs }: rec {
        libvfn = pkgs.stdenv.mkDerivation rec {
          pname = "libvfn";
          version = libvfnVersion;
          src = ./.;
          kernelHeaders = uPkgs.linuxHeaders;
          mesonFlags = [
            "-Ddocs=disabled"
            "-Dlibnvme=disabled"
            "-Dprofiling=false"
            "-Dlinux-headers=${kernelHeaders}/include/"
          ];
          nativeBuildInputs = with pkgs; [ meson ninja pkg-config perl ];
        };
        default = libvfn;
      });
    };
}
