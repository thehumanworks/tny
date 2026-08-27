"""Run one synchronous turn. Configure endpoint/key for your provider."""
import os

from tny import Runtime, RuntimeConfig, TextDeltaEvent

config = RuntimeConfig(
    workspace=os.getcwd(),
    state_dir=os.path.join(os.getcwd(), ".tny-sdk-state"),
    base_url=os.environ.get("OPENAI_BASE_URL", "https://api.openai.com/v1"),
    api_key=os.environ["OPENAI_API_KEY"],
)
with Runtime(config) as runtime, runtime.create_session() as session:
    for event in session.run("Give me a one-line repository summary"):
        if isinstance(event, TextDeltaEvent):
            print(event.text.decode("utf-8", "strict"), end="", flush=True)
