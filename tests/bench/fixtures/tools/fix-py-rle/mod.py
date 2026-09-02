def rle(s):
    """Run-length encode: 'aaab' -> [('a', 3), ('b', 1)]."""
    out = []
    if not s:
        return out
    cur, n = s[0], 1
    for ch in s[1:]:
        if ch == cur:
            n += 1
        else:
            out.append((cur, n))
            cur, n = ch, 1
    return out
