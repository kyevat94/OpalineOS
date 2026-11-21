# OpalineOS — Documento do Projeto

> Antigo nome de trabalho: ErickOS 3.

> Fonte única das decisões essenciais do projeto. Atualize este arquivo sempre que uma decisão
> for tomada ou mudar. Criado em 2026-11-17 · última revisão: 2026-11-17 (GPLv3, sistema em inglês, idiomas e Unicode).

---

## 1. Visão

Um sistema operacional **próprio, open source, leve, rápido e moderno**, em que **Python é o centro
do sistema**: a área de trabalho e os apps são escritos em **Python de verdade (CPython oficial)**,
usando **`opaline.relief`**, a biblioteca própria do sistema para criar janelas e interfaces. Os apps podem
usar bibliotecas reais baixadas com `pip`.

- **Kernel próprio em C**, escrito do zero, sem código de Linux nem de Windows.
- **Motor em C, lógica em Python**: o kernel, o compositor e o desenho dos componentes são nativos
  (a parte que define se o sistema "parece rápido"). A lógica dos apps é Python.
- Visual moderno em **relevo suave (neumorfismo)**: superfícies opacas que saltam ou afundam com luz
  e sombra, cantos arredondados, bege porcelana com destaque verde-petróleo. Mais leve de desenhar
  que vidro fosco (seção 5). Veja a seção 6 e `docs/references/`.
- Meta realista: chegar ao nível de projetos como SerenityOS, ToaruOS, Haiku e Redox (que também
  rodam o Python oficial), ou seja, um sistema completo e utilizável. **Não** é meta replicar o
  tamanho do Windows ou do Linux.
- **Sistema em inglês**: interface, mensagens, APIs, nomes das bibliotecas, código, comentários e
  documentação técnica. O **português (Brasil)** é o segundo idioma da interface, e o usuário troca
  de idioma a qualquer momento nas Configurações (seção 7).
- **Unicode completo**: textos, nomes de arquivos e entrada de teclado aceitam acentos e caracteres
  de qualquer idioma.
- **Open source sob GPLv3**.

### Princípios

1. **Leve**: pouca coisa em segundo plano. O boot até a área de trabalho deve levar poucos segundos.
2. **Rápido**: o caminho crítico (kernel, compositor, renderização) é nativo e otimizado. Os apps
   que precisam de velocidade são compilados com mypyc ou Cython.
3. **Seguro**: processos isolados em memória virtual e rodando em ring 3. Um app com erro nunca
   derruba o sistema.
4. **Compatível onde vale a pena**: POSIX via musl, para portar software real (CPython, zlib,
   OpenSSL...) em vez de reinventá-lo.
5. **Python em primeiro lugar**: toda API do sistema é pensada primeiro para ser usada em Python.
6. **Simples de entender**: código legível e comentado. Cada etapa precisa bootar e ser testável.

---

## 2. Relação com o ErickOS 2 (`../ErickOS/v2`)

O ErickOS 2 é um runtime da linguagem PyOS rodando direto no hardware: 32 bits, BIOS, sem
paginação, tudo em ring 0 e a VM de bytecode dentro do kernel. O v3 é um **kernel novo**, porque
paginação, ring 3, 64 bits, SMP e POSIX mudam a base inteira. **A linguagem PyOS própria é
aposentada**: no v3, o Python é o CPython oficial.

**O que reaproveitar do v2** (adaptando):

| Do v2 | Para o v3 |
|---|---|
| `kernel/net.c` (TCP/IP), `e1000.c` | pilha de rede do kernel (com interrupções em vez de polling), exposta como sockets BSD |
| `kernel/fs.c` (EFS2) | ponto de partida do sistema de arquivos (EFS3) |
| `kernel/wm.c`, `gfx.c` | ideias do compositor. O renderizador será reescrito (alfa, sombras, AA) |
| `root/*.pyos` (apps) | lógica dos apps reescrita em Python + `relief` (a sintaxe do PyOS já era parecida com Python) |
| `tools/vmctl.py` | controle e testes automáticos da VM pela serial |
| `kernel/pyos/*` (compilador e VM) | **não é reaproveitado** (o CPython oficial substitui) |
| `tools/proxy.py` (HTTPS) | **não é reaproveitado** (o OpenSSL portado faz o TLS) |

---

## 3. Arquitetura

### 3.1 Decisões tomadas

| Item | Decisão | Motivo |
|---|---|---|
| Arquitetura | **x86-64** | hardware atual |
| Boot | **UEFI** (bootloader próprio, GOP para vídeo) | padrão atual. BIOS fica fora |
| Memória | paginação de 4 níveis, **um espaço de endereços por processo**, kernel na metade alta | isolamento |
| Privilégio | kernel em ring 0, **apps em ring 3**, `syscall`/`sysret` | segurança |
| Executáveis | **ELF64**, estáticos e dinâmicos (`.so` + carregador dinâmico) | o CPython carrega extensões C como `.so` |
| Interface com programas | **POSIX** (≈100–150 syscalls no estilo Unix) | necessário para portar o CPython e outros softwares |
| libc | **musl** (MIT), portada para o OpalineOS | pequena, limpa e com licença permissiva |
| Python | **CPython oficial** (versão estável mais recente na época do porte: 3.14/3.15) | ecossistema real: pip, PyPI |
| Toolkit gráfico | **`opaline.relief`**: núcleo em C + API em Python | apps em Python com o visual do sistema |
| Apps rápidos | **mypyc / Cython** (Python tipado → extensão C) | ganho de ~3–10× sem criar um compilador próprio |
| TLS | **OpenSSL** portado (Apache-2.0) | HTTPS real, e o `pip` funciona |
| Linguagem do kernel | C (clang) + Assembly (NASM) | mesma toolchain do v2 |
| Ambiente de teste | **QEMU** (rápido, com depuração GDB) e **VirtualBox** | o v2 já usa VirtualBox |
| Onde roda | **VM no início**. Rodar em **PC real** e em **dual boot com Windows** é meta obrigatória (ver 3.3) | uso diário de verdade |
| Fonte da interface | **Inter** (licença OFL) | moderna, legível e pode ser distribuída |
| Nome do sistema | **OpalineOS** | escolhido pelo usuário após a pesquisa do Apêndice A.2 |
| Nomes dos módulos | pacote de distribuição **`opaline-relief`**; em Python: **`from opaline import relief`** (toolkit) e **`opaline.system`** (APIs do sistema). Documentação: [`opaline-relief.md`](opaline-relief.md) | nomes com hífen não podem ser importados em Python, e `relief` sozinho já existe no PyPI. Ficar dentro do pacote `opaline` evita qualquer conflito |
| Tipo de kernel | **monolítico modular** | drivers dentro do kernel, mas cada um isolado atrás de uma interface bem definida (podem ir para o espaço de usuário no futuro). Compositor e shell já rodam em espaço de usuário. É o modelo mais prático para os muitos drivers de PC real |
| ABI das syscalls | **mista** | as syscalls POSIX seguem o Linux x86-64: mesma numeração, mesmos registradores (`rax` = número; `rdi`, `rsi`, `rdx`, `r10`, `r8`, `r9` = argumentos), mesmas estruturas, constantes e `errno`. As syscalls exclusivas do sistema (compositor, IPC, notificações) ficam numa faixa própria, **a partir de 1000**. Syscall não implementada → `-ENOSYS`. Permite usar a musl quase sem mudanças e testar pacotes musllinux do PyPI. Nenhum código do Linux é usado, só o mesmo contrato |
| Licença do sistema | **GPL-3.0-or-later** (kernel, bootloader, compositor, shell, apps do sistema, ferramentas) | quem distribuir versões modificadas precisa publicar o código; "or later" permite usar versões futuras da GPL. Texto em `COPYING` |
| Licença das bibliotecas | **LGPL-3.0-or-later** para o que os apps importam: pacote `opaline` (`opaline.relief`, `opaline.system`, `opaline.i18n`) e o núcleo C `_relief` | apps para o OpalineOS podem ter qualquer licença. Texto em `COPYING.LESSER`; resumo em `LICENSING.md`. Componentes de terceiros mantêm as próprias licenças (seção 10) |
| Idioma do código | **inglês**: APIs, nomes de módulos/classes/funções, identificadores, comentários, mensagens de log, commits e documentação das bibliotecas | padrão de projetos open source; facilita contribuições |
| Idioma da interface | **inglês (en-US)** como padrão + **português (pt-BR)**. Troca a qualquer momento nas Configurações, **sem reiniciar** os apps | seção 7 |
| Texto | **Unicode / UTF-8 em todo o sistema** (kernel, nomes de arquivos, terminal, Python, interface) | aceitar acentos e qualquer idioma |

### 3.2 Decisões em aberto (decidir antes da etapa indicada)

| Pergunta | Opções | Recomendação | Até a etapa |
|---|---|---|---|
| (nenhuma no momento) | | | |

### 3.3 Hardware real e dual boot com Windows

Requisitos, para que tudo seja projetado desde o início sem atrapalhar o Windows:

