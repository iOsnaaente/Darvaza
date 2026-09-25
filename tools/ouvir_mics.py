#!/usr/bin/env python3
"""
Ouve os dois INMP441 do eyes_test no fone de ouvido, em estéreo (mic L -> ouvido
esquerdo, mic R -> ouvido direito), e mostra o log do ESP32 no terminal.

Uso:
    pip install pyserial numpy sounddevice
    python ouvir_mics.py                     # porta detectada sozinha
    python ouvir_mics.py -p COM5 --gain 12   # Windows, +12 dB
    python ouvir_mics.py --save teste.wav    # também grava em WAV
    python ouvir_mics.py --modo esquerdo     # só um mic nos dois ouvidos

A linha de status mostra o nível de cada mic e o atraso entre eles (GCC-PHAT,
igual ao firmware): palma à direita deve dar atraso positivo e coer alta.

Frames do firmware: ver tools/proto.py (canal AUDIO = PCM, LOG = texto, EVT = eventos).
"""

import argparse
import collections
import sys
import threading
import time
import wave

import numpy as np
import serial
import serial.tools.list_ports

import proto

try:
    import sounddevice as sd
except (ImportError, OSError) as e:   # OSError: PortAudio ausente
    sd = None
    SD_ERROR = e

SAMPLE_RATE = 16000
MIC_DIST_M = 0.15        # para converter atraso em ângulo no status


# ---------------------------------------------------------------------------
# Serial -> blocos de áudio + linhas de texto (log, eventos e lixo fora de frame)
# ---------------------------------------------------------------------------
class Parser:
    def __init__(self, on_audio, on_text):
        self.frames = proto.Parser()
        self.on_audio = on_audio
        self.on_text = on_text
        self.text = bytearray()
        self.last_seq = None
        self.lost = 0

    @property
    def bad(self):
        return self.frames.bad

    def feed(self, data):
        for f in self.frames.feed(data):
            if f.ch == proto.Ch.AUDIO:
                samples = np.frombuffer(f.payload, dtype="<i2").reshape(-1, 2)
                if self.last_seq is not None:
                    gap = (f.seq - self.last_seq - 1) & 0xFFFF
                    if 0 < gap < 100:
                        self.lost += gap
                        # silêncio no lugar do que se perdeu mantém o tempo certo
                        self.on_audio(np.zeros((gap * len(samples), 2), dtype=np.int16))
                self.last_seq = f.seq
                self.on_audio(samples)
            elif f.ch == proto.Ch.LOG:
                self._text(f.payload)
            elif f.ch == proto.Ch.EVT:
                name, d = proto.evt_decode(f.op, f.payload)
                if name != "sound":   # som já aparece na linha de status
                    self.on_text(f"[{name}] {d}")
        # fora de frame: boot a 115200 (lixo) ou panic (texto a 2 Mbaud)
        self._text(self.frames.take_junk())

    def _text(self, chunk):
        for b in chunk:
            if b == 0x0A:
                line = self.text.decode("utf-8", "ignore").rstrip("\r")
                if line.strip():
                    self.on_text(line)
                self.text.clear()
            elif b >= 0x20 or b in (0x09, 0x1B):
                self.text.append(b)
            if len(self.text) > 400:   # lixo sem \n (ex.: boot a 115200)
                self.text.clear()


# ---------------------------------------------------------------------------
# Buffer de reprodução com absorção de jitter e de diferença de clock
# ---------------------------------------------------------------------------
class Jitter:
    def __init__(self, target_ms, max_ms):
        self.q = collections.deque()
        self.count = 0
        self.lock = threading.Lock()
        self.target = SAMPLE_RATE * target_ms // 1000
        self.max = SAMPLE_RATE * max_ms // 1000
        self.primed = False
        self.underruns = 0

    def push(self, block):
        with self.lock:
            self.q.append(block)
            self.count += len(block)
            # clock do ESP um pouco mais rápido que o da placa de som: descarta o excesso
            while self.count > self.max and len(self.q) > 1:
                self.count -= len(self.q.popleft())

    def pull(self, n):
        out = np.zeros((n, 2), dtype=np.float32)
        with self.lock:
            if not self.primed:
                if self.count < self.target:
                    return out
                self.primed = True
            got = 0
            while got < n and self.q:
                blk = self.q[0]
                take = min(n - got, len(blk))
                out[got:got + take] = blk[:take]
                got += take
                if take == len(blk):
                    self.q.popleft()
                else:
                    self.q[0] = blk[take:]
                self.count -= take
            if got < n:
                self.underruns += 1
                self.primed = False   # espera encher de novo em vez de picotar
        return out


