#!/usr/bin/env python3
"""Baixa pacotes do Debian 12 (bookworm), confere e extrai numa pasta.

Uso: debian_debs.py <arquitetura> <pasta> <pacote>...

Cada pacote é conferido pelo SHA-256 do índice do Debian e extraído com
dpkg-deb em <pasta>/root; os .deb ficam em <pasta>/debs. Pega a versão
atual de cada um (o Debian só guarda a última no repositório).

Usado pelo scripts/asr.sh para:
  - armhf: o sysroot glibc do asr_vosk (a libvosk.so pronta é glibc e a
    placa é uClibc): crt, headers e libc.so para compilar, e o runtime que
    vai para a placa (loader, libc, libm, libstdc++, libgcc_s, libatomic);
  - amd64: o qemu-arm, para rodar os programas no PC sem a placa.
"""
import hashlib
import lzma
import subprocess
import sys
import urllib.request
from pathlib import Path

MIRROR = "http://deb.debian.org/debian"
SUITE = "bookworm"


def index(arch, wanted):
    url = f"{MIRROR}/dists/{SUITE}/main/binary-{arch}/Packages.xz"
    with urllib.request.urlopen(url) as r:
        text = lzma.decompress(r.read()).decode("utf-8")
    pkgs = {}
    for block in text.split("\n\n"):
        fields = {}
        for line in block.splitlines():
            if line and line[0] != " " and ":" in line:
                k, v = line.split(":", 1)
                fields[k] = v.strip()
        if fields.get("Package") in wanted:
            pkgs[fields["Package"]] = fields
    return pkgs


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    arch, dest, wanted = sys.argv[1], Path(sys.argv[2]), sys.argv[3:]
    debs = dest / "debs"
    debs.mkdir(parents=True, exist_ok=True)
    pkgs = index(arch, set(wanted))
    for name in wanted:
        p = pkgs.get(name)
        if not p:
            sys.exit(f"{name}: não está no índice do {SUITE} ({arch})")
        deb = debs / Path(p["Filename"]).name
        if not deb.exists() or hashlib.sha256(deb.read_bytes()).hexdigest() != p["SHA256"]:
            print(f"baixando {deb.name}")
            with urllib.request.urlopen(f"{MIRROR}/{p['Filename']}") as r:
                data = r.read()
            if hashlib.sha256(data).hexdigest() != p["SHA256"]:
                sys.exit(f"{deb.name}: SHA-256 não confere com o índice")
            deb.write_bytes(data)
        subprocess.run(["dpkg-deb", "-x", str(deb), str(dest / "root")], check=True)
        print(f"ok: {name} {p['Version']}")


if __name__ == "__main__":
    main()
