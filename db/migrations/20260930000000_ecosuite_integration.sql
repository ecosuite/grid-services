-- migrate:up
-- Ecosuite integration.
--
-- What the ecosuite server, planner and notifier need from the contract:
-- signals for event changes and command results, a full contract for manual
-- events, connections that can be retired, one health value per connection
-- and one state per event.
--
-- "The worker" below is any session of the role that owns these tables, which
-- is the role the worker and the migrations connect as. Every other role is a
-- client, such as the ecosuite API.

-- ---------------------------------------------------------------------------
-- Sites
--
-- site_code joins this database to ecosuite's, with no foreign key. Ecosuite
-- site codes can change, so its immutable site id is stored alongside.

ALTER TABLE connections ADD COLUMN site_id uuid;
ALTER TABLE events ADD COLUMN site_id uuid;
ALTER TABLE activity ADD COLUMN site_id uuid;

CREATE INDEX connections_site_id ON connections (site_id);
CREATE INDEX events_site_id_start ON events (site_id, start_at);
CREATE INDEX activity_site_id_time ON activity (site_id, created_at DESC);

COMMENT ON COLUMN connections.site_code IS
    'The ecosuite site code, not a project code. For display; ecosuite updates it when the site''s code changes.';
COMMENT ON COLUMN connections.site_id IS
    'Ecosuite''s immutable site id (sites.id). The join key to ecosuite.';
COMMENT ON COLUMN events.site_code IS
    'The site''s code when the row was written. It does not follow a later change of code; join on site_id.';
COMMENT ON COLUMN events.site_id IS
    'Ecosuite''s immutable site id. Copied from the connection for program events, set by ecosuite for manual ones.';
COMMENT ON COLUMN activity.site_code IS
    'The site''s code when the row was written. It does not follow a later change of code; join on site_id.';
COMMENT ON COLUMN activity.site_id IS
    'Ecosuite''s immutable site id. Copied from the connection by the worker, set by ecosuite for its own entries.';

-- ---------------------------------------------------------------------------
-- Connections: archiving and accepted signals

ALTER TABLE connections
    -- Set to retire a connection. An archived connection is never run, and
    -- its events and activity stay. A connection that has received events
    -- cannot be deleted, only archived.
    ADD COLUMN archived_at timestamptz,
    -- Signals the site can act on. An event with any other signal is refused
    -- with 460 and not stored.
    ADD COLUMN accepted_signals text[] NOT NULL DEFAULT ARRAY['SIMPLE']
        CONSTRAINT connections_accepted_signals_check CHECK (
            accepted_signals <@ ARRAY['SIMPLE', 'ELECTRICITY_PRICE', 'LOAD_DISPATCH']);

COMMENT ON COLUMN connections.enabled IS
    'False stops the connection. A registered connection is unregistered from the program first.';
COMMENT ON COLUMN connections.archived_at IS
    'Set to retire the connection for good. A registered connection is unregistered from the program first.';

-- Any change to the row is a configuration change.
CREATE OR REPLACE FUNCTION connections_bump_config_version() RETURNS trigger
LANGUAGE plpgsql AS $$
BEGIN
    IF to_jsonb(NEW) - 'config_version' IS DISTINCT FROM to_jsonb(OLD) - 'config_version' THEN
        NEW.config_version := OLD.config_version + 1;
        NEW.updated_at := now();
    END IF;
    RETURN NEW;
END;
$$;

-- A registration belongs to the program at vtn_url. Moving a registered
-- connection to another program would leave the old one with a VEN that no
-- longer polls, so the connection has to be unregistered first.
CREATE FUNCTION openadr2b_connections_keep_program() RETURNS trigger
LANGUAGE plpgsql AS $$
BEGIN
    IF rtrim(NEW.vtn_url, '/') IS DISTINCT FROM rtrim(OLD.vtn_url, '/')
       AND EXISTS (SELECT 1 FROM connection_state s
                   WHERE s.connection_id = OLD.connection_id
                     AND s.registration = 'registered') THEN
        RAISE EXCEPTION 'connection % is registered with the program at %; unregister it before changing vtn_url',
            OLD.connection_id, OLD.vtn_url
            USING ERRCODE = 'object_not_in_prerequisite_state';
    END IF;
    RETURN NEW;
END;
$$;

CREATE TRIGGER openadr2b_connections_keep_program
    BEFORE UPDATE OF vtn_url ON openadr2b_connections
    FOR EACH ROW EXECUTE FUNCTION openadr2b_connections_keep_program();

-- ---------------------------------------------------------------------------
-- Connection state: retries, and configuration errors

