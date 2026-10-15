# IMX708 + reconhecimento facial na web

Waveshare ESP32-P4-Module-DEV-KIT (chip rev v1.3, 16 MB de flash). Funciona
com a **OV5647** (a câmera que vem com o kit; 1280x960, foco fixo) ou com a
**IMX708** (Pi Camera Module 3; 1280x720, autofoco) — o firmware usa a que
responder no I2C. A IA detecta e reconhece rostos; pelo Wi-Fi (ESP32-C6 da
placa) vão só a caixa de cada rosto, a cada quadro analisado, e a imagem de
dentro dela — nunca o quadro inteiro. A página desenha as caixas num canvas.

## Antes do primeiro build

1. `main/wifi_credentials.h` (fora do git) com SSID e senha — modelo em
   `main/wifi_credentials.h.example`. O C6 só enxerga redes de 2,4 GHz.
   Opcional: IP fixo (`WIFI_STATIC_IP` / `_GATEWAY` / `_NETMASK`) para redes
   cujo DHCP não responde.
2. Gere o `sdkconfig` de novo: o `sdkconfig.defaults` só vale para chaves que
   ainda não existem nele, e o atual tem tick de 100 Hz e chip rev ≥ 3. O
   `set-target` faz isso (guarda o antigo em `sdkconfig.old`).

```bash
idf.py set-target esp32p4 && idf.py build flash monitor
```

## Usando

O log termina com `==== abra no navegador: http://<ip>/ ====`.

| | |
| --- | --- |
| `GET /` | caixas ao vivo com o rosto dentro, tabela dos rostos, desempenho, cadastro |
| `GET /events` | Server-Sent Events: `faces`, `crop`, `stats` (abaixo) |
| `GET /capture.jpg` | uma foto do quadro inteiro, com as caixas (só quando pedida) |
| `GET /status` | JSON: fps, rostos na imagem, IDs cadastrados (o mesmo do `stats`) |
| `GET /quality?q=N` | qualidade JPEG dos recortes e da foto |
| `POST /enroll` | cadastra o maior rosto na imagem e devolve o ID |
| `POST /delete?id=N` | apaga um ID (`id=all` apaga todos) |

Caixa **verde** com `ID n · xx%` = reconhecido (xx = semelhança com o
cadastro). **Laranja** `desconhecido` = rosto sem cadastro. Cadastrar quem já
está cadastrado devolve o ID existente em vez de duplicar.

### `/events`

Uma conexão por espectador (no máximo 2), cada evento numa linha `data:` JSON:

- `faces` — a cada quadro analisado com rosto (e uma vez quando o último sai):
  `{"seq","t","f":[{"trk","id","sim","x","y","dx","dy","age"}]}`. `t` =
  `time_detected` (ms desde o boot); `x,y,dx,dy` como no terminal; `trk`
  identifica o rosto enquanto ele é seguido; `age` = ms desde que apareceu.
- `crop` — até 15 por segundo, um por rosto: `{"trk","x","y","w","h","cx","cy","jpg"}`.
  `jpg` = JPEG em base64 da região `x,y,w,h` do quadro (a caixa arredondada
  para múltiplos de 16; rosto com mais de 256 px é reduzido). `cx,cy` = centro
  da caixa naquele quadro: a página desenha o recorte na caixa mais recente,
  deslocado do quanto ela andou.
- `stats` — 1 por segundo, o mesmo JSON do `/status`.

Os cadastros ficam em `/spiflash/faces.bin` (partição FAT `storage`) e
sobrevivem a reboot e a reflash do app; só somem com `idf.py erase-flash` ou
apagando pela página. O log serial também mostra `reconhecido: ID n`.

## Tracking no terminal

`idf.py -p /dev/ttyACM0 monitor` mostra uma linha por rosto em cada quadro
analisado:

```
[ID,X,Y,dX,dY,time_detected]
[1,642,388,180,220,45678]
```

