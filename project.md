# OpalineOS — Documento do Projeto

> Antigo nome de trabalho: ErickOS 3.

> Fonte única das decisões essenciais do projeto. Atualize este arquivo sempre que uma decisão
> for tomada ou mudar. Criado em 2026-11-17 · última revisão: 2026-11-17 (GPLv3, sistema em inglês, idiomas e Unicode).

---

## 1. Visão

Um sistema operacional **próprio, open source, leve, rápido e moderno**, em que **Python é o centro
do sistema**: a área de trabalho e os apps são escritos em **Python de verdade (CPython oficial)**,
usando **`opaline.vitrum`**, a biblioteca própria do sistema para criar janelas e interfaces. Os apps podem
usar bibliotecas reais baixadas com `pip`.

- **Kernel próprio em C**, escrito do zero, sem código de Linux nem de Windows.
- **Motor em C, lógica em Python**: o kernel, o compositor e o desenho dos componentes são nativos
  (a parte que define se o sistema "parece rápido"). A lógica dos apps é Python.
- Visual moderno: vidro fosco (glassmorphism), cantos arredondados, sombras suaves, destaque
  coral/rosa com brilho (glow) e tema escuro. Veja a seção 6 e `docs/references/`.
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
| `kernel/wm.c`, `gfx.c` | ideias do compositor. O renderizador será reescrito (alfa, blur, AA) |
| `root/*.pyos` (apps) | lógica dos apps reescrita em Python + `vitrum` (a sintaxe do PyOS já era parecida com Python) |
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
| Toolkit gráfico | **`opaline.vitrum`**: núcleo em C + API em Python | apps em Python com o visual do sistema |
| Apps rápidos | **mypyc / Cython** (Python tipado → extensão C) | ganho de ~3–10× sem criar um compilador próprio |
| TLS | **OpenSSL** portado (Apache-2.0) | HTTPS real, e o `pip` funciona |
| Linguagem do kernel | C (clang) + Assembly (NASM) | mesma toolchain do v2 |
| Ambiente de teste | **QEMU** (rápido, com depuração GDB) e **VirtualBox** | o v2 já usa VirtualBox |
| Onde roda | **VM no início**. Rodar em **PC real** e em **dual boot com Windows** é meta obrigatória (ver 3.3) | uso diário de verdade |
| Fonte da interface | **Inter** (licença OFL) | moderna, legível e pode ser distribuída |
| Nome do sistema | **OpalineOS** | escolhido pelo usuário após a pesquisa do Apêndice A.2 |
| Nomes dos módulos | pacote de distribuição **`opaline-vitrum`**; em Python: **`from opaline import vitrum`** (toolkit) e **`opaline.system`** (APIs do sistema). Documentação: [`opaline-vitrum.md`](opaline-vitrum.md) | nomes com hífen não podem ser importados em Python, e `vitrum` sozinho já existe no PyPI. Ficar dentro do pacote `opaline` evita qualquer conflito |
| Tipo de kernel | **monolítico modular** | drivers dentro do kernel, mas cada um isolado atrás de uma interface bem definida (podem ir para o espaço de usuário no futuro). Compositor e shell já rodam em espaço de usuário. É o modelo mais prático para os muitos drivers de PC real |
| ABI das syscalls | **mista** | as syscalls POSIX seguem o Linux x86-64: mesma numeração, mesmos registradores (`rax` = número; `rdi`, `rsi`, `rdx`, `r10`, `r8`, `r9` = argumentos), mesmas estruturas, constantes e `errno`. As syscalls exclusivas do sistema (compositor, IPC, notificações) ficam numa faixa própria, **a partir de 1000**. Syscall não implementada → `-ENOSYS`. Permite usar a musl quase sem mudanças e testar pacotes musllinux do PyPI. Nenhum código do Linux é usado, só o mesmo contrato |
| Licença do sistema | **GPL-3.0-or-later** (kernel, bootloader, compositor, shell, apps do sistema, ferramentas) | quem distribuir versões modificadas precisa publicar o código; "or later" permite usar versões futuras da GPL. Texto em `COPYING` |
| Licença das bibliotecas | **LGPL-3.0-or-later** para o que os apps importam: pacote `opaline` (`opaline.vitrum`, `opaline.system`, `opaline.i18n`) e o núcleo C `_vitrum` | apps para o OpalineOS podem ter qualquer licença. Texto em `COPYING.LESSER`; resumo em `LICENSING.md`. Componentes de terceiros mantêm as próprias licenças (seção 10) |
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
│ Shell gráfico (Python): barra superior, dock, lançador,      │
│ central de notificações                                      │
├─────────────────────────────────────────────────────────────┤
│ opaline.vitrum · opaline.system · opaline.i18n (Python)      │
│ _vitrum (núcleo C): componentes, layout, texto, animação     │
├─────────────────────────────────────────────────────────────┤
│ CPython oficial  ·  zlib, OpenSSL, libffi, sqlite...         │
├─────────────────────────────────────────────────────────────┤
│ musl libc  ·  carregador dinâmico (ld.so)                    │
├─────────────────────────────────────────────────────────────┤
│ Compositor (C, espaço de usuário): buffers compartilhados,   │
│ alfa, blur, sombras, animações                               │
├──────────────────── syscalls POSIX ─────────────────────────┤
│ Kernel (C): memória virtual, processos/threads, escalonador  │
│ SMP, VFS + EFS3, pipes, sockets, IPC, drivers (disco, rede,  │
│ entrada, vídeo, som)                                         │
├─────────────────────────────────────────────────────────────┤
│ Bootloader UEFI                                              │
└─────────────────────────────────────────────────────────────┘
```

---

## 4. Python como centro: CPython oficial + `opaline.vitrum`

### 4.1 Por que o Python oficial e não uma linguagem própria

- **Ecossistema real**: bibliotecas do PyPI, documentação, tutoriais e ferramentas existentes.
- **Menos trabalho reinventando**: o esforço vai para o que torna o OpalineOS único (kernel,
  compositor, visual e a API `opaline.vitrum`), e não para recriar uma linguagem e seu compilador.
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

### 4.4 A biblioteca `opaline.vitrum`

- **`_vitrum` (C)**: fala com o compositor por memória compartilhada e IPC, desenha os componentes
  do design system (seção 6) com blur, sombras, texto TrueType e animações, e trata a entrada.
- **`opaline.vitrum` (Python)**: uma API simples e "pythônica" por cima, **em inglês**.
- **`opaline.system` (Python)**: APIs do sistema (notificações, energia, rede, configurações, tema,
  arquivos recentes).
- **`opaline.i18n` (Python)**: traduções, idioma atual e formatação de datas/números (seção 7).
- Documentação completa: [`opaline-vitrum.md`](opaline-vitrum.md).

Exemplo do estilo de API desejado:

```python
from opaline import vitrum as vt

