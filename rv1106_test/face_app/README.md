# face_app — reconhecimento facial na Luckfox Pico Max

Porta do `imx708_capture` (ESP32-P4) para a Luckfox Pico Max (RV1106) com a
câmera MIS5001: mesma página, mesmos endpoints, mesmos JSONs, mesma saída no
terminal e o mesmo formato de arquivo de cadastros. A IA detecta e reconhece
rostos; pela rede vão a caixa de cada rosto, a cada quadro analisado, e a
imagem de dentro dela. A página desenha as caixas num canvas e tem um botão
para trocar para a **imagem inteira** da câmera (abaixo).

## Compilar e rodar

```bash
source env.sh
scripts/face_app.sh deploy    # compila e copia para /root/face_app na placa
scripts/face_app.sh start     # roda em segundo plano (para o rkipc da imagem)
scripts/face_app.sh monitor   # o "idf.py monitor": tracking e resumo; Ctrl+C sai
```

`scripts/face_app.sh` sem argumentos lista os outros comandos (`stop`,
`restart`, `run` em primeiro plano, `web`). Opções do app vão em
`FACE_APP_ARGS`:

| | |
| --- | --- |
| `--port N` | porta HTTP (80) |
| `--width N --height N` | quadro da câmera (1280x960, as coordenadas do P4) |
| `--fps N` | quadros/s fixos da câmera; 0 = automático (30) |
| `--hz 50\|60` | rede elétrica, para o antiflicker (60) |
| `--thr X` | semelhança mínima para reconhecer, 0..1 (0.60) |
| `--db arquivo` | cadastros (`/userdata/faces.bin`) |
| `--asr pasta` | `asr_bench` na placa, para a página **Fala** (`/root/asr_bench`; ver [asr_bench](../asr_bench/README.md)) |

## Abrir a página

Pela **USB** (localhost), com a placa anexada na WSL (`usbipd attach`, ver o
README da pasta de cima):

```bash
source env.sh
scripts/face_app.sh start    # se o app ainda não estiver rodando
scripts/face_app.sh web      # adb forward: porta 80 da placa -> 8080 do PC
```

e abra **<http://127.0.0.1:8080/>** no navegador do Windows (ou da WSL). Use
`127.0.0.1`, não `localhost`: o adb escuta só em IPv4, o Windows tenta
`localhost` primeiro em IPv6 (`::1`) e a rede espelhada da WSL não repassa
isso. O encaminhamento some quando a placa reinicia ou sai da WSL: rode o
`web` de novo.

Pela **Ethernet**, o log mostra `==== abra no navegador: http://<ip>/ ====`.

No topo da página, **Só os rostos** / **Imagem inteira** escolhe o que vem
da placa (cada aba escolhe a sua, e o navegador lembra):

- **Só os rostos** — a imagem só de dentro de cada caixa (recortes de até
  256 px, 15 por segundo): pouca banda, o modo original.
- **Imagem inteira** — o quadro todo, 1280x960, com as caixas daquele mesmo
  quadro (a caixa fica no rosto mesmo com a imagem atrasada). A página pede
  um quadro por vez ao `/frame.jpg` e só pede o próximo quando o anterior
  chegou: a imagem anda no ritmo que a rede aguenta, sem acumular atraso.
  Pela USB (adb, ~7 Mbit/s), em q60 (~70 KB por quadro): ~9 quadros/s, com
  atraso extra de 74 ms na mediana e 158 ms no p95. Antes, com os quadros
  empurrados pelo `/events` em base64, eram 8/s com **1,6–2 s** de atraso,
  porque os buffers do caminho (TCP, adb) guardavam vários quadros. Aba
  escondida não pede quadros.

| | |
| --- | --- |
| `GET /` | caixas ao vivo com o rosto dentro, tabela dos rostos, desempenho, cadastro |
| `GET /events` | Server-Sent Events: `faces`, `crop`, `stats` (como no P4) |
| `GET /events?view=full` | o mesmo, sem `crop` (a imagem inteira vem do `/frame.jpg`) |
| `GET /frame.jpg?after=N` | o próximo quadro inteiro depois do de número N, em JPEG; cabeçalhos `X-Frame-Seq` (número dele), `X-Frame-T` (ms desde o boot em que foi capturado) e `X-Faces` (as caixas dele, como no evento `faces`) |
| `GET /capture.jpg` | uma foto do quadro inteiro, com as caixas (só quando pedida) |
| `GET /status` | JSON: quadros/s, rostos na imagem, IDs cadastrados |
| `GET /quality?q=N` | qualidade JPEG dos recortes e da foto |
| `POST /enroll` | cadastra o maior rosto na imagem e devolve o ID |
| `POST /delete?id=N` | apaga um ID (`id=all` apaga todos) |

