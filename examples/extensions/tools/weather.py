"""A one-tool extension: the module name is the tool name (`weather`).

tny weather Lisbon --days 3
tools.weather(city="Lisbon", days=3)      # in code mode
"""

from __future__ import annotations

from typing import Annotated

import tny


@tny.tool
def forecast(
    city: str,
    days: Annotated[int, tny.Param(description="How many days, 1 to 7.")] = 1,
) -> str:
    """Forecast for a city.

    Args:
        city: City name.
    """
    if not 1 <= days <= 7:
        raise tny.ToolError("days must be between 1 and 7")
    return f"{city}: sunny for {days} day(s)"
