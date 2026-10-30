import QtQuick
import QtQuick.Shapes
import Victron.VenusOS

// Home-page tile: the web app's boat speed-performance view (Live Data > Boat > Speed). Percent of the learned
// best-ever speed, the sailing polar or motoring speed-vs-RPM curve evaluated at the present wind (or headwind) and
// sea state, and the live point: green while a steady run is being recorded, orange while settling. Math follows
// script.js (parseFrontCsv, frontPred, foldAwaJS, appToTrue, drawPerfPlot, drawMotorPlot, renderPerf); the bridge
// passes PerfLive, MotorLive and /perfcurve.csv through unchanged as /Perf/*, and the regulator's perfRidgeFrac and
// perfIdwPower settings as /Perf/RidgeFrac and /Perf/IdwPower.
Rectangle {
	id: perf

	property string svc
	property bool linkUp
	property bool active
	readonly property color brand: "#00a19a"    // web app --accent, the app's speed number

	radius: Theme.geometry_overviewPage_widget_radius
	color: Theme.color_background_secondary

	VeQuickItem { id: want; uid: perf.svc + "/Ui/Want/Perf" }
	Timer {
		interval: 15000
		repeat: true
		triggeredOnStart: true
		running: perf.active
		onTriggered: want.setValue(Math.floor(Date.now() / 1000))
	}

	VeQuickItem { id: sValid; uid: perf.svc + "/Perf/Live/valid" }
	VeQuickItem { id: sWs; uid: perf.svc + "/Perf/Live/ws" }
	VeQuickItem { id: sWa; uid: perf.svc + "/Perf/Live/wa" }
	VeQuickItem { id: sSpd; uid: perf.svc + "/Perf/Live/spd" }
	VeQuickItem { id: sPct; uid: perf.svc + "/Perf/Live/pct" }
	VeQuickItem { id: sSea; uid: perf.svc + "/Perf/Live/pitchStd" }
	VeQuickItem { id: sSrc; uid: perf.svc + "/Perf/Live/src" }
	VeQuickItem { id: sPts; uid: perf.svc + "/Perf/Live/ptCount" }
	VeQuickItem { id: sSteady; uid: perf.svc + "/Perf/Live/steady" }
	VeQuickItem { id: sSettle; uid: perf.svc + "/Perf/Live/settleSec" }
	VeQuickItem { id: sState; uid: perf.svc + "/Perf/Live/state" }
	VeQuickItem { id: sHours; uid: perf.svc + "/Perf/Live/sailHours" }
	VeQuickItem { id: mValid; uid: perf.svc + "/Perf/Motor/valid" }
	VeQuickItem { id: mRpm; uid: perf.svc + "/Perf/Motor/rpm" }
	VeQuickItem { id: mHw; uid: perf.svc + "/Perf/Motor/headwind" }
	VeQuickItem { id: mSpd; uid: perf.svc + "/Perf/Motor/spd" }
	VeQuickItem { id: mPct; uid: perf.svc + "/Perf/Motor/pct" }
	VeQuickItem { id: mSea; uid: perf.svc + "/Perf/Motor/pitchStd" }
	VeQuickItem { id: mSrc; uid: perf.svc + "/Perf/Motor/src" }
	VeQuickItem { id: mPts; uid: perf.svc + "/Perf/Motor/ptCount" }
	VeQuickItem { id: mSteady; uid: perf.svc + "/Perf/Motor/steady" }
	VeQuickItem { id: mSettle; uid: perf.svc + "/Perf/Motor/settleSec" }
	VeQuickItem { id: mState; uid: perf.svc + "/Perf/Motor/state" }
	VeQuickItem { id: mHours; uid: perf.svc + "/Perf/Motor/motorHours" }
	VeQuickItem { id: engRpm; uid: perf.svc + "/Live/Rpm" }
	VeQuickItem {
		id: curveCsv
		uid: perf.svc + "/Perf/CurveCsv"
		onValueChanged: perf.parseCurve()
	}
	Component.onCompleted: parseCurve()

	function num(it) { return perf.linkUp && it.valid && it.value !== null && it.value !== "" ? Number(it.value) : NaN }

	// Sailing or motoring: follows whichever the regulator grades now; the tile toggle pins one for this visit.
	// A boat with no sailing polar and no sailing hours is a motorboat (the firmware never learns sailing for one).
	readonly property bool sailBoat: num(sPts) > 0 || num(sHours) > 0 || num(sValid) >= 0.5
	property int pinned: -1
	property int autoView: num(sHours) > num(mHours) ? 0 : 1
	// Demo mode grades both at once; then the engine says which one the boat is doing
	readonly property int liveView: num(mValid) >= 0.5 && num(sValid) >= 0.5 ? (num(engRpm) > 0 ? 1 : 0)
		: num(mValid) >= 0.5 ? 1 : num(sValid) >= 0.5 ? 0 : -1
	onLiveViewChanged: if (liveView >= 0) autoView = liveView
	readonly property int view: !sailBoat ? 1 : pinned >= 0 ? pinned : autoView
	property bool trueFrame: false

	readonly property bool lValid: num(view ? mValid : sValid) >= 0.5
	readonly property int lState: isFinite(num(view ? mState : sState)) ? num(view ? mState : sState) | 0 : 3
	readonly property real lPct: num(view ? mPct : sPct)
	readonly property bool graded: lValid && (lState === 0 || lState === 1) && lPct > 0
	readonly property real lSpd: num(view ? mSpd : sSpd)
	readonly property real lSea: num(view ? mSea : sSea)
	readonly property bool lSteady: num(view ? mSteady : sSteady) >= 0.5
	readonly property real lSettle: num(view ? mSettle : sSettle)
	readonly property real lHours: num(view ? mHours : sHours)
	property real settlePeak: 0
	onLSettleChanged: {
		if (!(lSettle > 0) || lSteady)
			settlePeak = 0
		else if (lSettle > settlePeak)
			settlePeak = lSettle
	}

	function stateColor(st) { return st === 0 ? Theme.color_green : st === 1 ? Theme.color_blue : Theme.color_font_secondary }
	readonly property var stateLabels: ["MEASURED", "ESTIMATED", "Learning this operating region", "No reference here yet"]
	function seaWord(s) { return s < 0.5 ? "calm" : s < 1.5 ? "slight" : s < 3 ? "choppy" : s < 5 ? "rough" : "very rough" }

	// ---- best-ever fronts (BEFRONT1 sail + motor blocks), evaluated as script.js frontPred ----
	readonly property var sailScale: [2.0, 12.0, 1.0]     // AWS, AWA, sea: firmware axisScale[] (7_functions.ino)
	readonly property var motorScale: [100.0, 4.0, 1.0]   // RPM, headwind, sea
	VeQuickItem { id: ridgeItem; uid: perf.svc + "/Perf/RidgeFrac" }
	VeQuickItem { id: idwItem; uid: perf.svc + "/Perf/IdwPower" }
	readonly property real ridgeFrac: num(ridgeItem) >= 0 ? num(ridgeItem) : 0.10
	readonly property real idwPower: num(idwItem) > 0 ? num(idwItem) : 2.0
	property var sailFront: []
	property var motorFront: []
	property bool folded: true
	property int curveRev: 0
	function parseCurve() {
		let sail = [], motor = [], cur = null, naxis = 3
		// read value, not valid: inside a value-change handler valid may not have caught up yet
		const txt = curveCsv.value ? String(curveCsv.value) : ""
		for (const line of txt.split("\n")) {
			const t = line.trim()
			if (!t.length)
				continue
			const c = t.split(",")
			if (c[0] === "BEFRONT1") {
				cur = c[1] === "MOTOR" ? motor : sail
				naxis = (+c[2]) || 3
				continue
			}
			if (!cur || c.length < naxis + 1)
				continue
			let x = []
			for (let a = 0; a < naxis; ++a)
				x.push(+c[a])
			const y = +c[naxis]
			if (x.some(isNaN) || isNaN(y))
				continue
			cur.push({ x: x, y: y })
		}
		sailFront = sail
		motorFront = motor
		// the firmware stores the sailing front folded to 0..180 only when the polar is symmetric
		folded = !sail.some(p => p.x[1] < 0 || p.x[1] > 180)
		curveRev++
		refresh()
	}
	// The speed the regulator grades against (FrontQuery's prediction, 7_functions.ino): a ridge-regularised local linear
	// fit through the 48 nearest records, clamped to 0.1 .. 1.25 x the front's fastest; the whole-front inverse-distance
	// average only where that fit is singular. So a 96 % dot sits 4 % under the curve.
	function frontPred(pts, q, sc) {
		const K = 48, N = q.length, n = pts.length
		if (!n)
			return 0
		let inv = []
		for (let a = 0; a < N; ++a)
			inv.push(sc[a] > 1e-9 ? 1 / sc[a] : 1)
		const p = idwPower
		let wsum = 0, acc = 0, exact = -1, maxY = 0, nSel = 0
		let selI = new Array(K), selD = new Array(K)
		for (let i = 0; i < n; ++i) {
			const pt = pts[i]
			let d2 = 0
			for (let a = 0; a < N; ++a) {
				const dx = (q[a] - pt.x[a]) * inv[a]
				d2 += dx * dx
			}
			if (exact < 0) {
				if (d2 < 1e-12) {
					exact = i
				} else {
					const w = 1 / ((p === 2 ? d2 : Math.pow(Math.sqrt(d2), p)) + 1e-9)
					wsum += w
					acc += w * pt.y
				}
			}
			if (pt.y > maxY)
				maxY = pt.y
			if (nSel < K || d2 < selD[K - 1]) {
				let j = nSel < K ? nSel++ : K - 1
				while (j > 0 && selD[j - 1] > d2) {
					selD[j] = selD[j - 1]
					selI[j] = selI[j - 1]
					--j
				}
				selD[j] = d2
				selI[j] = i
			}
		}
		const idw = exact >= 0 ? pts[exact].y : wsum > 0 ? acc / wsum : 0
		const M = N + 1
		let A = [], b = []
		for (let r = 0; r < M; ++r) {
			A.push(new Array(M).fill(0))
			b.push(0)
		}
		for (let k = 0; k < nSel; ++k) {
			const pt = pts[selI[k]]
			let phi = [1], d2 = 0
			for (let a = 0; a < N; ++a) {
				const dx = (pt.x[a] - q[a]) * inv[a]
				phi.push(dx)
				d2 += dx * dx
			}
			const w = 1 / (d2 + 1e-9)
			for (let r = 0; r < M; ++r) {
				for (let c = r; c < M; ++c)
					A[r][c] += w * phi[r] * phi[c]
				b[r] += w * phi[r] * pt.y
			}
		}
		let tr = 0
		for (let r = 1; r < M; ++r) {
			for (let c = 0; c < r; ++c)
				A[r][c] = A[c][r]
			tr += A[r][r]
		}
		for (let a = 1; a < M; ++a)
			A[a][a] += ridgeFrac * tr / N
		for (let col = 0; col < M; ++col) {
			let pv = col
			for (let r = col + 1; r < M; ++r)
				if (Math.abs(A[r][col]) > Math.abs(A[pv][col]))
					pv = r
			if (Math.abs(A[pv][col]) < 1e-12)
				return idw
			if (pv !== col) {
				const t = A[col]; A[col] = A[pv]; A[pv] = t
				const tb = b[col]; b[col] = b[pv]; b[pv] = tb
			}
			for (let r = col + 1; r < M; ++r) {
				const f = A[r][col] / A[col][col]
				for (let c = col; c < M; ++c)
					A[r][c] -= f * A[col][c]
				b[r] -= f * b[col]
			}
		}
		let sol = new Array(M)
		for (let r = M - 1; r >= 0; --r) {
			let s = b[r]
			for (let c = r + 1; c < M; ++c)
				s -= A[r][c] * sol[c]
			sol[r] = s / A[r][r]
		}
		return Math.min(Math.max(sol[0], 0.1), 1.25 * maxY)
	}
	function foldAwa(a) {
		if (!folded)
			return a
		let pa = a
		while (pa < 0) pa += 360
		while (pa >= 360) pa -= 360
		return pa > 180 ? 360 - pa : pa
	}
	function appToTrue(awa, bs, aws) {
		const r = Math.PI / 180, ca = Math.cos(Math.abs(awa) * r)
		const tws = Math.sqrt(Math.max(0, aws * aws + bs * bs - 2 * aws * bs * ca))
		let twa = 0
		if (tws > 1e-6)
			twa = Math.acos(Math.max(-1, Math.min(1, (aws * ca - bs) / tws))) / r
		return awa < 0 ? -twa : twa
	}

	// The curve sweep costs (sweep steps x front points): redo it only when its inputs move noticeably, checked every
	// 2 s and at once on a view, frame or curve change
	property var curve: ({ pts: [], maxS: 4, maxR: 3600 })
	property string curveKey
	function refresh() {
		const sea = Math.round((lSea || 0) * 10) / 10
		const key = view + "|" + curveRev + "|" + trueFrame + "|" + sea + "|" + ridgeFrac + "|" + idwPower + "|"
			+ (view ? Math.round(num(mHw) * 2 || 0) : Math.round(num(sWs) * 2 || 0))
		if (key === curveKey)
			return
		curveKey = key
		let pts = [], maxS = 4, maxR = 3600
		if (view === 0 && sailFront.length >= 2) {
			const aws = num(sWs) || 0
			let lo = 999, hi = -999
			for (const p of sailFront) {
				lo = Math.min(lo, p.x[1])
				hi = Math.max(hi, p.x[1])
			}
			const st = Math.max(1, (hi - lo) / 90)
			for (let a = lo; a <= hi + 1e-6; a += st) {
				const s = frontPred(sailFront, [aws, foldAwa(a), sea], sailScale)
				if (s <= 0)
					continue
				pts.push([trueFrame ? appToTrue(a, s, aws) : a, s])
				maxS = Math.max(maxS, s)
			}
		} else if (view === 1 && motorFront.length >= 2) {
			const hw = num(mHw) || 0
			let lo = 1e9, hi = -1e9
			for (const p of motorFront) {
				lo = Math.min(lo, p.x[0])
				hi = Math.max(hi, p.x[0])
			}
			const st = Math.max(20, (hi - lo) / 120)
			for (let r = lo; r <= hi + 1e-6; r += st) {
				const v = frontPred(motorFront, [r, hw, sea], motorScale)
				if (v <= 0)
					continue
				pts.push([r, v])
				maxS = Math.max(maxS, v)
				maxR = Math.max(maxR, r)
			}
		}
		curve = { pts: pts, maxS: maxS, maxR: maxR }
	}
	onViewChanged: refresh()
	onTrueFrameChanged: refresh()
	Timer {
		interval: 2000
		repeat: true
		triggeredOnStart: true
		running: perf.visible
		onTriggered: perf.refresh()
	}
	readonly property bool haveCurve: curve.pts.length > 1
	readonly property real maxS: Math.ceil(Math.max(curve.maxS, lValid && lSpd > 0 ? lSpd : 0))
	readonly property real maxR: Math.ceil(Math.max(curve.maxR, view === 1 && lValid ? num(mRpm) || 0 : 0) / 500) * 500

	// ---- layout: header, then the app's four corners around the plot ----
	readonly property real k: Math.max(0.7, Math.min(1, width / 430, height / 348))
	function fs(px, floor) { return Math.max(floor, Math.round(px * k)) }
	readonly property real headH: 40
	readonly property real side: Math.round(84 * k)
	readonly property real toggleH: 28
	readonly property real plotS: Math.max(60, Math.min(height - headH - toggleH - 10, width - 20 - 2 * side))

	Label {
		x: 12
		y: 10
		text: "Speed"
		color: Theme.color_font_secondary
		font.pixelSize: Theme.font_size_caption
	}
	// Settle status: Settled, or Settling with the seconds left before recording starts
	Row {
		x: 72
		y: 8
		height: 22
		spacing: 6
		visible: isFinite(perf.lSettle) && perf.lSettle >= 0
		Item {
			width: 18
			height: 18
			anchors.verticalCenter: parent.verticalCenter
			Rectangle {
				anchors.centerIn: parent
				visible: perf.lSteady || !(perf.lSettle > 0)
				width: 10; height: 10; radius: 5
				color: perf.lSteady ? Theme.color_green : Theme.color_orange
			}
			Shape {
				anchors.fill: parent
				visible: !perf.lSteady && perf.lSettle > 0
				preferredRendererType: Shape.CurveRenderer
				ShapePath {
					strokeColor: Qt.rgba(Theme.color_orange.r, Theme.color_orange.g, Theme.color_orange.b, 0.3)
					strokeWidth: 2.5
					fillColor: "transparent"
					PathAngleArc { centerX: 9; centerY: 9; radiusX: 7; radiusY: 7; startAngle: 0; sweepAngle: 360 }
				}
				ShapePath {
					strokeColor: Theme.color_orange
					strokeWidth: 2.5
					fillColor: "transparent"
					capStyle: ShapePath.RoundCap
					PathAngleArc {
						centerX: 9; centerY: 9; radiusX: 7; radiusY: 7; startAngle: -90
						sweepAngle: perf.settlePeak > 0 ? 360 * perf.lSettle / perf.settlePeak : 0
					}
				}
			}
			Label {
				anchors.centerIn: parent
				visible: !perf.lSteady && perf.lSettle > 0
				text: Math.ceil(perf.lSettle)
				font.pixelSize: 9
				font.bold: true
			}
		}
		Label {
			anchors.verticalCenter: parent.verticalCenter
			text: perf.lSteady ? "Settled" : "Settling"
			color: perf.lSteady ? Theme.color_green : Theme.color_orange
			font.pixelSize: Theme.font_size_caption
			font.bold: true
		}
	}
	TextMetrics { id: motoringTm; font.family: Global.fontFamily; font.pixelSize: Theme.font_size_tiny; text: "Motoring" }
	SegmentedButtonRow {
		anchors { right: parent.right; rightMargin: 8; top: parent.top; topMargin: 6 }
		// never so narrow that "Motoring" loses its 12 px each side, as the 5-inch tile's 0.7 scale made it
		width: Math.max(Math.round(190 * perf.k), 2 * Math.ceil(motoringTm.advanceWidth + 24))
		height: 30
		visible: perf.sailBoat
		fontPixelSize: Theme.font_size_tiny
		model: [{ value: "Sailing" }, { value: "Motoring" }]
		currentIndex: perf.view
		onButtonClicked: function(buttonIndex) { perf.pinned = buttonIndex }
	}

	Item {
		id: body
		x: 10
		y: perf.headH
		width: perf.width - 20
		height: perf.height - perf.headH - 8

		// Top left: conditions
		Column {
			width: perf.side
			Label {
				width: parent.width
				wrapMode: Text.WordWrap
				text: perf.view ? "Engine RPM" : "Apparent wind"
				color: Theme.color_font_secondary
				font.pixelSize: perf.fs(14, 12)
			}
			XregNum {
				template: perf.view ? "8888" : "88 kn"
				text: !perf.lValid ? "--" : perf.view ? Math.round(perf.num(mRpm)) : Math.round(perf.num(sWs) || 0) + " kn"
				font.pixelSize: perf.fs(26, 18)
				font.bold: true
			}
			Row {
				spacing: 4
				XregNum {
					template: perf.view ? "88 kn" : "888°"
					text: {
						if (!perf.lValid)
							return ""
						return perf.view ? Math.round(Math.abs(perf.num(mHw) || 0)) + " kn" : Math.round(perf.num(sWa) || 0) + "°"
					}
					color: Theme.color_font_secondary
					font.pixelSize: perf.fs(14, 12)
				}
				Label {
					visible: perf.view && perf.lValid
					text: (perf.num(mHw) || 0) < 0 ? "tailwind" : "headwind"
					color: Theme.color_font_secondary
					font.pixelSize: perf.fs(14, 12)
				}
			}
		}
		// Top right: sea state
		Column {
			anchors.right: parent.right
			width: perf.side
			Label {
				width: parent.width
				horizontalAlignment: Text.AlignRight
				text: "Sea state"
				color: Theme.color_font_secondary
				font.pixelSize: perf.fs(14, 12)
			}
			XregNum {
				anchors.right: parent.right
				template: "88.8°"
				text: isFinite(perf.lSea) ? perf.lSea.toFixed(1) + "°" : "--"
				font.pixelSize: perf.fs(26, 18)
				font.bold: true
			}
			Label {
				width: parent.width
				horizontalAlignment: Text.AlignRight
				text: isFinite(perf.lSea) ? perf.seaWord(perf.lSea) : ""
				color: Theme.color_font_secondary
				font.pixelSize: perf.fs(14, 12)
			}
		}
		// Bottom left: % of best and what it is based on
		Column {
			anchors.bottom: parent.bottom
			width: perf.side
			XregNum {
				template: "100%"
				text: perf.graded ? Math.round(perf.lPct) + "%" : "--"
				color: perf.graded ? perf.stateColor(perf.lState) : Theme.color_font_primary
				font.pixelSize: perf.fs(34, 24)
				font.bold: true
			}
			Label {
				text: "of best"
				color: Theme.color_font_secondary
				font.pixelSize: perf.fs(14, 12)
			}
			Label {
				width: parent.width
				wrapMode: Text.WordWrap
				text: perf.lValid ? perf.stateLabels[perf.lState] || "" : ""
				color: perf.stateColor(perf.lState)
				font.pixelSize: perf.fs(12, 11)
			}
		}
		// Bottom right: data maturity
		Column {
			anchors { right: parent.right; bottom: parent.bottom }
			width: perf.side
			XregNum {
				anchors.right: parent.right
				template: "88888"
				text: isFinite(perf.num(perf.view ? mPts : sPts)) ? perf.num(perf.view ? mPts : sPts) | 0 : "--"
				font.pixelSize: perf.fs(20, 16)
				font.bold: true
			}
			Label {
				width: parent.width
				horizontalAlignment: Text.AlignRight
				wrapMode: Text.WordWrap
				text: "best surface points"
				color: Theme.color_font_secondary
				font.pixelSize: perf.fs(12, 11)
			}
			XregNum {
				anchors.right: parent.right
				template: "8888.8"
				text: !isFinite(perf.lHours) ? "--" : perf.lHours.toFixed(1)
				font.pixelSize: perf.fs(20, 16)
				font.bold: true
			}
			Label {
				width: parent.width
				horizontalAlignment: Text.AlignRight
				text: perf.view ? "motoring hours" : "sailing hours"
				color: Theme.color_font_secondary
				font.pixelSize: perf.fs(12, 11)
			}
		}

		// Plot box, centred between the corner columns
		Item {
			id: box
			x: (body.width - width) / 2
			y: 0
			width: perf.plotS
			height: perf.plotS

			// Sailing polar: rings, spokes every 45 degrees, best-ever line (mirrored when symmetric), live dot
			Item {
				id: polar
				anchors.fill: parent
				visible: perf.view === 0
				readonly property real cx: width / 2
				readonly property real cy: height * 0.54
				readonly property real rr: Math.min(width, height) * 0.42
				function pt(ang, s) {
					const r = s / perf.maxS * rr, a = ang * Math.PI / 180
					return Qt.point(cx + r * Math.sin(a), cy - r * Math.cos(a))
				}
				Repeater {
					model: 4
					delegate: Item {
						required property int index
						readonly property real r: polar.rr * (index + 1) / 4
						Rectangle {
							x: polar.cx - parent.r
							y: polar.cy - parent.r
							width: 2 * parent.r
							height: width
							radius: parent.r
							color: "transparent"
							border.width: 1
							border.color: Theme.color_card_separator
						}
						XregNum {
							x: polar.cx + 3
							y: polar.cy - parent.r + 1
							template: "88 kn"
							text: Math.round(perf.maxS * (index + 1) / 4) + " kn"
							color: Theme.color_font_disabled
							font.pixelSize: 10
						}
					}
				}
				Shape {
					anchors.fill: parent
					preferredRendererType: Shape.CurveRenderer
					ShapePath {
						strokeColor: Theme.color_card_separator
						strokeWidth: 1
						fillColor: "transparent"
						PathMultiline {
							paths: {
								let p = []
								for (const dg of [0, 45, 90, 135, 180, -45, -90, -135])
									p.push([Qt.point(polar.cx, polar.cy), polar.pt(dg, perf.maxS)])
								return p
							}
						}
					}
					ShapePath {
						strokeColor: Theme.color_blue
						strokeWidth: 2.2
						fillColor: "transparent"
						joinStyle: ShapePath.RoundJoin
						capStyle: ShapePath.RoundCap
						PathMultiline {
							paths: {
								if (perf.view !== 0 || !perf.haveCurve)
									return []
								const one = perf.curve.pts.map(q => polar.pt(q[0], q[1]))
								return perf.folded ? [one, perf.curve.pts.map(q => polar.pt(-q[0], q[1]))] : [one]
							}
						}
					}
				}
				Label {
					anchors.horizontalCenter: parent.horizontalCenter
					y: polar.cy - polar.rr - height - 1
					text: "0° into wind"
					color: Theme.color_font_secondary
					font.pixelSize: 10
				}
				Label {
					anchors.horizontalCenter: parent.horizontalCenter
					y: polar.cy + polar.rr + 1
					text: "180°"
					color: Theme.color_font_secondary
					font.pixelSize: 10
				}
				// Halfway down the lower half, but never over the speed readout, and inside the circle: on a small polar
				// (5-inch) it drops to the readout's foot, shrinks and breaks onto two lines
				Label {
					id: noSail
					readonly property real maxW: polar.rr * 1.5
					x: polar.cx - width / 2
					y: Math.max(polar.cy + polarSpd.height / 2 + 2, (polar.height + polar.rr - height) / 2)
					width: Math.min(implicitWidth, maxW)
					visible: !perf.haveCurve
					text: noSailTm.advanceWidth <= maxW ? noSailTm.text : "No sailing\ndata yet"
					horizontalAlignment: Text.AlignHCenter
					color: Theme.color_font_secondary
					font.pixelSize: perf.fs(Theme.font_size_caption, 11)
					TextMetrics { id: noSailTm; font: noSail.font; text: "No sailing data yet" }
				}
				Rectangle {
					readonly property point at: polar.pt(perf.trueFrame ? perf.appToTrue(perf.num(sWa) || 0, perf.lSpd || 0, perf.num(sWs) || 0)
						: perf.num(sWa) || 0, perf.lSpd || 0)
					visible: perf.view === 0 && perf.lValid
					x: at.x - width / 2
					y: at.y - height / 2
					width: 13; height: 13; radius: 6.5
					color: perf.lSteady ? Theme.color_green : Theme.color_orange
					border.width: 2
					border.color: Theme.color_white
				}
				Column {
					id: polarSpd
					x: polar.cx - width / 2
					y: polar.cy - height / 2
					XregNum {
						anchors.horizontalCenter: parent.horizontalCenter
						template: "88.8"
						text: perf.lValid && isFinite(perf.lSpd) ? perf.lSpd.toFixed(1) : "--"
						color: perf.brand
						font.pixelSize: perf.fs(38, 26)
						font.bold: true
					}
					Label {
						anchors.horizontalCenter: parent.horizontalCenter
						text: perf.num(sSrc) >= 1.5 ? "KN · SOG" : "KN · STW"
						color: Theme.color_font_secondary
						font.pixelSize: 10
						font.bold: true
					}
				}
			}

			// Motoring map: best-ever boat speed against RPM at the present headwind and sea state, live dot
			Item {
				id: motor
				anchors.fill: parent
				visible: perf.view === 1
				readonly property real padL: 38
				readonly property real padB: 16
				readonly property real cw: width - padL - 4
				readonly property real ch: height - padB - 4
				// at most six RPM labels, and at least 30 px apart (four-digit labels touch on the 5-inch plot at 1000)
				readonly property real rStep: [500, 1000, 2000, 5000, 10000].find(s => s >= perf.maxR / 6
						&& s / perf.maxR * cw >= 30) || 10000
				readonly property real sStep: Math.max(1, Math.ceil(perf.maxS / 5))
				function mx(r) { return padL + r / perf.maxR * cw }
				function my(s) { return 4 + ch - s / perf.maxS * ch }
				// The speed readout sits top left, over the low-RPM end the curve rises from; on a small plot (5-inch) the
				// curve and the dot reach that corner, and the readout moves to bottom right, under the high-RPM end
				function clear(rx, ry, rw, rh) {
					const hit = (x, y, pad) => x > rx - pad && x < rx + rw + pad && y > ry - pad && y < ry + rh + pad
					if (perf.lValid && hit(mx(perf.num(mRpm) || 0), my(perf.lSpd || 0), 8))
						return false
					return !perf.haveCurve || !perf.curve.pts.some(q => hit(mx(q[0]), my(q[1]), 3))
				}
				readonly property bool lowRight: !clear(padL + 8, 8, spdCol.width, spdCol.height)
					&& clear(padL + cw - 8 - spdCol.width, 4 + ch - 6 - spdCol.height, spdCol.width, spdCol.height)
				Repeater {
					model: Math.floor(perf.maxR / motor.rStep) + 1
					delegate: Item {
						required property int index
						Rectangle {
							x: motor.mx(index * motor.rStep)
							y: 4
							width: 1
							height: motor.ch
							color: Theme.color_card_separator
						}
						Label {
							x: motor.mx(index * motor.rStep) - width / 2
							y: motor.height - motor.padB + 2
							text: index * motor.rStep
							color: Theme.color_font_disabled
							font.pixelSize: 10
						}
					}
				}
				Repeater {
					model: Math.floor(perf.maxS / motor.sStep) + 1
					delegate: Item {
						required property int index
						Rectangle {
							x: motor.padL
							y: motor.my(index * motor.sStep)
							width: motor.cw
							height: 1
							color: Theme.color_card_separator
						}
						Label {
							x: motor.padL - width - 3
							y: motor.my(index * motor.sStep) - height / 2
							text: index * motor.sStep + (index === Math.floor(perf.maxS / motor.sStep) ? " kn" : "")
							color: Theme.color_font_disabled
							font.pixelSize: 10
						}
					}
				}
				Shape {
					anchors.fill: parent
					preferredRendererType: Shape.CurveRenderer
					ShapePath {
						strokeColor: Theme.color_blue
						strokeWidth: 2.2
						fillColor: "transparent"
						joinStyle: ShapePath.RoundJoin
						capStyle: ShapePath.RoundCap
						PathPolyline {
							path: perf.view === 1 && perf.haveCurve ? perf.curve.pts.map(q => Qt.point(motor.mx(q[0]), motor.my(q[1]))) : []
						}
					}
				}
				// Centred in the plot area, clear of the axis labels; on a small plot (5-inch) it shrinks and breaks onto two lines
				Label {
					id: noMotor
					readonly property real maxW: motor.cw - 8
					x: motor.padL + (motor.cw - width) / 2
					y: 4 + (motor.ch - height) / 2
					width: Math.min(implicitWidth, maxW)
					visible: !perf.haveCurve
					text: noMotorTm.advanceWidth <= maxW ? noMotorTm.text : "No motoring\ndata yet"
					horizontalAlignment: Text.AlignHCenter
					color: Theme.color_font_secondary
					font.pixelSize: perf.fs(Theme.font_size_caption, 11)
					TextMetrics { id: noMotorTm; font: noMotor.font; text: "No motoring data yet" }
				}
				Rectangle {
					visible: perf.view === 1 && perf.lValid
					x: motor.mx(perf.num(mRpm) || 0) - width / 2
					y: motor.my(perf.lSpd || 0) - height / 2
					width: 13; height: 13; radius: 6.5
					color: perf.lSteady ? Theme.color_green : Theme.color_orange
					border.width: 2
					border.color: Theme.color_white
				}
				Column {
					id: spdCol
					x: motor.lowRight ? motor.padL + motor.cw - 8 - width : motor.padL + 8
					y: motor.lowRight ? 4 + motor.ch - 6 - height : 8
					XregNum {
						anchors.right: motor.lowRight ? parent.right : undefined
						template: "88.8"
						text: perf.lValid && isFinite(perf.lSpd) ? perf.lSpd.toFixed(1) : "--"
						color: perf.brand
						font.pixelSize: perf.fs(34, 24)
						font.bold: true
					}
					Label {
						anchors.right: motor.lowRight ? parent.right : undefined
						text: perf.num(mSrc) >= 1.5 ? "KN · SOG" : "KN · STW"
						color: Theme.color_font_secondary
						font.pixelSize: 10
						font.bold: true
					}
				}
			}
		}

		// Under the plot: the polar's wind frame (Apparent is the front's own frame), or the motoring x axis name
		Row {
			anchors.horizontalCenter: box.horizontalCenter
			y: box.height + 4
			spacing: 0
			visible: perf.view === 0
			Repeater {
				model: ["Apparent", "True"]
				delegate: Rectangle {
					required property int index
					required property string modelData
					readonly property bool isOn: (index === 1) === perf.trueFrame
					width: frameLabel.implicitWidth + 18
					height: perf.toggleH - 6
					radius: 4
					color: isOn ? Theme.color_ok : "transparent"
					border.width: 1
					border.color: Theme.color_ok
					Label {
						id: frameLabel
						anchors.centerIn: parent
						text: parent.modelData
						color: parent.isOn ? Theme.color_button_down_text : Theme.color_font_secondary
						font.pixelSize: 12
					}
					MouseArea {
						anchors.fill: parent
						anchors.margins: -4
						onClicked: perf.trueFrame = parent.index === 1
					}
				}
			}
		}
		Label {
			anchors.horizontalCenter: box.horizontalCenter
			y: box.height + 4
			visible: perf.view === 1
			text: "RPM"
			color: Theme.color_font_secondary
			font.pixelSize: 12
		}
	}
}
