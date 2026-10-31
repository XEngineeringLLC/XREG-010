import QtQuick
FocusScope {
	property string title
	property color backgroundColor: Theme.color_page_background
	readonly property bool isCurrentPage: true
	property bool animationEnabled
	property var tryPop
	implicitWidth: Theme.geometry_screen_width
	implicitHeight: Theme.geometry_screen_height
}