ALTER TABLE connection_state
    -- Failed attempts to reach the program since the last success, and when
    -- the next attempt is due. Zero and null while contact is good.
    ADD COLUMN consecutive_failures integer NOT NULL DEFAULT 0,
    ADD COLUMN next_retry_at timestamptz,
    -- Why the connection's configuration cannot be used. Null when it can.
    ADD COLUMN config_error text;

-- ---------------------------------------------------------------------------
-- Commands: opt_in, expiry, structured results

ALTER TABLE connection_commands
    DROP CONSTRAINT connection_commands_kind_check,
    DROP CONSTRAINT connection_commands_check,
    ADD CONSTRAINT connection_commands_kind_check CHECK (kind IN (
        'register', 'unregister', 'test', 'pause', 'resume',
        'reload_config', 'opt_out', 'opt_in')),
    -- event_id is set only for opt_out and opt_in.
    ADD CONSTRAINT connection_commands_event_check CHECK (
        (kind IN ('opt_out', 'opt_in')) = (event_id IS NOT NULL)),
    -- A command still pending at this time is failed instead of run, so that
    -- nothing waits on a worker that is not there.
    ADD COLUMN expires_at timestamptz NOT NULL DEFAULT now() + interval '2 minutes',
    -- Structured result of a succeeded command; see the README for the shape
    -- per kind.
    ADD COLUMN result jsonb;

-- ---------------------------------------------------------------------------
-- Events: manual events

ALTER TABLE events ADD COLUMN created_by text;

COMMENT ON COLUMN events.created_by IS
    'Who created a manual event. Null for program events.';
COMMENT ON COLUMN events.test_event IS
    'For program events, the program''s test flag. Manual rows may set it to mark an operator''s test event.';
COMMENT ON COLUMN events.program_status IS
    'What the program last said, not a clock: it does not advance with time. Use start_at and duration_seconds for timing.';
COMMENT ON COLUMN events.opt IS
    'Written by the worker for program events, through the opt_out and opt_in commands. For manual events, ecosuite updates opt and opt_source itself.';
COMMENT ON COLUMN events.signals IS
    '[{"name", "type", "id", "intervals": [{"offset_seconds", "duration_seconds", "value"}]}]. Intervals are contiguous from the event start. SIMPLE values are levels 0 to 3, 0 meaning normal operation. See the README.';
COMMENT ON COLUMN events.duration_seconds IS
    'Zero means open-ended.';

-- Program rows belong to the worker. A client may insert, change and delete
-- manual rows, and change only lifecycle on program rows.
CREATE FUNCTION events_guard() RETURNS trigger
LANGUAGE plpgsql AS $$
DECLARE
    worker boolean := pg_has_role(
        current_user, (SELECT relowner FROM pg_class WHERE oid = TG_RELID), 'USAGE');
BEGIN
    IF NOT worker THEN
        IF TG_OP = 'INSERT' THEN
            IF NEW.source <> 'manual' THEN
                RAISE EXCEPTION 'only the worker inserts program events'
                    USING ERRCODE = 'insufficient_privilege';
            END IF;
        ELSIF TG_OP = 'DELETE' THEN
            IF OLD.source <> 'manual' THEN
                RAISE EXCEPTION 'event % is a program event; only manual events can be deleted', OLD.id
                    USING ERRCODE = 'insufficient_privilege';
            END IF;
        ELSIF (OLD.source <> 'manual' OR NEW.source <> 'manual')
              AND to_jsonb(NEW) - 'lifecycle' - 'updated_at'
                  IS DISTINCT FROM to_jsonb(OLD) - 'lifecycle' - 'updated_at' THEN
            RAISE EXCEPTION 'event % is a program event; only its lifecycle can be changed', OLD.id
                USING ERRCODE = 'insufficient_privilege';
        END IF;
    END IF;

    IF TG_OP = 'DELETE' THEN
        RETURN OLD;
    END IF;
    IF TG_OP = 'UPDATE' AND NEW IS DISTINCT FROM OLD THEN
        NEW.updated_at := now();
    END IF;
    RETURN NEW;
END;
$$;

CREATE TRIGGER events_guard
    BEFORE INSERT OR UPDATE OR DELETE ON events
    FOR EACH ROW EXECUTE FUNCTION events_guard();

-- ---------------------------------------------------------------------------
-- Events: one state per event
--
-- Three columns describe an event, with three writers: program_status (the
-- worker, from the program), opt (the worker or an operator) and lifecycle
-- (the planner). state resolves them in this order:
--
--   1. cancelled by the program  'cancelled'
--   2. opted out                 'opted_out'
--   3. otherwise                 lifecycle
--
-- so an event reads as cancelled or opted out as soon as that is known,
-- before the planner has caught up.

