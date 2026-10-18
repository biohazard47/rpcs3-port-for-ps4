#!/usr/bin/env bash
# Uso: ./install.sh /caminho/para/rpcs3-ps4     (a pasta que contém rpcs3/ e ps4-port/)
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="${1:-}"
[ -n "$root" ] && [ -f "$root/rpcs3/ps4_launcher.cpp" ] || { echo "Passe a pasta rpcs3-ps4 (a que tem rpcs3/ps4_launcher.cpp)."; exit 1; }
if grep -q "pkgui::store()" "$root/rpcs3/ps4_launcher.cpp"; then echo "O patch já está aplicado."; exit 0; fi
patch -p1 -d "$root" --dry-run < "$here/pkgstore_launcher.patch" >/dev/null || { echo "O patch não aplica limpo: seu ps4_launcher.cpp difere do da versão main usada aqui."; exit 1; }
cp "$here/util/ps4_pkgstore.hpp" "$here/util/ps4_pkgstore_http.hpp" "$root/rpcs3/util/"
patch -p1 -d "$root" < "$here/pkgstore_launcher.patch"
echo "Pronto. Agora compile o port como de costume e copie pkgi_games.txt para /data/rpcs3/ no PS4."
