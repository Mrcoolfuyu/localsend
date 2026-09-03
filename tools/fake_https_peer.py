#!/usr/bin/env python3
"""模拟官方 LocalSend 的 https 模式对端（自签名证书）。

用于验证 localsend-qt 客户端能否：
  1. 接受自签名证书完成 TLS 握手（不报 "SSL 握手失败"）
  2. 走完 prepare-upload -> upload 全流程

用法：
    python3 fake_https_peer.py [端口] [--http]
默认端口 54443；加 --http 则启动明文服务（用于对比测试）。
"""
import http.server
import json
import ssl
import sys
import os
import tempfile

PORT = int(sys.argv[1]) if len(sys.argv) > 1 and sys.argv[1].isdigit() else 54443
USE_HTTPS = "--http" not in sys.argv

CERT_DIR = os.path.join(tempfile.gettempdir(), "ls-fake-peer")
CERT = os.path.join(CERT_DIR, "cert.pem")
KEY = os.path.join(CERT_DIR, "key.pem")


def ensure_cert():
    os.makedirs(CERT_DIR, exist_ok=True)
    if os.path.exists(CERT) and os.path.exists(KEY):
        return
    os.system(
        "openssl req -x509 -newkey rsa:2048 -nodes -days 1 -subj '/CN=LocalSend Fake' "
        f"-keyout {KEY} -out {CERT} >/dev/null 2>&1"
    )


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        print("[py-https-peer] " + (fmt % args), flush=True)

    def _read_body(self):
        cl = int(self.headers.get("Content-Length", 0) or 0)
        return self.rfile.read(cl) if cl else b""

    def _reply(self, data, ctype="application/json"):
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(data)

    def do_POST(self):
        body = self._read_body()
        print(f"[py-https-peer] POST {self.path} ({len(body)} bytes)", flush=True)
        if "prepare-upload" in self.path:
            try:
                req = json.loads(body)
                files = req.get("files", {})
            except Exception:
                files = {}
            resp = {"sessionId": "sess123", "files": {k: "tok-%s" % k for k in files}}
            self._reply(json.dumps(resp).encode())
            print("[py-https-peer] -> prepare-upload OK, tokens=%d" % len(files), flush=True)
        elif "upload" in self.path:
            # body 即文件原始字节
            print("[py-https-peer] -> upload 收到 %d 字节" % len(body), flush=True)
            if len(sys.argv) > 2 and sys.argv[2].startswith("--save="):
                out = sys.argv[2].split("=", 1)[1]
                with open(out, "wb") as fh:
                    fh.write(body)
                print("[py-https-peer] 已保存 -> %s" % out, flush=True)
            self._reply(b"OK", "text/plain")
        else:
            self._reply(b"{}")

    def do_GET(self):
        self._reply(b"{}")


def main():
    srv = http.server.HTTPServer(("127.0.0.1", PORT), Handler)
    if USE_HTTPS:
        ensure_cert()
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(CERT, KEY)
        srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
        print(f"[py-https-peer] HTTPS 自签名服务已启动：127.0.0.1:{PORT}", flush=True)
    else:
        print(f"[py-https-peer] 明文 HTTP 服务已启动：127.0.0.1:{PORT}", flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
