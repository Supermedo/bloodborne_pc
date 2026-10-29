# SPDX-License-Identifier: GPL-2.0-or-later
"""The low latency options (Advanced > Performance: Reflex / Anti-Lag 2, frames sent in parts, VRR,
shaders built in the background), in every language besides English and Russian (Russian is in the
code). Keyed by the English text like bbport_lang; merged into its tables on import."""
import bbport_lang

KEYS = [
    'Low latency (Reflex / Anti-Lag 2)',
    'Less input lag: the game starts each frame just in time instead of queuing frames for the '
    'display. NVIDIA Reflex on GeForce, AMD Anti-Lag 2 on Radeon (current drivers); other GPUs ignore '
    'it. Boost keeps the GPU clocks up.',
    'Send frames to the GPU in parts',
    'The graphics card starts drawing a frame while the rest of it is still being prepared, '
    'so it is finished about 2 ms sooner. Uses a little more CPU. New: turn it off if you see '
    'flickering or missing objects.',
    'Variable refresh rate (VRR)',
    'For a G-Sync or FreeSync display: the display follows the frames of the game, without tearing '
    'and without the lag of V-Sync. Uses V-Sync (Presentation is ignored) and keeps the frame rate a '
    'little below the refresh rate (117 FPS at 120 Hz, at most 120). Turn on G-Sync/FreeSync in '
    'the NVIDIA/AMD control panel; fullscreen works best.',
    'New shaders while playing',
    'In the background: a place seen for the first time does not stutter while its shaders are '
    'compiled; things drawn with them appear a moment later instead. Each is compiled once and kept.',
    'Not available on AMD Radeon: there it adds lag and costs FPS.',
]
TEXTS = {
    'ar': [
        'زمن استجابة منخفض (Reflex / Anti-Lag 2)',
        'تأخير أقل في الإدخال: تبدأ اللعبة كل إطار في الوقت المناسب بدلاً من تكديس الإطارات للشاشة. NVIDIA Reflex '
        'على GeForce وAMD Anti-Lag 2 على Radeon (برامج تشغيل حديثة)؛ وتتجاهله البطاقات الأخرى. Boost يبقي ترددات '
        'البطاقة مرتفعة.',
        'إرسال الإطارات إلى البطاقة على أجزاء',
        'تبدأ بطاقة الرسوميات رسم الإطار بينما يُجهَّز باقيه، فينتهي قبل نحو 2 مللي ثانية. يستهلك المعالج أكثر قليلاً. '
        'جديد: أوقفه إذا رأيت وميضاً أو أجساماً مفقودة.',
        'معدل التحديث المتغير (VRR)',
        'لشاشات G-Sync أو FreeSync: تتبع الشاشة إطارات اللعبة دون تمزق ودون تأخير V-Sync. يستخدم V-Sync (يُتجاهل '
        'وضع العرض) ويبقي معدل الإطارات أقل قليلاً من معدل التحديث (117 إطاراً عند 120 هرتز، 120 كحد أقصى). فعّل '
        'G-Sync/FreeSync في لوحة تحكم NVIDIA/AMD؛ ويعمل ملء الشاشة بشكل أفضل.',
        'المظللات الجديدة أثناء اللعب',
        'في الخلفية: لا يتقطع المكان الذي يُرى لأول مرة أثناء تجميع مظللاته؛ بل تظهر الأشياء المرسومة بها بعد لحظة. '
        'يُجمَّع كل منها مرة واحدة ويُحفظ.',
        'غير متاح على AMD Radeon: هناك يزيد التأخير ويخفض الإطارات.',
    ],
    'es': [
        'Baja latencia (Reflex / Anti-Lag 2)',
        'Menos retraso de entrada: el juego empieza cada fotograma justo a tiempo en lugar de acumular fotogramas '
        'para la pantalla. NVIDIA Reflex en GeForce, AMD Anti-Lag 2 en Radeon (controladores actuales); las demás '
        'GPU lo ignoran. Boost mantiene altas las frecuencias de la GPU.',
        'Enviar los fotogramas a la GPU por partes',
        'La tarjeta gráfica empieza a dibujar un fotograma mientras el resto aún se prepara, así termina unos 2 ms '
        'antes. Usa un poco más de CPU. Nuevo: desactívalo si ves parpadeos u objetos que faltan.',
        'Frecuencia de actualización variable (VRR)',
        'Para pantallas G-Sync o FreeSync: la pantalla sigue los fotogramas del juego, sin tearing y sin el retraso '
        'de V-Sync. Usa V-Sync (se ignora Presentación) y mantiene los FPS un poco por debajo de la frecuencia '
        '(117 FPS a 120 Hz, 120 como máximo). Activa G-Sync/FreeSync en el panel de NVIDIA/AMD; funciona mejor en '
        'pantalla completa.',
        'Shaders nuevos durante la partida',
        'En segundo plano: un lugar visto por primera vez no da tirones mientras se compilan sus shaders; lo que se '
        'dibuja con ellos aparece un momento después. Cada uno se compila una vez y se guarda.',
        'No disponible en AMD Radeon: allí añade retraso y cuesta FPS.',
    ],
    'pt': [
        'Baixa latência (Reflex / Anti-Lag 2)',
        'Menos atraso de entrada: o jogo começa cada quadro na hora certa em vez de enfileirar quadros para a tela. '
        'NVIDIA Reflex em GeForce, AMD Anti-Lag 2 em Radeon (drivers atuais); outras GPUs ignoram. Boost mantém os '
        'clocks da GPU altos.',
        'Enviar os quadros à GPU em partes',
        'A placa de vídeo começa a desenhar um quadro enquanto o resto ainda está sendo preparado, então ele fica '
        'pronto cerca de 2 ms antes. Usa um pouco mais de CPU. Novo: desligue se vir piscadas ou objetos faltando.',
        'Taxa de atualização variável (VRR)',
        'Para telas G-Sync ou FreeSync: a tela acompanha os quadros do jogo, sem tearing e sem o atraso do V-Sync. '
        'Usa V-Sync (Apresentação é ignorada) e mantém o FPS um pouco abaixo da taxa de atualização (117 FPS a '
        '120 Hz, no máximo 120). Ative G-Sync/FreeSync no painel da NVIDIA/AMD; tela cheia funciona melhor.',
        'Shaders novos durante o jogo',
        'Em segundo plano: um lugar visto pela primeira vez não engasga enquanto seus shaders são compilados; o que '
        'é desenhado com eles aparece um instante depois. Cada um é compilado uma vez e guardado.',
        'Indisponível em AMD Radeon: lá aumenta o atraso e custa FPS.',
    ],
    'fr': [
        'Faible latence (Reflex / Anti-Lag 2)',
        'Moins de latence d’entrée : le jeu commence chaque image juste à temps au lieu d’accumuler des images pour '
        'l’écran. NVIDIA Reflex sur GeForce, AMD Anti-Lag 2 sur Radeon (pilotes récents) ; les autres GPU '
        'l’ignorent. Boost maintient les fréquences du GPU élevées.',
        'Envoyer les images au GPU par morceaux',
        'La carte graphique commence à dessiner une image pendant que le reste est encore préparé : elle est prête '
        'environ 2 ms plus tôt. Utilise un peu plus le CPU. Nouveau : désactivez-le si vous voyez des scintillements '
        'ou des objets manquants.',
        'Fréquence de rafraîchissement variable (VRR)',
        'Pour un écran G-Sync ou FreeSync : l’écran suit les images du jeu, sans déchirure et sans le retard du '
        'V-Sync. Utilise le V-Sync (Présentation est ignoré) et garde la fréquence d’images un peu sous celle de '
        'l’écran (117 FPS à 120 Hz, 120 au maximum). Activez G-Sync/FreeSync dans le panneau NVIDIA/AMD ; le plein '
        'écran fonctionne le mieux.',
        'Nouveaux shaders en jeu',
        'En arrière-plan : un lieu vu pour la première fois ne saccade pas pendant la compilation de ses shaders ; '
        'ce qu’ils dessinent apparaît un instant plus tard. Chacun est compilé une fois et conservé.',
        'Indisponible sur AMD Radeon : il y ajoute de la latence et coûte des FPS.',
    ],
    'de': [
        'Niedrige Latenz (Reflex / Anti-Lag 2)',
        'Weniger Eingabeverzögerung: Das Spiel beginnt jedes Bild genau rechtzeitig, statt Bilder für den Bildschirm '
        'anzustauen. NVIDIA Reflex auf GeForce, AMD Anti-Lag 2 auf Radeon (aktuelle Treiber); andere GPUs ignorieren '
        'es. Boost hält die GPU-Takte hoch.',
        'Bilder in Teilen an die GPU senden',
        'Die Grafikkarte beginnt ein Bild zu zeichnen, während der Rest noch vorbereitet wird, und ist so etwa 2 ms '
        'früher fertig. Braucht etwas mehr CPU. Neu: Schalte es aus, wenn du Flackern oder fehlende Objekte siehst.',
        'Variable Bildwiederholrate (VRR)',
        'Für G-Sync- oder FreeSync-Bildschirme: Der Bildschirm folgt den Bildern des Spiels, ohne Tearing und ohne '
        'die Verzögerung von V-Sync. Nutzt V-Sync (Darstellung wird ignoriert) und hält die Bildrate knapp unter der '
        'Bildwiederholrate (117 FPS bei 120 Hz, höchstens 120). G-Sync/FreeSync im NVIDIA-/AMD-Treibermenü '
        'einschalten; Vollbild funktioniert am besten.',
        'Neue Shader beim Spielen',
        'Im Hintergrund: Ein zum ersten Mal gesehener Ort ruckelt nicht, während seine Shader kompiliert werden; was '
        'damit gezeichnet wird, erscheint einen Moment später. Jeder wird einmal kompiliert und behalten.',
        'Auf AMD Radeon nicht verfügbar: Dort erhöht es die Latenz und kostet FPS.',
    ],
    'it': [
        'Bassa latenza (Reflex / Anti-Lag 2)',
        'Meno ritardo di input: il gioco inizia ogni fotogramma appena in tempo invece di accodare fotogrammi per lo '
        'schermo. NVIDIA Reflex su GeForce, AMD Anti-Lag 2 su Radeon (driver recenti); le altre GPU lo ignorano. '
        'Boost mantiene alte le frequenze della GPU.',
        'Invia i fotogrammi alla GPU a parti',
        'La scheda video inizia a disegnare un fotogramma mentre il resto è ancora in preparazione, così finisce '
        'circa 2 ms prima. Usa un po’ più di CPU. Novità: disattivalo se vedi sfarfallii o oggetti mancanti.',
        'Frequenza di aggiornamento variabile (VRR)',
        'Per schermi G-Sync o FreeSync: lo schermo segue i fotogrammi del gioco, senza tearing e senza il ritardo del '
        'V-Sync. Usa il V-Sync (Presentazione viene ignorata) e tiene il frame rate poco sotto la frequenza (117 FPS '
        'a 120 Hz, al massimo 120). Attiva G-Sync/FreeSync nel pannello NVIDIA/AMD; lo schermo intero funziona '
        'meglio.',
        'Nuovi shader durante il gioco',
        'In background: un luogo visto per la prima volta non scatta mentre i suoi shader vengono compilati; ciò '
        'che disegnano appare un attimo dopo. Ognuno viene compilato una volta e conservato.',
        'Non disponibile su AMD Radeon: lì aggiunge ritardo e costa FPS.',
    ],
    'pl': [
        'Niskie opóźnienie (Reflex / Anti-Lag 2)',
        'Mniejsze opóźnienie sterowania: gra zaczyna każdą klatkę w samą porę zamiast kolejkować klatki dla ekranu. '
        'NVIDIA Reflex na GeForce, AMD Anti-Lag 2 na Radeon (aktualne sterowniki); inne GPU to ignorują. Boost '
        'utrzymuje wysokie taktowanie GPU.',
        'Wysyłaj klatki do GPU w częściach',
        'Karta graficzna zaczyna rysować klatkę, gdy reszta jest jeszcze przygotowywana, więc kończy ją około 2 ms '
        'wcześniej. Trochę bardziej obciąża CPU. Nowość: wyłącz, jeśli widzisz migotanie lub brakujące obiekty.',
        'Zmienna częstotliwość odświeżania (VRR)',
        'Dla ekranów G-Sync lub FreeSync: ekran podąża za klatkami gry, bez rozrywania obrazu i bez opóźnienia '
        'V-Sync. Używa V-Sync (Tryb wyświetlania jest pomijany) i utrzymuje liczbę klatek nieco poniżej odświeżania '
        '(117 FPS przy 120 Hz, najwyżej 120). Włącz G-Sync/FreeSync w panelu NVIDIA/AMD; najlepiej działa pełny '
        'ekran.',
        'Nowe shadery podczas gry',
        'W tle: miejsce widziane po raz pierwszy nie przycina podczas kompilacji shaderów; to, co nimi rysowane, '
        'pojawia się chwilę później. Każdy jest kompilowany raz i zachowywany.',
        'Niedostępne na AMD Radeon: tam zwiększa opóźnienie i obniża FPS.',
    ],
    'tr': [
        'Düşük gecikme (Reflex / Anti-Lag 2)',
        'Daha az giriş gecikmesi: oyun, ekran için kare biriktirmek yerine her kareye tam zamanında başlar. '
        'GeForce’ta NVIDIA Reflex, Radeon’da AMD Anti-Lag 2 (güncel sürücüler); diğer GPU’lar yok sayar. Boost GPU '
        'saat hızlarını yüksek tutar.',
        'Kareleri GPU’ya parça parça gönder',
        'Ekran kartı, karenin geri kalanı hâlâ hazırlanırken çizmeye başlar; böylece kare yaklaşık 2 ms erken biter. '
        'Biraz daha fazla CPU kullanır. Yeni: titreme veya eksik nesneler görürseniz kapatın.',
        'Değişken yenileme hızı (VRR)',
        'G-Sync veya FreeSync ekranlar için: ekran oyunun karelerini takip eder; yırtılma ve V-Sync gecikmesi olmaz. '
        'V-Sync kullanır (Sunum yok sayılır) ve kare hızını yenileme hızının biraz altında tutar (120 Hz’de 117 FPS, '
        'en fazla 120). NVIDIA/AMD kontrol panelinden G-Sync/FreeSync’i açın; en iyi tam ekranda çalışır.',
        'Oyun sırasında yeni gölgelendiriciler',
        'Arka planda: ilk kez görülen bir yer, gölgelendiricileri derlenirken takılmaz; onlarla çizilenler bir an '
        'sonra görünür. Her biri bir kez derlenir ve saklanır.',
        'AMD Radeon’da kullanılamaz: orada gecikmeyi artırır ve FPS düşürür.',
    ],
    'zh': [
        '低延迟（Reflex / Anti-Lag 2）',
        '降低输入延迟：游戏在恰当的时机开始每一帧，而不是为显示器排队缓存帧。GeForce 上为 NVIDIA Reflex，Radeon 上为 '
        'AMD Anti-Lag 2（需较新驱动）；其他显卡会忽略此项。Boost 让显卡保持高频率。',
        '分段将帧发送给显卡',
        '显卡在帧的其余部分仍在准备时就开始绘制，因此约提前 2 毫秒完成。会略微增加 CPU 占用。新功能：如出现闪烁或物体缺失，请关闭。',
        '可变刷新率（VRR）',
        '适用于 G-Sync 或 FreeSync 显示器：显示器跟随游戏的帧，没有画面撕裂，也没有垂直同步的延迟。使用垂直同步（忽略“显示模式”），'
        '并让帧率略低于刷新率（120 Hz 时为 117 FPS，最高 120）。请在 NVIDIA/AMD 控制面板中开启 G-Sync/FreeSync；全屏效果最佳。',
        '游戏中的新着色器',
        '在后台进行：首次看到的地方在编译着色器时不会卡顿；用这些着色器绘制的物体会稍晚出现。每个着色器只编译一次并保存。',
        '在 AMD Radeon 上不可用：会增加延迟并降低帧率。',
    ],
    'ja': [
        '低遅延（Reflex / Anti-Lag 2）',
        '入力遅延を減らします：フレームをディスプレイ用にため込まず、ちょうどよいタイミングで各フレームを開始します。GeForce では '
        'NVIDIA Reflex、Radeon では AMD Anti-Lag 2（最新ドライバー）。その他の GPU では無視されます。Boost は GPU '
        'のクロックを高く保ちます。',
        'フレームを分割して GPU に送る',
        'フレームの残りを準備している間にグラフィックカードが描画を始めるため、約 2 ms 早く完成します。CPU '
        'をやや多く使います。新機能：ちらつきや表示されない物体がある場合はオフにしてください。',
        '可変リフレッシュレート（VRR）',
        'G-Sync / FreeSync 対応ディスプレイ向け：ディスプレイがゲームのフレームに合わせて表示し、ティアリングも V-Sync '
        'の遅延もありません。V-Sync を使い（表示モードは無視されます）、フレームレートをリフレッシュレートより少し低く保ちます'
        '（120 Hz で 117 FPS、最大 120）。NVIDIA/AMD のコントロールパネルで G-Sync/FreeSync をオンにしてください。'
        'フルスクリーンが最適です。',
        'プレイ中の新しいシェーダー',
        'バックグラウンドで処理：初めて訪れた場所でシェーダーのコンパイル中にカクつきません。その代わり、それで描かれる物は'
        '少し遅れて表示されます。各シェーダーは一度だけコンパイルされ保存されます。',
        'AMD Radeon では使用できません：遅延が増え、FPS が下がります。',
    ],
    'ko': [
        '낮은 지연 시간 (Reflex / Anti-Lag 2)',
        '입력 지연 감소: 디스플레이용 프레임을 쌓아 두지 않고 각 프레임을 딱 맞춰 시작합니다. GeForce에서는 NVIDIA '
        'Reflex, Radeon에서는 AMD Anti-Lag 2(최신 드라이버). 다른 GPU에서는 무시됩니다. Boost는 GPU 클럭을 높게 '
        '유지합니다.',
        '프레임을 나눠서 GPU로 보내기',
        '프레임의 나머지가 준비되는 동안 그래픽 카드가 그리기를 시작하므로 약 2ms 빨리 완성됩니다. CPU를 조금 더 '
        '사용합니다. 새 기능: 깜빡임이나 사라진 물체가 보이면 끄세요.',
        '가변 주사율 (VRR)',
        'G-Sync 또는 FreeSync 디스플레이용: 디스플레이가 게임의 프레임을 따라가므로 화면 찢어짐도 V-Sync 지연도 '
        '없습니다. V-Sync를 사용하며(표시 모드는 무시됨) 프레임 레이트를 주사율보다 조금 낮게 유지합니다(120Hz에서 '
        '117 FPS, 최대 120). NVIDIA/AMD 제어판에서 G-Sync/FreeSync를 켜세요. 전체 화면에서 가장 잘 작동합니다.',
        '플레이 중 새 셰이더',
        '백그라운드에서: 처음 보는 장소에서 셰이더를 컴파일하는 동안 끊기지 않으며, 그 셰이더로 그리는 물체는 잠시 '
        '후에 나타납니다. 각 셰이더는 한 번 컴파일되어 저장됩니다.',
        'AMD Radeon에서는 사용할 수 없습니다: 지연이 늘고 FPS가 떨어집니다.',
    ],
}

for _language, _texts in TEXTS.items():
    assert len(_texts) == len(KEYS), (_language, len(_texts), len(KEYS))
    bbport_lang.EXTRA_TRANSLATIONS.setdefault(_language, {}).update(zip(KEYS, _texts))
