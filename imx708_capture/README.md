# IMX708 + reconhecimento facial na web

Waveshare ESP32-P4-Module-DEV-KIT (chip rev v1.3, 16 MB de flash). Funciona
com a **OV5647** (a câmera que vem com o kit; 1280x960, foco fixo) ou com a
**IMX708** (Pi Camera Module 3; 1280x720, autofoco) — o firmware usa a que
responder no I2C. A IA detecta e reconhece rostos; pelo Wi-Fi (ESP32-C6 da
placa) vão a caixa de cada rosto, a cada quadro analisado, e a imagem de
dentro dela. A página desenha as caixas num canvas e tem um botão para trocar
para a **imagem inteira** da câmera (abaixo).

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

No topo da página, **Só os rostos** / **Imagem inteira** escolhe o que vem
da placa (cada aba escolhe a sua, e o navegador lembra):

- **Só os rostos** — a imagem só de dentro de cada caixa (recortes de até
  256 px, 15 por segundo): pouca banda, o modo original.
- **Imagem inteira** — o quadro todo com as caixas daquele mesmo quadro (a
  caixa fica no rosto mesmo com a imagem atrasada). A página pede um quadro
  por vez ao `/frame.jpg` e só pede o próximo quando o anterior chegou: a
  imagem anda no ritmo que o Wi-Fi aguenta, sem acumular atraso nos buffers
  do caminho, e a placa só codifica quadro que alguém vai receber (mais um
  adiantado, que fica pronto enquanto o anterior viaja). Em q60 o JPEG tem
  ~115 KB; com o Wi-Fi do C6 (o MJPEG antigo passava ~9 Mbit/s) devem ser
  uns 9 quadros/s (estimativa, não medido no P4). Baixe a qualidade JPEG
  para mais. O mesmo esquema, na Luckfox pela USB, tirou um atraso de
  1,6–2 s da versão anterior (quadros empurrados pelo `/events`). O JPEG sai
  do encoder de hardware direto do buffer da câmera, na task de captura, no
  máximo 15 por segundo.

| | |
| --- | --- |
| `GET /` | caixas ao vivo com o rosto dentro, tabela dos rostos, desempenho, cadastro |
| `GET /events` | Server-Sent Events: `faces`, `crop`, `stats`, `level` (abaixo) |
| `GET /events?view=full` | o mesmo, sem `crop` (a imagem inteira vem do `/frame.jpg`) |
| `GET /frame.jpg?after=N` | o próximo quadro inteiro depois do de número N, em JPEG; cabeçalhos `X-Frame-Seq` (número dele), `X-Frame-T` (ms desde o boot em que foi capturado) e `X-Faces` (as caixas dele, como no evento `faces`) |
| `GET /capture.jpg` | uma foto do quadro inteiro, com as caixas (só quando pedida) |
| `GET /status` | JSON: fps, rostos na imagem, IDs cadastrados (o mesmo do `stats`) |
| `GET /quality?q=N` | qualidade JPEG dos recortes e da foto |
| `POST /enroll` | cadastra o maior rosto na imagem e devolve o ID |
| `POST /delete?id=N` | apaga um ID (`id=all` apaga todos) |
| `GET /isp` | ajuste do ISP: valores, faixas e o que a página usa para gerar o JSON |
| `POST /isp?gamma=0.8&...` | muda e aplica na hora; `/isp?save` grava na NVS, `/isp?reset` volta ao tuning do firmware |
| `POST /audio?gain=N` | ganho do microfone, 0..42 dB (passos de 6) |
| `GET /asr` | página de fala: wake word e comandos do ESP-SR (abaixo) |

Caixa **verde** com `ID n · xx%` = reconhecido (xx = semelhança com o
cadastro). **Laranja** `desconhecido` = rosto sem cadastro. Cadastrar quem já
está cadastrado devolve o ID existente em vez de duplicar.

### `/events`

Uma conexão por espectador (no máximo 2), cada evento numa linha `data:` JSON:

- `faces` — a cada quadro analisado com rosto (e uma vez quando o último sai):
  `{"seq","t","f":[{"trk","id","sim","x","y","dx","dy","age"}]}`. `t` =
  `time_detected` (ms desde o boot); `x,y,dx,dy` como no terminal; `trk`
  identifica o rosto enquanto ele é seguido; `age` = ms desde que apareceu.
