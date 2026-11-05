-- migrate:up
-- A protocol-neutral contract.
--
-- The worker is to speak protocols other than OpenADR 2.0b, such as IEEE
-- 2030.5, so what every protocol shares is kept apart from what belongs to
-- one. A table named for a protocol holds that protocol's settings and
-- records; every other table means the same whatever the protocol.
--
--   - connection_state.ven_id is client_id: the id the program knows the
--     connection by, whatever the protocol calls it.
--   - Client certificates are client_credentials rows, which connections
--     refer to, so that a credential can serve one site or many.
--   - accepted_signals names OpenADR signals, so it is an OpenADR setting.
--   - What a site measures is telemetry_points. An OpenADR report offer
--     binds points into a report by point_id.
--   - report_offers and report_requests are OpenADR's offer and request,
--     and are named for it. report_deliveries stays shared: its entries name
--     their telemetry point, and their quality is in the worker's own terms.

-- Renames a table's sequence, indexes, constraints and triggers from one
-- prefix to another, so that they keep matching the table's name.
CREATE OR REPLACE FUNCTION pg_temp.rename_prefixed(
    tbl regclass, old_prefix text, new_prefix text) RETURNS void
LANGUAGE plpgsql AS $$
DECLARE
    item record;
BEGIN
    -- Renaming an index also renames the constraint it backs.
    FOR item IN SELECT c.relname FROM pg_index i
                JOIN pg_class c ON c.oid = i.indexrelid
                WHERE i.indrelid = tbl AND c.relname LIKE old_prefix || '\_%' LOOP
        EXECUTE format('ALTER INDEX %I RENAME TO %I', item.relname,
                       new_prefix || substr(item.relname, length(old_prefix) + 1));
    END LOOP;
    FOR item IN SELECT conname FROM pg_constraint
                WHERE conrelid = tbl AND conname LIKE old_prefix || '\_%' LOOP
        EXECUTE format('ALTER TABLE %s RENAME CONSTRAINT %I TO %I', tbl, item.conname,
                       new_prefix || substr(item.conname, length(old_prefix) + 1));
    END LOOP;
    FOR item IN SELECT s.relname FROM pg_class s
                JOIN pg_depend d ON d.objid = s.oid
                WHERE s.relkind = 'S' AND d.refobjid = tbl AND d.deptype = 'a'
                  AND s.relname LIKE old_prefix || '\_%' LOOP
        EXECUTE format('ALTER SEQUENCE %I RENAME TO %I', item.relname,
                       new_prefix || substr(item.relname, length(old_prefix) + 1));
    END LOOP;
    FOR item IN SELECT tgname FROM pg_trigger
                WHERE tgrelid = tbl AND NOT tgisinternal
                  AND tgname LIKE old_prefix || '\_%' LOOP
        EXECUTE format('ALTER TRIGGER %I ON %s RENAME TO %I', item.tgname, tbl,
                       new_prefix || substr(item.tgname, length(old_prefix) + 1));
    END LOOP;
END;
$$;

-- ---------------------------------------------------------------------------
-- Identity: client_id

ALTER TABLE connection_state RENAME COLUMN ven_id TO client_id;

COMMENT ON COLUMN connection_state.client_id IS
    'The id the program knows the connection by, as confirmed on registration: the VEN id for OpenADR 2.0b.';

-- Renamed in place, so that clients keep their grants on the view.
ALTER VIEW connection_health RENAME COLUMN ven_id TO client_id;

-- ---------------------------------------------------------------------------
-- Client credentials (written by the API)
--
-- The certificate a connection authenticates to its program with, whatever
-- the protocol. A credential can serve one connection or many: a site's own
-- certificate, or an aggregator's shared by the sites it acts for.

CREATE TABLE client_credentials (
    id               bigserial   PRIMARY KEY,
    -- For operators to tell credentials apart.
    name             text        NOT NULL CHECK (name <> ''),
    cert_pem         text        NOT NULL CHECK (cert_pem <> ''),
    -- The private key, sealed with the worker's public key (libsodium
    -- crypto_box_seal), base64 encoded. Only the worker can open it.
    key_sealed       text        NOT NULL CHECK (key_sealed <> ''),
    -- Derived by the API on upload, for display and readiness checks.
    cert_fingerprint text,
    cert_expires_at  timestamptz,
    created_at       timestamptz NOT NULL DEFAULT now(),
    updated_at       timestamptz NOT NULL DEFAULT now()
);

