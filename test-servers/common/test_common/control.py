"""What every test server's control API shares.

Each server plays the utility's part through JSON over HTTP under /api. When
a token is configured every /api request needs `Authorization: Bearer
<token>`. Handlers raise `InvalidRequest` for a 400 and `KeyError` for a 404.
"""

from __future__ import annotations

import hmac
import json
from dataclasses import asdict, is_dataclass
from datetime import date, datetime, timedelta, timezone
from typing import Any, Callable

from aiohttp import web


class InvalidRequest(ValueError):
    """A control request the server cannot carry out. Maps to HTTP 400."""


def to_json(value: Any) -> Any:
    if isinstance(value, datetime):
        return value.astimezone(timezone.utc).isoformat().replace("+00:00", "Z")
    if isinstance(value, date):
        return value.isoformat()
    if isinstance(value, timedelta):
        return value.total_seconds()
    if is_dataclass(value):
        return asdict(value)
    if isinstance(value, (bytes, bytearray)):
        return value.decode(errors="replace")
    if isinstance(value, (set, frozenset)):
        return list(value)
    return str(value)


def json_response(data: Any, status: int = 200) -> web.Response:
    return web.json_response(data, status=status,
                             dumps=lambda d: json.dumps(d, default=to_json))


def error(status: int, message: str) -> web.Response:
    return json_response({"error": message}, status=status)


def typed_field(body: dict[str, Any], name: str, kind: type | tuple[type, ...],
                default: Any = None) -> Any:
    """`body[name]`, or `default` when it is missing, checked to be a `kind`."""
    value = body.get(name, default)
    if value is None:
        return None
    kinds = kind if isinstance(kind, tuple) else (kind,)
    # A bool is an int in Python; accept one only where asked for.
    if isinstance(value, bool) and bool not in kinds or not isinstance(value, kinds):
        raise InvalidRequest(f"{name} must be {' or '.join(k.__name__ for k in kinds)}")
    return value


async def read_body(request: web.Request) -> dict[str, Any]:
    """The request's JSON object, or {} when it has no body."""
    if not request.can_read_body:
        return {}
    try:
        body = await request.json()
    except json.JSONDecodeError:
        raise InvalidRequest("the body is not JSON") from None
    if not isinstance(body, dict):
        raise InvalidRequest("the body must be a JSON object")
    return body


def control_middleware(token: str | None) -> Callable:
    """Checks the bearer token on /api requests, and turns `InvalidRequest`
    and `KeyError` into JSON errors."""

    @web.middleware
    async def middleware(request: web.Request,
                         handler: Callable[[web.Request], Any]) -> web.StreamResponse:
        if request.path.startswith("/api/"):
            if token:
                supplied = request.headers.get("Authorization", "")
                if not hmac.compare_digest(supplied.encode(), f"Bearer {token}".encode()):
                    return error(401, "a valid bearer token is required")
            try:
                return await handler(request)
            except InvalidRequest as problem:
                return error(400, str(problem))
            except KeyError as missing:
                return error(404, f"not found: {missing.args[0]}")
        return await handler(request)

    return middleware
