"""
Protocolo UART do Darvaza (espelho de components/proto/include/proto.hpp).
Só biblioteca padrão: serve para o PC e para a Raspberry Pi.

Frame (little-endian):
    A5 5A | ch u8 | op u8 | seq u16 | len u16 | payload | crc16 u16
    crc16 = CRC-16/CCITT-FALSE (binascii.crc_hqx com 0xFFFF) de ch até o fim do payload

Canais:
    CMD   host -> ESP   comando (op = Op); o host escolhe o seq
    RSP   ESP -> host   resposta: mesmo op e seq; payload[0] = Status
    EVT   ESP -> host   notificação espontânea (op = Evt)
    AUDIO ESP -> host   PCM 16 kHz estéreo i16 (L,R); seq conta blocos (buraco = perda)
    LOG   ESP -> host   texto do ESP_LOG, em fluxo: junte os payloads e quebre em '\\n'

Comandos (payload -> resposta além do status):
    PING          -                         -> versão u8
    STATS         -                         -> Stats (stats_decode)
    RESET         -                         -> (reinicia)
    ANIM_PLAY     id u16 (0 = automático)   -> NOT_FOUND / BAD_ARG / BUSY
    BACKLIGHT     % u8                      -> BAD_ARG
    STREAM        on u8                     -> -
    AUDIO_CFG     chave u8, valor f32       -> BAD_ARG  (gravado na NVS)
    UPLOAD_BEGIN  id u16, tamanho u32, crc32 u32   (id 1..0x7FFF)
    UPLOAD_CHUNK  offset u32 + até 1024 B   -> recebidos u32 (BAD_ARG traz o offset esperado)
    UPLOAD_END    -                         -> BAD_CRC / BAD_STATE
    ANIM_LIST     -                         -> n u16, ids u16 x n
    ANIM_DELETE   id u16                    -> NOT_FOUND
"""

import binascii
import json
import struct
import zlib
from dataclasses import dataclass
from enum import IntEnum

VERSION = 1
SOF = b"\xA5\x5A"
HEADER = 8
TRAILER = 2
MAX_PAYLOAD = 1040
CHUNK_MAX = 1024


class Ch(IntEnum):
    CMD = 1
    RSP = 2
    EVT = 3
    AUDIO = 4
    LOG = 5


class Op(IntEnum):
    PING = 0x01
    STATS = 0x02
    RESET = 0x03
    ANIM_PLAY = 0x10
    BACKLIGHT = 0x11
    STREAM = 0x20
    AUDIO_CFG = 0x21
    UPLOAD_BEGIN = 0x30
    UPLOAD_CHUNK = 0x31
    UPLOAD_END = 0x32
    ANIM_LIST = 0x33
    ANIM_DELETE = 0x34


class Evt(IntEnum):
    BOOT = 0x80
    SOUND = 0x81
    ANIM_DONE = 0x82


class Status(IntEnum):
    OK = 0
    UNKNOWN_OP = 1
    BAD_LEN = 2
    BUSY = 3
    BAD_ARG = 4
    NOT_FOUND = 5
    NO_SPACE = 6
    BAD_STATE = 7
    BAD_CRC = 8
    STORAGE = 9


class Cfg(IntEnum):
    TRIGGER_DB = 1     # dB acima do fundo que contam como som (0..40)
    MIC_POLARITY = 2   # +1 ou -1
    TDOA_TRIM = 3      # amostras (-8..8)
    STREAM_SHIFT = 4   # 24 -> 16 bit: 8 = sem ganho, 7 = +6 dB (0..8)


# Animações embutidas no firmware
BUILTIN = {"dormir": 0x8001, "susto": 0x8002, "piscadela": 0x8003, "procurar": 0x8004}

RESET_REASONS = ["UNKNOWN", "POWERON", "EXT", "SW", "PANIC", "INT_WDT", "TASK_WDT", "WDT",
                 "DEEPSLEEP", "BROWNOUT", "SDIO"]


def crc16(data):
    return binascii.crc_hqx(data, 0xFFFF)


def crc32(data):
    return zlib.crc32(data) & 0xFFFFFFFF


def encode(ch, op, seq, payload=b""):
    body = struct.pack("<BBHH", ch, op, seq & 0xFFFF, len(payload)) + bytes(payload)
    return SOF + body + struct.pack("<H", crc16(body))


@dataclass
class Frame:
    ch: int
    op: int
    seq: int
    payload: bytes


