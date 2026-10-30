import QtQuick
import Victron.VenusOS

// Cloud, the home page's fourth page (XregHome): the web app's Leaderboards (your standings, the boards by section, the
// sustained-speed scatter) and Fleet Stats, plus a read-only registration line. No Config Sharing, Software Update or
// registration editing on a GX. The bridge fetches everything in one call to the get-leaderboard-bundle edge function
// while this page asks for it (/Ui/Want/Cloud) and hands over /Cloud/Json, already shaped and formatted as the web page
// shows it (xreg_cloud.py); /Cloud/State says why there is nothing to show: 0 off by the Cloud Features setting,
// 1 not registered, 2 loading, 3 ready, 4 offline, 5 error, 6 regulator not connected.
Item {
	id: page

	property string svc
	readonly property real m: 14
	readonly property bool narrow: width < 760
	readonly property real barW: narrow ? width - 2 * m : Math.min(420, (width - 3 * m) * 0.45)
	readonly property real toolX: narrow ? m : m + barW + m
	readonly property real toolY: narrow ? 58 : 8
	readonly property real bodyY: narrow ? 108 : 58
	property int tab: 0
	property int section: 0
	property var model: null
	property real now: Date.now() / 1000

	readonly property bool useC: Global.systemSettings.temperatureUnit === VenusOS.Units_Temperature_Celsius
	readonly property int cloudState: stateItem.valid ? stateItem.value : -1
	readonly property string message: msgItem.valid ? String(msgItem.value) : ""
	readonly property string ago: {
		if (!asOfItem.valid || !(asOfItem.value > 0))
			return ""
		const s = Math.max(0, now - asOfItem.value)
		return s < 90 ? "updated just now" : s < 5400 ? "updated " + Math.round(s / 60) + " min ago"
			: s < 172800 ? "updated " + Math.round(s / 3600) + " h ago" : "updated " + Math.round(s / 86400) + " days ago"
	}

	VeQuickItem { id: want; uid: page.svc + "/Ui/Want/Cloud" }
	VeQuickItem { id: refreshItem; uid: page.svc + "/Cloud/Refresh" }
	VeQuickItem { id: stateItem; uid: page.svc + "/Cloud/State" }
	VeQuickItem { id: msgItem; uid: page.svc + "/Cloud/Message" }
	VeQuickItem { id: asOfItem; uid: page.svc + "/Cloud/AsOf" }
	VeQuickItem {
		id: jsonItem
		uid: page.svc + "/Cloud/Json"
		onValueChanged: page.parse()
	}
	Component.onCompleted: parse()
	Timer {
		interval: 15000
		repeat: true
		triggeredOnStart: true
		running: page.visible && !ScreenBlanker.blanked
		onTriggered: {
			want.setValue(Math.floor(Date.now() / 1000))
			page.now = Date.now() / 1000
		}
	}

	function parse() {
		// read value, not valid: inside a value-change handler valid may not have caught up yet
		const t = jsonItem.value ? String(jsonItem.value) : ""
		if (!t.length)
			return   // the bridge clears it when the page stops asking; keep what is on screen
		try {
			model = JSON.parse(t)
		} catch (e) {
			model = null
		}
	}
	// A value the bridge formatted; a Fahrenheit one is redone in the GX's unit
	function shown(r) {
		return r.f && useC && isFinite(r.v) ? ((r.v - 32) * 5 / 9).toFixed(1) + "°C" : r.t
	}

	readonly property string regLine: {
		if (cloudState === 1)
			return "Not registered"
		return model && model.you && model.you.n ? "Registered as " + model.you.n : ""
	}

	XregTabs {
		x: page.m
		y: 8
		width: page.barW
		model: ["Leaderboards", "Speed", "Fleet"]
		currentIndex: page.tab
		onPicked: function(index) { page.tab = index }
	}
	Row {
		x: page.toolX
		y: page.toolY
		width: page.width - page.toolX - page.m
		height: 40
		spacing: 12
		Column {
			width: parent.width - refresh.width - parent.spacing
			anchors.verticalCenter: parent.verticalCenter
			Label {
				width: parent.width
				horizontalAlignment: Text.AlignRight
				elide: Text.ElideRight
				text: [page.regLine, page.model ? page.ago : ""].filter(s => s.length).join(" · ")
				color: Theme.color_font_secondary
				font.pixelSize: Theme.font_size_caption
			}
			Label {
				width: parent.width
				horizontalAlignment: Text.AlignRight
				elide: Text.ElideRight
				visible: !!page.model && (page.cloudState === 4 || page.cloudState === 5 || page.cloudState === 6)
				text: page.message
				color: Theme.color_orange
				font.pixelSize: Theme.font_size_caption
			}
		}
		Button {
			id: refresh
			width: 100
			height: 40
			flat: false
			text: "Refresh"
			enabled: page.cloudState !== 0 && page.cloudState !== 1 && page.cloudState !== 2
			onClicked: refreshItem.setValue(1)
		}
	}

	// Nothing to show yet, or the cloud is off, unregistered or unreachable
	Label {
		anchors.centerIn: parent
		visible: !page.model
		width: Math.min(560, page.width - 4 * page.m)
		horizontalAlignment: Text.AlignHCenter
		wrapMode: Text.WordWrap
		text: page.message || (stateItem.valid ? "" : "Bridge not running")
		color: Theme.color_font_secondary
		font.pixelSize: Theme.font_size_body2
	}

	Loader {
		x: page.m
		y: page.bodyY
		width: page.width - 2 * page.m
		height: page.height - page.bodyY - page.m
		active: !!page.model
		sourceComponent: [boardsTab, speedTab, fleetTab][page.tab]
	}

	Component {
		id: boardsTab
		Item {
			id: bt
			readonly property var standings: page.model.standings || []
			readonly property int chipN: width >= 960 ? 6 : width >= 600 ? 4 : 3
			readonly property real chipW: (width - (chipN - 1) * 8) / chipN
			readonly property real headH: (standings.length ? 64 + 8 : 0) + (page.model.pulse ? 26 : 0)
			readonly property var sections: page.model.sections || []
			readonly property real listW: page.narrow ? 0 : 168

			Row {
				spacing: 8
				Repeater {
					model: bt.standings.slice(0, bt.chipN)
					delegate: Rectangle {
						required property var modelData
						width: bt.chipW
						height: 64
						radius: Theme.geometry_overviewPage_widget_radius
						color: Theme.color_background_secondary
						Label {
							x: 10; y: 6
							text: "#" + modelData.rank
							color: page.brand
							font.pixelSize: Theme.font_size_body2
							font.bold: true
						}
						XregNum {
							anchors { right: parent.right; rightMargin: 10 }
							y: 8
							width: parent.width - 60
							elide: Text.ElideRight
							text: page.shown(modelData)
							font.pixelSize: Theme.font_size_caption
							font.bold: true
						}
						Label {
							x: 10
							y: 36
							width: parent.width - 20
							elide: Text.ElideRight
							text: modelData.label
							color: Theme.color_font_secondary
							font.pixelSize: Theme.font_size_tiny
						}
					}
				}
			}
			Label {
				y: bt.standings.length ? 72 : 0
				visible: !!page.model.pulse
				width: parent.width
				elide: Text.ElideRight
				text: "Fleet: " + page.model.pulse
				color: Theme.color_font_secondary
				font.pixelSize: Theme.font_size_caption
			}

			// Sections down the left (a row of buttons when narrow), the section's boards on the right
			Column {
				y: bt.headH
				width: bt.listW
				visible: !page.narrow
				spacing: 4
				Repeater {
					model: bt.sections
					delegate: Rectangle {
						required property var modelData
						required property int index
						width: bt.listW
						height: 40
						radius: 6
						color: page.section === index ? Theme.color_ok : Theme.color_background_secondary
						Label {
							anchors { left: parent.left; leftMargin: 12; verticalCenter: parent.verticalCenter }
							text: modelData.title
							color: page.section === index ? Theme.color_button_down_text : Theme.color_font_primary
							font.pixelSize: Theme.font_size_body1
						}
						MouseArea {
							anchors.fill: parent
							onClicked: page.section = index
						}
					}
				}
			}
			XregTabs {
				id: sectionBar
				y: bt.headH
				width: parent.width
				rowH: 36
				visible: page.narrow
				model: bt.sections.map(s => s.title.split(" ")[0])
				currentIndex: page.section
				onPicked: function(index) { page.section = index }
			}
			Flickable {
				id: cards
				x: page.narrow ? 0 : bt.listW + 10
				y: page.narrow ? sectionBar.y + sectionBar.height + 8 : bt.headH
				width: parent.width - x
				height: parent.height - y
				contentHeight: grid.height
				clip: true
				interactive: contentHeight > height
				Grid {
					id: grid
					readonly property int cols: cards.width >= 600 ? 2 : 1
					width: cards.width
					columns: cols
					spacing: 10
					Repeater {
						model: (bt.sections[page.section] || { boards: [] }).boards
						delegate: XregCloudBoard {
							required property var modelData
							width: (grid.width - (grid.cols - 1) * grid.spacing) / grid.cols
							board: modelData
							brand: page.brand
							fmt: page.shown
						}
					}
				}
			}
		}
	}

	Component {
		id: speedTab
		XregCloudSpeed {
			boats: page.model.speed || []
			brand: page.brand
		}
	}

	Component {
		id: fleetTab
		Flickable {
			id: ft
			readonly property var f: page.model.fleet || {}
			readonly property real tileW: (width - 3 * 10) / (page.narrow ? 2 : 4)
			function whole(v) { return Number(Math.round(v)).toLocaleString(Qt.locale(), "f", 0) }
			function val(v, text, unit) { return v === null || v === undefined ? "No data" : text + (unit ? " " + unit : "") }
			readonly property var groups: [
				{ title: "Fleet Overview", tiles: [
					["Registered devices", val(f.totalDevices, whole(f.totalDevices || 0), "")],
					["Active last 7 days", val(f.activeDevices, whole(f.activeDevices || 0), "")]] },
				{ title: "Battery", tiles: [
					["Avg battery voltage", val(f.avgVoltage, Number(f.avgVoltage).toFixed(2), "V")],
					["Avg state of charge", val(f.avgSoc, Number(f.avgSoc).toFixed(1), "%")],
					["Charged energy", val(f.batteryCharged, whole(f.batteryCharged / 1000), "kWh")],
					["Discharged energy", val(f.discharged, whole(f.discharged / 1000), "kWh")]] },
				{ title: "Power Generation", tiles: [
					["Alternator energy", val(f.altEnergy, whole(f.altEnergy / 1000), "kWh")],
					["Solar energy", val(f.solarEnergy, whole(f.solarEnergy / 1000), "kWh")]] },
				{ title: "Fuel & Operation", tiles: [
					["Engine hours", val(f.engineHours, whole(f.engineHours), "hrs")],
					["Alternator on time", val(f.altTime, whole(f.altTime), "hrs")],
					["Engine fuel", val(f.engineFuel, whole(f.engineFuel), "L")],
					["Alternator fuel", val(f.altFuel, whole(f.altFuel), "L")]] },
				{ title: "Navigation", tiles: [
					["Distance traveled", val(f.totalDistance, whole(f.totalDistance), "nm")],
					["Average speed", val(f.avgSpeed, Number(f.avgSpeed).toFixed(1), "kts")]] }
			]
			contentHeight: groupsCol.height
			clip: true
			interactive: contentHeight > height
			Column {
				id: groupsCol
				width: ft.width
				spacing: 6
				Label {
					visible: !!page.model.pulse
					text: page.model.pulse || ""
					color: Theme.color_font_secondary
					font.pixelSize: Theme.font_size_caption
				}
				Repeater {
					model: ft.groups
					delegate: Column {
						required property var modelData
						width: ft.width
						spacing: 6
						Label {
							text: modelData.title
							color: Theme.color_font_secondary
							font.pixelSize: Theme.font_size_caption
						}
						Flow {
							width: ft.width
							spacing: 10
							Repeater {
								model: modelData.tiles
								delegate: Rectangle {
									required property var modelData
									width: ft.tileW
									height: 70
									radius: Theme.geometry_overviewPage_widget_radius
									color: Theme.color_background_secondary
									Label {
										x: 12; y: 8
										width: parent.width - 24
										elide: Text.ElideRight
										text: modelData[0]
										color: Theme.color_font_secondary
										font.pixelSize: Theme.font_size_caption
									}
									Label {
										x: 12; y: 32
										text: modelData[1]
										color: modelData[1] === "No data" ? Theme.color_font_secondary : Theme.color_font_primary
										font.pixelSize: Theme.font_size_body2
										font.bold: modelData[1] !== "No data"
									}
								}
							}
						}
					}
				}
			}
		}
	}

	readonly property color brand: "#00a19a"   // web app --accent
}
