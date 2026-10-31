import QtQuick
import QtQuick.Window
import Victron.VenusOS

// Geometry checks for the 2026-10-28 5-inch layout defects, at one GX layout (pageW x pageH), for each data set:
// Speed pane (1), Plots tab and Wind tool rows (2), compact header on a pushed page (3), Save Table / Set Manual rows
// (4), the Override manually label (5). Added the second night: the motoring plot's no-data message (6), the RPM
// Table's Charge Rate and Limit by rows (7), its caption and column descriptions (8). Added 2026-10-29: every tab row's
// titles inside their tabs (9); the table caption's, descriptions' and column headings' sizes (in 8). Later that day:
// check 9 on every button row, not only tabs; the column headings on one line, inside their columns (in 8).
Window {
	id: win
	width: pageW; height: pageH; visible: true
	readonly property string svc: "mock/com.victronenergy.switch.xregulator"
	property int fails: 0
	property var sets: JSON.parse(setsText)      // [name, ...] of dataFiles
	QtObject { id: pm; function pushPage() { return null } function popPage() {} }
	TextMetrics { id: tm; font.family: Global.fontFamily }

	function check(cond, msg) { if (cond) console.log("PASS [" + layoutName + "] " + msg); else { fails++; console.log("FAIL [" + layoutName + "] " + msg) } }
	function info(msg) { console.log("INFO [" + layoutName + "] " + msg) }
	function adv(t, px) { tm.font.pixelSize = px; tm.text = t; return tm.advanceWidth }
	function find(root, pred) {
		for (const c of root.children) { if (pred(c)) return c; const f = find(c, pred); if (f) return f }
		if (root.contentItem && root.contentItem !== root) { if (pred(root.contentItem)) return root.contentItem; const f = find(root.contentItem, pred); if (f) return f }
		return null
	}
	function findAll(root, pred, out) {
		out = out || []
		for (const c of root.children) { if (pred(c) && out.indexOf(c) < 0) out.push(c); findAll(c, pred, out) }
		if (root.contentItem && root.contentItem !== root) { if (pred(root.contentItem) && out.indexOf(root.contentItem) < 0) out.push(root.contentItem); findAll(root.contentItem, pred, out) }
		return out
	}
	function rect(it) { const p = it.mapToItem(win.contentItem, 0, 0); return { x: p.x, y: p.y, w: it.width, h: it.height } }
	function overlap(a, b) { return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h }
	function r1(v) { return Math.round(v * 10) / 10 }
	// Smallest gap between a segmented row's label and its button's edge
	function segPad(row) {
		const n = row.model.length
		let pad = 1e9
		for (const m of row.model) pad = Math.min(pad, (row.width / n - adv(m.value !== undefined ? m.value : m, row.fontPixelSize)) / 2)
		return pad
	}
	function isSeg(c) { return c.model !== undefined && c.buttonClicked !== undefined && c.fontPixelSize !== undefined && c.model && c.model.length > 0 }

	property var stage: null
	property var home: null
	property var top: null
	property string dataName
	function build(name, swipeIndex, tab, push) {
		if (stage) stage.destroy()
		dataName = name
		MockStore.reset()
		const data = JSON.parse(dataFiles[name])
		for (const k in data) MockStore.put(svc + "/" + k, data[k])
		MockStore.put(svc + "/Settings/Schema", schemaText)
		stage = Qt.createQmlObject('import QtQuick; Item {}', win.contentItem)
		stage.width = pageW; stage.height = pageH
		home = Qt.createComponent(pluginDir + "/XregHome.qml").createObject(stage, { svc: svc, animationEnabled: false, width: pageW, height: pageH, setupTab: tab || 0 })
		find(home, x => x.contentModel !== undefined && x.pageInView !== undefined).setCurrentIndex(swipeIndex || 0)
		top = null
		if (push) {
			top = Qt.createComponent(pluginDir + "/XregCfgSection.qml").createObject(stage, { svc: svc, nodeId: push, title: "", home: home, width: pageW, height: pageH })
			home.visible = false
		}
	}

	// 1: Speed pane
	function speedChecks() {
		const perf = find(home, x => x.sailBoat !== undefined && x.plotS !== undefined)
		if (!perf) { check(false, dataName + ": Speed pane found"); return }
		const seg = find(perf, x => isSeg(x) && x.model[1].value === "Motoring")
		if (seg && seg.visible) {
			const pad = (seg.width / 2 - adv("Motoring", seg.fontPixelSize)) / 2
			check(pad >= 12, dataName + ": Sailing / Motoring: " + r1(pad) + " px each side of \"Motoring\" (>= 12)")
		}
		const polar = find(perf, x => x.rr !== undefined && x.cx !== undefined && x.pt !== undefined)
		const msg = find(polar, x => typeof x.text === "string" && x.text.indexOf("No sailing") === 0)
		const kn = find(polar, x => typeof x.text === "string" && x.text.indexOf("KN · ") === 0)
		if (dataName === "empty")
			check(perf.view === 0 && msg.visible, dataName + ": sailing polar with its no-data message")
		if (!msg.visible)
			return
		const m = rect(msg), k = rect(kn)
		check(!overlap(m, k), dataName + ": \"" + msg.text.replace("\n", " / ") + "\" (y " + r1(m.y) + "..." + r1(m.y + m.h) + ") clear of \""
				+ kn.text + "\" (y " + r1(k.y) + "..." + r1(k.y + k.h) + ")")
		const c = polar.mapToItem(win.contentItem, polar.cx, polar.cy)
		// the drawn text, not the label box: each line's ink width, centred
		const lines = msg.text.split("\n")
		const lh = msg.contentHeight / lines.length
		let worst = 0
		lines.forEach((t, i) => {
			const w = adv(t, msg.font.pixelSize)
			for (const y of [m.y + i * lh + 0.2 * lh, m.y + (i + 1) * lh - 0.2 * lh])
				for (const x of [m.x + (m.w - w) / 2, m.x + (m.w + w) / 2])
					worst = Math.max(worst, Math.hypot(x - c.x, y - c.y))
		})
		check(worst <= polar.rr, dataName + ": message inside the polar circle: farthest text corner " + r1(worst) + " px from the centre, radius " + r1(polar.rr))
	}

	// 2: Plots tab row and Wind tool rows
	function windChecks() {
		const plots = find(home, x => x.windTrue !== undefined && x.shortWindow !== undefined)
		// XregTabs, or a plain TabBar (the plugin before 2026-10-28's fix)
		let tabs = find(plots, x => x.layout !== undefined && x.picked !== undefined)
		let tp = 1e9, px
		if (tabs) {
			px = tabs.layout.px
			for (const t of tabs.model) tp = Math.min(tp, (tabs.width / tabs.layout.per - adv(t, px)) / 2)
		} else {
			tabs = find(plots, x => isSeg(x) && x.model[0].value === "Short Term")
			px = tabs.fontPixelSize
			tp = segPad(tabs)
		}
		check(tp >= 8, dataName + ": Plots tabs at " + px + " px: " + r1(tp) + " px each side of the widest title (>= 8)")
		const wind = find(plots, x => x.windowMin !== undefined && x.trueWind !== undefined)
		const rows = findAll(wind, isSeg)
		check(rows.length === 2, dataName + ": Wind has its frame and window rows")
		const chart = find(wind, x => x.lAx !== undefined && x.rAx !== undefined)
		for (const r of rows) {
			const pad = segPad(r)
			check(pad >= 10, dataName + ": " + r.model.map(m => m.value).join("/") + " at " + r.fontPixelSize + " px: " + r1(pad) + " px each side (>= 10)")
			const rr = rect(r)
			check(rr.x >= 0 && rr.x + rr.w <= pageW && !overlap(rr, rect(chart.parent)) && !overlap(rr, rect(tabs)),
					dataName + ": " + r.model[0].value + " row inside the page, clear of the tabs and the chart")
		}
		if (rows.length === 2)
			check(!overlap(rect(rows[0]), rect(rows[1])), dataName + ": frame and window rows do not overlap")
		if (dataName === "empty")
			check(chart.rAx && chart.rAx.lo === 0 && chart.rAx.hi > 0, dataName + ": wind speed axis, every reading 0: " + (chart.rAx ? chart.rAx.lo + " to " + chart.rAx.hi : "none"))
		else
			check(chart.rAx && chart.rAx.lo >= 0, dataName + ": wind speed axis from " + (chart.rAx ? chart.rAx.lo : "?"))
	}

	// 3: header on a pushed settings page
	function headerChecks() {
		const h = find(top, x => x.setupTabs !== undefined && x.pick !== undefined)
		const bars = findAll(h, x => isSeg(x) && x.visible)
		const sw = find(h, x => x.checkable !== undefined && x.toggle !== undefined && x.scale > 1)
		const box = sw.parent
		const s = rect(sw), b = rect(box)
		// touch area (the scaled control) and the drawn knob (the scaled indicator)
		const visH = sw.height * sw.scale, visW = sw.width * sw.scale
		const sc = sw.mapToItem(win.contentItem, sw.width / 2, sw.height / 2)    // mapping applies the scale
		const sx = sc.x - visW / 2, sy = sc.y - visH / 2
		const knob = Theme.geometry_switch_indicator_width * sw.scale
		info(dataName + ": header " + h.height + " px; rows " + bars.map(x => Math.round(x.height)).join(", ") + "; switch touch " + r1(visW) + " x " + r1(visH)
				+ ", knob " + r1(knob) + ", in a " + b.w + " x " + b.h + " box")
		if (layoutName === "FiveInch")
			check(h.height <= 80, dataName + ": 5-inch pushed header (page tabs, Alternator Enable, sub-tabs) " + h.height + " px (was 98)")
		check(bars.every(x => x.height >= 34), dataName + ": tab rows at least 34 px tall")
		check(sw.visible && sw.enabled === (MockStore.get(svc + "/Connected") === 1) && visW >= 50 && visH >= b.h - 1 && sx >= b.x
				&& sx + visW <= b.x + b.w + 1 && sy >= b.y - 1 && sy + visH <= b.y + b.h + 1 && knob <= b.h + 2,
				dataName + ": Alternator Enable switch shown, touch area " + r1(visW) + " x " + r1(visH) + " filling its box's height, knob " + r1(knob) + " px")
		const cells = findAll(top, x => x.cellKey !== undefined)
		const ys = []
		for (const c of cells) { const y = Math.round(rect(c).y); if (ys.indexOf(y) < 0) ys.push(y) }
		ys.sort((a, b) => a - b)
		const rowH = cells.length ? cells[0].height : 0
		const full = ys.filter(y => y + rowH <= pageH).length
		info(dataName + ": RPM Table rows fully shown before scrolling: " + full + " (first at y " + ys[0] + ")")
	}

	// 4 and 5: Save Table, Set Manual, Override manually
	function buttonOf(lb, text) { return find(lb, x => x.flat !== undefined && x.down !== undefined && x.text === text) }
	function saveRowChecks(saveText, refText) {
		const root = top || home
		const save = find(root, x => x.secondaryText === saveText && x.clicked !== undefined)
		const ref = find(root, x => x.secondaryText === refText && x.clicked !== undefined)
		const sb = buttonOf(save || root, saveText), rb = ref ? buttonOf(ref, refText) : null
		if (!save) {
			check(false, dataName + ": " + saveText + " is a list row of its own (a plain button before: font " + (sb ? sb.font.pixelSize : "?")
					+ " px, padding " + (sb ? sb.leftPadding : "?") + "; " + refText + "'s: " + (rb ? rb.font.pixelSize + " px, " + rb.leftPadding : "?") + ")")
			return
		}
		check(!!sb && !!rb && sb.height === rb.height && sb.font.pixelSize === rb.font.pixelSize && sb.leftPadding === rb.leftPadding
				&& sb.rightPadding === rb.rightPadding, dataName + ": " + saveText + " button as " + refText + "'s: height " + sb.height + "/" + rb.height
				+ ", font " + sb.font.pixelSize + "/" + rb.font.pixelSize + ", padding " + sb.leftPadding + "/" + rb.leftPadding)
		const card = rect(save), btn = rect(sb)
		const cardTop = card.y + save.topInset, cardBottom = card.y + save.height - save.bottomInset
		check(btn.y >= cardTop - 0.5 && btn.y + btn.h <= cardBottom + 0.5 && save.background && save.background.visible,
				dataName + ": " + saveText + " sits in its own card (" + r1(cardTop) + "..." + r1(cardBottom) + ", button " + r1(btn.y) + "..." + r1(btn.y + btn.h) + ")")
		const rowsAfter = findAll(root, x => x.horizontalContentPadding !== undefined && x.visible && x !== save
				&& rect(x).y > card.y + 1).map(rect).sort((a, b) => a.y - b.y)
		const gap = rowsAfter.length ? rowsAfter[0].y - cardBottom : 99
		check(gap >= Theme.geometry_gradientList_spacing - 0.5, dataName + ": " + gap.toFixed(1) + " px between " + saveText + "'s card and the next (>= " + Theme.geometry_gradientList_spacing + ")")
	}
	function overrideChecks() {
		const lbl = find(home, x => x.text === "Override manually" && x.visible)
		const ref = find(home, x => x.text === "Defer to Solar" && x.font !== undefined && x.width > 0 && x.contentWidth !== undefined)
		check(!!lbl && !!ref && Math.abs(rect(lbl).x - rect(ref).x) <= 1, dataName + ": \"Override manually\" at x " + (lbl ? r1(rect(lbl).x) : "?")
				+ ", the row labels at x " + (ref ? r1(rect(ref).x) : "?"))
	}

	// 6: the motoring plot's no-data message: empty data, and demo data with its motoring front removed (a live point
	// and speed readout, no curve)
	function perfPane() { return find(home, x => x.sailBoat !== undefined && x.plotS !== undefined) }
	// corner: the live point at 300 RPM and 3.9 kn, top left of the plot's empty 3600 RPM x 4 kn range, so the
	// readout moves to bottom right
	property string motorCase
	function motorPrep(corner) {
		perfPane().pinned = 1
		motorCase = dataName === "empty" ? "" : corner ? " (motoring front removed, live point top left)" : " (motoring front removed)"
		if (dataName !== "empty") {
			const t = String(MockStore.get(svc + "/Perf/CurveCsv") || "")
			const i = t.indexOf("BEFRONT1,MOTOR")
			MockStore.put(svc + "/Perf/CurveCsv", i >= 0 ? t.slice(0, i) : t)
			if (corner) {
				MockStore.put(svc + "/Perf/Motor/rpm", 300)
				MockStore.put(svc + "/Perf/Motor/spd", 3.9)
			}
		}
	}
	// a label's drawn lines, each a rect in window coordinates
	function inkLines(lbl) {
		const r = rect(lbl), lines = lbl.text.split("\n"), lh = lbl.contentHeight / lines.length
		return lines.map((t, i) => {
			const w = adv(t, lbl.font.pixelSize)
			const x = lbl.horizontalAlignment === Text.AlignHCenter ? r.x + (r.w - w) / 2 : lbl.horizontalAlignment === Text.AlignRight ? r.x + r.w - w : r.x
			return { x: x, y: r.y + i * lh, w: w, h: lh }
		})
	}
	function motorChecks() {
		const perf = perfPane()
		const motor = find(perf, x => x.padL !== undefined && x.cw !== undefined && x.ch !== undefined)
		const msg = find(motor, x => typeof x.text === "string" && x.text.indexOf("No motoring") === 0)
		const tag = dataName + motorCase
		check(perf.view === 1 && motor.visible && !!msg && msg.visible && !perf.haveCurve && motor.lowRight === motorCase.indexOf("top left") >= 0, tag + ": motoring plot with its no-data message, live point "
				+ (perf.lValid ? "shown" : "absent") + ", readout " + (motor.lowRight ? "bottom right" : "top left"))
		if (!msg || !msg.visible)
			return
		const o = motor.mapToItem(win.contentItem, 0, 0)
		const plot = { x: o.x + motor.padL, y: o.y + 4, w: motor.cw, h: motor.ch }
		const yLabels = findAll(motor, x => typeof x.text === "string" && x.contentWidth !== undefined && x.visible && x !== msg
				&& x.mapToItem(motor, 0, 0).x + x.width <= motor.padL + 0.5).map(rect)
		const ink = inkLines(msg)
		const x0 = Math.min(...ink.map(l => l.x)), x1 = Math.max(...ink.map(l => l.x + l.w))
		const y0 = Math.min(...ink.map(l => l.y)), y1 = Math.max(...ink.map(l => l.y + l.h))
		const desc = "\"" + msg.text.replace("\n", " / ") + "\" at " + msg.font.pixelSize + " px (tile scale " + r1(perf.k) + ")"
		info(tag + ": motoring plot " + r1(plot.x) + "..." + r1(plot.x + plot.w) + " x " + r1(plot.y) + "..." + r1(plot.y + plot.h)
				+ "; message text " + r1(x0) + "..." + r1(x1) + " x " + r1(y0) + "..." + r1(y1))
		check(x0 >= plot.x && x1 <= plot.x + plot.w && y0 >= plot.y && y1 <= plot.y + plot.h, tag + ": " + desc + " inside the plot: text x "
				+ r1(x0) + "..." + r1(x1) + ", plot x " + r1(plot.x) + "..." + r1(plot.x + plot.w))
		const yRight = Math.max(...yLabels.map(a => a.x + a.w))
		const hits = yLabels.filter(a => ink.some(l => overlap(l, a)))
		check(yLabels.length > 0 && hits.length === 0, tag + ": " + desc + " clear of the " + yLabels.length + " y-axis labels: text from x "
				+ r1(x0) + ", labels end at x " + r1(yRight) + (hits.length ? "; runs over " + hits.length + " of them" : ""))
		const kn = find(motor, x => typeof x.text === "string" && x.text.indexOf("KN · ") === 0)
		const ro = rect(kn.parent)
		check(!ink.some(l => overlap(l, ro)), tag + ": " + desc + " clear of the speed readout (x " + r1(ro.x) + "..." + r1(ro.x + ro.w)
				+ ", y " + r1(ro.y) + "..." + r1(ro.y + ro.h) + ")")
	}

	// 7: Charge Rate and Limit by, list rows the height of the page's other rows, everything inside their cards
	function rowOf(text) { return find(top, x => x.text === text && x.horizontalContentPadding !== undefined && x.visible) }
	function labelIn(row) { return find(row, x => x.text === row.text && x.contentWidth !== undefined && x.visible) }
	function cardOf(row) {
		const r = rect(row)
		return { x: r.x + row.leftInset, y: r.y + row.topInset, w: r.w - row.leftInset - row.rightInset, h: r.h - row.topInset - row.bottomInset }
	}
	function within(a, b) { return a.x >= b.x - 0.5 && a.y >= b.y - 0.5 && a.x + a.w <= b.x + b.w + 0.5 && a.y + a.h <= b.y + b.h + 0.5 }
	function ctlRowChecks(state) {
		// the normal row: the theme's list row and gui-v2's ListButton (Save Table); a row sized by its 22 px label
		// (Show descriptions) is 2 px taller on 5-inch, where the label's 26 px line overflows the card's 24 px inside
		const ref = find(top, x => x.secondaryText === "Save Table" && x.clicked !== undefined && x.visible)
		const sd = rowOf("Show descriptions")
		const rows = findAll(top, x => x.ctl !== undefined && !!x.ctl && x.horizontalContentPadding !== undefined && x.visible)
		const normal = Theme.geometry_listItem_height + Theme.geometry_gradientList_spacing
		check(rows.length === 2 && !!ref && Math.abs(ref.height - normal) <= 0.5, dataName + state + ": Charge Rate and Limit by drawn; a normal row "
				+ (ref ? ref.height : "?") + " px (Save Table; the theme's " + normal + "; Show descriptions " + (sd ? sd.height : "?") + ")")
		for (const row of rows) {
			const card = cardOf(row), seg = rect(find(row, isSeg)), lbl = rect(labelIn(row))
			const capItem = row.caption.length ? find(row, x => x.contentWidth !== undefined && x.visible && x.text === row.caption) : null
			if (!capItem)
				check(Math.abs(row.height - ref.height) <= 0.5, dataName + state + ": " + row.text + " row " + row.height + " px, as a normal row (" + ref.height + ")")
			else
				check(row.height > ref.height, dataName + state + ": " + row.text + " row " + row.height + " px with its caption")
			check(within(seg, card) && within(lbl, card) && (!capItem || within(rect(capItem), card)), dataName + state + ": " + row.text + "'s label, buttons"
					+ (capItem ? " and caption" : "") + " inside its " + r1(card.h) + " px card (buttons " + r1(seg.y - card.y) + " px below its top, "
					+ r1(card.y + card.h - seg.y - seg.h) + " above its bottom)")
		}
	}
	function tableRowsShown() {
		const cells = findAll(top, x => x.cellKey !== undefined)
		const ys = []
		for (const c of cells) { const y = Math.round(rect(c).y); if (ys.indexOf(y) < 0) ys.push(y) }
		ys.sort((a, b) => a - b)
		const full = ys.filter(y => y + cells[0].height <= pageH).length
		if (layoutName === "FiveInch")
			check(full >= 2, dataName + ": 5-inch RPM Table rows fully shown before scrolling: " + full + " (first at y " + ys[0] + ")")
		else
			info(dataName + ": RPM Table rows fully shown before scrolling: " + full + " (first at y " + ys[0] + ")")
	}

	// 8: the table's caption and its column descriptions start where the row labels do, and end as far from the right;
	// (2026-10-29) they are one size, gui-v2's caption size, and the column headings are no larger, each on one line (its
	// own lines only, such as "auto") and no wider than its column
	function insetChecks(state) {
		const want = rect(labelIn(rowOf("Show descriptions"))).x
		const table = find(top, x => x.tableCfg !== undefined && x.cols !== undefined)
		const labels = [find(table, x => x.text === table.tableCfg.caption && x.contentWidth !== undefined && x.visible)]
		if (table.showHelp)
			for (const c of table.cols)
				if (c.help)
					labels.push(find(table, x => x.contentWidth !== undefined && x.visible && x.text.indexOf(c.label + (c.unit ? " (" + c.unit + ")" : "") + ": ") === 0))
		check(labels.every(l => !!l), dataName + state + ": " + labels.length + " caption and description labels found")
		for (const l of labels.filter(l => !!l)) {
			const r = rect(l)
			check(Math.abs(r.x - want) <= 1 && r.x + r.w <= pageW - want + 1, dataName + state + ": \"" + l.text.slice(0, 22) + "...\" at x " + r1(r.x)
					+ "..." + r1(r.x + r.w) + ", the row labels at x " + r1(want))
		}
		const heads = table.cols.map(c => find(table, x => x.contentWidth !== undefined && x.visible && x.horizontalAlignment === Text.AlignHCenter
				&& x.text.indexOf(c.label + (c.unit ? " (" + c.unit + ")" : "")) === 0))
		const capPx = labels[0] ? labels[0].font.pixelSize : -1
		const descPx = labels.slice(1).map(l => l ? l.font.pixelSize : -1), headPx = heads.map(h => h ? h.font.pixelSize : -1)
		check(capPx === Theme.font_listItem_caption_size && descPx.every(p => p === capPx) && headPx.every(p => p > 0 && p <= capPx),
				dataName + state + ": caption " + capPx + " px (gui-v2's list caption " + Theme.font_listItem_caption_size + "), descriptions "
				+ (descPx.length ? descPx.join("/") : "off") + ", column headings " + headPx.join("/") + " (no larger than the caption)")
		const desc = h => "\"" + h.text.replace("\n", " / ") + "\" " + h.lineCount + " line" + (h.lineCount > 1 ? "s" : "") + ", " + r1(h.implicitWidth)
				+ " px wide in a " + r1(h.width) + " px column"
		const bad = heads.filter(h => h && (h.lineCount > h.text.split("\n").length || h.implicitWidth > h.width + 0.5))
		check(heads.every(h => !!h) && bad.length === 0, dataName + state + ": column headings on one line, inside their columns: "
				+ (bad.length ? bad.map(desc).join("; ") : heads.map(desc).join("; ")))
	}

	// 9: every button row (gui-v2's SegmentedButtonRow and TabBar: the page tabs, the Setup sub-tabs, the Plots and Cloud
	// tabs, every Plots tab's window and frame rows, Sailing / Motoring, Charge Rate and Limit by, the Cloud sections, board
	// tabs and boat types). Each title as drawn: the label's rendered width in its own font, placed as its alignment
	// places it, not elided, and the tab bar's margin clear of its button's border on both sides. A row drawn the same
	// (titles, font, width) is checked once per data set.
	property var tabRowsSeen: ({})
	function tabChecks(where) {
		const margin = Theme.geometry_tabBar_horizontalMargin
		const bars = findAll(stage, x => isSeg(x) && x.visible)
		check(bars.length > 0, dataName + ", " + where + ": button rows found: " + bars.length)
		for (const bar of bars) {
			const btns = findAll(bar, x => !!x.contentItem && !!x.background && x.contentItem.truncated !== undefined)
			const tabs = btns.map(b => {
				const l = b.contentItem, r = rect(b), lr = rect(l), cw = l.contentWidth, bw = b.background.border.width
				const tx = l.horizontalAlignment === Text.AlignHCenter ? lr.x + (lr.w - cw) / 2
						: l.horizontalAlignment === Text.AlignRight ? lr.x + lr.w - cw : lr.x
				return { text: l.text, px: l.font.pixelSize, family: l.font.family, cw: cw, w: r.w, trunc: l.truncated, full: l.implicitWidth,
						gap: Math.min(tx - (r.x + bw), r.x + r.w - bw - (tx + cw)) }
			})
			const sig = dataName + "|" + tabs.map(t => t.text + "@" + t.px).join("|") + "|" + r1(bar.width)
			if (tabRowsSeen[sig])
				continue
			tabRowsSeen[sig] = true
			const bad = tabs.filter(t => t.trunc || t.gap < margin - 0.01)
			const tight = tabs.reduce((a, t) => !a || t.gap < a.gap ? t : a, null)
			const desc = t => "\"" + t.text + "\" " + r1(t.cw) + " px wide (" + t.family + " " + t.px + " px) in a " + r1(t.w) + " px tab, "
					+ r1(t.gap) + " px inside its border" + (t.trunc ? ", elided (" + r1(t.full) + " px in full)" : "")
			check(tabs.length === bar.model.length && tabs.length > 0 && bad.length === 0, dataName + ", " + where + ": button row "
					+ tabs.map(t => t.text).join(" | ") + ": " + (bad.length ? bad.map(desc).join("; ") : "tightest " + desc(tight))
					+ " (>= " + margin + ")")
		}
	}

	property var queue: []
	Timer { id: driver; interval: 500; repeat: true; running: true; onTriggered: win.stepOnce() }
	function stepOnce() {
		if (!queue.length) { driver.stop(); console.log("DONE fails=" + fails); Qt.quit(); return }
		try { queue.shift()() } catch (e) { fails++; console.log("FAIL [" + layoutName + "] exception " + e + "\n" + e.stack) }
	}
	Component.onCompleted: {
		Global.pageManager = pm
		for (const name of sets) {
			queue.push(() => build(name, 0), () => {}, () => speedChecks(), () => tabChecks("Dashboard"))
			queue.push(() => build(name, 1), () => { find(home, x => x.windTrue !== undefined && x.shortWindow !== undefined).tab = 3 }, () => {}, () => windChecks(),
					() => tabChecks("Plots"))
			queue.push(() => build(name, 2, 0, "alternator/rpm_table"), () => {}, () => headerChecks(), () => saveRowChecks("Save Table", "Clear Overheats / Safe Hours"),
					() => tabChecks("pushed RPM Table"))
			queue.push(() => build(name, 2, 4), () => {}, () => saveRowChecks("Set Manual", "Use automatic GPS"), () => overrideChecks(), () => tabChecks("Settings"))
			for (const t of [0, 1, 2])
				queue.push(() => build(name, 1), () => { find(home, x => x.windTrue !== undefined && x.shortWindow !== undefined).tab = t }, () => {},
						() => tabChecks("Plots tab " + t))
			queue.push(() => build(name, 3), () => {}, () => tabChecks("Cloud"))
			for (const t of [1, 2])
				queue.push(() => build(name, 3), () => { find(home, x => x.cloudState !== undefined).tab = t }, () => {}, () => tabChecks("Cloud tab " + t))
			// each Leaderboards section, for its section row and its boards' tab rows
			for (const sec of [1, 2, 3, 4, 5, 6, 7])
				queue.push(() => build(name, 3), () => {}, () => {
					const cl = find(home, x => x.cloudState !== undefined)
					if (cl.model && cl.model.sections && sec < cl.model.sections.length)
						cl.section = sec
				}, () => {}, () => {
					const cl = find(home, x => x.cloudState !== undefined)
					if (cl.section === sec)
						tabChecks("Cloud section " + sec)
				})
			queue.push(() => build(name, 0), () => motorPrep(false), () => {}, () => motorChecks())
			if (name !== "empty")
				queue.push(() => build(name, 0), () => motorPrep(true), () => {}, () => motorChecks())
			queue.push(() => build(name, 2, 0, "alternator/rpm_table"), () => {}, () => ctlRowChecks(""), () => tableRowsShown(), () => insetChecks(""),
					() => MockStore.put(svc + "/Settings/Value/PhysicalPanelOverride", 1), () => {}, () => ctlRowChecks(", Physical Panel Override on"),
					() => { top.showHelp = true }, () => {}, () => ctlRowChecks(", override and descriptions on"), () => insetChecks(", descriptions on"))
		}
	}
}
