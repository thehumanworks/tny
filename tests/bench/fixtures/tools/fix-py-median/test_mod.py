from mod import median


def main():
    assert median([3, 1, 2]) == 2
    assert median([4, 1, 3, 2]) == 2.5, median([4, 1, 3, 2])
    assert median([1, 1]) == 1


if __name__ == '__main__':
    main()
    print('ok')
