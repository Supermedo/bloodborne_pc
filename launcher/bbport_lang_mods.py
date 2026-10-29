# SPDX-License-Identifier: GPL-2.0-or-later
"""Mods and the port's own changes (bbport_mods: the notes under each mod, the Play page's line, Start once
without mods), in every language besides English. Keyed by the English text like bbport_lang; merged into
its tables on import. The game's own words (Quit to Desktop, Rebirth) as the port's menus write them
(scripts/quit_menu.py, rebirth.py)."""
import bbport_lang

KEYS = [
    'Start once without mods',
    'Plays this one start with no mod loaded, to see whether one of them causes a problem. They stay on for the '
    'next start.',
    'This start: without mods. They stay on for the next one.',
    'The game ended with an error (code {}).',
    '{} mods were loaded. Start once without them, to see whether one of them causes it? They stay on for later '
    'starts.',
    "Mod '{}': {} was off at the last start (click for why)",
    'Changes the title or System menu: Quit to Desktop and PC Settings are fitted into it, or stay off while this '
    'mod is on.',
    'Changes menu texts: the port adds its own (Quit to Desktop, PC Settings, Rebirth) to them.',
    "Changes the Doll's talk: Rebirth is fitted into it, or stays off while this mod is on.",
    'Changes game parameters: the field of view and Rebirth use its values.',
    'Changes menu screens: the ultrawide HUD layout is fitted to them.',
    '{} (lower in the list) replaces {} of its files.',
    "Off at the last start because of this mod's {}: {}.",
    'Quit to Desktop',
    'PC Settings in the game menu',
    'Rebirth at the Doll',
    'Ultrawide HUD layout',
    'Discord status',
]
TEXTS = {
    'ru': [
        'Запустить один раз без модов',
        'Только этот запуск без модов — чтобы проверить, не вызывает ли проблему один из них. При следующем запуске '
        'они снова включены.',
        'Этот запуск: без модов. При следующем они снова включены.',
        'Игра завершилась с ошибкой (код {}).',
        'Было загружено модов: {}. Запустить один раз без них, чтобы проверить, не в них ли причина? Для следующих '
        'запусков они остаются включёнными.',
        'Мод «{}»: {} был выключен при последнем запуске (нажмите, чтобы узнать почему)',
        'Меняет титульное или системное меню: «Выйти на рабочий стол» и настройки ПК встраиваются в него или '
        'остаются выключенными, пока этот мод включён.',
        'Меняет тексты меню: порт добавляет к ним свои («Выйти на рабочий стол», настройки ПК, «Перерождение»).',
        'Меняет разговоры Куклы: «Перерождение» встраивается в них или остаётся выключенным, пока этот мод включён.',
        'Меняет параметры игры: поле зрения и «Перерождение» используют его значения.',
        'Меняет экраны меню: раскладка интерфейса для сверхшироких экранов подгоняется под них.',
        '{} (ниже в списке) заменяет его файлов: {}.',
        'При последнем запуске выключено из-за {} этого мода: {}.',
        'Выйти на рабочий стол',
        'Настройки ПК в меню игры',
        'Перерождение у Куклы',
        'Интерфейс для сверхшироких экранов',
        'Статус в Discord',
    ],
    'ar': [
        'التشغيل مرة واحدة بدون تعديلات',
        'يشغّل هذه المرة فقط دون تحميل أي تعديل، لمعرفة ما إذا كان أحدها يسبب مشكلة. تبقى مفعّلة في التشغيل التالي.',
        'هذا التشغيل: بدون تعديلات. تبقى مفعّلة في التشغيل التالي.',
        'انتهت اللعبة بخطأ (الرمز {}).',
        'كانت هناك {} تعديلات محمّلة. هل تريد التشغيل مرة واحدة بدونها لمعرفة ما إذا كان أحدها هو السبب؟ تبقى '
        'مفعّلة في المرات القادمة.',
        'التعديل «{}»: كان {} متوقفًا في آخر تشغيل (انقر لمعرفة السبب)',
        'يغيّر قائمة العنوان أو قائمة النظام: يُدمج فيها الخروج إلى سطح المكتب وإعدادات الكمبيوتر، أو يبقيان '
        'متوقفين ما دام هذا التعديل مفعّلًا.',
        'يغيّر نصوص القوائم: يضيف المنفذ إليها نصوصه الخاصة (الخروج إلى سطح المكتب، إعدادات الكمبيوتر، إعادة '
        'الولادة).',
        'يغيّر حوارات الدمية: تُدمج فيها إعادة الولادة، أو تبقى متوقفة ما دام هذا التعديل مفعّلًا.',
        'يغيّر معاملات اللعبة: يستخدم مجال الرؤية وإعادة الولادة قيمه.',
        'يغيّر شاشات القوائم: يُكيَّف معها تخطيط واجهة الشاشات فائقة العرض.',
        '{} (أسفل في القائمة) يستبدل {} من ملفاته.',
        'كان متوقفًا في آخر تشغيل بسبب {} في هذا التعديل: {}.',
        'الخروج إلى سطح المكتب',
        'إعدادات الكمبيوتر في قائمة اللعبة',
        'إعادة الولادة عند الدمية',
        'تخطيط واجهة الشاشات فائقة العرض',
        'حالة Discord',
    ],
    'es': [
        'Iniciar una vez sin mods',
        'Inicia solo esta vez sin ningún mod, para ver si alguno causa un problema. Siguen activados para el próximo '
        'inicio.',
        'Este inicio: sin mods. Siguen activados para el próximo.',
        'El juego terminó con un error (código {}).',
        'Había {} mods cargados. ¿Iniciar una vez sin ellos, para ver si alguno es la causa? Siguen activados para '
        'los próximos inicios.',
        'Mod «{}»: {} estuvo desactivado en el último inicio (haz clic para ver por qué)',
        'Cambia el menú de título o de Sistema: Salir al escritorio y los ajustes de PC se integran en él, o quedan '
        'desactivados mientras este mod esté activo.',
        'Cambia textos de menú: el port les añade los suyos (Salir al escritorio, ajustes de PC, Renacimiento).',
        'Cambia los diálogos de la Muñeca: Renacimiento se integra en ellos, o queda desactivado mientras este mod '
        'esté activo.',
        'Cambia parámetros del juego: el campo de visión y Renacimiento usan sus valores.',
        'Cambia pantallas de menú: el diseño del HUD ultrapanorámico se adapta a ellas.',
        '{} (más abajo en la lista) reemplaza {} de sus archivos.',
        'Desactivado en el último inicio por el archivo {} de este mod: {}.',
        'Salir al escritorio',
        'Ajustes de PC en el menú del juego',
        'Renacimiento con la Muñeca',
        'HUD ultrapanorámico',
        'Estado de Discord',
    ],
    'pt': [
        'Iniciar uma vez sem mods',
        'Inicia só desta vez sem nenhum mod, para ver se algum deles causa um problema. Eles continuam ativados no '
        'próximo início.',
        'Este início: sem mods. Eles continuam ativados no próximo.',
        'O jogo terminou com um erro (código {}).',
        '{} mods estavam carregados. Iniciar uma vez sem eles, para ver se algum deles é a causa? Eles continuam '
        'ativados nos próximos inícios.',
        'Mod “{}”: {} ficou desativado no último início (clique para ver o motivo)',
        'Altera o menu de título ou do Sistema: Sair para a área de trabalho e as configurações de PC são encaixados '
        'nele, ou ficam desativados enquanto este mod estiver ativo.',
        'Altera textos de menu: o port acrescenta os seus (Sair para a área de trabalho, configurações de PC, '
        'Renascimento).',
        'Altera as falas da Boneca: o Renascimento é encaixado nelas, ou fica desativado enquanto este mod estiver '
        'ativo.',
        'Altera parâmetros do jogo: o campo de visão e o Renascimento usam os valores dele.',
        'Altera telas de menu: o layout do HUD ultrawide é ajustado a elas.',
        '{} (mais abaixo na lista) substitui {} dos arquivos dele.',
        'Desativado no último início por causa do arquivo {} deste mod: {}.',
        'Sair para a área de trabalho',
        'Configurações de PC no menu do jogo',
        'Renascimento com a Boneca',
        'Layout do HUD ultrawide',
        'Status no Discord',
    ],
    'fr': [
        'Lancer une fois sans mods',
        'Lance seulement cette fois sans aucun mod, pour voir si l’un d’eux cause un problème. Ils restent activés '
        'pour le prochain lancement.',
        'Ce lancement : sans mods. Ils restent activés pour le suivant.',
        'Le jeu s’est terminé sur une erreur (code {}).',
        '{} mods étaient chargés. Lancer une fois sans eux, pour voir si l’un d’eux en est la cause ? Ils restent '
        'activés pour les lancements suivants.',
        'Mod « {} » : {} était désactivé au dernier lancement (cliquez pour savoir pourquoi)',
        'Modifie le menu titre ou Système : Quitter vers le bureau et les réglages PC y sont intégrés, ou restent '
        'désactivés tant que ce mod est actif.',
        'Modifie des textes de menu : le portage y ajoute les siens (Quitter vers le bureau, réglages PC, '
        'Renaissance).',
        'Modifie les dialogues de la Poupée : Renaissance y est intégrée, ou reste désactivée tant que ce mod est '
        'actif.',
        'Modifie les paramètres du jeu : le champ de vision et Renaissance utilisent ses valeurs.',
        'Modifie des écrans de menu : la disposition de l’interface ultra-large y est adaptée.',
        '{} (plus bas dans la liste) remplace {} de ses fichiers.',
        'Désactivé au dernier lancement à cause du fichier {} de ce mod : {}.',
        'Quitter vers le bureau',
        'Réglages PC dans le menu du jeu',
        'Renaissance auprès de la Poupée',
        'Interface ultra-large',
        'Statut Discord',
    ],
    'de': [
        'Einmal ohne Mods starten',
        'Startet nur dieses eine Mal ohne Mods, um zu sehen, ob einer davon ein Problem verursacht. Beim nächsten '
        'Start sind sie wieder an.',
        'Dieser Start: ohne Mods. Beim nächsten sind sie wieder an.',
        'Das Spiel wurde mit einem Fehler beendet (Code {}).',
        '{} Mods waren geladen. Einmal ohne sie starten, um zu sehen, ob einer davon die Ursache ist? Für spätere '
        'Starts bleiben sie an.',
        'Mod „{}“: {} war beim letzten Start aus (klicken für den Grund)',
        'Ändert das Titel- oder System-Menü: Zum Desktop beenden und die PC-Einstellungen werden eingefügt oder '
        'bleiben aus, solange dieser Mod an ist.',
        'Ändert Menütexte: Der Port fügt seine eigenen hinzu (Zum Desktop beenden, PC-Einstellungen, Wiedergeburt).',
        'Ändert die Gespräche der Puppe: Wiedergeburt wird eingefügt oder bleibt aus, solange dieser Mod an ist.',
        'Ändert Spielparameter: Das Sichtfeld und die Wiedergeburt nutzen seine Werte.',
        'Ändert Menübildschirme: Das Ultrawide-HUD-Layout wird an sie angepasst.',
        '{} (weiter unten in der Liste) ersetzt {} seiner Dateien.',
        'Beim letzten Start wegen {} dieses Mods aus: {}.',
        'Zum Desktop beenden',
        'PC-Einstellungen im Spielmenü',
        'Wiedergeburt bei der Puppe',
        'Ultrawide-HUD-Layout',
        'Discord-Status',
    ],
    'it': [
        'Avvia una volta senza mod',
        'Avvia solo questa volta senza alcuna mod, per vedere se una di esse causa un problema. Restano attive per il '
        'prossimo avvio.',
        'Questo avvio: senza mod. Restano attive per il prossimo.',
        'Il gioco si è chiuso con un errore (codice {}).',
        'Erano caricate {} mod. Avviare una volta senza, per vedere se una di esse è la causa? Restano attive per gli '
        'avvii successivi.',
        'Mod «{}»: {} era disattivato all’ultimo avvio (clicca per sapere perché)',
        'Modifica il menu del titolo o di Sistema: Esci al desktop e le impostazioni PC vi vengono inseriti, oppure '
        'restano disattivati finché questa mod è attiva.',
        'Modifica testi dei menu: il port vi aggiunge i propri (Esci al desktop, impostazioni PC, Rinascita).',
        'Modifica i dialoghi della Bambola: Rinascita vi viene inserita, oppure resta disattivata finché questa mod è '
        'attiva.',
        'Modifica parametri di gioco: il campo visivo e Rinascita usano i suoi valori.',
        'Modifica schermate dei menu: il layout dell’HUD ultrawide viene adattato a esse.',
        '{} (più in basso nella lista) sostituisce {} dei suoi file.',
        'Disattivato all’ultimo avvio per il file {} di questa mod: {}.',
        'Esci al desktop',
        'Impostazioni PC nel menu di gioco',
        'Rinascita presso la Bambola',
        'Layout HUD ultrawide',
        'Stato di Discord',
    ],
    'pl': [
        'Uruchom raz bez modów',
        'Uruchamia tylko ten raz bez żadnego moda, żeby sprawdzić, czy któryś z nich powoduje problem. Przy '
        'następnym uruchomieniu znów są włączone.',
        'To uruchomienie: bez modów. Przy następnym znów są włączone.',
        'Gra zakończyła się błędem (kod {}).',
        'Wczytanych modów: {}. Uruchomić raz bez nich, żeby sprawdzić, czy któryś z nich jest przyczyną? Przy '
        'kolejnych uruchomieniach pozostają włączone.',
        'Mod „{}”: {} był wyłączony przy ostatnim uruchomieniu (kliknij, aby zobaczyć dlaczego)',
        'Zmienia menu tytułowe lub Systemu: Wyjdź do pulpitu i ustawienia PC są do niego wstawiane albo pozostają '
        'wyłączone, dopóki ten mod jest włączony.',
        'Zmienia teksty menu: port dodaje do nich własne (Wyjdź do pulpitu, ustawienia PC, Odrodzenie).',
        'Zmienia rozmowy Lalki: Odrodzenie jest do nich wstawiane albo pozostaje wyłączone, dopóki ten mod jest '
        'włączony.',
        'Zmienia parametry gry: pole widzenia i Odrodzenie korzystają z jego wartości.',
        'Zmienia ekrany menu: układ HUD dla ultrawide jest do nich dopasowywany.',
        '{} (niżej na liście) zastępuje plików tego moda: {}.',
        'Wyłączone przy ostatnim uruchomieniu przez plik {} tego moda: {}.',
        'Wyjdź do pulpitu',
        'Ustawienia PC w menu gry',
        'Odrodzenie u Lalki',
        'Układ HUD dla ultrawide',
        'Status w Discordzie',
    ],
    'tr': [
        'Bir kez modsuz başlat',
        'Yalnızca bu başlatmayı hiç mod yüklemeden yapar; böylece bir modun soruna yol açıp açmadığını görürsün. '
        'Sonraki başlatmada yine açık olurlar.',
        'Bu başlatma: modsuz. Bir sonrakinde yine açık olurlar.',
        'Oyun bir hatayla kapandı (kod {}).',
        '{} mod yüklüydü. Sebebin bunlardan biri olup olmadığını görmek için bir kez onlarsız başlatılsın mı? Sonraki '
        'başlatmalarda açık kalırlar.',
        "'{}' modu: {} son başlatmada kapalıydı (nedeni için tıkla)",
        'Başlık veya Sistem menüsünü değiştirir: Masaüstüne çık ve PC ayarları ona eklenir ya da bu mod açık olduğu '
        'sürece kapalı kalır.',
        'Menü metinlerini değiştirir: port kendi metinlerini (Masaüstüne çık, PC ayarları, Yeniden Doğuş) bunlara '
        'ekler.',
        'Bebek’in konuşmalarını değiştirir: Yeniden Doğuş bunlara eklenir ya da bu mod açık olduğu sürece kapalı '
        'kalır.',
        'Oyun parametrelerini değiştirir: görüş alanı ve Yeniden Doğuş onun değerlerini kullanır.',
        'Menü ekranlarını değiştirir: ultra geniş HUD düzeni bunlara uyarlanır.',
        '{} (listede daha aşağıda) dosyalarından {} tanesinin yerini alıyor.',
        'Son başlatmada bu modun {} dosyası yüzünden kapalıydı: {}.',
        'Masaüstüne çık',
        'Oyun menüsünde PC ayarları',
        'Bebek’te Yeniden Doğuş',
        'Ultra geniş HUD düzeni',
        'Discord durumu',
    ],
    'zh': [
        '不加载模组启动一次',
        '仅这一次在不加载任何模组的情况下启动，以确认是否是某个模组导致了问题。下次启动时它们仍然开启。',
        '本次启动：不加载模组。下次启动时它们仍然开启。',
        '游戏因错误而结束（代码 {}）。',
        '已加载 {} 个模组。要不加载它们启动一次，看看原因是否在其中吗？之后的启动中它们仍然开启。',
        '模组“{}”：上次启动时{}已关闭（点击查看原因）',
        '修改标题菜单或系统菜单：“退出到桌面”和 PC 设置会被加入其中，否则在此模组开启期间保持关闭。',
        '修改菜单文本：移植版会在其中加入自己的文本（退出到桌面、PC 设置、重生）。',
        '修改人偶的对话：“重生”会被加入其中，否则在此模组开启期间保持关闭。',
        '修改游戏参数：视野和“重生”会使用它的数值。',
        '修改菜单界面：超宽屏 HUD 布局会适配它们。',
        '{}（列表中更靠下）替换了它的 {} 个文件。',
        '上次启动时因此模组的 {} 而关闭：{}。',
        '退出到桌面',
        '游戏菜单中的 PC 设置',
        '在人偶处重生',
        '超宽屏 HUD 布局',
        'Discord 状态',
    ],
    'ja': [
        'MODなしで一度起動',
        '今回だけMODを読み込まずに起動し、どれかのMODが問題の原因かを確かめます。次回の起動ではまた有効です。',
        '今回の起動：MODなし。次回はまた有効です。',
        'ゲームがエラーで終了しました（コード {}）。',
        '{} 個のMODが読み込まれていました。原因がその中にあるか確かめるため、MODなしで一度起動しますか？以降の起動ではMODは'
        '有効のままです。',
        'MOD「{}」：前回の起動では{}がオフでした（クリックで理由を表示）',
        'タイトルメニューまたはシステムメニューを変更します：「デスクトップに戻る」とPC設定はそこに組み込まれます。できない場合、'
        'このMODが有効な間はオフになります。',
        'メニューのテキストを変更します：移植版は自分のテキスト（デスクトップに戻る、PC設定、生まれ直す）をそこに追加します。',
        '人形の会話を変更します：「生まれ直す」はそこに組み込まれます。できない場合、このMODが有効な間はオフになります。',
        'ゲームのパラメータを変更します：視野と「生まれ直す」はその値を使います。',
        'メニュー画面を変更します：ウルトラワイド用HUDレイアウトはそれに合わせられます。',
        '{}（リストのより下）がこのMODのファイルを {} 個置き換えます。',
        '前回の起動では、このMODの {} のためオフでした：{}。',
        'デスクトップに戻る',
        'ゲームメニューのPC設定',
        '人形のもとで生まれ直す',
        'ウルトラワイド用HUDレイアウト',
        'Discordステータス',
    ],
    'ko': [
        '모드 없이 한 번 시작',
        '이번 한 번만 모드를 불러오지 않고 시작해서, 모드 중 하나가 문제의 원인인지 확인합니다. 다음 시작 때는 다시 켜져 '
        '있습니다.',
        '이번 시작: 모드 없이. 다음에는 다시 켜집니다.',
        '게임이 오류로 종료되었습니다(코드 {}).',
        '모드 {}개를 불러왔습니다. 원인이 그중에 있는지 확인하기 위해 모드 없이 한 번 시작할까요? 이후 시작에서는 계속 '
        '켜져 있습니다.',
        "모드 '{}': 지난 시작에서 {}이(가) 꺼져 있었습니다(클릭하면 이유 표시)",
        '타이틀 또는 시스템 메뉴를 바꿉니다: 바탕 화면으로 종료와 PC 설정이 여기에 맞춰 들어가며, 안 되면 이 모드가 켜져 '
        '있는 동안 꺼집니다.',
        '메뉴 텍스트를 바꿉니다: 포트가 자체 텍스트(바탕 화면으로 종료, PC 설정, 다시 태어나기)를 여기에 추가합니다.',
        '인형의 대화를 바꿉니다: 다시 태어나기가 여기에 맞춰 들어가며, 안 되면 이 모드가 켜져 있는 동안 꺼집니다.',
        '게임 파라미터를 바꿉니다: 시야와 다시 태어나기가 이 값을 사용합니다.',
        '메뉴 화면을 바꿉니다: 울트라와이드 HUD 레이아웃이 여기에 맞춰집니다.',
        '{}(목록에서 더 아래)이(가) 이 모드의 파일 {}개를 대체합니다.',
        '지난 시작에서 이 모드의 {} 때문에 꺼져 있었습니다: {}.',
        '바탕 화면으로 종료',
        '게임 메뉴의 PC 설정',
        '인형에게서 다시 태어나기',
        '울트라와이드 HUD 레이아웃',
        'Discord 상태',
    ],
}

for _language, _texts in TEXTS.items():
    assert len(_texts) == len(KEYS), (_language, len(_texts), len(KEYS))
    bbport_lang.EXTRA_TRANSLATIONS.setdefault(_language, {}).update(zip(KEYS, _texts))
