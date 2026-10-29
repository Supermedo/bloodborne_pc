# SPDX-License-Identifier: GPL-2.0-or-later
"""How the launcher opens HTTPS: The Hunter's Dream (sign-in, Test connection, players online),
GitHub (updates, Other versions) and the downloads all go through urlopen() here.

1.9.5 beta 2: "Sign in with Discord" and "Test connection" failed on players' PCs with "certificate
verify failed: certificate has expired" although thehuntersdream.com sends a valid chain (up to ISRG
Root X1) and other PCs connected. Python's default context on Windows loads the certificates of the
Windows ROOT and CA stores as they are, and OpenSSL always takes an issuer from those before the one the
server sends (X509_V_FLAG_TRUSTED_FIRST, which cannot be turned off since OpenSSL 1.1.0). Windows adds
roots lazily, when its own chain building needs one, and Python only reads the stores: a PC that never
had ISRG Root X1 fetched but still holds the old "ISRG Root X1 signed by DST Root CA X3" (expired
September 2024), or an old cross-signed copy of an intermediate, builds the chain through the expired
copy. So:

1. Only current certificates from the stores: the ones outside their dates are left out. An expired
   copy can no longer stand in for the certificate the server sends; with nothing better in the stores
   OpenSSL fails with "unable to get local issuer certificate" instead, and 2. decides.
2. When OpenSSL refuses, Windows decides, the same rule as the game's own connections
   (gpu/bbnet/include/windows_trust.h): the certificates the server sent go through Windows' chain
   building and SSL policy for the host name (CertGetCertificateChain, which also downloads a root
   Windows keeps online only, ISRG Root X1 for example, and prefers a valid path to an expired one).
   When Windows trusts them, the connection goes on - the very connection whose certificate Windows
   checked, so nothing else can take its place; otherwise it fails as before. BB_WINDOWS_TRUST=0 turns
   this off. The Windows stores are only read, never changed.

A certificate refused by both says why in words a player can act on (CertificateRefused): the PC's
clock outside the certificate's dates, or a certificate from someone other than the server's authority
(an antivirus' HTTPS scanning, a proxy). Other failures keep Python's message.

Logic only, no Tk (tests/test_launcher_tls.py). The launcher binds itself (bind()) for its texts.
"""
import _ssl
import ctypes
import http.client
import os
import socket
import ssl
import sys
import threading
import urllib.error
import urllib.request
from collections import namedtuple
from datetime import datetime, timezone

L = None  # bbport_launcher_win, set by bind()

# Hosts whose certificates come from one authority: a certificate from another one means something on
# the PC answers in the server's place.
EXPECTED_ISSUERS = {'thehuntersdream.com': "Let's Encrypt"}


def bind(module):
    global L
    L = module


def _(*texts):
    """In the launcher's language; English without the launcher (a script run on its own)."""
    return L._(*texts) if L else texts[0]


class CertificateRefused(urllib.error.URLError):
    """A server certificate neither OpenSSL nor Windows trusts. str() is the reason a player can fix;
    `reason` holds the SSL error, as in urllib's own errors."""

    def __init__(self, text, error):
        super().__init__(error)
        self.text = text

    def __str__(self):
        return self.text


def store_certificates():
    """The DER certificates of the Windows ROOT and CA stores trusted for server authentication: what
    Python's own default context loads (ssl.SSLContext.load_default_certs)."""
    found = []
    for store in ('CA', 'ROOT'):
        try:
            entries = ssl.enum_certificates(store)
        except (AttributeError, OSError):  # not Windows, or a store that cannot be read
            continue
        found += [der for der, encoding, trust in entries
                  if encoding == 'x509_asn' and (trust is True or ssl.Purpose.SERVER_AUTH.oid in trust)]
    return found


def tls_context(stores=None, now=None):
    """Python's default context (host name and certificate checked), trusting the certificates of
    `stores` (DER; the Windows stores by default) that are within their dates at `now`."""
    tls = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    now = now or datetime.now(timezone.utc)
    for der in store_certificates() if stores is None else stores:
        facts = certificate_facts(der)
        if facts is None or facts.not_before <= now <= facts.not_after:  # what does not parse: OpenSSL decides
            try:
                tls.load_verify_locations(cadata=der)
            except ssl.SSLError:
                pass  # a broken store entry, as Python's own loading skips it
    if stores is None:
        tls.set_default_verify_paths()  # SSL_CERT_FILE and SSL_CERT_DIR, as in Python's default
    tls.set_alpn_protocols(['http/1.1'])
    return tls


