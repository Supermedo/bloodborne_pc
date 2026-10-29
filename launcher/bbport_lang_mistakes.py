# SPDX-License-Identifier: GPL-2.0-or-later
"""Texts of the repairs for player mistakes (the game already running, settings that cannot be saved, a game
folder without sce_module, saves in the wrong folder, a refused sign-in, a wrong PC clock, a missing saves folder,
a port started inside a zip), in every language besides English and Russian (Russian is in the code). Machine
translated with the launcher's existing page names as the glossary; native speakers are welcome to improve them.
MISTAKE_KEYS are the English texts; each language's list is in the same order. Merged into bbport_lang's tables
on import."""
import bbport_lang

MISTAKE_KEYS = [
    'The game runs already (started from Steam, a shortcut or the in-game restart): its window was brought to the '
    'front.',
    'Something went wrong in the launcher: {}',
    'The details are on the Log page. Help > "Save a report" makes the zip to attach when you ask for help.',
    'Your settings could not be saved, so the game cannot start with them.\n\nFile: {}\nFolder: {}\nWindows says: '
    '{}\n\nFree up space on that drive, or move the port to a folder you can write to (like C:\\Games\\BBPort).',
    'The game folder has no {}: the game was not unpacked completely. Unpack the base game into a new, empty folder '
    'again and copy the 1.09 update over it.',
    'Saves: a save in {} could not be copied to where the game reads it: {}',
    'Saves: {} copied to where the game reads them ({}); the original stays where it was.',
    "The Hunter's Dream refused this PC's sign-in: sign in again on the Online page.",
    "Your PC's date is wrong ({}), so secure connections fail. Set the date in Windows: Settings > Time & language > "
    'Date & time, "Set time automatically".',
    'The saves folder {} is not there: its drive is not connected, or the folder was renamed or moved. Connect the '
    'drive, or choose the saves folder again.',
    'The port runs from a temporary folder ({}), as when it is started from inside a zip or rar file. That folder is '
    'deleted when the archive closes, and your saves with it.\n\nExtract the port to a folder first (like '
    'C:\\Games\\BBPort) and start Bloodborne.exe from there.',
]

