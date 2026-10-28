import QtQuick
import QtQuick.Layouts
import Victron.VenusOS

// Table editor (RPM Table, manual position): cells are edited locally and sent together by one Save,
// as the app's Save Table button does, through /Settings/Table/<tableId>.
Page {
	id: page

	property var tableCfg
	property var echoes: ({})
	property var aux: ({})
	property var edits: ({})
	property var pending: ({})
	property int rev
	property string statusText
	property bool showHelp
	property bool wroteSinceOpen
	property int lastSeq: -1

	property string svc
	readonly property bool linkUp: connectedItem.valid && connectedItem.value === 1
	readonly property var cols: tableCfg ? tableCfg.columns : []
	readonly property int nRows: tableCfg ? tableCfg.rows : 0
	readonly property var auxKeys: {
		const out = tableCfg && tableCfg.include ? tableCfg.include.slice() : []
		for (const c of cols)
			if (c.readOnlyWhen && out.indexOf(c.readOnlyWhen.key) < 0)
				out.push(c.readOnlyWhen.key)
		if (tableCfg && tableCfg.capMode && out.indexOf(tableCfg.capMode.key) < 0)
			out.push(tableCfg.capMode.key)
		return out
	}
	readonly property bool kwMode: rev >= 0 && !!tableCfg && !!tableCfg.capMode
			&& aux[tableCfg.capMode.key] === tableCfg.capMode.kw
	// The app's getLiveBatteryV() ends at 12 V when no voltage reads.
	readonly property real battV: battVItem.valid && Number(battVItem.value) > 0 ? Number(battVItem.value) : 12
	readonly property bool dirty: rev >= 0 && Object.keys(edits).length > 0
	readonly property real cellW: Math.floor((width - 2 * Theme.geometry_page_content_horizontalMargin
			- (cols.length - 1) * Theme.geometry_listItem_content_spacing / 2) / Math.max(1, cols.length))

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
		rev++
	}
	function setAux(k, valid, v) {
		aux[k] = valid ? v : undefined
		rev++
	}
	function edit(col, r, v) {
		edits[keyOf(col, r)] = v
		statusText = "Unsaved changes"
		rev++
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

	VeQuickItem { id: connectedItem; uid: page.svc + "/Connected" }
	VeQuickItem { id: battVItem; uid: page.svc + "/Live/BattV" }
	VeQuickItem { id: wantItem; uid: page.svc + "/Ui/Want/Settings" }
	VeQuickItem { id: tableItem; uid: page.tableCfg ? page.svc + "/Settings/Table/" + page.tableCfg.tableId : "" }
	VeQuickItem {
		id: resultItem
		uid: page.svc + "/Settings/Result"
		onValueChanged: page.handleResult()
	}
	Component.onCompleted: handleResult()

	Timer {
		interval: 15000
		repeat: true
		triggeredOnStart: true
		running: page.visible
		onTriggered: wantItem.setValue(Math.floor(Date.now() / 1000))
	}
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

	Flickable {
		anchors.fill: parent
		contentHeight: content.height + Theme.geometry_gradientList_bottomMargin
		boundsBehavior: Flickable.StopAtBounds
		clip: true

		Column {
			id: content
			width: parent.width
			spacing: Theme.geometry_listItem_content_spacing / 2

			ListText {
				text: !page.linkUp ? "No data from regulator"
						: page.tableCfg && page.tableCfg.capMode ? "Limit by" : ""
				secondaryText: !page.linkUp || !page.tableCfg || !page.tableCfg.capMode ? ""
						: page.kwMode ? "kW" : "Amps"
				preferredVisible: text.length > 0
			}
			ListSwitch {
				text: "Show descriptions"
				checkable: true
				checked: page.showHelp
				writeAccessLevel: VenusOS.User_AccessType_User
				onCheckedChanged: page.showHelp = checked
			}
			Repeater {
				model: page.showHelp ? page.cols : []
				delegate: Label {
					x: Theme.geometry_page_content_horizontalMargin
					width: content.width - 2 * Theme.geometry_page_content_horizontalMargin
					text: modelData.label + (modelData.unit ? " (" + modelData.unit + ")" : "") + ": " + (modelData.help || "")
					visible: !!modelData.help
					wrapMode: Text.Wrap
					color: Theme.color_font_secondary
					font.pixelSize: Theme.font_size_caption
				}
			}

			Row {
				x: Theme.geometry_page_content_horizontalMargin
				spacing: Theme.geometry_listItem_content_spacing / 2
				Repeater {
					model: page.cols
					delegate: Label {
						width: page.cellW
						text: modelData.label + (modelData.unit ? " (" + modelData.unit + ")" : "")
								+ (page.rev >= 0 && page.gated(modelData) ? "\nauto" : "")
						horizontalAlignment: Text.AlignHCenter
						wrapMode: Text.Wrap
						color: Theme.color_font_secondary
						font.pixelSize: Theme.font_size_caption
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
							width: page.cellW
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

			RowLayout {
				x: Theme.geometry_page_content_horizontalMargin
				width: content.width - 2 * Theme.geometry_page_content_horizontalMargin
				spacing: Theme.geometry_listItem_content_spacing
				Label {
					text: page.statusText
					color: page.statusText === "Unsaved changes" ? Theme.color_warning : Theme.color_font_secondary
					Layout.fillWidth: true
				}
				Button {
					flat: false
					enabled: page.linkUp && page.dirty
					text: page.tableCfg ? page.tableCfg.buttonText : ""
					onClicked: page.save()
				}
			}
		}
	}
}
