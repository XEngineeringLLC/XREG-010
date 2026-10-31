import QtQuick
import Victron.VenusOS

// The regulator's header, on the home page (XregHome) and on every Settings page pushed from it: the page tabs
// (Dashboard, Plots, Settings, Cloud) and Alternator Enable; with setupRow, a second row of the app's Setup sub-tabs
// (XregTabs) under it. On a pushed page (pushed) a tab goes back to the home page and shows that page or sub-tab.
// Under 600 px wide the page tabs take the first line and Alternator Enable a line of its own. On the 5-inch layout
// (compact) the rows and the gaps between them are shorter, leaving more of its 424 px page to the list or plot below.
Item {
	id: root

	property string svc
	property Item home
	property bool pushed
	property bool setupRow
	property string setupId           // the Setup sub-tab lit in the second row (outline id of a top-level page)
	readonly property int current: pushed ? 2 : (home ? home.current : 0)
	readonly property bool linkUp: connected.valid && connected.value === 1
	readonly property bool portrait: width < 600
	readonly property bool small: width < 900
	readonly property bool compact: small && !portrait
	readonly property real m: 14
	readonly property real gap: compact ? 4 : 6
	readonly property real rowH: compact ? 34 : small ? 40 : 44
	readonly property real enableW: portrait ? width - 2 * m : small ? 250 : 300
	readonly property real tabsH: portrait ? rowH + 6 + rowH : rowH
	readonly property var setupTabs: {
		if (!setupRow || !schemaItem.valid || !schemaItem.value)
			return []
		try {
			return JSON.parse(schemaItem.value).outline.filter(n => !n.hidden)
		} catch (e) {
			return []
		}
	}

	function pick(index) {
		if (!home)
			return
		if (pushed)
			home.back(index)
		else
			home.go(index)
	}

	implicitHeight: gap + tabsH + gap + (setupTabs.length ? setupBar.height + gap : 0)

	VeQuickItem { id: connected; uid: root.svc + "/Connected" }
	VeQuickItem { id: onOff; uid: root.svc + "/SwitchableOutput/0/State" }
	// Alternator Enable as the bridge's fast lane has it while one is sent (the Dashboard asks for it): the same value,
	// holds of a write included, as frames arrive instead of every 500 ms
	VeQuickItem { id: fastLive; uid: root.svc + "/Fast/Live" }
	readonly property var fastOn: {
		if (!fastLive.valid || !fastLive.value)
			return undefined
		try {
			const v = JSON.parse(fastLive.value).OnOff
			return v === null ? undefined : v
		} catch (e) {
			return undefined
		}
	}
	readonly property bool enabledOn: fastOn !== undefined ? fastOn === 1 : onOff.valid && onOff.value === 1
	VeQuickItem { id: schemaItem; uid: root.setupRow ? root.svc + "/Settings/Schema" : "" }

	TabBar {
		id: pages
		x: root.m
		y: root.gap
		width: root.portrait ? root.width - 2 * root.m : root.width - 3 * root.m - root.enableW
		height: root.portrait ? 40 : root.rowH
		fontPixelSize: root.portrait ? Theme.font_size_tiny : root.small ? Theme.font_size_body1 : Theme.font_size_body2
		model: [{ value: "Dashboard" }, { value: "Plots" }, { value: "Settings" }, { value: "Cloud" }]
		currentIndex: root.current
		onButtonClicked: function(buttonIndex) {
			root.pick(buttonIndex)
			// the click set currentIndex itself; follow the page shown again
			Qt.callLater(() => pages.currentIndex = Qt.binding(() => root.current))
		}
	}

	// Alternator Enable, the critical control: a box that turns green with the word ON while the field is enabled
	Rectangle {
		id: enableBox
		readonly property bool on: enableSwitch.enabled && enableSwitch.checked
		x: root.portrait ? root.m : root.width - root.m - width
		y: root.portrait ? pages.y + pages.height + 6 : root.gap
		width: root.enableW
		height: root.rowH
		radius: Theme.geometry_button_radius
		color: on ? Qt.rgba(Theme.color_green.r, Theme.color_green.g, Theme.color_green.b, 0.25) : Theme.color_background_secondary
		border.width: 2
		border.color: on ? Theme.color_green : enableSwitch.enabled ? Theme.color_font_secondary : "transparent"
		Row {
			anchors { left: parent.left; leftMargin: 12; verticalCenter: parent.verticalCenter }
			spacing: 8
			Label {
				anchors.verticalCenter: parent.verticalCenter
				text: "Alternator"
				color: enableSwitch.enabled ? Theme.color_font_primary : Theme.color_font_secondary
				font.pixelSize: root.small ? Theme.font_size_body1 : Theme.font_size_body2
				font.bold: true
			}
			Label {
				anchors.verticalCenter: parent.verticalCenter
				text: !enableSwitch.enabled ? "--" : enableSwitch.checked ? "ON" : "OFF"
				color: enableBox.on ? Theme.color_green : Theme.color_font_secondary
				font.pixelSize: root.small ? Theme.font_size_body1 : Theme.font_size_body2
				font.bold: true
			}
		}
		XregSwitch {
			id: enableSwitch
			anchors { right: parent.right; rightMargin: 14 + (scale - 1) * width / 2; verticalCenter: parent.verticalCenter }
			scale: root.compact ? 1.25 : 1.5
			// the touch area takes the box's full height, as the groove alone is 25 px tall at the 5-inch scale
			topInset: Math.max(0, (enableBox.height / scale - Theme.geometry_switch_groove_height) / 2)
			bottomInset: topInset
			topPadding: topInset
			bottomPadding: topInset
			enabled: root.linkUp
			value: root.enabledOn
			onCommanded: function(on) {
				onOff.setValue(on ? 1 : 0)
				enableSwitch.expect(on)
			}
		}
	}

	XregTabs {
		id: setupBar
		x: root.m
		y: root.gap + root.tabsH + root.gap
		width: root.width - 2 * root.m
		rowH: root.compact ? 34 : 40
		visible: root.setupTabs.length > 0
		model: root.setupTabs.map(t => t.title)
		currentIndex: root.setupTabs.findIndex(t => t.id === root.setupId)
		onPicked: function(index) { if (root.home) root.home.back(2, index) }
	}
}
