# Instrumented copies (created-delegate counter on every chart delegate, as agentB's make_variants.py) of two plugin dirs
#   python3 make_variants.py <before dir> <after dir>   (into $XREG_TEST_WORK/variants)
import os, shutil, sys
T = os.environ['XREG_TEST_WORK']
for name, src in (('before', sys.argv[1]), ('after', sys.argv[2])):
    d = os.path.join(T, 'variants', name)
    os.makedirs(d, exist_ok=True)
    for f in os.listdir(src):
        if f.endswith('.qml'):
            shutil.copy(os.path.join(src, f), d)
    p = os.path.join(d, 'XregChart.qml')
    c = open(p).read()
    c = c.replace('delegate: Label {\n', 'delegate: Label {\n\t\t\tComponent.onCompleted: MockStore.created++\n')
    c = c.replace('delegate: Rectangle {\n', 'delegate: Rectangle {\n\t\t\t\tComponent.onCompleted: MockStore.created++\n')
    c = c.replace('delegate: Shape {\n', 'delegate: Shape {\n\t\t\t\tComponent.onCompleted: MockStore.created++\n')
    c = c.replace('delegate: Item {\n\t\t\t\tid: entry\n',
                  'delegate: Item {\n\t\t\t\tid: entry\n\t\t\t\tComponent.onCompleted: MockStore.created++\n')
    open(p, 'w').write(c)
    print(name, 'instrumented', c.count('MockStore.created++'))
