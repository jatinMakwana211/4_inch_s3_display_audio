"""Probe configured session-init and TLS WebSocket upgrade; never prints session tokens."""
import base64
import hashlib
import json
import os
from pathlib import Path
import re
import socket
import ssl
import urllib.parse
import urllib.request

config = (Path(__file__).resolve().parents[1] / 'main/include/agent_config.h').read_text()


def setting(name):
    value = re.search(r'^#define\s+' + name + r'\s+(".*")$', config, re.M)
    return json.loads(value[1])


payload = {
    'userData': {key: setting('AGENT_USER_' + key.upper()) for key in ('name', 'email', 'mobile')},
    'websiteName': setting('AGENT_WEBSITE_NAME'),
    'language': setting('AGENT_LANGUAGE'),
    'fileName': setting('AGENT_CONTEXT_FILE'),
}
request = urllib.request.Request(setting('AGENT_SESSION_INIT_URL'),
                                 data=json.dumps(payload).encode(),
                                 headers={'Content-Type': 'application/json'})
opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
with opener.open(request, timeout=20) as response:
    print('Session init HTTP:', response.status)
    session = json.load(response)
url = urllib.parse.urlsplit(session['websocketUrl'])
assert url.scheme == 'wss' and url.hostname, 'Expected a secure websocketUrl'
port = url.port or 443
path = url.path or '/'
if url.query:
    path += '?' + url.query
key = base64.b64encode(os.urandom(16)).decode()
host = url.hostname + (f':{port}' if port != 443 else '')
headers = (f'GET {path} HTTP/1.1\r\nHost: {host}\r\nUpgrade: websocket\r\n'
           f'Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\n'
           f'Sec-WebSocket-Version: 13\r\nOrigin: {setting("AGENT_WEBSOCKET_ORIGIN")}\r\n\r\n')
with socket.create_connection((url.hostname, port), timeout=20) as tcp:
    with ssl.create_default_context().wrap_socket(tcp, server_hostname=url.hostname) as ws:
        ws.sendall(headers.encode())
        reply = bytearray()
        while b'\r\n\r\n' not in reply:
            block = ws.recv(4096)
            if not block:
                raise RuntimeError('Socket closed before upgrade')
            reply.extend(block)
            assert len(reply) <= 65536, 'Upgrade headers too large'
        header = reply.split(b'\r\n\r\n', 1)[0].decode()
        lines = header.split('\r\n')
        print('WebSocket upgrade:', lines[0])
        assert lines[0].split()[1] == '101', 'WebSocket upgrade rejected'
        fields = {name.lower(): value.strip() for line in lines[1:] if ':' in line
                  for name, value in [line.split(':', 1)]}
        expected = base64.b64encode(hashlib.sha1((key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').encode()).digest()).decode()
        assert fields['sec-websocket-accept'] == expected, 'Invalid upgrade signature'
        # Masked close frame with normal closure status 1000.
        mask = os.urandom(4)
        code = b'\x03\xe8'
        ws.sendall(b'\x88\x82' + mask + bytes(value ^ mask[i] for i, value in enumerate(code)))
print('TLS certificate and WebSocket handshake verified; device audio remains to be tested.')
