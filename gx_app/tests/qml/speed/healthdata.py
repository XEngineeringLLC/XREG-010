# Synthetic /Health/TrendCsv/0 (200 points) and /Health/SessionCsv (30 min, a row every 5 s), as the regulator serves them
import sys, json, random, math
random.seed(int(sys.argv[2]) if len(sys.argv) > 2 else 3)
tr = ['eng,worst,overall'] + ['%.2f,%.1f,%.1f' % (i * 1.7, 92 + 4 * math.sin(i / 9) + random.random(), 97 + 2 * math.sin(i / 7)) for i in range(200)]
now = 5000000
rows = ['#nowMs=%d' % now, 't,pct,amps,measAmps,pred,rpm,fieldPct,tempF,vbus,tSlope,state,ring']
for k in range(360):
    t = now - (360 - k) * 5000
    st = 0 if k % 7 else 1
    rows.append('%d,%.1f,%.1f,%.1f,%.1f,%d,%.0f,%.1f,%.2f,%.2f,%d,%d' % (t, 96 + 3 * math.sin(k / 11), 80 + random.random() * 5, 80.5,
                82, 1800 + k, 60, 150 + k / 20, 13.9, 0.1, st if k % 13 else 2, 1 if k % 3 == 0 else 0))
json.dump({'Health/TrendCsv/0': '\n'.join(tr), 'Health/SessionCsv': '\n'.join(rows) + '\n', 'Health/Live/valid': 1}, open(sys.argv[1], 'w'))
