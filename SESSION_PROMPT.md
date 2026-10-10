# Prompt para nova sessão — AnyPS5 / Dreaming Sarah

Copia tudo abaixo para a sessão nova.

---

## Contexto crítico: os directórios correctos

**Sessão anterior perdeu tempo a trabalhar na árvore errada. Não repitas isso.**

| | Caminho | O que é |
|---|---|---|
| **Árvore de trabalho** | `E:\AnyPS5-upstream` | código-fonte real. É aqui que se edita e compila |
| **Deploy / run** | `E:\anyps5-run` | `eboot.exe`, `libs\*.prx`, DLLs MinGW, `app0` |
| Dados do jogo | `E:\PPSA02929-app0` | dados do jogo |
| ~~Árvore lateral~~ | ~~`E:\s\AnyPS5`~~ | **IGNORAR.** É uma cópia de trabalho antiga. Nada feito lá afecta o jogo |

Confirma com `Test-Path E:\AnyPS5-upstream` antes de começar. Se `E:\anyps5-run`
não tiver `eboot.exe` + pasta `libs`, estás no sítio errado.

## Build

```
cd E:\AnyPS5-upstream
cmake --build build-mingw --target libs --parallel 8
```

Toolchain: `C:\mingw64\mingw64\bin` (GCC 15.2.0, `x86_64-w64-mingw32`).
Compilar sempre a partir da raiz. Antes de copiar um `.prx`, matar processos
pendurados — o ficheiro fica trancado:

```
Get-Process eboot -ErrorAction SilentlyContinue | Stop-Process -Force
```

Deploy: copiar o `.prx` de `build-mingw\core\libs\libs\` para `E:\anyps5-run\libs\`.
O `eboot.exe` só muda se mexeres no relinker.

## Estado actual (validado)

O jogo **arranca e corre a ~60 FPS, e o menu inicial aparece correctamente**.
Já não há crashes.

Último bug resolvido: **enquadramento**. A cena estava espremida numa caixa
central com barras pretas laterais. Causa raiz: nas UVs dos vértices, `u_1` e
`v_1` vinham correctos mas **`u_2` e `v_2` vinham zerados**, e o sampler lia a
coluna X=0. Corrigido com `SanitizeVertexUVs` em
`core\libs\prx\libSceAgcDriver\Graphics\src\Draw.cpp` (linha ~753), que detecta
o padrão e restaura `u_2 = x_2/1280, v_2 = y_2/720`, escrevendo tanto no buffer
Vulkan como na memória do guest.

**ATENÇÃO:** essa correcção é uma inferência geométrica em runtime. Se alguma vez
aparecerem regressões visuais, ela é a primeira suspeita.

## Tarefa: teclas confortáveis

O utilizador acha o mapeamento actual confuso. Ficheiro a editar:

`E:\AnyPS5-upstream\core\libs\prx\libScePad\include\InputMapping.hpp`

É uma tabela `inline constexpr std::array InputMapping` de `InputBinding`,
**compilada em código** — não há ficheiro de configuração, e a função
`LoadInputMapping()` está declarada mas nunca definida (API morta, ignora-a).

### Problemas concretos do mapeamento actual

- `Cross` está ligado a **duas** teclas: `Return` **e** `Space`
- `R1` está ligado a **três**: `E`, `LAlt`, `RAlt`
- `L3` a duas (`LShift`, `RShift`); `R3` a duas (`LCtrl`, `RCtrl`)
- `Square` e `R2` **só existem no rato** (botão esquerdo / direito) — não há
  tecla de teclado para nenhuma das duas
- `d-pad` e `stick esquerdo` dividem clusters de forma pouco óbvia
- `TouchLeft`/`TouchRight` em `Backspace`/`Tab`, que são teclas usadas por outros
  papéis

### Esquema proposto

Cada acção com um sítio único e óbvio, sem sobreposições:

| Papel | Tecla |
|---|---|
| Cross (confirmar) | `Return` |
| Circle (cancelar) | `Backspace` |
| Square | `Q` |
| Triangle | `E` |
| d-pad | setas (`↑ ↓ ← →`) |
| Stick esquerdo | `WASD` |
| Stick direito | `IJKL` |
| L1 / R1 | `1` / `3` |
| L2 / R2 | `Z` / `X` |
| Options | `Tab` |
| Touchpad | `M` |
| L3 / R3 | `LShift` / `RShift` |
| Fullscreen | `F11` (manter) |
| Alternar rato | botão do meio do rato (manter) |
| Touch Left/Right | `4` / `5` |

Preserva também as duas entradas finais que dão ao d-pad um contributo analógico
nos sticks (`PadButton::Up, 1` e `PadButton::Down, -1`), para a navegação de menu
ser contínua.

Depois de editar: rebuild de `libScePad`, deploy para `E:\anyps5-run\libs`, e
testar a sério no jogo — confirmar que `Enter` avança, `Backspace` volta, os
botões do menu respondem, e que nenhuma tecla dispara duas acções.

## O que foi tentado e não deve ser repetido

Uma sessão anterior (minha) tentou construir isto e **falhou por trabalhar na
árvore errada** (`E:\s\AnyPS5`). Não recuperes nada de lá. Em particular não
existes e não são usados:

- `core\libs\common\Settings.{hpp,cpp}` e o target `emulator_settings`
  — um store de settings em formato `anyps5.ini`, com bindings de teclado
- uma reescrita de `InputMapping.hpp` na árvore errada
- um `libScePad\CMakeLists.txt` portado do upstream na árvore errada

O único takeaway real disso: **o mapeamento de teclas é útil, mas `E:\AnyPS5-upstream`
já compila `libScePad` e tem input funcional — não falta `CMakeLists.txt` lá.**

## Seguinte a seguir (por ordem)

1. **Bindings de teclado** (esta tarefa) — o que o utilizador pediu.
2. **Bindings configuráveis em runtime** — `LoadInputMapping()` está declarada e
   morta; ligá-la a um ficheiro `anyps5.ini` permitiria remapear sem recompilar.
   Começar por definir `LoadInputMapping()` e fazer o consumidor usar o vector
   devolvido em vez da tabela `constexpr`.
3. **Janela de config** (SDL_Renderer, sem ImGui) para os bindings + definições
   gráficas.
4. **Definições gráficas** ligadas ao caminho existente: `DisplayWindow::create`
   (fullscreen/borderless), `VulkanDevice` (vsync), loop de flip (FPS cap),
   `PresentationScaler` (render scale), threads e prioridade de processo.

Nota: **AVX2 não é um toggle de runtime.** Depende do CPU e dos stubs em tempo de
link (hardware com AVX2, ou SDE, ou os stubs). Não o pôr na UI como se fosse opção.

## Notas de hardware

- CPU: **i5-3570K (Ivy Bridge)** — tem AVX1, **não** tem AVX2/BMI2/FMA.
  Daí os stubs de conversão no relinker.
- GPU: GTX 1660 SUPER, Vulkan 1.4.341. CUDA é irrelevante aqui.
- O `eboot.exe` do deploy tem ~12,6 MB porque inclui os stubs AVX2/BMI.