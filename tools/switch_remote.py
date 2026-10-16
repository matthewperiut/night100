#!/usr/bin/env python3
"""Small sys-autopilot client; credentials are read privately, never logged."""
import hashlib
import http.client
import json
import os
from pathlib import Path, PurePosixPath
import socket
import urllib.error
import urllib.parse
import urllib.request
from package_resources import safe_path


class Client:
    def __init__(self, host=None, port=None):
        self.host = host or os.environ.get('SWITCH_HOST', 'switch.local')
        self.port = int(port or os.environ.get('SWITCH_PORT', '4150'))
        token_file = Path(os.environ.get('SWITCH_TOKEN_FILE', '~/.config/switch-dev/token')).expanduser()
        self._token = os.environ.get('SWITCH_TOKEN') or token_file.read_text().strip()
        if not self._token:
            raise ValueError('Switch bearer token is missing')

    def request(self, method, endpoint, data=None, timeout=30):
        if isinstance(data, dict):
            data = json.dumps(data).encode()
        headers = {'Authorization': f'Bearer {self._token}'}
        if data is not None:
            headers['Content-Type'] = 'application/json'
        request = urllib.request.Request(f'http://{self.host}:{self.port}{endpoint}', data=data,
                                         headers=headers, method=method)
        with urllib.request.urlopen(request, timeout=timeout) as response:
            return response.read()

    def json(self, endpoint):
        return json.loads(self.request('GET', endpoint))

    def status(self):
        return self.json('/status')

    def read(self, path):
        return self.request('GET', '/files?' + urllib.parse.urlencode({'path': path}))

    def hash(self, path):
        return self.json('/files/hash?' + urllib.parse.urlencode({'path': path}))

    def tap(self, *buttons):
        return self.request('POST', '/input/tap', {'buttons': list(buttons)})

    def screenshot(self, output):
        Path(output).write_bytes(self.request('GET', '/screenshot'))

    def upload(self, source, destination, timeout=600):
        """Stream files with fixed Content-Length; no multi-GB Python buffer."""
        source = Path(source)
        connection = http.client.HTTPConnection(self.host, self.port, timeout=timeout)
        connection.blocksize=1024*1024
        try:
            with source.open('rb') as stream:
                connection.request('PUT', '/files?' + urllib.parse.urlencode({'path': destination}), body=stream,
                    headers={'Authorization': f'Bearer {self._token}', 'Content-Length': str(source.stat().st_size),
                             'Content-Type': 'application/octet-stream'})
                response = connection.getresponse()
                body = response.read()
                if response.status not in (200, 201):
                    raise RuntimeError(f'Switch upload failed ({response.status}) for {destination}: {body[:200]!r}')
                return json.loads(body)
        finally:
            connection.close()

    def ip(self):
        return socket.gethostbyname(self.host)


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def remote_path(root, relative):
    if not isinstance(root,str) or not root.startswith('/'):
        raise ValueError('Unsafe Switch deployment path')
    root_parts=safe_path(root[1:])
    if len(root_parts.parts)<2 or root_parts.parts[0]!='switch':
        raise ValueError('Use an application directory beneath /switch')
    return str(PurePosixPath('/')/root_parts/safe_path(relative))
