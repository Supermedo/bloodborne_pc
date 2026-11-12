# SPDX-License-Identifier: GPL-2.0-or-later
"""The texts the launcher check of 30 October 2026 found wrong or stale, rewritten: help is asked in Help & Support
on forum.bbport.de (no longer the Discord's #help), an eboot.bin that does not read or is cut off, the pages named as
they are now (All graphics, Picture & performance, Gameplay, Help > Show the log), the own server that only Seamless
co-op's start offers and where a friend's address goes, Windows' page file dialog, the Intel hints without their old
version, the setup guide's welcome and a card that was not detected. In every language besides English; keyed by the
English text like bbport_lang, merged into its tables on import."""
import bbport_lang

KEYS = [
    # Help's topics
    'It is not the original file (a PS4 SELF file), or it is damaged: another tool has converted it, a copy went '
    'wrong, or it is not from Bloodborne.',
    'Use the eboot.bin of an unmodified 1.09 update (exactly 93,825,704 bytes). Copy it again even if yours has '
    'that size: the size alone does not show damage.',
    'In the System Properties window that opens: the Advanced tab > Performance: Settings…, there the Advanced tab '
    '> Virtual memory: Change…',
    'All graphics (expert view): Quality preset Balanced or Performance (DLSS on GeForce RTX cards).',
    "Seamless co-op or a friend's server: open this launcher from Seamless co-op (OPEN BBPORT LAUNCHER + BBSC), "
    "then choose Own server under Play with. For a friend's server, enter the address from the person who runs it "
    'under Server address.',
    'It is shorter than the clean file: unpacking or copying it stopped early (the log then says "prepare failed: '
    'out of bounds").',
    # Help's last card and the report
    "Ask in Help & Support on forum.bbport.de: start a new topic. Save a report and attach it: it has the log, "
    "your settings and this PC's details (no passwords), so a helper sees the cause at once.",
    'Open the forum',
    'Its summary is in the clipboard: post it in a new topic in Help & Support on forum.bbport.de (Help > "Open '
    'the forum"), then attach the zip.',
    # the launcher's messages
    'Your eboot.bin is not the clean 1.09 one (modified, or from another release). The port cannot patch it: the '
    'game runs at 30 FPS in 16:9 (ultrawide and the output resolution do not apply), the effects (All graphics > '
    'Effects, Picture & performance > Taste) and the gameplay tweaks (Gameplay) do nothing, and some scenes (like '
    'the cutscene after Rom) can freeze.\n\nReplace eboot.bin with the one from an unmodified 1.09 update.',
    'The details are in the log (Help > "Show the log"). Help > "Save a report" makes the zip to attach when you '
    'ask for help.',
    # Your PC and the guide
    'Intel graphics: keep the upscaler off, stretched polygons can still appear with it on.',
    'Intel graphics: stretched polygons can still appear with an upscaler on, so turn it off.',
    'The guide checks your game and your PC, sets the picture for your hardware and brings your character along.',
    'Your graphics card was not detected. Pick it or the closest model:',
]

