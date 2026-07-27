# Handoff prompt — HTTP status contract for the ingest endpoint

**Paste this when opening the `FieldMeshDashboard` repo.**

---

## The problem

A deployed FieldHub whose connection key has been revoked will retry its upload **forever**.

The ingest endpoint returns **403** for an invalid, revoked, or MAC-mismatched credential
(`ingest-fieldmesh/index.ts:59-64`). Until today the hub firmware treated only **400** and **401** as
non-retryable; everything else fell into the retryable branch, incremented a retry counter, and tried again on the
next scheduled wake. A revoked key therefore produced an endless retry loop — burning cellular data and battery on
a credential that can never succeed, on hardware that is typically solar-powered and unattended.

This was found during manuscript verification. Nothing was changed in this repository.

## What has already been fixed (firmware side)

`Data-Logger-network---ESP32` @ `mothership/firmware/v2/src/main.cpp` now classifies statuses with a shared helper
rather than an enumerated pair:

```cpp
static bool isNonRetryableHttpStatus(int status) {
  if (status < 400 || status >= 500) return false;  // 2xx/3xx/5xx/transport(-1) → retry
  if (status == 408 || status == 429) return false; // transient by definition → retry
  return true;                                      // every other 4xx → stop
}
```

Applied at all three upload sites (JSON POST, CSV fallback, status heartbeat). Builds clean. **403 now correctly
stops the retry loop.**

## Why this still needs a backend decision

The firmware fix only protects hubs **running the new firmware**. Every hub already in the field runs the old
build, and node/hub firmware update to deployed hardware is precisely the capability that is not yet reliable —
so we cannot assume the fleet gets patched.

A backend change would fix the existing fleet immediately, without touching a single device.

## What I'd like you to consider

**Return 401 rather than 403 for credential failures** — invalid key, revoked key, key not matching the registered
FieldHub MAC.

Two arguments for it:

1. **It fixes every hub, including un-updated ones.** Old firmware already treats 401 as non-retryable. This is the
   only change that protects hardware currently in the field.
2. **It is arguably more correct.** 401 means *the credential did not authenticate*; 403 means *you authenticated,
   but you may not do this*. A revoked or unrecognised bearer key is an authentication failure, not an authorisation
   one. Keep 403 for the case where a valid key genuinely lacks permission on a specific resource — if that case
   exists at all on this endpoint.

If you disagree and want to keep 403, that is defensible — just say so, and we will treat the firmware fix as the
sole remedy and accept that pre-update hubs stay vulnerable until they are reflashed by hand.

## Constraint this creates on the endpoint going forward

Now that the hub treats **all 4xx except 408 and 429** as terminal, the ingest endpoint must never use a 4xx for a
condition that is actually transient. If you ever need to tell a hub "not now, come back later" — maintenance, a
migration in progress, a warming cache — it must be **503** (or 429 with backoff), never 409 or 423. A 4xx will
stop that hub uploading until someone visits the site.

## What I need back

1. Your decision on 401-vs-403, with reasoning if you keep 403.
2. If you change it: the diff, plus a regression test asserting the status code for each of invalid key, revoked
   key, and MAC mismatch — this is exactly the kind of contract that silently drifts.
3. A short list of **every** non-2xx status this endpoint can return, and whether each is transient or terminal. I
   want to check it against the firmware classifier rather than assume the two agree. Right now the agreement is
   inferred, not verified — which is how this bug happened in the first place.
4. Confirmation that no current or planned response uses a 4xx to mean "retry later".

## Constraints

- Do not change any device-facing payload shape — only the status code, and only if you agree with the reasoning.
- Do not open-source or publish this repository (see the earlier verification handoff).
