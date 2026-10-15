# Luckfox Pico Max (RV1106) — reconhecimento facial

Luckfox Pico Max: RV1106G3 (Cortex-A7 + NPU de 1 TOPS INT8), 256 MB de RAM
(~180 MB livres para o Linux), 256 MB de SPI NAND, Ethernet 100M e câmera
MIPI CSI. Roda a imagem **Buildroot** da Luckfox (uclibc).

| | |
| --- | --- |
| `setup.sh` | baixa o toolchain e os exemplos da Luckfox (commits fixos) |
| `env.sh` | `source env.sh` em cada terminal: PATH, `adb`, `BOARD_LINK` |
| `cmake/rv1106.cmake` | toolchain CMake para os nossos projetos |
| `npu_check/` | teste da cadeia toda: compila, copia e roda inferência na NPU |
| `scripts/build_example.sh` | compila um exemplo oficial sem o menu interativo |
| `scripts/deploy.sh` | copia uma pasta para a placa (adb ou ssh) |
| `udev/51-rockchip.rules` | permissão de USB para a placa (ADB e modo MaskRom) |
| `sdk/`, `third_party/` | baixados pelo `setup.sh`, fora do git |

## Preparar o PC

```bash
./setup.sh
source env.sh
```

Do SDK da Luckfox só vem o toolchain (GCC 8.3
`arm-rockchip830-linux-uclibcgnueabihf`, ~320 MB). Para escrever programas não
precisa do resto: as bibliotecas de runtime estão na imagem da placa e os
headers/libs de link estão nos repositórios de exemplo em `third_party/`. O SDK
inteiro (vários GB, Ubuntu 22.04) só é necessário para recompilar kernel ou a
imagem.

## Ligar a placa (USB → WSL → ADB)

A placa vai para a WSL pelo `usbipd` e é acessada por **ADB**. A rede USB dela
(RNDIS) não funciona na WSL: o kernel da WSL não tem esse driver.

Uma vez, no PowerShell como administrador (o BUSID aparece no `usbipd list`
como `2207:0019 Remote NDIS ... ADB Interface`). Regravar a imagem muda o
número de série da placa e o Windows a trata como outro aparelho: repita o
`bind` depois de cada gravação.

```powershell
usbipd bind --force --busid 2-2
```

e na WSL, a permissão de USB:

```bash
sudo cp udev/51-rockchip.rules /etc/udev/rules.d/ && sudo udevadm control --reload-rules && sudo udevadm trigger
```

A cada vez que ligar a placa (ou ela reiniciar), no PowerShell:

```powershell
usbipd attach --wsl --busid 2-2        # --auto-attach para reanexar sozinho
```

Na WSL, depois do `source env.sh`:

```bash
adb devices          # 590ef40cbd0c8bec  device
adb shell            # shell de root na placa
```

O `adb` do `env.sh` é uma função que sobe o servidor adb antes: com a rede
espelhada da WSL, conectar numa porta fechada de `127.0.0.1` trava em vez de
falhar, e o `adb` puro fica pendurado procurando o servidor.

**Ethernet** é a alternativa: a placa pega IP por DHCP (`ip addr show eth0`
nela) e os scripts usam ssh com `export BOARD_LINK=ssh BOARD=<ip>` (usuário
`root`, senha `luckfox`; `ssh-copy-id root@<ip>` uma vez).

## Imagem da placa e runtime da NPU

A placa roda a `Luckfox_Pico_Pro_Max_Flash_250607` (kernel 5.10.160 de
jun/2025): `librknnmrt` 1.6.0 e driver da NPU 0.9.2. A imagem de fábrica era
mais velha (`librknnmrt` 1.4.1, driver 0.8.2, sem MIS5001) e não lia os modelos
do toolkit 1.6.0 (`E RKNN: failed to decode config data!` e segfault). Por
garantia o runtime 1.6.0 vai junto em `lib/` ao lado do binário (RPATH
`$ORIGIN/lib`): o `npu_check` e o `build_example.sh` já fazem isso.

## Testar a NPU

```bash
cmake -S npu_check -B build/npu_check -DCMAKE_TOOLCHAIN_FILE=$PWD/cmake/rv1106.cmake \
      -DCMAKE_INSTALL_PREFIX=$PWD/build/npu_check_deploy
cmake --build build/npu_check --target install
scripts/deploy.sh build/npu_check_deploy /root/npu_check
adb shell 'cd /root/npu_check && ./npu_check'
```

Na nossa placa: `runtime: 1.6.0`, `driver: 0.9.2`, RetinaFace 640x640 em
**24,8 ms** e MobileFaceNet 160x160 em **4,4 ms** por inferência
(`./npu_check <modelo.rknn> [repetições]`).

## Câmera (MIS5001)

