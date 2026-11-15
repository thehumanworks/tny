def _impl(ctx):
    output = ctx.actions.declare_output("tny")
    ctx.actions.run(
        cmd_args(["/bin/sh", ctx.attrs.script, ctx.attrs.source, output.as_output(), ctx.attrs.cc, ctx.attrs.cxx]),
        env = {"PATH": ctx.attrs.path},
        category = "make_tny",
        local_only = True,
    )
    return [DefaultInfo(default_output = output), RunInfo(args = cmd_args(output))]

make_binary = rule(impl = _impl, attrs = {
    "source": attrs.source(), "script": attrs.source(),
    "path": attrs.string(), "cc": attrs.string(), "cxx": attrs.string(),
})
