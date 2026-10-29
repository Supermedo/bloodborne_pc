# SPDX-License-Identifier: GPL-2.0-or-later
"""Localized texts of the hardware check: laptop processors, the processor as the limit, and the Your PC
page while the game tests are off."""

KEYS = (
    'Laptop processor: counted lower than the desktop chip of its generation (it holds lower clocks under '
    'long load).',
    'Your processor sets the limit: a steady cap at what it is expected to hold. Lower graphics settings would '
    'not raise the frame rate.',
    'What your PC can do, rated like the hardware check on bbport.de, and the system requirements.',
    'Your processor has no AVX: the port cannot run on it.',
    'Your processor lacks {}: the port translates them (compatibility mode), so expect bugs.',
    'Your processor has AVX, BMI1, MOVBE and LZCNT: the port runs on it directly.',
)
TEXTS = {
    'en': KEYS,
    'de': (
        'Laptop-Prozessor: zählt niedriger als der Desktop-Chip seiner Generation (er hält unter langer Last '
        'weniger Takt).',
        'Dein Prozessor setzt die Grenze: eine feste Grenze bei dem, was er voraussichtlich hält. Niedrigere '
        'Grafikeinstellungen würden die Bildrate nicht erhöhen.',
        'Was dein PC kann, bewertet wie der Hardware-Check auf bbport.de, und die Systemanforderungen.',
        'Dein Prozessor hat kein AVX: Der Port läuft darauf nicht.',
        'Deinem Prozessor fehlt {}: Der Port übersetzt das (Kompatibilitätsmodus), rechne mit Fehlern.',
        'Dein Prozessor hat AVX, BMI1, MOVBE und LZCNT: Der Port läuft darauf direkt.',
    ),
    'ru': (
        'Процессор ноутбука: оценивается ниже настольного чипа того же поколения (под долгой нагрузкой он '
        'держит более низкие частоты).',
        'Предел задаёт ваш процессор: ровное ограничение на уровне, который он должен удерживать. Более низкие '
        'настройки графики не повысят частоту кадров.',
        'Возможности вашего ПК, оценённые так же, как в проверке оборудования на bbport.de, и системные требования.',
        'У вашего процессора нет AVX: порт на нём не запустится.',
        'Вашему процессору не хватает {}: порт их транслирует (режим совместимости), возможны ошибки.',
        'У вашего процессора есть AVX, BMI1, MOVBE и LZCNT: порт работает на нём напрямую.',
    ),
    'ar': (
        'معالج حاسوب محمول: يُحتسب أقل من معالج سطح المكتب من الجيل نفسه (فهو يحافظ على تردد أقل تحت الحمل '
        'الطويل).',
        'المعالج هو العامل المحدِّد: حدّ ثابت عند ما يُتوقع أن يحافظ عليه. خفض إعدادات الرسوميات لن يرفع معدل '
        'الإطارات.',
        'قدرات حاسوبك، مقيّمة مثل فحص العتاد على bbport.de، ومتطلبات النظام.',
        'معالجك لا يدعم AVX: لا يمكن تشغيل المنفذ عليه.',
        'معالجك يفتقد {}: يترجمها المنفذ (وضع التوافق)، فتوقّع أخطاء.',
        'معالجك يدعم AVX وBMI1 وMOVBE وLZCNT: يعمل المنفذ عليه مباشرة.',
    ),
    'es': (
        'Procesador de portátil: cuenta menos que el chip de escritorio de su generación (mantiene frecuencias '
        'más bajas con carga prolongada).',
        'El límite lo pone tu procesador: un tope fijo en lo que se espera que mantenga. Bajar los ajustes '
        'gráficos no subiría la tasa de fotogramas.',
        'Lo que puede hacer tu PC, valorado como en la comprobación de hardware de bbport.de, y los '
        'requisitos del sistema.',
        'Tu procesador no tiene AVX: el port no puede ejecutarse en él.',
        'A tu procesador le falta {}: el port lo traduce (modo de compatibilidad), así que espera errores.',
        'Tu procesador tiene AVX, BMI1, MOVBE y LZCNT: el port se ejecuta en él directamente.',
    ),
    'pt': (
        'Processador de notebook: conta menos que o chip de desktop da mesma geração (mantém frequências mais '
        'baixas sob carga longa).',
        'Seu processador é o limite: um teto fixo no que ele deve manter. Configurações gráficas mais baixas não '
        'aumentariam a taxa de quadros.',
        'O que o seu PC consegue fazer, avaliado como na verificação de hardware do bbport.de, e os '
        'requisitos do sistema.',
        'Seu processador não tem AVX: o port não roda nele.',
        'Falta {} no seu processador: o port traduz isso (modo de compatibilidade), então espere erros.',
        'Seu processador tem AVX, BMI1, MOVBE e LZCNT: o port roda nele diretamente.',
    ),
    'fr': (
        'Processeur de portable : compté plus bas que la puce de bureau de sa génération (il tient des fréquences '
        'plus basses sous une charge prolongée).',
        'Votre processeur fixe la limite : un plafond stable à ce qu\'il devrait tenir. Des réglages graphiques '
        'plus bas n\'augmenteraient pas la fréquence d\'images.',
        'Ce dont votre PC est capable, évalué comme dans la vérification du matériel de bbport.de, et la '
        'configuration requise.',
        "Votre processeur n'a pas AVX : le port ne peut pas s'exécuter dessus.",
        'Il manque {} à votre processeur : le port les traduit (mode de compatibilité), attendez-vous à des bugs.',
        "Votre processeur a AVX, BMI1, MOVBE et LZCNT : le port s'exécute directement dessus.",
    ),
    'it': (
        'Processore per portatile: conta meno del chip desktop della sua generazione (sotto carico prolungato '
        'mantiene frequenze più basse).',
        'Il limite è il tuo processore: un tetto fisso a ciò che dovrebbe reggere. Impostazioni grafiche più '
        'basse non alzerebbero il frame rate.',
        'Cosa può fare il tuo PC, valutato come nella verifica hardware su bbport.de, e i requisiti di sistema.',
        'Il tuo processore non ha AVX: il port non può funzionare.',
        'Al tuo processore manca {}: il port le traduce (modalità compatibilità), quindi aspettati errori.',
        'Il tuo processore ha AVX, BMI1, MOVBE e LZCNT: il port funziona direttamente.',
    ),
    'pl': (
        'Procesor laptopa: liczony niżej niż desktopowy układ tej samej generacji (pod długim obciążeniem '
        'utrzymuje niższe taktowanie).',
        'Limit wyznacza twój procesor: stały limit na poziomie, który powinien utrzymać. Niższe ustawienia '
        'grafiki nie podniosłyby liczby klatek.',
        'Co potrafi twój PC, z oceną jak w teście sprzętu na bbport.de, oraz wymagania systemowe.',
        'Twój procesor nie ma AVX: port na nim nie zadziała.',
        'Twojemu procesorowi brakuje {}: port je tłumaczy (tryb zgodności), więc licz się z błędami.',
        'Twój procesor ma AVX, BMI1, MOVBE i LZCNT: port działa na nim bezpośrednio.',
    ),
    'tr': (
        'Dizüstü işlemcisi: kendi neslinin masaüstü yongasından daha düşük sayılır (uzun yük altında daha düşük '
        'saat hızında kalır).',
        'Sınırı işlemciniz belirliyor: tutması beklenen değerde sabit bir sınır. Daha düşük grafik ayarları kare '
        'hızını artırmaz.',
        'Bilgisayarınızın neler yapabildiği (bbport.de’deki donanım kontrolü gibi derecelendirilir) ve '
        'sistem gereksinimleri.',
        'İşlemcinizde AVX yok: port bu işlemcide çalışamaz.',
        'İşlemcinizde {} eksik: port bunları çeviriyor (uyumluluk modu), hatalar bekleyin.',
        'İşlemcinizde AVX, BMI1, MOVBE ve LZCNT var: port doğrudan çalışır.',
    ),
    'zh': (
        '笔记本处理器：评分低于同代台式机芯片（长时间负载下频率更低）。',
        '瓶颈在于你的处理器：帧率上限固定在它预计能保持的水平。降低画面设置不会提高帧率。',
        '你的电脑性能如何：按 bbport.de 硬件检测的标准评级，以及系统要求。',
        '你的处理器不支持 AVX：移植版无法在其上运行。',
        '你的处理器缺少 {}：移植版会转译这些指令（兼容模式），可能会出错。',
        '你的处理器支持 AVX、BMI1、MOVBE 和 LZCNT：移植版可直接运行。',
    ),
    'ja': (
        'ノート PC 用プロセッサー：同世代のデスクトップ用チップより低く評価されます（長時間の負荷ではクロックが'
        '下がるため）。',
        'プロセッサーが上限を決めています：維持できる見込みのフレームレートで固定します。グラフィック設定を'
        '下げてもフレームレートは上がりません。',
        'bbport.de のハードウェアチェックと同じ基準で評価した PC の性能と、必要なシステム構成。',
        'お使いのプロセッサーは AVX に対応していません：この移植版は動作しません。',
        'お使いのプロセッサーには {} がありません：移植版が変換して実行します（互換モード）。不具合が起きる可能性があります。',
        'お使いのプロセッサーは AVX、BMI1、MOVBE、LZCNT に対応しています：移植版はそのまま動作します。',
    ),
    'ko': (
        '노트북 프로세서: 같은 세대의 데스크톱 칩보다 낮게 평가됩니다(오랜 부하에서 클럭이 더 낮게 유지됩니다).',
        '프로세서가 성능의 한계입니다: 유지할 수 있을 것으로 예상되는 수준에서 고정합니다. 그래픽 설정을 '
        '낮춰도 프레임 레이트는 오르지 않습니다.',
        'bbport.de의 하드웨어 검사와 같은 방식으로 평가한 PC 성능과 시스템 요구 사항.',
        '프로세서가 AVX를 지원하지 않습니다: 이 포트를 실행할 수 없습니다.',
        '프로세서에 {}이(가) 없습니다: 포트가 이를 변환합니다(호환 모드). 오류가 생길 수 있습니다.',
        '프로세서가 AVX, BMI1, MOVBE, LZCNT를 지원합니다: 포트가 바로 실행됩니다.',
    ),
}
TRANSLATIONS = {language: dict(zip(KEYS, texts, strict=True)) for language, texts in TEXTS.items()}
