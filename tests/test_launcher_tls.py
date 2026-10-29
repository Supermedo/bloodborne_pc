"""The launcher's HTTPS (launcher/bbport_tls.py).

1.9.5 beta 2: on players' PCs "Sign in with Discord" and "Test connection" failed with "certificate has
expired" although thehuntersdream.com sends a valid chain. Python loads the Windows stores as they are
and OpenSSL always takes an issuer from them first, so an expired copy there broke a valid chain:

- the stale intermediate: the stores hold the root and an expired copy of an intermediate the server
  sends valid (an old cross-signature);
- the missing root: the stores hold DST Root CA X3 and "ISRG Root X1 signed by DST Root CA X3" (both
  expired) but not ISRG Root X1 itself, which Windows only downloads when its own chain building needs
  it. tests/data/tls holds those public certificates and the chain thehuntersdream.com sends.

Offline, a test PKI the openssl command makes plays both: R1 is the root, X2 an intermediate R1 signed,
I1 the issuing CA, the leaf is localhost's; "old root" and "R1 signed by the old root" (expired) are DST
Root CA X3 and the old ISRG Root X1. Python's default context fails as on the players' PCs; the
launcher's leaves the expired copies out and, where the root itself is missing, asks Windows. Windows'
part runs with a chain engine of the test's own whose only root is R1 (Windows having fetched the
missing root): nothing is added to the PC's stores. Bad certificates are still refused, and a reason a
player can fix is named. The online tests reach thehuntersdream.com (a GET of its start page and the
game server's info, what Test connection reads; never the sign-in) and are skipped without a network."""
from paths import ROOT
import ctypes
import functools
import http.server
import os
import re
import shutil
import socket
import ssl
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import urllib.error
import urllib.request
from datetime import datetime, timezone
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(ROOT / 'launcher'))
sys.path.insert(0, str(ROOT / 'scripts'))
import bbport_tls as T  # noqa: E402

BODY = b'hello from the test server'
DAY = 86400
DATA = ROOT / 'tests/data/tls'


def find_openssl():
    """The openssl command: on the PATH, else Git for Windows' own."""
    found = shutil.which('openssl')
    if found:
        return found
    git = shutil.which('git')
    if git:
        for base in Path(git).resolve().parents[:3]:
            for candidate in (base / 'mingw64/bin/openssl.exe', base / 'usr/bin/openssl.exe'):
                if candidate.is_file():
                    return str(candidate)
    return None


OPENSSL = find_openssl()

CONFIG = '''
[req]
distinguished_name = dn
prompt = no
[dn]
CN = unused

[ca]
default_ca = test_ca
[test_ca]
database = @DIR@/index.txt
new_certs_dir = @DIR@
serial = @DIR@/serial
default_md = sha256
policy = any
unique_subject = no
email_in_dn = no
[any]
organizationName = optional
commonName = supplied

[v3_ca]
basicConstraints = critical,CA:true
keyUsage = critical,keyCertSign,cRLSign
subjectKeyIdentifier = hash
authorityKeyIdentifier = keyid:always
[v3_leaf]
basicConstraints = critical,CA:false
keyUsage = critical,digitalSignature
extendedKeyUsage = serverAuth
subjectAltName = DNS:localhost
subjectKeyIdentifier = hash
authorityKeyIdentifier = keyid:always
'''


def when(seconds):
    return time.strftime('%y%m%d%H%M%SZ', time.gmtime(seconds))


def pem_certificates(path):
    """The DER certificates of a PEM file, in its order."""
    return [ssl.PEM_cert_to_DER_cert(block) for block in
            re.findall(r'-----BEGIN CERTIFICATE-----.+?-----END CERTIFICATE-----', Path(path).read_text(), re.S)]