TEXTS = {
    'ru': [
        'Это не исходный файл (файл PS4 SELF) или он повреждён: его преобразовала другая программа, копирование '
        'прошло с ошибкой, или он не от Bloodborne.',
        'Используйте eboot.bin из неизменённого обновления 1.09 (ровно 93 825 704 байта). Скопируйте его заново, '
        'даже если ваш файл такого же размера: по одному размеру повреждение не видно.',
        'В открывшемся окне «Свойства системы»: вкладка «Дополнительно» > Быстродействие: «Параметры…», там '
        'вкладка «Дополнительно» > Виртуальная память: «Изменить…»',
        'Вся графика (режим эксперта): пресет Balanced или Performance (DLSS на видеокартах GeForce RTX).',
        'Seamless co-op или сервер друга: откройте этот лаунчер из Seamless co-op (OPEN BBPORT LAUNCHER + BBSC), '
        'затем выберите «Свой сервер» в «Играть через». Для сервера друга введите в поле «Адрес сервера» адрес, '
        'который даст тот, кто его запускает.',
        'Он короче чистого файла: распаковка или копирование оборвались раньше времени (в журнале тогда: «prepare '
        'failed: out of bounds»).',
        'Спросите в разделе Help & Support на forum.bbport.de: создайте новую тему. Сохраните отчёт и приложите '
        'его: в нём журнал, ваши настройки и данные этого ПК (без паролей), так помощник сразу увидит причину.',
        'Открыть форум',
        'Краткая сводка в буфере обмена: опубликуйте её в новой теме в разделе Help & Support на forum.bbport.de '
        '(«Помощь» > «Открыть форум»), затем приложите zip-файл.',
        'Ваш eboot.bin — не чистый файл 1.09 (изменён или из другого релиза). Порт не может его пропатчить: игра '
        'идёт в 30 FPS в 16:9 (ультраширокий формат и разрешение вывода не применяются), эффекты («Вся графика» > '
        '«Эффекты», «Изображение и FPS» > «Дело вкуса») и изменения игрового процесса («Игровой процесс») ничего не '
        'делают, а некоторые сцены (например, ролик после Ром) могут зависать.\n\nЗамените eboot.bin файлом из '
        'неизменённого обновления 1.09.',
        'Подробности в журнале («Помощь» > «Показать журнал»). «Помощь» > «Сохранить отчёт» создаёт zip-файл, '
        'который нужно приложить, когда просите о помощи.',
        'Графика Intel: не включайте апскейлер, с ним всё ещё могут появляться растянутые полигоны.',
        'Графика Intel: с включённым апскейлером всё ещё могут появляться растянутые полигоны, поэтому выключите '
        'его.',
        'Мастер проверит игру и ваш ПК, настроит изображение под ваше оборудование и перенесёт вашего персонажа.',
        'Ваша видеокарта не определена. Выберите её или ближайшую модель:',
    ],
    'ar': [
        'ليس الملف الأصلي (ملف PS4 SELF)، أو أنه تالف: حوّلته أداة أخرى، أو فشل نسخه، أو أنه ليس من Bloodborne.',
        'استخدم eboot.bin من تحديث 1.09 غير معدّل (93,825,704 بايت بالضبط). انسخه من جديد حتى لو كان لملفك هذا '
        'الحجم: الحجم وحده لا يكشف التلف.',
        'في نافذة «خصائص النظام» التي تفتح: علامة التبويب «خيارات متقدمة» > الأداء: «إعدادات…»، ثم علامة التبويب '
        '«خيارات متقدمة» > الذاكرة الظاهرية: «تغيير…»',
        'كل إعدادات الرسومات (وضع الخبير): إعداد الجودة Balanced أو Performance (DLSS على بطاقات GeForce RTX).',
        'Seamless co-op أو خادم صديق: افتح هذا المشغّل من Seamless co-op (OPEN BBPORT LAUNCHER + BBSC)، ثم اختر '
        '«خادمك الخاص» في «العب باستخدام». لخادم صديق: أدخل في «عنوان الخادم» العنوان الذي يعطيك إياه من يشغّله.',
        'إنه أقصر من الملف السليم: توقّف فك الضغط أو النسخ قبل اكتماله (ويذكر السجل حينها «prepare failed: out of '
        'bounds»).',
        'اسأل في قسم Help & Support على forum.bbport.de: ابدأ موضوعًا جديدًا. احفظ تقريرًا وأرفقه: فهو يحتوي على '
        'السجل وإعداداتك وتفاصيل هذا الحاسوب (دون كلمات مرور)، فيرى من يساعدك السبب فورًا.',
        'فتح المنتدى',
        'الملخص في الحافظة: انشره في موضوع جديد في قسم Help & Support على forum.bbport.de (المساعدة > "فتح '
        'المنتدى")، ثم أرفق ملف zip.',
        'ملف eboot.bin لديك ليس نسخة 1.09 النظيفة (معدّل، أو من إصدار آخر). لا يستطيع المنفذ ترقيعه: تعمل اللعبة '
        'بـ 30 إطارًا في الثانية بنسبة 16:9 (لا تنطبق الشاشة العريضة ولا دقة الإخراج)، ولا تفعل المؤثرات (كل '
        'إعدادات الرسومات > المؤثرات، الصورة والأداء > حسب الذوق) ولا تعديلات أسلوب اللعب (أسلوب اللعب) شيئًا، وقد '
        'تتجمد بعض المشاهد (مثل المشهد السينمائي بعد روم).\n\nاستبدل eboot.bin بالملف من تحديث 1.09 غير معدّل.',
        'التفاصيل في السجل (المساعدة > "عرض السجل"). المساعدة > "حفظ تقرير" ينشئ ملف zip لإرفاقه عندما تطلب '
        'المساعدة.',
        'رسومات Intel: أبقِ أداة التكبير متوقفة، فقد تظهر مضلعات ممتدة عند تفعيلها.',
        'رسومات Intel: قد تظهر مضلعات ممتدة عند تفعيل أداة التكبير، لذا أوقفها.',
        'يفحص المعالج لعبتك وحاسوبك، ويضبط الصورة لتناسب عتادك، وينقل شخصيتك معك.',
        'لم يتم التعرف على بطاقة الرسومات لديك. اخترها أو أقرب طراز لها:',
    ],
    'es': [
        'No es el archivo original (un archivo SELF de PS4) o está dañado: otra herramienta lo ha convertido, una '
        'copia salió mal o no es de Bloodborne.',
        'Usa el eboot.bin de una actualización 1.09 sin modificar (exactamente 93.825.704 bytes). Cópialo de nuevo '
        'aunque el tuyo tenga ese tamaño: el tamaño por sí solo no revela daños.',
        'En la ventana Propiedades del sistema que se abre: pestaña Opciones avanzadas > Rendimiento: '
        'Configuración…, y allí pestaña Opciones avanzadas > Memoria virtual: Cambiar…',
        'Todos los gráficos (vista experta): Preajuste de calidad Balanced o Performance (DLSS en tarjetas GeForce '
        'RTX).',
        'Seamless co-op o el servidor de un amigo: abre este lanzador desde Seamless co-op (OPEN BBPORT LAUNCHER + '
        'BBSC) y elige Servidor propio en «Jugar con». Para el servidor de un amigo, escribe en «Dirección del '
        'servidor» la dirección que te dé quien lo gestiona.',
        'Es más corto que el archivo limpio: la extracción o la copia se detuvo antes de tiempo (el registro dice '
        'entonces «prepare failed: out of bounds»).',
        'Pregunta en Help & Support en forum.bbport.de: abre un tema nuevo. Guarda un informe y adjúntalo: contiene '
        'el registro, tus ajustes y los datos de este PC (sin contraseñas), así quien te ayude verá la causa '
        'enseguida.',
        'Abrir el foro',
        'El resumen está en el portapapeles: publícalo en un tema nuevo en Help & Support en forum.bbport.de (Ayuda '
        '> «Abrir el foro») y luego adjunta el zip.',
        'Tu eboot.bin no es el 1.09 limpio (modificado, o de otra versión). El port no puede parchearlo: el juego '
        'funciona a 30 FPS en 16:9 (el ultrapanorámico y la resolución de salida no se aplican), los efectos (Todos '
        'los gráficos > Efectos, Imagen y rendimiento > Al gusto) y los ajustes de juego (Jugabilidad) no hacen nada '
        'y algunas escenas (como la cinemática tras Rom) pueden congelarse.\n\nSustituye eboot.bin por el de una '
        'actualización 1.09 sin modificar.',
        'Los detalles están en el registro (Ayuda > «Mostrar el registro»). Ayuda > «Guardar un informe» crea el '
        'zip que debes adjuntar cuando pidas ayuda.',
        'Gráficos Intel: mantén el escalador desactivado; con él activado aún pueden aparecer polígonos estirados.',
        'Gráficos Intel: con un escalador activado aún pueden aparecer polígonos estirados, así que desactívalo.',
        'El asistente comprueba tu juego y tu PC, ajusta la imagen a tu hardware y trae tu personaje.',
        'No se ha detectado tu tarjeta gráfica. Elígela o el modelo más parecido:',
    ],
    'pt': [
        'Não é o arquivo original (um arquivo SELF de PS4) ou está danificado: outra ferramenta o converteu, uma '
        'cópia deu errado, ou ele não é do Bloodborne.',
        'Use o eboot.bin de uma atualização 1.09 sem modificações (exatamente 93.825.704 bytes). Copie-o de novo '
        'mesmo que o seu tenha esse tamanho: o tamanho sozinho não mostra danos.',
        'Na janela Propriedades do Sistema que se abre: aba Avançado > Desempenho: Configurações…, e lá a aba '
        'Avançado > Memória virtual: Alterar…',
        'Gráficos (todos), no modo especialista: Predefinição de qualidade Balanced ou Performance (DLSS em placas '
        'GeForce RTX).',
        'Seamless co-op ou o servidor de um amigo: abra este launcher pelo Seamless co-op (OPEN BBPORT LAUNCHER + '
        'BBSC) e escolha Servidor próprio em «Jogar com». Para o servidor de um amigo, digite em «Endereço do '
        'servidor» o endereço que a pessoa que o mantém passar para você.',
        'Ele é menor que o arquivo limpo: a extração ou a cópia parou antes do fim (o registro então diz "prepare '
        'failed: out of bounds").',
        'Pergunte em Help & Support no forum.bbport.de: crie um tópico novo. Salve um relatório e anexe-o: ele traz '
        'o registro, as suas configurações e os dados deste PC (sem senhas), e assim quem for ajudar vê a causa na '
        'hora.',
        'Abrir o fórum',
        'O resumo está na área de transferência: publique-o em um tópico novo em Help & Support no forum.bbport.de '
        '(Ajuda > "Abrir o fórum") e depois anexe o zip.',
        'O seu eboot.bin não é o 1.09 limpo (modificado, ou de outra versão). O port não consegue aplicar patches '
        'nele: o jogo roda a 30 FPS em 16:9 (ultrawide e a resolução de saída não se aplicam), os efeitos (Gráficos '
        '(todos) > Efeitos, Imagem e desempenho > Gosto pessoal) e os ajustes de jogabilidade (Jogabilidade) não '
        'fazem nada e algumas cenas (como a cinemática depois de Rom) podem travar.\n\nSubstitua o eboot.bin pelo de '
        'uma atualização 1.09 não modificada.',
        'Os detalhes estão no registro (Ajuda > "Mostrar o registro"). Ajuda > "Salvar um relatório" cria o zip para '
        'anexar quando você pedir ajuda.',
        'Gráficos Intel: deixe o upscaler desligado; com ele ligado, ainda podem aparecer polígonos esticados.',
        'Gráficos Intel: polígonos esticados ainda podem aparecer com um upscaler ligado, então desligue-o.',
        'O assistente verifica o seu jogo e o seu PC, ajusta a imagem ao seu hardware e traz o seu personagem.',
        'Sua placa de vídeo não foi detectada. Escolha-a ou o modelo mais próximo:',
    ],
    'fr': [
        'Ce n’est pas le fichier d’origine (un fichier SELF PS4), ou il est endommagé : un autre outil l’a converti, '
        'une copie a échoué, ou il ne vient pas de Bloodborne.',
        'Utilisez l’eboot.bin d’une mise à jour 1.09 non modifiée (exactement 93 825 704 octets). Copiez-le à '
        'nouveau même si le vôtre a cette taille : la taille seule ne révèle pas un fichier endommagé.',
        'Dans la fenêtre Propriétés système qui s’ouvre : onglet Paramètres système avancés > Performances : '
        'Paramètres…, puis onglet Paramètres avancés > Mémoire virtuelle : Modifier…',
        'Tous les graphismes (mode expert) : Préréglage de qualité Balanced ou Performance (DLSS sur les cartes '
        'GeForce RTX).',
        'Seamless co-op ou le serveur d’un ami : ouvrez ce lanceur depuis Seamless co-op (OPEN BBPORT LAUNCHER + '
        'BBSC), puis choisissez Serveur perso dans « Jouer avec ». Pour le serveur d’un ami, saisissez dans « Adresse '
        'du serveur » l’adresse que vous donne la personne qui le gère.',
        'Il est plus court que le fichier d’origine : l’extraction ou la copie s’est arrêtée trop tôt (le journal '
        'indique alors « prepare failed: out of bounds »).',
        'Demandez dans Help & Support sur forum.bbport.de : créez un nouveau sujet. Enregistrez un rapport et '
        'joignez-le : il contient le journal, vos réglages et les caractéristiques de ce PC (aucun mot de passe), ce '
        'qui permet à la personne qui vous aide de voir la cause tout de suite.',
        'Ouvrir le forum',
        'Le résumé est dans le presse-papiers : publiez-le dans un nouveau sujet de Help & Support sur '
        'forum.bbport.de (Aide > « Ouvrir le forum »), puis joignez le zip.',
        "Votre eboot.bin n'est pas la 1.09 d'origine (modifié, ou d'une autre version). Le portage ne peut pas le "
        "patcher : le jeu tourne à 30 FPS en 16:9 (l'ultrawide et la résolution de sortie ne s'appliquent pas), les "
        'effets (Tous les graphismes > Effets, Image et performances > Selon vos goûts) et les ajustements de '
        "gameplay (Gameplay) n'ont aucun effet, et certaines scènes (comme la cinématique après Rom) peuvent se "
        "figer.\n\nRemplacez eboot.bin par celui d'une mise à jour 1.09 non modifiée.",
        'Les détails sont dans le journal (Aide > « Afficher le journal »). Aide > « Enregistrer un rapport » crée le '
        'zip à joindre quand vous demandez de l’aide.',
        'Graphismes Intel : laissez l’upscaler désactivé, des polygones étirés peuvent encore apparaître s’il est '
        'activé.',
        'Graphismes Intel : des polygones étirés peuvent encore apparaître avec un upscaler, désactivez-le donc.',
        'L’assistant vérifie votre jeu et votre PC, règle l’image selon votre matériel et récupère votre personnage.',
        'Votre carte graphique n’a pas été détectée. Choisissez-la, ou le modèle le plus proche :',
    ],
    'de': [
        'Es ist nicht die Originaldatei (eine PS4-SELF-Datei), oder sie ist beschädigt: Ein anderes Werkzeug hat sie '
        'umgewandelt, eine Kopie ging schief, oder sie ist nicht von Bloodborne.',
        'Nimm die eboot.bin eines unveränderten Updates 1.09 (genau 93.825.704 Bytes). Kopiere sie neu, auch wenn '
        'deine diese Größe hat: An der Größe allein sieht man keinen Schaden.',
        'Im Fenster Systemeigenschaften, das sich öffnet: Registerkarte Erweitert > Leistung: Einstellungen…, dort '
        'Registerkarte Erweitert > Virtueller Arbeitsspeicher: Ändern…',
        'Grafik (alle) im Expertenmodus: Qualitätsstufe Balanced oder Performance (DLSS auf GeForce-RTX-Karten).',
        'Seamless co-op oder der Server von Freunden: Öffne diesen Launcher aus Seamless co-op (OPEN BBPORT LAUNCHER '
        '+ BBSC) und wähle dann unter „Spielen mit“ „Eigener Server“. Für den Server von Freunden trägst du unter '
        '„Serveradresse“ die Adresse ein, die du von der Person bekommst, die ihn betreibt.',
        'Sie ist kürzer als die saubere Datei: Das Entpacken oder Kopieren hat vorzeitig aufgehört (im Log steht dann '
        '„prepare failed: out of bounds“).',
        'Frag in Help & Support auf forum.bbport.de: Starte ein neues Thema. Speichere einen Bericht und häng ihn an: '
        'Er enthält das Log, deine Einstellungen und die Daten dieses PCs (keine Passwörter), so sieht ein Helfer '
        'die Ursache sofort.',
        'Forum öffnen',
        'Die Zusammenfassung ist in der Zwischenablage: Poste sie in einem neuen Thema in Help & Support auf '
        'forum.bbport.de (Hilfe > „Forum öffnen“) und hänge dann die ZIP-Datei an.',
        'Deine eboot.bin ist nicht die saubere 1.09 (verändert oder aus einer anderen Veröffentlichung). Der Port '
        'kann sie nicht patchen: Das Spiel läuft mit 30 FPS in 16:9 (Ultrawide und die Ausgabeauflösung greifen '
        'nicht), die Effekte (Grafik (alle) > Effekte, Bild & Leistung > Geschmack) und die Spielmechanik '
        '(Spielweise) bewirken nichts, und manche Szenen (wie die Zwischensequenz nach Rom) können einfrieren.\n\n'
        'Ersetze eboot.bin durch die aus einem unveränderten 1.09-Update.',
        'Die Einzelheiten stehen im Protokoll (Hilfe > „Protokoll zeigen“). Hilfe > „Bericht speichern“ erstellt '
        'die ZIP-Datei, die du anhängst, wenn du um Hilfe bittest.',
        'Intel-Grafik: Lass den Upscaler aus, sonst können gestreckte Polygone auftreten.',
        'Intel-Grafik: Mit Upscaler können noch gestreckte Polygone auftreten, also schalte ihn aus.',
        'Der Assistent prüft dein Spiel und deinen PC, stellt das Bild für deine Hardware ein und bringt deinen '
        'Charakter mit.',
        'Deine Grafikkarte wurde nicht erkannt. Wähle sie oder das ähnlichste Modell:',
    ],
    'it': [
        'Non è il file originale (un file SELF PS4) oppure è danneggiato: un altro strumento lo ha convertito, una '
        'copia non è riuscita, oppure non è di Bloodborne.',
        'Usa l’eboot.bin di un aggiornamento 1.09 non modificato (esattamente 93.825.704 byte). Copialo di nuovo '
        'anche se il tuo ha questa dimensione: la dimensione da sola non rivela i danni.',
        'Nella finestra Proprietà del sistema che si apre: scheda Avanzate > Prestazioni: Impostazioni…, lì scheda '
        'Avanzate > Memoria virtuale: Cambia…',
        'Tutta la grafica (modalità esperto): Preset di qualità Balanced o Performance (DLSS sulle schede GeForce '
        'RTX).',
        'Seamless co-op o il server di un amico: apri questo launcher da Seamless co-op (OPEN BBPORT LAUNCHER + '
        'BBSC), poi scegli Server proprio in «Gioca con». Per il server di un amico, inserisci in «Indirizzo del '
        'server» l’indirizzo che ti dà chi lo gestisce.',
        'È più corto del file pulito: l’estrazione o la copia si è interrotta prima del tempo (il registro riporta '
        'allora "prepare failed: out of bounds").',
        'Chiedi in Help & Support su forum.bbport.de: apri una nuova discussione. Salva un rapporto e allegalo: '
        'contiene il registro, le tue impostazioni e i dati di questo PC (nessuna password), così chi ti aiuta vede '
        'subito la causa.',
        'Apri il forum',
        'Il riepilogo è negli appunti: pubblicalo in una nuova discussione in Help & Support su forum.bbport.de '
        '(Aiuto > "Apri il forum"), poi allega lo zip.',
        'Il tuo eboot.bin non è la 1.09 pulita (modificato, o di un’altra versione). Il port non può patcharlo: il '
        'gioco gira a 30 FPS in 16:9 (ultrawide e risoluzione di uscita non si applicano), gli effetti (Tutta la '
        'grafica > Effetti, Immagine e prestazioni > Gusto personale) e le modifiche al gameplay (Gameplay) non '
        'fanno nulla e alcune scene (come il filmato dopo Rom) possono bloccarsi.\n\nSostituisci eboot.bin con '
        'quello di un aggiornamento 1.09 non modificato.',
        'I dettagli sono nel registro (Aiuto > "Mostra il registro"). Aiuto > "Salva un rapporto" crea lo zip da '
        'allegare quando chiedi aiuto.',
        'Grafica Intel: tieni disattivato l’upscaler, perché con l’upscaler attivo possono ancora comparire poligoni '
        'stirati.',
        'Grafica Intel: con un upscaler attivo possono ancora comparire poligoni stirati, quindi disattivalo.',
        'La procedura guidata verifica il tuo gioco e il tuo PC, regola l’immagine per il tuo hardware e porta qui '
        'il tuo personaggio.',
        'La tua scheda video non è stata rilevata. Sceglila, o il modello più vicino:',
    ],
    'pl': [
        'To nie jest oryginalny plik (plik PS4 SELF) albo jest uszkodzony: inne narzędzie go przekonwertowało, '
        'kopiowanie się nie udało albo nie pochodzi z Bloodborne.',
        'Użyj eboot.bin z niezmodyfikowanej aktualizacji 1.09 (dokładnie 93 825 704 bajty). Skopiuj go ponownie, '
        'nawet jeśli twój ma taki rozmiar: sam rozmiar nie pokazuje uszkodzenia.',
        'W oknie Właściwości systemu, które się otworzy: karta Zaawansowane > Wydajność: Ustawienia…, tam karta '
        'Zaawansowane > Pamięć wirtualna: Zmień…',
        'Grafika (wszystko) w widoku eksperta: Profil jakości Balanced lub Performance (DLSS na kartach GeForce RTX).',
        'Seamless co-op lub serwer znajomego: otwórz ten launcher z Seamless co-op (OPEN BBPORT LAUNCHER + BBSC), a '
        'potem wybierz Własny serwer w „Graj z”. W przypadku serwera znajomego wpisz w „Adres serwera” adres od '
        'osoby, która go prowadzi.',
        'Jest krótszy niż czysty plik: rozpakowywanie lub kopiowanie przerwało się przedwcześnie (w dzienniku jest '
        'wtedy „prepare failed: out of bounds”).',
        'Zapytaj w Help & Support na forum.bbport.de: załóż nowy temat. Zapisz raport i dołącz go: zawiera dziennik, '
        'twoje ustawienia i dane tego PC (bez haseł), więc pomagający od razu zobaczy przyczynę.',
        'Otwórz forum',
        'Podsumowanie jest w schowku: opublikuj je w nowym temacie w Help & Support na forum.bbport.de (Pomoc > '
        '„Otwórz forum”), a potem dołącz plik zip.',
        'Twój eboot.bin nie jest czystą wersją 1.09 (zmodyfikowany albo z innego wydania). Port nie może go załatać: '
        'gra działa w 30 FPS w 16:9 (ultrawide i rozdzielczość wyjściowa nie działają), efekty (Grafika (wszystko) > '
        'Efekty, Obraz i wydajność > Kwestia gustu) i zmiany rozgrywki (Rozgrywka) nic nie robią, a niektóre sceny '
        '(jak przerywnik po Rom) mogą się zawiesić.\n\nZastąp eboot.bin plikiem z niezmodyfikowanej aktualizacji '
        '1.09.',
        'Szczegóły są w dzienniku (Pomoc > „Pokaż dziennik”). Pomoc > „Zapisz raport” tworzy plik zip, który należy '
        'dołączyć, prosząc o pomoc.',
        'Grafika Intel: pozostaw upscaler wyłączony, bo przy włączonym nadal mogą pojawiać się rozciągnięte '
        'wielokąty.',
        'Grafika Intel: przy włączonym upscalerze nadal mogą pojawiać się rozciągnięte wielokąty, więc go wyłącz.',
        'Kreator sprawdzi twoją grę i twój PC, dostosuje obraz do twojego sprzętu i przeniesie twoją postać.',
        'Nie wykryto twojej karty graficznej. Wybierz ją lub najbliższy model:',
    ],
    'tr': [
        'Orijinal dosya (bir PS4 SELF dosyası) değil ya da hasarlı: başka bir araç dönüştürmüş, bir kopyalama hatalı '
        'olmuş ya da Bloodborne’a ait değil.',
        'Değiştirilmemiş bir 1.09 güncellemesinin eboot.bin dosyasını kullanın (tam olarak 93.825.704 bayt). '
        'Sizinki bu boyutta olsa bile yeniden kopyalayın: boyut tek başına hasarı göstermez.',
        'Açılan Sistem Özellikleri penceresinde: Gelişmiş sekmesi > Performans: Ayarlar…, orada Gelişmiş sekmesi > '
        'Sanal bellek: Değiştir…',
        'Tüm grafik ayarları (uzman görünümü): Kalite ön ayarı Balanced veya Performance (GeForce RTX kartlarda '
        'DLSS).',
        'Seamless co-op veya bir arkadaşın sunucusu: bu başlatıcıyı Seamless co-op’tan açın (OPEN BBPORT LAUNCHER + '
        'BBSC), ardından "Oynanacak sunucu" altında Kendi sunucun’u seçin. Bir arkadaşın sunucusu için, sunucuyu '
        'çalıştıran kişiden aldığınız adresi "Sunucu adresi" alanına girin.',
        'Temiz dosyadan daha kısa: çıkarma veya kopyalama erken durmuş (günlükte o zaman "prepare failed: out of '
        'bounds" yazar).',
        'forum.bbport.de üzerindeki Help & Support bölümünde sorun: yeni bir konu açın. Bir rapor kaydedip ekleyin: '
        'raporda günlük, ayarlarınız ve bu bilgisayarın bilgileri (parolalar hariç) bulunur; böylece yardım eden '
        'kişi nedeni hemen görür.',
        'Forumu aç',
        'Özet panoda: forum.bbport.de üzerindeki Help & Support bölümünde yeni bir konuda paylaşın (Yardım > '
        '"Forumu aç"), ardından zip dosyasını ekleyin.',
        'eboot.bin dosyanız temiz 1.09 değil (değiştirilmiş veya başka bir sürümden). Port onu yamalayamaz: oyun '
        '16:9’da 30 FPS çalışır (ultrawide ve çıkış çözünürlüğü uygulanmaz), efektler (Tüm grafik ayarları > '
        'Efektler, Görüntü ve performans > Kişisel tercih) ve oynanış ayarları (Oynanış) hiçbir şey yapmaz ve bazı '
        'sahneler (Rom’dan sonraki ara sahne gibi) donabilir.\n\neboot.bin’i değiştirilmemiş bir 1.09 '
        'güncellemesindekiyle değiştirin.',
        'Ayrıntılar günlükte (Yardım > "Günlüğü göster"). Yardım > "Rapor kaydet", yardım isterken ekleyeceğiniz '
        'zip dosyasını oluşturur.',
        'Intel grafik: ölçekleyiciyi kapalı tutun; açıkken hâlâ gerilmiş poligonlar görülebilir.',
        'Intel grafik: ölçekleyici açıkken hâlâ gerilmiş poligonlar görülebilir, bu yüzden ölçekleyiciyi kapatın.',
        'Rehber oyununuzu ve bilgisayarınızı kontrol eder, görüntüyü donanımınıza göre ayarlar ve karakterinizi '
        'getirir.',
        'Ekran kartınız algılanmadı. Kartınızı veya en yakın modeli seçin:',
    ],
    'zh': [
        '这不是原始文件（PS4 SELF 文件），或者文件已损坏：它已被其他工具转换过、复制时出错，或者并非来自 Bloodborne。',
        '使用未经修改的 1.09 更新中的 eboot.bin（正好 93,825,704 字节）。即使你的文件也是这个大小，也请重新复制：仅凭大小看'
        '不出损坏。',
        '在打开的“系统属性”窗口中：“高级”选项卡 > 性能：“设置…”，然后在那里的“高级”选项卡 > 虚拟内存：“更改…”',
        '全部图形（专家视图）：画质预设选 Balanced 或 Performance（GeForce RTX 显卡使用 DLSS）。',
        'Seamless co-op 或朋友的服务器：从 Seamless co-op 打开本启动器（OPEN BBPORT LAUNCHER + BBSC），然后在“在线游玩”'
        '页面的“游玩设备”中选择“自己的服务器”。如果是朋友的服务器，请在“服务器地址”中填入运行该服务器的人给你的地址。',
        '它比干净的文件短：解包或复制提前中断了（日志中会显示“prepare failed: out of bounds”）。',
        '请到 forum.bbport.de 的 Help & Support 版块提问：发一个新帖子。保存一份报告并附上：其中包含日志、你的设置和这台电脑'
        '的信息（不含密码），帮助者可以立即看出原因。',
        '打开论坛',
        '摘要已在剪贴板中：将其发布到 forum.bbport.de 的 Help & Support 版块中的新帖子里（“帮助”>“打开论坛”），然后附上 '
        'zip 文件。',
        '你的 eboot.bin 不是干净的 1.09 版本（已修改，或来自其他发行版本）。移植无法给它打补丁：游戏以 30 FPS、16:9 运行（超'
        '宽屏和输出分辨率不生效），效果（“全部图形”>“效果”、“画面与性能”>“个人喜好”）和玩法调整（“玩法”）都不起作用，某些'
        '场景（如罗姆之后的过场动画）可能会卡死。\n\n请用未修改的 1.09 更新中的 eboot.bin 替换它。',
        '详细信息在日志中（“帮助”>“显示日志”）。“帮助”>“保存报告”会生成求助时需要附上的 zip 文件。',
        'Intel 显卡：请关闭超分辨率，开启时仍可能出现拉伸的多边形。',
        'Intel 显卡：开启超分辨率时仍可能出现拉伸的多边形，因此请将其关闭。',
        '向导会检查你的游戏和电脑，根据你的硬件设置画面，并带上你的角色。',
        '未检测到你的显卡。请选择它或最接近的型号：',
    ],
    'ja': [
        '元のファイル（PS4 の SELF ファイル）ではないか、壊れています。別のツールで変換された、コピーに失敗した、または '
        'Bloodborne のものではありません。',
        '改変されていない 1.09 アップデートの eboot.bin（ちょうど 93,825,704 バイト）を使ってください。同じサイズでもコピー'
        'し直してください：サイズだけでは破損はわかりません。',
        '開いた「システムのプロパティ」ウィンドウで：「詳細設定」タブ > パフォーマンス：「設定…」、そこで「詳細設定」タブ > '
        '仮想メモリ：「変更…」',
        '全グラフィック設定（エキスパート表示）：画質プリセットを Balanced または Performance に（GeForce RTX カードでは '
        'DLSS）。',
        'Seamless co-op や友人のサーバー：このランチャーを Seamless co-op から開き（OPEN BBPORT LAUNCHER + BBSC）、'
        '「接続先」で「自分のサーバー」を選んでください。友人のサーバーの場合は、運営している人から聞いたアドレスを'
        '「サーバーアドレス」に入力してください。',
        '無改変のファイルより短くなっています。展開またはコピーが途中で止まりました（ログには「prepare failed: out of '
        'bounds」と出ます）。',
        'forum.bbport.de の Help & Support で質問してください：新しいトピックを作成します。レポートを保存して添付すると、'
        'ログ、設定、この PC の情報（パスワードは含みません）が入っているので、サポートする人がすぐに原因を把握できます。',
        'フォーラムを開く',
        '概要はクリップボードにあります：forum.bbport.de の Help & Support に新しいトピックとして投稿し（ヘルプ >'
        '「フォーラムを開く」）、zip を添付してください。',
        'お使いの eboot.bin はクリーンな 1.09 ではありません（改変済み、または別のリリースのもの）。移植版はこれにパッチを当て'
        'られません。ゲームは 16:9 の 30 FPS で動作し（ウルトラワイドと出力解像度は適用されません）、エフェクト（全グラフィッ'
        'ク設定 > エフェクト、画質とパフォーマンス > 好み）とゲームプレイの調整（ゲームプレイ）は効かず、一部の場面（ロマの後'
        'のムービーなど）でフリーズすることがあります。\n\neboot.bin を改変されていない 1.09 アップデートのものに置き換えて'
        'ください。',
        '詳細はログにあります（ヘルプ >「ログを表示」）。ヘルプ >「レポートを保存」で、助けを求めるときに添付する zip が作成'
        'されます。',
        'Intel グラフィックス：アップスケーラーはオフのままにしてください。オンにすると、ポリゴンが引き伸ばされて表示されるこ'
        'とがあります。',
        'Intel グラフィックス：アップスケーラーがオンだと、ポリゴンが引き伸ばされて表示されることがあります。オフにしてくださ'
        'い。',
        'ガイドがゲームと PC を確認し、ハードウェアに合わせて画質を設定し、キャラクターを引き継ぎます。',
        'グラフィックカードが検出されませんでした。一覧から選ぶか、最も近いモデルを選んでください：',
    ],
    'ko': [
        '원본 파일(PS4 SELF 파일)이 아니거나 손상되었습니다. 다른 도구로 변환되었거나, 복사가 잘못되었거나, Bloodborne의 '
        '파일이 아닙니다.',
        '수정되지 않은 1.09 업데이트의 eboot.bin(정확히 93,825,704바이트)을 사용하세요. 크기가 같더라도 다시 복사하세요: '
        '크기만으로는 손상을 알 수 없습니다.',
        "열리는 '시스템 속성' 창에서: '고급' 탭 > 성능: '설정…', 그곳의 '고급' 탭 > 가상 메모리: '변경…'",
        '그래픽 전체(전문가 보기): 품질 프리셋 Balanced 또는 Performance (GeForce RTX 카드에서는 DLSS).',
        "Seamless co-op 또는 친구 서버: Seamless co-op에서 이 런처를 연 다음(OPEN BBPORT LAUNCHER + BBSC) '함께 "
        "플레이'에서 자체 서버를 선택하세요. 친구 서버라면 서버를 운영하는 사람에게 받은 주소를 '서버 주소'에 "
        '입력하세요.',
        '원본 파일보다 짧습니다. 압축 해제나 복사가 도중에 멈췄습니다(이때 로그에 "prepare failed: out of bounds"가 '
        '표시됨).',
        'forum.bbport.de의 Help & Support에 질문하세요: 새 주제를 만드세요. 보고서를 저장해 첨부하면 로그, 설정, 이 PC의 '
        '정보 (비밀번호 제외)가 들어 있어 도와주는 사람이 원인을 바로 알 수 있습니다.',
        '포럼 열기',
        '요약이 클립보드에 있습니다: forum.bbport.de의 Help & Support에 새 주제로 올린 다음(도움말 > "포럼 열기") zip '
        '파일을 첨부하세요.',
        'eboot.bin이 깨끗한 1.09가 아닙니다(수정되었거나 다른 릴리스의 것). 포트가 패치할 수 없습니다: 게임은 16:9에서 30 '
        'FPS로 실행되고(울트라와이드와 출력 해상도가 적용되지 않음), 효과(그래픽 전체 > 효과, 화면 및 성능 > 취향)와 게임플레이 '
        '조정(게임플레이)은 아무 효과가 없으며, 일부 장면(롬 이후의 컷신 등)에서 멈출 수 있습니다.\n\neboot.bin을 수정되지 '
        '않은 1.09 업데이트의 것으로 바꾸세요.',
        '자세한 내용은 로그에 있습니다(도움말 > "로그 보기"). 도움말 > "보고서 저장"은 도움을 요청할 때 첨부할 zip을 만듭니다.',
        'Intel 그래픽: 업스케일러를 끄세요. 켜면 늘어난 폴리곤이 여전히 나타날 수 있습니다.',
        'Intel 그래픽: 업스케일러를 켜면 늘어난 폴리곤이 여전히 나타날 수 있으므로 업스케일러를 끄세요.',
        '안내가 게임과 PC를 확인하고, 하드웨어에 맞게 화질을 설정하고, 캐릭터를 가져옵니다.',
        '그래픽 카드가 감지되지 않았습니다. 해당 모델이나 가장 비슷한 모델을 고르세요:',
    ],
}

