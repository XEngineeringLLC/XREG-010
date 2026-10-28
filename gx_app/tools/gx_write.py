# One GX setting write over MQTT: W/<portal>/<service>/<path> {"value": v}, then read it back.
import sys, time, json
import paho.mqtt.client as mqtt
G, portal, path, val = sys.argv[1], sys.argv[2], sys.argv[3], json.loads(sys.argv[4])
got = {}
def on_connect(c, u, f, rc, props=None):
    c.subscribe('N/%s/%s' % (portal, path))
    c.publish('W/%s/%s' % (portal, path), json.dumps({'value': val}))
    c.publish('R/%s/%s' % (portal, path), '')
def on_message(c, u, m): got['v'] = m.payload.decode()
c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
c.on_connect = on_connect; c.on_message = on_message
c.connect(G, 1883, 10); c.loop_start(); time.sleep(4); c.loop_stop()
print(path, '->', got.get('v'))
