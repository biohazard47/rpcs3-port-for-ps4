# Compilar o PKG na nuvem (GitHub Actions)

O GitHub compila o port num computador Windows dele e te entrega o `.pkg`. Você não precisa de Windows nem de um PC forte.

## 1. Fork (uma vez)
No site do GitHub, abra `RoberixStudios/rpcs3-port-for-ps4` e toque em **Fork**. Mantenha o fork **público** (o tempo de build é grátis em repositório público).

## 2. Colocar a PKG Store e o workflow no fork (no Arch)
    sudo pacman -S git github-cli          # se ainda não tiver
    gh auth login -s workflow              # a permissão "workflow" é necessária para enviar o arquivo do workflow
    git clone --depth 1 https://github.com/SEU_USUARIO/rpcs3-port-for-ps4.git
    cd rpcs3-port-for-ps4
    cp -r /caminho/de/pkgstore-ps4 ./pkgstore-ps4          # a pasta do zip
    mkdir -p .github/workflows
    cp pkgstore-ps4/build-pkg.yml .github/workflows/build-pkg.yml
    git add pkgstore-ps4 .github
    git commit -m "PKG Store + workflow de build"
    git push

Não envie a sua `pkgi_games.txt`: ela não é usada no build.

## 3. Rodar
1. No fork, abra a aba **Actions** e confirme **"I understand my workflows, go ahead and enable them"** (forks vêm com isso desligado).
2. Escolha **Build PKG**, toque em **Run workflow** e em **Run workflow** de novo. Pode fechar o navegador: ele roda sozinho.
3. Quando terminar (veja o limite abaixo), abra a execução e baixe, em **Artifacts**, o arquivo **rpcs3-ps4-pkg**. Dentro vem o `RPCS3_PS4_NATIVE_<versão>_<data>.pkg`, o `.sha256` e o manifesto.
4. Se falhar, baixe **logs-do-build**: os arquivos `1-llvm.log`, `2-configure.log`, `3-build.log` e `4-package.log` mostram onde parou. Me mande o trecho do erro.

Para saber se um erro vem do patch ou do ambiente, rode de novo com **apply_pkgstore desmarcado**: ele compila o port original, sem a PKG Store.

## 4. Instalar no PS4
Copie o `.pkg` para `/data/pkg` e instale pelo GoldHEN (Debug Settings > Package Installer), como você já faz. É o mesmo Title ID (`RPCS30010`), então atualiza o port e mantém as suas configurações e caches. Copie também a `pkgi_games.txt` para `/data/rpcs3/`.

## Limites e riscos (não testei isto)
- **Tempo:** cada job do GitHub tem no máximo 6 horas, e o workflow para em 5h55. LLVM, FFmpeg, Mesa e o RPCS3 num runner de 4 núcleos podem passar disso. Se acontecer, o job é cortado e não há PKG. A estimativa de tempo é minha, não vem do `BUILDING.md`.
- **Disco e memória:** não sei se o runner aguenta o build inteiro. O log "Informações da máquina" mostra a CPU e o espaço livre.
- **Nomes dos arquivos baixados:** o workflow descobre o arquivo do OpenOrbis pelo release v0.5.4 e monta o Mesa e o porting-kit como o `BUILDING.md` descreve. Se algum release mudou de forma, o passo "Baixar OpenOrbis..." para com uma mensagem dizendo qual.
- O build do PKG só usa ferramentas do `BUILDING.md`: OpenOrbis v0.5.4, llvm-mingw 20240619, Ninja 1.12.1, CMake e Python 3.12 do runner.