class Pki:
    """The test certificates, made once for all the tests (EC keys: fast)."""

    def __init__(self, folder):
        self.folder = folder
        (folder / 'index.txt').write_text('')
        (folder / 'serial').write_text('1000\n')
        (folder / 'openssl.cnf').write_text(CONFIG.replace('@DIR@', folder.as_posix()))
        now = time.time()
        valid, expired = (when(now - DAY), when(now + 365 * DAY)), ('200101000000Z', '210101000000Z')
        self.root('r1', '/O=bbport test/CN=Test Root R1')
        self.request('x2', '/O=bbport test/CN=Test Root X2')
        self.sign('x2', 'x2-valid', ('r1', 'r1'), 'v3_ca', valid)
        self.sign('x2', 'x2-expired', ('r1', 'r1'), 'v3_ca', expired)
        self.request('i1', '/O=bbport test/CN=Test Issuing CA')
        self.sign('i1', 'i1', ('x2-valid', 'x2'), 'v3_ca', valid)
        self.request('leaf', '/CN=localhost')
        self.sign('leaf', 'leaf', ('i1', 'i1'), 'v3_leaf', valid)
        self.request('old-leaf', '/CN=localhost')
        self.sign('old-leaf', 'old-leaf', ('i1', 'i1'), 'v3_leaf', expired)
        # DST Root CA X3 and "ISRG Root X1 signed by DST Root CA X3": an old root and R1 signed by it.
        self.request('old-root', '/O=bbport test/CN=Test Old Root')
        self.sign('old-root', 'old-root', None, 'v3_ca', ('150101000000Z', '210101000000Z'))
        self.request('r1-cross', '/O=bbport test/CN=Test Root R1', key='r1')
        self.sign('r1-cross', 'r1-cross', ('old-root', 'old-root'), 'v3_ca', ('190101000000Z', '210101000000Z'))
        self.root('self-signed', '/CN=localhost', 'v3_leaf')
        self.root('av-root', '/O=Test Antivirus/CN=Test Antivirus Web Shield Root')
        self.request('av-leaf', '/CN=localhost')
        self.sign('av-leaf', 'av-leaf', ('av-root', 'av-root'), 'v3_leaf', valid)
        # What Python reads from the stores of the players' PCs.
        self.stale_intermediate = ('r1', 'x2-expired')
        self.missing_root = ('old-root', 'r1-cross')

    def run(self, *args):
        done = subprocess.run([OPENSSL, *args], cwd=self.folder, capture_output=True, text=True,
                              env={**os.environ, 'OPENSSL_CONF': str(self.folder / 'openssl.cnf')})
        if done.returncode:
            raise RuntimeError(f'openssl {args[0]}: {done.stderr}')

    def root(self, name, subject, extensions='v3_ca'):
        self.run('req', '-config', 'openssl.cnf', '-x509', '-new', '-newkey', 'ec', '-pkeyopt',
                 'ec_paramgen_curve:P-256', '-nodes', '-keyout', f'{name}.key', '-out', f'{name}.pem',
                 '-subj', subject, '-days', '3650', '-extensions', extensions)

    def request(self, name, subject, key=None):
        new_key = (['-key', f'{key}.key'] if key else
                   ['-newkey', 'ec', '-pkeyopt', 'ec_paramgen_curve:P-256', '-nodes', '-keyout', f'{name}.key'])
        self.run('req', '-config', 'openssl.cnf', '-new', *new_key, '-out', f'{name}.csr', '-subj', subject)

    def sign(self, request, name, issuer, extensions, dates):
        """`openssl ca` (dates in the past too); issuer None: self-signed."""
        by = ['-selfsign', '-keyfile', f'{request}.key'] if issuer is None else \
            ['-cert', f'{issuer[0]}.pem', '-keyfile', f'{issuer[1]}.key']
        self.run('ca', '-config', 'openssl.cnf', '-batch', '-notext', '-preserveDN', '-in', f'{request}.csr',
                 '-out', f'{name}.pem', *by, '-extensions', extensions, '-startdate', dates[0], '-enddate', dates[1])

    def path(self, name):
        return str(self.folder / f'{name}.pem')

    def der(self, name):
        return ssl.PEM_cert_to_DER_cert((self.folder / f'{name}.pem').read_text())

    def chain(self, *names):
        return [self.der(name) for name in names]

    def bundle(self, file, *names):
        path = self.folder / f'{file}.bundle.pem'
        path.write_text(''.join((self.folder / f'{name}.pem').read_text() for name in names))
        return str(path)

    def pythons_default(self, names):
        """Python's own default context on a PC whose stores hold `names`."""
        return ssl.create_default_context(cafile=self.bundle('-'.join(names), *names))

    def launchers(self, names):
        return T.tls_context(stores=self.chain(*names))


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        self.send_response(200)
        self.send_header('Content-Length', str(len(BODY)))
        self.end_headers()
        self.wfile.write(BODY)

    def log_message(self, *args):
        pass


