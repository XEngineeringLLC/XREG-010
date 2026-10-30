import QtQuick
import Victron.VenusOS

// Cloud page > Speed: the web page's sustained-speed scatter (vercel/leaderboards.js drawSpeedChart): boat length
// against best sustained speed, the theoretical hull-speed curve dashed, the frontier (no boat as short or shorter is
// faster) as a thin line with ringed dots, your boat in blue. The type filter shows only that type, plus your own boat.
// Tap a dot (or the curve) for what it is.
Item {
	id: sp

	property var boats: []          // [{ n, len, v, type, boat, you }] from /Cloud/Json speed
	property color brand
	property string filter: "overall"
	property var picked: null

	readonly property var filters: [["overall", "All"], ["sailboat", "Sailboats"], ["catamaran", "Catamarans"],
		["powerboat", "Powerboats"], ["trawler", "Trawlers"]]
	// 'sailboat' is not a boat_type: monohull + catamaran are the sailboats (SPEED_TAB_TYPES)
	readonly property var visibleBoats: {
		if (filter === "overall")
			return boats
		const types = filter === "sailboat" ? ["monohull", "catamaran"] : [filter]
		return boats.filter(b => types.indexOf(b.type) >= 0 || b.you)
	}
	readonly property var front: {
		const sorted = visibleBoats.slice().sort((a, b) => a.len - b.len)
		return sorted.filter(b => !sorted.some(o => o.len <= b.len && o.v > b.v))
	}
	function hullSpeed(l) { return 1.34 * Math.sqrt(l * 0.9) }
	// a CSS color string for the canvas, which takes strings, not QML colors with alpha
	function css(col, a) {
		return "rgba(" + Math.round(col.r * 255) + "," + Math.round(col.g * 255) + "," + Math.round(col.b * 255) + ","
			+ (a === undefined ? col.a : a) + ")"
	}
	function niceStep(range) {
		const raw = range / 5
		for (const s of [1, 2, 3, 5, 10, 20])
			if (s >= raw) return s
		return 25
	}
	// x fixed across filters (the whole fleet), y fitted to the boats shown
	readonly property var geo: {
		const pad = { top: 14, right: 16, bottom: 40, left: 36 }
		let xmin = 25, xmax = 65
		if (boats.length) {
			const lens = boats.map(b => b.len)
			xmin = Math.floor((Math.min.apply(null, lens) - 3) / 5) * 5
			xmax = Math.ceil((Math.max.apply(null, lens) + 3) / 5) * 5
			if (xmax - xmin < 10)
				xmax = xmin + 10
		}
		const maxKn = visibleBoats.length ? Math.max.apply(null, visibleBoats.map(b => b.v)) : 10
		const ystep = niceStep(Math.max(maxKn, 1) * 1.1)
		const ymax = Math.ceil(Math.max(maxKn, 1) * 1.08 / ystep) * ystep
		return { pad: pad, xmin: xmin, xmax: xmax, xstep: (xmax - xmin) > 30 ? 10 : 5, ymax: ymax, ystep: ystep }
	}
	function sx(l) { return geo.pad.left + (l - geo.xmin) / (geo.xmax - geo.xmin) * (chart.width - geo.pad.left - geo.pad.right) }
	function sy(s) { return chart.height - geo.pad.bottom - s / geo.ymax * (chart.height - geo.pad.top - geo.pad.bottom) }

	onVisibleBoatsChanged: { picked = null; chart.requestPaint() }
	onBoatsChanged: chart.requestPaint()

	XregTabs {
		id: seg
		width: Math.min(sp.width, 560)
		rowH: 36
		model: sp.filters.map(f => f[1])
		currentIndex: sp.filters.findIndex(f => f[0] === sp.filter)
		onPicked: function(index) { sp.filter = sp.filters[index][0] }
	}
	Row {
		anchors { right: parent.right; verticalCenter: seg.verticalCenter }
		visible: sp.width >= 760
		spacing: 14
		Repeater {
			model: [["Fleet", Qt.rgba(sp.brand.r, sp.brand.g, sp.brand.b, 0.3)], ["Frontier", sp.brand], ["Your boat", Theme.color_blue]]
			delegate: Row {
				required property var modelData
				spacing: 5
				Rectangle { anchors.verticalCenter: parent.verticalCenter; width: 10; height: 10; radius: 5; color: modelData[1] }
				Label { text: modelData[0]; color: Theme.color_font_secondary; font.pixelSize: Theme.font_size_caption }
			}
		}
	}

	Rectangle {
		y: seg.height + 10
		width: sp.width
		height: sp.height - y
		radius: Theme.geometry_overviewPage_widget_radius
		color: Theme.color_background_secondary

		Canvas {
			id: chart
			anchors { fill: parent; margins: 10; bottomMargin: 34 }
			readonly property color themeKey: Theme.color_background_secondary   // a light/dark switch repaints
			onThemeKeyChanged: requestPaint()
			onWidthChanged: requestPaint()
			onHeightChanged: requestPaint()
			onPaint: {
				const c = getContext("2d")
				const g = sp.geo
				c.reset()
				const grid = sp.css(Theme.color_card_separator), label = sp.css(Theme.color_font_secondary)
				c.lineWidth = 1
				c.strokeStyle = grid
				for (let s = g.ystep; s <= g.ymax; s += g.ystep) {
					c.beginPath(); c.moveTo(g.pad.left, sp.sy(s)); c.lineTo(width - g.pad.right, sp.sy(s)); c.stroke()
				}
				for (let l = g.xmin + g.xstep; l <= g.xmax; l += g.xstep) {
					c.beginPath(); c.moveTo(sp.sx(l), g.pad.top); c.lineTo(sp.sx(l), height - g.pad.bottom); c.stroke()
				}
				c.fillStyle = label
				c.font = Theme.font_size_micro + "px sans-serif"
				c.textAlign = "center"; c.textBaseline = "top"
				for (let l = g.xmin; l <= g.xmax; l += g.xstep)
					c.fillText(String(l), sp.sx(l), height - g.pad.bottom + 6)
				c.fillText("Length (ft)", g.pad.left + (width - g.pad.left - g.pad.right) / 2, height - 16)
				c.textAlign = "right"; c.textBaseline = "middle"
				for (let s = g.ystep; s <= g.ymax; s += g.ystep)
					c.fillText(String(s), g.pad.left - 7, sp.sy(s))
				c.textAlign = "left"; c.textBaseline = "bottom"
				c.fillText("kn", g.pad.left + 4, sp.sy(g.ymax) - 3)
				// theoretical hull speed (displacement hulls), dashed
				c.strokeStyle = sp.css(Theme.color_font_disabled); c.lineWidth = 2
				let started = false
				for (let l = g.xmin; l <= g.xmax; l += 0.5) {
					const y = sp.sy(sp.hullSpeed(l))
					if (y < g.pad.top || y > height - g.pad.bottom)
						continue
					// dashes by hand: Canvas setLineDash is not in every Qt build
					if (Math.floor(l * 2) % 4 < 2) {
						if (!started) { c.beginPath(); c.moveTo(sp.sx(l), y); started = true }
						else c.lineTo(sp.sx(l), y)
					} else if (started) {
						c.stroke(); started = false
					}
				}
				if (started) c.stroke()
				const brand = sp.css(sp.brand), surface = sp.css(Theme.color_background_secondary)
				if (sp.front.length > 1) {
					c.strokeStyle = sp.css(sp.brand, 0.35); c.lineWidth = 1.5
					c.beginPath()
					sp.front.forEach((b, i) => i === 0 ? c.moveTo(sp.sx(b.len), sp.sy(b.v)) : c.lineTo(sp.sx(b.len), sp.sy(b.v)))
					c.stroke()
				}
				const dot = (x, y, r, col) => { c.fillStyle = col; c.beginPath(); c.arc(x, y, r, 0, 2 * Math.PI); c.fill() }
				for (const b of sp.visibleBoats)
					if (!b.you && sp.front.indexOf(b) < 0)
						dot(sp.sx(b.len), sp.sy(b.v), 4, sp.css(sp.brand, 0.3))
				for (const b of sp.front)
					if (!b.you) {
						dot(sp.sx(b.len), sp.sy(b.v), 7, surface)
						dot(sp.sx(b.len), sp.sy(b.v), 5, brand)
					}
				const me = sp.visibleBoats.find(b => b.you)
				if (me) {
					const x = sp.sx(me.len), y = sp.sy(me.v)
					dot(x, y, 12, sp.css(Theme.color_blue, 0.2))
					dot(x, y, 9, surface)
					dot(x, y, 6.5, sp.css(Theme.color_blue))
					c.fillStyle = sp.css(Theme.color_blue)
					c.font = "bold " + Theme.font_size_micro + "px sans-serif"
					const flip = x > width - 60
					c.textAlign = flip ? "right" : "left"; c.textBaseline = "middle"
					c.fillText("You", x + (flip ? -13 : 13), y - 13)
				}
				if (sp.picked) {
					const pt = sp.picked.curve ? [sp.sx(sp.picked.len), sp.sy(sp.hullSpeed(sp.picked.len))] : [sp.sx(sp.picked.len), sp.sy(sp.picked.v)]
					c.strokeStyle = sp.css(Theme.color_font_primary); c.lineWidth = 2
					c.beginPath(); c.arc(pt[0], pt[1], 9, 0, 2 * Math.PI); c.stroke()
				}
			}
			MouseArea {
				anchors.fill: parent
				onClicked: (mouse) => {
					let best = null, bestD = 28
					for (const b of sp.visibleBoats) {
						const d = Math.hypot(mouse.x - sp.sx(b.len), mouse.y - sp.sy(b.v))
						if (d < bestD) { bestD = d; best = b }
					}
					if (!best && mouse.x >= sp.geo.pad.left && mouse.x <= chart.width - sp.geo.pad.right) {
						const l = sp.geo.xmin + (mouse.x - sp.geo.pad.left) / (chart.width - sp.geo.pad.left - sp.geo.pad.right) * (sp.geo.xmax - sp.geo.xmin)
						if (Math.abs(mouse.y - sp.sy(sp.hullSpeed(l))) <= 12)
							best = { curve: true, len: l }
					}
					sp.picked = best
					chart.requestPaint()
				}
			}
		}
		Label {
			anchors { left: parent.left; right: parent.right; bottom: parent.bottom; margins: 10 }
			elide: Text.ElideRight
			text: {
				const p = sp.picked
				if (!p)
					return sp.boats.length ? "Tap a dot for the boat behind it." : "No speed records yet"
				if (p.curve)
					return "Theoretical hull speed: " + sp.hullSpeed(p.len).toFixed(1) + " kn at " + Math.round(p.len) + " ft"
				return p.v.toFixed(1) + " kn · " + Math.round(p.len) + " ft · " + p.n + (p.you ? " (you)" : "") + " · " + p.type
					+ (p.boat ? " · " + p.boat : "")
			}
			color: sp.picked ? Theme.color_font_primary : Theme.color_font_secondary
			font.pixelSize: Theme.font_size_caption
		}
	}
}
