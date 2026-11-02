# SPDX-License-Identifier: GPL-2.0-or-later
"""Named object-motion messages shared by the launcher, Help and overlay."""
import bbport_lang

KEYS = {'Gpu': 'Object motion unavailable for this GPU/driver.',
 'Vram': 'Not available under 6 GB of video memory: textures need it more.',
 'Features': 'Object motion unavailable: vertex shader stores/atomics missing.',
 'Limits': 'Object motion unavailable: required SSBO limits not met.',
 'Buffers': 'Object motion unavailable: GPU buffer allocation failed.'}

TEXTS = {
    "en": KEYS,
    'ru': {'Gpu': 'Векторы движения объектов недоступны для этой видеокарты/драйвера.',
     'Vram': 'Недоступно при видеопамяти меньше 6 ГБ: она нужнее текстурам.',
     'Features': 'Векторы движения объектов недоступны: нет записи/атомарных операций в вершинном '
                 'шейдере.',
     'Limits': 'Векторы движения объектов недоступны: недостаточные лимиты SSBO.',
     'Buffers': 'Векторы движения объектов недоступны: не удалось выделить буферы GPU.'},
    'ar': {'Gpu': 'متجهات حركة الأجسام غير متاحة لهذه البطاقة/التعريف.',
     'Vram': 'غير متاح مع أقل من 6 غيغابايت من ذاكرة الفيديو: القوام أحوج إليها.',
     'Features': 'متجهات حركة الأجسام غير متاحة: عمليات الكتابة/العمليات الذرية في مظلل الرؤوس غير '
                 'مدعومة.',
     'Limits': 'متجهات حركة الأجسام غير متاحة: حدود SSBO المطلوبة غير مستوفاة.',
     'Buffers': 'متجهات حركة الأجسام غير متاحة: فشل تخصيص مخازن GPU.'},
    'es': {'Gpu': 'Vectores de movimiento de objetos no disponibles para esta GPU/controlador.',
     'Vram': 'No disponible con menos de 6 GB de memoria de vídeo: las texturas la necesitan más.',
     'Features': 'Vectores de movimiento de objetos no disponibles: faltan escrituras/atómicas del '
                 'shader de vértices.',
     'Limits': 'Vectores de movimiento de objetos no disponibles: límites SSBO insuficientes.',
     'Buffers': 'Vectores de movimiento de objetos no disponibles: falló la asignación de búferes GPU.'},
    'pt': {'Gpu': 'Vetores de movimento de objetos indisponíveis para esta GPU/driver.',
     'Vram': 'Indisponível com menos de 6 GB de memória de vídeo: as texturas precisam mais dela.',
     'Features': 'Vetores de movimento de objetos indisponíveis: faltam escritas/atômicas no shader de '
                 'vértices.',
     'Limits': 'Vetores de movimento de objetos indisponíveis: limites SSBO insuficientes.',
     'Buffers': 'Vetores de movimento de objetos indisponíveis: falha ao alocar buffers da GPU.'},
    'fr': {'Gpu': 'Vecteurs de mouvement des objets indisponibles pour ce GPU/pilote.',
     'Vram': 'Indisponible sous 6 Go de mémoire vidéo : les textures en ont plus besoin.',
     'Features': 'Vecteurs de mouvement des objets indisponibles : écritures/atomiques du vertex '
                 'shader absentes.',
     'Limits': 'Vecteurs de mouvement des objets indisponibles : limites SSBO insuffisantes.',
     'Buffers': 'Vecteurs de mouvement des objets indisponibles : allocation des tampons GPU échouée.'},
    'de': {'Gpu': 'Objektbewegungsvektoren für diese GPU/diesen Treiber nicht verfügbar.',
     'Vram': 'Nicht verfügbar unter 6 GB Videospeicher: Texturen brauchen ihn dringender.',
     'Features': 'Objektbewegungsvektoren nicht verfügbar: Vertex-Shader-Schreibzugriffe/Atomics '
                 'fehlen.',
     'Limits': 'Objektbewegungsvektoren nicht verfügbar: erforderliche SSBO-Limits nicht erfüllt.',
     'Buffers': 'Objektbewegungsvektoren nicht verfügbar: GPU-Puffer konnten nicht angelegt werden.'},
    'it': {'Gpu': 'Vettori di movimento degli oggetti non disponibili per questa GPU/driver.',
     'Vram': 'Non disponibile sotto i 6 GB di memoria video: servono di più alle texture.',
     'Features': 'Vettori di movimento degli oggetti non disponibili: scritture/atomiche del vertex '
                 'shader mancanti.',
     'Limits': 'Vettori di movimento degli oggetti non disponibili: limiti SSBO insufficienti.',
     'Buffers': 'Vettori di movimento degli oggetti non disponibili: allocazione dei buffer GPU non '
                'riuscita.'},
    'pl': {'Gpu': 'Wektory ruchu obiektów niedostępne dla tego GPU/sterownika.',
     'Vram': 'Niedostępne poniżej 6 GB pamięci wideo: bardziej potrzebują jej tekstury.',
     'Features': 'Wektory ruchu obiektów niedostępne: brak zapisów/atomik w shaderze wierzchołków.',
     'Limits': 'Wektory ruchu obiektów niedostępne: niewystarczające limity SSBO.',
     'Buffers': 'Wektory ruchu obiektów niedostępne: nie udało się przydzielić buforów GPU.'},
    'tr': {'Gpu': 'Bu GPU/sürücü için nesne hareket vektörleri kullanılamıyor.',
     'Vram': '6 GB altı video belleğinde kullanılamaz: dokuların ona daha çok ihtiyacı var.',
     'Features': 'Nesne hareket vektörleri kullanılamıyor: köşe gölgelendiricisinde yazma/atomik '
                 'işlemler eksik.',
     'Limits': 'Nesne hareket vektörleri kullanılamıyor: gerekli SSBO sınırları karşılanmıyor.',
     'Buffers': 'Nesne hareket vektörleri kullanılamıyor: GPU tamponları ayrılamadı.'},
    'zh': {'Gpu': '此 GPU/驱动不支持物体运动矢量。',
     'Vram': '显存低于 6 GB 时不可用：纹理更需要这部分显存。',
     'Features': '物体运动矢量不可用：缺少顶点着色器写入/原子操作支持。',
     'Limits': '物体运动矢量不可用：未满足所需的 SSBO 限制。',
     'Buffers': '物体运动矢量不可用：GPU 缓冲区分配失败。'},
    'ja': {'Gpu': 'この GPU/ドライバーではオブジェクトのモーションベクトルを利用できません。',
     'Vram': 'ビデオメモリが 6 GB 未満では使用できません：テクスチャに必要です。',
     'Features': 'オブジェクトのモーションベクトルを利用不可：頂点シェーダーの書き込み/アトミック操作に非対応。',
     'Limits': 'オブジェクトのモーションベクトルを利用不可：必要な SSBO の上限を満たしていません。',
     'Buffers': 'オブジェクトのモーションベクトルを利用不可：GPU バッファーの確保に失敗。'},
    'ko': {'Gpu': '이 GPU/드라이버에서 오브젝트 모션 벡터를 사용할 수 없습니다.',
     'Vram': '비디오 메모리가 6GB 미만이면 사용할 수 없습니다: 텍스처에 더 필요합니다.',
     'Features': '오브젝트 모션 벡터 사용 불가: 버텍스 셰이더 쓰기/원자 연산 미지원.',
     'Limits': '오브젝트 모션 벡터 사용 불가: 필요한 SSBO 한도 미충족.',
     'Buffers': '오브젝트 모션 벡터 사용 불가: GPU 버퍼 할당 실패.'},
}

