-- migrate:up
-- CSIP-AUS: the ramp into each of the caps in force.
--
-- The worker resolves each row's ramp as the CSIP-AUS v1.2 explainer
-- (section 6.1) says, so that ecosuite applies no CSIP-AUS ramp rules: a
-- control's rampTms, else the setGradW of the default in force. A row with
-- neither has no ramp. A ramp matters when a cap loosens; a site may meet a
-- tighter cap at once. Rows written before this have neither until the
-- worker's next walk replaces them.

ALTER TABLE csipaus_limits
    ADD COLUMN ramp_seconds numeric(5, 2)
        CHECK (ramp_seconds BETWEEN 0 AND 655.35),
    ADD COLUMN set_grad_w   integer
        CHECK (set_grad_w BETWEEN 0 AND 65535),
    ADD CONSTRAINT csipaus_limits_ramp_check CHECK (
        (ramp_seconds IS NULL OR set_grad_w IS NULL)
        AND (ramp_seconds IS NULL OR source = 'control'));

COMMENT ON COLUMN csipaus_limits.ramp_seconds IS
    'How long the move to this row''s value takes: the control''s rampTms, in seconds. Only on a control''s rows, and never with set_grad_w.';
COMMENT ON COLUMN csipaus_limits.set_grad_w IS
    'The rate of the move to this row''s value, in hundredths of a percent of setMaxW a second (0 is no limit): the setGradW of the default in force, on a default''s rows and on a control''s without rampTms.';

-- migrate:down

ALTER TABLE csipaus_limits
    DROP CONSTRAINT csipaus_limits_ramp_check,
    DROP COLUMN ramp_seconds,
    DROP COLUMN set_grad_w;
