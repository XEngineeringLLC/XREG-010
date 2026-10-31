import QtQuick
import Victron.VenusOS

Item {
	id: root
	width: 1024
	height: 544
	property Item page: loader.item
	Loader { id: loader; anchors.fill: parent }
	function load(src, props) { loader.setSource(src, props) }
	// Short Term still reading a history in slices (job) or a chart still building its streamed lines (sBuilding)
	function busy() {
		if (!page)
			return false
		if (page.job !== undefined && page.job !== null)
			return true
		const walk = (it) => {
			for (const c of it.children)
				if (c.sBuilding === true || walk(c))
					return true
			return false
		}
		return walk(page)
	}
	// every Shape's drawn input, in tree order: visible, status, then per ShapePath stroke/fill and its points
	function pathInfo() {
		let out = []
		const walk = (it) => {
			for (const c of it.children) {
				if (c.rendererType !== undefined && c.asynchronous !== undefined) {
					let sps = []
					for (let i = 0; i < c.data.length; ++i) {
						const sp = c.data[i]
						if (sp.pathElements === undefined) continue
						let els = []
						for (let j = 0; j < sp.pathElements.length; ++j) {
							const el = sp.pathElements[j]
							const lists = el.paths !== undefined ? el.paths : el.path !== undefined ? [el.path] : null
							if (!lists) { els.push({ kind: "other" }); continue }
							let pts = 0, sx = 0, sy = 0
							for (const l of lists) for (const p of l) { pts++; sx += p.x; sy += p.y }
							els.push({ subpaths: lists.length, points: pts, sumX: Math.round(sx * 1000) / 1000, sumY: Math.round(sy * 1000) / 1000 })
						}
						sps.push({ stroke: String(sp.strokeColor), width: sp.strokeWidth, fill: String(sp.fillColor), dash: sp.strokeStyle, els: els })
					}
					out.push({ visible: c.visible, status: c.status, renderer: c.rendererType, async: c.asynchronous,
						x: c.x, y: c.y, w: c.width, h: c.height, paths: sps })
				}
				walk(c)
			}
		}
		walk(root)
		return JSON.stringify(out)
	}
}
