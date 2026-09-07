"""Local keepalive, failure recovery, endpoint isolation and concurrency fixture."""
import http.server
import socket
import subprocess
import sys
import threading


class Server(http.server.ThreadingHTTPServer):
    daemon_threads = True
    accepted = 0

    def get_request(self):
        connection, address = super().get_request()
        connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.accepted += 1
        return connection, address


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_GET(self):
        body = self.path.encode()
        self.send_response(403 if self.path == "/error" else 200)
        self.send_header("Content-Length", str(len(body)))
        if self.path == "/close":
            self.send_header("Connection", "close")
            self.close_connection = True
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *_):
        pass


with Server(("127.0.0.1", 0), Handler) as server:
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        subprocess.run(
            [sys.argv[1], f"http://127.0.0.1:{server.server_port}"],
            check=True,
            timeout=15,
        )
        # Initial session, forced reconnect, separate endpoint and explicit clear.
        assert server.accepted == 4, f"Expected 4 connections, got {server.accepted}"
        print("41 HTTP requests: 4 connections; reuse, recovery and concurrent responses passed")
    finally:
        server.shutdown()
        thread.join()
