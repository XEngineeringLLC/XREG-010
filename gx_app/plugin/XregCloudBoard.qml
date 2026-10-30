import QtQuick
import Victron.VenusOS

// One leaderboard card on the Cloud page (the web page's leaderboard-wrapper): title, a tab per column when the board
// has several, the top five and, after a gap, your own row when you are not among them. A faint bar behind each row
// carries its size against the card's largest; tap a row for the boat behind it.
Rectangle {
	id: card

	property var board: ({ title: "", info: "", tabs: [] })
	property color brand
	property var fmt: r => r.t      // the page's formatter (a Fahrenheit value in the GX's unit)
	property int tabIndex: 0
	property int expanded: -1
	property bool infoOpen

	readonly property var tab: (board.tabs || [])[Math.min(tabIndex, (board.tabs || []).length - 1)] || { rows: [], gap: 0 }
	readonly property var rows: tab.rows || []

	height: body.height + 20
	radius: Theme.geometry_overviewPage_widget_radius
	color: Theme.color_background_secondary
	onTabIndexChanged: expanded = -1

	function badgeColor(r) {
		return r === 1 ? "#D4AF37" : r === 2 ? "#A8A9AD" : r === 3 ? "#CD7F32" : "transparent"
	}

	Column {
		id: body
		x: 10; y: 10
		width: card.width - 20
		spacing: 6

		Item {
			width: body.width
			height: titleLabel.height
			Label {
				id: titleLabel
				width: parent.width - (info.visible ? info.width + 8 : 0)
				elide: Text.ElideRight
				text: card.board.title
				font.pixelSize: Theme.font_size_body2
				font.bold: true
			}
			Rectangle {
				id: info
				anchors { right: parent.right; verticalCenter: titleLabel.verticalCenter }
				visible: !!card.board.info
				width: 24; height: 24; radius: 12
				color: card.infoOpen ? Theme.color_ok : "transparent"
				border.width: 1
				border.color: Theme.color_ok
				Label {
					anchors.centerIn: parent
					text: "i"
					color: card.infoOpen ? Theme.color_button_down_text : Theme.color_ok
					font.pixelSize: Theme.font_size_caption
					font.bold: true
				}
				MouseArea {
					anchors.fill: parent
					anchors.margins: -8
					onClicked: card.infoOpen = !card.infoOpen
				}
			}
		}
		Label {
			width: body.width
			visible: card.infoOpen
			wrapMode: Text.WordWrap
			text: card.board.info || ""
			color: Theme.color_font_secondary
			font.pixelSize: Theme.font_size_caption
		}
		XregTabs {
			width: body.width
			rowH: 32
			maxPx: Theme.font_size_tiny
			visible: (card.board.tabs || []).length > 1
			model: (card.board.tabs || []).map(t => t.label)
			currentIndex: card.tabIndex
			onPicked: function(index) { card.tabIndex = index }
		}
		Label {
			visible: card.rows.length === 0
			text: "No data yet"
			color: Theme.color_font_secondary
			font.pixelSize: Theme.font_size_body1
		}
		Repeater {
			model: card.rows
			delegate: Column {
				id: rowCol
				required property var modelData
				required property int index
				readonly property bool you: !!modelData.you
				width: body.width
				Label {
					// the web page's "···" before your own row
					visible: card.tab.gap && rowCol.index === card.rows.length - 1
					width: body.width
					horizontalAlignment: Text.AlignHCenter
					text: "···"
					color: Theme.color_font_secondary
					font.pixelSize: Theme.font_size_body1
				}
				Item {
					width: body.width
					height: 34
					Rectangle {
						width: Math.max(0, Math.min(1, rowCol.modelData.w || 0)) * parent.width
						height: parent.height
						radius: 4
						color: rowCol.you ? Qt.rgba(Theme.color_ok.r, Theme.color_ok.g, Theme.color_ok.b, 0.22)
								: Qt.rgba(card.brand.r, card.brand.g, card.brand.b, 0.12)
					}
					Rectangle {
						id: badge
						x: 4
						anchors.verticalCenter: parent.verticalCenter
						width: 26; height: 26; radius: 13
						color: card.badgeColor(rowCol.modelData.r)
						border.width: rowCol.modelData.r > 3 ? 1 : 0
						border.color: Theme.color_font_disabled
						Label {
							anchors.centerIn: parent
							text: rowCol.modelData.r
							color: rowCol.modelData.r <= 3 ? "#272622" : Theme.color_font_primary
							font.pixelSize: Theme.font_size_caption
							font.bold: true
						}
					}
					Row {
						x: badge.x + badge.width + 10
						anchors.verticalCenter: parent.verticalCenter
						width: parent.width - x - valueLabel.width - 12
						spacing: 6
						Label {
							width: Math.min(implicitWidth, parent.width - (youChip.visible ? youChip.width + 6 : 0))
							elide: Text.ElideRight
							text: rowCol.modelData.n
							font.pixelSize: Theme.font_size_body1
							font.bold: rowCol.you
						}
						Rectangle {
							id: youChip
							visible: rowCol.you
							anchors.verticalCenter: parent.verticalCenter
							width: youText.implicitWidth + 10; height: 18; radius: 4
							color: Theme.color_ok
							Label {
								id: youText
								anchors.centerIn: parent
								text: "YOU"
								color: Theme.color_button_down_text
								font.pixelSize: Theme.font_size_micro
								font.bold: true
							}
						}
					}
					XregNum {
						id: valueLabel
						anchors { right: parent.right; rightMargin: 8; verticalCenter: parent.verticalCenter }
						text: card.fmt(rowCol.modelData)
						font.pixelSize: Theme.font_size_body1
						font.bold: true
					}
					MouseArea {
						anchors.fill: parent
						enabled: !!rowCol.modelData.d
						onClicked: card.expanded = card.expanded === rowCol.index ? -1 : rowCol.index
					}
				}
				Label {
					visible: card.expanded === rowCol.index && !!rowCol.modelData.d
					x: 40
					width: body.width - 40
					wrapMode: Text.WordWrap
					text: rowCol.modelData.d || ""
					color: Theme.color_font_secondary
					font.pixelSize: Theme.font_size_caption
					bottomPadding: 4
				}
			}
		}
	}
}
