// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: HTTPS servers Windows trusts are trusted here too. OpenSSL checks the server's chain
// against the Windows ROOT and CA stores as they are when the game starts, plus the roots below
// that the port carries itself; a root Windows only fetches on demand (ISRG Root X1 on a fresh
// install), one it doesn't have yet (Let's Encrypt's Root YE and YR) or the certificate an
// antivirus' HTTPS scanning signs the connection with (Avast, AVG, Kaspersky) can fail that check
// while the launcher, browsers and the rest of Windows connect ("SSL server verification failed",
// "Failed to acquire Bloodborne server information"). When OpenSSL rejects the chain, the same
// chain and host name go through Windows' own chain building and SSL policy
// (CertGetCertificateChain, which fetches missing roots and intermediates): accepted when Windows
// accepts it, rejected otherwise. Windows gets the whole chain the server sent, not the leaf alone:
// from the leaf alone it could build through an expired cross-signed ISRG Root X2 in the user's
// intermediate store and answer 0x1 (not time valid). BB_WINDOWS_TRUST=0: OpenSSL's answer only.
#pragma once

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

#include <httplib.h>

namespace bbnet {

inline std::string HostOf(const std::string& url) {
    std::string host = url;
    if (const auto scheme = host.find("://"); scheme != std::string::npos) host.erase(0, scheme + 3);
    if (const auto end = host.find_first_of("/?#"); end != std::string::npos) host.erase(end);
    if (const auto at = host.rfind('@'); at != std::string::npos) host.erase(0, at + 1);
    if (!host.empty() && host[0] == '[') {
        if (const auto close = host.find(']'); close != std::string::npos) return host.substr(1, close - 1);
    }
    if (const auto colon = host.rfind(':'); colon != std::string::npos) host.erase(colon);
    return host;
}

// The roots Let's Encrypt's chains end in, carried by the port so a server with a Let's Encrypt
// certificate (game.supermedo.com) is trusted before Windows has fetched them:
// ISRG Root X1 and X2 (in every trust store, but on a fresh Windows only fetched when something
// first needs them) and Root YE and YR (Let's Encrypt's 2025 roots, which issue its certificates
// since 2026 through YE1-YE3 and YR1-YR3). Self-signed, from letsencrypt.org/certificates, SHA-256:
//   ISRG Root X1  96BCEC06264976F37460779ACF28C5A7CFE8A3C0AAE11A8FFCEE05C0BDDF08C6
//   ISRG Root X2  69729B8E15A86EFC177A57AFB7171DFC64ADD28C2FCA8CF1507E34453CCB1470
//   Root YE       E14FFCAD5B0025731006CAA43A121A22D8E9700F4FB9CF852F02A708AA5D5666
//   Root YR       E57B7E6F150C419102E8D5C055729FF967B9D1A829BF00CEC89CA604EBF4A86F
// (tests/test_windows_trust.cpp checks them.)
inline constexpr const char kBundledRoots[] = R"(-----BEGIN CERTIFICATE-----
MIIFazCCA1OgAwIBAgIRAIIQz7DSQONZRGPgu2OCiwAwDQYJKoZIhvcNAQELBQAw
TzELMAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2Vh
cmNoIEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDEwHhcNMTUwNjA0MTEwNDM4
WhcNMzUwNjA0MTEwNDM4WjBPMQswCQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJu
ZXQgU2VjdXJpdHkgUmVzZWFyY2ggR3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBY
MTCCAiIwDQYJKoZIhvcNAQEBBQADggIPADCCAgoCggIBAK3oJHP0FDfzm54rVygc
h77ct984kIxuPOZXoHj3dcKi/vVqbvYATyjb3miGbESTtrFj/RQSa78f0uoxmyF+
0TM8ukj13Xnfs7j/EvEhmkvBioZxaUpmZmyPfjxwv60pIgbz5MDmgK7iS4+3mX6U
A5/TR5d8mUgjU+g4rk8Kb4Mu0UlXjIB0ttov0DiNewNwIRt18jA8+o+u3dpjq+sW
T8KOEUt+zwvo/7V3LvSye0rgTBIlDHCNAymg4VMk7BPZ7hm/ELNKjD+Jo2FR3qyH
B5T0Y3HsLuJvW5iB4YlcNHlsdu87kGJ55tukmi8mxdAQ4Q7e2RCOFvu396j3x+UC
B5iPNgiV5+I3lg02dZ77DnKxHZu8A/lJBdiB3QW0KtZB6awBdpUKD9jf1b0SHzUv
KBds0pjBqAlkd25HN7rOrFleaJ1/ctaJxQZBKT5ZPt0m9STJEadao0xAH0ahmbWn
OlFuhjuefXKnEgV4We0+UXgVCwOPjdAvBbI+e0ocS3MFEvzG6uBQE3xDk3SzynTn
jh8BCNAw1FtxNrQHusEwMFxIt4I7mKZ9YIqioymCzLq9gwQbooMDQaHWBfEbwrbw
qHyGO0aoSCqI3Haadr8faqU9GY/rOPNk3sgrDQoo//fb4hVC1CLQJ13hef4Y53CI
rU7m2Ys6xt0nUW7/vGT1M0NPAgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNV
HRMBAf8EBTADAQH/MB0GA1UdDgQWBBR5tFnme7bl5AFzgAiIyBpY9umbbjANBgkq
hkiG9w0BAQsFAAOCAgEAVR9YqbyyqFDQDLHYGmkgJykIrGF1XIpu+ILlaS/V9lZL
ubhzEFnTIZd+50xx+7LSYK05qAvqFyFWhfFQDlnrzuBZ6brJFe+GnY+EgPbk6ZGQ
3BebYhtF8GaV0nxvwuo77x/Py9auJ/GpsMiu/X1+mvoiBOv/2X/qkSsisRcOj/KK
NFtY2PwByVS5uCbMiogziUwthDyC3+6WVwW6LLv3xLfHTjuCvjHIInNzktHCgKQ5
ORAzI4JMPJ+GslWYHb4phowim57iaztXOoJwTdwJx4nLCgdNbOhdjsnvzqvHu7Ur
TkXWStAmzOVyyghqpZXjFaH3pO3JLF+l+/+sKAIuvtd7u+Nxe5AW0wdeRlN8NwdC
jNPElpzVmbUq4JUagEiuTDkHzsxHpFKVK7q4+63SM1N95R1NbdWhscdCb+ZAJzVc
oyi3B43njTOQ5yOf+1CceWxG1bQVs5ZufpsMljq4Ui0/1lvh+wjChP4kqKOJ2qxq
4RgqsahDYVvTH9w7jXbyLeiNdd8XM2w9U/t7y0Ff/9yi0GE44Za4rF2LN9d11TPA
mRGunUHBcnWEvgJBQl9nJEiU0Zsnvgc/ubhPgXRR4Xq37Z0j4r7g1SgEEzwxA57d
emyPxgcYxn/eR44/KJ4EBs+lVDR3veyJm+kXQ99b21/+jh5Xos1AnX5iItreGCc=
-----END CERTIFICATE-----
-----BEGIN CERTIFICATE-----
MIICGzCCAaGgAwIBAgIQQdKd0XLq7qeAwSxs6S+HUjAKBggqhkjOPQQDAzBPMQsw
CQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJuZXQgU2VjdXJpdHkgUmVzZWFyY2gg
R3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBYMjAeFw0yMDA5MDQwMDAwMDBaFw00
MDA5MTcxNjAwMDBaME8xCzAJBgNVBAYTAlVTMSkwJwYDVQQKEyBJbnRlcm5ldCBT
ZWN1cml0eSBSZXNlYXJjaCBHcm91cDEVMBMGA1UEAxMMSVNSRyBSb290IFgyMHYw
EAYHKoZIzj0CAQYFK4EEACIDYgAEzZvVn4CDCuwJSvMWSj5cz3es3mcFDR0HttwW
+1qLFNvicWDEukWVEYmO6gbf9yoWHKS5xcUy4APgHoIYOIvXRdgKam7mAHf7AlF9
ItgKbppbd9/w+kHsOdx1ymgHDB/qo0IwQDAOBgNVHQ8BAf8EBAMCAQYwDwYDVR0T
AQH/BAUwAwEB/zAdBgNVHQ4EFgQUfEKWrt5LSDv6kviejM9ti6lyN5UwCgYIKoZI
zj0EAwMDaAAwZQIwe3lORlCEwkSHRhtFcP9Ymd70/aTSVaYgLXTWNLxBo1BfASdW
tL4ndQavEi51mI38AjEAi/V3bNTIZargCyzuFJ0nN6T5U6VR5CmD1/iQMVtCnwr1
/q4AaOeMSQ+2b1tbFfLn
-----END CERTIFICATE-----
-----BEGIN CERTIFICATE-----
MIIB2TCCAWCgAwIBAgIRAKQCa6LvbHwg1AR+XmWmk4AwCgYIKoZIzj0EAwMwLjEL
MAkGA1UEBhMCVVMxDTALBgNVBAoTBElTUkcxEDAOBgNVBAMTB1Jvb3QgWUUwHhcN
MjUwOTAzMDAwMDAwWhcNNDUwOTAyMjM1OTU5WjAuMQswCQYDVQQGEwJVUzENMAsG
A1UEChMESVNSRzEQMA4GA1UEAxMHUm9vdCBZRTB2MBAGByqGSM49AgEGBSuBBAAi
A2IABDwS/6vhrcVqcbBo+wgdI3fwn9x7DNJJOY/lTOti0vkwuRN87RhEhTH17E7X
yFjWsPYhIPt/wzOqxTd2b+4ZJNy9ID04YywF9U5zasDVyGSNErVNtz8uSGh5izW8
7j77GaNCMEAwDgYDVR0PAQH/BAQDAgEGMA8GA1UdEwEB/wQFMAMBAf8wHQYDVR0O
BBYEFKPIJlqOoUzQNWP8myPIOq5W809WMAoGCCqGSM49BAMDA2cAMGQCMHhMr8N9
LdL1VQKs9BdV81r76eXRB6mtjuNjzk6/lBsPNToWLTDzGYgtQKO1jl63uAIwGV7m
onyF377c+MM1oqVNs17sgu7F9YKZwgLmVbeOMDbKAXHtKMDLbiGllCcs8f47
-----END CERTIFICATE-----
-----BEGIN CERTIFICATE-----
MIIFKTCCAxGgAwIBAgIRAOxGNJNgz0sP+KmC2Tqpyj0wDQYJKoZIhvcNAQELBQAw
LjELMAkGA1UEBhMCVVMxDTALBgNVBAoTBElTUkcxEDAOBgNVBAMTB1Jvb3QgWVIw
HhcNMjUwOTAzMDAwMDAwWhcNNDUwOTAyMjM1OTU5WjAuMQswCQYDVQQGEwJVUzEN
MAsGA1UEChMESVNSRzEQMA4GA1UEAxMHUm9vdCBZUjCCAiIwDQYJKoZIhvcNAQEB
BQADggIPADCCAgoCggIBANvGJnN78CTJdWL3+eGfsLN5TrNBJs+VH9hRXqRbwxu9
sGNiB0BD1fcOxbSUQCJIM1xE13Db+5Cw1w0s0EBYsvuIP/6joF0w8cuImbgR1OGg
YbSQ4OpzI+DG8SGuTlcE873OCS+kh3srlo6vl43M5OJg4Aeo1sfHp6kTJDoIiFBN
JAY+OKfX/FUvYKuhjT+no49lmqmupSBI5PkBQiqrEGtWU5uxU/cQWHGu8jSjFBzn
ZqvbNPLMXMLFxCb3WTfrJBXXjqvWG+v4bjzxjjeAtOlU7qarRDvNOyAuQYLln904
M+faKx8hnLCpJ15ZqaEgcNlY+9MMWcC5yvL2A2j3l9+2buggZX+dOE91zYmIdawT
vSZuVvlbRrAlLxIB6pwMBjneXCjYQ8+3BCCjssbSNpZU3hTcBDdhfAlEDlYr6pEa
tnMdmDT5BqnKC92bd0EhM1fbLHioLccLCuievT8ZkPhZrq7Mii7gNXAcUEAR8+lz
Yal+9zTg7C5DALyVOeG/CqfRAMn1KSHCR0NSA6P8tn/mGRlnCct5rtVCLnVySVpU
6H1qGg3DgTOuskf8eahTMiYbI5ezPJmO5ertalskQ1utp74+eDy92PI4ftHKTbq9
IWhH4YZKh3WnJEIt+oQvlYZbY8tpEroKrFB6PFGzrJIDRyts4HqvuH52RFj2zv/B
AgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNVHRMBAf8EBTADAQH/MB0GA1Ud
DgQWBBTe51tg0CJtQCh9Pw0B/qS1UrRRlDANBgkqhkiG9w0BAQsFAAOCAgEAWHnf
713Bdkq7t5yN2dNIgQakUb94X9WuyhMEHHkgx4oDpSUlnG0w4g94MoqaEUE31ZjR
LU7L5LD1g9ujFHTQu8AD215AHMVQFbm6j8hQxdXHAzDajFNQnOlDJrLjzIx176oy
AjvUtejZx2NNmdb5fd0WGVGsCdoAJ3N8ozo7ajE8t6vfxStZb4BQ9WYJGHUDrv2N
i5tJF6CNiPnlzs3BUfECRbE4JSk+jvy8+VoGiFE8qsH/j78x2fjgQhAQFV7P7Zxy
dBTZ1wEkNpZNW2qnaK1SKBLa+xf6E06YRIq5uaI+HWH8SY1y5VbRgzq40EKg3yxP
06fz+uYAUIFJoLNfhwRCc3Q6pQVuMX3yAjHAes4gk4moGcLQ5p7HAh39yeylZc1J
41sx/jKwLIkPE6Rr1Nf4pxdsxf9SA4yOEiAkDgq04DVxn8hgYFdUtBCuiuVC2heA
EiqVEa+8QZjuw8Gj0EbHXcRd1nInvGqRS1o9Is7YBdQN57X1AYveGBNNqjICSb7c
awuw1EawTDrs13VUlJVEsbQ0/O/1aaV73mCdOQ8azqL2KTv1Ewu1xbquE2S+kdQU
To9TUwat3wUA6cwXh1EfpS/3fJ0aGah5hdpRyoCLDlsSn8tkrjMfFFX0viC+GxHc
sI1ANRYvqSFC2X1VRZfDg+wD6E21BccmifG4yWc=
-----END CERTIFICATE-----)";

