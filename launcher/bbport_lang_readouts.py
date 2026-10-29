# SPDX-License-Identifier: GPL-2.0-or-later
"""The FPS counter's lines (All graphics > FPS counter: READOUTS in bbport_launcher_win), in every language
besides English. Keyed by the English text like bbport_lang; merged into its tables on import."""
import bbport_lang

KEYS = [
    'FPS counter',
    'FPS',
    'Frame time',
    'Render latency',
    'Video memory (VRAM)',
    'GPU usage',
    'CPU usage',
    'RAM usage',
    'Frame time graph',
    'One box per line of the counter in the top right; also in the in-game menu. Render '
    'latency is the time from the game starting a frame to it being shown, which Low latency '
    'lowers; frame time is the time between two frames.',
    'Value colours',
    'Colour-coded (green, orange, red)',
    'Plain (grey)',
    'Colour-coded shows at a glance how each value stands; plain keeps the counter quiet.',
]
TEXTS = {
    'ru': ['Счётчик FPS', 'FPS', 'Время кадра', 'Задержка отрисовки', 'Видеопамять (VRAM)', 'Загрузка GPU',
           'Загрузка CPU', 'Использование RAM', 'График времени кадра',
           'По одной галочке на строку счётчика в правом верхнем углу; также во внутриигровом меню. Задержка '
           'отрисовки — время от начала кадра в игре до его показа, её снижает «Низкая задержка»; время кадра — '
           'время между двумя кадрами.',
           'Цвета значений',
           'Цветные (зелёный, оранжевый, красный)',
           'Обычные (серый)',
           'Цветные сразу показывают, в каком состоянии каждое значение; обычные делают счётчик неброским.'],
    'ar': ['عداد الإطارات', 'FPS', 'زمن الإطار', 'زمن استجابة العرض', 'ذاكرة الفيديو (VRAM)', 'استخدام GPU',
           'استخدام CPU', 'استخدام RAM', 'رسم زمن الإطار',
           'مربع لكل سطر في العداد أعلى اليمين؛ وكذلك في قائمة اللعبة. زمن استجابة العرض هو الوقت من بدء اللعبة '
           'للإطار حتى ظهوره، ويخفضه خيار زمن الاستجابة المنخفض؛ وزمن الإطار هو الوقت بين إطارين.',
           'ألوان القيم',
           'ملونة (أخضر، برتقالي، أحمر)',
           'عادية (رمادي)',
           'الألوان تُظهر حالة كل قيمة بنظرة واحدة؛ والعادية تُبقي العداد هادئاً.'],
    'es': ['Contador de FPS', 'FPS', 'Tiempo de fotograma', 'Latencia de renderizado', 'Memoria de vídeo (VRAM)',
           'Uso de GPU', 'Uso de CPU', 'Uso de RAM', 'Gráfico del tiempo de fotograma',
           'Una casilla por línea del contador de arriba a la derecha; también en el menú del juego. La latencia de '
           'renderizado es el tiempo desde que el juego empieza un fotograma hasta que se muestra, y Baja latencia la '
           'reduce; el tiempo de fotograma es el tiempo entre dos fotogramas.',
           'Colores de los valores',
           'Por colores (verde, naranja, rojo)',
           'Sencillo (gris)',
           'Por colores muestra de un vistazo cómo está cada valor; sencillo deja el contador discreto.'],
    'pt': ['Contador de FPS', 'FPS', 'Tempo de quadro', 'Latência de renderização', 'Memória de vídeo (VRAM)',
           'Uso da GPU', 'Uso da CPU', 'Uso de RAM', 'Gráfico do tempo de quadro',
           'Uma caixa por linha do contador no canto superior direito; também no menu do jogo. A latência de '
           'renderização é o tempo entre o jogo começar um quadro e ele aparecer, e a Baixa latência a reduz; o '
           'tempo de quadro é o tempo entre dois quadros.',
           'Cores dos valores',
           'Por cores (verde, laranja, vermelho)',
           'Simples (cinza)',
           'Por cores mostra num relance como está cada valor; simples deixa o contador discreto.'],
    'fr': ['Compteur de FPS', 'FPS', 'Temps d’image', 'Latence de rendu', 'Mémoire vidéo (VRAM)',
           'Utilisation du GPU', 'Utilisation du CPU', 'Utilisation de la RAM', 'Graphique du temps d’image',
           'Une case par ligne du compteur en haut à droite ; aussi dans le menu du jeu. La latence de rendu est le '
           'temps entre le début d’une image par le jeu et son affichage, que Faible latence réduit ; le temps '
           'd’image est le temps entre deux images.',
           'Couleurs des valeurs',
           'Codées par couleur (vert, orange, rouge)',
           'Simples (gris)',
           'Les couleurs montrent d’un coup d’œil l’état de chaque valeur ; simples, le compteur reste discret.'],
    'de': ['FPS-Anzeige', 'FPS', 'Frametime', 'Render-Latenz', 'Videospeicher (VRAM)', 'GPU-Auslastung',
           'CPU-Auslastung', 'RAM-Auslastung', 'Frametime-Graph',
           'Ein Kästchen pro Zeile der Anzeige oben rechts; auch im Spielmenü. Die Render-Latenz ist die Zeit vom '
           'Beginn eines Bildes im Spiel bis zu seiner Anzeige, Niedrige Latenz senkt sie; die Frametime ist die '
           'Zeit zwischen zwei Bildern.',
           'Farben der Werte',
           'Farbcodiert (grün, orange, rot)',
           'Schlicht (grau)',
           'Farbcodiert zeigt auf einen Blick, wie jeder Wert steht; schlicht hält die Anzeige unauffällig.'],
    'it': ['Contatore FPS', 'FPS', 'Tempo per fotogramma', 'Latenza di rendering', 'Memoria video (VRAM)',
           'Uso della GPU', 'Uso della CPU', 'Uso della RAM', 'Grafico del tempo per fotogramma',
           'Una casella per riga del contatore in alto a destra; anche nel menu di gioco. La latenza di rendering è '
           'il tempo da quando il gioco inizia un fotogramma a quando viene mostrato, e Bassa latenza la riduce; il '
           'tempo per fotogramma è il tempo tra due fotogrammi.',
           'Colori dei valori',
           'Per colore (verde, arancione, rosso)',
           'Semplici (grigio)',
           'Per colore mostra a colpo d’occhio come sta ogni valore; semplici rende il contatore discreto.'],
    'pl': ['Licznik FPS', 'FPS', 'Czas klatki', 'Opóźnienie renderowania', 'Pamięć wideo (VRAM)', 'Użycie GPU',
           'Użycie CPU', 'Użycie RAM', 'Wykres czasu klatki',
           'Jedno pole na każdy wiersz licznika w prawym górnym rogu; także w menu gry. Opóźnienie renderowania to '
           'czas od rozpoczęcia klatki przez grę do jej wyświetlenia, obniża je Niskie opóźnienie; czas klatki to '
           'czas między dwiema klatkami.',
           'Kolory wartości',
           'Kolorowe (zielony, pomarańczowy, czerwony)',
           'Zwykłe (szare)',
           'Kolory od razu pokazują stan każdej wartości; zwykłe sprawiają, że licznik jest dyskretny.'],
    'tr': ['FPS sayacı', 'FPS', 'Kare süresi', 'İşleme gecikmesi', 'Video belleği (VRAM)', 'GPU kullanımı',
           'CPU kullanımı', 'RAM kullanımı', 'Kare süresi grafiği',
           'Sağ üstteki sayacın her satırı için bir kutu; oyun içi menüde de var. İşleme gecikmesi, oyunun bir kareye '
           'başlamasından karenin gösterilmesine kadar geçen süredir ve Düşük gecikme bunu azaltır; kare süresi iki '
           'kare arasındaki süredir.',
           'Değer renkleri',
           'Renk kodlu (yeşil, turuncu, kırmızı)',
           'Sade (gri)',
           'Renk kodlu, her değerin durumunu bir bakışta gösterir; sade, sayacı göze batmayan tutar.'],
    'zh': ['FPS 计数器', 'FPS', '帧时间', '渲染延迟', '显存（VRAM）', 'GPU 占用', 'CPU 占用', '内存占用', '帧时间曲线',
           '右上角计数器每一行对应一个选项；游戏内菜单中也可设置。渲染延迟是从游戏开始一帧到它显示出来的时间，'
           '“低延迟”可以降低它；帧时间是两帧之间的时间。',
           '数值颜色',
           '按状态着色（绿、橙、红）',
           '普通（灰色）',
           '着色可一眼看出各项数值的状态；普通则让计数器更低调。'],
    'ja': ['FPS カウンター', 'FPS', 'フレーム時間', 'レンダーレイテンシー', 'ビデオメモリ（VRAM）', 'GPU 使用率',
           'CPU 使用率', 'RAM 使用量', 'フレーム時間グラフ',
           '右上のカウンターの各行に 1 つのチェックボックス。ゲーム内メニューでも設定できます。レンダーレイテンシーは、'
           'ゲームがフレームを始めてから表示されるまでの時間で、低遅延で短くなります。フレーム時間は 2 つのフレームの間の時間です。',
           '数値の色',
           '色分け（緑・オレンジ・赤）',
           'シンプル（グレー）',
           '色分けでは各数値の状態がひと目で分かります。シンプルではカウンターが目立ちません。'],
    'ko': ['FPS 카운터', 'FPS', '프레임 시간', '렌더링 지연 시간', '비디오 메모리 (VRAM)', 'GPU 사용률',
           'CPU 사용률', 'RAM 사용량', '프레임 시간 그래프',
           '오른쪽 위 카운터의 줄마다 확인란이 하나씩 있으며, 게임 내 메뉴에서도 설정할 수 있습니다. 렌더링 지연 시간은 '
           '게임이 프레임을 시작해서 화면에 표시될 때까지의 시간이며 낮은 지연 시간 옵션으로 줄어듭니다. 프레임 시간은 '
           '두 프레임 사이의 시간입니다.',
           '수치 색상',
           '색상 구분 (초록, 주황, 빨강)',
           '기본 (회색)',
           '색상 구분은 각 수치의 상태를 한눈에 보여 주고, 기본은 카운터를 눈에 띄지 않게 합니다.'],
}

for _language, _texts in TEXTS.items():
    assert len(_texts) == len(KEYS), (_language, len(_texts), len(KEYS))
    bbport_lang.EXTRA_TRANSLATIONS.setdefault(_language, {}).update(zip(KEYS, _texts))
