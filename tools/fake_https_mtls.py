#!/usr/bin/env python3
"""模拟"要求客户端证书"的 LocalSend https 对端（mTLS）。

复现官方 LocalSend 服务端行为：TLS 握手时要求客户端提供证书，
否则发送 tlsv13 alert certificate required。
用法: python3 fake_https_mtls.py [port] [client_ca_pem]
  client_ca_pem: 用于校验客户端证书的信任文件（默认与本目录 client.crt 相同）
"""
import http.server
import ssl
import sys
import json
import os

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 54443
CA_FILE = sys.argv[2] if len(sys.argv) > 2 else os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "client.crt")
CERT_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "client.crt")
KEY_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "client.key")


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        print("[peer]", fmt % args, flush=True)

    def _json(self, code, obj):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path.endswith("/info"):
            self._json(200, {
                "alias": "Fake-mTLS-Peer", "version": "2.0", "protocol": "https",
                "deviceModel": "pytest", "deviceType": "desktop",
                "fingerprint": "mtlsfakepeer0001", "download": True})
        else:
            self._json(200, {"ok": True})

    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0))
        raw = self.rfile.read(n)
        print("[peer] POST %s (%d bytes)" % (self.path, n), flush=True)
        if self.path.endswith("/prepare-upload"):
            try:
                req = json.loads(raw)
                files = req.get("files", {})
                self._json(200, {
                    "sessionId": "mtls-session-01",
                    "files": {k: "tok-" + k for k in files}})
            except Exception as e:
                self._json(400, {"error": str(e)})
        elif self.path.endswith("/upload"):
            self._json(200, {"ok": True})
        else:
            self._json(200, {"ok": True})


if __name__ == "__main__":
    httpd = http.server.HTTPServer(("0.0.0.0", PORT), Handler)
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(CERT_FILE, KEY_FILE)
    ctx.verify_mode = ssl.CERT_REQUIRED          # ← 关键：强制要求客户端证书
    ctx.load_verify_locations(CA_FILE)           # 信任我们内置的自签证书
    httpd.socket = ctx.wrap_socket(httpd.socket, server_side=True)
    print("[peer] mTLS https server on 0.0.0.0:%d (CERT_REQUIRED, CA=%s)"
          % (PORT, CA_FILE), flush=True)
    httpd.serve_forever()
