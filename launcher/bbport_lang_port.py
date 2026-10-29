# SPDX-License-Identifier: GPL-2.0-or-later
"""Texts of the 1.9.3 work (respec, controller buttons, the direct mouse camera's lock-on, the
low-VRAM profile, the eboot.bin warnings, the Cathedral Ward benchmark) and the Controls page's
key and button names, in every language besides English and Russian (Russian is in the code).
Machine translated with the existing translations as the glossary (the game's own terms: Blood
Echoes, Insight, the Doll); native speakers are welcome to improve them. PORT_KEYS are the English
texts; each language's list is in the same order. Merged into bbport_lang's tables on import."""
import bbport_lang

PORT_KEYS = [
    'Cross', 'Circle', 'Square', 'Triangle', 'R3 (lock-on)',
    'D-pad down', 'D-pad left', 'D-pad right', 'Touchpad (left click)', 'Touchpad (right click)',
    'Walk', 'Right Shift', 'Right Ctrl', 'Left Alt', 'Right Alt', 'Right Win', 'Right Meta', 'Space',
    'Left click', 'Right click', 'Wheel click', 'Mouse back', 'Mouse forward',
    'Wheel up', 'Wheel down', 'Wheel left', 'Wheel right',
    'D-pad ↑', 'D-pad ↓', 'D-pad ←', 'D-pad →', 'Paddle L4', 'Paddle R4', 'Paddle L5', 'Paddle R5',
    "Rebirth at the Doll: back to your origin's attributes for 5 Insight, levels regained free",
    'Auto (cards up to 6 GB)',
    'Respec',
    "Spread a character's attribute points again: same level, same Blood Echoes. Changes the save while the game is "
    'closed; the saves from before go into your backups (Saves).',
    "Keyboard and mouse bindings, up to three per action, and the controller's buttons below. Both work "
    "side by side.",
    "Direct turns the game's camera by exact angles: no stick acceleration or turn-speed cap. While locked "
    "on, moving the mouse switches target.",
    'Controller',
    'Click a slot, then press the controller button for that action; Esc cancels. Right-click a slot to put '
    'back its default. A button on two actions presses both. The sticks always move and turn the camera.',
    'Low-VRAM profile',
    'A larger texture budget and, at 1080p with an upscaler, a smaller scene from the start (preset changes '
    'then need a restart).',
    'Close the game first: it would save over the change.',
    'Press a button…',
    'Slot {}: {} (level {})',
    'No controller found. Connect one, then click the slot again.',
    'Respec a character…',
    'No characters found in {}.',
    'Done. A copy of the saves from before is in {}.',
    'Reset',
    'Reset controller buttons',
    'Low-VRAM profile by fixdag and bashiku.',
    'Benchmark for bug reports…',
    'Game tests with the Cathedral Ward scenario: the same save and route for everyone.',
    'Vulkan (fsr4vk)',
    'AMD DX12',
    'Your eboot.bin is not the clean 1.09 one (modified, or from another release). The port cannot patch '
    'it: the game runs at 30 FPS in 16:9 (ultrawide and the output resolution do not apply), the Game & '
    'effects options do nothing, and some scenes (like the cutscene after Rom) can freeze.\n\nReplace '
    'eboot.bin with the one from an unmodified 1.09 update.',
    'Points to place: {}',
    '{} points too many',
    "Your eboot.bin has been modified (for example with a 60 FPS patch already applied). Part of the port's "
    "patches cannot be used with it:\n\n{}\n\nFor every option, replace eboot.bin with the one from an "
    "unmodified 1.09 update.",
    'Cathedral Ward',
    'The same save and route for everyone: standing, a full camera turn and a short walk in Cathedral Ward '
    '(about 3 minutes). For comparing PCs and for bug reports: the report adds the texture cache and VRAM '
    'details.',
]

