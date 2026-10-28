import QtQuick
import Victron.VenusOS

// Settings root (pushed by the home page's Settings button): Connection, then the schema's sections.
Page {
	id: page

	title: "Settings"

	property string svc
	readonly property var sections: {
		if (!schemaItem.valid || !schemaItem.value)
			return []
		try {
			return JSON.parse(schemaItem.value).sections.map(s => ({ id: s.id, title: s.title }))
		} catch (e) {
			return []
		}
	}

	VeQuickItem { id: schemaItem; uid: page.svc + "/Settings/Schema" }
	VeQuickItem { id: connectedItem; uid: page.svc + "/Connected" }

	GradientListView {
		id: list
		anchors.fill: parent
		header: Column {
			width: list.width
			ListNavigation {
				text: "Connection"
				secondaryText: !connectedItem.valid ? "Bridge not running"
						: connectedItem.value === 1 ? "Connected" : "No data from regulator"
				onClicked: Global.pageManager.pushPage("qrc:/xregulator/XregCfgConnection.qml", { title: text, svc: page.svc })
			}
			ListText {
				text: "Settings are not available from the bridge"
				preferredVisible: connectedItem.valid && page.sections.length === 0
			}
		}
		model: page.sections
		delegate: ListNavigation {
			text: modelData.title
			onClicked: Global.pageManager.pushPage("qrc:/xregulator/XregCfgSection.qml",
					{ title: modelData.title, sectionId: modelData.id, svc: page.svc })
		}
	}
}
