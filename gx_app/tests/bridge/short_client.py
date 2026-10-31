# The Short Term page's /Fast/Short protocol in Python, the same rules as XregPlotsShort.qml take(): used to check the
# bridge's messages end to end and, sample for sample, the QML page's buffer.
NSER = 10   # 9 SHORT series + chargeStage


class ShortClient:
    def __init__(self, keep_s=300):
        self.keep_ms = keep_s * 1000
        self.epoch = None
        self.n = 0
        self.t = []
        self.cols = [[] for _ in range(NSER)]
        self.asks = 0
        self.resets = 0

    def _append(self, dts, cols, start):
        for i in range(start, len(dts)):
            self.t.append((self.t[-1] if self.t else 0) + int(dts[i]))
            for c, col in zip(self.cols, cols):
                c.append(col[i])
        while len(self.t) >= 2 and self.t[1] <= self.t[-1] - self.keep_ms:
            del self.t[0]
            for c in self.cols:
                del c[0]

    def take(self, msg):
        if not msg:
            return 'empty'
        p = msg.split(';')
        if len(p) != 6 + NSER or p[0] != 's1':
            return 'bad'
        epoch, n, k, full = p[1], int(p[2]), int(p[3]), p[4] == '1'
        first = n - k + 1
        dts = p[5].split(',') if k else []
        cols = [p[6 + j].split(',') if k else [] for j in range(NSER)]
        same = epoch == self.epoch
        if same and n <= self.n:
            return 'old'
        if same and first <= self.n < n:
            self._append(dts, cols, self.n - first + 1)
            self.n = n
            return 'tail'
        if (not full and same and first == self.n + 1) or (not full and not same and first == 1) or full:
            if not same or full:
                self.t, self.cols, self.resets = [], [[] for _ in range(NSER)], self.resets + 1
                if full and k:
                    dts = ['0'] + dts[1:]
            self.epoch = epoch
            self._append(dts, cols, 0)
            self.n = n
            return 'full' if full else 'new' if not same else 'inc'
        self.asks += 1
        return 'gap'

    def rows(self):
        # (dt from the sample before, values...) of each sample held, the first dt left out
        return [(None if i == 0 else self.t[i] - self.t[i - 1],) + tuple(c[i] for c in self.cols)
                for i in range(len(self.t))]


def recorder_rows(rec):
    t = list(rec.t)
    cols = [list(q) for q in rec.cols[1:]]
    return [(None if i == 0 else t[i] - t[i - 1],) + tuple(c[i] for c in cols) for i in range(len(t))]