for _language, _texts in TEXTS.items():
    assert len(_texts) == len(KEYS), (_language, len(_texts), len(KEYS))
    bbport_lang.EXTRA_TRANSLATIONS.setdefault(_language, {}).update(zip(KEYS, _texts))

# The second sentence of the old eboot.bin step, a step of its own since the first one names the plain ELF size too.
COPY_AGAIN = 'Copy it again even if yours has that size: the size alone does not show damage.'
COPY_AGAIN_TEXTS = {
    'ru': 'Скопируйте его заново, даже если ваш файл такого же размера: по одному размеру повреждение не видно.',
    'ar': 'انسخه من جديد حتى لو كان لملفك هذا الحجم: الحجم وحده لا يكشف التلف.',
    'es': 'Cópialo de nuevo aunque el tuyo tenga ese tamaño: el tamaño por sí solo no revela daños.',
    'pt': 'Copie-o de novo mesmo que o seu tenha esse tamanho: o tamanho sozinho não mostra danos.',
    'fr': 'Copiez-le à nouveau même si le vôtre a cette taille : la taille seule ne révèle pas un fichier endommagé.',
    'de': 'Kopiere sie neu, auch wenn deine diese Größe hat: An der Größe allein sieht man keinen Schaden.',
    'it': 'Copialo di nuovo anche se il tuo ha questa dimensione: la dimensione da sola non rivela i danni.',
    'pl': 'Skopiuj go ponownie, nawet jeśli twój ma taki rozmiar: sam rozmiar nie pokazuje uszkodzenia.',
    'tr': 'Sizinki bu boyutta olsa bile yeniden kopyalayın: boyut tek başına hasarı göstermez.',
    'zh': '即使你的文件也是这个大小，也请重新复制：仅凭大小看不出损坏。',
    'ja': '同じサイズでもコピーし直してください：サイズだけでは破損はわかりません。',
    'ko': '크기가 같더라도 다시 복사하세요: 크기만으로는 손상을 알 수 없습니다.',
}
for _language, _text in COPY_AGAIN_TEXTS.items():
    bbport_lang.EXTRA_TRANSLATIONS.setdefault(_language, {})[COPY_AGAIN] = _text
