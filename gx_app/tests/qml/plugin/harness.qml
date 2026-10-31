import QtQuick
import QtQuick.Window
import Victron.VenusOS

Window {
	id: win
	width: 1024; height: 600; visible: true
	readonly property string svc: "mock/com.victronenergy.switch.xregulator"
	property var schema: JSON.parse(schemaText)
	property int fails: 0
	property var openDialogs: []

	function check(cond, msg) { if (cond) console.log("PASS " + msg); else { fails++; console.log("FAIL " + msg) } }
	function writesTo(path) { return MockStore.writes.filter(w => w.uid === svc + path) }
	function lastWrite(path) { const w = writesTo(path); return w.length ? w[w.length - 1].value : undefined }

	Item { id: dialogParent; anchors.fill: parent }
	Item { id: pageParent; width: 1024; height: 30000 }
	Item { id: homeParent; width: 1024; height: 544 }

	QtObject {
		id: dialogLayer
		function open(comp, props) {
			const d = comp.createObject(dialogParent, props)
			if (!d) { console.log("FAIL dialog create: " + comp.errorString()); fails++; return null }
			d.closed.connect(function() { const i = win.openDialogs.indexOf(d); if (i >= 0) win.openDialogs.splice(i, 1); d.destroy() })
			win.openDialogs.push(d)
			d.open()
			return d
		}
	}
	QtObject {
		id: pageManager
		property var pushed: []
		property var pops: []
		function pushPage(url, props) {
			const u = String(url).replace("qrc:/xregulator/", pluginDir + "/")
			if (props && props.svc === undefined) props.svc = win.svc
			const c = Qt.createComponent(u)
			if (c.status !== Component.Ready) { console.log("FAIL push " + url + ": " + c.errorString()); win.fails++; return null }
			const p = c.createObject(pageParent, props)
			pushed.push(p)
			return p
		}
		function popPage(toPage) { pops.push(toPage) }
	}
	Page { id: hostPage; title: "XREG-010" }

	// -- schema helpers
	function walkNodes(nodes, out) {
		out = out || []
		for (const n of nodes) if (n.children) { out.push(n); walkNodes(n.children, out) }
		return out
	}
	readonly property var pages: walkNodes(schema.outline)
	function allItems() {
		const out = []
		for (const p of pages) for (const c of p.children) if (!c.children) out.push(c)
		return out
	}
	function itemCfg(key) { return allItems().find(it => it.key === key) || null }
	function pageOf(key) { return pages.find(p => p.children.some(c => !c.children && c.key === key)) || null }

	function seed() {
		const put = (p, v) => MockStore.put(svc + p, v)
		put("/Settings/Schema", schemaText)
		put("/Connected", 1)
		put("/Live/BattV", 13.2)
		put("/Live/AppWind", 11.4)
		put("/Live/TrueWind", 18.9)
		put("/History/AppWind", "10.0,10.5,11.0,11.2")
		put("/History/Wind", "18.0,18.5")
		put("/SwitchableOutput/0/State", 0)
		put("/Link/Channel", "gxevents")
		put("/Link/Firmware", "0.0.9")
		for (const it of allItems()) {
			if (it.type === "toggle") put("/Settings/Value/" + it.key, 0)
			else if (it.type === "select") put("/Settings/Value/" + it.key, it.options[0].value)
			else if (it.type === "number") put("/Settings/Value/" + it.key, it.temp ? 180 : Math.max(it.min, Math.min(it.max, 1)))
			else if (it.type === "action" && it.echo) put("/Settings/Value/" + it.key, 0)
		}
		put("/Settings/Value/kneeLearnEnable", 1)
		put("/Settings/Value/PhysicalPanelOverride", 1)
		for (let i = 0; i < 10; i++) {
			put("/Settings/Value/rpmTableRPMPoints" + i, 600 + i * 300)
			put("/Settings/Value/rpmCapCurrentTable" + i, 100)
			put("/Settings/Value/rpmCapKW" + i, 1.3)
			put("/Settings/Value/rpmMinDutyTable" + i, 2.5)
		}
		put("/Settings/Value/LatitudeNMEA", 41.5)
		put("/Settings/Value/LongitudeNMEA", -71.3)
		put("/Settings/Http/zerofitstate", JSON.stringify({ capHeld: 1, capA: 0.42, capEngOff: 1, capAltF: 150, capBusy: 0, capRej: 0 }))
		const bins = []
		for (let i = 0; i < 10; i++) bins.push({ rpm: 600 + i * 300, floor: 2.5, knee: 3.1, frozen: i < 3 ? 1 : 0, learnT: 175, ageS: i * 400 - 1 })
		put("/Settings/Http/kneeLearnState", JSON.stringify({ enable: 1, bins: bins, activeBin: 2, floorActive: 0, steady: 1 }))
		put("/Settings/Http/tempSensors", JSON.stringify({ probes: [{ id: "28ff0011223344aa", role: 1, present: 1, tempF: 77.5, ageS: 3 },
			{ id: "28ff0011223344bb", role: -1, present: 0, tempF: null, ageS: 900 }],
			roles: { batt: "28ff0011223344aa", extra: "" }, enabled: { batt: 1, extra: 1 }, scanning: 0, unassigned: 1 }))
		put("/Settings/Result", JSON.stringify({ seq: 1, key: "x", ok: true, message: "" }))
		seedGates()
	}
	function seedGates() {
		const put = (k, v) => MockStore.put(svc + "/Settings/Value/" + k, v)
		put("BatteryShuntPresent", 1)
		put("ShuntResistanceMicroOhm", 100)
		put("BattMonSrc", 0)
		put("extraTempProbeEnable", 1)
		put("UseFloat", 1)
	}

	function loaders(page) {
		const out = []
		const walk = (it) => { for (const c of it.children) { if (c.sourceComponent !== undefined && c.item !== undefined && c.item && c.item.row !== undefined) out.push(c); else walk(c) } }
		walk(page)
		return out
	}
	function findItem(root, pred) {
		for (const c of root.children) { if (pred(c)) return c; const f = findItem(c, pred); if (f) return f }
		if (root.contentItem && root.contentItem !== root) { if (pred(root.contentItem)) return root.contentItem; const f = findItem(root.contentItem, pred); if (f) return f }
		return null
	}
	function findAll(root, pred, out) {
		out = out || []
		for (const c of root.children) { if (pred(c) && out.indexOf(c) < 0) out.push(c); findAll(c, pred, out) }
		if (root.contentItem && root.contentItem !== root) { if (pred(root.contentItem) && out.indexOf(root.contentItem) < 0) out.push(root.contentItem); findAll(root.contentItem, pred, out) }
		return out
	}

	Timer { id: driver; interval: 40; repeat: true; running: true; onTriggered: win.stepOnce() }
	property var queue: []
	function stepOnce() {
		if (queue.length === 0) { driver.stop(); console.log("DONE fails=" + fails); Qt.callLater(Qt.quit); return }
		const f = queue.shift()
		try { f() } catch (e) { fails++; console.log("FAIL exception " + e + "\n" + e.stack) }
	}
	function later(f) { queue.push(f) }
	function now(fns) { queue.unshift.apply(queue, fns) }
	function acceptTop() { const d = openDialogs[openDialogs.length - 1]; if (!d) return false; d.accept(); return true }
	function openPage(id) {
		const n = pages.find(p => p.id === id)
		return pageManager.pushPage("qrc:/xregulator/XregCfgSection.qml", { title: n ? n.title : id, nodeId: id, height: 30000 })
	}

	Component.onCompleted: {
		Global.dialogLayer = dialogLayer
		Global.pageManager = pageManager
		seed()
		later(() => outlineChecks())
		later(() => homeOpen())
		later(() => {}); later(() => {}); later(() => {})
		later(() => homeChecks())
		later(() => {})
		later(() => homeSwipeChecks())
		later(() => {}); later(() => {})
		later(() => homeSetupChecks())
		later(() => {})
		later(() => pushedHeaderChecks())
		later(() => {})
		later(() => pushedHeaderBack())
		later(() => enableChecks())
		for (const p of pages) {
			if (p.hidden) continue
			const pid = p.id
			later(() => { win.cur = openPage(pid); pageRowsCheck(pages.find(x => x.id === pid), win.cur) })
			later(() => exerciseRows(pages.find(x => x.id === pid)))
			later(() => helpToggle(pages.find(x => x.id === pid), true))
			later(() => helpToggle(pages.find(x => x.id === pid), false))
			later(() => helpToggle(pages.find(x => x.id === pid), true))
			later(() => { seed(); win.cur.destroy() })
		}
		later(() => modeOpen())
		later(() => modeTest())
		later(() => rateCancelCheck())
		later(() => rateAcceptCheck())
		later(() => rateOutsideCheck())
		later(() => { seed(); tableTest() })
		later(() => {})
		later(() => tableSaveCheck())
		later(() => gpsTableTest())
		later(() => {})
		later(() => gpsSaveCheck())
		later(() => connectionTest())
		later(() => celsiusTest())
		later(() => celsiusCheck())
		later(() => {})
		later(() => gateAndResultTest())
		later(() => gatesOpen())
		later(() => gatesTest())
		later(() => captionStubCheck())
		later(() => chartAxisChecks())
		later(() => headerCompactChecks())
	}
	property var cur: null

	// -- outline as gen_settings.py wrote it
	function outlineChecks() {
		const tabs = schema.outline.filter(n => !n.hidden).map(n => n.title)
		check(JSON.stringify(tabs) === JSON.stringify(["Alternator", "Engine", "Battery", "Temperature", "Solar", "Alarms", "Integrations", "System"]),
			"Setup sub-tabs in the app's order: " + tabs.join(", "))
		const alt = schema.outline.find(n => n.id === "alternator")
		check(JSON.stringify(alt.children.filter(c => c.children).map(c => c.title)) === JSON.stringify(["Basic", "RPM Table", "CV Mode"]),
			"Alternator holds Basic, RPM Table, CV Mode")
		const rpm = pages.find(p => p.id === "alternator/rpm_table")
		check(rpm && rpm.children.filter(c => c.children).length === 0, "RPM Table has no page under it (no RPM Table > RPM Table)")
		const t = rpm.children.find(c => c.type === "table")
		check(t && JSON.stringify(t.controls) === '["HiLow","capLimitMode"]', "rpmTable draws Charge Rate and Limit by: " + (t ? t.controls : ""))
		const where = ["HiLow", "capLimitMode"].map(k => pages.filter(p => p.children.some(c => c.key === k)).map(p => p.id).join("|"))
		check(where[0] === "alternator/rpm_table" && where[1] === "alternator/rpm_table", "Charge Rate and Limit by only on the RPM Table page: " + where)
		check(!!itemCfg("@connection") && pageOf("@connection").id === "system" && itemCfg("@connection").group === "Victron GX", "Connection at System > Victron GX")
		check(pageOf("hardwarePresent").id === "system" && itemCfg("hardwarePresent").group === "Bench and Development", "Hardware Present at System > Bench and Development")
		check(pageOf("dvccEn").id === "integrations" && itemCfg("dvccEn").group === "Charge-Limit Follow", "Charge-Limit Follow under Integrations")
		check(pageOf("BulkVoltage").id === "battery/charge_settings/phase_definitions" && itemCfg("BulkVoltage").group === "Bulk Phase",
			"Bulk Voltage at Battery > Charge Settings > Phase Definitions, Bulk Phase")
		check(pageOf("ShuntLead").id === "battery/charge_settings" && itemCfg("ShuntLead").group === "Measurement Sources", "Shunt Location at Battery > Charge Settings, Measurement Sources")
		const shorts = pages.filter(p => p.short && !p.hidden).map(p => p.id)
		console.log("INFO short pages: " + shorts.join(", "))
		check(shorts.indexOf("system") >= 0, "System (Bench and Development) is a short page")
	}

	// -- home page: header, swipe, setup tabs
	property var home: null
	function homeOpen() {
		const c = Qt.createComponent(pluginDir + "/XregHome.qml")
		if (c.status !== Component.Ready) { check(false, "XregHome loads: " + c.errorString()); return }
		home = c.createObject(homeParent, { svc: svc, animationEnabled: false, hostPage: hostPage, width: 1024, height: 544 })
		check(!!home, "home page created")
	}
	property var swipe: null
	property var header: null
	function homeChecks() {
		swipe = findItem(home, c => c.contentModel !== undefined && c.pageInView !== undefined)
		header = findItem(home, c => c.setupTabs !== undefined && c.pick !== undefined)
		check(!!swipe && swipe.count === 4, "swipe view holds 4 pages: " + (swipe ? swipe.count : -1))
		check(!!header && header.current === 0, "header starts on Dashboard")
		const dash = findItem(home, c => c.statusWord !== undefined && c.offText !== undefined)
		check(!!dash, "Dashboard loaded")
		check(!findItem(dash, c => c.text === "Alternator Enable"), "no Alternator Enable box on the Dashboard")
		check(!findItem(dash, c => c.text === "Settings" && c.flat === false), "no Plots / Settings / Cloud buttons on the Dashboard")
		const tile = findItem(dash, c => c.title === "Wind, last hour")
		check(tile && tile.note.indexOf("11.4") === 0 && tile.note.indexOf("apparent") > 0, "wind tile reads apparent wind: '" + (tile ? tile.note : "") + "'")
		check(tile && tile.v.length === 4 && tile.v[3] === 11.2, "wind tile plots /History/AppWind: " + (tile ? tile.v : ""))
		const tabs = findItem(header, c => c.model !== undefined && c.buttonClicked !== undefined && c.model.length === 4)
		check(tabs && tabs.model.map(m => m.value).join("|") === "Dashboard|Plots|Settings|Cloud", "header tabs Dashboard | Plots | Settings | Cloud")
		tabs.clickButton(1)
		win.tabs = tabs
	}
	property var tabs: null
	function homeSwipeChecks() {
		check(swipe.currentIndex === 1 && header.current === 1 && tabs.currentIndex === 1, "Plots tab shows Plots, tab lit")
		const plots = findItem(home, c => c.windTrue !== undefined && c.shortWindow !== undefined)
		check(!!plots && plots.windTrue === false, "Plots opens Wind on apparent")
		swipe.setCurrentIndex(3)
	}
	function homeSetupChecks() {
		check(tabs.currentIndex === 3, "header follows a swipe to Cloud: lit " + tabs.currentIndex)
		swipe.setCurrentIndex(2)
		const setup = findItem(home, c => c.tabs !== undefined && c.tab !== undefined && c.home === home)
		check(!!setup && setup.tabs.length === 8, "Settings page lists 8 sub-tabs")
		const sec = findItem(setup, c => c.nodeId !== undefined && c.embedded === true)
		check(sec && sec.nodeId === "alternator" && sec.rowModel.map(r => r.cfg.title).join("|") === "Basic|RPM Table|CV Mode", "Settings opens on Alternator: " + (sec ? sec.rowModel.map(r => r.kind + ":" + (r.cfg.title || r.cfg.key)).join(",") : ""))
		check(sec && !findItem(sec, c => c.text === "Show descriptions" && c.visible), "Alternator list has no Show descriptions switch")
		check(!findItem(sec, c => c.setupTabs !== undefined), "embedded list draws no second header")
		home.setupTab = 6
		check(sec.nodeId === "integrations" && sec.rowModel.length === 3, "Integrations tab: heading + 2 rows: " + sec.rowModel.map(r => r.kind).join(","))
		// a chart press holds the swipe
		swipe.setCurrentIndex(1)
		win.setupSec = sec
	}
	property var setupSec: null
	function pushedHeaderChecks() {
		const chart = findItem(home, c => c.scrubbed !== undefined && c.hiddenKeys !== undefined)
		const area = chart ? findItem(chart, c => c.hold !== undefined && c.host !== undefined) : null
		check(!!area, "chart scrub area found")
		if (area) {
			area.hold(true)
			check(home.swipeHolds === 1 && !swipe.interactive, "finger on a chart: page swipe off")
			area.hold(true)
			check(home.swipeHolds === 1, "a second press event does not double-count")
			area.hold(false)
			check(home.swipeHolds === 0 && swipe.interactive, "finger lifted: page swipe on")
		}
		home.setupTab = 0
		swipe.setCurrentIndex(2)
		const n = pageManager.pushed.length
		const nav = loaders(setupSec).find(l => l.item.row.kind === "page" && l.item.row.cfg.id === "alternator/rpm_table")
		check(!!nav, "RPM Table row on the Alternator tab")
		nav.item.click()
		check(pageManager.pushed.length === n + 1, "RPM Table row pushes one page")
		win.cur = pageManager.pushed[pageManager.pushed.length - 1]
		check(cur.nodeId === "alternator/rpm_table" && cur.title === "RPM Table" && cur.home === home, "pushed RPM Table page with the home page")
		const h = findItem(cur, c => c.setupTabs !== undefined && c.pick !== undefined)
		check(!!h && h.pushed && h.current === 2 && h.setupTabs.length === 8, "pushed page carries the header with the Setup sub-tabs")
		const bars = findAll(h, c => c.model !== undefined && c.buttonClicked !== undefined && c.currentIndex !== undefined)
		// one row of 8 sub-tabs, or three (3, 3, 2) on a phone held upright (Portrait)
		const sub = bars.slice(1)
		check(sub.length >= 1 && sub.reduce((n, b) => n + b.model.length, 0) === 8 && sub[0].currentIndex === 0, "sub-tab rows: 8 sub-tabs, Alternator lit: " + bars.map(b => b.model.length + "/" + b.currentIndex))
		const t = loaders(cur).find(l => l.item.row.kind === "table")
		check(!!t && t.item.controls.map(c => c.key).join(",") === "HiLow,capLimitMode", "table drawn in place with Charge Rate and Limit by")
		check(!loaders(cur).some(l => l.item.row.cfg && (l.item.row.cfg.key === "HiLow" || l.item.row.cfg.key === "capLimitMode")), "no separate Charge Rate or Limit by row")
		check(loaders(cur).some(l => l.item.row.kind === "kneeStatus") && loaders(cur).some(l => l.item.row.cfg && l.item.row.cfg.key === "ResetLearningTable"), "keep-alive group and Reset Table on the same page")
		win.hbars = bars
	}
	property var hbars: []
	function pushedHeaderBack() {
		const np = pageManager.pops.length
		hbars[1].clickButton(2)
		check(pageManager.pops.length === np + 1 && pageManager.pops[np] === hostPage && home.setupTab === 2 && swipe.currentIndex === 2,
			"Battery in the pushed header: back to the home page, Settings, Battery")
		hbars[0].clickButton(0)
		check(pageManager.pops.length === np + 2 && swipe.currentIndex === 0, "Dashboard in the pushed header: back to the home page's Dashboard")
		cur.destroy()
	}
	function enableChecks() {
		const sw = findItem(header, c => c.checkable !== undefined && c.toggle !== undefined && c.scale > 1)
		check(!!sw && sw.enabled && !sw.checked, "Alternator Enable in the header, off, usable")
		const word = findItem(header, c => c.text === "OFF")
		check(!!word, "state word OFF")
		const n = writesTo("/SwitchableOutput/0/State").length
		sw.clicked()
		check(writesTo("/SwitchableOutput/0/State").length === n + 1 && lastWrite("/SwitchableOutput/0/State") === 1, "a tap writes 1 to /SwitchableOutput/0/State")
		check(sw.checked && !!findItem(header, c => c.text === "ON"), "follows the echo: checked, ON")
		MockStore.put(svc + "/Connected", 0)
		check(!sw.enabled && !!findItem(header, c => c.text === "--"), "no link: disabled, state --")
		MockStore.put(svc + "/Connected", 1)
		MockStore.put(svc + "/SwitchableOutput/0/State", 0)
	}

	// -- every outline page
	function expectedRows(p) {
		const ctl = []
		for (const c of p.children) if (c.type === "table") ctl.push(...(c.controls || []))
		let n = 0, g = null
		for (const c of p.children) {
			if (c.children) { if (!c.hidden) n++; g = null; continue }
			if (c.hidden || ctl.indexOf(c.key) >= 0) continue
			if (c.group && c.group !== g) n++
			g = c.group || null
			n++
		}
		return n
	}
	function pageRowsCheck(p, pg) {
		const ls = loaders(pg)
		check(pg.rowModel.length === expectedRows(p) && ls.length === pg.rowModel.length, p.id + ": " + ls.length + " rows loaded of " + expectedRows(p))
		const sw = findItem(pg, c => c.text === "Show descriptions")
		const anyHelp = p.children.some(c => !c.children && !c.hidden && (!!c.help || (c.type === "table" && c.columns.some(x => !!x.help))))
		check(!!sw && sw.visible === (!p.short && anyHelp), p.id + ": Show descriptions " + (sw.visible ? "shown" : "hidden") + " (short " + !!p.short + ")")
		if (p.short) {
			const rows = ls.filter(l => l.item.row.cfg && l.item.row.cfg.help && l.item.caption !== undefined)
			check(rows.every(l => l.item.caption.indexOf(l.item.row.cfg.help) >= 0 || (l.item.row.cfg.disabledWhen !== undefined)), p.id + ": short page shows every description (" + rows.length + ")")
		}
	}
	function exerciseRows(p) {
		const steps = []
		for (const l of loaders(cur)) {
			const it = l.item
			if (!it || !it.row || !it.row.cfg || it.row.kind === "page") continue
			const cfg = it.row.cfg
			const tag = p.id + "/" + cfg.key
			steps.push(() => seedGates())
			if (cfg.type === "toggle" && cfg.disabledWhen) {
				const n0 = writesTo("/Settings/Value/" + cfg.key).length
				steps.push(() => { it.click(); check(!it.interactive && it.caption === cfg.disabledWhen.note && writesTo("/Settings/Value/" + cfg.key).length === n0, tag + " toggle gated while " + cfg.disabledWhen.key + "=" + cfg.disabledWhen.value) })
			} else if (cfg.type === "toggle") {
				const want = it.checked ? cfg.options[0].value : cfg.options[cfg.options.length - 1].value
				steps.push(() => it.click())
				if (cfg.confirm && (cfg.confirmWhen === undefined || cfg.confirmWhen === want)) steps.push(() => acceptTop(), () => {})
				steps.push(() => check(lastWrite("/Settings/Value/" + cfg.key) === want, tag + " toggle writes " + want))
			} else if (cfg.type === "select") {
				if (cfg.disabledWhen) { steps.push(() => check(!it.interactive && it.caption.length > 0, tag + " gated while " + cfg.disabledWhen.key + "=" + cfg.disabledWhen.value)); continue }
				const last = cfg.options.length - 1
				const asks = !!cfg.confirm && (cfg.confirmWhen === undefined || cfg.confirmWhen === cfg.options[last].value)
				steps.push(() => it.choose(last))
				if (asks) steps.push(() => check(openDialogs.length === 1 && lastWrite("/Settings/Value/" + cfg.key) !== cfg.options[last].value, tag + " asks before writing"), () => acceptTop(), () => {})
				steps.push(() => check(lastWrite("/Settings/Value/" + cfg.key) === cfg.options[last].value, tag + " select writes " + cfg.options[last].value + (asks ? " after confirm" : "")))
			} else if (cfg.type === "number") {
				steps.push(() => check(it.secondaryText !== "--" && it.secondaryText.length > 0, tag + " shows '" + it.secondaryText + "'"))
				steps.push(() => it.click())
				steps.push(() => {
					const d = openDialogs[openDialogs.length - 1]
					check(!!d && d.title === cfg.label && d.to >= d.from, tag + " number dialog " + (d ? d.from + ".." + d.to : "missing"))
					if (d) { d.value = cfg.max; d.accept() }
				})
				steps.push(() => {})
				if (cfg.confirm) steps.push(() => check(openDialogs.length === 1, tag + " confirm after value"), () => acceptTop(), () => {})
				steps.push(() => check(lastWrite("/Settings/Value/" + cfg.key) === cfg.max, tag + " writes " + cfg.max + " (got " + lastWrite("/Settings/Value/" + cfg.key) + ")"))
			} else if (cfg.type === "action") {
				const n0 = () => writesTo("/Settings/Action/" + cfg.key).length
				let before = 0
				steps.push(() => { before = n0(); it.click() })
				if (cfg.confirm) steps.push(() => check(openDialogs.length === 1 && n0() === before, tag + " asks first"), () => acceptTop(), () => {})
				steps.push(() => check(n0() === before + 1, tag + " action fires once"))
			} else if (cfg.type === "info") {
				if (cfg.widget === "zeroStatus") steps.push(() => check(it.text.indexOf("Measured +0.42 A (engine off, 150°F)") === 0, "zeroStatus: " + it.text))
				if (cfg.widget === "kneeStatus") steps.push(() => check(it.statusLine() === "Status: watching, steady near 1200 RPM", "kneeStatus: " + it.statusLine()))
				if (cfg.widget === "connection") steps.push(() => check(it.text === "Connection" && it.secondaryText === "Connected", "connection row: " + it.text + " / " + it.secondaryText))
				if (cfg.widget === "probes") steps.push(() => {
					check(it.statusText() === "1 probe on the bus, 1 unassigned", "probes status: " + it.statusText())
					const rg = findAll(it, c => c.optionModel !== undefined && c.choose !== undefined)
					check(rg.length === 2, "probes: " + rg.length + " probe rows")
					if (rg[0]) rg[0].choose(3)
				}, () => check(lastWrite("/Settings/ProbeRole") === JSON.stringify({ id: "28ff0011223344aa", from: 1, to: 2 }), "probe role write " + lastWrite("/Settings/ProbeRole")))
			} else if (cfg.type === "table") {
				steps.push(() => check(it.tableCfg.key === cfg.key && it.cellText(it.tableCfg.columns[0], 0) !== "--", tag + " table drawn in place: first cell " + it.cellText(it.tableCfg.columns[0], 0)))
			}
		}
		now(steps)
	}

	// -- the RPM table and its buttons, on the RPM Table page
	property var modePage: null
	property var rateButtons: null
	property var capButtons: null
	function ctlRow(label) { return findItem(modePage, c => c.ctl !== undefined && c.ctl && c.ctl.label === label) }
	function buttonsIn(row) { return findItem(row, c => c.buttonClicked !== undefined && c.clickButton !== undefined) }
	property var rpmSection: null
	function modeOpen() {
		rpmSection = openPage("alternator/rpm_table")
		const tl = loaders(rpmSection).find(l => l.item.row.kind === "table")
		check(!!tl, "RPM Table page has the table")
		modePage = tl.item
		check(modePage.controls.map(c => c.key).join(",") === "HiLow,capLimitMode", "controls passed: " + modePage.controls.map(c => c.key))
		const cap = findItem(modePage, c => c.text === "Amps or kW limit and keep-alive per engine speed")
		check(!!cap && cap.visible, "GX caption over the table")
		check(!findItem(modePage, c => c.text === "RPM Table" && c.visible), "no second RPM Table title on the RPM Table page")
	}
	function modeTest() {
		const p = modePage, t = p.tableCfg
		const rpm = t.columns[0], amps = t.columns[1], kw = t.columns[2]
		const rr = ctlRow("Charge Rate"), cr = ctlRow("Limit by")
		check(!!rr && !!cr, "Charge Rate and Limit by rows drawn")
		rateButtons = buttonsIn(rr); capButtons = buttonsIn(cr)
		check(rateButtons.model.length === 2 && rateButtons.model[0].value === "Low" && rateButtons.model[1].value === "High", "rate buttons Low | High")
		check(rateButtons.currentIndex === 0 && capButtons.currentIndex === 0, "Low and Amps lit from the echo")
		check(!rateButtons.enabled && rr.caption.indexOf("Physical Panel Override is on") === 0, "rate greyed with the panel note under Physical Panel Override")
		check(capButtons.enabled, "Limit by not gated by the panel")
		MockStore.put(svc + "/Settings/Value/PhysicalPanelOverride", 0)
		check(rateButtons.enabled && rr.caption === "", "rate usable with the override off")
		p.edit(amps, 1, 90)
		p.edit(rpm, 0, 600)
		capButtons.clickButton(1)
		check(lastWrite("/Settings/Value/capLimitMode") === 1 && p.kwMode, "kW tap writes 1 and switches the table")
		check(p.derived(amps) && !p.derived(kw), "kW mode: kW entered, amps derived")
		check(!("rpmCapCurrentTable1" in p.edits) && ("rpmTableRPMPoints0" in p.edits), "amps edit dropped, RPM edit kept")
		check(openDialogs.length === 0, "no confirm for Limit by")
		check(rpmSection.wroteSinceOpen, "the page beneath knows a write went out (for its refusal toast)")
		const n = MockStore.writes.length
		rateButtons.clickButton(1)
		check(openDialogs.length === 1 && openDialogs[0].body.indexOf("Switching to High opens the High table") === 0, "rate change asks first when dirty: " + (openDialogs[0] ? openDialogs[0].body : ""))
		check(MockStore.writes.length === n, "nothing written before the answer")
		openDialogs[0].reject()
	}
	function rateCancelCheck() {
		const p = modePage
		check(openDialogs.length === 0 && rateButtons.currentIndex === 0 && ("rpmTableRPMPoints0" in p.edits), "cancel: still Low, edits kept: " + rateButtons.currentIndex)
		rateButtons.clickButton(1)
		acceptTop()
	}
	function rateAcceptCheck() {
		const p = modePage
		check(lastWrite("/Settings/Value/HiLow") === 1 && rateButtons.currentIndex === 1, "OK: High written and lit")
		check(Object.keys(p.edits).length === 0 && p.statusText === "", "OK: unsaved edits discarded, status cleared: " + p.statusText)
		const n = MockStore.writes.length
		rateButtons.clickButton(0)
		check(openDialogs.length === 0 && MockStore.writes.length === n + 1 && lastWrite("/Settings/Value/HiLow") === 0, "clean table: Low written at once")
	}
	function rateOutsideCheck() {
		const p = modePage
		check(rateButtons.currentIndex === 0, "Low lit after the clean tap")
		p.edit(p.tableCfg.columns[0], 2, 1234)
		const toasts = Global.toasts.length
		MockStore.put(svc + "/Settings/Value/HiLow", 1)
		check(Object.keys(p.edits).length === 0 && Global.toasts.length === toasts + 1
				&& Global.toasts[toasts].indexOf("Charge Rate is now High") === 0, "rate moved elsewhere: edits discarded, toast: " + Global.toasts[toasts])
		check(rateButtons.currentIndex === 1, "buttons follow the outside change")
		const kw = p.tableCfg.columns[2]
		p.edit(kw, 2, 1.5)
		p.save()
		const v = lastWrite("/Settings/Table/rpmTable")
		const o = v ? JSON.parse(v) : {}
		check(o.rpmCapKW2 === 1.5 && o.capLimitMode === 1 && !("rpmCapCurrentTable0" in o), "kW save: rpmCapKW2=" + o.rpmCapKW2 + " capLimitMode=" + o.capLimitMode)
		check(!("HiLow" in o), "Charge Rate is not part of the table save, as in the app")
		MockStore.put(svc + "/Settings/Value/capLimitMode", 0)
		check(!p.kwMode && capButtons.currentIndex === 0, "echo back to Amps moves the buttons")
		// a refused Charge Rate write toasts on the page the buttons are on
		const n = Global.toasts.length
		MockStore.put(svc + "/Settings/Result", JSON.stringify({ seq: 50, key: "HiLow", ok: false, message: "Settings not armed" }))
		check(Global.toasts.length === n + 1 && Global.toasts[n] === "Settings not armed", "refused Charge Rate write toasts once: " + Global.toasts.slice(n))
		MockStore.put(svc + "/Connected", 0)
		check(!ctlRow("Charge Rate").visible && !ctlRow("Limit by").visible, "rows hidden with no link")
		MockStore.put(svc + "/Connected", 1)
		// unsaved edits survive scrolling the page (rows are never destroyed)
		p.edit(p.tableCfg.columns[0], 5, 2150)
		const fl = findItem(rpmSection, c => c.contentY !== undefined && c.contentHeight !== undefined && c.boundsBehavior !== undefined)
		fl.contentY = Math.max(0, fl.contentHeight - fl.height)
		check(("rpmTableRPMPoints5" in modePage.edits) && loaders(rpmSection).find(l => l.item.row.kind === "table").item === modePage, "scrolled to the end: the table and its edit are still there")
		modePage.discardEdits()
		rpmSection.destroy()
	}
	property var tablePage: null
	function tableTest() {
		const sec = openPage("alternator/rpm_table")
		win.cur = sec
		tablePage = loaders(sec).find(l => l.item.row.kind === "table").item
		const t = tablePage.tableCfg
		const c0 = t.columns[0], c2 = t.columns[2], c3 = t.columns[3]
		check(tablePage.cellText(c0, 0) === "600", "table rpm row0 " + tablePage.cellText(c0, 0))
		check(tablePage.cellText(c2, 0) === "1.32", "table kW derived " + tablePage.cellText(c2, 0))
		MockStore.put(svc + "/Live/BattV", 0)
		check(tablePage.cellText(c2, 0) === "1.20", "table kW at 12 V when IBV reads 0: " + tablePage.cellText(c2, 0))
		MockStore.put(svc + "/Live/BattV", 13.2)
		check(tablePage.gated(c3), "keep-alive column locked while learning on")
		const saveRow = findItem(tablePage, c => c.secondaryText === "Save Table" && c.clicked !== undefined)
		check(!!saveRow && !saveRow.interactive && saveRow.text === "Save Table", "Save Table row, clean: '" + (saveRow ? saveRow.text : "") + "', not interactive")
		tablePage.edit(c0, 0, 500)
		check(tablePage.dirty && tablePage.statusText === "Unsaved changes", "table dirty")
		check(saveRow.interactive && saveRow.text.indexOf(String(Theme.color_warning)) > 0 && saveRow.text.indexOf(">Unsaved changes<") > 0,
			"Save Table row, dirty: interactive, labelled Unsaved changes in the warning colour: " + saveRow.text)
		saveRow.click()
		check(openDialogs.length === 1 && openDialogs[0].body.indexOf("You changed an RPM breakpoint") === 0, "breakpoint confirm shown")
		acceptTop()
	}
	function tableSaveCheck() {
		const v = lastWrite("/Settings/Table/rpmTable")
		check(v !== undefined, "table write sent")
		if (v === undefined) return
		const o = JSON.parse(v)
		const keys = Object.keys(o)
		check(o.rpmTableRPMPoints0 === 500 && o.rpmTableRPMPoints9 === 3300, "table RPM values " + o.rpmTableRPMPoints0 + ".." + o.rpmTableRPMPoints9)
		check(!("rpmCapKW0" in o) && o.rpmCapCurrentTable0 === 100, "table caps A=" + o.rpmCapCurrentTable0 + ", derived kW left to the bridge")
		check(!("rpmMinDutyTable0" in o), "keep-alive omitted while learning owns it")
		check(o.capLimitMode === 0, "capLimitMode included")
		check(keys.length === 21, "table write has " + keys.length + " keys")
		check(tablePage.statusText === "Saving...", "status Saving...")
		tablePage.edit(tablePage.cols[0], 3, 100)
		const n = MockStore.writes.length
		tablePage.save()
		check(MockStore.writes.length === n && Global.toasts[Global.toasts.length - 1].indexOf("RPM values must increase") === 0, "descending RPM refused: " + Global.toasts[Global.toasts.length - 1])
		cur.destroy()
	}
	property var gpsPage: null
	// Show descriptions switched: every row with a description shows it while on, and none while off
	function helpToggle(p, on) {
		if (p.short) return
		const sw = findItem(cur, c => c.text === "Show descriptions" && c.checkable !== undefined)
		if (!sw.visible) return
		cur.showHelp = on
		const rows = loaders(cur).filter(l => l.item.row.cfg && l.item.row.cfg.help && l.item.caption !== undefined && !l.item.row.cfg.disabledWhen
			&& !(l.item.row.cfg.gates || []).length && ["toggle", "select", "number"].indexOf(l.item.row.kind) >= 0)
		check(rows.every(l => (l.item.caption === l.item.row.cfg.help) === on), p.id + ": Show descriptions " + (on ? "on" : "off") + ", " + rows.length + " descriptions " + (on ? "shown" : "hidden"))
	}
	// The stub CaptionLabel carries gui-v2's own height binding (the source of #7's loop); without it no loop can show here.
	// Created with text, as the plugin's rows now are, so this check itself raises no loop.
	function captionStubCheck() {
		const c = Qt.createQmlObject('import QtQuick; import Victron.VenusOS; CaptionLabel { width: 300; text: "one two three" }', pageParent)
		const h1 = c.height
		c.text = ""
		const h0 = c.height
		c.text = "four five six"
		check(h1 > 0 && h0 === 0 && c.height === c.implicitHeight && c.height > 0, "stub CaptionLabel: its implicit height with text, 0 while empty (" + h1 + ", " + h0 + ", " + c.height + ")")
		c.destroy()
	}
	// #6: a zero-based axis never goes below 0
	function chartAxisChecks() {
		const c = Qt.createComponent(pluginDir + "/XregChart.qml").createObject(pageParent, { width: 600, height: 300, xLo: -10, xHi: 0 })
		const zeros = [0, 0, 0, 0]
		const xs = [-3, -2, -1, 0]
		c.rightFromZero = true
		c.lines = [{ key: "spd", label: "Speed", unit: "kn", dec: 1, right: true, x: xs, v: zeros }, { key: "dir", label: "Angle", unit: "°", x: xs, v: [90, 90, 90, 90] }]
		check(c.rAx.lo === 0 && c.rAx.hi === 1 && c.rAx.ticks[0] === 0, "speed axis, every reading 0: " + c.rAx.lo + " to " + c.rAx.hi + ", ticks " + c.rAx.ticks)
		check(c.lAx.lo < 90 && c.lAx.hi > 90, "a flat axis not from zero still opens both ways: " + c.lAx.lo + " to " + c.lAx.hi)
		c.lines = [{ key: "spd", label: "Speed", unit: "kn", right: true, x: xs, v: [6, 7, 8, 7] }]
		check(c.rAx.lo === 0 && c.rAx.hi >= 8, "speed axis with data from 0: " + c.rAx.lo + " to " + c.rAx.hi)
		c.leftFromZero = true
		c.lines = [{ key: "a", label: "A", unit: "A", x: xs, v: [-4, -6, -5, -4] }]
		check(c.lAx.lo <= -6 && c.lAx.hi === 0, "a negative zero-based axis still reaches 0: " + c.lAx.lo + " to " + c.lAx.hi)
		c.destroy()
	}
	// #3: the header and the Settings page's sub-tab row shrink at the 5-inch width only
	function headerCompactChecks() {
		const mk = (w) => Qt.createComponent(pluginDir + "/XregHeader.qml").createObject(pageParent, { svc: svc, width: w, setupRow: true, setupId: "alternator" })
		const h5 = mk(800), h7 = mk(1024), hp = mk(383)
		check(h5.compact && h5.implicitHeight === 80 && h5.rowH === 34, "800 px: compact header " + h5.implicitHeight + " px, rows " + h5.rowH)
		check(!h7.compact && h7.implicitHeight === 6 + 44 + 6 + 40 + 6, "1024 px: header as before, " + h7.implicitHeight + " px")
		check(!hp.compact && hp.rowH === 40, "383 px: portrait header not compacted, rows " + hp.rowH)
		h5.destroy(); h7.destroy(); hp.destroy()
	}
	function gpsTableTest() {
		const sec = openPage("solar")
		win.cur = sec
		const tl = loaders(sec).find(l => l.item.row.kind === "table")
		gpsPage = tl.item
		const t = gpsPage.tableCfg
		check(t.key === "gpsManual" && gpsPage.showLabel && !!findItem(gpsPage, c => c.text === t.label && c.visible), "manual position drawn in place, titled '" + t.label + "'")
		check(gpsPage.cellText(t.columns[0], 0) === "41.500000", "gps lat shown " + gpsPage.cellText(t.columns[0], 0))
		gpsPage.edit(t.columns[0], 0, 40.25)
		const setRow = findItem(gpsPage, c => c.secondaryText === "Set Manual" && c.clicked !== undefined)
		check(!!setRow && setRow.interactive, "Set Manual is a list row, interactive once edited")
		setRow.click()
	}
	function gpsSaveCheck() {
		const v = lastWrite("/Settings/Table/gpsManual")
		check(v === JSON.stringify({ LatitudeNMEA: 40.25, LongitudeNMEA: -71.3 }), "gps write " + v)
		cur.destroy()
	}
	function connectionTest() {
		const sec = pageManager.pushPage("qrc:/xregulator/XregCfgSection.qml", { title: "System", nodeId: "system", height: 30000, home: home })
		const row = loaders(sec).find(l => l.item.row.kind === "connection")
		const n = pageManager.pushed.length
		row.item.click()
		const p = pageManager.pushed[n]
		check(!!p && p.home === home && p.setupId === "system" && !!findItem(p, c => c.setupTabs !== undefined), "Connection opens with the header, System lit")
		const regs = findItem(p, c => c.text === "Regulators" && c.click !== undefined)
		regs.click()
		const r = pageManager.pushed[n + 1]
		check(!!r && r.home === home && r.svc === svc && !!findItem(r, c => c.setupTabs !== undefined), "Regulators opens with the header")
		const sw = findItem(sec, c => c.text === "Show descriptions")
		const hp = loaders(sec).find(l => l.item.row.cfg && l.item.row.cfg.key === "hardwarePresent")
		check(!sw.visible && hp.item.caption === itemCfg("hardwarePresent").help, "System: no Show descriptions switch, Hardware Present's description shown")
		sec.destroy(); p.destroy(); r.destroy()
	}
	function celsiusTest() {
		Global.systemSettings.temperatureUnit = VenusOS.Units_Temperature_Celsius
		win.cPage = openPage("alternator/basic")
	}
	property var cPage: null
	function celsiusCheck() {
		const l = loaders(cPage).find(l => l.item.row.cfg && l.item.row.cfg.key === "TemperatureLimitF")
		MockStore.put(svc + "/Settings/Value/TemperatureLimitF", 212)
		check(l.item.secondaryText === "100 °C", "TemperatureLimitF in C: " + l.item.secondaryText)
		l.item.click()
		const d = openDialogs[openDialogs.length - 1]
		check(d && d.suffix === " °C" && d.value === 100, "C dialog suffix/value " + (d ? d.suffix + "/" + d.value : ""))
		if (d) { d.value = 90; d.accept() }
		later(() => check(lastWrite("/Settings/Value/TemperatureLimitF") === 194, "90 C written as " + lastWrite("/Settings/Value/TemperatureLimitF") + " F"))
		later(() => { Global.systemSettings.temperatureUnit = VenusOS.Units_Temperature_Fahrenheit })
	}
	function gateAndResultTest() {
		const ls = loaders(cPage)
		const ap = ls.find(l => l.item.row.cfg && l.item.row.cfg.key === "altZeroApply")
		check(ap && ap.item.interactive, "Apply usable while a capture is held")
		MockStore.put(svc + "/Settings/Http/zerofitstate", JSON.stringify({ capHeld: 0, capRej: 2, capBusy: 0 }))
		check(ap && !ap.item.interactive, "Apply greyed with nothing held")
		const zs = ls.find(l => l.item.row.cfg && l.item.row.cfg.key === "@zeroStatus")
		check(zs && zs.item.text === "Last attempt refused: reading too large, check which cable the sensor is on", "zero refused text: " + (zs ? zs.item.text : ""))
		cPage.wroteSinceOpen = true
		const n = Global.toasts.length
		MockStore.put(svc + "/Settings/Result", JSON.stringify({ seq: 99, key: "TemperatureLimitF", ok: false, message: "Settings not armed" }))
		check(Global.toasts.length === n + 1 && Global.toasts[n] === "Settings not armed", "failed write toasts: " + Global.toasts[n])
	}
	// The app's feature gates (gen_settings.py GATES), now over Battery's two pages and Alarms
	property var gb: []
	property var ga: null
	function gatesOpen() {
		seed()
		MockStore.put(svc + "/Settings/Value/PhysicalPanelOverride", 0)
		gb = [openPage("battery/charge_settings"), openPage("battery/charge_settings/phase_definitions")]
		ga = openPage("alarms")
	}
	function rowItem(page, key) {
		for (const pg of (Array.isArray(page) ? page : [page])) {
			const l = loaders(pg).find(l => l.item.row.cfg && l.item.row.cfg.key === key)
			if (l) return l.item
		}
		return null
	}
	function noteOf(key, gate) {
		const g = (itemCfg(key).gates || []).find(x => x.name === gate)
		return g ? (g.note || "") : null
	}
	function putV(k, v) { MockStore.put(svc + "/Settings/Value/" + k, v) }
	function expectRows(page, keys, gate, on, tag) {
		for (const k of keys) {
			const it = rowItem(page, k)
			if (!it) { check(false, tag + ": no row " + k); continue }
			if (on) {
				const note = noteOf(k, gate)
				check(note !== null && !it.interactive && it.caption === note, tag + ": " + k + " greyed")
			} else {
				check(it.interactive && (it.caption === "" || it.caption === (itemCfg(k).help || "")), tag + ": " + k + " usable")
			}
		}
	}
	readonly property var shuntRows: ["MaintainMode", "InvertBattAmps", "BatteryCOffset", "ShuntResistanceMicroOhm", "ShuntLead"]
	readonly property var socRows: ["TailCurrent_A", "RebulkCurrent_A"]
	readonly property var extraRows: ["extraTempAlarmHiEnable", "extraTempAlarmHiF", "extraTempAlarmLoEnable", "extraTempAlarmLoF"]
	readonly property var floatRows: ["FloatVoltage", "FLOAT_DURATION"]
	readonly property var neverRows: ["BulkVoltage", "UseFloat", "MinFloatTime", "RebulkVoltage"]
	function zcOption(tag, gatedOn) {
		const sel = rowItem(gb, "UseFloat")
		const o = sel ? sel.optionModel : []
		const note = (itemCfg("UseFloat").options.find(x => x.value === 2).gates || [])[0].note
		if (gatedOn)
			check(o.length === 3 && o[2].interactive === false && o[2].caption === note && o[0].interactive && sel.interactive, tag + ": zero-current float listed but not selectable")
		else
			check(o.length === 3 && o.every(x => x.interactive && x.caption === ""), tag + ": all three float modes selectable")
	}
	function gatesTest() {
		const all = () => {
			expectRows(gb, shuntRows, "no-batt-current", false, "all present")
			expectRows(ga, ["MaximumAllowedBatteryAmps", "SocAlarmLow"], "", false, "all present")
			expectRows(gb, socRows.concat(floatRows, neverRows), "", false, "all present")
			expectRows(ga, extraRows.concat(["CurrentAlarmHigh", "VoltageAlarmLow"]), "", false, "all present")
			zcOption("all present", false)
		}
		const keys = gb.map(p => p.gateKeys.slice().sort().join(","))
		check(keys[0] === "BatteryShuntPresent,ShuntResistanceMicroOhm" && keys[1] === "BattMonSrc,BatteryShuntPresent,PhysicalPanelOverride,ShuntResistanceMicroOhm,UseFloat",
			"battery pages watch " + keys.join(" / "))
		all()
		putV("BattMonSrc", 1)
		putV("BatteryShuntPresent", 0)
		expectRows(gb, shuntRows, "no-batt-current", true, "shunt absent")
		expectRows(ga, ["MaximumAllowedBatteryAmps"], "no-batt-current", true, "shunt absent")
		expectRows(gb, socRows.concat(floatRows, neverRows), "", false, "shunt absent, monitor on")
		zcOption("shunt absent", true)
		const nr = rowItem(gb, "BatteryCOffset"), d0 = openDialogs.length
		nr.click()
		check(openDialogs.length === d0, "shunt absent: Battery Current Offset opens no dialog")
		MockStore.put(svc + "/Settings/Value/PhysicalPanelOverride", 1)
		const mm = rowItem(gb, "MaintainMode")
		check(!mm.interactive && mm.caption === noteOf("MaintainMode", "no-batt-current") + "\n" + itemCfg("MaintainMode").disabledWhen.note,
			"shunt absent + panel override: Maintain Mode shows both notes")
		MockStore.put(svc + "/Settings/Value/PhysicalPanelOverride", 0)
		putV("BatteryShuntPresent", 1)
		putV("ShuntResistanceMicroOhm", 0)
		expectRows(gb, shuntRows, "no-batt-current", true, "resistance 0")
		putV("ShuntResistanceMicroOhm", 100)
		all()
		putV("BatteryShuntPresent", 0)
		putV("BattMonSrc", 0)
		expectRows(gb, socRows, "no-soc-src", true, "no SoC source")
		expectRows(ga, ["SocAlarmLow"], "no-soc-src", true, "no SoC source")
		putV("BattMonSrc", 3)
		expectRows(gb, socRows, "", false, "VE.Direct monitor")
		putV("BattMonSrc", 0)
		putV("BatteryShuntPresent", 1)
		putV("extraTempProbeEnable", 0)
		expectRows(ga, extraRows, "no-extra-temp", true, "Extra probe off")
		putV("extraTempProbeEnable", 1)
		for (const v of [0, 2]) {
			putV("UseFloat", v)
			expectRows(gb, floatRows, "float-gated-fields", true, "UseFloat " + v)
		}
		putV("UseFloat", 1)
		all()
	}
}
