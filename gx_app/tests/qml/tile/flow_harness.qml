import QtQuick
import QtQuick.Window
import Victron.VenusOS

// The power-flow tile alone, every scenario at the three GX sizes: layout checks (check mode) or PNGs (render mode).
Window {
	id: win
	width: 1200; height: 900; visible: true
	color: Theme.color_page_background
	readonly property string svc: "mock/com.victronenergy.switch.xregulator"
	readonly property var scen: JSON.parse(scenariosText)
	readonly property var sizes: [
		{ name: "ekrano", w: 276.48, h: 298, compact: false, grow: false },
		{ name: "5inch", w: 216, h: 214, compact: true, grow: false },
		{ name: "portrait", w: 355, h: 300, compact: false, grow: true }]
	// the cut-over rows as the layout rule gives them at today's sizes (rowsN: N rows, House included)
	readonly property var expectModes: ({
		"5inch": { rows2: "normal", rows3: "dense", rows4: "dense", rows5: "compact", rows6: "compact" },
		ekrano: { rows2: "normal", rows3: "normal", rows4: "normal", rows5: "dense", rows6: "oneline" },
		portrait: { rows2: "normal", rows4: "normal", rows6: "normal" } })
	// footnote starts by scenario and size (the tile's text, measured in the harness's Roboto 12 px)
	readonly property var expectFoot: ({
		why_dcdc: { ekrano: "DC-DC charger not measured", "5inch": "DC-DC charger not measured", portrait: "DC-DC charger not measured, so House is unknown" },
		why_altx: { ekrano: "Other alternator not measured", "5inch": "Other alternator not measured", portrait: "Other alternator not measured, so House is unknown" },
		why_multi: { ekrano: "Inverter/charger not measured", "5inch": "Inverter/charger not measured", portrait: "Inverter/charger not measured, so House is unknown" },
		why_alt: { ekrano: "Alternator not measured, so House is unknown", "5inch": "Alternator not measured", portrait: "Alternator not measured, so House is unknown" },
		why_batt: { ekrano: "No battery reading, so House is unknown", "5inch": "No battery reading", portrait: "No battery reading, so House is unknown" },
		why_unmeasured_big: { ekrano: "An unmeasured source adds at least 123.4 A", "5inch": "Unmeasured source adds 123.4 A", portrait: "An unmeasured source adds at least 123.4 A" },
		meter_agree: { ekrano: "House = DC system meter", "5inch": "Alternator target 62 A", portrait: "House = DC system meter" },
		meter_gap: { ekrano: "An unmeasured load draws 12.0 A", "5inch": "An unmeasured load draws 12.0 A", portrait: "An unmeasured load draws 12.0 A" },
		meter_gap_big: { ekrano: "An unmeasured load draws 123.4 A", "5inch": "An unmeasured load draws 123.4 A", portrait: "An unmeasured load draws 123.4 A" },
		meter_source: { ekrano: "An unmeasured source adds 11.0 A", "5inch": "An unmeasured source adds 11.0 A", portrait: "An unmeasured source adds 11.0 A" },
		meter_inside: { ekrano: "House = DC system meter − measured loads", portrait: "House = DC system meter − measured loads" },
		meter_inside_one: { ekrano: "House = DC system meter − DC loads", "5inch": "House = DC system meter − DC loads", portrait: "House = DC system meter − DC loads" },
		meter_alt_in: { ekrano: "House = DC system meter + alternator", "5inch": "House = DC system meter + sources", portrait: "House = DC system meter + alternator" },
		meter_alt_in_loads: { ekrano: "House = DC system meter + sources − loads", "5inch": "House = DC meter + sources − loads", portrait: "House = DC system meter + alternator − DC loads" },
		meter_load_out: { ekrano: "House = DC system meter", "5inch": "House = DC system meter", portrait: "House = DC system meter" },
		sum_dcload: { ekrano: "House = sources − battery − DC loads", portrait: "House = sources − battery − DC loads" } })
	readonly property var expectHouse: ({ meter_agree: "measured", meter_nobatt: "measured", meter_inside: "measured", anchor: "calculated", why_dcdc: "calculated" })
	FontMetrics { id: microMetrics; font.family: Global.fontFamily; font.pixelSize: Theme.font_size_micro }
	property int fails: 0
	property var queue: []
	property var cur: null
	function check(cond, msg) { if (cond) console.log("PASS " + msg); else { fails++; console.log("FAIL " + msg) } }
	Rectangle { anchors.fill: parent; color: Theme.color_page_background }
	Item { id: stage; x: 20; y: 20; width: 600; height: 860 }
	function walk(it, out) {
		for (const c of it.children) { if (c.kd !== undefined && c.r !== undefined) out.push(c); walk(c, out) }
		return out
	}
	function labels(it, out) {
		for (const c of it.children) { if (c.text !== undefined && c.visible && c.font !== undefined) out.push(c); labels(c, out) }
		return out
	}
	function abs(item) { const p = item.mapToItem(stage, 0, 0); return { x: p.x, y: p.y, w: item.width, h: item.height } }
	Timer { interval: 120; repeat: true; running: true; onTriggered: { if (!win.queue.length) { console.log("DONE fails=" + win.fails); Qt.quit(); return } win.queue.shift()() } }
	Component.onCompleted: {
		MockStore.put(svc + "/Flow/Ok", 1)
		for (const sz of sizes) for (const name in scen) {
			const sc = scen[name]
			queue.push(() => {
				MockStore.put(svc + "/Flow/Json", sc.json)
				const c = Qt.createComponent(pluginDir + "/XregFlow.qml")
				if (c.status !== Component.Ready) { console.log("FAIL load " + c.errorString()); fails++; Qt.quit(); return }
				cur = c.createObject(stage, { svc: svc, linkUp: true, active: sc.active, target: sc.target === null ? NaN : sc.target,
					compact: sz.compact, grow: sz.grow, animationEnabled: false, shown: true, width: sz.w, height: sz.h })
				if (sz.grow) { const t = cur; t.height = Qt.binding(() => t.implicitHeight) }
			})
			queue.push(() => {
				const t = cur, tag = name + "@" + sz.name
				const rows = walk(t, [])
				const visRows = rows.filter(r => r.visible)
				check(visRows.length === t.nRows, tag + " rows " + visRows.length + " of " + t.nRows + " (" + t.mode + ")")
				let inside = true, ordered = true, prevBottom = -1
				for (const r of visRows.sort((a, b) => a.y - b.y)) {
					if (r.y < t.rowsTop - 0.5 || r.y + Math.min(r.height, t.oneLine ? 14 : 31) > t.height - t.pad + 0.5) inside = false
					if (r.y < prevBottom - 0.5) ordered = false
					prevBottom = r.y + (t.oneLine ? 13 : 29)
				}
				check(inside, tag + " every row inside the tile (h " + Math.round(t.height) + ", pitch " + t.pitch.toFixed(1) + ")")
				check(ordered, tag + " rows do not overlap")
				const lastBottom = visRows.length ? Math.max.apply(null, visRows.map(r => r.y + (t.oneLine ? 14 : 31))) : 0
				const footY = t.height - t.pad - t.footH
				check(!t.footOn || lastBottom <= footY + 1, tag + " footnote clear of the rows (" + lastBottom.toFixed(1) + " <= " + footY.toFixed(1) + ")")
				// every visible label stays inside the tile horizontally
				const out = labels(t, []).filter(l => { const a = l.mapToItem(t, 0, 0); return a.x < -0.5 || a.x + Math.min(l.width, l.implicitWidth) > t.width + 0.5 })
				check(out.length === 0, tag + " no label past the tile edge" + (out.length ? ": " + out.map(l => l.text).join(", ") : ""))
				// nothing clipped inside a row: a two-line row's name whole and clear of its amps; a one-line row's bar
				// still a bar
				const clipped = []
				for (const r of visRows) {
					const kids = Array.from(r.children)
					const am = kids.find(k => k.template !== undefined)
					const nm = kids.find(k => k.text === t.rowLabel(r.r) && k.font !== undefined && k !== am)
					const bar = kids.find(k => k.frac !== undefined)
					if (!t.oneLine && (nm.x + nm.width > am.x - 4 || nm.implicitWidth > nm.width + 0.5))
						clipped.push(nm.text)
					if (t.oneLine && bar.width < 24)
						clipped.push(nm.text + " bar " + bar.width.toFixed(0))
				}
				check(clipped.length === 0, tag + " no row clipped" + (clipped.length ? ": " + clipped.join(", ") : ""))
				// the footnote whole, whatever it says
				const feet = labels(t, []).filter(l => l.mapToItem(t, 0, 0).y >= t.height - t.pad - t.footH - 1 && l.elide !== undefined
					&& l.elide !== Text.ElideNone && l.implicitWidth > l.width + 0.5)
				check(feet.length === 0, tag + " footnote not elided" + (feet.length ? ": " + feet.map(l => l.text).join(", ") : ""))
				// the footnote's words: whatever reason or sum it gives, it fits the tile's width whole (2026-10-29: the long
				// form where it fits, else a shorter one)
				const footLabels = labels(t, []).filter(l => l.mapToItem(t, 0, 0).y >= t.height - t.pad - t.footH - 1 && l.elide !== undefined)
				const footText = footLabels.map(l => l.text).join(" / ")
				if (t.whyText)
					check(microMetrics.advanceWidth(t.whyText) <= t.inner + 0.01, tag + " reason fits: " + t.whyText
						  + " (" + microMetrics.advanceWidth(t.whyText).toFixed(1) + " of " + t.inner.toFixed(1) + ")")
				check(microMetrics.advanceWidth(t.sumText) <= t.inner + 0.01, tag + " sum fits: " + t.sumText)
				const wantFoot = (expectFoot[name] || {})[sz.name]
				if (wantFoot !== undefined)
					check(footText.indexOf(wantFoot) === 0, tag + " footnote \"" + footText + "\" (want \"" + wantFoot + "\")")
				const wantHouse = expectHouse[name]
				if (wantHouse !== undefined) {
					const house = t.rows[t.rows.length - 1]
					check(house.c === wantHouse, tag + " House caption " + house.c)
				}
				console.log("INFO " + tag + " foot=" + footText)
				const want = (expectModes[sz.name] || {})[name]
				if (want)
					check(t.mode === want, tag + " layout " + t.mode + " (want " + want + ", " + t.nRows + " rows)")
				console.log("INFO " + tag + " mode=" + t.mode + " summary=" + t.summary)
				win.lastTile = t
				if (renderDir.length) {
					const file = renderDir + "/" + name + "_" + sz.name + "_" + scheme + ".png"
					t.grabToImage(function(res) { res.saveToFile(file) })
				}
			})
			queue.push(() => { cur.destroy(); cur = null })
		}
		queue.push(() => {
			// the expected layouts at today's sizes
			console.log("INFO layouts checked above")
		})
	}
	property var lastTile: null
}
