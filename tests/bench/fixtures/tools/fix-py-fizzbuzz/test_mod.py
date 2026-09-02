from mod import fizzbuzz


def main():
    assert fizzbuzz(3) == 'fizz'
    assert fizzbuzz(5) == 'buzz'
    assert fizzbuzz(15) == 'fizzbuzz', fizzbuzz(15)
    assert fizzbuzz(7) == '7'


if __name__ == '__main__':
    main()
    print('ok')