#if defined(CPPHTTPLIB_OPENSSL_SUPPORT)
// A trust store with the roots above. cpp-httplib adds the Windows ROOT and CA stores to it when
// it connects (as to its own empty store), so Windows' roots keep working.
inline X509_STORE* BundledRootStore() {
    X509_STORE* store = X509_STORE_new();
    if (!store) return nullptr;
    if (BIO* bio = BIO_new_mem_buf(kBundledRoots, -1)) {
        while (X509* cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr)) {
            X509_STORE_add_cert(store, cert);
            X509_free(cert);
        }
        BIO_free(bio);
    }
    ERR_clear_error(); // the end of the PEM text
    return store;
}
#endif

#if defined(_WIN32) && defined(CPPHTTPLIB_OPENSSL_SUPPORT)
// Windows' chain building and SSL policy for the chain the server sent (the leaf, then the
// intermediates and cross-signed roots, as an extra store Windows may build through) and the host
// name. Unknown or offline revocation doesn't count against it, like cpp-httplib's own policy.
inline bool VerifyChainWithWindows(SSL* ssl, const std::string& host, uint64_t& error) {
    error = 0;
    STACK_OF(X509)* sent = ssl ? SSL_get_peer_cert_chain(ssl) : nullptr; // a client's: leaf first
    if (!sent || sk_X509_num(sent) == 0) return false;
    HCERTSTORE extra = CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, CERT_STORE_CREATE_NEW_FLAG, nullptr);
    if (!extra) {
        error = GetLastError();
        return false;
    }
    PCCERT_CONTEXT leaf = nullptr;
    for (int i = 0; i < sk_X509_num(sent); i++) {
        unsigned char* der = nullptr;
        const int size = i2d_X509(sk_X509_value(sent, i), &der);
        if (size <= 0) continue;
        PCCERT_CONTEXT added = nullptr;
        CertAddEncodedCertificateToStore(extra, X509_ASN_ENCODING, der, static_cast<DWORD>(size),
                                         CERT_STORE_ADD_USE_EXISTING, i == 0 ? &added : nullptr);
        OPENSSL_free(der);
        if (i == 0) leaf = added;
    }
    bool ok = false;
    if (leaf) {
        CERT_CHAIN_PARA para = {};
        para.cbSize = sizeof(para);
        PCCERT_CHAIN_CONTEXT chain = nullptr;
        if (CertGetCertificateChain(nullptr, leaf, nullptr, extra, &para,
                                    CERT_CHAIN_CACHE_END_CERT | CERT_CHAIN_REVOCATION_CHECK_END_CERT |
                                        CERT_CHAIN_REVOCATION_ACCUMULATIVE_TIMEOUT,
                                    nullptr, &chain) &&
            chain) {
            const DWORD status = chain->TrustStatus.dwErrorStatus &
                                 ~DWORD(CERT_TRUST_REVOCATION_STATUS_UNKNOWN | CERT_TRUST_IS_OFFLINE_REVOCATION);
            if (status != CERT_TRUST_NO_ERROR) {
                error = status;
            } else {
                std::wstring whost(host.size() + 1, L'\0');
                whost.resize(MultiByteToWideChar(CP_UTF8, 0, host.c_str(), -1, whost.data(),
                                                 static_cast<int>(whost.size())));
                if (!whost.empty()) whost.pop_back(); // the terminator MultiByteToWideChar counts
                SSL_EXTRA_CERT_CHAIN_POLICY_PARA extra_para = {};
                extra_para.cbSize = sizeof(extra_para);
                extra_para.dwAuthType = AUTHTYPE_SERVER;
                extra_para.pwszServerName = whost.data();
                CERT_CHAIN_POLICY_PARA policy = {};
                policy.cbSize = sizeof(policy);
                policy.dwFlags = CERT_CHAIN_POLICY_IGNORE_ALL_REV_UNKNOWN_FLAGS;
                policy.pvExtraPolicyPara = &extra_para;
                CERT_CHAIN_POLICY_STATUS result = {};
                result.cbSize = sizeof(result);
                if (!CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL, chain, &policy, &result)) {
                    error = GetLastError();
                } else if (result.dwError != 0) {
                    error = result.dwError;
                } else {
                    ok = true;
                }
            }
            CertFreeCertificateChain(chain);
        } else {
            error = GetLastError();
        }
        CertFreeCertificateContext(leaf);
    }
    CertCloseStore(extra, 0);
    return ok;
}
#endif

