# SPDX-License-Identifier: GPL-2.0-or-later
"""Online play for the Linux launcher (bbport_launcher.py): SuperMedo Server, its account and parties.

What the Windows launcher's Online page does (bbport_launcher_win.py, run.py's online_overrides), with the
Linux launcher's own settings (~/.config/bbport-launcher/settings.json, never the Windows launcher's).
SuperMedo Seamless co-op is not offered: BBSC is a Windows program (bbsc-launch.exe starts the Windows
bb-probe.exe and injects bbsc.dll into it with CreateRemoteThread; the DLL itself is built on windows.h,
WinHTTP and Winsock), so it cannot start the Linux game. No GTK here (tests/test_linux_online.py).
"""
import json
import socket
import ssl
import struct
import time
import urllib.error
import urllib.request
from pathlib import Path

# The port's version servers see (shadNet's client version, User-Agent): bbport_launcher_win.VERSION.
VERSION = '1.9.8.5'
# SuperMedo Server: supermedo.com's shadNet server and its game server (messages, bloodstains, sign-in,
# parties); the game's own server requests go there.
COMMUNITY_SERVER = 'https://game.supermedo.com'
SUPERMEDO_SERVER, SUPERMEDO_WEBAPI = 'np.supermedo.com:31313', 'http://np.supermedo.com:31315'
GAME_SERVER = 'https://ss4.scej-network.jp:20443'
COMMUNITY_API_PORT = 18671  # the server's ss.info sends the game to this port for its API
PLAY_LOG_HOSTS = ('bb-playlog-prod.s3.amazonaws.com', 'bb-playlog-test.s3.amazonaws.com')
# Seamless co-op's own user folder on Windows (run.py COOP_SAVES): characters BBSC played, with its goods in
# place of the game's bells ("?GoodsName?" without BBSC).
COOP_SAVES = 'seamless-coop'

# The launcher's settings for online play. Online is off until the player turns it on and signs in.
DEFAULTS = {
    'online': False,
    'online_upnp': True,  # the router opens the game's P2P port by itself (Steam Deck on Wi-Fi too)
    'supermedo_npid': '',
    'supermedo_pass': '',
    'party_key': '',  # SuperMedo Server's party key for this account (party_call 'signin')
}

SHADNET_ERRORS = {3: 'The name or the email address is not valid.', 4: 'Too soon: try again in a minute.',
                  11: 'This name is taken.', 12: 'This email provider is not allowed.',
                  13: 'An account with this email address exists already.', 23: 'Sign-up is closed.'}


def account(s):
    """The SuperMedo account the game signs in with, '' without one."""
    return str(s.get('supermedo_npid', '')).strip() if s.get('supermedo_pass') else ''


def coop_saves(path):
    """Whether a saves folder is Seamless co-op's own (a Windows port's user/seamless-coop)."""
    return Path(path).name == COOP_SAVES


def bbsc_overrides(path):
    """user/host_overrides.custom.json as BBSC's scripts write it: the summon messenger on BBSC's WebAPI port.
    A user folder brought from Windows can hold one; it would send the game to BBSC's server."""
    try:
        entries = json.loads(Path(path).read_text(encoding='utf-8-sig'))
    except (OSError, ValueError):
        return False
    return isinstance(entries, dict) and any('summon_messenger' in key and str(value).rstrip('/').endswith(':31315')
                                             for key, value in entries.items())


def host_overrides(user_dir, community=COMMUNITY_SERVER, log=print):
    """Writes <user>/host_overrides.json for this launch and returns the file the game reads. A player who
    needs other addresses puts them in host_overrides.custom.json, which is then used as it is."""
    user_dir = Path(user_dir)
    custom = user_dir / 'host_overrides.custom.json'
    if custom.exists():
        if not bbsc_overrides(custom):
            return custom
        log("Co-op: user/host_overrides.custom.json is BBSC's (Seamless co-op, Windows), not used")
    community = community.rstrip('/')
    scheme, host = community.split('://', 1)
    host = host.split('/', 1)[0].split(':', 1)[0]
    # The game's own server functions go over HTTP to the game server; play logs to its API port.
    overrides = {GAME_SERVER: community}
    overrides.update({f'https://{bucket}:443': f'{scheme}://{host}:{COMMUNITY_API_PORT}' for bucket in PLAY_LOG_HOSTS})
    user_dir.mkdir(parents=True, exist_ok=True)
    path = user_dir / 'host_overrides.json'
    path.write_text(json.dumps(overrides, indent=2) + '\n', encoding='utf-8')
    log(f'Co-op: game server {community}')
    return path


