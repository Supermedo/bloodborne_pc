# SPDX-License-Identifier: GPL-2.0-or-later
"""Localized texts of a server certificate the launcher refuses (bbport_tls.explain): the PC's clock,
an antivirus or proxy in between. Russian is beside the English in bbport_tls.py."""

KEYS = (
    "This PC's date or time looks wrong ({0}): the server's certificate is not valid then. "
    "Correct it in the Windows settings and try again.",
    "The certificate for {0} is from {1}, not from the server: an antivirus' HTTPS scanning "
    "or a proxy is in between. Exclude {0} there and try again.",
)
TEXTS = {
    'en': KEYS,
    'de': (
        'Datum oder Uhrzeit dieses PCs scheinen falsch zu sein ({0}): Dann ist das Zertifikat des Servers nicht '
        'gültig. Korrigiere sie in den Windows-Einstellungen und versuche es erneut.',
        'Das Zertifikat für {0} stammt von {1}, nicht vom Server: Die HTTPS-Prüfung eines Virenschutzes oder ein '
        'Proxy steht dazwischen. Nimm {0} dort aus und versuche es erneut.',
    ),
    'ar': (
        'يبدو أن تاريخ هذا الحاسوب أو وقته غير صحيح ({0}): شهادة الخادم غير صالحة في ذلك الوقت. صحّحه من '
        'إعدادات Windows ثم أعد المحاولة.',
        'شهادة {0} صادرة من {1} وليس من الخادم: يقف بينهما فحص HTTPS لبرنامج مكافحة فيروسات أو خادم وكيل. '
        'استثنِ {0} هناك ثم أعد المحاولة.',
    ),
    'es': (
        'La fecha o la hora de este PC parecen incorrectas ({0}): en ese momento el certificado del servidor no '
        'es válido. Corrígelas en la configuración de Windows y vuelve a intentarlo.',
        'El certificado de {0} viene de {1}, no del servidor: el análisis HTTPS de un antivirus o un proxy se '
        'interpone. Excluye {0} allí y vuelve a intentarlo.',
    ),
    'pt': (
        'A data ou a hora deste PC parecem erradas ({0}): nesse momento o certificado do servidor não é válido. '
        'Corrija-as nas configurações do Windows e tente novamente.',
        'O certificado de {0} vem de {1}, não do servidor: a verificação HTTPS de um antivírus ou um proxy está '
        'no meio. Coloque {0} nas exceções dele e tente novamente.',
    ),
    'fr': (
        "La date ou l'heure de ce PC semble fausse ({0}) : le certificat du serveur n'est pas valide à ce "
        'moment-là. Corrigez-la dans les paramètres de Windows, puis réessayez.',
        "Le certificat de {0} provient de {1}, pas du serveur : l'analyse HTTPS d'un antivirus ou un proxy "
        "s'interpose. Excluez {0} de cette analyse, puis réessayez.",
    ),
    'it': (
        "La data o l'ora di questo PC sembrano errate ({0}): in quel momento il certificato del server non è "
        'valido. Correggile nelle impostazioni di Windows e riprova.',
        "Il certificato di {0} proviene da {1}, non dal server: in mezzo c'è la scansione HTTPS di un antivirus "
        'o un proxy. Escludi {0} lì e riprova.',
    ),
    'pl': (
        'Data lub godzina na tym komputerze wydają się błędne ({0}): certyfikat serwera nie jest wtedy ważny. '
        'Popraw je w ustawieniach Windows i spróbuj ponownie.',
        'Certyfikat dla {0} pochodzi od {1}, a nie od serwera: pośredniczy skanowanie HTTPS antywirusa lub '
        'serwer proxy. Dodaj {0} tam do wyjątków i spróbuj ponownie.',
    ),
    'tr': (
        'Bu bilgisayarın tarihi veya saati yanlış görünüyor ({0}): sunucunun sertifikası o tarihte geçerli '
        'değil. Windows ayarlarından düzeltip yeniden deneyin.',
        '{0} sertifikası sunucudan değil, {1} kaynağından geliyor: araya bir antivirüsün HTTPS taraması veya bir '
        'proxy giriyor. {0} adresini orada hariç tutup yeniden deneyin.',
    ),
    'zh': (
        '这台电脑的日期或时间似乎不正确（{0}）：服务器证书在该时间无效。请在 Windows 设置中更正后重试。',
        '{0} 的证书来自 {1}，而不是服务器：中间有杀毒软件的 HTTPS 扫描或代理。请在其中排除 {0} 后重试。',
    ),
    'ja': (
        'この PC の日付または時刻が正しくないようです（{0}）：その時点ではサーバーの証明書は有効ではありません。'
        'Windows の設定で修正してから、もう一度お試しください。',
        '{0} の証明書はサーバーではなく {1} のものです：ウイルス対策ソフトの HTTPS スキャンまたはプロキシが間に'
        '入っています。そこで {0} を除外してから、もう一度お試しください。',
    ),
    'ko': (
        '이 PC의 날짜 또는 시간이 잘못된 것 같습니다({0}): 그 시점에는 서버 인증서가 유효하지 않습니다. '
        'Windows 설정에서 고친 뒤 다시 시도하세요.',
        '{0}의 인증서가 서버가 아닌 {1}에서 왔습니다: 백신의 HTTPS 검사나 프록시가 중간에 있습니다. 거기서 '
        '{0}을(를) 제외한 뒤 다시 시도하세요.',
    ),
}
TRANSLATIONS = {language: dict(zip(KEYS, texts, strict=True)) for language, texts in TEXTS.items()}