COMMENT ON TABLE client_credentials IS
    'Client certificates connections authenticate to their programs with. One can serve several connections. Written by the API.';

ALTER TABLE connections
    ADD COLUMN client_credential_id bigint REFERENCES client_credentials (id);

COMMENT ON COLUMN connections.client_credential_id IS
    'The client certificate the connection authenticates with. Null for none. A credential in use cannot be deleted.';

CREATE INDEX connections_client_credential
    ON connections (client_credential_id) WHERE client_credential_id IS NOT NULL;

CREATE TRIGGER client_credentials_updated_at
    BEFORE UPDATE ON client_credentials
    FOR EACH ROW EXECUTE FUNCTION touch_updated_at();

-- A change to a credential is a change to every connection that uses it.
CREATE FUNCTION client_credentials_touch_connections() RETURNS trigger
LANGUAGE plpgsql AS $$
BEGIN
    UPDATE connections SET updated_at = clock_timestamp()
    WHERE client_credential_id = NEW.id;
    RETURN NULL;
END;
$$;

CREATE TRIGGER client_credentials_touch_connections
    AFTER UPDATE ON client_credentials
    FOR EACH ROW
    WHEN (to_jsonb(OLD) - 'updated_at' IS DISTINCT FROM to_jsonb(NEW) - 'updated_at')
    EXECUTE FUNCTION client_credentials_touch_connections();

-- Each connection's certificate becomes a credential of its own. A
-- certificate without its key, or a key without its certificate, never
-- worked; it has to be completed or removed before migrating.
DO $$
DECLARE
    settings record;
    credential bigint;
BEGIN
    FOR settings IN SELECT o.connection_id
                    FROM openadr2b_connections o
                    WHERE (COALESCE(o.client_cert_pem, '') <> '')
                          <> (COALESCE(o.client_key_sealed, '') <> '') LOOP
        RAISE EXCEPTION 'connection % has a client certificate without its key, or a key without its certificate; set both or neither before migrating',
            settings.connection_id;
    END LOOP;

    FOR settings IN SELECT o.connection_id, o.client_cert_pem, o.client_key_sealed,
                           o.cert_fingerprint, o.cert_expires_at, c.site_code
                    FROM openadr2b_connections o
                    JOIN connections c ON c.id = o.connection_id
                    WHERE o.client_cert_pem <> '' AND o.client_key_sealed <> ''
                    ORDER BY o.connection_id LOOP
        INSERT INTO client_credentials (name, cert_pem, key_sealed,
                                        cert_fingerprint, cert_expires_at)
        VALUES (settings.site_code || ' (connection ' || settings.connection_id || ')',
                settings.client_cert_pem, settings.client_key_sealed,
                settings.cert_fingerprint, settings.cert_expires_at)
        RETURNING id INTO credential;
        UPDATE connections SET client_credential_id = credential
        WHERE id = settings.connection_id;
    END LOOP;
END;
$$;

ALTER TABLE openadr2b_connections
    DROP COLUMN client_cert_pem,
    DROP COLUMN client_key_sealed,
    DROP COLUMN cert_fingerprint,
    DROP COLUMN cert_expires_at;

-- ---------------------------------------------------------------------------
-- Accepted signals are an OpenADR setting

ALTER TABLE openadr2b_connections
    ADD COLUMN accepted_signals text[] NOT NULL DEFAULT ARRAY['SIMPLE']
        CONSTRAINT openadr2b_connections_accepted_signals_check CHECK (
            accepted_signals <@ ARRAY['SIMPLE', 'ELECTRICITY_PRICE', 'LOAD_DISPATCH']);

COMMENT ON COLUMN openadr2b_connections.accepted_signals IS
    'Signals the site can act on. An event with any other signal is refused with 460 and not stored.';

UPDATE openadr2b_connections o
SET accepted_signals = c.accepted_signals
FROM connections c
WHERE c.id = o.connection_id AND o.accepted_signals IS DISTINCT FROM c.accepted_signals;

