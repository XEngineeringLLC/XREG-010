import QtQuick
import QtQuick.Layouts
import Victron.VenusOS

// One page of the settings outline (/Settings/Schema "outline", built by gx_app/gen_settings.py into
// gx_app/xregulator/bridge/settings_schema.json): the app's Setup sub-tab, panel or nested <details> named by nodeId,
// its rows in the app's order. A row is a setting, a group heading, a deeper page (pushed), the GX-only Connection
// page, or a table drawn in place (XregCfgTable) with the app's buttons that sit above it (Charge Rate, Limit by).
// Embedded in the home page's Settings page it is the list alone; pushed, it carries the home page's header and
// sub-tab row (XregHeader). A short page (the schema's "short") always shows its descriptions; a longer one has a
// "Show descriptions" switch when anything on it has one.
// Values are in the schema's units (temperatures in °F, as on the wire); shown in °C when the GX is set so.
Page {
	id: page

	property string svc
	property string nodeId
	property Item home                // the home page this belongs to (XregHome); none when opened on its own
	property bool embedded            // inside the home page's Settings page, under its sub-tab row
	property bool showHelp
	property bool wroteSinceOpen
	property int lastSeq: -1
	property var pendingWrite: null

	readonly property bool useC: Global.systemSettings.temperatureUnit === VenusOS.Units_Temperature_Celsius
	readonly property bool linkUp: connectedItem.valid && connectedItem.value === 1
	readonly property var outline: {
		if (!schemaItem.valid || !schemaItem.value)
			return []
		try {
			return JSON.parse(schemaItem.value).outline || []
		} catch (e) {
			return []
		}
	}
	function findNode(nodes, id) {
		for (const n of nodes) {
			if (!n.children)
				continue
			if (n.id === id)
				return n
			if (id.indexOf(n.id + "/") === 0)
				return findNode(n.children, id)
		}
		return null
	}
	readonly property var node: page.nodeId ? findNode(page.outline, page.nodeId) : null
	readonly property var items: node ? node.children.filter(c => !c.children) : []
	readonly property bool shortPage: !!node && !!node.short
	readonly property bool helpOn: shortPage || showHelp
	// Charge Rate and Limit by: drawn by the table they set, never as rows of their own
	readonly property var tableControls: {
		const out = []
		for (const it of items)
			if (it.type === "table")
				out.push(...(it.controls || []))
		return out
	}
	readonly property bool anyHelp: items.some(it => !it.hidden
			&& (!!it.help || (it.type === "table" && it.columns.some(c => !!c.help))))
	readonly property var rowModel: {
		const out = []
		if (!node)
			return out
		let g = null
		for (const c of node.children) {
			if (c.children) {
				if (!c.hidden)
					out.push({ kind: "page", cfg: c })
				g = null
				continue
			}
			if (c.hidden || tableControls.indexOf(c.key) >= 0)
				continue
			if (c.group && c.group !== g)
				out.push({ kind: "header", label: c.group })
			g = c.group || null
			out.push({ kind: c.type === "info" ? c.widget : c.type, cfg: c })
		}
		return out
	}
	onNodeIdChanged: list.contentY = 0

	// Every key a gate on this page reads: disabledWhen, and gates[].when on an item or a select option.
	readonly property var gateKeys: {
		const out = []
		const add = c => {
			if (c.any || c.all)
				(c.any || c.all).forEach(add)
			else if (out.indexOf(c.key) < 0)
				out.push(c.key)
		}
		for (const it of page.items) {
			if (it.disabledWhen)
				add(it.disabledWhen)
			for (const g of (it.gates || []))
				add(g.when)
			for (const o of (it.options || []))
				for (const g of (o.gates || []))
					add(g.when)
		}
		return out
	}
	property var gateValues: ({})
	property int gateRev

	// Result keys this page answers for, Charge Rate and Limit by included; a table's own result is its editor's.
	readonly property var ownKeys: {
		const out = []
		for (const it of page.items) {
			if (it.type === "info" && it.widget === "probes")
				out.push("ProbeRole")
			else if (it.type !== "table" && it.type !== "info")
				out.push(it.key)
		}
		return out
	}

	function valueUid(key) { return svc + "/Settings/Value/" + key }
	function httpUid(ep) { return svc + "/Settings/Http/" + ep }
	function tick() { return Math.floor(Date.now() / 100) % 2000000000 }

	function toShown(cfg, v) {
		if (v === undefined || v === null || !cfg || !cfg.temp || !useC)
			return v
		if (cfg.zeroOff && v === 0)
			return 0
		return cfg.temp === "delta" ? v * 5 / 9 : (v - 32) * 5 / 9
	}
	function fromShown(cfg, v) {
		if (!cfg || !cfg.temp || !useC || (cfg.zeroOff && v === 0))
			return v
		return Math.round((cfg.temp === "delta" ? v * 9 / 5 : v * 9 / 5 + 32) * 10) / 10
	}
	function tempF(t) { return useC ? (t - 32) * 5 / 9 : t }
	// A number with a true minus (a digit's width), left-padded with figure spaces (also a digit's width) to `chars`, so
	// what follows it in the same line stays put as the value changes
	function signNum(v, dec) { const t = Math.abs(v).toFixed(dec); return (v < 0 && Number(t) !== 0 ? "\u2212" : "") + t }
	function figPad(text, chars) {
		let t = String(text)
		while (t.length < chars)
			t = "\u2007" + t
		return t
	}
	function tempUnit() { return useC ? "°C" : "°F" }
	function unitOf(cfg) { return cfg && cfg.temp && useC ? "°C" : (cfg && cfg.unit ? cfg.unit : "") }
	function stepDecimals(st) {
		const s = String(st)
		const i = s.indexOf(".")
		return i < 0 ? 0 : s.length - i - 1
	}
	function optionLabel(cfg, v) {
		const o = (cfg.options || []).find(x => x.value === v)
		return o ? o.label : ""
	}
	function fmt(cfg, valid, v) {
		if (!valid || v === undefined || v === null)
			return "--"
		if (cfg.zeroOff && v === 0)
			return typeof cfg.zeroOff === "string" ? cfg.zeroOff : "0"
		const u = unitOf(cfg)
		return Units.formatNumber(toShown(cfg, v), cfg.decimals || 0) + (u ? " " + u : "")
	}
	function setGateValue(k, valid, v) {
		gateValues[k] = valid ? v : undefined
		gateRev++
	}
	// A key not yet echoed never holds, so nothing greys before the first reading.
	function holds(c) {
		if (c.any)
			return c.any.some(x => holds(x))
		if (c.all)
			return c.all.every(x => holds(x))
		const v = gateValues[c.key]
		if (v === undefined || v === null)
			return false
		const n = Number(v)
		return c.eq !== undefined ? n === c.eq : c.ne !== undefined ? n !== c.ne : c.le !== undefined ? n <= c.le : false
	}
	// The notes of the gates holding on cfg (an item or a select option), joined; null while none holds.
	function gateNotes(cfg) {
		if (!cfg)
			return null
		const notes = []
		let on = false
		const gs = (cfg.gates || []).slice()
		if (cfg.disabledWhen)
			gs.push({ when: { key: cfg.disabledWhen.key, eq: cfg.disabledWhen.value }, note: cfg.disabledWhen.note })
		for (const g of gs) {
			if (holds(g.when)) {
				on = true
				if (g.note)
					notes.push(g.note)
			}
		}
		return on ? notes.join("\n") : null
	}
	function captionOf(cfg, gatedNote) {
		if (gatedNote)
			return gatedNote
		return helpOn && cfg && cfg.help ? cfg.help : ""
	}

	function confirmThen(cfg, fn) {
		if (cfg && cfg.confirm)
			Global.dialogLayer.open(confirmDialog, { title: cfg.label, body: cfg.confirm, okFn: fn })
		else
			fn()
	}
	function writeValue(item, v) {
		wroteSinceOpen = true
		item.setValue(v)
	}
	function openNumber(cfg, item) {
		// The firmware parses some keys with toInt(): offer whole steps there, except a °C view of a °F key.
		const whole = !!cfg.send && cfg.send.integer && !(cfg.temp && useC)
		const step = whole ? Math.max(1, cfg.step || 1) : (cfg.step || 1)
		const dec = whole ? 0 : Math.max(cfg.decimals || 0, stepDecimals(step))
		const lim = 2147483647 / Math.pow(10, dec)    // the dialog's spin box counts in int32 steps
		// A °C view rounds the limits inward, so the dialog's end stops convert back inside the °F range.
		const f = Math.pow(10, dec)
		const cvt = cfg.temp && useC
		const lo = Math.max(-lim, cfg.min !== undefined ? (cvt ? Math.ceil(toShown(cfg, cfg.min) * f - 1e-6) / f : cfg.min) : -lim)
		const hi = Math.min(lim, cfg.max !== undefined ? (cvt ? Math.floor(toShown(cfg, cfg.max) * f + 1e-6) / f : cfg.max) : lim)
		const cur = item.valid ? toShown(cfg, item.value) : Math.max(lo, Math.min(hi, 0))
		Global.dialogLayer.open(numberDialog, {
			title: cfg.label,
			suffix: unitOf(cfg) ? " " + unitOf(cfg) : "",
			decimals: dec,
			from: lo,
			to: hi,
			stepSize: step,
			value: cur,
			cfg: cfg,
			target: item
		})
	}
	function flushPending() {
		const p = pendingWrite
		pendingWrite = null
		if (p)
			confirmThen(p.cfg, () => writeValue(p.target, fromShown(p.cfg, p.value)))
	}
	function handleResult() {
		if (!resultItem.valid)
			return
		let r = null
		try {
			r = JSON.parse(resultItem.value)
		} catch (e) {
			return
		}
		if (!r || r.seq === lastSeq)
			return
		const stale = lastSeq < 0 && !wroteSinceOpen
		lastSeq = r.seq
		if (!stale && !r.ok && ownKeys.indexOf(r.key) >= 0)
			Global.showToastNotification(VenusOS.Notification_Warning, r.message || ("Not saved: " + r.key), 5000)
	}

	VeQuickItem { id: schemaItem; uid: page.svc + "/Settings/Schema" }
	VeQuickItem { id: connectedItem; uid: page.svc + "/Connected" }
	VeQuickItem { id: wantItem; uid: page.svc + "/Ui/Want/Settings" }
	VeQuickItem {
		id: resultItem
		uid: page.svc + "/Settings/Result"
		onValueChanged: page.handleResult()
	}

	Timer {
		interval: 15000
		repeat: true
		triggeredOnStart: true
		running: page.visible
		onTriggered: wantItem.setValue(Math.floor(Date.now() / 1000))
	}

	Component.onCompleted: handleResult()

	Repeater {
		model: page.gateKeys
		delegate: Item {
			VeQuickItem {
				uid: page.valueUid(modelData)
				onValueChanged: page.setGateValue(modelData, valid, value)
				onValidChanged: page.setGateValue(modelData, valid, value)
				Component.onCompleted: page.setGateValue(modelData, valid, value)
			}
		}
	}

	Component {
		id: confirmDialog
		XregCfgDialog {}
	}

	Component {
		id: numberDialog
		NumberSelectorDialog {
			property var cfg
			property var target
			onAccepted: page.pendingWrite = { cfg: cfg, target: target, value: value }
			onClosed: if (page.pendingWrite) Qt.callLater(page.flushPending)
		}
	}

	Loader {
		id: header
		width: page.width
		active: !!page.home && !page.embedded
		sourceComponent: XregHeader {
			svc: page.svc
			home: page.home
			pushed: true
			setupRow: true
			setupId: page.nodeId.split("/")[0]
		}
	}

	// A Column, not a ListView: rows are never destroyed while scrolled away, so a table's unsaved edits stay
	Flickable {
		id: list
		anchors { top: header.bottom; left: parent.left; right: parent.right; bottom: parent.bottom }
		contentHeight: rows.height
		bottomMargin: Theme.geometry_gradientList_bottomMargin
		boundsBehavior: Flickable.StopAtBounds
		clip: true
		ScrollBar.vertical: ScrollBar {}

		Column {
			id: rows
			width: list.width
			ListText {
				text: !schemaItem.valid ? "Settings are not available from the bridge"
						: !page.linkUp ? "No data from regulator" : ""
				preferredVisible: text.length > 0
			}
			XregListSwitch {
				text: "Show descriptions"
				value: page.showHelp
				writeAccessLevel: VenusOS.User_AccessType_User
				preferredVisible: !page.shortPage && page.anyHelp
				onCommanded: function(on) { page.showHelp = on }
			}
			// A row's caption is " " until its row arrives (onLoaded, after the row is created): gui-v2's CaptionLabel
			// reports a binding loop on its height the first time a caption created empty gets text, never on one
			// created with some.
			Repeater {
				model: page.rowModel
				delegate: Loader {
					width: rows.width
					sourceComponent: {
						switch (modelData.kind) {
						case "header": return headerRow
						case "page": return pageRow
						case "toggle": return toggleRow
						case "select": return selectRow
						case "number": return numberRow
						case "action": return actionRow
						case "table": return tableRow
						case "zeroStatus": return zeroStatusRow
						case "kneeStatus": return kneeStatusRow
						case "probes": return probesRow
						case "connection": return connectionRow
						default: return null
						}
					}
					onLoaded: item.row = Qt.binding(() => modelData)
				}
			}
		}
	}
	ViewGradient {
		anchors.bottom: list.bottom
		width: list.width
	}

	component GateWatch: QtObject {
		id: gw
		property var cfg
		readonly property var notes: page.gateRev >= 0 ? page.gateNotes(gw.cfg) : null
		readonly property bool gated: gw.notes !== null
		readonly property string note: gw.notes || ""
	}

	Component {
		id: headerRow
		SectionHeader {
			property var row
			text: row ? row.label : ""
		}
	}

	Component {
		id: toggleRow
		XregListSwitch {
			id: sw
			property var row
			readonly property var cfg: row ? row.cfg : null
			GateWatch { id: gate; cfg: sw.cfg }
			text: cfg ? cfg.label : ""
			caption: row ? page.captionOf(cfg, gate.note) : " "
			writeAccessLevel: VenusOS.User_AccessType_User
			dataItem.uid: cfg ? page.valueUid(cfg.key) : ""
			valueFalse: cfg && cfg.options ? cfg.options[0].value : 0
			valueTrue: cfg && cfg.options ? cfg.options[cfg.options.length - 1].value : 1
			value: dataItem.value === valueTrue
			interactive: dataItem.valid && page.linkUp && !gate.gated
			onCommanded: function(on) {
				const v = on ? valueTrue : valueFalse
				const write = () => {
					page.writeValue(dataItem, v)
					sw.expect(on)
				}
				if (cfg.confirm && (cfg.confirmWhen === undefined || cfg.confirmWhen === v))
					page.confirmThen(cfg, write)
				else
					write()
			}
		}
	}

	Component {
		id: selectRow
		ListRadioButtonGroup {
			id: sel
			property var row
			readonly property var cfg: row ? row.cfg : null
			readonly property bool asks: !!cfg && !!cfg.confirm
			GateWatch { id: gate; cfg: sel.cfg }
			text: cfg ? cfg.label : ""
			caption: row ? page.captionOf(cfg, gate.note) : " "
			writeAccessLevel: VenusOS.User_AccessType_User
			dataItem.uid: cfg ? page.valueUid(cfg.key) : ""
			// A gated option stays listed but cannot be picked, like the app's disabled <option>; readOnly would hide it.
			optionModel: cfg ? cfg.options.map(o => {
				const n = page.gateRev >= 0 ? page.gateNotes(o) : null
				return { display: o.label, value: o.value, readOnly: !!o.disabled, interactive: n === null, caption: n || "" }
			}) : []
			interactive: dataItem.valid && page.linkUp && !gate.gated
			defaultSecondaryText: "--"
			updateDataOnClick: false
			updateCurrentIndexOnClick: !asks
			popDestination: asks ? undefined : null
			onOptionClicked: function(index) {
				const v = optionModel[index].value
				if (asks && (cfg.confirmWhen === undefined || cfg.confirmWhen === v))
					page.confirmThen(cfg, () => page.writeValue(dataItem, v))
				else
					page.writeValue(dataItem, v)
			}
		}
	}

	Component {
		id: numberRow
		ListButton {
			id: nr
			property var row
			readonly property var cfg: row ? row.cfg : null
			GateWatch { id: gate; cfg: nr.cfg }
			text: cfg ? cfg.label : ""
			caption: row ? page.captionOf(cfg, gate.note) : " "
			writeAccessLevel: VenusOS.User_AccessType_User
			secondaryText: cfg ? page.fmt(cfg, valueItem.valid, valueItem.value) : "--"
			interactive: page.linkUp && !gate.gated
			onClicked: page.openNumber(cfg, valueItem)
			VeQuickItem {
				id: valueItem
				uid: nr.cfg ? page.valueUid(nr.cfg.key) : ""
			}
		}
	}

	Component {
		id: actionRow
		ListButton {
			id: ar
			property var row
			readonly property var cfg: row ? row.cfg : null
			readonly property var http: {
				if (!cfg || !cfg.enabledWhen || !httpItem.valid)
					return null
				try { return JSON.parse(httpItem.value) } catch (e) { return null }
			}
			readonly property bool allowed: !cfg || !cfg.enabledWhen
					|| (!!http && Number(http[cfg.enabledWhen.field]) === cfg.enabledWhen.value)
			GateWatch { id: gate; cfg: ar.cfg }
			text: cfg ? cfg.label : ""
			caption: {
				if (!row)
					return " "
				// a state's placeholder label ("---" for Alarm Test idle, as the app's echo shows it) reads as nothing here
				const lbl = cfg && cfg.options && echoItem.valid ? page.optionLabel(cfg, echoItem.value) : ""
				const st = /^[-\u2013\u2014\s]*$/.test(lbl) ? "" : lbl
				const h = page.captionOf(cfg, gate.note)
				return st && h ? st + "\n" + h : (st || h)
			}
			writeAccessLevel: VenusOS.User_AccessType_User
			secondaryText: cfg ? cfg.buttonText : ""
			interactive: page.linkUp && allowed && !gate.gated
			onClicked: page.confirmThen(cfg, () => page.writeValue(actItem, page.tick()))
			VeQuickItem {
				id: actItem
				uid: ar.cfg ? page.svc + "/Settings/Action/" + ar.cfg.key : ""
			}
			VeQuickItem {
				id: echoItem
				uid: ar.cfg && ar.cfg.echo ? page.valueUid(ar.cfg.key) : ""
			}
			VeQuickItem {
				id: httpItem
				uid: ar.cfg && ar.cfg.enabledWhen ? page.httpUid(ar.cfg.enabledWhen.endpoint) : ""
			}
		}
	}

	Component {
		id: pageRow
		ListNavigation {
			property var row
			readonly property var cfg: row ? row.cfg : null
			text: cfg ? cfg.title : ""
			onClicked: Global.pageManager.pushPage("qrc:/xregulator/XregCfgSection.qml",
					{ title: cfg.title, nodeId: cfg.id, svc: page.svc, home: page.home })
		}
	}

	Component {
		id: connectionRow
		ListNavigation {
			property var row
			text: "Connection"
			secondaryText: !connectedItem.valid ? "Bridge not running" : page.linkUp ? "Connected" : "No data from regulator"
			onClicked: Global.pageManager.pushPage("qrc:/xregulator/XregCfgConnection.qml",
					{ title: text, svc: page.svc, home: page.home, setupId: page.nodeId.split("/")[0] })
		}
	}

	Component {
		id: tableRow
		XregCfgTable {
			property var row
			readonly property var cfg: row ? row.cfg : null
			svc: page.svc
			tableCfg: cfg
			controls: cfg ? (cfg.controls || []).map(k => page.items.find(x => x.key === k)).filter(x => !!x) : []
			linkUp: page.linkUp
			showHelp: page.helpOn
			showLabel: !!cfg && !!page.node && cfg.label !== page.node.title
			onWrote: page.wroteSinceOpen = true
		}
	}

	Component {
		id: zeroStatusRow
		ListText {
			id: zs
			property var row
			readonly property var cfg: row ? row.cfg : null
			readonly property var j: {
				if (!jsonItem.valid)
					return null
				try { return JSON.parse(jsonItem.value) } catch (e) { return null }
			}
			function phaseText() {
				const ph = Number(j.capPhase) || 0
				if (ph === 3 && Number(j.capEngOff) === 1)
					return "Letting the reading settle"
				return cfg.strings.ALTZERO_PHASE_TEXT[ph] || ""
			}
			function where() {
				const t = (j.capAltF === null || j.capAltF === undefined) ? j.capBoardF : j.capAltF
				return (Number(j.capEngOff) === 1 ? "engine off" : Math.round(Number(j.capRpm)) + " RPM")
						+ ((t === null || t === undefined) ? "" : ", " + page.tempF(Number(t)).toFixed(0) + page.tempUnit())
			}
			text: {
				if (!j || !cfg)
					return "--"
				if (Number(j.capBusy) === 1)
					return "Measuring: " + (phaseText() || "working") + "."
				if (Number(j.capHeld) === 1)
					return "Measured " + (Number(j.capA) >= 0 ? "+" : "") + Number(j.capA).toFixed(2) + " A ("
							+ where() + "). Press Apply to fold it into the offset."
				const rej = Number(j.capRej) || 0
				if (rej > 0)
					return "Last attempt refused: " + (cfg.strings.ALTZERO_REJ_TEXT[rej] || "")
				const off = Number(j.capOffset)
				return Number(j.capApplied) === 1
						? "Applied " + (Number(j.capAppliedA) >= 0 ? "+" : "") + Number(j.capAppliedA).toFixed(2)
							+ " A; offset now " + (isFinite(off) ? off.toFixed(2) : "?") + " A"
						: "Not measured on this installation"
			}
			VeQuickItem {
				id: jsonItem
				uid: zs.cfg ? page.httpUid(zs.cfg.source) : ""
			}
		}
	}

	Component {
		id: kneeStatusRow
		ListItem {
			id: ks
			property var row
			readonly property var cfg: row ? row.cfg : null
			readonly property var j: {
				if (!jsonItem.valid)
					return null
				try { return JSON.parse(jsonItem.value) } catch (e) { return null }
			}
			readonly property var bins: j && Array.isArray(j.bins) ? j.bins : []
			function age(s) {
				const n = Number(s)
				if (n < 0)
					return "never"
				return n < 90 ? n + "s" : n < 5400 ? Math.round(n / 60) + "m" : Math.round(n / 3600) + "h"
			}
			function statusLine() {
				if (!j)
					return "Status: --"
				if (Number(j.enable) !== 1)
					return "Status: learning off"
				const b = Number(j.activeBin)
				const near = (b >= 0 && bins[b]) ? " near " + bins[b].rpm + " RPM" : ""
				return "Status: " + (Number(j.floorActive) === 1 ? "probing floor" : "watching")
						+ (Number(j.steady) === 1 ? ", steady" : ", settling") + near
			}
			contentItem: Column {
				spacing: Theme.geometry_listItem_content_verticalMargin / 2
				Label {
					width: parent.width
					text: ks.cfg ? ks.cfg.label : ""
					font: ks.font
					wrapMode: Text.Wrap
				}
				Label {
					width: parent.width
					text: page.helpOn && ks.cfg ? ks.cfg.help : ""
					visible: text.length > 0
					wrapMode: Text.Wrap
					color: Theme.color_font_secondary
					font.pixelSize: Theme.font_listItem_caption_size
				}
				GridLayout {
					width: parent.width
					columns: 6
					columnSpacing: Theme.geometry_listItem_content_spacing
					rowSpacing: 2
					Repeater {
						model: ks.cfg ? ks.cfg.headings : []
						delegate: Label {
							text: modelData.replace("°F", page.tempUnit())
							color: Theme.color_font_secondary
							font.pixelSize: Theme.font_size_caption
							Layout.fillWidth: true
							Layout.preferredWidth: 1   // equal columns, so a changing cell never resizes the table
						}
					}
					Repeater {
						model: ks.bins.length * 6
						delegate: Label {
							readonly property int bi: Math.floor(index / 6)
							readonly property var b: ks.bins[bi]
							readonly property bool locked: Number(b.frozen) === 1
							text: {
								switch (index % 6) {
								case 0: return (bi === 0 ? "<" : "") + b.rpm + (bi === ks.bins.length - 1 ? "+" : "")
								case 1: return Number(b.floor).toFixed(1)
								case 2: return Number(b.knee).toFixed(1)
								case 3: return locked ? "yes" : "—"
								case 4: return locked && Number(b.learnT) > 0 ? String(Math.round(page.tempF(Number(b.learnT)))) : "—"
								default: return ks.age(b.ageS)
								}
							}
							font.pixelSize: Theme.font_size_caption
							font.bold: !!ks.j && Number(ks.j.activeBin) === bi
							font.features: { "tnum": 1 }
							Layout.fillWidth: true
							Layout.preferredWidth: 1
						}
					}
				}
				Label {
					width: parent.width
					text: ks.statusLine()
					color: Theme.color_font_secondary
					font.pixelSize: Theme.font_size_caption
				}
			}
			VeQuickItem {
				id: jsonItem
				uid: ks.cfg ? page.httpUid(ks.cfg.source) : ""
			}
		}
	}

	Component {
		id: probesRow
		Column {
			id: pr
			property var row
			readonly property var cfg: row ? row.cfg : null
			readonly property var j: {
				if (!jsonItem.valid)
					return null
				try { return JSON.parse(jsonItem.value) } catch (e) { return null }
			}
			readonly property var probes: j && Array.isArray(j.probes) ? j.probes : []
			readonly property var roleNames: cfg && cfg.strings ? cfg.strings.OW_ROLE_NAMES : []
			readonly property var roleOptions: [{ display: "Unassigned", value: -1 }].concat(
					roleNames.map((n, i) => ({ display: n, value: i })))
			// Rows rebuild only when the set of probes changes, not on every 5 s poll: a rebuild would destroy
			// a row whose role list is open.
			readonly property string idSig: probes.map(p => String(p.id || "").toLowerCase()).join(",")
			readonly property var rowIds: idSig.length ? idSig.split(",") : []
			function probeOf(id) {
				return probes.find(p => String(p.id || "").toLowerCase() === id) || null
			}
			function fmtAge(s) {
				const n = Number(s)
				if (s === null || s === undefined || !isFinite(n))
					return "—"
				if (n < 120)
					return page.figPad(n, 3) + " s"
				if (n < 7200)
					return page.figPad(Math.round(n / 60), 3) + " min"
				return page.figPad((n / 3600).toFixed(1), 5) + " h"
			}
			function statusText() {
				if (!j)
					return "--"
				if (Number(j.scanning) === 1)
					return "Scanning the bus"
				const n = probes.filter(p => p.present).length
				return n + " probe" + (n === 1 ? "" : "s") + " on the bus"
						+ (Number(j.unassigned) > 0 ? ", " + j.unassigned + " unassigned" : "")
			}
			function warnText() {
				if (!j)
					return ""
				const roles = j.roles || {}, en = j.enabled || {}
				const present = probes.filter(p => p.present).map(p => String(p.id || "").toLowerCase())
				const lines = []
				const check = (key, name, on) => {
					if (!on)
						return
					const addr = String(roles[key] || "").toLowerCase()
					if (!addr)
						lines.push(name + " temperature probe is on, but no probe has the " + name + " role. Assign one in the table above.")
					else if (present.indexOf(addr) < 0)
						lines.push("The " + name + " probe (" + addr.slice(-6).toUpperCase() + ") is not on the bus.")
				}
				check("batt", "Battery", Number(en.batt) === 1)
				check("extra", "Extra", Number(en.extra) === 1)
				return lines.join(" ")
			}
			width: parent ? parent.width : 0

			ListText {
				text: pr.cfg && pr.cfg.headings ? pr.cfg.headings.join(" · ") : ""
				secondaryText: pr.statusText()
				caption: !pr.row ? " " : page.helpOn && pr.cfg ? pr.cfg.help : ""
			}
			ListText {
				text: "No temperature probes found on the bus. Check the probe wiring, then press Scan."
				preferredVisible: !!pr.j && pr.probes.length === 0
			}
			Repeater {
				model: pr.rowIds
				delegate: ListRadioButtonGroup {
					property bool primed     // " " caption until created, as for the page's rows
					Component.onCompleted: primed = true
					readonly property string pid: modelData
					readonly property var probe: pr.probeOf(pid)
					readonly property int probeRole: probe ? Number(probe.role) : -1
					readonly property real tF: !probe || probe.tempF === null || probe.tempF === undefined ? NaN : Number(probe.tempF)
					text: pid.slice(-6).toUpperCase() + "   "
							+ (isFinite(tF) ? page.figPad(page.signNum(page.tempF(tF), 1), 6) + " " + page.tempUnit() : "—")
							+ "   " + pr.fmtAge(probe ? probe.ageS : null)
					caption: !primed ? " " : probe && probe.present ? "" : "not on bus"
					writeAccessLevel: VenusOS.User_AccessType_User
					optionModel: pr.roleOptions
					currentIndex: Math.max(0, pr.roleOptions.findIndex(o => o.value === probeRole))
					secondaryText: currentIndex >= 0 ? pr.roleOptions[currentIndex].display : "--"
					interactive: page.linkUp && /^[0-9a-f]{16}$/.test(pid)
					updateCurrentIndexOnClick: false
					onOptionClicked: function(index) {
						const to = pr.roleOptions[index].value
						if (to !== probeRole) {
							page.wroteSinceOpen = true
							roleItem.setValue(JSON.stringify({ id: pid, from: probeRole, to: to }))
						}
					}
				}
			}
			ListText {
				text: pr.warnText()
				preferredVisible: text.length > 0
				secondaryTextColor: Theme.color_warning
			}
			VeQuickItem {
				id: jsonItem
				uid: pr.cfg ? page.httpUid(pr.cfg.source) : ""
			}
			VeQuickItem {
				id: roleItem
				uid: page.svc + "/Settings/ProbeRole"
			}
		}
	}
}
