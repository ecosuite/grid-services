"""A stand-in for SolarNetwork's datum query API, which the worker reads its
telemetry from whatever the protocol.

A test server mounts it beside its own endpoints. The suite puts datums in
through the server's control API, and the worker reads them back through the
two SolarQuery endpoints it uses, signed with SNWS2. Every request is checked
against the token secrets the suite registered, and recorded with the
outcome, so the suite can see that the worker signs correctly and asks for
the right windows.
"""

from __future__ import annotations

import hashlib
import hmac
import logging
from collections import deque
from dataclasses import dataclass, field
from datetime import datetime, timedelta, timezone
from email.utils import parsedate_to_datetime
from typing import Any
from urllib.parse import quote

from aiohttp import web

from test_common.control import InvalidRequest, json_response, read_body, typed_field

log = logging.getLogger("test_common.solarnetwork")

LIST_PATH = "/solarquery/api/v1/sec/datum/list"
MOST_RECENT_PATH = "/solarquery/api/v1/sec/datum/mostRecent"
REQUEST_LOG_SIZE = 500
EMPTY_SHA256 = hashlib.sha256(b"").hexdigest()


def parse_time(text: str) -> datetime:
    """SolarNetwork's minute-precision query dates, or ISO 8601."""
    text = text.replace("Z", "+00:00")
    parsed = datetime.fromisoformat(text)
    if parsed.tzinfo is None:
        parsed = parsed.replace(tzinfo=timezone.utc)
    return parsed.astimezone(timezone.utc)


def solarnetwork_time(at: datetime) -> str:
    """How SolarNetwork writes datum times: 2026-09-30 13:00:00.000Z."""
    return at.astimezone(timezone.utc).strftime("%Y-%m-%d %H:%M:%S.000Z")


def percent_encode(value: str) -> str:
    return quote(value, safe="-_.~")


def canonical_query(items: list[tuple[str, str]]) -> str:
    return "&".join(f"{percent_encode(k)}={percent_encode(v)}"
                    for k, v in sorted(items, key=lambda kv: kv[0]))


def signature(secret: str, method: str, path: str, query: list[tuple[str, str]],
              host: str, date: datetime) -> str:
    date_header = date.strftime("%a, %d %b %Y %H:%M:%S GMT")
    canonical = "\n".join([
        method, path, canonical_query(query), f"host:{host}",
        f"x-sn-date:{date_header}", "host;x-sn-date", EMPTY_SHA256,
    ])
    day_key = hmac.new(("SNWS2" + secret).encode(), date.strftime("%Y%m%d").encode(),
                       hashlib.sha256).digest()
    signing_key = hmac.new(day_key, b"snws2_request", hashlib.sha256).digest()
    to_sign = "\n".join([
        "SNWS2-HMAC-SHA256", date.strftime("%Y%m%dT%H%M%SZ"),
        hashlib.sha256(canonical.encode()).hexdigest(),
    ])
    return hmac.new(signing_key, to_sign.encode(), hashlib.sha256).hexdigest()


