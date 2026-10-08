# Tarefas: remap de teclado e controle e controle por mouse

Spec: [spec-design-keyboard-mouse-input.md](spec-design-keyboard-mouse-input.md) (v2.0, formato do
shadPS4). Os IDs entre colchetes apontam para requisitos e critérios de aceite da spec.

## Ordem e dependências

```text
T1 ──► T2 ──► T3 ──┐
                   ├──► T6 ──► T7 ──► T8 ──► T9
T4 ──────────► T5 ─┘
```

T1 e T4 podem começar em paralelo.

**Nota de ambiente (WSL2):** se `out/` do repositório estiver num disco Windows montado em
`/mnt/*` (9P/`drvfs`), o build pode travar a VM inteira sob a carga de I/O do Ninja, mesmo com
RAM e swap de sobra — não é falta de memória, é o driver 9P engasgando com muitos arquivos
pequenos simultâneos. Ocorreu uma vez nesta feature mesmo depois de ajustar `.wslconfig`
(RAM/cores/swap) para outro travamento anterior. Mitigação aplicada: mover `out/` para o
filesystem nativo da VM e deixar um symlink no lugar —
`mv out ~/bbport-build/out && ln -s ~/bbport-build/out out`, rodado uma vez a partir da raiz do
repositório. Os `.c`/`.cpp`-fonte continuam sendo lidos de `/mnt/d` (sem alternativa sem mover o
repositório inteiro), só os artefatos de build (`.o`, binários, o cache do CMake/Ninja) saem do
9P. Depois dessa mudança, o mesmo build rodou do início ao fim sem instabilidade. O symlink não
é rastreado pelo git; precisa ser recriado se o ambiente for refeito do zero.

---

## T1. Parser do `input.ini` no formato do shadPS4

- **Objetivo:** ler o arquivo de input com a sintaxe e os nomes do shadPS4 [CFG-001…007,
  NAM-001…005, HOT-001, HOT-002, GUD-001, GUD-004].
- **Arquivos:** `src/runtime_input_config.c` e `.h` (novos). **Feito** — implementados; ver estado
  de verificação ao final desta tarefa.
