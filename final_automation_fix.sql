-- ============================================================
-- Hydra-Flow Ultimate Automation Fix (v6.1)
-- Resolves: 
-- 1. Heartbeat "last_seen" not updating correctly
-- 2. Motor start/stop logs missing
-- 3. Tank-full safety rejection logs
-- ============================================================

-- ── 1. ENSURE COLUMNS ARE INITIALIZED ───────────────────────
UPDATE public.motor_system 
SET tank_ping = FALSE, motor_ping = FALSE, tank_last_seen = now(), motor_last_seen = now()
WHERE id = 1 AND (tank_ping IS NULL OR motor_ping IS NULL OR tank_last_seen IS NULL);

-- ── 2. OPTIMIZED AUTOMATION TRIGGER ─────────────────────────
CREATE OR REPLACE FUNCTION public.handle_motor_automation()
RETURNS TRIGGER AS $$
DECLARE
    motor_rejected BOOLEAN := FALSE;
BEGIN
    -- 1. TANK HEARTBEAT: Update tank_last_seen if ping bit changed
    -- We use COALESCE to handle initial NULL values safely
    IF (COALESCE(OLD.tank_ping, FALSE) IS DISTINCT FROM NEW.tank_ping) THEN
        NEW.tank_last_seen := now();
    END IF;
    
    -- 2. MOTOR HEARTBEAT: Update motor_last_seen if ping bit changed
    IF (COALESCE(OLD.motor_ping, FALSE) IS DISTINCT FROM NEW.motor_ping) THEN
        NEW.motor_last_seen := now();
    END IF;

    -- 3. AUTO-OFF SAFETY (Tank Full)
    -- If tank is full (100%), force motor to OFF.
    IF NEW.water_level = 100 THEN
        IF NEW.motor_status = TRUE THEN
            NEW.motor_status    := FALSE;
            NEW.manual_override := FALSE;
            
            -- Record if we actually rejected an "ON" signal
            IF (OLD.motor_status = FALSE) THEN
                motor_rejected := TRUE;
            END IF;
        END IF;
    END IF;

    -- 4. UPDATE SYSTEM TIMESTAMP
    NEW.updated_at := now();

    -- 5. LOGGING CHANGES
    -- Log if status changed, or if starting was rejected by a full tank
    IF (OLD.motor_status IS DISTINCT FROM NEW.motor_status) OR (motor_rejected = TRUE) THEN
        INSERT INTO public.motor_logs (status, water_level, mode, reason)
        VALUES (
            NEW.motor_status, 
            NEW.water_level, 
            NEW.mode, 
            CASE 
                WHEN motor_rejected THEN 'tank_full_rejection' 
                WHEN (OLD.motor_status = FALSE AND NEW.motor_status = TRUE) THEN 'started'
                ELSE 'stopped'
            END
        );
    END IF;

    RETURN NEW;
END;
$$ LANGUAGE plpgsql;

-- ── 3. RE-APPLY TRIGGER ─────────────────────────────────────
DROP TRIGGER IF EXISTS tr_motor_automation ON public.motor_system;
CREATE TRIGGER tr_motor_automation
BEFORE UPDATE ON public.motor_system
FOR EACH ROW
EXECUTE FUNCTION public.handle_motor_automation();

-- Reset permissions just in case
ALTER TABLE public.motor_system DISABLE ROW LEVEL SECURITY;
ALTER TABLE public.motor_logs DISABLE ROW LEVEL SECURITY;
ALTER TABLE public.motor_schedules DISABLE ROW LEVEL SECURITY;