from opaline.i18n import _

app = vt.App(_("Counter"))
window = vt.Window(_("Counter"), width=360, height=200)

total = vt.Label("0", style="h1")
def increment():
    total.text = str(int(total.text) + 1)

window.add(vt.Column([
    total,
    vt.Button(_("Add"), primary=True, on_click=increment),
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
| Vidro fosco (blur do fundo) | blur gaussiano aproximado (3 passes de box blur) em resolução reduzida (1/4) e depois ampliado | o blur do papel de parede é **cacheado**. Só é refeito quando o fundo muda |
| Cantos arredondados | máscaras de SDF (campo de distância) com antialiasing | barato |
| Sombras suaves | sombra pré-desfocada por raio, guardada em cache (9 fatias: cantos e bordas) | quase grátis depois do cache |
| Brilho (glow) coral | é uma sombra colorida com blur, igual à anterior | idem |
| Texto nítido | rasterizador TrueType (próprio ou FreeType portado) com antialiasing e cache de glifos | cache por tamanho |
| Ícones | ícones vetoriais próprios rasterizados com AA e cacheados | — |
| Animações | interpolação com *easing* a 60 fps, só na região que mudou (*damage tracking*) | redesenhar só o necessário |
| Imagens | libpng/libjpeg portadas (ou decodificadores próprios) | — |

- Compositor em **espaço de usuário**. Cada janela desenha no próprio buffer (memória
  compartilhada) e o compositor só monta a cena. Assim, uma janela travada não congela a tela.
- Metas de desempenho: 60 fps arrastando janelas em 1920×1080 na VM e menos de 16 ms por quadro.
- Mais adiante: driver *virtio-gpu* (QEMU) e, só muito depois, aceleração de GPU real.

---

## 6. Design system "Vitrum"

Referências em `docs/references/`:
- `desktop.webp`: área de trabalho completa (barra superior, dock lateral e inferior, menu de apps,
  gerenciador de arquivos, monitor do sistema, player e central de notificações).
- `componentes.webp`: kit de componentes (botões, inputs, toggle, abas, chips, dropdown, date picker,
  stepper, toast, tooltip, badges, paginação, breadcrumbs, card, tabela, estado vazio).

> Os ícones das referências incluem marcas de terceiros (Firefox, Spotify, Discord, Steam, VS Code,
> Chrome). O OpalineOS usa **somente ícones próprios**, nunca logotipos de outras marcas.

### 6.1 Tokens (valores iniciais, estimados das referências e ajustáveis)

Os nomes dos tokens são em inglês, como toda a API.

**Cores (tema escuro)**

| Token | Valor | Uso |
|---|---|---|
| `background.base` | `#0E1424` | atrás de tudo (quando não há papel de parede) |
| `surface.glass` | `#1C2433` com 72% de opacidade + blur 24 px | janelas, painéis, dock |
| `surface.raised` | `#26303F` com 85% | menus, dropdowns, tooltips |
| `surface.field` | `#2A3B40` | inputs, botões secundários (tom petróleo da ref. 2) |
| `border.subtle` | `#FFFFFF` com 10% | contorno de janelas e cards |
| `accent` | `#F28B82` (coral) | botão primário, item ativo, sliders, toggle ligado |
| `accent.strong` | `#FF6F7D` (rosa) | hover/pressionado, botão play, badge "novo" |
| `accent.glow` | `#F28B82` com 45%, blur 16 px | brilho no hover/foco |
| `text.primary` | `#F2F4F8` | títulos e texto principal |
| `text.secondary` | `#A9B3C4` | legendas, metadados |
| `text.disabled` | `#6B7586` | estados desabilitados |
| `success` / `warning` / `error` / `info` | `#5BD69A` / `#F5C451` / `#F0606A` / `#5AA9F7` | badges, toasts, gráficos |
| `chart.cpu` / `chart.memory` / `chart.disk` | `#F27A8A` / `#4FC3D9` / `#F2A65A` | séries do monitor do sistema |

**Forma e espaço**

| Token | Valor |
|---|---|
| Raio (`radius.*`) | `small 6` · `medium 10` · `large 14` · `window 16` · `pill 999` (px) |
| Espaçamento (grade de 4 px) | 4 · 8 · 12 · 16 · 24 · 32 · 48 |
| Borda | 1 px (padrão) · 2 px (foco) |
| Sombra | `s1` 0/2/6 · `s2` 0/8/24 · `s3` 0/16/48 (preto 35–50%) |
| Tipografia (`font.*`) | Inter. `h1` 28/600 · `h2` 20/600 · `body` 14/400 · `caption` 12/400 |
| Movimento (`motion.*`) | `fast` 120 ms (hover) · `open` 200 ms · `close` 160 ms · `window` 320 ms (minimizar/maximizar). Easing `cubic-bezier(.2,.8,.2,1)` |

### 6.2 Componentes do toolkit (prioridade)

1. Janela (barra de título com minimizar, maximizar e fechar), botão (primário, secundário,
   desabilitado, carregando), input (texto, senha, busca), rótulo, ícone
2. Toggle, abas, chips, dropdown, slider, barra de progresso/stepper, tooltip, badge
3. Lista, grade de ícones, tabela, card, breadcrumbs, paginação, estado vazio, toast/notificação
4. Date picker, gráficos (linha, anel), menu de contexto

### 6.3 Shell (área de trabalho)

Barra superior (menus Applications, Places e System — "Aplicativos", "Locais" e "Sistema" em pt-BR — mais indicadores de rede, som, bateria e
relógio), dock lateral, dock inferior, lançador de apps com busca e categorias, central de
notificações com atalhos rápidos (Wi-Fi, Bluetooth, modo avião, modo noturno e brilho) e papel de
parede espacial próprio.

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
- API para apps: `from opaline.i18n import _` (detalhes em `opaline-vitrum.md`, seção "Internationalization"):
  - `_("Save")` devolve um **texto traduzível "vivo"**: os componentes do `opaline.vitrum` guardam a
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
| **E3** | Processos, threads e escalonador preemptivo (APIC timer) | duas threads de kernel alternando |
| **E4** | Ring 3, syscalls e carregador ELF64 estático | programa em C de usuário escreve via syscall; um app com erro não derruba o kernel |
| **E5** | VFS, EFS3 (nomes de arquivo em UTF-8), disco (virtio-blk ou AHCI), pipes, terminal (TTY) | ler e gravar arquivos do espaço de usuário, inclusive com nomes acentuados |
| **E6** | **Camada POSIX + musl** portada + carregador dinâmico (`ld.so`) | programas de teste em C (arquivos, `fork`/`exec`, threads, `mmap`) passam |
| **E7** | **CPython** portado (estático → dinâmico) + zlib, libffi, sqlite | REPL do Python no console; subconjunto da suíte de testes do CPython passa |
| **E8** | Compositor + renderizador (alfa, blur, sombras, TrueType com fontes de reserva) + entrada (mouse; teclado com layouts US, US-International e ABNT2, teclas mortas) | janela de vidro arrastável a 60 fps; digitar "ação, über, ñ" funciona |
| **E9** | **`opaline.vitrum`** (núcleo C + API Python) + `opaline.system` + **`opaline.i18n`** + shell (barra, dock, lançador) | o app Counter da seção 4.4 roda; a área de trabalho segue o design; trocar en-US ↔ pt-BR atualiza a interface sem reiniciar |
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
├── opaline-vitrum.md     # documentação do toolkit
├── docs/references/      # imagens de referência visual
├── boot/                 # bootloader UEFI
├── kernel/               # kernel (C + ASM)
│   ├── arch/x86_64/
│   ├── mm/  sched/  fs/  net/  posix/  drivers/
├── system/
│   ├── compositor/       # compositor (C)
│   ├── vitrum/           # _vitrum (C) + vitrum (Python)
│   ├── opaline/          # pacote Python `opaline` (system/, i18n/; vitrum/ é instalado aqui)
│   └── shell/            # área de trabalho (Python)
├── apps/                 # apps em Python (pasta + app.toml + locale/)
├── locale/               # catálogos de tradução do sistema (en-US = textos do código, pt_BR.po)
├── ports/                # receitas de build: musl, cpython, zlib, openssl, libffi, numpy...
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
> Atenção: o nome `opaline` também está reservado no PyPI por terceiros. Por isso o sistema guarda o
> pacote `opaline` num diretório protegido do sistema, que vem antes do `site-packages`, e o `pip`
> do OpalineOS recusa instalar pacotes que sobrescrevam `opaline`.
