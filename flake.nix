{
  description = "tgtk — GTK4 Telegram client on TDLib";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-parts.url = "github:hercules-ci/flake-parts";
  };

  outputs = inputs@{ flake-parts, ... }:
    flake-parts.lib.mkFlake { inherit inputs; } {
      systems = [ "x86_64-linux" "aarch64-linux" ];

      perSystem = { pkgs, self', ... }: {
        packages.tgtk = pkgs.stdenv.mkDerivation {
          pname = "tgtk";
          version = "0.1";

          # Only the files the build needs, so unrelated edits don't trigger rebuilds.
          src = pkgs.lib.fileset.toSource {
            root = ./.;
            fileset = pkgs.lib.fileset.unions [
              ./main.c
              ./Makefile
            ];
          };

          nativeBuildInputs = with pkgs; [
            pkg-config
            wrapGAppsHook4
          ];

          buildInputs = with pkgs; [
            gtk4
            json-glib
            qrencode
            tdlib
            gst_all_1.gstreamer
            gst_all_1.gst-plugins-base
            gst_all_1.gst-plugins-good
          ];

          installPhase = ''
            runHook preInstall
            install -Dm755 tgtk $out/bin/tgtk
            runHook postInstall
          '';

          meta = {
            description = "Minimal GTK4 Telegram client on TDLib";
            mainProgram = "tgtk";
            platforms = pkgs.lib.platforms.linux;
          };
        };

        packages.default = self'.packages.tgtk;

        devShells.default = pkgs.mkShell {
          inputsFrom = [ self'.packages.tgtk ];
          packages = with pkgs; [ gdb valgrind ];
          # GSettings schemas for GTK dialogs when running ./tgtk from the dev shell
          shellHook = ''
            export XDG_DATA_DIRS="$GSETTINGS_SCHEMAS_PATH''${XDG_DATA_DIRS:+:$XDG_DATA_DIRS}"
          '';
        };
      };
    };
}
