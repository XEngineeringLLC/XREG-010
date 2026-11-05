import QtQuick
import Victron.VenusOS

// Health with change-only session chart inputs (new) against the binding-fed one (old): both charts' inputs equal value
// by value through opening, live dots, the 5 s clock, rows ageing out, new session text, window taps and returns; and
// what no longer reaches the session chart. The y axes (the old page's fixed frames, now each chart's padded autoscale
// from its view) and the cursor (gone; the line under the session chart reads its newest graded dot) are left out of the
// comparison. Context: oldDir, newDir, healthText.
Item {
	id: win
	width: 1024
	height: 600
	property int fails: 0
	property int compared
	readonly property var hd: JSON.parse(healthText)
	function check(ok, msg) { if (!ok) fails++; console.log((ok ? "PASS " : "FAIL ") + msg) }

	property var steps: []
	Timer { id: stepper; interval: 1; onTriggered: win.next() }
	function after(ms, f) { steps.push({ ms: ms, f: f }) }
	function next() {
		if (!steps.length) { console.log("DONE fails=" + fails); Qt.callLater(Qt.quit); return }
		const s = steps.shift()
		try { s.f() } catch (e) { check(false, "exception " + e + " " + e.stack) }
		stepper.interval = steps.length ? Math.max(1, steps[0].ms) : 1
		stepper.start()
	}

	property var oldComp: Qt.createComponent(oldDir + "/XregPlotsHealth.qml")
	property var newComp: Qt.createComponent(newDir + "/XregPlotsHealth.qml")
	property var oldP
	property var newP
	function put(k, v) { MockStore.put("mock/" + k, v) }
	function trendFor(h) {
		let t = ["eng,worst,overall"]
		const n = h === 10 ? 30 : h === 100 ? 120 : 200
		for (let i = 0; i < n; ++i)
			t.push((i * (h || 600) / n).toFixed(2) + "," + (90 + (i * 7) % 9) + "," + (95 + (i * 3) % 5))
		return t.join("\n")
	}
	function make(comp) {
		return comp.createObject(win, { svc: "mock", width: 1024, height: 544, toolX: 492, toolY: 8, bodyY: 58, windowH: 0, current: true })
	}
	function charts(root, out) {
		out = out || []
		for (const c of root.children) {
			if (c.hiddenKeys !== undefined && c.resets !== undefined) out.push(c)
			charts(c, out)
		}
		return out
	}
	function deq(a, b, path) {
		if (typeof a === "number" || typeof b === "number")
			return Object.is(a, b) ? (compared++, "") : path + ": " + a + " vs " + b
		if (a === b)
			return ""
		if (a === null || b === null || a === undefined || b === undefined || typeof a !== typeof b)
			return path + ": " + a + " vs " + b
		if (typeof a === "object") {
			if (Array.isArray(a) !== Array.isArray(b))
				return path + ": array vs not"
			if (!Array.isArray(a) && String(a).charAt(0) === "#")
				return String(a) === String(b) ? "" : path + ": " + a + " vs " + b
			const ka = Object.keys(a).sort(), kb = Object.keys(b).sort()
			if (ka.join() !== kb.join())
				return path + ": keys " + ka.join() + " vs " + kb.join()
			for (const k of ka) {
				const r = deq(a[k], b[k], path + "." + k)
				if (r)
					return r
			}
			return ""
		}
		return a === b ? "" : path + ": " + a + " vs " + b
	}
	function compare(label) {
		const co = charts(oldP), cn = charts(newP)
		let diff = co.length === 2 && cn.length === 2 ? "" : "chart count " + co.length + "/" + cn.length
		const props = ["title", "lines", "xLo", "xHi", "xTicks", "leftUnit", "shades", "emptyText"]
		for (let i = 0; i < 2 && !diff; ++i)
			for (const p of props) {
				diff = deq(co[i][p], cn[i][p], co[i].title + "." + p)
				if (diff)
					break
			}
		if (!diff)
			diff = deq(oldP.statusText, newP.statusText, "statusText")
		// the newest graded dot in the window, read as the old page read a dot under its cursor
		const g = newP.sess.rows.filter(r => r.g && r.t >= newP.now - 1800)
		const last = g.length ? g[g.length - 1] : null
		if (!diff && (last ? !newP.latestLine1.startsWith(Math.round(last.pct) + "% · " + (last.state === 0 ? "Measured" : "Estimated"))
					  || newP.latest !== last : newP.latestLine1 !== ""))
			diff = "latestLine1: " + newP.latestLine1
		const pts = cn.reduce((s, c) => s + c.lines.reduce((t, l) => t + (l.v ? l.v.length : 0), 0), 0)
		check(diff === "", label + ": both charts' inputs equal the old page's (" + pts + " line points)" + (diff ? " (" + diff + ")" : ""))
	}
	// the clock and the session anchor read Date.now(): give the new page the old one's, so the rest compares exactly
	function align() {
		newP.sessRows = oldP.sessRows
		newP.sessAt = oldP.sessAt
		newP.liveRows = oldP.liveRows
		newP.now = oldP.now
	}
	function liveRow(t, g, pct) {
		return { t: t, g: g, pct: pct, state: g ? 0 : 3, ring: g, rpm: 1800, amps: 80, pred: 82, fieldPct: 60, vbus: 13.9, temp: 70, tSlope: 0.1 }
	}
	// session chart inputs reaching XregChart
	property var hits: ({ lines: 0, leftFixed: 0, shades: 0, xLo: 0 })
	function watch() {
		const s = charts(newP)[1]
		s.linesChanged.connect(() => win.hits.lines++)
		s.leftFixedChanged.connect(() => win.hits.leftFixed++)
		s.shadesChanged.connect(() => win.hits.shades++)
		s.xLoChanged.connect(() => win.hits.xLo++)
	}
	function zero() { win.hits = { lines: 0, leftFixed: 0, shades: 0, xLo: 0 } }

	Component.onCompleted: {
		check(oldComp.status === Component.Ready && newComp.status === Component.Ready, "components load " + oldComp.errorString() + newComp.errorString())
		for (const unit of [2, 1]) {
			after(1, () => {
				Global.systemSettings.temperatureUnit = unit
				for (const k in hd)
					put(k, hd[k])
				put("Health/TrendCsv/0", trendFor(0)); put("Health/TrendCsv/10", trendFor(10)); put("Health/TrendCsv/100", trendFor(100))
				win.oldP = make(oldComp)
				win.newP = make(newComp)
				const tc = charts(newP)
				check(tc[0].lines.length === 0 && tc[0].emptyText === "" && tc[1].lines.length === 0 && tc[1].emptyText === "",
					"unit " + unit + ": the opening turn draws neither chart, and neither shows an empty-chart text")
			})
			after(30, () => {
				const ro = oldP.sessRows, rn = newP.sessRows
				let d = ro.length === rn.length && ro.length > 300 ? "" : "rows " + ro.length + "/" + rn.length
				for (let i = 0; i < ro.length && !d; ++i) {
					const a = Object.assign({}, ro[i], { t: 0 }), b = Object.assign({}, rn[i], { t: 0 })
					d = deq(a, b, "row " + i)
					if (!d && Math.abs((ro[i].t - oldP.sessAt) - (rn[i].t - newP.sessAt)) > 1e-6)
						d = "row " + i + " time"
				}
				check(d === "", "unit " + unit + ": session text parsed to the same " + ro.length + " rows" + (d ? " (" + d + ")" : ""))
				align()
				watch()
			})
			after(5, () => compare("unit " + unit + ", opening"))
			// live dots and the clock, in the old page's two timers' order, and rows ageing out of the 30 min
			for (let k = 1; k <= 4; ++k) {
				after(1, () => {
					zero()
					const rows = oldP.liveRows.concat([liveRow(oldP.now + 3, k % 2 === 1, 95 + k)])
					oldP.liveRows = rows; newP.liveRows = rows
					check(hits.lines === 0, "live dot " + k + ": nothing reaches the session chart in the update's own turn")
				})
				after(5, () => {
					check(hits.lines === 1 && hits.leftFixed === 0 && hits.xLo === 0, "live dot " + k + ": the session chart gets its lines only, a turn later (" + JSON.stringify(hits) + ")")
					zero()
					oldP.now += 5; newP.now += 5
				})
				after(5, () => {
					check(hits.lines === 0 && hits.leftFixed === 0 && hits.xLo === 1, "clock " + k + ": the session chart gets its x range only (" + JSON.stringify(hits) + ")")
				})
				after(1, () => compare("unit " + unit + ", live dot and clock " + k))
			}
			after(1, () => { oldP.now += 600; newP.now += 600 })
			after(1, () => compare("unit " + unit + ", 10 min on: rows age out"))
			after(1, () => { oldP.windowH = 100; newP.windowH = 100 })
			after(5, () => compare("unit " + unit + ", window 100 h"))
			after(1, () => { oldP.windowH = 10; newP.windowH = 10 })
			after(5, () => compare("unit " + unit + ", window 10 h"))
			after(1, () => { put("Health/SessionCsv", hd["Health/SessionCsv"].replace("#nowMs=5000000", "#nowMs=5100000")) })
			after(5, () => { align(); })
			after(5, () => compare("unit " + unit + ", new session text"))
			// a return: no chart is reset (what a larger view sets is kept in its view), and no chart has a cursor
			after(1, () => {
				zero()
				oldP.reopen(); newP.reopen()
				check(charts(newP).every(c => Object.keys(c.hiddenKeys).length === 0 && c.tapOpens && !!c.view),
					"return: both charts open a larger view, keep their settings in a view and have no cursor")
			})
			// the new page takes the session's dots a turn after the tap
			after(5, () => align())
			after(5, () => compare("unit " + unit + ", return"))
			after(1, () => { oldP.reopen(); newP.reopen() })
			// the new page takes the session's dots a turn after the tap
			after(5, () => align())
			after(5, () => compare("unit " + unit + ", a second return"))
			after(1, () => {
				const t0 = newP.now
				newP.visible = false
			})
			after(5, () => { win.nowWas = newP.now; newP.visible = true })
			after(20, () => {
				check(newP.now === nowWas, "shown again within a second: the clock's start trigger leaves the window where it is")
				oldP.destroy(); newP.destroy()
			})
		}
		after(1, () => console.log("INFO " + compared + " numbers compared value by value"))
		stepper.start()
	}
	property real nowWas
}
