# SPDX-License-Identifier: GPL-3.0-or-later
"""Read-only, path-free diagnostics for the four FSR 4.1 routes."""
import re
import bbport_fsr41 as model
import bbport_lang

KEYS = ('Show details', 'FSR 4.1 diagnostics', 'Copy details', 'Copied',
        'GPU', 'Driver', 'Updated', 'Passed', 'Failed', 'Not checked', 'Information',
        'Preflight check. The active provider is recorded in the game log.',
        'Probe', 'Runtime file', 'Library loading', 'API exports', 'GPU matching',
        'Device creation', 'Queue', 'Shared resources', 'Provider query', 'Provider version',
        'Provider GPU check', 'Provider', 'Vulkan features', 'Package files', 'Result', 'Reason')
TEXTS = {
 'en': KEYS,
 'de': ('Details anzeigen', 'FSR-4.1-Diagnose', 'Details kopieren', 'Kopiert',
    'GPU', 'Treiber', 'Aktualisiert', 'Bestanden', 'Fehlgeschlagen', 'Nicht geprüft', 'Information',
    'Vorprüfung. Der aktive Provider steht im Spielprotokoll.',
    'Prüfprogramm', 'Laufzeitdatei', 'Bibliothek laden', 'API-Funktionen', 'GPU-Zuordnung',
    'Gerät erstellen', 'Queue', 'Geteilte Ressourcen', 'Provider-Abfrage', 'Provider-Version',
    'GPU-Prüfung des Providers', 'Provider', 'Vulkan-Funktionen', 'Paketdateien', 'Ergebnis', 'Ursache'),
 'ru': ('Подробнее', 'Диагностика FSR 4.1', 'Копировать сведения', 'Скопировано',
    'GPU', 'Драйвер', 'Обновлено', 'Пройдено', 'Ошибка', 'Не проверено', 'Информация',
    'Предварительная проверка. Активный провайдер указан в журнале игры.',
    'Проверочная программа', 'Файл среды', 'Загрузка библиотеки', 'Функции API', 'Сопоставление GPU',
    'Создание устройства', 'Очередь', 'Общие ресурсы', 'Запрос провайдера', 'Версия провайдера',
    'Проверка GPU провайдером', 'Провайдер', 'Возможности Vulkan', 'Файлы пакета', 'Результат', 'Причина'),
 'ar': ('عرض التفاصيل', 'تشخيص FSR 4.1', 'نسخ التفاصيل', 'تم النسخ',
    'البطاقة', 'التعريف', 'آخر تحديث', 'نجح', 'فشل', 'لم يُفحص', 'معلومات',
    'فحص مسبق. يُسجَّل المزوّد النشط في سجل اللعبة.',
    'برنامج الفحص', 'ملف التشغيل', 'تحميل المكتبة', 'دوال API', 'مطابقة البطاقة',
    'إنشاء الجهاز', 'الطابور', 'الموارد المشتركة', 'استعلام المزوّد', 'إصدار المزوّد',
    'فحص البطاقة لدى المزوّد', 'المزوّد', 'ميزات Vulkan', 'ملفات الحزمة', 'النتيجة', 'السبب'),
 'es': ('Ver detalles', 'Diagnóstico de FSR 4.1', 'Copiar detalles', 'Copiado',
    'GPU', 'Controlador', 'Actualizado', 'Correcto', 'Error', 'Sin comprobar', 'Información',
    'Comprobación previa. El proveedor activo figura en el registro del juego.',
    'Programa de prueba', 'Archivo de ejecución', 'Carga de biblioteca', 'Funciones API', 'Identificación de GPU',
    'Creación del dispositivo', 'Cola', 'Recursos compartidos', 'Consulta del proveedor', 'Versión del proveedor',
    'Prueba de GPU del proveedor', 'Proveedor', 'Funciones Vulkan', 'Archivos del paquete', 'Resultado', 'Motivo'),
 'pt': ('Ver detalhes', 'Diagnóstico FSR 4.1', 'Copiar detalhes', 'Copiado',
    'GPU', 'Driver', 'Atualizado', 'Aprovado', 'Falhou', 'Não verificado', 'Informação',
    'Verificação prévia. O provedor ativo consta no log do jogo.',
    'Programa de teste', 'Arquivo de execução', 'Carregamento da biblioteca', 'Funções API', 'Identificação da GPU',
    'Criação do dispositivo', 'Fila', 'Recursos compartilhados', 'Consulta do provedor', 'Versão do provedor',
    'Verificação de GPU do provedor', 'Provedor', 'Recursos Vulkan', 'Arquivos do pacote', 'Resultado', 'Motivo'),
 'fr': ('Voir les détails', 'Diagnostic FSR 4.1', 'Copier les détails', 'Copié',
    'GPU', 'Pilote', 'Mis à jour', 'Réussi', 'Échec', 'Non vérifié', 'Information',
    'Vérification préalable. Le fournisseur actif figure dans le journal du jeu.',
    'Programme de test', 'Fichier du runtime', 'Chargement de bibliothèque', 'Fonctions API', 'Identification du GPU',
    'Création du périphérique', 'File', 'Ressources partagées', 'Requête du fournisseur', 'Version du fournisseur',
    'Vérification GPU du fournisseur', 'Fournisseur', 'Fonctions Vulkan', 'Fichiers du paquet', 'Résultat', 'Cause'),
 'it': ('Mostra dettagli', 'Diagnostica FSR 4.1', 'Copia dettagli', 'Copiato',
    'GPU', 'Driver', 'Aggiornato', 'Superato', 'Non riuscito', 'Non verificato', 'Informazioni',
    'Verifica preliminare. Il provider attivo è indicato nel registro del gioco.',
    'Programma di verifica', 'File runtime', 'Caricamento libreria', 'Funzioni API', 'Identificazione GPU',
    'Creazione dispositivo', 'Coda', 'Risorse condivise', 'Interrogazione provider', 'Versione provider',
    'Verifica GPU del provider', 'Provider', 'Funzioni Vulkan', 'File del pacchetto', 'Risultato', 'Motivo'),
 'pl': ('Pokaż szczegóły', 'Diagnostyka FSR 4.1', 'Kopiuj szczegóły', 'Skopiowano',
    'GPU', 'Sterownik', 'Zaktualizowano', 'Powodzenie', 'Błąd', 'Nie sprawdzono', 'Informacja',
    'Sprawdzenie wstępne. Aktywny dostawca jest wskazany w dzienniku gry.',
    'Program sprawdzający', 'Plik środowiska', 'Wczytanie biblioteki', 'Funkcje API', 'Dopasowanie GPU',
    'Tworzenie urządzenia', 'Kolejka', 'Wspólne zasoby', 'Zapytanie dostawcy', 'Wersja dostawcy',
    'Sprawdzenie GPU przez dostawcę', 'Dostawca', 'Funkcje Vulkan', 'Pliki pakietu', 'Wynik', 'Przyczyna'),
 'tr': ('Ayrıntıları göster', 'FSR 4.1 tanılama', 'Ayrıntıları kopyala', 'Kopyalandı',
    'GPU', 'Sürücü', 'Güncelleme', 'Başarılı', 'Başarısız', 'Denetlenmedi', 'Bilgi',
    'Ön denetim. Etkin sağlayıcı oyun günlüğüne kaydedilir.',
    'Denetim programı', 'Çalışma zamanı dosyası', 'Kitaplık yükleme', 'API işlevleri', 'GPU eşleştirme',
    'Aygıt oluşturma', 'Kuyruk', 'Paylaşılan kaynaklar', 'Sağlayıcı sorgusu', 'Sağlayıcı sürümü',
    'Sağlayıcı GPU denetimi', 'Sağlayıcı', 'Vulkan özellikleri', 'Paket dosyaları', 'Sonuç', 'Neden'),
 'zh': ('显示详情', 'FSR 4.1 诊断', '复制详情', '已复制',
    'GPU', '驱动程序', '更新时间', '通过', '失败', '未检查', '信息',
    '预检查。实际启用的提供程序会记录在游戏日志中。',
    '检查程序', '运行时文件', '加载库', 'API 函数', 'GPU 匹配',
    '创建设备', '队列', '共享资源', '查询提供程序', '提供程序版本',
    '提供程序 GPU 检查', '提供程序', 'Vulkan 功能', '软件包文件', '结果', '原因'),
 'ja': ('詳細を表示', 'FSR 4.1 診断', '詳細をコピー', 'コピー済み',
    'GPU', 'ドライバー', '更新日時', '成功', '失敗', '未確認', '情報',
    '事前確認です。実際のプロバイダーはゲームログに記録されます。',
    '確認プログラム', 'ランタイムファイル', 'ライブラリ読み込み', 'API 関数', 'GPU 照合',
    'デバイス作成', 'キュー', '共有リソース', 'プロバイダー照会', 'プロバイダーバージョン',
    'プロバイダーの GPU 確認', 'プロバイダー', 'Vulkan 機能', 'パッケージファイル', '結果', '原因'),
 'ko': ('세부 정보', 'FSR 4.1 진단', '세부 정보 복사', '복사됨',
    'GPU', '드라이버', '업데이트', '통과', '실패', '확인 안 됨', '정보',
    '사전 확인입니다. 실제 제공자는 게임 로그에 기록됩니다.',
    '확인 프로그램', '런타임 파일', '라이브러리 로드', 'API 함수', 'GPU 일치',
    '장치 생성', '큐', '공유 리소스', '제공자 조회', '제공자 버전',
    '제공자 GPU 확인', '제공자', 'Vulkan 기능', '패키지 파일', '결과', '원인'),
}
for language, values in TEXTS.items():
    assert len(values) == len(KEYS), language
    translations = dict(zip(KEYS, values))
    bbport_lang.EXTRA_TRANSLATIONS.setdefault(language, {}).update(translations)
    if language in bbport_lang._tables:
        bbport_lang._tables[language].update(translations)

