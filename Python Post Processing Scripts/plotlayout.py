"""
plotlayout.py — shared window/interaction helpers for the multi-plot analyzers.

tile_figures() — tile all open matplotlib (TkAgg) figure windows into a grid so
the analyzers stop opening full-screen stacked on top of each other.
Call it once, right before plt.show(). The grid auto-sizes to the number of open
figures so none overlap:
    1-4 -> 2x2 (true quadrants),  5-6 -> 3x2,  7-9 -> 3x3,  10-12 -> 4x3.
Pass cols/rows to force a specific layout (e.g. tile_figures(cols=2, rows=2)).

enable_pan() — left-drag anywhere in a plot to pan it, with no toolbar mode to
select first. Call once per figure, after the axes exist.

Both are no-ops on a non-Tk backend / when no windows exist, so they are always
safe to call.
"""

import matplotlib.pyplot as plt


# ===== Grab-and-drag panning =====
# Left-drag pans via the Axes' own start_pan/drag_pan/end_pan, so log scales, aspect and shared axes behave.
# Opt-outs: ax.set_navigate(False) never pans (widget axes; also blocks rubber-band zoom);
# ax._pan_x_only = True pans x only (event/state strips). A press is ignored when a toolbar mode is armed
# or it lands on a draggable legend or an artist in fig._pan_block_artists.

def _pan_axes_at(fig, x, y):
    """Every navigable axes whose box contains the point — plural, because a twinx() pair
    shares one box and both must move together or the two y scales silently desync."""
    return [ax for ax in fig.axes if ax.get_navigate() and ax.bbox.contains(x, y)]


def _pan_blocked(fig, event):
    for ax in fig.axes:
        leg = ax.get_legend()
        if leg is None or not leg.get_visible():
            continue
        try:
            if leg.get_window_extent().contains(event.x, event.y):
                return True
        except Exception:
            pass
    for art in getattr(fig, "_pan_block_artists", ()):
        try:
            if art.get_window_extent(fig.canvas.get_renderer()).contains(event.x, event.y):
                return True
        except Exception:
            pass
    return False


def enable_pan(fig):
    """Left-drag to pan every navigable axes under the cursor. Hold x or y to constrain."""
    state = {"axes": []}

    def _on_press(event):
        if event.button != 1 or event.inaxes is None:
            return
        tb = getattr(fig.canvas, "toolbar", None)
        if tb is not None and getattr(tb, "mode", ""):
            return                      # toolbar zoom/pan is armed; it owns the drag
        if _pan_blocked(fig, event):
            return
        axes = _pan_axes_at(fig, event.x, event.y)
        if not axes:
            return
        for ax in axes:
            ax.start_pan(event.x, event.y, event.button)
        state["axes"] = axes

    def _on_motion(event):
        if not state["axes"]:
            return
        for ax in state["axes"]:
            key = "x" if getattr(ax, "_pan_x_only", False) else event.key
            try:
                ax.drag_pan(1, key, event.x, event.y)
            except Exception:
                pass
        fig.canvas.draw_idle()

    def _on_release(event):
        for ax in state["axes"]:
            try:
                ax.end_pan()
            except Exception:
                pass
        state["axes"] = []

    fig.canvas.mpl_connect("button_press_event", _on_press)
    fig.canvas.mpl_connect("motion_notify_event", _on_motion)
    fig.canvas.mpl_connect("button_release_event", _on_release)


def block_pan_on(fig, *artists):
    """Register artists that own their own left-drag, so a press on them never also pans."""
    cur = list(getattr(fig, "_pan_block_artists", ()))
    cur.extend(artists)
    fig._pan_block_artists = cur


def _grid_for(n):
    if n <= 1:
        return 1, 1
    if n <= 2:
        return 2, 1
    if n <= 4:
        return 2, 2
    if n <= 6:
        return 3, 2
    if n <= 9:
        return 3, 3
    if n <= 12:
        return 4, 3
    return 4, 4


def tile_figures(cols=None, rows=None, top_margin=28, bottom_margin=0, title_bar=30):
    nums = plt.get_fignums()
    if not nums:
        return
    figs = [plt.figure(n) for n in nums]
    try:
        w0 = figs[0].canvas.manager.window
        w0.update_idletasks()
        sw = w0.winfo_screenwidth()
        sh = w0.winfo_screenheight()
    except Exception:
        return  # not a Tk window manager (headless / different backend)

    if not cols or not rows:
        gc, gr = _grid_for(len(figs))
        cols = cols or gc
        rows = rows or gr

    cell_w = sw // cols
    cell_h = (sh - top_margin - bottom_margin) // rows

    for i, fig in enumerate(figs):
        c = i % cols
        r = (i // cols) % rows
        x = c * cell_w
        y = top_margin + r * cell_h
        try:
            fig.canvas.manager.window.wm_geometry(
                "%dx%d+%d+%d" % (cell_w - 12, cell_h - title_bar, x, y))
        except Exception:
            pass