# ---------------------------------------------------------------------------
# Análise para a linha de status (mesma ideia do firmware)
# ---------------------------------------------------------------------------
def gcc_phat(l, r, max_lag):
    n = 2 * len(l)
    w = np.hanning(len(l))
    L = np.fft.rfft(l * w, n)
    R = np.fft.rfft(r * w, n)
    f = np.fft.rfftfreq(n, 1 / SAMPLE_RATE)
    X = L * np.conj(R)
    X /= np.abs(X) + 1e-20
    band = (f >= 300) & (f <= 4000)
    X[~band] = 0
    cc = np.fft.irfft(X, n)
    lags = np.arange(-max_lag, max_lag + 1)
    vals = cc[lags % n]
    k = int(np.argmax(vals))
    coh = vals[k] * n / (2 * band.sum())   # irfft já divide por n; 1 = fases alinhadas
    tau = float(lags[k])
    if 0 < k < len(vals) - 1:   # interpolação parabólica
        y0, y1, y2 = vals[k - 1], vals[k], vals[k + 1]
        den = y0 - 2 * y1 + y2
        if den < 0:
            tau += 0.5 * (y0 - y2) / den
    return tau, coh


def dbfs(x):
    return 20 * np.log10(np.sqrt(np.mean(x ** 2)) + 1e-9)


def find_port():
    ports = list(serial.tools.list_ports.comports())
    for p in ports:   # CH343/CH340 (1a86), CP210x (10c4), USB nativo ESP (303a)
        if p.vid in (0x1A86, 0x10C4, 0x303A):
            return p.device
    if ports:
        return ports[0].device
    sys.exit("nenhuma porta serial encontrada; use -p")


