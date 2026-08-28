"""Thread-safe cancellation request and persisted-session resume."""

import os
import threading

from tny import CancellationToken, Runtime, RuntimeConfig, TurnEndEvent

config = RuntimeConfig(
    workspace=os.getcwd(),
    state_dir=os.path.join(os.getcwd(), ".tny-sdk-state"),
    base_url=os.environ["OPENAI_BASE_URL"],
    api_key=os.environ["OPENAI_API_KEY"],
    persistence=True,
)
token = CancellationToken()
timer = threading.Timer(2.0, token.cancel)
timer.start()
with Runtime(config) as runtime:
    with runtime.create_session() as session:
        session_id = session.id
        for event in session.run("Perform a long analysis", cancellation=token):
            if isinstance(event, TurnEndEvent):
                print("stop reason:", event.stop_reason)
    with runtime.open_session(session_id) as resumed:
        for _event in resumed.run("Continue briefly"):
            pass
timer.cancel()
