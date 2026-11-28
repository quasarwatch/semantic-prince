# Shared model loader with binary cache.
# Parses pw_uni.tsv / pw_bi.tsv once per (K, minlen, maxlen), stores arrays in
# data/.cache_vocab_*.npz; later loads are ~0.3s instead of ~5-15s of TSV parsing.
# Cache is invalidated by source file size/mtime signature.
import os, json
import numpy as np

_DATA = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "data")


def _sig(paths):
    return [[os.path.basename(p), os.path.getsize(p), int(os.path.getmtime(p))]
            for p in paths]


def _parse_count(c, path, n):
    """Count field of a model TSV: must be a positive integer."""
    try:
        ci = int(c)
    except ValueError:
        raise ValueError("%s:%d: bad count %r" % (path, n, c))
    if ci <= 0:
        raise ValueError("%s:%d: count must be >= 1, got %d" % (path, n, ci))
    return ci


class VocabModel:
    __slots__ = ("words", "wb", "widx", "counts", "logu", "bi_i", "bi_j",
                 "bi_cnt", "K")

    def build_csr(self):
        """CSR adjacency (counts) over vocab: indptr/indices/data."""
        K = self.K
        indptr = np.zeros(K + 1, dtype=np.int64)
        np.add.at(indptr, self.bi_i + 1, 1)
        np.cumsum(indptr, out=indptr)
        order = np.argsort(self.bi_i, kind="stable")
        indices = self.bi_j[order].astype(np.int32)
        data = self.bi_cnt[order].astype(np.float64)
        return indptr, indices, data


def load_vocab_model(uni_path, bi_path, K, minlen=2, maxlen=10):
    """Returns VocabModel: words (str list), wb (bytes list), counts (int64),
    logu (float64), bi_i/bi_j (int32), bi_cnt (int64) filtered to vocab."""
    cache = os.path.join(
        _DATA, ".cache_vocab_K%d_%d_%d.npz" % (K, minlen, maxlen))
    sig = json.dumps(_sig([uni_path, bi_path]))
    if os.path.exists(cache):
        try:
            z = np.load(cache)
            if str(z["sig"][0]) == sig:
                m = VocabModel()
                blob = z["words_blob"].tobytes()
                m.wb = blob.split(b"\n") if blob else []
                m.words = [b.decode() for b in m.wb]
                m.widx = {w: i for i, w in enumerate(m.words)}
                m.counts = z["counts"]
                m.bi_i, m.bi_j, m.bi_cnt = z["bi_i"], z["bi_j"], z["bi_cnt"]
                m.K = len(m.words)
                if m.K != len(m.counts):
                    raise ValueError("corrupt cache: %d words vs %d counts"
                                     % (m.K, len(m.counts)))
                c = m.counts.astype(np.float64)
                m.logu = np.log(c / c.sum())
                return m
        except Exception:
            pass  # corrupt cache -> rebuild

    words, counts = [], []
    with open(uni_path) as f:
        for n, line in enumerate(f, 1):
            w, c = line.rstrip("\n").split("\t")
            if minlen <= len(w) <= maxlen:
                ci = _parse_count(c, uni_path, n)
                words.append(w)
                counts.append(ci)
            if len(words) == K:
                break
    widx = {w: i for i, w in enumerate(words)}
    K = len(words)

    bi_i, bi_j, bi_cnt = [], [], []
    with open(bi_path) as f:
        for n, line in enumerate(f, 1):
            pair, cnt = line.rstrip("\n").rsplit("\t", 1)
            ab = pair.split(" ")
            if len(ab) != 2:
                continue
            i = widx.get(ab[0])
            j = widx.get(ab[1])
            if i is not None and j is not None:
                bi_i.append(i)
                bi_j.append(j)
                bi_cnt.append(_parse_count(cnt, bi_path, n))

    m = VocabModel()
    m.words, m.widx, m.K = words, widx, K
    m.wb = [w.encode() for w in words]
    m.counts = np.array(counts, dtype=np.int64)
    m.bi_i = np.array(bi_i, dtype=np.int32)
    m.bi_j = np.array(bi_j, dtype=np.int32)
    m.bi_cnt = np.array(bi_cnt, dtype=np.int64)
    c = m.counts.astype(np.float64)
    m.logu = np.log(c / c.sum())

    blob = b"\n".join(m.wb)
    try:
      np.savez(cache,
             sig=np.array([sig]),
             words_blob=np.frombuffer(blob, dtype=np.uint8),
             counts=m.counts, bi_i=m.bi_i, bi_j=m.bi_j, bi_cnt=m.bi_cnt)
    except OSError:
      pass  # cache dir read-only (fixtures/tests): work without it
    return m
