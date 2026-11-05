-- migrate:up
-- Reports: what a connection offers the program, where the values come
-- from, and what was sent.
--
-- OpenADR 2.0b reporting is an offer and a request. The VEN registers the
-- data points it can report and at what sampling rates; the program then
-- asks for the ones it wants, at a granularity and report-back period of
-- its choosing. So the API configures the offer, and the worker records the
-- program's requests and every report it sends against them.
--
--   API     writes connection_data_credentials and report_offers
--   worker  writes report_requests and report_deliveries
--
-- TELEMETRY_STATUS is not an offer: the worker always offers it, from the
-- connection's own state.

-- ---------------------------------------------------------------------------
-- Where the values come from (written by the API)
--
-- One SolarNetwork data token per connection, read-only, sealed like a
-- client key.

CREATE TABLE connection_data_credentials (
    connection_id bigint      PRIMARY KEY REFERENCES connections (id) ON DELETE CASCADE,
    -- SolarNetwork API host, such as data.solarnetwork.net.
    host          text        NOT NULL CHECK (host <> ''),
    token         text        NOT NULL CHECK (token <> ''),
    -- Token secret, sealed with the worker's public key (libsodium
    -- crypto_box_seal), base64 encoded. Only the worker can open it.
    secret_sealed text        NOT NULL CHECK (secret_sealed <> ''),
    created_at    timestamptz NOT NULL DEFAULT now(),
    updated_at    timestamptz NOT NULL DEFAULT now()
);

COMMENT ON TABLE connection_data_credentials IS
    'The SolarNetwork token the worker reads datums with for the connection''s reports. Written by the API.';

-- ---------------------------------------------------------------------------
-- Offers (written by the API)
--
-- One row per report the connection offers, with its data points. A data
-- point is one rID in the report, and says where its value comes from:
--
--   [{"rid": "site-load-power",
--     "resource_id": "SITE-01",
--     "report_type": "usage",
--     "reading_type": "Direct Read",
--     "item": {"kind": "power_real", "units": "W", "scale": "none"},
--     "source": {"node_id": 604,
--                "source_id": "/PRJ/SITE-01/S1/MET/1",
--                "property": "watts",
--                "property_class": "instantaneous",
--                "reduce": "average",
--                "multiplier": 1,
--                "stale_after_seconds": 600}}]
--
--   report_type     an OpenADR ReportEnumeratedType value: usage, demand,
--                   baseline, storedEnergy, availableEnergyStorage, ...
--   reading_type    an OpenADR ReadingTypeEnumeratedType value: Direct Read,
--                   Mean, Summed, Estimated, ...
--   item.kind       power_real, power_reactive, power_apparent, energy_real,
--                   energy_reactive, energy_apparent, temperature, custom.
--                   custom carries a description; temperature's units are
--                   celsius or fahrenheit; scale is an SI prefix: none, k,
--                   M, ...
--   property_class  instantaneous (sampled, averaged over a window) or
--                   accumulating (a meter reading, differenced over a
--                   window)
--   reduce          how a window of datums becomes one value: average, min,
--                   max, difference or last
--   multiplier      sign and unit scaling applied to the value
--   stale_after_seconds
--                   a value older than this is reported with a Quality Bad
--                   data quality

CREATE TABLE report_offers (
    id                   bigserial   PRIMARY KEY,
    connection_id        bigint      NOT NULL REFERENCES connections (id) ON DELETE CASCADE,
    -- TELEMETRY_USAGE, HISTORY_USAGE, or a custom name. Metadata names are
    -- derived, and TELEMETRY_STATUS is always offered by the worker.
    report_name          text        NOT NULL CHECK (
                             report_name <> ''
                             AND report_name NOT LIKE 'METADATA\_%'
                             AND report_name <> 'TELEMETRY_STATUS'),
    -- The reportSpecifierID the program refers to the report by.
    specifier_id         text        NOT NULL CHECK (specifier_id <> ''),
    enabled              boolean     NOT NULL DEFAULT true,
    -- The oadrSamplingRate offered: the program may not request a
    -- granularity below the minimum.
    min_sampling_seconds integer     NOT NULL CHECK (min_sampling_seconds > 0),
    max_sampling_seconds integer     NOT NULL CHECK (max_sampling_seconds >= min_sampling_seconds),
    -- How far back the worker will serve the report's data. Zero offers no
    -- history.
    history_seconds      integer     NOT NULL DEFAULT 0 CHECK (history_seconds >= 0),
    data_points          jsonb       NOT NULL DEFAULT '[]'
                             CHECK (jsonb_typeof(data_points) = 'array'),
    created_at           timestamptz NOT NULL DEFAULT now(),
    updated_at           timestamptz NOT NULL DEFAULT now(),
    UNIQUE (connection_id, specifier_id)
);