class Parser:
    """Separa os frames do lixo entre eles (boot a 115200, panic).
    feed(bytes) -> lista de Frame; o lixo fica em .junk (take_junk())."""

    def __init__(self):
        self.buf = bytearray()
        self.junk = bytearray()
        self.bad = 0

    def feed(self, data):
        self.buf += data
        out = []
        while True:
            i = self.buf.find(SOF)
            if i < 0:
                # guarda o último byte: pode ser o começo de um SOF
                keep = 1 if self.buf.endswith(SOF[:1]) else 0
                self.junk += self.buf[:len(self.buf) - keep]
                del self.buf[:len(self.buf) - keep]
                return out
            if i > 0:
                self.junk += self.buf[:i]
                del self.buf[:i]
            if len(self.buf) < HEADER:
                return out
            ch, op, seq, n = struct.unpack_from("<BBHH", self.buf, 2)
            if n > MAX_PAYLOAD:
                self.bad += 1
                self.junk += self.buf[:1]
                del self.buf[:1]
                continue
            total = HEADER + n + TRAILER
            if len(self.buf) < total:
                return out
            (crc,) = struct.unpack_from("<H", self.buf, HEADER + n)
            if crc16(bytes(self.buf[2:HEADER + n])) != crc:
                # não era frame (ou chegou corrompido): procura o próximo SOF
                self.bad += 1
                self.junk += self.buf[:1]
                del self.buf[:1]
                continue
            out.append(Frame(ch, op, seq, bytes(self.buf[HEADER:HEADER + n])))
            del self.buf[:total]

    def take_junk(self):
        j = bytes(self.junk)
        self.junk.clear()
        return j


# ---------------------------------------------------------------------------
# Payloads
# ---------------------------------------------------------------------------
STATS_FMT = "<IIIIHHHH" + "HBBH" * 4
AO_NAMES = ("comm", "audio", "display", "storage")


def stats_decode(p):
    v = struct.unpack_from(STATS_FMT, p)
    s = {"uptime_ms": v[0], "heap_free": v[1], "heap_min": v[2], "blob_bytes": v[3],
         "rx_frames": v[4], "rx_bad": v[5], "audio_drop": v[6], "log_drop": v[7], "ao": {}}
    for i, name in enumerate(AO_NAMES):
        stack, queued, peak, dropped = v[8 + 4 * i: 12 + 4 * i]
        s["ao"][name] = {"stack_free": stack, "queued": queued, "queue_peak": peak, "dropped": dropped}
    return s


def evt_decode(op, p):
    """Evento -> (nome, dict)"""
    if op == Evt.BOOT:
        proto_ver, reason = struct.unpack_from("<BB", p)
        r = RESET_REASONS[reason] if reason < len(RESET_REASONS) else str(reason)
        return "boot", {"proto": proto_ver, "reset": r}
    if op == Evt.SOUND:
        s, sigma, coh, level_db = struct.unpack_from("<ffff", p)
        return "sound", {"s": s, "sigma": sigma, "coherence": coh, "level_db": level_db}
    if op == Evt.ANIM_DONE:
        (anim_id,) = struct.unpack_from("<H", p)
        return "anim_done", {"id": anim_id}
    return f"evt_{op:#x}", {"raw": p.hex()}


# ---------------------------------------------------------------------------
# Animação v1 (ver components/display/anim.hpp)
# ---------------------------------------------------------------------------
ANIM_MAGIC = 0x4E41
ANIM_VERSION = 1
ANIM_LOOP = 1
ANIM_HOLD = 2
ANIM_MAX_BYTES = 4064


def anim_build(keys, loop=False, hold=False):
    """keys: lista de dicts {ms, gx, gy, pupil, lid | lid_l, lid_r}. Campo omitido
    repete o da key anterior (a primeira parte de gx=gy=0, pupil=26, olhos abertos).
    gx/gy/pupil em px; lid 0 = aberto .. 255 = fechado."""
    if not keys:
        raise ValueError("animação sem keys")
    cur = {"ms": 0, "gx": 0, "gy": 0, "pupil": 26, "lid_l": 0, "lid_r": 0}
    body = b""
    for i, k in enumerate(keys):
        k = dict(k)
        if "lid" in k:
            k.setdefault("lid_l", k["lid"])
            k.setdefault("lid_r", k.pop("lid"))
        unknown = set(k) - set(cur)
        if unknown:
            raise ValueError(f"key {i}: campo desconhecido {sorted(unknown)}")
        cur.update(k)
        for name, lo, hi in (("ms", 0, 65535), ("gx", -128, 127), ("gy", -128, 127), ("pupil", 0, 255),
                             ("lid_l", 0, 255), ("lid_r", 0, 255)):
            if not lo <= int(cur[name]) <= hi:
                raise ValueError(f"key {i}: {name}={cur[name]} fora de {lo}..{hi}")
        body += struct.pack("<HbbBBBB", int(cur["ms"]), int(cur["gx"]), int(cur["gy"]), int(cur["pupil"]),
                            int(cur["lid_l"]), int(cur["lid_r"]), 0)
    flags = (ANIM_LOOP if loop else 0) | (ANIM_HOLD if hold else 0)
    if loop and sum(struct.unpack_from("<H", body, 8 * i)[0] for i in range(len(keys))) == 0:
        raise ValueError("loop com duração total zero")
    data = struct.pack("<HBBHH", ANIM_MAGIC, ANIM_VERSION, flags, len(keys), 0) + body
    if len(data) > ANIM_MAX_BYTES:
        raise ValueError(f"animação com {len(data)} B; máximo {ANIM_MAX_BYTES}")
    return data


def anim_from_json(path):
    with open(path, encoding="utf-8") as f:
        d = json.load(f)
    return anim_build(d["keys"], loop=d.get("loop", False), hold=d.get("hold", False))
