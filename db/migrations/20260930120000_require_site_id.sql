-- migrate:up
-- Sites: site_id is required, and a site has one connection.
--
-- Ecosuite site codes are unique only within a project, so two projects can
-- each have a site with the same code and site_code cannot find a site.
-- site_id is the join key on every row. It is also all that keeps one
-- organization's rows apart from another's, since nothing here names an
-- organization. site_code is for display.

-- ---------------------------------------------------------------------------
-- Rows written before site_id existed
--
-- They belong to no ecosuite site that this database knows of. Each gets a
-- generated id so that it can be kept: a connection gets its own, which its
-- events and activity share. A row with no connection takes the id of the
-- oldest connection with its site code, or one generated for that code.
-- Ecosuite sets the real id on a connection it wants to keep.

UPDATE connections SET site_id = gen_random_uuid() WHERE site_id IS NULL;

UPDATE events e
SET site_id = c.site_id
FROM connections c
WHERE e.site_id IS NULL AND e.connection_id = c.id;

UPDATE activity a
SET site_id = c.site_id
FROM connections c
WHERE a.site_id IS NULL AND a.connection_id = c.id;

WITH codes AS MATERIALIZED (
    SELECT legacy.site_code,
           COALESCE((SELECT c.site_id FROM connections c
                     WHERE c.site_code = legacy.site_code
                     ORDER BY c.id LIMIT 1),
                    gen_random_uuid()) AS site_id
    FROM (SELECT site_code FROM events WHERE site_id IS NULL
          UNION
          SELECT site_code FROM activity WHERE site_id IS NULL) legacy
), events_filled AS (
    UPDATE events e
    SET site_id = codes.site_id
    FROM codes
    WHERE e.site_id IS NULL AND e.site_code = codes.site_code
)
UPDATE activity a
SET site_id = codes.site_id
FROM codes
WHERE a.site_id IS NULL AND a.site_code = codes.site_code;

-- ---------------------------------------------------------------------------
-- site_id is required

ALTER TABLE connections ALTER COLUMN site_id SET NOT NULL;
ALTER TABLE events ALTER COLUMN site_id SET NOT NULL;
ALTER TABLE activity ALTER COLUMN site_id SET NOT NULL;

COMMENT ON COLUMN connections.site_code IS
    'The ecosuite site code, not a project code. For display only: codes repeat across projects, so a site is never looked up by its code. Ecosuite updates it when the site''s code changes.';
COMMENT ON COLUMN connections.site_id IS
    'Ecosuite''s immutable site id (sites.id). The join key to ecosuite. A site has one connection that is not archived.';
COMMENT ON COLUMN events.site_code IS
    'The site''s code when the row was written, for display only. It does not follow a later change of code; join on site_id.';
COMMENT ON COLUMN activity.site_code IS
    'The site''s code when the row was written, for display only. It does not follow a later change of code; join on site_id.';

-- Nothing looks a site up by its code.
DROP INDEX connections_site_code;
DROP INDEX events_site_start;
DROP INDEX activity_site_time;

-- ---------------------------------------------------------------------------
-- One connection per site
--
-- A site that leaves one program and joins another archives its connection
-- and gets a new one, so its history stays under the same site_id.

DROP INDEX connections_site_id;

CREATE UNIQUE INDEX connections_site_id_key
    ON connections (site_id) WHERE archived_at IS NULL;

-- migrate:down

DROP INDEX connections_site_id_key;

CREATE INDEX connections_site_id ON connections (site_id);

CREATE INDEX connections_site_code ON connections (site_code);
CREATE INDEX events_site_start ON events (site_code, start_at);
CREATE INDEX activity_site_time ON activity (site_code, created_at DESC);

COMMENT ON COLUMN connections.site_code IS
    'The ecosuite site code, not a project code. For display; ecosuite updates it when the site''s code changes.';
COMMENT ON COLUMN connections.site_id IS
    'Ecosuite''s immutable site id (sites.id). The join key to ecosuite.';
COMMENT ON COLUMN events.site_code IS
    'The site''s code when the row was written. It does not follow a later change of code; join on site_id.';
COMMENT ON COLUMN activity.site_code IS
    'The site''s code when the row was written. It does not follow a later change of code; join on site_id.';

-- Generated ids stay: nothing tells them from ones ecosuite set.
ALTER TABLE activity ALTER COLUMN site_id DROP NOT NULL;
ALTER TABLE events ALTER COLUMN site_id DROP NOT NULL;
ALTER TABLE connections ALTER COLUMN site_id DROP NOT NULL;