- **UEFI + disco GPT**: o OpalineOS usa **uma partição própria** e instala o bootloader na partição de
  sistema EFI (ESP) que já existe, em `\EFI\OpalineOS\`, **sem tocar** em `\EFI\Microsoft\`.
- **Menu de boot**: o bootloader do OpalineOS lista o OpalineOS e o **Windows Boot Manager** (que ele carrega em sequência, via *chainload*). O menu de boot do próprio firmware
  continua funcionando como alternativa.
- **Instalador seguro**: só escreve na partição escolhida e na pasta do OpalineOS dentro da ESP. O
  espaço livre é criado antes pelo usuário, no Windows (Gerenciamento de Disco → Diminuir volume).
  O instalador mostra o que vai alterar e pede confirmação.
- **Secure Boot**: PCs com Windows vêm com ele ligado, e o bootloader do OpalineOS não é assinado pela
  Microsoft. Fase 1: o usuário desliga o Secure Boot. Fase 2: o OpalineOS fornece a própria chave para
  o usuário registrar no firmware (MOK/chaves próprias).
- **Convivência**:
  - relógio: o Windows guarda a hora local no relógio do PC, então o OpalineOS lê nesse formato (ou
    torna isso configurável);
  - se o Windows estiver hibernado ou com Inicialização Rápida, a partição NTFS não deve ser
    alterada;
  - com BitLocker ativo, a partição do Windows fica inacessível (leitura de NTFS só mais tarde,
    somente leitura).
- **Drivers mínimos para PC real**: AHCI e NVMe (disco), xHCI + USB HID (teclado e mouse), GOP
  (vídeo), ACPI (desligar, reiniciar, bateria), placa de rede comum (Intel/Realtek).
- Antes do PC real, o dual boot é testado numa **VM com dois sistemas** (disco GPT com a ESP e uma
  partição "Windows" simulada).

### 3.4 Camadas

```
┌─────────────────────────────────────────────────────────────┐
│ Apps em Python: Arquivos, Terminal, Editor, Navegador,       │
│ Monitor, Música, Configurações, Calendário... (+ pip/PyPI)   │
├─────────────────────────────────────────────────────────────┤
│ Shell gráfico (Python): barra superior com tarefas, lançador,│
│ central de notificações                                      │
├─────────────────────────────────────────────────────────────┤
│ opaline.relief · opaline.system · opaline.i18n (Python)      │
│ _relief (núcleo C): componentes, layout, texto, animação     │
├─────────────────────────────────────────────────────────────┤
│ CPython oficial  ·  zlib, OpenSSL, libffi, sqlite...         │
├─────────────────────────────────────────────────────────────┤
│ musl libc  ·  carregador dinâmico (ld.so)                    │
├─────────────────────────────────────────────────────────────┤
│ Compositor (C, espaço de usuário): buffers compartilhados,   │
│ alfa, relevo (sombras em cache), animações                   │
├──────────────────── syscalls POSIX ─────────────────────────┤
│ Kernel (C): memória virtual, processos/threads, escalonador  │
│ SMP, VFS + EFS3, pipes, sockets, IPC, drivers (disco, rede,  │
│ entrada, vídeo, som)                                         │
├─────────────────────────────────────────────────────────────┤
│ Bootloader UEFI                                              │
└─────────────────────────────────────────────────────────────┘
```

---

## 4. Python como centro: CPython oficial + `opaline.relief`

### 4.1 Por que o Python oficial e não uma linguagem própria

- **Ecossistema real**: bibliotecas do PyPI, documentação, tutoriais e ferramentas existentes.
- **Menos trabalho reinventando**: o esforço vai para o que torna o OpalineOS único (kernel,
  compositor, visual e a API `opaline.relief`), e não para recriar uma linguagem e seu compilador.
- **Velocidade onde importa**: um app de interface passa quase todo o tempo esperando eventos, e o
  desenho é feito em C. Para cálculo pesado existem mypyc/Cython e o JIT experimental do CPython.
- **Custo aceitável**: cada app em CPython ocupa cerca de 15–25 MB e leva dezenas de
  milissegundos para abrir. Continua bem mais leve que o Windows.

### 4.2 Como o CPython roda no OpalineOS

1. O kernel implementa as syscalls POSIX necessárias: arquivos, diretórios, `mmap`, processos
   (`fork`/`execve` ou `posix_spawn`), threads + `futex`, pipes, sockets, `poll`, relógio e sinais
   básicos.
2. A **musl** é portada, fazendo suas chamadas ao kernel do OpalineOS.
3. O **CPython** é compilado de forma cruzada (no Windows, com clang mirando o OpalineOS):
   - primeiro **estático**, com os módulos essenciais;
   - depois **dinâmico**, carregando extensões `.so`.
4. Dependências portadas como "ports": zlib, OpenSSL, libffi (para `ctypes`), sqlite, bzip2/xz, expat.
5. Com rede e SSL funcionando, o **`pip`** funciona.

### 4.3 Bibliotecas da internet (pip / PyPI)

| Tipo de pacote | Situação |
|---|---|
| **Python puro** (requests, flask, rich, beautifulsoup...) | Funciona direto com `pip install` |
| **Com partes em C** (numpy, pillow, pandas...) | Os binários do PyPI são para Linux/Windows/Mac. Compilamos para o OpalineOS num **sistema de ports** (receitas de build) e servimos de um índice próprio |
| **Binários "musllinux" do PyPI** | **Podem funcionar sem recompilar** se a musl do OpalineOS mantiver as mesmas estruturas, constantes e (idealmente) a numeração de syscalls do Linux. A ser validado na etapa E12 |
| **Dependentes de GPU ou de outro sistema gráfico** (PyTorch, PyQt, Tkinter) | Fora de escopo |

### 4.4 A biblioteca `opaline.relief`

- **`_relief` (C)**: fala com o compositor por memória compartilhada e IPC, desenha os componentes
  do design system (seção 6) com relevo (luz e sombra), texto TrueType e animações, e trata a
  entrada.
- **`opaline.relief` (Python)**: uma API simples e "pythônica" por cima, **em inglês**.
- **`opaline.system` (Python)**: APIs do sistema (notificações, energia, rede, configurações, tema,
  arquivos recentes).
- **`opaline.i18n` (Python)**: traduções, idioma atual e formatação de datas/números (seção 7).
- Documentação completa: [`opaline-relief.md`](opaline-relief.md).

Exemplo do estilo de API desejado:

```python
from opaline import relief as rl

from opaline.i18n import _

app = rl.App(_("Counter"))
window = rl.Window(_("Counter"), width=360, height=200)

total = rl.Label("0", style="h1")
def increment():
    total.text = str(int(total.text) + 1)

window.add(rl.Column([
    total,
    rl.Button(_("Add"), primary=True, on_click=increment),
]))
window.show()
app.run()
```

- Laço de eventos integrável com `asyncio` (um app pode fazer requisições de rede sem travar a
  interface).
- Apps são pastas com `app.toml` (nome, ícone, categoria, traduções) + código Python. O lançador lê esse
  manifesto.

---

## 5. Gráficos e o compositor

Sem aceleração de GPU no início (a VM não oferece), **tudo é renderizado por software, em C com
SIMD (SSE/AVX)**. O visual das referências é viável com estas técnicas:

| Efeito | Técnica | Custo / otimização |
|---|---|---|
| Relevo (neumorfismo) | duas sombras por elemento: realce claro do lado da luz e sombra escura do outro (internas, para afundado), pré-desfocadas e guardadas em cache por tamanho, raio e elevação (9 fatias: cantos e bordas) | quase grátis depois do cache; mudar a elevação (passar o mouse, pressionar) é só uma mistura entre sombras já prontas |
| Superfícies opacas | janelas e componentes têm cor sólida | o compositor pula o que uma janela opaca cobre, e mover uma janela redesenha só a área descoberta. Nada atrás precisa ser lido e desfocado |
| Cantos arredondados | máscaras de SDF (campo de distância) com antialiasing | barato |
| Brilho (glow) de foco | sombra colorida desfocada, como as do relevo | idem |
| Vidro fosco (**opcional**) | blur aproximado (3 passes de box blur) em resolução reduzida (1/4) e ampliado | caro: refeito sempre que o que está atrás muda. Fica fora do tema do sistema; os apps podem usar em elementos pequenos e passageiros |
| Texto nítido | rasterizador TrueType (próprio ou FreeType portado) com antialiasing e cache de glifos | cache por tamanho |
| Ícones | ícones vetoriais próprios rasterizados com AA e cacheados | — |
| Animações | interpolação com *easing* a 60 fps, só na região que mudou (*damage tracking*) | redesenhar só o necessário |
| Imagens | libpng/libjpeg portadas (ou decodificadores próprios) | — |

- Compositor em **espaço de usuário**. Cada janela desenha no próprio buffer (memória
  compartilhada) e o compositor só monta a cena. Assim, uma janela travada não congela a tela.
- Metas de desempenho: 60 fps arrastando janelas em 1920×1080 na VM e menos de 16 ms por quadro.
- Mais adiante: driver *virtio-gpu* (QEMU) e, só muito depois, aceleração de GPU real.
- **Por que relevo e não vidro** (decisão de 2026-11-21): sem GPU, o desfoque do que está atrás de
  cada janela transparente é o efeito mais caro do compositor, e precisa ser refeito toda vez que o
  fundo muda. No relevo as superfícies são opacas e as sombras vêm prontas do cache.

---

## 6. Design system "Relief"

Relevo suave (**neumorfismo**): superfícies opacas da cor do fundo, que saltam ou afundam por causa de
uma luz (de cima à esquerda) — um realce claro de um lado e uma sombra do outro, ou os dois por dentro
para o que está afundado. Um único destaque forte (verde-petróleo) marca as ações principais.

Referências em `docs/references/`:
- `neumorfismo.webp` (**referência do estilo atual**, de 2026-11-21): kit completo — camadas (base,
  elevada, embutida, pressionada), níveis de elevação, espessuras de borda, escala de raios, regras de
  ícones, controles, navegação, exibição de dados e feedback.
- **`docs/mockup/opaline-desktop.html`** (**prévia interativa aprovada** em 2026-11-21): a área de
  trabalho no estilo Relief, com o layout da seção 6.3, as animações de janela, o kit de componentes
  clicável e as Configurações (relevo, destaque, luz, contraste, efeitos, idioma). Abrir no navegador.
  É a referência de como o sistema deve ficar e se mover.
- `desktop.webp`: área de trabalho completa (barra superior, dock lateral e inferior, menu de apps,
  gerenciador de arquivos, monitor do sistema, player e central de notificações). Vale pela
  **conteúdo** dos apps; o estilo de vidro escuro foi substituído pelo relevo e os docks foram
  abandonados (seção 6.3).
- `componentes.webp`: kit de componentes no estilo antigo (vidro), também só como referência de
  conteúdo.

> Os ícones das referências incluem marcas de terceiros (Firefox, Spotify, Discord, Steam, VS Code,
> Chrome). O OpalineOS usa **somente ícones próprios**, nunca logotipos de outras marcas.

**Cuidados do estilo** (o ponto fraco conhecido do neumorfismo é o contraste):
- toda ação principal usa o destaque, e texto e ícones ficam com contraste de pelo menos 4,5:1;
- estados (pressionado, desabilitado, foco) nunca dependem só da sombra: mudam também cor, opacidade
  ou ganham o anel de foco;
- o modo de **alto contraste** acrescenta borda de 1 px em todo elemento elevado ou afundado.

### 6.1 Tokens (valores iniciais, estimados da referência e ajustáveis)

Os nomes dos tokens são em inglês, como toda a API.

**Cores (tema claro, o padrão)**

| Token | Valor | Uso |
|---|---|---|
| `background.base` | `#EEE6DA` (bege porcelana) | fundo da área de trabalho e das janelas |
| `surface.base` | `#EEE6DA` | superfícies em relevo (a mesma cor do fundo: só a luz as separa) |
| `surface.inset` | `#E8DFD2` | campos de texto, trilhos de slider, poços |
| `relief.light` | `#FFFBF5` com 90% | realce do lado da luz |
| `relief.dark` | `#C9BBA6` com 70% | sombra do lado oposto |
| `border.subtle` | `#000000` com 8% | bordas finas (e do alto contraste) |
| `accent` | `#1E5D59` (verde-petróleo) | botão primário, item ativo, switch ligado, passo atual |
| `accent.strong` | `#164744` | hover/pressionado do destaque |
| `accent.on` | `#FFFFFF` | texto sobre o destaque |
| `focus` | `#2E8C86` com 50% | anel de foco (glow) |
| `text.primary` | `#2D2A26` | títulos e texto principal |
| `text.secondary` | `#6F665C` | legendas, metadados |
| `text.disabled` | `#A79E92` | estados desabilitados |
| `success` / `warning` / `error` / `info` | `#2E7D5B` / `#B7832F` / `#B3423A` / `#2E6F9E` | badges, toasts, alertas, gráficos |
| `chart.cpu` / `chart.memory` / `chart.disk` | `#1E5D59` / `#C9A26B` / `#8C6E9E` | séries do monitor do sistema |

O **tema escuro** segue a mesma regra de relevo com uma superfície escura e realces discretos; os
valores ficam para a E9 (questão aberta no `opaline-relief.md`).

**Elevação** (`elevation`, deslocamento/desfoque das duas sombras em px; o realce vai para o lado da
luz e a sombra para o outro)

| Nível | Nome | Sombras | Uso |
|---|---|---|---|
| −2 | `pressed` | internas 4/8 | botão pressionado, switch ligado |
| −1 | `inset` | internas 3/6 | campos, trilhos, poços |
| 0 | `base` | nenhuma | fundo, separadores |
| 1 | `flat` | 2/4 | itens de lista, cabeçalho de tabela |
| 2 | `hover` | 4/10 | o que está sob o ponteiro |
| 3 | `raised` | 6/16 | cards, botões, barra superior |
| 4 | `modal` | 10/28 | janelas, diálogos |
| 5 | `popover` | 14/36 | menus, tooltips, dropdowns, toasts |

**Forma e espaço**

| Token | Valor |
|---|---|
| Raio (`radius.*`) | `xs 4` · `sm 8` · `md 12` · `lg 16` · `xl 24` · `window 16` · `pill 999` (px) |
| Espaçamento (grade de 4 px) | 4 · 8 · 12 · 16 · 24 · 32 · 48 |
| Borda | 1 px (fina) · 2 px (normal, foco) · 4 px (grossa) |
| Ícones | 16 · 20 · 24 px, traço de 2 px, cantos arredondados |
| Tipografia (`font.*`) | Inter. `h1` 28/600 · `h2` 20/600 · `body` 14/400 · `caption` 12/400 |
| Movimento (`motion.*`) | `fast` 120 ms (hover) · `open` 200 ms · `close` 160 ms · `window` 320 ms (minimizar/maximizar). Easing `cubic-bezier(.2,.8,.2,1)` |

