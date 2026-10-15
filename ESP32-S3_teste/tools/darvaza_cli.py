#!/usr/bin/env python3
"""
Cliente do Darvaza pela UART: biblioteca (classe Device, para a Raspberry Pi)
e linha de comando.

    pip install pyserial
    python darvaza_cli.py monitor                 # log + eventos (padrão)
    python darvaza_cli.py ping | stats | reset
    python darvaza_cli.py play susto              # embutidas: dormir susto piscadela procurar
    python darvaza_cli.py play 12                 # gravada; "play auto" volta ao automático
    python darvaza_cli.py upload 12 anims/exemplo.json
    python darvaza_cli.py list | delete 12
    python darvaza_cli.py backlight 60
    python darvaza_cli.py stream off
    python darvaza_cli.py cfg trigger_db 8        # tdoa_trim, mic_polarity, stream_shift
    python darvaza_cli.py anim anims/exemplo.json -o exemplo.bin   # só compila, sem placa

Na Pi:
    from darvaza_cli import Device
    dev = Device("/dev/serial0", on_evt=lambda nome, d: print(nome, d))
    dev.play(0x8002)
"""

import argparse
import math
import os
import random
import struct
import sys
import threading
import time

import proto
from proto import Ch, Op, Status

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    serial = None


def _name(enum, v):
    try:
        return enum(v).name
    except ValueError:
        return f"{v:#x}"


class DeviceError(Exception):
    def __init__(self, op, status, payload=b""):
        self.op, self.status, self.payload = op, status, payload
        super().__init__(f"{_name(Op, op)}: {_name(Status, status)}")


