from tny_ext import context


def setup(api):
    immutable = False
    try:
        api.capabilities.extra["mutated"] = True
    except TypeError:
        immutable = True

    @api.on("status")
    def report(_event):
        selected = api.capabilities.selected
        state = selected.state("extensions.prompt.observe") if selected else "missing"
        return context(
            "%s|%s|%s|%s"
            % (
                api.capabilities.schema_version,
                api.capabilities.selected_provider,
                state,
                "immutable" if immutable else "mutable",
            ),
            custom_type="capability_fixture",
        )
