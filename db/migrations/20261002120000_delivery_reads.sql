-- migrate:up
-- The datum reads behind a delivered report, so the values it carried can
-- be audited by making the same requests again. The datums themselves are
-- not kept: the request is the record, and SolarNetwork answers it afresh.

ALTER TABLE report_deliveries
    ADD COLUMN reads jsonb NOT NULL DEFAULT '[]'
        CONSTRAINT report_deliveries_reads_check CHECK (jsonb_typeof(reads) = 'array');

COMMENT ON COLUMN report_deliveries.reads IS
    'One entry per stream read for the report: [{"node_id", "source_id", "kind": "list" | "most_recent", "path", "query", "datum_count", "error"}]. Each payload value names its stream and reduction under "source". See the README.';

-- migrate:down

ALTER TABLE report_deliveries DROP COLUMN IF EXISTS reads;