CREATE VIEW event_state AS
SELECT e.*,
       CASE
           WHEN e.program_status = 'cancelled' THEN 'cancelled'
           WHEN e.opt = 'out' THEN 'opted_out'
           ELSE e.lifecycle
       END AS state,
       -- Null for an open-ended event. The program's randomization window,
       -- start_after_seconds, is not included.
       CASE
           WHEN e.duration_seconds > 0
               THEN e.start_at + make_interval(secs => e.duration_seconds)
       END AS end_at
FROM events e;

-- ---------------------------------------------------------------------------
-- Connections: one health value per connection
--
-- The first of these that applies:
--
--   disabled        not enabled, or archived
--   worker_down     no worker has reported for the connection for two
--                   minutes, nor has its configuration changed in that time
--   invalid_config  the configuration cannot be used; see config_error
--   error           a permanent failure, such as rejected credentials;
--                   polling has stopped
--   unregistered    not registered with the program
--   degraded        registered, but the last attempts to reach the program
--                   failed; the worker is retrying
--   healthy         registered and in contact
--
-- config_pending is true while a saved configuration change has not reached
-- the worker yet.

CREATE VIEW connection_health AS
SELECT c.id AS connection_id,
       c.site_code,
       c.site_id,
       c.protocol,
       c.enabled,
       c.archived_at,
       CASE
           WHEN c.archived_at IS NOT NULL OR NOT c.enabled THEN 'disabled'
           WHEN GREATEST(s.heartbeat_at, c.updated_at) < now() - interval '2 minutes'
               THEN 'worker_down'
           WHEN s.config_error IS NOT NULL THEN 'invalid_config'
           WHEN s.registration = 'error' THEN 'error'
           WHEN s.registration IS DISTINCT FROM 'registered' THEN 'unregistered'
           WHEN s.consecutive_failures > 0 THEN 'degraded'
           ELSE 'healthy'
       END AS health,
       s.applied_config_version IS DISTINCT FROM c.config_version AS config_pending,
       COALESCE(s.registration, 'unregistered') AS registration,
       COALESCE(s.participation, 'active') AS participation,
       s.ven_id,
       s.config_error,
       s.last_error,
       s.last_error_at,
       COALESCE(s.consecutive_failures, 0) AS consecutive_failures,
       s.next_retry_at,
       s.last_poll_at,
       s.last_contact_at,
       s.heartbeat_at,
       s.worker_id
FROM connections c
LEFT JOIN connection_state s ON s.connection_id = c.id;

-- ---------------------------------------------------------------------------
-- Activity: the shape of the raw exchange
--
-- Connection entries carry the messages exchanged with the program:
--
--   {"exchange": [{"direction": "to_program" | "from_program",
--                  "at": "2026-09-30T14:00:00.000Z",
--                  "format": "xml",
--                  "body": "..."}]}
--
-- Entries written before this migration held the body under "xml" and had
-- no time, so they get their entry's.

COMMENT ON COLUMN activity.detail IS
    'Connection entries carry the raw exchange: {"exchange": [{"direction": "to_program" | "from_program", "at", "format", "body"}]}. See the README.';
COMMENT ON COLUMN activity.category IS
    'The worker writes connection and events entries. Ecosuite writes instructions and configuration entries, and events entries for operator actions on manual events.';

UPDATE activity a
SET detail = jsonb_set(a.detail, '{exchange}', (
        SELECT jsonb_agg(
                   CASE
                       WHEN message ? 'xml' THEN jsonb_build_object(
                           'direction', message -> 'direction',
                           'at', to_char(a.created_at AT TIME ZONE 'UTC',
                                         'YYYY-MM-DD"T"HH24:MI:SS.MS"Z"'),
                           'format', 'xml',
                           'body', message -> 'xml')
                       ELSE message
                   END ORDER BY position)
        FROM jsonb_array_elements(a.detail -> 'exchange')
            WITH ORDINALITY AS messages (message, position)))
WHERE jsonb_typeof(a.detail -> 'exchange') = 'array'
  AND jsonb_array_length(a.detail -> 'exchange') > 0;

-- ---------------------------------------------------------------------------
-- Change signals
--
-- grid_services_events           payload: event id. An event was inserted or
--                                deleted, or anything but its lifecycle
--                                changed. A listener that finds no row for
--                                the id knows the event was deleted.
-- grid_services_command_results  payload: command id. A command succeeded or
--                                failed.

CREATE FUNCTION events_notify() RETURNS trigger
LANGUAGE plpgsql AS $$
BEGIN
    -- The planner writes lifecycle, and needs no signal for its own changes.
    IF TG_OP = 'UPDATE'
       AND to_jsonb(NEW) - 'lifecycle' - 'updated_at'
           IS NOT DISTINCT FROM to_jsonb(OLD) - 'lifecycle' - 'updated_at' THEN
        RETURN NULL;
    END IF;
    PERFORM pg_notify('grid_services_events', COALESCE(NEW.id, OLD.id)::text);
    RETURN NULL;
