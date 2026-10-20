# asr_bench — reconhecimento de fala na Luckfox Pico Max

Mede dois reconhecedores de fala em português rodando **na CPU** da placa:
um único Cortex-A7, porque o exemplo de Whisper da Rockchip para a NPU não
suporta o RV1106.

| motor | modelo | tamanho | como roda |
| --- | --- | --- | --- |
| `whisper-tiny` | whisper.cpp v1.9.5, `ggml-tiny-q8_0.bin` (multilíngue, quantizado em 8 bits) | 42 MB | uClibc, como o resto |
| `vosk-small-pt` | Vosk 0.3.45, `vosk-model-small-pt-0.3` (pt-BR; é o único Vosk pequeno de português) | 52 MB | glibc levada junto (abaixo) |

Cada um também roda numa variante "afinada", com o mesmo modelo e outro ajuste
(`engines.tsv`):

- **`vosk-small-pt-rapido`:** o `small-pt-0.3` está no formato antigo dos
  modelos Vosk (sem `conf/model.conf`). Nesse formato a libvosk fixa uma busca
  larga no decodificador (`--max-active=7000 --beam=13 --lattice-beam=6`). A
  variante monta o mesmo modelo no formato novo, só com links simbólicos (0 MB
  a mais), e usa a busca dos modelos pequenos novos do Vosk
  (`vosk-rapido.conf`: 3000 / 10 / 2).
- **`whisper-tiny-ac`:** o encoder do Whisper sempre processa uma janela de
  30 s, mesmo para 5 s de fala; na placa isso é ~95% do tempo. Com `--ac auto`
  a janela vira a duração da fala + 0,6 s. O modelo foi treinado com 30 s e
  pode errar mais assim; a medição mostra quanto.
- **`vosk-comandos`:** o Vosk "rápido" restrito às frases de `comandos.json`
  (gramática), mais `[unk]` para o resto. Serve para um robô que obedece a uma
  lista fechada de comandos: a busca fica minúscula e roda em tempo real. Para
  trocar os comandos, edite `/root/asr_bench/comandos.json` na placa. Só valem
  palavras que o modelo conhece; as outras são ignoradas.

Os resultados aparecem na página **Fala** do `face_app`
(`http://127.0.0.1:8080/asr`, link no topo da página dos rostos).

## Preparar, compilar e mandar

```bash
source env.sh
scripts/asr.sh setup      # uma vez: ~400 MB de downloads em third_party/
scripts/asr.sh deploy     # compila e copia para /root/asr_bench na placa (~106 MB)
scripts/face_app.sh deploy && scripts/face_app.sh start && scripts/face_app.sh web
```

e abra **<http://127.0.0.1:8080/asr>**. O `deploy` usa `adb push --sync`:
depois da primeira vez só copia o que mudou, e os modelos não vão de novo.

Sem a página, `scripts/asr.sh run [motor]` roda os motores nas frases direto
na placa e imprime o JSON. `scripts/asr.sh qemu [motor]` faz o mesmo no PC,
emulado: serve para conferir que o build funciona sem a placa, mas os tempos
não valem nada (o whisper leva ~9 min por frase no qemu).

## A página

- **Medir:** escolhe os motores e as frases e clica em Medir. Roda um motor
  por vez. Cada motor carrega o modelo uma vez e passa por todas as frases.
  "Parar a IA de rostos" (marcado por padrão) tira a detecção de rostos da CPU
  durante a medição; a câmera continua.
- **Gravar uma frase:** grava pelo microfone do PC, reamostra para 16 kHz e
  manda para a placa como `clips/rec-<data>.wav`. O texto da caixa vira a
  referência do WER. O navegador só libera o microfone em `http://127.0.0.1`
  (adb) ou https; pelo IP da rede não funciona.
- **Resumo por motor:** usa a última medição de cada frase.
  - **RTF:** tempo de processamento ÷ duração da fala. Abaixo de 1, acompanha a
    fala em tempo real.
  - **WER:** erros ÷ palavras da referência. A comparação ignora maiúsculas e
    pontuação e escreve os números por extenso, porque o whisper escreve "4" e
    a referência "quatro".
  - Também mostra o pico de RAM, a carga do modelo, o uso da CPU e, no whisper,
    a fatia do encoder e do decoder.
- **Frase a frase:** o texto reconhecido com os erros marcados: palavra
  trocada em vermelho, sobrando em amarelo, faltando riscada.

Os resultados ficam na placa, em `/root/asr_bench/results.jsonl`: uma linha
JSON por frase e motor. O formato está em `src/bench.h`.
`/tmp/asr_bench.log` guarda a saída de erro dos motores.

## Frases de teste

Oito frases da parte `dev` do [FLEURS](https://huggingface.co/datasets/google/fleurs)
pt_br (Google, CC-BY-4.0). São textos da Wikipédia lidos por falantes do
Brasil, de 4 a 12 s cada, num total de 81 s. Vozes masculinas e femininas se
alternam, e nenhuma frase tem números na transcrição. Quem escolhe as frases é
o `tools/fleurs_clips.py`.

## Como o build funciona

- **whisper.cpp** é compilado com o toolchain da Luckfox (GCC 8.3, uClibc),
  com dois ajustes no `CMakeLists.txt`, ambos sem mexer no código dele:
  - A uClibc define `__GLIBC__`, então o ggml inclui `execinfo.h`, que ela não
    tem. A pasta `compat/` traz um `execinfo.h` vazio (só perde a pilha num
    erro fatal).
  - No GCC 8, `std::filesystem` fica em `-lstdc++fs`.
- **Vosk** só existe pronto para armv7 com glibc (2.27 ou mais nova, mais
  libstdc++), e a placa é uClibc. O `asr_vosk` é compilado com o mesmo GCC,
  mas contra um sysroot glibc montado com pacotes do Debian 12, cada um
  conferido pelo SHA-256 do índice (`tools/debian_debs.py`). Na placa, o
  `bin/vosk.sh` chama o loader da glibc direto
  (`glibc/ld-linux-armhf.so.3 --library-path glibc:lib bin/asr_vosk ...`). A
  glibc e a libstdc++ vão em `glibc/` (3 MB) e não se misturam com as do
  sistema.
- **`engines.tsv`** diz o comando de cada motor. Para testar outra variante
  na placa, é só acrescentar uma linha, sem recompilar. Exemplo: o whisper com
  a janela fixa em 15 s (`--ac 768`).
