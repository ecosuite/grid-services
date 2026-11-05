-- migrate:up
-- CSIP-AUS: each control's window as the device keeps it.
--
-- A device moves a control's start by up to randomizeStart and its end by
-- that plus up to randomizeDuration, by an amount of its own. The worker
-- writes the window it keeps, in the server's time, so that ecosuite judges
-- when a control is in force without working out the randomization itself.
-- A default has no window. A negative randomizeDuration can leave a window
-- empty, and such a control is never in force.

ALTER TABLE csipaus_events
    ADD COLUMN effective_start_at timestamptz,
    ADD COLUMN effective_end_at   timestamptz,
    ADD CONSTRAINT csipaus_events_effective_window_check CHECK (
        (effective_start_at IS NULL) = (effective_end_at IS NULL)
        AND (kind = 'control' OR effective_start_at IS NULL));

COMMENT ON COLUMN csipaus_events.effective_start_at IS
    'When a control starts on this device, after its randomizeStart, in the server''s time. Null for a default.';
COMMENT ON COLUMN csipaus_events.effective_end_at IS
    'When a control ends on this device, after its randomizeStart and randomizeDuration, in the server''s time. Null for a default.';

-- migrate:down

ALTER TABLE csipaus_events
    DROP CONSTRAINT csipaus_events_effective_window_check,
    DROP COLUMN effective_start_at,
    DROP COLUMN effective_end_at;
