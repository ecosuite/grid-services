-- migrate:up
-- CSIP-AUS events.
--
-- A CSIP-AUS server's DERControls are events: each sets caps on the site
-- for a while. A DERProgram's DefaultDERControl, the caps that apply when no
-- DERControl does, is an open-ended event of its own. What every protocol
-- shares stays in events; what only CSIP-AUS has is in csipaus_events.
--
-- Three things become protocol-neutral:
--
--   - program_status gains 'superseded': the program replaced the event
--     with a newer one before it ended.
--   - events.declinable says whether the site may opt out. CSIP-AUS
--     controls are network limits and cannot be declined.
--   - event_responses records any answer given, not only opt answers:
--     CSIP-AUS answers that a control was received, started, completed,
--     cancelled or superseded.

ALTER TABLE events
    DROP CONSTRAINT events_program_status_check,
    ADD CONSTRAINT events_program_status_check
        CHECK (program_status IN ('pending', 'active', 'completed', 'cancelled', 'superseded'));

COMMENT ON COLUMN events.program_status IS
    'What the program last said, not a clock: it does not advance with time. Use start_at and duration_seconds for timing. superseded means the program replaced the event with a newer one.';
COMMENT ON COLUMN events.signals IS
    '[{"name", "type", "id", "intervals": [{"offset_seconds", "duration_seconds", "value"}]}]. Intervals are contiguous from the event start. SIMPLE values are levels 0 to 3, 0 meaning normal operation. CSIP-AUS caps are signals of type limit named for the cap. See the README.';

ALTER TABLE events ADD COLUMN declinable boolean NOT NULL DEFAULT true;

COMMENT ON COLUMN events.declinable IS
    'Whether the site may opt out of the event. False for events the program applies whatever the site answers, such as CSIP-AUS controls; opt_out and opt_in are refused for them.';

-- The state reads superseded as soon as the program says so, as it does
-- cancelled. Columns are listed so the view keeps its grants: declinable
-- goes at the end.
CREATE OR REPLACE VIEW event_state AS
SELECT e.id, e.site_code, e.source, e.connection_id, e.program_event_id,
       e.modification_number, e.program_status, e.start_at,
       e.duration_seconds, e.start_after_seconds, e.signals, e.market_context,
       e.test_event, e.response_required, e.program_comment,
       e.program_request_id, e.opt, e.opt_source, e.lifecycle, e.received_at,
       e.updated_at, e.site_id, e.created_by,
       CASE
           WHEN e.program_status = 'cancelled' THEN 'cancelled'
           WHEN e.program_status = 'superseded' THEN 'superseded'
           WHEN e.opt = 'out' THEN 'opted_out'
           ELSE e.lifecycle
       END AS state,
       -- Null for an open-ended event. The program's randomization window,
       -- start_after_seconds, is not included.
       CASE
           WHEN e.duration_seconds > 0
               THEN e.start_at + make_interval(secs => e.duration_seconds)
       END AS end_at,
       e.declinable
FROM events e;

-- Answers: 'opt_in' and 'opt_out' are OpenADR's; the rest are CSIP-AUS's
-- DERControlResponse statuses.
ALTER TABLE event_responses ADD COLUMN response text;
UPDATE event_responses SET response = 'opt_' || opt;
ALTER TABLE event_responses
    ALTER COLUMN response SET NOT NULL,
    ADD CONSTRAINT event_responses_response_check CHECK (response IN (
        'opt_in', 'opt_out', 'received', 'started', 'completed', 'cancelled',
        'superseded')),
    DROP COLUMN opt;

COMMENT ON TABLE event_responses IS
    'Answers sent to the program about its events, or attempted. Written by the worker.';
COMMENT ON COLUMN event_responses.response IS
    'The answer: opt_in or opt_out (OpenADR), or received, started, completed, cancelled or superseded (CSIP-AUS).';

-- ---------------------------------------------------------------------------
-- CSIP-AUS events (written by the worker)
--
-- One row per csipaus program event: what CSIP-AUS says about the event that
-- the shared columns do not. kind is 'control' for a DERControl and
-- 'default' for a DERProgram's DefaultDERControl.

CREATE TABLE csipaus_events (
    event_id                   bigint      PRIMARY KEY REFERENCES events (id) ON DELETE CASCADE,
    kind                       text        NOT NULL CHECK (kind IN ('control', 'default')),
    -- The control's href on the server.
    href                       text        NOT NULL,
    -- The control's own mRID. The default's event is keyed by its program,
    -- as a server may give an updated default a new mRID.
    mrid                       text        NOT NULL,
    program_mrid               text        NOT NULL,
    program_description        text,
    -- Lower numbers take precedence when programs' controls overlap.
    primacy                    integer     NOT NULL CHECK (primacy BETWEEN 0 AND 255),
    -- When the server created the control. Null for a default.
    creation_time              timestamptz,
    -- The control's randomization: its start and end move by up to this
    -- many seconds, later when positive and earlier when negative.
    randomize_start_seconds    integer,
    randomize_duration_seconds integer,
    -- Where responses are posted, and which the server asks for: bit 0 for
    -- received, bit 1 for started, completed, cancelled and superseded.
    reply_to                   text,
    response_required          integer     NOT NULL DEFAULT 0 CHECK (response_required BETWEEN 0 AND 255),
    updated_at                 timestamptz NOT NULL DEFAULT now()
);

COMMENT ON TABLE csipaus_events IS
    'What CSIP-AUS says about each csipaus program event beyond the shared columns of events. Written by the worker.';

CREATE TRIGGER csipaus_events_updated_at
    BEFORE UPDATE ON csipaus_events
    FOR EACH ROW EXECUTE FUNCTION touch_updated_at();

-- migrate:down

DROP TABLE csipaus_events;

ALTER TABLE event_responses ADD COLUMN opt text;
-- CSIP-AUS answers have no opt to keep.
DELETE FROM event_responses WHERE response NOT IN ('opt_in', 'opt_out');
UPDATE event_responses SET opt = substr(response, 5);
ALTER TABLE event_responses
    ALTER COLUMN opt SET NOT NULL,
    ADD CONSTRAINT event_responses_opt_check CHECK (opt IN ('in', 'out')),
    DROP COLUMN response;

COMMENT ON TABLE event_responses IS NULL;

DROP VIEW event_state;
ALTER TABLE events DROP COLUMN declinable;
CREATE VIEW event_state AS
SELECT e.*,
       CASE
           WHEN e.program_status = 'cancelled' THEN 'cancelled'
           WHEN e.opt = 'out' THEN 'opted_out'
           ELSE e.lifecycle
       END AS state,
       CASE
           WHEN e.duration_seconds > 0
               THEN e.start_at + make_interval(secs => e.duration_seconds)
       END AS end_at
FROM events e;

-- Fails while superseded events remain.
ALTER TABLE events
    DROP CONSTRAINT events_program_status_check,
    ADD CONSTRAINT events_program_status_check
        CHECK (program_status IN ('pending', 'active', 'completed', 'cancelled'));

COMMENT ON COLUMN events.program_status IS
    'What the program last said, not a clock: it does not advance with time. Use start_at and duration_seconds for timing.';
COMMENT ON COLUMN events.signals IS
    '[{"name", "type", "id", "intervals": [{"offset_seconds", "duration_seconds", "value"}]}]. Intervals are contiguous from the event start. SIMPLE values are levels 0 to 3, 0 meaning normal operation. See the README.';
