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

	property var hiddenKeys: ({})
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
	readonly property var lAx: axisFor(false, leftFixed, leftFromZero, leftCap)
	readonly property var rAx: axisFor(true, rightFixed, rightFromZero)
	readonly property bool hasData: shownLines.some(l => !l.noLegend && inWindow(l))

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
			for (let i = 0; i < l.v.length; ++i) {
				if (!(l.x[i] >= xLo && l.x[i] <= xHi))
					continue
				for (const a of [l.v, l.lo, l.hi]) {
					const q = a ? a[i] : NaN
					if (isFinite(q)) {
						if (q < lo) lo = q
						if (q > hi) hi = q
					}
				}
			}
		}
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
		for (const v of (l.v || []).concat(isFinite(l.last) ? [l.last] : []))
			if (isFinite(v)) {
				lo = Math.min(lo, v)
				mm = Math.max(mm, Math.abs(v))
			}
		return numText(lo < 0 ? -mm : mm, l)
	}
	function scrub(mx) {
		lift.stop()
		const xv = xLo + Math.max(0, Math.min(1, mx / plot.width)) * (xHi - xLo)
		ownCursor = xv
		scrubbed(xv)
	}

	readonly property var stageRuns: {
		let out = []
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
	readonly property real stripH: stages ? 14 : 0
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
		Repeater {
			model: chart.lines.length
			delegate: Shape {
				id: sh
				required property int index
				readonly property var ln: chart.lines[index] || ({})
				readonly property var ax: ln.right ? chart.rAx : chart.lAx
				readonly property color col: ln.color !== undefined ? ln.color : Theme.color_font_secondary
				anchors.fill: parent
				visible: chart.hasData && !chart.isHidden(ln)
				preferredRendererType: Shape.CurveRenderer
				ShapePath {
					strokeColor: "transparent"
					strokeWidth: 0
					fillColor: Qt.rgba(sh.col.r, sh.col.g, sh.col.b, 0.16)
					PathMultiline { paths: sh.visible && sh.ln.lo ? chart.band(sh.ln, sh.ax) : [] }
				}
				ShapePath {
					strokeColor: sh.col
					strokeWidth: sh.ln.w || 2
					strokeStyle: sh.ln.dash ? ShapePath.DashLine : ShapePath.SolidLine
					dashPattern: [4, 3]
					fillColor: "transparent"
					capStyle: ShapePath.RoundCap
					joinStyle: ShapePath.RoundJoin
					PathMultiline { paths: sh.visible ? chart.segs(sh.ln, sh.ax) : [] }
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