### 6.2 Componentes do toolkit (prioridade)

1. Janela (barra de título com minimizar, maximizar e fechar), botão (primário, secundário,
   desabilitado, carregando), input (texto, senha, busca), rótulo, ícone
2. Toggle, checkbox, radio, abas, chips, dropdown, slider, barra de progresso, passos (stepper),
   tooltip, badge, spinner
3. Lista, grade de ícones, tabela, card, avatar, breadcrumbs, paginação, estado vazio,
   toast/notificação, banner de alerta, diálogo de confirmação, linhas de carregamento (skeleton)
4. Date picker, gráficos (linha, anel), menu de contexto

### 6.3 Shell (área de trabalho)

**Sem docks e sem barras laterais ou inferiores.** A única barra é uma **barra fina no topo**,
flutuante e em relevo, que faz o papel da barra de tarefas:
- à esquerda, o botão **Apps** (também Ctrl+Espaço), que abre o lançador: busca, grade de apps,
  usuário e os botões de reiniciar e desligar;
- no meio, **um botão para cada programa aberto**, que aparece com uma animação de mola quando o
  programa abre e some quando ele fecha. O do programa ativo fica afundado, com um traço de destaque.
  Clicar alterna entre focar, minimizar e restaurar; minimizar "entra" no botão e restaurar sai dele;
- à direita, os indicadores (rede, som, bateria), as notificações e o relógio, que abrem a central de
  notificações: atalhos rápidos (Wi-Fi, Bluetooth, modo avião, modo noturno), brilho, volume e a lista
  de avisos.

Janelas maximizadas ocupam a tela abaixo da barra. Os avisos (toasts) aparecem no canto superior
direito, logo abaixo dela. O fundo é a cor base ou um papel de parede próprio, suave e de pouco
contraste, para o relevo continuar legível. A prévia em `docs/mockup/opaline-desktop.html` mostra tudo
isso funcionando.

---

## 7. Idiomas, Unicode e entrada de texto

### 7.1 Idioma do sistema

- **Inglês (en-US)** é o idioma padrão e o idioma "fonte": todo texto da interface é escrito em
  inglês no código e traduzido por catálogos.
- **Português (Brasil) (pt-BR)** é o segundo idioma. Outros podem ser adicionados depois só com novos
  catálogos, sem mudar código.
- O usuário troca o idioma em **Settings → Language & Region**, a qualquer momento:
  - o shell e todos os apps abertos **atualizam os textos na hora, sem reiniciar**;
  - o instalador e a primeira inicialização perguntam o idioma;
  - a escolha é por usuário (cada conta pode ter o seu idioma).
- Ficam **sempre em inglês**: mensagens do kernel e do log serial, nomes de APIs, nomes das pastas
  do sistema (`/system`, `/apps`, `/home`) e mensagens de erro técnicas (rastreamentos do Python).
- Textos sem tradução aparecem em inglês (nunca vazios).

### 7.2 Como a tradução funciona

- Baseado no **`gettext`**, o padrão de tradução do mundo open source. O próprio Python já tem o
  módulo `gettext`, então ferramentas e editores de tradução (Poedit, Weblate) funcionam.
- Catálogos: `locale/pt_BR.po` (fonte, editável) → `.mo` (compilado no build).
- API para apps: `from opaline.i18n import _` (detalhes em `opaline-relief.md`, seção "Internationalization"):
  - `_("Save")` devolve um **texto traduzível "vivo"**: os componentes do `opaline.relief` guardam a
    referência e se redesenham sozinhos quando o idioma muda;
  - plurais (`_n("{n} file", "{n} files", n)`) e parâmetros (`_("Hello, {name}!", name=...)`);
  - formatação regional de datas, horas, números e tamanhos: en-US `Mon, Oct 27 · 9:42 PM` ·
    `1,234.5` / pt-BR `seg., 27 de out. · 21:42` · `1.234,5`.
- Cada app traz suas traduções em `locale/` dentro da pasta do app. O `app.toml` traz o nome do app
  traduzido (`name.pt-BR = "Calculadora"`) para o lançador.
- O sistema avisa a troca de idioma por um evento (`opaline.i18n.on_change`), para textos que o app
  monta por conta própria.

### 7.3 Unicode em todo o sistema

| Camada | Como fica |
|---|---|
| Kernel | strings em UTF-8. Nomes de arquivo são bytes UTF-8 (como no Linux) |
| Sistema de arquivos (EFS3) | nomes em UTF-8, normalizados em **NFC** ao criar (assim "ação" digitado de formas diferentes é o mesmo nome). Comparação sensível a maiúsculas |
| libc / POSIX | locale padrão `C.UTF-8` |
| Python | modo UTF-8 ligado (`PYTHONUTF8=1`): `open()`, `print()` e o terminal usam UTF-8 |
| Terminal | UTF-8, com caracteres largos (CJK) e combinantes (acentos compostos) |
| Interface | todo texto é Unicode. Renderização em duas fases (abaixo) |

**Renderização de texto:**
- **Fase 1 (E8)**: rasterizador de fontes + **Inter** (latim com acentos, grego, cirílico, vietnamita)
  + fontes de reserva **Noto** para o que a Inter não cobre. Já atende inglês, português, espanhol,
  francês, alemão, russo etc.
- **Fase 2 (E12)**: **HarfBuzz** (montagem de glifos para escritas complexas: árabe, hindi, tailandês),
  algoritmo **bidi** (texto da direita para a esquerda: árabe, hebraico), CJK (chinês, japonês,
  coreano) e **emoji** coloridos (Noto Color Emoji).

### 7.4 Teclado e entrada de texto

- Layouts: **US**, **US-International** (com teclas mortas: `´` + `e` → `é`, `~` + `a` → `ã`) e
  **ABNT2** (Brasil, com `ç` direto). Mais layouts depois, como arquivos de dados.
- Troca de layout em **Settings → Keyboard** e por atalho (padrão `Super+Space`). O layout é
  independente do idioma da interface (dá para usar a interface em inglês com teclado ABNT2).
- Tecla **Compose** opcional para caracteres especiais.
- Fase 2: métodos de entrada (IME) para chinês, japonês e coreano.

---

## 8. Roteiro (cada etapa precisa bootar e ter um teste automático)

| Etapa | Entrega | Critério de pronto |
|---|---|---|
| **E0** ✅ | Toolchain e build: clang/lld/NASM, script de build, imagem de disco GPT/FAT32, QEMU+OVMF | `build` gera a imagem e o QEMU chega ao bootloader. **Concluída em 2026-11-17** (QEMU e VirtualBox) |
| **E1** ✅ | Bootloader UEFI, kernel 64 bits, GOP, serial, console de texto no framebuffer, GDT/IDT, exceções | "Hello" na tela e no log serial (UTF-8); exceção mostra um diagnóstico. **Concluída em 2026-11-17** (QEMU e VirtualBox) |
| **E2** ✅ | Memória: alocador físico, paginação, heap do kernel | teste de estresse de alocação passa. **Concluída em 2026-11-19** (QEMU e VirtualBox) |
| **E3** ✅ | Processos, threads e escalonador preemptivo (APIC timer) | duas threads de kernel alternando. **Concluída em 2026-11-19** (QEMU e VirtualBox) |
| **E4** ✅ | Ring 3, syscalls e carregador ELF64 estático | programa em C de usuário escreve via syscall; um app com erro não derruba o kernel. **Concluída em 2026-11-19** (QEMU e VirtualBox) |
| **E5** ✅ | VFS, EFS3 (nomes de arquivo em UTF-8), disco (virtio-blk ou AHCI), pipes, terminal (TTY) | ler e gravar arquivos do espaço de usuário, inclusive com nomes acentuados. **Concluída em 2026-11-19** (QEMU e VirtualBox) |
| **E6** ✅ | **Camada POSIX + musl** portada + carregador dinâmico (`ld.so`) | programas de teste em C (arquivos, `fork`/`exec`, threads, `mmap`) passam. **Concluída em 2026-11-20** (QEMU e VirtualBox) |
| **SE1** ✅ | Sub-etapa: o que a E6 simplificou — copy-on-write, `MAP_SHARED` real, `mremap`, `execve` com threads, relógio real e tempos de arquivo, usuários e permissões, controle de tarefas (Ctrl+C/Ctrl+Z) | `posixtest` (104 verificações) passa, inclusive como usuário comum. **Concluída em 2026-11-21** (QEMU e VirtualBox) |
| **SE2** ✅ | Sub-etapa: sistema de arquivos — journal no EFS3, links simbólicos e físicos | queda de energia simulada no meio de uma escrita não corrompe o disco (fsck limpo). **Concluída em 2026-11-21** (QEMU e VirtualBox) |
| **SE3** | Sub-etapa: terminal e shell — `select`/`pselect6`, termios (modo canônico/cru, `TCGETS`/`TCSETS`), shell com `fork`/`execve` e ambiente herdado | um programa em modo cru lê teclas uma a uma; o shell passa variáveis de ambiente |
| **SE4** | Sub-etapa: revisão independente do código da E6 e das SEs, imagem nova no README | achados da revisão corrigidos ou registrados |
| **E7** | **CPython** portado (estático → dinâmico) + zlib, libffi, sqlite | REPL do Python no console; subconjunto da suíte de testes do CPython passa |
| **E8** | Compositor + renderizador (alfa, relevo com sombras em cache, TrueType com fontes de reserva) + entrada (mouse; teclado com layouts US, US-International e ABNT2, teclas mortas) | janela em relevo arrastável a 60 fps; digitar "ação, über, ñ" funciona |
| **E9** | **`opaline.relief`** (núcleo C + API Python) + `opaline.system` + **`opaline.i18n`** + shell (barra superior com as tarefas, lançador, central de notificações) | o app Counter da seção 4.4 roda; a área de trabalho segue o design; trocar en-US ↔ pt-BR atualiza a interface sem reiniciar |
| **E10** | Apps em Python: Terminal, Files, Editor, System Monitor, Settings (com a página **Language & Region**) | uso diário básico, nos dois idiomas |
| **E11** | Rede: driver (e1000/virtio-net, portado do v2), TCP/IP com sockets BSD + OpenSSL | `pip install requests` e um GET `https://` funcionando |
| **E12** | Sistema de ports (numpy, pillow...), teste de binários musllinux, apps com mypyc; HarfBuzz + bidi (escritas complexas e da direita para a esquerda), emoji | `import numpy` funciona; benchmark mypyc × CPython registrado; árabe, hebraico, hindi e CJK aparecem corretamente |
| **E13** | SMP, som, USB (xHCI), AHCI/NVMe, ACPI: rodar em PC real | boot num PC físico |
| **E14** | Instalador + **dual boot com Windows** (menu de boot, chainload, partição própria) | instala ao lado do Windows numa VM e depois num PC real; o Windows continua bootando |
| **E15** | Gerenciador de pacotes do sistema, atualizações, Secure Boot com chave própria, polimento | — |

---

## 9. Estrutura de pastas planejada

```
OpalineOS/
├── project.md            # este documento
├── opaline-relief.md     # documentação do toolkit
├── docs/references/      # imagens de referência visual
├── boot/                 # bootloader UEFI
├── kernel/               # kernel (C + ASM)
│   ├── arch/x86_64/
│   ├── mm/  sched/  fs/  net/  posix/  drivers/
├── system/
│   ├── compositor/       # compositor (C)
│   ├── relief/           # _relief (C) + relief (Python)
│   ├── opaline/          # pacote Python `opaline` (system/, i18n/; relief/ é instalado aqui)
│   └── shell/            # área de trabalho (Python)
├── apps/                 # apps em Python (pasta + app.toml + locale/)
├── locale/               # catálogos de tradução do sistema (en-US = textos do código, pt_BR.po)
├── ports/                # patches e código nosso dos ports (musl/patches, rt/); receitas em tools/ports/
├── tests/posix/          # programas de teste em C ligados à musl (E6)
├── tools/                # build, imagem de disco, vmctl, testes
└── build/                # gerado (não versionar)
```

