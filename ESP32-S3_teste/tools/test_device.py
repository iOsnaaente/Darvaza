#!/usr/bin/env python3
"""
Teste de integração com o firmware rodando (placa ou QEMU):

    python test_device.py                         # porta detectada
    python test_device.py -p socket://localhost:5555

Grava e apaga uma animação de teste (id 0x7F00) e reinicia o ESP uma vez.
Não altera os parâmetros gravados (só manda valores inválidos para eles).
"""

import argparse
import queue
import struct
import sys
import time

import proto
from darvaza_cli import Device, DeviceError, find_port
from proto import Op, Status

TEST_ID = 0x7F00
RESULTS = []


def check(name, ok, detail=""):
    RESULTS.append(ok)
    print(f"  [{'ok' if ok else 'FALHOU'}] {name}" + (f"  ({detail})" if detail and not ok else ""))


def expect_status(name, fn, status):
    try:
        fn()
    except DeviceError as e:
        return check(name, e.status == status, f"veio {e}")
    check(name, False, "veio OK")


def wait_evt(events, name, pred=lambda d: True, timeout=3.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            n, d = events.get(timeout=deadline - time.monotonic())
        except queue.Empty:
            break
        if n == name and pred(d):
            return d
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-p", "--port")
    ap.add_argument("-b", "--baud", type=int, default=2000000)
    args = ap.parse_args()

    events, logs, audio = queue.Queue(), [], {"n": 0}
    dev = Device(args.port or find_port(), args.baud,
                 on_evt=lambda n, d: events.put((n, d)),
                 on_log=logs.append,
                 on_audio=lambda seq, p: audio.__setitem__("n", audio["n"] + 1))
    try:
        print("transporte")
        check("ping", dev.ping() == proto.VERSION)
        expect_status("op desconhecido -> UNKNOWN_OP", lambda: dev.request(0x7E), Status.UNKNOWN_OP)
        expect_status("tamanho errado -> BAD_LEN", lambda: dev.request(Op.ANIM_PLAY, b"\x01"), Status.BAD_LEN)
        s = dev.stats()
        check("stats: as 4 tasks rodando", all(a["stack_free"] > 0 for a in s["ao"].values()), s["ao"])

        print("áudio")
        dev.stream(True)
        n0 = audio["n"]
        time.sleep(0.5)
        got = audio["n"] - n0
        check("stream ligado: ~31 blocos em 0,5 s", 20 <= got <= 40, f"{got} blocos")
        dev.stream(False)
        time.sleep(0.1)
        n0 = audio["n"]
        time.sleep(0.4)
        check("stream desligado: nenhum bloco", audio["n"] == n0, f"{audio['n'] - n0} blocos")
        dev.stream(True)
        expect_status("cfg fora da faixa -> BAD_ARG",
                      lambda: dev.request(Op.AUDIO_CFG, struct.pack("<Bf", proto.Cfg.TRIGGER_DB, 99.0)), Status.BAD_ARG)
        expect_status("cfg chave desconhecida -> BAD_ARG",
                      lambda: dev.request(Op.AUDIO_CFG, struct.pack("<Bf", 0x55, 1.0)), Status.BAD_ARG)
        print("      bata palmas perto de um dos mics...")
        snd = wait_evt(events, "sound", timeout=5.0)
        if snd:
            check("EVT_SOUND chega quando há som", True)
            print(f"      som: s={snd['s']:+.2f} sigma={snd['sigma']:.2f} coer={snd['coherence']:.2f}")
        else:
            print("  [--] nenhum EVT_SOUND em 5 s (sala quieta? não conta como falha)")

        print("display")
        expect_status("backlight 101 -> BAD_ARG", lambda: dev.request(Op.BACKLIGHT, b"\x65"), Status.BAD_ARG)
        dev.play(proto.BUILTIN["piscadela"])
        check("embutida termina com EVT_ANIM_DONE",
              wait_evt(events, "anim_done", lambda d: d["id"] == proto.BUILTIN["piscadela"]) is not None)
        expect_status("embutida inexistente -> NOT_FOUND", lambda: dev.play(0x80FF), Status.NOT_FOUND)
        expect_status("gravada inexistente -> NOT_FOUND", lambda: dev.play(TEST_ID + 1), Status.NOT_FOUND)

        print("storage")
        try:
            dev.anim_delete(TEST_ID)   # sobra de um teste anterior
        except DeviceError:
            pass
        data = proto.anim_build([{"ms": 150, "gx": 40, "lid_l": 255}, {"ms": 150, "gx": 0, "lid_l": 0}])
        t0 = time.monotonic()
        dev.upload(TEST_ID, data)
        check("upload", TEST_ID in dev.anim_list(), f"{time.monotonic() - t0:.2f} s")
        dev.play(TEST_ID)
        check("gravada toca e termina", wait_evt(events, "anim_done", lambda d: d["id"] == TEST_ID) is not None)

        big = proto.anim_build([{"ms": 5 + i % 10, "gx": (i * 7) % 100 - 50} for i in range(300)])   # 2408 B: 3 chunks, ~3 s
        dev.upload(TEST_ID, big)
        check("substitui (upload em 3 chunks)", dev.anim_list().count(TEST_ID) == 1)

        dev.request(Op.UPLOAD_BEGIN, struct.pack("<HII", TEST_ID, len(data), proto.crc32(data) ^ 1), timeout=1.0)
        dev.request(Op.UPLOAD_CHUNK, struct.pack("<I", 0) + data)
        expect_status("CRC errado -> BAD_CRC", lambda: dev.request(Op.UPLOAD_END), Status.BAD_CRC)
        dev.request(Op.UPLOAD_BEGIN, struct.pack("<HII", TEST_ID, len(data), proto.crc32(data)), timeout=1.0)
        try:
            dev.request(Op.UPLOAD_CHUNK, struct.pack("<I", 8) + data[8:])
            check("chunk fora de ordem -> BAD_ARG com o offset esperado", False, "veio OK")
        except DeviceError as e:
            check("chunk fora de ordem -> BAD_ARG com o offset esperado",
                  e.status == Status.BAD_ARG and struct.unpack("<I", e.payload[:4])[0] == 0, str(e))
        expect_status("END incompleto -> BAD_STATE", lambda: dev.request(Op.UPLOAD_END), Status.BAD_STATE)
        expect_status("id 0 -> BAD_ARG",
                      lambda: dev.request(Op.UPLOAD_BEGIN, struct.pack("<HII", 0, 8, 0)), Status.BAD_ARG)
        expect_status("maior que um slot -> BAD_ARG",
                      lambda: dev.request(Op.UPLOAD_BEGIN, struct.pack("<HII", TEST_ID, 5000, 0)), Status.BAD_ARG)

        print("reinício")
        while not events.empty():
            events.get()
        dev.reset()
        boot = wait_evt(events, "boot", timeout=10.0)
        check("EVT_BOOT depois do RESET", boot is not None and boot["reset"] == "SW", boot)
        time.sleep(0.5)
        check("animação continua gravada depois do reboot", TEST_ID in dev.anim_list())
        dev.play(TEST_ID)
        check("e toca", wait_evt(events, "anim_done", lambda d: d["id"] == TEST_ID, timeout=20.0) is not None)
        dev.anim_delete(TEST_ID)
        check("apagada", TEST_ID not in dev.anim_list())

        s = dev.stats()
        dropped = {k: v["dropped"] for k, v in s["ao"].items()}
        check("nenhum evento descartado", not any(dropped.values()), dropped)
        check("frames ruins no RX = 0", s["rx_bad"] == 0, s["rx_bad"])
        print(f"      heap livre {s['heap_free']} (mín {s['heap_min']}), filas: " +
              ", ".join(f"{k} pico {v['queue_peak']}" for k, v in s["ao"].items()))
        check("log chegando em frames LOG", any("eyes" in line for line in logs), f"{len(logs)} linhas")
    except (DeviceError, TimeoutError) as e:
        check("sem erro inesperado", False, str(e))
    finally:
        dev.close()

    print(f"{sum(RESULTS)}/{len(RESULTS)} ok")
    sys.exit(0 if all(RESULTS) else 1)


if __name__ == "__main__":
    main()
