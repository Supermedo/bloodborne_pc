# SPDX-License-Identifier: GPL-2.0-or-later
"""Localized texts of the Russia route (Online page, SuperMedo Server). Russian is beside the English in
bbport_launcher_win.py."""

KEYS = (
    'Russia route (through a relay in Kazakhstan)',
    'Only if your network in Russia cannot reach the SuperMedo server: the connection goes '
    'through a relay to the same server and players. Leave it off otherwise.',
)
TEXTS = {
    'en': KEYS,
    'ar': ('مسار روسيا (عبر خادم وسيط في كازاخستان)',
           'فقط إذا كانت شبكتك في روسيا لا تصل إلى خادم SuperMedo: يمر الاتصال عبر خادم وسيط إلى الخادم نفسه '
           'واللاعبين أنفسهم. اتركه مطفأً في غير ذلك.'),
    'es': ('Ruta para Rusia (a través de un relé en Kazajistán)',
           'Solo si tu red en Rusia no llega al servidor de SuperMedo: la conexión pasa por un relé hacia el '
           'mismo servidor y los mismos jugadores. Si no, déjalo desactivado.'),
    'pt': ('Rota para a Rússia (por um relay no Cazaquistão)',
           'Só se a sua rede na Rússia não alcança o servidor SuperMedo: a conexão passa por um relay até o '
           'mesmo servidor e os mesmos jogadores. Caso contrário, deixe desligado.'),
    'fr': ('Route pour la Russie (via un relais au Kazakhstan)',
           "Seulement si votre réseau en Russie n'atteint pas le serveur SuperMedo : la connexion passe par un "
           'relais vers le même serveur et les mêmes joueurs. Sinon, laissez-la désactivée.'),
    'de': ('Russland-Route (über ein Relay in Kasachstan)',
           'Nur wenn dein Netz in Russland den SuperMedo-Server nicht erreicht: Die Verbindung läuft über ein '
           'Relay zum selben Server und denselben Spielern. Sonst ausgeschaltet lassen.'),
    'it': ('Percorso per la Russia (tramite un relay in Kazakistan)',
           'Solo se la tua rete in Russia non raggiunge il server SuperMedo: la connessione passa da un relay '
           'verso lo stesso server e gli stessi giocatori. Altrimenti lascialo disattivato.'),
    'pl': ('Trasa dla Rosji (przez przekaźnik w Kazachstanie)',
           'Tylko jeśli twoja sieć w Rosji nie łączy się z serwerem SuperMedo: połączenie idzie przez przekaźnik '
           'do tego samego serwera i tych samych graczy. W innym razie zostaw wyłączone.'),
    'tr': ('Rusya yolu (Kazakistan\'daki bir aktarıcı üzerinden)',
           'Yalnızca Rusya\'daki ağın SuperMedo sunucusuna ulaşamıyorsa: bağlantı bir aktarıcı üzerinden aynı '
           'sunucuya ve aynı oyunculara gider. Aksi halde kapalı bırak.'),
    'zh': ('俄罗斯线路（经由哈萨克斯坦的中继）',
           '仅当你在俄罗斯的网络无法连接 SuperMedo 服务器时使用：连接经由中继到达同一服务器和同一批玩家。否则请保持关闭。'),
    'ja': ('ロシア経路（カザフスタンの中継経由）',
           'ロシアのネットワークから SuperMedo サーバーに接続できない場合のみ：接続は中継を通って同じサーバー・同じプレイヤーへ'
           'つながります。それ以外はオフのままにしてください。'),
    'ko': ('러시아 경로 (카자흐스탄 중계 서버 경유)',
           '러시아에서 네트워크가 SuperMedo 서버에 연결되지 않을 때만: 연결이 중계 서버를 거쳐 같은 서버와 같은 '
           '플레이어에게 갑니다. 그 외에는 꺼 두세요.'),
}
TRANSLATIONS = {language: dict(zip(KEYS, texts, strict=True)) for language, texts in TEXTS.items()}
