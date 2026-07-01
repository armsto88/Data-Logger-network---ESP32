# FieldMesh Firmware Integration Guide

## Supabase Backend Configuration

**Environment: Production**

### Ingestion Endpoint

```
POST https://unhzttnuayrgqrzeqetz.supabase.co/functions/v1/ingest-fieldmesh
```

### Authentication

Device credentials are issued per mothership. Each firmware node must include:

**Header:**
```
Authorization: Bearer <device-api-key>
```

**Device API Key Format:**
- Prefix: 6-20 character identifier (e.g., `dk_mothership_001_node_env_01`)
- Full key: issued via Device Ingestion API or provisioning flow
- Keys can be revoked; check response codes below for rejection handling

### Request Format

**Method:** POST  
**Content-Type:** application/json

**Payload (JSON):**
```json
{
  "mothership_id": "d7eaae7b-011e-4f57-a489-0fbaefb05ff0",
  "node_id": "ENV_6C0AA0",
  "seq_num": 1,
  "timestamp": "2026-06-29T12:34:56Z",
  "sensor_present": 311,
  "quality_flags": 0,
  "config_version": 1,
  "battery_voltage": 3.794,
  "air_temp": 30.394,
  "air_humidity": 42.322,
  "spectral_415": 0,
  "spectral_445": 1,
  "spectral_480": 1,
  "spectral_515": 2,
  "spectral_555": 2,
  "spectral_590": 2,
  "spectral_630": 2,
  "spectral_680": 3,
  "wind_speed": null,
  "wind_dir": null,
  "soil1_vwc": 3.96,
  "soil1_temp": 30.896,
  "soil2_vwc": 0.28,
  "soil2_temp": 30.368,
  "aux1": null,
  "aux2": null
}
```

**Required Fields:**
- `mothership_id` — UUID of the mothership
- `node_id` — Node identifier (string, 1-120 chars)
- `timestamp` — ISO 8601 UTC datetime (YYYY-MM-DDTHH:mm:ssZ)
- `battery_voltage` — Float (volts)
- `sensor_present` — Bitmask (integer)
- `quality_flags` — Bitmask (integer)

**Optional Fields:**
- All sensor readings (air_temp, humidity, soil moisture, spectral, wind, etc.) can be null
- `aux1`, `aux2` for custom sensors
- Additional custom fields are accepted and stored as extra_measurements

### HTTP Response Codes

| Code | Meaning | Action |
|------|---------|--------|
| 200 | Success | Data stored, continue normal operation |
| 401 | Unauthorized | Device API key invalid, expired, or revoked — request new credentials |
| 400 | Bad Request | Malformed payload (missing required fields, invalid format) — check JSON structure |
| 429 | Rate Limited | Too many requests — implement exponential backoff (max 60s) |
| 500 | Server Error | Supabase issue — retry with exponential backoff (max 60s) |

### Retry Strategy

**Recommended:**
- Initial retry: 2 seconds
- Max retries: 5
- Backoff multiplier: 1.5x (2s → 3s → 4.5s → 6.75s → 10s)
- Only retry on 429 and 5xx responses
- Log 401 responses immediately (credential issue)

### Device Credential Provisioning

Device API keys are issued via the Device Ingestion Edge Function (`issue-device-key`). Contact the backend team for:
- Initial key generation for this mothership
- Key rotation procedure
- Revocation/expiry management

### Mothership & Project Info

**Current Setup:**
- **Mothership ID:** `d7eaae7b-011e-4f57-a489-0fbaefb05ff0`
- **Project ID:** `f4efee37-e162-45df-8f9a-4945b760f292`
- **Supabase Project Ref:** `unhzttnuayrgqrzeqetz`

### Realtime Updates

Dashboard subscribers receive live updates via Supabase Realtime when readings are inserted. Latency: <100ms.

### Testing

Dry-run test endpoint (use with test credentials):
```
POST https://unhzttnuayrgqrzeqetz.supabase.co/functions/v1/ingest-fieldmesh?dry_run=true
```

Response includes validation without storing data.

### Questions or Issues

- Endpoint unreachable? Verify network routing to `unhzttnuayrgqrzeqetz.supabase.co`
- 401 errors? Check device API key validity and Authorization header format
- 400 errors? Validate JSON structure and required fields
- Timestamp parsing issues? Ensure ISO 8601 format (UTC) with Z suffix

---

**Prepared for:** FieldMesh Firmware Team  
**Date:** 2026-06-29  
**Backend:** Supabase (Phase 2)