---

## 10. Ferramentas e testes

### 10.1 Comandos (prontos desde a E0)

| Comando | O que faz |
|---|---|
| `python tools/build.py` | compila o bootloader (`build/BOOTX64.EFI`) e gera a imagem `build/opaline.img` (128 MiB, GPT + ESP FAT32 de 64 MiB); `--clean` recomeça do zero |
| `python tools/vm.py run [--cmdline "..."]` | abre o QEMU (q35, OVMF) com a imagem; `--cmdline` cria uma variante com `CMDLINE.TXT` |
| `python tools/vm.py run vbox` | abre a VM **OpalineOS** no VirtualBox (criada na primeira vez: EFI, 512 MB, SATA, COM1 em arquivo) |
| `python tools/vm.py test` | roda a lista `TESTS` de `tools/vm.py`: cada caso boota sem janela (QEMU ou VirtualBox), lê a COM1 e confere textos esperados e proibidos. Saída `TEST_OK`/`TEST_FAILED` por caso e código ≠ 0 se algum falhar |
| `python tools/vm.py screenshot OUT.png [--cmdline "..."]` | screenshot do QEMU quando o kernel termina ou entra em pânico |
| `python tools/gen_font.py FONTE.ttf` | regenera `assets/fonts/console.fnt` (precisa de Pillow e fontTools; o arquivo gerado é versionado) |
| `python tools/mkimage.py IMG --verify` | relê uma imagem e confere GPT (CRCs, cópia de reserva) e FAT32 |

- A imagem é gerada por um script próprio em Python (`tools/mkimage.py`), sem mtools nem permissão de
  administrador. GUIDs fixos, para builds reproduzíveis.
- O bootloader é um app UEFI em C (`boot/efi/`), compilado com `clang --target=x86_64-unknown-windows`
  e ligado com `lld-link /subsystem:efi_application`, sem bibliotecas externas (`efi.h` próprio).
- No QEMU, o OVMF espelha o console na serial, por isso as mensagens aparecem duas vezes em
  `build/serial-qemu.log`. Isso é normal.
- Instalados: LLVM 23.1, NASM, QEMU 11.1 (`C:\Program Files\qemu`, firmware `share/edk2-x86_64-code.fd`), VirtualBox.

### 10.1.1 Como a E1 funciona

- **Protocolo de boot** (`boot/bootinfo.h`, compartilhado): o bootloader lê `\OPALINE\KERNEL.ELF`,
  `\OPALINE\CONSOLE.FNT` e o opcional `\OPALINE\CMDLINE.TXT` da ESP, carrega os segmentos ELF,
  escolhe o modo de vídeo (preferência 1280x800, ou `video=LxA` na linha de comando, senão o maior
  até 1920x1080), monta as tabelas de páginas (identidade dos primeiros 4 GiB + RAM, a mesma memória
  em `0xFFFF800000000000` (HHDM) e o kernel em `0xFFFFFFFF80000000`), sai dos boot services e pula
  para o kernel com `RDI = struct boot_info *`.
- **Mapa de memória**: tudo é alocado como `EfiLoaderData` e o bootloader separa as próprias regiões
  (`kernel`, `boot data`) ao converter o mapa. Tipos de memória próprios (≥ 0x80000000), permitidos
  pela especificação UEFI, **derrubam o firmware do VirtualBox** no `ExitBootServices`: não usar.
- **Kernel** (`kernel/`): `entry.asm` (pilha própria de 64 KiB), console UTF-8 no framebuffer + COM1
  (`console.c`, `kprintf`), GDT com segmentos de usuário já na ordem do `SYSRET`, TSS com pilhas IST
  para #DF/NMI/#MC, IDT com as 32 exceções, `#BP` retomável e tela de pânico com registradores,
  CR2 e *stack trace* (o kernel é compilado com frame pointers).
- **Fonte do console**: `assets/fonts/console.fnt` (1271 caracteres, célula 9x18, alfa de 8 bits),
  gerada a partir da DejaVu Sans Mono. É um arquivo de dados separado, carregado pelo bootloader; não
  é ligado ao binário do kernel.
- **Testes da E1**: boot normal (QEMU e VirtualBox) e três exceções provocadas pela linha de comando
  (`test=pagefault`, `test=ud`, `test=divide`), cada uma conferindo a tela de pânico. Uma divisão por
  zero em C pode ser removida pelo compilador; por isso o teste usa `div` em assembly.
- Screenshots em `docs/screenshots/`.

### 10.1.2 Como a E2 funciona

- **Páginas físicas** (`kernel/mm/pmm.c`): bitmap de 1 bit por página de 4 KiB. Gerencia as regiões
  `usable` e também as `bootloader` (o código e os buffers do bootloader não servem mais depois que o
  kernel começa). O primeiro 1 MiB nunca é entregue (fica para o código de partida dos outros núcleos,
  na E13). Liberar uma página já livre causa pânico.
- **Tabelas de páginas do kernel** (`kernel/mm/vmm.c`): o kernel monta as próprias tabelas e
  abandona as do bootloader:
  - `.text` só leitura + execução; `.rodata` só leitura; `.data`/`.bss` leitura e escrita; dados
    nunca executáveis (bit NX ligado via `EFER.NXE`);
  - HHDM (`0xFFFF800000000000 + físico`) de toda a memória tipo RAM, com páginas de 2 MiB quando dá;
  - framebuffer em *write-combining* (entrada 1 da PAT reprogramada);
  - **sem mapa de identidade**: acessar um ponteiro NULL agora gera page fault;
  - as 256 entradas da metade alta do PML4 já existem, para que os processos (E3/E4) compartilhem a
    metade do kernel copiando essas entradas;
  - `vmm_map`, `vmm_unmap`, `vmm_translate`, `vmm_map_mmio` (sem cache, para registradores de
    dispositivos).
- **Heap** (`kernel/mm/heap.c`): `kmalloc`, `kcalloc`, `krealloc`, `kfree`, `ksize`. Até 2048 bytes
  usa *slabs* (uma página por slab, classes de 16 a 2048 bytes); acima disso, páginas contíguas.
  Blocos liberados são preenchidos com `0xDE`; `kfree` detecta liberação dupla e ponteiros que não
  vieram do `kmalloc`.
- **Ainda não é seguro para várias threads**: não há preempção nem outros núcleos até a E3, que
  adiciona travas (*locks*).
- **Autoteste de memória** (`kernel/mm/selftest.c`, roda em todo boot): 512 páginas com padrão
  verificado, uma sequência de 64 páginas contíguas, mapear/desmapear e traduzir endereços, e 40 mil
  operações aleatórias de `kmalloc`/`kfree` (de 1 byte a 64 KiB) conferindo o conteúdo de cada bloco.
  No fim, nada pode ter vazado.
- **Testes da E2** (`tools/vm.py`): além do boot, provocam e conferem a tela de pânico para
  ponteiro NULL (`test=null`), escrita em dados só de leitura (`test=wp`), execução de código no heap
  (`test=nx`) e liberação dupla (`test=doublefree`). Verificado também que o teste de boot **falha**
  se o heap for sabotado (o autoteste acusa o bloco corrompido).

### 10.1.3 Como a E3 funciona

- **Interrupções**: a IDT agora tem os 256 vetores (`isr.asm` gera um *stub* para cada um). O PIC 8259
  antigo é remapeado para 0x30-0x3F e totalmente mascarado; tudo passa pelo **LAPIC** (vetor 0x20 =
  timer, 0xFF = espúrio). Drivers registram tratadores com `irq_set_handler`.
- **Timer**: o timer do LAPIC é medido contra o PIT (canal 2, 10 ms, menor de 3 medições) e roda em
  modo periódico a **1000 Hz**. `time_ms()` conta a partir daí.
- **Processos e threads** (`kernel/sched/`): `struct process` (pid, tabela de páginas) e
  `struct thread` (estado, pilha, processo). Por enquanto só existe o processo do kernel (pid 0); a E4
  cria processos de usuário com espaço de endereços próprio.
- **Escalonador**: *round-robin* preemptivo com fatia de **10 ms**. A troca de contexto
  (`switch.asm`) salva só os registradores *callee-saved*; a troca pode acontecer dentro da
  interrupção do timer (a thread volta pelo mesmo caminho quando for escolhida de novo). A thread
  `idle` executa `hlt` quando não há nada pronto e libera threads destacadas que terminaram.
- **API**: `thread_create`, `thread_yield`, `thread_sleep_ms`, `thread_exit`, `thread_join` (devolve o
  código de saída e libera a thread), `thread_detach`.
- **Pilhas de thread**: 16 KiB numa região própria (`0xFFFFB00000000000`), cada uma abaixo de uma
  **página de guarda** sem mapeamento. Estourar a pilha vira *double fault*, que tem pilha IST própria;
  a tela de pânico diz "kernel stack overflow" e o nome da thread.
- **Travas**: `spinlock_t` com `spin_lock`/`spin_unlock`, que também desligam as interrupções.
  Protegem o alocador de páginas, o heap, as tabelas de páginas e o console (`kprintf` inteiro, para
  linhas de threads diferentes não se misturarem). Durante um pânico a trava do console é ignorada.
- **Autoteste do escalonador** (todo boot): duas threads que **nunca cedem a vez** precisam alternar
  (só o timer pode trocá-las: ~30 alternâncias em 300 ms); `thread_sleep_ms(50)` precisa levar 50-80 ms;
  32 threads somam 32.000 num contador com trava; o código de saída chega ao `thread_join`; no fim,
  nenhuma página pode ter vazado. Verificado que o teste **falha** se a preempção for desligada.
- **Testes da E3**: `test=stackoverflow` (recursão até a página de guarda → *double fault* com
  "kernel stack overflow") e `test=threadfault` (NULL dentro de uma thread → o pânico mostra o nome da
  thread).

### 10.1.4 Como a E4 funciona

- **Espaço de endereços por processo**: cada processo tem seu PML4; a metade alta aponta para as
  mesmas tabelas do kernel (pré-alocadas na E2), a metade baixa é só dele. O escalonador troca o CR3
  ao trocar de processo. Ao terminar, todas as páginas e tabelas do processo voltam ao alocador.
- **Carregador ELF** (`kernel/proc/elf.c`): ELF64 estático (`ET_EXEC`, x86-64). Cada segmento
  `PT_LOAD` ganha páginas zeradas com as permissões do próprio segmento (escrita só com `PF_W`,
  execução só com `PF_X`). Programas não podem mapear abaixo de 4 MiB nem no topo da metade baixa.
- **Pilha de usuário**: 64 KiB abaixo de `0x00007FFFFFFFE000`, com página de guarda. O topo segue a
  ABI System V (`argc`, `argv`, `envp`, `auxv` vazios), pronto para a musl na E6.
- **Syscalls** (`SYSCALL`/`SYSRET`): MSRs `STAR`/`LSTAR`/`FMASK`; a entrada (`syscall.asm`) usa
  `SWAPGS` só para trocar de pilha (bloco por CPU com a pilha do kernel da thread atual). Numeração,
  registradores e `errno` do **Linux x86-64**. Implementadas: `write` (stdout/stderr → console),
  `sched_yield`, `nanosleep`, `getpid`, `gettid`, `clock_gettime`, `exit`, `exit_group`. As demais
  devolvem `-ENOSYS`. **Todo ponteiro de usuário é verificado** nas tabelas de páginas do processo
  (`-EFAULT` se não for memória de usuário mapeada).
- **FPU/SSE por thread**: o estado é salvo (`fxsave`) e restaurado (`fxrstor`) a cada troca; o kernel
  é compilado sem SSE, então só os programas usam esses registradores.
- **Falhas de programas**: uma exceção em ring 3 (NULL, instrução privilegiada, ler memória do kernel,
  divisão por zero...) mostra uma mensagem em amarelo e encerra **só aquele processo**, com código de
  saída 128 + sinal (139 = SIGSEGV, 132 = SIGILL, 136 = SIGFPE). O sistema continua.