A nossa câmera é a MIS5001 de 5 MP (até 2592x1944). O driver dela entrou no
SDK da Luckfox em 2025-03-05; imagens anteriores não a enxergam. Com a imagem
250607 o boot mostra `mis5001 4-0031: Detected ... sensor`
(`adb shell 'dmesg | grep -i mis5001'`) e o ISP usa
`/etc/iqfiles/mis5001_CMK-OT2115-PC1_30IRC-F16.json`.
`Unexpected sensor id(000000)` = o sensor não respondeu. Ligue e desligue a
câmera com a placa sem energia, com o lado metálico do flat virado para o lado
do chip da placa.

Ao ligar, a imagem já sobe o `rkipc` (app de câmera + RTSP da Luckfox), que
segura a câmera. Os demos chamam `RkLunch-stop.sh` para pará-lo.

## Regravar a imagem (Windows, SocToolKit)

Precisa para a MIS5001 e traz um runtime da NPU novo. Apaga tudo da placa.

1. Imagem Buildroot da Pico Pro/Max, pasta **Flash** (SPI NAND), a mais nova
   (`Luckfox_Pico_Pro_Max_Flash_250607` ou posterior):
   <https://drive.google.com/drive/folders/1_tR-Rj0fY5y_aLzWKHja7OtaYhUSg_NF>.
   Descompacte.
2. Driver USB da Rockchip:
   [DriverAssitant](https://github.com/LuckfoxTECH/luckfox-aura-docs/releases/tag/v0.0.1)
   → `DriverInstall.exe` → *Install driver*.
3. [SocToolKit](https://files.luckfox.com/wiki/Luckfox-Pico/Software/SocToolKit_v1.98_20240705_01_win.zip):
   descompacte, rode como administrador e escolha **RV1106**.
4. Modo de gravação: `adb shell reboot loader`, ou desligue, segure **BOOT** e
   ligue a USB. A placa aparece no SocToolKit.
5. *Search Path* → pasta da imagem, marque todas as partições, *Download*.
6. Depois do boot, anexe de novo na WSL (`usbipd attach --wsl --busid 2-2`).

Guia oficial: <https://wiki.luckfox.com/Luckfox-Pico-RV1106/Getting-Started/Image-flashing/>.

## Demo de reconhecimento facial

RetinaFace (detecção) + MobileFaceNet (vetor de 128 posições por rosto), com a
câmera ao vivo:

```bash
scripts/build_example.sh luckfox_pico_retinaface_facenet
scripts/deploy.sh third_party/luckfox_pico_rknn_example/install/uclibc/luckfox_pico_retinaface_facenet_demo /root/face
adb shell
cd /root/face
./luckfox_pico_retinaface_facenet model/RetinaFace.rknn model/mobilefacenet.rknn model/test.jpg
```

Sem tela, captura em 240x240 e imprime uma linha por rosto:
`@ (esq topo dir baixo) distância`, onde distância é a euclidiana entre o rosto
visto e o de `test.jpg` (menor = mais parecido). Com o `test.jpg` original
(outra pessoa) deu 1,3–1,5; troque por uma foto de rosto sua para testar.
Ctrl+C para parar (a placa não tem `timeout`).

Para **ver** a câmera com as caixas, o demo RTSP (só detecção):

```bash
scripts/build_example.sh luckfox_pico_rtsp_retinaface
scripts/deploy.sh third_party/luckfox_pico_rkmpi_example/install/uclibc/luckfox_pico_rtsp_retinaface_demo /root/rtsp_face
adb shell 'cd /root/rtsp_face && ./luckfox_pico_rtsp_retinaface'
```

O vídeo sai em `rtsp://<placa>:554/live/0`. Pela USB não há rede, então
encaminhe pelo adb (`adb forward tcp:8554 tcp:554`) e abra
`rtsp://127.0.0.1:8554/live/0` com RTSP sobre TCP (`ffplay -rtsp_transport tcp
...` ou VLC). Ainda não testado.
`scripts/build_example.sh` sem argumentos lista os outros exemplos. O aviso
`OpenCV: Include directory doesn't exist` no cmake deles é do repositório e não
atrapalha.

## Converter modelos (opcional, não instalado)

Modelos novos (ONNX → `.rknn`) são convertidos no PC com o **RKNN-Toolkit2
1.6.0**, a mesma versão do `librknnmrt` que vai junto com os programas: modelo
gerado pelo toolkit 2.x falha no `rknn_init` com `incompatible model version`.
O 1.6.0 só tem wheels para Python 3.6–3.11 (o Ubuntu 24.04 tem 3.12) e puxa
torch 1.13.1 + tensorflow 2.8.0. Wheels em
<https://github.com/rockchip-linux/rknn-toolkit2/tree/v1.6.0/rknn-toolkit2/packages>;
o script de conversão dos modelos de rosto está em
`third_party/luckfox_pico_rknn_example/scripts/luckfox_onnx_to_rknn/`.