END;
$$;

CREATE TRIGGER events_notify
    AFTER INSERT OR UPDATE OR DELETE ON events
    FOR EACH ROW EXECUTE FUNCTION events_notify();

CREATE FUNCTION connection_commands_notify_result() RETURNS trigger
LANGUAGE plpgsql AS $$
BEGIN
    PERFORM pg_notify('grid_services_command_results', NEW.id::text);
    RETURN NULL;
END;
$$;

CREATE TRIGGER connection_commands_notify_result
    AFTER UPDATE OF status ON connection_commands
    FOR EACH ROW
    WHEN (NEW.status IN ('succeeded', 'failed') AND NEW.status IS DISTINCT FROM OLD.status)
    EXECUTE FUNCTION connection_commands_notify_result();

-- migrate:down

DROP TRIGGER IF EXISTS connection_commands_notify_result ON connection_commands;
DROP FUNCTION IF EXISTS connection_commands_notify_result();
DROP TRIGGER IF EXISTS events_notify ON events;
DROP FUNCTION IF EXISTS events_notify();

UPDATE activity a
SET detail = jsonb_set(a.detail, '{exchange}', (
        SELECT jsonb_agg(
                   CASE
                       WHEN message ->> 'format' = 'xml' THEN jsonb_build_object(
                           'direction', message -> 'direction',
                           'xml', message -> 'body')
                       ELSE message
                   END ORDER BY position)
        FROM jsonb_array_elements(a.detail -> 'exchange')
            WITH ORDINALITY AS messages (message, position)))
WHERE jsonb_typeof(a.detail -> 'exchange') = 'array'
  AND jsonb_array_length(a.detail -> 'exchange') > 0;

COMMENT ON COLUMN activity.category IS NULL;
COMMENT ON COLUMN activity.detail IS NULL;

DROP VIEW IF EXISTS connection_health;
DROP VIEW IF EXISTS event_state;

DROP TRIGGER IF EXISTS events_guard ON events;
DROP FUNCTION IF EXISTS events_guard();

COMMENT ON COLUMN events.duration_seconds IS NULL;
COMMENT ON COLUMN events.signals IS NULL;
COMMENT ON COLUMN events.opt IS NULL;
COMMENT ON COLUMN events.program_status IS NULL;
COMMENT ON COLUMN events.test_event IS NULL;

ALTER TABLE events DROP COLUMN created_by;

DELETE FROM connection_commands WHERE kind = 'opt_in';

ALTER TABLE connection_commands
    DROP COLUMN result,
    DROP COLUMN expires_at,
    DROP CONSTRAINT connection_commands_event_check,
    DROP CONSTRAINT connection_commands_kind_check,
    ADD CONSTRAINT connection_commands_kind_check CHECK (kind IN (
        'register', 'unregister', 'test', 'pause', 'resume',
        'reload_config', 'opt_out')),
    ADD CONSTRAINT connection_commands_check CHECK (
        (kind = 'opt_out') = (event_id IS NOT NULL));

ALTER TABLE connection_state
    DROP COLUMN config_error,
    DROP COLUMN next_retry_at,
    DROP COLUMN consecutive_failures;

DROP TRIGGER IF EXISTS openadr2b_connections_keep_program ON openadr2b_connections;
DROP FUNCTION IF EXISTS openadr2b_connections_keep_program();

CREATE OR REPLACE FUNCTION connections_bump_config_version() RETURNS trigger
LANGUAGE plpgsql AS $$
BEGIN
    IF ROW(NEW.site_code, NEW.protocol, NEW.enabled, NEW.default_opt, NEW.poll_interval_seconds)
       IS DISTINCT FROM
       ROW(OLD.site_code, OLD.protocol, OLD.enabled, OLD.default_opt, OLD.poll_interval_seconds)
       OR NEW.updated_at IS DISTINCT FROM OLD.updated_at THEN
        NEW.config_version := OLD.config_version + 1;
        NEW.updated_at := now();
    END IF;
    RETURN NEW;
END;
$$;

COMMENT ON COLUMN connections.enabled IS NULL;

ALTER TABLE connections
    DROP COLUMN accepted_signals,
    DROP COLUMN archived_at;

COMMENT ON COLUMN activity.site_code IS NULL;
COMMENT ON COLUMN events.site_code IS NULL;
COMMENT ON COLUMN connections.site_code IS NULL;

ALTER TABLE activity DROP COLUMN site_id;
ALTER TABLE events DROP COLUMN site_id;
ALTER TABLE connections DROP COLUMN site_id;