def _unverified_context():
    """Only to read the certificates a server sends after OpenSSL refused them; the connection is used
    only when Windows trusts exactly those."""
    tls = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    tls.check_hostname = False
    tls.verify_mode = ssl.CERT_NONE
    tls.set_alpn_protocols(['http/1.1'])
    return tls


def windows_trust_enabled():
    return sys.platform == 'win32' and not os.environ.get('BB_WINDOWS_TRUST', '').startswith('0')


def peer_chain(sock):
    """The DER certificates a server sent, its own first (the leaf alone when Python cannot say more)."""
    chain = []
    if hasattr(sock, 'get_unverified_chain'):  # Python 3.13
        chain = sock.get_unverified_chain() or []
    elif hasattr(getattr(sock, '_sslobj', None), 'get_unverified_chain'):  # 3.10 to 3.12
        chain = sock._sslobj.get_unverified_chain() or []
    chain = [cert if isinstance(cert, bytes) else cert.public_bytes(_ssl.ENCODING_DER) for cert in chain]
    if not chain:
        leaf = sock.getpeercert(binary_form=True)
        chain = [leaf] if leaf else []
    return chain


class _Connection(http.client.HTTPSConnection):
    def connect(self):
        try:
            super().connect()
            return
        except ssl.SSLCertVerificationError as error:
            refused = error
        # Only the socket: close() would also forget the request http.client is sending.
        self._drop_socket()
        host = self._tunnel_host or self.host
        try:
            http.client.HTTPConnection.connect(self)  # again, through the proxy when there is one
            sock = _UNVERIFIED.wrap_socket(self.sock, server_hostname=host)
        except OSError:
            self._drop_socket()
            raise refused from None
        chain = peer_chain(sock)
        if chain and windows_trust_enabled() and windows_trusts(chain, host):
            self.sock = sock
            return
        sock.close()
        self.sock = None
        text = explain(chain, host)
        if text:
            raise CertificateRefused(text, refused) from None
        raise refused

    def _drop_socket(self):
        sock, self.sock = self.sock, None
        if sock:
            sock.close()


class _Handler(urllib.request.HTTPSHandler):
    def __init__(self, tls):
        super().__init__(context=tls)
        self.tls = tls

    def https_open(self, req):
        return self.do_open(_Connection, req, context=self.tls)


_UNVERIFIED = _unverified_context()
_lock = threading.Lock()
_opener = None


def opener(context=None):
    """urllib's usual opener (proxies, redirects, errors) with the launcher's HTTPS."""
    global _opener
    if context is not None:
        return urllib.request.build_opener(_Handler(context))
    with _lock:  # the stores are read once, not for every request
        if _opener is None:
            _opener = urllib.request.build_opener(_Handler(tls_context()))
        return _opener


def urlopen(url, data=None, timeout=socket._GLOBAL_DEFAULT_TIMEOUT, context=None):
    """urllib.request.urlopen for the launcher. A certificate refused for a reason the player can fix
    raises CertificateRefused (a URLError) with that reason as its text."""
    try:
        return opener(context).open(url, data, timeout)
    except urllib.error.URLError as error:
        if isinstance(error.reason, CertificateRefused):
            raise error.reason from None
        raise


# ---- the reason, for a player ---------------------------------------------------------------------

Facts = namedtuple('Facts', 'not_before not_after issuer')


def _element(der, at):
    """(tag, start of the content, end) of the DER element at `at`."""
    tag, size = der[at], der[at + 1]
    at += 2
    if size & 0x80:
        count = size & 0x7f
        size = int.from_bytes(der[at:at + count], 'big')
        at += count
    if at + size > len(der):
        raise ValueError('truncated')
    return tag, at, at + size


def _elements(der, start, end):
    while start < end:
        tag, body, stop = _element(der, start)
        yield tag, body, stop
        start = stop


def _time(der, tag, body, stop):
    text = der[body:stop].decode('ascii')
    if tag == 0x17:  # UTCTime: two digits for the year
        text = ('19' if int(text[:2]) >= 50 else '20') + text
    return datetime.strptime(text, '%Y%m%d%H%M%SZ').replace(tzinfo=timezone.utc)


