import QtQuick
ListItem {
	id: root
	property string text
	property string caption
	property int showAccessLevel: 0
	property int writeAccessLevel: 1
	readonly property bool userHasWriteAccess: true
	property bool interactive
	readonly property bool clickable: enabled && interactive && userHasWriteAccess
	property color indicatorColor: "transparent"
	function checkWriteAccessLevel() { return true }
}
