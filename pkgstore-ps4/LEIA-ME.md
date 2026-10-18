# PKG Store para o RPCS3 PORT FOR PS4

Uma entrada **PKG Store** na coluna *PlayStation Network* do XMB: lista no formato pkgi, busca, filtros e downloads em segundo plano.

## Instalar (Arch ou qualquer Linux)
    bash install.sh /caminho/para/rpcs3-ps4     # a pasta que tem rpcs3/ e ps4-port/
Copia `ps4_pkgstore.hpp` e `ps4_pkgstore_http.hpp` para `rpcs3/util/` e aplica `pkgstore_launcher.patch` em `rpcs3/ps4_launcher.cpp`. Depois compile o port como de costume (o `BUILDING.md` do repositório). Não há arquivo novo para listar no CMake: tudo é cabeçalho.

## No PS4
1. Copie a sua `pkgi_games.txt` para **`/data/rpcs3/pkgi_games.txt`** (FTP do GoldHEN).
2. XMB > coluna **PlayStation Network** > **PKG Store**.
3. Botões: **X** baixa o item (ou cancela/limpa um download no topo) · **Quadrado** pesquisa (teclado do sistema) · **L1/R1** região · **L2/R2** tipo · **Triângulo** ordem · **Esquerda/Direita** pulam 10 linhas · **Círculo** volta.
4. O download aparece no topo da lista, com progresso. Ao terminar, o PKG surge na coluna **Jogos** (`/data/rpcs3/pkgs`); escolha-o ali para instalar, como o port já faz.

## Como funciona
- Em andamento: `/data/rpcs3/data/pkgstore/*.part` (o XMB não varre essa pasta). Se cair, ao baixar de novo ele continua de onde parou.
- Mostra até 300 resultados por vez; refine a busca para ver o resto.
- As colunas de RAP da lista **não são lidas**. Licenças: seus `.rap` em `/data/rpcs3/raps`.
- Um download continua mesmo se você abrir um jogo.

## Logs
`/data/rpcs3/startup.log` (linhas `pkgstore: ...`, com o motivo técnico de cada falha) e `/data/rpcs3/log/RPCS3.log`.

## O que foi testado
| Parte | Estado |
|---|---|
| Leitor da lista, busca, fila, retomada, cancelamento | testado no PC (ASan/UBSan/TSan), com a sua lista: 7909 itens, 5514 com link |
| Linhas e botões da tela (trechos reais do launcher) | compilados e simulados no PC contra a sua lista |
| Baixador com curl (`ps4_pkgstore_http.hpp`) | **só compilado contra cabeçalhos de teste** |
| `ps4_launcher.cpp` inteiro e o PS4 | **não compilei nem rodei**: sem o toolchain aqui |

Se o compilador reclamar, mande a saída. Se o download falhar no PS4, mande as linhas `pkgstore:` do `startup.log`.

## Testes no PC
    cd test && g++ -std=c++20 -fno-exceptions -Wall -Wextra -pthread -I.. pkgstore_test.cpp -o t && ./t /caminho/pkgi_games.txt
