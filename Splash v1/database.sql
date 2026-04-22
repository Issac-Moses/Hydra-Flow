-- ============================================================
-- Water Motor Automation - ULTIMATE STARTUP SQL (v5.7)
-- Updated with "Ping Toggle" columns for accurate node heartbeat detection.
-- ============================================================

-- ── 1. CREATE TABLES (The foundation) ───────────────────────

CREATE TABLE IF NOT EXISTS public.motor_system (
    id               INTEGER PRIMARY KEY CHECK (id = 1),
    motor_status     BOOLEAN NOT NULL DEFAULT FALSE,
    water_level      INTEGER NOT NULL DEFAULT 0,        
    mode             TEXT NOT NULL DEFAULT 'OFFLINE',   
    manual_override  BOOLEAN NOT NULL DEFAULT FALSE,
    auto_off_timeout INTEGER NOT NULL DEFAULT 30,       
    motor_on_time    INTEGER NOT NULL DEFAULT 0,        
    tank_last_seen   TIMESTAMPTZ DEFAULT '2000-01-01',
    motor_last_seen  TIMESTAMPTZ DEFAULT '2000-01-01',
    tank_ping        BOOLEAN NOT NULL DEFAULT FALSE, -- Ping bit for Tank board
    motor_ping       BOOLEAN NOT NULL DEFAULT FALSE, -- Ping bit for Motor board
    updated_at       TIMESTAMPTZ DEFAULT NOW()
);

CREATE TABLE IF NOT EXISTS public.motor_schedules (
    id          BIGSERIAL PRIMARY KEY,
    on_time     TEXT NOT NULL,                      
    enabled     BOOLEAN NOT NULL DEFAULT TRUE,
    created_at  TIMESTAMPTZ DEFAULT now()
);

CREATE TABLE IF NOT EXISTS public.motor_logs (
    id          BIGSERIAL PRIMARY KEY,
    status      BOOLEAN NOT NULL,
    water_level INTEGER NOT NULL DEFAULT 0,
    mode        TEXT NOT NULL DEFAULT 'ONLINE',
    reason      TEXT DEFAULT 'manual',  
    timestamp   TIMESTAMPTZ DEFAULT now()
);

-- ── 2. INITIAL SEED ───────────────────────────────────────
INSERT INTO public.motor_system (id, motor_status, water_level, mode)
VALUES (1, FALSE, 0, 'OFFLINE')
ON CONFLICT (id) DO NOTHING;

-- ── 3. AUTOMATION LOGIC (Trigger & Independent Heartbeats) ──────────

DROP TRIGGER IF EXISTS tr_motor_automation ON public.motor_system;

CREATE OR REPLACE FUNCTION public.handle_motor_automation()
RETURNS TRIGGER AS $$
BEGIN
    -- Heartbeat Detection (TANK): ONLY update tank_last_seen if tank_ping changed
    IF (OLD.tank_ping IS DISTINCT FROM NEW.tank_ping) THEN
        NEW.tank_last_seen := now();
    END IF;
    
    -- Heartbeat Detection (MOTOR): ONLY update motor_last_seen if motor_ping changed
    IF (OLD.motor_ping IS DISTINCT FROM NEW.motor_ping) THEN
        NEW.motor_last_seen := now();
    END IF;

    -- Auto-OFF Safety (Tank Full)
    IF NEW.water_level = 100 THEN
        -- Only trigger Auto-OFF if the motor was actually on
        IF NEW.motor_status = TRUE THEN
            NEW.motor_status    := FALSE;
            NEW.manual_override := FALSE;
        END IF;
    END IF;

    -- Update Timestamp
    NEW.updated_at := now();

    -- Logging Changes
    IF (OLD.motor_status IS DISTINCT FROM NEW.motor_status) THEN
        INSERT INTO public.motor_logs (status, water_level, mode)
        VALUES (NEW.motor_status, NEW.water_level, NEW.mode);
    END IF;

    RETURN NEW;
END;
$$ LANGUAGE plpgsql;

CREATE TRIGGER tr_motor_automation
BEFORE UPDATE ON public.motor_system
FOR EACH ROW
EXECUTE FUNCTION public.handle_motor_automation();

-- ── 4. PERMISSIONS ──────────────────────────────────────────

ALTER TABLE public.motor_system DISABLE ROW LEVEL SECURITY;
ALTER TABLE public.motor_logs   DISABLE ROW LEVEL SECURITY;
ALTER TABLE public.motor_schedules DISABLE ROW LEVEL SECURITY;

-- ── 5. REALTIME (Safe Mode) ────────────────────────────────

DO $$
BEGIN
    -- Ensure tables are added to the realtime publication
    BEGIN
        ALTER PUBLICATION supabase_realtime ADD TABLE public.motor_system;
    EXCEPTION WHEN OTHERS THEN NULL;
    END;

    BEGIN
        ALTER PUBLICATION supabase_realtime ADD TABLE public.motor_logs;
    EXCEPTION WHEN OTHERS THEN NULL;
    END;

    BEGIN
        ALTER PUBLICATION supabase_realtime ADD TABLE public.motor_schedules;
    EXCEPTION WHEN OTHERS THEN NULL;
    END;
END $$;