LABELS = dict(probe='Probe', gpu='GPU', runtime_file='Runtime file', runtime_load='Library loading',
    exports='API exports', adapter='GPU matching', device='Device creation', queue='Queue',
    interop='Shared resources', provider_query='Provider query', provider_version='Provider version',
    provider_support='Provider GPU check', provider='Provider', vulkan_features='Vulkan features',
    package_files='Package files')
STATES = dict(pass_='Passed', fail='Failed', skip='Not checked', info='Information')
STATES['pass'] = STATES.pop('pass_')
EXPECTED = (
    ('runtime_file', 'gpu', 'queue', 'device', 'runtime_load', 'provider_query', 'provider_version', 'provider_support'),
    ('runtime_file', 'adapter', 'device', 'runtime_load', 'provider_query', 'provider_version', 'provider_support'),
    ('runtime_file', 'runtime_load', 'exports', 'adapter', 'device', 'provider_query', 'provider_version'),
    ('package_files', 'vulkan_features'),
)


def safe(value):
    """Only allow bounded, printable technical text; omit absolute paths from reports."""
    text = str(value)[:8192]
    text = re.sub(r"(?i)(?:[a-z]:[\\/]|\\\\)[^\r\n'\"<>]*", '<path>', text)
    text = re.sub(r"(?i)(?<!\w)/(?:users|home|mnt|media|volumes)/[^\r\n'\"<>]*", '<path>', text)
    return ''.join(c for c in text if c in '\n\t' or ord(c) >= 32)