@dataclass
class FakeSolarNetwork:
    # Token -> secret.
    tokens: dict[str, str] = field(default_factory=dict)
    # (node id, source id) -> datums, each {"created": datetime, <property>: number}.
    streams: dict[tuple[int, str], list[dict[str, Any]]] = field(default_factory=dict)
    requests: deque[dict[str, Any]] = field(
        default_factory=lambda: deque(maxlen=REQUEST_LOG_SIZE))

    def routes(self) -> list[web.RouteDef]:
        """The SolarQuery endpoints the worker reads."""
        return [
            web.get(LIST_PATH, self.list_datums),
            web.get(MOST_RECENT_PATH, self.most_recent),
        ]

    def control_routes(self) -> list[web.RouteDef]:
        """The suite's part, under the server's control API."""
        return [
            web.put("/api/solarnetwork/tokens/{token}", self.put_token),
            web.put("/api/solarnetwork/streams", self.put_stream),
            web.get("/api/solarnetwork/requests", self.list_requests),
        ]

    # -- Suite side ---------------------------------------------------------

    def set_token(self, token: str, secret: str) -> None:
        self.tokens[token] = secret

    def set_stream(self, node_id: int, source_id: str, datums: list[dict[str, Any]]) -> None:
        stored = []
        for datum in datums:
            created = datum["created"]
            values = {k: v for k, v in datum.items() if k != "created"}
            stored.append({"created": created if isinstance(created, datetime)
                           else parse_time(created), **values})
        stored.sort(key=lambda d: d["created"])
        self.streams[(node_id, source_id)] = stored

    def clear_stream(self, node_id: int, source_id: str) -> None:
        self.streams.pop((node_id, source_id), None)

    async def put_token(self, request: web.Request) -> web.Response:
        """{"secret"} registers a token the fake accepts."""
        body = await read_body(request)
        secret = typed_field(body, "secret", str)
        if not secret:
            raise InvalidRequest("secret is required")
        self.set_token(request.match_info["token"], secret)
        return web.Response(status=204)

    async def put_stream(self, request: web.Request) -> web.Response:
        """{"node_id", "source_id", "datums": [{"created", <property>: number}]}
        replaces the stream's datums."""
        body = await read_body(request)
        node_id = typed_field(body, "node_id", int)
        source_id = typed_field(body, "source_id", str)
        if node_id is None or not source_id:
            raise InvalidRequest("node_id and source_id are required")
        datums = typed_field(body, "datums", list) or []
        self.set_stream(node_id, source_id, datums)
        return json_response({"node_id": node_id, "source_id": source_id,
                              "datums": len(datums)})

    async def list_requests(self, request: web.Request) -> web.Response:
        return json_response(list(self.requests))

    # -- Worker side --------------------------------------------------------

    def authorize(self, request: web.Request) -> str | None:
        """Why the request is not properly signed, or None when it is."""
        header = request.headers.get("Authorization", "")
        if not header.startswith("SNWS2 "):
            return "no SNWS2 authorization"
        parts = dict(part.split("=", 1) for part in header[6:].split(",") if "=" in part)
        token = parts.get("Credential")
        secret = self.tokens.get(token or "")
        if secret is None:
            return f"unknown token {token!r}"
        if parts.get("SignedHeaders") != "host;x-sn-date":
            return f"unexpected signed headers {parts.get('SignedHeaders')!r}"
        try:
            date = parsedate_to_datetime(request.headers["X-SN-Date"])
        except (KeyError, ValueError, TypeError):
            return "missing or malformed X-SN-Date"
        if abs(datetime.now(timezone.utc) - date) > timedelta(minutes=5):
            return "X-SN-Date is not current"
        expected = signature(secret, request.method, request.path,
                             [(k, v) for k, v in request.query.items()],
                             request.headers.get("Host", ""), date)
        if not hmac.compare_digest(expected, parts.get("Signature", "")):
            return "signature mismatch"
        return None

    def stream(self, request: web.Request) -> tuple[int, str]:
        try:
            return int(request.query["nodeId"]), request.query["sourceId"]
        except (KeyError, ValueError):
            raise web.HTTPBadRequest(text="nodeId and sourceId are required")

    def record(self, request: web.Request, failure: str | None, returned: int) -> None:
        self.requests.append({
            "at": datetime.now(timezone.utc),
            "path": request.path,
            "query": dict(request.query),
            "authorized": failure is None,
            "failure": failure,
            "returned": returned,
        })

    def to_result(self, node_id: int, source_id: str, datum: dict[str, Any]) -> dict[str, Any]:
        created = datum["created"]
        return {
            "created": solarnetwork_time(created),
            "nodeId": node_id,
            "sourceId": source_id,
            "localDate": created.strftime("%Y-%m-%d"),
            "localTime": created.strftime("%H:%M"),
            **{k: v for k, v in datum.items() if k != "created"},
        }

    async def list_datums(self, request: web.Request) -> web.Response:
        failure = self.authorize(request)
        if failure is not None:
            self.record(request, failure, 0)
            log.warning("refused %s: %s", request.path_qs, failure)
            return web.json_response({"success": False, "message": failure}, status=401)
        node_id, source_id = self.stream(request)
        datums = self.streams.get((node_id, source_id), [])
        start = request.query.get("startDate")
        end = request.query.get("endDate")
        if start:
            datums = [d for d in datums if d["created"] >= parse_time(start)]
        if end:
            datums = [d for d in datums if d["created"] < parse_time(end)]
        offset = int(request.query.get("offset", 0))
        maximum = int(request.query.get("max", 1000))
        page = datums[offset:offset + maximum]
        self.record(request, None, len(page))
        return web.json_response({
            "success": True,
            "data": {
                "totalResults": len(datums),
                "startingOffset": offset,
                "returnedResultCount": len(page),
                "results": [self.to_result(node_id, source_id, d) for d in page],
            },
        })

    async def most_recent(self, request: web.Request) -> web.Response:
        failure = self.authorize(request)
        if failure is not None:
            self.record(request, failure, 0)
            return web.json_response({"success": False, "message": failure}, status=401)
        node_id, source_id = self.stream(request)
        datums = self.streams.get((node_id, source_id), [])
        results = [self.to_result(node_id, source_id, datums[-1])] if datums else []
        self.record(request, None, len(results))
        return web.json_response({
            "success": True,
            "data": {"totalResults": len(results), "startingOffset": 0,
                     "returnedResultCount": len(results), "results": results},
        })
