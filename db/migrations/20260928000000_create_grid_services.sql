-- migrate:up
-- Grid services: initial schema.
--
-- The database is the only channel between the API and the worker.
--
--   API     writes connections, protocol settings, commands, manual events
--   worker  writes connection_state, program events, event_responses,
--           activity, and command results
--
-- Each table has one writer per column group, noted on the table. The
-- planner that turns events into instructions is not part of this schema.

-- ---------------------------------------------------------------------------
-- Connections (written by the API)

CREATE TABLE connections (
    id                    bigserial   PRIMARY KEY,
    site_code             text        NOT NULL,
    protocol              text        NOT NULL CHECK (protocol IN ('openadr2b')),
    enabled               boolean     NOT NULL DEFAULT true,
    -- Opt answer for newly received events while participation is active.
    default_opt           text        NOT NULL DEFAULT 'in' CHECK (default_opt IN ('in', 'out')),
    -- Overrides the poll interval the program requests.
    poll_interval_seconds integer     CHECK (poll_interval_seconds > 0),
    -- Bumped by trigger on any configuration change. The worker restarts a
    -- connection whose version differs from the one it is running.
    config_version        integer     NOT NULL DEFAULT 1,
    created_at            timestamptz NOT NULL DEFAULT now(),
    updated_at            timestamptz NOT NULL DEFAULT now()
);

CREATE INDEX connections_site_code ON connections (site_code);

-- OpenADR 2.0b settings, one row per openadr2b connection.
CREATE TABLE openadr2b_connections (
    connection_id             bigint      PRIMARY KEY REFERENCES connections (id) ON DELETE CASCADE,
    vtn_url                   text        NOT NULL,
    ven_name                  text        NOT NULL,
    -- Provisioned by the VTN operator. Null when the VTN assigns it on registration.
    ven_id                    text,
    market_context            text,
    client_cert_pem           text,
    -- Client private key, sealed with the worker's public key
    -- (libsodium crypto_box_seal), base64 encoded. Only the worker can open it.
    client_key_sealed         text,
    ca_cert_pem               text,
    verify_server_certificate boolean     NOT NULL DEFAULT true,
    -- Derived by the API on upload, for display and readiness checks.
    cert_fingerprint          text,
    cert_expires_at           timestamptz
);

-- ---------------------------------------------------------------------------
-- Commands (rows inserted by the API, status columns written by the worker)

CREATE TABLE connection_commands (
    id            bigserial   PRIMARY KEY,
    connection_id bigint      NOT NULL REFERENCES connections (id) ON DELETE CASCADE,
    kind          text        NOT NULL CHECK (kind IN (
                      'register', 'unregister', 'test', 'pause', 'resume',
                      'reload_config', 'opt_out')),
    -- Set only for opt_out.
    event_id      bigint,
    requested_by  text,
    status        text        NOT NULL DEFAULT 'pending'
                      CHECK (status IN ('pending', 'running', 'succeeded', 'failed')),
    message       text,
    created_at    timestamptz NOT NULL DEFAULT now(),
    claimed_at    timestamptz,
    completed_at  timestamptz,
    CHECK ((kind = 'opt_out') = (event_id IS NOT NULL))
);

CREATE INDEX connection_commands_pending
    ON connection_commands (connection_id, id) WHERE status = 'pending';

-- ---------------------------------------------------------------------------
-- Connection state (written by the worker)

CREATE TABLE connection_state (
    connection_id         bigint      PRIMARY KEY REFERENCES connections (id) ON DELETE CASCADE,
    registration          text        NOT NULL DEFAULT 'unregistered'
                              CHECK (registration IN ('unregistered', 'registered', 'error')),
    participation         text        NOT NULL DEFAULT 'active'
                              CHECK (participation IN ('active', 'paused')),
    -- Identity as confirmed by the program on registration.
    ven_id                text,
    registration_id       text,
    -- Poll interval the program requested on registration.
    program_poll_interval_seconds integer,
    last_poll_at          timestamptz,
    last_contact_at       timestamptz,
    last_error            text,
    last_error_at         timestamptz,
    worker_id             text,
    heartbeat_at          timestamptz,
    applied_config_version integer
);

-- ---------------------------------------------------------------------------
-- Events
--
-- One row per event. Program events are keyed by connection and the
-- program's event id; the modification number orders updates.
--
-- The worker owns the program columns for source = 'program'. The planner
-- owns lifecycle; the worker never writes it.