PORT = {
    'de': [
        'Kreuz', 'Kreis', 'Quadrat', 'Dreieck', 'R3 (Anvisieren)',
        'Steuerkreuz unten', 'Steuerkreuz links', 'Steuerkreuz rechts', 'Touchpad (Linksklick)',
        'Touchpad (Rechtsklick)',
        'Gehen', 'Umschalt rechts', 'Strg rechts', 'Alt links', 'Alt rechts', 'Win rechts', 'Meta rechts',
        'Leertaste',
        'Linksklick', 'Rechtsklick', 'Mausradklick', 'Maus zurück', 'Maus vor',
        'Mausrad hoch', 'Mausrad runter', 'Mausrad links', 'Mausrad rechts',
        'Steuerkreuz ↑', 'Steuerkreuz ↓', 'Steuerkreuz ←', 'Steuerkreuz →', 'Paddle L4', 'Paddle R4',
        'Paddle L5', 'Paddle R5',
        'Wiedergeburt bei der Puppe: zurück zu den Attributen deiner Herkunft für 5 Einsicht, die Stufen '
        'gibt es kostenlos zurück',
        'Auto (Karten bis 6 GB)',
        'Neu verteilen',
        'Die Attributpunkte eines Charakters neu verteilen: gleiche Stufe, gleiche Blutechos. Ändert den Spielstand, '
        'während das Spiel geschlossen ist; die Spielstände von vorher kommen in deine Sicherungen (Spielstände).',
        'Tastatur- und Mausbelegung, bis zu drei pro Aktion, und darunter die Tasten des Controllers. Beides '
        'funktioniert nebeneinander.',
        'Direkt dreht die Kamera um genaue Winkel: ohne Stick-Beschleunigung und Drehgrenze. Beim Anvisieren '
        'wechselt eine Mausbewegung das Ziel.',
        'Controller',
        'Klick auf ein Feld, dann drück die Controller-Taste für diese Aktion; Esc bricht ab. Rechtsklick auf '
        'ein Feld stellt die Vorgabe wieder her. Eine Taste auf zwei Aktionen drückt beide. Die Sticks bewegen '
        'und drehen immer die Kamera.',
        'Profil für wenig VRAM',
        'Ein größeres Texturbudget und, bei 1080p mit Upscaler, eine kleinere Szene von Anfang an '
        '(Voreinstellungen zu ändern braucht dann einen Neustart).',
        'Schließe zuerst das Spiel: Es würde über die Änderung speichern.',
        'Drück eine Taste…',
        'Platz {}: {} (Stufe {})',
        'Kein Controller gefunden. Schließ einen an und klick dann erneut auf das Feld.',
        'Charakter neu verteilen…',
        'Keine Charaktere gefunden in {}.',
        'Fertig. Eine Kopie der vorherigen Spielstände liegt in {}.',
        'Zurücksetzen',
        'Controller-Tasten zurücksetzen',
        'Profil für wenig VRAM von fixdag und bashiku.',
        'Benchmark für Fehlerberichte…',
        'Spieltests mit dem Szenario Kathedralbezirk: derselbe Spielstand und dieselbe Route für alle.',
        'Vulkan (fsr4vk)',
        'AMD DX12',
        'Deine eboot.bin ist nicht die saubere 1.09 (verändert oder aus einer anderen Veröffentlichung). Der '
        'Port kann sie nicht patchen: Das Spiel läuft mit 30 FPS in 16:9 (Ultrawide und die Ausgabeauflösung '
        'greifen nicht), die Optionen unter Spiel & Effekte bewirken nichts, und manche Szenen (wie die '
        'Zwischensequenz nach Rom) können einfrieren.\n\nErsetze eboot.bin durch die aus einem unveränderten '
        '1.09-Update.',
        'Zu verteilende Punkte: {}',
        '{} Punkte zu viel',
        'Deine eboot.bin wurde verändert (zum Beispiel mit einem schon angewendeten 60-FPS-Patch). Ein Teil '
        'der Patches des Ports passt nicht dazu:\n\n{}\n\nFür alle Optionen ersetze eboot.bin durch die aus '
        'einem unveränderten 1.09-Update.',
        'Kathedralbezirk',
        'Derselbe Spielstand und dieselbe Route für alle: Stehen, eine ganze Kameradrehung und ein kurzer Gang '
        'im Kathedralbezirk (etwa 3 Minuten). Zum Vergleichen von PCs und für Fehlerberichte: Der Bericht '
        'enthält zusätzlich Texturcache und VRAM im Detail.',
    ],
    'fr': [
        'Croix', 'Rond', 'Carré', 'Triangle', 'R3 (verrouillage)',
        'Croix directionnelle bas', 'Croix directionnelle gauche', 'Croix directionnelle droite',
        'Pavé tactile (clic gauche)', 'Pavé tactile (clic droit)',
        'Marcher', 'Maj droite', 'Ctrl droite', 'Alt gauche', 'Alt droite', 'Win droite', 'Méta droite',
        'Espace',
        'Clic gauche', 'Clic droit', 'Clic molette', 'Souris précédent', 'Souris suivant',
        'Molette haut', 'Molette bas', 'Molette gauche', 'Molette droite',
        'Croix ↑', 'Croix ↓', 'Croix ←', 'Croix →', 'Palette L4', 'Palette R4', 'Palette L5', 'Palette R5',
        "Renaissance auprès de la Poupée : retour aux attributs de votre origine pour 5 Perspicacité, les "
        "niveaux sont regagnés gratuitement",
        'Auto (cartes jusqu’à 6 Go)',
        'Redistribuer',
        "Redistribuer les points d'attribut d'un personnage : même niveau, mêmes Échos de sang. Modifie la "
        "sauvegarde pendant que le jeu est fermé ; les sauvegardes d'avant vont dans vos copies de sauvegarde "
        '(Sauvegardes).',
        "Assignations clavier et souris, jusqu'à trois par action, et les boutons de la manette ci-dessous. "
        "Les deux fonctionnent ensemble.",
        'Le mode Direct tourne la caméra du jeu selon des angles exacts : sans accélération du stick ni vitesse '
        'de rotation plafonnée. Pendant le verrouillage, bouger la souris change de cible.',
        'Manette',
        "Cliquez sur un emplacement, puis appuyez sur le bouton de la manette pour cette action ; Échap annule. "
        "Un clic droit sur un emplacement rétablit sa valeur par défaut. Un bouton assigné à deux actions les "
        "déclenche toutes les deux. Les sticks déplacent et tournent toujours la caméra.",
        'Profil faible VRAM',
        "Un budget de textures plus grand et, en 1080p avec un upscaler, une scène plus petite dès le départ "
        "(changer de préréglage demande alors un redémarrage).",
        "Fermez d'abord le jeu : il sauvegarderait par-dessus la modification.",
        'Appuyez sur un bouton…',
        'Emplacement {} : {} (niveau {})',
        'Aucune manette trouvée. Branchez-en une, puis cliquez à nouveau sur l’emplacement.',
        'Redistribuer un personnage…',
        'Aucun personnage trouvé dans {}.',
        "Terminé. Une copie des sauvegardes d'avant se trouve dans {}.",
        'Réinitialiser',
        'Réinitialiser les boutons de la manette',
        'Profil faible VRAM par fixdag et bashiku.',
        'Benchmark pour les rapports de bug…',
        'Tests en jeu avec le scénario Quartier de la cathédrale : la même sauvegarde et le même parcours pour '
        'tous.',
        'Vulkan (fsr4vk)',
        'AMD DX12',
        "Votre eboot.bin n'est pas la 1.09 d'origine (modifié, ou d'une autre version). Le portage ne peut pas "
        "le patcher : le jeu tourne à 30 FPS en 16:9 (l'ultrawide et la résolution de sortie ne s'appliquent "
        "pas), les options de Jeu et effets n'ont aucun effet, et certaines scènes (comme la cinématique après "
        "Rom) peuvent se figer.\n\nRemplacez eboot.bin par celui d'une mise à jour 1.09 non modifiée.",
        'Points à placer : {}',
        '{} points en trop',
        "Votre eboot.bin a été modifié (par exemple avec un patch 60 FPS déjà appliqué). Une partie des patchs "
        "du portage ne peut pas être utilisée avec lui :\n\n{}\n\nPour toutes les options, remplacez eboot.bin "
        "par celui d'une mise à jour 1.09 non modifiée.",
        'Quartier de la cathédrale',
        "La même sauvegarde et le même parcours pour tous : à l'arrêt, un tour complet de caméra et une courte "
        "marche dans le Quartier de la cathédrale (environ 3 minutes). Pour comparer des PC et pour les "
        "rapports de bug : le rapport ajoute le détail du cache de textures et de la VRAM.",
    ],
    'es': [
        'Cruz', 'Círculo', 'Cuadrado', 'Triángulo', 'R3 (fijar objetivo)',
        'Cruceta abajo', 'Cruceta izquierda', 'Cruceta derecha', 'Panel táctil (clic izquierdo)',
        'Panel táctil (clic derecho)',
        'Caminar', 'Mayús derecha', 'Ctrl derecha', 'Alt izquierda', 'Alt derecha', 'Win derecha',
        'Meta derecha', 'Espacio',
        'Clic izquierdo', 'Clic derecho', 'Clic de rueda', 'Ratón atrás', 'Ratón adelante',
        'Rueda arriba', 'Rueda abajo', 'Rueda izquierda', 'Rueda derecha',
        'Cruceta ↑', 'Cruceta ↓', 'Cruceta ←', 'Cruceta →', 'Paleta L4', 'Paleta R4', 'Paleta L5', 'Paleta R5',
        'Renacer ante la Muñeca: vuelves a los atributos de tu origen por 5 de Perspicacia y recuperas los '
        'niveles gratis',
        'Auto (tarjetas de hasta 6 GB)',
        'Redistribuir',
        'Vuelve a repartir los puntos de atributo de un personaje: mismo nivel, mismos ecos de sangre. Cambia la '
        'partida con el juego cerrado; las partidas de antes van a tus copias de seguridad (Partidas).',
        'Asignaciones de teclado y ratón, hasta tres por acción, y debajo los botones del mando. Ambos '
        'funcionan a la vez.',
        '«Directo» gira la cámara del juego en ángulos exactos: sin aceleración del stick ni límite de '
        'velocidad de giro. Con un objetivo fijado, mover el ratón cambia de objetivo.',
        'Mando',
        'Haz clic en una casilla y luego pulsa el botón del mando para esa acción; Esc cancela. Clic derecho '
        'en una casilla restaura su valor predeterminado. Un botón en dos acciones pulsa ambas. Los sticks '
        'siempre mueven y giran la cámara.',
        'Perfil de poca VRAM',
        'Un presupuesto de texturas mayor y, a 1080p con un escalador, una escena más pequeña desde el inicio '
        '(cambiar el preajuste necesita entonces un reinicio).',
        'Primero cierra el juego: guardaría encima del cambio.',
        'Pulsa un botón…',
        'Ranura {}: {} (nivel {})',
        'No se encontró ningún mando. Conecta uno y vuelve a hacer clic en la casilla.',
        'Redistribuir un personaje…',
        'No se encontraron personajes en {}.',
        'Hecho. Hay una copia de las partidas de antes en {}.',
        'Restablecer',
        'Restablecer los botones del mando',
        'Perfil de poca VRAM de fixdag y bashiku.',
        'Benchmark para informes de errores…',
        'Pruebas de juego con el escenario Distrito de la Catedral: la misma partida y la misma ruta para todos.',
        'Vulkan (fsr4vk)',
        'AMD DX12',
        'Tu eboot.bin no es el 1.09 limpio (modificado, o de otra versión). El port no puede parchearlo: el '
        'juego funciona a 30 FPS en 16:9 (el ultrapanorámico y la resolución de salida no se aplican), las '
        'opciones de Juego y efectos no hacen nada y algunas escenas (como la cinemática tras Rom) pueden '
        'congelarse.\n\nSustituye eboot.bin por el de una actualización 1.09 sin modificar.',
        'Puntos por repartir: {}',
        '{} puntos de más',
        'Tu eboot.bin ha sido modificado (por ejemplo, con un parche de 60 FPS ya aplicado). Parte de los '
        'parches del port no se pueden usar con él:\n\n{}\n\nPara todas las opciones, sustituye eboot.bin por '
        'el de una actualización 1.09 sin modificar.',
        'Distrito de la Catedral',
        'La misma partida y la misma ruta para todos: quieto, un giro completo de cámara y un paseo corto por '
        'el Distrito de la Catedral (unos 3 minutos). Para comparar PC y para informes de errores: el informe '
        'añade los detalles de la caché de texturas y de la VRAM.',
    ],
    'pt': [
        'Xis', 'Bola', 'Quadrado', 'Triângulo', 'R3 (travar mira)',
        'Direcional para baixo', 'Direcional para a esquerda', 'Direcional para a direita',
        'Touchpad (clique esquerdo)', 'Touchpad (clique direito)',
        'Andar', 'Shift direito', 'Ctrl direito', 'Alt esquerdo', 'Alt direito', 'Win direito', 'Meta direito',
        'Espaço',
        'Clique esquerdo', 'Clique direito', 'Clique da roda', 'Mouse voltar', 'Mouse avançar',
        'Roda para cima', 'Roda para baixo', 'Roda para a esquerda', 'Roda para a direita',
        'Direcional ↑', 'Direcional ↓', 'Direcional ←', 'Direcional →', 'Paleta L4', 'Paleta R4', 'Paleta L5',
        'Paleta R5',
        'Renascer diante da Boneca: volta aos atributos da sua origem por 5 de Discernimento, e os níveis são '
        'recuperados de graça',
        'Auto (placas de até 6 GB)',
        'Redistribuir',
        'Redistribui os pontos de atributo de um personagem: mesmo nível, mesmos Ecos de Sangue. Altera o save com o '
        'jogo fechado; os saves de antes vão para os seus backups (Saves).',
        'Mapeamentos de teclado e mouse, até três por ação, e os botões do controle abaixo. Os dois funcionam '
        'ao mesmo tempo.',
        'O modo Direto gira a câmera do jogo em ângulos exatos: sem aceleração do analógico nem limite de '
        'velocidade de giro. Com o alvo travado, mover o mouse troca de alvo.',
        'Controle',
        'Clique em um campo e depois pressione o botão do controle para essa ação; Esc cancela. Clique com o '
        'botão direito em um campo para restaurar o padrão. Um botão em duas ações aciona as duas. Os '
        'analógicos sempre movem e giram a câmera.',
        'Perfil de pouca VRAM',
        'Um orçamento de texturas maior e, em 1080p com um upscaler, uma cena menor desde o início (mudar a '
        'predefinição então exige reiniciar).',
        'Feche o jogo primeiro: ele salvaria por cima da alteração.',
        'Pressione um botão…',
        'Espaço {}: {} (nível {})',
        'Nenhum controle encontrado. Conecte um e clique no campo de novo.',
        'Redistribuir um personagem…',
        'Nenhum personagem encontrado em {}.',
        'Pronto. Uma cópia dos saves de antes está em {}.',
        'Restaurar',
        'Restaurar os botões do controle',
        'Perfil de pouca VRAM por fixdag e bashiku.',
        'Benchmark para relatórios de bugs…',
        'Testes de jogo com o cenário Distrito da Catedral: o mesmo save e a mesma rota para todos.',
        'Vulkan (fsr4vk)',
        'AMD DX12',
        'O seu eboot.bin não é o 1.09 limpo (modificado, ou de outra versão). O port não consegue aplicar '
        'patches nele: o jogo roda a 30 FPS em 16:9 (ultrawide e a resolução de saída não se aplicam), as '
        'opções de Jogo e efeitos não fazem nada e algumas cenas (como a cinemática depois de Rom) podem '
        'travar.\n\nSubstitua o eboot.bin pelo de uma atualização 1.09 não modificada.',
        'Pontos a distribuir: {}',
        '{} pontos a mais',
        'O seu eboot.bin foi modificado (por exemplo, com um patch de 60 FPS já aplicado). Parte dos patches '
        'do port não pode ser usada com ele:\n\n{}\n\nPara todas as opções, substitua o eboot.bin pelo de uma '
        'atualização 1.09 não modificada.',
        'Distrito da Catedral',
        'O mesmo save e a mesma rota para todos: parado, um giro completo de câmera e uma caminhada curta no '
        'Distrito da Catedral (cerca de 3 minutos). Para comparar PCs e para relatórios de bugs: o relatório '
        'acrescenta os detalhes do cache de texturas e da VRAM.',
    ],
    'it': [
        'Croce', 'Cerchio', 'Quadrato', 'Triangolo', 'R3 (aggancio)',
        'Croce direzionale giù', 'Croce direzionale sinistra', 'Croce direzionale destra',
        'Touchpad (clic sinistro)', 'Touchpad (clic destro)',
        'Camminare', 'Maiusc destro', 'Ctrl destro', 'Alt sinistro', 'Alt destro', 'Win destro', 'Meta destro',
        'Spazio',
        'Clic sinistro', 'Clic destro', 'Clic rotellina', 'Mouse indietro', 'Mouse avanti',
        'Rotellina su', 'Rotellina giù', 'Rotellina sinistra', 'Rotellina destra',
        'Croce ↑', 'Croce ↓', 'Croce ←', 'Croce →', 'Paletta L4', 'Paletta R4', 'Paletta L5', 'Paletta R5',
        'Rinascita presso la Bambola: torni agli attributi della tua origine per 5 Intuizione, i livelli si '
        'riottengono gratis',
        'Auto (schede fino a 6 GB)',
        'Ridistribuisci',
        'Ridistribuisce i punti attributo di un personaggio: stesso livello, stessi Echi del sangue. Modifica il '
        'salvataggio a gioco chiuso; i salvataggi di prima finiscono nei tuoi backup (Salvataggi).',
        'Assegnazioni di tastiera e mouse, fino a tre per azione, e sotto i pulsanti del controller. Funzionano '
        'entrambi insieme.',
        'Diretto ruota la telecamera del gioco di angoli esatti: niente accelerazione dello stick né limite '
        'alla velocità di rotazione. Con il bersaglio agganciato, muovere il mouse cambia bersaglio.',
        'Controller',
        'Fai clic su una casella, poi premi il pulsante del controller per quell’azione; Esc annulla. Clic '
        'destro su una casella per ripristinare il valore predefinito. Un pulsante su due azioni le preme '
        'entrambe. Gli stick muovono e ruotano sempre la telecamera.',
        'Profilo VRAM ridotta',
        'Un budget per le texture più grande e, a 1080p con un upscaler, una scena più piccola fin dall’inizio '
        '(cambiare preset richiede poi un riavvio).',
        'Chiudi prima il gioco: salverebbe sopra la modifica.',
        'Premi un pulsante…',
        'Slot {}: {} (livello {})',
        'Nessun controller trovato. Collegane uno, poi fai di nuovo clic sulla casella.',
        'Ridistribuisci un personaggio…',
        'Nessun personaggio trovato in {}.',
        'Fatto. Una copia dei salvataggi di prima è in {}.',
        'Ripristina',
        'Ripristina i pulsanti del controller',
        'Profilo VRAM ridotta di fixdag e bashiku.',
        'Benchmark per le segnalazioni di bug…',
        'Test di gioco con lo scenario Distretto della Cattedrale: lo stesso salvataggio e lo stesso percorso '
        'per tutti.',
        'Vulkan (fsr4vk)',
        'AMD DX12',
        'Il tuo eboot.bin non è la 1.09 pulita (modificato, o di un’altra versione). Il port non può patcharlo: '
        'il gioco gira a 30 FPS in 16:9 (ultrawide e risoluzione di uscita non si applicano), le opzioni di '
        'Gioco ed effetti non fanno nulla e alcune scene (come il filmato dopo Rom) possono bloccarsi.\n\n'
        'Sostituisci eboot.bin con quello di un aggiornamento 1.09 non modificato.',
        'Punti da assegnare: {}',
        '{} punti di troppo',
        'Il tuo eboot.bin è stato modificato (per esempio con una patch a 60 FPS già applicata). Una parte delle '
        'patch del port non si può usare con esso:\n\n{}\n\nPer tutte le opzioni, sostituisci eboot.bin con '
        'quello di un aggiornamento 1.09 non modificato.',
        'Distretto della Cattedrale',
        'Lo stesso salvataggio e lo stesso percorso per tutti: fermo, un giro completo della telecamera e una '
        'breve camminata nel Distretto della Cattedrale (circa 3 minuti). Per confrontare PC e per le '
        'segnalazioni di bug: il resoconto aggiunge i dettagli della cache delle texture e della VRAM.',
    ],
    'pl': [
        'Krzyżyk', 'Kółko', 'Kwadrat', 'Trójkąt', 'R3 (namierzanie)',
        'Krzyżak w dół', 'Krzyżak w lewo', 'Krzyżak w prawo', 'Touchpad (lewe kliknięcie)',
        'Touchpad (prawe kliknięcie)',
        'Chód', 'Prawy Shift', 'Prawy Ctrl', 'Lewy Alt', 'Prawy Alt', 'Prawy Win', 'Prawy Meta', 'Spacja',
        'Lewy przycisk', 'Prawy przycisk', 'Kliknięcie kółkiem', 'Mysz wstecz', 'Mysz dalej',
        'Kółko w górę', 'Kółko w dół', 'Kółko w lewo', 'Kółko w prawo',
        'Krzyżak ↑', 'Krzyżak ↓', 'Krzyżak ←', 'Krzyżak →', 'Łopatka L4', 'Łopatka R4', 'Łopatka L5',
        'Łopatka R5',
        'Odrodzenie u Lalki: powrót do atrybutów twojego pochodzenia za 5 Wglądu, poziomy odzyskujesz za darmo',
        'Auto (karty do 6 GB)',
        'Przydziel od nowa',
        'Przydziel od nowa punkty atrybutów postaci: ten sam poziom, te same Echa krwi. Zmienia zapis, gdy gra jest '
        'zamknięta; wcześniejsze zapisy trafiają do twoich kopii zapasowych (Zapisy).',
        'Przypisania klawiatury i myszy, do trzech na akcję, a poniżej przyciski kontrolera. Oba działają '
        'jednocześnie.',
        'Sterowanie bezpośrednie obraca kamerę gry o dokładne kąty: bez przyspieszenia gałki i limitu '
        'prędkości obrotu. Podczas namierzania ruch myszy zmienia cel.',
        'Kontroler',
        'Kliknij pole, a potem naciśnij przycisk kontrolera dla tej akcji; Esc anuluje. Prawe kliknięcie pola '
        'przywraca domyślne. Przycisk przypisany do dwóch akcji naciska obie. Gałki zawsze poruszają postacią '
        'i obracają kamerę.',
        'Profil dla małej ilości VRAM',
        'Większy budżet tekstur i, w 1080p z upscalerem, mniejsza scena od początku (zmiana profilu wymaga '
        'wtedy ponownego uruchomienia).',
        'Najpierw zamknij grę: zapisałaby na tę zmianę.',
        'Naciśnij przycisk…',
        'Miejsce {}: {} (poziom {})',
        'Nie znaleziono kontrolera. Podłącz go i kliknij pole ponownie.',
        'Przydziel postać od nowa…',
        'Nie znaleziono postaci w {}.',
        'Gotowe. Kopia wcześniejszych zapisów jest w {}.',
        'Przywróć',
        'Przywróć przyciski kontrolera',
        'Profil dla małej ilości VRAM: fixdag i bashiku.',
        'Benchmark do zgłoszeń błędów…',
        'Testy gry ze scenariuszem Dzielnica Katedralna: ten sam zapis i ta sama trasa dla wszystkich.',
        'Vulkan (fsr4vk)',
        'AMD DX12',
        'Twój eboot.bin nie jest czystą wersją 1.09 (zmodyfikowany albo z innego wydania). Port nie może go '
        'załatać: gra działa w 30 FPS w 16:9 (ultrawide i rozdzielczość wyjściowa nie działają), opcje w Gra '
        'i efekty nic nie robią, a niektóre sceny (jak przerywnik po Rom) mogą się zawiesić.\n\nZastąp '
        'eboot.bin plikiem z niezmodyfikowanej aktualizacji 1.09.',
        'Punkty do rozdzielenia: {}',
        '{} pkt za dużo',
        'Twój eboot.bin został zmodyfikowany (na przykład z już nałożoną łatką 60 FPS). Części łatek portu nie '
        'da się z nim użyć:\n\n{}\n\nAby działały wszystkie opcje, zastąp eboot.bin plikiem z '
        'niezmodyfikowanej aktualizacji 1.09.',
        'Dzielnica Katedralna',
        'Ten sam zapis i ta sama trasa dla wszystkich: stanie, pełny obrót kamery i krótki spacer po Dzielnicy '
        'Katedralnej (około 3 minut). Do porównywania komputerów i zgłoszeń błędów: raport zawiera dodatkowo '
        'szczegóły pamięci tekstur i VRAM.',
    ],
    'tr': [
        'Çarpı', 'Daire', 'Kare', 'Üçgen', 'R3 (kilitlenme)',
        'Yön tuşu aşağı', 'Yön tuşu sol', 'Yön tuşu sağ', 'Dokunmatik yüzey (sol tık)',
        'Dokunmatik yüzey (sağ tık)',
        'Yürü', 'Sağ Shift', 'Sağ Ctrl', 'Sol Alt', 'Sağ Alt', 'Sağ Win', 'Sağ Meta', 'Boşluk',
        'Sol tık', 'Sağ tık', 'Tekerlek tıklaması', 'Fare geri', 'Fare ileri',
        'Tekerlek yukarı', 'Tekerlek aşağı', 'Tekerlek sola', 'Tekerlek sağa',
        'Yön tuşu ↑', 'Yön tuşu ↓', 'Yön tuşu ←', 'Yön tuşu →', 'Arka tuş L4', 'Arka tuş R4', 'Arka tuş L5',
        'Arka tuş R5',
        'Bebek’te yeniden doğuş: 5 İçgörü karşılığında kökeninizin niteliklerine dönersiniz, seviyeler ücretsiz '
        'geri kazanılır',
        'Otomatik (6 GB’a kadar kartlar)',
        'Yeniden dağıt',
        'Bir karakterin nitelik puanlarını yeniden dağıtın: aynı seviye, aynı Kan Yankıları. Oyun kapalıyken kaydı '
        'değiştirir; önceki kayıtlar yedeklerinize gider (Kayıtlar).',
        'Klavye ve fare atamaları, eylem başına üçe kadar, ve aşağıda kumandanın düğmeleri. İkisi birlikte '
        'çalışır.',
        '"Doğrudan", oyunun kamerasını tam açılarla döndürür: çubuk ivmesi veya dönüş hızı sınırı yoktur. '
        'Hedefe kilitliyken fareyi hareket ettirmek hedefi değiştirir.',
        'Kumanda',
        'Bir alana tıklayın, ardından o eylem için kumanda düğmesine basın; Esc iptal eder. Bir alana sağ '
        'tıklamak varsayılanı geri getirir. İki eyleme atanmış bir düğme ikisine de basar. Çubuklar her zaman '
        'hareket ettirir ve kamerayı döndürür.',
        'Düşük VRAM profili',
        'Daha büyük bir doku bütçesi ve, 1080p’de bir ölçekleyiciyle, baştan daha küçük bir sahne (ön ayar '
        'değişiklikleri o zaman yeniden başlatma gerektirir).',
        'Önce oyunu kapatın: değişikliğin üzerine kaydederdi.',
        'Bir düğmeye basın…',
        'Yuva {}: {} (seviye {})',
        'Kumanda bulunamadı. Bir tane bağlayın, sonra alana tekrar tıklayın.',
        'Bir karakteri yeniden dağıt…',
        '{} içinde karakter bulunamadı.',
        'Tamamlandı. Önceki kayıtların bir kopyası şurada: {}.',
        'Sıfırla',
        'Kumanda düğmelerini sıfırla',
        'Düşük VRAM profili: fixdag ve bashiku.',
        'Hata raporları için benchmark…',
        'Katedral Bölgesi senaryosuyla oyun testleri: herkes için aynı kayıt ve aynı rota.',
        'Vulkan (fsr4vk)',
        'AMD DX12',
        'eboot.bin dosyanız temiz 1.09 değil (değiştirilmiş veya başka bir sürümden). Port onu yamalayamaz: '
        'oyun 16:9’da 30 FPS çalışır (ultrawide ve çıkış çözünürlüğü uygulanmaz), Oyun ve efektler seçenekleri '
        'hiçbir şey yapmaz ve bazı sahneler (Rom’dan sonraki ara sahne gibi) donabilir.\n\neboot.bin’i '
        'değiştirilmemiş bir 1.09 güncellemesindekiyle değiştirin.',
        'Dağıtılacak puan: {}',
        '{} puan fazla',
        'eboot.bin dosyanız değiştirilmiş (örneğin 60 FPS yaması zaten uygulanmış). Portun yamalarının bir '
        'kısmı onunla kullanılamaz:\n\n{}\n\nTüm seçenekler için eboot.bin’i değiştirilmemiş bir 1.09 '
        'güncellemesindekiyle değiştirin.',
        'Katedral Bölgesi',
        'Herkes için aynı kayıt ve aynı rota: dururken, tam bir kamera dönüşü ve Katedral Bölgesi’nde kısa bir '
        'yürüyüş (yaklaşık 3 dakika). Bilgisayarları karşılaştırmak ve hata raporları için: rapor doku '
        'önbelleği ve VRAM ayrıntılarını ekler.',
    ],
    'ar': [
        'إكس', 'دائرة', 'مربع', 'مثلث', 'R3 (التثبيت على الهدف)',
        'الأسهم لأسفل', 'الأسهم لليسار', 'الأسهم لليمين', 'لوحة اللمس (نقر أيسر)', 'لوحة اللمس (نقر أيمن)',
        'المشي', 'Shift الأيمن', 'Ctrl الأيمن', 'Alt الأيسر', 'Alt الأيمن', 'Win الأيمن', 'Meta الأيمن',
        'المسافة',
        'نقر أيسر', 'نقر أيمن', 'نقر العجلة', 'زر الفأرة للخلف', 'زر الفأرة للأمام',
        'العجلة لأعلى', 'العجلة لأسفل', 'العجلة لليسار', 'العجلة لليمين',
        'الأسهم ↑', 'الأسهم ↓', 'الأسهم ←', 'الأسهم →', 'الزر الخلفي L4', 'الزر الخلفي R4', 'الزر الخلفي L5',
        'الزر الخلفي R5',
        'الولادة من جديد عند الدمية: العودة إلى سمات أصلك مقابل 5 بصيرة، وتستعيد المستويات مجانًا',
        'تلقائي (بطاقات حتى 6 غيغابايت)',
        'إعادة التوزيع',
        'أعد توزيع نقاط سمات الشخصية: المستوى نفسه وأصداء الدم نفسها. يغيّر ملف الحفظ واللعبة مغلقة؛ وتذهب ملفات '
        'الحفظ السابقة إلى نسخك الاحتياطية (ملفات الحفظ).',
        'تعيينات لوحة المفاتيح والفأرة، حتى ثلاثة لكل إجراء، وأزرار يد التحكم في الأسفل. يعمل الاثنان معًا.',
        'الوضع المباشر يدير كاميرا اللعبة بزوايا دقيقة: بلا تسارع للعصا ولا حد لسرعة الدوران. وأثناء التثبيت '
        'على الهدف، يغيّر تحريك الفأرة الهدف.',
        'يد التحكم',
        'انقر خانة، ثم اضغط زر يد التحكم لهذا الإجراء؛ Esc للإلغاء. انقر بزر الفأرة الأيمن على خانة لإعادة '
        'الافتراضي. الزر المعيّن لإجراءين يضغط الاثنين. العصوان تحرّكان الشخصية وتديران الكاميرا دائمًا.',
        'ملف ذاكرة الفيديو المنخفضة',
        'ميزانية أكبر للخامات، وفي 1080p مع أداة رفع الدقة، مشهد أصغر من البداية (تغيير الإعداد المسبق يحتاج '
        'حينها إلى إعادة تشغيل).',
        'أغلق اللعبة أولًا: ستحفظ فوق التغيير.',
        'اضغط زرًا…',
        'الخانة {}: {} (المستوى {})',
        'لم يُعثر على يد تحكم. صِل واحدة، ثم انقر الخانة مرة أخرى.',
        'إعادة توزيع شخصية…',
        'لم يُعثر على شخصيات في {}.',
        'تم. توجد نسخة من ملفات الحفظ السابقة في {}.',
        'إعادة تعيين',
        'إعادة تعيين أزرار يد التحكم',
        'ملف ذاكرة الفيديو المنخفضة من fixdag وbashiku.',
        'اختبار أداء لتقارير الأخطاء…',
        'اختبارات اللعبة بسيناريو حي الكاتدرائية: ملف الحفظ نفسه والمسار نفسه للجميع.',
        'Vulkan (fsr4vk)',
        'AMD DX12',
        'ملف eboot.bin لديك ليس نسخة 1.09 النظيفة (معدّل، أو من إصدار آخر). لا يستطيع المنفذ ترقيعه: تعمل '
        'اللعبة بـ 30 إطارًا في الثانية بنسبة 16:9 (لا تنطبق الشاشة العريضة ولا دقة الإخراج)، ولا تفعل خيارات '
        'اللعبة والمؤثرات شيئًا، وقد تتجمد بعض المشاهد (مثل المشهد السينمائي بعد روم).\n\nاستبدل eboot.bin '
        'بالملف من تحديث 1.09 غير معدّل.',
        'نقاط للتوزيع: {}',
        '{} نقاط زائدة',
        'تم تعديل ملف eboot.bin لديك (مثلًا بترقيعة 60 إطارًا مطبقة مسبقًا). لا يمكن استخدام جزء من ترقيعات '
        'المنفذ معه:\n\n{}\n\nلكل الخيارات، استبدل eboot.bin بالملف من تحديث 1.09 غير معدّل.',
        'حي الكاتدرائية',
        'ملف الحفظ نفسه والمسار نفسه للجميع: الوقوف، ودورة كاملة للكاميرا، ومشي قصير في حي الكاتدرائية (نحو 3 '
        'دقائق). لمقارنة الحواسيب ولتقارير الأخطاء: يضيف التقرير تفاصيل ذاكرة الخامات وذاكرة الفيديو.',
    ],
    'zh': [
        '叉键', '圆圈键', '方块键', '三角键', 'R3（锁定）',
        '方向键下', '方向键左', '方向键右', '触摸板（左键点击）', '触摸板（右键点击）',
        '步行', '右 Shift', '右 Ctrl', '左 Alt', '右 Alt', '右 Win', '右 Meta', '空格',
        '左键单击', '右键单击', '滚轮单击', '鼠标后退键', '鼠标前进键',
        '滚轮向上', '滚轮向下', '滚轮向左', '滚轮向右',
        '方向键 ↑', '方向键 ↓', '方向键 ←', '方向键 →', '背键 L4', '背键 R4', '背键 L5', '背键 R5',
        '在人偶处重生：花费 5 点启蒙回到出身的属性，等级可免费重新获得',
        '自动（6 GB 及以下显卡）',
        '重新分配',
        '重新分配角色的属性点：等级不变，血之回响不变。在游戏关闭时修改存档；之前的存档会放入你的备份（存档）。',
        '键盘和鼠标绑定，每个动作最多三个，下方是手柄的按键。两者可同时使用。',
        '“直接”模式按精确角度转动游戏镜头：没有摇杆加速，也没有转速上限。锁定目标时，移动鼠标会切换目标。',
        '手柄',
        '点击一个栏位，然后按下该动作对应的手柄按键；Esc 取消。右键点击栏位可恢复默认。绑定到两个动作的按键会同时触发两者。'
        '摇杆始终用于移动和转动镜头。',
        '低显存配置',
        '更大的纹理预算；在 1080p 并使用超分辨率时，从一开始就使用更小的场景（之后更改预设需要重启）。',
        '请先关闭游戏：否则它会覆盖这次更改。',
        '请按一个按键…',
        '栏位 {}：{}（等级 {}）',
        '未找到手柄。请连接一个，然后再次点击该栏位。',
        '重新分配角色…',
        '在 {} 中未找到角色。',
        '完成。之前存档的副本在 {}。',
        '重置',
        '重置手柄按键',
        '低显存配置由 fixdag 和 bashiku 制作。',
        '用于错误报告的基准测试…',
        '使用“大教堂区”场景的游戏测试：所有人使用相同的存档和路线。',
        'Vulkan (fsr4vk)',
        'AMD DX12',
        '你的 eboot.bin 不是干净的 1.09 版本（已修改，或来自其他发行版本）。移植无法给它打补丁：游戏以 30 FPS、16:9 运行'
        '（超宽屏和输出分辨率不生效），“游戏与效果”中的选项不起作用，某些场景（如罗姆之后的过场动画）可能会卡死。\n\n'
        '请用未修改的 1.09 更新中的 eboot.bin 替换它。',
        '待分配点数：{}',
        '多出 {} 点',
        '你的 eboot.bin 已被修改（例如已经打上了 60 FPS 补丁）。移植的部分补丁无法与它一起使用：\n\n{}\n\n'
        '要使用全部选项，请用未修改的 1.09 更新中的 eboot.bin 替换它。',
        '大教堂区',
        '所有人使用相同的存档和路线：站立、镜头完整转一圈，以及在大教堂区短暂步行（约 3 分钟）。用于比较电脑和错误报告：'
        '报告会额外包含纹理缓存和显存的详细信息。',
    ],
    'ja': [
        '×ボタン', '○ボタン', '□ボタン', '△ボタン', 'R3（ロックオン）',
        '方向キー下', '方向キー左', '方向キー右', 'タッチパッド（左クリック）', 'タッチパッド（右クリック）',
        '歩く', '右 Shift', '右 Ctrl', '左 Alt', '右 Alt', '右 Win', '右 Meta', 'スペース',
        '左クリック', '右クリック', 'ホイールクリック', 'マウス戻る', 'マウス進む',
        'ホイール上', 'ホイール下', 'ホイール左', 'ホイール右',
        '方向キー ↑', '方向キー ↓', '方向キー ←', '方向キー →', '背面ボタン L4', '背面ボタン R4', '背面ボタン L5',
        '背面ボタン R5',
        '人形のもとで生まれ直し：啓蒙 5 で素性の能力値に戻り、レベルは無料で取り戻せます',
        '自動（6 GB 以下のカード）',
        '振り直し',
        'キャラクターの能力値ポイントを振り直します。レベルも血の遺志も同じままです。ゲームを閉じている間にセーブデータを変更し、変更前のセーブデータはバックアップ（セーブデータ）に入ります。',
        'キーボードとマウスの割り当て（1 つの操作につき最大 3 つ）と、その下にコントローラーのボタン。両方を同時に使えます。',
        '「ダイレクト」はゲームのカメラを正確な角度で回します。スティックの加速や回転速度の上限はありません。ロックオン中は'
        'マウスを動かすとターゲットが切り替わります。',
        'コントローラー',
        '枠をクリックしてから、その操作に使うコントローラーのボタンを押してください。Esc でキャンセルします。枠を右クリックすると'
        '初期設定に戻ります。2 つの操作に割り当てたボタンは両方を押します。スティックは常に移動とカメラ操作に使われます。',
        '低 VRAM プロファイル',
        'テクスチャ予算を大きくし、1080p でアップスケーラーを使うときは最初から小さいシーンで描画します（その後のプリセット'
        '変更には再起動が必要です）。',
        '先にゲームを終了してください。変更の上から保存されてしまいます。',
        'ボタンを押してください…',
        'スロット {}：{}（レベル {}）',
        'コントローラーが見つかりません。接続してから、もう一度枠をクリックしてください。',
        'キャラクターを振り直す…',
        '{} にキャラクターが見つかりません。',
        '完了しました。以前のセーブデータのコピーは {} にあります。',
        'リセット',
        'コントローラーのボタンをリセット',
        '低 VRAM プロファイル：fixdag と bashiku。',
        '不具合報告用ベンチマーク…',
        '聖堂街シナリオのゲームテスト：全員が同じセーブデータと同じルートを使います。',
        'Vulkan (fsr4vk)',
        'AMD DX12',
        'お使いの eboot.bin はクリーンな 1.09 ではありません（改変済み、または別のリリースのもの）。移植版はこれにパッチを'
        '当てられません。ゲームは 16:9 の 30 FPS で動作し（ウルトラワイドと出力解像度は適用されません）、「ゲームとエフェクト」の'
        'オプションは効かず、一部の場面（ロマの後のムービーなど）でフリーズすることがあります。\n\neboot.bin を改変されていない '
        '1.09 アップデートのものに置き換えてください。',
        '割り振るポイント：{}',
        '{} ポイント多すぎます',
        'お使いの eboot.bin は改変されています（たとえば 60 FPS パッチが当たった状態）。移植版のパッチの一部はこれと一緒に'
        '使えません：\n\n{}\n\nすべてのオプションを使うには、eboot.bin を改変されていない 1.09 アップデートのものに'
        '置き換えてください。',
        '聖堂街',
        '全員が同じセーブデータと同じルート：静止、カメラの一回転、聖堂街での短い歩行（約 3 分）。PC の比較や不具合報告に。'
        'レポートにはテクスチャキャッシュと VRAM の詳細が加わります。',
    ],
    'ko': [
        'X 버튼', '○ 버튼', '□ 버튼', '△ 버튼', 'R3 (록온)',
        '방향 키 아래', '방향 키 왼쪽', '방향 키 오른쪽', '터치패드 (왼쪽 클릭)', '터치패드 (오른쪽 클릭)',
        '걷기', '오른쪽 Shift', '오른쪽 Ctrl', '왼쪽 Alt', '오른쪽 Alt', '오른쪽 Win', '오른쪽 Meta', '스페이스',
        '왼쪽 클릭', '오른쪽 클릭', '휠 클릭', '마우스 뒤로', '마우스 앞으로',
        '휠 위로', '휠 아래로', '휠 왼쪽', '휠 오른쪽',
        '방향 키 ↑', '방향 키 ↓', '방향 키 ←', '방향 키 →', '후면 버튼 L4', '후면 버튼 R4', '후면 버튼 L5',
        '후면 버튼 R5',
        '인형 앞에서 다시 태어나기: 통찰 5로 출신의 능력치로 돌아가며, 레벨은 무료로 되찾습니다',
        '자동 (6 GB 이하 그래픽카드)',
        '재분배',
        '캐릭터의 능력치 포인트를 다시 분배합니다: 레벨과 피의 메아리는 그대로입니다. 게임이 꺼져 있을 때 세이브를 바꾸며, 이전 세이브는 백업(세이브)에 들어갑니다.',
        '키보드와 마우스 할당(동작마다 최대 3개)과 아래의 컨트롤러 버튼. 둘 다 함께 작동합니다.',
        '직접 방식은 게임 카메라를 정확한 각도만큼 돌립니다: 스틱 가속이나 회전 속도 제한이 없습니다. 록온 중에는 '
        '마우스를 움직이면 대상이 바뀝니다.',
        '컨트롤러',
        '칸을 클릭한 다음 그 동작에 쓸 컨트롤러 버튼을 누르세요. Esc를 누르면 취소됩니다. 칸을 마우스 오른쪽 버튼으로 '
        '클릭하면 기본값으로 돌아갑니다. 두 동작에 할당된 버튼은 둘 다 누릅니다. 스틱은 항상 이동과 카메라 회전에 '
        '쓰입니다.',
        '저 VRAM 프로필',
        '더 큰 텍스처 예산과, 1080p에서 업스케일러를 쓸 때는 처음부터 더 작은 장면(이후 프리셋 변경에는 재시작이 '
        '필요합니다).',
        '먼저 게임을 종료하세요: 변경 위에 저장해 버립니다.',
        '버튼을 누르세요…',
        '슬롯 {}: {} (레벨 {})',
        '컨트롤러를 찾지 못했습니다. 하나를 연결한 다음 칸을 다시 클릭하세요.',
        '캐릭터 재분배…',
        '{}에서 캐릭터를 찾지 못했습니다.',
        '완료했습니다. 이전 세이브의 사본은 {}에 있습니다.',
        '초기화',
        '컨트롤러 버튼 초기화',
        '저 VRAM 프로필: fixdag, bashiku.',
        '버그 보고용 벤치마크…',
        '대성당 구역 시나리오의 게임 테스트: 모두에게 같은 세이브와 같은 경로.',
        'Vulkan (fsr4vk)',
        'AMD DX12',
        'eboot.bin이 깨끗한 1.09가 아닙니다(수정되었거나 다른 릴리스의 것). 포트가 패치할 수 없습니다: 게임은 16:9에서 '
        '30 FPS로 실행되고(울트라와이드와 출력 해상도가 적용되지 않음), 게임 및 효과의 옵션은 아무 효과가 없으며, '
        '일부 장면(롬 이후의 컷신 등)에서 멈출 수 있습니다.\n\neboot.bin을 수정되지 않은 1.09 업데이트의 것으로 '
        '바꾸세요.',
        '분배할 포인트: {}',
        '{} 포인트 초과',
        'eboot.bin이 수정되었습니다(예: 60 FPS 패치가 이미 적용됨). 포트의 패치 일부를 함께 쓸 수 없습니다:\n\n{}\n\n'
        '모든 옵션을 쓰려면 eboot.bin을 수정되지 않은 1.09 업데이트의 것으로 바꾸세요.',
        '대성당 구역',
        '모두에게 같은 세이브와 같은 경로: 서 있기, 카메라 한 바퀴, 대성당 구역에서 짧게 걷기(약 3분). PC 비교와 버그 '
        '보고용: 보고서에 텍스처 캐시와 VRAM 세부 정보가 더해집니다.',
    ],
}


