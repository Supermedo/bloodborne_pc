---
title: Remap de teclado e controle e controle por mouse (câmera e botões)
version: 2.0
date_created: 2026-10-07
last_updated: 2026-10-07
owner: bbport (fork Windows)
tags: [design, input, pad, mouse, keyboard, config, shadps4-compat]
---

# Introdução

Esta especificação define o remap configurável de teclado e de controle e o controle por mouse
(câmera pelo stick emulado e botões do mouse como botões do PS4) no bbport. A configuração fica num
arquivo de input próprio (`input.ini`), com a sintaxe e a semântica do arquivo de input do shadPS4,
para que um arquivo de Bloodborne feito para o shadPS4 funcione aqui. Nesta etapa não há tela de
remap; o arquivo é editado à mão.

**Referência:** shadPS4, commit `0fe263a` (2026-10-07): `src/input/input_handler.{h,cpp}`,
`src/input/input_mouse.cpp`, `src/sdl_window.cpp`. Ambos os projetos são GPL-2.0-or-later, então
tabelas e trechos podem ser copiados com o cabeçalho SPDX e a marca `bbport:` nas mudanças, como já
é feito com o código vendorizado em `gpu/shadps4/`.

## 1. Propósito e escopo

**Propósito:** jogar com teclado e mouse ao estilo do shadPS4, aproveitando configurações feitas
para ele, sem perder o comportamento de quem joga só com controle.

**Dentro do escopo:**

- Arquivo `input.ini` com a sintaxe do shadPS4, gerado com os padrões do bbport se não existir.
- Remap do teclado, dos botões e eixos do controle e dos botões e roda do mouse para as saídas do PS4.
- Teclado, controle e mouse funcionando ao mesmo tempo.
- Mouse como stick (`mouse_to_joystick`), com a fórmula e os parâmetros do shadPS4.
- Modo meia-velocidade dos sticks (`leftjoystick_halfmode`, `rightjoystick_halfmode`).
- Zona morta configurável dos sticks e gatilhos (`analog_deadzone`).
- Hotkeys F7 (liga e desliga o mouse) e F8 (relê o `input.ini`), remapeáveis.
- Testes automatizados e documentação.

**Fora do escopo:**

- Tela de remap no menu do jogo ou no launcher (fase 2).
- Combos de várias teclas (`lalt, f12`), `key_toggle`, os demais hotkeys do shadPS4, mouse como
  giroscópio (F6) e como touchpad (Delete).
- Configuração separada por controle (sufixo `:N`), arquivo `global.ini` e perfis por jogo.
- Mira 1:1 com o mouse (CON-004).
- Mudanças em `BbSettings::Save()`: o `input.ini` é separado e o menu do jogo não o regrava.

**Público:** desenvolvedores e agentes LLM que implementam as tarefas de `tasks.md`.

**Premissas:** SDL3 é a única dependência de input. O código compila no Linux (GCC, `build.sh`) e no
Windows (MSYS2 CLANG64).

## 2. Definições

| Termo | Definição |
|---|---|
| Pad / `PadData` | Estrutura `OrbisPadData` devolvida ao jogo por `scePadReadState` ([src/runtime_pad.c](../../src/runtime_pad.c)). Sticks em `uint8_t` com centro 128; gatilhos `l2`/`r2` de 0 a 255. |
| Saída | O que o jogo enxerga: um botão do PS4, uma direção de eixo, um eixo analógico inteiro, um modo (halfmode) ou um hotkey. É o lado esquerdo de uma linha do `input.ini`. |
| Entrada | Uma tecla, um botão ou eixo do controle, ou um botão ou passo da roda do mouse. É o lado direito de uma linha. |
| Binding | Uma linha `saída = entrada`. Uma saída pode ter vários bindings (várias linhas). |
| Modo mouse | Estado ligado ou desligado da emulação de stick pelo mouse, alternado por F7. |
| Captura | Modo relativo do SDL ativo (`SDL_SetWindowRelativeMouseMode`): cursor oculto e preso à janela. |
| Thread da janela | A thread que roda `WindowSDL::PollEvents` ([gpu/shim/window.cpp](../../gpu/shim/window.cpp)), a única que pode chamar funções de vídeo e janela do SDL. |
| Thread do pad | A thread do jogo que chama `scePadReadState`; nela roda `sample()` em `runtime_pad.c`. |
| Menu | O menu de configurações do bbport em ImGui (Insert ou L3+R3). |
| Caixa de texto | A entrada de nome do personagem desenhada sobre o jogo (`SetTextEntry`). |
| Unidade de eixo | Valor com sinal de -127 a 127 usado na soma dos eixos; 0 é o centro. Na saída vira `128 + v`, limitado a 0..255. |

