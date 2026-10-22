#!/usr/bin/env python3
"""Recebe os quadros da Luckfox pela Ethernet (W5500, eth1) e serve no Wi-Fi.

A placa (eth_stream) conecta em TCP na porta 5000 e manda, por quadro,
"FRM1" u32 seq u32 ms u32 tamanho (big-endian) + o JPEG. Blocos "RAW1"
(eth_stream --bench) são lidos e descartados: só contam para a taxa.

HTTP (porta 8080, todas as interfaces):
  /            página com a imagem ao vivo
  /stream      MJPEG (multipart/x-mixed-replace) - abre em navegador e VLC
  /frame.jpg   o último quadro
  /status      JSON: quadros/s, Mbit/s, idade do último quadro
"""
import argparse
import json
import socket
import struct
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HDR = struct.Struct(">4sIII")

PAGE = b"""<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Camera RV1106</title>
<style>body{margin:0;background:#111;color:#ddd;font:14px sans-serif;text-align:center}
img{max-width:100%;height:auto}#s{padding:6px}</style></head><body>
<div id="s">...</div><img src="/stream">
<script>setInterval(async()=>{try{const r=await(await fetch('/status')).json();
document.getElementById('s').textContent=(r.connected?'placa conectada':'placa desconectada')+
' | '+r.fps.toFixed(1)+' quadros/s | '+r.mbps.toFixed(2)+' Mbit/s | '+r.kb+' KB/quadro'}catch(e){}},1000)</script>
</body></html>"""


class Latest:
    """O último quadro recebido; quem serve espera pelo próximo."""

    def __init__(self):
        self.cv = threading.Condition()
        self.jpg = None
        self.seq = 0
        self.rx_t = 0.0
        self.connected = False
        self.fps = 0.0
        self.mbps = 0.0

    def put(self, jpg):
        with self.cv:
            self.jpg = jpg
            self.seq += 1
            self.rx_t = time.monotonic()
            self.cv.notify_all()

    def wait_after(self, seq, timeout=2.0):
        with self.cv:
            self.cv.wait_for(lambda: self.seq != seq, timeout)
            return self.seq, self.jpg


latest = Latest()


def recv_exact(conn, n):
    buf = bytearray()
    while len(buf) < n:
        chunk = conn.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("fechada")
        buf += chunk
    return bytes(buf)


def board_loop(bind, port):
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((bind, port))
    srv.listen(1)
    print(f"esperando a placa em {bind}:{port}", flush=True)
    while True:
        conn, addr = srv.accept()
        print(f"placa conectada: {addr[0]}", flush=True)
        latest.connected = True
        t0, frames, nbytes = time.monotonic(), 0, 0
        start, total, kind = t0, 0, "?"
        try:
            while True:
                magic, _seq, _ms, size = HDR.unpack(recv_exact(conn, HDR.size))
                if magic not in (b"FRM1", b"RAW1") or size > 8 << 20:
                    raise ConnectionError(f"cabeçalho inválido {magic!r} {size}")
                kind = magic.decode()
                data = recv_exact(conn, size)
                if magic == b"FRM1":
                    latest.put(data)
                frames += 1
                nbytes += HDR.size + size
                total += HDR.size + size
                dt = time.monotonic() - t0
                if dt >= 2.0:
                    latest.fps, latest.mbps = frames / dt, nbytes * 8 / dt / 1e6
                    t0, frames, nbytes = time.monotonic(), 0, 0
        except (ConnectionError, OSError) as e:
            dt = time.monotonic() - start
            print(f"placa desconectada: {e}", flush=True)
            if dt > 0:
                print(f"MEDIA {kind}: {total * 8 / dt / 1e6:.2f} Mbit/s recebidos ({total / 1e6:.1f} MB em {dt:.1f} s)",
                      flush=True)
        finally:
            latest.connected = False
            latest.fps = latest.mbps = 0.0
            conn.close()


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        pass

    def send_body(self, ctype, body, code=200):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path = self.path.split("?", 1)[0]
        if path == "/":
            self.send_body("text/html; charset=utf-8", PAGE)
        elif path == "/frame.jpg":
            if latest.jpg is None:
                self.send_body("text/plain", b"sem quadros ainda", 503)
            else:
                self.send_body("image/jpeg", latest.jpg)
        elif path == "/status":
            age = time.monotonic() - latest.rx_t if latest.rx_t else None
            body = json.dumps({
                "connected": latest.connected,
                "fps": latest.fps,
                "mbps": latest.mbps,
                "kb": len(latest.jpg) // 1024 if latest.jpg else 0,
                "frames": latest.seq,
                "last_frame_age_s": age,
            }).encode()
            self.send_body("application/json", body)
        elif path == "/stream":
            self.stream()
        else:
            self.send_body("text/plain", b"nao encontrado", 404)

    def stream(self):
        self.send_response(200)
        self.send_header("Content-Type", "multipart/x-mixed-replace; boundary=frame")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Connection", "close")
        self.end_headers()
        self.close_connection = True
        seq = 0
        try:
            while True:
                new_seq, jpg = latest.wait_after(seq)
                if new_seq == seq or jpg is None:
                    continue
                seq = new_seq
                self.wfile.write(b"--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %d\r\n\r\n" % len(jpg))
                self.wfile.write(jpg)
                self.wfile.write(b"\r\n")
        except (BrokenPipeError, ConnectionResetError):
            pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--board-bind", default="192.168.77.1", help="IP do eth1 (W5500)")
    ap.add_argument("--board-port", type=int, default=5000)
    ap.add_argument("--http-port", type=int, default=8080)
    a = ap.parse_args()
    threading.Thread(target=board_loop, args=(a.board_bind, a.board_port), daemon=True).start()
    print(f"HTTP em 0.0.0.0:{a.http_port}", flush=True)
    ThreadingHTTPServer(("0.0.0.0", a.http_port), Handler).serve_forever()


if __name__ == "__main__":
    main()