ALTER TABLE connections DROP COLUMN accepted_signals;

-- ---------------------------------------------------------------------------
-- Telemetry points (written by the API)
--
-- What a site measures, and where its value comes from:
--
--   kind            power_real, power_reactive, power_apparent, energy_real,
--                   energy_reactive, energy_apparent, temperature, custom.
--                   custom needs a description; temperature's units are
--                   celsius or fahrenheit
--   units, scale    the unit, and an SI prefix: none, k, M, ...
--   source          {"node_id", "source_id", "property", "property_class",
--                    "reduce", "multiplier", "stale_after_seconds"}: the
--                   SolarNetwork stream property, and how a window of datums
--                   becomes one value
--
-- Protocols bind points into what they report; see openadr2b_report_offers.

CREATE TABLE telemetry_points (
    id            bigserial   PRIMARY KEY,
    connection_id bigint      NOT NULL REFERENCES connections (id) ON DELETE CASCADE,
    -- The connection's name for the point, which reports refer to it by.
    point_id      text        NOT NULL CHECK (point_id <> ''),
    kind          text        NOT NULL DEFAULT 'power_real' CHECK (kind IN (
                      'power_real', 'power_reactive', 'power_apparent',
                      'energy_real', 'energy_reactive', 'energy_apparent',
                      'temperature', 'custom')),
    units         text        NOT NULL DEFAULT 'W' CHECK (units <> ''),
    scale         text        NOT NULL DEFAULT 'none' CHECK (scale <> ''),
    description   text,
    source        jsonb       NOT NULL CHECK (jsonb_typeof(source) = 'object'),
    created_at    timestamptz NOT NULL DEFAULT now(),
    updated_at    timestamptz NOT NULL DEFAULT now(),
    UNIQUE (connection_id, point_id),
    CONSTRAINT telemetry_points_custom_check
        CHECK (kind <> 'custom' OR COALESCE(description, '') <> ''),
    CONSTRAINT telemetry_points_temperature_check
        CHECK (kind <> 'temperature' OR units IN ('celsius', 'fahrenheit'))
);

COMMENT ON TABLE telemetry_points IS
    'What the connection''s site measures, and where each value comes from. Protocols bind points into their reports. Written by the API.';
COMMENT ON COLUMN telemetry_points.source IS
    '{"node_id", "source_id", "property", "property_class", "reduce", "multiplier", "stale_after_seconds"}. See the README.';

CREATE TRIGGER telemetry_points_touch_connection
    AFTER INSERT OR UPDATE OR DELETE ON telemetry_points
    FOR EACH ROW EXECUTE FUNCTION protocol_settings_touch_connection();

CREATE TRIGGER telemetry_points_updated_at
    BEFORE UPDATE ON telemetry_points
    FOR EACH ROW EXECUTE FUNCTION touch_updated_at();

-- ---------------------------------------------------------------------------
-- OpenADR report offers bind telemetry points
--
-- A data point is now
--
--   {"rid", "point_id", "resource_id", "report_type", "reading_type"}
--
-- where rid defaults to point_id. Each data point an offer had becomes a
-- telemetry point named after its rid, or after its report and rid when
-- the connection had the same rid measuring different things in different
-- reports. Data points the worker could not have read are left as they
-- were, and stay a configuration error.

ALTER TABLE report_offers RENAME TO openadr2b_report_offers;
SELECT pg_temp.rename_prefixed('openadr2b_report_offers', 'report_offers',
                               'openadr2b_report_offers');

COMMENT ON TABLE openadr2b_report_offers IS
    'Reports the connection offers its OpenADR program, one row each, with the telemetry points they carry. Written by the API.';
COMMENT ON COLUMN openadr2b_report_offers.data_points IS
    '[{"rid", "point_id", "resource_id", "report_type", "reading_type"}]. rid defaults to point_id. See the README.';

