-- migrate:up
-- CSIP-AUS: IEEE 2030.5-2018 with the Common Smart Inverter Profile for
-- Australia, v1.1a and v1.2.
--
-- The worker is a utility client for each csipaus connection. It finds the
-- server's resources from DeviceCapability, and is known to the server by
-- the LFDI of the connection's client certificate, which every csipaus
-- connection needs.

ALTER TABLE connections
    DROP CONSTRAINT connections_protocol_check,
    ADD CONSTRAINT connections_protocol_check
        CHECK (protocol IN ('openadr2b', 'csipaus'));

-- CSIP-AUS settings, one row per csipaus connection.
CREATE TABLE csipaus_connections (
    connection_id             bigint      PRIMARY KEY REFERENCES connections (id) ON DELETE CASCADE,
    dcap_url                  text        NOT NULL CHECK (dcap_url <> ''),
    ca_cert_pem               text,
    verify_server_certificate boolean     NOT NULL DEFAULT true,
    connection_point_id       text        CHECK (length(connection_point_id) BETWEEN 1 AND 32),
    registration_pin          integer     CHECK (registration_pin BETWEEN 0 AND 999999)
);

COMMENT ON TABLE csipaus_connections IS
    'CSIP-AUS settings, one row per csipaus connection, inserted with it. Written by the API.';
COMMENT ON COLUMN csipaus_connections.dcap_url IS
    'The utility server''s DeviceCapability URL, from its interconnection handbook. Everything else is found by following its links.';
COMMENT ON COLUMN csipaus_connections.ca_cert_pem IS
    'Trusted for the server''s certificate in place of the system CAs. Null uses the system CAs.';
COMMENT ON COLUMN csipaus_connections.connection_point_id IS
    'The site''s connection point, by default its NMI, sent on registration. Null sends none.';
COMMENT ON COLUMN csipaus_connections.registration_pin IS
    'The PIN the utility gave for the device out of band. Registration fails when the server''s differs. Null skips the check.';

CREATE TRIGGER csipaus_connections_touch_connection
    AFTER INSERT OR UPDATE OR DELETE ON csipaus_connections
    FOR EACH ROW EXECUTE FUNCTION protocol_settings_touch_connection();

-- A registration belongs to the server at dcap_url, so a registered
-- connection has to be unregistered before it moves to another.
CREATE FUNCTION csipaus_connections_keep_program() RETURNS trigger
LANGUAGE plpgsql AS $$
BEGIN
    IF NEW.dcap_url IS DISTINCT FROM OLD.dcap_url
       AND EXISTS (SELECT 1 FROM connection_state s
                   WHERE s.connection_id = OLD.connection_id
                     AND s.registration = 'registered') THEN
        RAISE EXCEPTION 'connection % is registered with the server at %; unregister it before changing dcap_url',
            OLD.connection_id, OLD.dcap_url
            USING ERRCODE = 'object_not_in_prerequisite_state';
    END IF;
    RETURN NEW;
END;
$$;

CREATE TRIGGER csipaus_connections_keep_program
    BEFORE UPDATE OF dcap_url ON csipaus_connections
    FOR EACH ROW EXECUTE FUNCTION csipaus_connections_keep_program();

-- migrate:down

DROP TABLE csipaus_connections;
DROP FUNCTION csipaus_connections_keep_program();

-- Fails while csipaus connections remain.
ALTER TABLE connections
    DROP CONSTRAINT connections_protocol_check,
    ADD CONSTRAINT connections_protocol_check
        CHECK (protocol IN ('openadr2b'));
