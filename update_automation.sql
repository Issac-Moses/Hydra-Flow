-- ============================================================
-- Hydra-Flow Automation Update (Fix: Scheduled Shut-off & Logs)
-- This script improves the trigger logic to ensure that log events
-- are recorded even if the motor start is immediately rejected by a full tank.
-- ============================================================

CREATE OR REPLACE FUNCTION public.handle_motor_automation()
RETURNS TRIGGER AS $$
DECLARE
    motor_rejected BOOLEAN := FALSE;
BEGIN
    -- 1. Heartbeat Detection (TANK)
    IF (OLD.tank_ping IS DISTINCT FROM NEW.tank_ping) THEN
        NEW.tank_last_seen := now();
    END IF;
    
    -- 2. Heartbeat Detection (MOTOR)
    IF (OLD.motor_ping IS DISTINCT FROM NEW.motor_ping) THEN
        NEW.motor_last_seen := now();
    END IF;

    -- 3. Auto-OFF Safety (Tank Full)
    -- If tank is full (100%), force motor to OFF.
    IF NEW.water_level = 100 THEN
        IF NEW.motor_status = TRUE THEN
            NEW.motor_status    := FALSE;
            NEW.manual_override := FALSE;
            
            -- If it was ALREADY false in the database, but someone tried to turn it ON,
            -- we mark it as rejected so we can still record a "STOPPED" log event.
            IF OLD.motor_status = FALSE THEN
                motor_rejected := TRUE;
            END IF;
        END IF;
    END IF;

    -- 4. Update Timestamp
    NEW.updated_at := now();

    -- 5. Logging Changes
    -- Log if:
    --   a) Status changed from TRUE to FALSE (or vice versa)
    --   b) Someone tried to turn it ON while full (motor_rejected)
    IF (OLD.motor_status IS DISTINCT FROM NEW.motor_status) OR (motor_rejected = TRUE) THEN
        INSERT INTO public.motor_logs (status, water_level, mode, reason)
        VALUES (
            NEW.motor_status, 
            NEW.water_level, 
            NEW.mode, 
            CASE WHEN motor_rejected THEN 'tank_full_rejection' ELSE 'system' END
        );
    END IF;

    RETURN NEW;
END;
$$ LANGUAGE plpgsql;

-- Replace trigger (if for some reason it was deleted, but keep same name)
DROP TRIGGER IF EXISTS tr_motor_automation ON public.motor_system;
CREATE TRIGGER tr_motor_automation
BEFORE UPDATE ON public.motor_system
FOR EACH ROW
EXECUTE FUNCTION public.handle_motor_automation();

-- Note: No changes to tables needed, just the logic.