class Server(http.server.ThreadingHTTPServer):
    daemon_threads = True

    def server_bind(self):  # no name lookup of the address (slow on some PCs)
        http.server.socketserver.TCPServer.server_bind(self)
        self.server_name, self.server_port = 'localhost', self.server_address[1]

    def handle_error(self, request, client_address):
        pass  # refused handshakes are what these tests are about


def serve(test, pki, *names):
    """An HTTPS server sending the certificates `names` (its own first) on localhost's first address
    (so the client's first try connects); returns (URL with localhost, URL with the address)."""
    family, _type, _proto, _name, address = socket.getaddrinfo('localhost', 0, type=socket.SOCK_STREAM)[0]
    server = type('LocalServer', (Server,), {'address_family': family})(address, Handler)
    tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    tls.load_cert_chain(pki.bundle('server-' + '-'.join(names), *names), str(pki.folder / f'{names[0]}.key'))
    server.socket = tls.wrap_socket(server.socket, server_side=True, do_handshake_on_connect=False)
    thread = threading.Thread(target=server.serve_forever, args=(0.05,), daemon=True)
    thread.start()
    test.addCleanup(lambda: (server.shutdown(), server.server_close()))
    port = server.server_address[1]
    literal = f'[{address[0]}]' if family == socket.AF_INET6 else address[0]
    return f'https://localhost:{port}/', f'https://{literal}:{port}/'


def windows_trust(test, on):
    patcher = mock.patch.dict(os.environ, {'BB_WINDOWS_TRUST': '1' if on else '0'})
    patcher.start()
    test.addCleanup(patcher.stop)


class _EngineConfig(ctypes.Structure):  # CERT_CHAIN_ENGINE_CONFIG (Windows 8 and later)
    _fields_ = [('cbSize', ctypes.c_ulong), ('hRestrictedRoot', ctypes.c_void_p),
                ('hRestrictedTrust', ctypes.c_void_p), ('hRestrictedOther', ctypes.c_void_p),
                ('cAdditionalStore', ctypes.c_ulong), ('rghAdditionalStore', ctypes.c_void_p),
                ('dwFlags', ctypes.c_ulong), ('dwUrlRetrievalTimeout', ctypes.c_ulong),
                ('MaximumCachedCertificates', ctypes.c_ulong), ('CycleDetectionModulus', ctypes.c_ulong),
                ('hExclusiveRoot', ctypes.c_void_p), ('hExclusiveTrustedPeople', ctypes.c_void_p),
                ('dwExclusiveFlags', ctypes.c_ulong)]


def engine(test, *roots):
    """A Windows chain engine whose only trusted roots are `roots` (DER), in a store of its own: the
    PC's stores stay untouched."""
    crypt32 = ctypes.WinDLL('crypt32', use_last_error=True)
    crypt32.CertOpenStore.restype = ctypes.c_void_p
    crypt32.CertOpenStore.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_void_p, ctypes.c_ulong,
                                      ctypes.c_void_p]
    crypt32.CertAddEncodedCertificateToStore.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_char_p,
                                                         ctypes.c_ulong, ctypes.c_ulong, ctypes.c_void_p]
    crypt32.CertCreateCertificateChainEngine.argtypes = [ctypes.POINTER(_EngineConfig),
                                                         ctypes.POINTER(ctypes.c_void_p)]
    crypt32.CertFreeCertificateChainEngine.argtypes = [ctypes.c_void_p]
    crypt32.CertCloseStore.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
    store = crypt32.CertOpenStore(2, 0, None, 0, None)  # in memory
    test.addCleanup(crypt32.CertCloseStore, store, 0)
    for root in roots:
        test.assertTrue(crypt32.CertAddEncodedCertificateToStore(store, 0x10001, root, len(root), 4, None))
    handle = ctypes.c_void_p()
    config = _EngineConfig(cbSize=ctypes.sizeof(_EngineConfig), hExclusiveRoot=store)
    test.assertTrue(crypt32.CertCreateCertificateChainEngine(ctypes.byref(config), ctypes.byref(handle)),
                    ctypes.get_last_error())
    test.addCleanup(crypt32.CertFreeCertificateChainEngine, handle)
    return handle.value