COMMENT ON TABLE report_offers IS
    'Reports the connection offers the program, one row each, with their data points. Written by the API.';
COMMENT ON COLUMN report_offers.data_points IS
    '[{"rid", "resource_id", "report_type", "reading_type", "item": {"kind", "units", "scale", "description"}, "source": {"node_id", "source_id", "property", "property_class", "reduce", "multiplier", "stale_after_seconds"}}]. See the README.';

CREATE INDEX report_offers_connection ON report_offers (connection_id, id);

-- A change to an offer or a credential is a change to the connection.
CREATE TRIGGER connection_data_credentials_touch_connection
    AFTER INSERT OR UPDATE OR DELETE ON connection_data_credentials
    FOR EACH ROW EXECUTE FUNCTION protocol_settings_touch_connection();

CREATE TRIGGER report_offers_touch_connection
    AFTER INSERT OR UPDATE OR DELETE ON report_offers
    FOR EACH ROW EXECUTE FUNCTION protocol_settings_touch_connection();

CREATE FUNCTION touch_updated_at() RETURNS trigger
LANGUAGE plpgsql AS $$
BEGIN
    NEW.updated_at := now();
    RETURN NEW;
END;
$$;

CREATE TRIGGER connection_data_credentials_updated_at
    BEFORE UPDATE ON connection_data_credentials
    FOR EACH ROW EXECUTE FUNCTION touch_updated_at();

CREATE TRIGGER report_offers_updated_at
    BEFORE UPDATE ON report_offers
    FOR EACH ROW EXECUTE FUNCTION touch_updated_at();

-- ---------------------------------------------------------------------------
-- Requests (written by the worker)
--
-- One row per report request the program made. A one-shot request is
-- completed as soon as its report is sent; a periodic request stays active
-- until its window ends or the program cancels it; a history request is
-- completed once the history is sent. Requests do not survive a
-- re-registration: the program asks again, and the old rows are completed.

CREATE TABLE report_requests (
    id                  bigserial   PRIMARY KEY,
    connection_id       bigint      NOT NULL REFERENCES connections (id) ON DELETE CASCADE,
    -- The offer the request is for. Null once the offer is deleted, and
    -- for the program's metadata requests.
    offer_id            bigint      REFERENCES report_offers (id) ON DELETE SET NULL,
    -- The program's reportRequestID.
    report_request_id   text        NOT NULL,
    specifier_id        text        NOT NULL,
    report_name         text        NOT NULL,
    kind                text        NOT NULL CHECK (kind IN ('one_shot', 'periodic', 'history')),
    -- Zero asks for values only when they change.
    granularity_seconds integer     NOT NULL CHECK (granularity_seconds >= 0),
    -- Zero for one-shot and history requests.
    report_back_seconds integer     NOT NULL CHECK (report_back_seconds >= 0),
    -- The window the request covers. Null for a one-shot request. Zero
    -- duration means until cancelled.
    start_at            timestamptz,
    duration_seconds    integer     CHECK (duration_seconds >= 0),
    -- The rIDs requested.
    rids                text[]      NOT NULL,
    status              text        NOT NULL DEFAULT 'active'
                            CHECK (status IN ('active', 'completed', 'cancelled')),
    created_at          timestamptz NOT NULL DEFAULT now(),
    ended_at            timestamptz,
    CHECK ((status = 'active') = (ended_at IS NULL))
);

COMMENT ON TABLE report_requests IS
    'Report requests the program made, and whether they are still being served. Written by the worker.';

CREATE INDEX report_requests_connection ON report_requests (connection_id, created_at DESC);
CREATE INDEX report_requests_offer ON report_requests (offer_id, created_at DESC) WHERE offer_id IS NOT NULL;
-- The program may reuse a request id once the request has ended.
CREATE UNIQUE INDEX report_requests_active_key
    ON report_requests (connection_id, report_request_id) WHERE status = 'active';

