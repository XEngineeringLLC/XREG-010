import QtQuick
import QtQuick.Templates as T
import Victron.VenusOS

// A settings row whose few choices are side-by-side buttons, the current one lit, drawn as gui-v2's SegmentedButtonRow
// but with a choice's name wrapping inside its button (SegmentedButtonRow elides it). The buttons sit beside the row's
// name when every choice fits there on one line, else on their own line under it. The lit button is currentIndex,
// which the caller binds to the device's value; a tap only emits chosen(index).
ListSetting {
	id: root

	property var options: []          // [{ label, enabled }]
	property int currentIndex: -1
	signal chosen(int index)

	readonly property int count: options.length
	readonly property real pad: Theme.geometry_tabBar_horizontalMargin
	readonly property real besideW: Math.min(320, Math.round(availableWidth / 2))
	readonly property bool beside: count > 0 && count <= 2
			&& options.every(o => fm.advanceWidth(o.label) + 2 * pad <= besideW / count)

	FontMetrics {
		id: fm
		font.family: Global.fontFamily
		font.pixelSize: Theme.font_button_size
	}

	contentItem: Item {
		implicitHeight: root.beside && !captionLabel.visible && nameLabel.lineCount <= 1 ? Math.max(nameLabel.implicitHeight, buttons.height)
				: column.implicitHeight
		Column {
			id: column
			anchors.verticalCenter: parent.verticalCenter
			width: parent.width
			spacing: Theme.geometry_listItem_content_verticalMargin / 2
			Item {
				width: parent.width
				height: root.beside ? Math.max(nameLabel.implicitHeight, buttons.height) : nameLabel.implicitHeight + column.spacing + buttons.height
				Label {
					id: nameLabel
					y: root.beside ? (parent.height - implicitHeight) / 2 : 0
					width: root.beside ? parent.width - buttons.width - root.spacing : parent.width
					text: root.text
					font: root.font
					wrapMode: Text.Wrap
				}
				Row {
					id: buttons
					anchors { right: parent.right; bottom: parent.bottom }
					anchors.bottomMargin: root.beside ? (parent.height - height) / 2 : 0
					width: root.beside ? root.besideW : parent.width
					height: {
						let h = 0
						for (let i = 0; i < rep.count; i++) {
							const b = rep.itemAt(i)
							if (b)
								h = Math.max(h, b.labelH)
						}
						return Math.max(Theme.geometry_button_height, h + Theme.geometry_listItem_content_verticalMargin)
					}
					Repeater {
						id: rep
						model: root.options
						delegate: T.Button {
							id: btn
							readonly property bool lit: index === root.currentIndex
							readonly property real labelH: lbl.implicitHeight
							enabled: root.clickable && modelData.enabled !== false
							width: buttons.width / Math.max(1, rep.count)
							height: buttons.height
							focusPolicy: Qt.StrongFocus
							background: AsymmetricRoundedRectangle {
								width: btn.width
								height: btn.height
								color: !btn.enabled && !btn.lit ? Theme.color_background_disabled
										: btn.pressed || btn.lit ? Theme.color_ok : Theme.color_darkOk
								border.width: Theme.geometry_button_border_width
								border.color: !btn.enabled && !btn.lit ? color : Theme.color_ok
								radius: Theme.geometry_button_radius
								roundedSide: rep.count === 1 ? VenusOS.AsymmetricRoundedRectangle_RoundedSide_All
										: index === 0 ? VenusOS.AsymmetricRoundedRectangle_RoundedSide_Left
										: index === rep.count - 1 ? VenusOS.AsymmetricRoundedRectangle_RoundedSide_Right
										: VenusOS.AsymmetricRoundedRectangle_RoundedSide_NoneHorizontal
							}
							contentItem: Label {
								id: lbl
								x: root.pad
								width: btn.width - 2 * root.pad
								text: modelData.label
								font.pixelSize: Theme.font_button_size
								horizontalAlignment: Text.AlignHCenter
								verticalAlignment: Text.AlignVCenter
								wrapMode: Text.Wrap
								color: !btn.enabled && !btn.lit ? Theme.color_font_disabled
										: btn.pressed || btn.lit ? Theme.color_button_down_text : Theme.color_font_primary
							}
							onClicked: {
								if (root.checkWriteAccessLevel() && btn.enabled && !btn.lit)
									root.chosen(index)
							}
						}
					}
				}
			}
			Label {
				id: captionLabel
				width: parent.width
				text: root.caption
				visible: text.length > 0 && text !== " "
				wrapMode: Text.Wrap
				color: Theme.color_font_secondary
				font.pixelSize: Theme.font_listItem_caption_size
			}
		}
	}
}
