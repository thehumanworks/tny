from concurrent.futures import ThreadPoolExecutor, as_completed
from time import sleep


def _normalize(index, event):
    sleep((index % 11) * 0.001)
    return index, {"seq": event["seq"], "message": event["message"].strip().lower()}


def replay(events):
    with ThreadPoolExecutor(max_workers=8) as pool:
        futures = [
            pool.submit(_normalize, index, event) for index, event in enumerate(events)
        ]
        ordered = [None] * len(futures)
        for future in as_completed(futures):
            index, value = future.result()
            ordered[index] = value
        return ordered
