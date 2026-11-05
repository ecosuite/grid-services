"""Rendering outgoing OpenADR messages.

This is OpenLEADR's create_message without its preflight step, which
rewrites what we send: it sets the currentValue of every SIMPLE signal to 0
because it compares the event status with "ACTIVE" in the wrong case. Every
message is checked against the 2.0b schema instead, so a VTN bug fails here
rather than in the VEN.
"""

from typing import Any

from openleadr import utils
from openleadr.messaging import TEMPLATES, validate_xml_schema


def render(message_type: str, **fields: Any) -> str:
    signed_object = utils.flatten_xml(
        TEMPLATES.get_template(f"{message_type}.xml").render(**fields))
    message = TEMPLATES.get_template("oadrPayload.xml").render(
        template=message_type, signature=None, signed_object=signed_object)
    validate_xml_schema(message)
    return message
