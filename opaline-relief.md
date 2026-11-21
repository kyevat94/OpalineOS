# opaline-relief — Documentation

> **Status: specification v0.3 (planned API).** None of this is implemented yet. Implementation
> happens in stage **E9** of the roadmap (`project.md`, section 8). The examples show how the API
> **will** work. Once implementation starts, this document becomes the official reference and every
> API change is recorded here.
>
> v0.3 (2026-11-21): the look changes from glass (glassmorphism) to **soft relief** (neumorphism),
> and the toolkit is renamed from `opaline-vitrum` to `opaline-relief`.
>
> License: **LGPL-3.0-or-later**. Apps that use the library may have any license (see `LICENSING.md`).

`opaline-relief` is the GUI toolkit of **OpalineOS**. With it you build Python apps with windows,
components and the system's **soft relief** look: opaque surfaces that rise from or sink into the
background, shaped by light and shadow (neumorphism). You control **everything that moves, rises
or glows on screen**:

- animation of any visual property (position, size, scale, rotation, opacity, color, elevation,
  shadow, glow...);
- the **open, close, minimize, restore, maximize, fullscreen, focus and unfocus** effects of windows;
- **relief and transparency**: elevation (raised, flat, inset, pressed), depth and light, window
  opacity and, as an optional effect, frosted glass, per window and per component;
- motion curves, springs, keyframes, sequences and implicit transitions;
- **internationalization**: translatable text that updates live when the user changes the system
  language, and full Unicode support.

```python
from opaline import relief as rl
```

| Package (distribution) | Import | Contents |
|---|---|---|
| `opaline-relief` | `from opaline import relief` | GUI toolkit (this document) |
| (system) | `from opaline import system` | system APIs: notifications, power, network, settings, system-wide effects |
| (system) | `from opaline import i18n` | translations, current language, regional formatting (section 17) |

---

## Contents

