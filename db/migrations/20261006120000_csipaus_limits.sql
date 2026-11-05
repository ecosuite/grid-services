-- migrate:up
-- CSIP-AUS: the server's clock, and the caps in force.
--
-- A CSIP-AUS device keeps time by its server's Time resource, so the worker
-- records how far the server's clock is from its own. Times the server
-- sends, in events and csipaus_limits, are in the server's time: subtract
-- clock_offset_seconds for the worker's.
--
-- csipaus_limits is what the server's controls and defaults come to: for
-- each cap, the value in force over time. Ecosuite's planner acts on it.

ALTER TABLE connection_state
    ADD COLUMN clock_offset_seconds integer,
    ADD COLUMN clock_checked_at timestamptz;

COMMENT ON COLUMN connection_state.clock_offset_seconds IS
    'How far the program''s clock is ahead of the worker''s, in seconds; negative when behind. Null for protocols that do not keep time with the program.';
COMMENT ON COLUMN connection_state.clock_checked_at IS
    'When the worker last read the program''s clock.';

CREATE TABLE csipaus_limits (
    connection_id bigint      NOT NULL REFERENCES connections (id) ON DELETE CASCADE,
    -- The cap, as its signal is named.
    limit_name    text        NOT NULL CHECK (limit_name IN (
                      'export', 'import', 'generation', 'load', 'max_output',
                      'energize', 'connect')),
    start_at      timestamptz NOT NULL,
    -- Null while it holds until further notice.
    end_at        timestamptz CHECK (end_at > start_at),
    -- Watts for export, import, generation and load; percent of the site's
    -- rating for max_output; 1 for on and 0 for off for energize and connect.
    value         double precision NOT NULL,
    -- A program's default, or a control.
    source        text        NOT NULL CHECK (source IN ('default', 'control')),
    -- The event the value comes from.
    event_id      bigint      REFERENCES events (id) ON DELETE SET NULL,
    PRIMARY KEY (connection_id, limit_name, start_at)
);

COMMENT ON TABLE csipaus_limits IS
    'The caps in force for each csipaus connection, cap by cap over time, in the server''s time. Replaced by the worker whenever they change. Written by the worker.';

-- migrate:down

DROP TABLE csipaus_limits;

ALTER TABLE connection_state
    DROP COLUMN clock_offset_seconds,
    DROP COLUMN clock_checked_at;