- **Programas de usuário** (`user/`): cada pasta vira `build/user/<nome>.elf`, ligado em 0x400000 com
  uma biblioteca mínima (`user/lib`: `_start`, `printf`, `write`, `sleep_ms`... **LGPL-3.0-or-later**),
  que a musl substitui na E6. Sem sistema de arquivos ainda, os ELFs vão **embutidos no kernel**
  (`build/gen/user_programs.asm`, gerado pelo `build.py`).
- **Autoteste de usuário** (todo boot): `usertest` confere as syscalls a partir do ring 3 (resultados,
  `-ENOSYS`, `-EFAULT` para ponteiro do kernel e endereço não mapeado, `-EBADF`, `-EINVAL`, sleep,
  estado SSE preservado); dois `hello` rodam ao mesmo tempo; `crashnull`, `crashpriv` e `crashkernel`
  são encerrados e o kernel continua; no fim, nenhuma página vazou. Verificado que o teste **falha**
  se a verificação de ponteiros for desligada.
- **Timer mais estável**: a calibração do LAPIC agora usa a mediana de 5 medições de 25 ms (antes,
  menor de 3 × 10 ms variava de 268 a 717 MHz no VirtualBox; agora dá ~1000 MHz sempre). Testes de
  tempo exigem "nunca acordar antes", com folga de até 1 s para atrasos do host.
- **Ferramenta de testes**: as variantes com linha de comando reaproveitam um único
  `build/variant.img` (antes eram 128 MiB por teste), o VirtualBox roda primeiro e cada caso mostra
  quanto tempo levou. Houve duas falhas intermitentes no VirtualBox logo após gravar muitas imagens;
  a causa provável (disco do host ocupado) não foi confirmada, mas não se repetiu em 10 execuções.

### 10.1.5 Como a E5 funciona

- **Disco**: varredura PCI pelas portas 0xCF8/0xCFC e driver **AHCI** (SATA), que funciona no `q35` do
  QEMU, no VirtualBox e na maioria dos PCs. Versão simples: um comando por vez, conclusão por
  *polling*, LBA48 com DMA. Camada de blocos de 4 KiB com **cache write-through** (256 blocos) e
  leitura da **GPT** para achar a partição raiz (tipo `4f50414c-494e-4552-4f4f-542d45465333`).
- **Imagem**: o disco agora tem a ESP (64 MiB) e a **partição raiz EFS3** (o resto, ~62 MiB). O
  `build.py` monta `build/root/` com `rootfs/` (arquivos fixos do sistema) + os programas (`/apps/<nome>`,
  o shell em `/system/bin/sh`) e o `mkimage.py` grava a partição e a relê para conferir.
- **EFS3** (`kernel/fs/efs3.c`, formato documentado em `tools/efs3.py`): blocos de 4 KiB, inodes de
  128 bytes com 12 ponteiros diretos + indireto + duplamente indireto (até ~4 GiB por arquivo),
  bitmaps de blocos e inodes, diretórios com registros de tamanho variável. Criar, ler, gravar,
  truncar, renomear (substituindo arquivos), apagar, `mkdir`/`rmdir`. Um arquivo apagado enquanto
  aberto só some quando é fechado. Um *mutex* por sistema de arquivos. (Na E5 não havia journal; a
  SE2 o acrescentou, seção 10.1.9.)
- **Nomes UTF-8 em NFC**: o VFS valida o UTF-8 e compõe acentos (tabela gerada por
  `tools/gen_nfc.py` com os dados Unicode do Python: latim, grego, cirílico). `ação` digitado com
  acentos combinantes é o mesmo arquivo. O `efs3.py` também grava em NFC.
- **VFS** (`kernel/fs/vfs.c`): vnodes com tabela de operações, resolução de caminhos (`.`, `..`,
  barras repetidas), pontos de montagem (`/dev`), arquivos abertos compartilhados por `dup`.
- **/dev** (devfs): `console` e `tty` (o terminal), `null`, `zero`.
- **Pipes**: buffer circular de 4 KiB; leitura espera dados (fim de arquivo quando não há mais
  escritores), escrita espera espaço (`-EPIPE` sem leitores).
- **Terminal (TTY)**: modo canônico (eco, Backspace sabendo UTF-8, Enter entrega a linha, Ctrl+D =
  fim de arquivo, Ctrl+C descarta a linha), `TIOCGWINSZ` com o tamanho do console.
- **Teclado PS/2** (layout US), lido por uma thread a cada 10 ms. Interrupções pelo I/O APIC e os
  layouts US-International/ABNT2 ficam para a E8; USB para a E13.
- **Escalonador**: filas de espera (`waitqueue`) e `mutex` que bloqueia a thread (pode ser mantido
  durante acesso a disco).
- **Processos**: descritores de arquivo (0-2 = terminal), diretório atual, `argv` na pilha (ABI System
  V), pai/filho, `wait4` com status no formato do Linux, órfãos liberados sozinhos.
- **Syscalls novas** (numeração Linux): `read`, `open`, `openat` (só `AT_FDCWD`), `close`, `stat`,
  `fstat`, `lstat`, `lseek`, `ioctl`, `readv`, `writev`, `pipe`, `pipe2`, `dup`, `dup2`, `wait4`, `fsync`,
  `ftruncate`, `getcwd`, `chdir`, `rename`, `mkdir`, `rmdir`, `unlink`, `getdents64`, `getppid`; e a
  primeira própria do OpalineOS, **`spawn(path, argv)` = 1000** (até existir `fork`/`execve` na E6).
- **Shell** (`/system/bin/sh`): `ls [-l]`, `cat`, `echo`, `touch`, `mkdir`, `rmdir`, `rm`, `mv`, `cd`,
  `pwd`, `help`, `exit`, redirecionamento `>`/`>>`, aspas; o resto roda programas de `/apps` e
  `/system/bin`. O kernel inicia o shell no fim do boot e o reinicia se ele sair.
- **Testes da E5**:
  - `fstest` (todo boot, do ring 3): arquivo com acento criado no build, nomes NFD achando o arquivo
    NFC, arquivo de 300 KiB (blocos indiretos), `lseek`, `ftruncate`, `O_APPEND`, `rename`, erros
    (`ENOENT`, `EEXIST`, `ENOTEMPTY`, `EISDIR`, `ENOTDIR`, `ESPIPE`, `EPIPE`, `EBADF`), `/dev/null`,
    `/dev/zero`, pipes, `dup2` desviando o stdout para arquivo, `spawn` + `wait4` (inclusive filho que
    trava com sinal 11) e um contador em `/home/persist.txt`;
  - `shell`: o QEMU **digita** comandos (`sendkey`) e o teste confere a saída;
  - `persistence`: dois boots da mesma imagem; o segundo encontra o contador do primeiro; após cada
    boot o disco é lido **no Windows** (`tools/efs3.py`) e passa por um **fsck** (bitmaps, contagem de
    links, blocos duplicados). Verificado que o fsck **acusa** quando o kernel deixa de liberar blocos.
- **`tools/efs3.py`**: `ls`, `cat`, `tree` e `check` (fsck) numa imagem de disco, direto do Windows.
  No Git Bash, use `MSYS_NO_PATHCONV=1` antes do comando (senão `/home/...` vira um caminho do Windows).

### 10.1.6 Terminal e energia (depois da E5)

- **Edição de linha no terminal** (`kernel/dev/tty.c`): cursor em bloco piscante; setas ←/→,
  Home/End, Backspace e Delete em qualquer ponto da linha; inserir no meio do texto; histórico das
  últimas 16 linhas com ↑/↓. As teclas especiais chegam como sequências ANSI (`ESC [ D`...), as mesmas
  de um emulador de terminal, prontas para o app Terminal (E10).
- **A tela sempre mostra o que a linha contém**: o terminal registra quantas células cada byte ocupou
  ao ser ecoado (um Tab ocupa de 1 a 4) e redesenha o resto da linha a cada mudança no meio. Corrigiu
  um bug em que Tab + Backspace desalinhava a tela e o comando executado era diferente do visível.
- **Desligar e reiniciar por ACPI** (`kernel/dev/acpi.c`): RSDP → XSDT → FADT (portas PM1a/PM1b,
  registrador de reset) e o pacote `\_S5` do DSDT. QEMU usa a porta 0x604 com S5 = 0; o VirtualBox, a
  porta 0xB004 com S5 = 5 — os dois desligam pelas tabelas, sem depender das portas de reserva.
  Antes de desligar, o cache do disco é descarregado (`FLUSH CACHE`).
- **Syscall `reboot` (169)** com os números mágicos do Linux; o shell tem `poweroff` e `reboot`. **Sair
  do shell principal (`exit` ou Ctrl+D) desliga o computador**; um `sh` aberto dentro de outro só volta
  ao anterior. Desde a SE1, só o root pode desligar ou reiniciar.
- **Testes**: o teste do shell faz seis edições (Tab+Backspace, setas+Backspace, Home+digitar,
  Delete, ↑ do histórico, Tab no meio + setas + Delete) e compara a linha pixel a pixel com o mesmo
  comando digitado direto; no fim digita `exit` e exige que o QEMU desligue sozinho. O teste do
  VirtualBox também digita `exit` e exige que a VM desligue. Verificado que o teste de edição falha com
  o `tty.c` anterior.

### 10.1.7 Como a E6 funciona

- **musl 1.2.6** sem modificações (`tools/ports/musl.py`): baixada uma vez de musl.libc.org, conferida
  pelo **SHA-256** (registrado depois de verificar a assinatura GPG da chave da musl) e com o patch de
  segurança da `regcomp` (CVE-2026-107891) em `ports/musl/patches/`. Compilada para
  `x86_64-unknown-linux-musl` por um script Python que reproduz o Makefile (o Windows não tem `make`):
  `build/sysroot/` com `include/`, `libc.a`, `libc.so` e os `crt*.o`. Graças à ABI mista, o kernel
  atende as mesmas syscalls do Linux e a musl não sabe que não está no Linux.
- **Runtime do compilador próprio** (`ports/rt/`, LGPL): `__mulsc3`/`__divdc3`... (multiplicação e
  divisão de complexos, C99 Anexo G), que no Linux vêm da libgcc/compiler-rt.
- **Carregador dinâmico**: a `libc.so` da musl também é o `ld.so`; o build a instala como
  `/lib/ld-musl-x86_64.so.1`. O kernel carrega o programa (ET_EXEC ou PIE em 0x555555554000), vê o
  `PT_INTERP`, carrega o interpretador em 0x7FF000000000 e começa por ele, com o **vetor auxiliar**
  (`AT_PHDR`, `AT_ENTRY`, `AT_BASE`, `AT_RANDOM`, `AT_EXECFN`...) e o ambiente na pilha (ABI System V).
- **Memória por áreas** (`kernel/mm/uvm.c`): cada processo tem um `mm` com uma lista ordenada de
  áreas (início, fim, proteção, anônima ou de arquivo). As páginas só são alocadas no primeiro acesso
  (**paginação sob demanda**): a falta de página em modo usuário consulta as áreas, aloca uma página
  zerada ou lê o pedaço do arquivo. `mmap` (anônimo e de arquivo, `MAP_FIXED`, posições de cima para
  baixo abaixo de 0x7F0000000000), `munmap`, `mprotect` (divide áreas), `brk`; `mremap` só encolhe.
  Pilha principal de 8 MiB reservada e paginada sob demanda. Cada ponteiro recebido numa syscall é
  conferido contra as áreas e as páginas já são trazidas, então o kernel nunca falha ao copiá-los.
  (Na E6, `MAP_SHARED` agia como `MAP_PRIVATE` e o `fork` copiava todas as páginas; a SE1 resolveu,
  seção 10.1.8.)
