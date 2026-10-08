// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_text.h"

#include <cstring>

#include "bbport_settings.h"

namespace BbText {

namespace {

// One entry per overlay string. The key is the English text (also the base language and the
// fallback); the rest are Russian, German, French, Spanish and Italian in Language order.
struct Entry {
    const char* key;
    const char* text[LanguageCount - 1];
};

constexpr Entry kEntries[] = {
    {"Bloodborne — settings  (Insert / L3+R3)",
     "Bloodborne — настройки  (Insert / L3+R3)",
     "Bloodborne — Einstellungen  (Insert / L3+R3)",
     "Bloodborne — paramètres  (Insert / L3+R3)",
     "Bloodborne — ajustes  (Insert / L3+R3)",
     "Bloodborne — impostazioni  (Insert / L3+R3)"},
    {"%.0f FPS  (%.1f ms)",
     "%.0f FPS  (%.1f мс)",
     "%.0f FPS  (%.1f ms)",
     "%.0f FPS  (%.1f ms)",
     "%.0f FPS  (%.1f ms)",
     "%.0f FPS  (%.1f ms)"},
    {"Temporal upscaler",
     "Временной апскейлер",
     "Temporaler Upscaler",
     "Mise à l'échelle temporelle",
     "Escalado temporal",
     "Upscaling temporale"},
    {"Upscaler",
     "Апскейлер",
     "Upscaler",
     "Upscaler",
     "Escalador",
     "Upscaler"},
    {"Off",
     "Выкл",
     "Aus",
     "Désactivé",
     "Desactivado",
     "Disattivato"},
    {"TAA (native anti-aliasing)",
     "TAA (нативное сглаживание)",
     "TAA (natives Anti-Aliasing)",
     "TAA (anti-aliasing natif)",
     "TAA (antialiasing nativo)",
     "TAA (anti-aliasing nativo)"},
    {"DLSS (NVIDIA RTX)",
     "DLSS (NVIDIA RTX)",
     "DLSS (NVIDIA RTX)",
     "DLSS (NVIDIA RTX)",
     "DLSS (NVIDIA RTX)",
     "DLSS (NVIDIA RTX)"},
    {"— not supported by this GPU",
     "— не поддерживается видеокартой",
     "— von dieser GPU nicht unterstützt",
     "— non pris en charge par ce GPU",
     "— no compatible con esta GPU",
     "— non supportato da questa GPU"},
    {"— work in progress",
     "— в работе",
     "— in Arbeit",
     "— en préparation",
     "— en preparación",
     "— in lavorazione"},
    {"FSR 4 is unavailable: %s",
     "FSR 4 недоступен: %s",
     "FSR 4 ist nicht verfügbar: %s",
     "FSR 4 indisponible : %s",
     "FSR 4 no está disponible: %s",
     "FSR 4 non disponibile: %s"},
    {"The mode selected above is active. FSR 4 can be selected again.",
     "Активен режим, выбранный выше. FSR 4 можно выбрать снова.",
     "Der oben gewählte Modus ist aktiv. FSR 4 kann erneut gewählt werden.",
     "Le mode sélectionné ci-dessus est actif. FSR 4 peut être resélectionné.",
     "El modo seleccionado arriba está activo. FSR 4 se puede volver a seleccionar.",
     "La modalità selezionata sopra è attiva. FSR 4 può essere riselezionato."},
    {"FSR 4.1.1 in INT8 mode: AMD's 4.1.1 DLL model replayed in Vulkan (bit-exact with the DLL). "
     "One model for Native..Performance and one for Ultra Performance. Assets: "
     "tools/fsr4cap/build_assets.sh (needs the DLLs and Proton).",
     "FSR 4.1.1 в режиме INT8: модель из DLL AMD 4.1.1, воспроизведённая в Vulkan (результат "
     "совпадает с DLL). Одна модель для Native..Performance и отдельная для Ultra Performance. "
     "Ассеты: tools/fsr4cap/build_assets.sh (нужны DLL и Proton).",
     "FSR 4.1.1 im INT8-Modus: das Modell aus AMDs 4.1.1-DLL, in Vulkan nachgebildet (bitgenau "
     "zur DLL). Ein Modell für Native..Performance und eines für Ultra Performance. Assets: "
     "tools/fsr4cap/build_assets.sh (braucht die DLLs und Proton).",
     "FSR 4.1.1 en mode INT8 : le modèle de la DLL AMD 4.1.1 rejoué en Vulkan (identique au bit "
     "près à la DLL). Un modèle pour Native..Performance et un pour Ultra Performance. Assets : "
     "tools/fsr4cap/build_assets.sh (nécessite les DLL et Proton).",
     "FSR 4.1.1 en modo INT8: el modelo de la DLL de AMD 4.1.1 reproducido en Vulkan (idéntico "
     "bit a bit a la DLL). Un modelo para Native..Performance y otro para Ultra Performance. "
     "Assets: tools/fsr4cap/build_assets.sh (necesita las DLL y Proton).",
     "FSR 4.1.1 in modalità INT8: il modello della DLL AMD 4.1.1 riprodotto in Vulkan (identico "
     "bit per bit alla DLL). Un modello per Native..Performance e uno per Ultra Performance. "
     "Asset: tools/fsr4cap/build_assets.sh (servono le DLL e Proton)."},
    {"FSR 4 in INT8 mode (the v07 model from AMD's FidelityFX SDK sources). Higher quality than "
     "FSR 3.1, but a heavier pass. Changing the preset rebuilds the model (a short pause). "
     "Assets: tools/fetch_fsr4_assets.sh.",
     "FSR 4 в режиме INT8 (модель v07 из исходников AMD FidelityFX SDK). Качество выше, чем у "
     "FSR 3.1, но проход тяжелее. Смена пресета пересобирает модель (короткая пауза). Ассеты: "
     "tools/fetch_fsr4_assets.sh.",
     "FSR 4 im INT8-Modus (Modell v07 aus den FidelityFX-SDK-Quellen von AMD). Höhere Qualität "
     "als FSR 3.1, aber ein schwererer Pass. Ein Presetwechsel baut das Modell neu (kurze Pause). "
     "Assets: tools/fetch_fsr4_assets.sh.",
     "FSR 4 en mode INT8 (modèle v07 issu des sources du FidelityFX SDK d'AMD). Qualité "
     "supérieure à FSR 3.1, mais passe plus lourde. Changer le preset reconstruit le modèle "
     "(courte pause). Assets : tools/fetch_fsr4_assets.sh.",
     "FSR 4 en modo INT8 (modelo v07 de las fuentes del FidelityFX SDK de AMD). Más calidad que "
     "FSR 3.1, pero una pasada más pesada. Cambiar el preset reconstruye el modelo (pausa "
     "breve). Assets: tools/fetch_fsr4_assets.sh.",
     "FSR 4 in modalità INT8 (modello v07 dai sorgenti dell'FidelityFX SDK di AMD). Qualità "
     "superiore a FSR 3.1, ma passaggio più pesante. Cambiare il preset ricostruisce il modello "
     "(breve pausa). Asset: tools/fetch_fsr4_assets.sh."},
    {"FSR 4: auto exposure",
     "FSR 4: авто-экспозиция",
     "FSR 4: Auto-Belichtung",
     "FSR 4 : exposition auto",
     "FSR 4: exposición automática",
     "FSR 4: esposizione automatica"},
    {"FSR 4: inverted jitter sign",
     "FSR 4: обратный знак jitter",
     "FSR 4: umgekehrtes Jitter-Vorzeichen",
     "FSR 4 : signe de jitter inversé",
     "FSR 4: signo de jitter invertido",
     "FSR 4: segno del jitter invertito"},
    {"Ghosting check: the FSR 4 network normalizes colour by exposure and decides from it when "
     "to drop earlier frames. Changed immediately, no restart.",
     "Проверка при гостинге: сеть FSR 4 нормирует цвет по экспозиции и по ней решает, когда "
     "отбросить прошлые кадры. Меняются сразу, без перезапуска.",
     "Ghosting-Test: das FSR-4-Netz normalisiert die Farbe über die Belichtung und entscheidet "
     "daran, wann frühere Frames verworfen werden. Änderung sofort, ohne Neustart.",
     "Contrôle du ghosting : le réseau FSR 4 normalise la couleur par l'exposition et décide "
     "d'après elle quand écarter les images précédentes. Appliqué immédiatement, sans redémarrage.",
     "Comprobación de ghosting: la red de FSR 4 normaliza el color por la exposición y decide "
     "con ella cuándo descartar fotogramas anteriores. Se aplica al momento, sin reinicio.",
     "Controllo del ghosting: la rete FSR 4 normalizza il colore in base all'esposizione e "
     "decide da essa quando scartare i fotogrammi precedenti. Applicato subito, senza riavvio."},
    {"Preset",
     "Пресет",
     "Preset",
     "Preset",
     "Preset",
     "Preset"},
    {"%s (x%.1f)",
     "%s (x%.1f)",
     "%s (x%.1f)",
     "%s (x%.1f)",
     "%s (x%.1f)",
     "%s (x%.1f)"},
    {"%s (x%.1f, render %dx%d)",
     "%s (x%.1f, рендер %dx%d)",
     "%s (x%.1f, Render %dx%d)",
     "%s (x%.1f, rendu %dx%d)",
     "%s (x%.1f, render %dx%d)",
     "%s (x%.1f, render %dx%d)"},
    {"TAA anti-aliases the scene at the output resolution, without an FSR model or upscaling. "
     "The saved FSR preset is restored when FSR is selected.",
     "TAA сглаживает сцену в разрешении вывода, без модели FSR и апскейлинга. Сохранённый "
     "пресет FSR восстановится при выборе FSR.",
     "TAA glättet die Szene in der Ausgabeauflösung, ohne FSR-Modell oder Upscaling. Das "
     "gespeicherte FSR-Preset wird bei FSR wiederhergestellt.",
     "TAA lisse la scène à la résolution de sortie, sans modèle FSR ni mise à l'échelle. Le "
     "preset FSR enregistré est restauré en choisissant FSR.",
     "TAA suaviza la escena a la resolución de salida, sin modelo FSR ni escalado. El preset de "
     "FSR guardado se restaura al elegir FSR.",
     "TAA leviga la scena alla risoluzione di output, senza modello FSR né upscaling. Il preset "
     "FSR salvato viene ripristinato scegliendo FSR."},
    {"Active scene render: %d x %d",
     "Активный рендер сцены: %d x %d",
     "Aktiver Szenen-Render: %d x %d",
     "Rendu de scène actif : %d x %d",
     "Render de escena activo: %d x %d",
     "Render di scena attivo: %d x %d"},
    {"Preset at start: %s",
     "Пресет при запуске: %s",
     "Preset beim Start: %s",
     "Preset au lancement : %s",
     "Preset al iniciar: %s",
     "Preset all'avvio: %s"},
    {"The preset sets the game's own render size through a patch at start; the upscaler fills "
     "the output and the UI is drawn at the output size. Changing the preset or the output needs "
     "a restart. \"Live resolution changes\" below switches without a restart instead, by keeping "
     "the game at 1080p internally.",
     "Пресет задаёт собственный размер рендера игры патчем при запуске; апскейлер заполняет "
     "вывод, интерфейс рисуется в разрешении вывода. Смена пресета или вывода — после "
     "перезапуска. Пункт «Смена разрешения на лету» ниже переключает без перезапуска, оставляя "
     "игру в 1080p внутри.",
     "Das Preset setzt die eigene Render-Größe des Spiels per Patch beim Start; der Upscaler "
     "füllt die Ausgabe, die UI wird in Ausgabegröße gezeichnet. Preset- oder Ausgabewechsel "
     "brauchen einen Neustart. „Live-Auflösungswechsel“ unten wechselt stattdessen ohne Neustart, "
     "indem das Spiel intern bei 1080p bleibt.",
     "Le preset fixe la taille de rendu du jeu par un patch au lancement ; l'upscaler remplit la "
     "sortie et l'interface est dessinée à la résolution de sortie. Changer le preset ou la "
     "sortie nécessite un redémarrage. « Changement de résolution en direct » ci-dessous permute "
     "sans redémarrage, en gardant le jeu en 1080p en interne.",
     "El preset fija el tamaño de render propio del juego con un parche al iniciar; el escalador "
     "llena la salida y la interfaz se dibuja a la resolución de salida. Cambiar el preset o la "
     "salida requiere reiniciar. «Cambio de resolución en vivo» de abajo cambia sin reinicio, "
     "manteniendo el juego en 1080p internamente.",
     "Il preset imposta la dimensione di render del gioco con una patch all'avvio; l'upscaler "
     "riempie l'output e l'interfaccia è disegnata alla risoluzione di output. Cambiare preset o "
     "output richiede un riavvio. «Cambio risoluzione al volo» qui sotto cambia senza riavvio, "
     "tenendo il gioco a 1080p internamente."},
    {"BB_RENDER_RES fixes the scene size at start. Remove that variable to change the resolution "
     "and presets without restarting the game.",
     "BB_RENDER_RES фиксирует размер сцены при запуске. Уберите эту переменную для смены "
     "разрешения и пресетов без перезапуска игры.",
     "BB_RENDER_RES legt die Szenengröße beim Start fest. Entfernen Sie die Variable, um "
     "Auflösung und Presets ohne Neustart des Spiels zu ändern.",
     "BB_RENDER_RES fixe la taille de scène au lancement. Retirez cette variable pour changer "
     "résolution et presets sans redémarrer le jeu.",
     "BB_RENDER_RES fija el tamaño de escena al iniciar. Quita esa variable para cambiar "
     "resolución y presets sin reiniciar el juego.",
     "BB_RENDER_RES fissa la dimensione della scena all'avvio. Rimuovi quella variabile per "
     "cambiare risoluzione e preset senza riavviare il gioco."},
    {"The scene is scaled at run time: the game keeps rendering at 1080p internally, so presets "
     "and the output switch without a restart — with little gain. With \"Live resolution "
     "changes\" off, a reducing preset patches the game's own render size at start (changing it "
     "then needs a restart).",
     "Сцена масштабируется на лету: игра продолжает рендерить 1080p внутри, поэтому пресеты и "
     "вывод меняются без перезапуска — но выигрыш мал. При выключенной «Смене разрешения на "
     "лету» уменьшающий пресет задаёт размер рендера игры патчем при запуске (смена — с "
     "перезапуском).",
     "Die Szene wird zur Laufzeit skaliert: das Spiel rendert intern weiter mit 1080p, Presets "
     "und Ausgabe wechseln also ohne Neustart — mit wenig Gewinn. Mit ausgeschaltetem "
     "„Live-Auflösungswechsel“ patcht ein reduzierendes Preset die Render-Größe des Spiels beim "
     "Start (Wechsel braucht dann einen Neustart).",
     "La scène est mise à l'échelle en direct : le jeu continue de rendre en 1080p en interne, "
     "donc presets et sortie changent sans redémarrage — avec peu de gain. Avec « Changement de "
     "résolution en direct » désactivé, un preset réducteur patche la taille de rendu du jeu au "
     "lancement (le changer exige alors un redémarrage).",
     "La escena se escala en vivo: el juego sigue renderizando a 1080p internamente, así que "
     "preset y salida cambian sin reinicio — con poca ganancia. Con «Cambio de resolución en "
     "vivo» desactivado, un preset que reduce parchea el tamaño de render del juego al iniciar "
     "(cambiarlo necesita reinicio).",
     "La scena viene scalata al volo: il gioco continua a renderizzare a 1080p internamente, "
     "quindi preset e output cambiano senza riavvio — con poco guadagno. Con «Cambio risoluzione "
     "al volo» disattivato, un preset riducente applica una patch alla dimensione di render del "
     "gioco all'avvio (cambiarla richiede un riavvio)."},
    {"Sharpening (RCAS)",
     "Резкость (RCAS)",
     "Schärfung (RCAS)",
     "Netteté (RCAS)",
     "Nitidez (RCAS)",
     "Nitidezza (RCAS)"},
    {"Sharpness",
     "Сила резкости",
     "Schärfestärke",
     "Intensité de netteté",
     "Intensidad de nitidez",
     "Intensità nitidezza"},
    {"Up to 1 is the upscaler's own sharpening (RCAS). Above 1 an extra RCAS pass is added. "
     "Ctrl+click the slider to enter an exact value.",
     "До 1 — резкость самого апскейлера (RCAS). Выше 1 добавляется ещё один проход RCAS. "
     "Ctrl+клик по ползунку — ввести точное значение.",
     "Bis 1 ist die Schärfung des Upscalers selbst (RCAS). Über 1 kommt ein zusätzlicher "
     "RCAS-Pass hinzu. Strg+Klick auf den Regler für einen genauen Wert.",
     "Jusqu'à 1, c'est la netteté de l'upscaler lui-même (RCAS). Au-delà de 1, une passe RCAS "
     "supplémentaire est ajoutée. Ctrl+clic sur le curseur pour une valeur exacte.",
     "Hasta 1 es la nitidez del propio escalador (RCAS). Por encima de 1 se añade otra pasada "
     "RCAS. Ctrl+clic en el control para un valor exacto.",
     "Fino a 1 è la nitidezza dell'upscaler stesso (RCAS). Oltre 1 si aggiunge un'altra passata "
     "RCAS. Ctrl+clic sullo slider per un valore esatto."},
    {"Sub-pixel jitter",
     "Субпиксельный сдвиг (jitter)",
     "Subpixel-Jitter",
     "Jitter sous-pixel",
     "Jitter subpíxel",
     "Jitter sub-pixel"},
    {"Every frame the scene is shifted by a fraction of a pixel and the upscaler gathers more "
     "detail from several frames. Without it only history smoothing remains.",
     "Каждый кадр сцена сдвигается на долю пикселя, и апскейлер собирает из нескольких кадров "
     "больше деталей. Без него получается только сглаживание по истории.",
     "Jeden Frame wird die Szene um einen Pixelbruchteil verschoben, und der Upscaler sammelt "
     "aus mehreren Frames mehr Detail. Ohne ihn bleibt nur Glättung über die Historie.",
     "Chaque image, la scène est décalée d'une fraction de pixel et l'upscaler collecte plus de "
     "détail sur plusieurs images. Sans lui, il ne reste que le lissage par historique.",
     "Cada fotograma la escena se desplaza una fracción de píxel y el escalador reúne más "
     "detalle de varios fotogramas. Sin él solo queda suavizado por historial.",
     "Ogni fotogramma la scena è spostata di una frazione di pixel e l'upscaler raccoglie più "
     "dettaglio da più fotogrammi. Senza, resta solo la levigatura tramite storico."},
    {"Reactive mask",
     "Маска реактивности",
     "Reaktive Maske",
     "Masque réactif",
     "Máscara reactiva",
     "Maschera reattiva"},
    {"Enable the mask",
     "Включить маску",
     "Maske aktivieren",
     "Activer le masque",
     "Activar la máscara",
     "Attiva la maschera"},
    {"Marks transparent effects (particles, haze) so the upscaler relies less on earlier frames. "
     "Fewer trails behind effects, but shimmering returns under them.",
     "Помечает прозрачные эффекты (частицы, дымку), чтобы апскейлер меньше опирался на прошлые "
     "кадры. Меньше шлейфов за эффектами, но под ними возвращается дрожание.",
     "Markiert transparente Effekte (Partikel, Dunst), damit der Upscaler sich weniger auf "
     "frühere Frames stützt. Weniger Schlieren hinter Effekten, dafür kehrt Flimmern darunter "
     "zurück.",
     "Marque les effets transparents (particules, brume) pour que l'upscaler s'appuie moins sur "
     "les images précédentes. Moins de traînées derrière les effets, mais le scintillement "
     "revient dessous.",
     "Marca los efectos transparentes (partículas, neblina) para que el escalador dependa menos "
     "de fotogramas anteriores. Menos estelas tras los efectos, pero vuelve el parpadeo debajo.",
     "Marca gli effetti trasparenti (particelle, foschia) così l'upscaler si basa meno sui "
     "fotogrammi precedenti. Meno scie dietro gli effetti, ma sotto torna il tremolio."},
    {"Scale",
     "Масштаб",
     "Skalierung",
     "Échelle",
     "Escala",
     "Scala"},
    {"Threshold",
     "Порог",
     "Schwelle",
     "Seuil",
     "Umbral",
     "Soglia"},
    {"Maximum",
     "Максимум",
     "Maximum",
     "Maximum",
     "Máximo",
     "Massimo"},
    {"Show the mask (debug)",
     "Показать маску (отладка)",
     "Maske anzeigen (Debug)",
     "Afficher le masque (debug)",
     "Mostrar la máscara (depuración)",
     "Mostra la maschera (debug)"},
    {"Object motion vectors",
     "Векторы движения персонажей",
     "Objekt-Bewegungsvektoren",
     "Vecteurs de mouvement des objets",
     "Vectores de movimiento de objetos",
     "Vettori di movimento degli oggetti"},
    {"Exact vectors for animated objects: cloth and weapons break up less when moving. A static "
     "scene gets no extra pass. The change applies after restarting the game.",
     "Точные векторы для анимированных объектов: одежда и оружие меньше рассыпаются при "
     "движении. Статичная сцена не получает дополнительный проход. Изменение применяется после "
     "перезапуска игры.",
     "Genaue Vektoren für animierte Objekte: Kleidung und Waffen zerfallen beim Bewegen weniger. "
     "Eine statische Szene bekommt keinen Extra-Pass. Die Änderung greift nach einem Neustart "
     "des Spiels.",
     "Vecteurs exacts pour les objets animés : vêtements et armes se brisent moins en mouvement. "
     "Une scène statique n'a pas de passe supplémentaire. Le changement s'applique après "
     "redémarrage du jeu.",
     "Vectores exactos para objetos animados: ropa y armas se descomponen menos al moverse. Una "
     "escena estática no recibe una pasada extra. El cambio se aplica al reiniciar el juego.",
     "Vettori esatti per gli oggetti animati: vestiti e armi si scompongono meno in movimento. "
     "Una scena statica non riceve una passata extra. La modifica si applica dopo il riavvio del "
     "gioco."},
    {"Show motion vectors (debug)",
     "Показать векторы движения (отладка)",
     "Bewegungsvektoren anzeigen (Debug)",
     "Afficher les vecteurs de mouvement (debug)",
     "Mostrar vectores de movimiento (depuración)",
     "Mostra i vettori di movimento (debug)"},
    {"Red/green: horizontal/vertical movement (8 pixels = full brightness). Blue: the pixel got "
     "an exact object vector, not just camera motion. A moving item without blue and red/green "
     "is treated as static by the upscaler — that is the trail.",
     "Красный/зелёный: движение по горизонтали/вертикали (8 пикселей = полная яркость). Синий: "
     "пиксель получил точный вектор объекта, а не только движение камеры. Движущийся предмет "
     "без синего и без красного/зелёного апскейлер считает неподвижным, отсюда шлейф.",
     "Rot/Grün: horizontale/vertikale Bewegung (8 Pixel = volle Helligkeit). Blau: der Pixel hat "
     "einen exakten Objektvektor, nicht nur Kamerabewegung. Ein bewegtes Objekt ohne Blau und "
     "ohne Rot/Grün gilt dem Upscaler als statisch — daher die Schlieren.",
     "Rouge/vert : mouvement horizontal/vertical (8 pixels = pleine luminosité). Bleu : le pixel "
     "a reçu un vecteur d'objet exact, pas seulement le mouvement caméra. Un objet en mouvement "
     "sans bleu ni rouge/vert est traité comme statique par l'upscaler — d'où la traînée.",
     "Rojo/verde: movimiento horizontal/vertical (8 píxeles = brillo pleno). Azul: el píxel "
     "recibió un vector de objeto exacto, no solo movimiento de cámara. Un objeto en movimiento "
     "sin azul ni rojo/verde el escalador lo trata como estático: de ahí la estela.",
     "Rosso/verde: movimento orizzontale/verticale (8 pixel = luminosità piena). Blu: il pixel "
     "ha ricevuto un vettore oggetto esatto, non solo il movimento di camera. Un oggetto in "
     "movimento senza blu né rosso/verde è considerato statico dall'upscaler — da lì la scia."},
    {"Output resolution",
     "Разрешение вывода",
     "Ausgabeauflösung",
     "Résolution de sortie",
     "Resolución de salida",
     "Risoluzione di output"},
    {"The finished frame and the UI. The preset sets the game's own scene size relative to the "
     "output: 4K Performance = 1920x1080, 1080p Ultra Performance = 640x360. Applied after "
     "restarting the game.",
     "Готовый кадр и интерфейс. Пресет задаёт собственный размер сцены игры относительно "
     "вывода: 4K Performance = 1920x1080, 1080p Ultra Performance = 640x360. Применяется после "
     "перезапуска игры.",
     "Das fertige Bild und die UI. Das Preset setzt die eigene Szenengröße des Spiels relativ "
     "zur Ausgabe: 4K Performance = 1920x1080, 1080p Ultra Performance = 640x360. Wird nach "
     "einem Neustart des Spiels angewandt.",
     "L'image finale et l'interface. Le preset fixe la taille de scène du jeu par rapport à la "
     "sortie : 4K Performance = 1920x1080, 1080p Ultra Performance = 640x360. Appliqué après "
     "redémarrage du jeu.",
     "El fotograma final y la interfaz. El preset fija el tamaño de escena del juego respecto a "
     "la salida: 4K Performance = 1920x1080, 1080p Ultra Performance = 640x360. Se aplica tras "
     "reiniciar el juego.",
     "Il fotogramma finale e l'interfaccia. Il preset imposta la dimensione della scena del "
     "gioco rispetto all'output: 4K Performance = 1920x1080, 1080p Ultra Performance = 640x360. "
     "Applicato dopo il riavvio del gioco."},
    {"The finished frame and the UI change at the next frame boundary. The preset sets the scene "
     "size relative to the output: 4K Performance = 1920x1080. A size change resets FSR history "
     "and can cause a short pause.",
     "Готовый кадр и интерфейс меняются на границе следующего кадра. Пресет задаёт размер сцены "
     "относительно вывода: 4K Performance = 1920x1080. Смена размера сбрасывает историю FSR и "
     "может вызвать короткую паузу.",
     "Fertiges Bild und UI wechseln an der nächsten Frame-Grenze. Das Preset setzt die "
     "Szenengröße relativ zur Ausgabe: 4K Performance = 1920x1080. Eine Größenänderung setzt "
     "die FSR-Historie zurück und kann eine kurze Pause verursachen.",
     "L'image finale et l'interface changent à la frontière d'image suivante. Le preset fixe la "
     "taille de scène par rapport à la sortie : 4K Performance = 1920x1080. Un changement de "
     "taille réinitialise l'historique FSR et peut causer une courte pause.",
     "El fotograma final y la interfaz cambian en el límite del siguiente fotograma. El preset "
     "fija el tamaño de escena respecto a la salida: 4K Performance = 1920x1080. Un cambio de "
     "tamaño reinicia el historial de FSR y puede causar una pausa breve.",
     "Il fotogramma finale e l'interfaccia cambiano al confine del fotogramma successivo. Il "
     "preset imposta la dimensione della scena rispetto all'output: 4K Performance = 1920x1080. "
     "Un cambio di dimensione azzera lo storico FSR e può causare una breve pausa."},
    {"Auto (by graphics card)",
     "Авто (по видеокарте)",
     "Auto (nach Grafikkarte)",
     "Auto (selon la carte graphique)",
     "Auto (según la tarjeta gráfica)",
     "Auto (in base alla scheda grafica)"},
    {"Off (faster)",
     "Выключена (быстрее)",
     "Aus (schneller)",
     "Désactivé (plus rapide)",
     "Desactivado (más rápido)",
     "Disattivato (più veloce)"},
    {"On (change without restarting)",
     "Включена",
     "An (Wechsel ohne Neustart)",
     "Activé (changement sans redémarrage)",
     "Activado (cambio sin reiniciar)",
     "Attivato (cambio senza riavvio)"},
    {"Live resolution changes",
     "Смена разрешения на лету",
     "Live-Auflösungswechsel",
     "Changement de résolution en direct",
     "Cambio de resolución en vivo",
     "Cambio risoluzione al volo"},
    {"Off: a reducing preset sets the game's own render size through a patch at start — the "
     "fastest path (changing it in the game restarts it). On: the game stays at 1080p internally "
     "and the port scales the scene targets at run time — change the output and the preset "
     "without a restart, at a cost. Auto takes the patch at 1080p when a preset reduces, and "
     "live changes for other outputs on strong discrete GPUs.",
     "Выключена: уменьшающий пресет задаёт собственный размер рендера игры патчем при запуске — "
     "самый быстрый путь (смена в игре — с перезапуском). Включена: игра остаётся в 1080p, порт "
     "масштабирует цели сцены на лету — менять вывод и пресет без перезапуска, но медленнее. "
     "Авто: патч при 1080p с уменьшающим пресетом, на лету — для остальных выводов на мощных "
     "дискретных GPU.",
     "Aus: ein reduzierendes Preset setzt die eigene Render-Größe des Spiels per Patch beim "
     "Start — der schnellste Weg (Wechsel im Spiel startet neu). An: das Spiel bleibt intern bei "
     "1080p und der Port skaliert die Szenenziele zur Laufzeit — Output und Preset ohne Neustart "
     "wechseln, mit Kosten. Auto: Patch bei 1080p mit reduzierendem Preset, Live-Wechsel für "
     "andere Ausgaben auf starken diskreten GPUs.",
     "Désactivé : un preset réducteur fixe la taille de rendu du jeu par un patch au lancement — "
     "le chemin le plus rapide (le changer en jeu redémarre). Activé : le jeu reste en 1080p en "
     "interne et le port met à l'échelle les cibles de scène en direct — changer sortie et "
     "preset sans redémarrage, avec un coût. Auto : patch en 1080p avec un preset réducteur, "
     "changement en direct pour les autres sorties sur GPU discrets puissants.",
     "Desactivado: un preset que reduce el tamaño fija el render del juego con un parche al "
     "iniciar — la vía más rápida (cambiarlo en el juego reinicia). Activado: el juego se queda "
     "en 1080p internamente y el port escala los objetivos de escena en vivo — cambiar salida y "
     "preset sin reinicio, con coste. Auto: parche a 1080p con preset reductor, cambio en vivo "
     "para otras salidas en GPU discretas potentes.",
     "Disattivato: un preset riducente imposta la dimensione di render del gioco con una patch "
     "all'avvio — la via più veloce (cambiarlo in gioco riavvia). Attivato: il gioco resta a "
     "1080p internamente e il port scala i target di scena al volo — cambiare output e preset "
     "senza riavvio, con un costo. Auto: patch a 1080p con preset riducente, cambio al volo per "
     "gli altri output su GPU discrete potenti."},
    {"Changes apply after restarting the game",
     "Изменения применятся после перезапуска игры",
     "Änderungen greifen nach einem Neustart des Spiels",
     "Les changements s'appliquent après redémarrage du jeu",
     "Los cambios se aplican tras reiniciar el juego",
     "Le modifiche si applicano dopo il riavvio del gioco"},
    {"Apply and restart the game",
     "Применить и перезапустить игру",
     "Anwenden und Spiel neu starten",
     "Appliquer et redémarrer le jeu",
     "Aplicar y reiniciar el juego",
     "Applica e riavvia il gioco"},
    {"Other",
     "Прочее",
     "Sonstiges",
     "Divers",
     "Otros",
     "Altro"},
    {"Language",
     "Язык",
     "Sprache",
     "Langue",
     "Idioma",
     "Lingua"},
    {"FPS counter in the corner",
     "Счётчик FPS в углу",
     "FPS-Zähler in der Ecke",
     "Compteur FPS dans le coin",
     "Contador de FPS en la esquina",
     "Contatore FPS nell'angolo"},
    {"Close",
     "Закрыть",
     "Schließen",
     "Fermer",
     "Cerrar",
     "Chiudi"},
    {"Settings are saved in bbport.ini",
     "Настройки сохраняются в bbport.ini",
     "Einstellungen werden in bbport.ini gespeichert",
     "Les paramètres sont enregistrés dans bbport.ini",
     "Los ajustes se guardan en bbport.ini",
     "Le impostazioni vengono salvate in bbport.ini"},
    {"%.0f FPS  %.1f ms  %s",
     "%.0f FPS  %.1f мс  %s",
     "%.0f FPS  %.1f ms  %s",
     "%.0f FPS  %.1f ms  %s",
     "%.0f FPS  %.1f ms  %s",
     "%.0f FPS  %.1f ms  %s"},
    {"Mousecam enabled",
     "Мышь-камера включена",
     "Mousecam aktiviert",
     "Mousecam activée",
     "Mousecam activada",
     "Mousecam attivata"},
    {"Mousecam disabled",
     "Мышь-камера выключена",
     "Mousecam deaktiviert",
     "Mousecam désactivée",
     "Mousecam desactivada",
     "Mousecam disattivata"},
    {"Keyboard: type, Backspace to erase, Enter = OK, Esc = cancel",
     "Клавиатура: ввод, Backspace — стереть, Enter = OK, Esc — отмена",
     "Tastatur: tippen, Backspace zum Löschen, Enter = OK, Esc = Abbrechen",
     "Clavier : saisir, Retour arrière pour effacer, Entrée = OK, Échap = annuler",
     "Teclado: escribe, Retroceso para borrar, Intro = OK, Esc = cancelar",
     "Tastiera: digita, Backspace per cancellare, Invio = OK, Esc = annulla"},
    {"Controller: Cross (A) = OK, Circle (B) = cancel",
     "Геймпад: Cross (A) = OK, Circle (B) = отмена",
     "Controller: Cross (A) = OK, Circle (B) = Abbrechen",
     "Manette : Croix (A) = OK, Cercle (B) = annuler",
     "Mando: Cruz (A) = OK, Círculo (B) = cancelar",
     "Controller: Croce (A) = OK, Cerchio (B) = annulla"},
};

constexpr const char* kCodes[LanguageCount] = {"en", "ru", "de", "fr", "es", "it"};
constexpr const char* kNames[LanguageCount] = {"English", "Русский", "Deutsch", "Français",
                                               "Español", "Italiano"};

int Clamp(int language) {
    return language >= 0 && language < LanguageCount ? language : English;
}

} // namespace

const char* Tr(const char* key) {
    const int language = BbSettings::Get().ui_language.load();
    if (language <= English || language >= LanguageCount) {
        return key;
    }
    for (const Entry& entry : kEntries) {
        if (std::strcmp(entry.key, key) == 0) {
            const char* text = entry.text[language - 1];
            return text && text[0] ? text : key;
        }
    }
    return key;
}

int LanguageFromCode(const char* code) {
    if (code) {
        for (int i = 0; i < LanguageCount; ++i) {
            if (std::strcmp(code, kCodes[i]) == 0) {
                return i;
            }
        }
    }
    return English;
}

const char* CodeFromLanguage(int language) {
    return kCodes[Clamp(language)];
}

const char* LanguageName(int language) {
    return kNames[Clamp(language)];
}

int EntryCount() {
    return int(sizeof(kEntries) / sizeof(kEntries[0]));
}

const char* EntryKey(int index) {
    return index >= 0 && index < EntryCount() ? kEntries[index].key : "";
}

const char* EntryText(int index, int language) {
    if (index < 0 || index >= EntryCount()) {
        return "";
    }
    if (language <= English || language >= LanguageCount) {
        return kEntries[index].key;
    }
    return kEntries[index].text[language - 1];
}

} // namespace BbText
