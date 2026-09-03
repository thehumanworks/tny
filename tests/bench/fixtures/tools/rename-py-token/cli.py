from core import tok
from service import handle


def main():
    assert tok('  Ab ') == 'ab'
    assert handle(['A', 'B']) == ['a', 'b']
    print('ok')


if __name__ == '__main__':
    main()
