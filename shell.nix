{ pkgs ? import <nixpkgs> {} }:

pkgs.mkShell {
  # Список пакетов, которые будут доступны в окружении
  buildInputs = with pkgs; [
    gtk4
    json-glib
    tdlib
    pkg-config
    haskellPackages.haskell-qrencode
  ];
}