class Device:
    """Fala com o firmware. Uma thread lê a serial e despacha: RSP acorda quem
    esperava aquele seq; EVT/LOG/AUDIO vão para os callbacks (rodam na thread
    de leitura: não bloqueie neles)."""

    def __init__(self, port, baud=2000000, on_evt=None, on_log=None, on_audio=None, on_junk=None):
        if serial is None:
            sys.exit("falta o pyserial: pip install pyserial")
        # Aceita também URL: socket://host:porta (QEMU, ser2net), rfc2217://...
        self.ser = serial.serial_for_url(port, baudrate=baud, timeout=0.05)
        self.parser = proto.Parser()
        self.on_evt, self.on_log, self.on_audio, self.on_junk = on_evt, on_log, on_audio, on_junk
        self._seq = random.randrange(0x10000)
        self._waiting = set()   # seqs com alguém esperando a resposta
        self._rsp = {}
        self._cv = threading.Condition()
        self._wlock = threading.Lock()
        self._log = bytearray()
        self.audio_lost = 0
        self._audio_seq = None
        self._running = True
        self._thread = threading.Thread(target=self._reader, daemon=True)
        self._thread.start()

    def close(self):
        self._running = False
        self._thread.join(timeout=1)
        self.ser.close()

    # ---- leitura ---------------------------------------------------------
    def _reader(self):
        while self._running:
            try:
                data = self.ser.read(max(1, self.ser.in_waiting))
            except serial.SerialException:
                break
            if not data:
                continue
            for f in self.parser.feed(data):
                self._dispatch(f)
            junk = self.parser.take_junk()
            if junk and self.on_junk:
                self.on_junk(junk)

    def _dispatch(self, f):
        if f.ch == Ch.RSP:
            with self._cv:
                if f.seq in self._waiting:   # resposta atrasada de quem já desistiu: descarta
                    self._rsp[f.seq] = f
                    self._cv.notify_all()
        elif f.ch == Ch.EVT:
            if self.on_evt:
                self.on_evt(*proto.evt_decode(f.op, f.payload))
        elif f.ch == Ch.LOG:
            self._log += f.payload
            while b"\n" in self._log:
                line, _, rest = self._log.partition(b"\n")
                self._log = bytearray(rest)
                if self.on_log:
                    self.on_log(line.decode("utf-8", "replace").rstrip("\r"))
        elif f.ch == Ch.AUDIO:
            if self._audio_seq is not None:
                gap = (f.seq - self._audio_seq - 1) & 0xFFFF
                if 0 < gap < 1000:
                    self.audio_lost += gap
            self._audio_seq = f.seq
            if self.on_audio:
                self.on_audio(f.seq, f.payload)

    # ---- comandos --------------------------------------------------------
    def request(self, op, payload=b"", timeout=0.5, retries=3):
        """Manda o comando e espera a resposta. Devolve o payload depois do
        status; status != OK vira DeviceError. BUSY e silêncio: tenta de novo."""
        for attempt in range(retries + 1):
            with self._cv:
                self._seq = (self._seq + 1) & 0xFFFF
                seq = self._seq
                self._waiting.add(seq)
            with self._wlock:
                self.ser.write(proto.encode(Ch.CMD, op, seq, payload))
            deadline = time.monotonic() + timeout
            with self._cv:
                while seq not in self._rsp:
                    left = deadline - time.monotonic()
                    if left <= 0:
                        break
                    self._cv.wait(left)
                self._waiting.discard(seq)
                f = self._rsp.pop(seq, None)
            if f is None:
                continue   # sem resposta: manda de novo com outro seq
            status, rest = f.payload[0], f.payload[1:]
            if status == Status.BUSY and attempt < retries:
                time.sleep(0.05 * (attempt + 1))
                continue
            if status != Status.OK:
                raise DeviceError(op, status, rest)
            return rest
        raise TimeoutError(f"{_name(Op, op)}: sem resposta")

    def ping(self):
        return self.request(Op.PING)[0]

    def stats(self):
        return proto.stats_decode(self.request(Op.STATS))

    def reset(self):
        self.request(Op.RESET, retries=0)

    def play(self, anim_id):
        # gravada: o storage lê da flash antes de responder
        self.request(Op.ANIM_PLAY, struct.pack("<H", anim_id), timeout=1.0)

    def backlight(self, percent):
        self.request(Op.BACKLIGHT, struct.pack("<B", percent))

    def stream(self, on):
        self.request(Op.STREAM, struct.pack("<B", 1 if on else 0))

    def cfg(self, key, value):
        self.request(Op.AUDIO_CFG, struct.pack("<Bf", key, value))

    def anim_list(self):
        p = self.request(Op.ANIM_LIST)
        (n,) = struct.unpack_from("<H", p)
        return list(struct.unpack_from(f"<{n}H", p, 2))

    def anim_delete(self, anim_id):
        self.request(Op.ANIM_DELETE, struct.pack("<H", anim_id))

    def upload(self, anim_id, data, progress=None):
        # BEGIN apaga um setor da flash (~45 ms)
        self.request(Op.UPLOAD_BEGIN, struct.pack("<HII", anim_id, len(data), proto.crc32(data)), timeout=1.0)
        off = 0
        while off < len(data):
            chunk = data[off:off + proto.CHUNK_MAX]
            try:
                got = struct.unpack("<I", self.request(Op.UPLOAD_CHUNK, struct.pack("<I", off) + chunk))[0]
            except DeviceError as e:
                if e.status != Status.BAD_ARG or len(e.payload) < 4:
                    raise
                got = struct.unpack("<I", e.payload[:4])[0]   # dessincronizou: continua de onde o ESP parou
                if got == off:
                    raise
            off = got
            if progress:
                progress(off, len(data))
        self.request(Op.UPLOAD_END, timeout=1.0)


# ---------------------------------------------------------------------------
# Linha de comando
# ---------------------------------------------------------------------------
def find_port():
    ports = list(serial.tools.list_ports.comports())
    for p in ports:   # CH343/CH340 (1a86), CP210x (10c4), USB nativo ESP (303a)
        if p.vid in (0x1A86, 0x10C4, 0x303A):
            return p.device
    if ports:
        return ports[0].device
    sys.exit("nenhuma porta serial encontrada; use -p")


def parse_id(s):
    if s in ("auto", "automatico"):
        return 0
    if s in proto.BUILTIN:
        return proto.BUILTIN[s]
    return int(s, 0)


def load_anim(path):
    if path.endswith(".json"):
        return proto.anim_from_json(path)
    with open(path, "rb") as f:
        return f.read()


class JunkPrinter:
    """Texto fora de frame (panic/backtrace a 2 Mbaud) vira linha; o boot da ROM
    a 115200 chega como lixo e é filtrado."""

    def __init__(self):
        self.buf = bytearray()

    def __call__(self, data):
        self.buf += data
        while b"\n" in self.buf:
            line, _, rest = self.buf.partition(b"\n")
            self.buf = bytearray(rest)
            text = line.decode("latin-1").rstrip("\r")
            printable = sum(32 <= ord(c) < 127 for c in text)
            if text.strip() and printable >= 0.9 * len(text):
                print(f"[fora de frame] {text}")
        if len(self.buf) > 1000:
            self.buf.clear()