CREATE TABLE events (
    id                  bigserial   PRIMARY KEY,
    site_code           text        NOT NULL,
    source              text        NOT NULL CHECK (source IN ('program', 'manual')),
    connection_id       bigint      REFERENCES connections (id),
    program_event_id    text,
    modification_number integer     NOT NULL DEFAULT 0,
    program_status      text        NOT NULL DEFAULT 'pending'
                            CHECK (program_status IN ('pending', 'active', 'completed', 'cancelled')),
    start_at            timestamptz NOT NULL,
    -- Zero means open-ended.
    duration_seconds    integer     NOT NULL CHECK (duration_seconds >= 0),
    -- The program's randomization window (OpenADR startafter): the event
    -- starts somewhere between start_at and start_at plus this. The planner
    -- picks the actual start.
    start_after_seconds integer     CHECK (start_after_seconds >= 0),
    -- [{"name", "type", "id", "intervals": [{"offset_seconds",
    --   "duration_seconds", "value"}]}]
    signals             jsonb       NOT NULL DEFAULT '[]',
    market_context      text,
    test_event          boolean     NOT NULL DEFAULT false,
    response_required   boolean     NOT NULL DEFAULT true,
    program_comment     text,
    -- The program's request id from the latest delivery of this event. An
    -- opt change sent later refers back to it.
    program_request_id  text,
    opt                 text        NOT NULL DEFAULT 'in' CHECK (opt IN ('in', 'out')),
    opt_source          text        NOT NULL DEFAULT 'default'
                            CHECK (opt_source IN ('default', 'paused', 'operator')),
    lifecycle           text        NOT NULL DEFAULT 'received' CHECK (lifecycle IN (
                            'received', 'planned', 'prep', 'control', 'completed',
                            'opted_out', 'cancelled', 'failed')),
    received_at         timestamptz NOT NULL DEFAULT now(),
    updated_at          timestamptz NOT NULL DEFAULT now(),
    CHECK ((source = 'program') = (connection_id IS NOT NULL AND program_event_id IS NOT NULL))
);

CREATE UNIQUE INDEX events_program_key
    ON events (connection_id, program_event_id) WHERE source = 'program';
CREATE INDEX events_site_start ON events (site_code, start_at);

ALTER TABLE connection_commands
    ADD FOREIGN KEY (event_id) REFERENCES events (id) ON DELETE CASCADE;

-- Opt answers sent to the program (written by the worker).
CREATE TABLE event_responses (
    id                  bigserial   PRIMARY KEY,
    event_id            bigint      NOT NULL REFERENCES events (id) ON DELETE CASCADE,
    opt                 text        NOT NULL CHECK (opt IN ('in', 'out')),
    modification_number integer     NOT NULL,
    status              text        NOT NULL CHECK (status IN ('sent', 'failed')),
    message             text,
    created_at          timestamptz NOT NULL DEFAULT now()
);

CREATE INDEX event_responses_event ON event_responses (event_id, id);

-- ---------------------------------------------------------------------------
-- Activity: the operator-facing log and audit trail.

CREATE TABLE activity (
    id            bigserial   PRIMARY KEY,
    site_code     text        NOT NULL,
    connection_id bigint      REFERENCES connections (id) ON DELETE SET NULL,
    event_id      bigint      REFERENCES events (id) ON DELETE SET NULL,
    category      text        NOT NULL
                      CHECK (category IN ('connection', 'events', 'instructions', 'configuration')),
    actor         text        NOT NULL DEFAULT 'system',
    message       text        NOT NULL,
    -- Connection-level entries carry the raw protocol exchange here.
    detail        jsonb,
    created_at    timestamptz NOT NULL DEFAULT now()
);

CREATE INDEX activity_site_time ON activity (site_code, created_at DESC);
CREATE INDEX activity_event ON activity (event_id) WHERE event_id IS NOT NULL;

-- ---------------------------------------------------------------------------
-- Change signals
--
-- grid_services_config    payload: connection id. Any configuration change.
-- grid_services_commands  payload: connection id. A command was inserted.

CREATE FUNCTION connections_bump_config_version() RETURNS trigger
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

CREATE TRIGGER connections_bump_config_version
    BEFORE UPDATE ON connections
    FOR EACH ROW EXECUTE FUNCTION connections_bump_config_version();

CREATE FUNCTION connections_notify_config() RETURNS trigger
LANGUAGE plpgsql AS $$
BEGIN
    PERFORM pg_notify('grid_services_config', COALESCE(NEW.id, OLD.id)::text);
    RETURN NULL;
END;
$$;

CREATE TRIGGER connections_notify_config
    AFTER INSERT OR UPDATE OR DELETE ON connections
    FOR EACH ROW EXECUTE FUNCTION connections_notify_config();

-- A change to protocol settings is a change to the connection.
CREATE FUNCTION protocol_settings_touch_connection() RETURNS trigger
LANGUAGE plpgsql AS $$
BEGIN
    UPDATE connections SET updated_at = clock_timestamp()
    WHERE id = COALESCE(NEW.connection_id, OLD.connection_id);
    RETURN NULL;
END;
$$;

CREATE TRIGGER openadr2b_connections_touch_connection
    AFTER INSERT OR UPDATE OR DELETE ON openadr2b_connections
    FOR EACH ROW EXECUTE FUNCTION protocol_settings_touch_connection();

CREATE FUNCTION connection_commands_notify() RETURNS trigger
LANGUAGE plpgsql AS $$
BEGIN
    PERFORM pg_notify('grid_services_commands', NEW.connection_id::text);
    RETURN NULL;
END;
$$;

CREATE TRIGGER connection_commands_notify
    AFTER INSERT ON connection_commands
    FOR EACH ROW EXECUTE FUNCTION connection_commands_notify();

-- migrate:down

DROP TABLE IF EXISTS
    activity,
    event_responses,
    connection_commands,
    events,
    connection_state,
    openadr2b_connections,
    connections
    CASCADE;

DROP FUNCTION IF EXISTS connection_commands_notify();
DROP FUNCTION IF EXISTS protocol_settings_touch_connection();
DROP FUNCTION IF EXISTS connections_notify_config();
DROP FUNCTION IF EXISTS connections_bump_config_version();