- `crop` — até 15 por segundo, um por rosto (não com `?view=full`): `{"trk","x","y","w","h","cx","cy","jpg"}`.
  `jpg` = JPEG em base64 da região `x,y,w,h` do quadro (a caixa arredondada
  para múltiplos de 16; rosto com mais de 256 px é reduzido). `cx,cy` = centro
  da caixa naquele quadro: a página desenha o recorte na caixa mais recente,
  deslocado do quanto ela andou.
- `stats` — 1 por segundo, o mesmo JSON do `/status`.
- `level` — 20 por segundo, se o microfone iniciou: `{"rms","peak","ref"}` em
  dBFS (0 = fundo de escala). `rms` = último quadro de 32 ms do microfone,
  `peak` = pico segurado ~1 s, `ref` = canal da referência do DAC.

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

## Ajuste da imagem (ISP)

A OV5647 entrega RAW: quem faz a imagem é o ISP do P4, seguindo o JSON de
tuning. A seção **Imagem (ISP)** da página mexe nele ao vivo:

| slider | no JSON | efeito |
| --- | --- | --- |
| Brilho (alvo do AE) | — (é do sensor) | o AE da própria OV5647 ajusta exposição e ganho até este brilho médio; com a exposição pelo rosto ligada, vale só sem rosto |
| Exposição pelo rosto | — (firmware) | com rosto na imagem, move o alvo do AE até o rosto ficar com o brilho abaixo |
| Brilho do rosto | — (firmware) | brilho buscado no miolo do maior rosto (0-255, como sai na imagem; padrão 120) |
| Gamma | `aen.gamma.table[].gamma_param` | menor = sombras mais claras |
| Contraste | `aen.contrast[].value` | |
| Saturação | `acc.saturation[].value` | |
| Força da cor (CCM) | `acc.ccm.table[].matrix` | 0 = sem correção de cor, 1 = a matriz do JSON |
| Vermelho / Azul | `acc.ccm` (multiplicado na matriz) | ganho do canal antes da matriz: corrige o tom |
| Redução de ruído | `adn.bf[].param.level` | filtro bilateral no RAW; alto demais borra |
| Nitidez / bordas fortes / limiar | `aen.sharpen[].param` (`m_coeff`, `h_coeff`, `l_thresh`) | |

Na OV5647, exposição e balanço de branco rodam dentro do sensor (o JSON não
tem `agc` nem `awb`); o ISP só aplica o resto, com os mesmos valores em
qualquer luz. A matriz de cor do firmware é forte (2,0 na diagonal): dobra o
ruído do vermelho e do azul, que é o grão colorido em ambiente interno. Baixar
a **força da cor** e subir a **redução de ruído** é o primeiro teste.

**Exposição pelo rosto** (ligada por padrão): o AE do sensor mede a imagem
inteira, então com uma janela clara atrás ele escurece tudo e o rosto sai
escuro. A cada quadro com rosto, a IA mede o brilho do miolo do maior rosto
(`face.cpp`, grade de 16x16 pontos, microssegundos); uma task em
`isp_tune.c` move o alvo do AE do sensor até esse brilho chegar no **brilho
do rosto**: no máximo x1,5 por ajuste, um ajuste a cada ~300 ms (o tempo do
AE do sensor chegar lá), zona morta de 8%. Converge em ~1 s. Sem rosto,
segura o ajuste por 3 s e depois volta ao slider aos poucos. Em contraluz
muito forte para no máximo do sensor (alvo 235). A linha embaixo dos
sliders mostra ao vivo o alvo no sensor, o brilho do rosto e o modo.

Como funciona: `main/isp_tune.c` copia para a RAM a configuração do IPA que o
esp_video carregou, troca as tabelas e chama
`esp_video_isp_pipeline_set_ipa_config()`. O IPA continua sendo quem programa
o ISP, então nada é sobrescrito no quadro seguinte.

- **Salvar na placa** grava na NVS; o ajuste volta sozinho no boot, sem
  recompilar. **Voltar ao padrão** apaga e volta ao JSON do firmware.
