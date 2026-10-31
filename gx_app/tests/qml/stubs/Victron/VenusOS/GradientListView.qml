import QtQuick
ListView {
	width: parent ? parent.width : 0
	height: parent ? parent.height : 0
	boundsBehavior: Flickable.StopAtBounds
	bottomMargin: Theme.geometry_gradientList_bottomMargin
}
