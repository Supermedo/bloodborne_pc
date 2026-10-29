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
    'Online servers: the port now plays on its own servers. SuperMedo Server is the default and '
    "recommended; The Hunter's Dream stays available for any players who want to use their server. "
    'Other servers can no longer be chosen: if you used one, you now play on SuperMedo Server. Sign '
    'in, or make a free account, on the Online page.',
    'Join our Discord',
    'What changed in this version, and what to know.',
]
TEXTS = {
    'ru': ['ЧТО НОВОГО', 'Версия {}', 'Продолжить', 'Полный список на GitHub', 'Важно',
           'Онлайн-серверы: теперь порт играет на собственных серверах. SuperMedo Server выбран по умолчанию и '
           'рекомендуется; The Hunter\'s Dream остаётся доступным для всех игроков, которые хотят играть на его сервере. Другие серверы больше нельзя выбрать: если '
           'вы пользовались одним из них, теперь вы играете на SuperMedo Server. Войдите или создайте бесплатный '
           'аккаунт на странице «Онлайн».',
           'Наш Discord',
           'Что изменилось в этой версии и что нужно знать.'],
    'ar': ['ما الجديد', 'الإصدار {}', 'متابعة', 'الملاحظات الكاملة على GitHub', 'مهم',
           'خوادم اللعب عبر الإنترنت: يعمل المنفذ الآن على خوادمه الخاصة. خادم SuperMedo هو الافتراضي والموصى به؛ '
           'ويبقى The Hunter\'s Dream متاحاً لكل اللاعبين الذين يريدون استخدام خادمه. لم يعد بالإمكان اختيار خوادم أخرى: إذا كنت تستخدم أحدها، فأنت '
           'تلعب الآن على خادم SuperMedo. سجّل الدخول أو أنشئ حساباً مجانياً في صفحة اللعب عبر الإنترنت.',
           'انضم إلى Discord',
           'ما الذي تغيّر في هذا الإصدار وما يجب معرفته.'],
    'es': ['NOVEDADES', 'Versión {}', 'Continuar', 'Notas completas en GitHub', 'Importante',
           'Servidores en línea: el port ahora juega en sus propios servidores. SuperMedo Server es el predeterminado '
           'y el recomendado; The Hunter\'s Dream sigue disponible para quien quiera usar su servidor. Ya no se pueden elegir otros servidores: '
           'si usabas uno, ahora juegas en SuperMedo Server. Inicia sesión o crea una cuenta gratuita en la página En '
           'línea.',
           'Únete a nuestro Discord',
           'Qué cambió en esta versión y qué debes saber.'],
    'pt': ['NOVIDADES', 'Versão {}', 'Continuar', 'Notas completas no GitHub', 'Importante',
           'Servidores online: o port agora joga em servidores próprios. O SuperMedo Server é o padrão e o '
           'recomendado; The Hunter\'s Dream continua disponível para quem quiser usar o servidor dele. Outros servidores não podem mais ser '
           'escolhidos: se você usava um, agora joga no SuperMedo Server. Entre ou crie uma conta gratuita na página '
           'Online.',
           'Entre no nosso Discord',
           'O que mudou nesta versão e o que você precisa saber.'],
    'fr': ['NOUVEAUTÉS', 'Version {}', 'Continuer', 'Notes complètes sur GitHub', 'Important',
           'Serveurs en ligne : le port joue désormais sur ses propres serveurs. SuperMedo Server est le choix par '
           'défaut et recommandé ; The Hunter\'s Dream reste disponible pour les joueurs qui veulent utiliser son serveur. Les autres serveurs ne peuvent '
           'plus être choisis : si vous en utilisiez un, vous jouez maintenant sur SuperMedo Server. Connectez-vous ou '
           'créez un compte gratuit sur la page En ligne.',
           'Rejoignez notre Discord',
           'Ce qui a changé dans cette version, et ce qu’il faut savoir.'],
    'de': ['NEUIGKEITEN', 'Version {}', 'Weiter', 'Alle Notizen auf GitHub', 'Wichtig',
           'Online-Server: Der Port spielt jetzt auf eigenen Servern. SuperMedo Server ist voreingestellt und '
           'empfohlen; The Hunter\'s Dream bleibt für alle verfügbar, die dessen Server nutzen möchten. Andere Server lassen sich nicht mehr wählen: '
           'Wenn du einen genutzt hast, spielst du jetzt auf SuperMedo Server. Melde dich auf der Online-Seite an '
           'oder erstelle dort ein kostenloses Konto.',
           'Tritt unserem Discord bei',
           'Was sich in dieser Version geändert hat und was du wissen solltest.'],
    'it': ['NOVITÀ', 'Versione {}', 'Continua', 'Note complete su GitHub', 'Importante',
           'Server online: il port ora gioca sui propri server. SuperMedo Server è quello predefinito e consigliato; '
           'The Hunter\'s Dream resta disponibile per chi vuole usare il suo server. Gli altri server non si possono più scegliere: se ne usavi '
           'uno, ora giochi su SuperMedo Server. Accedi o crea un account gratuito nella pagina Online.',
           'Unisciti al nostro Discord',
           'Cosa è cambiato in questa versione e cosa sapere.'],
    'pl': ['NOWOŚCI', 'Wersja {}', 'Dalej', 'Pełne informacje na GitHubie', 'Ważne',
           'Serwery online: port gra teraz na własnych serwerach. SuperMedo Server jest domyślny i zalecany; The '
           'Hunter\'s Dream pozostaje dostępny dla graczy, którzy chcą korzystać z jego serwera. Innych serwerów nie można już wybrać: jeśli z któregoś '
           'korzystałeś, grasz teraz na SuperMedo Server. Zaloguj się lub załóż darmowe konto na stronie Online.',
           'Dołącz do naszego Discorda',
           'Co zmieniło się w tej wersji i co warto wiedzieć.'],
    'tr': ['YENİLİKLER', 'Sürüm {}', 'Devam', 'GitHub’da tüm notlar', 'Önemli',
           'Çevrim içi sunucular: port artık kendi sunucularında oynuyor. SuperMedo Server varsayılan ve önerilen '
           'seçenektir; The Hunter\'s Dream, sunucusunu kullanmak isteyen tüm oyuncular için kullanılabilir olmaya devam ediyor. Başka sunucular artık seçilemez: '
           'birini kullanıyorduysanız artık SuperMedo Server’da oynuyorsunuz. Çevrim içi sayfasında giriş yapın '
           'veya ücretsiz bir hesap oluşturun.',
           'Discord’umuza katıl',
           'Bu sürümde neler değişti ve bilmeniz gerekenler.'],
    'zh': ['新内容', '版本 {}', '继续', '在 GitHub 上查看完整说明', '重要',
           '在线服务器：本移植现在使用自己的服务器。SuperMedo Server 为默认且推荐的选择；The Hunter\'s Dream '
           '仍可供想使用其服务器的玩家使用。已无法再选择其他服务器：如果你之前使用其他服务器，现在将在 SuperMedo Server 上游戏。'
           '请在“在线”页面登录或创建免费账户。',
           '加入我们的 Discord',
           '此版本的变化，以及需要了解的事项。'],
    'ja': ['新着情報', 'バージョン {}', '続ける', 'GitHub で全文を見る', '重要',
           'オンラインサーバー：本ポートは独自のサーバーでプレイするようになりました。SuperMedo Server が既定かつ'
           '推奨です。The Hunter\'s Dream も、そのサーバーを使いたいプレイヤーは引き続き利用できます。その他のサーバーは選べなくなりました。'
           '使っていた場合は SuperMedo Server でプレイすることになります。オンラインのページでサインインするか、'
           '無料アカウントを作成してください。',
           'Discord に参加',
           'このバージョンの変更点と知っておくべきこと。'],
    'ko': ['새로운 소식', '버전 {}', '계속', 'GitHub에서 전체 내용 보기', '중요',
           '온라인 서버: 이제 포트는 자체 서버에서 플레이합니다. SuperMedo Server가 기본값이자 권장 서버이며, '
           'The Hunter\'s Dream은 해당 서버를 쓰고 싶은 플레이어라면 계속 사용할 수 있습니다. 다른 서버는 더 이상 선택할 수 없습니다. '
           '다른 서버를 쓰고 있었다면 이제 SuperMedo Server에서 플레이합니다. 온라인 페이지에서 로그인하거나 '
           '무료 계정을 만드세요.',
           'Discord 참여하기',
           '이 버전에서 바뀐 점과 알아 둘 내용.'],
}

for _language, _texts in TEXTS.items():
    assert len(_texts) == len(KEYS), (_language, len(_texts), len(KEYS))
    bbport_lang.EXTRA_TRANSLATIONS.setdefault(_language, {}).update(zip(KEYS, _texts))
