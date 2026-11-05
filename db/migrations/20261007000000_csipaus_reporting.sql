-- migrate:up
-- CSIP-AUS: describing the site's DER, and posting its readings.
--
-- A CSIP-AUS device tells its server what its DER is and how it is set
-- (DERCapability and DERSettings, on connection and on change), how it is
-- doing (DERStatus, at the DERList's poll rate and on change), and what the
-- site and the DER measure, through mirror usage points at the post rate
-- the server gives them.
--
--   API     writes csipaus_der and csipaus_readings
--   worker  writes report_deliveries, one row per document sent
--
-- Readings are telemetry points, read from SolarNetwork with the
-- connection's data credential like any other report's values.

-- ---------------------------------------------------------------------------
-- Telemetry points can measure voltage and frequency

ALTER TABLE telemetry_points
    DROP CONSTRAINT telemetry_points_kind_check,
    ADD CONSTRAINT telemetry_points_kind_check CHECK (kind IN (
        'power_real', 'power_reactive', 'power_apparent',
        'energy_real', 'energy_reactive', 'energy_apparent',
        'voltage', 'frequency', 'temperature', 'custom'));

-- ---------------------------------------------------------------------------
-- The DER (written by the API)
--
-- What the site's DER is rated for and set to, in watts, volt-amperes, vars,
-- watt-hours and volts. A site with several DER describes them as one: the
-- sum of their ratings, of the mix's type.

CREATE TABLE csipaus_der (
    connection_id            bigint           PRIMARY KEY REFERENCES csipaus_connections (connection_id) ON DELETE CASCADE,
    -- IEEE 2030.5 DERType: 0 unknown, 1 virtual or mixed, 2 reciprocating
    -- engine, 3 fuel cell, 4 photovoltaic, 5 combined heat and power, 6
    -- other generation, 80 other storage, 81 electric vehicle, 82 EVSE, 83
    -- photovoltaic and storage.
    der_type                 smallint         NOT NULL CHECK (der_type IN (
                                 0, 1, 2, 3, 4, 5, 6, 80, 81, 82, 83)),
    rtg_max_w                double precision NOT NULL CHECK (rtg_max_w > 0),
    rtg_max_va               double precision CHECK (rtg_max_va > 0),
    rtg_max_var              double precision CHECK (rtg_max_var > 0),
    rtg_max_var_neg          double precision CHECK (rtg_max_var_neg > 0),
    rtg_max_charge_rate_w    double precision CHECK (rtg_max_charge_rate_w > 0),
    rtg_max_discharge_rate_w double precision CHECK (rtg_max_discharge_rate_w > 0),
    rtg_max_wh               double precision CHECK (rtg_max_wh > 0),
    rtg_v_nom                double precision CHECK (rtg_v_nom > 0),
    -- The settings, where they are below the ratings. Null is the rating.
    set_max_w                double precision CHECK (set_max_w > 0),
    set_max_va               double precision CHECK (set_max_va > 0),
    set_max_var              double precision CHECK (set_max_var > 0),
    set_max_charge_rate_w    double precision CHECK (set_max_charge_rate_w > 0),
    set_max_discharge_rate_w double precision CHECK (set_max_discharge_rate_w > 0),
    -- The default ramp rate, in hundredths of a percent of set_max_w per
    -- second. 27 is AS/NZS 4777.2's 16.67 % a minute.
    set_grad_w               integer          NOT NULL DEFAULT 27 CHECK (set_grad_w BETWEEN 1 AND 65535),
    -- The caps the site can act on, and those it acts on now, as
    -- csipaus_limits names them.
    modes_supported          text[]           NOT NULL DEFAULT '{}' CHECK (modes_supported <@ ARRAY[
                                 'export', 'import', 'generation', 'load',
                                 'max_output', 'energize', 'connect']),
    modes_enabled            text[]           NOT NULL DEFAULT '{}',
    created_at               timestamptz      NOT NULL DEFAULT now(),
    updated_at               timestamptz      NOT NULL DEFAULT now(),
    CONSTRAINT csipaus_der_modes_enabled_check CHECK (modes_enabled <@ modes_supported),
    CONSTRAINT csipaus_der_set_max_w_rating_check CHECK (set_max_w <= rtg_max_w)
);

COMMENT ON TABLE csipaus_der IS
    'The site''s DER as a csipaus connection describes it to the server in DERCapability and DERSettings. Optional: without it neither is sent. Written by the API.';
COMMENT ON COLUMN csipaus_der.modes_supported IS
    'The caps the site can act on: export, import, generation and load are csipaus:doeModesSupported; max_output, energize and connect are opModMaxLimW, opModEnergize and opModConnect in modesSupported.';
COMMENT ON COLUMN csipaus_der.modes_enabled IS
    'The caps the site acts on now, sent as DERSettings'' modesEnabled and csipaus:doeModesEnabled. A subset of modes_supported.';
COMMENT ON COLUMN csipaus_der.updated_at IS
    'Sent as DERSettings'' updatedTime.';

CREATE TRIGGER csipaus_der_touch_connection
    AFTER INSERT OR UPDATE OR DELETE ON csipaus_der
    FOR EACH ROW EXECUTE FUNCTION protocol_settings_touch_connection();

CREATE TRIGGER csipaus_der_updated_at
    BEFORE UPDATE ON csipaus_der
    FOR EACH ROW EXECUTE FUNCTION touch_updated_at();

-- ---------------------------------------------------------------------------
-- Readings (written by the API)
--
-- Which telemetry point each reading is posted from. The site's readings go
-- to a mirror usage point for the connection point, and the DER's to one for
-- the DER:
--
--   reading         point kind      sent as
--   real_power      power_real      W, average
--   reactive_power  power_reactive  var, average
--   voltage         voltage         V, average, per phase on three phases
--   frequency       frequency       Hz, average
--
-- Site readings are positive for import; DER readings positive for
-- generation. A point's multiplier makes them so.

CREATE TABLE csipaus_readings (
    connection_id bigint NOT NULL REFERENCES csipaus_connections (connection_id) ON DELETE CASCADE,
    mirror        text   NOT NULL CHECK (mirror IN ('site', 'der')),
    reading       text   NOT NULL CHECK (reading IN (
                      'real_power', 'reactive_power', 'voltage', 'frequency')),
    -- none for a single-phase or total reading; a, b or c for a phase's
    -- voltage to neutral.
    phase         text   NOT NULL DEFAULT 'none' CHECK (phase IN ('none', 'a', 'b', 'c')),
    point_id      text   NOT NULL,
    PRIMARY KEY (connection_id, mirror, reading, phase),
    FOREIGN KEY (connection_id, point_id)
        REFERENCES telemetry_points (connection_id, point_id) ON UPDATE CASCADE ON DELETE CASCADE,
    CONSTRAINT csipaus_readings_phase_voltage_check CHECK (phase = 'none' OR reading = 'voltage')
);

COMMENT ON TABLE csipaus_readings IS
    'The readings a csipaus connection posts to its server, and the telemetry point each is read from. Written by the API.';

CREATE INDEX csipaus_readings_point ON csipaus_readings (connection_id, point_id);

CREATE TRIGGER csipaus_readings_touch_connection
    AFTER INSERT OR UPDATE OR DELETE ON csipaus_readings
    FOR EACH ROW EXECUTE FUNCTION protocol_settings_touch_connection();

-- ---------------------------------------------------------------------------
-- Deliveries say what they reported, and keep the exchange

ALTER TABLE report_deliveries
    ADD COLUMN report text CONSTRAINT report_deliveries_report_check CHECK (report <> ''),
    ADD COLUMN exchange jsonb CONSTRAINT report_deliveries_exchange_check
        CHECK (exchange IS NULL OR jsonb_typeof(exchange) = 'array');

COMMENT ON COLUMN report_deliveries.report IS
    'What was reported, for a protocol whose program does not ask for reports. CSIP-AUS: site_readings, der_readings, der_capability, der_settings or der_status. Null for OpenADR 2.0b, whose request says.';
COMMENT ON COLUMN report_deliveries.exchange IS
    'The messages exchanged to deliver it, in the form of activity.exchange. Null when they were not kept.';

CREATE INDEX report_deliveries_connection_report_time
    ON report_deliveries (connection_id, report, sent_at DESC) WHERE report IS NOT NULL;

-- migrate:down

DROP INDEX IF EXISTS report_deliveries_connection_report_time;
ALTER TABLE report_deliveries DROP COLUMN IF EXISTS exchange;
DELETE FROM report_deliveries WHERE request_id IS NULL AND report IS NOT NULL;
ALTER TABLE report_deliveries DROP COLUMN IF EXISTS report;

DROP TABLE IF EXISTS csipaus_readings;
DROP TABLE IF EXISTS csipaus_der;

DELETE FROM telemetry_points WHERE kind IN ('voltage', 'frequency');
ALTER TABLE telemetry_points
    DROP CONSTRAINT telemetry_points_kind_check,
    ADD CONSTRAINT telemetry_points_kind_check CHECK (kind IN (
        'power_real', 'power_reactive', 'power_apparent',
        'energy_real', 'energy_reactive', 'energy_apparent',
        'temperature', 'custom'));
