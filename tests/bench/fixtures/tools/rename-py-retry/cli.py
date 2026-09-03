from core import do_retry
from service import handle


def main():
    assert do_retry('  Ab ') == 'ab'
    assert handle(['A', 'B']) == ['a', 'b']
    print('ok')


if __name__ == '__main__':
    main()
