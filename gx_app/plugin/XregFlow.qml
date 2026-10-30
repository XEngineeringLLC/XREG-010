import QtQuick
import Victron.VenusOS

// Home-page power flow (redesign approved 2026-10-28, mockup claude_previews/power_flow_redesign.html): the battery first
// (state word, signed amps, where the reading comes from, the limit holding it), then one row per source the boat has in
// a fixed order, then Loads (an inverter's draw, a DC-DC charger that draws from the house bank, a GX DC meter on a load,
// and House: whatever no other meter measures, calculated, or from the GX's whole-DC-system meter where the boat has one;
// the footnote names an imbalance between that meter and the sum). Every bar shares one fixed scale,
// full = the alternator's maximum (the Command Limit held to the largest Charge Rate Limits entry, as DVCC is told); a
// bar past full gets a break mark and keeps its exact number. Rows change only when the boat's equipment changes: a
// device the GX has seen stays listed at 0.0 A. Data: the bridge's /Flow/Json (xreg_core.py, "Home page power flow").
// Layouts: two-line rows with Sources / Loads labels and a scale line; when those do not fit, the labels and the scale
// line drop out; when even that does not fit, one-line rows with short names, the alternator target in the footnote.
// compact (5-inch) follows the same rule under a one-line battery headline, its limit in the footnote. grow (portrait,
// the page scrolls): two-line rows, the tile as tall as they need (implicitHeight).
Rectangle {
	id: tile

	property string svc
	property bool linkUp
	property bool active          // the alternator is regulating (status ACTIVE): show its target
	property real target: NaN     // the alternator current command, A
	property bool compact
	property bool grow
	property bool animationEnabled
	property bool shown

	readonly property bool light: Theme.colorScheme === Theme.Light
	readonly property color dischargeColor: Theme.color_font_secondary
	readonly property color hatchColor: Qt.rgba(Theme.color_font_primary.r, Theme.color_font_primary.g,
												Theme.color_font_primary.b, light ? 0.22 : 0.20)
	// gui-v2's own colours where it has one; the rest picked to stay apart from each other (and every row is labelled).
	// A DC-DC charger keeps its colour on either side. sum: how the footnote's sum names it.
	readonly property var kinds: ({
		alt: { name: "Alternator", short: "Alternator", color: "#00a19a", sum: "alternator" },   // web app --accent: the product
		solar: { name: "Solar", short: "Solar", color: light ? "#C38608" : "#E3A21A", sum: "solar" },
		charger: { name: "Shore charger", short: "Shore", color: "#4A6FE3", sum: "shore charger" },
		multi: { name: "Inverter/charger", short: "Inv/chg", color: "#8FB8E8", sum: "inverter/charger" },
		dcgenset: { name: "DC generator", short: "DC gen", color: "#C0703F", sum: "DC generator" },
		fuelcell: { name: "Fuel cell", short: "Fuel cell", color: "#B0C62B", sum: "fuel cell" },
		altx: { name: "Other alternator", short: "Other alt", color: "#2FD0C4", sum: "other alternator" },
		dcdc: { name: "DC-DC charger", short: "DC-DC", color: "#8E7CF0", sum: "DC-DC" },
		wind: { name: "Wind generator", short: "Wind", color: light ? "#5E9DBF" : "#9CC9E0", sum: "wind generator" },
		water: { name: "Water generator", short: "Water gen", color: "#3FA6D6", sum: "water generator" },
		shaft: { name: "Shaft generator", short: "Shaft gen", color: "#B8915F", sum: "shaft generator" },
		source: { name: "Other source", short: "Other", color: "#A3A07A", sum: "other source" },
		inverter: { name: "Inverter", short: "Inverter", color: "#E07AA8", sum: "inverter" },
		dcdcload: { name: "DC-DC charger", short: "DC-DC", color: "#8E7CF0", sum: "DC-DC" },
		dcload: { name: "DC load", short: "DC load", color: "#F89179", sum: "DC loads" },
		house: { name: "House", short: "House", color: "#C477D6" }
	})
	readonly property var loadKinds: ["inverter", "dcdcload", "dcload", "house"]
	readonly property var battFromText: ({ "GX": "GX battery monitor" })

	radius: Theme.geometry_overviewPage_widget_radius
	color: Theme.color_background_secondary
	clip: true
	implicitHeight: grow ? growH : 300

	VeQuickItem { id: want; uid: tile.svc + "/Ui/Want/Flow" }
	Timer {
		interval: 15000
		repeat: true
		triggeredOnStart: true
		running: tile.shown
		onTriggered: want.setValue(Math.floor(Date.now() / 1000))
	}
	VeQuickItem { id: jsonItem; uid: tile.svc + "/Flow/Json" }
	VeQuickItem { id: okItem; uid: tile.svc + "/Flow/Ok" }

	readonly property var flow: {
		if (!linkUp || !jsonItem.valid || !jsonItem.value)
			return null
		try {
			return JSON.parse(jsonItem.value)
		} catch (e) {
			return null
		}
	}
	readonly property bool oldFirmware: linkUp && okItem.valid && okItem.value === 0
	readonly property bool houseMetered: !!(flow && flow.hm)
	readonly property var rows: flow ? flow.rows.concat([{ id: "house", k: "house", a: flow.house,
															  c: houseMetered ? "measured" : "calculated" }]) : []
	readonly property int nRows: rows.length
	readonly property int nSrc: rows.filter(r => loadKinds.indexOf(r.k) < 0).length
	readonly property real fullScale: flow && flow.fs > 0 ? flow.fs : 100
	readonly property real batt: flow && flow.b.a !== null ? flow.b.a : NaN
	readonly property int battState: flow && flow.b.st !== null ? flow.b.st : -2   // -2 no reading
	readonly property real limit: flow && flow.b.lim !== null && battState === 1 ? flow.b.lim : NaN
	readonly property real altA: rows.length && rows[0].a !== null ? rows[0].a : NaN
	readonly property bool targetOn: active && isFinite(target) && target > 0 && isFinite(altA)
	readonly property string why: flow ? flow.why : ""

	function kind(k) { return kinds[k] || kinds.source }
	// One decimal everywhere, and a true minus (a digit's width), so a readout never changes shape
	function amps(v) { return !isFinite(v) ? "--" : Math.abs(v).toFixed(1) }
	function signed(v) { return !isFinite(v) ? "--" : (v >= 0.05 ? "+" : v <= -0.05 ? "−" : "") + amps(v) }
	function num(v) { return v === null || v === undefined ? NaN : v }
	// One-line rows carry short names; two rows with one short name carry their device names instead
	function rowLabel(r) {
		if (!oneLine)
			return kind(r.k).name
		const twin = rows.filter(x => kind(x.k).short === kind(r.k).short).length > 1
		return twin && r.c && r.k !== "solar" ? r.c : kind(r.k).short
	}
	// The first text that fits the footnote's width, else the last
	function fit(texts) {
		for (const t of texts) {
			if (microMetrics.advanceWidth(t) <= inner)
				return t
		}
		return texts[texts.length - 1]
	}
	// With the DC system meter House is measured: the imbalance is exact, so no "at least"
	readonly property string whyText: {
		if (!flow)
			return ""
		const x = amps(flow.x) + " A"
		if (why === "batt")
			return fit(["No battery reading, so House is unknown", "No battery reading"])
		if (why === "unmeasured")
			return fit([(houseMetered ? "An unmeasured source adds " : "An unmeasured source adds at least ") + x,
						"Unmeasured source adds " + x, "Unmeasured source " + x])
		if (why === "gap")
			return fit(["An unmeasured load draws " + x, "Unmeasured load draws " + x, "Unmeasured load " + x])
		if (why)
			return fit([kind(why).name + " not measured, so House is unknown", kind(why).name + " not measured",
						kind(why).short + " not measured"])
		return ""
	}
	// one kind of measured load is named, more than one together; "loads" where that does not fit (5-inch). With the
	// meter, the sources and loads whose current passes through it ("in").
	readonly property string sumText: {
		const ks = loadKinds.filter(k => k !== "house" && rows.some(r => r.k === k && (!houseMetered || r.in)))
		if (!houseMetered) {
			const base = "House = sources − battery"
			if (!ks.length)
				return base
			return fit([base + " − " + (ks.length === 1 ? kind(ks[0]).sum : "measured loads"), base + " − loads"])
		}
		const ss = Object.keys(kinds).filter(k => loadKinds.indexOf(k) < 0 && rows.some(r => r.k === k && r.in))
		const named = (ss.length ? " + " + (ss.length === 1 ? kind(ss[0]).sum : "sources") : "")
			+ (ks.length ? " − " + (ks.length === 1 ? kind(ks[0]).sum : "measured loads") : "")
		const plain = (ss.length ? " + sources" : "") + (ks.length ? " − loads" : "")
		return fit(["House = DC system meter" + named, "House = DC system meter" + plain, "House = DC meter" + plain])
	}
	readonly property string scaleText: "Full bar = " + Math.round(fullScale) + " A"
		+ (flow && flow.fs > 0 && !compact ? ", the alternator maximum" : "")
	// The tile as text (the offline harness reads it)
	readonly property string summary: !flow ? "" : battWord + " " + signed(batt) + " A | "
		+ rows.map(r => rowLabel(r) + " " + amps(num(r.a))).join(" | ") + (whyText ? " | " + whyText : "")

	// ---- geometry ----
	readonly property real pad: compact ? 8 : 12
	readonly property real inner: width - 2 * pad
	readonly property real headTop: compact ? pad : 28
	readonly property real barTop: compact ? pad + 25 : 64
	readonly property real barH: compact ? 6 : 8
	readonly property real sepY: barTop + barH + 8
	readonly property real fullFootH: 30
	// normal: section labels, two-line rows and the two-line footnote
	readonly property real normalTop: sepY + 22
	readonly property real normalPitch: (height - pad - fullFootH - 4 - normalTop - 18) / Math.max(1, nRows)
	readonly property real growH: Math.ceil(Math.max(300, normalTop + nRows * 36 + 18 + 4 + fullFootH + pad))
	// dense: two-line rows without the section labels and the scale line (5-inch has no separator under the battery)
	readonly property real denseTop: compact ? sepY : sepY + 8
	readonly property real densePitch: (height - pad - 15 - 4 - denseTop - 9) / Math.max(1, nRows)
	readonly property string mode: grow || normalPitch >= 33 ? "normal" : densePitch >= 30 ? "dense"
		: compact ? "compact" : "oneline"
	readonly property bool oneLine: mode === "compact" || mode === "oneline"
	readonly property real rowsTop: mode === "normal" ? normalTop : denseTop
	readonly property real loadsH: mode === "normal" ? 18 : mode === "dense" ? 9 : 7
	readonly property real footH: mode === "normal" ? fullFootH : 15
	readonly property bool footOn: !oneLine || (height - pad - footH - rowsTop - loadsH) / Math.max(1, nRows) >= 15
	readonly property real pitch: {
		if (mode === "normal")
			return Math.max(30, Math.min(44, grow ? Math.max(36, normalPitch) : normalPitch))
		if (mode === "dense")
			return Math.min(44, densePitch)
		return Math.max(13, Math.min(24, (height - pad - (footOn ? footH : 0) - 4 - rowsTop - loadsH) / Math.max(1, nRows)))
	}
	readonly property real rowBarH: pitch >= 38 ? 8 : 6
	readonly property real nameColW: {
		let w = 0
		for (const r of rows)
			w = Math.max(w, microMetrics.advanceWidth(rowLabel(r)))
		return Math.ceil(Math.min(w, inner * 0.42))
	}
	function rowY(i) { return rowsTop + i * pitch + (i >= nSrc ? loadsH : 0) }

	FontMetrics { id: microMetrics; font.family: Global.fontFamily; font.pixelSize: Theme.font_size_micro }
	FontMetrics { id: captionMetrics; font.family: Global.fontFamily; font.pixelSize: Theme.font_size_caption }

	readonly property string battWord: battState === 1 ? "Charging" : battState === -1 ? "Discharging"
		: battState === 0 ? "Resting" : "No reading"
	readonly property color battWordColor: battState === 1 ? Theme.color_green : battState === -1 ? Theme.color_font_primary
		: Theme.color_font_secondary

	Item {
		anchors.fill: parent
		visible: !!tile.flow

		// Battery, its source and the limit holding it (full layouts); one headline line on 5-inch
		Label {
			id: battCaption
			x: tile.pad; y: 10
			visible: !tile.compact
			text: "Battery"
			color: Theme.color_font_secondary
			font.pixelSize: Theme.font_size_caption
		}
		Label {
			x: battCaption.x + battCaption.width + 8
			anchors.baseline: battCaption.baseline
			width: (limitCap.visible ? limitCap.x - 8 : tile.width - tile.pad) - x
			visible: !tile.compact
			elide: Text.ElideRight
			text: tile.battState === -2 ? "no shunt or battery monitor"
				  : (tile.battFromText[tile.flow ? tile.flow.b.from : ""] || (tile.flow ? tile.flow.b.from : ""))
			color: Theme.color_font_secondary
			font.pixelSize: Theme.font_size_micro
		}
		Caption {
			id: limitCap
			anchors { right: parent.right; rightMargin: tile.pad; baseline: battCaption.baseline }
			visible: !tile.compact && isFinite(tile.limit)
			glyph: "lim"
			text: "Held at " + Math.round(tile.limit) + " A limit"
		}
		Label {
			id: battWordLabel
			x: tile.pad
			y: tile.compact ? tile.pad + 1 : tile.headTop
			text: tile.battWord
			color: tile.battWordColor
			font.pixelSize: tile.compact ? Theme.font_size_caption : Theme.font_size_body2
			font.bold: tile.compact
		}
		Label {
			x: battWordLabel.x + battWordLabel.width + 6
			anchors.baseline: battWordLabel.baseline
			width: battNum.x - 4 - x
			visible: tile.compact && tile.battState !== -2
			elide: Text.ElideRight
			text: tile.flow ? tile.flow.b.from : ""
			color: Theme.color_font_secondary
			font.pixelSize: Theme.font_size_micro
		}
		XregNum {
			id: battNum
			anchors { right: parent.right; rightMargin: tile.pad; baseline: battWordLabel.baseline }
			template: "−888.8 A"
			text: tile.signed(tile.batt) + " A"
			color: isFinite(tile.batt) ? Theme.color_font_primary : Theme.color_font_secondary
			font.pixelSize: tile.compact ? Theme.font_size_body1 : Theme.font_size_body3
			font.bold: true
		}
		Bar {
			x: tile.pad; y: tile.barTop
			width: tile.inner; height: tile.barH
			value: tile.battState === -2 ? NaN : Math.abs(tile.batt) <= 0.3 ? 0 : Math.abs(tile.batt)
			fill: tile.batt > 0 ? Theme.color_green : tile.dischargeColor
			mark: tile.limit
			markKind: "lim"
		}
		Rectangle {
			x: tile.pad; y: tile.sepY
			width: tile.inner; height: 1
			visible: !tile.compact
			color: Theme.color_card_separator
		}
		Label {
			x: tile.pad; y: tile.sepY + 6
			visible: tile.mode === "normal"
			text: "Sources"
			color: Theme.color_font_secondary
			font.pixelSize: Theme.font_size_micro
		}
		Label {
			x: tile.pad; y: tile.rowY(tile.nSrc) - tile.loadsH + 2
			visible: tile.mode === "normal"
			text: "Loads"
			color: Theme.color_font_secondary
			font.pixelSize: Theme.font_size_micro
		}
		Rectangle {
			x: tile.pad; y: Math.round(tile.rowY(tile.nSrc) - tile.loadsH / 2)
			width: tile.inner; height: 1
			visible: tile.mode !== "normal"
			color: Theme.color_card_separator
		}

		Repeater {
			model: tile.nRows
			delegate: FlowRow {
				required property int index
				r: tile.rows[index] || ({ id: "", k: "source", a: null, c: "" })
				x: tile.pad
				y: tile.rowY(index)
				width: tile.inner
				height: tile.pitch
			}
		}

		// Footnote: why House is unknown, else the sum in words (5-inch: the battery limit, then the alternator target,
		// first: neither has room above); the scale line under it. One-line rows: one line, the limit or the alternator
		// target when nothing is missing.
		Caption {
			id: foot1
			x: tile.pad; y: tile.height - tile.pad - tile.footH
			width: tile.inner
			visible: tile.footOn && !tile.oneLine
			readonly property bool limitHere: tile.compact && !tile.whyText && isFinite(tile.limit)
			readonly property bool targetHere: tile.compact && !tile.whyText && !limitHere && tile.targetOn
			glyph: limitHere ? "lim" : targetHere ? "tgt" : ""
			text: tile.whyText || (limitHere ? "Held at " + Math.round(tile.limit) + " A battery limit"
								   : targetHere ? "Alternator target " + Math.round(tile.target) + " A" : tile.sumText)
		}
		Label {
			x: tile.pad; y: foot1.y + 15
			width: tile.inner
			visible: tile.mode === "normal"
			elide: Text.ElideRight
			text: tile.scaleText
			color: Theme.color_font_secondary
			font.pixelSize: Theme.font_size_micro
		}
		Caption {
			x: tile.pad; y: tile.height - tile.pad - tile.footH + 1
			width: tile.inner
			visible: tile.footOn && tile.oneLine
			// the full layout's one-line rows keep the limit on the battery line above
			readonly property bool limitHere: tile.compact && isFinite(tile.limit)
			glyph: tile.whyText ? "" : limitHere ? "lim" : tile.targetOn ? "tgt" : ""
			text: tile.whyText ? tile.whyText
				  : limitHere ? "Held at " + Math.round(tile.limit) + " A battery limit"
				  : tile.targetOn ? "Alternator target " + Math.round(tile.target) + " A"
				  : "Full bar = " + Math.round(tile.fullScale) + " A"
		}
	}

	Label {
		anchors.centerIn: parent
		visible: tile.oldFirmware
		width: tile.width - 2 * tile.pad
		horizontalAlignment: Text.AlignHCenter
		wrapMode: Text.WordWrap
		text: "Power flow needs newer regulator firmware"
		color: Theme.color_font_secondary
		font.pixelSize: Theme.font_size_caption
	}

	// One source or load: dot, name, caption and amps over its bar (two-line), or dot, name, bar, amps (one-line)
	component FlowRow: Item {
		id: row
		property var r
		readonly property var kd: tile.kind(r.k)
		readonly property real value: tile.num(r.a)
		readonly property bool isAlt: r.k === "alt"
		readonly property bool showTarget: isAlt && tile.targetOn
		// 5-inch: the caption has no room for it, the footnote carries it
		readonly property bool targetCaption: showTarget && !tile.compact

		Rectangle {
			id: dot
			x: 0
			y: tile.oneLine ? (row.height - height) / 2 : 5
			width: tile.oneLine ? 8 : 10; height: width; radius: width / 2
			color: row.kd.color
		}
		Label {
			id: nameLabel
			x: dot.width + (tile.oneLine ? 6 : 7)
			y: tile.oneLine ? (row.height - height) / 2 : 0
			width: tile.oneLine ? tile.nameColW : Math.ceil(captionMetrics.advanceWidth(text))
			elide: Text.ElideRight
			text: tile.rowLabel(row.r)
			font.pixelSize: tile.oneLine ? Theme.font_size_micro : Theme.font_size_caption
		}
		Caption {
			x: nameLabel.x + nameLabel.width + 7
			anchors.baseline: nameLabel.baseline
			width: ampsNum.x - 6 - x
			// left out where only an ellipsis or a few letters would show
			visible: !tile.oneLine && (implicitWidth <= width || width >= 40)
			glyph: row.targetCaption ? "tgt" : ""
			text: row.targetCaption ? "target " + Math.round(tile.target) + " A"
				  : !isFinite(row.value) && row.r.k !== "house" ? "not measured" : (row.r.c || "")
		}
		XregNum {
			id: ampsNum
			anchors { right: parent.right; baseline: nameLabel.baseline }
			template: "888.8 A"
			text: isFinite(row.value) ? tile.amps(row.value) + " A" : "--"
			color: isFinite(row.value) ? Theme.color_font_primary : Theme.color_font_secondary
			font.pixelSize: tile.oneLine ? Theme.font_size_tiny : Theme.font_size_caption
			font.bold: true
		}
		Bar {
			x: tile.oneLine ? nameLabel.x + nameLabel.width + 6 : 0
			y: tile.oneLine ? (row.height - height) / 2 : 23
			width: tile.oneLine ? ampsNum.x - 6 - x : row.width
			height: tile.oneLine ? 6 : tile.rowBarH
			value: row.value
			fill: row.kd.color
			mark: row.showTarget ? tile.target : NaN
			markKind: "tgt"
		}
	}

	// A bar on the tile's one scale: hatched with no reading, a break mark past full, a target or limit tick
	component Bar: Item {
		id: bar
		property real value
		property color fill
		property real mark: NaN
		property string markKind
		readonly property real frac: isFinite(value) ? Math.max(0, value) / tile.fullScale : 0
		Rectangle {
			id: track
			anchors.fill: parent
			radius: height / 2
			color: Theme.color_card_separator
		}
		Canvas {
			id: hatch
			anchors.fill: parent
			visible: !isFinite(bar.value)
			onPaint: {
				const c = getContext("2d")
				c.reset()
				c.beginPath()
				c.roundedRect(0, 0, width, height, height / 2, height / 2)
				c.clip()
				c.strokeStyle = tile.hatchColor
				c.lineWidth = 2
				for (let x = -height; x < width + height; x += 5.657) {
					c.beginPath()
					c.moveTo(x, height)
					c.lineTo(x + height, 0)
					c.stroke()
				}
			}
			onWidthChanged: requestPaint()
			onVisibleChanged: if (visible) requestPaint()
			Connections {
				target: tile
				function onHatchColorChanged() { hatch.requestPaint() }
			}
		}
		Rectangle {
			id: fillRect
			height: parent.height
			radius: height / 2
			width: !isFinite(bar.value) ? 0 : Math.max(bar.value >= 0.5 ? 3 : 0, Math.min(1, bar.frac) * bar.width)
			color: bar.fill
			Behavior on width { enabled: tile.animationEnabled; NumberAnimation { duration: 300 } }
		}
		// past full: two slashes in the tile's colour near the end; the number beside it stays exact
		Repeater {
			model: bar.frac > 1 ? [9, 14] : []
			delegate: Rectangle {
				required property int modelData
				x: bar.width - modelData - width
				y: -2
				width: 2; height: bar.height + 4
				rotation: 25
				antialiasing: true
				color: tile.color
			}
		}
		Rectangle {
			visible: isFinite(bar.mark)
			x: Math.min(1, Math.max(0, bar.mark) / tile.fullScale) * bar.width - width / 2
			y: bar.markKind === "tgt" ? -3 : -4
			width: bar.markKind === "tgt" ? 3 : 2
			height: bar.height - 2 * y
			radius: bar.markKind === "tgt" ? 1.5 : 0
			color: bar.markKind === "tgt" ? Theme.color_orange : Theme.color_font_primary
			Behavior on x { enabled: tile.animationEnabled; NumberAnimation { duration: 300 } }
		}
	}

	// A caption, after a small tick in the marker's own colour when it explains one (tgt: the alternator target,
	// lim: the battery limit)
	component Caption: Item {
		id: cap
		property string text
		property string glyph
		readonly property real glyphW: glyph === "tgt" ? 3 : glyph === "lim" ? 2 : 0
		implicitWidth: (glyphW ? glyphW + 4 : 0) + capLabel.implicitWidth
		implicitHeight: capLabel.implicitHeight
		baselineOffset: capLabel.baselineOffset
		width: implicitWidth
		Rectangle {
			visible: cap.glyphW > 0
			y: capLabel.baselineOffset - height + 1
			width: cap.glyphW; height: 11
			radius: cap.glyph === "tgt" ? 1 : 0
			color: cap.glyph === "tgt" ? Theme.color_orange : Theme.color_font_primary
		}
		Label {
			id: capLabel
			x: cap.glyphW ? cap.glyphW + 4 : 0
			width: Math.max(0, cap.width - x)
			elide: Text.ElideRight
			text: cap.text
			color: Theme.color_font_secondary
			font.pixelSize: Theme.font_size_micro
		}
	}
}
