# SPDX-License-Identifier: GPL-2.0-or-later
"""The video memory warning's switch (Advanced > Performance, beside the low-VRAM profile) and why object
motion vectors are unavailable on a card (Advanced > Upscaling), in every
language besides English and Russian (Russian is in the code). Keyed by the English text like
bbport_lang; merged into its tables on import."""
import bbport_lang

KEYS = [
    'Warn when video memory runs low',
    'Off: no "Video memory is running low" banner in the game. Memory is freed as before; the log still says '
    'when it ran short.',
    'Not available on RX 6000 and older Radeon cards: they lose the device with it.',
    'Not available under 6 GB of video memory: textures need it more.',
]
TEXTS = {
    'ar': [
        'التحذير عند انخفاض ذاكرة الفيديو',
        'إيقاف: لا تظهر لافتة "ذاكرة الفيديو منخفضة" في اللعبة. تُحرَّر الذاكرة كما من قبل، ويذكر السجل متى نقصت.',
        'غير متاح على بطاقات Radeon RX 6000 والأقدم: يفقد الجهاز اتصاله معه.',
        'غير متاح مع أقل من 6 غيغابايت من ذاكرة الفيديو: القوام أحوج إليها.',
    ],
    'es': [
        'Avisar cuando quede poca memoria de vídeo',
        'Desactivado: sin el aviso "Queda poca memoria de vídeo" en el juego. La memoria se libera como antes; el '
        'registro sigue indicando cuándo faltó.',
        'No disponible en Radeon RX 6000 y anteriores: con él se pierde el dispositivo.',
        'No disponible con menos de 6 GB de memoria de vídeo: las texturas la necesitan más.',
    ],
    'pt': [
        'Avisar quando a memória de vídeo estiver baixa',
        'Desligado: sem o aviso "A memória de vídeo está baixa" no jogo. A memória é liberada como antes; o log '
        'continua dizendo quando faltou.',
        'Indisponível em Radeon RX 6000 e anteriores: com ele o dispositivo é perdido.',
        'Indisponível com menos de 6 GB de memória de vídeo: as texturas precisam mais dela.',
    ],
    'fr': [
        'Avertir quand la mémoire vidéo est presque pleine',
        'Désactivé : plus de bandeau « La mémoire vidéo est presque pleine » en jeu. La mémoire est libérée comme '
        'avant ; le journal indique toujours quand elle a manqué.',
        'Indisponible sur les Radeon RX 6000 et plus anciennes : le périphérique est perdu avec.',
        'Indisponible sous 6 Go de mémoire vidéo : les textures en ont plus besoin.',
    ],
    'de': [
        'Warnen, wenn der Videospeicher knapp wird',
        'Aus: kein Hinweis „Videospeicher wird knapp“ im Spiel. Speicher wird wie bisher freigegeben; das Protokoll '
        'nennt weiterhin, wann er knapp war.',
        'Nicht verfügbar auf Radeon RX 6000 und älter: Das Gerät geht damit verloren.',
        'Nicht verfügbar unter 6 GB Videospeicher: Texturen brauchen ihn dringender.',
    ],
    'it': [
        'Avvisa quando la memoria video sta per esaurirsi',
        'Disattivato: nessun avviso "La memoria video sta per esaurirsi" nel gioco. La memoria viene liberata come '
        'prima; il registro indica comunque quando è mancata.',
        'Non disponibile su Radeon RX 6000 e precedenti: con esso il dispositivo viene perso.',
        'Non disponibile sotto i 6 GB di memoria video: servono di più alle texture.',
    ],
    'pl': [
        'Ostrzegaj, gdy kończy się pamięć wideo',
        'Wyłączone: brak komunikatu „Kończy się pamięć wideo” w grze. Pamięć jest zwalniana jak dotąd; dziennik '
        'nadal podaje, kiedy jej zabrakło.',
        'Niedostępne na Radeon RX 6000 i starszych: urządzenie zostaje z nim utracone.',
        'Niedostępne poniżej 6 GB pamięci wideo: bardziej potrzebują jej tekstury.',
    ],
    'tr': [
        'Video belleği azaldığında uyar',
        'Kapalı: oyunda "Video belleği azalıyor" uyarısı çıkmaz. Bellek eskisi gibi boşaltılır; günlük yine ne zaman '
        'yetmediğini yazar.',
        'Radeon RX 6000 ve daha eski kartlarda kullanılamaz: onunla cihaz kaybolur.',
        '6 GB altı video belleğinde kullanılamaz: dokuların ona daha çok ihtiyacı var.',
    ],
    'zh': [
        '显存不足时发出警告',
        '关闭：游戏中不再显示“显存即将不足”横幅。显存仍照常释放；日志仍会记录何时不足。',
        '在 Radeon RX 6000 及更早的显卡上不可用：开启后会丢失设备。',
        '显存低于 6 GB 时不可用：纹理更需要这部分显存。',
    ],
    'ja': [
        'ビデオメモリが不足しそうなときに警告する',
        'オフ：ゲーム内に「ビデオメモリが不足しています」のバナーを表示しません。メモリは従来どおり解放され、'
        '不足した時はログに記録されます。',
        'Radeon RX 6000 以前のカードでは使用できません：有効にするとデバイスが失われます。',
        'ビデオメモリが 6 GB 未満では使用できません：テクスチャに必要です。',
    ],
    'ko': [
        '비디오 메모리가 부족할 때 경고',
        '끄기: 게임에서 "비디오 메모리가 부족합니다" 배너를 표시하지 않습니다. 메모리는 전처럼 해제되며, 부족했던 때는 '
        '로그에 계속 기록됩니다.',
        'Radeon RX 6000 및 이전 카드에서는 사용할 수 없습니다: 켜면 장치가 손실됩니다.',
        '비디오 메모리가 6GB 미만이면 사용할 수 없습니다: 텍스처에 더 필요합니다.',
    ],
}

for _language, _texts in TEXTS.items():
    assert len(_texts) == len(KEYS), (_language, len(_texts), len(KEYS))
    bbport_lang.EXTRA_TRANSLATIONS.setdefault(_language, {}).update(zip(KEYS, _texts))
