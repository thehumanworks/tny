from mod import chunk


def main():
    assert chunk([1, 2, 3, 4], 2) == [[1, 2], [3, 4]], chunk([1, 2, 3, 4], 2)
    assert chunk([1, 2, 3], 2) == [[1, 2], [3]], chunk([1, 2, 3], 2)
    assert chunk([], 3) == []


if __name__ == '__main__':
    main()
    print('ok')
