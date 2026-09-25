#!/usr/bin/env python3
"""Testes do tools/proto.py (só biblioteca padrão):  python3 -m unittest test_proto"""

import json
import os
import struct
import unittest

import proto
from proto import Ch, Op

# Mesmos vetores de test/host/test_main.cpp: se um lado mudar, o outro quebra
REF_FRAME = bytes.fromhex("a55a0110341202000180b367")   # CMD, ANIM_PLAY, seq 0x1234, id 0x8001
REF_ANIM = bytes.fromhex("414e010001000000" "64000a001a00ff00")   # 1 key: 100 ms, gx 10, pupila 26, lid_r 255

HERE = os.path.dirname(os.path.abspath(__file__))


class TestProto(unittest.TestCase):
    def test_crc(self):
        self.assertEqual(proto.crc16(b"123456789"), 0x29B1)
        self.assertEqual(proto.crc32(b"123456789"), 0xCBF43926)

    def test_encode(self):
        self.assertEqual(proto.encode(Ch.CMD, Op.ANIM_PLAY, 0x1234, struct.pack("<H", 0x8001)), REF_FRAME)

    def test_parser_junk_and_split(self):
        p = proto.Parser()
        ping = proto.encode(Ch.RSP, Op.PING, 7, b"\x00\x01")
        stream = b"boot\xa5 lixo" + REF_FRAME + b"\xa5" + ping
        frames = []
        for i in range(len(stream)):   # um byte por vez
            frames += p.feed(stream[i:i + 1])
        self.assertEqual([(f.ch, f.op, f.seq) for f in frames], [(Ch.CMD, Op.ANIM_PLAY, 0x1234), (Ch.RSP, Op.PING, 7)])
        self.assertEqual(frames[0].payload, b"\x01\x80")
        self.assertEqual(p.take_junk(), b"boot\xa5 lixo\xa5")
        self.assertEqual(p.bad, 0)

    def test_parser_resync_after_corruption(self):
        # Diferente do firmware, o parser do host procura o próximo SOF dentro do frame ruim
        p = proto.Parser()
        bad = bytearray(REF_FRAME)
        bad[8] ^= 0xFF
        good = proto.encode(Ch.LOG, 0, 1, b"ola\n")
        frames = p.feed(bytes(bad) + good)
        self.assertEqual(len(frames), 1)
        self.assertEqual(frames[0].payload, b"ola\n")
        self.assertEqual(p.bad, 1)

    def test_stats_layout(self):
        self.assertEqual(struct.calcsize(proto.STATS_FMT), 48)   # sizeof(proto::Stats)
        s = proto.stats_decode(bytes(range(48)))
        self.assertEqual(set(s["ao"]), {"comm", "audio", "display", "storage"})

    def test_evt_decode(self):
        name, d = proto.evt_decode(proto.Evt.SOUND, struct.pack("<ffff", 0.5, 0.1, 0.9, -30.0))
        self.assertEqual(name, "sound")
        self.assertAlmostEqual(d["s"], 0.5)
        self.assertEqual(proto.evt_decode(proto.Evt.BOOT, b"\x01\x09")[1]["reset"], "BROWNOUT")

    def test_anim_layout(self):
        self.assertEqual(proto.anim_build([{"ms": 100, "gx": 10, "lid_r": 255}]), REF_ANIM)

    def test_anim_repeats_previous_fields(self):
        data = proto.anim_build([{"ms": 10, "gx": -40, "lid": 128}, {"ms": 500}], hold=True)
        hdr = struct.unpack_from("<HBBHH", data)
        self.assertEqual(hdr, (proto.ANIM_MAGIC, 1, proto.ANIM_HOLD, 2, 0))
        k1 = struct.unpack_from("<HbbBBBB", data, 16)
        self.assertEqual(k1, (500, -40, 0, 26, 128, 128, 0))

    def test_anim_rejects(self):
        with self.assertRaises(ValueError):
            proto.anim_build([])
        with self.assertRaises(ValueError):
            proto.anim_build([{"ms": 10, "gx": 200}])
        with self.assertRaises(ValueError):
            proto.anim_build([{"ms": 0}], loop=True)
        with self.assertRaises(ValueError):
            proto.anim_build([{"ms": 10, "olho": 1}])
        with self.assertRaises(ValueError):
            proto.anim_build([{"ms": 10}] * 600)   # > 4064 B

    def test_example_json(self):
        data = proto.anim_from_json(os.path.join(HERE, "anims", "exemplo.json"))
        self.assertEqual(len(data), 8 + 8 * 6)
        with open(os.path.join(HERE, "anims", "exemplo.json"), encoding="utf-8") as f:
            self.assertFalse(json.load(f)["loop"])


if __name__ == "__main__":
    unittest.main()
