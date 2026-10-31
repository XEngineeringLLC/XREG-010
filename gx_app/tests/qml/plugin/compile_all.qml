import QtQuick
import QtQuick.Window
import Victron.VenusOS

// Every plugin file compiles; the two entry pages also build with a regulator listed.
Window {
	width: 1024; height: 600; visible: true
	Component.onCompleted: {
		let bad = 0
		for (const f of fileList) {
			const c = Qt.createComponent(pluginDir + "/" + f)
			if (c.status !== Component.Ready) { bad++; console.log("FAIL compile " + f + ": " + c.errorString()) }
		}
		MockStore.put("mock/com.victronenergy.xregulator/Units", JSON.stringify([{ uid: "000088621AD9B4DC", name: "Bench", service: "com.victronenergy.switch.xregulator_88621a", instance: 0, connected: true }]))
		const s = Qt.createComponent(pluginDir + "/XregSettings.qml").createObject(contentItem, { width: 1024, height: 544 })
		const home = s ? s.children.find(x => x.item !== undefined && x.item && x.item.swipeHolds !== undefined) : null
		console.log((home && home.item.hostPage === s ? "PASS" : "FAIL") + " XregSettings hosts the home page with itself as hostPage")
		MockStore.put("mock/com.victronenergy.alternator/Serial", "88621AD9B4DC")
		const a = Qt.createComponent(pluginDir + "/XregAlternator.qml").createObject(contentItem, { width: 1024, height: 544, device: { serviceUid: "mock/com.victronenergy.alternator" } })
		const home2 = a ? a.children.find(x => x.item !== undefined && x.item && x.item.swipeHolds !== undefined) : null
		console.log((home2 && home2.item.hostPage === a ? "PASS" : "FAIL") + " XregAlternator hosts the home page with itself as hostPage")
		console.log("DONE compile failures " + bad)
		Qt.callLater(Qt.quit)
	}
}
