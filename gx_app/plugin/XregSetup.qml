import QtQuick
import Victron.VenusOS

// Settings, the home page's third page (XregHome): the app's Setup sub-tabs that hold a GX setting (XregTabs, the
// schema's outline) over the chosen one's page (XregCfgSection, embedded). Deeper pages (Alternator > Basic, RPM Table,
// CV Mode; Battery > Charge Settings) are pushed from it and carry the home page's header.
Item {
	id: setup

	property string svc
	property Item home
	readonly property real m: 14
	readonly property real gap: home && home.compact ? 4 : 8   // the header's 5-inch layout
	readonly property var tabs: {
		if (!schemaItem.valid || !schemaItem.value)
			return []
		try {
			return JSON.parse(schemaItem.value).outline.filter(n => !n.hidden)
		} catch (e) {
			return []
		}
	}
	readonly property int tab: home ? Math.max(0, Math.min(home.setupTab, tabs.length - 1)) : 0

	VeQuickItem { id: schemaItem; uid: setup.svc + "/Settings/Schema" }

	XregTabs {
		id: bar
		x: setup.m
		y: setup.gap
		width: setup.width - 2 * setup.m
		rowH: setup.home && setup.home.compact ? 34 : 40
		model: setup.tabs.map(t => t.title)
		currentIndex: setup.tab
		onPicked: function(index) { if (setup.home) setup.home.setupTab = index }
	}

	XregCfgSection {
		y: setup.tabs.length ? bar.y + bar.height + setup.gap : 0
		width: setup.width
		height: setup.height - y
		embedded: true
		svc: setup.svc
		home: setup.home
		nodeId: setup.tabs.length ? setup.tabs[setup.tab].id : ""
	}
}
