/* Benchmark-only API compatibility control. Preserve every model-produced byte:
 * only the prebound JSON adapter gains CPython's ensure_ascii keyword. This is
 * not a patch to MicroPython's dict order or Python language semantics. */
#ifndef TNY_MICROPYTHON_JSON_COMPAT_H
#define TNY_MICROPYTHON_JSON_COMPAT_H
static const char tny_micropython_json_compat[] =
    "_tny_native_json = json\n"
    "class _TnyJson:\n"
    "    def loads(self, text):\n"
    "        return _tny_native_json.loads(text)\n"
    "    def dumps(self, value, ensure_ascii=True, **kwargs):\n"
    "        text = _tny_native_json.dumps(value, **kwargs)\n"
    "        if not ensure_ascii:\n"
    "            return text\n"
    "        parts = []\n"
    "        for ch in text:\n"
    "            cp = ord(ch)\n"
    "            if cp < 128:\n"
    "                parts.append(ch)\n"
    "            elif cp <= 65535:\n"
    "                parts.append('\\\\u%04x' % cp)\n"
    "            else:\n"
    "                cp -= 65536\n"
    "                parts.append('\\\\u%04x\\\\u%04x' % (55296 + (cp >> 10), 56320 + (cp & "
    "1023)))\n"
    "        return ''.join(parts)\n"
    "json = _TnyJson()\n";
#endif
