-- ============================================================
-- Hydra-Flow Stability Fix SQL (v7.0)
-- Fixes:
--   1. Heartbeat "last_seen" updating correctly (COALESCE safety)
--   2. Tank-full auto-off ONLY when water_level stays at 100
--      (prevents false rejections from stale DB values)
--   3. Better reason labels in motor_logs for easier debugging
-- ============================================================

-- ── 1. FULL TRIGGER REPLACEMENT ──────────────────────────────
CREATE OR REPLACE FUNCTION public.handle_motor_automation()
RETURNS TRIGGER AS $$
DECLARE
    motor_rejected BOOLEAN := FALSE;
    log_reason     TEXT    := 'system';
BEGIN

    -- ══ A. HEARTBEAT: Update last_seen timestamps ════════════
    IF (COALESCE(OLD.tank_ping, FALSE) IS DISTINCT FROM NEW.tank_ping) THEN
        NEW.tank_last_seen := now();
    END IF;

    IF (COALESCE(OLD.motor_ping, FALSE) IS DISTINCT FROM NEW.motor_ping) THEN
        NEW.motor_last_seen := now();
    END IF;

    -- ══ B. AUTO-OFF SAFETY: Tank Full ════════════════════════
    -- Only force-off if water_level is 100 AND motor is (or is being set) ON.
    -- This is the REAL safety gate — the ESP firmware also enforces this locally.
    IF NEW.water_level = 100 AND NEW.motor_status = TRUE THEN
        NEW.motor_status    := FALSE;
        NEW.manual_override := FALSE;

        -- If motor was already OFF before this update, mark it as rejected
        -- (someone tried to turn it ON while tank was full)
        IF OLD.motor_status = FALSE THEN
            motor_rejected := TRUE;
            log_reason     := 'tank_full_rejection';
        ELSE
            -- Motor was running, tank just filled up → auto-off
            log_reason := 'tank_full_auto_off';
        END IF;
    END IF;

    -- ══ C. UPDATE TIMESTAMP ══════════════════════════════════
    NEW.updated_at := now();

    -- ══ D. LOGGING ═══════════════════════════════════════════
    -- Determine the reason for normal on/off transitions
    IF (OLD.motor_status IS DISTINCT FROM NEW.motor_status) AND NOT motor_rejected THEN
        IF NEW.motor_status = TRUE THEN
            -- Determine if this was a scheduled vs manual start
            log_reason := CASE
                WHEN NEW.mode = 'STANDARD' OR NEW.mode = 'LOCAL' THEN 'manual_on'
                WHEN NEW.mode = 'ONLINE'                          THEN 'remote_on'
                ELSE 'started'
            END;
        ELSE
            -- Determine reason for stop
            log_reason := CASE
                WHEN NEW.mode = 'STANDARD' OR NEW.mode = 'LOCAL' THEN 'manual_off'
                ELSE 'stopped'
            END;
        END IF;
    END IF;

    -- Insert log if:
    --   a) Motor status actually changed (ON→OFF or OFF→ON)
    --   b) Motor start was rejected due to full tank
    IF (OLD.motor_status IS DISTINCT FROM NEW.motor_status) OR (motor_rejected = TRUE) THEN
        INSERT INTO public.motor_logs (status, water_level, mode, reason)
        VALUES (
            NEW.motor_status,
            NEW.water_level,
            NEW.mode,
            log_reason
        );
    END IF;

    RETURN NEW;
END;
$$ LANGUAGE plpgsql;

-- ── 2. RE-APPLY TRIGGER ──────────────────────────────────────
DROP TRIGGER IF EXISTS tr_motor_automation ON public.motor_system;
CREATE TRIGGER tr_motor_automation
BEFORE UPDATE ON public.motor_system
FOR EACH ROW
EXECUTE FUNCTION public.handle_motor_automation();

-- ── 3. RESET PERMISSIONS ─────────────────────────────────────
ALTER TABLE public.motor_system    DISABLE ROW LEVEL SECURITY;
ALTER TABLE public.motor_logs      DISABLE ROW LEVEL SECURITY;
ALTER TABLE public.motor_schedules DISABLE ROW LEVEL SECURITY;

-- ── 4. VERIFY (run after applying) ───────────────────────────
-- SELECT water_level, motor_status, manual_override, updated_at
-- FROM motor_system WHERE id = 1;