-- ---------------------------------------------------------------------------
-- Deliveries (written by the worker)
--
-- One row per oadrUpdateReport sent for a request, or attempted. payload is
-- what the report carried:
--
--   [{"rid": "site-load-power",
--     "dtstart": "2026-09-30T14:00:00.000Z",
--     "duration_seconds": 300,
--     "value": 12345.6,
--     "quality": "Quality Good - Non Specific"}]
--
-- A resource status point carries {"online": true, "manual_override":
-- false} in place of "value". quality is an OpenADR oadrDataQualityType
-- value.

CREATE TABLE report_deliveries (
    id             bigserial   PRIMARY KEY,
    request_id     bigint      NOT NULL REFERENCES report_requests (id) ON DELETE CASCADE,
    connection_id  bigint      NOT NULL REFERENCES connections (id) ON DELETE CASCADE,
    sent_at        timestamptz NOT NULL DEFAULT now(),
    -- The span the report's intervals cover. Null when the report carried
    -- point data only.
    interval_start timestamptz,
    interval_end   timestamptz,
    payload        jsonb       NOT NULL DEFAULT '[]'
                       CHECK (jsonb_typeof(payload) = 'array'),
    status         text        NOT NULL CHECK (status IN ('sent', 'failed')),
    -- The program's eiResponse code, when it answered.
    response_code  text,
    message        text
);

COMMENT ON TABLE report_deliveries IS
    'Every report sent to the program, with the values it carried. Written by the worker.';
COMMENT ON COLUMN report_deliveries.payload IS
    '[{"rid", "dtstart", "duration_seconds", "value" | "online" + "manual_override", "quality"}]. See the README.';

CREATE INDEX report_deliveries_request_time ON report_deliveries (request_id, sent_at DESC);
CREATE INDEX report_deliveries_connection_time ON report_deliveries (connection_id, sent_at DESC);

-- ---------------------------------------------------------------------------
-- Activity: report entries

ALTER TABLE activity
    DROP CONSTRAINT activity_category_check,
    ADD CONSTRAINT activity_category_check
        CHECK (category IN ('connection', 'events', 'instructions', 'configuration', 'reports'));

COMMENT ON COLUMN activity.category IS
    'The worker writes connection, events and reports entries. Ecosuite writes instructions and configuration entries, and events entries for operator actions on manual events.';

-- ---------------------------------------------------------------------------
-- Change signals
--
-- grid_services_reports  payload: connection id. A report request started or
--                        ended, or a report was delivered or failed.

CREATE FUNCTION reports_notify() RETURNS trigger
LANGUAGE plpgsql AS $$
BEGIN
    PERFORM pg_notify('grid_services_reports', NEW.connection_id::text);
    RETURN NULL;
END;
$$;

CREATE TRIGGER report_requests_notify
    AFTER INSERT OR UPDATE OF status ON report_requests
    FOR EACH ROW EXECUTE FUNCTION reports_notify();

CREATE TRIGGER report_deliveries_notify
    AFTER INSERT ON report_deliveries
    FOR EACH ROW EXECUTE FUNCTION reports_notify();

-- migrate:down

DROP TRIGGER IF EXISTS report_deliveries_notify ON report_deliveries;
DROP TRIGGER IF EXISTS report_requests_notify ON report_requests;
DROP FUNCTION IF EXISTS reports_notify();

DELETE FROM activity WHERE category = 'reports';

COMMENT ON COLUMN activity.category IS
    'The worker writes connection and events entries. Ecosuite writes instructions and configuration entries, and events entries for operator actions on manual events.';

ALTER TABLE activity
    DROP CONSTRAINT activity_category_check,
    ADD CONSTRAINT activity_category_check
        CHECK (category IN ('connection', 'events', 'instructions', 'configuration'));

DROP TABLE IF EXISTS report_deliveries;
DROP TABLE IF EXISTS report_requests;
DROP TABLE IF EXISTS report_offers;
DROP TABLE IF EXISTS connection_data_credentials;
DROP FUNCTION IF EXISTS touch_updated_at();
