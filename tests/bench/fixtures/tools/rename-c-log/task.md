Rename the function `logmsg` to `log_message` everywhere in this workspace: the
definition, every call site, and every declaration. Do not change any
behaviour and do not rename anything else. Verify with `out="$(mktemp -t tnybench)" || exit 1
trap 'rm -f "$out"' EXIT
cc -std=c11 -o "$out" mod.c main.c
exec "$out"`.