- **Nota sobre `build.sh`:** `runtime=(src/runtime*.c)` ([build.sh:82](../../build.sh#L82)) já
  inclui `src/runtime_input_config.c` por glob no build principal e em `test_runtime.c`
  ([build.sh:97](../../build.sh#L97)); nenhuma mudança foi necessária aí. Foi acrescentada uma linha
  nova própria de `input-config-test` (abaixo da de `pad-test`, [build.sh:94-98](../../build.sh#L94-L98)),
  que já linka `src/runtime_input_config.c` diretamente, sem depender do glob. A linha de `pad-test`
  em si ([build.sh:95](../../build.sh#L95)) ainda compila só `tests/test_pad.c` sozinho (sem
  `${runtime[@]}`, porque `test_pad.c` inclui `runtime_pad.c` via `#include`) — isso só se torna um
  problema quando T2 fizer `runtime_pad.c` chamar algo de `runtime_input_config.c`; ver T7.
- **Entregas:**
  - Tabelas portadas de `input_handler.h` do shadPS4 (teclas, mouse, botões, eixos), com cabeçalho
    SPDX e comentário de origem (commit `0fe263a`).
  - Parse com as regras de CFG-003…006: espaço removido, `#`, sufixo `:N` descartado, nomes sem
    diferenciar maiúsculas, combos e linhas fora do escopo ignorados com aviso.
  - Linhas e nomes rejeitados acima dos limites de GUD-004 (255 bytes por linha, 47 por nome), sem
    `scanf`/`sscanf` com `%s` sem largura.
  - Linhas especiais `mouse_to_joystick`, `mouse_movement_params` e `analog_deadzone`, com faixas.
  - Hotkeys F7 e F8 remapeáveis e teclas reservadas.
  - Caminho por `BB_INPUT_CONFIG` ou ao lado do `bbport.ini`; criação do arquivo padrão (seção 4.3)
    quando ele não existir.
  - Nomes de tecla convertidos para scancode com `SDL_GetScancodeFromKey`.
- **Aceite:** AC-002, AC-003, AC-008, AC-009 e os casos de borda da seção 9, em
  `tests/test_input_config.c` (novo, registrado em `build.sh --test`), com o arquivo padrão do
  shadPS4 em `tests/data/shadps4_default_input.ini`.
- **Estado de verificação: compilado e testado (2026-10-08), via `nix-shell shell.nix --run
  'BB_LTO=OFF bash build.sh --test'` no WSL2 do usuário.** `input-config-test` compila e os 22
  casos passam (`test_input_config: all tests passed`); `pad-test` e o resto da suíte C também
  passam sem regressão; `python3 -m unittest discover -s tests` roda os 82 testes Python, OK. A
  compilação real pegou 3 bugs que a revisão estática anterior não viu:
  1. Um comentário malformado em `runtime_input_config.c` (`/* ... hotkey_*/mouse_*/... */`) cujo
     `*/` do meio fechava o bloco antes da hora, virando erro de sintaxe C.
  2. `hotkey_reload_inputs = f9` não era rejeitado: a checagem de scancodes reservados
     (Insert/Escape/F9) só se aplicava a bindings de jogo, nunca à própria linha de hotkey.
     Corrigido extraindo `IsUnconditionallyReserved()`, reaproveitada nos dois lugares.
  3. `AddBinding` não detectava duplicata: `cross = space` duas vezes virava 2 bindings iguais em
     vez de 1 (a seção 9 da spec já previa isso). Corrigido com uma comparação antes de inserir.
  A causa de build ter ficado tão demorado/instável não foi deste código: era o WSL2 do usuário
  sem `.wslconfig` (7.7 GiB de RAM, 16 cores vistos pelo Ninja); resolvido subindo para 11 GiB/8
  cores/8 GiB de swap. Build final rodou com `BB_LTO=OFF` (não testado ainda com LTO ligado, que é
  o padrão de produção do `build.sh` — recomendado rodar ao menos uma vez antes do release final).
- **Depende de:** nada.

## T2. Bindings no `sample_host`: teclado e controle

- **Objetivo:** trocar as tabelas fixas pelos bindings de T1 [OUT-001…006, MIX-001…006].
- **Arquivos:** `src/runtime_pad.c`. **Feito** — ver estado de verificação ao final desta tarefa.
- **Entregas:**
  - Avaliação de cada binding por amostra: OU nos botões e soma em unidades de eixo nos sticks e
    gatilhos, com limite.
  - Limiar de 30 para gatilho como botão; metade do curso para meio eixo como botão.
  - O touchpad físico continua passando dedos e clique; `touchpad_left`, `touchpad_center` e
    `touchpad_right` geram o toque na posição correspondente.
  - Remove o `return` antecipado que hoje ignora o teclado quando há controle.
  - Comentário de cabeçalho de `runtime_pad.c` atualizado: o layout agora vem do `input.ini`.
- **Aceite:** AC-001, AC-004, AC-005, AC-006, AC-007, AC-021.
- **Estado de verificação: compilado e testado (2026-10-08)**, com LTO ligado e desligado.
  `test_binding_evaluation` (novo em `tests/test_pad.c`) exercita diretamente `button_output_held`/
  `axis_output_value` com um `HostState` montado à mão (o driver SDL `dummy` não entrega teclas
  reais), cobrindo MIX-002 (duas teclas no mesmo botão) e a soma de eixos. `test_custom_binding_config`
  cobre o caminho completo via `BB_INPUT_CONFIG` + `pad_read_state`. O `pad-test` antigo (controle
  virtual, `BB_PAD_FILE`) continua passando sem regressão. Dois erros de compilação apareceram e
  foram corrigidos nesta tarefa: `-Werror=misleading-indentation` (dois `if` na mesma linha) e
  `-lm` faltando na linha de link de `pad-test` em `build.sh` (nova dependência de `math.h`,
  trazida junto por T5 já nesta mesma rodada de implementação).
- **Depende de:** T1.

## T3. Zona morta e halfmode

- **Objetivo:** aplicar `analog_deadzone` e `*joystick_halfmode` depois da soma [DZN-001, HLF-001].
- **Arquivos:** `src/runtime_pad.c`. **Feito** — ver estado de verificação ao final desta tarefa.
- **Entregas:** fórmula de `ApplyDeadzone` do shadPS4 portada; multiplicação por 0.5 enquanto o
  binding de halfmode estiver ativo.
- **Aceite:** AC-017 e um teste de zona morta com valores exatos.
- **Estado de verificação: compilado e testado (2026-10-08).** `test_binding_evaluation` confirma
  a rampa exata (`apply_deadzone(60,10,100)==70`), os extremos (`==0` no limite interno, `==127`
  no limite externo) e que uma tecla plena satura bem acima do limite externo. O halfmode é
  testado só até a resolução do binding (`button_output_held` em `OUT_LEFTJOYSTICK_HALFMODE`); a
  divisão por 2 em si, dentro de `sample_host`, não tem asserção isolada — fica coberta
  indiretamente pelos testes end-to-end, não por um valor exato de halfmode aplicado.
- **Depende de:** T2.

## T4. Infraestrutura do mouse e dos hotkeys na biblioteca de GPU

- **Objetivo:** capturar o mouse e tratar F7/F8 na thread da janela, e expor isso ao runtime
  [MOU-002…008, HOT-003, API-001, API-002].
- **Arquivos:** `gpu/bbgpu.h`, `gpu/shim/bbgpu.cpp`, `gpu/shim/window.cpp`, `gpu/shim/sdl_window.h`.
  **Feito** — implementados; ver estado de verificação ao final desta tarefa.
- **Entregas:**
  - `bbgpu_mouse_take`, `bbgpu_input_configure` e `bbgpu_input_reload_requested` (seção 4.5).
  - Em `PollEvents`:
    - tratar as teclas de toggle e reload por evento, antes de qualquer outro consumidor (F8 precisa
      funcionar com o menu aberto);
    - tratar `SDL_EVENT_WINDOW_FOCUS_GAINED/LOST` para MOU-003;
    - acumular movimento, botões e pulsos de roda de 33 ms só durante a captura (checada pelo estado
      da iteração anterior, `mouse_captured_last` — um evento de abrir o menu pode, por até uma
      chamada de `PollEvents`, ainda ver a captura antiga; aceito como caso de borda de um frame, sem
      reavaliar a condição por evento);
    - reavaliar a condição de captura uma vez ao fim de cada `PollEvents` (não só nos eventos que
      poderiam mudá-la, porque o menu também fecha pela thread de render) e só chamar
      `SDL_SetWindowRelativeMouseMode` quando ela mudar; ao sair da captura, zerar o acumulado e
      chamar `SDL_ShowCursor()` explicitamente (o modo relativo documenta esconder o cursor ao
      ligar, não devolvê-lo ao desligar).
  - `BbMouseInput.buttons` usa bits de `SDL_BUTTON_MASK()`; os bindings de `runtime_input_config.c`
    guardam o **índice** `SDL_BUTTON_*` (não a máscara) — T5 precisa converter com
    `SDL_BUTTON_MASK(valor)` antes de comparar. Documentado nos dois headers.
  - MOU-002 ("o modo mouse começa ligado"): só se aplica à primeira chamada de
    `bbgpu_input_configure` com o modo mouse disponível, nunca a uma releitura por F8 — senão um F8
    religaria um mouse que o jogador tinha desligado com F7.
- **Aceite:** AC-010, AC-011, AC-012, AC-013, AC-019 (manuais).
- **Estado de verificação: compilado com sucesso (2026-10-08)**, junto do build de T1 — `libbbgpu.so`
  linkou sem erro com `window.cpp`, `sdl_window.h`, `bbgpu.cpp` e `bbgpu.h` alterados. T4 não tem
  teste automatizado próprio (os critérios de aceite são manuais, T9), então compilar confirma só
  a sintaxe e as assinaturas, não o comportamento em runtime — os nomes/assinaturas de API usados
  (`SDL_SetWindowRelativeMouseMode`, `SDL_ShowCursor`, `SDL_BUTTON_MASK`, campos de
  `SDL_MouseWheelEvent`) já tinham sido confirmados contra a documentação oficial do SDL3 antes de
  escrever o código, e a convenção de sinal da roda bate com `GetMouseWheelEvent` do shadPS4 (y>0
  cima, x>0 direita). A validação comportamental (captura abre/fecha o cursor, hotkeys, foco) só
  acontece jogando, depois que T5/T6 a consumirem — nada disso foi testado ainda.
- **Depende de:** nada (T1 só fornece os valores para `bbgpu_input_configure`).

## T5. Mouse para stick e botões do mouse

- **Objetivo:** converter o estado do mouse em contribuições para o pad [MOU-006, MOU-009,
  CNV-001…003].
- **Arquivos:** `src/runtime_pad.c`. **Feito** — ver estado de verificação ao final desta tarefa.
- **Entregas:**
  - Fórmula `EmulateJoystick` do shadPS4 com normalização por `dt` para 33 ms (seção 4.4).
  - Contribuição somada ao stick de `mouse_to_joystick`, **depois** da zona morta do stick (T3),
    não antes: a fórmula já tem seu próprio piso de velocidade mínima
    (`mouse_movement_params`'s `deadzone_offset`), então aplicar `apply_deadzone` à soma
    resultante teria aplicado a zona morta duas vezes à mesma contribuição.
  - Entradas de mouse (`leftbutton`…, `mousewheel*`) avaliadas como qualquer binding, só com
    `captured = 1` (`HostState.mouse_buttons`/`mouse_wheel` vêm zerados quando não capturado).
  - Chamada a `bbgpu_input_configure` após carregar a configuração (em `ensure_config_loaded` e em
    `reload_config_if_requested`, T6).
- **Aceite:** AC-014, AC-015, AC-016, AC-020 (pelo stub em `test_pad.c`).
- **Estado de verificação: compilado e testado (2026-10-08).** `test_mouse_to_axis` reproduz o
  exemplo exato da spec (AC-016: `dx=10` a 33 ms → `x=64,y=0`), a invariância por `dt` diferente
  (AC-015, dentro de ±2 unidades) e a saturação diagonal a 45°. `test_mouse_buttons_and_wheel`
  cobre a conversão índice→máscara (`SDL_BUTTON_MASK`) e o layout de bits da roda. `test_mouse_end_to_end`
  cobre o fluxo completo via `BB_INPUT_CONFIG` + um stub controlável de `bbgpu_mouse_take` (novo em
  `test_pad.c`) + `pad_read_state`, incluindo MOU-006 (botão do mouse inerte com `captured=0`). Um
  erro de link (`-lm` ausente) e um de `-Wmissing-field-initializers` (um `HostState` antigo do
  teste não tinha os 2 campos novos de mouse) apareceram e foram corrigidos.
- **Revisão pós-uso (2026-10-08, mesma sessão): dois problemas reais na fórmula, achados jogando,
  não por teste automatizado — os valores de AC-016 estavam certos, mas a "sensação" não.**
  1. **"Microsaltos" na câmera.** A fórmula original do shadPS4 usa um piso de velocidade mínima
     (`deadzone_offset*128`) como `max(linear, piso)`: assim que há qualquer movimento, a
     velocidade salta direto para o piso (ex. 64 de 128). No shadPS4 isso é mascarado por ele
     amostrar num timer fixo de ~30 Hz; neste port, a 150 FPS, um movimento lento do mouse gera
     deltas por amostra pequenos o bastante para a câmera alternar entre 0 e o piso a cada
     frame — sentido como pequenos saltos em vez de suavidade. Primeira correção: trocar o piso
     rígido por uma curva racional (`floor*mag/(mag+crossing)`) que sobe suavemente a partir de
     zero e se junta à reta original no ponto em que o piso antigo entraria em ação.
  2. **"Ainda sinto um pouco de degrau"** (relatado pelo usuário depois de testar a correção
     acima). A curva racional da primeira correção nunca alcança de fato o piso — só se aproxima
     dele assintoticamente — então a troca de "usar a curva" para "usar a reta" no ponto de
     cruzamento ainda era uma troca abrupta de valor (confirmado numericamente: salto de ~32
     para 64 entre duas magnitudes a 0.01 de distância). Corrigido de vez trocando o branch por
     uma única fórmula sem costura: `speed(mag) = (mag*speed+offset) * (1 - exp(-mag/k))` — o
     fator exponencial vai suavemente de 0 a 1, então a curva *converge* para a reta em vez de
     *trocar* para ela; não há mais nada para saltar entre. Verificado com uma varredura de
     magnitude 0 a 200 em passos de 0.5 (novo teste em `test_mouse_to_axis`), confirmando que o
     salto entre dois pontos consecutivos nunca passa de 1 unidade de stick em toda a faixa —
     esse teste de varredura é o que teria pego o bug 2 antes de chegar ao jogo; os testes
     anteriores só verificavam valores pontuais, que por acaso não caíam perto do salto.
  A mudança foi validada com um programa C standalone (só a função, sem SDL3/o resto do projeto)
  compilado com `gcc` puro no WSL, já que o build completo via Nix estava travando o WSL
  repetidamente nesta sessão (ver nota de ambiente no topo deste arquivo); o código real em
  `src/runtime_pad.c` foi então compilado e testado de verdade pelo usuário via MSYS2 CLANG64
  (Windows nativo), não pelo WSL.
- **Depende de:** T1, T2, T4.

## T6. Releitura com F8

- **Objetivo:** reler o `input.ini` sem reiniciar [CFG-008, GUD-002].
- **Arquivos:** `src/runtime_pad.c`, `src/runtime_input_config.c`. **Feito** — a lógica já tinha
  sido escrita junto de T2 (`reload_config_if_requested`, chamada no início de `sample()`); só
  faltava o teste real, adicionado agora.
- **Entregas:**
  - No início de `sample()`, se `bbgpu_input_reload_requested()` retornar 1: carregar uma
    configuração nova, trocá-la sob o lock do pad, liberar a antiga e chamar
    `bbgpu_input_configure`.
  - Mensagem no log com o número de bindings e de avisos.
- **Aceite:** AC-018.
- **Estado de verificação: compilado e testado (2026-10-08).** `test_reload_on_f8` (novo em
  `tests/test_pad.c`) edita o arquivo apontado por `BB_INPUT_CONFIG` em disco, aciona um stub
  controlável de `bbgpu_input_reload_requested` (que drena para 0 como o real) e confirma que o
  binding de `cross` muda de J para Espaço numa única chamada de `pad_read_state`, sem reiniciar o
  processo — a saída do build confirma `"... loaded"` seguido de `"... reloaded"` no mesmo
  arquivo.
- **Depende de:** T2, T5.

## T7. Testes

- **Objetivo:** cobrir automaticamente o que não depende do jogo rodando. **Feito** — a maior
  parte foi escrita junto de T2/T5/T6, à medida que cada uma precisava de um teste real (não só
  "compila"); esta tarefa ficou sendo a consolidação e a confirmação final.
- **Arquivos:** `tests/test_pad.c`, `tests/test_input_config.c`, `tests/data/`, `build.sh`.
- **Entregas:**
  - Stubs controlados de `bbgpu_mouse_take` e `bbgpu_input_reload_requested` em `test_pad.c`
    (drenam como os reais); `bbgpu_input_configure` só registra os parâmetros recebidos, o que
    basta porque nada no runtime depende do retorno dela.
  - Seis funções de teste novas em `test_pad.c`: `test_binding_evaluation` (remap, OR entre
    fontes, zona morta exata, resolução do halfmode), `test_mouse_to_axis` (fórmula
    `EmulateJoystick`, valor exato de AC-016, invariância por `dt` de AC-015, saturação
    diagonal), `test_mouse_buttons_and_wheel` (conversão índice→máscara, bits da roda),
    `test_mouse_end_to_end`, `test_custom_binding_config` e `test_reload_on_f8` — os três últimos
    via `pad_read_state` de ponta a ponta com `BB_INPUT_CONFIG`.
  - **Build de `pad-test`:** resolvido pela opção 1 do problema original — `src/runtime_input_config.c`
    e `-lm` (nova dependência de `math.h` trazida por T5) foram adicionados à linha de compilação
    de `pad-test` em `build.sh`.
  - A linha de `test_input_config.c` em `build.sh --test` já existia desde T1.
- **Aceite:** AC-021, AC-022.
- **Estado de verificação: confirmado (2026-10-08)** no mesmo build que fechou T1–T6: todos os
  testes C (`pad-test`, `input-config-test`, `runtime-test`, `sema-test`, `content-test`,
  `file-mods-test`) e os 82 testes Python passam, com `BB_LTO=OFF` e com `BB_LTO=ON` (o padrão de
  produção).
- **Depende de:** T3, T5, T6.

## T8. Documentação

- **Objetivo:** documentar o arquivo, a importação do shadPS4 e as mudanças de comportamento.
  **Feito.**
- **Arquivos:** `README.md` (seção de teclas), `docs/INPUT.md` (novo).
- **Entregas:**
  - Formato e lista de saídas e entradas.
  - Como importar: copiar `user/input_config/CUSA03173.ini` (ou `default.ini`) do shadPS4 para
    `input.ini`.
  - O que é ignorado (combos, `key_toggle`, outros hotkeys).
  - Teclas reservadas, F7 e F8, e a limitação CON-004.
  - Aviso no README: o teclado agora funciona junto do controle, Q é lock-on e V é Triangle.
- **Decisão:** não criei `docs/CHANGES_<data>.md`. Os existentes (`CHANGES_2026-10-02.md`,
  `CHANGES_2026-10-03.md`) são em russo e documentam medições de benchmark detalhadas (RX 7800 XT,
  taskset, FPS); são um registro pessoal do mantenedor original, não um requisito rígido de
  processo — a própria entrega já dizia "se o projeto mantiver um". O README (inglês) e este
  `docs/INPUT.md` (inglês) já cobrem o que a tarefa pedia; forçar esse formato específico para
  esta feature não pareceu agregar.
- **Depende de:** T7.

## T9. Teste manual e fechamento

- **Objetivo:** validar no jogo, no Windows e no Linux, o que os testes automáticos não cobrem.
- **Roteiro:**
  1. Sem `input.ini`: o arquivo é criado; controle e teclado funcionam juntos; Q faz lock-on e V faz
     Triangle; o mouse não é capturado.
  2. Descomentar `mouse_to_joystick = right`: câmera com o mouse a 30, 60 e FPS destravado, com a
     mesma sensação; clique esquerdo = ataque leve, direito = ataque forte, laterais = Circle/Square.
  3. Abrir o menu com Insert e com L3+R3; fechar com Esc e com o botão "Fechar". O cursor aparece e
     some em cada caso.
  4. Alt+Tab e voltar; F7 desliga e liga o mouse; clicar com o mouse desligado não ataca.
  5. Caixa de nome do personagem: o cursor fica solto e o texto não vaza para o jogo.
  6. Editar o `input.ini` com o jogo aberto e apertar F8: a mudança vale na hora.
  7. Copiar o arquivo de Bloodborne de uma instalação real do shadPS4 e jogar com ele.
  8. `BB_PAD_RECORD` e `BB_PAD_REPLAY` com mouse: a gravação reproduz a câmera.
  9. Linux Wayland: a captura funciona ou o problema fica registrado como limitação.
- **Aceite:** AC-010…AC-013, AC-019, com o resultado de cada item registrado no PR.
- **Depende de:** T8.

---

## Fase 2 (fora desta etapa)

- **Tela de remap de teclado no launcher Windows: feita** (2026-10-08) —
  [launcher/bbport_input_config.py](../../launcher/bbport_input_config.py) (parse/escrita do
  `input.ini`, espelhando `runtime_input_config.c`) e a aba "Controls" em
  `launcher/bbport_launcher_win.py` (`build_controls`, captura de tecla por `bind_all` +
  `grab_set`, botões Set/Clear por ação). Só teclado, uma tecla por ação (sem combo — ver
  abaixo); controle e mouse continuam editados só via `input.ini` direto. Testado via lógica
  pura (`load_input_ini`/`save_input_ini`, incluindo o caso de borda que achou um bug real:
  sobrescrever sem querer uma linha de controller ao adicionar um binding de teclado na mesma
  ação); a UI em si (Tkinter) não pôde ser testada nesta sessão (sem display aqui), só pelo
  usuário no Windows.
  - Ainda falta: tela equivalente no launcher Linux (GTK); menu do jogo (ImGui, os dois
    sistemas, aplicação imediata com o mecanismo do F8).
- Botão "abrir input.ini" e "importar do shadPS4" nos launchers.
- **Combos de tecla (ex. Ctrl+E para uma ação).** Pedido pelo usuário em 2026-10-08; adiado a
  pedido dele mesmo. Duas pontas, nenhuma feita:
  1. **Parser C** (`src/runtime_input_config.c`): hoje CFG-005 rejeita qualquer vírgula na
     entrada de uma linha (trata como combo fora de escopo, aviso e ignora). Precisa aceitar
     `tecla1,tecla2` e exigir as duas pressionadas ao mesmo tempo para a ação disparar —
     equivalente ao combo do shadPS4, sem o `key_toggle` dele (modificador vira auto-hold).
  2. **Launcher:** `start_remap_capture` fecha a captura no primeiro `KeyPress`; para combo
     precisaria acumular modificadoras (Shift/Ctrl/Alt) seguradas até a tecla principal ser
     solta, e mostrar o combo formado antes de confirmar.
- `key_toggle`, mouse como giroscópio e como touchpad.
- `BbSettings::Save()` preservando chaves desconhecidas do `bbport.ini`: deixou de ser necessário
  para esta feature, mas continua sendo uma melhoria útil.