Caixa **verde** com `ID n · xx%` = reconhecido; **laranja** `desconhecido` =
rosto sem cadastro. Cadastrar quem já está cadastrado devolve o ID existente.
Os cadastros ficam em `/userdata/faces.bin` e sobrevivem a reboot; só somem
apagando pela página ou regravando a placa.

## Terminal

Uma linha por rosto em cada quadro analisado, e a cada 5 s uma linha `#`:

```
[ID,X,Y,dX,dY,time_detected]
[1,736,484,512,756,552042]
# IA 24.9 quadros/s (camera 24.9) | deteccao 31.3 ms | latencia 31 ms | rostos/quadro 0.0 | reconhecimento 0 ms cada, 0.0/s
```

`ID` = -1 se desconhecido; `X,Y` = centro e `dX,dY` = largura e altura da
caixa, em pixels do quadro 1280x960; `time_detected` = ms desde o boot da
placa em que o quadro chegou. A detecção roda em todo quadro; o
reconhecimento, em rosto novo (na hora), desconhecido (a cada 200 ms) e
conhecido (re-verifica a cada 500 ms), até 2 por quadro.

## Como funciona

- **Câmera** (`camera.cpp`): ISP com 3A (rkaiq) e canal VI do RKMPI em NV12,
  5 buffers, sem cópia. O antiflicker vai para 60 Hz e a taxa fica fixa em
  30 quadros/s (o IQ da MIS5001 vem com 50 Hz, que no Brasil dá faixas e
  prende em 25). A captura do OpenCV-mobile não serve: usa um buffer só e
  perde metade dos quadros em 1280x960.
- **Detecção** (`models.cpp`): o RGA reduz o quadro para 640x480 e converte
  para BGR direto na memória de entrada da NPU (sem CPU); RetinaFace na NPU
  (~25 ms) e decodificação das 16800 âncoras.
- **Reconhecimento**: recorte do rosto, alinhado pelos olhos e encaixado em
  160x160 com borda cinza numa transformação só, em RGB; MobileFaceNet na NPU
  (~4,4 ms) dá um vetor de 128 posições comparado por cosseno.
- **IA** (`face.cpp`): tracking por IoU, agendamento e cadastro iguais aos
  do P4, numa thread que analisa sempre o quadro mais novo.
- **Web** (`main.cpp`, `http.cpp`): recortes JPEG por hardware (MPP) até 15
  por segundo, só com alguém na página; SSE com no máximo 2 espectadores.
- **Imagem inteira** (`main.cpp`): só codifica quadro pedido. Um pedido ao
  `/frame.jpg` marca "quero um quadro"; a captura passa o próximo quadro
  analisado (com as caixas dele) para uma thread própria, com prioridade
  menor que a IA e a captura (o RV1106 tem um núcleo só). O RGA converte de
  NV12 para BGR num buffer DMA, o buffer da câmera volta logo, e o JPEG sai
  pelo MPP (~28 ms). Ao responder, o servidor já pede o próximo, que fica
  pronto enquanto este viaja. Medido: a latência da IA continua em ~34 ms
  com alguém na imagem inteira, e quase não há quadro codificado à toa (157
  codificados, 155 enviados).

## Diferenças para o P4

- Câmera a 30 quadros/s (a OV5647 do P4 dava 45); o sensor só tem o modo
  2592x1944 a 30.
- Modelos diferentes: o vetor tem 128 posições (no P4, 512), então os
  arquivos de cadastro não passam de uma placa para a outra, e o limiar
  padrão é 0.60 (no P4, 0.50).
- O reconhecimento custa ~5 ms de NPU (no P4, ~120 ms): roda na própria
  thread da IA, e o resumo não diz mais "(em paralelo)".
- O rodapé da página mostra a memória livre do Linux (não há heap interna nem
  PSRAM).

## Se algo der errado

- **`nenhum sensor`** — a MIS5001 só funciona com imagem de 2025-03 em diante
  (ver o README da pasta de cima); `dmesg | grep mis5001` na placa.
- **`porta 80: Address in use`** — outro face_app rodando:
  `scripts/face_app.sh stop`.
- **Página não abre pela USB** — `http://127.0.0.1:8080/`, não `localhost`;
  `adb forward --list` deve mostrar `tcp:8080 tcp:80` (senão,
  `scripts/face_app.sh web`).
- **"reconectando…" ao trocar de modo** — normal por 1-2 s: o `/events` de
  cada modo é uma conexão nova, e a placa só percebe que a anterior fechou
  no envio seguinte (limite de 2 espectadores).
- **Cores com faixas horizontais** — antiflicker errado para a rede elétrica:
  `--hz 50` fora do Brasil/Américas.
- **Reconhece pouco / confunde pessoas** — ajuste `--thr` (subir confunde
  menos, mas diz "desconhecido" mais vezes). Cadastros feitos antes de uma
  mudança no pré-processamento podem precisar ser refeitos.