inline void TrustWindowsRoots(httplib::Client& client, const std::string& url) {
#if defined(CPPHTTPLIB_OPENSSL_SUPPORT)
    client.set_ca_cert_store(BundledRootStore());
#endif
#if defined(_WIN32) && defined(CPPHTTPLIB_OPENSSL_SUPPORT)
    static const bool enabled = [] {
        const char* env = std::getenv("BB_WINDOWS_TRUST");
        return !(env && env[0] == '0');
    }();
    if (!enabled) return;
    client.set_session_verifier([host = HostOf(url)](httplib::tls::session_t session) {
        if (httplib::tls::get_verify_result(session) == 0) {
            return httplib::SSLVerifierResponse::NoDecisionMade;
        }
        uint64_t error = 0;
        if (VerifyChainWithWindows(static_cast<SSL*>(session), host, error)) {
            static std::atomic<int> noted{0};
            if (noted.fetch_add(1) < 3) {
                std::printf("Online: %s's certificate is trusted by Windows (not by the bundled TLS "
                            "library's copy of its stores): connecting\n",
                            host.c_str());
                std::fflush(stdout);
            }
            return httplib::SSLVerifierResponse::CertificateAccepted;
        }
        static std::atomic<int> refused{0};
        if (refused.fetch_add(1) < 3) {
            std::printf("Online: %s's certificate is not trusted by Windows either (error %#llx)\n",
                        host.c_str(), static_cast<unsigned long long>(error));
            std::fflush(stdout);
        }
        return httplib::SSLVerifierResponse::CertificateRejected;
    });
#else
    (void)client;
    (void)url;
#endif
}

} // namespace bbnet