def environment(s, user_dir, log=print):
    """The game's variables for online play ({} offline), and host_overrides.json written for it."""
    if not s.get('online'):
        return {}
    env = {'BB_ONLINE': '1', 'BB_SHADNET_SERVER': SUPERMEDO_SERVER, 'BB_SHADNET_WEBAPI': SUPERMEDO_WEBAPI,
           'BB_SHADNET_NPID': str(s.get('supermedo_npid', '')).strip(),
           'BB_SHADNET_PASSWORD': str(s.get('supermedo_pass', '')),
           'BB_UPNP': '1' if s.get('online_upnp', True) else '0', 'BB_COMMUNITY_SERVER': COMMUNITY_SERVER,
           'BB_CLIENT_VERSION': f'bbport/{VERSION}'}
    env['SHADPS4_HTTP_HOST_OVERRIDES_JSON'] = str(host_overrides(user_dir, COMMUNITY_SERVER, log))
    return env


def urlopen(request, timeout):
    """HTTPS with the system's certificate authorities (the AppImage sets SSL_CERT_FILE to its own)."""
    return urllib.request.urlopen(request, timeout=timeout, context=ssl.create_default_context())


def _varint(n):
    out = b''
    while True:
        byte, n = n & 0x7F, n >> 7
        out += bytes([byte | (0x80 if n else 0)])
        if not n:
            return out


def _text(field, value):
    data = value.encode('utf-8')
    return _varint(field << 3 | 2) + _varint(len(data)) + data if data else b''


def _shadnet_error(address, command, proto, timeout):
    """One shadNet command (framed as shadNet's documentation describes): the error byte of its reply."""
    def read(sock, n):
        data = b''
        while len(data) < n:
            chunk = sock.recv(n - len(data))
            if not chunk:
                raise OSError('the server closed the connection')
            data += chunk
        return data
    host, _, port = address.rpartition(':')
    payload = struct.pack('<I', len(proto)) + proto
    with socket.create_connection((host, int(port)), timeout=timeout) as sock:
        info = read(sock, 15)
        read(sock, struct.unpack_from('<I', info, 3)[0] - 15)  # ServerInfo: the protocol version
        sock.sendall(struct.pack('<BHIQ', 0, command, 15 + len(payload), 1) + payload)
        return read(sock, 16)[15]


def shadnet_register(address, npid, password, email, timeout=15):
    """Creates an account on a shadNet server (its Create command: the RegistrationRequest protobuf).
    Returns None, or why it failed."""
    error = _shadnet_error(address, 2, _text(1, npid) + _text(2, password) + _text(4, email), timeout)
    return None if error == 0 else SHADNET_ERRORS.get(error, f'The server said no (error {error}).')


def shadnet_check_login(address, npid, password, timeout=15):
    """Whether a shadNet server takes this Online ID and password: its Login command (the LoginRequest protobuf,
    as the game sends it, with appear_offline so nobody sees the check), then the connection is closed. Returns
    None, or why it was refused. "Already logged in" means the password is right (the game itself, elsewhere)."""
    proto = _text(1, npid) + _text(2, password) + _varint(6 << 3) + _varint(1)
    error = _shadnet_error(address, 0, proto, timeout)
    if error in (0, 6):
        return None
    return {7: 'No account has this Online ID.', 8: 'The password is wrong.'}.get(
        error, SHADNET_ERRORS.get(error, f'The server said no (error {error}).'))


