def setup(api):
    @api.on("*")
    def observe(event):
        return {"kind": "context", "content": event.type}