def _issuer(der, start, end):
    """The issuer's organisation and common name as one text: Let's Encrypt (YE2)."""
    names = {}
    for _set, set_start, set_end in _elements(der, start, end):
        for _seq, pair_start, pair_end in _elements(der, set_start, set_end):
            (_oid, oid_start, oid_end), (kind, value_start, value_end) = _elements(der, pair_start, pair_end)
            value = der[value_start:value_end]
            names[der[oid_start:oid_end]] = (value.decode('utf-16-be') if kind == 0x1e else
                                             value.decode('utf-8', 'replace'))
    organisation, common = names.get(b'\x55\x04\x0a', ''), names.get(b'\x55\x04\x03', '')
    if organisation and common and organisation not in common:
        return f'{organisation} ({common})'
    return common or organisation


def certificate_facts(der):
    """Facts(not before, not after (UTC), issuer) of a DER certificate; None when it does not parse."""
    try:
        _tag, start, end = _element(der, 0)
        _tag, start, end = _element(der, start)  # the signed part
        fields = list(_elements(der, start, end))
        if fields[0][0] == 0xa0:  # the version
            fields = fields[1:]
        _serial, _algorithm, issuer, validity = fields[:4]
        not_before, not_after = (_time(der, *element) for element in _elements(der, validity[1], validity[2]))
        return Facts(not_before, not_after, _issuer(der, issuer[1], issuer[2]))
    except (IndexError, ValueError):
        return None


def explain(chain, host, now=None):
    """Why a refused certificate is refused, in words a player can act on; None when it is none of
    the known reasons."""
    facts = certificate_facts(chain[0]) if chain else None
    if facts is None:
        return None
    now = now or datetime.now(timezone.utc)
    if not facts.not_before <= now <= facts.not_after:
        return _("This PC's date or time looks wrong ({0}): the server's certificate is not valid then. "
                 "Correct it in the Windows settings and try again.",
                 'Дата или время на этом ПК, похоже, неверны ({0}): в это время сертификат сервера '
                 'недействителен. Исправьте их в настройках Windows и попробуйте снова.'
                 ).format(now.astimezone().strftime('%Y-%m-%d %H:%M'))
    expected = next((issuer for name, issuer in EXPECTED_ISSUERS.items()
                     if host == name or host.endswith('.' + name)), None)
    if expected and expected.lower() not in facts.issuer.lower():
        return _("The certificate for {0} is from {1}, not from the server: an antivirus' HTTPS scanning "
                 "or a proxy is in between. Exclude {0} there and try again.",
                 'Сертификат для {0} выдан {1}, а не сервером: между ними проверка HTTPS антивируса или '
                 'прокси. Добавьте {0} в его исключения и попробуйте снова.').format(host, facts.issuer)
    return None


# ---- Windows' answer ------------------------------------------------------------------------------

_ENCODING = 0x00010001  # X509_ASN_ENCODING | PKCS_7_ASN_ENCODING
_STORE_PROV_MEMORY = 2
_STORE_ADD_ALWAYS = 4
_REVOCATION_CHECK_END_CERT = 0x10000000
_REVOCATION_ACCUMULATIVE_TIMEOUT = 0x08000000
# A revocation list Windows could not reach is no reason to refuse (browsers and Schannel do the same).
_REVOCATION_UNKNOWN = 0x00000040 | 0x01000000  # CERT_TRUST_REVOCATION_STATUS_UNKNOWN, _IS_OFFLINE_REVOCATION
_POLICY_IGNORE_ALL_REV_UNKNOWN = 0x00000f00
_POLICY_SSL = 4
_AUTHTYPE_SERVER = 2
_SERVER_AUTH = b'1.3.6.1.5.5.7.3.1'


class _TrustStatus(ctypes.Structure):
    _fields_ = [('dwErrorStatus', ctypes.c_ulong), ('dwInfoStatus', ctypes.c_ulong)]


class _ChainContext(ctypes.Structure):  # CERT_CHAIN_CONTEXT, as far as it is read
    _fields_ = [('cbSize', ctypes.c_ulong), ('TrustStatus', _TrustStatus)]


class _EnhKeyUsage(ctypes.Structure):
    _fields_ = [('cUsageIdentifier', ctypes.c_ulong), ('rgpszUsageIdentifier', ctypes.POINTER(ctypes.c_char_p))]


class _UsageMatch(ctypes.Structure):
    _fields_ = [('dwType', ctypes.c_ulong), ('Usage', _EnhKeyUsage)]


class _ChainPara(ctypes.Structure):
    _fields_ = [('cbSize', ctypes.c_ulong), ('RequestedUsage', _UsageMatch)]


