-- The ecosuite_api role, which the site simulator logs in as: the grants of
-- the root README, "Deployment" step 2, exactly. Change them together; a
-- grant the simulator needs beyond these is a contract change.

CREATE ROLE ecosuite_api LOGIN PASSWORD 'ecosuite_api';
GRANT USAGE ON SCHEMA public TO ecosuite_api;
GRANT SELECT, INSERT, UPDATE, DELETE ON connections, client_credentials, openadr2b_connections,
    csipaus_connections, telemetry_points, openadr2b_report_offers, connection_data_credentials,
    csipaus_der, csipaus_readings, events TO ecosuite_api;
GRANT SELECT, INSERT ON connection_commands, activity TO ecosuite_api;
GRANT SELECT ON connection_state, connection_health, event_state, event_responses,
    csipaus_events, csipaus_limits, openadr2b_report_requests, report_deliveries
    TO ecosuite_api;
GRANT USAGE ON ALL SEQUENCES IN SCHEMA public TO ecosuite_api;