HELP_KEYS = {
    'Title': 'GPU stopped on Radeon RX 6000',
    'Context': 'The GPU stopped while SSBO object motion was active. The cause is not identified.',
    'Disable': 'Enable “{expert}”, open “{graphics}”, turn “{option}” off, then restart the game.',
    'Repeat': 'Repeat the same scene. If it still fails, follow the graphics-driver checks.',
    'Report': 'Save a BB Report ZIP and report whether this test helped.',
    'Intro': 'The game ended with an error (code {}). Its log suggests this check:',
}
HELP_TEXTS = {
    'en': HELP_KEYS,
    'ru': {
        'Title': 'Сбой GPU на Radeon RX 6000',
        'Context': 'GPU остановился при активных векторах движения через SSBO. Причина не установлена.',
        'Disable': 'Включите «{expert}», откройте «{graphics}», отключите «{option}» и перезапустите игру.',
        'Repeat': 'Повторите ту же сцену. Если сбой остаётся, выполните проверки графического драйвера.',
        'Report': 'Сохраните ZIP BB Report и сообщите, помогла ли эта проверка.',
        'Intro': 'Игра завершилась с ошибкой (код {}). Журнал подсказывает следующую проверку:',
    },
    'ar': {
        'Title': 'توقف GPU على Radeon RX 6000',
        'Context': 'توقف GPU بينما كانت متجهات حركة الأجسام عبر SSBO نشطة. السبب غير محدد.',
        'Disable': 'فعّل «{expert}»، وافتح «{graphics}»، وعطّل «{option}»، ثم أعد تشغيل اللعبة.',
        'Repeat': 'كرر المشهد نفسه. إذا استمر العطل، اتبع فحوصات تعريف الرسوم.',
        'Report': 'احفظ ملف BB Report ZIP واذكر ما إذا كان هذا الاختبار قد ساعد.',
        'Intro': 'انتهت اللعبة بخطأ (الرمز {}). يقترح السجل هذا الفحص:',
    },
    'es': {
        'Title': 'Fallo de GPU en Radeon RX 6000',
        'Context': 'La GPU se detuvo con el movimiento de objetos por SSBO activo. La causa no está identificada.',
        'Disable': 'Activa «{expert}», abre «{graphics}», desactiva «{option}» y reinicia el juego.',
        'Repeat': 'Repite la misma escena. Si vuelve a fallar, sigue las comprobaciones del controlador gráfico.',
        'Report': 'Guarda un ZIP de BB Report e indica si esta prueba ayudó.',
        'Intro': 'El juego terminó con un error (código {}). El registro sugiere esta comprobación:',
    },
    'pt': {
        'Title': 'Falha da GPU na Radeon RX 6000',
        'Context': 'A GPU parou com o movimento de objetos via SSBO ativo. A causa não foi identificada.',
        'Disable': 'Ative “{expert}”, abra “{graphics}”, desative “{option}” e reinicie o jogo.',
        'Repeat': 'Repita a mesma cena. Se a falha continuar, siga as verificações do driver gráfico.',
        'Report': 'Salve um ZIP do BB Report e informe se esse teste ajudou.',
        'Intro': 'O jogo terminou com um erro (código {}). O registro sugere esta verificação:',
    },
    'fr': {
        'Title': 'Arrêt du GPU sur Radeon RX 6000',
        'Context': 'Le GPU s’est arrêté alors que le mouvement des objets par SSBO était actif. La cause est inconnue.',
        'Disable': 'Activez « {expert} », ouvrez « {graphics} », désactivez « {option} », puis relancez le jeu.',
        'Repeat': 'Rejouez la même scène. Si le problème persiste, suivez les vérifications du pilote graphique.',
        'Report': 'Enregistrez un ZIP BB Report et indiquez si ce test a aidé.',
        'Intro': 'Le jeu s’est terminé avec une erreur (code {}). Le journal suggère cette vérification :',
    },
    'de': {
        'Title': 'GPU-Fehler auf Radeon RX 6000',
        'Context': 'Die GPU stoppte bei aktiven SSBO-Objektbewegungsvektoren. Die Ursache ist nicht geklärt.',
        'Disable': '„{expert}“ aktivieren, „{graphics}“ öffnen, „{option}“ ausschalten und das Spiel neu starten.',
        'Repeat': 'Dieselbe Szene erneut testen. Bei erneutem Fehler die Grafiktreiber-Prüfungen durchführen.',
        'Report': 'Ein BB-Report-ZIP speichern und melden, ob die Gegenprobe geholfen hat.',
        'Intro': 'Das Spiel wurde mit einem Fehler beendet (Code {}). Das Protokoll legt diese Prüfung nahe:',
    },
    'it': {
        'Title': 'Arresto della GPU su Radeon RX 6000',
        'Context': 'La GPU si è fermata con il movimento degli oggetti tramite SSBO attivo. La causa non è nota.',
        'Disable': 'Attiva «{expert}», apri «{graphics}», disattiva «{option}» e riavvia il gioco.',
        'Repeat': 'Ripeti la stessa scena. Se il problema persiste, segui i controlli del driver grafico.',
        'Report': 'Salva uno ZIP BB Report e indica se questa prova è stata utile.',
        'Intro': 'Il gioco è terminato con un errore (codice {}). Il registro suggerisce questo controllo:',
    },
    'pl': {
        'Title': 'Błąd GPU na Radeon RX 6000',
        'Context': 'GPU zatrzymało się przy aktywnym ruchu obiektów przez SSBO. Przyczyna nie jest znana.',
        'Disable': 'Włącz „{expert}”, otwórz „{graphics}”, wyłącz „{option}” i uruchom grę ponownie.',
        'Repeat': 'Powtórz tę samą scenę. Jeśli błąd powróci, wykonaj kontrole sterownika graficznego.',
        'Report': 'Zapisz ZIP BB Report i zgłoś, czy ta próba pomogła.',
        'Intro': 'Gra zakończyła się błędem (kod {}). Dziennik sugeruje tę kontrolę:',
    },
    'tr': {
        'Title': 'Radeon RX 6000 üzerinde GPU durdu',
        'Context': 'SSBO nesne hareketi etkinken GPU durdu. Nedeni belirlenmedi.',
        'Disable': '“{expert}” seçeneğini açın, “{graphics}” bölümünü açıp “{option}” seçeneğini kapatın ve oyunu yeniden başlatın.',
        'Repeat': 'Aynı sahneyi tekrar deneyin. Hata sürerse grafik sürücüsü kontrollerini uygulayın.',
        'Report': 'Bir BB Report ZIP dosyası kaydedip bu testin yardımcı olup olmadığını bildirin.',
        'Intro': 'Oyun bir hatayla sona erdi (kod {}). Günlük şu kontrolü öneriyor:',
    },
    'zh': {
        'Title': 'Radeon RX 6000 GPU 停止',
        'Context': 'GPU 在 SSBO 物体运动矢量启用时停止。原因尚未确定。',
        'Disable': '启用“{expert}”，打开“{graphics}”，关闭“{option}”，然后重启游戏。',
        'Repeat': '重新测试相同场景。如果仍然出错，请检查显卡驱动。',
        'Report': '保存 BB Report ZIP，并说明此测试是否有帮助。',
        'Intro': '游戏因错误结束（代码 {}）。日志建议进行以下检查：',
    },
    'ja': {
        'Title': 'Radeon RX 6000 で GPU が停止',
        'Context': 'SSBO オブジェクトモーションが有効な状態で GPU が停止しました。原因は不明です。',
        'Disable': '「{expert}」を有効にして「{graphics}」を開き、「{option}」を無効にしてゲームを再起動してください。',
        'Repeat': '同じ場面を再度テストしてください。再発する場合はグラフィックドライバーの確認に進んでください。',
        'Report': 'BB Report ZIP を保存し、このテストで改善したか報告してください。',
        'Intro': 'ゲームがエラー（コード {}）で終了しました。ログから次の確認をお勧めします：',
    },
    'ko': {
        'Title': 'Radeon RX 6000에서 GPU 중지',
        'Context': 'SSBO 오브젝트 모션이 켜진 상태에서 GPU가 멈췄습니다. 원인은 확인되지 않았습니다.',
        'Disable': '“{expert}”를 켜고 “{graphics}”를 연 다음 “{option}”를 끄고 게임을 다시 시작하세요.',
        'Repeat': '같은 장면을 다시 테스트하세요. 계속 실패하면 그래픽 드라이버를 점검하세요.',
        'Report': 'BB Report ZIP을 저장하고 이 테스트가 도움이 되었는지 알려 주세요.',
        'Intro': '게임이 오류로 종료되었습니다(코드 {}). 로그에서 다음 점검을 권장합니다:',
    },
}

for _language, _texts in TEXTS.items():
    if _texts.keys() != KEYS.keys() or not all(_texts.values()):
        raise ValueError(f"Invalid object-motion translations: {_language}")
    bbport_lang.EXTRA_TRANSLATIONS.setdefault(_language, {}).update(
        (english, _texts[key]) for key, english in KEYS.items())

if HELP_TEXTS.keys() != TEXTS.keys():
    raise ValueError('Object-motion help and reasons must cover the same languages')
for _language, _texts in HELP_TEXTS.items():
    if _texts.keys() != HELP_KEYS.keys() or not all(_texts.values()):
        raise ValueError(f'Invalid object-motion help translations: {_language}')
    bbport_lang.EXTRA_TRANSLATIONS.setdefault(_language, {}).update(
        (english, _texts[key]) for key, english in HELP_KEYS.items())