def discord_sign_in(on_page, server=COMMUNITY_SERVER, timeout=600, opener=None, sleep=time.sleep):
    """Sign in with Discord on SuperMedo Server (its /auth pages): shows the sign-in page through on_page(url),
    waits for it and returns (Online ID, password), or raises ValueError (why)."""
    opener = opener or urlopen

    def call(path, data=None):
        request = urllib.request.Request(server + path, data=data, method='POST' if data is not None else 'GET')
        with opener(request, 15) as response:
            return json.loads(response.read())
    started = call('/auth/start', b'')
    on_page(started['url'])
    deadline = time.time() + timeout
    while time.time() < deadline:
        sleep(2)
        reply = call('/auth/poll?session=' + started['session'])
        if reply.get('status') == 'done':
            return reply['npid'], reply['password']
        if reply.get('status') == 'failed':
            raise ValueError(reply.get('message') or 'the sign-in failed')
        if reply.get('status') == 'expired':
            break
    raise ValueError('the sign-in took too long; start it again')


def party_call(call, body=None, key='', opener=None):
    """A call to SuperMedo Server's parties (/party/v1/<call>): (HTTP status, its JSON answer). The key comes
    from 'signin' with the SuperMedo account and stays in the settings (party_key)."""
    opener = opener or urlopen
    request = urllib.request.Request(COMMUNITY_SERVER + '/party/v1/' + call, data=json.dumps(body or {}).encode(),
                                     method='POST', headers={'Content-Type': 'application/json',
                                                             'Authorization': 'Bearer ' + key,
                                                             'User-Agent': f'bbport-launcher/{VERSION} (Linux)'})
    try:
        with opener(request, 20) as response:
            return response.status, json.loads(response.read())
    except urllib.error.HTTPError as error:
        try:
            return error.code, json.loads(error.read())
        except ValueError:
            return error.code, {'error': str(error)}


def party_act(call, body, name, password, key, opener=None):
    """One party call with the account: the stored key, or a new one from 'signin' when there is none or the
    server no longer knows it. Returns (answer, the key to keep); raises ValueError with the server's reason."""
    status, answer = party_call(call, body, key, opener) if key else (401, {})
    if status == 401:
        status, signed = party_call('signin', {'name': name, 'password': password}, '', opener)
        if status != 200:
            raise ValueError(signed.get('error', status))
        key = signed['key']
        status, answer = party_call(call, body, key, opener)
    if status != 200:
        raise ValueError(answer.get('error', status))
    return answer, key


# The launcher's switches patches.py leaves out of every online start (its OFFLINE_ONLY): cheats and debug features,
# fair to the other players. Third-party patches too (patches.py, "stay off").
OFFLINE_ONLY_EFFECTS = ('debug_camera', 'debug_menu')


def server_check(timeout=6):
    """A line about each of SuperMedo Server's addresses: reachable, or why not (runs off the GTK thread)."""
    lines = []
    try:
        with urlopen(COMMUNITY_SERVER + ':20443/bb-eu/ss.info', timeout) as reply:
            reply.read(64)
        lines.append(f'Game server {COMMUNITY_SERVER}: reachable')
    except Exception as error:  # a broken reply is an answer too
        lines.append(f'Game server {COMMUNITY_SERVER}: not reachable ({error})')
    host, _sep, port = SUPERMEDO_SERVER.rpartition(':')
    try:
        socket.create_connection((host, int(port)), timeout=4).close()
        lines.append(f'Server {SUPERMEDO_SERVER}: reachable')
    except Exception as error:
        lines.append(f'Server {SUPERMEDO_SERVER}: not reachable ({error})')
    try:
        with urllib.request.urlopen(SUPERMEDO_WEBAPI + '/status', timeout=4) as reply:
            ok = b'"ok":true' in reply.read(512).replace(b' ', b'')
        lines.append(f'WebAPI {SUPERMEDO_WEBAPI}: ' + ('reachable' if ok else 'answered, but it is not a shadNet server'))
    except Exception as error:
        lines.append(f'WebAPI {SUPERMEDO_WEBAPI}: not reachable ({error})')
    return '\n'.join(lines)