# The regrouped pages (Language & sound, Online's own servers, Controls' fold-out, Picture's resolution).
PORT_UI_KEYS = ['A fixed route in Cathedral Ward for testers: one zip to send.', 'The benchmark plays a fixed route in Cathedral Ward on a save of its own, with your graphics settings, in fullscreen (about 3 minutes). Your saves and settings stay as they are.\n\nLeave the mouse, keyboard and controller alone until it ends. Then a zip to send opens in Explorer.', 'Benchmark finished. Send this file: {}', 'The benchmark stopped: {}', 'Launcher customization', "The launcher's language and look, what it does while you play, and shortcuts to the game.", 'While you play', "The game's language and sound, and changes to how it plays and its menus.", 'Starting the game and sound', 'Game patches for version 1.09, applied at start.', 'Controller settings', 'Keyboard and mouse settings', 'When the game starts', 'Closed, it ends with the game; minimized, it waits on the task bar (or in the tray).', 'Open the launcher again when the game closes', 'After a crash it always comes back, with the log.', 'Minimize to the system tray', 'Minimized, the launcher leaves the task bar for an icon next to the clock: click it to open the launcher again.', 'Gameplay', 'Changes to how the game plays and its menus. Game patches for version 1.09, applied at start.', 'Keyboard, mouse and controller: each has its own buttons, and both work side by side.', 'Mouse buttons and the scroll wheel are bound in the slots below: click a slot, then click or turn the wheel. They work while the mouse is captured (mouse look on).', 'Keyboard & mouse', 'Quit', 'In the game', 'Starting the game', 'Shortcuts', 'The desktop shortcut opens this launcher; the Steam entry starts the game straight away with these settings.', 'Sound', 'Use my own servers', 'Off: the public servers. On: a private server, with the addresses from the person who runs it.', 'Key combinations and reserved keys', 'Resolution', 'The size of the picture. Your goal picks it for this PC; change it here if you like.']
PORT_UI = {
    'de': ['Eine feste Route im Kathedralbezirk für Tester: eine ZIP-Datei zum Schicken.', 'Der Benchmark spielt eine feste Route im Kathedralbezirk mit einem eigenen Spielstand und deinen Grafikeinstellungen, im Vollbild (etwa 3 Minuten). Deine Spielstände und Einstellungen bleiben, wie sie sind.\n\nLass Maus, Tastatur und Controller in Ruhe, bis er endet. Dann öffnet sich eine ZIP-Datei zum Schicken im Explorer.', 'Benchmark fertig. Schick diese Datei: {}', 'Der Benchmark wurde abgebrochen: {}', 'Launcher anpassen', 'Sprache und Aussehen des Launchers, was er tut, während du spielst, und Verknüpfungen zum Spiel.', 'Während du spielst', 'Sprache und Ton des Spiels und Änderungen daran, wie es sich spielt, und an seinen Menüs.', 'Spielstart und Ton', 'Spielpatches für Version 1.09, beim Start angewendet.', 'Controller-Einstellungen', 'Tastatur- und Mauseinstellungen', 'Wenn das Spiel startet', 'Geschlossen endet er mit dem Spiel; minimiert wartet er in der Taskleiste (oder im Infobereich).', 'Launcher wieder öffnen, wenn das Spiel endet', 'Nach einem Absturz kommt er immer zurück, mit dem Protokoll.', 'In den Infobereich minimieren', 'Minimiert verlässt der Launcher die Taskleiste für ein Symbol neben der Uhr: Ein Klick darauf öffnet ihn wieder.', 'Spielweise', 'Änderungen daran, wie das Spiel sich spielt, und an seinen Menüs. Spielpatches für Version 1.09, beim Start angewendet.', 'Tastatur, Maus und Controller: Jeder hat seine eigenen Tasten, und beide funktionieren nebeneinander.', 'Maustasten und Mausrad werden in den Feldern unten belegt: Feld anklicken, dann klicken oder am Rad drehen. Sie wirken, solange die Maus gefangen ist (Mausblick an).', 'Tastatur & Maus', 'Beenden', 'Im Spiel', 'Spielstart', 'Verknüpfungen', 'Die Desktop-Verknüpfung öffnet diesen Launcher; der Steam-Eintrag startet das Spiel sofort mit diesen Einstellungen.', 'Ton', 'Eigene Server verwenden', 'Aus: die öffentlichen Server. An: ein privater Server, mit den Adressen von der Person, die ihn betreibt.', 'Tastenkombinationen und reservierte Tasten', 'Auflösung', 'Die Größe des Bildes. Dein Ziel wählt sie für diesen PC; hier kannst du sie ändern.'],
    'fr': ['Un parcours fixe dans le Quartier de la cathédrale pour les testeurs : un zip à envoyer.', 'Le benchmark joue un parcours fixe dans le Quartier de la cathédrale avec sa propre sauvegarde et vos réglages graphiques, en plein écran (environ 3 minutes). Vos sauvegardes et réglages restent tels quels.\n\nNe touchez ni à la souris, ni au clavier, ni à la manette jusqu’à la fin. Un zip à envoyer s’ouvre ensuite dans l’Explorateur.', 'Benchmark terminé. Envoyez ce fichier : {}', 'Le benchmark s’est arrêté : {}', 'Personnalisation du lanceur', 'La langue et l’apparence du lanceur, ce qu’il fait pendant que vous jouez, et des raccourcis vers le jeu.', 'Pendant que vous jouez', 'La langue et le son du jeu, et des changements dans la façon de jouer et dans ses menus.', 'Lancement du jeu et son', 'Patchs du jeu pour la version 1.09, appliqués au démarrage.', 'Réglages de la manette', 'Réglages du clavier et de la souris', 'Quand le jeu démarre', 'Fermé, il s’arrête avec le jeu ; réduit, il attend dans la barre des tâches (ou dans la zone de notification).', 'Rouvrir le lanceur à la fermeture du jeu', 'Après un plantage, il revient toujours, avec le journal.', 'Réduire dans la zone de notification', 'Réduit, le lanceur quitte la barre des tâches pour une icône près de l’horloge : cliquez dessus pour le rouvrir.', 'Gameplay', 'Des changements dans la façon de jouer et dans les menus du jeu. Patchs du jeu pour la version 1.09, appliqués au démarrage.', 'Clavier, souris et manette : chacun a ses propres boutons, et les deux fonctionnent ensemble.', 'Les boutons de la souris et la molette s’assignent dans les emplacements ci-dessous : cliquez sur un emplacement, puis cliquez ou tournez la molette. Ils fonctionnent tant que la souris est capturée (caméra à la souris activée).', 'Clavier et souris', 'Quitter', 'En jeu', 'Lancement du jeu', 'Raccourcis', 'Le raccourci du bureau ouvre ce lanceur ; l’entrée Steam lance directement le jeu avec ces réglages.', 'Son', 'Utiliser mes propres serveurs', 'Désactivé : les serveurs publics. Activé : un serveur privé, avec les adresses données par la personne qui le gère.', 'Combinaisons de touches et touches réservées', 'Résolution', 'La taille de l’image. Votre objectif la choisit pour ce PC ; vous pouvez la changer ici.'],
    'es': ['Una ruta fija en el Distrito de la Catedral para testers: un zip para enviar.', 'El benchmark recorre una ruta fija en el Distrito de la Catedral con su propia partida y tus ajustes gráficos, a pantalla completa (unos 3 minutos). Tus partidas y ajustes no cambian.\n\nNo toques el ratón, el teclado ni el mando hasta que termine. Después se abre en el Explorador un zip para enviar.', 'Benchmark terminado. Envía este archivo: {}', 'El benchmark se detuvo: {}', 'Personalización del lanzador', 'El idioma y el aspecto del lanzador, lo que hace mientras juegas y accesos directos al juego.', 'Mientras juegas', 'El idioma y el sonido del juego, y cambios en cómo se juega y en sus menús.', 'Inicio del juego y sonido', 'Parches del juego para la versión 1.09, aplicados al iniciar.', 'Ajustes del mando', 'Ajustes de teclado y ratón', 'Al iniciar el juego', 'Cerrado, termina con el juego; minimizado, espera en la barra de tareas (o en la bandeja).', 'Volver a abrir el lanzador al cerrar el juego', 'Tras un cierre inesperado siempre vuelve, con el registro.', 'Minimizar a la bandeja del sistema', 'Minimizado, el lanzador deja la barra de tareas por un icono junto al reloj: haz clic en él para volver a abrirlo.', 'Jugabilidad', 'Cambios en cómo se juega y en los menús del juego. Parches del juego para la versión 1.09, aplicados al iniciar.', 'Teclado, ratón y mando: cada uno tiene sus propios botones, y ambos funcionan a la vez.', 'Los botones del ratón y la rueda se asignan en las casillas de abajo: haz clic en una casilla y luego haz clic o gira la rueda. Funcionan mientras el ratón está capturado (vista con ratón activada).', 'Teclado y ratón', 'Salir', 'En el juego', 'Inicio del juego', 'Accesos directos', 'El acceso directo del escritorio abre este lanzador; la entrada de Steam inicia el juego directamente con estos ajustes.', 'Sonido', 'Usar mis propios servidores', 'Desactivado: los servidores públicos. Activado: un servidor privado, con las direcciones de quien lo gestiona.', 'Combinaciones de teclas y teclas reservadas', 'Resolución', 'El tamaño de la imagen. Tu objetivo la elige para este PC; puedes cambiarla aquí.'],
    'pt': ['Uma rota fixa no Distrito da Catedral para testers: um zip para enviar.', 'O benchmark percorre uma rota fixa no Distrito da Catedral com um save próprio e as suas configurações gráficas, em tela cheia (cerca de 3 minutos). Seus saves e configurações continuam como estão.\n\nNão mexa no mouse, no teclado nem no controle até terminar. Depois um zip para enviar abre no Explorer.', 'Benchmark concluído. Envie este arquivo: {}', 'O benchmark parou: {}', 'Personalização do launcher', 'O idioma e a aparência do launcher, o que ele faz enquanto você joga e atalhos para o jogo.', 'Enquanto você joga', 'O idioma e o som do jogo, e mudanças em como ele é jogado e nos seus menus.', 'Início do jogo e som', 'Patches do jogo para a versão 1.09, aplicados ao iniciar.', 'Configurações do controle', 'Configurações de teclado e mouse', 'Quando o jogo inicia', 'Fechado, ele termina junto com o jogo; minimizado, espera na barra de tarefas (ou na bandeja).', 'Abrir o launcher de novo quando o jogo fechar', 'Depois de um travamento ele sempre volta, com o log.', 'Minimizar para a bandeja do sistema', 'Minimizado, o launcher sai da barra de tarefas e vira um ícone perto do relógio: clique nele para abrir de novo.', 'Jogabilidade', 'Mudanças em como o jogo é jogado e nos seus menus. Patches do jogo para a versão 1.09, aplicados ao iniciar.', 'Teclado, mouse e controle: cada um tem seus próprios botões, e os dois funcionam ao mesmo tempo.', 'Os botões do mouse e a roda são mapeados nos campos abaixo: clique em um campo e depois clique ou gire a roda. Eles funcionam enquanto o mouse está capturado (câmera pelo mouse ligada).', 'Teclado e mouse', 'Sair', 'No jogo', 'Início do jogo', 'Atalhos', 'O atalho da área de trabalho abre este launcher; a entrada da Steam inicia o jogo direto com estas configurações.', 'Som', 'Usar meus próprios servidores', 'Desligado: os servidores públicos. Ligado: um servidor privado, com os endereços de quem o administra.', 'Combinações de teclas e teclas reservadas', 'Resolução', 'O tamanho da imagem. O seu objetivo escolhe para este PC; você pode mudar aqui.'],
    'it': ['Un percorso fisso nel Distretto della Cattedrale per i tester: uno zip da inviare.', 'Il benchmark percorre un tragitto fisso nel Distretto della Cattedrale con un salvataggio proprio e le tue impostazioni grafiche, a schermo intero (circa 3 minuti). I tuoi salvataggi e le impostazioni restano come sono.\n\nNon toccare mouse, tastiera e controller finché non finisce. Poi si apre in Esplora file uno zip da inviare.', 'Benchmark completato. Invia questo file: {}', 'Il benchmark si è interrotto: {}', 'Personalizzazione del launcher', 'La lingua e l’aspetto del launcher, cosa fa mentre giochi e i collegamenti al gioco.', 'Mentre giochi', 'La lingua e l’audio del gioco, e modifiche a come si gioca e ai suoi menu.', 'Avvio del gioco e audio', 'Patch del gioco per la versione 1.09, applicate all’avvio.', 'Impostazioni del controller', 'Impostazioni di tastiera e mouse', 'All’avvio del gioco', 'Chiuso, termina con il gioco; ridotto a icona, aspetta nella barra delle applicazioni (o nell’area di notifica).', 'Riapri il launcher quando il gioco si chiude', 'Dopo un crash torna sempre, con il registro.', 'Riduci nell’area di notifica', 'Ridotto a icona, il launcher lascia la barra delle applicazioni per un’icona vicino all’orologio: fai clic per riaprirlo.', 'Gameplay', 'Modifiche a come si gioca e ai menu del gioco. Patch del gioco per la versione 1.09, applicate all’avvio.', 'Tastiera, mouse e controller: ognuno ha i propri pulsanti, e funzionano insieme.', 'I pulsanti del mouse e la rotellina si assegnano nelle caselle qui sotto: fai clic su una casella, poi fai clic o gira la rotellina. Funzionano mentre il mouse è catturato (visuale col mouse attiva).', 'Tastiera e mouse', 'Esci', 'Nel gioco', 'Avvio del gioco', 'Collegamenti', 'Il collegamento sul desktop apre questo launcher; la voce di Steam avvia subito il gioco con queste impostazioni.', 'Audio', 'Usa i miei server', 'Disattivato: i server pubblici. Attivato: un server privato, con gli indirizzi di chi lo gestisce.', 'Combinazioni di tasti e tasti riservati', 'Risoluzione', 'La dimensione dell’immagine. Il tuo obiettivo la sceglie per questo PC; puoi cambiarla qui.'],
    'pl': ['Stała trasa w Dzielnicy Katedralnej dla testerów: jeden zip do wysłania.', 'Benchmark przechodzi stałą trasę w Dzielnicy Katedralnej na własnym zapisie z twoimi ustawieniami grafiki, na pełnym ekranie (około 3 minut). Twoje zapisy i ustawienia zostają bez zmian.\n\nNie dotykaj myszy, klawiatury ani kontrolera do końca. Potem w Eksploratorze otworzy się zip do wysłania.', 'Benchmark zakończony. Wyślij ten plik: {}', 'Benchmark został przerwany: {}', 'Dostosowanie launchera', 'Język i wygląd launchera, co robi podczas gry, oraz skróty do gry.', 'Podczas gry', 'Język i dźwięk gry oraz zmiany w sposobie grania i w jej menu.', 'Uruchamianie gry i dźwięk', 'Łatki gry dla wersji 1.09, nakładane przy uruchomieniu.', 'Ustawienia kontrolera', 'Ustawienia klawiatury i myszy', 'Gdy gra się uruchamia', 'Zamknięty kończy się razem z grą; zminimalizowany czeka na pasku zadań (lub w zasobniku).', 'Otwórz launcher ponownie po zamknięciu gry', 'Po awarii zawsze wraca, z dziennikiem.', 'Minimalizuj do zasobnika systemowego', 'Po zminimalizowaniu launcher znika z paska zadań i zostaje ikoną obok zegara: kliknij ją, aby go otworzyć.', 'Rozgrywka', 'Zmiany w sposobie grania i w menu gry. Łatki gry dla wersji 1.09, nakładane przy uruchomieniu.', 'Klawiatura, mysz i kontroler: każdy ma własne przyciski, a oba działają jednocześnie.', 'Przyciski myszy i kółko przypisuje się w polach poniżej: kliknij pole, a potem kliknij lub obróć kółko. Działają, gdy mysz jest przechwycona (rozglądanie myszą włączone).', 'Klawiatura i mysz', 'Zakończ', 'W grze', 'Uruchamianie gry', 'Skróty', 'Skrót na pulpicie otwiera ten launcher; wpis w Steam od razu uruchamia grę z tymi ustawieniami.', 'Dźwięk', 'Używaj własnych serwerów', 'Wyłączone: serwery publiczne. Włączone: serwer prywatny, z adresami od osoby, która go prowadzi.', 'Kombinacje klawiszy i zarezerwowane klawisze', 'Rozdzielczość', 'Rozmiar obrazu. Twój cel dobiera go do tego komputera; możesz go tu zmienić.'],
    'tr': ['Testçiler için Katedral Bölgesi’nde sabit bir rota: gönderilecek tek bir zip.', 'Benchmark, Katedral Bölgesi’nde kendi kaydıyla ve sizin grafik ayarlarınızla tam ekranda sabit bir rotayı oynar (yaklaşık 3 dakika). Kayıtlarınız ve ayarlarınız olduğu gibi kalır.\n\nBitene kadar fareye, klavyeye ve kumandaya dokunmayın. Ardından gönderilecek bir zip Gezgin’de açılır.', 'Benchmark bitti. Bu dosyayı gönderin: {}', 'Benchmark durdu: {}', 'Başlatıcı özelleştirme', 'Başlatıcının dili ve görünümü, siz oynarken ne yaptığı ve oyuna kısayollar.', 'Siz oynarken', 'Oyunun dili ve sesi, oynanışında ve menülerinde değişiklikler.', 'Oyunu başlatma ve ses', 'Sürüm 1.09 için oyun yamaları, başlangıçta uygulanır.', 'Kumanda ayarları', 'Klavye ve fare ayarları', 'Oyun başladığında', 'Kapatılırsa oyunla birlikte sona erer; simge durumunda görev çubuğunda (veya bildirim alanında) bekler.', 'Oyun kapanınca başlatıcıyı yeniden aç', 'Bir çökmeden sonra her zaman günlükle birlikte geri gelir.', 'Sistem tepsisine küçült', 'Küçültüldüğünde başlatıcı görev çubuğundan ayrılıp saatin yanında bir simge olur: yeniden açmak için tıklayın.', 'Oynanış', 'Oyunun oynanışında ve menülerinde değişiklikler. Sürüm 1.09 için oyun yamaları, başlangıçta uygulanır.', 'Klavye, fare ve kumanda: her birinin kendi düğmeleri vardır ve ikisi birlikte çalışır.', 'Fare düğmeleri ve kaydırma tekerleği aşağıdaki alanlarda atanır: bir alana tıklayın, ardından tıklayın veya tekerleği çevirin. Fare yakalanmışken (fareyle bakış açık) çalışırlar.', 'Klavye ve fare', 'Çık', 'Oyunda', 'Oyunu başlatma', 'Kısayollar', 'Masaüstü kısayolu bu başlatıcıyı açar; Steam kaydı oyunu bu ayarlarla doğrudan başlatır.', 'Ses', 'Kendi sunucularımı kullan', 'Kapalı: genel sunucular. Açık: işleten kişinin verdiği adreslerle özel bir sunucu.', 'Tuş kombinasyonları ve ayrılmış tuşlar', 'Çözünürlük', 'Görüntünün boyutu. Hedefiniz bunu bu bilgisayar için seçer; isterseniz burada değiştirin.'],
    'ar': ['مسار ثابت في حي الكاتدرائية للمختبرين: ملف zip واحد للإرسال.', 'يلعب اختبار الأداء مسارًا ثابتًا في حي الكاتدرائية بملف حفظ خاص به وبإعدادات الرسوميات لديك، بملء الشاشة (نحو 3 دقائق). تبقى ملفات حفظك وإعداداتك كما هي.\n\nلا تلمس الفأرة ولوحة المفاتيح ويد التحكم حتى ينتهي. بعدها يُفتح في المستكشف ملف zip للإرسال.', 'انتهى اختبار الأداء. أرسل هذا الملف: {}', 'توقف اختبار الأداء: {}', 'تخصيص المشغّل', 'لغة المشغّل ومظهره، وما يفعله أثناء لعبك، واختصارات إلى اللعبة.', 'أثناء اللعب', 'لغة اللعبة وصوتها، وتغييرات على طريقة اللعب وقوائمها.', 'تشغيل اللعبة والصوت', 'ترقيعات للإصدار 1.09 تُطبَّق عند البدء.', 'إعدادات يد التحكم', 'إعدادات لوحة المفاتيح والفأرة', 'عند بدء اللعبة', 'إذا أُغلق ينتهي مع اللعبة؛ وإذا صُغّر ينتظر في شريط المهام (أو في علبة النظام).', 'افتح المشغّل مجددًا عند إغلاق اللعبة', 'بعد أي انهيار يعود دائمًا، مع السجل.', 'التصغير إلى علبة النظام', 'عند التصغير يغادر المشغّل شريط المهام ويصبح أيقونة بجانب الساعة: انقرها لفتحه مجددًا.', 'أسلوب اللعب', 'تغييرات على طريقة اللعب وعلى قوائم اللعبة. ترقيعات للإصدار 1.09 تُطبَّق عند البدء.', 'لوحة المفاتيح والفأرة ويد التحكم: لكلٍّ أزراره الخاصة، ويعمل الاثنان معًا.', 'تُعيَّن أزرار الماوس وعجلة التمرير في الخانات أدناه: انقر خانة، ثم انقر أو أدر العجلة. تعمل عندما يكون الماوس محتجزًا داخل اللعبة (النظر بالماوس مفعّل).', 'لوحة المفاتيح والفأرة', 'إنهاء', 'داخل اللعبة', 'تشغيل اللعبة', 'الاختصارات', 'اختصار سطح المكتب يفتح هذا المشغّل؛ وإدخال Steam يشغّل اللعبة مباشرة بهذه الإعدادات.', 'الصوت', 'استخدام خوادمي الخاصة', 'معطّل: الخوادم العامة. مفعّل: خادم خاص، بالعناوين من الشخص الذي يديره.', 'تركيبات المفاتيح والمفاتيح المحجوزة', 'الدقة', 'حجم الصورة. هدفك يختارها لهذا الحاسوب؛ ويمكنك تغييرها هنا.'],
    'zh': ['给测试者的大教堂区固定路线：发送一个 zip 即可。', '基准测试会使用自带的存档和你的图形设置，在大教堂区以全屏跑一条固定路线（约 3 分钟）。你的存档和设置保持不变。\n\n在结束前请不要动鼠标、键盘和手柄。之后会在资源管理器中打开一个要发送的 zip。', '基准测试完成。请发送此文件：{}', '基准测试已停止：{}', '启动器自定义', '启动器的语言和外观、游戏时它的行为，以及游戏的快捷方式。', '游戏时', '游戏的语言和声音，以及对游戏玩法和菜单的修改。', '启动游戏与声音', '适用于 1.09 版本的游戏补丁，在启动时应用。', '手柄设置', '键盘和鼠标设置', '游戏启动时', '关闭后会随游戏一起结束；最小化后在任务栏（或系统托盘）中等待。', '游戏关闭后重新打开启动器', '崩溃后总会带着日志回来。', '最小化到系统托盘', '最小化后，启动器离开任务栏，变成时钟旁的图标：点击即可重新打开。', '玩法', '对游戏玩法和菜单的修改。适用于 1.09 版本的游戏补丁，在启动时应用。', '键盘、鼠标和手柄：各有各的按键，两者可同时使用。', '鼠标按键和滚轮在下方的栏位中绑定：点击一个栏位，然后点击鼠标或滚动滚轮。它们在鼠标被捕获时（鼠标视角开启）生效。', '键盘和鼠标', '退出', '游戏内', '启动游戏', '快捷方式', '桌面快捷方式会打开此启动器；Steam 条目会直接以这些设置启动游戏。', '声音', '使用我自己的服务器', '关闭：使用公共服务器。开启：使用私人服务器，地址由运营者提供。', '组合键与保留按键', '分辨率', '画面的尺寸。你的目标会为这台电脑选择它；也可以在这里更改。'],
    'ja': ['テスター向けの聖堂街の固定ルート：送る zip は 1 つだけです。', 'ベンチマークは専用のセーブデータとあなたのグラフィック設定で、聖堂街の固定ルートをフルスクリーンで走ります（約 3 分）。あなたのセーブデータと設定はそのままです。\n\n終わるまでマウス、キーボード、コントローラーに触れないでください。その後、送る zip がエクスプローラーで開きます。', 'ベンチマークが完了しました。このファイルを送ってください：{}', 'ベンチマークが中断されました：{}', 'ランチャーのカスタマイズ', 'ランチャーの言語と見た目、プレイ中の動作、ゲームへのショートカット。', 'プレイ中', 'ゲームの言語とサウンド、遊び方とメニューの変更。', 'ゲームの起動とサウンド', 'バージョン 1.09 用のゲームパッチで、起動時に適用されます。', 'コントローラー設定', 'キーボードとマウスの設定', 'ゲーム起動時', '閉じるとゲームと一緒に終了し、最小化するとタスクバー（またはトレイ）で待機します。', 'ゲーム終了後にランチャーを再表示', 'クラッシュ後は必ずログと一緒に戻ってきます。', 'システムトレイに最小化', '最小化すると、ランチャーはタスクバーから時計の横のアイコンになります。クリックすると再び開きます。', 'ゲームプレイ', 'ゲームの遊び方とメニューの変更。バージョン 1.09 用のゲームパッチで、起動時に適用されます。', 'キーボード、マウス、コントローラー：それぞれに専用のボタンがあり、同時に使えます。', 'マウスボタンとホイールは下の枠で割り当てます：枠をクリックしてから、クリックするかホイールを回してください。マウスがキャプチャされている間（マウス視点オン）に機能します。', 'キーボードとマウス', '終了', 'ゲーム内', 'ゲームの起動', 'ショートカット', 'デスクトップのショートカットはこのランチャーを開きます。Steam の項目はこの設定でゲームを直接起動します。', 'サウンド', '自分のサーバーを使う', 'オフ：公開サーバー。オン：運営者から教わったアドレスでプライベートサーバーを使います。', 'キーの組み合わせと予約済みキー', '解像度', '画面のサイズ。目標がこの PC に合わせて選びます。ここで変更することもできます。'],
    'ko': ['테스터를 위한 대성당 구역의 고정 경로: 보낼 zip 하나.', '벤치마크는 자체 세이브와 사용자의 그래픽 설정으로 대성당 구역의 고정 경로를 전체 화면으로 돕니다(약 3분). 사용자의 세이브와 설정은 그대로 유지됩니다.\n\n끝날 때까지 마우스, 키보드, 컨트롤러를 건드리지 마세요. 그런 다음 보낼 zip이 탐색기에서 열립니다.', '벤치마크가 끝났습니다. 이 파일을 보내세요: {}', '벤치마크가 중단되었습니다: {}', '런처 사용자 지정', '런처의 언어와 모양, 플레이 중 동작, 게임 바로 가기.', '플레이 중', '게임의 언어와 소리, 플레이 방식과 메뉴의 변경.', '게임 시작 및 소리', '1.09 버전용 게임 패치이며, 시작할 때 적용됩니다.', '컨트롤러 설정', '키보드 및 마우스 설정', '게임이 시작되면', '닫으면 게임과 함께 끝나고, 최소화하면 작업 표시줄(또는 트레이)에서 기다립니다.', '게임이 끝나면 런처 다시 열기', '충돌 후에는 항상 로그와 함께 돌아옵니다.', '시스템 트레이로 최소화', '최소화하면 런처가 작업 표시줄에서 시계 옆 아이콘으로 바뀝니다: 클릭하면 다시 열립니다.', '게임플레이', '게임 플레이 방식과 메뉴의 변경. 1.09 버전용 게임 패치이며, 시작할 때 적용됩니다.', '키보드, 마우스, 컨트롤러: 각자 버튼이 있고, 둘 다 함께 작동합니다.', '마우스 버튼과 스크롤 휠은 아래의 칸에서 지정합니다: 칸을 클릭한 다음 클릭하거나 휠을 돌리세요. 마우스가 캡처된 동안 (마우스 시점 켬) 작동합니다.', '키보드 및 마우스', '종료', '게임 안', '게임 시작', '바로 가기', '바탕 화면 바로 가기는 이 런처를 엽니다. Steam 항목은 이 설정으로 게임을 바로 시작합니다.', '소리', '내 서버 사용', '끄기: 공개 서버. 켜기: 운영자에게 받은 주소로 개인 서버를 사용합니다.', '키 조합과 예약된 키', '해상도', '화면의 크기. 목표가 이 PC에 맞게 고릅니다. 여기서 바꿀 수도 있습니다.'],
}

for _language, _texts in PORT.items():
    if len(_texts) != len(PORT_KEYS):
        raise ValueError(f'bbport_lang_port {_language}: {len(_texts)} texts for {len(PORT_KEYS)} keys')
    bbport_lang.EXTRA_TRANSLATIONS.setdefault(_language, {}).update(zip(PORT_KEYS, _texts))
for _language, _texts in PORT_UI.items():
    if len(_texts) != len(PORT_UI_KEYS):
        raise ValueError(f'bbport_lang_port {_language}: {len(_texts)} texts for {len(PORT_UI_KEYS)} keys')
    bbport_lang.EXTRA_TRANSLATIONS.setdefault(_language, {}).update(zip(PORT_UI_KEYS, _texts))
