-- migrate:up
-- CSIP-AUS: DERSettings' time changes only with the DER's settings.
--
-- csipaus_der.updated_at is sent as DERSettings' updatedTime. Its state
-- bindings are not settings: renaming or deleting a bound point cascades
-- into the row, and leaves updated_at as it was.

CREATE FUNCTION csipaus_der_touch_updated_at() RETURNS trigger
LANGUAGE plpgsql AS $$
BEGIN
    IF to_jsonb(NEW) - ARRAY['connected_point_id', 'operating_point_id', 'updated_at']
       IS DISTINCT FROM
       to_jsonb(OLD) - ARRAY['connected_point_id', 'operating_point_id', 'updated_at'] THEN
        NEW.updated_at := now();
    END IF;
    RETURN NEW;
END;
$$;

DROP TRIGGER csipaus_der_updated_at ON csipaus_der;
CREATE TRIGGER csipaus_der_updated_at
    BEFORE UPDATE ON csipaus_der
    FOR EACH ROW EXECUTE FUNCTION csipaus_der_touch_updated_at();

COMMENT ON COLUMN csipaus_der.updated_at IS
    'Sent as DERSettings'' updatedTime. Set when anything but the state bindings changes.';

-- migrate:down

COMMENT ON COLUMN csipaus_der.updated_at IS
    'Sent as DERSettings'' updatedTime.';
DROP TRIGGER IF EXISTS csipaus_der_updated_at ON csipaus_der;
CREATE TRIGGER csipaus_der_updated_at
    BEFORE UPDATE ON csipaus_der
    FOR EACH ROW EXECUTE FUNCTION touch_updated_at();
DROP FUNCTION IF EXISTS csipaus_der_touch_updated_at();