MISTAKES = {
    'de': [
        'Das Spiel läuft bereits (gestartet über Steam, eine Verknüpfung oder den Neustart im Spiel): Sein Fenster '
        'wurde nach vorne geholt.',
        'Im Launcher ist ein Fehler aufgetreten: {}',
        'Die Einzelheiten stehen auf der Seite Protokoll. Hilfe > „Bericht speichern“ erstellt die ZIP-Datei, die du '
        'anhängst, wenn du um Hilfe bittest.',
        'Deine Einstellungen konnten nicht gespeichert werden, deshalb kann das Spiel nicht mit ihnen starten.\n\n'
        'Datei: {}\nOrdner: {}\nWindows meldet: {}\n\nSchaffe Platz auf diesem Laufwerk oder verschiebe den Port in '
        'einen Ordner, in den du schreiben darfst (etwa C:\\Games\\BBPort).',
        'Im Spielordner fehlt {}: Das Spiel wurde nicht vollständig entpackt. Entpacke das Grundspiel erneut in einen '
        'neuen, leeren Ordner und kopiere das Update 1.09 darüber.',
        'Spielstände: Ein Spielstand in {} konnte nicht dorthin kopiert werden, wo das Spiel ihn liest: {}',
        'Spielstände: {} wurde dorthin kopiert, wo das Spiel sie liest ({}); das Original bleibt, wo es war.',
        "The Hunter's Dream hat die Anmeldung dieses PCs abgelehnt: Melde dich auf der Seite Online spielen erneut an.",
        'Das Datum deines PCs ist falsch ({}), deshalb schlagen sichere Verbindungen fehl. Stelle das Datum in Windows '
        'ein: Einstellungen > Zeit und Sprache > Datum und Uhrzeit, „Uhrzeit automatisch festlegen“.',
        'Der Spielstand-Ordner {} ist nicht da: Sein Laufwerk ist nicht angeschlossen, oder der Ordner wurde umbenannt '
        'oder verschoben. Schließe das Laufwerk an oder wähle den Spielstand-Ordner erneut.',
        'Der Port läuft aus einem temporären Ordner ({}), wie beim Start direkt aus einer ZIP- oder RAR-Datei. Dieser '
        'Ordner wird gelöscht, wenn das Archiv geschlossen wird, und deine Spielstände mit ihm.\n\nEntpacke den Port '
        'zuerst in einen Ordner (etwa C:\\Games\\BBPort) und starte Bloodborne.exe von dort.',
    ],
    'es': [
        'El juego ya está en marcha (iniciado desde Steam, un acceso directo o el reinicio dentro del juego): su '
        'ventana se ha traído al frente.',
        'Algo ha fallado en el lanzador: {}',
        'Los detalles están en la página Registro. Ayuda > «Guardar un informe» crea el zip que debes adjuntar cuando '
        'pidas ayuda.',
        'No se pudo guardar tu configuración, así que el juego no puede iniciarse con ella.\n\nArchivo: {}\n'
        'Carpeta: {}\nWindows dice: {}\n\nLibera espacio en esa unidad o mueve el port a una carpeta en la que puedas '
        'escribir (como C:\\Games\\BBPort).',
        'A la carpeta del juego le falta {}: el juego no se descomprimió por completo. Vuelve a descomprimir el juego '
        'base en una carpeta nueva y vacía y copia encima la actualización 1.09.',
        'Partidas: no se pudo copiar una partida de {} adonde el juego la lee: {}',
        'Partidas: {} copiadas adonde el juego las lee ({}); el original se queda donde estaba.',
        "The Hunter's Dream rechazó el inicio de sesión de este PC: vuelve a iniciar sesión en la página Juego en "
        'línea.',
        'La fecha de tu PC es incorrecta ({}), por eso fallan las conexiones seguras. Ajusta la fecha en Windows: '
        'Configuración > Hora e idioma > Fecha y hora, «Establecer la hora automáticamente».',
        'La carpeta de partidas {} no está: su unidad no está conectada, o la carpeta se renombró o se movió. Conecta '
        'la unidad o vuelve a elegir la carpeta de partidas.',
        'El port se ejecuta desde una carpeta temporal ({}), como cuando se inicia desde dentro de un archivo zip o '
        'rar. Esa carpeta se borra al cerrar el archivo, y tus partidas con ella.\n\nPrimero extrae el port a una '
        'carpeta (como C:\\Games\\BBPort) e inicia Bloodborne.exe desde allí.',
    ],
    'pt': [
        'O jogo já está rodando (iniciado pela Steam, por um atalho ou pelo reinício dentro do jogo): a janela dele '
        'foi trazida para a frente.',
        'Algo deu errado no launcher: {}',
        'Os detalhes estão na página Registro. Ajuda > "Salvar um relatório" cria o zip para anexar quando você pedir '
        'ajuda.',
        'Não foi possível salvar suas configurações, então o jogo não pode iniciar com elas.\n\nArquivo: {}\n'
        'Pasta: {}\nO Windows diz: {}\n\nLibere espaço nessa unidade ou mova o port para uma pasta em que você possa '
        'gravar (como C:\\Games\\BBPort).',
        'Falta {} na pasta do jogo: o jogo não foi extraído por completo. Extraia o jogo base de novo numa pasta nova '
        'e vazia e copie a atualização 1.09 por cima.',
        'Saves: não foi possível copiar um save de {} para onde o jogo o lê: {}',
        'Saves: {} copiados para onde o jogo os lê ({}); o original fica onde estava.',
        "The Hunter's Dream recusou o login deste PC: entre de novo na página Jogo online.",
        'A data do seu PC está errada ({}), por isso as conexões seguras falham. Acerte a data no Windows: '
        'Configurações > Hora e idioma > Data e hora, "Definir horário automaticamente".',
        'A pasta de saves {} não está lá: a unidade dela não está conectada, ou a pasta foi renomeada ou movida. '
        'Conecte a unidade ou escolha a pasta de saves de novo.',
        'O port está rodando de uma pasta temporária ({}), como quando é iniciado de dentro de um arquivo zip ou rar. '
        'Essa pasta é apagada quando o arquivo é fechado, e seus saves junto.\n\nPrimeiro extraia o port para uma '
        'pasta (como C:\\Games\\BBPort) e inicie o Bloodborne.exe de lá.',
    ],
    'fr': [
        'Le jeu tourne déjà (lancé depuis Steam, un raccourci ou le redémarrage dans le jeu) : sa fenêtre a été '
        'ramenée au premier plan.',
        'Une erreur s’est produite dans le lanceur : {}',
        'Les détails sont sur la page Journal. Aide > « Enregistrer un rapport » crée le zip à joindre quand vous '
        'demandez de l’aide.',
        'Vos réglages n’ont pas pu être enregistrés, le jeu ne peut donc pas démarrer avec eux.\n\nFichier : {}\n'
        'Dossier : {}\nWindows indique : {}\n\nLibérez de la place sur ce lecteur, ou déplacez le port dans un '
        'dossier où vous pouvez écrire (comme C:\\Games\\BBPort).',
        'Le dossier du jeu n’a pas de {} : le jeu n’a pas été décompressé entièrement. Décompressez à nouveau le jeu '
        'de base dans un nouveau dossier vide et copiez la mise à jour 1.09 par-dessus.',
        'Sauvegardes : une sauvegarde de {} n’a pas pu être copiée là où le jeu la lit : {}',
        'Sauvegardes : {} copiées là où le jeu les lit ({}) ; l’original reste où il était.',
        "The Hunter's Dream a refusé la connexion de ce PC : reconnectez-vous sur la page Jeu en ligne.",
        'La date de votre PC est fausse ({}), les connexions sécurisées échouent donc. Réglez la date dans Windows : '
        'Paramètres > Heure et langue > Date et heure, « Régler l’heure automatiquement ».',
        'Le dossier de sauvegardes {} n’est pas là : son lecteur n’est pas branché, ou le dossier a été renommé ou '
        'déplacé. Branchez le lecteur ou choisissez à nouveau le dossier de sauvegardes.',
        'Le port tourne depuis un dossier temporaire ({}), comme lorsqu’il est lancé depuis l’intérieur d’un fichier '
        'zip ou rar. Ce dossier est supprimé à la fermeture de l’archive, et vos sauvegardes avec.\n\nExtrayez '
        'd’abord le port dans un dossier (comme C:\\Games\\BBPort) et lancez Bloodborne.exe depuis là.',
    ],
    'it': [
        'Il gioco è già in esecuzione (avviato da Steam, da un collegamento o dal riavvio nel gioco): la sua finestra '
        'è stata portata in primo piano.',
        'Qualcosa è andato storto nel launcher: {}',
        'I dettagli sono nella pagina Registro. Aiuto > "Salva un rapporto" crea lo zip da allegare quando chiedi '
        'aiuto.',
        'Non è stato possibile salvare le impostazioni, quindi il gioco non può avviarsi con esse.\n\nFile: {}\n'
        'Cartella: {}\nWindows dice: {}\n\nLibera spazio su quell’unità o sposta il port in una cartella in cui puoi '
        'scrivere (come C:\\Games\\BBPort).',
        'Nella cartella del gioco manca {}: il gioco non è stato estratto del tutto. Estrai di nuovo il gioco base in '
        'una cartella nuova e vuota e copiaci sopra l’aggiornamento 1.09.',
        'Salvataggi: non è stato possibile copiare un salvataggio da {} dove il gioco lo legge: {}',
        'Salvataggi: {} copiati dove il gioco li legge ({}); l’originale resta dov’era.',
        "The Hunter's Dream ha rifiutato l’accesso di questo PC: accedi di nuovo nella pagina Gioco online.",
        'La data del tuo PC è sbagliata ({}), quindi le connessioni sicure non riescono. Imposta la data in Windows: '
        'Impostazioni > Data/ora e lingua > Data e ora, "Imposta ora automaticamente".',
        'La cartella dei salvataggi {} non c’è: la sua unità non è collegata, oppure la cartella è stata rinominata o '
        'spostata. Collega l’unità o scegli di nuovo la cartella dei salvataggi.',
        'Il port è in esecuzione da una cartella temporanea ({}), come quando viene avviato dall’interno di un file zip '
        'o rar. Quella cartella viene eliminata quando l’archivio si chiude, e i tuoi salvataggi con essa.\n\nPrima '
        'estrai il port in una cartella (come C:\\Games\\BBPort) e avvia Bloodborne.exe da lì.',
    ],
    'pl': [
        'Gra już działa (uruchomiona ze Steama, ze skrótu lub po restarcie w grze): jej okno zostało przeniesione na '
        'wierzch.',
        'W launcherze wystąpił błąd: {}',
        'Szczegóły są na stronie Dziennik. Pomoc > „Zapisz raport” tworzy plik zip, który należy dołączyć, prosząc o '
        'pomoc.',
        'Nie udało się zapisać ustawień, więc gra nie może się z nimi uruchomić.\n\nPlik: {}\nFolder: {}\n'
        'Windows zgłasza: {}\n\nZwolnij miejsce na tym dysku albo przenieś port do folderu, w którym możesz zapisywać '
        '(np. C:\\Games\\BBPort).',
        'W folderze gry brakuje {}: gra nie została rozpakowana w całości. Rozpakuj grę podstawową ponownie do nowego, '
        'pustego folderu i skopiuj na nią aktualizację 1.09.',
        'Zapisy: nie udało się skopiować zapisu z {} tam, gdzie gra go czyta: {}',
        'Zapisy: {} skopiowano tam, gdzie gra je czyta ({}); oryginał został na miejscu.',
        "The Hunter's Dream odrzucił logowanie tego komputera: zaloguj się ponownie na stronie Gra online.",
        'Data na twoim komputerze jest błędna ({}), dlatego bezpieczne połączenia się nie udają. Ustaw datę w Windows: '
        'Ustawienia > Czas i język > Data i godzina, „Ustaw czas automatycznie”.',
        'Folderu zapisów {} nie ma: jego dysk nie jest podłączony albo folder zmienił nazwę lub miejsce. Podłącz dysk '
        'albo wybierz folder zapisów ponownie.',
        'Port działa z folderu tymczasowego ({}), jak przy uruchomieniu prosto z pliku zip lub rar. Ten folder jest '
        'usuwany po zamknięciu archiwum, a razem z nim twoje zapisy.\n\nNajpierw rozpakuj port do folderu (np. '
        'C:\\Games\\BBPort) i uruchamiaj Bloodborne.exe stamtąd.',
    ],
    'tr': [
        'Oyun zaten çalışıyor (Steam’den, bir kısayoldan ya da oyun içi yeniden başlatmayla açıldı): penceresi öne '
        'getirildi.',
        'Başlatıcıda bir sorun oluştu: {}',
        'Ayrıntılar Günlük sayfasında. Yardım > "Rapor kaydet", yardım isterken ekleyeceğiniz zip dosyasını oluşturur.',
        'Ayarlarınız kaydedilemedi, bu yüzden oyun onlarla başlayamıyor.\n\nDosya: {}\nKlasör: {}\nWindows diyor ki: '
        '{}\n\nO sürücüde yer açın ya da portu yazabildiğiniz bir klasöre taşıyın (örneğin C:\\Games\\BBPort).',
        'Oyun klasöründe {} yok: oyun tam olarak çıkarılmamış. Ana oyunu yeni, boş bir klasöre yeniden çıkarın ve '
        '1.09 güncellemesini üzerine kopyalayın.',
        'Kayıtlar: {} içindeki bir kayıt oyunun okuduğu yere kopyalanamadı: {}',
        'Kayıtlar: {} oyunun okuduğu yere kopyalandı ({}); asıl kayıt yerinde kalıyor.',
        "The Hunter's Dream bu bilgisayarın oturum açmasını reddetti: Çevrim içi oyun sayfasında yeniden oturum açın.",
        'Bilgisayarınızın tarihi yanlış ({}), bu yüzden güvenli bağlantılar başarısız oluyor. Tarihi Windows’ta '
        'ayarlayın: Ayarlar > Saat ve dil > Tarih ve saat, "Saati otomatik olarak ayarla".',
        '{} kayıt klasörü yok: sürücüsü bağlı değil ya da klasörün adı veya yeri değişti. Sürücüyü bağlayın ya da kayıt '
        'klasörünü yeniden seçin.',
        'Port geçici bir klasörden çalışıyor ({}); bir zip ya da rar dosyasının içinden başlatıldığında olduğu gibi. Bu '
        'klasör arşiv kapanınca silinir, kayıtlarınız da onunla birlikte.\n\nÖnce portu bir klasöre çıkarın (örneğin '
        'C:\\Games\\BBPort) ve Bloodborne.exe’yi oradan başlatın.',
    ],
    'ar': [
        'اللعبة تعمل بالفعل (بدأت من Steam أو من اختصار أو من إعادة التشغيل داخل اللعبة): تم إحضار نافذتها إلى الأمام.',
        'حدث خطأ في المشغّل: {}',
        'التفاصيل في صفحة السجل. المساعدة > "حفظ تقرير" ينشئ ملف zip لإرفاقه عندما تطلب المساعدة.',
        'تعذّر حفظ إعداداتك، لذلك لا يمكن للعبة أن تبدأ بها.\n\nالملف: {}\nالمجلد: {}\nيقول Windows: {}\n\nأفرغ مساحة '
        'على ذلك القرص، أو انقل المنفذ إلى مجلد يمكنك الكتابة فيه (مثل C:\\Games\\BBPort).',
        'مجلد اللعبة لا يحتوي على {}: لم يُفك ضغط اللعبة بالكامل. فك ضغط اللعبة الأساسية من جديد في مجلد جديد وفارغ '
        'وانسخ تحديث 1.09 فوقها.',
        'ملفات الحفظ: تعذّر نسخ ملف حفظ من {} إلى حيث تقرؤه اللعبة: {}',
        'ملفات الحفظ: تم نسخ {} إلى حيث تقرؤها اللعبة ({})؛ يبقى الأصل في مكانه.',
        "رفض The Hunter's Dream تسجيل دخول هذا الحاسوب: سجّل الدخول من جديد في صفحة اللعب عبر الإنترنت.",
        'تاريخ حاسوبك غير صحيح ({})، لذلك تفشل الاتصالات الآمنة. اضبط التاريخ في Windows: الإعدادات > الوقت واللغة > '
        'التاريخ والوقت، "ضبط الوقت تلقائيًا".',
        'مجلد الحفظ {} غير موجود: قرصه غير موصول، أو أُعيدت تسمية المجلد أو نُقل. صِل القرص، أو اختر مجلد الحفظ من جديد.',
        'المنفذ يعمل من مجلد مؤقت ({})، كما يحدث عند تشغيله من داخل ملف zip أو rar. يُحذف هذا المجلد عند إغلاق الأرشيف، '
        'ومعه ملفات حفظك.\n\nفك ضغط المنفذ إلى مجلد أولًا (مثل C:\\Games\\BBPort) وشغّل Bloodborne.exe من هناك.',
    ],
    'zh': [
        '游戏已在运行（从 Steam、快捷方式或游戏内重启启动）：已将其窗口移到最前。',
        '启动器出错：{}',
        '详细信息在“日志”页面。“帮助”>“保存报告”会生成求助时需要附上的 zip 文件。',
        '无法保存你的设置，因此游戏无法使用这些设置启动。\n\n文件：{}\n文件夹：{}\nWindows 提示：{}\n\n请在该驱动器上腾出空间，'
        '或将移植版移到你有写入权限的文件夹（例如 C:\\Games\\BBPort）。',
        '游戏文件夹中缺少 {}：游戏没有完整解包。请将本体游戏重新解包到一个新的空文件夹，再把 1.09 更新复制覆盖上去。',
        '存档：无法将 {} 中的存档复制到游戏读取的位置：{}',
        '存档：已将 {} 复制到游戏读取的位置（{}）；原件保留在原处。',
        "The Hunter's Dream 拒绝了这台电脑的登录：请在“在线游玩”页面重新登录。",
        '你的电脑日期不正确（{}），因此安全连接失败。请在 Windows 中设置日期：设置 > 时间和语言 > 日期和时间，“自动设置时间”。',
        '存档文件夹 {} 不在：它所在的驱动器未连接，或文件夹已被重命名或移动。请连接该驱动器，或重新选择存档文件夹。',
        '移植版正在从临时文件夹运行（{}），就像直接从 zip 或 rar 压缩包中启动一样。压缩包关闭时该文件夹会被删除，你的存档也会随之'
        '消失。\n\n请先将移植版解压到一个文件夹（例如 C:\\Games\\BBPort），再从那里启动 Bloodborne.exe。',
    ],
    'ja': [
        'ゲームはすでに起動しています（Steam、ショートカット、またはゲーム内の再起動から）：そのウィンドウを前面に表示しました。',
        'ランチャーでエラーが発生しました：{}',
        '詳細はログのページにあります。ヘルプ >「レポートを保存」で、助けを求めるときに添付する zip が作成されます。',
        '設定を保存できなかったため、ゲームはその設定で起動できません。\n\nファイル：{}\nフォルダー：{}\nWindows の'
        'メッセージ：{}\n\nそのドライブの空き容量を増やすか、書き込みできるフォルダー（例：C:\\Games\\BBPort）に移植版を'
        '移動してください。',
        'ゲームフォルダーに {} がありません：ゲームが完全に展開されていません。ベースゲームを新しい空のフォルダーに展開し直し、'
        'その上に 1.09 アップデートをコピーしてください。',
        'セーブデータ：{} のセーブデータをゲームが読む場所にコピーできませんでした：{}',
        'セーブデータ：{} をゲームが読む場所にコピーしました（{}）。元のデータはそのまま残ります。',
        "The Hunter's Dream がこの PC のサインインを拒否しました：オンラインプレイのページでもう一度サインインしてください。",
        'PC の日付が正しくありません（{}）。そのため安全な接続に失敗します。Windows で日付を設定してください：設定 > 時刻と'
        '言語 > 日付と時刻、「時刻を自動的に設定する」。',
        'セーブフォルダー {} が見つかりません：ドライブが接続されていないか、フォルダーの名前や場所が変わりました。ドライブを'
        '接続するか、セーブフォルダーを選び直してください。',
        '移植版は一時フォルダー（{}）から実行されています。zip や rar ファイルの中から直接起動したときと同じです。このフォルダーは'
        'アーカイブを閉じると削除され、セーブデータも一緒に消えます。\n\n先に移植版をフォルダー（例：C:\\Games\\BBPort）に'
        '展開し、そこから Bloodborne.exe を起動してください。',
    ],
    'ko': [
        '게임이 이미 실행 중입니다(Steam, 바로 가기 또는 게임 내 재시작으로 시작됨): 게임 창을 앞으로 가져왔습니다.',
        '런처에 오류가 발생했습니다: {}',
        '자세한 내용은 로그 페이지에 있습니다. 도움말 > "보고서 저장"은 도움을 요청할 때 첨부할 zip을 만듭니다.',
        '설정을 저장할 수 없어서 게임을 이 설정으로 시작할 수 없습니다.\n\n파일: {}\n폴더: {}\nWindows 메시지: {}\n\n'
        '해당 드라이브의 공간을 확보하거나, 쓰기 권한이 있는 폴더(예: C:\\Games\\BBPort)로 포트를 옮기세요.',
        '게임 폴더에 {}이(가) 없습니다: 게임 압축이 완전히 풀리지 않았습니다. 기본 게임을 새 빈 폴더에 다시 풀고 그 위에 '
        '1.09 업데이트를 복사하세요.',
        '세이브: {}의 세이브를 게임이 읽는 곳으로 복사하지 못했습니다: {}',
        '세이브: {}을(를) 게임이 읽는 곳으로 복사했습니다({}). 원본은 그대로 남아 있습니다.',
        "The Hunter's Dream이 이 PC의 로그인을 거부했습니다: 온라인 플레이 페이지에서 다시 로그인하세요.",
        'PC의 날짜가 잘못되었습니다({}). 그래서 보안 연결이 실패합니다. Windows에서 날짜를 설정하세요: 설정 > 시간 및 언어 > '
        '날짜 및 시간, "자동으로 시간 설정".',
        '세이브 폴더 {}이(가) 없습니다: 드라이브가 연결되어 있지 않거나 폴더 이름이 바뀌었거나 옮겨졌습니다. 드라이브를 '
        '연결하거나 세이브 폴더를 다시 선택하세요.',
        '포트가 임시 폴더({})에서 실행되고 있습니다. zip 또는 rar 파일 안에서 바로 실행했을 때처럼요. 이 폴더는 압축 파일을 '
        '닫으면 삭제되며, 세이브도 함께 사라집니다.\n\n먼저 포트를 폴더(예: C:\\Games\\BBPort)에 풀고 거기서 '
        'Bloodborne.exe를 실행하세요.',
    ],
}