def lines(caps, statuses, translate):
    """The UI and clipboard share this allowlisted report; settings/paths are never exported."""
    result = []
    add = lambda text='', style='normal': result.append((safe(text), style))
    add(translate('FSR 4.1 diagnostics'), 'heading')
    caps = caps or {}
    gpu = caps.get('gpu') or next((caps[key].get('gpu') for key in ('fsr412_vulkan', 'fsr412_dx12')
                                  if isinstance(caps.get(key), dict) and caps[key].get('gpu')), {})
    add(translate('GPU') + ': ' + safe(gpu.get('name') or translate('Not checked')))
    if 'vendor_id' in gpu:
        add(f"PCI: vendor=0x{gpu['vendor_id']:04x} device=0x{gpu.get('device_id', 0):04x}")
    add(translate('Driver') + ': ' + safe(' / '.join(filter(None,
        (gpu.get('driver_name', ''), gpu.get('driver_info', '')))) or translate('Not checked')))
    add('Vulkan: ' + safe(gpu.get('vulkan') or translate('Not checked')))
    if caps.get('checked_at'): add(translate('Updated') + ': ' + safe(caps['checked_at']))
    add(translate('Preflight check. The active provider is recorded in the game log.'), 'info')
    for index, ((version, method, title, _role), current) in enumerate(zip(model.ROUTES, statuses)):
        add()
        add(title, 'heading')
        add(translate('Result') + ': ' + translate(model.STATUS_KEYS[model.code(current)]),
            'pass' if current['supported'] else 'fail' if current.get('reason') != 'checking' else 'info')
        if current.get('reason') not in ('ready', '', 'checking'):
            add(translate('Reason') + ': ' + safe(current.get('reason', '')) + ' / ' + safe(current.get('detail', '')), 'fail')
        raw = caps.get('fsr412_vulkan' if method == 'vk' else 'fsr412_dx12', {}) if version == 'fsr412' else caps.get(method, {})
        checks = list(raw.get('checks', []))
        shared = caps.get('dx12_interop', {}) if method == 'dx12' else {}
        if shared:
            checks = list(shared.get('checks', [])) + checks
            if not shared.get('checks'):
                checks.insert(0, dict(id='interop', state='pass' if shared.get('supported') else 'fail',
                                     detail=shared.get('detail', '')))
        if version == 'fsr411':
            problem = current.get('files_problem') or (current if current.get('reason') in ('files', 'download') else None)
            checks.insert(0, dict(id='package_files', state='fail' if problem else 'pass',
                detail=problem.get('detail', '') if problem else 'fsr4vk / SHA-256' if method == 'vk' else 'AMD DX12 SDK'))
        seen = set()
        for check in checks:
            identifier = check['id']; state = check['state']
            seen.add(identifier)
            label = translate(LABELS.get(identifier, 'Probe'))
            add('  ' + label + ': ' + translate(STATES.get(state, 'Information')) +
                (' — ' + safe(check['detail']) if check.get('detail') else ''), state)
        for identifier in EXPECTED[index]:
            if identifier not in seen:
                add('  ' + translate(LABELS[identifier]) + ': ' + translate('Not checked'), 'info')
    return result


def report(caps, statuses, translate):
    return '\n'.join(text for text, _ in lines(caps, statuses, translate)) + '\n'
