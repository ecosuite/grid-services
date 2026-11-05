-- migrate:up
-- A random addition to each wait between polls, so connections polling on
-- the same interval do not reach the program at the same instant (OpenADR
-- 2.0b section 9.1.8).

ALTER TABLE connections
    ADD COLUMN poll_jitter double precision NOT NULL DEFAULT 0.1
        CONSTRAINT connections_poll_jitter_check CHECK (poll_jitter BETWEEN 0 AND 1);

COMMENT ON COLUMN connections.poll_jitter IS
    'Each wait between polls is lengthened by a random part of up to this fraction of the poll interval. 0 polls on the interval exactly. It never shortens the interval.';

-- migrate:down

ALTER TABLE connections DROP COLUMN IF EXISTS poll_jitter;
