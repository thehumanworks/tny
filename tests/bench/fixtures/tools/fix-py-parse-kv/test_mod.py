from mod import parse_kv


def main():
    assert parse_kv('a=1') == ('a', '1')
    assert parse_kv('url=http://x/?q=1') == ('url', 'http://x/?q=1')
    assert parse_kv('empty=') == ('empty', '')


if __name__ == '__main__':
    main()
    print('ok')
