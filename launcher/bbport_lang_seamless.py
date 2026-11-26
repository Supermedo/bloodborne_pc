# SPDX-License-Identifier: GPL-2.0-or-later
"""Localized texts of the SuperMedo Seamless co-op choice on the Online page (bbport_launcher_win.py)."""

KEYS = (
    'SuperMedo Seamless co-op (beta)',
    'SuperMedo Seamless co-op (beta): BBSC seamless co-op (up to 5 hunters through deaths, lamps and '
    'areas) on its own SuperMedo server, with the same account. Everyone in a session picks it.',
)
TEXTS = {
    'ru': (
        'Бесшовный кооператив SuperMedo (бета)',
        'Бесшовный кооператив SuperMedo (бета): бесшовный кооператив BBSC (до 5 охотников сквозь смерти, фонари '
        'и локации) на отдельном сервере SuperMedo, с тем же аккаунтом. Его выбирают все участники сессии.',
    ),
    'ar': (
        'اللعب التعاوني السلس من SuperMedo (تجريبي)',
        'اللعب التعاوني السلس من SuperMedo (تجريبي): اللعب التعاوني السلس من BBSC (حتى 5 صيادين عبر الموت '
        'والفوانيس والمناطق) على خادم SuperMedo خاص به، بالحساب نفسه. يختاره كل من في الجلسة.',
    ),
    'es': (
        'Cooperativo continuo de SuperMedo (beta)',
        'Cooperativo continuo de SuperMedo (beta): el cooperativo continuo de BBSC (hasta 5 cazadores a través de '
        'muertes, faroles y zonas) en su propio servidor de SuperMedo, con la misma cuenta. Todos en la sesión lo eligen.',
    ),
    'pt': (
        'Cooperativo contínuo SuperMedo (beta)',
        'Cooperativo contínuo SuperMedo (beta): o cooperativo contínuo do BBSC (até 5 caçadores através de mortes, '
        'lanternas e áreas) no seu próprio servidor SuperMedo, com a mesma conta. Todos na sessão o escolhem.',
    ),
    'fr': (
        'Coop continue SuperMedo (bêta)',
        'Coop continue SuperMedo (bêta) : la coop continue de BBSC (jusqu\'à 5 chasseurs à travers les morts, les '
        'lanternes et les zones) sur son propre serveur SuperMedo, avec le même compte. Tous les joueurs de la session '
        'le choisissent.',
    ),
    'de': (
        'SuperMedo Seamless-Koop (Beta)',
        'SuperMedo Seamless-Koop (Beta): BBSCs nahtloser Koop (bis zu 5 Jäger über Tode, Laternen und Gebiete '
        'hinweg) auf einem eigenen SuperMedo-Server, mit demselben Konto. Alle in einer Sitzung wählen ihn.',
    ),
    'it': (
        'Coop continua SuperMedo (beta)',
        'Coop continua SuperMedo (beta): la coop continua di BBSC (fino a 5 cacciatori attraverso morti, lanterne e '
        'zone) su un proprio server SuperMedo, con lo stesso account. Tutti nella sessione la scelgono.',
    ),
    'pl': (
        'Płynna kooperacja SuperMedo (beta)',
        'Płynna kooperacja SuperMedo (beta): płynna kooperacja BBSC (do 5 łowców przez śmierci, latarnie i obszary) '
        'na własnym serwerze SuperMedo, z tym samym kontem. Wybierają ją wszyscy w sesji.',
    ),
    'tr': (
        'SuperMedo Kesintisiz Ortak Oyun (beta)',
        'SuperMedo Kesintisiz Ortak Oyun (beta): BBSC kesintisiz ortak oyunu (ölümler, fenerler ve bölgeler boyunca 5 '
        'avcıya kadar) kendi SuperMedo sunucusunda, aynı hesapla. Oturumdaki herkes bunu seçer.',
    ),
    'zh': (
        'SuperMedo 无缝合作（测试版）',
        'SuperMedo 无缝合作（测试版）：BBSC 无缝合作（最多 5 名猎人，跨越死亡、提灯和区域）在其专属 SuperMedo 服务器上，'
        '使用同一账号。会话中的每个人都要选择它。',
    ),
    'ja': (
        'SuperMedo シームレス協力（ベータ）',
        'SuperMedo シームレス協力（ベータ）：BBSC のシームレス協力（死亡、灯り、エリアをまたいで最大 5 人の狩人）を、専用の '
        'SuperMedo サーバーで同じアカウントのまま。セッションの全員がこれを選びます。',
    ),
    'ko': (
        'SuperMedo 끊김 없는 협동 (베타)',
        'SuperMedo 끊김 없는 협동 (베타): BBSC의 끊김 없는 협동(죽음, 등불, 지역을 넘어 최대 5명의 사냥꾼)을 전용 '
        'SuperMedo 서버에서 같은 계정으로. 세션의 모두가 이것을 선택합니다.',
    ),
}
TRANSLATIONS = {language: dict(zip(KEYS, texts, strict=True)) for language, texts in TEXTS.items()}