## 3. Requisitos, restrições e diretrizes

### Arquivo e sintaxe

- **CFG-001**: O caminho é `BB_INPUT_CONFIG`, se definido e não vazio. Senão, `input.ini` na mesma
  pasta do `bbport.ini`, cujo caminho segue a regra de `BbSettings::Path()` (`BB_CONFIG`, senão
  `bbport.ini` no diretório atual).
- **CFG-002**: Se o arquivo não existir, o bbport o cria com o conteúdo da seção 4.3 e o usa. Se não
  conseguir criar, usa os mesmos padrões em memória e registra um aviso.
- **CFG-003**: A sintaxe é a do shadPS4:
  - todo espaço em branco da linha é removido antes do parse;
  - `#` começa um comentário até o fim da linha;
  - cada linha é `saída = entrada`;
  - um sufixo `:N` na saída ou na entrada é aceito e descartado;
  - linhas vazias são ignoradas.
- **CFG-004**: Os nomes são comparados sem diferenciar maiúsculas de minúsculas. É um superconjunto
  do shadPS4, que só aceita minúsculas.
- **CFG-005**: Cada linha tem uma única entrada. Uma entrada com vírgula (combo) é ignorada com
  aviso. A exceção são as linhas especiais da seção 4.2, que têm parâmetros separados por vírgula.
- **CFG-006**: Linhas com saída ou entrada desconhecidas, `key_toggle`, hotkeys fora de F7 e F8 e
  `override_controller_color` são ignoradas com um aviso por linha no carregamento. O resto do
  arquivo continua valendo, para que arquivos do shadPS4 importem sem erro.
- **CFG-007**: Sem nenhuma linha de controle, o controle não aciona nada, como no shadPS4. O arquivo
  padrão traz as linhas de controle.
- **CFG-008**: O arquivo é lido em `scePadOpen` e relido quando o usuário aperta a tecla de
  `hotkey_reload_inputs` (padrão F8). A releitura troca a configuração inteira de uma vez, entre duas
  amostras do pad, sem estado intermediário visível ao jogo.

### Nomes

- **NAM-001**: Os nomes de tecla são os da tabela `string_to_keyboard_key_map` do shadPS4
  (`a`…`z`, `0`…`9`, `f1`…`f12`, `space`, `enter`, `tab`, `backspace`, `lshift`, `lctrl`, `lalt`,
  `up`, `left`, `kp0`…`kp9`, `comma`, `period` e os demais). Cada nome corresponde a uma tecla
  lógica do SDL (`SDLK_*`), que é convertida para scancode com `SDL_GetScancodeFromKey` no
  carregamento. Assim os nomes seguem o layout do teclado, como no shadPS4.
- **NAM-002**: Nomes de mouse: `leftbutton`, `rightbutton`, `middlebutton`, `sidebuttonback`,
  `sidebuttonforward`, `mousewheelup`, `mousewheeldown`, `mousewheelleft`, `mousewheelright`.
- **NAM-003**: Nomes de botão do controle (tabela `string_to_cbutton_map`), por posição física:
  `cross` = sul, `circle` = leste, `square` = oeste, `triangle` = norte, mais `l1`, `r1`, `l3`, `r3`,
  `options`, `pad_up`, `pad_down`, `pad_left`, `pad_right`, `back` e `share` (ambos o botão Back do
  SDL), `lpaddle_high`, `lpaddle_low`, `rpaddle_high`, `rpaddle_low`, `l4`, `l5`, `r4`, `r5`, `qam`.
- **NAM-004**: Nomes de eixo do controle (tabela `string_to_axis_map`): `l2`, `r2` (gatilhos),
  `axis_left_x`, `axis_left_y`, `axis_right_x`, `axis_right_y` (eixo inteiro) e
  `axis_left_x_plus` / `_minus` e equivalentes (meio eixo).
- **NAM-005**: `unmapped` como entrada cria um binding que nunca dispara. Serve para deixar explícito
  que uma saída não tem tecla.

### Saídas

- **OUT-001**: As saídas válidas estão na seção 4.1. Botões, direções de eixo e eixos inteiros
  seguem os nomes do shadPS4. Os três lados do touchpad (`touchpad_left`, `touchpad_center`,
  `touchpad_right`) geram um toque em x = 480, 960 ou 1440, com y = 471.
