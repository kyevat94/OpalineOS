# opaline-vitrum — Documentation

> **Status: specification v0.2 (planned API).** None of this is implemented yet. Implementation
> happens in stage **E9** of the roadmap (`project.md`, section 8). The examples show how the API
> **will** work. Once implementation starts, this document becomes the official reference and every
> API change is recorded here.
>
> License: **LGPL-3.0-or-later**. Apps that use the library may have any license (see `LICENSING.md`).

`opaline-vitrum` is the GUI toolkit of **OpalineOS**. With it you build Python apps with windows,
components and the system's glass look, and you control **everything that moves or glows on screen**:

- animation of any visual property (position, size, scale, rotation, opacity, color, blur, shadow,
  glow...);
- the **open, close, minimize, restore, maximize, fullscreen, focus and unfocus** effects of windows;
- **transparency and materials** (glass, acrylic, solid), per window and per component;
- motion curves, springs, keyframes, sequences and implicit transitions;
- **internationalization**: translatable text that updates live when the user changes the system
  language, and full Unicode support.

```python
from opaline import vitrum as vt
```

| Package (distribution) | Import | Contents |
|---|---|---|
| `opaline-vitrum` | `from opaline import vitrum` | GUI toolkit (this document) |
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
10. [Materials, transparency, shadows and glow](#10-materials-transparency-shadows-and-glow)
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
 opaline.vitrum (Python)  ── public API, documented here
     │
     ▼
 _vitrum (C)  ── draws components into the window buffer (text, glass, shadows)
     │  shared memory + command queue
     ▼
 Compositor (C)  ── composes windows on screen and RUNS animations at 60 fps
```

The rule that keeps everything smooth: **Python describes, the compositor executes.**

- When you call `vt.animate(...)`, Python sends the compositor a *description* of the animation
  (target, properties, duration and curve). The compositor computes every frame in C, so the
  animation stays smooth even while Python is busy, waiting on the network or collecting garbage.
- **Transform** properties (`scale`, `rotation`, `offset`, `opacity`, `blur`) are applied by the
  compositor without redrawing content, which makes them the cheapest to animate.
- Properties that change **content** (`width`, `background`, `radius`, text...) require the
  component to be redrawn. `_vitrum` does this in C, only for the affected region.
- **Window effects** (section 9) are Python functions called when the event happens. They return an
  animation description that the compositor runs. If the app does not answer within **50 ms**
  (frozen or crashed), the compositor uses the system default effect.

---

## 2. Getting started

### Hello, world

```python
from opaline import vitrum as vt
from opaline.i18n import _

app = vt.App(_("Hello"))
window = vt.Window(_("Hello"), width=320, height=160)
window.add(vt.Label(_("Hello, OpalineOS!"), style="h1"))
window.show()
app.run()
```

### Counter

```python
from opaline import vitrum as vt
from opaline.i18n import _

app = vt.App(_("Counter"))
window = vt.Window(_("Counter"), width=360, height=200)

total = vt.Label("0", style="h1")

def increment():
    total.text = str(int(total.text) + 1)
    # a small "pop" on every click
    vt.animate(total, 220, keyframes=vt.Keyframes({0: dict(scale=1), 0.4: dict(scale=1.15), 1: dict(scale=1)}))

window.add(vt.Column([
    total,
    vt.Button(_("Add"), primary=True, on_click=increment),
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
| **Color** | `"#F28B82"`, `"#F28B82B3"` (with alpha), `vt.Color(242, 139, 130, a=0.7)`, `vt.theme.color("accent")` | methods: `.with_alpha(0.5)`, `.lighten(0.1)`, `.darken(0.1)`, `.mix(other, 0.3)` |
| **Point** | `(x, y)` or `vt.Point(x, y)` | logical pixels |
| **Size** | `(width, height)` or `vt.Size(w, h)` | |
| **Rect** | `vt.Rect(x, y, width, height)` | `.center`, `.center_x`, `.expand(n)`, `.contains(p)` |
| **Radius** | `12` or `vt.Radius(top_left, top_right, bottom_right, bottom_left)` | rounded corners |
| **Insets** | `16`, `(16, 8)` (horizontal, vertical) or `vt.Insets(top, right, bottom, left)` | `padding` and `margin` |
| **Angle** | `float` in **degrees** | `rotation=15` |
| **Origin** | `(0.5, 0.5)` = center, `(0, 0)` = top-left corner | point around which scale and rotation happen |
| **Text** | any `str` (full Unicode) or a translatable text from `_()` | see section 17 |

Pixels are **logical**: on high-density screens the system multiplies by the screen scale
(`vt.screen.scale`), so sizes look the same on any monitor.

---

## 4. Visual properties

Every component, and every window, has these properties. **All properties marked animatable can be
used in `vt.animate`, keyframes, transitions and effects.**

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
| `material` | vt.Material | ✅ | medium | background: solid, glass or acrylic (section 10) |
| `background` | Color / Gradient | ✅ | medium | |
| `text_color` | Color | ✅ | medium | |
| `radius` | int / Radius | ✅ | medium | rounded corners |
| `border` | vt.Border | ✅ | medium | `vt.Border(1, "#FFFFFF1A")` |
| `shadow` | vt.Shadow / list | ✅ | low* | *shadows are cached by size and radius |
| `glow` | vt.Glow | ✅ | low* | the colored glow |
| `blur` | px | ✅ | medium | blurs the component itself |
| `backdrop_blur` | px | ✅ | high | blurs what is **behind** it (glass effect) |
| `saturation` | float | ✅ | low | 0 = grayscale, 1 = normal |
| `brightness` | float | ✅ | low | 1 = normal |
| `warp` | vt.Warp | ✅ | high | mesh deformation (e.g. the "genie" effect). **Phase 2** |
| `clip` | bool | — | — | clips children to the rounded corners |
| `visible` | bool | — | — | animate `opacity` to fade out smoothly |
| `cursor` | str | — | — | `"arrow"`, `"hand"`, `"text"`, `"move"`... |

---

## 5. Theme and tokens

The theme holds the system's colors, radii, shadows, typography and **motion** (values in
`project.md`, section 6.1). Use tokens instead of hard-coded values so your app follows the user's
theme.

```python
vt.theme.color("accent")          # Color("#F28B82")
vt.theme.color("surface.glass")
vt.theme.radius("window")         # 16
vt.theme.shadow("s2")             # vt.Shadow(0, 8, 24, ...)
vt.theme.spacing(4)               # 4th step of the 4 px grid -> 16
vt.theme.font("h1")               # vt.Font("Inter", 28, weight=600)
vt.theme.motion("open")           # vt.Transition(200, vt.curves.smooth)
```

### Color tokens

`background.base`, `surface.glass`, `surface.raised`, `surface.field`, `border.subtle`, `accent`,
`accent.strong`, `accent.glow`, `text.primary`, `text.secondary`, `text.disabled`, `success`,
`warning`, `error`, `info`, `chart.cpu`, `chart.memory`, `chart.disk`.

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
my_theme = vt.Theme(
    base="dark",                                  # starts from the system default theme
    colors={"accent": "#7FD1B9", "accent.strong": "#5BC0A0"},
    radii={"window": 20},
    motion={"open": vt.Transition(260, vt.curves.spring(stiffness=300, damping=22))},
)
app.theme = my_theme          # this app only
# window.theme = my_theme     # or this window only

vt.theme.on_change(lambda theme: print("the user changed the theme"))
```

---

## 6. Styles and states

A `vt.Style` groups visual properties. Components have **states**, and each state can have its own
style. Switching between states is animated by `transition`.

States: `normal`, `hover`, `pressed`, `focused`, `disabled`, `selected`, `active`.

```python
button = vt.Button(
    _("Send"),
    style=vt.Style(background=vt.theme.color("accent"), radius=10),
    states={
        "hover":    vt.Style(glow=vt.Glow(vt.theme.color("accent"), radius=16, intensity=0.5),
                             scale=1.02),
        "pressed":  vt.Style(scale=0.97, brightness=0.9),
        "disabled": vt.Style(opacity=0.45, saturation=0.2),
    },
    transition=vt.Transition(120, vt.curves.smooth),
)
```

### Reusable style classes

```python
vt.styles.register("danger-button", vt.Style(background=vt.theme.color("error")),
                   states={"hover": vt.Style(glow=vt.Glow(vt.theme.color("error"), 14))})

vt.Button(_("Delete"), style_class="danger-button")
```

---

## 7. Animation

### 7.1 `vt.Anim` and `vt.animate`

- `vt.Anim(...)` **describes** an animation without starting it. Use it to build sequences and
  effects.
- `vt.animate(...)` creates **and starts** the animation (same as `vt.Anim(...).start()`).

```python
vt.Anim(target, duration=None, curve=None, *, from_=None, keyframes=None, delay=0, repeat=1,
        alternate=False, **properties)
```

| Parameter | Description |
|---|---|
| `target` | component, window or layer |
| `duration` | ms. Optional with `curves.spring` (the spring computes its own duration) |
| `curve` | motion curve (7.2). Default: `vt.theme.motion("open").curve` |
| `from_` | `dict` with start values. Without it, the animation starts from the current value |
| `keyframes` | `vt.Keyframes(...)`: keyframes instead of end values |
| `delay` | ms before starting |
| `repeat` | number of times, or `vt.FOREVER` |
| `alternate` | plays forward and backward on each repetition |
| `**properties` | end values: `opacity=1, scale=1.1, x=vt.by(40)` |

```python
card = vt.Card(...)

# appear while moving up
vt.animate(card, 240, vt.curves.smooth,
           from_=dict(opacity=0, offset=(0, 16)),
           opacity=1, offset=(0, 0))

# relative values
vt.animate(card, 200, x=vt.by(40), rotation=vt.by(-5))
```

### Controlling a running animation

```python
a = vt.animate(card, 1000, scale=1.5)

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
| `vt.curves.linear` | progress bars, continuous rotation |
| `vt.curves.smooth` | system default: `bezier(.2, .8, .2, 1)` |
| `vt.curves.ease_in` | starts slow and accelerates: elements **leaving** the screen |
| `vt.curves.ease_out` | starts fast and decelerates: elements **entering** |
| `vt.curves.ease_in_out` | slow → fast → slow |
| `vt.curves.bezier(x1, y1, x2, y2)` | same as CSS `cubic-bezier` |
| `vt.curves.spring(stiffness=380, damping=28, mass=1, initial_velocity=0)` | physical motion. No `duration` needed |
| `vt.curves.bounce(bounces=3)` | bouncing ball |
| `vt.curves.elastic(amplitude=1, period=0.3)` | overshoots and settles |
| `vt.curves.steps(n)` | animates in `n` jumps (clocks, sprites) |
| Python function `f(t) -> float` | custom curve. The system samples it at 256 points and sends the table to the compositor |

```python
import math

def breathe(t):
    return 0.5 - 0.5 * math.cos(t * math.pi)

vt.animate(icon, 1600, curve=breathe, scale=1.08, repeat=vt.FOREVER, alternate=True)
```

### 7.3 Keyframes

```python
pop = vt.Keyframes({
    0.0: dict(scale=1.0,  offset=(0, 0)),
    0.3: dict(scale=1.12, offset=(0, -10)),
    0.6: dict(scale=0.96, offset=(0, 0)),
    1.0: dict(scale=1.0),
}, curves={0.3: vt.curves.ease_out, 0.6: vt.curves.ease_in})   # per-segment curves (optional)

vt.animate(icon, 420, keyframes=pop)
```

### 7.4 Sequences, groups and staggering

```python
intro = vt.sequence(
    vt.Anim(title,  200, from_=dict(opacity=0), opacity=1),
    vt.parallel(
        vt.Anim(image, 300, from_=dict(scale=0.9), scale=1),
        vt.Anim(body,  300, from_=dict(opacity=0, offset=(0, 12)),
                opacity=1, offset=(0, 0)),
    ),
    vt.wait(100),
    vt.Anim(button, curve=vt.curves.spring(), from_=dict(scale=0.8), scale=1),
)
intro.start()

# the same effect on many items, one after the other
vt.stagger(list_view.items, interval=40,
           make=lambda item: vt.Anim(item, 220, from_=dict(opacity=0, offset=(-12, 0)),
                                     opacity=1, offset=(0, 0))).start()
```

`vt.sequence`, `vt.parallel` and `vt.stagger` return an `Anim`, so they also accept `delay`,
`repeat` and `alternate`.

### 7.5 Implicit transitions

Instead of calling `animate`, declare **how** a property changes. From then on, every assignment
is animated.

```python
panel.transitions = {
    "opacity":  vt.Transition(180),
    "position": vt.Transition(curve=vt.curves.spring(stiffness=300, damping=26)),
    "*":        vt.Transition(150),          # any other property
}

panel.opacity = 0.5      # animates over 180 ms
panel.x = 300            # animates with a spring

# animate a block of changes at once, without declaring transitions
with vt.transition(250, vt.curves.smooth):
    panel.width = 400
    bar.opacity = 0

# change without animating
with vt.no_animation():
    panel.x = 0
```

### 7.6 Enter/exit animations and layout animation

```python
item = vt.Card(_("New item"),
               enter=vt.effects.slide("down", 220),   # when added
               exit=vt.effects.fade(160))              # when removed (waits for it to finish)

column = vt.Column(spacing=8, animate_layout=vt.Transition(220, vt.curves.smooth))
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
window = vt.Window(
    _("Editor"),
    width=900, height=600,
    x=None, y=None,                 # None = the system places it (centered/cascaded)
    icon="icon.svg",
    resizable=True,
    min_size=(400, 300),
    material=vt.Material.glass(),   # system default
    radius=vt.theme.radius("window"),
    shadow=vt.theme.shadow("s3"),
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
window.close(effect=vt.effects.fade(80))
window.minimize(effect=vt.effects.none)
```

### 8.3 State and properties

```python
window.state             # vt.WindowState.NORMAL | MAXIMIZED | MINIMIZED | FULLSCREEN
window.focused           # bool
window.title = _("Untitled — Editor")
window.opacity = 0.9     # whole window, content included (animatable)
window.material = vt.Material.glass(opacity=0.6, blur=32)   # background only (animatable)
window.screen            # monitor it is on: .rect, .work_area, .scale
```

### 8.4 Events

```python
@window.on_close
def confirm():
    if document.modified:
        answer = vt.dialog.ask(_("Save changes?"), [_("Save"), _("Discard"), _("Cancel")])
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
window.inactive_style = vt.Style(opacity=0.92, saturation=0.75, shadow=vt.theme.shadow("s1"))
window.focus_transition = vt.Transition(180)
```

For full control, use the `focus_in`/`focus_out` effects (section 9).

### 8.6 Custom title bar

```python
window = vt.Window(_("Player"), width=420, height=240, decoration="none")

title_bar = vt.DragArea(               # dragging here moves the window; double-click maximizes
    vt.Row([
        vt.Label(_("Now playing"), style="caption"),
        vt.Spacer(),
        vt.WindowButton("minimize"),
        vt.WindowButton("maximize"),
        vt.WindowButton("close"),
    ], align="center"),
    height=36,
)
window.add(vt.Column([title_bar, content]))
```

---

## 9. Window effects

This is the part that gives **full control** over how windows move. Every moment in a window's life
has an **effect**, and an effect is a Python function:

```python
def my_effect(ctx: vt.EffectContext) -> vt.Anim | None:
    ...
```

- Return an `Anim`: the compositor runs it.
- Return `None`: no animation (instant change).

### 9.1 Available moments

| Moment | When it happens | When it ends |
|---|---|---|
| `open` | `show()` | the window stays in the animation's final state |
| `close` | `close()` / `hide()` | the window is destroyed/hidden **after** the animation |
| `minimize` | minimize button, `minimize()` | the window disappears (goes to the dock) |
| `restore` | dock click, `restore()` | — |
| `maximize` / `unmaximize` | button, title bar double-click, `maximize()` | content is laid out again at the final size |
| `fullscreen` / `exit_fullscreen` | `fullscreen(...)` | same |
| `focus_in` / `focus_out` | the active window changes | — |
| `drag_start` / `drag_end` | the user starts/stops dragging the window | — |
| `alert` | `alert()` | — |

### 9.2 The context (`vt.EffectContext`)

| Field | Description |
|---|---|
| `ctx.window` | the window **surface** in the compositor. It is the animation target (all properties from section 4) |
| `ctx.moment` | `"open"`, `"close"`, `"minimize"`... |
| `ctx.origin` | current `Rect` of the window |
| `ctx.target` | final `Rect` (e.g. the screen's work area when maximizing) |
| `ctx.dock_icon` | `Rect` of the app icon in the dock, or `None` |
| `ctx.cursor` | mouse position |
| `ctx.screen` | the monitor (`.rect`, `.work_area`) |
| `ctx.reason` | `"user"`, `"app"` or `"system"` |
| `ctx.reduce_motion` | `True` if the user asked for less motion (section 16) |
| `ctx.content` | while resizing: `"scale"` (default, fast: stretches the image and re-lays out at the end) or `"relayout"` (lays out every frame, more expensive) |

### 9.3 Where to set effects: window, app or system

```python
effects = vt.Effects(open=..., close=..., minimize=..., restore=...,
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
system.effects.set(minimize=vt.effects.genie(), close=vt.effects.zoom(0.94, 160))
```

### 9.4 Built-in effects (`vt.effects`)

Each one returns an effect function. Use them directly or inside your own effects.

| Effect | Description |
|---|---|
| `vt.effects.none` | no animation |
| `vt.effects.fade(duration=160)` | opacity only |
| `vt.effects.zoom(scale=0.94, duration=200)` | scale + opacity |
| `vt.effects.slide(direction="down", duration=220, distance=24)` | slide + opacity |
| `vt.effects.spring(stiffness=380, damping=26)` | opens with a spring |
| `vt.effects.to_dock(duration=320)` | shrinks into the dock icon (minimize/restore) |
| `vt.effects.genie(duration=420)` | "genie" deformation into the dock. **Phase 2** (uses `warp`) |
| `vt.effects.shake(intensity=8)` | for `alert` |
| `vt.effects.default` | the system effect for that moment |

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
from opaline import vitrum as vt

SPRING = vt.curves.spring(stiffness=380, damping=28)

def open_(ctx):
    if ctx.reduce_motion:
        return vt.Anim(ctx.window, 120, from_=dict(opacity=0), opacity=1)
    return vt.Anim(ctx.window, curve=SPRING,
                   from_=dict(opacity=0, scale=0.92, offset=(0, 20)),
                   opacity=1, scale=1, offset=(0, 0))

def close(ctx):
    return vt.Anim(ctx.window, 160, vt.curves.ease_in, scale=0.94, opacity=0, blur=6)

def minimize(ctx):
    target = ctx.dock_icon or vt.Rect(ctx.origin.center_x, ctx.screen.rect.height, 1, 1)
    return vt.Anim(ctx.window, 320, vt.curves.ease_in, rect=target, opacity=0.2)

def restore(ctx):
    return vt.Anim(ctx.window, curve=SPRING,
                   from_=dict(rect=ctx.dock_icon or ctx.target, opacity=0.2),
                   rect=ctx.target, opacity=1)

def maximize(ctx):
    ctx.content = "scale"
    return vt.Anim(ctx.window, 260, vt.curves.smooth, rect=ctx.target, radius=0)

def unmaximize(ctx):
    return vt.Anim(ctx.window, 260, vt.curves.smooth,
                   rect=ctx.target, radius=vt.theme.radius("window"))

def focus_in(ctx):
    return vt.Anim(ctx.window, 140, opacity=1, saturation=1, shadow=vt.theme.shadow("s3"))

def focus_out(ctx):
    return vt.Anim(ctx.window, 220, opacity=0.9, saturation=0.7, shadow=vt.theme.shadow("s1"))

def drag_start(ctx):
    return vt.Anim(ctx.window, 120, opacity=0.85, scale=1.01)

def drag_end(ctx):
    return vt.Anim(ctx.window, 160, opacity=1, scale=1)

app = vt.App("Effects")
app.effects = vt.Effects(
    open=open_, close=close,
    minimize=minimize, restore=restore,
    maximize=maximize, unmaximize=unmaximize,
    focus_in=focus_in, focus_out=focus_out,
    drag_start=drag_start, drag_end=drag_end,
    alert=vt.effects.shake(6),
)

vt.Window("Demo", width=600, height=400).show()
app.run()
```

---

## 10. Materials, transparency, shadows and glow

### 10.1 Materials

A material defines the **background** of a window or component. Content (text, icons) stays sharp
and opaque. To make everything transparent, use `opacity`.

```python
vt.Material.solid(color="#1C2433")

vt.Material.glass(
    blur=24,              # px of blur applied to what is behind
    opacity=0.72,         # how much the tint covers the background
    tint=vt.theme.color("surface.glass"),
    saturation=1.2,       # boosts the colors of the blurred background
    noise=0.02,           # subtle grain (avoids banding in gradients)
    edge_light=0.10,      # thin light line on the top edge
)

vt.Material.acrylic()     # glass with more noise and tint: more readable
vt.Material.transparent()
```

Materials are **animatable**. Interpolation happens parameter by parameter:

```python
vt.animate(window, 300, material=vt.Material.glass(opacity=0.35, blur=40))
```

### 10.2 Opacity vs. material vs. blur

| I want to... | Use |
|---|---|
| make the whole window more transparent (text included) | `window.opacity = 0.8` |
| make only the background more "glassy" | `window.material = vt.Material.glass(opacity=0.5)` |
| more blur behind the window | `material=vt.Material.glass(blur=40)` |
| blur the content itself (e.g. behind a dialog) | `component.blur = 8` |

### 10.3 Shadows, glow, borders and gradients

```python
vt.Shadow(x=0, y=8, blur=24, spread=0, color="#00000080")
shadow=[vt.Shadow(0, 2, 6, color="#0000004D"), vt.Shadow(0, 16, 48, color="#00000066")]  # layered

vt.Glow(color="#F28B82", radius=16, intensity=0.45)

vt.Border(1, "#FFFFFF1A")
vt.Border(2, vt.Gradient.linear(90, [(0, "#F28B82"), (1, "#5AA9F7")]))

vt.Gradient.linear(angle=135, stops=[(0, "#1C2433"), (1, "#26303F")])
vt.Gradient.radial(center=(0.5, 0.3), radius=0.8, stops=[(0, "#F28B8240"), (1, "#00000000")])
```

### 10.4 Cost

| Effect | Cost | Tip |
|---|---|---|
| opacity, scale, rotation, offset | very low | prefer these for animation |
| shadow and glow | low after the first frame | radius and size are cached. Animating the shadow **radius** costs more than animating its **opacity** |
| `backdrop_blur` / glass | high | static backgrounds (wallpaper) are cached. Glass over content that changes every frame costs more |
| `warp` | high | short effects only |

---

## 11. Layout

```python
vt.Column([...], spacing=12, padding=16, align="start" | "center" | "end" | "stretch")
vt.Row([...],    spacing=8,  align="center", justify="between")   # "start", "center", "end", "between", "around"
vt.Grid([...],   columns=4,  spacing=16)                          # or columns="auto", item_width=96
vt.Stack([...])                                                   # children on top of each other (overlays)
vt.Scroll(content, horizontal=False)
vt.Spacer()                                                       # takes the free space
vt.Divider()
```

Layout properties of any component: `grow=1` (share of the free space), fixed `width`/`height`,
`min_width`, `max_width`, `margin`, `align_self`.

In `vt.Stack`, each child can have `anchor="top_right"` (or `"center"`, `"bottom_left"`...) and
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
| `Tabs([...], active=0)` | `pages=[...]`, `animation=vt.effects.slide()` | `on_change` |
| `Chip(text, removable=False)` | `selected` | `on_click`, `on_remove` |
| `Dropdown(options, value=None)` | `searchable=True` | `on_change` |
| `Slider(value=0.5, min=0, max=1)` | `step` | `on_change` |
| `Progress(value=0.0)` | `indeterminate=False`, `steps=[...]` | — |
| `Tooltip(text)` | (used through `tooltip=` on any component) | — |
| `Badge(text_or_number, kind="info")` | `kind`: `success`, `warning`, `error`, `info` | — |
| `ListView(items, make=fn)` | `selection`: `none`, `single`, `multiple`; `virtual=True` (huge lists) | `on_select`, `on_activate` |
| `IconGrid(items)` | `icon_size` | `on_activate` |
| `Table(columns, rows)` | `sortable=True` | `on_select` |
| `Card(title=None, content=None)` | `elevation`: `s1`, `s2`, `s3` | `on_click` |
| `Breadcrumbs([...])` | — | `on_navigate` |
| `Pagination(total, current=1)` | — | `on_change` |
| `EmptyState(title, message, action=None)` | `illustration` | — |
| `Toast(text, kind="info")` | `duration=4000`, `action` | `on_close` |
| `Calendar(date=None)` | `min`, `max` | `on_change` |
| `Chart.line(series)` / `Chart.ring(value)` | `colors`, `animate=True` | — |
| `Menu([...])` / context menu | `vt.MenuItem(text, shortcut, action)` | — |
| `Image(path)` | `fit`: `cover`, `contain` | — |
| `Icon(name)` | system icons by name: `"folder"`, `"settings"`... | — |
| `Canvas(...)` | custom drawing (section 14) | — |

Every component that **appears and disappears** (menus, tooltips, toasts, dropdowns) uses
`enter`/`exit`, which you can replace:

```python
vt.Menu(items, enter=vt.effects.zoom(0.96, 140), exit=vt.effects.fade(100))
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
def draw(p: vt.Painter):
    p.rect(0, 0, p.width, p.height, radius=12, color=vt.theme.color("surface.field"))
    p.circle(60, 60, 30, color="#F28B82", glow=vt.Glow("#F28B82", 12))
    p.line([(0, 100), (50, 80), (100, 120)], width=2, color="#4FC3D9", smooth=True)
    p.text("CPU", 10, 10, font=vt.theme.font("caption"), color="#A9B3C4")
    p.image("cover.png", vt.Rect(120, 10, 64, 64), radius=8)
    with p.save():                   # saves/restores transforms and clipping
        p.translate(200, 50); p.rotate(30)
        p.rect(-10, -10, 20, 20, gradient=vt.Gradient.linear(0, [(0, "#F28B82"), (1, "#FF6F7D")]))

canvas = vt.Canvas(on_draw=draw, width=320, height=160)
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

button = vt.Button(_("Refresh"), on_click=load)
```

- The UI may only be changed from the **main thread**. From another thread, use
  `vt.on_ui_thread(fn, *args)`.
- `app.task(coroutine)` starts a task tied to the app (cancelled when the app closes).
- `vt.timer(ms, fn, repeat=False)` for periodic work that is not animation.

---

## 16. Accessibility and user preferences

```python
vt.preferences.reduce_motion       # bool: the user asked for less motion
vt.preferences.transparency        # "normal" | "reduced" (glass becomes solid)
vt.preferences.high_contrast       # bool
vt.preferences.text_scale          # 1.0 = normal
vt.preferences.on_change(fn)
```

- Built-in effects (`vt.effects.*`) and system components already respect these preferences.
- In your own effects, check `ctx.reduce_motion` (section 9.6).
- **Strict mode:** if the user enables "Reduce motion — strict" in Settings, the compositor replaces
  **any** animation with a fade of at most 120 ms, even if the app ignores the preference.
- With `transparency="reduced"`, every `Material.glass` becomes `Material.solid` with the tint color.
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

save = vt.Button(_("Save"))                         # "Save" / "Salvar"
title = vt.Label(_("Hello, {name}!", name=user))    # named parameters
count = vt.Label(_n("{n} file", "{n} files", n))    # plural forms
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
3. Large lists: `vt.ListView(..., virtual=True)` draws only the visible items.
4. Many components animating at once: use `vt.stagger` to spread them over time.
5. Large images: load with `vt.Image(..., size=(w, h))`, which decodes at the reduced size.

**Tools:**

```python
vt.debug.fps(True)              # frame counter in the corner of the screen
vt.debug.regions(True)          # paints the regions redrawn each frame
vt.debug.layers(True)           # outlines every component and window
vt.debug.slow_motion(5)         # ALL animations 5x slower: great for tuning motion
vt.debug.animations()           # lists the animations running right now
vt.debug.pseudo_language(True)  # shows "[Šåvé]"-style texts to find strings missing _()
```

Global system shortcut: **Ctrl+Alt+Shift+D** opens the compositor debug panel.

---

## 19. Complete examples

### 19.1 Card with hover, glow and click

```python
from opaline import vitrum as vt
from opaline.i18n import _

app = vt.App(_("Cards"))
window = vt.Window(_("Cards"), width=520, height=320)

def make_card(title, text):
    return vt.Card(
        title=title,
        content=vt.Label(text, wrap=True, style="caption"),
        width=220,
        material=vt.Material.glass(opacity=0.6),
        border=vt.Border(1, "#FFFFFF1A"),
        states={
            "hover": vt.Style(offset=(0, -4), shadow=vt.theme.shadow("s3"),
                              border=vt.Border(1, vt.theme.color("accent")),
                              glow=vt.Glow(vt.theme.color("accent"), 18, 0.35)),
            "pressed": vt.Style(scale=0.98),
        },
        transition=vt.Transition(160, vt.curves.smooth),
        on_click=lambda: print(str(title)),
    )

window.add(vt.Row([
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
    toast = vt.Toast(text, kind="success", anchor="bottom_right", margin=16)
    stack.add(toast)
    await vt.animate(toast, curve=vt.curves.spring(stiffness=420, damping=30),
                     from_=dict(offset=(0, 40), opacity=0),
                     offset=(0, 0), opacity=1)
    await asyncio.sleep(3)
    await vt.animate(toast, 180, vt.curves.ease_in, offset=(40, 0), opacity=0)
    stack.remove(toast, animate=False)

# app.task(notify(stack, _("File saved")))
```

### 19.3 List with staggered entrance

```python
column = vt.Column(spacing=8, animate_layout=vt.Transition(200))
for name in [_("Desktop"), _("Documents"), _("Downloads"), _("Music"), _("Pictures")]:
    column.add(vt.Card(name, opacity=0), animate=False)

vt.stagger(column.children, interval=45,
           make=lambda c: vt.Anim(c, 260, vt.curves.ease_out,
                                  from_=dict(opacity=0, offset=(0, 10)),
                                  opacity=1, offset=(0, 0))).start()
```

### 19.4 User-controlled transparency

```python
window = vt.Window(_("Glass"), width=420, height=220)
tint = vt.Slider(value=0.72, min=0.2, max=1.0)
blur = vt.Slider(value=24, min=0, max=64, step=1)
overall = vt.Slider(value=1.0, min=0.4, max=1.0)

def update(_value=None):
    window.material = vt.Material.glass(opacity=tint.value, blur=int(blur.value))
    window.opacity = overall.value

for s in (tint, blur, overall):
    s.on_change(update)

window.transitions = {"material": vt.Transition(120), "opacity": vt.Transition(120)}
window.add(vt.Column([
    vt.Label(_("Glass opacity")), tint,
    vt.Label(_("Blur")), blur,
    vt.Label(_("Window opacity")), overall,
], spacing=8, padding=20))
window.show()
```

### 19.5 Dock icon that bounces when an app opens

```python
def bounce(icon):
    return vt.animate(icon, 600, repeat=2, keyframes=vt.Keyframes({
        0.0:  dict(offset=(0, 0)),
        0.25: dict(offset=(0, -14)),
        0.5:  dict(offset=(0, 0)),
        0.7:  dict(offset=(0, -6)),
        1.0:  dict(offset=(0, 0)),
    }, curves={0.25: vt.curves.ease_out, 0.5: vt.curves.ease_in,
               0.7: vt.curves.ease_out, 1.0: vt.curves.ease_in}))
```

### 19.6 System-wide window effects (Settings app)

```python
from opaline import vitrum as vt, system
from opaline.i18n import _

PRESETS = {
    "smooth": dict(open=vt.effects.spring(), close=vt.effects.zoom(0.94, 160),
                   minimize=vt.effects.to_dock(320)),
    "fast":   dict(open=vt.effects.fade(100), close=vt.effects.fade(80),
                   minimize=vt.effects.to_dock(180)),
    "none":   dict(open=vt.effects.none, close=vt.effects.none,
                   minimize=vt.effects.none),
}
LABELS = {"smooth": _("Smooth"), "fast": _("Fast"), "none": _("None")}

choice = vt.Dropdown(LABELS, value="smooth",          # dict: stored value -> displayed text
                     on_change=lambda key: system.effects.set(**PRESETS[key]))
```

### 19.7 Language switcher (Settings → Language & Region)

```python
from opaline import vitrum as vt, i18n
from opaline.i18n import _

languages = vt.Dropdown(
    {"en-US": "English (United States)", "pt-BR": "Português (Brasil)"},   # names shown in their own language
    value=i18n.language,
    on_change=i18n.set_language,       # every open app updates immediately
)

window.add(vt.Column([
    vt.Label(_("Language"), style="h2"),
    languages,
    vt.Label(_("Changes apply immediately to all open apps."), style="caption"),
], spacing=8, padding=20))
```

---

## 20. Quick reference

```python
from opaline import vitrum as vt
from opaline.i18n import _, _n

# animation
vt.animate(target, 200, vt.curves.smooth, from_=dict(opacity=0), opacity=1)
vt.Anim(...)  ·  vt.sequence(...)  ·  vt.parallel(...)  ·  vt.stagger(...)  ·  vt.wait(ms)
vt.Keyframes({0: {...}, 0.5: {...}, 1: {...}})
vt.curves.linear | smooth | ease_in | ease_out | ease_in_out | bezier() | spring() | bounce() | elastic() | steps()
comp.transitions = {"opacity": vt.Transition(150)}
with vt.transition(250): ...       with vt.no_animation(): ...

# windows
window.show() close() hide() minimize() restore() maximize() unmaximize() fullscreen() focus() alert()
window.effects = vt.Effects(open=, close=, minimize=, restore=, maximize=, unmaximize=,
                            fullscreen=, exit_fullscreen=, focus_in=, focus_out=,
                            drag_start=, drag_end=, alert=)
def effect(ctx): return vt.Anim(ctx.window, ...)
vt.effects.none | fade() | zoom() | slide() | spring() | to_dock() | genie() | shake()

# visuals
vt.Material.solid() | glass() | acrylic() | transparent()
vt.Shadow(x, y, blur, spread, color)  ·  vt.Glow(color, radius, intensity)  ·  vt.Border(w, color)
vt.Gradient.linear(angle, stops)  ·  vt.Gradient.radial(center, radius, stops)
vt.theme.color() radius() shadow() spacing() font() motion()

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
- Distribution: package **`opaline-vitrum`**. Import: **`from opaline import vitrum as vt`**.
  System APIs: **`from opaline import system`**. Translations: **`from opaline import i18n`**.

---

## 22. Open design questions

| Question | Options | Decide by |
|---|---|---|
| Effects running **inside** the compositor (no 50 ms round trip) | ready-made description (current) × small sandboxed scripts in the compositor | E9 |
| 3D transforms (perspective, flipping windows) | 2D only × 2D + simple perspective | E9 |
| Custom shaders/filters written by apps | no × a simple filter language executed in C | after E10 |
| Declarative UI (describe the screen as data, reactive) | imperative only (current) × optional declarative layer | E10 |
| Text theme file (`.toml`) for community themes | yes × no | E9 |
