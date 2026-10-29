# SPDX-License-Identifier: GPL-2.0-or-later
"""ML-FG diagnostics; failed checks preserve the saved selection."""
import json
import subprocess
from pathlib import Path

REASONS = {'ready', 'windows11', 'rx9000', 'files', 'runtime', 'vulkan',
           'adapter', 'interop', 'dx12', 'provider', 'check'}

def query(port, env=None, version='4.0.1'):
    if version not in ('4.0.1', '4.0.2'):
        raise ValueError('unsupported ML-FG version')
    exe = Path(port)/'bin/bb-fg401-capabilities.exe'
    if not exe.is_file():
        exe = Path(port)/'out/bb-fg401-capabilities.exe'
    if not exe.is_file():
        return dict(supported=False, reason='files', detail='bb-fg401-capabilities.exe')
    try:
        command = [str(exe)] + (['--provider', version] if version != '4.0.1' else [])
        result = subprocess.run(command, cwd=port, env=env, capture_output=True,
            text=True, timeout=15, creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        data = json.loads(result.stdout)
        if result.returncode or not isinstance(data, dict) or data.get('provider') != version:
            raise ValueError('invalid probe response')
        if data.get('supported') is True:
            return dict(supported=True, reason='ready', detail='')
        if data.get('supported') is not False or data.get('reason') not in REASONS - {'ready'}:
            raise ValueError('invalid support result')
        detail = data.get('detail', '')
        if not isinstance(detail, str):
            raise ValueError('invalid probe detail')
        return dict(supported=False, reason=data['reason'], detail=detail)
    except (OSError, ValueError, TypeError, subprocess.TimeoutExpired):
        return dict(supported=False, reason='check', detail='')

def query_versions(port, env=None):
    return {key: query(port, env, version) for key, version in
            (('fsr401', '4.0.1'), ('fsr402', '4.0.2'))}

def status_for(statuses, selection):
    if statuses is None:
        return None
    key = selection if selection in ('fsr401', 'fsr402') else 'fsr401'
    # Keep callers of the original single-version query compatible.
    if 'supported' in statuses:
        return statuses if key == 'fsr401' else dict(supported=False, reason='provider', detail='4.0.2')
    return statuses[key]

def message(status, translate, hdr=False, selected=False, version='4.0.1'):
    original_translate = translate
    translate = lambda key: original_translate(key.replace('4.0.1', version))
    if status is None:
        return translate('ML-FG 4.0.1: checking support…')
    if status.get('supported'):
        return translate('ML-FG pauses during HDR output.') if hdr and selected else ''
    key = {
        'windows11': 'ML-FG requires Windows 11.',
        'rx9000': 'ML-FG requires a Radeon RX 9000.',
        'files': 'Reinstall the Windows package. Missing file: {}',
        'runtime': 'ML-FG runtime load failed: {}',
        'vulkan': 'ML-FG requires Vulkan 1.3.',
        'adapter': 'ML-FG GPU matching failed.',
        'interop': 'ML-FG sharing/queue support missing.',
        'dx12': 'ML-FG driver support missing: {}',
        'provider': 'ML-FG 4.0.1 provider unavailable.',
    }.get(status.get('reason'), 'ML-FG check failed. Recheck support.')
    return translate(key).format(status.get('detail') or 'AMD runtime')

KEYS = ('AMD ML-FG 4.0.1 (RX 9000, 2× SDR, experimental)', 'ML-FG 4.0.1: checking support…',
    'ML-FG requires Windows 11.', 'ML-FG requires a Radeon RX 9000.',
    'Reinstall the Windows package. Missing file: {}', 'ML-FG runtime load failed: {}',
    'ML-FG requires Vulkan 1.3.', 'ML-FG GPU matching failed.',
    'ML-FG sharing/queue support missing.', 'ML-FG driver support missing: {}',
    'ML-FG 4.0.1 provider unavailable.', 'ML-FG check failed. Recheck support.',
    'ML-FG pauses during HDR output.')
TEXTS = {
 'en': KEYS,
 'de': ('AMD ML-FG 4.0.1 (RX 9000, 2× SDR, experimentell)', 'ML-FG 4.0.1: Unterstützung wird geprüft…',
    'ML-FG benötigt Windows 11.', 'ML-FG benötigt eine Radeon RX 9000.',
    'Windows-Paket erneut installieren. Fehlende Datei: {}', 'ML-FG-Ladefehler: {}',
    'ML-FG benötigt Vulkan 1.3.', 'ML-FG: GPU-Zuordnung fehlgeschlagen.',
    'ML-FG: GPU-Freigabe oder zusätzliche Queue fehlt.', 'ML-FG: Treiberfunktion fehlt: {}',
    'ML-FG-4.0.1-Provider nicht verfügbar.', 'ML-FG-Prüfung fehlgeschlagen. Erneut prüfen.',
    'ML-FG pausiert bei HDR-Ausgabe.'),
 'ru': ('AMD ML-FG 4.0.1 (RX 9000, 2× SDR, экспериментально)', 'ML-FG 4.0.1: проверка поддержки…',
    'Для ML-FG нужна Windows 11.', 'Для ML-FG нужна Radeon RX 9000.',
    'Переустановите пакет Windows. Отсутствует файл: {}', 'Ошибка загрузки ML-FG: {}',
    'Для ML-FG нужен Vulkan 1.3.', 'ML-FG: ошибка сопоставления GPU.',
    'ML-FG: недоступен обмен GPU или дополнительная очередь.', 'ML-FG: нет функции драйвера: {}',
    'Провайдер ML-FG 4.0.1 недоступен.', 'Проверка ML-FG не удалась. Повторите проверку.',
    'ML-FG приостановлена при выводе HDR.'),
 'ar': ('AMD ML-FG 4.0.1 (RX 9000، 2× SDR، تجريبي)', 'ML-FG 4.0.1: جارٍ التحقق من الدعم…',
    'يتطلب ML-FG نظام Windows 11.', 'يتطلب ML-FG بطاقة Radeon RX 9000.',
    'أعد تثبيت حزمة Windows. الملف المفقود: {}', 'فشل تحميل ML-FG: {}',
    'يتطلب ML-FG واجهة Vulkan 1.3.', 'فشلت مطابقة بطاقة ML-FG.',
    'ML-FG: مشاركة البطاقة أو قائمة إضافية غير متاحة.', 'ML-FG: ميزة برنامج التشغيل مفقودة: {}',
    'مزوّد ML-FG 4.0.1 غير متاح.', 'فشل فحص ML-FG. أعد الفحص.',
    'يتوقف ML-FG مؤقتًا عند إخراج HDR.'),
 'es': ('AMD ML-FG 4.0.1 (RX 9000, 2× SDR, experimental)', 'ML-FG 4.0.1: comprobando compatibilidad…',
    'ML-FG requiere Windows 11.', 'ML-FG requiere una Radeon RX 9000.',
    'Reinstala el paquete de Windows. Falta el archivo: {}', 'Error de carga de ML-FG: {}',
    'ML-FG requiere Vulkan 1.3.', 'ML-FG: fallo al identificar la GPU.',
    'ML-FG: falta el uso compartido o una cola adicional.', 'ML-FG: falta una función del controlador: {}',
    'Proveedor ML-FG 4.0.1 no disponible.', 'No se pudo comprobar ML-FG. Reintenta.',
    'ML-FG se pausa durante la salida HDR.'),
 'pt': ('AMD ML-FG 4.0.1 (RX 9000, 2× SDR, experimental)', 'ML-FG 4.0.1: verificando compatibilidade…',
    'ML-FG requer Windows 11.', 'ML-FG requer uma Radeon RX 9000.',
    'Reinstale o pacote Windows. Arquivo ausente: {}', 'Falha ao carregar ML-FG: {}',
    'ML-FG requer Vulkan 1.3.', 'ML-FG: falha ao identificar a GPU.',
    'ML-FG: compartilhamento ou fila adicional indisponível.', 'ML-FG: falta um recurso do driver: {}',
    'Provedor ML-FG 4.0.1 indisponível.', 'Falha na verificação ML-FG. Verifique novamente.',
    'ML-FG pausa durante a saída HDR.'),
 'fr': ('AMD ML-FG 4.0.1 (RX 9000, 2× SDR, expérimental)', 'ML-FG 4.0.1 : vérification de la compatibilité…',
    'ML-FG nécessite Windows 11.', 'ML-FG nécessite une Radeon RX 9000.',
    'Réinstallez le package Windows. Fichier absent : {}', 'Échec du chargement ML-FG : {}',
    'ML-FG nécessite Vulkan 1.3.', 'ML-FG : échec de correspondance du GPU.',
    'ML-FG : partage GPU ou file supplémentaire indisponible.', 'ML-FG : fonction du pilote manquante : {}',
    'Fournisseur ML-FG 4.0.1 indisponible.', 'Échec de vérification ML-FG. Réessayez.',
    'ML-FG est suspendu pendant la sortie HDR.'),
 'it': ('AMD ML-FG 4.0.1 (RX 9000, 2× SDR, sperimentale)', 'ML-FG 4.0.1: verifica della compatibilità…',
    'ML-FG richiede Windows 11.', 'ML-FG richiede una Radeon RX 9000.',
    'Reinstalla il pacchetto Windows. File mancante: {}', 'Caricamento ML-FG non riuscito: {}',
    'ML-FG richiede Vulkan 1.3.', 'ML-FG: identificazione GPU non riuscita.',
    'ML-FG: condivisione GPU o coda aggiuntiva non disponibile.', 'ML-FG: manca una funzione del driver: {}',
    'Provider ML-FG 4.0.1 non disponibile.', 'Verifica ML-FG non riuscita. Riprova.',
    'ML-FG è in pausa durante l’uscita HDR.'),
 'pl': ('AMD ML-FG 4.0.1 (RX 9000, 2× SDR, eksperymentalne)', 'ML-FG 4.0.1: sprawdzanie obsługi…',
    'ML-FG wymaga Windows 11.', 'ML-FG wymaga Radeon RX 9000.',
    'Zainstaluj ponownie pakiet Windows. Brak pliku: {}', 'Błąd ładowania ML-FG: {}',
    'ML-FG wymaga Vulkan 1.3.', 'ML-FG: błąd dopasowania GPU.',
    'ML-FG: brak współdzielenia GPU lub dodatkowej kolejki.', 'ML-FG: brak funkcji sterownika: {}',
    'Dostawca ML-FG 4.0.1 niedostępny.', 'Sprawdzenie ML-FG nieudane. Ponów próbę.',
    'ML-FG jest wstrzymane podczas wyjścia HDR.'),
 'tr': ('AMD ML-FG 4.0.1 (RX 9000, 2× SDR, deneysel)', 'ML-FG 4.0.1: destek denetleniyor…',
    'ML-FG için Windows 11 gerekir.', 'ML-FG için Radeon RX 9000 gerekir.',
    'Windows paketini yeniden kurun. Eksik dosya: {}', 'ML-FG yüklenemedi: {}',
    'ML-FG için Vulkan 1.3 gerekir.', 'ML-FG GPU eşleştirmesi başarısız.',
    'ML-FG: GPU paylaşımı veya ek kuyruk eksik.', 'ML-FG: sürücü özelliği eksik: {}',
    'ML-FG 4.0.1 sağlayıcısı kullanılamıyor.', 'ML-FG denetimi başarısız. Tekrar denetleyin.',
    'HDR çıkışı sırasında ML-FG duraklatılır.'),
 'zh': ('AMD ML-FG 4.0.1（RX 9000，2× SDR，实验性）', 'ML-FG 4.0.1：正在检查支持…',
    'ML-FG 需要 Windows 11。', 'ML-FG 需要 Radeon RX 9000。',
    '请重新安装 Windows 软件包。缺失文件：{}', 'ML-FG 加载失败：{}',
    'ML-FG 需要 Vulkan 1.3。', 'ML-FG GPU 匹配失败。',
    'ML-FG 缺少 GPU 共享功能或额外队列。', 'ML-FG 缺少驱动功能：{}',
    'ML-FG 4.0.1 提供程序不可用。', 'ML-FG 检查失败。请重新检查。',
    'HDR 输出期间暂停 ML-FG。'),
 'ja': ('AMD ML-FG 4.0.1（RX 9000、2× SDR、実験的）', 'ML-FG 4.0.1：対応状況を確認中…',
    'ML-FG には Windows 11 が必要です。', 'ML-FG には Radeon RX 9000 が必要です。',
    'Windows パッケージを再インストールしてください。不足ファイル：{}', 'ML-FG 読み込み失敗：{}',
    'ML-FG には Vulkan 1.3 が必要です。', 'ML-FG の GPU 照合に失敗しました。',
    'ML-FG の GPU 共有機能または追加キューがありません。', 'ML-FG のドライバー機能がありません：{}',
    'ML-FG 4.0.1 プロバイダーを利用できません。', 'ML-FG の確認に失敗しました。再確認してください。',
    'HDR 出力中は ML-FG が一時停止します。'),
 'ko': ('AMD ML-FG 4.0.1 (RX 9000, 2× SDR, 실험적)', 'ML-FG 4.0.1: 지원 확인 중…',
    'ML-FG에는 Windows 11이 필요합니다.', 'ML-FG에는 Radeon RX 9000이 필요합니다.',
    'Windows 패키지를 다시 설치하세요. 누락된 파일: {}', 'ML-FG 로드 실패: {}',
    'ML-FG에는 Vulkan 1.3이 필요합니다.', 'ML-FG GPU 일치 확인 실패.',
    'ML-FG GPU 공유 기능 또는 추가 큐가 없습니다.', 'ML-FG 드라이버 기능 누락: {}',
    'ML-FG 4.0.1 공급자를 사용할 수 없습니다.', 'ML-FG 확인 실패. 다시 확인하세요.',
    'HDR 출력 중에는 ML-FG가 일시 중지됩니다.'),
}
TRANSLATIONS = {lang: dict(zip(KEYS, texts, strict=True)) for lang, texts in TEXTS.items()}
# Version-only variants retain the existing concise translations in all 13 languages.
for translations in TRANSLATIONS.values():
    translations.update({key.replace('4.0.1', '4.0.2'): value.replace('4.0.1', '4.0.2')
                         for key, value in list(translations.items()) if '4.0.1' in key})