1. [How it works under the hood](#1-how-it-works-under-the-hood)
2. [Getting started](#2-getting-started)
3. [Basic types](#3-basic-types)
4. [Visual properties](#4-visual-properties)
5. [Theme and tokens](#5-theme-and-tokens)
6. [Styles and states](#6-styles-and-states)
7. [Animation](#7-animation)
8. [Windows](#8-windows)
9. [Window effects](#9-window-effects)
10. [Surfaces, relief, shadows and transparency](#10-surfaces-relief-shadows-and-transparency)
11. [Layout](#11-layout)
12. [Components](#12-components)
13. [Events and input](#13-events-and-input)
14. [Custom drawing](#14-custom-drawing)
15. [Async and threads](#15-async-and-threads)
16. [Accessibility and user preferences](#16-accessibility-and-user-preferences)
17. [Internationalization and Unicode](#17-internationalization-and-unicode)
18. [Performance and debugging](#18-performance-and-debugging)
19. [Complete examples](#19-complete-examples)
20. [Quick reference](#20-quick-reference)
21. [Conventions](#21-conventions)
22. [Open design questions](#22-open-design-questions)

---

## 1. How it works under the hood

```
 Your app (Python)
     │  creates components, describes animations and effects
     ▼
 opaline.relief (Python)  ── public API, documented here
     │
     ▼
 _relief (C)  ── draws components into the window buffer (text, surfaces, relief)
     │  shared memory + command queue
     ▼
 Compositor (C)  ── composes windows on screen and RUNS animations at 60 fps
```

The rule that keeps everything smooth: **Python describes, the compositor executes.**

- When you call `rl.animate(...)`, Python sends the compositor a *description* of the animation
  (target, properties, duration and curve). The compositor computes every frame in C, so the
  animation stays smooth even while Python is busy, waiting on the network or collecting garbage.
- **Transform** properties (`scale`, `rotation`, `offset`, `opacity`) are applied by the
  compositor without redrawing content, which makes them the cheapest to animate. Changing
  `elevation` is almost as cheap: the relief shadows are cached and simply cross-faded.
- Properties that change **content** (`width`, `background`, `radius`, text...) require the
  component to be redrawn. `_relief` does this in C, only for the affected region.
- **Window effects** (section 9) are Python functions called when the event happens. They return an
  animation description that the compositor runs. If the app does not answer within **50 ms**
  (frozen or crashed), the compositor uses the system default effect.

---

## 2. Getting started

### Hello, world

```python
from opaline import relief as rl
from opaline.i18n import _

app = rl.App(_("Hello"))
window = rl.Window(_("Hello"), width=320, height=160)
window.add(rl.Label(_("Hello, OpalineOS!"), style="h1"))
window.show()
app.run()
```

### Counter

```python
from opaline import relief as rl
from opaline.i18n import _

app = rl.App(_("Counter"))
window = rl.Window(_("Counter"), width=360, height=200)

total = rl.Label("0", style="h1")

def increment():
    total.text = str(int(total.text) + 1)
    # a small "pop" on every click
    rl.animate(total, 220, keyframes=rl.Keyframes({0: dict(scale=1), 0.4: dict(scale=1.15), 1: dict(scale=1)}))

window.add(rl.Column([
    total,
    rl.Button(_("Add"), primary=True, on_click=increment),
], spacing=16, align="center"))

window.show()
app.run()
```

### Layout of an installed app

```
/apps/counter/
├── app.toml        # name, icon, category, permissions, translated names
├── icon.svg
├── main.py         # entry point
└── locale/
    └── pt_BR.po    # Portuguese translation of the app's texts
```

```toml
[app]
name = "Counter"
name.pt-BR = "Contador"
id = "org.example.counter"
icon = "icon.svg"
category = "Utilities"
entry = "main.py"
permissions = []          # e.g. ["network", "files.documents", "system.effects"]
```

---

## 3. Basic types

| Type | How to write it | Notes |
|---|---|---|
| **Duration** | `int` in **milliseconds** | `duration=200` |
| **Color** | `"#1E5D59"`, `"#1E5D59B3"` (with alpha), `rl.Color(30, 93, 89, a=0.7)`, `rl.theme.color("accent")` | methods: `.with_alpha(0.5)`, `.lighten(0.1)`, `.darken(0.1)`, `.mix(other, 0.3)` |
| **Point** | `(x, y)` or `rl.Point(x, y)` | logical pixels |
| **Size** | `(width, height)` or `rl.Size(w, h)` | |
| **Rect** | `rl.Rect(x, y, width, height)` | `.center`, `.center_x`, `.expand(n)`, `.contains(p)` |
| **Radius** | `12` or `rl.Radius(top_left, top_right, bottom_right, bottom_left)` | rounded corners |
| **Insets** | `16`, `(16, 8)` (horizontal, vertical) or `rl.Insets(top, right, bottom, left)` | `padding` and `margin` |
| **Angle** | `float` in **degrees** | `rotation=15` |
| **Origin** | `(0.5, 0.5)` = center, `(0, 0)` = top-left corner | point around which scale and rotation happen |
| **Text** | any `str` (full Unicode) or a translatable text from `_()` | see section 17 |

Pixels are **logical**: on high-density screens the system multiplies by the screen scale
(`rl.screen.scale`), so sizes look the same on any monitor.

---

## 4. Visual properties

Every component, and every window, has these properties. **All properties marked animatable can be
used in `rl.animate`, keyframes, transitions and effects.**

| Property | Type | Animatable | Cost | Description |
|---|---|---|---|---|
| `x`, `y`, `position` | int / Point | ✅ | medium | position in the layout. For windows: position on screen |
| `width`, `height`, `size` | int / Size | ✅ | high | changes the layout and redraws |
| `rect` | Rect | ✅ | — | shortcut for position + size. In window effects it is applied as a visual transform (cheap) |
| `offset` | Point | ✅ | **low** | moves without affecting layout (like CSS `translate`) |
| `scale`, `scale_x`, `scale_y` | float | ✅ | **low** | 1.0 = normal |
| `rotation` | degrees | ✅ | **low** | rotates around `origin` |
| `origin` | (fx, fy) | ✅ | low | reference point for scale and rotation. Default `(0.5, 0.5)` |
| `opacity` | 0.0–1.0 | ✅ | **low** | transparency of the whole component (content included) |
| `elevation` | int −2…5 | ✅ | **low** | place in the relief: pressed, inset, base, flat, hover, raised, modal, popover (section 10.1) |
| `surface` | rl.Surface | ✅ | medium | background: relief (default), solid, transparent or frosted (section 10.2) |
| `background` | Color / Gradient | ✅ | medium | |
| `text_color` | Color | ✅ | medium | |
| `radius` | int / Radius | ✅ | medium | rounded corners |
| `border` | rl.Border | ✅ | medium | `rl.Border(1, "#00000014")` |
| `shadow` | rl.Shadow / list | ✅ | low* | *shadows are cached by size and radius |
| `glow` | rl.Glow | ✅ | low* | colored glow: focus rings, active controls |
| `blur` | px | ✅ | medium | blurs the component itself |
| `backdrop_blur` | px | ✅ | high | blurs what is **behind** it (frosted glass; an optional effect, section 10.3) |
| `saturation` | float | ✅ | low | 0 = grayscale, 1 = normal |
| `brightness` | float | ✅ | low | 1 = normal |
| `warp` | rl.Warp | ✅ | high | mesh deformation (e.g. the "genie" effect). **Phase 2** |
| `clip` | bool | — | — | clips children to the rounded corners |
| `visible` | bool | — | — | animate `opacity` to fade out smoothly |
| `cursor` | str | — | — | `"arrow"`, `"hand"`, `"text"`, `"move"`... |

---

## 5. Theme and tokens

The theme holds the system's colors, radii, shadows, typography and **motion** (values in
`project.md`, section 6.1). Use tokens instead of hard-coded values so your app follows the user's
theme.

```python
rl.theme.color("accent")          # Color("#1E5D59"), deep teal
rl.theme.color("surface.base")    # Color("#EEE6DA"), warm porcelain
rl.theme.radius("window")         # 16
rl.theme.elevation(3)             # the highlight and shadow of "raised" (section 10.1)
rl.theme.light                    # (-1, -1): the light comes from the top left
rl.theme.spacing(4)               # 4th step of the 4 px grid -> 16
rl.theme.font("h1")               # rl.Font("Inter", 28, weight=600)
rl.theme.motion("open")           # rl.Transition(200, rl.curves.smooth)
```

### Color tokens

`background.base`, `surface.base`, `surface.inset`, `relief.light` (highlight), `relief.dark`
(shadow), `border.subtle`, `accent`, `accent.strong`, `accent.on` (text on the accent), `focus`,
`text.primary`, `text.secondary`, `text.disabled`, `success`, `warning`, `error`, `info`,
`chart.cpu`, `chart.memory`, `chart.disk`.

### Shape tokens

Radii `radius.xs` 4 · `sm` 8 · `md` 12 · `lg` 16 · `xl` 24 · `window` 16 · `pill`; borders 1, 2 and
4 px; elevations −2 to 5 (section 10.1); icons 16, 20 and 24 px with a 2 px stroke.

### Motion tokens

| Token | Default | Used for |
|---|---|---|
| `motion.fast` | 120 ms, `smooth` | hover, press |
| `motion.open` | 200 ms, `smooth` | menus, popovers, opening windows |
| `motion.close` | 160 ms, `ease_in` | closing |
| `motion.window` | 320 ms, `smooth` | minimize, maximize, restore |
| `motion.spring` | `spring(stiffness=380, damping=28)` | "physical" elements |

### Custom theme

```python
my_theme = rl.Theme(
    base="light",                                 # starts from a system theme ("light" or "dark")
    colors={"accent": "#7FD1B9", "accent.strong": "#5BC0A0"},
    radii={"window": 20},
    motion={"open": rl.Transition(260, rl.curves.spring(stiffness=300, damping=22))},
)
app.theme = my_theme          # this app only
# window.theme = my_theme     # or this window only

rl.theme.on_change(lambda theme: print("the user changed the theme"))
```

---

## 6. Styles and states

A `rl.Style` groups visual properties. Components have **states**, and each state can have its own
style. Switching between states is animated by `transition`.

States: `normal`, `hover`, `pressed`, `focused`, `disabled`, `selected`, `active`.

```python
button = rl.Button(
    _("Send"),
    style=rl.Style(background=rl.theme.color("accent"), radius=12, elevation=3),
    states={
        "hover":    rl.Style(elevation=2, brightness=1.05),
        "pressed":  rl.Style(elevation=-2),                     # sinks into the surface
        "focused":  rl.Style(glow=rl.Glow(rl.theme.color("focus"), radius=10, intensity=0.5)),
        "disabled": rl.Style(elevation=0, opacity=0.55, saturation=0),
    },
    transition=rl.Transition(120, rl.curves.smooth),
)
```

### Reusable style classes

```python
rl.styles.register("danger-button", rl.Style(background=rl.theme.color("error")),
                   states={"hover": rl.Style(glow=rl.Glow(rl.theme.color("error"), 14))})

rl.Button(_("Delete"), style_class="danger-button")
```

---

## 7. Animation

### 7.1 `rl.Anim` and `rl.animate`

- `rl.Anim(...)` **describes** an animation without starting it. Use it to build sequences and
  effects.
- `rl.animate(...)` creates **and starts** the animation (same as `rl.Anim(...).start()`).

```python
rl.Anim(target, duration=None, curve=None, *, from_=None, keyframes=None, delay=0, repeat=1,
        alternate=False, **properties)
```

| Parameter | Description |
|---|---|
| `target` | component, window or layer |
| `duration` | ms. Optional with `curves.spring` (the spring computes its own duration) |
| `curve` | motion curve (7.2). Default: `rl.theme.motion("open").curve` |
| `from_` | `dict` with start values. Without it, the animation starts from the current value |
| `keyframes` | `rl.Keyframes(...)`: keyframes instead of end values |
| `delay` | ms before starting |
| `repeat` | number of times, or `rl.FOREVER` |
| `alternate` | plays forward and backward on each repetition |
| `**properties` | end values: `opacity=1, scale=1.1, x=rl.by(40)` |

```python
card = rl.Card(...)

# appear while moving up
rl.animate(card, 240, rl.curves.smooth,
           from_=dict(opacity=0, offset=(0, 16)),
           opacity=1, offset=(0, 0))

# relative values
rl.animate(card, 200, x=rl.by(40), rotation=rl.by(-5))
```

### Controlling a running animation

```python
a = rl.animate(card, 1000, scale=1.5)

a.pause()
a.resume()
a.reverse()                  # goes back along the same path
a.progress = 0.5             # jump to the middle (0.0 to 1.0)
a.speed = 0.25               # slow motion
a.cancel(jump_to_end=False)

a.on_finish(lambda: print("done"))
await a                      # inside a coroutine: wait for it to finish
a.running                    # bool
```

If a new animation touches a property that is already being animated, the old one **stops for
that property** and the new one starts from the current value. No jumps.

### 7.2 Motion curves

| Curve | Use |
|---|---|
| `rl.curves.linear` | progress bars, continuous rotation |
| `rl.curves.smooth` | system default: `bezier(.2, .8, .2, 1)` |
| `rl.curves.ease_in` | starts slow and accelerates: elements **leaving** the screen |
| `rl.curves.ease_out` | starts fast and decelerates: elements **entering** |
| `rl.curves.ease_in_out` | slow → fast → slow |
| `rl.curves.bezier(x1, y1, x2, y2)` | same as CSS `cubic-bezier` |
| `rl.curves.spring(stiffness=380, damping=28, mass=1, initial_velocity=0)` | physical motion. No `duration` needed |
| `rl.curves.bounce(bounces=3)` | bouncing ball |
| `rl.curves.elastic(amplitude=1, period=0.3)` | overshoots and settles |
| `rl.curves.steps(n)` | animates in `n` jumps (clocks, sprites) |
| Python function `f(t) -> float` | custom curve. The system samples it at 256 points and sends the table to the compositor |

```python
import math

def breathe(t):
    return 0.5 - 0.5 * math.cos(t * math.pi)

rl.animate(icon, 1600, curve=breathe, scale=1.08, repeat=rl.FOREVER, alternate=True)
```

### 7.3 Keyframes

```python
pop = rl.Keyframes({
    0.0: dict(scale=1.0,  offset=(0, 0)),
    0.3: dict(scale=1.12, offset=(0, -10)),
    0.6: dict(scale=0.96, offset=(0, 0)),
    1.0: dict(scale=1.0),
}, curves={0.3: rl.curves.ease_out, 0.6: rl.curves.ease_in})   # per-segment curves (optional)

rl.animate(icon, 420, keyframes=pop)
```

### 7.4 Sequences, groups and staggering

```python
intro = rl.sequence(
    rl.Anim(title,  200, from_=dict(opacity=0), opacity=1),
    rl.parallel(
        rl.Anim(image, 300, from_=dict(scale=0.9), scale=1),
        rl.Anim(body,  300, from_=dict(opacity=0, offset=(0, 12)),
                opacity=1, offset=(0, 0)),
    ),
    rl.wait(100),
    rl.Anim(button, curve=rl.curves.spring(), from_=dict(scale=0.8), scale=1),
)
intro.start()

# the same effect on many items, one after the other
rl.stagger(list_view.items, interval=40,
           make=lambda item: rl.Anim(item, 220, from_=dict(opacity=0, offset=(-12, 0)),
                                     opacity=1, offset=(0, 0))).start()
```

`rl.sequence`, `rl.parallel` and `rl.stagger` return an `Anim`, so they also accept `delay`,
`repeat` and `alternate`.

### 7.5 Implicit transitions

Instead of calling `animate`, declare **how** a property changes. From then on, every assignment
is animated.

```python
panel.transitions = {
    "opacity":  rl.Transition(180),
    "position": rl.Transition(curve=rl.curves.spring(stiffness=300, damping=26)),
    "*":        rl.Transition(150),          # any other property
}

panel.opacity = 0.5      # animates over 180 ms
panel.x = 300            # animates with a spring

# animate a block of changes at once, without declaring transitions
with rl.transition(250, rl.curves.smooth):
    panel.width = 400
    bar.opacity = 0

# change without animating
with rl.no_animation():
    panel.x = 0
```

### 7.6 Enter/exit animations and layout animation

```python
item = rl.Card(_("New item"),
               enter=rl.effects.slide("down", 220),   # when added
               exit=rl.effects.fade(160))              # when removed (waits for it to finish)

column = rl.Column(spacing=8, animate_layout=rl.Transition(220, rl.curves.smooth))
column.add(item)      # slides in, and its neighbours move aside smoothly
column.remove(item)   # fades out, and the gap closes smoothly
```

### 7.7 Frame-by-frame animation (in Python)

For physics, games or generative effects. **Runs Python every frame**, so it is more expensive:
prefer the options above whenever possible.

```python
@app.every_frame
def spin(dt, time):              # dt and time in ms
    star.rotation = (time / 10) % 360

spin.stop()                      # stops calling it
```

---

## 8. Windows

### 8.1 Creating a window

```python
window = rl.Window(
    _("Editor"),
    width=900, height=600,
    x=None, y=None,                 # None = the system places it (centered/cascaded)
    icon="icon.svg",
    resizable=True,
    min_size=(400, 300),
    surface=rl.Surface.relief(),    # system default
    radius=rl.theme.radius("window"),
    elevation=4,                    # windows float above the desktop (section 10.1)
    decoration="system",            # "system" = standard title bar; "none" = you draw it
    always_on_top=False,
    show_in_taskbar=True,
)
window.show()
```

### 8.2 Methods

| Method | Effect used (section 9) |
|---|---|
| `show()` | `open` |
| `close()` | `close` (can be cancelled by `on_close`) |
| `hide()` | `close`, but the window keeps existing |
| `minimize()` / `restore()` | `minimize` / `restore` |
| `maximize()` / `unmaximize()` | `maximize` / `unmaximize` |
| `fullscreen(True/False)` | `fullscreen` / `exit_fullscreen` |
| `focus()` | `focus_in` (and `focus_out` on the window that lost focus) |
| `alert()` | `alert` (get the user's attention) |
| `center(animate=True)` | — |
| `move_to(x, y, animate=True)` | uses `motion.window` |
| `resize(w, h, animate=True)` | uses `motion.window` |

All of them accept `effect=` to use a different effect **for that call only**:

```python
window.close(effect=rl.effects.fade(80))
window.minimize(effect=rl.effects.none)
```

### 8.3 State and properties

```python
window.state             # rl.WindowState.NORMAL | MAXIMIZED | MINIMIZED | FULLSCREEN
window.focused           # bool
window.title = _("Untitled — Editor")
window.opacity = 0.9     # whole window, content included (animatable)
window.surface = rl.Surface.relief(depth=0.7)   # background only (animatable)
window.screen            # monitor it is on: .rect, .work_area, .scale
```

### 8.4 Events

```python
@window.on_close
def confirm():
    if document.modified:
        answer = rl.dialog.ask(_("Save changes?"), [_("Save"), _("Discard"), _("Cancel")])
        return answer != 2       # index of the chosen button; False cancels closing
    return True

window.on_focus(lambda: ...)
window.on_blur(lambda: ...)
window.on_move(lambda pos: ...)
window.on_resize(lambda size: ...)
window.on_state_change(lambda old, new: ...)
```

### 8.5 Inactive windows (focus)

The simple way: define a style for when the window is not focused. The change is animated.

```python
window.inactive_style = rl.Style(opacity=0.92, saturation=0.75, elevation=3)   # sinks a little below the active one
window.focus_transition = rl.Transition(180)
```

For full control, use the `focus_in`/`focus_out` effects (section 9).

### 8.6 Custom title bar

```python
window = rl.Window(_("Player"), width=420, height=240, decoration="none")

title_bar = rl.DragArea(               # dragging here moves the window; double-click maximizes
    rl.Row([
        rl.Label(_("Now playing"), style="caption"),
        rl.Spacer(),
        rl.WindowButton("minimize"),
        rl.WindowButton("maximize"),
        rl.WindowButton("close"),
    ], align="center"),
    height=36,
)
window.add(rl.Column([title_bar, content]))
```

---

## 9. Window effects

This is the part that gives **full control** over how windows move. Every moment in a window's life
has an **effect**, and an effect is a Python function:

```python
def my_effect(ctx: rl.EffectContext) -> rl.Anim | None:
    ...
```

- Return an `Anim`: the compositor runs it.
- Return `None`: no animation (instant change).

### 9.1 Available moments

| Moment | When it happens | When it ends |
|---|---|---|
| `open` | `show()` | the window stays in the animation's final state |
| `close` | `close()` / `hide()` | the window is destroyed/hidden **after** the animation |
| `minimize` | minimize button, `minimize()` | the window disappears (into its button in the top bar) |
| `restore` | a click on its button in the top bar, `restore()` | — |
| `maximize` / `unmaximize` | button, title bar double-click, `maximize()` | content is laid out again at the final size |
| `fullscreen` / `exit_fullscreen` | `fullscreen(...)` | same |
| `focus_in` / `focus_out` | the active window changes | — |
| `drag_start` / `drag_end` | the user starts/stops dragging the window | — |
| `alert` | `alert()` | — |

### 9.2 The context (`rl.EffectContext`)

| Field | Description |
|---|---|
| `ctx.window` | the window **surface** in the compositor. It is the animation target (all properties from section 4) |
| `ctx.moment` | `"open"`, `"close"`, `"minimize"`... |
| `ctx.origin` | current `Rect` of the window |
| `ctx.target` | final `Rect` (e.g. the screen's work area when maximizing) |
| `ctx.task_button` | `Rect` of the window's button in the top bar, or `None` |
| `ctx.cursor` | mouse position |
| `ctx.screen` | the monitor (`.rect`, `.work_area`) |
| `ctx.reason` | `"user"`, `"app"` or `"system"` |
| `ctx.reduce_motion` | `True` if the user asked for less motion (section 16) |
| `ctx.content` | while resizing: `"scale"` (default, fast: stretches the image and re-lays out at the end) or `"relayout"` (lays out every frame, more expensive) |

### 9.3 Where to set effects: window, app or system

```python
effects = rl.Effects(open=..., close=..., minimize=..., restore=...,
                     maximize=..., unmaximize=..., focus_in=..., focus_out=...)

window.effects = effects         # this window only
app.effects = effects            # every window of the app
```

**Priority:** effect passed in the call (`close(effect=...)`) → window → app → system.
Moments you do not define keep the effect from the level above.

**System effects** (used by every app that does not define its own) are configured by the Settings
app, or by any app with the `system.effects` permission:

```python
from opaline import system
system.effects.set(minimize=rl.effects.genie(), close=rl.effects.zoom(0.94, 160))
```

### 9.4 Built-in effects (`rl.effects`)

Each one returns an effect function. Use them directly or inside your own effects.

| Effect | Description |
|---|---|
| `rl.effects.none` | no animation |
| `rl.effects.fade(duration=160)` | opacity only |
| `rl.effects.zoom(scale=0.94, duration=200)` | scale + opacity |
| `rl.effects.slide(direction="down", duration=220, distance=24)` | slide + opacity |
| `rl.effects.spring(stiffness=380, damping=26)` | opens with a spring |
| `rl.effects.to_bar(duration=340)` | shrinks into its button in the top bar (minimize/restore) |
| `rl.effects.genie(duration=420)` | "genie" deformation into its button in the top bar. **Phase 2** (uses `warp`) |
| `rl.effects.shake(intensity=8)` | for `alert` |
| `rl.effects.default` | the system effect for that moment |

### 9.5 Compositor rules

- The effect function must answer within **50 ms**. Otherwise the compositor uses the system effect.
- If the app freezes or crashes, its windows close with the system effect.
- During `close`, the window no longer receives input. During `minimize`, `maximize` and `open`,
  input comes back when the animation ends.
- An effect can be interrupted. For example, restoring a window that is still minimizing: the new
  animation starts from the current point.
- Maximum effect duration: **2 s** (protection against apps that "hold" the screen).

### 9.6 Example: a complete set of effects

```python
from opaline import relief as rl

SPRING = rl.curves.spring(stiffness=380, damping=28)

def open_(ctx):
    if ctx.reduce_motion:
        return rl.Anim(ctx.window, 120, from_=dict(opacity=0), opacity=1)
    return rl.Anim(ctx.window, curve=SPRING,
                   from_=dict(opacity=0, scale=0.92, offset=(0, 20)),
                   opacity=1, scale=1, offset=(0, 0))

def close(ctx):
    return rl.Anim(ctx.window, 160, rl.curves.ease_in, scale=0.94, opacity=0, elevation=1)

def minimize(ctx):
    target = ctx.task_button or rl.Rect(ctx.origin.center_x, 0, 1, 1)
    return rl.Anim(ctx.window, 320, rl.curves.ease_in, rect=target, opacity=0.2)

def restore(ctx):
    return rl.Anim(ctx.window, curve=SPRING,
                   from_=dict(rect=ctx.task_button or ctx.target, opacity=0.2),
                   rect=ctx.target, opacity=1)

def maximize(ctx):
    ctx.content = "scale"
    return rl.Anim(ctx.window, 260, rl.curves.smooth, rect=ctx.target, radius=0)

def unmaximize(ctx):
    return rl.Anim(ctx.window, 260, rl.curves.smooth,
                   rect=ctx.target, radius=rl.theme.radius("window"))

def focus_in(ctx):
    return rl.Anim(ctx.window, 140, opacity=1, saturation=1, elevation=4)

def focus_out(ctx):
    return rl.Anim(ctx.window, 220, opacity=0.96, saturation=0.8, elevation=3)

def drag_start(ctx):
    return rl.Anim(ctx.window, 120, opacity=0.85, scale=1.01)

def drag_end(ctx):
    return rl.Anim(ctx.window, 160, opacity=1, scale=1)

app = rl.App("Effects")
app.effects = rl.Effects(
    open=open_, close=close,
    minimize=minimize, restore=restore,
    maximize=maximize, unmaximize=unmaximize,
    focus_in=focus_in, focus_out=focus_out,
    drag_start=drag_start, drag_end=drag_end,
    alert=rl.effects.shake(6),
)

rl.Window("Demo", width=600, height=400).show()
app.run()
```

---

## 10. Surfaces, relief, shadows and transparency

The OpalineOS look is **soft relief** (neumorphism): surfaces are opaque and share the color of what
is under them, and depth comes from light. One light (top left by default) gives every raised surface
a **highlight** on the lit side and a **shadow** on the other; a sunken surface has both on the
inside. Nothing behind a window has to be read back or blurred, so the look is cheap to draw on the
CPU (section 10.5).

### 10.1 Elevation

`elevation` places a surface in the relief. The values come from the theme (`project.md`,
section 6.1):

| Value | Name | Looks | Typical use |
|---|---|---|---|
| `-2` | pressed | deeply sunken | a button while pressed, a switch's track when on |
| `-1` | inset | sunken | text fields, slider tracks, wells, list backgrounds |
| `0` | base | flat with what is under it | window backgrounds, separators |
| `1` | flat | barely lifted | list items, table headers |
| `2` | hover | lifted a little | whatever is under the pointer |
| `3` | raised | clearly raised | cards, buttons, the top bar |
| `4` | modal | floating | dialogs, windows |
| `5` | popover | highest | menus, tooltips, dropdowns, toasts |

```python
card = rl.Card(title, elevation=3)
field = rl.TextField(placeholder=_("Email address"))       # elevation -1 by default
rl.animate(card, 160, elevation=2)                          # sinks a little: the shadows cross-fade
```

Elevation is **animatable**, even across 0 (raised → pressed): the compositor cross-fades the cached
outer and inner shadows, so pressing a button costs almost nothing.

### 10.2 Surfaces

A surface is the background of a window or component. The default, `rl.Surface.relief()`, follows
`elevation` and the theme. Every parameter is optional:

```python
rl.Surface.relief(
    color=rl.theme.color("surface.base"),   # the surface's own color (default: the color under it)
    light=(-1, -1),                          # where the light comes from (default: the theme's)
    depth=1.0,                               # strength of highlight and shadow (0 = flat)
    softness=1.0,                            # how far the shadows spread
    highlight=rl.theme.color("relief.light"),
    shade=rl.theme.color("relief.dark"),
)

rl.Surface.solid(color="#EEE6DA")            # no relief at all
rl.Surface.transparent()
rl.Surface.frosted(blur=24, tint="#EEE6DAB8")   # optional glass effect (sections 10.3 and 10.5)
```

Surfaces are **animatable**, parameter by parameter:

```python
rl.animate(window, 300, surface=rl.Surface.relief(depth=0.6))
```

### 10.3 Transparency

Transparency stays fully under Python's control, but it is an effect you choose, not the base of the
look.

| I want to... | Use | Cost |
|---|---|---|
| make the whole window more transparent (text included) | `window.opacity = 0.8` | very low: the compositor blends the window |
| see through the background only | `surface=rl.Surface.solid(color="#EEE6DA99")` | low |
| frosted glass: blur what is behind | `surface=rl.Surface.frosted(blur=24)` | **high**: off in the system theme |
| blur the content itself (e.g. behind a dialog) | `component.blur = 8` | medium |

With `rl.preferences.transparency == "reduced"`, frosted and translucent surfaces become solid.

### 10.4 Shadows, glow, borders and gradients

```python
rl.Shadow(x=6, y=6, blur=16, spread=0, color="#00000026")          # an extra shadow
rl.Shadow(x=-4, y=-4, blur=12, color="#FFFFFFCC")                   # a highlight is a light "shadow"
rl.Shadow(x=3, y=3, blur=8, color="#0000001F", inset=True)          # inner shadow (sunken)

rl.Glow(color="#2E8C86", radius=10, intensity=0.35)                 # focus ring, active control

rl.Border(1, rl.theme.color("border.subtle"))
rl.Border(2, rl.Gradient.linear(90, [(0, "#1E5D59"), (1, "#C9A26B")]))

rl.Gradient.linear(angle=135, stops=[(0, "#F2EBE1"), (1, "#E4DACB")])
rl.Gradient.radial(center=(0.5, 0.3), radius=0.8, stops=[(0, "#1E5D5940"), (1, "#00000000")])
```

The relief of `elevation` is made of these same shadows; use `shadow=` only for something the theme
does not give.

### 10.5 Cost

| Effect | Cost | Tip |
|---|---|---|
| opacity, scale, rotation, offset | very low | prefer these for animation |
| relief (`elevation`), shadows and glow | low after the first frame | pre-blurred and cached by size, radius and elevation (9 slices); changing the elevation cross-fades cached shadows. Animating a shadow's **blur radius** costs more |
| opaque surfaces | lowest | the compositor skips whatever an opaque window covers, and moving a window redraws only the uncovered area |
| `Surface.frosted` / `backdrop_blur` | high | the blur of what is behind must be redone whenever it changes: keep it for small, short-lived things |
| `warp` | high | short effects only |

---

## 11. Layout

```python
rl.Column([...], spacing=12, padding=16, align="start" | "center" | "end" | "stretch")
rl.Row([...],    spacing=8,  align="center", justify="between")   # "start", "center", "end", "between", "around"
rl.Grid([...],   columns=4,  spacing=16)                          # or columns="auto", item_width=96
rl.Stack([...])                                                   # children on top of each other (overlays)
rl.Scroll(content, horizontal=False)
rl.Spacer()                                                       # takes the free space
rl.Divider()
```

Layout properties of any component: `grow=1` (share of the free space), fixed `width`/`height`,
`min_width`, `max_width`, `margin`, `align_self`.

In `rl.Stack`, each child can have `anchor="top_right"` (or `"center"`, `"bottom_left"`...) and
`offset`. This is how badges, toasts and overlays are built.

**Right-to-left languages** (phase 2): rows and alignment automatically mirror when the UI language
is RTL. Use `"start"`/`"end"` instead of left/right so layouts mirror correctly.

---

## 12. Components

All of them accept the visual properties (section 4), `style`, `states`, `style_class`,
`transition`, `transitions`, `enter`, `exit` and `tooltip`.

| Component | Main parameters | Events |
|---|---|---|
| `Label(text, style="body")` | `style`: `h1`, `h2`, `body`, `caption`; `wrap=True`; `max_lines` | — |
| `Button(text, icon=None, primary=False)` | `variant`: `primary`, `secondary`, `ghost`; `loading=False` | `on_click` |
| `IconButton(icon)` | `size` | `on_click` |
| `TextField(value="", placeholder="")` | `kind`: `text`, `password`, `search`, `number`; `icon` | `on_change`, `on_submit` |
| `TextArea(value="")` | `lines`, `read_only` | `on_change` |
| `Switch(on=False)` | — | `on_change` |
| `Checkbox(checked=False, text="")` | — | `on_change` |
| `Radio(options, value=None)` | `direction`: `vertical`, `horizontal` | `on_change` |
| `Tabs([...], active=0)` | `pages=[...]`, `animation=rl.effects.slide()` | `on_change` |
| `Chip(text, removable=False)` | `selected` | `on_click`, `on_remove` |
| `Dropdown(options, value=None)` | `searchable=True` | `on_change` |
| `Slider(value=0.5, min=0, max=1)` | `step` | `on_change` |
| `Progress(value=0.0)` | `indeterminate=False` | — |
| `Steps([...], current=0)` | numbered steps of a process; `labels` | `on_change` |
| `Spinner()` | `size` | — |
| `Skeleton(lines=3)` | placeholder rows while content loads | — |
| `Tooltip(text)` | (used through `tooltip=` on any component) | — |
| `Badge(text_or_number, kind="info")` | `kind`: `success`, `warning`, `error`, `info` | — |
| `ListView(items, make=fn)` | `selection`: `none`, `single`, `multiple`; `virtual=True` (huge lists) | `on_select`, `on_activate` |
| `IconGrid(items)` | `icon_size` | `on_activate` |
| `Table(columns, rows)` | `sortable=True` | `on_select` |
| `Card(title=None, content=None)` | `elevation` 1–5 (default 3) | `on_click` |
| `Breadcrumbs([...])` | — | `on_navigate` |
| `Pagination(total, current=1)` | — | `on_change` |
| `EmptyState(title, message, action=None)` | `illustration` | — |
| `Toast(text, kind="info")` | `duration=4000`, `action` | `on_close` |
| `Banner(text, kind="info")` | `icon`, `action` | `on_close` |
| `Dialog(title, message, actions)` | `icon`, `danger=False` (destructive confirmation) | `on_result` |
| `Avatar(image=None, initials="")` | `size`, `ring=False` | `on_click` |
| `Calendar(date=None)` | `min`, `max` | `on_change` |
| `Chart.line(series)` / `Chart.ring(value)` | `colors`, `animate=True` | — |
| `Menu([...])` / context menu | `rl.MenuItem(text, shortcut, action)` | — |
| `Image(path)` | `fit`: `cover`, `contain` | — |
| `Icon(name)` | system icons by name: `"folder"`, `"settings"`... | — |
| `Canvas(...)` | custom drawing (section 14) | — |

Every component that **appears and disappears** (menus, tooltips, toasts, dropdowns) uses
`enter`/`exit`, which you can replace:

```python
rl.Menu(items, enter=rl.effects.zoom(0.96, 140), exit=rl.effects.fade(100))
```

---

## 13. Events and input

```python
comp.on_click(fn)                   # fn() or fn(event)
comp.on_double_click(fn)
comp.on_enter(fn); comp.on_leave(fn)        # mouse enters/leaves
comp.on_press(fn); comp.on_release(fn)
comp.on_drag(fn)                    # event.delta, event.position
comp.on_scroll(fn)                  # event.dx, event.dy
comp.on_key(fn)                     # event.key, event.text, event.ctrl/shift/alt
comp.on_text_input(fn)              # final text after dead keys / compose / IME (section 17.5)

app.shortcut("ctrl+s", save)
window.shortcut("esc", window.close)
```

The `event` object has: `position` (relative to the component), `screen_position`, `button`
(`"left"`, `"right"`, `"middle"`), `ctrl`, `shift`, `alt`, `time` and `stop()` (keeps the event
from reaching outer components).

Drag and drop between apps: `comp.draggable(data=...)` and `comp.on_drop(fn)`.

---

## 14. Custom drawing

```python
def draw(p: rl.Painter):
    p.rect(0, 0, p.width, p.height, radius=12, elevation=-1)     # a sunken well in the theme's relief
    p.circle(60, 60, 30, color="#1E5D59", glow=rl.Glow("#1E5D59", 12))
    p.line([(0, 100), (50, 80), (100, 120)], width=2, color="#2E8C86", smooth=True)
    p.text("CPU", 10, 10, font=rl.theme.font("caption"), color="#6F665C")
    p.image("cover.png", rl.Rect(120, 10, 64, 64), radius=8)
    with p.save():                   # saves/restores transforms and clipping
        p.translate(200, 50); p.rotate(30)
        p.rect(-10, -10, 20, 20, gradient=rl.Gradient.linear(0, [(0, "#1E5D59"), (1, "#2E8C86")]))

canvas = rl.Canvas(on_draw=draw, width=320, height=160)
canvas.redraw()          # request a new frame (only when the data changes)
```

`p.text` accepts any Unicode text and uses font fallback (section 17.4). All `Painter` rasterization
runs in C, with antialiasing.

---

## 15. Async and threads

`app.run()` integrates the UI event loop with **`asyncio`**. Callbacks can be coroutines:

```python
import asyncio

async def load():
    button.loading = True
    data = await asyncio.to_thread(fetch_data)    # does not freeze the UI
    list_view.items = data
    button.loading = False

button = rl.Button(_("Refresh"), on_click=load)
```

- The UI may only be changed from the **main thread**. From another thread, use
  `rl.on_ui_thread(fn, *args)`.
- `app.task(coroutine)` starts a task tied to the app (cancelled when the app closes).
- `rl.timer(ms, fn, repeat=False)` for periodic work that is not animation.

---

## 16. Accessibility and user preferences

```python
rl.preferences.reduce_motion       # bool: the user asked for less motion
rl.preferences.transparency        # "normal" | "reduced" (frosted and translucent surfaces become solid)
rl.preferences.high_contrast       # bool: relief also gets borders and stronger shadows
rl.preferences.text_scale          # 1.0 = normal
rl.preferences.on_change(fn)
```

- Built-in effects (`rl.effects.*`) and system components already respect these preferences.
- In your own effects, check `ctx.reduce_motion` (section 9.6).
- **Strict mode:** if the user enables "Reduce motion — strict" in Settings, the compositor replaces
  **any** animation with a fade of at most 120 ms, even if the app ignores the preference.
- With `transparency="reduced"`, every `Surface.frosted` becomes `Surface.solid` with its tint color.
- With `high_contrast`, raised and sunken surfaces also get a 1 px border and darker shadows, so an
  edge never depends on a shadow alone (the known weak point of soft relief). Text on the accent and
  on surfaces always keeps a contrast of at least 4.5:1.
- Components accept `accessible_label=` for screen readers (planned).

---

## 17. Internationalization and Unicode

OpalineOS is written in **English**, and English is the source language of every UI text. The user
can switch the system language at any time in **Settings → Language & Region** (currently **English
(en-US)** and **Português (Brasil) (pt-BR)**). Open apps update their texts **live, without a
restart**.

### 17.1 Translatable text: `_()`

```python
from opaline.i18n import _, _n

save = rl.Button(_("Save"))                         # "Save" / "Salvar"
title = rl.Label(_("Hello, {name}!", name=user))    # named parameters
count = rl.Label(_n("{n} file", "{n} files", n))    # plural forms
```

- `_()` returns a **live translatable text** (`i18n.Text`), not a plain `str`. Components keep the
  reference and redraw themselves when the language changes.
- `str(_("Save"))` gives the text in the current language, for when you really need a `str`
  (logs in English should use the plain English string instead).
- Combine texts with `_()` and parameters, never by concatenating translated fragments: word order
  changes between languages.
- Parameters can themselves be live texts or callables, so values stay current:
  `_("{count} items", count=lambda: len(items))`.
- Only mark texts that the user sees. Identifiers, file paths, log messages and exceptions stay in
  English.

### 17.2 Translation catalogs

Translations use the standard **gettext** format, so tools like Poedit and Weblate work.

```
/apps/counter/locale/pt_BR.po
```

```po
msgid "Counter"
msgstr "Contador"

msgid "Add"
msgstr "Somar"

msgid "{n} file"
msgid_plural "{n} files"
msgstr[0] "{n} arquivo"
msgstr[1] "{n} arquivos"
```

- The SDK extracts every `_()`/`_n()` call into a template: `opaline-i18n extract` → `locale/messages.pot`.
- `.po` files are compiled to `.mo` when the app is installed.
- Missing translations fall back to English, never to an empty string.
- Translation context, for words with more than one meaning: `_("Open", context="verb")`.

### 17.3 Language changes and regional formatting

```python
from opaline import i18n

i18n.language                  # "en-US" or "pt-BR"
i18n.on_change(lambda lang: rebuild_report())   # for text you build yourself

i18n.format_date(dt, "short")      # en-US "10/27/26"            pt-BR "27/10/26"
i18n.format_date(dt, "medium")     # en-US "Mon, Oct 27"         pt-BR "seg., 27 de out."
i18n.format_time(dt)               # en-US "9:42 PM"             pt-BR "21:42"
i18n.format_number(1234.5)         # en-US "1,234.5"             pt-BR "1.234,5"
i18n.format_size(15_400_000)       # en-US "15.4 MB"             pt-BR "15,4 MB"
i18n.format_relative(minutes=-5)   # en-US "5 minutes ago"       pt-BR "há 5 minutos"
i18n.compare(a, b)                 # alphabetical comparison for the language ("é" next to "e")
sorted(names, key=i18n.sort_key)
```

Components that show dates and numbers (`Calendar`, `Table`, `Chart`) format them automatically and
update when the language changes.

### 17.4 Unicode text

- Every text API (`Label`, `TextField`, `Painter.text`, window titles, file names) accepts **any
  Unicode text**: `"Configurações"`, `"Über"`, `"Ñandú"`, `"Ελληνικά"`, `"Русский"`.
- Fonts: **Inter** for Latin, Greek and Cyrillic, with **Noto** fallback fonts for other scripts.
  Fallback happens per character, automatically.
- Phase 2 (stage E12): complex scripts (Arabic, Devanagari, Thai) through HarfBuzz, right-to-left
  text (Arabic, Hebrew) with automatic layout mirroring, CJK and color emoji.
- Text measurement and cursor movement work on **grapheme clusters** (what the user sees as one
  character), so `"ã"` written as `a` + combining tilde is one character for the cursor.

### 17.5 Keyboard and text input

- Use `on_text_input` to receive typed text. It already applies the keyboard layout, dead keys
  (`´` + `e` → `é`), the Compose key and, in phase 2, input methods (IME) for Chinese, Japanese and
  Korean. `on_key` is for shortcuts and physical keys.
- `TextField` and `TextArea` handle all of this automatically.
- Layouts available at launch: **US**, **US-International** and **ABNT2** (Brazil), switchable in
  Settings → Keyboard or with `Super+Space`. The keyboard layout is independent from the UI language.

---

## 18. Performance and debugging

**Practical rules:**
1. To animate, prefer `opacity`, `scale`, `offset` and `rotation` (only the compositor works).
2. Avoid `every_frame`. Describe the animation and let the compositor run it.
3. Large lists: `rl.ListView(..., virtual=True)` draws only the visible items.
4. Many components animating at once: use `rl.stagger` to spread them over time.
5. Large images: load with `rl.Image(..., size=(w, h))`, which decodes at the reduced size.

**Tools:**

```python
rl.debug.fps(True)              # frame counter in the corner of the screen
rl.debug.regions(True)          # paints the regions redrawn each frame
rl.debug.layers(True)           # outlines every component and window
rl.debug.slow_motion(5)         # ALL animations 5x slower: great for tuning motion
rl.debug.animations()           # lists the animations running right now
rl.debug.pseudo_language(True)  # shows "[Šåvé]"-style texts to find strings missing _()
```

Global system shortcut: **Ctrl+Alt+Shift+D** opens the compositor debug panel.

---

## 19. Complete examples

### 19.1 Card that lifts on hover and sinks when pressed

```python
from opaline import relief as rl
from opaline.i18n import _

app = rl.App(_("Cards"))
window = rl.Window(_("Cards"), width=520, height=320)

def make_card(title, text):
    return rl.Card(
        title=title,
        content=rl.Label(text, wrap=True, style="caption"),
        width=220,
        elevation=3,
        states={
            "hover": rl.Style(elevation=4, offset=(0, -2)),
            "pressed": rl.Style(elevation=-1, scale=0.99),
            "focused": rl.Style(glow=rl.Glow(rl.theme.color("focus"), 10, 0.5)),
        },
        transition=rl.Transition(160, rl.curves.smooth),
        on_click=lambda: print(str(title)),
    )

window.add(rl.Row([
    make_card(_("Projects"), _("Your recent projects")),
    make_card(_("Music"), _("Continue listening")),
], spacing=16, padding=24))
window.show()
app.run()
```

### 19.2 Toast that slides in and leaves on its own

```python
import asyncio

async def notify(stack, text):
    toast = rl.Toast(text, kind="success", anchor="bottom_right", margin=16)
    stack.add(toast)
    await rl.animate(toast, curve=rl.curves.spring(stiffness=420, damping=30),
                     from_=dict(offset=(0, 40), opacity=0),
                     offset=(0, 0), opacity=1)
    await asyncio.sleep(3)
    await rl.animate(toast, 180, rl.curves.ease_in, offset=(40, 0), opacity=0)
    stack.remove(toast, animate=False)

# app.task(notify(stack, _("File saved")))
```

### 19.3 List with staggered entrance

```python
column = rl.Column(spacing=8, animate_layout=rl.Transition(200))
for name in [_("Desktop"), _("Documents"), _("Downloads"), _("Music"), _("Pictures")]:
    column.add(rl.Card(name, opacity=0), animate=False)

rl.stagger(column.children, interval=45,
           make=lambda c: rl.Anim(c, 260, rl.curves.ease_out,
                                  from_=dict(opacity=0, offset=(0, 10)),
                                  opacity=1, offset=(0, 0))).start()
```

### 19.4 User-controlled relief and transparency

```python
window = rl.Window(_("Relief"), width=420, height=240)
depth = rl.Slider(value=1.0, min=0.0, max=1.5)        # 0 = flat, 1 = the theme, 1.5 = deeper
overall = rl.Slider(value=1.0, min=0.4, max=1.0)
frosted = rl.Switch(on=False)

def update(_value=None):
    window.surface = (rl.Surface.frosted(blur=24) if frosted.on
                      else rl.Surface.relief(depth=depth.value))
    window.opacity = overall.value

for control in (depth, overall, frosted):
    control.on_change(update)

window.transitions = {"surface": rl.Transition(120), "opacity": rl.Transition(120)}
window.add(rl.Column([
    rl.Label(_("Depth of the relief")), depth,
    rl.Label(_("Window opacity")), overall,
    rl.Row([rl.Label(_("Frosted glass (uses more power)")), frosted], justify="between"),
], spacing=8, padding=20))
window.show()
```

### 19.5 Top-bar button that bounces when an app opens

```python
def bounce(icon):
    return rl.animate(icon, 600, repeat=2, keyframes=rl.Keyframes({
        0.0:  dict(offset=(0, 0)),
        0.25: dict(offset=(0, -14)),
        0.5:  dict(offset=(0, 0)),
        0.7:  dict(offset=(0, -6)),
        1.0:  dict(offset=(0, 0)),
    }, curves={0.25: rl.curves.ease_out, 0.5: rl.curves.ease_in,
               0.7: rl.curves.ease_out, 1.0: rl.curves.ease_in}))
```

### 19.6 System-wide window effects (Settings app)

```python
from opaline import relief as rl, system
from opaline.i18n import _

PRESETS = {
    "smooth": dict(open=rl.effects.spring(), close=rl.effects.zoom(0.94, 160),
                   minimize=rl.effects.to_bar(340)),
    "fast":   dict(open=rl.effects.fade(100), close=rl.effects.fade(80),
                   minimize=rl.effects.to_bar(180)),
    "none":   dict(open=rl.effects.none, close=rl.effects.none,
                   minimize=rl.effects.none),
}
LABELS = {"smooth": _("Smooth"), "fast": _("Fast"), "none": _("None")}

choice = rl.Dropdown(LABELS, value="smooth",          # dict: stored value -> displayed text
                     on_change=lambda key: system.effects.set(**PRESETS[key]))
```

### 19.7 Language switcher (Settings → Language & Region)

```python
from opaline import relief as rl, i18n
from opaline.i18n import _

languages = rl.Dropdown(
    {"en-US": "English (United States)", "pt-BR": "Português (Brasil)"},   # names shown in their own language
    value=i18n.language,
    on_change=i18n.set_language,       # every open app updates immediately
)

window.add(rl.Column([
    rl.Label(_("Language"), style="h2"),
    languages,
    rl.Label(_("Changes apply immediately to all open apps."), style="caption"),
], spacing=8, padding=20))
```

---

## 20. Quick reference

```python
from opaline import relief as rl
from opaline.i18n import _, _n

# animation
rl.animate(target, 200, rl.curves.smooth, from_=dict(opacity=0), opacity=1)
rl.Anim(...)  ·  rl.sequence(...)  ·  rl.parallel(...)  ·  rl.stagger(...)  ·  rl.wait(ms)
rl.Keyframes({0: {...}, 0.5: {...}, 1: {...}})
rl.curves.linear | smooth | ease_in | ease_out | ease_in_out | bezier() | spring() | bounce() | elastic() | steps()
comp.transitions = {"opacity": rl.Transition(150)}
with rl.transition(250): ...       with rl.no_animation(): ...

# windows
window.show() close() hide() minimize() restore() maximize() unmaximize() fullscreen() focus() alert()
window.effects = rl.Effects(open=, close=, minimize=, restore=, maximize=, unmaximize=,
                            fullscreen=, exit_fullscreen=, focus_in=, focus_out=,
                            drag_start=, drag_end=, alert=)
def effect(ctx): return rl.Anim(ctx.window, ...)
rl.effects.none | fade() | zoom() | slide() | spring() | to_bar() | genie() | shake()

# visuals
elevation=-2…5  ·  rl.Surface.relief() | solid() | transparent() | frosted()
rl.Shadow(x, y, blur, spread, color)  ·  rl.Glow(color, radius, intensity)  ·  rl.Border(w, color)
rl.Gradient.linear(angle, stops)  ·  rl.Gradient.radial(center, radius, stops)
rl.theme.color() radius() elevation() light spacing() font() motion()

# i18n
_("Text")  ·  _("Hi, {name}!", name=n)  ·  _n("{n} file", "{n} files", n)
i18n.language  ·  i18n.set_language("pt-BR")  ·  i18n.on_change(fn)
i18n.format_date() format_time() format_number() format_size() format_relative()
```

---

## 21. Conventions

- **Everything in English**: classes, functions, parameters, properties, events, tokens, comments,
  log messages and this documentation.
- User-visible text is written in English inside `_()` and translated through catalogs. It may
  contain any Unicode character.
- Classes in `PascalCase`, functions and properties in `snake_case`. Events start with `on_`.
  Python keywords are avoided with a trailing underscore (`from_`).
- Units: time in **ms**, angles in **degrees**, sizes in **logical pixels**, opacity from **0.0 to
  1.0**.
- Colors in hexadecimal (`"#RRGGBB"` or `"#RRGGBBAA"`) or theme tokens.
- Every callback can be a plain function or a coroutine (`async def`).
- Distribution: package **`opaline-relief`**. Import: **`from opaline import relief as rl`**.
  System APIs: **`from opaline import system`**. Translations: **`from opaline import i18n`**.

---

## 22. Open design questions

| Question | Options | Decide by |
|---|---|---|
| Effects running **inside** the compositor (no 50 ms round trip) | ready-made description (current) × small sandboxed scripts in the compositor | E9 |
| 3D transforms (perspective, flipping windows) | 2D only × 2D + simple perspective | E9 |
| Custom shaders/filters written by apps | no × a simple filter language executed in C | after E10 |
| Declarative UI (describe the screen as data, reactive) | imperative only (current) × optional declarative layer | E10 |
| Dark theme in relief | the same relief with a dark surface and dim highlights × a flatter dark theme with borders | E9 |
| Light direction | fixed per theme (current: top left) × follows a system setting | E9 |
| Text theme file (`.toml`) for community themes | yes × no | E9 |
