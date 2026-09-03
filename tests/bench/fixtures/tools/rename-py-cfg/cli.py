from core import getcfg
from service import handle


def main():
    assert getcfg('  Ab ') == 'ab'
    assert handle(['A', 'B']) == ['a', 'b']
    print('ok')


if __name__ == '__main__':
    main()
