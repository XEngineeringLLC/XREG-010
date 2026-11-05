import QtQuick
import QtQuick.Window
import Victron.VenusOS

// The larger views: Short Term's and Long Term's charts, the Health and Wind tabs' charts, the Dashboard's chips and its
// barometer, wind, power flow and speed-performance tiles, on a stand-in home page that hosts the plugin's XregPop. An
// axis limit is set in the panel's own limit box (XregPop's sheet), found here so a test can read it, set its value and
// accept it.
Window {
	id: win
	width: 1024; height: 600; visible: true
	readonly property string svc: "mock/com.victronenergy.switch.xregulator"
	readonly property string viewsUid: "mock/com.victronenergy.settings/Settings/Xregulator/Views"
	property var page: null
	property var dash: null

	property var pop: null
	Item {
		id: home
		width: 1024; height: 600
		readonly property bool popShown: !!win.pop && win.pop.shown
		function openPop(c) { win.pop.open(c) }
	}
	Component.onCompleted: {
		pop = Qt.createComponent(pluginDir + "/XregPop.qml").createObject(home, { width: 1024, height: 600 })
	}

	function start(keepViews) {
		const views = MockStore.get(viewsUid)
		MockStore.reset()
		MockStore.put(svc + "/Connected", 1)
		MockStore.put(svc + "/Serial", "0000FEEDC0DE1234")
		MockStore.put(svc + "/Plot/YLim/Current", "-20,20")
		MockStore.put(svc + "/Plot/YLim/Volts", "11,15")
		MockStore.put(svc + "/Plot/YLim/Rpm", "0,2500")
		MockStore.put(svc + "/Plot/YLim/Temp", "26.67,93.33")
		if (keepViews && views !== undefined)
			MockStore.put(viewsUid, views)
		else
			MockStore.put(viewsUid, "")
		if (page)
			page.destroy()
		const c = Qt.createComponent(pluginDir + "/XregPlotsShort.qml")
		if (c.status !== Component.Ready) { console.log("FAIL load " + c.errorString()); return false }
		page = c.createObject(home, { svc: svc, x: 0, y: 0, width: 1024, height: 544, toolX: 14, toolY: 8, bodyY: 58, windowS: 30 })
		page.z = 0
		return true
	}
	function startLong() {
		if (page) { page.destroy(); page = null }
		const now = Math.floor(Date.now() / 1000)
		MockStore.put(svc + "/Ui/LongRange", "24h")
		MockStore.put(svc + "/Plot/Long/Range", "24h")
		MockStore.put(svc + "/Plot/Long/Fields", "battVolt,altCurr")
		MockStore.put(svc + "/Plot/Long/StartEpoch", now - 86400)
		MockStore.put(svc + "/Plot/Long/StepS", 300)
		let bv = [], ac = []
		for (let i = 0; i < 288; ++i) { bv.push((13 + i / 1000).toFixed(2)); ac.push((50 + i % 7).toFixed(1)) }
		MockStore.put(svc + "/Plot/Long/battVolt", bv.join(","))
		MockStore.put(svc + "/Plot/Long/altCurr", ac.join(","))
		const c = Qt.createComponent(pluginDir + "/XregPlotsLong.qml")
		if (c.status !== Component.Ready) { console.log("FAIL load " + c.errorString()); return false }
		page = c.createObject(home, { svc: svc, x: 0, y: 0, width: 1024, height: 544, toolX: 14, toolY: 8, bodyY: 58 })
		return true
	}
	function startDash() {
		if (page) { page.destroy(); page = null }
		const live = { AltAmps: 42.5, BattA: -3.25, BattV: 13.62, AltAmpsHi: 88.26, AltAmpsLo: 0, BattAHi: 61.34, BattALo: -40.5,
			BattVHi: 14.42, BattVLo: 12.81, Soc: 85, SocHi: 97.25, SocLo: 64, AltTemp: 60, AltTempHi: 80.5, AltTempLo: 20,
			Rpm: 1500, RpmHi: 2800, RpmLo: 0, Ignition: 1, Health: 97 }
		for (const k in live)
			MockStore.put(svc + "/Live/" + k, live[k])
		MockStore.put(svc + "/Health/Live/sessionN", 12)
		MockStore.put(svc + "/Health/Live/sessionMean", 96)
		MockStore.put(svc + "/Health/Live/sessionP10", 93)
		// three days of 10-minute barometer samples, 1010 mbar +- 6, the newest now
		let b = []
		for (let i = 0; i < 432; ++i) b.push((1010 + 6 * Math.sin(i / 40)).toFixed(1))
		MockStore.put(svc + "/History/Baro", b.join(","))
		MockStore.put(svc + "/History/BaroEnd", Math.floor(Date.now() / 1000))
		const c = Qt.createComponent(pluginDir + "/XregDash.qml")
		if (c.status !== Component.Ready) { console.log("FAIL load " + c.errorString()); return false }
		dash = c.createObject(home, { svc: svc, x: 0, y: 0, width: 1024, height: 544 })
		return true
	}

	// A fresh store for the pages below (the views kept with keepViews, as across a restart), and the page made on it
	function fresh(keepViews) {
		const views = MockStore.get(viewsUid)
		MockStore.reset()
		MockStore.put(svc + "/Connected", 1)
		MockStore.put(svc + "/Serial", "0000FEEDC0DE1234")
		MockStore.put(viewsUid, keepViews && views !== undefined ? views : "")
		if (page) { page.destroy(); page = null }
		if (dash) { dash.destroy(); dash = null }
	}
	function make(file, props) {
		const c = Qt.createComponent(pluginDir + "/" + file)
		if (c.status !== Component.Ready) { console.log("FAIL load " + c.errorString()); return null }
		return c.createObject(home, Object.assign({ svc: svc, x: 0, y: 0, width: 1024, height: 544 }, props))
	}
	function put(key, value) { MockStore.put(svc + "/" + key, value) }
	// Health: a 40 h trend, 88-93 % low and 94-98 % average, and 20 min of session dots, 90 +- 3 %, the newest graded
	// 91.5 % (estimated, a full steady run), the 5 before it ungraded
	function startHealth(keepViews) {
		fresh(keepViews)
		let t = ["eng,worst,overall"]
		for (let i = 0; i < 40; ++i)
			t.push(i + "," + (88 + i % 6) + "," + (94 + i % 5))
		put("Health/TrendCsv/0", t.join("\n"))
		put("Health/Live/engHours", 40)
		let r = ["#nowMs=5000000", "t,pct,amps,measAmps,pred,rpm,fieldPct,tempF,vbus,tSlope,state,ring"]
		for (let i = 0; i < 240; ++i) {
			const last = i === 233, off = i > 233
			const pct = last ? 91.5 : off ? 0 : 90 + 3 * Math.sin(i / 10)
			r.push([5000000 - (239 - i) * 5000, pct.toFixed(2), 80, 80, 82, 1800, 60, 160, 13.9, 0.1, off ? 3 : last ? 1 : 0, last ? 1 : 0].join(","))
		}
		put("Health/SessionCsv", r.join("\n"))
		page = make("XregPlotsHealth.qml", { toolX: 492, toolY: 8, bodyY: 58, windowH: 0, current: true })
		return !!page
	}
	// The bridge's /Plot/Wind, the whole hour: rows [tws, twa, aws, awa], dtMs apart
	function windMsg(epoch, rows, dtMs) {
		const col = j => rows.map(r => r[j]).join(",")
		return ["w1", epoch, rows.length, rows.length, 1, rows.map((r, i) => i ? dtMs : 0).join(","), col(0), col(1), col(2), col(3)].join(";")
	}
	// Wind: an hour of readings 10 s apart, apparent 14-20 kn at 30-50 degrees, true 11-15 kn from 200-240 degrees
	function startWind(keepViews, isTrue) {
		fresh(keepViews)
		let rows = []
		for (let i = 0; i < 360; ++i)
			rows.push([(13 + 2 * Math.sin(i / 20)).toFixed(1), (220 + 20 * Math.sin(i / 30)).toFixed(0),
					   (17 + 3 * Math.sin(i / 20)).toFixed(1), (40 + 10 * Math.sin(i / 30)).toFixed(0)])
		put("Plot/Wind", windMsg("pw1", rows, 10000))
		page = make("XregPlotsWind.qml", { toolX: 492, toolY: 8, bodyY: 58, windowMin: 30, trueWind: !!isTrue })
		return !!page
	}
	// The next reading on the Wind tab or the Dashboard: one more sample after the n held (an increment, as the bridge sends)
	function windStep(epoch, n, tws, twa, aws, awa) {
		put("Plot/Wind", ["w1", epoch, n + 1, 1, 0, 1000, tws, twa, aws, awa].join(";"))
	}
	// what a wind chart's streamed store holds: samples (break samples included), the newest's values, the reset count
	function windStore(ofDash) {
		const st = find(ofDash ? dash : page, c => c.store !== undefined && c.angleCols !== undefined)
		const s = st.store, n = s.t.length
		return JSON.stringify({ n: s.n, len: n - s.h, breaks: s.b.reduce((a, x) => a + x, 0), ver: st.ver,
			last: st.names.map(k => s.v[k][n - 1]) })
	}
	// The Dashboard on a fresh store: an hour of wind readings 15 s apart (apparent 15-21 kn, true 22-28 kn), six
	// power-flow rows, and the speed-performance values and curve in perfJson
	function startDash2(keepViews, perfJson) {
		fresh(keepViews)
		let rows = []
		for (let i = 0; i < 240; ++i)
			rows.push([(25 + 3 * Math.sin(i / 18)).toFixed(1), 200, (18 + 3 * Math.sin(i / 18)).toFixed(1), 40])
		put("Plot/Wind", windMsg("pd1", rows, 15000))
		put("Flow/Ok", 1)
		put("Flow/Json", JSON.stringify({ b: { a: 31.5, st: 1, from: "Shunt", lim: null }, house: 14.2, hm: 0, why: "", x: 0, fs: 120,
			rows: [{ id: "alt", k: "alt", a: 52.0, c: "" }, { id: "solar", k: "solar", a: 8.4, c: "VE.Direct" },
				   { id: "charger:280", k: "charger", a: 0, c: "Phoenix Smart IP43 Charger 12/30" },
				   { id: "inverter:20", k: "inverter", a: 12.3, c: "MultiPlus" }, { id: "dcload:1", k: "dcload", a: 2.4, c: "Fridge" }] }))
		const perf = JSON.parse(perfJson)
		for (const k in perf)
			put(k, perf[k])
		dash = make("XregDash.qml", {})
		return !!dash
	}
	function cursorOf(c) { c = obj(c); return c ? (c.cursorX === undefined ? NaN : c.cursorX) : 0 }
	function latestLine() { return page.latestLine1 }
	function setTrueWind(on) { page.trueWind = on }
	function windTile() { return find(win.contentItem, c => c.title === "Wind, last hour" && c.ptsTrue === undefined && c.axis !== undefined) }
	function windInfo() {
		const t = windTile(), pl = find(t, c => c.ptsTrue !== undefined)
		return JSON.stringify({ lo: t.axis[0], hi: t.axis[1], app: pl.pts.length, tru: pl.ptsTrue.length, appOn: t.appOn, trueOn: t.trueOn,
			ys: pl.pts.concat(pl.ptsTrue).map(p => p.y), h: pl.height })
	}
	function windPoint() { const t = windTile(); return centre(find(t, c => c.ptsTrue !== undefined)) }
	// the power-flow tiles under root: the Dashboard's, or the larger view's (pop)
	function flowsIn(root) {
		let out = []
		const walk = r => { for (const c of r.children) { if (c.rows !== undefined && c.growH !== undefined) out.push(c); walk(c) } }
		walk(root)
		return out
	}
	function flowInfo() {
		const small = flowsIn(dash)[0], big = flowsIn(pop)[0]
		return JSON.stringify({ small: small.summary, smallMode: small.mode, smallNames: small.rows.map(r => small.rowLabel(r)),
			big: big ? big.summary : "", mode: big ? big.mode : "", zoom: big ? big.scale : 0, grow: big ? big.grow : false,
			names: big ? big.rows.map(r => big.rowLabel(r)) : [], bigW: big ? big.width : 0, bigH: big ? big.height : 0,
			growH: big ? big.growH : 0 })
	}
	function flowPoint() { return centre(flowsIn(dash)[0]) }
	function perfs() {
		let out = []
		const walk = r => { for (const c of r.children) { if (c.sailBoat !== undefined && c.plotS !== undefined) out.push(c); walk(c) } }
		walk(win.contentItem)
		return out
	}
	function perfSmall() { return perfs().find(p => !p.mirror) || null }
	function perfBig() { return perfs().find(p => !!p.mirror) || null }
	function perfInfo(which) {
		const p = which === "big" ? perfBig() : perfSmall()
		if (!p) return "null"
		return JSON.stringify({ view: p.view, pinned: p.pinned, trueFrame: p.trueFrame, maxS: p.maxS, maxR: p.maxR, autoS: p.autoS,
			autoR: p.autoR, curveS: p.shown.maxS, curveR: p.shown.maxR, spd: p.lValid ? p.lSpd : 0, haveCurve: p.haveCurve,
			sweeping: p.sweeping, job: p.job !== null, k: p.k, sameShown: !!perfBig() && perfBig().shown === perfSmall().shown,
			plotS: p.plotS, w: p.width, h: p.height })
	}
	// the small tile's plot box, clear of its buttons
	function perfPoint() { const p = perfSmall(); return p.mapToItem(contentItem, p.width / 2, p.height / 2) }
	// a tile's Sailing / Motoring button i (big: the larger view's), and the larger view's Apparent / True toggle
	function perfModePoint(i, which) {
		const row = find(which === "small" ? perfSmall() : perfBig(), c => c.buttonClicked !== undefined && c.model !== undefined && c.currentIndex !== undefined)
		return row.mapToItem(contentItem, row.width * (i + 0.5) / 2, row.height / 2)
	}
	function perfFramePoint(label, which) {
		const b = find(which === "small" ? perfSmall() : perfBig(), c => c.modelData === label && c.isOn !== undefined)
		return b ? centre(b) : Qt.point(-1, -1)
	}
	// the top or bottom edge's middle of a tile's Sailing / Motoring button i or wind-frame toggle (label), in the window
	function perfEdge(which, i, label, bottom) {
		const p = which === "small" ? perfSmall() : perfBig()
		const b = label ? find(p, c => c.modelData === label && c.isOn !== undefined)
			: find(p, c => c.buttonClicked !== undefined && c.model !== undefined && c.currentIndex !== undefined)
		if (!b)
			return Qt.point(-1, -1)
		const x = label ? b.width / 2 : b.width * (i + 0.5) / 2
		return b.mapToItem(contentItem, x, bottom ? b.height : 0)
	}
	// The speed tile at w x h, its type growing up to maxK times, mirroring the Dashboard's, so in the view the
	// Dashboard's shows (perfPin): made by perfLayoutStart, read by perfLayout once it has laid out
	property var layoutTile: null
	function perfLayoutStart(w, h, maxK) {
		if (layoutTile)
			layoutTile.destroy()
		const src = perfSmall()
		layoutTile = Qt.createComponent(pluginDir + "/XregChartPerf.qml").createObject(home, { svc: svc, linkUp: true, mirror: src,
			axes: src.axes, maxK: maxK, width: w, height: h, z: -1 })
		return !!layoutTile
	}
	function perfLayoutEnd() { if (layoutTile) layoutTile.destroy(); layoutTile = null }
	function perfPin(v) { perfSmall().pinned = v }
	function perfFrame(t) { perfSmall().setFrame(t) }
	// The tile's plot (the polar's circle or the motoring plot), what the old layout gave it (a square box between the
	// corner columns, or under the top ones on a tall tile, the circle 0.42 of it), and problems: any text drawn, or
	// control, outside the tile or over another, or the circle crossing one (a label's text as drawn, not its box)
	function perfLayout() {
		const t = layoutTile
		const polar = find(t, x => x.rr !== undefined && x.cx !== undefined && x.pt !== undefined)
		const motor = find(t, x => x.padL !== undefined && x.cw !== undefined && x.ch !== undefined)
		const box = polar.parent
		const under = (x, anc) => { for (let q = x; q; q = q.parent) if (q === anc) return true; return false }
		const shown = x => { for (let q = x; q && q !== t; q = q.parent) if (!q.visible || q.opacity === 0) return false; return true }
		const rectIn = (x, w, dx) => { const p = x.mapToItem(t, dx || 0, 0); return { x: p.x, y: p.y, w: w, h: x.height } }
		const ink = x => {
			const cw = Math.min(x.contentWidth, x.wrapMode === Text.NoWrap ? 1e9 : x.width)
			const dx = x.horizontalAlignment === Text.AlignRight ? x.width - cw : x.horizontalAlignment === Text.AlignHCenter ? (x.width - cw) / 2 : 0
			return rectIn(x, cw, dx)
		}
		let tile = [], plot = []
		const walk = r => {
			for (const x of r.children) {
				if (!shown(x))
					continue
				if (x.buttonClicked !== undefined || x.isOn !== undefined) {
					tile.push(Object.assign({ n: x.isOn !== undefined ? x.modelData : "Sailing/Motoring" }, rectIn(x, x.width)))
					continue
				}
				if (typeof x.text === "string" && x.font !== undefined && x.text.length && x.contentWidth > 0)
					(under(x, box) ? plot : tile).push(Object.assign({ n: x.text.replace("\n", " / ") }, ink(x)))
				walk(x)
			}
		}
		walk(t)
		const view = t.view, problems = []
		const o = box.mapToItem(t, 0, 0)
		const c = { x: o.x + polar.cx, y: o.y + polar.cy, r: polar.rr }
		const all = tile.concat(plot)
		for (const a of all)
			if (a.x < -0.5 || a.y < -0.5 || a.x + a.w > t.width + 0.5 || a.y + a.h > t.height + 0.5)
				problems.push("\"" + a.n + "\" outside the tile")
		const hit = (a, b) => a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h
		for (let i = 0; i < tile.length; ++i) {
			for (let j = i + 1; j < tile.length; ++j)
				if (hit(tile[i], tile[j]))
					problems.push("\"" + tile[i].n + "\" over \"" + tile[j].n + "\"")
			for (const p of plot)
				if (hit(tile[i], p))
					problems.push("plot's \"" + p.n + "\" over \"" + tile[i].n + "\"")
			if (view === 0) {
				const a = tile[i], dx = Math.max(a.x - c.x, 0, c.x - a.x - a.w), dy = Math.max(a.y - c.y, 0, c.y - a.y - a.h)
				if (Math.hypot(dx, dy) < c.r + 1)
					problems.push("the circle crosses \"" + a.n + "\"")
			}
		}
		if (view === 0)
			for (const p of plot)
				if (p.n === "0° into wind" || p.n === "180°") {
					const dx = Math.max(p.x - c.x, 0, c.x - p.x - p.w), dy = Math.max(p.y - c.y, 0, c.y - p.y - p.h)
					if (Math.hypot(dx, dy) < c.r + 1)
						problems.push("the circle crosses \"" + p.n + "\"")
				}
		const colH = col => col.height
		const cols = []
		const walkC = r => { for (const x of r.children) { if (x.width === t.side && x.spacing !== undefined && !under(x, box)) cols.push(x); walkC(x) } }
		walkC(t)
		const topH = Math.max(...cols.filter(x => x.y < 1).map(colH)) + 4
		const botH = Math.max(...cols.filter(x => x.y >= 1).map(colH)) + 4
		const besideS = Math.min(t.height - 78, t.width - 20 - 2 * t.side)
		const stackedS = Math.min(t.width - 20, t.height - 78 - topH - botH)
		const oldS = Math.max(60, stackedS > besideS + 20 ? stackedS : besideS)
		return JSON.stringify({ view: view, r: c.r, cx: c.x, cy: c.y, w: t.width, h: t.height, k: t.k,
			cw: motor.cw, ch: motor.ch, oldR: 0.42 * oldS, oldCw: oldS - motor.padL - 4, oldCh: oldS - motor.padB - 4,
			texts: all.length, problems: problems })
	}
	// The small tile's "now" dot as drawn against its curve as drawn, in pixels, measured from the items on screen: the
	// dot's distance from the polar's centre and the curve's on the same ray (sailing), or the dot's height and the
	// curve's at its x (motoring; larger y is slower); with the tile's demo-mode state
	function perfDot() {
		const p = perfSmall()
		const isDot = x => x.width === 13 && x.radius === 6.5 && x.visible
		let r = { view: p.view, demo: p.demo, lSpd: p.lSpd, dotSpd: p.dotSpd, curveAtDot: p.curveAtDot, sweeping: p.sweeping,
			key: p.curveKey, readout: "" }
		const kn = find(p, x => typeof x.text === "string" && x.visible && x.template === "88.8")
		r.readout = kn ? kn.text : ""
		if (p.view === 0) {
			const polar = find(p, x => x.rr !== undefined && x.cx !== undefined && x.pt !== undefined)
			const dot = find(polar, isDot)
			if (!dot || !p.haveCurve) return JSON.stringify(r)
			const dx = dot.x + dot.width / 2 - polar.cx, dy = dot.y + dot.height / 2 - polar.cy
			const ux = dx / Math.hypot(dx, dy), uy = dy / Math.hypot(dx, dy)
			r.dotR = Math.hypot(dx, dy)
			const line = [p.shown.pts.map(q => polar.pt(q[0], q[1]))]
			if (p.shown.folded) line.push(p.shown.pts.map(q => polar.pt(-q[0], q[1])))
			let best = -1
			for (const ln of line)
				for (let i = 1; i < ln.length; ++i) {
					const ax = ln[i - 1].x - polar.cx, ay = ln[i - 1].y - polar.cy, bx = ln[i].x - polar.cx, by = ln[i].y - polar.cy
					const den = ux * (by - ay) - uy * (bx - ax)
					if (Math.abs(den) < 1e-12) continue
					const t = (ax * (by - ay) - ay * (bx - ax)) / den                    // distance along the ray
					const u = Math.abs(bx - ax) > Math.abs(by - ay) ? (t * ux - ax) / (bx - ax) : (t * uy - ay) / (by - ay)
					if (t > 0 && u >= -1e-9 && u <= 1 + 1e-9) best = Math.max(best, t)
				}
			r.curveR = best
		} else {
			const motor = find(p, x => x.padL !== undefined && x.cw !== undefined && x.ch !== undefined)
			const dot = find(motor, isDot)
			if (!dot || !p.haveCurve) return JSON.stringify(r)
			const x = dot.x + dot.width / 2
			r.dotR = dot.y + dot.height / 2
			const ln = p.shown.pts.map(q => Qt.point(motor.mx(q[0]), motor.my(q[1])))
			r.curveR = -1
			for (let i = 1; i < ln.length; ++i)
				if (ln[i - 1].x <= x && x <= ln[i].x)
					r.curveR = ln[i - 1].y + (ln[i].y - ln[i - 1].y) * (x - ln[i - 1].x) / Math.max(1e-9, ln[i].x - ln[i - 1].x)
		}
		return JSON.stringify(r)
	}
	// the big tile's axis zone (right: the RPM axis)
	function perfZone(right) {
		const z = find(perfBig(), c => c.onRight === right && c.visible && c.pressed !== undefined)
		return z ? centre(z) : Qt.point(-1, -1)
	}
	// One /Fast/Short message (see XregPlotsShort.apply): samples first..n, 100 ms apart; rows: JSON { i: { series: value } }
	function feedRows(epoch, first, n, full, rowsJson) {
		const rows = JSON.parse(rowsJson)
		const names = ["MeasuredAmps", "Bcur", "iiout", "dutyCycle", "pidAltPV", "BatteryV", "IBV", "RPM", "AlternatorTemperatureC", "chargeStage"]
		const cols = names.map(() => [])
		const dts = []
		for (let i = first; i <= n; ++i) {
			dts.push(100)
			const v = rows[i] || {}
			for (let j = 0; j < names.length; ++j)
				cols[j].push(v[names[j]] !== undefined ? v[names[j]] : "")
		}
		const msg = ["s1", epoch, n, n - first + 1, full ? "1" : "0", dts.join(",")].concat(cols.map(c => c.join(","))).join(";")
		MockStore.put(svc + "/Fast/Short", msg)
	}

	function find(root, pred) {
		for (const c of root.children) { if (pred(c)) return c; const f = find(c, pred); if (f) return f }
		if (root.contentItem && root.contentItem !== root) { const f = find(root.contentItem, pred); if (f) return f }
		return null
	}
	function charts() {
		let out = []
		const walk = r => { for (const c of r.children) { if (c.streamSalt !== undefined && c.lAx !== undefined) out.push(c); walk(c) } }
		walk(win.contentItem)
		return out
	}
	function titled(t) { return charts().find(c => c.title === t && !c.editAxes) || null }
	function small() { return titled("Current History") }
	function big() { return charts().find(c => c.editAxes) || null }
	function obj(c) {
		return c === "small" ? small() : c === "big" ? big() : c === "volts" ? titled("Battery Voltage History")
			: c === "temp" ? titled("Alternator Temperature History") : c === "longCur" ? titled("Currents")
			: c === "baro" ? titled("Barometer") : c === "trend" ? titled("Charging System Health vs Engine Hours")
			: c === "sess" ? titled("This Session") : c === "wind" ? charts().find(x => /wind$/.test(x.title) && !x.editAxes) || null
			: null
	}
	function point(c) { return centre(obj(c)) }
	function bigTitle() { const b = big(); return b ? b.title : "" }
	function ax(c, right) {
		c = obj(c)
		const a = c ? (right ? c.rAx : c.lAx) : null
		return a ? JSON.stringify({ lo: a.lo, hi: a.hi, ticks: a.ticks }) : "null"
	}
	// a streamed chart's progress: finished steps, a rebuild running, its lines' paths (static, tail) per line
	function streamState(c) {
		c = obj(c)
		return c ? JSON.stringify({ sVer: c.sVer, building: c.sBuilding, smooth: c.smooth, has: c.hasData }) : "null"
	}
	// the Dashboard wind tile's columns: count, newest value and its time (s before the newest reading), per line
	function dashWindCols() {
		const w = dash.windCols, f = c => ({ n: c.v.length, last: c.v[c.v.length - 1], lastX: c.x[c.x.length - 1], firstX: c.x[0] })
		return JSON.stringify({ app: f(w.app), tru: f(w.tru) })
	}
	function centre(item) { return item.mapToItem(contentItem, item.width / 2, item.height / 2) }
	function smallPoint() { return centre(small()) }
	function has(c) { return !!obj(c) }
	function popShown() { return pop.shown }
	function tick(label) { return find(pop, c => c.toggled !== undefined && c.text === label && c.checked !== undefined) }
	function tickState(label) { const t = tick(label); return t ? JSON.stringify({ checked: t.checked, enabled: t.enabled }) : "null" }
	function tickPoint(label) { return centre(tick(label)) }
	function tapAxis(right, isMax) { big().axisTapped(right, isMax) }
	function axisZone(right, isMax) {
		const z = find(big(), c => c.isMax === isMax && c.onRight === right && c.zoneH !== undefined)
		return z ? centre(z) : Qt.point(-1, -1)
	}
	function sheet() { return find(pop, c => c.nudge !== undefined && c.accept !== undefined) }
	function sheetInfo() {
		const d = sheet()
		const f = d ? sheetField() : null
		return d ? JSON.stringify({ title: d.title, value: d.value, suffix: d.suffix, w: d.width, h: d.height, typing: d.typing,
			field: f ? f.text : null, selected: f ? f.selectedText : null, focus: f ? f.activeFocus : null }) : "null"
	}
	function sheetField() { const d = sheet(); return d ? find(d, c => c.inputMethodHints !== undefined && c.validator !== undefined) : null }
	function sheetNumber() { const f = sheetField(); return f ? centre(f) : Qt.point(-1, -1) }
	function setSheet(v) { const d = sheet(); d.value = v; d.accept() }
	function sheetPoint(label) {
		const d = sheet()
		const b = d ? find(d, c => c.text === label && (c.clicked !== undefined || c.nudged !== undefined)) : null
		return b ? centre(b) : Qt.point(-1, -1)
	}
	// the panel: its size, whether it fits its content, and the content's own size
	function box() {
		const b = find(pop, c => c.fits !== undefined && c.maxW !== undefined)
		return JSON.stringify({ w: b.width, h: b.height, fits: b.fits, cw: b.content ? b.content.implicitWidth : -1,
			ch: b.content ? b.content.implicitHeight : -1, dim: pop.dim })
	}
	function views() { return String(MockStore.get(viewsUid) || "") }
	function writes(path) { return JSON.stringify(MockStore.writes.filter(w => w.uid === svc + path).map(w => w.value)) }
	function legendPoint(c, label) {
		c = obj(c)
		const e = find(c, x => x.ln !== undefined && x.ln.label === label && x.width > 0)
		return e ? centre(e) : Qt.point(-1, -1)
	}
	function hidden(c, key) { c = obj(c); const ln = c.lines.find(l => l.key === key); return c.isHidden(ln) }
	// the y the chart puts a value at, in plot px, and the plot's height
	function yAt(c, v, right) { c = obj(c); return c.py(v, right ? c.rAx : c.lAx) }
	function plotH(c) { c = obj(c); return Math.max(10, c.height - c.padT - c.padB) }
	function chip(l1, l2) { return find(win.contentItem, c => c.label1 === l1 && c.label2 === l2 && c.opens !== undefined) }
	function chipPoint(l1, l2) { return centre(chip(l1 || "Alt", l2 === undefined ? "Batt" : l2)) }
	function baroTile() { return find(win.contentItem, c => c.title === "Barometer" && c.days !== undefined) }
	function baroPoint() { return centre(baroTile()) }
	function baroInfo() { const t = baroTile(); return JSON.stringify({ lo: t.lo, hi: t.hi, n: t.days.map(d => d.length) }) }
	function textPoint(str) {
		const t = find(pop, c => c.visible && typeof c.text === "string" && c.text === str)
		return t ? centre(t) : Qt.point(-1, -1)
	}
	function nowS() { return Math.floor(Date.now() / 1000) }
	function texts() {
		let out = []
		const walk = r => { for (const c of r.children) { if (c.text !== undefined && typeof c.text === "string" && c.visible) out.push(c.text); walk(c) } }
		walk(pop)
		return JSON.stringify(out)
	}
}
