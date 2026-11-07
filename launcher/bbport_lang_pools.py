# SPDX-License-Identifier: GPL-2.0-or-later
"""The co-op pools' texts: SuperMedo Server first, an own
server instead of shadPS4's public shadNet (the Online page, Play, the setup guide, Help), in every language
besides English, with the launcher's existing words for the pages and servers they name. Keyed by the English text like bbport_lang; merged into its tables on import."""
import bbport_lang

KEYS = [
    "SuperMedo Server (recommended): supermedo.com's own servers, with a free account made below.",
    'To play online with SuperMedo Server, sign in with Discord or make a free account first.',
    'With SuperMedo Server. Sign in once with Discord (free): your account is made for you.',
    'Online: another program changed the online settings (a co-op tool?). Its shadNet settings '
    'are used; your service and sign-ins stay as they were.',
    'SuperMedo Server (recommended)',
    "Own server: Seamless co-op's or a friend's private server, with its address and account.",
    "Own server (Seamless co-op, a friend's)",
    'Your account name on that server, not your email.',
    'host:port from the person who runs the server (Seamless co-op: its own address).',
    "Or your own server (Seamless co-op, a friend's): choose Own server there, with the address "
    'from the person who runs it.',
]

TEXTS = {
    'ru': [
        'Сервер SuperMedo (рекомендуется): собственные серверы supermedo.com, с бесплатным '
        'аккаунтом (создаётся ниже).',
        'Чтобы играть онлайн на сервере SuperMedo, сначала войдите через Discord или создайте '
        'бесплатный аккаунт.',
        'Через сервер SuperMedo. Войдите один раз через Discord (бесплатно): аккаунт создаётся '
        'для вас.',
        'Онлайн: другая программа изменила онлайн-настройки (инструмент для кооператива?). '
        'Используются её настройки shadNet; ваш сервис и входы остаются прежними.',
        'Сервер SuperMedo (рекомендуется)',
        'Свой сервер: сервер Seamless co-op или частный сервер друга, с его адресом и аккаунтом.',
        'Свой сервер (Seamless co-op, сервер друга)',
        'Имя вашего аккаунта на этом сервере, а не адрес эл. почты.',
        'host:port от того, кто держит сервер (у Seamless co-op — его собственный адрес).',
        'Или свой сервер (Seamless co-op, сервер друга): выберите там «Свой сервер» и укажите '
        'адрес от того, кто держит сервер.',
    ],
    'ar': [
        'خادم SuperMedo (موصى به): خوادم supermedo.com الخاصة، مع حساب مجاني تنشئه أدناه.',
        'للعب عبر الإنترنت مع خادم SuperMedo، سجّل الدخول باستخدام Discord أو أنشئ حسابًا مجانيًا '
        'أولًا.',
        'مع خادم SuperMedo. سجّل الدخول مرة واحدة باستخدام Discord (مجانًا): يُنشأ حسابك '
        'تلقائيًا.',
        'الإنترنت: غيّر برنامج آخر إعدادات اللعب عبر الإنترنت (أداة لعب تعاوني؟). تُستخدم إعدادات '
        'shadNet الخاصة به؛ تبقى خدمتك وتسجيلات دخولك كما كانت.',
        'خادم SuperMedo (موصى به)',
        'خادمك الخاص: خادم Seamless co-op أو خادم خاص لصديق، مع عنوانه وحسابه.',
        'خادمك الخاص (Seamless co-op، خادم صديق)',
        'اسم حسابك على ذلك الخادم، وليس بريدك الإلكتروني.',
        'host:port من الشخص الذي يدير الخادم (Seamless co-op: عنوانه الخاص).',
        'أو خادمك الخاص (Seamless co-op، خادم صديق): اختر «خادمك الخاص» هناك، مع العنوان من الشخص '
        'الذي يديره.',
    ],
    'es': [
        'Servidor SuperMedo (recomendado): los servidores propios de supermedo.com, con una '
        'cuenta gratuita que se crea abajo.',
        'Para jugar en línea con el servidor SuperMedo, primero inicia sesión con Discord o crea '
        'una cuenta gratuita.',
        'Con el servidor SuperMedo. Inicia sesión una vez con Discord (gratis): tu cuenta se crea '
        'sola.',
        'En línea: otro programa cambió los ajustes en línea (¿una herramienta de cooperativo?). '
        'Se usan sus ajustes de shadNet; tu servicio y tus inicios de sesión siguen como estaban.',
        'Servidor SuperMedo (recomendado)',
        'Servidor propio: el de Seamless co-op o el servidor privado de un amigo, con su '
        'dirección y su cuenta.',
        'Servidor propio (Seamless co-op, de un amigo)',
        'El nombre de tu cuenta en ese servidor, no tu correo.',
        'host:puerto que te da quien gestiona el servidor (Seamless co-op: su propia dirección).',
        'O tu propio servidor (Seamless co-op, de un amigo): elige allí Servidor propio, con la '
        'dirección que te dé quien lo gestiona.',
    ],
    'pt': [
        'Servidor SuperMedo (recomendado): os servidores próprios do supermedo.com, com uma conta '
        'gratuita criada abaixo.',
        'Para jogar online com o servidor SuperMedo, entre primeiro com o Discord ou crie uma '
        'conta gratuita.',
        'Com o servidor SuperMedo. Entre uma vez com o Discord (grátis): sua conta é criada para '
        'você.',
        'Online: outro programa mudou as configurações online (uma ferramenta de co-op?). As '
        'configurações shadNet dele são usadas; seu serviço e seus logins continuam como estavam.',
        'Servidor SuperMedo (recomendado)',
        'Servidor próprio: o do Seamless co-op ou o servidor privado de um amigo, com o endereço '
        'e a conta dele.',
        'Servidor próprio (Seamless co-op, de um amigo)',
        'O nome da sua conta nesse servidor, não o seu e-mail.',
        'host:porta de quem mantém o servidor (Seamless co-op: o endereço dele).',
        'Ou seu próprio servidor (Seamless co-op, de um amigo): escolha Servidor próprio lá, com '
        'o endereço de quem o mantém.',
    ],
    'fr': [
        'Serveur SuperMedo (recommandé) : les serveurs de supermedo.com, avec un compte gratuit '
        'créé ci-dessous.',
        "Pour jouer en ligne sur le serveur SuperMedo, connectez-vous d'abord avec Discord ou "
        'créez un compte gratuit.',
        'Avec le serveur SuperMedo. Connectez-vous une fois avec Discord (gratuit) : votre compte '
        'est créé pour vous.',
        'En ligne : un autre programme a modifié les paramètres en ligne (un outil de coopération '
        '?). Ses paramètres shadNet sont utilisés ; votre service et vos connexions restent comme '
        'avant.',
        'Serveur SuperMedo (recommandé)',
        "Serveur perso : celui de Seamless co-op ou le serveur privé d'un ami, avec son adresse "
        'et son compte.',
        "Serveur perso (Seamless co-op, celui d'un ami)",
        'Le nom de votre compte sur ce serveur, pas votre e-mail.',
        'hôte:port donné par la personne qui gère le serveur (Seamless co-op : sa propre '
        'adresse).',
        "Ou votre propre serveur (Seamless co-op, celui d'un ami) : choisissez-y Serveur perso, "
        "avec l'adresse donnée par la personne qui le gère.",
    ],
    'de': [
        'SuperMedo-Server (empfohlen): die eigenen Server von supermedo.com, mit einem '
        'kostenlosen Konto, das unten angelegt wird.',
        'Um mit dem SuperMedo-Server online zu spielen, melde dich zuerst mit Discord an oder '
        'lege ein kostenloses Konto an.',
        'Mit dem SuperMedo-Server. Einmal kostenlos mit Discord anmelden: Dein Konto wird für '
        'dich angelegt.',
        'Online: Ein anderes Programm hat die Online-Einstellungen geändert (ein Koop-Tool?). '
        'Seine shadNet-Einstellungen werden genutzt; dein Dienst und deine Anmeldungen bleiben, '
        'wie sie waren.',
        'SuperMedo-Server (empfohlen)',
        'Eigener Server: der von Seamless co-op oder der private Server eines Freundes, mit '
        'dessen Adresse und Konto.',
        'Eigener Server (Seamless co-op, von Freunden)',
        'Dein Kontoname auf diesem Server, nicht deine E-Mail.',
        'host:port von der Person, die den Server betreibt (Seamless co-op: dessen eigene '
        'Adresse).',
        'Oder ein eigener Server (Seamless co-op, von Freunden): dort „Eigener Server“ wählen, '
        'mit der Adresse von der Person, die ihn betreibt.',
    ],
    'it': [
        'Server SuperMedo (consigliato): i server di supermedo.com, con un account gratuito '
        'creato qui sotto.',
        'Per giocare online con il server SuperMedo, accedi prima con Discord o crea un account '
        'gratuito.',
        'Con il server SuperMedo. Accedi una volta con Discord (gratis): il tuo account viene '
        'creato per te.',
        'Online: un altro programma ha cambiato le impostazioni online (uno strumento co-op?). '
        'Vengono usate le sue impostazioni shadNet; il tuo servizio e i tuoi accessi restano '
        "com'erano.",
        'Server SuperMedo (consigliato)',
        'Server proprio: quello di Seamless co-op o il server privato di un amico, con il suo '
        'indirizzo e il suo account.',
        'Server proprio (Seamless co-op, di un amico)',
        'Il nome del tuo account su quel server, non la tua email.',
        'host:porta da chi gestisce il server (Seamless co-op: il suo indirizzo).',
        'Oppure il tuo server (Seamless co-op, di un amico): scegli lì Server proprio, con '
        "l'indirizzo di chi lo gestisce.",
    ],
    'pl': [
        'Serwer SuperMedo (zalecany): własne serwery supermedo.com, z darmowym kontem zakładanym '
        'poniżej.',
        'Aby grać online na serwerze SuperMedo, najpierw zaloguj się przez Discord albo załóż '
        'darmowe konto.',
        'Przez serwer SuperMedo. Zaloguj się raz przez Discord (za darmo): konto zostanie '
        'założone za ciebie.',
        'Online: inny program zmienił ustawienia online (narzędzie do co-opa?). Używane są jego '
        'ustawienia shadNet; twoja usługa i logowania pozostają bez zmian.',
        'Serwer SuperMedo (zalecany)',
        'Własny serwer: serwer Seamless co-op albo prywatny serwer znajomego, z jego adresem i '
        'kontem.',
        'Własny serwer (Seamless co-op, znajomego)',
        'Nazwa twojego konta na tym serwerze, nie adres e-mail.',
        'host:port od osoby, która prowadzi serwer (Seamless co-op: jego własny adres).',
        'Albo własny serwer (Seamless co-op, znajomego): wybierz tam Własny serwer, z adresem od '
        'osoby, która go prowadzi.',
    ],
    'tr': [
        'SuperMedo Sunucusu (önerilen): supermedo.com’un kendi sunucuları, aşağıda oluşturulan '
        'ücretsiz bir hesapla.',
        'SuperMedo Sunucusu ile çevrimiçi oynamak için önce Discord ile oturum aç veya ücretsiz '
        'bir hesap oluştur.',
        'SuperMedo Sunucusu ile. Bir kez Discord ile oturum aç (ücretsiz): hesabın senin için '
        'oluşturulur.',
        'Çevrimiçi: başka bir program çevrimiçi ayarları değiştirdi (bir co-op aracı mı?). Onun '
        'shadNet ayarları kullanılıyor; hizmetin ve oturumların olduğu gibi kalıyor.',
        'SuperMedo Sunucusu (önerilen)',
        'Kendi sunucun: Seamless co-op’un ya da bir arkadaşın özel sunucusu, adresi ve hesabıyla.',
        'Kendi sunucun (Seamless co-op, bir arkadaşın)',
        'O sunucudaki hesap adın, e-posta adresin değil.',
        'Sunucuyu çalıştıran kişiden host:port (Seamless co-op: kendi adresi).',
        'Ya da kendi sunucun (Seamless co-op, bir arkadaşın): orada Kendi sunucun’u seç, adresi '
        'sunucuyu çalıştıran kişiden al.',
    ],
    'zh': [
        'SuperMedo 服务器（推荐）：supermedo.com 自己的服务器，账户免费，在下方创建。',
        '要通过 SuperMedo 服务器在线游玩，请先使用 Discord 登录或创建免费账户。',
        '使用 SuperMedo 服务器。用 Discord 免费登录一次：账户会自动为你创建。',
        '在线：另一个程序更改了在线设置（合作工具？）。将使用它的 shadNet 设置；你的服务和登录保持不变。',
        'SuperMedo 服务器（推荐）',
        '自己的服务器：Seamless co-op 的服务器或朋友的私人服务器，填写其地址和账户。',
        '自己的服务器（Seamless co-op、朋友的服务器）',
        '你在该服务器上的账户名，而不是邮箱。',
        '由服务器运营者提供的 host:port（Seamless co-op：它自己的地址）。',
        '或者使用自己的服务器（Seamless co-op、朋友的服务器）：在那里选择“自己的服务器”，填写运营者提供的地址。',
    ],
    'ja': [
        'SuperMedo サーバー（推奨）：supermedo.com 独自のサーバーで、無料のアカウントは下で作成します。',
        'SuperMedo サーバーでオンラインプレイするには、先に Discord でサインインするか、無料のアカウントを作成してください。',
        'SuperMedo サーバーで。Discord で一度サインインするだけ（無料）：アカウントは自動で作成されます。',
        'オンライン：別のプログラムがオンライン設定を変更しました（Co-op ツール？）。その shadNet 設定を使います。サービスとサインインはそのままです。',
        'SuperMedo サーバー（推奨）',
        '自分のサーバー：Seamless co-op のサーバーや友人のプライベートサーバー。そのアドレスとアカウントを使います。',
        '自分のサーバー（Seamless co-op、友人のサーバー）',
        'そのサーバーでのアカウント名です（メールアドレスではありません）。',
        'サーバーを運営している人から聞いた host:port（Seamless co-op：その独自のアドレス）。',
        'または自分のサーバー（Seamless co-op、友人のサーバー）：そこで「自分のサーバー」を選び、運営者から聞いたアドレスを入力します。',
    ],
    'ko': [
        'SuperMedo 서버(권장): supermedo.com 자체 서버이며, 무료 계정은 아래에서 만듭니다.',
        'SuperMedo 서버로 온라인 플레이하려면 먼저 Discord로 로그인하거나 무료 계정을 만드세요.',
        'SuperMedo 서버로. Discord로 한 번만 로그인하세요(무료): 계정이 자동으로 만들어집니다.',
        '온라인: 다른 프로그램이 온라인 설정을 변경했습니다(코옵 도구?). 해당 shadNet 설정을 사용하며, 서비스와 로그인은 그대로 유지됩니다.',
        'SuperMedo 서버(권장)',
        '자체 서버: Seamless co-op 서버 또는 친구의 개인 서버와 그 주소 및 계정.',
        '자체 서버(Seamless co-op, 친구 서버)',
        '그 서버의 계정 이름이며, 이메일이 아닙니다.',
        '서버 운영자에게 받은 host:port(Seamless co-op: 자체 주소).',
        '또는 자체 서버(Seamless co-op, 친구 서버): 그곳에서 자체 서버를 선택하고 운영자에게 받은 주소를 입력하세요.',
    ],
}

for _language, _texts in TEXTS.items():
    assert len(_texts) == len(KEYS), (_language, len(_texts), len(KEYS))
    bbport_lang.EXTRA_TRANSLATIONS.setdefault(_language, {}).update(zip(KEYS, _texts))