`ID` = -1 se desconhecido; `X,Y` = centro da caixa e `dX,dY` = largura e
altura, em pixels do quadro (1280x960); `time_detected` = ms desde o boot em
que o quadro foi capturado (a diferença entre duas linhas seguidas é o período
do tracking; 22 ms = 45 quadros/s). A página mostra os mesmos campos na
tabela "Rostos na imagem". A cada 5 s sai uma linha `#` com quadros/s
da IA e da câmera, tempo de detecção e de reconhecimento, e latência (do
quadro capturado ao resultado). Para ver só o tracking:
`idf.py monitor | grep '^\['`.

A detecção roda em todo quadro; a identidade passa de um quadro para o outro
por sobreposição de caixa. O reconhecimento (caro) só roda em rosto novo (na
hora), desconhecido (a cada 200 ms) e conhecido (re-verifica a cada 500 ms),
no máximo 2 por quadro. A IA usa o buffer da câmera direto, sem cópia.

## Como o tempo é dividido

- **core 0** — captura: entrega o buffer da câmera à IA (quando ela está
  livre); quando ela devolve, recorta cada caixa e codifica em JPEG por
  hardware (~2 ms por rosto, até 15 vezes por segundo, só com alguém na
  página). Também o reconhecimento (MobileFaceNet, ~120 ms por rosto), numa
  task de prioridade menor.
- **core 1** — IA: detecção (MSR+MNP) em todo quadro e tracking, no ritmo que
  der. A captura nunca espera por ela.
- Caixas e IDs são suavizados entre ciclos: uma detecção perdida não apaga a
  caixa, e o ID só cai depois de 3 ciclos seguidos sem reconhecer.

O tuning do ISP da OV5647 é `cfg/ov5647_noaf.json`: o padrão do
esp_cam_sensor sem a seção de autofoco, que dá assert sem motor.

Ajustes estão no topo de `main/face.cpp` (`RECOG_THR`, tamanhos mínimos de
rosto) e de `main/imx708_capture_main.c` (qualidade, espectadores, cores).

## Se algo der errado

- **`esp_wifi_init falhou`** — o C6 não respondeu pelo SDIO.
- **`Version mismatch: Host [...] > Co-proc [...]`** — firmware de fábrica do
  C6 antigo. Funciona, mas pode dar timeouts de RPC; o conserto é atualizar o
  C6 por OTA com o exemplo `host_performs_slave_ota` do esp_hosted 2.12.
- **`ainda sem IP - último motivo 201`** — SSID não encontrado (2,4 GHz?).
  Motivo 15, 202 ou 204 = senha errada.
- **`associado há 15 s sem IP (DHCP travado)`** — a placa associa mas o DHCP
  da rede não responde (visto na rede pública depois de muitos reboots
  seguidos). O firmware reassocia sozinho; se persistir, use IP fixo em
  `wifi_credentials.h`.
- **`Failed to detect camera motor` / `VIDIOC_G_MOTOR_FMT is not supported`**
  com a OV5647 — esperado: ela não tem motor de foco.
- **Câmera não detectada** — o log mostra uma varredura do I2C: 0x1a = IMX708,
  0x36 = OV5647, 0x18 = codec da placa. Só 0x18 = cabo/conector da câmera.
- **Caixas congelam ~1 s em intervalos regulares** — perda de pacotes entre P4
  e C6 seguida de retransmissão TCP (RTO de 1,5 s). Resolvido com
  `CONFIG_LWIP_TCP_SND_BUF_DEFAULT=11520`; não aumente esse valor. O log do
  esp_hosted (`stats: STA ... drop[N]`, a cada 10 s) mostra se voltou a perder.
- **"nenhum rosto" no cadastro** — o detector só pega rosto em pé e de frente.
- **Página abre mas fica em "reconectando…"** — rede pública pode isolar clientes entre si;
  teste com o celular como roteador.
- **Assert em `prvCreateIdleTasks` antes do `app_main`** — RAM interna acabou;
  ver o bloco do esp_hosted no `sdkconfig.defaults`.
- **Cores estranhas / centro esverdeado** — tuning padrão do ISP (e, na
  IMX708 NoIR, falta de filtro de IR); não é o código.