- **OUT-002**: O touchpad físico do controle continua passando a posição real dos dedos e o clique,
  como hoje ([runtime_pad.c:118-130](../../src/runtime_pad.c#L118-L130)), independente de bindings.
  Como no shadPS4, o botão físico `touchpad` não pode ser remapeado.
- **OUT-003**: Um botão ou tecla ligado a `l2` ou `r2` aciona o bit do botão e põe o analógico em 255.
- **OUT-004**: Um gatilho físico ligado a uma saída de botão aciona o botão quando passa de 30 em
  0..255, o limiar atual do bbport. Ligado a `l2`/`r2`, passa o valor analógico e aciona o bit acima
  de 30.
- **OUT-005**: Um meio eixo físico (`axis_left_x_plus` etc.) ligado a uma saída de botão aciona o
  botão acima de metade do curso, como no shadPS4 (`0x40` de `0x7f`).
- **OUT-006**: A combinação L3+R3 que abre o menu usa os botões físicos, independente do remap.

### Combinação

- **MIX-001**: Teclado, controle e mouse são lidos em toda amostra, com ou sem controle conectado.
- **MIX-002**: Botões: basta um binding ativo para a saída ficar ativa (OU).
- **MIX-003**: Eixos, como no shadPS4: as contribuições de todos os bindings ativos (teclas e meio
  eixos com ±127, eixos físicos com o valor lido, mouse com o valor da seção 4.4) são somadas em
  unidades de eixo. Depois vêm a zona morta (`analog_deadzone`) e o fator do halfmode, e o resultado
  é limitado a -127..127. Direções opostas se anulam.
- **MIX-004**: Gatilhos analógicos: soma das contribuições, limitada a 0..255.
- **MIX-005**: A ordem de `sample()` não muda: amostra do host (teclado, controle e mouse
  combinados), depois gravação (`BB_PAD_RECORD`), injeção (`BB_PAD_FILE`) e replay
  (`BB_PAD_REPLAY`). A gravação registra o estado já combinado, mouse incluído.
- **MIX-006**: Com o menu ou a caixa de texto abertos, o jogo recebe entrada neutra, e o atraso de
  liberação depois do fechamento (`hold_after_capture`) continua valendo.

### Halfmode e zona morta

- **HLF-001**: Enquanto um binding de `leftjoystick_halfmode` (ou `rightjoystick_halfmode`) estiver
  ativo, a saída do stick correspondente é multiplicada por 0.5 depois da zona morta, como no
  shadPS4. O arquivo padrão não traz tecla para isso.
- **DZN-001**: `analog_deadzone = <leftjoystick|rightjoystick|l2|r2>, <interno>, <externo>`, com
  valores de 1 a 127. Abaixo do interno a saída é 0; do interno ao externo cresce linearmente até
  127; acima do externo é 127. A fórmula é a de `ApplyDeadzone` do shadPS4. O padrão é `1, 127`,
  praticamente sem zona morta, que equivale ao comportamento atual.

### Mouse

- **MOU-001**: `mouse_to_joystick = left|right` escolhe o stick do mouse. Sem essa linha o modo
  mouse não existe, F7 não faz nada e o cursor nunca é capturado. O arquivo padrão traz a linha
  comentada, então o mouse começa desligado.
- **MOU-002**: Com `mouse_to_joystick` definido, o modo mouse começa ligado ao abrir o jogo. A tecla
  de `hotkey_toggle_mouse_to_joystick` (padrão F7) alterna entre ligado e desligado, e o estado não
  é salvo entre sessões.
- **MOU-003**: A captura fica ativa se e somente se todas estas condições forem verdadeiras: o modo
  mouse está ligado, a janela tem foco, o menu está fechado e a caixa de texto está inativa.
- **MOU-004**: A thread da janela avalia MOU-003 a cada `PollEvents` e chama
  `SDL_SetWindowRelativeMouseMode` só quando o resultado muda. O menu também fecha pela thread de
  render ([bbport_overlay.cpp:396](../../gpu/shim/bbport_overlay.cpp#L396)), então a decisão não
  pode depender só dos eventos de abrir e fechar.
- **MOU-005**: Só durante a captura a thread da janela acumula `xrel`/`yrel`, o estado dos botões e
  os passos da roda. Quando a captura termina, tudo é zerado e nenhum botão fica preso.
- **MOU-006**: Botões e roda do mouse só acionam saídas durante a captura. Clicar na janela para dar
  foco nunca dispara uma ação. Isso diverge do shadPS4.
- **MOU-007**: Cada passo da roda mantém a entrada `mousewheel*` ativa por 33 ms, como no shadPS4.
  Passos seguidos se emendam.
- **MOU-008**: A thread do pad consome o acumulado a cada amostra (lê e zera) por
  `bbgpu_mouse_take` (seção 4.5), e nunca chama funções de mouse do SDL.
- **MOU-009**: A conversão para o stick usa a fórmula do shadPS4 (seção 4.4) e os parâmetros de
  `mouse_movement_params`. O deslocamento é normalizado para uma janela de 33 ms, para que a
  sensibilidade não dependa do FPS e os valores do shadPS4 se transfiram direto.

### Teclas reservadas e hotkeys

- **HOT-001**: `Insert` e `Escape` (menu) e `F9` (`BB_PAD_RECORD`) são reservadas. Um binding de
  saída do jogo para elas é ignorado com aviso.
- **HOT-002**: `hotkey_toggle_mouse_to_joystick` (padrão `f7`) e `hotkey_reload_inputs` (padrão
  `f8`) podem ser remapeados no `input.ini`. A tecla atribuída a eles também passa a ser reservada.
  Se o arquivo não tiver uma dessas linhas, vale o padrão.
- **HOT-003**: Os hotkeys são tratados na thread da janela, por eventos de tecla, e não pela leitura
  do pad. Eles funcionam com o menu aberto (F8) e com a captura desligada (F7).

### Compatibilidade e restrições

- **CON-001**: Não entram dependências novas. Só SDL3 e a biblioteca padrão.
- **CON-002**: O código segue o estilo existente: C11 em `src/`, C++ em `gpu/shim/`, comentários no
  mesmo tom e densidade. Tabelas copiadas do shadPS4 mantêm o cabeçalho SPDX e a origem num
  comentário.
- **CON-003**: `BB_PAD_FILE`, `BB_PAD_RECORD` e `BB_PAD_REPLAY` continuam funcionando com o mesmo
  formato de arquivo.
- **CON-004**: O mouse é emulado como stick. A rotação satura na velocidade máxima de câmera do jogo
  e herda a aceleração dele. Isso é limitação conhecida, documentada, e não bug.
- **CON-005**: Comparado à versão atual, sem `input.ini` (gerado com os padrões) o comportamento muda
  só no previsto: o teclado funciona junto do controle (MIX-001), Q passa a ser R3 e Triangle vai
  para V, e o mouse fica desligado.
- **CON-006**: Compila no Linux e no Windows sem `#ifdef` novos de plataforma para a lógica de input.

### Diretrizes

- **GUD-001**: O parse fica num módulo C próprio (`src/runtime_input_config.c`), testável sem GPU e
  sem janela. As tabelas de nomes do shadPS4 são portadas para arrays C.
- **GUD-002**: A configuração carregada é imutável. A releitura (F8) monta uma estrutura nova e só
  então troca o ponteiro sob o lock do pad, que já protege `sample()`
  ([src/runtime_pad.c:58](../../src/runtime_pad.c#L58)). O carregamento em si (I/O de disco, parse)
  roda fora do lock: o mutex do pad também é adquirido por `pad_read_state` a cada frame
  ([src/runtime_pad.c:313-319](../../src/runtime_pad.c#L313-L319)), e I/O sob esse lock travaria a
  leitura do pad do jogo enquanto o arquivo é lido.
- **GUD-003**: A tabela de nomes de `BB_PAD_FILE` ([runtime_pad.c:181-186](../../src/runtime_pad.c#L181-L186))
  passa a usar os mesmos valores de saída do módulo, sem mudar os nomes aceitos pela injeção.

## 4. Interfaces e contratos de dados

### 4.1 Saídas

| Saída | Tipo | Efeito no `PadData` |
|---|---|---|
| `cross` `circle` `square` `triangle` `l1` `r1` `l3` `r3` `options` | botão | bit em `buttons` |
| `pad_up` `pad_down` `pad_left` `pad_right` | botão | bit do D-pad em `buttons` |
| `l2` `r2` | botão + gatilho | bit em `buttons` e `l2`/`r2` (OUT-003, OUT-004) |
| `touchpad_left` `touchpad_center` `touchpad_right` | touchpad | `BTN_TOUCHPAD` e um toque em x=480/960/1440, y=471 |
| `axis_left_x_minus` `axis_left_x_plus` `axis_left_y_minus` `axis_left_y_plus` | meio eixo | contribuição -127 ou +127 em `left_x`/`left_y` |
| `axis_right_x_minus` … `axis_right_y_plus` | meio eixo | o mesmo em `right_x`/`right_y` |
| `axis_left_x` `axis_left_y` `axis_right_x` `axis_right_y` | eixo inteiro | contribuição do eixo físico ligado |
| `leftjoystick_halfmode` `rightjoystick_halfmode` | modo | HLF-001 |
| `hotkey_toggle_mouse_to_joystick` `hotkey_reload_inputs` | hotkey | HOT-002 |

### 4.2 Linhas especiais

| Linha | Parâmetros | Padrão sem a linha |
|---|---|---|
| `mouse_to_joystick = left\|right` | stick do mouse | modo mouse inexistente |
| `mouse_movement_params = <deadzone_offset>, <speed>, <speed_offset>` | três floats | `0.5, 1, 0.125` |
| `analog_deadzone = <dispositivo>, <interno>, <externo>` | `leftjoystick`/`rightjoystick`/`l2`/`r2`, 1..127, 1..127 | `1, 127` |

Faixas válidas de `mouse_movement_params`: `deadzone_offset` e `speed_offset` de 0.0 a 1.0; `speed`
de 0.01 a 20.0. Um valor fora da faixa invalida a linha inteira, que fica no padrão com aviso.

### 4.3 Arquivo padrão (`input.ini` gerado)

```ini
# bbport input (shadPS4 syntax: output = input, one input per line).
# A shadPS4 input config (user/input_config/CUSA03173.ini) can be copied over this file.
# F7 toggles the mouse, F8 reloads this file.

# Keyboard
cross = space
circle = lshift
square = e
triangle = v
l1 = 1
r1 = 3
l2 = r
r2 = f
l3 = z
r3 = q
r3 = c
options = enter
pad_up = i
pad_down = k
pad_left = j
pad_right = l
touchpad_left = tab
touchpad_right = backspace

axis_left_x_minus = a
axis_left_x_plus = d
axis_left_y_minus = w
axis_left_y_plus = s
axis_right_x_minus = left
axis_right_x_plus = right
axis_right_y_minus = up
axis_right_y_plus = down

# Hold to halve the left stick (walk): uncomment and pick a key
# leftjoystick_halfmode = lalt

# Mouse (uncomment mouse_to_joystick to turn it on; buttons work only while captured)
# mouse_to_joystick = right
mouse_movement_params = 0.5, 1, 0.125
r1 = leftbutton
r2 = rightbutton
circle = sidebuttonback
square = sidebuttonforward

# Controller
cross = cross
circle = circle
square = square
triangle = triangle
l1 = l1
r1 = r1
l2 = l2
r2 = r2
l3 = l3
r3 = r3
options = options
touchpad_left = back
pad_up = pad_up
pad_down = pad_down
pad_left = pad_left
pad_right = pad_right
axis_left_x = axis_left_x
axis_left_y = axis_left_y
axis_right_x = axis_right_x
axis_right_y = axis_right_y

# Hotkeys
hotkey_toggle_mouse_to_joystick = f7
hotkey_reload_inputs = f8
```

O texto do arquivo gerado fica em inglês, como os comentários do código; a documentação para o
usuário em `docs/` explica cada linha.

### 4.4 Conversão mouse → stick (fórmula do shadPS4, `EmulateJoystick`)

A cada amostra do pad, com o modo mouse ligado e a captura ativa:

```text
(dx, dy) = deslocamento consumido desde a amostra anterior (pixels SDL)
dt       = tempo desde a amostra anterior, limitado a [1 ms, 100 ms]
(dx, dy) = (dx, dy) * (33 ms / dt)        # normaliza para a janela de 33 ms do shadPS4
se dx == 0 e dy == 0: contribuição = (0, 0)
senão:
    velocidade = clamp(sqrt(dx² + dy²) * speed + speed_offset * 128,
                       deadzone_offset * 128, 128)
    ângulo     = atan2(dy, dx)
    contribuição = (cos(ângulo) * velocidade, sin(ângulo) * velocidade)   # unidades de eixo
```

- **CNV-001**: A contribuição entra na soma de MIX-003 no stick de `mouse_to_joystick`.
- **CNV-002**: Sem movimento numa amostra, a contribuição é zero e o stick volta ao centro na mesma
  amostra, como no shadPS4.
- **CNV-003**: A única diferença em relação ao shadPS4 é a normalização por `dt`. O shadPS4 consulta o
  mouse num timer fixo de 33 ms; o bbport amostra a cada leitura do pad e normaliza.

### 4.5 API entre a biblioteca de GPU e o runtime (`gpu/bbgpu.h`)

```c
/* Mouse input accumulated by the window thread since the last call; the call resets it.
 * All zeros while capture is inactive. buttons uses SDL_BUTTON_MASK bits; wheel holds the
 * mousewheel* inputs currently active (33 ms pulses), one bit per direction. */
typedef struct {
    float dx, dy;          /* relative motion, SDL pixels */
    uint32_t buttons;      /* currently held mouse buttons */
    uint32_t wheel;        /* bit 0 up, 1 down, 2 left, 3 right */
    int32_t captured;      /* 1 while capture is active */
} BbMouseInput;
void bbgpu_mouse_take(BbMouseInput *out);
/* Applies the loaded input config to the window thread: whether mouse mode exists, and the
 * scancodes of the toggle and reload hotkeys (SDL_SCANCODE_UNKNOWN = none). */
void bbgpu_input_configure(int mouse_mode_available, int toggle_scancode, int reload_scancode);
/* 1 once after the reload hotkey was pressed; the pad thread then re-reads input.ini. */
int bbgpu_input_reload_requested(void);
```

- **API-001**: O runtime chama `bbgpu_input_configure` depois de cada carregamento (abertura e F8).
- **API-002**: `bbgpu_mouse_take` e `bbgpu_input_reload_requested` são seguras em qualquer thread e
  não chamam o SDL. `BbMouseInput` tem 5 campos que precisam ser lidos e zerados como um grupo
  consistente (por exemplo, `buttons` sem o `captured` correspondente pode virar um clique fantasma
  depois que a captura terminou). Por isso o estado interno usa um mutex curto dedicado, não um
  `std::atomic` por campo — diferente de `width`/`height`/`is_open` em
  [sdl_window.h:39-40](../../gpu/shim/sdl_window.h#L39-L40), que são independentes entre si.
  `bbgpu_mouse_take` copia a struct inteira sob esse mutex e zera o acumulado antes de soltá-lo.
- **API-003**: `tests/test_pad.c` faz `#include` de `runtime_pad.c`, então ganha stubs dessas três
  funções, controlados pelo teste.

### 4.6 Módulo de configuração (`src/runtime_input_config.c`)

```c
typedef struct InputConfig InputConfig;   /* bindings, special lines, hotkeys */
InputConfig *input_config_load(const char *path);          /* creates the default file if missing */
InputConfig *input_config_parse(const char *text);          /* tests: parse from memory */
void input_config_free(InputConfig *config);
const char *input_config_default_text(void);                /* section 4.3 */
```

A estrutura interna fica a critério da implementação, mantendo a separação: parse sem vídeo do SDL
nem GPU, testável a partir de texto em memória.

- **GUD-004**: Toda linha é truncada em 255 bytes antes do parse (`fgets` com buffer de 256, como
  `BbSettings::Load()` em [bbport_settings.cpp](../../gpu/shim/bbport_settings.cpp)). Um nome de
  saída ou de entrada acima de 47 bytes (margem acima do maior nome real da tabela,
  `hotkey_toggle_mouse_to_joystick`, com 31) é rejeitado em vez de comparado truncado contra as
  tabelas da seção 4; nunca um `scanf`/`sscanf` com `%s` sem largura, pelo mesmo motivo de
  [src/runtime_pad.c:191](../../src/runtime_pad.c#L191) (`fscanf(f,"%63s",token)`, já limitado).
  Uma linha ou um nome que excede o limite é tratado como inválido (CFG-004: aviso, resto do arquivo
  continua).

## 5. Critérios de aceite

- **AC-001**: Dado `cross = j` no `input.ini`, quando o jogo lê o pad com J pressionado, então
  `BTN_CROSS` está ativo. Sem outra linha de `cross` para teclado, Space não aciona `cross`.
- **AC-002**: Dada a linha `cross = banana`, então ela é ignorada, há um aviso no carregamento e as
  demais linhas valem.
- **AC-003**: Dado `r1 = insert`, então a linha é ignorada como reservada e Insert continua abrindo
  o menu.
- **AC-004**: Dado o arquivo padrão e um controle conectado, quando W é pressionado, então `left_y`
  = 0. O teclado funciona junto do controle.
- **AC-005**: Dado o stick esquerdo do controle em +72 no X e A pressionado (-127), então `left_x` =
  128 + (72 − 127) = 73.
- **AC-006**: Dado `cross = circle`, quando o botão leste do controle é pressionado, então
  `BTN_CROSS` está ativo.
- **AC-007**: Dado `r1 = r2`, quando o gatilho direito passa de 30, então `BTN_R1` está ativo.
- **AC-008**: Dado o arquivo padrão do shadPS4 (`GetDefaultInputConfig`, commit `0fe263a`) copiado
  como `input.ini`, então o jogo carrega sem erro, os bindings de teclado e controle funcionam e só
  `override_controller_color` gera aviso.
- **AC-009**: Dado `input.ini` inexistente, quando o jogo abre o pad, então o arquivo é criado com o
  texto da seção 4.3 e o comportamento é o de CON-005.
- **AC-010**: Dado `mouse_to_joystick = right`, com a janela em foco e o menu fechado, então o cursor
  está capturado. Ao abrir o menu (Insert, L3+R3) o cursor volta; ao fechar pelo botão "Fechar" ele
  some de novo.
- **AC-011**: Dado `mouse_to_joystick = right`, quando a janela perde o foco, então a captura
  termina e nenhum botão do mouse fica preso. Ao recuperar o foco, a captura volta.
- **AC-012**: Dado `mouse_to_joystick = right`, quando F7 é pressionado, então o modo mouse desliga e
  o cursor é solto; um novo F7 liga e captura de novo.
- **AC-013**: Sem `mouse_to_joystick`, então o cursor nunca é capturado, F7 não faz nada e cliques
  não geram entrada.
- **AC-014**: Com o mouse capturado e o cursor solto por F7, um clique esquerdo não aciona `r1`.
- **AC-015**: Dado o mesmo movimento em velocidade constante, a contribuição do mouse é a mesma
  (±2 unidades) com amostras a cada 33 ms e a cada 7 ms.
- **AC-016**: Dado `mouse_movement_params = 0.5, 1, 0.125` e um deslocamento de 10 px em X em 33 ms,
  então a contribuição em X é clamp(10 + 16, 64, 128) = 64 (mínimo do `deadzone_offset`), e
  `right_x` = 192.
- **AC-017**: Dado `leftjoystick_halfmode = lalt` com W e Alt esquerdo pressionados, então `left_y`
  = 128 − 63 = 65 (−127 × 0.5, truncado).
- **AC-018**: Dado o jogo rodando, quando `cross = j` é adicionado ao `input.ini` e F8 é
  pressionado, então J passa a acionar `cross` sem reiniciar.
- **AC-019**: Dado `hotkey_toggle_mouse_to_joystick = f6`, então F6 alterna o mouse e F7 fica livre
  para bindings.
- **AC-020**: Dado `mousewheelup` ligado a `pad_up`, cada passo da roda gera `BTN_UP` por 33 ms.
- **AC-021**: `BB_PAD_FILE`, `BB_PAD_RECORD` e `BB_PAD_REPLAY` passam nos testes existentes de
  `test_pad.c` sem mudança de formato.
- **AC-022**: `bash build.sh --test` e `python3 -m unittest discover -s tests` passam no Linux, e o
  build MSYS2 CLANG64 compila sem avisos novos.

## 6. Estratégia de testes

- **Níveis:** unitário (parse, fórmula do mouse, zona morta, halfmode), integração (`sample()` com
  controle virtual do SDL e o stub do mouse) e manual (no jogo).
- **Ferramentas:** testes em C com `assert`, compilados por `build.sh --test`, no padrão de
  `tests/test_pad.c` (SDL com vídeo `dummy` e `SDL_AttachVirtualJoystick`).
- **Dados:** texto de configuração em memória (`input_config_parse`) e o arquivo padrão do shadPS4
  embutido como fixture (`tests/data/shadps4_default_input.ini`) para AC-008.
- **Unitários:** cada regra de CFG-003 a CFG-006; nomes de NAM-001 a NAM-005; reservas
  (HOT-001, HOT-002); AC-016 e AC-017 com valores exatos; normalização por `dt` (AC-015).
- **Integração:** combinação (AC-004, AC-005), remap do controle virtual (AC-006, AC-007), botões e
  roda pelo stub (AC-014, AC-020), entrada neutra com o menu aberto, releitura (AC-018 com o stub
  de `bbgpu_input_reload_requested`).
- **Manual:** AC-010 a AC-013 e AC-019 no Windows e no Linux (X11 e Wayland).
- **CI:** não há pipeline; os testes rodam localmente antes de cada merge.

## 7. Justificativa e contexto

- **Formato do shadPS4 num arquivo próprio:** quem já joga Bloodborne no shadPS4 reaproveita a
  configuração. O formato é documentado pela comunidade, e o arquivo separado não é regravado pelo
  menu do jogo (`BbSettings::Save()` reescreve o `bbport.ini` só com as chaves que conhece).
- **Tecla única por linha:** cobre os arquivos comuns do shadPS4 sem trazer a lógica de combos e de
  consumo de teclas, que é a parte mais complexa do `input_handler.cpp`.
- **Soma de eixos:** paridade com o shadPS4; o mouse entra na mesma soma, em vez de sobrescrever o
  eixo como lá.
- **Mouse ligado pela linha do arquivo:** mantém o mouse desligado por padrão e evita ter de apertar
  F7 a cada sessão.
- **Cliques só com captura:** clicar para dar foco à janela não pode disparar um ataque.
- **Normalização para 33 ms:** o jogo lê o pad uma vez por frame (30 a 150 FPS). A normalização
  torna a sensibilidade independente do FPS e mantém os valores de `mouse_movement_params` do
  shadPS4.
- **Captura decidida pela thread da janela a cada frame:** o menu também fecha pela thread de
  render, e as funções de janela do SDL precisam rodar na thread da janela.
- **Nomes de tecla do shadPS4 como teclas lógicas:** é a semântica do shadPS4 (`SDLK_*`), e o arquivo
  importado se comporta igual nos dois. A conversão para scancode no carregamento mantém a leitura
  por `SDL_GetKeyboardState`.
- **Q como lock-on, V como Triangle, mouse sem botão do meio, sem tecla de halfmode:** decisões do
  responsável.

## 8. Dependências e integrações externas

### Dependências de plataforma

- **PLT-001**: SDL3: estado do teclado, controles, eventos de mouse, modo relativo e
  `SDL_GetScancodeFromKey`. Já é dependência do projeto.
- **PLT-002**: Linux (X11 e Wayland) e Windows. No Wayland o modo relativo depende do compositor
  suportar pointer constraints; isso é verificado no teste manual.

### Dependências internas

- **INT-001**: `BbSettings::Path()` (`gpu/shim/bbport_settings.cpp`): define a pasta do `input.ini`
  (CFG-001). O runtime em C precisa da mesma regra; a forma recomendada é replicá-la em C (é um
  `getenv` e um `dirname`).
- **INT-002**: `BbOverlay::CapturesInput()`: menu e caixa de texto para MOU-003.
- **INT-003**: Launchers: o Windows (`launcher/bbport_launcher_win.py`) tem um botão que abre o
  `bbport.ini`; um botão equivalente para o `input.ini` fica para a fase 2.

### Referência externa

- **EXT-001**: shadPS4 `src/input/input_handler.h` (tabelas de nomes), `input_handler.cpp` (parse,
  `ApplyDeadzone`, padrão), `input_mouse.cpp` (`EmulateJoystick`), `sdl_window.cpp` (pulso da roda
  de 33 ms).

## 9. Exemplos e casos de borda

```ini
# Arquivo do shadPS4 com linhas fora do escopo
cross = n
hotkey_screenshot_with_overlays = lalt, f12   # hotkey fora de F7/F8: ignorado com aviso
key_toggle = f1, w                            # key_toggle: ignorado com aviso
l2 = lshift, mousewheelup                     # combo: ignorado com aviso
r1:1 = r1:2                                   # sufixos de controle: aceitos e descartados
mouse_to_joystick = right
mouse_movement_params = 0.4, 1.5, 0.1
```

Casos de borda:

- `cross =` (entrada vazia): linha ignorada com aviso.
- `Cross = SPACE`: válida (CFG-004).
- Duas linhas iguais (`cross = space` duas vezes): sem efeito extra e sem aviso.
- `mouse_to_joystick = both`: inválido; a linha é ignorada e o modo mouse não existe.
- `hotkey_reload_inputs = f9`: F9 já é reservada (HOT-001); a linha é ignorada e F8 continua valendo.
- `mouse_movement_params = 0.5, 1`: faltam parâmetros; a linha fica no padrão com aviso.
- Mouse e stick direito do controle ao mesmo tempo: as contribuições somam (MIX-003).
- F8 com o arquivo quebrado (por exemplo, apagado no meio da edição): vale o CFG-002 (o arquivo é
  recriado com os padrões) e o log avisa. O jogo nunca fica sem configuração.
- F8 com uma tecla segurada: a configuração nova vale na próxima amostra; a tecla segurada passa a
  acionar o que o arquivo novo diz.

## 10. Critérios de validação

- Todos os critérios da seção 5 verificados: os automatizáveis em `build.sh --test`, os manuais com
  registro no PR.
- Nenhuma regressão em `tests/test_pad.c` nem em `python3 -m unittest discover -s tests`.
- README (seção de teclas) e `docs/INPUT.md` com o formato, o layout padrão, como importar do
  shadPS4, o que é ignorado (CFG-005, CFG-006), as teclas reservadas e a limitação CON-004.
- Build Linux e Windows compilando.

## 11. Especificações relacionadas e leitura adicional

- [keyboard-and-mouse-addition.md](keyboard-and-mouse-addition.md): pedido original.
- [handout-from-another-llm.md](handout-from-another-llm.md): handoff anterior; esta spec o substitui
  onde houver conflito.
- [tasks.md](tasks.md): quebra em tarefas.
- shadPS4: https://github.com/shadps4-emu/shadPS4 (pasta `src/input/`).
- SDL3: `SDL_GetScancodeFromKey`, `SDL_SetWindowRelativeMouseMode` (wiki.libsdl.org).
