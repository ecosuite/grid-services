-- migrate:up
-- CSIP-AUS: the DER's connected and operating state, for DERStatus.
--
-- ecosuite binds two telemetry points from the inverter's own status, each 1
-- or 0: whether the DER is connected to the grid, and whether it is
-- operating. The worker sends DERStatus from them alone; with either
-- unbound it sends none, and a failed der_status delivery says why.

-- ---------------------------------------------------------------------------
-- Telemetry points can be a state: 1 or 0, without units

ALTER TABLE telemetry_points
    DROP CONSTRAINT telemetry_points_kind_check,
    ADD CONSTRAINT telemetry_points_kind_check CHECK (kind IN (
        'power_real', 'power_reactive', 'power_apparent',
        'energy_real', 'energy_reactive', 'energy_apparent',
        'voltage', 'frequency', 'temperature', 'state', 'custom')),
    ADD CONSTRAINT telemetry_points_state_check
        CHECK (kind <> 'state' OR (units = 'none' AND scale = 'none'));

-- ---------------------------------------------------------------------------
-- The DER's state points (written by the API)
--
-- Deleting a point unbinds it; renaming one keeps it bound.

ALTER TABLE csipaus_der
    ADD COLUMN connected_point_id text,
    ADD COLUMN operating_point_id text,
    ADD CONSTRAINT csipaus_der_connected_point_fkey
        FOREIGN KEY (connection_id, connected_point_id)
        REFERENCES telemetry_points (connection_id, point_id)
        ON UPDATE CASCADE ON DELETE SET NULL (connected_point_id),
    ADD CONSTRAINT csipaus_der_operating_point_fkey
        FOREIGN KEY (connection_id, operating_point_id)
        REFERENCES telemetry_points (connection_id, point_id)
        ON UPDATE CASCADE ON DELETE SET NULL (operating_point_id);

COMMENT ON TABLE csipaus_der IS
    'The site''s DER as a csipaus connection describes it to the server in DERCapability, DERSettings and DERStatus. Optional: without it none is sent. Written by the API.';
COMMENT ON COLUMN csipaus_der.connected_point_id IS
    'The state telemetry point that is 1 while the DER is connected to the grid: DERStatus genConnectStatus bit 0. Null sends no DERStatus.';
COMMENT ON COLUMN csipaus_der.operating_point_id IS
    'The state telemetry point that is 1 while the DER is operating: DERStatus genConnectStatus bit 2 and operationalModeStatus. Null sends no DERStatus.';

-- migrate:down

COMMENT ON TABLE csipaus_der IS
    'The site''s DER as a csipaus connection describes it to the server in DERCapability and DERSettings. Optional: without it neither is sent. Written by the API.';
ALTER TABLE csipaus_der
    DROP COLUMN IF EXISTS operating_point_id,
    DROP COLUMN IF EXISTS connected_point_id;

DELETE FROM telemetry_points WHERE kind = 'state';
ALTER TABLE telemetry_points
    DROP CONSTRAINT IF EXISTS telemetry_points_state_check,
    DROP CONSTRAINT telemetry_points_kind_check,
    ADD CONSTRAINT telemetry_points_kind_check CHECK (kind IN (
        'power_real', 'power_reactive', 'power_apparent',
        'energy_real', 'energy_reactive', 'energy_apparent',
        'voltage', 'frequency', 'temperature', 'custom'));