- **Processos e threads** (`kernel/proc/process.c`): `fork`, `vfork`, `clone` (threads com
  `CLONE_THREAD`, `CLONE_SETTLS`, `CLONE_PARENT_SETTID`, `CLONE_CHILD_SETTID`, `CLONE_CHILD_CLEARTID`;
  `CLONE_VM|CLONE_VFORK` do `posix_spawn` da musl), `execve` (fecha os descritores `FD_CLOEXEC`, zera
  os sinais tratados), `exit` (só a thread) e `exit_group` (todas). PIDs e TIDs saem do mesmo contador:
  o TID da thread principal é o PID, como no Linux. Um processo termina com a última thread, libera
  memória e arquivos na hora e fica **zumbi** até o pai (ou o kernel) recolher o status. (`execve` num
  processo com várias threads devolvia `EAGAIN`; resolvido na SE1.)
- **TLS**: `arch_prctl(ARCH_SET_FS)` guarda a base do FS por thread; o escalonador a recarrega a cada
  troca. **Futex** (`kernel/proc/futex.c`): `WAIT`, `WAKE`, `REQUEUE`, `CMP_REQUEUE`, `WAIT_BITSET`,
  com tempo limite — a base dos mutexes, variáveis de condição e do `pthread_join` da musl.
- **Sinais** (`kernel/proc/signal.c`): `rt_sigaction`, `rt_sigprocmask`, `rt_sigpending`,
  `rt_sigsuspend`/`pause`, `sigaltstack`, `kill`, `tkill`, `tgkill`. Entrega na volta ao modo usuário,
  com o mesmo `rt_sigframe` do Linux (registradores, `siginfo`, estado da FPU) e retorno por
  `rt_sigreturn`; `SA_SIGINFO`, `SA_RESTART`, `SA_NODEFER`, `SA_RESETHAND`, `SA_ONSTACK`. Falhas da CPU
  viram sinais (`SIGSEGV` com `si_addr`, `SIGILL`, `SIGFPE`...) que o programa pode tratar; sem
  tratador, o processo é encerrado como antes. Ação padrão: terminar (parar e continuar desde a SE1).
  `SIGPIPE` ao escrever num pipe sem leitores.
- **Esperas interrompíveis**: leitura de pipe/terminal, `wait4`, `nanosleep` e futex terminam com
  `EINTR` quando chega um sinal ou o processo está saindo; o escalonador ganhou bloqueio com tempo
  limite (`thread_block`) e interrupção (`thread_interrupt`).
- **Entrada de syscall**: agora salva o mesmo quadro de uma interrupção. A volta normal é por
  `SYSRET`; quando o kernel reescreve os registradores (tratador de sinal, `rt_sigreturn`, `execve`),
  a volta é por `IRETQ`. **Correção**: o MSR STAR passou a ter RPL 3 no seletor base — em CPUs AMD o
  `SYSRET` não força o RPL do SS, e o próximo `IRETQ` de uma interrupção derrubava o kernel (bug
  latente desde a E4, apareceu no VirtualBox num Ryzen).
- **Syscalls novas** (numeração Linux): `mmap`, `mprotect`, `munmap`, `brk`, `mremap`, `madvise`,
  `msync`, `clone`, `fork`, `vfork`, `execve`, `kill`, `tkill`, `tgkill`, `rt_sig*`, `sigaltstack`,
  `pause`, `futex`, `set_tid_address`, `set_robust_list`, `arch_prctl`, `openat`/`newfstatat`/
  `faccessat`/`mkdirat`/`unlinkat`/`renameat`/`readlinkat` relativos a um diretório aberto, `fcntl`,
  `dup3`, `pread64`, `pwrite64`, `access`, `readlink`, `uname`, `getrandom`, `prlimit64`, `getrlimit`,
  `clock_nanosleep`, `clock_getres`, `gettimeofday`, `time`, `sched_getaffinity`, `getuid`... (na E6
  havia um só usuário e o relógio contava desde o boot; a SE1 trouxe usuários e o relógio real).
- **Ambiente padrão** dos programas iniciados pelo kernel: `HOME=/home`, `PATH=/apps:/system/bin`,
  `LANG=C.UTF-8`. Aleatoriedade (`AT_RANDOM`, `getrandom`): RDRAND quando existe, misturado ao TSC.
- **Testes da E6**: `tests/posix/posixtest.c`, programa C comum, compilado duas vezes —
  `/apps/posixtest` (dinâmico, pelo `ld.so`) e `/apps/posixtest-static` — e executado em todo boot
  (63 verificações cada): arquivos e diretórios com stdio, `malloc` (2000 blocos + 8 MiB),
  `mmap`/`mprotect`/`munmap` com `SIGSEGV` capturado por `siglongjmp`, mapeamento de arquivo, sinais
  (bloqueio, pendentes, `SA_SIGINFO`), `fork` + pipe, 200 KB entre processos, `execve` com argumentos e
  ambiente, `posix_spawn`, filhos mortos por `SIGABRT` e `SIGTERM`, pthreads (mutex com 4 threads,
  produtor/consumidor com variável de condição, `__thread`, thread destacada), `uname`, `usleep`,
  `getrandom`, `printf` de ponto flutuante e números complexos. Depois dos dois, o kernel confere que
  **nenhuma página de memória se perdeu**. Testes negativos feitos: sem recarregar a base do FS na troca
  de thread, o teste cai com `#GP`; sem atualizar as páginas no `mprotect`, ele acusa 2 falhas.
- **Teste do VirtualBox mais confiável** (`tools/vm.py`): a consulta "a VM ainda está ligada?" às vezes
  respondia "não" com a VM rodando, e o teste a desligava no meio do boot (o log do VirtualBox mostrava
  o pedido de desligamento vindo de fora). Agora usa o estado da VM (`VMState`) e só acredita em
  "desligada" se a resposta se repetir 1 s depois. Era a origem das falhas esporádicas do VirtualBox
  registradas desde a E4; depois da correção, 10 boots seguidos passaram.

### 10.1.8 Como a SE1 funciona (simplificações da E6 resolvidas)

- **Contagem de referências por página física** (`kernel/mm/pmm.c`): 16 bits por página, guardados
  junto do bitmap. Uma página de usuário só é liberada quando a última entrada de tabela de páginas
  (ou objeto de memória) que a usa vai embora.
- **Copy-on-write no `fork`** (`kernel/mm/uvm.c`): pai e filho mapeiam as mesmas páginas privadas
  como somente leitura; a primeira escrita de qualquer um dos dois ganha uma cópia própria (ou, se a
  outra cópia já se foi, a página só volta a ser gravável). O kernel também quebra o COW antes de
  escrever em memória de usuário (ponteiros de syscalls, `CLONE_CHILD_SETTID`).
- **`MAP_SHARED` de verdade**: um **objeto de memória** guarda as páginas por índice. Para um arquivo,
  é o cache de páginas dele (`vnode->memobj`, enquanto houver mapeamento compartilhado); para memória
  anônima compartilhada, um objeto próprio que o `fork` compartilha. `read()`/`pread()` gravam no
  arquivo antes o que os mapeamentos mudaram; `write()`/`pwrite()` atualizam as páginas já carregadas,
  então os dois lados sempre concordam. `msync`, `fsync` e o último `munmap` gravam de volta (todas as
  páginas de um mapeamento gravável, nunca além do fim do arquivo). Mapear gravável e compartilhado
  exige o arquivo aberto para leitura e escrita (`EACCES`).
- **Futex compartilhado**: sem `FUTEX_PRIVATE_FLAG`, um endereço dentro de memória compartilhada vira a
  chave (objeto, deslocamento), igual em todos os processos que a mapeiam.
- **`mremap`**: encolhe, cresce no lugar se houver espaço livre logo depois, ou (com `MREMAP_MAYMOVE`)
  muda a área de lugar levando as páginas e suas referências; `MREMAP_FIXED` também.
- **`execve` num processo com várias threads**: depois que o novo programa carregou, as outras threads
  terminam (na próxima volta ao modo usuário; esperas interrompíveis acabam na hora), a que chamou
  `execve` vira a thread principal (TID = PID) e o processo segue.
- **Relógio real** (`kernel/dev/rtc.c`): o RTC CMOS é lido uma vez no boot (BCD ou binário, 12/24 h,
  lido duas vezes até as leituras baterem); daí em diante, hora = leitura + ticks. `CLOCK_REALTIME`,
  `gettimeofday` e `time` usam esse relógio; os outros relógios contam desde o boot.
  `clock_settime`/`settimeofday` só para o root. O RTC guarda UTC (o `vm.py` liga `--rtc-use-utc` no
  VirtualBox; o QEMU já é UTC). Com Windows no mesmo PC (que guarda hora local no RTC), a E14 resolve.
- **Tempos de arquivo**: o inode do EFS3 ganhou `uid`, `gid` e `atime` no espaço reservado (formato
  compatível: imagens antigas têm zero ali, ou seja, root e 1970). `mtime`/`ctime` mudam ao gravar,
  truncar e mexer em diretórios; `ctime` ao mudar dono, modo ou links; `atime` só na criação e no
  `utimensat` (não muda a cada leitura, para não gravar no disco a cada `read`). `stat` devolve os
  três com nanossegundos. O `efs3.py` grava a hora do build em todos os arquivos da imagem.
- **Usuários e permissões** (`opaline/cred.h`, `kernel/proc/cred.c`): uid/euid/suid, gid/egid/sgid e
  até 32 grupos por processo; `fork` copia, `execve` aplica os bits **setuid/setgid** do arquivo (e passa
  `AT_SECURE` à musl). O VFS confere, com o uid efetivo: `x` em cada diretório de um caminho, `r`/`w` no
  `open` (e `w` com `O_TRUNC`), `w`+`x` no diretório para criar, apagar e renomear, o **sticky bit**
  (em `/tmp`, só o dono apaga), `x` para executar. Root passa em tudo, mas só executa arquivos com algum
  bit `x`. `access()` usa o uid real. Arquivos novos: modo pedido menos a `umask` (022), dono = uid
  efetivo, grupo = gid efetivo (ou o do diretório, se ele tiver setgid). `chmod` (dono ou root), `chown`
  (root; o dono pode trocar o grupo para um dos seus; tira setuid/setgid), `utimensat` (dono, ou só
  "agora" com permissão de escrita), `umask`, `setuid`/`setreuid`/`setresuid` e os de grupo com as
  regras POSIX, `getgroups`/`setgroups`, `kill` só para processos do mesmo usuário (ou root),
  `reboot` só root. `/etc/passwd` e `/etc/group` têm `root` (0) e `user` (1000, em `/home/user`);
  `/tmp` é 1777 e `/root` é 0700. **O sistema continua iniciando como root**: login, senhas e `sudo`
  ficam para a E15.
- **Controle de tarefas**: cada processo tem grupo (`pgid`) e sessão (`sid`); os iniciados pelo kernel
  abrem uma sessão que é dona do terminal. `setpgid`, `getpgid`, `getpgrp`, `setsid`, `getsid`;
  `kill(0, …)` e `kill(-pgid, …)` mandam para um grupo. O terminal sabe qual grupo está em **primeiro
  plano** (`TIOCGPGRP`/`TIOCSPGRP`, `TIOCSCTTY`, `TIOCNOTTY`, `TIOCGSID`): **Ctrl+C = `SIGINT`,
  Ctrl+Z = `SIGTSTP`, Ctrl+\ = `SIGQUIT`** para esse grupo. Um grupo em segundo plano que lê o terminal
  recebe `SIGTTIN` (ou `EIO`, se o ignora); trocar o primeiro plano de fora dele dá `SIGTTOU`.