def main():
    ap = argparse.ArgumentParser(description="Ouve os mics do eyes_test pela serial")
    ap.add_argument("-p", "--port", help="porta serial (padrão: detecta)")
    ap.add_argument("-b", "--baud", type=int, default=2000000, help="igual a STREAM_BAUD no firmware")
    ap.add_argument("--gain", type=float, default=0.0, help="ganho extra em dB (padrão 0)")
    ap.add_argument("--modo", choices=["estereo", "esquerdo", "direito", "mono"], default="estereo",
                    help="estereo: L->ouvido esq., R->ouvido dir.")
    ap.add_argument("--save", metavar="ARQ.wav", help="grava o estéreo cru em WAV")
    ap.add_argument("--no-play", action="store_true", help="não toca (só log/status/gravação)")
    ap.add_argument("--latencia", type=int, default=120, help="buffer de reprodução em ms")
    ap.add_argument("--device", help="dispositivo de saída do sounddevice (nome ou número)")
    ap.add_argument("--listar", action="store_true", help="lista dispositivos de áudio e sai")
    args = ap.parse_args()

    play = not args.no_play
    if (play or args.listar) and sd is None:
        sys.exit(f"sounddevice indisponível ({SD_ERROR}).\n"
                 "  pip install sounddevice   (no Linux/WSL também: sudo apt install libportaudio2)\n"
                 "  ou rode com --no-play --save teste.wav")
    if args.listar:
        print(sd.query_devices())
        return
    if play and args.device is None and sd.default.device[1] < 0:
        try:
            sd.query_devices(kind="output")
        except sd.PortAudioError:
            msg = "nenhuma saída de áudio encontrada pelo PortAudio."
            wsl = sys.platform == "linux" and "microsoft" in open("/proc/version").read().lower()
            if wsl:
                msg += ("\n  WSL: o som sai pelo PulseAudio do WSLg; falta o plugin do ALSA:"
                        "\n    sudo apt install libasound2-plugins"
                        "\n    printf 'pcm.!default { type pulse }\\nctl.!default { type pulse }\\n' > ~/.asoundrc"
                        "\n  ou rode este script no Windows (usbipd detach a porta antes).")
            sys.exit(msg + "\n  Para só gravar: --no-play --save teste.wav")

    port = args.port or find_port()
    ser = serial.Serial(port, args.baud, timeout=0.05)
    print(f"[{port} @ {args.baud}]  Ctrl+C para sair")

    gain = 10 ** (args.gain / 20) / 32768.0
    jit = Jitter(args.latencia, args.latencia * 3)
    wav = None
    if args.save:
        wav = wave.open(args.save, "wb")
        wav.setnchannels(2)
        wav.setsampwidth(2)
        wav.setframerate(SAMPLE_RATE)

    # Janela para o status (0.5 s)
    win = collections.deque(maxlen=SAMPLE_RATE // 2)
    dc = np.zeros(2)
    status = {"blocks": 0}
    status_lock = threading.Lock()
    max_lag = int(MIC_DIST_M / 343.0 * SAMPLE_RATE) + 2

    def on_audio(block):
        nonlocal dc
        if wav:
            wav.writeframes(block.astype("<i2").tobytes())
        x = block.astype(np.float32)
        # tira o offset DC do INMP441 (média lenta)
        dc = 0.995 * dc + 0.005 * x.mean(axis=0)
        x -= dc
        with status_lock:
            win.extend(x)
            status["blocks"] += 1
        if play:
            y = x * gain
            if args.modo == "esquerdo":
                y = np.repeat(y[:, :1], 2, axis=1)
            elif args.modo == "direito":
                y = np.repeat(y[:, 1:], 2, axis=1)
            elif args.modo == "mono":
                y = np.repeat(y.mean(axis=1, keepdims=True), 2, axis=1)
            jit.push(np.clip(y, -1.0, 1.0))

    def on_text(line):
        sys.stdout.write("\r\033[K" + line + "\n")

    parser = Parser(on_audio, on_text)

    stream = None
    if play:
        def cb(outdata, frames, _time, _status):
            outdata[:] = jit.pull(frames)
        stream = sd.OutputStream(samplerate=SAMPLE_RATE, channels=2, dtype="float32",
                                 blocksize=256, callback=cb, device=args.device)
        stream.start()

    t_status = time.time()
    try:
        while True:
            data = ser.read(max(1, ser.in_waiting))
            if data:
                parser.feed(data)
            now = time.time()
            if now - t_status >= 0.5:
                with status_lock:
                    x = np.array(win) if len(win) >= 2048 else None
                    blocks = status["blocks"]
                    status["blocks"] = 0
                if x is not None:
                    l, r = x[:, 0], x[:, 1]
                    # atraso só no pedaço mais alto (onde há som de verdade)
                    seg = 1024
                    e = np.convolve(l ** 2 + r ** 2, np.ones(seg), "valid")[::256]
                    s0 = int(np.argmax(e)) * 256
                    tau, coh = gcc_phat(l[s0:s0 + seg], r[s0:s0 + seg], max_lag)
                    tau_max = MIC_DIST_M / 343.0 * SAMPLE_RATE
                    ang = np.degrees(np.arcsin(np.clip(tau / tau_max, -1, 1)))
                    kbps = blocks * 1034 / (now - t_status) / 1000
                    sys.stdout.write(
                        f"\r\033[KL {dbfs(l / 32768):6.1f}  R {dbfs(r / 32768):6.1f} dBFS  "
                        f"atraso {tau:+5.2f} ({ang:+4.0f}°)  coer {coh:.2f}  "
                        f"| {kbps:4.0f} kB/s  perdidos {parser.lost}  ruins {parser.bad}  "
                        f"underruns {jit.underruns}")
                    sys.stdout.flush()
                t_status = now
    except KeyboardInterrupt:
        pass
    finally:
        print()
        if stream:
            stream.stop()
            stream.close()
        if wav:
            wav.close()
            print(f"gravado: {args.save}")
        ser.close()


if __name__ == "__main__":
    main()