def print_evt(name, d):
    if name == "sound":
        ang = math.degrees(math.asin(max(-1.0, min(1.0, d["s"]))))
        print(f"[som] ângulo {ang:+4.0f}° ±{d['sigma']:.2f}  coer {d['coherence']:.2f}  {d['level_db']:.1f} dBFS")
    else:
        print(f"[{name}] {d}")


def main():
    ap = argparse.ArgumentParser(description="Cliente do Darvaza pela UART")
    ap.add_argument("-p", "--port", help="porta serial ou URL socket://host:porta (padrão: detecta)")
    ap.add_argument("-b", "--baud", type=int, default=2000000)
    sub = ap.add_subparsers(dest="cmd")
    sub.add_parser("monitor", help="mostra log e eventos (padrão)")
    sub.add_parser("ping")
    sub.add_parser("stats")
    sub.add_parser("reset")
    sub.add_parser("list", help="animações gravadas")
    p = sub.add_parser("play", help="toca animação: id, nome embutido ou 'auto'")
    p.add_argument("id")
    p = sub.add_parser("delete")
    p.add_argument("id")
    p = sub.add_parser("upload", help="grava animação (.json ou .bin)")
    p.add_argument("id")
    p.add_argument("file")
    p = sub.add_parser("backlight")
    p.add_argument("percent", type=int)
    p = sub.add_parser("stream")
    p.add_argument("state", choices=["on", "off"])
    p = sub.add_parser("cfg", help="parâmetro do áudio (gravado na NVS)")
    p.add_argument("key", choices=[k.name.lower() for k in proto.Cfg])
    p.add_argument("value", type=float)
    p = sub.add_parser("anim", help="compila .json -> .bin (sem placa)")
    p.add_argument("file")
    p.add_argument("-o", "--out")
    args = ap.parse_args()
    cmd = args.cmd or "monitor"

    if cmd == "anim":
        data = proto.anim_from_json(args.file)
        out = args.out or os.path.splitext(args.file)[0] + ".bin"
        with open(out, "wb") as f:
            f.write(data)
        print(f"{out}: {len(data)} B, crc32 {proto.crc32(data):08x}")
        return

    if serial is None:
        sys.exit("falta o pyserial: pip install pyserial")
    port = args.port or find_port()
    monitor = cmd == "monitor"
    dev = Device(port, args.baud,
                 on_evt=print_evt if monitor else None,
                 on_log=print if monitor else None,
                 on_junk=JunkPrinter() if monitor else None)
    try:
        if monitor:
            print(f"[{port} @ {args.baud}]  Ctrl+C para sair")
            while True:
                time.sleep(1)
        elif cmd == "ping":
            t0 = time.monotonic()
            v = dev.ping()
            print(f"protocolo v{v}, {1000 * (time.monotonic() - t0):.1f} ms")
        elif cmd == "stats":
            s = dev.stats()
            print(f"uptime {s['uptime_ms'] / 1000:.1f} s  heap {s['heap_free']} (mín {s['heap_min']})  "
                  f"blobs {s['blob_bytes']} B")
            print(f"rx {s['rx_frames']} frames ({s['rx_bad']} ruins)  áudio perdido {s['audio_drop']} blocos  "
                  f"log perdido {s['log_drop']} linhas")
            for name, a in s["ao"].items():
                print(f"  {name:8s} pilha livre {a['stack_free']:5d}  fila {a['queued']} (pico {a['queue_peak']})  "
                      f"descartados {a['dropped']}")
        elif cmd == "reset":
            dev.reset()
        elif cmd == "list":
            ids = dev.anim_list()
            print(" ".join(str(i) for i in ids) if ids else "(nenhuma)")
        elif cmd == "play":
            dev.play(parse_id(args.id))
        elif cmd == "delete":
            dev.anim_delete(parse_id(args.id))
        elif cmd == "upload":
            data = load_anim(args.file)
            t0 = time.monotonic()
            dev.upload(parse_id(args.id), data,
                       progress=lambda o, n: print(f"\r{o}/{n} B", end="", flush=True))
            print(f"\nok: {len(data)} B em {time.monotonic() - t0:.2f} s")
        elif cmd == "backlight":
            dev.backlight(args.percent)
        elif cmd == "stream":
            dev.stream(args.state == "on")
        elif cmd == "cfg":
            dev.cfg(proto.Cfg[args.key.upper()], args.value)
        if not monitor:
            print("ok")
    except (DeviceError, TimeoutError) as e:
        sys.exit(f"erro: {e}")
    except KeyboardInterrupt:
        pass
    finally:
        dev.close()


if __name__ == "__main__":
    main()
