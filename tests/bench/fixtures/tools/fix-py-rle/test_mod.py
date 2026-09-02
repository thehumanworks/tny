from mod import rle


def main():
    assert rle('') == []
    assert rle('a') == [('a', 1)], rle('a')
    assert rle('aaab') == [('a', 3), ('b', 1)], rle('aaab')
    assert rle('abb') == [('a', 1), ('b', 2)], rle('abb')


if __name__ == '__main__':
    main()
    print('ok')
