import QtQuick
import QtQuick.Templates as T
T.Switch {
	id: root
	checkable: false
	implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset, implicitContentWidth + leftPadding + rightPadding)
	implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset, implicitContentHeight + topPadding + bottomPadding)
	leftPadding: 0
	rightPadding: 0
	topPadding: 0
	bottomPadding: 0
	background: Rectangle {
		implicitWidth: Theme.geometry_switch_groove_width
		implicitHeight: Theme.geometry_switch_groove_height
		radius: Theme.geometry_switch_groove_radius
		color: root.enabled ? (root.checked ? Theme.color_switch_groove_on : Theme.color_switch_groove_off) : Theme.color_switch_groove_disabled
		border.color: root.checked ? Theme.color_switch_groove_border_on : Theme.color_switch_groove_border_off
		border.width: Theme.geometry_switch_groove_border_width
	}
	indicator: Image {
		x: root.checked ? root.background.width - width + Theme.geometry_switch_indicator_shadowOffset + root.leftPadding : -Theme.geometry_switch_indicator_shadowOffset + root.leftPadding
		y: (parent.height / 2) - (height / 2) + Theme.geometry_switch_indicator_shadowOffset
		width: Theme.geometry_switch_indicator_width
		height: Theme.geometry_switch_indicator_width
		source: Qt.resolvedUrl("switch_indicator.png")
	}
}
