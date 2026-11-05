-- migrate:up
-- CSIP-AUS: the power of ten power readings are sent at.
--
-- IEEE 2030.5 sends a reading as a 16-bit integer times a power of ten. A
-- mirror usage point declares each reading's power of ten once, when it is
-- posted, and a server may read every value it holds with the latest, so it
-- cannot follow the data: ecosuite sets it from the site's size. At 1,
-- values go in tens of W or var, up to ±327,670. A power reading written
-- without one gets 1. Voltage (-1) and frequency (-2) are fixed and have
-- none.

ALTER TABLE csipaus_readings
    ADD COLUMN power_of_ten smallint
        CONSTRAINT csipaus_readings_power_of_ten_range_check CHECK (power_of_ten BETWEEN -9 AND 9);

COMMENT ON COLUMN csipaus_readings.power_of_ten IS
    'The power of ten a real_power or reactive_power reading is sent at, -9 to 9: its powerOfTenMultiplier. 1 when written without one. Null for voltage and frequency, which are sent at -1 and -2.';

CREATE FUNCTION csipaus_readings_default_power_of_ten() RETURNS trigger
LANGUAGE plpgsql AS $$
BEGIN
    IF NEW.reading IN ('real_power', 'reactive_power') AND NEW.power_of_ten IS NULL THEN
        NEW.power_of_ten := 1;
    END IF;
    RETURN NEW;
END;
$$;

CREATE TRIGGER csipaus_readings_power_of_ten
    BEFORE INSERT OR UPDATE ON csipaus_readings
    FOR EACH ROW EXECUTE FUNCTION csipaus_readings_default_power_of_ten();

UPDATE csipaus_readings SET power_of_ten = 1
WHERE reading IN ('real_power', 'reactive_power');

ALTER TABLE csipaus_readings
    ADD CONSTRAINT csipaus_readings_power_of_ten_check
        CHECK ((power_of_ten IS NOT NULL) = (reading IN ('real_power', 'reactive_power')));

-- migrate:down

ALTER TABLE csipaus_readings DROP CONSTRAINT IF EXISTS csipaus_readings_power_of_ten_check;
DROP TRIGGER IF EXISTS csipaus_readings_power_of_ten ON csipaus_readings;
DROP FUNCTION IF EXISTS csipaus_readings_default_power_of_ten();
ALTER TABLE csipaus_readings DROP COLUMN IF EXISTS power_of_ten;