- **Parar e continuar**: `SIGSTOP` (na hora, não pode ser tratado nem bloqueado), `SIGTSTP`, `SIGTTIN` e
  `SIGTTOU` sem tratador param o processo inteiro: cada thread espera na volta ao modo usuário até
  `SIGCONT` (ou `SIGKILL`). Um processo sem pai ignora os de parada (ninguém o continuaria). O pai
  recebe **`SIGCHLD`** quando o filho termina, para ou continua (a não ser com `SA_NOCLDSTOP`), e o
  `wait4` informa com `WUNTRACED`/`WCONTINUED` (status no formato do Linux); grupos em `wait4(0)` e
  `wait4(-pgid)`. Um pai que ignora `SIGCHLD` (ou usa `SA_NOCLDWAIT`) não deixa zumbis.
- **Syscalls que recomeçam**: uma chamada interrompida por um sinal que não roda tratador (ignorado,
  ou uma parada e continuação) recomeça sozinha em vez de devolver `EINTR`, como no Linux; com
  tratador, só com `SA_RESTART`. Uma leitura bloqueada que o Ctrl+Z parou continua normalmente
  depois do `fg`.
- **`poll`/`ppoll`** (adiantado da SE3, porque a musl usa `poll` ao iniciar programas setuid): pipes e
  terminal dizem quando estão prontos e avisam a cada mudança; arquivos estão sempre prontos. Espera
  por eventos com tempo limite e máscara de sinais (`ppoll`). `select` fica para a SE3.
- **`sysinfo`**: memória total e livre (usada pelo teste do copy-on-write).
- **Shell**: cada comando roda como um job em primeiro plano (`spawn` ganhou a opção
  `SPAWN_FOREGROUND`); o próprio shell ignora os sinais do teclado; Ctrl+Z mostra
  `[programa stopped; type fg to continue it]` e `fg` continua o programa.
- **Testes da SE1** (no `posixtest`, que passou de 63 para 104 verificações, dinâmico e estático):
  - um `fork` de 16 MiB já tocados gasta menos de 4 MiB, e as escritas do filho viram cópias;
  - memória anônima compartilhada com um futex entre processos;
  - mapeamento compartilhado de arquivo nos dois sentidos, e com um filho;
  - `mremap` que muda de lugar e cresce;
  - `execve` a partir de 4 threads;
  - relógio depois de 2026, `mtime` de arquivo novo e `utimensat`;
  - `umask`, `/tmp` 1777, setuid 04755, `chown`;
  - **um filho como usuário 1000** (setgid/setuid) confere 18 recusas e permissões (`EACCES`, `EPERM`,
    sticky bit, programa setuid com `AT_SECURE`, `reboot` e `clock_settime` recusados, `kill` no root
    recusado);
  - `SIGSTOP`/`SIGCONT`/`SIGTSTP`/`SIGKILL` com `WUNTRACED`/`WCONTINUED` e `SIGCHLD`;
  - `read` parado no meio que recomeça;
  - grupos com `setpgid`, `kill(-pgid)`, `kill(0)` e `setsid`.

  O teste do shell digita `posixtest sleep` (um programa que dorme 300 s) e exige que **Ctrl+C**
  devolva o prompt; depois para com **Ctrl+Z**, continua com `fg` e encerra com Ctrl+C.

  Testes negativos feitos, cada um acusado: sem copiar no COW, o teste do `fork` falha (o pai vê as
  escritas do filho); sem a checagem de permissão, 6 verificações do usuário 1000 falham; sem marcar
  as threads paradas para recomeçar, a leitura volta com `EINTR`; sem atualizar as páginas no `write()`,
  o mapeamento compartilhado fica velho. (Este último exigiu corrigir o teste: a página precisava já
  estar carregada.)

### 10.1.9 Como a SE2 funciona (journal e links)

- **EFS3 versão 2**: o superbloco ganhou `journal_start` e `journal_blocks`, e uma área de **journal**
  de 256 blocos (1 MiB) fica entre a tabela de inodes e os dados. O formato é o mesmo de antes nos
  demais pontos; o kernel ainda monta imagens da versão 1, mas sem journal.
- **Cada operação é uma transação** (`kernel/fs/efs3.c`). Os blocos de metadados que ela muda
  (superbloco, bitmaps, inodes, diretórios, blocos indiretos, o alvo de um link simbólico) ficam em
  memória enquanto ela roda. No fim:
  1. as cópias vão para o journal com um descritor (a lista de blocos);
  2. *flush*;
  3. bloco de **commit** com checksum (FNV-1a de tudo), *flush*;
  4. cada bloco é gravado no seu lugar, *flush*;
  5. o cabeçalho do journal avança o número da transação ("feita"), *flush*.
- **Modo ordenado** (como o ext4): o **conteúdo** dos arquivos vai direto para o disco antes do
  commit, então um arquivo nunca aponta para lixo. Depois de uma queda, o sistema de arquivos está
  sempre consistente; o conteúdo de uma escrita interrompida pode ficar pela metade.
- **Blocos liberados só voltam ao bitmap no commit**, para a mesma transação não reaproveitar um bloco
  que os metadados já gravados ainda usam. Escritas grandes viram transações de 256 KiB cada, e tabelas
  indiretas liberadas inteiras não são zeradas, para uma transação sempre caber no journal.
- **Ao montar**: se há uma transação com commit válido (checksum certo) e número ainda não marcado como
  feito, ela é **regravada** ("EFS3: journal replayed"). Depois, inodes **órfãos** (apagados enquanto
  abertos quando a energia caiu) são liberados.
- **Custo**: cada operação grava os metadados duas vezes e faz 4 *flushes*. Os testes no QEMU usam
  `cache=unsafe`, o que deixa o *flush* barato; o VirtualBox já ignora *flushes* por padrão. Agrupar
  várias operações numa transação fica para quando o desempenho pedir.
- **Links simbólicos**: inode do tipo `S_IFLNK` (0777) cujo primeiro bloco guarda o caminho alvo; tipo
  3 nas entradas de diretório e `DT_LNK` no `getdents64`.
  - O VFS **segue links** ao resolver caminhos: alvos relativos partem do diretório do link, absolutos
    da raiz, e há no máximo 40 por busca (depois, `ELOOP`).
  - O último componente não é seguido por `lstat`, `readlink`, `unlink`, `rename`, `O_NOFOLLOW`
    (`ELOOP`), `AT_SYMLINK_NOFOLLOW` e `lchown`. `chmod` num link muda o alvo; `fchmodat` com
    `AT_SYMLINK_NOFOLLOW` dá `EOPNOTSUPP`, como no Linux.
- **Links físicos**: `link`/`linkat` (com `AT_SYMLINK_FOLLOW`) acrescentam um nome ao mesmo inode e
  aumentam a contagem de links; diretórios não podem (`EPERM`), nem entre sistemas de arquivos
  (`EXDEV`).
- **`getcwd` mostra o caminho real**: calculado na hora, subindo por `..` e procurando o nome de cada
  diretório no pai (`vfs_path_of`), então funciona depois de `cd` por um link e acompanha um diretório
  renomeado. `chdir` passou a exigir permissão de busca; `fchdir` novo.
- **`tools/efs3.py`**:
  - cria imagens da versão 2 com o journal vazio;
  - `check` acusa transação pendente e entende links físicos (o mesmo inode com vários nomes) e
    simbólicos, conferindo também se o tipo de cada entrada de diretório bate com o do inode;
  - `tree`, `ls` e `cat` reaplicam um journal pendente em memória, para ler imagens de uma máquina que
    "perdeu energia"; `tree` mostra `link -> alvo`.
- **Teste de queda de energia** (`kernel/fs/powercut.c` e `vm.py`, teste `powercut`).
  - Com `test=powercut:N`, o kernel roda uma carga fixa em `/home/pc`:
    - cria arquivos, inclusive um de 300 KiB (com blocos indiretos), e grava neles;
    - links simbólicos e físicos, renomear e substituir, truncar;
    - 20 arquivos criados e 10 apagados, remover diretório;
    - um arquivo apagado enquanto aberto.
  - Depois de N escritas no disco, as próximas são descartadas em silêncio ("o disco perdeu energia")
    e a máquina para sem *flush*. O `vm.py` então apaga o parâmetro da imagem, dá um boot normal e roda
    o fsck do host.
  - A primeira rodada mede o total de escritas da carga (1178); depois corta em 10 pontos espalhados
    por ela.
  - Resultado: fsck limpo em todos os pontos, o journal foi reaplicado em 5 deles e o órfão foi
    liberado.
  - **Teste negativo**: com o kernel gravando os metadados direto no lugar (sem journal), o mesmo
    teste acusa 4 pontos com bitmap e contadores do superbloco errados.
- **`posixtest`**: 127 verificações (22 novas de links), entre elas:
  - `readlink`, `lstat` × `stat`, link relativo e absoluto, link pendente;
  - laço de links (`ELOOP`), `O_NOFOLLOW`;
  - `chdir` por link e `getcwd`, `realpath` da musl, `DT_LNK`;
  - renomear e apagar links, `chmod` pelo link;
  - links físicos: mesmo inode, 2 links, o arquivo continua vivo pelo outro nome;
  - `EPERM` para diretório, `EEXIST`.

### 10.2 Geral

- **Toolchain**: LLVM (clang, ld.lld) para os alvos `x86_64-unknown-uefi` (bootloader) e
  `x86_64-opaline` (kernel, libc e ports; um alvo próprio baseado em ELF), NASM, Python 3 (scripts
  de build e o "python de build" necessário para compilar o CPython de forma cruzada), QEMU + OVMF,
  VirtualBox.
- **Depuração**: log serial (COM1), GDB pelo QEMU (`-s -S`) e canal de controle pela serial
  (evolução do `vmctl.py` do v2: cliques, teclas, comandos e screenshots).
- **Testes**: cada etapa tem um teste que roda a VM sem interface gráfica e lê o resultado na
  serial (`TEST_OK` / `TEST_FAILED`). Da E7 em diante, partes da suíte oficial do CPython viram
  teste de regressão da camada POSIX.
- **Licenças de terceiros** (todas compatíveis com a **GPLv3** do sistema): musl (MIT), CPython
  (PSF), OpenSSL 3 (Apache-2.0, compatível com GPLv3, mas não com GPLv2), zlib (zlib), libffi (MIT),
  sqlite (domínio público), FreeType (FTL, compatível com GPLv3), HarfBuzz (MIT), Inter e Noto
  (OFL; fontes são distribuídas junto, sem mudar a licença do sistema). Patches nossos em código de
  terceiros seguem a licença original de cada projeto.
- **Fronteira das syscalls**: como no Linux, programas que só usam o sistema via syscalls **não**
  são considerados obras derivadas do kernel. Essa nota está em `LICENSING.md`.

---

## 11. Fora de escopo (por enquanto)

- Rodar programas do Windows. Programas do Linux só via recompilação ou, se a ABI de syscalls for
  igual à do Linux, em casos simples.
- Pacotes Python que dependem de GPU ou de outro sistema gráfico (PyTorch, PyQt, Tkinter).
- Aceleração de GPU real, Wi-Fi e Bluetooth reais (os atalhos existem na interface, mas os drivers
  vêm muito depois).
- Linguagem própria (PyOS) e compilador próprio de Python.

---

## 12. Registro de decisões

| Data | Decisão |
|---|---|
| 2026-11-17 | Kernel novo (não evoluir o v2). x86-64 + UEFI, ring 3, ELF64 |
| 2026-11-17 | Visual baseado nas referências (vidro escuro + coral). Design system "Vitrum", ícones próprios |
| 2026-11-17 | ~~PyOS 3 compilado AOT, com compilador próprio~~ → substituído pela decisão abaixo |
| 2026-11-17 | **CPython oficial** como Python do sistema, sobre a **musl** e uma camada **POSIX**. Apps em Python com o toolkit **`vitrum`** (núcleo C + API Python). Velocidade extra via **mypyc/Cython**. pip/PyPI suportados; pacotes com C via sistema de ports |
| 2026-11-17 | Fonte **Inter**. Módulos **`vitrum`** e **`erickos`**. VM no início, mas **PC real + dual boot com Windows** é meta (seção 3.3) |
| 2026-11-17 | Kernel **monolítico modular**. ABI de syscalls **mista** (POSIX igual ao Linux x86-64 + faixa própria a partir de 1000) |
| 2026-11-17 | Nome do sistema: **OpalineOS**. Toolkit distribuído como **`opaline-vitrum`** e importado como `from opaline import vitrum`. APIs do sistema em `opaline.sistema` (substitui `erickos`). O toolkit controla, por Python, todos os movimentos, animações e efeitos visuais, inclusive abrir/fechar/minimizar/maximizar, foco e transparência das janelas |

