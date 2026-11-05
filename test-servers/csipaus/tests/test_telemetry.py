"""What the client writes: its DER's capability, settings, status and
availability, and its readings through mirror usage points."""

from support import CSIPAUS, LFDI, NS, SEP, text

DER = "/d/site-1/edev/1/der/1"

CAPABILITY = (
    f'<DERCapability xmlns="{SEP}" xmlns:csipaus="{CSIPAUS}">'
    "<modesSupported>00500088</modesSupported>"
    "<rtgMaxW><multiplier>0</multiplier><value>5000</value></rtgMaxW>"
    "<type>4</type><csipaus:doeModesSupported>03</csipaus:doeModesSupported>"
    "</DERCapability>")
SETTINGS = (
    f'<DERSettings xmlns="{SEP}" xmlns:csipaus="{CSIPAUS}">'
    "<setGradW>27</setGradW>"
    "<setMaxW><multiplier>0</multiplier><value>5000</value></setMaxW>"
    "<updatedTime>1700000000</updatedTime>"
    "<csipaus:doeModesEnabled>03</csipaus:doeModesEnabled></DERSettings>")
STATUS = (
    f'<DERStatus xmlns="{SEP}"><genConnectStatus><dateTime>1700000000</dateTime>'
    "<value>07</value></genConnectStatus><readingTime>1700000000</readingTime></DERStatus>")


async def test_the_der_is_linked_from_the_end_device(sep2, registered):
    ders = await sep2.get(f"{registered}/der")
    der = ders.find("s:DER", NS)
    assert der.get("href") == DER
    assert der.find("s:DERCapabilityLink", NS).get("href") == f"{DER}/dercap"
    assert der.find("s:CurrentDERProgramLink", NS).get("href") == "/d/site-1/edev/1/derp/1"


async def test_der_resources_are_kept_as_written(sep2, api, registered):
    await sep2.get(f"{DER}/dercap", status=404)
    await sep2.put(f"{DER}/dercap", CAPABILITY)
    await sep2.put(f"{DER}/derg", SETTINGS)
    await sep2.put(f"{DER}/ders", STATUS)

    capability = await sep2.get(f"{DER}/dercap")
    assert text(capability, "c:doeModesSupported") == "03"
    der = (await api.call("GET", "/clients/site-1/devices/1"))["der"]
    assert der["dercap"]["data"]["rtgMaxW"] == {"multiplier": 0, "value": 5000}
    assert der["dercap"]["data"]["type"] == 4
    assert der["derg"]["data"]["setGradW"] == 27
    assert der["ders"]["data"]["genConnectStatus"]["value"] == "07"


async def test_der_resources_take_their_own_document(sep2, registered):
    await sep2.put(f"{DER}/derg", CAPABILITY, status=400)
    await sep2.put(f"{DER}/dercap", "<DERCapability", status=400)


MUP = (
    f'<MirrorUsagePoint xmlns="{SEP}"><mRID>0600006CC8000000000000000000ABCD</mRID>'
    "<description>Site</description><roleFlags>49</roleFlags>"
    "<serviceCategoryKind>0</serviceCategoryKind><status>1</status>"
    f"<deviceLFDI>{LFDI}</deviceLFDI>"
    "<MirrorMeterReading><mRID>0700006CC8000000000000000000ABCD</mRID>"
    "<description>Real power</description>"
    "<Reading><timePeriod><duration>300</duration><start>1700000000</start></timePeriod>"
    "<value>1500</value></Reading>"
    "<ReadingType><dataQualifier>2</dataQualifier><flowDirection>1</flowDirection>"
    "<intervalLength>300</intervalLength><kind>37</kind><phase>0</phase>"
    "<powerOfTenMultiplier>0</powerOfTenMultiplier><uom>38</uom></ReadingType>"
    "</MirrorMeterReading></MirrorUsagePoint>")
READING = (
    f'<MirrorMeterReading xmlns="{SEP}"><mRID>0700006CC8000000000000000000ABCD</mRID>'
    "<Reading><timePeriod><duration>300</duration><start>1700000300</start></timePeriod>"
    "<value>1700</value></Reading></MirrorMeterReading>")


async def test_mirror_usage_points_take_readings(sep2, api, registered):
    location = await sep2.post("mup", MUP)
    assert location == "/d/site-1/mup/1"
    # Posting it again, with the same mRID, finds the same one.
    assert await sep2.post("mup", MUP) == location
    await sep2.post(location, READING)

    mups = await sep2.get("mup")
    assert mups.get("all") == "1"
    assert text(mups, "s:MirrorUsagePoint/s:deviceLFDI") == LFDI

    mup = await api.call("GET", "/clients/site-1/mirror-usage-points/1")
    [reading] = mup["meter_readings"]
    assert reading["description"] == "Real power"
    assert reading["reading_type"]["uom"] == 38
    assert reading["latest"]["value"] == 1700
    assert [p["value"] for p in mup["posts"]] == [1500, 1500, 1700]


async def test_the_server_sets_the_post_rate(sep2, api, registered):
    location = await sep2.post("mup", MUP)
    mup = await sep2.get(location)
    assert text(mup, "s:postRate") == "60"

    changed = await api.call("PATCH", "/clients/site-1/mirror-usage-points/1",
                             json={"post_rate_seconds": 300})
    assert changed["post_rate"] == 300
    assert text(await sep2.get("mup"), "s:MirrorUsagePoint/s:postRate") == "300"
    await api.call("PATCH", "/clients/site-1/mirror-usage-points/1",
                   status=400, json={"post_rate_seconds": 0})


async def test_readings_post_a_mirror_meter_reading(sep2, registered):
    location = await sep2.post("mup", MUP)
    await sep2.post(location, CAPABILITY, status=400)
    await sep2.post("mup/9", READING, status=404)
