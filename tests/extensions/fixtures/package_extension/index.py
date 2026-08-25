from tny_ext import context

from .helper import MESSAGE


def setup(api):
    @api.on("session_start")
    def session_start(_event):
        return context(MESSAGE, custom_type="package_fixture")