- **JSON para o cfg/** gera o arquivo no formato de `cfg/ov5647_noaf.json`
  (é o arquivo inteiro, para a OV5647). Colado lá e recompilado, vira o
  padrão. O alvo do AE e a exposição pelo rosto não vão no JSON.
- Ajuste olhando a **imagem inteira** e com a luz em que a câmera vai
  trabalhar.

Antes do ISP, vale conferir o que ele não conserta: lente fora de foco (na
maioria dos módulos OV5647 a lente é rosqueada e gira com cuidado), película
na lente, e versão sem filtro de IR (NoIR: tom rosado com luz do dia).

## Microfone

O ES8311 da placa (I2C 0x18, o mesmo barramento da câmera) no I2S0: MCLK 13,
BCLK 12, WS 10, DIN 11 (pinos do exemplo `12_I2SCodec` da Waveshare para o
P4). 16 kHz, 16 bits, estéreo: esquerda = microfone, direita = referência do
DAC (o que o alto-falante toca). A página mostra o nível num slider e tem o
ganho do codec (0..42 dB; padrão 30 dB, em `main/audio.c`). Só grava quando
a página de fala pede (abaixo), e o áudio fica na flash da placa.

## Página de fala (`/asr`)

O mesmo banco de medição da Luckfox (`rv1106_test/asr_bench`), com os
motores que cabem na P4. O Whisper não cabe: o menor modelo (`ggml-tiny-q8_0`)
tem 43 MB, a flash tem 16 MB e a PSRAM (32 MB) já leva câmera e app. Os
motores são do ESP-SR 2.6:

| motor | o que é |
| --- | --- |
| `wakenet-olaesp` | WakeNet10 "Olá ESP" (wake word em português), modo normal, direto no áudio |
| `wakenet-olaesp-95` | o mesmo no modo agressivo (detecta mais, e dá mais alarme falso) |
| `afe-wakenet-olaesp` | o WakeNet dentro do AFE (com VAD), como numa aplicação |
| `multinet-en` | MultiNet7 inglês, só os comandos da lista |
| `afe-multinet-en` | o mesmo, com o áudio passando antes pelo AFE |

O MultiNet7 só tem inglês e chinês: comando em português não existe (escrever
"liga a luz" na lista vira fonemas do inglês). A lista fica em
`/clips/commands.txt` e se edita na página.

- **Frases:** gravadas pelo microfone da placa (`mic-NNN`, 2 a 15 s), ou
  enviadas do PC (`wav-NNN`: qualquer áudio que o navegador abra, convertido
  para 16 kHz mono; o microfone do PC só funciona em https/127.0.0.1). O texto
  da caixa vira a referência. Ficam na FAT `clips` (~90 s no total).
- **Medir:** um motor por vez; cada um carrega o modelo uma vez e passa por
  todas as frases. "Parar a IA de rostos" libera o core 1 durante a medição.
- **Pontuação:** wake word — a frase deveria disparar se a referência tem
  "olá esp"; mostra detecção e alarme falso (grave também frases parecidas
  que não devem disparar, como "Olá Esperança"). Comandos — certo se
  reconheceu o comando da referência, ou nada quando a referência não está na
  lista.
- **Números:** RTF (processamento ÷ fala), carga do modelo, pico de RAM
  (PSRAM + interna, medido pela memória livre do sistema: outra task alocando
  junto também conta). Não há uso de CPU como na Luckfox: o FreeRTOS daqui
  está sem estatística por task.

Os resultados ficam em `/clips/results.txt`, uma linha JSON por frase e motor
(campos em `main/asr.c`). Os modelos vão na partição `model` (srmodels.bin de
3,3 MB, gravado pelo `idf.py flash`): `CONFIG_SR_*` no `sdkconfig.defaults`.

Duas coisas fora do óbvio no build:

- O esp-sr registra no esp-dl só os ops do WakeNet, e aí o esp-dl compila só
  esses: os modelos de rosto parariam de carregar. O `CMakeLists.txt` do
  projeto zera essa lista depois do `project()`, e o esp-dl volta a compilar
  tudo.
- A FAT está sem nomes longos (`CONFIG_FATFS_LFN_NONE`): nomes 8.3, por isso
  `mic-001.wav` e `results.txt`.

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
  IMX708 NoIR, falta de filtro de IR); não é o código. Ajuste em **Imagem
  (ISP)** na página.
- **`ajuste do ISP desligado`** no log — o chip-ID do sensor não foi lido
  (`sensor não identificado`) ou não há JSON de tuning para ele.
- **Slider do microfone parado** — o log diz `microfone (ES8311) falhou`, ou
  `/status` traz `"mic_gain":-1`. Sem som chegando, o nível fica perto de
  -90 dB: suba o ganho.
- **Página de fala sem motores** — o log diz `partição "model" sem modelos`
  (rode `idf.py flash`, que grava o srmodels.bin) ou `sem a partição "clips"`
  (a tabela de partições nova não foi gravada).
- **Rostos pararam de ser detectados depois do esp-sr** — confira se o
  `idf_build_set_property(ESPDL_COMPILE_REQS "")` continua depois do
  `project()` no `CMakeLists.txt`.