class _SslPolicyPara(ctypes.Structure):  # SSL_EXTRA_CERT_CHAIN_POLICY_PARA
    _fields_ = [('cbSize', ctypes.c_ulong), ('dwAuthType', ctypes.c_ulong), ('fdwChecks', ctypes.c_ulong),
                ('pwszServerName', ctypes.c_wchar_p)]


class _PolicyPara(ctypes.Structure):
    _fields_ = [('cbSize', ctypes.c_ulong), ('dwFlags', ctypes.c_ulong), ('pvExtraPolicyPara', ctypes.c_void_p)]


class _PolicyStatus(ctypes.Structure):
    _fields_ = [('cbSize', ctypes.c_ulong), ('dwError', ctypes.c_ulong), ('lChainIndex', ctypes.c_long),
                ('lElementIndex', ctypes.c_long), ('pvExtraPolicyStatus', ctypes.c_void_p)]


_crypt32_lib = None


def _crypt32():
    global _crypt32_lib
    if _crypt32_lib is None:
        lib = ctypes.WinDLL('crypt32', use_last_error=True)  # its own handle: argtypes stay ours
        p, u, chain = ctypes.c_void_p, ctypes.c_ulong, ctypes.POINTER(_ChainContext)
        for name, restype, argtypes in (
                ('CertOpenStore', p, [p, u, p, u, p]),
                ('CertCloseStore', ctypes.c_int, [p, u]),
                ('CertAddEncodedCertificateToStore', ctypes.c_int,
                 [p, u, ctypes.c_char_p, u, u, ctypes.POINTER(p)]),
                ('CertFreeCertificateContext', ctypes.c_int, [p]),
                ('CertGetCertificateChain', ctypes.c_int,
                 [p, p, p, p, ctypes.POINTER(_ChainPara), u, p, ctypes.POINTER(chain)]),
                ('CertFreeCertificateChain', None, [chain]),
                ('CertVerifyCertificateChainPolicy', ctypes.c_int,
                 [p, chain, ctypes.POINTER(_PolicyPara), ctypes.POINTER(_PolicyStatus)])):
            function = getattr(lib, name)
            function.restype, function.argtypes = restype, argtypes
        _crypt32_lib = lib
    return _crypt32_lib


def windows_trusts(chain, host, engine=None):
    """Whether Windows trusts chain[0] as `host`'s server certificate, with the server's other
    certificates as the intermediates it may use: its chain building (fetching a missing root, the
    end certificate's revocation) and its SSL policy (the host name, the server use). `engine`: a
    chain engine (the tests' own roots); the current user's by default."""
    if not chain:
        return False
    try:
        api = _crypt32()
    except (AttributeError, OSError):
        return False
    store = api.CertOpenStore(_STORE_PROV_MEMORY, 0, None, 0, None)
    if not store:
        return False
    leaf = ctypes.c_void_p()
    context = ctypes.POINTER(_ChainContext)()
    try:
        for index, der in enumerate(chain):
            added = api.CertAddEncodedCertificateToStore(store, _ENCODING, der, len(der), _STORE_ADD_ALWAYS,
                                                         ctypes.byref(leaf) if index == 0 else None)
            if not added and index == 0:
                return False
        usage = (ctypes.c_char_p * 1)(_SERVER_AUTH)
        para = _ChainPara(ctypes.sizeof(_ChainPara), _UsageMatch(0, _EnhKeyUsage(1, usage)))
        if not api.CertGetCertificateChain(engine, leaf, None, store, ctypes.byref(para),
                                           _REVOCATION_CHECK_END_CERT | _REVOCATION_ACCUMULATIVE_TIMEOUT,
                                           None, ctypes.byref(context)) or not context:
            return False
        if context.contents.TrustStatus.dwErrorStatus & ~_REVOCATION_UNKNOWN:
            return False
        server = _SslPolicyPara(ctypes.sizeof(_SslPolicyPara), _AUTHTYPE_SERVER, 0, host)
        policy = _PolicyPara(ctypes.sizeof(_PolicyPara), _POLICY_IGNORE_ALL_REV_UNKNOWN,
                             ctypes.cast(ctypes.pointer(server), ctypes.c_void_p))
        status = _PolicyStatus(ctypes.sizeof(_PolicyStatus))
        if not api.CertVerifyCertificateChainPolicy(_POLICY_SSL, context, ctypes.byref(policy), ctypes.byref(status)):
            return False
        return status.dwError == 0
    finally:
        if context:
            api.CertFreeCertificateChain(context)
        if leaf:
            api.CertFreeCertificateContext(leaf)
        api.CertCloseStore(store, 0)
