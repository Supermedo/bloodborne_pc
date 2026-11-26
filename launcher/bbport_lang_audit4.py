# SPDX-License-Identifier: GPL-2.0-or-later
"""The side bar search's new texts: where a side bar button is, the Saves page's actions it finds, and the note
on a setting that shows only for another choice (another online pool, another upscaler). In every language
besides English; keyed by the English text like bbport_lang, merged into its tables on import."""
import bbport_lang

KEYS = [
    # where a result is
    'Side bar',
    # the Saves page's actions
    'Restore a backup',
    'Import saves from other versions',
    # a setting shown only for another choice
    'only with SuperMedo Server',
    'only with an own server',
    'only with FSR 4.1.1',
]

TEXTS = {
    'ru': [
        'Боковая панель',
        'Восстановить резервную копию',
        'Импортировать сохранения из других версий',
        'только с сервером SuperMedo',
        'только со своим сервером',
        'только с FSR 4.1.1',
    ],
    'ar': [
        'الشريط الجانبي',
        'استرجاع نسخة احتياطية',
        'استيراد ملفات الحفظ من إصدارات أخرى',
        'فقط مع خادم SuperMedo',
        'فقط مع خادمك الخاص',
        'فقط مع FSR 4.1.1',
    ],
    'es': [
        'Barra lateral',
        'Restaurar una copia de seguridad',
        'Importar partidas de otras versiones',
        'solo con el servidor SuperMedo',
        'solo con un servidor propio',
        'solo con FSR 4.1.1',
    ],
    'pt': [
        'Barra lateral',
        'Restaurar um backup',
        'Importar saves de outras versões',
        'só com o servidor SuperMedo',
        'só com um servidor próprio',
        'só com FSR 4.1.1',
    ],
    'fr': [
        'Barre latérale',
        'Restaurer une copie de sauvegarde',
        "Importer des sauvegardes d'autres versions",
        'uniquement avec le serveur SuperMedo',
        'uniquement avec un serveur perso',
        'uniquement avec FSR 4.1.1',
    ],
    'de': [
        'Seitenleiste',
        'Sicherung zurückholen',
        'Spielstände aus anderen Versionen importieren',
        'nur mit dem SuperMedo-Server',
        'nur mit eigenem Server',
        'nur mit FSR 4.1.1',
    ],
    'it': [
        'Barra laterale',
        'Ripristina un backup',
        'Importa salvataggi da altre versioni',
        'solo con il server SuperMedo',
        'solo con un server proprio',
        'solo con FSR 4.1.1',
    ],
    'pl': [
        'Pasek boczny',
        'Przywróć kopię zapasową',
        'Importuj zapisy z innych wersji',
        'tylko z serwerem SuperMedo',
        'tylko z własnym serwerem',
        'tylko z FSR 4.1.1',
    ],
    'tr': [
        'Kenar çubuğu',
        'Bir yedeği geri yükle',
        'Diğer sürümlerden kayıtları içe aktar',
        'yalnızca SuperMedo Sunucusu ile',
        'yalnızca kendi sunucunla',
        'yalnızca FSR 4.1.1 ile',
    ],
    'zh': [
        '侧边栏',
        '恢复备份',
        '从其他版本导入存档',
        '仅限 SuperMedo 服务器',
        '仅限自己的服务器',
        '仅限 FSR 4.1.1',
    ],
    'ja': [
        'サイドバー',
        'バックアップを復元',
        '他のバージョンからセーブデータをインポート',
        'SuperMedo サーバーのみ',
        '自分のサーバーのみ',
        'FSR 4.1.1 のみ',
    ],
    'ko': [
        '사이드바',
        '백업 복원',
        '다른 버전에서 세이브 가져오기',
        'SuperMedo 서버에서만',
        '자체 서버에서만',
        'FSR 4.1.1에서만',
    ],
}

for _language, _texts in TEXTS.items():
    assert len(_texts) == len(KEYS), (_language, len(_texts), len(KEYS))
    bbport_lang.EXTRA_TRANSLATIONS.setdefault(_language, {}).update(zip(KEYS, _texts))
