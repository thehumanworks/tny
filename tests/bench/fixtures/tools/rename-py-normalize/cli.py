from core import norm_path
from service import handle


def main():
    assert norm_path('  Ab ') == 'ab'
    assert handle(['A', 'B']) == ['a', 'b']
    print('ok')


if __name__ == '__main__':
    main()