for _language, _texts in MISTAKES.items():
    if len(_texts) != len(MISTAKE_KEYS):
        raise ValueError(f'bbport_lang_mistakes {_language}: {len(_texts)} texts for {len(MISTAKE_KEYS)} keys')
    bbport_lang.EXTRA_TRANSLATIONS.setdefault(_language, {}).update(zip(MISTAKE_KEYS, _texts))

# The mods list's hint beside a mod extracted bare into the mods folder (mods\dvdroot_ps4).
BARE_DVDROOT = 'Put this in a folder of its own (e.g. mods\\<mod name>\\dvdroot_ps4)'
BARE_DVDROOT_TEXTS = {
    'de': 'In einen eigenen Ordner legen (z. B. mods\\<Mod-Name>\\dvdroot_ps4)',
    'es': 'Ponlo en una carpeta propia (p. ej. mods\\<nombre del mod>\\dvdroot_ps4)',
    'pt': 'Coloque-o numa pasta própria (ex.: mods\\<nome do mod>\\dvdroot_ps4)',
    'fr': 'Mets-le dans un dossier à part (ex. mods\\<nom du mod>\\dvdroot_ps4)',
    'it': 'Mettilo in una cartella a parte (es. mods\\<nome della mod>\\dvdroot_ps4)',
    'pl': 'Umieść go we własnym folderze (np. mods\\<nazwa moda>\\dvdroot_ps4)',
    'tr': 'Bunu kendi klasörüne koy (ör. mods\\<mod adı>\\dvdroot_ps4)',
    'ar': 'ضعه في مجلد خاص به (مثل mods\\<اسم التعديل>\\dvdroot_ps4)',
    'zh': '请把它放进单独的文件夹（例如 mods\\<模组名>\\dvdroot_ps4）',
    'ja': '専用のフォルダーに入れてください（例: mods\\<MOD名>\\dvdroot_ps4）',
    'ko': '별도의 폴더에 넣으세요 (예: mods\\<모드 이름>\\dvdroot_ps4)',
}
for _language, _text in BARE_DVDROOT_TEXTS.items():
    bbport_lang.EXTRA_TRANSLATIONS.setdefault(_language, {})[BARE_DVDROOT] = _text
