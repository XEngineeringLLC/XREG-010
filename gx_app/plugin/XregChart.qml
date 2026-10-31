import QtQuick
import QtQuick.Shapes
import Victron.VenusOS

// Line chart for the Plots page, drawn with Shapes. A line is
// { key, label, unit, dec, x: [..], v: [..], lo: [..], hi: [..], color, w, dash, right, off, dots, wrap, noLegend, last }:
// NaN in v breaks the line, lo/hi draw a min/max band, dots draws one round mark per sample, wrap breaks an angle line
// where it crosses 0/360, off starts the line hidden, noLegend keeps it out of the legend, last is the value the legend
// shows when the window holds none. Tapping a legend entry hides
// or shows its line; pressing on the plot reads every line at that point (scrubbed() lets a page move the cursor on
// several charts at once).
// Streamed lines (Short Term, `stream` set): a line names its series in the page's sample store by `sk` (and may convert
// it with `conv`) instead of carrying x and v; x is seconds before the newest sample. Each line is kept in data units
// (seconds, value), one minimum and one maximum per pixel column, and placed by ShapePath.scale and its Shape's offset,
// so an update converts only its new samples and re-sends only the newest columns (the tail); lo/hi, dots and wrap are
// not drawn for streamed lines.
Item {
	id: chart

	property string title
	property var lines: []
	property real xLo: 0
	property real xHi: 1
	property var xTicks: []                 // [{ x, t }]
	property var leftFixed: null            // [lo, hi]; null fits the data in the window
	property var leftCap: null              // [lo, hi, step]: a fitted left axis that would pass either end shows all of it
	property var rightFixed: null
	property real leftStep: 0               // tick spacing for a fixed left range; 0 picks one
	property bool leftFromZero
	property bool rightFromZero
	property string leftUnit
	property string rightUnit
	property var stages: null               // { x: [..], c: [..] }: charge-stage code from each x to the next
	property var shades: []                 // [[x0, x1], ..] tinted spans
	property string emptyText: "No data"
	property string dashNote                // legend entry explaining the dashed lines
	property real ownCursor: NaN
	property real cursorX: ownCursor
	signal scrubbed(real xValue)

	// Streamed lines: the page's store { t: [ms], v: { sk: [values] }, off, h, reset, stages: [{ a, b, c }] } (absolute
	// sample i at t[i - off], held from off + h), and its update count; streamStages draws its charge-stage runs
	property var stream: null
	property int streamVer
	property bool streamStages
	property int streamSalt                 // differs per chart on a page: spreads the charts' path re-sends (sPaths)
	onStreamVerChanged: sStep()
	onHiddenKeysChanged: if (stream) sAxes()
	onXLoChanged: if (stream) sStep()
	property var ss: null                   // the streamed lines' state (sBegin), not bound
	property int sVer                       // counts finished sStep()s: what the legend and stage strip follow
	property var sAx: ({ l: null, r: null }) // their axes, replaced only when one changes
	property bool sHas
	property bool sBuilding                 // sBegin()..sBuild(): the lines are hidden
	property real sXl                      // the window's left edge in the lines' seconds (sPlace)
	// px per second and the line layer's offset (the window's left edge at 0); bound, so a plot that changes width is
	// right at once, before sStep() redraws the columns for it
	readonly property real sKx: plot.width / (xHi - xLo)
	readonly property real sOffX: -sXl * sKx
	// Streamed lines and their stage strip scroll every display frame on a steady clock instead of stepping as samples
	// arrive: the right edge shows sBehindMs before the newest sample by the samples' own clock (the regulator's), so a
	// frame that comes a little late is in before its point scrolls on. 100 ms: on the Ekrano 99% of frames came within
	// ~110 ms of the fastest, and 0.1, 0.2 and 0.3 s looked the same; 60 redraws a second cost ~14% of the box over
	// stepping (bench, 2026-10-31).
	readonly property real sBehindMs: 100
	property real sRefT: NaN                // the newest sample's time when the lines were last placed (sPlace)
	property real sLat: NaN                 // local clock minus sample clock of the fastest recent frame (sClock)
	property var sLatQ: []                  // [local ms, local - sample ms] of the last 10 s of placements
	property int sLatFor: -1                // the store reset sLatQ belongs to
	property real sLatAt
	property real sLagS                     // seconds the shown right edge trails sRefT: every frame and every placement
	readonly property bool sSmoothOn: !!stream && visible && !ScreenBlanker.blanked && sLat === sLat
	FrameAnimation {
		running: chart.sSmoothOn
		onTriggered: chart.sLagS = chart.sLag()
	}
	// a frame later than sBehindMs lets the right edge run up to 1 s past the newest sample, then it waits there
	function sLag() {
		return sLat === sLat && sRefT === sRefT ? Math.max(-1, (sRefT - (Date.now() - sLat - sBehindMs)) / 1000) : 0
	}
	// sLat: the lowest of the last 10 s; it drops at once and rises at most 20 ms a second, so a link that turns slower
	// slows the scroll a little for a while instead of stepping it back
	function sClock(s, tN) {
		const now = Date.now(), q = s.reset === sLatFor ? sLatQ : []
		q.push([now, now - tN])
		while (q[0][0] < now - 10000)
			q.shift()
		let m = Infinity
		for (const e of q)
			m = Math.min(m, e[1])
		sLat = s.reset === sLatFor && sLat === sLat ? Math.min(m, sLat + (now - sLatAt) * 0.02) : m
		sLatQ = q
		sLatFor = s.reset
		sLatAt = now
		sRefT = tN
		sLagS = sLag()
	}

	property var hiddenKeys: ({})
	// a page that keeps its charts across tab switches bumps this on return: legend and cursor as on a new chart
	property int resets
	onResetsChanged: {
		lift.stop()
		hiddenKeys = ({})
		ownCursor = NaN
	}
	readonly property var stageNames: ["", "Bulk", "Absorption", "Float", "Manual", "Maintain", "Target V", "Idle", "Commissioning", "Shutdown"]

	function isHidden(ln) {
		return hiddenKeys[ln.key] !== undefined ? hiddenKeys[ln.key] : !!ln.off
	}
	function toggle(ln) {
		let h = Object.assign({}, hiddenKeys)
		h[ln.key] = !isHidden(ln)
		hiddenKeys = h
	}
	function stageColor(c) {
		return [Theme.color_font_disabled, Theme.color_blue, Theme.color_orange, Theme.color_green, Theme.color_lpg,
			Theme.color_wasteWater, Theme.color_blackWater, Theme.color_gray4, Theme.color_oil, Theme.color_red][c] || Theme.color_font_disabled
	}

	readonly property var shownLines: lines.filter(l => !isHidden(l))
	readonly property bool hasLeft: lines.some(l => !l.right && !l.noLegend)
	readonly property bool hasRight: lines.some(l => !!l.right)
	readonly property var lAx: stream ? sAx.l : axisFor(false, leftFixed, leftFromZero, leftCap)
	readonly property var rAx: stream ? sAx.r : axisFor(true, rightFixed, rightFromZero)
	readonly property bool hasData: stream ? sHas : shownLines.some(l => !l.noLegend && inWindow(l))

	// x/v lines: the paths (most of a redraw) are worked out once, on the turn after any of their inputs change (oDraw),
	// so a page that sets lines, xLo and xHi for a new range draws them once, not three times
	readonly property var oKey: stream ? null : [lines, lAx, rAx, hasData, xLo, xHi, hiddenKeys]
	onOKeyChanged: if (!stream) oLater.restart()
	Component.onCompleted: if (!stream) oLater.restart()
	Timer {
		id: oLater
		interval: 0
		onTriggered: chart.oDraw()
	}
	function sameAx(a, b) { return a === b || !!a && !!b && a.lo === b.lo && a.hi === b.hi && a.st === b.st }
	function oDraw() {
		if (stream)
			return
		for (let k = 0; k < lines.length; ++k) {
			const sh = lineRep.itemAt(k)
			if (!sh)
				continue
			const ln = lines[k], ax = ln.right ? rAx : lAx, on = hasData && !isHidden(ln)
			sh.oLine = on ? segs(ln, ax) : []
			sh.oBand = on && ln.lo ? band(ln, ax) : []
		}
	}

	function inWindow(l) {
		if (!l.v || !l.x)
			return false
		for (let i = 0; i < l.v.length; ++i) {
			if (isFinite(l.v[i]) && l.x[i] >= xLo && l.x[i] <= xHi)
				return true
		}
		return false
	}
	function step(span) {
		const raw = span / 4
		const p = Math.pow(10, Math.floor(Math.log10(raw)))
		const f = raw / p
		// 2.5 only where it stays a whole number (25, 250 ...), so tick labels never round
		return (f <= 1 ? 1 : f <= 2 ? 2 : f <= 2.5 && p >= 10 ? 2.5 : f <= 5 ? 5 : 10) * p
	}
	function mk(lo, hi, fixedStep) {
		const st = fixedStep > 0 ? fixedStep : step(hi - lo)
		let t = []
		for (let k = Math.ceil(lo / st - 1e-9); k * st <= hi + st * 1e-6; ++k)
			t.push(k * st)
		return { lo: lo, hi: hi, ticks: t, st: st }
	}
	function axisFor(right, fixed, fromZero, cap) {
		if (fixed)
			return mk(fixed[0], fixed[1], right ? 0 : leftStep)
		let lo = Infinity, hi = -Infinity
		for (const l of shownLines) {
			if (!!l.right !== right || !l.v || !l.x)
				continue
			const v = l.v, a = l.lo, b = l.hi
			for (let i = 0; i < v.length; ++i) {
				if (!(l.x[i] >= xLo && l.x[i] <= xHi))
					continue
				let q = v[i]
				if (isFinite(q)) {
					if (q < lo) lo = q
					if (q > hi) hi = q
				}
				q = a ? a[i] : NaN
				if (isFinite(q)) {
					if (q < lo) lo = q
					if (q > hi) hi = q
				}
				q = b ? b[i] : NaN
				if (isFinite(q)) {
					if (q < lo) lo = q
					if (q > hi) hi = q
				}
			}
		}
		return fitAxis(lo, hi, fromZero, cap)
	}
	// lo..hi of the readings in the window (lo > hi: none) -> a rounded axis
	function fitAxis(lo, hi, fromZero, cap) {
		if (lo > hi)
			return null
		if (fromZero) {
			lo = Math.min(lo, 0)
			hi = Math.max(hi, 0)
		}
		if (hi - lo < 1e-6) {
			// every reading equal: open the axis around it, but a zero-based axis (a speed, RPM) only upward from 0
			if (!fromZero)
				lo -= 1
			hi += 1
		}
		const st = step(hi - lo)
		lo = Math.floor(lo / st + 1e-9) * st
		hi = Math.ceil(hi / st - 1e-9) * st
		if (cap && (lo < cap[0] || hi > cap[1]))
			return mk(cap[0], cap[1], cap[2])
		return mk(lo, hi)
	}
	function tickText(v, st) {
		return v.toFixed(Math.max(0, Math.ceil(-Math.log10(st) - 1e-9)))
	}
	function px(xv) { return (xv - xLo) / (xHi - xLo) * plot.width }
	function py(v, ax) { return plot.height * (1 - (v - ax.lo) / (ax.hi - ax.lo)) }

	function segs(l, ax) {
		let out = []
		if (!ax || !l.v || !l.x || !(xHi > xLo))
			return out
		let cur = []
		for (let i = 0; i < l.v.length; ++i) {
			const v = l.v[i]
			const brk = !isFinite(v) || (l.wrap && cur.length > 0 && Math.abs(v - l.v[i - 1]) > 180)
			if (brk && cur.length) {
				out.push(cur)
				cur = []
			}
			if (!isFinite(v))
				continue
			const p = Qt.point(px(l.x[i]), py(v, ax))
			if (l.dots)
				out.push([p, Qt.point(p.x + 0.5, p.y)])
			else
				cur.push(p)
		}
		if (cur.length)
			out.push(cur)
		// a lone sample gets a short stub so its round cap shows as a dot
		return out.map(s => s.length > 1 ? s : [s[0], Qt.point(s[0].x + 0.5, s[0].y)])
	}
	function band(l, ax) {
		let out = []
		if (!ax || !l.lo || !l.hi || !l.x || !(xHi > xLo))
			return out
		let top = [], bot = []
		const n = Math.min(l.lo.length, l.hi.length, l.x.length)
		for (let i = 0; i <= n; ++i) {
			if (i < n && isFinite(l.lo[i]) && isFinite(l.hi[i])) {
				top.push(Qt.point(px(l.x[i]), py(l.hi[i], ax)))
				bot.push(Qt.point(px(l.x[i]), py(l.lo[i], ax)))
			} else if (top.length) {
				if (top.length > 1)
					out.push(top.concat(bot.reverse(), [top[0]]))
				top = []
				bot = []
			}
		}
		return out
	}
	// Value under the cursor (nearest sample), else the newest sample in the window
	function valueAt(l) {
		if (stream)
			return sValueAt(l)
		if (!l.v || !l.x)
			return NaN
		let best = NaN, bestD = Infinity
		for (let i = 0; i < l.v.length; ++i) {
			if (!isFinite(l.v[i]) || l.x[i] < xLo || l.x[i] > xHi)
				continue
			if (isNaN(cursorX)) {
				best = l.v[i]
			} else {
				const dd = Math.abs(l.x[i] - cursorX)
				if (dd < bestD) {
					bestD = dd
					best = l.v[i]
				}
			}
		}
		return best
	}
	function numText(v, l) {
		const u = l.unit || ""
		const t = Math.abs(v).toFixed(l.dec !== undefined ? l.dec : 1)
		return (v < 0 && Number(t) !== 0 ? "\u2212" : "") + t + (u === "" || u === "%" || u === "°" ? u : " " + u)
	}
	// the line's own `last` (its newest reading outside the window, where a page has one) before a dash
	function valueText(l) {
		const v = valueAt(l)
		return isFinite(v) ? numText(v, l) : isFinite(l.last) ? numText(l.last, l) : "--"
	}
	// The widest value a legend entry can show on its axis, so the entries after it never move as the value changes
	function valueTemplate(l) {
		const ax = l.right ? rAx : lAx
		const m = ax ? Math.max(Math.abs(ax.lo), Math.abs(ax.hi)) : 0
		let lo = ax ? ax.lo : 0, mm = m
		if (stream) {
			const w = sVer >= 0 && ss ? ss.w[l.key] : null
			if (w && w.n) {
				lo = Math.min(lo, w.lo)
				mm = Math.max(mm, Math.abs(w.lo), Math.abs(w.hi))
			}
			return numText(lo < 0 ? -mm : mm, l)
		}
		const vs = l.v || []
		for (let i = 0; i < vs.length; ++i) {
			const v = vs[i]
			if (isFinite(v)) {
				lo = Math.min(lo, v)
				mm = Math.max(mm, Math.abs(v))
			}
		}
		if (isFinite(l.last)) {
			lo = Math.min(lo, l.last)
			mm = Math.max(mm, Math.abs(l.last))
		}
		return numText(lo < 0 ? -mm : mm, l)
	}
	function scrub(mx) {
		lift.stop()
		const xv = xLo + Math.max(0, Math.min(1, mx / plot.width)) * (xHi - xLo)
		ownCursor = xv
		scrubbed(xv)
	}

	// ---- Streamed lines (see the top). Per line: entries e (data-unit points, null = a break) of closed runs, an open run
	// o (the samples since, in the newest column: drawn but not yet an entry), and the points' minimum and maximum queues
	// (nt/nv, xt/xv) for the window's range. e[s0..sEnd) is the static path; the rest and the open run, the tail, go each
	// update.
	readonly property int sTailMax: 64      // entries the tail collects before they join the static path
	// first array index of stream s at or after time ms
	function sFind(s, ms) {
		let a = s.h, b = s.t.length
		while (a < b) {
			const m = (a + b) >> 1
			if (s.t[m] < ms)
				a = m + 1
			else
				b = m
		}
		return a
	}
	function sStep() {
		const s = stream
		if (!s) {
			ss = null
			return
		}
		const span = xHi - xLo
		if (!(plot.width > 0) || !(span > 0))
			return
		const st = ss, n = s.t.length
		// a few pixels of width (the axis labels growing) only stretch the columns a little: no rebuild for that
		if (!st || st.reset !== s.reset || st.lines !== lines || st.span !== span || Math.abs(plot.width - st.W) > 0.1 * st.W
				|| st.i < s.off + s.h) {
			sBegin(true)
		} else if (st.building) {
			return            // sBuild() takes the new samples too
		} else if (n > s.h && s.t[n - 1] > st.rebaseAt) {
			sBegin(false)
		} else if (s.off + n > st.i) {
			sConsume(st, s, st.i, s.off + n)
			st.i = s.off + n
			for (let k = 0; k < lines.length; ++k)
				sPaths(st, k, false)
			sPlace(st)
			sAxes()
			sVer++
		}
	}
	// A new history, window or width: the lines are built again from the samples, a few milliseconds at a time so a
	// tap is never held long (sBuild), hidden until done. hide false (the offsets grown large over a long session): the
	// old lines stay, still, until the new ones replace them all at once.
	function sBegin(hide) {
		const s = stream, n = s.t.length, span = xHi - xLo, W = plot.width
		const st = { reset: s.reset, lines: lines, span: span, W: W, cw: span * 1000 / W, kx: W / span, base: 0,
					 i: s.off + n, rebaseAt: Infinity, L: [], w: ss && !hide ? ss.w : ({}), building: true, hide: hide }
		if (n > s.h) {
			// from the sample before the window's left edge; columns counted from it
			const j = Math.max(s.h, sFind(s, s.t[n - 1] + xLo * 1000) - 1)
			st.base = s.t[j]
			st.i = s.off + j
			st.rebaseAt = st.base + 4 * span * 1000 + 600000   // keeps the data-unit offsets small
		}
		for (let k = 0; k < lines.length; ++k)
			st.L.push({ e: [], s0: 0, sEnd: 0, col: NaN, o: { n: 0, mn: 0, mx: 0, tn: 0, tx: 0, tl: 0 }, vb: NaN,
						nt: [], nv: [], nh: 0, xt: [], xv: [], xh: 0 })
		st.k = hide ? 0 : lines.length   // lines whose paths are set, one per step after the samples
		ss = st
		if (hide)
			sBuilding = true
		sLater.restart()
	}
	// the next step of a build, or a step after a width change, on a later turn; a Timer, so none is left to run on a
	// chart that is gone
	Timer {
		id: sLater
		interval: 0
		onTriggered: chart.ss && chart.ss.building ? chart.sBuild() : chart.sStep()
	}
	// 3 ms of the build per animation tick (sLater; the four charts of a page run in the same tick): samples 128 at a
	// time, then one line's paths at a time
	function sBuild() {
		const st = ss, s = stream
		if (!st || !st.building || !s || st.reset !== s.reset)
			return
		if (st.i < s.off) {
			sBegin(true)      // the store dropped samples the build had not reached
			return
		}
		const t0 = Date.now()
		do {
			const end = s.off + s.t.length
			if (st.i < end) {
				const to = Math.min(end, st.i + 128)
				sConsume(st, s, st.i, to)
				st.i = to
			} else if (st.k < lines.length) {
				sPaths(st, st.k++, true)
			} else {
				st.building = false
				for (let k = 0; k < lines.length; ++k)
					sPaths(st, k, !st.hide)
				sPlace(st)
				sAxes()
				sBuilding = false
				sVer++
				return
			}
		} while (Date.now() - t0 < 3)
		sLater.restart()
	}
	// samples from..to (absolute) into every line: a break, or into the open run of its pixel column
	function sConsume(st, s, from, to) {
		const ts = s.t, off = s.off, base = st.base, cw = st.cw
		for (let k = 0; k < lines.length; ++k) {
			const ln = lines[k], L = st.L[k], vs = s.v[ln.sk], conv = ln.conv, o = L.o
			if (!vs)
				continue
			let col = L.col
			for (let i = from; i < to; ++i) {
				const a = i - off, t = ts[a]
				let v = vs[a]
				if (v - v !== 0) {          // NaN (no reading) or infinite
					if (o.n)
						sFlush(st, L)
					const e = L.e
					if (e.length && e[e.length - 1] !== null)
						e.push(null)
					continue
				}
				if (conv)
					v = conv(v)
				const c = Math.floor((t - base) / cw)
				if (c !== col) {
					if (o.n)
						sFlush(st, L)
					col = c
				}
				o.tl = t
				if (o.n === 0) {
					o.mn = o.mx = v
					o.tn = o.tx = t
					if (L.vb !== L.vb)
						L.vb = v
				} else {
					if (v < o.mn) {
						o.mn = v
						o.tn = t
					}
					if (v > o.mx) {
						o.mx = v
						o.tx = t
					}
				}
				o.n++
			}
			L.col = col
		}
	}
	// the open run's minimum and maximum in time order, a flat run's first and last sample: so a column of one or two
	// samples draws them all (one point when it is one sample)
	function sRunTx(o) { return o.mn === o.mx ? o.tl : o.tx }
	function sRunPts(st, L) {
		const o = L.o
		if (!o.n)
			return []
		const tx = sRunTx(o)
		const pn = Qt.point((o.tn - st.base) / 1000, L.vb - o.mn)
		if (o.tn === tx)
			return [pn]
		const px = Qt.point((tx - st.base) / 1000, L.vb - o.mx)
		return o.tn < tx ? [pn, px] : [px, pn]
	}
	function sFlush(st, L) {
		const o = L.o, tx = sRunTx(o)
		if (o.tn === tx) {
			sPt(st, L, o.tn, o.mn)
		} else if (o.tn < tx) {
			sPt(st, L, o.tn, o.mn)
			sPt(st, L, tx, o.mx)
		} else {
			sPt(st, L, tx, o.mx)
			sPt(st, L, o.tn, o.mn)
		}
		o.n = 0
	}
	// a point drawn, and into the line's window minimum and maximum (monotonic queues, oldest first)
	function sPt(st, L, t, v) {
		L.e.push(Qt.point((t - st.base) / 1000, L.vb - v))
		const nt = L.nt, nv = L.nv, xt = L.xt, xv = L.xv
		while (nv.length > L.nh && nv[nv.length - 1] >= v) {
			nv.pop()
			nt.pop()
		}
		nt.push(t)
		nv.push(v)
		while (xv.length > L.xh && xv[xv.length - 1] <= v) {
			xv.pop()
			xt.pop()
		}
		xt.push(t)
		xv.push(v)
	}
	// e[from..to) plus extra as polylines; a lone point gets a half-pixel stub so its round cap shows. dropLast: a lone
	// last point is left to the tail; firstDrawn: the first point ends a static polyline already
	function sLines(st, e, from, to, extra, dropLast, firstDrawn) {
		let out = [], cur = [], i0 = from
		const end = (lone, start) => {
			if (cur.length === 1) {
				if (lone && dropLast || start === from && firstDrawn)
					return
				cur.push(Qt.point(cur[0].x + 0.5 / st.kx, cur[0].y))
			}
			if (cur.length)
				out.push(cur)
		}
		for (let i = from; i < to; ++i) {
			const p = e[i]
			if (p === null) {
				end(false, i0)
				cur = []
				i0 = i + 1
			} else {
				cur.push(p)
			}
		}
		for (let i = 0; extra && i < extra.length; ++i)
			cur.push(extra[i])
		end(!extra || !extra.length, i0)
		return out
	}
	// a line's paths after new samples (reset: everything closed so far becomes static, less a few entries that differ
	// per line and chart, so the lines' static paths are not all sent again in the same update)
	function sPaths(st, k, reset) {
		const L = st.L[k], e = L.e, sh = lineRep.itemAt(k)
		if (reset || e.length - L.sEnd >= sTailMax) {
			L.sEnd = reset ? Math.max(0, e.length - (k * 11 + streamSalt) % (sTailMax - 8)) : e.length
			const xl = (stream.t[stream.t.length - 1] - st.base) / 1000 + xLo
			let j = L.s0
			while (j < L.sEnd && (e[j] === null || e[j].x < xl))
				++j
			L.s0 = Math.max(L.s0, j - 1)
			if (L.s0 > 1024) {
				e.splice(0, L.s0)
				L.sEnd -= L.s0
				L.s0 = 0
			}
			if (sh)
				sh.sPaths = sLines(st, e, L.s0, L.sEnd, null, true, false)
		}
		if (!sh)
			return
		sh.sVb = isNaN(L.vb) ? 0 : L.vb
		const from = Math.max(L.s0, L.sEnd - 1)
		// the static path's last point starts the tail; drawn there already when it was not alone
		const drawn = from === L.sEnd - 1 && e[from] !== null && from > L.s0 && e[from - 1] !== null
		sh.tPaths = sLines(st, e, from, e.length, sRunPts(st, L), false, drawn)
	}
	function sPlace(st) {
		const s = stream, n = s.t.length
		sXl = n > s.h ? (s.t[n - 1] - st.base) / 1000 + xLo : 0
		if (n > s.h)
			sClock(s, s.t[n - 1])
	}
	// the window's range per line, then the axes over the lines shown
	function sAxes() {
		const s = stream, st = ss, n = s.t.length
		let has = false, llo = Infinity, lhi = -Infinity, rlo = Infinity, rhi = -Infinity
		const w = {}
		if (st && n > s.h) {
			const tLo = s.t[n - 1] + xLo * 1000
			for (let k = 0; k < lines.length; ++k) {
				const ln = lines[k], L = st.L[k], o = L.o
				while (L.nh < L.nt.length && L.nt[L.nh] < tLo)
					L.nh++
				while (L.xh < L.xt.length && L.xt[L.xh] < tLo)
					L.xh++
				if (L.nh > 1024) {
					L.nt.splice(0, L.nh)
					L.nv.splice(0, L.nh)
					L.nh = 0
				}
				if (L.xh > 1024) {
					L.xt.splice(0, L.xh)
					L.xv.splice(0, L.xh)
					L.xh = 0
				}
				const any = L.nh < L.nt.length
				let lo = any ? L.nv[L.nh] : Infinity, hi = any ? L.xv[L.xh] : -Infinity, c = any ? 1 : 0
				if (o.n && o.tn >= tLo) {
					lo = Math.min(lo, o.mn)
					hi = Math.max(hi, o.mn)
					c++
				}
				if (o.n && sRunTx(o) >= tLo) {
					lo = Math.min(lo, o.mx)
					hi = Math.max(hi, o.mx)
					c++
				}
				w[ln.key] = { lo: lo, hi: hi, n: c }
				if (!c || isHidden(ln))
					continue
				if (!ln.noLegend)
					has = true
				if (ln.right) {
					rlo = Math.min(rlo, lo)
					rhi = Math.max(rhi, hi)
				} else {
					llo = Math.min(llo, lo)
					lhi = Math.max(lhi, hi)
				}
			}
			st.w = w
		}
		const la = leftFixed ? mk(leftFixed[0], leftFixed[1], leftStep) : fitAxis(llo, lhi, leftFromZero, leftCap)
		const ra = rightFixed ? mk(rightFixed[0], rightFixed[1], 0) : fitAxis(rlo, rhi, rightFromZero, null)
		if (!sameAx(la, sAx.l) || !sameAx(ra, sAx.r))
			sAx = { l: la, r: ra }
		if (sHas !== has)
			sHas = has
	}
	// the value under the cursor (the nearest reading, the earlier of two as near), else the newest in the window
	function sValueAt(l) {
		const s = stream, n = s.t.length, vs = s.v[l.sk]
		if (sVer < 0 || !vs || n <= s.h)
			return NaN
		const tN = s.t[n - 1], tLo = tN + xLo * 1000, tHi = tN + xHi * 1000
		const cv = v => l.conv ? l.conv(v) : v
		if (isNaN(cursorX)) {
			for (let i = n - 1; i >= s.h && s.t[i] >= tLo; --i) {
				if (s.t[i] <= tHi && isFinite(vs[i]))
					return cv(vs[i])
			}
			return NaN
		}
		// under the cursor: by the times on screen, which trail the newest sample by the scroll's lag
		const tR = (sRefT === sRefT ? sRefT : tN) - sLag() * 1000, cLo = tR + xLo * 1000, cHi = tR + xHi * 1000
		const i0 = sFind(s, tR + cursorX * 1000)
		let a = i0 - 1, b = i0
		while (a >= s.h && s.t[a] >= cLo && !isFinite(vs[a]))
			--a
		while (b < n && s.t[b] <= cHi && !isFinite(vs[b]))
			++b
		const okA = a >= s.h && s.t[a] >= cLo && s.t[a] <= cHi
		const okB = b < n && b >= s.h && s.t[b] >= cLo && s.t[b] <= cHi
		if (!okA && !okB)
			return NaN
		if (okA && okB)
			return cv(Math.abs((s.t[a] - tR) / 1000 - cursorX) <= Math.abs((s.t[b] - tR) / 1000 - cursorX) ? vs[a] : vs[b])
		return cv(okA ? vs[a] : vs[b])
	}
	// the stream's charge-stage runs in the window, in seconds from the lines' placed newest sample (sRefT): from the
	// first reading after 1 s before the window's left edge (the strip trails by sLagS), the last to the newest
	function sStageRuns() {
		const s = stream, n = s.t.length, out = []
		if (!s.stages || n <= s.h)
			return out
		const tN = sRefT === sRefT ? sRefT : s.t[n - 1], i0 = sFind(s, tN + xLo * 1000 - 1000)
		if (i0 >= n)
			return out
		const t0 = s.t[i0]
		for (let k = 0; k < s.stages.length; ++k) {
			const r = s.stages[k]
			if (r.c <= 0 || r.b !== null && r.b <= t0)
				continue
			out.push({ a: (Math.max(r.a, t0) - tN) / 1000, b: r.b === null ? Math.max(0, xHi) : (r.b - tN) / 1000, c: r.c })
		}
		return out
	}

	readonly property var stageRuns: {
		let out = []
		if (stream)
			return streamStages && sVer >= 0 ? sStageRuns() : out
		if (!stages || !stages.c || !stages.x)
			return out
		const c = stages.c, sx = stages.x, n = Math.min(c.length, sx.length)
		let s = 0
		for (let i = 1; i <= n; ++i) {
			if (i === n || c[i] !== c[s]) {
				if (c[s] > 0)
					out.push({ a: sx[s], b: i < n ? sx[i] : Math.max(sx[n - 1], xHi), c: c[s] })
				s = i
			}
		}
		return out
	}
	function axisLabel(ax, i, unit) {
		return tickText(ax.ticks[i], ax.st) + (i === ax.ticks.length - 1 && unit ? " " + unit : "")
	}
	function widest(ax, unit) {
		let w = ""
		if (ax) {
			for (let i = 0; i < ax.ticks.length; ++i) {
				const t = axisLabel(ax, i, unit)
				if (t.length > w.length)
					w = t
			}
		}
		return w
	}
	TextMetrics {
		id: leftMetrics
		font.family: Global.fontFamily
		font.pixelSize: Theme.font_size_micro
		text: chart.widest(chart.lAx, chart.leftUnit)
	}
	TextMetrics {
		id: rightMetrics
		font.family: Global.fontFamily
		font.pixelSize: Theme.font_size_micro
		text: chart.widest(chart.rAx, chart.rightUnit)
	}

	readonly property real padL: hasLeft ? Math.max(30, Math.ceil(leftMetrics.advanceWidth) + 10) : 8
	readonly property real padR: hasRight ? Math.max(30, Math.ceil(rightMetrics.advanceWidth) + 10) : 10
	readonly property real padT: head.height + 6
	readonly property real stripH: stages || stream && streamStages ? 14 : 0
	readonly property real padB: 20 + (stripH > 0 ? stripH + 2 : 0)

	Timer {
		id: lift
		interval: 4000
		onTriggered: {
			chart.ownCursor = NaN
			chart.scrubbed(NaN)
		}
	}

	// Title, then one legend entry per line (value under the cursor, or the newest), then the stages in view
	Flow {
		id: head
		width: chart.width
		spacing: 12
		Label {
			text: chart.title
			color: Theme.color_font_secondary
			font.pixelSize: Theme.font_size_caption
			visible: text.length > 0
		}
		Repeater {
			model: chart.lines.length
			delegate: Item {
				id: entry
				required property int index
				readonly property var ln: chart.lines[index] || ({})
				visible: !ln.noLegend
				width: entryRow.implicitWidth
				height: entryRow.implicitHeight
				opacity: chart.isHidden(ln) ? 0.4 : 1
				Row {
					id: entryRow
					spacing: 5
					Rectangle {
						anchors.verticalCenter: parent.verticalCenter
						width: 12
						height: entry.ln.dash ? 3 : 12
						radius: 2
						color: entry.ln.color !== undefined ? entry.ln.color : Theme.color_font_secondary
					}
					Label {
						text: entry.ln.label || ""
						color: Theme.color_font_secondary
						font.pixelSize: Theme.font_size_tiny
					}
					XregNum {
						template: chart.valueTemplate(entry.ln)
						text: chart.valueText(entry.ln)
						font.pixelSize: Theme.font_size_tiny
						font.bold: true
					}
				}
				MouseArea {
					anchors.fill: parent
					anchors.margins: -6
					onClicked: chart.toggle(entry.ln)
				}
			}
		}
		Row {
			visible: chart.dashNote.length > 0
			spacing: 5
			Rectangle {
				anchors.verticalCenter: parent.verticalCenter
				width: 12; height: 3; radius: 1
				color: Theme.color_font_secondary
			}
			Label {
				text: chart.dashNote
				color: Theme.color_font_secondary
				font.pixelSize: Theme.font_size_tiny
			}
		}
	}

	// Value-axis labels; each axis names its unit on its top tick
	Repeater {
		model: chart.hasData && chart.lAx && chart.hasLeft ? chart.lAx.ticks : []
		delegate: Label {
			required property real modelData
			required property int index
			x: chart.padL - width - 6
			y: plot.y + chart.py(modelData, chart.lAx) - height / 2
			visible: y > chart.padT - height && y < plot.y + plot.height
			text: chart.axisLabel(chart.lAx, index, chart.leftUnit)
			color: Theme.color_font_secondary
			font.pixelSize: Theme.font_size_micro
		}
	}
	Repeater {
		model: chart.hasData && chart.rAx ? chart.rAx.ticks : []
		delegate: Label {
			required property real modelData
			required property int index
			x: plot.x + plot.width + 6
			y: plot.y + chart.py(modelData, chart.rAx) - height / 2
			text: chart.axisLabel(chart.rAx, index, chart.rightUnit)
			color: Theme.color_font_secondary
			font.pixelSize: Theme.font_size_micro
		}
	}
	Repeater {
		model: chart.hasData ? chart.xTicks : []
		delegate: Label {
			required property var modelData
			x: Math.max(0, Math.min(chart.width - width, plot.x + chart.px(modelData.x) - width / 2))
			y: plot.y + plot.height + (chart.stripH > 0 ? chart.stripH + 2 : 0) + 3
			visible: modelData.x >= chart.xLo && modelData.x <= chart.xHi
			text: modelData.t
			color: Theme.color_font_secondary
			font.pixelSize: Theme.font_size_micro
		}
	}

	Item {
		id: plot
		x: chart.padL
		y: chart.padT
		width: Math.max(10, chart.width - chart.padL - chart.padR)
		height: Math.max(10, chart.height - chart.padT - chart.padB)
		clip: true
		// later: an axis change inside sStep() can widen the labels, and so move this
		onWidthChanged: chart.stream ? sLater.restart() : oLater.restart()
		onHeightChanged: if (!chart.stream) oLater.restart()

		Repeater {
			model: chart.hasData ? (chart.lAx && chart.hasLeft ? chart.lAx.ticks : chart.rAx ? chart.rAx.ticks : []) : []
			delegate: Rectangle {
				required property real modelData
				width: plot.width
				height: 1
				y: Math.round(chart.py(modelData, chart.lAx && chart.hasLeft ? chart.lAx : chart.rAx))
				color: Theme.color_card_separator
			}
		}
		Repeater {
			model: chart.hasData ? chart.xTicks : []
			delegate: Rectangle {
				required property var modelData
				x: Math.round(chart.px(modelData.x))
				width: 1
				height: plot.height
				color: Theme.color_card_separator
			}
		}
		Repeater {
			model: chart.shades
			delegate: Rectangle {
				required property var modelData
				x: chart.px(modelData[0])
				width: Math.max(1, chart.px(modelData[1]) - x)
				height: plot.height
				color: Qt.rgba(Theme.color_font_secondary.r, Theme.color_font_secondary.g, Theme.color_font_secondary.b, 0.12)
			}
		}
		// Streamed lines scroll by this layer's offset; their points stay where they were placed
		Item {
			id: lineLayer
			x: chart.stream ? chart.sOffX + chart.sLagS * chart.sKx : 0
			width: plot.width
			height: plot.height
			Repeater {
				id: lineRep
				model: chart.lines.length
				delegate: Shape {
					id: sh
					required property int index
					readonly property var ln: chart.lines[index] || ({})
					readonly property var ax: ln.right ? chart.rAx : chart.lAx
					readonly property color col: ln.color !== undefined ? ln.color : Theme.color_font_secondary
					// x/v lines: oLine and oBand, set by oDraw(); streamed: sPaths (all but the newest columns) and tPaths, in
					// seconds and value - sVb, set by sStep()
					property var oLine: []
					property var oBand: []
					property var sPaths: []
					property var tPaths: []
					property real sVb
					readonly property real sKy: ax ? plot.height / (ax.hi - ax.lo) : 1
					readonly property size sScale: chart.stream ? Qt.size(chart.sKx, sKy) : Qt.size(1, 1)
					// the line's own state, not the page's: a hidden tab or page keeps its paths, so a return redraws nothing
					readonly property bool drawn: chart.hasData && !chart.isHidden(ln) && !chart.sBuilding
					y: chart.stream && ax ? plot.height - (sVb - ax.lo) * sKy : 0
					width: plot.width
					height: plot.height
					visible: drawn
					// plain triangulated lines, no edge smoothing: on the Ekrano the smoothed CurveRenderer (or 4x MSAA) cost about
					// twice the CPU for the same plot and looked no better (bench, 2026-10-31); XregChartPerf and XregDash match
					preferredRendererType: Shape.GeometryRenderer
					// path processing on Qt's worker threads, off the GUI thread that handles taps; a Qt without threads (the
					// remote console's WebAssembly build) does it in place as before
					asynchronous: true
					ShapePath {
						strokeColor: "transparent"
						strokeWidth: 0
						fillColor: Qt.rgba(sh.col.r, sh.col.g, sh.col.b, 0.16)
						PathMultiline { paths: sh.drawn && !chart.stream ? sh.oBand : [] }
					}
					ShapePath {
						strokeColor: sh.col
						strokeWidth: sh.ln.w || 2
						strokeStyle: sh.ln.dash ? ShapePath.DashLine : ShapePath.SolidLine
						dashPattern: [4, 3]
						fillColor: "transparent"
						capStyle: ShapePath.RoundCap
						joinStyle: ShapePath.RoundJoin
						scale: sh.sScale
						PathMultiline { paths: sh.drawn ? (chart.stream ? sh.sPaths : sh.oLine) : [] }
					}
					ShapePath {
						strokeColor: sh.col
						strokeWidth: sh.ln.w || 2
						strokeStyle: sh.ln.dash ? ShapePath.DashLine : ShapePath.SolidLine
						dashPattern: [4, 3]
						fillColor: "transparent"
						capStyle: ShapePath.RoundCap
						joinStyle: ShapePath.RoundJoin
						scale: sh.sScale
						PathMultiline { paths: sh.drawn && chart.stream ? sh.tPaths : [] }
					}
				}
			}
		}
		Rectangle {
			visible: !isNaN(chart.cursorX) && chart.hasData
			x: Math.round(chart.px(chart.cursorX))
			width: 1
			height: plot.height
			color: Theme.color_font_primary
			opacity: 0.6
		}
		MouseArea {
			id: scrubArea
			// The swipe host (XregHome) stops paging while a finger is on the plot, so a sideways drag moves the cursor
			// and never the page; a vertical drag still scrolls a page of charts, which cancels the cursor.
			property Item host: null
			function hold(on) {
				if (on && !host) {
					for (let p = chart.parent; p; p = p.parent) {
						if (p.swipeHolds !== undefined) {
							host = p
							host.swipeHolds++
							break
						}
					}
				} else if (!on && host) {
					host.swipeHolds--
					host = null
				}
			}
			anchors.fill: parent
			onPressed: (mouse) => { hold(true); chart.scrub(mouse.x) }
			onPositionChanged: (mouse) => chart.scrub(mouse.x)
			onReleased: { hold(false); lift.restart() }
			onCanceled: { hold(false); lift.restart() }
			Component.onDestruction: hold(false)
		}
	}

	// Charge-stage strip under the plot, as in the app's mode ribbon: named where a band is wide enough
	Item {
		x: plot.x
		y: plot.y + plot.height + 2
		width: plot.width
		height: chart.stripH
		visible: chart.stripH > 0 && chart.hasData
		clip: true
		Rectangle {
			anchors.fill: parent
			color: Qt.rgba(Theme.color_font_secondary.r, Theme.color_font_secondary.g, Theme.color_font_secondary.b, 0.15)
		}
		Item {
			x: chart.stream ? chart.sLagS * chart.sKx : 0
			width: parent.width
			height: parent.height
			Repeater {
				model: chart.stageRuns
				delegate: Rectangle {
					required property var modelData
					x: chart.px(modelData.a)
					width: Math.max(1, chart.px(modelData.b) - x)
					height: parent.height
					color: chart.stageColor(modelData.c)
					Label {
						anchors.centerIn: parent
						visible: implicitWidth + 10 < parent.width
						text: chart.stageNames[modelData.c] || ""
						color: Theme.color_white
						font.pixelSize: 10
						font.bold: true
					}
				}
			}
		}
	}

	Label {
		anchors.centerIn: plot
		visible: !chart.hasData
		text: chart.emptyText
		width: Math.min(implicitWidth, plot.width)
		wrapMode: Text.WordWrap
		horizontalAlignment: Text.AlignHCenter
		color: Theme.color_font_secondary
		font.pixelSize: Theme.font_size_body1
	}
}
