Contexto: repositório bbport (port nativo de Bloodborne; fork Windows de Supermedo). Quero adicionar
(1) remap configurável de teclado e botões de controle e (2) captura de mouse (câmera + botões).
NÃO use o launcher nem a UI nesta etapa; só código + chaves no bbport.ini.

Arquivos relevantes (leia antes de editar):
- src/runtime_pad.c: sample_host() (linhas ~92-151) tem as tabelas fixas. Com controle conectado
  usa só o controle; o teclado só vale SEM controle. sample() aplica injeção/replay depois.
- gpu/shim/window.cpp: laço SDL_PollEvent (thread da janela); BbOverlay::HandleEvent é chamado antes
  do switch e retorna true quando consome o evento.
- gpu/shim/bbport_overlay.cpp: HandleEvent trata SDL_EVENT_MOUSE_* só para o menu ImGui (Insert / L3+R3),
  retorna false com o menu fechado. bbgpu_overlay_captures_input() (gpu/bbgpu.h) indica menu aberto.
- gpu/shim/bbport_settings.cpp/.h: parser do bbport.ini (um if (key == ...) por chave); BB_CONFIG
  sobrescreve o caminho. runtime_pad.c é C e roda na thread do pad.

Requisitos:
1. Remap de teclado: substituir a tabela fixa por uma tabela em memória com os padrões atuais
   (WASD move, setas câmera, Space=Cross, LShift=Circle, E=Square, Q=Triangle, 1/3=L1/R1, R/F=L2/R2,
   Z/C=L3/R3, IJKL=D-pad, Enter=Options, Tab/Backspace=touchpad). Ler do bbport.ini chaves como
   key_cross=SPACE, key_move_up=W, key_cam_left=LEFT, ... usando SDL_GetScancodeFromName.
   Chave ausente ou inválida = padrão (logar aviso uma vez).
2. Remap de botões do controle: chaves pad_cross=SOUTH etc. via SDL_GetGamepadButtonFromString,
   substituindo a tabela das linhas ~102-115.
3. Teclado e controle devem funcionar JUNTOS (somar as entradas), não só como fallback.
4. Mouse:
   - Acumular xrel/yrel de SDL_EVENT_MOUSE_MOTION em atomics na thread da janela (window.cpp);
     NÃO chamar SDL_GetRelativeMouseState na thread do pad.
   - Ativar SDL_SetWindowRelativeMouseMode(true) e esconder o cursor quando o menu está fechado;
     desativar ao abrir o menu (Insert / L3+R3) para o ImGui voltar a usar o cursor. Também
     desativar ao perder o foco da janela.
   - Em sample_host(), converter o delta acumulado (consumindo-o a cada leitura) em stick direito
     (câmera), com centro 128, decaindo para 128 quando o mouse para. Chaves: mouse_enable=0|1,
     mouse_sens (float), mouse_invert_y, mouse_deadzone.
   - Botões do mouse como botões PS4, configuráveis: mouse_left=R1 (ataque leve), mouse_right=R2
     (ataque forte; ao mapear R2 setar também d->r2=255), mouse_middle=L3, mouse_x1=Circle,
     mouse_x2=Square. [Para direito = L1 (arma de fogo/aparar), troque o padrão para mouse_right=L1.]
   - Eventos de botão do mouse devem alimentar atomics/bitmask lidos pelo pad; com o menu aberto
     o jogo recebe entrada neutra (comportamento atual).
5. Compatibilidade: sem nenhuma chave nova no .ini o comportamento atual (teclado só sem controle
   inclusive) deve continuar igual, exceto o mouse, que fica desligado por padrão (mouse_enable=0)
   — me diga se preferir ligado por padrão.
6. Manter BB_PAD_FILE / BB_PAD_RECORD / BB_PAD_REPLAY funcionando.
7. Documentar as novas chaves no README (seção de teclas) e em docs/.

Restrições: mudanças mínimas e no estilo do código existente; sem dependências novas. Build Linux
com `bash build.sh` e testes com `bash build.sh --test` e `python3 -m unittest discover -s tests`.
Se possível, adicione testes para o parse das chaves. O build Windows (MSYS2 CLANG64) eu faço depois.
Ao final, liste os arquivos alterados e como testar manualmente (câmera, ataques, abrir/fechar menu).