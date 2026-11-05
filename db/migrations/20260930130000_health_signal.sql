-- migrate:up
-- A signal for changes in connection health, so that the notifier need not
-- poll connection_health.
--
-- grid_services_health  payload: connection id. Something connection_health
--                       reports for the connection may have changed: its
--                       registration, its participation, its configuration
--                       error, whether it is failing to reach the program,
--                       whether a configuration change is pending, or the
--                       connection itself.
--
-- Counting further failures, polls and heartbeats are not signalled. Nor is
-- worker_down: it is time passing, not a change to a row, so a listener
-- still checks for it on a timer.

CREATE FUNCTION connection_state_notify_health() RETURNS trigger
LANGUAGE plpgsql AS $$
BEGIN
    IF TG_OP = 'UPDATE'
       AND ROW(NEW.registration, NEW.participation, NEW.config_error,
               NEW.consecutive_failures > 0, NEW.applied_config_version)
           IS NOT DISTINCT FROM
           ROW(OLD.registration, OLD.participation, OLD.config_error,
               OLD.consecutive_failures > 0, OLD.applied_config_version) THEN
        RETURN NULL;
    END IF;
    PERFORM pg_notify('grid_services_health', NEW.connection_id::text);
    RETURN NULL;
END;
$$;

CREATE TRIGGER connection_state_notify_health
    AFTER INSERT OR UPDATE ON connection_state
    FOR EACH ROW EXECUTE FUNCTION connection_state_notify_health();

-- Any change to a connection bumps config_version, which makes the change
-- pending, and enabled and archived_at decide whether it is disabled.
CREATE FUNCTION connections_notify_health() RETURNS trigger
LANGUAGE plpgsql AS $$
BEGIN
    IF TG_OP = 'UPDATE'
       AND NEW.config_version IS NOT DISTINCT FROM OLD.config_version THEN
        RETURN NULL;
    END IF;
    PERFORM pg_notify('grid_services_health', NEW.id::text);
    RETURN NULL;
END;
$$;

CREATE TRIGGER connections_notify_health
    AFTER INSERT OR UPDATE ON connections
    FOR EACH ROW EXECUTE FUNCTION connections_notify_health();

-- migrate:down

DROP TRIGGER IF EXISTS connections_notify_health ON connections;
DROP FUNCTION IF EXISTS connections_notify_health();
DROP TRIGGER IF EXISTS connection_state_notify_health ON connection_state;
DROP FUNCTION IF EXISTS connection_state_notify_health();
