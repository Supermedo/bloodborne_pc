"""What the update check offers (launcher/bbport_launcher_win.py update_to_offer): a newer release to
everyone; to a beta also its own version's release, else the next beta, so testers go from beta to
beta and on to the release without downloading by hand."""
from paths import ROOT
import io
import json
import sys
import unittest
from unittest import mock

sys.path.insert(0, str(ROOT / 'launcher'))
sys.path.insert(0, str(ROOT / 'scripts'))
import bbport_launcher_win as L  # noqa: E402


def release(tag, zip_name, prerelease=False):
    return {'tag_name': tag, 'prerelease': prerelease, 'draft': False, 'html_url': f'https://example.invalid/{tag}',
            'assets': [{'name': zip_name, 'browser_download_url': f'https://example.invalid/{zip_name}'},
                       {'name': 'SHA256SUMS.txt', 'browser_download_url': 'https://example.invalid/sums'}]}


RELEASE_194 = release('windows-v1.9.4', 'Bloodborne-Windows.zip')
RELEASE_195 = release('windows-v1.9.5', 'Bloodborne-Windows.zip')
BETAS = [release('v1.9.5-beta.3', 'Bloodborne-1.9.5-beta.3.zip', True),
         release('v1.9.5-beta.2', 'Bloodborne-1.9.5-beta.2.zip', True),
         release('v1.9.5-beta.1', 'Bloodborne-1.9.5-beta.1.zip', True),
         release('v1.9.2-launcher-test.6', 'Bloodborne-Launcher-1.9.2-redesign-test.6.zip', True)]


class Answers:
    """urlopen for the two addresses the check asks: Supermedo's latest release and the betas."""

    def __init__(self, latest, betas):
        self.latest, self.betas, self.asked = latest, betas, []

    def __call__(self, request, timeout=None):
        url = request.full_url
        self.asked.append(url)
        data = self.latest if url == L.RELEASES_API else self.betas if url == L.BETAS_API else None
        if data is None:
            raise OSError(f'unexpected {url}')
        response = io.BytesIO(json.dumps(data).encode())
        response.__enter__ = lambda self=response: self
        response.__exit__ = lambda *a: None
        return response


def offer(version, beta, latest, betas):
    answers = Answers(latest, betas)
    with mock.patch.object(L, 'VERSION', version), mock.patch.object(L, 'BETA', beta), \
            mock.patch.object(L.bbport_tls, 'urlopen', answers):
        return L.update_to_offer(), answers.asked


class BetaChannelTests(unittest.TestCase):
    def test_beta_2_gets_beta_3_while_the_release_is_1_9_4(self):
        found, _asked = offer('1.9.5', 2, RELEASE_194, BETAS)
        self.assertEqual(found[:2], ('1.9.5 beta 3', 'https://example.invalid/Bloodborne-1.9.5-beta.3.zip'))

    def test_a_beta_gets_its_release_once_it_is_out_even_with_a_newer_beta_listed(self):
        later_beta = [release('v1.9.6-beta.1', 'Bloodborne-1.9.6-beta.1.zip', True)] + BETAS
        found, _asked = offer('1.9.5', 2, RELEASE_195, later_beta)
        self.assertEqual(found[:2], ('1.9.5', 'https://example.invalid/Bloodborne-Windows.zip'))

    def test_the_newest_beta_offers_nothing(self):
        found, _asked = offer('1.9.5', 3, RELEASE_194, BETAS)
        self.assertIsNone(found)

    def test_a_beta_of_a_later_version_counts_as_newer(self):
        found, _asked = offer('1.9.5', 3, RELEASE_194, [release('v1.9.6-beta.1', 'Bloodborne-1.9.6-beta.1.zip', True)])
        self.assertEqual(found[0], '1.9.6 beta 1')

    def test_a_release_build_never_asks_for_betas(self):
        found, asked = offer('1.9.4', 0, RELEASE_194, BETAS)
        self.assertIsNone(found)
        self.assertEqual(asked, [L.RELEASES_API])

    def test_a_release_build_gets_the_newer_release(self):
        found, _asked = offer('1.9.4', 0, RELEASE_195, BETAS)
        self.assertEqual(found[:2], ('1.9.5', 'https://example.invalid/Bloodborne-Windows.zip'))

    def test_the_betas_still_answer_when_the_release_check_fails(self):
        answers = Answers(None, BETAS)  # the release address raises
        with mock.patch.object(L, 'VERSION', '1.9.5'), mock.patch.object(L, 'BETA', 1), \
                mock.patch.object(L.bbport_tls, 'urlopen', answers):
            self.assertEqual(L.update_to_offer()[0], '1.9.5 beta 3')

    def test_nothing_answered_is_an_error_for_the_manual_check(self):
        answers = Answers(None, None)
        with mock.patch.object(L, 'VERSION', '1.9.5'), mock.patch.object(L, 'BETA', 2), \
                mock.patch.object(L.bbport_tls, 'urlopen', answers):
            with self.assertRaises(OSError):
                L.update_to_offer()


if __name__ == '__main__':
    unittest.main()