CREATE TEMPORARY TABLE offer_points AS
SELECT o.id AS offer_id, o.connection_id, o.specifier_id, entry.position,
       entry.point,
       entry.point ->> 'rid' AS rid,
       jsonb_build_object(
           'kind', COALESCE(entry.point #>> '{item,kind}', 'power_real'),
           'units', COALESCE(entry.point #>> '{item,units}', 'W'),
           'scale', COALESCE(entry.point #>> '{item,scale}', 'none'),
           'description', COALESCE(entry.point #>> '{item,description}', '')) AS item,
       entry.point -> 'source' AS source,
       false AS valid,
       NULL::text AS point_id
FROM openadr2b_report_offers o,
     jsonb_array_elements(o.data_points) WITH ORDINALITY AS entry (point, position);

UPDATE offer_points
SET valid = jsonb_typeof(point) = 'object'
            AND COALESCE(rid, '') <> ''
            AND COALESCE(jsonb_typeof(point -> 'item'), 'null') IN ('object', 'null')
            AND jsonb_typeof(source) = 'object'
            AND item ->> 'kind' IN ('power_real', 'power_reactive', 'power_apparent',
                                    'energy_real', 'energy_reactive', 'energy_apparent',
                                    'temperature', 'custom')
            AND item ->> 'units' <> '' AND item ->> 'scale' <> ''
            AND (item ->> 'kind' <> 'custom' OR item ->> 'description' <> '')
            AND (item ->> 'kind' <> 'temperature'
                 OR item ->> 'units' IN ('celsius', 'fahrenheit'));

UPDATE offer_points p
SET point_id = CASE
        WHEN (SELECT count(DISTINCT (q.item, q.source)) FROM offer_points q
              WHERE q.valid AND q.connection_id = p.connection_id AND q.rid = p.rid) = 1
            THEN p.rid
        ELSE p.specifier_id || '/' || p.rid
    END
WHERE p.valid;

INSERT INTO telemetry_points (connection_id, point_id, kind, units, scale,
                              description, source)
SELECT DISTINCT ON (connection_id, point_id)
       connection_id, point_id, item ->> 'kind', item ->> 'units',
       item ->> 'scale', NULLIF(item ->> 'description', ''), source
FROM offer_points
WHERE point_id IS NOT NULL
ORDER BY connection_id, point_id, offer_id, position;

UPDATE openadr2b_report_offers o
SET data_points = (
    SELECT jsonb_agg(
               CASE
                   WHEN p.point_id IS NULL THEN p.point
                   ELSE (p.point - 'item' - 'source')
                        || jsonb_build_object('point_id', p.point_id)
               END ORDER BY p.position)
    FROM offer_points p
    WHERE p.offer_id = o.id)
WHERE EXISTS (SELECT 1 FROM offer_points p WHERE p.offer_id = o.id);

-- ---------------------------------------------------------------------------
-- OpenADR report requests

ALTER TABLE report_requests RENAME TO openadr2b_report_requests;
SELECT pg_temp.rename_prefixed('openadr2b_report_requests', 'report_requests',
                               'openadr2b_report_requests');

COMMENT ON TABLE openadr2b_report_requests IS
    'Report requests the OpenADR program made, and whether they are still being served. Written by the worker.';

-- ---------------------------------------------------------------------------
-- Report deliveries
--
-- Every protocol's reports are delivered here. An entry for a telemetry
-- point names it, and quality is one of
--
--   good        the value is good
--   no_value    there was nothing in the window
--   stale       the latest value is older than the source allows
--   unreadable  the datums could not be read
--
-- in place of the OpenADR data quality it was recorded as.

ALTER TABLE report_deliveries ALTER COLUMN request_id DROP NOT NULL;

COMMENT ON COLUMN report_deliveries.request_id IS
    'The OpenADR 2.0b report request the delivery answered. Null for a protocol whose program does not ask for reports.';
COMMENT ON COLUMN report_deliveries.payload IS
    '[{"rid", "point_id", "dtstart", "duration_seconds", "value" | "online" + "manual_override", "quality", "source"}]. quality is good, no_value, stale or unreadable. See the README.';

CREATE OR REPLACE FUNCTION pg_temp.neutral_entry(entry jsonb, point text) RETURNS jsonb
LANGUAGE sql IMMUTABLE AS $$
    SELECT entry
           || CASE WHEN point IS NULL THEN '{}'::jsonb
                   ELSE jsonb_build_object('point_id', point) END
           || CASE entry ->> 'quality'
                  WHEN 'Quality Good - Non Specific' THEN '{"quality": "good"}'::jsonb
                  WHEN 'No Quality - No Value' THEN '{"quality": "no_value"}'::jsonb
                  WHEN 'Quality Bad - Last Known Value' THEN '{"quality": "stale"}'::jsonb
                  WHEN 'Quality Bad - Comm Failure' THEN '{"quality": "unreadable"}'::jsonb
                  ELSE '{}'::jsonb
              END
$$;

UPDATE report_deliveries d
SET payload = (
    SELECT jsonb_agg(
               pg_temp.neutral_entry(entry.value, (
                   SELECT p.point_id
                   FROM offer_points p
                   JOIN openadr2b_report_requests r ON r.offer_id = p.offer_id
                   WHERE r.id = d.request_id
                     AND entry.value ? 'value'
                     AND p.rid = entry.value ->> 'rid'
                   ORDER BY p.position
                   LIMIT 1))
               ORDER BY entry.position)
    FROM jsonb_array_elements(d.payload) WITH ORDINALITY AS entry (value, position))
WHERE jsonb_typeof(d.payload) = 'array' AND jsonb_array_length(d.payload) > 0;

DROP TABLE offer_points;
DROP FUNCTION pg_temp.neutral_entry(jsonb, text);
DROP FUNCTION pg_temp.rename_prefixed(regclass, text, text);

-- migrate:down

CREATE OR REPLACE FUNCTION pg_temp.rename_prefixed(
    tbl regclass, old_prefix text, new_prefix text) RETURNS void
LANGUAGE plpgsql AS $$
DECLARE
    item record;
BEGIN
    FOR item IN SELECT c.relname FROM pg_index i
                JOIN pg_class c ON c.oid = i.indexrelid
                WHERE i.indrelid = tbl AND c.relname LIKE old_prefix || '\_%' LOOP
        EXECUTE format('ALTER INDEX %I RENAME TO %I', item.relname,
                       new_prefix || substr(item.relname, length(old_prefix) + 1));
    END LOOP;
    FOR item IN SELECT conname FROM pg_constraint
                WHERE conrelid = tbl AND conname LIKE old_prefix || '\_%' LOOP
        EXECUTE format('ALTER TABLE %s RENAME CONSTRAINT %I TO %I', tbl, item.conname,
                       new_prefix || substr(item.conname, length(old_prefix) + 1));
    END LOOP;
    FOR item IN SELECT s.relname FROM pg_class s
                JOIN pg_depend d ON d.objid = s.oid
                WHERE s.relkind = 'S' AND d.refobjid = tbl AND d.deptype = 'a'
                  AND s.relname LIKE old_prefix || '\_%' LOOP
        EXECUTE format('ALTER SEQUENCE %I RENAME TO %I', item.relname,
                       new_prefix || substr(item.relname, length(old_prefix) + 1));
    END LOOP;
    FOR item IN SELECT tgname FROM pg_trigger
                WHERE tgrelid = tbl AND NOT tgisinternal
                  AND tgname LIKE old_prefix || '\_%' LOOP
        EXECUTE format('ALTER TRIGGER %I ON %s RENAME TO %I', item.tgname, tbl,
                       new_prefix || substr(item.tgname, length(old_prefix) + 1));
    END LOOP;
END;
$$;

-- Report deliveries: OpenADR's quality, and no point ids. Deliveries no
-- OpenADR request asked for cannot be kept.

DELETE FROM report_deliveries WHERE request_id IS NULL;

UPDATE report_deliveries d
SET payload = (
    SELECT jsonb_agg(
               (entry.value - 'point_id')
               || CASE entry.value ->> 'quality'
                      WHEN 'good' THEN '{"quality": "Quality Good - Non Specific"}'::jsonb
                      WHEN 'no_value' THEN '{"quality": "No Quality - No Value"}'::jsonb
                      WHEN 'stale' THEN '{"quality": "Quality Bad - Last Known Value"}'::jsonb
                      WHEN 'unreadable' THEN '{"quality": "Quality Bad - Comm Failure"}'::jsonb
                      ELSE '{}'::jsonb
                  END
               ORDER BY entry.position)
    FROM jsonb_array_elements(d.payload) WITH ORDINALITY AS entry (value, position))
WHERE jsonb_typeof(d.payload) = 'array' AND jsonb_array_length(d.payload) > 0;

ALTER TABLE report_deliveries ALTER COLUMN request_id SET NOT NULL;

COMMENT ON COLUMN report_deliveries.request_id IS NULL;
COMMENT ON COLUMN report_deliveries.payload IS
    '[{"rid", "dtstart", "duration_seconds", "value" | "online" + "manual_override", "quality"}]. See the README.';

-- Report requests

ALTER TABLE openadr2b_report_requests RENAME TO report_requests;
SELECT pg_temp.rename_prefixed('report_requests', 'openadr2b_report_requests',
                               'report_requests');

COMMENT ON TABLE report_requests IS
    'Report requests the program made, and whether they are still being served. Written by the worker.';

-- Report offers carry their items and sources again

ALTER TABLE openadr2b_report_offers RENAME TO report_offers;
SELECT pg_temp.rename_prefixed('report_offers', 'openadr2b_report_offers',
                               'report_offers');

UPDATE report_offers o
SET data_points = (
    SELECT jsonb_agg(
               CASE
                   WHEN t.id IS NULL THEN entry.value
                   ELSE (entry.value - 'point_id')
                        || jsonb_build_object(
                               'rid', COALESCE(entry.value ->> 'rid', t.point_id),
                               'item', jsonb_strip_nulls(jsonb_build_object(
                                   'kind', t.kind, 'units', t.units,
                                   'scale', t.scale, 'description', t.description)),
                               'source', t.source)
               END ORDER BY entry.position)
    FROM jsonb_array_elements(o.data_points) WITH ORDINALITY AS entry (value, position)
    LEFT JOIN telemetry_points t
        ON t.connection_id = o.connection_id AND t.point_id = entry.value ->> 'point_id')
WHERE jsonb_array_length(o.data_points) > 0;

COMMENT ON TABLE report_offers IS
    'Reports the connection offers the program, one row each, with their data points. Written by the API.';
COMMENT ON COLUMN report_offers.data_points IS
    '[{"rid", "resource_id", "report_type", "reading_type", "item": {"kind", "units", "scale", "description"}, "source": {"node_id", "source_id", "property", "property_class", "reduce", "multiplier", "stale_after_seconds"}}]. See the README.';

DROP TABLE telemetry_points;

-- Accepted signals

ALTER TABLE connections
    ADD COLUMN accepted_signals text[] NOT NULL DEFAULT ARRAY['SIMPLE']
        CONSTRAINT connections_accepted_signals_check CHECK (
            accepted_signals <@ ARRAY['SIMPLE', 'ELECTRICITY_PRICE', 'LOAD_DISPATCH']);

UPDATE connections c
SET accepted_signals = o.accepted_signals
FROM openadr2b_connections o
WHERE o.connection_id = c.id AND c.accepted_signals IS DISTINCT FROM o.accepted_signals;

ALTER TABLE openadr2b_connections DROP COLUMN accepted_signals;

-- Client certificates go back to the connections that use them

ALTER TABLE openadr2b_connections
    ADD COLUMN client_cert_pem text,
    ADD COLUMN client_key_sealed text,
    ADD COLUMN cert_fingerprint text,
    ADD COLUMN cert_expires_at timestamptz;

UPDATE openadr2b_connections o
SET client_cert_pem = k.cert_pem,
    client_key_sealed = k.key_sealed,
    cert_fingerprint = k.cert_fingerprint,
    cert_expires_at = k.cert_expires_at
FROM connections c
JOIN client_credentials k ON k.id = c.client_credential_id
WHERE c.id = o.connection_id;

DROP INDEX connections_client_credential;
ALTER TABLE connections DROP COLUMN client_credential_id;
DROP TABLE client_credentials;
DROP FUNCTION client_credentials_touch_connections();

-- Identity

ALTER VIEW connection_health RENAME COLUMN client_id TO ven_id;
ALTER TABLE connection_state RENAME COLUMN client_id TO ven_id;
COMMENT ON COLUMN connection_state.ven_id IS NULL;

DROP FUNCTION pg_temp.rename_prefixed(regclass, text, text);
