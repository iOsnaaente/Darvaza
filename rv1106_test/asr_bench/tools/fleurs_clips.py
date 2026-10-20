#!/usr/bin/env python3
"""Separa frases de teste do FLEURS pt_br (Google, CC-BY-4.0): fala lida por
falantes nativos do Brasil, com a transcrição de referência.

Uso: fleurs_clips.py <pasta_de_saida> [quantas=8]

Pega da parte "dev" frases de 4 a 12 s, sem números na transcrição (o
número escrito por extenso ou em algarismos conta como erro à toa),
alternando voz masculina e feminina, uma gravação por frase. Grava
fleurs-NN.wav (16 kHz, mono, 16 bits; o FLEURS vem em float de 32 bits) e
fleurs-NN.txt (a transcrição). O áudio vem num tar.gz de ~250 MB, lido em
fluxo: para assim que achou todas.
"""
import array
import csv
import io
import struct
import sys
import tarfile
import urllib.request
import wave
from pathlib import Path

REV = "70bb2e84b976b7e960aa89f1c648e09c59f894dd"   # google/fleurs, fixo
BASE = f"https://huggingface.co/datasets/google/fleurs/resolve/{REV}/data/pt_br"
MIN_S, MAX_S = 4, 12


def pick(rows, count):
    """Uma gravação por frase, alternando o gênero; ordem fixa (id da frase)."""
    cands = []
    seen = set()
    for r in sorted(rows, key=lambda r: (int(r[0]), r[1])):
        sid, fname, raw, norm, samples, gender = r[0], r[1], r[2], r[3], int(r[5]), r[6]
        if sid in seen or not MIN_S * 16000 <= samples <= MAX_S * 16000 or any(c.isdigit() for c in norm):
            continue
        seen.add(sid)
        cands.append((fname, raw, gender))
    out = []
    want = "MALE"
    while len(out) < count and cands:
        c = next((c for c in cands if c[2] == want), cands[0])
        cands.remove(c)
        out.append(c)
        want = "FEMALE" if want == "MALE" else "MALE"
    return out


def to_pcm16(data):
    """WAV do FLEURS (16 kHz mono, float32 ou PCM 16) -> amostras PCM 16."""
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise ValueError("não é WAV")
    pos, fmt, samples = 12, None, None
    while pos + 8 <= len(data):
        cid, size = data[pos:pos + 4], struct.unpack_from("<I", data, pos + 4)[0]
        body = data[pos + 8:pos + 8 + size]
        if cid == b"fmt ":
            fmt = struct.unpack_from("<HHIIHH", body)
        elif cid == b"data":
            samples = body
            break
        pos += 8 + size + (size & 1)
    if not fmt or samples is None:
        raise ValueError("WAV sem fmt/data")
    tag, channels, rate, _, _, bits = fmt
    if channels != 1 or rate != 16000:
        raise ValueError(f"esperava 16 kHz mono, veio {rate} Hz x{channels}")
    if tag == 1 and bits == 16:
        return array.array("h", samples[:len(samples) // 2 * 2])
    if tag == 3 and bits == 32:
        f = array.array("f", samples[:len(samples) // 4 * 4])
        return array.array("h", (max(-32768, min(32767, round(v * 32767))) for v in f))
    raise ValueError(f"formato {tag}/{bits} bits não suportado")


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    out_dir = Path(sys.argv[1])
    count = int(sys.argv[2]) if len(sys.argv) > 2 else 8
    out_dir.mkdir(parents=True, exist_ok=True)

    with urllib.request.urlopen(f"{BASE}/dev.tsv") as r:
        rows = list(csv.reader(io.TextIOWrapper(r, encoding="utf-8"), delimiter="\t", quoting=csv.QUOTE_NONE))
    chosen = pick(rows, count)
    names = {fname: (i + 1, raw) for i, (fname, raw, _) in enumerate(chosen)}

    done = 0
    with urllib.request.urlopen(f"{BASE}/audio/dev.tar.gz") as r, tarfile.open(fileobj=r, mode="r|gz") as tar:
        for m in tar:
            n = Path(m.name).name
            if not m.isfile() or n not in names:
                continue
            pcm = to_pcm16(tar.extractfile(m).read())
            num, raw = names[n]
            with wave.open(str(out_dir / f"fleurs-{num:02d}.wav"), "wb") as w:
                w.setnchannels(1)
                w.setsampwidth(2)
                w.setframerate(16000)
                w.writeframes(pcm.tobytes())
            (out_dir / f"fleurs-{num:02d}.txt").write_text(raw + "\n", encoding="utf-8")
            done += 1
            print(f"fleurs-{num:02d}: {raw}")
            if done == len(names):
                break
    if done != len(names):
        sys.exit(f"só achei {done} de {len(names)} no tar")


if __name__ == "__main__":
    main()
