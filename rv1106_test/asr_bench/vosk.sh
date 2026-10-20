#!/bin/sh
# O asr_vosk é glibc (a libvosk.so pronta é glibc) e a placa é uClibc: roda
# pelo loader da glibc que vai em ../glibc, com as libs de lá e de ../lib.
# ASR_EMU (opcional) vai na frente: o scripts/asr.sh usa para rodar no PC
# pelo qemu-arm.
d=$(cd "$(dirname "$0")/.." && pwd)
exec $ASR_EMU "$d/glibc/ld-linux-armhf.so.3" --library-path "$d/glibc:$d/lib" "$d/bin/asr_vosk" "$@"
