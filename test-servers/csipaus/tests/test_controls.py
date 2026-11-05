"""The program, default control and controls, and the client's responses."""

from datetime import datetime, timezone

from support import LFDI, NS, SEP, text

PROGRAM = "/d/site-1/edev/1/derp/1"


def active_power(element, name: str) -> tuple[int, int] | None:
    found = element.find(f".//c:{name}", NS)
    if found is None:
        return None
    return int(text(found, "s:value")), int(text(found, "s:multiplier"))


async def test_the_program_is_reached_through_the_assignments(sep2, registered):
    fsas = await sep2.get(f"{registered}/fsa")
    assert fsas.get("all") == "1"
    programs_href = fsas.find("s:FunctionSetAssignments/s:DERProgramListLink", NS).get("href")
    programs = await sep2.get(programs_href)
    program = programs.find("s:DERProgram", NS)
    assert program.get("href") == PROGRAM
    assert text(program, "s:primacy") == "1"
    # No default until the utility sets one.
    assert program.find("s:DefaultDERControlLink", NS) is None
    await sep2.get(f"{PROGRAM}/dderc", status=404)


async def test_default_control(sep2, api, registered):
    await api.call("PUT", "/clients/site-1/devices/1/default-control",
                   json={"base": {"opModExpLimW": 5000, "opModImpLimW": 50000,
                                  "opModConnect": True}})
    program = await sep2.get(PROGRAM)
    assert program.find("s:DefaultDERControlLink", NS).get("href") == f"{PROGRAM}/dderc"
    default = await sep2.get(f"{PROGRAM}/dderc")
    assert active_power(default, "opModExpLimW") == (5000, 0)
    # 50 kW does not fit an Int16 value, so it takes a power of ten.
    assert active_power(default, "opModImpLimW") == (5000, 1)
    assert text(default, "s:DERControlBase/s:opModConnect") == "true"
    assert text(default, "s:version") == "0"

    await api.call("PUT", "/clients/site-1/devices/1/default-control",
                   json={"base": {"opModExpLimW": 1500}})
    default = await sep2.get(f"{PROGRAM}/dderc")
    assert text(default, "s:version") == "1"
    assert active_power(default, "opModImpLimW") is None


async def test_controls_are_scheduled_active_and_cancelled(sep2, api, registered):
    now = await api.call("POST", "/clients/site-1/devices/1/controls", json={
        "start_in_seconds": 0, "duration_seconds": 600, "base": {"opModExpLimW": 0},
        "description": "Export off"})
    later = await api.call("POST", "/clients/site-1/devices/1/controls", json={
        "start_in_seconds": 3600, "duration_seconds": 600,
        "base": {"opModGenLimW": 2000}, "randomize_start_seconds": 60})
    assert (now["status"], later["status"]) == ("active", "scheduled")

    controls = await sep2.get(f"{PROGRAM}/derc")
    assert controls.get("all") == "2"
    listed = controls.findall("s:DERControl", NS)
    # Ordered by start.
    assert [text(c, "s:mRID") for c in listed] == [now["mrid"], later["mrid"]]
    first = listed[0]
    assert text(first, "s:EventStatus/s:currentStatus") == "1"
    assert text(first, "s:description") == "Export off"
    assert first.get("replyTo") == "/d/site-1/rsps/1/rsp"
    assert first.get("responseRequired") == "03"
    assert active_power(first, "opModExpLimW") == (0, 0)
    start = datetime.fromisoformat(now["start"].replace("Z", "+00:00"))
    assert int(text(first, "s:interval/s:start")) == int(start.timestamp())
    assert text(first, "s:interval/s:duration") == "600"
    assert text(listed[1], "s:EventStatus/s:currentStatus") == "0"
    assert text(listed[1], "s:randomizeStart") == "60"

    active = await sep2.get(f"{PROGRAM}/actderc")
    assert [text(c, "s:mRID") for c in active.findall("s:DERControl", NS)] == [now["mrid"]]

    await api.call("POST", f"/clients/site-1/devices/1/controls/{now['id']}/cancel")
    cancelled = await sep2.get(f"{PROGRAM}/derc/{now['id']}")
    assert text(cancelled, "s:EventStatus/s:currentStatus") == "2"
    # Still listed, so the client sees the cancellation, but no longer active.
    assert (await sep2.get(f"{PROGRAM}/derc")).get("all") == "2"
    assert (await sep2.get(f"{PROGRAM}/actderc")).get("all") == "0"
    await api.call("POST", f"/clients/site-1/devices/1/controls/{now['id']}/cancel",
                   status=400)


async def test_ended_controls_leave_the_list(sep2, api, registered):
    await api.call("POST", "/clients/site-1/devices/1/controls", json={
        "start": "2020-01-01T00:00:00Z", "duration_seconds": 60, "base": {"opModExpLimW": 0}})
    assert (await sep2.get(f"{PROGRAM}/derc")).get("all") == "0"
    controls = await api.call("GET", "/clients/site-1/devices/1/controls")
    assert controls[0]["status"] == "completed"


async def test_responses_are_recorded_on_the_control(sep2, api, registered):
    control = await api.call("POST", "/clients/site-1/devices/1/controls", json={
        "start_in_seconds": 0, "base": {"opModExpLimW": 0}})
    sent = int(datetime.now(timezone.utc).timestamp())
    for status in (1, 2):
        await sep2.post("rsps/1/rsp", (
            f'<DERControlResponse xmlns="{SEP}"><createdDateTime>{sent}</createdDateTime>'
            f"<endDeviceLFDI>{LFDI}</endDeviceLFDI>"
            f"<status>{status}</status><subject>{control['mrid']}</subject>"
            "</DERControlResponse>"))
    seen = await api.call("GET", f"/clients/site-1/devices/1/controls/{control['id']}")
    assert [r["status_name"] for r in seen["responses"]] == ["event_received",
                                                              "event_started"]

    # A response to no control is refused.
    await sep2.post("rsps/1/rsp", (
        f'<DERControlResponse xmlns="{SEP}"><endDeviceLFDI>{"0" * 40}</endDeviceLFDI>'
        f"<status>1</status><subject>{'0' * 32}</subject></DERControlResponse>"), status=400)


async def test_control_requests_are_checked(api, registered):
    path = "/clients/site-1/devices/1/controls"
    for body, problem in [
        ({"base": {}}, "at least one"),
        ({"base": {"opModMaxLimW": 50}}, "cannot set opModMaxLimW"),
        ({"base": {"opModExpLimW": -1}}, "zero or more"),
        ({"base": {"opModConnect": 1}}, "true or false"),
        ({"base": {"opModExpLimW": 0}, "duration_seconds": 0}, "positive"),
        ({"base": {"opModExpLimW": 0}, "randomize_start_seconds": 7200}, "within an hour"),
        ({"base": {"opModExpLimW": 0}, "start": "2026-01-01T00:00:00"}, "time zone"),
    ]:
        error = await api.call("POST", path, status=400, json=body)
        assert problem in error["error"], (body, error)