| 2026-11-17 | Licença **GPLv3**. Sistema **em inglês** (interface, APIs, bibliotecas, código e documentação técnica). APIs do sistema renomeadas para `opaline.system` (antes `opaline.sistema`). Interface em **en-US + pt-BR**, trocável a qualquer momento sem reiniciar. **Unicode/UTF-8** em todo o sistema |
| 2026-11-17 | **E0 concluída**: bootloader UEFI mínimo, imagem GPT/FAT32 gerada em Python, testes automáticos no QEMU e no VirtualBox. `project.md` continua em português |
| 2026-11-17 | Licenças: sistema **GPL-3.0-or-later**, bibliotecas para apps **LGPL-3.0-or-later**. Arquivos `COPYING`, `COPYING.LESSER`, `LICENSING.md`; cabeçalho SPDX em cada arquivo-fonte, com copyright "The OpalineOS authors". Repositório git iniciado |
| 2026-11-17 | **E1 concluída**: kernel de 64 bits no *higher half*, console UTF-8 com fonte própria (DejaVu Sans Mono), GDT/TSS/IDT, diagnóstico de exceções. Bootloader usa só `EfiLoaderData` por causa do VirtualBox |
| 2026-11-19 | **E2 concluída**: bitmap de páginas físicas, tabelas de páginas próprias do kernel (W^X, NX, sem identidade, framebuffer WC), heap com slabs e detecção de liberação dupla, autoteste de memória em todo boot |
| 2026-11-19 | **E3 concluída**: LAPIC + timer a 1000 Hz (PIC mascarado), threads do kernel com escalonador round-robin preemptivo (fatia de 10 ms), pilhas com página de guarda, spinlocks no pmm/heap/vmm/console, autoteste do escalonador |
| 2026-11-19 | **E4 concluída**: processos com espaço de endereços próprio, carregador ELF64, SYSCALL/SYSRET com numeração Linux e verificação de ponteiros, FPU por thread, falhas de programas encerram só o processo. Biblioteca mínima de usuário (`user/lib`) sob LGPL-3.0-or-later |
| 2026-11-19 | **E5 concluída**: PCI + AHCI, cache de blocos, GPT com partição raiz EFS3, VFS com nomes UTF-8 em NFC, devfs, pipes, TTY canônico, teclado PS/2, syscalls de arquivos com numeração Linux, `spawn` (1000) + `wait4`, shell mínimo. Programas agora vêm do disco (`/apps`, `/system/bin`) |
| 2026-11-20 | **E6 concluída**: musl 1.2.6 sem modificações (build próprio em Python, SHA-256 conferido, patch da CVE-2026-107891), `libc.so` como `/lib/ld-musl-x86_64.so.1`, runtime do compilador próprio em `ports/rt` (LGPL). Kernel: memória por áreas com paginação sob demanda, `mmap`/`mprotect`/`brk`, `fork`/`vfork`/`clone`/`execve` com ELF dinâmico e vetor auxiliar, threads com TLS e futex, sinais no formato do Linux, esperas interrompíveis. STAR com RPL 3 (correção para CPUs AMD). Simplificações registradas: `MAP_SHARED` privado, `fork` sem copy-on-write, `execve` só em processo de uma thread |
| 2026-11-21 | Antes da E7, quatro **sub-etapas** (SE1–SE4) para o que ficou simplificado ou pendente até a E6. Usuários: permissões reais no kernel já na SE1, mas o sistema continua iniciando como root (login e `sudo` na E15). Controle de tarefas com Ctrl+C/Ctrl+Z junto na SE1 |
| 2026-11-21 | **SE1 concluída**: refcount de páginas, copy-on-write, `MAP_SHARED` real com cache de páginas coerente com `read`/`write`, futex compartilhado, `mremap` completo, `execve` com threads, RTC e tempos de arquivo, uid/gid/modo no EFS3 e permissões POSIX no VFS, setuid/setgid, grupos e sessões, parar/continuar, `SIGCHLD`, Ctrl+C/Ctrl+Z/Ctrl+\\, syscalls que recomeçam, `poll`/`ppoll`. `reboot` só root |
| 2026-11-21 | Visual passa de **vidro (glassmorphism) para relevo suave (neumorfismo)**: superfícies opacas, luz e sombra em cache, bege porcelana com destaque verde-petróleo (referência `docs/references/neumorfismo.webp`). Motivo: sem GPU, o desfoque do que está atrás das janelas é o efeito mais caro do compositor. Vidro fosco continua como efeito opcional. Toolkit renomeado: **`opaline-relief`** (`from opaline import relief as rl`; `opaline-relief` livre no PyPI e no GitHub, pesquisa de 2026-11-21). Design system "Relief". O nome do sistema continua **OpalineOS** (opalina é vidro leitoso e opaco, que combina com o relevo) |
| 2026-11-21 | **Layout da área de trabalho**: sem docks nem barras laterais ou inferiores; uma barra fina no topo é a barra de tarefas (botão Apps, um botão por programa aberto, indicadores, notificações e relógio). Prévia interativa aprovada: `docs/mockup/opaline-desktop.html` (movimentos, funções e design). Elementos coloridos afundados usam uma sombra interna própria (escura, quase sem realce), para não parecerem brilhantes. No toolkit, `ctx.dock_icon` virou `ctx.task_button` e `to_dock` virou `to_bar` |
| 2026-11-21 | **SE2 concluída**: EFS3 versão 2 com **journal** de metadados em modo ordenado (uma transação por operação, commit com checksum, reaplicado na montagem, órfãos liberados), links **simbólicos** (seguidos pelo VFS, até 40) e **físicos**, `getcwd` com o caminho real. Teste de queda de energia em 10 pontos de uma carga de 1178 escritas: fsck limpo em todos |
---

## Apêndice A — Candidatos a nome do sistema

Ideias ainda sem decisão. **Não foi verificado** se há marcas registradas ou projetos com o mesmo
nome: pesquisar antes de escolher.

- **Serpente / Python**: Serpens, Ophion, Hydrus, Naja, Jiboia, Caninana, Píton
- **Vidro / luz**: Opala, Vitra, Nácar, Lumen, Iris, Cristal
- **Cosmos** (papel de parede espacial): Lyra, Vega, Órbita, Zênite, Selene, Nyx
- **Leveza / velocidade**: Pluma, Brisa, Aero
- **Identidade visual**: Coral, Âmbar
- Evitar ou checar com cuidado: Mamba (gerenciador de pacotes Python), Aurora (já existe um OS),
  Quartz (camada gráfica da Apple), Prisma (ORM conhecido), Kaa (personagem da Disney)

### A.1 OpalOS (favorito) — pesquisa de 2026-11-17

Busca na web por "OpalOS", "Opal OS" e "opal-os": **nenhum sistema operacional ativo com esse nome**
foi encontrado. Ainda assim, "Opal" é um nome bastante usado em tecnologia:

| Uso de "Opal" | O que é | Risco para o OpalOS |
|---|---|---|
| Opal (Univ. de Washington, anos 1990) | sistema operacional **de pesquisa**, com espaço de endereços único. Encerrado | baixo, mas é um OS homônimo histórico |
| TCG Opal | padrão de **discos com criptografia própria** (aparece em BIOS e em ferramentas de disco) | confusão em contexto de disco e boot |
| OPAL (OpenPOWER) | **firmware** de servidores POWER | baixo |
| Google Opal | ferramenta de mini-apps com IA (Google Labs, 2025); pedido de marca "OPAL" na classe 42 (serviços de software) | **o maior risco de marca** se o projeto crescer |
| Optimizely Opal, Opal Security, o gerenciador de pacotes `opal` (originOS), a linguagem Ruby→JS Opal | produtos e projetos de software | confusão de busca |

Conclusão: para um projeto open source pessoal o risco é baixo. Se o projeto for divulgado em larga
escala, vale consultar um especialista em marcas. Variações que diferenciam mais: **OpalaOS**,
**Opaline**. Domínios e nomes no GitHub/PyPI ainda não foram verificados.

Por que a opala combina com o projeto:
- é **sílica amorfa hidratada**, praticamente um **vidro natural**, o mesmo material de que trata o
  toolkit `vitrum` ("vidro" em latim);
- o "jogo de cores" vem de **camadas de microesferas** que difratam a luz, como a interface feita de
  camadas de vidro translúcido;
- a **opala negra** parece uma nebulosa (fundo escuro com reflexos rosa, azul e verde), igual ao
  papel de parede e à paleta da referência;
- o **Brasil** é um dos maiores produtores (Pedro II, Piauí);
- é uma pedra que **dispersa a luz em várias cores**, assim como o sistema faz o Python se abrir para
  muitas bibliotecas e apps.

### A.2 OpalineOS e OpalinOS — pesquisa de 2026-11-17

| Verificação | OpalineOS | OpalinOS | OpalOS (comparação) |
|---|---|---|---|
| Sistema/produto com o nome exato (web) | nenhum | nenhum (só "opalinos": variedade de periquitos e vidro opalino, em espanhol) | nenhum ativo |
| Repositórios no GitHub com o nome | 0 | 0 | 6 pequenos e sem relação |
| Usuário/organização no GitHub | `opalineos` **livre** | `opalinos` **livre** | `opalos` ocupado (organização vazia, de 2021) |
| PyPI `<nome>` e `<nome>-os` | **livres** | **livres** | **livres** |
| Palavra-base no PyPI | `opaline` ocupado (pacote reservado, "name reservation" 0.0.2) | `opalin` livre | `opal` ocupado (framework web) |
| Palavra-base no GitHub | `opaline` muito usado (77 repos: cliente de YouTube para iOS, tema para TUI em Rust, framework de CLI...) | usuário `opalin` existe | — |
| Marcas registradas | OPALINE registrada para cosméticos, shows musicais e utensílios de vidro (Nadir Figueiredo, BR); **nenhuma em software** | **OPALIN: pedido pendente (mar/2026) que inclui a classe 9** (eletrônicos/software), de uma empresa da Califórnia | Google pediu OPAL na classe 42 |

Conclusão: **OpalineOS** é o mais livre dos três. Não há marca de software com esse nome, e o
GitHub e o PyPI estão livres com o "OS". OpalinOS também está livre no GitHub/PyPI, mas o pedido de
marca OPALIN na classe 9 é um risco maior. Os dados de marca vêm de agregadores (não do USPTO
direto) e só cobrem EUA/Canadá; o INPI (Brasil) não foi consultado.

> Observação: o nome **`vitrum` está ocupado no PyPI** (pacote científico de simulação de vidro).
> **Resolvido:** o toolkit é distribuído como `opaline-vitrum` e importado como `opaline.vitrum`.
>
> **Atualização de 2026-11-21:** com a troca do vidro pelo relevo, o toolkit passou a se chamar
> **`opaline-relief`** (importado como `opaline.relief`). Pesquisa: `opaline-relief` não existe no
> PyPI (a API JSON responde 404) nem no GitHub (nenhum repositório, conta `opaline-relief` livre), e a
> busca na web não achou projeto ou produto com esse nome. O nome curto `relief` está ocupado no PyPI
> (biblioteca de validação de dados de 2013, sem relação) — a mesma situação do `vitrum`, resolvida do
> mesmo jeito.
> Atenção: o nome `opaline` também está reservado no PyPI por terceiros. Por isso o sistema guarda o
> pacote `opaline` num diretório protegido do sistema, que vem antes do `site-packages`, e o `pip`
> do OpalineOS recusa instalar pacotes que sobrescrevam `opaline`.
