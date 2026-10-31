import QtQuick
import QtQuick.Templates as T
T.ScrollBar {
	id: root
	implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset, implicitContentWidth + leftPadding + rightPadding)
	implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset, implicitContentHeight + topPadding + bottomPadding)
	leftPadding: Theme.geometry_scrollBar_horizontalMargin
	rightPadding: Theme.geometry_scrollBar_horizontalMargin
	enabled: parent.contentHeight > parent.height
	opacity: enabled ? 1.0 : 0.0
	background: Item { implicitWidth: Theme.geometry_scrollBar_bar_width; implicitHeight: 100 }
	contentItem: Rectangle { implicitWidth: Theme.geometry_scrollBar_bar_width; radius: width / 2; color: Theme.color_scrollBar_bar }
}
