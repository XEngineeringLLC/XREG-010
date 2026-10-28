import QtQuick
import Victron.VenusOS

// OK / Cancel dialog with a scrolling body: the app's warnings run to a dozen lines, and ModalDialog
// clips at the screen height.
ModalDialog {
	id: root

	property string body
	property var okFn: null
	property bool _ok

	dialogDoneOptions: VenusOS.ModalDialog_DoneOptions_OkAndCancel

	contentItem: ModalDialog.FocusableContentItem {
		implicitHeight: Math.min(bodyLabel.implicitHeight + 2 * Theme.geometry_modalDialog_content_spacing,
				Theme.geometry_screen_height * 0.55)

		Flickable {
			anchors {
				fill: parent
				leftMargin: Theme.geometry_modalDialog_content_horizontalMargin
				rightMargin: Theme.geometry_modalDialog_content_horizontalMargin
				topMargin: Theme.geometry_modalDialog_content_spacing
				bottomMargin: Theme.geometry_modalDialog_content_spacing
			}
			clip: true
			contentWidth: width
			contentHeight: bodyLabel.implicitHeight
			boundsBehavior: Flickable.StopAtBounds

			Label {
				id: bodyLabel
				width: parent.width
				text: root.body
				wrapMode: Text.Wrap
				font.pixelSize: Theme.font_dialog_body_size
				color: Theme.color_font_primary
			}
		}
	}

	onAccepted: _ok = true
	// After close, not on accept: DialogLayer destroys "the current dialog" when this one closes, which
	// would take a dialog opened from okFn with it.
	onClosed: if (_ok && okFn) Qt.callLater(okFn)
}