@unittest.skipUnless(OPENSSL, 'needs the openssl command (Git for Windows brings one)')
class PkiTestCase(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory()
        cls.pki = Pki(Path(cls.temporary.name))

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()


class StoreTests(PkiTestCase):
    """OpenSSL's part: expired copies in the stores no longer stand in for the server's chain."""

    def setUp(self):
        windows_trust(self, False)  # Windows' answer: WindowsTrustTests

    def test_pythons_default_context_fails_on_the_stale_intermediate(self):
        url, _address = serve(self, self.pki, 'leaf', 'i1', 'x2-valid')
        with self.assertRaises(urllib.error.URLError) as caught:
            urllib.request.urlopen(url, timeout=10, context=self.pki.pythons_default(self.pki.stale_intermediate))
        self.assertIn('certificate has expired', str(caught.exception))

    def test_the_launcher_follows_the_servers_chain_past_it(self):
        url, _address = serve(self, self.pki, 'leaf', 'i1', 'x2-valid')
        with T.urlopen(url, timeout=10, context=self.pki.launchers(self.pki.stale_intermediate)) as reply:
            self.assertEqual(reply.read(), BODY)

    def test_pythons_default_context_fails_on_the_missing_root(self):
        url, _address = serve(self, self.pki, 'leaf', 'i1', 'x2-valid')
        with self.assertRaises(urllib.error.URLError) as caught:
            urllib.request.urlopen(url, timeout=10, context=self.pki.pythons_default(self.pki.missing_root))
        self.assertIn('certificate has expired', str(caught.exception))

    def test_without_windows_the_missing_root_is_missing(self):
        url, _address = serve(self, self.pki, 'leaf', 'i1', 'x2-valid')
        with self.assertRaises(urllib.error.URLError) as caught:
            T.urlopen(url, timeout=10, context=self.pki.launchers(self.pki.missing_root))
        self.assertIn('unable to get local issuer certificate', str(caught.exception))

    def test_store_certificates_outside_their_dates_are_left_out(self):
        names = ('r1', 'x2-expired', 'old-root', 'r1-cross', 'x2-valid')
        loaded = self.pki.launchers(names).get_ca_certs()
        self.assertEqual(sorted(dict(pair for rdn in cert['subject'] for pair in rdn)['commonName'] for cert in loaded),
                         ['Test Root R1', 'Test Root X2'])
        self.assertEqual(len(self.pki.pythons_default(names).get_ca_certs()), len(names))

    def test_the_launchers_context_checks_like_pythons(self):
        tls = T.tls_context()
        self.assertEqual(tls.verify_mode, ssl.CERT_REQUIRED)
        self.assertTrue(tls.check_hostname)
        self.assertGreaterEqual(tls.minimum_version, ssl.create_default_context().minimum_version)
        if sys.platform == 'win32':  # the Windows stores are read
            self.assertTrue(tls.get_ca_certs())

    def test_a_self_signed_server_is_refused(self):
        url, _address = serve(self, self.pki, 'self-signed')
        with self.assertRaises(urllib.error.URLError) as caught:
            T.urlopen(url, timeout=10, context=self.pki.launchers(self.pki.stale_intermediate))
        self.assertIn('CERTIFICATE_VERIFY_FAILED', str(caught.exception))

    def test_a_chain_only_through_the_expired_copy_is_refused(self):
        url, _address = serve(self, self.pki, 'leaf', 'i1', 'x2-expired')
        with self.assertRaises(urllib.error.URLError) as caught:
            T.urlopen(url, timeout=10, context=self.pki.launchers(('r1',)))
        self.assertIn('certificate has expired', str(caught.exception))

    def test_another_host_name_is_refused(self):
        _url, address = serve(self, self.pki, 'leaf', 'i1', 'x2-valid')
        with self.assertRaises(urllib.error.URLError) as caught:
            T.urlopen(address, timeout=10, context=self.pki.launchers(self.pki.stale_intermediate))
        self.assertIn('CERTIFICATE_VERIFY_FAILED', str(caught.exception))

    def test_plain_http_still_works(self):
        server = Server(('127.0.0.1', 0), Handler)
        threading.Thread(target=server.serve_forever, args=(0.05,), daemon=True).start()
        self.addCleanup(lambda: (server.shutdown(), server.server_close()))
        with T.urlopen(f'http://127.0.0.1:{server.server_address[1]}/', timeout=10) as reply:
            self.assertEqual(reply.read(), BODY)


class ReasonTests(PkiTestCase):
    """What a player reads when a certificate is still refused."""

    def setUp(self):
        windows_trust(self, False)
        english = mock.patch.object(T, 'L', None)  # the texts in English, whatever language other tests left
        english.start()
        self.addCleanup(english.stop)

    def test_a_clock_outside_the_certificate_names_the_pcs_date(self):
        url, _address = serve(self, self.pki, 'old-leaf', 'i1', 'x2-valid')
        with self.assertRaises(T.CertificateRefused) as caught:
            T.urlopen(url, timeout=10, context=self.pki.launchers(self.pki.stale_intermediate))
        self.assertIn('date or time', str(caught.exception))
        self.assertIn(datetime.now().strftime('%Y-%m-%d'), str(caught.exception))
        self.assertIsInstance(caught.exception, urllib.error.URLError)  # the callers catch it as before
        self.assertIsInstance(caught.exception.reason, ssl.SSLCertVerificationError)

    def test_a_certificate_from_someone_else_names_them(self):
        url, _address = serve(self, self.pki, 'av-leaf', 'av-root')
        with mock.patch.dict(T.EXPECTED_ISSUERS, {'localhost': "Let's Encrypt"}):
            with self.assertRaises(T.CertificateRefused) as caught:
                T.urlopen(url, timeout=10, context=self.pki.launchers(self.pki.stale_intermediate))
        self.assertIn('from Test Antivirus Web Shield Root,', str(caught.exception))
        self.assertIn('localhost', str(caught.exception))

    def test_the_servers_own_authority_is_no_reason(self):
        chain = self.pki.chain('leaf', 'i1', 'x2-valid')
        with mock.patch.dict(T.EXPECTED_ISSUERS, {'localhost': 'bbport test'}):
            self.assertIsNone(T.explain(chain, 'localhost'))
        self.assertIsNone(T.explain(chain, 'localhost'))  # no authority known for it
        self.assertIsNone(T.explain([], 'localhost'))
        self.assertIsNone(T.explain([b'not a certificate'], 'localhost'))

    def test_the_hunters_dream_is_lets_encrypt(self):
        chain = self.pki.chain('av-leaf')
        self.assertIn('Test Antivirus', T.explain(chain, 'thehuntersdream.com'))
        self.assertIn('Test Antivirus', T.explain(chain, 'www.thehuntersdream.com'))
        self.assertIsNone(T.explain(chain, 'notthehuntersdream.com'))
        served = pem_certificates(DATA / 'thehuntersdream-chain.pem')
        self.assertIsNone(T.explain(served, 'thehuntersdream.com', now=datetime(2026, 11, 1, tzinfo=timezone.utc)))
        self.assertIn('date or time', T.explain(served, 'thehuntersdream.com',
                                                now=datetime(2025, 11, 1, tzinfo=timezone.utc)))

    def test_certificate_facts_match_pythons(self):
        files = [self.pki.path(name) for name in ('x2-expired', 'leaf', 'av-root', 'old-root')]
        files += [str(path) for path in sorted(DATA.glob('*.pem')) if path.name != 'thehuntersdream-chain.pem']
        for file in files:
            with self.subTest(file=Path(file).name):
                facts = T.certificate_facts(pem_certificates(file)[0])
                decoded = ssl._ssl._test_decode_cert(file)
                self.assertEqual(facts.not_before.timestamp(), ssl.cert_time_to_seconds(decoded['notBefore']))
                self.assertEqual(facts.not_after.timestamp(), ssl.cert_time_to_seconds(decoded['notAfter']))
                issuer = dict(pair for rdn in decoded['issuer'] for pair in rdn)
                self.assertIn(issuer['commonName'], facts.issuer)
        self.assertEqual(T.certificate_facts(self.pki.der('x2-expired')).not_after,
                         datetime(2021, 1, 1, tzinfo=timezone.utc))
        served = [T.certificate_facts(der).issuer for der in pem_certificates(DATA / 'thehuntersdream-chain.pem')]
        self.assertEqual(served[0], "Let's Encrypt (YE2)")


@unittest.skipUnless(sys.platform == 'win32', "Windows' own certificate check")
class WindowsTrustTests(PkiTestCase):
    def test_the_servers_chain_is_trusted(self):
        chain = self.pki.chain('leaf', 'i1', 'x2-valid')
        self.assertTrue(T.windows_trusts(chain, 'localhost', engine(self, self.pki.der('r1'))))

    def test_windows_takes_the_valid_path_past_expired_copies(self):
        chain = self.pki.chain('leaf', 'i1', 'x2-expired', 'x2-valid', 'r1-cross')
        roots = engine(self, self.pki.der('r1'), self.pki.der('old-root'))
        self.assertTrue(T.windows_trusts(chain, 'localhost', roots))

    def test_bad_certificates_are_refused(self):
        cases = {'expired path': (('leaf', 'i1', 'x2-expired'), 'localhost'),
                 'expired server certificate': (('old-leaf', 'i1', 'x2-valid'), 'localhost'),
                 'only the old root': (('leaf', 'i1', 'x2-valid', 'r1-cross'), 'localhost', 'old-root'),
                 'another host': (('leaf', 'i1', 'x2-valid'), 'example.com'),
                 'self-signed': (('self-signed',), 'localhost'),
                 'unknown root': (('av-leaf', 'av-root'), 'localhost'),
                 'no certificate': ((), 'localhost')}
        for case, (names, host, *root) in cases.items():
            with self.subTest(case=case):
                roots = engine(self, self.pki.der(root[0] if root else 'r1'))
                self.assertFalse(T.windows_trusts(self.pki.chain(*names), host, roots))
        self.assertFalse(T.windows_trusts([b'not a certificate'], 'localhost', engine(self, self.pki.der('r1'))))

    def test_the_pcs_own_roots_do_not_know_the_test_root(self):
        self.assertFalse(T.windows_trusts(self.pki.chain('leaf', 'i1', 'x2-valid'), 'localhost'))

    def test_the_missing_root_connects_through_windows(self):
        url, address = serve(self, self.pki, 'leaf', 'i1', 'x2-valid')
        players = self.pki.launchers(self.pki.missing_root)
        windows = functools.partial(T.windows_trusts, engine=engine(self, self.pki.der('r1')))
        with mock.patch.object(T, 'windows_trusts', windows):
            windows_trust(self, True)
            with T.urlopen(url, timeout=10, context=players) as reply:
                self.assertEqual(reply.read(), BODY)
            with self.assertRaises(urllib.error.URLError):  # Windows checks the host name too
                T.urlopen(address, timeout=10, context=players)
            windows_trust(self, False)  # BB_WINDOWS_TRUST=0: OpenSSL's answer only
            with self.assertRaises(urllib.error.URLError):
                T.urlopen(url, timeout=10, context=players)

    def test_a_server_windows_does_not_trust_stays_refused(self):
        url, _address = serve(self, self.pki, 'self-signed')
        windows_trust(self, True)
        with self.assertRaises(urllib.error.URLError) as caught:
            T.urlopen(url, timeout=10, context=self.pki.launchers(self.pki.stale_intermediate))
        self.assertIn('CERTIFICATE_VERIFY_FAILED', str(caught.exception))


class LetsEncryptStoreTests(unittest.TestCase):
    """The real certificates: the old Let's Encrypt roots of the players' stores are left out."""

    def test_the_old_roots_are_left_out(self):
        old = [pem_certificates(DATA / name)[0] for name in
               ('dst-root-ca-x3.pem', 'isrg-root-x1-by-dst-root-ca-x3.pem', 'isrg-root-x2-by-isrg-root-x1-2020.pem')]
        x1 = pem_certificates(DATA / 'isrg-root-x1.pem')[0]
        self.assertEqual(T.tls_context(stores=old).get_ca_certs(), [])
        loaded = T.tls_context(stores=old + [x1]).get_ca_certs()
        self.assertEqual([dict(pair for rdn in cert['subject'] for pair in rdn)['commonName'] for cert in loaded],
                         ['ISRG Root X1'])


def reachable(host, port=443):
    try:
        socket.create_connection((host, port), timeout=3).close()
        return True
    except OSError:
        return False


@unittest.skipUnless(reachable('thehuntersdream.com'), 'thehuntersdream.com is not reachable')
class TheHuntersDreamTests(unittest.TestCase):
    """The players' stores (DST Root CA X3 and ISRG Root X1 signed by it, not ISRG Root X1) against the
    real server. Only its start page and the game server's info, never the sign-in."""

    URL = 'https://thehuntersdream.com/'

    def setUp(self):
        self.players = [pem_certificates(DATA / name)[0] for name in
                        ('dst-root-ca-x3.pem', 'isrg-root-x1-by-dst-root-ca-x3.pem')]

    def test_pythons_default_context_fails_like_on_the_players_pcs(self):
        context = ssl.create_default_context(cadata=b''.join(self.players))
        with self.assertRaises(urllib.error.URLError) as caught:
            urllib.request.urlopen(self.URL, timeout=10, context=context)
        self.assertIn('certificate has expired', str(caught.exception))

    @unittest.skipUnless(sys.platform == 'win32', "Windows' own certificate check")
    def test_the_launcher_connects_through_windows(self):
        windows_trust(self, True)
        with T.urlopen(self.URL, timeout=15, context=T.tls_context(stores=self.players)) as reply:
            self.assertEqual(reply.status, 200)
        windows_trust(self, False)
        with self.assertRaises(urllib.error.URLError) as caught:
            T.urlopen(self.URL, timeout=15, context=T.tls_context(stores=self.players))
        self.assertIn('unable to get local issuer certificate', str(caught.exception))

    @unittest.skipUnless(sys.platform == 'win32', "Windows' own certificate check")
    def test_test_connection_on_the_online_page(self):
        import bbport_launcher_win as L
        windows_trust(self, True)
        with mock.patch.object(T, 'store_certificates', lambda: self.players), mock.patch.object(T, '_opener', None), \
                mock.patch.object(L, 'LANG', 'en'):
            text = L.server_check_hunters_dream()
        self.assertNotIn('not reachable', text)
        self.assertEqual(text.count('reachable'), 2)


class LauncherTests(unittest.TestCase):
    def test_every_https_request_of_the_launcher_goes_through_it(self):
        for path in (ROOT / 'launcher').glob('*.py'):
            with self.subTest(file=path.name):
                self.assertNotIn('urllib.request.urlopen(', path.read_text(encoding='utf-8'))
        self.assertIn('bbport_tls.urlopen(', (ROOT / 'launcher/bbport_launcher_win.py').read_text(encoding='utf-8'))
        self.assertIn('bbport_tls.urlopen(', (ROOT / 'scripts/fetch_fsr4vk.py').read_text(encoding='utf-8'))

    def test_the_switch(self):
        with mock.patch.dict(os.environ, {'BB_WINDOWS_TRUST': '0'}):
            self.assertFalse(T.windows_trust_enabled())
        with mock.patch.dict(os.environ, {'BB_WINDOWS_TRUST': '1'}):
            self.assertEqual(T.windows_trust_enabled(), sys.platform == 'win32')


if __name__ == '__main__':
    unittest.main()
