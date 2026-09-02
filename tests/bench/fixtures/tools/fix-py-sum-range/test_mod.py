from mod import sum_range


def main():
    assert sum_range(1, 5) == 15, sum_range(1, 5)
    assert sum_range(0, 0) == 0, sum_range(0, 0)
    assert sum_range(-2, 2) == 0, sum_range(-2, 2)


if __name__ == '__main__':
    main()
    print('ok')
