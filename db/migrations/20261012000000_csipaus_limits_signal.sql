-- migrate:up
-- A signal for changes in the caps in force, so that ecosuite's planner
-- acts on them at once rather than on its next sweep.
--
-- grid_services_limits  payload: connection id. Its csipaus_limits rows
--                       changed. The worker replaces them in one
--                       transaction, and Postgres sends the transaction's
--                       identical notifications once.

CREATE FUNCTION csipaus_limits_notify() RETURNS trigger
LANGUAGE plpgsql AS $$
BEGIN
    PERFORM pg_notify('grid_services_limits',
                      COALESCE(NEW.connection_id, OLD.connection_id)::text);
    RETURN NULL;
END;
$$;

CREATE TRIGGER csipaus_limits_notify
    AFTER INSERT OR UPDATE OR DELETE ON csipaus_limits
    FOR EACH ROW EXECUTE FUNCTION csipaus_limits_notify();

-- migrate:down

DROP TRIGGER IF EXISTS csipaus_limits_notify ON csipaus_limits;
DROP FUNCTION IF EXISTS csipaus_limits_notify();
