# Headless remote-console driver for the bench GX (GX=<ip> overrides the address, SCHEME=dark for dark mode): rc.py out_dir step [step ...]
# step = "wait:<s>" | "click:<x>,<y>" | "shot:<name>". Canvas coordinates are in the 1024x600 viewport.
import os, sys, time
from playwright.sync_api import sync_playwright

out = sys.argv[1]
with sync_playwright() as p:
    b = p.chromium.launch(channel='chrome', headless=True,
                          args=['--use-angle=swiftshader', '--enable-unsafe-swiftshader', '--ignore-gpu-blocklist'])
    # SCHEME=dark|light: the browser remote console follows the browser's color scheme
    pg = b.new_page(viewport={'width': 1024, 'height': 600}, color_scheme=os.environ.get('SCHEME', 'light'))
    logs = []
    pg.on('console', lambda m: logs.append(m.text))
    pg.goto('http://%s/gui-v2/' % os.environ.get('GX', '172.20.10.2'), wait_until='load')
    for step in sys.argv[2:]:
        kind, _, arg = step.partition(':')
        if kind == 'wait':
            time.sleep(float(arg))
        elif kind == 'click':
            x, y = (float(v) for v in arg.split(','))
            pg.mouse.click(x, y)
            time.sleep(1.5)
        elif kind == 'wheel':
            pg.mouse.move(512, 300)
            pg.mouse.wheel(0, float(arg))
            time.sleep(1.5)
        elif kind == 'shot':
            pg.screenshot(path='%s/%s.png' % (out, arg))
    b.close()
    for l in logs:
        if any(k in l.lower() for k in ('plugin', 'xreg', 'error')):
            print('console:', l[:1200])
