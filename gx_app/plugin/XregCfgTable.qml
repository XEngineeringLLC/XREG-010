import QtQuick
import Victron.VenusOS

// A table drawn in place on its settings page (XregCfgSection: RPM Table, the manual position), with the app's buttons
// that sit above it (the schema's "controls": Charge Rate, Limit by). Cells are edited locally and sent together by one
// Save, as the app's Save Table button does, through /Settings/Table/<tableId>.
Item {
	id: page

	property var tableCfg
	property var controls: []   // the items of the app's buttons above the table, which this table draws
	property bool linkUp
	property bool showHelp
	property bool showLabel     // the table's own title, when the page is not already named after it
	signal wrote                // a write went out from here (the page beneath owns Charge Rate's and Limit by's results)
	property var echoes: ({})
	property var aux: ({})
	property var edits: ({})
	property var pending: ({})
	property int rev
	property string statusText
	property bool wroteSinceOpen
	property int lastSeq: -1

	property string svc
	readonly property var cols: tableCfg ? tableCfg.columns : []
	readonly property int nRows: tableCfg ? tableCfg.rows : 0
	readonly property var auxKeys: {
		const out = tableCfg && tableCfg.include ? tableCfg.include.slice() : []
		for (const c of cols)
			if (c.readOnlyWhen && out.indexOf(c.readOnlyWhen.key) < 0)
				out.push(c.readOnlyWhen.key)
		if (tableCfg && tableCfg.capMode && out.indexOf(tableCfg.capMode.key) < 0)
			out.push(tableCfg.capMode.key)
		for (const c of controls)
			for (const k of [c.key, c.disabledWhen ? c.disabledWhen.key : null])
				if (k && out.indexOf(k) < 0)
					out.push(k)
		return out
	}
	readonly property bool kwMode: rev >= 0 && !!tableCfg && !!tableCfg.capMode
			&& aux[tableCfg.capMode.key] === tableCfg.capMode.kw
	// The app's getLiveBatteryV() ends at 12 V when no voltage reads.
	readonly property real battV: battVItem.valid && Number(battVItem.value) > 0 ? Number(battVItem.value) : 12
	readonly property bool dirty: rev >= 0 && Object.keys(edits).length > 0
	// Each column an equal share of the row, widened to its heading on one line, the extra taken evenly from the others;
	// equal shares, the headings wrapping, when the row cannot hold every heading
	readonly property var colW: {
		const n = cols.length
		const avail = width - 2 * Theme.geometry_page_content_horizontalMargin
				- (n - 1) * Theme.geometry_listItem_content_spacing / 2
		const need = cols.map((c, i) => {
			const h = heads.count === n ? heads.itemAt(i) : null
			return h ? Math.ceil(h.implicitWidth) : 0
		})
		if (need.reduce((a, w) => a + w, 0) > avail)
			return need.map(() => Math.floor(avail / n))
		let wide = need.map(() => false)
		for (;;) {
			const k = wide.filter(x => !x).length
			const share = k ? Math.floor((avail - need.reduce((a, w, i) => a + (wide[i] ? w : 0), 0)) / k) : 0
			const next = need.map((w, i) => wide[i] || w > share)
			if (next.every((x, i) => x === wide[i]))
				return need.map((w, i) => wide[i] ? w : share)
			wide = next
		}
	}

	function keyOf(col, r) { return col.keyPattern.replace("{i}", String(r)) }
	function stepDecimals(st) {
		const s = String(st)
		const i = s.indexOf(".")
		return i < 0 ? 0 : s.length - i - 1
	}
	function gated(col) {
		return !!col.readOnlyWhen && aux[col.readOnlyWhen.key] === col.readOnlyWhen.value
	}
	function derived(col) {
		return !!col.capCol && !!tableCfg.capMode && ((col.capCol === "kw") !== kwMode)
	}
	function partner(col) {
		return cols.find(c => c.capCol && c.capCol !== col.capCol)
	}
	function rawValue(col, r) {
		const k = keyOf(col, r)
		if (k in edits)
			return edits[k]
		if (k in pending)
			return pending[k].value
		return echoes[k]
	}
	// Same arithmetic as the app's ampsToKW / kwToAmps, on the live battery voltage.
	function cellValue(col, r) {
		if (rev < 0)
			return undefined
		if (!derived(col))
			return rawValue(col, r)
		const src = partner(col)
		const v = src ? rawValue(src, r) : undefined
		if (v === undefined || v === null)
			return undefined
		if (col.capCol === "kw")
			return isFinite(battV) && v ? Number((battV * v / 1000).toFixed(2)) : 0
		return isFinite(battV) && battV >= 0.5 && v ? Math.round(v * 1000 / battV) : 0
	}
	function cellText(col, r) {
		const v = cellValue(col, r)
		return v === undefined || v === null ? "--" : Units.formatNumber(v, col.decimals || 0)
	}
	function setEcho(k, valid, v) {
		echoes[k] = valid ? v : undefined
		if (k in pending && valid && Math.abs(Number(v) - Number(pending[k].value)) < 0.011)
			delete pending[k]
		// every cell echoes on its own (twice each as the page opens): one redraw of the cells for all of a turn's echoes
		Qt.callLater(bump)
	}
	function bump() { rev++ }
	function isCapControl(c) { return !!tableCfg && !!tableCfg.capMode && c.key === tableCfg.capMode.key }
	function controlIndex(c) { return rev >= 0 ? (c.options || []).findIndex(o => o.value === aux[c.key]) : -1 }
	function controlGated(c) { return rev >= 0 && !!c.disabledWhen && aux[c.disabledWhen.key] === c.disabledWhen.value }
	function optionLabel(c, v) {
		const o = (c.options || []).find(x => x.value === v)
		return o ? o.label : String(v)
	}
	function discardEdits() {
		edits = ({})
		pending = ({})
		if (statusText === "Unsaved changes")
			statusText = ""
	}
	function setAux(k, valid, v) {
		const was = aux[k]
		aux[k] = valid ? v : undefined
		// Charge Rate moved by the panel switch or another screen: the table shown is now the other rate's
		const c = controls.find(x => x.key === k && !isCapControl(x))
		if (c && valid && was !== undefined && was !== v && Object.keys(edits).length > 0) {
			discardEdits()
			Global.showToastNotification(VenusOS.Notification_Warning, c.label + " is now " + optionLabel(c, v)
					+ ", so its table is shown and your unsaved changes were discarded", 6000)
		}
		rev++
	}
	function edit(col, r, v) {
		edits[keyOf(col, r)] = v
		statusText = "Unsaved changes"
		rev++
	}
	// The app's buttons above its table: written at once, on their own, and shown before the echo (the bridge publishes
	// the echo again if the regulator refuses; the settings page this table is on owns the key and toasts the refusal).
	// Limit by drops unsaved edits in the column the new mode derives, as the app's setCapMode() refills it from the
	// other. Charge Rate brings up the other rate's table, so unsaved edits are discarded, after a confirm.
	function setControl(c, v, writer) {
		if (aux[c.key] === v)
			return
		const cap = isCapControl(c)
		const go = () => {
			if (cap) {
				const toKw = v === tableCfg.capMode.kw
				for (const col of cols)
					if (col.capCol && (col.capCol === "kw") !== toKw)
						for (let r = 0; r < nRows; r++)
							delete edits[keyOf(col, r)]
				if (Object.keys(edits).length === 0 && statusText === "Unsaved changes")
					statusText = ""
			} else {
				discardEdits()
			}
			wroteSinceOpen = true
			page.wrote()
			aux[c.key] = v
			writer.setValue(v)
			rev++
		}
		if (!cap && Object.keys(edits).length > 0)
			Global.dialogLayer.open(confirmDialog, { title: c.label, okFn: go,
					body: "Switching to " + optionLabel(c, v) + " opens the " + optionLabel(c, v)
						+ " table, and your unsaved changes to this one are discarded." })
		else
			go()
	}
	function openCell(col, r) {
		const dec = Math.max(col.decimals || 0, stepDecimals(col.step || 1))
		const v = cellValue(col, r)
		Global.dialogLayer.open(cellDialog, {
			title: (nRows > 1 ? (r + 1) + ": " : "") + col.label + (col.unit ? " (" + col.unit + ")" : ""),
			decimals: dec,
			from: col.min !== undefined ? col.min : -2147483647 / Math.pow(10, dec),
			to: col.max !== undefined ? col.max : 2147483647 / Math.pow(10, dec),
			stepSize: col.step || 1,
			value: v === undefined ? (col.min !== undefined ? Math.max(col.min, 0) : 0) : v,
			col: col,
			rowIndex: r
		})
	}
	// The app's validateLearningTable: strictly increasing up to the last non-zero row, zeros only at the end.
	function validate() {
		const vr = tableCfg.validate
		if (!vr)
			return ""
		const col = cols.find(c => c.keyPattern === vr.keyPattern)
		const v = []
		for (let i = 0; i < nRows; i++)
			v.push(Number(cellValue(col, i)) || 0)
		let last = -1
		for (let i = nRows - 1; i >= 0; i--)
			if (v[i] !== 0) { last = i; break }
		for (let i = 1; i <= last; i++)
			if (v[i] <= v[i - 1])
				return "RPM values must increase. Row " + (i + 1) + " (" + v[i] + ") must be greater than Row " + i + " (" + v[i - 1] + ")"
		let seen = false
		for (let i = nRows - 1; i >= 0; i--) {
			if (v[i] !== 0)
				seen = true
			else if (seen)
				return "Zero RPM values are only allowed at the end of the table. Found zero at row " + (i + 1)
		}
		return ""
	}
	function save() {
		const err = validate()
		if (err) {
			Global.showToastNotification(VenusOS.Notification_Warning, err, 6000)
			return
		}
		const out = {}
		for (let r = 0; r < nRows; r++) {
			for (const col of cols) {
				if (col.readOnlyWhen && col.readOnlyWhen.omit && gated(col))
					continue    // a disabled input is not submitted by the app's form either
				if (derived(col))
					continue    // the bridge derives it at send time from IBV, else BatteryV, else 12 V, as the app does
				const v = cellValue(col, r)
				if (v === undefined || v === null) {
					Global.showToastNotification(VenusOS.Notification_Warning, "Waiting for the table from the regulator", 4000)
					return
				}
				out[keyOf(col, r)] = col.send && col.send.integer ? Math.round(v) : v
			}
		}
		for (const k of (tableCfg.include || [])) {
			if (aux[k] === undefined) {
				Global.showToastNotification(VenusOS.Notification_Warning, "Waiting for the table from the regulator", 4000)
				return
			}
			out[k] = aux[k]
		}
		const ci = tableCfg.confirmIfChanged
		const prefix = ci ? ci.keyPattern.replace("{i}", "") : ""
		const changed = ci && Object.keys(edits).some(k => k.indexOf(prefix) === 0 && /^\d+$/.test(k.slice(prefix.length)))
		const go = () => {
			const until = Date.now() + 3000
			for (const k in edits)
				pending[k] = { value: edits[k], until: until }
			edits = ({})
			statusText = "Saving..."
			wroteSinceOpen = true
			page.wrote()
			tableItem.setValue(JSON.stringify(out))
			rev++
			settleTimer.restart()
		}
		if (changed)
			Global.dialogLayer.open(confirmDialog, { title: tableCfg.label, body: ci.text, okFn: go })
		else
			go()
	}
	function handleResult() {
		if (!resultItem.valid)
			return
		let res = null
		try {
			res = JSON.parse(resultItem.value)
		} catch (e) {
			return
		}
		if (!res || res.seq === lastSeq)
			return
		const stale = lastSeq < 0 && !wroteSinceOpen
		lastSeq = res.seq
		if (stale || !tableCfg || res.key !== tableCfg.tableId)
			return
		if (!res.ok) {
			pending = ({})
			statusText = "Unsaved changes"
			Global.showToastNotification(VenusOS.Notification_Warning, res.message || "Not saved", 5000)
			rev++
		}
	}

	VeQuickItem { id: battVItem; uid: page.svc + "/Live/BattV" }
	VeQuickItem { id: tableItem; uid: page.tableCfg ? page.svc + "/Settings/Table/" + page.tableCfg.tableId : "" }
	VeQuickItem {
		id: resultItem
		uid: page.svc + "/Settings/Result"
		onValueChanged: page.handleResult()
	}
	Component.onCompleted: handleResult()

	Timer {
		id: settleTimer
		interval: 3200
		onTriggered: {
			page.pending = ({})
			if (!page.dirty)
				page.statusText = ""
			page.rev++
		}
	}

	Component {
		id: confirmDialog
		XregCfgDialog {}
	}
	Component {
		id: cellDialog
		NumberSelectorDialog {
			property var col
			property int rowIndex
			onAccepted: page.edit(col, rowIndex, value)
		}
	}

	Repeater {
		model: page.auxKeys
		delegate: Item {
			VeQuickItem {
				uid: page.svc + "/Settings/Value/" + modelData
				onValueChanged: page.setAux(modelData, valid, value)
				onValidChanged: page.setAux(modelData, valid, value)
			}
		}
	}

	implicitHeight: content.height

	Column {
		id: content
		width: parent.width
		spacing: Theme.geometry_listItem_content_spacing / 2

		// inset as a list row's label is inside its card
		Label {
			x: Theme.geometry_page_content_horizontalMargin + Theme.geometry_listItem_content_horizontalMargin
			width: content.width - 2 * x
			text: page.tableCfg ? page.tableCfg.label : ""
			visible: page.showLabel && text.length > 0
			wrapMode: Text.Wrap
			font.pixelSize: Theme.font_listItem_primary_size
		}
		// The app's Charge Rate and Limit by buttons above its table
		Repeater {
			model: page.controls
			delegate: ListSetting {
				id: ctlRow
				readonly property var ctl: modelData
				readonly property bool gatedNow: page.controlGated(ctl)
				text: ctl.label
				caption: gatedNow ? (ctl.disabledWhen.note || "") : (page.showHelp && ctl.help ? ctl.help : "")
				preferredVisible: page.linkUp && (ctl.options || []).length > 0
				writeAccessLevel: VenusOS.User_AccessType_User
				interactive: writer.valid && !gatedNow
				VeQuickItem {
					id: writer
					uid: page.svc + "/Settings/Value/" + ctlRow.ctl.key
				}
				// On one line the row keeps a list row's height, its buttons centred in the card as gui-v2's ListButton
				// centres its button; a caption or a wrapped label makes it as tall as its content.
				contentItem: Item {
					implicitHeight: ctlCaption.visible || ctlLabel.lineCount > 1 ? ctlColumn.implicitHeight : 0
					Column {
						id: ctlColumn
						anchors.verticalCenter: parent.verticalCenter
						width: parent.width
						spacing: Theme.geometry_listItem_content_verticalMargin / 2
						Item {
							width: parent.width
							height: Math.max(ctlLabel.implicitHeight, ctlButtons.height)
							Label {
								id: ctlLabel
								anchors.verticalCenter: parent.verticalCenter
								width: parent.width - ctlButtons.width - ctlRow.spacing
								text: ctlRow.text
								font: ctlRow.font
								wrapMode: Text.Wrap
							}
							SegmentedButtonRow {
								id: ctlButtons
								anchors { right: parent.right; verticalCenter: parent.verticalCenter }
								width: Math.min(320, Math.round(parent.width / 2))
								height: Theme.geometry_button_height
								fontPixelSize: Theme.font_button_size
								enabled: ctlRow.clickable
								model: (ctlRow.ctl.options || []).map(o => ({ value: o.label }))
								currentIndex: page.controlIndex(ctlRow.ctl)
								onButtonClicked: function(buttonIndex) {
									if (ctlRow.checkWriteAccessLevel() && ctlRow.clickable)
										page.setControl(ctlRow.ctl, ctlRow.ctl.options[buttonIndex].value, writer)
									// the click sets currentIndex itself; follow the echo again, so a refused or cancelled change shows the old one
									Qt.callLater(() => ctlButtons.currentIndex = Qt.binding(() => page.controlIndex(ctlRow.ctl)))
								}
							}
						}
						Label {
							id: ctlCaption
							width: parent.width
							text: ctlRow.caption
							visible: text.length > 0
							wrapMode: Text.Wrap
							color: Theme.color_font_secondary
							font.pixelSize: Theme.font_listItem_caption_size
						}
					}
				}
			}
		}
		// the caption and the column descriptions, inset as the title
		Label {
			x: Theme.geometry_page_content_horizontalMargin + Theme.geometry_listItem_content_horizontalMargin
			width: content.width - 2 * x
			text: page.tableCfg && page.tableCfg.caption ? page.tableCfg.caption : ""
			visible: text.length > 0
			wrapMode: Text.Wrap
			color: Theme.color_font_secondary
			font.pixelSize: Theme.font_listItem_caption_size
		}
		Repeater {
			model: page.showHelp ? page.cols : []
			delegate: Label {
				x: Theme.geometry_page_content_horizontalMargin + Theme.geometry_listItem_content_horizontalMargin
				width: content.width - 2 * x
				text: modelData.label + (modelData.unit ? " (" + modelData.unit + ")" : "") + ": " + (modelData.help || "")
				visible: !!modelData.help
				wrapMode: Text.Wrap
				color: Theme.color_font_secondary
				font.pixelSize: Theme.font_listItem_caption_size
			}
		}
		Row {
			x: Theme.geometry_page_content_horizontalMargin
			spacing: Theme.geometry_listItem_content_spacing / 2
			Repeater {
				id: heads
				model: page.cols
				delegate: Label {
					width: page.colW[index]
					text: modelData.label + (modelData.unit ? " (" + modelData.unit + ")" : "")
							+ (page.rev >= 0 && page.gated(modelData) ? "\nauto" : "")
					horizontalAlignment: Text.AlignHCenter
					wrapMode: Text.Wrap
					color: Theme.color_font_secondary
					// gui-v2's table heading size: font_size_caption, but never larger than the table's caption above
					font.pixelSize: Math.min(Theme.font_size_caption, Theme.font_listItem_caption_size)
				}
			}
		}

		Repeater {
			model: page.nRows
			delegate: Row {
				id: tableRowItem
				readonly property int r: index
				x: Theme.geometry_page_content_horizontalMargin
				spacing: Theme.geometry_listItem_content_spacing / 2
				Repeater {
					model: page.cols
					delegate: Button {
						id: cell
						readonly property var col: modelData
						readonly property string cellKey: page.keyOf(modelData, tableRowItem.r)
						readonly property bool locked: page.rev >= 0 && (page.gated(modelData) || page.derived(modelData))
						readonly property bool edited: page.rev >= 0 && (cellKey in page.edits || cellKey in page.pending)
						width: page.colW[index]
						flat: locked
						enabled: page.linkUp && !locked
						text: page.rev >= 0 ? page.cellText(modelData, tableRowItem.r) : "--"
						borderColor: edited ? "#00a19a" : (enabled ? Theme.color_ok : Theme.color_font_disabled)
						color: locked ? Theme.color_font_secondary : Theme.color_font_primary
						onClicked: page.openCell(modelData, tableRowItem.r)
						VeQuickItem {
							uid: page.svc + "/Settings/Value/" + cell.cellKey
							onValueChanged: page.setEcho(cell.cellKey, valid, value)
							onValidChanged: page.setEcho(cell.cellKey, valid, value)
						}
					}
				}
			}
		}

		// Save, a list row like the page's action rows; while unsaved or saving, that state is its label
		ListButton {
			readonly property string buttonText: page.tableCfg ? page.tableCfg.buttonText : ""
			text: !page.statusText ? buttonText : "<font color=\"" + (page.statusText === "Unsaved changes"
					? Theme.color_warning : Theme.color_font_secondary) + "\">" + page.statusText + "</font>"
			secondaryText: buttonText
			interactive: page.linkUp && page.dirty
			writeAccessLevel: VenusOS.User_AccessType_User
			onClicked: page.save()
		}
	}
}
