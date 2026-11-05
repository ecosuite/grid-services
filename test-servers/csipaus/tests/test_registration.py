"""Discovery and registration: DeviceCapability, EndDevice, Registration and
the connection point."""

from support import CSIPAUS, LFDI, NS, SEP, SFDI, Sep2Client, end_device, text


async def test_device_capability_links_under_the_client_path(sep2):
    dcap = await sep2.get("dcap")
    assert dcap.get("href") == "/d/site-1/dcap"
    assert dcap.get("pollRate") == "30"
    assert dcap.find("s:TimeLink", NS).get("href") == "/d/site-1/tm"
    edev = dcap.find("s:EndDeviceListLink", NS)
    assert (edev.get("href"), edev.get("all")) == ("/d/site-1/edev", "0")
    assert dcap.find("s:MirrorUsagePointListLink", NS).get("href") == "/d/site-1/mup"


async def test_time_reports_the_zone(sep2):
    tm = await sep2.get("tm")
    # Australia/Sydney: AEST, with an hour of daylight saving.
    assert text(tm, "s:tzOffset") == "36000"
    assert text(tm, "s:dstOffset") == "3600"
    assert int(text(tm, "s:dstStartTime")) > 0


async def test_registering_an_end_device(sep2, api):
    location = await sep2.post("edev", end_device())
    assert location == "/d/site-1/edev/1"

    devices = await sep2.get("edev")
    assert (devices.get("all"), devices.get("results")) == ("1", "1")
    device = await sep2.get(location)
    assert text(device, "s:lFDI") == LFDI
    assert text(device, "s:sFDI") == str(SFDI)
    links = {child.tag.split("}")[1]: child.get("href") for child in device
             if child.get("href")}
    assert links == {
        "DERListLink": "/d/site-1/edev/1/der",
        "FunctionSetAssignmentsListLink": "/d/site-1/edev/1/fsa",
        "RegistrationLink": "/d/site-1/edev/1/rg",
        "ConnectionPointLink": "/d/site-1/edev/1/cp",
    }
    assert device.find("c:ConnectionPointLink", NS) is not None

    registration = await sep2.get(f"{location}/rg")
    assert text(registration, "s:pIN") == "111115"

    seen = await api.call("GET", "/clients/site-1")
    assert [d["lfdi"] for d in seen["end_devices"]] == [LFDI]


async def test_registering_again_keeps_the_end_device(sep2):
    first = await sep2.post("edev", end_device())
    again = await sep2.post("edev", end_device(changed_time=1700000100))
    assert first == again
    assert (await sep2.get("edev")).get("all") == "1"


async def test_an_aggregator_registers_several_end_devices(sep2):
    first = await sep2.post("edev", end_device())
    second = await sep2.post("edev", end_device(lfdi="AA" * 20, sfdi=1234))
    assert first != second
    assert (await sep2.get("dcap")).find("s:EndDeviceListLink", NS).get("all") == "2"
    # Paging: one at a time.
    page = await sep2.get("/d/site-1/edev?s=1&l=1")
    assert (page.get("all"), page.get("results")) == ("2", "1")
    assert text(page, "s:EndDevice/s:sFDI") == "1234"


async def test_clients_are_kept_apart(sep2, http):
    await sep2.post("edev", end_device())
    other = Sep2Client(http, "site-2")
    assert (await other.get("edev")).get("all") == "0"
    await other.get("edev/1", status=404)


async def test_the_pin_can_be_changed(sep2, api, registered):
    await api.call("PATCH", f"/clients/site-1/devices/{LFDI}", json={"pin": 123455})
    assert text(await sep2.get(f"{registered}/rg"), "s:pIN") == "123455"


async def test_connection_point(sep2, api, registered):
    await sep2.get(f"{registered}/cp", status=404)
    await sep2.put(f"{registered}/cp",
                   f'<ConnectionPoint xmlns="{CSIPAUS}" xmlns:s="{SEP}">'
                   "<connectionPointId>4102345678</connectionPointId></ConnectionPoint>")
    point = await sep2.get(f"{registered}/cp")
    assert text(point, "c:connectionPointId") == "4102345678"
    device = await api.call("GET", "/clients/site-1/devices/1")
    assert device["connection_point_id"] == "4102345678"


async def test_forgetting_an_end_device(sep2, api, registered):
    await api.call("DELETE", f"/clients/site-1/devices/{SFDI}", status=204)
    await sep2.get(registered, status=404)
    assert (await sep2.get("edev")).get("all") == "0"
