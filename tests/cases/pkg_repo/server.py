#!/usr/bin/env python3
"""This is the HTTP server of the pkg_repo case. It serves the files under
DIR and answers two prefixes badly, for the error paths of the client.
   server.py PORT DIR [CERT KEY]
With CERT and KEY, the server speaks TLS 1.3 only, for the pkg_https
case. /short/PATH announces 100 bytes more than the file PATH contains and closes
the connection after the file; /stall/PATH sends the headers of PATH and
then nothing for 30 seconds."""
import functools
import http.server
import ssl
import sys
import time


class Handler(http.server.SimpleHTTPRequestHandler):
    def do_GET(self):
        parts = self.path.split('/', 2)
        if len(parts) == 3 and parts[1] in ('short', 'stall'):
            try:
                with open(self.translate_path('/' + parts[2]), 'rb') as f:
                    data = f.read()
            except OSError:
                self.send_error(404)
                return
            extra = 100 if parts[1] == 'short' else 0
            self.send_response(200)
            self.send_header('Content-Length', str(len(data) + extra))
            self.end_headers()
            if parts[1] == 'short':
                self.wfile.write(data)
            else:
                self.wfile.flush()
                time.sleep(30)
            self.close_connection = True
            return
        super().do_GET()


port, directory = int(sys.argv[1]), sys.argv[2]
server = http.server.ThreadingHTTPServer(('127.0.0.1', port), functools.partial(Handler, directory=directory))
if len(sys.argv) == 5:
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.minimum_version = ssl.TLSVersion.TLSv1_3
    ctx.load_cert_chain(sys.argv[3], sys.argv[4])
    server.socket = ctx.wrap_socket(server.socket, server_side=True)
server.serve_forever()
