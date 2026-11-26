# SPDX-License-Identifier: GPL-2.0-or-later
"""The What's new window (bbport_news): its own texts and each version's notice, and the side bar's
Discord button, in every language
besides English. Keyed by the English text like bbport_lang; merged into its tables on import. A
version's list of changes stays in English, like its release notes."""
import bbport_lang

KEYS = [
    "WHAT'S NEW",
    'Version {}',
    'Continue',
    'Full notes on GitHub',
    'Important',
    'Join our Discord',
    'What changed in this version, and what to know.',
]
TEXTS = {
    'ru': ['ЧТО НОВОГО', 'Версия {}', 'Продолжить', 'Полный список на GitHub', 'Важно',
           'Наш Discord',
           'Что изменилось в этой версии и что нужно знать.'],
    'ar': ['ما الجديد', 'الإصدار {}', 'متابعة', 'الملاحظات الكاملة على GitHub', 'مهم',
           'انضم إلى Discord',
           'ما الذي تغيّر في هذا الإصدار وما يجب معرفته.'],
    'es': ['NOVEDADES', 'Versión {}', 'Continuar', 'Notas completas en GitHub', 'Importante',
           'Únete a nuestro Discord',
           'Qué cambió en esta versión y qué debes saber.'],
    'pt': ['NOVIDADES', 'Versão {}', 'Continuar', 'Notas completas no GitHub', 'Importante',
           'Entre no nosso Discord',
           'O que mudou nesta versão e o que você precisa saber.'],
    'fr': ['NOUVEAUTÉS', 'Version {}', 'Continuer', 'Notes complètes sur GitHub', 'Important',
           'Rejoignez notre Discord',
           'Ce qui a changé dans cette version, et ce qu’il faut savoir.'],
    'de': ['NEUIGKEITEN', 'Version {}', 'Weiter', 'Alle Notizen auf GitHub', 'Wichtig',
           'Tritt unserem Discord bei',
           'Was sich in dieser Version geändert hat und was du wissen solltest.'],
    'it': ['NOVITÀ', 'Versione {}', 'Continua', 'Note complete su GitHub', 'Importante',
           'Unisciti al nostro Discord',
           'Cosa è cambiato in questa versione e cosa sapere.'],
    'pl': ['NOWOŚCI', 'Wersja {}', 'Dalej', 'Pełne informacje na GitHubie', 'Ważne',
           'Dołącz do naszego Discorda',
           'Co zmieniło się w tej wersji i co warto wiedzieć.'],
    'tr': ['YENİLİKLER', 'Sürüm {}', 'Devam', 'GitHub’da tüm notlar', 'Önemli',
           'Discord’umuza katıl',
           'Bu sürümde neler değişti ve bilmeniz gerekenler.'],
    'zh': ['新内容', '版本 {}', '继续', '在 GitHub 上查看完整说明', '重要',
           '加入我们的 Discord',
           '此版本的变化，以及需要了解的事项。'],
    'ja': ['新着情報', 'バージョン {}', '続ける', 'GitHub で全文を見る', '重要',
           'Discord に参加',
           'このバージョンの変更点と知っておくべきこと。'],
    'ko': ['새로운 소식', '버전 {}', '계속', 'GitHub에서 전체 내용 보기', '중요',
           'Discord 참여하기',
           '이 버전에서 바뀐 점과 알아 둘 내용.'],
}

for _language, _texts in TEXTS.items():
    assert len(_texts) == len(KEYS), (_language, len(_texts), len(KEYS))
    bbport_lang.EXTRA_TRANSLATIONS.setdefault(_language, {}).update(zip(KEYS, _texts))
